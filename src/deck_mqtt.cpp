// Home Assistant discovery: network I/O on one worker, UI and NVS on the loop.
#include "deck_mqtt.h"
#include "config.h"
#include "settings.h"
#include "psram_json.h"
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>

namespace {
const char* const OBJECTS[] = {"screen", "page", "night", "touch", "message", "led", "rssi", "ip", "restart"};
const char* const COMPONENTS[] = {"light", "select", "switch", "binary_sensor", "notify", "light", "sensor", "sensor", "button"};
const char* const NAMES[] = {"Screen", "Page", "Night", "Touch", "Message", "LED", "RSSI", "IP", "Restart"};
struct Command { MqttObject object; String payload; uint32_t generation; };
MqttSettings s_settings, s_workerSettings;
MqttSnapshot s_snapshot;
SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
std::vector<Command> s_commands;
uint32_t s_generation = 1, s_snapshotVersion = 0;
String s_state = "off", s_error;
String s_id;
bool s_started = false;
uint32_t s_restartAt = 0;

void status(const char* state, const String& error = "") {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_state = state;
  s_error = error;
  xSemaphoreGive(s_lock);
}

bool commandObject(int i) { return i != MQ_TOUCH && i != MQ_RSSI && i != MQ_IP; }
String configTopic(const MqttSettings& cfg, int i) {
  return cfg.prefix + "/" + COMPONENTS[i] + "/touchdeck_" + s_id + "/" + OBJECTS[i] + "/config";
}

// Streaming input lets PubSubClient deliver even a long message with its small buffer.
class MqttInput : public Stream {
 public:
  size_t length = 0;
  void begin(const std::vector<String>& names) {
    pages = &names;
    matches = (1U << names.size()) - 1;
    length = 0;
  }
  size_t write(uint8_t c) override {
    for (size_t i = 0; pages && i < pages->size(); i++) {
      const String& name = (*pages)[i];
      if (length >= name.length() || (uint8_t)name[length] != c) matches &= ~(1U << i);
    }
    length++;
    return 1;
  }
  int page() const {
    for (size_t i = 0; pages && i < pages->size(); i++)
      if ((matches & (1U << i)) && (*pages)[i].length() == length) return (int)i;
    return -1;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
 private:
  const std::vector<String>* pages = nullptr;
  uint32_t matches = 0;
};

// Batch JSON writes without allocating a serialized discovery payload.
class MqttWriter {
 public:
  explicit MqttWriter(PubSubClient& client) : mqtt(client) {}
  size_t write(uint8_t c) {
    if (!ok) return 0;
    bytes[used++] = c;
    if (used == sizeof(bytes)) flush();
    return ok ? 1 : 0;
  }
  size_t write(const uint8_t* p, size_t n) {
    size_t done = 0;
    while (done < n && write(p[done])) done++;
    return done;
  }
  bool flush() {
    if (used && ok) ok = mqtt.write(bytes, used) == used;
    used = 0;
    return ok;
  }
 private:
  PubSubClient& mqtt;
  uint8_t bytes[128];
  size_t used = 0;
  bool ok = true;
};

bool discovery(PubSubClient& mqtt, const MqttSettings& cfg, const MqttSnapshot& snap, const String& base) {
  for (int i = 0; i < MQ_COUNT; i++) {
    const String topic = configTopic(cfg, i);
    if (i == MQ_LED && !snap.led) {
      if (!mqtt.beginPublish(topic.c_str(), 0, true) || !mqtt.endPublish()) return false;
      continue;
    }
    JsonDocument doc(psramJson());
    doc["unique_id"] = "touchdeck_" + s_id + "_" + OBJECTS[i];
    doc["name"] = NAMES[i];
    doc["has_entity_name"] = true;
    doc["availability_topic"] = base + "/status";
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add("touchdeck_" + s_id);
    device["name"] = snap.hostname;
    device["manufacturer"] = "Guition";
    device["model"] = "JC3248W535 TouchDeck";
    device["sw_version"] = FW_VERSION;
    device["configuration_url"] = "http://" + snap.ip + "/";
    if (i != MQ_MESSAGE && i != MQ_RESTART) doc["state_topic"] = base + "/" + OBJECTS[i] + "/state";
    if (commandObject(i)) doc["command_topic"] = base + "/" + OBJECTS[i] + "/set";
    if (i == MQ_SCREEN || i == MQ_LED) {
      doc["schema"] = "json";
      doc["brightness"] = true;
      doc["supported_color_modes"].to<JsonArray>().add("brightness");
    } else if (i == MQ_PAGE) {
      JsonArray options = doc["options"].to<JsonArray>();
      for (const String& name : snap.pages) options.add(name);
    } else if (i == MQ_TOUCH) {
      doc["device_class"] = "occupancy";
    } else if (i == MQ_RSSI) {
      doc["device_class"] = "signal_strength";
      doc["unit_of_measurement"] = "dBm";
      doc["state_class"] = "measurement";
      doc["entity_category"] = "diagnostic";
    } else if (i == MQ_IP) {
      doc["entity_category"] = "diagnostic";
    } else if (i == MQ_RESTART) {
      doc["device_class"] = "restart";
      doc["entity_category"] = "config";
      doc["payload_press"] = "PRESS";
    }
    if (doc.overflowed() || !mqtt.beginPublish(topic.c_str(), measureJson(doc), true)) return false;
    MqttWriter writer(mqtt);
    const size_t written = serializeJson(doc, writer);
    if (!writer.flush() || written != measureJson(doc) || !mqtt.endPublish()) return false;
  }
  return true;
}

bool sameDiscovery(const MqttSnapshot& a, const MqttSnapshot& b) {
  return a.pages == b.pages && a.led == b.led && a.hostname == b.hostname && a.ip == b.ip;
}

void task(void*) {
  WiFiClient socket;
  PubSubClient mqtt(socket);
  MqttInput input;
  mqtt.setStream(input);
  socket.setConnectionTimeout(5000);
  socket.setTimeout(5000);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(30);
  MqttSettings cfg;
  MqttSnapshot snap;
  String sent[MQ_COUNT];
  uint32_t sentRssi = 0;
  uint32_t generation = 0, version = 0, retryAt = 0, backoff = 5000;
  bool replay = true, configs = true, wasConnected = false;
  const String base = "touchdeck/" + s_id;
  const String clientId = "touchdeck-" + s_id;
  const String availability = base + "/status";
  mqtt.setCallback([&](char* topic, uint8_t* payload, unsigned int length) {
    const String incoming(topic);
    length = input.length;
    if (incoming == cfg.prefix + "/status") {
      if (length == 6 && memcmp(payload, "online", 6) == 0) replay = configs = true;
      return;
    }
    for (int i = 0; i < MQ_COUNT; i++) {
      if (!commandObject(i) || incoming != base + "/" + OBJECTS[i] + "/set") continue;
      String text;
      if (i == MQ_PAGE) {
        const int page = input.page();
        if (page < 0) return;
        text = snap.pages[page];
      } else {
        if (i != MQ_MESSAGE && length > 256) return;
        const size_t take = i == MQ_MESSAGE ? min((size_t)length, (size_t)200) : length;
        text.concat((const char*)payload, take);
      }
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (s_workerSettings.enabled && generation == s_generation && s_commands.size() < 8)
        s_commands.push_back({(MqttObject)i, text, generation});
      xSemaphoreGive(s_lock);
      return;
    }
  });
  bool bufferReady = mqtt.setBufferSize(768);
  for (;;) {
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (generation != s_generation) {
      cfg = s_workerSettings;
      generation = s_generation;
      changed = true;
    }
    if (version != s_snapshotVersion) {
      configs = configs || !sameDiscovery(snap, s_snapshot);
      snap = s_snapshot;
      version = s_snapshotVersion;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
      if (mqtt.connected()) {
        mqtt.publish(availability.c_str(), "offline", true);
        mqtt.disconnect();
      }
      socket.stop();
      mqtt.setServer(cfg.host.c_str(), cfg.port);
      retryAt = 0;
      backoff = 5000;
      wasConnected = false;
      replay = configs = true;
      status(cfg.enabled ? "connecting" : "off");
    }
    if (!cfg.enabled) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      socket.stop();
      wasConnected = false;
      status("connecting", "Waiting for WiFi");
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (!mqtt.connected()) {
      if (wasConnected) {
        status("error", "Broker connection lost");
        retryAt = millis() + backoff;
        backoff = min(backoff * 2, (uint32_t)60000);
        wasConnected = false;
      }
      if (retryAt && (int32_t)(millis() - retryAt) < 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      status("connecting");
      if (!bufferReady) bufferReady = mqtt.setBufferSize(768);
      bool ok = bufferReady && mqtt.connect(clientId.c_str(), cfg.user.length() ? cfg.user.c_str() : nullptr,
                                            cfg.user.length() ? cfg.pass.c_str() : nullptr,
                                            availability.c_str(), 0, true, "offline");
      if (ok) ok = mqtt.subscribe((cfg.prefix + "/status").c_str());
      for (int i = 0; i < MQ_COUNT && ok; i++)
        if (commandObject(i)) ok = mqtt.subscribe((base + "/" + OBJECTS[i] + "/set").c_str());
      if (ok) ok = mqtt.publish(availability.c_str(), "online", true);
      if (!ok) {
        status("error", bufferReady ? String("MQTT connection failed (") + mqtt.state() + ")" : String("MQTT buffer allocation failed"));
        socket.stop();
        retryAt = millis() + backoff;
        backoff = min(backoff * 2, (uint32_t)60000);
        continue;
      }
      wasConnected = true;
      backoff = 5000;
      replay = configs = true;
      status("connected");
    }
    input.begin(snap.pages);
    if (!mqtt.loop()) continue;
    if (configs) replay = true;
    bool ok = !configs || discovery(mqtt, cfg, snap, base);
    if (ok) configs = false;
    for (int i = 0; i < MQ_COUNT && ok; i++) {
      if (i == MQ_MESSAGE || i == MQ_RESTART || (i == MQ_LED && !snap.led)) continue;
      if (replay || snap.state[i] != sent[i] || (i == MQ_RSSI && snap.rssiSeq != sentRssi)) {
        const String topic = base + "/" + OBJECTS[i] + "/state";
        const String& value = snap.state[i];
        ok = mqtt.beginPublish(topic.c_str(), value.length(), true);
        if (ok) ok = mqtt.write((const uint8_t*)value.c_str(), value.length()) == value.length() && mqtt.endPublish();
      }
    }
    if (ok) {
      for (int i = 0; i < MQ_COUNT; i++) sent[i] = snap.state[i];
      sentRssi = snap.rssiSeq;
      replay = false;
    } else {
      // A partial streamed packet cannot be reused; the next connection replays it.
      socket.stop();
      status("error", "MQTT publish failed");
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}
}  // namespace

const MqttSettings& mqttSettings() { return s_settings; }

bool mqttNormalize(MqttSettings& cfg, String& error) {
  cfg.host.trim();
  if (cfg.host.startsWith("mqtt://")) cfg.host.remove(0, 7);
  const int colon = cfg.host.indexOf(':');
  if (colon >= 0) {
    const String port = cfg.host.substring(colon + 1);
    bool digits = port.length() > 0 && port.length() <= 5;
    for (size_t i = 0; i < port.length(); i++) digits = digits && isDigit(port[i]);
    if (!digits || port.toInt() < 1 || port.toInt() > 65535) { error = "Invalid MQTT port"; return false; }
    cfg.port = (uint16_t)port.toInt();
    cfg.host.remove(colon);
  }
  cfg.host.toLowerCase();
  if (cfg.host.length() > 63 || (cfg.enabled && !cfg.host.length()) || !cfg.port) {
    error = "MQTT needs a broker host (up to 63 characters) and port 1..65535";
    return false;
  }
  for (size_t i = 0; i < cfg.host.length(); i++) {
    const char c = cfg.host[i];
    if (!isAlphaNumeric(c) && c != '.' && c != '-' && c != '_') { error = "Invalid MQTT broker host"; return false; }
  }
  cfg.prefix.trim();
  while (cfg.prefix.endsWith("/")) cfg.prefix.remove(cfg.prefix.length() - 1);
  if (!cfg.prefix.length()) cfg.prefix = "homeassistant";
  if (cfg.user.length() > 127 || cfg.pass.length() > 127 || cfg.prefix.length() > 127) {
    error = "MQTT username, password and prefix must be at most 127 bytes";
    return false;
  }
  for (size_t i = 0; i < cfg.prefix.length(); i++) {
    const uint8_t c = cfg.prefix[i];
    if (c <= 32 || c >= 127 || c == '+' || c == '#') { error = "Invalid MQTT discovery prefix"; return false; }
  }
  return true;
}

void mqttBegin() {
  Preferences prefs;
  if (prefs.begin("deckmqtt", true)) {
    s_settings.enabled = prefs.getBool("en", false);
    s_settings.host = prefs.getString("host", "");
    s_settings.port = prefs.getUShort("port", 1883);
    s_settings.user = prefs.getString("user", "");
    s_settings.pass = prefs.getString("pass", "");
    s_settings.prefix = prefs.getString("prefix", "homeassistant");
    prefs.end();
  }
  String error;
  if (!mqttNormalize(s_settings, error)) s_settings = MqttSettings();
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[7];
  snprintf(id, sizeof(id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  s_id = id;
  s_lock = xSemaphoreCreateMutex();
  s_workerSettings = s_settings;
  s_started = true;
}

bool mqttSaveSettings(const MqttSettings& settings) {
  MqttSettings next = settings;
  String error;
  if (!mqttNormalize(next, error)) return false;
  Preferences prefs;
  if (!prefs.begin("deckmqtt", false)) return false;
  auto putText = [&](const char* key, const String& value) {
    if (!value.length()) return !prefs.isKey(key) || prefs.remove(key);
    return prefs.putString(key, value) == value.length();
  };
  bool ok = prefs.putBool("en", next.enabled) && putText("host", next.host) &&
            prefs.putUShort("port", next.port) && putText("user", next.user) &&
            putText("pass", next.pass) && putText("prefix", next.prefix);
  prefs.end();
  if (!ok) return false;
  s_settings = next;
  if (s_lock) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_workerSettings = next;
    s_generation++;
    s_commands.clear();
    s_state = next.enabled ? "connecting" : "off";
    s_error = "";
    xSemaphoreGive(s_lock);
  }
  return true;
}

void mqttConfigJson(JsonObject out, bool includeHasPass) {
  out["enabled"] = s_settings.enabled;
  out["host"] = s_settings.host;
  out["port"] = s_settings.port;
  out["user"] = s_settings.user;
  out["prefix"] = s_settings.prefix;
  if (includeHasPass) out["has_pass"] = s_settings.pass.length() > 0;
}

void mqttStatusJson(JsonObject out) {
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
  out["state"] = !s_settings.enabled ? String("off") : !s_lock ? String("error") : s_state;
  out["error"] = s_lock ? s_error : String("MQTT mutex allocation failed");
  out["broker"] = s_settings.host + ":" + s_settings.port;
  if (s_lock) xSemaphoreGive(s_lock);
}

void mqttTick() {
  if (!s_started || !s_lock) return;
  if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) ESP.restart();
  if (!s_settings.enabled) return;
  for (int i = 0; i < 8 && mqttUiReady(); i++) {
    Command cmd;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = !s_commands.empty();
    if (have) { cmd = s_commands.front(); s_commands.erase(s_commands.begin()); }
    xSemaphoreGive(s_lock);
    if (!have) break;
    if (cmd.generation != s_generation) continue;
    if (cmd.object == MQ_RESTART) {
      if (cmd.payload == "PRESS" && !s_restartAt) s_restartAt = millis() + 500;
    } else mqttUiCommand(cmd.object, cmd.payload);
  }
  static uint32_t last = 0, lastRssi = 0, rssiSeq = 0;
  static String rssi;
  const uint32_t now = millis();
  if (last && now - last < 250) return;
  last = now;
  MqttSnapshot snap;
  mqttUiSnapshot(snap);
  snap.hostname = netSettings.hostname;
  snap.ip = WiFi.localIP().toString();
  snap.state[MQ_IP] = snap.ip;
  if (!lastRssi || now - lastRssi >= 60000) {
    lastRssi = now | 1;
    rssi = WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("0");
    rssiSeq++;
  }
  snap.state[MQ_RSSI] = rssi;
  snap.rssiSeq = rssiSeq;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool changed = !sameDiscovery(s_snapshot, snap) || s_snapshot.rssiSeq != snap.rssiSeq;
  for (int i = 0; i < MQ_COUNT; i++) changed = changed || s_snapshot.state[i] != snap.state[i];
  if (changed) { s_snapshot = snap; s_snapshotVersion++; }
  xSemaphoreGive(s_lock);
  if (!s_task) {
    // PSRAM stack: the worker never calls NVS, LittleFS or LVGL.
    if (xTaskCreatePinnedToCoreWithCaps(task, "deck_mqtt", 6144, nullptr, 2, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
      s_task = nullptr;
      status("error", "Cannot start MQTT worker");
    }
  }
}
