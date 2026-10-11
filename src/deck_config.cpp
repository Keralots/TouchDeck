// Guition deck configuration loader. See deck_config.h for the schema.
#if defined(BOARD_IS_JC3248W535)

#include "deck_config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace deck {
namespace {

const char* const CONFIG_PATH = "/deck.json";

// Built-in starter deck: the metric page plus two example pages. The HA
// entity ids are placeholders until the portal editor (Phase 4) lets the
// user pick real ones.
const char kDefaultDeck[] PROGMEM = R"JSON({
  "schema": 1,
  "theme": "midnight",
  "pages": [
    { "id": "pc-stats", "type": "metrics" },
    { "id": "desk", "name": "Desk",
      "layouts": { "landscape": { "cols": 4, "rows": 3 } },
      "tiles": [
        { "id": "lamp", "type": "toggle", "entity": "light.desk_lamp", "label": "Desk lamp", "icon": "mdi:lightbulb",
          "pos": { "landscape": { "x": 0, "y": 0, "w": 2, "h": 1 } } },
        { "id": "ceiling", "type": "toggle", "entity": "light.ceiling", "label": "Ceiling", "icon": "mdi:ceiling-light" },
        { "id": "bias", "type": "toggle", "entity": "light.bias", "label": "Bias light", "icon": "mdi:television-ambient-light" },
        { "id": "fan", "type": "toggle", "entity": "fan.desk_fan", "label": "Fan", "icon": "mdi:fan" },
        { "id": "evening", "type": "action", "entity": "scene.evening", "label": "Evening", "icon": "mdi:weather-night",
          "tap": { "action": "call-service", "domain": "scene", "service": "turn_on" } },
        { "id": "focus", "type": "action", "entity": "scene.focus", "label": "Focus", "icon": "mdi:eye" },
        { "id": "printer", "type": "toggle", "entity": "switch.printer", "label": "Printer", "icon": "mdi:printer-3d" },
        { "id": "speakers", "type": "toggle", "entity": "switch.speakers", "label": "Speakers", "icon": "mdi:speaker" },
        { "id": "door", "type": "toggle", "entity": "lock.front_door", "label": "Front door", "icon": "mdi:lock" },
        { "id": "alloff", "type": "action", "entity": "script.all_off", "label": "All off", "icon": "mdi:power", "color": "#FF8A7A" },
        { "id": "topc", "type": "action", "label": "PC", "sub": "Page", "icon": "mdi:chevron-right",
          "tap": { "action": "page", "page": "pc" } }
      ] },
    { "id": "pc", "name": "PC",
      "layouts": { "landscape": { "cols": 4, "rows": 3 } },
      "tiles": [
        { "id": "mute", "type": "action", "label": "Mute mic", "sub": "Hotkey", "icon": "mdi:microphone-off" },
        { "id": "shot", "type": "action", "label": "Screenshot", "sub": "Hotkey", "icon": "mdi:image" },
        { "id": "prev", "type": "action", "label": "Previous", "sub": "Media", "icon": "mdi:skip-previous" },
        { "id": "play", "type": "action", "label": "Play", "sub": "Media", "icon": "mdi:play" },
        { "id": "next", "type": "action", "label": "Next", "sub": "Media", "icon": "mdi:skip-next" },
        { "id": "lock", "type": "action", "label": "Lock PC", "sub": "Win+L", "icon": "mdi:lock" },
        { "id": "term", "type": "action", "label": "Terminal", "sub": "Companion", "icon": "mdi:console" },
        { "id": "vol", "type": "action", "label": "Volume", "sub": "Media", "icon": "mdi:volume-high",
          "pos": { "landscape": { "x": 3, "y": 1, "w": 1, "h": 2 } } },
        { "id": "sleep", "type": "action", "label": "Sleep PC", "sub": "Companion", "icon": "mdi:power-sleep", "color": "#FF8A7A" },
        { "id": "todesk", "type": "action", "label": "Desk", "sub": "Page", "icon": "mdi:chevron-left",
          "tap": { "action": "page", "page": "desk" } }
      ] }
  ]
})JSON";

bool parseColor(const char* s, uint32_t& out) {
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  char* end = nullptr;
  const unsigned long v = strtoul(s + 1, &end, 16);
  if (!end || *end) return false;
  out = (uint32_t)v;
  return true;
}

bool parseAction(JsonVariantConst v, TileAction& a, String* err, const String& where) {
  a = TileAction();
  if (v.isNull()) return true;
  const char* kind = v["action"] | "none";
  if (!strcmp(kind, "none")) {
    a.kind = ACT_NONE;
  } else if (!strcmp(kind, "toggle")) {
    a.kind = ACT_TOGGLE;
  } else if (!strcmp(kind, "more-info")) {
    a.kind = ACT_MORE_INFO;
  } else if (!strcmp(kind, "page")) {
    a.kind = ACT_PAGE;
    a.page = (const char*)(v["page"] | "");
    if (!a.page.length()) {
      if (err) *err = where + ": page action without a page id";
      return false;
    }
  } else if (!strcmp(kind, "call-service")) {
    a.kind = ACT_CALL_SERVICE;
    a.domain = (const char*)(v["domain"] | "");
    a.service = (const char*)(v["service"] | "");
    if (!a.domain.length() || !a.service.length()) {
      if (err) *err = where + ": call-service needs domain and service";
      return false;
    }
    if (!v["data"].isNull()) serializeJson(v["data"], a.data);
  } else {
    if (err) *err = where + ": unknown action '" + kind + "'";
    return false;
  }
  return true;
}

void parsePos(JsonVariantConst v, TilePos& p) {
  if (v.isNull()) return;
  p.set = true;
  p.x = (uint8_t)constrain((int)(v["x"] | 0), 0, 15);
  p.y = (uint8_t)constrain((int)(v["y"] | 0), 0, 15);
  p.w = (uint8_t)constrain((int)(v["w"] | 1), 1, 6);
  p.h = (uint8_t)constrain((int)(v["h"] | 1), 1, 6);
}

void parseGrid(JsonVariantConst v, uint8_t& cols, uint8_t& rows) {
  if (v.isNull()) return;
  cols = (uint8_t)constrain((int)(v["cols"] | cols), 1, 6);
  rows = (uint8_t)constrain((int)(v["rows"] | rows), 1, 6);
}

bool s_fsTried = false;
bool s_fsOk = false;

bool mountFs() {
  if (!s_fsTried) {
    s_fsTried = true;
    // The 16 MB table's "spiffs" partition has never been used on this
    // board, so formatting it on the first mount loses nothing.
    s_fsOk = LittleFS.begin(true, "/littlefs", 5, "spiffs");
    if (!s_fsOk) Serial.println("Deck: LittleFS mount FAILED");
  }
  return s_fsOk;
}

}  // namespace

bool deckConfigParse(const char* json, size_t len, DeckConfig& out, String* err) {
  JsonDocument doc;
  const DeserializationError de = deserializeJson(doc, json, len);
  if (de) {
    if (err) *err = String("JSON: ") + de.c_str();
    return false;
  }
  DeckConfig cfg;
  cfg.schema = doc["schema"] | 0;
  if (cfg.schema != 1) {
    if (err) *err = "unsupported schema (expected 1)";
    return false;
  }
  cfg.theme = (const char*)(doc["theme"] | "midnight");
  JsonArrayConst pages = doc["pages"].as<JsonArrayConst>();
  if (pages.isNull() || pages.size() == 0) {
    if (err) *err = "no pages";
    return false;
  }
  if (pages.size() > MAX_DECK_PAGES) {
    if (err) *err = "too many pages (max " + String(MAX_DECK_PAGES) + ")";
    return false;
  }
  for (JsonVariantConst pv : pages) {
    PageDef p;
    p.id = (const char*)(pv["id"] | "");
    p.name = (const char*)(pv["name"] | "");
    const char* type = pv["type"] | "tiles";
    p.metrics = !strcmp(type, "metrics");
    if (!p.id.length()) p.id = "page" + String(cfg.pages.size() + 1);
    if (p.metrics) {
      cfg.pages.push_back(p);
      continue;
    }
    parseGrid(pv["layouts"]["landscape"], p.cols[ORIENT_LANDSCAPE], p.rows[ORIENT_LANDSCAPE]);
    parseGrid(pv["layouts"]["portrait"], p.cols[ORIENT_PORTRAIT], p.rows[ORIENT_PORTRAIT]);
    JsonArrayConst tiles = pv["tiles"].as<JsonArrayConst>();
    if (tiles.size() > MAX_PAGE_TILES) {
      if (err) *err = "page '" + p.id + "': too many tiles";
      return false;
    }
    for (JsonVariantConst tv : tiles) {
      TileDef t;
      t.id = (const char*)(tv["id"] | "");
      const String where = "page '" + p.id + "' tile '" + t.id + "'";
      const char* tt = tv["type"] | "action";
      if (!strcmp(tt, "toggle")) t.type = TILE_TOGGLE;
      else if (!strcmp(tt, "action")) t.type = TILE_ACTION;
      else {
        if (err) *err = where + ": unknown type '" + tt + "'";
        return false;
      }
      t.entity = (const char*)(tv["entity"] | "");
      t.label = (const char*)(tv["label"] | "");
      t.sub = (const char*)(tv["sub"] | "");
      t.icon = (const char*)(tv["icon"] | "");
      t.hasColor = parseColor(tv["color"] | (const char*)nullptr, t.color);
      parsePos(tv["pos"]["landscape"], t.pos[ORIENT_LANDSCAPE]);
      parsePos(tv["pos"]["portrait"], t.pos[ORIENT_PORTRAIT]);
      if (!parseAction(tv["tap"], t.tap, err, where + " tap")) return false;
      if (!parseAction(tv["hold"], t.hold, err, where + " hold")) return false;
      // Defaults: a toggle toggles, anything holds to its detail card.
      if (tv["tap"].isNull() && t.type == TILE_TOGGLE) t.tap.kind = ACT_TOGGLE;
      if (tv["hold"].isNull()) t.hold.kind = ACT_MORE_INFO;
      p.tiles.push_back(t);
    }
    cfg.pages.push_back(p);
  }
  out = cfg;
  return true;
}

bool deckConfigLoad(DeckConfig& out, String* source, String* err) {
  if (err) *err = "";
  if (mountFs() && LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (f) {
      String text = f.readString();
      f.close();
      String why;
      if (deckConfigParse(text.c_str(), text.length(), out, &why)) {
        if (source) *source = CONFIG_PATH;
        return true;
      }
      if (err) *err = why;
      Serial.printf("Deck: %s rejected (%s), using the built-in deck\n", CONFIG_PATH, why.c_str());
    }
  }
  if (source) *source = "built-in";
  return deckConfigParse(kDefaultDeck, strlen_P(kDefaultDeck), out, nullptr);
}

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
