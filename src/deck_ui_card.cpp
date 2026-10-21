// Deck UI: toast, detail card, confirm card and the PC pairing card.
#include "deck_ui_int.h"

namespace deckui {

// --- Toast and detail card ------------------------------------------------------------
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_modalScrim = nullptr;
lv_obj_t* s_modalCard = nullptr;
lv_draw_buf_t* s_modalGlass = nullptr;  // Glass theme: the card's frosted background
int s_modalTile = -1;
lv_obj_t* s_modalState = nullptr;
lv_obj_t* s_modalBtnLbl = nullptr;
lv_obj_t* s_modalSlider = nullptr;
lv_obj_t* s_modalIcon = nullptr;
uint32_t s_modalIconCp = 0;
lv_obj_t* s_modalChart = nullptr;
lv_obj_t* s_modalRange = nullptr;
lv_chart_series_t* s_modalSeries = nullptr;
int32_t* s_modalPoints = nullptr;
uint32_t s_modalHistoryVersion = 0;
uint8_t s_modalHours = 24;
float s_modalMin = NAN, s_modalMax = NAN;
// Confirm dialog (shares the card slot): which action waits, the tile state it
// was asked for, and the auto-cancel timer.
int8_t s_confirmHold = -1;  // -1 = the card is a detail card
bool s_confirmOn = false;
uint32_t s_confirmMs = 0;
lv_timer_t* s_confirmTimer = nullptr;
constexpr uint32_t CONFIRM_TIMEOUT_MS = 8000;
constexpr uint32_t CONFIRM_ARM_MS = 400;  // a stray second tap must not confirm

bool modalShown() { return s_modalCard != nullptr; }

void setModalOpen(bool open) {
  portENTER_CRITICAL(&s_mux);
  sh.modalOpen = open;
  portEXIT_CRITICAL(&s_mux);
}

void toastOpa(void* obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }
void toastDone(lv_anim_t* a) {
  if (lv_obj_get_style_opa((lv_obj_t*)a->var, LV_PART_MAIN) == LV_OPA_TRANSP)
    lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
}

void deckToastShow(const char* text, uint32_t holdMs) {
  if (!s_toast) {
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 9, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0xE6E8EC), 0);  // dark pill on both themes
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_label_create(s_toast);
  }
  lv_obj_t* label = lv_obj_get_child(s_toast, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_width(label, logicalW() - 40 - 32, 0);
  lv_label_set_text(label, text);
  // Above the page dots, which the flush task draws at the very bottom.
  lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast);  // above cards and the editor made after it
  lv_anim_delete(s_toast, toastOpa);
  lv_anim_t in;
  lv_anim_init(&in);
  lv_anim_set_var(&in, s_toast);
  lv_anim_set_exec_cb(&in, toastOpa);
  lv_anim_set_values(&in, lv_obj_get_style_opa(s_toast, LV_PART_MAIN), LV_OPA_COVER);
  lv_anim_set_duration(&in, 120);
  lv_anim_start(&in);
  lv_anim_t out;
  lv_anim_init(&out);
  lv_anim_set_var(&out, s_toast);
  lv_anim_set_exec_cb(&out, toastOpa);
  lv_anim_set_values(&out, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&out, 120 + holdMs);
  lv_anim_set_duration(&out, 250);
  lv_anim_set_completed_cb(&out, toastDone);
  lv_anim_start(&out);
}

void closeModal() {
  if (s_modalSlider && s_modalTile >= 0 && s_modalTile < (int)s_tiles.size()) {
    if (Wish* w = findWish(s_tiles[s_modalTile].def->entity)) {
      if (w->holding) w->level = {};
      w->dragging = w->holding = false;
    }
  }
  if (s_modalCard) lv_obj_delete(s_modalCard);
  if (s_modalScrim) lv_obj_delete(s_modalScrim);
  s_modalCard = s_modalScrim = nullptr;
  if (s_modalGlass) lv_draw_buf_destroy(s_modalGlass);
  s_modalGlass = nullptr;
  s_modalSlider = nullptr;
  s_modalIcon = nullptr;
  s_modalChart = s_modalRange = nullptr;
  s_modalSeries = nullptr;
  heap_caps_free(s_modalPoints);
  s_modalPoints = nullptr;
  s_modalHistoryVersion = 0;
  s_modalMin = s_modalMax = NAN;
  haDetailHistory("");
  s_modalTile = -1;
  s_confirmHold = -1;
  if (s_confirmTimer) lv_timer_delete(s_confirmTimer);
  s_confirmTimer = nullptr;
  setModalOpen(false);
}

void onModalScrim(lv_event_t*) { closeModal(); }

void onModalPrimary(lv_event_t*) {
  if (s_modalTile < 0 || s_modalTile >= (int)s_tiles.size()) return;
  TileRt& r = s_tiles[s_modalTile];
  if (needsConfirm(r, r.def->tap)) {
    openConfirm(s_modalTile, false);  // replaces this card
    return;
  }
  runAction(r, r.def->tap);
  refreshModal();
}

// The card gets a level slider for lights and for entities that report a level.
bool modalHasSlider() {
  if (s_modalTile < 0) return false;
  const String& id = s_tiles[s_modalTile].def->entity;
  const String d = entityDomain(id);
  if (d == "light") return true;
  HaEntity e;
  return sliderDomain(d) && haGetEntity(id, e) && sliderFrac(d, e) >= 0;
}

void sendLevel(int pct, bool dragging) {
  if (s_modalTile < 0) return;
  const String& entity = s_tiles[s_modalTile].def->entity;
  if (dragging) setWish(entity, true, pct / 100.0f, true);
  else releaseLevel(entity);
}

void onModalSlider(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED && s_modalTile >= 0)
    holdLevel(s_tiles[s_modalTile].def->entity, lv_slider_get_value(s_modalSlider) / 100.0f);
  else if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    sendLevel(lv_slider_get_value(s_modalSlider), code == LV_EVENT_VALUE_CHANGED);
}

void refreshModal() {
  if (s_modalTile < 0 || !s_modalCard) return;
  const TileRt& r = s_tiles[s_modalTile];
  if (s_confirmHold >= 0) {
    // The state changed under the question (HA, another device): ask again.
    if (r.on != s_confirmOn) closeModal();
    return;
  }
  String st = tileSub(r);
  HaEntity e;
  const bool haKnown = r.def->entity.length() && haGetEntity(r.def->entity, e);
  if (haKnown) st = wishText(r.def->entity, e, false);
  if (s_modalSlider) {
    const float f = haKnown ? wishFrac(r.def->entity, e) : -1;
    const int pct = f > 0 ? (int)lroundf(f * 100) : 0;
    lv_slider_set_value(s_modalSlider, pct, LV_ANIM_OFF);
    lv_label_set_text(s_modalState, (haKnown ? wishText(r.def->entity, e, true) : st).c_str());
  } else {
    lv_label_set_text(s_modalState, st.c_str());
  }
  if (r.def->type == TILE_TOGGLE || (r.def->type == TILE_SLIDER && r.def->tap.kind == ACT_TOGGLE))
    lv_label_set_text(s_modalBtnLbl, actionVerb(r, r.def->tap));
  if (s_modalChart) {
    float value;
    const bool numeric = haKnown && e.known && haNumericState(e.state, value);
    if (numeric) {
      haDetailHistory(r.def->entity, s_modalHours);
      copyHistoryChart(*r.def, s_modalHours, s_modalHistoryVersion, s_modalChart,
                       s_modalPoints, s_modalMin, s_modalMax);
    }
    showObj(s_modalChart, numeric);
    showObj(s_modalRange, numeric);
    lv_chart_set_series_color(s_modalChart, s_modalSeries, lv_color_hex(sensorLine(r)));
    lv_obj_set_style_text_color(s_modalState, lv_color_hex(sensorColour(r)), 0);
    if (numeric) {
      char range[128];
      if (isfinite(s_modalMin))
        snprintf(range, sizeof(range), "Last %u h    Min %.3g    Max %.3g %s",
                 s_modalHours, s_modalMin, s_modalMax, e.unit.c_str());
      else snprintf(range, sizeof(range), "Last %u h    No history yet", s_modalHours);
      lv_label_set_text(s_modalRange, range);
    }
  }
  const bool on = haKnown && wishOn(r.def->entity, e);
  s_modalIconCp = showIcon(s_modalIcon, *r.def, haKnown ? &e : nullptr, on, 28, s_modalIconCp);
}

// Detail card for a tile (hold action "more-info"): name, entity, state, a
// level slider (light brightness, cover position...), and the tap action as a
// button.
void openModal(int tileIdx) {
  closeModal();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : domainColor(entityDomain(d.entity));
  s_modalTile = tileIdx;
  const bool light = modalHasSlider();
  HaEntity he;
  float hv;
  const bool haKnown = d.entity.length() && haGetEntity(d.entity, he);
  const bool graph = d.type == TILE_SENSOR && haKnown && he.known && haNumericState(he.state, hv);

  s_modalScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_modalScrim);
  lv_obj_set_size(s_modalScrim, w, h);
  lv_obj_set_style_bg_color(s_modalScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_modalScrim, LV_OPA_50, 0);
  lv_obj_add_event_cb(s_modalScrim, onModalScrim, LV_EVENT_CLICKED, nullptr);

  const int cw = w - 40 < 380 ? w - 40 : 380;
  const int ch = graph ? (light ? 304 : 284) : light ? 236 : 196;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS &&
      (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // taps on the card stay on the card
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 44, 44);
  lv_obj_set_pos(chip, 16, 16);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  styleIconChip(chip, accent);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  s_modalIcon = lv_label_create(chip);
  s_modalIconCp = 0;
  lv_label_set_text(s_modalIcon, "");
  lv_obj_center(s_modalIcon);

  // Name and entity id: one line each, cut with dots (ids get long).
  lv_obj_t* name = lv_label_create(c);
  String title = tileLabel(d);
  if (!d.label.length() && haKnown && he.name.length())
    title = he.name;
  lv_label_set_text(name, title.c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(name, cw - 88, lv_font_get_line_height(&deck_font_20));
  lv_obj_set_pos(name, 72, 15);
  lv_obj_t* ent = lv_label_create(c);
  lv_label_set_text(ent, d.entity.length() ? d.entity.c_str() : "No entity");
  lv_obj_set_style_text_color(ent, lv_color_hex(C_DIM), 0);
  lv_label_set_long_mode(ent, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(ent, cw - 88, lv_font_get_line_height(&deck_font_14));
  lv_obj_set_pos(ent, 72, 41);

  s_modalState = lv_label_create(c);
  lv_obj_set_style_text_font(s_modalState, &deck_font_28, 0);
  lv_label_set_long_mode(s_modalState, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(s_modalState, cw - 32, lv_font_get_line_height(&deck_font_28));
  lv_obj_set_pos(s_modalState, 16, 74);

  s_modalSlider = nullptr;
  if (light) {
    lv_obj_t* sl = lv_slider_create(c);
    s_modalSlider = sl;
    lv_obj_set_pos(sl, 26, 128);
    lv_obj_set_size(sl, cw - 52, 14);
    lv_slider_set_range(sl, 0, 100);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_PRESS_LOST, nullptr);
  }

  if (graph) {
    s_modalHours = d.style == METRIC_CHART ? d.hours : 24;
    s_modalPoints = (int32_t*)heap_caps_malloc(sizeof(int32_t) * HA_HISTORY_POINTS, MALLOC_CAP_SPIRAM);
    if (s_modalPoints) {
      s_modalChart = lv_chart_create(c);
      lv_obj_remove_style_all(s_modalChart);
      lv_obj_set_pos(s_modalChart, 16, light ? 154 : 112);
      lv_obj_set_size(s_modalChart, cw - 32, light ? 50 : 76);
      lv_chart_set_type(s_modalChart, LV_CHART_TYPE_LINE);
      lv_chart_set_div_line_count(s_modalChart, 0, 0);
      lv_chart_set_point_count(s_modalChart, HA_HISTORY_POINTS);
      lv_obj_set_style_line_width(s_modalChart, 2, LV_PART_ITEMS);
      lv_obj_set_style_width(s_modalChart, 0, LV_PART_INDICATOR);
      lv_obj_set_style_height(s_modalChart, 0, LV_PART_INDICATOR);
      lv_obj_remove_flag(s_modalChart, LV_OBJ_FLAG_CLICKABLE);
      s_modalSeries = lv_chart_add_series(s_modalChart, lv_color_hex(sensorLine(r)), LV_CHART_AXIS_PRIMARY_Y);
      for (int i = 0; i < HA_HISTORY_POINTS; i++) s_modalPoints[i] = LV_CHART_POINT_NONE;
      lv_chart_set_series_ext_y_array(s_modalChart, s_modalSeries, s_modalPoints);
      s_modalRange = lv_label_create(c);
      lv_obj_set_style_text_font(s_modalRange, &deck_font_12, 0);
      lv_label_set_long_mode(s_modalRange, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_size(s_modalRange, cw - 32, 18);
      lv_obj_set_pos(s_modalRange, 16, light ? 212 : 196);
    }
  }

  const int bw = (cw - 16 * 2 - 10) / 2;
  const int by = ch - 16 - 44;
  lv_obj_t* primary = qpButton(c, 16, by, bw, "", false);
  lv_obj_set_height(primary, 44);
  lv_obj_set_style_bg_color(primary, lv_color_hex(accent), 0);
  lv_obj_set_style_text_color(primary, lv_color_hex(C_TEXT_ON), 0);
  s_modalBtnLbl = lv_obj_get_child(primary, 0);
  lv_label_set_text(s_modalBtnLbl, "Run");
  lv_obj_add_event_cb(primary, onModalPrimary, LV_EVENT_CLICKED, nullptr);
  // Nothing to run when the tap only opens this card (sensors, numbers).
  const bool noPrimary = d.type == TILE_SENSOR || d.tap.kind == ACT_MORE_INFO;
  if (noPrimary) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* close = qpButton(c, noPrimary ? 16 : 16 + bw + 10, by, noPrimary ? cw - 32 : bw, "Close", false);
  lv_obj_set_height(close, 44);
  lv_obj_add_event_cb(close, onModalScrim, LV_EVENT_CLICKED, nullptr);

  setModalOpen(true);
  refreshModal();
}

void onConfirmOk(lv_event_t*) {
  if (s_modalTile < 0 || s_confirmHold < 0 || millis() - s_confirmMs < CONFIRM_ARM_MS) return;
  TileRt& r = s_tiles[s_modalTile];
  const TileAction& a = s_confirmHold ? r.def->hold : r.def->tap;
  closeModal();
  runAction(r, a);
}

void onConfirmTimeout(lv_timer_t*) {
  s_confirmTimer = nullptr;  // a one-shot timer deletes itself
  closeModal();
}

// "Turn off Printer?" card for tiles with "confirm". Cancel, the scrim, the
// timeout or a state change close it without running anything.
void openConfirm(int tileIdx, bool hold) {
  closeModal();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const TileAction& a = hold ? d.hold : d.tap;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : domainColor(entityDomain(d.entity));
  constexpr uint32_t C_DANGER = 0xE5534B;
  s_modalTile = tileIdx;
  s_confirmHold = hold ? 1 : 0;
  s_confirmOn = r.on;
  s_confirmMs = millis();

  s_modalScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_modalScrim);
  lv_obj_set_size(s_modalScrim, w, h);
  lv_obj_set_style_bg_color(s_modalScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_modalScrim, LV_OPA_50, 0);
  lv_obj_add_event_cb(s_modalScrim, onModalScrim, LV_EVENT_CLICKED, nullptr);

  const int cw = w - 40 < 360 ? w - 40 : 360;
  const int ch = 188;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS &&
      (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 44, 44);
  lv_obj_set_pos(chip, 16, 16);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  styleIconChip(chip, accent);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  s_modalIcon = lv_label_create(chip);
  lv_label_set_text(s_modalIcon, "");
  lv_obj_center(s_modalIcon);
  HaEntity he;
  const bool haKnown = d.entity.length() && haGetEntity(d.entity, he);
  s_modalIconCp = showIcon(s_modalIcon, d, haKnown ? &he : nullptr, r.on, 28, 0);

  String title = tileLabel(d);
  if (!d.label.length() && haKnown && he.name.length()) title = he.name;
  lv_obj_t* name = lv_label_create(c);
  lv_label_set_text(name, title.c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(name, cw - 88, lv_font_get_line_height(&deck_font_20));
  const String state = haKnown ? prettyState(entityDomain(d.entity), he) : tileSub(r);
  lv_obj_set_pos(name, 72, state.length() ? 15 : 26);
  if (state.length()) {
    lv_obj_t* st = lv_label_create(c);
    lv_label_set_text(st, state.c_str());
    lv_obj_set_style_text_color(st, lv_color_hex(C_DIM), 0);
    lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(st, cw - 88, lv_font_get_line_height(&deck_font_14));
    lv_obj_set_pos(st, 72, 41);
  }

  const char* verb = actionVerb(r, a);
  lv_obj_t* q = lv_label_create(c);
  lv_label_set_text_fmt(q, "%s?", verb);
  lv_obj_set_style_text_font(q, &deck_font_28, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(q, cw - 32, lv_font_get_line_height(&deck_font_28));
  lv_obj_set_pos(q, 16, 74);

  const int bw = (cw - 16 * 2 - 10) / 2;
  const int by = ch - 16 - 44;
  lv_obj_t* cancel = qpButton(c, 16, by, bw, "Cancel", false);
  lv_obj_set_height(cancel, 44);
  lv_obj_add_event_cb(cancel, onModalScrim, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ok = qpButton(c, 16 + bw + 10, by, bw, verb, false);
  lv_obj_set_height(ok, 44);
  lv_obj_set_style_bg_color(ok, lv_color_hex(C_DANGER), 0);
  lv_obj_set_style_text_color(ok, lv_color_white(), 0);
  lv_obj_add_event_cb(ok, onConfirmOk, LV_EVENT_CLICKED, nullptr);

  s_confirmTimer = lv_timer_create(onConfirmTimeout, CONFIRM_TIMEOUT_MS, nullptr);
  lv_timer_set_repeat_count(s_confirmTimer, 1);
  setModalOpen(true);
}

// The pairing card: Allow / Deny for a companion's request, or the code an
// older companion asked for, until answered, cancelled or timed out
// (pc_metrics owns the state).
lv_obj_t* s_pairCard = nullptr;
lv_obj_t* s_pairScrim = nullptr;
uint32_t s_pairChecked = 0;
String s_pairName, s_pairCode;

void pairClose() {
  if (s_pairCard) lv_obj_delete(s_pairCard);
  if (s_pairScrim) lv_obj_delete(s_pairScrim);
  s_pairCard = s_pairScrim = nullptr;
  s_pairName = s_pairCode = "";
  setModalOpen(s_modalCard != nullptr);
}

void onPairCancel(lv_event_t*) {
  pcPairCancel();
  lv_async_call([](void*) { pairClose(); }, nullptr);  // runs in the button's own event
}

void onPairAllow(lv_event_t*) {
  pcPairAllow();
  lv_async_call([](void*) { pairClose(); }, nullptr);
}

void pairSync() {
  const uint32_t now = millis();
  if (now - s_pairChecked < 250) return;
  s_pairChecked = now;
  String name, code;
  const bool pending = pcPairPending(name, code);
  if (!pending) {
    if (s_pairCard) pairClose();
    return;
  }
  if (s_pairCard && name == s_pairName && code == s_pairCode) return;
  if (s_pairCard) pairClose();
  s_pairName = name;
  s_pairCode = code;
  const int w = logicalW(), h = logicalH();
  s_pairScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_pairScrim);
  lv_obj_set_size(s_pairScrim, w, h);
  lv_obj_set_style_bg_color(s_pairScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_pairScrim, LV_OPA_50, 0);
  lv_obj_add_flag(s_pairScrim, LV_OBJ_FLAG_CLICKABLE);
  const int cw = w - 40 < 340 ? w - 40 : 340;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_pairCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, 196);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t = lv_label_create(c);
  lv_label_set_text_fmt(t, "Pair %s", name.c_str());
  lv_obj_set_style_text_font(t, &deck_font_20, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(t, cw - 32);
  lv_obj_set_pos(t, 16, 14);
  if (!code.length()) {
    // Tap mode: the companion waits for this answer.
    lv_obj_t* q = lv_label_create(c);
    lv_label_set_text_fmt(q, "Let this deck control %s: media, volume, lock, power and hotkeys?", name.c_str());
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(q, cw - 32);
    lv_obj_set_style_text_color(q, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(q, 16, 52);
    const int bw = (cw - 40) / 2;
    lv_obj_t* deny = qpButton(c, 16, 196 - 16 - 44, bw, "Deny", false);
    lv_obj_set_height(deny, 44);
    lv_obj_add_event_cb(deny, onPairCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* allow = qpButton(c, 24 + bw, 196 - 16 - 44, bw, "Allow", false);
    lv_obj_set_height(allow, 44);
    lv_obj_add_state(allow, LV_STATE_CHECKED);  // the accent look
    lv_obj_add_event_cb(allow, onPairAllow, LV_EVENT_CLICKED, nullptr);
    setModalOpen(true);
    return;
  }
  lv_obj_t* k = lv_label_create(c);
  String spaced = code.substring(0, 3) + " " + code.substring(3);
  lv_label_set_text(k, spaced.c_str());
  lv_obj_set_style_text_font(k, &deck_font_28, 0);
  lv_obj_set_style_text_letter_space(k, 4, 0);
  lv_obj_align(k, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_t* hint = lv_label_create(c);
  lv_label_set_text(hint, "Enter this code in TouchDeck Companion");
  lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 94);
  lv_obj_t* cancel = qpButton(c, 16, 196 - 16 - 44, cw - 32, "Cancel", false);
  lv_obj_set_height(cancel, 44);
  lv_obj_add_event_cb(cancel, onPairCancel, LV_EVENT_CLICKED, nullptr);
  setModalOpen(true);  // no swipes under it
}

}  // namespace deckui
