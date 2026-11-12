// Contract between the frame compositor (deck_panel.cpp) and the display
// transport of the board (deck_panel_<bus>.cpp). Only those files include it.
//
// The compositor decides what the next frame is and builds it, in native
// panel orientation and byte order, PANEL_ROWS rows at a time. The transport
// owns the bus and the flush task: it asks for frames, gives them buffers to
// be built into and puts them on the glass.
#ifndef DECK_PANEL_INT_H
#define DECK_PANEL_INT_H

#include "deck_panel.h"

namespace deck {

#if defined(BOARD_DISPLAY_AXS15231B_QSPI)
// 48 native rows per chunk: 30,720 bytes (the S3 limit for one SPI
// transaction is 32 KB), 10 chunks per frame. Smaller chunks save internal
// RAM but every transaction adds overhead, and with 20 chunks a frame grew
// to ~16 ms, too close to the 16.39 ms TE period.
constexpr int PANEL_ROWS = 48;
// Animations are sampled this far past the next TE edge; without recent TE,
// this far ahead of the build.
constexpr int64_t PANEL_SYNC_PERIOD_US = 16390;
constexpr int64_t PANEL_SYNC_LEAD_US = 6000;
constexpr int64_t PANEL_ANIM_LEAD_US = 8000;
constexpr bool PANEL_BIG_ENDIAN = true;
#elif defined(BOARD_DISPLAY_MIPI_DSI)
// One chunk = the whole frame, built straight into the back framebuffer.
constexpr int PANEL_ROWS = NATIVE_H;
// Frame times come from transportPresentUs(); these are the nominal 60 Hz
// period and the fallback lead before the first refresh was seen.
constexpr int64_t PANEL_SYNC_PERIOD_US = 16667;
constexpr int64_t PANEL_SYNC_LEAD_US = 0;
constexpr int64_t PANEL_ANIM_LEAD_US = 12000;
constexpr bool PANEL_BIG_ENDIAN = false;
#else
#error "The board header selects no display transport"
#endif

enum Kind : uint8_t { KIND_HSTRIP = 0, KIND_VSTRIP, KIND_PANEL, KIND_FRAME, KIND_MORPH };

// What changed between LVGL frames, as a grid of cells (32 columns, native
// orientation): a transport that keeps old frames copies only these.
constexpr int DIRTY_CW = (NATIVE_W + 31) / 32;
constexpr int DIRTY_CH = 24;
constexpr int DIRTY_ROWS = (NATIVE_H + DIRTY_CH - 1) / DIRTY_CH;
struct DirtyMask {
  uint32_t rows[DIRTY_ROWS];
  bool full;  // everything, the cells are not looked at
  void clear() { memset(rows, 0, sizeof(rows)); full = false; }
  void add(const DirtyMask& o) {
    full = full || o.full;
    for (int r = 0; r < DIRTY_ROWS; r++) rows[r] |= o.rows[r];
  }
  void addRect(int x0, int y0, int x1, int y1) {  // inclusive, clipped here
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= NATIVE_W) x1 = NATIVE_W - 1;
    if (y1 >= NATIVE_H) y1 = NATIVE_H - 1;
    if (x0 > x1 || y0 > y1) return;
    const int c0 = x0 / DIRTY_CW, c1 = x1 / DIRTY_CW;
    const uint32_t bits = (c1 >= 31 ? 0xFFFFFFFFu : (1u << (c1 + 1)) - 1) & ~((1u << c0) - 1);
    for (int r = y0 / DIRTY_CH; r <= y1 / DIRTY_CH; r++) rows[r] |= bits;
  }
};

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
  // Plain LVGL frame: what differs from the plain frame decided before it.
  DirtyMask dirty;
};

// --- Compositor side, called from the transport's flush task ---
// The next frame to show; false when nothing changed.
bool frameDecide(Frame& f);
// Native rows ny0 .. ny0 + PANEL_ROWS - 1 of f into dst (row stride NATIVE_W).
void frameBuild(const Frame& f, uint16_t* dst, int ny0);
// Plain LVGL frame over an older one in dst (a whole frame): only the cells of m.
// False when f cannot be built that way; nothing was written then.
bool frameBuildDirty(const Frame& f, uint16_t* dst, const DirtyMask& m);
// Nothing to show: sleep until LVGL posts a frame or an animation needs one.
void frameIdleWait();
// f no longer reads its LVGL framebuffer: LVGL may render into it again.
void frameReleaseLv();
// Statistics: f goes out now; then it is on the glass (or queued) with these costs.
void frameStarted(const Frame& f, int64_t startUs, bool backToBack);
void frameFinished(const Frame& f, int64_t startUs, uint32_t buildUs, uint32_t flushUs, uint32_t costUs);
// A sync edge came too late to use / never came.
void countLateSync();
void countMissedSync();
// panelQuiesce() is waiting: drop any half-prepared frame, then ack.
bool quiesceRequested();
void quiesceAck();

// --- Transport side, one implementation per display bus ---
// Bus, panel init and a black first frame; the backlight is left alone.
bool transportBegin();
// The flush loop (core 1, high priority).
void transportTask(void*);
// esp_timer time of the last panel sync edge (TE), 0 when the bus has none.
int64_t transportLastSyncUs();
// When a frame decided now will be on the glass; 0 = no prediction, the
// compositor uses the sync edge and the constants above.
int64_t transportPresentUs(int64_t nowUs);

}  // namespace deck

#endif  // DECK_PANEL_INT_H
