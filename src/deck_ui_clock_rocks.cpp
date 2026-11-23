// Deck UI: the Rocks clock style. An outline ship shoots the changing digit.
// Rocks and ship are A8 images redrawn each frame, so they turn and drift
// smoothly. Motion is scaled from a 128x64 panel (s) to the tile.
#include <math.h>

#include "deck_ui_a8.h"
#include "deck_ui_clock.h"

namespace deckui {

constexpr uint32_t C_SHIP = 0x00FFFF, C_ROCK = 0xC9A27A;
constexpr float TAU = 6.2831853f;
constexpr int TRIGGER_SEC = 56;  // the shooting starts here so the new time is mostly in by :00
constexpr int MAX_ROCKS = 8, ROCK_POINTS = 7;
constexpr int DIGIT_SHARDS = 10, SPARKS = 14;  // a digit's own shards, never short of slots
constexpr float BIG = 7, SMALL = 4, BUMP = 1.3f;  // rock radii and how far a corner sticks out, panel px
constexpr float SHARD_LIFE = 0.9f;

namespace {

// A shape drawn into its own A8 image around a point of the tile.
struct Sprite {
  lv_draw_buf_t* buf;
  int16_t x, y;    // image corner on the tile
  lv_area_t used;  // pixels drawn, in the image; x2 < x1 = none
  lv_area_t last;  // the frame before, on the tile

  bool size(int side);
  void free();
  Cv begin(float cx, float cy);
  void end(const Cv& c) { used = {c.x1, c.y1, c.x2, c.y2}; }
  bool empty() const { return used.x2 < used.x1; }
  lv_area_t area() const { return {x + used.x1, y + used.y1, x + used.x2, y + used.y2}; }
  void addTo(Dirty& d) const;  // this frame and the one before
  void draw(lv_layer_t* L, int ox, int oy, lv_color_t col) const;
};

struct Rock {
  bool on, big;
  float x, y, vx, vy, angle, spin;
  float r[ROCK_POINTS];
};
struct Shard {
  bool on;
  float x, y, vx, vy, a, spin, life;
};

enum Phase : uint8_t { P_IDLE, P_AIM, P_FIRE, P_SHATTER };

struct Rocks : ClockScene {
  DigitRow dig;
  int16_t w, h;
  float s, pen;  // panel px on this tile; half the line width
  char shown[6] = "  :  ";
  bool colonLit = true;
  uint32_t lastMs;
  int32_t triggered = -1;
  float sx, sy, svx, svy, heading, thrust, idle, turnTo, shot, respawn, aim;
  bool turning;
  Rock rocks[MAX_ROCKS];
  bool bulletOn;
  int8_t bulletRock;  // the rock it was fired at, -1 = a digit
  float bx, by, bvx, bvy, bLife;
  Shard shards[DIGIT_SHARDS + SPARKS];  // the digit's first
  Phase phase;
  uint8_t qIdx[4], qN, qi, refire;
  char qVal[4];
  int8_t hidden = -1;  // digit shot away
  Sprite rockImg[MAX_ROCKS], shipImg;

  ~Rocks() override;
  bool fit(int tw, int th, int pad) override;
  bool tick(lv_obj_t* o, const ClockNow& n) override;
  void draw(lv_layer_t* L, int ox, int oy, lv_color_t col) override;

  void spawn(bool inside);
  void shape(Rock& r, float base);
  void addShard(int from, int to, float px, float py, float vx, float vy, float life);
  void burst(float px, float py, int n, float k);
  void hit(Rock& r);
  void fire(float tx, float ty, int rock);
  void shatter(int i);
  void aimAt(int i, float& tx, float& ty) const;
  void fly(float dt);
  void shoot(float dt);
  void step(float dt);
  void render();
  void motion(Box* b) const;
  void reset();
};

}  // namespace

ClockScene* rocksScene() { return newScene<Rocks>(); }

// --- Sprites -----------------------------------------------------------------------

bool Sprite::size(int side) {
  if (buf && (int)buf->header.w == side) return true;
  free();
  buf = lv_draw_buf_create(side, side, LV_COLOR_FORMAT_A8, 0);
  used = {0, 0, side - 1, side - 1};  // new memory: wipe all of it
  return buf != nullptr;
}

void Sprite::free() {
  if (buf) lv_draw_buf_destroy(buf);
  buf = nullptr;
}

// Wipes the last frame and centres the image on cx, cy.
Cv Sprite::begin(float cx, float cy) {
  const int side = (int)buf->header.w, stride = (int)buf->header.stride;
  for (int r = used.y1; r <= used.y2; r++) memset(buf->data + r * stride + used.x1, 0, (size_t)(used.x2 - used.x1 + 1));
  last = empty() ? lv_area_t{0, 0, -1, -1} : area();
  x = (int16_t)(ifloor(cx) - side / 2);
  y = (int16_t)(ifloor(cy) - side / 2);
  return {buf->data, side, side, stride, side, side, -1, -1};
}

void Sprite::addTo(Dirty& d) const {
  lv_area_t a = area();
  if (last.x2 >= last.x1) {
    if (empty()) a = last;
    else a = {LV_MIN(a.x1, last.x1), LV_MIN(a.y1, last.y1), LV_MAX(a.x2, last.x2), LV_MAX(a.y2, last.y2)};
  } else if (empty()) {
    return;
  }
  d.add(a);
}

void Sprite::draw(lv_layer_t* L, int ox, int oy, lv_color_t col) const {
  if (!buf || empty()) return;
  lv_area_t a = area();
  lv_area_move(&a, ox, oy);
  if (!areaHits(a, L->_clip_area)) return;
  const int side = (int)buf->header.w;
  a = {ox + x, oy + y, ox + x + side - 1, oy + y + side - 1};
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = buf;
  d.recolor = col;
  d.recolor_opa = LV_OPA_COVER;
  lv_draw_image(L, &d, &a);
}

// --- Layout ------------------------------------------------------------------------

Rocks::~Rocks() {
  for (Sprite& q : rockImg) q.free();
  shipImg.free();
}

bool Rocks::fit(int tw, int th, int pad) {
  if ((tw - 2 * pad) / 29 < 5 || !dig.fit(tw, th)) return false;  // 1x1
  const bool same = w == tw && h == th;
  dig.place(tw, (th - dig.inkH) / 2);
  w = (int16_t)tw;
  h = (int16_t)th;
  s = LV_MIN(tw / 128.0f, th / 64.0f) * 0.9f + tw / 128.0f * 0.1f;
  pen = LV_MAX(1.0f, s * 0.75f) / 2;
  const int rockSide = 2 * (int)ceilf(BIG * BUMP * s + pen + 2), shipSide = 2 * (int)ceilf(8 * s + pen + 2);
  for (Sprite& q : rockImg)
    if (!q.size(rockSide)) return false;
  if (!shipImg.size(shipSide)) return false;
  if (!same) {
    memset(rocks, 0, sizeof(rocks));
    sx = tw * 0.25f;
    sy = th * 0.75f;
    svx = svy = heading = 0;
    idle = 1;
    shot = 5;
    for (int k = 0; k < 2; k++) spawn(true);
  }
  render();
  return true;
}

// --- Motion ------------------------------------------------------------------------

static float angDiff(float a, float b) {
  float d = b - a;
  while (d > TAU / 2) d -= TAU;
  while (d < -TAU / 2) d += TAU;
  return d;
}

// Turns a towards b by at most st; true when there.
static bool turn(float& a, float b, float st) {
  const float d = angDiff(a, b);
  if (fabsf(d) <= st) {
    a = b;
    return true;
  }
  a += d > 0 ? st : -st;
  return false;
}

static void wrap(const Rocks& a, float& x, float& y, float m) {
  if (x < -m) x = a.w + m;
  if (x > a.w + m) x = -m;
  if (y < -m) y = a.h + m;
  if (y > a.h + m) y = -m;
}

void Rocks::shape(Rock& r, float base) {
  for (float& v : r.r) v = LV_MAX(2.0f, base * s * rnd(0.7f, BUMP));
}

void Rocks::spawn(bool inside) {
  for (Rock& r : rocks) {
    if (r.on) continue;
    r = {};
    r.on = r.big = true;
    const int e = esp_random() % 4;
    r.x = inside ? rnd(0, w) : e == 0 ? -8 * s : e == 1 ? w + 8 * s : rnd(8, w - 8);
    r.y = inside ? rnd(0, h) : e == 2 ? -8 * s : e == 3 ? h + 8 * s : rnd(8, h - 8);
    const float ang = atan2f(h / 2.0f - r.y, w / 2.0f - r.x) + rnd(-0.6f, 0.6f), v = 8 * s;
    r.vx = cosf(ang) * v;
    r.vy = sinf(ang) * v;
    r.angle = rnd(0, TAU);
    r.spin = rnd(0.5f, 1.5f) * (coin() ? 1 : -1);
    shape(r, BIG);
    return;
  }
}

void Rocks::addShard(int from, int to, float px, float py, float vx, float vy, float life) {
  for (int i = from; i < to; i++)
    if (!shards[i].on) {
      shards[i] = {true, px, py, vx, vy, rnd(0, TAU), rnd(-4, 4), life};
      return;
    }
}

void Rocks::burst(float px, float py, int n, float k) {
  for (int i = 0; i < n; i++) {
    const float ang = rnd(0, TAU), v = rnd(15, 45) * k * s;
    addShard(DIGIT_SHARDS, DIGIT_SHARDS + SPARKS, px, py, cosf(ang) * v, sinf(ang) * v, SHARD_LIFE * rnd(0.6f, 1));
  }
}

void Rocks::hit(Rock& r) {
  if (!r.big) {
    r.on = false;
    burst(r.x, r.y, 5, 1);
    return;
  }
  r.big = false;
  shape(r, SMALL);
  const float ang = atan2f(r.vy, r.vx) + TAU / 4, kx = cosf(ang) * 8 * s, ky = sinf(ang) * 8 * s;
  for (Rock& c : rocks)
    if (!c.on) {
      c = r;
      c.vx = r.vx - kx;
      c.vy = r.vy - ky;
      c.spin = -r.spin;
      shape(c, SMALL);
      break;
    }
  r.vx += kx;
  r.vy += ky;
  burst(r.x, r.y, 3, 0.6f);
}

void Rocks::fire(float tx, float ty, int rock) {
  const float ang = atan2f(ty - sy, tx - sx), v = 140 * s;
  bulletOn = true;
  bx = sx + cosf(heading) * 6 * s;
  by = sy + sinf(heading) * 6 * s;
  bvx = cosf(ang) * v;
  bvy = sinf(ang) * v;
  bLife = 1.5f;
  bulletRock = (int8_t)rock;
}

void Rocks::aimAt(int i, float& tx, float& ty) const {
  float x1, y1, x2, y2;
  dig.box(i, x1, y1, x2, y2);
  tx = (x1 + x2) / 2;
  ty = (y1 + y2) / 2;
}

void Rocks::shatter(int i) {
  float cx, cy, px[35], py[35];
  aimAt(i, cx, cy);
  const int n = dig.points(i, shown[i], px, py), m = LV_MIN(DIGIT_SHARDS, n);
  for (int k = 0; k < m; k++) {
    const int src = k * n / m;
    const float ang = atan2f(py[src] - cy, px[src] - cx) + rnd(-0.5f, 0.5f), v = rnd(20, 50) * s;
    addShard(0, DIGIT_SHARDS, px[src], py[src], cosf(ang) * v, sinf(ang) * v, SHARD_LIFE * rnd(0.7f, 1));
  }
  hidden = (int8_t)i;
  phase = P_SHATTER;
}

// Wandering and potshots while the time stands; aiming and firing at a change.
void Rocks::fly(float dt) {
  if (phase == P_IDLE) {
    idle -= dt;
    shot -= dt;
    if (turning && turn(heading, turnTo, 2.5f * dt)) turning = false;
    if (thrust > 0) thrust -= dt;
    if (idle <= 0) {
      idle = rnd(1.2f, 3);
      const float act = rnd(0, 1);
      if (act < 0.4f) turnTo = rnd(0, TAU), turning = true;
      else if (act < 0.75f) thrust = rnd(0.3f, 0.7f);
    }
    if (shot <= 0 && !bulletOn) {  // an occasional potshot, big rocks first
      shot = rnd(6, 14);
      int best = -1;
      for (int i = 0; i < MAX_ROCKS; i++) {
        const Rock& r = rocks[i];
        if (!r.on || r.x < 4 || r.x > w - 4 || r.y < 4 || r.y > h - 4) continue;
        if (best < 0 || (r.big && !rocks[best].big)) best = i;
      }
      if (best >= 0) {
        heading = atan2f(rocks[best].y - sy, rocks[best].x - sx);
        turning = false;
        fire(rocks[best].x, rocks[best].y, best);
      }
    }
  } else if (phase == P_AIM) {
    thrust = 0;
    svx *= 1 - 1.5f * dt;
    svy *= 1 - 1.5f * dt;
    float tx, ty;
    aimAt(qIdx[qi], tx, ty);
    const float want = atan2f(ty - sy, tx - sx);
    aim += dt;
    if (turn(heading, want, 4.5f * dt) || aim > 2) {
      heading = want;
      fire(tx, ty, -1);
      phase = P_FIRE;
    }
  } else if (phase == P_FIRE) {
    if (!bulletOn) {
      if (refire++ < 2) phase = P_AIM, aim = 0;
      else shatter(qIdx[qi]);
    }
  } else if (phase == P_SHATTER) {
    bool left = false;
    for (int i = 0; i < DIGIT_SHARDS; i++) left |= shards[i].on;
    if (!left) {  // reveal the new digit with a drop bounce
      const int i = qIdx[qi];
      shown[i] = qVal[qi];
      dig.velY[i] = -3.5f * s;
      hidden = -1;
      if (++qi < qN) phase = P_AIM, aim = 0, refire = 0;
      else phase = P_IDLE, idle = 0.5f;
    }
  }
  if (thrust > 0 && phase == P_IDLE) svx += cosf(heading) * 48 * s * dt, svy += sinf(heading) * 48 * s * dt;
  // Keep clear of the digits so the ship stays in view.
  const float bl = dig.xs[0] - 6 * s, br = dig.xs[4] + dig.cell + 6 * s, bt = dig.top - 6 * s;
  const float bb = dig.top + dig.inkH + 6 * s;
  if (sx > bl && sx < br && sy > bt && sy < bb) {
    svy += (sy < (bt + bb) / 2 ? -90 : 90) * s * dt;
    svx += (sx < (bl + br) / 2 ? -25 : 25) * s * dt;
  }
  const float mx = 42 * s, sp = sqrtf(svx * svx + svy * svy);
  if (sp > mx) svx *= mx / sp, svy *= mx / sp;
  svx *= 1 - 0.15f * dt;
  svy *= 1 - 0.15f * dt;
  sx += svx * dt;
  sy += svy * dt;
  wrap(*this, sx, sy, 6 * s);
}

// The bullet in flight.
void Rocks::shoot(float dt) {
  if (!bulletOn) return;
  bx += bvx * dt;
  by += bvy * dt;
  bLife -= dt;
  if (bLife <= 0) {
    bulletOn = false;
  } else if (bulletRock >= 0) {
    Rock& r = rocks[bulletRock];
    const float dx = bx - r.x, dy = by - r.y, rad = (r.big ? 8 : 5) * s;
    if (!r.on) bulletOn = false;
    else if (dx * dx + dy * dy < rad * rad) hit(r), bulletOn = false;
  } else if (phase == P_FIRE) {
    float x1, y1, x2, y2;
    dig.box(qIdx[qi], x1, y1, x2, y2);
    if (bx >= x1 - 1 && bx <= x2 + 1 && by >= y1 - 1 && by <= y2 + 1) {
      bulletOn = false;
      shatter(qIdx[qi]);
    }
  }
}

void Rocks::step(float dt) {
  fly(dt);
  int n = 0;
  for (Rock& r : rocks) {
    if (!r.on) continue;
    n++;
    r.x += r.vx * dt;
    r.y += r.vy * dt;
    r.angle += r.spin * dt;
    wrap(*this, r.x, r.y, 10 * s);
  }
  if (n < 2 && (respawn -= dt) <= 0) {
    spawn(false);
    respawn = rnd(2, 5);
  }
  shoot(dt);
  for (Shard& q : shards) {
    if (!q.on) continue;
    q.x += q.vx * dt;
    q.y += q.vy * dt;
    q.a += q.spin * dt;
    if ((q.life -= dt) <= 0) q.on = false;
  }
  // The new digit's drop bounce (3.5 px up, 0.6 px gravity per 50 ms frame on the panel).
  const float ps = dt / 0.05f;
  for (int i : CLOCK_DIGITS) {
    if (!dig.moving(i)) continue;
    dig.velY[i] += 0.6f * s * ps;
    dig.offY[i] += dig.velY[i] * ps;
    if (dig.offY[i] >= 0) dig.offY[i] = dig.velY[i] = 0;
  }
}

// Stops a change half way.
void Rocks::reset() {
  for (int i = 0; i < DIGIT_SHARDS; i++) shards[i].on = false;
  phase = P_IDLE;
  hidden = -1;
  bulletOn = false;
}

// --- Frame -------------------------------------------------------------------------

// Draws the rocks and the ship into their images.
void Rocks::render() {
  for (int k = 0; k < MAX_ROCKS; k++) {
    const Rock& r = rocks[k];
    Sprite& q = rockImg[k];
    Cv c = q.begin(r.x, r.y);
    if (!r.on) {
      q.end(c);
      continue;
    }
    const float ox = r.x - q.x, oy = r.y - q.y;
    float px = 0, py = 0;
    for (int v = 0; v <= ROCK_POINTS; v++) {
      const int i = v % ROCK_POINTS;
      const float ang = r.angle + TAU * i / ROCK_POINTS, x = ox + cosf(ang) * r.r[i], y = oy + sinf(ang) * r.r[i];
      if (v) capsule(c, px, py, x, y, pen);
      px = x, py = y;
    }
    q.end(c);
  }
  Cv c = shipImg.begin(sx, sy);
  const float ox = sx - shipImg.x, oy = sy - shipImg.y, co = cosf(heading), si = sinf(heading);
  const float nx = ox + co * 5 * s, ny = oy + si * 5 * s;
  const float lx = ox - co * 4 * s - si * 3.5f * s, ly = oy - si * 4 * s + co * 3.5f * s;
  const float rx = ox - co * 4 * s + si * 3.5f * s, ry = oy - si * 4 * s - co * 3.5f * s;
  capsule(c, nx, ny, lx, ly, pen);
  capsule(c, nx, ny, rx, ry, pen);
  capsule(c, lx, ly, rx, ry, pen);
  if (thrust > 0 && phase == P_IDLE && (lastMs / 60) % 2 == 0)
    capsule(c, ox - co * 4 * s, oy - si * 4 * s, ox - co * 8 * s, oy - si * 8 * s, pen);
  shipImg.end(c);
}

// What else moves now, one slot per object: 0-4 digits, 5 bullet, 6 digit shards, 7 sparks.
constexpr int SLOTS = 8;

void Rocks::motion(Box* b) const {
  for (int i : CLOCK_DIGITS) {
    if (!dig.moving(i)) continue;
    b[i] = {(float)dig.xs[i], dig.top - 12 * s, (float)(dig.xs[i] + dig.cell), (float)(dig.top + dig.inkH), true};
  }
  if (bulletOn) b[5].add(bx, by, s + 1);
  for (int i = 0; i < DIGIT_SHARDS + SPARKS; i++)
    if (shards[i].on) b[i < DIGIT_SHARDS ? 6 : 7].add(shards[i].x, shards[i].y, 3.5f * s);
}

bool Rocks::tick(lv_obj_t* o, const ClockNow& n) {
  if (!dig.f) return false;
  Dirty d;
  if (n.colon != colonLit) {
    colonLit = n.colon;
    d.add(dig.colonArea());
  }
  if (!n.live) {
    lastMs = 0;
    triggered = -1;  // back before :00: shoot it again
    if (phase != P_IDLE || memcmp(shown, n.cur, 5)) {
      if (phase != P_IDLE) reset();
      memcpy(shown, n.cur, 6);
      d.add({0, 0, w, h});
    }
    return d.flush(o);
  }
  if (phase == P_IDLE) {
    if (n.sec >= TRIGGER_SEC && triggered != n.minute) {
      triggered = n.minute;
      qN = qi = 0;
      for (int i : CLOCK_DIGITS)
        if (shown[i] != n.next[i]) qIdx[qN] = (uint8_t)i, qVal[qN++] = n.next[i];
      if (qN) phase = P_AIM, aim = 0, refire = 0, bulletOn = false;
    } else {
      const char* want = triggered == n.minute ? n.next : n.cur;
      if (memcmp(shown, want, 5)) {
        memcpy(shown, want, 6);
        d.add({0, 0, w, h});
      }
    }
  }
  const float dt = lastMs ? LV_MIN(50u, n.ms - lastMs) / 1000.0f : 0.016f;
  lastMs = n.ms;
  Box was[SLOTS] = {}, is[SLOTS] = {};
  const int hid = hidden;
  motion(was);
  step(dt);
  render();
  motion(is);
  if (hidden != hid) {
    const int i = hidden >= 0 ? hidden : hid;
    d.add({dig.xs[i], dig.top, dig.xs[i] + dig.cell, dig.top + dig.inkH});
  }
  for (int k = 0; k < SLOTS; k++) {
    was[k].add(is[k]);
    if (was[k].on) d.add(was[k].area());
  }
  for (const Sprite& q : rockImg) q.addTo(d);
  shipImg.addTo(d);
  return d.flush(o);
}

void Rocks::draw(lv_layer_t* L, int ox, int oy, lv_color_t col) {
  if (!dig.f) return;
  const lv_color_t rock = lv_color_hex(C_ROCK), ship = lv_color_hex(C_SHIP);
  for (const Sprite& q : rockImg) q.draw(L, ox, oy, rock);
  shipImg.draw(L, ox, oy, ship);
  if (bulletOn) {
    const float r = s * 0.7f;
    fillRect(L, {ox + (int32_t)lroundf(bx - r), oy + (int32_t)lroundf(by - r), ox + (int32_t)lroundf(bx + r),
                 oy + (int32_t)lroundf(by + r)}, ship);
  }
  const int lw = LV_MAX(1, (int)lroundf(s * 0.75f));
  for (int i = 0; i < DIGIT_SHARDS + SPARKS; i++) {
    const Shard& q = shards[i];
    if (!q.on) continue;
    const float hl = LV_MAX(0.5f, 3 * s * q.life / SHARD_LIFE);
    const int cx = ox + (int)lroundf(q.x), cy = oy + (int)lroundf(q.y), m = (int)hl + lw + 1;
    if (!areaHits({cx - m, cy - m, cx + m, cy + m}, L->_clip_area)) continue;
    const float qc = cosf(q.a) * hl, qs = sinf(q.a) * hl;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = ox + (int32_t)lroundf(q.x - qc);
    d.p1.y = oy + (int32_t)lroundf(q.y - qs);
    d.p2.x = ox + (int32_t)lroundf(q.x + qc);
    d.p2.y = oy + (int32_t)lroundf(q.y + qs);
    d.width = lw;
    d.color = i < DIGIT_SHARDS ? col : rock;
    lv_draw_line(L, &d);
  }
  if (colonLit) dig.draw(L, ox, oy, 2, ':', col);
  for (int i : CLOCK_DIGITS)
    if (i != hidden) dig.draw(L, ox, oy, i, shown[i], col);
}

}  // namespace deckui
