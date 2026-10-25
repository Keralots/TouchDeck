// Deck UI: tile animations. A preset (Display > Animations) picks one effect
// for each moment: touch, on/off change, page change, card. Calm: a ring, a
// pop and halo, crossfade, the card growing out of its tile. Water: drop
// rings, on colour flooding out of the finger, a water edge between pages,
// the card revealed in a circle. Page changes and cards are drawn by the
// compositor (deck_panel.cpp).
#include "deck_ui_int.h"

namespace deckui {

namespace {

enum FxTouch : uint8_t { TOUCH_NONE, TOUCH_RING, TOUCH_DROP };
enum FxToggle : uint8_t { TOGGLE_NONE, TOGGLE_BLOOM, TOGGLE_LIQUID };
enum FxCard : uint8_t { CARD_NONE, CARD_MORPH, CARD_REVEAL };
struct FxSet {
  FxTouch touch;
  FxToggle toggle;
  PageFx page;
  FxCard card;
};
// Indexed by dispSettings.tileFx.
constexpr FxSet kSets[] = {
    {TOUCH_NONE, TOGGLE_NONE, PAGE_SLIDE, CARD_NONE},      // Classic
    {TOUCH_RING, TOGGLE_BLOOM, PAGE_FADE, CARD_MORPH},     // Calm
    {TOUCH_DROP, TOGGLE_LIQUID, PAGE_WATER, CARD_REVEAL},  // Water
};

const FxSet& fx() { return kSets[dispSettings.tileFx < sizeof(kSets) / sizeof(kSets[0]) ? dispSettings.tileFx : 0]; }

constexpr uint32_t RIPPLE_MS = 600, POP_MS = 560, SQUISH_MS = 300, HALO_MS = 700;
constexpr uint32_t DROP_MS = 1280, DROP_RING_MS = 1000, DROP_GAP_MS = 140, SPLASH_MS = 320;
constexpr uint32_t LIQUID_ON_MS = 460, LIQUID_OFF_MS = 380;
constexpr uint32_t MORPH_MS = 300, MORPH_BACK_MS = 240, REVEAL_MS = 460, REVEAL_BACK_MS = 320;
constexpr uint32_t PRESS_FRESH_MS = 3000;  // a touch this recent places on/off and card effects
constexpr int32_t P = 1024;  // animation progress range

uint32_t s_quietUntil = 0;
lv_draw_buf_t* s_cardPic = nullptr;  // screen sized, kept once used
FxCard s_openFx = CARD_NONE;        // the open card's effect, its close plays the same
lv_point_t s_revealAt = {};          // where the open card's circle came from

float smooth(float t) { return t * t * (3 - 2 * t); }
float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

// A non-clickable child outside the parent's layout. No clip_corner on the
// tile: it costs more than the rest of the effect (measured on the deck).
lv_obj_t* overlay(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_add_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

// Tile shaped clip box over the whole tile for an effect's objects:
// clip_corner on it redraws only its own children.
lv_obj_t* tileBox(const TileRt& r) {
  lv_area_t tc, cc;
  lv_obj_get_coords(r.obj, &tc);
  lv_obj_get_content_coords(r.obj, &cc);
  lv_obj_t* box = overlay(r.obj);
  lv_obj_set_pos(box, tc.x1 - cc.x1, tc.y1 - cc.y1);
  lv_obj_set_size(box, lv_area_get_width(&tc), lv_area_get_height(&tc));
  lv_obj_set_style_radius(box, lv_obj_get_style_radius(r.obj, LV_PART_MAIN), 0);
  lv_obj_set_style_clip_corner(box, true, 0);
  return box;
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

lv_obj_t* circle(lv_obj_t* parent, int cx, int cy, int size) {
  lv_obj_t* o = overlay(parent);
  lv_obj_set_user_data(o, pack(cx, cy, size));
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  placeCircle(o, cx, cy, 2);
  return o;
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

// Distance from p to the farthest corner of a: a circle that big covers it.
int farCorner(const lv_area_t& a, lv_point_t p) {
  const int dx = LV_MAX(p.x - a.x1, a.x2 - p.x), dy = LV_MAX(p.y - a.y1, a.y2 - p.y);
  return (int)sqrtf((float)(dx * dx + dy * dy)) + 2;
}

// The tile's last touch if it was recent and on the tile, else its centre.
lv_point_t touchPoint(const TileRt& r) {
  lv_area_t a;
  lv_obj_get_coords(r.obj, &a);
  const lv_point_t& p = r.press;
  if (r.pressMs && millis() - r.pressMs < PRESS_FRESH_MS && p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2)
    return p;
  return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

// --- Touch: ripple ring (Calm) -----------------------------------------------------------
void rippleStep(void* var, int32_t v) {
  lv_obj_t* o = (lv_obj_t*)var;
  int cx, cy, r;
  unpack(lv_obj_get_user_data(o), cx, cy, r);
  placeCircle(o, cx, cy, LV_MAX(2, 2 * r * v / P));
  // Fades faster than it grows, so the ring is faint by the rounded corners.
  const float left = (float)(P - v) / P;
  lv_obj_set_style_border_opa(o, (lv_opa_t)(200 * left * left), 0);
}

// The touch effect's object is r.fx, a child of the tile.
void touchDone(lv_anim_t* a) {
  lv_obj_t* o = (lv_obj_t*)a->var;
  if (TileRt* r = tileOf(lv_obj_get_parent(o)))
    if (r->fx == o) r->fx = nullptr;
  lv_obj_delete_async(o);
}

// --- Touch: water drop (Water) -------------------------------------------------------------
// Box children: three rings 140 ms apart, then the splash at the finger.
void dropStep(void* var, int32_t v) {
  lv_obj_t* box = (lv_obj_t*)var;
  const float ms = (float)v * DROP_MS / P;
  for (int i = 0; i < 4; i++) {
    lv_obj_t* o = lv_obj_get_child(box, i);
    int cx, cy, size;
    unpack(lv_obj_get_user_data(o), cx, cy, size);
    if (i == 3) {
      const float t = clamp01(ms / SPLASH_MS);
      placeCircle(o, cx, cy, LV_MAX(2, (int)(size * (.2f + 1.6f * t))));
      lv_obj_set_style_bg_opa(o, (lv_opa_t)(220 * (1 - t)), 0);
      continue;
    }
    const float t = clamp01((ms - i * DROP_GAP_MS) / DROP_RING_MS);
    const float a = t < .08f ? t / .08f : 1 - (t - .08f) / .92f;
    placeCircle(o, cx, cy, LV_MAX(2, (int)(size * easeOut(t))));
    lv_obj_set_style_border_opa(o, (lv_opa_t)(t > 0 ? 180 * a : 0), 0);
  }
}

void touchRing(TileRt& r, lv_point_t p, int rad) {
  lv_area_t cc;
  lv_obj_get_content_coords(r.obj, &cc);
  lv_obj_t* o = r.fx = circle(r.obj, p.x - cc.x1, p.y - cc.y1, rad);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, lv_obj_get_style_text_color(r.obj, LV_PART_MAIN), 0);
  startAnim(o, rippleStep, RIPPLE_MS, lv_anim_path_ease_out, touchDone);
}

void touchDrop(TileRt& r, lv_point_t p, int rad) {
  lv_area_t tc;
  lv_obj_get_coords(r.obj, &tc);
  const lv_color_t c = s_theme == THEME_DAYLIGHT ? lv_obj_get_style_text_color(r.obj, LV_PART_MAIN)
                                                 : lv_color_hex(0xDCF5FF);
  lv_obj_t* box = r.fx = tileBox(r);
  const int x = p.x - tc.x1, y = p.y - tc.y1;
  for (int i = 0; i < 3; i++) {
    lv_obj_t* o = circle(box, x, y, rad * 17 / 10);
    lv_obj_set_style_border_width(o, i < 2 ? 2 : 1, 0);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_TRANSP, 0);
  }
  lv_obj_t* s = circle(box, x, y, 12);
  lv_obj_set_style_bg_color(s, c, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
  startAnim(box, dropStep, DROP_MS, lv_anim_path_linear, touchDone);
}

// --- On/off: pop and halo (Calm) -----------------------------------------------------------
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

void toggleBloom(TileRt& r, bool on) {
  lv_obj_t* ic = r.chip ? r.chip : r.icon;
  if (ic) scaleAround(ic, on ? popStep : squishStep, on ? POP_MS : SQUISH_MS);
  if (!on || !ic || r.halo) return;
  lv_area_t a, tc;
  lv_obj_get_coords(ic, &a);
  lv_obj_get_coords(r.obj, &tc);
  const int d = LV_MIN(lv_obj_get_height(r.obj), lv_obj_get_width(r.obj)) * 4 / 5;
  lv_obj_t* box = r.halo = tileBox(r);
  lv_obj_t* h = circle(box, (a.x1 + a.x2) / 2 - tc.x1, (a.y1 + a.y2) / 2 - tc.y1, d);
  lv_obj_set_style_bg_color(h, lv_color_white(), 0);
  startAnim(h, haloStep, HALO_MS, lv_anim_path_linear, haloDone);
}

// --- On/off: liquid fill (Water) ------------------------------------------------------------
// The on colour grows from the finger in a circle under the tile's content
// (or drains back into it). Meanwhile the tile's "on" fill and the new text
// colour are held back: their local styles are saved and put back as found.
struct Held {
  lv_style_prop_t prop;
  lv_style_selector_t sel;
  lv_style_value_t v;
  bool had, used;
};
struct Liquid {
  Held fill, pressedFill, edge, text;  // the "on" fill (also pressed) and outline, the text's new colour
  bool on;
};

Held hold(lv_obj_t* o, lv_style_prop_t prop, lv_style_selector_t sel, lv_style_value_t to) {
  Held h = {prop, sel, {}, false, true};
  h.had = lv_obj_get_local_style_prop(o, prop, &h.v, sel) == LV_STYLE_RES_FOUND;
  lv_obj_set_local_style_prop(o, prop, to, sel);
  return h;
}

void putBack(lv_obj_t* o, Held& h) {
  if (!h.used) return;
  if (h.had) lv_obj_set_local_style_prop(o, h.prop, h.v, h.sel);
  else lv_obj_remove_local_style_prop(o, h.prop, h.sel);
  h.used = false;
}

void liquidDelete(lv_event_t* e) { delete (Liquid*)lv_event_get_user_data(e); }

Liquid* liquidOf(lv_obj_t* box) { return (Liquid*)lv_obj_get_user_data(box); }

// Ends the running liquid at once: the tile shows its state as it is.
void liquidStop(TileRt& r) {
  if (!r.halo) return;
  if (Liquid* q = liquidOf(r.halo)) {
    putBack(r.obj, q->fill);
    putBack(r.obj, q->pressedFill);
    putBack(r.obj, q->edge);
    putBack(r.obj, q->text);
  }
  lv_obj_delete(r.halo);  // its animation goes with it
  r.halo = nullptr;
}

// Box children: the fill, the ring on its edge.
void liquidStep(void* var, int32_t v) {
  lv_obj_t* box = (lv_obj_t*)var;
  Liquid* q = liquidOf(box);
  TileRt* r = tileOf(lv_obj_get_parent(box));
  if (!q || !r) return;
  const float t = (float)v / P;
  lv_obj_t* fill = lv_obj_get_child(box, 0);
  lv_obj_t* ring = lv_obj_get_child(box, 1);
  int cx, cy, rad;
  unpack(lv_obj_get_user_data(fill), cx, cy, rad);
  const int d = LV_MAX(2, (int)(2 * rad * (q->on ? easeOut(t) : 1 - t * t * t)));
  placeCircle(fill, cx, cy, d);
  placeCircle(ring, cx, cy, d);
  lv_obj_set_style_border_opa(ring, (lv_opa_t)(q->on ? 190 * (1 - t) : 170 * t), 0);
  // The text turns over part way, as the fill passes under it.
  if (t >= (q->on ? .37f : .6f)) putBack(r->obj, q->text);
}

void liquidDone(lv_anim_t* a) {
  lv_obj_t* box = (lv_obj_t*)a->var;
  TileRt* r = tileOf(lv_obj_get_parent(box));
  if (r && r->halo == box) liquidStop(*r);  // the tile's own fill takes over in the same frame
}

// Before the tile changes state: hold back what the change would show at once.
void startLiquid(TileRt& r, bool on) {
  liquidStop(r);
  const bool glass = s_theme == THEME_GLASS && wallBlur();
  Liquid* q = new Liquid();
  q->on = on;
  // Still in the old state, so the current values are the ones to keep.
  // Turning on: the CHECKED fill (bg_opa on Glass, bg_color else) waits for
  // the circle. Both ways: the text keeps its colour until part way.
  lv_style_value_t v;
  if (on) {
    if (glass) v.num = LV_OPA_TRANSP;
    else v.color = lv_obj_get_style_bg_color(r.obj, LV_PART_MAIN);
    const lv_style_prop_t prop = glass ? LV_STYLE_BG_OPA : LV_STYLE_BG_COLOR;
    q->fill = hold(r.obj, prop, LV_STATE_CHECKED, v);
    q->pressedFill = hold(r.obj, prop, LV_STATE_PRESSED | LV_STATE_CHECKED, v);  // a slider dragged on
    // Glass tiles with a faint fill also get an outline when on.
    if (lv_obj_get_local_style_prop(r.obj, LV_STYLE_BORDER_WIDTH, &v, LV_STATE_CHECKED) == LV_STYLE_RES_FOUND) {
      v.num = 0;
      q->edge = hold(r.obj, LV_STYLE_BORDER_WIDTH, LV_STATE_CHECKED, v);
    }
  }
  v.color = lv_obj_get_style_text_color(r.obj, LV_PART_MAIN);
  q->text = hold(r.obj, LV_STYLE_TEXT_COLOR, on ? LV_STATE_CHECKED : LV_STATE_DEFAULT, v);

  lv_area_t tc;
  lv_obj_get_coords(r.obj, &tc);
  const lv_point_t p = touchPoint(r);
  const int rad = farCorner(tc, p);
  lv_obj_t* box = r.halo = tileBox(r);
  lv_obj_move_to_index(box, 0);  // under the tile's content
  lv_obj_set_user_data(box, q);
  lv_obj_add_event_cb(box, liquidDelete, LV_EVENT_DELETE, q);
  const TileDef& d = *r.def;
  lv_obj_t* fill = circle(box, p.x - tc.x1, p.y - tc.y1, rad);
  lv_obj_set_style_bg_color(fill, lv_color_hex(tileOnFill(d)), 0);
  lv_obj_set_style_bg_opa(fill, glass ? (lv_opa_t)(tileOnOpa(d) * 255 / 100) : LV_OPA_COVER, 0);
  lv_obj_t* ring = circle(box, p.x - tc.x1, p.y - tc.y1, rad);
  lv_obj_set_style_border_width(ring, 2, 0);
  lv_obj_set_style_border_color(ring, lv_color_white(), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_TRANSP, 0);
  startAnim(box, liquidStep, on ? LIQUID_ON_MS : LIQUID_OFF_MS, lv_anim_path_linear, liquidDone);
}

// Only changes the user can see: not while pages are built, not under the
// card or the photo frame, not on pages off screen.
bool live(const TileRt& r) {
  return (int32_t)(millis() - s_quietUntil) >= 0 && r.page == sh.active && !frameActive() && !modalShown() &&
         lv_obj_is_visible(r.obj);
}

// --- Cards: morph (Calm) and reveal (Water) ------------------------------------------------
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

PageFx fxPage() { return fx().page; }

void fxQuiet() { s_quietUntil = millis() + 800; }

void fxPressed(lv_event_t* e) {
  TileRt* r = tileOf((lv_obj_t*)lv_event_get_current_target(e));
  lv_indev_t* in = lv_indev_active();
  if (!r || !in) return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  r->press = p;
  r->pressMs = millis();
  fxRipple(*r, p);
}

void fxRipple(TileRt& r, lv_point_t p) {
  const FxTouch t = fx().touch;
  if (t == TOUCH_NONE) return;
  lv_area_t tc;
  lv_obj_get_coords(r.obj, &tc);
  if (r.fx) lv_obj_delete(r.fx);
  r.fx = nullptr;
  if (t == TOUCH_DROP) touchDrop(r, p, farCorner(tc, p));
  else touchRing(r, p, farCorner(tc, p));
}

void fxCancelRipples() {
  for (TileRt& r : s_tiles) {
    if (!r.fx) continue;
    lv_obj_delete(r.fx);
    r.fx = nullptr;
  }
}

void fxBeforeTileOn(TileRt& r, bool on) {
  if (fx().toggle != TOGGLE_LIQUID) {
    if (r.halo && liquidOf(r.halo)) liquidStop(r);  // started under an earlier preset
    return;
  }
  if (live(r)) startLiquid(r, on);
  else liquidStop(r);
}

void fxTileOn(TileRt& r, bool wasOn) {
  const bool on = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  if (fx().toggle == TOGGLE_BLOOM && wasOn != on && live(r)) toggleBloom(r, on);
}

// Calm: the compositor flies the card picture out of the tile over the
// dimmed page. Water: it reveals the card in a circle from the finger. Then
// LVGL's frame with the real card takes over.
void fxCardOpen(lv_obj_t* card, int tileIdx) {
  const FxCard kind = s_openFx = fx().card;
  if (tileIdx >= 0 && tileIdx < (int)s_tiles.size()) s_revealAt = touchPoint(s_tiles[tileIdx]);
  lv_obj_t* t = kind != CARD_NONE ? visibleTile(tileIdx) : nullptr;
  if (!t || !card) return;
  if (!snapCard(card)) return;
  MorphScene m = cardScene(card);
  m.from = rectOf(t);
  m.to = rectOf(card);
  m.r0 = (int16_t)lv_obj_get_style_radius(t, LV_PART_MAIN);
  m.r1 = (int16_t)lv_obj_get_style_radius(card, LV_PART_MAIN);
  m.dim = 2;  // the scrim fades in with the card
  m.open = true;
  if (kind == CARD_REVEAL) {
    m.reveal = true;
    m.cx = (int16_t)s_revealAt.x;
    m.cy = (int16_t)s_revealAt.y;
  }
  compBeginMorph(m, kind == CARD_REVEAL ? REVEAL_MS : MORPH_MS, ++s_gen);
}

void fxCardClose(lv_obj_t* card, int tileIdx) {
  const FxCard kind = s_openFx;
  lv_obj_t* t = kind != CARD_NONE && card ? visibleTile(tileIdx) : nullptr;
  const bool morph = t && snapCard(card) && snapPage(sh.active);
  MorphScene m = {};
  if (morph) {
    m = cardScene(card);
    m.base = sh.src[sh.active];
    m.from = rectOf(card);
    m.to = rectOf(t);
    m.r0 = (int16_t)lv_obj_get_style_radius(card, LV_PART_MAIN);
    m.r1 = (int16_t)lv_obj_get_style_radius(t, LV_PART_MAIN);
    m.dim = 2;  // the scrim fades out with the card
    if (kind == CARD_REVEAL) {
      m.to = m.from;
      m.r1 = m.r0;
      m.reveal = true;
      m.cx = (int16_t)s_revealAt.x;
      m.cy = (int16_t)s_revealAt.y;
    }
  }
  closeModal();
  if (morph) compBeginMorph(m, kind == CARD_REVEAL ? REVEAL_BACK_MS : MORPH_BACK_MS, ++s_gen);
}

}  // namespace deckui
