// Guition deck: Glass theme wallpaper. See deck_wall.h.
#include "deck_wall.h"
#include "deck_config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>

namespace deck {
namespace {

constexpr int BLUR_R = 12;  // box radius; three passes come close to a gaussian
constexpr int DIM = 216;    // brightness of the shown picture, of 256

uint16_t* s_sharp = nullptr;
uint16_t* s_blur = nullptr;
int s_w = 0, s_h = 0;
lv_image_dsc_t s_sharpDsc, s_blurDsc;

void fillDsc(lv_image_dsc_t& d, const uint16_t* px, int w, int h) {
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_RGB565;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 2;
  d.data = (const uint8_t*)px;
  d.data_size = (uint32_t)w * h * 2;
}

uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// No file yet: a dusk gradient with a warm glow, so Glass works out of the box.
void gradient(uint16_t* px, int w, int h) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float t = (x / (float)w + y / (float)h) / 2;
      const float dx = (x - 0.2f * w) / w, dy = (y - 1.05f * h) / h;
      const float glow = fmaxf(0, 1 - sqrtf(dx * dx + dy * dy) / 0.75f);
      const float g2 = glow * glow;
      const int r = (int)(0x24 + (0x16 - 0x24) * t + 0xB0 * g2);
      const int g = (int)(0x2C + (0x5E - 0x2C) * t + 0x58 * g2);
      const int b = (int)(0x5C + (0x66 - 0x5C) * t + 0x10 * g2);
      px[y * w + x] = rgb565(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    }
}

// One box pass along rows (step 1) or columns (step w), edges clamped.
void boxPass(const uint8_t* src, uint8_t* dst, int w, int h, bool vertical) {
  const int n = 2 * BLUR_R + 1;
  const int lines = vertical ? w : h, len = vertical ? h : w, step = vertical ? w : 1;
  for (int l = 0; l < lines; l++) {
    const uint8_t* s = src + (vertical ? l : l * w);
    uint8_t* d = dst + (vertical ? l : l * w);
    int sum = 0;
    for (int i = -BLUR_R; i <= BLUR_R; i++) sum += s[(i < 0 ? 0 : i >= len ? len - 1 : i) * step];
    for (int i = 0; i < len; i++) {
      d[i * step] = (uint8_t)(sum / n);
      const int add = i + BLUR_R + 1, sub = i - BLUR_R;
      sum += s[(add >= len ? len - 1 : add) * step] - s[(sub < 0 ? 0 : sub) * step];
    }
  }
}

// Blur src into dst (both w x h RGB565), one channel plane at a time.
bool blurInto(const uint16_t* src, uint16_t* dst, int w, int h) {
  const size_t n = (size_t)w * h;
  uint8_t* a = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  uint8_t* b = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!a || !b) {
    free(a);
    free(b);
    return false;
  }
  memset(dst, 0, n * 2);
  for (int ch = 0; ch < 3; ch++) {
    for (size_t i = 0; i < n; i++) {
      const uint16_t p = src[i];
      a[i] = ch == 0 ? (p >> 8 & 0xF8) : ch == 1 ? (p >> 3 & 0xFC) : (p << 3 & 0xF8);
    }
    for (int pass = 0; pass < 3; pass++) {
      boxPass(a, b, w, h, false);
      boxPass(b, a, w, h, true);
    }
    for (size_t i = 0; i < n; i++)
      dst[i] |= ch == 0 ? (a[i] & 0xF8) << 8 : ch == 1 ? (a[i] & 0xFC) << 3 : a[i] >> 3;
    esp_task_wdt_reset();
  }
  free(a);
  free(b);
  return true;
}

}  // namespace

const char* wallPath(int w, int h) { return w > h ? "/wall_l.bin" : "/wall_p.bin"; }

bool wallLoad(int w, int h) {
  if (s_sharp && s_w == w && s_h == h) return true;
  wallFree();
  const size_t bytes = (size_t)w * h * 2;
  s_sharp = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  s_blur = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!s_sharp || !s_blur) {
    wallFree();
    return false;
  }
  bool loaded = false;
  if (deckFsReady()) {
    File f = LittleFS.open(wallPath(w, h), "r");
    if (f && f.size() == bytes) loaded = f.read((uint8_t*)s_sharp, bytes) == bytes;
    if (f) f.close();
  }
  if (!loaded) gradient(s_sharp, w, h);
  if (!blurInto(s_sharp, s_blur, w, h)) {
    wallFree();
    return false;
  }
  // The page shows the picture a little darker, for the text on it.
  for (size_t i = 0; i < (size_t)w * h; i++) {
    const uint16_t p = s_sharp[i];
    s_sharp[i] = rgb565((p >> 8 & 0xF8) * DIM >> 8, (p >> 3 & 0xFC) * DIM >> 8, (p << 3 & 0xF8) * DIM >> 8);
  }
  s_w = w;
  s_h = h;
  fillDsc(s_sharpDsc, s_sharp, w, h);
  fillDsc(s_blurDsc, s_blur, w, h);
  Serial.printf("Deck: wallpaper %dx%d %s\n", w, h, loaded ? "from file" : "built-in gradient");
  return true;
}

void wallFree() {
  free(s_sharp);
  free(s_blur);
  s_sharp = s_blur = nullptr;
  s_w = s_h = 0;
}

const lv_image_dsc_t* wallSharp() { return s_sharp ? &s_sharpDsc : nullptr; }
const lv_image_dsc_t* wallBlur() { return s_blur ? &s_blurDsc : nullptr; }

}  // namespace deck
