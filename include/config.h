#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//  Firmware version
// =============================================================================
#define FW_VERSION          "1.1.0"
#define PRODUCT_NAME        "TouchDeck"

// Board variant - reported to the portal and the OTA page.
#ifndef BOARD_VARIANT
#define BOARD_VARIANT       "jc3248w535"
#endif

// =============================================================================
//  PC stats ingest (UDP push from the StatNode Companion)
// =============================================================================
#define UDP_PORT                4210    // companion pushes V2.1 JSON here
#define PC_STALE_TIMEOUT_MS     15000   // no packet for 15s = PC offline
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
#define WIFI_AP_PREFIX      "TouchDeck-"
#define WIFI_AP_PASSWORD    "touchdeck1234"
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
// Kept from StatNode so a device moved over keeps its WiFi and network setup.
#define NVS_NAMESPACE       "pcmon"

// =============================================================================
//  PC metric tiles: default full scale when a tile sets none
// =============================================================================
#define GAUGE_TEMP_SCALE_DEFAULT   100   // C
#define GAUGE_POWER_SCALE_DEFAULT  300   // W
#define GAUGE_FAN_SCALE_DEFAULT   3000   // RPM and anything without a unit rule

// =============================================================================
//  External LED (optional, PWM dimmable via NPN/MOSFET)
// =============================================================================
#define LED_PWM_FREQ    5000  // PWM frequency (Hz)
#define LED_PWM_RES     8     // PWM resolution (bits) -> 0..255 duty
#define LED_DEFAULT_PIN 17    // extended IO header (meter it before wiring)

// LED effect tuning (ms periods)
#define LED_TICK_MIN_INTERVAL_MS    16   // throttle ledTick() to ~60Hz

#endif // CONFIG_H
