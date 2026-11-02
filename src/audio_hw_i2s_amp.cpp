// Plain I2S amplifier (NS4168 on the JC3248W535): no codec, and the amp wakes
// by itself when BCLK starts.
#include "board.h"
#if defined(BOARD_AUDIO_I2S_AMP)
#include "audio_hw.h"

bool audioHwStart(uint32_t) { return true; }
void audioHwStop() {}

#endif  // BOARD_AUDIO_I2S_AMP
