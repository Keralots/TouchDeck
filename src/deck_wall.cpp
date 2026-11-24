// Guition deck: Glass theme wallpaper. See deck_wall.h.
#include "deck_wall.h"
#include <src/misc/cache/instance/lv_image_cache.h>
#include "deck_art.h"
#include "deck_config.h"
#include "deck_panel.h"
#include "deck_weather.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <time.h>
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
  bool redraw;  // the picture on screen again, in its new light: into the loaded buffers
  ArtTime art;
};
WallJob s_job = {};
ArtTime s_loadedArt = {};
volatile uint32_t s_artPreview = 0;

uint64_t settingsKey() {
  return (uint64_t)dispSettings.rotation | (uint64_t)dispSettings.theme << 8 |
         (uint64_t)dispSettings.wallFollow << 15 |
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

// LittleFS reads disable the PSRAM cache, so they need an internal stack (wallRunWorker).
class WallReader {
 public:
  explicit WallReader(const char* path, bool legacy = false)
      : file_((legacy ? LittleFS : wallFs()).open(path, "r")) {
    // Over 4 KB, so malloc puts the stdio buffer in PSRAM instead of internal RAM.
    if (file_) file_.setBufferSize(8192);
  }
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
  int step = 1;  // bytes between two pixels of one channel
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
        in.b[i * in.step] = src[0];
        in.g[i * in.step] = src[1];
        in.r[i * in.step] = src[2];
      }
    }
  }
  return 1;
}

int artIndex(const char* ref) {
  if (strncmp(ref, "b/", 2) || strlen(ref) != 14) return -1;
  const unsigned long i = strtoul(ref + 2, nullptr, 16);
  return i < ART_COUNT ? (int)i : -1;
}

// The light to draw this picture in; {} = as designed, or not a built-in one.
ArtTime artTime(const char* ref) {
  ArtTime at = {};
  const uint32_t now = s_artPreview ? s_artPreview : (uint32_t)time(nullptr);
  if (!dispSettings.wallFollow || now < 1609459200 || !artFollows(artIndex(ref))) return at;
  at.utc = now;
  at.place = weatherPlace(at.lat, at.lon);
  if (!at.place) at.lat = at.lon = 0;
  at.utc = artStep(at);
  return at;
}

struct ArtOutput {
  uint8_t *r, *g, *b;
  uint16_t* rgb565;
  int w, step;
};

void artRow(void* ctx, int y, const uint8_t* src) {
  const ArtOutput& o = *(ArtOutput*)ctx;
  for (int x = 0; x < o.w; x++, src += 3) {
    const int i = y * o.w + x;
    if (o.rgb565) o.rgb565[i] = wallDither565(src[0], src[1], src[2], x, y);
    else {
      o.r[i * o.step] = src[0];
      o.g[i * o.step] = src[1];
      o.b[i * o.step] = src[2];
    }
  }
}

// A file, or a built-in picture ("b/...") drawn at this size.
bool jpegLoad(const char* path, int w, int h, uint8_t* r, uint8_t* g, uint8_t* b, uint16_t* rgb565 = nullptr,
              int step = 1, const ArtTime& at = {}) {
  const int art = artIndex(path);
  if (art >= 0) {
    ArtOutput out = {r, g, b, rgb565, w, step};
    return artDraw(art, w, h, at, artRow, &out);
  }
  JpegInput in(path, r, g, b, w, h);
  in.rgb565 = rgb565;
  in.step = step;
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
  if (s_source == 3) return portrait ? "wpin_rp" : "wpin_rl";
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
  if (s_pin[portrait].length() && !wallExists(portrait, s_pin[portrait]))
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

// Daily names carry no prefix in their own source.
String sourceRef(const String& name) { return s_source == 1 && wallValidName(name) ? "d/" + name : name; }

uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

// No file yet: a dusk gradient with a warm glow, so Glass works out of the box.
// Written as RGB888 channels (r, g, b; step bytes apart), like a loaded file.
void gradient(uint8_t* r, uint8_t* g, uint8_t* b, int w, int h, int step) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float t = (x / (float)w + y / (float)h) / 2;
      const float dx = (x - 0.2f * w) / w, dy = (y - 1.05f * h) / h;
      const float glow = fmaxf(0, 1 - sqrtf(dx * dx + dy * dy) / 0.75f);
      const float g2 = glow * glow;
      const int i = (y * w + x) * step;
      r[i] = clamp8(0x24 + (0x16 - 0x24) * t + 0xB0 * g2);
      g[i] = clamp8(0x2C + (0x5E - 0x2C) * t + 0x58 * g2);
      b[i] = clamp8(0x5C + (0x66 - 0x5C) * t + 0x10 * g2);
    }
}

// The file is RGB888 (r, g, b bytes) or, from older portals, RGB565.
bool loadFile(uint8_t* r, uint8_t* g, uint8_t* b, int w, int h, int step) {
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
        const int i = (y * w + x) * step;
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
  const uint32_t inv = 0xFFFFFFFFu / n + 1;  // exact x / n as (x * inv) >> 32 for these sums
  auto avg = [inv](uint32_t s) { return (uint8_t)(((uint64_t)s * inv) >> 32); };
  if (!vertical) {
    for (int l = 0; l < h; l++) {
      const uint8_t* s = src + l * w;
      uint8_t* d = dst + l * w;
      uint32_t sum = n / 2;
      for (int i = -r; i <= r; i++) sum += s[i < 0 ? 0 : i >= w ? w - 1 : i];
      for (int i = 0; i < w; i++) {
        d[i] = avg(sum);
        const int add = i + r + 1, sub = i - r;
        sum += s[add >= w ? w - 1 : add] - s[sub < 0 ? 0 : sub];
      }
    }
    return;
  }
  // Columns in strips walked row by row: PSRAM reads stay in cache lines.
  constexpr int STRIP = 32;
  uint32_t sum[STRIP];
  for (int x0 = 0; x0 < w; x0 += STRIP) {
    const int cols = std::min(STRIP, w - x0);
    for (int c = 0; c < cols; c++) sum[c] = n / 2;
    for (int i = -r; i <= r; i++) {
      const uint8_t* s = src + (i < 0 ? 0 : i >= h ? h - 1 : i) * w + x0;
      for (int c = 0; c < cols; c++) sum[c] += s[c];
    }
    for (int i = 0; i < h; i++) {
      uint8_t* d = dst + i * w + x0;
      const int add = i + r + 1, sub = i - r;
      const uint8_t* a = src + (add >= h ? h - 1 : add) * w + x0;
      const uint8_t* b = src + (sub < 0 ? 0 : sub) * w + x0;
      for (int c = 0; c < cols; c++) {
        d[c] = avg(sum[c]);
        sum[c] += a[c] - b[c];
      }
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

// Blur one channel of an interleaved picture in place (step bytes apart),
// same result as blurPlane. scratch: max(w, 32 * h) bytes.
void blurChannel(uint8_t* p, int step, int w, int h, int r, uint8_t* scratch) {
  if (r <= 0) return;
  const int n = 2 * r + 1;
  const uint32_t inv = 0xFFFFFFFFu / n + 1;
  auto avg = [inv](uint32_t s) { return (uint8_t)(((uint64_t)s * inv) >> 32); };
  constexpr int STRIP = 32;
  uint32_t sum[STRIP];
  for (int pass = 0; pass < 3; pass++) {
    for (int l = 0; l < h; l++) {
      uint8_t* s = p + (size_t)l * w * step;
      uint32_t rowSum = n / 2;
      for (int i = -r; i <= r; i++) rowSum += s[(i < 0 ? 0 : i >= w ? w - 1 : i) * step];
      for (int i = 0; i < w; i++) {
        scratch[i] = avg(rowSum);
        const int add = i + r + 1, sub = i - r;
        rowSum += s[(add >= w ? w - 1 : add) * step] - s[(sub < 0 ? 0 : sub) * step];
      }
      for (int i = 0; i < w; i++) s[i * step] = scratch[i];
    }
    for (int x0 = 0; x0 < w; x0 += STRIP) {
      const int cols = std::min(STRIP, w - x0);
      auto row = [&](int y) { return p + ((size_t)(y < 0 ? 0 : y >= h ? h - 1 : y) * w + x0) * step; };
      for (int c = 0; c < cols; c++) sum[c] = n / 2;
      for (int i = -r; i <= r; i++) {
        const uint8_t* s = row(i);
        for (int c = 0; c < cols; c++) sum[c] += s[c * step];
      }
      for (int i = 0; i < h; i++) {
        uint8_t* d = scratch + i * cols;
        const uint8_t *a = row(i + r + 1), *b = row(i - r);
        for (int c = 0; c < cols; c++) {
          d[c] = avg(sum[c]);
          sum[c] += a[c * step] - b[c * step];
        }
      }
      for (int i = 0; i < h; i++) {
        uint8_t* d = row(i);
        for (int c = 0; c < cols; c++) d[c * step] = scratch[i * cols + c];
      }
    }
  }
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

// blurChannel for a picture that comes as rows of 3 channels, top to bottom:
// keeps only the rows under the box instead of the whole picture.
struct RowBlur {
  int w3, r, n;
  void (*out)(void* ctx, int y, const uint8_t* row);
  void* ctx;
  int fed[3] = {};
  uint8_t *ring[3] = {}, *line = nullptr;  // line: one output row per pass, then two for the rows
  uint32_t *sum = nullptr, inv;

  RowBlur(int w, int radius, void (*o)(void*, int, const uint8_t*), void* c)
      : w3(w * 3), r(radius), n(2 * radius + 1), out(o), ctx(c), inv(0xFFFFFFFFu / n + 1) {}
  RowBlur(const RowBlur&) = delete;
  ~RowBlur() {
    for (uint8_t* p : ring) heap_caps_free(p);
    heap_caps_free(line);
    heap_caps_free(sum);
  }
  bool begin() {
    bool ok = true;
    for (uint8_t*& p : ring) ok &= (p = (uint8_t*)heap_caps_malloc(n * w3, MALLOC_CAP_SPIRAM)) != nullptr;
    line = (uint8_t*)heap_caps_malloc(5 * w3, MALLOC_CAP_SPIRAM);
    sum = (uint32_t*)heap_caps_malloc(3 * w3 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    return ok && line && sum;
  }
  uint8_t avg(uint32_t s) const { return (uint8_t)(((uint64_t)s * inv) >> 32); }

  void across(const uint8_t* s, uint8_t* d) const {
    for (int c = 0; c < 3; c++) {
      const int last = w3 - 3 + c;
      uint32_t acc = n / 2 + (r + 1) * s[c];
      for (int i = 1; i <= r; i++) acc += s[std::min(c + i * 3, last)];
      for (int i = c; i <= last; i += 3) {
        d[i] = avg(acc);
        acc += s[std::min(i + (r + 1) * 3, last)] - s[std::max(i - r * 3, c)];
      }
    }
  }
  // One row into a vertical pass; its output row goes into the next one.
  void down(int pass, const uint8_t* row) {
    uint32_t* acc = sum + pass * w3;
    const int k = fed[pass]++;
    if (k == 0) {
      for (int i = 0; i < w3; i++) acc[i] = n / 2 + (r + 1) * row[i];
    } else if (k <= r) {
      for (int i = 0; i < w3; i++) acc[i] += row[i];
    } else {
      const uint8_t* old = ring[pass] + (k <= n ? 0 : k % n) * w3;
      for (int i = 0; i < w3; i++) acc[i] += row[i] - old[i];
    }
    memcpy(ring[pass] + (k % n) * w3, row, w3);
    if (k < r) return;
    uint8_t* d = line + pass * w3;
    for (int i = 0; i < w3; i++) d[i] = avg(acc[i]);
    if (pass < 2) down(pass + 1, d);
    else out(ctx, k - r, d);
  }
  void row(const uint8_t* src) {
    uint8_t *a = line + 3 * w3, *b = a + w3;
    across(src, a);
    across(a, b);
    across(b, a);
    down(0, a);
  }
  // The rows still under the boxes: the last one repeats past the edge.
  void end() {
    uint8_t* last = line + 3 * w3;
    for (int pass = 0; pass < 3; pass++) {
      memcpy(last, ring[pass] + ((fed[pass] - 1) % n) * w3, w3);
      for (int i = 0; i < r; i++) down(pass, last);
    }
  }
};

}  // namespace

void wallBlurPlane(uint8_t* p, uint8_t* tmp, int w, int h, int r) { blurPlane(p, tmp, w, h, r); }

const char* wallPath(int w, int h) { return w > h ? "/wall_l.bin" : "/wall_p.bin"; }

namespace {

bool prepareImage(WallJob& job) {
  const int w = job.w, h = job.h;
  const size_t n = (size_t)w * h;
  // Decoded straight into the blurred picture (b, g, r bytes) and blurred
  // there: a new wallpaper takes what the old one frees, PSRAM has no more.
  job.sharp = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  job.blurred = (uint8_t*)heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM);
  uint8_t* tmp = (uint8_t*)heap_caps_malloc(std::max(w, 32 * h), MALLOC_CAP_SPIRAM);
  if (!job.sharp || !job.blurred || !tmp) {
    Serial.println("Deck: wallpaper PSRAM allocation failed");
    free(tmp);
    return false;
  }
  uint8_t *b = job.blurred, *g = job.blurred + 1, *r = job.blurred + 2;
  uint32_t start = millis();
  const bool jpeg = job.name[0];
  const bool loaded = jpeg ? jpegLoad(job.path, w, h, r, g, b, nullptr, 3, job.art)
                           : loadFile(r, g, b, w, h, 3);
  if (jpeg && !loaded) {
    Serial.printf("Deck: wallpaper decode failed: %s\n", job.name);
    free(tmp);
    return false;
  }
  if (!loaded) gradient(r, g, b, w, h, 3);
  const uint32_t decodeMs = millis() - start;
  start = millis();
  const int keep = (100 - job.dim) * 256 / 100;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const int i = (y * w + x) * 3;
      job.sharp[y * w + x] = wallDither565(r[i] * keep >> 8, g[i] * keep >> 8, b[i] * keep >> 8, x, y);
    }
  for (int c = 0; c < 3; c++) blurChannel(job.blurred + c, 3, w, h, job.blur, tmp);
  free(tmp);
  Serial.printf("Deck: wallpaper %s decode %lu ms, dim+blur %lu ms\n",
                jpeg ? job.name : loaded ? "legacy" : "gradient",
                (unsigned long)decodeMs, (unsigned long)(millis() - start));
  return true;
}

// The loaded picture in new light. Row by row into its own buffers: printer pages
// leave no PSRAM for a second picture. Nothing is written when memory is short.
bool redrawImage(const WallJob& job) {
  struct Output {
    int w, keep;
    RowBlur blur;
  };
  Output out = {job.w, (100 - job.dim) * 256 / 100,
                RowBlur(job.w, job.blur, [](void*, int y, const uint8_t* row) {
                  uint8_t* d = s_blur + (size_t)y * s_w * 3;
                  for (int i = 0; i < s_w * 3; i += 3) {
                    d[i] = row[i + 2];
                    d[i + 1] = row[i + 1];
                    d[i + 2] = row[i];
                  }
                }, nullptr)};
  const uint32_t start = millis();
  if (!out.blur.begin()) {
    Serial.println("Deck: wallpaper PSRAM allocation failed");
    return false;
  }
  const bool ok = artDraw(artIndex(job.name), job.w, job.h, job.art, [](void* ctx, int y, const uint8_t* row) {
    Output& o = *(Output*)ctx;
    uint16_t* d = s_sharp + (size_t)y * o.w;
    for (int x = 0; x < o.w; x++)
      d[x] = wallDither565(row[x * 3] * o.keep >> 8, row[x * 3 + 1] * o.keep >> 8, row[x * 3 + 2] * o.keep >> 8, x, y);
    o.blur.row(row);
  }, &out);
  if (ok) out.blur.end();
  Serial.printf("Deck: wallpaper redraw %lu ms\n", (unsigned long)(millis() - start));
  return ok;
}

void wallJob() {
  portENTER_CRITICAL(&s_jobMux);
  WallJob job = s_job;
  portEXIT_CRITICAL(&s_jobMux);
  job.ok = job.redraw ? redrawImage(job) : prepareImage(job);
  portENTER_CRITICAL(&s_jobMux);
  s_job = job;
  s_jobState = JOB_READY;
  portEXIT_CRITICAL(&s_jobMux);
}

QueueHandle_t s_sdJobs = nullptr;

void sdWorker(void*) {
  void (*job)();
  for (;;)
    if (xQueueReceive(s_sdJobs, &job, portMAX_DELAY) == pdTRUE) job();
}

void flashWorker(void* job) {
  ((void (*)())job)();
  vTaskDelete(nullptr);
}

}  // namespace

bool wallRunWorker(void (*job)(), bool onSd, const char* name) {
  if (!onSd) return xTaskCreatePinnedToCore(flashWorker, name, 6144, (void*)job, 1, nullptr, 0) == pdPASS;
  if (!s_sdJobs) {
    s_sdJobs = xQueueCreate(4, sizeof(job));
    if (!s_sdJobs) return false;
    if (xTaskCreatePinnedToCoreWithCaps(sdWorker, "deck_pics", 6144, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
      vQueueDelete(s_sdJobs);
      s_sdJobs = nullptr;
      return false;
    }
  }
  return xQueueSend(s_sdJobs, &job, 0) == pdTRUE;
}

namespace {

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
  wallProtect(job.w < job.h, 0, sourceRef(job.name));
  wallProtect(job.w < job.h, 1, "");
  s_loadedArt = job.art;
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
  job.art = artTime(job.name);
  wallProtect(p, 1, sourceRef(job.name));
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
  if (jobState() != JOB_IDLE && s_job.redraw) {  // it draws into the buffers freed here
    while (jobState() == JOB_RUNNING) delay(1);
    discardJob();
  }
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
         (name[0] == 'm' || name[0] == 'd') && wallValidName(name.substring(2))) ||
         wallArtIndex(name) >= 0;
}

int wallArtIndex(const String& name) { return artIndex(name.c_str()); }

bool wallExists(bool portrait, const String& name) {
  return wallArtIndex(name) >= 0 || wallFs().exists(wallGalleryPath(portrait, name));
}

uint64_t wallId(const String& name) {
  const bool qualified = name.length() == 14;
  return strtoul(name.c_str() + (qualified ? 2 : 0), nullptr, 16) |
         (uint64_t)(!qualified ? 0 : name[0] == 'd' ? 1 : name[0] == 'b' ? 2 : 0) << 32;
}

String WallFile::name() const {
  char text[15];
  if (qualified) snprintf(text, sizeof(text), "%c/%08lx.jpg", "mdb"[(id >> 32) % 3], (unsigned long)(uint32_t)id);
  else snprintf(text, sizeof(text), "%08lx.jpg", (unsigned long)(uint32_t)id);
  return text;
}

uint8_t wallSource() { loadSource(); return s_source; }
bool wallDaily() { return wallSource() == 1 || wallSource() == 2; }

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
  if (wallArtIndex(name) >= 0) return name;
  const bool qualified = name.length() == 14;
  const bool daily = qualified ? name[0] == 'd' : wallSource() == 1;
  return String(wallDir(portrait, daily)) + "/" + (qualified ? name.substring(2) : name);
}

WallFiles wallArtFiles() {
  WallFiles files;
  for (int i = 0; i < ART_COUNT; i++) files.push_back({(uint64_t)2 << 32 | (uint32_t)i, 0, 0, true});
  return files;
}

WallFiles wallFiles(bool portrait) {
  if (wallSource() == 3) return wallArtFiles();
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
  // Nothing yet: the built-in ones, unless an old single picture is in use.
  if (files.empty() && !(deckFsReady() && LittleFS.exists(wallPath(screenW(portrait), screenH(portrait)))))
    return wallArtFiles();
  return files;
}

String wallCurrent(bool portrait) { initGallery(portrait); return s_current[portrait]; }
String wallPinned(bool portrait) { initGallery(portrait); return s_pin[portrait]; }

bool wallPin(bool portrait, const String& name) {
  if (!wallValidRef(name)) return false;
  initGallery(portrait);
  const String ref = sourceRef(name);
  // Protect before checking existence, so eviction cannot race the NVS write.
  wallProtect(portrait, 7, ref);
  bool ok = false;
  Preferences prefs;
  if (wallExists(portrait, name) && prefs.begin(NVS_NAMESPACE, false)) {
    ok = prefs.putString(pinKey(portrait), name) > 0;
    prefs.end();
  }
  if (ok) {
    s_pin[portrait] = name;
    if (s_source < 3) wallProtect(portrait, 4 + s_source, ref);
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
  if (s_source < 3) wallProtect(portrait, 4 + s_source, "");
}

bool wallLoadPicture(const char* path, int w, int h, uint16_t* pixels) {
  return pixels && jpegLoad(path, w, h, nullptr, nullptr, nullptr, pixels, 1, artTime(path));
}

bool wallValidateJpeg(const char* path, bool portrait) {
  return jpegLoad(path, screenW(portrait), screenH(portrait), nullptr, nullptr, nullptr);
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
  wallProtect(portrait, 1, sourceRef(name));
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
      !wallExists(portrait, job.name)) ++s_requestId[portrait];
  auto files = wallFiles(portrait);
  const String current = s_current[portrait];
  bool found = false, pinned = false;
  for (const auto& f : files) {
    found |= f.name() == current;
    pinned |= f.name() == s_pin[portrait];
  }
  if (!pinned && s_pin[portrait].length()) wallClearPin(portrait);
  if (s_request[portrait] && s_requested[portrait].length() &&
      !wallExists(portrait, s_requested[portrait])) s_request[portrait] = false;
  if (found || s_request[portrait]) return;
  String next;
  if (!files.empty()) {
    next = files.front().name();
    for (const auto& f : files)
      if (f.name() > current) { next = f.name(); break; }
  }
  wallRequest(portrait, next);
}

bool wallPoll(int w, int h, bool idle, bool canSwitch, bool rotate, void (*makeRoom)()) {
  const bool p = w < h;
  const uint32_t now = millis();
  if (jobState() == JOB_READY && s_job.redraw) {
    // The buffers hold the new light already, whatever changed meanwhile.
    if (s_job.ok) return canSwitch && (idle || !s_job.automatic || s_request[p]);
    s_retryMs = now;
    discardJob();
  }
  if (jobState() == JOB_READY) {
    if (s_job.epoch != s_epoch || s_job.requestId != s_requestId[p] || s_job.settings != settingsKey() ||
        s_job.w != w || s_job.h != h || !s_job.ok || (s_job.automatic && !rotate)) {
      if (!s_job.ok) s_retryMs = now;
      discardJob();
    } else {
      if (!canSwitch || (s_job.automatic && !idle)) return false;
      if (!s_job.name[0] || wallExists(p, s_job.name)) return true;
      discardJob();
    }
  }
  if (jobState() != JOB_IDLE || dispSettings.theme != THEME_GLASS || !canSwitch) return false;
  if (s_retryMs && now - s_retryMs < 30000) return false;
  initGallery(p);
  if (s_request[p] && s_automatic[p] && !rotate) s_request[p] = false;
  if (s_request[p] && s_automatic[p] && !idle) return false;
  bool redraw = false, late = false;
  ArtTime art = {};
  if (!s_request[p]) {
    if (now - s_scanMs < 1000) return false;
    s_scanMs = now;
    if (rotate && idle && dispSettings.wallEvery && now - s_changedMs >= (uint32_t)dispSettings.wallEvery * 60000 &&
        wallFiles(p).size() > 1) {
      wallNext(p);
      s_automatic[p] = true;
    } else {
      art = artTime(s_loadedName.c_str());
      redraw = s_sharp && s_w == w && s_h == h && !(art == s_loadedArt);
      // Far off (drawn at boot before the clock was set, or before a long sleep): at once.
      late = art.utc - s_loadedArt.utc > 900;
      if (!redraw || !(idle || late)) return false;
    }
  }
  if (makeRoom) makeRoom();
  WallJob job = {};
  job.w = w;
  job.h = h;
  job.blur = redraw ? s_blurR : dispSettings.glassBlur;
  job.dim = redraw ? s_dim : dispSettings.glassDim;
  job.settings = settingsKey();
  job.epoch = s_epoch;
  job.requestId = s_requestId[p];
  job.automatic = redraw ? !late : s_automatic[p];
  job.redraw = redraw;
  strlcpy(job.name, redraw ? s_loadedName.c_str() : s_requested[p].c_str(), sizeof(job.name));
  job.art = redraw ? art : artTime(job.name);
  strlcpy(job.path, wallGalleryPath(p, job.name).c_str(), sizeof(job.path));
  portENTER_CRITICAL(&s_jobMux);
  s_job = job;
  s_jobState = JOB_RUNNING;
  portEXIT_CRITICAL(&s_jobMux);
  if (!wallRunWorker(wallJob, wallOnSd() && job.name[0], "deck_wall")) {
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
  if (s_job.redraw) {
    lv_image_cache_drop(&s_sharpDsc);
    lv_image_cache_drop(&s_blurDsc);
    s_loadedArt = s_job.art;
    ++s_generation;
  } else {
    installImage(s_job);
  }
  discardJob();
  Serial.printf("Deck: wallpaper swap %lu ms\n", (unsigned long)(millis() - start));
}

void wallArtPreview(uint32_t utc) { s_artPreview = utc; }

void wallRestartTimer() { s_changedMs = millis(); }

}  // namespace deck
