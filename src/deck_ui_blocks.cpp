// Deck UI: the Falling Blocks clock style. 5x7 block digits rebuilt dot by dot
// at the minute, over a strip that plays the block game by itself (Smooth play).
// Motion runs on the 16 ms tick its constants were tuned for, scaled to the tile.
#include <esp_random.h>
#include <math.h>

#include "deck_ui_int.h"

namespace deckui {

constexpr uint32_t TICK_MS = 16;
constexpr int TRIGGER_SEC = 56;  // the rebuild starts here so the new time is mostly in by :00
constexpr int MAX_ROWS = 24, MAX_COLS = 64, MAX_DOTS = 35;
constexpr int SLIP_PCT = 8;        // drops that may still leave a hole
constexpr int DOT_GAP_TICKS = 18;  // between released dots
constexpr float DOT_VY = 0.96f;    // px per tick at a 3 px digit pitch
constexpr float FALL_VY = 0.72f;   // px per tick at a 4 px strip cell
constexpr float SLIDE = 0.4f;      // columns per tick
constexpr int SPIN_TICKS = 12, CLEAR_TICKS = 15, PIECE_DELAY_TICKS = 28, RESET_DELAY_TICKS = 38;
constexpr float BOUNCE_V0 = 3.5f, BOUNCE_G = 0.6f, BOUNCE_STEP = 0.32f;  // per 50 ms frame, 3 px pitch

static const uint8_t DIG[10][7] = {
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110},
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},
    {0b00110, 0b01000, 0b10000, 0b10110, 0b10001, 0b10001, 0b01110},
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}};

struct Rot {
  int8_t w, h, cx[4], cy[4];
};
static const Rot ROTS[19] = {
    {4, 1, {0, 1, 2, 3}, {0, 0, 0, 0}}, {1, 4, {0, 0, 0, 0}, {0, 1, 2, 3}},  // I
    {2, 2, {0, 1, 0, 1}, {0, 0, 1, 1}},                                       // O
    {3, 2, {0, 1, 2, 1}, {0, 0, 0, 1}}, {2, 3, {1, 0, 1, 1}, {0, 1, 1, 2}},  // T
    {3, 2, {1, 0, 1, 2}, {0, 1, 1, 1}}, {2, 3, {0, 0, 1, 0}, {0, 1, 1, 2}},
    {3, 2, {1, 2, 0, 1}, {0, 0, 1, 1}}, {2, 3, {0, 0, 1, 1}, {0, 1, 1, 2}},  // S
    {3, 2, {0, 1, 1, 2}, {0, 0, 1, 1}}, {2, 3, {1, 0, 1, 0}, {0, 1, 1, 2}},  // Z
    {3, 2, {0, 0, 1, 2}, {0, 1, 1, 1}}, {2, 3, {0, 1, 0, 0}, {0, 0, 1, 2}},  // J
    {3, 2, {0, 1, 2, 2}, {0, 0, 0, 1}}, {2, 3, {1, 1, 0, 1}, {0, 1, 2, 2}},
    {3, 2, {2, 0, 1, 2}, {0, 1, 1, 1}}, {2, 3, {0, 0, 0, 1}, {0, 1, 2, 2}},  // L
    {3, 2, {0, 1, 2, 0}, {0, 0, 0, 1}}, {2, 3, {0, 1, 1, 1}, {0, 0, 1, 2}}};
static const uint8_t PIECE_ROT[7][2] = {{0, 2}, {2, 1}, {3, 4}, {7, 2}, {9, 2}, {11, 4}, {15, 4}};
static const uint32_t PIECE_COL[7] = {0xFF00FF, 0x00FFFF, 0xFFFF00, 0xFF8200, 0x00FF00, 0xFF0000, 0x0082FF};

enum { G_DELAY, G_MOVING, G_CLEARING };

struct Dot {
  int16_t tx, ty;
  float y;
  uint16_t at;
  bool locked;
};

struct BlocksRt {
  int16_t w, h, P, blk, Q, qblk;  // digit pitch and block, strip cell and block
  int16_t dx[5], dy0, colonX, gx, gy;
  uint8_t rows, cols;
  uint64_t occ[MAX_ROWS];
  uint8_t colour[MAX_ROWS][MAX_COLS];
  char shown[6];
  bool colonLit;
  // Rebuild: the changed digits of `to`, one at a time, left to right.
  char to[6];
  uint8_t queue[4], qLen, qPos;
  int8_t active;
  Dot dots[MAX_DOTS];
  uint8_t nDots;
  uint16_t dotTick;
  float bounceY[5], bounceV[5];
  int32_t triggered;  // epoch minute whose :56 started a rebuild
  // Game.
  uint8_t phase, piece, rot, drawRot, spins, spinT, clearFlash;
  uint16_t wait;
  float col, py;
  int8_t destCol, destOy;
  uint32_t clearMask;
  uint32_t lastMs;
};

BlocksRt* blocksNew() {
  BlocksRt* b = (BlocksRt*)lv_malloc_zeroed(sizeof(BlocksRt));
  if (!b) return nullptr;
  memcpy(b->shown, "  :  ", 6);
  b->active = -1;
  b->triggered = -1;
  b->colonLit = true;
  return b;
}

void blocksFree(BlocksRt* b) {
  if (b) lv_free(b);
}

static uint32_t rnd(uint32_t n) { return n ? esp_random() % n : 0; }
static uint64_t fullRow(const BlocksRt& b) { return b.cols >= 64 ? ~0ULL : (1ULL << b.cols) - 1; }

static void gameReset(BlocksRt& b) {
  memset(b.occ, 0, sizeof(b.occ));
  b.phase = G_DELAY;
  b.wait = PIECE_DELAY_TICKS;
  b.clearMask = 0;
}

// False when the tile is too small for it (the caller shows Roll instead).
bool blocksFit(BlocksRt* b, int w, int h, int pad) {
  const int iw = w - 2 * pad, ih = h - 2 * pad;
  const int P = LV_MIN(iw / 29, ih * 45 / 100 / 7);
  if (P < 3 || iw / 29 < 5) return false;  // too short, or 1x1 narrow: Roll
  const int Q = (P * 4 + 1) / 3;
  const int dy0 = pad + P, gy = dy0 + 8 * P;
  const int rows = LV_MIN(MAX_ROWS, (h - pad - gy) / Q), cols = LV_MIN(MAX_COLS, iw / Q);
  if (rows < 3 || cols < 8) return false;
  const bool same = b->w == w && b->h == h;
  b->w = (int16_t)w;
  b->h = (int16_t)h;
  b->P = (int16_t)P;
  b->blk = (int16_t)(P - LV_MAX(1, P / 3));
  b->Q = (int16_t)Q;
  b->qblk = (int16_t)(Q - LV_MAX(1, Q / 4));
  const int x0 = (w - 29 * P) / 2;
  for (int i = 0; i < 5; i++) b->dx[i] = (int16_t)(x0 + i * 6 * P);
  b->colonX = (int16_t)((b->dx[1] + 4 * P + b->dx[3]) / 2);
  b->dy0 = (int16_t)dy0;
  b->rows = (uint8_t)rows;
  b->cols = (uint8_t)cols;
  b->gx = (int16_t)((w - cols * Q) / 2);
  b->gy = (int16_t)(h - pad - rows * Q);
  if (!same) gameReset(*b);
  return true;
}

// --- Digit rebuild ---------------------------------------------------------------

static void startDigit(BlocksRt& b) {
  b.active = -1;
  while (b.qPos < b.qLen) {
    const int i = b.queue[b.qPos];
    const char v = b.to[i];
    b.nDots = 0;
    b.dotTick = 0;
    if (v >= '0' && v <= '9')
      for (int r = 6; r >= 0; r--)  // bottom row first, left to right
        for (int c = 0; c < 5; c++)
          if ((DIG[v - '0'][r] >> (4 - c)) & 1) {
            Dot& d = b.dots[b.nDots];
            d.tx = (int16_t)(b.dx[i] + c * b.P);
            d.ty = (int16_t)(b.dy0 + r * b.P);
            d.y = 0;  // from the top of the tile
            d.at = (uint16_t)(b.nDots * DOT_GAP_TICKS);
            d.locked = false;
            b.nDots++;
          }
    if (b.nDots) {
      b.active = (int8_t)i;
      return;
    }
    b.shown[i] = v;  // a blank (12 h) has nothing to build
    b.qPos++;
  }
}

static void startRebuild(BlocksRt& b, const char* to) {
  memcpy(b.to, to, sizeof(b.to));
  b.qLen = b.qPos = 0;
  for (int i : {0, 1, 3, 4})
    if (b.shown[i] != to[i]) b.queue[b.qLen++] = (uint8_t)i;
  startDigit(b);
}

static void finishAll(BlocksRt& b) {
  for (int k = b.qPos; k < b.qLen; k++) b.shown[b.queue[k]] = b.to[b.queue[k]];
  b.qPos = b.qLen;
  b.active = -1;
}

static void stepRebuild(BlocksRt& b) {
  if (b.active < 0) return;
  b.dotTick++;
  const float vy = DOT_VY * b.P / 3;
  bool done = true;
  for (int k = 0; k < b.nDots; k++) {
    Dot& d = b.dots[k];
    if (b.dotTick < d.at) {
      done = false;
      continue;
    }
    if (d.locked) continue;
    d.y += vy;
    if (d.y >= d.ty) {
      d.y = d.ty;
      d.locked = true;
    } else {
      done = false;
    }
  }
  if (!done) return;
  const int i = b.active;
  b.shown[i] = b.to[i];
  b.bounceV[i] = -BOUNCE_V0 * b.P / 3;
  b.qPos++;
  startDigit(b);
}

static void stepBounce(BlocksRt& b) {
  const float g = BOUNCE_G * b.P / 3;
  for (int i = 0; i < 5; i++) {
    if (b.bounceY[i] == 0 && b.bounceV[i] == 0) continue;
    b.bounceV[i] += g * BOUNCE_STEP;
    b.bounceY[i] += b.bounceV[i] * BOUNCE_STEP;
    if (b.bounceY[i] >= 0) b.bounceY[i] = b.bounceV[i] = 0;
  }
}

// --- Block game (Smooth play) ---------------------------------------------------

static int colTop(const BlocksRt& b, int c) {
  for (int r = 0; r < b.rows; r++)
    if (b.occ[r] & (1ULL << c)) return r;
  return b.rows;
}

// Resting top row of a rotation dropped at leftCol, -100 when it does not fit.
static int dropOy(const BlocksRt& b, int rot, int leftCol) {
  const Rot& p = ROTS[rot];
  if (leftCol < 0 || leftCol + p.w > b.cols) return -100;
  int oy = b.rows;
  for (int k = 0; k < 4; k++) oy = LV_MIN(oy, colTop(b, leftCol + p.cx[k]) - 1 - p.cy[k]);
  for (int k = 0; k < 4; k++)
    if (oy + p.cy[k] < 0 || oy + p.cy[k] >= b.rows) return -100;
  return oy;
}

static int holesIn(const BlocksRt& b, const uint64_t* occ) {
  int holes = 0;
  for (int c = 0; c < b.cols; c++) {
    int top = b.rows;
    for (int r = 0; r < b.rows; r++)
      if (occ[r] & (1ULL << c)) {
        top = r;
        break;
      }
    for (int r = top + 1; r < b.rows; r++)
      if (!(occ[r] & (1ULL << c))) holes++;
  }
  return holes;
}

// Keep the strip low and every column covered: the lowest landing spot wins.
static int scorePlacement(const BlocksRt& b, int rot, int leftCol, int oy, int& holes) {
  const Rot& p = ROTS[rot];
  uint64_t tmp[MAX_ROWS];
  memcpy(tmp, b.occ, sizeof(uint64_t) * b.rows);
  for (int k = 0; k < 4; k++) tmp[oy + p.cy[k]] |= 1ULL << (leftCol + p.cx[k]);
  int lines = 0;
  for (int r = 0; r < b.rows; r++)
    if (tmp[r] == fullRow(b)) lines++;
  int maxH = 0, bumps = 0, prevH = -1;
  for (int c = 0; c < b.cols; c++) {
    int top = b.rows;
    for (int r = 0; r < b.rows; r++)
      if (tmp[r] & (1ULL << c)) {
        top = r;
        break;
      }
    const int hgt = b.rows - top;
    maxH = LV_MAX(maxH, hgt);
    if (prevH >= 0) bumps += abs(hgt - prevH);
    prevH = hgt;
  }
  holes = holesIn(b, tmp);
  int maxCy = 0;
  for (int k = 0; k < 4; k++) maxCy = LV_MAX(maxCy, (int)p.cy[k]);
  return lines * 1000 + (oy + maxCy) * 24 - holes * 60 - maxH * 6 - bumps * 4;
}

// A random piece to the best spot that buries no new hole (relaxed for a slip
// or when nothing hole-free fits); equal spots are picked at random.
static bool pickPiece(BlocksRt& b) {
  const bool avoid = rnd(100) >= (uint32_t)SLIP_PCT;
  const int piece = (int)rnd(7), r0 = PIECE_ROT[piece][0], rn = PIECE_ROT[piece][1];
  const int cur = holesIn(b, b.occ);
  int best = -1000000, bestRot = -1, bestCol = 0, bestOy = 0, ties = 0;
  for (int pass = avoid ? 0 : 1; pass < 2 && bestRot < 0; pass++)
    for (int rot = r0; rot < r0 + rn; rot++)
      for (int c = 0; c + ROTS[rot].w <= b.cols; c++) {
        const int oy = dropOy(b, rot, c);
        if (oy <= -100) continue;
        int holes = 0;
        const int sc = scorePlacement(b, rot, c, oy, holes);
        if (pass == 0 && holes > cur) continue;
        if (sc > best) {
          best = sc, bestRot = rot, bestCol = c, bestOy = oy, ties = 1;
        } else if (sc == best && rnd(++ties) == 0) {
          bestRot = rot, bestCol = c, bestOy = oy;
        }
      }
  if (bestRot < 0) return false;
  b.piece = (uint8_t)piece;
  b.rot = (uint8_t)bestRot;
  b.destCol = (int8_t)bestCol;
  b.destOy = (int8_t)bestOy;
  b.drawRot = (uint8_t)(r0 + rnd(rn));
  b.spins = (uint8_t)(1 + rnd(4));
  b.spinT = 0;
  b.col = (b.cols - ROTS[bestRot].w) / 2.0f;
  b.py = -(float)(ROTS[bestRot].h * b.Q);  // above the tile, falls through the digits
  b.phase = G_MOVING;
  return true;
}

static void clearRows(BlocksRt& b) {
  int w = b.rows - 1;
  for (int r = b.rows - 1; r >= 0; r--) {
    if (b.clearMask & (1u << r)) continue;
    b.occ[w] = b.occ[r];
    memcpy(b.colour[w], b.colour[r], b.cols);
    w--;
  }
  for (; w >= 0; w--) b.occ[w] = 0;
  b.clearMask = 0;
}

// True when the strip changed as a whole (a clear or a reset).
static bool stepGame(BlocksRt& b) {
  if (b.phase == G_DELAY) {
    if (b.wait && --b.wait) return false;
    if (pickPiece(b)) return false;
    gameReset(b);
    b.wait = RESET_DELAY_TICKS;
    return true;
  }
  if (b.phase == G_CLEARING) {
    if (--b.clearFlash) return false;
    clearRows(b);
    b.phase = G_DELAY;
    b.wait = PIECE_DELAY_TICKS;
    return true;
  }
  if (b.col < b.destCol) b.col = LV_MIN(b.col + SLIDE, (float)b.destCol);
  else if (b.col > b.destCol) b.col = LV_MAX(b.col - SLIDE, (float)b.destCol);
  b.py += FALL_VY * b.Q / 4;
  const float landing = (float)(b.gy + b.destOy * b.Q);
  // Tumble a few times while high; the last turn lands in the chosen orientation.
  if (b.spins && b.py < landing - 2 * b.Q) {
    if (++b.spinT >= SPIN_TICKS) {
      b.spinT = 0;
      const int r0 = PIECE_ROT[b.piece][0], rn = PIECE_ROT[b.piece][1];
      b.drawRot = --b.spins ? (uint8_t)(r0 + (b.drawRot - r0 + 1) % rn) : b.rot;
    }
  } else {
    b.drawRot = b.rot;
  }
  if (b.py < landing) return false;
  b.py = landing;
  if (b.col != (float)b.destCol) return false;  // finish sliding first
  const Rot& p = ROTS[b.rot];
  for (int k = 0; k < 4; k++) {
    const int r = b.destOy + p.cy[k], c = b.destCol + p.cx[k];
    b.occ[r] |= 1ULL << c;
    b.colour[r][c] = b.piece;
  }
  b.clearMask = 0;
  for (int r = 0; r < b.rows; r++)
    if (b.occ[r] == fullRow(b)) b.clearMask |= 1u << r;
  if (b.clearMask) {
    b.phase = G_CLEARING;
    b.clearFlash = CLEAR_TICKS;
  } else {
    b.phase = G_DELAY;
    b.wait = PIECE_DELAY_TICKS;
  }
  return false;
}

// --- Redraw areas ----------------------------------------------------------------

// What moves now, object coordinates: the piece, the digit being built, bouncing digits, flashing rows.
static int motion(const BlocksRt& b, lv_area_t* out) {
  int n = 0;
  if (b.phase == G_MOVING) {
    const Rot& p = ROTS[b.drawRot];
    const int x = b.gx + (int)lroundf(b.col) * b.Q, y = (int)floorf(b.py);
    const int s = LV_MAX(p.w, p.h) * b.Q;  // a tumble may turn it either way
    out[n++] = {x - b.Q, y - 1, x + s + b.Q, y + s + 1};
  }
  if (b.active >= 0) {
    int top = b.dy0;
    for (int k = 0; k < b.nDots; k++)
      if (b.dotTick >= b.dots[k].at && !b.dots[k].locked) top = LV_MIN(top, (int)b.dots[k].y);
    out[n++] = {b.dx[b.active], top - 1, b.dx[b.active] + 5 * b.P, b.dy0 + 7 * b.P};
  }
  const int lift = (int)(BOUNCE_V0 * BOUNCE_V0 / (2 * BOUNCE_G) * b.P / 3) + b.P;
  for (int i = 0; i < 5; i++)
    if (b.bounceY[i] != 0 || b.bounceV[i] != 0) out[n++] = {b.dx[i], b.dy0 - lift, b.dx[i] + 5 * b.P, b.dy0 + 7 * b.P};
  if (b.phase == G_CLEARING) out[n++] = {b.gx, b.gy, b.gx + b.cols * b.Q, b.gy + b.rows * b.Q};
  return n;
}

static void invalidate(lv_obj_t* o, const lv_area_t* a, int n) {
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  for (int k = 0; k < n; k++) {
    lv_area_t s = a[k];
    lv_area_move(&s, c.x1, c.y1);
    lv_obj_invalidate_area(o, &s);
  }
}

// One animation frame. cur/next are "HH:MM" now and a minute on; false when nothing changed.
bool blocksTick(BlocksRt* b, lv_obj_t* o, uint32_t now, bool live, bool lit, const char* cur, const char* next, int sec,
                int32_t minute) {
  if (!b->P) return false;
  bool changed = false;
  if (lit != b->colonLit) {
    b->colonLit = lit;
    const lv_area_t a = {b->colonX, b->dy0 + 2 * b->P, b->colonX + b->P, b->dy0 + 5 * b->P};
    invalidate(o, &a, 1);
    changed = true;
  }
  if (!live) {
    b->lastMs = 0;
    if (b->active >= 0) finishAll(*b);
    if (memcmp(b->shown, cur, 5)) {
      memcpy(b->shown, cur, 6);
      lv_obj_invalidate(o);
      changed = true;
    }
    return changed;
  }
  if (b->active < 0) {
    if (sec >= TRIGGER_SEC && b->triggered != minute) {
      b->triggered = minute;
      if (memcmp(b->shown, next, 5)) startRebuild(*b, next);
    } else {
      const char* want = b->triggered == minute ? next : cur;
      if (memcmp(b->shown, want, 5)) startRebuild(*b, want);
    }
  }
  uint32_t ticks;
  if (!b->lastMs || now - b->lastMs > 5 * TICK_MS) {
    b->lastMs = now;
    ticks = 1;
  } else {
    ticks = (now - b->lastMs) / TICK_MS;
    b->lastMs += ticks * TICK_MS;
  }
  if (!ticks) return changed;
  lv_area_t a[12];
  int n = motion(*b, a);
  bool whole = false;
  for (uint32_t t = 0; t < ticks; t++) {
    stepRebuild(*b);
    stepBounce(*b);
    whole |= stepGame(*b);
  }
  n += motion(*b, a + n);
  if (whole) a[n++] = {b->gx, b->gy, b->gx + b->cols * b->Q, b->gy + b->rows * b->Q};
  invalidate(o, a, n);
  return changed || n > 0;
}

// --- Drawing ---------------------------------------------------------------------

static void block(lv_layer_t* L, int x, int y, int s, lv_color_t col) {
  const lv_area_t a = {x, y, x + s - 1, y + s - 1};
  const lv_area_t& c = L->_clip_area;
  if (a.x2 < c.x1 || a.x1 > c.x2 || a.y2 < c.y1 || a.y1 > c.y2) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = col;
  d.bg_opa = LV_OPA_COVER;
  lv_draw_rect(L, &d, &a);
}

void blocksDraw(const BlocksRt* b, lv_layer_t* L, int ox, int oy, lv_color_t col) {
  if (!b->P) return;
  const lv_area_t& clip = L->_clip_area;
  const int P = b->P, s = b->blk;
  for (int i : {0, 1, 3, 4}) {
    const int x = ox + b->dx[i];
    if (x + 5 * P < clip.x1 || x > clip.x2) continue;
    if (i == b->active) {
      for (int k = 0; k < b->nDots; k++) {
        const Dot& d = b->dots[k];
        if (b->dotTick >= d.at) block(L, ox + d.tx, oy + (int)d.y, s, col);
      }
      continue;
    }
    const char v = b->shown[i];
    if (v < '0' || v > '9') continue;
    const int y = oy + b->dy0 + (int)lroundf(b->bounceY[i]);
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 5; c++)
        if ((DIG[v - '0'][r] >> (4 - c)) & 1) block(L, x + c * P, y + r * P, s, col);
  }
  if (b->colonLit) {
    block(L, ox + b->colonX, oy + b->dy0 + 2 * P, s, col);
    block(L, ox + b->colonX, oy + b->dy0 + 4 * P, s, col);
  }
  // The strip: only the rows and columns inside the area being drawn.
  const int Q = b->Q, gx = ox + b->gx, gy = oy + b->gy;
  const int r0 = LV_MAX(0, (clip.y1 - gy) / Q), r1 = LV_MIN(b->rows - 1, (clip.y2 - gy) / Q);
  const int c0 = LV_MAX(0, (clip.x1 - gx) / Q), c1 = LV_MIN(b->cols - 1, (clip.x2 - gx) / Q);
  const bool hideClear = b->phase == G_CLEARING && (b->clearFlash / 5) % 2 == 0;
  for (int r = r0; r <= r1; r++) {
    if (!b->occ[r] || (hideClear && (b->clearMask & (1u << r)))) continue;
    for (int c = c0; c <= c1; c++)
      if (b->occ[r] & (1ULL << c)) block(L, gx + c * Q, gy + r * Q, b->qblk, lv_color_hex(PIECE_COL[b->colour[r][c]]));
  }
  if (b->phase == G_MOVING) {
    const Rot& p = ROTS[b->drawRot];
    const int px = gx + (int)lroundf(b->col) * Q, py = oy + (int)floorf(b->py);
    for (int k = 0; k < 4; k++)
      block(L, px + p.cx[k] * Q, py + p.cy[k] * Q, b->qblk, lv_color_hex(PIECE_COL[b->piece]));
  }
}

}  // namespace deckui
