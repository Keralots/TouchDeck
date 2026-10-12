// Guition deck configuration: `deck.json` schema v1 (see the deck plan).
//
// { "schema": 1, "theme": "midnight",
//   "pages": [
//     { "id": "pc-stats", "type": "metrics" },
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
// label / icon null = take them from Home Assistant later (Phase 3); until
// then the entity id stands in. `sub` is an optional fixed second line.
// Positions are keyed by orientation; a tile without one for the current
// orientation flows into the next free cell. A page of type "metrics" is the
// StatNode face page; without one it is inserted first.
#ifndef DECK_CONFIG_H
#define DECK_CONFIG_H

#if defined(BOARD_IS_JC3248W535)

#include <Arduino.h>
#include <vector>

namespace deck {

constexpr int ORIENT_LANDSCAPE = 0;
constexpr int ORIENT_PORTRAIT = 1;
constexpr int MAX_DECK_PAGES = 8;
constexpr int MAX_PAGE_TILES = 24;

enum TileType : uint8_t { TILE_ACTION = 0, TILE_TOGGLE, TILE_SENSOR };
enum ActionKind : uint8_t {
  ACT_NONE = 0,
  ACT_TOGGLE,        // toggle the tile's entity
  ACT_CALL_SERVICE,  // {domain, service, data}
  ACT_PAGE,          // {page: id}
  ACT_MORE_INFO,     // detail popup
};

struct TileAction {
  ActionKind kind = ACT_NONE;
  String domain, service, data;  // data = service_data as JSON text
  String page;
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
};

struct PageDef {
  String id, name;
  bool metrics = false;
  uint8_t cols[2] = {4, 3};
  uint8_t rows[2] = {3, 4};
  std::vector<TileDef> tiles;
};

struct DeckConfig {
  uint8_t schema = 1;
  String theme;
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
// The stored /deck.json text, or the built-in default.
void deckConfigText(String& out);
// Validate and store as /deck.json (written to a temp file, then renamed, so a
// power cut never leaves half a file). On success *out is the parsed config.
bool deckConfigStore(const String& text, DeckConfig& out, String& err);
// LittleFS mounted (mounts on first call).
bool deckFsReady();

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_CONFIG_H
