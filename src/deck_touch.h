// Guition JC3248W535 touch: the AXS15231B touch half on I2C 0x3B (SDA 4,
// SCL 8), read once per INT edge on GPIO 3 (a frame is only valid once per
// edge), plus the gesture arbiter that decides between an LVGL tap and a
// horizontal page swipe handled by the compositor.
#ifndef DECK_TOUCH_H
#define DECK_TOUCH_H

#include <Arduino.h>

namespace deck {

void touchBegin();

// What the LVGL pointer should see. pressed is false while a swipe owns the
// gesture and while input is suppressed (the touch that woke the display).
struct TouchPoint {
  int16_t x;
  int16_t y;
  bool pressed;
};
TouchPoint touchForLvgl();

bool touchIsDown();
// True once per touch-down (wake-from-sleep detection).
bool touchTakeActivity();
// True once when a swipe took over a touch (the loop cancels LVGL's press).
bool touchTakeSwipeStarted();
// Hide the current touch from LVGL and swipes until the finger lifts.
void touchSuppressUntilRelease();

}  // namespace deck

#endif  // DECK_TOUCH_H
