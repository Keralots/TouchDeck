// MIPI-DSI video-mode panel (ESP32-P4): the DPI driver scans one of three
// PSRAM framebuffers continuously. A frame is built into a free one and
// swapped in at the end of a refresh (no tearing, no TE); the next frame is
// built while that swap is still pending. PSRAM moves a whole frame in ~17 ms,
// so plain LVGL frames copy only what changed since the buffer was last used.
// Panel init commands, timing and pins come from the board header.
#include "board.h"
#if defined(BOARD_DISPLAY_MIPI_DSI)
#include "deck_panel_int.h"
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <esp_timer.h>
#include <esp32p4/rom/cache.h>
#include <soc/cache_reg.h>
#include <soc/soc.h>

namespace deck {
namespace {

struct InitCmd {
  uint8_t cmd;
  uint8_t data;
  uint8_t len;
  uint16_t delayMs;
};
constexpr InitCmd INIT_CMDS[] = {BOARD_DSI_INIT_CMDS};

constexpr int NFB = 3;
constexpr int64_t PERIOD_US = (int64_t)(NATIVE_W + BOARD_DPI_HSYNC + BOARD_DPI_HBP + BOARD_DPI_HFP) *
                              (NATIVE_H + BOARD_DPI_VSYNC + BOARD_DPI_VBP + BOARD_DPI_VFP) / BOARD_DPI_CLK_MHZ;
// No swap is asked for this close before a refresh ends: the scan DMA has
// started on its next framebuffer by the time refresh_done fires.
constexpr int64_t SWAP_GUARD_US = 1500;

esp_lcd_panel_handle_t s_dpi = nullptr;
uint16_t* s_fb[NFB] = {};
DirtyMask s_stale[NFB];  // what each framebuffer lacks of the latest LVGL frame
int s_glass = 0;         // being scanned
int s_pending = -1;      // presented, scanned from the next refresh on
SemaphoreHandle_t s_swapped = nullptr;
volatile bool s_swapPending = false;
portMUX_TYPE s_syncMux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_vsyncUs = 0;   // last refresh end (s_syncMux)
int64_t s_shownUs = 0;   // refresh that shows the last presented frame
uint32_t s_buildUs = 15000;  // composed frame build time, smoothed

// The DPI DMA picks the last presented framebuffer when it restarts at the
// end of a refresh, so the first refresh_done after a swap frees the old one.
bool IRAM_ATTR onRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&s_syncMux);
  s_vsyncUs = now;
  portEXIT_CRITICAL_ISR(&s_syncMux);
  if (!s_swapPending) return false;
  s_swapPending = false;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_swapped, &woken);
  return woken == pdTRUE;
}

int64_t lastVsyncUs() {
  portENTER_CRITICAL(&s_syncMux);
  const int64_t v = s_vsyncUs;
  portEXIT_CRITICAL(&s_syncMux);
  return v;
}

// The first refresh end after t.
int64_t vsyncAfter(int64_t t) {
  const int64_t v = lastVsyncUs();
  return t <= v ? v : v + (t - v + PERIOD_US - 1) / PERIOD_US * PERIOD_US;
}

void waitRefresh() {
  s_swapPending = true;
  if (xSemaphoreTake(s_swapped, pdMS_TO_TICKS(50)) != pdTRUE) {
    s_swapPending = false;
    countMissedSync();
  }
}

// The presented framebuffer is on the glass: the one before it is free.
void settle() {
  if (s_pending < 0) return;
  if (xSemaphoreTake(s_swapped, pdMS_TO_TICKS(50)) != pdTRUE) {
    s_swapPending = false;
    countMissedSync();
  }
  s_glass = s_pending;
  s_pending = -1;
}

// L2 cache prefetch over the PSRAM window: sequential reads get ~1.75x faster
// (9.4 -> 5.3 ms per frame). ROM calls, IDF has no API for it.
void prefetchBegin() {
  struct l1_dcache_l2_autoload_config ac = {};
  ac.order = CACHE_AUTOLOAD_POSITIVE;
  ac.trigger = CACHE_AUTOLOAD_MISS_TRIGGER;
  ac.ena0 = 1;
  ac.addr0 = SOC_EXTRAM_LOW;
  ac.size0 = SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW;
  Cache_Config_L2_Cache_Autoload(&ac);
  Cache_Enable_L2_Cache_Autoload();
}

// Every flash write resumes the cache with its boot setting: prefetch off.
inline void prefetchKeep() {
  if (!REG_GET_BIT(CACHE_L2_CACHE_AUTOLOAD_CTRL_REG, CACHE_L2_CACHE_AUTOLOAD_ENA)) Cache_Enable_L2_Cache_Autoload();
}

// A framebuffer to build into; for a plain frame the one that needs least.
int pickBack(bool plain) {
  for (;;) {
    int best = -1;
    for (int k = 0; k < NFB; k++) {
      if (k == s_glass || k == s_pending) continue;
      if (best < 0 || (s_stale[best].full && !s_stale[k].full)) best = k;
    }
    if (!plain || !s_stale[best].full || s_pending < 0) return best;
    settle();  // the one leaving the glass may need less than a whole copy
  }
}

// Build f into a free framebuffer and present it. Returns with the swap
// pending: the next build runs while this frame waits for its refresh.
void presentFrame(const Frame& f, bool backToBack) {
  const int64_t f0 = esp_timer_get_time();
  frameStarted(f, f0, backToBack);
  prefetchKeep();
  const bool plain = !f.comp;
  if (plain)
    for (DirtyMask& m : s_stale) m.add(f.dirty);
  const int x = pickBack(plain);
  uint16_t* dst = s_fb[x];
  if (!plain || !frameBuildDirty(f, dst, s_stale[x])) frameBuild(f, dst, 0);
  if (plain) s_stale[x].clear();
  else s_stale[x].full = true;
  const int64_t built = esp_timer_get_time();
  if (!plain) s_buildUs = (s_buildUs * 3 + (uint32_t)(built - f0)) / 4;
  if (f.releaseLv) frameReleaseLv();
  settle();
  int64_t now = esp_timer_get_time();
  const int64_t v = lastVsyncUs();
  if (v && now - v < 3 * PERIOD_US && (now - v) % PERIOD_US > PERIOD_US - SWAP_GUARD_US) {
    waitRefresh();
    now = esp_timer_get_time();
  }
  // Writes the cache back and selects dst for the next refresh. The flag goes
  // up after it: a refresh ending in between only costs one frame.
  esp_lcd_panel_draw_bitmap(s_dpi, 0, 0, NATIVE_W, NATIVE_H, dst);
  s_swapPending = true;
  s_pending = x;
  s_shownUs = vsyncAfter(now);
  const int64_t end = esp_timer_get_time();
  frameFinished(f, f0, (uint32_t)(built - f0), (uint32_t)(end - built), (uint32_t)(end - f0));
}

bool fail(const char* step, esp_err_t e) {
  Serial.printf("Deck panel: DSI %s failed: %s\n", step, esp_err_to_name(e));
  return false;
}

}  // namespace

int64_t transportLastSyncUs() { return 0; }

// The refresh after the build, and never the one the frame before it takes.
int64_t transportPresentUs(int64_t nowUs) {
  if (!lastVsyncUs()) return 0;
  const int64_t t = vsyncAfter(nowUs + s_buildUs + SWAP_GUARD_US);
  return t > s_shownUs ? t : s_shownUs + PERIOD_US;
}

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
  pc.num_fbs = NFB;
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
  void* fb[NFB] = {};
  if ((e = esp_lcd_dpi_panel_get_frame_buffer(s_dpi, NFB, &fb[0], &fb[1], &fb[2])) != ESP_OK)
    return fail("framebuffers", e);
  for (int k = 0; k < NFB; k++) {
    s_fb[k] = (uint16_t*)fb[k];
    s_stale[k].clear();
    s_stale[k].full = true;
  }

  prefetchBegin();
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
      settle();
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
