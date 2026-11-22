// Deck UI: the Falling Blocks clock style. 5x7 block digits rebuilt dot by dot
// at the minute, over a strip that plays the block game by itself (Smooth play).
// Motion runs on the 16 ms tick its constants were tuned for, scaled to the tile.
#include <math.h>

#include "deck_ui_clock.h"

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

namespace {
struct Blocks : ClockScene {
  int16_t w, h, P, blk, Q, qblk;  // digit pitch and block, strip cell and block
  int16_t dx[5], dy0, colonX, gx, gy;
  uint8_t rows, cols;
  uint64_t occ[MAX_ROWS];
  uint8_t colour[MAX_ROWS][MAX_COLS];
  char shown[6] = "  :  ";
  bool colonLit = true;
  // Rebuild: the changed digits of `to`, one at a time, left to right.
  char to[6];
  uint8_t queue[4], qLen, qPos;
  int8_t active = -1;
  Dot dots[MAX_DOTS];
  uint8_t nDots;
  uint16_t dotTick;
  float bounceY[5], bounceV[5];
  int32_t triggered = -1;  // epoch minute whose :56 started a rebuild
  // Game.
  uint8_t phase, piece, rot, drawRot, spins, spinT, clearFlash;
  uint16_t wait;
  float col, py;
  int8_t destCol, destOy;
  uint32_t clearMask;
  uint32_t lastMs;

  bool fit(int tw, int th, int pad) override;
  bool tick(lv_obj_t* o, const ClockNow& n) override;
  void draw(lv_layer_t* L, int ox, int oy, lv_color_t ink) override;
};
}  // namespace

ClockScene* blocksScene() { return newScene<Blocks>(); }

static uint64_t fullRow(const Blocks& b) { return b.cols >= 64 ? ~0ULL : (1ULL << b.cols) - 1; }

static void gameReset(Blocks& b) {
  memset(b.occ, 0, sizeof(b.occ));
  b.phase = G_DELAY;
  b.wait = PIECE_DELAY_TICKS;
  b.clearMask = 0;
}

bool Blocks::fit(int tw, int th, int pad) {
  const int iw = tw - 2 * pad, ih = th - 2 * pad;
  const int p = LV_MIN(iw / 29, ih * 45 / 100 / 7);
  if (p < 3 || iw / 29 < 5) return false;  // too short, or 1x1 narrow: Roll
  const int q = (p * 4 + 1) / 3, top = pad + p, strip = top + 8 * p;
  const int nr = LV_MIN(MAX_ROWS, (th - pad - strip) / q), nc = LV_MIN(MAX_COLS, iw / q);
  if (nr < 3 || nc < 8) return false;
  const bool same = w == tw && h == th;
  w = (int16_t)tw;
  h = (int16_t)th;
  P = (int16_t)p;
  blk = (int16_t)(p - LV_MAX(1, p / 3));
  Q = (int16_t)q;
  qblk = (int16_t)(q - LV_MAX(1, q / 4));
  for (int i = 0; i < 5; i++) dx[i] = (int16_t)((tw - 29 * p) / 2 + i * 6 * p);
  colonX = (int16_t)((dx[1] + 4 * p + dx[3]) / 2);
  dy0 = (int16_t)top;
  rows = (uint8_t)nr;
  cols = (uint8_t)nc;
  gx = (int16_t)((tw - nc * q) / 2);
  gy = (int16_t)(th - pad - nr * q);
  if (!same) gameReset(*this);
  return true;
}

// --- Digit rebuild ---------------------------------------------------------------

static void startDigit(Blocks& b) {
  b.active = -1;
  while (b.qPos < b.qLen) {
    const int i = b.queue[b.qPos];
    const char v = b.to[i];
    b.nDots = 0;
    b.dotTick = 0;
    if (v >= '0' && v <= '9')
      for (int r = 6; r >= 0; r--)  // bottom row first, left to right
        for (int c = 0; c < 5; c++)
          if (clockDot(v, r, c)) {
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

static void startRebuild(Blocks& b, const char* to) {
  memcpy(b.to, to, sizeof(b.to));
  b.qLen = b.qPos = 0;
  for (int i : CLOCK_DIGITS)
    if (b.shown[i] != to[i]) b.queue[b.qLen++] = (uint8_t)i;
  startDigit(b);
}

static void finishAll(Blocks& b) {
  for (int k = b.qPos; k < b.qLen; k++) b.shown[b.queue[k]] = b.to[b.queue[k]];
  b.qPos = b.qLen;
  b.active = -1;
}

static void stepRebuild(Blocks& b) {
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

static void stepBounce(Blocks& b) {
  const float g = BOUNCE_G * b.P / 3;
  for (int i = 0; i < 5; i++) {
    if (b.bounceY[i] == 0 && b.bounceV[i] == 0) continue;
    b.bounceV[i] += g * BOUNCE_STEP;
    b.bounceY[i] += b.bounceV[i] * BOUNCE_STEP;
    if (b.bounceY[i] >= 0) b.bounceY[i] = b.bounceV[i] = 0;
  }
}

// --- Block game (Smooth play) ---------------------------------------------------

// The strip before the next piece lands.
struct Skyline {
  int8_t top[MAX_COLS];  // first taken row of each column, rows = empty
  int holes;
};

// Empty cells under a taken one.
static int holesIn(const uint64_t* occ, int rows) {
  uint64_t covered = 0;
  int holes = 0;
  for (int r = 0; r < rows; r++) {
    holes += __builtin_popcountll(covered & ~occ[r]);
    covered |= occ[r];
  }
  return holes;
}

static void skyline(const Blocks& b, Skyline& s) {
  for (int c = 0; c < b.cols; c++) {
    int r = 0;
    while (r < b.rows && !(b.occ[r] & (1ULL << c))) r++;
    s.top[c] = (int8_t)r;
  }
  s.holes = holesIn(b.occ, b.rows);
}

// Resting top row of a rotation dropped at leftCol, negative when it does not fit.
static int dropOy(const Blocks& b, const Skyline& s, const Rot& p, int leftCol) {
  int oy = b.rows;
  for (int k = 0; k < 4; k++) oy = LV_MIN(oy, s.top[leftCol + p.cx[k]] - 1 - p.cy[k]);
  return oy;
}

// Keep the strip low and every column covered: the lowest landing spot wins.
static int scorePlacement(const Blocks& b, const Skyline& s, const Rot& p, int leftCol, int oy, int& holes) {
  uint64_t tmp[MAX_ROWS];
  memcpy(tmp, b.occ, sizeof(uint64_t) * b.rows);
  int8_t top[4];  // of the piece's columns
  for (int c = 0; c < p.w; c++) top[c] = s.top[leftCol + c];
  for (int k = 0; k < 4; k++) {
    tmp[oy + p.cy[k]] |= 1ULL << (leftCol + p.cx[k]);
    top[p.cx[k]] = (int8_t)LV_MIN((int)top[p.cx[k]], oy + p.cy[k]);
  }
  int lines = 0;
  for (int r = oy; r < oy + p.h; r++)
    if (tmp[r] == fullRow(b)) lines++;
  int minTop = b.rows, bumps = 0, prev = -1;
  for (int c = 0; c < b.cols; c++) {
    const int t = c >= leftCol && c < leftCol + p.w ? top[c - leftCol] : s.top[c];
    minTop = LV_MIN(minTop, t);
    if (prev >= 0) bumps += abs(t - prev);
    prev = t;
  }
  holes = holesIn(tmp, b.rows);
  return lines * 1000 + (oy + p.h - 1) * 24 - holes * 60 - (b.rows - minTop) * 6 - bumps * 4;
}

struct Spot {
  int score, rot, col, oy, ties;
  void offer(int sc, int r, int c, int y) {
    if (rot < 0 || sc > score) *this = {sc, r, c, y, 1};
    else if (sc == score && rndInt(++ties) == 0) rot = r, col = c, oy = y;
  }
};

// A random piece to the best spot that buries no new hole (relaxed for a slip
// or when nothing hole-free fits); equal spots are picked at random.
static bool pickPiece(Blocks& b) {
  const bool avoid = rndInt(100) >= (uint32_t)SLIP_PCT;
  const int piece = (int)rndInt(7), r0 = PIECE_ROT[piece][0], rn = PIECE_ROT[piece][1];
  Skyline s;
  skyline(b, s);
  Spot any = {0, -1, 0, 0, 0}, clean = any;
  for (int rot = r0; rot < r0 + rn; rot++)
    for (int c = 0; c + ROTS[rot].w <= b.cols; c++) {
      const int oy = dropOy(b, s, ROTS[rot], c);
      if (oy < 0) continue;
      int holes;
      const int sc = scorePlacement(b, s, ROTS[rot], c, oy, holes);
      any.offer(sc, rot, c, oy);
      if (avoid && holes <= s.holes) clean.offer(sc, rot, c, oy);
    }
  const Spot& t = clean.rot >= 0 ? clean : any;
  if (t.rot < 0) return false;
  b.piece = (uint8_t)piece;
  b.rot = (uint8_t)t.rot;
  b.destCol = (int8_t)t.col;
  b.destOy = (int8_t)t.oy;
  b.drawRot = (uint8_t)(r0 + rndInt(rn));
  b.spins = (uint8_t)(1 + rndInt(4));
  b.spinT = 0;
  b.col = (b.cols - ROTS[t.rot].w) / 2.0f;
  b.py = -(float)(ROTS[t.rot].h * b.Q);  // above the tile, falls through the digits
  b.phase = G_MOVING;
  return true;
}

static void clearRows(Blocks& b) {
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
static bool stepGame(Blocks& b) {
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

// What moves now: the piece, the digit being built, bouncing digits.
static void motion(const Blocks& b, Dirty& out) {
  if (b.phase == G_MOVING) {
    const Rot& p = ROTS[b.drawRot];
    const int x = b.gx + (int)lroundf(b.col) * b.Q, y = (int)floorf(b.py);
    const int s = LV_MAX(p.w, p.h) * b.Q;  // a tumble may turn it either way
    out.add({x - b.Q, y - 1, x + s + b.Q, y + s + 1});
  }
  if (b.active >= 0) {
    int top = b.dy0;
    for (int k = 0; k < b.nDots; k++)
      if (b.dotTick >= b.dots[k].at && !b.dots[k].locked) top = LV_MIN(top, (int)b.dots[k].y);
    out.add({b.dx[b.active], top - 1, b.dx[b.active] + 5 * b.P, b.dy0 + 7 * b.P});
  }
  const int lift = (int)(BOUNCE_V0 * BOUNCE_V0 / (2 * BOUNCE_G) * b.P / 3) + b.P;
  for (int i = 0; i < 5; i++)
    if (b.bounceY[i] != 0 || b.bounceV[i] != 0) out.add({b.dx[i], b.dy0 - lift, b.dx[i] + 5 * b.P, b.dy0 + 7 * b.P});
}

// Whether the full rows are blinked off right now.
static bool clearHidden(const Blocks& b) { return b.phase == G_CLEARING && (b.clearFlash / 5) % 2 == 0; }

static lv_area_t stripRows(const Blocks& b, int r0, int r1) {
  return {b.gx, b.gy + r0 * b.Q, b.gx + b.cols * b.Q, b.gy + (r1 + 1) * b.Q};
}

bool Blocks::tick(lv_obj_t* o, const ClockNow& n) {
  if (!P) return false;
  Dirty d;
  if (n.colon != colonLit) {
    colonLit = n.colon;
    d.add({colonX, dy0 + 2 * P, colonX + P, dy0 + 5 * P});
  }
  if (!n.live) {
    lastMs = 0;
    if (active >= 0) finishAll(*this);
    if (memcmp(shown, n.cur, 5)) {
      memcpy(shown, n.cur, 6);
      d.add({0, 0, w, h});
    }
    return d.flush(o);
  }
  if (active < 0) {
    const bool start = n.sec >= TRIGGER_SEC && triggered != n.minute;
    if (start) triggered = n.minute;
    const char* want = triggered == n.minute ? n.next : n.cur;
    if (memcmp(shown, want, 5)) startRebuild(*this, want);
  }
  uint32_t ticks = 1;
  if (lastMs && n.ms - lastMs <= 5 * TICK_MS) {
    ticks = (n.ms - lastMs) / TICK_MS;
    lastMs += ticks * TICK_MS;
  } else {
    lastMs = n.ms;
  }
  if (!ticks) return d.flush(o);
  motion(*this, d);
  const bool hid = clearHidden(*this);
  bool whole = false;
  for (uint32_t t = 0; t < ticks; t++) {
    stepRebuild(*this);
    stepBounce(*this);
    whole |= stepGame(*this);
  }
  motion(*this, d);
  if (whole) {
    d.add(stripRows(*this, 0, rows - 1));
  } else if (clearHidden(*this) != hid) {
    for (int r = 0; r < rows; r++)
      if (clearMask & (1u << r)) d.add(stripRows(*this, r, r));
  }
  return d.flush(o);
}

// --- Drawing ---------------------------------------------------------------------

static void block(lv_layer_t* L, int x, int y, int s, lv_color_t col) {
  fillRect(L, {x, y, x + s - 1, y + s - 1}, col);
}

void Blocks::draw(lv_layer_t* L, int ox, int oy, lv_color_t ink) {
  if (!P) return;
  const lv_area_t& clip = L->_clip_area;
  const int s = blk;
  for (int i : CLOCK_DIGITS) {
    const int x = ox + dx[i];
    if (x + 5 * P < clip.x1 || x > clip.x2) continue;
    if (i == active) {
      for (int k = 0; k < nDots; k++) {
        const Dot& d = dots[k];
        if (dotTick >= d.at) block(L, ox + d.tx, oy + (int)d.y, s, ink);
      }
      continue;
    }
    const char v = shown[i];
    if (v < '0' || v > '9') continue;
    const int y = oy + dy0 + (int)lroundf(bounceY[i]);
    for (int r = 0; r < 7; r++)
      for (int c = 0; c < 5; c++)
        if (clockDot(v, r, c)) block(L, x + c * P, y + r * P, s, ink);
  }
  if (colonLit) {
    block(L, ox + colonX, oy + dy0 + 2 * P, s, ink);
    block(L, ox + colonX, oy + dy0 + 4 * P, s, ink);
  }
  // The strip: only the rows and columns inside the area being drawn.
  const int sx = ox + gx, sy = oy + gy;
  const int r0 = LV_MAX(0, (clip.y1 - sy) / Q), r1 = LV_MIN(rows - 1, (clip.y2 - sy) / Q);
  const int c0 = LV_MAX(0, (clip.x1 - sx) / Q), c1 = LV_MIN(cols - 1, (clip.x2 - sx) / Q);
  const bool hideClear = clearHidden(*this);
  for (int r = r0; r <= r1; r++) {
    if (!occ[r] || (hideClear && (clearMask & (1u << r)))) continue;
    for (int c = c0; c <= c1; c++)
      if (occ[r] & (1ULL << c)) block(L, sx + c * Q, sy + r * Q, qblk, lv_color_hex(PIECE_COL[colour[r][c]]));
  }
  if (phase == G_MOVING) {
    const Rot& p = ROTS[drawRot];
    const int px = sx + (int)lroundf(col) * Q, top = oy + (int)floorf(py);
    for (int k = 0; k < 4; k++)
      block(L, px + p.cx[k] * Q, top + p.cy[k] * Q, qblk, lv_color_hex(PIECE_COL[piece]));
  }
}

}  // namespace deckui
