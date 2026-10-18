// ArduinoJson allocator backed by PSRAM. Documents grow in small blocks, and
// blocks under 4 KB land in internal RAM, which WiFi needs to stay alive.
#ifndef PSRAM_JSON_H
#define PSRAM_JSON_H

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

inline ArduinoJson::Allocator* psramJson() {
  struct Allocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
  };
  static Allocator allocator;
  return &allocator;
}

#endif  // PSRAM_JSON_H
