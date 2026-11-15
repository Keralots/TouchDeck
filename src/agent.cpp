#include "agent.h"
#include "agent_audio.h"
#include "board.h"
#include "settings.h"
#if defined(BOARD_MIC_ES7210)
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <atomic>
#include <esp_random.h>
#include <opus.h>

namespace {

// Device VAD: a frame is voice when it stands this far over the tracked floor.
constexpr float VOICE_OVER_FLOOR_DB = 9.0f;
constexpr float VOICE_MIN_DBFS = -60.0f;
constexpr int SPEECH_START_FRAMES = 3;    // 180 ms of voice opens a turn
constexpr uint32_t MIC_TEST_MS = 20000;
constexpr uint32_t MAX_TURN_MS = 20000;
constexpr uint32_t CONNECT_MS = 10000;    // socket + server hello
constexpr uint32_t STT_WAIT_MS = 4000;    // no transcript = server dropped the turn as noise
constexpr uint32_t REPLY_WAIT_MS = 30000;
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

void startListening() {
  s_voiced = s_quiet = 0;
  s_inSpeech = false;
  s_turnFrames = 0;
  int16_t drop[AGENT_FRAME];
  float db;
  while (agentAudioPopFrame(drop, db)) {}  // stale frames from before the reply
  sendListen("start");
  s_micOpen = false;
  enter(AGENT_LISTENING);
}

void endSession(const char* why) {
  agentAudioCapture(false);
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
      s_ttsDone = false;
      agentAudioCapture(false);
      enter(AGENT_SPEAKING);
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
    case WStype_TEXT: onText((const char*)payload, length); break;
    case WStype_ERROR: Serial.printf("Agent: socket error %.*s\n", (int)length, (const char*)payload); break;
    case WStype_BIN: onBinary(payload, length); break;
    default: break;
  }
}

bool beginSession() {
  setText(s_user, "");
  setText(s_reply, "");
  s_error[0] = 0;
  s_sessionId = "";
  s_turns = s_upFrames = s_downFrames = 0;
  String host, path;
  uint16_t port;
  if (!parseUrl(host, port, path)) { setError("server address must look like ws://host:port/path"); return false; }
  if (WiFi.status() != WL_CONNECTED) { setError("no WiFi"); return false; }
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
    const int n = opus_encode(s_enc, frame, AGENT_FRAME, pkt, sizeof(pkt));
    if (n > 0 && s_ws.sendBIN(pkt, n)) s_upFrames++;
    s_turnFrames++;
    if (s_inSpeech && (s_quiet * 60 >= agentSettings.endPauseMs || s_turnFrames * 60 >= MAX_TURN_MS)) {
      ev("turn end, %u frames", (unsigned)s_turnFrames);
      agentAudioCapture(false);
      sendListen("stop");
      s_turns++;
      s_gotStt = false;
      enter(AGENT_THINKING);
    }
  }
}

void tick() {
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
  } else if (st == AGENT_THINKING && !s_gotStt && now - s_since > STT_WAIT_MS) {
    startListening();  // the server judged the turn noise and stays silent
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
