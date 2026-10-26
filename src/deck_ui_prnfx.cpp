// Deck UI: printer tile animations. Temperatures count one degree at a time
// and (printerFx) glow while heating, frost over while cooling; the filament
// tile turns a spool and runs a print head extruding the active filament.
// The effects are ARGB pictures painted by the CPU, drawn by the tile.
#include "deck_ui_int.h"
#include <climits>
#include <new>

namespace deckui {

enum : uint8_t { TS_IDLE, TS_HEAT, TS_HOLD, TS_COOL };

constexpr int COUNT_FRAMES = 24;        // a jump counts out in about 0.8 s
constexpr uint32_t REACH_MS = 900;      // sheen when a target is reached
constexpr int HEAD_W = 16;              // print head column
constexpr int HEAD_TIP = 17;            // nozzle tip, rows from the column top
// Filament change: retract, unwind, (pause), extrude and wind the new colour.
constexpr uint32_t SWAP_RETRACT = 700, SWAP_UNWIND = 1260, SWAP_OUT = 1510, SWAP_END = 2070;

struct PrnFx {
  // Temperatures
  lv_obj_t* over;          // effects under the reading, the whole tile
  lv_obj_t* shine;         // light running along the bar while heating
  lv_draw_buf_t* buf;
  uint8_t* tables;         // per pixel: glow alpha, then frost alpha
  int tw, th;              // size the tables were made for
  float goal, target, scale;
  int shown;               // counted reading, INT_MIN = none
  uint32_t color;
  uint8_t state;
  uint32_t reachMs;
  float glow, haze, frost;
  bool painted;
  // Filament
  lv_obj_t* spool;
  lv_obj_t* head;
  lv_draw_buf_t* sbuf;
  lv_draw_buf_t* hbuf;
  bool has, clear, feeding, printing, dirty, nameDirty;
  int nameW;               // material label width its font was picked for
  uint32_t rgb, oldRgb, swapMs;
  float angle, level, flow, phase;
  uint32_t lastMs;
};

void fxDrawEvent(lv_event_t* e) {
  const lv_draw_buf_t* b = *(lv_draw_buf_t* const*)lv_event_get_user_data(e);
  lv_obj_t* o = lv_event_get_target_obj(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if (!b || lv_area_get_width(&a) != (int32_t)b->header.w || lv_area_get_height(&a) != (int32_t)b->header.h) return;
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = b;
  lv_draw_image(lv_event_get_layer(e), &d, &a);
}

lv_obj_t* fxObj(lv_obj_t* parent, lv_draw_buf_t** buf) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  if (buf) lv_obj_add_event_cb(o, fxDrawEvent, LV_EVENT_DRAW_MAIN, buf);
  return o;
}

lv_draw_buf_t* fxBuf(lv_draw_buf_t*& b, int w, int h) {
  if (b && ((int)b->header.w != w || (int)b->header.h != h)) {
    lv_draw_buf_destroy(b);
    b = nullptr;
  }
  if (!b && w > 0 && h > 0) {
    b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (b) lv_draw_buf_clear(b, nullptr);
  }
  return b;
}

// Premultiplied colour, composed back to front.
struct Acc {
  float r = 0, g = 0, b = 0, a = 0;
};
inline float clamp1(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline void over(Acc& d, uint32_t rgb, float a) {
  if (a <= 0) return;
  if (a > 1) a = 1;
  const float k = 1 - a;
  d.r = (float)((rgb >> 16) & 0xFF) * a + d.r * k;
  d.g = (float)((rgb >> 8) & 0xFF) * a + d.g * k;
  d.b = (float)(rgb & 0xFF) * a + d.b * k;
  d.a = a + d.a * k;
}
inline void store(uint8_t* p, const Acc& c, float cover) {
  const float a = c.a * cover;
  if (a < 0.004f) {
    *(uint32_t*)p = 0;
    return;
  }
  const float inv = 1 / c.a;
  p[0] = (uint8_t)LV_MIN(255.0f, c.b * inv);
  p[1] = (uint8_t)LV_MIN(255.0f, c.g * inv);
  p[2] = (uint8_t)LV_MIN(255.0f, c.r * inv);
  p[3] = (uint8_t)(LV_MIN(1.0f, a) * 255 + 0.5f);
}
uint32_t mixRgb(uint32_t a, uint32_t b, float k) {
  uint32_t out = 0;
  for (int s = 0; s <= 16; s += 8) {
    const float x = (float)((a >> s) & 0xFF), y = (float)((b >> s) & 0xFF);
    out |= (uint32_t)(x + (y - x) * k + 0.5f) << s;
  }
  return out;
}

// Coverage of a pixel inside a rounded rect w x h with radius rad.
float cornerCover(int x, int y, int w, int h, float rad) {
  const float px = x + 0.5f, py = y + 0.5f;
  const float cx = px < rad ? rad : px > w - rad ? w - rad : px;
  const float cy = py < rad ? rad : py > h - rad ? h - rad : py;
  if (cx == px || cy == py) return 1;
  return clamp1(rad + 0.5f - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)));
}

// showObj invalidates even when nothing changes; the tick runs every frame.
void setShown(lv_obj_t* o, bool on) {
  if (o && on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) showObj(o, on);
}

bool isTempTile(const TileDef& d) { return d.show == PS_NOZZLE || d.show == PS_BED || d.show == PS_CHAMBER; }

void prnFxMake(TileRt& r, lv_obj_t* t) {
  const TileDef& d = *r.def;
  if (!isTempTile(d) && d.show != PS_FILAMENT) return;
  PrnFx* f = (PrnFx*)lv_malloc_zeroed(sizeof(PrnFx));
  if (!f) return;
  new (f) PrnFx();
  r.pfx = f;
  f->shown = INT_MIN;
  f->goal = f->target = NAN;
  f->level = f->flow = 1;
  if (d.show == PS_FILAMENT) {
    f->spool = fxObj(t, &f->sbuf);
    lv_obj_move_to_index(f->spool, lv_obj_get_index(r.chip) + 1);
    f->head = fxObj(t, &f->hbuf);
    lv_obj_set_size(f->head, HEAD_W, lv_pct(100));
    lv_obj_move_to_index(f->head, lv_obj_get_index(r.px[3]));
    return;
  }
  if (d.show == PS_CHAMBER) return;  // no target: the reading only counts
  f->over = fxObj(t, &f->buf);
  lv_obj_add_flag(f->over, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
  lv_obj_move_to_index(f->over, 0);  // under the reading
  f->shine = fxObj(r.viz, nullptr);
  lv_obj_add_flag(f->shine, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_style_bg_color(f->shine, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(f->shine, LV_OPA_COVER, 0);
}

void prnFxFree(PrnFx* f) {
  if (!f) return;
  if (f->buf) lv_draw_buf_destroy(f->buf);
  if (f->sbuf) lv_draw_buf_destroy(f->sbuf);
  if (f->hbuf) lv_draw_buf_destroy(f->hbuf);
  if (f->tables) lv_free(f->tables);
  lv_free(f);
}

// --- Temperatures -----------------------------------------------------------------------
void tempShow(TileRt& r, const PrnFx& f) {
  char b[16];
  if (f.shown == INT_MIN) strcpy(b, "--");
  else snprintf(b, sizeof(b), "%d\xC2\xB0", f.shown);
  if (strcmp(lv_label_get_text(r.sub), b)) lv_label_set_text(r.sub, b);
  const bool cold = dispSettings.printerFx && f.state == TS_COOL && f.over;
  const uint32_t c = cold ? 0xBFE3FF : f.color;
  lv_obj_set_style_text_color(r.sub, lv_color_hex(c), 0);
  lv_obj_set_style_bg_color(r.viz, lv_color_hex(cold ? 0x8CC8FF : f.color), LV_PART_INDICATOR);
  const int v = f.shown == INT_MIN ? 0 : (int)constrain(f.shown * 100.0f / f.scale, 0.0f, 100.0f);
  lv_bar_set_value(r.viz, v, LV_ANIM_OFF);
}

// The reading counts toward now while its page is shown, else jumps there.
void prnTempSet(TileRt& r, float now, float target, float scale, uint32_t color) {
  PrnFx* f = r.pfx;
  if (!f) return;
  f->goal = now;
  f->target = target;
  f->scale = scale;
  f->color = color;
  if (!isfinite(now)) f->shown = INT_MIN;
  else if (f->shown == INT_MIN || r.page != sh.active) f->shown = (int)lroundf(now);
  uint8_t st = TS_IDLE;
  if (isfinite(now) && f->over) {
    const bool heating = isfinite(target) && target > 0;
    const float floor = r.def->show == PS_BED ? 35 : 50;
    st = heating && now < target - 2   ? TS_HEAT
         : heating && now <= target + 2 ? TS_HOLD
         : now > floor                  ? TS_COOL
                                        : TS_IDLE;
  }
  if (f->state == TS_HEAT && st == TS_HOLD) f->reachMs = millis() | 1;
  f->state = st;
  tempShow(r, *f);
}

// Glow from below and frost from above, as alpha per pixel (CSS-like radial
// gradients: 0 .. 55% .. 75% of the ellipse).
void tempTables(PrnFx& f, int w, int h) {
  if (f.tables && f.tw == w && f.th == h) return;
  if (f.tables) lv_free(f.tables);
  f.tables = (uint8_t*)lv_malloc((size_t)w * h * 2);
  f.tw = w;
  f.th = h;
  if (!f.tables) return;
  auto grad = [](float d, float a0, float a1) {
    return d < 0.55f ? a0 + (a1 - a0) * d / 0.55f : d < 0.75f ? a1 * (1 - (d - 0.55f) / 0.2f) : 0.0f;
  };
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float px = x + 0.5f - w / 2.0f, py = y + 0.5f;
      const float dg = sqrtf(px * px / (0.81f * w * w) + (py - 1.12f * h) * (py - 1.12f * h) / (0.5625f * h * h));
      const float df = sqrtf(px * px / (1.21f * w * w) + (py + 0.15f * h) * (py + 0.15f * h) / (0.64f * h * h));
      f.tables[y * w + x] = (uint8_t)(255 * grad(dg, 0.6f, 0.18f));
      f.tables[w * h + y * w + x] = (uint8_t)(255 * grad(df, 0.42f, 0.10f));
    }
}

void tempPaint(TileRt& r, PrnFx& f, uint32_t now) {
  lv_draw_buf_t* b = f.buf;
  const int w = b->header.w, h = b->header.h;
  const float rad = (float)lv_obj_get_style_radius(r.obj, LV_PART_MAIN);
  const uint8_t* G = f.tables;
  const uint8_t* F = f.tables + w * h;
  const float pulse = f.state == TS_HEAT ? 0.875f + 0.125f * sinf(now * (6.2832f / 1800)) : 1;
  const float gl = f.glow * pulse, hz = f.haze * 0.11f, fr = f.frost;
  const float hph = fmodf(now * (16.0f / 1400), 16.0f);
  float sw = -9, swA = 0;
  if (f.reachMs) {
    const float t = (float)(now - f.reachMs) / REACH_MS;
    const float e = 1 - (1 - t) * (1 - t) * (1 - t);
    sw = 1.3f - 1.6f * e;
    swA = 0.42f * (1 - t);
  }
  // Frost sparkles twinkle in turn (mockup spots on a 110 x 95 tile).
  static const float kSpark[5][2] = {{18, 20}, {70, 14}, {44, 30}, {88, 36}, {28, 44}};
  float sx[5], sy[5], sa[5];
  for (int i = 0; i < 5; i++) {
    sx[i] = kSpark[i][0] * w / 110;
    sy[i] = kSpark[i][1] * h / 95;
    const float ph = fmodf(now / 2200.0f + i * 0.45f / 2.2f, 1.0f);
    const float s = sinf(ph * 3.1416f);
    sa[i] = fr * 0.95f * s * s;
  }
  for (int y = 0; y < h; y++) {
    uint8_t* row = b->data + y * b->header.stride;
    const float yb = (h - y - 0.5f) / h;  // 0 at the bottom
    const float mask = yb < 0.45f ? 1 - yb / 0.45f * 0.4f : yb < 0.85f ? 0.6f * (1 - (yb - 0.45f) / 0.4f) : 0;
    const float v = fmodf(y + hph, 16.0f);
    const float haze = hz * mask * clamp1(1 - fabsf(v - 8) / 1.6f);
    for (int x = 0; x < w; x++) {
      Acc c;
      const int i = y * w + x;
      if (gl > 0 && G[i]) over(c, 0xFF8C32, G[i] / 255.0f * gl);
      if (haze > 0) over(c, 0xFFBE6E, haze);
      if (fr > 0 && F[i]) over(c, 0x8CC8FF, F[i] / 255.0f * fr);
      for (int k = 0; k < 5; k++) {
        if (sa[k] <= 0.01f) continue;
        const float dx = x + 0.5f - sx[k], dy = y + 0.5f - sy[k];
        const float d2 = dx * dx + dy * dy;
        if (d2 > 16) continue;
        const float d = sqrtf(d2);
        over(c, 0x9FD3FF, sa[k] * 0.5f * (1 - d / 4));
        over(c, 0xDDF1FF, sa[k] * clamp1(1.5f + 0.5f - d));
      }
      if (swA > 0) {
        const float u = ((x - w / 2.0f) * 0.966f + (y - h / 2.0f) * 0.259f) / w + 0.5f;
        over(c, 0xFFECBE, swA * clamp1(1 - fabsf(u - sw) / 0.18f));
      }
      store(row + x * 4, c, cornerCover(x, y, w, h, rad));
    }
  }
}

// Effect levels ease to the state's over about a second.
float easeLevel(float v, float to, float dt) {
  const float step = dt;
  if (fabsf(to - v) <= step) return to;
  return v + (to > v ? step : -step);
}

bool tempTick(TileRt& r, PrnFx& f, uint32_t now, bool shown, bool on, bool frame) {
  bool drew = false;
  const float dt = LV_MIN(0.1f, (now - f.lastMs) / 1000.0f);
  f.lastMs = now;
  if (f.shown != INT_MIN && isfinite(f.goal)) {
    const int g = (int)lroundf(f.goal), diff = g - f.shown;
    if (diff) {
      const int step = shown ? LV_MIN(abs(diff), LV_MAX(1, (abs(diff) + COUNT_FRAMES - 1) / COUNT_FRAMES)) : abs(diff);
      f.shown += diff > 0 ? step : -step;
      tempShow(r, f);
      drew = shown;
    }
  }
  if (!f.over) return drew;
  if (!on) {
    setShown(f.over, false);
    setShown(f.shine, false);
    return drew;
  }
  const float tg = f.state == TS_HEAT ? 1 : f.state == TS_HOLD ? 0.22f : 0;
  const float th = f.state == TS_HEAT ? 1 : 0, tf = f.state == TS_COOL ? 1 : 0;
  const bool moving = f.glow != tg || f.haze != th || f.frost != tf;
  f.glow = easeLevel(f.glow, tg, dt);
  f.haze = easeLevel(f.haze, th, dt);
  f.frost = easeLevel(f.frost, tf, dt);
  if (f.reachMs && now - f.reachMs >= REACH_MS) f.reachMs = 0;
  const bool visible = f.glow > 0 || f.haze > 0 || f.frost > 0 || f.reachMs;
  setShown(f.over, visible);
  // The bar's light runs over the filled part only.
  const bool heat = shown && f.state == TS_HEAT;
  setShown(f.shine, heat);
  if (heat) {
    const int bw = lv_obj_get_width(r.viz), bh = lv_obj_get_height(r.viz);
    const int fill = bw * lv_bar_get_value(r.viz) / 100, sw = LV_MAX(6, bw / 8);
    const float u = fmodf(now / 1100.0f, 1.0f);
    if (lv_obj_get_style_width(f.shine, LV_PART_MAIN) != sw || lv_obj_get_style_height(f.shine, LV_PART_MAIN) != bh) {
      lv_obj_set_size(f.shine, sw, bh);
      lv_obj_set_style_radius(f.shine, bh / 2, 0);
    }
    lv_obj_set_pos(f.shine, (int)(u * LV_MAX(0, fill - sw)), 0);
    lv_obj_set_style_bg_opa(f.shine, (lv_opa_t)(170 * sinf(u * 3.1416f)), 0);
    drew = true;
  }
  if (!shown || !visible) return drew;
  const int w = lv_obj_get_width(r.obj), h = lv_obj_get_height(r.obj);
  if (!f.buf || (int)f.buf->header.w != w || (int)f.buf->header.h != h) f.painted = false;
  const bool animate = moving || f.state == TS_HEAT || f.state == TS_COOL || f.reachMs || !f.painted;
  if (!animate || (!frame && f.painted)) return drew;
  if (!fxBuf(f.buf, w, h)) return drew;
  tempTables(f, w, h);
  if (!f.tables) return drew;
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  lv_obj_set_pos(f.over, -pad, -pad);
  lv_obj_set_size(f.over, w, h);
  tempPaint(r, f, now);
  f.painted = true;
  lv_obj_invalidate(f.over);
  return true;
}

// --- Filament -----------------------------------------------------------------------------
// A printed spool seen face on: winding in the filament's colour, three spokes,
// the hub; the spokes, the hub holes and the strand end turn with angle.
void spoolPaint(PrnFx& f, uint32_t rgb) {
  lv_draw_buf_t* b = f.sbuf;
  const int n = b->header.w;
  const float k = n / 64.0f, half = n / 2.0f;
  const float R = 30 * k, hub = 11 * k, outer = hub + (26 * k - hub) * f.level, step = 1.15f * k;
  const float ro = outer - 0.6f * k, rimR = R - 1.1f * k, rimW = 1.1f * k;
  const uint32_t lit = mixRgb(rgb, 0xFFFFFF, 0.35f), dark = mixRgb(rgb, 0, 0.5f);
  const float wa = f.clear ? 0.6f : 1;
  float sc[3], ss[3], hx[3], hy[3];
  for (int i = 0; i < 3; i++) {
    const float a = f.angle + i * 2.0944f;
    sc[i] = cosf(a);
    ss[i] = sinf(a);
    hx[i] = cosf(a + 1.0472f) * 7.6f * k;
    hy[i] = sinf(a + 1.0472f) * 7.6f * k;
  }
  const float ex = cosf(f.angle + 0.9f) * ro, ey = sinf(f.angle + 0.9f) * ro;
  const bool wound = f.has && f.level > 0.01f;
  for (int y = 0; y < n; y++) {
    uint8_t* row = b->data + y * b->header.stride;
    for (int x = 0; x < n; x++) {
      const float dx = x + 0.5f - half, dy = y + 0.5f - half;
      const float r = sqrtf(dx * dx + dy * dy);
      if (r > R + 0.5f) {
        *(uint32_t*)(row + x * 4) = 0;
        continue;
      }
      Acc c;
      over(c, 0x181C22, 0.82f);
      if (wound && r >= hub && r < outer + 0.5f) {
        const int ring = (int)((r - hub) / step);
        const float noise = fmodf(fabsf(sinf(ring * 12.9898f) * 43758.547f), 1.0f) * 2 - 1;
        uint32_t base = mixRgb(rgb, noise > 0 ? 0xFFFFFF : 0, fabsf(noise) * 0.16f);
        const float lx = dx + 9 * k, ly = dy + 9 * k;
        const float dl = sqrtf(lx * lx + ly * ly) / outer;
        base = dl < 0.6f ? mixRgb(base, 0xFFFFFF, 0.22f * (1 - dl / 0.6f))
                         : mixRgb(base, 0, LV_MIN(0.28f, 0.28f * (dl - 0.6f) / 0.4f));
        over(c, base, clamp1(outer + 0.5f - r) * wa);
        if (f.level > 0.15f && fabsf(r - ro) < 0.8f * k + 0.5f) {
          float rel = atan2f(dy, dx) - f.angle;
          rel = fmodf(rel + 3 * 3.1416f, 2 * 3.1416f) - 3.1416f;
          if (rel > -0.2f && rel < 0.9f) over(c, lit, clamp1(0.8f * k + 0.5f - fabsf(r - ro)));
        }
        const float de = sqrtf((dx - ex) * (dx - ex) + (dy - ey) * (dy - ey));
        if (f.level > 0.15f && de < 1.2f * k + 0.5f) over(c, dark, clamp1(1.2f * k + 0.5f - de));
      }
      for (int i = 0; i < 3; i++) {
        const float along = dx * sc[i] + dy * ss[i];
        if (along < hub - 2 * k || along > R - k) continue;
        const float across = fabsf(-dx * ss[i] + dy * sc[i]);
        over(c, 0x1E222A, 0.72f * clamp1(3.2f * k + 0.5f - across));
      }
      const float dr = fabsf(r - rimR);
      if (dr < rimW + 0.5f) over(c, 0xD2DAE4, 0.85f * clamp1(rimW + 0.5f - dr));
      if (r < hub + 0.5f) {
        over(c, 0x2B313B, clamp1(hub + 0.5f - r));
        over(c, 0xFFFFFF, 0.35f * clamp1(1 - fabsf(r - hub + 0.5f)));
        if (r < 4.5f * k + 0.5f) over(c, 0x080A0E, 0.9f * clamp1(4.5f * k + 0.5f - r));
        for (int i = 0; i < 3; i++) {
          const float dh = sqrtf((dx - hx[i]) * (dx - hx[i]) + (dy - hy[i]) * (dy - hy[i]));
          if (dh < 1.4f * k + 0.5f) over(c, 0x080A0E, 0.9f * clamp1(1.4f * k + 0.5f - dh));
        }
      }
      store(row + x * 4, c, clamp1(R + 0.5f - r));
    }
  }
}

// Print head (fins, heater block, brass nozzle) and the strand waving down
// out of it; the wave travels down while extruding, up while retracting.
void headPaint(PrnFx& f, uint32_t rgb, bool glow) {
  lv_draw_buf_t* b = f.hbuf;
  const int w = b->header.w, h = b->header.h;
  const float cx = w / 2.0f, tip = HEAD_TIP, end = tip + (h - 2 - tip) * f.flow;
  const uint32_t lit = mixRgb(rgb, 0xFFFFFF, 0.45f), dark = mixRgb(rgb, 0, 0.5f);
  for (int y = 0; y < h; y++) {
    uint8_t* row = b->data + y * b->header.stride;
    const float yy = y + 0.5f;
    const bool strand = f.flow > 0.01f && yy >= tip && yy <= end + 0.5f;
    float xc = cx, sc = 1, capA = 1;
    if (strand) {
      const float amp = 2.6f * LV_MIN(1.0f, (yy - tip) / 14);
      const float arg = (yy - f.phase) * 0.42f;
      xc = cx + amp * sinf(arg);
      const float slope = amp * 0.42f * cosf(arg);
      sc = 1 / sqrtf(1 + slope * slope);
      capA = clamp1(end + 0.5f - yy);
    }
    for (int x = 0; x < w; x++) {
      Acc c;
      const float xx = x + 0.5f;
      if (glow) {
        const float d = sqrtf((xx - cx) * (xx - cx) + (yy - tip) * (yy - tip)) / 6;
        if (d < 1) over(c, 0xFFB46E, 0.85f * (1 - d));
      }
      if (strand) {
        const float d = fabsf(xx - xc) * sc;
        over(c, 0x000000, 0.5f * capA * clamp1(2.2f + 0.5f - d));
        over(c, dark, capA * clamp1(1.7f + 0.5f - d));
        over(c, rgb, capA * clamp1(1.15f + 0.5f - d));
        over(c, lit, 0.9f * capA * clamp1(0.4f + 0.5f - fabsf(xx - xc + 0.5f) * sc));
      }
      const float ax = fabsf(xx - cx);
      if (yy >= 1 && yy < 6 && ax < 6) {
        over(c, 0x5C6470, clamp1(6.5f - ax));
        for (float i = -4; i <= 4.1f; i += 2.6f)
          if (fabsf(xx - cx - i) < 0.5f && yy > 1.6f && yy < 5.4f) over(c, 0x2E343D, 1);
      } else if (yy >= 6 && yy < 12 && ax < 8) {
        over(c, mixRgb(0xA7AFBB, 0x4A515C, (yy - 6) / 6), clamp1(8.5f - ax));
      } else if (yy >= 12 && yy < tip) {
        const float hw = 3.5f - 2.3f * (yy - 12) / (tip - 12);
        if (ax < hw + 0.5f) over(c, mixRgb(0xF0D27C, 0x9C7A2C, clamp1((xx - cx + hw) / (2 * hw))), clamp1(hw + 0.5f - ax));
      }
      store(row + x * 4, c, 1);
    }
  }
}

// From applyPrinterToTile: the active tray (has), whether a job feeds from it
// (feeding) and moves (printing). A new colour mid-print plays the change.
void prnFilamentSet(TileRt& r, bool has, uint32_t rgb, bool clear, bool feeding, bool printing) {
  PrnFx* f = r.pfx;
  if (!f) return;
  const bool change = f->has && has && rgb != f->rgb && feeding && dispSettings.printerFx && r.page == sh.active;
  if (change) {
    f->oldRgb = f->rgb;
    f->swapMs = millis() | 1;
  }
  if (has != f->has || rgb != f->rgb || clear != f->clear) f->dirty = true;
  f->nameDirty = true;
  f->has = has;
  f->rgb = rgb;
  f->clear = clear;
  f->feeding = feeding;
  f->printing = printing;
}

void prnFxFit(TileRt& r, int size) {
  if (!r.pfx || !r.pfx->spool) return;
  lv_obj_set_size(r.pfx->spool, size, size);
  r.pfx->dirty = true;
}

bool filamentTick(TileRt& r, PrnFx& f, uint32_t now, bool shown, bool on) {
  const float dt = LV_MIN(0.1f, (now - f.lastMs) / 1000.0f);
  f.lastMs = now;
  // The material drops to the small font when the head column leaves too little room.
  const int nw = lv_obj_get_width(r.sub);
  if (nw > 0 && (nw != f.nameW || f.nameDirty)) {
    f.nameW = nw;
    f.nameDirty = false;
    const bool tall = lv_obj_get_content_height(r.obj) >= 64;
    setFont(r.sub, tall && textW(lv_label_get_text(r.sub), &deck_font_20) <= nw ? &deck_font_20 : &deck_font_14);
    lv_obj_set_height(r.sub, lineH(lv_obj_get_style_text_font(r.sub, LV_PART_MAIN)));
  }
  setShown(r.chip, !on);
  setShown(f.spool, on);
  const bool headOn = on && f.has && (f.feeding || f.swapMs);
  if (headOn == lv_obj_has_flag(f.head, LV_OBJ_FLAG_HIDDEN)) {
    setShown(f.head, headOn);
    lv_obj_set_style_pad_column(r.obj, headOn ? 8 : 12, 0);
    f.dirty = true;
  }
  if (!on) return false;
  if (!shown) {
    f.swapMs = 0;
    f.level = 1;
    f.flow = f.feeding ? 1 : 0;
    f.dirty = true;
    return false;
  }
  float spin = f.printing ? 0.55f : 0, levelTo = f.has ? 1 : 0, flowTo = f.feeding ? 1 : 0;
  bool retract = false;
  uint32_t col = f.rgb;
  if (f.swapMs) {
    const uint32_t t = now - f.swapMs;
    if (t < SWAP_UNWIND) {
      col = f.oldRgb;
      spin = -2.6f;
      retract = true;
      flowTo = 0;
      if (t >= SWAP_RETRACT) levelTo = 0;
    } else if (t < SWAP_OUT) {
      spin = 0;
      levelTo = 0;
      flowTo = 0;
    } else if (t >= SWAP_END) f.swapMs = 0;
  }
  const float lv = f.level, fl = f.flow;
  f.angle = fmodf(f.angle + spin * dt, 6.2832f);
  f.level += (levelTo - f.level) * LV_MIN(1.0f, dt * 7);
  if (fabsf(f.level - levelTo) < 0.002f) f.level = levelTo;
  f.flow += (flowTo - f.flow) * LV_MIN(1.0f, dt * (retract ? 3.2f : 2.6f));
  if (fabsf(f.flow - flowTo) < 0.002f) f.flow = flowTo;
  const float wave = retract ? -30.0f : f.printing || f.flow != fl ? 16.0f : 0;
  f.phase = fmodf(f.phase + wave * dt, 1000.0f);
  bool drew = false;
  const int n = lv_obj_get_width(f.spool);
  if ((spin != 0 || f.level != lv || f.dirty) && fxBuf(f.sbuf, n, n)) {
    spoolPaint(f, col);
    lv_obj_invalidate(f.spool);
    drew = true;
  }
  if (headOn && (wave != 0 || f.flow != fl || f.dirty) && fxBuf(f.hbuf, HEAD_W, lv_obj_get_height(f.head))) {
    headPaint(f, col, f.printing || f.flow > fl);
    lv_obj_invalidate(f.head);
    drew = true;
  }
  f.dirty = false;
  return drew;
}

// Steps every printer effect on the shown page; true when something was drawn.
// Temperature effects step every other frame (about 15 fps).
bool prnFxTick(uint32_t now, bool lit) {
  static bool s_on = true;
  static uint32_t s_frame = 0;
  const bool on = dispSettings.printerFx;
  const bool frame = (s_frame++ & 1) == 0;
  bool drew = false;
  for (TileRt& r : s_tiles) {
    if (r.def->type != TILE_PRINTER || !r.pfx) continue;
    PrnFx& f = *r.pfx;
    const bool shown = lit && r.page == sh.active;
    if (isTempTile(*r.def)) {
      if (on != s_on) tempShow(r, f);  // cooling colours follow the switch
      drew |= tempTick(r, f, now, shown, on, frame);
    } else drew |= filamentTick(r, f, now, shown, on);
  }
  s_on = on;
  return drew;
}

}  // namespace deckui
