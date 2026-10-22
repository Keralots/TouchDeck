// Guition deck: Glass theme wallpaper. See deck_wall.h.
#include "deck_wall.h"
#include <src/misc/cache/instance/lv_image_cache.h>
#include "deck_config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <Preferences.h>
#include <esp_random.h>
#include <algorithm>
#include "settings.h"
#include "config.h"
#include <src/libs/tjpgd/tjpgd.h>

namespace deck {
namespace {

uint16_t* s_sharp = nullptr;  // RGB565, dimmed and dithered
uint8_t* s_blur = nullptr;    // RGB888 (LVGL order B, G, R), undimmed
int s_w = 0, s_h = 0, s_blurR = -1, s_dim = -1;
lv_image_dsc_t s_sharpDsc, s_blurDsc;
uint32_t s_generation = 0;
String s_current[2], s_pin[2], s_requested[2], s_loadedName;
bool s_initialized[2] = {}, s_request[2] = {}, s_automatic[2] = {};
uint8_t s_source = 0;
bool s_sourceLoaded = false;
WallVector<uint64_t> s_shown[2];  // shuffle: pictures shown in this round, RAM only
uint32_t s_requestId[2] = {};
uint32_t s_epoch = 0, s_changedMs = 0, s_retryMs = 0, s_scanMs = 0;
portMUX_TYPE s_jobMux = portMUX_INITIALIZER_UNLOCKED;
enum JobState { JOB_IDLE, JOB_RUNNING, JOB_READY };
JobState s_jobState = JOB_IDLE;
struct WallJob {
  int w, h, blur, dim;
  uint64_t settings;
  uint32_t epoch, requestId;
  char name[15], path[64];
  uint16_t* sharp;
  uint8_t* blurred;
  bool ok, automatic;
};
WallJob s_job = {};

uint64_t settingsKey() {
  return (uint64_t)dispSettings.rotation | (uint64_t)dispSettings.theme << 8 |
         (uint64_t)dispSettings.glassBlur << 16 | (uint64_t)dispSettings.glassDim << 24 |
         (uint64_t)dispSettings.glassTint << 32 | (uint64_t)dispSettings.wallEvery << 40 |
         (uint64_t)dispSettings.wallOrder << 48 | (uint64_t)dispSettings.glassOn << 56;
}

JobState jobState() {
  portENTER_CRITICAL(&s_jobMux);
  const JobState state = s_jobState;
  portEXIT_CRITICAL(&s_jobMux);
  return state;
}

void discardJob() {
  free(s_job.sharp);
  free(s_job.blurred);
  s_job.sharp = nullptr;
  s_job.blurred = nullptr;
  portENTER_CRITICAL(&s_jobMux);
  s_jobState = JOB_IDLE;
  portEXIT_CRITICAL(&s_jobMux);
}

// The worker has an internal-RAM stack: flash reads disable the PSRAM cache.
class WallReader {
 public:
  explicit WallReader(const char* path, bool legacy = false)
      : file_((legacy ? LittleFS : wallFs()).open(path, "r")) {}
  explicit operator bool() const { return (bool)file_; }
  size_t size() { return file_.size(); }
  size_t position() { return file_.position(); }
  size_t read(uint8_t* data, size_t count) { return file_.read(data, count); }
  int read() { return file_.read(); }
  bool seek(size_t position) { return file_.seek(position); }
  void close() { file_.close(); }

 private:
  File file_;
};

struct JpegInput {
  WallReader file;
  uint8_t *r, *g, *b;
  int w, h;
  uint16_t* rgb565 = nullptr;
  JpegInput(const char* path, uint8_t* red, uint8_t* green, uint8_t* blue, int width, int height)
      : file(path), r(red), g(green), b(blue), w(width), h(height) {}
};

size_t jpegRead(JDEC* jd, uint8_t* data, size_t size) {
  JpegInput& in = *(JpegInput*)jd->device;
  if (data) return in.file.read(data, size);
  const size_t pos = in.file.position();
  const size_t count = std::min(size, in.file.size() - pos);
  return in.file.seek(pos + count) ? count : 0;
}

int jpegOutput(JDEC* jd, void* pixels, JRECT* rect) {
  JpegInput& in = *(JpegInput*)jd->device;
  if (rect->right >= in.w || rect->bottom >= in.h) return 0;
  const uint8_t* src = (const uint8_t*)pixels;
  for (int y = rect->top; y <= rect->bottom; y++) {
    for (int x = rect->left; x <= rect->right; x++, src += 3) {
      const int i = y * in.w + x;
      // LVGL's bundled JD_FORMAT 0 emits B, G, R, not upstream RGB order.
      if (in.rgb565) in.rgb565[i] = wallDither565(src[2], src[1], src[0], x, y);
      else {
        in.b[i] = src[0];
        in.g[i] = src[1];
        in.r[i] = src[2];
      }
    }
  }
  return 1;
}

bool jpegLoad(const char* path, int w, int h, uint8_t* r, uint8_t* g, uint8_t* b, uint16_t* rgb565 = nullptr) {
  JpegInput in(path, r, g, b, w, h);
  in.rgb565 = rgb565;
  if (!in.file) return false;
  if (in.file.read() != 0xFF || in.file.read() != 0xD8 || !in.file.seek(0)) return false;
  // FASTDECODE 1 needs up to 4 KB for standard tables, input and MCU buffers.
  constexpr size_t workSize = 4096;
  void* work = heap_caps_malloc(workSize, MALLOC_CAP_SPIRAM);
  if (!work) return false;
  JDEC jd;
  bool ok = jd_prepare(&jd, jpegRead, work, workSize, &in) == JDR_OK &&
            jd.width == w && jd.height == h;
  if (ok && (r || rgb565)) ok = jd_decomp(&jd, jpegOutput, 0) == JDR_OK;
  free(work);
  return ok;
}

// Each gallery keeps its own pin.
const char* pinKey(bool portrait) {
  if (s_source == 2) return portrait ? "wpin_bp" : "wpin_bl";
  return s_source == 1 ? (portrait ? "wpin_dp" : "wpin_dl") : (portrait ? "wpin_p" : "wpin_l");
}

void loadSource() {
  if (s_sourceLoaded) return;
  s_source = dispSettings.wallSource;
  s_sourceLoaded = true;
}

void initGallery(bool portrait) {
  if (s_initialized[portrait]) return;
  loadSource();
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    s_pin[portrait] = prefs.getString(pinKey(portrait), "");
    prefs.end();
  }
  if (!wallValidRef(s_pin[portrait])) s_pin[portrait] = "";
  auto files = wallFiles(portrait);
  for (const auto& f : files)
    if (f.name() == s_pin[portrait]) s_current[portrait] = f.name();
  if (!s_current[portrait].length() && !files.empty())
    s_current[portrait] = files[dispSettings.wallOrder ? esp_random() % files.size() : 0].name();
  s_initialized[portrait] = true;
  if (s_pin[portrait].length() && !wallFs().exists(wallGalleryPath(portrait, s_pin[portrait])))
    wallClearPin(portrait);
}

void fillDsc(lv_image_dsc_t& d, const void* px, int w, int h, lv_color_format_t cf, int bpp) {
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = cf;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * bpp;
  d.data = (const uint8_t*)px;
  d.data_size = (uint32_t)w * h * bpp;
}

uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

// No file yet: a dusk gradient with a warm glow, so Glass works out of the box.
// Written as RGB888 planes (r, g, b), like a loaded file.
void gradient(uint8_t* r, uint8_t* g, uint8_t* b, int w, int h) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float t = (x / (float)w + y / (float)h) / 2;
      const float dx = (x - 0.2f * w) / w, dy = (y - 1.05f * h) / h;
      const float glow = fmaxf(0, 1 - sqrtf(dx * dx + dy * dy) / 0.75f);
      const float g2 = glow * glow;
      const int i = y * w + x;
      r[i] = clamp8(0x24 + (0x16 - 0x24) * t + 0xB0 * g2);
      g[i] = clamp8(0x2C + (0x5E - 0x2C) * t + 0x58 * g2);
      b[i] = clamp8(0x5C + (0x66 - 0x5C) * t + 0x10 * g2);
    }
}

// The file is RGB888 (r, g, b bytes) or, from older portals, RGB565.
bool loadFile(uint8_t* r, uint8_t* g, uint8_t* b, int w, int h) {
  if (!deckFsReady()) return false;
  WallReader f(wallPath(w, h), true);
  if (!f) return false;
  const size_t n = (size_t)w * h, size = f.size();
  bool ok = false;
  if (size == n * 3 || size == n * 2) {
    const int bpp = size == n * 3 ? 3 : 2;
    uint8_t* row = (uint8_t*)heap_caps_malloc(w * bpp, MALLOC_CAP_SPIRAM);
    ok = row != nullptr;
    for (int y = 0; ok && y < h; y++) {
      ok = f.read(row, w * bpp) == (size_t)(w * bpp);
      for (int x = 0; ok && x < w; x++) {
        const int i = y * w + x;
        if (bpp == 3) {
          r[i] = row[x * 3];
          g[i] = row[x * 3 + 1];
          b[i] = row[x * 3 + 2];
        } else {
          const uint16_t p = row[x * 2] | row[x * 2 + 1] << 8;
          r[i] = (p >> 8 & 0xF8) | p >> 13;
          g[i] = (p >> 3 & 0xFC) | (p >> 9 & 0x03);
          b[i] = (p << 3 & 0xF8) | (p >> 2 & 0x07);
        }
      }
    }
    free(row);
  }
  f.close();
  return ok;
}

// One box pass of radius r along rows or columns, edges clamped, rounded.
// Three passes come close to a gaussian.
void boxPass(const uint8_t* src, uint8_t* dst, int w, int h, bool vertical, int r) {
  const int n = 2 * r + 1;
  const int lines = vertical ? w : h, len = vertical ? h : w, step = vertical ? w : 1;
  for (int l = 0; l < lines; l++) {
    const uint8_t* s = src + (vertical ? l : l * w);
    uint8_t* d = dst + (vertical ? l : l * w);
    int sum = 0;
    for (int i = -r; i <= r; i++) sum += s[(i < 0 ? 0 : i >= len ? len - 1 : i) * step];
    for (int i = 0; i < len; i++) {
      d[i * step] = (uint8_t)((sum + n / 2) / n);
      const int add = i + r + 1, sub = i - r;
      sum += s[(add >= len ? len - 1 : add) * step] - s[(sub < 0 ? 0 : sub) * step];
    }
  }
}

// Blur one plane in place (tmp: scratch of the same size).
void blurPlane(uint8_t* p, uint8_t* tmp, int w, int h, int r) {
  if (r <= 0) return;
  for (int pass = 0; pass < 3; pass++) {
    boxPass(p, tmp, w, h, false, r);
    boxPass(tmp, p, w, h, true, r);
  }
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

}  // namespace

void wallBlurPlane(uint8_t* p, uint8_t* tmp, int w, int h, int r) { blurPlane(p, tmp, w, h, r); }

// 8x8 ordered dither: a smooth 8-bit gradient becomes a fine pattern of
// neighbouring RGB565 levels instead of coloured steps.
uint16_t wallDither565(int r, int g, int b, int x, int y) {
  static const uint8_t kBayer[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
      {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
  const int t = (kBayer[y & 7][x & 7] * 2 + 1) * 255;  // threshold, in 1/32640 of a level
  int r5 = (r * 31 * 128 + t) / 32640, g6 = (g * 63 * 128 + t) / 32640, b5 = (b * 31 * 128 + t) / 32640;
  if (r5 > 31) r5 = 31;
  if (g6 > 63) g6 = 63;
  if (b5 > 31) b5 = 31;
  return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

const char* wallPath(int w, int h) { return w > h ? "/wall_l.bin" : "/wall_p.bin"; }

namespace {

bool prepareImage(WallJob& job) {
  const int w = job.w, h = job.h;
  const size_t n = (size_t)w * h;
  job.sharp = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  job.blurred = (uint8_t*)heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM);
  uint8_t* planes = (uint8_t*)heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);
  if (!job.sharp || !job.blurred || !planes) {
    Serial.println("Deck: wallpaper PSRAM allocation failed");
    free(planes);
    return false;
  }
  uint8_t *r = planes, *g = planes + n, *b = planes + 2 * n, *tmp = planes + 3 * n;
  uint32_t start = millis();
  const bool jpeg = job.name[0];
  const bool loaded = jpeg ? jpegLoad(job.path, w, h, r, g, b)
                           : loadFile(r, g, b, w, h);
  if (jpeg && !loaded) {
    Serial.printf("Deck: wallpaper decode failed: %s\n", job.name);
    free(planes);
    return false;
  }
  if (!loaded) gradient(r, g, b, w, h);
  const uint32_t decodeMs = millis() - start;
  start = millis();
  const int keep = (100 - job.dim) * 256 / 100;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      job.sharp[i] = wallDither565(r[i] * keep >> 8, g[i] * keep >> 8, b[i] * keep >> 8, x, y);
    }
  uint8_t* ch[3] = {b, g, r};
  for (int c = 0; c < 3; c++) {
    blurPlane(ch[c], tmp, w, h, job.blur);
    for (size_t i = 0; i < n; i++) job.blurred[i * 3 + c] = ch[c][i];
  }
  free(planes);
  Serial.printf("Deck: wallpaper %s decode %lu ms, dim+blur %lu ms\n",
                jpeg ? job.name : loaded ? "legacy" : "gradient",
                (unsigned long)decodeMs, (unsigned long)(millis() - start));
  return true;
}

// One task per job, so its internal stack is only held while a picture loads.
void wallTask(void*) {
  portENTER_CRITICAL(&s_jobMux);
  WallJob job = s_job;
  portEXIT_CRITICAL(&s_jobMux);
  job.ok = prepareImage(job);
  portENTER_CRITICAL(&s_jobMux);
  s_job = job;
  s_jobState = JOB_READY;
  portEXIT_CRITICAL(&s_jobMux);
  vTaskDelete(nullptr);
}

void installImage(WallJob& job) {
  lv_image_cache_drop(&s_sharpDsc);
  lv_image_cache_drop(&s_blurDsc);
  free(s_sharp);
  free(s_blur);
  s_sharp = job.sharp;
  s_blur = job.blurred;
  job.sharp = nullptr;
  job.blurred = nullptr;
  s_w = job.w;
  s_h = job.h;
  s_blurR = job.blur;
  s_dim = job.dim;
  s_loadedName = job.name;
  s_current[job.w < job.h] = job.name;
  wallProtect(job.w < job.h, 0, wallSource() == 1 && job.name[0] ? "d/" + String(job.name) : String(job.name));
  wallProtect(job.w < job.h, 1, "");
  s_changedMs = millis();
  ++s_generation;
  fillDsc(s_sharpDsc, s_sharp, s_w, s_h, LV_COLOR_FORMAT_RGB565, 2);
  fillDsc(s_blurDsc, s_blur, s_w, s_h, LV_COLOR_FORMAT_RGB888, 3);
}

}  // namespace

bool wallLoad(int w, int h, int blurR, int dimPct) {
  const bool p = w < h;
  initGallery(p);
  if (s_request[p]) {
    s_current[p] = s_requested[p];
    s_request[p] = false;
  }
  if (s_sharp && s_w == w && s_h == h && s_blurR == blurR && s_dim == dimPct &&
      s_loadedName == s_current[p]) return true;
  wallInvalidateJobs();
  // Settings rebuilds are synchronous; avoid a second pending PSRAM allocation.
  while (jobState() == JOB_RUNNING) delay(1);
  if (jobState() == JOB_READY) discardJob();
  wallFree();
  WallJob job = {};
  job.w = w;
  job.h = h;
  job.blur = blurR;
  job.dim = dimPct;
  strlcpy(job.name, s_current[p].c_str(), sizeof(job.name));
  strlcpy(job.path, wallGalleryPath(p, job.name).c_str(), sizeof(job.path));
  wallProtect(p, 1, s_source == 1 && job.name[0] ? "d/" + String(job.name) : String(job.name));
  if (!prepareImage(job)) {
    free(job.sharp);
    free(job.blurred);
    return false;
  }
  installImage(job);
  return true;
}

void wallFree() {
  wallInvalidateJobs();
  lv_image_cache_drop(&s_sharpDsc);
  lv_image_cache_drop(&s_blurDsc);
  s_generation++;  // also covers reloads that reuse the same allocation addresses
  free(s_sharp);
  free(s_blur);
  s_sharp = nullptr;
  s_blur = nullptr;
  wallProtect(false, 0, "");
  wallProtect(true, 0, "");
  s_w = s_h = 0;
  s_blurR = s_dim = -1;
}

const lv_image_dsc_t* wallSharp() { return s_sharp ? &s_sharpDsc : nullptr; }
const lv_image_dsc_t* wallBlur() { return s_blur ? &s_blurDsc : nullptr; }
uint32_t wallGeneration() { return s_generation; }

bool wallValidName(const String& name) {
  if (name.length() != 12 || !name.endsWith(".jpg")) return false;
  for (int i = 0; i < 8; i++)
    if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
  return true;
}

bool wallValidRef(const String& name) {
  return wallValidName(name) || (name.length() == 14 && name[1] == '/' &&
         (name[0] == 'm' || name[0] == 'd') && wallValidName(name.substring(2)));
}

uint64_t wallId(const String& name) {
  const bool qualified = name.length() == 14;
  return strtoul(name.c_str() + (qualified ? 2 : 0), nullptr, 16) |
         (uint64_t)(qualified && name[0] == 'd') << 32;
}

String WallFile::name() const {
  char text[15];
  if (qualified) snprintf(text, sizeof(text), "%c/%08lx.jpg", id >> 32 ? 'd' : 'm', (unsigned long)(uint32_t)id);
  else snprintf(text, sizeof(text), "%08lx.jpg", (unsigned long)(uint32_t)id);
  return text;
}

uint8_t wallSource() { loadSource(); return s_source; }
bool wallDaily() { return wallSource() != 0; }

void wallSetSource(uint8_t source) {
  loadSource();
  if (s_source == source) return;
  s_source = source;
  wallInvalidateJobs();
  for (int p = 0; p < 2; p++) {
    s_initialized[p] = false;
    s_current[p] = "";
    s_shown[p].clear();
    initGallery(p);
    wallRequest(p, s_current[p]);
  }
}

String wallGalleryPath(bool portrait, const String& name) {
  const bool qualified = name.length() == 14;
  const bool daily = qualified ? name[0] == 'd' : wallSource() == 1;
  return String(wallDir(portrait, daily)) + "/" + (qualified ? name.substring(2) : name);
}

WallFiles wallFiles(bool portrait) {
  auto files = wallListDir(wallDir(portrait, wallSource() == 1));
  if (wallSource() == 1)
    for (auto& f : files) f.id = (uint32_t)f.id;
  if (wallSource() == 2) {
    auto daily = wallListDir(wallDir(portrait, true));
    for (auto& f : daily) f.id |= (uint64_t)1 << 32;
    files.insert(files.end(), daily.begin(), daily.end());
    for (auto& f : files) f.qualified = true;
    std::sort(files.begin(), files.end(), [](const WallFile& a, const WallFile& b) {
      return (a.id >> 32) != (b.id >> 32) ? (a.id >> 32) > (b.id >> 32) : a.id < b.id;
    });
  }
  return files;
}

String wallCurrent(bool portrait) { initGallery(portrait); return s_current[portrait]; }
String wallPinned(bool portrait) { initGallery(portrait); return s_pin[portrait]; }

bool wallPin(bool portrait, const String& name) {
  if (!wallValidRef(name)) return false;
  initGallery(portrait);
  const String ref = s_source == 1 ? "d/" + name : name;
  // Protect before checking existence, so eviction cannot race the NVS write.
  wallProtect(portrait, 7, ref);
  bool ok = false;
  Preferences prefs;
  if (wallFs().exists(wallGalleryPath(portrait, name)) && prefs.begin(NVS_NAMESPACE, false)) {
    ok = prefs.putString(pinKey(portrait), name) > 0;
    prefs.end();
  }
  if (ok) {
    s_pin[portrait] = name;
    wallProtect(portrait, 4 + s_source, ref);
  }
  wallProtect(portrait, 7, "");
  return ok;
}

void wallClearPin(bool portrait) {
  initGallery(portrait);
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.remove(pinKey(portrait));
    prefs.end();
  }
  s_pin[portrait] = "";
  wallProtect(portrait, 4 + s_source, "");
}

bool wallLoadPicture(const char* path, int w, int h, uint16_t* pixels) {
  return pixels && jpegLoad(path, w, h, nullptr, nullptr, nullptr, pixels);
}

bool wallValidateJpeg(const char* path, bool portrait) {
  return jpegLoad(path, portrait ? 320 : 480, portrait ? 480 : 320, nullptr, nullptr, nullptr);
}

void wallInvalidateJobs() {
  ++s_epoch;
  for (int p = 0; p < 2; p++)
    if (s_automatic[p]) s_request[p] = false;
}

void wallRequest(bool portrait, const String& name) {
  if (name.length() && !wallValidRef(name)) return;
  initGallery(portrait);
  ++s_requestId[portrait];
  s_automatic[portrait] = false;
  s_requested[portrait] = name;
  wallProtect(portrait, 1, s_source == 1 && name.length() ? "d/" + name : name);
  s_request[portrait] = true;
  s_retryMs = 0;
  if (portrait != ((dispSettings.rotation & 1) == 0) || dispSettings.theme != THEME_GLASS)
    s_current[portrait] = name;
}

bool wallNext(bool portrait) {
  initGallery(portrait);
  auto files = wallFiles(portrait);
  if (files.empty()) return false;
  size_t index = 0;
  if (dispSettings.wallOrder && files.size() > 1) {
    // Shuffle: no picture again until every one was shown, and a new round
    // never starts with the one on screen.
    WallVector<uint64_t>& shown = s_shown[portrait];
    const String& cur = s_current[portrait];
    if (cur.length() && std::find(shown.begin(), shown.end(), wallId(cur)) == shown.end())
      shown.push_back(wallId(cur));
    WallVector<size_t> choices;
    for (size_t i = 0; i < files.size(); i++)
      if (std::find(shown.begin(), shown.end(), files[i].id) == shown.end()) choices.push_back(i);
    if (choices.empty()) {
      shown.assign(1, wallId(cur));
      for (size_t i = 0; i < files.size(); i++)
        if (files[i].name() != cur) choices.push_back(i);
    }
    index = choices[esp_random() % choices.size()];
    shown.push_back(files[index].id);
  } else {
    for (size_t i = 0; i < files.size(); i++)
      if (files[i].name() > s_current[portrait]) { index = i; break; }
  }
  wallRequest(portrait, files[index].name());
  return true;
}

void wallGalleryChanged(bool portrait) {
  initGallery(portrait);
  portENTER_CRITICAL(&s_jobMux);
  const WallJob job = s_job;
  const JobState state = s_jobState;
  portEXIT_CRITICAL(&s_jobMux);
  if (state != JOB_IDLE && (job.w < job.h) == portrait && job.name[0] &&
      !wallFs().exists(wallGalleryPath(portrait, job.name))) ++s_requestId[portrait];
  auto files = wallFiles(portrait);
  const String current = s_current[portrait];
  bool found = false, pinned = false;
  for (const auto& f : files) {
    found |= f.name() == current;
    pinned |= f.name() == s_pin[portrait];
  }
  if (!pinned && s_pin[portrait].length()) wallClearPin(portrait);
  if (s_request[portrait] && s_requested[portrait].length() &&
      !wallFs().exists(wallGalleryPath(portrait, s_requested[portrait]))) s_request[portrait] = false;
  if (found || s_request[portrait]) return;
  String next;
  if (!files.empty()) {
    next = files.front().name();
    for (const auto& f : files)
      if (f.name() > current) { next = f.name(); break; }
  }
  wallRequest(portrait, next);
}

bool wallPoll(int w, int h, bool idle, bool canSwitch) {
  const bool p = w < h;
  const uint32_t now = millis();
  if (jobState() == JOB_READY) {
    if (s_job.epoch != s_epoch || s_job.requestId != s_requestId[p] || s_job.settings != settingsKey() ||
        s_job.w != w || s_job.h != h || !s_job.ok) {
      if (!s_job.ok) s_retryMs = now;
      discardJob();
    } else {
      if (!canSwitch || (s_job.automatic && !idle)) return false;
      if (!s_job.name[0] || wallFs().exists(wallGalleryPath(p, s_job.name))) return true;
      discardJob();
    }
  }
  if (jobState() != JOB_IDLE || dispSettings.theme != THEME_GLASS || !canSwitch) return false;
  if (s_retryMs && now - s_retryMs < 30000) return false;
  initGallery(p);
  if (s_request[p] && s_automatic[p] && !idle) return false;
  if (!s_request[p]) {
    if (!idle || !dispSettings.wallEvery || now - s_changedMs < (uint32_t)dispSettings.wallEvery * 60000)
      return false;
    if (now - s_scanMs < 1000) return false;
    s_scanMs = now;
    if (wallFiles(p).size() < 2) return false;
    wallNext(p);
    s_automatic[p] = true;
  }
  WallJob job = {};
  job.w = w;
  job.h = h;
  job.blur = dispSettings.glassBlur;
  job.dim = dispSettings.glassDim;
  job.settings = settingsKey();
  job.epoch = s_epoch;
  job.requestId = s_requestId[p];
  job.automatic = s_automatic[p];
  strlcpy(job.name, s_requested[p].c_str(), sizeof(job.name));
  strlcpy(job.path, wallGalleryPath(p, job.name).c_str(), sizeof(job.path));
  portENTER_CRITICAL(&s_jobMux);
  s_job = job;
  s_jobState = JOB_RUNNING;
  portEXIT_CRITICAL(&s_jobMux);
  if (xTaskCreatePinnedToCore(wallTask, "deck_wall", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    portENTER_CRITICAL(&s_jobMux);
    s_jobState = JOB_IDLE;
    portEXIT_CRITICAL(&s_jobMux);
    s_retryMs = now;
    return false;
  }
  s_request[p] = false;
  return false;
}

void wallCommit() {
  if (jobState() != JOB_READY) return;
  const uint32_t start = millis();
  installImage(s_job);
  discardJob();
  Serial.printf("Deck: wallpaper swap %lu ms\n", (unsigned long)(millis() - start));
}

}  // namespace deck
