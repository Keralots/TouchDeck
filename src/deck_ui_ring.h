// Orbit ring round a tile's edge (the Orbit clock, the Orbit on/off effect).
#pragma once
#include "deck_ui_int.h"

namespace deckui {

constexpr float RING_GLOW = 2.2f;  // head glow radius, band widths

// One piece of the ring: a straight side or a quarter circle.
// Points run on the band's centre line, in pixel centres.
struct RingSeg {
  bool arc;
  float x0, y0, x1, y1;  // line ends; an arc's centre is x0/y0
  int16_t a0;            // arc start, degrees (0 = right, clockwise)
  float at, len;         // start along the ring, length
  uint8_t side;          // lines: 0 top, 1 right, 2 bottom, 3 left
};

// From the top centre, clockwise, on the edge of a w x h object. Object coordinates.
struct Ring {
  int16_t w, h;
  RingSeg seg[9];
  float len;           // 0 = no ring
  int16_t lw, ro, rr;  // band width, outer edge inset, arc outer radius
  float rc;            // centre line radius
  int16_t ext;         // how far the head's glow sticks out of the object
};

// w and h set; rad: the object's inner corner radius.
void ringBuild(Ring& c, int rad);
void ringPoint(const Ring& c, float d, float& x, float& y);
// Redraws the ring between d0 and d1 on o, with room for the head's glow.
void ringInvalidate(lv_obj_t* o, const Ring& c, float d0, float d1);
// Band of width w between d0 and d1, centred on the ring's band.
void ringStroke(lv_layer_t* L, const Ring& c, int ox, int oy, float d0, float d1, int w, lv_color_t col,
                lv_opa_t opa);
// Comet tail from d0 to d1: opacity lo at the far end, hi at the head (d1, or d0 when rev).
// steps: pieces of the tail on boards that step it.
void ringTail(lv_layer_t* L, const Ring& c, int ox, int oy, float d0, float d1, lv_color_t col, lv_opa_t lo = 100,
              lv_opa_t hi = 255, bool rev = false, int steps = 6);
// The comet's head at d: a glow and a bright core.
void ringHead(lv_layer_t* L, const Ring& c, int ox, int oy, float d, lv_color_t glow, lv_color_t core,
              lv_opa_t opa = LV_OPA_COVER);

}  // namespace deckui
