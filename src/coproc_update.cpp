// See coproc_update.h. Same flow as Arduino's ESP_HostedOTA example, but with
// the server certificate checked and off the loop task.
#include "coproc_update.h"
#include "board.h"
#if defined(BOARD_WIFI_ESP_HOSTED)
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

namespace {

// espressif.github.io chains to ISRG Root X1 (Let's Encrypt), valid to 2035.
constexpr const char* ROOT_CA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";

enum State : uint8_t { ST_IDLE, ST_RUNNING, ST_DONE, ST_ERROR };
volatile State s_state = ST_IDLE;
volatile int s_progress = 0;
char s_error[64] = "";

void fail(const char* why) {
  strlcpy(s_error, why, sizeof(s_error));
  Serial.printf("Co-processor update: %s\n", why);
  s_state = ST_ERROR;
}

void updateTask(void*) {
  NetworkClientSecure tls;
  tls.setCACert(ROOT_CA);
  HTTPClient http;
  http.setTimeout(15000);
  const char* url = hostedGetUpdateURL();
  Serial.printf("Co-processor update from %s\n", url);
  uint8_t* buf = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  int code = 0;
  if (!buf) fail("no memory");
  else if (!http.begin(tls, url)) fail("bad address");
  else if ((code = http.GET()) != HTTP_CODE_OK) {
    snprintf(s_error, sizeof(s_error), code == HTTP_CODE_NOT_FOUND ? "no image for this version" : "HTTP %d", code);
    fail(s_error);
  } else {
    const int total = http.getSize();
    NetworkClient* stream = http.getStreamPtr();
    if (total <= 0) {
      fail("size unknown");
    } else if (!stream) {
      fail("download cut");
    } else if (!hostedBeginUpdate()) {
      fail("co-processor refused the update");
    } else {
      int left = total;
      uint32_t last = millis();
      while (left > 0 && millis() - last < 15000) {
        const int n = stream->read(buf, std::min(left, 2048));
        if (n <= 0) {
          vTaskDelay(1);
          continue;
        }
        if (!hostedWriteUpdate(buf, n)) break;
        left -= n;
        last = millis();
        s_progress = (int)((int64_t)(total - left) * 100 / total);
      }
      if (left > 0) fail("download or write cut");
      else if (!hostedEndUpdate()) fail("image rejected");
      else if (!hostedActivateUpdate()) fail("activation failed");
      else s_state = ST_DONE;
    }
  }
  http.end();
  free(buf);
  if (s_state == ST_DONE) {
    // The host must handshake with the new firmware from scratch.
    Serial.println("Co-processor updated, restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
  }
  vTaskDelete(nullptr);
}

}  // namespace

bool coprocUpdateStart(const char** why) {
  if (s_state == ST_RUNNING) {
    *why = "an update is already running";
    return false;
  }
  if (!hostedIsInitialized() || !WiFi.isConnected()) {
    *why = "WiFi is not connected";
    return false;
  }
  s_state = ST_RUNNING;
  s_progress = 0;
  s_error[0] = 0;
  if (xTaskCreate(updateTask, "coproc_ota", 8192, nullptr, 3, nullptr) != pdPASS) {
    s_state = ST_IDLE;
    *why = "no memory";
    return false;
  }
  return true;
}

const char* coprocUpdateState() {
  static const char* const names[] = {"idle", "running", "done", "error"};
  return names[s_state];
}
int coprocUpdateProgress() { return s_progress; }
const char* coprocUpdateError() { return s_error; }

#else

bool coprocUpdateStart(const char** why) {
  *why = "this board has no WiFi co-processor";
  return false;
}
const char* coprocUpdateState() { return "idle"; }
int coprocUpdateProgress() { return 0; }
const char* coprocUpdateError() { return ""; }

#endif  // BOARD_WIFI_ESP_HOSTED
