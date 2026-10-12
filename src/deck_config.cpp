// Guition deck configuration loader. See deck_config.h for the schema.
#include "deck_config.h"
#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace deck {
namespace {

const char* const CONFIG_PATH = "/deck.json";

// Built-in starter deck: an example Home Assistant page (placeholder entity
// ids) and the PC stats from the companion's first eight metrics.
const char kDefaultDeck[] PROGMEM = R"JSON({
  "schema": 1,
  "theme": "midnight",
  "pages": [
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
        { "id": "m1", "type": "metric", "metric": 1, "style": "chart",
          "pos": { "landscape": { "x": 0, "y": 0, "w": 2, "h": 1 } } },
        { "id": "m2", "type": "metric", "metric": 2, "style": "chart",
          "pos": { "landscape": { "x": 2, "y": 0, "w": 2, "h": 1 } } },
        { "id": "m3", "type": "metric", "metric": 3 },
        { "id": "m4", "type": "metric", "metric": 4 },
        { "id": "m5", "type": "metric", "metric": 5 },
        { "id": "m6", "type": "metric", "metric": 6 },
        { "id": "m7", "type": "metric", "metric": 7, "style": "bar",
          "pos": { "landscape": { "x": 0, "y": 2, "w": 2, "h": 1 } } },
        { "id": "m8", "type": "metric", "metric": 8, "style": "bar",
          "pos": { "landscape": { "x": 2, "y": 2, "w": 2, "h": 1 } } }
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
  } else if (!strcmp(kind, "http")) {
    a.kind = ACT_HTTP;
    a.url = (const char*)(v["url"] | "");
    if (a.url.startsWith("https://")) {
      if (err) *err = where + ": https is not supported, use http on the LAN";
      return false;
    }
    if (!a.url.startsWith("http://") || a.url.length() < 8 || a.url.length() > 512) {
      if (err) *err = where + ": http action needs an http:// url (max 512 characters)";
      return false;
    }
    // A JSON body may be given as an object; it is sent as application/json.
    JsonVariantConst body = v["body"];
    if (body.is<const char*>()) a.body = (const char*)body;
    else if (!body.isNull()) {
      serializeJson(body, a.body);
      a.ctype = "application/json";
    }
    if (a.body.length() > 2048) {
      if (err) *err = where + ": http body too long (max 2048)";
      return false;
    }
    if (v["contentType"].is<const char*>()) a.ctype = (const char*)v["contentType"];
    if (!a.ctype.length() && a.body.length())
      a.ctype = a.body[0] == '{' || a.body[0] == '[' ? "application/json" : "text/plain";
    a.method = (const char*)(v["method"] | (a.body.length() ? "POST" : "GET"));
    a.method.toUpperCase();
    if (a.method != "GET" && a.method != "POST" && a.method != "PUT" && a.method != "PATCH" &&
        a.method != "DELETE") {
      if (err) *err = where + ": unknown http method '" + a.method + "'";
      return false;
    }
    // "headers": { "Name": "value" } -> "Name: value\n" lines.
    for (JsonPairConst kv : v["headers"].as<JsonObjectConst>()) {
      const String name = kv.key().c_str();
      const String val = kv.value().as<const char*>() ? kv.value().as<const char*>() : "";
      if (!name.length() || name.indexOf(':') >= 0 || (name + val).indexOf('\n') >= 0 ||
          (name + val).indexOf('\r') >= 0) {
        if (err) *err = where + ": bad http header '" + name + "'";
        return false;
      }
      a.headers += name + ": " + val + "\n";
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
  p.set = v["x"].is<int>() && v["y"].is<int>();
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
  cfg.ledEntity = (const char*)(doc["led"]["entity"] | "");
  if (cfg.ledEntity.length() && !cfg.ledEntity.startsWith("input_boolean.") &&
      !cfg.ledEntity.startsWith("input_number.") && !cfg.ledEntity.startsWith("light.")) {
    if (err) *err = "led entity must be an input_boolean, input_number or light";
    return false;
  }
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
    p.group = (const char*)(pv["group"] | "");
    const char* type = pv["type"] | "tiles";
    if (!strcmp(type, "metrics")) continue;
    if (!p.id.length()) p.id = "page" + String(cfg.pages.size() + 1);
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
      else if (!strcmp(tt, "sensor")) t.type = TILE_SENSOR;
      else if (!strcmp(tt, "metric")) t.type = TILE_METRIC;
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
      if (t.type == TILE_METRIC) {
        const int m = tv["metric"] | 0;
        if (m < 1 || m > MAX_METRICS) {
          if (err) *err = where + ": metric id must be 1.." + String(MAX_METRICS);
          return false;
        }
        t.metric = (uint8_t)m;
        const char* st = tv["style"] | "number";
        t.style = !strcmp(st, "chart") ? METRIC_CHART : !strcmp(st, "bar") ? METRIC_BAR : METRIC_NUMBER;
        t.scaleMax = (uint16_t)constrain((int)(tv["scaleMax"] | 0), 0, 65535);
        p.tiles.push_back(t);
        continue;  // read-only: no tap or hold actions
      }
      if (!parseAction(tv["tap"], t.tap, err, where + " tap")) return false;
      if (!parseAction(tv["hold"], t.hold, err, where + " hold")) return false;
      const char* cf = tv["confirm"] | "";
      if (!strcmp(cf, "off")) t.confirm = CONFIRM_OFF;
      else if (!strcmp(cf, "always")) t.confirm = CONFIRM_ALWAYS;
      else if (*cf) {
        if (err) *err = where + ": unknown confirm '" + cf + "'";
        return false;
      }
      // Defaults: a toggle toggles, anything holds to its detail card.
      if (tv["tap"].isNull() && t.type == TILE_TOGGLE) t.tap.kind = ACT_TOGGLE;
      // A read-only sensor opens its card on a tap too.
      if (tv["tap"].isNull() && t.type == TILE_SENSOR) t.tap.kind = ACT_MORE_INFO;
      if (tv["hold"].isNull()) t.hold.kind = ACT_MORE_INFO;
      p.tiles.push_back(t);
    }
    cfg.pages.push_back(p);
  }
  if (cfg.pages.empty()) {
    if (err) *err = "no pages";
    return false;
  }
  out = cfg;
  return true;
}

bool deckActionParse(const char* json, size_t len, TileAction& out, String* err) {
  JsonDocument doc;
  const DeserializationError de = deserializeJson(doc, json, len);
  if (de) {
    if (err) *err = String("JSON: ") + de.c_str();
    return false;
  }
  return parseAction(doc.as<JsonVariantConst>(), out, err, "action");
}

bool deckFsReady() { return mountFs(); }

void deckConfigText(String& out) {
  if (mountFs() && LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (f) {
      out = f.readString();
      f.close();
      DeckConfig probe;
      if (deckConfigParse(out.c_str(), out.length(), probe, nullptr)) return;
    }
  }
  out = FPSTR(kDefaultDeck);
}

bool deckConfigStore(const String& text, DeckConfig& out, String& err) {
  DeckConfig cfg;
  if (!deckConfigParse(text.c_str(), text.length(), cfg, &err)) return false;
  if (!mountFs()) {
    err = "storage not available";
    return false;
  }
  const char* tmp = "/deck.json.tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    err = "cannot write the file";
    return false;
  }
  const size_t n = f.print(text);
  f.close();
  if (n != text.length()) {
    LittleFS.remove(tmp);
    err = "storage full";
    return false;
  }
  LittleFS.remove(CONFIG_PATH);
  if (!LittleFS.rename(tmp, CONFIG_PATH)) {
    err = "cannot replace the file";
    return false;
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
