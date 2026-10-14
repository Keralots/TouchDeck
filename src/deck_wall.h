// Guition deck: wallpaper for the Glass theme. The portal crops the owner's
// picture to the screen in the browser and uploads it as raw RGB888, one file
// per orientation. On load the picture goes to PSRAM with a blurred copy that
// the glass tiles show through.
#ifndef DECK_WALL_H
#define DECK_WALL_H

#include <Arduino.h>
#include <lvgl.h>

namespace deck {

// Where the wallpaper for a w x h screen lives on LittleFS.
const char* wallPath(int w, int h);
// Load the w x h wallpaper (a built-in gradient when there is no file),
// darken it by dimPct and blur a copy by blurR px (0 = an unblurred copy).
// False only when PSRAM is short.
bool wallLoad(int w, int h, int blurR, int dimPct);
void wallFree();
// Changes whenever the images are freed or reloaded, including failed loads.
uint32_t wallGeneration();
// The picture (RGB565, dimmed for the text on it) and its blurred copy
// (RGB888, so the glass keeps its precision until wallDither565);
// nullptr when not loaded.
const lv_image_dsc_t* wallSharp();
const lv_image_dsc_t* wallBlur();
// An 8-bit colour as RGB565, ordered-dithered by its screen position.
uint16_t wallDither565(int r, int g, int b, int x, int y);

}  // namespace deck

#endif  // DECK_WALL_H
