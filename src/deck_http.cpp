// Guition deck: HTTP tile actions. See deck_http.h.
#include "deck_http.h"
#include <HTTPClient.h>
#include <WiFi.h>

namespace deck {
namespace {

struct Job {
  TileAction a;
  String label;
  uint32_t ticket;  // portal test, 0 = a tile
};

QueueHandle_t s_jobs = nullptr;
QueueHandle_t s_done = nullptr;  // HttpOutcome*
SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
uint32_t s_nextTicket = 1;
// The last few finished tests, so two quick Test clicks both get an answer.
struct TestResult {
  uint32_t ticket = 0;
  int code = 0;
  String err, body;
};
TestResult s_tests[4];
uint8_t s_testNext = 0;

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
  for (int at = 0; at < (int)j.a.headers.length();) {
    int nl = j.a.headers.indexOf('\n', at);
    if (nl < 0) nl = j.a.headers.length();
    const String line = j.a.headers.substring(at, nl);
    const int colon = line.indexOf(':');
    if (colon > 0) http.addHeader(line.substring(0, colon), line.substring(colon + 2));
    at = nl + 1;
  }
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

void task(void*) {
  for (;;) {
    Job* j = nullptr;
    if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE || !j) continue;
    int code;
    String err, body;
    run(*j, code, err, j->ticket ? &body : nullptr);
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
  return xTaskCreatePinnedToCore(task, "deck_http", 6144, nullptr, 3, &s_task, 0) == pdPASS;
}

bool enqueue(const TileAction& a, const String& label, uint32_t ticket) {
  if (!ensureTask()) return false;
  Job* j = new Job{a, label, ticket};
  if (xQueueSend(s_jobs, &j, 0) == pdTRUE) return true;
  delete j;
  return false;
}

}  // namespace

bool httpSend(const TileAction& a, const String& label) { return enqueue(a, label, 0); }

bool httpTakeResult(HttpOutcome& out) {
  HttpOutcome* o = nullptr;
  if (!s_done || xQueueReceive(s_done, &o, 0) != pdTRUE || !o) return false;
  out = *o;
  delete o;
  return true;
}

uint32_t httpTestStart(const TileAction& a) {
  const uint32_t t = s_nextTicket++;
  return enqueue(a, "", t) ? t : 0;
}

bool httpTestResult(uint32_t ticket, int& code, String& err, String& body) {
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

}  // namespace deck
