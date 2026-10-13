// Guition deck: Home Assistant client. See deck_ha.h.
#include "deck_ha.h"
#include "deck_config.h"
#include <LittleFS.h>
#include <time.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <algorithm>
#include <deque>
#include <new>

namespace deck {
namespace {

// The WebSocket library drops any frame over 15 KB, and the first frame of a
// subscription carries every attribute of every entity in it. A few entities
// per subscription keeps that frame small even for chatty lights.
constexpr size_t SUB_BATCH = 4;
constexpr uint32_t RECONNECT_MS = 5000;
constexpr size_t MAX_QUEUED_CALLS = 8;

// ArduinoJson grows a document in small blocks, and small blocks land in
// internal RAM. A filtered /api/states of a real installation is hundreds of
// blocks; it emptied the internal heap and stalled the portal's sockets.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
ArduinoJson::Allocator* psram() {
  static PsramAllocator a;
  return &a;
}

SemaphoreHandle_t s_lock = nullptr;
struct Lock {
  Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_lock); }
};

// --- guarded by s_lock ---
String s_url, s_token;
uint32_t s_credGen = 1;
HaLink s_link = HA_NOT_CONFIGURED;
String s_error, s_haVersion;
struct Entry {
  String id;
  HaEntity e;
};
std::vector<Entry> s_store;
std::vector<String> s_wanted;
uint32_t s_wantedGen = 1;
uint32_t s_version = 1;
struct Call {
  String domain, service, target, data;
  uint32_t tracking;
};
struct CallResultSlot {
  uint32_t id = 0;
  bool ready = false;
  HaCallResult result;
};
CallResultSlot s_results[16];
uint32_t s_nextTracking = 0;
std::deque<Call> s_calls;
struct Failure {
  String entity, message;
};
std::deque<Failure> s_failures;

// --- HA task only ---
WebSocketsClient s_ws;
bool s_started = false;
uint32_t s_appliedCredGen = 0;
uint32_t s_subscribedGen = 0;
uint32_t s_nextId = 1;
std::vector<uint32_t> s_subIds;
struct Pending {
  uint32_t id;
  String entity;
  uint32_t tracking;
};
std::vector<Pending> s_pending;
bool s_everConnected = false;

// --- History store and worker handle, guarded by s_lock ---
struct HistoryData {
  float value[HA_HISTORY_POINTS], weight[HA_HISTORY_POINTS];
  float low[HA_HISTORY_POINTS], high[HA_HISTORY_POINTS];
  time_t end = 0, cursor = 0;
  float live = NAN;
};
struct History {
  HaHistoryKey key;
  HistoryData data, tail;
  bool configured = false, detail = false, fetching = false;
  uint32_t id = 0, request = 0, version = 0, due = 0;
};
std::vector<History*> s_history;
uint32_t s_histVersion = 1, s_histId = 0;
TaskHandle_t s_histTask = nullptr;
void historyTask(void*);

void clearHistory(HistoryData& d, time_t end, float live) {
  for (int i = 0; i < HA_HISTORY_POINTS; i++) {
    d.value[i] = d.low[i] = d.high[i] = NAN;
    d.weight[i] = 0;
  }
  d.end = d.cursor = end;
  d.live = live;
}

void addHistory(HistoryData& d, int i, float value, float seconds) {
  if (!isfinite(value) || seconds <= 0) return;
  const float total = d.weight[i] + seconds;
  d.value[i] = d.weight[i] > 0 ? (float)(((double)d.value[i] * d.weight[i] +
                                       (double)value * seconds) / total) : value;
  d.weight[i] = total;
  d.low[i] = isnan(d.low[i]) ? value : min(d.low[i], value);
  d.high[i] = isnan(d.high[i]) ? value : max(d.high[i], value);
}

void shiftHistory(HistoryData& d, time_t end, int width) {
  const int n = (int)min((time_t)HA_HISTORY_POINTS, (end - d.end) / width);
  if (n <= 0) return;
  for (int i = 0; i < HA_HISTORY_POINTS; i++) {
    d.value[i] = i + n < HA_HISTORY_POINTS ? d.value[i + n] : NAN;
    d.weight[i] = i + n < HA_HISTORY_POINTS ? d.weight[i + n] : 0;
    d.low[i] = i + n < HA_HISTORY_POINTS ? d.low[i + n] : NAN;
    d.high[i] = i + n < HA_HISTORY_POINTS ? d.high[i + n] : NAN;
  }
  d.end = end;
}

bool advanceHistory(HistoryData& d, uint8_t hours, time_t now) {
  if (!d.end || now <= d.cursor) return false;
  const int width = hours * 3600 / HA_HISTORY_POINTS;
  const time_t end = now > d.end ? d.end + ((now - d.end + width - 1) / width) * width : d.end;
  const bool shifted = end != d.end;
  shiftHistory(d, end, width);
  const time_t start = d.end - hours * 3600;
  time_t at = max(d.cursor, start);
  bool changed = shifted;
  while (at < now) {
    const int i = min(HA_HISTORY_POINTS - 1, (int)((at - start) / width));
    const time_t to = min(now, start + (i + 1) * width);
    const float before = d.value[i], low = d.low[i], high = d.high[i];
    addHistory(d, i, d.live, (float)(to - at));
    changed = changed || (isfinite(before) != isfinite(d.value[i])) ||
              (isfinite(before) && (before != d.value[i] || low != d.low[i] || high != d.high[i]));
    at = to;
  }
  d.cursor = now;
  return changed;
}

// Called with s_lock held; integrate the old state before accepting the new one.
void historyLive(time_t now) {
  if (now < 1609459200) return;
  for (History* h : s_history) {
    bool changed = advanceHistory(h->data, h->key.hours, now);
    float value = NAN;
    if (s_link == HA_CONNECTED)
      for (const Entry& en : s_store)
        if (en.id == h->key.entity && en.e.known) { haNumericState(en.e.state, value); break; }
    if (!h->data.end) clearHistory(h->data, now, value);
    h->data.live = value;
    if (h->fetching) {
      advanceHistory(h->tail, h->key.hours, now);
      h->tail.live = value;
    }
    if (changed) h->version = ++s_histVersion;
  }
}

History* findHistory(const String& entity, uint8_t hours) {
  for (History* h : s_history)
    if (h->key.entity == entity && h->key.hours == hours) return h;
  return nullptr;
}

History* ensureHistory(const String& entity, uint8_t hours) {
  if (!entity.length()) return nullptr;
  if (History* h = findHistory(entity, hours)) return h;
  void* mem = heap_caps_malloc(sizeof(History), MALLOC_CAP_SPIRAM);
  if (!mem) return nullptr;
  History* h = new (mem) History;
  h->key.entity = entity;
  h->key.hours = hours;
  h->id = ++s_histId;
  clearHistory(h->data, 0, NAN);
  clearHistory(h->tail, 0, NAN);
  s_history.push_back(h);
  return h;
}

void pruneHistory() {
  for (auto it = s_history.begin(); it != s_history.end();) {
    History* h = *it;
    if (h->configured || h->detail) { ++it; continue; }
    h->~History();
    heap_caps_free(h);
    it = s_history.erase(it);
  }
}

void startHistoryTask() {
  if (!s_histTask && !s_history.empty())
    xTaskCreatePinnedToCore(historyTask, "deck_hist", 6144, nullptr, 2, &s_histTask, 0);
}

void setLink(HaLink l, const String& err = String()) {
  Lock g;
  if (s_link != l || (err.length() && err != s_error)) s_version++;
  const bool connected = l == HA_CONNECTED && s_link != l;
  historyLive(time(nullptr));
  s_link = l;
  if (connected) for (History* h : s_history) { h->due = 0; h->request++; }
  if (l != HA_CONNECTED)
    for (History* h : s_history) h->data.live = h->tail.live = NAN;
  if (err.length()) s_error = err;
  if (l == HA_CONNECTED) s_error = "";
}

void loadCredentials() {
  Preferences p;
  if (p.begin("deckha", true)) {
    s_url = p.getString("url", "");
    s_token = p.getString("token", "");
    p.end();
  }
}

// "http://host:port" (or "host:port", "host") -> host, port.
bool parseUrl(const String& url, String& host, uint16_t& port, String& err) {
  String u = url;
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.startsWith("https://")) {
    err = "https is not supported yet - use the LAN http address";
    return false;
  }
  if (u.startsWith("http://")) u = u.substring(7);
  const int slash = u.indexOf('/');
  if (slash >= 0) u = u.substring(0, slash);
  const int colon = u.indexOf(':');
  if (colon >= 0) {
    host = u.substring(0, colon);
    port = (uint16_t)u.substring(colon + 1).toInt();
  } else {
    host = u;
    port = 8123;
  }
  if (!host.length() || port == 0) {
    err = "invalid Home Assistant address";
    return false;
  }
  return true;
}

void sendJson(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
}

void entityFromAttrs(HaEntity& e, JsonObjectConst a) {
  if (a.isNull()) return;
  if (a["friendly_name"].is<const char*>()) e.name = (const char*)a["friendly_name"];
  if (a["icon"].is<const char*>()) e.icon = (const char*)a["icon"];
  if (a["unit_of_measurement"].is<const char*>()) e.unit = (const char*)a["unit_of_measurement"];
  if (a["device_class"].is<const char*>()) e.deviceClass = (const char*)a["device_class"];
  // Present-but-null (a light turning off) must clear it, so look at the
  // keys rather than at a["brightness"], which cannot tell null from absent.
  for (JsonPairConst kv : a) {
    const char* k = kv.key().c_str();
    if (!strcmp(k, "brightness")) {
      e.brightness = kv.value().is<int>() ? kv.value().as<int>() : -1;
    } else if (!strcmp(k, "current_position") || !strcmp(k, "percentage")) {
      e.level = kv.value().is<float>() ? (int)lroundf(kv.value().as<float>()) : -1;
    } else if (!strcmp(k, "volume_level")) {
      e.level = kv.value().is<float>() ? (int)lroundf(kv.value().as<float>() * 100) : -1;
    }
  }
  if (a["min"].is<float>()) e.numMin = a["min"].as<float>();
  if (a["max"].is<float>()) e.numMax = a["max"].as<float>();
  if (a["step"].is<float>()) e.numStep = a["step"].as<float>();
}

Entry* findEntry(const char* id) {
  for (Entry& en : s_store)
    if (en.id == id) return &en;
  return nullptr;
}

// subscribe_entities event: "a" = full states, "c" = diffs, "r" = removed.
void applyEvent(JsonObjectConst ev) {
  Lock g;
  bool changed = false;
  for (JsonPairConst kv : ev["a"].as<JsonObjectConst>()) {
    Entry* en = findEntry(kv.key().c_str());
    if (!en) continue;
    HaEntity& e = en->e;
    e.known = true;
    e.state = (const char*)(kv.value()["s"] | "");
    e.name = "";
    e.icon = "";
    e.unit = "";
    e.deviceClass = "";
    e.brightness = -1;
    e.level = -1;
    e.numMin = e.numMax = e.numStep = NAN;
    entityFromAttrs(e, kv.value()["a"].as<JsonObjectConst>());
    e.version++;
    changed = true;
  }
  for (JsonPairConst kv : ev["c"].as<JsonObjectConst>()) {
    Entry* en = findEntry(kv.key().c_str());
    if (!en) continue;
    HaEntity& e = en->e;
    JsonObjectConst plus = kv.value()["+"];
    if (!plus.isNull()) {
      if (plus["s"].is<const char*>()) e.state = (const char*)plus["s"];
      entityFromAttrs(e, plus["a"].as<JsonObjectConst>());
    }
    for (JsonVariantConst k : kv.value()["-"]["a"].as<JsonArrayConst>()) {
      const char* key = k | "";
      if (!strcmp(key, "brightness")) e.brightness = -1;
      else if (!strcmp(key, "current_position") || !strcmp(key, "percentage") ||
               !strcmp(key, "volume_level"))
        e.level = -1;
      else if (!strcmp(key, "icon")) e.icon = "";
      else if (!strcmp(key, "unit_of_measurement")) e.unit = "";
      else if (!strcmp(key, "device_class")) e.deviceClass = "";
      else if (!strcmp(key, "friendly_name")) e.name = "";
    }
    e.version++;
    changed = true;
  }
  for (JsonVariantConst r : ev["r"].as<JsonArrayConst>()) {
    Entry* en = findEntry(r | "");
    if (!en) continue;
    en->e.known = false;
    en->e.version++;
    changed = true;
  }
  if (changed) { s_version++; historyLive(time(nullptr)); }
}

// Called with s_lock held. Keep the state version and time at the actual result.
void finishCall(uint32_t tracking, const String& entity, bool ok) {
  if (!tracking) return;
  for (CallResultSlot& slot : s_results) {
    if (slot.id != tracking) continue;
    const Entry* en = findEntry(entity.c_str());
    slot.result = {ok, millis(), en ? en->e.version : 0};
    slot.ready = true;
    return;
  }
}

void failPending(const String& message) {
  Lock g;
  for (const Pending& p : s_pending) {
    finishCall(p.tracking, p.entity, false);
    s_failures.push_back({p.entity, message});
  }
  s_pending.clear();
  for (const Call& c : s_calls) {
    finishCall(c.tracking, c.target, false);
    s_failures.push_back({c.target, message});
  }
  s_calls.clear();
}

void handleText(const uint8_t* payload, size_t len) {
  JsonDocument doc(psram());
  if (deserializeJson(doc, payload, len)) return;
  const char* type = doc["type"] | "";
  if (!strcmp(type, "auth_required")) {
    String token;
    {
      Lock g;
      token = s_token;
    }
    JsonDocument a(psram());
    a["type"] = "auth";
    a["access_token"] = token;
    sendJson(a);
    setLink(HA_AUTHENTICATING);
  } else if (!strcmp(type, "auth_ok")) {
    {
      Lock g;
      s_haVersion = (const char*)(doc["ha_version"] | "");
    }
    s_everConnected = true;
    s_subscribedGen = 0;  // subscribe from scratch on every connection
    s_subIds.clear();
    setLink(HA_CONNECTED);
    Serial.printf("HA: connected, Home Assistant %s\n", (const char*)(doc["ha_version"] | "?"));
  } else if (!strcmp(type, "auth_invalid")) {
    setLink(HA_AUTH_FAILED, String("token rejected: ") + (const char*)(doc["message"] | ""));
    Serial.println("HA: authentication failed, waiting for new settings");
    s_ws.disconnect();
  } else if (!strcmp(type, "event")) {
    applyEvent(doc["event"].as<JsonObjectConst>());
  } else if (!strcmp(type, "result")) {
    const uint32_t id = doc["id"] | 0;
    const bool ok = doc["success"] | false;
    for (size_t i = 0; i < s_pending.size(); i++) {
      if (s_pending[i].id != id) continue;
      {
        Lock g;
        finishCall(s_pending[i].tracking, s_pending[i].entity, ok);
        if (!ok)
          s_failures.push_back({s_pending[i].entity, (const char*)(doc["error"]["message"] | "service call failed")});
      }
      s_pending.erase(s_pending.begin() + i);
      return;
    }
    if (!ok) Serial.printf("HA: request %u failed: %s\n", (unsigned)id,
                           (const char*)(doc["error"]["message"] | "?"));
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      setLink(HA_AUTHENTICATING);
      break;
    case WStype_DISCONNECTED: {
      HaLink l;
      {
        Lock g;
        l = s_link;
      }
      if (l != HA_AUTH_FAILED) setLink(s_everConnected ? HA_CONNECTING : HA_UNREACHABLE,
                                       s_everConnected ? String() : String("no answer from Home Assistant"));
      s_subscribedGen = 0;
      s_subIds.clear();
      // Never replay an uncertain call after a reconnect: report it instead.
      failPending("connection to Home Assistant lost");
      break;
    }
    case WStype_TEXT:
      handleText(payload, length);
      break;
    default:
      break;
  }
}

void resubscribe() {
  std::vector<String> wanted;
  uint32_t gen;
  {
    Lock g;
    wanted = s_wanted;
    gen = s_wantedGen;
  }
  for (uint32_t sub : s_subIds) {
    JsonDocument u(psram());
    u["id"] = s_nextId++;
    u["type"] = "unsubscribe_events";
    u["subscription"] = sub;
    sendJson(u);
  }
  s_subIds.clear();
  for (size_t i = 0; i < wanted.size(); i += SUB_BATCH) {
    JsonDocument d(psram());
    const uint32_t id = s_nextId++;
    d["id"] = id;
    d["type"] = "subscribe_entities";
    JsonArray ids = d["entity_ids"].to<JsonArray>();
    for (size_t k = i; k < wanted.size() && k < i + SUB_BATCH; k++) ids.add(wanted[k]);
    sendJson(d);
    s_subIds.push_back(id);
  }
  s_subscribedGen = gen;
}

void sendQueuedCalls() {
  for (int n = 0; n < 4; n++) {
    Call c;
    {
      Lock g;
      if (s_link != HA_CONNECTED || s_calls.empty()) return;
      c = s_calls.front();
      s_calls.pop_front();
    }
    JsonDocument d(psram());
    const uint32_t id = s_nextId++;
    d["id"] = id;
    d["type"] = "call_service";
    d["domain"] = c.domain;
    d["service"] = c.service;
    if (c.target.length()) d["target"]["entity_id"] = c.target;
    if (c.data.length()) {
      JsonDocument data(psram());
      if (!deserializeJson(data, c.data)) d["service_data"] = data.as<JsonObjectConst>();
    }
    s_pending.push_back({id, c.target, c.tracking});
    sendJson(d);
  }
}

void haTask(void*) {
  for (;;) {
    uint32_t credGen;
    String url, token;
    {
      Lock g;
      credGen = s_credGen;
      url = s_url;
      token = s_token;
    }
    if (credGen != s_appliedCredGen) {
      s_appliedCredGen = credGen;
      if (s_started) s_ws.disconnect();
      s_started = false;
      s_everConnected = false;
      setLink(HA_NOT_CONFIGURED);
      failPending("connection to Home Assistant lost");
      {
        Lock g;
        s_error = "";
      }
    }
    if (!url.length() || !token.length()) {
      setLink(HA_NOT_CONFIGURED);
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      if (s_started) {
        s_ws.disconnect();
        s_started = false;
      }
      setLink(HA_WAITING_WIFI);
      failPending("connection to Home Assistant lost");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (haLink() == HA_AUTH_FAILED) {
      vTaskDelay(pdMS_TO_TICKS(250));  // until the settings change
      continue;
    }
    if (!s_started) {
      String host, err;
      uint16_t port;
      if (!parseUrl(url, host, port, err)) {
        setLink(HA_UNREACHABLE, err);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      s_ws.begin(host.c_str(), port, "/api/websocket", "");
      s_ws.onEvent(onWsEvent);
      s_ws.setReconnectInterval(RECONNECT_MS);
      s_ws.enableHeartbeat(25000, 10000, 2);
      s_started = true;
      setLink(HA_CONNECTING);
      Serial.printf("HA: connecting to %s:%u\n", host.c_str(), (unsigned)port);
    }
    s_ws.loop();
    static uint32_t lastHistory = 0;
    if (millis() - lastHistory >= 1000) {
      lastHistory = millis();
      Lock g;
      historyLive(time(nullptr));
    }
    if (haLink() == HA_CONNECTED) {
      uint32_t wantedGen;
      {
        Lock g;
        wantedGen = s_wantedGen;
      }
      if (s_subscribedGen != wantedGen) resubscribe();
      sendQueuedCalls();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

const char* haLinkName(HaLink s) {
  switch (s) {
    case HA_NOT_CONFIGURED: return "not set up";
    case HA_WAITING_WIFI: return "waiting for WiFi";
    case HA_CONNECTING: return "connecting";
    case HA_AUTHENTICATING: return "authenticating";
    case HA_CONNECTED: return "connected";
    case HA_AUTH_FAILED: return "token rejected";
    case HA_UNREACHABLE: return "unreachable";
  }
  return "?";
}

void haBegin() {
  if (s_lock) return;
  s_lock = xSemaphoreCreateMutex();
  loadCredentials();
  // Core 0 with the WiFi stack; the display pipeline owns core 1.
  xTaskCreatePinnedToCore(haTask, "deck_ha", 8192, nullptr, 4, nullptr, 0);
}

void haSetCredentials(const String& url, const String& token) {
  String u = url;
  u.trim();
  Preferences p;
  if (p.begin("deckha", false)) {
    p.putString("url", u);
    if (token.length()) p.putString("token", token);
    p.end();
  }
  Lock g;
  s_url = u;
  if (token.length()) s_token = token;
  s_credGen++;
  s_version++;
}

void haClearCredentials() {
  Preferences p;
  if (p.begin("deckha", false)) {
    p.clear();
    p.end();
  }
  Lock g;
  s_url = "";
  s_token = "";
  s_credGen++;
  s_version++;
}

String haUrl() {
  Lock g;
  return s_url;
}

bool haHasToken() {
  Lock g;
  return s_token.length() > 0;
}

HaLink haLink() {
  Lock g;
  return s_link;
}

String haLastError() {
  Lock g;
  return s_error;
}

String haServerVersion() {
  Lock g;
  return s_haVersion;
}

void haSetEntities(const std::vector<String>& ids) {
  std::vector<String> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  Lock g;
  if (sorted == s_wanted) return;
  s_wanted = sorted;
  std::vector<Entry> next;
  for (const String& id : sorted) {
    Entry* old = findEntry(id.c_str());
    next.push_back(old ? *old : Entry{id, HaEntity()});
  }
  s_store.swap(next);
  s_wantedGen++;
  s_version++;
}

bool haGetEntity(const String& id, HaEntity& out) {
  Lock g;
  Entry* en = findEntry(id.c_str());
  if (!en) return false;
  out = en->e;
  return true;
}

uint32_t haStoreVersion() {
  if (!s_lock) return 0;
  Lock g;
  return s_version;
}

static uint32_t queueService(const String& domain, const String& service, const String& target,
                             const String& dataJson, bool tracked) {
  if (!s_lock) return 0;
  Lock g;
  if (s_link != HA_CONNECTED || s_calls.size() >= MAX_QUEUED_CALLS) return 0;
  uint32_t id = 0;
  if (tracked) {
    for (CallResultSlot& slot : s_results) {
      if (slot.id) continue;
      do { id = ++s_nextTracking; } while (!id);
      slot.id = id;
      slot.ready = false;
      break;
    }
    if (!id) return 0;
  }
  s_calls.push_back({domain, service, target, dataJson, id});
  return tracked ? id : 1;
}

bool haCallService(const String& domain, const String& service, const String& target,
                   const String& dataJson) {
  return queueService(domain, service, target, dataJson, false) != 0;
}

uint32_t haCallServiceTracked(const String& domain, const String& service, const String& target,
                              const String& dataJson) {
  return queueService(domain, service, target, dataJson, true);
}

bool haTakeCallResult(uint32_t id, HaCallResult& result) {
  if (!s_lock || !id) return false;
  Lock g;
  for (CallResultSlot& slot : s_results) {
    if (slot.id != id || !slot.ready) continue;
    result = slot.result;
    slot.id = 0;
    return true;
  }
  return false;
}

bool haTakeFailure(String& entity, String& message) {
  if (!s_lock) return false;
  Lock g;
  if (s_failures.empty()) return false;
  entity = s_failures.front().entity;
  message = s_failures.front().message;
  s_failures.pop_front();
  return true;
}

// --- Entity list for the portal picker ---------------------------------------------
namespace {

const char* const LIST_PATH = "/ha_entities.json";
constexpr size_t MAX_BODY = 4 * 1024 * 1024;

// Rendered by Home Assistant itself, so the device never parses the full
// /api/states JSON: one record per entity, fields split by 0x1F, records by
// 0x1E (neither can appear in a name), already sorted. This is the complete
// request body: the separators must reach HA as \u001f escapes, because a
// raw control character inside a JSON string is invalid (and ArduinoJson
// would write it raw).
const char kListRequest[] =
    "{\"template\":\""
    "{% for s in states|sort(attribute='entity_id') %}"
    "{{ s.entity_id }}\\u001f{{ s.name }}\\u001f{{ s.state }}\\u001f"
    "{{ s.attributes.icon|default('') }}\\u001f"
    "{{ s.attributes.unit_of_measurement|default('') }}\\u001e"
    "{% endfor %}\"}";

// Whole response body into a PSRAM buffer (caller frees). Parsing straight
// off the socket reads byte by byte and big answers hit the read timeout.
bool readBody(HTTPClient& http, char*& body, size_t& len, String& err) {
  const int declared = http.getSize();
  size_t cap = declared > 0 ? (size_t)declared + 1 : 256 * 1024;
  if (cap > MAX_BODY) {
    err = "the entity list is too large";
    return false;
  }
  body = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!body) {
    err = "out of memory";
    return false;
  }
  len = 0;
  WiFiClient* in = http.getStreamPtr();
  uint32_t lastData = millis();
  while (true) {
    if (declared > 0 && len >= (size_t)declared) break;
    const int avail = in->available();
    if (avail > 0) {
      if (len + avail + 1 > cap) {
        size_t ncap = cap * 2;
        if (ncap > MAX_BODY) ncap = MAX_BODY;
        char* nb = ncap > cap ? (char*)heap_caps_realloc(body, ncap, MALLOC_CAP_SPIRAM) : nullptr;
        if (!nb) {
          heap_caps_free(body);
          body = nullptr;
          err = "the entity list is too large";
          return false;
        }
        body = nb;
        cap = ncap;
      }
      const int got = in->read((uint8_t*)body + len, min((size_t)avail, cap - len - 1));
      if (got > 0) {
        len += got;
        lastData = millis();
      }
    } else if (!in->connected()) {
      break;
    } else if (millis() - lastData > 5000) {
      break;  // stalled
    } else {
      vTaskDelay(1);
    }
    esp_task_wdt_reset();
  }
  body[len] = 0;
  if (declared > 0 && len < (size_t)declared) {
    heap_caps_free(body);
    body = nullptr;
    err = "the answer from Home Assistant was cut off";
    return false;
  }
  return true;
}

void appendJsonString(String& o, const char* s, size_t n) {
  o += '"';
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char)c;
    } else if (c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      o += esc;
    } else {
      o += (char)c;
    }
  }
  o += '"';
}

// Template answer -> [{"id","name","state","icon","unit"}, ...]
void recordsToJson(const char* body, size_t len, String& out, size_t& count) {
  out = "";
  out.reserve(len + len / 3 + 64);
  out += '[';
  count = 0;
  const char* p = body;
  const char* end = body + len;
  static const char* const kKeys[5] = {"id", "name", "state", "icon", "unit"};
  while (p < end) {
    const char* rec = p;
    while (p < end && *p != '\x1e') p++;
    const char* recEnd = p;
    if (p < end) p++;
    const char* f = rec;
    int field = 0;
    bool any = false;
    while (f < recEnd && field < 5) {
      const char* fe = f;
      while (fe < recEnd && *fe != '\x1f') fe++;
      if (field == 0) {
        while (f < fe && (*f == '\n' || *f == '\r' || *f == ' ')) f++;  // template whitespace
        if (f == fe) break;
        if (count) out += ',';
        out += '{';
        any = true;
      }
      if (field < 3 || fe > f) {  // icon and unit only when set
        if (field) out += ',';
        out += '"';
        out += kKeys[field];
        out += "\":";
        appendJsonString(out, f, fe - f);
      }
      field++;
      f = fe < recEnd ? fe + 1 : fe;
    }
    if (any) {
      out += '}';
      count++;
    }
  }
  out += ']';
}

bool openHa(HTTPClient& http, const char* path, String& token, String& err) {
  String url;
  {
    Lock g;
    url = s_url;
    token = s_token;
  }
  if (!url.length() || !token.length()) {
    err = "Home Assistant is not set up";
    return false;
  }
  String host;
  uint16_t port;
  if (!parseUrl(url, host, port, err)) return false;
  http.useHTTP10(true);  // plain body, no chunked framing
  http.setTimeout(8000);
  if (!http.begin("http://" + host + ":" + String(port) + path)) {
    err = "cannot open the connection";
    return false;
  }
  http.addHeader("Authorization", "Bearer " + token);
  return true;
}

// ISO timestamps are UTC or carry an explicit offset; mktime would use local TZ.
bool historyEpoch(const char* s, time_t& epoch) {
  int y, m, d, hh, mm, ss, n = 0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &m, &d, &hh, &mm, &ss, &n) != 6 ||
      y < 1970 || m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 ||
      mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;
  static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (d > monthDays[m - 1] + (m == 2 && leap ? 1 : 0)) return false;
  const char* p = s + n;
  if (*p == '.') { p++; if (!isdigit(*p)) return false; while (isdigit(*p)) p++; }
  int offset = 0;
  if (*p == 'Z' && !p[1]) { p++; }
  else if (*p == '+' || *p == '-') {
    const int sign = *p++ == '+' ? 1 : -1;
    int oh, om, used = 0;
    if (sscanf(p, "%2d:%2d%n", &oh, &om, &used) != 2 || p[used] ||
        oh < 0 || oh > 23 || om < 0 || om > 59) return false;
    offset = sign * (oh * 3600 + om * 60);
    p += used;
  } else return false;
  if (*p) return false;
  y -= m <= 2;
  const int era = y / 400, yo = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int days = era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
  epoch = (time_t)((int64_t)days * 86400 + hh * 3600 + mm * 60 + ss - offset);
  return true;
}

String historyIso(time_t epoch) {
  struct tm utc;
  gmtime_r(&epoch, &utc);
  char text[24];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(text);
}

String historyEscape(const String& text) {
  String out;
  for (size_t i = 0; i < text.length(); i++) {
    const unsigned char c = text[i];
    if (isalnum(c) || c == '_' || c == '.' || c == '-') out += (char)c;
    else { char hex[4]; snprintf(hex, sizeof(hex), "%%%02X", c); out += hex; }
  }
  return out;
}

// A reply can run to megabytes (a power sensor over a week), so it is parsed
// one state at a time off the socket through a small buffer.
struct HistoryStream {
  WiFiClient* in;
  uint32_t deadline;
  bool timedOut = false;
  int back = -1;
  size_t pos = 0, len = 0;
  uint8_t buf[512];
  int read() {
    if (back >= 0) {
      const int c = back;
      back = -1;
      return c;
    }
    while (pos >= len) {
      if ((int32_t)(millis() - deadline) >= 0) {
        timedOut = true;
        return -1;
      }
      const int avail = in->available();
      if (avail > 0) {
        const int got = in->read(buf, min((size_t)avail, sizeof(buf)));
        if (got > 0) { len = got; pos = 0; }
      } else if (!in->connected()) {
        return -1;
      } else {
        vTaskDelay(1);
      }
    }
    return buf[pos++];
  }
  size_t readBytes(char* out, size_t n) {
    size_t i = 0;
    for (int c; i < n && (c = read()) >= 0; i++) out[i] = (char)c;
    return i;
  }
  int next() {
    int c;
    do c = read();
    while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    return c;
  }
};

// slow: the reply did not arrive in time (too much history), retry much later.
bool fetchHistory(const HaHistoryKey& key, time_t now, HistoryData& out, String& err, bool& slow) {
  const time_t start = now - key.hours * 3600;
  clearHistory(out, now, NAN);
  HTTPClient http;
  String token;
  const String path = "/api/history/period/" + historyIso(start) + "?filter_entity_id=" +
                      historyEscape(key.entity) + "&end_time=" + historyIso(now) +
                      "&minimal_response&no_attributes";
  if (!openHa(http, path.c_str(), token, err)) return false;
  http.setConnectTimeout(3000);
  const int code = http.GET();
  if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
  HistoryStream s{http.getStreamPtr(), millis() + 20000};
  time_t at = start;
  float value = NAN;
  const int width = key.hours * 3600 / HA_HISTORY_POINTS;
  auto hold = [&](time_t until) {
    until = min(now, until);
    while (at < until) {
      const int i = min(HA_HISTORY_POINTS - 1, (int)((at - start) / width));
      const time_t to = min(until, start + (i + 1) * width);
      addHistory(out, i, value, (float)(to - at));
      at = to;
    }
  };
  JsonDocument filter(psram()), doc(psram());
  filter["state"] = true;
  filter["last_changed"] = true;
  // [[{...}, {...}]], or [] when the entity has no history.
  bool valid = s.next() == '[';
  int c = valid ? s.next() : -1;
  if (c != ']') {
    valid = valid && c == '[';
    c = valid ? s.next() : -1;
    time_t previous = 0;
    unsigned records = 0;
    while (valid && c != ']') {
      s.back = c;
      if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) { valid = false; break; }
      time_t stamp;
      if (!historyEpoch(doc["last_changed"] | "", stamp) || (previous && stamp < previous)) {
        valid = false;
        break;
      }
      previous = stamp;
      if (++records % 128 == 0) vTaskDelay(1);
      if (stamp <= now) {
        if (stamp > start) hold(stamp);
        haNumericState(String(doc["state"] | ""), value);
      }
      c = s.next();
      if (c == ',') c = s.next();
      else if (c != ']') valid = false;
    }
  }
  http.end();
  if (!valid) {
    slow = s.timedOut;
    err = slow ? "history timed out" : "bad history reply";
    return false;
  }
  hold(now);
  return true;
}

void historyTask(void*) {
  size_t next = 0;
  for (;;) {
    HaHistoryKey key;
    uint32_t id = 0, request = 0, credentials = 0;
    const time_t now = time(nullptr);
    {
      Lock g;
      if (s_link == HA_CONNECTED && now >= 1609459200) {
        for (size_t n = 0; n < s_history.size(); n++) {
          const size_t i = (next + n) % s_history.size();
          History* h = s_history[i];
          if (h->due && (int32_t)(millis() - h->due) < 0) continue;
          next = (i + 1) % s_history.size();
          key = h->key;
          id = h->id;
          request = h->request;
          credentials = s_credGen;
          clearHistory(h->tail, now, h->data.live);
          h->fetching = true;
          break;
        }
      }
    }
    if (!id) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    HistoryData* result = (HistoryData*)heap_caps_malloc(sizeof(HistoryData), MALLOC_CAP_SPIRAM);
    String err;
    bool slow = false;
    const bool ok = result && fetchHistory(key, now, *result, err, slow);
    if (!result) err = "out of memory";
    {
      Lock g;
      History* h = findHistory(key.entity, key.hours);
      if (h && h->id == id) {
        historyLive(time(nullptr));
        h->fetching = false;
        if (h->request == request && s_credGen == credentials) {
          h->due = millis() + (ok || slow ? 15 * 60 * 1000 : 60 * 1000);
          if (ok) {
            shiftHistory(*result, h->tail.end, key.hours * 3600 / HA_HISTORY_POINTS);
            for (int i = 0; i < HA_HISTORY_POINTS; i++) {
              addHistory(*result, i, h->tail.value[i], h->tail.weight[i]);
              if (!isfinite(h->tail.low[i])) continue;
              result->low[i] = min(result->low[i], h->tail.low[i]);
              result->high[i] = max(result->high[i], h->tail.high[i]);
            }
            result->cursor = h->tail.cursor;
            result->live = h->tail.live;
            h->data = *result;
            h->version = ++s_histVersion;
          }
        }
      }
    }
    heap_caps_free(result);
    if (!ok) Serial.printf("HA: history %s (%uh) failed: %s\n", key.entity.c_str(), key.hours, err.c_str());
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool fetchViaTemplate(String& out, size_t& count, String& err) {
  HTTPClient http;
  String token;
  if (!openHa(http, "/api/template", token, err)) return false;
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST((uint8_t*)kListRequest, sizeof(kListRequest) - 1);
  if (code != 200) {
    err = code == 401 ? String("token rejected") : String("template answered ") + code;
    if (code > 0) err += ": " + http.getString().substring(0, 160);
    http.end();
    return false;
  }
  char* body = nullptr;
  size_t len = 0;
  const bool ok = readBody(http, body, len, err);
  http.end();
  if (!ok) return false;
  recordsToJson(body, len, out, count);
  heap_caps_free(body);
  return true;
}

// Fallback when the template endpoint is not available to this token. Slow
// on a big installation (ArduinoJson 7 deduplicates strings linearly, ~10 s
// for 1400 entities), but correct.
bool fetchViaStates(String& out, size_t& count, String& err) {
  HTTPClient http;
  String token;
  if (!openHa(http, "/api/states", token, err)) return false;
  const int code = http.GET();
  if (code != 200) {
    err = code == 401 ? String("token rejected") : String("Home Assistant answered ") + code;
    http.end();
    return false;
  }
  char* body = nullptr;
  size_t len = 0;
  const bool ok = readBody(http, body, len, err);
  http.end();
  if (!ok) return false;
  JsonDocument filter(psram());
  filter[0]["entity_id"] = true;
  filter[0]["state"] = true;
  filter[0]["attributes"]["friendly_name"] = true;
  filter[0]["attributes"]["icon"] = true;
  filter[0]["attributes"]["unit_of_measurement"] = true;
  JsonDocument doc(psram());
  // const char*: a mutable buffer would be parsed in place (zero-copy) and the
  // strings would dangle once it is freed below.
  const DeserializationError de =
      deserializeJson(doc, (const char*)body, len, DeserializationOption::Filter(filter));
  heap_caps_free(body);
  if (de) {
    err = String("bad answer: ") + de.c_str();
    return false;
  }
  out = "[";
  count = 0;
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    const char* id = o["entity_id"] | "";
    const char* name = o["attributes"]["friendly_name"] | "";
    const char* state = o["state"] | "";
    if (count) out += ',';
    out += "{\"id\":";
    appendJsonString(out, id, strlen(id));
    out += ",\"name\":";
    appendJsonString(out, name, strlen(name));
    out += ",\"state\":";
    appendJsonString(out, state, strlen(state));
    const char* icon = o["attributes"]["icon"] | "";
    if (*icon) {
      out += ",\"icon\":";
      appendJsonString(out, icon, strlen(icon));
    }
    const char* unit = o["attributes"]["unit_of_measurement"] | "";
    if (*unit) {
      out += ",\"unit\":";
      appendJsonString(out, unit, strlen(unit));
    }
    out += '}';
    count++;
  }
  out += ']';
  return true;  // unsorted; the portal sorts
}

bool storeList(const String& listJson, size_t count, String& doc) {
  const time_t now = time(nullptr);
  doc = "{\"updated\":";
  doc += String((uint32_t)(now > 1609459200 ? now : 0));
  doc += ",\"count\":";
  doc += String((unsigned)count);
  doc += ",\"entities\":";
  doc += listJson;
  doc += '}';
  if (!deckFsReady()) return false;
  File f = LittleFS.open("/ha_entities.json.tmp", "w");
  if (!f) return false;
  const size_t n = f.print(doc);
  f.close();
  if (n != doc.length()) {
    LittleFS.remove("/ha_entities.json.tmp");
    return false;
  }
  LittleFS.remove(LIST_PATH);
  return LittleFS.rename("/ha_entities.json.tmp", LIST_PATH);
}

}  // namespace

bool haNumericState(const String& state, float& value) {
  char* end = nullptr;
  value = strtof(state.c_str(), &end);
  if (end != state.c_str() && end && !*end && isfinite(value)) return true;
  value = NAN;
  return false;
}

void haSetHistory(const std::vector<HaHistoryKey>& keys) {
  Lock g;
  bool changed = false;
  for (History* h : s_history) {
    bool wanted = false;
    for (const HaHistoryKey& key : keys)
      wanted = wanted || (h->key.entity == key.entity && h->key.hours == constrain(key.hours, 1, 168));
    changed = changed || (h->configured != wanted);
    h->configured = false;
  }
  for (const HaHistoryKey& key : keys) {
    changed = changed || !findHistory(key.entity, constrain(key.hours, 1, 168));
    History* h = ensureHistory(key.entity, constrain(key.hours, 1, 168));
    if (h) h->configured = true;
  }
  pruneHistory();
  if (changed) for (History* h : s_history) { h->due = 0; h->request++; }
  historyLive(time(nullptr));
  startHistoryTask();
}

void haDetailHistory(const String& entity, uint8_t hours) {
  if (!s_lock) return;
  Lock g;
  History* existing = findHistory(entity, constrain(hours, 1, 168));
  if (existing && existing->detail) return;
  for (History* h : s_history) h->detail = false;
  History* h = ensureHistory(entity, constrain(hours, 1, 168));
  if (h) h->detail = true;
  pruneHistory();
  historyLive(time(nullptr));
  startHistoryTask();
}

bool haCopyHistory(const String& entity, uint8_t hours, uint32_t& version, float* points,
                   float& low, float& high) {
  Lock g;
  History* h = findHistory(entity, hours);
  if (!h || !h->version || h->version == version) return false;
  memcpy(points, h->data.value, sizeof(h->data.value));
  low = high = NAN;
  for (int i = 0; i < HA_HISTORY_POINTS; i++) {
    if (!isfinite(h->data.low[i])) continue;
    low = isnan(low) ? h->data.low[i] : min(low, h->data.low[i]);
    high = isnan(high) ? h->data.high[i] : max(high, h->data.high[i]);
  }
  version = h->version;
  return true;
}

uint32_t haHistoryVersion() {
  Lock g;
  return s_histVersion;
}

const char* haEntityListPath() { return LIST_PATH; }

bool haFetchEntityList(String& outJson, bool refresh, String& err) {
  if (!refresh && deckFsReady() && LittleFS.exists(LIST_PATH)) {
    File f = LittleFS.open(LIST_PATH, "r");
    if (f) {
      outJson = f.readString();
      f.close();
      if (outJson.length()) return true;
    }
  }
  if (!refresh) {
    // Nothing cached yet: answer with an empty list, the portal offers the
    // update button. Fetching implicitly here would surprise with a long wait.
    outJson = "{\"updated\":0,\"count\":0,\"entities\":[]}";
    return true;
  }
  const uint32_t t0 = millis();
  String list;
  size_t count = 0;
  String why;
  if (!fetchViaTemplate(list, count, why)) {
    Serial.printf("HA: template list failed (%s), falling back to /api/states\n", why.c_str());
    if (!fetchViaStates(list, count, err)) return false;
  }
  if (!storeList(list, count, outJson)) Serial.println("HA: could not cache the entity list");
  Serial.printf("HA: entity list updated, %u entities, %u bytes, %u ms\n", (unsigned)count,
                (unsigned)outJson.length(), (unsigned)(millis() - t0));
  return true;
}

}  // namespace deck
