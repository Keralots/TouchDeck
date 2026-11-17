#include "agent.h"
#include "agent_audio.h"
#include "board.h"
#include "settings.h"
#if defined(BOARD_MIC_ES7210)
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <atomic>
#include <esp_random.h>
#include <opus.h>
#include "deck_ha.h"

namespace {

// Device VAD: a frame is voice when it stands this far over the tracked floor.
constexpr float VOICE_OVER_FLOOR_DB = 9.0f;
constexpr float VOICE_MIN_DBFS = -60.0f;
constexpr int SPEECH_START_FRAMES = 3;    // 180 ms of voice opens a turn
constexpr uint32_t MIC_TEST_MS = 20000;
constexpr uint32_t MAX_TURN_MS = 20000;
constexpr uint32_t CONNECT_MS = 10000;    // socket + server hello
constexpr uint32_t STT_WAIT_MS = 4000;    // no transcript = server dropped the turn as noise
constexpr uint32_t STT_WAIT_HA_MS = 15000;  // HA always answers with stt-end or an error
constexpr uint32_t REPLY_WAIT_MS = 60000;  // the LLM has taken 29 s to its first token
constexpr uint32_t DRAIN_MS = 300;        // after the last speaker sample, before the mic reopens
constexpr int OPUS_COMPLEXITY = 2;        // 9 ms per frame at 3 on the P4
constexpr int OPUS_BITRATE = 20000;
constexpr size_t TEXT_MAX = 240;

std::atomic<AgentState> s_state{AGENT_IDLE};
std::atomic<bool> s_startReq{false}, s_stopReq{false}, s_testReq{false};
uint8_t s_testPga = 12, s_testBoost = 6;
TaskHandle_t s_task = nullptr;
WebSocketsClient s_ws;
OpusEncoder* s_enc = nullptr;
OpusDecoder* s_dec = nullptr;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char s_user[TEXT_MAX] = "", s_reply[TEXT_MAX] = "", s_error[80] = "";
String s_sessionId;

// Session timing, owned by the task.
uint32_t s_since = 0;       // entered the current state
bool s_connected = false, s_ttsDone = false, s_gotStt = false;
int s_voiced = 0, s_quiet = 0;
bool s_inSpeech = false;
bool s_micOpen = false;  // capture starts once the chime or reply has finished playing
uint32_t s_turnFrames = 0, s_upFrames = 0, s_downFrames = 0, s_turns = 0;

// Home Assistant backend: Assist pipeline runs stt -> intent on the HA WebSocket,
// the reply is fetched as 16 kHz WAV through /api/tts_get_url (the run's own TTS is MP3 only).
bool s_ha = false;  // backend of the running session
String s_haHost, s_haToken;
uint16_t s_haPort = 0;
uint32_t s_haMsgId = 0, s_pipeGetId = 0, s_runId = 0;
int s_hid = -1;          // stt_binary_handler_id of the open run
bool s_sttOpen = false;  // the run still takes audio
String s_convId, s_ttsEngine, s_ttsLang, s_ttsVoice;
String s_haFail, s_ttsText;  // set in the socket callback, acted on in tick()
bool s_noSpeech = false;
HTTPClient s_http;
WiFiClient* s_ttsIn = nullptr;
uint8_t s_wavHdr[256];
size_t s_wavHdrN = 0;
bool s_wavData = false;
int s_carry = -1;  // odd byte left over between reads
uint32_t s_ttsLastRx = 0;

// Session event log for /api/agent: what happened when, relative to the tap.
constexpr int EV_MAX = 48;
struct Ev { uint32_t ms; char msg[44]; };
Ev s_ev[EV_MAX];
int s_evN = 0;
uint32_t s_evT0 = 0;

void ev(const char* fmt, ...) {
  char m[44];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof(m), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&s_mux);
  Ev& e = s_ev[s_evN++ % EV_MAX];
  e.ms = millis() - s_evT0;
  memcpy(e.msg, m, sizeof(m));
  portEXIT_CRITICAL(&s_mux);
}

const char* const STATE_NAMES[] = {"idle", "connecting", "listening", "thinking", "speaking", "error", "mic test"};

void setText(char* dst, const char* src) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(dst, src, TEXT_MAX);
  portEXIT_CRITICAL(&s_mux);
}

void setError(const char* why) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_error, why, sizeof(s_error));
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("Agent: %s\n", why);
}

void enter(AgentState st) {
  if (st != s_state) ev("%s", STATE_NAMES[st]);
  s_state = st;
  s_since = millis();
}

void ensureClientId() {
  if (agentSettings.clientId[0]) return;
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(agentSettings.clientId, sizeof(agentSettings.clientId),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5],
           b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  saveSettings();
}

bool parseUrl(String& host, uint16_t& port, String& path) {
  String u = agentSettings.url;
  u.trim();
  if (!u.startsWith("ws://")) return false;
  u = u.substring(5);
  const int slash = u.indexOf('/');
  path = slash >= 0 ? u.substring(slash) : String("/");
  String hp = slash >= 0 ? u.substring(0, slash) : u;
  const int colon = hp.indexOf(':');
  port = colon >= 0 ? (uint16_t)hp.substring(colon + 1).toInt() : 80;
  host = colon >= 0 ? hp.substring(0, colon) : hp;
  return host.length() && port;
}

void sendJson(JsonDocument& d) {
  if (s_sessionId.length()) d["session_id"] = s_sessionId;
  String out;
  serializeJson(d, out);
  s_ws.sendTXT(out);
}

void sendListen(const char* state) {
  JsonDocument d;
  d["type"] = "listen";
  d["state"] = state;
  d["mode"] = "auto";
  sendJson(d);
}

// Two short notes, rising = "speak now", falling = "conversation over".
void chime(bool rising) {
  static int16_t buf[AGENT_RATE * 180 / 1000];
  const size_t half = sizeof(buf) / sizeof(buf[0]) / 2;
  for (size_t i = 0; i < 2 * half; i++) {
    const bool second = i >= half;
    const float hz = (second == rising) ? 1320.0f : 880.0f;
    const size_t k = second ? i - half : i;
    const float env = k < 160 ? k / 160.0f : (half - k < 400 ? (half - k) / 400.0f : 1.0f);
    buf[i] = (int16_t)(6000.0f * env * sinf(2.0f * (float)M_PI * hz * k / AGENT_RATE));
  }
  agentAudioPush(buf, 2 * half);
}

void startListening();
void endTurn();

// --- Home Assistant backend ---

void haSend(JsonDocument& d) {
  String out;
  serializeJson(d, out);
  s_ws.sendTXT(out);
}

void haRunStart() {
  JsonDocument d;
  d["id"] = s_runId = ++s_haMsgId;
  d["type"] = "assist_pipeline/run";
  d["start_stage"] = "stt";
  d["end_stage"] = "intent";
  d["input"]["sample_rate"] = AGENT_RATE;
  if (agentSettings.pipeline[0]) d["pipeline"] = agentSettings.pipeline;
  if (s_convId.length()) d["conversation_id"] = s_convId;
  s_hid = -1;
  s_sttOpen = true;
  haSend(d);
}

// A lone handler byte ends the run's audio.
void haEndAudio() {
  if (s_sttOpen && s_hid >= 0) {
    const uint8_t b = (uint8_t)s_hid;
    s_ws.sendBIN(&b, 1);
  }
  s_sttOpen = false;
}

void ttsClose() {
  if (s_ttsIn) s_http.end();
  s_ttsIn = nullptr;
}

bool ttsOpen(const String& text, String& err) {
  const String base = "http://" + s_haHost + ":" + String(s_haPort);
  String path;
  {
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(base + "/api/tts_get_url")) {
      err = "TTS: cannot connect";
      return false;
    }
    http.addHeader("Authorization", "Bearer " + s_haToken);
    http.addHeader("Content-Type", "application/json");
    JsonDocument req;
    req["engine_id"] = s_ttsEngine;
    req["message"] = text;
    if (s_ttsLang.length()) req["language"] = s_ttsLang;
    JsonObject o = req["options"].to<JsonObject>();
    if (s_ttsVoice.length()) o["voice"] = s_ttsVoice;
    o["preferred_format"] = "wav";
    o["preferred_sample_rate"] = AGENT_RATE;
    o["preferred_sample_channels"] = 1;
    o["preferred_sample_bytes"] = 2;
    String body;
    serializeJson(req, body);
    const int code = http.POST(body);
    if (code == 200) {
      JsonDocument r;
      // "path", not "url": the url may carry HA's external address.
      if (!deserializeJson(r, http.getString())) path = (const char*)(r["path"] | "");
    }
    http.end();
    if (!path.length()) {
      err = "TTS URL failed, HTTP " + String(code);
      return false;
    }
  }
  s_http.useHTTP10(true);  // no chunked framing: the body is the WAV as is
  s_http.setTimeout(10000);
  if (!s_http.begin(base + path)) {
    err = "TTS: cannot connect";
    return false;
  }
  const int code = s_http.GET();
  if (code != 200) {
    s_http.end();
    err = "TTS audio failed, HTTP " + String(code);
    return false;
  }
  s_ttsIn = s_http.getStreamPtr();
  s_ttsLastRx = millis();
  s_wavHdrN = 0;
  s_wavData = false;
  s_carry = -1;
  return true;
}

// PCM start in s_wavHdr; 0 = need more bytes, -1 = not 16 kHz mono 16-bit PCM.
// Streamed WAV carries 0xFFFFFFFF sizes, so only the chunk walk is trusted.
int wavDataOffset() {
  if (s_wavHdrN < 12) return 0;
  if (memcmp(s_wavHdr, "RIFF", 4) || memcmp(s_wavHdr + 8, "WAVE", 4)) return -1;
  size_t i = 12;
  bool fmtOk = false;
  while (i + 8 <= s_wavHdrN) {
    uint32_t sz;
    memcpy(&sz, s_wavHdr + i + 4, 4);
    if (!memcmp(s_wavHdr + i, "data", 4)) return fmtOk ? (int)(i + 8) : -1;
    if (sz > sizeof(s_wavHdr) || i + 8 + sz > sizeof(s_wavHdr)) return -1;
    if (i + 8 + sz > s_wavHdrN) return 0;
    if (!memcmp(s_wavHdr + i, "fmt ", 4) && sz >= 16) {
      uint16_t fmt, ch, bits;
      uint32_t rate;
      memcpy(&fmt, s_wavHdr + i + 8, 2);
      memcpy(&ch, s_wavHdr + i + 10, 2);
      memcpy(&rate, s_wavHdr + i + 12, 4);
      memcpy(&bits, s_wavHdr + i + 22, 2);
      fmtOk = fmt == 1 && ch == 1 && rate == AGENT_RATE && bits == 16;
    }
    i += 8 + sz + (sz & 1);
  }
  return s_wavHdrN >= sizeof(s_wavHdr) ? -1 : 0;
}

void pushPcm(const uint8_t* p, size_t n) {
  static int16_t pcm[256];
  size_t k = 0;
  auto flush = [&] {
    if (!k) return;
    if (s_state == AGENT_THINKING) enter(AGENT_SPEAKING);
    agentAudioPush(pcm, k);
    k = 0;
  };
  if (s_carry >= 0 && n) {
    pcm[k++] = (int16_t)(s_carry | (p[0] << 8));
    s_carry = -1;
    p++;
    n--;
  }
  for (; n >= 2; p += 2, n -= 2) {
    pcm[k++] = (int16_t)(p[0] | (p[1] << 8));
    if (k == sizeof(pcm) / sizeof(pcm[0])) flush();
  }
  if (n) s_carry = p[0];
  flush();
}

// Feeds the speaker ring from the TTS stream as far as it has room.
void pumpTts() {
  static uint8_t buf[1024];
  for (int round = 0; s_ttsIn && round < 8; round++) {
    const int avail = s_ttsIn->available();
    if (avail <= 0) {
      // A kept-alive socket never reports the end: 3 s of nothing counts as done.
      if (!s_http.connected() || millis() - s_ttsLastRx > 3000) {
        ttsClose();
        s_ttsDone = true;
        ev("tts end");
      }
      return;
    }
    s_ttsLastRx = millis();
    size_t want = min<size_t>(avail, sizeof(buf));
    if (s_wavData) want = min<size_t>(want, agentAudioRoom() * 2);
    if (!want) return;
    const int got = s_ttsIn->read(buf, want);
    if (got <= 0) return;
    s_downFrames++;
    const uint8_t* p = buf;
    size_t n = got;
    if (!s_wavData) {
      const size_t take = min(n, sizeof(s_wavHdr) - s_wavHdrN);
      memcpy(s_wavHdr + s_wavHdrN, p, take);
      s_wavHdrN += take;
      p += take;
      n -= take;
      const int off = wavDataOffset();
      if (off < 0) {
        ttsClose();
        ev("tts is not 16 kHz mono WAV");
        s_ttsDone = true;
        return;
      }
      if (!off) continue;
      s_wavData = true;
      pushPcm(s_wavHdr + off, s_wavHdrN - off);
    }
    pushPcm(p, n);
  }
}

void haOnText(const char* payload, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char* type = d["type"] | "";
  if (!strcmp(type, "auth_required")) {
    JsonDocument a;
    a["type"] = "auth";
    a["access_token"] = s_haToken;
    haSend(a);
  } else if (!strcmp(type, "auth_invalid")) {
    s_haFail = "Home Assistant refused the token";
  } else if (!strcmp(type, "auth_ok")) {
    JsonDocument g;
    g["id"] = s_pipeGetId = ++s_haMsgId;
    g["type"] = "assist_pipeline/pipeline/get";
    if (agentSettings.pipeline[0]) g["pipeline_id"] = agentSettings.pipeline;
    haSend(g);
  } else if (!strcmp(type, "result")) {
    const uint32_t id = d["id"] | 0u;
    if (id != s_pipeGetId && id != s_runId) return;
    if (!(d["success"] | false)) {
      s_haFail = String("Home Assistant: ") + (const char*)(d["error"]["message"] | "request failed");
      return;
    }
    if (id == s_pipeGetId) {
      JsonObjectConst r = d["result"];
      s_ttsEngine = (const char*)(r["tts_engine"] | "");
      s_ttsLang = (const char*)(r["tts_language"] | "");
      s_ttsVoice = (const char*)(r["tts_voice"] | "");
      ev("pipeline %.34s", (const char*)(r["name"] | ""));
      if (!s_ttsEngine.length()) ev("pipeline has no TTS");
      if (s_state == AGENT_CONNECTING) {
        if (agentSettings.chimes) chime(true);
        startListening();
      }
    }
  } else if (!strcmp(type, "event") && (d["id"] | 0u) == s_runId) {
    JsonObjectConst e = d["event"];
    const char* et = e["type"] | "";
    JsonObjectConst data = e["data"];
    if (!strcmp(et, "run-start")) {
      s_hid = data["runner_data"]["stt_binary_handler_id"] | -1;
      if (data["conversation_id"].is<const char*>()) s_convId = (const char*)data["conversation_id"];
    } else if (!strcmp(et, "stt-vad-start")) {
      // HA heard speech the deck's VAD missed: do not end the chat as idle.
      if (s_state == AGENT_LISTENING && !s_inSpeech) {
        s_inSpeech = true;
        ev("ha heard speech");
      }
    } else if (!strcmp(et, "stt-vad-end")) {
      if (s_state == AGENT_LISTENING) {
        ev("ha vad end");
        endTurn();
      }
    } else if (!strcmp(et, "stt-end")) {
      String t = (const char*)(data["stt_output"]["text"] | "");
      t.trim();
      s_gotStt = true;
      setText(s_user, t.c_str());
      ev("stt %.36s", t.c_str());
    } else if (!strcmp(et, "intent-end")) {
      JsonObjectConst out = data["intent_output"];
      if (out["conversation_id"].is<const char*>()) s_convId = (const char*)out["conversation_id"];
      const char* speech = out["response"]["speech"]["plain"]["speech"] | "";
      setText(s_reply, speech);
      ev("reply %.34s", speech);
      if (*speech && s_ttsEngine.length()) s_ttsText = speech;
      else s_ttsDone = true;
    } else if (!strcmp(et, "error")) {
      const char* code = data["code"] | "";
      ev("error %.36s", code);
      if (!strcmp(code, "stt-no-text-recognized")) s_noSpeech = true;
      else s_haFail = String("Home Assistant: ") + (const char*)(data["message"] | code);
    }
  }
}

// --- Session ---

void startListening() {
  s_voiced = s_quiet = 0;
  s_inSpeech = false;
  s_turnFrames = 0;
  int16_t drop[AGENT_FRAME];
  float db;
  while (agentAudioPopFrame(drop, db)) {}  // stale frames from before the reply
  if (s_ha) haRunStart();
  else sendListen("start");
  s_micOpen = false;
  enter(AGENT_LISTENING);
}

void endTurn() {
  ev("turn end, %u frames", (unsigned)s_turnFrames);
  agentAudioCapture(false);
  if (s_ha) haEndAudio();
  else sendListen("stop");
  s_turns++;
  s_gotStt = false;
  s_ttsDone = false;  // left true by the previous reply
  enter(AGENT_THINKING);
}

void endSession(const char* why) {
  agentAudioCapture(false);
  ttsClose();
  s_ttsText = "";
  s_haFail = "";
  s_noSpeech = false;
  if (!why && agentSettings.chimes) chime(false);
  // A goodbye arrives just before the server closes: let it finish playing.
  for (uint32_t t0 = millis(); !why && agentAudioQueued() && millis() - t0 < 3000;) vTaskDelay(pdMS_TO_TICKS(20));
  if (s_connected) s_ws.disconnect();
  s_connected = false;
  agentAudioStop();
  if (why) {
    setError(why);
    enter(AGENT_ERROR);
  } else {
    enter(AGENT_IDLE);
  }
  Serial.printf("Agent: session over (%u turns, %u up, %u down)\n", (unsigned)s_turns, (unsigned)s_upFrames,
                (unsigned)s_downFrames);
}

void onText(const char* payload, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char* type = d["type"] | "";
  if (!strcmp(type, "hello")) {
    s_sessionId = (const char*)(d["session_id"] | "");
    if (s_state == AGENT_CONNECTING) {
      if (agentSettings.chimes) chime(true);
      startListening();
    }
  } else if (!strcmp(type, "stt")) {
    s_gotStt = true;
    setText(s_user, d["text"] | "");
    ev("stt %.36s", (const char*)(d["text"] | ""));
  } else if (!strcmp(type, "tts")) {
    const char* st = d["state"] | "";
    if (!strcmp(st, "start")) {
      // Stays "thinking" until audio arrives: the server sends this before the LLM answers.
      s_ttsDone = false;
      agentAudioCapture(false);
    } else if (!strcmp(st, "sentence_start")) {
      setText(s_reply, d["text"] | "");
    } else if (!strcmp(st, "stop")) {
      s_ttsDone = true;
      ev("tts stop");
    }
  } else if (!strcmp(type, "alert")) {
    setText(s_reply, d["message"] | "");
  }
}

void onBinary(const uint8_t* p, size_t len) {
  static int16_t pcm[AGENT_RATE * 120 / 1000];
  const int n = opus_decode(s_dec, p, (opus_int32)len, pcm, sizeof(pcm) / sizeof(pcm[0]), 0);
  if (n <= 0) return;
  s_downFrames++;
  if (s_state == AGENT_THINKING) enter(AGENT_SPEAKING);
  size_t off = 0;
  const uint32_t t0 = millis();
  // The server paces in real time; a full ring only waits briefly.
  while (off < (size_t)n && millis() - t0 < 500) {
    off += agentAudioPush(pcm + off, n - off);
    if (off < (size_t)n) vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      s_connected = true;
      if (s_ha) break;  // HA opens with auth_required
      JsonDocument d;
      d["type"] = "hello";
      d["version"] = 1;
      d["transport"] = "websocket";
      d["features"]["mcp"] = false;
      JsonObject a = d["audio_params"].to<JsonObject>();
      a["format"] = "opus";
      a["sample_rate"] = AGENT_RATE;
      a["channels"] = 1;
      a["frame_duration"] = 60;
      String out;
      serializeJson(d, out);
      s_ws.sendTXT(out);
      break;
    }
    case WStype_DISCONNECTED:
      if (s_connected || s_state == AGENT_CONNECTING) {
        const bool early = s_state == AGENT_CONNECTING;
        s_connected = false;
        if (s_state != AGENT_IDLE && s_state != AGENT_ERROR) endSession(early ? "server refused the connection" : nullptr);
      }
      break;
    case WStype_TEXT:
      if (s_ha) haOnText((const char*)payload, length);
      else onText((const char*)payload, length);
      break;
    case WStype_ERROR: Serial.printf("Agent: socket error %.*s\n", (int)length, (const char*)payload); break;
    case WStype_BIN:
      if (!s_ha) onBinary(payload, length);
      break;
    default: break;
  }
}

bool beginSession() {
  setText(s_user, "");
  setText(s_reply, "");
  s_error[0] = 0;
  s_sessionId = "";
  s_turns = s_upFrames = s_downFrames = 0;
  s_ha = agentSettings.backend == AGENT_BACKEND_HA;
  if (WiFi.status() != WL_CONNECTED) { setError("no WiFi"); return false; }
  if (s_ha) {
    String err;
    if (!deck::haEndpoint(s_haHost, s_haPort, s_haToken, err)) { setError(err.c_str()); return false; }
    s_convId = s_ttsEngine = s_ttsLang = s_ttsVoice = "";
    s_pipeGetId = s_runId = 0;
    s_hid = -1;
    s_sttOpen = false;
    if (!agentAudioStart(agentSettings.pga, agentSettings.boostDb, agentSettings.volume)) { setError("audio is busy"); return false; }
    s_ws.setExtraHeaders("");
    s_ws.onEvent(onWsEvent);
    s_ws.setReconnectInterval(1000);
    s_connected = false;
    s_ws.begin(s_haHost.c_str(), s_haPort, "/api/websocket", "");
    enter(AGENT_CONNECTING);
    return true;
  }
  String host, path;
  uint16_t port;
  if (!parseUrl(host, port, path)) { setError("server address must look like ws://host:port/path"); return false; }
  int err = 0;
  if (!s_enc) {
    s_enc = opus_encoder_create(AGENT_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (s_enc) {
      opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
      opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    }
  }
  if (!s_dec) s_dec = opus_decoder_create(AGENT_RATE, 1, &err);
  if (!s_enc || !s_dec) { setError("Opus init failed"); return false; }
  opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
  opus_decoder_ctl(s_dec, OPUS_RESET_STATE);
  if (!agentAudioStart(agentSettings.pga, agentSettings.boostDb, agentSettings.volume)) { setError("audio is busy"); return false; }
  ensureClientId();
  String hdr = "Protocol-Version: 1\r\nDevice-Id: " + WiFi.macAddress() + "\r\nClient-Id: " + agentSettings.clientId;
  if (agentSettings.token[0]) hdr = "Authorization: Bearer " + String(agentSettings.token) + "\r\n" + hdr;
  s_ws.setExtraHeaders(hdr.c_str());
  s_ws.onEvent(onWsEvent);
  // The library dials only once this long has passed since its last failure (0 at boot).
  s_ws.setReconnectInterval(1000);
  s_connected = false;
  s_ws.begin(host.c_str(), port, path.c_str(), "");
  enter(AGENT_CONNECTING);
  return true;
}

// Mic frames: device VAD and the uplink.
void pumpMic() {
  static int16_t frame[AGENT_FRAME];
  static uint8_t pkt[400];
  float db;
  while (agentAudioPopFrame(frame, db)) {
    if (s_state != AGENT_LISTENING) continue;
    const bool voice = agentAudioFloorReady() && db > agentAudioFloor() + VOICE_OVER_FLOOR_DB && db > VOICE_MIN_DBFS;
    if (voice) {
      s_voiced++;
      s_quiet = 0;
      if (s_voiced >= SPEECH_START_FRAMES && !s_inSpeech) {
        s_inSpeech = true;
        ev("speech %.1f dB, floor %.1f", db, agentAudioFloor());
      }
    } else {
      s_quiet++;
      if (!s_inSpeech) s_voiced = 0;
    }
    if (s_ha) {
      // Raw 16-bit PCM behind the run's handler byte; frames before run-start are dropped.
      static uint8_t raw[1 + AGENT_FRAME * 2];
      if (s_sttOpen && s_hid >= 0) {
        raw[0] = (uint8_t)s_hid;
        memcpy(raw + 1, frame, AGENT_FRAME * 2);
        if (s_ws.sendBIN(raw, sizeof(raw))) s_upFrames++;
      }
    } else {
      const int n = opus_encode(s_enc, frame, AGENT_FRAME, pkt, sizeof(pkt));
      if (n > 0 && s_ws.sendBIN(pkt, n)) s_upFrames++;
    }
    s_turnFrames++;
    if (s_inSpeech && (s_quiet * 60 >= agentSettings.endPauseMs || s_turnFrames * 60 >= MAX_TURN_MS)) endTurn();
  }
}

void tick() {
  if (s_ha) {
    if (s_haFail.length()) {
      const String why = s_haFail;
      endSession(why.c_str());
      return;
    }
    if (s_noSpeech) {
      s_noSpeech = false;
      if (s_state == AGENT_THINKING || s_state == AGENT_LISTENING) startListening();
    }
    if (s_ttsText.length()) {
      const String text = s_ttsText;
      s_ttsText = "";
      String err;
      if (!ttsOpen(text, err)) {
        ev("%.42s", err.c_str());
        s_ttsDone = true;
      }
    }
    pumpTts();
  }
  const uint32_t now = millis();
  const AgentState st = s_state;
  if (st == AGENT_CONNECTING && now - s_since > CONNECT_MS) {
    endSession(s_connected ? "no hello from the server" : "server not reachable");
  } else if (st == AGENT_LISTENING && !s_micOpen) {
    if (!agentAudioQueued() && now - agentAudioLastPlayMs() > DRAIN_MS) {
      s_micOpen = true;
      agentAudioCapture(true);
      ev("mic open");
      s_since = now;
    }
  } else if (st == AGENT_LISTENING && !s_inSpeech && now - s_since > agentSettings.idleSec * 1000u) {
    endSession(nullptr);
  } else if (st == AGENT_THINKING && !s_gotStt && now - s_since > (s_ha ? STT_WAIT_HA_MS : STT_WAIT_MS)) {
    startListening();  // the server judged the turn noise and stays silent
  } else if (st == AGENT_THINKING && s_ttsDone) {
    startListening();  // a reply with no audio
  } else if (st == AGENT_THINKING && now - s_since > REPLY_WAIT_MS) {
    endSession("no reply from the server");
  } else if (st == AGENT_SPEAKING && s_ttsDone && !agentAudioQueued() &&
             now - agentAudioLastPlayMs() > DRAIN_MS) {
    startListening();
  }
}

void agentTask(void*) {
  for (;;) {
    const AgentState st = s_state;
    const bool active = st != AGENT_IDLE && st != AGENT_ERROR;
    if (!active) {
      if (agentAudioRunning()) {
        vTaskDelay(pdMS_TO_TICKS(20));  // the engine is still closing
        continue;
      }
      s_stopReq = false;
      if (s_testReq.exchange(false)) {
        if (agentAudioStart(s_testPga, s_testBoost, 0)) enter(AGENT_MICTEST);
        continue;
      }
      if (!s_startReq.exchange(false)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      s_evN = 0;
      s_evT0 = millis();
      beginSession();
      continue;
    }
    if (st == AGENT_MICTEST) {
      if (s_stopReq.exchange(false) || millis() - s_since > MIC_TEST_MS) {
        agentAudioStop();
        enter(AGENT_IDLE);
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (s_stopReq.exchange(false)) {
      s_startReq = false;
      // A tap mid-reply cuts it: drop the queue before the closing chime goes in.
      agentAudioFlush();
      for (int i = 0; i < 10 && agentAudioQueued(); i++) vTaskDelay(pdMS_TO_TICKS(10));
      endSession(nullptr);
      continue;
    }
    s_ws.loop();
    pumpMic();
    tick();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void kick() {
  if (!s_task) xTaskCreatePinnedToCore(agentTask, "agent", 16384, nullptr, 3, &s_task, 1);
  if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

bool agentHasMic() { return true; }
bool agentAvailable() { return agentSettings.enabled; }

void agentStart() {
  if (!agentAvailable()) return;
  s_startReq = true;
  kick();
}

void agentStop() {
  s_stopReq = true;
  kick();
}

void agentToggle() {
  const AgentState st = s_state;
  if (st == AGENT_IDLE || st == AGENT_ERROR) agentStart();
  else agentStop();
}

AgentState agentState() { return s_state; }
float agentLevel() { return agentAudioMicLevel(); }

void agentTexts(String& user, String& reply) {
  char u[TEXT_MAX], r[TEXT_MAX];
  portENTER_CRITICAL(&s_mux);
  memcpy(u, s_user, TEXT_MAX);
  memcpy(r, s_reply, TEXT_MAX);
  portEXIT_CRITICAL(&s_mux);
  user = u;
  reply = r;
}

void agentStatusJson(JsonObject o) {
  o["mic"] = true;
  o["enabled"] = agentSettings.enabled;
  o["state"] = STATE_NAMES[s_state.load()];
  o["level"] = agentAudioMicLevel();
  o["floor"] = agentAudioFloor();
  o["turns"] = s_turns;
  o["up_frames"] = s_upFrames;
  o["down_frames"] = s_downFrames;
  o["queued"] = agentAudioQueued();
  String u, r;
  agentTexts(u, r);
  o["user"] = u;
  o["reply"] = r;
  char e[sizeof(s_error)];
  portENTER_CRITICAL(&s_mux);
  memcpy(e, s_error, sizeof(e));
  portEXIT_CRITICAL(&s_mux);
  o["error"] = e;
  o["task_stack_free"] = s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
  o["record_frames"] = agentAudioRecFrames();
  JsonArray evs = o["events"].to<JsonArray>();
  portENTER_CRITICAL(&s_mux);
  const int n = min(s_evN, EV_MAX);
  Ev copy[EV_MAX];
  for (int i = 0; i < n; i++) copy[i] = s_ev[(s_evN - n + i) % EV_MAX];
  portEXIT_CRITICAL(&s_mux);
  for (int i = 0; i < n; i++) evs.add(String(copy[i].ms) + " " + copy[i].msg);
}

void agentMicTest(uint8_t pga, uint8_t boostDb) {
  s_testPga = min<uint8_t>(pga, AGENT_PGA_MAX);
  s_testBoost = min<uint8_t>(boostDb, AGENT_BOOST_MAX);
  s_testReq = true;
  kick();
}

#else

bool agentHasMic() { return false; }
bool agentAvailable() { return false; }
void agentStart() {}
void agentStop() {}
void agentToggle() {}
AgentState agentState() { return AGENT_IDLE; }
float agentLevel() { return -96.0f; }
void agentTexts(String& user, String& reply) { user = reply = ""; }
void agentStatusJson(JsonObject o) { o["mic"] = false; }
void agentMicTest(uint8_t, uint8_t) {}

#endif  // BOARD_MIC_ES7210
