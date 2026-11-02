#include "led.h"
#include "config.h"
#include "settings.h"
#include "display_ui.h"
#include "pc_metrics.h"
#include "board.h"

// Which GPIO currently carries the PWM signal, or -1 when nothing is attached.
// Kept separate from ledSettings.pin so a configuration change detaches the
// OLD pin before claiming the new one.
static int8_t  attachedPin     = -1;
static int16_t lastWrittenDuty = -1;   // -1 = unknown, forces the first write
static uint32_t lastTickMs     = 0;

// Live preview from the portal. While set, the tick uses previewBrightness and
// ignores the schedule, so the 60 Hz tick does not fight a slider drag with the
// stale saved value. Cleared by initLed() (which the save handler calls) and by
// shutdownLed().
static int16_t haLevel           = LED_HA_NONE;
static bool    previewActive     = false;
static uint8_t previewBrightness = 0;

// ---------------------------------------------------------------------------
//  Pin deny-list
// ---------------------------------------------------------------------------
// Refuses a pin that would kill the display, touch, USB or boot (the board
// header's list); it does not model which pins the board breaks out.
bool ledPinAllowed(uint8_t pin, bool sdCard) {
  // GPIO 0 is the "unset" sentinel (LED_DEFAULT_PIN), never a valid choice.
  if (pin == 0) return false;
  if (boardPinReserved(pin, sdCard)) return false;
  // Pins the chip does not have (the S3 has no GPIO 22..25).
  if (pin > GPIO_PIN_MAX || !((SOC_GPIO_VALID_OUTPUT_GPIO_MASK >> pin) & 1)) return false;
  return true;
}

bool ledPinAllowed(uint8_t pin) { return ledPinAllowed(pin, dispSettings.sdCard); }

bool ledPinValid() {
  return ledSettings.enabled && ledPinAllowed(ledSettings.pin);
}

void sanitizeLedPin() {
  ledSettings.enabled = ledSettings.enabled ? 1 : 0;
  ledSettings.nightEnabled = ledSettings.nightEnabled ? 1 : 0;
  ledSettings.followDisplay = ledSettings.followDisplay ? 1 : 0;
  ledSettings.offlineOff = ledSettings.offlineOff ? 1 : 0;
  if (ledSettings.nightStartMinute >= 24 * 60) ledSettings.nightStartMinute = 22 * 60;
  if (ledSettings.nightEndMinute >= 24 * 60) ledSettings.nightEndMinute = 7 * 60;
  // Nothing is driven while the LED is off, so a stored pin is just a
  // remembered choice - policing it there would silently erase what the user
  // typed before they ticked the box. initLed() guards the park-LOW path with
  // its own ledPinAllowed() check, so an unvalidated pin can never be poked.
  if (!ledSettings.enabled) return;
  if (!ledPinAllowed(ledSettings.pin)) {
    Serial.printf("LED: GPIO %u is not allowed on this board, disabling\n",
                  (unsigned)ledSettings.pin);
    ledSettings.enabled = 0;
    ledSettings.pin = LED_DEFAULT_PIN;
  }
}

// ---------------------------------------------------------------------------
//  Duty output
// ---------------------------------------------------------------------------
static void writeDuty(uint8_t duty) {
  if (attachedPin < 0) return;
  if ((int16_t)duty == lastWrittenDuty) return;
  ledcWrite((uint8_t)attachedPin, duty);
  lastWrittenDuty = duty;
}

// Park the pin as a driven LOW output rather than leaving it high-Z, so the
// transistor gate is held off by firmware and does not depend on an external
// pulldown. pinMode() re-takes the pin from the LEDC peripheral, so this also
// serves as the detach.
static void detachAndForceLow() {
  if (attachedPin >= 0) {
    ledcWrite((uint8_t)attachedPin, 0);
    ledcDetach((uint8_t)attachedPin);
    pinMode(attachedPin, OUTPUT);
    digitalWrite(attachedPin, LOW);
  }
  attachedPin = -1;
  lastWrittenDuty = -1;
}

static bool attachPin(uint8_t pin) {
  if (attachedPin == (int8_t)pin) return true;
  detachAndForceLow();
  if (!ledcAttach(pin, LED_PWM_FREQ, LED_PWM_RES)) {
    Serial.printf("LED: could not attach PWM to GPIO %u\n", (unsigned)pin);
    return false;
  }
  attachedPin = (int8_t)pin;
  lastWrittenDuty = -1;
  return true;
}

uint8_t ledCurrentDuty() {
  return (lastWrittenDuty < 0) ? 0 : (uint8_t)lastWrittenDuty;
}

void ledSetHaLevel(int16_t level) { haLevel = level; }
int16_t ledHaLevel() { return haLevel; }
uint8_t ledDayLevel() { return haLevel >= 0 ? (uint8_t)haLevel : ledSettings.brightness; }

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
void initLed() {
  previewActive = false;

  if (!ledSettings.enabled) {
    // A saved-but-disabled pin still gets parked LOW - but only if it is a pin
    // we are allowed to touch, so a stale setting can never poke the display
    // bus or USB.
    const uint8_t saved = ledSettings.pin;
    detachAndForceLow();
    if (saved != LED_DEFAULT_PIN && ledPinAllowed(saved)) {
      pinMode(saved, OUTPUT);
      digitalWrite(saved, LOW);
    }
    Serial.println("LED: disabled");
    return;
  }
  if (!ledPinAllowed(ledSettings.pin)) {
    Serial.printf("LED: GPIO %u is not allowed on this board\n",
                  (unsigned)ledSettings.pin);
    detachAndForceLow();
    return;
  }
  if (!attachPin(ledSettings.pin)) return;
  writeDuty(ledSettings.brightness);
  Serial.printf("LED: GPIO %u at brightness %u\n",
                (unsigned)ledSettings.pin, (unsigned)ledSettings.brightness);
}

void shutdownLed() {
  previewActive = false;
  detachAndForceLow();
}

void previewLed(bool enabled, uint8_t pin, uint8_t brightness) {
  if (!enabled || !ledPinAllowed(pin)) {
    previewActive = false;
    detachAndForceLow();
    return;
  }
  if (!attachPin(pin)) {
    previewActive = false;
    return;
  }
  previewActive = true;
  previewBrightness = brightness;
  writeDuty(brightness);
}

// Unconditional re-init, not "only if a preview is running": previewing the
// LED as DISABLED clears previewActive on the way in, and an early return here
// would then leave the LED dark against a saved config that says it is on.
void clearLedPreview() {
  previewActive = false;
  initLed();   // back to the saved configuration
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void ledTick() {
  if (attachedPin < 0) return;
  const uint32_t now = millis();
  if (now - lastTickMs < LED_TICK_MIN_INTERVAL_MS) return;
  lastTickMs = now;

  if (previewActive) {
    writeDuty(previewBrightness);
    return;
  }
  if (!ledSettings.enabled) return;

  uint8_t duty = ledNightActive() ? ledSettings.nightBrightness : ledDayLevel();
  if (haLevel == 0) duty = 0;  // switched off from HA: off at night too
  // One test covers manual off and a night brightness of 0: whatever put the
  // panel out, the LED goes with it.
  if (ledSettings.followDisplay && currentBacklightLevel() == 0) duty = 0;
  if (ledSettings.offlineOff && !pcAnyOnline()) duty = 0;
  writeDuty(duty);
}
