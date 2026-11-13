// Deck UI: the audio visualizer tile, bars or a scope fed by the companion's spectrum packets.
#include "deck_ui_int.h"
#include "deck_viz.h"

namespace deckui {

constexpr uint32_t VIZ_FRAME_MS = 20;    // bars tick
constexpr float VIZ_FALL_PER_S = 2.2f;   // bar drop, tile heights per second
constexpr float VIZ_PEAK_FALL_PER_S = 0.7f;
constexpr uint32_t VIZ_PEAK_HOLD_MS = 400;
constexpr uint32_t VIZ_STALE_MS = 500;   // no packet this long: bars fall to zero
constexpr uint32_t VIZ_IDLE_MS = 3000;   // then "No audio"
constexpr int VIZ_PEAK_PX = 2;
constexpr int VIZ_GROUPS = 4;  // column groups invalidated separately, only the rows that moved
constexpr int SCOPE_TRAILS = 3;                // phosphor ghosts behind the trace
constexpr uint32_t SCOPE_TRAIL_STEP_MS = 36;  // one ghost per ~40 ms, whatever the packet rate
constexpr float SCOPE_LINE = 1.6f;            // trace thickness, px
constexpr uint32_t SCOPE_MIN_MS = 12;          // at most about one scope frame per vsync

struct VizRt {
  float lvl[VIZ_BANDS];
  float peak[VIZ_BANDS];
  uint32_t peakMs[VIZ_BANDS];
  int16_t barPx[VIZ_BANDS], peakPx[VIZ_BANDS];  // what the draw event paints
  uint32_t color;
  bool idle;
  bool full;  // next frame repaints the whole tile
  // Scope: A8 trace image drawn in the accent colour.
  bool scope;
  lv_draw_buf_t* buf;
  bool painted;
  uint32_t waveSerial, trailMs;
  uint8_t trails;  // ghosts in use
  uint8_t prev[VIZ_WAVE_POINTS];
  uint8_t trail[SCOPE_TRAILS][VIZ_WAVE_POINTS];  // newest first
};

static uint32_t s_vizSerial = 0;
static uint32_t s_vizLastTick = 0, s_scopeMs = 0;

void vizFree(VizRt* v) {
  if (!v) return;
  if (v->buf) lv_draw_buf_destroy(v->buf);
  lv_free(v);
}

// Bars that fit: 32, 16 or 8, each at least 4 px wide.
static int vizLayout(int w, int& bw, int& gap, int& x0) {
  gap = w >= 200 ? 3 : 2;
  int n = 32;
  while (n > 8 && (w - (n - 1) * gap) / n < 4) n /= 2;
  bw = std::max(1, (w - (n - 1) * gap) / n);
  x0 = (w - (n * bw + (n - 1) * gap)) / 2;
  return n;
}

void makeVizContent(TileRt& r, lv_obj_t* t) {
  r.vz = (VizRt*)lv_malloc_zeroed(sizeof(VizRt));
  if (r.vz) {
    r.vz->color = tileAccent(*r.def);
    r.vz->idle = true;
    r.vz->full = true;
    r.vz->scope = r.def->vizScope;
  }
  r.viz = lv_obj_create(t);
  lv_obj_remove_style_all(r.viz);
  lv_obj_remove_flag(r.viz, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  r.name = lv_label_create(t);
  lv_label_set_text(r.name, "No audio");
  lv_obj_set_style_text_opa(r.name, LV_OPA_70, 0);
}

void fitViz(TileRt& r, int cw, int ch) {
  lv_obj_set_size(r.viz, cw, ch);
  lv_obj_set_pos(r.viz, 0, 0);
  setFont(r.name, ch >= 100 ? &deck_font_20 : &deck_font_14);
  lv_obj_center(r.name);
  if (r.vz) r.vz->full = true;
}

// One frame: levels fall, new bands push them up. True when a bar moved a pixel.
static bool vizStep(TileRt& r, uint32_t now, uint32_t dtMs) {
  VizRt& v = *r.vz;
  const int w = lv_obj_get_width(r.viz), h = lv_obj_get_height(r.viz);
  if (w < 8 || h < 8) return false;
  int bw, gap, x0;
  const int n = vizLayout(w, bw, gap, x0), group = VIZ_BANDS / n;
  const uint8_t* bands = vizBands();
  const uint32_t age = vizRxMs() ? now - vizRxMs() : UINT32_MAX;
  const float dt = (float)dtMs / 1000.0f;
  bool changed = false, any = false;
  int gTop[VIZ_GROUPS], gBot[VIZ_GROUPS], gX1[VIZ_GROUPS], gX2[VIZ_GROUPS];
  for (int g = 0; g < VIZ_GROUPS; g++) {
    gTop[g] = h;
    gBot[g] = -1;
    gX1[g] = w;
    gX2[g] = -1;
  }
  auto mark = [&](int g, int y1, int y2) {
    gTop[g] = std::min(gTop[g], y1);
    gBot[g] = std::max(gBot[g], y2);
  };
  for (int i = 0; i < n; i++) {
    uint8_t b = 0;
    if (age < VIZ_STALE_MS)
      for (int k = 0; k < group; k++) b = std::max(b, bands[i * group + k]);
    const float target = b / 255.0f;
    v.lvl[i] = std::max(target, v.lvl[i] - VIZ_FALL_PER_S * dt);
    if (v.lvl[i] < 0) v.lvl[i] = 0;
    if (v.lvl[i] >= v.peak[i]) {
      v.peak[i] = v.lvl[i];
      v.peakMs[i] = now;
    } else if (now - v.peakMs[i] > VIZ_PEAK_HOLD_MS) {
      v.peak[i] = std::max(v.lvl[i], v.peak[i] - VIZ_PEAK_FALL_PER_S * dt);
    }
    const int16_t bp = (int16_t)lroundf(v.lvl[i] * (h - VIZ_PEAK_PX - 1));
    const int16_t pp = v.peak[i] > 0.01f ? (int16_t)lroundf(v.peak[i] * (h - VIZ_PEAK_PX - 1)) : -1;
    any |= bp > 0 || pp >= 0;
    const int g = i * VIZ_GROUPS / n, x = x0 + i * (bw + gap);
    gX1[g] = std::min(gX1[g], x);
    gX2[g] = std::max(gX2[g], x + bw - 1);
    // Rows between the old and new top, plus the rounded corners.
    if (bp != v.barPx[i]) mark(g, h - std::max(bp, v.barPx[i]) - 3, h - std::min(bp, v.barPx[i]) + 2);
    if (pp != v.peakPx[i])
      for (int q : {(int)pp, (int)v.peakPx[i]})
        if (q >= 0) mark(g, h - 1 - q - VIZ_PEAK_PX - 1, h - q);
    if (bp != v.barPx[i] || pp != v.peakPx[i]) {
      v.barPx[i] = bp;
      v.peakPx[i] = pp;
      changed = true;
    }
  }
  const bool idle = !any && age >= VIZ_IDLE_MS;
  if (idle != v.idle) {
    v.idle = idle;
    showObj(r.name, idle);
    v.full = true;
  }
  if (!changed && !v.full) return false;
  if (v.full) {
    v.full = false;
    lv_obj_invalidate(r.viz);
    return true;
  }
  lv_area_t a;
  lv_obj_get_coords(r.viz, &a);
  for (int g = 0; g < VIZ_GROUPS; g++) {
    if (gBot[g] < gTop[g] || gX2[g] < gX1[g]) continue;
    const lv_area_t d = {a.x1 + gX1[g], a.y1 + std::max(0, gTop[g]), a.x1 + gX2[g], a.y1 + std::min(h - 1, gBot[g])};
    lv_obj_invalidate_area(r.viz, &d);
  }
  return true;
}

// Max-blends one column span [lo, hi] (rows, fractional) into the A8 image.
static void scopeSpan(lv_draw_buf_t* b, int x, float lo, float hi, uint8_t alpha) {
  const int h = b->header.h;
  const int y0 = std::max(0, (int)floorf(lo)), y1 = std::min(h - 1, (int)ceilf(hi) - 1);
  for (int y = y0; y <= y1; y++) {
    const float cov = std::min(hi, (float)y + 1) - std::max(lo, (float)y);
    if (cov <= 0) continue;
    uint8_t& px = b->data[y * b->header.stride + x];
    const uint8_t a = (uint8_t)(alpha * std::min(1.0f, cov));
    if (a > px) px = a;
  }
}

// One trace, the 128 samples stretched over the width; each column joins its
// neighbours' midpoints, so steep edges stay connected.
static void scopeTrace(lv_draw_buf_t* b, const uint8_t* wave, uint8_t alpha) {
  const int w = b->header.w, h = b->header.h;
  const float cy = (h - 1) / 2.0f, half = (h - 1) / 2.0f - SCOPE_LINE;
  auto yAt = [&](int x) {
    const float f = (float)x * (VIZ_WAVE_POINTS - 1) / (float)(w - 1);
    const int i = std::min((int)f, VIZ_WAVE_POINTS - 2);
    const float v = wave[i] + (wave[i + 1] - wave[i]) * (f - (float)i);
    return cy - (v - 128.0f) / 128.0f * half + 0.5f;
  };
  float prev = yAt(0), cur = prev;
  for (int x = 0; x < w; x++) {
    const float next = x + 1 < w ? yAt(x + 1) : cur;
    const float a = (prev + cur) / 2, c = (cur + next) / 2;
    const float lo = std::min(cur, std::min(a, c)) - SCOPE_LINE / 2, hi = std::max(cur, std::max(a, c)) + SCOPE_LINE / 2;
    scopeSpan(b, x, lo, hi, alpha);
    prev = cur;
    cur = next;
  }
}

// Graticule: centre axis, dotted divisions.
static void scopeGrid(lv_draw_buf_t* b) {
  const int w = b->header.w, h = b->header.h;
  const int cy = (h - 1) / 2;
  for (int x = 0; x < w; x++) b->data[cy * b->header.stride + x] = 70;
  for (int k : {-2, -1, 1, 2}) {
    const int y = cy + k * (h - 1) / 5;
    if (y < 0 || y >= h) continue;
    for (int x = 0; x < w; x += 3) b->data[y * b->header.stride + x] = 40;
  }
  for (int k = 1; k < 8; k++) {
    const int x = k * (w - 1) / 8;
    for (int y = 0; y < h; y += 3) b->data[y * b->header.stride + x] = 40;
  }
}

// One scope frame per new waveform. True when it painted.
static bool scopeStep(TileRt& r, uint32_t now) {
  VizRt& v = *r.vz;
  const int w = lv_obj_get_width(r.viz), h = lv_obj_get_height(r.viz);
  if (w < 8 || h < 8) return false;
  if (!v.buf || v.buf->header.w != w || v.buf->header.h != h) {
    if (v.buf) lv_draw_buf_destroy(v.buf);
    v.buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    if (!v.buf) return false;
    v.full = true;
  }
  const uint32_t age = vizRxMs() ? now - vizRxMs() : UINT32_MAX;
  const uint8_t* wave = age < VIZ_STALE_MS ? vizWave() : nullptr;
  const bool idle = age >= VIZ_IDLE_MS;
  if (idle != v.idle) {
    v.idle = idle;
    showObj(r.name, idle);
    v.full = true;
  }
  const bool fresh = wave && vizSerial() != v.waveSerial;
  if (!fresh && !v.full && (wave || !v.trails)) return false;
  if (!wave) v.trails = 0;  // stale: the trace collapses to the grid
  if (fresh) {
    if (v.painted && now - v.trailMs >= SCOPE_TRAIL_STEP_MS) {
      v.trailMs = now;
      memmove(v.trail[1], v.trail[0], sizeof(v.trail[0]) * (SCOPE_TRAILS - 1));
      memcpy(v.trail[0], v.prev, VIZ_WAVE_POINTS);
      if (v.trails < SCOPE_TRAILS) v.trails++;
    }
    memcpy(v.prev, wave, VIZ_WAVE_POINTS);
    v.waveSerial = vizSerial();
  }
  for (int y = 0; y < h; y++) memset(v.buf->data + y * v.buf->header.stride, 0, w);
  scopeGrid(v.buf);
  for (int k = v.trails - 1; k >= 0; k--) scopeTrace(v.buf, v.trail[k], (uint8_t)(97 - k * 25));
  if (wave) scopeTrace(v.buf, v.prev, 255);
  v.painted = wave != nullptr;
  v.full = false;
  lv_obj_invalidate(r.viz);
  return true;
}

static void scopeDraw(const VizRt* v, lv_layer_t* layer, const lv_area_t& a) {
  if (!v->buf || lv_area_get_width(&a) != (int32_t)v->buf->header.w ||
      lv_area_get_height(&a) != (int32_t)v->buf->header.h)
    return;
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = v->buf;
  d.recolor = lv_color_hex(v->color);
  d.recolor_opa = LV_OPA_COVER;
  lv_draw_image(layer, &d, &a);
}

void vizDrawEvent(lv_event_t* e) {
  const VizRt* v = (const VizRt*)lv_event_get_user_data(e);
  lv_obj_t* o = lv_event_get_target_obj(e);
  if (!v) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_layer_t* layer = lv_event_get_layer(e);
  if (v->scope) {
    scopeDraw(v, layer, a);
    return;
  }
  int bw, gap, x0;
  const int n = vizLayout(lv_area_get_width(&a), bw, gap, x0);
  lv_draw_rect_dsc_t bar;
  lv_draw_rect_dsc_init(&bar);
  bar.bg_color = lv_color_hex(v->color);
  bar.bg_opa = LV_OPA_COVER;
  bar.radius = std::min(2, bw / 2);
  lv_draw_rect_dsc_t cap = bar;
  cap.bg_color = lv_color_mix(lv_color_white(), lv_color_hex(v->color), 160);
  for (int i = 0; i < n; i++) {
    const int x = a.x1 + x0 + i * (bw + gap);
    if (v->barPx[i] > 0) {
      const lv_area_t ba = {x, a.y2 - v->barPx[i] + 1, x + bw - 1, a.y2};
      lv_draw_rect(layer, &bar, &ba);
    }
    if (v->peakPx[i] >= 0) {
      const int y = a.y2 - v->peakPx[i] - VIZ_PEAK_PX;
      const lv_area_t pa = {x, y, x + bw - 1, y + VIZ_PEAK_PX - 1};
      lv_draw_rect(layer, &cap, &pa);
    }
  }
}

static bool vizShowing() {
  return !manualDisplayOff() && currentBacklightLevel() && !s_qp.open && !s_modalCard && !s_ed.root;
}

// Bars animate on a fixed tick; a scope paints each new waveform as it lands,
// since a tick at the packet rate beats against the packets and drops frames.
void vizTick() {
  const uint32_t now = millis();
  const uint32_t serial = vizSerial();
  const bool barsDue = now - s_vizLastTick >= VIZ_FRAME_MS;
  const bool scopeDue = serial != s_vizSerial && now - s_scopeMs >= SCOPE_MIN_MS;
  if (!barsDue && !scopeDue) return;
  uint32_t dt = 0;
  if (barsDue) {
    dt = std::min<uint32_t>(now - s_vizLastTick, 200);
    // Fixed steps, so loop jitter does not stretch the frame period.
    s_vizLastTick = now - s_vizLastTick < 2 * VIZ_FRAME_MS ? s_vizLastTick + VIZ_FRAME_MS : now;
  }
  if (!vizShowing()) return;
  s_vizSerial = serial;
  if (scopeDue) s_scopeMs = now;
  bool painted = false;
  for (TileRt& r : s_tiles) {
    if (!r.vz || r.page != sh.active || r.off) continue;
    if (r.vz->scope ? scopeStep(r, now) : barsDue && vizStep(r, now, dt)) painted = true;
  }
  if (painted) s_sparkPaintMs = now;  // like a gliding chart: other pages snapshot rarely meanwhile
}

}  // namespace deckui
