// mbedTLS buffers (two 16 KB records per session) in PSRAM; small blocks stay
// internal. Installed once, for every TLS client on the device.
#ifndef TLS_PSRAM_H
#define TLS_PSRAM_H

#include <esp_heap_caps.h>
#include <stdint.h>

extern "C" int mbedtls_platform_set_calloc_free(void* (*)(size_t, size_t), void (*)(void*));

inline void* tlsPsramCalloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  if (n * size >= 1024) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

inline bool tlsPsramInstall() {
  static bool installed = false;
  if (!installed) installed = mbedtls_platform_set_calloc_free(tlsPsramCalloc, heap_caps_free) == 0;
  return installed;
}

#endif  // TLS_PSRAM_H
