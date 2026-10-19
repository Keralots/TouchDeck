// Guition deck: HTTP tile actions. See deck_http.h.
#include "deck_http.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <ctype.h>

namespace deck {
namespace {

struct Job {
  TileAction a;
  String label;
  uint32_t ticket;  // portal test, 0 = a tile
  bool sensor = false;
  String artId;
  int artPx = 0;
};

QueueHandle_t s_jobs = nullptr;
QueueHandle_t s_done = nullptr;  // HttpOutcome*
SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
uint32_t s_nextTicket = 1;
bool s_artBusy = false, s_artDone = false;
String s_artId;
int s_artPx = 0;
uint8_t* s_artPixels = nullptr;
// The last few finished tests, so two quick Test clicks both get an answer.
struct TestResult {
  uint32_t ticket = 0;
  int code = 0;
  String err;
  std::shared_ptr<char> body;
};
TestResult s_tests[4];
uint8_t s_testNext = 0;

struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
template<class T> struct PsramVectorAllocator {
  using value_type = T;
  PsramVectorAllocator() = default;
  template<class U> PsramVectorAllocator(const PsramVectorAllocator<U>&) {}
  T* allocate(size_t n) { return (T*)heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM); }
  void deallocate(T* p, size_t) { heap_caps_free(p); }
  template<class U> bool operator==(const PsramVectorAllocator<U>&) const { return true; }
  template<class U> bool operator!=(const PsramVectorAllocator<U>&) const { return false; }
};
template<class T> using PsramVector = std::vector<T, PsramVectorAllocator<T>>;
struct PathValue {
  String path;
  HttpSensorValue value;
  explicit PathValue(const String& p) : path(p) {
    for (float& v : value.history) v = NAN;
  }
};
struct Source {
  String url, headers, err;
  uint16_t poll = 30;
  uint32_t lastOk = 0, next = 0;
  bool ok = false, attempted = false, offline = false;
  PsramVector<PathValue> paths;
};
PsramVector<Source> s_sources;
uint32_t s_sensorVersion = 1, s_sourceGeneration = 0;
size_t s_pollNext = 0;

String canonicalHeaders(const String& headers) {
  std::vector<String> lines;
  for (int at = 0; at < (int)headers.length();) {
    int nl = headers.indexOf('\n', at);
    if (nl < 0) nl = headers.length();
    lines.push_back(headers.substring(at, nl));
    at = nl + 1;
  }
  std::sort(lines.begin(), lines.end(), [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  String out;
  for (const String& line : lines) out += line + "\n";
  return out;
}

class PollBody : public Stream {
 public:
  char* data = (char*)heap_caps_malloc(32769, MALLOC_CAP_SPIRAM);
  size_t size = 0;
  bool overflow = false;
  ~PollBody() { heap_caps_free(data); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    const size_t take = min(n, (size_t)32768 - size);
    if (take) memcpy(data + size, p, take);
    size += take;
    data[size] = 0;
    if (take != n) overflow = true;
    return take;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void addHeaders(HTTPClient& http, const String& headers) {
  for (int at = 0; at < (int)headers.length();) {
    int nl = headers.indexOf('\n', at);
    if (nl < 0) nl = headers.length();
    const String line = headers.substring(at, nl);
    const int colon = line.indexOf(':');
    if (colon > 0) http.addHeader(line.substring(0, colon), line.substring(colon + 2));
    at = nl + 1;
  }
}

void fetchSensor(const TileAction& a, PollBody& body, int& code, String& err) {
  code = 0;
  if (!body.data) { err = "No PSRAM for HTTP body"; return; }
  body.data[0] = 0;
  if (WiFi.status() != WL_CONNECTED) { err = "WiFi is offline"; return; }
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(6000);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(a.url)) { err = "bad url"; return; }
  addHeaders(http, a.headers);
  const int rc = http.GET();
  if (rc > 0) {
    code = rc;
    if (http.getSize() > 32768) err = "HTTP body exceeds 32 KB";
    else {
      const int n = http.writeToStream(&body);
      if (body.overflow) err = "HTTP body exceeds 32 KB";
      else if (n < 0) err = HTTPClient::errorToString(n);
    }
    if (!err.length() && (rc < 200 || rc >= 300)) err = "HTTP " + String(rc);
  } else err = HTTPClient::errorToString(rc);
  http.end();
}

JsonVariantConst atPath(JsonVariantConst value, const String& path) {
  if (!path.length()) return value;
  for (int at = 0; at <= (int)path.length();) {
    int dot = path.indexOf('.', at);
    if (dot < 0) dot = path.length();
    const String key = path.substring(at, dot);
    if (!key.length()) return JsonVariantConst();
    bool digits = true;
    size_t index = 0;
    for (size_t i = 0; i < key.length(); i++) {
      if (key[i] < '0' || key[i] > '9') { digits = false; break; }
    }
    if (digits) {
      for (size_t i = 0; i < key.length(); i++) {
        if (index > (SIZE_MAX - 9) / 10) return JsonVariantConst();
        index = index * 10 + key[i] - '0';
      }
      value = value.as<JsonArrayConst>()[index];
    } else value = value.as<JsonObjectConst>()[key];
    if (dot == (int)path.length()) return value;
    at = dot + 1;
  }
  return JsonVariantConst();
}

std::shared_ptr<char> copyText(const char* text, size_t len) {
  char* copy = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
  if (!copy) return {};
  memcpy(copy, text, len);
  copy[len] = 0;
  return std::shared_ptr<char>(copy, heap_caps_free);
}

void updatePath(PathValue& p, JsonVariantConst v) {
  auto& out = p.value;
  out.present = out.numeric = false;
  out.text.reset();
  if (v.is<bool>() || v.is<const char*>()) {
    const char* text = v.is<bool>() ? (v.as<bool>() ? "On" : "Off") : v.as<const char*>();
    out.text = copyText(text, strlen(text));
    out.present = (bool)out.text;
  } else if (v.is<double>()) {
    out.number = v.as<float>();
    out.present = out.numeric = isfinite(out.number);
    if (out.numeric) {
      memmove(out.history, out.history + 1, sizeof(float) * (HTTP_HISTORY_POINTS - 1));
      out.history[HTTP_HISTORY_POINTS - 1] = out.number;
    }
  }
}

void pollOne() {
  TileAction a;
  size_t index = SIZE_MAX;
  uint32_t generation;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    for (size_t n = 0; n < s_sources.size(); n++) {
      const size_t i = (s_pollNext + n) % s_sources.size();
      const Source& s = s_sources[i];
      if ((int32_t)(now - s.next) < 0) continue;
      index = i;
      a.url = s.url;
      a.headers = s.headers;
      break;
    }
  }
  generation = s_sourceGeneration;
  xSemaphoreGive(s_lock);
  if (index == SIZE_MAX || uxQueueMessagesWaiting(s_jobs)) return;
  PollBody body;
  int code;
  String err;
  const uint32_t started = millis();
  fetchSensor(a, body, code, err);
  PsramAllocator allocator;
  JsonDocument doc(&allocator);
  if (!err.length()) {
    const auto de = deserializeJson(doc, body.data, body.size);
    if (de) err = String("JSON: ") + de.c_str();
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (generation == s_sourceGeneration && index < s_sources.size()) {
    Source& s = s_sources[index];
    s.err = err;
    s.attempted = true;
    s.next = started + (uint32_t)s.poll * 1000;
    s_pollNext = (index + 1) % s_sources.size();
    if (!err.length()) {
      s.lastOk = millis();
      s.ok = true;
      for (PathValue& p : s.paths) updatePath(p, atPath(doc.as<JsonVariantConst>(), p.path));
    }
    s.offline = !s.ok || millis() - s.lastOk > (uint32_t)s.poll * 3000;
    ++s_sensorVersion;
  }
  xSemaphoreGive(s_lock);
}

void run(const Job& j, int& code, String& err, String* body) {
  code = 0;
  if (WiFi.status() != WL_CONNECTED) {
    err = "WiFi is offline";
    return;
  }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.setReuse(false);
  http.useHTTP10(true);  // plain body, no chunked framing
  if (!http.begin(client, j.a.url)) {
    err = "bad url";
    return;
  }
  if (j.a.ctype.length()) http.addHeader("Content-Type", j.a.ctype);
  addHeaders(http, j.a.headers);
  const int rc = http.sendRequest(j.a.method.c_str(), (uint8_t*)j.a.body.c_str(), j.a.body.length());
  if (rc > 0) {
    code = rc;
    if (body) {
      WiFiClient* s = http.getStreamPtr();
      int left = http.getSize();  // -1 = unknown
      const uint32_t t0 = millis();
      while (s && body->length() < 512 && left != 0 && millis() - t0 < 2000) {
        const int c = s->read();
        if (c < 0) {
          if (!s->connected()) break;
          delay(5);
          continue;
        }
        *body += (char)c;
        if (left > 0) left--;
      }
    }
  } else if (rc == HTTPC_ERROR_CONNECTION_REFUSED) {
    err = "cannot connect";
  } else if (rc == HTTPC_ERROR_READ_TIMEOUT) {
    err = "no answer";
  } else {
    err = HTTPClient::errorToString(rc);
  }
  http.end();
}

void fetchArt(const Job& j) {
  uint8_t* pixels = nullptr;
  const size_t bytes = j.artPx * j.artPx * 2;
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    http.setReuse(false);
    http.useHTTP10(true);
    const uint32_t started = millis();
    if (http.begin(client, j.a.url) && http.GET() == 200 && http.getSize() == (int)bytes) {
      pixels = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
      size_t read = 0;
      WiFiClient* stream = http.getStreamPtr();
      while (pixels && stream && read < bytes && millis() - started < 3000) {
        const int available = stream->available();
        if (available > 0) {
          const int n = stream->read(pixels + read, min(bytes - read, (size_t)available));
          if (n <= 0) break;
          read += n;
        } else {
          if (!stream->connected()) break;
          delay(5);
        }
      }
      if (read != bytes) { heap_caps_free(pixels); pixels = nullptr; }
    }
    http.end();
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_artId = j.artId;
  s_artPx = j.artPx;
  s_artPixels = pixels;
  s_artDone = true;
  xSemaphoreGive(s_lock);
}

void task(void*) {
  for (;;) {
    Job* j = nullptr;
    if (xQueueReceive(s_jobs, &j, pdMS_TO_TICKS(100)) != pdTRUE || !j) {
      pollOne();
      continue;
    }
    if (j->artPx) {
      fetchArt(*j);
      delete j;
      continue;
    }
    int code;
    String err;
    std::shared_ptr<char> body;
    if (j->sensor) {
      PollBody reply;
      fetchSensor(j->a, reply, code, err);
      if (reply.data) body = copyText(reply.data, min(reply.size, (size_t)8192));
    } else {
      String text;
      run(*j, code, err, j->ticket ? &text : nullptr);
      if (j->ticket) body = copyText(text.c_str(), text.length());
    }
    Serial.printf("[http] %s %s -> %d %s\n", j->a.method.c_str(), j->a.url.c_str(), code, err.c_str());
    if (j->ticket) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      TestResult& t = s_tests[s_testNext++ % 4];
      t.ticket = j->ticket;
      t.code = code;
      t.err = err;
      t.body = body;
      xSemaphoreGive(s_lock);
    } else {
      HttpOutcome* o = new HttpOutcome{j->label, code, err};
      if (xQueueSend(s_done, &o, 0) != pdTRUE) delete o;
    }
    delete j;
  }
}

bool ensureTask() {
  if (s_task) return true;
  if (!s_jobs) s_jobs = xQueueCreate(4, sizeof(Job*));
  if (!s_done) s_done = xQueueCreate(8, sizeof(HttpOutcome*));
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_jobs || !s_done || !s_lock) return false;
  // Stack in PSRAM: this task never touches flash (NVS, LittleFS).
  return xTaskCreatePinnedToCoreWithCaps(task, "deck_http", 6144, nullptr, 3, &s_task, 0, MALLOC_CAP_SPIRAM) == pdPASS;
}

bool enqueue(const TileAction& a, const String& label, uint32_t ticket, bool sensor = false) {
  if (!ensureTask()) return false;
  Job* j = new Job{a, label, ticket, sensor};
  if (xQueueSend(s_jobs, &j, 0) == pdTRUE) return true;
  delete j;
  return false;
}

}  // namespace

bool httpArtFetch(IPAddress ip, uint16_t port, const char* id, int px) {
  if (!port || !id || strlen(id) != 8 || px < 32 || px > 200 || !ensureTask()) return false;
  for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)id[i])) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_artBusy) { xSemaphoreGive(s_lock); return false; }
  Job* j = new Job{};
  j->artId = id;
  j->artPx = px;
  j->a.url = "http://" + ip.toString() + ":" + String(port) + "/art?id=" + id + "&s=" + String(px);
  s_artBusy = xQueueSend(s_jobs, &j, 0) == pdTRUE;
  if (!s_artBusy) delete j;
  const bool queued = s_artBusy;
  xSemaphoreGive(s_lock);
  return queued;
}

bool httpArtTake(String& id, int& px, uint8_t*& pixels) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool done = s_artDone;
  if (done) {
    id = s_artId;
    px = s_artPx;
    pixels = s_artPixels;
    s_artPixels = nullptr;
    s_artBusy = s_artDone = false;
  }
  xSemaphoreGive(s_lock);
  return done;
}

bool httpSend(const TileAction& a, const String& label) { return enqueue(a, label, 0); }

bool httpTakeResult(HttpOutcome& out) {
  HttpOutcome* o = nullptr;
  if (!s_done || xQueueReceive(s_done, &o, 0) != pdTRUE || !o) return false;
  out = *o;
  delete o;
  return true;
}

uint32_t httpTestStart(const TileAction& a, bool sensor) {
  const uint32_t t = s_nextTicket++;
  return enqueue(a, "", t, sensor) ? t : 0;
}

bool httpTestResult(uint32_t ticket, int& code, String& err, std::shared_ptr<char>& body) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool done = false;
  for (const TestResult& t : s_tests) {
    if (!ticket || t.ticket != ticket) continue;
    code = t.code;
    err = t.err;
    body = t.body;
    done = true;
  }
  xSemaphoreGive(s_lock);
  return done;
}

void httpSetSensors(const DeckConfig& cfg) {
  bool any = false;
  for (const auto& page : cfg.pages) for (const auto& tile : page.tiles) any |= tile.http.enabled;
  if (!any && !s_task) return;
  if (!ensureTask()) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  PsramVector<Source> next;
  for (const auto& page : cfg.pages) for (const auto& tile : page.tiles) {
    const auto& d = tile.http;
    if (!d.enabled) continue;
    const String headers = canonicalHeaders(d.headers);
    auto source = std::find_if(next.begin(), next.end(), [&](const Source& s) {
      return s.url == d.url && s.headers == headers;
    });
    if (source == next.end()) {
      Source s;
      s.url = d.url;
      s.headers = headers;
      s.poll = d.poll;
      next.push_back(std::move(s));
      source = next.end() - 1;
    } else source->poll = min(source->poll, d.poll);
    std::vector<String> paths;
    if (tile.rows.empty()) paths.push_back(d.path);
    for (const SensorRowDef& row : tile.rows) paths.push_back(row.path);
    for (const DetailPart& part : httpParseDetail(tile.detail))
      if (part.field) paths.push_back(part.text);
    for (const String& want : paths) {
      auto path = std::find_if(source->paths.begin(), source->paths.end(), [&](const PathValue& p) { return p.path == want; });
      if (path == source->paths.end()) source->paths.emplace_back(want);
    }
  }
  for (Source& s : next) for (const Source& old : s_sources) {
    if (s.url != old.url || s.headers != old.headers) continue;
    s.ok = old.ok;
    s.attempted = old.attempted;
    s.lastOk = old.lastOk;
    s.err = old.err;
    s.offline = !s.ok ? s.attempted : millis() - s.lastOk > (uint32_t)s.poll * 3000;
    for (PathValue& p : s.paths) for (const PathValue& prev : old.paths)
      if (p.path == prev.path) p.value = prev.value;
  }
  s_sources = std::move(next);
  s_pollNext = 0;
  ++s_sourceGeneration;
  ++s_sensorVersion;
  xSemaphoreGive(s_lock);
}

uint32_t httpSensorVersion() {
  if (!s_lock) return 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const uint32_t now = millis();
  for (Source& s : s_sources) {
    const bool offline = s.ok ? now - s.lastOk > (uint32_t)s.poll * 3000 : s.attempted;
    if (offline != s.offline) { s.offline = offline; ++s_sensorVersion; }
  }
  const uint32_t v = s_sensorVersion;
  xSemaphoreGive(s_lock);
  return v;
}

bool httpSensorRead(const HttpSensorDef& def, const String& path, HttpSensorValue& out) {
  if (!s_lock) return false;
  const String headers = canonicalHeaders(def.headers);
  bool found = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (const Source& s : s_sources) {
    if (s.url != def.url || s.headers != headers) continue;
    for (const PathValue& p : s.paths) if (p.path == path) {
      out = p.value;
      out.offline = s.offline;
      found = true;
      break;
    }
    break;
  }
  xSemaphoreGive(s_lock);
  return found;
}

std::vector<DetailPart> httpParseDetail(const String& tmpl) {
  std::vector<DetailPart> parts;
  for (int at = 0; at < (int)tmpl.length();) {
    const int open = tmpl.indexOf('{', at);
    const int close = open < 0 ? -1 : tmpl.indexOf('}', open);
    DetailPart text;
    text.text = tmpl.substring(at, close < 0 ? tmpl.length() : open);
    if (text.text.length()) parts.push_back(text);
    if (close < 0) break;
    DetailPart field;
    field.field = true;
    field.text = tmpl.substring(open + 1, close);
    const int bar = field.text.indexOf('|');
    if (bar >= 0) {
      field.unit = field.text.substring(bar + 1);
      field.text.remove(bar);
    }
    field.text.trim();
    field.unit.trim();
    parts.push_back(field);
    at = close + 1;
  }
  return parts;
}

}  // namespace deck
