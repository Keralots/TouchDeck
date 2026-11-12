// Goodix GT911 touch, polled over I2C (16-bit registers, big-endian address).
// Coordinates are taken as native panel pixels: the controller's configured
// resolution is logged at start to check that.
#include "board.h"
#if defined(BOARD_TOUCH_GT911)
#include "deck_touch_int.h"
#include "deck_panel.h"
#include <Wire.h>

namespace deck {
namespace {

constexpr uint16_t REG_CONFIG = 0x8047;      // config version, then x max, y max (LE)
constexpr uint16_t REG_PRODUCT_ID = 0x8140;  // 4 ASCII bytes, "911"
constexpr uint16_t REG_STATUS = 0x814E;      // bit 7 = new data, bits 0-3 = contacts
constexpr uint16_t REG_POINT1 = 0x8150;      // x (LE), y (LE), size, ...

uint8_t s_addr = 0;
// The status register only flags NEW samples; between them the last contact
// still stands.
bool s_down = false;
int s_x = 0, s_y = 0;

bool readReg(uint16_t reg, uint8_t* d, size_t n) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)s_addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

bool writeReg(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(s_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool touchHwBegin(TaskHandle_t) {
  Wire.begin(BOARD_TOUCH_SDA, BOARD_TOUCH_SCL, 400000);
  Wire.setTimeOut(5);  // bounded; Wire's default is 50 ms
  // The address is latched from INT at reset: 0x5D or 0x14. Nothing drives
  // INT here, so try both.
  for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      s_addr = a;
      break;
    }
  }
  if (!s_addr) {
    Serial.println("Touch: GT911 not found at 0x5D or 0x14");
    return false;
  }
  char id[5] = {};
  uint8_t cfg[5] = {};
  readReg(REG_PRODUCT_ID, (uint8_t*)id, 4);
  readReg(REG_CONFIG, cfg, 5);
  Serial.printf("Touch: GT911 at 0x%02X, id \"%s\", config v%u, %ux%u\n", s_addr, id, cfg[0],
                cfg[1] | cfg[2] << 8, cfg[3] | cfg[4] << 8);
  return false;
}

bool touchHwRead(int& nx, int& ny) {
  if (!s_addr) return false;
  uint8_t st;
  if (!readReg(REG_STATUS, &st, 1)) return false;
  if (st & 0x80) {
    const uint8_t n = st & 0x0F;
    uint8_t p[4];
    s_down = n > 0 && n <= 5 && readReg(REG_POINT1, p, 4);
    if (s_down) {
      s_x = p[0] | p[1] << 8;
      s_y = p[2] | p[3] << 8;
    }
    writeReg(REG_STATUS, 0);  // hand the buffer back to the controller
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

#endif  // BOARD_TOUCH_GT911
