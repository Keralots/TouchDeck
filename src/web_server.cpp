// StatNode web portal, JSON APIs, live screen capture, OTA update, and
// captive-portal redirects. Static UI assets are streamed from PROGMEM in
// bounded chunks so low-RAM boards never assemble the full page in heap.
#include "web_server.h"
#include "settings.h"
#include "wifi_manager.h"
#include "display_ui.h"
#include "pc_metrics.h"
#include "touch_button.h"
#include "led.h"
#include "fonts.h"
#include "config.h"
#include "web_pages.h"
#if defined(BOARD_IS_JC3248W535)
#include "deck_ui.h"
#include "deck_config.h"
#include "deck_ha.h"
#include "deck_web.h"
#include <LittleFS.h>
#endif
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "esp_ota_ops.h"
#include <WiFi.h>
#include <time.h>

static WebServer server(80);

// Deferred restart (so the HTTP response flushes before reboot).
static unsigned long restartAt = 0;
static bool eraseWifiAtRestart = false;
static void scheduleRestart(unsigned long delayMs, bool eraseWifi = false) {
  restartAt = millis() + delayMs;
  eraseWifiAtRestart = eraseWifi;
}

// OTA state.
static bool   otaInProgress = false;
static bool   otaFirstChunk = true;
static String otaError = "";

bool        isOtaAutoInProgress() { return otaInProgress; }
int         getOtaAutoProgress()  { return 0; }
const char* getOtaAutoStatus()    { return otaError.c_str(); }

// ---------------------------------------------------------------------------
//  Portal assets and JSON helpers
// ---------------------------------------------------------------------------
static bool writeClientAll(WiFiClient& client, const uint8_t* data, size_t len) {
  unsigned long lastProgress = millis();
  while (len > 0 && client.connected()) {
    size_t sent = client.write(data, len);
    if (sent > 0) {
      data += sent;
      len -= sent;
      lastProgress = millis();
    } else {
      if (millis() - lastProgress > 4000) return false;
      delay(1);
    }
  }
  return len == 0;
}

static void streamAsset(const char* data, size_t len, const char* contentType,
                        const char* cacheControl) {
  static const size_t CHUNK_SIZE = 2048;
  uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
  if (!chunk) {
    server.send(503, "text/plain", "Out of memory");
    return;
  }

  server.sendHeader("Cache-Control", cacheControl);
  server.setContentLength(len);
  server.send(200, contentType, "");
  WiFiClient client = server.client();

  bool ok = true;
  for (size_t offset = 0; offset < len && ok; offset += CHUNK_SIZE) {
    size_t count = min(CHUNK_SIZE, len - offset);
    memcpy_P(chunk, data + offset, count);
    ok = writeClientAll(client, chunk, count);
    esp_task_wdt_reset();   // large asset + slow client can outlast the WDT
    delay(0);
  }
  free(chunk);
  if (!ok) client.stop();
}

static void handleRoot() {
  streamAsset(PAGE_HTML, sizeof(PAGE_HTML) - 1, "text/html",
              "no-cache, no-store, must-revalidate");
}

static void handlePortalCss() {
  streamAsset(PORTAL_CSS, sizeof(PORTAL_CSS) - 1, "text/css",
              "public, max-age=31536000, immutable");
}

static void handlePortalLayoutCss() {
  streamAsset(PORTAL_LAYOUT_CSS, sizeof(PORTAL_LAYOUT_CSS) - 1, "text/css",
              "public, max-age=31536000, immutable");
}

static void handlePortalJs() {
  streamAsset(PORTAL_JS, sizeof(PORTAL_JS) - 1, "application/javascript",
              "public, max-age=31536000, immutable");
}

static void sendJsonMessage(int status, bool ok, const char* message,
                            bool restarting = false) {
  JsonDocument doc;
  doc["success"] = ok;
  doc["message"] = message;
  if (restarting) doc["restarting"] = true;
  String out;
  serializeJson(doc, out);
  server.send(status, "application/json", out);
}

static long clampedArg(const char* name, long current, long minValue, long maxValue) {
  if (!server.hasArg(name)) return current;
  long value = server.arg(name).toInt();
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
  return value;
}

// Read the whole face tuple from one request as a UNIT. Two independently
// validated axes would make order matter: checking a requested layout against
// the surface still in the globals rejects a legal move like Tiles+Aero to Big
// numbers, whose only legal surface is Flat. Read every raw value first, then
// normalise the requested pair together.
//
// A blank or out-of-range field means KEEP WHAT IS SET, which is what clampedArg
// already does. That rule exists because an empty select used to parse as 0 and
// silently threw the face back to the default - the old "style keeps resetting
// to Tiles" report.
static void faceArgs(uint8_t& layout, uint8_t& surface, uint8_t& bands,
                     uint8_t& rowStyle) {
  layout   = (uint8_t)clampedArg("layout", displayLayout, 0, LAYOUT_COUNT - 1);
  surface  = (uint8_t)clampedArg("surface", displaySurface, 0, SURFACE_COUNT - 1);
  bands    = (uint8_t)clampedArg("duoBands", duoHeroBands, 1, DUO_BANDS_MAX);
  rowStyle = (uint8_t)clampedArg("duoRows", duoRowStyle, 0, DUO_ROWS_LIST);
  normalizeFace(layout, surface, bands, rowStyle);
}

// Same "absent means keep" rule the face fields follow. Switching pages from
// here rather than from a dedicated endpoint keeps the portal's Display page the
// one place that changes what the panel shows.
static void applyMetricPageArg() {
  const uint8_t page =
    (uint8_t)clampedArg("page", activeMetricPage, 0, METRIC_PAGE_COUNT - 1);
  if (page == activeMetricPage) return;
  setActiveMetricPage(page);
  // Picking a page in the portal is as deliberate as tapping for it.
  pinMetricPage();
  saveStartMetricPage();
}

static void applyPageSettingsArgs() {
  // rememberPage is a checkbox, and an absent checkbox reads as false. Without
  // this guard a client posting only "page" or only a face field would silently
  // clear it, which is exactly how the night-mode flags got wiped during
  // testing. Only touch these when the request actually carries the block.
  if (!server.hasArg("pageRotate") && !server.hasArg("pagePin") &&
      !server.hasArg("pageRemember")) return;
  pageSettings.rotateSeconds =
    (uint8_t)clampedArg("pageRotate", pageSettings.rotateSeconds, 0, 255);
  pageSettings.rotateUnit =
    (uint8_t)clampedArg("pageRotateUnit", pageSettings.rotateUnit, 0, 1);
  pageSettings.pinSeconds =
    (uint8_t)clampedArg("pagePin", pageSettings.pinSeconds, 0, 240);
  pageSettings.rememberPage = server.hasArg("pageRemember") ? 1 : 0;
  if (pageSettings.rememberPage) pageSettings.startPage = activeMetricPage;
}

static void applyFaceArgs() {
  uint8_t layout, surface, bands, rowStyle;
  faceArgs(layout, surface, bands, rowStyle);
  displayLayout = layout;
  displaySurface = surface;
  duoHeroBands = bands;
  duoRowStyle = rowStyle;
}

static void addHtmlColor(JsonObject object, const char* key, uint16_t color) {
  char buf[8];
  rgb565ToHtml(color, buf);
  object[key] = buf;
}

static void formatMinuteOfDay(uint16_t minute, char* out, size_t outSize) {
  snprintf(out, outSize, "%02u:%02u", minute / 60, minute % 60);
}

static uint16_t minuteOfDayArg(const char* name, uint16_t current) {
  if (!server.hasArg(name)) return current;
  int hour = -1, minute = -1;
  if (sscanf(server.arg(name).c_str(), "%d:%d", &hour, &minute) != 2)
    return current;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return current;
  return (uint16_t)(hour * 60 + minute);
}

static void handleApiConfig() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"] = BOARD_VARIANT;
  doc["ip"] = WiFi.localIP().toString();
#if defined(DISPLAY_CYD)
  doc["isCyd"] = true;
#else
  doc["isCyd"] = false;
#endif
#if defined(BOARD_IS_JC3248W535)
  doc["deck"] = true;
#endif

  JsonObject display = doc["display"].to<JsonObject>();
  // The tuple is what the portal edits. "style" stays as the derived legacy
  // value so older tooling keeps reading something meaningful.
  display["layout"] = displayLayout;
  display["surface"] = displaySurface;
  display["duoBands"] = duoHeroBands;
  display["duoRows"] = duoRowStyle;
  display["faceName"] = faceNameFor(displayLayout, displaySurface, duoRowStyle);
  display["style"] = displayStyle;
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["smoothing"] = dispSettings.gaugeSmoothing;
  display["sparkSeconds"] = sparkRedrawSec;
  display["chartSmooth"] = chartSmoothing;
  display["glassTheme"] = glassTheme;
  display["glassGloss"] = glassGlossPct;
  display["glassBow"] = glassBowPct;
  display["glassChartFill"] = glassChartFillPct;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["smallLabels"] = dispSettings.smallLabels;
  display["invertColors"] = dispSettings.invertColors;
  display["cydClassic"] = dispSettings.cydPanelClassic;

  JsonObject backlight = doc["backlight"].to<JsonObject>();
  backlight["nightEnabled"] = backlightSettings.nightEnabled != 0;
  backlight["nightBrightness"] = backlightSettings.nightBrightness;
  char timeBuf[6];
  formatMinuteOfDay(backlightSettings.nightStartMinute, timeBuf, sizeof(timeBuf));
  backlight["nightStart"] = timeBuf;
  formatMinuteOfDay(backlightSettings.nightEndMinute, timeBuf, sizeof(timeBuf));
  backlight["nightEnd"] = timeBuf;
  backlight["offlineSleepMinutes"] = backlightSettings.offlineSleepMinutes;
  backlight["nightOfflineOff"] = backlightSettings.nightOfflineOff != 0;

  // The face table, so the portal can name every face and disable the surfaces
  // a layout does not carry, without hardcoding the product of the two axes.
  JsonArray faces = doc["faces"].to<JsonArray>();
  for (uint8_t i = 0; i < FACE_COUNT; i++) {
    const FaceFamily& f = faceSpec(i);
    JsonObject entry = faces.add<JsonObject>();
    entry["layout"] = f.layout;
    entry["surface"] = f.surface;
    entry["bands"] = f.bands;
    entry["rows"] = f.rowStyle;
    entry["name"] = f.name;
  }

  JsonObject touch = doc["touch"].to<JsonObject>();
  touch["supported"] = touchInputSupported();
  touch["enabled"] = touchSettings.enabled != 0;
  touch["pin"] = touchSettings.pin;
  touch["pinValid"] = touchPinValid();
  touch["activeHigh"] = touchSettings.activeHigh != 0;
  touch["shortAction"] = touchSettings.shortAction;
  touch["longAction"] = touchSettings.longAction;
  touch["styleMask"] = touchSettings.styleMask;
  touch["rememberStyle"] = touchSettings.rememberStyle != 0;
  JsonArray allowedPins = touch["allowedPins"].to<JsonArray>();
  for (uint8_t pin = 0; pin <= 48; pin++) {
    if (touchPinAllowed(pin)) allowedPins.add(pin);
  }

  JsonObject led = doc["led"].to<JsonObject>();
  led["enabled"] = ledSettings.enabled != 0;
  led["pin"] = ledSettings.pin;
  led["pinValid"] = ledPinValid();
  led["brightness"] = ledSettings.brightness;
  led["nightEnabled"] = ledSettings.nightEnabled != 0;
  led["nightBrightness"] = ledSettings.nightBrightness;
  led["followDisplay"] = ledSettings.followDisplay != 0;
  led["offlineOff"] = ledSettings.offlineOff != 0;
  JsonArray ledPins = led["allowedPins"].to<JsonArray>();
  for (uint8_t pin = 0; pin <= 48; pin++) {
    if (ledPinAllowed(pin)) ledPins.add(pin);
  }

  JsonObject colors = doc["colors"].to<JsonObject>();
  addHtmlColor(colors, "bg", dispSettings.bgColor);
  addHtmlColor(colors, "track", dispSettings.trackColor);
  addHtmlColor(colors, "warn", dispSettings.warnColor);
  addHtmlColor(colors, "clock", dispSettings.clockTimeColor);
  addHtmlColor(colors, "date", dispSettings.clockDateColor);
  addHtmlColor(colors, "value", themeSettings.valueColor);
  addHtmlColor(colors, "label", themeSettings.labelColor);
  addHtmlColor(colors, "secondary", themeSettings.secondaryColor);
  addHtmlColor(colors, "tile", themeSettings.tileColor);
  colors["labelMode"] = themeSettings.labelMode;
  colors["tileTint"] = themeSettings.tileTintPct;

  JsonObject clock = doc["clock"].to<JsonObject>();
  clock["face"] = clockFace;
  clock["use24h"] = netSettings.use24h;
  clock["dateFormat"] = netSettings.dateFormat;
  clock["hideDate"] = dispSettings.hideClockDate;
  clock["timezone"] = netSettings.timezoneStr;

  JsonObject network = doc["network"].to<JsonObject>();
  network["ssid"] = wifiSSID;
  network["hostname"] = netSettings.hostname;
  network["mdns"] = netSettings.mdnsEnabled;
  network["showIp"] = netSettings.showIPAtStartup;
  network["dhcp"] = netSettings.useDHCP;
  network["staticIp"] = netSettings.staticIP;
  network["gateway"] = netSettings.gateway;
  network["subnet"] = netSettings.subnet;
  network["dns"] = netSettings.dns;

  // The editor defaults to the page the panel is showing; ?page=N reads another
  // one so a page can be edited without switching the panel onto it.
  const uint8_t editPage =
    (uint8_t)clampedArg("page", activeMetricPage, 0, METRIC_PAGE_COUNT - 1);
  const GaugeMapping& editMap = gaugePages[editPage];
  const GaugeLabels& editLabels = gaugePageLabels[editPage];

  JsonObject pages = doc["pages"].to<JsonObject>();
  pages["count"] = METRIC_PAGE_COUNT;
  pages["active"] = activeMetricPage;
  pages["editing"] = editPage;
  pages["rotateSeconds"] = pageSettings.rotateSeconds;
  pages["rotateUnit"] = pageSettings.rotateUnit;
  pages["pinSeconds"] = pageSettings.pinSeconds;
  pages["rememberPage"] = pageSettings.rememberPage != 0;
  pages["startPage"] = pageSettings.startPage;
  JsonArray bound = pages["bound"].to<JsonArray>();
  for (uint8_t p = 0; p < METRIC_PAGE_COUNT; p++) bound.add(metricPageHasBindings(p));
  JsonArray gauges = doc["gauges"].to<JsonArray>();
  char colorBuf[8];
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    JsonObject slot = gauges.add<JsonObject>();
    slot["metricId"] = editMap.slots[i].metricId;
    slot["type"] = editMap.slots[i].type;
    slot["scaleMax"] = editMap.slots[i].scaleMax;
    slot["label"] = editLabels.labels[i];
    rgb565ToHtml(editMap.slots[i].arcColor, colorBuf);
    slot["color"] = colorBuf;
  }

  JsonArray metrics = doc["metrics"].to<JsonArray>();
  for (uint8_t i = 0; i < pcData.count; i++) {
    JsonObject metric = metrics.add<JsonObject>();
    metric["id"] = pcData.metrics[i].id;
    metric["name"] = pcData.metrics[i].name;
    metric["value"] = pcData.metrics[i].value;
    metric["unit"] = pcData.metrics[i].unit;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Portable configuration backup
//
//  WiFi credentials are intentionally omitted. A backup can therefore be
//  shared between devices without exposing the network password, and restore
//  never disconnects a device by replacing its SSID without a matching key.
// ---------------------------------------------------------------------------
static const char* CONFIG_BACKUP_FORMAT = "statnode-config";
// Backups exported by pre-rename (PCMonitorColor) firmware remain importable.
static const char* CONFIG_BACKUP_FORMAT_LEGACY = "pcmonitorcolor-config";
static const char* CONFIG_BACKUP_PRODUCT_LEGACY = "PCMonitorColor";
// Schema 2 added the face tuple to display and changed touch.styleMask from
// DisplayStyle bits to face-index bits. Schema 1 backups still import: their
// legacy style seeds the tuple and their mask is remapped.
static const uint8_t CONFIG_BACKUP_SCHEMA = 2;
static const size_t CONFIG_IMPORT_MAX_BYTES = 16 * 1024;

static void handleConfigExport() {
  JsonDocument doc;
  doc["format"] = CONFIG_BACKUP_FORMAT;
  doc["schema"] = CONFIG_BACKUP_SCHEMA;
  doc["product"] = PRODUCT_NAME;
  doc["firmware"] = FW_VERSION;
  doc["board"] = BOARD_VARIANT;
  doc["wifiCredentialsIncluded"] = false;

  JsonObject display = doc["display"].to<JsonObject>();
  // Schema 2 carries the face tuple; "style" remains the derived legacy value
  // so a schema 2 backup still restores onto firmware that only knows styles.
  display["layout"] = displayLayout;
  display["surface"] = displaySurface;
  display["duoBands"] = duoHeroBands;
  display["duoRows"] = duoRowStyle;
  display["style"] = displayStyle;
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["smoothing"] = dispSettings.gaugeSmoothing;
  display["sparkSeconds"] = sparkRedrawSec;
  display["chartSmooth"] = chartSmoothing;
  display["glassTheme"] = glassTheme;
  display["glassGloss"] = glassGlossPct;
  display["glassBow"] = glassBowPct;
  display["glassChartFill"] = glassChartFillPct;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["smallLabels"] = dispSettings.smallLabels;
  display["invertColors"] = dispSettings.invertColors;
  display["cydClassic"] = dispSettings.cydPanelClassic;

  JsonObject backlight = doc["backlight"].to<JsonObject>();
  backlight["nightEnabled"] = backlightSettings.nightEnabled != 0;
  backlight["nightBrightness"] = backlightSettings.nightBrightness;
  backlight["nightStartMinute"] = backlightSettings.nightStartMinute;
  backlight["nightEndMinute"] = backlightSettings.nightEndMinute;
  backlight["offlineSleepMinutes"] = backlightSettings.offlineSleepMinutes;
  backlight["nightOfflineOff"] = backlightSettings.nightOfflineOff != 0;

  JsonObject touch = doc["touch"].to<JsonObject>();
  touch["enabled"] = touchSettings.enabled != 0;
  touch["pin"] = touchSettings.pin;
  touch["activeHigh"] = touchSettings.activeHigh != 0;
  touch["shortAction"] = touchSettings.shortAction;
  touch["longAction"] = touchSettings.longAction;
  touch["styleMask"] = touchSettings.styleMask;
  touch["rememberStyle"] = touchSettings.rememberStyle != 0;

  JsonObject colors = doc["colors"].to<JsonObject>();
  addHtmlColor(colors, "bg", dispSettings.bgColor);
  addHtmlColor(colors, "track", dispSettings.trackColor);
  addHtmlColor(colors, "warn", dispSettings.warnColor);
  addHtmlColor(colors, "clock", dispSettings.clockTimeColor);
  addHtmlColor(colors, "date", dispSettings.clockDateColor);
  addHtmlColor(colors, "value", themeSettings.valueColor);
  addHtmlColor(colors, "label", themeSettings.labelColor);
  addHtmlColor(colors, "secondary", themeSettings.secondaryColor);
  addHtmlColor(colors, "tile", themeSettings.tileColor);
  colors["labelMode"] = themeSettings.labelMode;
  colors["tileTint"] = themeSettings.tileTintPct;

  JsonObject clock = doc["clock"].to<JsonObject>();
  clock["face"] = clockFace;
  clock["use24h"] = netSettings.use24h;
  clock["dateFormat"] = netSettings.dateFormat;
  clock["hideDate"] = dispSettings.hideClockDate;
  clock["timezone"] = netSettings.timezoneStr;

  JsonObject network = doc["network"].to<JsonObject>();
  network["hostname"] = netSettings.hostname;
  network["mdns"] = netSettings.mdnsEnabled;
  network["showIp"] = netSettings.showIPAtStartup;
  network["dhcp"] = netSettings.useDHCP;
  network["staticIp"] = netSettings.staticIP;
  network["gateway"] = netSettings.gateway;
  network["subnet"] = netSettings.subnet;
  network["dns"] = netSettings.dns;

  // The backup's "gauges" array is page 0 by definition, so that older firmware
  // restoring this file gets the page it knows how to show.
  JsonArray gauges = doc["gauges"].to<JsonArray>();
  char colorBuf[8];
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    JsonObject slot = gauges.add<JsonObject>();
    slot["metricId"] = gaugePages[0].slots[i].metricId;
    slot["type"] = gaugePages[0].slots[i].type;
    slot["scaleMax"] = gaugePages[0].slots[i].scaleMax;
    slot["label"] = gaugePageLabels[0].labels[i];
    rgb565ToHtml(gaugePages[0].slots[i].arcColor, colorBuf);
    slot["color"] = colorBuf;
  }

  // Pages past the first ride in an OPTIONAL object rather than changing the
  // mandatory "gauges" array or the schema number. A backup taken here still
  // restores on a firmware that predates pages: it reads "gauges" as page 0 and
  // ignores what it does not recognise. Bumping the schema would instead have
  // made those builds reject the file outright, since they range-check it.
  JsonObject pagesOut = doc["pages"].to<JsonObject>();
  pagesOut["rotateSeconds"] = pageSettings.rotateSeconds;
  // Absent on backups taken before the minute option existed, and absent means
  // seconds, so an old file restores the interval it was written with.
  pagesOut["rotateUnit"] = pageSettings.rotateUnit;
  pagesOut["pinSeconds"] = pageSettings.pinSeconds;
  pagesOut["rememberPage"] = pageSettings.rememberPage != 0;
  pagesOut["startPage"] = pageSettings.startPage;
  JsonArray extra = pagesOut["slots"].to<JsonArray>();
  for (uint8_t p = 1; p < METRIC_PAGE_COUNT; p++) {
    JsonArray pageArr = extra.add<JsonArray>();
    for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
      JsonObject slot = pageArr.add<JsonObject>();
      slot["metricId"] = gaugePages[p].slots[i].metricId;
      slot["type"] = gaugePages[p].slots[i].type;
      slot["scaleMax"] = gaugePages[p].slots[i].scaleMax;
      slot["label"] = gaugePageLabels[p].labels[i];
      rgb565ToHtml(gaugePages[p].slots[i].arcColor, colorBuf);
      slot["color"] = colorBuf;
    }
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Content-Disposition",
                    "attachment; filename=statnode-config.json");
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Save handlers
// ---------------------------------------------------------------------------
// Peripherals moved to their own page and endpoint; /save/display keeps only
// what the panel itself does. Touch args posted here are ignored rather than
// rejected, so an old bookmark or script cannot 400 on a display change.
static void handleSaveHardware() {
  // The LED pin is validated against the pad settings from THIS post, so the
  // touch block has to land in the globals before the LED block runs. Keep the
  // previous values to roll back with: a rejected save must leave nothing
  // applied, or the next save from any other page would persist half of it.
  const TouchSettings prevTouch = touchSettings;
  if (touchInputSupported()) {
    TouchSettings next = touchSettings;
    next.enabled = server.hasArg("touchEnabled") ? 1 : 0;
    next.pin = (uint8_t)clampedArg("touchPin", next.pin, 0, 48);
    next.activeHigh = server.hasArg("touchActiveHigh") ? 1 : 0;
    next.shortAction =
      (uint8_t)clampedArg("touchShort", next.shortAction, 0, TOUCH_ACTION_COUNT - 1);
    next.longAction =
      (uint8_t)clampedArg("touchLong", next.longAction, 0, TOUCH_ACTION_COUNT - 1);
    next.rememberStyle = server.hasArg("touchRemember") ? 1 : 0;
    next.styleMask = 0;
    for (uint8_t i = 0; i < FACE_COUNT; i++) {
      char key[16];
      snprintf(key, sizeof(key), "touchFace%u", i);
      if (server.hasArg(key)) next.styleMask |= 1u << i;
    }
    if (!touchPinAllowed(next.pin)) {
      sendJsonMessage(400, false, "That touch GPIO is reserved or unsupported on this board.");
      return;
    }
    // Only meaningful while the pad is enabled - an unused pad must not block
    // saving the rest of the page.
    if (next.enabled && next.styleMask == 0) {
      sendJsonMessage(400, false, "Select at least one layout for touch cycling.");
      return;
    }
    if (next.styleMask == 0) next.styleMask = touchSettings.styleMask;
    touchSettings = next;
  }

  LedSettings nextLed = ledSettings;
  nextLed.enabled = server.hasArg("ledEnabled") ? 1 : 0;
  nextLed.pin = (uint8_t)clampedArg("ledPin", nextLed.pin, 0, 48);
  nextLed.brightness = (uint8_t)clampedArg("ledBrightness", nextLed.brightness, 0, 255);
  nextLed.nightEnabled = server.hasArg("ledNightEnabled") ? 1 : 0;
  nextLed.nightBrightness =
    (uint8_t)clampedArg("ledNightBrightness", nextLed.nightBrightness, 0, 255);
  nextLed.followDisplay = server.hasArg("ledFollowDisplay") ? 1 : 0;
  nextLed.offlineOff = server.hasArg("ledOfflineOff") ? 1 : 0;
  // Validated against the touch settings that were just accepted above, so a
  // save that moves the pad onto the LED's pin is refused instead of silently
  // disabling the LED on the next boot.
  if (nextLed.enabled && !ledPinAllowed(nextLed.pin)) {
    touchSettings = prevTouch;
    sendJsonMessage(400, false,
                    "That LED GPIO is reserved, in use, or unsupported on this board.");
    return;
  }
  ledSettings = nextLed;

  saveSettings();
  initTouchButton();
  initLed();
  sendJsonMessage(200, true, "Hardware settings applied.");
}

static void handleLedPreview() {
  const bool enabled = server.hasArg("ledEnabled");
  const uint8_t pin = (uint8_t)clampedArg("ledPin", ledSettings.pin, 0, 48);
  const uint8_t level =
    (uint8_t)clampedArg("ledBrightness", ledSettings.brightness, 0, 255);
  if (server.hasArg("stop")) {
    clearLedPreview();
    sendJsonMessage(200, true, "Preview ended.");
    return;
  }
  if (enabled && !ledPinAllowed(pin)) {
    sendJsonMessage(400, false,
                    "That LED GPIO is reserved, in use, or unsupported on this board.");
    return;
  }
  previewLed(enabled, pin, level);
  sendJsonMessage(200, true, "Preview applied.");
}

// Live preview for the Glass surface sliders: applies to RAM and repaints, but
// deliberately does NOT persist. A slider drag would otherwise commit one NVS
// write per pixel of travel. /save/display commits; the portal's Revert reloads
// /api/config and re-sends the stored values, which snaps the panel back.
static void handleGlassPreview() {
  if (server.hasArg("stop")) {
    setGlassPreview(false, 0, 0, 0);
    forceDisplayRedraw();
    sendJsonMessage(200, true, "Preview ended.");
    return;
  }
  setGlassPreview(true,
                  (uint8_t)clampedArg("glassGloss", glassGlossPct, 0, 100),
                  (uint8_t)clampedArg("glassBow", glassBowPct, 0, 100),
                  (uint8_t)clampedArg("glassChartFill", glassChartFillPct, 0, 100));
  forceDisplayRedraw();
  sendJsonMessage(200, true, "Preview applied.");
}

static void handleSaveDisplay() {
  bool panelChanged = false;

  applyPageSettingsArgs();
  applyMetricPageArg();
  applyFaceArgs();
  dispSettings.rotation = (uint8_t)clampedArg("rotation", dispSettings.rotation, 0, 3);
  brightness = (uint8_t)clampedArg("brightness", brightness, 0, 255);
  dispSettings.gaugeSmoothing = (uint8_t)clampedArg("smoothing", dispSettings.gaugeSmoothing, 0, 3);
  sparkRedrawSec = (uint8_t)clampedArg("sparks", sparkRedrawSec, 1, 60);
  chartSmoothing =
    (uint8_t)clampedArg("chartSmooth", chartSmoothing, 0, CHART_SMOOTH_MAX);
  glassTheme =
    (uint8_t)clampedArg("glassTheme", glassTheme, 0, GLASS_THEME_COUNT - 1);
  glassGlossPct = (uint8_t)clampedArg("glassGloss", glassGlossPct, 0, 100);
  glassBowPct = (uint8_t)clampedArg("glassBow", glassBowPct, 0, 100);
  glassChartFillPct =
    (uint8_t)clampedArg("glassChartFill", glassChartFillPct, 0, 100);
  // The saved values are now the truth; drop any live preview sitting in front
  // of them or the panel would keep showing the last dragged slider position.
  setGlassPreview(false, 0, 0, 0);
  dispSettings.tempScaleMax = (uint16_t)clampedArg("tempScale", dispSettings.tempScaleMax, 1, 500);
  dispSettings.powerScaleW = (uint16_t)clampedArg("powerScale", dispSettings.powerScaleW, 1, 65535);
  dispSettings.warnThresholdPct = (uint8_t)clampedArg("warnThreshold", dispSettings.warnThresholdPct, 0, 100);
  dispSettings.smallLabels = server.hasArg("smallLabels");
  dispSettings.invertColors = server.hasArg("invertColors");
  backlightSettings.nightEnabled = server.hasArg("nightEnabled") ? 1 : 0;
  backlightSettings.nightBrightness =
    (uint8_t)clampedArg("nightBrightness", backlightSettings.nightBrightness, 0, 255);
  backlightSettings.nightStartMinute =
    minuteOfDayArg("nightStart", backlightSettings.nightStartMinute);
  backlightSettings.nightEndMinute =
    minuteOfDayArg("nightEnd", backlightSettings.nightEndMinute);
  backlightSettings.offlineSleepMinutes =
    (uint16_t)clampedArg("offlineSleep", backlightSettings.offlineSleepMinutes, 0, 1440);
  backlightSettings.nightOfflineOff = server.hasArg("nightOfflineOff") ? 1 : 0;
#if defined(DISPLAY_CYD)
  bool cydClassic = server.hasArg("cydClassic");
  panelChanged = cydClassic != dispSettings.cydPanelClassic;
  dispSettings.cydPanelClassic = cydClassic;
#endif

  saveSettings();
  refreshBacklightControl();
  applyDisplaySettings();
  markScreenCleared();
  forceDisplayRedraw();
  sendJsonMessage(200, true,
                  panelChanged ? "Display saved. Restarting for the panel change."
                               : "Display settings applied.",
                  panelChanged);
  if (panelChanged) scheduleRestart(1500);
}

static void handleSaveGauges() {
  // The face selector lives on the Display page now. A legacy "style" field is
  // still accepted here, mapped through the face table, so existing scripts and
  // bookmarks that switched the face this way keep working.
  if (server.hasArg("style")) {
    const long raw = server.arg("style").toInt();
    if (raw >= STYLE_BIG_NUMBERS && raw < STYLE_COUNT)
      applyFace(faceIndexFromLegacyStyle((uint8_t)raw));
  }
  if (server.hasArg("sparks"))
    sparkRedrawSec = (uint8_t)clampedArg("sparks", sparkRedrawSec, 1, 60);

  // Which page this save edits. Defaults to the one on screen, so a client that
  // knows nothing about pages keeps its old behavior; the portal names the page
  // explicitly. An out-of-range value clamps rather than silently hitting page 0.
  const uint8_t editPage =
    (uint8_t)clampedArg("page", activeMetricPage, 0, METRIC_PAGE_COUNT - 1);
  GaugeMapping& editMap = gaugePages[editPage];
  GaugeLabels& editLabels = gaugePageLabels[editPage];
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    GaugeSlot& slot = editMap.slots[i];
    char key[4];
    snprintf(key, sizeof(key), "m%u", i);
    slot.metricId = (uint8_t)clampedArg(key, slot.metricId, 0, MAX_METRICS);
    snprintf(key, sizeof(key), "t%u", i);
    slot.type = (uint8_t)clampedArg(key, slot.type, 0, GAUGE_TYPE_COUNT - 1);
    snprintf(key, sizeof(key), "s%u", i);
    slot.scaleMax = (uint16_t)clampedArg(key, slot.scaleMax, 0, 65535);
    snprintf(key, sizeof(key), "c%u", i);
    if (server.hasArg(key)) slot.arcColor = htmlToRgb565(server.arg(key).c_str());
    snprintf(key, sizeof(key), "l%u", i);
    if (server.hasArg(key)) {
      sanitizeGaugeLabel(server.arg(key).c_str(), editLabels.labels[i],
                         GAUGE_LABEL_LENGTH);
    }
  }
  saveSettings();
  // Clearing every binding on the page currently shown would leave the panel
  // blank with no obvious way back, so fall back to one that still has some.
  // If nothing is bound anywhere there is nothing to fall back to, and staying
  // put is the honest result.
  if (!metricPageHasBindings(activeMetricPage)) {
    setActiveMetricPage(firstBoundMetricPage(activeMetricPage));
    saveStartMetricPage();
  }
  // Editing a page that is not on screen must not flash the one that is.
  if (editPage == activeMetricPage) forceDisplayRedraw(true);
  sendJsonMessage(200, true, "Metric layout applied.");
}

struct ColorPreset {
  const char* id;
  uint16_t bg, track, warn, clockTime, clockDate;
  uint16_t value, label, secondary, tile;
  uint8_t labelMode, tileTintPct;
  uint16_t slots[NUM_GAUGE_SLOTS];
};

// Every preset must fill ALL NUM_GAUGE_SLOTS accent colours. A short
// initialiser list zero-fills the rest, and 0x0000 is BLACK - that is what
// blanked slots 7 and 8 when the slot count went 6 -> 8 while these lists
// stayed at six. Raising the count must be a deliberate edit here too.
static_assert(NUM_GAUGE_SLOTS == 8,
              "Slot count changed: extend every kPresets slots[] list below.");

static const ColorPreset kPresets[] = {
  // Factory defaults (config.h palette).
  { "default", CLR_BG, CLR_TRACK, CLR_RED, CLR_TEXT, CLR_TEXT_DIM,
    CLR_TEXT, CLR_TEXT_DIM, CLR_TEXT_DIM, CLR_CARD,
    THEME_LABEL_CLASSIC, 0,
    { CLR_ORANGE, CLR_BLUE, CLR_GREEN, CLR_CYAN, CLR_GOLD, CLR_RED,
      CLR_VIOLET, CLR_ROSE } },
  // "Modern": the colorblind-validated categorical set from the redesign.
  { "modern", 0x1082 /*#101214*/, 0x2146 /*#202830*/, 0xE249 /*#E5484D*/,
    0xF7BE /*#F5F5F5*/, 0x9D35 /*#9AA4AD*/,
    0xF7BE /*#F5F5F5*/, 0xBE19 /*#B8C3CC*/, 0x84B4 /*#8695A1*/,
    0x10C4 /*#151A20*/, THEME_LABEL_CUSTOM, 12,
    { 0x3C3C /*#3987E5*/, 0x0400 /*#008300*/, 0xD290 /*#D55181*/,
      0xCC20 /*#C98500*/, 0x1CEE /*#199E70*/, 0xDAC4 /*#D95926*/,
      CLR_VIOLET, CLR_CYAN } },
  { "oled", CLR_BG, 0x1082 /*#101214*/, CLR_RED, CLR_TEXT, CLR_TEXT_DIM,
    CLR_TEXT, CLR_TEXT_DIM, CLR_TEXT_DIM, CLR_BG,
    THEME_LABEL_ACCENT, 12,
    { CLR_ORANGE, CLR_BLUE, CLR_GREEN, CLR_CYAN, CLR_GOLD, CLR_RED,
      CLR_VIOLET, CLR_ROSE } },
  { "slate", 0x10C4 /*#101820*/, 0x3209 /*#31404C*/, 0xFAEB /*#FF5D5D*/,
    0xF7BE /*#F2F5F7*/, 0x84B4 /*#8695A1*/,
    0xF7BE /*#F2F5F7*/, 0xBE19 /*#B8C3CC*/, 0x84B4 /*#8695A1*/,
    0x1905 /*#18232D*/, THEME_LABEL_CUSTOM, 12,
    { 0x3C3C, CLR_GREEN, CLR_CYAN, CLR_GOLD, CLR_ORANGE, 0xF81F,
      CLR_VIOLET, CLR_ROSE } },
  { "amber", 0x1040 /*#100B05*/, 0x4183 /*#44321D*/, 0xFAE8 /*#FF5D45*/,
    0xFF37 /*#FFE7BD*/, 0xAC4A /*#AC8956*/,
    0xFF37 /*#FFE7BD*/, 0xF5CB /*#F1B85E*/, 0xAC4A /*#AC8956*/,
    0x20A1 /*#21170B*/, THEME_LABEL_CUSTOM, 12,
    { CLR_GOLD, CLR_ORANGE, CLR_YELLOW, CLR_RED, 0xF5CB, 0xAC4A,
      0xFDCE /*#FFB870*/, 0xCBC5 /*#C87A2A*/ } },
  { "contrast", CLR_BG, 0x52AA /*#555555*/, CLR_RED, CLR_TEXT, CLR_YELLOW,
    CLR_TEXT, CLR_YELLOW, CLR_TEXT, 0x1082 /*#101010*/,
    THEME_LABEL_AUTO, 0,
    { CLR_CYAN, CLR_YELLOW, CLR_GREEN, 0xF81F, CLR_ORANGE, CLR_RED,
      CLR_VIOLET, CLR_TEXT } },
};

static void handleSaveColors() {
  const String pre = server.arg("preset");
  const ColorPreset* preset = nullptr;
  for (const ColorPreset& cp : kPresets) {
    if (pre == cp.id) { preset = &cp; break; }
  }
  if (preset) {
    dispSettings.bgColor        = preset->bg;
    dispSettings.trackColor     = preset->track;
    dispSettings.warnColor      = preset->warn;
    dispSettings.clockTimeColor = preset->clockTime;
    dispSettings.clockDateColor = preset->clockDate;
    themeSettings.valueColor     = preset->value;
    themeSettings.labelColor     = preset->label;
    themeSettings.secondaryColor = preset->secondary;
    themeSettings.tileColor      = preset->tile;
    themeSettings.labelMode      = preset->labelMode;
    themeSettings.tileTintPct    = preset->tileTintPct;
    // A preset replaces the whole theme, so it resets the slot accents on every
    // page rather than leaving pages the user is not looking at wearing the
    // colors of the previous palette.
    for (uint8_t p = 0; p < METRIC_PAGE_COUNT; p++)
      for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++)
        gaugePages[p].slots[i].arcColor = preset->slots[i];
  } else {
    if (server.hasArg("cbg"))    dispSettings.bgColor        = htmlToRgb565(server.arg("cbg").c_str());
    if (server.hasArg("ctrack")) dispSettings.trackColor     = htmlToRgb565(server.arg("ctrack").c_str());
    if (server.hasArg("cwarn"))  dispSettings.warnColor      = htmlToRgb565(server.arg("cwarn").c_str());
    if (server.hasArg("cct"))    dispSettings.clockTimeColor = htmlToRgb565(server.arg("cct").c_str());
    if (server.hasArg("ccd"))    dispSettings.clockDateColor = htmlToRgb565(server.arg("ccd").c_str());
    if (server.hasArg("cvalue")) themeSettings.valueColor = htmlToRgb565(server.arg("cvalue").c_str());
    if (server.hasArg("clabel")) themeSettings.labelColor = htmlToRgb565(server.arg("clabel").c_str());
    if (server.hasArg("csecondary"))
      themeSettings.secondaryColor = htmlToRgb565(server.arg("csecondary").c_str());
    if (server.hasArg("ctile")) themeSettings.tileColor = htmlToRgb565(server.arg("ctile").c_str());
    themeSettings.labelMode = (uint8_t)clampedArg(
      "labelMode", themeSettings.labelMode, THEME_LABEL_CLASSIC, THEME_LABEL_AUTO);
    themeSettings.tileTintPct = (uint8_t)clampedArg(
      "tileTint", themeSettings.tileTintPct, 0, 30);
  }
  saveSettings();
  applyDisplaySettings();   // repaints the panel with the new background
  markScreenCleared();      // styles must repaint their static chrome
  forceDisplayRedraw();
  sendJsonMessage(200, true, "Color palette applied.");
}

static void handleSaveClock() {
  clockFace = (uint8_t)clampedArg(
    "clockFace", clockFace, CLOCK_FACE_STANDARD, CLOCK_FACE_COUNT - 1);
  netSettings.use24h = server.arg("timeFormat") != "12";
  netSettings.dateFormat = (uint8_t)clampedArg("dateFormat", netSettings.dateFormat, 0, 5);
  dispSettings.hideClockDate = server.hasArg("hideClockDate");
  if (server.hasArg("timezone"))
    strlcpy(netSettings.timezoneStr, server.arg("timezone").c_str(),
            sizeof(netSettings.timezoneStr));
  saveSettings();
  configTzTime(netSettings.timezoneStr, "pool.ntp.org", "time.nist.gov");
  refreshBacklightControl();
  forceDisplayRedraw(true);
  sendJsonMessage(200, true, "Clock settings applied.");
}

static bool validIp(const String& value, bool allowEmpty = false) {
  if (allowEmpty && value.length() == 0) return true;
  IPAddress parsed;
  return parsed.fromString(value);
}

static bool backupFieldError(String& error, const char* path,
                             const char* expectation) {
  error = "Invalid backup field '";
  error += path;
  error += "': ";
  error += expectation;
  return false;
}

static bool readBackupInteger(JsonObjectConst object, const char* key,
                              long minValue, long maxValue, long& out,
                              String& error, const char* path) {
  JsonVariantConst value = object[key];
  if (value.isNull() || !value.is<long>())
    return backupFieldError(error, path, "expected an integer");
  const long parsed = value.as<long>();
  if (parsed < minValue || parsed > maxValue)
    return backupFieldError(error, path, "value is out of range");
  out = parsed;
  return true;
}

static bool readBackupBool(JsonObjectConst object, const char* key, bool& out,
                           String& error, const char* path) {
  JsonVariantConst value = object[key];
  if (value.isNull() || !value.is<bool>())
    return backupFieldError(error, path, "expected true or false");
  out = value.as<bool>();
  return true;
}

static bool readBackupString(JsonObjectConst object, const char* key,
                             size_t maxLength, const char*& out,
                             String& error, const char* path) {
  JsonVariantConst value = object[key];
  if (value.isNull() || !value.is<const char*>())
    return backupFieldError(error, path, "expected text");
  out = value.as<const char*>();
  if (!out || strlen(out) > maxLength)
    return backupFieldError(error, path, "text is too long");
  return true;
}

static bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

// Forward declaration: the page parser below is defined after readBackupColor.
static bool readBackupPage(JsonArrayConst arr, GaugeMapping& map,
                           GaugeLabels& labels, const char* what, String& error);

static bool readBackupColor(JsonObjectConst object, const char* key,
                            uint16_t& out, String& error, const char* path) {
  const char* value = nullptr;
  if (!readBackupString(object, key, 7, value, error, path)) return false;
  if (strlen(value) != 7 || value[0] != '#')
    return backupFieldError(error, path, "expected a #RRGGBB color");
  for (uint8_t i = 1; i < 7; i++) {
    if (!isHexDigit(value[i]))
      return backupFieldError(error, path, "expected a #RRGGBB color");
  }
  out = htmlToRgb565(value);
  return true;
}

// One page's worth of slot objects. Shared by the mandatory top-level "gauges"
// array (page 0) and by each entry of the optional "pages.slots", so every page
// is validated identically no matter which key carried it.
static bool readBackupPage(JsonArrayConst arr, GaugeMapping& map,
                           GaugeLabels& labels, const char* what, String& error) {
  if (arr.isNull() || arr.size() != NUM_GAUGE_SLOTS) {
    error = "The backup has the wrong number of metric slots.";
    return false;
  }
  long integer = 0;
  const char* textValue = nullptr;
  uint8_t slotIndex = 0;
  for (JsonObjectConst slot : arr) {
    char path[40];
    snprintf(path, sizeof(path), "%s[%u].metricId", what, slotIndex);
    if (!readBackupInteger(slot, "metricId", 0, MAX_METRICS, integer, error, path))
      return false;
    map.slots[slotIndex].metricId = (uint8_t)integer;
    snprintf(path, sizeof(path), "%s[%u].type", what, slotIndex);
    if (!readBackupInteger(slot, "type", 0, GAUGE_TYPE_COUNT - 1, integer, error, path))
      return false;
    map.slots[slotIndex].type = (uint8_t)integer;
    snprintf(path, sizeof(path), "%s[%u].scaleMax", what, slotIndex);
    if (!readBackupInteger(slot, "scaleMax", 0, 65535, integer, error, path))
      return false;
    map.slots[slotIndex].scaleMax = (uint16_t)integer;
    snprintf(path, sizeof(path), "%s[%u].label", what, slotIndex);
    if (!readBackupString(slot, "label", GAUGE_LABEL_LENGTH - 1, textValue, error, path))
      return false;
    sanitizeGaugeLabel(textValue, labels.labels[slotIndex], GAUGE_LABEL_LENGTH);
    snprintf(path, sizeof(path), "%s[%u].color", what, slotIndex);
    if (!readBackupColor(slot, "color", map.slots[slotIndex].arcColor, error, path))
      return false;
    slotIndex++;
  }
  return true;
}

static void handleConfigImport() {
  const String& body = server.arg("plain");
  if (body.length() == 0) {
    sendJsonMessage(400, false, "Choose a StatNode backup file.");
    return;
  }
  if (body.length() > CONFIG_IMPORT_MAX_BYTES) {
    sendJsonMessage(413, false, "The configuration backup is too large.");
    return;
  }

  JsonDocument doc;
  DeserializationError parseError = deserializeJson(doc, body);
  if (parseError) {
    sendJsonMessage(400, false, "The selected file is not valid JSON.");
    return;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    sendJsonMessage(400, false, "The backup root must be a JSON object.");
    return;
  }

  const char* format = nullptr;
  String error;
  if (!readBackupString(root, "format", 40, format, error, "format") ||
      (strcmp(format, CONFIG_BACKUP_FORMAT) != 0 &&
       strcmp(format, CONFIG_BACKUP_FORMAT_LEGACY) != 0)) {
    sendJsonMessage(400, false, "This is not a StatNode configuration backup.");
    return;
  }
  const char* product = nullptr;
  if (!readBackupString(root, "product", 40, product, error, "product") ||
      (strcmp(product, PRODUCT_NAME) != 0 &&
       strcmp(product, CONFIG_BACKUP_PRODUCT_LEGACY) != 0)) {
    sendJsonMessage(400, false, "This backup belongs to a different product.");
    return;
  }
  long integer = 0;
  if (!readBackupInteger(root, "schema", 1, CONFIG_BACKUP_SCHEMA,
                         integer, error, "schema")) {
    sendJsonMessage(400, false, "This backup schema is not supported by this firmware.");
    return;
  }
  // Schema 1 predates the Layout x Surface split: it carries no face tuple and
  // its touch mask is in DisplayStyle bits.
  const bool legacyFaceSchema = integer < 2;

  JsonObjectConst display = root["display"].as<JsonObjectConst>();
  JsonObjectConst backlight = root["backlight"].as<JsonObjectConst>();
  JsonObjectConst touch = root["touch"].as<JsonObjectConst>();
  JsonObjectConst colors = root["colors"].as<JsonObjectConst>();
  JsonObjectConst clock = root["clock"].as<JsonObjectConst>();
  JsonObjectConst network = root["network"].as<JsonObjectConst>();
  JsonArrayConst gauges = root["gauges"].as<JsonArrayConst>();
  if (display.isNull() || backlight.isNull() || touch.isNull() ||
      colors.isNull() || clock.isNull() || network.isNull() || gauges.isNull()) {
    sendJsonMessage(400, false, "The backup is incomplete.");
    return;
  }
  if (gauges.size() != NUM_GAUGE_SLOTS) {
    sendJsonMessage(400, false, "The backup has the wrong number of metric slots.");
    return;
  }

  DisplaySettings nextDisplay = dispSettings;
  NetworkSettings nextNetwork = netSettings;
  // The backup's "gauges" array restores into page 0, matching what export
  // writes. Pages past the first start EMPTY rather than from what the device
  // currently holds: a backup that carries no "pages" block describes a
  // single-page device, so restoring it must not silently leave pages behind.
  GaugeMapping nextPages[METRIC_PAGE_COUNT];
  GaugeLabels nextPageLabels[METRIC_PAGE_COUNT];
  nextPages[0] = gaugePages[0];
  nextPageLabels[0] = gaugePageLabels[0];
  for (uint8_t p = 1; p < METRIC_PAGE_COUNT; p++) {
    emptyGaugeMapping(nextPages[p]);
    defaultGaugeLabels(nextPageLabels[p]);
  }
  PageSettings nextPageSettings = pageSettings;
  BacklightSettings nextBacklight = backlightSettings;
  TouchSettings nextTouch = touchSettings;
  ThemeSettings nextTheme = themeSettings;
  uint8_t nextDisplayStyle = displayStyle;
  uint8_t nextLayout = displayLayout;
  uint8_t nextSurface = displaySurface;
  uint8_t nextDuoBands = duoHeroBands;
  uint8_t nextDuoRows = duoRowStyle;
  uint8_t nextClockFace = clockFace;
  uint8_t nextSparkRedrawSec = sparkRedrawSec;
  uint8_t nextChartSmoothing = chartSmoothing;
  uint8_t nextGlassTheme = glassTheme;
  uint8_t nextGlassGlossPct = glassGlossPct;
  uint8_t nextGlassBowPct = glassBowPct;
  uint8_t nextGlassChartFillPct = glassChartFillPct;
  uint8_t nextBrightness = brightness;
  bool flag = false;

#define READ_INT(section, key, minValue, maxValue, target, castType) \
  do { \
    if (!readBackupInteger(section, key, minValue, maxValue, integer, error, \
                           #section "." key)) { \
      sendJsonMessage(400, false, error.c_str()); \
      return; \
    } \
    target = (castType)integer; \
  } while (0)
// Same as READ_INT but a MISSING key leaves the target alone instead of
// failing the restore. The backup schema is pinned at one exact version, so a
// file written before a setting existed still declares the current schema and
// would be rejected outright if every new key were mandatory. Anything added
// after the schema was frozen has to come in this way.
#define READ_INT_OPT(section, key, minValue, maxValue, target, castType) \
  do { \
    if (!(section)[key].isNull()) \
      READ_INT(section, key, minValue, maxValue, target, castType); \
  } while (0)
#define READ_BOOL(section, key, target) \
  do { \
    if (!readBackupBool(section, key, flag, error, #section "." key)) { \
      sendJsonMessage(400, false, error.c_str()); \
      return; \
    } \
    target = flag; \
  } while (0)
#define READ_COLOR(section, key, target) \
  do { \
    if (!readBackupColor(section, key, target, error, #section "." key)) { \
      sendJsonMessage(400, false, error.c_str()); \
      return; \
    } \
  } while (0)

  READ_INT(display, "style", 0, STYLE_COUNT - 1, nextDisplayStyle, uint8_t);
  nextDisplayStyle = normalizeDisplayStyle(nextDisplayStyle);
  // The legacy value seeds the tuple, so a schema 1 backup restores the face it
  // was taken on. Schema 2 then overwrites the seed with what it actually
  // stored, which is how the four combinations with no legacy value survive a
  // backup round trip.
  {
    const FaceFamily& seed = faceSpec(faceIndexFromLegacyStyle(nextDisplayStyle));
    nextLayout = seed.layout;
    nextSurface = seed.surface;
    nextDuoBands = seed.bands;
    nextDuoRows = seed.rowStyle;
  }
  READ_INT_OPT(display, "layout", 0, LAYOUT_COUNT - 1, nextLayout, uint8_t);
  READ_INT_OPT(display, "surface", 0, SURFACE_COUNT - 1, nextSurface, uint8_t);
  READ_INT_OPT(display, "duoBands", 1, DUO_BANDS_MAX, nextDuoBands, uint8_t);
  READ_INT_OPT(display, "duoRows", 0, DUO_ROWS_LIST, nextDuoRows, uint8_t);
  normalizeFace(nextLayout, nextSurface, nextDuoBands, nextDuoRows);
  READ_INT(display, "rotation", 0, 3, nextDisplay.rotation, uint8_t);
  READ_INT(display, "brightness", 0, 255, nextBrightness, uint8_t);
  READ_INT(display, "smoothing", 0, 3, nextDisplay.gaugeSmoothing, uint8_t);
  READ_INT(display, "sparkSeconds", 1, 60, nextSparkRedrawSec, uint8_t);
  READ_INT(display, "chartSmooth", 0, CHART_SMOOTH_MAX, nextChartSmoothing, uint8_t);
  READ_INT_OPT(display, "glassTheme", 0, GLASS_THEME_COUNT - 1, nextGlassTheme, uint8_t);
  READ_INT_OPT(display, "glassGloss", 0, 100, nextGlassGlossPct, uint8_t);
  READ_INT_OPT(display, "glassBow", 0, 100, nextGlassBowPct, uint8_t);
  READ_INT_OPT(display, "glassChartFill", 0, 100, nextGlassChartFillPct, uint8_t);
  READ_INT(display, "tempScale", 1, 500, nextDisplay.tempScaleMax, uint16_t);
  READ_INT(display, "powerScale", 1, 65535, nextDisplay.powerScaleW, uint16_t);
  READ_INT(display, "warnThreshold", 0, 100,
           nextDisplay.warnThresholdPct, uint8_t);
  READ_BOOL(display, "smallLabels", nextDisplay.smallLabels);
  READ_BOOL(display, "invertColors", nextDisplay.invertColors);
  READ_BOOL(display, "cydClassic", nextDisplay.cydPanelClassic);

  READ_BOOL(backlight, "nightEnabled", nextBacklight.nightEnabled);
  READ_INT(backlight, "nightBrightness", 0, 255,
           nextBacklight.nightBrightness, uint8_t);
  READ_INT(backlight, "nightStartMinute", 0, 1439,
           nextBacklight.nightStartMinute, uint16_t);
  READ_INT(backlight, "nightEndMinute", 0, 1439,
           nextBacklight.nightEndMinute, uint16_t);
  READ_INT(backlight, "offlineSleepMinutes", 0, 1440,
           nextBacklight.offlineSleepMinutes, uint16_t);
  // Optional: absent in backups exported before the night screen-off option.
  if (!backlight["nightOfflineOff"].isNull())
    READ_BOOL(backlight, "nightOfflineOff", nextBacklight.nightOfflineOff);

  READ_BOOL(touch, "enabled", nextTouch.enabled);
  READ_INT(touch, "pin", 0, 48, nextTouch.pin, uint8_t);
  READ_BOOL(touch, "activeHigh", nextTouch.activeHigh);
  READ_INT(touch, "shortAction", 0, TOUCH_ACTION_COUNT - 1,
           nextTouch.shortAction, uint8_t);
  READ_INT(touch, "longAction", 0, TOUCH_ACTION_COUNT - 1,
           nextTouch.longAction, uint8_t);
  READ_INT(touch, "styleMask", 1, (1u << FACE_COUNT) - 1u,
           nextTouch.styleMask, uint16_t);
  if (legacyFaceSchema)
    nextTouch.styleMask = faceMaskFromStyleMask(nextTouch.styleMask);
  nextTouch.styleMask &= FACE_ACTIVE_MASK;
  if (nextTouch.styleMask == 0)
    nextTouch.styleMask = 1u << FACE_DEFAULT;
  READ_BOOL(touch, "rememberStyle", nextTouch.rememberStyle);
  if (nextTouch.enabled && (!touchInputSupported() || !touchPinAllowed(nextTouch.pin))) {
    sendJsonMessage(400, false,
                    "The restored touch GPIO is unsupported on this board.");
    return;
  }

  READ_COLOR(colors, "bg", nextDisplay.bgColor);
  READ_COLOR(colors, "track", nextDisplay.trackColor);
  READ_COLOR(colors, "warn", nextDisplay.warnColor);
  READ_COLOR(colors, "clock", nextDisplay.clockTimeColor);
  READ_COLOR(colors, "date", nextDisplay.clockDateColor);
  READ_COLOR(colors, "value", nextTheme.valueColor);
  READ_COLOR(colors, "label", nextTheme.labelColor);
  READ_COLOR(colors, "secondary", nextTheme.secondaryColor);
  READ_COLOR(colors, "tile", nextTheme.tileColor);
  READ_INT(colors, "labelMode", 0, THEME_LABEL_MODE_COUNT - 1,
           nextTheme.labelMode, uint8_t);
  READ_INT(colors, "tileTint", 0, 30, nextTheme.tileTintPct, uint8_t);

  READ_INT(clock, "face", 0, CLOCK_FACE_COUNT - 1, nextClockFace, uint8_t);
  READ_BOOL(clock, "use24h", nextNetwork.use24h);
  READ_INT(clock, "dateFormat", 0, 5, nextNetwork.dateFormat, uint8_t);
  READ_BOOL(clock, "hideDate", nextDisplay.hideClockDate);
  const char* textValue = nullptr;
  if (!readBackupString(clock, "timezone", sizeof(nextNetwork.timezoneStr) - 1,
                        textValue, error, "clock.timezone")) {
    sendJsonMessage(400, false, error.c_str());
    return;
  }
  strlcpy(nextNetwork.timezoneStr, textValue, sizeof(nextNetwork.timezoneStr));

  READ_BOOL(network, "mdns", nextNetwork.mdnsEnabled);
  READ_BOOL(network, "showIp", nextNetwork.showIPAtStartup);
  READ_BOOL(network, "dhcp", nextNetwork.useDHCP);
  if (!readBackupString(network, "hostname", sizeof(nextNetwork.hostname) - 1,
                        textValue, error, "network.hostname")) {
    sendJsonMessage(400, false, error.c_str());
    return;
  }
  sanitizeHostname(textValue, nextNetwork.hostname, sizeof(nextNetwork.hostname));

  const char* networkKeys[] = { "staticIp", "gateway", "subnet", "dns" };
  char* networkTargets[] = { nextNetwork.staticIP, nextNetwork.gateway,
                             nextNetwork.subnet, nextNetwork.dns };
  const size_t networkSizes[] = { sizeof(nextNetwork.staticIP), sizeof(nextNetwork.gateway),
                                  sizeof(nextNetwork.subnet), sizeof(nextNetwork.dns) };
  for (uint8_t i = 0; i < 4; i++) {
    char path[28];
    snprintf(path, sizeof(path), "network.%s", networkKeys[i]);
    if (!readBackupString(network, networkKeys[i], networkSizes[i] - 1,
                          textValue, error, path)) {
      sendJsonMessage(400, false, error.c_str());
      return;
    }
    const bool allowEmpty = nextNetwork.useDHCP || i == 3;
    if (!validIp(String(textValue), allowEmpty)) {
      sendJsonMessage(400, false,
                      "The backup contains an invalid static IPv4 configuration.");
      return;
    }
    strlcpy(networkTargets[i], textValue, networkSizes[i]);
  }

  if (!readBackupPage(gauges, nextPages[0], nextPageLabels[0], "gauges", error)) {
    sendJsonMessage(400, false, error.c_str());
    return;
  }

  // Optional. Absent means the backup came from a single-page device or an
  // older firmware, and the empty pages seeded above are the right answer.
  JsonObjectConst pagesIn = root["pages"].as<JsonObjectConst>();
  if (!root["pages"].isNull() && pagesIn.isNull()) {
    sendJsonMessage(400, false, "Invalid backup field 'pages': expected an object");
    return;
  }
  if (!pagesIn.isNull()) {
    if (!pagesIn["rotateSeconds"].isNull()) {
      if (!readBackupInteger(pagesIn, "rotateSeconds", 0, 255, integer, error,
                             "pages.rotateSeconds")) {
        sendJsonMessage(400, false, error.c_str());
        return;
      }
      nextPageSettings.rotateSeconds = (uint8_t)integer;
      // The interval and its unit travel together. A file written before the
      // unit existed always meant seconds, so take the number without its unit
      // rather than reading it against whatever the device is set to now.
      nextPageSettings.rotateUnit = 0;
    }
    if (!pagesIn["rotateUnit"].isNull()) {
      if (!readBackupInteger(pagesIn, "rotateUnit", 0, 1, integer, error,
                             "pages.rotateUnit")) {
        sendJsonMessage(400, false, error.c_str());
        return;
      }
      nextPageSettings.rotateUnit = (uint8_t)integer;
    }
    if (!pagesIn["pinSeconds"].isNull()) {
      if (!readBackupInteger(pagesIn, "pinSeconds", 0, 240, integer, error,
                             "pages.pinSeconds")) {
        sendJsonMessage(400, false, error.c_str());
        return;
      }
      nextPageSettings.pinSeconds = (uint8_t)integer;
    }
    if (!pagesIn["rememberPage"].isNull()) {
      if (!readBackupBool(pagesIn, "rememberPage", flag, error,
                          "pages.rememberPage")) {
        sendJsonMessage(400, false, error.c_str());
        return;
      }
      nextPageSettings.rememberPage = flag ? 1 : 0;
      // Page backups written before startPage was added cannot name their
      // remembered page. Page 0 is deterministic; retaining the target
      // device's previous value would make the restore depend on its history.
      nextPageSettings.startPage = 0;
    }
    if (!pagesIn["startPage"].isNull()) {
      if (!readBackupInteger(pagesIn, "startPage", 0, METRIC_PAGE_COUNT - 1,
                             integer, error, "pages.startPage")) {
        sendJsonMessage(400, false, error.c_str());
        return;
      }
      nextPageSettings.startPage = (uint8_t)integer;
    }

    JsonArrayConst extra = pagesIn["slots"].as<JsonArrayConst>();
    if (!pagesIn["slots"].isNull() && extra.isNull()) {
      sendJsonMessage(400, false,
                      "Invalid backup field 'pages.slots': expected an array");
      return;
    }
    if (!extra.isNull()) {
      // "slots" carries pages 1..N-1 only; page 0 is the top-level "gauges"
      // array, so there is exactly one source of truth per page. A backup from
      // a build with more pages than this one simply has its tail ignored.
      uint8_t page = 1;
      for (JsonArrayConst pageArr : extra) {
        if (page >= METRIC_PAGE_COUNT) break;
        char what[16];
        snprintf(what, sizeof(what), "pages.slots[%u]", (unsigned)(page - 1));
        if (!readBackupPage(pageArr, nextPages[page], nextPageLabels[page],
                            what, error)) {
          sendJsonMessage(400, false, error.c_str());
          return;
        }
        page++;
      }
    }
  }
  if (nextPageSettings.startPage >= METRIC_PAGE_COUNT) nextPageSettings.startPage = 0;

#undef READ_INT
#undef READ_BOOL
#undef READ_COLOR

  dispSettings = nextDisplay;
  netSettings = nextNetwork;
  for (uint8_t p = 0; p < METRIC_PAGE_COUNT; p++) {
    gaugePages[p] = nextPages[p];
    gaugePageLabels[p] = nextPageLabels[p];
  }
  pageSettings = nextPageSettings;
  if (!metricPageHasBindings(pageSettings.startPage))
    pageSettings.startPage = firstBoundMetricPage(0);
  // The restored set may not include the page that was on screen.
  if (!metricPageHasBindings(activeMetricPage))
    setActiveMetricPage(firstBoundMetricPage(0));
  backlightSettings = nextBacklight;
  touchSettings = nextTouch;
  themeSettings = nextTheme;
  displayLayout = nextLayout;
  displaySurface = nextSurface;
  duoHeroBands = nextDuoBands;
  duoRowStyle = nextDuoRows;
  clockFace = nextClockFace;
  sparkRedrawSec = nextSparkRedrawSec;
  chartSmoothing = nextChartSmoothing;
  glassTheme = nextGlassTheme;
  glassGlossPct = nextGlassGlossPct;
  glassBowPct = nextGlassBowPct;
  glassChartFillPct = nextGlassChartFillPct;
  brightness = nextBrightness;
  saveSettings();

  sendJsonMessage(200, true,
                  "Configuration restored. WiFi credentials were preserved. Restarting.",
                  true);
  scheduleRestart(1800);
}

static void handleFactoryReset() {
  if (server.arg("confirmation") != "RESET") {
    sendJsonMessage(400, false, "Type RESET to confirm the factory reset.");
    return;
  }
  if (!factoryResetSettings()) {
    sendJsonMessage(500, false, "Could not erase the stored configuration.");
    return;
  }
  sendJsonMessage(200, true,
                  "Factory reset complete. Restarting in setup mode.", true);
  scheduleRestart(1800, true);
}

static void handleSaveNetwork() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) {
    sendJsonMessage(400, false, "WiFi network name must be 1 to 32 characters.");
    return;
  }

  NetworkSettings next = netSettings;
  next.useDHCP = server.arg("ipMode") != "static";
  next.mdnsEnabled = server.hasArg("mdns");
  next.showIPAtStartup = server.hasArg("showIp");
  if (server.hasArg("hostname"))
    sanitizeHostname(server.arg("hostname").c_str(), next.hostname, sizeof(next.hostname));

  if (!next.useDHCP) {
    String ip = server.arg("staticIp"); ip.trim();
    String gateway = server.arg("gateway"); gateway.trim();
    String subnet = server.arg("subnet"); subnet.trim();
    String dns = server.arg("dns"); dns.trim();
    if (!validIp(ip) || !validIp(gateway) || !validIp(subnet) || !validIp(dns, true)) {
      sendJsonMessage(400, false, "Enter valid IPv4 values for the static network configuration.");
      return;
    }
    strlcpy(next.staticIP, ip.c_str(), sizeof(next.staticIP));
    strlcpy(next.gateway, gateway.c_str(), sizeof(next.gateway));
    strlcpy(next.subnet, subnet.c_str(), sizeof(next.subnet));
    strlcpy(next.dns, dns.c_str(), sizeof(next.dns));
  }

  strlcpy(wifiSSID, ssid.c_str(), sizeof(wifiSSID));
  if (server.hasArg("pass") && server.arg("pass").length() > 0)
    strlcpy(wifiPass, server.arg("pass").c_str(), sizeof(wifiPass));
  netSettings = next;
  saveSettings();
  sendJsonMessage(200, true, "Network settings saved. Device is restarting.", true);
  scheduleRestart(1500);
}

static void handleSaveWifiLegacy() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) {
    sendJsonMessage(400, false, "WiFi network name must be 1 to 32 characters.");
    return;
  }
  strlcpy(wifiSSID, ssid.c_str(), sizeof(wifiSSID));
  if (server.hasArg("pass") && server.arg("pass").length() > 0)
    strlcpy(wifiPass, server.arg("pass").c_str(), sizeof(wifiPass));
  saveSettings();
  sendJsonMessage(200, true, "WiFi settings saved. Device is restarting.", true);
  scheduleRestart(1500);
}

// Post-mortem breadcrumb for the silent-hang investigation: after the task
// watchdog (see main.cpp) fires, the next boot reports "task_wdt"/"panic"
// here, distinguishing a wedged loop task from a plain power cycle.
static const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

static void handleApiStatus() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"]   = BOARD_VARIANT;
  doc["ip"]      = WiFi.localIP().toString();
  doc["pc_online"] = pcData.online;
  doc["pc_status"] = pcData.status;
  // "style" is the derived legacy int, kept because the companion and older
  // tooling read it. face_name is the one that can name all twelve faces.
  doc["style"]     = displayStyle;
  doc["face_name"] = faceNameFor(displayLayout, displaySurface, duoRowStyle);
  doc["metric_page"] = activeMetricPage;
  doc["metric_pages"] = METRIC_PAGE_COUNT;
  doc["metric_pages_bound"] = metricPageCountBound();
  switch (clockFace) {
    case CLOCK_FACE_BREAKOUT: doc["clock_face"] = "breakout"; break;
    case CLOCK_FACE_RUNNER:   doc["clock_face"] = "runner"; break;
    default:                  doc["clock_face"] = "standard"; break;
  }
  doc["backlight"] = currentBacklightLevel();
  doc["night_active"] = nightBrightnessActive();
  doc["offline_sleeping"] = offlineDisplaySleeping();
  doc["time_valid"] = backlightTimeValid();
  doc["manual_display_off"] = manualDisplayOff();
  doc["touch_supported"] = touchInputSupported();
  doc["touch_enabled"] = touchSettings.enabled != 0;
  doc["touch_pin_valid"] = touchPinValid();
  doc["touch_pressed"] = touchInputPressed();
  doc["touch_last_action"] = touchLastAction();
  doc["touch_events"] = touchEventCount();
  doc["touch_last_ms"] = touchLastEventMs();
  doc["led_enabled"] = ledSettings.enabled != 0;
  doc["led_pin_valid"] = ledPinValid();
  doc["led_duty"] = ledCurrentDuty();
  doc["spark_sprite"] = sparkSpriteActive();
  doc["spark_fails"]  = sparkSpriteFails();
  doc["frame_us"]     = monitorFrameUs();
  doc["frame_max_us"] = monitorFrameMaxUs();
  doc["raw_n_changes"]      = rawNChanges();
  doc["raw_status_changes"] = rawStatusChanges();
  doc["relayouts"]          = acceptedRelayouts();
  doc["status_flips"]       = acceptedStatusFlips();
  doc["free_heap"]    = ESP.getFreeHeap();
  doc["max_block"]    = ESP.getMaxAllocHeap();
  doc["reset_reason"] = resetReasonName();
  doc["wifi_disconnects"]     = wifiDisconnectCount();
  doc["wifi_last_disc_reason"] = wifiLastDisconnectReason();
  doc["metric_count"] = pcData.count;
  doc["timestamp"] = pcData.timestamp;
  doc["hostname"] = netSettings.hostname;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["uptime_seconds"] = millis() / 1000UL;
  doc["ap_mode"] = isAPMode();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Screenshot (GET /screen.bmp)
//  The ST7789 panels here are write-only (readable=false, MISO unwired), so a
//  GRAM readback is impossible. Instead the current screen is re-rendered into
//  an offscreen 8-bit (RGB332) sprite by retargeting tft_ptr - the same
//  mechanism the JC3248W535 framebuffer uses - and streamed as a 24-bit BMP.
//  8-bit keeps the capture buffer at w*h bytes (57.6 KB on 240x240), which
//  fits the C3/CYD heap; colors are quantized but layout-faithful.
// ---------------------------------------------------------------------------
static void handleScreenshot() {
  lgfx::LovyanGFX* panel = tft_ptr;
  const int16_t w = (int16_t)panel->width();
  const int16_t h = (int16_t)panel->height();

  // Persistent capture sprite: allocated once and KEPT. Re-allocating per
  // request made the depth nondeterministic (16-bit when the heap happened to
  // have a 115 KB hole, 8-bit RGB332 otherwise), so consecutive previews
  // flipped between true color and a green-shifted tint - the "graphs change
  // colors" report. Prefer 16-bit, fall back to 8-bit, then stick with it.
  static lgfx::LGFX_Sprite spr;
  static int16_t capW = 0, capH = 0;
  static int8_t capDepth = 0;      // 0 = not probed yet, else 16 or 8
  static bool capPsram = false;
  if (capW != w || capH != h) {
    spr.deleteSprite();
    capDepth = 0;
  }
  // The DEPTH decision is what has to be sticky (a per-request probe made the
  // preview flip between true colour and the 8-bit green tint); the BUFFER does
  // not. On a part without PSRAM a 240x240 sprite is 57.6 KB of internal heap,
  // and holding it for the rest of uptime left the web server unable to
  // allocate for new connections - portal saves then failed in the browser with
  // "Failed to fetch" while curl still squeezed through. So: probe once, keep
  // the answer, hand the memory back after every capture.
  if (!spr.getBuffer()) {
    bool ok = false;
    if (capDepth != 0) {
      spr.setPsram(capPsram);
      spr.setColorDepth(capDepth);
      ok = spr.createSprite(w, h);
    }
    if (!ok && capDepth == 0) {
      // PSRAM FIRST on any board that has it. Rendering into PSRAM is slower,
      // which this path does not care about, and it keeps internal heap for
      // WiFi/HTTP - so a PSRAM buffer is worth KEEPING between requests, while
      // an internal one is not. Runtime psramFound() rather than
      // BOARD_HAS_PSRAM so a board whose PSRAM did not come up still gets the
      // internal fallbacks.
      if (psramFound()) {
        spr.setPsram(true);
        spr.setColorDepth(16);
        ok = spr.createSprite(w, h);
        if (ok) { capDepth = 16; capPsram = true; }
      }
      if (!ok) {
        // No PSRAM (e.g. the C3) or the PSRAM allocation failed. 240x240 fits
        // internal at 16-bit (115 KB) or 8-bit (57.6 KB). A 320x480 frame needs
        // 307 KB / 154 KB contiguous and will not fit either, so such a board
        // legitimately reports 503.
        spr.setPsram(false);
        spr.setColorDepth(16);
        ok = spr.createSprite(w, h);
        if (ok) { capDepth = 16; capPsram = false; }
        if (!ok) {
          spr.setColorDepth(8);
          ok = spr.createSprite(w, h);
          if (ok) { capDepth = 8; capPsram = false; }
        }
      }
    }
    if (!ok) {
      spr.setPsram(false);
      capW = capH = 0;
      capDepth = 0;
      server.send(503, "text/plain", "Not enough RAM for capture");
      return;
    }
    capW = w;
    capH = h;
  }
  spr.fillSprite(dispSettings.bgColor);

  // Render one full frame into the sprite. The font cache and the gauge text
  // caches describe the panel, not the sprite - reset both around the swap.
  tft_ptr = &spr;
  resetFontCache();
  setCaptureRender(true);
  forceDisplayRedraw();
  updateDisplay();
  setCaptureRender(false);
  tft_ptr = panel;
  resetFontCache();
  forceDisplayRedraw();   // panel repaints (opaquely, no blanking) next loop tick

  const uint32_t rowBytes = (uint32_t)w * 3;   // w is a multiple of 4 on all boards
  const uint32_t imgBytes = rowBytes * (uint32_t)h;
  const uint32_t fileBytes = 54 + imgBytes;

  uint8_t* row = (uint8_t*)malloc(rowBytes);
  if (!row) {
    if (!capPsram) spr.deleteSprite();
    server.send(503, "text/plain", "Not enough RAM for row buffer");
    return;
  }

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = fileBytes & 0xFF; hdr[3] = (fileBytes >> 8) & 0xFF;
  hdr[4] = (fileBytes >> 16) & 0xFF; hdr[5] = (fileBytes >> 24) & 0xFF;
  hdr[10] = 54;                       // pixel data offset
  hdr[14] = 40;                       // BITMAPINFOHEADER
  hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
  hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
  hdr[26] = 1;                        // planes
  hdr[28] = 24;                       // bpp
  hdr[34] = imgBytes & 0xFF; hdr[35] = (imgBytes >> 8) & 0xFF;
  hdr[36] = (imgBytes >> 16) & 0xFF; hdr[37] = (imgBytes >> 24) & 0xFF;

  server.setContentLength(fileBytes);
  server.send(200, "image/bmp", "");
  WiFiClient client = server.client();

  // Abort on the first failed/stalled write. The old raw client.write()
  // ignored failures, so a browser that navigated away mid-stream (the Colors
  // page preview does this constantly) left the loop pushing the remaining
  // rows into a dead socket - each write burning its full TCP timeout while
  // the whole device sat frozen inside this handler.
  bool ok = writeClientAll(client, hdr, sizeof(hdr));

  // BMP rows run bottom-up. readPixel() returns RGB565 regardless of the
  // sprite's storage depth, so one loop serves both capture modes.
  for (int16_t y = h - 1; y >= 0 && ok; y--) {
    for (int16_t x = 0; x < w; x++) {
      const uint16_t c = spr.readPixel(x, y);
      row[x * 3 + 0] = (uint8_t)((((c) & 0x1F) * 255) / 31);        // B (5 bit)
      row[x * 3 + 1] = (uint8_t)((((c >> 5) & 0x3F) * 255) / 63);   // G (6 bit)
      row[x * 3 + 2] = (uint8_t)((((c >> 11) & 0x1F) * 255) / 31);  // R (5 bit)
    }
    ok = writeClientAll(client, row, rowBytes);
    esp_task_wdt_reset();   // a slow-but-alive client may take >30 s total
  }
  if (!ok) client.stop();

  free(row);
  // Internal-heap capture buffers go straight back to the allocator; only a
  // PSRAM one is worth keeping. The remembered depth means the next request
  // still renders identically.
  if (!capPsram) spr.deleteSprite();
}

#if defined(BOARD_IS_JC3248W535)
// ---------------------------------------------------------------------------
//  Live frame (GET /screen_live.bmp, JC3248W535 only)
//  Streams the framebuffer the deck pipeline last sent to the panel, i.e. what
//  is really on the glass (deck pages included), unlike /screen.bmp, which
//  re-renders the metric face. LVGL runs in this same loop task, so the
//  buffer cannot change while the handler streams it.
// ---------------------------------------------------------------------------
static void handleLiveScreenshot() {
  const uint16_t* fb = deck::panelLatestLvFrame();
  if (!fb) {
    server.send(503, "text/plain", "No frame yet");
    return;
  }
  const int w = deck::logicalW();
  const int h = deck::logicalH();
  const uint32_t rowBytes = (uint32_t)w * 3;
  const uint32_t imgBytes = rowBytes * (uint32_t)h;
  const uint32_t fileBytes = 54 + imgBytes;
  uint8_t* row = (uint8_t*)malloc(rowBytes);
  if (!row) {
    server.send(503, "text/plain", "Not enough RAM for row buffer");
    return;
  }
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = fileBytes & 0xFF; hdr[3] = (fileBytes >> 8) & 0xFF;
  hdr[4] = (fileBytes >> 16) & 0xFF; hdr[5] = (fileBytes >> 24) & 0xFF;
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
  hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
  hdr[26] = 1;
  hdr[28] = 24;
  hdr[34] = imgBytes & 0xFF; hdr[35] = (imgBytes >> 8) & 0xFF;
  hdr[36] = (imgBytes >> 16) & 0xFF; hdr[37] = (imgBytes >> 24) & 0xFF;
  server.setContentLength(fileBytes);
  server.send(200, "image/bmp", "");
  WiFiClient client = server.client();
  bool ok = writeClientAll(client, hdr, sizeof(hdr));
  for (int y = h - 1; y >= 0 && ok; y--) {
    const uint16_t* src = fb + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      const uint16_t c = src[x];
      row[x * 3 + 0] = (uint8_t)(((c & 0x1F) * 255) / 31);
      row[x * 3 + 1] = (uint8_t)((((c >> 5) & 0x3F) * 255) / 63);
      row[x * 3 + 2] = (uint8_t)((((c >> 11) & 0x1F) * 255) / 31);
    }
    ok = writeClientAll(client, row, rowBytes);
    esp_task_wdt_reset();
  }
  if (!ok) client.stop();
  free(row);
}

// POST /api/deck/swipe?dir=1|-1 - bench hook, one full page slide.
static void handleDeckSwipe() {
  const int dir = server.hasArg("dir") ? server.arg("dir").toInt() : 1;
  const bool ok = deckTestSwipe(dir);
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Swipe started" : "Swipe not possible now");
}

// --- Deck page and its API ----------------------------------------------------
static void handleDeckPage() {
  streamAsset(DECK_HTML, sizeof(DECK_HTML) - 1, "text/html; charset=utf-8", "no-cache");
}

// GET /api/deck -> {"source": "...", "deck": <deck.json>}
static void handleDeckGet() {
  String text;
  deck::deckConfigText(text);
  String out;
  out.reserve(text.length() + 48);
  out += "{\"source\":\"";
  out += "device";
  out += "\",\"deck\":";
  out += text;
  out += "}";
  server.send(200, "application/json", out);
}

// POST /api/deck (JSON body) - validate, store, rebuild the pages.
static void handleDeckPost() {
  String err;
  if (!server.hasArg("plain")) {
    sendJsonMessage(400, false, "Empty body");
    return;
  }
  if (!deckApplyConfig(server.arg("plain"), err)) {
    sendJsonMessage(400, false, err.c_str());
    return;
  }
  sendJsonMessage(200, true, "Deck saved");
}

// GET /api/ha/status - never includes the token.
static void handleHaStatus() {
  JsonDocument doc;
  doc["url"] = deck::haUrl();
  doc["has_token"] = deck::haHasToken();
  doc["link"] = deck::haLinkName(deck::haLink());
  doc["error"] = deck::haLastError();
  doc["version"] = deck::haServerVersion();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/ha/config (url, token) - an empty token keeps the stored one.
static void handleHaConfig() {
  const String url = server.arg("url");
  if (!url.length()) {
    sendJsonMessage(400, false, "Address is required");
    return;
  }
  deck::haSetCredentials(url, server.arg("token"));
  sendJsonMessage(200, true, "Saved, connecting");
}

static void handleHaForget() {
  deck::haClearCredentials();
  sendJsonMessage(200, true, "Removed");
}

// GET /api/ha/entities[?refresh=1] - picker list, cached on flash.
static void handleHaEntities() {
  String out, err;
  const bool refresh = server.arg("refresh") == "1";
  if (!refresh && deck::deckFsReady() && LittleFS.exists(deck::haEntityListPath())) {
    // Straight from flash: loading 160 KB into a String first took seconds.
    File f = LittleFS.open(deck::haEntityListPath(), "r");
    if (f) {
      server.streamFile(f, "application/json");
      f.close();
      return;
    }
  }
  if (!deck::haFetchEntityList(out, refresh, err)) {
    sendJsonMessage(502, false, err.c_str());
    return;
  }
  server.send(200, "application/json", out);
}

// POST /api/deck/panel?open=1|0 - bench hook, quick panel slide.
static void handleDeckPanel() {
  const bool open = !server.hasArg("open") || server.arg("open") != "0";
  const bool ok = deckTestPanel(open);
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Panel slide started" : "Panel slide not possible now");
}
#endif

// ---------------------------------------------------------------------------
//  OTA (multipart upload to /ota/upload)
// ---------------------------------------------------------------------------
static void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaError = "";
    otaInProgress = true;
    otaFirstChunk = true;
    setScreenState(SCREEN_OTA_UPDATE);
    updateDisplay();
    flushFrame();
    Serial.printf("OTA: start, file=%s\n", upload.filename.c_str());

    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
      otaError = "No OTA partition found";
      otaInProgress = false;
      return;
    }
    if (!Update.begin(partition->size)) {
      otaError = "OTA begin failed: " + String(Update.errorString());
      otaInProgress = false;
      return;
    }
    if (server.hasHeader("X-MD5")) {
      String md5 = server.header("X-MD5");
      if (md5.length() == 32) Update.setMD5(md5.c_str());
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaInProgress) return;
    // The whole multipart upload is parsed inside ONE handleClient() pass, so
    // loop() cannot feed the task watchdog for the duration - feed it per
    // received chunk instead (a stalled chunk read is bounded by the HTTP
    // client timeout, well under the WDT period).
    esp_task_wdt_reset();
    // Validate the ESP32 image magic byte on the first chunk.
    if (otaFirstChunk && upload.currentSize > 0) {
      otaFirstChunk = false;
      if (upload.buf[0] != 0xE9) {
        otaError = "Invalid firmware file";
        Update.abort();
        otaInProgress = false;
        return;
      }
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaError = Update.errorString();
      Update.abort();
      otaInProgress = false;
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaInProgress) return;
    if (Update.end(true)) {
      Serial.printf("OTA: success, %u bytes\n", upload.totalSize);
    } else {
      otaError = Update.errorString();
      Serial.printf("OTA: end failed: %s\n", otaError.c_str());
    }
    otaInProgress = false;

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    Serial.println("OTA: aborted");
  }
}

static void handleOtaFinish() {
  if (otaError.length() > 0) {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"" + otaError + "\"}");
    otaError = "";
    return;
  }
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"message\":\"Update successful. Restarting...\"}");
  scheduleRestart(1500);
}

// ---------------------------------------------------------------------------
//  Captive portal
// ---------------------------------------------------------------------------
static void handleCaptiveDetect() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

static void handleNotFound() {
  if (isAPMode()) {
    handleCaptiveDetect();
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

// ---------------------------------------------------------------------------
//  Init & handle
// ---------------------------------------------------------------------------
void initWebServer() {
  server.on("/generate_204", HTTP_GET, handleCaptiveDetect);
  server.on("/gen_204", HTTP_GET, handleCaptiveDetect);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveDetect);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveDetect);
  server.on("/canonical.html", HTTP_GET, handleCaptiveDetect);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/portal.css", HTTP_GET, handlePortalCss);
  server.on("/portal-layout.css", HTTP_GET, handlePortalLayoutCss);
  server.on("/portal.js", HTTP_GET, handlePortalJs);
  server.on("/save/wifi", HTTP_POST, handleSaveWifiLegacy);
  server.on("/save/display", HTTP_POST, handleSaveDisplay);
  server.on("/save/hardware", HTTP_POST, handleSaveHardware);
  server.on("/led/preview", HTTP_POST, handleLedPreview);
  server.on("/glass/preview", HTTP_POST, handleGlassPreview);
  server.on("/save/gauges", HTTP_POST, handleSaveGauges);
  server.on("/save/colors", HTTP_POST, handleSaveColors);
  server.on("/save/clock", HTTP_POST, handleSaveClock);
  server.on("/save/network", HTTP_POST, handleSaveNetwork);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config/export", HTTP_GET, handleConfigExport);
  server.on("/api/config/import", HTTP_POST, handleConfigImport);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/screen.bmp", HTTP_GET, handleScreenshot);
#if defined(BOARD_IS_JC3248W535)
  server.on("/screen_live.bmp", HTTP_GET, handleLiveScreenshot);
  server.on("/api/deck/swipe", HTTP_POST, handleDeckSwipe);
  server.on("/api/deck/panel", HTTP_POST, handleDeckPanel);
  server.on("/deck", HTTP_GET, handleDeckPage);
  server.on("/api/deck", HTTP_GET, handleDeckGet);
  server.on("/api/deck", HTTP_POST, handleDeckPost);
  server.on("/api/ha/status", HTTP_GET, handleHaStatus);
  server.on("/api/ha/config", HTTP_POST, handleHaConfig);
  server.on("/api/ha/forget", HTTP_POST, handleHaForget);
  server.on("/api/ha/entities", HTTP_GET, handleHaEntities);
#endif
  server.on("/ota/upload", HTTP_POST, handleOtaFinish, handleOtaUpload);

  const char* headers[] = { "X-MD5" };
  server.collectHeaders(headers, 1);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on :80");
}

void handleWebServer() {
  server.handleClient();
  if (restartAt != 0 && millis() >= restartAt) {
    Serial.println("Rebooting...");
    Serial.flush();
    if (eraseWifiAtRestart) {
      WiFi.disconnect(true, true);
      delay(50);
    }
    ESP.restart();
  }
}
