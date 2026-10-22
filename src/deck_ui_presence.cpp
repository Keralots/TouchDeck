// Deck UI: screen sleep and wake from presence (presenceSettings, Screen & light page).
#include "deck_ui_int.h"

namespace deckui {

bool s_present = false;     // an entity or PC says someone is there
uint32_t s_activeMs = 0;    // last touch, presence or wake request
uint32_t s_presenceEval = 0;

bool presenceNow() {
  const PresenceSettings& p = presenceSettings;
  if (p.pc && pcAnyOnline()) return true;
  for (const String& id : p.entities) {
    HaEntity e;
    if (id.length() && haGetEntity(id, e) && e.known && (e.state == "on" || e.state == "home")) return true;
  }
  return false;
}

// New config: the timer starts again.
void presenceRestart() { s_activeMs = millis(); }

void presenceWake() {
  s_activeMs = millis();
  wakeDisplayFromTouch();
}

void presenceTick() {
  const uint32_t now = millis();
  if (!s_activeMs) s_activeMs = now | 1;
  if (now - s_presenceEval < 250) return;
  s_presenceEval = now;
  const PresenceSettings& p = presenceSettings;
  const bool present = presenceNow();
  // Arrival also undoes a manual off; staying present only holds the timer.
  if (present && !s_present) presenceWake();
  s_present = present;
  if (present) s_activeMs = now;
  const uint32_t touch = touchLastActivity();
  if (touch && (int32_t)(touch - s_activeMs) > 0) s_activeMs = touch;
  const bool sleep = p.sleepMin && getScreenState() == SCREEN_DECK && now - s_activeMs >= p.sleepMin * 60000UL;
  if (!frameSleep(sleep)) setIdleSleep(sleep, (uint8_t)((p.dimPct * 255 + 50) / 100));
}

}  // namespace deckui

using namespace deckui;

void deckPresenceWake() { presenceWake(); }

void deckPresenceApply() {
  haSetEntities(entityIds());
  presenceRestart();
}

void deckPresenceJson(JsonObject out) {
  const PresenceSettings& p = presenceSettings;
  out["present"] = s_present;
  out["asleep"] = idleSleepActive();
  out["idle_s"] = (millis() - s_activeMs) / 1000;
  out["sleep_min"] = p.sleepMin;
  JsonObject states = out["states"].to<JsonObject>();
  for (const String& id : p.entities) {
    if (!id.length()) continue;
    HaEntity e;
    states[id] = haGetEntity(id, e) && e.known ? e.state : String("?");
  }
}
