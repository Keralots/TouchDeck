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
#include "web_pages.h"
#include "deck_ui.h"
#include "deck_config.h"
#include "deck_ha.h"
#include "deck_http.h"
#include "deck_web.h"
#include "deck_icons.h"
#include "deck_sys.h"
#include <LittleFS.h>
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
  doc["deck"] = true;

  JsonObject display = doc["display"].to<JsonObject>();
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["theme"] = dispSettings.theme;

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
  led["followDisplay"] = ledSettings.followDisplay != 0;
  led["offlineOff"] = ledSettings.offlineOff != 0;
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
  JsonDocument doc;
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
  led["followDisplay"] = ledSettings.followDisplay != 0;
  led["offlineOff"] = ledSettings.offlineOff != 0;

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

  JsonObject deckOut = doc["deck"].to<JsonObject>();
  deckOut["header"] = deckHeaderVisible();
  String deckText;
  deck::deckConfigText(deckText);
  JsonDocument deckDoc;
  if (!deserializeJson(deckDoc, deckText)) deckOut["config"] = deckDoc;

  String out;
  serializeJson(doc, out);
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

  JsonDocument doc;
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
  READ_INT(display, "theme", 0, 1, nextDisplay.theme, uint8_t);

  READ_BOOL(backlight, "nightEnabled", nextBacklight.nightEnabled);
  READ_INT(backlight, "nightBrightness", 0, 255, nextBacklight.nightBrightness, uint8_t);
  READ_INT(backlight, "nightStartMinute", 0, 1439, nextBacklight.nightStartMinute, uint16_t);
  READ_INT(backlight, "nightEndMinute", 0, 1439, nextBacklight.nightEndMinute, uint16_t);

  READ_BOOL(led, "enabled", nextLed.enabled);
  READ_INT(led, "pin", 0, 48, nextLed.pin, uint8_t);
  READ_INT(led, "brightness", 0, 255, nextLed.brightness, uint8_t);
  READ_BOOL(led, "nightEnabled", nextLed.nightEnabled);
  READ_INT(led, "nightBrightness", 0, 255, nextLed.nightBrightness, uint8_t);
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

  dispSettings = nextDisplay;
  netSettings = nextNetwork;
  backlightSettings = nextBacklight;
  ledSettings = nextLed;
  brightness = nextBrightness;
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
static void handleSaveHardware() {
  LedSettings nextLed = ledSettings;
  nextLed.enabled = server.hasArg("ledEnabled") ? 1 : 0;
  nextLed.pin = (uint8_t)clampedArg("ledPin", nextLed.pin, 0, 48);
  nextLed.brightness = (uint8_t)clampedArg("ledBrightness", nextLed.brightness, 0, 255);
  nextLed.nightEnabled = server.hasArg("ledNightEnabled") ? 1 : 0;
  nextLed.nightBrightness =
    (uint8_t)clampedArg("ledNightBrightness", nextLed.nightBrightness, 0, 255);
  nextLed.followDisplay = server.hasArg("ledFollowDisplay") ? 1 : 0;
  nextLed.offlineOff = server.hasArg("ledOfflineOff") ? 1 : 0;
  if (nextLed.enabled && !ledPinAllowed(nextLed.pin)) {
    sendJsonMessage(400, false,
                    "That LED GPIO is reserved, in use, or unsupported on this board.");
    return;
  }
  ledSettings = nextLed;
  saveSettings();
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

static void handleSaveDisplay() {
  const uint8_t oldRotation = dispSettings.rotation;
  const uint8_t oldTheme = dispSettings.theme;
  dispSettings.theme = (uint8_t)clampedArg("theme", dispSettings.theme, 0, 1);
  dispSettings.rotation = (uint8_t)clampedArg("rotation", dispSettings.rotation, 0, 3);
  brightness = (uint8_t)clampedArg("brightness", brightness, 0, 255);
  dispSettings.tempScaleMax = (uint16_t)clampedArg("tempScale", dispSettings.tempScaleMax, 1, 500);
  dispSettings.powerScaleW = (uint16_t)clampedArg("powerScale", dispSettings.powerScaleW, 1, 65535);
  dispSettings.warnThresholdPct =
    (uint8_t)clampedArg("warnThreshold", dispSettings.warnThresholdPct, 0, 100);
  backlightSettings.nightEnabled = server.hasArg("nightEnabled") ? 1 : 0;
  backlightSettings.nightBrightness =
    (uint8_t)clampedArg("nightBrightness", backlightSettings.nightBrightness, 0, 255);
  backlightSettings.nightStartMinute =
    minuteOfDayArg("nightStart", backlightSettings.nightStartMinute);
  backlightSettings.nightEndMinute =
    minuteOfDayArg("nightEnd", backlightSettings.nightEndMinute);

  saveSettings();
  refreshBacklightControl();
  if (dispSettings.rotation != oldRotation || dispSettings.theme != oldTheme) applyDisplaySettings();
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
  doc["led_enabled"] = ledSettings.enabled != 0;
  doc["led_pin_valid"] = ledPinValid();
  doc["led_duty"] = ledCurrentDuty();
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
  if (pcPairPending(name, code)) {
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
  streamAsset(DECK_JS, sizeof(DECK_JS) - 1, "application/javascript", "no-cache");
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

// GET /mdi.ttf - the loaded font for the portal's icon picker, from PSRAM.
// The portal adds ?v=<size> so a new font gets a new URL.
static void handleIconsFont() {
  if (!deck::iconsReady()) {
    server.send(404, "text/plain", "No icon font");
    return;
  }
  streamAsset((const char*)deck::iconsData(), deck::iconsFileSize(), "font/ttf",
              "public, max-age=31536000, immutable");
}

// POST /api/deck/icons (multipart "font") - store, then load on finish.
static void handleIconsUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    iconError = "";
    if (!deck::deckFsReady()) {
      iconError = "file system not available";
      return;
    }
    LittleFS.remove(ICONS_UPLOAD);
    // Old font plus new one may not both fit; the running copy is in PSRAM.
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 2 * 1024 * 1024)
      LittleFS.remove(deck::iconsPath());
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
// {ticket}. GET /api/deck/http-test?ticket=N: {done, code, err, body}.
static void handleHttpTestPost() {
  String err;
  deck::TileAction a;
  const String& body = server.arg("plain");
  if (!deck::deckActionParse(body.c_str(), body.length(), a, &err) || a.kind != deck::ACT_HTTP) {
    sendJsonMessage(400, false, err.length() ? err.c_str() : "Not an http action");
    return;
  }
  const uint32_t t = deck::httpTestStart(a);
  if (!t) {
    sendJsonMessage(503, false, "Busy, try again");
    return;
  }
  server.send(200, "application/json", String("{\"ticket\":") + t + "}");
}

static void handleHttpTestGet() {
  int code = 0;
  String err, body;
  JsonDocument doc;
  doc["done"] = deck::httpTestResult((uint32_t)server.arg("ticket").toInt(), code, err, body);
  if (doc["done"]) {
    doc["code"] = code;
    doc["err"] = err;
    doc["body"] = body;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Pairing, called by the companion: POST /api/pc/pair?id=<pc_id> shows a code
// on the device; POST /api/pc/pair/confirm?id=&code= answers {"key"} once.
static void handlePcPair() {
  String err;
  const bool ok = pcPairStart(server.arg("id"), err);
  sendJsonMessage(ok ? 200 : 409, ok, ok ? "Enter the code shown on the device" : err.c_str());
}

static void handlePcPairConfirm() {
  String key, err;
  if (!pcPairConfirm(server.arg("id"), server.arg("code"), key, err)) {
    sendJsonMessage(403, false, err.c_str());
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
// - bench hook, drives the on-device edit mode.
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

static void handleOtaUpload() {
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
  server.on("/save/hardware", HTTP_POST, handleSaveHardware);
  server.on("/led/preview", HTTP_POST, handleLedPreview);
  server.on("/save/clock", HTTP_POST, handleSaveClock);
  server.on("/save/network", HTTP_POST, handleSaveNetwork);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config/export", HTTP_GET, handleConfigExport);
  server.on("/api/config/import", HTTP_POST, handleConfigImport);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  // /screen.bmp is what the StatNode Companion's preview fetches.
  server.on("/screen.bmp", HTTP_GET, handleLiveScreenshot);
  server.on("/screen_live.bmp", HTTP_GET, handleLiveScreenshot);
  server.on("/api/deck/swipe", HTTP_POST, handleDeckSwipe);
  server.on("/api/deck/panel", HTTP_POST, handleDeckPanel);
  server.on("/api/deck/card", HTTP_POST, handleDeckCard);
  server.on("/api/deck/tap", HTTP_POST, handleDeckTap);
  server.on("/api/pc/forget", HTTP_POST, handlePcForget);
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
  server.on("/api/deck/prefs", HTTP_POST, handleDeckPrefs);
  server.on("/api/deck/icons", HTTP_POST, handleIconsFinish, handleIconsUpload);
  server.on("/mdi.ttf", HTTP_GET, handleIconsFont);
  server.on("/api/deck", HTTP_GET, handleDeckGet);
  server.on("/api/deck", HTTP_POST, handleDeckPost);
  server.on("/api/ha/status", HTTP_GET, handleHaStatus);
  server.on("/api/ha/config", HTTP_POST, handleHaConfig);
  server.on("/api/ha/forget", HTTP_POST, handleHaForget);
  server.on("/api/ha/entities", HTTP_GET, handleHaEntities);
  server.on("/ota/upload", HTTP_POST, handleOtaFinish, handleOtaUpload);

  const char* headers[] = { "X-MD5" };
  server.collectHeaders(headers, 1);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on :80");
}

void handleWebServer() {
  server.handleClient();
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
