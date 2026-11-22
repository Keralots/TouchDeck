// Deck UI: clock tiles in the Roll and Orbit styles. One object over the whole
// tile draws the time; a frame invalidates only what moves in it.
#include <math.h>
#include <sys/time.h>

#include <utility>

#include "board.h"
#include "deck_ui_int.h"

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
  uint8_t style, want;  // style shown, style asked for (Blocks falls back to Roll on small tiles)
  BlocksRt* blk;
  ArcadeRt* arc;
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
  int16_t lw, ro, rr;  // band width, outer edge inset, arc outer radius
  float rc;            // centre line radius
  int32_t head;    // px along the ring as last drawn, -1 = none yet
  int32_t minute;  // epoch minute last applied while shown
};

void clockFxFree(ClockFx* c) {
  if (!c) return;
  blocksFree(c->blk);
  arcadeFree(c->arc);
  lv_free(c);
}

static bool fxShowing() {
  return !manualDisplayOff() && currentBacklightLevel() && !s_qp.open && !s_modalCard && !s_ed.root;
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

// Box of the ring between d0 and d1 within one piece, object coordinates, grown by g.
static lv_area_t segBox(const ClockFx& c, const RingSeg& s, float d0, float d1, int g) {
  float xa, ya, xb, yb;
  if (s.arc) {
    xa = s.x0 - c.rr, xb = s.x0 + c.rr, ya = s.y0 - c.rr, yb = s.y0 + c.rr;
  } else {
    ringPoint(c, d0, xa, ya);
    ringPoint(c, d1, xb, yb);
    if (xa > xb) std::swap(xa, xb);
    if (ya > yb) std::swap(ya, yb);
  }
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
    lv_area_t r = segBox(c, s, a, b, g);
    lv_area_move(&r, o.x1, o.y1);
    lv_obj_invalidate_area(c.obj, &r);
  }
}

static bool intersect(lv_area_t* out, const lv_area_t* a, const lv_area_t* b) {
  *out = {LV_MAX(a->x1, b->x1), LV_MAX(a->y1, b->y1), LV_MIN(a->x2, b->x2), LV_MIN(a->y2, b->y2)};
  return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static bool hits(const lv_area_t& a, const lv_area_t& clip) {
  lv_area_t x;
  return intersect(&x, &a, &clip);
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
      if (!sideRect(c, s, a, b, w, os, ox, oy, r) || !hits(r, clip)) continue;
      lv_draw_rect_dsc_t d;
      lv_draw_rect_dsc_init(&d);
      d.bg_color = col;
      d.bg_opa = opa;
      lv_draw_rect(L, &d, &r);
    } else {
      lv_area_t box = segBox(c, s, a, b, w);
      lv_area_move(&box, ox, oy);
      if (!hits(box, clip)) continue;
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
  const int ri = LV_MAX(1, (int)lroundf(r));
  const lv_area_t a = {(int32_t)lroundf(x) - ri, (int32_t)lroundf(y) - ri, (int32_t)lroundf(x) + ri,
                       (int32_t)lroundf(y) + ri};
  if (!hits(a, L->_clip_area)) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = col;
  d.bg_opa = opa;
  d.radius = LV_RADIUS_CIRCLE;
  lv_draw_rect(L, &d, &a);
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
    if (!sideRect(c, s, a, b, c.lw, c.ro, ox, oy, r) || !hits(r, L->_clip_area)) continue;
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
  // A mark every 5 s, lit once passed.
  for (int k = 0; k < 12; k++) {
    const float d = c.len * k / 12;
    float x, y;
    ringPoint(c, d, x, y);
    const bool lit = d <= head;
    dot(L, ox + x, oy + y, c.lw * (k % 3 ? 0.6f : 0.95f), lit ? acc : text, lit ? LV_OPA_COVER : LV_OPA_50);
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
  if (ch == ' ' || opa < 2) return;
  lv_area_t clip;
  if (!intersect(&clip, &slot, &L->_clip_area)) return;
  const lv_area_t a = {x, y, x + w - 1, y + lineH(f) - 1};
  lv_area_t vis;
  if (!intersect(&vis, &a, &clip)) return;
  const char s[2] = {ch, 0};
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = s;
  d.text_local = 1;
  d.font = f;
  d.color = col;
  d.opa = opa;
  d.align = LV_TEXT_ALIGN_CENTER;
  const lv_area_t keep = L->_clip_area;
  L->_clip_area = clip;
  lv_draw_label(L, &d, &a);
  L->_clip_area = keep;
}

static lv_opa_t fade(float d, float span) { return (lv_opa_t)(255 * clampf(1 - fabsf(d) / span, 0, 1)); }

static void drawTime(lv_layer_t* L, const ClockFx& c, int ox, int oy, lv_color_t col) {
  // Changed digits roll in from the right one after another; Orbit slides them together.
  int rank[5] = {-1, -1, -1, -1, -1}, n = 0;
  const bool changing = c.el < AT_REST && c.from[0];
  for (int i = 4; i >= 0; i--)
    if (i != 2 && changing && c.shown[i] != c.from[i]) rank[i] = n++;
  const bool roll = c.style == CLOCK_ROLL;
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
    } else if (roll) {
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
    if (hits(a, L->_clip_area)) lv_draw_label(L, &d, &a);
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
  if (c->style == CLOCK_BLOCKS) {
    blocksDraw(c->blk, L, a.x1, a.y1, c->hasColor ? lv_color_hex(c->color) : text);
    return;
  }
  if (c->style == CLOCK_BREAKOUT || c->style == CLOCK_ROCKS) {
    arcadeDraw(c->arc, L, a.x1, a.y1, c->hasColor ? lv_color_hex(c->color) : text, s_animMs);
    return;
  }
  if (c->style == CLOCK_ORBIT && c->len > 0) drawRing(L, *c, a.x1, a.y1, text);
  drawTime(L, *c, a.x1, a.y1, c->hasColor ? lv_color_hex(c->color) : text);
  if (c->dateY >= 0) {
    const lv_area_t d = {a.x1, a.y1 + c->dateY, a.x2, a.y1 + c->dateY + lineH(c->df) - 1};
    if (!hits(d, L->_clip_area)) return;
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
  c->style = c->want = r.def->clockStyle;
  if (c->want == CLOCK_BLOCKS && !(c->blk = blocksNew())) c->style = c->want = CLOCK_ROLL;
  if ((c->want == CLOCK_BREAKOUT || c->want == CLOCK_ROCKS) && !(c->arc = arcadeNew(c->want)))
    c->style = c->want = CLOCK_ROLL;
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
}

void fitClockFx(TileRt& r, int cw, int ch) {
  ClockFx* c = r.cfx;
  if (!c) return;
  static const lv_font_t* const kTime[] = {&deck_font_clock_72, &deck_font_clock_56, &lv_font_montserrat_40,
                                           &deck_font_28, &deck_font_20, &deck_font_14};
  constexpr int N = sizeof(kTime) / sizeof(kTime[0]);
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  c->w = (int16_t)(cw + 2 * pad);
  c->h = (int16_t)(ch + 2 * pad);
  lv_obj_set_pos(c->obj, -pad, -pad);
  lv_obj_set_size(c->obj, c->w, c->h);
  c->style = c->want;
  if (c->style == CLOCK_BLOCKS && !blocksFit(c->blk, c->w, c->h, pad)) c->style = CLOCK_ROLL;
  if ((c->style == CLOCK_BREAKOUT || c->style == CLOCK_ROCKS) && !arcadeFit(c->arc, c->w, c->h, pad))
    c->style = CLOCK_ROLL;
  int aw = cw, ah = ch;
  c->len = 0;
  if (c->style == CLOCK_ORBIT) {
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
  auto cellW = [](const lv_font_t* f) {
    int w = 0;
    for (char d = '0'; d <= '9'; d++) w = LV_MAX(w, (int)lv_font_get_glyph_width(f, d, 0));
    return w;
  };
  auto width = [&](const lv_font_t* f) {
    return 4 * cellW(f) + (int)lv_font_get_glyph_width(f, ':', 0) + (ampm ? 4 + textW("PM", unitFont(f)) : 0);
  };
  int best = -1, dated = -1;
  for (int i = 0; i < N; i++) {
    int top, h;
    digitInk(kTime[i], top, h);
    if (width(kTime[i]) > aw || h > ah) continue;
    if (best < 0) best = i;
    if (h + LV_MAX(4, h / 5) + dH <= ah) {
      dated = i;
      break;
    }
  }
  if (best < 0) best = N - 1;
  const bool date = dated >= 0 && (dated <= best + 1 || r.def->clockDate);
  const lv_font_t* f = kTime[date ? dated : best];
  c->f = f;
  c->df = df;
  c->uf = unitFont(f);
  c->cell = (int16_t)cellW(f);
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
  // Fonts put the colon at x-height middle: centre it on the digits instead.
  lv_font_glyph_dsc_t dg = {}, dc = {};
  c->colonDy = 0;
  if (lv_font_get_glyph_dsc(f, &dg, '0', 0) && lv_font_get_glyph_dsc(f, &dc, ':', 0))
    c->colonDy = (int16_t)((dc.ofs_y * 2 + dc.box_h - dg.ofs_y * 2 - dg.box_h) / 2);
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
  if (c->style == CLOCK_BLOCKS || c->style == CLOCK_BREAKOUT || c->style == CLOCK_ROCKS) {  // kept for a fall back to Roll; these run on their own
    memcpy(c->shown, t, sizeof(c->shown));
    snprintf(c->ap, sizeof(c->ap), "%s", ap);
    snprintf(c->date, sizeof(c->date), "%s", date);
    return;
  }
  const bool live = c->shown[0] && strcmp(t, c->shown) && c->f && r.page == sh.active && fxShowing();
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
static void blocksTime(time_t t, char* out) {
  if (!t) {
    memcpy(out, "  :  ", 6);
    return;
  }
  struct tm lt;
  localtime_r(&t, &lt);
  const int h = netSettings.use24h ? lt.tm_hour : (lt.tm_hour + 11) % 12 + 1;
  snprintf(out, 6, netSettings.use24h ? "%02d:%02d" : "%2d:%02d", h, lt.tm_min);
}

static uint32_t changeMs(const ClockFx& c) {
  return c.style == CLOCK_ROLL ? ROLL_MS + 3 * ROLL_STAGGER_MS : LV_MAX(SLIDE_MS, FLASH_MS);
}

void clockFxTick() {
  if (!(advSettings.clock60 ? s_animFastDue : s_animDue)) return;
  const uint32_t now = s_animMs;
  const bool showing = fxShowing();
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool synced = tv.tv_sec >= 1609459200;
  const bool lit = !synced || tv.tv_usec < 500000;
  bool painted = false;
  for (TileRt& r : s_tiles) {
    ClockFx* c = r.cfx;
    if (!c || r.off || !c->f) continue;
    const bool live = showing && r.page == sh.active;
    if (c->style == CLOCK_BLOCKS) {
      char cur[6], next[6];
      blocksTime(synced ? tv.tv_sec : 0, cur);
      blocksTime(synced ? tv.tv_sec + 60 : 0, next);
      struct tm lt;
      localtime_r(&tv.tv_sec, &lt);
      if (blocksTick(c->blk, c->obj, now, live, lit || !live, cur, next, synced ? lt.tm_sec : 0,
                     (int32_t)(tv.tv_sec / 60))) {
        if (live) painted = true;
        else markPageStale(r.page);
      }
      continue;
    }
    if (c->style == CLOCK_BREAKOUT || c->style == CLOCK_ROCKS) {
      char cur[6], next[6];
      blocksTime(synced ? tv.tv_sec : 0, cur);
      blocksTime(synced ? tv.tv_sec + 60 : 0, next);
      struct tm lt;
      localtime_r(&tv.tv_sec, &lt);
      if (arcadeTick(c->arc, c->obj, now, live, lit || !live, cur, next, synced ? lt.tm_sec : 0,
                     (int32_t)(tv.tv_sec / 60))) {
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
      if (c->style == CLOCK_ORBIT) ringInvalidate(*c, 0, c->len);
      painted = true;
    }
    if (c->style == CLOCK_ORBIT && synced && c->len > 0) {
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
