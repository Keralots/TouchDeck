// MQTT settings and loop-task handoff to the network worker.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

struct MqttSettings {
  bool enabled = false;
  String host, user, pass;
  uint16_t port = 1883;
  String prefix = "homeassistant";
};

// NVS and these entry points belong to the loop task.
void mqttBegin();
void mqttTick();
const MqttSettings& mqttSettings();
bool mqttNormalize(MqttSettings& settings, String& error);
bool mqttSaveSettings(const MqttSettings& settings);
void mqttConfigJson(JsonObject out, bool includeHasPass);
void mqttStatusJson(JsonObject out);

enum MqttObject { MQ_SCREEN, MQ_PAGE, MQ_NIGHT, MQ_TOUCH, MQ_MESSAGE, MQ_LED, MQ_RSSI, MQ_IP, MQ_RESTART, MQ_COUNT };
struct MqttSnapshot {
  String state[MQ_COUNT];
  std::vector<String> pages;
  String hostname, ip;
  bool led = false;
  uint32_t rssiSeq = 0;
};
// UI bridge, called only by mqttTick after the compositor releases LVGL.
bool mqttUiReady();
void mqttUiSnapshot(MqttSnapshot& out);
void mqttUiCommand(MqttObject object, const String& payload);
