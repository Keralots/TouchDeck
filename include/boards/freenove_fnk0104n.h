// Freenove ESP32-S3 3.5" Display (FNK0104N): ESP32-S3 N16R8, 3.5" 320x480
// IPS with an ST77922 TDDI panel over QSPI (touch lives in the same chip and
// answers on I2C), ES8311 codec with speaker and microphone, microSD.
//
// Pins from Freenove's own examples (Freenove_ESP32_S3_Display,
// Tutorial_With_Touch/Sketches/Sketch_18.1_Lvgl_Multifunctionality/public.h,
// Libraries/FNK0104N/TFT_eSPI_v2.5.43/ST77922.h). The QSPI transport runs the
// panel at 80 MHz, as the vendor does.
#ifndef BOARDS_FREENOVE_FNK0104N_H
#define BOARDS_FREENOVE_FNK0104N_H

#define BOARD_VARIANT "freenove_fnk0104n"
#define BOARD_NAME "Freenove_FNK0104N"

// --- Display: ST77922 TDDI over QSPI (deck_panel_qspi.cpp) ---
#define BOARD_DISPLAY_ST77922_QSPI 1
#define BOARD_PANEL_W 320
#define BOARD_PANEL_H 480
#define BOARD_ROT_BASE 0  // native portrait, like the Guition
#define BOARD_LCD_CS 10
#define BOARD_LCD_SCK 12
#define BOARD_LCD_D0 11
#define BOARD_LCD_D1 13
#define BOARD_LCD_D2 14
#define BOARD_LCD_D3 9
#define BOARD_LCD_TE -1  // not wired: the transport paces on a 60 Hz timer
#define BOARD_BACKLIGHT_PIN 41
#define BOARD_BACKLIGHT_ACTIVE_LOW 0
#define BOARD_BACKLIGHT_FREQ 5000

// --- Deck grid: same cell size as the Guition (same panel size) ---
#define BOARD_GRID_MAX 6
#define BOARD_GRID_LANDSCAPE_COLS 4
#define BOARD_GRID_LANDSCAPE_ROWS 3
#define BOARD_GRID_PORTRAIT_COLS 3
#define BOARD_GRID_PORTRAIT_ROWS 4
#define BOARD_GRID_PICK_LANDSCAPE_COLS 6
#define BOARD_GRID_PICK_LANDSCAPE_ROWS 4
#define BOARD_GRID_PICK_PORTRAIT_COLS 4
#define BOARD_GRID_PICK_PORTRAIT_ROWS 6
#define BOARD_MAX_PAGE_TILES 24
#define BOARD_MEDIA_ART_MAX 200
#define BOARD_WALL_PRINTER_PAUSE 1  // same 8 MB PSRAM as the Guition

// --- Touch: ST77922 TDDI over I2C, polled (deck_touch_st77922.cpp) ---
// 16-bit register addressing, address 0x55. INT and RST are wired on the board
// but the draft polls: the controller only flags NEW samples, so a still finger
// is reported from the cached contact.
#define BOARD_TOUCH_ST77922 1
#define BOARD_TOUCH_SDA 38
#define BOARD_TOUCH_SCL 39
#define BOARD_TOUCH_INT 47
#define BOARD_TOUCH_RST 48

// --- Audio: ES8311 codec on the same I2C bus, amp enable on GPIO 1 (low) ---
#define BOARD_I2C_SDA 38
#define BOARD_I2C_SCL 39
#define BOARD_AUDIO_ES8311 1
#define BOARD_CODEC_ADDR 0x18
#define BOARD_I2S_MCLK 17
#define BOARD_I2S_BCLK 18
#define BOARD_I2S_WS 21
#define BOARD_I2S_DOUT 15
#define BOARD_I2S_DIN 16
#define BOARD_AMP_EN 1
#define BOARD_AMP_EN_ACTIVE_LOW 1
#define BOARD_AUDIO_WAKE_MS 150
#define BOARD_AUDIO_KEEP_ON_MS 30000
#define BOARD_AUDIO_MAX_GAIN_PCT 100

// --- microSD: 4-bit SD on slot 0, no SPI fallback ---
#define BOARD_SD_SLOT 0
#define BOARD_SD_WIDTH 4
#define BOARD_SD_CLK 5
#define BOARD_SD_CMD 4
#define BOARD_SD_D0 6
#define BOARD_SD_D1 7
#define BOARD_SD_D2 2
#define BOARD_SD_D3 3
#define BOARD_SD_HAS_SPI 0

// Card pins, free for other uses while the card is off.
inline bool boardSdPin(uint8_t pin) { return pin >= 2 && pin <= 7; }

// Display QSPI, touch I2C, audio, card (when used), USB, flash/PSRAM, boot
// strap, amp enable and the on-board WS2812 (GPIO 40).
inline bool boardPinReserved(uint8_t pin, bool sdCard) {
  if (pin == 0) return true;                            // boot strap
  if (pin == BOARD_BACKLIGHT_PIN) return true;
  if (pin == BOARD_AMP_EN) return true;                 // speaker amp enable
  if (pin == 40) return true;                           // on-board WS2812
  if (sdCard && boardSdPin(pin)) return true;
  if (pin == BOARD_LCD_CS || pin == BOARD_LCD_SCK || pin == BOARD_LCD_D0 ||
      pin == BOARD_LCD_D1 || pin == BOARD_LCD_D2 || pin == BOARD_LCD_D3)
    return true;                                        // display QSPI
  if (pin == 38 || pin == 39) return true;              // touch + codec I2C
  if (pin == 47 || pin == 48) return true;              // touch INT / RST
  if (pin == 15 || pin == 16 || pin == 17 || pin == 18 || pin == 21)
    return true;                                        // ES8311 I2S
  if (pin == 19 || pin == 20) return true;              // USB CDC D-/D+
  if (pin >= 26 && pin <= 37) return true;              // SPI flash + PSRAM
  return false;
}

#endif  // BOARDS_FREENOVE_FNK0104N_H
