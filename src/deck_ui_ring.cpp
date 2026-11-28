// Orbit ring geometry and drawing, shared by the Orbit clock and the Orbit
// on/off effect.
#include "deck_ui_ring.h"

#include <math.h>

#include <utility>

#include "board.h"
#include "deck_ui_clock.h"

namespace deckui {

constexpr int TAIL_STEP_PX = 3;  // tail fades in steps this long

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

void ringPoint(const Ring& c, float d, float& x, float& y) {
  d = clampf(d, 0, c.len);
  for (const RingSeg& s : c.seg) {
    if (d > s.at + s.len && &s != &c.seg[8]) continue;
    const float t = s.len > 0 ? clampf((d - s.at) / s.len, 0, 1) : 0;
    if (!s.arc) {
      x = s.x0 + (s.x1 - s.x0) * t;
      y = s.y0 + (s.y1 - s.y0) * t;
    } else {
      const float a = (s.a0 + 90 * t) * (float)M_PI / 180;
      x = s.x0 + c.rc * cosf(a);
      y = s.y0 + c.rc * sinf(a);
    }
    return;
  }
}

// Box of the ring's centre line between d0 and d1 within one piece (a quarter
// circle never turns back), object coordinates, grown by g.
static lv_area_t segBox(const Ring& c, float d0, float d1, int g) {
  float xa, ya, xb, yb;
  ringPoint(c, d0, xa, ya);
  ringPoint(c, d1, xb, yb);
  if (xa > xb) std::swap(xa, xb);
  if (ya > yb) std::swap(ya, yb);
  return {(int32_t)floorf(xa) - g, (int32_t)floorf(ya) - g, (int32_t)ceilf(xb) + g, (int32_t)ceilf(yb) + g};
}

void ringInvalidate(lv_obj_t* obj, const Ring& c, float d0, float d1) {
  if (d0 < 0) {
    ringInvalidate(obj, c, c.len + d0, c.len);
    d0 = 0;
  }
  d1 = LV_MIN(d1, c.len);
  if (d1 <= d0) return;
  lv_area_t o;
  lv_obj_get_coords(obj, &o);
  const int g = (int)lroundf(c.lw * RING_GLOW) + 1;  // the head's glow
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b < a || s.len <= 0) continue;
    lv_area_t r = segBox(c, a, b, g);
    lv_area_move(&r, o.x1, o.y1);
    lv_obj_invalidate_area(obj, &r);
  }
}

static bool intersect(lv_area_t* out, const lv_area_t* a, const lv_area_t* b) {
  *out = {LV_MAX(a->x1, b->x1), LV_MAX(a->y1, b->y1), LV_MIN(a->x2, b->x2), LV_MIN(a->y2, b->y2)};
  return out->x1 <= out->x2 && out->y1 <= out->y2;
}

// Pixels of a straight side's band between a and b (screen), false when empty.
static bool sideRect(const Ring& c, const RingSeg& s, float a, float b, int w, int os, int ox, int oy,
                     lv_area_t& r) {
  float xa, ya, xb, yb;
  ringPoint(c, a, xa, ya);
  ringPoint(c, b, xb, yb);
  if (s.side == 0 || s.side == 2) {
    r.x1 = (int32_t)lroundf(LV_MIN(xa, xb));
    r.x2 = (int32_t)lroundf(LV_MAX(xa, xb)) - 1;
    r.y1 = s.side == 0 ? os : c.h - os - w;
    r.y2 = r.y1 + w - 1;
  } else {
    r.y1 = (int32_t)lroundf(LV_MIN(ya, yb));
    r.y2 = (int32_t)lroundf(LV_MAX(ya, yb)) - 1;
    r.x1 = s.side == 3 ? os : c.w - os - w;
    r.x2 = r.x1 + w - 1;
  }
  lv_area_move(&r, ox, oy);
  return r.x2 >= r.x1 && r.y2 >= r.y1;
}

// Screen pixels of corner s between t0 and t1 (shares of the quarter, on one side of
// its middle). Pieces meet on pixel edges: cut across x on the half next to the
// horizontal side and across y on the other one.
static lv_area_t cornerClip(const Ring& c, const RingSeg& s, float t0, float t1, int os, int ox, int oy) {
  const bool right = s.a0 == 270 || s.a0 == 0, bottom = s.a0 == 0 || s.a0 == 90;
  const int in = c.ro + c.rr;
  // Half-open ranges, object coordinates.
  int x0 = right ? c.w - in : os, x1 = right ? c.w - os : in;
  int y0 = bottom ? c.h - in : os, y1 = bottom ? c.h - os : in;
  float xa, ya, xb, yb, xm, ym;
  ringPoint(c, s.at + s.len * t0, xa, ya);
  ringPoint(c, s.at + s.len * t1, xb, yb);
  ringPoint(c, s.at + s.len * 0.5f, xm, ym);
  const bool columns = (t1 <= 0.5f) == (s.a0 == 270 || s.a0 == 90);
  if (columns) {
    x0 = LV_MAX(x0, (int)lroundf(LV_MIN(xa, xb)));
    x1 = LV_MIN(x1, (int)lroundf(LV_MAX(xa, xb)));
  } else {
    if (right) x0 = (int)lroundf(xm);
    else x1 = (int)lroundf(xm);
    auto cut = [&](float t, float y) { return t == 0.5f ? (bottom ? y1 : y0) : (int)lroundf(y); };
    const int ca = cut(t0, ya), cb = cut(t1, yb);
    y0 = LV_MIN(ca, cb);
    y1 = LV_MAX(ca, cb);
  }
  return {ox + x0, oy + y0, ox + x1 - 1, oy + y1 - 1};
}

// Ring band of width w between d0 and d1: sides as pixel rects, corners cut out of
// the whole band drawn as a border. A band wider or narrower than lw stays centred on it.
void ringStroke(lv_layer_t* L, const Ring& c, int ox, int oy, float d0, float d1, int w, lv_color_t col,
                       lv_opa_t opa) {
  if (d1 <= d0 || opa < 2 || w <= 0) return;
  const int os = c.ro + (c.lw - w) / 2;  // outer edge inset of this band
  const lv_area_t band = {ox + os, oy + os, ox + c.w - 1 - os, oy + c.h - 1 - os};
  lv_draw_border_dsc_t bd;
  lv_draw_border_dsc_init(&bd);
  bd.radius = c.rr + (c.ro - os);
  bd.width = w;
  bd.color = col;
  bd.opa = opa;
  if (d0 <= 0 && d1 >= c.len) {
    lv_draw_border(L, &bd, &band);
    return;
  }
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b <= a) continue;
    lv_area_t r;
    if (!s.arc) {
      if (sideRect(c, s, a, b, w, os, ox, oy, r)) fillRect(L, r, col, 0, opa);
      continue;
    }
    const float t0 = clampf((a - s.at) / s.len, 0, 1), t1 = clampf((b - s.at) / s.len, 0, 1);
    const lv_area_t keep = L->_clip_area;
    for (int half = 0; half < 2; half++) {
      const float u0 = LV_MAX(t0, half * 0.5f), u1 = LV_MIN(t1, 0.5f + half * 0.5f);
      if (u1 <= u0) continue;
      r = cornerClip(c, s, u0, u1, os, ox, oy);
      if (r.x2 < r.x1 || r.y2 < r.y1 || !intersect(&r, &r, &keep)) continue;
      L->_clip_area = r;
      lv_draw_border(L, &bd, &band);
    }
    L->_clip_area = keep;
  }
}

static void dot(lv_layer_t* L, float x, float y, float r, lv_color_t col, lv_opa_t opa) {
  const int ri = LV_MAX(1, (int)lroundf(r)), cx = (int)lroundf(x), cy = (int)lroundf(y);
  fillRect(L, {cx - ri, cy - ri, cx + ri, cy + ri}, col, LV_RADIUS_CIRCLE, opa);
}

// A gradient on the sides, steps on the arcs.
void ringTail(lv_layer_t* L, const Ring& c, int ox, int oy, float d0, float d1, lv_color_t col, lv_opa_t lo,
              lv_opa_t hi, bool rev, int steps) {
  if (d1 <= d0) return;
  auto opa = [&](float d) {
    const float u = clampf((d - d0) / (d1 - d0), 0, 1);
    return (lv_opa_t)(lo + (hi - lo) * (rev ? 1 - u : u));
  };
  // A step takes the opacity of its end nearer the head.
  auto step = [&](float a, float b) { ringStroke(L, c, ox, oy, a, b, c.lw, col, opa(rev ? a : b)); };
#if !defined(BOARD_CLOCK_SMOOTH_TAIL)
  for (int k = 0; k < steps; k++) step(d0 + (d1 - d0) * k / steps, d0 + (d1 - d0) * (k + 1) / steps);
  return;
#endif
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b <= a) continue;
    if (s.arc) {
      const int n = LV_MAX(1, (int)((b - a) / TAIL_STEP_PX));
      for (int k = 0; k < n; k++) step(a + (b - a) * k / n, a + (b - a) * (k + 1) / n);
      continue;
    }
    lv_area_t r;
    if (!sideRect(c, s, a, b, c.lw, c.ro, ox, oy, r) || !areaHits(r, L->_clip_area)) continue;
    // The ring runs right on top and down on the right: there the far end is the bright one.
    const bool fwd = s.side == 0 || s.side == 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = s.side == 0 || s.side == 2 ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0] = {col, fwd ? opa(a) : opa(b), 0};
    d.bg_grad.stops[1] = {col, fwd ? opa(b) : opa(a), 255};
    lv_draw_rect(L, &d, &r);
  }
}

void ringHead(lv_layer_t* L, const Ring& c, int ox, int oy, float d, lv_color_t glow, lv_color_t core, lv_opa_t opa) {
  if (opa < 2) return;
  float x, y;
  ringPoint(c, d, x, y);
  dot(L, ox + x, oy + y, c.lw * RING_GLOW, glow, (lv_opa_t)(LV_OPA_50 * opa / 255));
  dot(L, ox + x, oy + y, LV_MAX(2.0f, c.lw * 1.1f), core, opa);
}

void ringBuild(Ring& c, int rad) {
  const int lw = LV_CLAMP(2, LV_MIN(c.w, c.h) / 45, 4), o = 0;  // on the tile's edge
  int R = LV_MAX(lw + 1, rad - o);
  R = LV_MIN(R, LV_MIN(c.w, c.h) / 2 - o - 1);
  c.lw = (int16_t)lw;
  c.ro = (int16_t)o;
  c.rr = (int16_t)R;
  c.rc = R - lw / 2.0f;
  // Arc centres on pixel edges, where LVGL puts a border's.
  const float L = o + R - 0.5f, T = L, Rt = c.w - 0.5f - o - R, B = c.h - 0.5f - o - R;
  const float e = (lw - 1) / 2.0f;  // centre line inside the band
  const float top = o + e, bot = c.h - 1 - o - e, left = o + e, right = c.w - 1 - o - e, mid = c.w / 2.0f;
  const float q = (float)M_PI / 2 * c.rc;
  c.seg[0] = {false, mid, top, Rt, top, 0, 0, Rt - mid, 0};
  c.seg[1] = {true, Rt, T, 0, 0, 270, 0, q, 0};
  c.seg[2] = {false, right, T, right, B, 0, 0, B - T, 1};
  c.seg[3] = {true, Rt, B, 0, 0, 0, 0, q, 0};
  c.seg[4] = {false, Rt, bot, L, bot, 0, 0, Rt - L, 2};
  c.seg[5] = {true, L, B, 0, 0, 90, 0, q, 0};
  c.seg[6] = {false, left, B, left, T, 0, 0, B - T, 3};
  c.seg[7] = {true, L, T, 0, 0, 180, 0, q, 0};
  c.seg[8] = {false, L, top, mid, top, 0, 0, mid - L, 0};
  float at = 0;
  for (RingSeg& s : c.seg) {
    s.len = LV_MAX(0.0f, s.len);
    s.at = at;
    at += s.len;
  }
  c.len = at;
  c.ext = (int16_t)LV_MAX(0, (int)lroundf(lw * RING_GLOW) - (int)(o + e));
}

}  // namespace deckui
