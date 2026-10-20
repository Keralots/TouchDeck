// Deck UI: Bambu printer tiles.
#include "deck_ui_int.h"

namespace deckui {

// --- Printer tiles --------------------------------------------------------------------
// Bambu printers (deck_bambu): flex layouts inside the tile, so any grid size
// works; fitPrinter only picks fonts and drops what does not fit.
constexpr uint32_t C_PRN_OK = 0x4ADE80;    // printing
constexpr uint32_t C_PRN_BLUE = 0x7DB5FF;  // progress, idle

constexpr uint32_t C_PRN_BADGE_TEXT = 0x0B0F1C;

PrinterState* s_prn[MAX_PRINTERS] = {};  // last copy per s_cfg.printers index (PSRAM)
bool s_prnHave[MAX_PRINTERS] = {};

const PrinterState* printerFor(const TileDef& d) {
  for (size_t i = 0; i < s_cfg.printers.size() && i < MAX_PRINTERS; i++)
    if (s_cfg.printers[i].id == d.printer) return s_prnHave[i] ? s_prn[i] : nullptr;
  return nullptr;
}

const PrinterDef* printerDef(const String& id) {
  for (const PrinterDef& p : s_cfg.printers)
    if (p.id == id) return &p;
  return nullptr;
}

void readPrinters() {
  for (size_t i = 0; i < s_cfg.printers.size() && i < MAX_PRINTERS; i++) {
    if (!s_prn[i]) {
      void* mem = heap_caps_malloc(sizeof(PrinterState), MALLOC_CAP_SPIRAM);
      if (!mem) continue;
      s_prn[i] = new (mem) PrinterState();
    }
    s_prnHave[i] = printerRead(s_cfg.printers[i].id, *s_prn[i]);
  }
}

lv_obj_t* prnBox(lv_obj_t* parent, lv_flex_flow_t flow) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_flex_flow(o, flow);
  return o;
}

lv_obj_t* prnLabel(lv_obj_t* parent, const lv_font_t* f, lv_opa_t opa) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, "");
  setFont(l, f);
  if (opa != LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
  return l;
}

lv_obj_t* prnBar(lv_obj_t* parent, int h) {
  lv_obj_t* b = lv_bar_create(parent);
  lv_obj_set_size(b, lv_pct(100), h);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_PRN_BLUE), LV_PART_INDICATOR);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
  return b;
}

// Column layout: the first child grows, so the rest sits at the bottom.
void prnColumn(lv_obj_t* t) {
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(t, 4, 0);
}

lv_obj_t* prnSwatch(lv_obj_t* parent, int radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_white(), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
  return o;
}

void makePrinterContent(TileRt& r, lv_obj_t* t) {
  const TileDef& d = *r.def;
  prnColumn(t);
  switch (d.show) {
    case PS_AMS: {
      lv_obj_set_style_pad_row(t, 2, 0);
      r.px[0] = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(r.px[0], lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(r.px[0], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(r.px[0], 4, 0);
      r.name = prnLabel(r.px[0], &deck_font_12, LV_OPA_COVER);
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_flex_grow(r.name, 1);
      r.px[1] = prnBox(r.px[0], LV_FLEX_FLOW_ROW);
      lv_obj_set_size(r.px[1], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_align(r.px[1], LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(r.px[1], 3, 0);
      r.icon = prnLabel(r.px[1], &deck_font_12, LV_OPA_70);
      const uint32_t cp = iconCodepoint("mdi:water-percent");
      const lv_font_t* f = cp ? iconFont(14) : nullptr;
      if (f) {
        char u[5];
        iconUtf8(cp, u);
        setFont(r.icon, f);
        lv_label_set_text(r.icon, u);
      } else lv_label_set_text(r.icon, "RH");
      r.sub = prnLabel(r.px[1], &deck_font_12, LV_OPA_70);
      r.unit = prnLabel(r.px[1], &deck_font_12, LV_OPA_60);
      lv_obj_set_style_margin_left(r.unit, 3, 0);
      r.detail = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_width(r.detail, lv_pct(100));
      lv_obj_set_flex_grow(r.detail, 1);
      lv_obj_set_style_pad_column(r.detail, 3, 0);
      // Each column owns its ring (and colour disc), type and remaining label.
      for (int i = 0; i < PRN_AMS_SLOTS; i++) {
        lv_obj_t* col = prnBox(r.detail, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_size(col, 0, lv_pct(100));
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* arc = lv_arc_create(col);
        lv_obj_remove_style_all(arc);
        lv_obj_remove_flag(arc, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_set_size(arc, 36, 36);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_arc_set_rotation(arc, 270);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_10, LV_PART_MAIN);
        lv_obj_t* disc = prnSwatch(arc, LV_RADIUS_CIRCLE);
        lv_obj_set_size(disc, 27, 27);
        lv_obj_set_style_border_opa(disc, (lv_opa_t)(255 * 35 / 100), 0);
        lv_obj_center(disc);
        lv_obj_t* type = prnLabel(col, &deck_font_12, LV_OPA_COVER);
        lv_obj_set_width(type, lv_pct(100));
        lv_label_set_long_mode(type, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(type, LV_TEXT_ALIGN_CENTER, 0);
        // Tighten the font's spare leading so two lines fit below the spool.
        lv_obj_set_style_margin_top(type, -4, 0);
        lv_obj_t* remain = prnLabel(col, &deck_font_12, LV_OPA_60);
        lv_obj_set_style_margin_top(remain, -4, 0);
      }
      break;
    }
    case PS_SLOTS: {
      lv_obj_t* top = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(top, 5, 0);
      r.name = prnLabel(top, &deck_font_12, LV_OPA_70);
      lv_label_set_text(r.name, "Filament");
      r.chip = prnSwatch(top, LV_RADIUS_CIRCLE);
      lv_obj_set_size(r.chip, 8, 8);
      r.sub = prnLabel(top, &deck_font_12, LV_OPA_COVER);
      lv_obj_set_flex_grow(r.sub, 1);
      lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
      r.unit = prnLabel(top, &deck_font_12, LV_OPA_60);
      r.detail = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_width(r.detail, lv_pct(100));
      lv_obj_set_flex_grow(r.detail, 1);
      lv_obj_set_style_pad_column(r.detail, 8, 0);
      // Keep every group's bars allocated, including slots not reported yet.
      for (int u = 0; u < PRN_AMS_UNITS; u++) {
        lv_obj_t* group = prnBox(r.detail, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_size(group, 0, lv_pct(100));
        lv_obj_set_flex_grow(group, 40);
        lv_obj_set_style_pad_row(group, 2, 0);
        lv_obj_t* row = prnBox(group, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_pad_column(row, 3, 0);
        for (int i = 0; i < PRN_AMS_SLOTS; i++) {
          lv_obj_t* bar = prnSwatch(row, 5);
          lv_obj_set_size(bar, 0, lv_pct(100));
          lv_obj_set_flex_grow(bar, 1);
          lv_obj_set_style_border_opa(bar, (lv_opa_t)(255 * 28 / 100), 0);
        }
        lv_obj_t* name = prnLabel(group, &deck_font_12, LV_OPA_60);
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
      }
      break;
    }
    case PS_SUMMARY: {
      lv_obj_t* top = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(top, 5, 0);
      lv_obj_set_flex_grow(top, 1);
      r.px[0] = prnSwatch(top, LV_RADIUS_CIRCLE);
      lv_obj_set_size(r.px[0], 7, 7);
      r.name = prnLabel(top, &deck_font_14, LV_OPA_COVER);
      lv_label_set_text(r.name, printerDef(d.printer) ? printerDef(d.printer)->name.c_str() : d.printer.c_str());
      lv_obj_set_flex_grow(r.name, 1);
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      r.px[1] = prnLabel(top, &deck_font_12, LV_OPA_70);
      r.px[2] = prnSwatch(top, LV_RADIUS_CIRCLE);
      lv_obj_set_size(r.px[2], 8, 8);
      lv_obj_t* mid = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(mid, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
      lv_obj_set_style_pad_column(mid, 6, 0);
      r.sub = prnLabel(mid, &deck_font_28, LV_OPA_COVER);
      lv_obj_set_flex_grow(r.sub, 1);
      lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
      r.unit = prnLabel(mid, &deck_font_14, LV_OPA_COVER);
      lv_obj_set_style_text_color(r.unit, lv_color_hex(C_PRN_HEAT), 0);
      lv_obj_set_style_pad_bottom(r.unit, 3, 0);
      r.viz = prnBar(t, 5);
      break;
    }
    case PS_HERO: {
      lv_obj_t* top = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(top, 8, 0);
      lv_obj_set_flex_grow(top, 1);
      lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      r.name = prnLabel(top, &deck_font_12, LV_OPA_COVER);  // state badge
      lv_obj_set_style_bg_opa(r.name, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(r.name, 7, 0);
      lv_obj_set_style_pad_hor(r.name, 6, 0);
      lv_obj_set_style_pad_ver(r.name, 1, 0);
      lv_obj_set_style_text_color(r.name, lv_color_hex(C_PRN_BADGE_TEXT), 0);
      r.sub = prnLabel(top, &deck_font_14, LV_OPA_COVER);  // job
      lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_flex_grow(r.sub, 1);
      r.unit = prnLabel(top, &deck_font_12, LV_OPA_70);  // printer name
      lv_label_set_text(r.unit, printerDef(d.printer) ? printerDef(d.printer)->name.c_str() : "");
      lv_obj_t* mid = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(mid, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
      lv_obj_set_style_pad_column(mid, 10, 0);
      r.px[0] = prnLabel(mid, &deck_font_28, LV_OPA_COVER);  // time left
      r.px[1] = prnLabel(mid, &deck_font_12, LV_OPA_70);     // "left - done 17:28"
      lv_obj_set_style_pad_bottom(r.px[1], 4, 0);
      lv_label_set_long_mode(r.px[1], LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_flex_grow(r.px[1], 1);
      r.px[2] = prnLabel(mid, &deck_font_20, LV_OPA_COVER);  // percent
      lv_obj_set_style_text_color(r.px[2], lv_color_hex(C_PRN_BLUE), 0);
      r.viz = prnBar(t, 8);
      break;
    }
    case PS_NOZZLE:
    case PS_BED:
    case PS_CHAMBER: {
      lv_obj_t* top = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_flex_grow(top, 1);
      lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      r.name = prnLabel(top, &deck_font_12, LV_OPA_70);
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_flex_grow(r.name, 1);
      // The nozzle that is printing (dual nozzle printers).
      r.px[0] = lv_obj_create(top);
      lv_obj_remove_style_all(r.px[0]);
      lv_obj_set_size(r.px[0], 8, 8);
      lv_obj_set_style_radius(r.px[0], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(r.px[0], LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(r.px[0], lv_color_hex(C_PRN_HEAT), 0);
      lv_obj_add_flag(r.px[0], LV_OBJ_FLAG_HIDDEN);
      lv_obj_t* row = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
      lv_obj_set_style_pad_column(row, 3, 0);
      r.sub = prnLabel(row, &deck_font_28, LV_OPA_COVER);
      r.unit = prnLabel(row, &deck_font_14, LV_OPA_60);
      lv_obj_set_style_pad_bottom(r.unit, 4, 0);
      r.viz = prnBar(t, 3);
      break;
    }
    case PS_FILAMENT: {
      lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(t, 12, 0);
      r.chip = lv_obj_create(t);  // the spool's colour
      lv_obj_remove_style_all(r.chip);
      lv_obj_set_size(r.chip, 44, 44);
      lv_obj_set_style_radius(r.chip, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(r.chip, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(r.chip, 2, 0);
      lv_obj_set_style_border_color(r.chip, lv_color_white(), 0);
      lv_obj_set_style_border_opa(r.chip, LV_OPA_80, 0);
      lv_obj_t* col = prnBox(t, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_height(col, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(col, 1);
      lv_obj_set_style_pad_row(col, 2, 0);
      r.name = prnLabel(col, &deck_font_12, LV_OPA_70);
      lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_width(r.name, lv_pct(100));
      r.sub = prnLabel(col, &deck_font_20, LV_OPA_COVER);
      lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_width(r.sub, lv_pct(100));
      r.viz = prnBar(col, 4);
      lv_obj_set_style_margin_top(r.viz, 4, 0);
      // The next filaments of the job.
      lv_obj_t* next = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(next, LV_SIZE_CONTENT, lv_pct(80));
      lv_obj_set_style_pad_column(next, 3, 0);
      r.px[3] = next;
      for (int i = 0; i < 3; i++) {
        r.px[i] = lv_obj_create(next);
        lv_obj_remove_style_all(r.px[i]);
        lv_obj_set_size(r.px[i], 12, lv_pct(100));
        lv_obj_set_style_radius(r.px[i], 4, 0);
        lv_obj_set_style_bg_opa(r.px[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r.px[i], 1, 0);
        lv_obj_set_style_border_color(r.px[i], lv_color_white(), 0);
        lv_obj_set_style_border_opa(r.px[i], LV_OPA_30, 0);
        lv_obj_add_flag(r.px[i], LV_OBJ_FLAG_HIDDEN);
      }
      break;
    }
    case PS_LAYER: {
      r.name = prnLabel(t, &deck_font_12, LV_OPA_70);
      lv_label_set_text(r.name, "Layer");
      lv_obj_set_flex_grow(r.name, 1);
      r.sub = prnLabel(t, &deck_font_20, LV_OPA_COVER);
      r.unit = prnLabel(t, &deck_font_12, LV_OPA_60);
      lv_label_set_long_mode(r.unit, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_width(r.unit, lv_pct(100));
      break;
    }
    case PS_LIGHT: {
      r.chip = lv_obj_create(t);
      lv_obj_remove_flag(r.chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
      lv_obj_set_size(r.chip, 30, 30);
      lv_obj_set_style_radius(r.chip, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(r.chip, 0, 0);
      lv_obj_set_style_pad_all(r.chip, 0, 0);
      styleIconChip(r.chip, C_PRN_HEAT);
      r.icon = lv_label_create(r.chip);
      lv_obj_center(r.icon);
      const uint32_t cp = iconCodepoint("mdi:lightbulb");
      const lv_font_t* f = cp ? iconFont(18) : nullptr;
      if (f) {
        char u[5];
        iconUtf8(cp, u);
        lv_obj_set_style_text_font(r.icon, f, 0);
        lv_label_set_text(r.icon, u);
      } else lv_label_set_text(r.icon, LV_SYMBOL_CHARGE);
      lv_obj_set_flex_grow(r.chip, 0);
      lv_obj_t* fill = prnBox(t, LV_FLEX_FLOW_ROW);
      lv_obj_set_size(fill, 1, 1);
      lv_obj_set_flex_grow(fill, 1);
      r.name = prnLabel(t, &deck_font_14, LV_OPA_COVER);
      lv_label_set_text(r.name, "Light");
      r.sub = prnLabel(t, &deck_font_12, LV_OPA_70);
      break;
    }
  }
}

// A label cut with dots must have a fixed height, or it wraps instead.
void prnOneLine(lv_obj_t* l) {
  if (l) lv_obj_set_height(l, lineH(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
}

// Fonts by room; parts that cannot fit are hidden.
void fitPrinter(TileRt& r, int cw, int ch) {
  const TileDef& d = *r.def;
  switch (d.show) {
    case PS_AMS: {
      const bool narrow = cw < 150;
      lv_obj_set_flex_flow(r.px[0], narrow ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_grow(r.name, narrow ? 0 : 1);
      lv_obj_set_width(r.name, narrow ? lv_pct(100) : 0);
      lv_obj_set_width(r.px[1], narrow ? lv_pct(100) : LV_SIZE_CONTENT);
      prnOneLine(r.name);
      const PrinterState* s = printerFor(d);
      const int slots = s && d.amsUnit <= s->amsCount ? constrain(s->ams[d.amsUnit - 1].slots, 1, PRN_AMS_SLOTS) : 4;
      const int colw = LV_MAX(1, (cw - 3 * (slots - 1)) / slots);
      const int header = LV_MAX(lineH(&deck_font_12), lineH(lv_obj_get_style_text_font(r.icon, LV_PART_MAIN))) + (narrow ? lineH(&deck_font_12) : 0);
      const int room = LV_MAX(1, ch - header - 2), lines = 2 * (lineH(&deck_font_12) - 4);
      const bool labels = room >= 28 + lines && colw >= 32;
      const int size = LV_MAX(8, LV_MIN(36, LV_MIN(colw, room - (labels ? lines : 0))));
      for (int i = 0; i < PRN_AMS_SLOTS; i++) {
        lv_obj_t* col = lv_obj_get_child(r.detail, i);
        lv_obj_t* arc = lv_obj_get_child(col, 0);
        lv_obj_set_size(arc, size, size);
        lv_obj_set_size(lv_obj_get_child(arc, 0), LV_MAX(2, size - 9), LV_MAX(2, size - 9));
        showObj(lv_obj_get_child(col, 1), labels);
        showObj(lv_obj_get_child(col, 2), labels);
        prnOneLine(lv_obj_get_child(col, 1));
      }
      break;
    }
    case PS_SLOTS: {
      const PrinterState* s = printerFor(d);
      const int count = s ? LV_MIN(s->amsCount, PRN_AMS_UNITS) : 0;
      lv_obj_set_style_pad_column(r.detail, cw < 300 && count > 3 ? 4 : 8, 0);
      prnOneLine(r.sub);
      for (int u = 0; u < PRN_AMS_UNITS; u++) {
        lv_obj_t* group = lv_obj_get_child(r.detail, u);
        lv_obj_set_flex_grow(group, u < count && s->ams[u].slots == 1 ? 13 : 40);
        lv_obj_set_style_pad_column(lv_obj_get_child(group, 0), cw < 300 ? 2 : 3, 0);
        showObj(lv_obj_get_child(group, 1), ch >= 48);
        prnOneLine(lv_obj_get_child(group, 1));
      }
      break;
    }
    case PS_SUMMARY: {
      const lv_font_t* f = ch >= 70 ? &deck_font_28 : &deck_font_20;
      const int room = cw - (lv_obj_has_flag(r.unit, LV_OBJ_FLAG_HIDDEN) ? 0 : textW(lv_label_get_text(r.unit), &deck_font_14) + 6);
      if (textW(lv_label_get_text(r.sub), f) > room) f = &deck_font_14;
      setFont(r.sub, f);
      showObj(r.px[1], cw >= 150);
      prnOneLine(r.name);
      prnOneLine(r.sub);
      break;
    }
    case PS_HERO: {
      const bool wide = cw >= 300;
      showObj(r.sub, cw >= 200);
      showObj(r.unit, cw >= 200);
      showObj(r.px[1], wide);
      setFont(r.px[0], ch >= 70 && cw >= 200 ? &deck_font_28 : &deck_font_20);
      setFont(r.px[2], ch >= 70 ? &deck_font_20 : &deck_font_14);
      lv_obj_set_height(r.viz, ch >= 70 ? 8 : 5);
      prnOneLine(r.sub);
      prnOneLine(r.px[1]);
      break;
    }
    case PS_NOZZLE:
    case PS_BED:
    case PS_CHAMBER:
      setFont(r.sub, ch >= 70 && cw >= 86 ? &deck_font_28 : &deck_font_20);
      showObj(r.unit, cw >= 86);
      prnOneLine(r.name);
      break;
    case PS_FILAMENT: {
      const int c = LV_MIN(44, ch - 8);
      lv_obj_set_size(r.chip, c, c);
      showObj(r.px[3], cw >= 180);
      setFont(r.sub, ch >= 64 ? &deck_font_20 : &deck_font_14);
      showObj(r.name, ch >= 56);
      prnOneLine(r.name);
      prnOneLine(r.sub);
      break;
    }
    case PS_LAYER:
      setFont(r.sub, ch >= 64 ? &deck_font_20 : &deck_font_14);
      prnOneLine(r.unit);
      break;
    case PS_LIGHT:
      showObj(r.sub, ch >= 64);
      break;
  }
}

String prnDuration(uint16_t min) {
  char b[16];
  if (min < 60) snprintf(b, sizeof(b), "%um", min);
  else if (min < 1440) snprintf(b, sizeof(b), min % 60 ? "%uh %um" : "%uh", min / 60, min % 60);
  else snprintf(b, sizeof(b), "%ud %uh", min / 1440, min % 1440 / 60);
  return b;
}

// "PLA Matte" -> "PLA M", "PLA Basic" -> "PLA": the material stays readable
// under a spool where the full name would be cut.
String prnShortType(const char* type) {
  const char* sp = strchr(type, ' ');
  if (!sp) return type;
  String base(type, sp - type), rest(sp + 1);
  const int cut = rest.indexOf(' ');
  if (cut > 0) rest = rest.substring(0, cut);
  if (rest == "Basic") return base;
  if (rest.length() <= 2) return base + " " + rest;  // "PC FR", "PLA CF"
  static const char* const kAbbr[][2] = {{"Matte", "M"}, {"Metal", "Mt"}, {"Silk", "S"}, {"Wood", "W"},
                                         {"Translucent", "T"}, {"Marble", "Mb"}, {"Galaxy", "G"},
                                         {"Sparkle", "Sp"}, {"Glow", "Gl"}, {"Tough", "T+"}, {"Aero", "A"}};
  for (const auto& a : kAbbr)
    if (rest == a[0]) return base + " " + a[1];
  return base + " " + rest.substring(0, 2);
}

String prnUnitName(uint8_t id, bool shortName = false) {
  if (id >= 128) {
    String n = shortName ? "HT" : "AMS HT";
    if (id > 128) n += (shortName ? "" : " ") + String(id - 127);
    return n;
  }
  return (shortName ? String("") : String("AMS ")) + String((char)('A' + id));
}

// "AMS C, slot 4" for an ams[] index.
String prnSlotName(const PrinterState& s, uint8_t unit, uint8_t slot) {
  if (unit == 254) return "External spool";
  if (unit >= s.amsCount) return "";
  String n = prnUnitName(s.ams[unit].id);
  if (s.ams[unit].slots > 1) n += ", slot " + String(slot + 1);
  return n;
}

const PrinterTray* prnTray(const PrinterState& s, uint8_t unit, uint8_t slot) {
  if (unit == 254) return s.extPresent ? &s.ext : nullptr;
  if (unit >= s.amsCount || slot >= PRN_AMS_SLOTS || !s.ams[unit].tray[slot].present) return nullptr;
  return &s.ams[unit].tray[slot];
}

void prnBadge(TileRt& r, const char* text, uint32_t bg) {
  lv_label_set_text(r.name, text);
  lv_obj_set_style_bg_color(r.name, lv_color_hex(bg), 0);
}

void applyPrinterToTile(TileRt& r) {
  const TileDef& d = *r.def;
  if (d.type != TILE_PRINTER) return;
  const PrinterState* s = printerFor(d);
  const bool online = s && s->link == PRN_ONLINE && s->lastReportMs;
  lv_opa_t opa = online || d.show == PS_HERO ? LV_OPA_COVER : LV_OPA_50;
  char b[48];
  switch (d.show) {
    case PS_AMS: {
      const int u = d.amsUnit - 1;
      const PrinterAmsUnit* ams = online && u < s->amsCount && s->ams[u].present ? &s->ams[u] : nullptr;
      lv_label_set_text(r.name, ams ? prnUnitName(ams->id).c_str() : "AMS");
      showObj(r.detail, ams);
      showObj(r.icon, ams);
      showObj(r.unit, ams);
      if (!ams) {
        lv_label_set_text(r.sub, "--");
        lv_obj_set_style_text_color(r.sub, lv_color_hex(C_TEXT), 0);
        opa = LV_OPA_50;
        break;
      }
      if (ams->humidity <= 100) snprintf(b, sizeof(b), "%u%%", ams->humidity);
      else strcpy(b, "--");
      lv_label_set_text(r.sub, b);
      const uint32_t humidityColor = ams->humidity <= 100 && ams->humidity >= 40 ? C_PRN_HEAT : C_TEXT;
      lv_obj_set_style_text_color(r.sub, lv_color_hex(humidityColor), 0);
      lv_obj_set_style_text_color(r.icon, lv_color_hex(humidityColor), 0);
      if (isfinite(ams->temp)) snprintf(b, sizeof(b), "%.0f\xC2\xB0", ams->temp);
      else strcpy(b, "--");
      lv_label_set_text(r.unit, b);
      for (int i = 0; i < PRN_AMS_SLOTS; i++) {
        lv_obj_t* col = lv_obj_get_child(r.detail, i);
        showObj(col, i < ams->slots);
        lv_obj_t* arc = lv_obj_get_child(col, 0);
        lv_obj_t* disc = lv_obj_get_child(arc, 0);
        lv_obj_t* remain = lv_obj_get_child(col, 2);
        const PrinterTray& tray = ams->tray[i];
        const bool low = tray.present && tray.remain >= 0 && tray.remain <= 10;
        const bool active = tray.present && s->activeUnit == u && s->activeSlot == i;
        lv_arc_set_value(arc, tray.present && tray.remain >= 0 ? tray.remain : 100);
        lv_obj_set_style_arc_color(arc, lv_color_hex(low ? C_CRIT : C_TEXT), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, !tray.present ? LV_OPA_10 : tray.remain < 0 ? LV_OPA_30 : LV_OPA_COVER, LV_PART_INDICATOR);
        showObj(disc, tray.present);
        lv_obj_set_style_bg_color(disc, lv_color_hex(tray.rgb), 0);
        lv_obj_set_style_bg_opa(disc, tray.alpha < 128 ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(disc, active ? 2 : 1, 0);
        lv_obj_set_style_border_opa(disc, active ? LV_OPA_COVER : (lv_opa_t)(255 * 35 / 100), 0);
        lv_obj_t* type = lv_obj_get_child(col, 1);
        const int colw = lv_obj_get_width(col);
        const bool fits = colw > 0 && textW(tray.type, lv_obj_get_style_text_font(type, LV_PART_MAIN)) <= colw;
        lv_label_set_text(type, !tray.present ? "Empty" : fits ? tray.type : prnShortType(tray.type).c_str());
        if (!tray.present) b[0] = 0;
        else if (tray.remain < 0) strcpy(b, "?");
        else snprintf(b, sizeof(b), "%d%%", tray.remain);
        lv_label_set_text(remain, b);
        lv_obj_set_style_text_color(remain, lv_color_hex(low ? C_CRIT : C_TEXT), 0);
        lv_obj_set_style_text_opa(remain, low ? LV_OPA_COVER : LV_OPA_60, 0);
      }
      break;
    }
    case PS_SLOTS: {
      const PrinterTray* active = online ? prnTray(*s, s->activeUnit, s->activeSlot) : nullptr;
      showObj(r.chip, active);
      String feeding = online ? "Not feeding" : "--";
      if (active) {
        lv_obj_set_style_bg_color(r.chip, lv_color_hex(active->rgb), 0);
        lv_obj_set_style_bg_opa(r.chip, active->alpha < 128 ? LV_OPA_50 : LV_OPA_COVER, 0);
        feeding = s->activeUnit == 254 ? String("External") : prnUnitName(s->ams[s->activeUnit].id, true);
        if (s->activeUnit < s->amsCount && s->ams[s->activeUnit].slots > 1) feeding += String(s->activeSlot + 1);
        feeding += " " + String(active->type);
      }
      lv_label_set_text(r.sub, feeding.c_str());
      int count = 0;
      for (int u = 0; u < PRN_AMS_UNITS; u++) {
        lv_obj_t* group = lv_obj_get_child(r.detail, u);
        const PrinterAmsUnit* ams = online && u < s->amsCount && s->ams[u].present ? &s->ams[u] : nullptr;
        showObj(group, ams && ams->slots);
        if (!ams) continue;
        lv_label_set_text(lv_obj_get_child(group, 1), prnUnitName(ams->id, true).c_str());
        lv_obj_t* row = lv_obj_get_child(group, 0);
        for (int i = 0; i < PRN_AMS_SLOTS; i++) {
          lv_obj_t* bar = lv_obj_get_child(row, i);
          showObj(bar, i < ams->slots);
          if (i >= ams->slots) continue;
          count++;
          const PrinterTray& tray = ams->tray[i];
          const bool low = tray.present && tray.remain >= 0 && tray.remain <= 10;
          const bool feeding = tray.present && s->activeUnit == u && s->activeSlot == i;
          lv_obj_set_style_bg_color(bar, lv_color_hex(tray.rgb), 0);
          lv_obj_set_style_bg_opa(bar, !tray.present ? LV_OPA_TRANSP : tray.alpha < 128 ? LV_OPA_50 : LV_OPA_COVER, 0);
          lv_obj_set_style_border_width(bar, low || feeding ? 2 : 1, 0);
          lv_obj_set_style_border_color(bar, low ? lv_color_hex(C_CRIT) : lv_color_white(), 0);
          lv_obj_set_style_border_opa(bar, low || feeding ? LV_OPA_COVER : (lv_opa_t)(255 * 28 / 100), 0);
        }
      }
      snprintf(b, sizeof(b), "%d slots", count);
      lv_label_set_text(r.unit, online ? b : "--");
      break;
    }
    case PS_SUMMARY: {
      const bool powered = printerPowered(d.printer);
      const String st = powered && online ? String(s->gcodeState) : "";
      const bool running = st == "RUNNING" || st == "PREPARE" || st == "SLICING";
      const bool paused = st == "PAUSE";
      const bool starting = running && !s->layer && !s->progress;
      const char* stage = running ? printerStageName(s->stage) : nullptr;
      const char* state = !powered ? "Off" : !online ? "Connecting" : paused ? "Paused" :
                          running ? (starting || st != "RUNNING" ? "Preparing" : "Printing") :
                          st == "FINISH" ? "Done" : st == "FAILED" ? "Failed" : st == "IDLE" ? "Ready" : "Connecting";
      const uint32_t color = !powered || !online ? 0x9AA3B2 : paused ? C_PRN_HEAT : running ? C_PRN_OK :
                             st == "FAILED" ? C_CRIT : st == "IDLE" || st == "FINISH" ? C_PRN_BLUE : 0x9AA3B2;
      lv_label_set_text(r.px[1], state);
      lv_obj_set_style_bg_color(r.px[0], lv_color_hex(color), 0);
      const PrinterTray* tray = running ? prnTray(*s, s->activeUnit, s->activeSlot) : nullptr;
      showObj(r.px[2], tray);
      if (tray) {
        lv_obj_set_style_bg_color(r.px[2], lv_color_hex(tray->rgb), 0);
        lv_obj_set_style_bg_opa(r.px[2], tray->alpha < 128 ? LV_OPA_50 : LV_OPA_COVER, 0);
      }
      const bool progress = running || paused;
      if (running && (stage || starting)) lv_label_set_text(r.sub, stage ? stage : "Starting");
      else if (progress) {
        snprintf(b, sizeof(b), "%u%%", s->progress);
        lv_label_set_text(r.sub, b);
      } else lv_label_set_text(r.sub, state);
      showObj(r.unit, progress && !stage && !starting);
      lv_label_set_text(r.unit, progress ? prnDuration(s->remainingMin).c_str() : "");
      showObj(r.viz, progress);
      lv_bar_set_value(r.viz, progress ? s->progress : 0, LV_ANIM_OFF);
      opa = powered ? LV_OPA_COVER : LV_OPA_60;
      break;
    }
    case PS_HERO: {
      const String st = online ? String(s->gcodeState) : "";
      const bool running = st == "RUNNING" || st == "PREPARE" || st == "SLICING";
      const bool paused = st == "PAUSE";
      if (!online) {
        prnBadge(r, s && s->link == PRN_FAILED ? "OFFLINE" : "CONNECTING", 0x9AA3B2);
        lv_label_set_text(r.sub, s && s->link == PRN_FAILED ? s->err : "");
        lv_label_set_text(r.px[0], "--");
        lv_label_set_text(r.px[1], "");
        lv_label_set_text(r.px[2], "");
        lv_bar_set_value(r.viz, 0, LV_ANIM_OFF);
        break;
      }
      // RUNNING also covers the start-up stages (leveling, heating) and filament swaps.
      const char* stage = running ? printerStageName(s->stage) : nullptr;
      const bool starting = running && !s->layer && !s->progress;
      if (running && (stage || starting)) {
        prnBadge(r, starting ? "PREPARING" : "PRINTING", C_PRN_OK);
        lv_label_set_text(r.sub, s->job);
        lv_label_set_text(r.px[0], stage ? stage : "Starting");
        lv_label_set_text(r.px[1], "");
        snprintf(b, sizeof(b), "%u%%", s->progress);
        lv_label_set_text(r.px[2], starting ? "" : b);
        lv_bar_set_value(r.viz, s->progress, LV_ANIM_OFF);
      } else if (running || paused) {
        prnBadge(r, paused ? "PAUSED" : st == "RUNNING" ? "PRINTING" : "PREPARING", paused ? C_PRN_HEAT : C_PRN_OK);
        lv_label_set_text(r.sub, s->job);
        lv_label_set_text(r.px[0], prnDuration(s->remainingMin).c_str());
        String sub = "left";
        const time_t now = time(nullptr);
        if (now > 1609459200) {
          const time_t eta = now + (time_t)s->remainingMin * 60;
          struct tm lt;
          localtime_r(&eta, &lt);
          if (netSettings.use24h) snprintf(b, sizeof(b), " - done %02d:%02d", lt.tm_hour, lt.tm_min);
          else snprintf(b, sizeof(b), " - done %d:%02d %s", (lt.tm_hour + 11) % 12 + 1, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
          sub += b;
        }
        lv_label_set_text(r.px[1], sub.c_str());
        snprintf(b, sizeof(b), "%u%%", s->progress);
        lv_label_set_text(r.px[2], b);
        lv_bar_set_value(r.viz, s->progress, LV_ANIM_OFF);
      } else {
        const bool done = st == "FINISH", failed = st == "FAILED";
        prnBadge(r, done ? "DONE" : failed ? "FAILED" : "READY", done ? C_PRN_BLUE : failed ? C_CRIT : 0x9AA3B2);
        lv_label_set_text(r.sub, done || failed ? s->job : "");
        lv_label_set_text(r.px[0], done ? "Finished" : failed ? "Stopped" : "Ready");
        lv_label_set_text(r.px[1], "");
        lv_label_set_text(r.px[2], done ? "100%" : "");
        lv_bar_set_value(r.viz, done ? 100 : 0, LV_ANIM_OFF);
      }
      break;
    }
    case PS_NOZZLE:
    case PS_BED:
    case PS_CHAMBER: {
      float now = NAN, target = NAN;
      const char* label = d.show == PS_BED ? "Bed" : d.show == PS_CHAMBER ? "Chamber" : "Nozzle";
      bool active = false;
      if (s && d.show == PS_NOZZLE) {
        const uint8_t n = s->dualNozzle ? d.nozzle : 0;
        now = s->nozzle[n];
        target = s->nozzleTarget[n];
        if (s->dualNozzle) {
          label = n ? "Left nozzle" : "Right nozzle";
          active = online && s->activeNozzle == n;
        }
      } else if (s && d.show == PS_BED) {
        now = s->bed;
        target = s->bedTarget;
      } else if (s) now = s->chamber;
      lv_label_set_text(r.name, label);
      showObj(r.px[0], active);
      const bool heating = online && isfinite(target) && target > 0;
      if (online && isfinite(now)) snprintf(b, sizeof(b), "%.0f\xC2\xB0", now);
      else strcpy(b, "--");
      lv_label_set_text(r.sub, b);
      if (heating) snprintf(b, sizeof(b), "/%.0f", target);
      else b[0] = 0;
      lv_label_set_text(r.unit, b);
      const uint32_t c = heating ? C_PRN_HEAT : C_TEXT;
      lv_obj_set_style_text_color(r.sub, lv_color_hex(c), 0);
      lv_obj_set_style_bg_color(r.viz, lv_color_hex(c), LV_PART_INDICATOR);
      const float scale = heating ? target : d.show == PS_NOZZLE ? 300 : d.show == PS_BED ? 110 : 65;
      lv_bar_set_value(r.viz, online && isfinite(now) ? (int32_t)constrain(now * 100 / scale, 0.0f, 100.0f) : 0, LV_ANIM_OFF);
      break;
    }
    case PS_FILAMENT: {
      const PrinterTray* t = online ? prnTray(*s, s->activeUnit, s->activeSlot) : nullptr;
      if (t) {
        lv_obj_set_style_bg_color(r.chip, lv_color_hex(t->rgb), 0);
        lv_obj_set_style_bg_opa(r.chip, t->alpha < 128 ? LV_OPA_50 : LV_OPA_COVER, 0);
        const bool wide = lv_obj_get_content_width(r.obj) >= 300;
        lv_label_set_text(r.name, ((wide ? String("Feeding from ") : String("")) + prnSlotName(*s, s->activeUnit, s->activeSlot)).c_str());
        lv_label_set_text(r.sub, t->type);
        showObj(r.viz, t->remain >= 0);
        lv_bar_set_value(r.viz, t->remain >= 0 ? t->remain : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(r.viz, lv_color_hex(t->rgb), LV_PART_INDICATOR);
      } else {
        lv_obj_set_style_bg_color(r.chip, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(r.chip, LV_OPA_10, 0);
        snprintf(b, sizeof(b), "%u AMS unit%s", online ? s->amsCount : 0, online && s->amsCount == 1 ? "" : "s");
        lv_label_set_text(r.name, online ? b : "");
        lv_label_set_text(r.sub, online ? "Not feeding" : "--");
        showObj(r.viz, false);
      }
      // Up to three other filaments the job uses, in slicer order.
      int shown = 0;
      for (int i = 0; online && i < s->mapCount && shown < 3; i++) {
        const uint8_t raw = s->map[i] >> 8, slot = s->map[i] & 0xFF;
        uint8_t unit = 255;
        for (uint8_t u = 0; u < s->amsCount; u++)
          if (s->ams[u].id == raw) unit = u;
        if (unit == s->activeUnit && slot == s->activeSlot) continue;
        const PrinterTray* n = prnTray(*s, unit, slot);
        if (!n) continue;
        lv_obj_set_style_bg_color(r.px[shown], lv_color_hex(n->rgb), 0);
        showObj(r.px[shown++], true);
      }
      for (int i = shown; i < 3; i++) showObj(r.px[i], false);
      break;
    }
    case PS_LAYER: {
      if (online && s->totalLayers) {
        snprintf(b, sizeof(b), "%u", s->layer);
        lv_label_set_text(r.sub, b);
        static const char* const kSpeed[] = {"", "Silent", "Std", "Sport", "Ludicrous"};
        snprintf(b, sizeof(b), "of %u%s%s", s->totalLayers, s->speedLevel ? " - " : "",
                 kSpeed[s->speedLevel < 5 ? s->speedLevel : 0]);
        lv_label_set_text(r.unit, b);
      } else {
        lv_label_set_text(r.sub, "--");
        lv_label_set_text(r.unit, "");
      }
      break;
    }
    case PS_LIGHT: {
      const bool on = online && s->light == 1;
      setTileOn(r, on);
      lv_label_set_text(r.sub, !online ? "--" : s->light < 0 ? "Unknown" : on ? "On" : "Off");
      break;
    }
  }
  if (d.show == PS_AMS || d.show == PS_SLOTS || d.show == PS_SUMMARY)
    fitPrinter(r, lv_obj_get_content_width(r.obj), lv_obj_get_content_height(r.obj));
  lv_obj_set_style_opa(r.obj, opa, 0);
  markPageStale(r.page);
}

void printerTap(TileRt& r) {
  const TileDef& d = *r.def;
  if (d.show == PS_SUMMARY) {
    for (int i = 0; i < s_count; i++) {
      if (!s_pages[i].def || s_pages[i].def->printer != d.printer) continue;
      goToPage(s_pages[i].def->id);
      return;
    }
    return;
  }
  if (d.show != PS_LIGHT) return;
  const PrinterState* s = printerFor(d);
  if (!s || s->link != PRN_ONLINE || !printerLight(d.printer, s->light != 1)) {
    deckToastShow("Printer not connected", 1800);
    return;
  }
  setTileOn(r, s->light != 1);  // optimistic; the next report confirms it
  lv_label_set_text(r.sub, s->light != 1 ? "On" : "Off");
  markPageStale(r.page);
}

}  // namespace deckui
