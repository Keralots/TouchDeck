// Animation timing log: a ring of events from the loop and the panel task.
#include "debug_anim.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace {
struct Ev {
  int64_t us;
  int32_t a, b, c, d;
  uint8_t type;
};
constexpr int N = 8192;
Ev* s_ev = nullptr;  // never freed: the panel task may be logging into it
volatile bool s_on = false;
uint32_t s_head = 0, s_tail = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void danim(uint8_t type, int32_t a, int32_t b, int32_t c, int32_t d) {
  if (!s_on) return;
  const int64_t us = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&s_mux);
  s_ev[s_head % N] = {us, a, b, c, d, type};
  s_head = s_head + 1;
  portEXIT_CRITICAL_SAFE(&s_mux);
}

bool danimEnable(bool on) {
  if (on && !s_ev) s_ev = (Ev*)heap_caps_calloc(N, sizeof(Ev), MALLOC_CAP_SPIRAM);
  __sync_synchronize();  // the ring before the flag, for the other core
  s_on = on && s_ev;
  return s_on;
}

bool danimOn() { return s_on; }

void danimDrain(String& out) {
  if (!s_ev) return;
  portENTER_CRITICAL(&s_mux);
  const uint32_t head = s_head;
  portEXIT_CRITICAL(&s_mux);
  if (head - s_tail > N) s_tail = head - N;
  out.reserve((head - s_tail) * 40 + 16);
  char b[96];
  for (; s_tail != head; s_tail++) {
    const Ev& e = s_ev[s_tail % N];
    snprintf(b, sizeof(b), "%u %lld %ld %ld %ld %ld\n", e.type, e.us, (long)e.a, (long)e.b, (long)e.c, (long)e.d);
    out += b;
  }
}
