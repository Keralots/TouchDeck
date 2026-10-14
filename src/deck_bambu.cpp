// Guition deck: Bambu LAN printers. See deck_bambu.h.
#include "deck_bambu.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <new>

extern "C" int mbedtls_platform_set_calloc_free(void* (*)(size_t, size_t), void (*)(void*));

namespace deck {
namespace {

constexpr size_t RX_SIZE = 65536;
constexpr uint32_t STACK_SIZE = 10240;

struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};

void* tlsCalloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  if (n * size >= 1024) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

SemaphoreHandle_t mutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
struct Lock {
  Lock() { xSemaphoreTake(mutex(), portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex()); }
};

struct Slot {
  PrinterConn conn;
  bool powered = false;
  uint32_t generation = 0;
  PrinterState* state = nullptr;  // worker-owned snapshot, copied under the lock
  PrinterLink link = PRN_OFF;
  char err[48] = "";
  int8_t light = -1;
};
Slot s_slots[PRN_MAX];
TaskHandle_t s_task = nullptr;
uint32_t s_version = 1, s_generation = 0;

bool current(int index, uint32_t generation) {
  Lock lock;
  return s_slots[index].powered && s_slots[index].generation == generation;
}

// Assemble large reports without making PubSubClient wait for their next fragment.
// Only small control packets pass through its internal buffer.
class MqttRxClient : public Client {
 public:
  using Handler = void (*)(void*, const char*, const uint8_t*, size_t);
  MqttRxClient(WiFiClientSecure& tls, int index, uint32_t generation)
      : _tls(tls), _index(index), _generation(generation) {}
  ~MqttRxClient() { heap_caps_free(_packet); }
  bool begin(Handler handler, void* context) {
    _handler = handler;
    _context = context;
    _packet = (uint8_t*)heap_caps_malloc(RX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return _packet != nullptr;
  }
  int connect(IPAddress ip, uint16_t port) override {
    reset();
    error = nullptr;
    return current(_index, _generation) ? _tls.connect(ip, port, 3000) : 0;
  }
  int connect(const char* host, uint16_t port) override {
    reset();
    error = nullptr;
    return current(_index, _generation) ? _tls.connect(host, port, 3000) : 0;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t n) override {
    return current(_index, _generation) ? _tls.write(data, n) : 0;
  }
  int available() override {
    pump();
    if (_passPos == _passLen) vTaskDelay(1);  // CONNECT polls available() until CONNACK
    return (int)(_passLen - _passPos);
  }
  int read() override { return _passPos < _passLen ? _pass[_passPos++] : -1; }
  int read(uint8_t* data, size_t n) override {
    n = std::min(n, _passLen - _passPos);
    if (!n) return -1;
    memcpy(data, _pass + _passPos, n);
    _passPos += n;
    return (int)n;
  }
  int peek() override { return _passPos < _passLen ? _pass[_passPos] : -1; }
  void flush() override { _tls.flush(); }
  void stop() override { reset(); _tls.stop(); }
  uint8_t connected() override { return _tls.connected(); }
  operator bool() override { return connected(); }
  const char* error = nullptr;
  bool subscribed = false;

 private:
  void reset() {
    _headerLen = 0;
    _body = false;
    _passPos = _passLen = 0;
    subscribed = false;
  }
  void fail(const char* reason) { stop(); error = reason; }
  void pump() {
    if (!_packet || _passPos < _passLen) return;
    if (!current(_index, _generation)) { stop(); return; }
    if (_headerLen && millis() - _started > 10000) { fail("Incomplete MQTT packet"); return; }
    size_t budget = 8192;
    while (budget && _passPos == _passLen) {
      const int available = _tls.available();
      if (available <= 0) return;
      if (!_body) {
        const int c = _tls.read();
        if (c < 0) return;
        --budget;
        _header[_headerLen++] = (uint8_t)c;
        if (_headerLen == 1) {
          _remaining = 0;
          _multiplier = 1;
          _started = millis();
          continue;
        }
        _remaining += (c & 127) * _multiplier;
        _multiplier *= 128;
        if (c & 128) {
          if (_headerLen == 5) { fail("Invalid MQTT length"); return; }
          continue;
        }
        const bool publish = (_header[0] >> 4) == 3;
        if (_remaining > (publish ? RX_SIZE : sizeof(_pass) - _headerLen)) {
          fail("MQTT packet exceeds buffer");
          return;
        }
        if (publish && (_header[0] & 6)) { fail("Unexpected MQTT QoS"); return; }
        _got = 0;
        _body = true;
      } else {
        const size_t n = std::min(budget, std::min((size_t)available, (size_t)(_remaining - _got)));
        const int got = _tls.read(_packet + _got, n);
        if (got <= 0) return;
        _got += got;
        budget -= got;
      }
      if (_body && _got == _remaining) { finish(); return; }
    }
  }
  void finish() {
    const uint8_t type = _header[0] >> 4;
    if (type == 3) {
      const size_t topicLen = _remaining >= 2 ? (_packet[0] << 8) | _packet[1] : 0;
      if (!topicLen || topicLen >= 128 || topicLen + 2 > _remaining) {
        fail("Invalid MQTT topic");
        return;
      }
      char topic[128];
      memcpy(topic, _packet + 2, topicLen);
      topic[topicLen] = 0;
      if (_handler) _handler(_context, topic, _packet + 2 + topicLen, _remaining - 2 - topicLen);
    } else {
      if (type == 9) {
        if (_remaining != 3 || _packet[2] != 0) { fail("MQTT subscription refused"); return; }
        subscribed = true;
      }
      memcpy(_pass, _header, _headerLen);
      memcpy(_pass + _headerLen, _packet, _remaining);
      _passPos = 0;
      _passLen = _headerLen + _remaining;
    }
    _headerLen = 0;
    _body = false;
  }
  WiFiClientSecure& _tls;
  int _index;
  uint32_t _generation;
  Handler _handler = nullptr;
  void* _context = nullptr;
  uint8_t* _packet = nullptr;
  uint8_t _header[5], _headerLen = 0, _pass[64];
  size_t _passPos = 0, _passLen = 0;
  uint32_t _remaining = 0, _multiplier = 1, _got = 0, _started = 0;
  bool _body = false;
};

struct TrayNames {
  char type[24] = "", brand[24] = "";
};

struct Session {
  PrinterConn conn;
  int index;
  uint32_t generation;
  WiFiClientSecure tls;
  MqttRxClient io;
  PubSubClient mqtt;
  PrinterState state{}, snapshot{};
  bool ready = false, connected = false, gotReport = false, secondLight = false, ctcSeen = false;
  uint32_t nextTry = 0, backoff = 5000, connectedMs = 0, lastPush = 0, sequence = 0;
  uint32_t lastCore = 0;
  uint16_t snow[2] = {0xffff, 0xffff};
  bool snowSeen[2] = {false, false};
  uint16_t trayNow = 255;
  TrayNames trayNames[PRN_AMS_UNITS][PRN_AMS_SLOTS], extNames;
  char taskId[32] = "";
  Session(const PrinterConn& c, int i, uint32_t gen)
      : conn(c), index(i), generation(gen), io(tls, i, gen), mqtt(io) {}
  ~Session() { io.stop(); }
};

void publishState(Session& c) {
  Lock lock;
  Slot& slot = s_slots[c.index];
  if (!slot.powered || slot.generation != c.generation) return;
  c.snapshot = c.state;
  slot.state = &c.snapshot;
  ++s_version;
}

void link(Session& c, PrinterLink value, const char* error = "") {
  if (c.state.link == value && strcmp(c.state.err, error) == 0) return;
  c.state.link = value;
  strlcpy(c.state.err, error, sizeof(c.state.err));
  publishState(c);
}

bool number(JsonVariantConst value) { return value.is<double>() || value.is<const char*>(); }
long integer(JsonVariantConst value) { return value.as<long>(); }

const char* skipSpace(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) ++p;
  return p;
}

// Read deep subtrees from the unchanged payload, including keys outside print.
// Quoted braces and escaped quotes must not terminate an object.
bool readObject(JsonDocument& doc, const uint8_t* data, size_t length, const char* key) {
  const char* end = (const char*)data + length;
  const size_t keyLen = strlen(key);
  for (const char* p = (const char*)data; p < end;) {
    if (*p++ != '"') continue;
    const char* name = p;
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) ++p;
      ++p;
    }
    if (p == end) return false;
    const bool match = (size_t)(p - name) == keyLen && memcmp(name, key, keyLen) == 0;
    p = skipSpace(p + 1, end);
    if (!match || p == end || *p != ':') continue;
    const char* start = skipSpace(p + 1, end);
    if (start == end || *start != '{') continue;
    p = start;
    int depth = 0;
    bool quoted = false;
    do {
      const char ch = *p++;
      if (quoted && ch == '\\' && p < end) { ++p; continue; }
      if (ch == '"') quoted = !quoted;
      else if (!quoted && ch == '{') ++depth;
      else if (!quoted && ch == '}') --depth;
    } while (p < end && depth);
    return depth == 0 && !deserializeJson(doc, start, (size_t)(p - start), DeserializationOption::NestingLimit(32));
  }
  return false;
}

void parseTray(JsonObjectConst obj, PrinterTray& tray, TrayNames& names) {
  const char* type = obj["tray_type"];
  const char* brand = obj["tray_sub_brands"];
  if (type && !*type && (!brand || !*brand)) {
    tray = PrinterTray{};
    names = TrayNames{};
    return;
  }
  if (type) strlcpy(names.type, type, sizeof(names.type));
  if (brand) strlcpy(names.brand, brand, sizeof(names.brand));
  if (type || brand) {
    const char* name = names.brand[0] ? names.brand : names.type;
    tray.present = *name != 0;
    strlcpy(tray.type, name, sizeof(tray.type));
  }
  const char* color = obj["tray_color"];
  if (!color || strlen(color) < 6) color = obj["cols"][0];
  if (color && (strlen(color) == 6 || strlen(color) == 8)) {
    char* end;
    const uint32_t rgba = strtoul(color, &end, 16);
    if (!*end) {
      const bool alpha = strlen(color) == 8;
      tray.rgb = alpha ? rgba >> 8 : rgba;
      tray.alpha = alpha ? rgba & 255 : 255;
    }
  }
  if (number(obj["remain"])) {
    const long remain = integer(obj["remain"]);
    tray.remain = remain > 0 && remain <= 100 ? remain : -1;
  }
}

void activeTray(Session& c) {
  PrinterState& s = c.state;
  uint16_t unit, slot;
  if (c.snowSeen[s.activeNozzle]) {
    unit = c.snow[s.activeNozzle] >> 8;
    slot = c.snow[s.activeNozzle] & 3;
  } else {
    unit = c.trayNow >= 254 ? c.trayNow : c.trayNow / 4;
    slot = c.trayNow % 4;
  }
  s.activeUnit = unit == 254 ? 254 : 255;
  s.activeSlot = 255;
  if (unit >= 254) return;
  for (uint8_t i = 0; i < s.amsCount; ++i) {
    if (!s.ams[i].present || s.ams[i].id != unit || slot >= s.ams[i].slots) continue;
    s.activeUnit = i;
    s.activeSlot = slot;
    break;
  }
}

void report(void* context, const char* topic, const uint8_t* payload, size_t length) {
  Session& c = *(Session*)context;
  if (!current(c.index, c.generation) || String(topic) != "device/" + c.conn.serial + "/report") return;
  PsramAllocator allocator;
  JsonDocument filter(&allocator), doc(&allocator), sub(&allocator);
  JsonObject pf = filter["print"].to<JsonObject>();
  for (const char* key : {"gcode_state", "subtask_name", "task_id", "mapping", "mc_percent",
                         "mc_remaining_time", "layer_num", "total_layer_num", "spd_lvl",
                         "nozzle_temper", "nozzle_target_temper", "bed_temper",
                         "bed_target_temper", "chamber_temper", "lights_report", "stg_cur"}) pf[key] = true;
  if (filter.overflowed()) { link(c, PRN_FAILED, "No PSRAM for JSON filter"); return; }
  const auto error = deserializeJson(doc, (const char*)payload, length,
                                    DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(32));
  if (error) { link(c, PRN_FAILED, error.c_str()); return; }
  PrinterState& s = c.state;
  JsonObjectConst p = doc["print"];
  bool core = false;
  bool snowUpdated[2] = {};
  if (readObject(sub, payload, length, "extruder")) {
    JsonArrayConst info = sub["info"];
    if (info.size() >= 2) s.dualNozzle = true;
    for (JsonObjectConst entry : info) if (integer(entry["id"]) == 1) s.dualNozzle = true;
    if (s.dualNozzle && number(sub["state"])) {
      s.activeNozzle = (sub["state"].as<uint32_t>() >> 4) & 15;
      if (s.activeNozzle > 1) s.activeNozzle = 0;
    }
    for (JsonObjectConst entry : info) {
      if (!number(entry["id"])) continue;
      const long id = integer(entry["id"]);
      if (id < 0 || id > 1) continue;
      if (number(entry["temp"])) {
        const uint32_t packed = entry["temp"].as<uint32_t>();
        s.nozzle[id] = packed & 0xffff;
        s.nozzleTarget[id] = packed >> 16;
        core = true;
      }
      if (number(entry["snow"])) {
        c.snow[id] = entry["snow"].as<uint16_t>();
        c.snowSeen[id] = true;
        snowUpdated[id] = true;
      }
    }
  }
  sub.clear();
  if (readObject(sub, payload, length, "ctc") && number(sub["info"]["temp"])) {
    s.chamber = sub["info"]["temp"].as<uint32_t>() & 0xffff;
    c.ctcSeen = true;
    core = true;
  }
  sub.clear();
  if (readObject(sub, payload, length, "ams")) {
    JsonArrayConst units = sub["ams"];
    if (!units.isNull() && units.size() == 0) {
      s.amsCount = 0;
      for (auto& unit : s.ams) unit = PrinterAmsUnit{};
      for (auto& unit : c.trayNames) for (auto& names : unit) names = TrayNames{};
    }
    for (JsonObjectConst obj : units) {
      if (!number(obj["id"])) continue;
      const long id = integer(obj["id"]);
      if (id < 0 || id >= 254) continue;
      uint8_t index = 0;
      while (index < s.amsCount && s.ams[index].id != id) ++index;
      if (index == PRN_AMS_UNITS) continue;
      if (index == s.amsCount) {
        ++s.amsCount;
        s.ams[index] = PrinterAmsUnit{};
        s.ams[index].id = id;
        s.ams[index].slots = id >= 128 ? 1 : 4;
      }
      PrinterAmsUnit& unit = s.ams[index];
      unit.present = true;
      if (number(obj["humidity_raw"])) {
        const long rh = integer(obj["humidity_raw"]);
        unit.humidity = rh >= 0 && rh <= 100 ? rh : 255;
      }
      if (number(obj["temp"])) unit.temp = obj["temp"].as<float>();
      if (number(obj["dry_time"])) unit.dryRemainMin = constrain(integer(obj["dry_time"]), 0L, 65535L);
      for (JsonObjectConst tray : obj["tray"].as<JsonArrayConst>()) {
        if (!number(tray["id"])) continue;
        const long id = integer(tray["id"]);
        if (id >= 0 && id < unit.slots) parseTray(tray, unit.tray[id], c.trayNames[index][id]);
      }
    }
    if (number(sub["tray_now"])) {
      c.trayNow = sub["tray_now"].as<uint16_t>();
      if (!s.dualNozzle && !snowUpdated[0]) c.snowSeen[0] = false;
    }
  }
  sub.clear();
  if (readObject(sub, payload, length, "vt_tray")) {
    parseTray(sub.as<JsonObjectConst>(), s.ext, c.extNames);
    s.extPresent = s.ext.present;
  }
  activeTray(c);
  if (p["gcode_state"].is<const char*>()) {
    strlcpy(s.gcodeState, p["gcode_state"], sizeof(s.gcodeState));
    core = true;
  }
  if (p["subtask_name"].is<const char*>()) {
    const char* name = p["subtask_name"];
    if (strncmp(s.job, name, sizeof(s.job) - 1)) s.mapCount = 0;
    strlcpy(s.job, name, sizeof(s.job));
    core = true;
  }
  if (!p["task_id"].isNull()) {
    const String id = p["task_id"].as<String>();
    if (c.taskId[0] && id != c.taskId) s.mapCount = 0;
    strlcpy(c.taskId, id.c_str(), sizeof(c.taskId));
  }
  if (p["mapping"].is<JsonArrayConst>()) {
    s.mapCount = 0;
    for (JsonVariantConst value : p["mapping"].as<JsonArrayConst>()) {
      if (!number(value)) continue;
      const long raw = integer(value);
      if (raw >= 0 && raw < 65535 && s.mapCount < PRN_MAP_MAX) s.map[s.mapCount++] = raw;
    }
  }
  auto mergeInt = [&](const char* key, auto& out, long limit) {
    if (number(p[key])) { out = constrain(integer(p[key]), 0L, limit); core = true; }
  };
  auto mergeTemp = [&](const char* key, float& out) {
    if (number(p[key])) { out = p[key].as<float>(); core = true; }
  };
  mergeInt("mc_percent", s.progress, 100);
  mergeInt("mc_remaining_time", s.remainingMin, 65535);
  mergeInt("layer_num", s.layer, 65535);
  mergeInt("total_layer_num", s.totalLayers, 65535);
  mergeInt("spd_lvl", s.speedLevel, 4);
  if (number(p["stg_cur"])) {
    const long stage = integer(p["stg_cur"]);
    s.stage = stage < 0 || stage >= 255 ? -1 : stage;
    core = true;
  }
  if (!s.dualNozzle) {
    mergeTemp("nozzle_temper", s.nozzle[0]);
    mergeTemp("nozzle_target_temper", s.nozzleTarget[0]);
  }
  mergeTemp("bed_temper", s.bed);
  mergeTemp("bed_target_temper", s.bedTarget);
  // P1 reports a bogus chamber value. Only known X1 sensors use this field.
  if (!c.ctcSeen && (c.conn.serial.startsWith("00M") || c.conn.serial.startsWith("03W")))
    mergeTemp("chamber_temper", s.chamber);
  for (JsonObjectConst light : p["lights_report"].as<JsonArrayConst>()) {
    const char* node = light["node"] | "";
    const char* mode = light["mode"];
    if (!strcmp(node, "chamber_light") && mode) s.light = strcmp(mode, "off") ? 1 : 0;
    if (!strcmp(node, "chamber_light2")) c.secondLight = true;
  }
  s.lastReportMs = millis();
  if (core) c.lastCore = s.lastReportMs;
  c.gotReport = true;
  c.backoff = 5000;
  if (c.io.subscribed) { s.link = PRN_ONLINE; s.err[0] = 0; }
  publishState(c);
}

bool pushall(Session& c) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"pushing\":{\"sequence_id\":\"%lu\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}",
           (unsigned long)c.sequence++);
  const bool ok = c.mqtt.publish(("device/" + c.conn.serial + "/request").c_str(), payload);
  if (ok) c.lastPush = millis();
  return ok;
}

bool sendLight(Session& c, const char* node, bool on) {
  char payload[240];
  snprintf(payload, sizeof(payload),
           "{\"system\":{\"sequence_id\":\"%lu\",\"command\":\"ledctrl\",\"led_node\":\"%s\","
           "\"led_mode\":\"%s\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}",
           (unsigned long)c.sequence++, node, on ? "on" : "off");
  return c.mqtt.publish(("device/" + c.conn.serial + "/request").c_str(), payload);
}

void failed(Session& c, const char* error) {
  c.io.stop();
  c.connected = false;
  c.nextTry = millis() + c.backoff;
  c.backoff = std::min(c.backoff * 2, (uint32_t)60000);
  {
    Lock lock;
    if (s_slots[c.index].generation == c.generation) s_slots[c.index].light = -1;
  }
  link(c, PRN_FAILED, error);
}

void service(Session& c) {
  if (!current(c.index, c.generation)) return;
  if (!c.mqtt.connected()) {
    if (c.connected) { failed(c, c.io.error ? c.io.error : "MQTT disconnected"); return; }
    if (c.nextTry && (int32_t)(millis() - c.nextTry) < 0) return;
    if (WiFi.status() != WL_CONNECTED) { failed(c, "WiFi is offline"); return; }
    IPAddress address;
    if (!address.fromString(c.conn.ip) || !c.conn.serial.length() || !c.conn.code.length() || c.conn.serial.length() > 64) {
      failed(c, "Invalid LAN credentials");
      return;
    }
    link(c, PRN_CONNECTING);
    c.io.stop();
    c.mqtt.setServer(address, 8883);
    char clientId[40];
    snprintf(clientId, sizeof(clientId), "touchdeck_%08lx_%d", (unsigned long)esp_random(), c.index);
    if (!c.mqtt.connect(clientId, "bblp", c.conn.code.c_str())) {
      char error[48];
      snprintf(error, sizeof(error), "MQTT connect failed (%d)", c.mqtt.state());
      failed(c, error);
      return;
    }
    if (!current(c.index, c.generation)) { c.io.stop(); return; }
    c.connected = true;
    c.gotReport = false;
    c.connectedMs = c.lastCore = millis();
    if (!c.mqtt.subscribe(("device/" + c.conn.serial + "/report").c_str(), 0) || !pushall(c)) {
      failed(c, "MQTT subscribe/pushall failed");
      return;
    }
  }
  if (!c.mqtt.loop()) { failed(c, c.io.error ? c.io.error : "MQTT connection lost"); return; }
  const uint32_t now = millis();
  if (!c.io.subscribed && now - c.connectedMs > 10000) { failed(c, "MQTT subscription timeout"); return; }
  if (c.gotReport && c.io.subscribed && c.state.link == PRN_CONNECTING) link(c, PRN_ONLINE);
  if (now - (c.gotReport ? c.state.lastReportMs : c.connectedMs) > 90000) {
    failed(c, "No printer reports");
    return;
  }
  const bool stale = now - c.lastCore > 60000;
  if (now - c.lastPush >= (!c.gotReport || stale ? 10000u : 30000u)) {
    if (!pushall(c)) { failed(c, "Pushall publish failed"); return; }
  }
  int8_t light = -1;
  {
    Lock lock;
    Slot& slot = s_slots[c.index];
    if (slot.powered && slot.generation == c.generation) {
      light = slot.light;
      slot.light = -1;
    }
  }
  if (light >= 0) {
    bool ok = sendLight(c, "chamber_light", light);
    if (c.secondLight || c.state.dualNozzle) ok = sendLight(c, "chamber_light2", light) && ok;
    if (!ok) failed(c, "Light publish failed");
  }
}

void destroy(Session*& c) {
  if (!c) return;
  {
    Lock lock;
    Slot& slot = s_slots[c->index];
    if (slot.state == &c->snapshot) slot.state = nullptr;
  }
  c->~Session();
  heap_caps_free(c);
  c = nullptr;
}

void task(void*) {
  static bool tlsInstalled = false;
  if (!tlsInstalled) tlsInstalled = mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free) == 0;
  Session* sessions[PRN_MAX] = {};
  uint32_t allocationRetry[PRN_MAX] = {}, seenGeneration[PRN_MAX] = {};
  uint32_t lastStackLog = millis();
  for (;;) {
    for (int i = 0; i < PRN_MAX; ++i) {
      PrinterConn conn;
      uint32_t generation;
      bool powered;
      {
        Lock lock;
        const Slot& slot = s_slots[i];
        powered = slot.powered;
        generation = slot.generation;
        if (powered && (!sessions[i] || sessions[i]->generation != generation)) conn = slot.conn;
      }
      if (sessions[i] && (!powered || sessions[i]->generation != generation)) destroy(sessions[i]);
      if (seenGeneration[i] != generation) { seenGeneration[i] = generation; allocationRetry[i] = 0; }
      if (powered && !sessions[i] && (!allocationRetry[i] || (int32_t)(millis() - allocationRetry[i]) >= 0)) {
        const char* error = tlsInstalled ? "No PSRAM for printer" : "TLS allocator setup failed";
        void* memory = tlsInstalled ? heap_caps_malloc(sizeof(Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
        if (memory) {
          sessions[i] = new (memory) Session(conn, i, generation);
          Session& c = *sessions[i];
          c.tls.setInsecure();
          c.tls.setHandshakeTimeout(5);
          c.tls.setTimeout(3000);
          c.mqtt.setKeepAlive(60);
          c.mqtt.setSocketTimeout(5);
          c.ready = c.io.begin(report, &c) && c.mqtt.setBufferSize(512);
          if (!c.ready) destroy(sessions[i]);
          else link(c, PRN_CONNECTING);
        }
        if (!sessions[i]) {
          allocationRetry[i] = millis() + 5000;
          Lock lock;
          Slot& slot = s_slots[i];
          if (slot.powered && slot.generation == generation) {
            slot.link = PRN_FAILED;
            strlcpy(slot.err, error, sizeof(slot.err));
            ++s_version;
          }
        }
      }
      if (sessions[i]) service(*sessions[i]);
    }
    // Release stopped sessions before allowing a replacement worker to start.
    for (int i = 0; i < PRN_MAX; ++i)
      if (sessions[i] && !current(i, sessions[i]->generation)) destroy(sessions[i]);
    bool quit = true;
    {
      Lock lock;
      for (int i = 0; i < PRN_MAX; ++i) if (s_slots[i].powered || sessions[i]) quit = false;
      if (quit) s_task = nullptr;
    }
    if (quit) break;
    if (millis() - lastStackLog >= 180000) {
      Serial.printf("[bambu] stack free minimum: %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
      lastStackLog = millis();
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
  Serial.printf("[bambu] stopped, stack free minimum: %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  vTaskDelete(nullptr);
}

void invalidate(Slot& slot) {
  slot.generation = ++s_generation;
  slot.state = nullptr;
  slot.light = -1;
  slot.link = slot.powered ? PRN_CONNECTING : PRN_OFF;
  slot.err[0] = 0;
  ++s_version;
}

void wake() { if (s_task) xTaskNotifyGive(s_task); }

}  // namespace

void printersSet(const std::vector<PrinterConn>& list) {
  const PrinterConn* wanted[PRN_MAX] = {};
  size_t count = 0;
  for (const auto& conn : list) {
    if (!conn.id.length()) continue;
    bool duplicate = false;
    for (size_t i = 0; i < count; ++i) if (wanted[i]->id == conn.id) duplicate = true;
    if (!duplicate) wanted[count++] = &conn;
    if (count == PRN_MAX) break;
  }
  Lock lock;
  bool keep[PRN_MAX] = {};
  for (size_t n = 0; n < count; ++n) {
    const auto& conn = *wanted[n];
    int index = -1;
    for (int i = 0; i < PRN_MAX; ++i) if (s_slots[i].conn.id == conn.id) index = i;
    if (index < 0) continue;
    keep[index] = true;
    Slot& slot = s_slots[index];
    if (slot.conn.ip != conn.ip || slot.conn.serial != conn.serial || slot.conn.code != conn.code) {
      slot.conn = conn;
      invalidate(slot);
    }
  }
  for (int i = 0; i < PRN_MAX; ++i) if (!keep[i] && s_slots[i].conn.id.length()) {
    s_slots[i].powered = false;
    s_slots[i].conn = PrinterConn{};
    invalidate(s_slots[i]);
  }
  for (size_t n = 0; n < count; ++n) {
    const auto& conn = *wanted[n];
    bool exists = false;
    for (const auto& slot : s_slots) if (slot.conn.id == conn.id) exists = true;
    if (exists) continue;
    for (auto& slot : s_slots) if (!slot.conn.id.length()) {
      slot.conn = conn;
      invalidate(slot);
      break;
    }
  }
  wake();
}

void printerPower(const String& id, bool on) {
  Lock lock;
  for (auto& slot : s_slots) {
    if (!id.length() || slot.conn.id != id) continue;
    if (slot.powered != on) { slot.powered = on; invalidate(slot); }
    if (on && !s_task) {
      // IDF's dynamic task allocator uses internal RAM, as required by TLS.
      if (xTaskCreatePinnedToCore(task, "deck_bambu", STACK_SIZE, nullptr, 3, &s_task, 0) != pdPASS) {
        s_task = nullptr;
        slot.link = PRN_FAILED;
        strlcpy(slot.err, "Cannot start printer worker", sizeof(slot.err));
        ++s_version;
      }
    }
    wake();
    break;
  }
}

bool printerPowered(const String& id) {
  Lock lock;
  for (const auto& slot : s_slots) if (id.length() && slot.conn.id == id) return slot.powered;
  return false;
}

bool printerRead(const String& id, PrinterState& out) {
  Lock lock;
  for (const auto& slot : s_slots) {
    if (!id.length() || slot.conn.id != id || !slot.powered) continue;
    if (slot.state) out = *slot.state;
    else {
      out = PrinterState{};
      out.link = slot.link;
      strlcpy(out.err, slot.err, sizeof(out.err));
    }
    return true;
  }
  return false;
}

uint32_t printersVersion() { Lock lock; return s_version; }

bool printerLight(const String& id, bool on) {
  Lock lock;
  for (auto& slot : s_slots) {
    if (!id.length() || slot.conn.id != id || !slot.powered || !slot.state || slot.state->link != PRN_ONLINE) continue;
    slot.light = on ? 1 : 0;
    wake();
    return true;
  }
  return false;
}

// Stage ids from BambuStudio's CURRENT_STAGE_IDS; the paused ones flip
// gcode_state to PAUSE and are left out.
const char* printerStageName(int stage) {
  switch (stage) {
    case 1: return "Bed Leveling";
    case 2: return "Preheating Bed";
    case 3: return "Sweeping XY";
    case 4: return "Changing Filament";
    case 7: return "Heating Nozzle";
    case 8: case 19: return "Calibrating Flow";
    case 9: return "Scanning Bed";
    case 10: return "Inspecting Layer 1";
    case 11: return "Detecting Plate";
    case 12: case 18: return "Calibrating Lidar";
    case 13: return "Homing";
    case 14: return "Cleaning Nozzle";
    case 15: return "Checking Temp";
    case 22: return "Unloading Filament";
    case 24: return "Loading Filament";
    case 25: case 31: return "Motor Noise Cali";
    case 29: return "Cooling Chamber";
    case 36: return "Accuracy Check";
    case 37: return "Accuracy Cali";
    case 38: return "Accuracy Verify";
    case 39: return "Nozzle Offset Cali";
    case 40: return "Hot Bed Leveling";
    case 41: return "Quick Release Check";
    case 42: return "Door Check";
    case 43: return "Laser Cali";
    case 44: return "Platform Check";
    case 45: return "Camera Position Check";
    case 46: return "Camera Cali";
    case 47: return "Bed Leveling 1";
    case 48: return "Bed Leveling 2";
    case 49: return "Heating Chamber";
    case 50: return "Cooling Bed";
    case 51: return "Cali Lines";
    case 52: return "Checking Material";
    case 53: return "Live Camera Cali";
    case 54: return "Waiting for Bed";
    case 55: return "Material Position Check";
    case 56: return "Cutter Offset Cali";
    case 57: return "Measuring Surface";
    case 58: return "Thermal Precondition";
    case 59: return "Homing Blade";
    case 60: return "Camera Offset Cali";
    case 61: return "Blade Position Cali";
    case 62: return "Hotend Test";
    case 63: return "Chamber Temp Wait";
    case 64: return "Preparing Hotend";
    case 65: return "Nozzle Clump Cali";
    case 66: return "Purifying Air";
    case 67: return "Rotary Measure";
    case 68: return "Moving to Purge";
    case 69: return "Cooling Nozzle";
    case 70: return "Centering Head";
    case 71: return "Arc Fitting";
    case 72: return "Hotend Detect";
    case 73: return "Plate Align Check";
    case 74: return "Foreign Object Check";
    case 75: return "Bed Underside Check";
    case 76: return "Pre-Extruding";
    case 77: return "Preparing AMS";
    default: return nullptr;
  }
}

}  // namespace deck
