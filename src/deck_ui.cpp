// Guition JC3248W535 deck UI. See deck_ui.h for the threading contract.
#include "deck_ui.h"
#include "deck_config.h"
#include "deck_bambu.h"
#include "deck_ha.h"
#include "deck_http.h"
#include "deck_icons.h"
#include "deck_printers.h"
#include "deck_sys.h"
#include "deck_touch.h"
#include "deck_wall.h"
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
#include <algorithm>
#include <new>
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
constexpr uint32_t C_WARN = 0xE5A50A;
constexpr uint32_t C_CRIT = 0xE5484D;

// Theme colours (D11): Midnight (mockup A, default), Daylight, or Glass
// (Midnight text, tiles of tinted glass over a wallpaper), set by
// setPalette() before the pages are built.
uint8_t s_theme = 0;
uint32_t C_BG, C_TILE, C_TILE_PRESSED, C_TILE_LINE, C_CHIP, C_TEXT, C_TEXT_ON, C_TITLE,
    C_CLOCK, C_OK, C_DIM, C_PANEL, C_TRACK, C_BUTTON;

void setPalette(uint8_t theme) {
  s_theme = theme <= THEME_GLASS ? theme : THEME_MIDNIGHT;
  const bool day = s_theme == THEME_DAYLIGHT, glass = s_theme == THEME_GLASS;
  C_BG = day ? 0xECEFF3 : 0x0D0F13;
  C_TILE = day ? 0xFFFFFF : glass ? 0x0B0E13 : 0x1A1D23;  // glass: the tint
  C_TILE_PRESSED = day ? 0xDFE3E9 : glass ? 0x000000 : 0x2A2F38;
  C_TILE_LINE = day ? 0xD9DDE4 : glass ? 0xFFFFFF : 0x1A1D23;
  C_CHIP = day ? 0xE9ECF1 : 0x272B33;
  C_TEXT = day ? 0x1D2129 : 0xE6E8EC;
  C_TEXT_ON = 0x221602;
  C_TITLE = day ? 0x161A20 : 0xF1F2F4;
  C_CLOCK = day ? 0x4A5160 : 0xC9CDD6;
  C_OK = day ? 0x23966F : 0x5CC9A7;
  C_DIM = day ? 0x8D95A3 : 0x596070;
  C_PANEL = day ? 0xF8F9FB : 0x161A20;
  C_TRACK = day ? 0xD9DEE5 : 0x2E333C;
  C_BUTTON = day ? 0xDFE3E9 : 0x272B33;
}

// An icon in its round chip: on Daylight a light tint of the accent behind a
// darker accent, since pale accents (amber, yellow) vanish on white.
void styleIconChip(lv_obj_t* chip, uint32_t accent) {
  const lv_color_t a = lv_color_hex(accent);
  const bool day = s_theme == THEME_DAYLIGHT;
  lv_obj_set_style_bg_color(chip, day ? lv_color_mix(a, lv_color_white(), 60) : lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_text_color(chip, day ? lv_color_mix(a, lv_color_black(), 165) : a, 0);
}

struct Page {
  lv_obj_t* root;
  lv_obj_t* title;
  lv_obj_t* clock;
  lv_obj_t* wifi;
  lv_obj_t* grid;
  lv_draw_buf_t* snap;
  lv_draw_buf_t* glass;  // Glass theme: the baked background
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
  lv_obj_t* chip;
  int iconPx;
  uint32_t iconCp;        // MDI codepoint shown, 0 = LVGL symbol
  bool on;
  uint32_t haVersion;     // last HA entity version applied
  // Metric and sensor charts
  lv_obj_t* unit;
  lv_obj_t* viz;          // chart or bar
  lv_chart_series_t* ser;
  int32_t* pts;           // chart points, LVGL heap
  bool warn;
  uint8_t threshold;
  uint32_t historyVersion;
  const lv_font_t* valFont;  // metric value font picked by fitTile, shrinks per value
  int16_t valW;              // room for value + unit
  bool besideName;           // chart: the value shares the top line with the name
  int16_t chipRoom;          // sensor: icon width the reading may take over
  const lv_font_t* nameFont;  // largest name font, null = fixed one-line name
  int16_t nameArea, nameW;    // room for the name
  int16_t fitW, fitH;        // tile size the layout was fitted to
  // Slider tiles: the fill behind the content shows the level (0..1).
  lv_obj_t* fill;
  float frac;
  bool sliding;           // this press became a drag: no tap or hold action
  int16_t slX0;
  float slFrac0, slValue;
  lv_obj_t* detail;       // HTTP sensor: the "detail" line
  lv_obj_t* px[4];        // printer tiles: extra labels, swatches
  // Clock tiles: hours in sub, then these two; the colon blinks.
  lv_obj_t* colon;
  lv_obj_t* mins;
  bool colonLit;
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
  // Slider tiles of the shown page: a touch starting on one is a slider drag,
  // never a page swipe.
  struct Zone {
    int16_t x1, y1, x2, y2;
  } zones[MAX_PAGE_TILES] = {};
  int nZones = 0;
};
Shared sh;
int s_zonesPage = -1;     // page the zones were taken from
bool s_zonesDirty = true;  // tiles rebuilt or moved: take them again

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
  if (t.type == TILE_CLOCK) return "Clock";
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
void openConfirm(int tileIdx, bool hold);
const char* pcCmdLabel(const String& cmd);
void pairSync();
void refreshModal();
void deckToastShow(const char* text, uint32_t holdMs);
void qpSyncValues();
void fitValue(TileRt& r);
void printerTap(TileRt& r);
void fitNameText(TileRt& r);
uint32_t tileText(const TileDef& d);
void showObj(lv_obj_t* o, bool on);

uint32_t sensorColour(const TileRt& r) {
  return r.threshold == 2 ? C_CRIT : r.threshold == 1 ? C_WARN : tileText(*r.def);
}

uint32_t sensorLine(const TileRt& r) {
  const TileDef& d = *r.def;
  return r.threshold ? sensorColour(r) : d.hasChartColor ? d.chartColor :
         d.hasColor ? d.color : domainColor(entityDomain(d.entity));
}

void sensorThreshold(TileRt& r, bool numeric, float value) {
  const TileDef& d = *r.def;
  if (!numeric) r.threshold = 0;
  else {
    const bool low = d.hasWarn && d.hasCrit && d.crit < d.warn;
    auto active = [&](float threshold, bool held) {
      const float margin = held ? max(0.01f, fabsf(threshold) * 0.01f) : 0;
      return low ? (held ? value < threshold + margin : value <= threshold) :
                   (held ? value > threshold - margin : value >= threshold);
    };
    if (d.hasCrit && active(d.crit, r.threshold == 2)) r.threshold = 2;
    else if (d.hasWarn && active(d.warn, r.threshold >= 1)) r.threshold = 1;
    else r.threshold = 0;
  }
  lv_obj_set_style_text_color(r.sub, lv_color_hex(sensorColour(r)), 0);
  if (r.viz && r.ser) lv_chart_set_series_color(r.viz, r.ser, lv_color_hex(sensorLine(r)));
}

// Leave room for axis padding and the LVGL missing-point sentinel.
int32_t historyPoint(float value) {
  return (int32_t)lround(constrain((double)value * 10, -1000000000.0, 1000000000.0));
}

void plotHistory(lv_obj_t* chart, int32_t* points, const float* values, int count) {
  // Scale to the plotted readings, with padding above and below.
  int32_t low = INT32_MAX, high = INT32_MIN;
  for (int i = 0; i < count; i++) {
    points[i] = isfinite(values[i]) ? historyPoint(values[i]) : LV_CHART_POINT_NONE;
    if (points[i] == LV_CHART_POINT_NONE) continue;
    low = min(low, points[i]);
    high = max(high, points[i]);
  }
  if (low <= high) {
    const int32_t pad = (int32_t)((int64_t)(high - low) * 15 / 100) + 5;
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, low - pad, high + pad);
  }
  lv_chart_refresh(chart);
}

bool copyHistoryChart(const TileDef& d, uint8_t hours, uint32_t& version, lv_obj_t* chart,
                      int32_t* points, float& lo, float& hi) {
  float values[HA_HISTORY_POINTS];
  if (!haCopyHistory(d.entity, hours, version, values, lo, hi)) return false;
  plotHistory(chart, points, values, HA_HISTORY_POINTS);
  return true;
}

void applySensorHistory(TileRt& r) {
  if (r.def->type != TILE_SENSOR || r.def->http.enabled || !r.viz || !r.pts) return;
  float lo, hi;
  if (copyHistoryChart(*r.def, r.def->hours, r.historyVersion, r.viz, r.pts, lo, hi))
    markPageStale(r.page);
}

// --- Home Assistant binding ------------------------------------------------------------
constexpr uint32_t TOGGLE_SETTLE_MS = 2000;
constexpr uint32_t LEVEL_SETTLE_MS = 5000;
constexpr uint32_t CALL_ANSWER_MS = 6000;
constexpr uint32_t LEVEL_GAP_MS = 300;

bool haStateOn(const String& domain, const String& st) {
  if (domain == "lock") return st == "locked";
  if (domain == "cover") return st == "open" || st == "opening";
  if (domain == "media_player") return st == "playing";
  if (domain == "climate") return st != "off" && st.length();
  return st == "on" || st == "home";
}

// A reading with at most one decimal.
void formatReading(float v, char* out, size_t n) {
  snprintf(out, n, fabsf(v - roundf(v)) < 0.05f ? "%.0f" : "%.1f", v);
}

// A number in s / min / h / d as "45s", "31m", "1h 49m", "3d 2h"; false for
// other units.
bool formatDuration(float v, const String& unit, char* out, size_t n) {
  const double scale = unit == "ms" ? 0.001 : unit == "s" ? 1 : unit == "min" ? 60 :
                       unit == "h" ? 3600 : unit == "d" ? 86400 : 0;
  if (!scale || !isfinite(v)) return false;
  const double secs = fmin(fabs(v * scale), 4e9);
  const char* sign = v < 0 ? "-" : "";
  const uint32_t t = (uint32_t)secs, d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
  if (t < 60 && scale < 60) snprintf(out, n, "%s%lus", sign, (unsigned long)t);
  else if (t < 3600) snprintf(out, n, "%s%lum", sign, (unsigned long)m);
  else if (t < 86400) snprintf(out, n, m ? "%s%luh %lum" : "%s%luh", sign, (unsigned long)h, (unsigned long)m);
  else snprintf(out, n, h ? "%s%lud %luh" : "%s%lud", sign, (unsigned long)d, (unsigned long)h);
  return true;
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
  else if (domain == "cover") svc = currentlyOn ? "close_cover" : "open_cover";
  else if (domain == "media_player") svc = currentlyOn ? "media_pause" : "media_play";
  else if (domain == "light" || domain == "switch" || domain == "fan" ||
           domain == "input_boolean" || domain == "automation" || domain == "siren" ||
           domain == "humidifier") svc = currentlyOn ? "turn_off" : "turn_on";
  else if (domain == "scene" || domain == "script") svc = "turn_on";
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

// --- Slider tiles (also the detail card's slider) ---------------------------------------
bool sliderDomain(const String& d) {
  return d == "light" || d == "cover" || d == "fan" || d == "media_player" || d == "input_number" ||
         d == "number";
}

bool parseNum(const String& s, float& v) {
  char* end = nullptr;
  v = strtof(s.c_str(), &end);
  return s.length() && end && *end == 0;
}

// The entity's level as 0..1, or -1 when it has none (unavailable, a cover
// without a position).
float sliderFrac(const String& d, const HaEntity& e) {
  if (!e.known || e.state == "unavailable" || e.state == "unknown") return -1;
  if (d == "input_number" || d == "number") {
    float v;
    const float lo = isnan(e.numMin) ? 0 : e.numMin, hi = isnan(e.numMax) ? 100 : e.numMax;
    if (!parseNum(e.state, v) || hi <= lo) return -1;
    return constrain((v - lo) / (hi - lo), 0.0f, 1.0f);
  }
  if (d == "light") return e.state != "on" ? 0 : e.brightness >= 0 ? e.brightness / 255.0f : 1;
  if (e.level >= 0) return constrain(e.level / 100.0f, 0.0f, 1.0f);
  return (d == "fan" && e.state == "off") ? 0 : -1;
}

// What the tile shows for a level: "62%", or a number's value and unit.
String sliderText(const String& d, const HaEntity& e, float frac) {
  if (frac < 0) return prettyState(d, e);
  if (d == "input_number" || d == "number") {
    const float lo = isnan(e.numMin) ? 0 : e.numMin, hi = isnan(e.numMax) ? 100 : e.numMax;
    char num[24];
    snprintf(num, sizeof num, "%.4g", lo + (hi - lo) * frac);
    return e.unit.length() ? String(num) + " " + e.unit : String(num);
  }
  if ((d == "light" || d == "fan") && frac <= 0) return "Off";
  return String((int)lroundf(frac * 100)) + "%";
}

// Set the entity to a level, with a result slot for the shared wish.
uint32_t sliderSend(const String& entity, float frac) {
  const String d = entityDomain(entity);
  const int pct = (int)lroundf(constrain(frac, 0.0f, 1.0f) * 100);
  if (d == "light")
    return pct ? haCallServiceTracked("light", "turn_on", entity, "{\"brightness_pct\":" + String(pct) + "}")
               : haCallServiceTracked("light", "turn_off", entity, "");
  if (d == "cover")
    return haCallServiceTracked("cover", "set_cover_position", entity, "{\"position\":" + String(pct) + "}");
  if (d == "fan")
    return pct ? haCallServiceTracked("fan", "set_percentage", entity, "{\"percentage\":" + String(pct) + "}")
               : haCallServiceTracked("fan", "turn_off", entity, "");
  if (d == "media_player")
    return haCallServiceTracked("media_player", "volume_set", entity,
                                "{\"volume_level\":" + String(pct / 100.0f, 2) + "}");
  if (d == "input_number" || d == "number") {
    HaEntity e;
    haGetEntity(entity, e);
    const float lo = isnan(e.numMin) ? 0 : e.numMin, hi = isnan(e.numMax) ? 100 : e.numMax;
    if (hi <= lo) return 0;
    float val = lo + (hi - lo) * frac;
    if (e.numStep > 0) {
      val = lo + lroundf((val - lo) / e.numStep) * e.numStep;
      if (val > hi) val -= e.numStep;  // HA refuses values past max
    }
    char num[24];
    snprintf(num, sizeof num, "%.7g", constrain(val, lo, hi));
    return haCallServiceTracked(d, "set_value", entity, String("{\"value\":") + num + "}");
  }
  return 0;
}

struct WishField {
  bool active = false, dirty = false;
  float value = 0;
  uint32_t order = 0, version = 0;
};
struct Wish {
  String entity;
  WishField on, level;
  uint32_t call = 0, sentMs = 0, levelMs = 0;
  uint32_t resultMs = 0, resultVersion = 0;
  float sentValue = 0, lastLevel = -1;
  bool sentLevel = false, levelSent = false, dragging = false;
  bool answered = false, timedOut = false, holding = false;
};
std::vector<Wish> s_wishes;
uint32_t s_wishOrder = 0;

Wish* findWish(const String& entity) {
  for (Wish& w : s_wishes) if (w.entity == entity) return &w;
  return nullptr;
}

void rememberLevel(Wish& w, const HaEntity& e) {
  const float f = sliderFrac(entityDomain(w.entity), e);
  if (f > 0) w.lastLevel = f;
}

Wish& ensureWish(const String& entity) {
  if (Wish* w = findWish(entity)) return *w;
  s_wishes.emplace_back();
  Wish& w = s_wishes.back();
  w.entity = entity;
  HaEntity e;
  if (haGetEntity(entity, e)) rememberLevel(w, e);
  return w;
}

bool levelControlsOn(const String& domain) {
  return domain == "light" || domain == "fan" || domain == "cover";
}

bool wishOn(const String& entity, const HaEntity& e) {
  const String domain = entityDomain(entity);
  const Wish* w = findWish(entity);
  if (w && w->on.active) return w->on.value != 0;
  if (w && w->level.active && levelControlsOn(domain)) return w->level.value > 0;
  return e.known && e.state != "unavailable" && e.state != "unknown" && haStateOn(domain, e.state);
}

float wishFrac(const String& entity, const HaEntity& e) {
  const Wish* w = findWish(entity);
  if (w && w->on.active && (!w->level.active || (int32_t)(w->on.order - w->level.order) > 0))
    return w->on.value == 0 ? 0 : w->lastLevel > 0 ? w->lastLevel : 1;
  if (w && w->level.active) return w->level.value;
  return sliderFrac(entityDomain(entity), e);
}

String wishText(const String& entity, const HaEntity& e, bool slider) {
  const String domain = entityDomain(entity);
  const Wish* w = findWish(entity);
  if (slider) {
    if (w && w->on.active && !w->on.value &&
        (!w->level.active || (int32_t)(w->on.order - w->level.order) > 0)) return "Off";
    return sliderText(domain, e, wishFrac(entity, e));
  }
  if (w && w->on.active) {
    const bool on = w->on.value != 0;
    if (domain == "lock") return on ? "Locked" : "Unlocked";
    if (domain == "cover") return on ? "Open" : "Closed";
    if (domain == "media_player") return on ? "Playing" : "Paused";
    return on ? "On" : "Off";
  }
  if (w && w->level.active) return sliderText(domain, e, w->level.value);
  return prettyState(domain, e);
}

void applyHaToTile(TileRt& r, bool force);

void showWish(const String& entity) {
  for (TileRt& r : s_tiles) if (r.def->entity == entity) applyHaToTile(r, true);
  refreshModal();
}

void clearWish(Wish& w) {
  w.on = {};
  w.level = {};
  w.dragging = w.holding = false;
  w.answered = false;
}

bool sendWish(Wish& w) {
  if (w.call || (!w.on.dirty && !w.level.dirty)) return true;
  const bool level = w.level.dirty && (!w.on.dirty || (int32_t)(w.level.order - w.on.order) < 0);
  const uint32_t now = millis();
  if (level && w.dragging && w.levelSent && now - w.levelMs < LEVEL_GAP_MS) return true;
  WishField& f = level ? w.level : w.on;
  uint32_t call;
  if (level) call = sliderSend(w.entity, f.value);
  else {
    String domain, service;
    toggleService(entityDomain(w.entity), f.value == 0, domain, service);
    call = haCallServiceTracked(domain, service, w.entity, "");
  }
  if (!call) {
    clearWish(w);
    deckToastShow(haLink() == HA_CONNECTED ? "Home Assistant is busy, try again" :
                                          "Home Assistant is offline", 1800);
    return false;
  }
  w.call = call;
  w.sentLevel = level;
  w.sentValue = f.value;
  w.sentMs = now;
  w.timedOut = w.answered = false;
  f.dirty = false;
  if (level) { w.levelMs = now; w.levelSent = true; }
  return true;
}

bool setWish(const String& entity, bool level, float value, bool dragging = false) {
  Wish& w = ensureWish(entity);
  WishField& f = level ? w.level : w.on;
  if (level) w.dragging = dragging;
  if (!f.active || f.value != value || (level && w.holding)) {
    HaEntity e;
    haGetEntity(entity, e);
    f.version = e.version;
    f.active = true;
    f.value = value;
    f.order = ++s_wishOrder;
    if (w.call && w.timedOut) { w.sentMs = millis(); w.timedOut = false; }
    f.dirty = !w.call || w.sentLevel != level || w.sentValue != value;
    if (levelControlsOn(entityDomain(entity))) {
      if (level) w.on = {};
      else { w.level = {}; w.dragging = w.holding = false; }
    }
    if (level) w.holding = false;
    if (level && value > 0) w.lastLevel = value;
  }
  const bool ok = sendWish(w);
  showWish(entity);
  return ok;
}

void holdLevel(const String& entity, float value) {
  Wish& w = ensureWish(entity);
  w.dragging = true;
  if (!w.level.active) {
    w.level.active = w.holding = true;
    w.level.value = value;
    w.level.order = ++s_wishOrder;
  }
}

void releaseLevel(const String& entity) {
  if (Wish* w = findWish(entity)) {
    if (w->holding) { w->level = {}; w->holding = false; }
    w->dragging = false;
    sendWish(*w);
    showWish(entity);
  }
}

// Results gate both resends and settling. A missing result keeps the call owned.
void syncWishes() {
  const uint32_t now = millis();
  for (Wish& w : s_wishes) {
    if (!w.call && !w.on.active && !w.level.active) continue;
    bool changed = false;
    HaCallResult result;
    if (w.call && haTakeCallResult(w.call, result)) {
      w.call = 0;
      changed = true;
      if (!result.ok) clearWish(w);
      else {
        WishField& sent = w.sentLevel ? w.level : w.on;
        if (sent.active && sent.value == w.sentValue) sent.dirty = false;
        w.answered = true;
        w.resultMs = result.finishedMs;
        w.resultVersion = result.entityVersion;
      }
    }
    if (haLink() != HA_CONNECTED && (w.on.active || w.level.active)) {
      clearWish(w);
      changed = true;
    }
    if (w.call && !w.timedOut && now - w.sentMs >= CALL_ANSWER_MS) {
      clearWish(w);
      w.timedOut = true;
      changed = true;
      deckToastShow("No answer from Home Assistant", 2000);
    }
    if (!w.call && (w.on.dirty || w.level.dirty)) {
      sendWish(w);
      changed = changed || w.call || (!w.on.dirty && !w.level.dirty);
    }
    HaEntity e;
    if (haGetEntity(w.entity, e)) {
      if (w.on.active && !w.on.dirty && !w.level.dirty && e.version != w.on.version &&
          e.known && e.state != "unavailable" && e.state != "unknown" &&
          haStateOn(entityDomain(w.entity), e.state) == (w.on.value != 0)) {
        w.on.active = false;
        changed = true;
      }
      if (!w.call && !w.on.dirty && !w.level.dirty && w.answered && !w.dragging) {
        const bool newer = e.version != w.resultVersion;
        const bool matches = e.known && e.state != "unavailable" && e.state != "unknown" &&
                             haStateOn(entityDomain(w.entity), e.state) == (w.on.value != 0);
        if (w.on.active && (newer || matches ||
                            (int32_t)(now - w.resultMs) >= (int32_t)TOGGLE_SETTLE_MS)) {
          w.on.active = false;
          changed = true;
        }
        if (w.level.active && (newer || (int32_t)(now - w.resultMs) >= (int32_t)LEVEL_SETTLE_MS)) {
          w.level.active = false;
          changed = true;
        }
      }
    }
    if (changed) showWish(w.entity);
  }
  s_wishes.erase(std::remove_if(s_wishes.begin(), s_wishes.end(), [](const Wish& w) {
    if (w.call) return false;
    for (const TileRt& r : s_tiles) if (r.def->entity == w.entity) return false;
    return true;
  }), s_wishes.end());
}

void setSliderFill(TileRt& r, float frac) {
  r.frac = frac < 0 ? 0 : frac;
  const int w = lv_obj_get_width(r.obj), h = lv_obj_get_height(r.obj);
  if (w <= 0) return;  // not laid out yet: SIZE_CHANGED comes back here
  // The fill starts at the tile edge, outside the padding.
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  lv_obj_set_pos(r.fill, -pad, -pad);
  lv_obj_set_size(r.fill, (int)lroundf(w * r.frac), h);
  if (r.frac <= 0) lv_obj_add_flag(r.fill, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(r.fill, LV_OBJ_FLAG_HIDDEN);
}

// Pull the latest HA state into one tile's widgets.
void applyHaToTile(TileRt& r, bool force) {
  const TileDef& d = *r.def;
  if (d.http.enabled || !d.entity.length()) return;
  HaEntity e;
  if (!haGetEntity(d.entity, e)) return;
  if (!force && e.version == r.haVersion) return;
  if (e.version != r.haVersion)
    if (Wish* w = findWish(d.entity)) rememberLevel(*w, e);
  r.haVersion = e.version;
  const String domain = entityDomain(d.entity);
  const bool avail = e.known && e.state != "unavailable" && e.state != "unknown";
  const bool control = d.type == TILE_TOGGLE || d.type == TILE_SLIDER || d.tap.kind == ACT_TOGGLE;
  const bool on = control ? wishOn(d.entity, e) : avail && haStateOn(domain, e.state);
  if (control) r.on = on;
  if (d.type == TILE_TOGGLE) setTileOn(r, on);
  if (r.icon) r.iconCp = showIcon(r.icon, d, &e, on, r.iconPx, r.iconCp);
  lv_obj_set_style_opa(r.obj, avail ? LV_OPA_COVER : LV_OPA_50, 0);
  if (!d.label.length() && e.name.length() && strcmp(lv_label_get_text(r.name), e.name.c_str())) {
    lv_label_set_text(r.name, e.name.c_str());
    fitNameText(r);
  }
  if (d.type == TILE_SENSOR) {
    float value = 0;
    const bool numeric = e.known && haNumericState(e.state, value);
    sensorThreshold(r, numeric, value);
  }
  if (d.type == TILE_SENSOR && d.style == METRIC_CHART) {
    float value;
    const bool numeric = e.known && haNumericState(e.state, value);
    char text[32];
    const bool dur = numeric && d.duration && formatDuration(value, e.unit, text, sizeof(text));
    if (numeric && !dur) formatReading(value, text, sizeof(text));
    lv_label_set_text(r.sub, d.sub.length() ? d.sub.c_str() : numeric ? text : prettyState(domain, e).c_str());
    lv_label_set_text(r.unit, numeric && !dur && !d.sub.length() ? e.unit.c_str() : "");
    fitValue(r);
  } else if (d.type == TILE_SLIDER) {
    setSliderFill(r, wishFrac(d.entity, e));
    if (!d.sub.length()) lv_label_set_text(r.sub, wishText(d.entity, e, true).c_str());
  } else if (!d.sub.length()) {
    float value;
    char text[32];
    if (d.type == TILE_SENSOR && d.duration && e.known && haNumericState(e.state, value) &&
        formatDuration(value, e.unit, text, sizeof(text)))
      lv_label_set_text(r.sub, text);
    else lv_label_set_text(r.sub, (control ? wishText(d.entity, e, false) : prettyState(domain, e)).c_str());
    fitValue(r);
  }
  markPageStale(r.page);
}

uint32_t s_haSeen = 0;
uint32_t s_historySeen = 0;

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
  syncWishes();
  const uint32_t v = haStoreVersion();
  if (v != s_haSeen) {
    s_haSeen = v;
    for (TileRt& r : s_tiles) applyHaToTile(r, false);
    refreshModal();
    ledFromHa(false);
  }
  const uint32_t historyVersion = haHistoryVersion();
  if (historyVersion != s_historySeen) {
    s_historySeen = historyVersion;
    for (TileRt& r : s_tiles) applySensorHistory(r);
    refreshModal();
  }
  String ent, msg;
  while (haTakeFailure(ent, msg)) {
    if (ent == s_cfg.ledEntity) ledFromHa(true);  // back to what HA has
    deckToastShow(msg.c_str(), 2500);
  }
}

// Finished PC commands: a short toast either way.
void pcCmdSync() {
  PcCmdResult o;
  while (pcTakeCmdResult(o)) {
    const String t = o.pcName + ": " + pcCmdLabel(o.cmd) + (o.ok ? "" : String(" - ") + (o.msg.length() ? o.msg : "failed"));
    deckToastShow(t.c_str(), o.ok ? 1200 : 2500);
  }
}

uint32_t s_httpSeen = 0;
uint32_t s_prnSeen = 0;  // printersVersion() last applied
bool s_prnRebuild = false;

// The detail line: literal text and fields from the same reply ("--" while missing).
String httpDetail(const TileDef& d) {
  String out;
  for (const DetailPart& part : httpParseDetail(d.detail)) {
    if (!part.field) { out += part.text; continue; }
    HttpSensorValue v;
    char buf[48];
    if (!httpSensorRead(d.http, part.text, v) || v.offline || !v.present) out += "--";
    else if (!v.numeric) out += v.text ? v.text.get() : "--";
    else {
      if (!part.unit.length() || !formatDuration(v.number, part.unit, buf, sizeof(buf)))
        formatReading(v.number, buf, sizeof(buf));
      out += buf;
    }
  }
  return out;
}

void applyHttpToTile(TileRt& r) {
  const TileDef& d = *r.def;
  if (!d.http.enabled) return;
  HttpSensorValue value;
  const bool found = httpSensorRead(d.http, d.http.path, value);
  const bool numeric = found && !value.offline && value.present && value.numeric;
  String text = "--";
  char number[48];
  const bool dur = numeric && d.duration && formatDuration(value.number, d.unit, number, sizeof(number));
  if (dur) text = number;
  else if (numeric) {
    formatReading(value.number, number, sizeof(number));
    text = number;
    if (d.style != METRIC_CHART && d.unit.length())
      text += (d.unit.startsWith("\xC2\xB0") ? "" : " ") + d.unit;
  }
  const char* shown = found && !value.offline && value.present && !value.numeric && value.text
                          ? value.text.get() : text.c_str();
  lv_label_set_text(r.sub, shown);
  if (r.unit) lv_label_set_text(r.unit, numeric && !dur ? d.unit.c_str() : "");
  if (r.detail) lv_label_set_text(r.detail, httpDetail(d).c_str());
  lv_obj_set_style_opa(r.obj, !found || value.offline ? LV_OPA_50 : LV_OPA_COVER, 0);
  sensorThreshold(r, numeric, value.number);
  if (found && r.viz && r.pts) plotHistory(r.viz, r.pts, value.history, HTTP_HISTORY_POINTS);
  fitValue(r);
  markPageStale(r.page);
}

// Finished HTTP tile actions: a short toast either way.
void httpSync() {
  const uint32_t version = httpSensorVersion();
  if (version != s_httpSeen) {
    s_httpSeen = version;
    for (TileRt& r : s_tiles) applyHttpToTile(r);
  }
  HttpOutcome o;
  while (httpTakeResult(o)) {
    const bool ok = o.code >= 200 && o.code < 300;
    const String t = o.label + ": " + (ok ? String("OK") : o.code > 0 ? "HTTP " + String(o.code) : o.err);
    deckToastShow(t.c_str(), ok ? 1200 : 2500);
  }
}

void collectEntities() {
  printersApply(s_cfg);
  s_prnSeen = 0;
  httpSetSensors(s_cfg);
  s_httpSeen = 0;
  for (TileRt& r : s_tiles) applyHttpToTile(r);
  std::vector<String> ids;
  for (const TileRt& r : s_tiles)
    if (r.def->entity.length()) ids.push_back(r.def->entity);
  for (const PrinterDef& d : s_cfg.printers)
    if (d.plug.length()) ids.push_back(d.plug);
  if (s_cfg.ledEntity.length()) ids.push_back(s_cfg.ledEntity);
  else ledSetHaLevel(LED_HA_NONE);
  s_ledSeen = 0;
  haSetEntities(ids);
  for (Wish& w : s_wishes) {
    if (w.holding) w.level = {};
    w.dragging = w.holding = false;
  }
  for (const TileRt& r : s_tiles)
    if (sliderDomain(entityDomain(r.def->entity))) ensureWish(r.def->entity);
  std::vector<HaHistoryKey> histories;
  for (const TileRt& r : s_tiles)
    if (r.def->type == TILE_SENSOR && !r.def->http.enabled && r.def->style == METRIC_CHART)
      histories.push_back({r.def->entity, r.def->hours});
  haSetHistory(histories);
  s_historySeen = 0;
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
      if (domain == "scene" || domain == "script" || domain == "button" || domain == "input_button") {
        String sd, svc;
        toggleService(domain, r.on, sd, svc);
        if (!haCallService(sd, svc, entity, "")) deckToastShow("Home Assistant is busy, try again", 1800);
      } else {
        HaEntity e;
        haGetEntity(entity, e);
        setWish(entity, false, wishOn(entity, e) ? 0 : 1);
      }
      break;
    }
    case ACT_PAGE:
      goToPage(a.page);
      break;
    case ACT_MORE_INFO:
      openModal((int)(&r - &s_tiles[0]));
      break;
    case ACT_HTTP:
      if (!httpSend(a, tileLabel(*r.def))) deckToastShow("Busy, try again", 1500);
      break;
    case ACT_PC: {
      String err;
      if (!pcSendCommand(a.pc, a.cmd.c_str(), err)) deckToastShow(err.c_str(), 2200);
      break;
    }
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

const char* pcCmdLabel(const String& cmd) {
  static const char* const kLabels[][2] = {
      {"media_play_pause", "Play / pause"}, {"media_next", "Next track"}, {"media_prev", "Previous track"},
      {"media_stop", "Stop"},               {"volume_up", "Volume up"},   {"volume_down", "Volume down"},
      {"volume_mute", "Mute"},              {"lock", "Lock"},             {"sleep", "Sleep"},
      {"restart", "Restart"},               {"shutdown", "Shut down"}};
  for (const auto& l : kLabels)
    if (cmd == l[0]) return l[1];
  return "Run";
}

// Tile setting "confirm": "off" asks before an action that turns the tile off.
bool needsConfirm(const TileRt& r, const TileAction& a) {
  if (a.kind == ACT_PAGE || a.kind == ACT_MORE_INFO) return false;
  switch (r.def->confirm) {
    case CONFIRM_ALWAYS: return true;
    case CONFIRM_OFF:
      return (a.kind == ACT_TOGGLE && r.on) || (a.kind == ACT_CALL_SERVICE && a.service == "turn_off");
    default: return false;
  }
}

// Button text for what the action is about to do.
const char* actionVerb(const TileRt& r, const TileAction& a) {
  const String domain = entityDomain(r.def->entity);
  if (a.kind == ACT_TOGGLE) {
    if (domain == "lock") return r.on ? "Unlock" : "Lock";
    if (domain == "cover") return r.on ? "Close" : "Open";
    if (domain == "media_player") return "Play / pause";
    if (domain == "scene" || domain == "script") return "Run";
    if (domain == "button" || domain == "input_button") return "Press";
    return r.on ? "Turn off" : "Turn on";
  }
  if (a.kind == ACT_HTTP) return "Send";
  if (a.kind == ACT_PC) return pcCmdLabel(a.cmd);
  if (a.kind == ACT_CALL_SERVICE) {
    if (a.service == "turn_off") return "Turn off";
    if (a.service == "turn_on") return "Turn on";
  }
  if (domain == "button" || domain == "input_button") return "Press";
  return "Run";
}

void requestAction(TileRt& r, bool hold) {
  if (r.def->type == TILE_PRINTER) {
    if (!hold) printerTap(r);
    return;
  }
  const TileAction& a = hold ? r.def->hold : r.def->tap;
  if (r.def->http.enabled && a.kind == ACT_NONE) return;
  if (needsConfirm(r, a)) openConfirm((int)(&r - &s_tiles[0]), hold);
  else runAction(r, a);
}

void tileEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  // SHORT_CLICKED, not CLICKED: CLICKED also fires on the release after a
  // long press, which would run the tap action as well as the hold action.
  if (r.sliding) return;  // the press was a slider drag
  if (code == LV_EVENT_SHORT_CLICKED) requestAction(r, false);
  else if (code == LV_EVENT_LONG_PRESSED) requestAction(r, true);
}

bool modalShown();

// Slider tile: a sideways drag moves the level from where it was (no jump to
// the finger). The shared wish coalesces calls and flushes the final value.
void sliderEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_SIZE_CHANGED) {
    setSliderFill(r, r.frac);
    return;
  }
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) {
    r.sliding = false;
    r.slX0 = p.x;
    return;
  }
  const bool canSlide = r.def->entity.length() && haLink() == HA_CONNECTED && !modalShown();
  if (code == LV_EVENT_PRESSING) {
    if (!r.sliding) {
      if (!canSlide || abs(p.x - r.slX0) < 8) return;
      r.sliding = true;
      r.slX0 = p.x;
      r.slFrac0 = r.slValue = r.frac;
      holdLevel(r.def->entity, r.frac);
    }
    const int w = lv_obj_get_width(r.obj);
    const float f = constrain(r.slFrac0 + (p.x - r.slX0) / (float)(w > 0 ? w : 1), 0.0f, 1.0f);
    if (fabsf(f - r.slValue) < 0.005f) return;
    r.slValue = f;
    setWish(r.def->entity, true, f, true);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!r.sliding) return;
    releaseLevel(r.def->entity);
  }
}

// --- Quick panel ------------------------------------------------------------------------
// Pulled down from the top edge. Lives on LVGL's top layer so page images
// never include it. The container is QP_LIP rows taller than what shows and
// sits that much above the screen edge when open, which hides its top
// rounded corners; the compositor skips those rows.
constexpr int QP_H = 196;
constexpr int QP_LIP = 20;
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
  const int pcs = pcOnlineCount();
  setChip(s_qp.pc, pcs > 1 ? String(pcs) + " PCs online" : pcs ? String("PC online") : String("PC offline"), pcs > 0);
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
  lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
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
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BUTTON), 0);
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
  lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
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

void runAction(TileRt& r, const TileAction& a);
void refreshModal();

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
constexpr int NAME_LINE_SP = -2;  // Montserrat's line height has room for accents

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
    lv_obj_set_style_pad_left(r.sub, o.chip + 6, 0);  // clear of the icon
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
    fitName(r.name, &deck_font_12, 1);
    lv_obj_set_width(r.name, cw - 28 - (state ? 48 : 0));
    lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 28, 0);
    setFont(r.sub, &deck_font_12);
    lv_obj_set_size(r.sub, 44, lineH(&deck_font_12));
    lv_obj_set_style_pad_left(r.sub, 0, 0);
    lv_obj_align(r.sub, LV_ALIGN_RIGHT_MID, 0, 0);
    showObj(r.name, true);
    showObj(r.sub, state);
    return;
  }
  const int s = LV_MIN(30, LV_MIN(cw, ch));
  fitChip(r, s);
  lv_obj_center(r.chip);
  showObj(r.name, false);
  showObj(r.sub, false);
}

void fitDetail(TileRt& r, const lv_font_t* f, int w, lv_align_t align, int y, bool show) {
  if (!r.detail) return;
  showObj(r.detail, show);
  if (!show) return;
  setFont(r.detail, f);
  lv_obj_set_size(r.detail, w, lineH(f));
  lv_obj_align(r.detail, align, 0, y);
}

// Sensor: the reading big at the top with a small icon top right, the name
// under it; a row (name left, reading right) on short tiles.
void fitSensor(TileRt& r, int cw, int ch) {
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
    const int chip = o.val == &deck_font_14 ? 18 : 22;
    fitChip(r, chip);
    lv_obj_align(r.chip, LV_ALIGN_TOP_RIGHT, 2, -2);
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

// Sensor reading or metric value: the largest font, from the fitted one down,
// that fits the room left next to the unit.
void fitValue(TileRt& r) {
  if (!r.valFont) return;
  static const lv_font_t* const kSteps[] = {&lv_font_montserrat_40, &deck_font_28, &deck_font_20,
                                            &deck_font_14, &deck_font_12};
  int room = r.valW;
  if (r.unit) {
    lv_point_t u;
    lv_text_get_size(&u, lv_label_get_text(r.unit), lv_obj_get_style_text_font(r.unit, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    room -= 4 + u.x;
  }
  if (r.besideName)
    room -= LV_MIN(textW(lv_label_get_text(r.name), lv_obj_get_style_text_font(r.name, LV_PART_MAIN)),
                   r.valW * 55 / 100) + 8;
  const char* text = lv_label_get_text(r.sub);
  // A sensor's icon gives way when the reading does not fit beside it.
  if (r.chipRoom) {
    const bool chip = textW(text, r.valFont) <= room;
    showObj(r.chip, chip);
    lv_obj_set_style_pad_right(r.sub, chip ? r.chipRoom : 0, 0);
    if (!chip) room += r.chipRoom;
  }
  const lv_font_t* f = r.valFont;
  bool from = false;
  for (const lv_font_t* s : kSteps) {
    from = from || s == r.valFont;
    if (!from) continue;
    f = s;
    if (textW(text, s) <= room) break;
  }
  setFont(r.sub, f);
}

// PC metric: a chart or bar under a smaller number on wide tiles with the
// room, else the name on top and the number big at the bottom, else a row.
void fitMetric(TileRt& r, int cw, int ch) {
  const TileDef& d = *r.def;
  lv_obj_t* row = lv_obj_get_parent(r.sub);
  // Bar: name, number and the 8 px bar stacked; chart: the line under the number.
  const lv_font_t* nf = ch >= 64 ? &deck_font_20 : &deck_font_14;
  const bool viz = r.viz && cw >= WIDE_W &&
                   (d.style == METRIC_BAR ? ch >= lineH(nf) + lineH(&deck_font_28) * 3 / 4 + 10 : ch >= 44);
  showObj(r.viz, viz);
  r.besideName = viz && d.style != METRIC_BAR;
  if (viz) {
    setFont(r.name, nf);
    lv_obj_set_size(r.name, d.style == METRIC_BAR ? lv_pct(100) : lv_pct(55), lineH(nf));
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);
    setFont(r.unit, &deck_font_14);
    r.valFont = &deck_font_28;
    if (d.style == METRIC_BAR) {
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
  for (int pass = r.detail ? 0 : 1; pass < 2; pass++)
  for (const Opt& o : kOpts) {
    const int dh = pass ? 0 : lineH(&deck_font_12);
    if ((o.wideOnly && cw < WIDE_W) || lineH(o.name) + dh + lineH(o.val) * 3 / 4 > ch) continue;
    setFont(r.name, o.name);
    lv_obj_set_size(r.name, lv_pct(100), lineH(o.name));
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, -2);
    setFont(r.unit, o.unit);
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
  setFont(r.name, &deck_font_12);
  lv_obj_set_size(r.name, cw / 2 - 4, lineH(&deck_font_12));
  lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);
  showObj(r.name, name);
  setFont(r.unit, &deck_font_12);
  r.valFont = ch >= lineH(&deck_font_20) ? &deck_font_20 : &deck_font_14;
  r.valW = name ? cw / 2 : cw;
  lv_obj_align(row, name ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER, 0, 0);
  fitValue(r);
}

// Clock: the largest time that fits, the date under it when there is room
// (a one step smaller time is worth it for the date).
void fitClock(TileRt& r, int cw, int ch) {
  static const lv_font_t* const kTime[] = {&deck_font_clock_72, &deck_font_clock_56, &lv_font_montserrat_40,
                                           &deck_font_28, &deck_font_20, &deck_font_14};
  constexpr int N = sizeof(kTime) / sizeof(kTime[0]);
  const lv_font_t* df = ch >= 120 ? &deck_font_20 : &deck_font_14;
  const bool ampm = !netSettings.use24h;
  auto unitFont = [](const lv_font_t* f) { return lineH(f) >= 48 ? &deck_font_20 : &deck_font_14; };
  int best = N - 1, dated = -1;
  for (int i = N - 1; i >= 0; i--) {
    const lv_font_t* f = kTime[i];
    // Digits have no accents or descenders: a quarter of the line box is air.
    const int w = textW("00:00", f) + (ampm ? 4 + textW("PM", unitFont(f)) : 0), h = lineH(f) * 3 / 4;
    if (w > cw || h > ch) continue;
    best = i;
    if (h + lineH(df) <= ch) dated = i;
  }
  const bool date = dated >= 0 && dated <= best + 1;
  const lv_font_t* f = kTime[date ? dated : best];
  setFont(r.sub, f);
  setFont(r.colon, f);
  setFont(r.mins, f);
  // Fonts put the colon at x-height middle: centre it on the digits instead.
  lv_font_glyph_dsc_t dg = {}, dc = {};
  if (lv_font_get_glyph_dsc(f, &dg, '0', 0) && lv_font_get_glyph_dsc(f, &dc, ':', 0))
    lv_obj_set_style_translate_y(r.colon, (dc.ofs_y * 2 + dc.box_h - dg.ofs_y * 2 - dg.box_h) / 2, 0);
  setFont(r.unit, unitFont(f));
  lv_obj_set_style_pad_bottom(r.unit, (lineH(f) - lineH(unitFont(f))) / 5, 0);  // share the baseline
  setFont(r.name, df);
  lv_obj_set_style_margin_top(r.name, -lineH(f) / 8, 0);
  showObj(r.name, date);
}

// --- Printer tiles --------------------------------------------------------------------
// Bambu printers (deck_bambu): flex layouts inside the tile, so any grid size
// works; fitPrinter only picks fonts and drops what does not fit.
constexpr uint32_t C_PRN_OK = 0x4ADE80;    // printing
constexpr uint32_t C_PRN_BLUE = 0x7DB5FF;  // progress, idle
constexpr uint32_t C_PRN_HEAT = 0xF5B83D;  // heating, light on
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
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < n; i++) {
    const float v = pcHistoryAt(hs, (uint8_t)i);
    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
  }
  // A rate without a set scale never warns; its bar is relative to the recent peak.
  const bool autoRate = metricIsRate(*m) && !d.scaleMax;
  const float scale = autoRate ? max(hi, m->value) : metricScaleMax(*m, d.scaleMax);
  const float pct = scale > 0 ? m->value * 100.0f / scale : 0;
  // Enter the warning colour at warnThresholdPct, leave it 5 points below.
  const float enter = (float)dispSettings.warnThresholdPct;
  const bool warn = !autoRate && enter > 0 && (r.warn ? pct >= enter - 5 : pct >= enter);
  if (warn != r.warn) {
    r.warn = warn;
    lv_obj_set_style_text_color(r.sub, lv_color_hex(warn ? C_WARN : tileText(d)), 0);
  }
  const uint32_t accent = warn ? C_WARN : metricChart(d);
  if (r.viz && d.style == METRIC_BAR) {
    lv_bar_set_value(r.viz, (int32_t)constrain(pct, 0.0f, 100.0f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
  } else if (r.viz && r.pts) {
    // History as tenths, oldest first, padded on the left while it fills.
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
  lv_label_set_text(r.name, d.label.length() ? d.label.c_str() : sensor ? tileLabel(d).c_str() : "");
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
  lv_obj_set_style_pad_bottom(r.unit, 4, 0);
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
  // Chart: the number top right, the line under it.
  r.viz = lv_chart_create(t);
  lv_obj_remove_style_all(r.viz);
  lv_obj_set_size(r.viz, lv_pct(100), lv_pct(62));
  lv_obj_align(r.viz, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_chart_set_type(r.viz, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(r.viz, 0, 0);
  const int count = sensor ? (d.http.enabled ? HTTP_HISTORY_POINTS : HA_HISTORY_POINTS) : PC_HISTORY_LEN;
  lv_chart_set_point_count(r.viz, count);
  lv_obj_set_style_line_width(r.viz, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(r.viz, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(r.viz, LV_OBJ_FLAG_CLICKABLE);
  r.ser = lv_chart_add_series(r.viz, lv_color_hex(sensor ? sensorLine(r) : metricChart(d)), LV_CHART_AXIS_PRIMARY_Y);
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
    if (r.def->tap.kind == ACT_PC) applyPcStateToTile(r);
    if (r.def->type != TILE_METRIC) continue;
    applyPcToTile(r);
    markPageStale(r.page);
  }
}

// Glass tiles are see-through: their glass is baked into the page
// background (bakeGlass); pressed and on states draw over it.
void glassStyle(lv_obj_t* t, uint32_t accent) {
  const lv_opa_t on = (lv_opa_t)(dispSettings.glassOn * 255 / 100);
  lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(t, 0, 0);
  // A faint fill alone hardly tells on from off: outline it in the accent.
  if (dispSettings.glassOn < 50) {
    lv_obj_set_style_border_width(t, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(t, lv_color_hex(accent), LV_STATE_CHECKED);
  }
}

// Text on an "on" tile: dark while the accent fill is bright enough, else
// the normal light text (Glass with a faint fill over a dark picture).
uint32_t onTextColor(uint32_t accent, bool glass) {
  if (!glass) return C_TEXT_ON;
  const uint32_t lum = ((accent >> 16 & 0xFF) * 299 + (accent >> 8 & 0xFF) * 587 + (accent & 0xFF) * 114) / 1000;
  return lum * dispSettings.glassOn / 100 >= 115 ? C_TEXT_ON : C_TEXT;
}

void makeTile(lv_obj_t* grid, const TileDef& def, int page, int col, int row, int w, int h) {
  const String domain = entityDomain(def.entity);
  const uint32_t accent = def.hasColor ? def.color : def.type == TILE_PRINTER ? C_PRN_HEAT : domainColor(domain);
  const bool glass = s_theme == THEME_GLASS && wallBlur();

  lv_obj_t* t = lv_obj_create(grid);
  lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, w, LV_GRID_ALIGN_STRETCH, row, h);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(t, 16, 0);
  // Daylight tiles get a hairline, white on light grey is too soft alone.
  lv_obj_set_style_border_width(t, s_theme == THEME_DAYLIGHT ? 1 : 0, 0);
  lv_obj_set_style_border_color(t, lv_color_hex(C_TILE_LINE), 0);
  lv_obj_set_style_pad_all(t, 9, 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(accent), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(t, lv_color_hex(tileText(def)), 0);
  lv_obj_set_style_text_color(t, lv_color_hex(onTextColor(accent, glass)), LV_STATE_CHECKED);
  // Press feedback: the card darkens and shrinks a little, eased in 90 ms.
  lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(accent), lv_color_black(), 200),
                            LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_transform_width(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transition(t, &s_pressTrans, 0);
  lv_obj_set_style_transition(t, &s_pressTrans, LV_STATE_PRESSED);
  if (glass) glassStyle(t, accent);

  if (def.type == TILE_PRINTER) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makePrinterContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyPrinterToTile(s_tiles.back());
    if (def.show == PS_LIGHT || def.show == PS_SUMMARY) lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
    else lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_METRIC || (def.type == TILE_SENSOR && def.style == METRIC_CHART)) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeMetricContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    if (def.type == TILE_METRIC) applyPcToTile(s_tiles.back());
    else {
      applyHaToTile(s_tiles.back(), true);
      applySensorHistory(s_tiles.back());
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
    }
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Clock: the time ("color") over the date, centred; fitClock sizes them.
  if (def.type == TILE_CLOCK) {
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    lv_obj_t* row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t* hm = lv_obj_create(row);
    lv_obj_remove_style_all(hm);
    lv_obj_set_size(hm, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hm, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(hm, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (def.hasColor) lv_obj_set_style_text_color(hm, lv_color_hex(def.color), 0);
    r.sub = lv_label_create(hm);
    r.colon = lv_label_create(hm);
    lv_label_set_text(r.colon, ":");
    r.colonLit = true;
    r.mins = lv_label_create(hm);
    r.unit = lv_label_create(row);
    lv_obj_set_style_text_opa(r.unit, LV_OPA_70, 0);
    r.name = lv_label_create(t);
    lv_obj_set_style_text_opa(r.name, LV_OPA_70, 0);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyClock(s_tiles.back());
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Slider: the level fill goes in first so it stays behind the content.
  lv_obj_t* fill = nullptr;
  if (def.type == TILE_SLIDER) {
    fill = lv_obj_create(t);
    lv_obj_remove_style_all(fill);
    lv_obj_add_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(fill, 16, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(fill, s_theme == THEME_DAYLIGHT ? LV_OPA_40 : LV_OPA_50, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_PRESS_LOCK);  // the drag may leave the tile
  }

  lv_obj_t* chip = lv_obj_create(t);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  styleIconChip(chip, accent);
  lv_obj_t* ic = lv_label_create(chip);
  lv_obj_center(ic);

  // Fonts and places come from fitTile once the tile has its size.
  lv_obj_t* name = lv_label_create(t);
  lv_label_set_text(name, tileLabel(def).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_t* sub = lv_label_create(t);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
  if (def.type == TILE_SENSOR) {
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);
  }

  const int iconPx = def.type == TILE_SENSOR ? 16 : 20;
  TileRt r = {};
  r.def = &def;
  r.page = page;
  r.obj = t;
  r.name = name;
  r.sub = sub;
  r.icon = ic;
  r.chip = chip;
  r.iconPx = iconPx;
  r.iconCp = showIcon(ic, def, nullptr, false, iconPx, 0);
  r.fill = fill;
  if (def.type == TILE_SENSOR) r.detail = makeDetail(t, def);
  lv_label_set_text(sub, tileSub(r).c_str());
  const size_t idx = s_tiles.size();
  s_tiles.push_back(r);
  if (def.tap.kind == ACT_PC) applyPcStateToTile(s_tiles.back());
  lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
  if (fill)
    for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                              LV_EVENT_SIZE_CHANGED})
      lv_obj_add_event_cb(t, sliderEvent, c, (void*)(uintptr_t)idx);
}

// Glass: the page background is baked once the tiles are laid out: the
// wallpaper, and under each tile the blurred wallpaper cut to its rounded
// rect with the tint and a hairline. A page render is then one image copy
// instead of per-tile blending.
void bakeGlass(Page& p) {
  const lv_image_dsc_t* sharp = wallSharp();
  const lv_image_dsc_t* blur = wallBlur();
  if (!sharp || !blur || !p.root || !p.grid) return;
  const int w = sharp->header.w, h = sharp->header.h;
  if (!p.glass) p.glass = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!p.glass) return;
  for (int y = 0; y < h; y++)
    memcpy(p.glass->data + y * p.glass->header.stride, sharp->data + y * sharp->header.stride, w * 2);
  // The glass is drawn in ARGB8888 over nothing, then laid over the picture
  // and dithered to RGB565 once: blending in RGB565 left coloured bands.
  lv_draw_buf_t* top = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
  if (!top) return;
  lv_draw_buf_clear(top, nullptr);
  lv_obj_update_layout(p.root);
  lv_obj_t* cv = lv_canvas_create(p.root);
  lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_draw_buf(cv, top);
  lv_layer_t layer;
  lv_canvas_init_layer(cv, &layer);
  const lv_area_t all = {0, 0, w - 1, h - 1};
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (uint32_t i = 0; i < lv_obj_get_child_count(p.grid); i++) {
    lv_obj_t* t = lv_obj_get_child(p.grid, i);
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    const int r = lv_obj_get_style_radius(t, LV_PART_MAIN);
    lv_draw_image_dsc_t id;
    lv_draw_image_dsc_init(&id);
    id.src = blur;
    id.image_area = tc;  // the radius mask works on this area
    id.clip_radius = r;
    layer._clip_area = tc;
    lv_draw_image(&layer, &id, &all);
    layer._clip_area = all;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = r;
    rd.bg_color = lv_color_hex(C_TILE);
    rd.bg_opa = (lv_opa_t)(dispSettings.glassTint * 255 / 100);
    rd.border_color = lv_color_white();
    rd.border_opa = LV_OPA_20;
    rd.border_width = 1;
    lv_draw_rect(&layer, &rd, &tc);
  }
  lv_canvas_finish_layer(cv, &layer);
  lv_obj_delete(cv);
  for (int y = 0; y < h; y++) {
    const uint8_t* s = top->data + y * top->header.stride;  // B, G, R, A
    uint16_t* d = (uint16_t*)(p.glass->data + y * p.glass->header.stride);
    for (int x = 0; x < w; x++, s += 4) {
      const int a = s[3];
      if (!a) continue;
      int r = s[2], g = s[1], b = s[0];
      if (a < 255) {  // rounded edge: mix with the picture under it
        const uint16_t q = d[x];
        r = (r * a + ((q >> 8 & 0xF8) | q >> 13) * (255 - a) + 127) / 255;
        g = (g * a + ((q >> 3 & 0xFC) | (q >> 9 & 0x03)) * (255 - a) + 127) / 255;
        b = (b * a + ((q << 3 & 0xF8) | (q >> 2 & 0x07)) * (255 - a) + 127) / 255;
      }
      d[x] = wallDither565(r, g, b, x, y);
    }
  }
  lv_draw_buf_destroy(top);
  lv_obj_set_style_bg_image_src(p.root, p.glass, 0);
  lv_obj_invalidate(p.root);
  p.stale = true;
}

// Shows or hides a tiles page's top bar; the grid takes the freed height.
void layoutHeader(Page& p) {
  if (!p.grid) return;
  s_zonesDirty = true;
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
  if (p.glass) bakeGlass(p);  // the tiles moved
}

// Where each tile of a page lands: explicit cells first (in tile order), then
// the rest flow into the first free place that fits. The portal grid editor
// mirrors this rule (layout() in deck_web.h). Cells count in half cells.
struct Cell {
  bool placed;
  int x, y, w, h;
};

void placeTiles(const PageDef& def, int orient, std::vector<Cell>& out) {
  const int cols = def.cols[orient] * 2, rows = def.rows[orient] * 2;
  bool used[12][12] = {};
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
  out.assign(def.tiles.size(), Cell{false, 0, 0, 2, 2});
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

  // Two tracks per cell: tiles sit and span in half cells. Static descriptor
  // arrays per page (LVGL keeps the pointers).
  const int cols = def.cols[orient] * 2, rows = def.rows[orient] * 2;
  static int32_t colDsc[MAX_PAGES][13];
  static int32_t rowDsc[MAX_PAGES][13];
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
  int orient = 0, cols = 1, rows = 1, cw = 1, ch = 1;  // in half cells, px per half cell
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
  // Tiles added from the picker: s_ed.tiles past the page's own get these.
  std::vector<TileDef> added;
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
      c.w = constrain((int)lroundf((p.x - ED_PAD) / sx) - c.x, 1, s_ed.cols - c.x);
      c.h = constrain((int)lroundf((p.y - ED_BAR_H) / sy) - c.y, 1, s_ed.rows - c.y);
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

// --- Edit mode: add a tile from the cached HA entity list ------------------------------
struct PickRow {
  String id, name, state;
};
// One record of the cached list: offsets of its strings in s_pickBuf.
struct PickSpan {
  uint32_t id, name, state;
  uint16_t idLen, nameLen, stLen;
};
struct PickDomain {
  const char* label;
  const char* domains;  // space separated
};
const PickDomain kPickDomains[] = {
    {"Lights", "light"},          {"Switches", "switch input_boolean"}, {"Scenes", "scene"},
    {"Scripts", "script"},        {"Covers", "cover"},                  {"Fans", "fan"},
    {"Media", "media_player"},    {"Locks", "lock"},                    {"Climate", "climate"},
    {"Buttons", "button input_button"}, {"Numbers", "input_number number"},
    {"Sensors", "sensor"},        {"Binary", "binary_sensor"},          {"Automations", "automation"},
};
constexpr int PICK_DOMAINS = sizeof(kPickDomains) / sizeof(kPickDomains[0]);
constexpr size_t PICK_MAX = 50;  // rows per page of the list

lv_obj_t* s_pick = nullptr;  // the picker sheet, a child of the editor root
lv_obj_t* s_pickDead = nullptr;  // a closed sheet, deleted later (see pickClose)
lv_obj_t* s_pickList = nullptr;
lv_obj_t* s_pickInfo = nullptr;
lv_obj_t* s_pickChips[PICK_DOMAINS] = {};
std::vector<PickRow> s_pickRows;  // the rows shown
// The cached list file in PSRAM while the picker is open (100+ KB), and the
// shown group's records sorted by name.
char* s_pickBuf = nullptr;
size_t s_pickLen = 0;
PickSpan* s_pickAll = nullptr;
size_t s_pickAllN = 0;
int s_pickDom = 0;
size_t s_pickFirst = 0;
uint32_t s_addSeq = 0;

bool domainInSpan(const char* d, size_t n, const char* list) {
  for (const char* p = list; *p;) {
    const char* e = strchr(p, ' ');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == n && !strncmp(p, d, len)) return true;
    if (!e) break;
    p = e + 1;
  }
  return false;
}
bool domainIn(const String& d, const char* list) { return domainInSpan(d.c_str(), d.length(), list); }

void pickFree() {
  if (s_pickBuf) heap_caps_free(s_pickBuf);
  if (s_pickAll) heap_caps_free(s_pickAll);
  s_pickBuf = nullptr;
  s_pickAll = nullptr;
  s_pickLen = s_pickAllN = 0;
  s_pickRows.clear();
  s_pickRows.shrink_to_fit();
}

bool pickReadFile() {
  if (s_pickBuf) return true;
  if (!deckFsReady()) return false;
  File f = LittleFS.open(haEntityListPath(), "r");
  if (!f) return false;
  const size_t n = f.size();
  s_pickBuf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (s_pickBuf) {
    s_pickLen = f.read((uint8_t*)s_pickBuf, n);
    s_pickBuf[s_pickLen] = 0;
  }
  f.close();
  return s_pickBuf != nullptr;
}

// End of the JSON string starting at p (just past its opening quote).
const char* jsonStrEnd(const char* p, const char* end) {
  while (p < end && *p != '"') p += *p == '\\' ? 2 : 1;
  return p < end ? p : nullptr;
}

// A JSON string's text without its escapes (\uXXXX as UTF-8).
String spanText(uint32_t off, uint16_t len) {
  String s;
  const char* p = s_pickBuf + off;
  const char* end = p + len;
  while (p < end) {
    if (*p != '\\' || p + 1 >= end) {
      s += *p++;
      continue;
    }
    const char c = p[1];
    p += 2;
    if (c == 'u' && p + 4 <= end) {
      const unsigned v = strtoul(String(p).substring(0, 4).c_str(), nullptr, 16);
      p += 4;
      if (v < 0x80) {
        s += (char)v;
      } else if (v < 0x800) {
        s += (char)(0xC0 | (v >> 6));
        s += (char)(0x80 | (v & 0x3F));
      } else {
        s += (char)(0xE0 | (v >> 12));
        s += (char)(0x80 | ((v >> 6) & 0x3F));
        s += (char)(0x80 | (v & 0x3F));
      }
    } else {
      s += c == 'n' ? ' ' : c == 't' ? ' ' : c;
    }
  }
  return s;
}

// The flat {"id":..,"name":..,"state":..} records of one group, sorted by
// name. Returns how many there are.
size_t pickIndex(const char* domains) {
  if (s_pickAll) heap_caps_free(s_pickAll);
  s_pickAll = nullptr;
  s_pickAllN = 0;
  if (!pickReadFile()) return 0;
  const char* buf = s_pickBuf;
  const char* end = buf + s_pickLen;
  const char* p = strstr(buf, "\"entities\":[");
  if (!p) return 0;
  p += 12;
  size_t cap = 0;
  while (p < end) {
    while (p < end && *p != '{' && *p != ']') p++;
    if (p >= end || *p == ']') break;
    p++;
    PickSpan s = {};
    bool haveId = false;
    while (p < end && *p != '}') {
      if (*p != '"') {
        p++;
        continue;
      }
      const char* ks = p + 1;
      const char* ke = jsonStrEnd(ks, end);
      if (!ke) return s_pickAllN;
      p = ke + 1;
      while (p < end && (*p == ':' || *p == ' ')) p++;
      if (p >= end || *p != '"') {
        while (p < end && *p != ',' && *p != '}') p++;
        continue;
      }
      const char* vs = p + 1;
      const char* ve = jsonStrEnd(vs, end);
      if (!ve) return s_pickAllN;
      p = ve + 1;
      const size_t kl = ke - ks;
      const uint32_t off = vs - buf;
      const uint16_t vl = (uint16_t)min<size_t>(ve - vs, 0xFFFF);
      if (kl == 2 && !strncmp(ks, "id", 2)) {
        s.id = off;
        s.idLen = vl;
        haveId = true;
      } else if (kl == 4 && !strncmp(ks, "name", 4)) {
        s.name = off;
        s.nameLen = vl;
      } else if (kl == 5 && !strncmp(ks, "state", 5)) {
        s.state = off;
        s.stLen = vl;
      }
    }
    if (p < end) p++;
    if (!haveId) continue;
    const char* dot = (const char*)memchr(buf + s.id, '.', s.idLen);
    if (!dot || !domainInSpan(buf + s.id, dot - (buf + s.id), domains)) continue;
    if (s_pickAllN == cap) {
      cap = cap ? cap * 2 : 64;
      PickSpan* g = (PickSpan*)heap_caps_realloc(s_pickAll, cap * sizeof(PickSpan), MALLOC_CAP_SPIRAM);
      if (!g) break;
      s_pickAll = g;
    }
    s_pickAll[s_pickAllN++] = s;
  }
  std::sort(s_pickAll, s_pickAll + s_pickAllN, [buf](const PickSpan& a, const PickSpan& b) {
    const char* x = buf + (a.nameLen ? a.name : a.id);
    const char* y = buf + (b.nameLen ? b.name : b.id);
    const size_t xl = a.nameLen ? a.nameLen : a.idLen, yl = b.nameLen ? b.nameLen : b.idLen;
    const int c = strncasecmp(x, y, min(xl, yl));
    return c ? c < 0 : xl < yl;
  });
  return s_pickAllN;
}

const char* edAutoType(const String& d) {
  static const char* kToggle = "light switch fan input_boolean lock cover media_player climate automation siren humidifier valve";
  static const char* kSensor = "sensor binary_sensor weather sun person device_tracker zone";
  return domainIn(d, kToggle) ? "toggle" : domainIn(d, kSensor) ? "sensor" : "action";
}

bool edCellFree(const Cell& c) {
  for (const EdTile& o : s_ed.tiles)
    if (o.c.placed && !o.removed && o.c.x < c.x + c.w && c.x < o.c.x + o.c.w && o.c.y < c.y + c.h &&
        c.y < o.c.y + o.c.h)
      return false;
  return true;
}

void edMakeBox(int k, const TileDef& d);

// A new 1x1 tile for the entity on the first free cell. False (with a toast)
// when the page is full.
bool edAdd(const String& entity, const String& name) {
  int live = 0;
  for (const EdTile& t : s_ed.tiles) live += !t.removed;
  Cell c = {true, 0, 0, 2, 2};
  bool found = false;
  for (int y = 0; y < s_ed.rows && !found; y++)
    for (int x = 0; x < s_ed.cols && !found; x++) {
      c.x = x;
      c.y = y;
      found = edCellFree(c);
    }
  if (!found || live >= MAX_PAGE_TILES) {
    deckToastShow(found ? "This page has the most tiles it can" : "No free cell: remove or shrink a tile", 2200);
    return false;
  }
  TileDef d;
  d.id = "d" + String(millis()) + "_" + String(s_addSeq++);
  const int dot = entity.indexOf('.');
  const String dom = dot > 0 ? entity.substring(0, dot) : String();
  d.type = !strcmp(edAutoType(dom), "toggle") ? TILE_TOGGLE : !strcmp(edAutoType(dom), "sensor") ? TILE_SENSOR : TILE_ACTION;
  d.entity = entity;
  s_ed.added.push_back(d);
  const int k = (int)s_ed.tiles.size();
  s_ed.tiles.push_back({c, false, nullptr});
  edMakeBox(k, s_ed.added.back());
  if (name.length()) lv_label_set_text(lv_obj_get_child(s_ed.tiles[k].box, 0), name.c_str());
  s_ed.dirty = true;
  return true;
}

// Hidden now, deleted on the next open or with the editor: this can run in
// an event of the sheet's own child, and an async delete could outlive the root.
void pickClose() {
  if (s_pick) {
    lv_obj_add_flag(s_pick, LV_OBJ_FLAG_HIDDEN);
    s_pickDead = s_pick;
  }
  s_pick = s_pickList = s_pickInfo = nullptr;
  pickFree();
}

void pickFill(int dom, size_t first);

// Row i of the shown page; the "more" rows page through the group.
constexpr size_t PICK_PREV = 0xFFFE, PICK_NEXT = 0xFFFF;
void pickRowEvent(lv_event_t* e) {
  const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (i == PICK_PREV || i == PICK_NEXT) {
    // Deleting the list's rows from inside a row's event: let it finish first.
    lv_async_call([](void* d) {
      pickFill(s_pickDom, (size_t)(uintptr_t)d);
    }, (void*)(uintptr_t)(i == PICK_NEXT ? s_pickFirst + PICK_MAX : s_pickFirst - PICK_MAX));
    return;
  }
  if (i >= s_pickRows.size()) return;
  if (edAdd(s_pickRows[i].id, s_pickRows[i].name)) pickClose();
}

bool onPage(const String& entity) {
  for (size_t i = 0; i < s_ed.tiles.size(); i++) {
    if (s_ed.tiles[i].removed) continue;
    const size_t own = s_ed.tiles.size() - s_ed.added.size();
    const String& e = i < own ? s_cfg.pages[s_ed.cfgPage].tiles[i].entity : s_ed.added[i - own].entity;
    if (e == entity) return true;
  }
  return false;
}

lv_obj_t* pickRowBox(int lw, size_t tag) {
  lv_obj_t* b = lv_obj_create(s_pickList);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, lw, 46);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(b, 10, 0);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, pickRowEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)tag);
  return b;
}

void pickMoreRow(int lw, size_t tag, const char* text) {
  lv_obj_t* l = lv_label_create(pickRowBox(lw, tag));
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(C_OK), 0);
  lv_obj_center(l);
}

// Show group dom from its record `first` (a new group indexes it first).
void pickFill(int dom, size_t first) {
  if (!s_pick || dom < 0 || dom >= PICK_DOMAINS) return;
  for (int i = 0; i < PICK_DOMAINS; i++) {
    if (i == dom) lv_obj_add_state(s_pickChips[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(s_pickChips[i], LV_STATE_CHECKED);
  }
  if (dom != s_pickDom || !s_pickAll) pickIndex(kPickDomains[dom].domains);
  s_pickDom = dom;
  const size_t total = s_pickAllN;
  s_pickFirst = first < total ? first : 0;
  const size_t last = min(total, s_pickFirst + PICK_MAX);
  s_pickRows.clear();
  for (size_t i = s_pickFirst; i < last; i++) {
    const PickSpan& s = s_pickAll[i];
    s_pickRows.push_back({spanText(s.id, s.idLen), spanText(s.name, s.nameLen), spanText(s.state, s.stLen)});
  }
  lv_obj_clean(s_pickList);
  if (!total) {
    lv_label_set_text(s_pickInfo, "None. The list comes from \"Update from HA\" in the portal.");
  } else if (total > PICK_MAX) {
    lv_label_set_text_fmt(s_pickInfo, "%u-%u of %u", (unsigned)s_pickFirst + 1, (unsigned)last, (unsigned)total);
  } else {
    lv_label_set_text_fmt(s_pickInfo, "%u", (unsigned)total);
  }
  const int lw = lv_obj_get_content_width(s_pickList);
  if (s_pickFirst) pickMoreRow(lw, PICK_PREV, LV_SYMBOL_UP " Previous");
  for (size_t i = 0; i < s_pickRows.size(); i++) {
    const PickRow& row = s_pickRows[i];
    lv_obj_t* b = pickRowBox(lw, i);
    const bool here = onPage(row.id);
    lv_obj_t* n = lv_label_create(b);
    const String shown = row.name.length() ? row.name : row.id;
    lv_label_set_text(n, here ? (String(LV_SYMBOL_OK " ") + shown).c_str() : shown.c_str());
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(n, lw - 20 - 90);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 5);
    lv_obj_t* id = lv_label_create(b);
    lv_label_set_text(id, row.id.c_str());
    lv_label_set_long_mode(id, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(id, lw - 20 - 90);
    lv_obj_set_style_text_color(id, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(id, &deck_font_14, 0);
    lv_obj_align(id, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_t* st = lv_label_create(b);
    lv_label_set_text(st, row.state.c_str());
    lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(st, 86, lv_font_get_line_height(lv_obj_get_style_text_font(st, LV_PART_MAIN)));
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(C_DIM), 0);
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  if (last < total) pickMoreRow(lw, PICK_NEXT, "Next " LV_SYMBOL_DOWN);
  lv_obj_scroll_to_y(s_pickList, 0, LV_ANIM_OFF);
}

void pickChipEvent(lv_event_t* e) { pickFill((int)(uintptr_t)lv_event_get_user_data(e), 0); }
void pickCloseEvent(lv_event_t*) { pickClose(); }

void pickOpen(int dom) {
  if (!s_ed.root || s_pick || dom < 0 || dom >= PICK_DOMAINS) return;
  if (s_pickDead) lv_obj_delete(s_pickDead);
  s_pickDead = nullptr;
  const int w = logicalW(), h = logicalH();
  s_pick = lv_obj_create(s_ed.root);
  lv_obj_remove_style_all(s_pick);
  lv_obj_set_size(s_pick, w, h);
  lv_obj_set_style_bg_color(s_pick, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_pick, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s_pick, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_pick, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(s_pick);
  lv_label_set_text(title, "Add a tile");
  lv_obj_set_style_text_font(title, &deck_font_20, 0);
  lv_obj_set_pos(title, 12, 13);
  s_pickInfo = lv_label_create(s_pick);
  lv_obj_set_style_text_color(s_pickInfo, lv_color_hex(C_DIM), 0);
  lv_label_set_long_mode(s_pickInfo, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s_pickInfo, w - 140 - 96);
  lv_obj_set_pos(s_pickInfo, 140, 17);
  lv_obj_t* close = qpButton(s_pick, w - 84 - 8, 4, 84, "Close", false);
  lv_obj_add_event_cb(close, pickCloseEvent, LV_EVENT_CLICKED, nullptr);

  // Domain chips: one row that scrolls sideways.
  lv_obj_t* chips = lv_obj_create(s_pick);
  lv_obj_remove_style_all(chips);
  lv_obj_set_pos(chips, 0, ED_BAR_H);
  lv_obj_set_size(chips, w, 40);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_hor(chips, 8, 0);
  lv_obj_set_style_pad_column(chips, 6, 0);
  lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
  for (int i = 0; i < PICK_DOMAINS; i++) {
    lv_obj_t* c = qpButton(chips, 0, 0, LV_SIZE_CONTENT, kPickDomains[i].label, false);
    lv_obj_set_height(c, 34);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_add_event_cb(c, pickChipEvent, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    s_pickChips[i] = c;
  }

  s_pickList = lv_obj_create(s_pick);
  lv_obj_remove_style_all(s_pickList);
  lv_obj_set_pos(s_pickList, 0, ED_BAR_H + 46);
  lv_obj_set_size(s_pickList, w, h - ED_BAR_H - 46);
  lv_obj_set_flex_flow(s_pickList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(s_pickList, 8, 0);
  lv_obj_set_style_pad_bottom(s_pickList, 8, 0);
  lv_obj_set_style_pad_row(s_pickList, 6, 0);
  lv_obj_set_scroll_dir(s_pickList, LV_DIR_VER);
  lv_obj_update_layout(s_pick);
  pickFill(dom, 0);
  lv_obj_scroll_to_view(s_pickChips[dom], LV_ANIM_OFF);
}

void edAddEvent(lv_event_t*) { pickOpen(0); }

void edRemoveEvent(lv_event_t* e) { edRemove((int)(uintptr_t)lv_event_get_user_data(e)); }
void edDoneEvent(lv_event_t*) { s_ed.pending = EditState::P_SAVE; }
void edCancelEvent(lv_event_t*) { s_ed.pending = EditState::P_CANCEL; }

// The name the tile shows (HA friendly name, metric name), else from the config.
String edTileName(const TileDef& d) {
  for (const TileRt& r : s_tiles)
    if (r.def == &d && r.name && *lv_label_get_text(r.name)) return lv_label_get_text(r.name);
  if (d.type != TILE_METRIC || d.label.length()) return tileLabel(d);
  const PcMetric* m = pcMetricFind(pcSource(d.pc), d.metric);
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
  const Cell& c = s_ed.tiles[k].c;
  edPlace(b, c);
  // Half-cell boxes: a smaller x and grip, so part of the box is left to drag.
  const bool small = c.w * s_ed.cw < 70 || c.h * s_ed.ch < 50;
  const int xs = small ? 20 : 30, gs = small ? 22 : 34;

  lv_obj_t* name = lv_label_create(b);
  lv_label_set_text(name, edTileName(d).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  // Full width, mid height: below the x and above the grip.
  lv_obj_set_width(name, lv_pct(100));
  if (small) lv_obj_set_style_text_font(name, &deck_font_12, 0);
  lv_obj_set_style_max_height(name, small ? lineH(&deck_font_12) : 2 * lineH(&deck_font_14), 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, small ? 0 : 6);

  lv_obj_t* x = lv_obj_create(b);
  lv_obj_remove_style_all(x);
  lv_obj_set_size(x, xs, xs);
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
  lv_obj_set_size(g, gs, gs);
  lv_obj_align(g, LV_ALIGN_BOTTOM_RIGHT, 8, 8);
  lv_obj_add_flag(g, LV_OBJ_FLAG_PRESS_LOCK);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* bar = lv_obj_create(g);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, i ? 3 : gs * 14 / 34, i ? gs * 14 / 34 : 3);
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
  s_ed.cols = def.cols[s_ed.orient] * 2;  // half cells
  s_ed.rows = def.rows[s_ed.orient] * 2;
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

  constexpr int BW = 84, AW = 48;
  s_ed.title = lv_label_create(root);
  lv_obj_set_style_text_font(s_ed.title, &deck_font_20, 0);
  lv_label_set_long_mode(s_ed.title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s_ed.title, w - 2 * BW - AW - 48);
  lv_obj_set_pos(s_ed.title, 12, 13);
  lv_obj_t* add = qpButton(root, w - 2 * BW - AW - 24, 4, AW, LV_SYMBOL_PLUS, false);
  lv_obj_add_event_cb(add, edAddEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel = qpButton(root, w - 2 * BW - 16, 4, BW, "Cancel", false);
  lv_obj_add_event_cb(cancel, edCancelEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* done = qpButton(root, w - BW - 8, 4, BW, LV_SYMBOL_OK " Done", false);
  lv_obj_set_style_bg_color(done, lv_color_hex(C_OK), 0);
  lv_obj_set_style_text_color(done, lv_color_hex(C_TEXT_ON), 0);
  lv_obj_add_event_cb(done, edDoneEvent, LV_EVENT_CLICKED, nullptr);

  for (int y = 0; y < s_ed.rows; y += 2)
    for (int x = 0; x < s_ed.cols; x += 2) {
      lv_obj_t* o = lv_obj_create(root);
      lv_obj_remove_style_all(o);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_radius(o, 12, 0);
      lv_obj_set_style_border_width(o, 1, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(C_TRACK), 0);
      edPlace(o, Cell{true, x, y, 2, 2});
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
  s_pick = s_pickList = s_pickInfo = s_pickDead = nullptr;  // deleted with the root
  pickFree();
  lv_obj_delete(s_ed.root);
  s_ed = EditState();
  setModalOpen(false);
  if (s_count) setPager(sh.active);
}

// The stored deck.json with this page's positions (pinned for every placed
// tile, in the current orientation) and removals. Tiles match by index: the
// parser keeps every tile of a page in file order. Other fields stay as they are.
// A cell back into deck.json units: whole numbers stay integers, w/h 1 left out.
void putHalf(JsonObject q, const char* key, int v) {
  if (v % 2) q[key] = v / 2.0f;
  else q[key] = v / 2;
}

void putHalves(JsonObject q, const Cell& c) {
  putHalf(q, "x", c.x);
  putHalf(q, "y", c.y);
  if (c.w != 2) putHalf(q, "w", c.w);
  if (c.h != 2) putHalf(q, "h", c.h);
}

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
  const size_t own = s_ed.tiles.size() - s_ed.added.size();
  if (pv.isNull() || tiles.size() != own) {
    err = "page changed meanwhile";
    return false;
  }
  const char* key = s_ed.orient == ORIENT_LANDSCAPE ? "landscape" : "portrait";
  for (size_t i = 0; i < own; i++) {
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
    putHalves(q, t.c);
  }
  for (size_t i = own; i-- > 0;)
    if (s_ed.tiles[i].removed) tiles.remove(i);
  for (size_t j = 0; j < s_ed.added.size(); j++) {
    const EdTile& t = s_ed.tiles[own + j];
    if (t.removed) continue;
    const TileDef& d = s_ed.added[j];
    JsonObject tv = tiles.add<JsonObject>();
    tv["id"] = d.id;
    tv["type"] = d.type == TILE_TOGGLE ? "toggle" : d.type == TILE_SENSOR ? "sensor" : "action";
    tv["entity"] = d.entity;
    JsonObject q = tv["pos"][key].to<JsonObject>();
    putHalves(q, t.c);
  }
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
  if (s_theme == THEME_GLASS) wallLoad(w, logicalH(), dispSettings.glassBlur, dispSettings.glassDim);
  else wallFree();
  s_count = 0;
  s_tiles.clear();
  s_zonesDirty = true;
  s_tiles.reserve(64);
  for (const PageDef& pd : s_cfg.pages) {
    if (s_count >= MAX_PAGES) break;
    if (pd.printer.length() && !printerPowered(pd.printer)) continue;
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
  if (s_theme == THEME_GLASS)
    for (int i = 0; i < s_count; i++) bakeGlass(s_pages[i]);
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
    if (s_pages[i].glass) lv_draw_buf_destroy(s_pages[i].glass);
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

// The pairing card: shows the code a companion asked for, until it is
// entered, cancelled or times out (pc_metrics owns the state).
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

void syncSwipeZones() {
  const int a = sh.active;
  if (!s_zonesDirty && a == s_zonesPage) return;
  s_zonesDirty = false;
  s_zonesPage = a;
  Shared::Zone z[MAX_PAGE_TILES];
  int n = 0;
  if (a >= 0 && a < s_count && s_pages[a].root) {
    lv_obj_update_layout(s_pages[a].root);
    lv_area_t root;
    lv_obj_get_coords(s_pages[a].root, &root);
    for (const TileRt& r : s_tiles) {
      if (r.page != a || !r.fill || n == MAX_PAGE_TILES) continue;
      lv_area_t t;
      lv_obj_get_coords(r.obj, &t);
      z[n++] = {(int16_t)(t.x1 - root.x1), (int16_t)(t.y1 - root.y1), (int16_t)(t.x2 - root.x1),
                (int16_t)(t.y2 - root.y1)};
    }
  }
  portENTER_CRITICAL(&s_mux);
  memcpy(sh.zones, z, sizeof(Shared::Zone) * n);
  sh.nZones = n;
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

// Before the page images are freed: no gesture may take them any more, and
// one the touch task (other core) took just before gets time to reach the
// compositor, so panelQuiesce cancels it.
void stopGestures() {
  portENTER_CRITICAL(&s_mux);
  for (int i = 0; i < MAX_PAGES; i++) sh.valid[i] = false;
  sh.panelValid = false;
  portEXIT_CRITICAL(&s_mux);
  vTaskDelay(2);
  panelQuiesce();
  releaseSwipe();
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

// The colon of the shown page's clocks blinks with the second; other pages
// keep it lit, so their images stay valid.
void blinkClocks() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const bool lit = tv.tv_sec < 1609459200 || tv.tv_usec < 500000;
  for (TileRt& r : s_tiles) {
    if (!r.colon) continue;
    const bool on = lit || r.page != sh.active;
    if (on == r.colonLit) continue;
    r.colonLit = on;
    lv_obj_set_style_text_opa(r.colon, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (r.page != sh.active) markPageStale(r.page);
  }
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
  for (TileRt& r : s_tiles)
    if (r.def->type == TILE_CLOCK) applyClock(r);
}

// Plugs power the printers; a printer page comes and goes with its printer,
// so a change rebuilds the pages once nothing is in the user's hands.
void printerSync() {
  if (printersPowerSync(s_cfg)) s_prnRebuild = true;
  if (s_prnRebuild && !touchIsDown() && !s_ed.root && !s_modalCard && !s_qp.open) {
    bool pages = false;
    for (const PageDef& pd : s_cfg.pages) pages |= pd.printer.length() > 0;
    s_prnRebuild = false;
    if (pages) {
      stopGestures();
      destroyPages();
      buildPages();
      s_gen++;
      lv_obj_invalidate(lv_screen_active());
      return;
    }
  }
  const uint32_t v = printersVersion();
  if (v == s_prnSeen) return;
  s_prnSeen = v;
  readPrinters();
  for (TileRt& r : s_tiles) applyPrinterToTile(r);
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

  setPalette(dispSettings.theme);
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
  blinkClocks();
  qpUpdateStatus(false);
  haSync();
  httpSync();
  printerSync();
  pcCmdSync();
  pairSync();
  pcSync(false);
  syncSwipeZones();
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
  stopGestures();
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

void deckApplyTheme(uint8_t theme) {
  if (!s_disp || theme == s_theme) return;
  stopGestures();
  destroyPages();
  setPalette(theme);
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
}

void deckReloadWallpaper(bool fileChanged) {
  if (!s_disp || s_theme != THEME_GLASS) return;
  stopGestures();
  destroyPages();
  if (fileChanged) wallFree();  // else wallLoad re-blurs only when blur or dimming changed
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
}

bool deckBenchRender(int n, uint32_t& avgUs, uint32_t& maxUs) {
  if (!s_disp || !s_count) return false;
  const int a = sh.active;
  Page& pg = s_pages[a];
  if (!pg.snap) return false;
  portENTER_CRITICAL(&s_mux);
  const bool owned = sh.owned;
  if (!owned) {
    sh.busy[a] = true;
    sh.valid[a] = false;
  }
  portEXIT_CRITICAL(&s_mux);
  if (owned) return false;
  lv_obj_update_layout(pg.root);
  uint64_t sum = 0;
  maxUs = 0;
  n = constrain(n, 1, 20);
  for (int i = 0; i < n; i++) {
    const uint32_t t0 = micros();
    lv_snapshot_reshape_draw_buf(pg.root, pg.snap);
    lv_snapshot_take_to_draw_buf(pg.root, LV_COLOR_FORMAT_RGB565, pg.snap);
    const uint32_t dt = micros() - t0;
    sum += dt;
    if (dt > maxUs) maxUs = dt;
  }
  avgUs = (uint32_t)(sum / n);
  portENTER_CRITICAL(&s_mux);
  sh.busy[a] = false;
  portEXIT_CRITICAL(&s_mux);
  pg.stale = true;
  return true;
}

bool deckApplyConfig(const String& text, String& err) {
  if (!s_disp) {
    err = "display not running";
    return false;
  }
  DeckConfig next;
  if (!deckConfigStore(text, next, err)) return false;
  // Tiles point into s_cfg: tear the pages down before replacing it.
  stopGestures();
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
  stopGestures();
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

void deckReleaseIcons() {
  if (!s_disp) {
    iconsUnload();
    return;
  }
  stopGestures();
  destroyPages();
  iconsUnload();
  buildPages();
  s_gen++;
  lv_obj_invalidate(lv_screen_active());
}

String deckPrintersJson() {
  JsonDocument doc;
  JsonArray arr = doc["printers"].to<JsonArray>();
  doc["stack_free"] = printersStackFree();
  PrinterState* s = (PrinterState*)heap_caps_malloc(sizeof(PrinterState), MALLOC_CAP_SPIRAM);
  if (s) new (s) PrinterState();
  for (const PrinterDef& d : s_cfg.printers) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = d.id;
    o["secret"] = printerHasSecret(d.id);
    o["powered"] = printerPowered(d.id);
    if (!s || !printerRead(d.id, *s)) continue;
    static const char* const kLink[] = {"off", "connecting", "online", "failed"};
    o["link"] = kLink[s->link < 4 ? s->link : 0];
    o["err"] = s->err;
    o["state"] = s->gcodeState;
    o["progress"] = s->progress;
    o["dual"] = s->dualNozzle;
    o["chamber"] = isfinite(s->chamber);
    o["ams"] = s->amsCount;
  }
  heap_caps_free(s);
  String out;
  serializeJson(doc, out);
  return out;
}

void deckPrintersReapply() { printersApply(s_cfg); }

bool deckTestCard(const String& tileId) {
  if (s_ed.root) return false;  // the editor blocks gestures until it closes
  for (size_t i = 0; i < s_tiles.size(); i++) {
    if (s_tiles[i].def->id != tileId) continue;
    openModal((int)i);
    return true;
  }
  return false;
}

bool deckTestTap(const String& tileId, bool hold) {
  if (s_ed.root) return false;
  for (TileRt& r : s_tiles) {
    if (r.def->id != tileId || r.def->type == TILE_METRIC || r.def->type == TILE_CLOCK) continue;
    const TileAction& a = hold ? r.def->hold : r.def->tap;
    if (a.kind == ACT_PC) return false;  // PC commands require a real touch.
    requestAction(r, hold);
    return true;
  }
  return false;
}

bool deckTestSlide(const String& tileId, int pct) {
  for (TileRt& r : s_tiles) {
    if (r.def->id != tileId || !r.fill) continue;
    const float f = constrain(pct, 0, 100) / 100.0f;
    return setWish(r.def->entity, true, f);
  }
  return false;
}

bool deckTestConfirm(bool ok) {
  if (s_confirmHold < 0 || s_modalTile < 0 || (size_t)s_modalTile >= s_tiles.size()) return false;
  const TileDef& d = *s_tiles[s_modalTile].def;
  const TileAction& a = s_confirmHold ? d.hold : d.tap;
  if (a.kind == ACT_PC) return false;  // PC commands require a real touch.
  if (ok) onConfirmOk(nullptr);
  else closeModal();
  return true;
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
  // Picker: pick = open it on group `tile`, pickrow = add row `tile`, pickclose.
  if (op == "pick") {
    if (s_pick) pickFill(tile, x > 0 ? x : 0);  // x = first row, for paging
    else pickOpen(tile);
    return s_pick != nullptr;
  }
  if (op == "pickrow") {
    if (!s_pick || tile < 0 || tile >= (int)s_pickRows.size()) return false;
    const bool ok = edAdd(s_pickRows[tile].id, s_pickRows[tile].name);
    if (ok) pickClose();
    return ok;
  }
  if (op == "pickclose") {
    pickClose();
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

bool uiNoSwipeAt(int x, int y) {
  bool hit = false;
  portENTER_CRITICAL(&s_mux);
  const int n = sh.panelOpen || sh.modalOpen ? 0 : sh.nZones;  // those cover the tiles
  for (int i = 0; i < n && !hit; i++) {
    const Shared::Zone& z = sh.zones[i];
    hit = x >= z.x1 && x <= z.x2 && y >= z.y1 && y <= z.y2;
  }
  portEXIT_CRITICAL(&s_mux);
  return hit;
}

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
