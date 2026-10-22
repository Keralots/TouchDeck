// Idle photo frame: loop owns LVGL and buffers; the worker only decodes JPEGs.
#include "deck_ui_int.h"
#include <esp_random.h>

namespace deckui {
namespace {

enum FrameState : uint8_t { FRAME_OFF, FRAME_LOADING, FRAME_ENTER, FRAME_IDLE, FRAME_CHANGE, FRAME_EXIT };
FrameState s_state = FRAME_OFF;
uint16_t *s_page = nullptr, *s_base = nullptr, *s_picture = nullptr, *s_next = nullptr;
FrameRect* s_rects = nullptr;
FrameTileRun* s_runs = nullptr;
uint16_t s_rectCount = 0;
int s_width = 0, s_height = 0;
lv_obj_t *s_root = nullptr, *s_clockBox = nullptr, *s_clockTime = nullptr, *s_clockDate = nullptr;
lv_image_dsc_t s_pictureDsc = {};
lv_draw_buf_t s_clockBuf = {};
uint32_t* s_clockPixels = nullptr;
uint32_t s_changed = 0, s_clockCheck = 0, s_retry = 0, s_epoch = 0, s_galleryCheck = 0;
bool s_bench = false, s_tried = false, s_nextNow = false, s_daily = false, s_night = false;
String s_name, s_nextName, s_error;
std::vector<String> s_shown;
PanelStats s_lastStats = {};
const char* s_animation = "none";
const char* s_lastAnimation = "none";
bool s_lastCancelled = false;

portMUX_TYPE s_loadMux = portMUX_INITIALIZER_UNLOCKED;
enum LoadState : uint8_t { LOAD_IDLE, LOAD_RUNNING, LOAD_READY };
LoadState s_loadState = LOAD_IDLE;
bool s_loadCancelled = false;
struct LoadJob {
  char path[64];
  int w, h;
  uint32_t epoch;
  uint16_t* pixels;
  bool ok;
};
LoadJob s_load = {};

LoadState loadState() {
  portENTER_CRITICAL(&s_loadMux);
  const LoadState state = s_loadState;
  portEXIT_CRITICAL(&s_loadMux);
  return state;
}

void loadTask(void*) {
  portENTER_CRITICAL(&s_loadMux);
  LoadJob job = s_load;
  portEXIT_CRITICAL(&s_loadMux);
  job.pixels = (uint16_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
  job.ok = wallLoadPicture(job.path, job.w, job.h, job.pixels);
  portENTER_CRITICAL(&s_loadMux);
  const bool cancelled = s_loadCancelled;
  if (cancelled) {
    s_load = {};
    s_loadState = LOAD_IDLE;
  } else {
    s_load = job;
    s_loadState = LOAD_READY;
  }
  portEXIT_CRITICAL(&s_loadMux);
  if (cancelled) free(job.pixels);
  vTaskDelete(nullptr);
}

bool requestPicture() {
  if (loadState() != LOAD_IDLE || s_next) return false;
  const auto files = wallFiles(s_width < s_height);
  if (files.empty()) { s_error = "empty gallery"; return false; }
  size_t index = 0;
  if (dispSettings.wallOrder && files.size() > 1) {
    std::vector<size_t> choices;
    for (size_t i = 0; i < files.size(); i++)
      if (std::find(s_shown.begin(), s_shown.end(), files[i].name) == s_shown.end()) choices.push_back(i);
    if (choices.empty()) {
      s_shown.assign(1, s_name);
      for (size_t i = 0; i < files.size(); i++)
        if (files[i].name != s_name) choices.push_back(i);
    }
    index = choices[esp_random() % choices.size()];
  } else {
    for (size_t i = 0; i < files.size(); i++)
      if (files[i].name > s_name) { index = i; break; }
  }
  s_nextName = files[index].name;
  LoadJob job = {};
  strlcpy(job.path, wallGalleryPath(s_width < s_height, s_nextName).c_str(), sizeof(job.path));
  job.w = s_width;
  job.h = s_height;
  job.epoch = s_epoch;
  portENTER_CRITICAL(&s_loadMux);
  s_load = job;
  s_loadState = LOAD_RUNNING;
  s_loadCancelled = false;
  portEXIT_CRITICAL(&s_loadMux);
  // Flash reads require an internal stack, as in wallTask.
  if (xTaskCreatePinnedToCore(loadTask, "deck_frame", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    portENTER_CRITICAL(&s_loadMux);
    s_loadState = LOAD_IDLE;
    portEXIT_CRITICAL(&s_loadMux);
    s_error = "worker allocation failed";
    return false;
  }
  return true;
}

void cancelPicture() {
  uint16_t* pixels = nullptr;
  portENTER_CRITICAL(&s_loadMux);
  s_loadCancelled = true;
  if (s_loadState == LOAD_READY) {
    pixels = s_load.pixels;
    s_load = {};
    s_loadState = LOAD_IDLE;
  }
  portEXIT_CRITICAL(&s_loadMux);
  free(pixels);
}

void pollPicture() {
  if (loadState() != LOAD_READY) return;
  portENTER_CRITICAL(&s_loadMux);
  const LoadJob job = s_load;
  s_load = {};
  s_loadState = LOAD_IDLE;
  portEXIT_CRITICAL(&s_loadMux);
  if (job.epoch == s_epoch && s_state != FRAME_OFF && job.ok) {
    s_next = job.pixels;
    s_error = "";
  } else {
    free(job.pixels);
    if (job.epoch == s_epoch && !job.ok) {
      s_error = "picture decode failed";
      s_retry = millis();
    }
  }
}

Src source(const uint16_t* pixels) { return {pixels, (uint16_t)s_width, true, (uint16_t)s_height}; }

void updateClock(bool force) {
  if (!s_clockBox) return;
  showObj(s_clockBox, presenceSettings.frameClock);
  if (!force && millis() - s_clockCheck < 1000) return;
  s_clockCheck = millis();
  const time_t epoch = time(nullptr);
  char clock[16] = "--:--", date[32] = "";
  if (epoch >= 1609459200) {
    struct tm lt;
    localtime_r(&epoch, &lt);
    if (netSettings.use24h) snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    else snprintf(clock, sizeof(clock), "%d:%02d %s", (lt.tm_hour + 11) % 12 + 1, lt.tm_min,
                  lt.tm_hour < 12 ? "AM" : "PM");
    strftime(date, sizeof(date), "%a %d %b", &lt);
  }
  setText(s_clockTime, clock);
  setText(s_clockDate, date);
}

void installPicture() {
  lv_image_cache_drop(&s_pictureDsc);
  s_pictureDsc = {};
  s_pictureDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_pictureDsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_pictureDsc.header.w = s_width;
  s_pictureDsc.header.h = s_height;
  s_pictureDsc.header.stride = s_width * 2;
  s_pictureDsc.data_size = FRAME_BYTES;
  s_pictureDsc.data = (const uint8_t*)s_picture;
  lv_obj_set_style_bg_image_src(s_root, &s_pictureDsc, 0);
  lv_obj_invalidate(s_root);
}

void makeScreen() {
  s_root = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_root);
  lv_obj_set_size(s_root, s_width, s_height);
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  installPicture();
  s_clockBox = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_clockBox);
  lv_obj_remove_flag(s_clockBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(s_clockBox, 12, s_height - 78);
  lv_obj_set_size(s_clockBox, 220, 64);
  lv_obj_set_style_bg_color(s_clockBox, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_color(s_clockBox, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_clockBox, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_main_opa(s_clockBox, 160, 0);
  lv_obj_set_style_bg_grad_opa(s_clockBox, 0, 0);
  lv_obj_set_style_bg_grad_dir(s_clockBox, LV_GRAD_DIR_HOR, 0);
  s_clockTime = lv_label_create(s_clockBox);
  lv_obj_set_pos(s_clockTime, 10, 6);
  lv_obj_set_style_text_color(s_clockTime, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_clockTime, &deck_font_20, 0);
  s_clockDate = lv_label_create(s_clockBox);
  lv_obj_set_pos(s_clockDate, 10, 38);
  lv_obj_set_style_text_color(s_clockDate, lv_color_white(), 0);
  lv_obj_set_style_text_font(s_clockDate, &deck_font_14, 0);
  updateClock(true);
}

bool capturePage() {
  s_page = (uint16_t*)heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM);
  s_base = (uint16_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
  Page& page = s_pages[sh.active];
  const int capacity = lv_obj_get_child_count(page.grid) + 1;
  s_rects = (FrameRect*)heap_caps_malloc(capacity * sizeof(FrameRect), MALLOC_CAP_SPIRAM);
  s_runs = (FrameTileRun*)heap_caps_malloc(capacity * sizeof(FrameTileRun), MALLOC_CAP_SPIRAM);
  if (!s_page || !s_base || !s_rects || !s_runs) return false;
  lv_obj_update_layout(page.root);
  lv_draw_buf_t buf;
  if (lv_draw_buf_init(&buf, s_width, s_height, LV_COLOR_FORMAT_RGB565, s_width * 2,
                       s_page, FRAME_BYTES) != LV_RESULT_OK ||
      lv_snapshot_take_to_draw_buf(page.root, LV_COLOR_FORMAT_RGB565, &buf) != LV_RESULT_OK) return false;
  const lv_image_dsc_t* sharp = s_theme == THEME_GLASS ? wallSharp() : nullptr;
  const uint16_t color = lv_color_to_u16(lv_color_hex(C_BG));
  for (int y = 0; y < s_height; y++) {
    uint16_t* row = s_base + y * s_width;
    if (sharp) memcpy(row, sharp->data + y * sharp->header.stride, s_width * 2);
    else std::fill(row, row + s_width, color);
  }
  s_rectCount = 0;
  if (s_header) s_rects[s_rectCount++] = {0, 0, (int16_t)s_width, HEADER_H};
  for (uint32_t i = 0; i < lv_obj_get_child_count(page.grid); i++) {
    lv_obj_t* tile = lv_obj_get_child(page.grid, i);
    if (lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    const int x = std::max<int>(0, a.x1), y = std::max<int>(0, a.y1);
    const int x2 = std::min<int>(s_width - 1, a.x2), y2 = std::min<int>(s_height - 1, a.y2);
    if (x2 >= x && y2 >= y) s_rects[s_rectCount++] = {(int16_t)x, (int16_t)y, (int16_t)(x2 - x + 1), (int16_t)(y2 - y + 1)};
  }
  std::sort(s_rects, s_rects + s_rectCount, [](const FrameRect& a, const FrameRect& b) {
    return a.y == b.y ? a.x < b.x : a.y < b.y;
  });
  return true;
}

void startAnimation(FrameState state) {
  panelQuiesce();
  updateClock(true);
  free(s_clockPixels);
  s_clockPixels = presenceSettings.frameClock ?
      (uint32_t*)heap_caps_aligned_alloc(64, 220 * 64 * 4, MALLOC_CAP_SPIRAM) : nullptr;
  if (s_clockPixels &&
      (lv_draw_buf_init(&s_clockBuf, 220, 64, LV_COLOR_FORMAT_ARGB8888, 220 * 4,
                        s_clockPixels, 220 * 64 * 4) != LV_RESULT_OK ||
       lv_snapshot_take_to_draw_buf(s_clockBox, LV_COLOR_FORMAT_ARGB8888, &s_clockBuf) != LV_RESULT_OK)) {
    free(s_clockPixels);
    s_clockPixels = nullptr;
  }
  FrameScene scene = {};
  scene.base = source(state == FRAME_CHANGE ? s_picture : s_base);
  scene.picture = source(state == FRAME_CHANGE ? s_next : s_picture);
  scene.page = source(s_page);
  scene.waking = state == FRAME_EXIT;
  scene.clockFade = state != FRAME_CHANGE;
  scene.rects = s_rects;
  scene.runs = s_runs;
  scene.count = state == FRAME_CHANGE ? 0 : s_rectCount;
  if (s_clockPixels) {
    scene.clock = s_clockPixels;
    scene.clockStride = s_clockBuf.header.stride / 4;
    scene.clockRect = {12, (int16_t)(s_height - 78), 220, 64};
  }
  const uint32_t duration = state == FRAME_CHANGE ? 1000 : presenceSettings.frameAnim ?
                            frameTilesMs(s_rectCount) + FRAME_FADE_MS : 0;
  compBeginFrame(scene, duration);
  s_state = state;
  s_animation = state == FRAME_ENTER ? "enter" : state == FRAME_EXIT ? "exit" : "next";
}

void recordAnimation(const PanelStats& stats, bool cancelled) {
  s_lastStats = stats;
  s_lastAnimation = s_animation;
  s_lastCancelled = cancelled;
  Serial.printf("Deck frame: %s%s frames=%u avg=%uus max=%uus gap=%uus\n", s_lastAnimation,
                cancelled ? " cancelled" : "", (unsigned)stats.frames, (unsigned)stats.frameUsAvg,
                (unsigned)stats.frameUsMax, (unsigned)stats.gapMaxUs);
}

void shown() {
  s_name = s_nextName;
  if (std::find(s_shown.begin(), s_shown.end(), s_name) == s_shown.end()) s_shown.push_back(s_name);
  s_changed = millis();
  s_nextNow = false;
}

bool enter() {
  if (s_state != FRAME_OFF || !s_disp || !s_count || getScreenState() != SCREEN_DECK ||
      s_qp.open || s_modalCard || s_ed.root || sh.owned || compMode() != COMP_NONE ||
      touchIsDown() || manualDisplayOff() || nightBrightnessActive()) return false;
  stopGestures();
  s_width = logicalW();
  s_height = logicalH();
  s_daily = wallDaily();
  s_galleryCheck = millis();
  s_error = "";
  s_name = "";
  s_shown.clear();
  ++s_epoch;
  s_state = FRAME_LOADING;
  s_retry = 0;
  s_tried = true;
  portENTER_CRITICAL(&s_mux);
  sh.swipeEnabled = false;
  portEXIT_CRITICAL(&s_mux);
  setIdleSleep(true, 255);
  if (loadState() == LOAD_IDLE && !requestPicture()) {
    frameAbort();
    setIdleSleep(true, 0);
    return false;
  }
  return true;
}

}  // namespace

bool frameActive() { return s_state != FRAME_OFF; }

void frameAbort() {
  if (!frameActive()) return;
  panelQuiesce();
  PanelStats stats;
  if (compFrameResult(stats)) recordAnimation(stats, true);
  ++s_epoch;
  cancelPicture();  // A running decoder frees its own output when it finishes.
  if (s_root) lv_obj_delete(s_root);
  s_root = s_clockBox = s_clockTime = s_clockDate = nullptr;
  lv_image_cache_drop(&s_pictureDsc);
  free(s_clockPixels);
  s_clockPixels = nullptr;
  free(s_page); free(s_base); free(s_picture); free(s_next); free(s_rects); free(s_runs);
  s_page = s_base = s_picture = s_next = nullptr;
  s_rects = nullptr;
  s_runs = nullptr;
  s_rectCount = 0;
  s_state = FRAME_OFF;
  s_bench = s_nextNow = false;
  for (int i = 0; i < s_count; i++) s_pages[i].stale = true;
  lv_obj_invalidate(lv_screen_active());
  ++s_gen;
}

bool frameSleep(bool sleep) {
  const bool night = nightBrightnessActive();
  if (!sleep || night != s_night) s_tried = false;
  s_night = night;
  if (frameActive()) {
    if (!sleep && !s_bench) setIdleSleep(false, 0);
    return true;
  }
  if (!presenceSettings.frame || !sleep) return false;
  if (!s_tried) enter();
  if (!frameActive()) setIdleSleep(true, 0);
  return true;
}

void frameTick() {
  pollPicture();
  if (!frameActive()) return;
  if (getScreenState() != SCREEN_DECK || manualDisplayOff() || nightBrightnessActive() ||
      s_daily != wallDaily() || s_width != logicalW() || s_height != logicalH() ||
      s_qp.open || s_modalCard || s_ed.root) {
    frameAbort();
    setIdleSleep(getScreenState() == SCREEN_DECK, 0);
    return;
  }
  if (s_state == FRAME_LOADING) {
    if (!idleSleepActive()) { frameAbort(); return; }
    if (!s_next) {
      if (loadState() == LOAD_IDLE && (s_retry || !requestPicture())) {
        frameAbort();
        setIdleSleep(true, 0);
      }
      return;
    }
    if (!capturePage()) {
      s_error = "snapshot allocation failed";
      frameAbort();
      setIdleSleep(true, 0);
      return;
    }
    s_picture = s_next;
    s_next = nullptr;
    shown();
    makeScreen();
    startAnimation(FRAME_ENTER);
  }
  if (s_state == FRAME_ENTER || s_state == FRAME_EXIT || s_state == FRAME_CHANGE) {
    PanelStats stats;
    if (!compFrameResult(stats)) return;
    recordAnimation(stats, false);
    panelQuiesce();
    if (s_state == FRAME_EXIT) { frameAbort(); return; }
    if (s_state == FRAME_CHANGE) {
      lv_image_cache_drop(&s_pictureDsc);
      free(s_picture);
      s_picture = s_next;
      s_next = nullptr;
      installPicture();
      shown();
    }
    s_state = FRAME_IDLE;
  }
  if (s_state != FRAME_IDLE) return;
  if (!idleSleepActive()) { startAnimation(FRAME_EXIT); return; }
  updateClock(false);
  const uint32_t now = millis();
  if (now - s_galleryCheck >= 5000) {
    s_galleryCheck = now;
    if (wallFiles(s_width < s_height).empty()) {
      s_error = "empty gallery";
      frameAbort();
      setIdleSleep(true, 0);
      return;
    }
  }
  if (!s_next && loadState() == LOAD_IDLE && (!s_retry || now - s_retry >= 30000)) {
    if (!requestPicture()) {
      s_retry = now;
      if (s_error == "empty gallery") {
        frameAbort();
        setIdleSleep(true, 0);
        return;
      }
    }
  }
  if (s_next && (s_nextNow || now - s_changed >= presenceSettings.frameEvery * 60000UL)) {
    if (!LittleFS.exists(wallGalleryPath(s_width < s_height, s_nextName))) {
      free(s_next);
      s_next = nullptr;
      return;
    }
    startAnimation(FRAME_CHANGE);
  }
}

}  // namespace deckui

bool deckFrameCommand(const String& op) {
  using namespace deckui;
  if (op == "enter") {
    if (!enter()) return false;
    s_bench = true;
    return true;
  }
  if (op == "exit") {
    if (!frameActive()) return false;
    s_bench = false;
    presenceWake();
    return true;
  }
  if (op == "next" && (s_state == FRAME_IDLE || s_state == FRAME_ENTER)) {
    s_nextNow = true;
    return true;
  }
  return false;
}

void deckFrameJson(JsonObject out) {
  using namespace deckui;
  const char* names[] = {"off", "loading", "entering", "idle", "changing", "exiting"};
  out["state"] = names[s_state];
  out["picture"] = s_name;
  out["nextReady"] = s_next != nullptr;
  out["loading"] = loadState() == LOAD_RUNNING;
  out["error"] = s_error;
  out["animation"] = s_lastAnimation;
  out["cancelled"] = s_lastCancelled;
  out["frames"] = s_lastStats.frames;
  out["averageUs"] = s_lastStats.frameUsAvg;
  out["maxUs"] = s_lastStats.frameUsMax;
  out["longestGapUs"] = s_lastStats.gapMaxUs;
  out["buildAverageUs"] = s_lastStats.buildUsAvg;
  out["flushAverageUs"] = s_lastStats.flushUsAvg;
}
