// Weather for the deck's weather tiles, from Open-Meteo (no account or key).
// A one-shot task fetches now, the next hours and a week of days.
#ifndef DECK_WEATHER_H
#define DECK_WEATHER_H

#include <Arduino.h>

namespace deck {

constexpr int WEATHER_DAYS = 7;    // today first
constexpr int WEATHER_HOURS = 12;  // the current hour first

// Icon groups, mapped from WMO weather codes.
enum WeatherKind : uint8_t { WX_CLEAR = 0, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM };

struct WeatherDay {
  int8_t wday;  // 0 = Sunday
  uint8_t code;
  float hi, lo;
  int8_t rain;  // highest precipitation probability %, -1 = unknown
};

struct WeatherHour {
  int8_t hour;  // local hour of the place, 0..23
  uint8_t code;
  bool day;
  float temp;
  int8_t rain;
};

struct WeatherData {
  bool valid;
  uint32_t serial;  // grows with every stored fetch
  uint32_t fetchedMs;
  float temp, feels, wind;
  uint8_t humidity, code;
  bool day;
  char sunrise[6], sunset[6];  // "HH:MM"
  uint8_t nDays, nHours;
  WeatherDay days[WEATHER_DAYS];
  WeatherHour hours[WEATHER_HOURS];
};

// The place and units of deck.json's "weather"; wanted = a weather tile exists.
void weatherConfigure(bool wanted, float lat, float lon, bool imperial);
bool weatherPlaceSet();
// The place; false when none is set. Any task.
bool weatherPlace(float& lat, float& lon);
// Call from the loop: starts a fetch when one is due.
void weatherTick();
uint32_t weatherSerial();
void weatherGet(WeatherData& out);
// Why the last fetch failed, "" after a good one.
void weatherError(char* out, size_t n);
// Bench hook: show this WMO code now and the other icon kinds in the forecast
// until the next fetch; false without data to dress up.
bool weatherPreview(int wmoCode, bool day);
WeatherKind weatherKind(int wmoCode);
const char* weatherText(int wmoCode);

}  // namespace deck

#endif  // DECK_WEATHER_H
