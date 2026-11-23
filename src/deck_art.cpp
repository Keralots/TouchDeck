// Built-in wallpapers. Each scene is a sky gradient, an optional sun or moon and
// ridge layers back to front, plus one effect. x is measured in short screen
// sides, so shapes keep their size in both orientations.
#include "deck_art.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>

namespace deck {
namespace {

enum Kind : uint8_t { HILLS, PEAKS, DUNES };
enum Fx : uint8_t { FX_NONE, FX_AURORA, FX_LAKE, FX_RIBBONS };

struct Stop { float y; uint32_t c; };
struct Sun { float x, y, r; uint32_t c, glow; float glowR, glowAmt; };
struct Layer { Kind kind; uint32_t top, bottom; float y, amp, freq, seed; };
struct Scene {
  const char* name;
  Stop sky[4];
  Sun sun;      // r 0 = none
  float stars;  // share of the sky pixels
  Fx fx;
  Layer layers[5];  // amp 0 = end
};

const Scene SCENES[ART_COUNT] = {
  {"Sunset hills", {{0, 0x1F3459}, {.32f, 0x6B4D78}, {.5f, 0xC9706A}, {.64f, 0xF0A868}},
   {.68f, .58f, .075f, 0xFFE0A0, 0xFFB070, .5f, .55f}, 0, FX_NONE,
   {{HILLS, 0x6E5070, 0x4D3A60, .60f, .045f, 3.1f, 1.3f}, {HILLS, 0x4A3A58, 0x352A48, .70f, .05f, 4.2f, 2.1f},
    {HILLS, 0x2C2640, 0x191628, .81f, .05f, 5.2f, 3.4f}}},
  {"Misty ridges", {{0, 0x3D6A86}, {.3f, 0x86A9B0}, {.5f, 0xD9CDB4}, {.62f, 0xE6C9A2}},
   {.3f, .42f, .05f, 0xFFF3D6, 0xFFE2B0, .55f, .45f}, 0, FX_NONE,
   {{PEAKS, 0x86A3AB, 0xA9B9B6, .44f, .20f, 1.5f, 1}, {PEAKS, 0x5F8592, 0x86A2A8, .55f, .20f, 1.9f, 2},
    {PEAKS, 0x3C6675, 0x638692, .67f, .18f, 2.3f, 3}, {PEAKS, 0x214656, 0x3B6270, .79f, .16f, 2.8f, 4},
    {PEAKS, 0x0E2733, 0x16343F, .92f, .13f, 3.6f, 5}}},
  {"Aurora", {{0, 0x050A1C}, {.4f, 0x0B1F3A}, {.72f, 0x12405A}, {1, 0x12405A}}, {}, .0016f, FX_AURORA,
   {{PEAKS, 0x1A3148, 0x10202F, .72f, .20f, 1.7f, 8}, {PEAKS, 0x0A1722, 0x050C13, .84f, .14f, 2.7f, 4}}},
  {"Dunes", {{0, 0x2D4A73}, {.28f, 0x8A7F95}, {.45f, 0xE0A982}, {.5f, 0xF2C79A}},
   {.24f, .36f, .045f, 0xFFF0D0, 0xFFD09A, .5f, .4f}, 0, FX_NONE,
   {{DUNES, 0xC98F63, 0x9A6248, .46f, .035f, 2.3f, .6f}, {DUNES, 0xC7804E, 0x84492F, .56f, .055f, 2.0f, 2.2f},
    {DUNES, 0xB3683B, 0x66331F, .70f, .075f, 1.7f, 4.1f}, {DUNES, 0x8A4A2B, 0x3D1C14, .88f, .08f, 1.9f, 5.6f}}},
  {"Moonlit lake", {{0, 0x070D22}, {.35f, 0x15284F}, {.6f, 0x35548A}, {1, 0x35548A}},
   {.64f, .26f, .055f, 0xF4F1E2, 0x9DB8E8, .45f, .42f}, .0011f, FX_LAKE,
   {{PEAKS, 0x21365F, 0x2B4674, .55f, .16f, 1.6f, 3}, {PEAKS, 0x121F3E, 0x1A2B50, .60f, .11f, 2.6f, 6}}},
  {"Ribbons", {{0, 0x14123A}, {.5f, 0x16305E}, {1, 0x0C4A5C}, {1, 0x0C4A5C}}, {}, 0, FX_RIBBONS, {}},
};
constexpr int RIBBONS = 6, CURTAINS = 3;
const uint32_t RIBBON_C[RIBBONS] = {0x3A2A86, 0x5A35A8, 0x2F58B8, 0x1F86B0, 0x189A9C, 0x105C74};
constexpr float HORIZON = .62f;

struct Rgb { float r, g, b; };
Rgb rgb(uint32_t c) { return {(float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255)}; }
void mix(Rgb& p, const Rgb& c, float a) {
  p.r += (c.r - p.r) * a;
  p.g += (c.g - p.g) * a;
  p.b += (c.b - p.b) * a;
}
float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float smooth(float a, float b, float v) {
  v = sat((v - a) / (b - a));
  return v * v * (3 - 2 * v);
}
float hash(uint32_t n) {
  n = (n ^ 61) ^ (n >> 16);
  n += n << 3;
  n ^= n >> 4;
  n *= 0x27d4eb2d;
  n ^= n >> 15;
  return (float)n * (1.0f / 4294967296.0f);
}
float noise(float x, int s) {
  const float i = floorf(x), f = x - i, u = f * f * (3 - 2 * f);
  const int k = (int)i + s * 131;
  return hash(k) * (1 - u) + hash(k + 1) * u;
}
float noise2(float x, float y, int s) {
  const float i = floorf(y), f = y - i, u = f * f * (3 - 2 * f);
  const int k = s + (int)i * 57;
  return noise(x, k) * (1 - u) + noise(x, k + 57) * u;
}
float ridge(const Layer& l, float x) {
  if (l.kind == PEAKS) {
    float v = 0, a = .55f, q = l.freq;
    for (int o = 0; o < 5; o++, a *= .48f, q *= 2.1f)
      v += a * (1 - fabsf(2 * noise(x * q + l.seed * 7.3f, (int)l.seed + o) - 1));
    return v - .55f;
  }
  return .6f * sinf(x * l.freq + l.seed) + .28f * sinf(x * l.freq * 2.3f + l.seed * 1.7f) +
         (l.kind == DUNES ? 0 : .12f * sinf(x * l.freq * 5.1f + l.seed * 2.9f));
}
Rgb skyAt(const Scene& s, float y) {
  if (y <= s.sky[0].y) return rgb(s.sky[0].c);
  for (int i = 1; i < 4; i++)
    if (y <= s.sky[i].y) {
      Rgb p = rgb(s.sky[i - 1].c);
      if (s.sky[i].y > s.sky[i - 1].y) mix(p, rgb(s.sky[i].c), (y - s.sky[i - 1].y) / (s.sky[i].y - s.sky[i - 1].y));
      return p;
    }
  return rgb(s.sky[3].c);
}

// Everything that depends on the column only.
struct Painter {
  const Scene& s;
  int w, h, n = 0;
  float m, sx, sy;
  float *edge[5] = {}, *light[5] = {};                                          // ridge lines; dunes: slope light
  float *ribbon[RIBBONS] = {}, *base[CURTAINS] = {}, *ray[CURTAINS] = {}, *tall[CURTAINS] = {};
  Rgb top[5], bottom[5], ribbonC[RIBBONS], sunC, glowC;
  float* mem = nullptr;

  Painter(const Scene& scene, int w_, int h_) : s(scene), w(w_), h(h_) {}
  ~Painter() { heap_caps_free(mem); }

  bool begin() {
    m = w < h ? w : h;
    sx = s.sun.x * w;
    sy = s.sun.y * h;
    sunC = rgb(s.sun.c);
    glowC = rgb(s.sun.glow);
    while (n < 5 && s.layers[n].amp > 0) n++;
    const int rows = 2 * n + (s.fx == FX_RIBBONS ? RIBBONS : s.fx == FX_AURORA ? 3 * CURTAINS : 0);
    mem = (float*)heap_caps_malloc(rows * w * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!mem) return false;
    float* at = mem;
    const auto take = [&] { float* p = at; at += w; return p; };
    for (int i = 0; i < n; i++) {
      const Layer& l = s.layers[i];
      top[i] = rgb(l.top);
      bottom[i] = rgb(l.bottom);
      edge[i] = take();
      light[i] = take();
      for (int x = 0; x < w; x++) {
        const float xs = x / m;
        edge[i][x] = (l.y - l.amp * ridge(l, xs)) * h;
        if (l.kind == DUNES) light[i][x] = .5f * tanhf((ridge(l, xs + .01f) - ridge(l, xs - .01f)) * 26);
      }
    }
    if (s.fx == FX_RIBBONS)
      for (int i = 0; i < RIBBONS; i++) {
        ribbon[i] = take();
        ribbonC[i] = rgb(RIBBON_C[i]);
        for (int x = 0; x < w; x++) {
          const float xs = x / m;
          ribbon[i][x] = (.2f + .13f * i + (.10f + .012f * i) * sinf(xs * (1.5f + .13f * i) + 1.1f * i + .4f) +
                          .035f * sinf(xs * 3.7f + 2.1f * i)) * h;
        }
      }
    if (s.fx == FX_AURORA)
      for (int i = 0; i < CURTAINS; i++) {
        static const float kAmt[CURTAINS] = {1, .75f, .5f};
        base[i] = take();
        ray[i] = take();
        tall[i] = take();
        for (int x = 0; x < w; x++) {
          const float xs = x / m;
          base[i][x] = (.58f - .10f * i + .06f * sinf(xs * 2.1f + 1.9f * i) + .03f * sinf(xs * 4.7f + i)) * h;
          const float rays = .55f * noise(xs * 13 + 40 * i, 20 + i) + .45f * noise(xs * 37 + 9 * i, 30 + i);
          ray[i][x] = (.25f + .75f * rays) * (.35f + .65f * noise(xs * 1.9f + 5 * i, 40 + i)) * kAmt[i];
          tall[i][x] = (.16f + .16f * noise(xs * 3 + 3 * i, 50 + i)) * h;
        }
      }
    return true;
  }

  // Sky, sun and ridges. sky = its colour on this row; mirrored: no stars, no disc.
  Rgb land(int x, int y, const Rgb& sky, bool mirrored) const {
    Rgb p = sky;
    if (!mirrored && s.stars > 0 && y < h * .7f && hash(x * 7919 + y * 104729 + 17) < s.stars)
      mix(p, {255, 255, 255}, (.25f + .75f * hash(x + y * 613)) * (1 - y / (h * .7f)));
    if (s.sun.r > 0) {
      const float dx = x - sx, dy = y - sy, far = s.sun.glowR * m, d2 = dx * dx + dy * dy;
      if (d2 < far * far) {
        const float d = sqrtf(d2) / m, g = 1 - d / s.sun.glowR;
        mix(p, glowC, g * g * s.sun.glowAmt);
        if (!mirrored) mix(p, sunC, sat((s.sun.r - d) * m + .5f));
      }
    }
    if (s.fx == FX_AURORA)
      for (int i = 0; i < CURTAINS; i++) {
        const float t = (base[i][x] - y) / tall[i][x];
        if (t < -.5f || t > 4.5f) continue;
        const float z = sat(t * .75f - .12f);
        mix(p, {60 + 120 * z, 230 - 150 * z, 150 + 75 * z},
            sat(ray[i][x] * (t < 0 ? expf(-t * t * 22) : expf(-t * 1.5f)) * .85f));
      }
    // From the front: the ones behind an opaque layer need no drawing.
    int first = n;
    while (first > 0 && y - edge[first - 1][x] + .5f < 1) first--;
    for (int i = first > 0 ? first - 1 : 0; i < n; i++) {
      const Layer& l = s.layers[i];
      const float t = y - edge[i][x] + .5f;
      if (t <= 0) continue;
      float k;
      if (l.kind == DUNES) {
        const float dep = sat(t / (h * .26f));
        k = sat(.5f - light[i][x] * (1 - dep * .8f) + .38f * dep);
      } else {
        k = sat(((float)y / h - l.y + l.amp * .5f) / (l.kind == PEAKS ? .30f : .26f));
      }
      Rgb c = top[i];
      mix(c, bottom[i], k);
      mix(p, c, sat(t));
    }
    return p;
  }

  void row(int y, uint8_t* out) const {
    const bool water = s.fx == FX_LAKE && y > HORIZON * h;
    const float t = (y - HORIZON * h) / h;  // water: depth below the horizon
    const int yy = water ? (int)fmaxf(0, floorf(HORIZON * h - t * h * 1.15f)) : y;
    const Rgb sky = skyAt(s, (float)yy / h);
    const float q = water ? 11 * logf(1 + t * 14) : 0;
    const float spread = water ? (.03f + .20f * t) * (.7f + .6f * noise2(3.5f, q * .8f, 70)) : 1;
    for (int x = 0; x < w; x++) {
      Rgb p;
      if (water) {
        const float xs = x / m;
        const float wob = sinf(y * 260.0f / h * (1 - t) + sinf(xs * 9) * 1.5f) * (1 + 14 * t) * m / 480;
        int xx = (int)floorf(x + wob);
        xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
        p = land(xx, yy, sky, true);
        mix(p, {8, 18, 44}, .30f + .5f * t);
        // Ripples in the moon's path: noise rows that grow towards the viewer.
        const float dx = (x - sx) / m, e = dx * dx / (spread * spread);
        if (e < 7) {
          const float rip = .6f * noise2(xs * (16 - 18 * t), q * 1.6f, 61) + .4f * noise2(xs * (31 - 30 * t) + 7, q * 2.9f, 83);
          mix(p, {206, 220, 250}, expf(-e) * smooth(.38f, .74f, rip) * (.30f + .50f * (1 - t / .38f)));
        }
        const float e2 = dx * dx / (.02f + .10f * t);
        if (e2 < 7) mix(p, {120, 150, 205}, .20f * expf(-e2));
        mix(p, {150, 180, 225}, .35f * sat(1 - t * 60));
      } else {
        p = land(x, y, sky, false);
      }
      if (s.fx == FX_RIBBONS)
        for (int i = 0; i < RIBBONS; i++) {
          const float below = y - ribbon[i][x], d = below / h;
          const float e = sat(below + .5f);
          if (e <= 0) continue;
          mix(p, ribbonC[i], e * (.10f + .55f * expf(-d * 6.5f)));
          if (d < .12f) mix(p, {200, 225, 255}, e * .30f * expf(-d * 55));
        }
      *out++ = (uint8_t)(fminf(fmaxf(p.r, 0), 255) + .5f);
      *out++ = (uint8_t)(fminf(fmaxf(p.g, 0), 255) + .5f);
      *out++ = (uint8_t)(fminf(fmaxf(p.b, 0), 255) + .5f);
    }
  }
};

}  // namespace

const char* artName(int index) { return index >= 0 && index < ART_COUNT ? SCENES[index].name : ""; }

bool artDraw(int index, int w, int h, void (*row)(void* ctx, int y, const uint8_t* rgb), void* ctx) {
  if (index < 0 || index >= ART_COUNT) return false;
  Painter p(SCENES[index], w, h);
  uint8_t* line = (uint8_t*)heap_caps_malloc(w * 3, MALLOC_CAP_SPIRAM);
  const bool ok = line && p.begin();
  const bool wdt = esp_task_wdt_status(nullptr) == ESP_OK;
  for (int y = 0; ok && y < h; y++) {
    p.row(y, line);
    row(ctx, y, line);
    if (wdt && !(y & 31)) esp_task_wdt_reset();
  }
  heap_caps_free(line);
  return ok;
}

}  // namespace deck
