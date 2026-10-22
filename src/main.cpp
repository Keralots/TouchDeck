// TouchDeck - firmware entry point.
// Boot: settings -> display (splash) -> WiFi (STA or AP setup) -> web/OTA
// server -> UDP metric listener. Home Assistant tiles and PC metric tiles live
// on the deck pages (deck_ui); the PC companion pushes V2.1 JSON to UDP :4210.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "display_ui.h"
#include "settings.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "pc_metrics.h"
#include "led.h"
#include "deck_mqtt.h"
#include "deck_daily.h"
#include "deck_sd.h"

// Loop-task watchdog: an indefinite block in loop() panics after
// WDT_TIMEOUT_S with a backtrace and reboots (/api/status then reports
// reset_reason "task_wdt"). Every legitimate blocking path finishes far
// below this bound.
static const uint32_t WDT_TIMEOUT_S = 30;

// LVGL rendering and the Glass bake left under 1 KB of the default 8 KB.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() {
  // Companion metrics arrive over the cable as ~1 KB lines; HWCDC's default is 256.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PRODUCT_NAME " " FW_VERSION " boot");
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

  loadSettings();
  deck::wallStorageBegin();
  setScreenState(SCREEN_SPLASH);
  initDisplay();

  startWiFiDuringSplash();        // kick off a background STA connect
  unsigned long splashStart = millis();
  while (millis() - splashStart < 800) {
    displayTick();
    delay(20);
  }

  initWiFi();                     // blocks until STA connects or AP comes up
  initWebServer();
  pcMetricsBegin();
  initLed();
  mqttBegin();

  if (isWiFiConnected()) setScreenState(SCREEN_DECK);

  // Subscribe only after the blocking boot phases; loop() feeds it every pass.
  // The Arduino core usually arrives with the TWDT already initialized, so
  // fall back to reconfigure when init reports that state.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_init(&wdtConfig) != ESP_OK)
    esp_task_wdt_reconfigure(&wdtConfig);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  handleWiFi();
  handleWebServer();
  pcMetricsHandle();

  // Finish boot/connect transitions.
  ScreenState s = getScreenState();
  if (isWiFiConnected() &&
      (s == SCREEN_SPLASH || s == SCREEN_CONNECTING_WIFI ||
       s == SCREEN_WIFI_CONNECTED)) {
    setScreenState(SCREEN_DECK);
  }

  updateBacklightControl();
  // After the backlight: the LED reads the level the panel just settled on.
  ledTick();
  displayTick();
  deck::dailyPoll();
}
