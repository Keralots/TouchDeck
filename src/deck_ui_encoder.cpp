#include "board.h"

#if BOARD_HAS_ENCODER
#include "deck_ui_int.h"
#include "encoder.h"
#include <esp_timer.h>
#include <math.h>
#include <string_view>

namespace deckui {
namespace {

constexpr int RADIUS_PCT[] = {15, 19, 23};
constexpr int VALUE_MS = 120;
static_assert(ENCODER_COUNT == PANEL_OVERLAY_SLOTS);
static_assert((int)PopupStyle::Droplet == ENCODER_POPUP_DROPLET &&
              (int)PopupStyle::Edge == ENCODER_POPUP_EDGE &&
              (int)PopupStyle::Bead == ENCODER_POPUP_BEAD &&
              (int)PopupStyle::Pill == ENCODER_POPUP_SIDE_PILL &&
              (int)PopupStyle::Dial == ENCODER_POPUP_DIAL &&
              (int)PopupStyle::Card == ENCODER_POPUP_CARD);
constexpr int PILL_WIDTH = 46, PILL_INSET = 4, PILL_GAP = 8;
constexpr int TAG_WIDTH = 170, TAG_PAD_X = 12, TAG_PAD_Y = 8, LABEL_GAP = 2;
constexpr uint32_t PREVIEW_STEP_MS = 200;
constexpr int PREVIEW_START = 20, PREVIEW_END = 80, PREVIEW_STEPS = 5;
constexpr uint32_t LEVEL_RELEASE_MS = 400, KELVIN_GAP_MS = 300;
constexpr uint32_t TRACK_GAP_MS = 500, CLIMATE_TEMP_MS = 800, CLIMATE_MODE_MS = 1000;
constexpr uint32_t CLIMATE_SETTLE_MS = 5000;
constexpr uint32_t DECK_SAVE_MS = 1500;
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

enum class PopupRun { Idle, Shape, Value };

struct PictureBuffer {
  uint32_t* pixels = nullptr;
  uint32_t bytes = 0;
};

struct Popup {
  lv_obj_t* root = nullptr;
  lv_obj_t* value = nullptr;
  lv_obj_t* name = nullptr;
  char valueText[24] = "";
  char nameText[128] = "", nameSource[128] = "";
  const lv_font_t* valueFont = nullptr;
  PanelOverlay overlay = {};
  PictureBuffer buffers[2];
  int slot = 0, textWidth = 0, valueWidth = 0, valueH = 0;
  bool contentDirty = false;
  int rotation = -1, position = -1, edge = -1, size = -1;
  EncoderPopupSettings settings;
  PopupRun run = PopupRun::Idle;
  uint32_t runAt = 0;
  int shownValue = -1, valueFrom = 0, valueTarget = -1;
  uint32_t lastInput = 0;
  int outlinedTile = -1;
  bool visible = false;
};

struct Knob {
  Popup popup;
  Target fixed;
  ClimateTurn climate;
  int tile = -1, pageTarget = -1;
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
lv_obj_t* s_popupScreen = nullptr;
PopupPreview s_preview;
int s_previewKnob = -1, s_previewValue = PREVIEW_START;
uint32_t s_previewAt = 0;
Target s_focus;
std::vector<HaTurn> s_turns;
uint32_t s_trackAt = 0;
bool s_trackAttempted = false;
uint32_t s_deckSettingsAt = 0;
bool s_deckSettingsDirty = false;

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
  // An inactive screen allows snapshots without invalidating the live display.
  if (!s_popupScreen) {
    s_popupScreen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_popupScreen);
  }
  p.root = lv_obj_create(s_popupScreen);
  lv_obj_remove_style_all(p.root);
  lv_obj_remove_flag(p.root, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  p.value = popupLabel(p.root, &deck_font_28, 0xF2F4F8, p.valueText);
  lv_label_set_long_mode(p.value, LV_LABEL_LONG_MODE_WRAP);  // messages take two lines
  p.name = popupLabel(p.root, &deck_font_14, 0xC6CCD7, p.nameText);
}

bool popupIsPill(const Popup& p) { return p.settings.style == ENCODER_POPUP_SIDE_PILL; }

void popupPlaceLabels(Popup& p, const FrameRect& picture, const FrameRect& value, const FrameRect& name) {
  FrameRect& old = p.overlay.picture;
  if (old.w != picture.w || old.h != picture.h || p.valueH != value.h) {
    p.overlay.pixels = nullptr;
    p.contentDirty = true;
  }
  old = picture;
  p.valueH = value.h;
  p.valueWidth = value.w;
  p.textWidth = name.w;
  lv_obj_set_size(p.root, picture.w, picture.h);
  lv_obj_set_size(p.value, value.w, value.h);
  lv_obj_set_pos(p.value, value.x, value.y);
  lv_obj_set_size(p.name, name.w, name.h);
  lv_obj_set_pos(p.name, name.x, name.y);
}

void popupLabels(Popup& p, int x, int y, int valueH, int nameH) {
  const int16_t width = p.textWidth;
  const int h = valueH + LABEL_GAP + nameH;
  popupPlaceLabels(p, {(int16_t)x, (int16_t)y, width, (int16_t)h},
                   {0, 0, width, (int16_t)valueH}, {0, (int16_t)(valueH + LABEL_GAP), width, (int16_t)nameH});
}

void popupArc(Popup& p, int d, int inset, int start, int sweep) {
  const int diameter = 2 * (d / 2 - inset);
  PopupMeter& m = p.overlay.meter;
  m.box = {(int16_t)inset, (int16_t)inset, (int16_t)diameter, (int16_t)diameter};
  m.start = start;
  m.sweep = sweep;
  m.width = 5;
  m.trackColor = lv_color_to_u16(lv_color_hex(0xE6E8EC));
  m.trackOpacity = LV_OPA_20;
}

void popupLayoutDisc(Popup& p, int r, int angle) {
  popupArc(p, 2 * r, 7, angle, 180);

  // Text box: as wide as the circle inside the arc allows at the block's farthest row.
  constexpr int VALUE_H = 34, NAME_H = 20, BLOCK_H = VALUE_H + LABEL_GAP + NAME_H, EDGE = 8;
  const int inner = r - 18;  // arc outer edge r - 7, width 5, 6 px padding
  int x, y;
  const PopupGeometry& g = p.overlay.geometry;
  if (g.outX) {
    const int half = (int)sqrtf((float)(inner * inner - (BLOCK_H / 2) * (BLOCK_H / 2)));
    p.textWidth = half - EDGE;
    x = g.outX > 0 ? r - half : r + EDGE;
    y = r - BLOCK_H / 2;
  } else {
    const int far = EDGE + BLOCK_H;
    const int half = (int)sqrtf((float)std::max(0, inner * inner - far * far));
    p.textWidth = 2 * half;
    x = r - half;
    y = g.outY > 0 ? r - far : r + EDGE;
  }
  popupLabels(p, x + GLASS_BORDER_WIDTH, y + GLASS_BORDER_WIDTH, VALUE_H, NAME_H);
}

void popupLayoutBead(Popup& p, int d) {
  constexpr int ARC_INSET = 9;
  const int valueH = lv_font_get_line_height(&deck_font_28), nameH = lv_font_get_line_height(&deck_font_14);
  popupArc(p, d, ARC_INSET, 135, 270);
  p.textWidth = d * 64 / 100;
  popupLabels(p, (d - p.textWidth) / 2,
              (d - valueH - LABEL_GAP - nameH) / 2, valueH, nameH);
}

void popupLayoutPill(Popup& p, int tagH) {
  PanelOverlay& o = p.overlay;
  const PopupGeometry& g = o.geometry;
  const bool landscape = g.outX != 0, farSide = g.outX + g.outY > 0;
  const int tagAcross = landscape ? TAG_WIDTH : tagH, tagAlong = landscape ? tagH : TAG_WIDTH;
  const int across = PILL_WIDTH + PILL_GAP + tagAcross;
  const int along = std::max(g.along * 106 / 100, tagAlong);
  const int capsuleCenter = PILL_WIDTH / 2 + (farSide ? tagAcross + PILL_GAP : 0);
  const int tagStart = farSide ? 0 : PILL_WIDTH + PILL_GAP;
  o.pillX = landscape ? capsuleCenter : along / 2;
  o.pillY = landscape ? along / 2 : capsuleCenter;
  o.layout = {0, 0, (int16_t)(landscape ? across : along), (int16_t)(landscape ? along : across)};
  const int w = landscape ? g.across : g.along, h = landscape ? g.along : g.across;
  o.meter.box = {(int16_t)(o.pillX - w / 2 + PILL_INSET), (int16_t)(o.pillY - h / 2 + PILL_INSET),
                 (int16_t)(w - 2 * PILL_INSET), (int16_t)(h - 2 * PILL_INSET)};
  o.meter.sweep = 0;
  o.tag = {(int16_t)(landscape ? tagStart : o.pillX - TAG_WIDTH / 2),
           (int16_t)(landscape ? o.pillY - tagH / 2 : tagStart), TAG_WIDTH, (int16_t)tagH};
  p.textWidth = TAG_WIDTH - 2 * TAG_PAD_X;
  popupLabels(p, o.tag.x + TAG_PAD_X, o.tag.y + TAG_PAD_Y, lv_font_get_line_height(&deck_font_28),
              lv_font_get_line_height(&deck_font_14));
}

int popupSizePx(int px, int r) { return std::max(1, (px * r + POPUP_MEDIUM_RADIUS / 2) / POPUP_MEDIUM_RADIUS); }

void popupLayoutDial(Popup& p, int r, int angle) {
  PopupGeometry& g = p.overlay.geometry;
  g.across = g.along = 2 * r;
  g.slide = r;
  PopupMeter& m = p.overlay.meter;
  m.start = angle + 10;
  m.trackColor = lv_color_to_u16(lv_color_hex(0xE6E8EC));
  m.trackOpacity = 56;
  constexpr int VALUE_H = 34, NAME_H = 20, H = VALUE_H + LABEL_GAP + NAME_H;
  p.textWidth = r * (g.outX ? 60 : 90) / 100;
  const int x = g.outX > 0 ? r - 8 - p.textWidth : g.outX < 0 ? r + 8 : r - p.textWidth / 2;
  const int y = g.outY > 0 ? r - 8 - H : g.outY < 0 ? r + 8 : r - H / 2;
  popupLabels(p, x, y, VALUE_H, NAME_H);
}

void popupLayoutCard(Popup& p, int r) {
  PanelOverlay& o = p.overlay;
  PopupGeometry& g = o.geometry;
  const int16_t w = popupSizePx(250, r), h = popupSizePx(112, r), dot = popupSizePx(28, r);
  const int16_t px = popupSizePx(14, r), py = popupSizePx(12, r), bar = popupSizePx(6, r);
  const int16_t contentW = w - 2 * px, barY = h - py - bar;
  const int16_t nameX = dot + popupSizePx(10, r), nameH = lv_font_get_line_height(&deck_font_14);
  const int16_t valueY = dot + popupSizePx(5, r), contentH = barY - py;
  g.across = g.outX ? w : h;
  g.along = g.outX ? h : w;
  g.slide = g.across + popupSizePx(16, r);
  o.cornerRadius = popupSizePx(16, r);
  o.dot = {px, py, dot, dot};
  o.meter.box = {px, barY, contentW, bar};
  o.meter.trackColor = lv_color_to_u16(lv_color_hex(0xE6E8EC));
  o.meter.trackOpacity = 46;
  popupPlaceLabels(p, {px, py, contentW, contentH}, {0, valueY, contentW, (int16_t)(contentH - valueY)},
                   {nameX, (int16_t)((dot - nameH) / 2), (int16_t)(contentW - nameX), nameH});
}

bool popupLayout(Popup& p, int position, int edge, int size) {
  const int rotation = panelRotation();
  if (p.rotation == rotation && p.position == position && p.edge == edge && p.size == size) return false;
  PopupGeometry& g = p.overlay.geometry;
  const int w = logicalW(), h = logicalH();
  const int r = std::min(w, h) * RADIUS_PCT[size] / 100;
  p.rotation = rotation;
  p.position = position;
  p.edge = edge;
  p.size = size;
  const bool landscape = rotation & 1;
  const int offset = (landscape ? h : w) * position / 100;
  const int side = edge == 0 ? 1 : -1;
  int angle;
  g.outX = g.outY = 0;
  switch (rotation) {
    case 0: g.cx = w - offset; g.outY = side; angle = 180; break;
    case 1: g.cy = offset; g.outX = side; angle = 90; break;
    case 2: g.cx = offset; g.outY = -side; angle = 0; break;
    default: g.cy = h - offset; g.outX = -side; angle = 270; break;
  }
  if (landscape) g.cx = g.outX > 0 ? w : 0;
  else g.cy = g.outY > 0 ? h : 0;
  angle = (angle + (edge ? 180 : 0)) % 360;
  p.overlay.layout = p.overlay.tag = {};
  p.overlay.dot = {};
  p.overlay.cornerRadius = 0;
  p.overlay.meter.fromEnd = false;
  p.shownValue = -1;
  int inset = 0, span = 0;
  if (g.style == PopupStyle::Dial) {
    popupLayoutDial(p, r, angle);
  } else if (g.style == PopupStyle::Card) {
    popupLayoutCard(p, r);
    inset = (g.across + (g.outX + g.outY < 0)) / 2 + popupSizePx(16, r);
    span = g.along;
  } else if (popupIsPill(p)) {
    const int tagH = 2 * TAG_PAD_Y + lv_font_get_line_height(&deck_font_28) +
                     LABEL_GAP + lv_font_get_line_height(&deck_font_14);
    g.across = PILL_WIDTH;
    g.along = 22 * r / 10;
    // In portrait, start the fill from the same end as the half-disc arc.
    p.overlay.meter.fromEnd = landscape || angle == 0;
    inset = PILL_WIDTH / 2 + 12;
    span = std::max(g.along, landscape ? tagH : TAG_WIDTH);
    g.slide = inset + PILL_WIDTH / 2 + PILL_GAP + (landscape ? TAG_WIDTH : tagH);
    popupLayoutPill(p, tagH);
  } else if (p.settings.style == ENCODER_POPUP_BEAD) {
    g.across = g.along = 17 * r / 10;
    const int round = g.outX + g.outY > 0 ? 1 : 0;
    g.beadTravel = inset = (g.across + round) / 2 + 14;
    span = g.along;
    g.slide = g.across + 14;
    popupLayoutBead(p, g.across);
  } else {
    g.across = g.along = 2 * r;
    g.slide = r;
    popupLayoutDisc(p, r, angle);
  }
  if (span) {
    // Keep the new styles on screen at positions near the ends of the edge.
    if (landscape) g.cy = std::clamp(g.cy, span / 2, h - (span + 1) / 2);
    else g.cx = std::clamp(g.cx, span / 2, w - (span + 1) / 2);
    g.cx -= g.outX * inset;
    g.cy -= g.outY * inset;
  }
  return true;
}

PopupMotion popupStyleMotion(const EncoderPopupSettings& s, bool show) {
  switch (s.anim) {
    case ENCODER_POPUP_SLIDE: return PopupMotion::Slide;
    case ENCODER_POPUP_WOBBLE:
      if (show) return PopupMotion::Wobble;
      break;
    case ENCODER_POPUP_FADE: return PopupMotion::Fade;
    case ENCODER_POPUP_NONE: return PopupMotion::None;
    case ENCODER_POPUP_SPRING: return PopupMotion::Spring;
    case ENCODER_POPUP_POP: return PopupMotion::Pop;
  }
  if (s.style == ENCODER_POPUP_DROPLET) return PopupMotion::Droplet;
  if (s.style == ENCODER_POPUP_BEAD) return PopupMotion::Drip;
  return PopupMotion::Slide;
}

bool popupSetValue(Popup& p, int value) {
  p.shownValue = value;
  const int meterValue = p.valueTarget < 0 ? -1 : std::clamp(value, 0, 100);
  if (p.overlay.meter.value == meterValue) return false;
  p.overlay.meter.value = meterValue;
  return true;
}

bool popupRefreshPicture(Popup& p) {
  if (!p.contentDirty) return false;
  for (PictureBuffer& b : p.buffers) {
    if (b.pixels && (b.pixels == p.overlay.pixels || panelOverlayPictureInUse(p.slot, b.pixels))) continue;
    const int64_t start = esp_timer_get_time();
    const FrameRect& picture = p.overlay.picture;
    const uint32_t stride = lv_draw_buf_width_to_stride(picture.w, LV_COLOR_FORMAT_ARGB8888);
    const uint32_t bytes = stride * picture.h;
    if (b.bytes != bytes) {
      free(b.pixels);
      b.pixels = (uint32_t*)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
      b.bytes = b.pixels ? bytes : 0;
    }
    lv_draw_buf_t buf;
    const bool ok = b.pixels &&
                    lv_draw_buf_init(&buf, picture.w, picture.h, LV_COLOR_FORMAT_ARGB8888, stride,
                                     b.pixels, b.bytes) == LV_RESULT_OK &&
                    lv_snapshot_take_to_draw_buf(p.root, LV_COLOR_FORMAT_ARGB8888, &buf) == LV_RESULT_OK;
    panelCountPopupRefresh(esp_timer_get_time() - start);
    if (!ok) return false;
    p.overlay.stride = buf.header.stride / 4;
    p.overlay.pixels = (const uint32_t*)buf.data;
    p.contentDirty = false;
    return true;
  }
  return false;
}

void popupReleaseSparePictures(Popup& p) {
  for (PictureBuffer& b : p.buffers) {
    if (!b.pixels || b.pixels == p.overlay.pixels || panelOverlayPictureInUse(p.slot, b.pixels)) continue;
    free(b.pixels);
    b = {};
  }
}

void clearTileOutline(Popup& p) {
  if (p.outlinedTile < 0) return;
  // destroyPages clears these indices before it deletes the tiles.
  TileRt& r = s_tiles[p.outlinedTile];
  for (auto prop : {LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_PAD, LV_STYLE_OUTLINE_COLOR, LV_STYLE_OUTLINE_OPA})
    lv_obj_remove_local_style_prop(r.obj, prop, 0);
  markPageStale(r.page);
  p.outlinedTile = -1;
}

void popupHide(Popup& p) {
  clearTileOutline(p);
  if (!p.visible) return;
  panelOverlayClear(p.slot);
  p.run = PopupRun::Idle;
  p.visible = false;
  popupReleaseSparePictures(p);
}

void popupAnimate(Popup& p, bool show, bool fresh = false) {
  const int64_t now = esp_timer_get_time();
  PanelOverlay& o = p.overlay;
  const PopupGeometry& g = o.geometry;
  PopupAnimation& a = o.animation;
  const PopupFrame last = !fresh && a.duration ? popupFrame(g, a, popupProgress(a, now)) : popupRestFrame(g);
  a = {popupStyleMotion(p.settings, show), !show, p.settings.speed, 0, now, last};
  if (fresh) a.from = popupStartFrame(g, a);
  const bool settled = a.from.across == g.across && a.from.along == g.along && !a.from.shift;
  switch (a.kind) {
    case PopupMotion::Droplet:
      a.duration = show ? CONTENT_IN_MS : CONTENT_OUT_MS + DROPLET_SHRINK_MS;
      if (show && !settled) a.duration += DROPLET_SWELL_MS;
      break;
    case PopupMotion::Drip:
      a.duration = show ? CONTENT_IN_MS : CONTENT_OUT_MS + BEAD_RETURN_MS;
      if (show && !settled) a.duration += fresh ? BEAD_DRIP_MS : BEAD_DRIP_MS - BEAD_BUMP_MS;
      break;
    case PopupMotion::Slide: a.duration = show ? SLIDE_IN_MS + CONTENT_IN_MS : SLIDE_OUT_MS; break;
    case PopupMotion::Wobble: a.duration = CONTENT_IN_MS + (settled ? 0 : WOBBLE_MS); break;
    case PopupMotion::Fade: a.duration = show ? SLIDE_IN_MS : SLIDE_OUT_MS; break;
    case PopupMotion::Spring: a.duration = show ? SPRING_MS : SPRING_MS * 7 / 10; break;
    case PopupMotion::Pop: a.duration = show ? POP_MS : POP_MS * 7 / 10; break;
    case PopupMotion::None:
      if (!show) {
        popupHide(p);
        return;
      }
      break;
  }
  p.run = a.duration ? PopupRun::Shape : PopupRun::Idle;
  panelOverlaySet(p.slot, o);
}

void popupAnimateValue(Popup& p) {
  if (p.run != PopupRun::Idle || p.overlay.animation.exiting || p.valueTarget < 0 || p.valueTarget == p.shownValue) return;
  p.valueFrom = p.shownValue;
  p.run = PopupRun::Value;
  p.runAt = millis();
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
      strcpy(source + len, "...");
    } while (len && textWidth(source, &deck_font_14) > p.textWidth);
  }
  if (!strcmp(p.nameText, source)) return;
  strcpy(p.nameText, source);
  lv_label_set_text_static(p.name, p.nameText);
  p.contentDirty = true;
}

void popupShow(Popup& p, int position, int edge, uint32_t now, const char* value, const char* name,
               int percent = -1, int32_t entityAccent = -1, int targetOn = -1,
               const EncoderPopupSettings& settings = encoderPopupSettings) {
  if (!p.root) popupCreate(p);
  if (p.visible && p.rotation != panelRotation()) popupHide(p);
  const bool fresh = !p.visible;
  if (fresh) {
    const bool styleChanged = p.settings.style != settings.style;
    const bool card = settings.style == ENCODER_POPUP_CARD;
    p.settings = settings;
    p.overlay.geometry.style = (PopupStyle)settings.style;
    p.overlay.fillOpacity = settings.opacity * 255 / 100;
    p.overlay.borderColor = lv_color_to_u16(lv_color_hex(GLASS_BORDER_COLOR));
    p.overlay.borderOpacity = settings.style == ENCODER_POPUP_EDGE ? GLASS_BORDER_OPA : LV_OPA_30;
    if (styleChanged) {
      p.size = -1;
      p.contentDirty = true;
      lv_obj_set_style_text_align(p.value, card ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_align(p.name, card ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(p.name, lv_color_hex(card ? 0xE8ECF1 : 0xC6CCD7), 0);
    }
  }
  const bool relayout = popupLayout(p, position, edge, p.settings.size);
  const bool hasValueChanged = (percent >= 0) != (p.valueTarget >= 0);
  if (p.run == PopupRun::Value && percent != p.valueTarget) p.run = PopupRun::Idle;
  p.valueTarget = percent;
  if (relayout || hasValueChanged || strcmp(p.valueText, value)) {
    const lv_font_t* font = percent >= 0 ? &deck_font_28 : &deck_font_14;
    if (p.settings.style == ENCODER_POPUP_CARD && percent >= 0 &&
        (value[strspn(value, "0123456789% .-K")] == '\0' || !strcmp(value, "Off")) &&
        lv_font_get_line_height(&lv_font_montserrat_40) <= p.valueH &&
        textWidth(value, &lv_font_montserrat_40) <= p.valueWidth) font = &lv_font_montserrat_40;
    if (percent >= 0 && textWidth(value, font) > p.valueWidth) font = &deck_font_20;
    if (font == &deck_font_20 && textWidth(value, font) > p.valueWidth) font = &deck_font_14;
    if (p.valueFont != font) {
      p.valueFont = font;
      lv_obj_set_style_text_font(p.value, font, 0);
      p.contentDirty = true;
    }
    if (strcmp(p.valueText, value)) {
      strcpy(p.valueText, value);
      lv_label_set_text_static(p.value, p.valueText);
      p.contentDirty = true;
    }
  }
  popupName(p, name, relayout);
  bool meterChanged = false;
  const bool card = p.settings.style == ENCODER_POPUP_CARD;
  if (card) {
    const bool on = targetOn < 0 ? percent > 0 : targetOn != 0;
    meterChanged = p.overlay.dotOn != on;
    p.overlay.dotOn = on;
  }
  if (percent >= 0 || card) {
    const uint32_t color = p.settings.entityColor && entityAccent >= 0 ? (uint32_t)entityAccent : p.settings.accentColor;
    const uint16_t accent = lv_color_to_u16(lv_color_hex(color));
    meterChanged |= p.overlay.meter.accent != accent;
    p.overlay.meter.accent = accent;
  }
  p.lastInput = now;
  if (fresh || relayout) meterChanged |= popupSetValue(p, std::max(0, percent));
  else if (hasValueChanged) meterChanged |= popupSetValue(p, p.shownValue);
  p.contentDirty |= !p.overlay.pixels;
  const bool pictureChanged = popupRefreshPicture(p);
  if (fresh || relayout || p.overlay.animation.exiting) {
    p.visible = true;
    popupAnimate(p, true, fresh || relayout);
  } else {
    if (pictureChanged || meterChanged) panelOverlaySet(p.slot, p.overlay);
    popupAnimateValue(p);
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
      if (!p.overlay.animation.exiting && now - p.lastInput >= p.settings.holdMs) popupAnimate(p, false);
      if (p.visible) popupAnimateValue(p);
      return;
    }
    if (p.run == PopupRun::Shape) return;
    popupAnimateValue(p);
    // Let the opening motion finish before the first change.
    const int start = std::max((int)(2 * PREVIEW_STEP_MS), p.overlay.animation.duration);
    const int elapsed = (int)(now - s_previewAt) - start + (int)PREVIEW_STEP_MS;
    const int step = std::clamp(elapsed / (int)PREVIEW_STEP_MS, 0, PREVIEW_STEPS);
    value = PREVIEW_START + (PREVIEW_END - PREVIEW_START) * step / PREVIEW_STEPS;
    if (value == s_previewValue) return;
  }
  s_previewValue = value;
  char text[6];
  snprintf(text, sizeof(text), "%d%%", value);
  popupShow(s_knobs[s_previewKnob].popup, s_preview.position, s_preview.edge, now,
            text, "Preview", value, -1, true, s_preview.settings);
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
    popupShow(p, position, s.edge, now, value, name, percent, e.rgb, wishOn(id, e));
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

void volumePopup(Popup& p, const EncoderSettings& s, uint32_t now, const char* name, int percent,
                 const char* action, int targetOn = -1) {
  char value[24] = "No volume";
  if (percent >= 0) snprintf(value, sizeof(value), "%d%%", percent);
  popupShow(p, s.popupY, s.edge, now, action ? action : value, name, percent, -1, targetOn);
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
  volumePopup(p, s, now, name, percent, action, e.state != "off" && e.state != "standby");
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
  int targetOn = state && state->mute >= 0 ? state->mute == 0 : -1;
  if (cmd) {
    String err;
    if (!pcSendCommand(id, cmd, err)) deckToastShow(err.c_str(), PC_TOAST_MS);
    else if (targetOn >= 0 && !strcmp(cmd, "volume_mute")) targetOn = !targetOn;
  }
  if (!action && want < 0 && state && state->mute == 1) action = "Muted";
  volumePopup(p, s, now, name, percent, action, targetOn);
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
  popupShow(k.popup, s.popupY, s.edge, now, value, name, percent, accent, *mode && strcmp(mode, "off"));
}

void popupItem(Popup& p, const EncoderSettings& s, uint32_t now, const char* item,
               const char* kind, int pos, int count) {
  char value[sizeof(p.valueText)], name[24];
  size_t len = strlen(item);
  if (len < sizeof(value)) {
    strcpy(value, item);
  } else {
    len = sizeof(value) - 4;
    while (len && (item[len] & 0xC0) == 0x80) --len;
    memcpy(value, item, len);
    strcpy(value + len, "...");
  }
  snprintf(name, sizeof(name), "%s %d of %d", kind, pos, count);
  popupShow(p, s.popupY, s.edge, now, value, name);
}

bool tilePickable(int tile) {
  if (tile < 0 || (size_t)tile >= s_tiles.size()) return false;
  const TileRt& r = s_tiles[tile];
  return r.page == sh.active && !r.off && !lv_obj_has_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
}

void showTileOutline(Popup& p, int tile) {
  if (p.outlinedTile == tile) return;
  clearTileOutline(p);
  for (Knob& k : s_knobs)
    if (k.popup.outlinedTile == tile) clearTileOutline(k.popup);
  TileRt& r = s_tiles[tile];
  const uint32_t accent = p.settings.entityColor ? tileAccent(*r.def) : p.settings.accentColor;
  lv_obj_set_style_outline_color(r.obj, lv_color_hex(accent), 0);
  lv_obj_set_style_outline_pad(r.obj, 2, 0);
  lv_obj_set_style_outline_opa(r.obj, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_width(r.obj, 3, 0);
  markPageStale(r.page);
  p.outlinedTile = tile;
}

void pickTile(Knob& k, const EncoderSettings& s, int detents, bool click, uint32_t now) {
  struct Pick { int tile, row, col; } picks[MAX_PAGE_TILES];
  int count = 0;
  for (int i = 0; i < (int)s_tiles.size() && count < MAX_PAGE_TILES; i++) {
    if (!tilePickable(i)) continue;
    lv_obj_t* obj = s_tiles[i].obj;
    picks[count++] = {i, lv_obj_get_style_grid_cell_row_pos(obj, LV_PART_MAIN),
                        lv_obj_get_style_grid_cell_column_pos(obj, LV_PART_MAIN)};
  }
  if (!count) {
    k.tile = -1;
    clearTileOutline(k.popup);
    popupShow(k.popup, s.popupY, s.edge, now, "No tiles", "Tile picker");
    return;
  }
  std::sort(picks, picks + count, [](const Pick& a, const Pick& b) {
    return a.row == b.row ? a.col < b.col : a.row < b.row;
  });
  const Pick* current = std::find_if(picks, picks + count, [&](const Pick& p) { return p.tile == k.tile; });
  const bool selected = current != picks + count;
  int pos = selected ? (int)(current - picks) : 0;
  if (detents) {
    if (!selected && detents > 0) pos = -1;
    pos = (pos + detents % count + count) % count;
  }
  k.tile = picks[pos].tile;
  TileRt& r = s_tiles[k.tile];
  const TileDef& d = *r.def;
  const char* label = d.label.length() ? d.label.c_str() : d.entity.length() ? d.entity.c_str() : "Tile";
  popupItem(k.popup, s, now, label, "Tile", pos + 1, count);
  showTileOutline(k.popup, k.tile);
  if (click && selected) {
    encoderFocus(r);
    hapticTap();
    audioClick();
    requestAction(r, false);
  }
}

void pagePopup(Popup& p, const EncoderSettings& s, uint32_t now) {
  if (!s_count) {
    popupShow(p, s.popupY, s.edge, now, "No pages", "Pages");
    return;
  }
  const PageDef& d = *s_pages[sh.active].def;
  popupItem(p, s, now, d.name.length() ? d.name.c_str() : d.id.c_str(), "Page", sh.active + 1, s_count);
}

void turnPage(Knob& k, int detents) {
  // A slide consumes one detent; the rest arrived while it was busy.
  if (slideBy(detents > 0 ? 1 : -1, false)) k.pageTarget = sh.target;
}

void deckLevelAction(Popup& p, const EncoderSettings& s, int detents, bool volume, uint32_t now) {
  if (volume && !qpWantVolume()) {
    popupShow(p, s.popupY, s.edge, now, "Speaker off", "Volume");
    return;
  }
  uint8_t& level = volume ? audioSettings.volume : screenLevelRef();
  const int step = volume ? s.stepPct : (s.stepPct * 255 + 50) / 100;
  const int next = (int)std::clamp<int64_t>(level + (int64_t)detents * step, volume ? 0 : 1, volume ? 100 : 255);
  if (next != level) {
    level = (uint8_t)next;
    s_deckSettingsDirty = true;
    s_deckSettingsAt = now;
    if (!volume) refreshBacklightControl();
    qpSyncValues();
  }
  volumePopup(p, s, now, volume ? "Volume" : "Screen", volume ? level : (level * 100 + 127) / 255, nullptr, true);
}

const Target& knobTarget(const Knob& k, const EncoderSettings& s) {
  return s.mode == ENCODER_FOLLOW ? s_focus : k.fixed;
}

// A followed tile that needs the PIN again; using an open one keeps it open.
bool targetLocked(const Target& target) {
  bool locked = false;
  for (TileRt& t : s_tiles) {
    const TileDef& d = *t.def;
    if (!tilePinned(d)) continue;
    const bool pcMedia = d.type == TILE_MEDIA && !t.mediaHa;
    const bool match = target.kind == TargetKind::Ha         ? d.entity == target.id
                       : target.kind == TargetKind::PcVolume ? d.pcVolume && tilePcId(d) == target.id
                       : target.kind == TargetKind::PcMedia  ? pcMedia && tilePcId(d) == target.id
                                                             : false;
    if (!match) continue;
    if (tileOpen(t)) {
      tileGuard(t, nullptr);
      return false;
    }
    locked = true;
  }
  return locked;
}

// A media tile with "knob": "tracks" for this player, else null.
TileRt* tracksTile(const String& id) {
  for (TileRt& t : s_tiles)
    if (t.def->type == TILE_MEDIA && t.def->knobTracks && t.mediaHa && t.def->entity == id) return &t;
  return nullptr;
}

void knobAction(Knob& k, const EncoderSettings& s, const EncoderInput& in, bool click, uint32_t now) {
  // Music mode while the player card is open: the first knob browses the
  // queue, the others set the player's volume.
  if (playerCardOpen()) {
    if (&k == &s_knobs[0]) {
      popupHide(k.popup);
      playerCardKnob(in.turnWithKey ? 0 : in.detents, click);
      return;
    }
    const String id = playerCardEntity();
    HaEntity e;
    if (haAvailable(id, e)) haMediaAction(k.popup, s, in, click, now, id, e, e.name.length() ? e.name.c_str() : "Volume");
    return;
  }
  switch (s.mode) {
    case ENCODER_TILES:
      if (in.detents && in.turnWithKey) turnPage(k, in.detents);
      else if (!s_qp.open && !modalShown() && !compOwnsScreen()) pickTile(k, s, in.detents, click, now);
      return;
    case ENCODER_PAGES:
      if (in.detents && in.turnWithKey) {
        deckLevelAction(k.popup, s, in.detents, false, now);
      } else if (click) {
        const int home = homePage();
        k.pageTarget = -1;
        if (s_count && sh.active != home) {
          if (compMode() != COMP_NONE) stopGestures();
          goToPage(s_pages[home].def->id);
          if (sh.active != home) k.pageTarget = home;
        }
        if (k.pageTarget < 0) pagePopup(k.popup, s, now);
      } else {
        turnPage(k, in.detents);
      }
      return;
    case ENCODER_DECK:
      if (click) {
        hidePanelNow();
        setManualDisplayOff(true);
      } else {
        deckLevelAction(k.popup, s, in.detents, in.turnWithKey, now);
      }
      return;
  }
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
  if (s.mode == ENCODER_FOLLOW && targetLocked(target)) {
    popupShow(k.popup, s.popupY, s.edge, now, "Locked", "");
    return;
  }
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
  else if (id.startsWith("media_player.")) {
    TileRt* tracks = in.detents && !in.turnWithKey ? tracksTile(id) : nullptr;
    if (tracks && !modalShown()) {
      popupHide(k.popup);
      openPlayerCard(*tracks, true);
      playerCardKnob(in.detents, false);
    } else {
      haMediaAction(k.popup, s, in, click, now, id, e, name);
    }
  }
  else climateAction(k, s, in, now, id, e, name);
}

}  // namespace

void encoderClearTiles(bool rebuild) {
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    Knob& k = s_knobs[i];
    if (k.popup.outlinedTile >= 0 || (encoderSettings[i].mode == ENCODER_TILES && s_previewKnob != (int)i))
      popupHide(k.popup);
    k.tile = -1;
    if (rebuild) k.pageTarget = -1;
  }
}

void encoderFocus(const TileRt& r) {
  const TileDef& d = *r.def;
  if (!tileOpen(r)) return;  // a knob would act past the PIN
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
  for (Knob& k : s_knobs) {
    Popup& p = k.popup;
    if (!p.visible) continue;
    bool meterChanged = false;
    if (p.run == PopupRun::Value) {
      const int elapsed = (int32_t)(s_animMs - p.runAt) * p.settings.speed / 100;
      const int t = std::clamp(elapsed, 0, VALUE_MS) * POPUP_PROGRESS / VALUE_MS;
      meterChanged = popupSetValue(p, popupMix(p.valueFrom, p.valueTarget, popupEaseOut(t)));
      if (t == POPUP_PROGRESS) p.run = PopupRun::Idle;
    }
    if (popupRefreshPicture(p) || meterChanged) panelOverlaySet(p.slot, p.overlay);
  }
}

void encoderTick() {
  const uint32_t now = millis();
  const bool allowed = getScreenState() == SCREEN_DECK && !s_ed.root;
  const bool sleeping = displaySleepingForTouch() || !currentBacklightLevel() || frameActive();
  const int64_t nowUs = esp_timer_get_time();
  bool wake = false;
  for (size_t i = 0; i < ENCODER_COUNT; i++) {
    Knob& k = s_knobs[i];
    Popup& p = k.popup;
    const EncoderSettings& s = encoderSettings[i];
    p.slot = i;
    if (p.outlinedTile >= 0 && (s_qp.open || modalShown() || !tilePickable(p.outlinedTile))) popupHide(p);
    if (p.visible && p.run == PopupRun::Shape && nowUs >= popupEndUs(p.overlay.animation)) {
      p.run = PopupRun::Idle;
      if (p.overlay.animation.exiting) popupHide(p);
    }
    if (!p.visible) popupReleaseSparePictures(p);
    const EncoderInput in = encoderPoll(i);
    if (s_deckSettingsDirty && in.activity && (s.mode == ENCODER_PAGES || s.mode == ENCODER_DECK))
      s_deckSettingsAt = now;
    if (allowed && in.activity) {
      touchNoteActivity();
      wake |= sleeping;
    }
    if (!allowed || sleeping || manualDisplayOff()) {
      k.armed = k.turned = false;
      k.ignorePress = in.keyDown;
      k.pageTarget = -1;
      encoderDiscardRemainder(i);
      popupHide(k.popup);
      continue;
    }
    if (k.pageTarget >= 0) {
      if (k.pageTarget == sh.active) {
        k.pageTarget = -1;
        popupPreviewStop();
        if (s.mode == ENCODER_TILES) pickTile(k, s, 0, false, now);
        else pagePopup(p, s, now);
      } else if (compMode() == COMP_NONE) {
        k.pageTarget = -1;
      }
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
      knobAction(k, s, in, click, now);
    }

    if (!p.visible || s_previewKnob == (int)i) continue;
    if (p.rotation != panelRotation()) {
      popupHide(p);
    } else if (in.activity) {
      p.lastInput = now;
      if (p.overlay.animation.exiting) popupAnimate(p, true);
    } else if (!p.overlay.animation.exiting && now - p.lastInput >= p.settings.holdMs) {
      popupAnimate(p, false);
    }
    if (p.visible) popupAnimateValue(p);
  }
  if (wake) wakeDisplayFromTouch();
  popupPreviewTick(now, allowed, sleeping);
  if (s_deckSettingsDirty && now - s_deckSettingsAt >= DECK_SAVE_MS) {
    s_deckSettingsDirty = false;
    saveSettings();
  }
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
  encoderClearTiles(true);
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
