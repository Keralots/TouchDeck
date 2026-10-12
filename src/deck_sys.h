// System screens (splash, WiFi setup, connecting, connected, firmware update)
// drawn by LVGL on the system layer, over the deck pages. Loop task only.
#ifndef DECK_SYS_H
#define DECK_SYS_H

#include <Arduino.h>
#include "display_ui.h"

namespace deck {

// Show the screen for this state (SCREEN_DECK hides it). Cheap when nothing
// changed; called from every deckTick().
void sysScreenSync(ScreenState state);
// Rebuild the screen that is up (new logical size, or a fresh OTA attempt).
void sysScreenRelayout();
// Firmware update screen. total 0 = size unknown.
void sysOtaProgress(uint32_t done, uint32_t total);
void sysOtaResult(bool ok, const char* message);

}  // namespace deck

#endif  // DECK_SYS_H
