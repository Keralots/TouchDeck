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
// Latest waveform (128 samples centred on 128), null when the companion sends none.
const uint8_t* vizWave();
uint32_t vizRxMs();
uint32_t vizSerial();  // bumps per accepted packet
uint8_t vizPeak();     // loudest band of the latest packet
// The PC plays sound, with hysteresis (see deck_viz.cpp). No packets = silence.
bool vizSoundOn();
