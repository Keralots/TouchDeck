// Deck UI: the weather tile. One custom-drawn object lays itself out for the
// room it gets; only the big icon animates.
#include "deck_ui_int.h"
#include "deck_weather.h"

namespace deckui {

constexpr uint32_t WX_FRAME_MS = 40;
constexpr uint32_t WX_STALE_MS = 3UL * 60UL * 60UL * 1000UL;  // older than this is not "now"
constexpr uint32_t C_SUN = 0xFFC533, C_RAIN = 0x4FA3FF, C_BOLT = 0xFFD23F;
constexpr uint32_t C_COLD = 0x5AA9FF, C_WARM = 0xFFB03D;

struct WxRt {
  WeatherData d;
  uint32_t serial;
  uint32_t rain;      // colour of the rain chance
  uint8_t forecast;   // 0 days, 1 hours, 2 none; a tap flips days and hours
  bool flips;         // a forecast is on the tile, so a tap has something to flip
  bool animate;
  bool shown;         // d is fresh enough to show
  lv_area_t icon;     // the animated icon inside the object, empty when x2 < x1
};

static uint32_t s_wxMs = 0;

void wxFree(WxRt* w) {
  if (w) lv_free(w);
}

// --- Icons ---------------------------------------------------------------------

struct Pen {
  lv_layer_t* layer;
  bool light;  // Daylight theme: clouds and snow need to be darker than the tile
};

static void disc(const Pen& p, float cx, float cy, float r, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(color);
  d.bg_opa = opa;
  d.radius = LV_RADIUS_CIRCLE;
  const lv_area_t a = {(int32_t)lroundf(cx - r), (int32_t)lroundf(cy - r), (int32_t)lroundf(cx + r) - 1,
                       (int32_t)lroundf(cy + r) - 1};
  if (a.x2 >= a.x1 && a.y2 >= a.y1) lv_draw_rect(p.layer, &d, &a);
}

static void pill(const Pen& p, float x1, float y1, float x2, float y2, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(color);
  d.bg_opa = opa;
  d.radius = LV_RADIUS_CIRCLE;
  const lv_area_t a = {(int32_t)lroundf(x1), (int32_t)lroundf(y1), (int32_t)lroundf(x2) - 1, (int32_t)lroundf(y2) - 1};
  if (a.x2 >= a.x1 && a.y2 >= a.y1) lv_draw_rect(p.layer, &d, &a);
}

static void stroke(const Pen& p, float x1, float y1, float x2, float y2, float width, uint32_t color,
                   lv_opa_t opa = LV_OPA_COVER) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(color);
  d.opa = opa;
  d.width = std::max(1, (int)lroundf(width));
  d.round_start = d.round_end = 1;
  d.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
  d.p2 = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
  lv_draw_line(p.layer, &d);
}

static void tri(const Pen& p, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color, lv_opa_t opa) {
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.color = lv_color_hex(color);
  d.opa = opa;
  d.p[0] = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
  d.p[1] = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
  d.p[2] = {(lv_value_precise_t)x3, (lv_value_precise_t)y3};
  lv_draw_triangle(p.layer, &d);
}

static void sun(const Pen& p, float cx, float cy, float s, uint32_t t) {
  const float turn = (float)(t % 16000) / 16000.0f * 2.0f * (float)M_PI;
  const float pulse = 0.015f * sinf((float)(t % 3000) / 3000.0f * 2.0f * (float)M_PI);
  const float r1 = s * 0.33f, r2 = s * (0.45f + pulse);
  for (int i = 0; i < 8; i++) {
    const float a = turn + (float)i * (float)M_PI / 4.0f, c = cosf(a), sn = sinf(a);
    stroke(p, cx + c * r1, cy + sn * r1, cx + c * r2, cy + sn * r2, s / 14.0f + 1.0f, C_SUN);
  }
  disc(p, cx, cy, s * 0.24f, C_SUN);
}

// A crescent as an A8 image: LVGL has no way to cut one disc out of another.
// One per size in use; freed with the pages, never while a frame may still draw one.
constexpr int MOONS = 6;
static lv_draw_buf_t* s_moon[MOONS] = {};

void wxMoonFree() {
  for (lv_draw_buf_t*& b : s_moon) {
    if (b) lv_draw_buf_destroy(b);
    b = nullptr;
  }
}

static lv_draw_buf_t* moonImage(int px) {
  int slot = -1;
  for (int i = 0; i < MOONS; i++)
    if (s_moon[i] && (int)s_moon[i]->header.w == px) return s_moon[i];
  for (int i = 0; i < MOONS && slot < 0; i++)
    if (!s_moon[i]) slot = i;
  if (slot < 0) return nullptr;
  lv_draw_buf_t* b = lv_draw_buf_create(px, px, LV_COLOR_FORMAT_A8, 0);
  if (!b) return nullptr;
  const float R = px / 2.0f - 0.5f, c = px / 2.0f;
  const float ix = c + R * 0.42f, iy = c - R * 0.22f, Ri = R * 0.86f;
  for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++) {
      const float fx = x + 0.5f, fy = y + 0.5f;
      const float in = constrain(R - hypotf(fx - c, fy - c) + 0.5f, 0.0f, 1.0f);
      const float out = constrain(hypotf(fx - ix, fy - iy) - Ri + 0.5f, 0.0f, 1.0f);
      b->data[y * b->header.stride + x] = (uint8_t)(255.0f * in * out);
    }
  s_moon[slot] = b;
  return b;
}

static void moon(const Pen& p, float cx, float cy, float s, uint32_t t, bool stars) {
  const int px = std::max(8, (int)lroundf(s * 0.62f));
  const uint32_t color = p.light ? 0x8592AB : 0xE3E8F2;
  if (lv_draw_buf_t* b = moonImage(px)) {
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    d.recolor = lv_color_hex(color);
    d.recolor_opa = LV_OPA_COVER;
    const int x = (int)lroundf(cx - px / 2.0f), y = (int)lroundf(cy - px / 2.0f);
    const lv_area_t a = {x, y, x + px - 1, y + px - 1};
    lv_draw_image(p.layer, &d, &a);
  } else {
    disc(p, cx, cy, px / 2.0f, color);
  }
  if (!stars || s < 30) return;
  static const float at[2][2] = {{0.30f, -0.26f}, {0.36f, 0.20f}};
  for (int i = 0; i < 2; i++) {
    const float ph = (float)((t + i * 900) % 2600) / 2600.0f;
    const float k = 0.5f + 0.5f * sinf(ph * 2.0f * (float)M_PI);
    disc(p, cx + at[i][0] * s, cy + at[i][1] * s, std::max(1.0f, s / 30.0f), color, (lv_opa_t)(90 + 165 * k));
  }
}

// Spans 0.72 sc wide and 0.52 sc high around (cx, cy).
static void cloud(const Pen& p, float cx, float cy, float sc, uint32_t color) {
  pill(p, cx - 0.36f * sc, cy, cx + 0.36f * sc, cy + 0.26f * sc, color);
  disc(p, cx - 0.14f * sc, cy + 0.04f * sc, 0.16f * sc, color);
  disc(p, cx + 0.08f * sc, cy - 0.04f * sc, 0.22f * sc, color);
}

static float fract(float v) { return v - floorf(v); }

// t = 0 and still = true: the icon at rest, for the small forecast icons.
static void wxIcon(const Pen& p, WeatherKind kind, bool day, float cx, float cy, float s, uint32_t t, bool still) {
  const uint32_t front = p.light ? 0xA9B4C4 : 0xDDE3EC, back = p.light ? 0xC6CEDA : 0x8E9AAD;
  const float drift = still ? 0 : 0.03f * s * sinf((float)(t % 7000) / 7000.0f * 2.0f * (float)M_PI);
  auto drops = [&](float top, int n, float spread, bool snow) {
    static const float rest[3] = {0.25f, 0.65f, 0.45f};
    for (int i = 0; i < n; i++) {
      const float ph = still ? rest[i] : fract((float)(t % 36000) / (snow ? 2400.0f : 950.0f) + (float)i * 0.37f);
      const float x = cx + ((float)i - (float)(n - 1) / 2.0f) * spread;
      const float bottom = cy + 0.48f * s, len = snow ? 0 : 0.10f * s;
      const float y = top + ph * (bottom - top - len);
      const lv_opa_t opa = (lv_opa_t)(ph > 0.75f ? 255.0f * (1.0f - ph) / 0.25f : 255);
      if (snow) {
        const float sway = 0.035f * s * sinf(ph * 2.0f * (float)M_PI + (float)i);
        disc(p, x + sway, y, std::max(1.5f, s / 20.0f), p.light ? 0x7FB6E8 : 0xFFFFFF, opa);
      } else {
        stroke(p, x, y, x - 0.04f * s, y + len, s / 18.0f + 1.0f, C_RAIN, opa);
      }
    }
  };
  switch (kind) {
    case WX_CLEAR:
      if (day) sun(p, cx, cy, s, still ? 0 : t);
      else moon(p, cx, cy, s, t, !still);
      break;
    case WX_PARTLY:
      if (day) sun(p, cx - 0.15f * s, cy - 0.14f * s, s * 0.62f, still ? 0 : t);
      else moon(p, cx - 0.15f * s, cy - 0.14f * s, s * 0.8f, t, false);
      cloud(p, cx + 0.07f * s + drift, cy + 0.10f * s, 0.8f * s, front);
      break;
    case WX_CLOUD:
      cloud(p, cx + 0.15f * s - drift, cy - 0.14f * s, 0.6f * s, back);
      cloud(p, cx - 0.04f * s + drift, cy + 0.04f * s, 0.85f * s, front);
      break;
    case WX_FOG: {
      cloud(p, cx, cy - 0.20f * s, 0.75f * s, front);
      static const float half[3] = {0.32f, 0.22f, 0.27f};
      for (int i = 0; i < 3; i++) {
        const float dx = still ? 0 : 0.05f * s * sinf((float)((t + i * 1300) % 5200) / 5200.0f * 2.0f * (float)M_PI);
        const float y = cy + (0.14f + 0.13f * (float)i) * s;
        stroke(p, cx - half[i] * s + dx, y, cx + half[i] * s + dx, y, s / 16.0f + 1.0f, front, LV_OPA_70);
      }
      break;
    }
    case WX_RAIN:
    case WX_SNOW:
      cloud(p, cx + drift, cy - 0.18f * s, 0.85f * s, front);
      drops(cy + 0.10f * s, 3, 0.2f * s, kind == WX_SNOW);
      break;
    case WX_STORM: {
      const uint32_t q = still ? 0 : t % 2600;
      const bool flash = !still && (q < 120 || (q > 220 && q < 300));
      cloud(p, cx, cy - 0.18f * s, 0.85f * s, flash ? front : 0x7C8798);
      drops(cy + 0.10f * s, 2, 0.5f * s, false);
      const lv_opa_t opa = still || flash ? LV_OPA_COVER : (lv_opa_t)170;
      tri(p, cx + 0.08f * s, cy + 0.02f * s, cx - 0.12f * s, cy + 0.27f * s, cx + 0.04f * s, cy + 0.24f * s, C_BOLT, opa);
      tri(p, cx - 0.04f * s, cy + 0.20f * s, cx + 0.14f * s, cy + 0.18f * s, cx - 0.08f * s, cy + 0.48f * s, C_BOLT, opa);
      break;
    }
  }
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

static void still(const Ink& k, WeatherKind kind, bool day, int x, int y, int s) {
  if (!k.layer || s < 8) return;
  const Pen p = {k.layer, s_theme == THEME_DAYLIGHT};
  wxIcon(p, kind, day, (float)(k.x0 + x) + s / 2.0f, (float)(k.y0 + y) + s / 2.0f, (float)s, 0, true);
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
static void columns(const Ink& k, const WxRt& w, int x, int y, int wd, int h) {
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
    still(k, s[i].kind, s[i].day, cx + (cw - icon) / 2, yy, icon);
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
static void rows(const Ink& k, const WxRt& w, int x, int y, int wd, int h) {
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
    still(k, s[i].kind, s[i].day, x + lw + 2, ry + (rh - icon) / 2, icon);
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
    w.icon = {x + (wd - s) / 2, top, x + (wd - s) / 2 + s - 1, top + s - 1};
    degText(d.temp, t, sizeof(t));
    say(k, t, f, x, top + s, wd, LV_TEXT_ALIGN_CENTER);
    if (range) say(k, hl, &deck_font_12, x, top + s + th, wd, LV_TEXT_ALIGN_CENTER, LV_OPA_70);
    return;
  }
  // Wide: icon, then temperature over condition and range, then the extras.
  const int s = constrain(std::min(h, wd * 2 / 5), 28, 124);
  w.icon = {x, y + (h - s) / 2, x + s - 1, y + (h - s) / 2 + s - 1};
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

// Everything on the tile. Without a layer it only works out the layout (icon place, tap).
static void paint(const Ink& k, WxRt& w, const TileDef& def, int wd, int h) {
  w.icon = {0, 0, -1, -1};
  w.flips = false;
  if (!w.shown) {
    const char* text = !weatherPlaceSet() ? "Set a place" : "No data yet";
    const lv_font_t* f = wd >= 110 ? &deck_font_14 : &deck_font_12;
    const int s = constrain(std::min(wd, h - lineH(f)) * 3 / 5, 0, 64);
    const int top = (h - s - lineH(f)) / 2;
    still(k, WX_CLOUD, true, (wd - s) / 2, top, s);
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

static void wxMeasure(TileRt& r) {
  if (!r.wx) return;
  const Ink k = {nullptr, lv_color_white(), 0, 0};
  paint(k, *r.wx, *r.def, lv_obj_get_width(r.viz), lv_obj_get_height(r.viz));
}

void fitWeather(TileRt& r, int cw, int ch) {
  lv_obj_set_size(r.viz, cw, ch);
  lv_obj_set_pos(r.viz, 0, 0);
  wxMeasure(r);
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
  // An animation frame redraws the icon alone.
  const lv_area_t ic = {a.x1 + w.icon.x1, a.y1 + w.icon.y1, a.x1 + w.icon.x2, a.y1 + w.icon.y2};
  const lv_area_t& cl = k.layer->_clip_area;
  const bool iconOnly = w.icon.x2 >= w.icon.x1 && cl.x1 >= ic.x1 && cl.y1 >= ic.y1 && cl.x2 <= ic.x2 && cl.y2 <= ic.y2;
  if (!iconOnly) paint(k, w, *r.def, lv_area_get_width(&a), lv_area_get_height(&a));
  if (w.icon.x2 < w.icon.x1) return;
  const Pen p = {k.layer, s_theme == THEME_DAYLIGHT};
  const float s = (float)lv_area_get_width(&w.icon);
  wxIcon(p, weatherKind(w.d.code), w.d.day, (float)(a.x1 + w.icon.x1) + s / 2.0f, (float)(a.y1 + w.icon.y1) + s / 2.0f,
         s, millis(), !w.animate);
}

// A tap flips the forecast between days and hours.
void wxTap(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size() || !s_tiles[idx].wx || s_ed.root) return;
  TileRt& r = s_tiles[idx];
  if (!r.wx->flips || !r.wx->shown) return;
  r.wx->forecast = r.wx->forecast == 1 ? 0 : 1;
  lv_obj_invalidate(r.viz);
  markPageStale(r.page);
}

static bool wxShowing() {
  return !manualDisplayOff() && currentBacklightLevel() && !s_qp.open && !s_modalCard && !s_ed.root;
}

void wxTick() {
  const uint32_t now = millis();
  if (now - s_wxMs < WX_FRAME_MS) return;
  s_wxMs = now;
  const uint32_t serial = weatherSerial();
  const bool showing = wxShowing();
  bool painted = false;
  for (TileRt& r : s_tiles) {
    WxRt* w = r.wx;
    if (!w || r.off) continue;
    if (serial != w->serial || w->shown != wxFresh(w->d)) {
      weatherGet(w->d);
      w->serial = w->d.serial;
      w->shown = wxFresh(w->d);
      wxMeasure(r);
      lv_obj_invalidate(r.viz);
      markPageStale(r.page);
      continue;
    }
    if (!showing || r.page != sh.active || !w->animate || !w->shown || w->icon.x2 < w->icon.x1) continue;
    lv_area_t a;
    lv_obj_get_coords(r.viz, &a);
    const lv_area_t d = {a.x1 + w->icon.x1, a.y1 + w->icon.y1, a.x1 + w->icon.x2, a.y1 + w->icon.y2};
    lv_obj_invalidate_area(r.viz, &d);
    painted = true;
  }
  if (painted) s_sparkPaintMs = now;  // like a gliding chart: other pages snapshot rarely meanwhile
}

// The place and whether any tile wants the weather, after a config load.
void wxSync() {
  bool wanted = false;
  for (const PageDef& p : s_cfg.pages)
    for (const TileDef& t : p.tiles) wanted |= t.type == TILE_WEATHER;
  weatherConfigure(wanted, s_cfg.weatherLat, s_cfg.weatherLon, s_cfg.weatherImperial);
}

}  // namespace deckui
