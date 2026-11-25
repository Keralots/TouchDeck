// Deck UI: the cover tile (blinds, shades, garage doors) and the cover buttons
// on the detail card.
#include "deck_ui_int.h"

namespace deckui {

// HA CoverEntityFeature bits.
constexpr int32_t COVER_OPEN = 1, COVER_CLOSE = 2, COVER_STOP = 8;

bool coverMoving(const HaEntity& e) { return e.state == "opening" || e.state == "closing"; }

static bool coverAvail(const HaEntity& e) {
  return e.known && e.state != "unavailable" && e.state != "unknown";
}

static bool coverCan(const HaEntity& e, int32_t bit) { return e.features < 0 || (e.features & bit); }

// "Opening", "Closed", "45%": the level the finger asked for until HA reports it.
String coverText(const String& entity, const HaEntity& e) {
  const Wish* w = findWish(entity);
  if ((w && (w->on.active || w->level.active)) || !coverAvail(e)) return wishText(entity, e, false);
  if (coverMoving(e)) return e.state == "opening" ? "Opening" : "Closing";
  if (e.level >= 0) return e.level <= 0 ? "Closed" : e.level >= 100 ? "Open" : String(e.level) + "%";
  return prettyState("cover", e);
}

// How much of the window the cover hides, 0..1, drawn from the top.
static float coverShadeOf(const String& entity, const HaEntity& e) {
  const float f = wishFrac(entity, e);
  if (f >= 0) return 1 - f;
  return wishOn(entity, e) ? 0 : 1;
}

void coverShade(TileRt& r) {
  const int w = lv_obj_get_width(r.obj), h = lv_obj_get_height(r.obj);
  if (w <= 0) return;
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  const int fh = (int)lroundf(h * r.frac);
  lv_obj_set_pos(r.fill, -pad, -pad);
  lv_obj_set_size(r.fill, w, fh);
  showObj(r.fill, fh > 0);
}

// Up / stop / down: hidden when the cover cannot do it, dimmed when it is
// already there (HA's own rule: fully open, or opening, greys out up).
void coverButtonsSync(lv_obj_t* const btns[3], const String& entity, const HaEntity* e) {
  const bool avail = e && coverAvail(*e);
  const Wish* w = findWish(entity);
  const bool known = avail && !(w && (w->on.active || w->level.active));
  const bool fullOpen = known && (e->level >= 0 ? e->level >= 100 : e->state == "open");
  const bool fullShut = known && (e->level >= 0 ? e->level <= 0 : e->state == "closed");
  const bool can[3] = {!e || coverCan(*e, COVER_OPEN), !e || coverCan(*e, COVER_STOP),
                       !e || coverCan(*e, COVER_CLOSE)};
  const bool off[3] = {!avail || fullOpen || (known && e->state == "opening"), !avail,
                       !avail || fullShut || (known && e->state == "closing")};
  for (int i = 0; i < 3; i++) {
    if (!btns[i]) continue;
    if (!can[i]) showObj(btns[i], false);
    if (off[i]) lv_obj_add_state(btns[i], LV_STATE_DISABLED);
    else lv_obj_remove_state(btns[i], LV_STATE_DISABLED);
  }
}

void coverCommand(const String& entity, int button) {
  if (haLink() != HA_CONNECTED) {
    deckToastShow("Home Assistant is offline", 1800);
    return;
  }
  if (button == 1) {
    dropWish(entity);
    if (!haCallService("cover", "stop_cover", entity, "")) deckToastShow("Home Assistant is busy, try again", 1800);
    return;
  }
  setWish(entity, false, button == 0 ? 1 : 0);
}

void applyCover(TileRt& r, const HaEntity& e) {
  const TileDef& d = *r.def;
  if (!d.sub.length()) lv_label_set_text(r.sub, coverText(d.entity, e).c_str());
  r.frac = coverShadeOf(d.entity, e);
  coverShade(r);
  // Hidden buttons stay hidden: fitCover shows the ones the tile has room for.
  lv_obj_t* shown[3];
  for (int i = 0; i < 3; i++) shown[i] = lv_obj_has_flag(r.coverBtns[i], LV_OBJ_FLAG_HIDDEN) ? nullptr : r.coverBtns[i];
  coverButtonsSync(shown, d.entity, &e);
}

void coverEvent(lv_event_t* e) {
  const size_t tag = (size_t)(uintptr_t)lv_event_get_user_data(e), idx = tag / 4;
  if (idx >= s_tiles.size()) return;
  hapticTap();
  audioClick();
  const int button = tag % 4;
  tileGuard(s_tiles[idx], [button](TileRt& r) { coverCommand(r.def->entity, button); });
}

static void coverGlyphs(lv_obj_t* const btns[3], int px) {
  static const char* const names[] = {"arrow-up", "stop", "arrow-down"};
  static const char* const fallback[] = {"Up", "Stop", "Down"};
  for (int i = 0; i < 3; i++) mediaGlyph(lv_obj_get_child(btns[i], 0), names[i], fallback[i], px);
}

lv_obj_t* coverButton(lv_obj_t* parent, uint32_t accent) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TRACK), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
  lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
  lv_obj_center(l);
  return b;
}

// Tile: the shade behind, then the usual icon / state / name in r.box.
void makeCoverButtons(TileRt& r, size_t idx, uint32_t accent) {
  for (int i = 0; i < 3; i++) {
    r.coverBtns[i] = coverButton(r.obj, accent);
    lv_obj_add_event_cb(r.coverBtns[i], coverEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(idx * 4 + i));
  }
  coverGlyphs(r.coverBtns, 20);
}

// Buttons under the name on taller tiles, beside it on wide short ones, none
// on small tiles (a tap opens or closes, or stops it while it moves).
void fitCover(TileRt& r, int cw, int ch) {
  constexpr int GAP = 6;
  int boxW = cw, boxH = ch, bw = 0, bh = 0, x0 = 0, y0 = 0;
  if (ch >= 100 && cw >= 90) {
    bh = constrain(ch * 2 / 5, 36, 52);
    bw = (cw - 2 * GAP) / 3;
    y0 = ch - bh;
    boxH = ch - bh - 8;
  } else if (ch >= 34) {
    bh = bw = min(ch, 52);
    if (cw - 3 * bw - 2 * GAP - 10 < 90) bh = bw = max(34, (cw - 100 - 2 * GAP) / 3);
    x0 = cw - 3 * bw - 2 * GAP;
    y0 = (ch - bh) / 2;
    boxW = x0 - 10;
    if (boxW < 80) bw = bh = 0, boxW = cw;
  }
  for (int i = 0; i < 3; i++) {
    showObj(r.coverBtns[i], bw > 0);
    if (!bw) continue;
    lv_obj_set_size(r.coverBtns[i], bw, bh);
    lv_obj_set_pos(r.coverBtns[i], x0 + i * (bw + GAP), y0);
  }
  if (bw) coverGlyphs(r.coverBtns, min(bw, bh) >= 44 ? 28 : 20);
  lv_obj_set_size(r.box, boxW, boxH);
  lv_obj_set_pos(r.box, 0, 0);
  fitButton(r, boxW, boxH);
  coverShade(r);
  HaEntity e;
  if (haGetEntity(r.def->entity, e)) applyCover(r, e);
}

// --- Detail card -----------------------------------------------------------------------
lv_obj_t* s_cardCover[3] = {};

void onCardCover(lv_event_t* e) {
  if (s_modalTile < 0 || s_modalTile >= (int)s_tiles.size()) return;
  hapticTap();
  coverCommand(s_tiles[s_modalTile].def->entity, (int)(intptr_t)lv_event_get_user_data(e));
}

void coverCardBuild(lv_obj_t* c, int cw, int y, uint32_t accent) {
  constexpr int GAP = 10, H = 48;
  const int bw = (cw - 32 - 2 * GAP) / 3;
  for (int i = 0; i < 3; i++) {
    s_cardCover[i] = coverButton(c, accent);
    lv_obj_set_size(s_cardCover[i], bw, H);
    lv_obj_set_pos(s_cardCover[i], 16 + i * (bw + GAP), y);
    lv_obj_add_event_cb(s_cardCover[i], onCardCover, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  coverGlyphs(s_cardCover, 28);
}

void coverCardRefresh(const String& entity, const HaEntity* e) {
  if (s_cardCover[0]) coverButtonsSync(s_cardCover, entity, e);
}

void coverCardClose() {
  for (lv_obj_t*& b : s_cardCover) b = nullptr;
}

}  // namespace deckui
