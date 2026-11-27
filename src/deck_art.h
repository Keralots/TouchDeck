// Built-in wallpapers: scenes drawn by code at any size, nothing stored.
#ifndef DECK_ART_H
#define DECK_ART_H

#include <stdint.h>

namespace deck {

constexpr int ART_COUNT = 6;
const char* artName(int index);

// When and where a scene is drawn for. utc 0 = the scene as designed.
// Without a place the sun is up from 7 to 19 local time.
struct ArtTime {
  uint32_t utc;
  bool place;
  float lat, lon;
  bool operator==(const ArtTime& o) const {
    return utc == o.utc && place == o.place && lat == o.lat && lon == o.lon;
  }
};
// False for scenes without a sky.
bool artFollows(int index);
// The moment to draw for: every 5 minutes while the light changes, else every 10.
uint32_t artStep(const ArtTime& now);

// Calls row once per line, top to bottom, with w * 3 bytes r, g, b.
// False when memory is short. May take over a second: not on a busy loop.
bool artDraw(int index, int w, int h, const ArtTime& at, void (*row)(void* ctx, int y, const uint8_t* rgb), void* ctx);

// What the moving details need of a drawn scene, in pixels.
struct ArtLive {
  float night;  // 0..1, how much the stars show
  int skyEnd;   // stars only above this row
  int horizon;  // water line, -1 = no water
  int bodies;
  struct { float x, y, path; } body[2];  // path: strength of its glitter on the water
};
// Height of the sun in degrees (without a place: up from 7 to 19 local time).
float artSunHeight(const ArtTime& at);
bool artLive(int index, int w, int h, const ArtTime& at, ArtLive& out);

}  // namespace deck

#endif  // DECK_ART_H
