#include "led.h"
#include "config.h"
#include "settings.h"
#include "display_ui.h"
#include "pc_metrics.h"

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
static bool    previewActive     = false;
static uint8_t previewBrightness = 0;

// ---------------------------------------------------------------------------
//  Pin deny-list
// ---------------------------------------------------------------------------
// Ported from BambuHelper so the two projects accept the same wiring. The job
// of this list is to refuse a pin that would kill the display, USB or boot -
// it deliberately does not try to model which pins a given board physically
// breaks out, because the user knows their own wiring and picks from it.
bool ledPinAllowed(uint8_t pin) {
  // GPIO 0 is the "unset" sentinel (LED_DEFAULT_PIN) and a strapping pin on
  // every board here, so it is never a valid choice.
  if (pin == 0) return false;
  if (pin == BACKLIGHT_PIN) return false;
  // Dynamic conflict: the capacitive pad owns its pin while it is enabled.
  if (touchSettings.enabled && pin == touchSettings.pin) return false;

#if defined(BOARD_IS_S3_ZERO)
  // Waveshare ESP32-S3-Zero + external ST7789 240x240
  if (pin >= 8 && pin <= 12) return false;    // display SPI
  if (pin == 13) return false;                // display backlight
  if (pin == 19 || pin == 20) return false;   // USB CDC D-/D+
  if (pin == 21) return false;                // onboard WS2812 RGB LED
  if (pin >= 26 && pin <= 37) return false;   // SPI flash + PSRAM / not exposed
  if (pin > 48) return false;

#elif defined(BOARD_IS_WS154)
  // Waveshare ESP32-S3-Touch-LCD-1.54"
  if (pin == 21 || pin == 38 || pin == 39 || pin == 40 || pin == 45) return false; // display SPI
  if (pin == 41 || pin == 42 || pin == 47 || pin == 48) return false; // CST816 I2C + RST/IRQ
  if (pin == 8 || pin == 9 || pin == 10 || pin == 12) return false;   // ES8311 I2S
  if (pin == 7) return false;                 // audio PA control
  if (pin == 2) return false;                 // BAT_EN
  if (pin == 4 || pin == 5) return false;     // board buttons (0 is already out)
  if (pin == 19 || pin == 20) return false;   // USB CDC D-/D+
  if (pin >= 26 && pin <= 37) return false;   // SPI flash + PSRAM
  if (pin > 48) return false;

#elif defined(BOARD_IS_JC3248W535)
  // Guition JC3248W535 (AXS15231B QSPI 320x480)
  if (pin == 21 || pin == 39 || pin == 40 ||
      pin == 45 || pin == 47 || pin == 48) return false;  // display QSPI
  if (pin == 38) return false;                // display TE
  if (pin == 4 || pin == 8) return false;     // touch I2C
  if (pin == 3) return false;                 // touch INT
  if (pin == 5) return false;                 // battery sense ADC
  if (pin == 2 || pin == 41 || pin == 42) return false;   // NS4168 I2S audio
  if (pin == 19 || pin == 20) return false;   // USB CDC D-/D+
  if (pin >= 26 && pin <= 37) return false;   // SPI flash + PSRAM (qio_opi)
  if (pin > 48) return false;

#elif defined(BOARD_IS_S3)
  // LOLIN S3 mini + external ST7789 240x240. The pre-wired units put the LED
  // on GPIO 2, the pad on 4 and the buzzer on 5, all of which pass here.
  if (pin >= 8 && pin <= 12) return false;    // display SPI (backlight 13 is above)
  if (pin == 19 || pin == 20) return false;   // USB CDC D-/D+
  if (pin == 43 || pin == 44) return false;   // UART0
  if (pin == 45 || pin == 46) return false;   // strapping / VDD_SPI
  if (pin >= 26 && pin <= 37) return false;   // SPI flash + PSRAM (qio_qspi)
  if (pin > 48) return false;

#elif defined(BOARD_IS_C3)
  // LOLIN C3 mini + external ST7789 240x240
  if (pin == 6 || pin == 7 || pin == 10 ||
      pin == 20 || pin == 21) return false;   // display SPI
  if (pin == 18 || pin == 19) return false;   // USB CDC D-/D+
  if (pin >= 11 && pin <= 17) return false;   // flash
  if (pin == 8 || pin == 9) return false;     // strapping
  if (pin > 21) return false;
#endif

  return true;
}

bool ledPinValid() {
  return ledSettings.enabled && ledPinAllowed(ledSettings.pin);
}

void sanitizeLedPin() {
  ledSettings.enabled = ledSettings.enabled ? 1 : 0;
  ledSettings.nightEnabled = ledSettings.nightEnabled ? 1 : 0;
  ledSettings.followDisplay = ledSettings.followDisplay ? 1 : 0;
  ledSettings.offlineOff = ledSettings.offlineOff ? 1 : 0;
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

  uint8_t duty = (ledSettings.nightEnabled && nightBrightnessActive())
                   ? ledSettings.nightBrightness
                   : ledSettings.brightness;
  // One test covers manual off, the PC-offline sleep timer and a night
  // brightness of 0: whatever put the panel out, the LED goes with it.
  if (ledSettings.followDisplay && currentBacklightLevel() == 0) duty = 0;
  if (ledSettings.offlineOff && !pcData.online) duty = 0;
  writeDuty(duty);
}
