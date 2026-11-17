#pragma once

#include "board.h"

#if BOARD_HAS_ENCODER
#include "settings.h"

struct EncoderInput {
  int detents = 0;
  bool activity = false;
  bool keyDown = false;
  bool turnWithKey = false;
  bool pressed = false;
  bool released = false;
};

void initEncoders();
void stopEncoders();
EncoderInput encoderPoll(size_t i);
void encoderDiscardRemainder(size_t i);
void encoderStatus(size_t i, int& count, bool& key);

bool encoderUsesPin(uint8_t pin);
bool encoderHaEntity(const char* id);
String encoderPinError(const EncoderSettings* items, size_t i, const LedSettings& led,
                       const HapticSettings& haptic, bool sdCard);
void sanitizeEncoderSettings();
#else
inline bool encoderUsesPin(uint8_t) { return false; }
#endif
