// Deck UI: fitting tile content to the tile size.
#include "deck_ui_int.h"

namespace deckui {

// --- Pages -----------------------------------------------------------------------------
lv_obj_t* makeRoot(int w, int h) {
  lv_obj_t* r = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, w, h);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  if (s_theme == THEME_GLASS && wallSharp()) lv_obj_set_style_bg_image_src(r, wallSharp(), 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

// --- PC metric tiles ------------------------------------------------------------------
// Identity colours by metric id.
constexpr uint32_t kMetricPalette[8] = {0x3987E5, 0x00A35C, 0xD55181, 0xC98500,
                                        0x199E70, 0xD95926, 0x9085E9, 0xE66767};

uint32_t metricAccent(const TileDef& d) {
  return d.hasColor ? d.color : kMetricPalette[(d.metric - 1) % 8];
}

uint32_t metricChart(const TileDef& d) { return d.hasChartColor ? d.chartColor : metricAccent(d); }
uint32_t tileText(const TileDef& d) { return d.hasTextColor ? d.textColor : C_TEXT; }

void setText(lv_obj_t* l, const char* text) {
  if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

// --- Tile fitting -----------------------------------------------------------------------
// A tile picks its padding, icon chip and type sizes from its pixel size (grid,
// span, top bar), so small tiles shrink text instead of overlapping it. Runs
// again on every size change.
constexpr int WIDE_W = 150;      // content width that earns the larger fonts

int lineH(const lv_font_t* f) { return lv_font_get_line_height(f); }
int textH(const lv_font_t* f, int lines) { return lines * lineH(f) + (lines - 1) * NAME_LINE_SP; }

void showObj(lv_obj_t* o, bool on) {
  if (!o) return;
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void setFont(lv_obj_t* o, const lv_font_t* f) {
  if (lv_obj_get_style_text_font(o, LV_PART_MAIN) != f) lv_obj_set_style_text_font(o, f, 0);
}

void fitName(lv_obj_t* name, const lv_font_t* f, int lines) {
  setFont(name, f);
  lv_obj_set_style_text_line_space(name, NAME_LINE_SP, 0);
  lv_obj_set_width(name, lv_pct(100));
  lv_obj_set_height(name, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(name, textH(f, lines), 0);
}

bool iconOff(const TileRt& r) { return r.def->icon == "none"; }

void fitChip(TileRt& r, int size) {
  lv_obj_set_size(r.chip, size, size);
  const int px = size >= 28 ? 20 : 16;
  if (px != r.iconPx) {
    r.iconPx = px;
    HaEntity e;
    const bool known = r.def->entity.length() && haGetEntity(r.def->entity, e);
    r.iconCp = showIcon(r.icon, *r.def, known ? &e : nullptr, r.on, px, 0);
  }
}

int textW(const char* text, const lv_font_t* f) {
  lv_point_t s;
  lv_text_get_size(&s, text, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return s.x;
}

// Lines of f that fit in h px, at most 3.
int linesIn(const lv_font_t* f, int h) {
  int n = 0;
  while (n < 3 && textH(f, n + 1) <= h) n++;
  return n;
}

// No word wider than the line (LVGL would break it mid-word) and the wrapped
// text within the lines.
bool nameFits(const char* text, const lv_font_t* f, int w, int lines) {
  char word[48];
  for (const char* p = text; *p;) {
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && p[n] != ' ') n++;
    if (n) {
      const size_t k = LV_MIN(n, sizeof(word) - 1);
      memcpy(word, p, k);
      word[k] = 0;
      if (textW(word, f) > w) return false;
    }
    p += n;
  }
  lv_point_t s;
  lv_text_get_size(&s, text, f, 0, NAME_LINE_SP, w, LV_TEXT_FLAG_NONE);
  return s.y <= textH(f, lines);
}

// The name font: the fitted one, or smaller until the name fits whole (12 px
// truncates). Up to three lines where the tile is tall enough.
void fitNameText(TileRt& r) {
  if (!r.nameFont) return;
  static const lv_font_t* const kSteps[] = {&deck_font_20, &deck_font_14, &deck_font_12};
  const char* text = lv_label_get_text(r.name);
  const lv_font_t* f = r.nameFont;
  int lines = 1;
  bool from = false;
  for (const lv_font_t* s : kSteps) {
    from = from || s == r.nameFont;
    if (!from) continue;
    f = s;
    lines = LV_MAX(1, linesIn(s, r.nameArea));
    if (nameFits(text, s, r.nameW, lines)) break;
  }
  fitName(r.name, f, lines);
}

// Action, toggle and slider: icon top left, state top right, name at the
// bottom; a one-line row on short wide tiles; the icon alone on tiny ones.
void fitButton(TileRt& r, int cw, int ch) {
  struct Opt {
    int chip;
    const lv_font_t* font;
    int lines;
    bool wideOnly;
  };
  static const Opt kOpts[] = {
      {30, &deck_font_20, 2, true}, {30, &deck_font_20, 1, true}, {30, &deck_font_14, 2, false},
      {22, &deck_font_12, 2, false}, {30, &deck_font_14, 1, false}, {22, &deck_font_12, 1, false},
  };
  // "icon": "none" keeps the layout and leaves the icon's corner empty.
  const bool icon = !iconOff(r);
  showObj(r.chip, icon);
  for (const Opt& o : kOpts) {
    if (cw < 40 || (o.wideOnly && cw < WIDE_W) || o.chip + textH(o.font, o.lines) > ch) continue;
    fitChip(r, o.chip);
    lv_obj_align(r.chip, LV_ALIGN_TOP_LEFT, 0, 0);
    r.nameFont = o.font;
    r.nameArea = ch - o.chip;
    r.nameW = cw;
    fitNameText(r);
    lv_obj_align(r.name, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    // The state beside the icon, smaller or gone when "Off" would not fit.
    const int room = cw - o.chip - 6;
    const lv_font_t* sf = o.chip >= 30 && textW("Off", &deck_font_14) <= room ? &deck_font_14 : &deck_font_12;
    setFont(r.sub, sf);
    lv_obj_set_size(r.sub, lv_pct(100), lineH(sf));
    lv_obj_set_style_pad_left(r.sub, icon ? o.chip + 6 : 0, 0);  // clear of the icon
    lv_obj_align(r.sub, LV_ALIGN_TOP_RIGHT, 0, (o.chip - lineH(sf)) / 2);
    showObj(r.name, true);
    showObj(r.sub, textW("Off", sf) <= room);
    return;
  }
  r.nameFont = nullptr;
  if (cw >= 80) {
    fitChip(r, 22);
    lv_obj_align(r.chip, LV_ALIGN_LEFT_MID, 0, 0);
    const bool state = cw >= 140;
    const int nameX = icon ? 28 : 0;
    fitName(r.name, &deck_font_12, 1);
    lv_obj_set_width(r.name, cw - nameX - (state ? 48 : 0));
    lv_obj_align(r.name, LV_ALIGN_LEFT_MID, nameX, 0);
    setFont(r.sub, &deck_font_12);
    lv_obj_set_size(r.sub, 44, lineH(&deck_font_12));
    lv_obj_set_style_pad_left(r.sub, 0, 0);
    lv_obj_align(r.sub, LV_ALIGN_RIGHT_MID, 0, 0);
    showObj(r.name, true);
    showObj(r.sub, state);
    return;
  }
  showObj(r.sub, false);
  if (!icon) {  // too small for the icon alone: the name takes its place
    fitName(r.name, &deck_font_12, LV_MAX(1, linesIn(&deck_font_12, ch)));
    lv_obj_set_width(r.name, cw);
    lv_obj_center(r.name);
    showObj(r.name, true);
    return;
  }
  const int s = LV_MIN(30, LV_MIN(cw, ch));
  fitChip(r, s);
  lv_obj_center(r.chip);
  showObj(r.name, false);
}

void fitDetail(TileRt& r, const lv_font_t* f, int w, lv_align_t align, int y, bool show) {
  if (!r.detail) return;
  showObj(r.detail, show);
  if (!show) return;
  setFont(r.detail, f);
  lv_obj_set_size(r.detail, w, lineH(f));
  lv_obj_align(r.detail, align, 0, y);
}

constexpr int ROW_BAR_MIN = 20;

// Sensor rows: labels and values take what they need, the bar the rest.
void layoutRows(TileRt& r) {
  const lv_font_t* f = r.valFont;
  if (!f || !r.rows) return;
  const TileDef& d = *r.def;
  const int n = LV_MIN((int)lv_obj_get_child_count(r.rows), (int)d.rows.size());
  bool templated = false;
  for (int i = 0; i < n; i++) templated = templated || d.rows[i].value.length();
  // Row value templates give way to the plain reading when the bar would not fit.
  const int gap = 6;
  int labelW = 0, valueW = 0, barX = 0, barW = 0;
  for (int pass = templated ? 0 : 1; pass < 2; pass++) {
    labelW = 0;
    valueW = textW(("100" + rowUnit(d)).c_str(), f);
    for (int i = 0; i < n; i++) {
      lv_obj_t* box = lv_obj_get_child(r.rows, i);
      setText(lv_obj_get_child(box, 2), rowText(d, i, pass == 0).c_str());
      labelW = LV_MAX(labelW, textW(lv_label_get_text(lv_obj_get_child(box, 0)), f));
      valueW = LV_MAX(valueW, textW(lv_label_get_text(lv_obj_get_child(box, 2)), f));
    }
    valueW = LV_MIN(valueW, r.valW - labelW);
    barX = labelW ? labelW + gap : 0;
    barW = r.valW - barX - valueW - gap;
    if (barW >= ROW_BAR_MIN) break;
  }
  for (int i = 0; i < n; i++) {
    lv_obj_t* box = lv_obj_get_child(r.rows, i);
    lv_obj_t* label = lv_obj_get_child(box, 0);
    lv_obj_t* bar = lv_obj_get_child(box, 1);
    lv_obj_t* value = lv_obj_get_child(box, 2);
    lv_obj_set_size(box, r.valW, r.rowH);
    lv_obj_set_pos(box, 0, i * r.rowH);
    setFont(label, f);
    setFont(value, f);
    lv_obj_set_size(label, labelW, lineH(f));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(value, valueW, lineH(f));
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
    showObj(bar, barW >= ROW_BAR_MIN);
    lv_obj_set_size(bar, LV_MAX(barW, 0), 6);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, barX, 0);
  }
}

// Sensor rows: the name and icon on top, the rows sharing the space under it.
void fitRows(TileRt& r, int cw, int ch) {
  struct Opt {
    const lv_font_t* name;
    const lv_font_t* row;
  };
  static const Opt kOpts[] = {{&deck_font_20, &deck_font_20}, {&deck_font_14, &deck_font_20},
                              {&deck_font_14, &deck_font_14}, {&deck_font_14, &deck_font_12},
                              {&deck_font_12, &deck_font_12}};
  const int n = lv_obj_get_child_count(r.rows);
  const Opt* o = &kOpts[4];
  for (const Opt& k : kOpts)
    if (lineH(k.name) + n * lineH(k.row) <= ch) { o = &k; break; }
  showObj(r.sub, false);
  showObj(r.detail, false);
  const int chip = iconOff(r) ? 0 : lineH(o->name) >= 22 ? 22 : 18;
  if (chip) {
    fitChip(r, chip);
    lv_obj_align(r.chip, LV_ALIGN_TOP_RIGHT, 2, -2);
  }
  showObj(r.chip, chip > 0);
  r.chipRoom = 0;
  r.nameFont = nullptr;
  fitName(r.name, o->name, 1);
  lv_obj_set_width(r.name, chip ? cw - chip - 4 : cw);
  lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);
  showObj(r.name, true);
  const int top = lineH(o->name);
  r.rowH = n ? LV_MAX(lineH(o->row), (ch - top) / n) : 0;
  lv_obj_set_size(r.rows, cw, ch - top);
  lv_obj_align(r.rows, LV_ALIGN_TOP_LEFT, 0, top);
  r.valFont = o->row;
  r.valW = cw;
  layoutRows(r);
}

// Sensor: the reading big at the top with a small icon top right, the name
// under it; a row (name left, reading right) on short tiles.
void fitSensor(TileRt& r, int cw, int ch) {
  if (r.rows) {
    fitRows(r, cw, ch);
    return;
  }
  struct Opt {
    const lv_font_t* val;
    const lv_font_t* name;
    int lines;
    bool wideOnly;
  };
  static const Opt kOpts[] = {
      {&deck_font_28, &deck_font_20, 2, true},  {&deck_font_28, &deck_font_20, 1, true},
      {&deck_font_28, &deck_font_14, 1, true},  {&deck_font_20, &deck_font_14, 2, false},
      {&deck_font_20, &deck_font_12, 2, false}, {&deck_font_20, &deck_font_14, 1, false},
      {&deck_font_20, &deck_font_12, 1, false}, {&deck_font_14, &deck_font_12, 1, false},
  };
  for (int pass = r.detail ? 0 : 1; pass < 2; pass++)
  for (const Opt& o : kOpts) {
    const lv_font_t* df = o.val == &deck_font_28 ? &deck_font_14 : &deck_font_12;
    const int dh = pass ? 0 : lineH(df);
    if ((o.wideOnly && cw < WIDE_W) || lineH(o.val) + textH(o.name, o.lines) + dh > ch) continue;
    const int chip = iconOff(r) ? 0 : o.val == &deck_font_14 ? 18 : 22;
    if (chip) {
      fitChip(r, chip);
      lv_obj_align(r.chip, LV_ALIGN_TOP_RIGHT, 2, -2);
    }
    showObj(r.chip, chip > 0);
    lv_obj_set_style_pad_right(r.sub, 0, 0);
    setFont(r.sub, o.val);
    lv_obj_set_size(r.sub, lv_pct(100), lineH(o.val));
    lv_obj_set_style_text_align(r.sub, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(r.sub, LV_ALIGN_TOP_LEFT, 0, 0);
    r.valFont = o.val;
    r.valW = cw - chip;
    r.chipRoom = chip;
    fitValue(r);
    r.nameFont = o.name;
    r.nameArea = ch - lineH(o.val) - dh;
    r.nameW = cw;
    fitNameText(r);
    lv_obj_align(r.name, LV_ALIGN_BOTTOM_LEFT, 0, -dh);
    showObj(r.name, true);
    fitDetail(r, df, cw, LV_ALIGN_BOTTOM_LEFT, 0, !pass);
    return;
  }
  const lv_font_t* vf = ch >= lineH(&deck_font_20) ? &deck_font_20 : &deck_font_14;
  showObj(r.detail, false);
  showObj(r.chip, false);
  r.chipRoom = 0;
  r.nameFont = nullptr;
  lv_obj_set_style_pad_right(r.sub, 0, 0);
  lv_obj_set_style_text_align(r.sub, LV_TEXT_ALIGN_RIGHT, 0);
  const bool name = cw >= 80;
  lv_obj_set_size(r.sub, name ? cw / 2 : cw, lineH(vf));
  lv_obj_align(r.sub, name ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER, 0, 0);
  r.valFont = vf;
  r.valW = name ? cw / 2 : cw;
  fitValue(r);
  fitName(r.name, &deck_font_12, 1);
  lv_obj_set_width(r.name, cw - cw / 2 - 4);
  lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);
  showObj(r.name, name);
}

// The largest font from `from` down that fits text in room, null if none.
const lv_font_t* valueFont(const lv_font_t* from, const char* text, int room) {
  static const lv_font_t* const kSteps[] = {&lv_font_montserrat_40, &deck_font_28, &deck_font_20,
                                            &deck_font_14, &deck_font_12};
  bool on = false;
  for (const lv_font_t* s : kSteps) {
    on = on || s == from;
    if (on && textW(text, s) <= room) return s;
  }
  return nullptr;
}

void setWidth(lv_obj_t* o, int w) {
  if (lv_obj_get_style_width(o, LV_PART_MAIN) != w) lv_obj_set_width(o, w);
}

constexpr int ROW_VIZ_MIN = 40;

int rowNameW(const TileRt& r) {
  const int t = textW(lv_label_get_text(r.name), lv_obj_get_style_text_font(r.name, LV_PART_MAIN));
  return LV_MIN(t, r.valW * 2 / 5);
}

// Chart or bar width between the name and a value sized for three digits, so
// it does not jump as the value changes.
int rowVizW(const TileRt& r, const lv_font_t* f, int unitW) {
  const int valueW = LV_MAX(textW(lv_label_get_text(r.sub), f), textW("000", f)) + unitW;
  return r.valW - rowNameW(r) - valueW - 20;
}

// Row: the name gets what the value leaves, or on a wide row shares it with
// the chart or bar.
void layoutRow(TileRt& r, const lv_font_t* f, int unitW) {
  const int w = r.row == 2 ? rowVizW(r, f, unitW) : 0;
  const bool viz = w >= ROW_VIZ_MIN;
  if (r.row == 2) showObj(r.viz, viz);
  if (!viz) {
    setWidth(r.name, LV_MAX(0, r.valW - textW(lv_label_get_text(r.sub), f) - unitW - 8));
    return;
  }
  const int x = rowNameW(r) + 10;
  setWidth(r.name, x - 10);
  setWidth(r.viz, w);
  if (lv_obj_get_style_x(r.viz, LV_PART_MAIN) != x) lv_obj_align(r.viz, LV_ALIGN_LEFT_MID, x, 0);
}

// Sensor reading or metric value: the largest font, from the fitted one down,
// that fits the room left next to the unit. The unit sits on the value's
// baseline, never larger than the value, and goes when nothing fits beside it.
void fitValue(TileRt& r) {
  if (!r.valFont) return;
  const char* text = lv_label_get_text(r.sub);
  int room = r.valW;
  if (r.besideName)
    room -= LV_MIN(textW(lv_label_get_text(r.name), lv_obj_get_style_text_font(r.name, LV_PART_MAIN)),
                   r.valW * (r.row ? 35 : 55) / 100) + 8;
  // A sensor's icon gives way when the reading does not fit beside it.
  if (r.chipRoom) {
    const bool chip = textW(text, r.valFont) <= room;
    showObj(r.chip, chip);
    lv_obj_set_style_pad_right(r.sub, chip ? r.chipRoom : 0, 0);
    if (!chip) room += r.chipRoom;
  }
  const lv_font_t* uf = r.unitFont;
  const int unitW = r.unit && uf ? 4 + textW(lv_label_get_text(r.unit), uf) : 0;
  const lv_font_t* f = valueFont(r.valFont, text, room - unitW);
  bool unit = unitW > 0;
  if (!f && unit) {
    unit = false;
    f = valueFont(r.valFont, text, room);
  }
  if (!f) f = &deck_font_12;
  // A wide row gives up the largest value font for its chart or bar.
  if (r.row == 2 && f == &deck_font_28 && rowVizW(r, f, unit ? unitW : 0) < ROW_VIZ_MIN) f = &deck_font_20;
  setFont(r.sub, f);
  if (r.unit && uf) {
    showObj(r.unit, unit);
    if (lineH(uf) >= lineH(f)) uf = &deck_font_12;
    setFont(r.unit, uf);
    const int pad = LV_MAX(0, f->base_line - uf->base_line);
    if (lv_obj_get_style_pad_bottom(r.unit, LV_PART_MAIN) != pad) lv_obj_set_style_pad_bottom(r.unit, pad, 0);
  }
  if (r.row) layoutRow(r, f, unit ? unitW : 0);
}

// PC metric: a chart or bar under a smaller number on wide tiles with the
// room, else the name on top and the number big at the bottom, else a row
// (a wide one keeps the chart or bar, between the name and the number).
void fitMetric(TileRt& r, int cw, int ch) {
  const TileDef& d = *r.def;
  lv_obj_t* row = lv_obj_get_parent(r.sub);
  // Bar: name, number and the 8 px bar stacked; chart: the line under the number.
  const lv_font_t* nf = ch >= 64 ? &deck_font_20 : &deck_font_14;
  const bool wideViz = r.viz && cw >= WIDE_W;
  const bool viz = wideViz &&
                   (d.style == METRIC_BAR ? ch >= lineH(nf) + lineH(&deck_font_28) * 3 / 4 + 10 : ch >= 44);
  showObj(r.viz, viz);
  r.besideName = viz && d.style != METRIC_BAR;
  r.row = 0;
  if (viz) {
    setFont(r.name, nf);
    lv_obj_set_size(r.name, d.style == METRIC_BAR ? lv_pct(100) : lv_pct(55), lineH(nf));
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);
    r.unitFont = &deck_font_14;
    r.valFont = &deck_font_28;
    lv_obj_set_width(r.viz, lv_pct(100));
    lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
    if (d.style == METRIC_BAR) {
      lv_obj_set_height(r.viz, 8);
      r.valW = cw;
      lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    } else {
      r.valW = cw;
      lv_obj_align(row, LV_ALIGN_TOP_RIGHT, 0, -6);
      const int top = lineH(nf) - 2 + lineH(&deck_font_12);
      const bool detail = r.detail && ch - top >= 24;
      lv_obj_set_height(r.viz, LV_MIN(ch * 62 / 100, detail ? ch - top : ch - 22));
      fitDetail(r, &deck_font_12, cw * 55 / 100, LV_ALIGN_TOP_LEFT, lineH(nf) - 2, detail);
    }
    showObj(r.name, true);
    fitValue(r);
    return;
  }
  // Digits have no accents or descenders: a quarter of the value's line box is air.
  struct Opt {
    const lv_font_t* name;
    const lv_font_t* val;
    const lv_font_t* unit;
    bool wideOnly;
  };
  static const Opt kOpts[] = {
      {&deck_font_20, &lv_font_montserrat_40, &deck_font_20, true},
      {&deck_font_14, &deck_font_28, &deck_font_14, false},
      {&deck_font_12, &deck_font_20, &deck_font_12, false},
  };
  for (int pass = r.detail ? 0 : 1; pass < 2 && !wideViz; pass++)
  for (const Opt& o : kOpts) {
    const int dh = pass ? 0 : lineH(&deck_font_12);
    if ((o.wideOnly && cw < WIDE_W) || lineH(o.name) + dh + lineH(o.val) * 3 / 4 > ch) continue;
    setFont(r.name, o.name);
    lv_obj_set_size(r.name, lv_pct(100), lineH(o.name));
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);
    r.unitFont = o.unit;
    r.valFont = o.val;
    r.valW = cw;
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    showObj(r.name, true);
    fitDetail(r, &deck_font_12, cw, LV_ALIGN_TOP_LEFT, lineH(o.name) - 2, !pass);
    fitValue(r);
    return;
  }
  showObj(r.detail, false);
  const bool name = cw >= 80;
  const lv_font_t* rf = cw >= WIDE_W && ch >= lineH(&deck_font_14) ? &deck_font_14 : &deck_font_12;
  setFont(r.name, rf);
  lv_obj_set_height(r.name, lineH(rf));
  lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);
  showObj(r.name, name);
  r.unitFont = name ? &deck_font_14 : &deck_font_12;
  r.valFont = lineH(&deck_font_28) * 3 / 4 <= ch ? &deck_font_28
              : lineH(&deck_font_20) * 3 / 4 <= ch ? &deck_font_20 : &deck_font_14;
  r.valW = cw;
  r.besideName = name;
  r.row = !name ? 0 : wideViz ? 2 : 1;
  if (r.row == 2) {
    lv_obj_set_height(r.viz, d.style == METRIC_BAR ? 6 : ch);
    lv_obj_align(r.viz, LV_ALIGN_LEFT_MID, 0, 0);  // layoutRow places it
  }
  lv_obj_align(row, name ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER, 0, 0);
  fitValue(r);
}

void fitTile(TileRt& r) {
  const int w = lv_obj_get_width(r.obj), h = lv_obj_get_height(r.obj);
  if (w <= 0 || h <= 0 || (w == r.fitW && h == r.fitH)) return;
  r.fitW = w;
  r.fitH = h;
  const int m = LV_MIN(w, h);
  const int pad = m >= 80 ? 9 : m >= 56 ? 7 : 5;
  lv_obj_set_style_pad_all(r.obj, pad, 0);
  const int cw = w - 2 * pad, ch = h - 2 * pad;
  if (r.def->type == TILE_METRIC || (r.def->type == TILE_SENSOR && r.def->style == METRIC_CHART)) fitMetric(r, cw, ch);
  else if (r.def->type == TILE_MEDIA) fitMedia(r, cw, ch);
  else if (r.def->type == TILE_CLOCK) fitClock(r, cw, ch);
  else if (r.def->type == TILE_PRINTER) fitPrinter(r, cw, ch);
  else if (r.def->type == TILE_SENSOR) fitSensor(r, cw, ch);
  else fitButton(r, cw, ch);
  markPageStale(r.page);
}

void fitEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx < s_tiles.size()) fitTile(s_tiles[idx]);
}

}  // namespace deckui
