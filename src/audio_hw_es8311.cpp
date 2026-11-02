// ES8311 codec on the shared I2C bus plus an amplifier enable pin (Waveshare
// P4 7B). The codec is clocked from MCLK = 256 x rate.
#include "board.h"
#if defined(BOARD_AUDIO_ES8311)
#include "audio_hw.h"
#include <Wire.h>
#include "es8311.h"

namespace {

// The vendor demo's codec level; loudness is set digitally in audio.cpp.
constexpr int CODEC_VOLUME = 90;

es8311_handle_t s_codec = nullptr;

}  // namespace

bool audioHwStart(uint32_t rate) {
  const int mclk = (int)rate * 256;
  if (!s_codec) {
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, 400000);  // a no-op when touch already did it
    s_codec = es8311_create(0, BOARD_CODEC_ADDR);  // bus 0 = Wire
    const es8311_clock_config_t clk = {false, false, true, mclk, (int)rate};
    esp_err_t e = s_codec ? es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = es8311_microphone_config(s_codec, false);
    if (e == ESP_OK) e = es8311_voice_volume_set(s_codec, CODEC_VOLUME, nullptr);
    if (e != ESP_OK) {
      Serial.printf("Audio: ES8311 init failed: %s\n", esp_err_to_name(e));
      if (s_codec) es8311_delete(s_codec);
      s_codec = nullptr;
      return false;
    }
  } else if (es8311_sample_frequency_config(s_codec, mclk, (int)rate) != ESP_OK) {
    Serial.printf("Audio: ES8311 has no clock setting for %u Hz\n", (unsigned)rate);
    return false;
  }
  pinMode(BOARD_AMP_EN, OUTPUT);
  digitalWrite(BOARD_AMP_EN, HIGH);
  return true;
}

void audioHwStop() {
  if (BOARD_AMP_EN >= 0) digitalWrite(BOARD_AMP_EN, LOW);
}

#endif  // BOARD_AUDIO_ES8311
