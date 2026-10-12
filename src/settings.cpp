#include "settings.h"
#include "config.h"
#include "led.h"       // sanitizeLedPin()
#include <Preferences.h>

char wifiSSID[33] = "";
char wifiPass[65] = "";
uint8_t brightness = 200;
DisplaySettings dispSettings;
NetworkSettings netSettings;
BacklightSettings backlightSettings;
LedSettings ledSettings;

static Preferences prefs;
static const uint8_t BACKLIGHT_SETTINGS_VERSION = 1;
static const uint8_t LED_SETTINGS_VERSION = 1;

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
void defaultDisplaySettings(DisplaySettings& ds) {
  ds.rotation         = 1;   // landscape
  ds.tempScaleMax     = GAUGE_TEMP_SCALE_DEFAULT;
  ds.powerScaleW      = GAUGE_POWER_SCALE_DEFAULT;
  ds.warnThresholdPct = 90;
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
  if (prefs.getBytesLength("net") == sizeof(NetworkSettings))
    prefs.getBytes("net", &netSettings, sizeof(NetworkSettings));
  if (prefs.getBytesLength("backlight") == sizeof(BacklightSettings)) {
    BacklightSettings stored;
    prefs.getBytes("backlight", &stored, sizeof(stored));
    if (stored.version == BACKLIGHT_SETTINGS_VERSION) backlightSettings = stored;
  }
  if (prefs.getBytesLength("led") == sizeof(LedSettings)) {
    LedSettings stored;
    prefs.getBytes("led", &stored, sizeof(stored));
    if (stored.version == LED_SETTINGS_VERSION) ledSettings = stored;
  }
  prefs.getString("ssid", wifiSSID, sizeof(wifiSSID));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", 200);
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
}

void saveSettings() {
  sanitizeLedPin();
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("rot", dispSettings.rotation);
  prefs.putUShort("tscale", dispSettings.tempScaleMax);
  prefs.putUShort("pscale", dispSettings.powerScaleW);
  prefs.putUChar("warnpct", dispSettings.warnThresholdPct);
  prefs.putBytes("net", &netSettings, sizeof(NetworkSettings));
  prefs.putBytes("backlight", &backlightSettings, sizeof(BacklightSettings));
  prefs.putBytes("led", &ledSettings, sizeof(LedSettings));
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.end();
}

bool factoryResetSettings() {
  prefs.begin(NVS_NAMESPACE, false);
  const bool cleared = prefs.clear();
  prefs.end();
  return cleared;
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
