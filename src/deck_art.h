// Built-in wallpapers: scenes drawn by code at any size, nothing stored.
#ifndef DECK_ART_H
#define DECK_ART_H

#include <stdint.h>

namespace deck {

constexpr int ART_COUNT = 6;
const char* artName(int index);
// Calls row once per line, top to bottom, with w * 3 bytes r, g, b.
// False when memory is short. May take over a second: not on a busy loop.
bool artDraw(int index, int w, int h, void (*row)(void* ctx, int y, const uint8_t* rgb), void* ctx);

}  // namespace deck

#endif  // DECK_ART_H
