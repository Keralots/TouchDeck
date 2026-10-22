// TouchDeck web portal, JSON APIs, live screen capture, OTA update, and
// captive-portal redirects. Static UI assets are streamed from PROGMEM in
// bounded chunks so the full page is never assembled in heap.
#include "web_server.h"
#include "settings.h"
#include "wifi_manager.h"
#include "display_ui.h"
#include "pc_metrics.h"
#include "led.h"
#include "config.h"
#include "web_assets.h"
#include "deck_ui.h"
#include "deck_printers.h"
#include "deck_config.h"
#include "deck_ha.h"
#include "deck_http.h"
#include "deck_mqtt.h"
#include "deck_icons.h"
#include "deck_wall.h"
#include "deck_daily.h"
#include "deck_sys.h"
#include "psram_json.h"
#include <LittleFS.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
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

static void streamAsset(const void* data, size_t len, const char* contentType,
                        const char* cacheControl, bool gzipped = false) {
  static const size_t CHUNK_SIZE = 2048;
  uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
  if (!chunk) {
    server.send(503, "text/plain", "Out of memory");
    return;
  }

  server.sendHeader("Cache-Control", cacheControl);
  if (gzipped) server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(len);
  server.send(200, contentType, "");
  WiFiClient client = server.client();

  bool ok = true;
  for (size_t offset = 0; offset < len && ok; offset += CHUNK_SIZE) {
    size_t count = min(CHUNK_SIZE, len - offset);
    memcpy_P(chunk, (const uint8_t*)data + offset, count);
    ok = writeClientAll(client, chunk, count);
    esp_task_wdt_reset();   // large asset + slow client can outlast the WDT
    delay(0);
  }
  free(chunk);
  if (!ok) client.stop();
}

static void handleRoot() {
  streamAsset(WEB_INDEX_HTML, sizeof(WEB_INDEX_HTML), "text/html",
              "no-cache, no-store, must-revalidate", true);
}

static void handlePortalCss() {
  streamAsset(WEB_PORTAL_CSS, sizeof(WEB_PORTAL_CSS), "text/css",
              "public, max-age=31536000, immutable", true);
}

static void handlePortalLayoutCss() {
  streamAsset(WEB_PORTAL_LAYOUT_CSS, sizeof(WEB_PORTAL_LAYOUT_CSS), "text/css",
              "public, max-age=31536000, immutable", true);
}

static void handlePortalJs() {
  streamAsset(WEB_PORTAL_JS, sizeof(WEB_PORTAL_JS), "application/javascript",
              "public, max-age=31536000, immutable", true);
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

static void presenceJson(JsonObject out) {
  out["sleep"] = presenceSettings.sleepMin;
  out["dim"] = presenceSettings.dimPct;
  out["pc"] = presenceSettings.pc;
  JsonArray ids = out["entities"].to<JsonArray>();
  for (const String& id : presenceSettings.entities)
    if (id.length()) ids.add(id);
}

// An HA entity id as typed into the portal: domain.object, lowercase letters, digits, '_'.
static bool validEntityId(const String& id) {
  const int dot = id.indexOf('.');
  if (id.length() > 80 || dot <= 0 || dot == (int)id.length() - 1 || id.indexOf('.', dot + 1) >= 0) return false;
  for (size_t i = 0; i < id.length(); i++) {
    const char c = id[i];
    if (c != '.' && c != '_' && !isDigit(c) && !(c >= 'a' && c <= 'z')) return false;
  }
  return true;
}

static void handleApiConfig() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"] = BOARD_VARIANT;
  doc["ip"] = WiFi.localIP().toString();
  doc["deck"] = true;

  JsonObject display = doc["display"].to<JsonObject>();
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["theme"] = dispSettings.theme;
  display["glassTint"] = dispSettings.glassTint;
  display["glassBlur"] = dispSettings.glassBlur;
  display["glassDim"] = dispSettings.glassDim;
  display["wallEvery"] = dispSettings.wallEvery;
  display["wallOrder"] = dispSettings.wallOrder;
  display["wallDaily"] = dispSettings.wallDaily;
  display["dailyHour"] = dispSettings.dailyHour;
  display["dailyOrient"] = dispSettings.dailyOrient;
  display["dailyCount"] = dispSettings.dailyCount;
  display["glassOn"] = dispSettings.glassOn;
  display["glassAutoText"] = dispSettings.glassAutoText;
  display["smoothCharts"] = dispSettings.smoothCharts;

  JsonObject backlight = doc["backlight"].to<JsonObject>();
  backlight["nightEnabled"] = backlightSettings.nightEnabled != 0;
  backlight["nightBrightness"] = backlightSettings.nightBrightness;
  char timeBuf[6];
  formatMinuteOfDay(backlightSettings.nightStartMinute, timeBuf, sizeof(timeBuf));
  backlight["nightStart"] = timeBuf;
  formatMinuteOfDay(backlightSettings.nightEndMinute, timeBuf, sizeof(timeBuf));
  backlight["nightEnd"] = timeBuf;

  JsonObject led = doc["led"].to<JsonObject>();
  led["enabled"] = ledSettings.enabled != 0;
  led["pin"] = ledSettings.pin;
  led["pinValid"] = ledPinValid();
  led["brightness"] = ledSettings.brightness;
  led["nightEnabled"] = ledSettings.nightEnabled != 0;
  led["nightBrightness"] = ledSettings.nightBrightness;
  formatMinuteOfDay(ledSettings.nightStartMinute, timeBuf, sizeof(timeBuf));
  led["nightStart"] = timeBuf;
  formatMinuteOfDay(ledSettings.nightEndMinute, timeBuf, sizeof(timeBuf));
  led["nightEnd"] = timeBuf;
  led["followDisplay"] = ledSettings.followDisplay != 0;
  led["offlineOff"] = ledSettings.offlineOff != 0;
  led["entity"] = deckLedEntity();
  doc["advanced"]["printers"] = advSettings.printers;
  mqttConfigJson(doc["mqtt"].to<JsonObject>(), true);
  presenceJson(doc["presence"].to<JsonObject>());
  JsonArray ledPins = led["allowedPins"].to<JsonArray>();
  for (uint8_t pin = 0; pin <= 48; pin++) {
    if (ledPinAllowed(pin)) ledPins.add(pin);
  }

  JsonObject clock = doc["clock"].to<JsonObject>();
  clock["use24h"] = netSettings.use24h;
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

  // "metrics": the default PC's (older portals), "pcs": every known PC.
  JsonArray metrics = doc["metrics"].to<JsonArray>();
  JsonArray pcs = doc["pcs"].to<JsonArray>();
  for (int p = 0; p < MAX_PCS; p++) {
    const PcSource& src = pcSources[p];
    if (!src.used) continue;
    JsonObject pc = pcs.add<JsonObject>();
    pc["id"] = src.id;
    pc["name"] = src.name;
    pc["online"] = src.data.online;
    pc["paired"] = src.paired;
    pc["control"] = src.data.online && src.cmdPort != 0;
    if (src.data.online) pc["ip"] = src.ip.toString();
    JsonArray list = pc["metrics"].to<JsonArray>();
    for (uint8_t i = 0; i < src.data.count; i++) {
      const PcMetric& m = src.data.metrics[i];
      for (JsonArray a : {list, p == 0 ? metrics : JsonArray()}) {
        if (a.isNull()) continue;
        JsonObject metric = a.add<JsonObject>();
        metric["id"] = m.id;
        metric["name"] = m.name;
        metric["value"] = m.value;
        metric["unit"] = m.unit;
      }
    }
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Portable configuration backup
//
//  WiFi credentials and the Home Assistant token are never included, so a
//  backup can be shared, and a restore never disconnects the device.
// ---------------------------------------------------------------------------
static const char* CONFIG_BACKUP_FORMAT = "touchdeck-config";
static const uint8_t CONFIG_BACKUP_SCHEMA = 1;
// The deck layout rides along, so this is far more than the settings alone.
static const size_t CONFIG_IMPORT_MAX_BYTES = 96 * 1024;

static void handleConfigExport() {
  JsonDocument doc(psramJson());
  doc["format"] = CONFIG_BACKUP_FORMAT;
  doc["schema"] = CONFIG_BACKUP_SCHEMA;
  doc["product"] = PRODUCT_NAME;
  doc["firmware"] = FW_VERSION;
  doc["board"] = BOARD_VARIANT;
  doc["wifiCredentialsIncluded"] = false;

  JsonObject display = doc["display"].to<JsonObject>();
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["theme"] = dispSettings.theme;
  display["glassTint"] = dispSettings.glassTint;
  display["glassBlur"] = dispSettings.glassBlur;
  display["glassDim"] = dispSettings.glassDim;
  display["wallEvery"] = dispSettings.wallEvery;
  display["wallOrder"] = dispSettings.wallOrder;
  display["wallDaily"] = dispSettings.wallDaily;
  display["dailyHour"] = dispSettings.dailyHour;
  display["dailyOrient"] = dispSettings.dailyOrient;
  display["dailyCount"] = dispSettings.dailyCount;
  display["glassOn"] = dispSettings.glassOn;
  display["glassAutoText"] = dispSettings.glassAutoText;
  display["smoothCharts"] = dispSettings.smoothCharts;

  JsonObject backlight = doc["backlight"].to<JsonObject>();
  backlight["nightEnabled"] = backlightSettings.nightEnabled != 0;
  backlight["nightBrightness"] = backlightSettings.nightBrightness;
  backlight["nightStartMinute"] = backlightSettings.nightStartMinute;
  backlight["nightEndMinute"] = backlightSettings.nightEndMinute;

  JsonObject led = doc["led"].to<JsonObject>();
  led["enabled"] = ledSettings.enabled != 0;
  led["pin"] = ledSettings.pin;
  led["brightness"] = ledSettings.brightness;
  led["nightEnabled"] = ledSettings.nightEnabled != 0;
  led["nightBrightness"] = ledSettings.nightBrightness;
  led["nightStartMinute"] = ledSettings.nightStartMinute;
  led["nightEndMinute"] = ledSettings.nightEndMinute;
  led["followDisplay"] = ledSettings.followDisplay != 0;
  led["offlineOff"] = ledSettings.offlineOff != 0;
  doc["advanced"]["printers"] = advSettings.printers;
  mqttConfigJson(doc["mqtt"].to<JsonObject>(), false);
  presenceJson(doc["presence"].to<JsonObject>());

  JsonObject clock = doc["clock"].to<JsonObject>();
  clock["use24h"] = netSettings.use24h;
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

  String out;
  serializeJson(doc, out);
  // deckConfigText() returns validated JSON, so splice it in as text: a parsed
  // copy of a large deck emptied the internal heap and WiFi aborted.
  String deckText;
  deck::deckConfigText(deckText);
  out.remove(out.length() - 1);
  out.reserve(out.length() + deckText.length() + 48);
  out += ",\"deck\":{\"header\":";
  out += deckHeaderVisible() ? "true" : "false";
  out += ",\"config\":";
  out += deckText;
  out += "}}";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Content-Disposition",
                    "attachment; filename=touchdeck-config.json");
  server.send(200, "application/json", out);
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

static void handleConfigImport() {
  const String& body = server.arg("plain");
  if (body.length() == 0) {
    sendJsonMessage(400, false, "Choose a TouchDeck backup file.");
    return;
  }
  if (body.length() > CONFIG_IMPORT_MAX_BYTES) {
    sendJsonMessage(413, false, "The configuration backup is too large.");
    return;
  }

  JsonDocument doc(psramJson());
  if (deserializeJson(doc, body)) {
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
      strcmp(format, CONFIG_BACKUP_FORMAT) != 0) {
    sendJsonMessage(400, false, "This is not a TouchDeck configuration backup.");
    return;
  }
  long integer = 0;
  if (!readBackupInteger(root, "schema", 1, CONFIG_BACKUP_SCHEMA, integer, error, "schema")) {
    sendJsonMessage(400, false, "This backup schema is not supported by this firmware.");
    return;
  }

  JsonObjectConst display = root["display"].as<JsonObjectConst>();
  JsonObjectConst backlight = root["backlight"].as<JsonObjectConst>();
  JsonObjectConst led = root["led"].as<JsonObjectConst>();
  JsonObjectConst clock = root["clock"].as<JsonObjectConst>();
  JsonObjectConst network = root["network"].as<JsonObjectConst>();
  JsonObjectConst deckIn = root["deck"].as<JsonObjectConst>();
  if (display.isNull() || backlight.isNull() || led.isNull() || clock.isNull() ||
      network.isNull() || deckIn.isNull()) {
    sendJsonMessage(400, false, "The backup is incomplete.");
    return;
  }

  DisplaySettings nextDisplay = dispSettings;
  NetworkSettings nextNetwork = netSettings;
  BacklightSettings nextBacklight = backlightSettings;
  LedSettings nextLed = ledSettings;
  uint8_t nextBrightness = brightness;
  bool nextHeader = deckHeaderVisible();
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
#define READ_BOOL(section, key, target) \
  do { \
    if (!readBackupBool(section, key, flag, error, #section "." key)) { \
      sendJsonMessage(400, false, error.c_str()); \
      return; \
    } \
    target = flag; \
  } while (0)

  READ_INT(display, "rotation", 0, 3, nextDisplay.rotation, uint8_t);
  READ_INT(display, "brightness", 0, 255, nextBrightness, uint8_t);
  READ_INT(display, "tempScale", 1, 500, nextDisplay.tempScaleMax, uint16_t);
  READ_INT(display, "powerScale", 1, 65535, nextDisplay.powerScaleW, uint16_t);
  READ_INT(display, "warnThreshold", 0, 100, nextDisplay.warnThresholdPct, uint8_t);
  READ_INT(display, "theme", 0, 2, nextDisplay.theme, uint8_t);
  // Backups from before the Glass sliders restore them to their defaults.
  DisplaySettings displayDefaults;
  defaultDisplaySettings(displayDefaults);
  nextDisplay.glassTint = displayDefaults.glassTint;
  nextDisplay.glassBlur = displayDefaults.glassBlur;
  nextDisplay.glassDim = displayDefaults.glassDim;
  nextDisplay.wallEvery = displayDefaults.wallEvery;
  nextDisplay.wallOrder = displayDefaults.wallOrder;
  nextDisplay.wallDaily = displayDefaults.wallDaily;
  nextDisplay.dailyHour = displayDefaults.dailyHour;
  nextDisplay.dailyOrient = displayDefaults.dailyOrient;
  nextDisplay.dailyCount = displayDefaults.dailyCount;
  nextDisplay.glassOn = displayDefaults.glassOn;
  nextDisplay.glassAutoText = displayDefaults.glassAutoText;
  nextDisplay.smoothCharts = displayDefaults.smoothCharts;
  if (!display["glassTint"].isNull()) READ_INT(display, "glassTint", 0, 90, nextDisplay.glassTint, uint8_t);
  if (!display["glassBlur"].isNull()) READ_INT(display, "glassBlur", 0, 24, nextDisplay.glassBlur, uint8_t);
  if (!display["glassDim"].isNull()) READ_INT(display, "glassDim", 0, 60, nextDisplay.glassDim, uint8_t);
  if (!display["wallEvery"].isNull()) READ_INT(display, "wallEvery", 0, 60, nextDisplay.wallEvery, uint8_t);
  if (!display["wallOrder"].isNull()) READ_INT(display, "wallOrder", 0, 1, nextDisplay.wallOrder, uint8_t);
  if (!display["wallDaily"].isNull()) READ_BOOL(display, "wallDaily", nextDisplay.wallDaily);
  if (!display["dailyHour"].isNull()) READ_INT(display, "dailyHour", 0, 23, nextDisplay.dailyHour, uint8_t);
  if (!display["dailyOrient"].isNull()) READ_INT(display, "dailyOrient", 0, 2, nextDisplay.dailyOrient, uint8_t);
  if (!display["dailyCount"].isNull())
    READ_INT(display, "dailyCount", 1, DAILY_MAX_COUNT, nextDisplay.dailyCount, uint8_t);
  if (!validWallEvery(nextDisplay.wallEvery)) {
    sendJsonMessage(400, false, "invalid wallEvery");
    return;
  }
  if (!display["glassOn"].isNull()) READ_INT(display, "glassOn", 20, 100, nextDisplay.glassOn, uint8_t);
  if (!display["glassAutoText"].isNull()) READ_BOOL(display, "glassAutoText", nextDisplay.glassAutoText);
  if (!display["smoothCharts"].isNull()) READ_BOOL(display, "smoothCharts", nextDisplay.smoothCharts);

  READ_BOOL(backlight, "nightEnabled", nextBacklight.nightEnabled);
  READ_INT(backlight, "nightBrightness", 0, 255, nextBacklight.nightBrightness, uint8_t);
  READ_INT(backlight, "nightStartMinute", 0, 1439, nextBacklight.nightStartMinute, uint16_t);
  READ_INT(backlight, "nightEndMinute", 0, 1439, nextBacklight.nightEndMinute, uint16_t);

  READ_BOOL(led, "enabled", nextLed.enabled);
  READ_INT(led, "pin", 0, 48, nextLed.pin, uint8_t);
  READ_INT(led, "brightness", 0, 255, nextLed.brightness, uint8_t);
  READ_BOOL(led, "nightEnabled", nextLed.nightEnabled);
  READ_INT(led, "nightBrightness", 0, 255, nextLed.nightBrightness, uint8_t);
  if (!led["nightStartMinute"].isNull()) {
    READ_INT(led, "nightStartMinute", 0, 1439, nextLed.nightStartMinute, uint16_t);
    READ_INT(led, "nightEndMinute", 0, 1439, nextLed.nightEndMinute, uint16_t);
  } else {
    // Older backup: the LED dimmed only inside the backlight's schedule.
    nextLed.nightStartMinute = nextBacklight.nightStartMinute;
    nextLed.nightEndMinute = nextBacklight.nightEndMinute;
    if (!nextBacklight.nightEnabled) nextLed.nightEnabled = 0;
  }
  READ_BOOL(led, "followDisplay", nextLed.followDisplay);
  READ_BOOL(led, "offlineOff", nextLed.offlineOff);
  if (nextLed.enabled && !ledPinAllowed(nextLed.pin)) {
    sendJsonMessage(400, false, "The backup uses an LED GPIO this board does not allow.");
    return;
  }

  READ_BOOL(clock, "use24h", nextNetwork.use24h);
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

  READ_BOOL(deckIn, "header", nextHeader);
  bool nextPrinters = false;  // backups from before the Advanced page: the default
  JsonObjectConst advanced = root["advanced"].as<JsonObjectConst>();
  if (!advanced.isNull() && !advanced["printers"].isNull()) READ_BOOL(advanced, "printers", nextPrinters);
  PresenceSettings nextPresence;  // older backups: never sleeps
  JsonObjectConst presence = root["presence"].as<JsonObjectConst>();
  if (!presence.isNull()) {
    READ_INT(presence, "sleep", 0, 1440, nextPresence.sleepMin, uint16_t);
    READ_INT(presence, "dim", 0, 100, nextPresence.dimPct, uint8_t);
    READ_BOOL(presence, "pc", nextPresence.pc);
    size_t n = 0;
    for (JsonVariantConst v : presence["entities"].as<JsonArrayConst>()) {
      const String id = (const char*)(v | "");
      if (n >= MAX_PRESENCE_ENTITIES || !validEntityId(id)) {
        sendJsonMessage(400, false, "invalid presence.entities");
        return;
      }
      nextPresence.entities[n++] = id;
    }
  }
  MqttSettings nextMqtt;
  if (root.containsKey("mqtt")) {
    JsonObjectConst mqtt = root["mqtt"].as<JsonObjectConst>();
    const char* host = nullptr;
    const char* user = nullptr;
    const char* prefix = nullptr;
    if (mqtt.isNull() || !readBackupBool(mqtt, "enabled", nextMqtt.enabled, error, "mqtt.enabled") ||
        !readBackupString(mqtt, "host", 80, host, error, "mqtt.host") ||
        !readBackupInteger(mqtt, "port", 1, 65535, integer, error, "mqtt.port") ||
        !readBackupString(mqtt, "user", 127, user, error, "mqtt.user") ||
        !readBackupString(mqtt, "prefix", 127, prefix, error, "mqtt.prefix")) {
      sendJsonMessage(400, false, error.length() ? error.c_str() : "Invalid MQTT settings");
      return;
    }
    nextMqtt.host = host;
    nextMqtt.port = (uint16_t)integer;
    nextMqtt.user = user;
    nextMqtt.prefix = prefix;
    if (!mqttNormalize(nextMqtt, error)) {
      sendJsonMessage(400, false, error.c_str());
      return;
    }
    const MqttSettings& stored = mqttSettings();
    if (nextMqtt.host == stored.host && nextMqtt.user == stored.user) nextMqtt.pass = stored.pass;
  }
#undef READ_INT
#undef READ_BOOL

  // Validate the deck before anything is applied, so a bad file changes nothing.
  String deckText;
  serializeJson(deckIn["config"], deckText);
  deck::DeckConfig parsed;
  String deckErr;
  if (!deck::deckConfigParse(deckText.c_str(), deckText.length(), parsed, &deckErr)) {
    sendJsonMessage(400, false, ("The deck in the backup is invalid: " + deckErr).c_str());
    return;
  }
  if (!deckApplyConfig(deckText, deckErr)) {
    sendJsonMessage(500, false, ("Could not store the deck: " + deckErr).c_str());
    return;
  }

  if (!mqttSaveSettings(nextMqtt)) {
    sendJsonMessage(500, false, "Could not store MQTT settings");
    return;
  }
  deck::wallInvalidateJobs();
  dispSettings = nextDisplay;
  netSettings = nextNetwork;
  backlightSettings = nextBacklight;
  ledSettings = nextLed;
  brightness = nextBrightness;
  advSettings.printers = nextPrinters;
  presenceSettings = nextPresence;
  saveSettings();
  deckSetHeader(nextHeader);

  sendJsonMessage(200, true,
                  "Configuration restored. WiFi credentials were preserved. Restarting.",
                  true);
  scheduleRestart(1800);
}

// ---------------------------------------------------------------------------
//  Save handlers
// ---------------------------------------------------------------------------
// Screen & light page: brightness, night schedule, sleep, status LED.
static void handleSaveLight() {
  LedSettings nextLed = ledSettings;
  nextLed.enabled = server.hasArg("ledEnabled") ? 1 : 0;
  nextLed.pin = (uint8_t)clampedArg("ledPin", nextLed.pin, 0, 48);
  nextLed.brightness = (uint8_t)clampedArg("ledBrightness", nextLed.brightness, 0, 255);
  nextLed.nightEnabled = server.hasArg("ledNightEnabled") ? 1 : 0;
  nextLed.nightBrightness =
    (uint8_t)clampedArg("ledNightBrightness", nextLed.nightBrightness, 0, 255);
  nextLed.nightStartMinute = minuteOfDayArg("ledNightStart", nextLed.nightStartMinute);
  nextLed.nightEndMinute = minuteOfDayArg("ledNightEnd", nextLed.nightEndMinute);
  nextLed.followDisplay = server.hasArg("ledFollowDisplay") ? 1 : 0;
  nextLed.offlineOff = server.hasArg("ledOfflineOff") ? 1 : 0;
  if (nextLed.enabled && !ledPinAllowed(nextLed.pin)) {
    sendJsonMessage(400, false,
                    "That LED GPIO is reserved, in use, or unsupported on this board.");
    return;
  }
  String ledEntity = server.arg("ledEntity");
  ledEntity.trim();
  if (ledEntity.length() && !ledEntity.startsWith("input_boolean.") &&
      !ledEntity.startsWith("input_number.") && !ledEntity.startsWith("light.")) {
    sendJsonMessage(400, false, "The LED entity must be an input_boolean, input_number or light.");
    return;
  }
  PresenceSettings nextPresence;
  nextPresence.sleepMin = (uint16_t)clampedArg("sleepMin", presenceSettings.sleepMin, 0, 1440);
  nextPresence.dimPct = server.arg("sleepMode") == "dim" ? (uint8_t)clampedArg("sleepDim", 10, 1, 100) : 0;
  nextPresence.pc = server.hasArg("presencePc");
  size_t n = 0;
  for (size_t i = 0; i < MAX_PRESENCE_ENTITIES; i++) {
    String id = server.arg("presence" + String(i));
    id.trim();
    if (!id.length()) continue;
    if (!validEntityId(id)) {
      sendJsonMessage(400, false, "A presence entity is not an entity id (domain.object).");
      return;
    }
    nextPresence.entities[n++] = id;
  }

  brightness = (uint8_t)clampedArg("brightness", brightness, 0, 255);
  backlightSettings.nightEnabled = server.hasArg("nightEnabled") ? 1 : 0;
  backlightSettings.nightBrightness =
    (uint8_t)clampedArg("nightBrightness", backlightSettings.nightBrightness, 0, 255);
  backlightSettings.nightStartMinute =
    minuteOfDayArg("nightStart", backlightSettings.nightStartMinute);
  backlightSettings.nightEndMinute =
    minuteOfDayArg("nightEnd", backlightSettings.nightEndMinute);
  ledSettings = nextLed;
  presenceSettings = nextPresence;
  if (!ledEntity.length()) ledSetHaLevel(LED_HA_NONE);  // the saved level replaces one set over MQTT
  saveSettings();
  initLed();
  refreshBacklightControl();  // re-evaluates both night intervals
  deckPresenceApply();
  String err;
  if (!deckSetLedEntity(ledEntity, err)) {
    sendJsonMessage(500, false, ("Saved, but the LED entity was not stored: " + err).c_str());
    return;
  }
  sendJsonMessage(200, true, "Screen and light settings applied.");
}

static void handleSaveAdvanced() {
  MqttSettings next = mqttSettings();
  next.enabled = server.hasArg("mqttEnabled");
  next.host = server.arg("mqttHost");
  const String port = server.arg("mqttPort");
  bool digits = port.length() > 0 && port.length() <= 5;
  for (size_t i = 0; i < port.length(); i++) digits = digits && isDigit(port[i]);
  if (!digits || port.toInt() < 1 || port.toInt() > 65535) {
    sendJsonMessage(400, false, "MQTT port must be 1..65535");
    return;
  }
  next.port = (uint16_t)port.toInt();
  next.user = server.arg("mqttUser");
  next.prefix = server.arg("mqttPrefix");
  if (server.hasArg("mqttClearPass")) next.pass = "";
  else if (server.arg("mqttPass").length()) next.pass = server.arg("mqttPass");
  String error;
  if (!mqttNormalize(next, error)) {
    sendJsonMessage(400, false, error.c_str());
    return;
  }
  if (!mqttSaveSettings(next)) {
    sendJsonMessage(500, false, "Could not store MQTT settings");
    return;
  }
  deckSetPrinters(server.hasArg("printers"));
  saveSettings();
  sendJsonMessage(200, true, "Advanced settings applied.");
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

static void handleSaveDisplay() {
  int every = dispSettings.wallEvery, order = dispSettings.wallOrder;
  if (server.hasArg("wallEvery")) {
    const String value = server.arg("wallEvery");
    every = value.toInt();
    if (String(every) != value || !validWallEvery(every)) {
      sendJsonMessage(400, false, "invalid wallEvery");
      return;
    }
  }
  if (server.hasArg("wallOrder")) {
    const String value = server.arg("wallOrder");
    if (value != "0" && value != "1") {
      sendJsonMessage(400, false, "invalid wallOrder");
      return;
    }
    order = value.toInt();
  }
  deck::wallInvalidateJobs();
  dispSettings.wallEvery = every;
  dispSettings.wallOrder = order;
  const bool oldDaily = dispSettings.wallDaily;
  const uint32_t oldPlan = dispSettings.dailyOrient << 8 | dispSettings.dailyCount;
  if (server.hasArg("wallSource")) dispSettings.wallDaily = server.arg("wallSource") == "1";
  dispSettings.dailyHour = (uint8_t)clampedArg("dailyHour", dispSettings.dailyHour, 0, 23);
  dispSettings.dailyOrient = (uint8_t)clampedArg("dailyOrient", dispSettings.dailyOrient, 0, 2);
  dispSettings.dailyCount = (uint8_t)clampedArg("dailyCount", dispSettings.dailyCount, 1, DAILY_MAX_COUNT);
  const uint8_t oldRotation = dispSettings.rotation;
  const uint8_t oldTheme = dispSettings.theme;
  dispSettings.theme = (uint8_t)clampedArg("theme", dispSettings.theme, 0, 2);
  const uint32_t oldGlass = dispSettings.glassOn << 24 | dispSettings.glassTint << 16 |
                            dispSettings.glassBlur << 8 | dispSettings.glassDim;
  const bool oldAuto = dispSettings.glassAutoText;
  dispSettings.glassAutoText = server.hasArg("glassAutoText");
  dispSettings.glassTint = (uint8_t)clampedArg("glassTint", dispSettings.glassTint, 0, 90);
  dispSettings.glassBlur = (uint8_t)clampedArg("glassBlur", dispSettings.glassBlur, 0, 24);
  dispSettings.glassDim = (uint8_t)clampedArg("glassDim", dispSettings.glassDim, 0, 60);
  dispSettings.glassOn = (uint8_t)clampedArg("glassOn", dispSettings.glassOn, 20, 100);
  const bool glassChanged = oldAuto != dispSettings.glassAutoText ||
                            oldGlass != (uint32_t)(dispSettings.glassOn << 24 | dispSettings.glassTint << 16 |
                                                   dispSettings.glassBlur << 8 | dispSettings.glassDim);
  dispSettings.rotation = (uint8_t)clampedArg("rotation", dispSettings.rotation, 0, 3);
  dispSettings.tempScaleMax = (uint16_t)clampedArg("tempScale", dispSettings.tempScaleMax, 1, 500);
  dispSettings.powerScaleW = (uint16_t)clampedArg("powerScale", dispSettings.powerScaleW, 1, 65535);
  dispSettings.warnThresholdPct =
    (uint8_t)clampedArg("warnThreshold", dispSettings.warnThresholdPct, 0, 100);
  dispSettings.smoothCharts = server.hasArg("smoothCharts");

  saveSettings();
  if (dispSettings.wallDaily != oldDaily) deck::wallSetDaily(dispSettings.wallDaily);
  // A new plan fetches again today instead of waiting for the hour.
  if (oldPlan != (uint32_t)(dispSettings.dailyOrient << 8 | dispSettings.dailyCount)) deck::dailyReplan();
  if (dispSettings.rotation != oldRotation || dispSettings.theme != oldTheme) applyDisplaySettings();
  else if (glassChanged) deckReloadWallpaper(false);
  sendJsonMessage(200, true, "Display settings applied.");
}

static void handleSaveClock() {
  if (server.hasArg("timeFormat")) netSettings.use24h = server.arg("timeFormat") != "12";
  if (server.hasArg("timezone"))
    strlcpy(netSettings.timezoneStr, server.arg("timezone").c_str(),
            sizeof(netSettings.timezoneStr));
  saveSettings();
  configTzTime(netSettings.timezoneStr, "pool.ntp.org", "time.nist.gov");
  refreshBacklightControl();
  sendJsonMessage(200, true, "Time settings applied.");
}

static void handleFactoryReset() {
  if (server.arg("confirmation") != "RESET") {
    sendJsonMessage(400, false, "Type RESET to confirm the factory reset.");
    return;
  }
  // The icon font stays: a public download, not a setting.
  if (!factoryResetSettings() || !deck::deckFsWipe(deck::iconsPath())) {
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

static const char* screenName(ScreenState s) {
  switch (s) {
    case SCREEN_SPLASH:          return "splash";
    case SCREEN_AP_MODE:         return "wifi_setup";
    case SCREEN_CONNECTING_WIFI: return "connecting";
    case SCREEN_WIFI_CONNECTED:  return "connected";
    case SCREEN_OTA_UPDATE:      return "ota";
    default:                     return "deck";
  }
}

static void handleApiStatus() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"]   = BOARD_VARIANT;
  doc["ip"]      = WiFi.localIP().toString();
  doc["screen"]  = screenName(getScreenState());
  const PcSource* pc0 = pcSource("");
  doc["pc_online"] = pcAnyOnline();
  doc["pc_status"] = pc0 ? pc0->data.status : PC_STATUS_OK;
  doc["pcs_online"] = pcOnlineCount();
  doc["ha_link"] = deck::haLinkName(deck::haLink());
  doc["backlight"] = currentBacklightLevel();
  doc["night_active"] = nightBrightnessActive();
  doc["time_valid"] = backlightTimeValid();
  doc["manual_display_off"] = manualDisplayOff();
  deckPresenceJson(doc["presence"].to<JsonObject>());
  mqttStatusJson(doc["mqtt"].to<JsonObject>());
  doc["led_enabled"] = ledSettings.enabled != 0;
  doc["led_pin_valid"] = ledPinValid();
  doc["led_duty"] = ledCurrentDuty();
  doc["led_night"] = ledNightActive();
  doc["led_ha"] = ledHaLevel();  // -1 not linked, -2 on at the local level
  doc["free_heap"]    = ESP.getFreeHeap();
  doc["max_block"]    = ESP.getMaxAllocHeap();
  doc["internal_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  doc["psram_free"]   = ESP.getFreePsram();
  doc["reset_reason"] = resetReasonName();
  doc["wifi_disconnects"]     = wifiDisconnectCount();
  doc["wifi_last_disc_reason"] = wifiLastDisconnectReason();
  doc["metric_count"] = pc0 ? pc0->data.count : 0;
  doc["timestamp"] = pc0 ? pc0->data.timestamp : "";
  doc["hostname"] = netSettings.hostname;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["uptime_seconds"] = millis() / 1000UL;
  doc["ap_mode"] = isAPMode();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Live frame (GET /screen_live.bmp)
//  Streams the framebuffer the deck pipeline last sent to the panel, i.e. what
//  is really on the glass. LVGL runs in this same loop task, so the buffer
//  cannot change while the handler streams it.
// ---------------------------------------------------------------------------
static void handleLiveScreenshot() {
  String name, code;
  if (pcPairPending(name, code) && code.length()) {  // the code must stay on the glass
    sendJsonMessage(409, false, "Pairing in progress");
    return;
  }
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

// POST /api/deck/swipe?dir=1|-1[&axis=v] - bench hook, one full page slide.
static void handleDeckSwipe() {
  const int dir = server.hasArg("dir") ? server.arg("dir").toInt() : 1;
  const bool ok = deckTestSwipe(dir, server.arg("axis") == "v");
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Swipe started" : "Swipe not possible now");
}

// --- Deck page and its API ----------------------------------------------------
// The Deck section of the portal; loaded by portal.js only when /api/config
// reports "deck". no-cache so a firmware update is picked up at once.
static void handleDeckJs() {
  streamAsset(WEB_DECK_JS, sizeof(WEB_DECK_JS), "application/javascript", "no-cache", true);
}

// Old address of the separate Deck page: the section now lives in the portal.
static void handleDeckPage() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// GET /api/deck -> {"source": "...", "deck": <deck.json>}
static void handleDeckGet() {
  String text;
  deck::deckConfigText(text);
  String out;
  out.reserve(text.length() + 48);
  out += "{\"source\":\"";
  out += "device";
  out += "\",\"header\":";
  out += deckHeaderVisible() ? "true" : "false";
  out += ",\"deck\":";
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

// GET /api/printers - printer config and live status, never the secrets.
static void handlePrintersGet() { server.send(200, "application/json", deckPrintersJson()); }

// POST /api/printers/secret (id, serial, code) - an empty field keeps the stored one.
static void handlePrinterSecret() {
  const String id = server.arg("id");
  if (!id.length() || !deck::printerSecretSet(id, server.arg("serial"), server.arg("code"))) {
    sendJsonMessage(400, false, "Could not store the printer credentials");
    return;
  }
  deckPrintersReapply();
  sendJsonMessage(200, true, "Stored");
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

// WebServer hands a non-form POST body to a route's upload callback too, where
// server.upload() is a null reference: upload callbacks bail out on those.
static bool multipartRequest() { return server.header("Content-Type").startsWith("multipart/form-data"); }

// --- Icon font (MDI TTF on LittleFS) ------------------------------------------
static const char* const ICONS_UPLOAD = "/mdi.ttf.up";
static File iconUpload;
static String iconError;

// GET /api/deck/icons -> {"loaded", "count", "size"}
static void handleIconsStatus() {
  JsonDocument doc;
  doc["loaded"] = deck::iconsReady();
  doc["count"] = deck::iconsCount();
  doc["size"] = deck::iconsFileSize();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// GET /mdi.ttf - the loaded font for the portal's icon picker.
// The portal adds ?v=<size> so a new font gets a new URL.
static void handleIconsFont() {
  File f = deck::iconsReady() ? LittleFS.open(deck::iconsPath(), "r") : File();
  if (!f) {
    server.send(404, "text/plain", "No icon font");
    return;
  }
  server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server.streamFile(f, "font/ttf");
  f.close();
}

// POST /api/deck/icons (multipart "font") - store, then load on finish.
static void handleIconsUpload() {
  if (!multipartRequest()) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    iconError = "";
    if (!deck::deckFsReady()) {
      iconError = "file system not available";
      return;
    }
    LittleFS.remove(ICONS_UPLOAD);
    // Old font plus new one may not both fit; the pages read the old one, so
    // they lose their icons until the new font is in.
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 2 * 1024 * 1024) {
      deckReleaseIcons();
      LittleFS.remove(deck::iconsPath());
    }
    iconUpload = LittleFS.open(ICONS_UPLOAD, "w");
    if (!iconUpload) iconError = "cannot create the file";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();  // one handleClient() pass for the whole upload
    if (iconUpload && iconUpload.write(up.buf, up.currentSize) != up.currentSize) {
      iconError = "flash full";
      iconUpload.close();
    }
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (iconUpload) iconUpload.close();
    if (up.status == UPLOAD_FILE_ABORTED) iconError = "upload aborted";
  }
}

static void handleIconsFinish() {
  String err = iconError;
  if (!err.length() && !deckInstallIcons(ICONS_UPLOAD, err) && !err.length()) err = "install failed";
  if (iconError.length()) LittleFS.remove(ICONS_UPLOAD);
  iconError = "";
  if (err.length()) {
    sendJsonMessage(400, false, err.c_str());
    return;
  }
  handleIconsStatus();
}

// --- Glass wallpaper galleries -----------------------------------------------
static const char* const WALL_UPLOAD = "/wall.up";
static File wallUpload;
static String wallError;
static size_t wallBytes;
static unsigned wallParts;
static bool wallComplete;

static const char* wallFile(bool portrait) { return deck::wallPath(portrait ? 320 : 480, portrait ? 480 : 320); }

static bool wallOrientation(bool& portrait) {
  const String o = server.arg("o");
  if (o != "l" && o != "p") {
    sendJsonMessage(400, false, "invalid orientation");
    return false;
  }
  portrait = o == "p";
  return true;
}

static void handleWallStatus() {
  JsonDocument doc;
  const bool fs = deck::deckFsReady();
  doc["landscape"] = fs && LittleFS.exists(wallFile(false));
  doc["portrait"] = fs && LittleFS.exists(wallFile(true));
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleWallsStatus() {
  bool p;
  if (!wallOrientation(p)) return;
  JsonDocument doc;
  JsonArray files = doc["files"].to<JsonArray>();
  for (const auto& f : deck::wallFiles(p)) {
    JsonObject item = files.add<JsonObject>();
    item["n"] = f.name;
    item["s"] = f.size;
  }
  doc["current"] = deck::wallCurrent(p);
  doc["pinned"] = deck::wallPinned(p);
  doc["legacy"] = LittleFS.exists(wallFile(p));
  doc["free"] = LittleFS.totalBytes() - LittleFS.usedBytes();
  doc["total"] = LittleFS.totalBytes();
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleWallsFile() {
  bool p;
  if (!wallOrientation(p)) return;
  const String name = server.arg("n");
  if (!deck::wallValidName(name)) {
    sendJsonMessage(400, false, "invalid file name");
    return;
  }
  File f = deck::deckFsReady() ? LittleFS.open(deck::wallGalleryPath(p, name), "r") : File();
  if (!f) {
    sendJsonMessage(404, false, "picture not found");
    return;
  }
  server.sendHeader("Cache-Control", "public, max-age=3600");
  server.streamFile(f, "image/jpeg");
}

static void wallUploadCleanup() {
  if (wallUpload) wallUpload.close();
  LittleFS.remove(WALL_UPLOAD);
}

// Free space plus the legacy picture a stored JPEG replaces. usedBytes() walks
// the whole file system, so uploads measure once, not per chunk.
static size_t wallRoom(bool portrait) {
  size_t room = LittleFS.totalBytes() - LittleFS.usedBytes();
  File f = LittleFS.open(wallFile(portrait), "r");
  if (f) room += f.size();
  return room;
}
static size_t wallUploadRoom;

static void handleWallUpload() {
  if (!multipartRequest()) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (wallParts++ == 0) {
      wallError = "";
      wallComplete = false;
      wallBytes = 0;
      if (!deck::deckFsReady()) wallError = "file system not available";
      else if (deck::wallDaily()) wallError = "daily pictures are downloaded, switch to My pictures to add your own";
      else {
        wallUploadCleanup();
        wallUploadRoom = wallRoom(server.arg("o") == "p");
      }
    }
    if (wallParts != 1 || up.name != "wall") wallError = "expected one wall file";
    if (!wallError.length()) {
      wallUpload = LittleFS.open(WALL_UPLOAD, "w");
      if (!wallUpload) wallError = "cannot create the file";
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (wallError.length() || !wallUpload) return;
    wallBytes += up.currentSize;
    if (wallBytes > deck::WALL_MAX_JPEG) wallError = "picture too large";
    else if (wallUploadRoom < deck::WALL_RESERVE + wallBytes) wallError = "flash full";
    else if (wallUpload.write(up.buf, up.currentSize) != up.currentSize) wallError = "flash full";
    if (wallError.length()) wallUploadCleanup();
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (wallUpload) wallUpload.close();
    wallComplete = up.status == UPLOAD_FILE_END && wallParts == 1 && !wallError.length();
    if (up.status == UPLOAD_FILE_ABORTED) {
      wallError = "upload aborted";
      wallUploadCleanup();
      wallParts = 0;
    }
  }
}

static void handleWallFinish() {
  String err = wallError;
  const bool complete = wallComplete && wallParts == 1 && multipartRequest();
  wallError = "";
  wallParts = 0;
  wallComplete = false;
  bool p;
  if (!wallOrientation(p)) { wallUploadCleanup(); return; }
  if (!complete && !err.length()) err = "expected one JPEG file";
  if (!err.length() && !deck::wallValidateJpeg(WALL_UPLOAD, p))
    err = p ? "expected a baseline 320x480 JPEG" : "expected a baseline 480x320 JPEG";
  if (!err.length() && wallRoom(p) < deck::WALL_RESERVE) err = "flash full";
  auto files = deck::wallFiles(p);
  const uint32_t last = files.empty() ? 0 : strtoul(files.back().name.c_str(), nullptr, 16);
  if (!err.length() && last == UINT32_MAX) err = "file counter exhausted";
  char name[13];
  snprintf(name, sizeof(name), "%08lx.jpg", (unsigned long)(last + 1));
  if (!err.length()) {
    deck::wallCurrent(p);  // Capture the empty gallery before adding its first file.
    LittleFS.mkdir("/wall");
    LittleFS.mkdir(deck::wallDir(p, deck::wallDaily()));
    LittleFS.remove(wallFile(p));
    const String path = deck::wallGalleryPath(p, name);
    if (!LittleFS.rename(WALL_UPLOAD, path)) err = "could not store the picture";
    else if (LittleFS.totalBytes() - LittleFS.usedBytes() < deck::WALL_RESERVE) {
      LittleFS.remove(path);
      err = "flash full";
    }
  }
  if (err.length()) {
    wallUploadCleanup();
    sendJsonMessage(400, false, err.c_str());
    return;
  }
  if (files.empty()) deck::wallRequest(p, name);
  const int index = server.arg("i").toInt(), count = server.arg("c").toInt();
  if (index > 0 && count >= index) {
    char toast[48];
    snprintf(toast, sizeof(toast), "Wallpaper %d/%d", index, count);
    deckToast(toast);
  }
  handleWallsStatus();
}

// Keep this separate from the multipart route so POST query arguments survive.
static void handleWallRemove() {
  bool p;
  if (!wallOrientation(p)) return;
  if (deck::deckFsReady()) LittleFS.remove(wallFile(p));
  if (deck::wallFiles(p).empty()) deck::wallRequest(p, "");
  handleWallStatus();
}

static void handleWallsDelete() {
  bool p;
  if (!wallOrientation(p)) return;
  const String names = server.arg("n");
  std::vector<String> selected;
  if (names == "*") {
    for (const auto& f : deck::wallFiles(p)) selected.push_back(f.name);
  } else {
    int start = 0;
    do {
      const int comma = names.indexOf(',', start);
      const String name = comma < 0 ? names.substring(start) : names.substring(start, comma);
      if (!deck::wallValidName(name)) {
        sendJsonMessage(400, false, "invalid file name");
        return;
      }
      selected.push_back(name);
      if (comma < 0) break;
      start = comma + 1;
    } while (true);
  }
  deck::wallCurrent(p);
  bool ok = true;
  for (const String& name : selected) {
    const String path = deck::wallGalleryPath(p, name);
    if (LittleFS.exists(path)) ok &= LittleFS.remove(path);
  }
  deck::wallGalleryChanged(p);
  if (!ok) { sendJsonMessage(500, false, "could not delete a picture"); return; }
  handleWallsStatus();
}

static void handleWallsShow() {
  bool p;
  if (!wallOrientation(p)) return;
  const String name = server.arg("n");
  if (!deck::wallValidName(name)) { sendJsonMessage(400, false, "invalid file name"); return; }
  if (!deck::deckFsReady() || !LittleFS.exists(deck::wallGalleryPath(p, name))) {
    sendJsonMessage(404, false, "picture not found");
    return;
  }
  if (!deck::wallPin(p, name)) { sendJsonMessage(500, false, "could not save pin"); return; }
  deck::wallRequest(p, name);
  sendJsonMessage(200, true, "Wallpaper queued.");
}

static void handleDailyRun() {
  if (!deck::dailyStart()) sendJsonMessage(409, false, "already fetching or offline");
  else sendJsonMessage(200, true, "Fetching daily pictures.");
}

static void handleDailyStatus() {
  JsonDocument doc(psramJson());
  deck::dailyStatus(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleWallsNext() {
  if (!deck::wallNext((dispSettings.rotation & 1) == 0)) {
    sendJsonMessage(404, false, "gallery is empty");
    return;
  }
  sendJsonMessage(200, true, "Wallpaper queued.");
}

// POST /api/deck/bench?n=5 - bench hook, time a render of the shown page.
static void handleDeckBench() {
  uint32_t avg = 0, mx = 0;
  if (!deckBenchRender(server.hasArg("n") ? server.arg("n").toInt() : 5, avg, mx)) {
    sendJsonMessage(409, false, "Not possible now");
    return;
  }
  server.send(200, "application/json", String("{\"avg_us\":") + avg + ",\"max_us\":" + mx + "}");
}

// POST /api/deck/prefs?header=0|1 - deck display preferences.
static void handleDeckPrefs() {
  if (server.hasArg("header")) deckSetHeader(server.arg("header") != "0");
  sendJsonMessage(200, true, "Saved");
}

// POST /api/deck/card?tile=<id> - bench hook, open a tile's detail card.
static void handleDeckCard() {
  const bool ok = deckTestCard(server.arg("tile"));
  sendJsonMessage(ok ? 200 : 404, ok, ok ? "Card open" : "No such tile");
}

// POST /api/deck/http-test, body = an "http" action object: queue it, answer
// {ticket}. GET ?ticket=N: {done, code, err, body}. POST ?sensor=1 tests a sensor GET.
static void handleHttpTestPost() {
  String err;
  deck::TileAction a;
  const String& body = server.arg("plain");
  if (!deck::deckActionParse(body.c_str(), body.length(), a, &err) || a.kind != deck::ACT_HTTP) {
    sendJsonMessage(400, false, err.length() ? err.c_str() : "Not an http action");
    return;
  }
  const bool sensor = server.arg("sensor") == "1";
  if (sensor && (a.method != "GET" || a.body.length())) {
    sendJsonMessage(400, false, "HTTP sensors use GET without a body");
    return;
  }
  const uint32_t t = deck::httpTestStart(a, sensor);
  if (!t) {
    sendJsonMessage(503, false, "Busy, try again");
    return;
  }
  server.send(200, "application/json", String("{\"ticket\":") + t + "}");
}

static void handleHttpTestGet() {
  int code = 0;
  String err;
  std::shared_ptr<char> body;
  JsonDocument doc(psramJson());
  doc["done"] = deck::httpTestResult((uint32_t)server.arg("ticket").toInt(), code, err, body);
  if (doc["done"]) {
    doc["code"] = code;
    doc["err"] = err;
    doc["body"] = body ? body.get() : "";
  }
  const size_t len = measureJson(doc);
  char* out = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
  if (!out || doc.overflowed()) {
    heap_caps_free(out);
    sendJsonMessage(503, false, "No PSRAM for HTTP test result");
    return;
  }
  serializeJson(doc, out, len + 1);
  streamAsset(out, len, "application/json", "no-store");
  heap_caps_free(out);
}

// Pairing, called by the companion: POST /api/pc/pair?id=<pc_id> shows a code
// on the device; POST /api/pc/pair/confirm?id=&code= answers {"key"} once.
// POST /api/pc/pair?id=<pc_id>[&mode=tap]. Tap mode answers a token for
// /api/pc/pair/confirm; without it the device shows a code (older companions).
static void handlePcPair() {
  String token, err;
  const bool tap = server.arg("mode") == "tap";
  if (!pcPairStart(server.arg("id"), tap, token, err)) {
    sendJsonMessage(409, false, err.c_str());
    return;
  }
  if (!tap) {
    sendJsonMessage(200, true, "Enter the code shown on the device");
    return;
  }
  JsonDocument doc;
  doc["success"] = true;
  doc["message"] = "Tap Allow on the device";
  doc["token"] = token;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// 202 while a tap pairing waits for Allow; the companion polls.
static void handlePcPairConfirm() {
  String key, err;
  bool waiting;
  if (!pcPairConfirm(server.arg("id"), server.arg("code"), server.arg("token"), key, waiting, err)) {
    sendJsonMessage(waiting ? 202 : 403, false, err.c_str());
    return;
  }
  JsonDocument doc;
  doc["success"] = true;
  doc["key"] = key;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
  deckToast("PC paired");
}

static void handlePcPairCancel() {
  pcPairCancel();
  sendJsonMessage(200, true, "Pairing cancelled");
}

// GET /api/pc/status?id=<pc_id> -> {"known", "paired"}: the companion checks the
// device still holds its key (it is gone after a flash erase).
static void handlePcStatus() {
  bool paired = false;
  const bool known = pcPairedState(server.arg("id"), paired);
  JsonDocument doc;
  doc["known"] = known;
  doc["paired"] = known && paired;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/pc/forget?id=<pc_id> - drop a PC from the known list.
static void handlePcForget() {
  const bool ok = pcForget(server.arg("id"));
  sendJsonMessage(ok ? 200 : 404, ok, ok ? "PC removed" : "No such PC");
}

// POST /api/deck/tap?tile=<id>[&hold=1] - bench hook, a tap or hold on a tile
// (runs its action, or opens the confirm card).
static void handleDeckTap() {
  const bool ok = deckTestTap(server.arg("tile"), server.arg("hold") == "1");
  sendJsonMessage(ok ? 200 : 404, ok, ok ? "Tapped" : "No such tile");
}

// POST /api/deck/slide?tile=<id>&pct=0..100 - bench hook, a slider tile drag
// ending at pct (sends it to HA like a real one).
static void handleDeckSlide() {
  const bool ok = deckTestSlide(server.arg("tile"), server.arg("pct").toInt());
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Sent" : "No such slider, or HA not connected");
}

// POST /api/deck/confirm?ok=1|0 - bench hook, answer the open confirm card.
static void handleDeckConfirm() {
  const bool ok = deckTestConfirm(server.arg("ok") == "1");
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "OK" : "No confirm card open");
}

// POST /api/deck/edit?op=open|move|remove|done|cancel[&tile=<index>&x=&y=&w=&h=]
// - bench hook, drives the on-device edit mode. x, y, w, h in half cells.
static void handleDeckEdit() {
  const bool ok = deckTestEdit(server.arg("op"), server.arg("tile").toInt(), server.arg("x").toInt(),
                               server.arg("y").toInt(), server.arg("w").toInt(), server.arg("h").toInt());
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "OK" : "Not possible now");
}

// POST /api/deck/panel?open=1|0 - bench hook, quick panel slide.
static void handleDeckPanel() {
  const bool open = !server.hasArg("open") || server.arg("open") != "0";
  const bool ok = deckTestPanel(open);
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Panel slide started" : "Panel slide not possible now");
}

// POST /api/deck/screen?state=splash|setup|connecting|connected|ota|deck
// [&done=&total=|&ok=1|&fail=msg][&shot=1] - bench hook for system screens.
static void handleDeckScreen() {
  const String st = server.arg("state");
  ScreenState s = SCREEN_DECK;
  if (st == "splash") s = SCREEN_SPLASH;
  else if (st == "setup") s = SCREEN_AP_MODE;
  else if (st == "connecting") s = SCREEN_CONNECTING_WIFI;
  else if (st == "connected") s = SCREEN_WIFI_CONNECTED;
  else if (st == "ota") s = SCREEN_OTA_UPDATE;
  setScreenState(s);
  displayTick();
  if (s == SCREEN_OTA_UPDATE) {
    if (server.hasArg("fail")) deck::sysOtaResult(false, server.arg("fail").c_str());
    else if (server.hasArg("ok")) deck::sysOtaResult(true, "");
    else deck::sysOtaProgress(server.arg("done").toInt(), server.arg("total").toInt());
  }
  // shot=1: answer with the frame at once, before loop() moves on from a boot screen.
  if (server.hasArg("shot")) {
    deckRefreshNow();
    delay(60);  // the flush task picks the frame up asynchronously
    handleLiveScreenshot();
    return;
  }
  sendJsonMessage(200, true, "Screen set");
}

// ---------------------------------------------------------------------------
//  OTA (multipart upload to /ota/upload)
// ---------------------------------------------------------------------------
// The whole upload runs inside one handleClient() pass, so the update screen
// is redrawn from here, a few times a second.
static ScreenState otaPrevScreen = SCREEN_DECK;
static uint32_t otaLastDrawMs = 0;
static uint32_t otaFailedAt = 0;

static void otaShowProgress(uint32_t done, bool force) {
  const uint32_t now = millis();
  if (!force && now - otaLastDrawMs < 150) return;
  otaLastDrawMs = now;
  // The request body is the firmware plus a few hundred bytes of multipart.
  const size_t total = server.clientContentLength();
  deck::sysOtaProgress(done, total > done ? (uint32_t)total : 0);
  deckRefreshNow();
}

static void otaFail(const String& message) {
  otaError = message;
  otaInProgress = false;
  Serial.printf("OTA: failed: %s\n", message.c_str());
  deck::sysOtaResult(false, message.c_str());
  deckRefreshNow();
  otaFailedAt = millis();
}

static bool otaSucceeded = false;  // this request carried a complete image

static void handleOtaUpload() {
  if (!multipartRequest()) return;
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaError = "";
    otaInProgress = true;
    otaFirstChunk = true;
    otaFailedAt = 0;
    if (getScreenState() != SCREEN_OTA_UPDATE) otaPrevScreen = getScreenState();
    setScreenState(SCREEN_OTA_UPDATE);
    deck::sysScreenRelayout();  // a retry right after a failure starts clean
    displayTick();
    otaShowProgress(0, true);
    Serial.printf("OTA: start, file=%s\n", upload.filename.c_str());

    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
      otaFail("No OTA partition found");
      return;
    }
    if (!Update.begin(partition->size)) {
      otaFail("OTA begin failed: " + String(Update.errorString()));
      return;
    }
    if (server.hasHeader("X-MD5")) {
      String md5 = server.header("X-MD5");
      if (md5.length() == 32) Update.setMD5(md5.c_str());
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaInProgress) return;
    // loop() cannot feed the task watchdog during the upload; feed it per chunk.
    esp_task_wdt_reset();
    if (otaFirstChunk && upload.currentSize > 0) {
      otaFirstChunk = false;
      if (upload.buf[0] != 0xE9) {
        Update.abort();
        otaFail("Not a firmware file");
        return;
      }
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      const String err = Update.errorString();
      Update.abort();
      otaFail(err);
      return;
    }
    otaShowProgress(upload.totalSize, false);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaInProgress) return;
    if (Update.end(true)) {
      Serial.printf("OTA: success, %u bytes\n", upload.totalSize);
      otaInProgress = false;
      otaSucceeded = true;
      deck::sysOtaResult(true, "");
      deckRefreshNow();
    } else {
      otaFail(Update.errorString());
    }

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaFail("Upload aborted");
  }
}

static void handleOtaFinish() {
  if (!otaError.length() && !otaSucceeded) otaError = "No firmware file in the request";
  otaSucceeded = false;
  if (otaError.length() > 0) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = otaError;
    String out;
    serializeJson(doc, out);
    server.send(400, "application/json", out);
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
  server.on("/save/light", HTTP_POST, handleSaveLight);
  server.on("/save/advanced", HTTP_POST, handleSaveAdvanced);
  server.on("/led/preview", HTTP_POST, handleLedPreview);
  server.on("/save/clock", HTTP_POST, handleSaveClock);
  server.on("/save/network", HTTP_POST, handleSaveNetwork);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config/export", HTTP_GET, handleConfigExport);
  server.on("/api/config/import", HTTP_POST, handleConfigImport);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);  server.on("/api/status", HTTP_GET, handleApiStatus);
  // /screen.bmp is what the StatNode Companion's preview fetches.
  server.on("/screen.bmp", HTTP_GET, handleLiveScreenshot);
  server.on("/screen_live.bmp", HTTP_GET, handleLiveScreenshot);
  server.on("/api/deck/swipe", HTTP_POST, handleDeckSwipe);
  server.on("/api/deck/panel", HTTP_POST, handleDeckPanel);
  server.on("/api/deck/card", HTTP_POST, handleDeckCard);
  server.on("/api/deck/tap", HTTP_POST, handleDeckTap);
  server.on("/api/pc/forget", HTTP_POST, handlePcForget);
  server.on("/api/pc/status", HTTP_GET, handlePcStatus);
  server.on("/api/pc/pair", HTTP_POST, handlePcPair);
  server.on("/api/pc/pair/confirm", HTTP_POST, handlePcPairConfirm);
  server.on("/api/pc/pair/cancel", HTTP_POST, handlePcPairCancel);
  server.on("/api/deck/http-test", HTTP_POST, handleHttpTestPost);
  server.on("/api/deck/http-test", HTTP_GET, handleHttpTestGet);
  server.on("/api/deck/confirm", HTTP_POST, handleDeckConfirm);
  server.on("/api/deck/slide", HTTP_POST, handleDeckSlide);
  server.on("/api/deck/edit", HTTP_POST, handleDeckEdit);
  server.on("/api/deck/screen", HTTP_POST, handleDeckScreen);
  server.on("/deck", HTTP_GET, handleDeckPage);
  server.on("/deck.js", HTTP_GET, handleDeckJs);
  server.on("/api/deck/icons", HTTP_GET, handleIconsStatus);
  server.on("/api/deck/wallpaper", HTTP_GET, handleWallStatus);
  server.on("/api/deck/walls", HTTP_GET, handleWallsStatus);
  server.on("/api/deck/walls/file", HTTP_GET, handleWallsFile);
  server.on("/api/deck/walls", HTTP_POST, handleWallFinish, handleWallUpload);
  server.on("/api/deck/walls/delete", HTTP_POST, handleWallsDelete);
  server.on("/api/deck/walls/show", HTTP_POST, handleWallsShow);
  server.on("/api/deck/walls/next", HTTP_POST, handleWallsNext);
  server.on("/api/deck/daily", HTTP_GET, handleDailyStatus);
  server.on("/api/deck/daily/run", HTTP_POST, handleDailyRun);
  server.on("/api/deck/wallpaper/remove", HTTP_POST, handleWallRemove);
  server.on("/api/deck/bench", HTTP_POST, handleDeckBench);
  server.on("/api/deck/prefs", HTTP_POST, handleDeckPrefs);
  server.on("/api/deck/icons", HTTP_POST, handleIconsFinish, handleIconsUpload);
  server.on("/mdi.ttf", HTTP_GET, handleIconsFont);
  server.on("/api/deck", HTTP_GET, handleDeckGet);
  server.on("/api/deck", HTTP_POST, handleDeckPost);
  server.on("/api/ha/status", HTTP_GET, handleHaStatus);
  server.on("/api/printers", HTTP_GET, handlePrintersGet);
  server.on("/api/printers/secret", HTTP_POST, handlePrinterSecret);
  server.on("/api/ha/config", HTTP_POST, handleHaConfig);
  server.on("/api/ha/forget", HTTP_POST, handleHaForget);
  server.on("/api/ha/entities", HTTP_GET, handleHaEntities);
  server.on("/ota/upload", HTTP_POST, handleOtaFinish, handleOtaUpload);

  const char* headers[] = { "X-MD5", "Content-Type" };
  server.collectHeaders(headers, 2);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on :80");
}

void handleWebServer() {
  server.handleClient();
  // Parsing can fail after a file ends without calling the finish handler.
  if (wallParts) {
    wallUploadCleanup();
    wallParts = 0;
    wallComplete = false;
    wallError = "";
  }
  // A failed update shows its error for a moment, then the previous screen.
  if (otaFailedAt && millis() - otaFailedAt > 4000) {
    otaFailedAt = 0;
    if (getScreenState() == SCREEN_OTA_UPDATE) setScreenState(otaPrevScreen);
  }
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
