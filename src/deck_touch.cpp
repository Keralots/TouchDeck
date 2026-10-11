// Guition JC3248W535 touch reader and swipe arbiter. See deck_touch.h.
#if defined(BOARD_IS_JC3248W535)

#include "deck_touch.h"
#include "deck_panel.h"
#include "deck_ui.h"
#include <Wire.h>
#include <driver/gpio.h>

namespace deck {
namespace {

constexpr int PIN_SDA = 4;
constexpr int PIN_SCL = 8;
constexpr int PIN_INT = 3;   // wired on this board (Phase 0), despite older docs
constexpr uint8_t ADDR = 0x3B;

// No valid frame for this long = finger lifted (the controller reports no
// explicit release event that we have seen).
constexpr uint32_t RELEASE_MS = 60;
// Movement before a touch is classified as a swipe or a vertical gesture.
constexpr int SLOP_PX = 12;
// A released swipe changes page when it travelled this share of the width,
// or when it is flung faster than FLING_PX_PER_MS toward the next page. The
// owner found the Phase 0 rule (half the width) needed far too long a swipe.
constexpr float COMMIT_FRACTION = 0.18f;
constexpr float FLING_PX_PER_MS = 0.25f;
constexpr uint32_t VELOCITY_WINDOW_MS = 90;
constexpr uint32_t SLIDE_MAX_MS = 280;
constexpr uint32_t SLIDE_MIN_MS = 90;

TaskHandle_t s_task = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TouchPoint s_lv = {0, 0, false};
volatile bool s_down = false;
volatile bool s_activity = false;
volatile bool s_swipeStarted = false;
volatile bool s_suppress = false;

enum GestureState : uint8_t { G_IDLE, G_PENDING, G_PASS, G_DRAG };

struct Sample {
  uint32_t ms;
  int16_t x;
};
constexpr int NSAMPLES = 8;

void IRAM_ATTR intIsr(void*) {
  if (!s_task) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// One 8-byte read. Returns true with native coordinates for a valid contact.
bool readPoint(int& nx, int& ny) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  Wire.beginTransmission(ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return false;
  uint8_t d[8] = {0};
  const uint8_t got = Wire.requestFrom((int)ADDR, 8);
  for (int i = 0; i < 8 && Wire.available(); i++) d[i] = Wire.read();
  if (got < 8) return false;
  // Valid frame: 00 <points 1..2> E?xH xL 0?yH yL ... Idle/filler reads come
  // back as B0.. or FF.. and are ignored.
  if (d[0] != 0 || d[1] == 0 || d[1] > 2) return false;
  nx = ((d[2] & 0x0F) << 8) | d[3];
  ny = ((d[4] & 0x0F) << 8) | d[5];
  return nx < NATIVE_W && ny < NATIVE_H;
}

// Native portrait coordinates to the logical (rotated) space; same mapping
// as the pixels in deck_panel.cpp.
void toLogical(int nx, int ny, int16_t& x, int16_t& y) {
  switch (panelRotation()) {
    case 1:  x = ny;                 y = NATIVE_W - 1 - nx; break;
    case 2:  x = NATIVE_W - 1 - nx;  y = NATIVE_H - 1 - ny; break;
    case 3:  x = NATIVE_H - 1 - ny;  y = nx;                break;
    default: x = nx;                 y = ny;                break;
  }
}

void publish(int16_t x, int16_t y, bool pressed) {
  portENTER_CRITICAL(&s_mux);
  s_lv.x = x;
  s_lv.y = y;
  s_lv.pressed = pressed;
  portEXIT_CRITICAL(&s_mux);
}

void touchTask(void*) {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.setTimeOut(5);  // bounded; Wire's default is 50 ms
  // Raw IDF handler: the GPIO ISR service is already installed by
  // deck_panel (TE), and attachInterrupt() would log an error re-installing it.
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_INT;
  g.mode = GPIO_MODE_INPUT;
  g.pull_up_en = GPIO_PULLUP_ENABLE;
  g.intr_type = GPIO_INTR_NEGEDGE;
  gpio_config(&g);
  gpio_isr_handler_add((gpio_num_t)PIN_INT, intIsr, nullptr);

  GestureState state = G_IDLE;
  int16_t x0 = 0, y0 = 0, lastX = 0, lastY = 0;
  uint32_t lastMs = 0;
  int dir = 0, startOff = 0, off = 0;
  Sample samples[NSAMPLES];
  int nsamples = 0;

  for (;;) {
    const bool woke = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_down ? RELEASE_MS : 1000)) != 0;
    const uint32_t now = millis();
    int nx, ny;
    if (woke && readPoint(nx, ny)) {
      int16_t x, y;
      toLogical(nx, ny, x, y);
      if (!s_down) {
        s_down = true;
        s_activity = true;
        x0 = x;
        y0 = y;
        state = G_PENDING;
        nsamples = 0;
      }
      lastX = x;
      lastY = y;
      lastMs = now;
      if (nsamples == NSAMPLES) {
        memmove(samples, samples + 1, sizeof(Sample) * (NSAMPLES - 1));
        nsamples--;
      }
      samples[nsamples++] = {now, x};

      if (state == G_PENDING && !s_suppress) {
        const int dx = x - x0, dy = y - y0;
        if (abs(dx) >= SLOP_PX && abs(dx) > abs(dy)) {
          // Finger moving left reveals the next page.
          const int d = dx < 0 ? 1 : -1;
          Src a, b;
          int so;
          if (uiTryBeginSwipe(d, a, b, so)) {
            if (compBeginDrag(a, b, so)) {
              state = G_DRAG;
              dir = d;
              startOff = so;
              s_swipeStarted = true;
            } else {
              uiSwipeAborted();
              state = G_PASS;
            }
          } else {
            state = G_PASS;
          }
        } else if (abs(dy) >= SLOP_PX) {
          state = G_PASS;
        }
      }
      if (state == G_DRAG) {
        const int w = logicalW();
        off = startOff - (x - x0);
        if (off < 0) off = 0;
        if (off > w) off = w;
        compDrag(off);
      }
      publish(x, y, state != G_DRAG && !s_suppress);
      continue;
    }

    if (s_down && now - lastMs >= RELEASE_MS) {
      if (state == G_DRAG) {
        const int w = logicalW();
        float v = 0.0f;  // px per ms, positive = finger moving right
        int first = nsamples - 1;
        while (first > 0 && lastMs - samples[first - 1].ms <= VELOCITY_WINDOW_MS) first--;
        const uint32_t dt = samples[nsamples - 1].ms - samples[first].ms;
        if (dt >= 15) v = (float)(samples[nsamples - 1].x - samples[first].x) / (float)dt;
        const float toward = -v * (float)dir;       // speed toward the new page
        const int progress = abs(off - startOff);
        bool commit;
        if (toward > FLING_PX_PER_MS) commit = true;
        else if (toward < -FLING_PX_PER_MS) commit = false;
        else commit = progress > (int)(w * COMMIT_FRACTION);
        const int target = commit ? (dir > 0 ? w : 0) : startOff;
        const int remaining = abs(target - off);
        uint32_t dur = (uint32_t)remaining * SLIDE_MAX_MS / (uint32_t)w;
        const float speed = fabsf(v);
        if (commit && speed > FLING_PX_PER_MS) {
          const uint32_t flingDur = (uint32_t)(remaining / speed * 1.6f);
          if (flingDur < dur) dur = flingDur;
        }
        if (dur < SLIDE_MIN_MS) dur = SLIDE_MIN_MS;
        if (dur > SLIDE_MAX_MS) dur = SLIDE_MAX_MS;
        compRelease(target, dur, commit ? (int8_t)dir : 0);
      }
      s_down = false;
      s_suppress = false;
      state = G_IDLE;
      publish(lastX, lastY, false);
    }
  }
}

}  // namespace

void touchBegin() {
  xTaskCreatePinnedToCore(touchTask, "deck_touch", 4096, nullptr, 10, &s_task, 0);
}

TouchPoint touchForLvgl() {
  portENTER_CRITICAL(&s_mux);
  TouchPoint p = s_lv;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

bool touchIsDown() { return s_down; }

bool touchTakeActivity() {
  if (!s_activity) return false;
  s_activity = false;
  return true;
}

bool touchTakeSwipeStarted() {
  if (!s_swipeStarted) return false;
  s_swipeStarted = false;
  return true;
}

void touchSuppressUntilRelease() {
  s_suppress = true;
  portENTER_CRITICAL(&s_mux);
  s_lv.pressed = false;
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
