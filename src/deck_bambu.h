// Guition deck: Bambu Lab printers over LAN MQTT (TLS :8883, user "bblp",
// the printer's access code). One worker task serves every powered printer;
// a printer that is not powered has no connection, no buffers and no state.
// The UI reads copies under a lock and redraws when printersVersion() moves.
#ifndef DECK_BAMBU_H
#define DECK_BAMBU_H

#include <Arduino.h>
#include <vector>

namespace deck {

constexpr int PRN_MAX = 2;          // printers configured at once
constexpr int PRN_AMS_UNITS = 6;    // AMS / AMS 2 Pro (4 slots) and AMS HT (1 slot)
constexpr int PRN_AMS_SLOTS = 4;
constexpr int PRN_MAP_MAX = 16;     // filaments a job uses, in slicer order

// What the deck needs to reach a printer. serial and code never leave the
// device (NVS, see deck_printers.cpp).
struct PrinterConn {
  String id;      // deck.json printer id ("h2c")
  String ip;
  String serial;
  String code;    // LAN access code
};

enum PrinterLink : uint8_t {
  PRN_OFF = 0,     // not powered / stopped: nothing allocated
  PRN_CONNECTING,  // TLS + MQTT handshake or waiting to retry
  PRN_ONLINE,      // subscribed, reports arriving
  PRN_FAILED,      // last attempt failed (err says why); retrying with backoff
};

struct PrinterTray {
  bool present = false;
  uint32_t rgb = 0;      // 0xRRGGBB from tray_color
  uint8_t alpha = 255;   // tray_color's last byte (translucent filaments)
  char type[24] = "";    // tray_sub_brands, else tray_type ("PLA Basic", "PETG")
  int8_t remain = -1;    // 0..100 %, -1 = unknown (no RFID / third party)
};

struct PrinterAmsUnit {
  bool present = false;
  uint8_t id = 0;            // raw MQTT id: 0..3 AMS, 128.. AMS HT
  uint8_t slots = 0;         // trays this unit has (4 or 1)
  uint8_t humidity = 255;    // raw RH %, 255 = not reported
  float temp = NAN;          // inside the unit, C
  uint16_t dryRemainMin = 0; // 0 = not drying
  PrinterTray tray[PRN_AMS_SLOTS];
};

struct PrinterState {
  PrinterLink link = PRN_OFF;
  char err[48] = "";
  uint32_t lastReportMs = 0;  // millis() of the last report, 0 = none yet

  // Job
  char gcodeState[16] = "";   // IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED
  char job[64] = "";          // subtask_name
  uint8_t progress = 0;       // %
  uint16_t remainingMin = 0;
  uint16_t layer = 0, totalLayers = 0;
  uint8_t speedLevel = 0;     // 1 silent, 2 standard, 3 sport, 4 ludicrous

  // Temperatures (C); NAN = not reported by this model
  bool dualNozzle = false;    // H2C / H2D
  uint8_t activeNozzle = 0;   // 0 = right, 1 = left (dual only)
  float nozzle[2] = {NAN, NAN}, nozzleTarget[2] = {NAN, NAN};  // [0] = the only / right nozzle
  float bed = NAN, bedTarget = NAN;
  float chamber = NAN;        // NAN on models without a chamber sensor (P1S)

  int8_t light = -1;          // chamber light: -1 unknown, 0 off, 1 on

  // AMS
  uint8_t amsCount = 0;
  PrinterAmsUnit ams[PRN_AMS_UNITS];
  uint8_t activeUnit = 255;   // index into ams[], 254 = external spool, 255 = none
  uint8_t activeSlot = 255;
  bool extPresent = false;
  PrinterTray ext;            // external spool (vt_tray)
  uint8_t mapCount = 0;       // print.mapping of the current job
  uint16_t map[PRN_MAP_MAX];  // (unit raw id << 8) | slot
};

// Replace the printer list. Printers no longer listed are stopped; a changed
// ip / serial / code reconnects. Does not power anything on.
void printersSet(const std::vector<PrinterConn>& list);
// Power gate: on starts connecting (with backoff), off disconnects and frees
// every buffer and the state of that printer.
void printerPower(const String& id, bool on);
bool printerPowered(const String& id);
// Copy of a printer's state; false when the id is unknown or not powered.
bool printerRead(const String& id, PrinterState& out);
// Moves whenever any printer's state or link changes.
uint32_t printersVersion();
// Chamber light (both bars on H2C/H2D). Queued; false when not online.
bool printerLight(const String& id, bool on);

}  // namespace deck

#endif  // DECK_BAMBU_H
