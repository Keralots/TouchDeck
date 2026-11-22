// Deck UI: the Breakout and Rocks clock styles. Breakout: a ball knocks the
// changing digit apart before its pieces fly in as the new one. Rocks: an
// outline ship shoots the changing digit. Motion is scaled from a 128x64
// panel (s) to the tile; only what moves is redrawn.
#include <esp_random.h>
#include <math.h>

#include "deck_ui_int.h"

namespace deckui {

static const uint8_t GLYPH[10][7] = {{14, 17, 17, 17, 17, 17, 14}, {4, 12, 4, 4, 4, 4, 14},  {14, 17, 1, 2, 4, 8, 31},
                                     {14, 17, 1, 6, 1, 17, 14},    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
                                     {6, 8, 16, 22, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},   {14, 17, 17, 14, 17, 17, 14},
                                     {14, 17, 17, 15, 1, 2, 12}};
constexpr uint32_t C_CYAN = 0x00FFFF, C_ROCK = 0xC9A27A, C_BALL = 0xFFFFFF;
constexpr float TAU = 6.2831853f;
constexpr int MAX_FRAGS = 64, MAX_ROCKS = 8, MAX_SHARDS = 20;

static float rnd(float a, float b) { return a + (b - a) * (float)(esp_random() % 10000) / 10000.0f; }
static bool coin() { return esp_random() & 1; }

struct Frag {
  bool on;
  int8_t home;  // assembling into digit home, -1 = flying debris
  float x, y, vx, vy, tx, ty;
};
struct Ball {
  bool on;
  float x, y, vx, vy;
};
struct Trans {
  uint8_t state;  // 0 none, 1 breaking, 2 assembling
  char from, to;
  uint8_t hits;
  uint32_t at;
};
struct Rock {
  bool on, big;
  float x, y, vx, vy, angle, spin;
  float vr[7];
};
struct Shard {
  bool on, digit;
  float x, y, vx, vy, a, spin, life;
};

struct ArcadeRt {
  uint8_t style;  // CLOCK_BREAKOUT or CLOCK_ROCKS
  // Layout.
  int16_t w, h;
  float s;
  const lv_font_t* f;
  int16_t cell, colonW, inkTop, inkH, colonDy, top, xs[5];
  char shown[6];
  bool colonLit;
  uint32_t lastMs;
  int32_t triggered;
  // Digit springs (Breakout) and drop bounce (Rocks), px and px per 50 ms frame.
  float offX[5], offY[5], velX[5], velY[5];
  // Breakout.
  Ball balls[2];
  float padX, padW, padH, padY;
  Trans tr[5];
  Frag frags[MAX_FRAGS];
  // Rocks.
  float sx, sy, svx, svy, sh, thrust, idle, turnTo, shot, respawn, aim;
  bool turning;
  Rock rocks[MAX_ROCKS];
  bool bOn;
  int8_t bRock;
  float bx, by, bvx, bvy, bLife;
  Shard shards[MAX_SHARDS];
  uint8_t phase;  // 0 idle, 1 aim, 2 fire, 3 shatter
  uint8_t qIdx[4], qN, qi, refire;
  char qVal[4];
  int8_t hidden;
};

enum { P_IDLE, P_AIM, P_FIRE, P_SHATTER };

ArcadeRt* arcadeNew(uint8_t style) {
  ArcadeRt* a = (ArcadeRt*)lv_malloc_zeroed(sizeof(ArcadeRt));
  if (!a) return nullptr;
  a->style = style;
  memcpy(a->shown, "  :  ", 6);
  a->colonLit = true;
  a->triggered = -1;
  a->hidden = -1;
  return a;
}

void arcadeFree(ArcadeRt* a) {
  if (a) lv_free(a);
}

// The digit's ink box, object coordinates.
static void digitBox(const ArcadeRt& a, int i, float& x1, float& y1, float& x2, float& y2) {
  x1 = a.xs[i] + a.cell * 0.08f;
  x2 = a.xs[i] + a.cell * 0.92f;
  y1 = a.top;
  y2 = a.top + a.inkH;
}

// Centres of the lit cells of digit v's 5x7 glyph laid over digit slot i.
static int glyphPoints(const ArcadeRt& a, int i, char v, float* px, float* py) {
  if (v < '0' || v > '9') return 0;
  float x1, y1, x2, y2;
  digitBox(a, i, x1, y1, x2, y2);
  const float gx = (x2 - x1) / 5, gy = (y2 - y1) / 7;
  int n = 0;
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 5; c++)
      if ((GLYPH[v - '0'][r] >> (4 - c)) & 1) px[n] = x1 + (c + 0.5f) * gx, py[n] = y1 + (r + 0.5f) * gy, n++;
  return n;
}

static void newBall(ArcadeRt& a, Ball& b) {
  const float v = 70 * a.s;
  b = {true, a.padX, a.padY - 4 * a.s, coin() ? v : -v, -v};
}

static void rockShape(Rock& r, float base) {
  for (float& v : r.vr) v = LV_MAX(2.0f, base * rnd(0.7f, 1.3f));
}

static void spawnRock(ArcadeRt& a, bool inside) {
  for (Rock& r : a.rocks) {
    if (r.on) continue;
    r = {};
    r.on = r.big = true;
    const int e = esp_random() % 4;
    r.x = inside ? rnd(0, a.w) : e == 0 ? -8 * a.s : e == 1 ? a.w + 8 * a.s : rnd(8, a.w - 8);
    r.y = inside ? rnd(0, a.h) : e == 2 ? -8 * a.s : e == 3 ? a.h + 8 * a.s : rnd(8, a.h - 8);
    const float ang = atan2f(a.h / 2.0f - r.y, a.w / 2.0f - r.x) + rnd(-0.6f, 0.6f), v = 8 * a.s;
    r.vx = cosf(ang) * v;
    r.vy = sinf(ang) * v;
    r.angle = rnd(0, TAU);
    r.spin = rnd(0.5f, 1.5f) * (coin() ? 1 : -1);
    rockShape(r, 7 * a.s);
    return;
  }
}

// False when the tile is too small (the caller shows Roll instead).
bool arcadeFit(ArcadeRt* a, int w, int h, int pad) {
  static const lv_font_t* const kTime[] = {&deck_font_clock_72, &deck_font_clock_56, &lv_font_montserrat_40,
                                           &deck_font_28, &deck_font_20, &deck_font_14};
  if ((w - 2 * pad) / 29 < 5) return false;  // 1x1
  const lv_font_t* f = nullptr;
  int top = 0, ink = 0, cell = 0;
  for (const lv_font_t* k : kTime) {
    int t, hh;
    digitInk(k, t, hh);
    int cw = 0;
    for (char d = '0'; d <= '9'; d++) cw = LV_MAX(cw, (int)lv_font_get_glyph_width(k, d, 0));
    if (hh > h * 30 / 100 || 4 * cw + (int)lv_font_get_glyph_width(k, ':', 0) > w * 86 / 100) continue;
    f = k, top = t, ink = hh, cell = cw;
    break;
  }
  if (!f) return false;
  const bool same = a->w == w && a->h == h;
  a->w = (int16_t)w;
  a->h = (int16_t)h;
  a->s = LV_MIN(w / 128.0f, h / 64.0f) * 0.9f + w / 128.0f * 0.1f;
  a->f = f;
  a->cell = (int16_t)cell;
  a->colonW = (int16_t)lv_font_get_glyph_width(f, ':', 0);
  a->inkTop = (int16_t)top;
  a->inkH = (int16_t)ink;
  lv_font_glyph_dsc_t dg = {}, dc = {};
  a->colonDy = 0;
  if (lv_font_get_glyph_dsc(f, &dg, '0', 0) && lv_font_get_glyph_dsc(f, &dc, ':', 0))
    a->colonDy = (int16_t)((dc.ofs_y * 2 + dc.box_h - dg.ofs_y * 2 - dg.box_h) / 2);
  a->top = (int16_t)(a->style == CLOCK_BREAKOUT ? h * 16 / 100 : (h - ink) / 2);
  const int tw = 4 * cell + a->colonW, x0 = (w - tw) / 2;
  a->xs[0] = (int16_t)x0;
  a->xs[1] = (int16_t)(x0 + cell);
  a->xs[2] = (int16_t)(x0 + 2 * cell);
  a->xs[3] = (int16_t)(x0 + 2 * cell + a->colonW);
  a->xs[4] = (int16_t)(x0 + 3 * cell + a->colonW);
  if (same) return true;
  const float s = a->s;
  if (a->style == CLOCK_BREAKOUT) {
    a->padW = LV_MAX(24.0f, 22 * s);
    a->padH = LV_MAX(3.0f, 2 * s);
    a->padY = h - LV_MAX(6.0f, 4 * s);
    a->padX = w / 2.0f;
    newBall(*a, a->balls[0]);
    a->balls[1].on = false;
  } else {
    memset(a->rocks, 0, sizeof(a->rocks));
    a->sx = w * 0.25f;
    a->sy = h * 0.75f;
    a->svx = a->svy = a->sh = 0;
    a->idle = 1;
    a->shot = 5;
    for (int k = 0; k < 2; k++) spawnRock(*a, true);
  }
  return true;
}

// --- Breakout ----------------------------------------------------------------------

static void addFrag(ArcadeRt& a, const Frag& f) {
  for (Frag& q : a.frags)
    if (!q.on) {
      q = f;
      q.on = true;
      return;
    }
}

static void stepBreakout(ArcadeRt& a, float dt, uint32_t now) {
  const float s = a.s;
  int breaking = -1;
  for (int i = 0; i < 5; i++)
    if (a.tr[i].state == 1) breaking = i;
  const float speed = 70 * s * (breaking >= 0 ? 1.35f : 1);
  if (breaking >= 0 && !a.balls[1].on) newBall(a, a.balls[1]);
  if (breaking < 0) a.balls[1].on = false;
  // The paddle follows the nearest falling ball, faster when far away.
  const Ball* near = &a.balls[0];
  for (const Ball& b : a.balls)
    if (b.on && b.vy > 0 && (near->vy <= 0 || b.y > near->y)) near = &b;
  const float d = near->x - a.padX, ad = fabsf(d);
  const float sp = (ad > 20 * s ? 5 : ad > 10 * s ? 4 : ad > 3 * s ? 3 : 2) * s * 60;
  a.padX += (d > 0 ? 1 : -1) * LV_MIN(ad, sp * dt);
  a.padX = LV_CLAMP(a.padW / 2, a.padX, a.w - a.padW / 2);
  const float r = LV_MAX(1.5f, s);
  for (Ball& b : a.balls) {
    if (!b.on) continue;
    const float m = sqrtf(b.vx * b.vx + b.vy * b.vy);
    if (m > 0) b.vx = b.vx / m * speed, b.vy = b.vy / m * speed;
    if (fabsf(b.vy) < speed * 0.35f) b.vy = (b.vy < 0 ? -1 : 1) * speed * 0.35f;
    b.x += b.vx * dt;
    b.y += b.vy * dt;
    if (b.x < r + 2) b.x = r + 2, b.vx = fabsf(b.vx);
    if (b.x > a.w - r - 2) b.x = a.w - r - 2, b.vx = -fabsf(b.vx);
    if (b.y < r + 2) b.y = r + 2, b.vy = fabsf(b.vy);
    if (b.vy > 0 && b.y + r >= a.padY && b.y < a.padY + a.padH && fabsf(b.x - a.padX) <= a.padW / 2 + r) {
      b.y = a.padY - r;
      b.vy = -fabsf(b.vy);
      b.vx += (b.x - a.padX) / (a.padW / 2) * speed * 0.5f;
    }
    if (b.y > a.h + 10) newBall(a, b);
    for (int i : {0, 1, 3, 4}) {
      Trans& t = a.tr[i];
      if (t.state == 2) continue;
      float x1, y1, x2, y2;
      digitBox(a, i, x1, y1, x2, y2);
      if (b.x + r < x1 || b.x - r > x2 || b.y + r < y1 || b.y - r > y2) continue;
      const float cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
      if (t.state == 1 && t.hits < 3) {  // each hit knocks off part of the old digit
        static const float SHARE[3] = {0.25f, 0.5f, 0.25f};
        const float share = SHARE[t.hits++];
        float px[35], py[35];
        const int n = glyphPoints(a, i, t.from, px, py);
        for (int k = 0; k < n; k++)
          for (int dup = 0; dup < 2; dup++)
            if (rnd(0, 1) < share) {
              const float ang = atan2f(py[k] - cy, px[k] - cx) + rnd(-0.3f, 0.3f), v = (1.2f + rnd(-0.5f, 0.5f)) * 62.5f * s;
              addFrag(a, {true, -1, px[k] + rnd(-1, 1) * s, py[k] + rnd(-1, 1) * s, cosf(ang) * v, sinf(ang) * v - 30 * s, 0, 0});
            }
      }
      // Knock the digit away from the ball; a spring brings it back.
      if (fabsf(b.x - cx) > 4 * s) a.velX[i] = (b.x < cx ? 3 : -3) * s;
      else a.velY[i] = (b.y < cy ? 3 : -3) * s;
      const float vary = rnd(-3, 3) / 16 * speed;
      if (fabsf(b.x - cx) / (x2 - x1) > fabsf(b.y - cy) / (y2 - y1)) {
        b.vx = b.x < cx ? -fabsf(b.vx) : fabsf(b.vx);
        b.vy += vary;
        b.x = b.x < cx ? x1 - r - 1 : x2 + r + 1;
      } else {
        b.vy = b.y < cy ? -fabsf(b.vy) : fabsf(b.vy);
        b.vx += vary;
        b.y = b.y < cy ? y1 - r - 1 : y2 + r + 1;
      }
      break;
    }
    // A ball between the two hour or minute digits pushes them apart.
    for (int l : {0, 3}) {
      float lx1, ly1, lx2, ly2, rx1, ry1, rx2, ry2;
      digitBox(a, l, lx1, ly1, lx2, ly2);
      digitBox(a, l + 1, rx1, ry1, rx2, ry2);
      if (b.y + r < ly1 || b.y - r > ly2 || b.x <= lx2 || b.x >= rx1) continue;
      a.velX[l] = -3 * s;
      a.velX[l + 1] = 3 * s;
      b.vx = -b.vx;
      b.x = b.vx > 0 ? rx1 + r + 1 : lx2 - r - 1;
    }
  }
  for (int i : {0, 1, 3, 4}) {
    Trans& t = a.tr[i];
    if (!t.state) continue;
    const uint32_t el = now - t.at;
    if (t.state == 1 && (t.hits >= 3 || el > 3000)) {
      t.state = 2;
      t.at = now;
      float px[35], py[35];
      const int n = glyphPoints(a, i, t.to, px, py);
      for (int k = 0; k < n; k++)
        for (int dup = 0; dup < 2; dup++) {
          if (!coin()) continue;
          const int side = esp_random() % 4;
          const float x = side == 0 || side == 2 ? rnd(0, a.w) : side == 1 ? a.w + 5 : -5.0f;
          const float y = side == 1 || side == 3 ? rnd(0, a.h) : side == 0 ? -5.0f : a.h + 5;
          addFrag(a, {true, (int8_t)i, x, y, 0, 0, px[k] + (dup - 0.5f) * s, py[k] + (dup - 0.5f) * s});
        }
    } else if (t.state == 2 && el > 800) {
      a.shown[i] = t.to;
      t.state = 0;
      for (Frag& q : a.frags)
        if (q.on && q.home == i) q.on = false;
    }
  }
  const float g = 0.048f * 62.5f * 62.5f * s, v = 1.2f * 3 * 62.5f * s;
  for (Frag& q : a.frags) {
    if (!q.on) continue;
    if (q.home >= 0) {
      const float dx = q.tx - q.x, dy = q.ty - q.y, dd = sqrtf(dx * dx + dy * dy);
      if (dd < v * dt) q.x = q.tx, q.y = q.ty;
      else q.x += dx / dd * v * dt, q.y += dy / dd * v * dt;
    } else {
      q.vy += g * dt;
      q.x += q.vx * dt;
      q.y += q.vy * dt;
      if (q.y > a.h + 6) q.on = false;
    }
  }
  // Springs: strength 0.3, damping 0.85 per 50 ms frame, at most 4 panel px.
  const float ps = dt / 0.05f, lim = 4 * s;
  for (int i = 0; i < 5; i++) {
    float* o[2] = {&a.offX[i], &a.offY[i]};
    float* vv[2] = {&a.velX[i], &a.velY[i]};
    for (int k = 0; k < 2; k++) {
      if (*o[k] == 0 && *vv[k] == 0) continue;
      *vv[k] += -*o[k] * 0.3f * ps;
      *vv[k] *= powf(0.85f, ps);
      *o[k] = LV_CLAMP(-lim, *o[k] + *vv[k] * ps, lim);
      if (fabsf(*o[k]) < 0.1f && fabsf(*vv[k]) < 0.1f) *o[k] = *vv[k] = 0;
    }
  }
}

// --- Rocks ---------------------------------------------------------------------------

static float angDiff(float a, float b) {
  float d = b - a;
  while (d > TAU / 2) d -= TAU;
  while (d < -TAU / 2) d += TAU;
  return d;
}

static void wrap(const ArcadeRt& a, float& x, float& y, float m) {
  if (x < -m) x = a.w + m;
  if (x > a.w + m) x = -m;
  if (y < -m) y = a.h + m;
  if (y > a.h + m) y = -m;
}

static void addShard(ArcadeRt& a, float x, float y, float vx, float vy, float life, bool digit) {
  for (Shard& q : a.shards)
    if (!q.on) {
      q = {true, digit, x, y, vx, vy, rnd(0, TAU), rnd(-4, 4), life};
      return;
    }
}

static void burst(ArcadeRt& a, float x, float y, int n, float k) {
  for (int i = 0; i < n; i++) {
    const float ang = rnd(0, TAU), v = rnd(15, 45) * k * a.s;
    addShard(a, x, y, cosf(ang) * v, sinf(ang) * v, 0.9f * rnd(0.6f, 1), false);
  }
}

static void hitRock(ArcadeRt& a, Rock& r) {
  if (!r.big) {
    r.on = false;
    burst(a, r.x, r.y, 5, 1);
    return;
  }
  r.big = false;
  rockShape(r, 4 * a.s);
  const float ang = atan2f(r.vy, r.vx) + TAU / 4, kx = cosf(ang) * 8 * a.s, ky = sinf(ang) * 8 * a.s;
  for (Rock& c : a.rocks)
    if (!c.on) {
      c = r;
      c.vx = r.vx - kx;
      c.vy = r.vy - ky;
      c.spin = -r.spin;
      rockShape(c, 4 * a.s);
      break;
    }
  r.vx += kx;
  r.vy += ky;
  burst(a, r.x, r.y, 3, 0.6f);
}

static void fire(ArcadeRt& a, float tx, float ty, int rock) {
  const float ang = atan2f(ty - a.sy, tx - a.sx), v = 140 * a.s;
  a.bOn = true;
  a.bx = a.sx + cosf(a.sh) * 6 * a.s;
  a.by = a.sy + sinf(a.sh) * 6 * a.s;
  a.bvx = cosf(ang) * v;
  a.bvy = sinf(ang) * v;
  a.bLife = 1.5f;
  a.bRock = (int8_t)rock;
}

static void shatter(ArcadeRt& a, int i) {
  float x1, y1, x2, y2, px[35], py[35];
  digitBox(a, i, x1, y1, x2, y2);
  const float cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
  const int n = glyphPoints(a, i, a.shown[i], px, py), m = LV_MIN(10, n);
  for (int k = 0; k < m; k++) {
    const int src = k * n / m;
    const float ang = atan2f(py[src] - cy, px[src] - cx) + rnd(-0.5f, 0.5f), v = rnd(20, 50) * a.s;
    addShard(a, px[src], py[src], cosf(ang) * v, sinf(ang) * v, 0.9f * rnd(0.7f, 1), true);
  }
  a.hidden = (int8_t)i;
  a.phase = P_SHATTER;
}

static void stepRocks(ArcadeRt& a, float dt, uint32_t now) {
  const float s = a.s;
  if (a.phase == P_IDLE) {
    a.idle -= dt;
    a.shot -= dt;
    if (a.turning) {
      const float d = angDiff(a.sh, a.turnTo), st = 2.5f * dt;
      if (fabsf(d) <= st) a.sh = a.turnTo, a.turning = false;
      else a.sh += d > 0 ? st : -st;
    }
    if (a.thrust > 0) a.thrust -= dt;
    if (a.idle <= 0) {
      a.idle = rnd(1.2f, 3);
      const float act = rnd(0, 1);
      if (act < 0.4f) a.turnTo = rnd(0, TAU), a.turning = true;
      else if (act < 0.75f) a.thrust = rnd(0.3f, 0.7f);
    }
    if (a.shot <= 0 && !a.bOn) {  // an occasional potshot, big rocks first
      a.shot = rnd(6, 14);
      int best = -1;
      for (int i = 0; i < MAX_ROCKS; i++) {
        const Rock& r = a.rocks[i];
        if (!r.on || r.x < 4 || r.x > a.w - 4 || r.y < 4 || r.y > a.h - 4) continue;
        if (best < 0 || (r.big && !a.rocks[best].big)) best = i;
      }
      if (best >= 0) {
        a.sh = atan2f(a.rocks[best].y - a.sy, a.rocks[best].x - a.sx);
        a.turning = false;
        fire(a, a.rocks[best].x, a.rocks[best].y, best);
      }
    }
  } else if (a.phase == P_AIM) {
    a.thrust = 0;
    a.svx *= 1 - 1.5f * dt;
    a.svy *= 1 - 1.5f * dt;
    float x1, y1, x2, y2;
    digitBox(a, a.qIdx[a.qi], x1, y1, x2, y2);
    const float tx = (x1 + x2) / 2, ty = (y1 + y2) / 2;
    const float want = atan2f(ty - a.sy, tx - a.sx), d = angDiff(a.sh, want), st = 4.5f * dt;
    a.aim += dt;
    if (fabsf(d) <= st || a.aim > 2) {
      a.sh = want;
      fire(a, tx, ty, -1);
      a.phase = P_FIRE;
    } else {
      a.sh += d > 0 ? st : -st;
    }
  } else if (a.phase == P_FIRE) {
    if (!a.bOn) {
      if (a.refire++ < 2) a.phase = P_AIM, a.aim = 0;
      else shatter(a, a.qIdx[a.qi]);
    }
  } else if (a.phase == P_SHATTER) {
    bool left = false;
    for (const Shard& q : a.shards) left |= q.on && q.digit;
    if (!left) {  // reveal the new digit with a drop bounce
      const int i = a.qIdx[a.qi];
      a.shown[i] = a.qVal[a.qi];
      a.velY[i] = -3.5f * s;
      a.hidden = -1;
      if (++a.qi < a.qN) a.phase = P_AIM, a.aim = 0, a.refire = 0;
      else a.phase = P_IDLE, a.idle = 0.5f;
    }
  }
  if (a.thrust > 0 && a.phase == P_IDLE) a.svx += cosf(a.sh) * 48 * s * dt, a.svy += sinf(a.sh) * 48 * s * dt;
  // Keep clear of the digits so the ship stays in view.
  const float bl = a.xs[0] - 6 * s, br = a.xs[4] + a.cell + 6 * s, bt = a.top - 6 * s, bb = a.top + a.inkH + 6 * s;
  if (a.sx > bl && a.sx < br && a.sy > bt && a.sy < bb) {
    a.svy += (a.sy < (bt + bb) / 2 ? -90 : 90) * s * dt;
    a.svx += (a.sx < (bl + br) / 2 ? -25 : 25) * s * dt;
  }
  const float mx = 42 * s, sp = sqrtf(a.svx * a.svx + a.svy * a.svy);
  if (sp > mx) a.svx *= mx / sp, a.svy *= mx / sp;
  a.svx *= 1 - 0.15f * dt;
  a.svy *= 1 - 0.15f * dt;
  a.sx += a.svx * dt;
  a.sy += a.svy * dt;
  wrap(a, a.sx, a.sy, 6 * s);
  int n = 0;
  for (Rock& r : a.rocks) {
    if (!r.on) continue;
    n++;
    r.x += r.vx * dt;
    r.y += r.vy * dt;
    r.angle += r.spin * dt;
    wrap(a, r.x, r.y, 10 * s);
  }
  if (n < 2 && (a.respawn -= dt) <= 0) {
    spawnRock(a, false);
    a.respawn = rnd(2, 5);
  }
  if (a.bOn) {
    a.bx += a.bvx * dt;
    a.by += a.bvy * dt;
    a.bLife -= dt;
    if (a.bLife <= 0) {
      a.bOn = false;
    } else if (a.bRock >= 0) {
      Rock& r = a.rocks[a.bRock];
      const float dx = a.bx - r.x, dy = a.by - r.y, rad = (r.big ? 8 : 5) * s;
      if (!r.on) a.bOn = false;
      else if (dx * dx + dy * dy < rad * rad) hitRock(a, r), a.bOn = false;
    } else if (a.phase == P_FIRE) {
      float x1, y1, x2, y2;
      digitBox(a, a.qIdx[a.qi], x1, y1, x2, y2);
      if (a.bx >= x1 - 1 && a.bx <= x2 + 1 && a.by >= y1 - 1 && a.by <= y2 + 1) {
        a.bOn = false;
        shatter(a, a.qIdx[a.qi]);
      }
    }
  }
  for (Shard& q : a.shards) {
    if (!q.on) continue;
    q.x += q.vx * dt;
    q.y += q.vy * dt;
    q.a += q.spin * dt;
    if ((q.life -= dt) <= 0) q.on = false;
  }
  // The new digit's drop bounce (3.5 px up, 0.6 px gravity per 50 ms frame on the panel).
  const float ps = dt / 0.05f;
  for (int i = 0; i < 5; i++) {
    if (a.offY[i] == 0 && a.velY[i] == 0) continue;
    a.velY[i] += 0.6f * s * ps;
    a.offY[i] += a.velY[i] * ps;
    if (a.offY[i] >= 0) a.offY[i] = a.velY[i] = 0;
  }
  (void)now;
}

// --- Redraw areas -----------------------------------------------------------------

struct Box {
  float x1, y1, x2, y2;
  bool on;
  void add(float x, float y, float r) {
    if (!on) x1 = x - r, y1 = y - r, x2 = x + r, y2 = y + r, on = true;
    else x1 = LV_MIN(x1, x - r), y1 = LV_MIN(y1, y - r), x2 = LV_MAX(x2, x + r), y2 = LV_MAX(y2, y + r);
  }
};

// What moves now, object coordinates, one slot per object so a frame redraws
// each object's old and new place as one area: 0-4 digits, 5 ball or ship,
// 6 second ball or bullet, 7 paddle, 8 fragments or shards, 9.. rocks.
constexpr int SLOTS = 9 + MAX_ROCKS;

static void motion(const ArcadeRt& a, Box* b) {
  const float s = a.s, lim = 4 * s + 2;
  for (int i = 0; i < 5; i++) {
    const bool moving = a.offX[i] != 0 || a.offY[i] != 0 || a.velX[i] != 0 || a.velY[i] != 0 || a.tr[i].state;
    if (!moving) continue;
    const float lift = a.style == CLOCK_ROCKS ? 12 * s : lim;
    b[i] = {a.xs[i] - lim, a.top - lift - 2, a.xs[i] + a.cell + lim, a.top + a.inkH + lim, true};
  }
  if (a.style == CLOCK_BREAKOUT) {
    for (int k = 0; k < 2; k++)
      if (a.balls[k].on) b[5 + k].add(a.balls[k].x, a.balls[k].y, LV_MAX(1.5f, s) + 1);
    b[7] = {a.padX - a.padW / 2 - 1, a.padY - 1, a.padX + a.padW / 2 + 1, a.padY + a.padH + 1, true};
    for (const Frag& q : a.frags)
      if (q.on) b[8].add(q.x, q.y, LV_MAX(2.0f, 1.6f * s));
    return;
  }
  b[5].add(a.sx, a.sy, 9 * s);
  if (a.bOn) b[6].add(a.bx, a.by, s + 1);
  for (const Shard& q : a.shards)
    if (q.on) b[8].add(q.x, q.y, 3.5f * s);
  for (int k = 0; k < MAX_ROCKS; k++)
    if (a.rocks[k].on) b[9 + k].add(a.rocks[k].x, a.rocks[k].y, (a.rocks[k].big ? 9.5f : 5.5f) * a.s);
}

static void invalidate(lv_obj_t* o, const lv_area_t* a, int n) {
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  for (int k = 0; k < n; k++) {
    lv_area_t s = a[k];
    lv_area_move(&s, c.x1, c.y1);
    lv_obj_invalidate_area(o, &s);
  }
}

static void resetChange(ArcadeRt& a) {
  memset(a.tr, 0, sizeof(a.tr));
  for (Frag& q : a.frags) q.on = false;
  for (Shard& q : a.shards)
    if (q.digit) q.on = false;
  a.phase = P_IDLE;
  a.hidden = -1;
  a.bOn = false;
}

// One animation frame. cur/next are "HH:MM" now and a minute on; false when nothing changed.
bool arcadeTick(ArcadeRt* a, lv_obj_t* o, uint32_t now, bool live, bool lit, const char* cur, const char* next, int sec,
                int32_t minute) {
  if (!a->f) return false;
  bool changed = false;
  if (lit != a->colonLit) {
    a->colonLit = lit;
    const lv_area_t c = {a->xs[2], a->top - 4, a->xs[2] + a->colonW, a->top + a->inkH + 4};
    invalidate(o, &c, 1);
    changed = true;
  }
  if (!live) {
    a->lastMs = 0;
    bool busy = a->phase != P_IDLE;
    for (const Trans& t : a->tr) busy |= t.state != 0;
    if (busy) resetChange(*a);
    if (busy || memcmp(a->shown, cur, 5)) {
      memcpy(a->shown, cur, 6);
      lv_obj_invalidate(o);
      changed = true;
    }
    return changed;
  }
  if (a->style == CLOCK_BREAKOUT) {
    // The change starts on the minute: each changed digit has to be broken first.
    bool busy = false;
    for (const Trans& t : a->tr) busy |= t.state != 0;
    if (!busy && memcmp(a->shown, cur, 5))
      for (int i : {0, 1, 3, 4})
        if (a->shown[i] != cur[i]) a->tr[i] = {1, a->shown[i], cur[i], 0, now};
  } else if (a->phase == P_IDLE) {
    if (sec >= 56 && a->triggered != minute) {
      a->triggered = minute;
      a->qN = a->qi = 0;
      for (int i : {0, 1, 3, 4})
        if (a->shown[i] != next[i]) a->qIdx[a->qN] = (uint8_t)i, a->qVal[a->qN++] = next[i];
      if (a->qN) a->phase = P_AIM, a->aim = 0, a->refire = 0, a->bOn = false;
    } else {
      const char* want = a->triggered == minute ? next : cur;
      if (memcmp(a->shown, want, 5)) {
        memcpy(a->shown, want, 6);
        lv_obj_invalidate(o);
        changed = true;
      }
    }
  }
  const float dt = a->lastMs ? LV_MIN(50u, now - a->lastMs) / 1000.0f : 0.016f;
  a->lastMs = now;
  Box was[SLOTS] = {}, is[SLOTS] = {};
  motion(*a, was);
  if (a->style == CLOCK_BREAKOUT) stepBreakout(*a, dt, now);
  else stepRocks(*a, dt, now);
  motion(*a, is);
  lv_area_t ar[SLOTS];
  int n = 0;
  for (int k = 0; k < SLOTS; k++) {
    Box u = was[k];
    if (is[k].on) u.add(is[k].x1, is[k].y1, 0), u.add(is[k].x2, is[k].y2, 0);
    if (u.on) ar[n++] = {(int32_t)floorf(u.x1) - 2, (int32_t)floorf(u.y1) - 2, (int32_t)ceilf(u.x2) + 2, (int32_t)ceilf(u.y2) + 2};
  }
  invalidate(o, ar, n);
  return changed || n > 0;
}

// --- Drawing ---------------------------------------------------------------------

static bool hits(const lv_area_t& a, const lv_area_t& c) { return !(a.x2 < c.x1 || a.x1 > c.x2 || a.y2 < c.y1 || a.y1 > c.y2); }

static void fill(lv_layer_t* L, float x1, float y1, float x2, float y2, lv_color_t col, int radius = 0) {
  const lv_area_t a = {(int32_t)lroundf(x1), (int32_t)lroundf(y1), (int32_t)lroundf(x2), (int32_t)lroundf(y2)};
  if (!hits(a, L->_clip_area)) return;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = col;
  d.bg_opa = LV_OPA_COVER;
  d.radius = radius;
  lv_draw_rect(L, &d, &a);
}

static void seg(lv_layer_t* L, float x1, float y1, float x2, float y2, int w, lv_color_t col) {
  const lv_area_t a = {(int32_t)floorf(LV_MIN(x1, x2)) - w, (int32_t)floorf(LV_MIN(y1, y2)) - w,
                       (int32_t)ceilf(LV_MAX(x1, x2)) + w, (int32_t)ceilf(LV_MAX(y1, y2)) + w};
  if (!hits(a, L->_clip_area)) return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.p1.x = (int32_t)lroundf(x1);
  d.p1.y = (int32_t)lroundf(y1);
  d.p2.x = (int32_t)lroundf(x2);
  d.p2.y = (int32_t)lroundf(y2);
  d.width = w;
  d.color = col;
  lv_draw_line(L, &d);
}

static void glyph(lv_layer_t* L, const lv_font_t* f, char ch, int x, int y, int w, lv_color_t col) {
  if (ch == ' ') return;
  const lv_area_t a = {x, y, x + w - 1, y + lineH(f) - 1};
  if (!hits(a, L->_clip_area)) return;
  const char s[2] = {ch, 0};
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = s;
  d.text_local = 1;
  d.font = f;
  d.color = col;
  d.align = LV_TEXT_ALIGN_CENTER;
  lv_draw_label(L, &d, &a);
}

void arcadeDraw(const ArcadeRt* a, lv_layer_t* L, int ox, int oy, lv_color_t col, uint32_t now) {
  if (!a->f) return;
  const float s = a->s;
  if (a->style == CLOCK_BREAKOUT) {
    const lv_color_t cyan = lv_color_hex(C_CYAN), white = lv_color_hex(C_BALL);
    fill(L, ox + a->padX - a->padW / 2, oy + a->padY, ox + a->padX + a->padW / 2 - 1, oy + a->padY + a->padH - 1, cyan);
    const float r = LV_MAX(1.5f, s);
    for (const Ball& b : a->balls)
      if (b.on) fill(L, ox + b.x - r, oy + b.y - r, ox + b.x + r, oy + b.y + r, white, LV_RADIUS_CIRCLE);
    const float fz = LV_MAX(2.0f, 1.6f * s) / 2;
    for (const Frag& q : a->frags)
      if (q.on) fill(L, ox + q.x - fz, oy + q.y - fz, ox + q.x + fz - 1, oy + q.y + fz - 1, col);
  } else {
    const int lw = LV_MAX(1, (int)lroundf(s * 0.75f));
    const lv_color_t rock = lv_color_hex(C_ROCK), cyan = lv_color_hex(C_CYAN);
    for (const Rock& r : a->rocks) {
      if (!r.on) continue;
      float px = 0, py = 0;
      for (int v = 0; v <= 7; v++) {
        const int i = v % 7;
        const float ang = r.angle + TAU * i / 7, x = ox + r.x + cosf(ang) * r.vr[i], y = oy + r.y + sinf(ang) * r.vr[i];
        if (v) seg(L, px, py, x, y, lw, rock);
        px = x, py = y;
      }
    }
    const float c = cosf(a->sh), n = sinf(a->sh), x = ox + a->sx, y = oy + a->sy;
    const float nx = x + c * 5 * s, ny = y + n * 5 * s;
    const float lx = x - c * 4 * s - n * 3.5f * s, ly = y - n * 4 * s + c * 3.5f * s;
    const float rx = x - c * 4 * s + n * 3.5f * s, ry = y - n * 4 * s - c * 3.5f * s;
    seg(L, nx, ny, lx, ly, lw, cyan);
    seg(L, nx, ny, rx, ry, lw, cyan);
    seg(L, lx, ly, rx, ry, lw, cyan);
    if (a->thrust > 0 && a->phase == P_IDLE && (now / 60) % 2 == 0)
      seg(L, x - c * 4 * s, y - n * 4 * s, x - c * 8 * s, y - n * 8 * s, lw, cyan);
    if (a->bOn) fill(L, ox + a->bx - s * 0.7f, oy + a->by - s * 0.7f, ox + a->bx + s * 0.7f, oy + a->by + s * 0.7f, cyan);
    for (const Shard& q : a->shards) {
      if (!q.on) continue;
      const float hl = LV_MAX(0.5f, 3 * s * q.life / 0.9f), qc = cosf(q.a) * hl, qs = sinf(q.a) * hl;
      seg(L, ox + q.x - qc, oy + q.y - qs, ox + q.x + qc, oy + q.y + qs, lw, q.digit ? col : rock);
    }
  }
  // Digits on top.
  const int ty = oy + a->top - a->inkTop;
  for (int i = 0; i < 5; i++) {
    const int w = i == 2 ? a->colonW : a->cell;
    if (i == 2) {
      if (a->colonLit) glyph(L, a->f, ':', ox + a->xs[2], ty + a->colonDy, w, col);
      continue;
    }
    char ch = a->shown[i];
    if (a->style == CLOCK_ROCKS && a->hidden == i) continue;
    if (a->style == CLOCK_BREAKOUT && a->tr[i].state == 1) {
      const Trans& t = a->tr[i];
      ch = t.from;
      if (t.hits && (now / (100 - t.hits * 20)) % 2 == 0) continue;  // flickers faster with each hit
    } else if (a->style == CLOCK_BREAKOUT && a->tr[i].state == 2) {
      if (now - a->tr[i].at < 640) continue;  // the pieces are still flying in
      ch = a->tr[i].to;
    }
    glyph(L, a->f, ch, ox + a->xs[i] + (int)lroundf(a->offX[i]), ty + (int)lroundf(a->offY[i]), w, col);
  }
}

}  // namespace deckui
