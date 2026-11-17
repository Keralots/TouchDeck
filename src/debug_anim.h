// Animation timing log for bench work: off until POST /api/debug/anim?on=1,
// then GET /api/debug/anim drains it as text lines "type us a b c d".
#pragma once
#include <Arduino.h>

enum : uint8_t {
  DA_FRAME = 1,    // panel frame (DSI): a=(shown-showAt)/100 us or -999999, b=shown-end us, c=showAt us mod 1e9, d=end-f0 us
  DA_SPARK = 2,    // chart glide step: a=off*1e4, b=target*1e4, c=dt ms | 0x40000000 without easing, d=chart id
  DA_FEED = 3,     // chart sample: a=next | chart id << 1, b=off before*1e4, c=off after*1e4, d=packet interval ms
  DA_WX = 4,       // weather icon frame: a=animation ms, b=1 every second refresh / 2 every refresh
  DA_LVFLUSH = 5,  // LVGL frame handed to the panel: a=buffer
  DA_AREA = 7,     // LVGL area drawn: a=x1 | y1 << 16, b=x2 | y2 << 16
};

void danim(uint8_t type, int32_t a = 0, int32_t b = 0, int32_t c = 0, int32_t d = 0);
// On allocates the ring (about 200 KB PSRAM, kept until restart); off stops logging.
bool danimEnable(bool on);
bool danimOn();
void danimDrain(String& out);
