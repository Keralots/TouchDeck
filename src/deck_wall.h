// Guition deck: wallpaper for the Glass theme. The portal crops the owner's
// picture to the screen in the browser and uploads it as raw RGB565 (LVGL's
// byte order), one file per orientation. On load the picture goes to PSRAM
// with a blurred copy that the glass tiles show through.
#ifndef DECK_WALL_H
#define DECK_WALL_H

#include <Arduino.h>
#include <lvgl.h>

namespace deck {

// Where the wallpaper for a w x h screen lives on LittleFS.
const char* wallPath(int w, int h);
// Load the w x h wallpaper (a built-in gradient when there is no file) and
// blur a copy. False only when PSRAM is short.
bool wallLoad(int w, int h);
void wallFree();
// The picture (a little dimmed for the text on it) and its blurred copy;
// nullptr when not loaded.
const lv_image_dsc_t* wallSharp();
const lv_image_dsc_t* wallBlur();

}  // namespace deck

#endif  // DECK_WALL_H
