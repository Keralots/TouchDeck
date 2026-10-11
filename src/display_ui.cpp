// Slim display layer for StatNode: panel bring-up and backlight. On the
// JC3248W535 the panel belongs to the deck pipeline (deck_ui / deck_panel). All the BambuHelper printer/AMS rendering
// is gone - the gauge grid is drawn by the renderer (see renderer.cpp / main).
#include "display_ui.h"
#include "config.h"
#include "layout.h"
#include "settings.h"
#include "pc_metrics.h"
#include "fonts.h"
#include <time.h>
#include <new>   // placement new for CYD panel variant selection

// The LGFX board device classes + the per-board `_tft_instance` live in their
// own header. Included exactly once, here. See src/lgfx_boards.h.
#include "lgfx_boards.h"
#if defined(BOARD_IS_JC3248W535)
#include "deck_ui.h"
#endif

// Global pointer - accessed via the `tft` macro throughout the codebase.
lgfx::LovyanGFX* tft_ptr = &_tft_instance;

void markFrameDirty() {
#if defined(BOARD_IS_JC3248W535)
  deckFaceDirty();
#endif
}

void flushFrame() {
#if defined(BOARD_IS_JC3248W535)
  deckTick();
#endif
}

static uint8_t sanitizeRotation(uint8_t r) { return r; }

// Use user-configured bg color instead of the hardcoded CLR_BG macro.
#undef  CLR_BG
#define CLR_BG  (dispSettings.bgColor)

// ---------------------------------------------------------------------------
//  Backlight
// ---------------------------------------------------------------------------
static uint8_t lastAppliedBrightness = 0;
static bool g_night_active = false;
static bool g_offline_sleeping = false;
static bool g_time_valid = false;
static bool g_offline_tracking = false;
static bool g_backlight_refresh = true;
static bool g_manual_display_off = false;
static uint32_t g_offline_since_ms = 0;
static uint32_t g_last_backlight_check_ms = 0;
static uint32_t g_touch_wake_until_ms = 0;

void setBacklight(uint8_t level) {
#if defined(BACKLIGHT_PIN) && BACKLIGHT_PIN >= 0
  analogWrite(BACKLIGHT_PIN, level);
#endif
  lastAppliedBrightness = level;
}

uint8_t currentBacklightLevel() { return lastAppliedBrightness; }
bool nightBrightnessActive() { return g_night_active; }
bool offlineDisplaySleeping() { return g_offline_sleeping; }
bool backlightTimeValid() { return g_time_valid; }
bool manualDisplayOff() { return g_manual_display_off; }
bool displaySleepingForTouch() {
  return g_manual_display_off || g_offline_sleeping;
}

void setManualDisplayOff(bool off) {
  g_manual_display_off = off;
  if (off) g_touch_wake_until_ms = 0;
  refreshBacklightControl();
}

void wakeDisplayFromTouch() {
  g_manual_display_off = false;
  g_touch_wake_until_ms = millis() + TOUCH_WAKE_MS;
  refreshBacklightControl();
}

void refreshBacklightControl() {
  g_backlight_refresh = true;
}

void updateBacklightControl() {
  const uint32_t nowMs = millis();
  if (!g_backlight_refresh && nowMs - g_last_backlight_check_ms < 1000) return;
  g_backlight_refresh = false;
  g_last_backlight_check_ms = nowMs;

  const bool touchWakeActive = g_touch_wake_until_ms != 0 &&
    (int32_t)(g_touch_wake_until_ms - nowMs) > 0;
  if (!touchWakeActive) g_touch_wake_until_ms = 0;

  // Night-interval membership is needed by both the dimming schedule and the
  // offline screen-off option, so it is computed first. Dimming stays gated on
  // its own enable flag; the off option only needs the interval itself.
  const time_t epoch = time(nullptr);
  g_time_valid = epoch >= 1609459200;  // 2021-01-01, safely past an unsynced epoch
  bool nightWindow = false;
  if (g_time_valid &&
      (backlightSettings.nightEnabled || backlightSettings.nightOfflineOff)) {
    struct tm localTime;
    localtime_r(&epoch, &localTime);
    const uint16_t minute = localTime.tm_hour * 60 + localTime.tm_min;
    const uint16_t start = backlightSettings.nightStartMinute;
    const uint16_t end = backlightSettings.nightEndMinute;
    if (start < end) nightWindow = minute >= start && minute < end;
    else if (start > end) nightWindow = minute >= start || minute < end;
  }
  g_night_active = backlightSettings.nightEnabled && nightWindow;

  const ScreenState screen = getScreenState();
  bool sleepEligible = screen == SCREEN_MONITOR || screen == SCREEN_CLOCK;
#if defined(BOARD_IS_JC3248W535)
  // A deck page is in use whether or not the PC is online.
  if (!deckFacePageActive()) sleepEligible = false;
#endif
  if (!sleepEligible || pcData.online) {
    g_offline_tracking = false;
    g_offline_sleeping = false;
  } else {
    if (!g_offline_tracking) {
      g_offline_tracking = true;
      g_offline_since_ms = nowMs;
    }
    const uint32_t delayMs =
      (uint32_t)backlightSettings.offlineSleepMinutes * 60000UL;
    const bool sleepDue =
      (delayMs > 0 && nowMs - g_offline_since_ms >= delayMs) ||
      (backlightSettings.nightOfflineOff && nightWindow);
    g_offline_sleeping = sleepDue && !touchWakeActive;
  }

  uint8_t target = g_night_active ? backlightSettings.nightBrightness : brightness;
  if (g_offline_sleeping) target = 0;
  if (g_manual_display_off) target = 0;
  if (target != lastAppliedBrightness) setBacklight(target);
}

#if defined(DISPLAY_CYD)
static void applyCydPanelInversion() {
  _tft_instance.invertDisplay(dispSettings.invertColors);
}
#endif

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------
void initDisplay() {
  Serial.println("Display: pre-init delay...");
  delay(500);

#if defined(DISPLAY_CYD)
  // Pick CYD panel variant based on loaded settings. Default static-init
  // already constructed V2; swap to Classic if the user selected it.
  if (dispSettings.cydPanelClassic) {
    _tft_storage.v2.~LGFX_CYD_V2();
    new (&_tft_storage.classic) LGFX_CYD_Classic();
    Serial.println("Display: CYD panel variant = Classic (Panel_ILI9341)");
  } else {
    Serial.println("Display: CYD panel variant = V2 (Panel_ILI9341_2)");
  }
#endif

#if defined(BOARD_IS_JC3248W535)
  // The deck pipeline owns the panel (deck_panel.cpp) and shows the faces
  // through LVGL. `_tft_instance` is the face sprite the renderer draws into;
  // deckBegin() sizes it to the logical resolution of the rotation.
  if (!deckBegin(&_tft_instance, sanitizeRotation(dispSettings.rotation)))
    Serial.println("Display: deck pipeline init FAILED");
  tft.fillScreen(CLR_BG);
#else
  Serial.println("Display: calling _tft_instance.init()...");
  _tft_instance.init();
#if defined(DISPLAY_CYD)
  applyCydPanelInversion();
#elif defined(USE_ST7789_INVERT)
  _tft_instance.invertDisplay(true);  // ST7789 panels need color inversion
#endif
  Serial.println("Display: tft.init() done");

#if defined(DISPLAY_240x320)
  // Clear entire GRAM at rotation 0 first so rotations 1/3 don't leave 80px of
  // uninitialized VRAM visible as garbage on the extra screen edge.
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
#endif

  tft.setRotation(dispSettings.rotation);

#if defined(DISPLAY_CYD)
  applyCydPanelInversion();
#elif defined(DISPLAY_240x320)
  if (dispSettings.invertColors) _tft_instance.invertDisplay(false);
#endif
  tft.fillScreen(CLR_BG);
#endif

#if defined(BACKLIGHT_PIN) && BACKLIGHT_PIN >= 0
  pinMode(BACKLIGHT_PIN, OUTPUT);
  setBacklight(brightness);
#endif

  // Splash screen - center on the active canvas.
  {
    const int16_t sw = (int16_t)tft.width();
    const int16_t sh = (int16_t)tft.height();
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(CLR_GREEN, CLR_BG);
    setFont(tft, FONT_LARGE);
    tft.drawString(PRODUCT_NAME, sw / 2, sh / 2 - 20);
    setFont(tft, FONT_BODY);
    tft.setTextColor(CLR_TEXT_DIM, CLR_BG);
    tft.drawString("StatNode", sw / 2, sh / 2 + 10);
    setFont(tft, FONT_SMALL);
    tft.drawString(FW_VERSION, sw / 2, sh / 2 + 30);
  }
#if defined(BOARD_IS_JC3248W535)
  markFrameDirty();
  flushFrame();
#endif
}

void applyDisplaySettings() {
#if defined(BOARD_IS_JC3248W535)
  // Rotation is a logical-size change for the deck: face sprite, LVGL and
  // pages are rebuilt at the new size, the panel itself never rotates.
  deckApplyRotation(sanitizeRotation(dispSettings.rotation));
  tft.fillScreen(dispSettings.bgColor);
  markFrameDirty();
  return;
#endif
#if defined(DISPLAY_240x320)
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
#endif
  tft.setRotation(sanitizeRotation(dispSettings.rotation));
#if defined(DISPLAY_CYD)
  applyCydPanelInversion();
#elif defined(DISPLAY_240x320)
  _tft_instance.invertDisplay(dispSettings.invertColors ? false : true);
#endif
  tft.fillScreen(dispSettings.bgColor);
  markFrameDirty();
}
