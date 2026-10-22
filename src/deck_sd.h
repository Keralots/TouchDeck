// Optional gallery storage, mounted once at boot.
#ifndef DECK_SD_H
#define DECK_SD_H

#include <FS.h>
#include <esp_heap_caps.h>
#include <vector>

namespace deck {

template <typename T> struct WallAllocator {
  using value_type = T;
  WallAllocator() = default;
  template <typename U> WallAllocator(const WallAllocator<U>&) {}
  T* allocate(size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) abort();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t) { heap_caps_free(p); }
  template <typename U> bool operator==(const WallAllocator<U>&) const { return true; }
  template <typename U> bool operator!=(const WallAllocator<U>&) const { return false; }
};
template <typename T> using WallVector = std::vector<T, WallAllocator<T>>;

void wallStorageBegin();
bool wallOnSd();
// Last full index rebuild, ms.
uint32_t wallIndexMs();
fs::FS& wallFs();
const char* wallRoot();
const char* wallTemp(bool daily);
uint64_t wallFreeBytes();
uint64_t wallTotalBytes();

}  // namespace deck
#endif  // DECK_SD_H
