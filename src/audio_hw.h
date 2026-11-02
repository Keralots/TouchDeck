// Board audio hardware around the shared I2S stream of audio.cpp: a codec to
// set up, an amplifier to switch. Implemented by audio_hw_<kind>.cpp.
#ifndef AUDIO_HW_H
#define AUDIO_HW_H

#include <Arduino.h>

// The I2S channel runs at `rate` (a codec needs MCLK before it is set up):
// configure the codec for it and power the amplifier.
bool audioHwStart(uint32_t rate);
// The channel is about to stop.
void audioHwStop();

#endif  // AUDIO_HW_H
