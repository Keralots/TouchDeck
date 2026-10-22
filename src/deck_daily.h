// Daily wallpapers: random Windows Spotlight pictures fetched once a day into
// their own gallery (/wall/dl, /wall/dp), scaled and re-encoded on the device.
#ifndef DECK_DAILY_H
#define DECK_DAILY_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace deck {

// Fetch now with the current settings. False when a fetch is already running
// or the task cannot start.
bool dailyStart();
bool dailyRunning();
// True while fetching; done/total count pictures.
bool dailyProgress(int& done, int& total);
// Loop task: keep the microSD archive within dailyMax after an upload.
void dailyTrim(bool portrait);
// Orientation or count changed: fetch again today.
void dailyReplan();
void dailyStatus(JsonObject out);
// Loop task: schedule, retries, gallery refresh.
void dailyPoll();

}  // namespace deck

#endif  // DECK_DAILY_H
