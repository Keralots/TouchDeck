// MQTT UI bridge. The loop task owns every call here.
#include "deck_ui_int.h"
#include "deck_mqtt.h"
#include "psram_json.h"

namespace {
std::vector<String> pageNames() {
  std::vector<String> names;
  for (int i = 0; i < deckui::s_count; i++) {
    const auto* def = deckui::s_pages[i].def;
    const String base = def->name.length() ? def->name : def->id;
    String name = base;
    int suffix = 2;
    while (std::find(names.begin(), names.end(), name) != names.end()) name = base + " (" + suffix++ + ")";
    names.push_back(name);
  }
  return names;
}

// The fonts cover ASCII, U+00A0..U+017F (Polish included) and the bullet; typographic
// punctuation is mapped like track names, anything else becomes '?'.
String messageText(const String& payload) {
  String out;
  const size_t n = min(payload.length(), (size_t)200);
  for (size_t i = 0; i < n;) {
    const uint8_t c = payload[i];
    if (i + 2 < n && c == 0xE2 && (uint8_t)payload[i + 1] == 0x80) {
      const uint8_t p = payload[i + 2];
      const char* rep = p == 0x98 || p == 0x99 ? "'" : p == 0x9C || p == 0x9D ? "\"" :
                        p == 0x93 || p == 0x94 ? "-" : p == 0xA6 ? "..." : p == 0xA2 ? "\xE2\x80\xA2" : nullptr;
      if (rep) { out += rep; i += 3; continue; }
    }
    if (i + 1 < n && c >= 0xC2 && c <= 0xC5 && ((uint8_t)payload[i + 1] & 0xC0) == 0x80) {
      out += (char)c;
      out += (char)payload[i + 1];
      i += 2;
    } else if (c < 128) {
      out += c >= 32 && c < 127 ? (char)c : ' ';
      i++;
    } else {
      out += '?';
      i++;
      while (i < n && ((uint8_t)payload[i] & 0xC0) == 0x80) i++;
    }
  }
  return out;
}
}  // namespace

bool mqttUiReady() { return deckui::s_disp && !deck::compOwnsScreen(); }

void mqttUiSnapshot(MqttSnapshot& out) {
  using namespace deckui;
  const bool lit = !manualDisplayOff() && currentBacklightLevel() != 0;
  out.state[MQ_SCREEN] = String("{\"state\":\"") + (lit ? "ON" : "OFF") + "\",\"brightness\":" + screenLevelRef() + "}";
  out.pages = pageNames();
  const int active = sh.active;
  if (active >= 0 && active < (int)out.pages.size()) out.state[MQ_PAGE] = out.pages[active];
  out.state[MQ_NIGHT] = manualNightActive() ? "ON" : "OFF";
  const uint32_t touch = touchLastActivity();
  out.state[MQ_TOUCH] = touch && millis() - touch < 60000 ? "ON" : "OFF";
  out.led = ledSettings.enabled && ledPinValid();
  out.state[MQ_LED] = String("{\"state\":\"") + (ledHaLevel() == 0 ? "OFF" : "ON") + "\",\"brightness\":" + ledDayLevel() + "}";
}

void mqttUiCommand(MqttObject object, const String& payload) {
  using namespace deckui;
  if (object == MQ_PAGE) {
    if (s_ed.root) return;  // never drop an unsaved on-device edit
    const auto names = pageNames();
    for (size_t i = 0; i < names.size(); i++) {
      if (names[i] != payload) continue;
      deckPresenceWake();
      hidePanelNow();
      closeModal();
      goToPage(s_pages[i].def->id);
      return;
    }
  } else if (object == MQ_NIGHT) {
    if (payload != "ON" && payload != "OFF") return;
    setManualNight(payload == "ON");
    updateBacklightControl();
    qpSyncValues();
  } else if (object == MQ_MESSAGE) {
    deckPresenceWake();
    deckToastShow(messageText(payload).c_str(), 6000);
  } else if (object == MQ_SCREEN || object == MQ_LED) {
    JsonDocument doc(psramJson());
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) return;
    const String state = doc["state"] | "";
    if (state.length() && state != "ON" && state != "OFF") return;
    const bool hasLevel = !doc["brightness"].isNull();
    const int level = doc["brightness"] | 0;
    if (hasLevel && (!doc["brightness"].is<int>() || level < (object == MQ_SCREEN ? 1 : 0) || level > 255)) return;
    if (object == MQ_SCREEN) {
      if (hasLevel) {
        screenLevelRef() = (uint8_t)level;
        refreshBacklightControl();
        qpSyncValues();
        saveSettings();
      }
      if (state == "OFF") {
        hidePanelNow();
        setManualDisplayOff(true);
      } else if (hasLevel || state == "ON") {
        deckPresenceWake();
      }
    } else if (ledSettings.enabled && ledPinValid()) {
      if (state == "OFF") ledSetHaLevel(0);
      else if (hasLevel) ledSetHaLevel(level);
      else if (state == "ON") ledSetHaLevel(LED_HA_LOCAL);
      qpSyncValues();
    }
  }
}
