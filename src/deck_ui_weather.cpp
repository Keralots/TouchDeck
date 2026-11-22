// Deck UI: the weather tile. One custom-drawn object lays itself out for the
// room it gets; its icons are small images the deck rasterizes itself.
#include "deck_ui_a8.h"
#include "deck_ui_int.h"
#include "deck_weather.h"

namespace deckui {

constexpr uint32_t WX_STALE_MS = 3UL * 60UL * 60UL * 1000UL;  // older than this is not "now"
constexpr uint32_t C_SUN = 0xFFC533, C_RAIN = 0x4FA3FF, C_BOLT = 0xFFD23F;
constexpr uint32_t C_COLD = 0x5AA9FF, C_WARM = 0xFFB03D;
constexpr int WX_SPOTS = 1 + WEATHER_HOURS;  // now, and a forecast's icons

// One icon on the tile: where it is, what it shows, and its image.
struct Spot {
  int16_t x, y, s;
  uint8_t kind;
  bool day;
};

struct WxRt {
  WeatherData d;
  uint32_t serial;
  uint32_t rain;      // colour of the rain chance
  uint8_t forecast;   // 0 days, 1 hours, 2 none; a tap flips days and hours
  bool flips;         // a forecast is on the tile, so a tap has something to flip
  bool animate;
  bool shown;         // d is fresh enough to show
  uint32_t lastMs;    // animation clock of the last animated frame, 0 = none yet
  uint32_t lag;       // time the tile spent hidden: its icons pause there, not jump
  uint8_t nSpots;
  uint8_t at;         // the next spot while painting
  Spot spots[WX_SPOTS];
  lv_draw_buf_t* buf[WX_SPOTS][3];  // A8, spot size, one per colour of the icon
  lv_area_t dirty[WX_SPOTS][3];     // what the last frame drew on, empty when y2 < y1
  uint32_t color[WX_SPOTS][3];
};

void wxFree(WxRt* w) {
  if (!w) return;
  for (auto& spot : w->buf)
    for (lv_draw_buf_t* b : spot)
      if (b) lv_draw_buf_destroy(b);
  lv_free(w);
}

// --- Icons ---------------------------------------------------------------------
// Drawn into one A8 image per colour: a byte a pixel keeps the PSRAM traffic of a frame small.

// A horizontal bar with round ends from ax to bx (a disc when they are equal),
// r thick on each side. Row by row: the inside of a row is one run of whole
// pixels, and only the few pixels on its ends get their coverage worked out.
static void stadium(Cv& c, float ax, float bx, float cy, float r, int opa = 255) {
  const int y1 = std::max(0, ifloor(cy - r - 1)), y2 = std::min(c.h - 1, iceil(cy + r + 1));
  if (y2 < y1) return;
  const float rin = r - 0.5f, rout = r + 0.5f;
  touch(c, std::max(0, ifloor(ax - rout - 1)), y1, std::min(c.w - 1, iceil(bx + rout + 1)), y2);
  auto at = [&](int x, int y, float dy) {
    const float px = (float)x + 0.5f, dx = px < ax ? ax - px : px > bx ? px - bx : 0;
    const float cover = edge(dx * dx + dy * dy, r);
    if (cover > 0) put(c, x, y, std::min(cover, 1.0f), opa);
  };
  for (int y = y1; y <= y2; y++) {
    const float dy = (float)y + 0.5f - cy, o2 = rout * rout - dy * dy;
    if (o2 <= 0) continue;
    const float ho = fsqrt(o2), i2 = rin > 0 ? rin * rin - dy * dy : -1;
    const int xo1 = std::max(0, ifloor(ax - ho - 0.5f)), xo2 = std::min(c.w - 1, iceil(bx + ho - 0.5f));
    int xs = xo2 + 1, xe = xo2;  // the whole pixels, none by default
    if (i2 > 0) {
      const float hi = fsqrt(i2);
      xs = std::max(0, iceil(ax - hi - 0.5f));
      xe = std::min(c.w - 1, ifloor(bx + hi - 0.5f));
      if (xe < xs) xs = xo2 + 1, xe = xo2;
    }
    for (int x = xo1; x < xs && x <= xo2; x++) at(x, y, dy);
    if (xe < xs) continue;
    uint8_t* p = c.px + y * c.stride + xs;
    if (opa >= 255) memset(p, 255, (size_t)(xe - xs + 1));
    else
      for (int x = xs; x <= xe; x++, p++)
        if (opa > *p) *p = (uint8_t)opa;
    for (int x = xe + 1; x <= xo2; x++) at(x, y, dy);
  }
}

static void disc(Cv& c, float cx, float cy, float r, int opa = 255) { stadium(c, cx, cx, cy, r, opa); }

static float wave(uint32_t t, uint32_t period, float phase = 0) {
  return sinf(((float)(t % period) / (float)period + phase) * 2.0f * (float)M_PI);
}

static float fract(float v) { return v - floorf(v); }

static void sun(Cv& c, float cx, float cy, float s, uint32_t t) {
  const float turn = (float)(t % 16000) / 16000.0f * 2.0f * (float)M_PI;
  const float r1 = s * 0.33f, r2 = s * (0.44f + 0.02f * wave(t, 3000));
  for (int i = 0; i < 8; i++) {
    const float a = turn + (float)i * (float)M_PI / 4.0f, co = cosf(a), si = sinf(a);
    capsule(c, cx + co * r1, cy + si * r1, cx + co * r2, cy + si * r2, s / 28.0f + 0.5f);
  }
  disc(c, cx, cy, s * 0.24f);
}

// A crescent: one disc with another cut out of it.
static void moon(Cv& c, float cx, float cy, float s, uint32_t t, bool stars) {
  const float R = s * 0.31f, ix = cx + R * 0.42f, iy = cy - R * 0.22f, Ri = R * 0.86f;
  const int x1 = std::max(0, ifloor(cx - R - 1)), x2 = std::min(c.w - 1, iceil(cx + R + 1));
  const int y1 = std::max(0, ifloor(cy - R - 1)), y2 = std::min(c.h - 1, iceil(cy + R + 1));
  touch(c, x1, y1, x2, y2);
  for (int y = y1; y <= y2; y++)
    for (int x = x1; x <= x2; x++) {
      const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
      const float in = edge((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy), R);
      if (in <= 0) continue;
      const float out = 1.0f - edge((fx - ix) * (fx - ix) + (fy - iy) * (fy - iy), Ri);
      if (out > 0) put(c, x, y, in * out, 255);
    }
  if (!stars || s < 30) return;
  static const float at[2][2] = {{0.30f, -0.26f}, {0.36f, 0.20f}};
  for (int i = 0; i < 2; i++)
    disc(c, cx + at[i][0] * s, cy + at[i][1] * s, std::max(1.0f, s / 30.0f),
         (int)(150.0f + 105.0f * wave(t, 2600, (float)i * 0.35f)));
}

// Spans 0.72 sc wide and 0.52 sc high around (cx, cy).
static void cloud(Cv& c, float cx, float cy, float sc) {
  stadium(c, cx - 0.23f * sc, cx + 0.23f * sc, cy + 0.13f * sc, 0.13f * sc);
  disc(c, cx - 0.14f * sc, cy + 0.04f * sc, 0.16f * sc);
  disc(c, cx + 0.08f * sc, cy - 0.04f * sc, 0.22f * sc);
}

constexpr int WX_LAYERS = 3;

// Images an icon is made of, one per colour, drawn in this order.
static int wxLayers(WeatherKind kind) {
  return kind == WX_CLEAR || kind == WX_FOG ? 1 : kind == WX_STORM ? 3 : 2;
}

// Each layer image is the whole icon. still: at rest, for a tile with the
// animation off. light: the Daylight theme, where clouds and snow go darker.
static void wxIcon(Cv* c, uint32_t* color, WeatherKind kind, bool day, uint32_t t, bool still, bool light) {
  const float s = (float)c[0].w, cx = s / 2.0f, cy = s / 2.0f;
  const uint32_t front = light ? 0xA9B4C4 : 0xDDE3EC, back = light ? 0xC6CEDA : 0x8E9AAD;
  const uint32_t night = light ? 0x8592AB : 0xE3E8F2;
  if (still) t = 0;
  const float drift = still ? 0 : 0.035f * s * wave(t, 7000);
  auto drops = [&](Cv& l, float top, int n, float spread, bool snow) {
    static const float rest[3] = {0.25f, 0.65f, 0.45f};
    for (int i = 0; i < n; i++) {
      const float ph = still ? rest[i] : fract((float)(t % 45600) / (snow ? 2400.0f : 950.0f) + (float)i * 0.37f);
      const float x = cx + ((float)i - (float)(n - 1) / 2.0f) * spread;
      const float bottom = cy + 0.47f * s, len = snow ? 0 : 0.10f * s;
      const float y = top + ph * (bottom - top - len);
      const int opa = ph > 0.75f ? (int)(255.0f * (1.0f - ph) / 0.25f) : ph < 0.1f ? (int)(255.0f * ph / 0.1f) : 255;
      if (snow) disc(l, x + 0.035f * s * sinf(ph * 2.0f * (float)M_PI + (float)i), y, std::max(1.2f, s / 22.0f), opa);
      else capsule(l, x, y, x - 0.04f * s, y + len, s / 40.0f + 0.5f, opa);
    }
  };
  switch (kind) {
    case WX_CLEAR:
      color[0] = day ? C_SUN : night;
      if (day) sun(c[0], cx, cy, s, t);
      else moon(c[0], cx, cy, s, t, !still);
      break;
    case WX_PARTLY:
      color[0] = day ? C_SUN : night;
      color[1] = front;
      if (day) sun(c[0], cx - 0.15f * s, cy - 0.14f * s, s * 0.62f, t);
      else moon(c[0], cx - 0.15f * s, cy - 0.14f * s, s * 0.8f, t, false);
      cloud(c[1], cx + 0.07f * s + drift, cy + 0.10f * s, 0.8f * s);
      break;
    case WX_CLOUD:
      color[0] = back;
      color[1] = front;
      cloud(c[0], cx + 0.15f * s - drift, cy - 0.14f * s, 0.6f * s);
      cloud(c[1], cx - 0.04f * s + drift, cy + 0.04f * s, 0.85f * s);
      break;
    case WX_FOG: {
      color[0] = front;
      cloud(c[0], cx, cy - 0.20f * s, 0.75f * s);
      static const float half[3] = {0.32f, 0.22f, 0.27f};
      for (int i = 0; i < 3; i++) {
        const float dx = still ? 0 : 0.05f * s * wave(t, 5200, (float)i * 0.25f);
        stadium(c[0], cx - half[i] * s + dx, cx + half[i] * s + dx, cy + (0.14f + 0.13f * (float)i) * s,
                s / 32.0f + 0.5f, 180);
      }
      break;
    }
    case WX_RAIN:
    case WX_SNOW:
      color[0] = front;
      color[1] = kind == WX_RAIN ? C_RAIN : light ? 0x7FB6E8 : 0xFFFFFF;
      cloud(c[0], cx + drift, cy - 0.18f * s, 0.85f * s);
      drops(c[1], cy + 0.10f * s, 3, 0.2f * s, kind == WX_SNOW);
      break;
    case WX_STORM: {
      // Two quick flashes every 2.6 s, fading out rather than switching.
      const float q = (float)(t % 2600);
      const float flash = still ? 0 : q < 120 ? 1 : q < 220 ? 0.2f : q < 300 ? 1 : q < 700 ? (700 - q) / 400.0f : 0;
      color[0] = lv_color_to_u32(lv_color_mix(lv_color_hex(front), lv_color_hex(0x7C8798), (uint8_t)(flash * 255))) & 0xFFFFFF;
      color[1] = C_RAIN;
      color[2] = C_BOLT;
      cloud(c[0], cx, cy - 0.18f * s, 0.85f * s);
      drops(c[1], cy + 0.10f * s, 2, 0.5f * s, false);
      const int opa = still ? 255 : 150 + (int)(105.0f * flash);
      const float r = std::max(1.0f, 0.035f * s);
      capsule(c[2], cx + 0.07f * s, cy + 0.04f * s, cx - 0.07f * s, cy + 0.24f * s, r, opa);
      capsule(c[2], cx - 0.07f * s, cy + 0.24f * s, cx + 0.07f * s, cy + 0.22f * s, r, opa);
      capsule(c[2], cx + 0.07f * s, cy + 0.22f * s, cx - 0.05f * s, cy + 0.44f * s, r, opa);
      break;
    }
  }
}

// Draws one spot's images afresh: last frame's pixels are wiped first.
static void spotRender(WxRt& w, int i, uint32_t t) {
  const Spot& p = w.spots[i];
  Cv c[WX_LAYERS];
  const int n = wxLayers((WeatherKind)p.kind);
  for (int l = 0; l < n; l++) {
    lv_draw_buf_t* b = w.buf[i][l];
    if (!b) return;
    lv_area_t& d = w.dirty[i][l];
    for (int y = d.y1; y <= d.y2; y++) memset(b->data + y * b->header.stride + d.x1, 0, (size_t)(d.x2 - d.x1 + 1));
    c[l] = {b->data, (int)b->header.w, (int)b->header.h, (int)b->header.stride, p.s, p.s, -1, -1};
  }
  wxIcon(c, w.color[i], (WeatherKind)p.kind, p.day, t, !w.animate, s_theme == THEME_DAYLIGHT);
  for (int l = 0; l < n; l++) w.dirty[i][l] = {c[l].x1, c[l].y1, c[l].x2, c[l].y2};
}

// --- Layout and paint ------------------------------------------------------------

struct Ink {
  lv_layer_t* layer;  // null = measure only
  lv_color_t text;
  int x0, y0;  // the object's corner on screen
};

static void say(const Ink& k, const char* s, const lv_font_t* f, int x, int y, int w, lv_text_align_t align,
                lv_opa_t opa = LV_OPA_COVER, const lv_color_t* color = nullptr) {
  if (!k.layer || w <= 0) return;
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = s;
  d.text_local = 1;
  d.font = f;
  d.color = color ? *color : k.text;
  d.opa = opa;
  d.align = align;
  const lv_area_t a = {k.x0 + x, k.y0 + y, k.x0 + x + w - 1, k.y0 + y + lineH(f) - 1};
  lv_draw_label(k.layer, &d, &a);
}

static void box(const Ink& k, int x, int y, int w, int h, lv_color_t color, lv_opa_t opa) {
  if (!k.layer || w <= 0 || h <= 0) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = color;
  d.bg_opa = opa;
  d.radius = LV_RADIUS_CIRCLE;
  const lv_area_t a = {k.x0 + x, k.y0 + y, k.x0 + x + w - 1, k.y0 + y + h - 1};
  lv_draw_rect(k.layer, &d, &a);
}

// The box around spots from..to-1 inside the object, empty (x2 < x1) for none.
static lv_area_t spotArea(const WxRt& w, int from, int to) {
  lv_area_t r = {INT16_MAX, INT16_MAX, -1, -1};
  for (int i = from; i < to && i < w.nSpots; i++) {
    const Spot& p = w.spots[i];
    r.x1 = std::min<int32_t>(r.x1, p.x);
    r.y1 = std::min<int32_t>(r.y1, p.y);
    r.x2 = std::max<int32_t>(r.x2, p.x + p.s - 1);
    r.y2 = std::max<int32_t>(r.y2, p.y + p.s - 1);
  }
  return r;
}

static void spotDraw(const Ink& k, const WxRt& w, int i) {
  const Spot& p = w.spots[i];
  const lv_area_t a = {k.x0 + p.x, k.y0 + p.y, k.x0 + p.x + p.s - 1, k.y0 + p.y + p.s - 1};
  for (int l = 0; l < WX_LAYERS; l++) {
    const lv_draw_buf_t* b = w.buf[i][l];
    if (!b || (int)b->header.w != p.s) continue;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    d.recolor = lv_color_hex(w.color[i][l]);
    d.recolor_opa = LV_OPA_COVER;
    lv_draw_image(k.layer, &d, &a);
  }
}

// An icon: measuring books its place, painting draws the image made for it.
static void spot(const Ink& k, WxRt& w, WeatherKind kind, bool day, int x, int y, int s) {
  if (s < 8) return;
  if (k.layer) {
    if (w.at < w.nSpots) spotDraw(k, w, w.at++);
    return;
  }
  if (w.nSpots < WX_SPOTS) w.spots[w.nSpots++] = {(int16_t)x, (int16_t)y, (int16_t)s, (uint8_t)kind, day};
}

static void degText(float v, char* out, size_t n) {
  const long r = lroundf(v);
  snprintf(out, n, "%ld\xC2\xB0", r);
}

// The temperature in the clock font (digits only): sign and degree drawn beside it.
static int bigTemp(const Ink& k, float v, const lv_font_t* f, int x, int y, bool measure) {
  char num[8];
  const long r = lroundf(v);
  snprintf(num, sizeof(num), "%ld", labs(r));
  const bool clock = f == &deck_font_clock_56 || f == &deck_font_clock_72;
  if (!clock) {
    char text[12];
    degText(v, text, sizeof(text));
    const int w = textW(text, f);
    if (!measure) say(k, text, f, x, y, w + 2, LV_TEXT_ALIGN_LEFT);
    return w;
  }
  const int h = lineH(f), dash = r < 0 ? h / 4 : 0, gap = dash ? h / 12 : 0;
  const int nw = textW(num, f), dw = textW("\xC2\xB0", &deck_font_28);
  if (!measure) {
    if (dash) box(k, x, y + h / 2 - h / 24, dash, std::max(3, h / 12), k.text, LV_OPA_COVER);
    say(k, num, f, x + dash + gap, y, nw + 2, LV_TEXT_ALIGN_LEFT);
    say(k, "\xC2\xB0", &deck_font_28, x + dash + gap + nw + 1, y + h / 10, dw + 2, LV_TEXT_ALIGN_LEFT);
  }
  return dash + gap + nw + 1 + dw;
}

static const char* const WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

struct Slot {
  char label[6];
  WeatherKind kind;
  bool day;
  float hi, lo;
  int8_t rain;
  bool hour;
};

// What the forecast shows: the days after today, or hours ahead spread over the room.
static int slots(const WxRt& w, Slot* out, int room, bool minutes = false) {
  const WeatherData& d = w.d;
  int n = 0;
  if (w.forecast == 1) {
    const int have = d.nHours - 1;
    if (have <= 0 || room <= 0) return 0;
    const int step = constrain(have / room, 1, 3);
    for (int i = 1; i < d.nHours && n < room; i += step) {
      const WeatherHour& h = d.hours[i];
      Slot& s = out[n++];
      snprintf(s.label, sizeof(s.label), minutes ? "%02d:00" : "%02d", h.hour);
      s.kind = weatherKind(h.code);
      s.day = h.day;
      s.hi = s.lo = h.temp;
      s.rain = h.rain;
      s.hour = true;
    }
    return n;
  }
  for (int i = 1; i < d.nDays && n < room; i++) {
    const WeatherDay& y = d.days[i];
    Slot& s = out[n++];
    strlcpy(s.label, y.wday >= 0 && y.wday < 7 ? WDAY[y.wday] : "", sizeof(s.label));
    s.kind = weatherKind(y.code);
    s.day = true;
    s.hi = y.hi;
    s.lo = y.lo;
    s.rain = y.rain;
    s.hour = false;
  }
  return n;
}

static void rainText(const Ink& k, const WxRt& w, const Slot& s, const lv_font_t* f, int x, int y, int wd,
                     lv_text_align_t align) {
  if (s.rain < 20) return;
  char t[8];
  snprintf(t, sizeof(t), "%d%%", s.rain);
  const lv_color_t c = lv_color_hex(w.rain);
  say(k, t, f, x, y, wd, align, LV_OPA_COVER, &c);
}

// Forecast as columns: label, icon, temperatures.
static void columns(const Ink& k, WxRt& w, int x, int y, int wd, int h) {
  const bool big = h >= 104 && wd >= 5 * 58;
  const lv_font_t* lf = big ? &deck_font_14 : &deck_font_12;
  const lv_font_t* tf = &deck_font_14;
  const int minW = big ? 58 : 44;
  Slot s[WEATHER_HOURS];
  const int n = slots(w, s, std::min(wd / minW, (int)WEATHER_HOURS));
  if (n <= 0) return;
  const int lh = lineH(lf), th = lineH(tf), sh = lineH(&deck_font_12);
  const bool second = h >= lh + 20 + th + sh + 4;  // low, or the rain chance of an hour
  const int icon = constrain(h - lh - th - (second ? sh : 0) - 4, 12, big ? 44 : 34);
  const int used = lh + icon + th + (second ? sh : 0);
  const int top = y + std::max(0, (h - used) / 2), gap = std::max(0, (h - used) / 8);
  for (int i = 0; i < n; i++) {
    const int cx = x + wd * i / n, cw = wd * (i + 1) / n - wd * i / n;
    int yy = top;
    say(k, s[i].label, lf, cx, yy, cw, LV_TEXT_ALIGN_CENTER, LV_OPA_70);
    yy += lh + gap;
    spot(k, w, s[i].kind, s[i].day, cx + (cw - icon) / 2, yy, icon);
    yy += icon + gap;
    char t[12];
    degText(s[i].hi, t, sizeof(t));
    say(k, t, tf, cx, yy, cw, LV_TEXT_ALIGN_CENTER);
    yy += th;
    if (!second) continue;
    if (s[i].hour) {
      rainText(k, w, s[i], &deck_font_12, cx, yy, cw, LV_TEXT_ALIGN_CENTER);
    } else {
      degText(s[i].lo, t, sizeof(t));
      say(k, t, &deck_font_12, cx, yy, cw, LV_TEXT_ALIGN_CENTER, LV_OPA_60);
    }
  }
}

// Forecast as rows: label, icon, and a bar from the day's low to its high where there is room.
static void rows(const Ink& k, WxRt& w, int x, int y, int wd, int h) {
  Slot s[WEATHER_HOURS];
  const int n = slots(w, s, std::min(h / 22, (int)WEATHER_HOURS), true);
  if (n <= 0) return;
  const int rh = std::min(h / n, 52);
  const bool large = rh >= 44 && wd >= 300;
  const lv_font_t* f = large ? &deck_font_20 : rh >= 28 && wd >= 150 ? &deck_font_14 : &deck_font_12;
  const int fh = lineH(f), lw = textW(s[0].hour ? "00:00" : "Wed", f) + 4, icon = std::min(rh - 2, large ? 38 : 26), tw = textW("-00\xC2\xB0", f) + 2;
  const bool bar = wd >= lw + icon + 2 * tw + 60 && !s[0].hour;
  float lo = s[0].lo, hi = s[0].hi;
  for (int i = 1; i < n; i++) {
    lo = std::min(lo, s[i].lo);
    hi = std::max(hi, s[i].hi);
  }
  const float span = std::max(1.0f, hi - lo);
  const int top = y + (h - rh * n) / 2;
  for (int i = 0; i < n; i++) {
    const int ry = top + i * rh, ty = ry + (rh - fh) / 2;
    say(k, s[i].label, f, x, ty, lw, LV_TEXT_ALIGN_LEFT, LV_OPA_70);
    spot(k, w, s[i].kind, s[i].day, x + lw + 2, ry + (rh - icon) / 2, icon);
    char t[12];
    degText(s[i].hi, t, sizeof(t));
    say(k, t, f, x + wd - tw, ty, tw, LV_TEXT_ALIGN_RIGHT);
    if (s[i].hour) {
      rainText(k, w, s[i], f, x + wd - 2 * tw - 6, ty, tw + 4, LV_TEXT_ALIGN_RIGHT);
      continue;
    }
    degText(s[i].lo, t, sizeof(t));
    if (!bar) {
      if (wd >= lw + icon + 2 * tw + 6) say(k, t, f, x + wd - 2 * tw - 2, ty, tw, LV_TEXT_ALIGN_RIGHT, LV_OPA_60);
      continue;
    }
    const int bx = x + lw + icon + 8 + tw, bw = wd - (lw + icon + 8 + tw) - tw - 6, by = ry + rh / 2 - 3;
    say(k, t, f, bx - tw - 4, ty, tw, LV_TEXT_ALIGN_RIGHT, LV_OPA_60);
    box(k, bx, by, bw, 6, k.text, LV_OPA_20);
    if (!k.layer) continue;
    const int x1 = bx + (int)lroundf((s[i].lo - lo) / span * (float)bw);
    const int x2 = std::max(x1 + 6, bx + (int)lroundf((s[i].hi - lo) / span * (float)bw));
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = lv_color_hex(C_COLD);
    d.bg_grad.dir = LV_GRAD_DIR_HOR;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0] = {lv_color_hex(C_COLD), LV_OPA_COVER, 0};
    d.bg_grad.stops[1] = {lv_color_hex(C_WARM), LV_OPA_COVER, 255};
    const lv_area_t a = {k.x0 + x1, k.y0 + by, k.x0 + std::min(x2, bx + bw) - 1, k.y0 + by + 5};
    lv_draw_rect(k.layer, &d, &a);
  }
}

// Now: the icon with the temperature, and what else fits beside or under it.
static void header(const Ink& k, WxRt& w, const TileDef& def, int x, int y, int wd, int h) {
  const WeatherData& d = w.d;
  char t[24];
  if (wd < 62 || h < 40) {  // just the temperature
    const lv_font_t* f = h >= 34 && wd >= 54 ? &deck_font_28 : h >= 24 ? &deck_font_20 : &deck_font_14;
    degText(d.temp, t, sizeof(t));
    say(k, t, f, x, y + (h - lineH(f)) / 2, wd, LV_TEXT_ALIGN_CENTER);
    return;
  }
  char hl[24], hi[10], lo[10];
  degText(d.nDays ? d.days[0].hi : d.temp, hi, sizeof(hi));
  degText(d.nDays ? d.days[0].lo : d.temp, lo, sizeof(lo));
  snprintf(hl, sizeof(hl), "%s / %s", hi, lo);
  const char* cond = weatherText(d.code);
  if (wd < 132) {  // narrow: icon over the temperature
    const lv_font_t* f = wd >= 84 && h >= 96 ? &deck_font_28 : &deck_font_20;
    const int th = lineH(f), sh = lineH(&deck_font_12);
    const bool range = h >= 44 + th + sh;
    const int s = constrain(std::min(wd - 8, h - th - (range ? sh : 0) - 2), 20, 96);
    const int used = s + th + (range ? sh : 0), top = y + (h - used) / 2;
    spot(k, w, weatherKind(d.code), d.day, x + (wd - s) / 2, top, s);
    degText(d.temp, t, sizeof(t));
    say(k, t, f, x, top + s, wd, LV_TEXT_ALIGN_CENTER);
    if (range) say(k, hl, &deck_font_12, x, top + s + th, wd, LV_TEXT_ALIGN_CENTER, LV_OPA_70);
    return;
  }
  // Wide: icon, then temperature over condition and range, then the extras.
  const int s = constrain(std::min(h, wd * 2 / 5), 28, 124);
  spot(k, w, weatherKind(d.code), d.day, x, y + (h - s) / 2, s);
  const int tx = x + s + std::max(6, s / 8), room = x + wd - tx;
  const lv_font_t* f = h >= 124 && room >= 150 ? &deck_font_clock_72
                       : h >= 78 && room >= 110 ? &deck_font_clock_56 : &deck_font_28;
  const lv_font_t* cf = h >= 92 ? &deck_font_14 : &deck_font_12;
  // The clock fonts hold digits only: nothing hangs under the baseline.
  const int th = f == &deck_font_28 ? lineH(f) : lineH(f) - f->base_line + 3, ch = lineH(cf);
  const String& name = def.noName ? def.label : def.label.length() ? def.label : s_cfg.weatherName;
  const bool place = name.length() > 0 && h >= th + 3 * ch && textW(name.c_str(), cf) <= room;
  const bool two = h >= th + 2 * ch + (place ? ch : 0);  // condition and range on their own lines
  const bool one = h >= th + ch;
  const int used = th + (two ? 2 * ch : one ? ch : 0) + (place ? ch : 0);
  int yy = y + (h - used) / 2;
  if (place) {
    say(k, name.c_str(), cf, tx, yy, room, LV_TEXT_ALIGN_LEFT, LV_OPA_70);
    yy += ch;
  }
  const int tw = bigTemp(k, d.temp, f, tx, yy, !k.layer);
  yy += th;
  if (two) {
    if (textW(cond, cf) <= room) say(k, cond, cf, tx, yy, room, LV_TEXT_ALIGN_LEFT);
    say(k, hl, cf, tx, yy + ch, room, LV_TEXT_ALIGN_LEFT, LV_OPA_70);
  } else if (one) {
    const bool fits = textW(cond, cf) <= room;
    say(k, fits ? cond : hl, cf, tx, yy, room, LV_TEXT_ALIGN_LEFT, fits ? LV_OPA_COVER : LV_OPA_70);
  }
  // Extras on the right: feels like, humidity, wind.
  const lv_font_t* ef = h >= 92 ? &deck_font_14 : &deck_font_12;
  const int eh = lineH(ef), textEnd = tx + std::max(tw, std::max(textW(cond, cf), textW(hl, cf)));
  char e[3][24];
  char deg[10];
  degText(d.feels, deg, sizeof(deg));
  snprintf(e[0], sizeof(e[0]), "Feels %s", deg);
  snprintf(e[1], sizeof(e[1]), "Humidity %d%%", d.humidity);
  snprintf(e[2], sizeof(e[2]), "Wind %ld %s", lroundf(d.wind), s_cfg.weatherImperial ? "mph" : "km/h");
  int ew = 0;
  for (int i = 0; i < 3; i++) ew = std::max(ew, textW(e[i], ef));
  if (x + wd - ew - 14 < textEnd || h < 3 * eh) return;
  const int ey = y + (h - 3 * eh) / 2;
  for (int i = 0; i < 3; i++) say(k, e[i], ef, x + wd - ew - 2, ey + i * eh, ew + 2, LV_TEXT_ALIGN_RIGHT, LV_OPA_70);
}

// Everything on the tile. Without a layer it only works out the layout (icon places, tap).
static void paint(const Ink& k, WxRt& w, const TileDef& def, int wd, int h) {
  w.flips = false;
  if (!w.shown) {
    const char* text = !weatherPlaceSet() ? "Set a place" : "No data yet";
    const lv_font_t* f = wd >= 110 ? &deck_font_14 : &deck_font_12;
    const int s = constrain(std::min(wd, h - lineH(f)) * 3 / 5, 0, 64);
    const int top = (h - s - lineH(f)) / 2;
    spot(k, w, WX_CLOUD, true, (wd - s) / 2, top, s);
    if (textW(text, f) <= wd) say(k, text, f, 0, top + s, wd, LV_TEXT_ALIGN_CENTER, LV_OPA_70);
    return;
  }
  const bool want = w.forecast != 2;
  if (want && h >= 150 && wd >= 84) {  // forecast under now
    // Rows when the tile is narrow, or tall enough to give every day a line.
    const bool asRows = wd < 250 || h >= 330;
    const int hh = asRows ? constrain(h * 2 / 5, 70, 190) : constrain(h / 2, 74, h - 78);
    header(k, w, def, 0, 0, wd, hh);
    box(k, 0, hh + 3, wd, 1, k.text, LV_OPA_20);
    if (asRows) rows(k, w, 0, hh + 8, wd, h - hh - 8);
    else columns(k, w, 0, hh + 8, wd, h - hh - 8);
    w.flips = true;
  } else if (want && wd >= 300 && h >= 56) {  // forecast beside now
    const int hw = constrain(wd * 2 / 5, 132, 260);
    header(k, w, def, 0, 0, hw, h);
    box(k, hw + 5, 4, 1, h - 8, k.text, LV_OPA_20);
    columns(k, w, hw + 12, 0, wd - hw - 12, h);
    w.flips = true;
  } else {
    header(k, w, def, 0, 0, wd, h);
  }
}

// --- Tile ------------------------------------------------------------------------

static bool wxFresh(const WeatherData& d) { return d.valid && millis() - d.fetchedMs < WX_STALE_MS; }

void makeWeatherContent(TileRt& r, lv_obj_t* t) {
  r.wx = (WxRt*)lv_malloc_zeroed(sizeof(WxRt));
  if (r.wx) {
    r.wx->forecast = r.def->wxForecast;
    r.wx->animate = !r.def->wxStill;
    r.wx->rain = r.def->hasColor ? r.def->color : C_RAIN;
    weatherGet(r.wx->d);
    r.wx->serial = r.wx->d.serial;
    r.wx->shown = wxFresh(r.wx->d);
  }
  r.viz = lv_obj_create(t);
  lv_obj_remove_style_all(r.viz);
  lv_obj_remove_flag(r.viz, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

// Works out the layout and gives every icon an image of its size. Never
// called from a draw event: a frame in progress may still use the old images.
// Takes the size from fitWeather: right after lv_obj_set_size the coords are stale.
static void wxMeasure(TileRt& r, int wd, int h) {
  if (!r.wx) return;
  WxRt& w = *r.wx;
  const Ink k = {nullptr, lv_color_white(), 0, 0};
  w.nSpots = 0;
  paint(k, w, *r.def, wd, h);
  const uint32_t now = w.lastMs ? w.lastMs - w.lag : millis();
  for (int i = 0; i < WX_SPOTS; i++) {
    const int s = i < w.nSpots ? w.spots[i].s : 0;
    const int layers = s ? wxLayers((WeatherKind)w.spots[i].kind) : 0;
    for (int l = 0; l < WX_LAYERS; l++) {
      lv_draw_buf_t*& b = w.buf[i][l];
      if (b && (l >= layers || (int)b->header.w != s)) {
        lv_draw_buf_destroy(b);
        b = nullptr;
      }
      if (l < layers && !b) {
        b = lv_draw_buf_create(s, s, LV_COLOR_FORMAT_A8, 0);
        if (b) w.dirty[i][l] = {0, 0, s - 1, s - 1};  // new memory: wipe all of it
      }
    }
    if (s) spotRender(w, i, now);
  }
}

void fitWeather(TileRt& r, int cw, int ch) {
  lv_obj_set_size(r.viz, cw, ch);
  lv_obj_set_pos(r.viz, 0, 0);
  wxMeasure(r, cw, ch);
}

void wxDrawEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size() || !s_tiles[idx].wx) return;
  TileRt& r = s_tiles[idx];
  WxRt& w = *r.wx;
  lv_obj_t* o = lv_event_get_target_obj(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const Ink k = {lv_event_get_layer(e), lv_obj_get_style_text_color(o, LV_PART_MAIN), a.x1, a.y1};
  // An animation frame redraws the icon of now, or the strip the forecast's
  // icons stand in: nothing else is in either.
  const lv_area_t& cl = k.layer->_clip_area;
  auto within = [&](const lv_area_t& r) {
    return r.x2 >= r.x1 && cl.x1 >= a.x1 + r.x1 && cl.y1 >= a.y1 + r.y1 && cl.x2 <= a.x1 + r.x2 && cl.y2 <= a.y1 + r.y2;
  };
  if (w.nSpots && within(spotArea(w, 0, 1))) {
    spotDraw(k, w, 0);
    return;
  }
  if (within(spotArea(w, 1, w.nSpots))) {
    for (int i = 1; i < w.nSpots; i++) spotDraw(k, w, i);
    return;
  }
  w.at = 0;
  paint(k, w, *r.def, lv_area_get_width(&a), lv_area_get_height(&a));
}

// A tap flips the forecast between days and hours.
void wxTap(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size() || !s_tiles[idx].wx || s_ed.root) return;
  TileRt& r = s_tiles[idx];
  if (!r.wx->flips || !r.wx->shown) return;
  r.wx->forecast = r.wx->forecast == 1 ? 0 : 1;
  wxMeasure(r, lv_obj_get_width(r.viz), lv_obj_get_height(r.viz));
  lv_obj_invalidate(r.viz);
  markPageStale(r.page);
}

void wxTick() {
  if (!(advSettings.weather60 ? s_animFastDue : s_animDue)) return;
  const uint32_t now = s_animMs;
  const uint32_t serial = weatherSerial();
  const bool showing = tilesShowing();
  bool painted = false;
  for (TileRt& r : s_tiles) {
    WxRt* w = r.wx;
    if (!w || r.off) continue;
    if (serial != w->serial || w->shown != wxFresh(w->d)) {
      weatherGet(w->d);
      w->serial = w->d.serial;
      w->shown = wxFresh(w->d);
      wxMeasure(r, lv_obj_get_width(r.viz), lv_obj_get_height(r.viz));
      lv_obj_invalidate(r.viz);
      markPageStale(r.page);
      continue;
    }
    if (!showing || r.page != sh.active || !w->animate || !w->shown) continue;
    lv_area_t a;
    lv_obj_get_coords(r.viz, &a);
    if (w->lastMs && now - w->lastMs > 200) w->lag += now - w->lastMs - SPARK_FRAME_MS;
    w->lastMs = now;
    for (int i = 0; i < w->nSpots; i++) spotRender(*w, i, now - w->lag);
    // Two areas a tile at most: every area LVGL redraws has a cost of its own.
    for (int part = 0; part < 2; part++) {
      const lv_area_t s = part ? spotArea(*w, 1, w->nSpots) : spotArea(*w, 0, 1);
      if (s.x2 < s.x1) continue;
      const lv_area_t d = {a.x1 + s.x1, a.y1 + s.y1, a.x1 + s.x2, a.y1 + s.y2};
      lv_obj_invalidate_area(r.viz, &d);
      painted = true;
    }
  }
  if (!painted) return;
  s_sparkPaintMs = millis();  // like a gliding chart: other pages snapshot rarely meanwhile
  s_animPainted = true;
  danim(DA_WX, (int32_t)now, advSettings.weather60 ? 2 : 1);
}

// The place and whether any tile wants the weather, after a config load.
void wxSync() {
  bool wanted = false;
  for (const PageDef& p : s_cfg.pages)
    for (const TileDef& t : p.tiles) wanted |= t.type == TILE_WEATHER;
  weatherConfigure(wanted, s_cfg.weatherLat, s_cfg.weatherLon, s_cfg.weatherImperial);
}

}  // namespace deckui
