// Guition JC3248W535 deck UI. See deck_ui.h for the threading contract.
#if defined(BOARD_IS_JC3248W535)

#include "deck_ui.h"
#include "deck_config.h"
#include "deck_ha.h"
#include "deck_touch.h"
#include "display_ui.h"
#include "settings.h"
#include "led.h"
#include "pc_metrics.h"
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>
#include <vector>

using namespace deck;

namespace {

constexpr int MAX_PAGES = MAX_DECK_PAGES + 1;  // + the face page if inserted
constexpr int HEADER_H = 30;
// While a deck page is showing, the face is still redrawn about this often so
// a swipe back to it does not reveal stale numbers.
constexpr uint32_t HIDDEN_FACE_MS = 1000;
// Page images are refreshed one per pass, at most this often (each costs
// ~40 ms of loop time at 480x320).
constexpr uint32_t SNAP_SPACING_MS = 40;
constexpr uint32_t STATS_MS = 10000;
constexpr uint32_t SLIDE_MS = 280;

// Mockup A ("Midnight", the default theme) colours.
constexpr uint32_t C_BG = 0x0D0F13;
constexpr uint32_t C_TILE = 0x1A1D23;
constexpr uint32_t C_TILE_PRESSED = 0x2A2F38;
constexpr uint32_t C_CHIP = 0x272B33;
constexpr uint32_t C_TEXT = 0xE6E8EC;
constexpr uint32_t C_TEXT_ON = 0x221602;
constexpr uint32_t C_TITLE = 0xF1F2F4;
constexpr uint32_t C_CLOCK = 0xC9CDD6;
constexpr uint32_t C_OK = 0x5CC9A7;
constexpr uint32_t C_DIM = 0x596070;

struct Page {
  lv_obj_t* root;
  lv_obj_t* clock;
  lv_obj_t* wifi;
  lv_draw_buf_t* snap;
  const PageDef* def;
  bool isFace;
  bool stale;
};

// One tile on screen. LVGL user data holds the index into s_tiles.
struct TileRt {
  const TileDef* def;
  int page;
  lv_obj_t* obj;
  lv_obj_t* name;
  lv_obj_t* sub;
  bool on;
  uint32_t haVersion;     // last HA entity version applied
  bool pending;           // optimistic toggle waiting for HA
  bool pendingOn;
  uint32_t pendingUntil;
};

lgfx::LGFX_Sprite* s_face = nullptr;
lv_display_t* s_disp = nullptr;
lv_indev_t* s_indev = nullptr;
lv_obj_t* s_canvas = nullptr;
DeckConfig s_cfg;
Page s_pages[MAX_PAGES];
std::vector<TileRt> s_tiles;
int s_count = 0;
int s_facePage = 0;
uint32_t s_gen = 1;
bool s_faceDirty = true;
uint32_t s_lastHiddenFace = 0;
uint32_t s_lastSnapMs = 0;
uint32_t s_lastClockCheck = 0;
int s_lastMinute = -1;
int s_lastWifi = -1;
uint32_t s_lastStats = 0;
uint32_t s_waitTimeouts = 0;
uint32_t s_snapUsMax = 0;
lv_style_transition_dsc_t s_pressTrans;

// Shared with the touch task, guarded by s_mux. `active` is only written by
// the loop task.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Shared {
  int active = 0;
  int count = 0;
  int facePage = 0;
  bool swipeEnabled = false;
  bool owned = false;      // a swipe owns the screen until its landing is handled
  bool faceBusy = false;   // the renderer is drawing into the face sprite
  bool valid[MAX_PAGES] = {};
  bool busy[MAX_PAGES] = {};
  Src src[MAX_PAGES] = {};
  // What the current compositor gesture is, for its landing.
  enum Gesture : uint8_t { GEST_PAGE, GEST_PANEL_OPEN, GEST_PANEL_CLOSE } gesture = GEST_PAGE;
  bool panelOpen = false;
  bool modalOpen = false;  // detail card up: no swipes or panel
  bool panelValid = false;
  bool panelBusy = false;
  Src panelSrc = {};
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

// --- Tile look ------------------------------------------------------------------------
String entityDomain(const String& entity) {
  const int dot = entity.indexOf('.');
  return dot > 0 ? entity.substring(0, dot) : String();
}

uint32_t domainColor(const String& domain) {
  if (domain == "light") return 0xF6C56B;
  if (domain == "switch" || domain == "fan" || domain == "input_boolean") return 0x5CC9A7;
  if (domain == "scene" || domain == "script") return 0xB3A6FF;
  if (domain == "lock" || domain == "cover") return 0x9DB8FF;
  return 0x7DB5FF;
}

// Material Design Icons names (what HA uses) mapped onto the LVGL symbol font
// until the MDI subset font lands with the Home Assistant work.
const char* iconSymbol(const TileDef& t) {
  struct Map { const char* name; const char* sym; };
  static const Map kMap[] = {
    {"lightbulb", LV_SYMBOL_EYE_OPEN}, {"ceiling-light", LV_SYMBOL_EYE_OPEN},
    {"television-ambient-light", LV_SYMBOL_IMAGE}, {"fan", LV_SYMBOL_REFRESH},
    {"weather-night", LV_SYMBOL_IMAGE}, {"eye", LV_SYMBOL_EYE_OPEN},
    {"printer-3d", LV_SYMBOL_CHARGE}, {"speaker", LV_SYMBOL_AUDIO},
    {"lock", LV_SYMBOL_HOME}, {"power", LV_SYMBOL_POWER}, {"power-sleep", LV_SYMBOL_POWER},
    {"chevron-right", LV_SYMBOL_RIGHT}, {"chevron-left", LV_SYMBOL_LEFT},
    {"microphone-off", LV_SYMBOL_MUTE}, {"image", LV_SYMBOL_IMAGE},
    {"skip-previous", LV_SYMBOL_PREV}, {"skip-next", LV_SYMBOL_NEXT},
    {"play", LV_SYMBOL_PLAY}, {"pause", LV_SYMBOL_PAUSE},
    {"console", LV_SYMBOL_KEYBOARD}, {"volume-high", LV_SYMBOL_VOLUME_MAX},
    {"volume-off", LV_SYMBOL_MUTE}, {"wifi", LV_SYMBOL_WIFI}, {"cog", LV_SYMBOL_SETTINGS},
    {"bell", LV_SYMBOL_BELL}, {"home", LV_SYMBOL_HOME},
  };
  String name = t.icon;
  if (name.startsWith("mdi:")) name = name.substring(4);
  for (const Map& m : kMap)
    if (name == m.name) return m.sym;
  const String d = entityDomain(t.entity);
  if (d == "light") return LV_SYMBOL_EYE_OPEN;
  if (d == "fan") return LV_SYMBOL_REFRESH;
  if (d == "lock") return LV_SYMBOL_HOME;
  if (d == "media_player") return LV_SYMBOL_PLAY;
  if (d == "sensor" || d == "binary_sensor" || d == "weather") return LV_SYMBOL_LIST;
  return LV_SYMBOL_POWER;
}

String tileLabel(const TileDef& t) {
  if (t.label.length()) return t.label;
  const int dot = t.entity.indexOf('.');
  if (dot > 0) return t.entity.substring(dot + 1);
  return t.id.length() ? t.id : String("Tile");
}

String tileSub(const TileRt& r) {
  const TileDef& t = *r.def;
  if (t.sub.length()) return t.sub;
  if (t.type == TILE_TOGGLE) return r.on ? "On" : "Off";
  const String d = entityDomain(t.entity);
  if (d == "scene") return "Scene";
  if (d == "script") return "Script";
  if (d == "button" || d == "input_button") return "Press";
  return "";
}

void markPageStale(int page) {
  if (page >= 0 && page < s_count) s_pages[page].stale = true;
}

// --- Navigation -------------------------------------------------------------------------
void switchTo(int page);
void releaseSwipe();

// Slide one page toward dir through the compositor, as a fling would.
bool slideBy(int dir) {
  Src a, b;
  int so, pageA;
  if (!uiTryBeginSwipe(dir, a, b, so, pageA)) return false;
  if (!compBeginDrag(a, b, so, pageA)) {
    releaseSwipe();
    return false;
  }
  compRelease(dir > 0 ? logicalW() : 0, SLIDE_MS, (int8_t)dir);
  return true;
}

void goToPage(const String& id) {
  for (int i = 0; i < s_count; i++) {
    if (!s_pages[i].def || s_pages[i].def->id != id) continue;
    const int d = i - sh.active;
    if (d == 0) return;
    if ((d == 1 || d == -1) && slideBy(d)) return;
    switchTo(i);  // not adjacent (or images not ready): jump
    return;
  }
}

// --- Tile events ------------------------------------------------------------------------
void openModal(int tileIdx);
void refreshModal();
void deckToastShow(const char* text, uint32_t holdMs);

// --- Home Assistant binding ------------------------------------------------------------
constexpr uint32_t PENDING_MS = 4000;  // optimistic toggle waits this long for HA

bool haStateOn(const String& domain, const String& st) {
  if (domain == "lock") return st == "locked";
  if (domain == "cover") return st == "open" || st == "opening";
  if (domain == "media_player") return st == "playing";
  if (domain == "climate") return st != "off" && st.length();
  return st == "on" || st == "home";
}

String prettyState(const String& domain, const HaEntity& e) {
  if (!e.known) return "Unavailable";
  const String& st = e.state;
  if (st == "unavailable" || st == "unknown") return "Unavailable";
  if (domain == "light" && st == "on" && e.brightness >= 0)
    return String((e.brightness * 100 + 127) / 255) + "%";
  if (domain == "scene") return "Scene";
  if (domain == "script") return st == "on" ? "Running" : "Script";
  if (domain == "button" || domain == "input_button") return "Press";
  if (e.unit.length()) {
    // Numeric reading: at most one decimal, then the unit.
    char* end = nullptr;
    const float v = strtof(st.c_str(), &end);
    if (end && *end == 0 && st.length()) {
      char buf[24];
      if (fabsf(v - roundf(v)) < 0.05f) snprintf(buf, sizeof(buf), "%.0f", v);
      else snprintf(buf, sizeof(buf), "%.1f", v);
      return String(buf) + " " + e.unit;
    }
    return st + " " + e.unit;
  }
  String s = st;
  s.replace('_', ' ');
  if (s.length()) s.setCharAt(0, toupper(s[0]));
  return s;
}

// The service a toggle tile calls for its entity's domain.
void toggleService(const String& domain, bool currentlyOn, String& svcDomain, String& svc) {
  svcDomain = domain;
  if (domain == "lock") svc = currentlyOn ? "unlock" : "lock";
  else if (domain == "media_player") svc = "media_play_pause";
  else if (domain == "scene") svc = "turn_on";
  else if (domain == "button" || domain == "input_button") svc = "press";
  else svc = "toggle";
}

// The service an action tile without an explicit action runs on its entity.
bool defaultActionService(const String& domain, String& svc) {
  if (domain == "scene" || domain == "script") { svc = "turn_on"; return true; }
  if (domain == "button" || domain == "input_button") { svc = "press"; return true; }
  if (domain == "automation") { svc = "trigger"; return true; }
  return false;
}

void setTileOn(TileRt& r, bool on) {
  r.on = on;
  if (on) lv_obj_add_state(r.obj, LV_STATE_CHECKED);
  else lv_obj_remove_state(r.obj, LV_STATE_CHECKED);
}

// Pull the latest HA state into one tile's widgets.
void applyHaToTile(TileRt& r, bool force) {
  const TileDef& d = *r.def;
  if (!d.entity.length()) return;
  HaEntity e;
  if (!haGetEntity(d.entity, e)) return;
  if (!force && e.version == r.haVersion) return;
  r.haVersion = e.version;
  const String domain = entityDomain(d.entity);
  const bool avail = e.known && e.state != "unavailable" && e.state != "unknown";
  const bool on = avail && haStateOn(domain, e.state);
  if (r.pending) {
    if (on != r.pendingOn) return;  // not there yet; the timeout decides
    r.pending = false;
  }
  if (d.type == TILE_TOGGLE) setTileOn(r, on);
  lv_obj_set_style_opa(r.obj, avail ? LV_OPA_COVER : LV_OPA_50, 0);
  if (!d.label.length() && e.name.length()) lv_label_set_text(r.name, e.name.c_str());
  if (!d.sub.length()) lv_label_set_text(r.sub, prettyState(domain, e).c_str());
  markPageStale(r.page);
}

uint32_t s_haSeen = 0;

void haSync() {
  const uint32_t v = haStoreVersion();
  if (v != s_haSeen) {
    s_haSeen = v;
    for (TileRt& r : s_tiles) applyHaToTile(r, false);
    refreshModal();
  }
  String ent, msg;
  while (haTakeFailure(ent, msg)) {
    for (TileRt& r : s_tiles) {
      if (r.def->entity != ent || !r.pending) continue;
      r.pending = false;
      applyHaToTile(r, true);
    }
    deckToastShow(msg.c_str(), 2500);
  }
  const uint32_t now = millis();
  for (TileRt& r : s_tiles) {
    if (!r.pending || (int32_t)(now - r.pendingUntil) < 0) continue;
    r.pending = false;
    applyHaToTile(r, true);
    deckToastShow("No answer from Home Assistant", 2000);
  }
}

void collectEntities() {
  std::vector<String> ids;
  for (const TileRt& r : s_tiles)
    if (r.def->entity.length()) ids.push_back(r.def->entity);
  haSetEntities(ids);
  s_haSeen = 0;  // re-apply everything to the new widgets
  for (TileRt& r : s_tiles) applyHaToTile(r, true);
}

void runAction(TileRt& r, const TileAction& a) {
  const String& entity = r.def->entity;
  const String domain = entityDomain(entity);
  const bool haUp = haLink() == HA_CONNECTED;
  switch (a.kind) {
    case ACT_TOGGLE: {
      if (!entity.length()) {
        // No entity: a local switch, handy for trying layouts.
        setTileOn(r, !r.on);
        lv_label_set_text(r.sub, tileSub(r).c_str());
        markPageStale(r.page);
        break;
      }
      if (!haUp) {
        deckToastShow("Home Assistant is offline", 1800);
        break;
      }
      String sd, svc;
      toggleService(domain, r.on, sd, svc);
      if (!haCallService(sd, svc, entity, "")) {
        deckToastShow("Home Assistant is busy, try again", 1800);
        break;
      }
      if (r.def->type == TILE_TOGGLE && svc != "turn_on" && svc != "press") {
        // Optimistic: flip now, confirm or revert when HA reports the state.
        setTileOn(r, !r.on);
        r.pending = true;
        r.pendingOn = r.on;
        r.pendingUntil = millis() + PENDING_MS;
        markPageStale(r.page);
      }
      break;
    }
    case ACT_PAGE:
      goToPage(a.page);
      break;
    case ACT_MORE_INFO:
      openModal((int)(&r - &s_tiles[0]));
      break;
    case ACT_CALL_SERVICE:
      if (!haUp) deckToastShow("Home Assistant is offline", 1800);
      else if (!haCallService(a.domain, a.service, entity, a.data))
        deckToastShow("Home Assistant is busy, try again", 1800);
      break;
    default: {
      String svc;
      if (entity.length() && defaultActionService(domain, svc)) {
        if (!haUp) deckToastShow("Home Assistant is offline", 1800);
        else if (!haCallService(domain, svc, entity, "")) deckToastShow("Home Assistant is busy, try again", 1800);
      } else {
        deckToastShow("No action set for this tile", 1500);
      }
      break;
    }
  }
}

void tileEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  // SHORT_CLICKED, not CLICKED: CLICKED also fires on the release after a
  // long press, which would run the tap action as well as the hold action.
  if (code == LV_EVENT_SHORT_CLICKED) runAction(r, r.def->tap);
  else if (code == LV_EVENT_LONG_PRESSED) runAction(r, r.def->hold);
}

// --- Quick panel ------------------------------------------------------------------------
// Pulled down from the top edge. Lives on LVGL's top layer so page images
// never include it. The container is QP_LIP rows taller than what shows and
// sits that much above the screen edge when open, which hides its top
// rounded corners; the compositor skips those rows.
constexpr int QP_H = 196;
constexpr int QP_LIP = 20;
constexpr uint32_t C_PANEL = 0x161A20;
constexpr uint32_t C_SCREEN_ACCENT = 0xF6C56B;
constexpr uint32_t C_LIGHT_ACCENT = 0xFFB35C;

struct QuickPanel {
  lv_obj_t* scrim;
  lv_obj_t* box;
  lv_obj_t* wifi;
  lv_obj_t* pc;
  lv_obj_t* ha;
  lv_obj_t* ip;
  lv_obj_t* screenLbl;
  lv_obj_t* screenSlider;
  lv_obj_t* screenVal;
  lv_obj_t* lightSlider;
  lv_obj_t* lightVal;
  lv_obj_t* nightBtn;
  lv_draw_buf_t* snap;
  bool open;
  bool stale;
  uint32_t lastStatus;
};
QuickPanel s_qp = {};

void closePanelAnimated();

void qpMarkStale() { s_qp.stale = true; }

uint8_t& screenLevelRef() {
  return nightBrightnessActive() ? backlightSettings.nightBrightness : brightness;
}

bool ledUsable() { return ledSettings.enabled && ledPinValid(); }

uint8_t& lightLevelRef() {
  return (ledSettings.nightEnabled && nightBrightnessActive()) ? ledSettings.nightBrightness
                                                               : ledSettings.brightness;
}

void qpSyncValues() {
  if (!s_qp.box) return;
  const bool night = nightBrightnessActive();
  lv_label_set_text(s_qp.screenLbl, night ? "Screen (night)" : "Screen");
  lv_slider_set_value(s_qp.screenSlider, screenLevelRef(), LV_ANIM_OFF);
  lv_label_set_text_fmt(s_qp.screenVal, "%d%%", (screenLevelRef() * 100 + 127) / 255);
  if (ledUsable()) {
    lv_obj_remove_state(s_qp.lightSlider, LV_STATE_DISABLED);
    lv_slider_set_value(s_qp.lightSlider, lightLevelRef(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_qp.lightVal, "%d%%", (lightLevelRef() * 100 + 127) / 255);
  } else {
    lv_obj_add_state(s_qp.lightSlider, LV_STATE_DISABLED);
    lv_slider_set_value(s_qp.lightSlider, 0, LV_ANIM_OFF);
    lv_label_set_text(s_qp.lightVal, "off");
  }
  if (manualNightActive()) lv_obj_add_state(s_qp.nightBtn, LV_STATE_CHECKED);
  else lv_obj_remove_state(s_qp.nightBtn, LV_STATE_CHECKED);
  qpMarkStale();
}

void setChip(lv_obj_t* chip, const String& text, bool good) {
  lv_obj_t* lbl = lv_obj_get_child(chip, 0);
  if (strcmp(lv_label_get_text(lbl), text.c_str()) == 0) return;
  lv_label_set_text(lbl, text.c_str());
  lv_obj_set_style_text_color(lbl, lv_color_hex(good ? C_OK : C_CLOCK), 0);
  qpMarkStale();
}

// Link status, every 2 s. RSSI is rounded so a jittery signal does not keep
// invalidating the panel image.
void qpUpdateStatus(bool force) {
  if (!s_qp.box) return;
  const uint32_t now = millis();
  if (!force && now - s_qp.lastStatus < 2000) return;
  s_qp.lastStatus = now;
  const bool wifi = WiFi.status() == WL_CONNECTED;
  const int rssi = wifi ? (WiFi.RSSI() / 5) * 5 : 0;
  setChip(s_qp.wifi, wifi ? String(LV_SYMBOL_WIFI " ") + rssi + " dBm" : String(LV_SYMBOL_WIFI " offline"), wifi);
  setChip(s_qp.pc, pcData.online ? "PC online" : "PC offline", pcData.online);
  const HaLink hl = haLink();
  setChip(s_qp.ha, String("HA ") + haLinkName(hl), hl == HA_CONNECTED);
  setChip(s_qp.ip, wifi ? WiFi.localIP().toString() : String("-"), wifi);
}

lv_obj_t* qpChip(lv_obj_t* parent) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(c, 10, 0);
  lv_obj_set_style_pad_ver(c, 5, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = lv_label_create(c);
  lv_label_set_text(l, "");
  return c;
}

lv_obj_t* qpSlider(lv_obj_t* parent, int x, int y, int w, uint32_t accent) {
  lv_obj_t* sl = lv_slider_create(parent);
  lv_obj_set_pos(sl, x, y);
  lv_obj_set_size(sl, w, 8);
  lv_slider_set_range(sl, 5, 255);
  lv_obj_set_style_bg_color(sl, lv_color_hex(C_CHIP), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sl, lv_color_hex(C_DIM), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(sl, LV_OPA_40, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_DISABLED);
  // A finger is wider than the 8 px track.
  lv_obj_set_ext_click_area(sl, 16);
  return sl;
}

void onScreenSlider(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    screenLevelRef() = (uint8_t)lv_slider_get_value(s_qp.screenSlider);
    lv_label_set_text_fmt(s_qp.screenVal, "%d%%", (screenLevelRef() * 100 + 127) / 255);
    refreshBacklightControl();
    qpMarkStale();
  } else if (code == LV_EVENT_RELEASED) {
    saveSettings();
  }
}

void onLightSlider(lv_event_t* e) {
  if (!ledUsable()) return;
  const lv_event_code_t code = lv_event_get_code(e);
  const uint8_t v = (uint8_t)lv_slider_get_value(s_qp.lightSlider);
  if (code == LV_EVENT_VALUE_CHANGED) {
    previewLed(true, ledSettings.pin, v);
    lv_label_set_text_fmt(s_qp.lightVal, "%d%%", (v * 100 + 127) / 255);
    qpMarkStale();
  } else if (code == LV_EVENT_RELEASED) {
    lightLevelRef() = v;
    saveSettings();
    clearLedPreview();
  }
}

void hidePanelNow();

void onScreenOff(lv_event_t*) {
  hidePanelNow();
  setManualDisplayOff(true);  // any touch wakes it again
}

void onNight(lv_event_t*) {
  setManualNight(lv_obj_has_state(s_qp.nightBtn, LV_STATE_CHECKED));
  updateBacklightControl();  // apply now so the sliders show the right level
  qpSyncValues();
}

void onScrim(lv_event_t*) { closePanelAnimated(); }

lv_obj_t* qpButton(lv_obj_t* parent, int x, int y, int w, const char* text, bool checkable) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, 40);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xB3A6FF), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT_ON), LV_STATE_CHECKED);
  if (checkable) lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

void buildQuickPanel() {
  const int w = logicalW(), h = logicalH();
  lv_obj_t* top = lv_layer_top();

  s_qp.scrim = lv_obj_create(top);
  lv_obj_remove_style_all(s_qp.scrim);
  lv_obj_set_size(s_qp.scrim, w, h);
  lv_obj_set_style_bg_color(s_qp.scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_qp.scrim, LV_OPA_50, 0);  // = the compositor's 50 % dim
  lv_obj_add_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_qp.scrim, onScrim, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* box = lv_obj_create(top);
  s_qp.box = box;
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, QP_H + QP_LIP);
  lv_obj_set_pos(box, 0, -(QP_H + QP_LIP + 8));  // parked above the screen
  lv_obj_set_style_bg_color(box, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 20, 0);
  lv_obj_set_style_text_color(box, lv_color_hex(C_TEXT), 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  // Taps on the panel background must not fall through to the scrim.
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

  const int y0 = QP_LIP;
  lv_obj_t* chips = lv_obj_create(box);
  lv_obj_remove_style_all(chips);
  lv_obj_set_pos(chips, 12, y0 + 12);
  lv_obj_set_size(chips, w - 24, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(chips, 6, 0);
  lv_obj_set_style_pad_row(chips, 6, 0);
  lv_obj_remove_flag(chips, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  s_qp.wifi = qpChip(chips);
  s_qp.pc = qpChip(chips);
  s_qp.ha = qpChip(chips);
  s_qp.ip = qpChip(chips);

  const int labelW = 108, valW = 46;
  const int slX = 16 + labelW, slW = w - slX - valW - 22;
  s_qp.screenLbl = lv_label_create(box);
  lv_obj_set_pos(s_qp.screenLbl, 16, y0 + 58);
  s_qp.screenSlider = qpSlider(box, slX, y0 + 63, slW, C_SCREEN_ACCENT);
  s_qp.screenVal = lv_label_create(box);
  lv_obj_set_pos(s_qp.screenVal, w - valW - 10, y0 + 58);
  lv_obj_add_event_cb(s_qp.screenSlider, onScreenSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_qp.screenSlider, onScreenSlider, LV_EVENT_RELEASED, nullptr);

  lv_obj_t* lightLbl = lv_label_create(box);
  lv_label_set_text(lightLbl, "Light");
  lv_obj_set_pos(lightLbl, 16, y0 + 98);
  s_qp.lightSlider = qpSlider(box, slX, y0 + 103, slW, C_LIGHT_ACCENT);
  s_qp.lightVal = lv_label_create(box);
  lv_obj_set_pos(s_qp.lightVal, w - valW - 10, y0 + 98);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_RELEASED, nullptr);

  const int bw = (w - 16 * 2 - 10) / 2;
  lv_obj_t* off = qpButton(box, 16, y0 + 132, bw, LV_SYMBOL_POWER "  Screen off", false);
  lv_obj_add_event_cb(off, onScreenOff, LV_EVENT_CLICKED, nullptr);
  s_qp.nightBtn = qpButton(box, 16 + bw + 10, y0 + 132, bw, LV_SYMBOL_EYE_CLOSE "  Night", true);
  lv_obj_add_event_cb(s_qp.nightBtn, onNight, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* grip = lv_obj_create(box);
  lv_obj_remove_style_all(grip);
  lv_obj_set_size(grip, 36, 4);
  lv_obj_set_style_bg_color(grip, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(grip, 2, 0);
  lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, -7);

  s_qp.snap = lv_draw_buf_create(w, QP_H + QP_LIP, LV_COLOR_FORMAT_RGB565, 0);
  s_qp.open = false;
  qpUpdateStatus(true);
  qpSyncValues();
}

void destroyQuickPanel() {
  if (s_qp.scrim) lv_obj_delete(s_qp.scrim);
  if (s_qp.box) lv_obj_delete(s_qp.box);
  if (s_qp.snap) lv_draw_buf_destroy(s_qp.snap);
  s_qp = {};
  portENTER_CRITICAL(&s_mux);
  sh.panelOpen = false;
  sh.panelValid = false;
  portEXIT_CRITICAL(&s_mux);
}

// Put LVGL in the open or closed state the compositor just animated to.
void showPanel(bool open) {
  if (!s_qp.box) return;
  s_qp.open = open;
  if (open) {
    lv_obj_set_y(s_qp.box, -QP_LIP);
    lv_obj_remove_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
    qpUpdateStatus(true);
    qpSyncValues();
    // Closing slides the panel off the page image, so make it current.
    markPageStale(sh.active);
  } else {
    lv_obj_set_y(s_qp.box, -(QP_H + QP_LIP + 8));
    lv_obj_add_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
  }
  portENTER_CRITICAL(&s_mux);
  sh.panelOpen = open;
  portEXIT_CRITICAL(&s_mux);
  s_gen++;
}

void hidePanelNow() {
  if (s_qp.open) showPanel(false);
}

void refreshPanelSnapshot() {
  if (!s_qp.box || !s_qp.snap || !s_qp.stale) return;
  portENTER_CRITICAL(&s_mux);
  const bool owned = sh.owned;
  if (!owned) sh.panelBusy = true;
  portEXIT_CRITICAL(&s_mux);
  if (owned) return;
  const uint32_t t0 = micros();
  lv_snapshot_reshape_draw_buf(s_qp.box, s_qp.snap);
  const bool ok = lv_snapshot_take_to_draw_buf(s_qp.box, LV_COLOR_FORMAT_RGB565, s_qp.snap) == LV_RESULT_OK;
  const uint32_t dt = micros() - t0;
  if (dt > s_snapUsMax) s_snapUsMax = dt;
  const uint16_t stride = (uint16_t)(s_qp.snap->header.stride / 2);
  portENTER_CRITICAL(&s_mux);
  sh.panelBusy = false;
  sh.panelValid = ok;
  if (ok) {
    sh.panelSrc = {(const uint16_t*)s_qp.snap->data + QP_LIP * stride, stride, true, (uint16_t)QP_H};
  }
  portEXIT_CRITICAL(&s_mux);
  s_qp.stale = !ok;
  s_lastSnapMs = millis();
}

void closePanelAnimated() {
  if (!s_qp.open) return;
  Src panel, page;
  int so, ph;
  if (uiTryBeginPanel(false, panel, page, so, ph)) {
    if (compBeginPanel(panel, page, so)) {
      compRelease(0, 200, -1);
      return;
    }
    releaseSwipe();
  }
  showPanel(false);  // images not ready: close without the slide
}

// --- Toast and detail card ------------------------------------------------------------
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_modalScrim = nullptr;
lv_obj_t* s_modalCard = nullptr;
int s_modalTile = -1;

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
    lv_obj_set_style_text_color(s_toast, lv_color_hex(C_TEXT), 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_label_create(s_toast);
  }
  lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
  // Above the page dots, which the flush task draws at the very bottom.
  lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
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
  if (s_modalCard) lv_obj_delete(s_modalCard);
  if (s_modalScrim) lv_obj_delete(s_modalScrim);
  s_modalCard = s_modalScrim = nullptr;
  s_modalTile = -1;
  setModalOpen(false);
}

void runAction(TileRt& r, const TileAction& a);
void refreshModal();

void onModalScrim(lv_event_t*) { closeModal(); }

void onModalPrimary(lv_event_t*) {
  if (s_modalTile < 0 || s_modalTile >= (int)s_tiles.size()) return;
  TileRt& r = s_tiles[s_modalTile];
  runAction(r, r.def->tap);
  refreshModal();
}

lv_obj_t* s_modalState = nullptr;
lv_obj_t* s_modalBtnLbl = nullptr;

void refreshModal() {
  if (s_modalTile < 0 || !s_modalCard) return;
  const TileRt& r = s_tiles[s_modalTile];
  String st = tileSub(r);
  HaEntity e;
  if (r.def->entity.length() && haGetEntity(r.def->entity, e))
    st = prettyState(entityDomain(r.def->entity), e);
  lv_label_set_text(s_modalState, st.c_str());
  if (r.def->type == TILE_TOGGLE)
    lv_label_set_text(s_modalBtnLbl, r.on ? "Turn off" : "Turn on");
}

// Detail card for a tile (hold action "more-info"). Phase 3 adds the HA
// attributes here (brightness, colour, position...).
void openModal(int tileIdx) {
  closeModal();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : domainColor(entityDomain(d.entity));

  s_modalScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_modalScrim);
  lv_obj_set_size(s_modalScrim, w, h);
  lv_obj_set_style_bg_color(s_modalScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_modalScrim, LV_OPA_50, 0);
  lv_obj_add_event_cb(s_modalScrim, onModalScrim, LV_EVENT_CLICKED, nullptr);

  const int cw = w - 40 < 360 ? w - 40 : 360;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, 214);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // taps on the card stay on the card
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 48, 48);
  lv_obj_set_pos(chip, 18, 18);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(chip, lv_color_hex(accent), 0);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_label_set_text(ic, iconSymbol(d));
  lv_obj_center(ic);

  lv_obj_t* name = lv_label_create(c);
  lv_label_set_text(name, tileLabel(d).c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_obj_set_pos(name, 80, 20);
  lv_obj_set_width(name, cw - 96);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_t* ent = lv_label_create(c);
  lv_label_set_text(ent, d.entity.length() ? d.entity.c_str() : "No entity");
  lv_obj_set_style_text_color(ent, lv_color_hex(C_DIM), 0);
  lv_obj_set_pos(ent, 80, 46);

  s_modalState = lv_label_create(c);
  lv_obj_set_style_text_font(s_modalState, &deck_font_28, 0);
  lv_obj_set_pos(s_modalState, 18, 84);

  const int bw = (cw - 18 * 2 - 10) / 2;
  lv_obj_t* primary = qpButton(c, 18, 214 - 18 - 44, bw, "", false);
  lv_obj_set_height(primary, 44);
  lv_obj_set_style_bg_color(primary, lv_color_hex(accent), 0);
  lv_obj_set_style_text_color(primary, lv_color_hex(C_TEXT_ON), 0);
  s_modalBtnLbl = lv_obj_get_child(primary, 0);
  lv_label_set_text(s_modalBtnLbl, d.type == TILE_TOGGLE ? "" : "Run");
  lv_obj_add_event_cb(primary, onModalPrimary, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* close = qpButton(c, 18 + bw + 10, 214 - 18 - 44, bw, "Close", false);
  lv_obj_set_height(close, 44);
  lv_obj_add_event_cb(close, onModalScrim, LV_EVENT_CLICKED, nullptr);

  s_modalTile = tileIdx;
  setModalOpen(true);
  refreshModal();
}

// --- Pages -----------------------------------------------------------------------------
lv_obj_t* makeRoot(int w, int h) {
  lv_obj_t* r = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, w, h);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

void makeTile(lv_obj_t* grid, const TileDef& def, int page, int col, int row, int w, int h) {
  const String domain = entityDomain(def.entity);
  const uint32_t accent = def.hasColor ? def.color : domainColor(domain);

  lv_obj_t* t = lv_obj_create(grid);
  lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, w, LV_GRID_ALIGN_STRETCH, row, h);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(t, 16, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_all(t, 9, 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(accent), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT_ON), LV_STATE_CHECKED);
  // Press feedback: the card darkens and shrinks a little, eased in 90 ms.
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(accent), lv_color_black(), 200),
                            LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_transform_width(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transition(t, &s_pressTrans, 0);
  lv_obj_set_style_transition(t, &s_pressTrans, LV_STATE_PRESSED);

  lv_obj_t* chip = lv_obj_create(t);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_text_color(chip, lv_color_hex(accent), 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_label_set_text(ic, iconSymbol(def));
  lv_obj_center(ic);

  lv_obj_t* name = lv_label_create(t);
  lv_label_set_text(name, tileLabel(def).c_str());
  // One line, cut with dots: HA names are often long and must not wrap into
  // the state line.
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(name, lv_pct(100));
  lv_obj_set_height(name, lv_font_get_line_height(&deck_font_14));
  lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, -14);
  lv_obj_t* sub = lv_label_create(t);
  if (def.type == TILE_SENSOR) {
    // The reading is the point of a sensor tile: big and top left, with the
    // icon moved out of its way into a smaller top-right chip.
    lv_obj_set_size(chip, 24, 24);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_text_font(sub, &deck_font_20, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  }

  TileRt r = {&def, page, t, name, sub, false, 0, false, false, 0};
  lv_label_set_text(sub, tileSub(r).c_str());
  const size_t idx = s_tiles.size();
  s_tiles.push_back(r);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
}

void addTilesPage(const PageDef& def) {
  const int w = logicalW(), h = logicalH();
  const int orient = w > h ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
  const int page = s_count;
  Page& p = s_pages[s_count];
  p = {};
  p.def = &def;
  p.root = makeRoot(w, h);

  lv_obj_t* t = lv_label_create(p.root);
  lv_label_set_text(t, def.name.length() ? def.name.c_str() : def.id.c_str());
  lv_obj_set_style_text_font(t, &deck_font_20, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TITLE), 0);
  lv_obj_set_pos(t, 12, 5);
  p.clock = lv_label_create(p.root);
  lv_label_set_text(p.clock, "");
  lv_obj_set_style_text_color(p.clock, lv_color_hex(C_CLOCK), 0);
  lv_obj_align(p.clock, LV_ALIGN_TOP_RIGHT, -12, 8);
  p.wifi = lv_label_create(p.root);
  lv_label_set_text(p.wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(p.wifi, lv_color_hex(C_DIM), 0);
  lv_obj_align(p.wifi, LV_ALIGN_TOP_RIGHT, -62, 8);

  const int cols = def.cols[orient], rows = def.rows[orient];
  // Static descriptor arrays per page (LVGL keeps the pointers).
  static int32_t colDsc[MAX_PAGES][8];
  static int32_t rowDsc[MAX_PAGES][8];
  for (int i = 0; i < cols; i++) colDsc[page][i] = LV_GRID_FR(1);
  colDsc[page][cols] = LV_GRID_TEMPLATE_LAST;
  for (int i = 0; i < rows; i++) rowDsc[page][i] = LV_GRID_FR(1);
  rowDsc[page][rows] = LV_GRID_TEMPLATE_LAST;

  lv_obj_t* grid = lv_obj_create(p.root);
  lv_obj_remove_style_all(grid);
  lv_obj_set_pos(grid, 0, HEADER_H);
  lv_obj_set_size(grid, w, h - HEADER_H);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(grid, LV_LAYOUT_GRID);
  lv_obj_set_grid_dsc_array(grid, colDsc[page], rowDsc[page]);
  lv_obj_set_style_pad_all(grid, 8, 0);
  lv_obj_set_style_pad_top(grid, 2, 0);
  lv_obj_set_style_pad_row(grid, 7, 0);
  lv_obj_set_style_pad_column(grid, 7, 0);

  // Explicit positions first, then the rest flow into free cells.
  bool used[6][6] = {};
  auto fits = [&](int x, int y, int tw, int th) {
    if (x + tw > cols || y + th > rows) return false;
    for (int yy = y; yy < y + th; yy++)
      for (int xx = x; xx < x + tw; xx++)
        if (used[yy][xx]) return false;
    return true;
  };
  auto take = [&](int x, int y, int tw, int th) {
    for (int yy = y; yy < y + th; yy++)
      for (int xx = x; xx < x + tw; xx++) used[yy][xx] = true;
  };
  std::vector<bool> placed(def.tiles.size(), false);
  for (size_t i = 0; i < def.tiles.size(); i++) {
    const TilePos& ps = def.tiles[i].pos[orient];
    if (ps.set && fits(ps.x, ps.y, ps.w, ps.h)) {
      take(ps.x, ps.y, ps.w, ps.h);
      makeTile(grid, def.tiles[i], page, ps.x, ps.y, ps.w, ps.h);
      placed[i] = true;
    }
  }
  for (size_t i = 0; i < def.tiles.size(); i++) {
    if (placed[i]) continue;
    const TilePos& ps = def.tiles[i].pos[orient];
    const int tw = ps.w, th = ps.h;
    bool done = false;
    for (int y = 0; y < rows && !done; y++)
      for (int x = 0; x < cols && !done; x++)
        if (fits(x, y, tw, th)) {
          take(x, y, tw, th);
          makeTile(grid, def.tiles[i], page, x, y, tw, th);
          done = true;
        }
    if (!done) Serial.printf("Deck: page '%s' has no room for tile '%s'\n",
                             def.id.c_str(), def.tiles[i].id.c_str());
  }

  p.snap = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  p.stale = true;
  s_count++;
}

void addFacePage() {
  const int w = logicalW(), h = logicalH();
  Page& f = s_pages[s_count];
  f = {};
  f.isFace = true;
  f.root = makeRoot(w, h);
  s_canvas = lv_canvas_create(f.root);
  lv_canvas_set_buffer(s_canvas, s_face->getBuffer(), w, h, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_obj_set_pos(s_canvas, 0, 0);
  s_facePage = s_count;
  s_count++;
}

void buildPages() {
  const int w = logicalW(), h = logicalH();
  s_count = 0;
  s_tiles.clear();
  s_tiles.reserve(64);
  bool haveFace = false;
  for (const PageDef& pd : s_cfg.pages) {
    if (s_count >= MAX_PAGES) break;
    if (pd.metrics) {
      if (!haveFace) { addFacePage(); s_pages[s_count - 1].def = &pd; haveFace = true; }
    } else {
      addTilesPage(pd);
    }
  }
  if (!haveFace) {
    // The metric page is part of the product: put it first if the config
    // forgot it. Shift what was built one slot right.
    for (int i = s_count; i > 0; i--) s_pages[i] = s_pages[i - 1];
    for (TileRt& t : s_tiles) t.page++;
    const int n = s_count;
    s_count = 0;
    addFacePage();
    s_count = n + 1;
  }

  portENTER_CRITICAL(&s_mux);
  sh.count = s_count;
  sh.facePage = s_facePage;
  sh.owned = false;
  for (int i = 0; i < MAX_PAGES; i++) { sh.valid[i] = false; sh.busy[i] = false; }
  // The face page is shown straight from the renderer's sprite (already in
  // panel byte order) instead of a snapshot.
  sh.valid[s_facePage] = true;
  sh.src[s_facePage] = {(const uint16_t*)s_face->getBuffer(), (uint16_t)w, false};
  const int active = sh.active < s_count ? sh.active : s_facePage;
  sh.active = active;
  portEXIT_CRITICAL(&s_mux);

  panelSetPager(s_count, active);
  // Inactive pages wait just off the right edge: they keep their layout and
  // can be snapshotted, but their invalidations are clipped away.
  for (int i = 0; i < s_count; i++) lv_obj_set_pos(s_pages[i].root, i == active ? 0 : w, 0);
  s_lastMinute = -1;
  s_lastWifi = -1;
  s_faceDirty = true;
  buildQuickPanel();
  collectEntities();
}

void destroyPages() {
  closeModal();
  destroyQuickPanel();
  for (int i = 0; i < s_count; i++) {
    if (s_pages[i].root) lv_obj_delete(s_pages[i].root);
    if (s_pages[i].snap) lv_draw_buf_destroy(s_pages[i].snap);
    s_pages[i] = {};
  }
  s_count = 0;
  s_tiles.clear();
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
  panelSetPager(s_count, page);
  s_gen++;
  // New neighbours; cheap to re-take, and the page we left changed state.
  for (int i = 0; i < s_count; i++) s_pages[i].stale = true;
  if (page == s_facePage) s_faceDirty = true;
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
  // An open panel is what the next gesture slides; it goes first.
  if (s_qp.open && s_qp.stale) {
    refreshPanelSnapshot();
    return;
  }
  const int a = sh.active;
  int pick = -1;
  const int order[2] = {a + 1, a - 1};
  for (int k = 0; k < 2 && pick < 0; k++) {
    const int p = order[k];
    if (p >= 0 && p < s_count && !s_pages[p].isFace && s_pages[p].stale) pick = p;
  }
  for (int p = 0; p < s_count && pick < 0; p++)
    if (!s_pages[p].isFace && s_pages[p].stale) pick = p;
  if (pick < 0) {
    refreshPanelSnapshot();
    return;
  }
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

// Header clock and WiFi indicator, checked once a second; page images are
// only marked stale when something visible changed.
void updateHeaders() {
  const uint32_t now = millis();
  if (now - s_lastClockCheck < 1000) return;
  s_lastClockCheck = now;
  const int wifi = WiFi.status() == WL_CONNECTED ? 1 : 0;
  if (wifi != s_lastWifi) {
    s_lastWifi = wifi;
    for (int i = 0; i < s_count; i++) {
      if (!s_pages[i].wifi) continue;
      lv_obj_set_style_text_color(s_pages[i].wifi, lv_color_hex(wifi ? C_OK : C_DIM), 0);
      s_pages[i].stale = true;
    }
  }
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

  String source, err;
  deckConfigLoad(s_cfg, &source, &err);
  Serial.printf("Deck: config from %s, %u pages\n", source.c_str(), (unsigned)s_cfg.pages.size());

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
  lv_indev_set_long_press_time(s_indev, 450);

  static const lv_style_prop_t kPressProps[] = {
    LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV};
  lv_style_transition_dsc_init(&s_pressTrans, kPressProps, lv_anim_path_ease_out, 90, 0, nullptr);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_text_font(scr, &deck_font_14, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  haBegin();
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
      switch (sh.gesture) {
        case Shared::GEST_PAGE: switchTo(sh.active + landing); break;
        case Shared::GEST_PANEL_OPEN: showPanel(true); break;
        case Shared::GEST_PANEL_CLOSE: showPanel(false); break;
      }
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
  if (systemScreen && (sh.active != s_facePage || s_qp.open)) {
    compCancel();
    releaseSwipe();
    hidePanelNow();
    closeModal();
    switchTo(s_facePage);
  }
  portENTER_CRITICAL(&s_mux);
  sh.swipeEnabled = !systemScreen;
  sh.faceBusy = false;  // the renderer only draws before this call
  portEXIT_CRITICAL(&s_mux);

  if (compOwnsScreen()) return;  // LVGL stays frozen during a drag or slide

  if (s_faceDirty) {
    s_faceDirty = false;
    if (sh.active == s_facePage && s_canvas) lv_obj_invalidate(s_canvas);
  }
  updateHeaders();
  qpUpdateStatus(false);
  haSync();
  lv_timer_handler();
  refreshOneSnapshot();
  logStats();
}

void deckFaceDirty() { s_faceDirty = true; }

void deckToast(const char* text) {
  if (s_disp) deckToastShow(text, 1800);
}

bool deckFaceMayRender() {
  if (!s_disp) return true;  // before deckBegin (or if it failed)
  if (compOwnsScreen()) return false;
  if (sh.active != s_facePage) {
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

bool deckFacePageActive() { return !s_disp || sh.active == s_facePage; }

void deckApplyRotation(uint8_t rotation) {
  if (!s_disp) return;
  rotation &= 3;
  if (rotation == panelRotation()) return;
  panelQuiesce();
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

bool deckApplyConfig(const String& text, String& err) {
  if (!s_disp) {
    err = "display not running";
    return false;
  }
  DeckConfig next;
  if (!deckConfigStore(text, next, err)) return false;
  // Tiles point into s_cfg: tear the pages down before replacing it.
  panelQuiesce();
  releaseSwipe();
  destroyPages();
  s_cfg = next;
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
  Serial.printf("Deck: new config applied, %u pages\n", (unsigned)s_cfg.pages.size());
  return true;
}

bool deckTestSwipe(int dir) {
  if (!s_disp || (dir != 1 && dir != -1)) return false;
  return slideBy(dir);
}

bool deckTestPanel(bool open) {
  if (!s_disp || open == s_qp.open) return false;
  if (!open) {
    closePanelAnimated();
    return true;
  }
  Src panel, page;
  int so, ph;
  if (!uiTryBeginPanel(true, panel, page, so, ph)) return false;
  if (!compBeginPanel(panel, page, so)) {
    releaseSwipe();
    return false;
  }
  compRelease(ph, 240, 1);
  return true;
}

namespace deck {

bool uiTryBeginSwipe(int dir, Src& a, Src& b, int& startOff, int& pageA) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  const int target = sh.active + dir;
  if (sh.swipeEnabled && !sh.owned && !sh.panelOpen && !sh.modalOpen &&
      target >= 0 && target < sh.count &&
      sh.valid[target] && !sh.busy[target] &&
      !(target == sh.facePage && sh.faceBusy)) {
    sh.owned = true;
    sh.gesture = Shared::GEST_PAGE;
    const Src cur = {nullptr, 0, true};  // the live LVGL frame
    if (dir > 0) {
      a = cur;
      b = sh.src[target];
      startOff = 0;
      pageA = sh.active;
    } else {
      a = sh.src[target];
      b = cur;
      startOff = logicalW();
      pageA = target;
    }
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void uiSwipeAborted() { releaseSwipe(); }

bool uiTryBeginPanel(bool opening, Src& panel, Src& page, int& startOff, int& panelH) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  const int a = sh.active;
  if (sh.swipeEnabled && !sh.owned && !sh.modalOpen && sh.panelValid && !sh.panelBusy &&
      sh.panelOpen != opening) {
    if (opening) {
      page = {nullptr, 0, true};  // the live LVGL frame, no panel in it yet
      ok = true;
    } else if (sh.valid[a] && !sh.busy[a] && !(a == sh.facePage && sh.faceBusy)) {
      page = sh.src[a];  // the page without the panel on top
      ok = true;
    }
    if (ok) {
      sh.owned = true;
      sh.gesture = opening ? Shared::GEST_PANEL_OPEN : Shared::GEST_PANEL_CLOSE;
      panel = sh.panelSrc;
      panelH = panel.h;
      startOff = opening ? 0 : panel.h;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

bool uiPanelIsOpen() {
  portENTER_CRITICAL(&s_mux);
  const bool open = sh.panelOpen;
  portEXIT_CRITICAL(&s_mux);
  return open;
}

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
