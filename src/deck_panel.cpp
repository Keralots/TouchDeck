// Guition JC3248W535 display pipeline. See deck_panel.h for the contract and
// the measured panel facts behind it.
#include "deck_panel.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
// Only for axs15231b_180640_init_operations and its op-code enum; the
// library's databus and panel classes are not used.
#include <Arduino_GFX_Library.h>

namespace deck {
namespace {

constexpr int PIN_CS = 45;
constexpr int PIN_SCK = 47;
constexpr int PIN_D0 = 21;
constexpr int PIN_D1 = 48;
constexpr int PIN_D2 = 40;
constexpr int PIN_D3 = 39;
constexpr int PIN_TE = 38;

// 48 native rows per chunk: 30,720 bytes (the S3 limit for one SPI
// transaction is 32 KB), 10 chunks per frame. Smaller chunks save internal
// RAM but every transaction adds overhead, and with 20 chunks a frame grew
// to ~16 ms, too close to the 16.39 ms TE period. Two staging buffers only:
// the next frame's first chunk is pre-built into whichever one frees first.
constexpr int ROWS = 48;
constexpr size_t CHUNK_BYTES = (size_t)NATIVE_W * ROWS * 2;
constexpr int NCHUNK = NATIVE_H / ROWS;
constexpr uint32_t SPI_HZ = 40000000;  // 80 MHz corrupts the image on this panel
// The write (15.4 ms) is slower than the scan, so a frame that starts later
// than this after the TE rising edge is overtaken near the bottom.
constexpr int64_t TE_LATE_US = 900;
// Animation frames are sampled this far ahead of the build so the offset
// matches the moment the frame is actually on the glass.
constexpr int64_t ANIM_LEAD_US = 8000;

spi_device_handle_t s_dev = nullptr;
uint16_t* s_fb[2] = {nullptr, nullptr};
uint16_t* s_bounce[2] = {nullptr, nullptr};
DMA_ATTR uint8_t s_cmdbuf[64];
spi_transaction_ext_t s_tr[NCHUNK];
TaskHandle_t s_task = nullptr;
volatile uint8_t s_rot = 1;

// --- TE ----------------------------------------------------------------------
portMUX_TYPE s_teMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_teSeq = 0;
volatile int64_t s_teUs = 0;
SemaphoreHandle_t s_teSem = nullptr;
uint32_t s_teUsed = 0;

void IRAM_ATTR teIsr(void*) {
  portENTER_CRITICAL_ISR(&s_teMux);
  s_teUs = esp_timer_get_time();
  s_teSeq = s_teSeq + 1;
  portEXIT_CRITICAL_ISR(&s_teMux);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_teSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// --- Shared frame / compositor state (s_mux) ----------------------------------
SemaphoreHandle_t s_lvReady = nullptr;
SemaphoreHandle_t s_quiet = nullptr;
bool s_quiescing = false;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

enum Kind : uint8_t { KIND_HSTRIP = 0, KIND_VSTRIP, KIND_PANEL, KIND_FRAME, KIND_MORPH };

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

// A frame to send, resolved under the lock so the build never reads shared
// state: one LVGL framebuffer, a horizontal or vertical A|B strip, or a panel
// pulled down over a (dimmed) page. Plus the page-dot overlay.
struct Frame {
  FrameScene scene;
  uint32_t elapsedMs, durationMs, build0Us;
  bool frameLast;
  bool comp;
  bool releaseLv;
  uint8_t rot;
  int w, h;
  const uint16_t* lv;
  Kind kind;
  Src a, b;
  int off;
  uint8_t dim;
  PageFx pageFx;
  // Morph window this frame, the card picture's step per window pixel (16.16),
  // picture share over the ghost and window share over the base (both 0..32).
  // Reveal: the circle's radius in wr.
  MorphScene morph;
  int16_t wx, wy, ww, wh, wr;
  uint32_t wdx, wdy, wa5, wo5;
  uint32_t base8;  // the base's brightness this frame in eighths, 4 (the scrim) .. 8
  uint8_t dotCount, dotActive, dotAlpha;
  bool dotsV;
};

// --- Stats (approximate, single writer) ----------------------------------------
volatile uint32_t st_frames = 0, st_comp = 0, st_flushSum = 0, st_flushMax = 0;
volatile uint32_t st_buildSum = 0, st_teLate = 0, st_teTimeout = 0, st_lvDropped = 0;
volatile uint32_t st_gapMax = 0, st_frameSum = 0, st_frameMax = 0;
PanelStats s_frameStats = {};
uint64_t s_frameSum = 0, s_frameBuildSum = 0, s_frameFlushSum = 0;
int64_t s_frameStart = 0;
int64_t s_lastStartUs = 0;
bool s_lastWasComp = false;

inline void csLow() { gpio_set_level((gpio_num_t)PIN_CS, 0); }
inline void csHigh() { gpio_set_level((gpio_num_t)PIN_CS, 1); }

// Command with optional parameters on one data line, polling. Only the flush
// task (or panelBegin before it exists) may call this: polling transfers are
// not allowed while queued transactions are pending.
void txCmd(uint8_t c, const uint8_t* d, size_t n) {
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.cmd = 0x02;
  t.addr = ((uint32_t)c) << 8;
  if (n > 0 && n <= 4) {
    t.flags |= SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, d, n);
  } else if (n > 0) {
    memcpy(s_cmdbuf, d, n);
    t.tx_buffer = s_cmdbuf;
  }
  t.length = n * 8;
  csLow();
  spi_device_polling_transmit(s_dev, &t);
  csHigh();
}

void panelInitSequence() {
  txCmd(0x01, nullptr, 0);  // SWRESET
  delay(200);
  const uint8_t* op = axs15231b_180640_init_operations;
  const size_t len = sizeof(axs15231b_180640_init_operations);
  for (size_t i = 0; i < len; ++i) {
    switch (op[i]) {
      case BEGIN_WRITE:
      case END_WRITE:
        break;
      case WRITE_COMMAND_8:
        txCmd(op[++i], nullptr, 0);
        break;
      case WRITE_C8_D8: {
        uint8_t c = op[++i];
        txCmd(c, &op[i + 1], 1);
        i += 1;
        break;
      }
      case WRITE_C8_D16: {
        uint8_t c = op[++i];
        txCmd(c, &op[i + 1], 2);
        i += 2;
        break;
      }
      case WRITE_C8_BYTES: {
        uint8_t c = op[++i];
        uint8_t l = op[++i];
        txCmd(c, &op[i + 1], l);
        i += l;
        break;
      }
      case DELAY:
        delay(op[++i]);
        break;
      default:
        Serial.printf("Deck panel: unknown init op %u at %u\n", op[i], (unsigned)i);
        break;
    }
  }
  uint8_t v;
  txCmd(0x20, nullptr, 0);         // INVOFF
  v = 0x00; txCmd(0x36, &v, 1);    // MADCTL: rotation is done in software
  v = 0x05; txCmd(0x3A, &v, 1);    // COLMOD RGB565
  v = 0x00; txCmd(0x35, &v, 1);    // TEON, V-blank only
}

// Full-width window. RASET is deliberately not sent (QSPI quirk); RAMWR
// restarts the frame at the top-left native pixel.
void setWindow() {
  const uint8_t ca[4] = {0, 0, (uint8_t)((NATIVE_W - 1) >> 8),
                         (uint8_t)((NATIVE_W - 1) & 0xFF)};
  txCmd(0x2A, ca, 4);
  txCmd(0x2C, nullptr, 0);
}

void fillTr(spi_transaction_ext_t& t, bool first, const void* buf, size_t len) {
  memset(&t, 0, sizeof(t));
  t.base.flags = SPI_TRANS_MODE_QIO;
  if (first) {
    t.base.cmd = 0x32;
    t.base.addr = 0x003C00;  // RAMWRC pixel stream, as Arduino_GFX does
  } else {
    t.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                    SPI_TRANS_VARIABLE_DUMMY;
  }
  t.base.tx_buffer = buf;
  t.base.length = len * 8;
}

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
      if (!SWAP) c = __builtin_bswap16(c);
      c = dimLE<DIM>(c);
      if (!SWAP) c = __builtin_bswap16(c);
    }
    d[k * dstep] = SWAP ? __builtin_bswap16(c) : c;
  }
}

inline void copyAny(uint16_t* d, int dstep, const uint16_t* s, int n, bool swap, uint8_t dim) {
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
      const uint16_t a0 = SA ? (uint16_t)wa : __builtin_bswap16((uint16_t)wa);
      const uint16_t a1 = SA ? (uint16_t)(wa >> 16) : __builtin_bswap16((uint16_t)(wa >> 16));
      const uint16_t b0 = SB ? (uint16_t)wb : __builtin_bswap16((uint16_t)wb);
      const uint16_t b1 = SB ? (uint16_t)(wb >> 16) : __builtin_bswap16((uint16_t)(wb >> 16));
      d[0] = __builtin_bswap16(mix565(a0, b0, a5));
      d[step] = __builtin_bswap16(mix565(a1, b1, a5));
    }
  }
  for (; k < n; k++, d += step) {
    const uint16_t ca = SA ? a[k] : __builtin_bswap16(a[k]);
    const uint16_t cb = SB ? b[k] : __builtin_bswap16(b[k]);
    *d = __builtin_bswap16(mix565(ca, cb, a5));
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
  for (int k = 0; k < n; k++, d += step) *d = __builtin_bswap16(scale565(SWAP ? s[k] : __builtin_bswap16(s[k]), a5));
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
  for (int k = 0; k < n; k++, d += step) *d = __builtin_bswap16(shade8(SWAP ? s[k] : __builtin_bswap16(s[k]), L));
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
      *out = __builtin_bswap16(mix565(__builtin_bswap16(*out), page.swap ? c : __builtin_bswap16(c), r.a5));
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
      out = __builtin_bswap16(mix565(__builtin_bswap16(out), fg, ca5));
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
    *out = __builtin_bswap16(px);
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

void emitMorphRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int step) {
  const MorphScene& m = f.morph;
  if (m.reveal) {
    emitRevealRow(f, ly, x0, n, d, step);
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
  if (r > l && m.card.swap && m.base.swap) {
    const uint16_t* row = m.card.px + ((uint32_t)(ly - f.wy) * f.wdy >> 16) * m.card.stride;
    const uint32_t sx = (uint32_t)(l - f.wx) * f.wdx;
    uint16_t* out = d + (l - x0) * step;
    const bool ghost = f.wa5 < 32, over = f.wo5 < 32;
    if (ghost && !over && f.wa5 == 0) {  // still all ghost
      const uint16_t g = __builtin_bswap16(m.ghost);
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
      uint16_t px = m.card.swap ? c : __builtin_bswap16(c);
      if (f.wa5 < 32) px = mix565(m.ghost, px, f.wa5);
      if (f.wo5 < 32) {
        uint16_t b = m.base.swap ? base[x] : __builtin_bswap16(base[x]);
        px = mix565(shade8(b, b8), px, f.wo5);
      }
      *out = __builtin_bswap16(px);
    }
  }
  if (end > r) shadeAny(d + (r - x0) * step, step, base + r, end - r, m.base.swap, b8);
}

// Water edge: both pages nearly in place (A drifts away and dims, B drifts
// in), split along a wavy edge with a light line on it. Per-frame values and
// the edge per row (horizontal strip) or per column (vertical strip) are set
// before chunk 0 (flush task only).
struct WaterPrep {
  int16_t edge[NATIVE_H];
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
  if (dist < 0) c = scale565(A.swap ? *pa : __builtin_bswap16(*pa), s_wp.a5);
  else c = B.swap ? *pb : __builtin_bswap16(*pb);
  if (s_wp.line5 && dist >= -2 && dist <= 1)
    c = mix565(c, 0xFFFF, dist == -1 || dist == 0 ? s_wp.line5 : s_wp.line5 / 2);
  return __builtin_bswap16(c);
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

// Logical (lx, ly) to native (nx, ny) for the user rotation.
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
void overlayDots(const Frame& f, uint16_t* dst, int ny0) {
  const int n = f.dotCount;
  if (n < 2 || !f.dotAlpha) return;
  constexpr int SPACING = 14;
  constexpr float DOT_R = 3.5f;
  constexpr float PILL_R = 8.0f;
  const bool v = f.dotsV;
  const float cy = v ? (float)f.h * 0.5f : (float)f.h - 14.0f;
  const float cx = v ? (float)f.w - 14.0f : (float)f.w * 0.5f;
  const float half = (float)(n - 1) * SPACING * 0.5f;  // pill segment half length
  const float hx = v ? 0.0f : half, hy = v ? half : 0.0f;
  const int bx0 = (int)(cx - hx - PILL_R) - 1, bx1 = (int)(cx + hx + PILL_R) + 1;
  const int by0 = (int)(cy - hy - PILL_R) - 1, by1 = (int)(cy + hy + PILL_R) + 1;
  // Quick reject: does the box touch the native rows of this chunk?
  int nxa, nya, nxb, nyb;
  toNative(f.rot, bx0, by0, nxa, nya);
  toNative(f.rot, bx1, by1, nxb, nyb);
  const int lo = nya < nyb ? nya : nyb, hi = nya < nyb ? nyb : nya;
  if (hi < ny0 || lo >= ny0 + ROWS) return;
  const float fade = f.dotAlpha / 255.0f;
  for (int ly = by0; ly <= by1; ly++) {
    for (int lx = bx0; lx <= bx1; lx++) {
      int nx, ny;
      toNative(f.rot, lx, ly, nx, ny);
      if (ny < ny0 || ny >= ny0 + ROWS || nx < 0 || nx >= NATIVE_W) continue;
      const float px = lx + 0.5f, py = ly + 0.5f;
      // Along and across the pill's axis.
      const float along = v ? py : px, mid = v ? cy : cx;
      const float across = v ? px - cx : py - cy;
      float sa = along - mid;
      if (sa < -half) sa += half; else if (sa > half) sa -= half; else sa = 0;
      const float pill = clamp01(PILL_R + 0.5f - sqrtf(sa * sa + across * across));
      if (pill <= 0) continue;
      uint16_t& out = dst[(ny - ny0) * NATIVE_W + nx];
      uint16_t c = __builtin_bswap16(out);
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
      out = __builtin_bswap16(c);
    }
  }
}

// Fill one native chunk (rows ny0 .. ny0+ROWS-1). Mapping per user rotation:
//   0: lx = nx,        ly = ny
//   1: lx = ny,        ly = 319 - nx
//   2: lx = 319 - nx,  ly = 479 - ny
//   3: lx = 479 - ny,  ly = nx
// Part [lo, hi) of a chunk: logical rows in landscape, chunk rows in portrait.
void buildPart(const Frame& f, uint16_t* dst, int ny0, int lo, int hi) {
  if (!f.comp && (f.rot & 1)) {
    // Plain landscape LVGL frame: the hot path, free of the per-run
    // bookkeeping emitRow() needs for composed frames.
    const uint16_t* lv = f.lv;
    if (f.rot == 1) {
      for (int ly = lo; ly < hi; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + ny0;
        uint16_t* d = dst + (NATIVE_W - 1 - ly);
        for (int k = 0; k < ROWS; k++) d[k * NATIVE_W] = __builtin_bswap16(s[k]);
      }
    } else {
      for (int ly = lo; ly < hi; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + (NATIVE_H - ROWS - ny0);
        uint16_t* d = dst + (ROWS - 1) * NATIVE_W + ly;
        for (int k = 0; k < ROWS; k++) d[-k * NATIVE_W] = __builtin_bswap16(s[k]);
      }
    }
  } else {
    switch (f.rot) {
      case 1:
        for (int ly = lo; ly < hi; ly++)
          emitRow(f, ly, ny0, ROWS, dst + (NATIVE_W - 1 - ly), NATIVE_W);
        break;
      case 3:
        for (int ly = lo; ly < hi; ly++)
          emitRow(f, ly, NATIVE_H - ROWS - ny0, ROWS,
                  dst + (ROWS - 1) * NATIVE_W + ly, -NATIVE_W);
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
  if (f.comp && (f.kind == KIND_HSTRIP || f.kind == KIND_VSTRIP) && f.pageFx == PAGE_WATER && ny0 == 0) prepWater(f);
  const int n = (f.rot & 1) ? NATIVE_W : ROWS;
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
  const int64_t now = esp_timer_get_time();
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
    const int64_t elapsed = now + ANIM_LEAD_US - s.t0;
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
    const int64_t t = now + ANIM_LEAD_US - s.t0;
    const bool last = t >= (int64_t)s.durUs;
    const float p = last ? 1.0f : t <= 0 ? 0.0f : easeOutCubic((float)t / (float)s.durUs);
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
    f.base8 = f.morph.dim == 2 ? 4 : 8;  // the scrim
    if (f.morph.reveal) {
      // Out to the card's farthest corner; closing starts slowly (ease in).
      const float dx = (float)std::max(std::abs(f.morph.cx - b.x), std::abs(b.x + b.w - f.morph.cx));
      const float dy = (float)std::max(std::abs(f.morph.cy - b.y), std::abs(b.y + b.h - f.morph.cy));
      const float u = last ? 1.0f : t <= 0 ? 0.0f : (float)t / (float)s.durUs;
      const float q = f.morph.open ? p : 1 - u * u * u;
      f.wr = (int16_t)lroundf((sqrtf(dx * dx + dy * dy) + 1) * q);
    }
    have = true;
  }
  if (!have && (s.mode == COMP_DRAG || s.mode == COMP_ANIM)) {
    int off = s.off;
    if (s.mode == COMP_ANIM) {
      const int64_t t = now + ANIM_LEAD_US - s.t0;
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
      if (!f.a.px) f.a = {cur, (uint16_t)w, true, (uint16_t)h};
      if (!f.b.px) f.b = {cur, (uint16_t)w, true, (uint16_t)h};
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
    s.lastSentDots = dots;
  }
  portEXIT_CRITICAL(&s_mux);
  if (give) xSemaphoreGive(s_lvReady);
  return have;
}

// Wait for a TE rising edge that is fresh enough to start a frame on.
// `backToBack` = this frame follows the previous one without a pause, so a
// missed edge is a real dropped frame rather than an idle panel.
void waitTe(bool backToBack) {
  for (int tries = 0; tries < 4; tries++) {
    portENTER_CRITICAL(&s_teMux);
    const uint32_t seq = s_teSeq;
    const int64_t at = s_teUs;
    portEXIT_CRITICAL(&s_teMux);
    if (seq != s_teUsed) {
      const int64_t age = esp_timer_get_time() - at;
      if (age < TE_LATE_US) {
        s_teUsed = seq;
        return;
      }
      if (tries == 0 && backToBack) st_teLate = st_teLate + 1;
      s_teUsed = seq;  // too late for this one; wait for the next
    }
    xSemaphoreTake(s_teSem, pdMS_TO_TICKS(25));
  }
  st_teTimeout = st_teTimeout + 1;
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

// Send f (chunk 0 already built into s_bounce[0]). While the tail of f is on
// the wire, decide the following frame and pre-build its chunk 0; returns
// true when `next` is ready that way.
bool sendFrame(const Frame& f, Frame& next, bool backToBack) {
  setWindow();  // before the TE wait: the pixel stream must start at once
  waitTe(backToBack);
  const int64_t f0 = esp_timer_get_time();
  if (backToBack && f.comp && s_lastWasComp) {
    const uint32_t gap = (uint32_t)(f0 - s_lastStartUs);
    if (gap > st_gapMax) st_gapMax = gap;
  }
  s_lastStartUs = f0;
  s_lastWasComp = f.comp;

  csLow();
  fillTr(s_tr[0], true, s_bounce[0], CHUNK_BYTES);
  spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[0], portMAX_DELAY);
  int inflight = 1;
  uint32_t build = f.build0Us;
  spi_transaction_t* r;
  for (int i = 1; i < NCHUNK; i++) {
    if (inflight == 2) {
      spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
      inflight--;
    }
    const int64_t b0 = esp_timer_get_time();
    buildChunk(f, s_bounce[i & 1], i * ROWS);
    build += (uint32_t)(esp_timer_get_time() - b0);
    fillTr(s_tr[i], false, s_bounce[i & 1], CHUNK_BYTES);
    spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[i], portMAX_DELAY);
    inflight++;
  }
  // Every pixel of an LVGL frame is now in staging buffers: LVGL may reuse
  // its framebuffer while the DMA tail runs. (Valid only because DMA never
  // reads the framebuffer itself.)
  if (f.releaseLv) xSemaphoreGive(s_lvReady);
  // The second-to-last chunk (even index, s_bounce[0]) finishes first; build
  // the next frame's chunk 0 there while the last chunk is still on the wire.
  spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  inflight--;
  bool have = decide(next);
  if (have) {
    const int64_t b0 = esp_timer_get_time();
    buildChunk(next, s_bounce[0], 0);
    next.build0Us = (uint32_t)(esp_timer_get_time() - b0);
  }
  while (inflight--) spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  csHigh();

  const int64_t end = esp_timer_get_time();
  const uint32_t fl = (uint32_t)(end - f0);
  st_frames = st_frames + 1;
  if (f.comp) st_comp = st_comp + 1;
  st_flushSum = st_flushSum + fl;
  if (fl > st_flushMax) st_flushMax = fl;
  st_buildSum = st_buildSum + build;
  const uint32_t cost = fl + f.build0Us;
  st_frameSum = st_frameSum + cost;
  if (cost > st_frameMax) st_frameMax = cost;
  if (f.comp && f.kind == KIND_FRAME) {
    s_frameStats.frames++;
    s_frameStats.compFrames++;
    s_frameSum += cost;
    s_frameBuildSum += build;
    s_frameFlushSum += fl;
    if (cost > s_frameStats.frameUsMax) s_frameStats.frameUsMax = cost;
    if (fl > s_frameStats.flushUsMax) s_frameStats.flushUsMax = fl;
    if (s_frameStart && f0 - s_frameStart > s_frameStats.gapMaxUs)
      s_frameStats.gapMaxUs = f0 - s_frameStart;
    s_frameStart = f0;
    if (f.frameLast) publishFrameStats();
  }
  return have;
}

static_assert(NCHUNK % 2 == 0, "the last chunk must use s_bounce[1]");

void flushTask(void*) {
  Frame cur = {}, next = {};
  bool ready = false;
  for (;;) {
    portENTER_CRITICAL(&s_mux);
    const bool quiet = s_quiescing;
    const bool partial = quiet && s.kind == KIND_FRAME && !s.frameDone;
    s_quiescing = false;
    portEXIT_CRITICAL(&s_mux);
    if (quiet) {
      if (partial) publishFrameStats();
      if (ready && cur.releaseLv) xSemaphoreGive(s_lvReady);
      ready = false;
      xSemaphoreGive(s_quiet);
    }
    if (!ready) {
      if (!decide(cur)) {
        const CompMode mode = compMode();
        const bool animating = mode == COMP_ANIM || mode == COMP_FRAME;
        ulTaskNotifyTake(pdTRUE, animating ? 1 : pdMS_TO_TICKS(50));
        continue;
      }
      const int64_t b0 = esp_timer_get_time();
      buildChunk(cur, s_bounce[0], 0);
      cur.build0Us = esp_timer_get_time() - b0;
    }
    ready = sendFrame(cur, next, ready);
    if (ready) cur = next;
  }
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

bool panelBegin(uint8_t rotation) {
  s_rot = rotation & 3;
  gpio_reset_pin((gpio_num_t)PIN_CS);
  gpio_set_direction((gpio_num_t)PIN_CS, GPIO_MODE_OUTPUT);
  csHigh();

  spi_bus_config_t b = {};
  b.data0_io_num = PIN_D0;
  b.data1_io_num = PIN_D1;
  b.sclk_io_num = PIN_SCK;
  b.data2_io_num = PIN_D2;
  b.data3_io_num = PIN_D3;
  b.data4_io_num = b.data5_io_num = b.data6_io_num = b.data7_io_num = -1;
  b.max_transfer_sz = CHUNK_BYTES;
  b.flags = SPICOMMON_BUSFLAG_MASTER;
  if (spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t d = {};
  d.command_bits = 8;
  d.address_bits = 24;
  d.mode = 0;
  d.clock_source = SPI_CLK_SRC_DEFAULT;
  d.clock_speed_hz = SPI_HZ;
  d.spics_io_num = -1;  // CS is held low by hand across a whole frame
  d.flags = SPI_DEVICE_HALFDUPLEX;
  d.queue_size = NCHUNK + 2;
  if (spi_bus_add_device(SPI2_HOST, &d, &s_dev) != ESP_OK) return false;

  for (int i = 0; i < 2; i++) {
    s_fb[i] = (uint16_t*)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_bounce[i] = (uint16_t*)heap_caps_aligned_alloc(64, CHUNK_BYTES,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  }
  if (!s_fb[0] || !s_fb[1] || !s_bounce[0] || !s_bounce[1]) {
    Serial.println("Deck panel: buffer allocation FAILED");
    return false;
  }
  memset(s_fb[0], 0, FRAME_BYTES);
  memset(s_fb[1], 0, FRAME_BYTES);

  panelInitSequence();

  // Black first frame, before the backlight comes on.
  memset(s_bounce[0], 0, CHUNK_BYTES);
  setWindow();
  csLow();
  for (int i = 0; i < NCHUNK; i++) {
    fillTr(s_tr[i], i == 0, s_bounce[0], CHUNK_BYTES);
    spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[i], portMAX_DELAY);
  }
  spi_transaction_t* r;
  for (int i = 0; i < NCHUNK; i++) spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  csHigh();

  s_teSem = xSemaphoreCreateBinary();
  s_lvReady = xSemaphoreCreateCounting(2, 0);  // up to two LVGL frames in flight
  s_quiet = xSemaphoreCreateBinary();
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_TE;
  g.mode = GPIO_MODE_INPUT;
  g.intr_type = GPIO_INTR_POSEDGE;
  gpio_config(&g);
  esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  gpio_isr_handler_add((gpio_num_t)PIN_TE, teIsr, nullptr);

  // Core 1 at high priority: on core 0 the flush loses to WiFi/lwIP under
  // network load (measured: ~30 fps slides instead of 60).
  xTaskCreatePinnedToCore(helperTask, "deck_build", 3072, nullptr, 15, &s_helper, 0);
  xTaskCreatePinnedToCore(flushTask, "deck_flush", 4096, nullptr, 20, &s_task, 1);
  Serial.printf("Deck panel: up, rotation %u, internal free %u, largest %u\n",
                (unsigned)s_rot,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

void panelSetRotation(uint8_t rotation) {
  compCancel();
  s_rot = rotation & 3;
}

uint8_t panelRotation() { return s_rot; }

int logicalW() { return (s_rot & 1) ? NATIVE_H : NATIVE_W; }
int logicalH() { return (s_rot & 1) ? NATIVE_W : NATIVE_H; }

uint16_t* panelLvBuffer(int i) { return s_fb[i & 1]; }

const uint16_t* panelLatestLvFrame() {
  portENTER_CRITICAL(&s_mux);
  const int idx = s.lvLatest;
  portEXIT_CRITICAL(&s_mux);
  return idx >= 0 ? s_fb[idx] : nullptr;
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
  if (!m.card.px || m.cw <= 0 || m.ch <= 0) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0) {
    s.mode = COMP_ANIM;
    s.kind = KIND_MORPH;
    s.morph = m;
    // Pinned now: LVGL may draw one more frame into the other buffer.
    int w, h;
    logicalSize(s_rot, w, h);
    if (!s.morph.base.px) s.morph.base = {s_fb[s.lvLatest], (uint16_t)w, true, (uint16_t)h};
    s.morphHoldGen = holdGen;
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

}  // namespace deck
