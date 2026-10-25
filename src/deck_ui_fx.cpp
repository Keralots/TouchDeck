// Deck UI: Calm tile animations. A ring from the finger on touch, a pop and
// halo when a tile turns on, the detail card growing out of its tile. Page
// crossfade and card morph are drawn by the compositor (deck_panel.cpp).
#include "deck_ui_int.h"

namespace deckui {

namespace {

constexpr uint32_t RIPPLE_MS = 600, POP_MS = 560, SQUISH_MS = 300, HALO_MS = 700;
constexpr uint32_t MORPH_MS = 300, MORPH_BACK_MS = 240;
constexpr int32_t P = 1024;  // animation progress range

uint32_t s_quietUntil = 0;
lv_draw_buf_t* s_cardPic = nullptr;  // screen sized, kept once used

bool calm() { return dispSettings.tileFx == 1; }

float smooth(float t) { return t * t * (3 - 2 * t); }

// A non-clickable child outside the parent's layout. No clip_corner on the
// tile: it costs more than the rest of the effect (measured on the deck).
lv_obj_t* overlay(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_add_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

// Centre (in the parent's content coords) and size packed into a user pointer.
void* pack(int cx, int cy, int size) {
  return (void*)(uintptr_t)(((uint32_t)(cx + 1024) & 0x7FF) | ((uint32_t)(cy + 1024) & 0x7FF) << 11 |
                            (uint32_t)LV_MIN(size, 1023) << 22);
}
void unpack(const void* k, int& cx, int& cy, int& size) {
  const uint32_t v = (uint32_t)(uintptr_t)k;
  cx = (int)(v & 0x7FF) - 1024;
  cy = (int)(v >> 11 & 0x7FF) - 1024;
  size = (int)(v >> 22);
}

void placeCircle(lv_obj_t* o, int cx, int cy, int d) {
  lv_obj_set_size(o, d, d);
  lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
}

void startAnim(void* var, lv_anim_exec_xcb_t exec, uint32_t ms, lv_anim_path_cb_t path,
               lv_anim_completed_cb_t done) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, exec);
  lv_anim_set_values(&a, 0, P);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, path);
  if (done) lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}

// --- Touch ripple ---------------------------------------------------------------------
void rippleStep(void* var, int32_t v) {
  lv_obj_t* o = (lv_obj_t*)var;
  int cx, cy, r;
  unpack(lv_obj_get_user_data(o), cx, cy, r);
  placeCircle(o, cx, cy, LV_MAX(2, 2 * r * v / P));
  // Fades faster than it grows, so the ring is faint by the rounded corners.
  const float left = (float)(P - v) / P;
  lv_obj_set_style_border_opa(o, (lv_opa_t)(200 * left * left), 0);
}

void rippleDone(lv_anim_t* a) {
  lv_obj_t* o = (lv_obj_t*)a->var;
  if (TileRt* r = tileOf(lv_obj_get_parent(o)))
    if (r->fx == o) r->fx = nullptr;
  lv_obj_delete_async(o);
}

// --- Turning on -------------------------------------------------------------------------
void popStep(void* var, int32_t v) {  // 1 -> 1.3 -> 0.92 -> 1
  const float t = (float)v / P;
  const float s = t < .35f ? 1 + .3f * smooth(t / .35f)
                : t < .7f  ? 1.3f - .38f * smooth((t - .35f) / .35f)
                           : .92f + .08f * smooth((t - .7f) / .3f);
  lv_obj_set_style_transform_scale((lv_obj_t*)var, (int32_t)(256 * s), 0);
}

void squishStep(void* var, int32_t v) {  // 1 -> 0.86 -> 1
  const float t = (float)v / P;
  const float s = t < .4f ? 1 - .14f * smooth(t / .4f) : .86f + .14f * smooth((t - .4f) / .6f);
  lv_obj_set_style_transform_scale((lv_obj_t*)var, (int32_t)(256 * s), 0);
}

void haloStep(void* var, int32_t v) {
  lv_obj_t* o = (lv_obj_t*)var;
  int cx, cy, d;
  unpack(lv_obj_get_user_data(o), cx, cy, d);
  const float t = (float)v / P;
  const float s = t < .3f ? .3f + .7f * smooth(t / .3f) : 1 + .6f * smooth((t - .3f) / .7f);
  const float a = t < .3f ? smooth(t / .3f) : 1 - smooth((t - .3f) / .7f);
  placeCircle(o, cx, cy, LV_MAX(2, (int)(d * s)));
  lv_obj_set_style_bg_opa(o, (lv_opa_t)(110 * a), 0);
}

void haloDone(lv_anim_t* a) {
  lv_obj_t* box = lv_obj_get_parent((lv_obj_t*)a->var);
  if (TileRt* r = tileOf(lv_obj_get_parent(box)))
    if (r->halo == box) r->halo = nullptr;
  lv_obj_delete_async(box);
}

void scaleAround(lv_obj_t* o, lv_anim_exec_xcb_t step, uint32_t ms) {
  lv_anim_delete(o, popStep);
  lv_anim_delete(o, squishStep);
  lv_obj_update_layout(o);
  lv_obj_set_style_transform_pivot_x(o, lv_obj_get_width(o) / 2, 0);
  lv_obj_set_style_transform_pivot_y(o, lv_obj_get_height(o) / 2, 0);
  startAnim(o, step, ms, lv_anim_path_linear, nullptr);
}

// Only changes the user can see: not while pages are built, not under the
// card or the photo frame, not on pages off screen.
bool live(const TileRt& r) {
  return (int32_t)(millis() - s_quietUntil) >= 0 && r.page == sh.active && !frameActive() && !modalShown() &&
         lv_obj_is_visible(r.obj);
}

// --- Card morph -------------------------------------------------------------------------
FrameRect rectOf(lv_obj_t* o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return {(int16_t)a.x1, (int16_t)a.y1, (int16_t)lv_area_get_width(&a), (int16_t)lv_area_get_height(&a)};
}

// The card's picture for the compositor. Only while no morph reads the buffer.
bool snapCard(lv_obj_t* card) {
  if (compMode() != COMP_NONE) return false;
  const uint32_t w = logicalW(), h = logicalH();
  if (s_cardPic && s_cardPic->header.w * s_cardPic->header.h < w * h) {
    lv_draw_buf_destroy(s_cardPic);
    s_cardPic = nullptr;
  }
  if (!s_cardPic) s_cardPic = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!s_cardPic) return false;
  lv_obj_update_layout(card);
  if (!lv_snapshot_reshape_draw_buf(card, s_cardPic)) return false;
  return lv_snapshot_take_to_draw_buf(card, LV_COLOR_FORMAT_RGB565, s_cardPic) == LV_RESULT_OK;
}

MorphScene cardScene(lv_obj_t* card) {
  MorphScene m = {};
  m.card = {(const uint16_t*)s_cardPic->data, (uint16_t)(s_cardPic->header.stride / 2), true};
  m.cw = (int16_t)lv_obj_get_width(card);
  m.ch = (int16_t)lv_obj_get_height(card);
  m.ghost = lv_color_to_u16(lv_color_hex(C_PANEL));
  return m;
}

lv_obj_t* visibleTile(int tileIdx) {
  if (tileIdx < 0 || tileIdx >= (int)s_tiles.size()) return nullptr;
  lv_obj_t* t = s_tiles[tileIdx].obj;
  return lv_obj_is_visible(t) ? t : nullptr;
}

}  // namespace

void fxQuiet() { s_quietUntil = millis() + 800; }

void fxPressed(lv_event_t* e) {
  TileRt* r = tileOf((lv_obj_t*)lv_event_get_current_target(e));
  lv_indev_t* in = lv_indev_active();
  if (!r || !in) return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  fxRipple(*r, p);
}

void fxRipple(TileRt& r, lv_point_t p) {
  if (!calm()) return;
  lv_obj_t* t = r.obj;
  lv_area_t tc, cc;
  lv_obj_get_coords(t, &tc);
  lv_obj_get_content_coords(t, &cc);
  // Radius to the farthest corner: the ring just covers the tile.
  const int dx = LV_MAX(p.x - tc.x1, tc.x2 - p.x), dy = LV_MAX(p.y - tc.y1, tc.y2 - p.y);
  const int rad = (int)sqrtf((float)(dx * dx + dy * dy)) + 2;
  if (r.fx) lv_obj_delete(r.fx);
  lv_obj_t* o = r.fx = overlay(t);
  lv_obj_set_user_data(o, pack(p.x - cc.x1, p.y - cc.y1, rad));
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, lv_obj_get_style_text_color(t, LV_PART_MAIN), 0);
  startAnim(o, rippleStep, RIPPLE_MS, lv_anim_path_ease_out, rippleDone);
}

void fxCancelRipples() {
  for (TileRt& r : s_tiles) {
    if (!r.fx) continue;
    lv_obj_delete(r.fx);
    r.fx = nullptr;
  }
}

void fxTileOn(TileRt& r, bool wasOn) {
  const bool on = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  if (!calm() || wasOn == on || !live(r)) return;
  lv_obj_t* ic = r.chip ? r.chip : r.icon;
  if (ic) scaleAround(ic, on ? popStep : squishStep, on ? POP_MS : SQUISH_MS);
  if (!on || !ic || r.halo) return;
  lv_area_t a, tc, cc;
  lv_obj_get_coords(ic, &a);
  lv_obj_get_coords(r.obj, &tc);
  lv_obj_get_content_coords(r.obj, &cc);
  const int d = LV_MIN(lv_obj_get_height(r.obj), lv_obj_get_width(r.obj)) * 4 / 5;
  // Tile shaped clip for the glow alone: clip_corner redraws only its children.
  lv_obj_t* box = r.halo = overlay(r.obj);
  lv_obj_set_pos(box, tc.x1 - cc.x1, tc.y1 - cc.y1);
  lv_obj_set_size(box, lv_area_get_width(&tc), lv_area_get_height(&tc));
  lv_obj_set_style_radius(box, lv_obj_get_style_radius(r.obj, LV_PART_MAIN), 0);
  lv_obj_set_style_clip_corner(box, true, 0);
  lv_obj_t* h = overlay(box);
  lv_obj_set_user_data(h, pack((a.x1 + a.x2) / 2 - tc.x1, (a.y1 + a.y2) / 2 - tc.y1, d));
  lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(h, lv_color_white(), 0);
  startAnim(h, haloStep, HALO_MS, lv_anim_path_linear, haloDone);
}

// The compositor flies the card picture out of the tile over the dimmed page,
// then shows LVGL's frame with the real card.
void fxCardOpen(lv_obj_t* card, int tileIdx) {
  lv_obj_t* t = calm() ? visibleTile(tileIdx) : nullptr;
  if (!t || !card) return;
  if (!snapCard(card)) return;
  MorphScene m = cardScene(card);
  m.from = rectOf(t);
  m.to = rectOf(card);
  m.r0 = (int16_t)lv_obj_get_style_radius(t, LV_PART_MAIN);
  m.r1 = (int16_t)lv_obj_get_style_radius(card, LV_PART_MAIN);
  m.dim = 2;  // LVGL's scrim
  m.open = true;
  compBeginMorph(m, MORPH_MS, ++s_gen);
}

void fxCardClose(lv_obj_t* card, int tileIdx) {
  lv_obj_t* t = calm() && card ? visibleTile(tileIdx) : nullptr;
  const bool morph = t && snapCard(card) && snapPage(sh.active);
  MorphScene m = {};
  if (morph) {
    m = cardScene(card);
    m.base = sh.src[sh.active];
    m.from = rectOf(card);
    m.to = rectOf(t);
    m.r0 = (int16_t)lv_obj_get_style_radius(card, LV_PART_MAIN);
    m.r1 = (int16_t)lv_obj_get_style_radius(t, LV_PART_MAIN);
  }
  closeModal();
  if (morph) compBeginMorph(m, MORPH_BACK_MS, ++s_gen);
}

}  // namespace deckui
