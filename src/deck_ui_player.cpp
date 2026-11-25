// Deck UI: the player card of a Home Assistant Now playing tile. Cover,
// transport and sources on the left; with a BluOS address also the play
// queue, shuffle, repeat and love/ban. While it is open the knobs play music:
// the first browses the queue (press plays), the second sets the volume.
#include "deck_ui_int.h"
#include "deck_bluos.h"

namespace deckui {
namespace {

constexpr int QUEUE_WINDOW = 40;
constexpr uint32_t KNOB_CLOSE_MS = 8000;  // a card the knob opened closes after this idle
enum { B_SHUFFLE, B_PREV, B_PLAY, B_NEXT, B_REPEAT, B_COUNT };

struct PlayerCard {
  int tile = -1;
  bool byKnob = false;
  uint32_t lastInput = 0;
  bool wide = false;
  int coverPx = 0;
  lv_obj_t* coverBox = nullptr;
  lv_obj_t* cover = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* artist = nullptr;
  lv_obj_t* love = nullptr;  // buttons, with their icons
  lv_obj_t* loveIco = nullptr;
  lv_obj_t* ban = nullptr;
  lv_obj_t* banIco = nullptr;
  lv_obj_t* bar = nullptr;
  lv_obj_t* tPos = nullptr;
  lv_obj_t* tDur = nullptr;
  lv_obj_t* quality = nullptr;
  lv_obj_t* btn[B_COUNT] = {};
  lv_obj_t* ico[B_COUNT] = {};
  lv_obj_t* head = nullptr;
  lv_obj_t* sub = nullptr;
  lv_obj_t* chips = nullptr;
  lv_obj_t* list = nullptr;
  std::vector<lv_obj_t*> rows;  // queue rows, rows[i] = queue index q.start + i
  lv_obj_t* eq = nullptr;  // animated bars on the playing row
  lv_obj_t* eqBars[3] = {};
  int eqOn = -1;           // bars moving (1) or still (0)
  lv_obj_t* earlier = nullptr;
  lv_obj_t* more = nullptr;
  deck::BluosQueue q;
  deck::BluosStatus st;
  bool bluos = false;   // the queue part is built
  int sel = -1;         // queue index the knob points at, -1 = none
  int rowsFor = -2;     // q.start the rows were built for
  uint32_t rowsPid = 0;
  int rowsLen = 0;
  int lit = -2;         // queue index shown as playing
  uint32_t seen = 0;    // bluosVersion applied
  uint32_t asked = 0;   // last window request
  int askedStart = -1;
  uint32_t glyphs = 0;  // icon state bits last set
  String sourcesKey;    // source list + current, as built
  std::vector<String> sources;
  String artWanted;
  uint32_t artAsked = 0;
  std::shared_ptr<MediaArt> art;
};
PlayerCard s_pc;

TileRt* cardTile() { return s_pc.tile >= 0 && s_pc.tile < (int)s_tiles.size() ? &s_tiles[s_pc.tile] : nullptr; }

void touched() { s_pc.lastInput = millis(); }

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  if (opa != LV_OPA_COVER) lv_obj_set_style_text_opa(l, opa, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* iconButton(lv_obj_t* parent, int size, lv_event_cb_t cb, uintptr_t tag, lv_obj_t** icon) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, size / 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, (void*)tag);
  *icon = lv_label_create(b);
  lv_obj_center(*icon);
  return b;
}

String clock(int32_t secs) {
  if (secs < 0) secs = 0;
  char t[12];
  snprintf(t, sizeof(t), "%d:%02d", (int)(secs / 60), (int)(secs % 60));
  return t;
}

bool haCall(const char* service, const String& data = "") {
  TileRt* r = cardTile();
  if (!r) return false;
  if (haCallService("media_player", service, r->def->entity, data)) return true;
  deckToastShow("Home Assistant is not connected", 2200);
  return false;
}

void playIndex(int index) {
  if (!deck::bluosCommand("/Play?id=" + String(index))) deckToastShow("The player does not answer", 2200);
  s_pc.sel = -1;
}

void onTransport(lv_event_t* e) {
  touched();
  hapticTap();
  audioClick();
  switch ((int)(uintptr_t)lv_event_get_user_data(e)) {
    case B_PREV: haCall("media_previous_track"); break;
    case B_PLAY: haCall("media_play_pause"); break;
    case B_NEXT: haCall("media_next_track"); break;
    case B_SHUFFLE: deck::bluosCommand(String("/Shuffle?state=") + (s_pc.st.shuffle ? "0" : "1")); break;
    case B_REPEAT: {
      // off -> queue -> track -> off
      const int next = s_pc.st.repeat == 2 ? 0 : s_pc.st.repeat == 0 ? 1 : 2;
      deck::bluosCommand("/Repeat?state=" + String(next));
      break;
    }
  }
}

void onLove(lv_event_t* e) {
  touched();
  hapticTap();
  audioClick();
  const bool ban = lv_event_get_user_data(e) != nullptr;
  const String& url = ban ? s_pc.st.banUrl : s_pc.st.loveUrl;
  if (url.length() && deck::bluosCommand(url)) deckToastShow(ban ? "Banned" : "Loved", 1200);
}

void onSource(lv_event_t* e) {
  touched();
  const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (i >= s_pc.sources.size()) return;
  hapticTap();
  audioClick();
  JsonDocument doc;
  doc["source"] = s_pc.sources[i];
  String data;
  serializeJson(doc, data);
  haCall("select_source", data);
}

void onRow(lv_event_t* e) {
  touched();
  hapticTap();
  audioClick();
  playIndex((int)(uintptr_t)lv_event_get_user_data(e));
}

void askWindow(int start) {
  start = max(0, start);
  if (start == s_pc.askedStart && millis() - s_pc.asked < 3000) return;
  s_pc.askedStart = start;
  s_pc.asked = millis();
  deck::bluosLoadQueue(start, QUEUE_WINDOW);
}

void onPage(lv_event_t* e) {
  touched();
  const bool later = lv_event_get_user_data(e) != nullptr;
  askWindow(later ? s_pc.q.start + (int)s_pc.q.tracks.size() - 3 : s_pc.q.start - QUEUE_WINDOW + 3);
}

void onListScroll(lv_event_t*) { touched(); }

// A row of the list: icon or number, title over artist, time on the right.
lv_obj_t* listRow(lv_obj_t* parent, int h) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_set_size(row, lv_pct(100), h);
  lv_obj_set_style_radius(row, 14, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(row, 12, 0);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return row;
}

void pageRow(bool later) {
  lv_obj_t* row = listRow(s_pc.list, 44);
  lv_obj_t* ic = lv_label_create(row);
  mediaGlyph(ic, later ? "chevron-down" : "chevron-up", later ? "v" : "^", 20);
  lv_obj_t* t = label(row, &deck_font_14, LV_OPA_70);
  lv_label_set_text(t, later ? "Later tracks" : "Earlier tracks");
  lv_obj_add_event_cb(row, onPage, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)later);
  (later ? s_pc.more : s_pc.earlier) = row;
}

void buildRows() {
  const deck::BluosQueue& q = s_pc.q;
  lv_obj_clean(s_pc.list);
  s_pc.eq = nullptr;  // went with the rows
  s_pc.eqOn = -1;
  s_pc.rows.clear();
  s_pc.earlier = s_pc.more = nullptr;
  s_pc.rowsFor = q.start;
  s_pc.rowsPid = q.pid;
  s_pc.rowsLen = (int)q.tracks.size();
  s_pc.lit = -2;
  if (q.start > 0) pageRow(false);
  for (size_t i = 0; i < q.tracks.size(); i++) {
    const deck::BluosTrack& t = q.tracks[i];
    lv_obj_t* row = listRow(s_pc.list, 54);
    lv_obj_set_style_outline_color(row, lv_color_hex(tileAccent(*s_tiles[s_pc.tile].def)), 0);
    lv_obj_set_style_outline_pad(row, -2, 0);
    lv_obj_t* n = label(row, &deck_font_12, LV_OPA_60);
    lv_obj_set_width(n, 28);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(n, "%d", q.start + (int)i + 1);
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_t* title = label(col, &deck_font_14);
    lv_obj_set_size(title, lv_pct(100), lineH(&deck_font_14));
    lv_label_set_text(title, t.title.c_str());
    lv_obj_t* artist = label(col, &deck_font_12, LV_OPA_60);
    lv_obj_set_size(artist, lv_pct(100), lineH(&deck_font_12));
    lv_label_set_text(artist, t.artist.c_str());
    lv_obj_t* time = label(row, &deck_font_12, LV_OPA_70);
    lv_label_set_text(time, t.secs ? clock(t.secs).c_str() : "");
    lv_obj_add_event_cb(row, onRow, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(q.start + (int)i));
    s_pc.rows.push_back(row);
  }
  if (q.start + (int)q.tracks.size() < q.length) pageRow(true);
}

lv_obj_t* rowFor(int index) {
  const int i = index - s_pc.q.start;
  return i >= 0 && i < (int)s_pc.rows.size() ? s_pc.rows[i] : nullptr;
}

void eqHeight(void* bar, int32_t v) { lv_obj_set_height((lv_obj_t*)bar, v); }

// Three bars that bounce while the player plays and rest when it pauses.
void eqAnimate(bool playing) {
  if (!s_pc.eq || s_pc.eqOn == (int)playing) return;
  s_pc.eqOn = playing;
  static const uint16_t ms[] = {420, 560, 360};
  static const uint8_t rest[] = {8, 14, 6};
  for (int i = 0; i < 3; i++) {
    lv_anim_delete(s_pc.eqBars[i], eqHeight);
    if (!playing) {
      lv_obj_set_height(s_pc.eqBars[i], rest[i]);
      continue;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pc.eqBars[i]);
    lv_anim_set_exec_cb(&a, eqHeight);
    lv_anim_set_values(&a, 4, 18);
    lv_anim_set_duration(&a, ms[i]);
    lv_anim_set_reverse_duration(&a, ms[i]);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, i * 130);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  }
}

void eqBuild(lv_obj_t* label, uint32_t color) {
  s_pc.eq = lv_obj_create(label);
  lv_obj_remove_style_all(s_pc.eq);
  lv_obj_remove_flag(s_pc.eq, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(s_pc.eq, 20, 18);
  lv_obj_center(s_pc.eq);
  for (int i = 0; i < 3; i++) {
    lv_obj_t* b = s_pc.eqBars[i] = lv_obj_create(s_pc.eq);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(b, 4, 8);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, i * 8, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  }
  s_pc.eqOn = -1;
}

// The playing row tinted, the knob's row outlined.
void markRows(bool scroll) {
  const uint32_t accent = tileAccent(*s_tiles[s_pc.tile].def);
  const int playing = s_pc.st.song;
  if (s_pc.lit != playing) {
    if (lv_obj_t* old = rowFor(s_pc.lit)) {
      lv_obj_set_style_bg_opa(old, LV_OPA_TRANSP, 0);
      lv_obj_t* n = lv_obj_get_child(old, 0);
      if (s_pc.eq) lv_obj_delete(s_pc.eq);
      s_pc.eq = nullptr;
      lv_label_set_text_fmt(n, "%d", s_pc.lit + 1);
    }
    if (lv_obj_t* now = rowFor(playing)) {
      lv_obj_set_style_bg_color(now, lv_color_hex(accent), 0);
      lv_obj_set_style_bg_opa(now, LV_OPA_30, 0);
      lv_obj_t* n = lv_obj_get_child(now, 0);
      lv_label_set_text(n, "");
      lv_obj_set_height(n, 20);
      eqBuild(n, accent);
      if (scroll && s_pc.sel < 0) lv_obj_scroll_to_view(now, LV_ANIM_ON);
    }
    s_pc.lit = playing;
  }
  for (size_t i = 0; i < s_pc.rows.size(); i++)
    lv_obj_set_style_outline_width(s_pc.rows[i], s_pc.q.start + (int)i == s_pc.sel ? 2 : 0, 0);
  if (lv_obj_t* r = rowFor(s_pc.sel)) {
    if (scroll) lv_obj_scroll_to_view(r, LV_ANIM_ON);
  }
}

// Source chips: the streaming service when no input plays, then the HA source list.
void buildChips(const HaEntity& e) {
  const String key = e.mediaInputs + "|" + e.mediaSource + "|" + s_pc.st.service;
  if (key == s_pc.sourcesKey) return;
  s_pc.sourcesKey = key;
  s_pc.sources.clear();
  lv_obj_clean(s_pc.chips);
  for (int from = 1, end; (end = e.mediaInputs.indexOf('\n', from)) > from; from = end + 1)
    s_pc.sources.push_back(e.mediaInputs.substring(from, end));
  const uint32_t accent = tileAccent(*s_tiles[s_pc.tile].def);
  const bool input = haPlaysInput(e);
  auto chip = [&](const String& text, const char* glyph, bool lit, int index) {
    lv_obj_t* c = lv_obj_create(s_pc.chips);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_column(c, 8, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(lit ? accent : C_TRACK), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(c, lv_color_hex(lit ? C_TEXT_ON : C_TEXT), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* ic = lv_label_create(c);
    mediaGlyph(ic, glyph, "", 20);
    lv_obj_t* t = lv_label_create(c);
    lv_obj_set_style_text_font(t, &deck_font_14, 0);
    lv_label_set_text(t, text.c_str());
    if (index >= 0) lv_obj_add_event_cb(c, onSource, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)index);
    else lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
  };
  if (!input && (s_pc.st.service.length() || e.mediaSource.length()))
    chip(s_pc.st.service.length() ? s_pc.st.service : e.mediaSource, "music", true, -1);
  for (size_t i = 0; i < s_pc.sources.size(); i++)
    chip(s_pc.sources[i], sourceGlyph(s_pc.sources[i], true), input && s_pc.sources[i] == e.mediaSource, (int)i);
}

void setGlyph(lv_obj_t* l, const char* name, const char* fallback, int px, uint32_t color, bool on) {
  mediaGlyph(l, name, fallback, px);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_opa(l, on ? LV_OPA_COVER : LV_OPA_60, 0);
}

// Everything that follows the HA entity and the BluOS status.
void refresh(bool scroll) {
  TileRt* r = cardTile();
  if (!r || !s_modalCard) return;
  HaEntity e;
  const bool known = haGetEntity(r->def->entity, e) && e.known;
  const bool playing = known && e.state == "playing";
  setText(s_pc.title, known && e.mediaTitle.length() ? e.mediaTitle.c_str() : "Nothing playing");
  String who = known ? e.mediaArtist : String();
  if (s_pc.bluos && s_pc.st.album.length()) who += (who.length() ? " \xC2\xB7 " : "") + s_pc.st.album;
  setText(s_pc.artist, who.c_str());
  // Progress: the BluOS player's own clock when it answers, else HA's.
  int32_t dur = known ? e.mediaDuration : 0, pos = known ? e.mediaPosition : 0;
  deck::BluosStatus bs;
  if (s_pc.bluos && deck::bluosStatus(bs) && bs.totlen > 0) {
    dur = bs.totlen;
    pos = bs.secs + (playing ? (int32_t)((millis() - bs.secsAt) / 1000) : 0);
  } else {
    const time_t now = time(nullptr);
    if (playing && e.mediaPosAt && now >= 1609459200) pos = (int32_t)(now - e.mediaPosAt) + e.mediaPosition;
  }
  pos = constrain(pos, 0, max(dur, (int32_t)0));
  eqAnimate(playing);
  lv_bar_set_value(s_pc.bar, dur > 0 ? (int)((int64_t)pos * 1000 / dur) : 0, LV_ANIM_OFF);
  setText(s_pc.tPos, dur > 0 ? clock(pos).c_str() : "");
  setText(s_pc.tDur, dur > 0 ? clock(dur).c_str() : "");
  String q;
  if (s_pc.bluos && s_pc.st.ok) q = s_pc.st.service + (s_pc.st.service.length() && s_pc.st.format.length() ? " \xC2\xB7 " : "") + s_pc.st.format;
  setText(s_pc.quality, q.c_str());

  const uint32_t accent = tileAccent(*r->def);
  const bool queue = s_pc.bluos && s_pc.st.ok && s_pc.st.queue;
  const uint32_t bits = (playing ? 1 : 0) | (queue ? 2 : 0) | (s_pc.st.shuffle ? 4 : 0) | (s_pc.st.repeat << 3) |
                        (s_pc.st.love == 1 ? 32 : 0) | (s_pc.st.loveUrl.length() ? 64 : 0) |
                        (s_pc.st.banUrl.length() ? 128 : 0) | 256;
  if (bits != s_pc.glyphs) {
    s_pc.glyphs = bits;
    mediaGlyph(s_pc.ico[B_PLAY], playing ? "pause" : "play", playing ? "||" : ">", 32);
    showObj(s_pc.btn[B_SHUFFLE], queue);
    showObj(s_pc.btn[B_REPEAT], queue);
    if (queue) {
      setGlyph(s_pc.ico[B_SHUFFLE], s_pc.st.shuffle ? "shuffle-variant" : "shuffle-disabled", "S", 26,
               s_pc.st.shuffle ? accent : C_TEXT, s_pc.st.shuffle);
      const char* rep = s_pc.st.repeat == 0 ? "repeat" : s_pc.st.repeat == 1 ? "repeat-once" : "repeat-off";
      setGlyph(s_pc.ico[B_REPEAT], rep, "R", 26, s_pc.st.repeat == 2 ? C_TEXT : accent, s_pc.st.repeat != 2);
    }
    showObj(s_pc.love, s_pc.st.loveUrl.length() > 0);
    showObj(s_pc.ban, s_pc.st.banUrl.length() > 0);
    setGlyph(s_pc.loveIco, s_pc.st.love == 1 ? "heart" : "heart-outline", "<3", 26, 0xFF7A9A, true);
  }
  if (known) buildChips(e);
  if (s_pc.bluos) {
    char head[96];
    if (queue && s_pc.q.length > 0)
      snprintf(head, sizeof(head), "%s \xC2\xB7 %d of %d", s_pc.q.name.length() ? s_pc.q.name.c_str() : "Queue",
               s_pc.st.song + 1, s_pc.q.length);
    else snprintf(head, sizeof(head), "%s", queue ? "Loading the queue..." : "No queue for this source");
    setText(s_pc.sub, head);
  }
  markPageStale(r->page);
}

// The BluOS side: the queue window follows the status.
void syncBluos(bool force) {
  if (!s_pc.bluos) return;
  const uint32_t v = deck::bluosVersion();
  if (v == s_pc.seen && !force) return;
  s_pc.seen = v;
  deck::bluosStatus(s_pc.st);
  deck::BluosQueue q;
  deck::bluosQueue(q);
  const bool queue = s_pc.st.ok && s_pc.st.queue;
  showObj(s_pc.list, queue);
  if (queue) {
    const bool stale = q.tracks.empty() || q.pid != s_pc.st.pid;
    const int song = s_pc.st.song;
    const int want = s_pc.sel >= 0 ? s_pc.sel : song;
    if (stale || (s_pc.sel < 0 && (want < q.start || want >= q.start + (int)q.tracks.size())))
      askWindow(want - 2);
    if (!stale) {
      s_pc.q = q;
      if (q.start != s_pc.rowsFor || q.pid != s_pc.rowsPid || (int)q.tracks.size() != s_pc.rowsLen) buildRows();
    }
    markRows(true);
  }
  refresh(false);
}

// The card's own cover, decoded at its size.
void coverTick() {
  TileRt* r = cardTile();
  if (!r || !s_pc.cover) return;
  HaEntity e;
  String picture;
  if (haGetEntity(r->def->entity, e) && e.known)
    picture = e.mediaPicture.length() ? e.mediaPicture : e.mediaPictureRemote;
  const String id = picture.length() ? pictureId(picture) : String();
  if (id != s_pc.artWanted) {
    s_pc.artWanted = id;
    s_pc.artAsked = 0;
    // The tile's smaller cover stands in until the big one arrives.
    s_pc.art = r->art && r->art->id == id ? r->art : nullptr;
    lv_image_set_src(s_pc.cover, s_pc.art ? &s_pc.art->dsc : nullptr);
    showObj(s_pc.cover, (bool)s_pc.art);
  }
  if (!id.length() || (s_pc.art && s_pc.art->px == s_pc.coverPx)) return;
  if (s_pc.artAsked && millis() - s_pc.artAsked < 10000) return;
  const String url = picture.startsWith("/") ? haBaseUrl() + picture : picture;
  if (!url.startsWith("http://")) return;
  if (httpArtFetchJpeg(url, id.c_str(), s_pc.coverPx)) s_pc.artAsked = millis() | 1;
}

void build(TileRt& r) {
  const int w = logicalW(), h = logicalH();
  const int cw = min(w - 32, 960), ch = min(h - 32, 568);
  constexpr int PAD = 22;
  s_pc.wide = cw >= 640;
  s_pc.bluos = r.def->bluos.length() > 0;
  const uint32_t accent = tileAccent(*r.def);
  s_modalScrim = makeScrim();
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // a tap on the card does not reach the scrim
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS && (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);

  // Left column (or the top on a narrow screen): cover, title, progress, transport.
  constexpr int INFO = 174;  // title, progress and transport under the cover
  const int leftW = s_pc.wide ? min(340, cw * 38 / 100) : cw - 2 * PAD;
  s_pc.coverPx = s_pc.wide ? max(0, min(leftW, ch - 2 * PAD - INFO)) : 0;
  s_pc.coverPx = min(s_pc.coverPx, BOARD_MEDIA_ART_MAX * 2);
  int y = PAD;
  if (s_pc.coverPx >= 120) {
    s_pc.coverBox = lv_obj_create(c);
    lv_obj_remove_style_all(s_pc.coverBox);
    lv_obj_remove_flag(s_pc.coverBox, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(s_pc.coverBox, s_pc.coverPx, s_pc.coverPx);
    lv_obj_set_pos(s_pc.coverBox, PAD + (leftW - s_pc.coverPx) / 2, y);
    lv_obj_set_style_radius(s_pc.coverBox, 16, 0);
    lv_obj_set_style_clip_corner(s_pc.coverBox, true, 0);
    lv_obj_set_style_bg_color(s_pc.coverBox, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(s_pc.coverBox, LV_OPA_COVER, 0);
    lv_obj_t* note = lv_label_create(s_pc.coverBox);
    mediaGlyph(note, "music", "M", 48);
    lv_obj_center(note);
    s_pc.cover = lv_image_create(s_pc.coverBox);
    lv_obj_set_size(s_pc.cover, s_pc.coverPx, s_pc.coverPx);
    lv_image_set_inner_align(s_pc.cover, LV_IMAGE_ALIGN_STRETCH);  // the tile's smaller cover fills it too
    showObj(s_pc.cover, false);
    y += s_pc.coverPx + 12;
  } else {
    s_pc.coverPx = 0;
  }
  const int heartW = 44;
  s_pc.title = label(c, &deck_font_20);
  lv_obj_set_pos(s_pc.title, PAD, y);
  lv_obj_set_size(s_pc.title, leftW - 2 * heartW - 8, lineH(&deck_font_20));
  s_pc.artist = label(c, &deck_font_14, LV_OPA_70);
  lv_obj_set_pos(s_pc.artist, PAD, y + 28);
  lv_obj_set_size(s_pc.artist, leftW - 2 * heartW - 8, lineH(&deck_font_14));
  s_pc.love = iconButton(c, heartW, onLove, 0, &s_pc.loveIco);
  lv_obj_set_pos(s_pc.love, PAD + leftW - 2 * heartW, y);
  s_pc.ban = iconButton(c, heartW, onLove, 1, &s_pc.banIco);
  lv_obj_set_pos(s_pc.ban, PAD + leftW - heartW, y);
  setGlyph(s_pc.banIco, "thumb-down-outline", "X", 24, C_TEXT, false);
  y += 58;
  s_pc.bar = lv_bar_create(c);
  lv_obj_remove_style_all(s_pc.bar);
  lv_bar_set_range(s_pc.bar, 0, 1000);
  lv_obj_set_size(s_pc.bar, leftW, 5);
  lv_obj_set_pos(s_pc.bar, PAD, y);
  lv_obj_set_style_radius(s_pc.bar, 3, 0);
  lv_obj_set_style_radius(s_pc.bar, 3, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_pc.bar, lv_color_hex(C_TRACK), 0);
  lv_obj_set_style_bg_opa(s_pc.bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_pc.bar, lv_color_hex(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_pc.bar, LV_OPA_COVER, LV_PART_INDICATOR);
  y += 11;
  s_pc.tPos = label(c, &deck_font_12, LV_OPA_70);
  lv_obj_set_pos(s_pc.tPos, PAD, y);
  s_pc.tDur = label(c, &deck_font_12, LV_OPA_70);
  lv_obj_set_width(s_pc.tDur, 60);
  lv_obj_set_style_text_align(s_pc.tDur, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(s_pc.tDur, PAD + leftW - 60, y);
  s_pc.quality = label(c, &deck_font_12, LV_OPA_70);
  lv_obj_set_width(s_pc.quality, leftW - 130);
  lv_obj_set_style_text_align(s_pc.quality, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_pc.quality, PAD + 65, y);
  y += 26;
  static const char* const names[] = {"shuffle-disabled", "skip-previous", "play", "skip-next", "repeat-off"};
  static const char* const fb[] = {"S", "|<", ">", ">|", "R"};
  for (int i = 0; i < B_COUNT; i++) {
    const int size = i == B_PLAY ? 64 : 52;
    s_pc.btn[i] = iconButton(c, size, onTransport, i, &s_pc.ico[i]);
    lv_obj_set_pos(s_pc.btn[i], PAD + i * (leftW - 52) / (B_COUNT - 1) - (size - 52) / 2, y + (64 - size) / 2);
    mediaGlyph(s_pc.ico[i], names[i], fb[i], i == B_PLAY ? 32 : 28);
  }
  lv_obj_set_style_bg_color(s_pc.btn[B_PLAY], lv_color_hex(accent), 0);
  lv_obj_set_style_bg_opa(s_pc.btn[B_PLAY], LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(s_pc.ico[B_PLAY], lv_color_hex(C_TEXT_ON), 0);
  y += 64 + 14;

  // Right column (or below on a narrow screen): sources and the queue.
  const int rx = s_pc.wide ? PAD + leftW + 28 : PAD;
  const int ry = s_pc.wide ? PAD : y;
  const int rw = cw - rx - PAD;
  lv_obj_t* closeIcon;
  lv_obj_t* close = iconButton(c, 44, [](lv_event_t*) { closeModal(); }, 0, &closeIcon);
  mediaGlyph(closeIcon, "close", "X", 22);
  lv_obj_set_style_bg_color(close, lv_color_hex(C_TRACK), 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_COVER, 0);
  lv_obj_set_pos(close, cw - PAD - 44, PAD);
  int yy = ry;
  if (s_pc.wide) {
    s_pc.head = label(c, &deck_font_20);
    lv_obj_set_pos(s_pc.head, rx, yy);
    lv_obj_set_width(s_pc.head, rw - 54);
    lv_label_set_text(s_pc.head, s_pc.bluos ? "Up next" : "Source");
    s_pc.sub = label(c, &deck_font_12, LV_OPA_70);
    lv_obj_set_pos(s_pc.sub, rx, yy + 26);
    lv_obj_set_width(s_pc.sub, rw - 54);
    yy += 58;
  }
  s_pc.chips = lv_obj_create(c);
  lv_obj_remove_style_all(s_pc.chips);
  lv_obj_set_pos(s_pc.chips, rx, yy);
  lv_obj_set_size(s_pc.chips, rw, 44);
  lv_obj_set_flex_flow(s_pc.chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(s_pc.chips, 8, 0);
  lv_obj_set_scroll_dir(s_pc.chips, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(s_pc.chips, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(s_pc.chips, onListScroll, LV_EVENT_SCROLL, nullptr);
  yy += 44 + 12;
  if (s_pc.bluos && ch - PAD - yy >= 54) {
    s_pc.list = lv_obj_create(c);
    lv_obj_remove_style_all(s_pc.list);
    lv_obj_set_pos(s_pc.list, rx, yy);
    lv_obj_set_size(s_pc.list, rw, ch - PAD - yy);
    lv_obj_set_flex_flow(s_pc.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pc.list, 4, 0);
    lv_obj_set_scroll_dir(s_pc.list, LV_DIR_VER);
    lv_obj_add_event_cb(s_pc.list, onListScroll, LV_EVENT_SCROLL, nullptr);
    showObj(s_pc.list, false);
  } else {
    s_pc.bluos = false;
  }
}

}  // namespace

bool playerCardOpen() { return s_pc.tile >= 0 && s_modalCard; }

const String& playerCardEntity() {
  static const String none;
  TileRt* r = cardTile();
  return r ? r->def->entity : none;
}

void openPlayerCard(TileRt& r, bool byKnob) {
  const int idx = (int)(&r - s_tiles.data());
  if (playerCardOpen() && s_pc.tile == idx) {
    s_pc.byKnob = s_pc.byKnob && byKnob;
    touched();
    return;
  }
  closeModal();
  cleanPage();
  s_pc = PlayerCard();
  s_pc.tile = idx;
  s_pc.byKnob = byKnob;
  touched();
  build(r);
  if (s_pc.bluos) {
    deck::bluosFollow(r.def->bluos, true);
    syncBluos(true);
  }
  refresh(true);
  coverTick();
  setModalOpen(true);
}

void playerCardClose() {
  if (s_pc.tile < 0) return;
  s_pc = PlayerCard();  // the widgets go with the card
}

// Knob 0 in the card: turn moves the pointer through the queue, press plays it
// (or play/pause on the playing track).
void playerCardKnob(int detents, bool click) {
  touched();
  if (!s_pc.bluos || !s_pc.st.ok || !s_pc.st.queue) {
    if (click) haCall("media_play_pause");
    else if (detents) haCall(detents > 0 ? "media_next_track" : "media_previous_track");
    return;
  }
  if (click) {
    hapticTap();
    audioClick();
    if (s_pc.sel >= 0 && s_pc.sel != s_pc.st.song) playIndex(s_pc.sel);
    else haCall("media_play_pause");
    s_pc.sel = -1;
    markRows(true);
    return;
  }
  const int len = max(s_pc.q.length, 1);
  const int from = s_pc.sel >= 0 ? s_pc.sel : max(0, s_pc.st.song);
  s_pc.sel = constrain(from + detents, 0, len - 1);
  if (!rowFor(s_pc.sel)) askWindow(detents > 0 ? s_pc.sel - 3 : s_pc.sel - QUEUE_WINDOW + 4);
  markRows(true);
}

// The loop: who the BluOS client follows, card refresh, knob timeout.
void playerTick() {
  static uint32_t lastSecond = 0;
  const uint32_t now = millis();
  TileRt* card = playerCardOpen() ? cardTile() : nullptr;
  if (!card && s_pc.tile >= 0) s_pc = PlayerCard();  // another card replaced it
  // Follow the open card's player, else the first BluOS tile (fast while its page shows).
  String host;
  bool fast = false;
  if (card && card->def->bluos.length()) {
    host = card->def->bluos;
    fast = true;
  } else {
    for (const TileRt& r : s_tiles) {
      if (r.def->type != TILE_MEDIA || !r.def->bluos.length() || !r.mediaHa) continue;
      if (!host.length()) host = r.def->bluos;
      if (r.def->bluos == host && r.page == sh.active) fast = true;
    }
  }
  deck::bluosFollow(host, fast);
  static uint32_t tilesSeen = 0;
  const uint32_t v = deck::bluosVersion();
  if (v != tilesSeen) {
    tilesSeen = v;
    for (TileRt& r : s_tiles)
      if (r.def->type == TILE_MEDIA && r.def->bluos.length()) applyMedia(r);
  }
  if (!card) return;
  syncBluos(false);
  if (now - lastSecond >= 1000) {
    lastSecond = now;
    refresh(false);
    coverTick();
  }
  if (s_pc.byKnob && now - s_pc.lastInput >= KNOB_CLOSE_MS) closeModal();
}

// A decoded cover for the card (mediaArtTick hands every result here first).
void playerArtTake(const std::shared_ptr<MediaArt>& art) {
  if (!playerCardOpen() || !art || !s_pc.cover || art->px != s_pc.coverPx || art->id != s_pc.artWanted) return;
  s_pc.art = art;
  lv_image_set_src(s_pc.cover, &art->dsc);
  showObj(s_pc.cover, true);
  if (TileRt* r = cardTile()) markPageStale(r->page);
}

// Applied when the tile's entity changes in HA.
void playerCardHaChanged(const TileRt& r) {
  if (playerCardOpen() && &r == cardTile()) refresh(false);
}

}  // namespace deckui
