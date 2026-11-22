// Deck UI: tile look and building a tile.
#include "deck_ui_int.h"

namespace deckui {

// --- Tile look ------------------------------------------------------------------------
String entityDomain(const String& entity) {
  const int dot = entity.indexOf('.');
  return dot > 0 ? entity.substring(0, dot) : String();
}

uint32_t domainColor(const String& domain) {
  if (domain == "light") return 0xF6C56B;
  if (domain == "switch" || domain == "fan" || domain == "input_boolean") return 0x5CC9A7;
  if (domain == "scene" || domain == "script") return 0xB3A6FF;
  if (domain == "lock" || domain == "cover") return 0x9DB8FF;
  return 0x7DB5FF;
}

// Material Design Icons names (what HA uses) mapped onto the LVGL symbol font
// until the MDI subset font lands with the Home Assistant work.
const char* iconSymbol(const TileDef& t) {
  struct Map { const char* name; const char* sym; };
  static const Map kMap[] = {
    {"lightbulb", LV_SYMBOL_EYE_OPEN}, {"ceiling-light", LV_SYMBOL_EYE_OPEN},
    {"television-ambient-light", LV_SYMBOL_IMAGE}, {"fan", LV_SYMBOL_REFRESH},
    {"weather-night", LV_SYMBOL_IMAGE}, {"eye", LV_SYMBOL_EYE_OPEN},
    {"printer-3d", LV_SYMBOL_CHARGE}, {"speaker", LV_SYMBOL_AUDIO},
    {"lock", LV_SYMBOL_HOME}, {"power", LV_SYMBOL_POWER}, {"power-sleep", LV_SYMBOL_POWER},
    {"chevron-right", LV_SYMBOL_RIGHT}, {"chevron-left", LV_SYMBOL_LEFT},
    {"microphone-off", LV_SYMBOL_MUTE}, {"image", LV_SYMBOL_IMAGE},
    {"skip-previous", LV_SYMBOL_PREV}, {"skip-next", LV_SYMBOL_NEXT},
    {"play", LV_SYMBOL_PLAY}, {"pause", LV_SYMBOL_PAUSE},
    {"console", LV_SYMBOL_KEYBOARD}, {"volume-high", LV_SYMBOL_VOLUME_MAX},
    {"volume-off", LV_SYMBOL_MUTE}, {"wifi", LV_SYMBOL_WIFI}, {"cog", LV_SYMBOL_SETTINGS},
    {"bell", LV_SYMBOL_BELL}, {"home", LV_SYMBOL_HOME},
  };
  String name = t.icon;
  if (name.startsWith("mdi:")) name = name.substring(4);
  for (const Map& m : kMap)
    if (name == m.name) return m.sym;
  const String d = entityDomain(t.entity);
  if (d == "light") return LV_SYMBOL_EYE_OPEN;
  if (d == "fan") return LV_SYMBOL_REFRESH;
  if (d == "lock") return LV_SYMBOL_HOME;
  if (d == "media_player") return LV_SYMBOL_PLAY;
  if (d == "sensor" || d == "binary_sensor" || d == "weather") return LV_SYMBOL_LIST;
  return LV_SYMBOL_POWER;
}

// Default MDI icon for an entity without one of its own.
const char* defaultIcon(const String& domain, const String& dc, const String& unit, bool on) {
  if (domain == "light") return on ? "lightbulb" : "lightbulb-outline";
  if (domain == "switch" && dc == "outlet") return on ? "power-plug" : "power-plug-off";
  if (domain == "switch" || domain == "input_boolean")
    return on ? "toggle-switch-variant" : "toggle-switch-variant-off";
  if (domain == "fan") return on ? "fan" : "fan-off";
  if (domain == "lock") return on ? "lock" : "lock-open-variant";
  if (domain == "cover") return on ? "window-shutter-open" : "window-shutter";
  if (domain == "scene") return "palette";
  if (domain == "script") return "script-text";
  if (domain == "automation") return "robot";
  if (domain == "button" || domain == "input_button") return "gesture-tap-button";
  if (domain == "media_player") return "speaker";
  if (domain == "climate") return "thermostat";
  if (domain == "weather") return "weather-partly-cloudy";
  if (domain == "person") return "account";
  if (domain == "binary_sensor") {
    if (dc == "door" || dc == "garage_door") return on ? "door-open" : "door-closed";
    if (dc == "window") return on ? "window-open" : "window-closed";
    if (dc == "motion" || dc == "occupancy" || dc == "presence") return "motion-sensor";
    return on ? "checkbox-marked-circle" : "checkbox-blank-circle-outline";
  }
  if (domain == "sensor") {
    if (dc == "temperature" || unit.startsWith("\xC2\xB0")) return "thermometer";
    if (dc == "humidity") return "water-percent";
    if (dc == "power") return "flash";
    if (dc == "energy") return "lightning-bolt";
    if (dc == "battery") return "battery";
    if (dc == "illuminance") return "brightness-5";
    if (dc == "pressure" || dc == "atmospheric_pressure") return "gauge";
    if (dc == "carbon_dioxide") return "molecule-co2";
    if (dc == "voltage") return "sine-wave";
    if (dc == "current") return "current-ac";
    return "eye";
  }
  return "gesture-tap-button";
}

// Puts a tile's icon on a label: the tile's own MDI icon, else the entity's
// icon in HA, else a default; an LVGL symbol while no icon font is loaded.
// Returns the codepoint shown (0 = symbol); shown = what the label has now.
uint32_t showIcon(lv_obj_t* label, const TileDef& d, const HaEntity* e, bool on, int px,
                  uint32_t shown) {
  uint32_t cp = 0;
  if (on && d.onIcon.length()) cp = iconCodepoint(d.onIcon);
  if (!cp && d.icon.length()) cp = iconCodepoint(d.icon);
  if (!cp && e && e->icon.length()) cp = iconCodepoint(e->icon);
  if (!cp)
    cp = iconCodepoint(defaultIcon(entityDomain(d.entity), e ? e->deviceClass : String(),
                                   e ? e->unit : String(), on));
  const lv_font_t* f = cp ? iconFont(px, cp) : nullptr;
  if (!f) cp = 0;
  if (cp && cp == shown) return cp;
  if (cp) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(label, f, 0);
    lv_label_set_text(label, u);
  } else {
    lv_label_set_text(label, iconSymbol(d));
  }
  return cp;
}

String tileLabel(const TileDef& t) {
  if (t.label.length()) return t.label;
  if (t.type == TILE_CLOCK) return "Clock";
  if (t.type == TILE_MEDIA) return "Now playing";
  if (t.type == TILE_VIZ) return "Audio";
  if (t.type == TILE_WEATHER) return "Weather";
  if (t.type == TILE_AGENT) return "AI Agent";
  const int dot = t.entity.indexOf('.');
  if (dot > 0) return t.entity.substring(dot + 1);
  return t.id.length() ? t.id : String("Tile");
}

String tileSub(const TileRt& r) {
  const TileDef& t = *r.def;
  if (t.sub.length()) return t.sub;
  if (t.type == TILE_TOGGLE) return r.on ? "On" : "Off";
  if (t.type == TILE_AGENT) return agentTileSub();
  const String d = entityDomain(t.entity);
  if (d == "scene") return "Scene";
  if (d == "script") return "Script";
  if (d == "button" || d == "input_button") return "Press";
  return "";
}

void markPageStale(int page) {
  if (page >= 0 && page < s_count) s_pages[page].stale = true;
}

uint32_t tileAccent(const TileDef& def) {
  return def.hasColor ? def.color : def.type == TILE_PRINTER ? C_PRN_HEAT : domainColor(entityDomain(def.entity));
}

// Per-tile look from deck.json, else the theme and display settings.
uint32_t tileBg(const TileDef& d) { return d.hasBgColor ? d.bgColor : C_TILE; }
uint32_t tileOnFill(const TileDef& d) { return d.hasOnColor ? d.onColor : tileAccent(d); }
int tileTint(const TileDef& d) { return d.opacity >= 0 ? d.opacity : dispSettings.glassTint; }
int tileOnOpa(const TileDef& d) { return d.onOpacity >= 0 ? d.onOpacity : dispSettings.glassOn; }

// The icon's colour: a lit light's own colour ("lightIcon"; dimmer at low
// brightness), else "onIconColor" / "iconColor", else the accent; a plain icon
// on an on tile takes the text colour, as the accent would vanish on the on fill.
// e: the tile's entity if the caller has it, else looked up when needed.
void tileIconColor(TileRt& r, const HaEntity* e) {
  const TileDef& d = *r.def;
  if (!r.chip || (d.type != TILE_ACTION && d.type != TILE_TOGGLE && d.type != TILE_SLIDER && d.type != TILE_SENSOR &&
                  d.type != TILE_COVER && d.type != TILE_AGENT))
    return;
  const bool on = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  HaEntity own;
  if (d.lightIcon && !e && haGetEntity(d.entity, own)) e = &own;
  const bool lit = d.lightIcon && e && e->state == "on";
  const lv_opa_t opa = lit && e->brightness >= 0 ? (lv_opa_t)(102 + e->brightness * 153 / 255) : LV_OPA_COVER;  // 40..100 %
  if (lit && e->rgb >= 0) {
    lv_obj_set_style_text_color(r.chip, lv_color_hex(e->rgb), 0);
  } else if (on && d.hasOnIconColor) {
    lv_obj_set_style_text_color(r.chip, lv_color_hex(d.onIconColor), 0);
  } else if (on && d.iconPlain) {
    lv_obj_remove_local_style_prop(r.chip, LV_STYLE_TEXT_COLOR, 0);
  } else if (d.hasIconColor) {
    lv_obj_set_style_text_color(r.chip, lv_color_hex(d.iconColor), 0);
  } else {
    styleIconChip(r.chip, tileAccent(d));
  }
  if (lv_obj_get_style_text_opa(r.icon, LV_PART_MAIN) != opa) lv_obj_set_style_text_opa(r.icon, opa, 0);
}

// Glass tiles are see-through: their glass is baked into the page
// background (bakeGlass); pressed and on states draw over it.
void glassStyle(lv_obj_t* t, const TileDef& def) {
  const int onPct = tileOnOpa(def);
  const lv_opa_t on = (lv_opa_t)(onPct * 255 / 100);
  lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(t, 0, 0);
  // A faint fill alone hardly tells on from off: outline it in the accent.
  if (onPct < 50) {
    lv_obj_set_style_border_width(t, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(t, lv_color_hex(tileOnFill(def)), LV_STATE_CHECKED);
  }
}

// --- Tile frames ----------------------------------------------------------------------
std::vector<FrameRt> s_frames;

uint32_t frameBg(const FrameDef& f) { return f.hasBgColor ? f.bgColor : C_TILE; }
int frameTint(const FrameDef& f) { return f.opacity >= 0 ? f.opacity : dispSettings.glassTint; }

const FrameRt* frameOf(lv_obj_t* o) {
  for (const FrameRt& f : s_frames)
    if (f.obj == o) return &f;
  return nullptr;
}

// Not clickable: a touch on the gaps between its tiles reaches the grid (edit mode hold).
void makeFrames(Page& p) {
  const bool glass = s_theme == THEME_GLASS && wallBlur();
  for (const FrameDef& f : p.def->frames) {
    lv_obj_t* o = lv_obj_create(p.grid);
    lv_obj_set_size(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_grid_cell(o, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_radius(o, 16, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    if (f.outline) {
      lv_obj_set_style_border_width(o, f.outline, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(f.hasOutColor ? f.outColor : C_TEXT), 0);
      lv_obj_set_style_border_opa(o, f.outOpa >= 0 ? (lv_opa_t)(f.outOpa * 255 / 100) : f.hasOutColor ? LV_OPA_COVER : 64, 0);
    } else {
      lv_obj_set_style_border_width(o, s_theme == THEME_DAYLIGHT ? 1 : 0, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(C_TILE_LINE), 0);
    }
    lv_obj_set_style_bg_color(o, lv_color_hex(frameBg(f)), 0);
    lv_obj_set_style_bg_opa(o, glass ? LV_OPA_TRANSP : LV_OPA_COVER, 0);  // Glass: baked like a tile
    s_frames.push_back({&f, o});
  }
}

static lv_color_t pressedBg(bool has, uint32_t bg) {
  return has ? lv_color_mix(lv_color_hex(bg), lv_color_black(), 200) : lv_color_hex(C_TILE_PRESSED);
}

// A tile in a shown frame has no background of its own; pressed (darker frame colour) and on still fill it.
void setFramed(TileRt& r, lv_obj_t* frame) {
  if (r.frameObj == frame) return;
  r.frameObj = frame;
  const FrameRt* f = frame ? frameOf(frame) : nullptr;
  lv_obj_set_style_bg_color(r.obj, f ? pressedBg(f->def->hasBgColor, f->def->bgColor)
                                     : pressedBg(r.def->hasBgColor, r.def->bgColor), LV_STATE_PRESSED);
  if (!(s_theme == THEME_GLASS && wallBlur())) {
    lv_obj_set_style_bg_opa(r.obj, frame ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_STATE_CHECKED);
  }
  if (s_theme == THEME_DAYLIGHT) lv_obj_set_style_border_width(r.obj, frame ? 0 : 1, 0);
}

static bool cellsTouch(const Cell& a, const Cell& b) {
  const bool rows = a.y < b.y + b.h && b.y < a.y + a.h, cols = a.x < b.x + b.w && b.x < a.x + a.w;
  return (rows && (a.x + a.w == b.x || b.x + b.w == a.x)) || (cols && (a.y + a.h == b.y || b.y + b.h == a.y));
}

// Divider rects (x, y, w, h in the frame's content coords) in the gaps where two
// of its tiles touch: "short" keeps a fifth of the edge clear at each end, "full"
// reaches the frame's edges and meets the crossing lines. No two rects overlap:
// a faint line would come out brighter where it is drawn twice.
static std::vector<int32_t> dividerRects(const FrameRt& f, const std::vector<lv_obj_t*>& objs,
                                         const std::vector<const Cell*>& cs, int32_t gapRow, int32_t gapCol) {
  struct Seg { bool v; int32_t at, s1, s2; };  // v: vertical; at: first px across; s1..s2 along
  std::vector<int32_t> out;
  if (f.def->dividers == FRAME_DIV_OFF) return out;
  const bool full = f.def->dividers == FRAME_DIV_FULL;
  const int32_t t = f.def->divWidth;
  auto line = [t](int32_t end, int32_t start) { return (end + start) / 2 - (t - 1) / 2; };  // in the gap end..start
  lv_area_t fa, fc;
  lv_obj_get_coords(f.obj, &fa);
  lv_obj_get_content_coords(f.obj, &fc);
  std::vector<Seg> segs;
  for (size_t i = 0; i < cs.size(); i++)
    for (size_t j = 0; j < cs.size(); j++) {
      const Cell &a = *cs[i], &b = *cs[j];
      const bool v = a.x + a.w == b.x && a.y < b.y + b.h && b.y < a.y + a.h;
      if (!v && !(a.y + a.h == b.y && a.x < b.x + b.w && b.x < a.x + a.w)) continue;
      if (f.def->divAxis == (v ? FRAME_AXIS_ROWS : FRAME_AXIS_COLS)) continue;
      lv_area_t ca, cb;
      lv_obj_get_coords(objs[i], &ca);
      lv_obj_get_coords(objs[j], &cb);
      Seg s = v ? Seg{true, line(ca.x2, cb.x1), max(ca.y1, cb.y1), min(ca.y2, cb.y2)}
                : Seg{false, line(ca.y2, cb.y1), max(ca.x1, cb.x1), min(ca.x2, cb.x2)};
      const int32_t lo = v ? fa.y1 : fa.x1, hi = v ? fa.y2 : fa.x2, gap = v ? gapRow : gapCol;
      if (full) {
        if (s.s1 > lo) s.s1 = line(s.s1 - gap - 1, s.s1);
        if (s.s2 < hi) s.s2 = line(s.s2, s.s2 + gap + 1) + t - 1;
        s.s1 = max(s.s1, v ? fc.y1 : fc.x1), s.s2 = min(s.s2, v ? fc.y2 : fc.x2);  // inside the outline
      } else {
        const int32_t in = (s.s2 - s.s1 + 1) / 5;
        s.s1 += in, s.s2 -= in;
      }
      if (s.s2 >= s.s1) segs.push_back(s);
    }
  // Join lines that run on in one row, then cut the horizontal ones where a vertical one crosses.
  std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
    return a.v != b.v ? a.v : a.at != b.at ? a.at < b.at : a.s1 < b.s1;
  });
  std::vector<Seg> m;
  for (const Seg& s : segs)
    if (!m.empty() && m.back().v == s.v && m.back().at == s.at && s.s1 <= m.back().s2 + 1) m.back().s2 = max(m.back().s2, s.s2);
    else m.push_back(s);
  for (const Seg& s : m) {
    if (s.v) {
      out.insert(out.end(), {s.at - fc.x1, s.s1 - fc.y1, t, s.s2 - s.s1 + 1});
      continue;
    }
    int32_t x = s.s1;
    for (const Seg& c : m) {  // verticals first in m, sorted by x
      if (!c.v || c.at + t - 1 < x || c.at > s.s2 || c.s2 < s.at || c.s1 > s.at + t - 1) continue;
      if (c.at > x) out.insert(out.end(), {x - fc.x1, s.at - fc.y1, c.at - x, t});
      x = c.at + t;
    }
    if (x <= s.s2) out.insert(out.end(), {x - fc.x1, s.at - fc.y1, s.s2 - x + 1, t});
  }
  return out;
}

static void drawDividers(FrameRt& f, std::vector<int32_t>&& rs) {
  lv_obj_clean(f.obj);
  const lv_color_t c = lv_color_hex(f.def->hasDivColor ? f.def->divColor : C_TEXT);
  const lv_opa_t opa = f.def->divOpa >= 0 ? (lv_opa_t)(f.def->divOpa * 255 / 100) : f.def->hasDivColor ? LV_OPA_COVER : 38;
  for (size_t i = 0; i + 3 < rs.size(); i += 4) {
    lv_obj_t* o = lv_obj_create(f.obj);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(o, rs[i], rs[i + 1]);
    lv_obj_set_size(o, rs[i + 2], rs[i + 3]);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
  }
  f.lines = std::move(rs);
}

// A frame shows around its placed tiles while there are two or more, they all
// connect edge to edge and no other placed tile is in their box. changed grows
// by the page area (page coords) where a frame came, went, moved or redrew its dividers.
bool placeFrames(Page& p, const std::vector<Cell>& cells, lv_area_t& changed) {
  const PageDef& def = *p.def;
  bool any = false;
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (FrameRt& f : s_frames) {
    if (lv_obj_get_parent(f.obj) != p.grid) continue;
    const String& id = f.def->id;
    std::vector<int> m;
    int x1 = INT_MAX, y1 = INT_MAX, x2 = 0, y2 = 0;
    for (size_t i = 0; i < def.tiles.size(); i++) {
      const Cell& c = cells[i];
      if (!c.placed || def.tiles[i].frame != id) continue;
      m.push_back(i);
      x1 = min(x1, c.x), y1 = min(y1, c.y), x2 = max(x2, c.x + c.w), y2 = max(y2, c.y + c.h);
    }
    bool ok = m.size() >= 2;
    for (size_t i = 0; ok && i < def.tiles.size(); i++) {
      const Cell& c = cells[i];
      if (c.placed && def.tiles[i].frame != id && c.x < x2 && c.x + c.w > x1 && c.y < y2 && c.y + c.h > y1) ok = false;
    }
    if (ok) {  // connected: grow from the first tile through shared edges
      std::vector<bool> in(m.size(), false);
      in[0] = true;
      size_t n = 1;
      for (bool grew = true; grew;) {
        grew = false;
        for (size_t a = 0; a < m.size(); a++)
          for (size_t b = 0; !in[a] && b < m.size(); b++)
            if (in[b] && cellsTouch(cells[m[a]], cells[m[b]])) in[a] = true, n++, grew = true;
      }
      ok = n == m.size();
    }
    const bool was = !lv_obj_has_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
    lv_area_t a0;
    lv_obj_get_coords(f.obj, &a0);
    if (ok) lv_obj_set_grid_cell(f.obj, LV_GRID_ALIGN_STRETCH, x1, x2 - x1, LV_GRID_ALIGN_STRETCH, y1, y2 - y1);
    showObj(f.obj, ok);
    for (TileRt& r : s_tiles)
      if (r.obj && lv_obj_get_parent(r.obj) == p.grid && r.def->frame == id) setFramed(r, ok ? f.obj : nullptr);
    lv_obj_update_layout(p.root);
    bool redrawn = false;
    if (ok) {
      std::vector<lv_obj_t*> objs;
      std::vector<const Cell*> cs;
      for (int i : m)
        for (const TileRt& r : s_tiles)
          if (r.def == &def.tiles[i] && r.obj) {
            objs.push_back(r.obj), cs.push_back(&cells[i]);
            break;
          }
      std::vector<int32_t> rs = dividerRects(f, objs, cs, lv_obj_get_style_pad_row(p.grid, LV_PART_MAIN),
                                             lv_obj_get_style_pad_column(p.grid, LV_PART_MAIN));
      if (rs != f.lines) {
        drawDividers(f, std::move(rs));
        lv_obj_update_layout(p.root);
        redrawn = true;
      }
    } else if (!f.lines.empty()) {
      lv_obj_clean(f.obj);
      f.lines.clear();
    }
    lv_area_t a1;
    lv_obj_get_coords(f.obj, &a1);
    if (was == ok && !redrawn && (!ok || (a0.x1 == a1.x1 && a0.y1 == a1.y1 && a0.x2 == a1.x2 && a0.y2 == a1.y2))) continue;
    any = true;
    for (const lv_area_t* a : {was ? &a0 : nullptr, ok ? &a1 : nullptr}) {
      if (!a) continue;
      changed = {LV_MIN(changed.x1, a->x1 - rc.x1), LV_MIN(changed.y1, a->y1 - rc.y1),
                 LV_MAX(changed.x2, a->x2 - rc.x1), LV_MAX(changed.y2, a->y2 - rc.y1)};
    }
  }
  return any;
}

// Text on an "on" tile: its own colour, else dark while the fill is bright
// enough, else the normal light text (Glass with a faint fill over a dark picture).
uint32_t onTextColor(const TileDef& def, bool glass) {
  if (def.hasOnTextColor) return def.onTextColor;
  if (!glass) return C_TEXT_ON;
  const uint32_t fill = tileOnFill(def);
  const uint32_t lum = ((fill >> 16 & 0xFF) * 299 + (fill >> 8 & 0xFF) * 587 + (fill & 0xFF) * 114) / 1000;
  return lum * tileOnOpa(def) / 100 >= 115 ? C_TEXT_ON : C_TEXT;
}

void makeTile(lv_obj_t* grid, const TileDef& def, int page, int col, int row, int w, int h) {
  const String domain = entityDomain(def.entity);
  const uint32_t accent = tileAccent(def);
  const bool glass = s_theme == THEME_GLASS && wallBlur();

  lv_obj_t* t = lv_obj_create(grid);
  lv_obj_set_size(t, 0, 0);  // the grid stretches it; LVGL's default 130 px would be fitted first
  lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, w, LV_GRID_ALIGN_STRETCH, row, h);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(t, 16, 0);
  // Daylight tiles get a hairline, white on light grey is too soft alone.
  lv_obj_set_style_border_width(t, s_theme == THEME_DAYLIGHT ? 1 : 0, 0);
  lv_obj_set_style_border_color(t, lv_color_hex(C_TILE_LINE), 0);
  lv_obj_set_style_pad_all(t, 9, 0);
  const uint32_t onFill = tileOnFill(def);
  lv_obj_set_style_bg_color(t, lv_color_hex(tileBg(def)), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(onFill), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(t, lv_color_hex(tileText(def)), 0);
  lv_obj_set_style_text_color(t, lv_color_hex(onTextColor(def, glass)), LV_STATE_CHECKED);
  // Press feedback: the card darkens and shrinks a little, eased in 90 ms.
  lv_obj_set_style_bg_color(t, pressedBg(def.hasBgColor, def.bgColor), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(onFill), lv_color_black(), 200),
                            LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_transform_width(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transition(t, &s_pressTrans, 0);
  lv_obj_set_style_transition(t, &s_pressTrans, LV_STATE_PRESSED);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_PRESS_LOST, nullptr);
  if (glass) glassStyle(t, def);

  if (def.type == TILE_PRINTER) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makePrinterContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyPrinterToTile(s_tiles.back());
    // The hero is clickable only while it has errors to list (applyPrinterToTile).
    if (def.show == PS_LIGHT || def.show == PS_SUMMARY || def.show == PS_HERO)
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
    if (def.show != PS_LIGHT && def.show != PS_SUMMARY && def.show != PS_HERO)
      lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_METRIC || (def.type == TILE_SENSOR && def.style == METRIC_CHART)) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeMetricContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    if (r.spark) {
      lv_obj_add_event_cb(r.viz, sparkDrawEvent, LV_EVENT_DRAW_MAIN, r.spark);
      lv_obj_add_event_cb(r.viz, sparkSizeEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    }
    if (def.type == TILE_METRIC) applyPcToTile(s_tiles.back());
    else {
      applyHaToTile(s_tiles.back(), true);
      applySensorHistory(s_tiles.back());
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
    }
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_MEDIA) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    r.chip = lv_obj_create(t);
    lv_obj_remove_style_all(r.chip);
    lv_obj_remove_flag(r.chip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(r.chip, 8, 0);
    lv_obj_set_style_clip_corner(r.chip, true, 0);
    lv_obj_set_style_bg_color(r.chip, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(r.chip, LV_OPA_COVER, 0);
    r.cover = lv_image_create(r.chip);
    lv_obj_remove_flag(r.cover, LV_OBJ_FLAG_CLICKABLE);
    showObj(r.cover, false);
    r.icon = lv_label_create(r.chip);
    mediaGlyph(r.icon, "music", "M", 32);
    lv_obj_center(r.icon);
    r.name = lv_label_create(t);
    r.sub = lv_label_create(t);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_opa(r.sub, LV_OPA_70, 0);
    r.viz = lv_bar_create(t);
    lv_obj_remove_style_all(r.viz);
    lv_obj_remove_flag(r.viz, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(r.viz, 0, 1000);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r.viz, 2, 0);
    lv_obj_set_style_radius(r.viz, 2, LV_PART_INDICATOR);
    const size_t idx = s_tiles.size();
    static const char* const icons[] = {"skip-previous", "play", "skip-next"};
    static const char* const fallback[] = {"|<", ">", ">|"};
    for (int i = 0; i < 3; i++) {
      lv_obj_t* button = r.mediaButtons[i] = lv_obj_create(t);
      lv_obj_remove_style_all(button);
      lv_obj_remove_flag(button, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
      lv_obj_set_style_radius(button, 8, 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_PRESSED);
      if (i == 1) {
        r.mediaDisc = lv_obj_create(button);
        lv_obj_remove_style_all(r.mediaDisc);
        lv_obj_remove_flag(r.mediaDisc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_radius(r.mediaDisc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r.mediaDisc, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(r.mediaDisc, LV_OPA_COVER, 0);
        showObj(r.mediaDisc, false);
      }
      r.mediaIcons[i] = lv_label_create(button);
      mediaGlyph(r.mediaIcons[i], icons[i], fallback[i], 24);
      lv_obj_center(r.mediaIcons[i]);
      lv_obj_add_event_cb(button, mediaEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(idx * 8 + i));
#if BOARD_HAS_ENCODER
      lv_obj_add_event_cb(button, mediaEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)(idx * 8 + i));
#endif
    }
    r.volBar = lv_bar_create(t);
    lv_obj_remove_style_all(r.volBar);
    lv_obj_add_flag(r.volBar, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED,
                              LV_EVENT_LONG_PRESSED_REPEAT, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
      lv_obj_add_event_cb(r.volBar, mediaVolEvent, c, (void*)(uintptr_t)idx);
    lv_bar_set_range(r.volBar, 0, 100);
    lv_obj_set_style_bg_color(r.volBar, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(r.volBar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.volBar, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r.volBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r.volBar, 14, 0);
    lv_obj_set_style_clip_corner(r.volBar, true, 0);
    showObj(r.volBar, false);
    static const char* const volIcons[] = {"volume-minus", "volume-plus"};
    static const char* const volFallback[] = {"-", "+"};
    for (int i = 0; i < 2; i++) {
      r.volIcons[i] = lv_label_create(r.volBar);
      mediaGlyph(r.volIcons[i], volIcons[i], volFallback[i], 24);
    }
    r.iconCp = 2;  // force the first play glyph
    s_tiles.push_back(r);
    applyMedia(s_tiles.back());
    lv_obj_add_event_cb(t, mediaEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(idx * 8 + 3));
#if BOARD_HAS_ENCODER
    lv_obj_add_event_cb(t, mediaEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)(idx * 8 + 3));
#endif
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_VIZ) {
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeVizContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    lv_obj_add_event_cb(r.viz, vizDrawEvent, LV_EVENT_DRAW_MAIN, r.vz);
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_WEATHER) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeWeatherContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    lv_obj_add_event_cb(r.viz, wxDrawEvent, LV_EVENT_DRAW_MAIN, (void*)(uintptr_t)idx);
    lv_obj_add_event_cb(t, wxTap, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Roll and Orbit clocks: one object the deck draws, see deck_ui_clock.cpp.
  if (def.type == TILE_CLOCK && def.clockStyle != CLOCK_CLASSIC) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeClockFx(r, t);
    if (r.cfx) {  // else Classic
      lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
      const size_t idx = s_tiles.size();
      s_tiles.push_back(r);
      applyClock(s_tiles.back());
      lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
      return;
    }
  }

  // Clock: the time ("color") over the date, centred; fitClock sizes them.
  if (def.type == TILE_CLOCK) {
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    lv_obj_t* row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t* hm = lv_obj_create(row);
    lv_obj_remove_style_all(hm);
    lv_obj_set_size(hm, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hm, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(hm, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (def.hasColor) lv_obj_set_style_text_color(hm, lv_color_hex(def.color), 0);
    r.sub = lv_label_create(hm);
    r.colon = lv_label_create(hm);
    lv_label_set_text(r.colon, ":");
    r.colonLit = true;
    r.mins = lv_label_create(hm);
    r.unit = lv_label_create(row);
    lv_obj_set_style_text_opa(r.unit, LV_OPA_70, 0);
    r.name = lv_label_create(t);
    lv_obj_set_style_text_opa(r.name, LV_OPA_70, 0);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyClock(s_tiles.back());
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Slider: the level fill goes in first so it stays behind the content; a
  // cover's fill is the shade, from the top.
  lv_obj_t* fill = nullptr;
  if (def.type == TILE_SLIDER || def.type == TILE_COVER) {
    fill = lv_obj_create(t);
    lv_obj_remove_style_all(fill);
    lv_obj_add_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(fill, 16, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(fill, s_theme == THEME_DAYLIGHT ? LV_OPA_40 : LV_OPA_50, 0);
    if (def.type == TILE_SLIDER) lv_obj_add_flag(t, LV_OBJ_FLAG_PRESS_LOCK);  // the drag may leave the tile
  }
  // Cover and light presets: icon, state and name share a box that leaves room for the buttons.
  lv_obj_t* host = t;
  lv_obj_t* box = nullptr;
  if (def.type == TILE_COVER || hasPresets(def)) {
    host = box = lv_obj_create(t);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  }

  lv_obj_t* chip = lv_obj_create(host);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  styleIconChip(chip, accent);
  if (def.iconPlain) lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
  if (def.hasIconColor) lv_obj_set_style_text_color(chip, lv_color_hex(def.iconColor), 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_obj_center(ic);

  // Fonts and places come from fitTile once the tile has its size.
  lv_obj_t* name = lv_label_create(host);
  lv_label_set_text(name, def.noName ? "" : tileLabel(def).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_t* sub = lv_label_create(host);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
  if (def.type == TILE_SENSOR) {
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);
  }

  const int iconPx = def.type == TILE_SENSOR ? 16 : 20;
  TileRt r = {};
  r.def = &def;
  r.page = page;
  r.obj = t;
  r.name = name;
  r.sub = sub;
  r.icon = ic;
  r.chip = chip;
  r.iconPx = iconPx;
  r.iconCp = showIcon(ic, def, nullptr, false, iconPx, 0);
  r.fill = fill;
  r.box = box;
  if (def.type == TILE_SENSOR) {
    r.detail = makeDetail(t, def);
    r.rows = makeRows(t, def);
  }
  lv_label_set_text(sub, tileSub(r).c_str());
  const size_t idx = s_tiles.size();
  s_tiles.push_back(r);
  if (def.type == TILE_COVER) makeCoverButtons(s_tiles.back(), idx, accent);
  else if (box) makePresetButtons(s_tiles.back(), idx);
  if (def.tap.kind == ACT_PC) applyPcStateToTile(s_tiles.back());
  lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
  if (def.type == TILE_SLIDER)
    for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                              LV_EVENT_SIZE_CHANGED})
      lv_obj_add_event_cb(t, sliderEvent, c, (void*)(uintptr_t)idx);
}

}  // namespace deckui
