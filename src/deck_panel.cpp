// Frame compositor shared by every display: LVGL frames, page slides and
// card/frame animations built in native orientation and panel byte order for
// the board's transport (deck_panel_<bus>.cpp, contract in deck_panel_int.h).
#include "deck_panel_int.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>

namespace deck {
namespace {

// Pixels in panel byte order (PANEL_BIG_ENDIAN) from little-endian RGB565
// (LVGL), and back: its own inverse.
inline uint16_t pnl(uint16_t c) { return PANEL_BIG_ENDIAN ? __builtin_bswap16(c) : c; }

constexpr int64_t SYNC_STALE_US = 50000;
constexpr int64_t SCRIM_IN_US = 50000;  // the scrim's fade for a card begun before its picture

uint16_t* s_fb[2] = {nullptr, nullptr};
TaskHandle_t s_task = nullptr;
// Native rotation: the user rotation + BOARD_ROT_BASE.
volatile uint8_t s_rot = 1;

// --- Shared frame / compositor state (s_mux) ----------------------------------
SemaphoreHandle_t s_lvReady = nullptr;
SemaphoreHandle_t s_quiet = nullptr;
bool s_quiescing = false;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Page dots: shown while a page slide owns the screen, then held and faded
// out over LVGL frames by this task (LVGL never draws them). Horizontal dots
// at the bottom count columns, vertical ones at the right edge count the
// pages of the column.
constexpr int64_t DOTS_HOLD_US = 1200000;
constexpr int64_t DOTS_FADE_US = 300000;

struct State {
  FrameScene scene = {};
  uint8_t frameDots = 0;
  bool frameDone = false;
  bool frameResult = false;
  PanelStats frameStats = {};
  bool lvPending = false;
  DirtyMask lvDirty = {{}, true};  // LVGL areas since the last plain frame went out
  int lvIdx = 0;
  uint32_t lvGen = 0;
  int lvLatest = -1;
  CompMode mode = COMP_NONE;
  Kind kind = KIND_HSTRIP;
  Src a = {nullptr, 0, true};
  Src b = {nullptr, 0, true};
  int pageA = 0;
  int off = 0;
  int lastSentOff = 0;
  bool sentAny = false;
  int fromOff = 0;
  int toOff = 0;
  int64_t t0 = 0;
  uint32_t durUs = 1;
  int8_t landing = 0;
  PageFx pageFx = PAGE_SLIDE;  // of this strip
  MorphScene morph = {};
  uint32_t morphHoldGen = 0;
  bool morphEarly = false;    // begun without the card picture
  bool morphStill = false;    // ... and the scrim's last frame is out
  int64_t morphCardUs = 0;    // when the picture arrived
  uint32_t holdGen = UINT32_MAX;
  bool landedPending = false;
  int8_t landed = 0;
  // pager / dots
  int pageCount = 1;
  int pageActive = 0;
  int vCount = 1;
  int vActive = 0;
  bool dotsV = false;       // the last slide was vertical: show column dots
  bool dotsForce = false;
  int64_t dotsUntil = 0;
  uint8_t lastSentDots = 0;
};
State s;
volatile PageFx s_pageFx = PAGE_SLIDE;


// --- Stats (approximate, single writer) ----------------------------------------
volatile uint32_t st_frames = 0, st_comp = 0, st_flushSum = 0, st_flushMax = 0;
volatile uint32_t st_buildSum = 0, st_teLate = 0, st_teTimeout = 0, st_lvDropped = 0;
volatile uint32_t st_gapMax = 0, st_frameSum = 0, st_frameMax = 0;
PanelStats s_frameStats = {};
uint64_t s_frameSum = 0, s_frameBuildSum = 0, s_frameFlushSum = 0;
int64_t s_frameStart = 0;
int64_t s_lastStartUs = 0;
bool s_lastWasComp = false;


inline void logicalSize(uint8_t rot, int& w, int& h) {
  if (rot & 1) { w = NATIVE_H; h = NATIVE_W; }
  else         { w = NATIVE_W; h = NATIVE_H; }
}

// --- Chunk build ----------------------------------------------------------------
// Dim levels for the page under a pulled-down panel, applied in little-endian
// RGB565: 1 = 75 %, 2 = 50 % (matches LVGL's 50 % black scrim).
template <uint8_t DIM>
inline uint16_t dimLE(uint16_t c) {
  if (DIM == 1) return (uint16_t)(c - ((c >> 2) & 0x39E7));
  if (DIM == 2) return (uint16_t)((c >> 1) & 0x7BEF);
  return c;
}

// Copy n logical pixels (read ascending from s) to d, walking dstep.
template <bool SWAP, uint8_t DIM>
inline void copyRun(uint16_t* d, int dstep, const uint16_t* s, int n) {
  for (int k = 0; k < n; k++) {
    uint16_t c = s[k];
    if (DIM) {
      if (!SWAP) c = pnl(c);
      c = dimLE<DIM>(c);
      if (!SWAP) c = pnl(c);
    }
    d[k * dstep] = SWAP ? pnl(c) : c;
  }
}

inline void copyAny(uint16_t* d, int dstep, const uint16_t* s, int n, bool swap, uint8_t dim) {
  if (!PANEL_BIG_ENDIAN && dstep == 1 && dim == 0) {
    memcpy(d, s, (size_t)n * 2);
    return;
  }
  if (swap) {
    if (dim == 0) copyRun<true, 0>(d, dstep, s, n);
    else if (dim == 1) copyRun<true, 1>(d, dstep, s, n);
    else copyRun<true, 2>(d, dstep, s, n);
  } else {
    if (dim == 0) copyRun<false, 0>(d, dstep, s, n);
    else if (dim == 1) copyRun<false, 1>(d, dstep, s, n);
    else copyRun<false, 2>(d, dstep, s, n);
  }
}

// RGB565 with green moved to the top half: blends a pixel with 5-bit alpha in one multiply.
inline uint32_t spread565(uint16_t c) { return (c | (uint32_t)c << 16) & 0x07E0F81F; }
inline uint16_t mix565(uint16_t a, uint16_t b, uint32_t a5) {
  const uint32_t sa = spread565(a);
  const uint32_t x = (((spread565(b) - sa) * a5 >> 5) + sa) & 0x07E0F81F;
  return (uint16_t)(x | x >> 16);
}

// Sources are read little-endian; `swap` says they are stored that way already.
template <bool SA, bool SB>
void blendRun(uint16_t* d, int step, const uint16_t* a, const uint16_t* b, int n, uint32_t a5) {
  int k = 0;
  // Two pixels per PSRAM load when both runs are word aligned (the usual case).
  if (!(((uintptr_t)a | (uintptr_t)b) & 3)) {
    const uint32_t* a2 = (const uint32_t*)a;
    const uint32_t* b2 = (const uint32_t*)b;
    for (; k + 1 < n; k += 2, d += 2 * step) {
      const uint32_t wa = *a2++, wb = *b2++;
      const uint16_t a0 = SA ? (uint16_t)wa : pnl((uint16_t)wa);
      const uint16_t a1 = SA ? (uint16_t)(wa >> 16) : pnl((uint16_t)(wa >> 16));
      const uint16_t b0 = SB ? (uint16_t)wb : pnl((uint16_t)wb);
      const uint16_t b1 = SB ? (uint16_t)(wb >> 16) : pnl((uint16_t)(wb >> 16));
      d[0] = pnl(mix565(a0, b0, a5));
      d[step] = pnl(mix565(a1, b1, a5));
    }
  }
  for (; k < n; k++, d += step) {
    const uint16_t ca = SA ? a[k] : pnl(a[k]);
    const uint16_t cb = SB ? b[k] : pnl(b[k]);
    *d = pnl(mix565(ca, cb, a5));
  }
}

// a5 = 0 is all A, 32 all B.
inline void mixAny(uint16_t* d, int step, const uint16_t* a, bool sa, const uint16_t* b, bool sb, int n,
                   uint32_t a5) {
  if (a5 == 0) copyAny(d, step, a, n, sa, 0);
  else if (a5 >= 32) copyAny(d, step, b, n, sb, 0);
  else if (sa && sb) blendRun<true, true>(d, step, a, b, n, a5);
  else if (sa) blendRun<true, false>(d, step, a, b, n, a5);
  else if (sb) blendRun<false, true>(d, step, a, b, n, a5);
  else blendRun<false, false>(d, step, a, b, n, a5);
}

inline uint16_t scale565(uint16_t c, uint32_t a5) {
  const uint32_t x = (spread565(c) * a5 >> 5) & 0x07E0F81F;
  return (uint16_t)(x | x >> 16);
}

// n pixels of s scaled by a5 (0..32), panel byte order out.
template <bool SWAP>
void scaleRun(uint16_t* d, int step, const uint16_t* s, int n, uint32_t a5) {
  for (int k = 0; k < n; k++, d += step) *d = pnl(scale565(SWAP ? s[k] : pnl(s[k]), a5));
}

inline void scaleAny(uint16_t* d, int step, const uint16_t* s, int n, bool swap, uint32_t a5) {
  if (n <= 0) return;
  if (a5 >= 32) copyAny(d, step, s, n, swap, 0);
  else if (swap) scaleRun<true>(d, step, s, n, a5);
  else scaleRun<false>(d, step, s, n, a5);
}

// c at l/8 brightness (l = 4..8), shifts only: the scrim's fade steps.
inline uint16_t shade8(uint16_t c, uint32_t l) {
  switch (l) {
    case 8: return c;
    case 7: return (uint16_t)(c - ((c >> 3) & 0x18E3));
    case 6: return dimLE<1>(c);
    case 5: return (uint16_t)(((c >> 1) & 0x7BEF) + ((c >> 3) & 0x18E3));
    default: return dimLE<2>(c);
  }
}

template <bool SWAP, uint32_t L>
void shadeRun(uint16_t* d, int step, const uint16_t* s, int n) {
  for (int k = 0; k < n; k++, d += step) *d = pnl(shade8(SWAP ? s[k] : pnl(s[k]), L));
}

inline void shadeAny(uint16_t* d, int step, const uint16_t* s, int n, bool swap, uint32_t l) {
  if (n <= 0) return;
  if (l == 7) swap ? shadeRun<true, 7>(d, step, s, n) : shadeRun<false, 7>(d, step, s, n);
  else if (l == 5) swap ? shadeRun<true, 5>(d, step, s, n) : shadeRun<false, 5>(d, step, s, n);
  else copyAny(d, step, s, n, swap, l >= 8 ? 0 : l == 6 ? 1 : 2);
}

// Per-frame values of a frame scene, set before chunk 0 (flush task only).
struct FramePrep {
  uint32_t photo5, clock8;
  int count;
};
FramePrep s_fp;

void prepFrame(const Frame& f) {
  const FrameScene& sc = f.scene;
  const int tilesMs = (int)frameTilesMs(sc.count), step = (int)frameTileStep(sc.count);
  const int fadeMs = (int)f.durationMs - tilesMs;
  // Leaving: tiles first, then the fade; waking: the fade first, then tiles.
  const int el = (int)f.elapsedMs;
  const int fadeAt = sc.waking ? el : el - tilesMs, tileAt = sc.waking ? el - fadeMs : el;
  const uint32_t progress = f.frameLast || fadeAt >= fadeMs ? 256 : fadeAt <= 0 ? 0 : fadeAt * 256 / fadeMs;
  const uint32_t photo = sc.waking ? 256 - progress : progress;
  s_fp.photo5 = (photo + 4) >> 3;
  s_fp.clock8 = sc.clockFade ? photo : 256;
  s_fp.count = 0;
  for (int i = 0; i < sc.count && sc.runs; i++) {
    const FrameRect& r = sc.rects[i];
    const int t = tileAt - i * step;
    const int p = f.frameLast ? 256 : (t <= 0 ? 0 : t >= (int)FRAME_TILE_MS ? 256 : t * 256 / (int)FRAME_TILE_MS);
    const int alpha = sc.waking ? p : 256 - p;
    const int a5 = (alpha + 4) >> 3;
    if (!a5) continue;
    const int scale = 218 + (38 * alpha >> 8);
    const int w = (r.w * scale + 128) >> 8, h = (r.h * scale + 128) >> 8;
    if (!w || !h) continue;
    FrameTileRun& run = sc.runs[s_fp.count++];
    run.left = r.x + (r.w - w) / 2;
    run.top = r.y + (r.h - h) / 2;
    run.w = w;
    run.h = h;
    run.srcX = r.x;
    run.srcY = r.y;
    run.dx = ((uint32_t)r.w << 16) / w;
    run.dy = ((uint32_t)r.h << 16) / h;
    run.a5 = a5;
  }
}

void emitFrameRow(const Frame& f, int y, int x0, int n, uint16_t* d, int step) {
  const FrameScene& sc = f.scene;
  const uint32_t a5 = s_fp.photo5;
  const Src& base = sc.base;
  const Src& pic = sc.picture;
  const uint16_t* pa = base.px + y * base.stride + x0;
  const uint16_t* pb = pic.px + y * pic.stride + x0;
  mixAny(d, step, pa, base.swap, pb, pic.swap, n, a5);
  const Src& page = sc.page;
  for (int i = 0; i < s_fp.count; i++) {
    const FrameTileRun& r = sc.runs[i];
    if (y < r.top || y >= r.top + r.h) continue;
    const int first = x0 > r.left ? x0 : r.left;
    const int end = x0 + n < r.left + r.w ? x0 + n : r.left + r.w;
    if (first >= end) continue;
    const uint16_t* row = page.px + (r.srcY + ((uint32_t)(y - r.top) * r.dy >> 16)) * page.stride + r.srcX;
    uint32_t sx = (first - r.left) * r.dx;
    uint16_t* out = d + (first - x0) * step;
    for (int x = first; x < end; x++, sx += r.dx, out += step) {
      const uint16_t c = row[sx >> 16];
      *out = pnl(mix565(pnl(*out), page.swap ? c : pnl(c), r.a5));
    }
  }
  const FrameRect& c = sc.clockRect;
  if (sc.clock && s_fp.clock8 && y >= c.y && y < c.y + c.h) {
    const int first = x0 > c.x ? x0 : c.x;
    const int end = x0 + n < c.x + c.w ? x0 + n : c.x + c.w;
    const uint32_t* row = sc.clock + (y - c.y) * sc.clockStride;
    for (int x = first; x < end; x++) {
      const uint32_t px = row[x - c.x];
      const uint32_t ca5 = ((px >> 24) * s_fp.clock8 + 1024) >> 11;
      if (!ca5) continue;
      const uint16_t fg = ((px >> 8) & 0xF800) | ((px >> 5) & 0x07E0) | ((px >> 3) & 0x001F);
      uint16_t& out = d[(x - x0) * step];
      out = pnl(mix565(pnl(out), fg, ca5));
    }
  }
}

// Card window pixels (both sources little-endian): nearest-scaled card, optional
// ghost tint, optional fade over the dimmed page.
template <bool GHOST, bool OVER>
void morphRun(uint16_t* out, int step, const uint16_t* row, uint32_t sx, uint32_t dx, int n,
              const uint16_t* base, uint16_t ghost, uint32_t wa5, uint32_t wo5, uint32_t base8) {
  const uint32_t gs = spread565(ghost) * (32 - wa5);  // the ghost's share, the same for every pixel
  for (int k = 0; k < n; k++, sx += dx, out += step) {
    uint16_t px = row[sx >> 16];
    if (GHOST) {
      const uint32_t x = ((spread565(px) * wa5 + gs) >> 5) & 0x07E0F81F;
      px = (uint16_t)(x | x >> 16);
    }
    if (OVER) {
      px = mix565(shade8(base[k], base8), px, wo5);
    }
    *out = pnl(px);
  }
}

// Closing, the card all ghost and fading out: the ghost colour over the
// shaded page (gw = the ghost's share, spread565(ghost) * wo5).
template <uint32_t L>
void ghostFadeRun(uint16_t* out, int step, const uint16_t* base, bool swap, int n, uint32_t gw, uint32_t wo5) {
  for (int k = 0; k < n; k++, out += step) {
    const uint16_t b = shade8(swap ? base[k] : pnl(base[k]), L);
    const uint32_t x = ((spread565(b) * (32 - wo5) + gw) >> 5) & 0x07E0F81F;
    *out = pnl((uint16_t)(x | x >> 16));
  }
}

void ghostFade(uint16_t* out, int step, const uint16_t* base, bool swap, int n, uint16_t ghost, uint32_t wo5,
               uint32_t base8) {
  const uint32_t gw = spread565(ghost) * wo5;
  switch (base8) {
    case 8: ghostFadeRun<8>(out, step, base, swap, n, gw, wo5); break;
    case 7: ghostFadeRun<7>(out, step, base, swap, n, gw, wo5); break;
    case 6: ghostFadeRun<6>(out, step, base, swap, n, gw, wo5); break;
    case 5: ghostFadeRun<5>(out, step, base, swap, n, gw, wo5); break;
    default: ghostFadeRun<4>(out, step, base, swap, n, gw, wo5); break;
  }
}

// Pixels a rounded rect (radius rad) leaves out at each end of its row ly.
inline int cornerInset(const FrameRect& a, int rad, int ly) {
  const int top = ly - a.y, bottom = a.y + a.h - 1 - ly;
  const int e = top < rad ? top : bottom < rad ? bottom : -1;
  if (e < 0) return 0;
  const float dy = rad - e - 0.5f;
  return rad - (int)sqrtf((float)(rad * rad) - dy * dy);
}

// Reveal: the card in place inside a circle of radius wr around (cx, cy).
void emitRevealRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const MorphScene& m = f.morph;
  const FrameRect& c = m.to;
  const uint16_t* base = m.base.px + ly * m.base.stride;
  const int end = x0 + n, dy = ly - m.cy;
  int l = end, r = end;
  if (ly >= c.y && ly < c.y + c.h && dy > -f.wr && dy < f.wr) {
    const int half = (int)sqrtf((float)(f.wr * f.wr - dy * dy));
    const int inset = cornerInset(c, m.r1, ly);
    l = std::min(std::max({x0, c.x + inset, m.cx - half}), end);
    r = std::min(std::max(l, std::min(c.x + c.w - inset, m.cx + half + 1)), end);
  }
  if (l > x0) shadeAny(d, step, base + x0, l - x0, m.base.swap, f.base8);
  if (r > l) copyAny(d + (l - x0) * step, step, m.card.px + (ly - c.y) * m.card.stride + (l - c.x), r - l, m.card.swap, 0);
  if (end > r) shadeAny(d + (r - x0) * step, step, base + r, end - r, m.base.swap, f.base8);
}

// Sheet: the card at `to` with its top moved to wy.
void emitSheetRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const MorphScene& m = f.morph;
  const FrameRect c = {m.to.x, f.wy, m.to.w, m.to.h};
  const uint16_t* base = m.base.px + ly * m.base.stride;
  const int end = x0 + n;
  int l = end, r = end;
  if (ly >= c.y && ly < c.y + c.h) {
    const int inset = cornerInset(c, m.r1, ly);
    l = std::min(std::max(x0, c.x + inset), end);
    r = std::min(std::max(l, c.x + c.w - inset), end);
  }
  if (l > x0) shadeAny(d, step, base + x0, l - x0, m.base.swap, f.base8);
  if (r > l) copyAny(d + (l - x0) * step, step, m.card.px + (ly - c.y) * m.card.stride + (l - c.x), r - l, m.card.swap, 0);
  if (end > r) shadeAny(d + (r - x0) * step, step, base + r, end - r, m.base.swap, f.base8);
}

void emitMorphRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const MorphScene& m = f.morph;
  if (!m.card.px) {  // begun early, no picture yet: the page under the scrim
    shadeAny(d, step, m.base.px + ly * m.base.stride + x0, n, m.base.swap, f.base8);
    return;
  }
  if (m.reveal) {
    emitRevealRow(f, ly, x0, n, d, step);
    return;
  }
  if (m.sheet) {
    emitSheetRow(f, ly, x0, n, d, step);
    return;
  }
  const uint16_t* base = m.base.px + ly * m.base.stride;
  const int end = x0 + n;
  const uint32_t b8 = f.base8;
  int l = end, r = end;  // window span on this row
  if (ly >= f.wy && ly < f.wy + f.wh) {
    const int top = ly - f.wy, bottom = f.wy + f.wh - 1 - ly;
    const int e = top < f.wr ? top : bottom < f.wr ? bottom : -1;
    int inset = 0;
    if (e >= 0) {
      const float dy = f.wr - e - 0.5f;
      inset = f.wr - (int)sqrtf((float)(f.wr * f.wr) - dy * dy);
    }
    l = std::min(std::max(x0, f.wx + inset), end);
    r = std::min(std::max(l, f.wx + f.ww - inset), end);
  }
  if (l > x0) shadeAny(d, step, base + x0, l - x0, m.base.swap, b8);
  if (r > l && f.wa5 == 0 && f.wo5 < 32) {
    ghostFade(d + (l - x0) * step, step, base + l, m.base.swap, r - l, m.ghost, f.wo5, b8);
  } else if (r > l && m.card.swap && m.base.swap) {
    const uint16_t* row = m.card.px + ((uint32_t)(ly - f.wy) * f.wdy >> 16) * m.card.stride;
    const uint32_t sx = (uint32_t)(l - f.wx) * f.wdx;
    uint16_t* out = d + (l - x0) * step;
    const bool ghost = f.wa5 < 32, over = f.wo5 < 32;
    if (ghost && !over && f.wa5 == 0) {  // still all ghost
      const uint16_t g = pnl(m.ghost);
      for (int k = l; k < r; k++, out += step) *out = g;
    } else if (ghost && over) morphRun<true, true>(out, step, row, sx, f.wdx, r - l, base + l, m.ghost, f.wa5, f.wo5, b8);
    else if (ghost) morphRun<true, false>(out, step, row, sx, f.wdx, r - l, base + l, m.ghost, f.wa5, f.wo5, b8);
    else if (over) morphRun<false, true>(out, step, row, sx, f.wdx, r - l, base + l, m.ghost, f.wa5, f.wo5, b8);
    else morphRun<false, false>(out, step, row, sx, f.wdx, r - l, base + l, m.ghost, f.wa5, f.wo5, b8);
  } else if (r > l) {
    const uint16_t* row = m.card.px + ((uint32_t)(ly - f.wy) * f.wdy >> 16) * m.card.stride;
    uint32_t sx = (uint32_t)(l - f.wx) * f.wdx;
    uint16_t* out = d + (l - x0) * step;
    for (int x = l; x < r; x++, sx += f.wdx, out += step) {
      const uint16_t c = row[sx >> 16];
      uint16_t px = m.card.swap ? c : pnl(c);
      if (f.wa5 < 32) px = mix565(m.ghost, px, f.wa5);
      if (f.wo5 < 32) {
        uint16_t b = m.base.swap ? base[x] : pnl(base[x]);
        px = mix565(shade8(b, b8), px, f.wo5);
      }
      *out = pnl(px);
    }
  }
  if (end > r) shadeAny(d + (r - x0) * step, step, base + r, end - r, m.base.swap, b8);
}

// Water edge: both pages nearly in place (A drifts away and dims, B drifts
// in), split along a wavy edge with a light line on it. Per-frame values and
// the edge per row (horizontal strip) or per column (vertical strip) are set
// before chunk 0 (flush task only).
struct WaterPrep {
  int16_t edge[MAX_DIM];
  int16_t lo, hi;  // vertical strip: the edge's range over all columns
  int sa, sb;      // A's and B's drift in px
  uint32_t a5;     // A's brightness, 0..32
  uint32_t line5;  // light line strength, 0..32
};
WaterPrep s_wp;

void prepWater(const Frame& f) {
  const bool v = f.kind == KIND_VSTRIP;
  const int full = v ? f.h : f.w, len = v ? f.w : f.h;
  const float e = (float)f.off / (float)full, wave = sinf((float)M_PI * e);
  const float amp = 24 * wave, ph = 7 * e;
  // sin(i * k + p) for i = 0, 1, ...: a vector turned by k each step. This
  // runs before chunk 0, in the short gap before the next TE; sinf per
  // column did not fit.
  struct Osc {
    float c, s, dc, ds;
    float next() {
      const float v = s, c2 = c * dc - s * ds;
      s = s * dc + c * ds;
      c = c2;
      return v;
    }
  };
  Osc w1 = {cosf(ph), sinf(ph), cosf(.021f), sinf(.021f)};
  Osc w2 = {cosf(-1.6f * ph), sinf(-1.6f * ph), cosf(.053f), sinf(.053f)};
  s_wp.lo = INT16_MAX;
  s_wp.hi = INT16_MIN;
  for (int i = 0; i < len; i++) {
    const int16_t e1 = (int16_t)lroundf((float)(full - f.off) + amp * (w1.next() + .45f * w2.next()));
    s_wp.edge[i] = e1;
    s_wp.lo = std::min(s_wp.lo, e1);
    s_wp.hi = std::max(s_wp.hi, e1);
  }
  s_wp.sa = (int)lroundf(26 * e);
  s_wp.sb = (int)lroundf(26 * (1 - e));
  s_wp.a5 = 32 - (uint32_t)lroundf(11.2f * e);  // 35 % darker at the end
  s_wp.line5 = (uint32_t)lroundf(29 * wave);
}

// One pixel at `dist` from the edge: A dimmed before it (only that page is
// read), B after it, the light line across it.
inline uint16_t waterPx(const Src& A, const uint16_t* pa, const Src& B, const uint16_t* pb, int dist) {
  uint16_t c;
  if (dist < 0) c = scale565(A.swap ? *pa : pnl(*pa), s_wp.a5);
  else c = B.swap ? *pb : pnl(*pb);
  if (s_wp.line5 && dist >= -2 && dist <= 1)
    c = mix565(c, 0xFFFF, dist == -1 || dist == 0 ? s_wp.line5 : s_wp.line5 / 2);
  return pnl(c);
}

// Runs of A (dimmed, drifted by sa) for logical x in [x0, x1) of row ra; the
// drift runs past the right edge, where the last column repeats.
void waterRunA(const Frame& f, const uint16_t* ra, int x0, int x1, uint16_t* d, int step) {
  const int inside = std::min(x1, f.w - s_wp.sa);
  if (inside > x0) scaleAny(d, step, ra + x0 + s_wp.sa, inside - x0, f.a.swap, s_wp.a5);
  for (int x = std::max(x0, inside); x < x1; x++)
    scaleAny(d + (x - x0) * step, step, ra + f.w - 1, 1, f.a.swap, s_wp.a5);
}

// B drifted by sb for [x0, x1): the first column repeats where it is short.
void waterRunB(const Frame& f, const uint16_t* rb, int x0, int x1, uint16_t* d, int step) {
  const int from = std::max(x0, s_wp.sb);
  for (int x = x0; x < std::min(from, x1); x++) copyAny(d + (x - x0) * step, step, rb, 1, f.b.swap, 0);
  if (x1 > from) copyAny(d + (from - x0) * step, step, rb + from - s_wp.sb, x1 - from, f.b.swap, 0);
}

void emitWaterRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const Src& A = f.a;
  const Src& B = f.b;
  const int end = x0 + n;
  if (f.kind == KIND_VSTRIP) {
    const uint16_t* ra = A.px + std::min(ly + s_wp.sa, f.h - 1) * A.stride;
    const uint16_t* rb = B.px + std::max(ly - s_wp.sb, 0) * B.stride;
    // Runs of A, of B, and line pixels one by one; rows clear of the wave
    // are a single run.
    if (ly < s_wp.lo - 2) return scaleAny(d, step, ra + x0, n, A.swap, s_wp.a5);
    if (ly > s_wp.hi + 1) return copyAny(d, step, rb + x0, n, B.swap, 0);
    for (int x = x0; x < end;) {
      const int dist = ly - s_wp.edge[x];
      int e = x + 1;
      if (dist < -2) {
        while (e < end && ly - s_wp.edge[e] < -2) e++;
        scaleAny(d + (x - x0) * step, step, ra + x, e - x, A.swap, s_wp.a5);
      } else if (dist > 1) {
        while (e < end && ly - s_wp.edge[e] > 1) e++;
        copyAny(d + (x - x0) * step, step, rb + x, e - x, B.swap, 0);
      } else {
        d[(x - x0) * step] = waterPx(A, ra + x, B, rb + x, dist);
      }
      x = e;
    }
    return;
  }
  // A up to the line, the line's four pixels, B after it.
  const int edge = s_wp.edge[ly];
  const uint16_t* ra = A.px + ly * A.stride;
  const uint16_t* rb = B.px + ly * B.stride;
  const int l = std::min(std::max(x0, edge - 2), end), r = std::min(std::max(l, edge + 2), end);
  waterRunA(f, ra, x0, l, d, step);
  for (int x = l; x < r; x++)
    d[(x - x0) * step] = waterPx(A, ra + std::min(x + s_wp.sa, f.w - 1), B, rb + std::max(x - s_wp.sb, 0), x - edge);
  waterRunB(f, rb, r, end, d + (r - x0) * step, step);
}

// Depth: A sinks back (shrinks around the centre, dims, black around it)
// while B slides over it from the side or from below. Per-frame values, set
// before chunk 0 (flush task only).
struct DepthPrep {
  int32_t inv;  // A's source step per screen pixel, 16.16
  uint32_t l8;  // A's brightness in eighths, 8 .. 4
  int edge;     // B starts at this x (horizontal strip) or y (vertical)
};
DepthPrep s_dp;

void prepDepth(const Frame& f) {
  const int full = f.kind == KIND_VSTRIP ? f.h : f.w;
  const float e = (float)f.off / (float)full;
  s_dp.inv = (int32_t)(65536.0f / (1 - .1f * e));
  s_dp.l8 = 8 - (uint32_t)lroundf(4 * e);  // half as bright at the end
  s_dp.edge = full - f.off;
}

template <bool SWAP, uint32_t L>
void depthRun(uint16_t* d, int step, const uint16_t* row, int32_t sx, int32_t inv, int n) {
  for (int k = 0; k < n; k++, sx += inv, d += step) {
    const uint16_t c = row[sx >> 16];
    *d = pnl(shade8(SWAP ? c : pnl(c), L));
  }
}

template <bool SWAP>
void depthRunL(uint16_t* d, int step, const uint16_t* row, int32_t sx, int32_t inv, int n, uint32_t l) {
  switch (l) {
    case 8: return depthRun<SWAP, 8>(d, step, row, sx, inv, n);
    case 7: return depthRun<SWAP, 7>(d, step, row, sx, inv, n);
    case 6: return depthRun<SWAP, 6>(d, step, row, sx, inv, n);
    case 5: return depthRun<SWAP, 5>(d, step, row, sx, inv, n);
    default: return depthRun<SWAP, 4>(d, step, row, sx, inv, n);
  }
}

// A for logical x in [x0, x1) of row ly: black where the shrunk page does
// not reach, the columns it covers worked out once per row.
void depthRunA(const Frame& f, int ly, int x0, int x1, uint16_t* d, int step) {
  const int cx = f.w / 2, cy = f.h / 2;
  const int32_t inv = s_dp.inv;
  const int sy = cy + (int)(((int64_t)(ly - cy) * inv) >> 16);
  int lo = x1, hi = x1;
  if (sy >= 0 && sy < f.h) {
    auto src = [&](int x) { return (cx << 16) + (x - cx) * inv; };
    lo = std::max(x0, cx - (int)(((int64_t)cx << 16) / inv));
    while (lo < x1 && src(lo) < 0) lo++;
    hi = std::max(lo, std::min(x1, cx + (int)(((int64_t)(f.w - cx) << 16) / inv) + 1));
    while (hi > lo && src(hi - 1) >= (f.w << 16)) hi--;
  }
  for (int x = x0; x < lo; x++) d[(x - x0) * step] = 0;
  if (hi > lo) {
    const uint16_t* row = f.a.px + sy * f.a.stride;
    uint16_t* o = d + (lo - x0) * step;
    const int32_t sx = (cx << 16) + (lo - cx) * inv;
    if (f.a.swap) depthRunL<true>(o, step, row, sx, inv, hi - lo, s_dp.l8);
    else depthRunL<false>(o, step, row, sx, inv, hi - lo, s_dp.l8);
  }
  for (int x = std::max(lo, hi); x < x1; x++) d[(x - x0) * step] = 0;
}

void emitDepthRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const Src& B = f.b;
  const int end = x0 + n;
  if (f.kind == KIND_VSTRIP) {
    if (ly >= s_dp.edge) copyAny(d, step, B.px + (ly - s_dp.edge) * B.stride + x0, n, B.swap, 0);
    else depthRunA(f, ly, x0, end, d, step);
    return;
  }
  const int l = std::min(std::max(x0, s_dp.edge), end);
  depthRunA(f, ly, x0, l, d, step);
  if (end > l) copyAny(d + (l - x0) * step, step, B.px + ly * B.stride + (l - s_dp.edge), end - l, B.swap, 0);
}

// Cascade: the strip in bands, one per grid column (edges in the middle of
// the gaps), each with its own progress: column k starts 9 % later than
// k - 1. Per-frame values, set before chunk 0 (flush task only).
constexpr int MAX_BANDS = BOARD_GRID_MAX > 8 ? BOARD_GRID_MAX : 8;
struct CascadeRun {
  int16_t x, end;  // screen columns
  int16_t shift;   // source column = x + shift
  int8_t src;      // 0 black, 1 A, 2 B
};
struct CascadeSide {
  int n;
  int edge[MAX_BANDS + 1];
  int off[MAX_BANDS];  // each band's strip offset, like f.off
};
struct CascadePrep {
  CascadeSide a, b;    // the leaving and the arriving page's columns (vertical: a's for both)
  int top;             // rows above this are the header
  // Horizontal strip: the row below the header as runs, the same for every row.
  CascadeRun runs[4 * MAX_BANDS + 2];
  int nRuns;
};
CascadePrep s_cp;

// Bands between the column edges no tile crosses: a tile two columns wide
// moves as one.
void cascadeSide(CascadeSide& c, int cols, uint16_t cuts, int w, int full, float e) {
  cols = std::min(std::max(cols, 1), MAX_BANDS);
  const float cw = (float)(w - 16 - 7 * (cols - 1)) / cols;  // grid: pad 8, gap 7
  int n = 0;
  c.edge[n++] = 0;
  for (int k = 1; k < cols; k++)
    if (!cuts || (cuts >> k & 1)) c.edge[n++] = 8 + (int)lroundf(k * (cw + 7)) - 4;
  c.edge[n] = w;
  c.n = n;
  // The whole stagger stays within 63 % of the slide on wide grids.
  const float step = n > 1 ? std::min(.09f, .63f / (n - 1)) : 0;
  const float spread = step * (n - 1);
  for (int k = 0; k < n; k++) {
    const float q = std::min(1.0f, std::max(0.0f, (e - step * k) / (1 - spread)));
    c.off[k] = (int)lroundf(q * full);
  }
}

void prepCascade(const Frame& f) {
  const bool v = f.kind == KIND_VSTRIP;
  const int full = v ? f.h : f.w;
  const float e = (float)f.off / (float)full;
  cascadeSide(s_cp.a, f.a.cols ? f.a.cols : f.b.cols, f.a.cols ? f.a.cuts : f.b.cuts, f.w, full, e);
  cascadeSide(s_cp.b, f.b.cols ? f.b.cols : f.a.cols, f.b.cols ? f.b.cuts : f.a.cuts, f.w, full, e);
  s_cp.top = f.a.top ? f.a.top : f.b.top;
  if (v) return;
  // Owner of each column: black, A's bands moved left, B's bands over them.
  static int16_t own[MAX_DIM], shift[MAX_DIM];
  for (int x = 0; x < f.w; x++) own[x] = 0;
  for (int pass = 1; pass <= 2; pass++)
    for (int k = 0; k < (pass == 1 ? s_cp.a.n : s_cp.b.n); k++) {
      const CascadeSide& c = pass == 1 ? s_cp.a : s_cp.b;
      const int d = pass == 1 ? -c.off[k] : f.w - c.off[k];
      for (int x = std::max(0, c.edge[k] + d); x < std::min(f.w, c.edge[k + 1] + d); x++) {
        own[x] = (int16_t)(pass * 16 + k);
        shift[x] = (int16_t)-d;
      }
    }
  s_cp.nRuns = 0;
  for (int x = 0; x < f.w;) {
    int e = x + 1;
    while (e < f.w && own[e] == own[x]) e++;
    if (s_cp.nRuns < (int)(sizeof(s_cp.runs) / sizeof(s_cp.runs[0])))
      s_cp.runs[s_cp.nRuns++] = {(int16_t)x, (int16_t)e, shift[x], (int8_t)(own[x] >> 4)};
    x = e;
  }
}

void emitCascadeRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const Src& A = f.a;
  const Src& B = f.b;
  const int end = x0 + n;
  if (f.kind == KIND_VSTRIP) {
    // Each band slides on its own: rows of A, then of B below it.
    const CascadeSide& c = s_cp.a;
    for (int k = 0; k < c.n; k++) {
      const int lo = std::max(x0, c.edge[k]), hi = std::min(end, c.edge[k + 1]);
      if (hi <= lo) continue;
      const int vy = ly + c.off[k];
      const Src& src = vy < f.h ? A : B;
      copyAny(d + (lo - x0) * step, step, src.px + (vy < f.h ? vy : vy - f.h) * src.stride + lo, hi - lo, src.swap, 0);
    }
    return;
  }
  const uint16_t* ra = A.px + ly * A.stride;
  const uint16_t* rb = B.px + ly * B.stride;
  if (ly < s_cp.top) {  // the header moves as one, like a slide
    const int cut = std::min(std::max(x0, f.w - f.off), end);
    if (cut > x0) copyAny(d, step, ra + x0 + f.off, cut - x0, A.swap, 0);
    if (end > cut) copyAny(d + (cut - x0) * step, step, rb + cut - (f.w - f.off), end - cut, B.swap, 0);
    return;
  }
  for (int i = 0; i < s_cp.nRuns; i++) {
    const CascadeRun& r = s_cp.runs[i];
    const int lo = std::max(x0, (int)r.x), hi = std::min(end, (int)r.end);
    if (hi <= lo) continue;
    uint16_t* o = d + (lo - x0) * step;
    if (r.src == 0) for (int x = lo; x < hi; x++, o += step) *o = 0;
    else if (r.src == 1) copyAny(o, step, ra + lo + r.shift, hi - lo, A.swap, 0);
    else copyAny(o, step, rb + lo + r.shift, hi - lo, B.swap, 0);
  }
}

// Emit n pixels of logical row ly starting at logical x0 (ascending). PSRAM
// is always read forward; mirrored rotations walk the destination backwards.
inline void emitRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int dstep) {
  if (!f.comp) {
    copyAny(d, dstep, f.lv + ly * f.w + x0, n, true, 0);
    return;
  }
  if (f.kind == KIND_FRAME) {
    emitFrameRow(f, ly, x0, n, d, dstep);
    return;
  }
  if (f.kind == KIND_MORPH) {
    emitMorphRow(f, ly, x0, n, d, dstep);
    return;
  }
  if (f.kind == KIND_PANEL) {
    if (ly < f.off) {
      const int py = ly + f.a.h - f.off;
      copyAny(d, dstep, f.a.px + py * f.a.stride + x0, n, f.a.swap, 0);
    } else {
      copyAny(d, dstep, f.b.px + ly * f.b.stride + x0, n, f.b.swap, f.dim);
    }
    return;
  }
  if (f.pageFx == PAGE_CASCADE) {
    emitCascadeRow(f, ly, x0, n, d, dstep);
    return;
  }
  if (f.pageFx == PAGE_DEPTH) {
    emitDepthRow(f, ly, x0, n, d, dstep);
    return;
  }
  if (f.pageFx == PAGE_WATER) {
    emitWaterRow(f, ly, x0, n, d, dstep);
    return;
  }
  if (f.pageFx == PAGE_FADE) {  // both pages in place, B's share follows the offset
    const int full = f.kind == KIND_VSTRIP ? f.h : f.w;
    const uint32_t a5 = ((uint32_t)f.off * 32 + full / 2) / full;
    mixAny(d, dstep, f.a.px + ly * f.a.stride + x0, f.a.swap, f.b.px + ly * f.b.stride + x0, f.b.swap, n, a5);
    return;
  }
  if (f.kind == KIND_VSTRIP) {
    const int vy = ly + f.off;  // position in the A-over-B strip
    const Src& src = vy < f.h ? f.a : f.b;
    const int sy = vy < f.h ? vy : vy - f.h;
    copyAny(d, dstep, src.px + sy * src.stride + x0, n, src.swap, 0);
    return;
  }
  while (n > 0) {
    const int vx = x0 + f.off;  // position in the A|B strip
    const Src& src = vx < f.w ? f.a : f.b;
    const int sx = vx < f.w ? vx : vx - f.w;
    int run = f.w - sx;
    if (run > n) run = n;
    copyAny(d, dstep, src.px + ly * src.stride + sx, run, src.swap, 0);
    x0 += run;
    d += dstep * run;
    n -= run;
  }
}

// Logical (lx, ly) to native (nx, ny) for the native rotation.
inline void toNative(uint8_t rot, int lx, int ly, int& nx, int& ny) {
  switch (rot) {
    case 1:  nx = NATIVE_W - 1 - ly; ny = lx;                 break;
    case 2:  nx = NATIVE_W - 1 - lx; ny = NATIVE_H - 1 - ly;  break;
    case 3:  nx = ly;                ny = NATIVE_H - 1 - lx;  break;
    default: nx = lx;                ny = ly;                 break;
  }
}

inline uint16_t blendLE(uint16_t bg, uint16_t fg, uint32_t a) {
  if (a >= 255) return fg;
  const uint32_t ia = 255 - a;
  const uint32_t r = (((bg >> 11) & 0x1F) * ia + ((fg >> 11) & 0x1F) * a) / 255;
  const uint32_t g = (((bg >> 5) & 0x3F) * ia + ((fg >> 5) & 0x3F) * a) / 255;
  const uint32_t b = ((bg & 0x1F) * ia + (fg & 0x1F) * a) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Page dots on a translucent pill, bottom centre (columns) or right edge
// (pages of a column), drawn into the native chunk (panel byte order) after
// it was built.
constexpr int DOTS_SPACING = 14;
constexpr float DOTS_PILL_R = 8.0f;

// The pill's centre, half length and bounding box in logical pixels.
struct DotsBox {
  float cx, cy, half;
  int x0, y0, x1, y1;
};
DotsBox dotsBox(const Frame& f) {
  DotsBox b;
  const bool v = f.dotsV;
  b.cy = v ? (float)f.h * 0.5f : (float)f.h - 14.0f;
  b.cx = v ? (float)f.w - 14.0f : (float)f.w * 0.5f;
  b.half = (float)(f.dotCount - 1) * DOTS_SPACING * 0.5f;
  const float hx = v ? 0.0f : b.half, hy = v ? b.half : 0.0f;
  b.x0 = (int)(b.cx - hx - DOTS_PILL_R) - 1;
  b.x1 = (int)(b.cx + hx + DOTS_PILL_R) + 1;
  b.y0 = (int)(b.cy - hy - DOTS_PILL_R) - 1;
  b.y1 = (int)(b.cy + hy + DOTS_PILL_R) + 1;
  return b;
}

void overlayDots(const Frame& f, uint16_t* dst, int ny0) {
  const int n = f.dotCount;
  if (n < 2 || !f.dotAlpha) return;
  constexpr int SPACING = DOTS_SPACING;
  constexpr float DOT_R = 3.5f;
  constexpr float PILL_R = DOTS_PILL_R;
  const bool v = f.dotsV;
  const DotsBox box = dotsBox(f);
  const float cx = box.cx, cy = box.cy, half = box.half;
  const int bx0 = box.x0, bx1 = box.x1, by0 = box.y0, by1 = box.y1;
  // Quick reject: does the box touch the native rows of this chunk?
  int nxa, nya, nxb, nyb;
  toNative(f.rot, bx0, by0, nxa, nya);
  toNative(f.rot, bx1, by1, nxb, nyb);
  const int lo = nya < nyb ? nya : nyb, hi = nya < nyb ? nyb : nya;
  if (hi < ny0 || lo >= ny0 + PANEL_ROWS) return;
  const float fade = f.dotAlpha / 255.0f;
  for (int ly = by0; ly <= by1; ly++) {
    for (int lx = bx0; lx <= bx1; lx++) {
      int nx, ny;
      toNative(f.rot, lx, ly, nx, ny);
      if (ny < ny0 || ny >= ny0 + PANEL_ROWS || nx < 0 || nx >= NATIVE_W) continue;
      const float px = lx + 0.5f, py = ly + 0.5f;
      // Along and across the pill's axis.
      const float along = v ? py : px, mid = v ? cy : cx;
      const float across = v ? px - cx : py - cy;
      float sa = along - mid;
      if (sa < -half) sa += half; else if (sa > half) sa -= half; else sa = 0;
      const float pill = clamp01(PILL_R + 0.5f - sqrtf(sa * sa + across * across));
      if (pill <= 0) continue;
      uint16_t& out = dst[(ny - ny0) * NATIVE_W + nx];
      uint16_t c = pnl(out);
      c = blendLE(c, 0x0000, (uint32_t)(150 * pill * fade));
      const int i = (int)lroundf((along - (mid - half)) / SPACING);
      if (i >= 0 && i < n) {
        const float da = along - (mid - half + i * SPACING);
        const float dot = clamp01(DOT_R + 0.5f - sqrtf(da * da + across * across));
        if (dot > 0) {
          const uint16_t col = i == f.dotActive ? 0xFFFF : 0x8C71;
          c = blendLE(c, col, (uint32_t)(255 * dot * fade));
        }
      }
      out = pnl(c);
    }
  }
}

// Fill one native chunk (rows ny0 .. ny0+PANEL_ROWS-1). Mapping per native
// rotation (W, H = NATIVE_W, NATIVE_H):
//   0: lx = nx,          ly = ny
//   1: lx = ny,          ly = W - 1 - nx
//   2: lx = W - 1 - nx,  ly = H - 1 - ny
//   3: lx = H - 1 - ny,  ly = nx
// Part [lo, hi) of a chunk: logical rows when rotated by 90, chunk rows otherwise.
void buildPart(const Frame& f, uint16_t* dst, int ny0, int lo, int hi) {
  if (!PANEL_BIG_ENDIAN && !f.comp && f.rot == 0) {
    // Plain LVGL frame already in panel order and orientation: row copies.
    for (int k = lo; k < hi; k++)
      memcpy(dst + k * NATIVE_W, f.lv + (ny0 + k) * NATIVE_W, NATIVE_W * 2);
    return;
  }
  if (!f.comp && (f.rot & 1)) {
    // Plain LVGL frame turned by 90 (landscape on the S3): the hot path, free
    // of the per-run bookkeeping emitRow() needs for composed frames.
    const uint16_t* lv = f.lv;
    if (f.rot == 1) {
      for (int ly = lo; ly < hi; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + ny0;
        uint16_t* d = dst + (NATIVE_W - 1 - ly);
        for (int k = 0; k < PANEL_ROWS; k++) d[k * NATIVE_W] = pnl(s[k]);
      }
    } else {
      for (int ly = lo; ly < hi; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + (NATIVE_H - PANEL_ROWS - ny0);
        uint16_t* d = dst + (PANEL_ROWS - 1) * NATIVE_W + ly;
        for (int k = 0; k < PANEL_ROWS; k++) d[-k * NATIVE_W] = pnl(s[k]);
      }
    }
  } else {
    switch (f.rot) {
      case 1:
        for (int ly = lo; ly < hi; ly++)
          emitRow(f, ly, ny0, PANEL_ROWS, dst + (NATIVE_W - 1 - ly), NATIVE_W);
        break;
      case 3:
        for (int ly = lo; ly < hi; ly++)
          emitRow(f, ly, NATIVE_H - PANEL_ROWS - ny0, PANEL_ROWS,
                  dst + (PANEL_ROWS - 1) * NATIVE_W + ly, -NATIVE_W);
        break;
      case 2:
        for (int k = lo; k < hi; k++)
          emitRow(f, NATIVE_H - 1 - (ny0 + k), 0, NATIVE_W,
                  dst + k * NATIVE_W + (NATIVE_W - 1), -1);
        break;
      default:
        for (int k = lo; k < hi; k++)
          emitRow(f, ny0 + k, 0, NATIVE_W, dst + k * NATIVE_W, 1);
        break;
    }
  }
}

// A helper on core 0 takes batches of the same job (panelShare). The caller
// never waits for a helper that has not started (WiFi can hold core 0).
struct SharedJob {
  void (*fn)(void*, int, int);
  void* ctx;
  int n, batch;
};
SharedJob s_job;
// 0 idle, 4 being posted, 1 posted, 2 helper in, 3 helper out
std::atomic<int> s_jobNext{0}, s_jobState{0};
TaskHandle_t s_helper = nullptr;

void runBatches(const SharedJob& j) {
  for (int lo; (lo = s_jobNext.fetch_add(j.batch)) < j.n;) j.fn(j.ctx, lo, std::min(lo + j.batch, j.n));
}

void helperTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int posted = 1;
    if (!s_jobState.compare_exchange_strong(posted, 2)) continue;  // stale wake-up
    runBatches(s_job);
    s_jobState.store(3);
  }
}

struct ChunkJob {
  const Frame* f;
  uint16_t* dst;
  int ny0;
};

void buildBatch(void* ctx, int lo, int hi) {
  const ChunkJob& c = *(const ChunkJob*)ctx;
  buildPart(*c.f, c.dst, c.ny0, lo, hi);
}

void buildChunk(const Frame& f, uint16_t* dst, int ny0) {
  if (f.comp && f.kind == KIND_FRAME && ny0 == 0) prepFrame(f);
  if (f.comp && (f.kind == KIND_HSTRIP || f.kind == KIND_VSTRIP) && ny0 == 0) {
    if (f.pageFx == PAGE_WATER) prepWater(f);
    if (f.pageFx == PAGE_DEPTH) prepDepth(f);
    if (f.pageFx == PAGE_CASCADE) prepCascade(f);
  }
  const int n = (f.rot & 1) ? NATIVE_W : PANEL_ROWS;
  ChunkJob c = {&f, dst, ny0};
  if (f.comp) panelShare(buildBatch, &c, n, 16);
  else buildPart(f, dst, ny0, 0, n);
  overlayDots(f, dst, ny0);
}

inline float easeOutCubic(float p) {
  const float q = 1.0f - p;
  return 1.0f - q * q * q;
}

inline uint8_t dotsAlphaAt(int64_t now) {
  if (s.dotsForce) return 255;
  if (now >= s.dotsUntil) return 0;
  const int64_t left = s.dotsUntil - now;
  if (left >= DOTS_FADE_US) return 255;
  return (uint8_t)(255 * left / DOTS_FADE_US);
}

// Pick the next frame. Returns false when there is nothing new to show.
bool decide(Frame& f) {
  bool have = false;
  bool give = false;
  const int64_t te = transportLastSyncUs();
  const int64_t now = esp_timer_get_time();
  const int64_t shown = transportPresentUs(now);
  const int64_t frameTime = shown ? shown
                          : te && now - te <= 2 * PANEL_SYNC_PERIOD_US ? te + PANEL_SYNC_PERIOD_US + PANEL_SYNC_LEAD_US
                                                                      : now + PANEL_ANIM_LEAD_US;
  portENTER_CRITICAL(&s_mux);
  const uint8_t rot = s_rot;
  int w, h;
  logicalSize(rot, w, h);
  f.rot = rot;
  f.w = w;
  f.h = h;
  f.releaseLv = false;
  f.build0Us = 0;
  f.frameLast = false;
  f.dotsV = s.dotsV;
  f.dotCount = (uint8_t)(s.dotsV ? s.vCount : s.pageCount);
  f.dotActive = (uint8_t)(s.dotsV ? s.vActive : s.pageActive);
  if (s.lvPending) {
    s.lvPending = false;
    if (s.mode == COMP_HOLD && s.lvGen >= s.holdGen) s.mode = COMP_NONE;
    s.lvLatest = s.lvIdx;
    if (s.mode == COMP_NONE) {
      f.comp = false;
      f.lv = s_fb[s.lvIdx];
      f.releaseLv = true;
      have = true;
    } else {
      // Superseded by the compositor. The buffer still holds a complete frame
      // (direct mode), so it stays usable as the "current page" image.
      give = true;
      st_lvDropped = st_lvDropped + 1;
    }
  }
  if (!have && !s_quiescing && s.mode == COMP_FRAME) {
    const int64_t elapsed = frameTime - s.t0;
    f.comp = true;
    f.kind = KIND_FRAME;
    f.scene = s.scene;
    f.durationMs = (s.durUs + 999) / 1000;
    f.elapsedMs = elapsed > 0 ? elapsed / 1000 : 0;
    f.frameLast = elapsed >= s.durUs;
    if (f.frameLast) s.mode = COMP_FRAME_HOLD;
    have = true;
  }
  if (!have && s.kind == KIND_MORPH && s.mode == COMP_HOLD && !s.lvPending && s.lvGen >= s.holdGen) {
    // The frame LVGL drew while the morph ran already shows the end state.
    s.mode = COMP_NONE;
    f.comp = false;
    f.lv = s_fb[s.lvLatest];
    have = true;
  }
  if (!have && s.mode == COMP_ANIM && s.kind == KIND_MORPH) {
    const int64_t t = frameTime - s.t0;
    // Begun early: the scrim fades in from the tap, the card's own animation
    // runs from the moment its picture arrives (tc).
    const bool early = s.morphEarly, waiting = !s.morph.card.px;
    const int64_t tc = !early ? t : waiting ? 0 : frameTime - s.morphCardUs;
    const bool last = !waiting && tc >= (int64_t)s.durUs;
    const float u = last ? 1.0f : tc <= 0 ? 0.0f : (float)tc / (float)s.durUs;
    const float p = last ? 1.0f : easeOutCubic(u);
    // Waiting with the scrim done: no frames, the loop needs both cores.
    const bool still = waiting && t >= SCRIM_IN_US + PANEL_SYNC_PERIOD_US;
    const bool skip = still && s.morphStill;
    s.morphStill = still;
    if (last) {
      s.mode = COMP_HOLD;
      s.holdGen = s.morphHoldGen;
    }
    f.comp = true;
    f.kind = KIND_MORPH;
    f.morph = s.morph;
    if (!f.morph.base.px) f.morph.base = {s_fb[s.lvLatest], (uint16_t)w, true, (uint16_t)h};
    const FrameRect& a = f.morph.from;
    const FrameRect& b = f.morph.to;
    auto mix = [p](int x, int y) { return (int16_t)(x + (int)lroundf((y - x) * p)); };
    f.wx = mix(a.x, b.x);
    f.wy = mix(a.y, b.y);
    f.ww = std::max<int16_t>(1, mix(a.w, b.w));
    f.wh = std::max<int16_t>(1, mix(a.h, b.h));
    f.wr = std::min<int16_t>(mix(f.morph.r0, f.morph.r1), std::min(f.ww, f.wh) / 2);
    f.wdx = ((uint32_t)f.morph.cw << 16) / f.ww;
    f.wdy = ((uint32_t)f.morph.ch << 16) / f.wh;
    const float in = f.morph.open ? clamp01((p - 0.35f) / 0.65f) : 1 - clamp01(p / 0.6f);
    const float shown = f.morph.open ? 1 : 1 - clamp01((p - 0.6f) / 0.4f);
    f.wa5 = (uint32_t)lroundf(in * 32);
    f.wo5 = (uint32_t)lroundf(shown * 32);
    // The scrim comes in with the card and goes with it, while the card is
    // small: the steps in between shade every base pixel.
    const float scrim = f.morph.dim != 2 ? 0
                      : early        ? clamp01((float)t / (float)SCRIM_IN_US)
                      : f.morph.open ? clamp01(p / .5f) : 1 - clamp01((p - .5f) / .5f);
    f.base8 = 8 - (uint32_t)lroundf(4 * scrim);
    if (f.morph.reveal) {
      // Out to the card's farthest corner; closing starts slowly (ease in).
      const float dx = (float)std::max(std::abs(f.morph.cx - b.x), std::abs(b.x + b.w - f.morph.cx));
      const float dy = (float)std::max(std::abs(f.morph.cy - b.y), std::abs(b.y + b.h - f.morph.cy));
      const float q = f.morph.open ? p : 1 - u * u * u;
      f.wr = (int16_t)lroundf((sqrtf(dx * dx + dy * dy) + 1) * q);
    }
    if (f.morph.sheet) {
      // Up from below the screen with a small overshoot; down again with an ease in.
      const float y0 = (float)(h - b.y);
      const float y = !f.morph.open ? y0 * u * u
                    : u < .72f     ? y0 + (-7 - y0) * easeOutCubic(u / .72f)
                                   : -7 + 7 * ((u - .72f) / .28f) * ((u - .72f) / .28f) * (3 - 2 * (u - .72f) / .28f);
      f.wy = (int16_t)(b.y + lroundf(y));
    }
    have = !skip;
  }
  if (!have && s.kind != KIND_MORPH && (s.mode == COMP_DRAG || s.mode == COMP_ANIM)) {
    int off = s.off;
    if (s.mode == COMP_ANIM) {
      const int64_t t = frameTime - s.t0;
      if (t >= (int64_t)s.durUs) {
        off = s.toOff;
        s.mode = s.landing ? COMP_HOLD : COMP_NONE;
        s.holdGen = UINT32_MAX;
        s.landed = s.landing;
        s.landedPending = true;
        if (s.kind != KIND_PANEL) {
          s.dotsForce = false;
          s.dotsUntil = now + DOTS_HOLD_US + DOTS_FADE_US;
          int& act = s.kind == KIND_VSTRIP ? s.vActive : s.pageActive;
          if (s.landing) act = s.pageA + (s.landing > 0 ? 1 : 0);
        }
      } else {
        const float p = t <= 0 ? 0.0f : (float)t / (float)s.durUs;
        off = s.fromOff + (int)((s.toOff - s.fromOff) * easeOutCubic(p));
      }
      s.off = off;
    }
    if ((!s.sentAny || off != s.lastSentOff) && s.lvLatest >= 0) {
      f.comp = true;
      f.kind = s.kind;
      f.a = s.a;
      f.b = s.b;
      f.pageFx = s.kind != KIND_PANEL ? s.pageFx : PAGE_SLIDE;
      const uint16_t* cur = s_fb[s.lvLatest];
      if (!f.a.px) f.a = {cur, (uint16_t)w, true, (uint16_t)h, f.a.cols, f.a.top, f.a.cuts};
      if (!f.b.px) f.b = {cur, (uint16_t)w, true, (uint16_t)h, f.b.cols, f.b.top, f.b.cuts};
      if (s.kind == KIND_HSTRIP) {
        f.off = off < 0 ? 0 : (off > w ? w : off);
        f.dotActive = (uint8_t)(s.pageA + (f.off * 2 >= w ? 1 : 0));
        f.dim = 0;
      } else if (s.kind == KIND_VSTRIP) {
        f.off = off < 0 ? 0 : (off > h ? h : off);
        f.dotActive = (uint8_t)(s.pageA + (f.off * 2 >= h ? 1 : 0));
        f.dim = 0;
      } else {
        const int ph = f.a.h;
        f.off = off < 0 ? 0 : (off > ph ? ph : off);
        f.dim = f.off * 3 >= ph * 2 ? 2 : (f.off * 3 >= ph ? 1 : 0);
      }
      s.lastSentOff = off;
      s.sentAny = true;
      have = true;
    }
  }
  const bool panelOwns = (s.kind == KIND_PANEL || s.kind == KIND_FRAME) && s.mode != COMP_NONE;
  uint8_t dots = panelOwns ? 0 : dotsAlphaAt(now);
  if (have && f.comp && f.kind == KIND_FRAME && !f.scene.waking)
    dots = f.elapsedMs >= 300 || f.frameLast ? 0 : s.frameDots * (300 - f.elapsedMs) / 300;
  if (!have && s.mode == COMP_NONE && s.lvLatest >= 0 && dots != s.lastSentDots) {
    // Dots fading over a static LVGL screen: resend the latest complete frame.
    // Safe: LVGL only ever renders into the other buffer.
    f.comp = false;
    f.lv = s_fb[s.lvLatest];
    have = true;
  }
  if (have) {
    f.dotAlpha = dots;
    if (!f.comp) {
      f.dirty = s.lvDirty;
      s.lvDirty.clear();
      if (rot != 0) f.dirty.full = true;
      // The dots are drawn over the copy, and must go from it afterwards.
      if (f.dotCount >= 2 && (dots || s.lastSentDots)) {
        const DotsBox b = dotsBox(f);
        f.dirty.addRect(b.x0, b.y0, b.x1, b.y1);
      }
    }
    s.lastSentDots = dots;
  }
  portEXIT_CRITICAL(&s_mux);
  if (give) xSemaphoreGive(s_lvReady);
  return have;
}

void publishFrameStats() {
  const uint32_t n = s_frameStats.frames;
  s_frameStats.frameUsAvg = n ? s_frameSum / n : 0;
  s_frameStats.buildUsAvg = n ? s_frameBuildSum / n : 0;
  s_frameStats.flushUsAvg = n ? s_frameFlushSum / n : 0;
  portENTER_CRITICAL(&s_mux);
  s.frameStats = s_frameStats;
  s.frameDone = true;
  s.frameResult = true;
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace

// --- Public API -------------------------------------------------------------------
// LVGL and the flush task both post here (same core): a busy helper means alone.
void panelShare(void (*fn)(void* ctx, int lo, int hi), void* ctx, int n, int batch) {
  int idle = 0;
  if (!s_helper || n <= batch || !s_jobState.compare_exchange_strong(idle, 4)) {
    fn(ctx, 0, n);
    return;
  }
  s_job = {fn, ctx, n, batch};
  s_jobNext.store(0);
  s_jobState.store(1);
  xTaskNotifyGive(s_helper);
  runBatches(s_job);
  int posted = 1;
  if (!s_jobState.compare_exchange_strong(posted, 0)) {
    while (s_jobState.load() != 3) {
    }
    s_jobState.store(0);
  }
}

uint32_t panelFrameClockMs() {
  const int64_t te = transportLastSyncUs();
  const int64_t now = esp_timer_get_time();
  const uint32_t ms = (uint32_t)((te && now - te <= SYNC_STALE_US ? te + PANEL_SYNC_PERIOD_US : now) / 1000);
  // Clamp a backward prediction when TE resumes, including across millis wrap.
  static uint32_t lastMs = ms;
  if ((int32_t)(ms - lastMs) > 0) lastMs = ms;
  return lastMs;
}

bool panelBegin(uint8_t rotation) {
  s_rot = (rotation + BOARD_ROT_BASE) & 3;
  for (int i = 0; i < 2; i++) {
    s_fb[i] = (uint16_t*)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb[i]) {
      Serial.println("Deck panel: framebuffer allocation FAILED");
      return false;
    }
    memset(s_fb[i], 0, FRAME_BYTES);
  }
  if (!transportBegin()) return false;

  s_lvReady = xSemaphoreCreateCounting(2, 0);  // up to two LVGL frames in flight
  s_quiet = xSemaphoreCreateBinary();
  // Core 1 at high priority: on core 0 the flush loses to WiFi/lwIP under
  // network load (measured on the S3: ~30 fps slides instead of 60).
  xTaskCreatePinnedToCore(helperTask, "deck_build", 3072, nullptr, 15, &s_helper, 0);
  xTaskCreatePinnedToCore(transportTask, "deck_flush", 4096, nullptr, 20, &s_task, 1);
  Serial.printf("Deck panel: up, rotation %u, internal free %u, largest %u, PSRAM free %u\n",
                (unsigned)rotation,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

void panelSetRotation(uint8_t rotation) {
  compCancel();
  s_rot = (rotation + BOARD_ROT_BASE) & 3;
  portENTER_CRITICAL(&s_mux);
  s.lvDirty.full = true;
  portEXIT_CRITICAL(&s_mux);
}

uint8_t panelRotation() { return (s_rot + 4 - BOARD_ROT_BASE) & 3; }
uint8_t panelNativeRotation() { return s_rot; }

int logicalW() { return (s_rot & 1) ? NATIVE_H : NATIVE_W; }
int logicalH() { return (s_rot & 1) ? NATIVE_W : NATIVE_H; }


uint16_t* panelLvBuffer(int i) { return s_fb[i & 1]; }

const uint16_t* panelLatestLvFrame() {
  portENTER_CRITICAL(&s_mux);
  const int idx = s.lvLatest;
  portEXIT_CRITICAL(&s_mux);
  return idx >= 0 ? s_fb[idx] : nullptr;
}

void panelLvDirty(int x0, int y0, int x1, int y1) {
  portENTER_CRITICAL(&s_mux);
  s.lvDirty.addRect(x0, y0, x1, y1);
  portEXIT_CRITICAL(&s_mux);
}

void panelPostLv(int idx, uint32_t gen) {
  portENTER_CRITICAL(&s_mux);
  const bool superseded = s.lvPending;  // never read: its buffer is free again
  s.lvPending = true;
  s.lvIdx = idx & 1;
  s.lvGen = gen;
  portEXIT_CRITICAL(&s_mux);
  if (superseded) xSemaphoreGive(s_lvReady);
  if (s_task) xTaskNotifyGive(s_task);
}

bool panelWaitLvReady(uint32_t timeoutMs) {
  return xSemaphoreTake(s_lvReady, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool compBeginDrag(const Src& a, const Src& b, int off, int pageA, bool vertical) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0) {
    s.mode = COMP_DRAG;
    s.kind = vertical ? KIND_VSTRIP : KIND_HSTRIP;
    s.pageFx = s_pageFx;
    s.dotsV = vertical;
    s.a = a;
    s.b = b;
    s.pageA = pageA;
    s.off = off;
    s.sentAny = false;
    s.landedPending = false;
    s.dotsForce = true;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void panelSetPageFx(PageFx fx) { s_pageFx = fx; }

bool compBeginMorph(const MorphScene& m, uint32_t durMs, uint32_t holdGen) {
  if ((!m.card.px && !m.open) || m.cw <= 0 || m.ch <= 0) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0) {
    s.mode = COMP_ANIM;
    s.kind = KIND_MORPH;
    s.morph = m;
    // Pinned now: LVGL may draw one more frame into the other buffer.
    int w, h;
    logicalSize(s_rot, w, h);
    // The newest complete frame, also one posted but not yet taken.
    const int latest = s.lvPending ? s.lvIdx : s.lvLatest;
    if (!s.morph.base.px) s.morph.base = {s_fb[latest], (uint16_t)w, true, (uint16_t)h};
    s.morphHoldGen = holdGen;
    s.morphEarly = !m.card.px;
    s.morphStill = false;
    s.morphCardUs = 0;
    s.t0 = esp_timer_get_time();
    s.durUs = durMs ? durMs * 1000u : 1u;
    s.landing = 0;
    s.landedPending = false;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  if (ok && s_task) xTaskNotifyGive(s_task);
  return ok;
}

bool compMorphWaiting() {
  portENTER_CRITICAL(&s_mux);
  const bool w = s.kind == KIND_MORPH && s.mode == COMP_ANIM && !s.morph.card.px;
  portEXIT_CRITICAL(&s_mux);
  return w;
}

void compMorphCard(const Src& card) {
  portENTER_CRITICAL(&s_mux);
  if (s.kind == KIND_MORPH && s.mode == COMP_ANIM && !s.morph.card.px) {
    s.morph.card = card;
    s.morphCardUs = esp_timer_get_time();
  }
  portEXIT_CRITICAL(&s_mux);
}

bool compBeginPanel(const Src& panel, const Src& page, int off) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0 && panel.px && panel.h) {
    s.mode = COMP_DRAG;
    s.kind = KIND_PANEL;
    s.a = panel;
    s.b = page;
    s.off = off;
    s.sentAny = false;
    s.landedPending = false;
    s.dotsForce = false;
    s.dotsUntil = 0;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void panelSetPager(int count, int active, int vCount, int vActive) {
  portENTER_CRITICAL(&s_mux);
  s.pageCount = count;
  s.pageActive = active;
  s.vCount = vCount;
  s.vActive = vActive;
  portEXIT_CRITICAL(&s_mux);
}

void compDrag(int off) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_DRAG) s.off = off;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

void compRelease(int toOff, uint32_t durMs, int8_t landing) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_DRAG) {
    s.mode = COMP_ANIM;
    s.fromOff = s.off;
    s.toOff = toOff;
    s.t0 = esp_timer_get_time();
    s.durUs = durMs ? durMs * 1000u : 1u;
    s.landing = landing;
  }
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

CompMode compMode() {
  portENTER_CRITICAL(&s_mux);
  CompMode m = s.mode;
  portEXIT_CRITICAL(&s_mux);
  return m;
}

bool compOwnsScreen() {
  CompMode m = compMode();
  return m == COMP_DRAG || m == COMP_ANIM || m == COMP_FRAME || m == COMP_FRAME_HOLD;
}

bool compTakeLanding(int8_t* landing) {
  bool got = false;
  portENTER_CRITICAL(&s_mux);
  if (s.landedPending) {
    s.landedPending = false;
    *landing = s.landed;
    got = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return got;
}

void compSetHoldGen(uint32_t gen) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_HOLD) s.holdGen = gen;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

void compCancel() {
  portENTER_CRITICAL(&s_mux);
  s.mode = COMP_NONE;
  s.landedPending = false;
  s.dotsForce = false;
  portEXIT_CRITICAL(&s_mux);
}

void panelQuiesce() {
  compCancel();
  if (!s_task) return;
  portENTER_CRITICAL(&s_mux);
  s_quiescing = true;
  portEXIT_CRITICAL(&s_mux);
  xTaskNotifyGive(s_task);
  xSemaphoreTake(s_quiet, portMAX_DELAY);
}

bool compBeginFrame(const FrameScene& scene, uint32_t durationMs) {
  if (!scene.base.px || !scene.picture.px || (scene.count && (!scene.page.px || !scene.rects))) return false;
  panelQuiesce();
  s_frameStats = {};
  s_frameSum = s_frameBuildSum = s_frameFlushSum = 0;
  s_frameStart = 0;
  portENTER_CRITICAL(&s_mux);
  s.scene = scene;
  s.frameDots = dotsAlphaAt(esp_timer_get_time());
  s.kind = KIND_FRAME;
  s.mode = COMP_FRAME;
  s.frameDone = s.frameResult = false;
  s.t0 = esp_timer_get_time();
  s.durUs = durationMs ? durationMs * 1000 : 1;
  s.dotsForce = false;
  s.dotsUntil = 0;
  portEXIT_CRITICAL(&s_mux);
  xTaskNotifyGive(s_task);
  return true;
}

bool compFrameResult(PanelStats& out) {
  portENTER_CRITICAL(&s_mux);
  const bool done = s.frameDone && s.frameResult;
  if (done) { out = s.frameStats; s.frameResult = false; }
  portEXIT_CRITICAL(&s_mux);
  return done;
}

PanelStats panelTakeStats() {
  PanelStats p;
  const uint32_t n = st_frames;
  p.frames = n;
  p.compFrames = st_comp;
  p.flushUsAvg = n ? st_flushSum / n : 0;
  p.flushUsMax = st_flushMax;
  p.buildUsAvg = n ? st_buildSum / n : 0;
  p.frameUsAvg = n ? st_frameSum / n : 0;
  p.frameUsMax = st_frameMax;
  p.teLate = st_teLate;
  p.teTimeouts = st_teTimeout;
  p.lvDropped = st_lvDropped;
  p.gapMaxUs = st_gapMax;
  st_frames = 0;
  st_comp = 0;
  st_flushSum = 0;
  st_flushMax = 0;
  st_buildSum = 0;
  st_frameSum = 0;
  st_frameMax = 0;
  st_teLate = 0;
  st_teTimeout = 0;
  st_lvDropped = 0;
  st_gapMax = 0;
  return p;
}

// --- Transport contract (deck_panel_int.h) ------------------------------------------
bool frameDecide(Frame& f) { return decide(f); }
void frameBuild(const Frame& f, uint16_t* dst, int ny0) { buildChunk(f, dst, ny0); }

bool frameBuildDirty(const Frame& f, uint16_t* dst, const DirtyMask& m) {
  if (PANEL_BIG_ENDIAN || f.comp || f.rot != 0 || m.full || PANEL_ROWS != NATIVE_H) return false;
  for (int r = 0; r < DIRTY_ROWS; r++) {
    uint32_t bits = m.rows[r];
    const int y0 = r * DIRTY_CH, y1 = std::min(y0 + DIRTY_CH, NATIVE_H);
    while (bits) {
      const int c0 = __builtin_ctz(bits);
      int c1 = c0;  // end of this run of cells
      while (c1 < 32 && (bits >> c1 & 1)) c1++;
      bits &= c1 >= 32 ? 0 : ~0u << c1;
      const int x0 = c0 * DIRTY_CW, x1 = std::min(c1 * DIRTY_CW, NATIVE_W);
      if (x0 >= x1) continue;
      for (int y = y0; y < y1; y++)
        memcpy(dst + y * NATIVE_W + x0, f.lv + y * NATIVE_W + x0, (size_t)(x1 - x0) * 2);
    }
  }
  overlayDots(f, dst, 0);
  return true;
}

void frameIdleWait() {
  const CompMode mode = compMode();
  const bool animating = mode == COMP_ANIM || mode == COMP_FRAME;
  ulTaskNotifyTake(pdTRUE, animating ? 1 : pdMS_TO_TICKS(50));
}

void frameReleaseLv() { xSemaphoreGive(s_lvReady); }

void frameStarted(const Frame& f, int64_t startUs, bool backToBack) {
  if (backToBack && f.comp && s_lastWasComp) {
    const uint32_t gap = (uint32_t)(startUs - s_lastStartUs);
    if (gap > st_gapMax) st_gapMax = gap;
  }
  s_lastStartUs = startUs;
  s_lastWasComp = f.comp;
}

void frameFinished(const Frame& f, int64_t startUs, uint32_t buildUs, uint32_t flushUs, uint32_t costUs) {
  st_frames = st_frames + 1;
  if (f.comp) st_comp = st_comp + 1;
  st_flushSum = st_flushSum + flushUs;
  if (flushUs > st_flushMax) st_flushMax = flushUs;
  st_buildSum = st_buildSum + buildUs;
  st_frameSum = st_frameSum + costUs;
  if (costUs > st_frameMax) st_frameMax = costUs;
  if (f.comp && f.kind == KIND_FRAME) {
    s_frameStats.frames++;
    s_frameStats.compFrames++;
    s_frameSum += costUs;
    s_frameBuildSum += buildUs;
    s_frameFlushSum += flushUs;
    if (costUs > s_frameStats.frameUsMax) s_frameStats.frameUsMax = costUs;
    if (flushUs > s_frameStats.flushUsMax) s_frameStats.flushUsMax = flushUs;
    if (s_frameStart && startUs - s_frameStart > s_frameStats.gapMaxUs)
      s_frameStats.gapMaxUs = startUs - s_frameStart;
    s_frameStart = startUs;
    if (f.frameLast) publishFrameStats();
  }
}

void countLateSync() { st_teLate = st_teLate + 1; }
void countMissedSync() { st_teTimeout = st_teTimeout + 1; }

bool quiesceRequested() {
  portENTER_CRITICAL(&s_mux);
  const bool quiet = s_quiescing;
  const bool partial = quiet && s.kind == KIND_FRAME && !s.frameDone;
  s_quiescing = false;
  portEXIT_CRITICAL(&s_mux);
  if (partial) publishFrameStats();
  return quiet;
}

void quiesceAck() { xSemaphoreGive(s_quiet); }

}  // namespace deck
