// Guition JC3248W535: ESP32-S3-N16R8, 3.5" AXS15231B 320x480 over QSPI
// (native portrait) with its built-in touch, NS4168 I2S amplifier, microSD.
#ifndef BOARDS_JC3248W535_H
#define BOARDS_JC3248W535_H

#define BOARD_VARIANT "jc3248w535"
#define BOARD_NAME "Guition_JC3248W535"

// --- Display: AXS15231B over QSPI (deck_panel_qspi.cpp) ---
#define BOARD_DISPLAY_AXS15231B_QSPI 1
#define BOARD_PANEL_W 320
#define BOARD_PANEL_H 480
// User rotation r shows at native rotation (r + BOARD_ROT_BASE) & 3; user
// rotation stays "even = portrait" on every board.
#define BOARD_ROT_BASE 0
#define BOARD_LCD_CS 45
#define BOARD_LCD_SCK 47
#define BOARD_LCD_D0 21
#define BOARD_LCD_D1 48
#define BOARD_LCD_D2 40
#define BOARD_LCD_D3 39
#define BOARD_LCD_TE 38
#define BOARD_BACKLIGHT_PIN 1
#define BOARD_BACKLIGHT_ACTIVE_LOW 0
#define BOARD_BACKLIGHT_FREQ 1000  // analogWrite's default, as before

// --- Deck grid: limits and defaults for this screen size ---
#define BOARD_GRID_MAX 6
#define BOARD_GRID_LANDSCAPE_COLS 4
#define BOARD_GRID_LANDSCAPE_ROWS 3
#define BOARD_GRID_PORTRAIT_COLS 3
#define BOARD_GRID_PORTRAIT_ROWS 4
#define BOARD_GRID_PICK_LANDSCAPE_COLS 6  // portal grid picker limits
#define BOARD_GRID_PICK_LANDSCAPE_ROWS 4
#define BOARD_GRID_PICK_PORTRAIT_COLS 4
#define BOARD_GRID_PICK_PORTRAIT_ROWS 6
#define BOARD_MAX_PAGE_TILES 24
#define BOARD_MEDIA_ART_MAX 200  // Now playing cover side, px
#define BOARD_WALL_PRINTER_PAUSE 1  // printer pages leave too little PSRAM for a wallpaper change (b59f47b)

// --- Touch: AXS15231B controller (deck_touch_axs15231b.cpp) ---
#define BOARD_TOUCH_AXS15231B 1
#define BOARD_TOUCH_SDA 4
#define BOARD_TOUCH_SCL 8
#define BOARD_TOUCH_INT 3  // wired on this board (Phase 0), despite older docs

// --- Audio: NS4168 I2S amplifier, no codec, no MCLK (audio_hw_i2s_amp.cpp) ---
#define BOARD_AUDIO_I2S_AMP 1
#define BOARD_I2S_MCLK -1
#define BOARD_I2S_BCLK 42
#define BOARD_I2S_WS 2
#define BOARD_I2S_DOUT 41
#define BOARD_AUDIO_WAKE_MS 30      // speaker defaults (portal adjustable)
#define BOARD_AUDIO_KEEP_ON_MS 1500
#define BOARD_AUDIO_MAX_GAIN_PCT 100  // sample gain at 100 % volume

// --- microSD: 1-bit SD mode or SPI, same pins ---
#define BOARD_SD_SLOT 1
#define BOARD_SD_WIDTH 1
#define BOARD_SD_CLK 12
#define BOARD_SD_CMD 11
#define BOARD_SD_D0 13
#define BOARD_SD_D3 10  // SPI mode chip select
#define BOARD_SD_HAS_SPI 1

// Card pins, free for other uses while the card is off.
inline bool boardSdPin(uint8_t pin) { return pin >= 10 && pin <= 13; }

// Display, touch, audio, card (when used), USB, flash/PSRAM, boot strap,
// battery sense.
inline bool boardPinReserved(uint8_t pin, bool sdCard) {
  if (pin == 0) return true;                            // boot strap
  if (pin == BOARD_BACKLIGHT_PIN) return true;
  if (sdCard && boardSdPin(pin)) return true;
  if (pin == 21 || pin == 39 || pin == 40 ||
      pin == 45 || pin == 47 || pin == 48) return true; // display QSPI
  if (pin == 38) return true;                           // display TE
  if (pin == 4 || pin == 8) return true;                // touch I2C
  if (pin == 3) return true;                            // touch INT
  if (pin == 5) return true;                            // battery sense ADC
  if (pin == 2 || pin == 41 || pin == 42) return true;  // NS4168 I2S audio
  if (pin == 19 || pin == 20) return true;              // USB CDC D-/D+
  if (pin >= 26 && pin <= 37) return true;              // SPI flash + PSRAM (qio_opi)
  return false;
}

#endif  // BOARDS_JC3248W535_H
