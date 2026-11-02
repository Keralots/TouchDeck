// AXS15231B touch (Guition JC3248W535): 8-byte command/response reads over
// I2C, woken by the INT line.
#include "board.h"
#if defined(BOARD_TOUCH_AXS15231B)
#include "deck_touch_int.h"
#include "deck_panel.h"
#include <Wire.h>
#include <driver/gpio.h>

namespace deck {
namespace {

constexpr int PIN_SDA = BOARD_TOUCH_SDA;
constexpr int PIN_SCL = BOARD_TOUCH_SCL;
constexpr int PIN_INT = BOARD_TOUCH_INT;
constexpr uint8_t ADDR = 0x3B;

TaskHandle_t s_task = nullptr;

void IRAM_ATTR intIsr(void*) {
  if (!s_task) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

}  // namespace

bool touchHwBegin(TaskHandle_t task) {
  s_task = task;
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.setTimeOut(5);  // bounded; Wire's default is 50 ms
  // Raw IDF handler: attachInterrupt() logs an error when the panel's TE
  // handler already installed the GPIO ISR service.
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_INT;
  g.mode = GPIO_MODE_INPUT;
  g.pull_up_en = GPIO_PULLUP_ENABLE;
  g.intr_type = GPIO_INTR_NEGEDGE;
  gpio_config(&g);
  const esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  gpio_isr_handler_add((gpio_num_t)PIN_INT, intIsr, nullptr);
  return true;
}

// One 8-byte read. Returns true with native coordinates for a valid contact.
bool touchHwRead(int& nx, int& ny) {
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  Wire.beginTransmission(ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return false;
  uint8_t d[8] = {0};
  const uint8_t got = Wire.requestFrom((int)ADDR, 8);
  for (int i = 0; i < 8 && Wire.available(); i++) d[i] = Wire.read();
  if (got < 8) return false;
  // Valid frame: 00 <points 1..2> E?xH xL 0?yH yL ... Idle/filler reads come
  // back as B0.. or FF.. and are ignored.
  if (d[0] != 0 || d[1] == 0 || d[1] > 2) return false;
  nx = ((d[2] & 0x0F) << 8) | d[3];
  ny = ((d[4] & 0x0F) << 8) | d[5];
  return nx < NATIVE_W && ny < NATIVE_H;
}

}  // namespace deck

#endif  // BOARD_TOUCH_AXS15231B
