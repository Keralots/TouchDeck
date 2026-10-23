#include "haptic.h"
#include "settings.h"
#include "led.h"
#include <esp_timer.h>

// Above hearing range, so a partial duty does not whine through the motor.
static constexpr uint32_t HAPTIC_PWM_FREQ = 20000;
static constexpr uint8_t HAPTIC_PWM_RES = 8;

static int8_t attachedPin = -1;
static esp_timer_handle_t offTimer = nullptr;

static void parkLow(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

// pinMode() takes the pin back from LEDC, so parking also detaches.
static void detachPin() {
  if (offTimer) esp_timer_stop(offTimer);
  if (attachedPin >= 0) {
    ledcWrite((uint8_t)attachedPin, 0);
    ledcDetach((uint8_t)attachedPin);
    parkLow((uint8_t)attachedPin);
  }
  attachedPin = -1;
}

static bool attachPin(uint8_t pin) {
  if (attachedPin == (int8_t)pin) return true;
  detachPin();
  if (!ledcAttach(pin, HAPTIC_PWM_FREQ, HAPTIC_PWM_RES)) {
    Serial.printf("Haptic: could not attach PWM to GPIO %u\n", (unsigned)pin);
    return false;
  }
  ledcWrite(pin, 0);
  attachedPin = (int8_t)pin;
  return true;
}

static void pulseOff(void*) {
  const int8_t pin = attachedPin;
  if (pin >= 0) ledcWrite((uint8_t)pin, 0);
}

static void pulse(uint8_t pin, uint8_t strength, uint8_t pulseMs) {
  if (!offTimer) {
    esp_timer_create_args_t args = {};
    args.callback = pulseOff;
    args.name = "haptic";
    if (esp_timer_create(&args, &offTimer) != ESP_OK) return;
  }
  if (!attachPin(pin)) return;
  esp_timer_stop(offTimer);  // a tap during a pulse restarts it
  ledcWrite(pin, (uint32_t)strength * 255 / 100);
  esp_timer_start_once(offTimer, (uint64_t)pulseMs * 1000);
}

bool hapticPinAllowed(uint8_t pin) {
  if (!ledPinAllowed(pin)) return false;
  return !(ledSettings.enabled && ledSettings.pin == pin);
}

void sanitizeHapticPin() {
  hapticSettings.strength = constrain(hapticSettings.strength, HAPTIC_MIN_STRENGTH, 100);
  hapticSettings.pulseMs = constrain(hapticSettings.pulseMs, HAPTIC_MIN_MS, HAPTIC_MAX_MS);
  // A disabled pin is only a remembered choice; initHaptic() checks it before parking.
  if (hapticSettings.enabled && !hapticPinAllowed(hapticSettings.pin)) {
    Serial.printf("Haptic: GPIO %u is not allowed, disabling\n", (unsigned)hapticSettings.pin);
    hapticSettings.enabled = false;
  }
}

void initHaptic() {
  detachPin();
  const uint8_t pin = hapticSettings.pin;
  if (!hapticSettings.enabled) {
    // Hold a wired but disabled module's input low instead of floating.
    if (hapticPinAllowed(pin)) parkLow(pin);
    Serial.println("Haptic: disabled");
    return;
  }
  if (!hapticPinAllowed(pin)) {
    Serial.printf("Haptic: GPIO %u is not allowed on this board\n", (unsigned)pin);
    return;
  }
  if (attachPin(pin))
    Serial.printf("Haptic: GPIO %u, %u%%, %u ms\n", (unsigned)pin,
                  (unsigned)hapticSettings.strength, (unsigned)hapticSettings.pulseMs);
}

void hapticTap() {
  if (!hapticSettings.enabled || !hapticPinAllowed(hapticSettings.pin)) return;
  pulse(hapticSettings.pin, hapticSettings.strength, hapticSettings.pulseMs);
}

bool hapticTest(uint8_t pin, uint8_t strength, uint8_t pulseMs) {
  if (!hapticPinAllowed(pin)) return false;
  pulse(pin, constrain(strength, HAPTIC_MIN_STRENGTH, 100),
        constrain(pulseMs, HAPTIC_MIN_MS, HAPTIC_MAX_MS));
  return true;
}
