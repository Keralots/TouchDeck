// Deck UI: PC metric tiles, gliding charts and sensor rows.
#include "deck_ui_int.h"

namespace deckui {

// --- PC metric charts -----------------------------------------------------------------
constexpr float SPARK_LINE = 2.0f;  // line thickness, px
constexpr float SPARK_EASE_MS = 250.0f;
constexpr int SPARK_MAX_W = 480;

constexpr float SPARK_SYNC_MS = 400.0f;
constexpr int SPARK_SPAN = PC_HISTORY_LEN - 2;  // sample intervals across the chart

uint32_t s_sparkPaintMs = 0;  // last glide frame of the shown page

void sparkFree(Spark* s) {
  if (!s) return;
  if (s->buf) lv_draw_buf_destroy(s->buf);
  lv_free(s);
}

// Catmull-Rom between samples, clamped to the four neighbours so it never
// overshoots the data (a percentage below zero).
float sparkAt(const Spark& s, float fi) {
  const int n = s.n;
  int i1 = (int)fi;
  i1 = i1 < 0 ? 0 : i1 > n - 1 ? n - 1 : i1;
  const float t = fi - (float)i1;
  const float p0 = s.v[i1 > 0 ? i1 - 1 : 0], p1 = s.v[i1];
  const float p2 = s.v[i1 + 1 < n ? i1 + 1 : n - 1], p3 = s.v[i1 + 2 < n ? i1 + 2 : n - 1];
  const float v = 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                          (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
  const float lo = std::min(std::min(p0, p1), std::min(p2, p3));
  const float hi = std::max(std::max(p0, p1), std::max(p2, p3));
  return v < lo ? lo : v > hi ? hi : v;
}

// Segment i runs from vertex i to i + 1; lo / hi: the y band it can reach.
struct SparkSeg {
  float ay, dy, inv, lo, hi;
  bool ok;
};

// sparkPaint scratch, once, in PSRAM: internal RAM is what WiFi buffers need.
struct SparkScratch {
  float ys[SPARK_MAX_W];
  SparkSeg seg[SPARK_MAX_W];
  int16_t cy0[SPARK_MAX_W], cy1[SPARK_MAX_W];
};
SparkScratch* s_sparkScratch = nullptr;

// Each pixel near the line gets alpha from its distance to the line, so steep
// spikes get smooth sides too. The line has a vertex per column.
void sparkPaint(Spark& s, float right) {
  lv_draw_buf_t* b = s.buf;
  const int w = b->header.w, h = b->header.h;
  for (int y = 0; y < h; y++) memset(b->data + y * b->header.stride, 0, w);
  if (s.n < 2 || !(s.viewHi > s.viewLo)) return;
  if (!s_sparkScratch) s_sparkScratch = (SparkScratch*)lv_malloc(sizeof(SparkScratch));
  if (!s_sparkScratch) return;
  float* ys = s_sparkScratch->ys;
  SparkSeg* seg = s_sparkScratch->seg;
  int16_t* cy0 = s_sparkScratch->cy0;
  int16_t* cy1 = s_sparkScratch->cy1;
  const float step = (float)SPARK_SPAN / (float)(w - 1);
  const float m = SPARK_LINE / 2 + 0.5f;
  const float k = ((float)h - 2 * m) / (s.viewHi - s.viewLo);
  for (int x = 0; x < w; x++) {
    const float fi = right - (float)(w - 1 - x) * step;
    ys[x] = fi < 0 ? NAN : m + (s.viewHi - sparkAt(s, fi)) * k;
  }
  // Pixels of column x can only be reached by segments x - 2 .. x + 1, and
  // only by the part of the line within rad of the column. Rows are written
  // in order: PSRAM hates column strides.
  const float rad = SPARK_LINE / 2 + 0.5f;  // full alpha within half a pixel of the stroke
  for (int i = 0; i + 1 < w; i++) {
    SparkSeg& g = seg[i];
    g.ok = !isnan(ys[i]) && !isnan(ys[i + 1]);
    g.ay = ys[i];
    g.dy = ys[i + 1] - ys[i];
    g.inv = 1.0f / (1.0f + g.dy * g.dy);
    g.lo = std::min(ys[i], ys[i + 1]) - rad;
    g.hi = std::max(ys[i], ys[i + 1]) + rad;
  }
  int yMin = h, yMax = -1;
  for (int x = 0; x < w; x++) {
    float top = INFINITY, bot = -INFINITY;
    auto take = [&](float v) {
      if (isnan(v)) return;
      top = std::min(top, v);
      bot = std::max(bot, v);
    };
    for (int i = x - 1; i <= x + 1; i++)
      if (i >= 0 && i < w) take(ys[i]);
    if (x >= 2) take((ys[x - 2] + ys[x - 1]) / 2);
    if (x + 2 < w) take((ys[x + 1] + ys[x + 2]) / 2);
    cy0[x] = top > bot ? h : (int16_t)std::max(0, (int)floorf(top - rad));
    cy1[x] = top > bot ? -1 : (int16_t)std::min(h - 1, (int)ceilf(bot + rad));
    yMin = std::min(yMin, (int)cy0[x]);
    yMax = std::max(yMax, (int)cy1[x]);
  }
  const float rad2 = rad * rad, full2 = (rad - 1) * (rad - 1);
  for (int y = yMin; y <= yMax; y++) {
    uint8_t* row = b->data + y * b->header.stride;
    const float py = (float)y + 0.5f;
    for (int x = 0; x < w; x++) {
      if (y < cy0[x] || y > cy1[x]) continue;
      float d2 = rad2;
      for (int i = std::max(0, x - 2); i <= std::min(w - 2, x + 1); i++) {
        const SparkSeg& g = seg[i];
        if (!g.ok || py < g.lo || py > g.hi) continue;
        const float ex0 = (float)(x - i), ey0 = py - g.ay;  // pixel centre minus vertex i
        float t = (ex0 + ey0 * g.dy) * g.inv;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float ex = ex0 - t, ey = ey0 - t * g.dy;
        d2 = std::min(d2, ex * ex + ey * ey);
      }
      if (d2 >= rad2) continue;
      row[x] = d2 <= full2 ? 255 : (uint8_t)((rad - sqrtf(d2)) * 255);
    }
  }
}

// Repaint when the glide moved a quarter of a pixel or the axis moved; true
// when it painted. ease moves the glide and the axis on by dtMs; without it
// both jump to where they should be.
bool sparkRender(TileRt& r, uint32_t now, bool ease, uint32_t dtMs) {
  Spark& s = *r.spark;
  if (lv_obj_has_flag(r.viz, LV_OBJ_FLAG_HIDDEN)) return false;
  const int w = lv_obj_get_width(r.viz), h = lv_obj_get_height(r.viz);
  if (w < 8 || h < 8 || w > SPARK_MAX_W) return false;
  if (!s.buf || s.buf->header.w != w || s.buf->header.h != h) {
    if (s.buf) lv_draw_buf_destroy(s.buf);
    s.buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    s.key = -1;
    s.painted = false;
    if (!s.buf) return false;
  }
  if (!s.haveView || !ease || !isfinite(s.viewLo) || !isfinite(s.viewHi)) {
    s.viewLo = s.lo;
    s.viewHi = s.hi;
    s.haveView = true;
  } else {
    const float a = std::min(1.0f, (float)dtMs / SPARK_EASE_MS);
    s.viewLo += (s.lo - s.viewLo) * a;
    s.viewHi += (s.hi - s.viewHi) * a;
  }
  // The packet clock says one sample behind the newest at arrival, the newest
  // one interval later; the glide runs at that rate and eases onto it.
  const float target = std::min(0.0f, -1.0f + (float)(now - s.rxMs) / (float)s.gapMs);
  if (!dispSettings.smoothCharts || s.n < 3) s.off = 0;
  else if (!ease) s.off = target;
  else s.off += (float)dtMs / (float)s.gapMs + (target - s.off) * std::min(1.0f, (float)dtMs / SPARK_SYNC_MS);
  s.off = std::min(s.off, 0.0f);
  if (s.n > SPARK_SPAN) s.off = std::max(s.off, (float)(SPARK_SPAN + 1 - s.n));  // left edge on data
  const int32_t key = (int32_t)floorf(s.off * (float)(w - 1) / SPARK_SPAN * 4);
  const float tol = (s.hi - s.lo) / 400;
  if (key == s.key && fabsf(s.viewLo - s.paintLo) <= tol && fabsf(s.viewHi - s.paintHi) <= tol) return false;
  sparkPaint(s, (float)(s.n - 1) + s.off);
  s.painted = true;
  s.key = key;
  s.paintLo = s.viewLo;
  s.paintHi = s.viewHi;
  lv_obj_invalidate(r.viz);
  return true;
}

void sparkDrawEvent(lv_event_t* e) {
  const Spark* s = (const Spark*)lv_event_get_user_data(e);
  lv_obj_t* o = lv_event_get_target_obj(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if (!s->buf || !s->painted || lv_area_get_width(&a) != (int32_t)s->buf->header.w ||
      lv_area_get_height(&a) != (int32_t)s->buf->header.h)
    return;
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = s->buf;
  d.recolor = lv_color_hex(s->color);
  d.recolor_opa = LV_OPA_COVER;
  lv_draw_image(lv_event_get_layer(e), &d, &a);
}

void sparkSizeEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx < s_tiles.size() && s_tiles[idx].spark) sparkRender(s_tiles[idx], millis(), false, 0);
}

// A sample for the chart: a non-finite one repeats its predecessor.
float sparkSample(const SlotHistory& hs, int i, float prev) {
  const float v = pcHistoryAt(hs, (uint8_t)i);
  return isfinite(v) ? v : prev;
}

// Take a packet's new sample (or the whole history after a gap) into the
// tile's chart and aim its axis.
void sparkFeed(TileRt& r, const PcSource& pc, const SlotHistory& hs, int n, float scale, bool autoRate,
               uint32_t accent) {
  Spark& s = *r.spark;
  if (!n) {
    s.n = 0;
  } else if (hs.head != s.head || hs.count != s.cnt || !s.n) {
    const uint32_t dt = pc.data.sampleMs - s.rxMs;
    if (s.rxMs && dt >= 150 && dt <= 10000) s.gapMs = (s.gapMs * 3 + dt) / 4;
    s.rxMs = pc.data.sampleMs;
    const bool next = s.n && hs.count >= s.cnt && (hs.head + PC_HISTORY_LEN - s.head) % PC_HISTORY_LEN == 1;
    if (next) {
      if (s.n == SPARK_KEEP) memmove(s.v, s.v + 1, sizeof(float) * --s.n);
      s.v[s.n] = sparkSample(hs, n - 1, s.v[s.n - 1]);
      s.n++;
      s.off -= 1;  // the same place on screen, one sample further from the newest
    } else {
      for (int i = 0; i < n; i++) s.v[i] = sparkSample(hs, i, i ? s.v[i - 1] : 0.0f);
      s.n = (uint8_t)n;
      s.off = -1;
    }
    s.head = hs.head;
    s.cnt = hs.count;
  }
  float lo = 1e9f, hi = -1e9f;
  for (int i = std::max(0, s.n - PC_HISTORY_LEN); i < s.n; i++) {
    lo = std::min(lo, s.v[i]);
    hi = std::max(hi, s.v[i]);
  }
  if (s.n) {
    // Noise on a quiet metric must not fill the chart: span at least a tenth of the scale.
    const float minSpan = autoRate ? 0 : scale * 0.1f;
    if (hi - lo < minSpan) {
      lo = std::max(lo >= 0 ? 0.0f : lo - minSpan, (lo + hi - minSpan) / 2);
      hi = lo + minSpan;
    }
    const float pad = (hi - lo) * 0.15f + 0.5f;
    s.lo = lo - pad;
    s.hi = hi + pad;
  }
  s.color = accent;
  s.key = -1;
  // The shown page glides in deckTick; anything else is painted once per packet.
  if (r.page != sh.active || !sparkGliding()) sparkRender(r, millis(), false, 0);
}

// Pull the latest companion packet into one metric tile.
void applyPcToTile(TileRt& r) {
  const TileDef& d = *r.def;
  const PcSource* pc = pcSource(d.pc);
  const PcMetric* m = pcMetricFind(pc, d.metric);
  if (!m) {
    setText(r.sub, "--");
    setText(r.unit, "");
    if (!d.label.length()) setText(r.name, pc ? (String(pc->name) + " offline").c_str() : "PC offline");
    lv_obj_set_style_opa(r.obj, LV_OPA_50, 0);
    return;
  }
  lv_obj_set_style_opa(r.obj, LV_OPA_COVER, 0);
  if (!d.label.length()) setText(r.name, m->name);
  MetricText mt;
  formatMetricText(*m, m->value, mt);
  setText(r.sub, mt.value);
  setText(r.unit, mt.unit);
  fitValue(r);

  const SlotHistory& hs = pc->hist[d.metric <= MAX_METRICS ? d.metric : 0];
  const int n = hs.metricId == d.metric ? hs.count : 0;
  float hi = -1e9f;
  for (int i = 0; i < n; i++) hi = max(hi, pcHistoryAt(hs, (uint8_t)i));
  // A rate without a set scale never warns; its bar is relative to the recent peak.
  const bool autoRate = metricIsRate(*m) && !d.scaleMax;
  const float scale = autoRate ? max(hi, m->value) : metricScaleMax(*m, d.scaleMax);
  const float pct = scale > 0 ? m->value * 100.0f / scale : 0;
  // Enter the warning colour at warnThresholdPct, leave it 5 points below.
  const float enter = (float)dispSettings.warnThresholdPct;
  const bool warn = !autoRate && enter > 0 && (r.warn ? pct >= enter - 5 : pct >= enter);
  if (warn != r.warn) {
    r.warn = warn;
    lv_obj_set_style_text_color(r.sub, lv_color_hex(warn ? C_WARN : tileTextRt(r)), 0);
  }
  const uint32_t accent = warn ? C_WARN : metricChart(d);
  if (r.viz && d.style == METRIC_BAR) {
    lv_bar_set_value(r.viz, (int32_t)constrain(pct, 0.0f, 100.0f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
  } else if (r.spark) {
    sparkFeed(r, *pc, hs, n, scale, autoRate, accent);
  }
}

lv_obj_t* makeRows(lv_obj_t* t, const TileDef& d) {
  if (!d.http.enabled || d.rows.empty()) return nullptr;
  const auto plain = [](lv_obj_t* o) {
    lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  };
  lv_obj_t* rows = lv_obj_create(t);
  lv_obj_remove_style_all(rows);
  plain(rows);
  for (const SensorRowDef& row : d.rows) {
    lv_obj_t* box = lv_obj_create(rows);
    lv_obj_remove_style_all(box);
    plain(box);
    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text(label, row.label.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_opa(label, LV_OPA_70, 0);
    lv_obj_t* bar = lv_bar_create(box);
    plain(bar);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t* value = lv_label_create(box);
    lv_label_set_text(value, "--");
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  }
  return rows;
}

lv_obj_t* makeDetail(lv_obj_t* t, const TileDef& d) {
  if (!d.http.enabled || !d.detail.length()) return nullptr;
  lv_obj_t* l = lv_label_create(t);
  lv_label_set_text(l, "");
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
  return l;
}

// Number big at the bottom, or a chart / bar under a smaller number (wide
// tiles only; fitMetric hides the chart on small ones).
void makeMetricContent(TileRt& r, lv_obj_t* t) {
  const TileDef& d = *r.def;
  const MetricStyle style = d.style;
  const bool sensor = d.type == TILE_SENSOR;
  const uint32_t accent = sensor ? sensorLine(r) : metricAccent(d);
  if (!sensor) lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only

  r.name = lv_label_create(t);
  lv_label_set_text(r.name, d.label.length() ? d.label.c_str() : sensor && !d.noName ? tileLabel(d).c_str() : "");
  // On glass the accent alone is hard to read: lift it toward white.
  const lv_color_t nameCol = lv_color_hex(accent);
  lv_obj_set_style_text_color(r.name, s_theme == THEME_GLASS ? lv_color_mix(lv_color_white(), nameCol, 110) : nameCol, 0);
  lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);

  // Value and unit side by side, bottoms aligned.
  lv_obj_t* row = lv_obj_create(t);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 4, 0);
  lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  r.sub = lv_label_create(row);
  lv_label_set_text(r.sub, "--");
  r.unit = lv_label_create(row);
  lv_label_set_text(r.unit, "");
  lv_obj_set_style_text_opa(r.unit, LV_OPA_70, 0);
  if (sensor) r.detail = makeDetail(t, d);

  // Fonts and places come from fitMetric once the tile has its size.
  if (style == METRIC_NUMBER) return;
  if (style == METRIC_BAR) {
    r.viz = lv_bar_create(t);
    lv_obj_set_size(r.viz, lv_pct(100), 8);
    lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(r.viz, 0, 100);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(metricChart(d)), LV_PART_INDICATOR);
    return;
  }
  // Chart: the number top right, the line under it. PC metrics draw their own
  // (sparkPaint) to glide between packets; makeTile hooks up its events.
  if (!sensor) {
    r.spark = (Spark*)lv_malloc_zeroed(sizeof(Spark));
    if (!r.spark) return;
    r.spark->gapMs = 1000;
    r.spark->key = -1;
    r.viz = lv_obj_create(t);
    lv_obj_remove_style_all(r.viz);
    lv_obj_set_size(r.viz, lv_pct(100), lv_pct(62));
    lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(r.viz, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return;
  }
  r.viz = lv_chart_create(t);
  lv_obj_remove_style_all(r.viz);
  lv_obj_set_size(r.viz, lv_pct(100), lv_pct(62));
  lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_chart_set_type(r.viz, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(r.viz, 0, 0);
  const int count = d.http.enabled ? HTTP_HISTORY_POINTS : HA_HISTORY_POINTS;
  lv_chart_set_point_count(r.viz, count);
  lv_obj_set_style_line_width(r.viz, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(r.viz, LV_OBJ_FLAG_CLICKABLE);
  r.ser = lv_chart_add_series(r.viz, lv_color_hex(sensorLine(r)), LV_CHART_AXIS_PRIMARY_Y);
  r.pts = (int32_t*)lv_malloc(sizeof(int32_t) * count);
  if (!r.pts) { lv_obj_delete(r.viz); r.viz = nullptr; r.ser = nullptr; return; }
  for (int i = 0; i < count; i++) r.pts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_series_ext_y_array(r.viz, r.ser, r.pts);
}

// A PC command tile shows what its PC reports: the volume, mute (lit while
// muted), the track. A fixed "sub" wins; only the portal's default icon and
// label follow (Play / pause then shows the track as its name).
void applyPcStateToTile(TileRt& r) {
  const TileDef& d = *r.def;
  if (d.tap.kind != ACT_PC || !r.sub) return;
  if (d.pcVolume && (r.sliding || pcVolHeld())) return;  // the finger's level stands
  const String& cmd = d.tap.cmd;
  const PcState* st = pcStateOf(pcSource(d.tap.pc));
  String sub;
  const char* icon = nullptr;
  bool on = false;
  if (st && cmd == "volume_mute") {
    on = st->mute == 1;
    sub = on ? String("Muted") : st->vol >= 0 ? String(st->vol) + "%" : String();
    if (st->mute >= 0) icon = on ? "volume-off" : "volume-high";
  } else if (st && (cmd == "volume_up" || cmd == "volume_down")) {
    if (st->vol >= 0) sub = String(st->vol) + "%";
  } else if (st && cmd.startsWith("media_") && st->media >= 0) {
    sub = st->title;
    if (st->artist[0]) sub += sub.length() ? String(" - ") + st->artist : String(st->artist);
    if (cmd == "media_play_pause") icon = st->media == PC_MEDIA_PLAYING ? "pause" : "play";
  }
  bool changed = false;
  if (d.pcVolume) {
    const float f = st && st->vol >= 0 ? st->vol / 100.0f : 0;
    if (fabsf(f - r.frac) > 0.001f) {
      setSliderFill(r, f);
      changed = true;
    }
  }
  if (cmd == "media_play_pause" && r.name && (d.label.isEmpty() || d.label == "Play / pause")) {
    String name = tileLabel(d);
    if (st && st->media >= 0 && st->title[0]) {
      name = st->title;
      sub = st->artist;
    }
    if (name != lv_label_get_text(r.name)) {
      lv_label_set_text(r.name, name.c_str());
      fitNameText(r);
      changed = true;
    }
  }
  if (!d.sub.length() && sub != lv_label_get_text(r.sub)) {
    lv_label_set_text(r.sub, sub.c_str());
    changed = true;
  }
  if (on != r.on) {
    setTileOn(r, on);
    changed = true;
  }
  if ((cmd == "volume_mute" || cmd == "media_play_pause") &&
      (d.icon.isEmpty() || d.icon == "mdi:volume-mute" || d.icon == "mdi:play-pause")) {
    const uint32_t was = r.iconCp;
    const lv_font_t* f = iconFont(r.iconPx);
    const uint32_t cp = icon && f ? iconCodepoint(icon) : 0;
    if (cp && cp != r.iconCp) {
      char u[5];
      iconUtf8(cp, u);
      lv_obj_set_style_text_font(r.icon, f, 0);
      lv_label_set_text(r.icon, u);
      r.iconCp = cp;
    } else if (!cp) {
      r.iconCp = showIcon(r.icon, d, nullptr, false, r.iconPx, r.iconCp);  // back to the default
    }
    changed = changed || r.iconCp != was;
  }
  if (changed) markPageStale(r.page);
}

// Clock tile text; "--:--" until the time is synced.
void applyClock(TileRt& r) {
  static const char* const kDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char h[4] = "--", m[4] = "--", date[20] = "";
  const char* ap = "";
  const time_t epoch = time(nullptr);
  if (epoch >= 1609459200) {
    struct tm lt;
    localtime_r(&epoch, &lt);
    snprintf(h, sizeof(h), netSettings.use24h ? "%02d" : "%d",
             netSettings.use24h ? lt.tm_hour : (lt.tm_hour + 11) % 12 + 1);
    snprintf(m, sizeof(m), "%02d", lt.tm_min);
    if (!netSettings.use24h) ap = lt.tm_hour < 12 ? "AM" : "PM";
    snprintf(date, sizeof(date), "%s %d %s", kDay[lt.tm_wday], lt.tm_mday, kMon[lt.tm_mon]);
  }
  setText(r.sub, h);
  setText(r.mins, m);
  setText(r.unit, ap);
  setText(r.name, date);
  markPageStale(r.page);
}

uint32_t s_pcSeen = 0;

// A companion packet or an online change of any PC: refresh metric tiles.
void pcSync(bool force) {
  if (!force && pcVersion() == s_pcSeen) return;
  s_pcSeen = pcVersion();
  for (TileRt& r : s_tiles) {
    if (r.def->type == TILE_MEDIA) applyMedia(r);
    if (r.def->tap.kind == ACT_PC) applyPcStateToTile(r);
    if (r.def->type != TILE_METRIC) continue;
    applyPcToTile(r);
    markPageStale(r.page);
  }
}

}  // namespace deckui
