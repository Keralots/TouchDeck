#include "deck_viz.h"
#include <algorithm>

static uint8_t s_bands[VIZ_BANDS];
static uint8_t s_wave[VIZ_WAVE_POINTS];
static bool s_hasWave = false;
static uint32_t s_rxMs = 0;
static uint32_t s_serial = 0;

// "Sound playing": on after VIZ_SOUND_ON_MS of sound without a VIZ_SOUND_GAP_MS
// break (a notification does not count), off after VIZ_SOUND_OFF_MS of silence.
constexpr uint8_t VIZ_SOUND_MIN = 8;  // loudest band; companion AGC sends 0 on digital silence
constexpr uint32_t VIZ_SOUND_ON_MS = 2000;
constexpr uint32_t VIZ_SOUND_OFF_MS = 30000;
constexpr uint32_t VIZ_SOUND_GAP_MS = 500;
static uint32_t s_loudFrom = 0, s_loudMs = 0;
static uint8_t s_peak = 0;
static bool s_loud = false;  // any sound seen yet
static bool s_sound = false;

bool vizIngest(const uint8_t* buf, int len) {
  if (len < 4 + VIZ_BANDS || memcmp(buf, "FFT1", 4)) return false;
  memcpy(s_bands, buf + 4, VIZ_BANDS);
  s_hasWave = len >= 4 + VIZ_BANDS + VIZ_WAVE_POINTS;
  if (s_hasWave) memcpy(s_wave, buf + 4 + VIZ_BANDS, VIZ_WAVE_POINTS);
  const uint32_t now = millis();
  s_peak = 0;
  for (int i = 0; i < VIZ_BANDS; i++) s_peak = std::max(s_peak, s_bands[i]);
  if (s_peak >= VIZ_SOUND_MIN) {
    if (!s_loud || now - s_loudMs > VIZ_SOUND_GAP_MS) s_loudFrom = now;
    s_loudMs = now;
    s_loud = true;
  }
  s_rxMs = now;
  s_serial++;
  return true;
}

const uint8_t* vizBands() { return s_bands; }
uint32_t vizRxMs() { return s_rxMs; }
const uint8_t* vizWave() { return s_hasWave ? s_wave : nullptr; }
uint32_t vizSerial() { return s_serial; }
uint8_t vizPeak() { return s_peak; }

bool vizSoundOn() {
  const uint32_t now = millis();
  const uint32_t quiet = s_loud ? (uint32_t)std::max<int32_t>(0, (int32_t)(now - s_loudMs)) : UINT32_MAX;
  if (s_sound) {
    if (quiet >= VIZ_SOUND_OFF_MS) s_sound = false;
  } else if (quiet <= VIZ_SOUND_GAP_MS && now - s_loudFrom >= VIZ_SOUND_ON_MS) {
    s_sound = true;
  }
  return s_sound;
}
