#include "deck_sys.h"
#include "deck_panel.h"
#include "config.h"
#include "settings.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <lvgl.h>

namespace deck {
namespace {

constexpr uint32_t C_BG = 0x0D0F13;
constexpr uint32_t C_CHIP = 0x272B33;
constexpr uint32_t C_TITLE = 0xF1F2F4;
constexpr uint32_t C_TEXT = 0xE6E8EC;
constexpr uint32_t C_DIM = 0x8A91A0;
constexpr uint32_t C_AMBER = 0xF6C56B;
constexpr uint32_t C_TEAL = 0x5CC9A7;
constexpr uint32_t C_VIOLET = 0xB3A6FF;
constexpr uint32_t C_BLUE = 0x7DB5FF;
constexpr uint32_t C_RED = 0xFF8A7A;

lv_obj_t* s_root = nullptr;
ScreenState s_shown = SCREEN_DECK;
bool s_rebuild = false;

// Firmware update widgets, valid while that screen is up.
lv_obj_t* s_otaTitle = nullptr;
lv_obj_t* s_otaPct = nullptr;
lv_obj_t* s_otaBar = nullptr;
lv_obj_t* s_otaInfo = nullptr;
lv_obj_t* s_otaNote = nullptr;
int s_otaLastPct = -1;

lv_obj_t* box(lv_obj_t* parent, lv_flex_flow_t flow, int gap) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(o, flow);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(o, gap, 0);
  lv_obj_set_style_pad_column(o, gap, 0);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return o;
}

lv_obj_t* text(lv_obj_t* parent, const char* t, const lv_font_t* f, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  return l;
}

lv_obj_t* gap(lv_obj_t* parent, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 1, h);
  return o;
}

// Four rounded tiles in the deck's accent colours: the product mark.
void logoMark(lv_obj_t* parent, int cell) {
  const int g = cell / 4;
  lv_obj_t* m = lv_obj_create(parent);
  lv_obj_remove_style_all(m);
  lv_obj_set_size(m, 2 * cell + g, 2 * cell + g);
  const uint32_t colors[4] = {C_AMBER, C_TEAL, C_VIOLET, C_BLUE};
  for (int i = 0; i < 4; i++) {
    lv_obj_t* c = lv_obj_create(m);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, cell, cell);
    lv_obj_set_pos(c, (i % 2) * (cell + g), (i / 2) * (cell + g));
    lv_obj_set_style_radius(c, cell * 3 / 10, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(colors[i]), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  }
}

// Round chip with a symbol, for the state screens.
void badge(lv_obj_t* parent, const char* symbol, uint32_t bg, uint32_t fg) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, 56, 56);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_t* l = text(c, symbol, &deck_font_28, fg);
  lv_obj_center(l);
}

void buildSplash(lv_obj_t* col) {
  logoMark(col, 30);
  gap(col, 6);
  text(col, PRODUCT_NAME, &lv_font_montserrat_40, C_TITLE);
  text(col, "v" FW_VERSION, &deck_font_14, C_DIM);
}

void buildConnecting(lv_obj_t* col) {
  lv_obj_t* sp = lv_spinner_create(col);
  lv_obj_set_size(sp, 48, 48);
  lv_spinner_set_anim_params(sp, 1100, 240);
  lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, lv_color_hex(C_CHIP), LV_PART_MAIN);
  lv_obj_set_style_arc_color(sp, lv_color_hex(C_AMBER), LV_PART_INDICATOR);
  gap(col, 8);
  text(col, "Connecting to WiFi", &deck_font_20, C_TITLE);
  text(col, wifiSSID, &deck_font_14, C_AMBER);
}

void buildConnected(lv_obj_t* col) {
  badge(col, LV_SYMBOL_OK, C_TEAL, C_BG);
  gap(col, 6);
  text(col, "Connected", &deck_font_20, C_TITLE);
  text(col, WiFi.localIP().toString().c_str(), &lv_font_montserrat_40, C_TEXT);
  if (netSettings.mdnsEnabled && netSettings.hostname[0]) {
    String host = String("http://") + netSettings.hostname + ".local";
    text(col, host.c_str(), &deck_font_14, C_DIM);
  }
}

// WiFi join QR code next to the written steps; stacked in portrait.
void buildApSetup(lv_obj_t* root, bool landscape) {
  const String ssid = getAPSSID();
  lv_obj_t* wrap = box(root, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN, landscape ? 28 : 16);

  lv_obj_t* qrBox = lv_obj_create(wrap);
  lv_obj_remove_style_all(qrBox);
  lv_obj_set_size(qrBox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(qrBox, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(qrBox, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qrBox, 14, 0);
  lv_obj_set_style_pad_all(qrBox, 10, 0);
  lv_obj_t* qr = lv_qrcode_create(qrBox);
  lv_qrcode_set_size(qr, landscape ? 150 : 132);
  lv_qrcode_set_dark_color(qr, lv_color_hex(C_BG));
  lv_qrcode_set_light_color(qr, lv_color_white());
  const String data = "WIFI:T:WPA;S:" + ssid + ";P:" WIFI_AP_PASSWORD ";;";
  lv_qrcode_update(qr, data.c_str(), data.length());

  lv_obj_t* col = box(wrap, LV_FLEX_FLOW_COLUMN, 2);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, landscape ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  text(col, "WiFi setup", &deck_font_28, C_TITLE);
  gap(col, 8);
  text(col, "1. Join this network", &deck_font_14, C_DIM);
  text(col, ssid.c_str(), &deck_font_20, C_AMBER);
  text(col, "Password  " WIFI_AP_PASSWORD, &deck_font_14, C_TEXT);
  gap(col, 8);
  text(col, "2. Open in a browser", &deck_font_14, C_DIM);
  text(col, WiFi.softAPIP().toString().c_str(), &deck_font_20, C_AMBER);
  gap(col, 8);
  text(col, "Or scan the code with a phone.", &deck_font_14, C_DIM);
}

void buildOta(lv_obj_t* col, int w) {
  s_otaTitle = text(col, "Updating firmware", &deck_font_28, C_TITLE);
  s_otaPct = text(col, "0%", &lv_font_montserrat_40, C_AMBER);
  gap(col, 4);
  s_otaBar = lv_bar_create(col);
  lv_obj_set_size(s_otaBar, w - 80 < 360 ? w - 80 : 360, 12);
  lv_bar_set_range(s_otaBar, 0, 1000);
  lv_obj_set_style_radius(s_otaBar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(s_otaBar, 6, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_otaBar, lv_color_hex(C_CHIP), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_otaBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_otaBar, lv_color_hex(C_AMBER), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_otaBar, LV_OPA_COVER, LV_PART_INDICATOR);
  gap(col, 4);
  s_otaInfo = text(col, "Receiving", &deck_font_14, C_TEXT);
  s_otaNote = text(col, "Keep the power on.", &deck_font_14, C_DIM);
  s_otaLastPct = -1;
}

void destroy() {
  if (s_root) lv_obj_delete(s_root);
  s_root = nullptr;
  s_otaTitle = s_otaPct = s_otaBar = s_otaInfo = s_otaNote = nullptr;
}

void build(ScreenState st) {
  destroy();
  if (st == SCREEN_DECK) return;
  const int w = logicalW(), h = logicalH();
  // Opaque and clickable: covers the pages and swallows their taps.
  s_root = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, w, h);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_root, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_root, 6, 0);
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

  switch (st) {
    case SCREEN_SPLASH: buildSplash(s_root); break;
    case SCREEN_CONNECTING_WIFI: buildConnecting(s_root); break;
    case SCREEN_WIFI_CONNECTED: buildConnected(s_root); break;
    case SCREEN_AP_MODE: buildApSetup(s_root, w > h); break;
    case SCREEN_OTA_UPDATE: buildOta(s_root, w); break;
    default: break;
  }
}

}  // namespace

void sysScreenSync(ScreenState state) {
  if (state == s_shown && !s_rebuild) return;
  s_shown = state;
  s_rebuild = false;
  build(state);
}

void sysScreenRelayout() { s_rebuild = s_shown != SCREEN_DECK; }

void sysOtaProgress(uint32_t done, uint32_t total) {
  if (!s_otaBar) return;
  char buf[40];
  if (total) {
    const int permille = (int)((uint64_t)done * 1000 / total);
    lv_bar_set_value(s_otaBar, permille > 1000 ? 1000 : permille, LV_ANIM_OFF);
    const int pct = permille / 10;
    if (pct != s_otaLastPct) {
      s_otaLastPct = pct;
      lv_label_set_text_fmt(s_otaPct, "%d%%", pct > 100 ? 100 : pct);
    }
    snprintf(buf, sizeof(buf), "%.1f of %.1f MB", done / 1048576.0f, total / 1048576.0f);
  } else {
    snprintf(buf, sizeof(buf), "%.1f MB received", done / 1048576.0f);
  }
  lv_label_set_text(s_otaInfo, buf);
}

void sysOtaResult(bool ok, const char* message) {
  if (!s_otaBar) return;
  const uint32_t color = ok ? C_TEAL : C_RED;
  lv_label_set_text(s_otaTitle, ok ? "Update complete" : "Update failed");
  lv_obj_set_style_text_color(s_otaPct, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(s_otaBar, lv_color_hex(color), LV_PART_INDICATOR);
  if (ok) {
    lv_label_set_text(s_otaPct, "100%");
    lv_bar_set_value(s_otaBar, 1000, LV_ANIM_OFF);
    lv_label_set_text(s_otaInfo, "Restarting");
    lv_label_set_text(s_otaNote, "");
  } else {
    lv_label_set_text(s_otaPct, LV_SYMBOL_CLOSE);
    lv_label_set_text(s_otaInfo, message && message[0] ? message : "The upload did not complete");
    lv_label_set_text(s_otaNote, "The current firmware keeps running.");
  }
}

}  // namespace deck
