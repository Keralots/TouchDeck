// Guition deck configuration: `deck.json` schema v1 (see the deck plan).
//
// { "schema": 1, "theme": "midnight",
//   "pages": [
//     { "id": "desk", "name": "Desk",
//       "layouts": { "landscape": { "cols": 4, "rows": 3 } },
//       "tiles": [
//         { "id": "t1", "type": "toggle", "entity": "light.desk_lamp",
//           "label": "Desk lamp", "sub": null, "icon": "mdi:lightbulb",
//           "color": "#F6C56B",
//           "pos": { "landscape": { "x": 0, "y": 0, "w": 1, "h": 1 } },
//           "tap":  { "action": "toggle" },
//           "hold": { "action": "more-info" } } ] } ] }
//
// "led": { "entity": "input_number.desk_led" } (optional): the status LED's
// day level follows an input_boolean, input_number or light.
//
// A PC metric tile: { "type": "metric", "metric": <companion id>,
//   "pc": "<companion pc_id>" (optional, default the first known PC),
//   "style": "number" | "chart" | "bar", "scaleMax": 0 (= by unit) }.
//
// A sensor tile: { "type": "sensor", "entity": "sensor.temperature",
//   "style": "number" | "chart", "hours": 24 (1..168),
//   "warn": 25, "crit": 30 } (thresholds optional, in the sensor unit).
// crit < warn means lower is worse; otherwise higher is worse.
// "format": "duration" shows a number in s / min / h / d as "1h 49m".
//
// An HTTP sensor uses "http" instead of "entity":
// { "type": "sensor", "http": { "url": "http://host/usage",
//   "path": "cpu.used", "poll": 30, "headers": { "Name": "value" } },
//   "unit": "%", "style": "number" | "chart", "warn": 70, "crit": 90 }.
// GET only; poll = 5..3600 seconds (default 30). Dot paths accept array
// indexes; empty path reads a scalar body. Charts keep 48 numeric readings.
// HTTP sensors have no default tap/hold action or HA detail card.
// "detail": "runtime {ups.runtime_min|min}" adds a small line with
// values from the same reply; "|unit" formats a number as a duration.
//
// Bambu printers: "printers": [{ "id": "h2c", "name": "H2C",
//   "ip": "192.168.0.54", "plug": "switch.printer_plug" }] (plug optional:
//   its HA state powers the printer's connection and pages; serial and access
//   code live in NVS, never in deck.json). A page with "printer": "h2c" exists
//   only while that printer is powered. A printer tile:
// { "type": "printer", "printer": "h2c", "show": "hero" | "nozzle" | "bed" |
//   "chamber" | "filament" | "layer" | "light" | "ams" | "slots" | "summary",
//   "nozzle": "left" | "right", "unit": 1 } (unit is the 1-based AMS position).
//
// "type": "clock": the time, and the date when the tile has room
// (24h and time zone from the device settings). Read-only.
//
// "textColor": "#RRGGBB" (optional) colours the tile's text; for a metric
// tile "chartColor" colours the chart or bar line apart from "color" (the name).
//
// "type": "slider": the tile is a level control (light brightness, cover
// position, fan speed, media volume, input_number / number); drag sideways to
// set it, tap toggles (numbers: opens the card).
//
// "confirm": "off" | "always" (optional, tap and hold actions): ask on the
// device before turning the tile off, or before any action it runs.
//
// An HTTP request as a tap or hold action (plain http on the LAN only):
// { "action": "http", "method": "POST", "url": "http://host/path",
//   "body": "text" | { json }, "contentType": "...", "headers": { "K": "v" } }.
// method defaults to GET, or POST with a body.
//
// A command to a PC running the companion (paired from the companion):
// { "action": "pc", "pc": "<pc_id>" (optional), "cmd": "media_play_pause" |
//   "media_next" | "media_prev" | "media_stop" | "volume_up" | "volume_down" |
//   "volume_mute" | "lock" | "sleep" | "restart" | "shutdown" }.
//
// label / icon null = take them from Home Assistant later (Phase 3); until
// then the entity id stands in. `sub` is an optional fixed second line.
// Positions are keyed by orientation; a tile without one for the current
// orientation flows into the next free cell. Adjacent pages with the same
// "group" stack into one column: left/right moves between columns, up/down
// between the pages of a column. Pages of type "metrics" (left
// over from StatNode) are skipped.
#ifndef DECK_CONFIG_H
#define DECK_CONFIG_H

#include <Arduino.h>
#include <vector>

namespace deck {

constexpr int ORIENT_LANDSCAPE = 0;
constexpr int ORIENT_PORTRAIT = 1;
constexpr int MAX_DECK_PAGES = 8;
constexpr int MAX_PAGE_TILES = 24;

enum TileType : uint8_t { TILE_ACTION = 0, TILE_TOGGLE, TILE_SENSOR, TILE_METRIC, TILE_SLIDER, TILE_CLOCK, TILE_PRINTER };
enum PrinterShow : uint8_t { PS_HERO = 0, PS_NOZZLE, PS_BED, PS_CHAMBER, PS_FILAMENT, PS_LAYER, PS_LIGHT, PS_AMS, PS_SLOTS, PS_SUMMARY };
constexpr int MAX_PRINTERS = 2;
enum MetricStyle : uint8_t { METRIC_NUMBER = 0, METRIC_CHART, METRIC_BAR };
enum ActionKind : uint8_t {
  ACT_NONE = 0,
  ACT_TOGGLE,        // toggle the tile's entity
  ACT_CALL_SERVICE,  // {domain, service, data}
  ACT_PAGE,          // {page: id}
  ACT_MORE_INFO,     // detail popup
  ACT_HTTP,          // {method, url, body, contentType, headers}
  ACT_PC,            // {pc, cmd}: a command to a paired PC's companion
};
enum ConfirmMode : uint8_t { CONFIRM_NONE = 0, CONFIRM_OFF, CONFIRM_ALWAYS };

struct TileAction {
  ActionKind kind = ACT_NONE;
  String domain, service, data;  // data = service_data as JSON text
  String page;
  String method, url, body, ctype;  // ACT_HTTP
  String headers;                   // ACT_HTTP: "Name: value\n" lines
  String pc, cmd;                   // ACT_PC: companion pc_id ("" = first PC), command
};

// set = an explicit cell (x, y). w and h always apply: a tile with only a
// span flows into the next free place that fits it. All four in half cells
// (deck.json takes 0.5 steps: "w": 1.5).
struct TilePos {
  bool set = false;
  uint8_t x = 0, y = 0, w = 2, h = 2;
};

struct HttpSensorDef {
  bool enabled = false;
  String url, headers, path;
  uint16_t poll = 30;
};

struct TileDef {
  String id;
  TileType type = TILE_ACTION;
  String entity, label, sub, icon;
  HttpSensorDef http;
  String unit;  // HTTP sensor: unit for JSON numbers only
  String detail;  // HTTP sensor: second line template, see above
  bool duration = false;  // sensor: "format": "duration"
  bool hasColor = false;
  uint32_t color = 0;
  bool hasTextColor = false, hasChartColor = false;
  uint32_t textColor = 0, chartColor = 0;
  TilePos pos[2];
  TileAction tap, hold;
  ConfirmMode confirm = CONFIRM_NONE;
  uint8_t metric = 0;  // TILE_METRIC: companion metric id
  String pc;           // TILE_METRIC: companion pc_id, "" = the first known PC
  MetricStyle style = METRIC_NUMBER;
  uint8_t hours = 24;  // TILE_SENSOR: history period, 1..168 hours
  bool hasWarn = false, hasCrit = false;
  float warn = 0, crit = 0;
  uint16_t scaleMax = 0;  // 0 = full scale by unit
  String printer;         // TILE_PRINTER: printer id
  PrinterShow show = PS_HERO;
  uint8_t nozzle = 0;     // PS_NOZZLE: 0 = right / only, 1 = left
  uint8_t amsUnit = 1;    // PS_AMS: 1-based position in the reported units
};

struct PrinterDef {
  String id, name, ip;
  String plug;  // HA switch that powers the printer, "" = always powered
};

struct PageDef {
  String id, name;
  String group;  // adjacent pages with the same group form one column
  String printer;  // built only while this printer is powered
  uint8_t cols[2] = {4, 3};
  uint8_t rows[2] = {3, 4};
  std::vector<TileDef> tiles;
};

struct DeckConfig {
  uint8_t schema = 1;
  String theme;
  String ledEntity;  // HA entity the LED's day level follows, empty = none
  std::vector<PrinterDef> printers;
  std::vector<PageDef> pages;
};

// Mounts LittleFS (the board's spare data partition) on first use. Loads
// /deck.json when present and valid, otherwise the built-in default. Fills
// *source with where the config came from and *err with why a stored file
// was rejected (empty when it was not).
bool deckConfigLoad(DeckConfig& out, String* source, String* err);
// Parse and validate a JSON document. Used by the loader and, later, by the
// portal before anything is written.
bool deckConfigParse(const char* json, size_t len, DeckConfig& out, String* err);
// One tap/hold action object on its own (the portal's HTTP Test button).
bool deckActionParse(const char* json, size_t len, TileAction& out, String* err);
// The stored /deck.json text, or the built-in default.
void deckConfigText(String& out);
// Validate and store as /deck.json (written to a temp file, then renamed, so a
// power cut never leaves half a file). On success *out is the parsed config.
bool deckConfigStore(const String& text, DeckConfig& out, String& err);
// LittleFS mounted (mounts on first call).
bool deckFsReady();
// Factory reset: delete every stored file but keep (a path such as "/mdi.ttf").
bool deckFsWipe(const char* keep);

}  // namespace deck

#endif  // DECK_CONFIG_H
