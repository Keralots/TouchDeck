// Deck UI: Calm tile animations. A ring from the finger on touch, a pop and
// glow when a tile turns on, the detail card growing out of its tile. The page
// crossfade is drawn by the compositor (panelSetPageFade).
#include "deck_ui_int.h"

namespace deckui {

namespace {

constexpr uint32_t RIPPLE_MS = 600, POP_MS = 560, SQUISH_MS = 300, HALO_MS = 700, GLOW_MS = 700;
constexpr uint32_t MORPH_MS = 300, MORPH_BACK_MS = 240, CARD_FADE_MS = 140;
constexpr int32_t P = 1024;  // animation progress range

uint32_t s_quietUntil = 0;

struct Morph {
  lv_obj_t* ghost = nullptr;
  lv_obj_t* card = nullptr;  // fades in once the ghost lands
  lv_area_t from, to;
  int r0, r1;
  bool back;
};
Morph s_m;

bool calm() { return dispSettings.tileFx == 1; }

float smooth(float t) { return t * t * (3 - 2 * t); }

// A non-clickable child outside the parent's layout.
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

// Rings and halos stay inside the rounded tile only while one runs.
void clipSync(TileRt& r) { lv_obj_set_style_clip_corner(r.obj, r.fx || r.halo, 0); }

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
  lv_obj_set_style_border_opa(o, (lv_opa_t)(190 * (P - v) / P), 0);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)(28 * (P - v) / P), 0);
}

void rippleDone(lv_anim_t* a) {
  lv_obj_t* o = (lv_obj_t*)a->var;
  if (TileRt* r = tileOf(lv_obj_get_parent(o))) {
    if (r->fx == o) r->fx = nullptr;
    clipSync(*r);
  }
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
  lv_obj_t* o = (lv_obj_t*)a->var;
  if (TileRt* r = tileOf(lv_obj_get_parent(o))) {
    if (r->halo == o) r->halo = nullptr;
    clipSync(*r);
  }
  lv_obj_delete_async(o);
}

void glowStep(void* var, int32_t v) {
  const float t = (float)v / P;
  const float a = t < .35f ? smooth(t / .35f) : 1 - smooth((t - .35f) / .65f);
  lv_obj_set_style_outline_opa((lv_obj_t*)var, (lv_opa_t)(170 * a), 0);
}

void glowDone(lv_anim_t* a) {
  lv_obj_t* t = (lv_obj_t*)a->var;
  lv_obj_remove_local_style_prop(t, LV_STYLE_OUTLINE_WIDTH, 0);
  lv_obj_remove_local_style_prop(t, LV_STYLE_OUTLINE_OPA, 0);
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
void ghostStep(void* var, int32_t v) {
  lv_obj_t* g = (lv_obj_t*)var;
  const lv_area_t& a = s_m.from;
  const lv_area_t& b = s_m.to;
  auto mix = [v](int32_t x, int32_t y) { return x + (y - x) * v / P; };
  const int32_t x1 = mix(a.x1, b.x1), y1 = mix(a.y1, b.y1), x2 = mix(a.x2, b.x2), y2 = mix(a.y2, b.y2);
  lv_obj_set_pos(g, x1, y1);
  lv_obj_set_size(g, x2 - x1 + 1, y2 - y1 + 1);
  lv_obj_set_style_radius(g, mix(s_m.r0, s_m.r1), 0);
  if (s_m.back) {
    const float t = (float)v / P;
    lv_obj_set_style_opa(g, (lv_opa_t)(255 * (t < .6f ? 1 : 1 - smooth((t - .6f) / .4f))), 0);
  }
}

void cardOpaStep(void* var, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)(255 * v / P), 0); }

void dropGhost() {
  if (s_m.ghost) lv_obj_delete_async(s_m.ghost);
  s_m.ghost = nullptr;
  s_m.card = nullptr;
}

void cardShown(lv_anim_t*) { dropGhost(); }

void ghostLanded(lv_anim_t*) {
  if (!s_m.card) {
    dropGhost();
    return;
  }
  startAnim(s_m.card, cardOpaStep, CARD_FADE_MS, lv_anim_path_ease_out, cardShown);
}

lv_obj_t* makeGhost() {
  lv_obj_t* g = overlay(lv_layer_top());
  lv_obj_set_style_bg_color(g, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(g, s_theme == THEME_GLASS ? LV_OPA_80 : LV_OPA_COVER, 0);
  if (s_theme == THEME_GLASS) {
    lv_obj_set_style_border_width(g, GLASS_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(g, lv_color_hex(GLASS_BORDER_COLOR), 0);
    lv_obj_set_style_border_opa(g, GLASS_BORDER_OPA, 0);
  }
  return g;
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
  const lv_color_t c = lv_obj_get_style_text_color(t, LV_PART_MAIN);
  lv_obj_set_style_border_color(o, c, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  clipSync(r);
  startAnim(o, rippleStep, RIPPLE_MS, lv_anim_path_ease_out, rippleDone);
}

void fxCancelRipples() {
  for (TileRt& r : s_tiles) {
    if (!r.fx) continue;
    lv_obj_delete(r.fx);
    r.fx = nullptr;
    clipSync(r);
  }
}

void fxTileOn(TileRt& r, bool wasOn) {
  const bool on = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  if (!calm() || wasOn == on || !live(r)) return;
  lv_obj_t* ic = r.chip ? r.chip : r.icon;
  if (ic) scaleAround(ic, on ? popStep : squishStep, on ? POP_MS : SQUISH_MS);
  if (!on) return;

  if (ic && !r.halo) {
    lv_area_t a, cc;
    lv_obj_get_coords(ic, &a);
    lv_obj_get_content_coords(r.obj, &cc);
    const int d = LV_MIN(lv_obj_get_height(r.obj), lv_obj_get_width(r.obj)) * 4 / 5;
    lv_obj_t* h = r.halo = overlay(r.obj);
    lv_obj_set_user_data(h, pack((a.x1 + a.x2) / 2 - cc.x1, (a.y1 + a.y2) / 2 - cc.y1, d));
    lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(h, lv_color_white(), 0);
    clipSync(r);
    startAnim(h, haloStep, HALO_MS, lv_anim_path_linear, haloDone);
  }
  lv_anim_delete(r.obj, glowStep);
  lv_obj_set_style_outline_color(r.obj, lv_color_hex(tileOnFill(*r.def)), 0);
  lv_obj_set_style_outline_width(r.obj, 3, 0);
  startAnim(r.obj, glowStep, GLOW_MS, lv_anim_path_linear, glowDone);
}

void fxCardOpen(lv_obj_t* card, int tileIdx) {
  dropGhost();
  lv_obj_t* t = calm() ? visibleTile(tileIdx) : nullptr;
  if (!t || !card) return;
  lv_obj_update_layout(card);
  lv_obj_get_coords(t, &s_m.from);
  lv_obj_get_coords(card, &s_m.to);
  s_m.r0 = lv_obj_get_style_radius(t, LV_PART_MAIN);
  s_m.r1 = lv_obj_get_style_radius(card, LV_PART_MAIN);
  s_m.back = false;
  s_m.card = card;
  lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
  s_m.ghost = makeGhost();
  lv_obj_move_to_index(s_m.ghost, lv_obj_get_index(card));  // under the card
  startAnim(s_m.ghost, ghostStep, MORPH_MS, lv_anim_path_ease_out, ghostLanded);
}

void fxCardClose(lv_obj_t* card, int tileIdx) {
  lv_obj_t* t = calm() && card ? visibleTile(tileIdx) : nullptr;
  if (t) {
    lv_obj_get_coords(card, &s_m.from);
    lv_obj_get_coords(t, &s_m.to);
    s_m.r0 = lv_obj_get_style_radius(card, LV_PART_MAIN);
    s_m.r1 = lv_obj_get_style_radius(t, LV_PART_MAIN);
  }
  closeModal();
  if (!t) return;
  s_m.back = true;
  s_m.ghost = makeGhost();
  startAnim(s_m.ghost, ghostStep, MORPH_BACK_MS, lv_anim_path_ease_in, cardShown);
}

void fxCardDrop() { dropGhost(); }

}  // namespace deckui
