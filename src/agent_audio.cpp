#include "agent_audio.h"
#include "board.h"
#if defined(BOARD_MIC_ES7210)
#include "audio.h"
#include "audio_hw.h"
#include <Wire.h>
#include <atomic>
#include <driver/i2s_tdm.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <math.h>
#include "es7210.h"

namespace {

// TX and RX both TDM with four 16-bit slots: std TX + TDM RX on one port
// timed out, and ES8311 reads slot 0 (left) and slot 2 (right) of this frame.
constexpr int SLOTS = 4;
constexpr int SLOT_MIC1 = 0, SLOT_MIC2 = 2;
constexpr size_t CHUNK = 240;  // 15 ms per read/write
constexpr uint32_t UP_FRAMES = 8;
constexpr uint32_t PLAY_CAP = 1 << 16;  // samples, ~4 s

std::atomic<bool> s_running{false}, s_stopReq{false}, s_capture{false}, s_flushReq{false};
std::atomic<uint32_t> s_upHead{0}, s_upTail{0}, s_playHead{0}, s_playTail{0};
std::atomic<uint32_t> s_lastPlayMs{0};
std::atomic<int32_t> s_micLevelCenti{-12000}, s_floorCenti{-6000};
constexpr int FLOOR_WIN = 50;  // frames, 3 s
float s_levels[FLOOR_WIN];
int s_levelsN = 0;
int16_t* s_up = nullptr;     // UP_FRAMES x AGENT_FRAME
float s_upDb[UP_FRAMES];
int16_t* s_play = nullptr;   // PLAY_CAP
uint8_t s_pga = 12;
int32_t s_boostQ8 = 256;

float dbfs(const int16_t* x, size_t n) {
  int64_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (int32_t)x[i] * x[i];
  const float rms = sqrtf((float)acc / n);
  return rms < 1.0f ? -96.0f : 20.0f * log10f(rms / 32768.0f);
}

bool codecStart() {
  audio_hal_codec_config_t cfg = {};
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
  esp_err_t e = es7210_adc_init(&Wire, &cfg);
  e |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  e |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), (es7210_gain_value_t)s_pga);
  e |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4), GAIN_0DB);
  // REG12 = 0x02: all four ADCs on SDOUT1 as TDM (esp_codec_dev es7210.c).
  Wire.beginTransmission(BOARD_MIC_ADDR);
  Wire.write(0x12);
  Wire.write(0x02);
  e |= Wire.endTransmission();
  e |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  return e == ESP_OK;
}

void engine(void*) {
  static int16_t txBuf[CHUNK * SLOTS], rxBuf[CHUNK * SLOTS];
  static int16_t acc[AGENT_FRAME];
  size_t accN = 0;
  uint32_t settle = AGENT_RATE / 5;  // the ES7210 starts on near-zero samples
  i2s_chan_handle_t tx = nullptr, rx = nullptr;
  const int32_t playQ12 = audioGainQ12();
  esp_err_t e;

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = CHUNK;
  cc.auto_clear = true;
  i2s_tdm_config_t tc = {
    .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AGENT_RATE),
    .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                     (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)),
    .gpio_cfg = {
      .mclk = (gpio_num_t)BOARD_I2S_MCLK,
      .bclk = (gpio_num_t)BOARD_I2S_BCLK,
      .ws = (gpio_num_t)BOARD_I2S_WS,
      .dout = (gpio_num_t)BOARD_I2S_DOUT,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {false, false, false},
    },
  };
  tc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tc.clk_cfg.bclk_div = 8;
  tc.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
  tc.slot_cfg.left_align = false;
  // Each channel only gets its own data pin, or the second init re-routes the first.
  i2s_tdm_config_t rc = tc;
  rc.gpio_cfg.dout = I2S_GPIO_UNUSED;
  rc.gpio_cfg.din = (gpio_num_t)BOARD_I2S_DIN;

  e = i2s_new_channel(&cc, &tx, &rx);
  if (e == ESP_OK) e = i2s_channel_init_tdm_mode(tx, &tc);
  if (e == ESP_OK) e = i2s_channel_init_tdm_mode(rx, &rc);
  if (e == ESP_OK) e = i2s_channel_enable(tx);
  if (e == ESP_OK) e = i2s_channel_enable(rx);
  if (e != ESP_OK) Serial.printf("Agent audio: I2S %s\n", esp_err_to_name(e));
  if (e == ESP_OK && !audioHwStart(AGENT_RATE)) e = ESP_FAIL;
  if (e == ESP_OK && !codecStart()) {
    Serial.println("Agent audio: ES7210 did not start");
    e = ESP_FAIL;
  }

  while (e == ESP_OK && !s_stopReq) {
    if (s_flushReq.exchange(false)) s_playTail.store(s_playHead.load());
    const uint32_t head = s_playHead.load(std::memory_order_acquire);
    uint32_t tail = s_playTail.load();
    const uint32_t n = min<uint32_t>(head - tail, CHUNK);
    for (uint32_t i = 0; i < CHUNK; i++) {
      int16_t v = 0;
      if (i < n) v = (int16_t)(((int32_t)s_play[(tail + i) & (PLAY_CAP - 1)] * playQ12) >> 12);
      for (int c = 0; c < SLOTS; c++) txBuf[i * SLOTS + c] = v;
    }
    if (n) {
      s_playTail.store(tail + n, std::memory_order_release);
      s_lastPlayMs.store(millis());
    }
    size_t done = 0;
    i2s_channel_write(tx, txBuf, sizeof(txBuf), &done, pdMS_TO_TICKS(100));
    if ((e = i2s_channel_read(rx, rxBuf, sizeof(rxBuf), &done, pdMS_TO_TICKS(100))) != ESP_OK) {
      Serial.printf("Agent audio: read %s\n", esp_err_to_name(e));
      break;
    }
    const size_t got = done / (SLOTS * sizeof(int16_t));
    if (settle) {
      settle = got >= settle ? 0 : settle - got;
      continue;
    }
    for (size_t i = 0; i < got; i++) {
      const int32_t m = ((int32_t)rxBuf[i * SLOTS + SLOT_MIC1] + rxBuf[i * SLOTS + SLOT_MIC2]) * s_boostQ8 >> 9;
      acc[accN++] = (int16_t)constrain(m, -32768, 32767);
      if (accN < AGENT_FRAME) continue;
      accN = 0;
      const float lv = dbfs(acc, AGENT_FRAME);
      s_micLevelCenti.store((int32_t)(lv * 100));
      // Frames that heard the speaker would lift the floor over the user's voice.
      if (millis() - s_lastPlayMs.load() > 500) {
        s_levels[s_levelsN++ % FLOOR_WIN] = lv;
        float w[FLOOR_WIN];
        const int k = min(s_levelsN, FLOOR_WIN);
        memcpy(w, s_levels, k * sizeof(float));
        std::nth_element(w, w + k / 10, w + k);
        s_floorCenti.store((int32_t)(w[k / 10] * 100));
      }
      const uint32_t uh = s_upHead.load();
      if (s_capture && uh - s_upTail.load(std::memory_order_acquire) < UP_FRAMES) {
        memcpy(s_up + (uh % UP_FRAMES) * AGENT_FRAME, acc, sizeof(acc));
        s_upDb[uh % UP_FRAMES] = lv;
        s_upHead.store(uh + 1, std::memory_order_release);
      }
    }
  }

  es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
  audioHwStop();
  if (tx) { i2s_channel_disable(tx); i2s_del_channel(tx); }
  if (rx) { i2s_channel_disable(rx); i2s_del_channel(rx); }
  s_capture = false;
  s_running = false;
}

}  // namespace

bool agentAudioStart(uint8_t pga, uint8_t boostDb) {
  if (s_running) return false;
  if (!s_up) s_up = (int16_t*)heap_caps_malloc(UP_FRAMES * AGENT_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!s_play) s_play = (int16_t*)heap_caps_malloc(PLAY_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!s_up || !s_play) return false;
  s_pga = min<uint8_t>(pga, 14);
  s_boostQ8 = (int32_t)lroundf(256.0f * powf(10.0f, boostDb / 20.0f));
  s_upHead = s_upTail = 0;
  s_levelsN = 0;
  s_playHead = s_playTail = 0;
  s_capture = false;
  s_flushReq = false;
  s_stopReq = false;
  s_running = true;
  if (!audioRunExclusive(engine, nullptr)) {
    s_running = false;
    return false;
  }
  return true;
}

void agentAudioStop() { s_stopReq = true; }
bool agentAudioRunning() { return s_running; }
void agentAudioCapture(bool on) { s_capture = on; }
float agentAudioMicLevel() { return s_micLevelCenti.load() / 100.0f; }
float agentAudioFloor() { return s_floorCenti.load() / 100.0f; }
bool agentAudioFloorReady() { return s_levelsN >= 10; }

bool agentAudioPopFrame(int16_t* dst, float& db) {
  const uint32_t t = s_upTail.load();
  if (t == s_upHead.load(std::memory_order_acquire)) return false;
  memcpy(dst, s_up + (t % UP_FRAMES) * AGENT_FRAME, AGENT_FRAME * sizeof(int16_t));
  db = s_upDb[t % UP_FRAMES];
  s_upTail.store(t + 1, std::memory_order_release);
  return true;
}

size_t agentAudioPush(const int16_t* pcm, size_t n) {
  if (!s_play) return 0;
  const uint32_t h = s_playHead.load();
  const uint32_t room = PLAY_CAP - (h - s_playTail.load(std::memory_order_acquire));
  n = min<size_t>(n, room);
  for (size_t i = 0; i < n; i++) s_play[(h + i) & (PLAY_CAP - 1)] = pcm[i];
  s_playHead.store(h + n, std::memory_order_release);
  return n;
}

uint32_t agentAudioQueued() { return s_playHead.load() - s_playTail.load(); }
void agentAudioFlush() { s_flushReq = true; }
uint32_t agentAudioLastPlayMs() { return s_lastPlayMs.load(); }

#else

bool agentAudioStart(uint8_t, uint8_t) { return false; }
void agentAudioStop() {}
bool agentAudioRunning() { return false; }
void agentAudioCapture(bool) {}
bool agentAudioPopFrame(int16_t*, float&) { return false; }
float agentAudioMicLevel() { return -96.0f; }
float agentAudioFloor() { return -96.0f; }
bool agentAudioFloorReady() { return false; }
size_t agentAudioPush(const int16_t*, size_t) { return 0; }
uint32_t agentAudioQueued() { return 0; }
void agentAudioFlush() {}
uint32_t agentAudioLastPlayMs() { return 0; }

#endif  // BOARD_MIC_ES7210
