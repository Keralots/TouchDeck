#include "pin_lock.h"
#include "config.h"
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <atomic>

static constexpr uint8_t FREE_TRIES = 5;

static bool s_set = false;
static uint8_t s_salt[16];
static uint8_t s_hash[32];
static struct {
  uint8_t fails;
  uint32_t lockMs;  // how long from sinceMs, 0 = not locked
  uint32_t sinceMs;
} s_try[PIN_CHANNELS];
static std::atomic<bool> s_clear{false};

static void hashPin(const char* pin, uint8_t out[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, s_salt, sizeof(s_salt));
  mbedtls_sha256_update(&c, (const uint8_t*)pin, strlen(pin));
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
}

void pinInit() {
  Preferences p;
  if (!p.begin(PIN_NAMESPACE, true)) return;  // never set
  s_set = p.getBytes("salt", s_salt, sizeof(s_salt)) == sizeof(s_salt) &&
          p.getBytes("hash", s_hash, sizeof(s_hash)) == sizeof(s_hash);
  p.end();
}

bool pinIsSet() { return s_set; }

bool pinValid(const String& pin) {
  if (pin.length() < 4 || pin.length() > 8) return false;
  for (size_t i = 0; i < pin.length(); i++)
    if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}

bool pinStore(const String& pin) {
  if (!pinValid(pin)) return false;
  esp_fill_random(s_salt, sizeof(s_salt));
  hashPin(pin.c_str(), s_hash);
  Preferences p;
  if (!p.begin(PIN_NAMESPACE, false)) return false;
  const bool ok = p.putBytes("salt", s_salt, sizeof(s_salt)) == sizeof(s_salt) &&
                  p.putBytes("hash", s_hash, sizeof(s_hash)) == sizeof(s_hash);
  // Half a PIN in flash must not outlive a restart as a lock nobody can open.
  if (!ok) p.clear();
  p.end();
  s_set = ok;
  memset(s_try, 0, sizeof(s_try));
  return ok;
}

bool pinRemove() {
  Preferences p;
  if (!p.begin(PIN_NAMESPACE, false)) return false;
  const bool ok = p.clear();
  p.end();
  if (ok) s_set = false;
  memset(s_try, 0, sizeof(s_try));
  return ok;
}

PinResult pinTry(PinChannel ch, const char* pin, uint32_t& lockedSecs) {
  auto& t = s_try[ch];
  lockedSecs = 0;
  if (t.lockMs) {
    const uint32_t past = millis() - t.sinceMs;
    if (past < t.lockMs) {
      lockedSecs = (t.lockMs - past + 999) / 1000;
      return PIN_LOCKED;
    }
    t.lockMs = 0;
  }
  uint8_t h[32];
  hashPin(pin, h);
  uint8_t diff = s_set ? 0 : 1;
  for (size_t i = 0; i < sizeof(h); i++) diff |= h[i] ^ s_hash[i];
  if (!diff) {
    t.fails = 0;
    return PIN_OK;
  }
  if (t.fails < 255) t.fails++;
  if (t.fails >= FREE_TRIES) {  // 60 s, then twice as long per further miss, up to 32 min
    t.lockMs = 60000UL << min(t.fails - FREE_TRIES, 5);
    t.sinceMs = millis();
    lockedSecs = t.lockMs / 1000;
  }
  return PIN_WRONG;
}

void pinRequestClear() { s_clear = true; }
bool pinTakeClearRequest() { return s_clear.exchange(false); }
