#include "deck_weather.h"
#include "psram_json.h"
#include <HTTPClient.h>
#include <WiFi.h>

namespace deck {

namespace {

constexpr uint32_t FETCH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t RETRY_MS = 60UL * 1000UL;
constexpr uint32_t BOOT_WAIT_MS = 8000;  // let Home Assistant and the pages come up first

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
WeatherData s_data = {};
char s_error[48] = "";
bool s_wanted = false, s_imperial = false;
float s_lat = 0, s_lon = 0;
// s_busy is set by the loop before the task starts and cleared by the task last.
volatile bool s_busy = false;
bool s_kick = false;
uint32_t s_waitFrom = 0, s_wait = BOOT_WAIT_MS;
volatile bool s_ok = false;
// The task's stack is in PSRAM: the loop deletes it once it has parked itself.
TaskHandle_t s_task = nullptr;

void setError(const char* text) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_error, text, sizeof(s_error));
  portEXIT_CRITICAL(&s_mux);
}

// "HH:MM" out of "2026-07-03T04:30".
void clockOf(const char* iso, char out[6]) {
  out[0] = '\0';
  const char* t = iso ? strchr(iso, 'T') : nullptr;
  if (t && strlen(t) >= 6) {
    memcpy(out, t + 1, 5);
    out[5] = '\0';
  }
}

int8_t weekdayOf(const char* iso) {
  int y, m, d;
  if (!iso || sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return -1;
  static const int T[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (int8_t)((y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7);
}

bool fetch(float lat, float lon, bool imperial) {
  char url[520];
  // Plain HTTP: a public forecast, and no TLS handshake beside the UI.
  const int len = snprintf(
      url, sizeof(url),
      "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
      "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
      "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
      "&forecast_hours=%d&timezone=auto&forecast_days=%d%s",
      lat, lon, WEATHER_HOURS, WEATHER_DAYS, imperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  if (len < 0 || len >= (int)sizeof(url)) return false;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    setError("Weather: no connection");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    char text[48];
    if (code < 0) snprintf(text, sizeof(text), "Weather service unreachable (%d)", code);
    else snprintf(text, sizeof(text), "Weather service HTTP %d", code);
    setError(text);
    return false;
  }
  JsonDocument doc(psramJson());
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  JsonObjectConst current = doc["current"], daily = doc["daily"], hourly = doc["hourly"];
  if (err || current.isNull() || daily.isNull()) {
    setError("Weather: unreadable reply");
    return false;
  }

  WeatherData w = {};
  w.valid = true;
  w.temp = current["temperature_2m"] | 0.0f;
  w.feels = current["apparent_temperature"] | w.temp;
  w.wind = current["wind_speed_10m"] | 0.0f;
  w.humidity = (uint8_t)constrain((int)(current["relative_humidity_2m"] | 0), 0, 100);
  w.code = (uint8_t)(current["weather_code"] | 3);
  w.day = (current["is_day"] | 1) != 0;
  clockOf(daily["sunrise"][0], w.sunrise);
  clockOf(daily["sunset"][0], w.sunset);
  for (int i = 0; i < WEATHER_DAYS; i++) {
    if (daily["temperature_2m_max"][i].isNull()) break;
    WeatherDay& d = w.days[w.nDays++];
    d.wday = weekdayOf(daily["time"][i]);
    d.code = (uint8_t)(daily["weather_code"][i] | 3);
    d.hi = daily["temperature_2m_max"][i] | 0.0f;
    d.lo = daily["temperature_2m_min"][i] | 0.0f;
    d.rain = (int8_t)constrain((int)(daily["precipitation_probability_max"][i] | -1), -1, 100);
  }
  for (int i = 0; i < WEATHER_HOURS; i++) {
    if (hourly["temperature_2m"][i].isNull()) break;
    WeatherHour& h = w.hours[w.nHours++];
    char hm[6];
    clockOf(hourly["time"][i], hm);
    h.hour = (int8_t)atoi(hm);
    h.code = (uint8_t)(hourly["weather_code"][i] | 3);
    h.day = (hourly["is_day"][i] | 1) != 0;
    h.temp = hourly["temperature_2m"][i] | 0.0f;
    h.rain = (int8_t)constrain((int)(hourly["precipitation_probability"][i] | -1), -1, 100);
  }
  w.fetchedMs = millis();
  portENTER_CRITICAL(&s_mux);
  w.serial = s_data.serial + 1;
  s_data = w;
  s_error[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("Weather: %.1f, code %d, %d days, %d hours\n", w.temp, w.code, w.nDays, w.nHours);
  return true;
}

void task(void*) {
  portENTER_CRITICAL(&s_mux);
  const float lat = s_lat, lon = s_lon;
  const bool imperial = s_imperial;
  portEXIT_CRITICAL(&s_mux);
  s_ok = fetch(lat, lon, imperial);
  s_busy = false;
  vTaskSuspend(nullptr);
}

}  // namespace

void weatherConfigure(bool wanted, float lat, float lon, bool imperial) {
  const bool changed = lat != s_lat || lon != s_lon || imperial != s_imperial;
  portENTER_CRITICAL(&s_mux);
  s_lat = lat;
  s_lon = lon;
  s_imperial = imperial;
  if (changed) {
    const uint32_t serial = s_data.serial + 1;
    s_data = {};
    s_data.serial = serial;
  }
  portEXIT_CRITICAL(&s_mux);
  if (changed || (wanted && !s_wanted)) s_kick = true;
  s_wanted = wanted;
}

// 0,0 (open sea) is "not set".
bool weatherPlaceSet() { return s_lat != 0 || s_lon != 0; }

bool weatherPlace(float& lat, float& lon) {
  portENTER_CRITICAL(&s_mux);
  lat = s_lat;
  lon = s_lon;
  portEXIT_CRITICAL(&s_mux);
  return lat != 0 || lon != 0;
}

void weatherTick() {
  const uint32_t now = millis();
  if (s_task) {
    if (s_busy || eTaskGetState(s_task) != eSuspended) return;
    vTaskDeleteWithCaps(s_task);
    s_task = nullptr;
    s_waitFrom = now;
    s_wait = s_ok ? FETCH_MS : RETRY_MS;
  }
  if (s_kick && now >= BOOT_WAIT_MS) {
    s_kick = false;
    s_wait = 0;
  }
  if (now - s_waitFrom < s_wait) return;
  if (!s_wanted || !weatherPlaceSet() || WiFi.status() != WL_CONNECTED) {
    s_waitFrom = now;
    s_wait = 5000;
    return;
  }
  s_busy = true;
  if (xTaskCreatePinnedToCoreWithCaps(task, "deck_weather", 8192, nullptr, 1, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    s_task = nullptr;
    s_busy = false;
    s_waitFrom = now;
    s_wait = RETRY_MS;
    setError("Weather: not enough memory");
  }
}

uint32_t weatherSerial() { return s_data.serial; }

void weatherGet(WeatherData& out) {
  portENTER_CRITICAL(&s_mux);
  out = s_data;
  portEXIT_CRITICAL(&s_mux);
}

void weatherError(char* out, size_t n) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(out, s_error, n);
  portEXIT_CRITICAL(&s_mux);
}

bool weatherPreview(int code, bool day) {
  static const uint8_t kinds[] = {0, 2, 3, 45, 63, 73, 95};
  portENTER_CRITICAL(&s_mux);
  const bool ok = s_data.valid;
  if (ok) {
    s_data.code = (uint8_t)code;
    s_data.day = day;
    for (int i = 1; i < s_data.nDays; i++) s_data.days[i].code = kinds[(i - 1) % 7];
    for (int i = 1; i < s_data.nHours; i++) {
      s_data.hours[i].code = kinds[(i - 1) % 7];
      s_data.hours[i].day = day;
    }
    s_data.serial++;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

WeatherKind weatherKind(int code) {
  if (code == 0) return WX_CLEAR;
  if (code <= 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_STORM;
  return WX_RAIN;  // the 5x / 6x / 8x drizzle, rain and showers
}

// WMO code 4677 as Open-Meteo reports it.
const char* weatherText(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "";
  }
}

}  // namespace deck
