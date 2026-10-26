// Internal to the deck UI (src/deck_ui*.cpp): shared types, state and helpers.
#pragma once
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
#include <src/misc/cache/instance/lv_image_cache.h>
#include "display_ui.h"
#include "settings.h"
#include "led.h"
#include "haptic.h"
#include "pc_metrics.h"
#include "metric_format.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lvgl.h>
#include <time.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <new>
#include <vector>

using namespace deck;

namespace deckui {

constexpr int MAX_PAGES = MAX_DECK_PAGES;
constexpr int HEADER_H = 30;

constexpr uint32_t C_WARN = 0xE5A50A;
constexpr uint32_t C_CRIT = 0xE5484D;
constexpr uint32_t GLASS_BORDER_COLOR = 0xFFFFFF;
constexpr lv_opa_t GLASS_BORDER_OPA = LV_OPA_20;
constexpr int GLASS_BORDER_WIDTH = 1;

struct Page {
  lv_obj_t* root;
  lv_obj_t* title;
  lv_obj_t* clock;
  lv_obj_t* wifi;
  lv_obj_t* grid;
  lv_draw_buf_t* snap;
  lv_draw_buf_t* glass;  // Glass theme: the baked background
  std::vector<uint32_t> glassKey;  // empty until a bake finishes
  const PageDef* def;
  bool stale;
  bool wallBake;
  uint32_t snapMs;  // when snap was last taken
};

// Stable descriptors shared by tiles showing the same cover and size.
struct MediaArt {
  String id;
  int px = 0;
  lv_image_dsc_t dsc = {};
  uint8_t* pixels = nullptr;
  ~MediaArt() {
    lv_image_cache_drop(&dsc);
    heap_caps_free(pixels);
  }
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
  const lv_font_t* unitFont;  // unit font picked by fitTile
  bool besideName;           // chart or row: the value shares the line with the name
  uint8_t row;               // metric row: 1 = name and value, 2 = chart or bar between them
  char rowMax[7];            // widest number seen on a wide row as zeros ("0000.0"), its chart only shrinks
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
  struct Spark* spark;    // PC metric chart, drawn by the deck (see sparkPaint)
  lv_obj_t* detail;       // HTTP sensor: the "detail" line
  lv_obj_t* rows;         // HTTP sensor rows: one box per row (label, bar, value)
  int16_t rowH;           // height of one row box
  lv_obj_t* px[4];        // printer tiles: extra labels, swatches
  uint32_t prnKey;        // printer tiles: what the last apply showed, 0 = redraw
  struct PrnFx* pfx;      // printer tiles: counted reading and animations
  lv_obj_t* fx;           // touch effect running on this tile
  lv_obj_t* halo;         // on/off effect running on this tile
  lv_point_t press;       // last touch on the tile (screen coords), for effects
  uint32_t pressMs;
  // Clock tiles: hours in sub, then these two; the colon blinks.
  lv_obj_t* colon;
  lv_obj_t* mins;
  bool colonLit;
  lv_obj_t* cover;
  lv_obj_t* mediaButtons[3];
  lv_obj_t* mediaIcons[3];
  std::shared_ptr<MediaArt> art;
  String artWanted;
  uint32_t artAttempt;
  int16_t coverPx;
  bool mediaFull;
  // Glass auto text: the colour picked from the picture under the tile.
  uint32_t autoText;
  bool hasAuto;
};

struct Shared {
  int active = 0;
  int count = 0;
  bool swipeEnabled = false;
  bool owned = false;      // a swipe owns the screen until its landing is handled
  bool valid[MAX_PAGES] = {};
  bool busy[MAX_PAGES] = {};
  Src src[MAX_PAGES] = {};
  // Each page's grid columns and where its grid starts (cascade page fx).
  uint8_t gridCols[MAX_PAGES] = {};
  int16_t gridTop[MAX_PAGES] = {};
  uint16_t gridCuts[MAX_PAGES] = {};  // column edges no tile crosses, bit k = edge k
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
  lv_obj_t* lightLbl;
  lv_obj_t* lightSlider;
  lv_obj_t* lightVal;
  lv_obj_t* nightBtn;
  lv_obj_t* headerBtn;
  lv_draw_buf_t* snap;
  lv_draw_buf_t* glass;  // Glass theme: frosted page under the open panel
  bool open;
  bool stale;
  uint32_t lastStatus;
};

constexpr int NAME_LINE_SP = -2;  // Montserrat's line height has room for accents

constexpr uint32_t C_PRN_HEAT = 0xF5B83D;  // heating, light on

// The companion sends about once a second. The chart lags one sample and
// glides at the packet rate, so the newest reading walks in instead of the
// line jumping a column per packet. It eases toward the packet clock rather
// than snapping to it, so an early or late packet causes no jump; a few extra
// older samples keep the left edge on real data while it catches up.
constexpr int SPARK_FRAME_MS = 33;

constexpr uint32_t SPARK_SNAP_MS = 5000;

constexpr int SPARK_KEEP = PC_HISTORY_LEN + 4;

struct Spark {
  float v[SPARK_KEEP];      // oldest first
  uint8_t n;
  uint8_t head, cnt;        // the metric's history ring when last copied
  float off;                // right edge minus the newest sample, in samples (<= 0)
  float lo, hi;             // axis range the samples ask for
  float viewLo, viewHi;     // range on screen, eased toward lo / hi
  bool haveView;
  uint32_t rxMs;            // arrival of the newest sample
  uint32_t gapMs;           // average interval between samples
  uint32_t color;
  lv_draw_buf_t* buf;       // A8 coverage, the chart object's size; drawn in color
  int32_t key;              // glide step last painted, -1 = repaint
  bool painted;             // buf holds a chart (a repaint keeps the old one until then)
  float paintLo, paintHi;
};

// Where each tile of a page lands: explicit cells first (in tile order), then
// the rest flow into the first free place that fits. The portal grid editor
// mirrors this rule (layout() in web/deck.js). Cells count in half cells.
struct Cell {
  bool placed;
  int x, y, w, h;
  bool off = false;  // printer tiles with printers disabled, "when" not met: no cell, stored pos untouched
};

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
  Cell swapTo = {};  // where tile swap goes when the drag lands
  lv_point_t p0 = {};
  int bx0 = 0, by0 = 0;
  Cell to = {};
  // Tiles added from the picker: s_ed.tiles past the page's own get these.
  std::vector<TileDef> added;
};

struct PickRow {
  String id, name, state;
};

// deck_ui_bind.cpp
extern uint32_t s_prnSeen;
extern uint32_t s_prnGraceAt;
extern bool s_prnRebuild;
extern String s_keepPage;
extern std::vector<std::pair<String, String>> s_whenStates;
uint32_t tileTextRt(const TileRt& r);
uint32_t sensorColour(const TileRt& r, bool onTile = false);
uint32_t sensorLine(const TileRt& r);
bool copyHistoryChart(const TileDef& d, uint8_t hours, uint32_t& version, lv_obj_t* chart, int32_t* points, float& lo, float& hi);
void applySensorHistory(TileRt& r);
String prettyState(const String& domain, const HaEntity& e);
void setTileOn(TileRt& r, bool on);
bool sliderDomain(const String& d);
float sliderFrac(const String& d, const HaEntity& e);
Wish* findWish(const String& entity);
bool wishOn(const String& entity, const HaEntity& e);
float wishFrac(const String& entity, const HaEntity& e);
String wishText(const String& entity, const HaEntity& e, bool slider);
bool setWish(const String& entity, bool level, float value, bool dragging = false);
void holdLevel(const String& entity, float value);
void releaseLevel(const String& entity);
void setSliderFill(TileRt& r, float frac);
void applyHaToTile(TileRt& r, bool force);
bool ledToHa(uint8_t v);
void haSync();
void pcCmdSync();
bool whenWanted(const WhenDef& w, const String& pcId);
bool whenWanted(const TileDef& t);
bool whenWanted(const PageDef& p);
void whenTrack();
void whenApply();
String rowUnit(const TileDef& d);
String rowText(const TileDef& d, size_t i, bool useTemplate);
void httpSync();
std::vector<String> entityIds();
void collectEntities();
void stopGestures();
bool frameActive();
void frameAbort();
bool frameSleep(bool sleep);
void frameTick();
void frameCatchUp();
void presenceTick();
void presenceWake();
void presenceRestart();
void runAction(TileRt& r, const TileAction& a);
bool needsConfirm(const TileRt& r, const TileAction& a);
const char* actionVerb(const TileRt& r, const TileAction& a);
void requestAction(TileRt& r, bool hold);
void tileEvent(lv_event_t* e);
bool pcVolHeld();
void pcVolumeTick();
void sliderEvent(lv_event_t* e);

// deck_ui_panel.cpp
extern bool s_header;
extern QuickPanel s_qp;
void qpMarkStale();
uint8_t& screenLevelRef();
void qpSyncValues();
void qpUpdateStatus(bool force);
lv_obj_t* qpButton(lv_obj_t* parent, int x, int y, int w, const char* text, bool checkable);
const uint16_t* lastFrame();
lv_draw_buf_t* frostedCopy(int x, int y, int w, int h, int rad);
void frostStyle(lv_obj_t* o, lv_draw_buf_t* glass);
void buildQuickPanel();
void destroyQuickPanel();
void showPanel(bool open);
void hidePanelNow();
void refreshPanelSnapshot();
void closePanelAnimated();

// deck_ui_card.cpp
extern lv_obj_t* s_modalCard;
extern int s_modalTile;
extern int8_t s_confirmHold;
bool modalShown();
void setModalOpen(bool open);
void deckToastShow(const char* text, uint32_t holdMs);
void dailyBarSync();
void closeModal();
void refreshModal();
void openModal(int tileIdx);
void onConfirmOk(lv_event_t*);
void openConfirm(int tileIdx, bool hold);
void pairSync();

// deck_ui_fit.cpp
lv_obj_t* makeRoot(int w, int h);
uint32_t metricAccent(const TileDef& d);
uint32_t metricChart(const TileDef& d);
uint32_t tileText(const TileDef& d);
void setText(lv_obj_t* l, const char* text);
int lineH(const lv_font_t* f);
int textH(const lv_font_t* f, int lines);
void showObj(lv_obj_t* o, bool on);
void setFont(lv_obj_t* o, const lv_font_t* f);
int textW(const char* text, const lv_font_t* f);
void fitNameText(TileRt& r);
void layoutRows(TileRt& r);
void fitValue(TileRt& r);
void fitEvent(lv_event_t* e);

// deck_ui_media.cpp
void mediaGlyph(lv_obj_t* label, const char* name, const char* fallback, int px);
void mediaProgress(TileRt& r);
void mediaCover(TileRt& r);
void applyMedia(TileRt& r);
void mediaArtTick();
void mediaEvent(lv_event_t* e);
void fitMedia(TileRt& r, int cw, int ch);
void fitClock(TileRt& r, int cw, int ch);

// deck_ui_printer.cpp
void readPrinters();
void makePrinterContent(TileRt& r, lv_obj_t* t);
void fitPrinter(TileRt& r, int cw, int ch);
void applyPrinterToTile(TileRt& r);
void printerTap(TileRt& r);
void heroTick();

// deck_ui_prnfx.cpp
void prnFxMake(TileRt& r, lv_obj_t* t);
void prnFxFree(PrnFx* f);
void prnFxFit(TileRt& r, int size);
void prnTempSet(TileRt& r, float now, float target, float scale, uint32_t color);
void prnFilamentSet(TileRt& r, bool has, uint32_t rgb, bool clear, bool feeding, bool printing,
                    const char* name);
bool prnFxTick(uint32_t now, bool lit);

// deck_ui_metric.cpp
extern uint32_t s_sparkPaintMs;
void sparkFree(Spark* s);
bool sparkRender(TileRt& r, uint32_t now, bool ease, uint32_t dtMs);
void sparkDrawEvent(lv_event_t* e);
void sparkSizeEvent(lv_event_t* e);
void applyPcToTile(TileRt& r);
lv_obj_t* makeRows(lv_obj_t* t, const TileDef& d);
lv_obj_t* makeDetail(lv_obj_t* t, const TileDef& d);
void makeMetricContent(TileRt& r, lv_obj_t* t);
void applyPcStateToTile(TileRt& r);
void applyClock(TileRt& r);
void pcSync(bool force);

// deck_ui_tile.cpp
String entityDomain(const String& entity);
uint32_t domainColor(const String& domain);
uint32_t showIcon(lv_obj_t* label, const TileDef& d, const HaEntity* e, bool on, int px, uint32_t shown);
String tileLabel(const TileDef& t);
String tileSub(const TileRt& r);
void markPageStale(int page);
// Fresh image of a page into its swipe buffer (sh.src); false while a gesture owns them.
bool snapPage(int page);
extern uint32_t s_gen;  // UI generation, see compSetHoldGen
uint32_t tileBg(const TileDef& d);
int tileTint(const TileDef& d);
uint32_t onTextColor(const TileDef& def, bool glass);
void makeTile(lv_obj_t* grid, const TileDef& def, int page, int col, int row, int w, int h);
TileRt* tileOf(lv_obj_t* o);
uint32_t tileOnFill(const TileDef& d);
// Tile animations (deck_ui_fx.cpp).
PageFx fxPage();
int tileOnOpa(const TileDef& d);
void fxQuiet();
void fxPressed(lv_event_t* e);
void fxRipple(TileRt& r, lv_point_t p);  // the touch effect, p in screen coords
bool fxCancelRipples();  // true when one was running
void fxBeforeTileOn(TileRt& r, bool on);  // before the CHECKED state changes
void fxTileOn(TileRt& r, bool wasOn);
void fxCardOpen(lv_obj_t* card, int tileIdx);
void fxCardClose(lv_obj_t* card, int tileIdx);

// deck_ui_glass.cpp
std::vector<uint32_t> glassKey(Page& p);
void autoTextPage(Page& p);
bool bakeGlass(Page& p);

// deck_ui_edit.cpp
extern EditState s_ed;
extern lv_obj_t* s_pick;
extern std::vector<PickRow> s_pickRows;
bool edApply(int k, Cell c, bool sizing);
void edLayout();
void edRemove(int k);
bool edAdd(const String& entity, const String& name);
void pickClose();
void pickFill(int dom, size_t first);
void pickOpen(int dom);
void edOpen(int page);
void edClose();
void edFinish();

// deck_ui.cpp
extern uint8_t s_theme;
extern uint32_t C_BG, C_TILE, C_TILE_PRESSED, C_TILE_LINE, C_CHIP, C_TEXT, C_TEXT_ON, C_TITLE, C_CLOCK, C_OK, C_DIM, C_PANEL, C_TRACK, C_BUTTON;
extern lv_display_t* s_disp;
extern DeckConfig s_cfg;
extern Page s_pages[MAX_PAGES];
extern std::vector<TileRt> s_tiles;
extern int s_count;
extern uint32_t s_gen;
extern uint32_t s_lastSnapMs;
extern uint32_t s_snapUsMax;
extern lv_style_transition_dsc_t s_pressTrans;
extern portMUX_TYPE s_mux;
extern Shared sh;
void styleIconChip(lv_obj_t* chip, uint32_t accent);
void goToPage(const String& id);
void placeTiles(const PageDef& def, int orient, std::vector<Cell>& out);
void setPager(int page);
void releaseSwipe();
bool sparkGliding();
void setHeader(bool on);

}  // namespace deckui
