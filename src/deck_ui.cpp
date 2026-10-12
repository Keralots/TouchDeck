// Guition JC3248W535 deck UI. See deck_ui.h for the threading contract.
#include "deck_ui.h"
#include "deck_config.h"
#include "deck_ha.h"
#include "deck_icons.h"
#include "deck_sys.h"
#include "deck_touch.h"
#include "display_ui.h"
#include "settings.h"
#include "led.h"
#include "pc_metrics.h"
#include "metric_format.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>
#include <vector>

using namespace deck;

namespace {

constexpr int MAX_PAGES = MAX_DECK_PAGES;
constexpr int HEADER_H = 30;
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
  lv_obj_t* title;
  lv_obj_t* clock;
  lv_obj_t* wifi;
  lv_obj_t* grid;
  lv_draw_buf_t* snap;
  const PageDef* def;
  bool stale;
};

// One tile on screen. LVGL user data holds the index into s_tiles.
struct TileRt {
  const TileDef* def;
  int page;
  lv_obj_t* obj;
  lv_obj_t* name;
  lv_obj_t* sub;
  lv_obj_t* icon;
  int iconPx;
  uint32_t iconCp;        // MDI codepoint shown, 0 = LVGL symbol
  bool on;
  uint32_t haVersion;     // last HA entity version applied
  bool pending;           // optimistic toggle waiting for HA
  bool pendingOn;
  uint32_t pendingUntil;
  // PC metric tiles
  lv_obj_t* unit;
  lv_obj_t* viz;          // chart or bar
  lv_chart_series_t* ser;
  int32_t* pts;           // chart points (PC_HISTORY_LEN), LVGL heap
  bool warn;
  bool compact;           // 1x1 number tile: value font shrinks to fit
};

lv_display_t* s_disp = nullptr;
lv_indev_t* s_indev = nullptr;
DeckConfig s_cfg;
Page s_pages[MAX_PAGES];
std::vector<TileRt> s_tiles;
int s_count = 0;
uint32_t s_gen = 1;
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
  bool swipeEnabled = false;
  bool owned = false;      // a swipe owns the screen until its landing is handled
  bool valid[MAX_PAGES] = {};
  bool busy[MAX_PAGES] = {};
  Src src[MAX_PAGES] = {};
  // Columns: adjacent pages of one group. colCur is the page a column shows
  // when it is swiped to (the one it showed last).
  int nCols = 0;
  int col[MAX_PAGES] = {};
  int colFirst[MAX_PAGES] = {};
  int colLen[MAX_PAGES] = {};
  int colCur[MAX_PAGES] = {};
  // What the current compositor gesture is, and for a page slide where it lands.
  enum Gesture : uint8_t { GEST_PAGE, GEST_PANEL_OPEN, GEST_PANEL_CLOSE } gesture = GEST_PAGE;
  int target = 0;
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

// Default MDI icon for an entity without one of its own.
const char* defaultIcon(const String& domain, const String& dc, const String& unit, bool on) {
  if (domain == "light") return on ? "lightbulb" : "lightbulb-outline";
  if (domain == "switch" && dc == "outlet") return on ? "power-plug" : "power-plug-off";
  if (domain == "switch" || domain == "input_boolean")
    return on ? "toggle-switch-variant" : "toggle-switch-variant-off";
  if (domain == "fan") return on ? "fan" : "fan-off";
  if (domain == "lock") return on ? "lock" : "lock-open-variant";
  if (domain == "cover") return on ? "window-shutter-open" : "window-shutter";
  if (domain == "scene") return "palette";
  if (domain == "script") return "script-text";
  if (domain == "automation") return "robot";
  if (domain == "button" || domain == "input_button") return "gesture-tap-button";
  if (domain == "media_player") return "speaker";
  if (domain == "climate") return "thermostat";
  if (domain == "weather") return "weather-partly-cloudy";
  if (domain == "person") return "account";
  if (domain == "binary_sensor") {
    if (dc == "door" || dc == "garage_door") return on ? "door-open" : "door-closed";
    if (dc == "window") return on ? "window-open" : "window-closed";
    if (dc == "motion" || dc == "occupancy" || dc == "presence") return "motion-sensor";
    return on ? "checkbox-marked-circle" : "checkbox-blank-circle-outline";
  }
  if (domain == "sensor") {
    if (dc == "temperature" || unit.startsWith("\xC2\xB0")) return "thermometer";
    if (dc == "humidity") return "water-percent";
    if (dc == "power") return "flash";
    if (dc == "energy") return "lightning-bolt";
    if (dc == "battery") return "battery";
    if (dc == "illuminance") return "brightness-5";
    if (dc == "pressure" || dc == "atmospheric_pressure") return "gauge";
    if (dc == "carbon_dioxide") return "molecule-co2";
    if (dc == "voltage") return "sine-wave";
    if (dc == "current") return "current-ac";
    return "eye";
  }
  return "gesture-tap-button";
}

// Puts a tile's icon on a label: the tile's own MDI icon, else the entity's
// icon in HA, else a default; an LVGL symbol while no icon font is loaded.
// Returns the codepoint shown (0 = symbol); shown = what the label has now.
uint32_t showIcon(lv_obj_t* label, const TileDef& d, const HaEntity* e, bool on, int px,
                  uint32_t shown) {
  const lv_font_t* f = iconFont(px);
  uint32_t cp = 0;
  if (f) {
    if (d.icon.length()) cp = iconCodepoint(d.icon);
    if (!cp && e && e->icon.length()) cp = iconCodepoint(e->icon);
    if (!cp)
      cp = iconCodepoint(defaultIcon(entityDomain(d.entity), e ? e->deviceClass : String(),
                                     e ? e->unit : String(), on));
  }
  if (cp && cp == shown) return cp;
  if (cp) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(label, f, 0);
    lv_label_set_text(label, u);
  } else {
    lv_label_set_text(label, iconSymbol(d));
  }
  return cp;
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
void setPager(int page);
void releaseSwipe();
void edOpen(int page);

// Slide one column (or, vertical, one page of the column) toward dir through
// the compositor, as a fling would.
bool slideBy(int dir, bool vertical) {
  Src a, b;
  int so, idxA;
  if (!uiTryBeginSwipe(dir, vertical, a, b, so, idxA)) return false;
  if (!compBeginDrag(a, b, so, idxA, vertical)) {
    releaseSwipe();
    return false;
  }
  const int full = vertical ? logicalH() : logicalW();
  compRelease(dir > 0 ? full : 0, SLIDE_MS, (int8_t)dir);
  return true;
}

void goToPage(const String& id) {
  for (int i = 0; i < s_count; i++) {
    if (!s_pages[i].def || s_pages[i].def->id != id) continue;
    const int a = sh.active;
    if (i == a) return;
    const int dc = sh.col[i] - sh.col[a];
    if (dc == 0 && (i - a == 1 || i - a == -1) && slideBy(i - a, true)) return;
    if ((dc == 1 || dc == -1) && sh.colCur[sh.col[i]] == i && slideBy(dc, false)) return;
    switchTo(i);  // not a neighbour (or images not ready): jump
    return;
  }
}

// --- Tile events ------------------------------------------------------------------------
void openModal(int tileIdx);
void refreshModal();
void deckToastShow(const char* text, uint32_t holdMs);
void qpSyncValues();

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
      // "25.9°C" (no space before a degree sign) saves the width that
      // decides whether a reading fits a small tile.
      return String(buf) + (e.unit.startsWith("\xC2\xB0") ? "" : " ") + e.unit;
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
  r.iconCp = showIcon(r.icon, d, &e, on, r.iconPx, r.iconCp);
  lv_obj_set_style_opa(r.obj, avail ? LV_OPA_COVER : LV_OPA_50, 0);
  if (!d.label.length() && e.name.length()) lv_label_set_text(r.name, e.name.c_str());
  if (!d.sub.length()) lv_label_set_text(r.sub, prettyState(domain, e).c_str());
  markPageStale(r.page);
}

uint32_t s_haSeen = 0;

// The LED's day level from its HA entity, when that entity changed (force:
// also when not, to undo a refused slider write). A state that is not a level
// (unavailable, unknown) keeps the last one, as does a lost link.
uint32_t s_ledSeen = 0;  // entity version last applied, 0 = none

void ledFromHa(bool force) {
  if (!s_cfg.ledEntity.length()) return;
  HaEntity e;
  if (!haGetEntity(s_cfg.ledEntity, e) || !e.known) return;
  if (!force && e.version == s_ledSeen) return;  // other entities changed
  s_ledSeen = e.version;
  const String d = entityDomain(s_cfg.ledEntity);
  int16_t level;
  if (d == "input_number") {
    char* end;
    const float v = strtof(e.state.c_str(), &end);
    const float lo = isnan(e.numMin) ? 0 : e.numMin, hi = isnan(e.numMax) ? 100 : e.numMax;
    if (end == e.state.c_str() || hi <= lo) return;
    level = (int16_t)lroundf(constrain((v - lo) / (hi - lo), 0.0f, 1.0f) * 255);
  } else if (e.state == "on") {
    level = d == "light" && e.brightness >= 0 ? e.brightness : LED_HA_LOCAL;
  } else if (e.state == "off") {
    level = 0;
  } else {
    return;
  }
  if (level == ledHaLevel()) return;
  ledSetHaLevel(level);
  qpSyncValues();
}

// The Light slider by day on a linked LED: send the level to the entity and
// use it at once (HA echoes it back). False when HA is not connected.
bool ledToHa(uint8_t v) {
  const String& id = s_cfg.ledEntity;
  const String d = entityDomain(id);
  bool ok;
  if (d == "input_number") {
    HaEntity e;
    haGetEntity(id, e);
    const float lo = isnan(e.numMin) ? 0 : e.numMin, hi = isnan(e.numMax) ? 100 : e.numMax;
    if (hi <= lo) return false;
    float val = lo + (hi - lo) * v / 255.0f;
    if (e.numStep > 0) {
      val = lo + lroundf((val - lo) / e.numStep) * e.numStep;
      if (val > hi) val -= e.numStep;  // HA refuses values past max
    }
    val = constrain(val, lo, hi);
    char num[24];
    snprintf(num, sizeof num, "%.7g", val);
    ok = haCallService("input_number", "set_value", id, String("{\"value\":") + num + "}");
    // The level of the value sent: HA sends nothing back when it is unchanged.
    v = (uint8_t)lroundf((val - lo) / (hi - lo) * 255);
  } else if (d == "light") {
    ok = v ? haCallService("light", "turn_on", id, "{\"brightness\":" + String(v) + "}")
           : haCallService("light", "turn_off", id, "");
  } else {
    // input_boolean: "on" means the local day level, so the slider sets that.
    ok = haCallService("input_boolean", v ? "turn_on" : "turn_off", id, "");
    if (ok && v) {
      ledSettings.brightness = v;
      saveSettings();
    }
  }
  if (ok) ledSetHaLevel(d == "input_boolean" ? (v ? LED_HA_LOCAL : 0) : v);
  return ok;
}

void haSync() {
  const uint32_t v = haStoreVersion();
  if (v != s_haSeen) {
    s_haSeen = v;
    for (TileRt& r : s_tiles) applyHaToTile(r, false);
    refreshModal();
    ledFromHa(false);
  }
  String ent, msg;
  while (haTakeFailure(ent, msg)) {
    for (TileRt& r : s_tiles) {
      if (r.def->entity != ent || !r.pending) continue;
      r.pending = false;
      applyHaToTile(r, true);
    }
    if (ent == s_cfg.ledEntity) ledFromHa(true);  // back to what HA has
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
  if (s_cfg.ledEntity.length()) ids.push_back(s_cfg.ledEntity);
  else ledSetHaLevel(LED_HA_NONE);
  s_ledSeen = 0;
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

// Top bar (page name, WiFi, clock) shown; owner setting, NVS "deckui"/"hdr".
bool s_header = true;

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
  lv_obj_t* headerBtn;
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
    const uint8_t lv = (ledSettings.nightEnabled && nightBrightnessActive()) ? ledSettings.nightBrightness
                                                                           : ledDayLevel();
    lv_slider_set_value(s_qp.lightSlider, lv, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_qp.lightVal, "%d%%", (lv * 100 + 127) / 255);
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
    // By day a linked LED is set through its HA entity; the night level stays local.
    if (s_cfg.ledEntity.length() && !(ledSettings.nightEnabled && nightBrightnessActive())) {
      if (!ledToHa(v)) deckToastShow("Home Assistant is offline", 1800);
      clearLedPreview();
      qpSyncValues();
      return;
    }
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

void setHeader(bool on);
void onHeaderBtn(lv_event_t*) { setHeader(lv_obj_has_state(s_qp.headerBtn, LV_STATE_CHECKED)); }
void onEditBtn(lv_event_t*) { edOpen(sh.active); }

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
  lv_slider_set_range(s_qp.lightSlider, 0, 255);  // 0 = LED off (or the HA entity off)
  s_qp.lightVal = lv_label_create(box);
  lv_obj_set_pos(s_qp.lightVal, w - valW - 10, y0 + 98);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_RELEASED, nullptr);

  // Four buttons; on a portrait screen they carry only their symbols.
  const int bw = (w - 16 * 2 - 3 * 10) / 4;
  const bool wide = w >= 400;
  lv_obj_t* off = qpButton(box, 16, y0 + 132, bw, wide ? LV_SYMBOL_POWER "  Off" : LV_SYMBOL_POWER, false);
  lv_obj_add_event_cb(off, onScreenOff, LV_EVENT_CLICKED, nullptr);
  s_qp.nightBtn = qpButton(box, 16 + bw + 10, y0 + 132, bw, wide ? LV_SYMBOL_EYE_CLOSE "  Night" : LV_SYMBOL_EYE_CLOSE, true);
  lv_obj_add_event_cb(s_qp.nightBtn, onNight, LV_EVENT_VALUE_CHANGED, nullptr);
  s_qp.headerBtn = qpButton(box, 16 + 2 * (bw + 10), y0 + 132, bw, wide ? LV_SYMBOL_UP "  Top bar" : LV_SYMBOL_UP, true);
  if (s_header) lv_obj_add_state(s_qp.headerBtn, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_qp.headerBtn, onHeaderBtn, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* edit = qpButton(box, 16 + 3 * (bw + 10), y0 + 132, bw, wide ? LV_SYMBOL_EDIT "  Edit" : LV_SYMBOL_EDIT, false);
  lv_obj_add_event_cb(edit, onEditBtn, LV_EVENT_CLICKED, nullptr);

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
lv_obj_t* s_modalState = nullptr;
lv_obj_t* s_modalBtnLbl = nullptr;
lv_obj_t* s_modalSlider = nullptr;
lv_obj_t* s_modalIcon = nullptr;
uint32_t s_modalIconCp = 0;

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
  s_modalSlider = nullptr;
  s_modalIcon = nullptr;
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

uint32_t s_modalSentMs = 0;      // last brightness call while dragging
uint32_t s_modalReleasedMs = 0;  // HA echoes old values for a moment after

bool modalIsLight() {
  return s_modalTile >= 0 && entityDomain(s_tiles[s_modalTile].def->entity) == "light";
}

void sendBrightness(int pct) {
  if (s_modalTile < 0) return;
  const String& entity = s_tiles[s_modalTile].def->entity;
  if (!haCallService("light", "turn_on", entity, String("{\"brightness_pct\":") + pct + "}"))
    deckToastShow("Home Assistant is offline", 1500);
}

void onModalSlider(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int pct = lv_slider_get_value(s_modalSlider);
  const uint32_t now = millis();
  if (code == LV_EVENT_VALUE_CHANGED) {
    lv_label_set_text_fmt(s_modalState, "%d%%", pct);
    // Live while dragging, but not faster than HA and the light can follow.
    if (now - s_modalSentMs >= 300) {
      s_modalSentMs = now;
      sendBrightness(pct);
    }
  } else if (code == LV_EVENT_RELEASED) {
    s_modalReleasedMs = now;
    sendBrightness(pct);
  }
}

void refreshModal() {
  if (s_modalTile < 0 || !s_modalCard) return;
  const TileRt& r = s_tiles[s_modalTile];
  const String domain = entityDomain(r.def->entity);
  String st = tileSub(r);
  HaEntity e;
  const bool haKnown = r.def->entity.length() && haGetEntity(r.def->entity, e);
  if (haKnown) st = prettyState(domain, e);
  if (s_modalSlider) {
    const bool dragging = lv_obj_has_state(s_modalSlider, LV_STATE_PRESSED);
    const bool settling = millis() - s_modalReleasedMs < 1500;
    if (!dragging && !settling) {
      const bool on = haKnown && e.state == "on";
      const int pct = on && e.brightness >= 0 ? (e.brightness * 100 + 127) / 255 : 0;
      lv_slider_set_value(s_modalSlider, pct > 0 ? pct : 1, LV_ANIM_OFF);
      lv_label_set_text(s_modalState, st.c_str());
    }
  } else {
    lv_label_set_text(s_modalState, st.c_str());
  }
  if (r.def->type == TILE_TOGGLE)
    lv_label_set_text(s_modalBtnLbl, r.on ? "Turn off" : "Turn on");
  const bool on = haKnown && haStateOn(domain, e.state);
  s_modalIconCp = showIcon(s_modalIcon, *r.def, haKnown ? &e : nullptr, on, 28, s_modalIconCp);
}

// Detail card for a tile (hold action "more-info"): name, entity, state, a
// brightness slider for lights, and the tap action as a button.
void openModal(int tileIdx) {
  closeModal();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : domainColor(entityDomain(d.entity));
  s_modalTile = tileIdx;
  const bool light = modalIsLight();

  s_modalScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_modalScrim);
  lv_obj_set_size(s_modalScrim, w, h);
  lv_obj_set_style_bg_color(s_modalScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_modalScrim, LV_OPA_50, 0);
  lv_obj_add_event_cb(s_modalScrim, onModalScrim, LV_EVENT_CLICKED, nullptr);

  const int cw = w - 40 < 380 ? w - 40 : 380;
  const int ch = light ? 236 : 196;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // taps on the card stay on the card
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 44, 44);
  lv_obj_set_pos(chip, 16, 16);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(chip, lv_color_hex(accent), 0);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  s_modalIcon = lv_label_create(chip);
  s_modalIconCp = 0;
  lv_label_set_text(s_modalIcon, "");
  lv_obj_center(s_modalIcon);

  // Name and entity id: one line each, cut with dots (ids get long).
  lv_obj_t* name = lv_label_create(c);
  String title = tileLabel(d);
  HaEntity he;
  if (!d.label.length() && d.entity.length() && haGetEntity(d.entity, he) && he.name.length())
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
    lv_slider_set_range(sl, 1, 100);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2E333C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(sl, onModalSlider, LV_EVENT_RELEASED, nullptr);
  }

  const int bw = (cw - 16 * 2 - 10) / 2;
  const int by = ch - 16 - 44;
  lv_obj_t* primary = qpButton(c, 16, by, bw, "", false);
  lv_obj_set_height(primary, 44);
  lv_obj_set_style_bg_color(primary, lv_color_hex(accent), 0);
  lv_obj_set_style_text_color(primary, lv_color_hex(C_TEXT_ON), 0);
  s_modalBtnLbl = lv_obj_get_child(primary, 0);
  lv_label_set_text(s_modalBtnLbl, d.type == TILE_TOGGLE ? "" : (d.type == TILE_SENSOR ? "Refresh" : "Run"));
  lv_obj_add_event_cb(primary, onModalPrimary, LV_EVENT_CLICKED, nullptr);
  if (d.type == TILE_SENSOR) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* close = qpButton(c, d.type == TILE_SENSOR ? 16 : 16 + bw + 10, by,
                             d.type == TILE_SENSOR ? cw - 32 : bw, "Close", false);
  lv_obj_set_height(close, 44);
  lv_obj_add_event_cb(close, onModalScrim, LV_EVENT_CLICKED, nullptr);

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

// --- PC metric tiles ------------------------------------------------------------------
// Identity colours by metric id.
constexpr uint32_t kMetricPalette[8] = {0x3987E5, 0x00A35C, 0xD55181, 0xC98500,
                                        0x199E70, 0xD95926, 0x9085E9, 0xE66767};
constexpr uint32_t C_WARN = 0xE5A50A;

uint32_t metricAccent(const TileDef& d) {
  return d.hasColor ? d.color : kMetricPalette[(d.metric - 1) % 8];
}

void setText(lv_obj_t* l, const char* text) {
  if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

// Pull the latest companion packet into one metric tile.
void applyPcToTile(TileRt& r) {
  const TileDef& d = *r.def;
  const PcMetric* m = pcData.online ? pcMetricFindById(d.metric) : nullptr;
  if (!m) {
    setText(r.sub, "--");
    setText(r.unit, "");
    if (!d.label.length()) setText(r.name, "PC offline");
    lv_obj_set_style_opa(r.obj, LV_OPA_50, 0);
    return;
  }
  lv_obj_set_style_opa(r.obj, LV_OPA_COVER, 0);
  if (!d.label.length()) setText(r.name, m->name);
  MetricText mt;
  formatMetricText(*m, m->value, mt);
  setText(r.sub, mt.value);
  setText(r.unit, mt.unit);
  if (r.compact) {
    // "2.3k RPM" overflows a narrow 1x1 tile at 28 px: drop the value to 20 px.
    lv_point_t v, u;
    lv_text_get_size(&v, mt.value, &deck_font_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&u, mt.unit, &deck_font_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_update_layout(r.obj);
    const bool fits = v.x + 4 + u.x <= lv_obj_get_content_width(r.obj);
    const lv_font_t* f = fits ? &deck_font_28 : &deck_font_20;
    if (lv_obj_get_style_text_font(r.sub, LV_PART_MAIN) != f) lv_obj_set_style_text_font(r.sub, f, 0);
  }

  const float scale = metricScaleMax(*m, d.scaleMax);
  const float pct = scale > 0 ? m->value * 100.0f / scale : 0;
  // Enter the warning colour at warnThresholdPct, leave it 5 points below.
  const float enter = (float)dispSettings.warnThresholdPct;
  const bool warn = enter > 0 && (r.warn ? pct >= enter - 5 : pct >= enter);
  if (warn != r.warn) {
    r.warn = warn;
    lv_obj_set_style_text_color(r.sub, lv_color_hex(warn ? C_WARN : C_TEXT), 0);
  }
  const uint32_t accent = warn ? C_WARN : metricAccent(d);
  if (r.viz && d.style == METRIC_BAR) {
    lv_bar_set_value(r.viz, (int32_t)constrain(pct, 0.0f, 100.0f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
  } else if (r.viz && r.pts) {
    // History as tenths, oldest first, padded on the left while it fills.
    const SlotHistory& hs = pcHistoryById[d.metric];
    const int n = hs.metricId == d.metric ? hs.count : 0;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) {
      const float v = pcHistoryAt(hs, (uint8_t)i);
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    for (int i = 0; i < PC_HISTORY_LEN; i++) {
      const int k = i - (PC_HISTORY_LEN - n);
      r.pts[i] = k < 0 ? LV_CHART_POINT_NONE : (int32_t)lroundf(pcHistoryAt(hs, (uint8_t)k) * 10);
    }
    if (n) {
      const float pad = (hi - lo) * 0.15f + 0.5f;
      lv_chart_set_axis_range(r.viz, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf((lo - pad) * 10),
                              (int32_t)ceilf((hi + pad) * 10));
    }
    lv_chart_set_series_color(r.viz, r.ser, lv_color_hex(accent));
    lv_chart_refresh(r.viz);
  }
}

// Number big at the bottom, or a chart / bar under a smaller number (2x1 and
// up only; a 1x1 tile always shows the number).
void makeMetricContent(TileRt& r, lv_obj_t* t, int w, int h) {
  const TileDef& d = *r.def;
  const bool roomy = w >= 2 || h >= 2;
  const MetricStyle style = roomy ? d.style : METRIC_NUMBER;
  const uint32_t accent = metricAccent(d);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only

  r.name = lv_label_create(t);
  lv_label_set_text(r.name, d.label.length() ? d.label.c_str() : "");
  const lv_font_t* nameFont = roomy ? &deck_font_20 : &deck_font_14;
  lv_obj_set_style_text_font(r.name, nameFont, 0);
  lv_obj_set_style_text_color(r.name, lv_color_hex(accent), 0);
  lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(r.name, lv_pct(100), lv_font_get_line_height(nameFont));
  lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);

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
  lv_obj_set_style_pad_bottom(r.unit, 4, 0);

  if (style == METRIC_NUMBER) {
    lv_obj_set_style_text_font(r.sub, roomy ? &lv_font_montserrat_40 : &deck_font_28, 0);
    lv_obj_set_style_text_font(r.unit, roomy ? &deck_font_20 : &deck_font_14, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    r.compact = !roomy;
    return;
  }
  lv_obj_set_style_text_font(r.sub, &deck_font_28, 0);
  lv_obj_set_style_text_font(r.unit, &deck_font_14, 0);
  if (style == METRIC_BAR) {
    lv_obj_align(row, LV_ALIGN_LEFT_MID, 0, 4);
    r.viz = lv_bar_create(t);
    lv_obj_set_size(r.viz, lv_pct(100), 8);
    lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(r.viz, 0, 100);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(0x2E333C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
    return;
  }
  // Chart: the number top right, the line under it.
  lv_obj_align(row, LV_ALIGN_TOP_RIGHT, 0, -6);
  lv_obj_set_width(r.name, lv_pct(55));
  r.viz = lv_chart_create(t);
  lv_obj_remove_style_all(r.viz);
  lv_obj_set_size(r.viz, lv_pct(100), lv_pct(62));
  lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_chart_set_type(r.viz, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(r.viz, 0, 0);
  lv_chart_set_point_count(r.viz, PC_HISTORY_LEN);
  lv_obj_set_style_line_width(r.viz, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(r.viz, LV_OBJ_FLAG_CLICKABLE);
  r.ser = lv_chart_add_series(r.viz, lv_color_hex(accent), LV_CHART_AXIS_PRIMARY_Y);
  r.pts = (int32_t*)lv_malloc(sizeof(int32_t) * PC_HISTORY_LEN);
  for (int i = 0; i < PC_HISTORY_LEN; i++) r.pts[i] = LV_CHART_POINT_NONE;
  lv_chart_set_series_ext_y_array(r.viz, r.ser, r.pts);
}

uint32_t s_pcSeen = 0;
bool s_pcOnline = false;

// New companion packet or a change of the online flag: refresh metric tiles.
void pcSync(bool force) {
  if (!force && pcData.lastReceived == s_pcSeen && pcData.online == s_pcOnline) return;
  s_pcSeen = pcData.lastReceived;
  s_pcOnline = pcData.online;
  for (TileRt& r : s_tiles) {
    if (r.def->type != TILE_METRIC) continue;
    applyPcToTile(r);
    markPageStale(r.page);
  }
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

  if (def.type == TILE_METRIC) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeMetricContent(r, t, w, h);
    s_tiles.push_back(r);
    applyPcToTile(s_tiles.back());
    return;
  }

  lv_obj_t* chip = lv_obj_create(t);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_text_color(chip, lv_color_hex(accent), 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_obj_center(ic);

  // Layout: icon top left with the state top right next to it, the name at
  // the bottom over up to two lines (bigger on wide tiles). A sensor shows
  // its reading big at the top instead, with a small icon top right.
  const bool wide = w >= 2;
  const lv_font_t* nameFont = wide ? &deck_font_20 : &deck_font_14;
  lv_obj_t* name = lv_label_create(t);
  lv_label_set_text(name, tileLabel(def).c_str());
  lv_obj_set_style_text_font(name, nameFont, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(name, lv_pct(100));
  lv_obj_set_height(name, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(name, 2 * lv_font_get_line_height(nameFont), 0);
  lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  lv_obj_t* sub = lv_label_create(t);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
  if (def.type == TILE_SENSOR) {
    lv_obj_set_size(chip, 22, 22);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 2, -2);
    const lv_font_t* valFont = wide ? &deck_font_28 : &deck_font_20;
    lv_obj_set_style_text_font(sub, valFont, 0);
    lv_obj_set_size(sub, lv_pct(100), lv_font_get_line_height(valFont));
    lv_obj_set_style_pad_right(sub, 22, 0);  // clear of the icon
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(sub, lv_pct(100), lv_font_get_line_height(&deck_font_14));
    lv_obj_set_style_pad_left(sub, 36, 0);  // clear of the icon
    lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, 0, 7);
  }

  const int iconPx = def.type == TILE_SENSOR ? 16 : 20;
  TileRt r = {};
  r.def = &def;
  r.page = page;
  r.obj = t;
  r.name = name;
  r.sub = sub;
  r.icon = ic;
  r.iconPx = iconPx;
  r.iconCp = showIcon(ic, def, nullptr, false, iconPx, 0);
  lv_label_set_text(sub, tileSub(r).c_str());
  const size_t idx = s_tiles.size();
  s_tiles.push_back(r);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
}

// Shows or hides a tiles page's top bar; the grid takes the freed height.
void layoutHeader(Page& p) {
  if (!p.grid) return;
  const int w = logicalW(), h = logicalH();
  for (lv_obj_t* o : {p.title, p.clock, p.wifi}) {
    if (s_header) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  const int top = s_header ? HEADER_H : 0;
  lv_obj_set_pos(p.grid, 0, top);
  lv_obj_set_size(p.grid, w, h - top);
  lv_obj_set_style_pad_top(p.grid, s_header ? 2 : 8, 0);
  p.stale = true;
}

// Where each tile of a page lands: explicit cells first (in tile order), then
// the rest flow into the first free place that fits. The portal grid editor
// mirrors this rule (layout() in deck_web.h).
struct Cell {
  bool placed;
  int x, y, w, h;
};

void placeTiles(const PageDef& def, int orient, std::vector<Cell>& out) {
  const int cols = def.cols[orient], rows = def.rows[orient];
  bool used[6][6] = {};
  auto fits = [&](int x, int y, int tw, int th) {
    if (x + tw > cols || y + th > rows) return false;
    for (int yy = y; yy < y + th; yy++)
      for (int xx = x; xx < x + tw; xx++)
        if (used[yy][xx]) return false;
    return true;
  };
  auto take = [&](Cell& c, int x, int y) {
    for (int yy = y; yy < y + c.h; yy++)
      for (int xx = x; xx < x + c.w; xx++) used[yy][xx] = true;
    c.placed = true;
    c.x = x;
    c.y = y;
  };
  out.assign(def.tiles.size(), Cell{false, 0, 0, 1, 1});
  for (size_t i = 0; i < def.tiles.size(); i++) {
    const TilePos& ps = def.tiles[i].pos[orient];
    out[i].w = ps.w;
    out[i].h = ps.h;
    if (ps.set && fits(ps.x, ps.y, ps.w, ps.h)) take(out[i], ps.x, ps.y);
  }
  for (Cell& c : out)
    for (int y = 0; y < rows && !c.placed; y++)
      for (int x = 0; x < cols && !c.placed; x++)
        if (fits(x, y, c.w, c.h)) take(c, x, y);
}

void onGridHold(lv_event_t* e) {
  const int page = (int)(uintptr_t)lv_event_get_user_data(e);
  if (page == sh.active) edOpen(page);
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
  p.title = t;
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
  p.grid = grid;
  lv_obj_remove_style_all(grid);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(grid, LV_LAYOUT_GRID);
  lv_obj_set_grid_dsc_array(grid, colDsc[page], rowDsc[page]);
  lv_obj_set_style_pad_all(grid, 8, 0);
  layoutHeader(p);
  lv_obj_set_style_pad_row(grid, 7, 0);
  lv_obj_set_style_pad_column(grid, 7, 0);

  // Long press on an empty part of the page opens the edit mode.
  lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(grid, onGridHold, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)page);

  std::vector<Cell> cells;
  placeTiles(def, orient, cells);
  for (size_t i = 0; i < cells.size(); i++) {
    const Cell& c = cells[i];
    if (c.placed) makeTile(grid, def.tiles[i], page, c.x, c.y, c.w, c.h);
    else Serial.printf("Deck: page '%s' has no room for tile '%s'\n",
                       def.id.c_str(), def.tiles[i].id.c_str());
  }

  p.snap = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  p.stale = true;
  s_count++;
}

// --- Edit mode ------------------------------------------------------------------------
// Long press on an empty part of a page, or Edit in the quick panel: a sketch
// of the active page on the top layer. Drag a tile to a free place or onto a
// tile of its size (swap), resize it from the corner, remove it with x. Done
// writes the page's positions into the stored deck.json; Cancel drops them.
constexpr int ED_BAR_H = 48;
constexpr int ED_PAD = 8;
constexpr int ED_GAP = 7;
constexpr int ED_SLOP = 6;
constexpr uint32_t C_ED_OK = 0x5CC9A7;
constexpr uint32_t C_ED_BAD = 0xE5534B;

struct EdTile {
  Cell c;
  bool removed;
  lv_obj_t* box;
};

struct EditState {
  lv_obj_t* root = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* ghost = nullptr;
  int cfgPage = -1;  // index into s_cfg.pages
  String name;
  int orient = 0, cols = 1, rows = 1, cw = 1, ch = 1;
  std::vector<EdTile> tiles;
  bool dirty = false;
  enum Pending : uint8_t { P_NONE, P_SAVE, P_CANCEL } pending = P_NONE;
  // The drag in progress: which tile, resize or move, where it would land.
  int k = -1;
  bool sizing = false, moved = false, ok = false;
  int swap = -1;
  lv_point_t p0 = {};
  int bx0 = 0, by0 = 0;
  Cell to = {};
};
EditState s_ed;

void edPlace(lv_obj_t* o, const Cell& c) {
  lv_obj_set_pos(o, ED_PAD + c.x * (s_ed.cw + ED_GAP), ED_BAR_H + c.y * (s_ed.ch + ED_GAP));
  lv_obj_set_size(o, c.w * s_ed.cw + (c.w - 1) * ED_GAP, c.h * s_ed.ch + (c.h - 1) * ED_GAP);
}

// Can tile k go to c? Free cells, or (a move, not a resize) exactly onto a
// tile of the same size, which then takes k's place (*swap).
bool edCheck(int k, const Cell& c, bool sizing, int* swap) {
  *swap = -1;
  if (c.x < 0 || c.y < 0 || c.x + c.w > s_ed.cols || c.y + c.h > s_ed.rows) return false;
  const Cell& me = s_ed.tiles[k].c;
  int hit = -1, hits = 0;
  for (int j = 0; j < (int)s_ed.tiles.size(); j++) {
    const EdTile& o = s_ed.tiles[j];
    if (j == k || !o.c.placed || o.removed) continue;
    if (o.c.x < c.x + c.w && c.x < o.c.x + o.c.w && o.c.y < c.y + c.h && c.y < o.c.y + o.c.h) {
      hit = j;
      hits++;
    }
  }
  if (!hits) return true;
  if (sizing || hits > 1 || !me.placed || c.w != me.w || c.h != me.h) return false;
  const Cell& o = s_ed.tiles[hit].c;
  if (o.x != c.x || o.y != c.y || o.w != me.w || o.h != me.h) return false;
  *swap = hit;
  return true;
}

bool edApply(int k, const Cell& c, bool sizing) {
  int swap;
  if (!edCheck(k, c, sizing, &swap)) return false;
  EdTile& t = s_ed.tiles[k];
  if (swap >= 0) {
    s_ed.tiles[swap].c.x = t.c.x;
    s_ed.tiles[swap].c.y = t.c.y;
  }
  t.c = c;
  t.c.placed = true;
  s_ed.dirty = true;
  return true;
}

void edLayout() {
  for (EdTile& t : s_ed.tiles)
    if (t.box) edPlace(t.box, t.c);
}

void edDragEvent(lv_event_t* e) {
  const uint32_t tag = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  const int k = tag & 0xFF;
  const bool sizing = tag & 0x100;
  if (k >= (int)s_ed.tiles.size() || !s_ed.tiles[k].box) return;
  EdTile& t = s_ed.tiles[k];
  const lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) {
    s_ed.k = k;
    s_ed.sizing = sizing;
    s_ed.moved = false;
    s_ed.ok = false;
    s_ed.p0 = p;
    s_ed.bx0 = lv_obj_get_x(t.box);
    s_ed.by0 = lv_obj_get_y(t.box);
    return;
  }
  if (s_ed.k != k) return;
  if (code == LV_EVENT_PRESSING) {
    const int dx = p.x - s_ed.p0.x, dy = p.y - s_ed.p0.y;
    if (!s_ed.moved && abs(dx) + abs(dy) < ED_SLOP) return;
    if (!s_ed.moved) {
      s_ed.moved = true;
      // A moved tile floats over the target frame, a resized one under it.
      lv_obj_move_foreground(sizing ? s_ed.ghost : t.box);
      lv_obj_set_style_opa(t.box, LV_OPA_80, 0);
    }
    const float sx = s_ed.cw + ED_GAP, sy = s_ed.ch + ED_GAP;
    Cell c = t.c;
    if (sizing) {
      c.w = constrain((int)lroundf((p.x - ED_PAD) / sx) - c.x, 1, min(6, s_ed.cols - c.x));
      c.h = constrain((int)lroundf((p.y - ED_BAR_H) / sy) - c.y, 1, min(6, s_ed.rows - c.y));
    } else {
      lv_obj_set_pos(t.box, s_ed.bx0 + dx, s_ed.by0 + dy);
      c.x = constrain((int)lroundf((s_ed.bx0 + dx - ED_PAD) / sx), 0, s_ed.cols - c.w);
      c.y = constrain((int)lroundf((s_ed.by0 + dy - ED_BAR_H) / sy), 0, s_ed.rows - c.h);
    }
    s_ed.to = c;
    s_ed.ok = edCheck(k, c, sizing, &s_ed.swap);
    const lv_color_t col = lv_color_hex(s_ed.ok ? C_ED_OK : C_ED_BAD);
    lv_obj_set_style_border_color(s_ed.ghost, col, 0);
    lv_obj_set_style_bg_color(s_ed.ghost, col, 0);
    edPlace(s_ed.ghost, c);
    lv_obj_remove_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // RELEASED or PRESS_LOST
  if (s_ed.moved && s_ed.ok && code == LV_EVENT_RELEASED) edApply(k, s_ed.to, sizing);
  s_ed.k = -1;
  lv_obj_add_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(t.box, LV_OPA_COVER, 0);
  edLayout();
}

void edRemove(int k) {
  if (k < 0 || k >= (int)s_ed.tiles.size() || !s_ed.tiles[k].box) return;
  s_ed.tiles[k].removed = true;
  // Hidden, not deleted: this runs in an event of the box's own child.
  lv_obj_add_flag(s_ed.tiles[k].box, LV_OBJ_FLAG_HIDDEN);
  s_ed.dirty = true;
}

void edRemoveEvent(lv_event_t* e) { edRemove((int)(uintptr_t)lv_event_get_user_data(e)); }
void edDoneEvent(lv_event_t*) { s_ed.pending = EditState::P_SAVE; }
void edCancelEvent(lv_event_t*) { s_ed.pending = EditState::P_CANCEL; }

// The name the tile shows (HA friendly name, metric name), else from the config.
String edTileName(const TileDef& d) {
  for (const TileRt& r : s_tiles)
    if (r.def == &d && r.name && *lv_label_get_text(r.name)) return lv_label_get_text(r.name);
  if (d.type != TILE_METRIC || d.label.length()) return tileLabel(d);
  const PcMetric* m = pcMetricFindById(d.metric);
  return m ? String(m->name) : "PC metric " + String(d.metric);
}

void edMakeBox(int k, const TileDef& d) {
  const uint32_t accent = d.type == TILE_METRIC ? metricAccent(d)
                          : d.hasColor          ? d.color
                                                : domainColor(entityDomain(d.entity));
  lv_obj_t* b = lv_obj_create(s_ed.root);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
  lv_obj_set_style_pad_all(b, 8, 0);
  s_ed.tiles[k].box = b;
  edPlace(b, s_ed.tiles[k].c);

  lv_obj_t* name = lv_label_create(b);
  lv_label_set_text(name, edTileName(d).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  // Full width, mid height: below the x and above the grip.
  lv_obj_set_width(name, lv_pct(100));
  lv_obj_set_style_max_height(name, 2 * lv_font_get_line_height(&deck_font_14), 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 6);

  lv_obj_t* x = lv_obj_create(b);
  lv_obj_remove_style_all(x);
  lv_obj_set_size(x, 30, 30);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 8, -8);
  lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x3A2226), 0);
  lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(x, edRemoveEvent, LV_EVENT_CLICKED, (void*)(uintptr_t)k);
  lv_obj_t* xl = lv_label_create(x);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(xl, lv_color_hex(C_ED_BAD), 0);
  lv_obj_center(xl);

  // Resize grip: an L-shaped corner mark with a finger-sized hit area.
  lv_obj_t* g = lv_obj_create(b);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, 34, 34);
  lv_obj_align(g, LV_ALIGN_BOTTOM_RIGHT, 8, 8);
  lv_obj_add_flag(g, LV_OBJ_FLAG_PRESS_LOCK);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* bar = lv_obj_create(g);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, i ? 3 : 14, i ? 14 : 3);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CLOCK), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, -7, -7);
  }
  // Not LV_EVENT_ALL: the handler reads the touch point, which draw events lack.
  for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(b, edDragEvent, c, (void*)(uintptr_t)k);
    lv_obj_add_event_cb(g, edDragEvent, c, (void*)(uintptr_t)(k | 0x100));
  }
}

void edOpen(int page) {
  if (s_ed.root || page < 0 || page >= s_count || !s_pages[page].def) return;
  hidePanelNow();
  closeModal();
  const PageDef& def = *s_pages[page].def;
  const int w = logicalW(), h = logicalH();
  s_ed = EditState();
  s_ed.cfgPage = (int)(&def - &s_cfg.pages[0]);
  s_ed.name = def.name.length() ? def.name : def.id;
  s_ed.orient = w > h ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
  s_ed.cols = def.cols[s_ed.orient];
  s_ed.rows = def.rows[s_ed.orient];
  s_ed.cw = (w - 2 * ED_PAD - (s_ed.cols - 1) * ED_GAP) / s_ed.cols;
  s_ed.ch = (h - ED_BAR_H - ED_PAD - (s_ed.rows - 1) * ED_GAP) / s_ed.rows;

  lv_obj_t* root = lv_obj_create(lv_layer_top());
  s_ed.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, w, h);
  lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(root, lv_color_hex(C_TEXT), 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // taps never reach the page

  constexpr int BW = 84;
  s_ed.title = lv_label_create(root);
  lv_obj_set_style_text_font(s_ed.title, &deck_font_20, 0);
  lv_label_set_long_mode(s_ed.title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s_ed.title, w - 2 * BW - 40);
  lv_obj_set_pos(s_ed.title, 12, 13);
  lv_obj_t* cancel = qpButton(root, w - 2 * BW - 16, 4, BW, "Cancel", false);
  lv_obj_add_event_cb(cancel, edCancelEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* done = qpButton(root, w - BW - 8, 4, BW, LV_SYMBOL_OK " Done", false);
  lv_obj_set_style_bg_color(done, lv_color_hex(C_OK), 0);
  lv_obj_set_style_text_color(done, lv_color_hex(C_TEXT_ON), 0);
  lv_obj_add_event_cb(done, edDoneEvent, LV_EVENT_CLICKED, nullptr);

  for (int y = 0; y < s_ed.rows; y++)
    for (int x = 0; x < s_ed.cols; x++) {
      lv_obj_t* o = lv_obj_create(root);
      lv_obj_remove_style_all(o);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_radius(o, 12, 0);
      lv_obj_set_style_border_width(o, 1, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(C_CHIP), 0);
      edPlace(o, Cell{true, x, y, 1, 1});
    }

  std::vector<Cell> cells;
  placeTiles(def, s_ed.orient, cells);
  int hidden = 0;
  for (const Cell& c : cells) {
    s_ed.tiles.push_back({c, false, nullptr});
    if (!c.placed) hidden++;
  }
  for (size_t i = 0; i < cells.size(); i++)
    if (cells[i].placed) edMakeBox((int)i, def.tiles[i]);

  s_ed.ghost = lv_obj_create(root);
  lv_obj_remove_style_all(s_ed.ghost);
  lv_obj_remove_flag(s_ed.ghost, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(s_ed.ghost, 12, 0);
  lv_obj_set_style_border_width(s_ed.ghost, 2, 0);
  lv_obj_set_style_bg_opa(s_ed.ghost, LV_OPA_20, 0);
  lv_obj_add_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);

  String t = "Edit " + s_ed.name;
  if (hidden) t += " (" + String(hidden) + " not shown)";
  lv_label_set_text(s_ed.title, t.c_str());

  setModalOpen(true);  // no swipes or quick panel under the editor
  panelSetPager(1, 0, 1, 0);  // and no page dots over it
  Serial.printf("Deck: edit page '%s'\n", def.id.c_str());
}

void edClose() {
  if (!s_ed.root) return;
  lv_obj_delete(s_ed.root);
  s_ed = EditState();
  setModalOpen(false);
  if (s_count) setPager(sh.active);
}

// The stored deck.json with this page's positions (pinned for every placed
// tile, in the current orientation) and removals. Tiles match by index: the
// parser keeps every tile of a page in file order. Other fields stay as they are.
bool edBuild(String& out, String& err) {
  String text;
  deckConfigText(text);
  JsonDocument doc;
  if (deserializeJson(doc, text)) {
    err = "stored deck.json unreadable";
    return false;
  }
  JsonObject pv;
  int n = 0;
  for (JsonObject p : doc["pages"].as<JsonArray>()) {
    if (!strcmp(p["type"] | "tiles", "metrics")) continue;  // skipped by the parser
    if (n++ == s_ed.cfgPage) {
      pv = p;
      break;
    }
  }
  JsonArray tiles = pv["tiles"].as<JsonArray>();
  if (pv.isNull() || tiles.size() != s_ed.tiles.size()) {
    err = "page changed meanwhile";
    return false;
  }
  const char* key = s_ed.orient == ORIENT_LANDSCAPE ? "landscape" : "portrait";
  for (size_t i = 0; i < tiles.size(); i++) {
    const EdTile& t = s_ed.tiles[i];
    if (t.removed) continue;
    JsonObject tv = tiles[i];
    if (!t.c.placed) {
      // A hidden tile loses its old cell, or it could claim space before a pinned one.
      JsonObject old = tv["pos"][key].as<JsonObject>();
      if (!old.isNull()) {
        old.remove("x");
        old.remove("y");
      }
      continue;
    }
    JsonObject pos = tv["pos"].is<JsonObject>() ? tv["pos"].as<JsonObject>() : tv["pos"].to<JsonObject>();
    JsonObject q = pos[key].to<JsonObject>();
    q["x"] = t.c.x;
    q["y"] = t.c.y;
    if (t.c.w > 1) q["w"] = t.c.w;
    if (t.c.h > 1) q["h"] = t.c.h;
  }
  for (size_t i = tiles.size(); i-- > 0;)
    if (s_ed.tiles[i].removed) tiles.remove(i);
  out = "";
  serializeJson(doc, out);
  return true;
}

// From deckTick, outside the LVGL event that asked for it: applying the
// config rebuilds the pages, and the editor goes with them (destroyPages).
// A failed save keeps the editor and its changes.
void edFinish() {
  const bool save = s_ed.pending == EditState::P_SAVE;
  s_ed.pending = EditState::P_NONE;
  if (!save || !s_ed.dirty) {
    edClose();
    return;
  }
  String text, err;
  if (edBuild(text, err) && deckApplyConfig(text, err)) {
    deckToastShow("Page saved", 1500);
    return;
  }
  Serial.printf("Deck: edit not saved: %s\n", err.c_str());
  deckToastShow("Not saved", 1800);
}

void buildPages() {
  const int w = logicalW();
  s_count = 0;
  s_tiles.clear();
  s_tiles.reserve(64);
  for (const PageDef& pd : s_cfg.pages) {
    if (s_count >= MAX_PAGES) break;
    addTilesPage(pd);
  }

  portENTER_CRITICAL(&s_mux);
  sh.count = s_count;
  sh.owned = false;
  for (int i = 0; i < MAX_PAGES; i++) { sh.valid[i] = false; sh.busy[i] = false; }
  const int active = sh.active < s_count ? sh.active : 0;
  sh.active = active;
  sh.nCols = 0;
  for (int i = 0; i < s_count; i++) {
    const String& g = s_pages[i].def->group;
    const bool joins = i > 0 && g.length() && g == s_pages[i - 1].def->group;
    if (!joins) {
      sh.colFirst[sh.nCols] = i;
      sh.colLen[sh.nCols] = 0;
      sh.colCur[sh.nCols] = i;
      sh.nCols++;
    }
    sh.col[i] = sh.nCols - 1;
    sh.colLen[sh.nCols - 1]++;
  }
  sh.colCur[sh.col[active]] = active;
  portEXIT_CRITICAL(&s_mux);

  setPager(active);
  // Inactive pages wait just off the right edge: they keep their layout and
  // can be snapshotted, but their invalidations are clipped away.
  for (int i = 0; i < s_count; i++) lv_obj_set_pos(s_pages[i].root, i == active ? 0 : w, 0);
  s_lastMinute = -1;
  s_lastWifi = -1;
  buildQuickPanel();
  collectEntities();
}

void setPager(int page) {
  const int c = sh.col[page];
  panelSetPager(sh.nCols, c, sh.colLen[c], page - sh.colFirst[c]);
}

void destroyPages() {
  edClose();
  closeModal();
  destroyQuickPanel();
  for (int i = 0; i < s_count; i++) {
    if (s_pages[i].root) lv_obj_delete(s_pages[i].root);
    if (s_pages[i].snap) lv_draw_buf_destroy(s_pages[i].snap);
    s_pages[i] = {};
  }
  s_count = 0;
  for (TileRt& r : s_tiles)
    if (r.pts) lv_free(r.pts);
  s_tiles.clear();
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
  sh.colCur[sh.col[page]] = page;
  portEXIT_CRITICAL(&s_mux);
  setPager(page);
  s_gen++;
  // New neighbours; cheap to re-take, and the page we left changed state.
  for (int i = 0; i < s_count; i++) s_pages[i].stale = true;
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
  // The pages one gesture away first: next / previous column, then the
  // pages above and below in this column.
  const int a = sh.active;
  const int c = sh.col[a];
  int pick = -1;
  const int order[4] = {c + 1 < sh.nCols ? sh.colCur[c + 1] : -1, c > 0 ? sh.colCur[c - 1] : -1,
                        a + 1 < s_count && sh.col[a + 1] == c ? a + 1 : -1,
                        a > 0 && sh.col[a - 1] == c ? a - 1 : -1};
  for (int k = 0; k < 4 && pick < 0; k++) {
    const int p = order[k];
    if (p >= 0 && s_pages[p].stale) pick = p;
  }
  for (int p = 0; p < s_count && pick < 0; p++)
    if (s_pages[p].stale) pick = p;
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
  char buf[12];
  if (netSettings.use24h) snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
  else snprintf(buf, sizeof(buf), "%d:%02d %s", (lt.tm_hour + 11) % 12 + 1, lt.tm_min,
                lt.tm_hour < 12 ? "AM" : "PM");
  for (int i = 0; i < s_count; i++) {
    if (!s_pages[i].clock) continue;
    lv_label_set_text(s_pages[i].clock, buf);
    s_pages[i].stale = true;
  }
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
bool deckBegin(uint8_t rotation) {
  if (!panelBegin(rotation)) return false;

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

  if (!iconsLoad(iconsPath(), err)) Serial.printf("Deck: icons: %s\n", err.c_str());
  {
    Preferences prefs;
    if (prefs.begin("deckui", true)) {
      s_header = prefs.getBool("hdr", true);
      prefs.end();
    }
  }
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
        case Shared::GEST_PAGE: switchTo(sh.target); break;
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

  // System screens (splash, WiFi setup, OTA) cover the pages: no gestures,
  // no panel or card under them.
  const ScreenState sc = getScreenState();
  const bool systemScreen = sc != SCREEN_DECK;
  if (systemScreen && (sh.swipeEnabled || s_qp.open || s_modalCard || s_ed.root)) {
    compCancel();
    releaseSwipe();
    hidePanelNow();
    closeModal();
    edClose();
  }
  portENTER_CRITICAL(&s_mux);
  sh.swipeEnabled = !systemScreen;
  portEXIT_CRITICAL(&s_mux);
  sysScreenSync(sc);

  if (compOwnsScreen()) return;  // LVGL stays frozen during a drag or slide

  if (s_ed.pending != EditState::P_NONE) edFinish();
  updateHeaders();
  qpUpdateStatus(false);
  haSync();
  pcSync(false);
  lv_timer_handler();
  if (!systemScreen) refreshOneSnapshot();
  logStats();
}

void deckToast(const char* text) {
  if (s_disp) deckToastShow(text, 1800);
}

void deckRefreshNow() {
  if (s_disp && !compOwnsScreen()) lv_timer_handler();
}

void deckApplyRotation(uint8_t rotation) {
  if (!s_disp) return;
  rotation &= 3;
  if (rotation == panelRotation()) return;
  panelQuiesce();
  releaseSwipe();
  destroyPages();
  panelSetRotation(rotation);
  lv_display_set_resolution(s_disp, logicalW(), logicalH());
  lv_display_set_buffers(s_disp, panelLvBuffer(0), panelLvBuffer(1), FRAME_BYTES,
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  buildPages();
  sysScreenRelayout();
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

namespace {
void setHeader(bool on) {
  if (on == s_header) return;
  s_header = on;
  Preferences prefs;
  if (prefs.begin("deckui", false)) {
    prefs.putBool("hdr", on);
    prefs.end();
  }
  for (int i = 0; i < s_count; i++) layoutHeader(s_pages[i]);
  if (s_qp.headerBtn) {
    if (on) lv_obj_add_state(s_qp.headerBtn, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_qp.headerBtn, LV_STATE_CHECKED);
    qpMarkStale();
  }
}
}  // namespace

void deckSetHeader(bool on) {
  if (s_disp) setHeader(on);
}

bool deckHeaderVisible() { return s_header; }

bool deckInstallIcons(const char* uploaded, String& err) {
  if (!s_disp) {
    err = "display not running";
    return false;
  }
  // The pages hold the current fonts: tear them down before the swap.
  panelQuiesce();
  releaseSwipe();
  destroyPages();
  const bool ok = iconsLoad(uploaded, err);
  if (ok) {
    LittleFS.remove(iconsPath());
    if (!LittleFS.rename(uploaded, iconsPath())) err = "font loaded but not stored";
  } else {
    LittleFS.remove(uploaded);
  }
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
  return ok && !err.length();
}

bool deckTestCard(const String& tileId) {
  if (s_ed.root) return false;  // the editor blocks gestures until it closes
  for (size_t i = 0; i < s_tiles.size(); i++) {
    if (s_tiles[i].def->id != tileId) continue;
    openModal((int)i);
    return true;
  }
  return false;
}

bool deckTestEdit(const String& op, int tile, int x, int y, int w, int h) {
  if (!s_disp) return false;
  if (op == "open") {
    edOpen(sh.active);
    return s_ed.root != nullptr;
  }
  if (!s_ed.root) return false;
  if (op == "done" || op == "cancel") {
    s_ed.pending = op == "done" ? EditState::P_SAVE : EditState::P_CANCEL;
    return true;
  }
  if (tile < 0 || tile >= (int)s_ed.tiles.size() || !s_ed.tiles[tile].box || s_ed.tiles[tile].removed)
    return false;
  if (op == "remove") {
    edRemove(tile);
    return true;
  }
  if (op != "move") return false;
  const Cell& cur = s_ed.tiles[tile].c;
  const Cell c = {true, x, y, w > 0 ? w : cur.w, h > 0 ? h : cur.h};
  const bool resize = c.w != cur.w || c.h != cur.h;
  if (!edApply(tile, c, resize)) return false;
  edLayout();
  return true;
}

bool deckTestSwipe(int dir, bool vertical) {
  if (!s_disp || (dir != 1 && dir != -1)) return false;
  return slideBy(dir, vertical);
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

bool uiTryBeginSwipe(int dir, bool vertical, Src& a, Src& b, int& startOff, int& idxA) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  const int cur = sh.active;
  const int c = cur < sh.count ? sh.col[cur] : 0;
  int target = -1, idxCur = 0, idxTarget = 0;
  if (vertical) {
    target = cur + dir;
    if (target < 0 || target >= sh.count || sh.col[target] != c) target = -1;
    idxCur = cur - sh.colFirst[c];
    idxTarget = idxCur + dir;
  } else if (c + dir >= 0 && c + dir < sh.nCols) {
    target = sh.colCur[c + dir];
    idxCur = c;
    idxTarget = c + dir;
  }
  if (sh.swipeEnabled && !sh.owned && !sh.panelOpen && !sh.modalOpen &&
      target >= 0 && sh.valid[target] && !sh.busy[target]) {
    sh.owned = true;
    sh.gesture = Shared::GEST_PAGE;
    sh.target = target;
    const Src live = {nullptr, 0, true};  // the live LVGL frame
    if (dir > 0) {
      a = live;
      b = sh.src[target];
      startOff = 0;
      idxA = idxCur;
    } else {
      a = sh.src[target];
      b = live;
      startOff = vertical ? logicalH() : logicalW();
      idxA = idxTarget;
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
    } else if (sh.valid[a] && !sh.busy[a]) {
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
