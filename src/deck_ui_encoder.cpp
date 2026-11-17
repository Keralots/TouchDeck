#include "board.h"

#if BOARD_HAS_ENCODER
#include "deck_ui_int.h"
#include "encoder.h"
#include <string_view>

namespace deckui {
namespace {

constexpr int POPUP_PROGRESS = 1000;
constexpr int SLIDE_IN_MS = 150, SLIDE_OUT_MS = 200;
constexpr int DROPLET_SWELL_MS = 220, DROPLET_SHRINK_MS = 160;
constexpr int CONTENT_IN_MS = 120, CONTENT_OUT_MS = 80, ARC_MS = 120;
constexpr uint32_t PREVIEW_STEP_MS = 200;
constexpr int PREVIEW_START = 20, PREVIEW_END = 80, PREVIEW_STEPS = 5;
constexpr uint32_t LEVEL_RELEASE_MS = 400, KELVIN_GAP_MS = 300;
constexpr uint32_t TRACK_GAP_MS = 500, CLIMATE_TEMP_MS = 800, CLIMATE_MODE_MS = 1000;
constexpr uint32_t CLIMATE_SETTLE_MS = 5000;
constexpr uint32_t HA_TOAST_MS = 1800, PC_TOAST_MS = 2200;
constexpr uint32_t WARM_AMBER = 0xFFB45C;
constexpr uint32_t CLIMATE_COOL = 0x66B9FF, CLIMATE_NEUTRAL = 0xA0A6B0;

enum class TargetKind { None, Ha, PcVolume, PcMedia };

struct Target {
  TargetKind kind = TargetKind::None;
  String id;
};

struct ClimateTurn {
  float low = NAN, high = NAN;
  char mode[16] = "";
  uint32_t tempAt = 0, modeAt = 0, haVersion = 0;
  bool tempSent = false, modeSent = false;
};

enum class PopupMotion { Droplet, Slide, Fade, None };
enum class PopupRun { Idle, Shape, Arc };

struct PopupFrame {
  int across = -1, along = -1, shift = 0;
  int opacity = -1, content = -1;
};

struct Popup {
  lv_obj_t* root = nullptr;
  lv_obj_t* shine = nullptr;
  lv_obj_t* arc = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* name = nullptr;
  char valueText[24] = "";
  char nameText[128] = "", nameSource[128] = "";
  const lv_font_t* valueFont = nullptr;
  int cx = 0, cy = 0, outX = 0, outY = 0, r = 0, textWidth = 0;
  int rotation = -1, position = -1, edge = -1, size = -1;
  EncoderPopupSettings settings;
  PopupMotion motion = PopupMotion::Droplet;
  PopupRun run = PopupRun::Idle;
  uint32_t runAt = 0;  // anim clock ms the running motion started at
  int runMs = 0;
  bool runEaseOut = false;
  PopupFrame frame, from;
  int duration = 0, shownValue = 0, arcFrom = 0, arcTarget = -1;
  uint32_t lastInput = 0;
  bool visible = false, exiting = false;
};

struct Knob {
  Popup popup;
  Target fixed;
  ClimateTurn climate;
  bool armed = false, turned = false, ignorePress = false;
};

struct HaTurn {
  String id;
  bool level = false;
  uint32_t levelAt = 0;
  int kelvin = 0, sentKelvin = 0;
  uint32_t kelvinAt = 0, sentAt = 0;
  bool attemptedKelvin = false;
};

struct PopupPreview {
  EncoderPopupSettings settings;
  size_t index = 0;
  uint8_t position = 0, edge = 0;
  uint32_t requestedAt = 0;
  bool pending = false, waking = false;
};

Knob s_knobs[ENCODER_COUNT];
PopupPreview s_preview;
int s_previewKnob = -1, s_previewValue = PREVIEW_START;
uint32_t s_previewAt = 0;
Target s_focus;
std::vector<HaTurn> s_turns;
uint32_t s_trackAt = 0;
bool s_trackAttempted = false;

lv_obj_t* popupLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, char* buffer) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_remove_style_all(label);
  lv_obj_remove_flag(label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);
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
  lv_obj_set_style_bg_opa(p.root, (lv_opa_t)(p.settings.opacity * 255 / 100), 0);
  lv_obj_set_style_opa(p.root, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.root, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(p.root, lv_color_hex(GLASS_BORDER_COLOR), 0);
  lv_obj_set_style_border_width(p.root, GLASS_BORDER_WIDTH, 0);
  lv_obj_set_style_border_opa(p.root, LV_OPA_30, 0);
  lv_obj_set_style_border_post(p.root, true, 0);

  p.shine = lv_obj_create(p.root);
  lv_obj_remove_style_all(p.shine);
  lv_obj_remove_flag(p.shine, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(p.shine, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_radius(p.shine, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(p.shine, lv_color_white(), 0);
  lv_obj_set_style_bg_grad_color(p.shine, lv_color_white(), 0);
  lv_obj_set_style_bg_grad_dir(p.shine, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(p.shine, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_main_opa(p.shine, 70, 0);
  lv_obj_set_style_bg_grad_opa(p.shine, LV_OPA_TRANSP, 0);
  lv_obj_set_style_opa(p.shine, LV_OPA_TRANSP, 0);

  p.arc = lv_arc_create(p.root);
  lv_obj_remove_style_all(p.arc);
  lv_obj_remove_flag(p.arc, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_add_flag(p.arc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(p.arc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_arc_color(p.arc, lv_color_hex(0xE6E8EC), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(p.arc, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_arc_width(p.arc, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(p.arc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(p.arc, lv_color_hex(WARM_AMBER), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(p.arc, true, LV_PART_INDICATOR);
  lv_arc_set_range(p.arc, 0, 100);
  lv_arc_set_bg_angles(p.arc, 0, 180);
  lv_arc_set_value(p.arc, 0);
  p.value = popupLabel(p.root, &deck_font_28, 0xF2F4F8, p.valueText);
  lv_label_set_long_mode(p.value, LV_LABEL_LONG_MODE_WRAP);  // messages take two lines
  p.name = popupLabel(p.root, &deck_font_14, 0xC6CCD7, p.nameText);
}

bool popupLayout(Popup& p, int position, int edge, int size) {
  constexpr int RADIUS_PCT[] = {15, 19, 23};
  const int rotation = panelRotation();
  if (p.rotation == rotation && p.position == position && p.edge == edge && p.size == size) return false;
  const int w = logicalW(), h = logicalH();
  const int r = std::min(w, h) * RADIUS_PCT[size] / 100;
  p.rotation = rotation;
  p.position = position;
  p.edge = edge;
  p.size = size;
  p.r = r;
  const bool landscape = rotation & 1;
  const int offset = (landscape ? h : w) * position / 100;
  const int side = edge == 0 ? 1 : -1;
  int angle;
  p.outX = p.outY = 0;
  switch (rotation) {
    case 0: p.cx = w - offset; p.outY = side; angle = 180; break;
    case 1: p.cy = offset; p.outX = side; angle = 90; break;
    case 2: p.cx = offset; p.outY = -side; angle = 0; break;
    default: p.cy = h - offset; p.outX = -side; angle = 270; break;
  }
  if (landscape) p.cx = p.outX > 0 ? w : 0;
  else p.cy = p.outY > 0 ? h : 0;
  p.frame = {};
  lv_obj_set_size(p.arc, 2 * r - 14, 2 * r - 14);
  lv_obj_set_pos(p.arc, 7 - GLASS_BORDER_WIDTH, 7 - GLASS_BORDER_WIDTH);
  lv_arc_set_rotation(p.arc, (angle + (edge ? 180 : 0)) % 360);
  const int shineW = r / 2, shineH = 22 * r / 100;
  lv_obj_set_size(p.shine, shineW, shineH);
  lv_obj_set_pos(p.shine, r - shineW / 2 - p.outX * r * 38 / 100,
                 r - shineH / 2 - (landscape ? r * 57 / 100 : p.outY * r * 80 / 100));

  // Text box: as wide as the circle inside the arc allows at the block's farthest row.
  constexpr int VALUE_H = 34, NAME_H = 20, BLOCK_H = VALUE_H + 2 + NAME_H, EDGE = 8;
  const int inner = r - 18;  // arc outer edge r - 7, width 5, 6 px padding
  int x, y;
  if (landscape) {
    const int half = (int)sqrtf((float)(inner * inner - (BLOCK_H / 2) * (BLOCK_H / 2)));
    p.textWidth = half - EDGE;
    x = p.outX > 0 ? r - half : r + EDGE;
    y = r - BLOCK_H / 2;
  } else {
    const int far = EDGE + BLOCK_H;
    const int half = (int)sqrtf((float)std::max(0, inner * inner - far * far));
    p.textWidth = 2 * half;
    x = r - half;
    y = p.outY > 0 ? r - far : r + EDGE;
  }
  lv_obj_set_size(p.value, p.textWidth, VALUE_H);
  lv_obj_set_pos(p.value, x, y);
  lv_obj_set_size(p.name, p.textWidth, NAME_H);
  lv_obj_set_pos(p.name, x, y + VALUE_H + 2);
  return true;
}

PopupMotion popupStyleMotion(const EncoderPopupSettings& s) {
  switch (s.anim) {
    case ENCODER_POPUP_SLIDE: return PopupMotion::Slide;
    case ENCODER_POPUP_FADE: return PopupMotion::Fade;
    case ENCODER_POPUP_NONE: return PopupMotion::None;
    default: return s.style == ENCODER_POPUP_DROPLET ? PopupMotion::Droplet : PopupMotion::Slide;
  }
}

int popupMix(int from, int to, int progress) {
  return (from * (POPUP_PROGRESS - progress) + to * progress + POPUP_PROGRESS / 2) / POPUP_PROGRESS;
}

int popupPhase(int elapsed, int start, int duration) {
  return std::clamp((elapsed - start * POPUP_PROGRESS) / duration, 0, POPUP_PROGRESS);
}

int popupEaseIn(int progress) {
  return progress * progress / POPUP_PROGRESS;
}

int popupEaseOut(int progress) {
  return POPUP_PROGRESS - popupEaseIn(POPUP_PROGRESS - progress);
}

int popupSlideEase(int progress, bool exiting) {
  const int x = progress * LV_BEZIER_VAL_MAX / POPUP_PROGRESS;
  const int y = exiting ? lv_cubic_bezier(x, LV_BEZIER_VAL_FLOAT(0.42), 0, LV_BEZIER_VAL_MAX, LV_BEZIER_VAL_MAX) :
                         lv_cubic_bezier(x, 0, 0, LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_MAX);
  return y * POPUP_PROGRESS / LV_BEZIER_VAL_MAX;
}

PopupFrame popupRestFrame(const Popup& p) {
  return {2 * p.r, 2 * p.r, 0, LV_OPA_COVER, LV_OPA_COVER};
}

PopupFrame popupStartFrame(const Popup& p) {
  PopupFrame f = popupRestFrame(p);
  switch (p.motion) {
    case PopupMotion::Droplet: f.across = 0; f.along = 90 * p.r / 100; f.content = 0; break;
    case PopupMotion::Slide: f.shift = p.r; f.opacity = f.content = 0; break;
    case PopupMotion::Fade: f.opacity = 0; break;
    case PopupMotion::None: break;
  }
  return f;
}

PopupFrame popupFrame(const Popup& p, int progress) {
  PopupFrame f = popupRestFrame(p);
  const PopupFrame& from = p.from;
  const int elapsed = progress * p.duration;  // Thousandths of a millisecond.
  switch (p.motion) {
    case PopupMotion::Droplet:
      if (p.exiting) {
        f.content = popupMix(from.content, 0, popupPhase(elapsed, 0, CONTENT_OUT_MS));
        const int shrink = popupEaseIn(popupPhase(elapsed, CONTENT_OUT_MS, DROPLET_SHRINK_MS));
        const int remaining = POPUP_PROGRESS - shrink;
        f.across = popupMix(0, from.across, remaining);
        f.along = popupMix(0, from.along, popupEaseIn(remaining));
      } else {
        const int swellMs = p.duration - CONTENT_IN_MS;
        if (swellMs) {
          constexpr int PEAK_MS = 143, STRETCH_MS = 195;
          const int peak = std::max(from.across, f.across * 112 / 100);
          if (elapsed < PEAK_MS * POPUP_PROGRESS) {
            f.across = popupMix(from.across, peak, popupEaseOut(popupPhase(elapsed, 0, PEAK_MS)));
          } else {
            const int t = popupPhase(elapsed, PEAK_MS, DROPLET_SWELL_MS - PEAK_MS);
            const int smooth = t * t * (3 * POPUP_PROGRESS - 2 * t) / (POPUP_PROGRESS * POPUP_PROGRESS);
            f.across = popupMix(peak, f.across, smooth);
          }
          f.along = popupMix(from.along, f.along, popupEaseOut(popupPhase(elapsed, 0, STRETCH_MS)));
        }
        f.content = popupMix(from.content, LV_OPA_COVER, popupPhase(elapsed, swellMs, CONTENT_IN_MS));
      }
      break;
    case PopupMotion::Slide: {
      const int time = p.exiting ? SLIDE_OUT_MS : SLIDE_IN_MS;
      const int slide = popupSlideEase(popupPhase(elapsed, 0, time), p.exiting);
      f.shift = popupMix(from.shift, p.exiting ? p.r : 0, slide);
      f.opacity = popupMix(from.opacity, p.exiting ? 0 : LV_OPA_COVER, slide);
      f.content = p.exiting ? 0 : popupMix(from.content, LV_OPA_COVER, popupPhase(elapsed, time, CONTENT_IN_MS));
      break;
    }
    case PopupMotion::Fade:
      f.opacity = popupMix(from.opacity, p.exiting ? 0 : LV_OPA_COVER, popupSlideEase(progress, p.exiting));
      break;
    case PopupMotion::None: break;
  }
  return f;
}

void popupSetHidden(lv_obj_t* obj, bool hidden) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void popupContent(Popup& p, int opacity) {
  const bool droplet = p.settings.style == ENCODER_POPUP_DROPLET;
  popupSetHidden(p.arc, opacity == 0 || p.arcTarget < 0);
  popupSetHidden(p.shine, opacity == 0 || !droplet);
  popupSetHidden(p.value, opacity == 0);
  popupSetHidden(p.name, opacity == 0);
  lv_obj_set_style_opa(p.arc, (lv_opa_t)opacity, 0);
  if (droplet) lv_obj_set_style_opa(p.shine, (lv_opa_t)opacity, 0);
  lv_obj_set_style_opa(p.value, (lv_opa_t)opacity, 0);
  lv_obj_set_style_opa(p.name, (lv_opa_t)opacity, 0);
}

void popupApplyFrame(Popup& p, const PopupFrame& f) {
  if (f.content == 0 && p.frame.content != 0) popupContent(p, 0);
  const bool sizeChanged = p.frame.across != f.across || p.frame.along != f.along;
  if (sizeChanged || p.frame.shift != f.shift) {
    const int w = p.outX ? f.across : f.along, h = p.outX ? f.along : f.across;
    if (sizeChanged) lv_obj_set_size(p.root, w, h);
    lv_obj_set_pos(p.root, p.cx - w / 2 + p.outX * f.shift, p.cy - h / 2 + p.outY * f.shift);
  }
  if (p.frame.opacity != f.opacity) lv_obj_set_style_opa(p.root, (lv_opa_t)f.opacity, 0);
  if (f.content && p.frame.content != f.content) popupContent(p, f.content);
  p.frame = f;
}

void popupSetArc(Popup& p, int value) {
  if (p.shownValue == value) return;
  p.shownValue = value;
  lv_arc_set_value(p.arc, value);
}

void popupPause(Popup& p) { p.run = PopupRun::Idle; }

void popupHide(Popup& p) {
  if (!p.visible) return;
  popupPause(p);
  lv_obj_add_flag(p.root, LV_OBJ_FLAG_HIDDEN);
  p.visible = p.exiting = false;
}

void popupStep(Popup& p, int progress) {
  if (p.run == PopupRun::Shape) popupApplyFrame(p, popupFrame(p, progress));
  else if (p.run == PopupRun::Arc) popupSetArc(p, popupMix(p.arcFrom, p.arcTarget, progress));
  if (progress != POPUP_PROGRESS) return;
  p.run = PopupRun::Idle;
  if (p.exiting) popupHide(p);
}

// Motions step in encoderAnimTick on the deck's animation clock, so they share
// one refresh with the page's own animation frame.
void popupRun(Popup& p, PopupRun run, int duration, bool easeOut) {
  p.run = run;
  if (!duration) {
    popupStep(p, POPUP_PROGRESS);
    return;
  }
  p.runAt = millis();
  p.runMs = duration;
  p.runEaseOut = easeOut;
}

void popupAnimate(Popup& p, bool show, bool fresh = false) {
  p.exiting = !show;
  p.from = fresh ? popupStartFrame(p) : p.frame;
  switch (p.motion) {
    case PopupMotion::Droplet:
      p.duration = show ? CONTENT_IN_MS : CONTENT_OUT_MS + DROPLET_SHRINK_MS;
      if (show && (p.from.across != 2 * p.r || p.from.along != 2 * p.r)) p.duration += DROPLET_SWELL_MS;
      break;
    case PopupMotion::Slide: p.duration = show ? SLIDE_IN_MS + CONTENT_IN_MS : SLIDE_OUT_MS; break;
    case PopupMotion::Fade: p.duration = show ? SLIDE_IN_MS : SLIDE_OUT_MS; break;
    case PopupMotion::None:
      if (show) popupApplyFrame(p, popupRestFrame(p));
      else popupHide(p);
      return;
  }
  popupApplyFrame(p, popupFrame(p, 0));
  popupRun(p, PopupRun::Shape, p.duration, false);
}

void popupAnimateArc(Popup& p) {
  if (p.run != PopupRun::Idle || p.exiting || p.arcTarget < 0 || p.arcTarget == p.shownValue) return;
  p.arcFrom = p.shownValue;
  popupRun(p, PopupRun::Arc, ARC_MS, true);
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

void popupShow(Popup& p, int position, int edge, uint32_t now, const char* value, const char* name,
               int percent = -1, int32_t entityAccent = -1,
               const EncoderPopupSettings& settings = encoderPopupSettings) {
  if (!p.root) popupCreate(p);
  if (p.visible && p.rotation != panelRotation()) popupHide(p);
  const bool fresh = !p.visible;
  if (fresh) {
    if (p.settings.opacity != settings.opacity)
      lv_obj_set_style_bg_opa(p.root, (lv_opa_t)(settings.opacity * 255 / 100), 0);
    if (p.settings.style != settings.style) {
      lv_obj_set_style_border_opa(p.root, settings.style == ENCODER_POPUP_DROPLET ? LV_OPA_30 : GLASS_BORDER_OPA, 0);
      p.frame.content = -1;
    }
    p.settings = settings;
    p.motion = popupStyleMotion(settings);
  }
  const bool relayout = popupLayout(p, position, edge, p.settings.size);
  const bool arcChanged = (percent >= 0) != (p.arcTarget >= 0);
  if (p.run == PopupRun::Arc && percent != p.arcTarget) popupPause(p);
  p.arcTarget = percent;
  if (relayout || arcChanged || strcmp(p.valueText, value)) {
    const lv_font_t* font = percent >= 0 ? &deck_font_28 : &deck_font_14;
    if (percent >= 0 && textWidth(value, font) > p.textWidth) font = &deck_font_20;
    if (font == &deck_font_20 && textWidth(value, font) > p.textWidth) font = &deck_font_14;
    if (p.valueFont != font) {
      p.valueFont = font;
      lv_obj_set_style_text_font(p.value, font, 0);
    }
    strcpy(p.valueText, value);
    lv_label_set_text_static(p.value, p.valueText);
  }
  popupName(p, name, relayout);
  if (percent >= 0) {
    const uint32_t color = p.settings.entityColor && entityAccent >= 0 ? (uint32_t)entityAccent : p.settings.accentColor;
    const lv_color_t accent = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_arc_color(p.arc, LV_PART_INDICATOR), accent))
      lv_obj_set_style_arc_color(p.arc, accent, LV_PART_INDICATOR);
  }
  p.lastInput = now;
  if (fresh) {
    popupSetArc(p, std::max(0, percent));
    popupAnimate(p, true, true);
    popupSetHidden(p.arc, p.frame.content == 0 || percent < 0);
    p.visible = true;
    lv_obj_remove_flag(p.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(p.root);
  } else {
    if (arcChanged) popupSetHidden(p.arc, p.frame.content == 0 || percent < 0);
    if (p.exiting) popupAnimate(p, true);
    popupAnimateArc(p);
  }
}

void popupPreviewStop() {
  if (s_previewKnob < 0) return;
  popupHide(s_knobs[s_previewKnob].popup);
  s_previewKnob = -1;
}

void popupPreviewTick(uint32_t now, bool allowed, bool sleeping) {
  if (!allowed || sleeping) {
    popupPreviewStop();
    if (!s_preview.pending) return;
    if (!allowed || now - s_preview.requestedAt >= 1500) {
      s_preview.pending = false;
    } else if (!s_preview.waking) {
      s_preview.waking = true;
      touchNoteActivity();
      wakeDisplayFromTouch();
    }
    return;
  }

  int value;
  if (s_preview.pending) {
    s_preview.pending = false;
    popupPreviewStop();
    s_previewKnob = s_preview.index;
    s_previewAt = now;
    value = PREVIEW_START;
    popupHide(s_knobs[s_previewKnob].popup);
    touchNoteActivity();
  } else {
    if (s_previewKnob < 0) return;
    Popup& p = s_knobs[s_previewKnob].popup;
    if (!p.visible || p.rotation != panelRotation()) {
      popupPreviewStop();
      return;
    }
    if (s_previewValue == PREVIEW_END) {
      if (!p.exiting && now - p.lastInput >= p.settings.holdMs) popupAnimate(p, false);
      if (p.visible) popupAnimateArc(p);
      return;
    }
    popupAnimateArc(p);
    // Let the opening motion finish before the first change.
    const int step = std::clamp((int)((now - s_previewAt) / PREVIEW_STEP_MS) - 1, 0, PREVIEW_STEPS);
    value = PREVIEW_START + (PREVIEW_END - PREVIEW_START) * step / PREVIEW_STEPS;
    if (value == s_previewValue) return;
  }
  s_previewValue = value;
  char text[6];
  snprintf(text, sizeof(text), "%d%%", value);
  popupShow(s_knobs[s_previewKnob].popup, s_preview.position, s_preview.edge, now,
            text, "Preview", value, -1, s_preview.settings);
}

bool haAvailable(const String& id, HaEntity& e) {
  return haGetEntity(id, e) && e.known && e.state != "unavailable" && e.state != "unknown";
}

auto findTurn(const String& id) {
  return std::find_if(s_turns.begin(), s_turns.end(), [&](const HaTurn& t) { return t.id == id; });
}

HaTurn& haTurn(const String& id) {
  const auto it = findTurn(id);
  if (it != s_turns.end()) return *it;
  s_turns.emplace_back();
  s_turns.back().id = id;
  return s_turns.back();
}

void sendKelvin(HaTurn& t, uint32_t now) {
  char data[40];
  snprintf(data, sizeof(data), "{\"color_temp_kelvin\":%d}", t.kelvin);
  t.sentAt = now;
  t.attemptedKelvin = true;
  if (haCallService("light", "turn_on", t.id, data)) t.sentKelvin = t.kelvin;
}

void finishLevel(const String& id) {
  HaEntity e;
  if (haAvailable(id, e)) releaseLevel(id);
  else dropWish(id);
}

void finishTurns(uint32_t now) {
  for (HaTurn& t : s_turns) {
    if (t.level && now - t.levelAt >= LEVEL_RELEASE_MS) {
      finishLevel(t.id);
      t.level = false;
    }
    if (t.kelvin && now - t.kelvinAt >= KELVIN_GAP_MS) {
      HaEntity e;
      if (t.kelvin != t.sentKelvin && haAvailable(t.id, e)) sendKelvin(t, now);
      t.kelvin = 0;
      t.sentKelvin = 0;
    }
    if (t.attemptedKelvin && now - t.sentAt >= KELVIN_GAP_MS) t.attemptedKelvin = false;
  }
  s_turns.erase(std::remove_if(s_turns.begin(), s_turns.end(), [](const HaTurn& t) {
    return !t.level && !t.kelvin && !t.attemptedKelvin;
  }), s_turns.end());
}

int nextPercent(int percent, int detents, int step) {
  return (int)std::clamp<int64_t>(std::max(0, percent) + (int64_t)detents * step, 0, 100);
}

void lightAction(Popup& p, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now,
                 const String& id, const HaEntity& e, const char* name) {
  const int position = s.popupY;
  char value[24];
  if (in.detents && in.turnWithKey) {
    if (e.kelvinMin <= 0 || e.kelvinMax < e.kelvinMin) {
      popupShow(p, position, s.edge, now, "No white mode", name);
      return;
    }
    HaTurn& t = haTurn(id);
    const int range = e.kelvinMax - e.kelvinMin;
    const int start = t.kelvin ? t.kelvin : e.kelvin > 0 ? e.kelvin : e.kelvinMin + range / 2;
    const int step = std::max(50, ((range + 625) / 1250) * 50);
    t.kelvin = (int)std::clamp((int64_t)start + (int64_t)step * in.detents,
                               (int64_t)e.kelvinMin, (int64_t)e.kelvinMax);
    t.kelvinAt = now;
    if (t.kelvin != t.sentKelvin && (!t.attemptedKelvin || now - t.sentAt >= KELVIN_GAP_MS))
      sendKelvin(t, now);
    const int percent = range ? (int)((int64_t)(t.kelvin - e.kelvinMin) * 100 / range) : 0;
    snprintf(value, sizeof(value), "%d K", t.kelvin);
    popupShow(p, position, s.edge, now, value, name, percent, e.rgb);
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
    const int next = nextPercent(percent, in.detents, s.stepPct);
    if (next != percent || (!next && frac > 0)) {
      HaTurn& t = haTurn(id);
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
  popupShow(p, position, s.edge, now, value, name, percent, e.rgb);
}

bool trackAllowed(uint32_t now) {
  // Both knobs share the cooldown and drop extra detents.
  if (s_trackAttempted && now - s_trackAt < TRACK_GAP_MS) return false;
  s_trackAttempted = true;
  s_trackAt = now;
  return true;
}

void volumePopup(Popup& p, const EncoderSettings& s, uint32_t now, const char* name, int percent, const char* action) {
  char value[24] = "No volume";
  if (percent >= 0) snprintf(value, sizeof(value), "%d%%", percent);
  popupShow(p, s.popupY, s.edge, now, action ? action : value, name, percent);
}

void haMediaAction(Popup& p, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now,
                   const String& id, const HaEntity& e, const char* name) {
  const float frac = wishFrac(id, e);
  int percent = frac >= 0 ? (int)lroundf(frac * 100) : -1;
  const char* action = nullptr;
  const char* service = nullptr;
  if (in.detents && in.turnWithKey) {
    if (!trackAllowed(now)) return;
    const bool next = in.detents > 0;
    service = next ? "media_next_track" : "media_previous_track";
    action = next ? "Next" : "Previous";
  } else if (click) {
    service = "media_play_pause";
    action = e.state == "playing" ? "Pause" : "Play";
  } else {
    const int next = nextPercent(percent, in.detents, s.stepPct);
    if (percent != next) {
      HaTurn& t = haTurn(id);
      t.level = setWish(id, true, next / 100.0f, true);
      t.levelAt = now;
    } else {
      const auto it = findTurn(id);
      if (it != s_turns.end()) it->levelAt = now;
    }
    percent = next;
  }
  if (service && !haCallService("media_player", service, id, ""))
    deckToastShow(haLink() == HA_CONNECTED ? "Home Assistant is busy, try again" :
                 "Home Assistant is offline", HA_TOAST_MS);
  volumePopup(p, s, now, name, percent, action);
}

void pcAction(Popup& p, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now,
              const Target& target) {
  const String& id = target.id;
  const PcSource* source = pcSource(id);
  const char* name = source && source->name[0] ? source->name : "PC";
  if (!pcIsOnline(id)) {
    popupShow(p, s.popupY, s.edge, now, "PC offline", name);
    return;
  }
  const PcState* state = pcStateOf(source);
  int want = pcVolWanted(id);
  int percent = want >= 0 ? want : state ? state->vol : -1;
  const char* cmd = nullptr;
  const char* action = nullptr;
  if (in.detents && in.turnWithKey) {
    if (!trackAllowed(now)) return;
    const bool next = in.detents > 0;
    cmd = next ? "media_next" : "media_prev";
    action = next ? "Next" : "Previous";
  } else if (click) {
    const bool media = target.kind == TargetKind::PcMedia;
    cmd = media ? "media_play_pause" : "volume_mute";
    if (media) action = state && state->media == PC_MEDIA_PLAYING ? "Pause" : "Play";
  } else {
    const int next = nextPercent(percent, in.detents, s.stepPct);
    if (next != percent) {
      pcVolumeAsk(id, next);
      want = percent = next;
    }
  }
  if (cmd) {
    String err;
    if (!pcSendCommand(id, cmd, err)) deckToastShow(err.c_str(), PC_TOAST_MS);
  }
  if (!action && want < 0 && state && state->mute == 1) action = "Muted";
  volumePopup(p, s, now, name, percent, action);
}

bool modalFor(const String& id) {
  return modalShown() && modalTileOk() && modalEntity() == id;
}

bool climateActive(const ClimateTurn& t) { return isfinite(t.low) || t.mode[0]; }

bool climateTempValid(const ClimateTurn& t, const HaEntity& e) {
  return climateHasSetpoint(e) && (climateRange(e) == isfinite(t.high));
}

bool climateTempReached(const ClimateTurn& t, const HaEntity& e) {
  const float tol = climateStep(e) / 2;
  if (!isfinite(t.high)) return fabsf(e.target - t.low) < tol;
  return fabsf(e.targetLow - t.low) < tol && fabsf(e.targetHigh - t.high) < tol;
}

void finishClimate(ClimateTurn& t, const String& id, uint32_t now) {
  if (!climateActive(t)) return;
  // Keep the sent value visible until Home Assistant reports it or it expires.
  const uint32_t tempDelay = CLIMATE_TEMP_MS + (t.tempSent ? CLIMATE_SETTLE_MS : 0);
  const uint32_t modeDelay = CLIMATE_MODE_MS + (t.modeSent ? CLIMATE_SETTLE_MS : 0);
  const bool tempDue = isfinite(t.low) && now - t.tempAt >= tempDelay;
  const bool modeDue = t.mode[0] && now - t.modeAt >= modeDelay;
  const uint32_t version = haStoreVersion();
  if (!tempDue && !modeDue && version == t.haVersion) return;
  t.haVersion = version;
  HaEntity e;
  if (!haAvailable(id, e)) {
    t = {};
    if (modalFor(id)) refreshModal();
    return;
  }

  bool changed = false;
  if (isfinite(t.low)) {
    if (!climateTempValid(t, e) || (t.tempSent && (tempDue || climateTempReached(t, e)))) {
      t.low = t.high = NAN;
      changed = true;
    } else if (tempDue) {
      t.tempSent = climateSendTemperature(id, t.low, t.high);
      if (!t.tempSent) {
        t.low = t.high = NAN;
        changed = true;
      }
    }
  }
  if (t.mode[0]) {
    if (t.modeSent && (modeDue || e.state == t.mode)) {
      t.mode[0] = '\0';
      changed = true;
    } else if (modeDue) {
      char data[40];
      snprintf(data, sizeof(data), "{\"hvac_mode\":\"%s\"}", t.mode);
      t.modeSent = haCallService("climate", "set_hvac_mode", id, data);
      if (!t.modeSent) {
        climateCallFailed();
        t.mode[0] = '\0';
        changed = true;
      }
    }
  }
  if (changed && modalFor(id)) refreshModal();
}

std::string_view nextClimateMode(std::string_view modes, std::string_view current, int detents) {
  int count = 0, index = -1;
  for (size_t from = 0, end; from < modes.size(); from = end + 1) {
    end = std::min(modes.find(',', from), modes.size());
    if (end == from) continue;
    if (modes.substr(from, end - from) == current) index = count;
    ++count;
  }
  if (!count) return {};
  if (index < 0) index = detents > 0 ? -1 : 0;
  int next = (index + detents % count + count) % count;
  for (size_t from = 0, end; from < modes.size(); from = end + 1) {
    end = std::min(modes.find(',', from), modes.size());
    if (end > from && next-- == 0) return modes.substr(from, end - from);
  }
  return {};
}

void climateAction(Knob& k, const EncoderSettings& s, const EncoderInput& in, uint32_t now,
                   const String& id, const HaEntity& e, const char* name) {
  ClimateTurn& t = k.climate;
  const bool setpoint = climateHasSetpoint(e), range = climateRange(e);
  const bool modeTurn = in.detents && in.turnWithKey;
  bool changed = false;
  if (isfinite(t.low) && (!climateTempValid(t, e) || (t.tempSent && climateTempReached(t, e)))) {
    t.low = t.high = NAN;
    changed = true;
  }
  if (t.mode[0] && t.modeSent && e.state == t.mode) {
    t.mode[0] = '\0';
    changed = true;
  }
  float low = isfinite(t.low) ? t.low : range ? e.targetLow : e.target;
  float high = isfinite(t.low) ? t.high : range ? e.targetHigh : NAN;
  if (modeTurn && e.hvacModes.length()) {
    const std::string_view current = t.mode[0] ? t.mode : e.state.length() ? e.state.c_str() : "";
    const auto next = nextClimateMode(e.hvacModes.c_str(), current, in.detents);
    if (!next.empty() && next.size() < sizeof(t.mode) &&
        next.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string_view::npos) {
      if (next != current) {
        memcpy(t.mode, next.data(), next.size());
        t.mode[next.size()] = '\0';
        t.modeSent = false;
        changed = true;
      }
      if (t.mode[0] && !t.modeSent) t.modeAt = now;
    }
  } else if (!modeTurn && in.detents && setpoint) {
    const float base = low, baseHigh = high;
    if (climateAdjust(e, in.detents, low, high) && (low != base || (range && high != baseHigh))) {
      t.low = low;
      t.high = high;
      t.tempSent = false;
      changed = true;
    }
    if (isfinite(t.low) && !t.tempSent) t.tempAt = now;
  }
  if (changed && modalFor(id)) refreshModal();

  const char* mode = modeTurn && t.mode[0] ? t.mode : e.state.length() ? e.state.c_str() : "";
  char value[24];
  int percent = -1;
  if (setpoint) {
    float min = isfinite(e.tempMin) ? e.tempMin : 7.0f, max = isfinite(e.tempMax) ? e.tempMax : 35.0f;
    if (max <= min) { min = 7.0f; max = 35.0f; }
    const float pos = range ? (low + high) / 2 : low;
    percent = (int)lroundf(std::clamp((pos - min) / (max - min), 0.0f, 1.0f) * 100);
  }
  if (modeTurn || !setpoint) {
    climateWord(mode, value, sizeof(value));
  } else if (range) {
    char a[10], b[10];
    formatReading(low, a, sizeof(a));
    formatReading(high, b, sizeof(b));
    snprintf(value, sizeof(value), "%s-%s\xC2\xB0", a, b);
  } else {
    formatReading(low, value, sizeof(value) - 2);
    strcat(value, "\xC2\xB0");
  }
  const bool reported = e.state == mode;
  const uint32_t accent = !strcmp(mode, "heat") || (reported && e.hvacAction == "heating") ? WARM_AMBER :
                          !strcmp(mode, "cool") || (reported && e.hvacAction == "cooling") ? CLIMATE_COOL :
                          CLIMATE_NEUTRAL;
  popupShow(k.popup, s.popupY, s.edge, now, value, name, percent, accent);
}

const Target& knobTarget(const Knob& k, const EncoderSettings& s) {
  return s.mode == ENCODER_FOLLOW ? s_focus : k.fixed;
}

void knobAction(Knob& k, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now) {
  if (s.mode != ENCODER_FOLLOW) {
    const bool fixedHa = s.mode == ENCODER_FIXED && encoderHaEntity(s.entity);
    const char* id = fixedHa ? s.entity : "";
    const TargetKind kind = fixedHa ? TargetKind::Ha : s.mode == ENCODER_PC ? TargetKind::PcVolume : TargetKind::None;
    if (k.fixed.kind != kind || k.fixed.id != id) {
      k.climate = {};
      k.fixed.kind = kind;
      k.fixed.id = id;
    }
  }
  const Target& target = knobTarget(k, s);
  if (target.kind == TargetKind::None) {
    popupShow(k.popup, s.popupY, s.edge, now, "Tap a tile", "");
    return;
  }
  if (target.kind != TargetKind::Ha) {
    pcAction(k.popup, s, in, click, now, target);
    return;
  }
  const String& id = target.id;
  HaEntity e;
  const bool available = haAvailable(id, e);
  const char* name = e.name.length() ? e.name.c_str() : id.c_str();
  if (!available) {
    char value[24] = "Unavailable";
    if (id.startsWith("climate.") && e.known && e.state.length()) climateWord(e.state.c_str(), value, sizeof(value));
    popupShow(k.popup, s.popupY, s.edge, now, value, name);
  } else if (id.startsWith("light.")) lightAction(k.popup, s, in, click, now, id, e, name);
  else if (id.startsWith("media_player.")) haMediaAction(k.popup, s, in, click, now, id, e, name);
  else climateAction(k, s, in, now, id, e, name);
}

}  // namespace

void encoderFocus(const TileRt& r) {
  const TileDef& d = *r.def;
  TargetKind kind;
  const String* id;
  if (d.pcVolume || (d.type == TILE_MEDIA && !r.mediaHa)) {
    kind = d.pcVolume ? TargetKind::PcVolume : TargetKind::PcMedia;
    id = &tilePcId(d);
  } else if (encoderHaEntity(d.entity.c_str())) {
    kind = TargetKind::Ha;
    id = &d.entity;
  } else {
    return;
  }
  if (s_focus.kind == kind && s_focus.id == *id) return;

  bool refresh = false;
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    if (encoderSettings[i].mode != ENCODER_FOLLOW) continue;
    Knob& k = s_knobs[i];
    ClimateTurn& t = k.climate;
    if (climateActive(t)) {
      // Send what the knob set on the old target instead of dropping it.
      const uint32_t now = millis();
      if (isfinite(t.low) && !t.tempSent) t.tempAt = now - CLIMATE_TEMP_MS;
      if (t.mode[0] && !t.modeSent) t.modeAt = now - CLIMATE_MODE_MS;
      finishClimate(t, s_focus.id, now);
    }
    refresh |= climateActive(t) && modalFor(s_focus.id);
    t = {};
  }
  s_focus.kind = kind;
  s_focus.id = *id;
  if (refresh) refreshModal();
}

void encoderClimatePreview(const String& entity, float& low, float& high, const char*& mode) {
  const ClimateTurn* temp = nullptr;
  const ClimateTurn* hvac = nullptr;
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    const Knob& k = s_knobs[i];
    const Target& target = knobTarget(k, encoderSettings[i]);
    if (target.kind != TargetKind::Ha || target.id != entity) continue;
    const ClimateTurn& t = k.climate;
    if (isfinite(t.low) && (!temp || (int32_t)(t.tempAt - temp->tempAt) > 0)) temp = &t;
    if (t.mode[0] && (!hvac || (int32_t)(t.modeAt - hvac->modeAt) > 0)) hvac = &t;
  }
  if (temp) {
    low = temp->low;
    high = temp->high;
  }
  if (hvac) mode = hvac->mode;
}

void encoderAnimTick() {
  if (!(advSettings.charts60 ? s_animFastDue : s_animDue)) return;
  bool moved = false;
  for (Knob& k : s_knobs) {
    Popup& p = k.popup;
    if (p.run == PopupRun::Idle) continue;
    const int32_t elapsed = (int32_t)(s_animMs - p.runAt);
    int progress = elapsed <= 0 ? 0 : elapsed >= p.runMs ? POPUP_PROGRESS : elapsed * POPUP_PROGRESS / p.runMs;
    if (p.runEaseOut) progress = popupEaseOut(progress);
    popupStep(p, progress);
    moved = true;
  }
  if (!moved) return;
  s_sparkPaintMs = millis();  // like a chart glide: other pages snapshot rarely meanwhile
  s_animPainted = true;
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
    if (in.detents || click) {
      popupPreviewStop();
      knobAction(k, encoderSettings[i], in, click, now);
    }

    Popup& p = k.popup;
    if (!p.visible || s_previewKnob == (int)i) continue;
    if (p.rotation != panelRotation()) {
      popupHide(p);
    } else if (in.activity) {
      p.lastInput = now;
      if (p.exiting) popupAnimate(p, true);
    } else if (!p.exiting && now - p.lastInput >= p.settings.holdMs) {
      popupAnimate(p, false);
    }
    if (p.visible) popupAnimateArc(p);
  }
  if (wake) wakeDisplayFromTouch();
  popupPreviewTick(now, allowed, sleeping);
  finishTurns(now);
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    Knob& k = s_knobs[i];
    finishClimate(k.climate, knobTarget(k, encoderSettings[i]).id, now);
  }
}

}  // namespace deckui

void deckEncoderPreview(size_t index, uint8_t position, uint8_t edge, const EncoderPopupSettings& settings) {
  using namespace deckui;
  s_preview = {settings, index, position, edge, millis(), true, false};
}

void deckEncodersApply() {
  using namespace deckui;
  s_preview.pending = false;
  popupPreviewStop();
  for (const HaTurn& t : s_turns)
    if (t.level) finishLevel(t.id);
  s_turns.clear();
  bool refresh = false;
  for (Knob& k : s_knobs) {
    refresh |= climateActive(k.climate);
    k.climate = {};
    k.fixed = {};
    k.armed = k.turned = k.ignorePress = false;
    popupHide(k.popup);
    k.popup.rotation = k.popup.position = k.popup.edge = k.popup.size = -1;
  }
  if (refresh && modalShown()) refreshModal();
  haSetEntities(entityIds());
}

#endif
