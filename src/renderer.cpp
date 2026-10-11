// Renderer: screen-state machine + per-frame drawing for StatNode.
// Replaces BambuHelper's printer-centric display_ui rendering. Screens:
//   SPLASH/AP/CONNECTING/CONNECTED - status text
//   MONITOR  - gauge grid driven by pcData (auto-mapped for now; configurable
//              metric->gauge assignment lands in the web-portal step)
//   CLOCK    - idle screen shown when the PC is offline
//   OTA_UPDATE - "Updating..." hold screen
#include "display_ui.h"
#include "display_gauges.h"
#include "clock_mode.h"
#include "clock_runner.h"
#include "clock_pong.h"
#include "pc_metrics.h"
#include "settings.h"
#include "fonts.h"
#include "config.h"
#include "layout.h"
#include "wifi_manager.h"
#include "renderer_internal.h"
#include "glass_surface.h"
#include <WiFi.h>
#if defined(BOARD_IS_JC3248W535)
#include "deck_ui.h"
#endif
#include <math.h>

static ScreenState currentScreen = SCREEN_SPLASH;
static bool forceRedraw = true;
static bool clearBeforeRedraw = false;
static unsigned long lastUpdate = 0;
static bool prevOnline = false;

ScreenState getScreenState() { return currentScreen; }

void markScreenCleared();  // defined below with the style helpers

void setScreenState(ScreenState state) {
  if (state == currentScreen) return;
  currentScreen = state;
  forceRedraw = true;
  clearBeforeRedraw = false;
  lastUpdate = 0;            // bypass throttle so the new screen paints at once
  tft.fillScreen(dispSettings.bgColor);
  resetGaugeTextCache();
  resetClock();
  resetRunnerClock();
  resetPongClock();
  markScreenCleared();
  markFrameDirty();
}

// ---------------------------------------------------------------------------
//  Status screens
// ---------------------------------------------------------------------------
static void drawCenteredLines(const char* title, uint16_t titleColor,
                              const char* l1, const char* l2, const char* l3) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  tft.setTextDatum(MC_DATUM);

  setFont(tft, FONT_LARGE);
  tft.setTextColor(titleColor, dispSettings.bgColor);
  tft.drawString(title, w / 2, h / 2 - 40);

  setFont(tft, FONT_BODY);
  tft.setTextColor(CLR_TEXT, dispSettings.bgColor);
  if (l1 && l1[0]) tft.drawString(l1, w / 2, h / 2 - 6);
  setFont(tft, FONT_SMALL);
  tft.setTextColor(CLR_TEXT_DIM, dispSettings.bgColor);
  if (l2 && l2[0]) tft.drawString(l2, w / 2, h / 2 + 18);
  if (l3 && l3[0]) tft.drawString(l3, w / 2, h / 2 + 36);
}

static void drawApScreen() {
  String ssid = getAPSSID();
  drawCenteredLines("WiFi Setup", CLR_ORANGE,
                    ssid.c_str(), "Pass: " WIFI_AP_PASSWORD, "Open 192.168.4.1");
}

static void drawConnectingScreen() {
  drawCenteredLines("Connecting", CLR_BLUE, wifiSSID, "Please wait...", "");
}

static void drawConnectedScreen() {
  String ip = WiFi.localIP().toString();
  drawCenteredLines("Connected", CLR_GREEN, ip.c_str(), "Open IP in browser", "");
}

static void drawOtaScreen() {
  drawCenteredLines("Updating", CLR_GOLD, "Firmware upload", "Do not power off", "");
}


// Status badge - the LHM trouble indicator now that the bottom bar is gone.
// A small dot in the top-right corner, drawn ONLY while the companion reports
// a non-OK status, so the screen stays clean in the healthy steady state.
// Repainted every frame (it may overlay a cell/tile that redraws itself);
// the status-flip full repaint in drawMonitorStyled clears it residue-free.
static void drawStatusBadge(int16_t w) {
  uint16_t color;
  switch (gStableStatus) {
    case PC_STATUS_API_ERROR:       color = CLR_ORANGE; break;
    case PC_STATUS_LHM_NOT_RUNNING: color = CLR_RED;    break;
    case PC_STATUS_LHM_STARTING:    color = CLR_YELLOW; break;
    default: return;   // OK: no badge
  }
  tft.fillCircle(w - 9, 9, 4, color);
  // Separation ring. On glass the flat bgColor would punch a dark hole in the
  // gradient, so the ring wears the backdrop colour of the row it sits on.
  const uint16_t ring = surfaceUsesGlass(activeFace().surface)
    ? rgbTo565(glassSkyAt(9), w - 9, 9) : dispSettings.bgColor;
  tft.drawCircle(w - 9, 9, 5, ring);
}

// Dispatch on the configured monitor face. A live style change (portal save)
// or a companion status flip arrives without a screen-state transition, so
// the previous pixels are still on the panel: detect both here and start
// from a clean screen.
// Compose+blit cost of the last monitor frame, and the worst seen since boot.
// The glass faces run a per-pixel compositor at 20 Hz, so this is the number
// that says whether the frame budget actually holds on a given board.
static uint32_t gFrameUs = 0, gFrameMaxUs = 0;
uint32_t monitorFrameUs()    { return gFrameUs; }
uint32_t monitorFrameMaxUs() { return gFrameMaxUs; }
void resetMonitorFrameStats() { gFrameMaxUs = 0; }

static void drawMonitorStyled(bool fr) {
  const uint32_t t0 = micros();
  const ActiveFace face = activeFace();
  // The whole render tuple, not the face family: a bands-only or surface-only
  // change moves geometry underneath a text cache that is keyed on coordinates
  // and strings alone, so it cannot notice on its own.
  static uint8_t lastLayout = 0xFF, lastSurface = 0xFF;
  static uint8_t lastBands = 0xFF, lastRows = 0xFF;
  static uint8_t lastPcStatus = 0xFF;
  const uint8_t stableStatus = debouncedStatus();
  const bool first = (lastLayout == 0xFF);
  const bool faceChanged = face.layout != lastLayout || face.surface != lastSurface ||
                           face.bands != lastBands || face.rowStyle != lastRows;
  if (!gCaptureRender && (faceChanged || stableStatus != lastPcStatus)) {
    if (!first) {
      tft.fillScreen(dispSettings.bgColor);
      resetGaugeTextCache();
      gScreenCleared = true;
      fr = true;
    }
    // Leaving glass frees the compose buffers - the glass layouts size their
    // sprites differently and must not both hold one while only one can draw.
    // Keyed on the NEW surface, independently of what else moved.
    if (faceChanged && !surfaceUsesGlass(face.surface)) glassReleaseSprites();
    // A face change is one of the two moments the per-slot hysteresis is known
    // to be stale; the other is a metric-mapping save (forceDisplayRedraw).
    // Warn latches and chart bounds go stale with the wash step, so clearing
    // only the wash left a slot wearing the previous face's warn colour and
    // charting on its old scale.
    if (faceChanged) {
      resetWashState();
      resetSlotWarnState();
      resetSparkBoundsState();
    }
    lastLayout = face.layout;
    lastSurface = face.surface;
    lastBands = face.bands;
    lastRows = face.rowStyle;
    lastPcStatus = stableStatus;
  }
  const SurfaceCtx sfc = surfaceCtxFor(face.surface);
  switch (face.layout) {
    case LAYOUT_BIG_NUMBERS: drawBigNumbersScreen(fr); break;
    case LAYOUT_STRIPS:      drawStripsScreen(fr, sfc); break;
    case LAYOUT_DUO:
      drawDuoLayout(fr, sfc, face.bands, face.rowStyle);
      break;
    case LAYOUT_TILES:
    default:                 drawTilesLayout(fr, sfc); break;
  }
  drawStatusBadge((int16_t)tft.width());
  if (!gCaptureRender) {
    gScreenCleared = false;
    gFrameUs = micros() - t0;
    if (gFrameUs > gFrameMaxUs) gFrameMaxUs = gFrameUs;
  }
}

static void drawIdleClock() {
  switch (clockFace) {
    case CLOCK_FACE_BREAKOUT:
      if (gCaptureRender) drawPongClockSnapshot();
      else tickPongClock();
      break;
    case CLOCK_FACE_RUNNER:
      if (gCaptureRender) drawRunnerClockSnapshot();
      else tickRunnerClock();
      break;
    default:
      if (gCaptureRender) drawClockSnapshot();
      else drawClock();
      break;
  }
}

// ---------------------------------------------------------------------------
//  Frame entry point
// ---------------------------------------------------------------------------
void updateDisplay() {
#if defined(BOARD_IS_JC3248W535)
  // The face sprite can be on screen as a page image during a swipe, and is
  // only refreshed slowly while a deck page is showing. Pending redraw flags
  // stay set, so a skipped call is simply picked up by a later one.
  if (!gCaptureRender && !deckFaceMayRender()) return;
#endif
  unsigned long now = millis();
  bool fr = forceRedraw;

  // Layout edits can move or shrink text and cards, leaving pixels that an
  // opaque value-only repaint does not cover. Clear only on the real panel and
  // only from this display-loop context; capture renders already start with a
  // freshly filled sprite and must not consume a pending panel clear.
  if (clearBeforeRedraw && !gCaptureRender) {
    tft.fillScreen(dispSettings.bgColor);
    resetGaugeTextCache();
    markScreenCleared();
    clearBeforeRedraw = false;
    fr = true;
  }

  // Animated clock faces bypass the monitor renderer's slower update cadence.
  // Screenshot renderers are stateless and never advance the live animation.
  const bool idleVisible = currentScreen == SCREEN_CLOCK ||
                           (currentScreen == SCREEN_MONITOR && !pcData.online);
  const bool animatedClock = clockFace == CLOCK_FACE_BREAKOUT ||
                             clockFace == CLOCK_FACE_RUNNER;
  if (idleVisible && animatedClock) {
    if (gCaptureRender) {
      drawIdleClock();
    } else {
      if (currentScreen == SCREEN_MONITOR && prevOnline) {
        tft.fillScreen(dispSettings.bgColor);
        resetGaugeTextCache();
        markScreenCleared();
      }
      drawIdleClock();
      if (currentScreen == SCREEN_MONITOR) prevOnline = false;
    }
    forceRedraw = false;
    return;
  }

  // EVERY monitor face animates its charts now, not just the glass pair, so
  // they all need a frame rate the eye reads as motion rather than the 4 Hz the
  // static screens are paced at. Only while the monitor screen is actually
  // showing a live feed - an idle clock or an offline panel keeps the calm
  // cadence, where there is nothing to animate anyway.
  //
  // Pace off what the LAST frame actually cost so a heavy face can never run
  // back to back and starve WiFi and the web server. Measured on the C3 at
  // 240x240 with 7 bound slots: Big 0.7 ms, Pulse 0.8 ms, Hero 7 ms, Duo 13 ms,
  // Tiles 21 ms, Liquid 46 ms, Aero 96 ms - but Strips 135 ms, because it
  // composes a full-width sprite per lane. A fixed 100 ms would leave that face
  // permanently overrunning. Cheap faces still get the full animation rate; an
  // expensive one degrades to a slower glide instead of eating the loop. This
  // also protects the bigger panels, where every face costs more.
  uint32_t frameInterval = GLASS_UPDATE_MS;
  const uint32_t lastCostMs = gFrameUs / 1000;
  const uint32_t paced = lastCostMs + (lastCostMs >> 1);   // cost + 50% headroom
  if (paced > frameInterval) frameInterval = paced;
  if (frameInterval > 500) frameInterval = 500;
  if (!(currentScreen == SCREEN_MONITOR && pcData.online))
    frameInterval = DISPLAY_UPDATE_MS;
  if (!forceRedraw && (now - lastUpdate < frameInterval)) return;
  lastUpdate = now;

  switch (currentScreen) {
    case SCREEN_SPLASH:           break;  // initDisplay drew it
    case SCREEN_AP_MODE:          if (fr) drawApScreen();        break;
    case SCREEN_CONNECTING_WIFI:  if (fr) drawConnectingScreen(); break;
    case SCREEN_WIFI_CONNECTED:   if (fr) drawConnectedScreen();  break;
    case SCREEN_OTA_UPDATE:       if (fr) drawOtaScreen();        break;
    case SCREEN_MONITOR:
      // Switch to the idle clock when the PC drops offline, back when it returns.
      if (pcData.online) {
        if (!gCaptureRender && (fr || prevOnline != pcData.online)) {
          if (!prevOnline) {
            tft.fillScreen(dispSettings.bgColor);
            resetGaugeTextCache();
            markScreenCleared();
            fr = true;
          }
        }
        drawMonitorStyled(fr);
      } else {
        if (!gCaptureRender && prevOnline) {
          tft.fillScreen(dispSettings.bgColor);
          resetGaugeTextCache();
          markScreenCleared();
          fr = true;
        }
        drawIdleClock();
      }
      // A capture render must not consume an offline->online transition; the
      // panel still needs its physical clear on the next real frame.
      if (!gCaptureRender) prevOnline = pcData.online;
      break;
    case SCREEN_CLOCK:
      drawIdleClock();
      break;
    case SCREEN_OFF:
      break;
  }

  forceRedraw = false;
  markFrameDirty();
}

// Force a repaint on the next updateDisplay() without a screen-state transition.
// Layout-changing callers request a deferred panel clear; capture and live-value
// callers retain the flicker-free opaque repaint path.
void forceDisplayRedraw(bool clearFirst) {
  forceRedraw = true;
  if (clearFirst) clearBeforeRedraw = true;
  lastUpdate = 0;
  resetGaugeTextCache();
  // The clearing form is what a metric-mapping save asks for, and a rebind or
  // a scale change is exactly when a slot's wash step, warn latch and smoothed
  // chart bounds all stop meaning anything.
  if (clearFirst) {
    resetWashState();
    resetSlotWarnState();
    resetSparkBoundsState();
  }
}

void setActiveMetricPage(uint8_t page) {
  if (page >= METRIC_PAGE_COUNT || page == activeMetricPage) return;
  activeMetricPage = page;
  // A page switch rebinds every slot at once, which is the strongest form of
  // the metric-mapping save this call already models: it clears the panel, the
  // gauge text cache and all three per-slot hysteresis states.
  forceDisplayRedraw(true);
}

void cycleMetricPage(int8_t direction) {
  if (metricPageCountBound() < 2) return;
  for (uint8_t step = 1; step <= METRIC_PAGE_COUNT; step++) {
    int candidate = (int)activeMetricPage + direction * step;
    while (candidate < 0) candidate += METRIC_PAGE_COUNT;
    candidate %= METRIC_PAGE_COUNT;
    if (metricPageHasBindings((uint8_t)candidate)) {
      setActiveMetricPage((uint8_t)candidate);
      return;
    }
  }
}

// Automatic page rotation.
//
// Ticked from loop() rather than from updateDisplay() on purpose: the
// \screen.bmp handler calls updateDisplay() synchronously, so a timer living
// there would advance the page every time the portal takes its five second
// screenshot. The gCaptureRender guard below is belt and braces for the same
// reason.
static uint32_t gPageRotateAt = 0;
static uint32_t gPagePinUntil = 0;
static uint32_t gPageRotateInterval = 0;
static bool gPageRotationActive = false;
static bool gPagePinned = false;

static void resetMetricPageRotationDeadline() {
  gPageRotateAt = 0;
  gPageRotateInterval = 0;
  gPageRotationActive = false;
}

void pinMetricPage() {
  gPagePinned = pageSettings.pinSeconds != 0;
  gPagePinUntil = millis() + (uint32_t)pageSettings.pinSeconds * 1000u;
  // Re-arm from the pin expiry, not from the old deadline.
  resetMetricPageRotationDeadline();
}

void handleMetricPageRotation() {
  if (gCaptureRender) return;

  const uint32_t now = millis();
  // Expire the pin even while rotation is disabled or the clock is showing.
  // The loop observes the short deadline continuously, so it is cleared before
  // the signed wrap-safe comparison could become ambiguous 2^31 ms later.
  if (gPagePinned && (int32_t)(now - gPagePinUntil) >= 0) {
    gPagePinned = false;
    resetMetricPageRotationDeadline();
  }

  // Rotating behind a clock or a blank panel would just mean the user finds a
  // different page than the one they left when the monitor comes back. Reset
  // the deadline so returning to the monitor grants a complete interval.
  if (pageSettings.rotateSeconds == 0 || getScreenState() != SCREEN_MONITOR ||
      metricPageCountBound() < 2) {
    resetMetricPageRotationDeadline();
    return;
  }

  if (gPagePinned) return;

  const uint32_t step = pageSettings.rotateUnit ? 60000u : 1000u;
  const uint32_t interval = (uint32_t)pageSettings.rotateSeconds * step;
  // A changed value or unit starts a fresh interval instead of inheriting the
  // deadline calculated from the previous configuration.
  if (!gPageRotationActive || gPageRotateInterval != interval) {
    gPageRotationActive = true;
    gPageRotateInterval = interval;
    gPageRotateAt = now + interval;
    return;
  }
  if ((int32_t)(now - gPageRotateAt) < 0) return;
  cycleMetricPage(1);
  gPageRotateAt = millis() + interval;
}
