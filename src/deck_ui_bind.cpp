// Deck UI: tiles bound to Home Assistant, HTTP sensors and the PC; tile actions.
#include "deck_ui_int.h"

namespace deckui {

// --- Tile events ------------------------------------------------------------------------

const char* pcCmdLabel(const String& cmd);

uint32_t tileTextRt(const TileRt& r) { return r.hasAuto ? r.autoText : tileText(*r.def); }

// onTile: the reading on the tile itself, which follows Glass auto text.
uint32_t sensorColour(const TileRt& r, bool onTile) {
  return r.threshold == 2 ? C_CRIT : r.threshold == 1 ? C_WARN : onTile ? tileTextRt(r) : tileText(*r.def);
}

uint32_t sensorLine(const TileRt& r) {
  const TileDef& d = *r.def;
  return r.threshold ? sensorColour(r) : d.hasChartColor ? d.chartColor :
         d.hasColor ? d.color : domainColor(entityDomain(d.entity));
}

// 0 = fine, 1 = warn, 2 = crit; prev is the level shown now (hysteresis).
uint8_t thresholdLevel(const TileDef& d, float value, uint8_t prev) {
  const bool low = d.hasWarn && d.hasCrit && d.crit < d.warn;
  auto active = [&](float threshold, bool held) {
    const float margin = held ? max(0.01f, fabsf(threshold) * 0.01f) : 0;
    return low ? (held ? value < threshold + margin : value <= threshold) :
                 (held ? value > threshold - margin : value >= threshold);
  };
  if (d.hasCrit && active(d.crit, prev == 2)) return 2;
  if (d.hasWarn && active(d.warn, prev >= 1)) return 1;
  return 0;
}

void sensorThreshold(TileRt& r, bool numeric, float value) {
  r.threshold = numeric ? thresholdLevel(*r.def, value, r.threshold) : 0;
  lv_obj_set_style_text_color(r.sub, lv_color_hex(sensorColour(r, true)), 0);
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

// A temperature as "21.5°" (whole numbers without the decimal).
String formatTemp(float v) {
  if (!isfinite(v)) return "--";
  char buf[16];
  formatReading(v, buf, sizeof(buf));
  return String(buf) + "\xC2\xB0";
}

// HVAC modes and actions as words: "heat_cool" -> "Heat/Cool".
String climateWord(const String& s) {
  if (s == "heat_cool") return "Heat/Cool";
  if (s == "fan_only") return "Fan";
  String w = s;
  w.replace('_', ' ');
  if (w.length()) w.setCharAt(0, toupper(w[0]));
  return w;
}

bool climateRange(const HaEntity& e) {
  return !isfinite(e.target) && isfinite(e.targetLow) && isfinite(e.targetHigh);
}

// The setpoint the entity reports: "21.5°", "19-24°", or "" without one.
String climateTarget(const HaEntity& e) {
  if (isfinite(e.target)) return formatTemp(e.target);
  if (climateRange(e)) {
    char lo[16];
    formatReading(e.targetLow, lo, sizeof(lo));
    return String(lo) + "-" + formatTemp(e.targetHigh);
  }
  return "";
}

String prettyState(const String& domain, const HaEntity& e) {
  if (!e.known) return "Unavailable";
  const String& st = e.state;
  if (st == "unavailable" || st == "unknown") return "Unavailable";
  if (domain == "climate") {
    // "Heat 21.5°": the mode, then the setpoint while it is not off.
    const String t = st == "off" ? String() : climateTarget(e);
    return t.length() ? climateWord(st) + " " + t : climateWord(st);
  }
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

// e: the tile's entity if the caller has it, else looked up when needed.
void setTileOn(TileRt& r, bool on, const HaEntity* e) {
  const bool was = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  r.on = on;
  if (was != on) fxBeforeTileOn(r, on);
  if (on) lv_obj_add_state(r.obj, LV_STATE_CHECKED);
  else lv_obj_remove_state(r.obj, LV_STATE_CHECKED);
  if (was != on && r.def->onIcon.length() && r.icon) {
    HaEntity own;
    if (!e && r.def->entity.length() && haGetEntity(r.def->entity, own)) e = &own;
    r.iconCp = showIcon(r.icon, *r.def, e, on, r.iconPx, r.iconCp);
  }
  tileIconColor(r, e);
  fxTileOn(r, was);
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

bool setWish(const String& entity, bool level, float value, bool dragging) {
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
  if (d.type == TILE_MEDIA || d.http.enabled || !d.entity.length()) return;
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
  if (d.type == TILE_TOGGLE) setTileOn(r, on, &e);
  if (r.icon) r.iconCp = showIcon(r.icon, d, &e, on, r.iconPx, r.iconCp);
  if (d.lightIcon && d.type != TILE_TOGGLE) tileIconColor(r, &e);
  lv_obj_set_style_opa(r.obj, avail ? LV_OPA_COVER : LV_OPA_50, 0);
  if (!d.label.length() && !d.noName && e.name.length() && strcmp(lv_label_get_text(r.name), e.name.c_str())) {
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
    if (o.ok && o.cmd.startsWith("volume_set:")) continue;  // the slider is the feedback
    const String t = o.pcName + ": " + pcCmdLabel(o.cmd) + (o.ok ? "" : String(" - ") + (o.msg.length() ? o.msg : "failed"));
    deckToastShow(t.c_str(), o.ok ? 1200 : 2500);
  }
}

uint32_t s_httpSeen = 0;
uint32_t s_prnSeen = 0;  // printersVersion() last applied
bool s_prnRebuild = false;
String s_keepPage;  // buildPages: show this page again (set by rebuildPages)

// "when" tiles. A PC counts as off only after this long offline, on top of
// PC_STALE_TIMEOUT_MS, so a companion restart does not reshuffle the page.
constexpr uint32_t WHEN_PC_GRACE_MS = 45000;
struct WhenPc {
  String id;  // "" = the first known PC
  bool on = false;
  uint32_t offSince = 0;
};
std::vector<WhenPc> s_whenPcs;
std::vector<std::pair<String, String>> s_whenStates;  // entity, last state HA reported

const String& tilePcId(const TileDef& d) { return d.tap.kind == ACT_PC ? d.tap.pc : d.pc; }

bool pcIsOnline(const String& id) {
  const PcSource* src = pcSource(id);
  return src && src->data.online;
}

// pcId: the PC a pc-on / pc-off condition follows ("" = the first known one).
bool whenWanted(const WhenDef& w, const String& pcId) {
  if (w.kind == WHEN_PC_ON || w.kind == WHEN_PC_OFF) {
    bool on = false;
    for (const WhenPc& p : s_whenPcs)
      if (p.id == pcId) on = p.on;
    return on == (w.kind == WHEN_PC_ON);
  }
  if (w.kind == WHEN_ENTITY) {
    for (const auto& s : s_whenStates)
      if (s.first == w.entity) return s.second.length() && (s.second == w.state) != w.negate;
    return false;
  }
  return true;
}

bool whenWanted(const TileDef& t) { return whenWanted(t.when, tilePcId(t)); }
bool whenWanted(const PageDef& p) { return whenWanted(p.when, String()); }

void whenWatch(const WhenDef& w, const String& pcId, std::vector<WhenPc>& pcs,
               std::vector<std::pair<String, String>>& states) {
  if (w.kind == WHEN_ENTITY) {
    for (const auto& s : states)
      if (s.first == w.entity) return;
    String last;
    for (const auto& s : s_whenStates)
      if (s.first == w.entity) last = s.second;
    HaEntity e;
    if (!last.length() && haGetEntity(w.entity, e) && e.known) last = e.state;
    states.push_back({w.entity, last});
  } else if (w.kind != WHEN_ALWAYS) {
    for (const WhenPc& p : pcs)
      if (p.id == pcId) return;
    WhenPc n;
    n.id = pcId;
    n.on = pcIsOnline(pcId);
    for (const WhenPc& p : s_whenPcs)
      if (p.id == pcId) n = p;
    pcs.push_back(n);
  }
}

// The PCs and entities the "when" tiles and pages watch, keeping what is already known.
void whenCollect() {
  std::vector<WhenPc> pcs;
  std::vector<std::pair<String, String>> states;
  for (const PageDef& pd : s_cfg.pages) {
    whenWatch(pd.when, String(), pcs, states);
    for (const TileDef& t : pd.tiles) whenWatch(t.when, tilePcId(t), pcs, states);
  }
  s_whenPcs.swap(pcs);
  s_whenStates.swap(states);
}

void whenTrack() {
  const uint32_t now = millis();
  for (WhenPc& p : s_whenPcs) {
    if (pcIsOnline(p.id)) {
      p.on = true;
      p.offSince = 0;
    } else if (p.on) {
      if (!p.offSince) p.offSince = now | 1;
      else if (now - p.offSince >= WHEN_PC_GRACE_MS) p.on = false;
    }
  }
  // While HA is away the last known states stand.
  if (haLink() != HA_CONNECTED) return;
  for (auto& s : s_whenStates) {
    HaEntity e;
    if (haGetEntity(s.first, e) && e.known) s.second = e.state;
  }
}

// The layout follows the conditions as they are now.
void whenApply() {
  whenCollect();
  for (PageDef& pd : s_cfg.pages) {
    pd.shown = whenWanted(pd);
    for (TileDef& t : pd.tiles) t.shown = whenWanted(t);
  }
}

// A template (detail line, row value, name): literal text and fields from the
// same reply ("--" while missing).
String httpTemplate(const TileDef& d, const String& tmpl) {
  String out;
  for (const DetailPart& part : httpParseDetail(tmpl)) {
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

// Rows put "%" and "°" right after the number ("8%").
String rowUnit(const TileDef& d) {
  if (!d.unit.length()) return "";
  return d.unit.startsWith("%") || d.unit.startsWith("\xC2\xB0") ? d.unit : " " + d.unit;
}

// Row i's value text: its "value" template when asked for, else the reading.
String rowText(const TileDef& d, size_t i, bool useTemplate) {
  const SensorRowDef& row = d.rows[i];
  if (useTemplate && row.value.length()) return httpTemplate(d, row.value);
  HttpSensorValue v;
  if (!httpSensorRead(d.http, row.path, v) || v.offline || !v.present) return "--";
  if (!v.numeric) return v.text ? v.text.get() : "--";
  char number[48];
  if (d.duration && formatDuration(v.number, d.unit, number, sizeof(number))) return number;
  formatReading(v.number, number, sizeof(number));
  return String(number) + rowUnit(d);
}

// Each row box holds label, bar, value; its user data is the threshold level.
// layoutRows sets the value texts.
void applyHttpRows(TileRt& r) {
  const TileDef& d = *r.def;
  const float full = d.scaleMax ? d.scaleMax : 100;
  const uint32_t line = d.hasChartColor ? d.chartColor : d.hasColor ? d.color : domainColor(entityDomain(d.entity));
  bool dim = true;
  for (size_t i = 0; i < d.rows.size() && i < lv_obj_get_child_count(r.rows); i++) {
    lv_obj_t* box = lv_obj_get_child(r.rows, i);
    lv_obj_t* value = lv_obj_get_child(box, 2);
    HttpSensorValue v;
    const bool found = httpSensorRead(d.http, d.rows[i].path, v);
    dim = dim && (!found || v.offline);
    const bool numeric = found && !v.offline && v.present && v.numeric;
    const uint8_t level = numeric ? thresholdLevel(d, v.number, (uint8_t)(uintptr_t)lv_obj_get_user_data(box)) : 0;
    lv_obj_set_user_data(box, (void*)(uintptr_t)level);
    // Level 0 inherits the tile text colour, so Glass auto text reaches it.
    if (level) lv_obj_set_style_text_color(value, lv_color_hex(level == 2 ? C_CRIT : C_WARN), 0);
    else lv_obj_remove_local_style_prop(value, LV_STYLE_TEXT_COLOR, 0);
    lv_obj_t* bar = lv_obj_get_child(box, 1);
    lv_bar_set_value(bar, numeric ? (int32_t)lround(constrain(v.number * 100 / full, 0.0f, 100.0f)) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(level == 2 ? C_CRIT : level ? C_WARN : line), LV_PART_INDICATOR);
  }
  lv_obj_set_style_opa(r.obj, dim ? LV_OPA_50 : LV_OPA_COVER, 0);
  layoutRows(r);
  markPageStale(r.page);
}

void applyHttpToTile(TileRt& r) {
  const TileDef& d = *r.def;
  if (!d.http.enabled) return;
  if (d.label.indexOf('{') >= 0) setText(r.name, httpTemplate(d, d.label).c_str());
  if (r.rows) {
    applyHttpRows(r);
    return;
  }
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
  if (r.detail) lv_label_set_text(r.detail, httpTemplate(d, d.detail).c_str());
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

// Every HA entity the deck follows: tiles, printer plugs, conditions, LED, presence.
std::vector<String> entityIds() {
  std::vector<String> ids;
  for (const TileRt& r : s_tiles)
    if (r.def->entity.length()) ids.push_back(r.def->entity);
  for (const PrinterDef& d : s_cfg.printers)
    if (advSettings.printers && d.plug.length()) ids.push_back(d.plug);
  for (const auto& s : s_whenStates) ids.push_back(s.first);
  if (s_cfg.ledEntity.length()) ids.push_back(s_cfg.ledEntity);
  for (const String& id : presenceSettings.entities)
    if (id.length()) ids.push_back(id);
  return ids;
}

void collectEntities() {
  printersApply(s_cfg);
  s_prnSeen = 0;
  httpSetSensors(s_cfg);
  s_httpSeen = 0;
  for (TileRt& r : s_tiles) applyHttpToTile(r);
  static bool hadLedEntity = false;
  const bool hasLedEntity = s_cfg.ledEntity.length() > 0;
  if (!hasLedEntity && hadLedEntity) ledSetHaLevel(LED_HA_NONE);
  hadLedEntity = hasLedEntity;
  s_ledSeen = 0;
  haSetEntities(entityIds());
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
  if (cmd.startsWith("hotkey:")) return "Hotkey";
  if (cmd.startsWith("volume_set:")) return "Volume";
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
  hapticTap();
  if (code == LV_EVENT_SHORT_CLICKED) requestAction(r, false);
  else if (code == LV_EVENT_LONG_PRESSED) requestAction(r, true);
}

// PC volume slider: the level the finger asks for goes out at most every
// LEVEL_GAP_MS (each command spends a companion nonce); the tile shows it
// until PC_VOL_HOLD_MS after the last move, then follows the PC again.
constexpr uint32_t PC_VOL_HOLD_MS = 1500;
struct PcVolume {
  String pc;
  int want = -1, sent = -1;
  uint32_t sentMs = 0, holdUntil = 0;
  bool reapply = false;
};
PcVolume s_pcVol;

bool pcVolHeld() { return (int32_t)(millis() - s_pcVol.holdUntil) < 0; }

void pcVolumeTick() {
  const uint32_t now = millis();
  if (s_pcVol.want >= 0 && s_pcVol.want != s_pcVol.sent && now - s_pcVol.sentMs >= LEVEL_GAP_MS) {
    String err;
    s_pcVol.sentMs = now;
    if (pcSendCommand(s_pcVol.pc, ("volume_set:" + String(s_pcVol.want)).c_str(), err)) s_pcVol.sent = s_pcVol.want;
    else if (!pcVolHeld()) {
      s_pcVol.want = -1;
      deckToastShow(err.c_str(), 2200);
    }
  }
  if (s_pcVol.reapply && !pcVolHeld()) {
    s_pcVol.reapply = false;
    s_pcVol.want = s_pcVol.sent = -1;
    for (TileRt& r : s_tiles)
      if (r.def->pcVolume && !r.sliding) applyPcStateToTile(r);
  }
}

void pcVolumeDrag(TileRt& r, lv_event_code_t code, const lv_point_t& p) {
  if (code == LV_EVENT_PRESSING) {
    if (!r.sliding) {
      if (!pcIsOnline(tilePcId(*r.def)) || modalShown() || abs(p.x - r.slX0) < 8) return;
      r.sliding = true;
      r.slX0 = p.x;
      r.slFrac0 = r.slValue = r.frac;
    }
    const int w = lv_obj_get_width(r.obj);
    const float f = constrain(r.slFrac0 + (p.x - r.slX0) / (float)(w > 0 ? w : 1), 0.0f, 1.0f);
    if (fabsf(f - r.slValue) < 0.005f) return;
    r.slValue = f;
    setSliderFill(r, f);
    s_pcVol.pc = tilePcId(*r.def);
    s_pcVol.want = (int)lroundf(f * 100);
    if (!r.def->sub.length()) lv_label_set_text(r.sub, (String(s_pcVol.want) + "%").c_str());
  } else if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
    return;
  }
  if (!r.sliding) return;
  s_pcVol.holdUntil = millis() + PC_VOL_HOLD_MS;
  s_pcVol.reapply = true;
  markPageStale(r.page);
}

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
  if (r.def->pcVolume) {
    pcVolumeDrag(r, code, p);
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

}  // namespace deckui
