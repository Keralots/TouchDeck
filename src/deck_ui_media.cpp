// Deck UI: the Now playing and clock tiles.
#include "deck_ui_int.h"

namespace deckui {

// --- Now playing ----------------------------------------------------------------------
void mediaGlyph(lv_obj_t* label, const char* name, const char* fallback, int px) {
  const uint32_t cp = iconCodepoint(name);
  const lv_font_t* font = cp ? iconFont(px) : nullptr;
  char text[5];
  if (font) iconUtf8(cp, text);
  setFont(label, font ? font : &deck_font_20);
  setText(label, font ? text : fallback);
}

void mediaSetArt(TileRt& r, const std::shared_ptr<MediaArt>& art) {
  if (r.art == art) return;
  lv_image_set_src(r.cover, art ? &art->dsc : nullptr);
  r.art = art;  // the previous descriptor is no longer referenced by this image
  showObj(r.cover, (bool)art);
  showObj(r.icon, !art);
  markPageStale(r.page);
}

void mediaProgress(TileRt& r) {
  const PcState* st = pcStateOf(pcSource(r.def->pc));
  const time_t now = time(nullptr);
  const bool visible = r.mediaFull && st && st->media >= 0 && st->dur > 0 && now >= 1609459200 &&
                       (st->media != PC_MEDIA_PLAYING || st->t0 > 0);
  bool changed = visible == lv_obj_has_flag(r.viz, LV_OBJ_FLAG_HIDDEN);
  showObj(r.viz, visible);
  if (visible) {
    const int64_t elapsed = st->media == PC_MEDIA_PLAYING ? (int64_t)now - st->t0 : st->pos;
    const int value = (int)(std::max((int64_t)0, std::min(elapsed, (int64_t)st->dur)) * 1000 / st->dur);
    if (value != lv_bar_get_value(r.viz)) {
      lv_bar_set_value(r.viz, value, LV_ANIM_OFF);
      changed = true;
    }
  }
  if (changed) markPageStale(r.page);
}

void mediaCover(TileRt& r) {
  const PcSource* pc = pcSource(r.def->pc);
  const PcState* st = pcStateOf(pc);
  const String wanted = st && st->media >= 0 && r.coverPx ? String(st->art) : String();
  if (wanted != r.artWanted || (r.art && r.art->px != r.coverPx)) {
    r.artWanted = wanted;
    r.artAttempt = 0;
    mediaSetArt(r, {});
  }
  if (!wanted.length() || r.art) return;
  for (const TileRt& other : s_tiles) {
    if (other.art && other.art->id == wanted && other.art->px == r.coverPx) {
      mediaSetArt(r, other.art);
      return;
    }
  }
  if (!pc || pc->viaSerial || !pc->artPort || (r.artAttempt && millis() - r.artAttempt < 10000)) return;
  if (httpArtFetch(pc->ip, pc->artPort, wanted.c_str(), r.coverPx)) {
    for (TileRt& other : s_tiles)
      if (other.def->type == TILE_MEDIA && other.artWanted == wanted && other.coverPx == r.coverPx)
        other.artAttempt = millis() ? millis() : 1;
  }
}

void applyMedia(TileRt& r) {
  const PcState* st = pcStateOf(pcSource(r.def->pc));
  const bool available = st && st->media >= 0;
  const char* title = available && st->title[0] ? st->title : "Nothing playing";
  const char* artist = available ? st->artist : "";
  const uint32_t cp = available && st->media == PC_MEDIA_PLAYING ? 1 : 0;
  bool changed = strcmp(lv_label_get_text(r.name), title) || strcmp(lv_label_get_text(r.sub), artist);
  setText(r.name, title);
  setText(r.sub, artist);
  const lv_opa_t opacity = available ? LV_OPA_COVER : LV_OPA_50;
  if (lv_obj_get_style_opa(r.obj, LV_PART_MAIN) != opacity) {
    lv_obj_set_style_opa(r.obj, opacity, 0);
    changed = true;
  }
  if (r.iconCp != cp) {
    mediaGlyph(r.mediaIcons[1], cp ? "pause" : "play", cp ? "||" : ">", 24);
    r.iconCp = cp;
    changed = true;
  }
  mediaCover(r);
  mediaProgress(r);
  if (changed) markPageStale(r.page);
}

void mediaArtTick() {
  String id;
  int px;
  uint8_t* pixels = nullptr;
  if (!httpArtTake(id, px, pixels)) return;
  std::shared_ptr<MediaArt> art;
  if (pixels) {
    art = std::make_shared<MediaArt>();
    art->id = id;
    art->px = px;
    art->pixels = pixels;
    art->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    art->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    art->dsc.header.w = art->dsc.header.h = px;
    art->dsc.header.stride = px * 2;
    art->dsc.data_size = px * px * 2;
    art->dsc.data = pixels;
  }
  for (TileRt& r : s_tiles) {
    if (r.def->type != TILE_MEDIA || r.coverPx != px) continue;
    const PcState* st = pcStateOf(pcSource(r.def->pc));
    if (!st || st->media < 0 || id != st->art) continue;
    r.artWanted = id;
    r.artAttempt = millis() ? millis() : 1;
    if (art) mediaSetArt(r, art);
  }
}

void mediaEvent(lv_event_t* e) {
  const size_t tag = (size_t)(uintptr_t)lv_event_get_user_data(e), idx = tag / 4;
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const int button = tag % 4;
  if (button == 3 && r.mediaFull) return;
  hapticTap();
  static const char* const cmds[] = {"media_prev", "media_play_pause", "media_next", "media_play_pause"};
  String err;
  if (!pcSendCommand(r.def->pc, cmds[button], err)) deckToastShow(err.c_str(), 2200);
}

void fitMedia(TileRt& r, int cw, int ch) {
  r.mediaFull = cw >= 150 && ch >= 112;
  const int topH = r.mediaFull ? ch - 48 : ch;
  const int cover = cw >= 150 && topH >= 40 ? min(200, min(topH, cw * 3 / 8)) : 0;
  if (r.coverPx != cover) {
    mediaSetArt(r, {});
    r.coverPx = cover;
    r.artAttempt = 0;
  }
  showObj(r.chip, cover > 0);
  if (cover) {
    lv_obj_set_size(r.chip, cover, cover);
    lv_obj_set_pos(r.chip, 0, (topH - cover) / 2);
  }
  const int left = cover ? cover + 10 : 0, width = max(1, cw - left);
  const lv_font_t* font = cw >= 150 ? &deck_font_14 : &deck_font_12;
  const int titleH = min(topH, textH(font, 2));
  const bool artist = cover && topH >= titleH + lineH(&deck_font_12) + 4;
  const int blockH = titleH + (artist ? lineH(&deck_font_12) + 4 : 0);
  setFont(r.name, font);
  lv_obj_set_style_text_line_space(r.name, NAME_LINE_SP, 0);
  lv_obj_set_pos(r.name, left, max(0, (topH - blockH) / 2));
  lv_obj_set_size(r.name, width, titleH);
  setFont(r.sub, &deck_font_12);
  lv_obj_set_pos(r.sub, left, max(0, (topH - blockH) / 2) + titleH + 4);
  lv_obj_set_size(r.sub, width, lineH(&deck_font_12));
  showObj(r.sub, artist);
  lv_obj_set_pos(r.viz, 0, ch - 44);
  lv_obj_set_size(r.viz, cw, 4);
  for (int i = 0; i < 3; i++) {
    showObj(r.mediaButtons[i], r.mediaFull);
    lv_obj_set_pos(r.mediaButtons[i], i * cw / 3, ch - 34);
    lv_obj_set_size(r.mediaButtons[i], cw / 3, 34);
  }
  applyMedia(r);
}

// Clock: the largest time that fits, the date under it when there is room
// (a one step smaller time is worth it for the date).
void fitClock(TileRt& r, int cw, int ch) {
  static const lv_font_t* const kTime[] = {&deck_font_clock_72, &deck_font_clock_56, &lv_font_montserrat_40,
                                           &deck_font_28, &deck_font_20, &deck_font_14};
  constexpr int N = sizeof(kTime) / sizeof(kTime[0]);
  const lv_font_t* df = ch >= 120 ? &deck_font_20 : &deck_font_14;
  const bool ampm = !netSettings.use24h;
  auto unitFont = [](const lv_font_t* f) { return lineH(f) >= 48 ? &deck_font_20 : &deck_font_14; };
  int best = N - 1, dated = -1;
  for (int i = N - 1; i >= 0; i--) {
    const lv_font_t* f = kTime[i];
    // Digits have no accents or descenders: a quarter of the line box is air.
    const int w = textW("00:00", f) + (ampm ? 4 + textW("PM", unitFont(f)) : 0), h = lineH(f) * 3 / 4;
    if (w > cw || h > ch) continue;
    best = i;
    if (h + lineH(df) <= ch) dated = i;
  }
  const bool date = dated >= 0 && dated <= best + 1;
  const lv_font_t* f = kTime[date ? dated : best];
  setFont(r.sub, f);
  setFont(r.colon, f);
  setFont(r.mins, f);
  // Fonts put the colon at x-height middle: centre it on the digits instead.
  lv_font_glyph_dsc_t dg = {}, dc = {};
  if (lv_font_get_glyph_dsc(f, &dg, '0', 0) && lv_font_get_glyph_dsc(f, &dc, ':', 0))
    lv_obj_set_style_translate_y(r.colon, (dc.ofs_y * 2 + dc.box_h - dg.ofs_y * 2 - dg.box_h) / 2, 0);
  setFont(r.unit, unitFont(f));
  lv_obj_set_style_pad_bottom(r.unit, (lineH(f) - lineH(unitFont(f))) / 5, 0);  // share the baseline
  setFont(r.name, df);
  lv_obj_set_style_margin_top(r.name, -lineH(f) / 8, 0);
  showObj(r.name, date);
}

}  // namespace deckui
