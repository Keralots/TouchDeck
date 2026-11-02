// Contract between the touch task (deck_touch.cpp) and the controller driver
// of the board (deck_touch_<chip>.cpp). Only those files include it.
#ifndef DECK_TOUCH_INT_H
#define DECK_TOUCH_INT_H

#include <Arduino.h>

namespace deck {

// Bus and controller setup, called from the touch task. Returns true when the
// controller's interrupt line notifies `task` on new data, false when the
// task has to poll.
bool touchHwBegin(TaskHandle_t task);
// One read: true with native panel coordinates while a finger is down.
bool touchHwRead(int& nx, int& ny);

}  // namespace deck

#endif  // DECK_TOUCH_INT_H
