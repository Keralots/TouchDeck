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
  float numMin = NAN;       // input_number range and step
  float numMax = NAN;
  float numStep = NAN;
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
// Copy of one entity. False when the id is not in the subscribed set.
bool haGetEntity(const String& id, HaEntity& out);
// Bumps on any entity change or link change.
uint32_t haStoreVersion();

// Queue a service call. target = entity_id (may be empty), dataJson = the
// service_data object as JSON text (may be empty). False when not connected.
bool haCallService(const String& domain, const String& service, const String& target,
                   const String& dataJson);
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
