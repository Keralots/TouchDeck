// Deck UI: the Breakout clock style. A ball knocks the changing digit apart
// before its pieces fly in as the new one. Motion is scaled from a 128x64
// panel (s) to the tile; only what moves is redrawn.
#include <math.h>

#include "deck_ui_clock.h"

namespace deckui {

constexpr uint32_t C_PADDLE = 0x00FFFF, C_BALL = 0xFFFFFF;
constexpr int MAX_FRAGS = 128;  // four digits changing at once
constexpr uint32_t BREAK_MS = 3000, ASSEMBLE_MS = 800, REVEAL_MS = 640;

namespace {

struct Frag {
  bool on;
  int8_t home;  // assembling into digit home, -1 = flying debris
  float x, y, vx, vy, tx, ty;
};
struct Ball {
  bool on;
  float x, y, vx, vy;
};
enum ChangeState : uint8_t { CH_NONE, CH_BREAK, CH_ASSEMBLE };
struct Change {
  ChangeState state;
  char from, to;
  uint8_t hits;
  uint32_t at;
};

struct Breakout : ClockScene {
  DigitRow dig;
  int16_t w, h;
  float s;
  char shown[6] = "  :  ";
  bool colonLit = true;
  uint32_t lastMs;
  Ball balls[2];
  float padX, padW, padH, padY, ballR, fragR;
  Change ch[5];
  Frag frags[MAX_FRAGS];

  bool fit(int tw, int th, int pad) override;
  bool tick(lv_obj_t* o, const ClockNow& n) override;
  void draw(lv_layer_t* L, int ox, int oy, lv_color_t col) override;

  void serve(Ball& b);
  void addFrag(const Frag& f);
  void knock(int i, Ball& b, float speed);
  void moveBall(Ball& b, float speed, float dt);
  void step(float dt, uint32_t now);
  bool busy() const;
  lv_area_t paddle() const;
  void motion(Box* b) const;
};

}  // namespace

ClockScene* breakoutScene() { return newScene<Breakout>(); }

bool Breakout::fit(int tw, int th, int pad) {
  if ((tw - 2 * pad) / 29 < 5 || !dig.fit(tw, th)) return false;  // 1x1
  dig.place(tw, th * 16 / 100);
  if (w == tw && h == th) return true;
  w = (int16_t)tw;
  h = (int16_t)th;
  s = LV_MIN(tw / 128.0f, th / 64.0f) * 0.9f + tw / 128.0f * 0.1f;
  padW = LV_MAX(24.0f, 22 * s);
  padH = LV_MAX(3.0f, 2 * s);
  padY = th - LV_MAX(6.0f, 4 * s);
  padX = tw / 2.0f;
  ballR = LV_MAX(1.5f, s);
  fragR = LV_MAX(2.0f, 1.6f * s) / 2;
  serve(balls[0]);
  balls[1].on = false;
  return true;
}

void Breakout::serve(Ball& b) {
  const float v = 70 * s;
  b = {true, padX, padY - 4 * s, coin() ? v : -v, -v};
}

void Breakout::addFrag(const Frag& f) {
  for (Frag& q : frags)
    if (!q.on) {
      q = f;
      return;
    }
}

bool Breakout::busy() const {
  for (const Change& c : ch)
    if (c.state) return true;
  return false;
}

// Ball b is on digit i: a breaking digit loses a part, the digit springs away, the ball bounces.
void Breakout::knock(int i, Ball& b, float speed) {
  float x1, y1, x2, y2;
  dig.box(i, x1, y1, x2, y2);
  const float cx = (x1 + x2) / 2, cy = (y1 + y2) / 2, r = ballR;
  Change& t = ch[i];
  if (t.state == CH_BREAK && t.hits < 3) {
    static const float SHARE[3] = {0.25f, 0.5f, 0.25f};
    const float share = SHARE[t.hits++];
    float px[35], py[35];
    const int n = dig.points(i, t.from, px, py);
    for (int k = 0; k < 2 * n; k++) {
      if (rnd(0, 1) >= share) continue;
      const float x = px[k / 2], y = py[k / 2];
      const float ang = atan2f(y - cy, x - cx) + rnd(-0.3f, 0.3f), v = rnd(0.7f, 1.7f) * 62.5f * s;
      addFrag({true, -1, x + rnd(-1, 1) * s, y + rnd(-1, 1) * s, cosf(ang) * v, sinf(ang) * v - 30 * s, 0, 0});
    }
  }
  if (fabsf(b.x - cx) > 4 * s) dig.velX[i] = (b.x < cx ? 3 : -3) * s;
  else dig.velY[i] = (b.y < cy ? 3 : -3) * s;
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
}

void Breakout::moveBall(Ball& b, float speed, float dt) {
  const float r = ballR, m = sqrtf(b.vx * b.vx + b.vy * b.vy);
  if (m > 0) b.vx = b.vx / m * speed, b.vy = b.vy / m * speed;
  if (fabsf(b.vy) < speed * 0.35f) b.vy = (b.vy < 0 ? -1 : 1) * speed * 0.35f;
  b.x += b.vx * dt;
  b.y += b.vy * dt;
  if (b.x < r + 2) b.x = r + 2, b.vx = fabsf(b.vx);
  if (b.x > w - r - 2) b.x = w - r - 2, b.vx = -fabsf(b.vx);
  if (b.y < r + 2) b.y = r + 2, b.vy = fabsf(b.vy);
  if (b.vy > 0 && b.y + r >= padY && b.y < padY + padH && fabsf(b.x - padX) <= padW / 2 + r) {
    b.y = padY - r;
    b.vy = -fabsf(b.vy);
    b.vx += (b.x - padX) / (padW / 2) * speed * 0.5f;
  }
  if (b.y > h + 10) serve(b);
  float bx[5][4];  // digit ink boxes
  for (int i : CLOCK_DIGITS) dig.box(i, bx[i][0], bx[i][1], bx[i][2], bx[i][3]);
  for (int i : CLOCK_DIGITS) {
    const float* q = bx[i];
    if (ch[i].state == CH_ASSEMBLE || b.x + r < q[0] || b.x - r > q[2] || b.y + r < q[1] || b.y - r > q[3]) continue;
    knock(i, b, speed);
    break;
  }
  // A ball between the two hour or minute digits pushes them apart.
  for (int l : {0, 3}) {
    const float lx2 = bx[l][2], rx1 = bx[l + 1][0];
    if (b.y + r < bx[l][1] || b.y - r > bx[l][3] || b.x <= lx2 || b.x >= rx1) continue;
    dig.velX[l] = -3 * s;
    dig.velX[l + 1] = 3 * s;
    b.vx = -b.vx;
    b.x = b.vx > 0 ? rx1 + r + 1 : lx2 - r - 1;
  }
}

void Breakout::step(float dt, uint32_t now) {
  bool breaking = false;
  for (const Change& c : ch) breaking |= c.state == CH_BREAK;
  const float speed = 70 * s * (breaking ? 1.35f : 1);
  if (breaking && !balls[1].on) serve(balls[1]);
  if (!breaking) balls[1].on = false;
  // The paddle follows the nearest falling ball, faster when far away.
  const Ball* near = &balls[0];
  for (const Ball& b : balls)
    if (b.on && b.vy > 0 && (near->vy <= 0 || b.y > near->y)) near = &b;
  const float d = near->x - padX, ad = fabsf(d);
  const float sp = (ad > 20 * s ? 5 : ad > 10 * s ? 4 : ad > 3 * s ? 3 : 2) * s * 60;
  padX += (d > 0 ? 1 : -1) * LV_MIN(ad, sp * dt);
  padX = LV_CLAMP(padW / 2, padX, w - padW / 2);
  for (Ball& b : balls)
    if (b.on) moveBall(b, speed, dt);
  for (int i : CLOCK_DIGITS) {
    Change& t = ch[i];
    if (!t.state) continue;
    const uint32_t el = now - t.at;
    if (t.state == CH_BREAK && (t.hits >= 3 || el > BREAK_MS)) {
      t.state = CH_ASSEMBLE;
      t.at = now;
      float px[35], py[35];
      const int n = dig.points(i, t.to, px, py);
      for (int k = 0; k < 2 * n; k++) {
        if (!coin()) continue;
        const int side = esp_random() % 4;
        const float x = side == 0 || side == 2 ? rnd(0, w) : side == 1 ? w + 5 : -5.0f;
        const float y = side == 1 || side == 3 ? rnd(0, h) : side == 0 ? -5.0f : h + 5;
        const float dup = (k & 1) - 0.5f;
        addFrag({true, (int8_t)i, x, y, 0, 0, px[k / 2] + dup * s, py[k / 2] + dup * s});
      }
    } else if (t.state == CH_ASSEMBLE && el > ASSEMBLE_MS) {
      shown[i] = t.to;
      t.state = CH_NONE;
      for (Frag& q : frags)
        if (q.on && q.home == i) q.on = false;
    }
  }
  const float g = 0.048f * 62.5f * 62.5f * s, v = 1.2f * 3 * 62.5f * s;
  for (Frag& q : frags) {
    if (!q.on) continue;
    if (q.home >= 0) {
      const float dx = q.tx - q.x, dy = q.ty - q.y, dd = sqrtf(dx * dx + dy * dy);
      if (dd < v * dt) q.x = q.tx, q.y = q.ty;
      else q.x += dx / dd * v * dt, q.y += dy / dd * v * dt;
    } else {
      q.vy += g * dt;
      q.x += q.vx * dt;
      q.y += q.vy * dt;
      if (q.y > h + 6) q.on = false;
    }
  }
  // Springs: strength 0.3, damping 0.85 per 50 ms frame, at most 4 panel px.
  const float ps = dt / 0.05f, lim = 4 * s;
  float damp = 0;
  auto spring = [&](float& o, float& vel) {
    if (o == 0 && vel == 0) return;
    if (damp == 0) damp = powf(0.85f, ps);
    vel = (vel - o * 0.3f * ps) * damp;
    o = LV_CLAMP(-lim, o + vel * ps, lim);
    if (fabsf(o) < 0.1f && fabsf(vel) < 0.1f) o = vel = 0;
  };
  for (int i : CLOCK_DIGITS) {
    spring(dig.offX[i], dig.velX[i]);
    spring(dig.offY[i], dig.velY[i]);
  }
}

// --- Frame -------------------------------------------------------------------------

lv_area_t Breakout::paddle() const {
  return {(int32_t)lroundf(padX - padW / 2), (int32_t)lroundf(padY), (int32_t)lroundf(padX + padW / 2 - 1),
          (int32_t)lroundf(padY + padH - 1)};
}

// What moves now, one slot per object: 0-4 digits, 5-6 balls, 7 debris, 8-12 the pieces of digit 0-4.
constexpr int SLOTS = 13;

void Breakout::motion(Box* b) const {
  const float lim = 4 * s + 2;
  for (int i : CLOCK_DIGITS) {
    if (!dig.moving(i) && !ch[i].state) continue;
    b[i] = {dig.xs[i] - lim, dig.top - lim - 2, dig.xs[i] + dig.cell + lim, dig.top + dig.inkH + lim, true};
  }
  for (int k = 0; k < 2; k++)
    if (balls[k].on) b[5 + k].add(balls[k].x, balls[k].y, ballR + 1);
  for (const Frag& q : frags)
    if (q.on) b[q.home < 0 ? 7 : 8 + q.home].add(q.x, q.y, 2 * fragR);
}

bool Breakout::tick(lv_obj_t* o, const ClockNow& n) {
  if (!dig.f) return false;
  Dirty d;
  if (n.colon != colonLit) {
    colonLit = n.colon;
    d.add(dig.colonArea());
  }
  if (!n.live) {
    lastMs = 0;
    if (busy() || memcmp(shown, n.cur, 5)) {
      memset(ch, 0, sizeof(ch));
      for (Frag& q : frags) q.on = false;
      memcpy(shown, n.cur, 6);
      d.add({0, 0, w, h});
    }
    return d.flush(o);
  }
  // The change starts on the minute: each changed digit has to be broken first.
  if (memcmp(shown, n.cur, 5) && !busy())
    for (int i : CLOCK_DIGITS)
      if (shown[i] != n.cur[i]) ch[i] = {CH_BREAK, shown[i], n.cur[i], 0, n.ms};
  const float dt = lastMs ? LV_MIN(50u, n.ms - lastMs) / 1000.0f : 0.016f;
  lastMs = n.ms;
  Box was[SLOTS] = {}, is[SLOTS] = {};
  const lv_area_t pad = paddle();
  motion(was);
  step(dt, n.ms);
  motion(is);
  for (int k = 0; k < SLOTS; k++) {
    was[k].add(is[k]);
    if (was[k].on) d.add(was[k].area());
  }
  const lv_area_t p = paddle();
  if (p.x1 != pad.x1 || p.x2 != pad.x2) d.add({LV_MIN(p.x1, pad.x1), p.y1, LV_MAX(p.x2, pad.x2), p.y2});
  return d.flush(o);
}

void Breakout::draw(lv_layer_t* L, int ox, int oy, lv_color_t col) {
  if (!dig.f) return;
  lv_area_t p = paddle();
  lv_area_move(&p, ox, oy);
  fillRect(L, p, lv_color_hex(C_PADDLE));
  auto square = [&](float x, float y, float r, float r2, lv_color_t c, int round) {
    fillRect(L, {(int32_t)lroundf(ox + x - r), (int32_t)lroundf(oy + y - r), (int32_t)lroundf(ox + x + r2),
                 (int32_t)lroundf(oy + y + r2)}, c, round);
  };
  for (const Ball& b : balls)
    if (b.on) square(b.x, b.y, ballR, ballR, lv_color_hex(C_BALL), LV_RADIUS_CIRCLE);
  for (const Frag& q : frags)
    if (q.on) square(q.x, q.y, fragR, fragR - 1, col, 0);
  if (colonLit) dig.draw(L, ox, oy, 2, ':', col);
  for (int i : CLOCK_DIGITS) {
    const Change& t = ch[i];
    char c = shown[i];
    if (t.state == CH_BREAK) {
      c = t.from;
      if (t.hits && (lastMs / (100 - t.hits * 20)) % 2 == 0) continue;  // flickers faster with each hit
    } else if (t.state == CH_ASSEMBLE) {
      if (lastMs - t.at < REVEAL_MS) continue;  // the pieces are still flying in
      c = t.to;
    }
    dig.draw(L, ox, oy, i, c, col);
  }
}

}  // namespace deckui
