// Guition JC3248W535 display pipeline. See deck_panel.h for the contract and
// the measured panel facts behind it.
#if defined(BOARD_IS_JC3248W535)

#include "deck_panel.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
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
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

enum Kind : uint8_t { KIND_HSTRIP = 0, KIND_PANEL };

// Page dots: shown while a horizontal slide owns the screen, then held and
// faded out over LVGL frames by this task (LVGL never draws them).
constexpr int64_t DOTS_HOLD_US = 1200000;
constexpr int64_t DOTS_FADE_US = 300000;

struct State {
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
  uint32_t holdGen = UINT32_MAX;
  bool landedPending = false;
  int8_t landed = 0;
  // pager / dots
  int pageCount = 1;
  int pageActive = 0;
  bool dotsForce = false;
  int64_t dotsUntil = 0;
  uint8_t lastSentDots = 0;
};
State s;

// A frame to send, resolved under the lock so the build never reads shared
// state: one LVGL framebuffer, a horizontal A|B strip, or a panel pulled down
// over a (dimmed) page. Plus the page-dot overlay.
struct Frame {
  bool comp;
  bool releaseLv;
  uint8_t rot;
  int w, h;
  const uint16_t* lv;
  Kind kind;
  Src a, b;
  int off;
  uint8_t dim;
  uint8_t dotCount, dotActive, dotAlpha;
};

// --- Stats (approximate, single writer) ----------------------------------------
volatile uint32_t st_frames = 0, st_comp = 0, st_flushSum = 0, st_flushMax = 0;
volatile uint32_t st_buildSum = 0, st_teLate = 0, st_teTimeout = 0, st_lvDropped = 0;
volatile uint32_t st_gapMax = 0;
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

// Emit n pixels of logical row ly starting at logical x0 (ascending). PSRAM
// is always read forward; mirrored rotations walk the destination backwards.
inline void emitRow(const Frame& f, int ly, int x0, int n, uint16_t* d, int dstep) {
  if (!f.comp) {
    copyAny(d, dstep, f.lv + ly * f.w + x0, n, true, 0);
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

// Page dots on a translucent pill, bottom centre, drawn into the native chunk
// (panel byte order) after it was built.
void overlayDots(const Frame& f, uint16_t* dst, int ny0) {
  const int n = f.dotCount;
  if (n < 2 || !f.dotAlpha) return;
  constexpr int SPACING = 14;
  constexpr float DOT_R = 3.5f;
  constexpr float PILL_R = 8.0f;
  const float cy = (float)f.h - 14.0f;
  const float cx = (float)f.w * 0.5f;
  const float half = (float)(n - 1) * SPACING * 0.5f;  // pill segment half length
  const int bx0 = (int)(cx - half - PILL_R) - 1, bx1 = (int)(cx + half + PILL_R) + 1;
  const int by0 = (int)(cy - PILL_R) - 1, by1 = (int)(cy + PILL_R) + 1;
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
      float sx = px - cx;
      if (sx < -half) sx += half; else if (sx > half) sx -= half; else sx = 0;
      const float dy = py - cy;
      const float pill = clamp01(PILL_R + 0.5f - sqrtf(sx * sx + dy * dy));
      if (pill <= 0) continue;
      uint16_t& out = dst[(ny - ny0) * NATIVE_W + nx];
      uint16_t c = __builtin_bswap16(out);
      c = blendLE(c, 0x0000, (uint32_t)(150 * pill * fade));
      const int i = (int)lroundf((px - (cx - half)) / SPACING);
      if (i >= 0 && i < n) {
        const float dx = px - (cx - half + i * SPACING);
        const float dot = clamp01(DOT_R + 0.5f - sqrtf(dx * dx + dy * dy));
        if (dot > 0) {
          const uint16_t col = i == f.dotActive ? 0xFFFF : 0x8C71;
          c = blendLE(c, col, (uint32_t)(255 * dot * fade));
        }
      }
      out = __builtin_bswap16(c);
    }
  }
}

// Fill one native chunk (rows ny0 .. ny0+ROWS-1). Mapping per user rotation
// (identical to LovyanGFX's sprite rotation):
//   0: lx = nx,        ly = ny
//   1: lx = ny,        ly = 319 - nx
//   2: lx = 319 - nx,  ly = 479 - ny
//   3: lx = 479 - ny,  ly = nx
void buildChunk(const Frame& f, uint16_t* dst, int ny0) {
  if (!f.comp && (f.rot & 1)) {
    // Plain landscape LVGL frame: the hot path, free of the per-run
    // bookkeeping emitRow() needs for composed frames.
    const uint16_t* lv = f.lv;
    if (f.rot == 1) {
      for (int ly = 0; ly < NATIVE_W; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + ny0;
        uint16_t* d = dst + (NATIVE_W - 1 - ly);
        for (int k = 0; k < ROWS; k++) d[k * NATIVE_W] = __builtin_bswap16(s[k]);
      }
    } else {
      for (int ly = 0; ly < NATIVE_W; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + (NATIVE_H - ROWS - ny0);
        uint16_t* d = dst + (ROWS - 1) * NATIVE_W + ly;
        for (int k = 0; k < ROWS; k++) d[-k * NATIVE_W] = __builtin_bswap16(s[k]);
      }
    }
  } else {
    switch (f.rot) {
      case 1:
        for (int ly = 0; ly < NATIVE_W; ly++)
          emitRow(f, ly, ny0, ROWS, dst + (NATIVE_W - 1 - ly), NATIVE_W);
        break;
      case 3:
        for (int ly = 0; ly < NATIVE_W; ly++)
          emitRow(f, ly, NATIVE_H - ROWS - ny0, ROWS,
                  dst + (ROWS - 1) * NATIVE_W + ly, -NATIVE_W);
        break;
      case 2:
        for (int k = 0; k < ROWS; k++)
          emitRow(f, NATIVE_H - 1 - (ny0 + k), 0, NATIVE_W,
                  dst + k * NATIVE_W + (NATIVE_W - 1), -1);
        break;
      default:
        for (int k = 0; k < ROWS; k++)
          emitRow(f, ny0 + k, 0, NATIVE_W, dst + k * NATIVE_W, 1);
        break;
    }
  }
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
  f.dotCount = (uint8_t)s.pageCount;
  f.dotActive = (uint8_t)s.pageActive;
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
        if (s.kind == KIND_HSTRIP) {
          s.dotsForce = false;
          s.dotsUntil = now + DOTS_HOLD_US + DOTS_FADE_US;
          if (s.landing) s.pageActive = s.pageA + (s.landing > 0 ? 1 : 0);
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
      const uint16_t* cur = s_fb[s.lvLatest];
      if (!f.a.px) f.a = {cur, (uint16_t)w, true, (uint16_t)h};
      if (!f.b.px) f.b = {cur, (uint16_t)w, true, (uint16_t)h};
      if (s.kind == KIND_HSTRIP) {
        f.off = off < 0 ? 0 : (off > w ? w : off);
        f.dotActive = (uint8_t)(s.pageA + (f.off * 2 >= w ? 1 : 0));
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
  const bool panelOwns = s.kind == KIND_PANEL && s.mode != COMP_NONE;
  const uint8_t dots = panelOwns ? 0 : dotsAlphaAt(now);
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
  uint32_t build = 0;
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
    build += (uint32_t)(esp_timer_get_time() - b0);
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
  return have;
}

static_assert(NCHUNK % 2 == 0, "the last chunk must use s_bounce[1]");

void flushTask(void*) {
  Frame cur = {}, next = {};
  bool ready = false;
  for (;;) {
    if (!ready) {
      if (!decide(cur)) {
        const bool animating = compMode() == COMP_ANIM;
        ulTaskNotifyTake(pdTRUE, animating ? 1 : pdMS_TO_TICKS(50));
        continue;
      }
      buildChunk(cur, s_bounce[0], 0);
    }
    ready = sendFrame(cur, next, ready);
    if (ready) cur = next;
  }
}

}  // namespace

// --- Public API -------------------------------------------------------------------
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
  s_lvReady = xSemaphoreCreateBinary();
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
  s.lvPending = true;
  s.lvIdx = idx & 1;
  s.lvGen = gen;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

bool panelWaitLvReady(uint32_t timeoutMs) {
  return xSemaphoreTake(s_lvReady, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool compBeginDrag(const Src& a, const Src& b, int off, int pageA) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0) {
    s.mode = COMP_DRAG;
    s.kind = KIND_HSTRIP;
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

void panelSetPager(int count, int active) {
  portENTER_CRITICAL(&s_mux);
  s.pageCount = count;
  s.pageActive = active;
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
  return m == COMP_DRAG || m == COMP_ANIM;
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

PanelStats panelTakeStats() {
  PanelStats p;
  const uint32_t n = st_frames;
  p.frames = n;
  p.compFrames = st_comp;
  p.flushUsAvg = n ? st_flushSum / n : 0;
  p.flushUsMax = st_flushMax;
  p.buildUsAvg = n ? st_buildSum / n : 0;
  p.teLate = st_teLate;
  p.teTimeouts = st_teTimeout;
  p.lvDropped = st_lvDropped;
  p.gapMaxUs = st_gapMax;
  st_frames = 0;
  st_comp = 0;
  st_flushSum = 0;
  st_flushMax = 0;
  st_buildSum = 0;
  st_teLate = 0;
  st_teTimeout = 0;
  st_lvDropped = 0;
  st_gapMax = 0;
  return p;
}

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
