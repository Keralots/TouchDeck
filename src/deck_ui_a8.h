// Antialiased shapes on an A8 image. LVGL places its shapes on whole pixels, so
// a slow drift steps visibly; these get their edge coverage per pixel.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>

namespace deckui {

// The S3's FPU multiplies but has no square root, and newlib's sqrtf, floorf
// and ceilf are slow software: these stand in for them in the pixel loops.
static inline float fsqrt(float x) {
  if (x <= 0) return 0;
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i = 0x5f3759df - (u.i >> 1);
  float y = u.f;
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  return x * y;
}
static inline int ifloor(float v) { return (int)(v + 4096.0f) - 4096; }
static inline int iceil(float v) { return 4096 - (int)(4096.0f - v); }

struct Cv {
  uint8_t* px;
  int w, h, stride;
  int x1, y1, x2, y2;  // what was drawn on, for the next clear
};

static inline void put(Cv& c, int x, int y, float cover, int opa) {
  const int a = (int)(cover * (float)opa);
  uint8_t& p = c.px[y * c.stride + x];
  if (a > p) p = (uint8_t)a;
}

static inline void touch(Cv& c, int x1, int y1, int x2, int y2) {
  c.x1 = std::min(c.x1, x1);
  c.y1 = std::min(c.y1, y1);
  c.x2 = std::max(c.x2, x2);
  c.y2 = std::max(c.y2, y2);
}

// Coverage from the squared distance to a shape r thick: the root is taken on
// the edge only (the S3 has no hardware square root).
static inline float edge(float d2, float r) {
  const float in = r - 0.5f, out = r + 0.5f;
  if (d2 >= out * out) return 0;
  if (in > 0 && d2 <= in * in) return 1;
  return r - fsqrt(d2) + 0.5f;
}

// A line with round ends at any angle, r thick on each side. A row visits only
// the pixels near the line.
static inline void capsule(Cv& c, float ax, float ay, float bx, float by, float r, int opa = 255) {
  const int x1 = std::max(0, ifloor(std::min(ax, bx) - r - 1)), x2 = std::min(c.w - 1, iceil(std::max(ax, bx) + r + 1));
  const int y1 = std::max(0, ifloor(std::min(ay, by) - r - 1)), y2 = std::min(c.h - 1, iceil(std::max(ay, by) + r + 1));
  if (x2 < x1 || y2 < y1) return;
  touch(c, x1, y1, x2, y2);
  const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy, inv = len2 > 0 ? 1.0f / len2 : 0;
  const bool slanted = vy > 0.01f || vy < -0.01f;
  const float slope = slanted ? vx / vy : 0, invVy = slanted ? 1.0f / vy : 0;
  const float reach = slanted ? (r + 0.6f) * fsqrt(len2) * (invVy < 0 ? -invVy : invVy) : 0;
  for (int y = y1; y <= y2; y++) {
    const float py = (float)y + 0.5f - ay;
    int xa = x1, xb = x2;
    if (slanted) {  // within reach of the endless line through a and b
      const float mid = ax + py * slope - 0.5f;
      xa = std::max((float)x1, mid - reach);
      const float hi = std::min((float)x2, mid + reach + 1);
      if (hi < xa) continue;
      xb = (int)hi;
    }
    for (int x = xa; x <= xb; x++) {
      const float px = (float)x + 0.5f - ax;
      const float u = std::min(1.0f, std::max(0.0f, (px * vx + py * vy) * inv));
      const float dx = px - u * vx, dy = py - u * vy;
      const float cover = edge(dx * dx + dy * dy, r);
      if (cover > 0) put(c, x, y, std::min(cover, 1.0f), opa);
    }
  }
}

}  // namespace deckui
