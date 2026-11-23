// Glass wallpaper galleries and PSRAM image loading.
#ifndef DECK_WALL_H
#define DECK_WALL_H

#include <Arduino.h>
#include <lvgl.h>
#include "deck_sd.h"
#include "board.h"
#include "deck_blue_noise.h"

namespace deck {

constexpr uint32_t WALL_IDLE_MS = 30000;
constexpr size_t WALL_RESERVE = 256 * 1024;
// 200 KB for a 320x480 screen, scaled with the screen area.
constexpr size_t WALL_MAX_JPEG = (size_t)(200ull * 1024 * BOARD_PANEL_W * BOARD_PANEL_H / (320 * 480));
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
// "b/0000000N.jpg" = built-in picture N, -1 for any other name.
int wallArtIndex(const String& name);
bool wallExists(bool portrait, const String& name);
void wallRebuildIndex();
void wallIndexChanged(bool portrait, bool daily, const String& name);
bool wallRemoveFile(bool portrait, bool daily, const String& name, bool protect = false);
// Slots: wallpaper, queued wallpaper, frame, queued frame, three source pins, pending pin.
void wallProtect(bool portrait, int slot, const String& name);
bool wallValidName(const String& name);
// Galleries: own uploads or daily downloads. Built-in pictures have no files.
const char* wallDir(bool portrait, bool daily);
bool wallDaily();
// Loop task only, after dispSettings.wallSource changed.
// 0 = own, 1 = daily, 2 = both, 3 = built-in.
uint8_t wallSource();
void wallSetSource(uint8_t source);
String wallGalleryPath(bool portrait, const String& name = "");
// The pictures of the source; the built-in ones while it has none.
WallFiles wallFiles(bool portrait);
WallFiles wallArtFiles();
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
// rotate false: no automatic change starts and a finished one is dropped.
// makeRoom: called before a job starts, to free PSRAM the UI can rebuild.
bool wallPoll(int w, int h, bool idle, bool canSwitch, bool rotate, void (*makeRoom)() = nullptr);
void wallCommit();
// The automatic change interval starts again (the photo frame covered the wallpaper).
void wallRestartTimer();

// Where the wallpaper for a w x h screen lives on LittleFS.
const char* wallPath(int w, int h);
// Load the w x h wallpaper (a plain gradient when there is nothing else),
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
// Dither threshold at (x, y), in 1/32640 of a level.
inline int wallDitherT(int x, int y) { return (BLUE_NOISE[y & 63][x & 63] * 2 + 1) * 255; }
constexpr int WALL_DITHER_ONE = 32640;

// An 8-bit colour as RGB565, dithered by its screen position: a smooth 8-bit
// gradient becomes a fine grain of neighbouring RGB565 levels instead of coloured
// steps. Green runs against red and blue, which evens out the brightness.
// Inline: per pixel loops.
inline uint16_t wallDither565(int r, int g, int b, int x, int y) {
  const int t = wallDitherT(x, y);
  int r5 = (r * 31 * 128 + t) / 32640, g6 = (g * 63 * 128 + WALL_DITHER_ONE - t) / 32640, b5 = (b * 31 * 128 + t) / 32640;
  if (r5 > 31) r5 = 31;
  if (g6 > 63) g6 = 63;
  if (b5 > 31) b5 = 31;
  return (uint16_t)(r5 << 11 | g6 << 5 | b5);
}

}  // namespace deck

#endif  // DECK_WALL_H
