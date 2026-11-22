// Deck UI: Glass theme bake and auto text colour.
#include "deck_ui_int.h"
#include <atomic>
#include <math.h>

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
    if (lv_obj_has_flag(t, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    for (int32_t v : {tc.x1, tc.y1, tc.x2, tc.y2, lv_obj_get_style_radius(t, LV_PART_MAIN)})
      key.push_back((uint32_t)v);
    const TileRt* r = tileOf(t);
    const FrameRt* f = r ? nullptr : frameOf(t);
    key.push_back(r ? tileBg(*r->def) : f ? frameBg(*f->def) : C_TILE);
    key.push_back(r ? (uint32_t)tileTint(*r->def) | (r->frameObj ? 0x100u : 0) : f ? frameTint(*f->def) : dispSettings.glassTint);
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
  uint32_t hist[256] = {};  // a large tile has over 65k samples
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

// One tile's glass, in page coordinates.
struct GlassTile {
  int16_t x1, y1, x2, y2, radius;
  uint8_t base;    // tint percent
  uint8_t mode;    // auto text: 0 off, 1 on, 2 printer tile (stays light)
  bool hit;        // inside the baked area
  uint32_t bg;
  uint32_t child;  // index in the page grid
  AutoPick pick;
  int16_t frame;   // a tile in a frame: the frame's index in the plan (no glass of its own), else -1
};

// All a bake reads, so it can run off the loop task.
struct GlassPlan {
  const lv_image_dsc_t* sharp;
  const lv_image_dsc_t* blur;
  std::vector<GlassTile> tiles;
};

// Light text if it reads, else dark text, else glass darkened until light
// text reads. Printer tiles and tiles with their own text colour stay light.
AutoPick pickFor(const GlassTile& t, const lv_image_dsc_t* blur) {
  const int base = t.base;
  AutoPick pk = {(lv_opa_t)(base * 255 / 100), C_TEXT, false};
  if (!t.mode || !blur) return pk;
  int lo, hi;
  lumaRange(blur, {t.x1, t.y1, t.x2, t.y2}, lo, hi);
  const int tint = luma(t.bg);
  auto glassed = [&](int v) { return (v * (100 - base) + tint * base) / 100; };
  pk.set = true;
  if (glassed(hi) <= AUTO_LIGHT_MAX) return pk;
  if (t.mode != 2 && glassed(lo) >= AUTO_DARK_MIN) {
    pk.text = AUTO_DARK_TEXT;
    return pk;
  }
  if (tint > AUTO_LIGHT_MAX) {
    // A light tint ("bgColor"): more glass brightens it toward dark text.
    if (t.mode == 2) return pk;
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

// a: the tile on its page. A tile in a frame reads its text against the frame's glass.
GlassTile glassTile(lv_obj_t* t, const lv_area_t& a, uint32_t child) {
  const TileRt* r = tileOf(t);
  const FrameRt* f = r ? (r->frameObj ? frameOf(r->frameObj) : nullptr) : frameOf(t);
  GlassTile g = {};
  g.x1 = a.x1, g.y1 = a.y1, g.x2 = a.x2, g.y2 = a.y2;
  g.radius = lv_obj_get_style_radius(t, LV_PART_MAIN);
  g.base = f ? frameTint(*f->def) : r ? tileTint(*r->def) : dispSettings.glassTint;
  g.mode = !dispSettings.glassAutoText || !r || r->def->hasTextColor ? 0 : r->def->type == TILE_PRINTER ? 2 : 1;
  g.bg = f ? frameBg(*f->def) : r ? tileBg(*r->def) : C_TILE;
  g.child = child;
  g.frame = -1;
  return g;
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
    if (!r || r->off) continue;
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    applyAutoPick(*r, pickFor(glassTile(t, tc, i), wallBlur()));
  }
  autoHeader(p);
}

namespace {

bool makePlan(Page& p, GlassPlan& pl) {
  pl.sharp = wallSharp();
  pl.blur = wallBlur();
  pl.tiles.clear();
  if (!pl.sharp || !pl.blur || !p.root || !p.grid) return false;
  lv_obj_update_layout(p.root);
  lv_area_t rc;
  lv_obj_get_coords(p.root, &rc);
  const uint32_t count = lv_obj_get_child_count(p.grid);
  pl.tiles.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t* t = lv_obj_get_child(p.grid, i);
    if (lv_obj_has_flag(t, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t tc;
    lv_obj_get_coords(t, &tc);
    lv_area_move(&tc, -rc.x1, -rc.y1);
    pl.tiles.push_back(glassTile(t, tc, i));
    // Frames come first in the grid, so a framed tile finds its frame already planned.
    const TileRt* r = tileOf(t);
    if (!r || !r->frameObj) continue;
    for (size_t k = 0; k < pl.tiles.size(); k++)
      if (lv_obj_get_child(p.grid, pl.tiles[k].child) == r->frameObj) pl.tiles.back().frame = (int16_t)k;
  }
  return true;
}

// How much of pixel (x, y) a w x h rect with corner radius r covers, 0..1.
float cornerCov(int x, int y, int w, int h, int r) {
  const float cx = std::max(0.0f, std::max((float)r - ((float)x + 0.5f), (float)x + 0.5f - (float)(w - r)));
  const float cy = std::max(0.0f, std::max((float)r - ((float)y + 0.5f), (float)y + 0.5f - (float)(h - r)));
  if (cx <= 0 || cy <= 0) return 1;
  const float c = (float)r + 0.5f - sqrtf(cx * cx + cy * cy);
  return c >= 1 ? 1 : c <= 0 ? 0 : c;
}

// Area a of the page into out (RGB565): the picture, and under each tile the
// blurred picture cut to its rounded rect, with the tint and a hairline.
// Mixed in 8 bits and dithered once: blending in RGB565 left coloured bands.
// Plain pixel code, no LVGL: also runs on a worker task.
void glassRender(GlassPlan& pl, uint8_t* out, uint32_t stride, const lv_area_t& a, const std::atomic<bool>* cancel) {
  constexpr int BW = GLASS_BORDER_WIDTH, BOPA = GLASS_BORDER_OPA;
  constexpr int LR = GLASS_BORDER_COLOR >> 16 & 0xFF, LG = GLASS_BORDER_COLOR >> 8 & 0xFF, LB = GLASS_BORDER_COLOR & 0xFF;
  for (GlassTile& t : pl.tiles) {
    t.hit = !(t.x2 < a.x1 || t.x1 > a.x2 || t.y2 < a.y1 || t.y1 > a.y2);
    if (t.frame >= 0) t.hit = pl.tiles[t.frame].hit;  // its text reads against all of the frame
    if (t.hit) t.pick = pickFor(t, pl.blur);
    if (cancel && cancel->load()) return;
  }
  // A frame darkens as far as the tile in it that needs the most.
  for (const GlassTile& t : pl.tiles)
    if (t.frame >= 0 && t.hit) pl.tiles[t.frame].pick.opa = std::max(pl.tiles[t.frame].pick.opa, t.pick.opa);
  for (int y = a.y1; y <= a.y2; y++) {
    if (cancel && cancel->load()) return;
    uint16_t* d = (uint16_t*)(out + (size_t)y * stride);
    memcpy(d + a.x1, pl.sharp->data + (size_t)y * pl.sharp->header.stride + a.x1 * 2, (a.x2 - a.x1 + 1) * 2);
    const uint8_t* brow = pl.blur->data + (size_t)y * pl.blur->header.stride;  // B, G, R
    for (const GlassTile& t : pl.tiles) {
      if (!t.hit || t.frame >= 0 || y < t.y1 || y > t.y2) continue;
      const int tw = t.x2 - t.x1 + 1, th = t.y2 - t.y1 + 1, ly = y - t.y1;
      const int r = std::min<int>(t.radius, std::min(tw, th) / 2), e = std::max(r, BW);
      const int opa = t.pick.opa, ia = 255 - opa;
      const int tr = (int)(t.bg >> 16 & 0xFF) * opa, tg = (int)(t.bg >> 8 & 0xFF) * opa,
                tb = (int)(t.bg & 0xFF) * opa;
      const bool edgeRow = ly < e || ly >= th - e, lineRow = ly < BW || ly >= th - BW;
      const int xl = std::max<int>(t.x1, a.x1), xr = std::min<int>(t.x2, a.x2);
      for (int x = xl; x <= xr; x++) {
        const uint8_t* s = brow + x * 3;
        int R = (s[2] * ia + tr) / 255, G = (s[1] * ia + tg) / 255, B = (s[0] * ia + tb) / 255;
        const int lx = x - t.x1;
        const bool side = lx < BW || lx >= tw - BW;
        if (edgeRow && (lx < e || lx >= tw - e)) {
          // A corner: the hairline lies between the outer and the inner rounded rect.
          const float co = cornerCov(lx, ly, tw, th, r);
          if (co <= 0) continue;
          const float ci = side || lineRow ? 0 : cornerCov(lx - BW, ly - BW, tw - 2 * BW, th - 2 * BW, std::max(r - BW, 0));
          const float line = std::max(co - ci, 0.0f) / co * (BOPA / 255.0f);
          float fr = R + (LR - R) * line, fg = G + (LG - G) * line, fb = B + (LB - B) * line;
          if (co < 1) {  // the rounded edge: mix with the picture under it
            const uint16_t q = d[x];
            fr = fr * co + ((q >> 8 & 0xF8) | q >> 13) * (1 - co);
            fg = fg * co + ((q >> 3 & 0xFC) | (q >> 9 & 0x03)) * (1 - co);
            fb = fb * co + ((q << 3 & 0xF8) | (q >> 2 & 0x07)) * (1 - co);
          }
          R = (int)(fr + 0.5f), G = (int)(fg + 0.5f), B = (int)(fb + 0.5f);
        } else if (side || lineRow) {
          R = (R * (255 - BOPA) + LR * BOPA) / 255;
          G = (G * (255 - BOPA) + LG * BOPA) / 255;
          B = (B * (255 - BOPA) + LB * BOPA) / 255;
        }
        d[x] = wallDither565(R, G, B, x, y);
      }
    }
  }
}

void applyPicks(Page& p, const GlassPlan& pl) {
  for (const GlassTile& t : pl.tiles) {
    if (!t.hit) continue;
    TileRt* r = tileOf(lv_obj_get_child(p.grid, t.child));
    if (r) applyAutoPick(*r, t.pick);
  }
}

// One page baked on a worker into a spare buffer, which the loop then swaps
// with the page's own: a wallpaper change does not stall the animations.
struct AsyncBake {
  std::atomic<int> state{0};  // 0 idle, 1 running, 2 done
  std::atomic<bool> cancel{false};
  int page = -1;
  uint32_t ms = 0;
  GlassPlan plan;
  std::vector<uint32_t> key;
  lv_draw_buf_t* spare = nullptr;
};
AsyncBake s_ab;

void asyncJob() {
  const uint32_t start = millis();
  const lv_area_t all = {0, 0, (int32_t)s_ab.spare->header.w - 1, (int32_t)s_ab.spare->header.h - 1};
  glassRender(s_ab.plan, s_ab.spare->data, s_ab.spare->header.stride, all, &s_ab.cancel);
  s_ab.ms = millis() - start;
  s_ab.state = 2;
}

}  // namespace

// Glass: the page background is baked once the tiles are laid out. A page
// render is then one image copy instead of per-tile blending. only: just this
// part of the page (tiles came or went there), the rest of a finished bake stays.
bool bakeGlass(Page& p, const lv_area_t* only) {
  const bool part = only && p.glass && !p.glassKey.empty();
  p.glassKey.clear();  // a partial bake must never be reused
  GlassPlan pl;
  if (!makePlan(p, pl)) return false;
  const int w = pl.sharp->header.w, h = pl.sharp->header.h;
  if (!p.glass) p.glass = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!p.glass) return false;
  std::vector<uint32_t> key = glassKey(p);
  const lv_area_t a = {part ? LV_MAX(only->x1, 0) : 0, part ? LV_MAX(only->y1, 0) : 0,
                       part ? LV_MIN(only->x2, w - 1) : w - 1, part ? LV_MIN(only->y2, h - 1) : h - 1};
  lv_image_cache_drop(p.glass);
  if (a.x1 <= a.x2 && a.y1 <= a.y2) glassRender(pl, p.glass->data, p.glass->header.stride, a, nullptr);
  applyPicks(p, pl);
  if (part) {
    lv_area_t rc;
    lv_obj_get_coords(p.root, &rc);
    const lv_area_t inv = {rc.x1 + a.x1, rc.y1 + a.y1, rc.x1 + a.x2, rc.y1 + a.y2};
    lv_obj_invalidate_area(p.root, &inv);
  } else {
    autoHeader(p);
    lv_obj_set_style_bg_image_src(p.root, p.glass, 0);
    lv_obj_invalidate(p.root);
  }
  p.stale = true;
  p.glassKey.swap(key);
  return true;
}

bool glassBakeStart(int page) {
  Page& p = s_pages[page];
  const lv_image_dsc_t* sharp = wallSharp();
  if (s_ab.state != 0 || !sharp) return false;
  const int w = sharp->header.w, h = sharp->header.h;
  if (s_ab.spare && (s_ab.spare->header.w != w || s_ab.spare->header.h != h)) {
    lv_draw_buf_destroy(s_ab.spare);
    s_ab.spare = nullptr;
  }
  if (!s_ab.spare) s_ab.spare = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!s_ab.spare || !makePlan(p, s_ab.plan)) return false;
  s_ab.key = glassKey(p);
  s_ab.page = page;
  s_ab.cancel = false;
  s_ab.state = 1;
  if (wallRunWorker(asyncJob, false, "deck_glass")) return true;
  s_ab.state = 0;
  return false;
}

bool glassBakeRunning() { return s_ab.state == 1; }

bool glassBakeTake(int& page, uint32_t& ms) {
  if (s_ab.state != 2) return false;
  s_ab.state = 0;
  page = -1;
  ms = s_ab.ms;
  Page& p = s_pages[s_ab.page];
  // The tiles moved meanwhile: the page is baked again.
  if (s_ab.cancel || !p.root || s_ab.key.empty() || s_ab.key != glassKey(p)) return true;
  lv_draw_buf_t* old = p.glass;
  p.glass = s_ab.spare;
  s_ab.spare = old;
  if (old) lv_image_cache_drop(old);
  lv_image_cache_drop(p.glass);
  applyPicks(p, s_ab.plan);
  autoHeader(p);
  lv_obj_set_style_bg_image_src(p.root, p.glass, 0);
  lv_obj_invalidate(p.root);
  p.stale = true;
  p.glassKey.swap(s_ab.key);
  page = s_ab.page;
  return true;
}

void glassBakeStop() {
  if (s_ab.state == 1) {
    s_ab.cancel = true;
    while (s_ab.state == 1) delay(1);
  }
  s_ab.state = 0;
}

void glassBakeRelease() {
  glassBakeStop();
  if (s_ab.spare) lv_draw_buf_destroy(s_ab.spare);
  s_ab.spare = nullptr;
  s_ab.plan.tiles.clear();
}

}  // namespace deckui
