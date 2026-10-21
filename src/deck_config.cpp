// Guition deck configuration loader. See deck_config.h for the schema.
#include "deck_config.h"
#include "deck_bambu.h"
#include "config.h"
#include "psram_json.h"
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

bool parseHttp(JsonVariantConst v, TileAction& a, String* err, const String& where) {
  a.url = (const char*)(v["url"] | "");
  if (a.url.startsWith("https://")) {
    if (err) *err = where + ": https is not supported, use http on the LAN";
    return false;
  }
  if (!a.url.startsWith("http://") || a.url.length() < 8 || a.url.length() > 512) {
    if (err) *err = where + ": http needs an http:// url (max 512 characters)";
    return false;
  }
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
    if (!parseHttp(v, a, err, where)) return false;
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
  } else if (!strcmp(kind, "pc")) {
    static const char* const kCmds[] = {"media_play_pause", "media_next", "media_prev", "media_stop",
                                        "volume_up", "volume_down", "volume_mute", "lock", "sleep",
                                        "restart", "shutdown", "hotkey"};
    a.kind = ACT_PC;
    a.pc = (const char*)(v["pc"] | "");
    a.cmd = (const char*)(v["cmd"] | "");
    bool known = false;
    for (const char* c : kCmds) known = known || a.cmd == c;
    if (!known) {
      if (err) *err = where + ": unknown PC command '" + a.cmd + "'";
      return false;
    }
    if (a.cmd == "hotkey") {
      // Sent as "hotkey:ctrl+shift+m"; the companion checks the key names.
      String keys = (const char*)(v["keys"] | "");
      keys.toLowerCase();
      bool ok = keys.length() > 0 && keys.length() <= 64;
      for (const char* k = keys.c_str(); *k; k++) ok = ok && (isalnum((unsigned char)*k) || *k == '+');
      if (!ok) {
        if (err) *err = where + ": hotkey keys must look like \"ctrl+shift+m\"";
        return false;
      }
      a.cmd += ":" + keys;
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

// A number of cells in half cells (1.5 -> 3), rounded to the nearest half.
uint8_t halves(JsonVariantConst v, int def, int lo, int hi) {
  if (!v.is<float>()) return (uint8_t)def;
  return (uint8_t)constrain((int)lroundf(v.as<float>() * 2), lo, hi);
}

void parsePos(JsonVariantConst v, TilePos& p) {
  if (v.isNull()) return;
  p.set = v["x"].is<float>() && v["y"].is<float>();
  p.x = halves(v["x"], 0, 0, 30);
  p.y = halves(v["y"], 0, 0, 30);
  p.w = halves(v["w"], 2, 1, 12);
  p.h = halves(v["h"], 2, 1, 12);
}

bool parseWhen(JsonVariantConst v, WhenDef& w, String* err, const String& where) {
  if (v.isNull()) return true;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!strcmp(s, "pc-on")) w.kind = WHEN_PC_ON;
    else if (!strcmp(s, "pc-off")) w.kind = WHEN_PC_OFF;
    else {
      if (err) *err = where + ": unknown when '" + s + "'";
      return false;
    }
    return true;
  }
  const char* entity = v["entity"] | "";
  const char* state = v["state"] | "";
  if (!strchr(entity, '.') || state[0] == '\0' || !strcmp(state, "!")) {
    if (err) *err = where + ": when needs an entity and a state";
    return false;
  }
  w.kind = WHEN_ENTITY;
  w.entity = entity;
  w.negate = state[0] == '!';
  w.state = state + (w.negate ? 1 : 0);
  return true;
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
    s_fsOk = LittleFS.begin(true, "/littlefs", 10, "spiffs");  // 4 held by icon fonts
    if (!s_fsOk) Serial.println("Deck: LittleFS mount FAILED");
  }
  return s_fsOk;
}

}  // namespace

bool deckConfigParse(const char* json, size_t len, DeckConfig& out, String* err) {
  JsonDocument doc(psramJson());
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
  JsonObjectConst pres = doc["presence"].as<JsonObjectConst>();
  if (pres) {
    for (JsonVariantConst v : pres["entities"].as<JsonArrayConst>()) {
      const String id = (const char*)(v | "");
      if (id.indexOf('.') <= 0 || cfg.presence.entities.size() >= MAX_PRESENCE_ENTITIES) {
        if (err) *err = "presence: up to 4 entity ids";
        return false;
      }
      cfg.presence.entities.push_back(id);
    }
    cfg.presence.pc = pres["pc"] | false;
    const int sleep = pres["sleep"] | 0, dim = pres["dim"] | 0;
    if (sleep < 0 || sleep > 1440 || dim < 0 || dim > 100) {
      if (err) *err = "presence: sleep 0..1440 min, dim 0..100 %";
      return false;
    }
    cfg.presence.sleepMin = sleep;
    cfg.presence.dimPct = dim;
  }
  auto printerKnown = [&](const String& id) {
    for (const PrinterDef& d : cfg.printers)
      if (d.id == id) return true;
    return false;
  };
  for (JsonVariantConst pv : doc["printers"].as<JsonArrayConst>()) {
    PrinterDef d;
    d.id = (const char*)(pv["id"] | "");
    d.name = (const char*)(pv["name"] | "");
    d.ip = (const char*)(pv["ip"] | "");
    d.plug = (const char*)(pv["plug"] | "");
    // The id names NVS keys ("s_<id>"): short and plain.
    bool plain = d.id.length() >= 1 && d.id.length() <= 12;
    for (size_t i = 0; plain && i < d.id.length(); i++)
      plain = isalnum((unsigned char)d.id[i]) || d.id[i] == '_';
    if (!plain || printerKnown(d.id)) {
      if (err) *err = "printer id '" + d.id + "' must be unique, 1-12 letters, digits or _";
      return false;
    }
    if (!d.ip.length()) {
      if (err) *err = "printer '" + d.id + "' needs an ip";
      return false;
    }
    if (d.plug.length() && !d.plug.startsWith("switch.") && !d.plug.startsWith("input_boolean.")) {
      if (err) *err = "printer '" + d.id + "' plug must be a switch or input_boolean";
      return false;
    }
    if (cfg.printers.size() >= MAX_PRINTERS) {
      if (err) *err = "too many printers (max " + String(MAX_PRINTERS) + ")";
      return false;
    }
    if (!d.name.length()) d.name = d.id;
    cfg.printers.push_back(d);
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
    p.printer = (const char*)(pv["printer"] | "");
    if (p.printer.length() && !printerKnown(p.printer)) {
      if (err) *err = "page '" + p.id + "': unknown printer '" + p.printer + "'";
      return false;
    }
    const char* type = pv["type"] | "tiles";
    if (!strcmp(type, "metrics")) continue;
    if (!p.id.length()) p.id = "page" + String(cfg.pages.size() + 1);
    if (!parseWhen(pv["when"], p.when, err, "page '" + p.id + "'")) return false;
    p.autoShow = p.when.kind != WHEN_ALWAYS && (pv["auto"] | false);
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
      else if (!strcmp(tt, "slider")) t.type = TILE_SLIDER;
      else if (!strcmp(tt, "metric")) t.type = TILE_METRIC;
      else if (!strcmp(tt, "clock")) t.type = TILE_CLOCK;
      else if (!strcmp(tt, "media")) t.type = TILE_MEDIA;
      else if (!strcmp(tt, "printer")) t.type = TILE_PRINTER;
      else {
        if (err) *err = where + ": unknown type '" + tt + "'";
        return false;
      }
      t.entity = (const char*)(tv["entity"] | "");
      t.label = (const char*)(tv["label"] | "");
      t.sub = (const char*)(tv["sub"] | "");
      t.icon = (const char*)(tv["icon"] | "");
      t.hasColor = parseColor(tv["color"] | (const char*)nullptr, t.color);
      t.hasTextColor = parseColor(tv["textColor"] | (const char*)nullptr, t.textColor);
      t.hasChartColor = parseColor(tv["chartColor"] | (const char*)nullptr, t.chartColor);
      t.hasBgColor = parseColor(tv["bgColor"] | (const char*)nullptr, t.bgColor);
      t.hasOnColor = parseColor(tv["onColor"] | (const char*)nullptr, t.onColor);
      t.hasOnTextColor = parseColor(tv["onTextColor"] | (const char*)nullptr, t.onTextColor);
      if (tv["opacity"].is<int>()) t.opacity = (int8_t)constrain(tv["opacity"].as<int>(), 0, 100);
      if (tv["onOpacity"].is<int>()) t.onOpacity = (int8_t)constrain(tv["onOpacity"].as<int>(), 0, 100);
      parsePos(tv["pos"]["landscape"], t.pos[ORIENT_LANDSCAPE]);
      parsePos(tv["pos"]["portrait"], t.pos[ORIENT_PORTRAIT]);
      if (!parseWhen(tv["when"], t.when, err, where)) return false;
      if (t.type == TILE_METRIC) {
        const int m = tv["metric"] | 0;
        if (m < 1 || m > MAX_METRICS) {
          if (err) *err = where + ": metric id must be 1.." + String(MAX_METRICS);
          return false;
        }
        t.metric = (uint8_t)m;
        t.pc = (const char*)(tv["pc"] | "");
        const char* st = tv["style"] | "number";
        t.style = !strcmp(st, "chart") ? METRIC_CHART : !strcmp(st, "bar") ? METRIC_BAR : METRIC_NUMBER;
        t.scaleMax = (uint16_t)constrain((int)(tv["scaleMax"] | 0), 0, 65535);
        p.tiles.push_back(t);
        continue;  // read-only: no tap or hold actions
      }
      if (t.type == TILE_SENSOR) {
        if (!tv["http"].isNull()) {
          TileAction source;
          if (!tv["http"].is<JsonObjectConst>() || !parseHttp(tv["http"], source, err, where + " http")) {
            if (err && !err->length()) *err = where + ": http must be an object";
            return false;
          }
          t.http.enabled = true;
          t.http.url = source.url;
          t.http.headers = source.headers;
          t.http.path = (const char*)(tv["http"]["path"] | "");
          t.http.poll = (uint16_t)constrain((int)(tv["http"]["poll"] | 30), 5, 3600);
          t.unit = (const char*)(tv["unit"] | "");
          t.detail = (const char*)(tv["detail"] | "");
          t.entity = "";
          JsonVariantConst rows = tv["rows"];
          if (!rows.isNull()) {
            if (!rows.is<JsonArrayConst>() || rows.size() < 1 || rows.size() > MAX_SENSOR_ROWS) {
              if (err) *err = where + ": rows must be an array of 1.." + String(MAX_SENSOR_ROWS);
              return false;
            }
            for (JsonVariantConst rv : rows.as<JsonArrayConst>()) {
              SensorRowDef row;
              row.label = (const char*)(rv["label"] | "");
              row.path = (const char*)(rv["path"] | "");
              row.value = (const char*)(rv["value"] | "");
              if (!row.path.length()) {
                if (err) *err = where + ": every row needs a path";
                return false;
              }
              t.rows.push_back(row);
            }
            t.scaleMax = (uint16_t)constrain((int)(tv["scaleMax"] | 0), 0, 65535);
          }
        }
        t.style = !strcmp(tv["style"] | "number", "chart") && t.rows.empty() ? METRIC_CHART : METRIC_NUMBER;
        t.duration = !strcmp(tv["format"] | "", "duration");
        t.hours = (uint8_t)constrain((int)(tv["hours"] | 24), 1, 168);
        t.warn = tv["warn"] | 0.0f;
        t.crit = tv["crit"] | 0.0f;
        t.hasWarn = tv["warn"].is<float>() && isfinite(t.warn);
        t.hasCrit = tv["crit"].is<float>() && isfinite(t.crit);
      }
      if (t.type == TILE_CLOCK || t.type == TILE_MEDIA) {
        if (t.type == TILE_MEDIA) t.pc = (const char*)(tv["pc"] | "");
        p.tiles.push_back(t);
        continue;
      }
      if (t.type == TILE_PRINTER) {
        t.printer = (const char*)(tv["printer"] | "");
        if (!printerKnown(t.printer)) {
          if (err) *err = where + ": unknown printer '" + t.printer + "'";
          return false;
        }
        static const char* const kShow[] = {"hero", "nozzle", "bed", "chamber", "filament", "layer", "light", "ams", "slots", "summary"};
        const char* sh = tv["show"] | "hero";
        int k = 0;
        while (k < 10 && strcmp(sh, kShow[k])) k++;
        if (k == 10) {
          if (err) *err = where + ": unknown show '" + sh + "'";
          return false;
        }
        t.show = (PrinterShow)k;
        t.nozzle = !strcmp(tv["nozzle"] | "right", "left") ? 1 : 0;
        t.amsUnit = constrain(tv["unit"] | 1, 1, PRN_AMS_UNITS);
        p.tiles.push_back(t);
        continue;  // built-in taps: light toggles, summary opens the printer page
      }
      if (t.type == TILE_SLIDER && !strcmp(tv["control"] | "", "pc-volume")) {
        t.pcVolume = true;
        t.pc = (const char*)(tv["pc"] | "");
        t.entity = "";
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
      if (tv["tap"].isNull() && t.type == TILE_SENSOR && !t.http.enabled) t.tap.kind = ACT_MORE_INFO;
      // A slider toggles on a tap; a number has no on/off, so its card opens.
      if (tv["tap"].isNull() && t.type == TILE_SLIDER)
        t.tap.kind = t.entity.startsWith("input_number.") || t.entity.startsWith("number.")
                         ? ACT_MORE_INFO
                         : ACT_TOGGLE;
      // PC volume: a tap mutes; no HA card to hold for.
      if (t.pcVolume && tv["tap"].isNull()) {
        t.tap.kind = ACT_PC;
        t.tap.pc = t.pc;
        t.tap.cmd = "volume_mute";
      }
      if (tv["hold"].isNull() && !t.http.enabled && !t.pcVolume) t.hold.kind = ACT_MORE_INFO;
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

static bool wipeDirectory(const String& path, const char* keep) {
  std::vector<String> paths;
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return false;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) paths.push_back(f.path());
  dir.close();
  bool ok = true;
  for (const String& p : paths) {
    if (p == keep) continue;
    File f = LittleFS.open(p);
    const bool directory = f && f.isDirectory();
    f.close();
    if (directory) {
      ok &= wipeDirectory(p, keep);
      ok &= LittleFS.rmdir(p);
    } else ok &= LittleFS.remove(p);
  }
  return ok;
}

bool deckFsWipe(const char* keep) {
  return mountFs() && wipeDirectory("/", keep);
}

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
