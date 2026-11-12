// WiFi co-processor (ESP32-C6 over ESP-Hosted) firmware update: the image
// matching this build's ESP-Hosted version is downloaded from Espressif's
// arduino-esp32 site and written over the hosted link, then the deck restarts.
#ifndef COPROC_UPDATE_H
#define COPROC_UPDATE_H

#include <Arduino.h>

// Starts the download and write in a background task. False with a reason
// when the board has no co-processor or an update already runs.
bool coprocUpdateStart(const char** why);
// "idle", "running", "done" or "error", plus progress 0..100 and the last error.
const char* coprocUpdateState();
int coprocUpdateProgress();
const char* coprocUpdateError();

#endif  // COPROC_UPDATE_H
