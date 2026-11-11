// Audio spectrum feed for the visualizer tile: binary "FFT1" UDP packets from
// the companion ("FFT1" + 32 bands 0..255 [+ 128 waveform bytes]), ~50/s.
#pragma once
#include <Arduino.h>

constexpr int VIZ_BANDS = 32;
constexpr int VIZ_WAVE_POINTS = 128;

// Takes the packet if it is a spectrum packet; true when consumed.
bool vizIngest(const uint8_t* buf, int len);
// Latest bands (always readable) and when they arrived (0 = never).
const uint8_t* vizBands();
uint32_t vizRxMs();
uint32_t vizSerial();  // bumps per accepted packet

// Spike instrumentation (GET /api/deck/viz).
struct VizStats {
  uint32_t packets;      // accepted
  uint32_t superseded;   // replaced before a tick showed them
  uint32_t ticks;        // viz frames that changed a bar
  uint32_t lvUsSum, lvUsMax, lvCalls;  // lv_timer_handler after a viz invalidate
  uint32_t tickGapMax;   // longest ms between viz frames while fed
  uint32_t rxGapMax;     // longest ms between packets while fed
};
extern VizStats vizStats;
extern uint32_t vizFrameMs;  // tick period, overridable for the spike
extern bool vizPartial;      // invalidate only the moved rows (else the whole tile)
