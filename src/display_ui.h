// Display state and backlight. The screen itself is LVGL (deck_ui); this
// layer says which screen is up and how bright the panel is.
#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>

// SCREEN_DECK is the deck pages; every other state is a full-screen system
// screen drawn over them (deck_sys.cpp).
enum ScreenState {
  SCREEN_SPLASH,
  SCREEN_AP_MODE,
  SCREEN_CONNECTING_WIFI,
  SCREEN_WIFI_CONNECTED,
  SCREEN_DECK,
  SCREEN_OTA_UPDATE
};

void initDisplay();
void setScreenState(ScreenState state);
ScreenState getScreenState();
// Once per loop pass, and from any blocking loop that keeps the screen alive.
void displayTick();
// Rotation changed: re-layout everything at the new logical size.
void applyDisplaySettings();

void setBacklight(uint8_t level);
// Recompute the backlight from the night schedule and the manual states.
// Call updateBacklightControl() from the main loop.
void updateBacklightControl();
void refreshBacklightControl();
uint8_t currentBacklightLevel();
bool nightBrightnessActive();
// Inside the LED's own night interval (and dimming enabled); manual night does not count.
bool ledNightActive();
bool backlightTimeValid();
void setManualDisplayOff(bool off);
bool manualDisplayOff();
// Night level on demand (quick panel), on top of the schedule. Not persisted.
void setManualNight(bool on);
bool manualNightActive();
// Idle sleep (deck.json "presence"): backlight capped at level (0 = off), faded.
void setIdleSleep(bool sleep, uint8_t level);
bool idleSleepActive();
// Manual off or idle sleep: the next touch only wakes the screen.
bool displaySleepingForTouch();
// Clears manual off and idle sleep.
void wakeDisplayFromTouch();

#endif // DISPLAY_UI_H
