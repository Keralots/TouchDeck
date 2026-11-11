// Deck UI: the audio visualizer tile, bars fed by the companion's spectrum packets.
#include "deck_ui_int.h"
#include "deck_viz.h"

namespace deckui {

constexpr float VIZ_FALL_PER_S = 2.2f;   // bar drop, tile heights per second
constexpr float VIZ_PEAK_FALL_PER_S = 0.7f;
constexpr uint32_t VIZ_PEAK_HOLD_MS = 400;
constexpr uint32_t VIZ_STALE_MS = 500;   // no packet this long: bars fall to zero
constexpr uint32_t VIZ_IDLE_MS = 3000;   // then "No audio"
constexpr int VIZ_PEAK_PX = 2;
constexpr int VIZ_GROUPS = 4;  // column groups invalidated separately, only the rows that moved

struct VizRt {
  float lvl[VIZ_BANDS];
  float peak[VIZ_BANDS];
  uint32_t peakMs[VIZ_BANDS];
  int16_t barPx[VIZ_BANDS], peakPx[VIZ_BANDS];  // what the draw event paints
  uint32_t color;
  bool idle;
  bool full;  // next frame repaints the whole tile
};

static uint32_t s_vizSerial = 0;
static uint32_t s_vizLastTick = 0, s_vizLastPaint = 0;
static bool s_vizDirty = false;
static bool s_vizMeasuring = false;

void vizFree(VizRt* v) { lv_free(v); }

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
  if (v.full || !vizPartial) {
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

void vizDrawEvent(lv_event_t* e) {
  const VizRt* v = (const VizRt*)lv_event_get_user_data(e);
  lv_obj_t* o = lv_event_get_target_obj(e);
  if (!v) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  int bw, gap, x0;
  const int n = vizLayout(lv_area_get_width(&a), bw, gap, x0);
  lv_layer_t* layer = lv_event_get_layer(e);
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

void vizTick() {
  const uint32_t now = millis();
  if (now - s_vizLastTick < vizFrameMs) return;
  const uint32_t dt = std::min<uint32_t>(now - s_vizLastTick, 200);
  // Fixed steps, so loop jitter does not stretch the frame period.
  s_vizLastTick = now - s_vizLastTick < 2 * vizFrameMs ? s_vizLastTick + vizFrameMs : now;
  if (!vizShowing()) return;
  const uint32_t serial = vizSerial();
  if (serial - s_vizSerial > 1 && s_vizSerial) vizStats.superseded += serial - s_vizSerial - 1;
  s_vizSerial = serial;
  bool painted = false;
  for (TileRt& r : s_tiles)
    if (r.vz && r.page == sh.active && vizStep(r, now, dt)) painted = true;
  if (!painted) return;
  const bool fed = vizRxMs() && now - vizRxMs() < VIZ_STALE_MS;
  if (fed && s_vizLastPaint && now - s_vizLastPaint < 1000 && now - s_vizLastPaint > vizStats.tickGapMax)
    vizStats.tickGapMax = now - s_vizLastPaint;
  s_vizLastPaint = now;
  vizStats.ticks++;
  s_vizDirty = true;
  s_sparkPaintMs = now;  // like a gliding chart: other pages snapshot rarely meanwhile
}

bool vizTakeDirty() {
  const bool d = s_vizDirty;
  s_vizDirty = false;
  return d;
}

void vizNoteHandler(uint32_t us) {
  vizStats.lvUsSum += us;
  vizStats.lvCalls++;
  if (us > vizStats.lvUsMax) vizStats.lvUsMax = us;
}

bool vizMeasuring() { return s_vizMeasuring; }

}  // namespace deckui

// Spike stats: viz counters plus the panel's frame counters since the last reset.
void deckVizStats(String& out, bool reset) {
  using namespace deckui;
  static PanelStats acc = {};
  static uint32_t t0 = millis();
  const PanelStats p = panelTakeStats();
  acc.frames += p.frames;
  acc.compFrames += p.compFrames;
  acc.teLate += p.teLate;
  acc.lvDropped += p.lvDropped;
  acc.flushUsMax = std::max(acc.flushUsMax, p.flushUsMax);
  acc.gapMaxUs = std::max(acc.gapMaxUs, p.gapMaxUs);
  const uint32_t ms = millis() - t0;
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"ms\":%u,\"packets\":%u,\"superseded\":%u,\"ticks\":%u,\"lvCalls\":%u,\"lvUsAvg\":%u,"
           "\"lvUsMax\":%u,\"tickGapMax\":%u,\"rxGapMax\":%u,\"frames\":%u,\"comp\":%u,\"teLate\":%u,"
           "\"lvDrop\":%u,\"flushUsMax\":%u,\"gapMaxUs\":%u,\"frameMs\":%u,\"snapUsMax\":%u}",
           (unsigned)ms, (unsigned)vizStats.packets, (unsigned)vizStats.superseded, (unsigned)vizStats.ticks,
           (unsigned)vizStats.lvCalls, (unsigned)(vizStats.lvCalls ? vizStats.lvUsSum / vizStats.lvCalls : 0),
           (unsigned)vizStats.lvUsMax, (unsigned)vizStats.tickGapMax, (unsigned)vizStats.rxGapMax,
           (unsigned)acc.frames, (unsigned)acc.compFrames, (unsigned)acc.teLate, (unsigned)acc.lvDropped,
           (unsigned)acc.flushUsMax, (unsigned)acc.gapMaxUs, (unsigned)vizFrameMs, (unsigned)s_snapUsMax);
  out = buf;
  if (reset) {
    s_vizMeasuring = true;
    vizStats = {};
    acc = {};
    s_snapUsMax = 0;
    t0 = millis();
  }
}
