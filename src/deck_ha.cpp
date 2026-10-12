// Guition deck: Home Assistant client. See deck_ha.h.
#if defined(BOARD_IS_JC3248W535)

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
};
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
};
std::vector<Pending> s_pending;
bool s_everConnected = false;

void setLink(HaLink l, const String& err = String()) {
  Lock g;
  if (s_link != l || (err.length() && err != s_error)) s_version++;
  s_link = l;
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
  for (JsonPairConst kv : a)
    if (!strcmp(kv.key().c_str(), "brightness"))
      e.brightness = kv.value().is<int>() ? kv.value().as<int>() : -1;
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
  if (changed) s_version++;
}

void failPending(const String& message) {
  Lock g;
  for (const Pending& p : s_pending) s_failures.push_back({p.entity, message});
  s_pending.clear();
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
      if (!ok) {
        Lock g;
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
      if (s_calls.empty()) return;
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
    sendJson(d);
    s_pending.push_back({id, c.target});
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

bool haCallService(const String& domain, const String& service, const String& target,
                   const String& dataJson) {
  if (!s_lock) return false;
  Lock g;
  if (s_link != HA_CONNECTED || s_calls.size() >= MAX_QUEUED_CALLS) return false;
  s_calls.push_back({domain, service, target, dataJson});
  return true;
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

#endif  // BOARD_IS_JC3248W535
