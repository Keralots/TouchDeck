// The PIN that protects the portal and the settings on the device. Kept as a
// salted hash in its own NVS namespace, so a backup never carries it.
#pragma once
#include <Arduino.h>

// Wrong tries are counted per channel: one that is locked leaves the others usable.
enum PinChannel : uint8_t { PIN_LOGIN = 0, PIN_BASIC, PIN_TOUCH, PIN_CHANNELS };
enum PinResult : uint8_t { PIN_OK = 0, PIN_WRONG, PIN_LOCKED };

void pinInit();
bool pinIsSet();
bool pinValid(const String& pin);  // 4 to 8 digits
bool pinStore(const String& pin);
bool pinRemove();

// Counts a wrong try. lockedSecs = how long this channel stays locked, else 0.
PinResult pinTry(PinChannel ch, const char* pin, uint32_t& lockedSecs);

// USB recovery ("@TD {"pin":"clear"}"): asked on the serial task, done by the loop.
void pinRequestClear();
bool pinTakeClearRequest();
