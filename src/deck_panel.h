// Guition JC3248W535 display pipeline: AXS15231B over QSPI driven directly
// through spi_master, one flush task that owns the panel bus, software
// rotation of a logical framebuffer into internal DMA bounce buffers, frames
// started on the TE rising edge, and a page-slide compositor that builds
// transition frames from cached page images without asking LVGL to render.
//
// Facts this rests on (measured in the Phase 0 spike, see the deck plan):
// the panel only accepts full native 320x480 frames (no MADCTL rotation, the
// CASET start column is ignored), 40 MHz is the clock ceiling, a full frame is
// 15.4 ms on the wire against a 16.39 ms TE period, and the write is slower
// than the scan, so a frame must start within ~1 ms of the TE rising edge.
#ifndef DECK_PANEL_H
#define DECK_PANEL_H

#if defined(BOARD_IS_JC3248W535)

#include <Arduino.h>

namespace deck {

static constexpr int NATIVE_W = 320;
static constexpr int NATIVE_H = 480;
static constexpr size_t FRAME_BYTES = (size_t)NATIVE_W * NATIVE_H * 2;

// A full-screen logical image (logicalW() x logicalH()). stride is in pixels.
// swap=true for little-endian RGB565 (LVGL), false for data already in panel
// byte order (LovyanGFX sprites). px == nullptr means "the latest complete
// LVGL frame", resolved by the flush task when the frame is built.
struct Src {
  const uint16_t* px;
  uint16_t stride;
  bool swap;
};

// Bus, panel init, a black first frame, TE interrupt and the flush task.
// Backlight is left alone. Returns false if a buffer could not be allocated.
bool panelBegin(uint8_t rotation);

// User rotation 0..3, same meaning as LovyanGFX setRotation() on the old
// full-frame sprite (1 = landscape with USB-C on the right).
void panelSetRotation(uint8_t rotation);
uint8_t panelRotation();
int logicalW();
int logicalH();

// The two logical PSRAM framebuffers LVGL renders into (direct mode).
uint16_t* panelLvBuffer(int i);
// The latest complete LVGL frame (little-endian RGB565, logicalW() stride),
// or nullptr before the first one. Stable while the LVGL task is busy
// elsewhere, e.g. inside a web handler.
const uint16_t* panelLatestLvFrame();

// LVGL side. panelPostLv() is called from the flush callback on the last
// area of a frame; gen is the UI generation the frame was rendered under.
// panelWaitLvReady() blocks the LVGL task until the flush task has copied
// every pixel out of that framebuffer (or dropped the frame).
void panelPostLv(int idx, uint32_t gen);
bool panelWaitLvReady(uint32_t timeoutMs);

// --- Page-slide compositor ---------------------------------------------------
// The visible strip is A | B, offset `off` pixels into it (0 = all A,
// logicalW() = all B). Only the touch task drives drags; only the flush task
// advances animations.
enum CompMode : uint8_t { COMP_NONE = 0, COMP_DRAG, COMP_ANIM, COMP_HOLD };

bool compBeginDrag(const Src& a, const Src& b, int off);
void compDrag(int off);
// Animate to `toOff` over durMs. landing != 0 means the slide ends on the
// other page (the strip stays frozen until LVGL shows it, see compSetHoldGen),
// landing == 0 means it returns to the page it started from.
void compRelease(int toOff, uint32_t durMs, int8_t landing);
CompMode compMode();
// True while LVGL must not render (drag or animation in progress).
bool compOwnsScreen();
// Loop side: true once after an animation finished. *landing is +1/-1 when
// the slide ended on the next/previous page, 0 when it went back.
bool compTakeLanding(int8_t* landing);
// After switching LVGL to the new page, release the frozen strip as soon as an
// LVGL frame rendered under generation >= gen arrives.
void compSetHoldGen(uint32_t gen);
// Abort any slide immediately (rotation change, system screen).
void compCancel();

struct PanelStats {
  uint32_t frames;
  uint32_t compFrames;
  uint32_t flushUsAvg;
  uint32_t flushUsMax;
  uint32_t buildUsAvg;
  uint32_t teLate;      // edges skipped because the frame was not ready in time
  uint32_t teTimeouts;  // frames sent without a TE edge (TE line dead)
  uint32_t lvDropped;   // LVGL frames superseded by the compositor
  uint32_t gapMaxUs;    // longest interval between frame starts while animating
};
// Snapshot and reset the counters.
PanelStats panelTakeStats();

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_PANEL_H
