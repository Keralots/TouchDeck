// Deck UI: the Now playing and clock tiles.
#include "deck_ui_clock.h"

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
  bool inputs = false;        // HA player with a source_list (inputs, presets)
  String source;              // HA source now
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
  m.fromInput = haPlaysInput(e);
  m.inputs = e.mediaInputs.length() > 0;
  m.source = e.mediaSource;
  return m;
}

bool haPlaysInput(const HaEntity& e) {
  return e.mediaSource.length() && e.mediaInputs.indexOf("\n" + e.mediaSource + "\n") >= 0;
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
  const String wanted = m.available && r.coverPx && !r.off ? m.art : String();
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

void refreshPcMediaVolume(TileRt& r) {
  MediaNow m;
  if (const PcState* st = pcStateOf(pcSource(r.def->pc)); st && st->media >= 0) {
    m.available = true;
    m.vol = st->vol;
  }
  mediaVolume(r, m);
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
  if (!r.sliding && !tileOpen(r)) {
    if (code == LV_EVENT_SHORT_CLICKED) tileGuard(r, nullptr);
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
#if BOARD_HAS_ENCODER
  if (!r.sliding && (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED)) encoderFocus(r);
#endif
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

// Source icon: the input's kind when one plays, else a plain "pick a source".
const char* sourceGlyph(const String& source, bool input) {
  if (!input) return "import";
  String s = source;
  s.toLowerCase();
  if (s.indexOf("hdmi") >= 0 || s.indexOf("arc") >= 0 || s.indexOf("tv") >= 0) return "video-input-hdmi";
  if (s.indexOf("optical") >= 0 || s.indexOf("spdif") >= 0 || s.indexOf("toslink") >= 0) return "toslink";
  if (s.indexOf("bluetooth") >= 0) return "bluetooth";
  if (s.indexOf("analog") >= 0 || s.indexOf("line") >= 0 || s.indexOf("aux") >= 0 || s.indexOf("phono") >= 0)
    return "audio-input-rca";
  return "import";
}

// The cover of a big enough HA tile opens the source card; the badge says so.
bool mediaBadge(TileRt& r, const MediaNow& m) {
  const bool on = r.mediaHa && m.available && m.inputs && r.coverPx >= 72;
  const bool was = !lv_obj_has_flag(r.srcBadge, LV_OBJ_FLAG_HIDDEN);
  if (on) lv_obj_add_flag(r.chip, LV_OBJ_FLAG_CLICKABLE);
  else lv_obj_remove_flag(r.chip, LV_OBJ_FLAG_CLICKABLE);
  showObj(r.srcBadge, on);
  const char* glyph = on ? sourceGlyph(m.source, m.fromInput) : nullptr;
  if (glyph && glyph != r.srcGlyph) {
    mediaGlyph(r.srcBadge, glyph, "IN", r.coverPx >= 120 ? 20 : 16);
    r.srcGlyph = glyph;
    return true;
  }
  return on != was;
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
  if (mediaBadge(r, m)) changed = true;
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
#if BOARD_HAS_ENCODER
  encoderFocus(r);
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) return;
#endif
  const int button = tag % 8;
  if (button == 3 && r.mediaFull) return;
  hapticTap();
  audioClick();
  if (!tileOpen(r)) return tileGuard(r, nullptr);
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

// The source card: the HA player's source_list, the current one lit. A pick
// goes to media_player.select_source and closes the card.
std::vector<String> s_srcNames;
String s_srcEntity;

void onSourcePick(lv_event_t* e) {
  const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (i < s_srcNames.size()) {
    hapticTap();
    audioClick();
    JsonDocument doc;
    doc["source"] = s_srcNames[i];
    String data;
    serializeJson(doc, data);
    if (!haCallService("media_player", "select_source", s_srcEntity, data))
      deckToastShow("Home Assistant is not connected", 2200);
  }
  closeModal();
}

void openMediaSources(TileRt& r) {
  HaEntity e;
  if (!haGetEntity(r.def->entity, e) || !e.mediaInputs.length()) return;
  closeModal();
  cleanPage();
  s_srcEntity = r.def->entity;
  s_srcNames.clear();
  for (int from = 1, end; (end = e.mediaInputs.indexOf('\n', from)) > from; from = end + 1)
    s_srcNames.push_back(e.mediaInputs.substring(from, end));
  const int w = logicalW(), h = logicalH();
  constexpr int ROW = 52, GAP = 8, PAD = 16, TITLE = 28;
  const int cw = min(w - 32, 360);
  const int ch = min(h - 40, PAD * 2 + TITLE + (int)s_srcNames.size() * (ROW + GAP));
  s_modalScrim = makeScrim();
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS && (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_pad_all(c, PAD, 0);
  lv_obj_set_style_pad_row(c, GAP, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // a tap beside the rows does not reach the scrim

  lv_obj_t* title = lv_label_create(c);
  lv_label_set_text(title, "Source");
  lv_obj_set_style_text_font(title, &deck_font_20, 0);
  lv_obj_set_height(title, TITLE - GAP);

  const uint32_t accent = tileAccent(*r.def);
  for (size_t i = 0; i < s_srcNames.size(); i++) {
    const bool cur = s_srcNames[i] == e.mediaSource;
    lv_obj_t* row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(row, lv_pct(100), ROW);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(cur ? accent : C_TRACK), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(row, lv_color_hex(cur ? C_TEXT_ON : C_TEXT), 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t* icon = lv_label_create(row);
    mediaGlyph(icon, sourceGlyph(s_srcNames[i], true), "", 24);
    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, s_srcNames[i].c_str());
    lv_obj_set_style_text_font(name, &deck_font_20, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_add_event_cb(row, onSourcePick, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
  }
  setModalOpen(true);
}

void mediaSrcEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  if (!r.mediaHa) return;
  hapticTap();
  audioClick();
  tileGuard(r, [](TileRt& t) { openMediaSources(t); });
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
  const int side = cw >= 150 && topH >= 40 ? min(BOARD_MEDIA_ART_MAX, min(topH, cw * 3 / 8)) : 0;
  const int stackText = lineH(&deck_font_20) + 4 + lineH(&deck_font_12);
  const int stacked = r.mediaFull && !wide ? min(BOARD_MEDIA_ART_MAX, min(cw - PILL - GAP, topH - stackText - 8)) : 0;
  const bool stack = stacked >= 90;
  r.mediaVol = wide || stack;
  const int cover = stack ? stacked : side;
  if (r.coverPx != cover) {
    mediaSetArt(r, {});
    r.coverPx = cover;
    r.artAttempt = 0;
    r.srcGlyph = nullptr;  // the badge icon follows the cover size
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

// Digit ink in a line box: rows of air above it and its height. The clock
// fonts are digits only and have no air; text fonts do.
void digitInk(const lv_font_t* f, int& top, int& h) {
  lv_font_glyph_dsc_t g = {};
  if (!lv_font_get_glyph_dsc(f, &g, '0', 0)) {
    top = lineH(f) / 8;
    h = lineH(f) * 3 / 4;
    return;
  }
  top = lineH(f) - f->base_line - (g.ofs_y + g.box_h);
  h = g.box_h;
}

// Date ink: from the digit tops down to the descenders (Sep, Aug, May).
void dateInk(const lv_font_t* f, int& top, int& h) {
  digitInk(f, top, h);
  lv_font_glyph_dsc_t g = {};
  const int bot = lv_font_get_glyph_dsc(f, &g, 'p', 0) ? lineH(f) - f->base_line - g.ofs_y : lineH(f);
  h = LV_MAX(h, bot - top);
}

// Clock: the largest time that fits, the date under it when there is room
// (a one step smaller time is worth it for the date). "date": "always" takes
// a tighter gap first, then any smaller time. Fit and centring go by the ink,
// not by the line boxes.
void fitClock(TileRt& r, int cw, int ch) {
  if (r.cfx) {
    fitClockFx(r, cw, ch);
    return;
  }
  if (r.pillW) {
    lv_obj_set_style_pad_right(r.obj, lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN) + r.pillW, 0);
    cw -= r.pillW;
  }
  const lv_font_t* df = ch >= 120 ? &deck_font_20 : &deck_font_14;
  const bool ampm = !netSettings.use24h;
  auto unitFont = [](const lv_font_t* f) { return lineH(f) >= 48 ? &deck_font_20 : &deck_font_14; };
  int dTop, dH;
  dateInk(df, dTop, dH);
  const int gap = lv_obj_get_style_pad_row(r.obj, LV_PART_MAIN);  // theme gap between time and date
  constexpr int MIN_GAP = 5;  // ink rows between digits and date when "always" tightens it
  int cut = 0;                // rows taken off the gap
  // Ink rows from the top of the column; the date line sits L/8 up into the time's.
  auto dateY = [&](const lv_font_t* f) { return lineH(f) - lineH(f) / 8 + gap - cut; };
  auto inkSpan = [&](const lv_font_t* f, bool withDate, int& top, int& bot) {
    int h;
    digitInk(f, top, h);
    bot = withDate ? dateY(f) + dTop + dH : top + h;
  };
  int best = CLOCK_FONTS - 1, dated = -1;
  for (int i = CLOCK_FONTS - 1; i >= 0; i--) {
    const lv_font_t* f = CLOCK_FONT[i];
    const int w = textW("00:00", f) + (ampm ? 4 + textW("PM", unitFont(f)) : 0);
    int top, bot;
    inkSpan(f, false, top, bot);
    if (w > cw || bot - top > ch) continue;
    best = i;
    inkSpan(f, true, top, bot);
    if (bot - top <= ch) dated = i;
  }
  // Fonts from best down already fit the width and, without the date, the height.
  if (r.def->clockDate) {
    const int last = dated < 0 ? CLOCK_FONTS : dated;
    for (int i = best; i < last; i++) {
      const lv_font_t* f = CLOCK_FONT[i];
      int top, bot, h;
      inkSpan(f, true, top, bot);
      digitInk(f, top, h);
      const int over = bot - top - ch, room = dateY(f) + dTop - (top + h) - MIN_GAP;
      if (over <= room) {
        cut = LV_MAX(0, over);
        dated = i;
        break;
      }
    }
  }
  const bool date = dated >= 0 && (dated <= best + 1 || r.def->clockDate);
  const lv_font_t* f = CLOCK_FONT[date ? dated : best];
  // Flex centres the boxes; move them so the ink is centred instead.
  int top, bot;
  inkSpan(f, date, top, bot);
  const int boxH = date ? dateY(f) + lineH(df) : lineH(f);
  const int shift = boxH / 2 - (top + bot) / 2;
  lv_obj_set_style_translate_y(lv_obj_get_parent(r.unit), shift, 0);
  lv_obj_set_style_translate_y(r.name, shift, 0);
  setFont(r.sub, f);
  setFont(r.colon, f);
  setFont(r.mins, f);
  lv_obj_set_style_translate_y(r.colon, clockColonDy(f), 0);
  setFont(r.unit, unitFont(f));
  lv_obj_set_style_pad_bottom(r.unit, (lineH(f) - lineH(unitFont(f))) / 5, 0);  // share the baseline
  setFont(r.name, df);
  lv_obj_set_style_margin_top(r.name, -lineH(f) / 8 - cut, 0);
  showObj(r.name, date);
}

}  // namespace deckui
