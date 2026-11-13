// Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: ESP32-P4 + ESP32-C6 (WiFi over
// ESP-Hosted SDIO), 7" EK79007 1024x600 MIPI-DSI, GT911 touch, ES8311 codec
// with amplifier, ES7210 mics, microSD, RS485, CAN.
// Pins from the vendor examples (github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B)
// and the schematic; none probed on hardware yet.
#ifndef BOARDS_WS_P4_7B_H
#define BOARDS_WS_P4_7B_H

#define BOARD_VARIANT "ws_p4_7b"
#define BOARD_NAME "Waveshare_ESP32-P4-WIFI6-Touch-LCD-7B"

// --- WiFi: ESP32-C6 over ESP-Hosted SDIO (CLK 18, CMD 19, D0-D3 14-17,
// reset 54), the pins Arduino's esp32p4 variant already uses ---
#define BOARD_WIFI_ESP_HOSTED 1

// --- Display: MIPI-DSI video mode (deck_panel_dsi.cpp) ---
#define BOARD_DISPLAY_MIPI_DSI 1
#define BOARD_PANEL_W 1024
#define BOARD_PANEL_H 600
// Native scan is landscape: user rotation 1 (landscape) is native rotation 0.
#define BOARD_ROT_BASE 3
#define BOARD_LCD_RST 33
#define BOARD_DSI_LDO_CHAN 3    // LDO_VO3 feeds the DSI PHY
#define BOARD_DSI_LDO_MV 2500
#define BOARD_DSI_LANES 2
#define BOARD_DSI_LANE_MBPS 1000
#define BOARD_DPI_CLK_MHZ 52    // ~60 Hz with the timing below
#define BOARD_DPI_HSYNC 10
#define BOARD_DPI_HBP 160
#define BOARD_DPI_HFP 160
#define BOARD_DPI_VSYNC 1
#define BOARD_DPI_VBP 23
#define BOARD_DPI_VFP 12
// EK79007 init from the vendor BSP: 2-lane pad control, vendor registers,
// sleep out. {cmd, data, data length, delay ms after}.
// MADCTL 0x36 = 0x02 (default 0x01): scan turned by 180 in the panel itself,
// so landscape stays on the row-copy path; touch follows via BOARD_PANEL_FLIP.
#define BOARD_PANEL_FLIP 1
#define BOARD_DSI_INIT_CMDS                                                    \
  {0xB2, 0x10, 1, 0}, {0x80, 0x8B, 1, 0}, {0x81, 0x78, 1, 0},                 \
  {0x82, 0x84, 1, 0}, {0x83, 0x88, 1, 0}, {0x84, 0xA8, 1, 0},                 \
  {0x85, 0xE3, 1, 0}, {0x86, 0x88, 1, 0}, {0x36, 0x02, 1, 0},                 \
  {0x11, 0x00, 0, 120}
#define BOARD_BACKLIGHT_PIN 32
#define BOARD_BACKLIGHT_ACTIVE_LOW 1
#define BOARD_BACKLIGHT_FREQ 5000  // vendor BSP
#define BOARD_BACKLIGHT_HOLD_ON_RESTART 1  // pin floats on during reset: white flash

// --- Deck grid: about the JC3248W535's cell size (same pixel density) ---
#define BOARD_GRID_MAX 10
#define BOARD_GRID_LANDSCAPE_COLS 8
#define BOARD_GRID_LANDSCAPE_ROWS 6
#define BOARD_GRID_PORTRAIT_COLS 5
#define BOARD_GRID_PORTRAIT_ROWS 9
#define BOARD_GRID_PICK_LANDSCAPE_COLS 8  // portal grid picker limits (cell >= ~100 px)
#define BOARD_GRID_PICK_LANDSCAPE_ROWS 6
#define BOARD_GRID_PICK_PORTRAIT_COLS 6
#define BOARD_GRID_PICK_PORTRAIT_ROWS 9
#define BOARD_MAX_PAGE_TILES 48
#define BOARD_MEDIA_ART_MAX 400  // Now playing cover side, px
#define BOARD_WALL_PRINTER_PAUSE 0  // ~12 MB PSRAM free with printer pages up: rotation keeps going

// --- Touch: GT911 on the shared I2C bus, polled (deck_touch_gt911.cpp) ---
// The vendor Arduino code uses neither INT nor RST.
#define BOARD_I2C_SDA 7         // shared by touch and codec
#define BOARD_I2C_SCL 8
#define BOARD_TOUCH_GT911 1
#define BOARD_TOUCH_SDA BOARD_I2C_SDA
#define BOARD_TOUCH_SCL BOARD_I2C_SCL
#define BOARD_TOUCH_INT -1

// --- Audio: ES8311 codec on the same I2C bus (audio_hw_es8311.cpp) ---
#define BOARD_AUDIO_ES8311 1
#define BOARD_CODEC_ADDR 0x18
#define BOARD_I2S_MCLK 13
#define BOARD_I2S_BCLK 12
#define BOARD_I2S_WS 10
#define BOARD_I2S_DOUT 9
#define BOARD_I2S_DIN 11        // ES7210 mics, unused
#define BOARD_AMP_EN 53         // speaker amplifier, high = on
// Speaker defaults (portal adjustable). By ear: 120 ms of wake-up drops the
// first click, 200 ms does not; kept on 30 s with no audible hiss.
#define BOARD_AUDIO_WAKE_MS 200
#define BOARD_AUDIO_KEEP_ON_MS 30000
#define BOARD_AUDIO_MAX_GAIN_PCT 70  // 100 % in settings; full scale crackles on the kit speaker

// --- microSD: 4-bit SD on slot 0 (IO MUX pins), card powered from LDO_VO4 ---
// Power handling as Arduino's SD_MMC does for the P4 variant.
#define BOARD_SD_SLOT 0
#define BOARD_SD_WIDTH 4
#define BOARD_SD_CLK 43
#define BOARD_SD_CMD 44
#define BOARD_SD_D0 39
#define BOARD_SD_D1 40
#define BOARD_SD_D2 41
#define BOARD_SD_D3 42
#define BOARD_SD_LDO_CHAN 4
#define BOARD_SD_POWER_PIN 45
#define BOARD_SD_POWER_ON_LEVEL 0
#define BOARD_SD_HAS_SPI 0

// The card has dedicated pins (always reserved below).
inline bool boardSdPin(uint8_t) { return false; }

// Display, I2C, audio, card, C6 link, USB, UART, RS485, CAN, boot straps.
// The 7B header pins themselves are not mapped yet.
inline bool boardPinReserved(uint8_t pin, bool) {
  if (pin == BOARD_LCD_RST || pin == BOARD_BACKLIGHT_PIN) return true;
  if (pin == 7 || pin == 8) return true;               // touch + codec I2C
  if (pin >= 9 && pin <= 13) return true;              // I2S
  if (pin == BOARD_AMP_EN) return true;
  if (pin >= 39 && pin <= 45) return true;             // microSD + its power
  if (pin >= 14 && pin <= 19) return true;             // ESP32-C6 SDIO
  if (pin == 54) return true;                          // ESP32-C6 reset
  if (pin == 24 || pin == 25) return true;             // USB serial/JTAG
  if (pin == 37 || pin == 38) return true;             // UART0 (USB bridge)
  if (pin == 26 || pin == 27) return true;             // RS485
  if (pin == 21 || pin == 22) return true;             // CAN
  if (pin >= 34 && pin <= 36) return true;             // boot straps
  return false;
}

#endif  // BOARDS_WS_P4_7B_H
