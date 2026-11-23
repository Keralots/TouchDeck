// Internal to the clock tile styles (src/deck_ui_clock*.cpp).
#pragma once
#include <esp_random.h>
#include <math.h>

#include <new>

#include "deck_ui_int.h"

namespace deckui {

// One animation frame, shared by all the clock tiles.
struct ClockNow {
  uint32_t ms;      // animation clock
  char cur[6];      // "HH:MM" (12 h: " H:MM"), blank before the time is synced
  char next[6];     // a minute on
  int sec;
  int32_t minute;   // epoch minute
  bool colon;
  bool live;        // the tile is on the lit, uncovered page
};

// A style that draws the whole tile. Lives in the LVGL heap (PSRAM), zeroed.
struct ClockScene {
  virtual ~ClockScene() {}
  // False when the tile is too small (the tile shows Roll instead).
  virtual bool fit(int w, int h, int pad) = 0;
  // False when nothing changed.
  virtual bool tick(lv_obj_t* o, const ClockNow& n) = 0;
  virtual void draw(lv_layer_t* L, int ox, int oy, lv_color_t col) = 0;
};

template <class T>
ClockScene* newScene() {
  void* p = lv_malloc(sizeof(T));
  return p ? new (p) T() : nullptr;
}
ClockScene* blocksScene();
ClockScene* breakoutScene();
ClockScene* rocksScene();

constexpr int CLOCK_DIGITS[4] = {0, 1, 3, 4};  // of "HH:MM"
extern const uint8_t CLOCK_DOTS[10][7];        // 5x7 digits, a byte a row
static inline bool clockDot(char v, int r, int c) { return (CLOCK_DOTS[v - '0'][r] >> (4 - c)) & 1; }

// The time in the deck font, object coordinates; each digit moves on its own.
struct DigitRow {
  const lv_font_t* f;
  int16_t cell, colonW, inkTop, inkH, colonDy, top, xs[5];
  float offX[5], offY[5], velX[5], velY[5];  // px, px per 50 ms frame

  bool fit(int w, int h);  // sizes only: place() puts it on the tile
  void place(int w, int y);
  void box(int i, float& x1, float& y1, float& x2, float& y2) const;  // ink box of digit i
  int points(int i, char v, float* px, float* py) const;  // centres of v's 5x7 dots over digit i, up to 35
  bool moving(int i) const { return offX[i] != 0 || offY[i] != 0 || velX[i] != 0 || velY[i] != 0; }
  lv_area_t colonArea() const;
  void draw(lv_layer_t* L, int ox, int oy, int i, char ch, lv_color_t col) const;
};

// Areas one frame redraws, object coordinates.
struct Dirty {
  lv_area_t a[32];
  int n = 0;
  void add(const lv_area_t& r) {
    if (n < 32) a[n++] = r;
  }
  bool flush(lv_obj_t* o) const;  // false when there is nothing to redraw
};

// The box round some moving points.
struct Box {
  float x1, y1, x2, y2;
  bool on;
  void add(float x, float y, float r) {
    if (!on) x1 = x - r, y1 = y - r, x2 = x + r, y2 = y + r, on = true;
    else x1 = LV_MIN(x1, x - r), y1 = LV_MIN(y1, y - r), x2 = LV_MAX(x2, x + r), y2 = LV_MAX(y2, y + r);
  }
  void add(const Box& b) {
    if (b.on) add(b.x1, b.y1, 0), add(b.x2, b.y2, 0);
  }
  lv_area_t area() const {
    return {(int32_t)floorf(x1) - 2, (int32_t)floorf(y1) - 2, (int32_t)ceilf(x2) + 2, (int32_t)ceilf(y2) + 2};
  }
};

static inline bool areaHits(const lv_area_t& a, const lv_area_t& c) {
  return !(a.x2 < c.x1 || a.x1 > c.x2 || a.y2 < c.y1 || a.y1 > c.y2);
}
// Deck clock fonts, the largest first.
constexpr int CLOCK_FONTS = 6;
extern const lv_font_t* const CLOCK_FONT[CLOCK_FONTS];
int clockCellW(const lv_font_t* f);  // widest digit
// Fonts put the colon at x-height middle: moves it to the middle of the digits.
int clockColonDy(const lv_font_t* f);

// Screen coordinates, skipped outside the clip area.
void fillRect(lv_layer_t* L, const lv_area_t& a, lv_color_t col, int radius = 0, lv_opa_t opa = LV_OPA_COVER);
// One character centred in a cell w wide.
void drawGlyph(lv_layer_t* L, const lv_font_t* f, char ch, int x, int y, int w, lv_color_t col,
               lv_opa_t opa = LV_OPA_COVER);

static inline float rnd(float a, float b) { return a + (b - a) * (float)(esp_random() % 10000) / 10000.0f; }
static inline uint32_t rndInt(uint32_t n) { return n ? esp_random() % n : 0; }
static inline bool coin() { return esp_random() & 1; }

}  // namespace deckui
