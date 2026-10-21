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
  setIdleSleep(sleep, (uint8_t)((p.dimPct * 255 + 50) / 100));
}

}  // namespace deckui

using namespace deckui;

void deckPresenceWake() { presenceWake(); }

void deckPresenceApply() {
  haSetEntities(entityIds());
  presenceRestart();
}

String deckPresenceJson() {
  const PresenceSettings& p = presenceSettings;
  const uint32_t idle = (millis() - s_activeMs) / 1000;
  String out = String("{\"present\":") + (s_present ? "true" : "false") + ",\"asleep\":" +
               (idleSleepActive() ? "true" : "false") + ",\"idle_s\":" + idle + ",\"sleep_min\":" + p.sleepMin +
               ",\"states\":{";
  bool first = true;
  for (const String& id : p.entities) {
    if (!id.length()) continue;
    HaEntity e;
    const bool known = haGetEntity(id, e) && e.known;
    out += String(first ? "" : ",") + "\"" + id + "\":\"" + (known ? e.state : String("?")) + "\"";
    first = false;
  }
  return out + "}}";
}
