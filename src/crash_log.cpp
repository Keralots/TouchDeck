#include "crash_log.h"
#include "config.h"
#include <Preferences.h>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {

constexpr uint32_t CRASH_MAGIC = 0x43524153;
constexpr unsigned CRASH_DEPTH = 16;

struct CrashRecord {
  uint32_t magic;
  int core;
  uint32_t pc;
  char reason[48];
  bool corrupt, more;
  uint8_t depth;
  uint32_t backtrace[CRASH_DEPTH];
};
RTC_NOINIT_ATTR CrashRecord s_rec;
String s_last;

// Panic context: plain copies only. Needs -Wl,--wrap=esp_panic_handler.
void onPanic(arduino_panic_info_t* info, void*) {
  s_rec.core = info->core;
  s_rec.pc = (uint32_t)info->pc;
  const char* reason = info->reason ? info->reason : "";
  size_t n = 0;
  for (; n < sizeof(s_rec.reason) - 1 && reason[n]; n++) s_rec.reason[n] = reason[n];
  s_rec.reason[n] = 0;
  s_rec.depth = info->backtrace_len < CRASH_DEPTH ? info->backtrace_len : CRASH_DEPTH;
  for (unsigned i = 0; i < s_rec.depth; i++) s_rec.backtrace[i] = info->backtrace[i];
  s_rec.corrupt = info->backtrace_corrupt;
  s_rec.more = info->backtrace_continues || info->backtrace_len > CRASH_DEPTH;
  s_rec.magic = CRASH_MAGIC;
}

}  // namespace

void crashLogBegin() {
  Preferences prefs;
  // RTC memory holds garbage after power-on.
  if (s_rec.magic == CRASH_MAGIC && esp_reset_reason() != ESP_RST_POWERON) {
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    char line[300];
    int len = snprintf(line, sizeof(line), "elf %s core %d %s pc 0x%08lx bt", sha, s_rec.core,
                       s_rec.reason, (unsigned long)s_rec.pc);
    for (unsigned i = 0; i < s_rec.depth && i < CRASH_DEPTH && len < (int)sizeof(line) - 12; i++)
      len += snprintf(line + len, sizeof(line) - len, " 0x%08lx", (unsigned long)s_rec.backtrace[i]);
    if (len < (int)sizeof(line) - 12)
      snprintf(line + len, sizeof(line) - len, "%s", s_rec.corrupt ? " |corrupt" : s_rec.more ? " ..." : "");
    s_last = line;
    Serial.printf("Last crash: %s\n", line);
    if (prefs.begin(NVS_NAMESPACE, false)) {
      prefs.putString("crash", s_last);
      prefs.end();
    }
  } else if (prefs.begin(NVS_NAMESPACE, true)) {
    s_last = prefs.getString("crash", "");
    prefs.end();
  }
  s_rec.magic = 0;
  set_arduino_panic_handler(onPanic, nullptr);
}

const String& lastCrash() { return s_last; }
