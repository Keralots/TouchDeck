#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//  Firmware version
// =============================================================================
#define FW_VERSION          "1.3.0"
#define PRODUCT_NAME        "StatNode"

// Board variant - injected into the web UI for OTA asset filtering.
// Normally set via build_flags in platformio.ini; this is a fallback.
#ifndef BOARD_VARIANT
#define BOARD_VARIANT       "esp32s3"
#endif

// =============================================================================
//  Display
// =============================================================================
#include "layout.h"
#define SCREEN_W        LY_W
#define SCREEN_H        LY_H
#ifndef BACKLIGHT_PIN
#define BACKLIGHT_PIN   TFT_BL
#endif
#define BACKLIGHT_CH    0
#define BACKLIGHT_FREQ  5000
#define BACKLIGHT_RES   8

// =============================================================================
//  Color palette (RGB565)
// =============================================================================
#define CLR_BG          0x0000   // black
#define CLR_CARD        0x1926   // dark card bg
#define CLR_HEADER      0x10A2   // header bar bg
#define CLR_TEXT         0xFFFF   // white
#define CLR_TEXT_DIM     0xC618   // gray text
#define CLR_TEXT_DARK    0x7BEF   // darker gray
#define CLR_GREEN        0x07E0   // bright green
#define CLR_GREEN_DARK   0x0400   // dark green (track)
#define CLR_BLUE         0x34DF   // accent blue
#define CLR_ORANGE       0xFBE0   // warm accent
#define CLR_CYAN         0x07FF   // cool accent
#define CLR_RED          0xF800   // error / hot
#define CLR_YELLOW       0xFFE0   // warn
#define CLR_GOLD         0xFEA0   // highlight
#define CLR_TRACK        0x18E3   // arc background track
// Accents 7 and 8 for the extra gauge slots, taken from the colorblind-safe
// categorical set already used by the portal's "modern" preset (#9085e9,
// #d55181) so a full 8-slot layout stays distinguishable.
#define CLR_VIOLET       0x943D   // #9085e9
#define CLR_ROSE         0xD290   // #d55181

// =============================================================================
//  PC stats ingest (UDP push from the PC companion app)
// =============================================================================
#define UDP_PORT                4210    // companion pushes V2.1 JSON here
#define PC_STALE_TIMEOUT_MS     15000   // no packet for 15s = PC offline -> idle screen
#define PC_PACKET_BUF_SIZE      2048    // max UDP packet (oversize discarded)
#define MAX_METRICS             32      // max metrics tracked per packet

// PC companion status codes (mirror the V2.1 protocol)
#define PC_STATUS_OK             1
#define PC_STATUS_API_ERROR      2
#define PC_STATUS_LHM_NOT_RUNNING 3
#define PC_STATUS_LHM_STARTING   4

// =============================================================================
//  WiFi
// =============================================================================
#define WIFI_AP_PREFIX      "StatNode-"
#define WIFI_AP_PASSWORD    "statnode1234"
#define WIFI_CONNECT_TIMEOUT 15000  // 15s STA connect timeout
#define WIFI_RECONNECT_TIMEOUT 60000 // 60s before re-entering AP mode
#define WIFI_BACKOFF_PHASE1_MS    10000   // 10s between attempts in phase 1
#define WIFI_BACKOFF_PHASE2_MS    30000   // 30s between attempts after phase 1
#define WIFI_BACKOFF_PHASE3_MS    60000   // 60s between attempts after phase 2
#define WIFI_BACKOFF_PHASE2_START 5       // start phase 2 after this many attempts
#define WIFI_BACKOFF_PHASE3_START 10      // start phase 3 after this many attempts
#define WIFI_STA_PROBE_INTERVAL   120000  // 2 min between STA probes while in AP mode
#define WIFI_STA_PROBE_CHECK_MS    10000  // 10s after probe start before checking result
#define WIFI_AP_FALLBACK_MS       900000  // 15 min in phase 3 before falling back to AP

// Improv WiFi serial setup window on first boot (no stored credentials).
#define IMPROV_SETUP_WINDOW_MS    180000  // 3 min window for Improv setup at first boot

// =============================================================================
//  NVS
// =============================================================================
#define NVS_NAMESPACE       "pcmon"

// =============================================================================
//  Gauge full-scale ranges (user-configurable arc maxima)
//  Defaults cover typical desktop PC sensors. Lowering a scale makes the arc
//  sweep fuller for a metric's normal range (better visual resolution).
// =============================================================================
#define GAUGE_TEMP_SCALE_DEFAULT   100   // C (CPU/GPU temperature gauges)
#define GAUGE_TEMP_SCALE_MIN        60
#define GAUGE_TEMP_SCALE_MAX       120
#define GAUGE_POWER_SCALE_DEFAULT  300   // W (CPU/GPU package power gauge)
#define GAUGE_POWER_SCALE_MIN       50
#define GAUGE_POWER_SCALE_MAX      1000
#define GAUGE_FAN_SCALE_DEFAULT   3000   // RPM full-scale for the generic fan/value gauge
// Percent metrics (CPU/GPU load, RAM/VRAM usage) are always 0..100.

// Configurable metric->gauge mapping: number of editable gauge slots laid out
// in a responsive grid on the monitor screen. Each slot binds one PC metric
// (by id) to a gauge style/range/color (see GaugeSlot in settings.h).
// Raised 6 -> 8 so the larger panels can show more readings at once; the faces
// lay themselves out from the number of BOUND slots, so this is a ceiling, not
// a fixed count. Do NOT exceed 8: slotWarn()'s hysteresis state and the Pulse
// face's quantizer both track slots in a uint8_t bitmask.
// Growing this changes sizeof(GaugeMapping)/sizeof(GaugeLabels), so
// loadSettings() migrates the older, smaller NVS blobs (see LEGACY_*_SLOTS in
// settings.cpp) instead of falling through to defaults and wiping the user's
// bindings.
#define NUM_GAUGE_SLOTS              8
#define LEGACY_GAUGE_SLOTS_V1        6   // slot count shipped before 2026-07-25

// How many slots come pre-bound on a factory-fresh or reset device. The 320x480
// panel shows eight readings comfortably; a 240x240 square starts to crowd past
// six. This is ONLY the out-of-box default - how many tiles appear is always the
// number of slots that have a metric bound, which the user changes on the
// portal's Metrics page. There is deliberately no separate "slot count" setting
// to disagree with the bindings.
#if defined(DISPLAY_320x480)
#define DEFAULT_BOUND_SLOTS          8
#else
#define DEFAULT_BOUND_SLOTS          6
#endif

// How many independent sets of slot bindings ("metric pages") the device holds.
// At 1 this is byte-for-byte the single-mapping behavior that shipped before
// pages existed. Page 0 always persists under the original NVS keys "gauges" and
// "labels", so raising this count adds keys instead of resizing a blob and never
// migrates an existing device.
#define METRIC_PAGE_COUNT            3

// Default GPIO for the TTP223 capacitive pad. 4 on both S3 and C3: that is where
// the pre-shipped units wire it, and the C3's pad was never physically wired, so
// there is no reason for the two boards to differ. Only affects factory-fresh and
// reset devices - the pin is configurable from the portal, and touchPinAllowed()
// in touch_button.cpp decides what is selectable per board.
#define TOUCH_DEFAULT_PIN            4

// =============================================================================
//  Physical button
// =============================================================================
#ifdef DISPLAY_240x320
#define BUTTON_DEFAULT_PIN    0       // CYD: GPIO4 is RGB LED, not usable
#else
#define BUTTON_DEFAULT_PIN    4       // default GPIO for physical button
#endif
#define TOUCH_DEBOUNCE_MS     50
// 1200 ms, not 800. A mechanical button is released cleanly, but a fingertip
// rests on a capacitive pad, so 800 ms turned ordinary taps into long presses.
#define TOUCH_LONG_PRESS_MS 1200
#define TOUCH_WAKE_MS      30000

// =============================================================================
//  Display refresh
// =============================================================================
#define DISPLAY_UPDATE_MS          250    // ~4 Hz refresh rate
// ~10 Hz for the animated glass faces. Chosen from measurement, not taste: a
// Glass Tiles frame at eight metrics costs ~42 ms on the S3 (per-pixel
// compositing plus ~11 ms of serialised SPI), so 20 Hz would blow the budget
// and starve WiFi. The chart only scrolls ~1.7 px/second, so 10 sub-steps per
// pixel is already continuous motion - a faster rate would burn CPU redrawing
// a picture that has not visibly changed.
// Named for the glass faces that needed it first; it now paces EVERY monitor
// face, since they all animate their charts.
#define GLASS_UPDATE_MS            100
#define SPARK_REDRAW_MS           3000    // min ms between sparkline repaints (calm charts)
#define DISPLAY_STATE_TIMEOUT_MS   60000  // 60s timeout for intermediate display states

// =============================================================================
//  Buzzer (optional passive buzzer)
// =============================================================================
#if defined(BOARD_IS_C3)
#define BUZZER_DEFAULT_PIN    3       // C3: GPIO 3 (GPIO 5 is backlight)
#elif defined(DISPLAY_CYD) || defined(DISPLAY_240x320)
#define BUZZER_DEFAULT_PIN    26      // CYD: GPIO 26
#else
#define BUZZER_DEFAULT_PIN    5       // S3: GPIO 5
#endif

// =============================================================================
//  External LED (optional, PWM dimmable via NPN/MOSFET)
// =============================================================================
#define LED_PWM_CH      2     // LEDC channel (timer 1, isolated from tone() ch0 and analogWrite backlight)
#define LED_PWM_FREQ    5000  // PWM frequency (Hz)
#define LED_PWM_RES     8     // PWM resolution (bits) -> 0..255 duty

#if defined(DISPLAY_CYD)
#define LED_DEFAULT_PIN 22    // CYD: GPIO 22 on P3 connector
#elif defined(BOARD_IS_JC3248W535)
#define LED_DEFAULT_PIN 17    // Guition: extended IO header (single source, meter it before wiring)
#else
#define LED_DEFAULT_PIN 0     // other boards: user must configure
#endif

// LED effect tuning (ms periods)
#define LED_BREATH_PERIOD_MS      2000   // breathing pulse 0->peak->0
#define LED_HEARTBEAT_PERIOD_MS   1500   // pyk-pyk-pause cycle
#define LED_ERROR_STROBE_MS        180   // strobe half-period (180 on / 180 off)
#define LED_TICK_MIN_INTERVAL_MS    16   // throttle ledTick() to ~60Hz
#define LED_TEST_DURATION_S          8   // /led/test runs the chosen effect for 8s

#endif // CONFIG_H
