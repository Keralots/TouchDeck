// Deck UI: the quick panel and the frosted glass behind it.
#include "deck_ui_int.h"
#include "agent.h"

namespace deckui {

// --- Quick panel ------------------------------------------------------------------------
// Pulled down from the top edge. Lives on LVGL's top layer so page images
// never include it. The container is QP_LIP rows taller than what shows and
// sits that much above the screen edge when open, which hides its top
// rounded corners; the compositor skips those rows.
constexpr int QP_H_BASE = 196;
constexpr int QP_ROW = 40;
constexpr int QP_LIP = 20;
constexpr uint32_t C_SCREEN_ACCENT = 0xF6C56B;
constexpr uint32_t C_LIGHT_ACCENT = 0xFFB35C;
constexpr uint32_t C_VOLUME_ACCENT = 0x7CC8FF;

// Top bar (page name, WiFi, clock) shown; owner setting, NVS "deckui"/"hdr".
bool s_header = true;

QuickPanel s_qp = {};
uint32_t s_frostGen = 0;  // see PanelFrost

void qpMarkStale() { s_qp.stale = true; }
void qpMarkPageStale() {
  s_qp.stale = true;
  s_frostGen++;
}

uint8_t& screenLevelRef() {
  if (nightWakeActive()) return presenceSettings.nightWakeLevel;
  return nightBrightnessActive() ? backlightSettings.nightBrightness : brightness;
}

bool ledUsable() { return ledSettings.enabled && ledPinValid(); }

// The Volume row: the speaker is on, or the AI agent talks through it.
bool qpWantVolume() { return audioSettings.enabled || agentAvailable(); }

int qpH() { return QP_H_BASE + (s_qp.volSlider ? QP_ROW : 0); }

uint8_t& lightLevelRef() {
  return ledNightActive() ? ledSettings.nightBrightness : ledSettings.brightness;
}

void qpSyncValues() {
  if (!s_qp.box) return;
  lv_label_set_text(s_qp.screenLbl, nightWakeActive() || nightBrightnessActive() ? "Screen (night)" : "Screen");
  lv_slider_set_value(s_qp.screenSlider, screenLevelRef(), LV_ANIM_OFF);
  lv_label_set_text_fmt(s_qp.screenVal, "%d%%", (screenLevelRef() * 100 + 127) / 255);
  lv_label_set_text(s_qp.lightLbl, ledNightActive() ? "Light (night)" : "Light");
  if (ledUsable()) {
    lv_obj_remove_state(s_qp.lightSlider, LV_STATE_DISABLED);
    const uint8_t lv = ledNightActive() ? ledSettings.nightBrightness : ledDayLevel();
    lv_slider_set_value(s_qp.lightSlider, lv, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_qp.lightVal, "%d%%", (lv * 100 + 127) / 255);
  } else {
    lv_obj_add_state(s_qp.lightSlider, LV_STATE_DISABLED);
    lv_slider_set_value(s_qp.lightSlider, 0, LV_ANIM_OFF);
    lv_label_set_text(s_qp.lightVal, "off");
  }
  if (s_qp.volSlider) {
    lv_slider_set_value(s_qp.volSlider, audioSettings.volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_qp.volVal, "%d%%", audioSettings.volume);
  }
  if (manualNightActive()) lv_obj_add_state(s_qp.nightBtn, LV_STATE_CHECKED);
  else lv_obj_remove_state(s_qp.nightBtn, LV_STATE_CHECKED);
  qpMarkStale();
}

void setChip(lv_obj_t* chip, const String& text, bool good) {
  lv_obj_t* lbl = lv_obj_get_child(chip, 0);
  if (strcmp(lv_label_get_text(lbl), text.c_str()) == 0) return;
  lv_label_set_text(lbl, text.c_str());
  lv_obj_set_style_text_color(lbl, lv_color_hex(good ? C_OK : C_CLOCK), 0);
  qpMarkStale();
}

// Link status, every 2 s. RSSI moves in 5 dB steps and only once the signal
// leaves the shown step: each change re-takes the panel image (~150 ms).
void qpUpdateStatus(bool force) {
  if (!s_qp.box) return;
  const uint32_t now = millis();
  if (!force && now - s_qp.lastStatus < 2000) return;
  s_qp.lastStatus = now;
  const bool wifi = WiFi.status() == WL_CONNECTED;
  const int raw = wifi ? WiFi.RSSI() : 0;
  if (!s_qp.rssi || abs(raw - s_qp.rssi) > 5) s_qp.rssi = (raw / 5) * 5;
  const int rssi = s_qp.rssi;
  setChip(s_qp.wifi, wifi ? String(LV_SYMBOL_WIFI " ") + rssi + " dBm" : String(LV_SYMBOL_WIFI " offline"), wifi);
  const int pcs = pcOnlineCount();
  setChip(s_qp.pc, pcs > 1 ? String(pcs) + " PCs online" : pcs ? String("PC online") : String("PC offline"), pcs > 0);
  const HaLink hl = haLink();
  setChip(s_qp.ha, String("HA ") + haLinkName(hl), hl == HA_CONNECTED);
  setChip(s_qp.ip, wifi ? WiFi.localIP().toString() : String("-"), wifi);
}

lv_obj_t* qpChip(lv_obj_t* parent) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CHIP), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(c, 10, 0);
  lv_obj_set_style_pad_ver(c, 5, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* l = lv_label_create(c);
  lv_label_set_text(l, "");
  return c;
}

lv_obj_t* qpSlider(lv_obj_t* parent, int x, int y, int w, uint32_t accent) {
  lv_obj_t* sl = lv_slider_create(parent);
  lv_obj_set_pos(sl, x, y);
  lv_obj_set_size(sl, w, 8);
  lv_slider_set_range(sl, 5, 255);
  lv_obj_set_style_bg_color(sl, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, lv_color_hex(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sl, lv_color_hex(C_DIM), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(sl, LV_OPA_40, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_DISABLED);
  // A finger is wider than the 8 px track.
  lv_obj_set_ext_click_area(sl, 16);
  return sl;
}

void onScreenSlider(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    screenLevelRef() = (uint8_t)lv_slider_get_value(s_qp.screenSlider);
    lv_label_set_text_fmt(s_qp.screenVal, "%d%%", (screenLevelRef() * 100 + 127) / 255);
    refreshBacklightControl();
    qpMarkStale();
  } else if (code == LV_EVENT_RELEASED) {
    saveSettings();
  }
}

void onLightSlider(lv_event_t* e) {
  if (!ledUsable()) return;
  const lv_event_code_t code = lv_event_get_code(e);
  const uint8_t v = (uint8_t)lv_slider_get_value(s_qp.lightSlider);
  if (code == LV_EVENT_VALUE_CHANGED) {
    previewLed(true, ledSettings.pin, v);
    lv_label_set_text_fmt(s_qp.lightVal, "%d%%", (v * 100 + 127) / 255);
    qpMarkStale();
  } else if (code == LV_EVENT_RELEASED) {
    // By day a linked LED is set through its HA entity; the night level stays local.
    if (s_cfg.ledEntity.length() && !ledNightActive()) {
      if (!ledToHa(v)) deckToastShow("Home Assistant is offline", 1800);
      clearLedPreview();
      qpSyncValues();
      return;
    }
    lightLevelRef() = v;
    if (!ledNightActive()) ledSetHaLevel(LED_HA_NONE);  // drops a level set over MQTT
    saveSettings();
    clearLedPreview();
  }
}

// Live, also for an agent reply that is playing; a click previews the level.
void onVolumeSlider(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) {
    audioSettings.volume = (uint8_t)lv_slider_get_value(s_qp.volSlider);
    lv_label_set_text_fmt(s_qp.volVal, "%d%%", audioSettings.volume);
    qpMarkStale();
  } else if (code == LV_EVENT_RELEASED) {
    saveSettings();
    const AgentState st = agentState();
    String err;
    if (st == AGENT_IDLE || st == AGENT_ERROR) audioTest(SND_CLICK, audioSettings.volume, "", err);
  }
}

void onScreenOff(lv_event_t*) {
  hidePanelNow();
  setManualDisplayOff(true);  // any touch wakes it again
}

void onNight(lv_event_t*) {
  setManualNight(lv_obj_has_state(s_qp.nightBtn, LV_STATE_CHECKED));
  updateBacklightControl();  // apply now so the sliders show the right level
  qpSyncValues();
}

void onScrim(lv_event_t*) { closePanelAnimated(); }

void onHeaderBtn(lv_event_t*) { setHeader(lv_obj_has_state(s_qp.headerBtn, LV_STATE_CHECKED)); }
void onEditBtn(lv_event_t*) { edOpen(sh.active); }

lv_obj_t* qpButton(lv_obj_t* parent, int x, int y, int w, const char* text, bool checkable) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, 40);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BUTTON), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0xB3A6FF), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT_ON), LV_STATE_CHECKED);
  if (checkable) lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

// --- Frosted glass: the card and the quick panel -------------------------------------
// Glass theme: they show the screen under them, blurred much harder than the
// tiles and lightly tinted, so they stand out from the page.
constexpr int FROST_BLUR = 7;   // px at half resolution
constexpr int FROST_TINT = 40;  // % of C_PANEL over the blur

// LVGL renders straight into one of two frame buffers and swaps on flush, so
// the one it is not drawing into holds the last complete frame.
const uint16_t* lastFrame() {
  const lv_draw_buf_t* act = lv_display_get_buf_active(s_disp);
  const uint16_t* b0 = panelLvBuffer(0);
  return act && act->data == (const uint8_t*)b0 ? panelLvBuffer(1) : b0;
}

// One frosted copy, worked on by both cores (panelShare) in three steps.
struct Frost {
  const uint16_t* fb;
  int sw, shgt, x, y, w, h, rad, hw, hh;
  uint8_t* plane[3];
  uint8_t* scratch[3];  // per blur plane, then per half of the upscale
  lv_draw_buf_t* out;
  uint8_t* mem;    // planes, scratch and cols; freed by frostEnd
  uint32_t* cols;  // per output column: left sample | right sample << 12 | weight << 24
};

// x / 1600 (x <= 408000) and x / 32640 (x < 2^21) as multiplies; both checked
// exhaustively against the division over those ranges.
inline uint32_t div1600(uint32_t x) { return (uint32_t)(((uint64_t)x * 2684355u) >> 32); }
inline uint32_t div32640(uint32_t x) { return (uint32_t)(((uint64_t)x * 2105377u) >> 32) >> 4; }

// Half resolution: cheaper, and the same blur radius reaches twice as far.
void frostDown(void* ctx, int lo, int hi) {
  const Frost& f = *(const Frost*)ctx;
  for (int j = lo; j < hi; j++) {
    const uint16_t* r0 = f.fb + constrain(f.y + 2 * j, 0, f.shgt - 1) * f.sw;
    const uint16_t* r1 = f.fb + constrain(f.y + 2 * j + 1, 0, f.shgt - 1) * f.sw;
    const size_t o = (size_t)j * f.hw;
    for (int i = 0; i < f.hw; i++) {
      const int p0 = constrain(f.x + 2 * i, 0, f.sw - 1), p1 = constrain(f.x + 2 * i + 1, 0, f.sw - 1);
      const uint16_t a = r0[p0], b = r0[p1], c = r1[p0], d = r1[p1];
      f.plane[0][o + i] = (uint8_t)(((a >> 11) + (b >> 11) + (c >> 11) + (d >> 11)) * 255 / 124);
      f.plane[1][o + i] = (uint8_t)(((a >> 5 & 63) + (b >> 5 & 63) + (c >> 5 & 63) + (d >> 5 & 63)) * 255 / 252);
      f.plane[2][o + i] = (uint8_t)(((a & 31) + (b & 31) + (c & 31) + (d & 31)) * 255 / 124);
    }
  }
}

void frostBlur(void* ctx, int lo, int hi) {
  const Frost& f = *(const Frost*)ctx;
  for (int k = lo; k < hi; k++) wallBlurPlane(f.plane[k], f.scratch[k], f.hw, f.hh, FROST_BLUR);
}

// Rows lo..hi: tinted, dithered, corners cut by alpha.
void frostUp(void* ctx, int lo, int hi) {
  const Frost& f = *(const Frost*)ctx;
  const int w = f.w, h = f.h, hw = f.hw, hh = f.hh, rad = f.rad;
  if (w <= 0) return;
  // Tint and blur in 1/1600: the bilinear sum is in 16ths.
  const uint32_t tint[3] = {(uint32_t)(C_PANEL >> 16 & 0xFF) * FROST_TINT * 16, (uint32_t)(C_PANEL >> 8 & 0xFF) * FROST_TINT * 16,
                            (uint32_t)(C_PANEL & 0xFF) * FROST_TINT * 16};
  const uint32_t stride = f.out->header.stride;
  uint8_t* alpha = f.out->data + stride * h;  // RGB565A8: the A8 plane follows, half the stride
  // Bilinear between the half-resolution samples (centres at 2i + 0.5), in
  // quarters. A plane row interpolated across serves four output rows.
  uint16_t* scratch = (uint16_t*)f.scratch[lo ? 1 : 0];
  uint16_t* across[2] = {scratch, scratch + 3 * w};
  int rowOf[2] = {-1, -1};
  auto fill = [&](uint16_t* d, int j) {
    for (int k = 0; k < 3; k++) {
      const uint8_t* q = f.plane[k] + (size_t)j * hw;
      uint16_t* dk = d + k * w;
      for (int xx = 0; xx < w; xx++) {
        const uint32_t c = f.cols[xx], wx = c >> 24;
        dk[xx] = (uint16_t)(q[c & 0xFFF] * (4 - wx) + q[c >> 12 & 0xFFF] * wx);
      }
    }
  };
  for (int yy = lo; yy < hi; yy++) {
    uint16_t* row = (uint16_t*)(f.out->data + yy * stride);
    uint8_t* arow = alpha + yy * (stride / 2);
    const int fy = constrain(2 * yy - 1, 0, 4 * (hh - 1));
    const int j0 = fy >> 2, j1 = std::min(j0 + 1, hh - 1), wy = fy & 3;
    if (rowOf[0] != j0 && rowOf[1] == j0) {
      std::swap(across[0], across[1]);
      std::swap(rowOf[0], rowOf[1]);
    }
    if (rowOf[0] != j0) fill(across[0], rowOf[0] = j0);
    if (rowOf[1] != j1) fill(across[1], rowOf[1] = j1);
    const uint16_t* a0 = across[0];
    const uint16_t* a1 = across[1];
    // Same sums as wallDither565, divisions as multiplies.
    const uint32_t w0 = (4 - wy) * (100 - FROST_TINT), w1 = wy * (100 - FROST_TINT);
    for (int xx = 0; xx < w; xx++) {
      const uint32_t t = wallDitherT(f.x + xx, f.y + yy);
      const uint32_t r = div1600(a0[xx] * w0 + a1[xx] * w1 + tint[0]);
      const uint32_t g = div1600(a0[w + xx] * w0 + a1[w + xx] * w1 + tint[1]);
      const uint32_t b = div1600(a0[2 * w + xx] * w0 + a1[2 * w + xx] * w1 + tint[2]);
      const uint32_t r5 = std::min<uint32_t>(div32640(r * 31 * 128 + t), 31);
      const uint32_t g6 = std::min<uint32_t>(div32640(g * 63 * 128 + WALL_DITHER_ONE - t), 63);
      const uint32_t b5 = std::min<uint32_t>(div32640(b * 31 * 128 + t), 31);
      row[xx] = (uint16_t)(r5 << 11 | g6 << 5 | b5);
    }
    memset(arow, 255, w);
    const float cy = std::max(0.0f, std::max((float)rad - ((float)yy + 0.5f), (float)yy + 0.5f - (float)(h - rad)));
    if (cy <= 0) continue;
    for (int xx = 0; xx < w; xx++) {
      if (xx == rad && w - rad > xx) xx = w - rad;  // only the corners
      const float cx = std::max(0.0f, std::max((float)rad - ((float)xx + 0.5f), (float)xx + 0.5f - (float)(w - rad)));
      if (cx <= 0) continue;
      const float cov = (float)rad + 0.5f - sqrtf(cx * cx + cy * cy);
      arow[xx] = cov >= 1 ? 255 : cov <= 0 ? 0 : (uint8_t)(cov * 255);
    }
  }
}

// The last frame under (x, y, w, h), blurred and tinted, with corners of
// radius rad cut by alpha (a background image ignores the radius). Pixels off
// the screen repeat its edge. frostBegin takes the half-size planes from the
// frame (loop only); frostEnd blurs and scales up into f.out, on both cores
// (shared) or on the calling task alone.
bool frostBegin(Frost& f, int x, int y, int w, int h, int rad) {
  f = {lastFrame(), logicalW(), logicalH(), x, y, w, h, rad, (w + 1) / 2, (h + 1) / 2};
  const size_t hn = (size_t)f.hw * f.hh, sn = std::max(hn, (size_t)w * 12);
  f.mem = (uint8_t*)heap_caps_malloc(hn * 3 + sn * 3 + (size_t)w * 4, MALLOC_CAP_SPIRAM);
  f.out = f.mem ? lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565A8, 0) : nullptr;
  if (!f.out) {
    heap_caps_free(f.mem);
    f.mem = nullptr;
    return false;
  }
  for (int k = 0; k < 3; k++) {
    f.plane[k] = f.mem + k * hn;
    f.scratch[k] = f.mem + 3 * hn + k * sn;
  }
  f.cols = (uint32_t*)(f.mem + 3 * hn + 3 * sn);
  for (int xx = 0; xx < w; xx++) {
    const int fx = constrain(2 * xx - 1, 0, 4 * (f.hw - 1));
    const int i0 = fx >> 2;
    f.cols[xx] = (uint32_t)(i0 | std::min(i0 + 1, f.hw - 1) << 12 | (fx & 3) << 24);
  }
  panelShare(frostDown, &f, f.hh, 16);
  return true;
}

void frostEnd(Frost& f, bool shared) {
  if (shared) {
    panelShare(frostBlur, &f, 3, 1);
    panelShare(frostUp, &f, f.h, (f.h + 1) / 2);
  } else {
    frostBlur(&f, 0, 3);
    frostUp(&f, 0, (f.h + 1) / 2);
    frostUp(&f, (f.h + 1) / 2, f.h);
  }
  heap_caps_free(f.mem);
  f.mem = nullptr;
}

// nullptr when PSRAM is short.
lv_draw_buf_t* frostedCopy(int x, int y, int w, int h, int rad) {
  Frost f;
  if (!frostBegin(f, x, y, w, h, rad)) return nullptr;
  frostEnd(f, true);
  return f.out;
}

void frostStyle(lv_obj_t* o, lv_draw_buf_t* glass) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_image_src(o, glass, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_white(), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
}

// The panel frost runs on a worker; s_frostGen moves when the page under the
// panel changes or the panel is rebuilt, and a frost from before is dropped.
enum { FROST_READY, FROST_STARTED, FROST_WAIT };
struct PanelFrost {
  Frost f;
  volatile uint8_t state;  // 0 idle, 1 on the worker, 2 done
  uint32_t gen;
};
PanelFrost s_pf = {};

void panelFrostJob() {
  frostEnd(s_pf.f, false);
  __sync_synchronize();  // the pixels before the flag
  s_pf.state = 2;
}

void qpUseFrost(lv_draw_buf_t* g) {
  frostStyle(s_qp.box, g);
  if (s_qp.glass) lv_draw_buf_destroy(s_qp.glass);
  s_qp.glass = g;
}

// Frost the panel from the page under its open place. Only while the page
// alone is on screen; it is re-frosted when the panel goes stale.
int qpFrost() {
  const bool want = s_theme == THEME_GLASS && s_qp.box && !s_qp.open && !modalShown();
  if (s_pf.state == 1) return want ? FROST_WAIT : FROST_READY;
  if (s_pf.state == 2) {
    s_pf.state = 0;
    lv_draw_buf_t* g = s_pf.f.out;
    s_pf.f.out = nullptr;
    if (want && s_pf.gen == s_frostGen && (int)g->header.w == logicalW() && (int)g->header.h == qpH() + QP_LIP) {
      qpUseFrost(g);
      return FROST_READY;
    }
    lv_draw_buf_destroy(g);
  }
  if (!want) return FROST_READY;
  if (s_pages[sh.active].wallBake) return FROST_WAIT;  // its new glass comes first
  if (!frostBegin(s_pf.f, 0, -QP_LIP, logicalW(), qpH() + QP_LIP, 20)) return FROST_READY;
  s_pf.gen = s_frostGen;
  s_pf.state = 1;
  if (wallRunWorker(panelFrostJob, false, "deck_frost")) return FROST_STARTED;
  s_pf.state = 0;
  frostEnd(s_pf.f, true);
  qpUseFrost(s_pf.f.out);
  s_pf.f.out = nullptr;
  return FROST_READY;
}

void buildQuickPanel() {
  const int w = logicalW(), h = logicalH();
  lv_obj_t* top = lv_layer_top();

  s_qp.scrim = lv_obj_create(top);
  lv_obj_remove_style_all(s_qp.scrim);
  lv_obj_set_size(s_qp.scrim, w, h);
  lv_obj_set_style_bg_color(s_qp.scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_qp.scrim, LV_OPA_50, 0);  // = the compositor's 50 % dim
  lv_obj_add_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_qp.scrim, onScrim, LV_EVENT_CLICKED, nullptr);

  const bool vol = qpWantVolume();
  const int qh = QP_H_BASE + (vol ? QP_ROW : 0);
  lv_obj_t* box = lv_obj_create(top);
  s_qp.box = box;
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, w, qh + QP_LIP);
  lv_obj_set_pos(box, 0, -(qh + QP_LIP + 8));  // parked above the screen
  lv_obj_set_style_bg_color(box, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 20, 0);
  lv_obj_set_style_text_color(box, lv_color_hex(C_TEXT), 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  // Taps on the panel background must not fall through to the scrim.
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

  const int y0 = QP_LIP;
  lv_obj_t* chips = lv_obj_create(box);
  lv_obj_remove_style_all(chips);
  lv_obj_set_pos(chips, 12, y0 + 12);
  lv_obj_set_size(chips, w - 24, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(chips, 6, 0);
  lv_obj_set_style_pad_row(chips, 6, 0);
  lv_obj_remove_flag(chips, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  s_qp.wifi = qpChip(chips);
  s_qp.pc = qpChip(chips);
  s_qp.ha = qpChip(chips);
  s_qp.ip = qpChip(chips);

  const int labelW = 108, valW = 46;
  const int slX = 16 + labelW, slW = w - slX - valW - 22;
  s_qp.screenLbl = lv_label_create(box);
  lv_obj_set_pos(s_qp.screenLbl, 16, y0 + 58);
  s_qp.screenSlider = qpSlider(box, slX, y0 + 63, slW, C_SCREEN_ACCENT);
  s_qp.screenVal = lv_label_create(box);
  lv_obj_set_pos(s_qp.screenVal, w - valW - 10, y0 + 58);
  lv_obj_add_event_cb(s_qp.screenSlider, onScreenSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_qp.screenSlider, onScreenSlider, LV_EVENT_RELEASED, nullptr);

  s_qp.lightLbl = lv_label_create(box);
  lv_label_set_text(s_qp.lightLbl, "Light");
  lv_obj_set_pos(s_qp.lightLbl, 16, y0 + 98);
  s_qp.lightSlider = qpSlider(box, slX, y0 + 103, slW, C_LIGHT_ACCENT);
  lv_slider_set_range(s_qp.lightSlider, 0, 255);  // 0 = LED off (or the HA entity off)
  s_qp.lightVal = lv_label_create(box);
  lv_obj_set_pos(s_qp.lightVal, w - valW - 10, y0 + 98);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_qp.lightSlider, onLightSlider, LV_EVENT_RELEASED, nullptr);

  int by = y0 + 132;
  if (vol) {
    lv_obj_t* volLbl = lv_label_create(box);
    lv_label_set_text(volLbl, "Volume");
    lv_obj_set_pos(volLbl, 16, y0 + 138);
    s_qp.volSlider = qpSlider(box, slX, y0 + 143, slW, C_VOLUME_ACCENT);
    lv_slider_set_range(s_qp.volSlider, 0, 100);
    s_qp.volVal = lv_label_create(box);
    lv_obj_set_pos(s_qp.volVal, w - valW - 10, y0 + 138);
    lv_obj_add_event_cb(s_qp.volSlider, onVolumeSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_qp.volSlider, onVolumeSlider, LV_EVENT_RELEASED, nullptr);
    by += QP_ROW;
  }

  // Four buttons; on a portrait screen they carry only their symbols.
  const int bw = (w - 16 * 2 - 3 * 10) / 4;
  const bool wide = w >= 400;
  lv_obj_t* off = qpButton(box, 16, by, bw, wide ? LV_SYMBOL_POWER "  Off" : LV_SYMBOL_POWER, false);
  lv_obj_add_event_cb(off, onScreenOff, LV_EVENT_CLICKED, nullptr);
  s_qp.nightBtn = qpButton(box, 16 + bw + 10, by, bw, wide ? LV_SYMBOL_EYE_CLOSE "  Night" : LV_SYMBOL_EYE_CLOSE, true);
  lv_obj_add_event_cb(s_qp.nightBtn, onNight, LV_EVENT_VALUE_CHANGED, nullptr);
  s_qp.headerBtn = qpButton(box, 16 + 2 * (bw + 10), by, bw, wide ? LV_SYMBOL_UP "  Top bar" : LV_SYMBOL_UP, true);
  if (s_header) lv_obj_add_state(s_qp.headerBtn, LV_STATE_CHECKED);
  lv_obj_add_event_cb(s_qp.headerBtn, onHeaderBtn, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* edit = qpButton(box, 16 + 3 * (bw + 10), by, bw, wide ? LV_SYMBOL_EDIT "  Edit" : LV_SYMBOL_EDIT, false);
  lv_obj_add_event_cb(edit, onEditBtn, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* grip = lv_obj_create(box);
  lv_obj_remove_style_all(grip);
  lv_obj_set_size(grip, 36, 4);
  lv_obj_set_style_bg_color(grip, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(grip, 2, 0);
  lv_obj_align(grip, LV_ALIGN_BOTTOM_MID, 0, -7);

  s_qp.snap = lv_draw_buf_create(w, qh + QP_LIP, LV_COLOR_FORMAT_RGB565, 0);
  s_qp.open = false;
  qpUpdateStatus(true);
  qpSyncValues();
}

void destroyQuickPanel() {
  if (s_qp.scrim) lv_obj_delete(s_qp.scrim);
  if (s_qp.box) lv_obj_delete(s_qp.box);
  if (s_qp.snap) lv_draw_buf_destroy(s_qp.snap);
  if (s_qp.glass) lv_draw_buf_destroy(s_qp.glass);
  s_qp = {};
  s_frostGen++;
  portENTER_CRITICAL(&s_mux);
  sh.panelOpen = false;
  sh.panelValid = false;
  portEXIT_CRITICAL(&s_mux);
}

// Put LVGL in the open or closed state the compositor just animated to.
void showPanel(bool open) {
  if (!s_qp.box) return;
  s_qp.open = open;
  if (open) {
    lv_obj_set_y(s_qp.box, -QP_LIP);
    lv_obj_remove_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
    qpUpdateStatus(true);
    qpSyncValues();
    // Closing slides the panel off the page image, so make it current.
    markPageStale(sh.active);
  } else {
    lv_obj_set_y(s_qp.box, -(qpH() + QP_LIP + 8));
    lv_obj_add_flag(s_qp.scrim, LV_OBJ_FLAG_HIDDEN);
  }
  portENTER_CRITICAL(&s_mux);
  sh.panelOpen = open;
  portEXIT_CRITICAL(&s_mux);
  s_gen++;
}

void hidePanelNow() {
  if (s_qp.open) showPanel(false);
}

bool refreshPanelSnapshot() {
  if (!s_qp.box || !s_qp.snap || !s_qp.stale) return false;
  const int frost = qpFrost();
  if (frost != FROST_READY) return frost == FROST_STARTED;
  portENTER_CRITICAL(&s_mux);
  const bool owned = sh.owned;
  if (!owned) sh.panelBusy = true;
  portEXIT_CRITICAL(&s_mux);
  if (owned) return true;
  const uint32_t t0 = micros();
  lv_snapshot_reshape_draw_buf(s_qp.box, s_qp.snap);
  const bool ok = lv_snapshot_take_to_draw_buf(s_qp.box, LV_COLOR_FORMAT_RGB565, s_qp.snap) == LV_RESULT_OK;
  const uint32_t dt = micros() - t0;
  if (dt > s_snapUsMax) s_snapUsMax = dt;
  const uint16_t stride = (uint16_t)(s_qp.snap->header.stride / 2);
  portENTER_CRITICAL(&s_mux);
  sh.panelBusy = false;
  sh.panelValid = ok;
  if (ok) {
    sh.panelSrc = {(const uint16_t*)s_qp.snap->data + QP_LIP * stride, stride, true, (uint16_t)qpH()};
  }
  portEXIT_CRITICAL(&s_mux);
  s_qp.stale = !ok;
  s_lastSnapMs = millis();
  return true;
}

// Rebuild the closed panel when the Volume row should come or go.
void qpCheckLayout() {
  if (!s_qp.box || s_qp.open || (s_qp.volSlider != nullptr) == qpWantVolume()) return;
  portENTER_CRITICAL(&s_mux);
  const bool owned = sh.owned;
  if (!owned) sh.panelValid = false;  // no gesture may start on the old snapshot
  portEXIT_CRITICAL(&s_mux);
  if (owned) return;
  destroyQuickPanel();
  buildQuickPanel();
}

void closePanelAnimated() {
  if (!s_qp.open) return;
  Src panel, page;
  int so, ph;
  if (uiTryBeginPanel(false, panel, page, so, ph)) {
    if (compBeginPanel(panel, page, so)) {
      compRelease(0, 200, -1);
      return;
    }
    releaseSwipe();
  }
  showPanel(false);  // images not ready: close without the slide
}

}  // namespace deckui
