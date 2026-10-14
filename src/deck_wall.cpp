// Guition deck: Glass theme wallpaper. See deck_wall.h.
#include "deck_wall.h"
#include "deck_config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>

namespace deck {
namespace {

uint16_t* s_sharp = nullptr;  // RGB565, dimmed and dithered
uint8_t* s_blur = nullptr;    // RGB888 (LVGL order B, G, R), undimmed
int s_w = 0, s_h = 0, s_blurR = -1, s_dim = -1;
lv_image_dsc_t s_sharpDsc, s_blurDsc;
uint32_t s_generation = 0;

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
  File f = LittleFS.open(wallPath(w, h), "r");
  if (!f) return false;
  const size_t n = (size_t)w * h, size = f.size();
  bool ok = false;
  if (size == n * 3 || size == n * 2) {
    const int bpp = size == n * 3 ? 3 : 2;
    uint8_t row[480 * 3];
    ok = w <= 480;
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
  esp_task_wdt_reset();
}

}  // namespace

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

bool wallLoad(int w, int h, int blurR, int dimPct) {
  if (s_sharp && s_w == w && s_h == h && s_blurR == blurR && s_dim == dimPct) return true;
  wallFree();
  const size_t n = (size_t)w * h;
  s_sharp = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  s_blur = (uint8_t*)heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM);
  uint8_t* planes = (uint8_t*)heap_caps_malloc(n * 4, MALLOC_CAP_SPIRAM);  // r, g, b, scratch
  if (!s_sharp || !s_blur || !planes) {
    free(planes);
    wallFree();
    return false;
  }
  uint8_t *r = planes, *g = planes + n, *b = planes + 2 * n, *tmp = planes + 3 * n;
  const bool loaded = loadFile(r, g, b, w, h);
  if (!loaded) gradient(r, g, b, w, h);

  // The page shows the picture darker, for the text on it.
  const int keep = (100 - dimPct) * 256 / 100;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const int i = y * w + x;
      s_sharp[i] = wallDither565(r[i] * keep >> 8, g[i] * keep >> 8, b[i] * keep >> 8, x, y);
    }
  uint8_t* ch[3] = {b, g, r};  // LVGL's RGB888 byte order
  for (int c = 0; c < 3; c++) {
    blurPlane(ch[c], tmp, w, h, blurR);
    for (size_t i = 0; i < n; i++) s_blur[i * 3 + c] = ch[c][i];
  }
  free(planes);

  s_w = w;
  s_h = h;
  s_blurR = blurR;
  s_dim = dimPct;
  fillDsc(s_sharpDsc, s_sharp, w, h, LV_COLOR_FORMAT_RGB565, 2);
  fillDsc(s_blurDsc, s_blur, w, h, LV_COLOR_FORMAT_RGB888, 3);
  Serial.printf("Deck: wallpaper %dx%d %s\n", w, h, loaded ? "from file" : "built-in gradient");
  return true;
}

void wallFree() {
  s_generation++;  // also covers reloads that reuse the same allocation addresses
  free(s_sharp);
  free(s_blur);
  s_sharp = nullptr;
  s_blur = nullptr;
  s_w = s_h = 0;
  s_blurR = s_dim = -1;
}

const lv_image_dsc_t* wallSharp() { return s_sharp ? &s_sharpDsc : nullptr; }
const lv_image_dsc_t* wallBlur() { return s_blur ? &s_blurDsc : nullptr; }
uint32_t wallGeneration() { return s_generation; }

}  // namespace deck
