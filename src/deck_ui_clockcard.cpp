// Deck UI: the card behind a clock tile. A countdown timer (one per deck) with
// a pill on the clock tiles and a melody at the end, and the clock style picker.
#include <esp_timer.h>
#include <math.h>
#include <time.h>

#include "audio.h"
#include "deck_ui_clock.h"
#include "haptic.h"
#include "psram_json.h"

namespace deckui {

namespace {
constexpr uint32_t C_TIMER = 0xF0B429;       // pill, progress, primary button
constexpr int TIMER_MAX_MIN = 180;
constexpr uint32_t TIMER_MAX_S = 5 * 3600;   // with +1 min taps
constexpr uint32_t RING_MS = 60000;          // the melody repeats this long
constexpr uint32_t RING_GAP_MS = 300;        // between two plays
constexpr uint32_t DONE_ARM_MS = 400;        // a stray tap must not silence the end
constexpr int PRESETS[6] = {1, 3, 5, 10, 15, 30};
constexpr const char* STYLE_NAMES[6] = {"Classic", "Roll", "Orbit", "Falling blocks", "Breakout", "Rocks"};

// --- Timer -------------------------------------------------------------------------
enum TimerState : uint8_t { TM_IDLE, TM_RUN, TM_PAUSE, TM_DONE };
TimerState s_tm = TM_IDLE;
int64_t s_tmEndUs = 0;   // TM_RUN
int64_t s_tmLeftUs = 0;  // TM_PAUSE
uint32_t s_tmTotal = 0;  // seconds
int s_tmSet = 5;         // minutes picked on the card
bool s_ring = false;
uint32_t s_ringMs = 0, s_ringGapMs = 0, s_doneMs = 0;

// --- Card --------------------------------------------------------------------------
enum CardKind : uint8_t { CK_NONE, CK_CLOCK, CK_DONE };
enum Act : uint8_t {
  A_CLOSE, A_TIMER, A_STYLE, A_START, A_CANCEL, A_PLUS, A_PAUSE, A_STOP, A_SNOOZE, A_LESS, A_MORE, A_GLIDE, A_TICK,
  A_PRESET = 32, A_PICK = 48
};
CardKind s_card = CK_NONE;
int s_clkTile = -1;
bool s_tabStyle = false;
bool s_rebuild = false;  // fill the card again on the next pass, outside its own events
int s_cw, s_ch, s_pad, s_gap, s_headH, s_btnH;
bool s_big, s_narrow;
lv_obj_t *s_now, *s_date, *s_left, *s_track, *s_ends, *s_chips[6];

// A style picked on the card, written to deck.json once the card is closed.
struct StylePending {
  bool on;
  int page, tile;  // in s_cfg
} s_save = {};
}  // namespace

static uint32_t timerLeft() {
  int64_t us = s_tm == TM_RUN ? s_tmEndUs - esp_timer_get_time() : s_tm == TM_PAUSE ? s_tmLeftUs : 0;
  return us <= 0 ? 0 : (uint32_t)((us + 999999) / 1000000);
}

static void timerText(uint32_t s, char* out, size_t n) {
  if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60), (unsigned)(s % 60));
  else snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void clockHm(time_t t, char* out, size_t n) {
  if (t < 1609459200) {
    snprintf(out, n, "--:--");
    return;
  }
  struct tm lt;
  localtime_r(&t, &lt);
  if (netSettings.use24h) snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
  else snprintf(out, n, "%d:%02d %s", (lt.tm_hour + 11) % 12 + 1, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
}

static void timerStart(uint32_t seconds) {
  s_tmTotal = seconds;
  s_tmEndUs = esp_timer_get_time() + (int64_t)seconds * 1000000;
  s_tm = TM_RUN;
  s_ring = false;
}

static void timerAdd(uint32_t seconds) {
  if (timerLeft() + seconds > TIMER_MAX_S) return;
  if (s_tm == TM_RUN) s_tmEndUs += (int64_t)seconds * 1000000;
  else if (s_tm == TM_PAUSE) s_tmLeftUs += (int64_t)seconds * 1000000;
  else return;
  s_tmTotal += seconds;
}

static void timerPause(bool pause) {
  if (pause && s_tm == TM_RUN) {
    s_tmLeftUs = LV_MAX(s_tmEndUs - esp_timer_get_time(), (int64_t)1);
    s_tm = TM_PAUSE;
  } else if (!pause && s_tm == TM_PAUSE) {
    s_tmEndUs = esp_timer_get_time() + s_tmLeftUs;
    s_tm = TM_RUN;
  }
}

static void timerIdle() {
  if (s_tm == TM_DONE) audioStopTimer();
  s_tm = TM_IDLE;
  s_ring = false;
}

// --- Style ---------------------------------------------------------------------------
static TileRt* cardTile() {
  if (s_clkTile < 0 || s_clkTile >= (int)s_tiles.size()) return nullptr;
  TileRt& r = s_tiles[s_clkTile];
  return r.def->type == TILE_CLOCK ? &r : nullptr;
}

static void styleSet(int style, bool tick) {
  TileRt* r = cardTile();
  if (!r || (style == r->def->clockStyle && tick == r->def->clockTick)) return;
  for (size_t p = 0; p < s_cfg.pages.size(); p++) {
    auto& tiles = s_cfg.pages[p].tiles;
    for (size_t t = 0; t < tiles.size(); t++) {
      if (&tiles[t] != r->def) continue;
      tiles[t].clockStyle = (uint8_t)style;
      tiles[t].clockTick = tick;
      s_save = {true, (int)p, (int)t};
      clockRestyle(*r);
      return;
    }
  }
}

// Writes the picked style into the stored deck.json; the pages already show it.
void clockStyleFlush() {
  if (!s_save.on) return;
  s_save.on = false;
  if (s_save.page >= (int)s_cfg.pages.size() || s_save.tile >= (int)s_cfg.pages[s_save.page].tiles.size()) return;
  const auto& tiles = s_cfg.pages[s_save.page].tiles;
  String text, err;
  deckConfigText(text);
  JsonDocument doc(psramJson());
  JsonObject tv;
  if (!deserializeJson(doc, text)) {
    int n = 0;
    for (JsonObject p : doc["pages"].as<JsonArray>()) {
      if (!strcmp(p["type"] | "tiles", "metrics")) continue;  // skipped by the parser
      if (n++ != s_save.page) continue;
      JsonArray a = p["tiles"].as<JsonArray>();
      if (a.size() == tiles.size()) tv = a[s_save.tile];
      break;
    }
  }
  bool ok = false;
  if (!tv.isNull() && !strcmp(tv["type"] | "", "clock")) {
    static const char* const kStyle[] = {"", "roll", "orbit", "blocks", "breakout", "rocks"};  // as in deckConfigParse
    const TileDef& d = tiles[s_save.tile];
    if (d.clockStyle) tv["style"] = kStyle[d.clockStyle];
    else tv.remove("style");
    if (d.clockTick) tv["ring"] = "tick";
    else tv.remove("ring");
    text = "";
    serializeJson(doc, text);
    DeckConfig same;
    ok = deckConfigStore(text, same, err);
  }
  if (ok) return;
  Serial.printf("Deck: clock style not saved: %s\n", err.c_str());
  deckToastShow("Clock style not saved", 1800);
}

// A new config came from elsewhere: it wins.
void clockStyleDrop() { s_save.on = false; }

// --- Card parts ------------------------------------------------------------------------
static void onAct(lv_event_t* e);

static lv_obj_t* actButton(lv_obj_t* p, int x, int y, int w, int h, const char* text, int act, bool primary = false) {
  lv_obj_t* b = qpButton(p, x, y, w, text, false);
  lv_obj_set_height(b, h);
  lv_obj_set_style_text_font(b, s_big ? &deck_font_20 : &deck_font_14, 0);
  if (primary) {
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TIMER), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(0x221602), 0);
  }
  lv_obj_add_event_cb(b, onAct, LV_EVENT_CLICKED, (void*)(uintptr_t)act);
  return b;
}

static lv_obj_t* text(lv_obj_t* p, const char* t, const lv_font_t* f, bool dim) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  if (dim) lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
  return l;
}

// n buttons of one row, evenly.
static int rowW(int n) { return (s_cw - 2 * s_pad - (n - 1) * s_gap) / n; }
static int rowX(int i, int n) { return s_pad + i * (rowW(n) + s_gap); }

static void showSet() {
  if (!s_left || s_track) return;
  char b[16];
  timerText(s_tmSet * 60, b, sizeof(b));
  setText(s_left, b);
  for (int i = 0; i < 6; i++) {
    if (!s_chips[i]) continue;
    const bool on = PRESETS[i] == s_tmSet;
    lv_obj_set_style_bg_color(s_chips[i], lv_color_hex(on ? C_TIMER : C_BUTTON), 0);
    lv_obj_set_style_text_color(s_chips[i], lv_color_hex(on ? 0x221602 : C_TEXT), 0);
  }
}

static void onStep(lv_event_t* e) {
  static int reps = 0;
  const lv_event_code_t code = lv_event_get_code(e);
  const int dir = (int)(intptr_t)lv_event_get_user_data(e);
  if (code == LV_EVENT_PRESSED) {
    reps = 0;
    return;
  }
  // Held: one minute at a time first, then five.
  const int by = code == LV_EVENT_LONG_PRESSED_REPEAT && ++reps > 10 ? 5 : 1;
  s_tmSet = LV_CLAMP(1, s_tmSet + dir * by, TIMER_MAX_MIN);
  showSet();
}

static void stepButton(lv_obj_t* p, int x, int y, int d, const char* sym, int dir) {
  lv_obj_t* b = qpButton(p, x, y, d, sym, false);
  lv_obj_set_height(b, d);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_font(b, s_big ? &deck_font_28 : &deck_font_20, 0);
  for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT})
    lv_obj_add_event_cb(b, onStep, c, (void*)(intptr_t)dir);
}

// A still picture of a style: the time, and what moves round it.
static void thumbDraw(lv_event_t* e) {
  const int st = (int)(intptr_t)lv_event_get_user_data(e);
  lv_layer_t* L = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
  const lv_font_t* f = s_big ? &deck_font_28 : &deck_font_20;
  const int cell = clockCellW(f), cw = lv_font_get_glyph_width(f, ':', 0), lh = lineH(f);
  const int x0 = a.x1 + (w - 4 * cell - cw) / 2, y0 = a.y1 + (h - lh) / 2;
  const lv_color_t col = lv_color_hex(C_OK);
  static const char* kTime = "19:45";
  for (int i = 0, x = x0; i < 5; i++) {
    const bool colon = i == 2, roll = st == CLOCK_ROLL && i == 4;
    if (roll) {
      drawGlyph(L, f, '4', x, y0 - lh / 3, cell, col, LV_OPA_50);
      drawGlyph(L, f, '5', x, y0 + lh / 3, cell, col);
    } else {
      drawGlyph(L, f, kTime[i], x, y0, colon ? cw : cell, col);
    }
    x += colon ? cw : cell;
  }
  auto rect = [&](int x, int y, int rw, int rh, uint32_t c) {
    const lv_area_t r = {a.x1 + x, a.y1 + y, a.x1 + x + rw - 1, a.y1 + y + rh - 1};
    fillRect(L, r, lv_color_hex(c));
  };
  auto line = [&](float x1, float y1, float x2, float y2, uint32_t c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(c);
    d.width = 1;
    d.p1.x = a.x1 + x1, d.p1.y = a.y1 + y1, d.p2.x = a.x1 + x2, d.p2.y = a.y1 + y2;
    lv_draw_line(L, &d);
  };
  if (st == CLOCK_ORBIT) {
    lv_draw_border_dsc_t d;
    lv_draw_border_dsc_init(&d);
    d.color = col;
    d.opa = LV_OPA_60;
    d.width = 2;
    d.radius = 8;
    lv_draw_border(L, &d, &a);
    const lv_area_t dot = {a.x1 + w * 2 / 3 - 3, a.y1 - 2, a.x1 + w * 2 / 3 + 3, a.y1 + 4};
    fillRect(L, dot, lv_color_white(), LV_RADIUS_CIRCLE);
  } else if (st == CLOCK_BLOCKS) {
    const int q = LV_MAX(3, h / 9);
    static const uint32_t kCol[4] = {0xE05FE0, 0x3FC4D8, 0xE8C93F, 0x4FC26A};
    for (int i = 0; i < 4; i++)
      for (int k = 0; k <= (i * 7 + 2) % 3; k++) {
        rect(2 + i * q, h - (k + 1) * q, q - 1, q - 1, kCol[i]);
        rect(w - 2 - (i + 1) * q, h - (k + 1) * q, q - 1, q - 1, kCol[(i + k) % 4]);
      }
  } else if (st == CLOCK_BREAKOUT) {
    rect(w * 2 / 5, h - 3, w / 5, 3, 0x8AB4FF);
    rect(w * 5 / 8, h - 9, 3, 3, 0xFFFFFF);
  } else if (st == CLOCK_ROCKS) {
    static const float kR[8] = {1, 0.8f, 0.95f, 0.7f, 1, 0.85f, 0.75f, 0.9f};
    auto rock = [&](float cx, float cy, float s) {
      for (int k = 0; k < 8; k++) {
        const float u = k * (float)M_PI / 4, v = (k + 1) * (float)M_PI / 4, p = kR[k] * s, q = kR[(k + 1) & 7] * s;
        line(cx + cosf(u) * p, cy + sinf(u) * p, cx + cosf(v) * q, cy + sinf(v) * q, 0xD9A066);
      }
    };
    rock(w * 0.06f + 2, h * 0.3f, h * 0.2f);
    rock(w * 0.94f - 2, h * 0.7f, h * 0.22f);
    line(6, h - 2, 9, h - 10, 0x5FE0E0);
    line(9, h - 10, 12, h - 2, 0x5FE0E0);
    line(12, h - 2, 6, h - 2, 0x5FE0E0);
  }
}

static void fillStyles(lv_obj_t* c, int top, int bottom) {
  TileRt* r = cardTile();
  if (!r) return;
  const int cols = s_narrow ? 2 : 3, rows = 6 / cols, g = s_gap;
  const int w = (s_cw - 2 * s_pad - (cols - 1) * g) / cols, h = (bottom - top - (rows - 1) * g) / rows;
  const lv_font_t* f = s_big ? &deck_font_20 : &deck_font_14;
  for (int i = 0; i < 6; i++) {
    lv_obj_t* b = actButton(c, s_pad + i % cols * (w + g), top + i / cols * (h + g), w, h, "", A_PICK + i);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_OK), 0);
    lv_obj_set_style_border_opa(b, i == r->def->clockStyle ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_t* l = lv_obj_get_child(b, 0);
    lv_label_set_text(l, STYLE_NAMES[i]);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, s_big ? -5 : -2);
    lv_obj_t* t = lv_obj_create(b);
    lv_obj_remove_style_all(t);
    lv_obj_remove_flag(t, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(t, LV_MIN(w - 14, s_big ? 130 : 96), h - lineH(f) - (s_big ? 18 : 9));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, s_big ? 9 : 4);
    lv_obj_add_event_cb(t, thumbDraw, LV_EVENT_DRAW_MAIN, (void*)(intptr_t)i);
    if (!clockStyleFits(*r, i)) {  // the tile would show Roll
      lv_obj_add_state(b, LV_STATE_DISABLED);
      lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    }
  }
  const bool orbit = r->def->clockStyle == CLOCK_ORBIT;
  const int n = orbit ? 3 : 1, y = s_ch - s_pad - s_btnH;
  if (orbit) {
    for (int i = 0; i < 2; i++) {
      lv_obj_t* b = actButton(c, rowX(i, 3), y, rowW(3), s_btnH, i ? "Tick" : "Glide", i ? A_TICK : A_GLIDE);
      if ((i == 1) != r->def->clockTick) continue;
      lv_obj_set_style_bg_color(b, lv_color_hex(C_OK), 0);
      lv_obj_set_style_text_color(b, lv_color_hex(0x0B1D17), 0);
    }
  }
  actButton(c, rowX(n - 1, n), y, rowW(n), s_btnH, "Close", A_CLOSE);
}

static void fillTimer(lv_obj_t* c, int top, int bottom) {
  const int y = s_ch - s_pad - s_btnH;
  const lv_font_t* bf = s_big ? &deck_font_clock_72 : &deck_font_clock_56;
  if (s_tm == TM_RUN || s_tm == TM_PAUSE) {
    const lv_font_t* sf = s_big ? &deck_font_20 : &deck_font_14;
    const int block = lineH(bf) + 2 * s_gap + 8 + lineH(sf);
    int at = top + (bottom - top - block) / 2;
    s_left = text(c, "", bf, false);
    lv_obj_set_width(s_left, s_cw - 2 * s_pad);
    lv_obj_set_style_text_align(s_left, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_left, s_pad, at);
    at += lineH(bf) + s_gap;
    s_track = lv_bar_create(c);
    lv_obj_set_pos(s_track, s_pad, at);
    lv_obj_set_size(s_track, s_cw - 2 * s_pad, 8);
    lv_bar_set_range(s_track, 0, 1000);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(C_TIMER), LV_PART_INDICATOR);
    s_ends = text(c, "", sf, true);
    lv_obj_set_width(s_ends, s_cw - 2 * s_pad);
    lv_obj_set_style_text_align(s_ends, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ends, s_pad, at + 8 + s_gap);
    actButton(c, rowX(0, 3), y, rowW(3), s_btnH, "Cancel", A_CANCEL);
    actButton(c, rowX(1, 3), y, rowW(3), s_btnH, "+1 min", A_PLUS);
    actButton(c, rowX(2, 3), y, rowW(3), s_btnH, s_tm == TM_RUN ? "Pause" : "Resume", A_PAUSE, true);
    return;
  }
  const int cols = s_narrow ? 3 : 6, h = s_big ? 54 : 40;
  for (int i = 0; i < 6; i++) {
    char b[12];
    snprintf(b, sizeof(b), "%d min", PRESETS[i]);
    s_chips[i] = actButton(c, rowX(i % cols, cols), top + i / cols * (h + s_gap), rowW(cols), h, b, A_PRESET + i);
    lv_obj_set_style_pad_hor(s_chips[i], 0, 0);
  }
  top += (6 / cols) * (h + s_gap);
  const int d = s_big ? 64 : 48, mid = (top + bottom) / 2;
  stepButton(c, s_pad + s_gap, mid - d / 2, d, LV_SYMBOL_MINUS, -1);
  stepButton(c, s_cw - s_pad - s_gap - d, mid - d / 2, d, LV_SYMBOL_PLUS, 1);
  const lv_font_t* f = s_narrow ? &lv_font_montserrat_40 : bf;
  s_left = text(c, "", f, false);
  lv_obj_set_width(s_left, s_cw - 2 * (s_pad + s_gap + d));
  lv_obj_set_style_text_align(s_left, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_left, s_pad + s_gap + d, mid - lineH(f) / 2);
  actButton(c, rowX(0, 2), y, rowW(2), s_btnH, "Close", A_CLOSE);
  actButton(c, rowX(1, 2), y, rowW(2), s_btnH, "Start", A_START, true);
  showSet();
}

static void bellAnim(void* o, int32_t v) {
  // Two short shakes, then a rest.
  lv_obj_set_style_translate_x((lv_obj_t*)o, v < 400 ? (int32_t)lroundf(sinf(v * (float)M_PI / 50) * 4) : 0, 0);
}

static void fillDone(lv_obj_t* c, int bottom) {
  const int d = s_big ? 76 : 56;
  const lv_font_t* tf = s_big ? &lv_font_montserrat_40 : &deck_font_28;
  const lv_font_t* sf = s_big ? &deck_font_20 : &deck_font_14;
  int at = (bottom - (d + lineH(tf) + lineH(sf) + 2 * s_gap)) / 2;
  lv_obj_t* o = lv_obj_create(c);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(o, d, d);
  lv_obj_align(o, LV_ALIGN_TOP_MID, 0, at);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(C_TIMER), 0);
  lv_obj_t* bell = text(o, LV_SYMBOL_BELL, &deck_font_28, false);
  lv_obj_set_style_text_color(bell, lv_color_hex(0x221602), 0);
  lv_obj_center(bell);
  if (s_ring) {
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, bell);
    lv_anim_set_exec_cb(&an, bellAnim);
    lv_anim_set_values(&an, 0, 1000);
    lv_anim_set_duration(&an, 1000);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&an);
  }
  at += d + s_gap;
  lv_obj_align(text(c, "Time's up", tf, false), LV_ALIGN_TOP_MID, 0, at);
  char b[16], t[32];
  timerText(s_tmTotal, b, sizeof(b));
  snprintf(t, sizeof(t), "%s timer", b);
  lv_obj_align(text(c, t, sf, true), LV_ALIGN_TOP_MID, 0, at + lineH(tf) + s_gap);
  const int y = s_ch - s_pad - s_btnH;
  actButton(c, rowX(0, 2), y, rowW(2), s_btnH, "+5 min", A_SNOOZE);
  actButton(c, rowX(1, 2), y, rowW(2), s_btnH, "Stop", A_STOP, true);
}

static void cardLive() {
  char b[40];
  if (s_now) {
    clockHm(time(nullptr), b, sizeof(b));
    setText(s_now, b);
    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char d[24], m[16];
    strftime(d, sizeof(d), s_big ? "%A" : "%a", &lt);
    strftime(m, sizeof(m), s_big ? "%B" : "%b", &lt);
    if (t < 1609459200) b[0] = 0;
    else snprintf(b, sizeof(b), "%s %d %s", d, lt.tm_mday, m);
    setText(s_date, b);
  }
  if (!s_track) return;
  const uint32_t left = timerLeft();
  timerText(left, b, sizeof(b));
  setText(s_left, b);
  lv_bar_set_value(s_track, s_tmTotal ? (int32_t)((uint64_t)left * 1000 / s_tmTotal) : 0, LV_ANIM_OFF);
  char total[16], end[16], t[64];
  timerText(s_tmTotal, total, sizeof(total));
  clockHm(time(nullptr) + left, end, sizeof(end));
  if (s_tm == TM_PAUSE) snprintf(t, sizeof(t), "Paused, %s set", total);
  else snprintf(t, sizeof(t), "of %s, ends at %s", total, end);
  setText(s_ends, t);
}

static void fillCard() {
  lv_obj_t* c = s_modalCard;
  lv_obj_clean(c);
  s_rebuild = false;
  s_now = s_date = s_left = s_track = s_ends = nullptr;
  memset(s_chips, 0, sizeof(s_chips));
  const int bottom = s_ch - s_pad - s_btnH - s_gap;
  if (s_card == CK_DONE) {
    fillDone(c, bottom);
    return;
  }
  s_now = text(c, "", s_big ? &lv_font_montserrat_40 : &deck_font_28, false);
  lv_obj_set_pos(s_now, s_pad, s_pad - (s_big ? 4 : 3));
  s_date = text(c, "", s_big ? &deck_font_20 : &deck_font_14, true);
  lv_obj_set_pos(s_date, s_pad, s_pad + s_headH - lineH(s_big ? &deck_font_20 : &deck_font_14) + 2);
  const int tw = s_big ? 100 : 64, th = s_headH - 8;
  lv_obj_t* tabs = lv_obj_create(c);
  lv_obj_remove_style_all(tabs);
  lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(tabs, 2 * tw + 8, th);
  lv_obj_set_pos(tabs, s_cw - s_pad - 2 * tw - 8, s_pad + 4);
  lv_obj_set_style_radius(tabs, 14, 0);
  lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tabs, lv_color_hex(C_BUTTON), 0);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* b = actButton(tabs, 4 + i * tw, 4, tw, th - 8, i ? "Style" : "Timer", i ? A_STYLE : A_TIMER);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 0, 0);
    if (s_tabStyle == (i == 1))
      lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_hex(C_TEXT), lv_color_hex(C_BUTTON), 45), 0);
  }
  const int top = s_pad + s_headH + s_gap;
  if (s_tabStyle) fillStyles(c, top, bottom);
  else fillTimer(c, top, bottom);
  cardLive();
}

static void openCard(CardKind kind, int tileIdx) {
  closeModal();
  cleanPage();
  const int w = logicalW(), h = logicalH();
  s_big = LV_MIN(w, h) >= 560;
  s_cw = LV_MIN(w - 40, s_big ? 600 : 440);
  s_narrow = s_cw < 400;
  s_ch = LV_MIN(h - 32, s_big ? 404 : s_narrow ? 350 : 288);
  s_pad = s_big ? 22 : 14;
  s_gap = s_big ? 14 : s_narrow ? 8 : 10;
  s_headH = s_big ? 64 : 46;
  s_btnH = s_big ? 54 : 44;
  const int x = (w - s_cw) / 2, y = (h - s_ch) / 2;
  const bool fx = kind == CK_CLOCK;  // the end opens plainly, like a confirm card
  if (fx) fxCardOpenBegin(tileIdx, {(int16_t)x, (int16_t)y, (int16_t)s_cw, (int16_t)s_ch}, 22);
  s_modalScrim = makeScrim(!fx);
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  s_card = kind;
  s_clkTile = fx ? tileIdx : -1;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, s_cw, s_ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS && (s_modalGlass = frostedCopy(x, y, s_cw, s_ch, 22))) frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  setModalOpen(true);
  fillCard();
  if (!fx) return;
  fxCardOpen(c, tileIdx);
  coverFill();
}

void openClockCard(int tileIdx) {
  if (tileIdx < 0 || tileIdx >= (int)s_tiles.size() || s_tiles[tileIdx].def->type != TILE_CLOCK) return;
  if (s_tm == TM_DONE) return;  // its card is coming
  s_tabStyle = false;
  openCard(CK_CLOCK, tileIdx);
}

static void cardHide() {
  if (s_card == CK_CLOCK) fxCardClose(s_modalCard, s_clkTile);
  else if (s_card == CK_DONE) closeModal();
}

// From closeModal, whoever closed the card.
void clockCardClosed() {
  s_card = CK_NONE;
  s_clkTile = -1;
  s_rebuild = false;
  s_now = s_date = s_left = s_track = s_ends = nullptr;
  memset(s_chips, 0, sizeof(s_chips));
}

static void stopDone() {
  if (millis() - s_doneMs < DONE_ARM_MS) return;
  timerIdle();
  cardHide();
}

// A tap beside the card; false when it is not the clock's.
bool clockCardScrim() {
  if (s_card == CK_NONE) return false;
  if (s_card == CK_DONE) stopDone();
  else cardHide();
  return true;
}

static void act(int a) {
  TileRt* r = cardTile();
  if (a >= A_PICK) {
    if (r && a - A_PICK < 6) styleSet(a - A_PICK, r->def->clockTick);
    s_rebuild = true;
    return;
  }
  if (a >= A_PRESET) {
    if (a - A_PRESET < 6) s_tmSet = PRESETS[a - A_PRESET];
    showSet();
    return;
  }
  switch (a) {
    case A_CLOSE: cardHide(); break;
    case A_TIMER:
    case A_STYLE:
      s_tabStyle = a == A_STYLE;
      s_rebuild = true;
      break;
    case A_START:
      timerStart(s_tmSet * 60);
      cardHide();
      break;
    case A_CANCEL:
      timerIdle();
      s_rebuild = true;
      break;
    case A_PLUS: timerAdd(60); break;
    case A_PAUSE:
      timerPause(s_tm == TM_RUN);
      s_rebuild = true;
      break;
    case A_STOP: stopDone(); break;
    case A_SNOOZE:
      if (millis() - s_doneMs < DONE_ARM_MS) break;
      timerIdle();
      timerStart(5 * 60);
      cardHide();
      break;
    case A_GLIDE:
    case A_TICK:
      if (r) styleSet(CLOCK_ORBIT, a == A_TICK);
      s_rebuild = true;
      break;
    default: break;
  }
}

static void onAct(lv_event_t* e) { act((int)(uintptr_t)lv_event_get_user_data(e)); }

void clockTap(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size() || s_ed.root) return;
  // A hold opens the page editor, as on an empty part of the page.
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
    if (s_tiles[idx].page == sh.active) edOpen(sh.active);
    return;
  }
  hapticTap();
  audioClick();
  openClockCard((int)idx);
}

// --- Every pass ------------------------------------------------------------------------
static void pillRoom(TileRt& r, int w) {
  r.pillW = (int16_t)w;
  r.fitW = r.fitH = 0;
  fitTile(r);
}

// Bottom right, else top right, where it covers neither the time nor the date;
// else the time makes room for it.
static void pillPlace(TileRt& r) {
  const bool large = r.fitH >= 120;
  const int m = large ? 9 : 6;
  lv_obj_update_layout(r.pill);
  const int pw = lv_obj_get_width(r.pill), ph = lv_obj_get_height(r.pill);
  lv_area_t o, time, date;
  lv_obj_get_coords(r.obj, &o);
  bool top = false;
  if (clockInk(r, time, date)) {
    const int x = o.x2 - m - pw;
    const lv_area_t low = {x, o.y2 - m - ph, o.x2 - m, o.y2 - m}, high = {x, o.y1 + m, o.x2 - m, o.y1 + m + ph};
    const auto free = [&](const lv_area_t& a) { return !areaHits(a, time) && (date.x2 < date.x1 || !areaHits(a, date)); };
    if (!free(low)) {
      top = free(high);
      if (!top) pillRoom(r, pw + 4);
    }
  }
  // Aligned to the tile's content area: take its padding back.
  const int px = lv_obj_get_style_pad_right(r.obj, LV_PART_MAIN) - m, py = lv_obj_get_style_pad_top(r.obj, LV_PART_MAIN) - m;
  lv_obj_align(r.pill, top ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_BOTTOM_RIGHT, px, top ? -py : py);
}

static void pillSync() {
  const bool on = s_tm == TM_RUN || s_tm == TM_PAUSE;
  char b[16] = "";
  if (on) timerText(timerLeft(), b, sizeof(b));
  for (TileRt& r : s_tiles) {
    if (r.def->type != TILE_CLOCK || r.off || (!on && !r.pill)) continue;
    if (!on) {
      lv_obj_delete(r.pill);
      r.pill = nullptr;
      if (r.pillW) pillRoom(r, 0);
      markPageStale(r.page);
      continue;
    }
    if (!r.fitH) continue;  // not laid out yet
    const bool fresh = !r.pill;
    if (fresh) {
      lv_obj_t* p = r.pill = lv_label_create(r.obj);
      lv_obj_add_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
      lv_label_set_text(p, "");
      lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(p, lv_color_hex(0x221602), 0);
    }
    const bool large = r.fitH >= 120;
    const uint32_t bg = s_tm == TM_PAUSE ? 0x8D95A3 : C_TIMER;
    if (lv_color_to_u32(lv_obj_get_style_bg_color(r.pill, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(bg)) || fresh) {
      lv_obj_set_style_bg_color(r.pill, lv_color_hex(bg), 0);
      markPageStale(r.page);
    }
    if (strcmp(lv_label_get_text(r.pill), b) == 0) continue;
    if (fresh) {
      lv_obj_set_style_text_font(r.pill, large ? &deck_font_20 : &deck_font_14, 0);
      lv_obj_set_style_pad_hor(r.pill, large ? 10 : 7, 0);
      lv_obj_set_style_pad_ver(r.pill, large ? 3 : 1, 0);
    }
    const bool wider = strlen(b) > strlen(lv_label_get_text(r.pill));
    lv_label_set_text(r.pill, b);
    if (wider) {
      if (r.pillW) pillRoom(r, 0);
      pillPlace(r);
    }
    markPageStale(r.page);
  }
}

static void ringTick(uint32_t now) {
  if (!s_ring) return;
  if (now - s_ringMs >= RING_MS) {
    s_ring = false;
    audioStopTimer();
    s_rebuild = s_card == CK_DONE;  // the bell rests
    return;
  }
  if (audioBusy()) {
    s_ringGapMs = 0;
  } else if (!s_ringGapMs) {
    s_ringGapMs = now | 1;
  } else if (now - s_ringGapMs >= RING_GAP_MS) {
    s_ringGapMs = 0;
    if (!audioTimer()) s_ring = false;
  }
}

// From deckTick on every pass, also under the photo frame and system screens.
void clockTimerTick(bool systemScreen) {
  const uint32_t now = millis();
  if (s_tm == TM_RUN && esp_timer_get_time() >= s_tmEndUs) {
    s_tm = TM_DONE;
    s_doneMs = now;
    s_ringMs = now;
    s_ringGapMs = 0;
    s_ring = audioTimer();
    Serial.println("Deck: timer finished");
    prnWake();
  }
  ringTick(now);
  if (compOwnsScreen()) return;
  if (s_tm == TM_DONE && s_card != CK_DONE && !systemScreen && !frameActive() && compMode() == COMP_NONE &&
      !touchIsDown()) {
    hidePanelNow();
    openCard(CK_DONE, -1);
    s_doneMs = now;
  }
  if (frameActive()) return;
  if (s_card == CK_NONE && s_save.on && compMode() == COMP_NONE) clockStyleFlush();
  // The timer ran out or was changed from elsewhere under the open card.
  static TimerState shown = TM_IDLE;
  if (s_tm != shown) {
    shown = s_tm;
    if (s_card == CK_CLOCK && !s_tabStyle) s_rebuild = true;
  }
  if (s_card != CK_NONE) {
    if (s_rebuild) fillCard();
    else cardLive();
  }
  pillSync();
}

}  // namespace deckui

using namespace deckui;

// --- Bench hook (/api/deck/timer) --------------------------------------------------------
bool deckTimerCommand(const String& op, int value) {
  if (op == "start") {
    if (value < 1 || value > (int)TIMER_MAX_S) return false;
    timerIdle();
    timerStart(value);
  } else if (op == "add") timerAdd(constrain(value, 1, 3600));
  else if (op == "pause" || op == "resume") timerPause(op == "pause");
  else if (op == "cancel") {
    if (s_tm == TM_DONE) return false;
    timerIdle();
  } else if (op == "act") {  // a button of the open card
    if (s_card == CK_NONE) return false;
    act(value);
  } else return false;
  return true;
}

void deckTimerJson(JsonObject o) {
  static const char* const kState[] = {"idle", "running", "paused", "done"};
  o["state"] = kState[s_tm];
  o["left_s"] = timerLeft();
  o["total_s"] = s_tmTotal;
  o["ringing"] = s_ring;
  o["card"] = s_card == CK_NONE ? "none" : s_card == CK_DONE ? "done" : s_tabStyle ? "style" : "timer";
  o["unsaved_style"] = s_save.on;
}

void deckClockStyleFlush() { clockStyleFlush(); }
