// Built-in wallpapers. Each scene is a sky gradient, an optional sun or moon and
// ridge layers back to front, plus one effect. x is measured in short screen
// sides, so shapes keep their size in both orientations.
#include "deck_art.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <string.h>
#include <time.h>

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

// Colours through the day, for the scenes with a sky. The scene's own light is one of the four.
enum Phase : uint8_t { NIGHT, TWILIGHT, GOLDEN, DAY, PHASES };
constexpr int SKIES = 5;
constexpr float PHASE_EL[PHASES] = {-9, -3, 3, 12};  // sun height in degrees
struct Palette { uint32_t sky[4], layer[5][2]; };
const Palette PALETTES[SKIES][PHASES] = {
  {  // Sunset hills
   {{0x060B1D, 0x0C1734, 0x1A2A50, 0x2A3D66},
    {{0x1B2444, 0x141B36}, {0x111831, 0x0C1226}, {0x090D1C, 0x05080F}}},
   {{0x101C3F, 0x2C2F5E, 0x6A4A72, 0xB06A6C},
    {{0x43395A, 0x30294A}, {0x2C2542, 0x201B33}, {0x1A1729, 0x0F0D19}}},
   {{0x1F3459, 0x6B4D78, 0xC9706A, 0xF0A868},
    {{0x6E5070, 0x4D3A60}, {0x4A3A58, 0x352A48}, {0x2C2640, 0x191628}}},
   {{0x2F74BD, 0x5C9CD4, 0x9CC7E6, 0xD3E6EF},
    {{0x7FA58F, 0x5F8A74}, {0x55865A, 0x3F6C48}, {0x35602F, 0x213F22}}}},
  {  // Misty ridges
   {{0x070D1C, 0x10203A, 0x22384F, 0x30485C},
    {{0x263C50, 0x2C4457}, {0x1C3044, 0x24394C}, {0x132434, 0x1A2D3D}, {0x0B1824, 0x111F2B}, {0x050B12, 0x081018}}},
   {{0x17294A, 0x3F5678, 0x8A8391, 0xB58F84},
    {{0x56687F, 0x6E7B8A}, {0x3F5369, 0x566A7C}, {0x284053, 0x3E5467}, {0x152B3C, 0x253D4D}, {0x091722, 0x0E1F2A}}},
   {{0x3D6A86, 0x86A9B0, 0xD9CDB4, 0xE6C9A2},
    {{0x86A3AB, 0xA9B9B6}, {0x5F8592, 0x86A2A8}, {0x3C6675, 0x638692}, {0x214656, 0x3B6270}, {0x0E2733, 0x16343F}}},
   {{0x3F86BD, 0x86B7D3, 0xC4DBE2, 0xDFE9E6},
    {{0x8FB4C2, 0xB4CCCF}, {0x67969F, 0x90B1B4}, {0x427A7C, 0x6A9694}, {0x265A55, 0x42766C}, {0x123A30, 0x1C4A3C}}}},
  {  // Aurora
   {{0x050A1C, 0x0B1F3A, 0x12405A, 0x12405A},
    {{0x1A3148, 0x10202F}, {0x0A1722, 0x050C13}}},
   {{0x101C42, 0x35406E, 0x86708A, 0x86708A},
    {{0x3A4866, 0x263348}, {0x1C2636, 0x0E141E}}},
   {{0x274674, 0x8A86A0, 0xE9B08A, 0xE9B08A},
    {{0x8A8498, 0x5E5E74}, {0x474A5A, 0x23262F}}},
   {{0x2E78BE, 0x78B0DA, 0xC6DFEC, 0xC6DFEC},
    {{0xD5E2EC, 0x8FA9BD}, {0x4D6A7C, 0x223444}}}},
  {  // Dunes
   {{0x060A1A, 0x0F1A36, 0x1D2C4E, 0x28385A},
    {{0x2D3350, 0x1C2140}, {0x272A45, 0x151833}, {0x1F2038, 0x0F1126}, {0x151528, 0x07080F}}},
   {{0x14224A, 0x413F68, 0x9A6675, 0xC98574},
    {{0x7D5A60, 0x53394C}, {0x704A52, 0x402838}, {0x5A3842, 0x2E1B28}, {0x3F2730, 0x1A0F16}}},
   {{0x2D4A73, 0x8A7F95, 0xE0A982, 0xF2C79A},
    {{0xC98F63, 0x9A6248}, {0xC7804E, 0x84492F}, {0xB3683B, 0x66331F}, {0x8A4A2B, 0x3D1C14}}},
   {{0x2A78C4, 0x6AAADC, 0xB4D6EA, 0xDCE9EC},
    {{0xE6C493, 0xC39A68}, {0xE0B478, 0xB98550}, {0xD19E62, 0x9A6A3E}, {0xB57F4A, 0x6D4526}}}},
  {  // Moonlit lake
   {{0x070D22, 0x15284F, 0x35548A, 0x35548A},
    {{0x21365F, 0x2B4674}, {0x121F3E, 0x1A2B50}}},
   {{0x16244C, 0x4A4A7C, 0xB97F83, 0xB97F83},
    {{0x4B4A70, 0x5A567C}, {0x2B2C4C, 0x37365A}}},
   {{0x2B4A7C, 0x8A7894, 0xEFAB7C, 0xEFAB7C},
    {{0x7A6480, 0x8D7088}, {0x463A58, 0x574666}}},
   {{0x2C74BB, 0x6FA9D6, 0xBFDCEA, 0xBFDCEA},
    {{0x5F8AA0, 0x7EA5B4}, {0x2F5F54, 0x44766A}}}},
};
const Phase OWN_LIGHT[SKIES] = {GOLDEN, GOLDEN, NIGHT, GOLDEN, NIGHT};
const float SKYLINE[SKIES] = {.60f, .46f, .68f, .45f, .60f};  // where the sun sets
const uint32_t WATER_C[PHASES] = {0x08122C, 0x22203A, 0x563E42, 0x285476};
const uint32_t GLINT_C[PHASES] = {0xCEDCFA, 0xF0BEB4, 0xFFD6A0, 0xFFFCEC};
const uint32_t PATH_C[PHASES] = {0x7896CD, 0xBE828C, 0xF0AA6E, 0xBED7E6};
const uint32_t SHORE_C[PHASES] = {0x96B4E1, 0xC8A0AA, 0xFACDA5, 0xD7E8F0};

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

// The sun or the moon.
struct Body {
  float x, y, r, disc;  // disc: how much of it shows, 0..1
  Rgb c, glow;
  float glowR, glowAmt;  // amount 0 = not there
  float path;            // strength of its path on the water
};
// All colours and lights of one picture.
struct Light {
  Rgb sky[4], layer[5][2], water, glint, path, shore;
  Body body[2];
  float stars, night;  // night: how much of the stars and the aurora shows
};
// Monotone cubic through the stops: straight pieces showed a band at every stop.
float skyChannel(const Stop* s, const Light& l, float Rgb::*c, float y) {
  float d[3], m[4] = {};
  for (int i = 0; i < 3; i++) {
    const float h = s[i + 1].y - s[i].y;
    d[i] = h > 0 ? (l.sky[i + 1].*c - l.sky[i].*c) / h : 0;
  }
  m[0] = d[0];
  for (int i = 1; i < 3; i++)
    if (d[i - 1] * d[i] > 0) {
      const float h0 = s[i].y - s[i - 1].y, h1 = s[i + 1].y - s[i].y, w0 = 2 * h1 + h0, w1 = h1 + 2 * h0;
      m[i] = (w0 + w1) / (w0 / d[i - 1] + w1 / d[i]);
    }
  if (y <= s[0].y) return l.sky[0].*c;
  for (int i = 0; i < 3; i++)
    if (y <= s[i + 1].y) {
      const float h = s[i + 1].y - s[i].y, t = (y - s[i].y) / h, t2 = t * t, t3 = t2 * t;
      return (2 * t3 - 3 * t2 + 1) * l.sky[i].*c + (t3 - 2 * t2 + t) * h * m[i] +
             (3 * t2 - 2 * t3) * l.sky[i + 1].*c + (t3 - t2) * h * m[i + 1];
    }
  return l.sky[3].*c;
}
Rgb skyAt(const Scene& s, const Light& l, float y) {
  return {skyChannel(s.sky, l, &Rgb::r, y), skyChannel(s.sky, l, &Rgb::g, y), skyChannel(s.sky, l, &Rgb::b, y)};
}

constexpr float DEG = (float)M_PI / 180;
// Degrees: height now and at noon, hour angle (0 = noon, west positive), hour angle of sunset.
struct SunPos { float el, top, ha, set; };
SunPos sunPos(const ArtTime& at) {
  if (!at.place) {
    const time_t t = at.utc;
    struct tm lt;
    localtime_r(&t, &lt);
    const float min = lt.tm_hour * 60 + lt.tm_min + lt.tm_sec / 60.0f;
    return {38 * sinf((min - 420) / 720 * (float)M_PI), 38, (min - 780) / 4, 90};
  }
  // Low precision solar position (Astronomical Almanac), good to about a minute.
  const double n = at.utc / 86400.0 - 10957.5, rad = M_PI / 180;
  const double g = (357.529 + 0.98560028 * n) * rad, q = 280.459 + 0.98564736 * n;
  const double lam = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * rad, eps = 23.439 * rad;
  const double dec = asin(sin(eps) * sin(lam)), ra = atan2(cos(eps) * sin(lam), cos(lam)) / rad;
  const float ha = (float)remainder(280.46062 + 360.98564736629 * n + at.lon - ra, 360.0);
  const float lat = at.lat * DEG, sl = sinf(lat), cl = cosf(lat), sd = (float)sin(dec), cd = (float)cos(dec);
  const float c = (sinf(-.833f * DEG) - sl * sd) / fmaxf(cl * cd, .001f);
  SunPos p;
  p.ha = ha;
  p.el = asinf(fminf(fmaxf(sl * sd + cl * cd * cosf(ha * DEG), -1), 1)) / DEG;
  p.top = fmaxf(90 - fabsf(at.lat - (float)(dec / rad)), 5);
  p.set = fminf(fmaxf(acosf(fminf(fmaxf(c, -1), 1)) / DEG, 5), 175);  // polar day and night: never 0 or 180
  return p;
}

Rgb blend(uint32_t a, uint32_t b, float k) {
  Rgb p = rgb(a);
  mix(p, rgb(b), k);
  return p;
}

Light lightOwn(int index) {
  const Scene& s = SCENES[index];
  Light l = {};
  for (int i = 0; i < 4; i++) l.sky[i] = rgb(s.sky[i].c);
  for (int i = 0; i < 5; i++) {
    l.layer[i][0] = rgb(s.layers[i].top);
    l.layer[i][1] = rgb(s.layers[i].bottom);
  }
  l.water = rgb(WATER_C[NIGHT]);
  l.glint = rgb(GLINT_C[NIGHT]);
  l.path = rgb(PATH_C[NIGHT]);
  l.shore = rgb(SHORE_C[NIGHT]);
  if (s.sun.r > 0)
    l.body[0] = {s.sun.x, s.sun.y, s.sun.r, 1, rgb(s.sun.c), rgb(s.sun.glow), s.sun.glowR, s.sun.glowAmt, 1};
  l.stars = s.stars;
  l.night = 1;
  return l;
}

Light lightAt(int index, const ArtTime& at) {
  const Scene& s = SCENES[index];
  const SunPos sun = sunPos(at);
  const float e = sun.el;
  int i = 0;
  while (i < DAY - 1 && e > PHASE_EL[i + 1]) i++;
  const float k = smooth(PHASE_EL[i], PHASE_EL[i + 1], e);
  const Palette &a = PALETTES[index][i], &b = PALETTES[index][i + 1];
  Light l = {};
  for (int n = 0; n < 4; n++) l.sky[n] = blend(a.sky[n], b.sky[n], k);
  for (int n = 0; n < 5; n++)
    for (int c = 0; c < 2; c++) l.layer[n][c] = blend(a.layer[n][c], b.layer[n][c], k);
  l.water = blend(WATER_C[i], WATER_C[i + 1], k);
  l.glint = blend(GLINT_C[i], GLINT_C[i + 1], k);
  l.path = blend(PATH_C[i], PATH_C[i + 1], k);
  l.shore = blend(SHORE_C[i], SHORE_C[i + 1], k);
  l.night = 1 - smooth(-9, -3, e);
  l.stars = s.stars > 0 ? s.stars : .0009f;

  // The sun from left to right over the skyline, the moon (always full) from sunset to sunrise.
  const bool own = OWN_LIGHT[index] == GOLDEN;
  const float low = 1 - smooth(2, 14, e), hor = SKYLINE[index], peak = .34f - .24f * sat(sun.top / 60);
  l.body[0] = {.12f + .76f * (sun.ha + sun.set) / (2 * sun.set), hor - e / sun.top * (hor - peak),
               own ? s.sun.r : s.fx == FX_LAKE ? .055f : .05f, smooth(-3, -1, e),
               blend(0xFFFBE8, own ? s.sun.c : 0xFFDF9C, low), blend(0xFFF2D0, own ? s.sun.glow : 0xFFB070, low),
               .5f, (.35f + .20f * low) * smooth(-7, 1, e), smooth(-1, 3, e)};
  const float g = fmodf(sun.ha - sun.set + 720, 360) / (360 - 2 * sun.set);
  if (g <= 1) {
    const float up = sinf((float)M_PI * g), show = sat(up * 5) * (1 - smooth(-6, 0, e));
    l.body[1] = {.12f + .76f * g, hor - up * (hor - .20f), .048f, show, rgb(0xF4F1E2), rgb(0x9DB8E8),
                 .45f, .40f * show, show};
  }
  return l;
}

// Everything that depends on the column only.
struct Painter {
  const Scene& s;
  const Light& look;
  int w, h, n = 0;
  float m;
  float *edge[5] = {}, *light[5] = {};                                          // ridge lines; dunes: slope light
  float *ribbon[RIBBONS] = {}, *base[CURTAINS] = {}, *ray[CURTAINS] = {}, *tall[CURTAINS] = {};
  Rgb ribbonC[RIBBONS];
  float* mem = nullptr;

  Painter(const Scene& scene, const Light& l, int w_, int h_) : s(scene), look(l), w(w_), h(h_) {}
  ~Painter() { heap_caps_free(mem); }

  bool begin() {
    m = w < h ? w : h;
    while (n < 5 && s.layers[n].amp > 0) n++;
    const int rows = 2 * n + (s.fx == FX_RIBBONS ? RIBBONS : s.fx == FX_AURORA ? 3 * CURTAINS : 0);
    mem = (float*)heap_caps_malloc(rows * w * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!mem) return false;
    float* at = mem;
    const auto take = [&] { float* p = at; at += w; return p; };
    for (int i = 0; i < n; i++) {
      const Layer& l = s.layers[i];
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
    if (!mirrored && look.stars > 0 && look.night > 0 && y < h * .7f &&
        hash(x * 7919 + y * 104729 + 17) < look.stars)
      mix(p, {255, 255, 255}, (.25f + .75f * hash(x + y * 613)) * (1 - y / (h * .7f)) * look.night);
    for (const Body& b : look.body) {
      if (b.glowAmt <= 0) continue;
      const float dx = x - b.x * w, dy = y - b.y * h, far = b.glowR * m, d2 = dx * dx + dy * dy;
      if (d2 < far * far) {
        const float d = sqrtf(d2) / m, g = 1 - d / b.glowR;
        mix(p, b.glow, g * g * b.glowAmt);
        if (!mirrored) mix(p, b.c, sat((b.r - d) * m + .5f) * b.disc);
      }
    }
    if (s.fx == FX_AURORA && look.night > 0)
      for (int i = 0; i < CURTAINS; i++) {
        const float t = (base[i][x] - y) / tall[i][x];
        if (t < -.5f || t > 4.5f) continue;
        const float z = sat(t * .75f - .12f);
        mix(p, {60 + 120 * z, 230 - 150 * z, 150 + 75 * z},
            sat(ray[i][x] * (t < 0 ? expf(-t * t * 22) : expf(-t * 1.5f)) * .85f) * look.night);
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
      Rgb c = look.layer[i][0];
      mix(c, look.layer[i][1], k);
      mix(p, c, sat(t));
    }
    return p;
  }

  void row(int y, uint8_t* out) const {
    const bool water = s.fx == FX_LAKE && y > HORIZON * h;
    const float t = (y - HORIZON * h) / h;  // water: depth below the horizon
    const int yy = water ? (int)fmaxf(0, floorf(HORIZON * h - t * h * 1.15f)) : y;
    const Rgb sky = skyAt(s, look, (float)yy / h);
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
        mix(p, look.water, .30f + .5f * t);
        // Ripples in the path of the sun or moon: noise rows that grow towards the viewer.
        for (const Body& b : look.body) {
          if (b.path <= 0) continue;
          const float dx = x / m - b.x * w / m, e = dx * dx / (spread * spread);
          if (e < 7) {
            const float rip = .6f * noise2(xs * (16 - 18 * t), q * 1.6f, 61) + .4f * noise2(xs * (31 - 30 * t) + 7, q * 2.9f, 83);
            mix(p, look.glint, b.path * expf(-e) * smooth(.38f, .74f, rip) * (.30f + .50f * (1 - t / .38f)));
          }
          const float e2 = dx * dx / (.02f + .10f * t);
          if (e2 < 7) mix(p, look.path, b.path * .20f * expf(-e2));
        }
        mix(p, look.shore, .35f * sat(1 - t * 60));
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
      // Rounded at random, or the 8-bit steps of a slow gradient show as bands.
      const float n = hash(x * 5987 + y * 92821 + 3) * .999f;
      *out++ = (uint8_t)fminf(fmaxf(p.r, 0) + n, 255);
      *out++ = (uint8_t)fminf(fmaxf(p.g, 0) + n, 255);
      *out++ = (uint8_t)fminf(fmaxf(p.b, 0) + n, 255);
    }
  }
};

}  // namespace

const char* artName(int index) { return index >= 0 && index < ART_COUNT ? SCENES[index].name : ""; }

bool artFollows(int index) { return index >= 0 && index < SKIES; }

uint32_t artStep(const ArtTime& now) {
  // Judged once per 10 minutes, so a step never goes back.
  ArtTime slot = now;
  slot.utc = now.utc / 600 * 600;
  const float el = sunPos(slot).el;
  return el > -10 && el < 13 ? now.utc / 300 * 300 : slot.utc;
}

bool artDraw(int index, int w, int h, const ArtTime& at, void (*row)(void* ctx, int y, const uint8_t* rgb), void* ctx) {
  if (index < 0 || index >= ART_COUNT) return false;
  const Light light = at.utc && artFollows(index) ? lightAt(index, at) : lightOwn(index);
  Painter p(SCENES[index], light, w, h);
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
