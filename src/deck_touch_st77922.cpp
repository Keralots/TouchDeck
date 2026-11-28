// ST77922 TDDI touch, polled over I2C (16-bit registers, big-endian address,
// address 0x55). The touch controller sits in the panel chip itself, so the
// coordinates are native panel pixels; the protocol is Freenove's
// (Libraries/FNK0104N/TFT_eSPI_v2.5.43/ST77922_Touch.cpp).
#include "board.h"
#if defined(BOARD_TOUCH_ST77922)
#include "deck_touch_int.h"
#include "deck_panel.h"
#include <Wire.h>

namespace deck {
namespace {

constexpr uint8_t ADDR = 0x55;
constexpr uint16_t REG_FW_VER = 0x0000;
constexpr uint16_t REG_STATUS = 0x0001;       // low nibble: samples not yet read
constexpr uint16_t REG_MAX_TOUCHES = 0x0009;  // how many points the fw reports
constexpr uint16_t REG_TOUCH_INFO = 0x0010;   // bit 3 = new sample
constexpr uint16_t REG_TOUCH_POINT0 = 0x0014; // 7 bytes per point

bool s_ready = false;
uint8_t s_points = 1;
// The controller only flags NEW samples; between them the last contact still
// stands, exactly as on the GT911.
bool s_down = false;
int s_x = 0, s_y = 0;

bool readReg(uint16_t reg, uint8_t* d, size_t n) {
  Wire.beginTransmission(ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

}  // namespace

bool touchHwBegin(TaskHandle_t) {
  pinMode(BOARD_TOUCH_RST, OUTPUT);
  digitalWrite(BOARD_TOUCH_RST, HIGH);
  pinMode(BOARD_TOUCH_INT, INPUT);

  digitalWrite(BOARD_TOUCH_RST, LOW);
  delay(100);
  digitalWrite(BOARD_TOUCH_RST, HIGH);
  delay(100);

  Wire.begin(BOARD_TOUCH_SDA, BOARD_TOUCH_SCL, 400000);
  Wire.setTimeOut(5);  // bounded; Wire's default is 50 ms

  // The status nibble counts samples the controller has not handed out yet.
  uint8_t st = 0xFF;
  int tries = 0;
  for (; tries < 20; tries++) {
    if (readReg(REG_STATUS, &st, 1) && (st & 0x0F) == 0) break;
    delay(10);
  }
  if (tries == 20) {
    Serial.printf("Touch: ST77922 not ready (status 0x%02X)\n", st);
    return false;
  }

  uint8_t fw = 0, points = 0;
  readReg(REG_FW_VER, &fw, 1);
  readReg(REG_MAX_TOUCHES, &points, 1);
  s_points = points && points <= 10 ? points : 1;
  s_ready = true;
  Serial.printf("Touch: ST77922 fw %u, %u touch points\n", fw, points);
  return false;  // no interrupt wired into the task: the touch task polls
}

bool touchHwRead(int& nx, int& ny) {
  if (!s_ready) return false;
  // Match the vendor driver's read sequence: poll INFO/STATUS first, then the
  // whole point buffer. Reading the points without the preceding status reads
  // leaves the controller serving stale data, and reading only point 0 keeps
  // the NEW-sample flag latched so a release never arrives.
  uint8_t info = 0, st = 0;
  readReg(REG_TOUCH_INFO, &info, 1);
  readReg(REG_STATUS, &st, 1);
  uint8_t d[70] = {};
  const size_t len = 7 * (size_t)s_points;
  if (!readReg(REG_TOUCH_POINT0, d, len)) return false;
  const bool down = (d[0] & 0x80) != 0;
  s_down = down;
  if (down) {
    s_x = ((d[0] & 0x3F) << 8) | d[1];
    s_y = ((d[2] & 0x3F) << 8) | d[3];
  }
  if (!s_down || s_x >= NATIVE_W || s_y >= NATIVE_H) return false;
#if BOARD_PANEL_FLIP
  nx = NATIVE_W - 1 - s_x;
  ny = NATIVE_H - 1 - s_y;
#else
  nx = s_x;
  ny = s_y;
#endif
  return true;
}

}  // namespace deck

#endif  // BOARD_TOUCH_ST77922
