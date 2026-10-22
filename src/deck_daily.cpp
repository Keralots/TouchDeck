// Guition deck: daily wallpapers. See deck_daily.h.
#include "deck_daily.h"
#include "deck_wall.h"
#include "deck_config.h"
#include "deck_touch.h"
#include "psram_json.h"
#include "settings.h"
#include "config.h"
#include "tls_psram.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <math.h>
#include <time.h>
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
const char* tempPath() { return wallTemp(true); }
constexpr int MAX_API_CALLS = 12;  // 4 candidates each; some are progressive JPEGs
constexpr int MAX_LOG = 2 * DAILY_MAX_COUNT;
constexpr int MAX_TRIES = 3;       // failed runs per day
constexpr uint32_t RETRY_MS = 30 * 60 * 1000;

// The API (RSA chain) and the picture CDN (ECC chain) end in these roots.
constexpr const char* ROOT_CA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICPzCCAcWgAwIBAgIQBVVWvPJepDU1w6QP1atFcjAKBggqhkjOPQQDAzBhMQsw\n"
    "CQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cu\n"
    "ZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMzAe\n"
    "Fw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVTMRUw\n"
    "EwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20x\n"
    "IDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEczMHYwEAYHKoZIzj0CAQYF\n"
    "K4EEACIDYgAE3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FG\n"
    "fp4tn+6OYwwX7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPO\n"
    "Z9wj/wMco+I+o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAd\n"
    "BgNVHQ4EFgQUs9tIpPmhxdiuNkHMEWNpYim8S8YwCgYIKoZIzj0EAwMDaAAwZQIx\n"
    "AK288mw/EkrRLTnDCgmXc/SINoyIJ7vmiI1Qhadj+Z4y3maTD/HMsQmP3Wyr+mt/\n"
    "oAIwOWZbwmSNuJ5Q3KjVSaLtx9zRSX8XAbjIho9OjIgrqJqpisXRAL34VOKa5Vt8\n"
    "sycX\n"
    "-----END CERTIFICATE-----\n";

struct LogEntry {
  char name[13];
  bool portrait, ok;
  uint32_t bytesIn, bytesOut, ms;
  char error[24];
};

struct Status {
  bool running;
  int want[2], got[2], apiCalls, removed, archiveMax;
  uint32_t startMs, totalMs, minInternal;
  char step[40], error[64];
  LogEntry log[MAX_LOG];
  int logCount;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Status* s_status = nullptr;  // PSRAM, allocated by the first run
volatile bool s_changed[2] = {}, s_finished = false;
// Scheduler state, loop task only.
bool s_replan = false, s_lastLoaded = false;
uint32_t s_lastRun = 0;  // epoch of the last run that stored a picture (NVS "dlast")
uint32_t s_checkMs = 0, s_retryAtMs = 0, s_triesDay = 0;
int s_tries = 0;

template <typename F>
void update(F f) {
  portENTER_CRITICAL(&s_mux);
  f(*s_status);
  portEXIT_CRITICAL(&s_mux);
}

void sampleRam() {
  const uint32_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  update([&](Status& s) { if (!s.minInternal || free < s.minInternal) s.minInternal = free; });
}

void setStep(const char* text) {
  update([&](Status& s) { strlcpy(s.step, text, sizeof(s.step)); });
}

void setError(const char* text) {
  update([&](Status& s) { strlcpy(s.error, text, sizeof(s.error)); });
}

struct Pick {
  String url;
  bool portrait;
};

// One API call: up to 4 pictures, each in both orientations.
bool fetchApi(std::vector<Pick>& picks) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CA);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(tls, API_URL)) return false;
  const int code = http.GET();
  sampleRam();
  if (code != 200) {
    http.end();
    char text[40];
    snprintf(text, sizeof(text), code < 0 ? "Spotlight unreachable (%d)" : "Spotlight HTTP %d", code);
    setError(text);
    return false;
  }
  JsonDocument filter;
  filter["batchrsp"]["items"][0]["item"] = true;
  JsonDocument doc(psramJson());
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    setError("Spotlight sent an unexpected reply");
    return false;
  }
  for (JsonVariant item : doc["batchrsp"]["items"].as<JsonArray>()) {
    // Each item is a JSON document stored as a string.
    JsonDocument ad(psramJson());
    if (deserializeJson(ad, item["item"].as<const char*>())) continue;
    const char* urls[2] = {ad["ad"]["landscapeImage"]["asset"], ad["ad"]["portraitImage"]["asset"]};
    for (int p = 0; p < 2; p++)
      if (urls[p] && !strncmp(urls[p], "https://", 8)) picks.push_back({urls[p], p == 1});
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

uint64_t fsFree() { return wallFreeBytes(); }

String dailyPath(bool portrait, const String& name) { return String(wallDir(portrait, true)) + "/" + name; }

// Make room for one more picture by dropping daily files that are not kept.
bool makeRoom(bool portrait, const WallVector<uint64_t>& keep) {
  while (fsFree() < WALL_RESERVE + WALL_MAX_JPEG) {
    bool removed = false;
    auto files = wallListDir(wallDir(portrait, true));
    if (wallOnSd()) std::sort(files.begin(), files.end(), [](const WallFile& a, const WallFile& b) {
      return a.date == b.date ? a.id < b.id : a.date < b.date;
    });
    for (const WallFile& f : files) {
      if (std::find(keep.begin(), keep.end(), (uint64_t)(uint32_t)f.id) != keep.end()) continue;
      if (!wallRemoveFile(portrait, true, f.name(), wallOnSd())) continue;
      update([](Status& s) { s.removed++; });
      s_changed[portrait] = true;
      removed = true;
      break;
    }
    if (!removed) return false;
  }
  return true;
}

// Drop the oldest daily pictures above max; -1 when protected ones keep it above.
int trimArchive(bool portrait, int max) {
  auto files = wallListDir(wallDir(portrait, true));
  std::sort(files.begin(), files.end(), [](const WallFile& a, const WallFile& b) {
    return a.date == b.date ? a.id < b.id : a.date < b.date;
  });
  size_t count = files.size();
  int removed = 0;
  for (const auto& f : files) {
    if (count <= (size_t)max) break;
    if (!wallRemoveFile(portrait, true, f.name(), true)) continue;
    --count;
    ++removed;
  }
  return count > (size_t)max ? -1 : removed;
}

void clearDaily(bool portrait) {
  for (const WallFile& f : wallListDir(wallDir(portrait, true))) {
    if (!wallRemoveFile(portrait, true, f.name())) continue;
    update([](Status& s) { s.removed++; });
    s_changed[portrait] = true;
  }
}

// Download, decode at 1/2^n scale, crop, resample, encode and store one picture.
// False with log.error set; a network failure also sets *offline.
bool fetchImage(const Pick& pick, const String& name, LogEntry& log, bool* offline) {
  const bool p = pick.portrait;
  const int tw = p ? 320 : 480, th = p ? 480 : 320;
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CA);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(tls, pick.url)) {
    strlcpy(log.error, "bad address", sizeof(log.error));
    return false;
  }
  const int code = http.GET();
  sampleRam();
  if (code != 200) {
    http.end();
    *offline = code < 0;
    snprintf(log.error, sizeof(log.error), "HTTP %d", code);
    return false;
  }
  Decode d = {http.getStreamPtr(), nullptr, 0, 0, 0, 0};
  void* pool = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  JDEC jd;
  JRESULT r = pool ? romPrepare(&jd, readInput, pool, 4096, &d) : JDR_MEM1;
  uint8_t scale = 0;
  if (r == JDR_OK) {
    while (scale < 3 && (int)(jd.width >> (scale + 1)) >= tw && (int)(jd.height >> (scale + 1)) >= th) scale++;
    d.w = (jd.width + (1 << scale) - 1) >> scale;
    d.h = (jd.height + (1 << scale) - 1) >> scale;
    if (d.w < tw || d.h < th) r = JDR_FMT2;
    else {
      d.px = (uint8_t*)heap_caps_malloc((size_t)d.w * d.h * 3, MALLOC_CAP_SPIRAM);
      r = d.px ? romDecomp(&jd, writeOutput, scale) : JDR_MEM1;
    }
  }
  http.end();
  free(pool);
  log.bytesIn = d.bytes;
  if (r != JDR_OK) {
    free(d.px);
    strlcpy(log.error,
            r == JDR_FMT3 ? "progressive JPEG" : r == JDR_FMT2 ? "picture too small"
            : r == JDR_MEM1 ? "no PSRAM" : r == JDR_INP ? "download cut" : "bad JPEG",
            sizeof(log.error));
    return false;
  }
  uint8_t* out = (uint8_t*)heap_caps_malloc(tw * th * 3, MALLOC_CAP_SPIRAM);
  const bool scaled = out && resample(d.px, d.w, d.h, out, tw, th);
  free(d.px);
  if (!scaled) {
    free(out);
    strlcpy(log.error, "no PSRAM", sizeof(log.error));
    return false;
  }
  bool ok = false;
  for (int quality = 85; quality >= 60 && !ok; quality -= 10) {
    File f = wallFs().open(tempPath(), "w");
    ok = f && fmt2jpg_cb(out, tw * th * 3, tw, th, PIXFORMAT_RGB888, quality, writeJpeg, &f);
    log.bytesOut = f ? f.position() : 0;
    f.close();
    ok = ok && log.bytesOut <= WALL_MAX_JPEG;
  }
  free(out);
  ok = ok && wallValidateJpeg(tempPath(), p);
  if (ok) {
    wallFs().remove(dailyPath(p, name));
    ok = wallFs().rename(tempPath(), dailyPath(p, name));
  }
  wallFs().remove(tempPath());
  if (ok) wallIndexChanged(p, true, name);
  if (!ok) strlcpy(log.error, "could not store", sizeof(log.error));
  return ok;
}

void runTask(void*) {
  int want[2], archiveMax;
  portENTER_CRITICAL(&s_mux);
  want[0] = s_status->want[0];
  want[1] = s_status->want[1];
  archiveMax = s_status->archiveMax;
  portEXIT_CRITICAL(&s_mux);
  if (!tlsPsramInstall()) setError("TLS setup failed");
  else {
    wallFs().remove(tempPath());
    wallFs().mkdir(wallRoot());
    wallFs().mkdir(wallDir(false, true));
    wallFs().mkdir(wallDir(true, true));
    for (int p = 0; p < 2; p++)
      if (!wallOnSd() && !want[p]) clearDaily(p);  // orientation no longer wanted: free its flash first
    WallVector<uint64_t> keep[2], seen;
    if (wallOnSd())
      for (int p = 0; p < 2; p++)
        for (const auto& f : wallListDir(wallDir(p, true)))
          seen.push_back((uint64_t)(uint32_t)f.id | (uint64_t)p << 32);
    bool stop = false;
    int got[2] = {}, offlineRun = 0;
    for (int call = 0; call < MAX_API_CALLS && !stop && (got[0] < want[0] || got[1] < want[1]); call++) {
      setStep("Asking Spotlight");
      update([](Status& s) { s.apiCalls++; });
      std::vector<Pick> picks;
      if (!fetchApi(picks)) break;
      for (const Pick& pick : picks) {
        const bool p = pick.portrait;
        if (got[p] >= want[p]) continue;
        char name[13];
        snprintf(name, sizeof(name), "%08lx.jpg",
                 (unsigned long)esp_rom_crc32_le(0, (const uint8_t*)pick.url.c_str(), pick.url.length()));
        const uint64_t id = wallId(name) | (uint64_t)(wallOnSd() && p) << 32;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        if (wallOnSd() && wallFs().exists(dailyPath(p, name))) continue;
        LogEntry log = {};
        strlcpy(log.name, name, sizeof(log.name));
        log.portrait = p;
        char step[40];
        snprintf(step, sizeof(step), "Picture %d of %d", got[0] + got[1] + 1, want[0] + want[1]);
        setStep(step);
        const uint32_t start = millis();
        bool offline = false;
        if (wallFs().exists(dailyPath(p, name))) log.ok = true;
        else if (!makeRoom(p, keep[p])) strlcpy(log.error, "flash full", sizeof(log.error));
        else log.ok = fetchImage(pick, name, log, &offline);
        log.ms = millis() - start;
        if (log.ok) {
          keep[p].push_back(wallId(name));
          got[p]++;
          s_changed[p] = true;
        }
        offlineRun = offline ? offlineRun + 1 : 0;
        update([&](Status& s) {
          s.got[p] = got[p];
          if (s.logCount == MAX_LOG) memmove(s.log, s.log + 1, sizeof(LogEntry) * --s.logCount);
          s.log[s.logCount++] = log;
        });
        if (offlineRun >= 3 || !strcmp(log.error, "flash full")) {
          if (offlineRun >= 3) setError("Picture server unreachable");
          else setError("Flash full: delete some of your own pictures");
          stop = true;
          break;
        }
      }
    }
    // A full set replaces the previous pictures; a short one keeps them.
    for (int p = 0; p < 2; p++) {
      if (wallOnSd()) {
        const int removed = trimArchive(p, archiveMax);
        update([removed](Status& s) { s.removed += removed < 0 ? 0 : removed; });
        if (removed != 0) s_changed[p] = true;
        if (removed < 0) setError("Could not trim daily archive");
        continue;
      }
      if (!want[p] || got[p] < want[p]) continue;
      for (const WallFile& f : wallListDir(wallDir(p, true)))
        if (std::find(keep[p].begin(), keep[p].end(), (uint64_t)(uint32_t)f.id) == keep[p].end()) {
          if (!wallRemoveFile(p, true, f.name())) continue;
          update([](Status& s) { s.removed++; });
          s_changed[p] = true;
        }
    }
  }
  wallRebuildIndex();
  update([&](Status& s) {
    s.running = false;
    s.totalMs = millis() - s.startMs;
    strlcpy(s.step, "Done", sizeof(s.step));
  });
  s_finished = true;
  vTaskDelete(nullptr);
}

uint32_t dayKey(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

void loadLastRun() {
  if (s_lastLoaded) return;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    s_lastRun = prefs.getUInt("dlast", 0);
    prefs.end();
  }
  s_lastLoaded = true;
}

void saveLastRun(uint32_t t) {
  s_lastRun = t;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUInt("dlast", t);
    prefs.end();
  }
}

bool wants(int p) { return dispSettings.dailyOrient == 2 || dispSettings.dailyOrient == p; }

}  // namespace

bool dailyStart() {
  if (!s_status) s_status = (Status*)heap_caps_calloc(1, sizeof(Status), MALLOC_CAP_SPIRAM);
  if (!s_status || (!wallOnSd() && !deckFsReady()) || !WiFi.isConnected() ||
      (wallOnSd() && time(nullptr) < 1700000000)) return false;
  bool busy;
  portENTER_CRITICAL(&s_mux);
  busy = s_status->running;
  if (!busy) {
    memset(s_status, 0, sizeof(Status));
    s_status->running = true;
    const int count = std::min<int>(dispSettings.dailyCount, wallOnSd() ? 32 : 16);
    s_status->want[0] = wants(0) ? count : 0;
    s_status->want[1] = wants(1) ? count : 0;
    s_status->archiveMax = dispSettings.dailyMax;
    s_status->startMs = millis();
    strlcpy(s_status->step, "Starting", sizeof(s_status->step));
  }
  portEXIT_CRITICAL(&s_mux);
  if (busy) return false;
  // Internal stack: LittleFS writes disable the PSRAM cache.
  if (xTaskCreatePinnedToCore(runTask, "deck_daily", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    update([](Status& s) { s.running = false; });
    return false;
  }
  s_replan = false;
  return true;
}

void dailyTrim(bool portrait) {
  if (wallOnSd() && !dailyRunning()) trimArchive(portrait, dispSettings.dailyMax);
}

bool dailyRunning() {
  if (!s_status) return false;
  portENTER_CRITICAL(&s_mux);
  const bool running = s_status->running;
  portEXIT_CRITICAL(&s_mux);
  return running;
}

bool dailyProgress(int& done, int& total) {
  if (!s_status) return false;
  portENTER_CRITICAL(&s_mux);
  const bool running = s_status->running;
  done = s_status->got[0] + s_status->got[1];
  total = s_status->want[0] + s_status->want[1];
  portEXIT_CRITICAL(&s_mux);
  return running;
}

void dailyReplan() {
  s_replan = true;
  s_tries = 0;
  s_retryAtMs = 0;
}

void dailyStatus(JsonObject out) {
  loadLastRun();
  out["active"] = wallDaily();
  out["source"] = wallSource();
  out["archiveMax"] = wallOnSd() ? dispSettings.dailyMax : 0;
  out["running"] = false;
  out["lastRun"] = s_lastRun;
  out["files"][0] = wallListDir(wallDir(false, true)).size();
  out["files"][1] = wallListDir(wallDir(true, true)).size();
  out["fsFree"] = fsFree();
  Status* s = s_status ? (Status*)heap_caps_calloc(1, sizeof(Status), MALLOC_CAP_SPIRAM) : nullptr;
  if (!s) return;
  portENTER_CRITICAL(&s_mux);
  *s = *s_status;
  portEXIT_CRITICAL(&s_mux);
  // char arrays would be stored by pointer (as literals); s is freed below.
  out["running"] = s->running;
  out["step"] = (const char*)s->step;
  out["error"] = (const char*)s->error;
  out["want"][0] = s->want[0];
  out["want"][1] = s->want[1];
  out["got"][0] = s->got[0];
  out["got"][1] = s->got[1];
  out["removed"] = s->removed;
  out["ms"] = s->running ? millis() - s->startMs : s->totalMs;
  out["minInternal"] = s->minInternal;
  JsonArray log = out["log"].to<JsonArray>();
  for (int i = 0; i < s->logCount; i++) {
    const LogEntry& e = s->log[i];
    JsonObject o = log.add<JsonObject>();
    o["n"] = (const char*)e.name;
    o["o"] = e.portrait ? "p" : "l";
    o["ok"] = e.ok;
    if (e.error[0]) o["err"] = (const char*)e.error;
    o["in"] = e.bytesIn;
    o["out"] = e.bytesOut;
    o["ms"] = e.ms;
  }
  free(s);
}

void dailyPoll() {
  for (int p = 0; p < 2; p++)
    if (s_changed[p]) {
      s_changed[p] = false;
      if (wallDaily()) wallGalleryChanged(p);
    }
  if (s_finished) {
    s_finished = false;
    portENTER_CRITICAL(&s_mux);
    const bool stored = s_status->got[0] + s_status->got[1] > 0;
    portEXIT_CRITICAL(&s_mux);
    if (stored) saveLastRun(time(nullptr));
    else s_retryAtMs = millis() + RETRY_MS;
  }
  if (!wallDaily() || dailyRunning() || millis() - s_checkMs < 10000) return;
  s_checkMs = millis();
  const time_t now = time(nullptr);
  if (now < 1700000000 || !WiFi.isConnected()) return;  // clock not set yet
  loadLastRun();
  struct tm tm;
  localtime_r(&now, &tm);
  const uint32_t today = dayKey(now);
  if (s_triesDay != today) {
    s_triesDay = today;
    s_tries = 0;
  }
  bool empty = false;
  for (int p = 0; p < 2; p++) empty |= wants(p) && wallListDir(wallDir(p, true)).empty();
  const bool due = s_replan || empty ||
                   ((!s_lastRun || dayKey(s_lastRun) != today) && tm.tm_hour >= dispSettings.dailyHour);
  if (!due || s_tries >= MAX_TRIES) return;
  if (s_retryAtMs && (int32_t)(millis() - s_retryAtMs) < 0) return;
  // Not while someone is using the screen: decoding slows page swipes.
  if (millis() - touchLastActivity() < WALL_IDLE_MS) return;
  if (dailyStart()) {
    s_tries++;
    s_retryAtMs = 0;
  }
}

}  // namespace deck
