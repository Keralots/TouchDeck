#include "deck_viz.h"

VizStats vizStats = {};
uint32_t vizFrameMs = 20;
bool vizPartial = true;

static uint8_t s_bands[VIZ_BANDS];
static uint8_t s_wave[VIZ_WAVE_POINTS];
static bool s_hasWave = false;
static uint32_t s_rxMs = 0;
static uint32_t s_serial = 0;

bool vizIngest(const uint8_t* buf, int len) {
  if (len < 4 + VIZ_BANDS || memcmp(buf, "FFT1", 4)) return false;
  memcpy(s_bands, buf + 4, VIZ_BANDS);
  s_hasWave = len >= 4 + VIZ_BANDS + VIZ_WAVE_POINTS;
  if (s_hasWave) memcpy(s_wave, buf + 4 + VIZ_BANDS, VIZ_WAVE_POINTS);
  const uint32_t now = millis();
  if (s_rxMs && now - s_rxMs < 1000 && now - s_rxMs > vizStats.rxGapMax) vizStats.rxGapMax = now - s_rxMs;
  s_rxMs = now;
  s_serial++;
  vizStats.packets++;
  return true;
}

const uint8_t* vizBands() { return s_bands; }
uint32_t vizRxMs() { return s_rxMs; }
const uint8_t* vizWave() { return s_hasWave ? s_wave : nullptr; }
uint32_t vizSerial() { return s_serial; }
