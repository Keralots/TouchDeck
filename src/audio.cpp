#include "audio.h"
#include "deck_sd.h"
#include "display_ui.h"
#include "settings.h"
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

namespace {

// Vendor pincfg.h (DEMO_MP3): NS4168 has no MCLK.
constexpr gpio_num_t PIN_BCLK = GPIO_NUM_42;
constexpr gpio_num_t PIN_WS = GPIO_NUM_2;
constexpr gpio_num_t PIN_DOUT = GPIO_NUM_41;

constexpr uint32_t SYNTH_RATE = 22050;
constexpr size_t CHUNK = 256;  // frames per i2s_channel_write
constexpr uint32_t FADE_FRAMES = 128;  // ramp when a sound is cut short
constexpr uint32_t IDLE_OFF_MS = 1500;
constexpr uint32_t AMP_SETTLE_MS = 30;
constexpr uint32_t MAX_CLIP_FRAMES = 1024 * 1024;  // 2 MB of mono PCM
constexpr uint32_t CACHE_MAX_BYTES = 256 * 1024;   // larger file clips load per play
constexpr int FILE_CACHE = 3;
// Playing outranks deck_build (15) on core 0, which otherwise starves the
// 46 ms DMA ring during photo frame transitions; loading and synthesis stay low.
constexpr UBaseType_t PRIO_WORK = 2, PRIO_PLAY = 17;

struct Clip {
  int16_t* pcm;  // mono, PSRAM
  uint32_t frames;
  uint32_t rate;
};

enum ReqType : uint8_t { REQ_SOUND, REQ_TONE, REQ_WARM };

struct Req {
  ReqType type;
  AudioSound sound;
  bool low;
  uint16_t gain;  // Q12, 4096 = unity
  uint16_t hz, ms;
  uint32_t reqMs;
  char file[AUDIO_FILE_LEN];
};

struct CachedFile {
  char name[AUDIO_FILE_LEN];
  Clip* clip;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Req s_req;
volatile bool s_has = false;
volatile bool s_stop = false;
volatile bool s_playing = false;
volatile uint32_t s_fileGen = 0;
TaskHandle_t s_task = nullptr;

// Owned by the audio task.
i2s_chan_handle_t s_tx = nullptr;
uint32_t s_rate = 0;
Clip* s_builtin[SND_COUNT] = {};
CachedFile s_files[FILE_CACHE] = {};
int s_fileNext = 0;
uint32_t s_seenGen = 0;

// Measurements for /api/audio.
volatile uint32_t s_plays = 0, s_latencyMs = 0, s_underruns = 0, s_writeErrors = 0;
volatile uint32_t s_opens = 0, s_channelCost = 0, s_renderMs = 0, s_loadMs = 0;
volatile int s_lastErr = 0;
char s_fileErr[64] = "";

void freeClip(Clip* c) {
  if (!c) return;
  heap_caps_free(c->pcm);
  delete c;
}

Clip* newClip(uint32_t frames, uint32_t rate) {
  int16_t* pcm = (int16_t*)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!pcm) return nullptr;
  return new Clip{pcm, frames, rate};
}

uint16_t gainFor(uint8_t vol) { return (uint16_t)((uint32_t)min<uint8_t>(vol, 100) * 4096 / 100); }

bool IRAM_ATTR onSendOvf(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  if (s_playing) s_underruns = s_underruns + 1;
  return false;
}

// ---- I2S channel ----

void closeChannel() {
  if (!s_tx) return;
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_tx);
  s_tx = nullptr;
  s_rate = 0;
}

bool openChannel(uint32_t rate) {
  esp_err_t err;
  if (s_tx && s_rate == rate) return true;
  if (s_tx) {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(s_tx);
    err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
      s_lastErr = err;
      closeChannel();
      return false;
    }
    s_rate = rate;
    return true;
  }
  const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 4;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;
  err = i2s_new_channel(&cc, &s_tx, nullptr);
  if (err != ESP_OK) {
    s_lastErr = err;
    s_tx = nullptr;
    return false;
  }
  // Mono on the S3 goes out on both slots, so the amp's L/R strap does not matter.
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = PIN_BCLK,
      .ws = PIN_WS,
      .dout = PIN_DOUT,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {false, false, false},
    },
  };
  i2s_event_callbacks_t cbs = {};
  cbs.on_send_q_ovf = onSendOvf;
  err = i2s_channel_init_std_mode(s_tx, &sc);
  if (err == ESP_OK) err = i2s_channel_register_event_callback(s_tx, &cbs, nullptr);
  if (err == ESP_OK) err = i2s_channel_enable(s_tx);
  if (err != ESP_OK) {
    s_lastErr = err;
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }
  s_rate = rate;
  s_opens = s_opens + 1;
  s_channelCost = before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // The amp wakes when BCLK returns; let it settle on silence.
  vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
  return true;
}

// ---- synthesis ----

struct Partial {
  float ratio, amp, decay;  // decay scales the note's time constant
};
struct Note {
  float at, hz, tau;
};

// Decaying partials with a short attack; recursive oscillator and envelope.
void addNote(float* acc, uint32_t frames, const Note& n, const Partial* p, int np) {
  const uint32_t start = (uint32_t)(n.at * SYNTH_RATE);
  const uint32_t attack = SYNTH_RATE * 4 / 1000;
  for (int k = 0; k < np; k++) {
    const float f = n.hz * p[k].ratio;
    if (f > SYNTH_RATE * 0.45f) continue;
    const float w = 2.0f * (float)M_PI * f / SYNTH_RATE;
    const float cw = cosf(w), sw = sinf(w);
    const float fall = expf(-1.0f / (n.tau * p[k].decay * SYNTH_RATE));
    float s = 0.0f, c = 1.0f, env = p[k].amp;
    for (uint32_t i = start; i < frames; i++) {
      const uint32_t t = i - start;
      acc[i] += env * (t < attack ? (float)t / attack : 1.0f) * s;
      const float ns = s * cw + c * sw;
      c = c * cw - s * sw;
      s = ns;
      env *= fall;
      if (env < 1e-4f) break;
    }
  }
}

const Partial BELL[] = {{1, 1, 1}, {2, 0.5f, 0.6f}, {3, 0.25f, 0.4f}, {4.2f, 0.12f, 0.3f}, {5.4f, 0.08f, 0.25f}};
const Partial ALERT[] = {{1, 1, 1}, {3, 0.3f, 0.6f}, {5, 0.12f, 0.4f}};
const Partial MARIMBA[] = {{1, 1, 1}, {4, 0.35f, 0.25f}, {10, 0.1f, 0.1f}};
const Partial TICK[] = {{1, 1, 1}, {2.7f, 0.5f, 0.6f}};

Clip* render(AudioSound s) {
  static const Note finished[] = {{0, 1046.5f, 0.35f}, {0.12f, 1318.5f, 0.35f}, {0.24f, 1568.0f, 0.35f}, {0.36f, 2093.0f, 0.6f}};
  static const Note error[] = {{0, 880.0f, 0.18f}, {0.18f, 659.3f, 0.18f}, {0.5f, 880.0f, 0.18f}, {0.68f, 659.3f, 0.25f}};
  static const Note cooled[] = {{0, 784.0f, 0.3f}, {0.15f, 1174.7f, 0.35f}};
  static const Note click[] = {{0, 2400.0f, 0.004f}};
  const uint32_t t0 = millis();
  const Note* notes;
  const Partial* p;
  int nn, np;
  float lenS, peak = 26000.0f;
  switch (s) {
    case SND_FINISHED: notes = finished; nn = 4; p = BELL; np = 5; lenS = 1.8f; break;
    case SND_ERROR: notes = error; nn = 4; p = ALERT; np = 3; lenS = 1.3f; break;
    case SND_COOLED: notes = cooled; nn = 2; p = MARIMBA; np = 3; lenS = 1.2f; break;
    case SND_CLICK: notes = click; nn = 1; p = TICK; np = 2; lenS = 0.03f; peak = 12000.0f; break;
    default: return nullptr;
  }
  const uint32_t frames = (uint32_t)(lenS * SYNTH_RATE);
  float* acc = (float*)heap_caps_calloc(frames, sizeof(float), MALLOC_CAP_SPIRAM);
  if (!acc) return nullptr;
  for (int i = 0; i < nn; i++) addNote(acc, frames, notes[i], p, np);
  float maxv = 1e-6f;
  for (uint32_t i = 0; i < frames; i++) maxv = max(maxv, fabsf(acc[i]));
  Clip* c = newClip(frames, SYNTH_RATE);
  if (c) {
    // The last notes still ring when the buffer ends; fade them out instead of cutting.
    const uint32_t fade = min<uint32_t>(SYNTH_RATE * 30 / 1000, frames / 4);
    const float k = peak / maxv;
    for (uint32_t i = 0; i < frames; i++) {
      const uint32_t left = frames - 1 - i;
      const float g = left < fade ? (float)left / fade : 1.0f;
      c->pcm[i] = (int16_t)lrintf(acc[i] * k * g);
    }
  }
  heap_caps_free(acc);
  s_renderMs = millis() - t0;
  return c;
}

Clip* renderTone(uint16_t hz, uint16_t ms) {
  const uint32_t frames = (uint32_t)ms * SYNTH_RATE / 1000;
  Clip* c = newClip(frames, SYNTH_RATE);
  if (!c) return nullptr;
  const uint32_t ramp = SYNTH_RATE * 5 / 1000;
  // Recursive oscillator: sinf() of a growing argument took ~300 ms per second of tone.
  const float w = 2.0f * (float)M_PI * hz / SYNTH_RATE, cw = cosf(w), sw = sinf(w);
  float s = 0.0f, co = 1.0f;
  for (uint32_t i = 0; i < frames; i++) {
    float a = 1.0f;
    if (i < ramp) a = (float)i / ramp;
    else if (frames - i < ramp) a = (float)(frames - i) / ramp;
    c->pcm[i] = (int16_t)(s * 26000.0f * a);
    const float ns = s * cw + co * sw;
    co = co * cw - s * sw;
    s = ns;
  }
  return c;
}

// ---- WAV ----

uint32_t rd32(const uint8_t* b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
uint16_t rd16(const uint8_t* b) { return b[0] | (b[1] << 8); }

struct WavInfo {
  uint32_t rate, frames;
  uint8_t channels;
};

// Leaves f at the first sample.
bool wavHeader(File& f, WavInfo& w, String& err) {
  uint8_t h[16];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    err = "not a WAV file";
    return false;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  bool haveFmt = false;
  while (f.read(h, 8) == 8) {
    const uint32_t sz = rd32(h + 4);
    // 64-bit: a chunk size near 4 GB must not wrap the seek back.
    const uint64_t next = (uint64_t)f.position() + sz + (sz & 1);
    if (!memcmp(h, "fmt ", 4) && sz >= 16) {
      if (f.read(h, 16) != 16) break;
      fmt = rd16(h);
      ch = rd16(h + 2);
      rate = rd32(h + 4);
      bits = rd16(h + 14);
      haveFmt = true;
      if (next > f.size() || !f.seek((uint32_t)next)) break;
    } else if (!memcmp(h, "data", 4)) {
      if (!haveFmt) break;
      if ((fmt != 1 && fmt != 0xFFFE) || bits != 16 || (ch != 1 && ch != 2)) {
        err = "needs 16-bit PCM, mono or stereo";
        return false;
      }
      if (rate < 8000 || rate > 48000) {
        err = "sample rate must be 8000 to 48000 Hz";
        return false;
      }
      w.rate = rate;
      w.channels = ch;
      w.frames = min<uint32_t>(sz, f.size() - f.position()) / (2 * ch);
      if (!w.frames) {
        err = "no samples";
        return false;
      }
      if (w.frames > MAX_CLIP_FRAMES) {
        err = "too long";
        return false;
      }
      return true;
    } else if (next > f.size() || !f.seek((uint32_t)next)) {
      break;
    }
  }
  err = "missing fmt or data chunk";
  return false;
}

String filePath(const char* name) { return String(audioDir()) + "/" + name; }

Clip* loadWav(const char* name, String& err) {
  const uint32_t t0 = millis();
  File f = deck::wallFs().open(filePath(name), "r");
  if (!f) {
    err = "file not found";
    return nullptr;
  }
  WavInfo w;
  if (!wavHeader(f, w, err)) return nullptr;
  Clip* c = newClip(w.frames, w.rate);
  if (!c) {
    err = "out of memory";
    return nullptr;
  }
  bool ok = true;
  if (w.channels == 1) {
    ok = f.read((uint8_t*)c->pcm, w.frames * 2) == w.frames * 2;
  } else {
    // Stereo is downmixed in blocks.
    int16_t* tmp = (int16_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    ok = tmp != nullptr;
    for (uint32_t done = 0; ok && done < w.frames;) {
      const uint32_t n = min<uint32_t>(512, w.frames - done);
      ok = f.read((uint8_t*)tmp, n * 4) == n * 4;
      for (uint32_t i = 0; ok && i < n; i++) c->pcm[done + i] = (int16_t)(((int32_t)tmp[2 * i] + tmp[2 * i + 1]) / 2);
      done += n;
    }
    heap_caps_free(tmp);
  }
  if (!ok) {
    freeClip(c);
    err = "read failed";
    return nullptr;
  }
  s_loadMs = millis() - t0;
  return c;
}

void flushFiles() {
  for (CachedFile& e : s_files) {
    freeClip(e.clip);
    e = CachedFile{};
  }
}

// Clip for a request; owned = the caller frees it after playing.
Clip* clipFor(const Req& r, bool& owned) {
  owned = false;
  if (r.type == REQ_TONE) {
    owned = true;
    return renderTone(r.hz, r.ms);
  }
  if (r.file[0]) {
    const uint32_t gen = s_fileGen;
    if (gen != s_seenGen) {
      flushFiles();
      s_seenGen = gen;
    }
    for (CachedFile& e : s_files)
      if (e.clip && !strcmp(e.name, r.file)) return e.clip;
    String err;
    Clip* c = loadWav(r.file, err);
    if (c) {
      if (c->frames * 2 > CACHE_MAX_BYTES) {
        owned = true;
        return c;
      }
      CachedFile& e = s_files[s_fileNext];
      s_fileNext = (s_fileNext + 1) % FILE_CACHE;
      freeClip(e.clip);
      strlcpy(e.name, r.file, sizeof(e.name));
      e.clip = c;
      return c;
    }
    // A missing or bad file plays the built-in sound.
    portENTER_CRITICAL(&s_mux);
    snprintf(s_fileErr, sizeof(s_fileErr), "%s: %s", r.file, err.c_str());
    portEXIT_CRITICAL(&s_mux);
  }
  if (!s_builtin[r.sound]) s_builtin[r.sound] = render(r.sound);
  return s_builtin[r.sound];
}

void playClip(const Clip* c, uint16_t gain, uint32_t reqMs, int16_t* buf) {
  if (s_stop || !openChannel(c->rate)) return;
  s_plays = s_plays + 1;
  s_playing = true;
  bool first = true;
  uint32_t pos = 0;
  while (pos < c->frames) {
    const bool cut = s_stop || (s_has && !s_req.low);
    const uint32_t n = min<uint32_t>(cut ? FADE_FRAMES : CHUNK, c->frames - pos);
    for (uint32_t i = 0; i < n; i++) {
      int32_t v = ((int32_t)c->pcm[pos + i] * gain) >> 12;
      if (cut) v = v * (int32_t)(n - i) / (int32_t)n;
      buf[i] = (int16_t)v;
    }
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(s_tx, buf, n * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
      s_writeErrors = s_writeErrors + 1;
      s_lastErr = err;
      break;
    }
    if (first) {
      s_latencyMs = millis() - reqMs;
      first = false;
    }
    pos += written / sizeof(int16_t);
    if (cut) break;
  }
  s_playing = false;
}

bool takeReq(Req& r) {
  portENTER_CRITICAL(&s_mux);
  const bool has = s_has;
  if (has) {
    r = s_req;
    s_stop = false;  // a stop after this point cancels this request, also while it loads
  }
  s_has = false;
  portEXIT_CRITICAL(&s_mux);
  return has;
}

void audioTask(void*) {
  static int16_t buf[CHUNK];
  for (;;) {
    const uint32_t woke = ulTaskNotifyTake(pdTRUE, s_tx ? pdMS_TO_TICKS(IDLE_OFF_MS) : portMAX_DELAY);
    Req r;
    if (!takeReq(r)) {
      // Without BCLK the NS4168 powers down.
      if (!woke) closeChannel();
      continue;
    }
    if (r.type == REQ_WARM) {
      for (int i = 0; i < SND_COUNT; i++)
        if (!s_builtin[i]) s_builtin[i] = render((AudioSound)i);
      continue;
    }
    bool owned;
    Clip* c = clipFor(r, owned);
    if (!c) continue;
    vTaskPrioritySet(nullptr, PRIO_PLAY);
    playClip(c, r.gain, r.reqMs, buf);
    vTaskPrioritySet(nullptr, PRIO_WORK);
    if (owned) freeClip(c);
  }
}

bool startTask() {
  if (s_task) return true;
  // Internal stack: the LittleFS fallback reads flash, which a PSRAM stack must not do.
  if (xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, PRIO_WORK, &s_task, 0) != pdPASS) {
    s_task = nullptr;
    return false;
  }
  return true;
}

bool submit(const Req& r) {
  if (!startTask()) return false;
  portENTER_CRITICAL(&s_mux);
  // A click never replaces a sound that is playing or waiting.
  const bool drop = r.low && (s_playing || (s_has && !s_req.low));
  if (!drop) {
    s_req = r;
    s_has = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!drop) xTaskNotifyGive(s_task);
  return true;
}

Req soundReq(AudioSound s, uint8_t vol, const char* file) {
  Req r = {};
  r.type = REQ_SOUND;
  r.sound = s;
  r.low = s == SND_CLICK;
  r.gain = gainFor(vol);
  r.reqMs = millis();
  if (file) strlcpy(r.file, file, sizeof(r.file));
  return r;
}

const char* eventFile(AudioSound s) {
  switch (s) {
    case SND_FINISHED: return audioSettings.fileFinished;
    case SND_ERROR: return audioSettings.fileError;
    case SND_COOLED: return audioSettings.fileCooled;
    default: return "";
  }
}

bool eventOn(AudioSound s) {
  switch (s) {
    case SND_FINISHED: return audioSettings.finished;
    case SND_ERROR: return audioSettings.error;
    case SND_COOLED: return audioSettings.cooled;
    case SND_CLICK: return audioSettings.click;
    default: return false;
  }
}

}  // namespace

void audioInit() {
  if (!audioSettings.enabled || !startTask()) return;
  // Built-in sounds are rendered once, off the UI loop.
  Req r = {};
  r.type = REQ_WARM;
  r.low = true;
  submit(r);
}

void audioEvent(AudioSound s) {
  if (!audioSettings.enabled || !audioSettings.volume || !eventOn(s)) return;
  if (audioSettings.nightMute && nightBrightnessActive()) return;
  submit(soundReq(s, audioSettings.volume, eventFile(s)));
}

void audioClick() { audioEvent(SND_CLICK); }

bool audioTest(AudioSound s, uint8_t vol, const String& file, String& err) {
  if (s >= SND_COUNT) {
    err = "unknown sound";
    return false;
  }
  if (file.length()) {
    uint32_t rate, ms;
    uint8_t ch;
    if (!audioProbe(file, rate, ch, ms, err)) return false;
  }
  if (!submit(soundReq(s, vol, file.c_str()))) {
    err = "the audio task did not start";
    return false;
  }
  return true;
}

bool audioPlayTone(uint16_t hz, uint16_t ms, uint8_t vol) {
  Req r = {};
  r.type = REQ_TONE;
  r.hz = constrain(hz, 50, 10000);
  r.ms = constrain(ms, 10, 5000);
  r.gain = gainFor(vol);
  r.reqMs = millis();
  return submit(r);
}

void audioStop() {
  portENTER_CRITICAL(&s_mux);
  s_has = false;
  s_stop = true;
  portEXIT_CRITICAL(&s_mux);
}

const char* audioDir() { return deck::wallOnSd() ? "/touchdeck/sound" : "/sound"; }

bool audioValidName(const String& n) {
  if (n.length() < 5 || n.length() >= AUDIO_FILE_LEN || !n.endsWith(".wav")) return false;
  for (char ch : n)
    if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.')) return false;
  return true;
}

bool audioProbe(const String& name, uint32_t& rate, uint8_t& channels, uint32_t& ms, String& err) {
  if (!audioValidName(name)) {
    err = "not a valid .wav name";
    return false;
  }
  return audioProbeFile(filePath(name.c_str()), rate, channels, ms, err);
}

bool audioProbeFile(const String& path, uint32_t& rate, uint8_t& channels, uint32_t& ms, String& err) {
  File f = deck::wallFs().open(path, "r");
  if (!f) {
    err = "file not found";
    return false;
  }
  WavInfo w;
  if (!wavHeader(f, w, err)) return false;
  rate = w.rate;
  channels = w.channels;
  ms = (uint32_t)((uint64_t)w.frames * 1000 / w.rate);
  return true;
}

void audioFilesChanged() { s_fileGen = s_fileGen + 1; }

void audioStatusJson(JsonObject o) {
  o["task"] = s_task != nullptr;
  o["on"] = s_tx != nullptr;
  o["playing"] = (bool)s_playing;
  o["rate"] = s_rate;
  o["plays"] = s_plays;
  o["latency_ms"] = s_latencyMs;
  o["underruns"] = s_underruns;
  o["write_errors"] = s_writeErrors;
  o["last_err"] = s_lastErr;
  o["opens"] = s_opens;
  o["channel_cost"] = s_channelCost;
  o["render_ms"] = s_renderMs;
  o["load_ms"] = s_loadMs;
  char fe[sizeof(s_fileErr)];
  portENTER_CRITICAL(&s_mux);
  memcpy(fe, s_fileErr, sizeof(fe));
  portEXIT_CRITICAL(&s_mux);
  o["file_error"] = fe;
  o["internal_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  o["internal_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  o["internal_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  o["task_stack_free"] = s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
  o["night_mute"] = audioSettings.nightMute && nightBrightnessActive();
}

void audioFilesJson(JsonArray a) {
  File d = deck::wallFs().open(audioDir());
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    const String name = f.name();
    if (f.isDirectory() || !name.endsWith(".wav")) continue;
    JsonObject e = a.add<JsonObject>();
    e["name"] = name;
    e["size"] = f.size();
    WavInfo w;
    String err;
    if (audioValidName(name) && wavHeader(f, w, err)) {
      e["rate"] = w.rate;
      e["channels"] = w.channels;
      e["ms"] = (uint32_t)((uint64_t)w.frames * 1000 / w.rate);
    } else {
      e["error"] = audioValidName(name) ? err : String("unsupported name");
    }
  }
}
