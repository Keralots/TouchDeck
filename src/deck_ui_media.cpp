// Deck UI: the Now playing and clock tiles.
#include "deck_ui_int.h"

namespace deckui {

// --- Now playing ----------------------------------------------------------------------
// What the tile shows: from a PC (companion) or, with an entity, from a Home
// Assistant media_player (#6).
struct MediaNow {
  bool available = false;
  bool playing = false;
  bool fromInput = false;     // HA player playing one of its inputs (the PC over optical, a TV)
  String title, artist, art;  // art: cover id, "" = none
  String picture;             // HA entity_picture
  time_t t0 = 0;              // track start while playing, 0 = unknown
  int32_t pos = 0, dur = 0;   // seconds; pos is used while paused
  int vol = -1;               // 0..100, -1 = unknown
};

// The proxy token in entity_picture rotates; the cover id must not.
String pictureId(const String& picture) {
  String key = picture;
  const int t = key.indexOf("token=");
  if (t >= 0) {
    const int end = key.indexOf('&', t);
    key.remove(t, end < 0 ? key.length() - t : end - t + 1);
  }
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < key.length(); i++) h = (h ^ (uint8_t)key[i]) * 16777619u;
  char id[9];
  snprintf(id, sizeof(id), "%08x", (unsigned)h);
  return id;
}

MediaNow haNow(const TileDef& d) {
  MediaNow m;
  HaEntity e;
  if (!haGetEntity(d.entity, e) || !e.known || e.state == "unavailable" || e.state == "unknown" ||
      e.state == "off")
    return m;
  m.available = true;
  m.playing = e.state == "playing";
  m.title = e.mediaTitle;
  m.artist = e.mediaArtist;
  m.dur = e.mediaDuration;
  m.pos = e.mediaPosition;
  if (m.playing && e.mediaPosAt) m.t0 = e.mediaPosAt - e.mediaPosition;
  // A remotely accessible image puts its own URL in entity_picture and the
  // HA proxy path in entity_picture_local.
  m.picture = e.mediaPicture.length() ? e.mediaPicture : e.mediaPictureRemote;
  if (m.picture.length()) m.art = pictureId(m.picture);
  m.vol = e.level;
  m.fromInput = e.mediaSource.length() && e.mediaInputs.indexOf("\n" + e.mediaSource + "\n") >= 0;
  return m;
}

MediaNow pcNow(const TileDef& d) {
  MediaNow m;
  const PcState* st = pcStateOf(pcSource(d.pc));
  if (!st || st->media < 0) return m;
  m.available = true;
  m.playing = st->media == PC_MEDIA_PLAYING;
  m.title = st->title;
  m.artist = st->artist;
  m.art = st->art;
  m.t0 = st->t0;
  m.pos = st->pos;
  m.dur = st->dur;
  m.vol = st->vol;
  return m;
}

MediaNow mediaNow(const TileRt& r) { return r.mediaHa ? haNow(*r.def) : pcNow(*r.def); }

// Auto tiles: whichever plays, the PC when both do. An HA player playing one of
// its inputs is fed by another device and does not count. Else keep the last.
bool mediaChoose(TileRt& r) {
  const bool was = r.mediaHa;
  if (!r.def->mediaAuto) {
    r.mediaHa = r.def->entity.length() > 0;
  } else if (!r.volHeld) {  // a held volume pill keeps its source
    const MediaNow ha = haNow(*r.def), pc = pcNow(*r.def);
    if (pc.playing) r.mediaHa = false;
    else if (ha.playing && !ha.fromInput) r.mediaHa = true;
    else if (!(r.mediaHa ? ha : pc).available && (r.mediaHa ? pc : ha).available) r.mediaHa = !r.mediaHa;
  }
  return r.mediaHa != was;
}

void mediaGlyph(lv_obj_t* label, const char* name, const char* fallback, int px) {
  const uint32_t cp = iconCodepoint(name);
  const lv_font_t* font = cp ? iconFont(px, cp) : nullptr;
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

void mediaProgress(TileRt& r, const MediaNow& m) {
  const time_t now = time(nullptr);
  const bool visible = r.mediaFull && m.available && m.dur > 0 && now >= 1609459200 && (!m.playing || m.t0 > 0);
  bool changed = visible == lv_obj_has_flag(r.viz, LV_OBJ_FLAG_HIDDEN);
  showObj(r.viz, visible);
  if (visible) {
    const int64_t elapsed = m.playing ? (int64_t)now - m.t0 : m.pos;
    const int value = (int)(std::max((int64_t)0, std::min(elapsed, (int64_t)m.dur)) * 1000 / m.dur);
    if (value != lv_bar_get_value(r.viz)) {
      lv_bar_set_value(r.viz, value, LV_ANIM_OFF);
      changed = true;
    }
  }
  if (changed) markPageStale(r.page);
}

void mediaCover(TileRt& r, const MediaNow& m) {
  const bool ha = r.mediaHa;
  const PcSource* pc = ha ? nullptr : pcSource(r.def->pc);
  const String wanted = m.available && r.coverPx ? m.art : String();
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
  if (r.artAttempt && millis() - r.artAttempt < 10000) return;
  bool queued;
  if (ha) {
    if (wanted == r.artFailed && millis() - r.artAttempt < 120000) return;
    const String url = m.picture.startsWith("/") ? haBaseUrl() + m.picture : m.picture;
    if (!url.startsWith("http://")) {  // https or no HA address: back off like a failed cover
      r.artFailed = wanted;
      r.artAttempt = millis() ? millis() : 1;
      return;
    }
    queued = httpArtFetchJpeg(url, wanted.c_str(), r.coverPx);
  } else {
    if (!pc || !pc->artPort || !(uint32_t)pc->ip) return;
    queued = httpArtFetch(pc->ip, pc->artPort, wanted.c_str(), r.coverPx);
  }
  if (queued) {
    for (TileRt& other : s_tiles)
      if (other.def->type == TILE_MEDIA && other.artWanted == wanted && other.coverPx == r.coverPx)
        other.artAttempt = millis() ? millis() : 1;
  }
}

// The level the finger asked for stands until the source reports it.
int mediaVolShown(const TileRt& r, const MediaNow& m) {
  if (r.mediaHa) {
    const Wish* w = findWish(r.def->entity);
    return w && w->level.active ? (int)lroundf(w->level.value * 100) : m.vol;
  }
  const int want = pcVolWanted(r.def->pc);
  return want >= 0 ? want : m.vol;
}

void mediaVolume(TileRt& r, const MediaNow& m) {
  const bool visible = r.mediaVol && m.available && m.vol >= 0;
  bool changed = visible == lv_obj_has_flag(r.volBar, LV_OBJ_FLAG_HIDDEN);
  if (changed) s_zonesDirty = true;
  showObj(r.volBar, visible);
  const int value = mediaVolShown(r, m);
  if (visible && value != lv_bar_get_value(r.volBar)) {
    lv_bar_set_value(r.volBar, value, LV_ANIM_OFF);
    // Each icon takes the on-accent colour once the fill is under it.
    for (int i = 0; i < 2; i++) {
      if (value >= (i ? 85 : 20)) lv_obj_set_style_text_color(r.volIcons[i], lv_color_hex(C_TEXT_ON), 0);
      else lv_obj_remove_local_style_prop(r.volIcons[i], LV_STYLE_TEXT_COLOR, 0);
    }
    changed = true;
  }
  if (changed) markPageStale(r.page);
}

// Once a second: progress and a cover that may be due.
void mediaTick(TileRt& r) {
  const MediaNow m = mediaNow(r);
  mediaProgress(r, m);
  mediaCover(r, m);
}

// The HA level goes through the shared wish, the PC level through the slider
// tile's throttle.
void mediaVolSet(TileRt& r, int pct, bool dragging) {
  if (r.mediaHa) {
    setWish(r.def->entity, true, pct / 100.0f, dragging);
    return;
  }
  pcVolumeAsk(r.def->pc, pct);
  mediaVolume(r, mediaNow(r));
}

void mediaVolEnd(TileRt& r) {
  if (!r.volHeld) return;
  r.volHeld = false;
  if (r.def->entity.length()) releaseLevel(r.def->entity);
}

// The volume pill: a drag along it moves the level from where it was (like a
// slider tile), a tap on its upper or lower half steps to the next 5 %, and
// holding a half keeps moving the level.
void mediaVolEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    mediaVolEnd(r);
    return;
  }
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  lv_area_t a;
  lv_obj_get_coords(r.volBar, &a);
  const bool upright = lv_area_get_height(&a) > lv_area_get_width(&a);
  const int len = max(1, (int)(upright ? lv_area_get_height(&a) : lv_area_get_width(&a)));
  const int at = upright ? a.y2 - p.y : p.x - a.x1;  // along the pill from its quiet end
  if (code == LV_EVENT_PRESSED) {
    r.sliding = false;  // stays set after the release: RELEASED comes before SHORT_CLICKED
    r.slX0 = at;
    return;
  }
  const int base = mediaVolShown(r, mediaNow(r));
  if (base < 0) return;
  const bool can = r.mediaHa ? haLink() == HA_CONNECTED : pcIsOnline(r.def->pc);
  if (code == LV_EVENT_PRESSING) {
    if (!r.sliding) {
      if (!can || r.volHeld || abs(at - r.slX0) < 8) return;
      r.sliding = r.volHeld = true;
      r.slX0 = at;
      r.slFrac0 = r.slValue = base / 100.0f;
      if (r.mediaHa) holdLevel(r.def->entity, r.slFrac0);
    }
    const float f = constrain(r.slFrac0 + (at - r.slX0) / (float)len, 0.0f, 1.0f);
    if (lroundf(f * 100) == lroundf(r.slValue * 100)) return;
    r.slValue = f;
    mediaVolSet(r, (int)lroundf(f * 100), true);
    return;
  }
  if (r.sliding) return;
  const bool tap = code == LV_EVENT_SHORT_CLICKED, up = at * 2 >= len;
  if (!can) {
    if (tap) deckToastShow(r.mediaHa ? "Home Assistant is not connected" : "The PC is offline", 2200);
    return;
  }
  if (tap || !r.volHeld) {
    hapticTap();
    audioClick();
  }
  if (!tap) r.volHeld = true;
  const int want = constrain(tap ? (up ? base / 5 * 5 + 5 : (base + 4) / 5 * 5 - 5) : base + (up ? 2 : -2), 0, 100);
  if (want != base) mediaVolSet(r, want, !tap);
}

void applyMedia(TileRt& r) {
  if (mediaChoose(r)) r.iconCp = 2;
  const MediaNow m = mediaNow(r);
  const char* title = m.available && m.title.length() ? m.title.c_str() : "Nothing playing";
  const char* artist = m.available ? m.artist.c_str() : "";
  const uint32_t cp = m.playing ? 1 : 0;
  bool changed = strcmp(lv_label_get_text(r.name), title) || strcmp(lv_label_get_text(r.sub), artist);
  setText(r.name, title);
  setText(r.sub, artist);
  const lv_opa_t opacity = m.available ? LV_OPA_COVER : LV_OPA_50;
  if (lv_obj_get_style_opa(r.obj, LV_PART_MAIN) != opacity) {
    lv_obj_set_style_opa(r.obj, opacity, 0);
    changed = true;
  }
  if (r.iconCp != cp) {
    mediaGlyph(r.mediaIcons[1], cp ? "pause" : "play", cp ? "||" : ">", r.mediaPx ? r.mediaPx : 24);
    r.iconCp = cp;
    changed = true;
  }
  mediaCover(r, m);
  mediaProgress(r, m);
  mediaVolume(r, m);
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
    if (mediaNow(r).art != id) continue;
    r.artWanted = id;
    r.artAttempt = millis() ? millis() : 1;
    if (art) mediaSetArt(r, art);
    else if (r.mediaHa) r.artFailed = id;
  }
}

void mediaEvent(lv_event_t* e) {
  const size_t tag = (size_t)(uintptr_t)lv_event_get_user_data(e), idx = tag / 8;
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const int button = tag % 8;
  if (button == 3 && r.mediaFull) return;
  hapticTap();
  audioClick();
  if (r.mediaHa) {
    static const char* const services[] = {"media_previous_track", "media_play_pause", "media_next_track",
                                           "media_play_pause"};
    if (!haCallService("media_player", services[button], r.def->entity, ""))
      deckToastShow("Home Assistant is not connected", 2200);
    return;
  }
  static const char* const cmds[] = {"media_prev", "media_play_pause", "media_next", "media_play_pause"};
  String err;
  if (!pcSendCommand(r.def->pc, cmds[button], err)) deckToastShow(err.c_str(), 2200);
}

// Bigger tiles get bigger transport buttons and, with room, a volume row (#6):
// under the transport on tall tiles, beside it on wide ones.
void fitMedia(TileRt& r, int cw, int ch) {
  r.mediaFull = cw >= 150 && ch >= 112;
  const int rowH = r.mediaFull ? constrain(ch * 3 / 10, 34, 52) : 0;
  const bool wide = r.mediaFull && cw >= 300;
  const int rowY = ch - rowH;
  const int topH = r.mediaFull ? rowY - 14 : ch;
  // Tall room: a big cover with the volume beside it, over a one-line title
  // and artist. Else the cover beside the text.
  constexpr int PILL = 52, GAP = 10;
  const int side = cw >= 150 && topH >= 40 ? min(200, min(topH, cw * 3 / 8)) : 0;
  const int stackText = lineH(&deck_font_20) + 4 + lineH(&deck_font_12);
  const int stacked = r.mediaFull && !wide ? min(200, min(cw - PILL - GAP, topH - stackText - 8)) : 0;
  const bool stack = stacked >= 90;
  r.mediaVol = wide || stack;
  const int cover = stack ? stacked : side;
  if (r.coverPx != cover) {
    mediaSetArt(r, {});
    r.coverPx = cover;
    r.artAttempt = 0;
  }
  showObj(r.chip, cover > 0);
  const lv_font_t* font = stack ? &deck_font_20 : cw >= 150 ? &deck_font_14 : &deck_font_12;
  setFont(r.name, font);
  lv_obj_set_style_text_line_space(r.name, NAME_LINE_SP, 0);
  setFont(r.sub, &deck_font_12);
  const lv_text_align_t align = stack ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
  lv_obj_set_style_text_align(r.name, align, 0);
  lv_obj_set_style_text_align(r.sub, align, 0);
  if (stack) {
    const int y = max(0, (topH - cover - 8 - stackText) / 2);
    const int x = (cw - cover - GAP - PILL) / 2;
    lv_obj_set_size(r.chip, cover, cover);
    lv_obj_set_pos(r.chip, x, y);
    lv_obj_set_pos(r.volBar, x + cover + GAP, y);
    lv_obj_set_size(r.volBar, PILL, cover);
    lv_obj_align(r.volIcons[0], LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_align(r.volIcons[1], LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_pos(r.name, 0, y + cover + 8);
    lv_obj_set_size(r.name, cw, lineH(font));
    lv_obj_set_pos(r.sub, 0, y + cover + 8 + lineH(font) + 4);
    lv_obj_set_size(r.sub, cw, lineH(&deck_font_12));
    showObj(r.sub, true);
  } else {
    if (cover) {
      lv_obj_set_size(r.chip, cover, cover);
      lv_obj_set_pos(r.chip, 0, (topH - cover) / 2);
    }
    const int left = cover ? cover + 10 : 0, width = max(1, cw - left);
    const int titleH = min(topH, textH(font, 2));
    const bool artist = cover && topH >= titleH + lineH(&deck_font_12) + 4;
    const int blockH = titleH + (artist ? lineH(&deck_font_12) + 4 : 0);
    lv_obj_set_pos(r.name, left, max(0, (topH - blockH) / 2));
    lv_obj_set_size(r.name, width, titleH);
    lv_obj_set_pos(r.sub, left, max(0, (topH - blockH) / 2) + titleH + 4);
    lv_obj_set_size(r.sub, width, lineH(&deck_font_12));
    showObj(r.sub, artist);
  }
  lv_obj_set_pos(r.viz, 0, rowY - 10);
  lv_obj_set_size(r.viz, cw, 4);
  const int tw = wide ? min(cw / 2, 3 * 72) : cw;
  const int px = rowH >= 44 ? 32 : 24;
  const bool disc = rowH >= 44;
  static const char* const icons[] = {"skip-previous", "skip-next"};
  static const char* const fallback[] = {"|<", ">|"};
  for (int i = 0; i < 3; i++) {
    showObj(r.mediaButtons[i], r.mediaFull);
    lv_obj_set_pos(r.mediaButtons[i], i * tw / 3, rowY);
    lv_obj_set_size(r.mediaButtons[i], (i + 1) * tw / 3 - i * tw / 3, rowH);
    if (i != 1 && px != r.mediaPx) mediaGlyph(r.mediaIcons[i], icons[i / 2], fallback[i / 2], px);
  }
  if (px != r.mediaPx) {
    r.mediaPx = px;
    r.iconCp = 2;  // applyMedia sets the play glyph at the new size
  }
  showObj(r.mediaDisc, disc);
  if (disc) {
    lv_obj_set_size(r.mediaDisc, rowH - 4, rowH - 4);
    lv_obj_center(r.mediaDisc);
    lv_obj_set_style_text_color(r.mediaIcons[1], lv_color_hex(C_TEXT_ON), 0);
  } else {
    lv_obj_remove_local_style_prop(r.mediaIcons[1], LV_STYLE_TEXT_COLOR, 0);
  }
  if (wide) {
    const int h = min(rowH, 44);
    lv_obj_set_pos(r.volBar, tw + 12, rowY + (rowH - h) / 2);
    lv_obj_set_size(r.volBar, cw - tw - 12, h);
    lv_obj_align(r.volIcons[0], LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_align(r.volIcons[1], LV_ALIGN_RIGHT_MID, -10, 0);
  }
  s_zonesDirty = true;
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
