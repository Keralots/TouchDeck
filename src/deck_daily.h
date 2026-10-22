// Daily wallpapers: random Windows Spotlight pictures fetched into their own
// gallery (/wall/dl, /wall/dp), scaled and re-encoded on the device.
#ifndef DECK_DAILY_H
#define DECK_DAILY_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace deck {

// Starts a background fetch of up to `landscape` + `portrait` new pictures.
// False when a fetch is already running or the task cannot start.
bool dailyStart(int landscape, int portrait);
bool dailyRunning();
void dailyStatus(JsonObject out);
// Loop task: tells the wallpaper gallery about finished fetches.
void dailyPoll();

}  // namespace deck

#endif  // DECK_DAILY_H
