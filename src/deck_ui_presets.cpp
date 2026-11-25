// Deck UI: light presets, round keys on a light's tile and card that turn it on
// in a colour or a white shade; a ring marks the one the light shows now.
#include "deck_ui_int.h"

namespace deckui {

constexpr uint32_t PICK_MS = 3000;  // a tapped key keeps its ring this long before HA decides

bool hasPresets(const TileDef& d) {
  return !d.presets.empty() && (d.type == TILE_TOGGLE || d.type == TILE_SLIDER);
}

// Approximate colour of a white shade (Tanner Helland's fit), for the key's fill.
static uint32_t kelvinRgb(int k) {
  const float t = k / 100.0f;
  float r, g, b;
  if (t <= 66) {
    r = 255;
    g = 99.4708025861f * logf(t) - 161.1195681661f;
  } else {
    r = 329.698727446f * powf(t - 60, -0.1332047592f);
    g = 288.1221695283f * powf(t - 60, -0.0755148492f);
  }
  b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231f * logf(t - 10) - 305.0447927307f;
  auto c = [](float v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : lroundf(v)); };
  return c(r) << 16 | c(g) << 8 | c(b);
}

static uint32_t presetRgb(const LightPreset& p) { return p.kelvin ? kelvinRgb(p.kelvin) : p.rgb; }

static void hueSat(uint32_t rgb, float& h, float& s) {
  const float r = (rgb >> 16 & 255) / 255.0f, g = (rgb >> 8 & 255) / 255.0f, b = (rgb & 255) / 255.0f;
  const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
  s = mx > 0 ? d / mx : 0;
  if (d <= 0) h = 0;
  else if (mx == r) h = 60 * fmodf((g - b) / d + 6, 6);
  else if (mx == g) h = 60 * ((b - r) / d + 2);
  else h = 60 * ((r - g) / d + 4);
}

// Hue and saturation only: HA reports rgb_color at full value, and xy lights
// come back a little off what was sent.
static bool sameColour(uint32_t a, uint32_t b) {
  float ha, sa, hb, sb;
  hueSat(a, ha, sa);
  hueSat(b, hb, sb);
  if (sa < 0.12f && sb < 0.12f) return true;
  if (fabsf(sa - sb) > 0.15f) return false;
  float dh = fabsf(ha - hb);
  if (dh > 180) dh = 360 - dh;
  return dh <= 12;
}

// The key last tapped, ringed until PICK_MS passes.
struct Tapped {
  String entity;
  int8_t idx = -1;
  uint32_t at = 0;
};
static Tapped s_tapped;
static lv_timer_t* s_tappedTimer = nullptr;

// Which key matches the light now, -1 for none. color_temp_kelvin is only
// in the state while the light is in white mode, so it tells the two apart.
static int presetIndex(const TileDef& d, const HaEntity* e) {
  const int n = (int)d.presets.size();
  if (s_tapped.idx >= 0 && s_tapped.idx < n && s_tapped.entity == d.entity && millis() - s_tapped.at < PICK_MS)
    return s_tapped.idx;
  if (!e || !e->known || e->state != "on") return -1;
  for (int i = 0; i < n; i++) {
    const LightPreset& p = d.presets[i];
    if (p.kelvin) {
      if (e->kelvin >= 0) {
        int k = p.kelvin;
        if (e->kelvinMin > 0 && k < e->kelvinMin) k = e->kelvinMin;
        if (e->kelvinMax > 0 && k > e->kelvinMax) k = e->kelvinMax;
        if (abs(e->kelvin - k) <= std::max(50, k / 25)) return i;
      } else if (e->rgb >= 0 && sameColour(kelvinRgb(p.kelvin), (uint32_t)e->rgb)) {
        return i;  // a colour-only light makes the shade from RGB
      }
    } else if (e->kelvin < 0 && e->rgb >= 0 && sameColour(p.rgb, (uint32_t)e->rgb)) {
      return i;
    }
  }
  return -1;
}

static void ringButtons(lv_obj_t* const* btns, int n, int active, lv_color_t ring) {
  for (int i = 0; i < n; i++) {
    if (!btns[i]) continue;
    lv_obj_set_style_outline_color(btns[i], ring, 0);
    lv_obj_set_style_outline_width(btns[i], i == active ? 2 : 0, 0);
  }
}

void applyPresets(TileRt& r, const HaEntity& e) {
  const int n = (int)r.def->presets.size();
  ringButtons(r.presetBtns, n, presetIndex(*r.def, &e), lv_obj_get_style_text_color(r.obj, LV_PART_MAIN));
}

static void refreshEntity(const String& entity) {
  HaEntity e;
  const bool known = haGetEntity(entity, e);
  for (TileRt& r : s_tiles)
    if (r.presetBtns[0] && r.def->entity == entity) {
      if (known) applyPresets(r, e);
      markPageStale(r.page);
    }
  refreshModal();
}

static void tappedEnd(lv_timer_t* t) {
  lv_timer_pause(t);
  const String entity = s_tapped.entity;
  s_tapped = Tapped();
  refreshEntity(entity);
}

bool presetPress(const TileDef& d, int i) {
  if (i < 0 || i >= (int)d.presets.size()) return false;
  if (haLink() != HA_CONNECTED) {
    deckToastShow("Home Assistant is offline", 1800);
    return false;
  }
  const LightPreset& p = d.presets[i];
  char data[48];
  if (p.kelvin) snprintf(data, sizeof data, "{\"color_temp_kelvin\":%u}", (unsigned)p.kelvin);
  else snprintf(data, sizeof data, "{\"rgb_color\":[%u,%u,%u]}", (unsigned)(p.rgb >> 16 & 255),
                (unsigned)(p.rgb >> 8 & 255), (unsigned)(p.rgb & 255));
  if (!haCallService("light", "turn_on", d.entity, data)) {
    deckToastShow("Home Assistant is busy, try again", 1800);
    return false;
  }
  const String old = s_tapped.entity;
  s_tapped.entity = d.entity;
  s_tapped.idx = (int8_t)i;
  s_tapped.at = millis();
  if (!s_tappedTimer) s_tappedTimer = lv_timer_create(tappedEnd, PICK_MS + 20, nullptr);
  lv_timer_set_period(s_tappedTimer, PICK_MS + 20);
  lv_timer_reset(s_tappedTimer);
  lv_timer_resume(s_tappedTimer);
  if (old.length() && old != d.entity) refreshEntity(old);
  refreshEntity(d.entity);
  return true;
}

static lv_obj_t* presetButton(lv_obj_t* parent, const LightPreset& p) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(presetRgb(p)), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  // A hairline keeps dark colours visible on a dark tile.
  lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_outline_pad(b, 2, 0);
  lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_PRESSED);
  return b;
}

static void presetEvent(lv_event_t* e) {
  const size_t tag = (size_t)(uintptr_t)lv_event_get_user_data(e), idx = tag / MAX_LIGHT_PRESETS;
  if (idx >= s_tiles.size()) return;
  hapticTap();
  audioClick();
  const int preset = (int)(tag % MAX_LIGHT_PRESETS);
  tileGuard(s_tiles[idx], [preset](TileRt& r) { presetPress(*r.def, preset); });
}

void makePresetButtons(TileRt& r, size_t idx) {
  for (size_t i = 0; i < r.def->presets.size(); i++) {
    r.presetBtns[i] = presetButton(r.obj, r.def->presets[i]);
    lv_obj_add_event_cb(r.presetBtns[i], presetEvent, LV_EVENT_SHORT_CLICKED,
                        (void*)(uintptr_t)(idx * MAX_LIGHT_PRESETS + i));
  }
}

// Keys in a row under the name on taller tiles, beside it on wide short ones,
// none on small tiles. The ring draws in the tile's padding.
void fitPresets(TileRt& r, int cw, int ch) {
  const int n = (int)r.def->presets.size();
  int boxW = cw, boxH = ch, d = 0, gap = 12, x0 = 0, y0 = 0;
  if (ch >= 100 && cw >= 90 && (cw - (n - 1) * gap) / n >= 24) {
    d = std::min(constrain(ch * 2 / 5, 28, 44), (cw - (n - 1) * gap) / n);
    if (n > 1) gap = constrain((cw - n * d) / (n - 1), 12, d);
    x0 = (cw - n * d - (n - 1) * gap) / 2;
    y0 = ch - d;
    boxH = ch - d - 8;
  } else if (ch >= 28 && ch < 100) {
    // Smaller keys before the name loses its room; none below 24 px.
    d = std::min(std::min(ch, 40), (cw - 90 - (n - 1) * gap) / n);
    if (d < 24) d = 0;
    x0 = cw - n * d - (n - 1) * gap;
    y0 = (ch - d) / 2;
    boxW = d ? x0 - 10 : cw;
  }
  for (int i = 0; i < n; i++) {
    showObj(r.presetBtns[i], d > 0);
    if (!d) continue;
    lv_obj_set_size(r.presetBtns[i], d, d);
    lv_obj_set_pos(r.presetBtns[i], x0 + i * (d + gap), y0);
  }
  lv_obj_set_size(r.box, boxW, boxH);
  lv_obj_set_pos(r.box, 0, 0);
  fitButton(r, boxW, boxH);
  HaEntity e;
  if (haGetEntity(r.def->entity, e)) applyPresets(r, e);
}

// --- Detail card -----------------------------------------------------------------------
static lv_obj_t* s_cardPreset[MAX_LIGHT_PRESETS] = {};

static void onCardPreset(lv_event_t* e) {
  if (s_modalTile < 0 || s_modalTile >= (int)s_tiles.size()) return;
  hapticTap();
  presetPress(*s_tiles[s_modalTile].def, (int)(intptr_t)lv_event_get_user_data(e));
}

void presetCardBuild(lv_obj_t* c, int cw, int y, const TileDef& d) {
  constexpr int D = 48, GAP = 18;
  const int n = (int)d.presets.size(), x0 = (cw - n * D - (n - 1) * GAP) / 2;
  for (int i = 0; i < n; i++) {
    s_cardPreset[i] = presetButton(c, d.presets[i]);
    lv_obj_set_size(s_cardPreset[i], D, D);
    lv_obj_set_pos(s_cardPreset[i], x0 + i * (D + GAP), y);
    lv_obj_add_event_cb(s_cardPreset[i], onCardPreset, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

void presetCardRefresh(const TileDef& d, const HaEntity* e) {
  if (s_cardPreset[0])
    ringButtons(s_cardPreset, (int)d.presets.size(), presetIndex(d, e), lv_color_hex(C_TEXT));
}

void presetCardClose() {
  for (lv_obj_t*& b : s_cardPreset) b = nullptr;
}

}  // namespace deckui
