// Guition deck: daily wallpapers. See deck_daily.h.
#include "deck_daily.h"
#include "deck_wall.h"
#include "deck_config.h"
#include "psram_json.h"
#include "tls_psram.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include "img_converters.h"
#include "esp32s3/rom/tjpgd.h"

namespace deck {
namespace {

// ROM TJpgDec can scale while it decodes (LVGL's copy is built without that).
// Called by address (esp32s3.rom.ld) because LVGL defines the same names.
using PrepareFn = JRESULT (*)(JDEC*, UINT (*)(JDEC*, BYTE*, UINT), void*, UINT, void*);
using DecompFn = JRESULT (*)(JDEC*, UINT (*)(JDEC*, void*, JRECT*), BYTE);
const PrepareFn romPrepare = (PrepareFn)0x40000858;
const DecompFn romDecomp = (DecompFn)0x40000864;

constexpr const char* API_URL =
    "https://fd.api.iris.microsoft.com/v4/api/selection?placement=88000820&bcnt=4&country=US&locale=en-US&fmt=json";
constexpr const char* TMP_PATH = "/wall/daily.up";
constexpr int MAX_API_CALLS = 10;
constexpr int MAX_LOG = 24;

struct LogEntry {
  char name[13];
  bool portrait, ok;
  uint16_t srcW, srcH;
  uint8_t scale;
  uint32_t bytesIn, bytesOut, fetchMs, resampleMs, encodeMs;
  char error[32];
  char url[200];
};

struct Status {
  bool running;
  int wantL, wantP, gotL, gotP, apiCalls, removed;
  uint32_t startMs, totalMs;
  uint32_t stackFree, minInternal;
  char step[48], error[64];
  LogEntry log[MAX_LOG];
  int logCount;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Status* s_status = nullptr;  // PSRAM, allocated by the first run
volatile bool s_changed[2] = {};

template <typename F>
void update(F f) {
  portENTER_CRITICAL(&s_mux);
  f(*s_status);
  portEXIT_CRITICAL(&s_mux);
}

// Spike: lowest internal heap seen during the run.
void sampleRam() {
  const uint32_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  update([&](Status& s) { if (!s.minInternal || free < s.minInternal) s.minInternal = free; });
}

void setStep(const char* text) {
  update([&](Status& s) { strlcpy(s.step, text, sizeof(s.step)); });
}

struct Pick {
  String url;
  bool portrait;
};

bool fetchApi(std::vector<Pick>& picks, int wantL, int wantP) {
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(tls, API_URL)) return false;
  const int code = http.GET();
  if (code != 200) {
    http.end();
    update([&](Status& s) { snprintf(s.error, sizeof(s.error), "API HTTP %d", code); });
    return false;
  }
  JsonDocument filter;
  filter["batchrsp"]["items"][0]["item"] = true;
  JsonDocument doc(psramJson());
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    update([&](Status& s) { snprintf(s.error, sizeof(s.error), "API JSON %s", err.c_str()); });
    return false;
  }
  int haveL = 0, haveP = 0;
  for (const Pick& p : picks) (p.portrait ? haveP : haveL)++;
  for (JsonVariant item : doc["batchrsp"]["items"].as<JsonArray>()) {
    // Each item is a JSON document stored as a string.
    JsonDocument ad(psramJson());
    if (deserializeJson(ad, item["item"].as<const char*>())) continue;
    const char* urls[2] = {ad["ad"]["landscapeImage"]["asset"], ad["ad"]["portraitImage"]["asset"]};
    for (int p = 0; p < 2; p++) {
      if (!urls[p] || strncmp(urls[p], "https://", 8)) continue;
      if (p ? haveP >= wantP : haveL >= wantL) continue;
      bool dup = false;
      for (const Pick& old : picks) dup |= old.url == urls[p];
      if (dup) continue;
      picks.push_back({urls[p], p == 1});
      (p ? haveP : haveL)++;
    }
  }
  return true;
}

struct Decode {
  NetworkClient* stream;
  uint8_t* px;  // RGB888 at the decoded scale
  int w, h;
  uint32_t bytes, blocks;
};

UINT readInput(JDEC* jd, BYTE* buf, UINT n) {
  Decode& d = *(Decode*)jd->device;
  uint8_t skip[64];
  UINT got = 0;
  uint32_t last = millis();
  while (got < n) {
    const size_t want = buf ? n - got : std::min<size_t>(n - got, sizeof(skip));
    // The TLS client returns -1 while no record has arrived yet, not 0.
    const int r = d.stream->read(buf ? buf + got : skip, want);
    if (r > 0) {
      got += r;
      last = millis();
    } else if ((!d.stream->connected() && !d.stream->available()) || millis() - last > 15000) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  d.bytes += got;
  return got;
}

UINT writeOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  Decode& d = *(Decode*)jd->device;
  const uint8_t* src = (const uint8_t*)bitmap;
  const int rw = rect->right - rect->left + 1;
  for (int y = rect->top; y <= rect->bottom; y++, src += rw * 3) {
    if (y >= d.h) continue;
    const int n = std::min(rw, d.w - (int)rect->left);
    if (n > 0) memcpy(d.px + ((size_t)y * d.w + rect->left) * 3, src, n * 3);
  }
  // Let the core 0 idle task feed its watchdog during long decodes.
  if ((++d.blocks & 255) == 0) {
    sampleRam();
    vTaskDelay(1);
  }
  return 1;
}

// Area average of one source row into tw pixels, starting at x0 with step fx.
void areaRow(const uint8_t* row, int sw, float x0, float fx, int tw, float* out) {
  for (int x = 0; x < tw; x++) {
    const float a = x0 + x * fx, b = a + fx;
    float r = 0, g = 0, bl = 0;
    for (int sx = (int)a; sx < (int)ceilf(b) && sx < sw; sx++) {
      const float w = fminf(b, sx + 1) - fmaxf(a, sx);
      const uint8_t* p = row + sx * 3;
      r += w * p[0];
      g += w * p[1];
      bl += w * p[2];
    }
    out[x * 3] = r / fx;
    out[x * 3 + 1] = g / fx;
    out[x * 3 + 2] = bl / fx;
  }
}

// Centre crop to the target aspect, then box-filter down to tw x th.
bool resample(const uint8_t* src, int sw, int sh, uint8_t* dst, int tw, int th) {
  float cw = sw, ch = sh;
  if ((float)sw / sh > (float)tw / th) cw = sh * (float)tw / th;
  else ch = sw * (float)th / tw;
  const float cx = (sw - cw) / 2, cy = (sh - ch) / 2, fx = cw / tw, fy = ch / th;
  float* acc = (float*)heap_caps_malloc(tw * 3 * sizeof(float) * 2, MALLOC_CAP_SPIRAM);
  if (!acc) return false;
  float* row = acc + tw * 3;
  for (int y = 0; y < th; y++) {
    const float a = cy + y * fy, b = a + fy;
    memset(acc, 0, tw * 3 * sizeof(float));
    for (int sy = (int)a; sy < (int)ceilf(b) && sy < sh; sy++) {
      const float w = fminf(b, sy + 1) - fmaxf(a, sy);
      areaRow(src + (size_t)sy * sw * 3, sw, cx, fx, tw, row);
      for (int i = 0; i < tw * 3; i++) acc[i] += w * row[i];
    }
    // ROM TJpgDec emits R, G, B; the esp32-camera encoder reads B, G, R.
    uint8_t* out = dst + (size_t)y * tw * 3;
    for (int i = 0; i < tw * 3; i++) {
      const float v = acc[i - i % 3 + 2 - i % 3] / fy;
      out[i] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f);
    }
    if ((y & 15) == 15) vTaskDelay(1);
  }
  free(acc);
  return true;
}

size_t writeJpeg(void* arg, size_t, const void* data, size_t len) {
  return ((File*)arg)->write((const uint8_t*)data, len);
}

size_t fsFree() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

// Make room for one more picture by dropping files that are not kept.
bool makeRoom(const char* dir, const std::vector<String>& keep) {
  while (fsFree() < WALL_RESERVE + WALL_MAX_JPEG) {
    bool removed = false;
    for (const WallFile& f : wallListDir(dir)) {
      if (std::find(keep.begin(), keep.end(), f.name) != keep.end()) continue;
      LittleFS.remove(String(dir) + "/" + f.name);
      update([](Status& s) { s.removed++; });
      removed = true;
      break;
    }
    if (!removed) return false;
  }
  return true;
}

bool fetchImage(const Pick& pick, const String& name, LogEntry& log) {
  const bool p = pick.portrait;
  const int tw = p ? 320 : 480, th = p ? 480 : 320;
  uint32_t t = millis();
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(tls, pick.url)) {
    strlcpy(log.error, "bad URL", sizeof(log.error));
    return false;
  }
  const int code = http.GET();
  sampleRam();
  if (code != 200) {
    http.end();
    snprintf(log.error, sizeof(log.error), "HTTP %d", code);
    return false;
  }
  Decode d = {http.getStreamPtr(), nullptr, 0, 0, 0, 0};
  void* pool = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  JDEC jd;
  JRESULT r = pool ? romPrepare(&jd, readInput, pool, 4096, &d) : JDR_MEM1;
  uint8_t scale = 0;
  if (r == JDR_OK) {
    log.srcW = jd.width;
    log.srcH = jd.height;
    while (scale < 3 && (int)(jd.width >> (scale + 1)) >= tw && (int)(jd.height >> (scale + 1)) >= th) scale++;
    d.w = (jd.width + (1 << scale) - 1) >> scale;
    d.h = (jd.height + (1 << scale) - 1) >> scale;
    log.scale = 1 << scale;
    if (d.w < tw || d.h < th) r = JDR_FMT2;
    else {
      d.px = (uint8_t*)heap_caps_malloc((size_t)d.w * d.h * 3, MALLOC_CAP_SPIRAM);
      r = d.px ? romDecomp(&jd, writeOutput, scale) : JDR_MEM1;
    }
  }
  http.end();
  free(pool);
  log.bytesIn = d.bytes;
  log.fetchMs = millis() - t;
  if (r != JDR_OK) {
    free(d.px);
    snprintf(log.error, sizeof(log.error), "decode %d", (int)r);
    return false;
  }
  t = millis();
  uint8_t* out = (uint8_t*)heap_caps_malloc(tw * th * 3, MALLOC_CAP_SPIRAM);
  const bool scaled = out && resample(d.px, d.w, d.h, out, tw, th);
  free(d.px);
  log.resampleMs = millis() - t;
  if (!scaled) {
    free(out);
    strlcpy(log.error, "no PSRAM", sizeof(log.error));
    return false;
  }
  t = millis();
  bool ok = false;
  for (int quality = 85; quality >= 60 && !ok; quality -= 10) {
    File f = LittleFS.open(TMP_PATH, "w");
    ok = f && fmt2jpg_cb(out, tw * th * 3, tw, th, PIXFORMAT_RGB888, quality, writeJpeg, &f);
    log.bytesOut = f ? f.position() : 0;
    f.close();
    ok = ok && log.bytesOut <= WALL_MAX_JPEG;
  }
  free(out);
  ok = ok && wallValidateJpeg(TMP_PATH, p);
  const String path = String(wallDir(p, true)) + "/" + name;
  if (ok) {
    LittleFS.remove(path);
    ok = LittleFS.rename(TMP_PATH, path);
  }
  LittleFS.remove(TMP_PATH);
  log.encodeMs = millis() - t;
  if (!ok) strlcpy(log.error, "encode/store", sizeof(log.error));
  return ok;
}

void runTask(void*) {
  int wantL, wantP;
  portENTER_CRITICAL(&s_mux);
  wantL = s_status->wantL;
  wantP = s_status->wantP;
  portEXIT_CRITICAL(&s_mux);
  if (!tlsPsramInstall()) {
    update([](Status& s) { strlcpy(s.error, "TLS allocator setup failed", sizeof(s.error)); });
  } else {
    LittleFS.mkdir("/wall");
    LittleFS.mkdir(wallDir(false, true));
    LittleFS.mkdir(wallDir(true, true));
    std::vector<Pick> picks;
    for (int call = 0; call < MAX_API_CALLS && (int)picks.size() < wantL + wantP; call++) {
      setStep("asking Spotlight");
      update([](Status& s) { s.apiCalls++; });
      if (!fetchApi(picks, wantL, wantP)) break;
    }
    std::vector<String> keep[2];
    int index = 0;
    for (const Pick& pick : picks) {
      char name[13];
      snprintf(name, sizeof(name), "%08lx.jpg",
               (unsigned long)esp_rom_crc32_le(0, (const uint8_t*)pick.url.c_str(), pick.url.length()));
      const char* dir = wallDir(pick.portrait, true);
      keep[pick.portrait].push_back(name);
      LogEntry log = {};
      strlcpy(log.name, name, sizeof(log.name));
      strlcpy(log.url, pick.url.c_str(), sizeof(log.url));
      log.portrait = pick.portrait;
      char step[48];
      snprintf(step, sizeof(step), "picture %d of %d", ++index, (int)picks.size());
      setStep(step);
      if (LittleFS.exists(String(dir) + "/" + name)) {
        log.ok = true;
        strlcpy(log.error, "already here", sizeof(log.error));
      } else if (!makeRoom(dir, keep[pick.portrait])) {
        strlcpy(log.error, "flash full", sizeof(log.error));
      } else {
        log.ok = fetchImage(pick, name, log);
      }
      if (!log.ok) keep[pick.portrait].pop_back();
      else s_changed[pick.portrait] = true;
      update([&](Status& s) {
        if (log.ok) (pick.portrait ? s.gotP : s.gotL)++;
        if (s.logCount < MAX_LOG) s.log[s.logCount++] = log;
      });
    }
    // A full set replaces yesterday's pictures; a short one keeps them.
    for (int p = 0; p < 2; p++) {
      if ((int)keep[p].size() < (p ? wantP : wantL)) continue;
      for (const WallFile& f : wallListDir(wallDir(p, true)))
        if (std::find(keep[p].begin(), keep[p].end(), f.name) == keep[p].end()) {
          LittleFS.remove(String(wallDir(p, true)) + "/" + f.name);
          update([](Status& s) { s.removed++; });
          s_changed[p] = true;
        }
    }
  }
  const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  update([&](Status& s) {
    s.running = false;
    s.totalMs = millis() - s.startMs;
    s.stackFree = stackFree;
    strlcpy(s.step, "done", sizeof(s.step));
  });
  vTaskDelete(nullptr);
}

}  // namespace

bool dailyStart(int landscape, int portrait) {
  if (!s_status) s_status = (Status*)heap_caps_calloc(1, sizeof(Status), MALLOC_CAP_SPIRAM);
  if (!s_status) return false;
  bool busy;
  portENTER_CRITICAL(&s_mux);
  busy = s_status->running;
  if (!busy) {
    memset(s_status, 0, sizeof(Status));
    s_status->running = true;
    s_status->wantL = landscape;
    s_status->wantP = portrait;
    s_status->startMs = millis();
  }
  portEXIT_CRITICAL(&s_mux);
  if (busy || !deckFsReady()) return false;
  // Internal stack: LittleFS writes disable the PSRAM cache.
  if (xTaskCreatePinnedToCore(runTask, "deck_daily", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    update([](Status& s) { s.running = false; });
    return false;
  }
  return true;
}

bool dailyRunning() {
  if (!s_status) return false;
  portENTER_CRITICAL(&s_mux);
  const bool running = s_status->running;
  portEXIT_CRITICAL(&s_mux);
  return running;
}

void dailyStatus(JsonObject out) {
  Status* s = (Status*)heap_caps_calloc(1, sizeof(Status), MALLOC_CAP_SPIRAM);
  if (!s) return;
  portENTER_CRITICAL(&s_mux);
  if (s_status) *s = *s_status;
  portEXIT_CRITICAL(&s_mux);
  out["running"] = s->running;
  out["step"] = (const char*)s->step;
  out["error"] = (const char*)s->error;
  out["want"] = String(s->wantL) + "/" + s->wantP;
  out["got"] = String(s->gotL) + "/" + s->gotP;
  out["apiCalls"] = s->apiCalls;
  out["removed"] = s->removed;
  out["ms"] = s->running ? millis() - s->startMs : s->totalMs;
  out["stackFree"] = s->stackFree;
  out["minInternal"] = s->minInternal;
  out["daily"] = wallDaily();
  out["fsFree"] = deckFsReady() ? fsFree() : 0;
  out["psramFree"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  out["psramBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  out["internalFree"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // char arrays would be stored by pointer (as literals); s is freed below.
  JsonArray log = out["log"].to<JsonArray>();
  for (int i = 0; i < s->logCount; i++) {
    const LogEntry& e = s->log[i];
    JsonObject o = log.add<JsonObject>();
    o["n"] = (const char*)e.name;
    o["o"] = e.portrait ? "p" : "l";
    o["ok"] = e.ok;
    if (e.error[0]) o["err"] = (const char*)e.error;
    o["src"] = String(e.srcW) + "x" + e.srcH + "/" + e.scale;
    o["in"] = e.bytesIn;
    o["out"] = e.bytesOut;
    o["fetch"] = e.fetchMs;
    o["scale"] = e.resampleMs;
    o["enc"] = e.encodeMs;
    o["url"] = (const char*)e.url;
  }
  free(s);
}

void dailyPoll() {
  for (int p = 0; p < 2; p++)
    if (s_changed[p]) {
      s_changed[p] = false;
      if (wallDaily()) wallGalleryChanged(p);
    }
}

}  // namespace deck
