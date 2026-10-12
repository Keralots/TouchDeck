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
//   "style": "number" | "chart" | "bar", "scaleMax": 0 (= by unit) }.
//
// "confirm": "off" | "always" (optional, tap and hold actions): ask on the
// device before turning the tile off, or before any action it runs.
//
// An HTTP request as a tap or hold action (plain http on the LAN only):
// { "action": "http", "method": "POST", "url": "http://host/path",
//   "body": "text" | { json }, "contentType": "...", "headers": { "K": "v" } }.
// method defaults to GET, or POST with a body.
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

enum TileType : uint8_t { TILE_ACTION = 0, TILE_TOGGLE, TILE_SENSOR, TILE_METRIC };
enum MetricStyle : uint8_t { METRIC_NUMBER = 0, METRIC_CHART, METRIC_BAR };
enum ActionKind : uint8_t {
  ACT_NONE = 0,
  ACT_TOGGLE,        // toggle the tile's entity
  ACT_CALL_SERVICE,  // {domain, service, data}
  ACT_PAGE,          // {page: id}
  ACT_MORE_INFO,     // detail popup
  ACT_HTTP,          // {method, url, body, contentType, headers}
};
enum ConfirmMode : uint8_t { CONFIRM_NONE = 0, CONFIRM_OFF, CONFIRM_ALWAYS };

struct TileAction {
  ActionKind kind = ACT_NONE;
  String domain, service, data;  // data = service_data as JSON text
  String page;
  String method, url, body, ctype;  // ACT_HTTP
  String headers;                   // ACT_HTTP: "Name: value\n" lines
};

// set = an explicit cell (x, y). w and h always apply: a tile with only a
// span flows into the next free place that fits it.
struct TilePos {
  bool set = false;
  uint8_t x = 0, y = 0, w = 1, h = 1;
};

struct TileDef {
  String id;
  TileType type = TILE_ACTION;
  String entity, label, sub, icon;
  bool hasColor = false;
  uint32_t color = 0;
  TilePos pos[2];
  TileAction tap, hold;
  ConfirmMode confirm = CONFIRM_NONE;
  uint8_t metric = 0;  // TILE_METRIC: companion metric id
  MetricStyle style = METRIC_NUMBER;
  uint16_t scaleMax = 0;  // 0 = full scale by unit
};

struct PageDef {
  String id, name;
  String group;  // adjacent pages with the same group form one column
  uint8_t cols[2] = {4, 3};
  uint8_t rows[2] = {3, 4};
  std::vector<TileDef> tiles;
};

struct DeckConfig {
  uint8_t schema = 1;
  String theme;
  String ledEntity;  // HA entity the LED's day level follows, empty = none
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

}  // namespace deck

#endif  // DECK_CONFIG_H
