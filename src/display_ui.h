#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <LovyanGFX.hpp>

// Screen states for the PC monitor. No printer/MQTT states - the device is a
// passive UDP receiver. SCREEN_MONITOR shows the gauge grid; SCREEN_CLOCK is
// the idle screen shown when the PC has gone offline (no UDP packets).
enum ScreenState {
  SCREEN_SPLASH,
  SCREEN_AP_MODE,
  SCREEN_CONNECTING_WIFI,
  SCREEN_WIFI_CONNECTED,
  SCREEN_MONITOR,
  SCREEN_CLOCK,
  SCREEN_OFF,
  SCREEN_OTA_UPDATE
};

extern lgfx::LovyanGFX* tft_ptr;
// Macro (NOT a reference) so callers' `tft.method()` always dereferences the
// current value of `tft_ptr`. The /screen.bmp capture retargets it to an
// off-screen sprite while it renders; a C++ reference would have been permanently bound
// to the panel at static-init time, defeating the redirection.
#define tft (*tft_ptr)

void initDisplay();
void setBacklight(uint8_t level);
// Recompute the effective backlight from day/night scheduling and PC-offline
// sleep. Call updateBacklightControl() from the main loop.
void updateBacklightControl();
void refreshBacklightControl();
uint8_t currentBacklightLevel();
bool nightBrightnessActive();
bool offlineDisplaySleeping();
bool backlightTimeValid();
void setManualDisplayOff(bool off);
bool manualDisplayOff();
bool displaySleepingForTouch();
void wakeDisplayFromTouch();
void applyDisplaySettings();  // re-apply rotation, bg, force redraw

// Renderer (renderer.cpp): screen-state machine + per-frame draw. updateDisplay()
// is throttled internally and safe to call every loop tick; it also redraws fully
// on a state change. setScreenState() marks a transition (forces a clean redraw).
void setScreenState(ScreenState state);
ScreenState getScreenState();
void updateDisplay();
// Force a repaint on the next updateDisplay() without a state change. Pass
// clearFirst=true when the layout footprint may have changed; the panel clear
// is deferred to the display loop. Safe from any context.
void forceDisplayRedraw(bool clearFirst = false);
// Switch the metric page the panel draws. A page swaps every slot's binding at
// once, so this takes the same clearing repaint a metric-mapping save does.
// No-ops on an out-of-range page or on the page already showing.
void setActiveMetricPage(uint8_t page);
// Step to the next/previous page that has bindings, wrapping. Does nothing when
// fewer than two pages are bound, so an unconfigured device never blanks itself.
void cycleMetricPage(int8_t direction);
// Suppress auto-rotation for PageSettings.pinSeconds. Called after any manual
// page change so the page you just picked stays put long enough to read.
void pinMetricPage();
// Auto-rotation tick. Call from loop(), never from updateDisplay().
void handleMetricPageRotation();
// Bracket for the /screen.bmp capture render: while set, the renderer leaves
// the panel's pacing state (packet stamps, layout counts, smoothing, cached
// text widths) untouched so the capture never disturbs the live display.
void setCaptureRender(bool on);

// Portal live preview for the Glass surface sliders. The persisted settings are
// NOT touched: an override sits in front of them until it is cleared, so
// /api/config keeps reporting what is actually stored and the portal's Revert
// genuinely restores the panel. Saving the display form clears the override.
void setGlassPreview(bool on, uint8_t glossPct, uint8_t bowPct, uint8_t fillPct);
// Tell the renderer the panel content was wiped (e.g. after a color change
// repaint) so styles repaint their static chrome on the next frame.
void markScreenCleared();
// Diagnostics: is the sparkline compose sprite allocated (flicker-free path),
// and how many allocation attempts have failed since boot.
bool sparkSpriteActive();
uint16_t sparkSpriteFails();

// Monitor frame compose+blit cost, microseconds. The glass faces run a
// per-pixel compositor at GLASS_UPDATE_MS, so this is what says whether the
// frame budget holds on a given board.
uint32_t monitorFrameUs();
uint32_t monitorFrameMaxUs();
void resetMonitorFrameStats();
// Companion wobble diagnostics: raw per-frame changes seen vs debounced
// transitions the layout actually reacted to.
uint16_t rawNChanges();
uint16_t rawStatusChanges();
uint16_t acceptedRelayouts();
uint16_t acceptedStatusFlips();

// JC3248W535: run the deck UI tick (LVGL, page slides, touch wake) and show
// what the renderer drew. No-op elsewhere. Call once per loop pass after
// updateDisplay(), and from any blocking loop that keeps the screen alive.
void flushFrame();
// JC3248W535: the renderer drew into the face sprite. No-op elsewhere.
void markFrameDirty();

#endif // DISPLAY_UI_H
