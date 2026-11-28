// QSPI panels with the DCS-over-QSPI framing: the Guition JC3248W535
// (AXS15231B) and the Freenove FNK0104N (ST77922). spi_master driven
// directly, CS held by hand across a frame, chunks staged through two internal
// DMA bounce buffers, every frame started on the panel's sync edge (TE, or a
// 60 Hz timer on boards that do not wire TE).
//
// Facts this rests on (measured in the Phase 0 spike on the Guition, see the
// deck plan): the panel only accepts full native 320x480 frames (no MADCTL
// rotation, the CASET start column is ignored), 40 MHz is the clock ceiling
// there, a full frame is 15.4 ms on the wire against a 16.39 ms TE period, and
// the write is slower than the scan, so a frame must start within ~1 ms of the
// TE rising edge. The ST77922 runs at 80 MHz in Freenove's own driver, so a
// frame there is ~7.7 ms; its command framing differs (see txCmd).
#include "board.h"
#if defined(BOARD_DISPLAY_AXS15231B_QSPI) || defined(BOARD_DISPLAY_ST77922_QSPI)
#include "deck_panel_int.h"
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#if defined(BOARD_DISPLAY_AXS15231B_QSPI)
// Only for axs15231b_180640_init_operations and its op-code enum; the
// library's databus and panel classes are not used.
#include <Arduino_GFX_Library.h>
#endif

namespace deck {
namespace {

constexpr int PIN_CS = BOARD_LCD_CS;
constexpr int PIN_SCK = BOARD_LCD_SCK;
constexpr int PIN_D0 = BOARD_LCD_D0;
constexpr int PIN_D1 = BOARD_LCD_D1;
constexpr int PIN_D2 = BOARD_LCD_D2;
constexpr int PIN_D3 = BOARD_LCD_D3;
#if BOARD_LCD_TE >= 0
constexpr int PIN_TE = BOARD_LCD_TE;
#endif

// Two staging buffers only: the next frame's first chunk is pre-built into
// whichever one frees first.
constexpr size_t CHUNK_BYTES = (size_t)NATIVE_W * PANEL_ROWS * 2;
constexpr int NCHUNK = NATIVE_H / PANEL_ROWS;
static_assert(NCHUNK % 2 == 0, "the last chunk must use s_bounce[1]");
#if defined(BOARD_DISPLAY_ST77922_QSPI)
constexpr uint32_t SPI_HZ = 80000000;  // Freenove runs this panel at 80 MHz
// No TE line: the sync edge is a timer, so there is no scan to overtake and a
// frame may start later than the edge without losing the bottom rows.
constexpr int64_t TE_LATE_US = 4000;
#else
constexpr uint32_t SPI_HZ = 40000000;  // 80 MHz corrupts the image on this panel
// The write (15.4 ms) is slower than the scan, so a frame that starts later
// than this after the TE rising edge is overtaken near the bottom.
constexpr int64_t TE_LATE_US = 900;
#endif

spi_device_handle_t s_dev = nullptr;
uint16_t* s_bounce[2] = {nullptr, nullptr};
DMA_ATTR uint8_t s_cmdbuf[64];
spi_transaction_ext_t s_tr[NCHUNK];

// --- TE ----------------------------------------------------------------------
portMUX_TYPE s_teMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_teSeq = 0;
volatile int64_t s_teUs = 0;
SemaphoreHandle_t s_teSem = nullptr;
uint32_t s_teUsed = 0;

#if BOARD_LCD_TE >= 0
void IRAM_ATTR teIsr(void*) {
  portENTER_CRITICAL_ISR(&s_teMux);
  s_teUs = esp_timer_get_time();
  s_teSeq = s_teSeq + 1;
  portEXIT_CRITICAL_ISR(&s_teMux);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_teSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}
#else
// No TE pin on this board: a periodic timer plays the vertical blank, so the
// compositor's frame clock and pacing stay on a steady ~60 Hz grid. Frames may
// tear (there is no scan to lock to); a faster bus keeps the window small.
esp_timer_handle_t s_teTimer = nullptr;

void softTeTick(void*) {
  portENTER_CRITICAL(&s_teMux);
  s_teUs = esp_timer_get_time();
  s_teSeq = s_teSeq + 1;
  portEXIT_CRITICAL(&s_teMux);
  xSemaphoreGive(s_teSem);
}
#endif

inline void csLow() { gpio_set_level((gpio_num_t)PIN_CS, 0); }
inline void csHigh() { gpio_set_level((gpio_num_t)PIN_CS, 1); }

// Command with optional parameters on one data line, polling. Only the flush
// task (or panelBegin before it exists) may call this: polling transfers are
// not allowed while queued transactions are pending.
void txCmd(uint8_t c, const uint8_t* d, size_t n) {
#if defined(BOARD_DISPLAY_ST77922_QSPI)
  // Freenove's framing for this panel: command 0x02, an 8-bit command and a
  // 24-bit address on one data line, then the parameters on one line. The
  // VARIABLE_* flags take the phase lengths from the extended transaction.
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
  t.command_bits = 8;
  t.address_bits = 24;
  t.base.cmd = 0x02;
  t.base.addr = ((uint32_t)c) << 8;
  if (n > 0 && n <= 4) {
    t.base.flags |= SPI_TRANS_USE_TXDATA;
    memcpy(t.base.tx_data, d, n);
  } else if (n > 0) {
    memcpy(s_cmdbuf, d, n);
    t.base.tx_buffer = s_cmdbuf;
  }
  t.base.length = n * 8;
  csLow();
  spi_device_polling_transmit(s_dev, (spi_transaction_t*)&t);
  csHigh();
#else
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.cmd = 0x02;
  t.addr = ((uint32_t)c) << 8;
  if (n > 0 && n <= 4) {
    t.flags |= SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, d, n);
  } else if (n > 0) {
    memcpy(s_cmdbuf, d, n);
    t.tx_buffer = s_cmdbuf;
  }
  t.length = n * 8;
  csLow();
  spi_device_polling_transmit(s_dev, &t);
  csHigh();
#endif
}

#if defined(BOARD_DISPLAY_ST77922_QSPI)
// Freenove's ST77922 init (Libraries/FNK0104N/TFT_eSPI_v2.5.43/ST77922.cpp),
// flattened: 0xFF <cmd> <len> <data...> writes a command, 0xFE <ms> waits.
// COLMOD 0x3A = 0x01 is RGB565 on this TDDI; INVON (0x21) is on, as there.
static const uint8_t kSt77922Init[] = {
    0xFF, 0xF1, 1, 0x00,
    0xFF, 0x60, 3, 0x00, 0x00, 0x00,
    0xFF, 0x65, 1, 0x80,
    0xFF, 0x79, 1, 0x06,
    0xFF, 0x7B, 3, 0x00, 0x08, 0x08,
    0xFF, 0x80, 11, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
    0xFF, 0x81, 4, 0x26, 0x52, 0x72, 0x27,
    0xFF, 0x84, 2, 0x92, 0x25,
    0xFF, 0x87, 6, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
    0xFF, 0x88, 15, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06,
    0xFF, 0x89, 3, 0x00, 0x00, 0x00,
    0xFF, 0x8A, 11, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
    0xFF, 0x8B, 9, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
    0xFF, 0x8C, 13, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09,
    0xFF, 0x8D, 1, 0x0C,
    0xFF, 0x8E, 6, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
    0xFF, 0xB3, 2, 0x00, 0x30,
    0xFF, 0xF1, 1, 0x00,
    0xFF, 0x71, 1, 0xD0,
    0xFF, 0x66, 2, 0x02, 0x3F,
    0xFF, 0xBE, 3, 0x26, 0x00, 0x9D,
    0xFF, 0x70, 12, 0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A,
    0xFF, 0x90, 9, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27,
    0xFF, 0x91, 9, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27,
    0xFF, 0x92, 10, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27,
    0xFF, 0x93, 10, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27,
    0xFF, 0x94, 6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x95, 5, 0x96, 0x16, 0x00, 0x00, 0xFF,
    0xFF, 0x96, 12, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44,
    0xFF, 0x97, 12, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44,
    0xFF, 0xBA, 5, 0x55, 0x94, 0x2D, 0x94, 0x27,
    0xFF, 0x9A, 7, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x9B, 7, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x9C, 13, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00,
    0xFF, 0x9D, 8, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0xFF, 0x9E, 7, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0xFF, 0xB4, 12, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10,
    0xFF, 0xB5, 12, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12,
    0xFF, 0xB6, 7, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
    0xFF, 0x86, 14, 0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5,
    0xFF, 0xB7, 16, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D,
    0xFF, 0xB8, 16, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D,
    0xFF, 0xB9, 2, 0x23, 0x23,
    0xFF, 0xBF, 6, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
    0xFF, 0xF2, 1, 0x00,
    0xFF, 0x73, 5, 0x04, 0xDA, 0x12, 0x54, 0x47,
    0xFF, 0x77, 5, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
    0xFF, 0x7A, 2, 0x15, 0x27,
    0xFF, 0x7B, 2, 0x04, 0x57,
    0xFF, 0x7E, 2, 0x01, 0x0E,
    0xFF, 0xBF, 1, 0x36,
    0xFF, 0xE3, 2, 0x40, 0x40,
    0xFF, 0xF0, 1, 0x00,
    0xFF, 0xD0, 1, 0x00,
    0xFF, 0x2A, 4, 0x00, 0x00, 0x01, 0x3F,
    0xFF, 0x2B, 4, 0x00, 0x00, 0x01, 0xDF,
    0xFF, 0x21, 0,
    0xFF, 0x11, 0,
    0xFE, 120,
    0xFF, 0x29, 0,
    0xFF, 0x2C, 0,
    0xFF, 0x3A, 1, 0x01,
    0xFF, 0x36, 1, 0x00,
    0xFF, 0x35, 1, 0x01,
    0xFE, 20,
};
#endif

void panelInitSequence() {
#if defined(BOARD_DISPLAY_ST77922_QSPI)
  for (size_t i = 0; i + 1 < sizeof(kSt77922Init);) {
    const uint8_t op = kSt77922Init[i++];
    if (op == 0xFE) {
      delay(kSt77922Init[i++]);
      continue;
    }
    const uint8_t c = kSt77922Init[i++];
    const uint8_t n = kSt77922Init[i++];
    txCmd(c, n ? &kSt77922Init[i] : nullptr, n);
    i += n;
  }
#else
  txCmd(0x01, nullptr, 0);  // SWRESET
  delay(200);
  const uint8_t* op = axs15231b_180640_init_operations;
  const size_t len = sizeof(axs15231b_180640_init_operations);
  for (size_t i = 0; i < len; ++i) {
    switch (op[i]) {
      case BEGIN_WRITE:
      case END_WRITE:
        break;
      case WRITE_COMMAND_8:
        txCmd(op[++i], nullptr, 0);
        break;
      case WRITE_C8_D8: {
        uint8_t c = op[++i];
        txCmd(c, &op[i + 1], 1);
        i += 1;
        break;
      }
      case WRITE_C8_D16: {
        uint8_t c = op[++i];
        txCmd(c, &op[i + 1], 2);
        i += 2;
        break;
      }
      case WRITE_C8_BYTES: {
        uint8_t c = op[++i];
        uint8_t l = op[++i];
        txCmd(c, &op[i + 1], l);
        i += l;
        break;
      }
      case DELAY:
        delay(op[++i]);
        break;
      default:
        Serial.printf("Deck panel: unknown init op %u at %u\n", op[i], (unsigned)i);
        break;
    }
  }
  uint8_t v;
  txCmd(0x20, nullptr, 0);         // INVOFF
  v = 0x00; txCmd(0x36, &v, 1);    // MADCTL: rotation is done in software
  v = 0x05; txCmd(0x3A, &v, 1);    // COLMOD RGB565
  v = 0x00; txCmd(0x35, &v, 1);    // TEON, V-blank only
#endif
}

// Full-width window. On the AXS panel RASET is deliberately not sent (QSPI
// quirk); RAMWR restarts the frame at the top-left native pixel. The ST77922
// needs both ranges, and its pixel stream carries the RAMWRC command itself.
void setWindow() {
  const uint8_t ca[4] = {0, 0, (uint8_t)((NATIVE_W - 1) >> 8),
                         (uint8_t)((NATIVE_W - 1) & 0xFF)};
  txCmd(0x2A, ca, 4);
#if defined(BOARD_DISPLAY_ST77922_QSPI)
  const uint8_t ra[4] = {0, 0, (uint8_t)((NATIVE_H - 1) >> 8),
                         (uint8_t)((NATIVE_H - 1) & 0xFF)};
  txCmd(0x2B, ra, 4);
#else
  txCmd(0x2C, nullptr, 0);
#endif
}

void fillTr(spi_transaction_ext_t& t, bool first, const void* buf, size_t len) {
  memset(&t, 0, sizeof(t));
  t.base.flags = SPI_TRANS_MODE_QIO;
  if (first) {
    t.base.cmd = 0x32;
    t.base.addr = 0x003C00;  // RAMWRC pixel stream, as Arduino_GFX does
  } else {
    t.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                    SPI_TRANS_VARIABLE_DUMMY;
  }
  t.base.tx_buffer = buf;
  t.base.length = len * 8;
}

// Wait for a TE rising edge that is fresh enough to start a frame on.
// `backToBack` = this frame follows the previous one without a pause, so a
// missed edge is a real dropped frame rather than an idle panel.
void waitTe(bool backToBack) {
  for (int tries = 0; tries < 4; tries++) {
    portENTER_CRITICAL(&s_teMux);
    const uint32_t seq = s_teSeq;
    const int64_t at = s_teUs;
    portEXIT_CRITICAL(&s_teMux);
    if (seq != s_teUsed) {
      const int64_t age = esp_timer_get_time() - at;
      if (age < TE_LATE_US) {
        s_teUsed = seq;
        return;
      }
      if (tries == 0 && backToBack) countLateSync();
      s_teUsed = seq;  // too late for this one; wait for the next
    }
    xSemaphoreTake(s_teSem, pdMS_TO_TICKS(25));
  }
  countMissedSync();
}

// Send f (chunk 0 already built into s_bounce[0]). While the tail of f is on
// the wire, decide the following frame and pre-build its chunk 0; returns
// true when `next` is ready that way.
bool sendFrame(const Frame& f, Frame& next, bool backToBack) {
  setWindow();  // before the TE wait: the pixel stream must start at once
  waitTe(backToBack);
  const int64_t f0 = esp_timer_get_time();
  frameStarted(f, f0, backToBack);

  csLow();
  fillTr(s_tr[0], true, s_bounce[0], CHUNK_BYTES);
  spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[0], portMAX_DELAY);
  int inflight = 1;
  uint32_t build = f.build0Us;
  spi_transaction_t* r;
  for (int i = 1; i < NCHUNK; i++) {
    if (inflight == 2) {
      spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
      inflight--;
    }
    const int64_t b0 = esp_timer_get_time();
    frameBuild(f, s_bounce[i & 1], i * PANEL_ROWS);
    build += (uint32_t)(esp_timer_get_time() - b0);
    fillTr(s_tr[i], false, s_bounce[i & 1], CHUNK_BYTES);
    spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[i], portMAX_DELAY);
    inflight++;
  }
  // Every pixel of an LVGL frame is now in staging buffers: LVGL may reuse
  // its framebuffer while the DMA tail runs. (Valid only because DMA never
  // reads the framebuffer itself.)
  frameReleaseOverlays(f);
  if (f.releaseLv) frameReleaseLv();
  // The second-to-last chunk (even index, s_bounce[0]) finishes first; build
  // the next frame's chunk 0 there while the last chunk is still on the wire.
  spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  inflight--;
  bool have = frameDecide(next);
  if (have) {
    const int64_t b0 = esp_timer_get_time();
    frameBuild(next, s_bounce[0], 0);
    next.build0Us = (uint32_t)(esp_timer_get_time() - b0);
  }
  while (inflight--) spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  csHigh();

  const uint32_t fl = (uint32_t)(esp_timer_get_time() - f0);
  frameFinished(f, f0, build, fl, fl + f.build0Us);
  return have;
}

}  // namespace

int64_t transportLastSyncUs() {
  portENTER_CRITICAL(&s_teMux);
  const int64_t te = s_teUs;
  portEXIT_CRITICAL(&s_teMux);
  return te;
}

int64_t transportPresentUs(int64_t) { return 0; }

int64_t transportOverlayPresentUs(int64_t) { return 0; }

bool transportBackToBack() { return false; }

bool panelScanRingOption() { return false; }

int64_t transportAnimTargetUs(int64_t, int64_t*) { return 0; }

bool transportBegin() {
  gpio_reset_pin((gpio_num_t)PIN_CS);
  gpio_set_direction((gpio_num_t)PIN_CS, GPIO_MODE_OUTPUT);
  csHigh();

  spi_bus_config_t b = {};
  b.data0_io_num = PIN_D0;
  b.data1_io_num = PIN_D1;
  b.sclk_io_num = PIN_SCK;
  b.data2_io_num = PIN_D2;
  b.data3_io_num = PIN_D3;
  b.data4_io_num = b.data5_io_num = b.data6_io_num = b.data7_io_num = -1;
  b.max_transfer_sz = CHUNK_BYTES;
  b.flags = SPICOMMON_BUSFLAG_MASTER;
  if (spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  spi_device_interface_config_t d = {};
  d.command_bits = 8;
  d.address_bits = 24;
  d.mode = 0;
  d.clock_source = SPI_CLK_SRC_DEFAULT;
  d.clock_speed_hz = SPI_HZ;
  d.spics_io_num = -1;  // CS is held low by hand across a whole frame
  d.flags = SPI_DEVICE_HALFDUPLEX;
  d.queue_size = NCHUNK + 2;
  if (spi_bus_add_device(SPI2_HOST, &d, &s_dev) != ESP_OK) return false;

  for (int i = 0; i < 2; i++)
    s_bounce[i] = (uint16_t*)heap_caps_aligned_alloc(64, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!s_bounce[0] || !s_bounce[1]) {
    Serial.println("Deck panel: bounce buffer allocation FAILED");
    return false;
  }

  panelInitSequence();

  // Black first frame, before the backlight comes on.
  memset(s_bounce[0], 0, CHUNK_BYTES);
  setWindow();
  csLow();
  for (int i = 0; i < NCHUNK; i++) {
    fillTr(s_tr[i], i == 0, s_bounce[0], CHUNK_BYTES);
    spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[i], portMAX_DELAY);
  }
  spi_transaction_t* r;
  for (int i = 0; i < NCHUNK; i++) spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  csHigh();

  s_teSem = xSemaphoreCreateBinary();
#if BOARD_LCD_TE >= 0
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_TE;
  g.mode = GPIO_MODE_INPUT;
  g.intr_type = GPIO_INTR_POSEDGE;
  gpio_config(&g);
  esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  gpio_isr_handler_add((gpio_num_t)PIN_TE, teIsr, nullptr);
#else
  // No TE line: the panel's vertical blank is faked by a periodic timer so the
  // compositor keeps its ~60 Hz frame clock. Frames can tear.
  esp_timer_create_args_t ta = {};
  ta.callback = softTeTick;
  ta.dispatch_method = ESP_TIMER_TASK;
  ta.name = "deck_te";
  if (esp_timer_create(&ta, &s_teTimer) != ESP_OK) return false;
  if (esp_timer_start_periodic(s_teTimer, PANEL_SYNC_PERIOD_US) != ESP_OK) return false;
#endif
  return true;
}

void transportTask(void*) {
  Frame cur = {}, next = {};
  bool ready = false;
  for (;;) {
    if (quiesceRequested()) {
      if (ready) frameReleaseOverlays(cur);
      if (ready && cur.releaseLv) frameReleaseLv();
      ready = false;
      quiesceAck();
    }
    if (!ready) {
      if (!frameDecide(cur)) {
        frameIdleWait();
        continue;
      }
      const int64_t b0 = esp_timer_get_time();
      frameBuild(cur, s_bounce[0], 0);
      cur.build0Us = esp_timer_get_time() - b0;
    }
    ready = sendFrame(cur, next, ready);
    if (ready) cur = next;
  }
}

}  // namespace deck

#endif  // AXS15231B or ST77922 QSPI panel
