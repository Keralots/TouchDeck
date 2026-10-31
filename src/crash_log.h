#ifndef CRASH_LOG_H
#define CRASH_LOG_H

#include <Arduino.h>

// Last panic: reason, PC and backtrace caught by the panic hook, kept in RTC
// memory across the reboot and then in NVS. Decode with addr2line against the
// ELF whose sha256 prefix the line names.

// Early in setup(): store a fresh record and install the hook.
void crashLogBegin();
// "" when no panic has been recorded.
const String& lastCrash();

#endif  // CRASH_LOG_H
