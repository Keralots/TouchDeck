// Guition deck: Home Assistant client.
//
// One FreeRTOS task owns the WebSocket (ws://<ha>/api/websocket): auth with
// a long-lived token, subscribe_entities for the entities the deck tiles use
// (in small batches, because the WebSocket library drops frames over 15 KB),
// call_service, heartbeat and reconnect. Entity state lands in a small store
// the UI reads by polling haStoreVersion(). The entity picker in the portal
// uses haFetchEntityList(), a streamed REST call (/api/states) filtered down
// to id, name, state and icon, so a large installation does not need a large
// buffer.
//
// The token lives only in NVS (namespace "deckha"), like the WiFi password:
// never in deck.json, backups, logs or /api responses.
#ifndef DECK_HA_H
#define DECK_HA_H

#if defined(BOARD_IS_JC3248W535)

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
  int brightness = -1;      // 0..255 for lights that report it
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

// Portal entity picker: JSON array [{"id","name","state","icon"}] sorted by id.
// Blocking REST call (a second or two on a big install); loop task only.
bool haFetchEntityList(String& outJson, String& err);

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_HA_H
