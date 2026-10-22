#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include "config.h"

// Night brightness schedule. Versioned NVS blob "backlight"; `brightness` is
// the day level and lives in its own key.
struct BacklightSettings {
  uint8_t  version;
  uint8_t  nightEnabled;
  uint8_t  nightBrightness;
  uint8_t  reserved0;            // StatNode's offline-off flag, unused
  uint16_t nightStartMinute;     // minute of day, 0..1439
  uint16_t nightEndMinute;       // minute of day, 0..1439
  uint16_t reserved1;            // StatNode's offline sleep delay, unused
};

// Filament / status LED on a PWM pin. Versioned NVS blob "led"; see led.cpp.
struct LedSettings {
  uint8_t version;
  uint8_t enabled;
  uint8_t pin;
  uint8_t brightness;       // 0..255, the normal daytime level
  uint8_t nightEnabled;     // use nightBrightness inside the LED's own night interval
  uint8_t nightBrightness;  // 0..255, 0 switches the LED fully off at night
  uint8_t followDisplay;    // dark whenever the panel itself is dark
  uint8_t offlineOff;       // dark while the PC companion is offline
  uint16_t nightStartMinute;  // minute of day, 0..1439; v2, v1 shared the backlight's
  uint16_t nightEndMinute;    // minute of day, 0..1439
};

// Panel and PC metric tile settings, each its own NVS scalar.
constexpr uint8_t DAILY_MAX_COUNT = 32;

struct DisplaySettings {
  uint8_t  rotation;          // 0..3 (x90 degrees)
  uint16_t tempScaleMax;      // C, default full scale of temperature tiles
  uint16_t powerScaleW;       // W, default full scale of power tiles
  uint8_t  warnThresholdPct;  // metric tiles turn amber at >= this % of scale; 0 = off
  uint8_t  theme;             // THEME_MIDNIGHT, THEME_DAYLIGHT or THEME_GLASS
  uint8_t  glassTint;         // Glass: tile tint opacity %, 0..90
  uint8_t  glassBlur;         // Glass: blur radius px, 0..24 (0 = clear glass)
  uint8_t  glassDim;          // Glass: wallpaper darkened by %, 0..60
  uint8_t  glassOn;           // Glass: accent fill of an "on" tile, opacity %, 20..100
  uint8_t  wallEvery;         // Wallpaper interval in minutes, 0 = off
  uint8_t  wallOrder;         // 0 = in order, 1 = shuffle
  bool     wallDaily;         // Daily downloads enabled
  uint8_t  wallSource;        // 0 = own, 1 = daily, 2 = both
  bool     sdCard;
  uint16_t dailyMax;          // SD archive per orientation: 50, 100, 200 or 500
  uint8_t  dailyHour;         // Daily fetch hour, local time 0..23
  uint8_t  dailyOrient;       // 0 = landscape, 1 = portrait, 2 = both
  uint8_t  dailyCount;        // Daily pictures per orientation, 1..DAILY_MAX_COUNT
  bool     glassAutoText;     // Glass: text colour and tint follow the picture per tile
  bool     smoothCharts;      // PC metric charts glide between packets
};

// Screen sleep (portal Screen & light page); NVS scalars. Present = an entity
// "on"/"home", or (pc) any PC online.
constexpr size_t MAX_PRESENCE_ENTITIES = 4;
struct PresenceSettings {
  uint16_t sleepMin = 0;  // 0 = never sleeps
  uint8_t dimPct = 0;     // asleep: 0 = backlight off, else at most this % of full
  bool frame = false;
  uint8_t frameEvery = 5;
  bool frameClock = true;
  bool frameAnim = true;
  bool pc = false;
  String entities[MAX_PRESENCE_ENTITIES];  // "" = unused slot
};

// Portal Advanced page; NVS scalars.
struct AdvancedSettings {
  bool printers;  // Bambu printers over LAN; off hides their pages and tiles, config kept
};

constexpr uint8_t THEME_MIDNIGHT = 0;
constexpr uint8_t THEME_DAYLIGHT = 1;
constexpr uint8_t THEME_GLASS = 2;  // Midnight colours, glass tiles over a wallpaper

// Network settings. NVS blob "net", layout shared with StatNode.
struct NetworkSettings {
  bool useDHCP;           // true = DHCP, false = static
  char staticIP[16];
  char gateway[16];
  char subnet[16];
  char dns[16];
  bool showIPAtStartup;   // show the IP screen for a moment after WiFi connects
  uint8_t timezoneIndex;  // unused
  char timezoneStr[64];   // POSIX TZ string
  bool use24h;            // top bar clock: 24h (default) or 12h
  uint8_t dateFormat;     // unused
  bool mdnsEnabled;       // advertise <hostname>.local over mDNS
  char hostname[32];      // sanitized [a-z0-9-], default "touchdeck"
};

extern char wifiSSID[33];
extern char wifiPass[65];
extern uint8_t brightness;
extern DisplaySettings dispSettings;
extern NetworkSettings netSettings;
extern BacklightSettings backlightSettings;
extern LedSettings ledSettings;
extern AdvancedSettings advSettings;
extern PresenceSettings presenceSettings;

bool validDailyMax(int count);
bool validWallEvery(int minutes);
bool validFrameEvery(int minutes);
void loadSettings();
void saveSettings();
bool factoryResetSettings();
void defaultDisplaySettings(DisplaySettings& ds);
void defaultNetworkSettings(NetworkSettings& ns);
void defaultBacklightSettings(BacklightSettings& bs);
void defaultLedSettings(LedSettings& ls);

// Normalize a string into a valid mDNS/DNS label: lowercase, keep only
// [a-z0-9-], strip leading/trailing hyphens. Falls back to "touchdeck".
void sanitizeHostname(const char* in, char* out, size_t outSize);

#endif // SETTINGS_H
