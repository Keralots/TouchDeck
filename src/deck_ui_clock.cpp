// Deck UI: clock tiles with a drawn style. One object over the whole tile draws
// the time; a frame invalidates only what moves in it. Roll and Orbit live here,
// the other styles are scenes (deck_ui_clock_*.cpp).
#include <math.h>
#include <string.h>
#include <sys/time.h>

#include <utility>

#include "board.h"
#include "deck_ui_clock.h"

namespace deckui {

constexpr uint32_t ROLL_MS = 620, ROLL_STAGGER_MS = 85, SLIDE_MS = 520, FLASH_MS = 650;
constexpr uint32_t AT_REST = 100000;  // el of a clock that is not changing
constexpr int TAIL_STEP_PX = 3;  // tail fades in steps this long
constexpr int TAIL_STEPS = 6;     // boards without BOARD_CLOCK_SMOOTH_TAIL
constexpr float TAIL_PART = 0.14f;  // comet tail, share of the ring

// One piece of the ring: a straight side or a quarter circle.
// Points run on the band's centre line; drawing uses whole pixels so the
// straight sides and the arcs meet without a step.
struct RingSeg {
  bool arc;
  float x0, y0, x1, y1;  // line ends; an arc's centre is x0/y0 (a whole pixel)
  int16_t a0;            // arc start, degrees (0 = right, clockwise)
  float at, len;         // start along the ring, length
  uint8_t side;          // lines: 0 top, 1 right, 2 bottom, 3 left
};

struct ClockFx {
  lv_obj_t* obj;
  bool orbit;
  ClockScene* scene;  // the style when it is one
  bool sceneFits;     // else Roll
  bool tick;
  bool hasColor;
  uint32_t color, accent;
  char shown[6], from[6];  // "HH:MM", one-digit hours as " H"
  char ap[3];
  char date[20];
  bool anim;
  uint32_t t0, el;  // change start on the animation clock; ms into it as of the last tick
  bool colonLit;
  // Layout in object coordinates.
  const lv_font_t *f, *df, *uf;
  int16_t w, h;
  int16_t cell, colonW, tw, tx, ty;  // digit cell, colon cell, time width, time line box
  int16_t inkTop, inkH, colonDy, apX, apY;
  int16_t dateY;  // date line box top, -1 = no date
  lv_area_t timeArea;
  // Orbit.
  RingSeg seg[9];
  float len;
  float mark[12][2];  // a mark every 5 s
  int16_t lw, ro, rr;  // band width, outer edge inset, arc outer radius
  float rc;            // centre line radius
  int32_t head;    // px along the ring as last drawn, -1 = none yet
  int32_t minute;  // epoch minute last applied while shown
};

void clockFxFree(ClockFx* c) {
  if (!c) return;
  if (c->scene) {
    c->scene->~ClockScene();
    lv_free(c->scene);
  }
  lv_free(c);
}

// --- Shared by the styles ----------------------------------------------------------

const uint8_t CLOCK_DOTS[10][7] = {{14, 17, 17, 17, 17, 17, 14}, {4, 12, 4, 4, 4, 4, 14},  {14, 17, 1, 2, 4, 8, 31},
                                   {14, 17, 1, 6, 1, 17, 14},    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
                                   {6, 8, 16, 22, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},   {14, 17, 17, 14, 17, 17, 14},
                                   {14, 17, 17, 15, 1, 2, 12}};

const lv_font_t* const CLOCK_FONT[CLOCK_FONTS] = {&deck_font_clock_72, &deck_font_clock_56, &lv_font_montserrat_40,
                                                  &deck_font_28, &deck_font_20, &deck_font_14};

int clockCellW(const lv_font_t* f) {
  int w = 0;
  for (char d = '0'; d <= '9'; d++) w = LV_MAX(w, (int)lv_font_get_glyph_width(f, d, 0));
  return w;
}

int clockColonDy(const lv_font_t* f) {
  lv_font_glyph_dsc_t d = {}, c = {};
  if (!lv_font_get_glyph_dsc(f, &d, '0', 0) || !lv_font_get_glyph_dsc(f, &c, ':', 0)) return 0;
  return (c.ofs_y * 2 + c.box_h - d.ofs_y * 2 - d.box_h) / 2;
}

bool Dirty::flush(lv_obj_t* o) const {
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  for (int k = 0; k < n; k++) {
    lv_area_t r = a[k];
    lv_area_move(&r, c.x1, c.y1);
    lv_obj_invalidate_area(o, &r);
  }
  return n > 0;
}

void fillRect(lv_layer_t* L, const lv_area_t& a, lv_color_t col, int radius, lv_opa_t opa) {
  if (!areaHits(a, L->_clip_area)) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = col;
  d.bg_opa = opa;
  d.radius = radius;
  lv_draw_rect(L, &d, &a);
}

void drawGlyph(lv_layer_t* L, const lv_font_t* f, char ch, int x, int y, int w, lv_color_t col, lv_opa_t opa) {
  if (ch == ' ' || opa < 2) return;
  const lv_area_t a = {x, y, x + w - 1, y + lineH(f) - 1};
  if (!areaHits(a, L->_clip_area)) return;
  const char s[2] = {ch, 0};
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = s;
  d.text_local = 1;
  d.font = f;
  d.color = col;
  d.opa = opa;
  d.align = LV_TEXT_ALIGN_CENTER;
  lv_draw_label(L, &d, &a);
}

// The largest font that leaves room for the motion around the time.
bool DigitRow::fit(int w, int h) {
  f = nullptr;
  for (const lv_font_t* k : CLOCK_FONT) {
    int t, ink;
    digitInk(k, t, ink);
    const int cw = clockCellW(k), colon = lv_font_get_glyph_width(k, ':', 0);
    if (ink > h * 30 / 100 || 4 * cw + colon > w * 86 / 100) continue;
    f = k;
    cell = (int16_t)cw;
    colonW = (int16_t)colon;
    inkTop = (int16_t)t;
    inkH = (int16_t)ink;
    colonDy = (int16_t)clockColonDy(k);
    return true;
  }
  return false;
}

void DigitRow::place(int w, int y) {
  top = (int16_t)y;
  int x = (w - 4 * cell - colonW) / 2;
  for (int i = 0; i < 5; i++) {
    xs[i] = (int16_t)x;
    x += i == 2 ? colonW : cell;
  }
}

void DigitRow::box(int i, float& x1, float& y1, float& x2, float& y2) const {
  x1 = xs[i] + cell * 0.08f;
  x2 = xs[i] + cell * 0.92f;
  y1 = top;
  y2 = top + inkH;
}

int DigitRow::points(int i, char v, float* px, float* py) const {
  if (v < '0' || v > '9') return 0;
  float x1, y1, x2, y2;
  box(i, x1, y1, x2, y2);
  const float gx = (x2 - x1) / 5, gy = (y2 - y1) / 7;
  int n = 0;
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 5; c++)
      if (clockDot(v, r, c)) px[n] = x1 + (c + 0.5f) * gx, py[n] = y1 + (r + 0.5f) * gy, n++;
  return n;
}

lv_area_t DigitRow::colonArea() const { return {xs[2], top - 4, xs[2] + colonW, top + inkH + 4}; }

void DigitRow::draw(lv_layer_t* L, int ox, int oy, int i, char ch, lv_color_t col) const {
  drawGlyph(L, f, ch, ox + xs[i] + (int)lroundf(offX[i]), oy + top - inkTop + (int)lroundf(offY[i]) + (i == 2 ? colonDy : 0),
            i == 2 ? colonW : cell, col);
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float easeOutBack(float t) {
  const float c = 1.5f, u = t - 1;
  return 1 + (c + 1) * u * u * u + c * u * u;
}
static float easeInOut(float t) { return t < 0.5f ? 2 * t * t : 1 - (2 - 2 * t) * (2 - 2 * t) / 2; }
static float easeOutCubic(float t) {
  const float u = 1 - t;
  return 1 - u * u * u;
}

// --- Ring ----------------------------------------------------------------------

static void ringPoint(const ClockFx& c, float d, float& x, float& y) {
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
static lv_area_t segBox(const ClockFx& c, float d0, float d1, int g) {
  float xa, ya, xb, yb;
  ringPoint(c, d0, xa, ya);
  ringPoint(c, d1, xb, yb);
  if (xa > xb) std::swap(xa, xb);
  if (ya > yb) std::swap(ya, yb);
  return {(int32_t)floorf(xa) - g, (int32_t)floorf(ya) - g, (int32_t)ceilf(xb) + g, (int32_t)ceilf(yb) + g};
}

static void ringInvalidate(const ClockFx& c, float d0, float d1) {
  if (d0 < 0) {
    ringInvalidate(c, c.len + d0, c.len);
    d0 = 0;
  }
  d1 = LV_MIN(d1, c.len);
  if (d1 <= d0) return;
  lv_area_t o;
  lv_obj_get_coords(c.obj, &o);
  const int g = c.lw * 3 + 2;  // the head's glow
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b < a || s.len <= 0) continue;
    lv_area_t r = segBox(c, a, b, g);
    lv_area_move(&r, o.x1, o.y1);
    lv_obj_invalidate_area(c.obj, &r);
  }
}

static bool intersect(lv_area_t* out, const lv_area_t* a, const lv_area_t* b) {
  *out = {LV_MAX(a->x1, b->x1), LV_MAX(a->y1, b->y1), LV_MIN(a->x2, b->x2), LV_MIN(a->y2, b->y2)};
  return out->x1 <= out->x2 && out->y1 <= out->y2;
}

// Pixels of a straight side's band between a and b (screen), false when empty.
static bool sideRect(const ClockFx& c, const RingSeg& s, float a, float b, int w, int os, int ox, int oy,
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

// Ring band of width w between d0 and d1: sides as pixel rects, corners as arcs
// on the same grid. A band wider or narrower than lw stays centred on it.
static void ringStroke(lv_layer_t* L, const ClockFx& c, int ox, int oy, float d0, float d1, int w, lv_color_t col,
                       lv_opa_t opa) {
  if (d1 <= d0 || opa < 2 || w <= 0) return;
  const lv_area_t& clip = L->_clip_area;
  const int os = c.ro + (c.lw - w) / 2;  // outer edge inset of this band
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b <= a) continue;
    if (!s.arc) {
      lv_area_t r;
      if (sideRect(c, s, a, b, w, os, ox, oy, r)) fillRect(L, r, col, 0, opa);
    } else {
      lv_area_t box = segBox(c, a, b, w + 1);
      lv_area_move(&box, ox, oy);
      if (!areaHits(box, clip)) continue;
      const int s0 = (int)lroundf(s.a0 + 90 * (a - s.at) / s.len), s1 = (int)lroundf(s.a0 + 90 * (b - s.at) / s.len);
      if (s1 <= s0) continue;
      lv_draw_arc_dsc_t d;
      lv_draw_arc_dsc_init(&d);
      d.center.x = ox + (int32_t)s.x0;
      d.center.y = oy + (int32_t)s.y0;
      d.radius = c.rr + (c.ro - os);
      d.width = w;
      d.start_angle = s0 % 360;
      d.end_angle = s1 % 360;
      d.color = col;
      d.opa = opa;
      lv_draw_arc(L, &d);
    }
  }
}

static void dot(lv_layer_t* L, float x, float y, float r, lv_color_t col, lv_opa_t opa) {
  const int ri = LV_MAX(1, (int)lroundf(r)), cx = (int)lroundf(x), cy = (int)lroundf(y);
  fillRect(L, {cx - ri, cy - ri, cx + ri, cy + ri}, col, LV_RADIUS_CIRCLE, opa);
}

// Comet tail from d0 (faint) to d1 (full): a gradient on the sides, steps on the arcs.
static void ringTail(lv_layer_t* L, const ClockFx& c, int ox, int oy, float d0, float d1, lv_color_t col) {
  if (d1 <= d0) return;
  auto opa = [&](float d) { return (lv_opa_t)(100 + 155 * clampf((d - d0) / (d1 - d0), 0, 1)); };
#if !defined(BOARD_CLOCK_SMOOTH_TAIL)
  for (int k = 0; k < TAIL_STEPS; k++) {
    const float b = d0 + (d1 - d0) * (k + 1) / TAIL_STEPS;
    ringStroke(L, c, ox, oy, d0 + (d1 - d0) * k / TAIL_STEPS, b, c.lw, col, opa(b));
  }
  return;
#endif
  for (const RingSeg& s : c.seg) {
    const float a = LV_MAX(d0, s.at), b = LV_MIN(d1, s.at + s.len);
    if (b <= a) continue;
    if (s.arc) {
      const int n = LV_MAX(1, (int)((b - a) / TAIL_STEP_PX));
      for (int k = 0; k < n; k++)
        ringStroke(L, c, ox, oy, a + (b - a) * k / n, a + (b - a) * (k + 1) / n, c.lw, col,
                   opa(a + (b - a) * (k + 1) / n));
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

static void drawRing(lv_layer_t* L, const ClockFx& c, int ox, int oy, lv_color_t text) {
  const lv_color_t acc = lv_color_hex(c.accent);
  ringStroke(L, c, ox, oy, 0, c.len, LV_MAX(1, c.lw / 2), text, LV_OPA_20);
  const float head = c.head < 0 ? 0 : (float)c.head;
  const float tail = LV_MIN(head, c.len * TAIL_PART);
  ringStroke(L, c, ox, oy, 0, head - tail, c.lw, acc, LV_OPA_40);
  ringTail(L, c, ox, oy, head - tail, head, acc);
  // The marks light up once passed.
  for (int k = 0; k < 12; k++) {
    const bool lit = c.len * k / 12 <= head;
    dot(L, ox + c.mark[k][0], oy + c.mark[k][1], c.lw * (k % 3 ? 0.6f : 0.95f), lit ? acc : text, lit ? LV_OPA_COVER : LV_OPA_50);
  }
  if (c.el < FLASH_MS) {
    const float e = 1 - (float)c.el / FLASH_MS;
    ringStroke(L, c, ox, oy, 0, c.len, c.lw + (int)(c.lw * 1.4f * e), acc, (lv_opa_t)(230 * e));
  }
  float x, y;
  ringPoint(c, head, x, y);
  dot(L, ox + x, oy + y, c.lw * 2.2f, acc, LV_OPA_50);
  dot(L, ox + x, oy + y, LV_MAX(2.0f, c.lw * 1.1f), lv_color_white(), LV_OPA_COVER);
}

// --- Time ----------------------------------------------------------------------

// One character in a cell, clipped to the cell's slot.
static void glyph(lv_layer_t* L, const lv_font_t* f, char ch, int x, int y, int w, const lv_area_t& slot,
                  lv_color_t col, lv_opa_t opa) {
  lv_area_t clip;
  if (!intersect(&clip, &slot, &L->_clip_area)) return;
  const lv_area_t keep = L->_clip_area;
  L->_clip_area = clip;
  drawGlyph(L, f, ch, x, y, w, col, opa);
  L->_clip_area = keep;
}

static lv_opa_t fade(float d, float span) { return (lv_opa_t)(255 * clampf(1 - fabsf(d) / span, 0, 1)); }

static void drawTime(lv_layer_t* L, const ClockFx& c, int ox, int oy, lv_color_t col) {
  // Changed digits roll in from the right one after another; Orbit slides them together.
  int rank[5] = {-1, -1, -1, -1, -1}, n = 0;
  const bool changing = c.el < AT_REST && c.from[0];
  for (int i = 4; i >= 0; i--)
    if (i != 2 && changing && c.shown[i] != c.from[i]) rank[i] = n++;
  const int m = c.inkH / 6 + 2;
  const int sy1 = oy + c.ty + c.inkTop - m, sy2 = oy + c.ty + c.inkTop + c.inkH + m;
  int x = ox + c.tx;
  const int y = oy + c.ty;
  for (int i = 0; i < 5; i++) {
    const int cw = i == 2 ? c.colonW : c.cell;
    const lv_area_t slot = {x, sy1, x + cw - 1, sy2};
    if (i == 2) {
      glyph(L, c.f, ':', x, y + c.colonDy, cw, slot, col, c.colonLit ? LV_OPA_COVER : LV_OPA_TRANSP);
    } else if (rank[i] < 0) {
      glyph(L, c.f, c.shown[i], x, y, cw, slot, col, LV_OPA_COVER);
    } else if (!c.orbit) {
      const float p = clampf(((float)c.el - rank[i] * (float)ROLL_STAGGER_MS) / ROLL_MS, 0, 1);
      const float travel = c.inkH * 1.25f, off = easeOutBack(p) * travel;
      glyph(L, c.f, c.from[i], x, y - (int)lroundf(off), cw, slot, col, fade(off, travel * 0.75f));
      glyph(L, c.f, c.shown[i], x, y + (int)lroundf(travel - off), cw, slot, col, fade(travel - off, travel * 0.75f));
    } else {
      const float e = easeInOut(clampf((float)c.el / SLIDE_MS, 0, 1)), travel = c.inkH * 0.5f;
      glyph(L, c.f, c.from[i], x, y - (int)lroundf(e * travel), cw, slot, col, (lv_opa_t)(255 * (1 - e)));
      glyph(L, c.f, c.shown[i], x, y + (int)lroundf((1 - e) * travel), cw, slot, col, (lv_opa_t)(255 * e));
    }
    x += cw;
  }
  if (c.ap[0]) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = c.ap;
    d.text_local = 1;
    d.font = c.uf;
    d.color = col;
    d.opa = LV_OPA_70;
    const lv_area_t a = {ox + c.apX, oy + c.apY, ox + c.apX + textW(c.ap, c.uf), oy + c.apY + lineH(c.uf) - 1};
    if (areaHits(a, L->_clip_area)) lv_draw_label(L, &d, &a);
  }
}

static void clockDraw(lv_event_t* e) {
  ClockFx* c = (ClockFx*)lv_event_get_user_data(e);
  if (!c || !c->f) return;
  lv_obj_t* o = lv_event_get_target_obj(e);
  lv_layer_t* L = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const lv_color_t text = lv_obj_get_style_text_color(o, LV_PART_MAIN);
  const lv_color_t ink = c->hasColor ? lv_color_hex(c->color) : text;
  if (c->sceneFits) {
    c->scene->draw(L, a.x1, a.y1, ink);
    return;
  }
  if (c->len > 0) drawRing(L, *c, a.x1, a.y1, text);
  drawTime(L, *c, a.x1, a.y1, ink);
  if (c->dateY >= 0) {
    const lv_area_t d = {a.x1, a.y1 + c->dateY, a.x2, a.y1 + c->dateY + lineH(c->df) - 1};
    if (!areaHits(d, L->_clip_area)) return;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.text = c->date;
    ld.text_local = 1;
    ld.font = c->df;
    ld.color = text;
    ld.opa = LV_OPA_70;
    ld.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(L, &ld, &d);
  }
}

// --- Tile ----------------------------------------------------------------------

void makeClockFx(TileRt& r, lv_obj_t* t) {
  ClockFx* c = (ClockFx*)lv_malloc_zeroed(sizeof(ClockFx));
  if (!c) return;
  r.cfx = c;
  c->obj = lv_obj_create(t);
  lv_obj_remove_style_all(c->obj);
  lv_obj_remove_flag(c->obj, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  c->orbit = r.def->clockStyle == CLOCK_ORBIT;
  switch (r.def->clockStyle) {  // Roll when there is no memory for it
    case CLOCK_BLOCKS: c->scene = blocksScene(); break;
    case CLOCK_BREAKOUT: c->scene = breakoutScene(); break;
    case CLOCK_ROCKS: c->scene = rocksScene(); break;
    default: break;
  }
  c->tick = r.def->clockTick;
  c->hasColor = r.def->hasColor;
  c->color = r.def->color;
  c->accent = r.def->hasChartColor ? r.def->chartColor : C_OK;
  c->head = -1;
  c->el = AT_REST;
  c->colonLit = true;
  c->dateY = -1;
  lv_obj_add_event_cb(c->obj, clockDraw, LV_EVENT_DRAW_MAIN, c);
}

static void buildRing(ClockFx& c, int rad) {
  const int lw = LV_CLAMP(2, LV_MIN(c.w, c.h) / 45, 4), o = 1 + lw / 2;
  int R = LV_MAX(lw + 1, rad - o);
  R = LV_MIN(R, LV_MIN(c.w, c.h) / 2 - o - 1);
  c.lw = (int16_t)lw;
  c.ro = (int16_t)o;
  c.rr = (int16_t)R;
  c.rc = R - (lw - 1) / 2.0f;
  // Arc centres on whole pixels; the band's outer edge is R from them.
  const float L = o + R, T = o + R, Rt = c.w - 1 - o - R, B = c.h - 1 - o - R;
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
  c.head = -1;
  for (int k = 0; k < 12; k++) ringPoint(c, c.len * k / 12, c.mark[k][0], c.mark[k][1]);
}

void fitClockFx(TileRt& r, int cw, int ch) {
  ClockFx* c = r.cfx;
  if (!c) return;
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  c->w = (int16_t)(cw + 2 * pad);
  c->h = (int16_t)(ch + 2 * pad);
  lv_obj_set_pos(c->obj, -pad, -pad);
  lv_obj_set_size(c->obj, c->w, c->h);
  c->sceneFits = c->scene && c->scene->fit(c->w, c->h, pad);
  int aw = cw, ah = ch;
  c->len = 0;
  if (c->orbit) {
    buildRing(*c, lv_obj_get_style_radius(r.obj, LV_PART_MAIN));
    const int keep = c->lw * 4 + 5;  // text stays clear of the ring and its head
    aw = LV_MIN(aw, c->w - 2 * keep);
    ah = LV_MIN(ah, c->h - 2 * keep);
  }
  const bool ampm = !netSettings.use24h;
  const lv_font_t* df = ch >= 120 ? &deck_font_20 : &deck_font_14;
  int dTop, dH;
  dateInk(df, dTop, dH);
  auto unitFont = [](const lv_font_t* f) { return lineH(f) >= 48 ? &deck_font_20 : &deck_font_14; };
  auto width = [&](const lv_font_t* f) {
    return 4 * clockCellW(f) + (int)lv_font_get_glyph_width(f, ':', 0) + (ampm ? 4 + textW("PM", unitFont(f)) : 0);
  };
  int best = -1, dated = -1;
  for (int i = 0; i < CLOCK_FONTS; i++) {
    int top, h;
    digitInk(CLOCK_FONT[i], top, h);
    if (width(CLOCK_FONT[i]) > aw || h > ah) continue;
    if (best < 0) best = i;
    if (h + LV_MAX(4, h / 5) + dH <= ah) {
      dated = i;
      break;
    }
  }
  if (best < 0) best = CLOCK_FONTS - 1;
  const bool date = dated >= 0 && (dated <= best + 1 || r.def->clockDate);
  const lv_font_t* f = CLOCK_FONT[date ? dated : best];
  c->f = f;
  c->df = df;
  c->uf = unitFont(f);
  c->cell = (int16_t)clockCellW(f);
  c->colonW = (int16_t)lv_font_get_glyph_width(f, ':', 0);
  c->tw = (int16_t)width(f);
  int top, h;
  digitInk(f, top, h);
  c->inkTop = (int16_t)top;
  c->inkH = (int16_t)h;
  const int gap = LV_MAX(4, h / 5);
  const int block = h + (date ? gap + dH : 0);
  const int bt = (c->h - block) / 2;
  c->ty = (int16_t)(bt - top);
  c->tx = (int16_t)((c->w - c->tw) / 2);
  c->dateY = date ? (int16_t)(bt + h + gap - dTop) : -1;
  c->colonDy = (int16_t)clockColonDy(f);
  c->apX = (int16_t)(c->tx + 4 * c->cell + c->colonW + 4);
  c->apY = (int16_t)(c->ty + lineH(f) - lineH(c->uf) - (lineH(f) - lineH(c->uf)) / 5);
  const int m = h / 6 + 2;
  c->timeArea = {c->tx, c->ty + top - m, c->tx + c->tw, c->ty + top + h + m};
  lv_obj_invalidate(c->obj);
}

void applyClockFx(TileRt& r, const char* h, const char* m, const char* ap, const char* date) {
  ClockFx* c = r.cfx;
  char t[6];
  snprintf(t, sizeof(t), "%2s:%s", h, m);
  if (!strcmp(t, c->shown) && !strcmp(ap, c->ap) && !strcmp(date, c->date)) return;
  if (c->sceneFits) {  // runs on its own; kept for a fall back to Roll
    memcpy(c->shown, t, sizeof(c->shown));
    snprintf(c->ap, sizeof(c->ap), "%s", ap);
    snprintf(c->date, sizeof(c->date), "%s", date);
    return;
  }
  const bool live = c->shown[0] && strcmp(t, c->shown) && c->f && r.page == sh.active && tilesShowing();
  memcpy(c->from, c->shown, sizeof(c->from));
  memcpy(c->shown, t, sizeof(c->shown));
  snprintf(c->ap, sizeof(c->ap), "%s", ap);
  snprintf(c->date, sizeof(c->date), "%s", date);
  c->anim = live;
  c->t0 = s_animMs;
  c->el = live ? 0 : AT_REST;
  lv_obj_invalidate(c->obj);
  markPageStale(r.page);
}

// "HH:MM" at epoch t (12 h: " H:MM"), blank before the time is synced.
static void clockText(time_t t, char* out) {
  if (!t) {
    memcpy(out, "  :  ", 6);
    return;
  }
  struct tm lt;
  localtime_r(&t, &lt);
  const int h = netSettings.use24h ? lt.tm_hour : (lt.tm_hour + 11) % 12 + 1;
  snprintf(out, 6, netSettings.use24h ? "%02d:%02d" : "%2d:%02d", h, lt.tm_min);
}

// The texts once a second: the time zone rules are slow.
static const ClockNow& clockNow(const timeval& tv, bool synced) {
  static ClockNow n = {};
  static time_t at = -1;
  if (at != tv.tv_sec) {
    at = tv.tv_sec;
    clockText(synced ? at : 0, n.cur);
    clockText(synced ? at + 60 : 0, n.next);
    n.sec = synced ? (int)(at % 60) : 0;
    n.minute = (int32_t)(at / 60);
  }
  return n;
}

static uint32_t changeMs(const ClockFx& c) {
  return c.orbit ? LV_MAX(SLIDE_MS, FLASH_MS) : ROLL_MS + 3 * ROLL_STAGGER_MS;
}

void clockFxTick() {
  if (!(advSettings.clock60 ? s_animFastDue : s_animDue)) return;
  const uint32_t now = s_animMs;
  const bool showing = tilesShowing();
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool synced = tv.tv_sec >= 1609459200;
  const bool lit = !synced || tv.tv_usec < 500000;
  bool painted = false;
  for (TileRt& r : s_tiles) {
    ClockFx* c = r.cfx;
    if (!c || r.off || !c->f) continue;
    const bool live = showing && r.page == sh.active;
    if (c->sceneFits) {
      ClockNow n = clockNow(tv, synced);
      n.ms = now;
      n.live = live;
      n.colon = lit || !live;
      if (c->scene->tick(c->obj, n)) {
        if (live) painted = true;
        else markPageStale(r.page);
      }
      continue;
    }
    lv_area_t o;
    lv_obj_get_coords(c->obj, &o);
    const bool on = lit || !live;
    if (on != c->colonLit) {
      c->colonLit = on;
      const int x = o.x1 + c->tx + 2 * c->cell;
      const lv_area_t a = {x, o.y1 + c->timeArea.y1, x + c->colonW - 1, o.y1 + c->timeArea.y2};
      lv_obj_invalidate_area(c->obj, &a);
      if (live) painted = true;
      else markPageStale(r.page);
    }
    if (!live) {
      if (c->anim) {
        c->anim = false;
        c->el = AT_REST;
        lv_obj_invalidate(c->obj);
        markPageStale(r.page);
      }
      continue;
    }
    // The header clock checks once a second: catch the minute on the frame it turns.
    if (synced && tv.tv_sec / 60 != c->minute) {
      c->minute = tv.tv_sec / 60;
      applyClock(r);
    }
    if (c->anim) {
      c->el = now - c->t0;
      if (c->el >= changeMs(*c)) {
        c->anim = false;
        c->el = AT_REST;
      }
      lv_area_t a = c->timeArea;
      lv_area_move(&a, o.x1, o.y1);
      lv_obj_invalidate_area(c->obj, &a);
      if (c->orbit) ringInvalidate(*c, 0, c->len);
      painted = true;
    }
    if (synced && c->len > 0) {
      float sec = (float)(tv.tv_sec % 60) + tv.tv_usec / 1e6f;
      if (c->tick) sec = floorf(sec) + easeOutCubic(clampf((sec - floorf(sec)) / 0.3f, 0, 1));
      const int32_t head = (int32_t)(sec / 60 * c->len);
      if (head != c->head) {
        const float tail = c->len * TAIL_PART;
        if (c->head < 0 || head < c->head || head - c->head > c->len / 4)
          ringInvalidate(*c, 0, c->len);
        else
          ringInvalidate(*c, c->head - tail - 2, head + 1);
        c->head = head;
        painted = true;
      }
    }
  }
  if (!painted) return;
  s_sparkPaintMs = millis();  // like a gliding chart: other pages snapshot rarely meanwhile
  s_animPainted = true;
}

}  // namespace deckui
