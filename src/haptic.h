#ifndef HAPTIC_H
#define HAPTIC_H

#include <Arduino.h>

// Vibration motor module (driver transistor on the module) on a GPIO: one short
// PWM pulse per tile tap on the panel. Uses the status LED's pin deny-list.

// initHaptic() re-reads hapticSettings; the save handler calls it.
void initHaptic();
// Pulse with the saved settings; does nothing while disabled.
void hapticTap();
// Portal Test button: one pulse from unsaved form values. False = pin refused.
bool hapticTest(uint8_t pin, uint8_t strength, uint8_t pulseMs);

// Board deny-list plus the pin the enabled status LED holds.
bool hapticPinAllowed(uint8_t pin);
// Runs on load and before every NVS write.
void sanitizeHapticPin();

#endif // HAPTIC_H
