// Guition deck: LVGL heap in PSRAM (lv_conf.h: LV_USE_STDLIB_MALLOC =
// LV_STDLIB_CUSTOM). With the C library allocator every widget, style and
// draw task under 4 KB landed in internal RAM, which WiFi, the portal, the
// Home Assistant socket and the display's DMA staging buffers also need.
// Frame buffers were already in PSRAM, so the render cost barely moves.
#if defined(BOARD_IS_JC3248W535)

#include <esp_heap_caps.h>
#include <lvgl.h>

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

// PSRAM first, internal RAM only if PSRAM is exhausted.
void* lv_malloc_core(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* p, size_t new_size) {
  void* n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

#endif  // BOARD_IS_JC3248W535
