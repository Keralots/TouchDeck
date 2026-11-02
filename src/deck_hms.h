// Guition deck: Bambu printer errors (print.hms and print.print_error), shared by
// the LAN and Home Assistant sources. Rules follow BambuHelper's hms_lookup.
#ifndef DECK_HMS_H
#define DECK_HMS_H

#include <Arduino.h>

namespace deck {

struct PrinterState;

constexpr uint8_t PRN_HMS_MAX = 8;       // kept for display, worst first
constexpr uint8_t PRN_HMS_BASELINE = 16;

struct PrinterHms {
  uint32_t attr = 0;  // module in the top byte
  uint32_t code = 0;  // severity in the high 16 bits
  bool quiet = false; // listed, never raises the badge
  bool fresh = false; // appeared after the baseline: worth an alert
};

// Codes standing when a source first sees the printer never alert. Kept for the
// session; a code that clears leaves the set, so its return alerts.
struct HmsBaseline {
  uint64_t key[PRN_HMS_BASELINE];
  uint8_t count = 0;
  bool saturated = false;  // too many standing codes: nothing on this printer alerts
  bool open = true;        // the next hms report fills the set
  bool ownRejected = false;  // the standing command refusal is ours
  void reset() { count = 0; saturated = false; open = true; ownRejected = false; }
};

// What a source knows besides the codes, for the filters.
struct HmsContext {
  bool doorShown = false;     // the deck shows the door, so the door advisory is muted
  bool ownCommandRecent = false;  // we sent a control command within HMS_OWN_CMD_MS
};
constexpr uint32_t HMS_OWN_CMD_MS = 30000;

inline uint64_t hmsKey(uint32_t attr, uint32_t code) { return ((uint64_t)attr << 32) | code; }
// 1 fatal, 2 serious, 3 common; anything else is info.
inline uint8_t hmsSeverity(uint32_t code) { return code >> 16 > 255 ? 255 : (uint8_t)(code >> 16); }

// One hms report: keys as the printer sent them. Rebuilds s.hms / s.hmsCount.
void hmsReport(PrinterState& s, HmsBaseline& base, const uint64_t* keys, size_t n, const HmsContext& ctx);

const char* hmsSeverityLabel(uint8_t sev);  // "Fatal" / "Serious" / "Common" / "Info"
const char* hmsModuleLabel(uint32_t attr);  // "AMS" / "Mainboard" / "Motion" / "Camera" / "Printer"
uint32_t hmsSeverityColor(uint8_t sev);
// "HMS_0300-0100-0001-0007", as Bambu writes it.
void hmsFormatCode(uint32_t attr, uint32_t code, char* out, size_t size);
// "0300-8007".
void printErrorFormatCode(uint32_t err, char* out, size_t size);
// Bambu's English text, nullptr when the table has none.
const char* hmsText(uint32_t attr, uint32_t code);
const char* printErrorText(uint32_t err);
const char* hmsTableVersion();

// A user cancel also sets print_error; it is a state, not a fault.
bool printErrorIsCancel(uint32_t err);
// After a state or print_error update: print_error drops back to 0 seconds after a
// cancel, so the cancel is kept until the printer moves on to another state.
void printerCancelTrack(PrinterState& s, const char* stateBefore);
bool printerCanceled(const PrinterState& s);

// The worst error worth a badge, or sev 0 when none.
struct PrinterAlert {
  uint8_t sev = 0;       // 1 fatal .. 3 common; print_error counts as 1
  bool printError = false;
  uint32_t attr = 0, code = 0;  // code = print_error value when printError
  bool fresh = false;    // new since the baseline: jump and wake
  // Unique per error; bit 63 marks print_error (HMS attrs never reach it).
  uint64_t key() const { return printError ? (1ULL << 63) | code : hmsKey(attr, code); }
};
constexpr uint8_t PRN_ALERT_MAX = PRN_HMS_MAX + 1;
// Every error worth a badge, worst first (print_error, then HMS); returns the count.
uint8_t printerAlerts(const PrinterState& s, PrinterAlert* out, uint8_t max);
PrinterAlert printerAlert(const PrinterState& s);
// Text for an alert: table text, else a generic line with the module.
String printerAlertText(const PrinterAlert& a);

}  // namespace deck

#endif  // DECK_HMS_H
