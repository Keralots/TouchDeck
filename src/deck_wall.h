// Glass wallpaper galleries and PSRAM image loading.
#ifndef DECK_WALL_H
#define DECK_WALL_H

#include <Arduino.h>
#include <lvgl.h>
#include "deck_sd.h"

namespace deck {

constexpr uint32_t WALL_IDLE_MS = 30000;
constexpr size_t WALL_RESERVE = 256 * 1024;
constexpr size_t WALL_MAX_JPEG = 200 * 1024;
struct WallFile {
  uint64_t id;
  size_t size;
  uint32_t date;
  bool qualified = false;
  String name() const;
};
using WallFiles = WallVector<WallFile>;
uint64_t wallId(const String& name);
bool wallValidRef(const String& name);
void wallRebuildIndex();
void wallIndexChanged(bool portrait, bool daily, const String& name);
bool wallRemoveFile(bool portrait, bool daily, const String& name, bool protect = false);
// Slots: wallpaper, queued wallpaper, frame, queued frame, three source pins, pending pin.
void wallProtect(bool portrait, int slot, const String& name);
bool wallValidName(const String& name);
// Galleries: own uploads or daily downloads.
const char* wallDir(bool portrait, bool daily);
bool wallDaily();
// Loop task only, after dispSettings.wallSource changed.
uint8_t wallSource();
void wallSetSource(uint8_t source);
String wallGalleryPath(bool portrait, const String& name = "");
WallFiles wallFiles(bool portrait);
WallFiles wallListDir(const char* path);
String wallCurrent(bool portrait);
String wallPinned(bool portrait);
bool wallPin(bool portrait, const String& name);
void wallClearPin(bool portrait);
bool wallValidateJpeg(const char* path, bool portrait);
// Worker from wallRunWorker only. Undimmed RGB565, no full-size scratch.
bool wallLoadPicture(const char* path, int w, int h, uint16_t* pixels);
// Runs a picture job off the loop task. SD jobs share one long-lived worker with
// a PSRAM stack; LittleFS jobs get a short-lived task with an internal stack.
bool wallRunWorker(void (*job)(), bool onSd, const char* name);
// Loop task only. Requests replace older jobs without touching their buffers.
void wallRequest(bool portrait, const String& name);
bool wallNext(bool portrait);
void wallGalleryChanged(bool portrait);
void wallInvalidateJobs();
// Poll/start work, then commit only after the UI has stopped gestures.
bool wallPoll(int w, int h, bool idle, bool canSwitch);
void wallCommit();
// The automatic change interval starts again (the photo frame covered the wallpaper).
void wallRestartTimer();

// Where the wallpaper for a w x h screen lives on LittleFS.
const char* wallPath(int w, int h);
// Load the w x h wallpaper (a built-in gradient when there is no file),
// darken it by dimPct and blur a copy by blurR px (0 = an unblurred copy).
// False when memory is short or the selected JPEG cannot be decoded.
bool wallLoad(int w, int h, int blurR, int dimPct);
void wallFree();
// Changes whenever the images are freed or reloaded, including failed loads.
uint32_t wallGeneration();
// The picture (RGB565, dimmed for the text on it) and its blurred copy
// (RGB888, so the glass keeps its precision until wallDither565);
// nullptr when not loaded.
const lv_image_dsc_t* wallSharp();
const lv_image_dsc_t* wallBlur();
// Near-gaussian blur of one 8-bit plane in place (tmp: scratch of the same size).
void wallBlurPlane(uint8_t* p, uint8_t* tmp, int w, int h, int r);
// An 8-bit colour as RGB565, ordered-dithered by its screen position. 8x8
// ordered dither: a smooth 8-bit gradient becomes a fine pattern of
// neighbouring RGB565 levels instead of coloured steps. Inline: per pixel loops.
inline uint16_t wallDither565(int r, int g, int b, int x, int y) {
  static const uint8_t kBayer[8][8] = {
      {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
      {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
      {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
  const int t = (kBayer[y & 7][x & 7] * 2 + 1) * 255;  // threshold, in 1/32640 of a level
  int r5 = (r * 31 * 128 + t) / 32640, g6 = (g * 63 * 128 + t) / 32640, b5 = (b * 31 * 128 + t) / 32640;
  if (r5 > 31) r5 = 31;
  if (g6 > 63) g6 = 63;
  if (b5 > 31) b5 = 31;
  return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

}  // namespace deck

#endif  // DECK_WALL_H
