#include "display_ui.h"
#include "config.h"
#include "settings.h"
#include "deck_ui.h"
#include <time.h>

static ScreenState s_screen = SCREEN_SPLASH;

ScreenState getScreenState() { return s_screen; }
void setScreenState(ScreenState state) { s_screen = state; }
void displayTick() { deckTick(); }

// ---------------------------------------------------------------------------
//  Backlight
// ---------------------------------------------------------------------------
static uint8_t lastAppliedBrightness = 0;
static bool g_night_active = false;
static bool g_led_night = false;
static bool g_time_valid = false;
static bool g_backlight_refresh = true;
static bool g_manual_display_off = false;
static bool g_manual_night = false;
static uint32_t g_last_backlight_check_ms = 0;

void setBacklight(uint8_t level) {
  analogWrite(BACKLIGHT_PIN, level);
  lastAppliedBrightness = level;
}

uint8_t currentBacklightLevel() { return lastAppliedBrightness; }
bool nightBrightnessActive() { return g_night_active; }
bool ledNightActive() { return g_led_night; }
bool backlightTimeValid() { return g_time_valid; }
bool manualDisplayOff() { return g_manual_display_off; }
bool manualNightActive() { return g_manual_night; }
bool displaySleepingForTouch() { return g_manual_display_off; }

void setManualNight(bool on) {
  g_manual_night = on;
  refreshBacklightControl();
}

void setManualDisplayOff(bool off) {
  g_manual_display_off = off;
  refreshBacklightControl();
}

void wakeDisplayFromTouch() {
  g_manual_display_off = false;
  refreshBacklightControl();
}

void refreshBacklightControl() { g_backlight_refresh = true; }

// start == end is no interval; start > end wraps past midnight.
static bool inNightWindow(uint16_t minute, uint16_t start, uint16_t end) {
  if (start < end) return minute >= start && minute < end;
  if (start > end) return minute >= start || minute < end;
  return false;
}

void updateBacklightControl() {
  const uint32_t nowMs = millis();
  if (!g_backlight_refresh && nowMs - g_last_backlight_check_ms < 1000) return;
  g_backlight_refresh = false;
  g_last_backlight_check_ms = nowMs;

  const time_t epoch = time(nullptr);
  g_time_valid = epoch >= 1609459200;  // 2021-01-01, safely past an unsynced epoch
  bool nightWindow = false;
  g_led_night = false;
  if (g_time_valid) {
    struct tm localTime;
    localtime_r(&epoch, &localTime);
    const uint16_t minute = localTime.tm_hour * 60 + localTime.tm_min;
    if (backlightSettings.nightEnabled)
      nightWindow = inNightWindow(minute, backlightSettings.nightStartMinute,
                                  backlightSettings.nightEndMinute);
    g_led_night = ledSettings.nightEnabled &&
                  inNightWindow(minute, ledSettings.nightStartMinute, ledSettings.nightEndMinute);
  }
  g_night_active = nightWindow || g_manual_night;

  uint8_t target = g_night_active ? backlightSettings.nightBrightness : brightness;
  if (g_manual_display_off) target = 0;
  if (target != lastAppliedBrightness) setBacklight(target);
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------
void initDisplay() {
  // Panel bring-up wants the rails settled after power-on.
  delay(500);
  if (!deckBegin(dispSettings.rotation)) Serial.println("Display: deck pipeline init FAILED");
  displayTick();  // the splash is on the panel before the light comes on
  pinMode(BACKLIGHT_PIN, OUTPUT);
  setBacklight(brightness);
}

void applyDisplaySettings() {
  deckApplyTheme(dispSettings.theme);
  deckApplyRotation(dispSettings.rotation);
}
