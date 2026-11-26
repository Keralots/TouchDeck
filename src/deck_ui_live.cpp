// Deck UI: moving details on a built-in Glass wallpaper. Water ripples under
// the moon or sun, stars twinkle, now and then a shooting star. Pixels change
// only between the tiles, in the shown page's baked background.
#include "deck_ui_int.h"
#include <math.h>

namespace deckui {
namespace {

constexpr uint8_t FX_WATER = 1, FX_STARS = 2, FX_METEOR = 4;
constexpr int MAX_STARS = 40, SPARKLERS = 6, TWINKLES = 6;
// A redrawn area costs about as much as this many extra pixels (7B, measured).
constexpr int AREA_PX = 1300;

struct Star {
  int16_t x, y;
  uint8_t c[3], bg[3];  // the star and the sky around it
  uint16_t shown;
  float phase, rate;
  bool sparkler;
  uint32_t sparkAt;  // start of the next sparkle
  bool sparkDrawn;
};

struct Meteor {
  bool on = false;
  uint32_t start, dur;
  float sx, sy, dx, dy, len, tail;
  lv_area_t last;
  bool drawn = false;
};

struct Live {
  int page = -1;
  lv_draw_buf_t* glass = nullptr;
  uint32_t gen = 0;
  int scene = -1, w = 0, h = 0, keep = 256;
  ArtLive art = {};
  std::vector<uint32_t> key;
  std::vector<lv_area_t> rects;  // page-relative areas the wallpaper may not change
  uint8_t* mask = nullptr;       // one bit a pixel: covered
  int stride = 0;
  std::vector<Star> stars;
  Meteor met;
  uint32_t nextMeteor = 0, lastMs = 0, frame = 0;
  bool waterDrawn = false;
};
Live L;

uint8_t s_fx = FX_WATER | FX_STARS | FX_METEOR;
uint8_t s_bench = 0;  // 1: no pixel writes, 2: no redraw requests
uint16_t s_meteorMin = 7, s_meteorMax = 18, s_everyMs = 33;
// Bench counters for GET /api/deck/live, since the last read.
uint32_t s_liveFrames = 0, s_usSum = 0, s_usMax = 0, s_areasMax = 0, s_pxMax = 0;
uint32_t s_rnd = 0x9E3779B9;

uint32_t rnd() {
  s_rnd ^= s_rnd << 13;
  s_rnd ^= s_rnd >> 17;
  s_rnd ^= s_rnd << 5;
  return s_rnd;
}
float rndf() { return (rnd() >> 8) / 16777216.0f; }

inline void unpack(uint16_t q, int& r, int& g, int& b) {
  r = (q >> 8 & 0xF8) | q >> 13;
  g = (q >> 3 & 0xFC) | (q >> 9 & 0x03);
  b = (q << 3 & 0xF8) | (q >> 2 & 0x07);
}
inline uint16_t pack(int r, int g, int b) {
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}
inline bool covered(int x, int y) { return L.mask[y * L.stride + (x >> 3)] >> (x & 7) & 1; }
inline const uint16_t* sharpRow(int y) { return (const uint16_t*)(wallSharp()->data + (size_t)y * wallSharp()->header.stride); }
inline uint16_t* glassRow(int y) { return (uint16_t*)(L.glass->data + (size_t)y * L.glass->header.stride); }

bool clip(lv_area_t& a) {
  a.x1 = LV_MAX(a.x1, 0);
  a.y1 = LV_MAX(a.y1, 0);
  a.x2 = LV_MIN(a.x2, L.w - 1);
  a.y2 = LV_MIN(a.y2, L.h - 1);
  return a.x1 <= a.x2 && a.y1 <= a.y2;
}

void addRect(lv_obj_t* o, const lv_area_t& rc) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_area_move(&a, -rc.x1, -rc.y1);
  if (clip(a)) L.rects.push_back(a);
}

// Tiles, frames and the top bar: everything drawn over the picture.
bool buildMask(Page& p) {
  L.rects.clear();
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (uint32_t i = 0; i < lv_obj_get_child_count(p.grid); i++) addRect(lv_obj_get_child(p.grid, i), rc);
  addRect(p.title, rc);
  addRect(p.clock, rc);
  addRect(p.wifi, rc);
  L.stride = (L.w + 7) / 8;
  heap_caps_free(L.mask);
  L.mask = (uint8_t*)heap_caps_calloc(L.stride, L.h, MALLOC_CAP_SPIRAM);
  if (!L.mask) return false;
  for (const lv_area_t& a : L.rects)
    for (int y = a.y1; y <= a.y2; y++)
      for (int x = a.x1; x <= a.x2; x++) L.mask[y * L.stride + (x >> 3)] |= 1 << (x & 7);
  return true;
}

// The parts of a that no tile covers, as few rects as the tile edges allow.
void gapRects(const lv_area_t& a, std::vector<lv_area_t>& out) {
  int ys[2 * 128 + 2], n = 0;
  ys[n++] = a.y1;
  ys[n++] = a.y2 + 1;
  for (const lv_area_t& r : L.rects) {
    if (n > 2 * 128) break;
    if (r.y1 > a.y1 && r.y1 <= a.y2) ys[n++] = r.y1;
    if (r.y2 + 1 > a.y1 && r.y2 + 1 <= a.y2) ys[n++] = r.y2 + 1;
  }
  std::sort(ys, ys + n);
  n = std::unique(ys, ys + n) - ys;
  const size_t before = out.size();
  for (int b = 0; b + 1 < n; b++) {
    const int y0 = ys[b], y1 = ys[b + 1] - 1;
    std::pair<int, int> cov[128];
    int c = 0;
    for (const lv_area_t& r : L.rects)
      if (c < 128 && r.y1 <= y0 && r.y2 >= y1 && r.x2 >= a.x1 && r.x1 <= a.x2) cov[c++] = {r.x1, r.x2};
    std::sort(cov, cov + c);
    int x = a.x1;
    const auto emit = [&](int x0, int x1) {
      // Same columns as a rect of the band above: grow it.
      for (size_t k = before; k < out.size(); k++)
        if (out[k].x1 == x0 && out[k].x2 == x1 && out[k].y2 == y0 - 1) {
          out[k].y2 = y1;
          return;
        }
      out.push_back({x0, y0, x1, y1});
    };
    for (int i = 0; i < c; i++) {
      if (cov[i].first > x) emit(x, cov[i].first - 1);
      x = LV_MAX(x, cov[i].second + 1);
    }
    if (x <= a.x2) emit(x, a.x2);
  }
}

uint32_t s_areas = 0, s_px = 0;
std::vector<lv_area_t> s_dirty;
void invalidate(lv_area_t a) {
  if (!clip(a) || (s_bench & 2)) return;
  gapRects(a, s_dirty);
}

bool overTile(const lv_area_t& a) {
  for (const lv_area_t& r : L.rects)
    if (r.x1 <= a.x2 && r.x2 >= a.x1 && r.y1 <= a.y2 && r.y2 >= a.y1) return true;
  return false;
}

// Joins areas while the joined one costs less to redraw than both, then asks LVGL.
void flushDirty() {
  std::vector<lv_area_t>& d = s_dirty;
  for (bool joined = true; joined && d.size() > 1;) {
    joined = false;
    for (size_t i = 0; i < d.size() && !joined; i++)
      for (size_t j = i + 1; j < d.size(); j++) {
        const lv_area_t u = {LV_MIN(d[i].x1, d[j].x1), LV_MIN(d[i].y1, d[j].y1), LV_MAX(d[i].x2, d[j].x2),
                             LV_MAX(d[i].y2, d[j].y2)};
        if ((int)lv_area_get_size(&u) - (int)lv_area_get_size(&d[i]) - (int)lv_area_get_size(&d[j]) > AREA_PX ||
            overTile(u))
          continue;
        d[i] = u;
        d.erase(d.begin() + j);
        joined = true;
        break;
      }
  }
  if (!d.empty() && L.page >= 0) {
    lv_area_t rc;
    lv_obj_get_coords(s_pages[L.page].root, &rc);
    for (lv_area_t g : d) {
      s_px += lv_area_get_size(&g);
      lv_area_move(&g, rc.x1, rc.y1);
      lv_obj_invalidate_area(s_pages[L.page].root, &g);
      s_areas++;
    }
  }
  d.clear();
}

void restoreArea(lv_area_t a) {
  if (!clip(a)) return;
  for (int y = a.y1; y <= a.y2; y++) {
    const uint16_t* s = sharpRow(y);
    uint16_t* d = glassRow(y);
    for (int x = a.x1; x <= a.x2; x++)
      if (!covered(x, y)) d[x] = s[x];
  }
}

// ---- water ----

int16_t s_win[257];  // raised cosine 0..256, so the ripple fades out at the sides

int halfAt(int y) {
  const float m = std::min(L.w, L.h), t = (float)(y - L.art.horizon) / L.h;
  return (int)((.03f + .20f * t) * 2.0f * m);
}

// One frame of the water: every row of the glitter paths shifted sideways,
// read from the unmoved picture. amp 0 puts the picture back.
void drawWater(uint32_t ms, float amp) {
  if (s_bench & 1) return;
  const float T = ms / 1000.0f, m = std::min(L.w, L.h);
  const int hor = L.art.horizon;
  for (int b = 0; b < L.art.bodies; b++) {
    const int bx = (int)L.art.body[b].x;
    if (L.art.body[b].path < .05f) continue;
    for (int y = hor + 1; y < L.h; y++) {
      const float t = (float)(y - hor) / L.h, q = 11 * logf(1 + t * 14);
      const float fade = std::min(1.0f, (y - hor) / 6.0f);
      const float off = amp * fade * (.35f + 9 * t) * m / 600 *
                        (.62f * sinf(q * 1.9f - T * 1.6f) + .38f * sinf(q * 3.3f + T * 1.05f + 1.3f));
      const int half = halfAt(y), x0 = std::max(0, bx - half), x1 = std::min(L.w - 1, bx + half);
      const int offQ = (int)lrintf(off * 16);  // 1/16 px
      const uint32_t step = (256u << 16) / (half + 1);
      const uint16_t* s = sharpRow(y);
      uint16_t* d = glassRow(y);
      const uint8_t* mrow = L.mask + y * L.stride;
      for (int x = x0; x <= x1; x++) {
        if (mrow[x >> 3] >> (x & 7) & 1) continue;
        const int wv = s_win[((uint32_t)std::abs(x - bx) * step) >> 16];
        const int sx = x * 16 + ((offQ * wv) >> 8);
        int i = sx >> 4;
        const int f = sx & 15;
        if (i < 0) i = 0;
        if (i >= L.w - 1) i = L.w - 2;
        if (!f) {
          d[x] = s[i];
          continue;
        }
        // Red and blue in one word, green in another: no carry between them.
        const uint32_t a = s[i], c = s[i + 1];
        d[x] = (uint16_t)(((((a & 0xF81F) * (16 - f) + (c & 0xF81F) * f) >> 4) & 0xF81F) |
                          ((((a & 0x07E0) * (16 - f) + (c & 0x07E0) * f) >> 4) & 0x07E0));
      }
    }
  }
}

void invalidateWater() {
  const int hor = L.art.horizon;
  for (int b = 0; b < L.art.bodies; b++) {
    if (L.art.body[b].path < .05f) continue;
    const int bx = (int)L.art.body[b].x;
    for (int y0 = hor + 1; y0 < L.h; y0 += 32) {
      const int y1 = std::min(L.h - 1, y0 + 31), half = halfAt(y1);
      invalidate({bx - half, y0, bx + half, y1});
    }
  }
}

// ---- stars ----

int luma(int r, int g, int b) { return (r * 77 + g * 150 + b * 29) >> 8; }

void findStars() {
  L.stars.clear();
  if (L.art.night < .2f) return;
  struct Cand { int16_t x, y; int16_t d; };
  std::vector<Cand> cands;
  int end = L.art.skyEnd;
  if (L.art.horizon > 0) end = std::min(end, L.art.horizon - 2);
  for (int y = 3; y < end - 2; y++) {
    const uint16_t *s0 = sharpRow(y - 2), *s1 = sharpRow(y - 1), *s = sharpRow(y), *s3 = sharpRow(y + 1), *s4 = sharpRow(y + 2);
    for (int x = 3; x < L.w - 3; x++) {
      if (covered(x, y)) continue;
      int r, g, b;
      unpack(s[x], r, g, b);
      const int c = luma(r, g, b);
      int n1 = 0, ring = 0;
      for (uint16_t q : {s[x - 1], s[x + 1], s1[x], s3[x]}) {
        unpack(q, r, g, b);
        n1 = std::max(n1, luma(r, g, b));
      }
      if (c < n1 + 6) continue;
      for (uint16_t q : {s[x - 2], s[x + 2], s0[x], s4[x]}) {
        unpack(q, r, g, b);
        ring += luma(r, g, b);
      }
      const int d = c - ring / 4;
      if (d >= 14) cands.push_back({(int16_t)x, (int16_t)y, (int16_t)d});
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d > b.d; });
  const uint32_t now = millis();
  for (const Cand& k : cands) {
    if ((int)L.stars.size() >= MAX_STARS) break;
    Star st = {};
    st.x = k.x;
    st.y = k.y;
    int r, g, b;
    unpack(sharpRow(k.y)[k.x], r, g, b);
    st.c[0] = r, st.c[1] = g, st.c[2] = b;
    int br = 0, bg = 0, bb = 0;
    for (uint16_t q : {sharpRow(k.y)[k.x - 2], sharpRow(k.y)[k.x + 2], sharpRow(k.y - 2)[k.x], sharpRow(k.y + 2)[k.x]}) {
      unpack(q, r, g, b);
      br += r, bg += g, bb += b;
    }
    st.bg[0] = br / 4, st.bg[1] = bg / 4, st.bg[2] = bb / 4;
    st.shown = sharpRow(k.y)[k.x];
    st.phase = rndf() * 6.283f;
    st.rate = 1.6f + rndf() * 2.8f;
    st.sparkler = (int)L.stars.size() < SPARKLERS;
    st.sparkAt = now + 1500 + rnd() % 9000;
    L.stars.push_back(st);
  }
}

int armLen() { return std::max(2, (int)lrintf(std::min(L.w, L.h) / 600.0f * 4)); }

// Every redrawn area has a cost of its own: sparkles each call, the plain
// twinkle round robin, a few stars a call.
size_t s_starAt = 0;
void drawStars(uint32_t ms) {
  const float T = ms / 1000.0f, night = L.art.night;
  const int arm = armLen();
  const size_t n = L.stars.size();
  int budget = TWINKLES;
  for (size_t k = 0; k < n + SPARKLERS; k++) {
    // First the sparklers, then the round robin from where it stopped.
    const size_t idx = k < SPARKLERS ? k : (s_starAt + k - SPARKLERS) % n;
    if (idx >= n) continue;
    Star& st = L.stars[idx];
    if (k >= SPARKLERS && (st.sparkler || budget <= 0)) {
      if (budget <= 0) {
        s_starAt = idx;
        break;
      }
      continue;
    }
    float f = 1 + .6f * night * sinf(st.phase + T * st.rate);
    float spark = 0;
    if (st.sparkler) {
      const int32_t into = (int32_t)(ms - st.sparkAt);
      if (into >= 0 && into < 900) {
        const float p = into / 900.0f, e = sinf((float)M_PI * p);
        spark = e * e * night;
      } else if (into >= 900) {
        st.sparkAt = ms + 4000 + rnd() % 8000;
      }
    }
    f += 1.4f * spark;
    const uint16_t q = pack(st.bg[0] + (int)((st.c[0] - st.bg[0]) * f), st.bg[1] + (int)((st.c[1] - st.bg[1]) * f),
                            st.bg[2] + (int)((st.c[2] - st.bg[2]) * f));
    if (spark > .01f || st.sparkDrawn) {
      // The cross of a sparkle, then the plain star once it is over.
      const lv_area_t box = {st.x - arm, st.y - arm, st.x + arm, st.y + arm};
      lv_area_t c = box;
      if (!clip(c)) continue;
      restoreArea(c);
      if (spark > .01f) {
        const int lit = L.keep * 235 >> 8;
        for (int d = 1; d <= arm; d++) {
          const float k = spark * .85f * powf(1 - (float)d / (arm + 1), 1.5f);
          const int pts[4][2] = {{st.x - d, st.y}, {st.x + d, st.y}, {st.x, st.y - d}, {st.x, st.y + d}};
          for (const auto& pt : pts) {
            if (pt[0] < 0 || pt[1] < 0 || pt[0] >= L.w || pt[1] >= L.h || covered(pt[0], pt[1])) continue;
            uint16_t& px = glassRow(pt[1])[pt[0]];
            int r, g, b;
            unpack(px, r, g, b);
            px = pack(r + (int)((lit - r) * k), g + (int)((lit - g) * k), b + (int)(((L.keep * 250 >> 8) - b) * k));
          }
        }
      }
      glassRow(st.y)[st.x] = q;
      st.shown = q;
      st.sparkDrawn = spark > .01f;
      invalidate(c);
      continue;
    }
    if (k < SPARKLERS || q == st.shown) continue;
    glassRow(st.y)[st.x] = q;
    st.shown = q;
    invalidate({st.x, st.y, st.x, st.y});
    budget--;
  }
}

void restoreStars() {
  const int arm = armLen();
  for (Star& st : L.stars) {
    restoreArea({st.x - arm, st.y - arm, st.x + arm, st.y + arm});
    st.shown = sharpRow(st.y)[st.x];
    st.sparkDrawn = false;
  }
}

// ---- shooting star ----

void startMeteor(uint32_t ms) {
  Meteor& mt = L.met;
  const float m = std::min(L.w, L.h);
  int end = L.art.skyEnd;
  if (L.art.horizon > 0) end = std::min(end, L.art.horizon);
  mt.sx = L.w * (.12f + .76f * rndf());
  mt.sy = HEADER_H + (end - HEADER_H) * (.05f + .35f * rndf());
  const float ang = (18 + 17 * rndf()) * (float)M_PI / 180, dir = mt.sx < L.w / 2 ? 1 : -1;
  mt.dx = cosf(ang) * dir;
  mt.dy = sinf(ang);
  mt.len = m * (.35f + .2f * rndf());
  mt.tail = m * (.10f + .06f * rndf());
  mt.start = ms;
  mt.dur = 650 + rnd() % 450;
  mt.on = true;
  mt.drawn = false;
}

void drawMeteor(uint32_t ms) {
  Meteor& mt = L.met;
  const float p = (float)(ms - mt.start) / mt.dur;
  if (mt.drawn) restoreArea(mt.last);
  lv_area_t dirty = mt.last;
  const bool had = mt.drawn;
  mt.drawn = false;
  if (p >= 1) {
    mt.on = false;
    if (had) invalidate(dirty);
    return;
  }
  const float env = std::min(1.0f, p / .12f) * (1 - std::max(0.0f, (p - .7f) / .3f));
  const float hx = mt.sx + mt.dx * mt.len * p, hy = mt.sy + mt.dy * mt.len * p;
  const float tl = std::min(mt.tail, mt.len * p + 1);
  const float tx = hx - mt.dx * tl, ty = hy - mt.dy * tl;
  lv_area_t box = {(int32_t)floorf(std::min(hx, tx)) - 3, (int32_t)floorf(std::min(hy, ty)) - 3,
                   (int32_t)ceilf(std::max(hx, tx)) + 3, (int32_t)ceilf(std::max(hy, ty)) + 3};
  if (!clip(box)) {
    if (had) invalidate(dirty);
    return;
  }
  const int cr = L.keep * 215 >> 8, cg = L.keep * 230 >> 8, cb = L.keep * 255 >> 8;
  for (int y = box.y1; y <= box.y2; y++) {
    uint16_t* d = glassRow(y);
    for (int x = box.x1; x <= box.x2; x++) {
      if (covered(x, y)) continue;
      const float vx = x + .5f - tx, vy = y + .5f - ty;
      const float u = (vx * mt.dx + vy * mt.dy) / tl, perp = fabsf(vx * mt.dy - vy * mt.dx);
      float k = 0;
      if (u >= 0 && u <= 1) k = u * u * std::max(0.0f, 1 - perp / 1.3f);
      const float hdx = x + .5f - hx, hdy = y + .5f - hy;
      k = std::max(k, std::max(0.0f, 1 - sqrtf(hdx * hdx + hdy * hdy) / 2.4f));
      k = std::min(1.0f, k * env);
      if (k < .02f) continue;
      int r, g, b;
      unpack(d[x], r, g, b);
      d[x] = wallDither565(r + (int)((cr - r) * k), g + (int)((cg - g) * k), b + (int)((cb - b) * k), x, y);
    }
  }
  mt.last = box;
  mt.drawn = true;
  if (had) {
    dirty.x1 = LV_MIN(dirty.x1, box.x1);
    dirty.y1 = LV_MIN(dirty.y1, box.y1);
    dirty.x2 = LV_MAX(dirty.x2, box.x2);
    dirty.y2 = LV_MAX(dirty.y2, box.y2);
  } else {
    dirty = box;
  }
  invalidate(dirty);
}

// ---- state ----

void restoreAll() {
  if (L.waterDrawn) {
    drawWater(0, 0);
    invalidateWater();
  }
  restoreStars();
  if (L.met.drawn) {
    restoreArea(L.met.last);
    invalidate(L.met.last);
  }
  L.met = {};
  L.waterDrawn = false;
}

// The page still shows the bake these details were drawn into.
bool stillOurs() {
  if (L.page < 0 || L.page >= s_count) return false;
  Page& p = s_pages[L.page];
  return p.root && p.glass == L.glass && wallGeneration() == L.gen && wallSharp() &&
         lv_obj_get_style_bg_image_src(p.root, LV_PART_MAIN) == p.glass;
}

void stop() {
  if (stillOurs() && L.mask) {
    restoreAll();
    flushDirty();
    lv_image_cache_drop(L.glass);
    s_animPainted = true;
  }
  s_dirty.clear();
  L.page = -1;
  L.glass = nullptr;
  L.stars.clear();
  L.met = {};
  L.waterDrawn = false;
  L.key.clear();
}

}  // namespace

void liveTick() {
  if (!s_animDue) return;
  const uint32_t ms = s_animMs;
  const bool showing = s_fx && s_theme == THEME_GLASS && getScreenState() == SCREEN_DECK && tilesShowing() &&
                       sh.active >= 0 && sh.active < s_count;
  ArtTime at;
  int dim = 0;
  const int scene = showing ? wallLiveScene(at, dim) : -1;
  if (scene < 0) {
    if (L.page >= 0 && (!showing || !stillOurs())) stop();
    return;
  }
  Page& p = s_pages[sh.active];
  const bool ready = p.root && p.grid && p.glass && !p.wallBake && !p.glassKey.empty() && !glassBakeRunning() &&
                     lv_obj_get_style_bg_image_src(p.root, LV_PART_MAIN) == p.glass && wallSharp() &&
                     wallSharp()->header.w == p.glass->header.w && wallSharp()->header.h == p.glass->header.h;
  if (L.page >= 0 && (L.page != sh.active || !stillOurs() || L.scene != scene || !ready)) stop();
  if (!ready) return;
  if (L.page >= 0 && L.key != p.glassKey) {
    // Tiles came or went: back to the plain picture on the new layout, then start over.
    if (!buildMask(p)) return;
    restoreAll();
    flushDirty();
    s_animPainted = true;
    L.page = -1;
  }
  if (ms - L.lastMs < s_everyMs - 4u) return;
  L.lastMs = ms;
  const uint32_t t0 = micros();
  s_areas = s_px = 0;
  if (L.page < 0) {
    for (int i = 0; i <= 256; i++) s_win[i] = (int16_t)lrintf(128 + 128 * cosf((float)M_PI * i / 256));
    L.page = sh.active;
    L.glass = p.glass;
    L.gen = wallGeneration();
    L.scene = scene;
    L.w = p.glass->header.w;
    L.h = p.glass->header.h;
    L.keep = (100 - dim) * 256 / 100;
    L.key = p.glassKey;
    artLive(scene, L.w, L.h, at, L.art);
    if (!buildMask(p)) {
      L.page = -1;
      return;
    }
    findStars();
    L.nextMeteor = ms + 2000 + rnd() % 3000;
    Serial.printf("Deck: live details on page %d, scene %d, %d stars, night %.2f, %u ms\n", L.page, scene,
                  (int)L.stars.size(), L.art.night, (unsigned)((micros() - t0) / 1000));
  }
  // Water and stars take turns: each at half the frame rate, a frame does one.
  const bool waterTurn = !(L.frame++ & 1);
  if ((s_fx & FX_WATER) && L.art.horizon > 0) {
    if (waterTurn) {
      drawWater(ms, 1);
      invalidateWater();
      L.waterDrawn = true;
    }
  } else if (L.waterDrawn) {
    drawWater(0, 0);
    invalidateWater();
    L.waterDrawn = false;
  }
  if ((s_fx & FX_STARS) && !waterTurn) drawStars(ms);
  if ((s_fx & FX_METEOR) && L.art.night > .5f && !L.met.on && (int32_t)(ms - L.nextMeteor) >= 0) startMeteor(ms);
  if (L.met.on) {
    drawMeteor(ms);
    if (!L.met.on) L.nextMeteor = ms + 1000u * (s_meteorMin + rnd() % (s_meteorMax - s_meteorMin + 1));
  }
  flushDirty();
  if (s_areas) {
    lv_image_cache_drop(L.glass);
    s_animPainted = true;
    s_sparkPaintMs = millis();
  }
  const uint32_t us = micros() - t0;
  s_liveFrames++;
  s_usSum += us;
  s_usMax = std::max(s_usMax, us);
  s_areasMax = std::max(s_areasMax, s_areas);
  s_pxMax = std::max(s_pxMax, s_px);
}

}  // namespace deckui

using namespace deckui;

void deckLiveJson(JsonObject o) {
  o["fx"] = s_fx | s_bench << 3;
  o["every_ms"] = s_everyMs;
  o["meteor_min_s"] = s_meteorMin;
  o["meteor_max_s"] = s_meteorMax;
  o["page"] = L.page;
  o["scene"] = L.scene;
  o["night"] = L.art.night;
  o["stars"] = (int)L.stars.size();
  o["rects"] = (int)L.rects.size();
  o["frames"] = s_liveFrames;
  o["us_avg"] = s_liveFrames ? s_usSum / s_liveFrames : 0;
  o["us_max"] = s_usMax;
  o["areas_max"] = s_areasMax;
  o["px_max"] = s_pxMax;
  s_liveFrames = s_usSum = s_usMax = s_areasMax = s_pxMax = 0;
}

void deckLiveSet(int fx, int everyMs, int meteorMin, int meteorMax) {
  if (fx >= 0) s_fx = fx & 7;
  if (fx >= 0) s_bench = fx >> 3 & 3;
  if (everyMs >= 16) s_everyMs = everyMs;
  if (meteorMin > 0) s_meteorMin = meteorMin;
  if (meteorMax >= s_meteorMin) s_meteorMax = meteorMax;
  stop();
}
