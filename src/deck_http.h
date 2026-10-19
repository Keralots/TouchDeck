// Guition deck: one worker for HTTP actions, sensor polls and portal tests.
// Queued actions run before due polls; the UI reads results under a lock.
#ifndef DECK_HTTP_H
#define DECK_HTTP_H

#include <Arduino.h>
#include <IPAddress.h>
#include <memory>
#include <vector>
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
uint32_t httpTestStart(const TileAction& a, bool sensor = false);
// The test's outcome once it has run (false while it waits). body = the
// first 512 bytes of an action answer, or 8192 bytes for a sensor test.
bool httpTestResult(uint32_t ticket, int& code, String& err, std::shared_ptr<char>& body);

// One cover job at a time, including a result waiting for the UI.
bool httpArtFetch(IPAddress ip, uint16_t port, const char* id, int px);
// A finished job, including failure (pixels == nullptr). Caller owns pixels.
bool httpArtTake(String& id, int& px, uint8_t*& pixels);

constexpr int HTTP_HISTORY_POINTS = 48;
struct HttpSensorValue {
  bool present = false, numeric = false, offline = false;
  float number = 0;
  std::shared_ptr<char> text;
  float history[HTTP_HISTORY_POINTS];
};

void httpSetSensors(const DeckConfig& cfg);
uint32_t httpSensorVersion();
bool httpSensorRead(const HttpSensorDef& def, const String& path, HttpSensorValue& out);

// A detail template split into text and "{path|unit}" fields.
struct DetailPart {
  bool field = false;
  String text;  // literal text, or the field's JSON path
  String unit;  // field: show a number as a duration in this unit
};
std::vector<DetailPart> httpParseDetail(const String& tmpl);

}  // namespace deck

#endif  // DECK_HTTP_H
