#include "settings.h"
#include "config.h"
#include "led.h"       // sanitizeLedPin()
#include "haptic.h"    // sanitizeHapticPin()
#include <Preferences.h>

char wifiSSID[33] = "";
char wifiPass[65] = "";
uint8_t brightness = 200;
DisplaySettings dispSettings;
NetworkSettings netSettings;
BacklightSettings backlightSettings;
LedSettings ledSettings;
AdvancedSettings advSettings = {};
PresenceSettings presenceSettings;
HapticSettings hapticSettings;

bool validFrameEvery(int minutes) {
  return minutes == 1 || minutes == 2 || minutes == 5 || minutes == 10 || minutes == 15 || minutes == 30;
}

static Preferences prefs;
static const uint8_t BACKLIGHT_SETTINGS_VERSION = 1;
static const uint8_t LED_SETTINGS_VERSION = 2;

// ---------------------------------------------------------------------------
//  StatNode migration
// ---------------------------------------------------------------------------
// StatNode's display blob, only to read rotation and the metric scales from a
// device that ran it. Must stay byte-identical to StatNode 1.3.0.
struct LegacyGaugeColors {
  uint16_t arc, label, value;
};
struct LegacyDisplaySettings {
  uint8_t  rotation;
  uint16_t bgColor, trackColor, progressBarColor;
  bool     animatedBar, smallLabels, invertColors, cydPanelClassic, pongClock;
  uint16_t tempScaleMax, powerScaleW;
  uint8_t  gaugeSmoothing;
  uint16_t warnColor;
  uint8_t  warnThresholdPct;
  uint16_t clockTimeColor, clockDateColor;
  bool     hideClockDate;
  LegacyGaugeColors progress, gauge;
};

// StatNode face, clock, touch pad and theme keys TouchDeck does not use.
static const char* const kLegacyKeys[] = {
  "disp", "gauges", "labels", "gauges1", "labels1", "gauges2", "labels2",
  "pages", "touch", "theme", "style", "facever", "layout", "surface",
  "duobands", "duorows", "clockface", "sparks", "chartsm", "glasstheme",
  "glossa", "glossbow", "chartfill",
};

// Called with prefs open read-write when no "rot" key exists yet.
static void migrateFromStatNode() {
  if (prefs.getBytesLength("disp") == sizeof(LegacyDisplaySettings)) {
    LegacyDisplaySettings old;
    prefs.getBytes("disp", &old, sizeof(old));
    dispSettings.rotation = old.rotation & 3;
    dispSettings.tempScaleMax = old.tempScaleMax;
    dispSettings.powerScaleW = old.powerScaleW;
    dispSettings.warnThresholdPct = old.warnThresholdPct;
    Serial.println("Settings: display settings taken over from StatNode");
  }
  // "rot" marks the migration done, so it is written last and the legacy keys
  // go only after every new key is stored.
  const bool ok = prefs.putUShort("tscale", dispSettings.tempScaleMax) &&
                  prefs.putUShort("pscale", dispSettings.powerScaleW) &&
                  prefs.putUChar("warnpct", dispSettings.warnThresholdPct) &&
                  prefs.putUChar("rot", dispSettings.rotation);
  if (!ok) return;
  for (const char* key : kLegacyKeys)
    if (prefs.isKey(key)) prefs.remove(key);
}

// ---------------------------------------------------------------------------
//  Defaults
// ---------------------------------------------------------------------------
bool validDailyMax(int count) {
  return count == 50 || count == 100 || count == 200 || count == 500;
}

bool validWallEvery(int minutes) {
  return minutes == 0 || minutes == 1 || minutes == 2 || minutes == 5 ||
         minutes == 10 || minutes == 15 || minutes == 30 || minutes == 60;
}

void defaultDisplaySettings(DisplaySettings& ds) {
  ds.rotation         = 1;   // landscape
  ds.tempScaleMax     = GAUGE_TEMP_SCALE_DEFAULT;
  ds.powerScaleW      = GAUGE_POWER_SCALE_DEFAULT;
  ds.warnThresholdPct = 90;
  ds.theme            = THEME_MIDNIGHT;
  ds.glassTint        = 40;
  ds.glassBlur        = 12;
  ds.glassDim         = 15;
  ds.glassOn          = 80;
  ds.wallEvery        = 5;
  ds.wallOrder        = 0;
  ds.wallDaily        = false;
  ds.wallSource       = 0;
  ds.sdCard           = false;
  ds.dailyMax         = 100;
  ds.dailyHour        = 3;
  ds.dailyOrient      = 0;
  ds.dailyCount       = 12;
  ds.glassAutoText    = false;
  ds.smoothCharts     = true;
  ds.tileFx           = 0;
}

void defaultBacklightSettings(BacklightSettings& bs) {
  memset(&bs, 0, sizeof(bs));
  bs.version = BACKLIGHT_SETTINGS_VERSION;
  bs.nightBrightness = 32;
  bs.nightStartMinute = 22 * 60;
  bs.nightEndMinute = 7 * 60;
}

void defaultLedSettings(LedSettings& ls) {
  ls.version = LED_SETTINGS_VERSION;
  ls.enabled = 0;
  ls.pin = LED_DEFAULT_PIN;
  ls.brightness = 160;
  ls.nightEnabled = 1;
  ls.nightBrightness = 24;
  ls.followDisplay = 1;
  ls.offlineOff = 0;
  ls.nightStartMinute = 22 * 60;
  ls.nightEndMinute = 7 * 60;
}

void defaultNetworkSettings(NetworkSettings& ns) {
  memset(&ns, 0, sizeof(ns));
  ns.useDHCP = true;
  ns.showIPAtStartup = true;
  ns.use24h = true;
  ns.mdnsEnabled = true;
  sanitizeHostname("touchdeck", ns.hostname, sizeof(ns.hostname));
}

// ---------------------------------------------------------------------------
//  Persistence. Blobs are taken only when size (and version) match.
// ---------------------------------------------------------------------------
static uint16_t clampScale(uint16_t v, uint16_t def) { return v ? v : def; }

void loadSettings() {
  defaultDisplaySettings(dispSettings);
  defaultNetworkSettings(netSettings);
  defaultBacklightSettings(backlightSettings);
  defaultLedSettings(ledSettings);

  prefs.begin(NVS_NAMESPACE, false);
  if (!prefs.isKey("rot")) {
    migrateFromStatNode();
  } else {
    dispSettings.rotation = prefs.getUChar("rot", dispSettings.rotation) & 3;
    dispSettings.tempScaleMax = prefs.getUShort("tscale", dispSettings.tempScaleMax);
    dispSettings.powerScaleW = prefs.getUShort("pscale", dispSettings.powerScaleW);
    dispSettings.warnThresholdPct = prefs.getUChar("warnpct", dispSettings.warnThresholdPct);
  }
  dispSettings.theme = prefs.getUChar("theme", THEME_MIDNIGHT);
  if (dispSettings.theme > THEME_GLASS) dispSettings.theme = THEME_MIDNIGHT;
  dispSettings.glassTint = min<uint8_t>(prefs.getUChar("gtint", dispSettings.glassTint), 90);
  dispSettings.glassBlur = min<uint8_t>(prefs.getUChar("gblur", dispSettings.glassBlur), 24);
  dispSettings.glassDim = min<uint8_t>(prefs.getUChar("gdim", dispSettings.glassDim), 60);
  dispSettings.glassOn = constrain(prefs.getUChar("gon", dispSettings.glassOn), 20, 100);
  dispSettings.wallEvery = prefs.getUChar("wevery", 5);
  if (!validWallEvery(dispSettings.wallEvery)) dispSettings.wallEvery = 5;
  dispSettings.wallOrder = min<uint8_t>(prefs.getUChar("worder", 0), 1);
  dispSettings.wallSource = prefs.isKey("wsrc") ? min<uint8_t>(prefs.getUChar("wsrc", 0), 2)
                                               : prefs.getBool("wdaily", false) ? 1 : 0;
  dispSettings.wallDaily = dispSettings.wallSource != 0;
  dispSettings.sdCard = prefs.getBool("sdon", false);
  dispSettings.dailyMax = prefs.getUShort("dmax", 100);
  if (!validDailyMax(dispSettings.dailyMax)) dispSettings.dailyMax = 100;
  dispSettings.dailyHour = min<uint8_t>(prefs.getUChar("dhour", 3), 23);
  dispSettings.dailyOrient = min<uint8_t>(prefs.getUChar("dorient", 0), 2);
  dispSettings.dailyCount = constrain(prefs.getUChar("dcount", 12), 1, DAILY_MAX_COUNT);
  dispSettings.glassAutoText = prefs.getBool("gauto", false);
  dispSettings.smoothCharts = prefs.getBool("smooth", true);
  dispSettings.tileFx = min<uint8_t>(prefs.getUChar("tilefx", 0), 2);
  advSettings.printers = prefs.getBool("prn", false);
  if (prefs.getBytesLength("net") == sizeof(NetworkSettings))
    prefs.getBytes("net", &netSettings, sizeof(NetworkSettings));
  if (prefs.getBytesLength("backlight") == sizeof(BacklightSettings)) {
    BacklightSettings stored;
    prefs.getBytes("backlight", &stored, sizeof(stored));
    if (stored.version == BACKLIGHT_SETTINGS_VERSION) backlightSettings = stored;
  }
  const size_t ledLen = prefs.getBytesLength("led");
  if (ledLen == sizeof(LedSettings)) {
    LedSettings stored;
    prefs.getBytes("led", &stored, sizeof(stored));
    if (stored.version == LED_SETTINGS_VERSION) ledSettings = stored;
  } else if (ledLen == offsetof(LedSettings, nightStartMinute)) {
    // v1 dimmed only inside the backlight's schedule: take its times and switch.
    LedSettings stored = ledSettings;
    prefs.getBytes("led", &stored, ledLen);
    if (stored.version == 1) {
      stored.version = LED_SETTINGS_VERSION;
      stored.nightStartMinute = backlightSettings.nightStartMinute;
      stored.nightEndMinute = backlightSettings.nightEndMinute;
      if (!backlightSettings.nightEnabled) stored.nightEnabled = 0;
      ledSettings = stored;
    }
  }
  prefs.getString("ssid", wifiSSID, sizeof(wifiSSID));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", 200);
  presenceSettings.sleepMin = min<uint16_t>(prefs.getUShort("psleep", 0), 1440);
  presenceSettings.dimPct = min<uint8_t>(prefs.getUChar("pdim", 0), 100);
  presenceSettings.pc = prefs.getBool("ppc", false);
  presenceSettings.frame = prefs.getBool("pframe", false);
  presenceSettings.frameEvery = prefs.getUChar("pfevery", 5);
  if (!validFrameEvery(presenceSettings.frameEvery)) presenceSettings.frameEvery = 5;
  presenceSettings.frameClock = prefs.getBool("pfclock", true);
  presenceSettings.frameAnim = prefs.getBool("pfanim", true);
  hapticSettings.enabled = prefs.getBool("hapon", false);
  hapticSettings.pin = prefs.getUChar("happin", 0);
  hapticSettings.strength = constrain(prefs.getUChar("hapstr", 100), HAPTIC_MIN_STRENGTH, 100);
  hapticSettings.pulseMs = constrain(prefs.getUChar("hapms", 40), HAPTIC_MIN_MS, HAPTIC_MAX_MS);
  // Entity ids, comma-separated.
  const String ids = prefs.getString("pent", "");
  for (size_t i = 0, from = 0; i < MAX_PRESENCE_ENTITIES && from < ids.length(); i++) {
    int comma = ids.indexOf(',', from);
    if (comma < 0) comma = ids.length();
    presenceSettings.entities[i] = ids.substring(from, comma);
    from = comma + 1;
  }
  prefs.end();

  dispSettings.tempScaleMax = clampScale(dispSettings.tempScaleMax, GAUGE_TEMP_SCALE_DEFAULT);
  dispSettings.powerScaleW = clampScale(dispSettings.powerScaleW, GAUGE_POWER_SCALE_DEFAULT);
  if (dispSettings.warnThresholdPct > 100) dispSettings.warnThresholdPct = 90;
  backlightSettings.nightEnabled = backlightSettings.nightEnabled ? 1 : 0;
  if (backlightSettings.nightStartMinute >= 24 * 60) backlightSettings.nightStartMinute = 22 * 60;
  if (backlightSettings.nightEndMinute >= 24 * 60) backlightSettings.nightEndMinute = 7 * 60;
  netSettings.hostname[sizeof(netSettings.hostname) - 1] = '\0';
  netSettings.timezoneStr[sizeof(netSettings.timezoneStr) - 1] = '\0';
  sanitizeLedPin();
  sanitizeHapticPin();
}

void saveSettings() {
  sanitizeLedPin();
  sanitizeHapticPin();
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("rot", dispSettings.rotation);
  prefs.putUShort("tscale", dispSettings.tempScaleMax);
  prefs.putUShort("pscale", dispSettings.powerScaleW);
  prefs.putUChar("warnpct", dispSettings.warnThresholdPct);
  prefs.putUChar("theme", dispSettings.theme);
  prefs.putUChar("gtint", dispSettings.glassTint);
  prefs.putUChar("gblur", dispSettings.glassBlur);
  prefs.putUChar("gdim", dispSettings.glassDim);
  prefs.putUChar("gon", dispSettings.glassOn);
  prefs.putUChar("wevery", dispSettings.wallEvery);
  prefs.putUChar("worder", dispSettings.wallOrder);
  prefs.putBool("wdaily", dispSettings.wallSource != 0);
  prefs.putUChar("wsrc", dispSettings.wallSource);
  prefs.putBool("sdon", dispSettings.sdCard);
  prefs.putUShort("dmax", dispSettings.dailyMax);
  prefs.putUChar("dhour", dispSettings.dailyHour);
  prefs.putUChar("dorient", dispSettings.dailyOrient);
  prefs.putUChar("dcount", dispSettings.dailyCount);
  prefs.putBool("gauto", dispSettings.glassAutoText);
  prefs.putBool("smooth", dispSettings.smoothCharts);
  prefs.putUChar("tilefx", dispSettings.tileFx);
  prefs.putBool("prn", advSettings.printers);
  prefs.putBytes("net", &netSettings, sizeof(NetworkSettings));
  prefs.putBytes("backlight", &backlightSettings, sizeof(BacklightSettings));
  prefs.putBytes("led", &ledSettings, sizeof(LedSettings));
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.putUShort("psleep", presenceSettings.sleepMin);
  prefs.putUChar("pdim", presenceSettings.dimPct);
  prefs.putBool("ppc", presenceSettings.pc);
  prefs.putBool("pframe", presenceSettings.frame);
  prefs.putUChar("pfevery", presenceSettings.frameEvery);
  prefs.putBool("pfclock", presenceSettings.frameClock);
  prefs.putBool("pfanim", presenceSettings.frameAnim);
  prefs.putBool("hapon", hapticSettings.enabled);
  prefs.putUChar("happin", hapticSettings.pin);
  prefs.putUChar("hapstr", hapticSettings.strength);
  prefs.putUChar("hapms", hapticSettings.pulseMs);
  String ids;
  for (const String& id : presenceSettings.entities)
    if (id.length()) ids += (ids.length() ? "," : "") + id;
  prefs.putString("pent", ids);
  prefs.end();
}

bool factoryResetSettings() {
  // Every NVS namespace the firmware writes: settings, PCs, HA link, deck UI, printers, MQTT.
  static const char* const kSpaces[] = {NVS_NAMESPACE, "deckpc", "deckha", "deckui", "printers", "deckmqtt"};
  bool ok = true;
  for (const char* ns : kSpaces) {
    if (!prefs.begin(ns, false)) {
      ok = false;
      continue;
    }
    ok &= prefs.clear();
    prefs.end();
  }
  return ok;
}

// ---------------------------------------------------------------------------
//  Hostname sanitizer
// ---------------------------------------------------------------------------
void sanitizeHostname(const char* in, char* out, size_t outSize) {
  if (outSize == 0) return;
  size_t j = 0;
  bool lastHyphen = false;
  for (size_t i = 0; in && in[i] && j < outSize - 1; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
      lastHyphen = false;
    } else if ((c == '-' || c == ' ' || c == '_') && j > 0 && !lastHyphen) {
      out[j++] = '-';
      lastHyphen = true;
    }
  }
  while (j > 0 && out[j - 1] == '-') j--;
  out[j] = '\0';
  if (j == 0) strlcpy(out, "touchdeck", outSize);
}
