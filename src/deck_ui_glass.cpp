// Deck UI: Glass theme bake and auto text colour.
#include "deck_ui_int.h"

namespace deckui {

// Keep the exact draw inputs, including child order, to avoid hash collisions.
// Coordinates are page-relative so moving a page offscreen does not change its key.
TileRt* tileOf(lv_obj_t* o);

std::vector<uint32_t> glassKey(Page& p) {
  if (!wallSharp() || !wallBlur() || !p.root || !p.grid) return {};
  lv_obj_update_layout(p.root);
  std::vector<uint32_t> key = {
      wallGeneration(), (uint32_t)logicalW(), (uint32_t)logicalH(), (uint32_t)panelRotation(),
      s_theme, C_TILE, dispSettings.glassTint, dispSettings.glassBlur, dispSettings.glassDim,
      dispSettings.glassOn, dispSettings.glassAutoText, GLASS_BORDER_COLOR, GLASS_BORDER_OPA,
      GLASS_BORDER_WIDTH};
  const uint32_t count = lv_obj_get_child_count(p.grid);
  key.reserve(key.size() + 1 + count * 7);
  key.push_back(count);
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t* t = lv_obj_get_child(p.grid, i);
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    for (int32_t v : {tc.x1, tc.y1, tc.x2, tc.y2, lv_obj_get_style_radius(t, LV_PART_MAIN)})
      key.push_back((uint32_t)v);
    const TileRt* r = tileOf(t);
    key.push_back(r ? tileBg(*r->def) : C_TILE);
    key.push_back(r ? (uint32_t)tileTint(*r->def) : dispSettings.glassTint);
  }
  return key;
}

// Glass auto text: light text needs the glass at or below AUTO_LIGHT_MAX luma,
// dark text at or above AUTO_DARK_MIN (about 3:1 contrast either way).
constexpr int AUTO_LIGHT_MAX = 132, AUTO_DARK_MIN = 107;
constexpr uint32_t AUTO_DARK_TEXT = 0x1D2129, AUTO_DARK_TITLE = 0x161A20, AUTO_DARK_CLOCK = 0x3A404C;

int luma(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }
int luma(uint32_t c) { return luma(c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF); }

// 10th and 90th percentile luma of an image area (RGB565, or RGB888 as B, G, R),
// from every other pixel and row.
void lumaRange(const lv_image_dsc_t* img, lv_area_t a, int& lo, int& hi) {
  lo = hi = 0;
  const int w = img->header.w, h = img->header.h;
  a.x1 = max<int32_t>(a.x1, 0);
  a.y1 = max<int32_t>(a.y1, 0);
  a.x2 = min<int32_t>(a.x2, w - 1);
  a.y2 = min<int32_t>(a.y2, h - 1);
  uint16_t hist[256] = {};
  uint32_t n = 0;
  const bool rgb565 = img->header.cf == LV_COLOR_FORMAT_RGB565;
  for (int y = a.y1; y <= a.y2; y += 2) {
    const uint8_t* row = img->data + y * img->header.stride;
    for (int x = a.x1; x <= a.x2; x += 2, n++) {
      if (rgb565) {
        const uint16_t q = ((const uint16_t*)row)[x];
        hist[luma(q >> 8 & 0xF8, q >> 3 & 0xFC, q << 3 & 0xF8)]++;
      } else hist[luma(row[x * 3 + 2], row[x * 3 + 1], row[x * 3])]++;
    }
  }
  uint32_t acc = 0;
  bool low = false;
  for (int v = 0; v < 256 && n; v++) {
    acc += hist[v];
    if (!low && acc * 10 >= n) lo = v, low = true;
    if (acc * 10 >= n * 9) {
      hi = v;
      break;
    }
  }
}

struct AutoPick {
  lv_opa_t opa;   // glass tint of the tile
  uint32_t text;
  bool set;       // auto text applies to this tile
};

// Light text if it reads, else dark text, else glass darkened until light
// text reads. Printer tiles and tiles with their own text colour stay light.
AutoPick autoPick(const TileRt* r, const lv_area_t& a) {
  const int base = r ? tileTint(*r->def) : dispSettings.glassTint;
  AutoPick pk = {(lv_opa_t)(base * 255 / 100), C_TEXT, false};
  if (!dispSettings.glassAutoText || !r || r->def->hasTextColor || !wallBlur()) return pk;
  int lo, hi;
  lumaRange(wallBlur(), a, lo, hi);
  const int tint = luma(tileBg(*r->def));
  auto glassed = [&](int v) { return (v * (100 - base) + tint * base) / 100; };
  pk.set = true;
  if (glassed(hi) <= AUTO_LIGHT_MAX) return pk;
  if (r->def->type != TILE_PRINTER && glassed(lo) >= AUTO_DARK_MIN) {
    pk.text = AUTO_DARK_TEXT;
    return pk;
  }
  if (tint > AUTO_LIGHT_MAX) {
    // A light tint ("bgColor"): more glass brightens it toward dark text.
    if (r->def->type == TILE_PRINTER) return pk;
    const int pct = min(95, ((AUTO_DARK_MIN - lo) * 100 + (tint - lo) - 1) / (tint - lo));
    pk.text = AUTO_DARK_TEXT;
    pk.opa = (lv_opa_t)(max(pct, base) * 255 / 100);
    return pk;
  }
  const int pct = min(95, ((hi - AUTO_LIGHT_MAX) * 100 + (hi - tint) - 1) / (hi - tint));
  pk.opa = (lv_opa_t)(max(pct, base) * 255 / 100);
  return pk;
}

TileRt* tileOf(lv_obj_t* o) {
  for (TileRt& r : s_tiles)
    if (r.obj == o) return &r;
  return nullptr;
}

void applyAutoPick(TileRt& r, const AutoPick& pk) {
  if (!pk.set && !r.hasAuto) return;
  r.hasAuto = pk.set;
  r.autoText = pk.text;
  const uint32_t text = tileTextRt(r), on = onTextColor(*r.def, true);
  lv_obj_set_style_text_color(r.obj, lv_color_hex(text), 0);
  lv_obj_set_style_text_color(r.obj, lv_color_hex(on == C_TEXT && !r.def->hasOnTextColor ? text : on),
                              LV_STATE_CHECKED);
  if (r.sub && r.def->type == TILE_SENSOR && !r.threshold)
    lv_obj_set_style_text_color(r.sub, lv_color_hex(text), 0);
  if (r.sub && r.def->type == TILE_METRIC && !r.warn) lv_obj_set_style_text_color(r.sub, lv_color_hex(text), 0);
}

// The top bar sits on the sharp picture, no glass.
void autoHeader(Page& p) {
  const lv_image_dsc_t* sharp = wallSharp();
  if (!p.title || !p.clock || !sharp) return;
  bool dark = false;
  if (dispSettings.glassAutoText) {
    int lo, hi;
    lumaRange(sharp, {0, 0, (int32_t)sharp->header.w - 1, HEADER_H - 1}, lo, hi);
    dark = hi > AUTO_LIGHT_MAX && lo >= AUTO_DARK_MIN;
  }
  lv_obj_set_style_text_color(p.title, lv_color_hex(dark ? AUTO_DARK_TITLE : C_TITLE), 0);
  lv_obj_set_style_text_color(p.clock, lv_color_hex(dark ? AUTO_DARK_CLOCK : C_CLOCK), 0);
}

// Re-applies the picks of a page whose bake came from the cache.
void autoTextPage(Page& p) {
  if (!p.root || !p.grid) return;
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (uint32_t i = 0; i < lv_obj_get_child_count(p.grid); i++) {
    lv_obj_t* t = lv_obj_get_child(p.grid, i);
    TileRt* r = tileOf(t);
    if (!r) continue;
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    applyAutoPick(*r, autoPick(r, tc));
  }
  autoHeader(p);
}

// Glass: the page background is baked once the tiles are laid out: the
// wallpaper, and under each tile the blurred wallpaper cut to its rounded
// rect with the tint and a hairline. A page render is then one image copy
// instead of per-tile blending.
bool bakeGlass(Page& p) {
  p.glassKey.clear();  // a partial bake must never be reused
  const lv_image_dsc_t* sharp = wallSharp();
  const lv_image_dsc_t* blur = wallBlur();
  if (!sharp || !blur || !p.root || !p.grid) return false;
  const int w = sharp->header.w, h = sharp->header.h;
  if (!p.glass) p.glass = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!p.glass) return false;
  lv_image_cache_drop(p.glass);
  for (int y = 0; y < h; y++)
    memcpy(p.glass->data + y * p.glass->header.stride, sharp->data + y * sharp->header.stride, w * 2);
  // The glass is drawn in ARGB8888 over nothing, then laid over the picture
  // and dithered to RGB565 once: blending in RGB565 left coloured bands.
  lv_draw_buf_t* top = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
  if (!top) return false;
  lv_draw_buf_clear(top, nullptr);
  std::vector<uint32_t> key = glassKey(p);
  lv_obj_t* cv = lv_canvas_create(p.root);
  lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_draw_buf(cv, top);
  lv_layer_t layer;
  lv_canvas_init_layer(cv, &layer);
  const lv_area_t all = {0, 0, w - 1, h - 1};
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  for (uint32_t i = 0; i < lv_obj_get_child_count(p.grid); i++) {
    lv_obj_t* t = lv_obj_get_child(p.grid, i);
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    const int r = lv_obj_get_style_radius(t, LV_PART_MAIN);
    lv_draw_image_dsc_t id;
    lv_draw_image_dsc_init(&id);
    id.src = blur;
    id.image_area = tc;  // the radius mask works on this area
    id.clip_radius = r;
    layer._clip_area = tc;
    lv_draw_image(&layer, &id, &all);
    layer._clip_area = all;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = r;
    TileRt* tr = tileOf(t);
    const AutoPick pk = autoPick(tr, tc);
    if (tr) applyAutoPick(*tr, pk);
    rd.bg_color = lv_color_hex(tr ? tileBg(*tr->def) : C_TILE);
    rd.bg_opa = pk.opa;
    rd.border_color = lv_color_hex(GLASS_BORDER_COLOR);
    rd.border_opa = GLASS_BORDER_OPA;
    rd.border_width = GLASS_BORDER_WIDTH;
    lv_draw_rect(&layer, &rd, &tc);
  }
  lv_canvas_finish_layer(cv, &layer);
  lv_obj_delete(cv);
  for (int y = 0; y < h; y++) {
    const uint8_t* s = top->data + y * top->header.stride;  // B, G, R, A
    uint16_t* d = (uint16_t*)(p.glass->data + y * p.glass->header.stride);
    for (int x = 0; x < w; x++, s += 4) {
      const int a = s[3];
      if (!a) continue;
      int r = s[2], g = s[1], b = s[0];
      if (a < 255) {  // rounded edge: mix with the picture under it
        const uint16_t q = d[x];
        r = (r * a + ((q >> 8 & 0xF8) | q >> 13) * (255 - a) + 127) / 255;
        g = (g * a + ((q >> 3 & 0xFC) | (q >> 9 & 0x03)) * (255 - a) + 127) / 255;
        b = (b * a + ((q << 3 & 0xF8) | (q >> 2 & 0x07)) * (255 - a) + 127) / 255;
      }
      d[x] = wallDither565(r, g, b, x, y);
    }
  }
  lv_draw_buf_destroy(top);
  autoHeader(p);
  lv_obj_set_style_bg_image_src(p.root, p.glass, 0);
  lv_obj_invalidate(p.root);
  p.stale = true;
  p.glassKey.swap(key);
  return true;
}

}  // namespace deckui
