// Guition JC3248W535 touch reader and swipe arbiter. See deck_touch.h.
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
// The quick panel is pulled down from this band at the top edge (the page
// header), and opens or closes after this share of its height.
constexpr int EDGE_PX = 30;
constexpr float PANEL_COMMIT_FRACTION = 0.3f;

TaskHandle_t s_task = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TouchPoint s_lv = {0, 0, false};
volatile bool s_down = false;
volatile bool s_activity = false;
volatile bool s_swipeStarted = false;
volatile bool s_suppress = false;

// G_EAT: a swipe with no page that way; LVGL must not see it as a tap.
enum GestureState : uint8_t { G_IDLE, G_PENDING, G_PASS, G_DRAG, G_EAT };

struct Sample {
  uint32_t ms;
  int16_t x, y;
};

// One compositor drag: horizontal (columns), vertical (pages of a column) or
// the quick panel, in compositor offset units: off runs from startOff toward
// commitOff.
struct Drag {
  bool vertical;
  bool panel;
  int startOff, commitOff, range, off;
  int8_t landing;  // reported when the drag commits
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
  bool noSwipe = false;
  uint32_t lastMs = 0;
  Drag drag = {};
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
        noSwipe = uiNoSwipeAt(x, y);
        nsamples = 0;
      }
      lastX = x;
      lastY = y;
      lastMs = now;
      if (nsamples == NSAMPLES) {
        memmove(samples, samples + 1, sizeof(Sample) * (NSAMPLES - 1));
        nsamples--;
      }
      samples[nsamples++] = {now, x, y};

      if (state == G_PENDING && !s_suppress) {
        const int dx = x - x0, dy = y - y0;
        if (noSwipe && (abs(dx) >= SLOP_PX || abs(dy) >= SLOP_PX)) {
          state = G_PASS;  // a slider tile takes the drag
        } else if (abs(dx) >= SLOP_PX && abs(dx) > abs(dy)) {
          // Finger moving left reveals the next page.
          const int d = dx < 0 ? 1 : -1;
          Src a, b;
          int so, idxA;
          if (uiTryBeginSwipe(d, false, a, b, so, idxA)) {
            if (compBeginDrag(a, b, so, idxA, false)) {
              state = G_DRAG;
              const int w = logicalW();
              drag = {false, false, so, d > 0 ? w : 0, w, so, (int8_t)d};
              s_swipeStarted = true;
            } else {
              uiSwipeAborted();
              state = G_PASS;
            }
          } else {
            state = uiSwipeOwnsTouch() ? G_EAT : G_PASS;
          }
        } else if (abs(dy) >= SLOP_PX && abs(dy) > abs(dx)) {
          // Pull the quick panel down from the top edge, or push an open
          // one back up from anywhere. Elsewhere a vertical drag moves
          // between the pages of a column.
          const bool open = uiPanelIsOpen();
          state = G_PASS;
          if ((!open && dy > 0 && y0 < EDGE_PX) || (open && dy < 0)) {
            Src panel, page;
            int so, ph;
            if (uiTryBeginPanel(!open, panel, page, so, ph)) {
              if (compBeginPanel(panel, page, so)) {
                state = G_DRAG;
                drag = {true, true, so, open ? 0 : ph, ph, so, (int8_t)(open ? -1 : 1)};
                s_swipeStarted = true;
              } else {
                uiSwipeAborted();
              }
            }
          } else if (!open && y0 >= EDGE_PX) {
            // Finger moving up reveals the page below.
            const int d = dy < 0 ? 1 : -1;
            Src a, b;
            int so, idxA;
            if (uiTryBeginSwipe(d, true, a, b, so, idxA)) {
              if (compBeginDrag(a, b, so, idxA, true)) {
                state = G_DRAG;
                const int h = logicalH();
                drag = {true, false, so, d > 0 ? h : 0, h, so, (int8_t)d};
                s_swipeStarted = true;
              } else {
                uiSwipeAborted();
              }
            } else if (uiSwipeOwnsTouch()) {
              state = G_EAT;
            }
          }
        }
        if (state == G_EAT) s_swipeStarted = true;  // deckTick resets LVGL's press
      }
      if (state == G_DRAG) {
        int off = drag.panel      ? drag.startOff + (y - y0)
                  : drag.vertical ? drag.startOff - (y - y0)
                                  : drag.startOff - (x - x0);
        if (off < 0) off = 0;
        if (off > drag.range) off = drag.range;
        drag.off = off;
        compDrag(off);
      }
      publish(x, y, state != G_DRAG && state != G_EAT && !s_suppress);
      continue;
    }

    if (s_down && now - lastMs >= RELEASE_MS) {
      if (state == G_DRAG) {
        // Offset velocity (px/ms) over the last samples. Page offsets grow as
        // the finger moves left or up, the panel's as it moves down.
        float v = 0.0f;
        int first = nsamples - 1;
        while (first > 0 && lastMs - samples[first - 1].ms <= VELOCITY_WINDOW_MS) first--;
        const uint32_t dt = samples[nsamples - 1].ms - samples[first].ms;
        if (dt >= 15) {
          const Sample& a = samples[first];
          const Sample& b = samples[nsamples - 1];
          v = drag.panel      ? (float)(b.y - a.y) / dt
            : drag.vertical ? -(float)(b.y - a.y) / dt
                            : -(float)(b.x - a.x) / dt;
        }
        const float toward = drag.commitOff > drag.startOff ? v : -v;
        const int progress = abs(drag.off - drag.startOff);
        const float fraction = drag.panel ? PANEL_COMMIT_FRACTION : COMMIT_FRACTION;
        bool commit;
        if (toward > FLING_PX_PER_MS) commit = true;
        else if (toward < -FLING_PX_PER_MS) commit = false;
        else commit = progress > (int)(drag.range * fraction);
        const int target = commit ? drag.commitOff : drag.startOff;
        const int remaining = abs(target - drag.off);
        uint32_t dur = (uint32_t)remaining * SLIDE_MAX_MS / (uint32_t)drag.range;
        const float speed = fabsf(v);
        if (commit && speed > FLING_PX_PER_MS) {
          const uint32_t flingDur = (uint32_t)(remaining / speed * 1.6f);
          if (flingDur < dur) dur = flingDur;
        }
        if (dur < SLIDE_MIN_MS) dur = SLIDE_MIN_MS;
        if (dur > SLIDE_MAX_MS) dur = SLIDE_MAX_MS;
        compRelease(target, dur, commit ? drag.landing : 0);
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
  // Stack in PSRAM: this task never touches flash (NVS, LittleFS).
  xTaskCreatePinnedToCoreWithCaps(touchTask, "deck_touch", 4096, nullptr, 10, &s_task, 0, MALLOC_CAP_SPIRAM);
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
