// MIPI-DSI video-mode panel (ESP32-P4): the DPI driver scans one of two PSRAM
// framebuffers continuously; each frame is built whole into the other one and
// swapped in at the end of a refresh, so there is no tearing and no TE.
// Panel init commands, timing and pins come from the board header.
#include "board.h"
#if defined(BOARD_DISPLAY_MIPI_DSI)
#include "deck_panel_int.h"
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_timer.h>

namespace deck {
namespace {

struct InitCmd {
  uint8_t cmd;
  uint8_t data;
  uint8_t len;
  uint16_t delayMs;
};
constexpr InitCmd INIT_CMDS[] = {BOARD_DSI_INIT_CMDS};

esp_lcd_panel_handle_t s_dpi = nullptr;
uint16_t* s_fb[2] = {nullptr, nullptr};
int s_back = 1;  // the framebuffer not being scanned
SemaphoreHandle_t s_swapped = nullptr;
volatile bool s_swapPending = false;

// The DPI DMA picks the last presented framebuffer when it restarts at the
// end of a refresh, so the first refresh_done after a swap frees the old one.
bool IRAM_ATTR onRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  if (!s_swapPending) return false;
  s_swapPending = false;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_swapped, &woken);
  return woken == pdTRUE;
}

// Build f into the back framebuffer, present it and wait for the swap.
void presentFrame(const Frame& f, bool backToBack) {
  const int64_t f0 = esp_timer_get_time();
  frameStarted(f, f0, backToBack);
  uint16_t* dst = s_fb[s_back];
  frameBuild(f, dst, 0);
  const int64_t built = esp_timer_get_time();
  if (f.releaseLv) frameReleaseLv();
  // Writes the cache back and selects dst for the next refresh. The flag goes
  // up after it: a refresh ending in between only costs one frame.
  esp_lcd_panel_draw_bitmap(s_dpi, 0, 0, NATIVE_W, NATIVE_H, dst);
  s_swapPending = true;
  if (xSemaphoreTake(s_swapped, pdMS_TO_TICKS(50)) != pdTRUE) {
    s_swapPending = false;
    countMissedSync();
  }
  s_back ^= 1;
  const int64_t end = esp_timer_get_time();
  frameFinished(f, f0, (uint32_t)(built - f0), (uint32_t)(end - built), (uint32_t)(end - f0));
}

bool fail(const char* step, esp_err_t e) {
  Serial.printf("Deck panel: DSI %s failed: %s\n", step, esp_err_to_name(e));
  return false;
}

}  // namespace

bool transportBegin() {
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t lc = {};
  lc.chan_id = BOARD_DSI_LDO_CHAN;
  lc.voltage_mv = BOARD_DSI_LDO_MV;
  esp_err_t e = esp_ldo_acquire_channel(&lc, &ldo);
  if (e != ESP_OK) return fail("PHY power", e);

  pinMode(BOARD_LCD_RST, OUTPUT);
  digitalWrite(BOARD_LCD_RST, LOW);
  delay(10);
  digitalWrite(BOARD_LCD_RST, HIGH);
  delay(20);

  esp_lcd_dsi_bus_config_t bc = {};
  bc.bus_id = 0;
  bc.num_data_lanes = BOARD_DSI_LANES;
  bc.lane_bit_rate_mbps = BOARD_DSI_LANE_MBPS;  // phy_clk_src 0: the silicon-revision default
  esp_lcd_dsi_bus_handle_t bus = nullptr;
  if ((e = esp_lcd_new_dsi_bus(&bc, &bus)) != ESP_OK) return fail("bus", e);

  esp_lcd_dbi_io_config_t ioc = {};
  ioc.virtual_channel = 0;
  ioc.lcd_cmd_bits = 8;
  ioc.lcd_param_bits = 8;
  esp_lcd_panel_io_handle_t io = nullptr;
  if ((e = esp_lcd_new_panel_io_dbi(bus, &ioc, &io)) != ESP_OK) return fail("command io", e);

  esp_lcd_dpi_panel_config_t pc = {};
  pc.virtual_channel = 0;
  pc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  pc.dpi_clock_freq_mhz = BOARD_DPI_CLK_MHZ;
  pc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  pc.num_fbs = 2;
  pc.video_timing.h_size = NATIVE_W;
  pc.video_timing.v_size = NATIVE_H;
  pc.video_timing.hsync_pulse_width = BOARD_DPI_HSYNC;
  pc.video_timing.hsync_back_porch = BOARD_DPI_HBP;
  pc.video_timing.hsync_front_porch = BOARD_DPI_HFP;
  pc.video_timing.vsync_pulse_width = BOARD_DPI_VSYNC;
  pc.video_timing.vsync_back_porch = BOARD_DPI_VBP;
  pc.video_timing.vsync_front_porch = BOARD_DPI_VFP;
  if ((e = esp_lcd_new_panel_dpi(bus, &pc, &s_dpi)) != ESP_OK) return fail("DPI panel", e);

  for (const InitCmd& c : INIT_CMDS) {
    if ((e = esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? &c.data : nullptr, c.len)) != ESP_OK)
      return fail("init command", e);
    if (c.delayMs) delay(c.delayMs);
  }
  if ((e = esp_lcd_panel_init(s_dpi)) != ESP_OK) return fail("video start", e);

  // Both come zeroed (black); the driver scans framebuffer 0 first.
  void* fb0 = nullptr;
  void* fb1 = nullptr;
  if ((e = esp_lcd_dpi_panel_get_frame_buffer(s_dpi, 2, &fb0, &fb1)) != ESP_OK) return fail("framebuffers", e);
  s_fb[0] = (uint16_t*)fb0;
  s_fb[1] = (uint16_t*)fb1;
  s_back = 1;

  s_swapped = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_refresh_done = onRefreshDone;
  if ((e = esp_lcd_dpi_panel_register_event_callbacks(s_dpi, &cbs, nullptr)) != ESP_OK)
    return fail("callbacks", e);
  return true;
}

void transportTask(void*) {
  Frame cur = {};
  bool backToBack = false;
  for (;;) {
    if (quiesceRequested()) quiesceAck();  // nothing is held between frames
    if (!frameDecide(cur)) {
      frameIdleWait();
      backToBack = false;
      continue;
    }
    presentFrame(cur, backToBack);
    backToBack = true;
  }
}

}  // namespace deck

#endif  // BOARD_DISPLAY_MIPI_DSI
