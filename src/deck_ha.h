// Guition deck: Home Assistant client.
//
// One FreeRTOS task owns the WebSocket (ws://<ha>/api/websocket): auth with
// a long-lived token, subscribe_entities for the entities the deck tiles use
// (in small batches, because the WebSocket library drops frames over 15 KB),
// call_service, heartbeat and reconnect. Entity state lands in a small store
// the UI reads by polling haStoreVersion(). The entity picker in the portal
// uses haFetchEntityList(): a list Home Assistant renders itself through
// /api/template (id, name, state, icon, unit), kept on flash and refreshed
// only when the user asks.
//
// The token lives only in NVS (namespace "deckha"), like the WiFi password:
// never in deck.json, backups, logs or /api responses.
#ifndef DECK_HA_H
#define DECK_HA_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

namespace deck {

enum HaLink : uint8_t {
  HA_NOT_CONFIGURED = 0,
  HA_WAITING_WIFI,
  HA_CONNECTING,
  HA_AUTHENTICATING,
  HA_CONNECTED,
  HA_AUTH_FAILED,   // token rejected: no retries until the settings change
  HA_UNREACHABLE,
};
const char* haLinkName(HaLink s);

struct HaEntity {
  bool known = false;       // HA reported it (false = missing or removed)
  String state;
  String name;              // friendly_name
  String icon;              // "mdi:..." when the entity sets one
  String unit;              // unit_of_measurement (sensors)
  String deviceClass;       // device_class (picks a default icon)
  int brightness = -1;      // 0..255 for lights that report it
  int32_t rgb = -1;         // 0xRRGGBB from rgb_color, lights that report it
  int level = -1;           // 0..100: cover position, fan percentage, media volume
  float numMin = NAN;       // input_number range and step
  float numMax = NAN;
  float numStep = NAN;
  // Climate: setpoint(s), room temperature, limits and what it is doing now.
  float target = NAN;       // temperature (single setpoint)
  float targetLow = NAN;    // target_temp_low / _high (heat_cool range)
  float targetHigh = NAN;
  float current = NAN;      // current_temperature
  float tempMin = NAN;      // min_temp / max_temp / target_temp_step
  float tempMax = NAN;
  float tempStep = NAN;
  String hvacAction;        // "heating", "cooling", "idle"...
  String hvacModes;         // hvac_modes joined with ',' ("off,heat,cool")
  uint32_t version = 0;     // bumps on every change of this entity
};

void haBegin();
// Replace the URL and/or token. An empty token keeps the stored one.
void haSetCredentials(const String& url, const String& token);
void haClearCredentials();
String haUrl();
bool haHasToken();
HaLink haLink();
String haLastError();
String haServerVersion();

// Entities the deck shows; (re)subscribed whenever the set changes.
void haSetEntities(const std::vector<String>& ids);

// Entities another module follows itself: their subscribe_entities events
// skip the store and go to the hook on the HA task. full = {"s","a"} of a
// first state, else the diff {"+":{"s","a"},"-":{"a":[...]}}; removed
// entities arrive as full with an empty object.
using HaRawHook = void (*)(const char* id, JsonObjectConst ev, bool full);
constexpr size_t HA_RAW_ID = 72;  // longest entity id, with the terminator
// Kept in PSRAM: dozens of short Strings would sit in internal RAM.
void haSetRawEntities(const char* const* ids, size_t count, HaRawHook hook);

// One entity found by haRegistryLookup.
struct HaRegEntry {
  char entity[HA_RAW_ID];
  char extra[64];  // second template field
  char uid[96];    // registry unique_id, "" when HA has none
};
// Done on the HA task; list is valid only during the call.
using HaRegDone = void (*)(uint32_t tag, bool ok, const HaRegEntry* list, size_t count,
                           const char* err);
// Renders tmpl through /api/template (one "entity_id|extra" per line), then
// reads each entity's unique_id from the entity registry. False when a
// lookup is already queued.
bool haRegistryLookup(const String& tmpl, uint32_t tag, HaRegDone done);

// Renders a template through /api/template. Blocking, loop task only.
bool haRenderTemplate(const String& tmpl, String& out, String& err);
// Copy of one entity. False when the id is not in the subscribed set.
bool haGetEntity(const String& id, HaEntity& out);
// Bumps on any entity change or link change.
uint32_t haStoreVersion();

constexpr int HA_HISTORY_POINTS = 48;
struct HaHistoryKey {
  String entity;
  uint8_t hours = 24;
};
// Shared series, copied under the HA lock. NaN means no data.
void haSetHistory(const std::vector<HaHistoryKey>& keys);
void haDetailHistory(const String& entity, uint8_t hours = 24);
bool haCopyHistory(const String& entity, uint8_t hours, uint32_t& version, float* points,
                   float& low, float& high);
uint32_t haHistoryVersion();
bool haNumericState(const String& state, float& value);

// Queue a service call. target = entity_id (may be empty), dataJson = the
// service_data object as JSON text (may be empty). False when not connected.
bool haCallService(const String& domain, const String& service, const String& target,
                   const String& dataJson);
// Tracked calls reserve a result slot until the UI takes the result. Zero = refused.
struct HaCallResult {
  bool ok = false;
  uint32_t finishedMs = 0;
  uint32_t entityVersion = 0;  // captured at the result, before later state events
};
uint32_t haCallServiceTracked(const String& domain, const String& service, const String& target,
                              const String& dataJson);
bool haTakeCallResult(uint32_t id, HaCallResult& result);
// A failed call's entity and message, one at a time, for the UI to revert and
// show. False when none is pending.
bool haTakeFailure(String& entity, String& message);

// Portal entity picker: {"updated": epoch, "count": n, "entities":
// [{"id","name","state","icon","unit"}]}. Served from a copy on flash
// (/ha_entities.json); refresh = fetch it from Home Assistant first (a
// blocking call of about a second, loop task only).
bool haFetchEntityList(String& outJson, bool refresh, String& err);
// Where that copy lives, for streaming it straight from flash.
const char* haEntityListPath();

}  // namespace deck

#endif  // DECK_HA_H
