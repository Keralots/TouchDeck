// Guition JC3248W535 display pipeline. See deck_panel.h for the contract and
// the measured panel facts behind it.
#if defined(BOARD_IS_JC3248W535)

#include "deck_panel.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
// Only for axs15231b_180640_init_operations and its op-code enum; the
// library's databus and panel classes are not used.
#include <Arduino_GFX_Library.h>

namespace deck {
namespace {

constexpr int PIN_CS = 45;
constexpr int PIN_SCK = 47;
constexpr int PIN_D0 = 21;
constexpr int PIN_D1 = 48;
constexpr int PIN_D2 = 40;
constexpr int PIN_D3 = 39;
constexpr int PIN_TE = 38;

// 48 native rows per chunk: 30,720 bytes (the S3 limit for one SPI
// transaction is 32 KB), 10 chunks per frame. Smaller chunks save internal
// RAM but every transaction adds overhead, and with 20 chunks a frame grew
// to ~16 ms, too close to the 16.39 ms TE period. Two staging buffers only:
// the next frame's first chunk is pre-built into whichever one frees first.
constexpr int ROWS = 48;
constexpr size_t CHUNK_BYTES = (size_t)NATIVE_W * ROWS * 2;
constexpr int NCHUNK = NATIVE_H / ROWS;
constexpr uint32_t SPI_HZ = 40000000;  // 80 MHz corrupts the image on this panel
// The write (15.4 ms) is slower than the scan, so a frame that starts later
// than this after the TE rising edge is overtaken near the bottom.
constexpr int64_t TE_LATE_US = 900;
// Animation frames are sampled this far ahead of the build so the offset
// matches the moment the frame is actually on the glass.
constexpr int64_t ANIM_LEAD_US = 8000;

spi_device_handle_t s_dev = nullptr;
uint16_t* s_fb[2] = {nullptr, nullptr};
uint16_t* s_bounce[2] = {nullptr, nullptr};
DMA_ATTR uint8_t s_cmdbuf[64];
spi_transaction_ext_t s_tr[NCHUNK];
TaskHandle_t s_task = nullptr;
volatile uint8_t s_rot = 1;

// --- TE ----------------------------------------------------------------------
portMUX_TYPE s_teMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t s_teSeq = 0;
volatile int64_t s_teUs = 0;
SemaphoreHandle_t s_teSem = nullptr;
uint32_t s_teUsed = 0;

void IRAM_ATTR teIsr(void*) {
  portENTER_CRITICAL_ISR(&s_teMux);
  s_teUs = esp_timer_get_time();
  s_teSeq = s_teSeq + 1;
  portEXIT_CRITICAL_ISR(&s_teMux);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_teSem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// --- Shared frame / compositor state (s_mux) ----------------------------------
SemaphoreHandle_t s_lvReady = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct State {
  bool lvPending = false;
  int lvIdx = 0;
  uint32_t lvGen = 0;
  int lvLatest = -1;
  CompMode mode = COMP_NONE;
  Src a = {nullptr, 0, true};
  Src b = {nullptr, 0, true};
  int off = 0;
  int lastSentOff = 0;
  bool sentAny = false;
  int fromOff = 0;
  int toOff = 0;
  int64_t t0 = 0;
  uint32_t durUs = 1;
  int8_t landing = 0;
  uint32_t holdGen = UINT32_MAX;
  bool landedPending = false;
  int8_t landed = 0;
};
State s;

// A frame to send, resolved under the lock so the build never reads shared
// state: either one LVGL framebuffer or a composed A|B strip.
struct Frame {
  bool comp;
  bool releaseLv;
  uint8_t rot;
  int w;
  const uint16_t* lv;
  Src a, b;
  int off;
};

// --- Stats (approximate, single writer) ----------------------------------------
volatile uint32_t st_frames = 0, st_comp = 0, st_flushSum = 0, st_flushMax = 0;
volatile uint32_t st_buildSum = 0, st_teLate = 0, st_teTimeout = 0, st_lvDropped = 0;
volatile uint32_t st_gapMax = 0;
int64_t s_lastStartUs = 0;
bool s_lastWasComp = false;

inline void csLow() { gpio_set_level((gpio_num_t)PIN_CS, 0); }
inline void csHigh() { gpio_set_level((gpio_num_t)PIN_CS, 1); }

// Command with optional parameters on one data line, polling. Only the flush
// task (or panelBegin before it exists) may call this: polling transfers are
// not allowed while queued transactions are pending.
void txCmd(uint8_t c, const uint8_t* d, size_t n) {
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
}

void panelInitSequence() {
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
}

// Full-width window. RASET is deliberately not sent (QSPI quirk); RAMWR
// restarts the frame at the top-left native pixel.
void setWindow() {
  const uint8_t ca[4] = {0, 0, (uint8_t)((NATIVE_W - 1) >> 8),
                         (uint8_t)((NATIVE_W - 1) & 0xFF)};
  txCmd(0x2A, ca, 4);
  txCmd(0x2C, nullptr, 0);
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

inline void logicalSize(uint8_t rot, int& w, int& h) {
  if (rot & 1) { w = NATIVE_H; h = NATIVE_W; }
  else         { w = NATIVE_W; h = NATIVE_H; }
}

// --- Chunk build ----------------------------------------------------------------
// Copy n logical pixels starting at s (walking +1 or -1) to d (walking dstep).
template <bool SWAP, int DIR>
inline void copyRun(uint16_t* d, int dstep, const uint16_t* s, int n) {
  for (int k = 0; k < n; k++) {
    uint16_t c = s[DIR * k];
    d[k * dstep] = SWAP ? __builtin_bswap16(c) : c;
  }
}

inline void copyAny(uint16_t* d, int dstep, const uint16_t* s, int dir, int n, bool swap) {
  if (swap) {
    if (dir > 0) copyRun<true, 1>(d, dstep, s, n);
    else         copyRun<true, -1>(d, dstep, s, n);
  } else {
    if (dir > 0) copyRun<false, 1>(d, dstep, s, n);
    else         copyRun<false, -1>(d, dstep, s, n);
  }
}

// Emit n pixels of logical row ly, starting at logical x0 and walking dir.
void emitRow(const Frame& f, int ly, int x0, int dir, int n, uint16_t* d, int dstep) {
  if (!f.comp) {
    copyAny(d, dstep, f.lv + ly * f.w + x0, dir, n, true);
    return;
  }
  while (n > 0) {
    const int vx = x0 + f.off;  // position in the A|B strip
    const Src* src = vx < f.w ? &f.a : &f.b;
    const int sx = vx < f.w ? vx : vx - f.w;
    int run = dir > 0 ? f.w - sx : sx + 1;
    if (run > n) run = n;
    copyAny(d, dstep, src->px + ly * src->stride + sx, dir, run, src->swap);
    x0 += dir * run;
    d += dstep * run;
    n -= run;
  }
}

// Fill one native chunk (rows ny0 .. ny0+ROWS-1). The inner loops always walk
// along a logical row, which keeps PSRAM reads sequential. Mapping per user
// rotation (identical to LovyanGFX's sprite rotation):
//   0: lx = nx,        ly = ny
//   1: lx = ny,        ly = 319 - nx
//   2: lx = 319 - nx,  ly = 479 - ny
//   3: lx = 479 - ny,  ly = nx
void buildChunk(const Frame& f, uint16_t* dst, int ny0) {
  if (!f.comp && (f.rot & 1)) {
    // Plain landscape LVGL frame: the hot path, kept free of the per-run
    // bookkeeping emitRow() needs for composed strips.
    const uint16_t* lv = f.lv;
    if (f.rot == 1) {
      for (int ly = 0; ly < NATIVE_W; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + ny0;
        uint16_t* d = dst + (NATIVE_W - 1 - ly);
        for (int k = 0; k < ROWS; k++) d[k * NATIVE_W] = __builtin_bswap16(s[k]);
      }
    } else {
      for (int ly = 0; ly < NATIVE_W; ly++) {
        const uint16_t* s = lv + ly * NATIVE_H + (NATIVE_H - 1 - ny0);
        uint16_t* d = dst + ly;
        for (int k = 0; k < ROWS; k++) d[k * NATIVE_W] = __builtin_bswap16(s[-k]);
      }
    }
    return;
  }
  if (!f.comp) {
    // Plain portrait LVGL frame: whole logical rows, forward or reversed.
    for (int k = 0; k < ROWS; k++) {
      uint16_t* d = dst + k * NATIVE_W;
      if (f.rot == 0) {
        const uint16_t* s = f.lv + (ny0 + k) * NATIVE_W;
        for (int x = 0; x < NATIVE_W; x++) d[x] = __builtin_bswap16(s[x]);
      } else {
        const uint16_t* s = f.lv + (NATIVE_H - 1 - (ny0 + k)) * NATIVE_W + (NATIVE_W - 1);
        for (int x = 0; x < NATIVE_W; x++) d[x] = __builtin_bswap16(s[-x]);
      }
    }
    return;
  }
  switch (f.rot) {
    case 1:
      for (int ly = 0; ly < NATIVE_W; ly++)
        emitRow(f, ly, ny0, 1, ROWS, dst + (NATIVE_W - 1 - ly), NATIVE_W);
      break;
    case 3:
      for (int ly = 0; ly < NATIVE_W; ly++)
        emitRow(f, ly, NATIVE_H - 1 - ny0, -1, ROWS, dst + ly, NATIVE_W);
      break;
    case 2:
      for (int k = 0; k < ROWS; k++)
        emitRow(f, NATIVE_H - 1 - (ny0 + k), NATIVE_W - 1, -1, NATIVE_W,
                dst + k * NATIVE_W, 1);
      break;
    default:
      for (int k = 0; k < ROWS; k++)
        emitRow(f, ny0 + k, 0, 1, NATIVE_W, dst + k * NATIVE_W, 1);
      break;
  }
}

inline float easeOutCubic(float p) {
  const float q = 1.0f - p;
  return 1.0f - q * q * q;
}

// Pick the next frame. Returns false when there is nothing new to show.
bool decide(Frame& f) {
  bool have = false;
  bool give = false;
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&s_mux);
  const uint8_t rot = s_rot;
  int w, h;
  logicalSize(rot, w, h);
  f.rot = rot;
  f.w = w;
  f.releaseLv = false;
  if (s.lvPending) {
    s.lvPending = false;
    if (s.mode == COMP_HOLD && s.lvGen >= s.holdGen) s.mode = COMP_NONE;
    s.lvLatest = s.lvIdx;
    if (s.mode == COMP_NONE) {
      f.comp = false;
      f.lv = s_fb[s.lvIdx];
      f.releaseLv = true;
      have = true;
    } else {
      // Superseded by the compositor. The buffer still holds a complete frame
      // (direct mode), so it stays usable as the "current page" image.
      give = true;
      st_lvDropped = st_lvDropped + 1;
    }
  }
  if (!have && (s.mode == COMP_DRAG || s.mode == COMP_ANIM)) {
    int off = s.off;
    if (s.mode == COMP_ANIM) {
      const int64_t t = now + ANIM_LEAD_US - s.t0;
      if (t >= (int64_t)s.durUs) {
        off = s.toOff;
        s.mode = s.landing ? COMP_HOLD : COMP_NONE;
        s.holdGen = UINT32_MAX;
        s.landed = s.landing;
        s.landedPending = true;
      } else {
        const float p = t <= 0 ? 0.0f : (float)t / (float)s.durUs;
        off = s.fromOff + (int)((s.toOff - s.fromOff) * easeOutCubic(p));
      }
      s.off = off;
    }
    if ((!s.sentAny || off != s.lastSentOff) && s.lvLatest >= 0) {
      f.comp = true;
      f.a = s.a;
      f.b = s.b;
      const uint16_t* cur = s_fb[s.lvLatest];
      if (!f.a.px) f.a = {cur, (uint16_t)w, true};
      if (!f.b.px) f.b = {cur, (uint16_t)w, true};
      f.off = off < 0 ? 0 : (off > w ? w : off);
      s.lastSentOff = off;
      s.sentAny = true;
      have = true;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  if (give) xSemaphoreGive(s_lvReady);
  return have;
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
      if (tries == 0 && backToBack) st_teLate = st_teLate + 1;
      s_teUsed = seq;  // too late for this one; wait for the next
    }
    xSemaphoreTake(s_teSem, pdMS_TO_TICKS(25));
  }
  st_teTimeout = st_teTimeout + 1;
}

// Send f (chunk 0 already built into s_bounce[0]). While the tail of f is on
// the wire, decide the following frame and pre-build its chunk 0; returns
// true when `next` is ready that way.
bool sendFrame(const Frame& f, Frame& next, bool backToBack) {
  setWindow();  // before the TE wait: the pixel stream must start at once
  waitTe(backToBack);
  const int64_t f0 = esp_timer_get_time();
  if (backToBack && f.comp && s_lastWasComp) {
    const uint32_t gap = (uint32_t)(f0 - s_lastStartUs);
    if (gap > st_gapMax) st_gapMax = gap;
  }
  s_lastStartUs = f0;
  s_lastWasComp = f.comp;

  csLow();
  fillTr(s_tr[0], true, s_bounce[0], CHUNK_BYTES);
  spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[0], portMAX_DELAY);
  int inflight = 1;
  uint32_t build = 0;
  spi_transaction_t* r;
  for (int i = 1; i < NCHUNK; i++) {
    if (inflight == 2) {
      spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
      inflight--;
    }
    const int64_t b0 = esp_timer_get_time();
    buildChunk(f, s_bounce[i & 1], i * ROWS);
    build += (uint32_t)(esp_timer_get_time() - b0);
    fillTr(s_tr[i], false, s_bounce[i & 1], CHUNK_BYTES);
    spi_device_queue_trans(s_dev, (spi_transaction_t*)&s_tr[i], portMAX_DELAY);
    inflight++;
  }
  // Every pixel of an LVGL frame is now in staging buffers: LVGL may reuse
  // its framebuffer while the DMA tail runs. (Valid only because DMA never
  // reads the framebuffer itself.)
  if (f.releaseLv) xSemaphoreGive(s_lvReady);
  // The second-to-last chunk (even index, s_bounce[0]) finishes first; build
  // the next frame's chunk 0 there while the last chunk is still on the wire.
  spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  inflight--;
  bool have = decide(next);
  if (have) {
    const int64_t b0 = esp_timer_get_time();
    buildChunk(next, s_bounce[0], 0);
    build += (uint32_t)(esp_timer_get_time() - b0);
  }
  while (inflight--) spi_device_get_trans_result(s_dev, &r, portMAX_DELAY);
  csHigh();

  const int64_t end = esp_timer_get_time();
  const uint32_t fl = (uint32_t)(end - f0);
  st_frames = st_frames + 1;
  if (f.comp) st_comp = st_comp + 1;
  st_flushSum = st_flushSum + fl;
  if (fl > st_flushMax) st_flushMax = fl;
  st_buildSum = st_buildSum + build;
  return have;
}

static_assert(NCHUNK % 2 == 0, "the last chunk must use s_bounce[1]");

void flushTask(void*) {
  Frame cur = {}, next = {};
  bool ready = false;
  for (;;) {
    if (!ready) {
      if (!decide(cur)) {
        const bool animating = compMode() == COMP_ANIM;
        ulTaskNotifyTake(pdTRUE, animating ? 1 : pdMS_TO_TICKS(50));
        continue;
      }
      buildChunk(cur, s_bounce[0], 0);
    }
    ready = sendFrame(cur, next, ready);
    if (ready) cur = next;
  }
}

}  // namespace

// --- Public API -------------------------------------------------------------------
bool panelBegin(uint8_t rotation) {
  s_rot = rotation & 3;
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

  for (int i = 0; i < 2; i++) {
    s_fb[i] = (uint16_t*)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_bounce[i] = (uint16_t*)heap_caps_aligned_alloc(64, CHUNK_BYTES,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  }
  if (!s_fb[0] || !s_fb[1] || !s_bounce[0] || !s_bounce[1]) {
    Serial.println("Deck panel: buffer allocation FAILED");
    return false;
  }
  memset(s_fb[0], 0, FRAME_BYTES);
  memset(s_fb[1], 0, FRAME_BYTES);

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
  s_lvReady = xSemaphoreCreateBinary();
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_TE;
  g.mode = GPIO_MODE_INPUT;
  g.intr_type = GPIO_INTR_POSEDGE;
  gpio_config(&g);
  esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
  gpio_isr_handler_add((gpio_num_t)PIN_TE, teIsr, nullptr);

  // Core 1 at high priority: on core 0 the flush loses to WiFi/lwIP under
  // network load (measured: ~30 fps slides instead of 60).
  xTaskCreatePinnedToCore(flushTask, "deck_flush", 4096, nullptr, 20, &s_task, 1);
  Serial.printf("Deck panel: up, rotation %u, internal free %u, largest %u\n",
                (unsigned)s_rot,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

void panelSetRotation(uint8_t rotation) {
  compCancel();
  s_rot = rotation & 3;
}

uint8_t panelRotation() { return s_rot; }

int logicalW() { return (s_rot & 1) ? NATIVE_H : NATIVE_W; }
int logicalH() { return (s_rot & 1) ? NATIVE_W : NATIVE_H; }

uint16_t* panelLvBuffer(int i) { return s_fb[i & 1]; }

const uint16_t* panelLatestLvFrame() {
  portENTER_CRITICAL(&s_mux);
  const int idx = s.lvLatest;
  portEXIT_CRITICAL(&s_mux);
  return idx >= 0 ? s_fb[idx] : nullptr;
}

void panelPostLv(int idx, uint32_t gen) {
  portENTER_CRITICAL(&s_mux);
  s.lvPending = true;
  s.lvIdx = idx & 1;
  s.lvGen = gen;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

bool panelWaitLvReady(uint32_t timeoutMs) {
  return xSemaphoreTake(s_lvReady, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool compBeginDrag(const Src& a, const Src& b, int off) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_NONE && s.lvLatest >= 0) {
    s.mode = COMP_DRAG;
    s.a = a;
    s.b = b;
    s.off = off;
    s.sentAny = false;
    s.landedPending = false;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void compDrag(int off) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_DRAG) s.off = off;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

void compRelease(int toOff, uint32_t durMs, int8_t landing) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_DRAG) {
    s.mode = COMP_ANIM;
    s.fromOff = s.off;
    s.toOff = toOff;
    s.t0 = esp_timer_get_time();
    s.durUs = durMs ? durMs * 1000u : 1u;
    s.landing = landing;
  }
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

CompMode compMode() {
  portENTER_CRITICAL(&s_mux);
  CompMode m = s.mode;
  portEXIT_CRITICAL(&s_mux);
  return m;
}

bool compOwnsScreen() {
  CompMode m = compMode();
  return m == COMP_DRAG || m == COMP_ANIM;
}

bool compTakeLanding(int8_t* landing) {
  bool got = false;
  portENTER_CRITICAL(&s_mux);
  if (s.landedPending) {
    s.landedPending = false;
    *landing = s.landed;
    got = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return got;
}

void compSetHoldGen(uint32_t gen) {
  portENTER_CRITICAL(&s_mux);
  if (s.mode == COMP_HOLD) s.holdGen = gen;
  portEXIT_CRITICAL(&s_mux);
  if (s_task) xTaskNotifyGive(s_task);
}

void compCancel() {
  portENTER_CRITICAL(&s_mux);
  s.mode = COMP_NONE;
  s.landedPending = false;
  portEXIT_CRITICAL(&s_mux);
}

PanelStats panelTakeStats() {
  PanelStats p;
  const uint32_t n = st_frames;
  p.frames = n;
  p.compFrames = st_comp;
  p.flushUsAvg = n ? st_flushSum / n : 0;
  p.flushUsMax = st_flushMax;
  p.buildUsAvg = n ? st_buildSum / n : 0;
  p.teLate = st_teLate;
  p.teTimeouts = st_teTimeout;
  p.lvDropped = st_lvDropped;
  p.gapMaxUs = st_gapMax;
  st_frames = 0;
  st_comp = 0;
  st_flushSum = 0;
  st_flushMax = 0;
  st_buildSum = 0;
  st_teLate = 0;
  st_teTimeout = 0;
  st_lvDropped = 0;
  st_gapMax = 0;
  return p;
}

}  // namespace deck

#endif  // BOARD_IS_JC3248W535
