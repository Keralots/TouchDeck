// Guition deck: HTTP requests for tile actions ("action": "http").
//
// One worker task, started on first use, sends them off the UI task with
// short timeouts (connect 3 s, answer 5 s); the UI polls the outcomes. The
// portal's Test button goes through the same worker.
#ifndef DECK_HTTP_H
#define DECK_HTTP_H

#include <Arduino.h>
#include "deck_config.h"

namespace deck {

struct HttpOutcome {
  String label;  // the tile's name, for the toast
  int code = 0;  // HTTP status, 0 = no answer (see err)
  String err;
};

// Queue a tile's request. False when the queue is full or the task failed.
bool httpSend(const TileAction& a, const String& label);
// Next finished tile request, if any.
bool httpTakeResult(HttpOutcome& out);
// Portal test: queue a request; returns its ticket, 0 = busy.
uint32_t httpTestStart(const TileAction& a);
// The test's outcome once it has run (false while it waits). body = the
// first 512 bytes of the answer.
bool httpTestResult(uint32_t ticket, int& code, String& err, String& body);

}  // namespace deck

#endif  // DECK_HTTP_H
