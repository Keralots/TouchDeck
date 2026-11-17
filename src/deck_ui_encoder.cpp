#include "board.h"

#if BOARD_HAS_ENCODER
#include "deck_ui_int.h"
#include "encoder.h"

namespace deckui {
namespace {

constexpr int POPUP_RADIUS_PCT = 19;
constexpr uint32_t POPUP_HOLD_MS = 1500;
constexpr uint32_t POPUP_IN_MS = 150, POPUP_OUT_MS = 200;
constexpr uint32_t LEVEL_RELEASE_MS = 400, KELVIN_GAP_MS = 300;
constexpr uint32_t WARM_AMBER = 0xFFB45C, COOL_WHITE = 0xC3DFFF;

struct Popup {
  lv_obj_t* root = nullptr;
  lv_obj_t* arc = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* name = nullptr;
  char valueText[24] = "";
  char nameText[128] = "", nameSource[128] = "";
  const lv_font_t* valueFont = nullptr;
  int x = 0, y = 0, dx = 0, dy = 0, textWidth = 0;
  int rotation = -1, position = -1;
  uint32_t lastInput = 0;
  int32_t progress = 0;
  bool visible = false, exiting = false;
};

struct Knob {
  Popup popup;
  String fixedId;
  bool armed = false, turned = false, ignorePress = false;
};

struct LightTurn {
  String id;
  bool level = false;
  uint32_t levelAt = 0;
  int kelvin = 0, sentKelvin = 0;
  uint32_t kelvinAt = 0, sentAt = 0;
  bool attemptedKelvin = false;
};

Knob s_knobs[ENCODER_COUNT];
String s_focus;
std::vector<LightTurn> s_turns;

lv_obj_t* popupLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, char* buffer) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_remove_style_all(label);
  lv_obj_remove_flag(label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
  lv_label_set_text_static(label, buffer);
  return label;
}

void popupCreate(Popup& p) {
  p.root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(p.root);
  lv_obj_remove_flag(p.root, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(p.root, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
  lv_obj_set_style_bg_color(p.root, lv_color_hex(0x202630), 0);
  lv_obj_set_style_bg_opa(p.root, 224, 0);
  lv_obj_set_style_radius(p.root, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(p.root, lv_color_hex(GLASS_BORDER_COLOR), 0);
  lv_obj_set_style_border_width(p.root, GLASS_BORDER_WIDTH, 0);
  lv_obj_set_style_border_opa(p.root, GLASS_BORDER_OPA, 0);
  lv_obj_set_style_border_post(p.root, true, 0);

  p.arc = lv_arc_create(p.root);
  lv_obj_remove_style_all(p.arc);
  lv_obj_remove_flag(p.arc, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_style_arc_color(p.arc, lv_color_hex(0xE6E8EC), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(p.arc, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_arc_width(p.arc, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(p.arc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(p.arc, true, LV_PART_INDICATOR);
  lv_arc_set_range(p.arc, 0, 100);
  lv_arc_set_bg_angles(p.arc, 0, 180);
  lv_arc_set_value(p.arc, 0);
  p.value = popupLabel(p.root, &deck_font_28, 0xF2F4F8, p.valueText);
  lv_label_set_long_mode(p.value, LV_LABEL_LONG_MODE_WRAP);  // messages take two lines
  p.name = popupLabel(p.root, &deck_font_14, 0xC6CCD7, p.nameText);
}

bool popupLayout(Popup& p, int position) {
  const int rotation = panelRotation();
  if (p.rotation == rotation && p.position == position) return false;
  p.rotation = rotation;
  p.position = position;
  const int w = logicalW(), h = logicalH();
  const int r = std::min(w, h) * POPUP_RADIUS_PCT / 100;
  const bool landscape = rotation & 1;
  const int offset = (landscape ? h : w) * position / 100;
  int cx, cy, angle;
  p.dx = p.dy = 0;
  // Rotations 0/2/3 are unverified on hardware.
  switch (rotation) {
    case 0: cx = w - offset; cy = h; p.dy = r; angle = 180; break;
    case 1: cx = w; cy = offset; p.dx = r; angle = 90; break;
    case 2: cx = offset; cy = 0; p.dy = -r; angle = 0; break;
    default: cx = 0; cy = h - offset; p.dx = -r; angle = 270; break;
  }
  p.x = cx - r;
  p.y = cy - r;
  lv_obj_set_pos(p.root, p.x, p.y);
  lv_obj_set_size(p.root, 2 * r, 2 * r);
  lv_obj_set_size(p.arc, 2 * r - 14, 2 * r - 14);
  lv_obj_center(p.arc);
  lv_arc_set_rotation(p.arc, angle);

  // Text box: as wide as the circle inside the arc allows at the block's farthest row.
  constexpr int VALUE_H = 34, NAME_H = 20, BLOCK_H = VALUE_H + 2 + NAME_H, EDGE = 8;
  const int inner = r - 18;  // arc outer edge r - 7, width 5, 6 px padding
  int x, y;
  if (landscape) {
    const int half = (int)sqrtf((float)(inner * inner - (BLOCK_H / 2) * (BLOCK_H / 2)));
    p.textWidth = half - EDGE;
    x = rotation == 1 ? r - half : r + EDGE;
    y = r - BLOCK_H / 2;
  } else {
    const int far = EDGE + BLOCK_H;
    const int half = (int)sqrtf((float)std::max(0, inner * inner - far * far));
    p.textWidth = 2 * half;
    x = r - half;
    y = rotation == 0 ? r - far : r + EDGE;
  }
  lv_obj_set_size(p.value, p.textWidth, VALUE_H);
  lv_obj_set_pos(p.value, x, y);
  lv_obj_set_size(p.name, p.textWidth, NAME_H);
  lv_obj_set_pos(p.name, x, y + VALUE_H + 2);
  return true;
}

void popupMotion(void* var, int32_t progress) {
  Popup& p = *static_cast<Popup*>(var);
  p.progress = progress;
  const int shift = 255 - progress;
  // Move the popup and its children without changing their layout.
  lv_obj_move_to(p.root, p.x + p.dx * shift / 255, p.y + p.dy * shift / 255);
  lv_obj_set_style_opa(p.root, (lv_opa_t)progress, 0);
}

void popupDone(lv_anim_t* animation) {
  Popup& p = *static_cast<Popup*>(lv_anim_get_user_data(animation));
  lv_obj_add_flag(p.root, LV_OBJ_FLAG_HIDDEN);
  p.visible = p.exiting = false;
}

void popupAnimate(Popup& p, bool show) {
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, &p);
  lv_anim_set_exec_cb(&animation, popupMotion);
  lv_anim_set_values(&animation, p.progress, show ? 255 : 0);
  lv_anim_set_duration(&animation, show ? POPUP_IN_MS : POPUP_OUT_MS);
  lv_anim_set_path_cb(&animation, show ? lv_anim_path_ease_out : lv_anim_path_ease_in);
  lv_anim_set_user_data(&animation, &p);
  if (!show) lv_anim_set_completed_cb(&animation, popupDone);
  p.exiting = !show;
  lv_anim_start(&animation);
}

void popupHide(Popup& p) {
  if (!p.visible) return;
  lv_anim_delete(&p, popupMotion);
  lv_obj_add_flag(p.root, LV_OBJ_FLAG_HIDDEN);
  p.progress = 0;
  p.visible = p.exiting = false;
}

int textWidth(const char* text, const lv_font_t* font) {
  lv_point_t size;
  lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
  return size.x;
}

void popupName(Popup& p, const char* name, bool relayout) {
  char source[sizeof(p.nameSource)];
  size_t len = std::min(strlen(name), sizeof(source) - 4);
  while (len && (name[len] & 0xC0) == 0x80) --len;
  memcpy(source, name, len);
  strcpy(source + len, name[len] ? "..." : "");
  if (!relayout && !strcmp(p.nameSource, source)) return;
  strcpy(p.nameSource, source);
  for (char* c = source; *c; ++c)
    if (*c == '\n' || *c == '\r') *c = ' ';
  if (textWidth(source, &deck_font_14) > p.textWidth) {
    len = strlen(source);
    do {
      do { --len; } while (len && (source[len] & 0xC0) == 0x80);
      memcpy(p.nameText, source, len);
      strcpy(p.nameText + len, "...");
    } while (len && textWidth(p.nameText, &deck_font_14) > p.textWidth);
  } else {
    strcpy(p.nameText, source);
  }
  lv_label_set_text_static(p.name, p.nameText);
}

void popupShow(Popup& p, int position, uint32_t now, const char* value, const char* name,
               int percent = -1, lv_color_t accent = lv_color_hex(WARM_AMBER)) {
  if (!p.root) popupCreate(p);
  const bool relayout = popupLayout(p, position);
  if (relayout || strcmp(p.valueText, value)) {
    const lv_font_t* font = percent >= 0 ? &deck_font_28 : &deck_font_14;
    if (percent >= 0 && textWidth(value, font) > p.textWidth) font = &deck_font_20;
    if (p.valueFont != font) {
      p.valueFont = font;
      lv_obj_set_style_text_font(p.value, font, 0);
    }
    strcpy(p.valueText, value);
    lv_label_set_text_static(p.value, p.valueText);
  }
  popupName(p, name, relayout);
  if (percent >= 0) {
    lv_obj_remove_flag(p.arc, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(p.arc, percent);
    if (!lv_color_eq(lv_obj_get_style_arc_color(p.arc, LV_PART_INDICATOR), accent))
      lv_obj_set_style_arc_color(p.arc, accent, LV_PART_INDICATOR);
  } else {
    lv_obj_add_flag(p.arc, LV_OBJ_FLAG_HIDDEN);
  }
  p.lastInput = now;
  if (!p.visible) {
    p.visible = true;
    lv_obj_remove_flag(p.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(p.root);
    lv_obj_update_layout(p.root);
    popupMotion(&p, 0);
    popupAnimate(p, true);
  } else if (p.exiting) {
    popupAnimate(p, true);
  }
}

bool lightAvailable(const String& id, HaEntity& e) {
  return haGetEntity(id, e) && e.known && e.state != "unavailable" && e.state != "unknown";
}

auto findTurn(const String& id) {
  return std::find_if(s_turns.begin(), s_turns.end(), [&](const LightTurn& t) { return t.id == id; });
}

LightTurn& lightTurn(const String& id) {
  const auto it = findTurn(id);
  if (it != s_turns.end()) return *it;
  s_turns.emplace_back();
  s_turns.back().id = id;
  return s_turns.back();
}

void sendKelvin(LightTurn& t, uint32_t now) {
  char data[40];
  snprintf(data, sizeof(data), "{\"color_temp_kelvin\":%d}", t.kelvin);
  t.sentAt = now;
  t.attemptedKelvin = true;
  if (haCallService("light", "turn_on", t.id, data)) t.sentKelvin = t.kelvin;
}

void finishLevel(const String& id) {
  HaEntity e;
  if (lightAvailable(id, e)) releaseLevel(id);
  else dropWish(id);
}

void finishTurns(uint32_t now) {
  for (LightTurn& t : s_turns) {
    if (t.level && now - t.levelAt >= LEVEL_RELEASE_MS) {
      finishLevel(t.id);
      t.level = false;
    }
    if (t.kelvin && now - t.kelvinAt >= KELVIN_GAP_MS) {
      HaEntity e;
      if (t.kelvin != t.sentKelvin && lightAvailable(t.id, e)) sendKelvin(t, now);
      t.kelvin = 0;
      t.sentKelvin = 0;
    }
    if (t.attemptedKelvin && now - t.sentAt >= KELVIN_GAP_MS) t.attemptedKelvin = false;
  }
  s_turns.erase(std::remove_if(s_turns.begin(), s_turns.end(), [](const LightTurn& t) {
    return !t.level && !t.kelvin && !t.attemptedKelvin;
  }), s_turns.end());
}

void knobAction(Knob& k, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now) {
  if (s.mode == ENCODER_FIXED && k.fixedId != s.entity) k.fixedId = s.entity;
  const String& id = s.mode == ENCODER_FIXED ? k.fixedId : s_focus;
  Popup& p = k.popup;
  const int position = s.popupY;
  if (!id.length()) {
    popupShow(p, position, now, "Tap a light tile", "");
    return;
  }
  HaEntity e;
  const bool available = lightAvailable(id, e);
  const char* name = e.name.length() ? e.name.c_str() : id.c_str();
  if (!available) {
    popupShow(p, position, now, "Unavailable", name);
    return;
  }

  char value[24];
  if (in.detents && in.turnWithKey) {
    if (e.kelvinMin <= 0 || e.kelvinMax < e.kelvinMin) {
      popupShow(p, position, now, "No white mode", name);
      return;
    }
    LightTurn& t = lightTurn(id);
    const int range = e.kelvinMax - e.kelvinMin;
    const int start = t.kelvin ? t.kelvin : e.kelvin > 0 ? e.kelvin : e.kelvinMin + range / 2;
    const int step = std::max(50, ((range + 625) / 1250) * 50);
    t.kelvin = (int)std::clamp((int64_t)start + (int64_t)step * in.detents,
                               (int64_t)e.kelvinMin, (int64_t)e.kelvinMax);
    t.kelvinAt = now;
    if (t.kelvin != t.sentKelvin && (!t.attemptedKelvin || now - t.sentAt >= KELVIN_GAP_MS))
      sendKelvin(t, now);
    const int percent = range ? (int)((int64_t)(t.kelvin - e.kelvinMin) * 100 / range) : 0;
    const lv_color_t accent = lv_color_mix(lv_color_hex(COOL_WHITE), lv_color_hex(WARM_AMBER), percent * 255 / 100);
    snprintf(value, sizeof(value), "%d K", t.kelvin);
    popupShow(p, position, now, value, name, percent, accent);
    return;
  }

  const float frac = wishFrac(id, e);
  int percent = (int)lroundf(frac * 100);
  if (click) {
    const bool on = !wishOn(id, e);
    setWish(id, false, on ? 1 : 0);
    const auto it = findTurn(id);
    if (it != s_turns.end()) {
      it->level = false;
      if (!on) it->kelvin = it->sentKelvin = 0;
    }
    percent = on ? std::max(1, (int)lroundf(wishFrac(id, e) * 100)) : 0;
  } else {
    const int next = (int)std::clamp<int64_t>(percent + (int64_t)in.detents * s.stepPct, 0, 100);
    if (next != percent || (!next && frac > 0)) {
      LightTurn& t = lightTurn(id);
      if (!next) t.kelvin = t.sentKelvin = 0;
      t.level = setWish(id, true, next / 100.0f, true);
      t.levelAt = now;
    } else {
      const auto it = findTurn(id);
      if (it != s_turns.end()) it->levelAt = now;
    }
    percent = next;
  }
  if (percent > 0) snprintf(value, sizeof(value), "%d%%", percent);
  else strcpy(value, "Off");
  popupShow(p, position, now, value, name, percent, lv_color_hex(e.rgb >= 0 ? e.rgb : WARM_AMBER));
}

}  // namespace

void encoderFocus(const String& entity) {
  if (entity.startsWith("light.") && s_focus != entity) s_focus = entity;
}

void encoderTick() {
  const uint32_t now = millis();
  const bool allowed = getScreenState() == SCREEN_DECK && !s_ed.root;
  const bool sleeping = displaySleepingForTouch() || !currentBacklightLevel() || frameActive();
  bool wake = false;
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    Knob& k = s_knobs[i];
    const EncoderInput in = encoderPoll(i);
    if (allowed && in.activity) {
      touchNoteActivity();
      wake |= sleeping;
    }
    if (!allowed || sleeping) {
      k.armed = k.turned = false;
      k.ignorePress = in.keyDown;
      encoderDiscardRemainder(i);
      popupHide(k.popup);
      continue;
    }
    if (k.ignorePress) {
      k.ignorePress = in.keyDown;
      encoderDiscardRemainder(i);
      continue;
    }
    if (in.pressed) k.armed = true;
    if (in.detents && in.turnWithKey) k.turned = true;
    const bool click = in.released && k.armed && !k.turned;
    if (!in.keyDown) k.armed = k.turned = false;
    if (in.detents || click) knobAction(k, encoderSettings[i], in, click, now);

    Popup& p = k.popup;
    if (!p.visible) continue;
    if (p.rotation != panelRotation()) {
      popupHide(p);
    } else if (in.activity) {
      p.lastInput = now;
      if (p.exiting) popupAnimate(p, true);
    } else if (!p.exiting && now - p.lastInput >= POPUP_HOLD_MS) {
      popupAnimate(p, false);
    }
  }
  if (wake) wakeDisplayFromTouch();
  finishTurns(now);
}

}  // namespace deckui

void deckEncodersApply() {
  using namespace deckui;
  for (const LightTurn& t : s_turns)
    if (t.level) finishLevel(t.id);
  s_turns.clear();
  for (Knob& k : s_knobs) {
    k.armed = k.turned = k.ignorePress = false;
    popupHide(k.popup);
  }
  haSetEntities(entityIds());
}

#endif
