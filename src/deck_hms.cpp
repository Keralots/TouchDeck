// Guition deck: Bambu printer errors. See deck_hms.h.
#include "deck_hms.h"
#include "deck_bambu.h"
#include "settings.h"
#include "error_tables/hms_table.h"
#include "error_tables/print_error_table.h"

namespace deck {
namespace {

bool described(uint64_t key) {
  uint16_t lo = 0, hi = HMS_TABLE_COUNT;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    if (HMS_KEYS[mid] == key) return true;
    if (HMS_KEYS[mid] < key) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

// Door advisories, muted where the deck shows the door itself.
bool muted(uint64_t key) { return key == 0x0300960000030001ULL || key == 0x0500040000030008ULL; }

// AMS runout pair on a slot (0x20-0x23): the printer already switched spools (BambuHelper #181).
bool selfResolved(uint32_t attr, uint32_t code) {
  if (code != 0x00030001UL && code != 0x00030002UL) return false;
  const uint32_t module = attr >> 24, slot = (attr >> 8) & 0xFF;
  return (module == 0x07 || module == 0x18) && slot >= 0x20 && slot <= 0x23;
}

// "MQTT command verification failed": another client's refused command (BambuHelper #185).
bool rejectedCommand(uint64_t key) { return key == 0x0500050000010007ULL; }

// 0 fatal .. 3 info/unknown.
uint8_t rank(uint32_t code) {
  const uint8_t sev = hmsSeverity(code);
  return sev >= 1 && sev <= 3 ? sev - 1 : 3;
}

bool inBaseline(const HmsBaseline& b, uint64_t key) {
  if (b.saturated) return true;
  for (uint8_t i = 0; i < b.count; i++)
    if (b.key[i] == key) return true;
  return false;
}

void addBaseline(HmsBaseline& b, uint64_t key) {
  if (inBaseline(b, key)) return;
  if (b.count < PRN_HMS_BASELINE) b.key[b.count++] = key;
  else b.saturated = true;
}

}  // namespace

void hmsReport(PrinterState& s, HmsBaseline& base, const uint64_t* keys, size_t n, const HmsContext& ctx) {
  const bool fill = base.open;
  base.open = false;
  // Cleared codes leave the baseline, so a return alerts.
  for (uint8_t i = 0; i < base.count;) {
    bool still = false;
    for (size_t k = 0; k < n && !still; k++) still = keys[k] == base.key[i];
    if (still) i++;
    else base.key[i] = base.key[--base.count];
  }
  if (!fill && base.saturated && !n) base.saturated = false;
  s.hmsCount = 0;
  s.hmsTotal = 0;
  bool rejected = false;
  for (size_t k = 0; k < n; k++) {
    const uint64_t key = keys[k];
    const uint32_t attr = key >> 32, code = (uint32_t)key;
    // Codes Bambu ships no text for stay silent on the printer too (BambuHelper #164);
    // fatal is exempt, the table may predate the printer.
    if (hmsSeverity(code) != 1 && !described(key)) continue;
    if (muted(key) && ctx.doorShown) continue;
    bool dup = false;
    for (uint8_t i = 0; i < s.hmsCount && !dup; i++) dup = hmsKey(s.hms[i].attr, s.hms[i].code) == key;
    if (dup) continue;
    if (s.hmsTotal < 255) s.hmsTotal++;
    if (fill) addBaseline(base, key);
    PrinterHms e;
    e.attr = attr;
    e.code = code;
    // A refusal right after our own command is ours, and stays loud while it stands.
    if (rejectedCommand(key)) {
      rejected = true;
      if (ctx.ownCommandRecent) base.ownRejected = true;
    }
    e.quiet = selfResolved(attr, code) || (rejectedCommand(key) && !base.ownRejected);
    e.fresh = !inBaseline(base, key);
    // Sorted by severity, then key, so a reordered report is not a change.
    uint8_t pos = 0;
    while (pos < s.hmsCount && (rank(s.hms[pos].code) < rank(code) ||
                                (rank(s.hms[pos].code) == rank(code) && hmsKey(s.hms[pos].attr, s.hms[pos].code) < key)))
      pos++;
    if (pos >= PRN_HMS_MAX) continue;
    const uint8_t last = s.hmsCount < PRN_HMS_MAX ? s.hmsCount : PRN_HMS_MAX - 1;
    for (uint8_t i = last; i > pos; i--) s.hms[i] = s.hms[i - 1];
    s.hms[pos] = e;
    if (s.hmsCount < PRN_HMS_MAX) s.hmsCount++;
  }
  if (!rejected) base.ownRejected = false;
}

const char* hmsSeverityLabel(uint8_t sev) {
  switch (sev) {
    case 1: return "Fatal";
    case 2: return "Serious";
    case 3: return "Common";
    default: return "Info";
  }
}

// Module bytes seen in Bambu's feed (BambuHelper hmsModuleLabel).
const char* hmsModuleLabel(uint32_t attr) {
  switch ((attr >> 24) & 0xFF) {
    case 0x07:
    case 0x18: return "AMS";
    case 0x05: return "Mainboard";
    case 0x03: return "Motion";
    case 0x0C: return "Camera";
    default: return "Printer";
  }
}

uint32_t hmsSeverityColor(uint8_t sev) {
  switch (sev) {
    case 1: return 0xE5484D;
    case 2: return 0xF07B2E;
    case 3: return 0xE5A50A;
    default: return 0x9AA3B2;
  }
}

void hmsFormatCode(uint32_t attr, uint32_t code, char* out, size_t size) {
  snprintf(out, size, "HMS_%04X-%04X-%04X-%04X", (unsigned)(attr >> 16), (unsigned)(attr & 0xFFFF),
           (unsigned)(code >> 16), (unsigned)(code & 0xFFFF));
}

void printErrorFormatCode(uint32_t err, char* out, size_t size) {
  snprintf(out, size, "%04X-%04X", (unsigned)(err >> 16), (unsigned)(err & 0xFFFF));
}

const char* hmsText(uint32_t attr, uint32_t code) {
  const uint64_t want = hmsKey(attr, code);
  uint16_t lo = 0, hi = HMS_TABLE_COUNT;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    if (HMS_KEYS[mid] == want) {
      const uint16_t id = HMS_TEXT_ID[mid];
      return id < HMS_TEXT_COUNT ? HMS_TEXT_BLOB + HMS_TEXT_OFF[id] : nullptr;
    }
    if (HMS_KEYS[mid] < want) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

const char* printErrorText(uint32_t err) {
  if (!err) return nullptr;
  uint16_t lo = 0, hi = PRINT_ERROR_TABLE_COUNT;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    if (PRINT_ERROR_KEYS[mid] == err) {
      const uint16_t id = PRINT_ERROR_TEXT_ID[mid];
      return id < PRINT_ERROR_TEXT_COUNT ? PRINT_ERROR_TEXT_BLOB + PRINT_ERROR_TEXT_OFF[id] : nullptr;
    }
    if (PRINT_ERROR_KEYS[mid] < err) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

const char* hmsTableVersion() { return HMS_TABLE_VER; }

// "The task was canceled." / "Printing was cancelled."
bool printErrorIsCancel(uint32_t err) { return err == 0x0300400CUL || err == 0x0500400EUL; }

void printerCancelTrack(PrinterState& s, const char* stateBefore) {
  if (s.gcodeState[0] && strcmp(s.gcodeState, stateBefore) && strcmp(s.gcodeState, "FAILED")) s.canceled = false;
  if (printErrorIsCancel(s.printError)) s.canceled = true;
}

bool printerCanceled(const PrinterState& s) { return !strcmp(s.gcodeState, "FAILED") && s.canceled; }

uint8_t printerAlerts(const PrinterState& s, PrinterAlert* out, uint8_t max) {
  uint8_t n = 0;
  if (!printerSettings.errors) return 0;
  // print_error first: the job stopped.
  if (s.printError && !printErrorIsCancel(s.printError) && n < max) {
    PrinterAlert& a = out[n++];
    a = PrinterAlert{};
    a.sev = 1;
    a.printError = true;
    a.code = s.printError;
    a.fresh = s.printErrorFresh;
  }
  // Info codes are listed only; common ones only with "errorsAll".
  const uint8_t worst = printerSettings.errorsAll ? 3 : 2;
  for (uint8_t i = 0; i < s.hmsCount && n < max; i++) {
    const PrinterHms& e = s.hms[i];
    const uint8_t sev = hmsSeverity(e.code);
    if (sev < 1 || sev > worst || e.quiet) continue;
    PrinterAlert& a = out[n++];
    a = PrinterAlert{};
    a.sev = sev;
    a.attr = e.attr;
    a.code = e.code;
    a.fresh = e.fresh;
  }
  return n;
}

PrinterAlert printerAlert(const PrinterState& s) {
  PrinterAlert a;
  printerAlerts(s, &a, 1);
  return a;
}

String printerAlertText(const PrinterAlert& a) {
  if (!a.sev) return "";
  const char* t = a.printError ? printErrorText(a.code) : hmsText(a.attr, a.code);
  if (t && *t) return t;
  return a.printError ? String("Print error") : String(hmsModuleLabel(a.attr)) + " error";
}

}  // namespace deck
