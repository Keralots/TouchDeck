#include "encoder.h"

#if BOARD_HAS_ENCODER
#include "led.h"
#include <cstring>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>

namespace {

constexpr int COUNT_LIMIT = 1000;
constexpr uint32_t KEY_DEBOUNCE_MS = 25;

struct EncoderRt {
  pcnt_unit_handle_t unit = nullptr;
  pcnt_channel_handle_t a = nullptr, b = nullptr;
  bool enabled = false, started = false;
  uint8_t pinA = 0, pinB = 0, pinKey = 0;
  int lastCount = 0, remainder = 0;
  bool rawKey = false, key = false;
  uint32_t keyChangedAt = 0;
};

EncoderRt s_encoders[ENCODER_COUNT];

bool usesPin(const EncoderSettings& s, uint8_t pin) {
  return s.enabled && pin && (s.pinA == pin || s.pinB == pin || s.pinKey == pin);
}

void stopEncoder(EncoderRt& r) {
  if (r.started) pcnt_unit_stop(r.unit);
  if (r.enabled) pcnt_unit_disable(r.unit);
  if (r.b) pcnt_del_channel(r.b);
  if (r.a) pcnt_del_channel(r.a);
  if (r.unit) pcnt_del_unit(r.unit);
  for (uint8_t pin : {r.pinA, r.pinB, r.pinKey}) {
    if (!pin) continue;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
  }
  r = {};
}

bool startEncoder(EncoderRt& r, const EncoderSettings& s) {
  r.pinA = s.pinA;
  r.pinB = s.pinB;
  r.pinKey = s.pinKey;

  pcnt_unit_config_t unit = {};
  unit.low_limit = -COUNT_LIMIT;
  unit.high_limit = COUNT_LIMIT;
  unit.flags.accum_count = 1;
  pcnt_glitch_filter_config_t filter = {};
  filter.max_glitch_ns = 1000;
  if (pcnt_new_unit(&unit, &r.unit) != ESP_OK ||
      pcnt_unit_set_glitch_filter(r.unit, &filter) != ESP_OK) return false;

  // A pin may have been parked as an output by the LED or haptic driver.
  gpio_config_t inputs = {};
  inputs.pin_bit_mask = (1ULL << s.pinA) | (1ULL << s.pinB);
  if (s.pinKey) inputs.pin_bit_mask |= 1ULL << s.pinKey;
  inputs.mode = GPIO_MODE_INPUT;
  inputs.pull_up_en = GPIO_PULLUP_ENABLE;
  if (gpio_config(&inputs) != ESP_OK) return false;
  r.rawKey = r.key = s.pinKey && gpio_get_level((gpio_num_t)s.pinKey) == 0;

  pcnt_chan_config_t chan = {};
  chan.edge_gpio_num = s.pinA;
  chan.level_gpio_num = s.pinB;
  if (pcnt_new_channel(r.unit, &chan, &r.a) != ESP_OK) return false;
  chan.edge_gpio_num = s.pinB;
  chan.level_gpio_num = s.pinA;
  if (pcnt_new_channel(r.unit, &chan, &r.b) != ESP_OK ||
      pcnt_channel_set_edge_action(r.a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                   PCNT_CHANNEL_EDGE_ACTION_INCREASE) != ESP_OK ||
      pcnt_channel_set_edge_action(r.b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                   PCNT_CHANNEL_EDGE_ACTION_DECREASE) != ESP_OK ||
      pcnt_channel_set_level_action(r.a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                    PCNT_CHANNEL_LEVEL_ACTION_INVERSE) != ESP_OK ||
      pcnt_channel_set_level_action(r.b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                    PCNT_CHANNEL_LEVEL_ACTION_INVERSE) != ESP_OK) return false;

  if (pcnt_unit_add_watch_point(r.unit, -COUNT_LIMIT) != ESP_OK ||
      pcnt_unit_add_watch_point(r.unit, COUNT_LIMIT) != ESP_OK ||
      pcnt_unit_enable(r.unit) != ESP_OK) return false;
  r.enabled = true;
  if (pcnt_unit_clear_count(r.unit) != ESP_OK || pcnt_unit_start(r.unit) != ESP_OK) return false;
  r.started = true;
  return true;
}

}  // namespace

bool encoderUsesPin(uint8_t pin) {
  for (const EncoderSettings& s : encoderSettings)
    if (usesPin(s, pin)) return true;
  return false;
}

String encoderPinError(const EncoderSettings* items, size_t i, const LedSettings& led,
                       const HapticSettings& haptic, bool sdCard) {
  const EncoderSettings& s = items[i];
  if (!s.enabled) return {};
  if (!s.pinA || !s.pinB) return "needs GPIOs for A and B.";
  if (s.pinA == s.pinB || s.pinKey == s.pinA || s.pinKey == s.pinB)
    return "needs a different GPIO for each signal.";
  for (uint8_t pin : {s.pinA, s.pinB, s.pinKey}) {
    if (!pin) continue;
    if (!ledPinAllowed(pin, sdCard)) return String("uses reserved or unsupported GPIO ") + pin + ".";
    if (led.enabled && led.pin == pin) return String("shares GPIO ") + pin + " with the status LED.";
    if (haptic.enabled && haptic.pin == pin) return String("shares GPIO ") + pin + " with the vibration motor.";
    for (size_t j = 0; j < i; j++)
      if (usesPin(items[j], pin)) return String("shares GPIO ") + pin + " with Encoder " + (j + 1) + ".";
  }
  return {};
}

bool encoderHaEntity(const char* id) {
  return id && (!strncmp(id, "light.", 6) || !strncmp(id, "media_player.", 13) || !strncmp(id, "climate.", 8));
}

void sanitizeEncoderSettings() {
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    EncoderSettings& s = encoderSettings[i];
    String error = encoderPinError(encoderSettings, i, ledSettings, hapticSettings, dispSettings.sdCard);
    if (error.length()) {
      Serial.printf("Encoder %u %s Disabling.\n", (unsigned)(i + 1), error.c_str());
      s.enabled = false;
    }
    if (!ledPinAllowed(s.pinA)) s.pinA = 0;
    if (!ledPinAllowed(s.pinB)) s.pinB = 0;
    if (!ledPinAllowed(s.pinKey)) s.pinKey = 0;
    if (s.mode > ENCODER_PC) s.mode = ENCODER_FOLLOW;
    s.stepPct = constrain(s.stepPct, 1, 25);
    s.countsPerDetent = s.countsPerDetent == 2 ? 2 : 4;
    s.popupY = constrain(s.popupY, 5, 95);
    s.entity[sizeof(s.entity) - 1] = '\0';
  }
}

void stopEncoders() {
  for (EncoderRt& r : s_encoders) stopEncoder(r);
}

void initEncoders() {
  stopEncoders();
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    if (encoderSettings[i].enabled && !startEncoder(s_encoders[i], encoderSettings[i])) {
      Serial.printf("Encoder %u: PCNT setup failed\n", (unsigned)(i + 1));
      stopEncoder(s_encoders[i]);
    }
  }
}

void encoderStatus(size_t i, int& count, bool& key) {
  const EncoderRt& r = s_encoders[i];
  count = 0;
  key = r.key;
  if (r.started) pcnt_unit_get_count(r.unit, &count);
  if (encoderSettings[i].reverse) count = -count;
}

EncoderInput encoderPoll(size_t i) {
  EncoderInput out;
  EncoderRt& r = s_encoders[i];
  if (!r.started) return out;

  int count;
  bool key;
  encoderStatus(i, count, key);
  const int delta = count - r.lastCount;
  r.lastCount = count;
  r.remainder += delta;
  const int perDetent = encoderSettings[i].countsPerDetent;
  out.detents = r.remainder / perDetent;
  r.remainder %= perDetent;

  const bool rawKey = r.pinKey && gpio_get_level((gpio_num_t)r.pinKey) == 0;
  const uint32_t now = millis();
  out.activity = delta || rawKey != r.rawKey;
  // Counts since the last poll may include movement just before release.
  out.turnWithKey = key || rawKey || r.rawKey;
  r.rawKey = rawKey;
  // A lockout keeps a press seen in just one loop pass responsive.
  if (rawKey != key && now - r.keyChangedAt >= KEY_DEBOUNCE_MS) {
    r.key = rawKey;
    r.keyChangedAt = now;
    out.pressed = rawKey;
    out.released = !rawKey;
    out.activity = true;
  }
  out.keyDown = rawKey || r.key;
  return out;
}

void encoderDiscardRemainder(size_t i) { s_encoders[i].remainder = 0; }

#endif
