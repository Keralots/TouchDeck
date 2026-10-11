// Guition JC3248W535 deck UI. See deck_ui.h for the threading contract.
#if defined(BOARD_IS_JC3248W535)

#include "deck_ui.h"
#include "deck_touch.h"
#include "display_ui.h"
#include "settings.h"
#include <lvgl.h>
#include <time.h>

using namespace deck;

namespace {

constexpr int MAX_PAGES = 6;
constexpr int FACE_PAGE = 0;
constexpr int HEADER_H = 30;
// While a deck page is showing, the face is still redrawn about this often so
// a swipe back to it does not reveal stale numbers.
constexpr uint32_t HIDDEN_FACE_MS = 1000;
// Page images are refreshed one per pass, at most this often (each costs
// ~25 ms of loop time at 480x320).
constexpr uint32_t SNAP_SPACING_MS = 40;
constexpr uint32_t STATS_MS = 10000;

// Mockup A ("default theme") colours.
constexpr uint32_t C_BG = 0x0D0F13;
constexpr uint32_t C_TILE = 0x1A1D23;
constexpr uint32_t C_TILE_PRESSED = 0x2A2F38;
constexpr uint32_t C_CHIP = 0x272B33;
constexpr uint32_t C_TEXT = 0xE6E8EC;
constexpr uint32_t C_TEXT_ON = 0x221602;
constexpr uint32_t C_TITLE = 0xF1F2F4;
constexpr uint32_t C_CLOCK = 0xC9CDD6;

struct Page {
  lv_obj_t* root;
  lv_obj_t* clock;
  lv_draw_buf_t* snap;
  bool isFace;
  bool stale;
};

lgfx::LGFX_Sprite* s_face = nullptr;
lv_display_t* s_disp = nullptr;
lv_indev_t* s_indev = nullptr;
lv_obj_t* s_canvas = nullptr;
Page s_pages[MAX_PAGES];
int s_count = 0;
uint32_t s_gen = 1;
bool s_faceDirty = true;
uint32_t s_lastHiddenFace = 0;
uint32_t s_lastSnapMs = 0;
uint32_t s_lastClockCheck = 0;
int s_lastMinute = -1;
uint32_t s_lastStats = 0;
uint32_t s_waitTimeouts = 0;
uint32_t s_snapUsMax = 0;

// Shared with the touch task, guarded by s_mux. `active` is only written by
// the loop task.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Shared {
  int active = FACE_PAGE;
  int count = 0;
  bool swipeEnabled = false;
  bool owned = false;      // a swipe owns the screen until its landing is handled
  bool faceBusy = false;   // the renderer is drawing into the face sprite
  bool valid[MAX_PAGES] = {};
  bool busy[MAX_PAGES] = {};
  Src src[MAX_PAGES] = {};
};
Shared sh;

// --- LVGL glue ---------------------------------------------------------------------
uint32_t tickCb() { return millis(); }

void flushCb(lv_display_t* d, const lv_area_t*, uint8_t* px) {
  if (!lv_display_flush_is_last(d)) {
    lv_display_flush_ready(d);
    return;
  }
  const uint8_t* fb1 = (const uint8_t*)panelLvBuffer(1);
  const int idx = (px >= fb1 && px < fb1 + FRAME_BYTES) ? 1 : 0;
  panelPostLv(idx, s_gen);
}

// Blocking wait instead of LVGL's default busy spin on `flushing`. The flush
// task gives exactly one token per posted frame, and never calls
// lv_display_flush_ready() itself, so tokens and waits stay paired.
void flushWaitCb(lv_display_t*) {
  if (!panelWaitLvReady(250)) s_waitTimeouts++;
}

void indevRead(lv_indev_t*, lv_indev_data_t* data) {
  const TouchPoint p = touchForLvgl();
  data->point.x = p.x;
  data->point.y = p.y;
  data->state = p.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// --- Pages -----------------------------------------------------------------------------
struct TileDef {
  const char* name;
  const char* state;
  const char* sym;
  bool on;
  uint32_t accent;
};

// Phase 1 placeholder content (local toggles only). Phase 2 replaces it with
// pages built from deck.json, Phase 3 binds tiles to Home Assistant.
const TileDef kDeskPage[12] = {
  {"Desk lamp", "80%", LV_SYMBOL_IMAGE, true, 0xF6C56B},
  {"Ceiling", "Off", LV_SYMBOL_POWER, false, 0xF6C56B},
  {"Bias light", "Warm", LV_SYMBOL_EYE_OPEN, true, 0xF6C56B},
  {"Fan", "Off", LV_SYMBOL_REFRESH, false, 0x5CC9A7},
  {"Evening", "Scene", LV_SYMBOL_IMAGE, false, 0xB3A6FF},
  {"Focus", "Scene", LV_SYMBOL_EYE_OPEN, false, 0xB3A6FF},
  {"Printer", "On", LV_SYMBOL_CHARGE, true, 0x5CC9A7},
  {"Speakers", "Off", LV_SYMBOL_VOLUME_MAX, false, 0x5CC9A7},
  {"Play", "Spotify", LV_SYMBOL_PLAY, false, 0x7DB5FF},
  {"Front door", "Locked", LV_SYMBOL_HOME, true, 0x9DB8FF},
  {"All off", "Script", LV_SYMBOL_POWER, false, 0xFF8A7A},
  {"PC", "Page", LV_SYMBOL_RIGHT, false, 0x7DB5FF},
};
const TileDef kPcPage[12] = {
  {"Mute mic", "Hotkey", LV_SYMBOL_MUTE, false, 0x7DB5FF},
  {"Screenshot", "Hotkey", LV_SYMBOL_IMAGE, false, 0x7DB5FF},
  {"Previous", "Media", LV_SYMBOL_PREV, false, 0x7DB5FF},
  {"Play", "Media", LV_SYMBOL_PLAY, false, 0x7DB5FF},
  {"Next", "Media", LV_SYMBOL_NEXT, false, 0x7DB5FF},
  {"Lock PC", "Win+L", LV_SYMBOL_HOME, false, 0x9DB8FF},
  {"Terminal", "Companion", LV_SYMBOL_KEYBOARD, false, 0x5CC9A7},
  {"Browser", "Companion", LV_SYMBOL_LIST, false, 0x5CC9A7},
  {"Volume -", "Media", LV_SYMBOL_VOLUME_MID, false, 0x7DB5FF},
  {"Volume +", "Media", LV_SYMBOL_VOLUME_MAX, false, 0x7DB5FF},
  {"Sleep PC", "Companion", LV_SYMBOL_POWER, false, 0xFF8A7A},
  {"Desk", "Page", LV_SYMBOL_LEFT, false, 0x7DB5FF},
};

void tileChanged(lv_event_t*) {
  // The live framebuffer is the source for the page being shown; its cached
  // image is only needed once it becomes a neighbour, so just mark it.
  s_pages[sh.active].stale = true;
}

void makeTile(lv_obj_t* parent, int col, int row, const TileDef& d) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(t, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_set_style_radius(t, 16, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_all(t, 9, 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(d.accent), LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT_ON), LV_STATE_CHECKED);
  if (d.on) lv_obj_add_state(t, LV_STATE_CHECKED);
  lv_obj_add_event_cb(t, tileChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* chip = lv_obj_create(t);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(C_CHIP), 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_label_set_text(ic, d.sym);
  lv_obj_center(ic);

  lv_obj_t* name = lv_label_create(t);
  lv_label_set_text(name, d.name);
  lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, -14);
  lv_obj_t* st = lv_label_create(t);
  lv_label_set_text(st, d.state);
  lv_obj_set_style_text_opa(st, LV_OPA_70, 0);
  lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 0, 2);
}

lv_obj_t* makeRoot(int w, int h) {
  lv_obj_t* r = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, w, h);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

void addDeckPage(const char* title, const TileDef* defs) {
  const int w = logicalW(), h = logicalH();
  const bool landscape = w > h;
  Page& p = s_pages[s_count];
  p = {};
  p.root = makeRoot(w, h);

  lv_obj_t* t = lv_label_create(p.root);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TITLE), 0);
  lv_obj_set_pos(t, 12, 5);
  p.clock = lv_label_create(p.root);
  lv_label_set_text(p.clock, "");
  lv_obj_set_style_text_color(p.clock, lv_color_hex(C_CLOCK), 0);
  lv_obj_align(p.clock, LV_ALIGN_TOP_RIGHT, -12, 8);

  static const int32_t colsL[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static const int32_t rowsL[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static const int32_t colsP[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static const int32_t rowsP[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  lv_obj_t* grid = lv_obj_create(p.root);
  lv_obj_remove_style_all(grid);
  lv_obj_set_pos(grid, 0, HEADER_H);
  lv_obj_set_size(grid, w, h - HEADER_H);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(grid, LV_LAYOUT_GRID);
  lv_obj_set_grid_dsc_array(grid, landscape ? colsL : colsP, landscape ? rowsL : rowsP);
  lv_obj_set_style_pad_all(grid, 8, 0);
  lv_obj_set_style_pad_top(grid, 2, 0);
  lv_obj_set_style_pad_row(grid, 7, 0);
  lv_obj_set_style_pad_column(grid, 7, 0);
  const int cols = landscape ? 4 : 3;
  for (int i = 0; i < 12; i++) makeTile(grid, i % cols, i / cols, defs[i]);

  p.snap = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  p.stale = true;
  s_count++;
}

void buildPages() {
  const int w = logicalW(), h = logicalH();
  s_count = 0;

  Page& f = s_pages[s_count++];
  f = {};
  f.isFace = true;
  f.root = makeRoot(w, h);
  s_canvas = lv_canvas_create(f.root);
  lv_canvas_set_buffer(s_canvas, s_face->getBuffer(), w, h, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_obj_set_pos(s_canvas, 0, 0);

  addDeckPage("Desk", kDeskPage);
  addDeckPage("PC", kPcPage);

  portENTER_CRITICAL(&s_mux);
  sh.count = s_count;
  sh.owned = false;
  for (int i = 0; i < MAX_PAGES; i++) { sh.valid[i] = false; sh.busy[i] = false; }
  // The face page is shown straight from the renderer's sprite (already in
  // panel byte order) instead of a snapshot.
  sh.valid[FACE_PAGE] = true;
  sh.src[FACE_PAGE] = {(const uint16_t*)s_face->getBuffer(), (uint16_t)w, false};
  const int active = sh.active < s_count ? sh.active : FACE_PAGE;
  sh.active = active;
  portEXIT_CRITICAL(&s_mux);

  // Inactive pages wait just off the right edge: they keep their layout and
  // can be snapshotted, but their invalidations are clipped away.
  for (int i = 0; i < s_count; i++) lv_obj_set_pos(s_pages[i].root, i == active ? 0 : w, 0);
  s_lastMinute = -1;
  s_faceDirty = true;
}

void destroyPages() {
  for (int i = 0; i < s_count; i++) {
    if (s_pages[i].root) lv_obj_delete(s_pages[i].root);
    if (s_pages[i].snap) lv_draw_buf_destroy(s_pages[i].snap);
    s_pages[i] = {};
  }
  s_count = 0;
  s_canvas = nullptr;
  portENTER_CRITICAL(&s_mux);
  sh.count = 0;
  for (int i = 0; i < MAX_PAGES; i++) sh.valid[i] = false;
  portEXIT_CRITICAL(&s_mux);
}

void switchTo(int page) {
  const int w = logicalW();
  const int old = sh.active;
  if (page == old || page < 0 || page >= s_count) return;
  lv_obj_set_x(s_pages[old].root, w);
  lv_obj_set_x(s_pages[page].root, 0);
  portENTER_CRITICAL(&s_mux);
  sh.active = page;
  portEXIT_CRITICAL(&s_mux);
  s_gen++;
  // New neighbours; cheap to re-take, and the page we left changed state.
  for (int i = 0; i < s_count; i++) s_pages[i].stale = true;
  if (page == FACE_PAGE) s_faceDirty = true;
}

void releaseSwipe() {
  portENTER_CRITICAL(&s_mux);
  sh.owned = false;
  portEXIT_CRITICAL(&s_mux);
}

void refreshOneSnapshot() {
  if (touchIsDown() || compMode() != COMP_NONE) return;
  const uint32_t now = millis();
  if (now - s_lastSnapMs < SNAP_SPACING_MS) return;
  const int a = sh.active;
  int pick = -1;
  const int order[2] = {a + 1, a - 1};
  for (int k = 0; k < 2 && pick < 0; k++) {
    const int p = order[k];
    if (p >= 0 && p < s_count && !s_pages[p].isFace && s_pages[p].stale) pick = p;
  }
  for (int p = 0; p < s_count && pick < 0; p++)
    if (!s_pages[p].isFace && s_pages[p].stale) pick = p;
  if (pick < 0) return;
  Page& pg = s_pages[pick];
  if (!pg.snap) return;

  portENTER_CRITICAL(&s_mux);
  const bool owned = sh.owned;
  if (!owned) sh.busy[pick] = true;
  portEXIT_CRITICAL(&s_mux);
  if (owned) return;

  const uint32_t t0 = micros();
  lv_snapshot_reshape_draw_buf(pg.root, pg.snap);
  const bool ok = lv_snapshot_take_to_draw_buf(pg.root, LV_COLOR_FORMAT_RGB565, pg.snap) == LV_RESULT_OK;
  const uint32_t dt = micros() - t0;
  if (dt > s_snapUsMax) s_snapUsMax = dt;

  portENTER_CRITICAL(&s_mux);
  sh.busy[pick] = false;
  sh.valid[pick] = ok;
  if (ok) sh.src[pick] = {(const uint16_t*)pg.snap->data, (uint16_t)(pg.snap->header.stride / 2), true};
  portEXIT_CRITICAL(&s_mux);
  pg.stale = !ok;
  s_lastSnapMs = millis();
}

void updateClocks() {
  const uint32_t now = millis();
  if (now - s_lastClockCheck < 1000) return;
  s_lastClockCheck = now;
  const time_t epoch = time(nullptr);
  if (epoch < 1609459200) return;  // not synced yet
  struct tm lt;
  localtime_r(&epoch, &lt);
  const int minute = lt.tm_hour * 60 + lt.tm_min;
  if (minute == s_lastMinute) return;
  s_lastMinute = minute;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
  for (int i = 0; i < s_count; i++) {
    if (!s_pages[i].clock) continue;
    lv_label_set_text(s_pages[i].clock, buf);
    s_pages[i].stale = true;
  }
}

void createFaceSprite() {
  s_face->deleteSprite();
  s_face->setPsram(true);
  s_face->setColorDepth(16);
  s_face->setRotation(0);
  if (!s_face->createSprite(logicalW(), logicalH()))
    Serial.println("Deck: face sprite allocation FAILED");
  s_face->setTextDatum(MC_DATUM);
}

void logStats() {
  const uint32_t now = millis();
  if (now - s_lastStats < STATS_MS) return;
  s_lastStats = now;
  const PanelStats p = panelTakeStats();
  if (!p.frames && !s_waitTimeouts) return;
  Serial.printf("Deck: frames=%u comp=%u flush=%u/%uus build=%uus te_late=%u te_to=%u "
                "lv_drop=%u gap_max=%uus wait_to=%u snap_max=%uus int_free=%u\n",
                (unsigned)p.frames, (unsigned)p.compFrames, (unsigned)p.flushUsAvg,
                (unsigned)p.flushUsMax, (unsigned)p.buildUsAvg, (unsigned)p.teLate,
                (unsigned)p.teTimeouts, (unsigned)p.lvDropped, (unsigned)p.gapMaxUs,
                (unsigned)s_waitTimeouts, (unsigned)s_snapUsMax,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  s_waitTimeouts = 0;
  s_snapUsMax = 0;
}

}  // namespace

// --- Public API ----------------------------------------------------------------------
bool deckBegin(lgfx::LGFX_Sprite* face, uint8_t rotation) {
  s_face = face;
  if (!panelBegin(rotation)) return false;
  createFaceSprite();

  lv_init();
  lv_tick_set_cb(tickCb);
  s_disp = lv_display_create(logicalW(), logicalH());
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(s_disp, panelLvBuffer(0), panelLvBuffer(1), FRAME_BYTES,
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(s_disp, flushCb);
  lv_display_set_flush_wait_cb(s_disp, flushWaitCb);

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, indevRead);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  buildPages();
  touchBegin();
  Serial.printf("Deck: LVGL %d.%d.%d up, %d pages, %dx%d\n", LVGL_VERSION_MAJOR,
                LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, s_count, logicalW(), logicalH());
  return true;
}

void deckTick() {
  if (!s_disp) return;

  int8_t landing;
  if (compTakeLanding(&landing)) {
    if (landing) {
      switchTo(sh.active + landing);
      compSetHoldGen(s_gen);
    }
    releaseSwipe();
  }
  // A swipe took the touch: make sure LVGL never turns it into a click.
  if (touchTakeSwipeStarted()) lv_indev_reset(s_indev, nullptr);

  if (touchTakeActivity() && displaySleepingForTouch()) {
    wakeDisplayFromTouch();
    touchSuppressUntilRelease();
  }

  // Status screens (splash, WiFi setup, OTA) live on the face page and must be
  // visible whatever page was showing.
  const ScreenState sc = getScreenState();
  const bool systemScreen = !(sc == SCREEN_MONITOR || sc == SCREEN_CLOCK || sc == SCREEN_OFF);
  if (systemScreen && sh.active != FACE_PAGE) {
    compCancel();
    releaseSwipe();
    switchTo(FACE_PAGE);
  }
  portENTER_CRITICAL(&s_mux);
  sh.swipeEnabled = !systemScreen && s_count > 1;
  sh.faceBusy = false;  // the renderer only draws before this call
  portEXIT_CRITICAL(&s_mux);

  if (compOwnsScreen()) return;  // LVGL stays frozen during a drag or slide

  if (s_faceDirty) {
    s_faceDirty = false;
    if (sh.active == FACE_PAGE && s_canvas) lv_obj_invalidate(s_canvas);
  }
  updateClocks();
  lv_timer_handler();
  refreshOneSnapshot();
  logStats();
}

void deckFaceDirty() { s_faceDirty = true; }

bool deckFaceMayRender() {
  if (!s_disp) return true;  // before deckBegin (or if it failed)
  if (compOwnsScreen()) return false;
  if (sh.active != FACE_PAGE) {
    const uint32_t now = millis();
    if (now - s_lastHiddenFace < HIDDEN_FACE_MS) return false;
    s_lastHiddenFace = now;
  }
  portENTER_CRITICAL(&s_mux);
  const bool ok = !sh.owned;
  if (ok) sh.faceBusy = true;
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

bool deckFacePageActive() { return !s_disp || sh.active == FACE_PAGE; }

void deckApplyRotation(uint8_t rotation) {
  if (!s_disp) return;
  rotation &= 3;
  if (rotation == panelRotation()) return;
  compCancel();
  releaseSwipe();
  destroyPages();
  panelSetRotation(rotation);
  createFaceSprite();
  lv_display_set_resolution(s_disp, logicalW(), logicalH());
  lv_display_set_buffers(s_disp, panelLvBuffer(0), panelLvBuffer(1), FRAME_BYTES,
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
}

bool deckTestSwipe(int dir) {
  if (!s_disp || (dir != 1 && dir != -1)) return false;
  Src a, b;
  int so;
  if (!uiTryBeginSwipe(dir, a, b, so)) return false;
  if (!compBeginDrag(a, b, so)) {
    releaseSwipe();
    return false;
  }
  compRelease(dir > 0 ? logicalW() : 0, 280, (int8_t)dir);
  return true;
}

namespace deck {

bool uiTryBeginSwipe(int dir, Src& a, Src& b, int& startOff) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  const int target = sh.active + dir;
  if (sh.swipeEnabled && !sh.owned && target >= 0 && target < sh.count &&
      sh.valid[target] && !sh.busy[target] &&
      !(target == FACE_PAGE && sh.faceBusy)) {
    sh.owned = true;
    const Src cur = {nullptr, 0, true};  // the live LVGL frame
    if (dir > 0) {
      a = cur;
      b = sh.src[target];
      startOff = 0;
    } else {
      a = sh.src[target];
      b = cur;
      startOff = logicalW();
    }
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void uiSwipeAborted() { releaseSwipe(); }

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
