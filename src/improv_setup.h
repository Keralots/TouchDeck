#pragma once

#include <Arduino.h>

// Non-blocking Improv-Serial setup window.
//
// improvSetupBegin() arms a Serial listener for `windowMs` milliseconds. The
// listener runs cooperatively as the caller pumps improvSetupTick() from its
// own loop, so the AP captive portal can stay up in parallel - legacy users
// (and users who flashed via the web flasher but didn't catch the WiFi
// dialog in time) keep AP access without any extra wait.
//
// Typical lifecycle from wifi_manager:
//   startAP();
//   if (no creds) improvSetupBegin(IMPROV_SETUP_WINDOW_MS);
//   // ... and in handleWiFi() while in AP mode:
//   if (improvSetupTick()) { ESP.restart(); }    // creds saved by callback
//   if (improvSetupExpired()) improvSetupEnd();

void improvSetupBegin(uint32_t windowMs);

// Pumps the Improv parser. Returns true exactly once - when valid WiFi
// credentials have been received AND a STA connection succeeded. The
// credentials are already persisted to NVS at that point; the caller is
// expected to ESP.restart() so the device boots cleanly into STA mode.
bool improvSetupTick();

// True once the setup window has elapsed without success. Callers should
// then improvSetupEnd() to release resources; AP stays up regardless.
bool improvSetupExpired();
// True while the setup window listens for Improv on Serial.
bool improvSetupActive();

void improvSetupEnd();
