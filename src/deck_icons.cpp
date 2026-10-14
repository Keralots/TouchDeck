// Guition deck: MDI icon font. See deck_icons.h.
#include "deck_icons.h"
#include "deck_config.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <algorithm>
#include <vector>

namespace deck {
namespace {

constexpr size_t MAX_FILE = 3 * 1024 * 1024;
constexpr size_t MAX_ICONS = 20000;
constexpr int MAX_FONTS = 4;

// name = offset of the glyph's Pascal string inside the font data.
struct Entry {
  uint32_t name;
  uint32_t cp;
};

uint8_t* s_names = nullptr;  // the indexed names as Pascal strings
size_t s_size = 0;
Entry* s_index = nullptr;
size_t s_count = 0;
lv_fs_drv_t s_fsDrv;
bool s_fsReg = false;
struct Sized {
  int px;
  lv_font_t* font;
};
Sized s_fonts[MAX_FONTS] = {};

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Pascal string at off versus (s, n), memcmp order.
int cmpName(const uint8_t* data, uint32_t off, const char* s, size_t n) {
  const size_t len = data[off];
  const int c = memcmp(data + off + 1, s, std::min(len, n));
  if (c) return c;
  return len < n ? -1 : (len > n ? 1 : 0);
}

bool findTable(const uint8_t* d, size_t size, const char* tag, uint32_t& off, uint32_t& len) {
  if (size < 12) return false;
  const uint16_t n = be16(d + 4);
  if (12 + 16u * n > size) return false;
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t* r = d + 12 + 16 * i;
    if (memcmp(r, tag, 4)) continue;
    off = be32(r + 8);
    len = be32(r + 12);
    return off < size && len <= size - off;
  }
  return false;
}

// Builds the sorted name index of font data d. Returns nullptr with err set
// when the file is not an MDI-style font (needs post v2 names + cmap format 12).
Entry* buildIndex(const uint8_t* d, size_t size, size_t& count, String& err) {
  count = 0;
  uint32_t po, pl, co, cl;
  if (size < 12 || (be32(d) != 0x00010000 && memcmp(d, "true", 4))) {
    err = "not a TrueType font";
    return nullptr;
  }
  if (!findTable(d, size, "post", po, pl) || pl < 34 || be32(d + po) != 0x00020000 ||
      !findTable(d, size, "cmap", co, cl) || cl < 4) {
    err = "font has no glyph names (post v2) or no cmap";
    return nullptr;
  }
  const uint16_t glyphs = be16(d + po + 32);
  const uint32_t idxOff = po + 34;
  if (idxOff + 2u * glyphs > po + pl) {
    err = "broken post table";
    return nullptr;
  }
  // Offsets of the custom names, in order.
  std::vector<uint32_t> custom;
  for (uint32_t p = idxOff + 2u * glyphs; p < po + pl && p + 1 + d[p] <= po + pl; p += 1 + d[p])
    custom.push_back(p);

  // The Unicode full-repertoire subtable (format 12) carries the U+F0000+ icons.
  const uint8_t* groups = nullptr;
  uint32_t nGroups = 0;
  const uint16_t subs = be16(d + co + 2);
  for (uint16_t i = 0; i < subs && 4 + 8u * (i + 1) <= cl; i++) {
    const uint8_t* r = d + co + 4 + 8 * i;
    const uint32_t so = co + be32(r + 4);
    if (so + 16 > co + cl || be16(d + so) != 12) continue;
    const uint32_t n = be32(d + so + 12);
    if (so + 16 + 12ull * n > co + cl) continue;
    groups = d + so + 16;
    nGroups = n;
    break;
  }
  if (!groups) {
    err = "font has no format 12 cmap";
    return nullptr;
  }

  auto nameOf = [&](uint32_t gid) -> int64_t {
    if (gid >= glyphs) return -1;
    const uint16_t ni = be16(d + idxOff + 2 * gid);
    if (ni < 258 || ni - 258u >= custom.size()) return -1;
    return custom[ni - 258];
  };
  size_t n = 0;
  for (int pass = 0; pass < 2; pass++) {
    Entry* out = pass ? (Entry*)heap_caps_malloc(n * sizeof(Entry), MALLOC_CAP_SPIRAM) : nullptr;
    if (pass && !out) {
      err = "out of memory";
      return nullptr;
    }
    size_t k = 0;
    for (uint32_t g = 0; g < nGroups; g++) {
      const uint8_t* gr = groups + 12 * g;
      const uint32_t start = be32(gr), end = be32(gr + 4), gid = be32(gr + 8);
      for (uint32_t cp = start; cp <= end && k < MAX_ICONS; cp++) {
        const int64_t name = nameOf(gid + (cp - start));
        if (name < 0 || d[name] == 0) continue;
        if (pass) out[k] = {(uint32_t)name, cp};
        k++;
      }
    }
    if (!pass) {
      n = k;
      if (n < 1) {
        err = "font has no named icons";
        return nullptr;
      }
      continue;
    }
    std::sort(out, out + n, [d](const Entry& a, const Entry& b) {
      return cmpName(d, a.name, (const char*)d + b.name + 1, d[b.name]) < 0;
    });
    count = n;
    return out;
  }
  return nullptr;
}

void freeFonts() {
  for (Sized& s : s_fonts) {
    if (s.font) lv_tiny_ttf_destroy(s.font);
    s = {};
  }
}

// LVGL drive "I:" = LittleFS, read only, for TinyTTF's file mode.
void* fsOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD) return nullptr;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  // stdio would buffer a whole 4 KB block in internal RAM per open font;
  // LVGL's read cache (s_fsDrv.cache_size, PSRAM) sits in front anyway.
  f.setBufferSize(128);
  return new File(f);
}
lv_fs_res_t fsClose(lv_fs_drv_t*, void* f) {
  static_cast<File*>(f)->close();
  delete static_cast<File*>(f);
  return LV_FS_RES_OK;
}
lv_fs_res_t fsRead(lv_fs_drv_t*, void* f, void* buf, uint32_t n, uint32_t* got) {
  const int r = static_cast<File*>(f)->read((uint8_t*)buf, n);
  *got = r > 0 ? r : 0;
  return r < 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}
lv_fs_res_t fsSeek(lv_fs_drv_t*, void* f, uint32_t pos, lv_fs_whence_t w) {
  const SeekMode m = w == LV_FS_SEEK_CUR ? SeekCur : w == LV_FS_SEEK_END ? SeekEnd : SeekSet;
  return static_cast<File*>(f)->seek(pos, m) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}
lv_fs_res_t fsTell(lv_fs_drv_t*, void* f, uint32_t* pos) {
  *pos = static_cast<File*>(f)->position();
  return LV_FS_RES_OK;
}

void registerFs() {
  if (s_fsReg) return;
  lv_fs_drv_init(&s_fsDrv);
  s_fsDrv.letter = 'I';
  s_fsDrv.cache_size = 256;
  s_fsDrv.open_cb = fsOpen;
  s_fsDrv.close_cb = fsClose;
  s_fsDrv.read_cb = fsRead;
  s_fsDrv.seek_cb = fsSeek;
  s_fsDrv.tell_cb = fsTell;
  lv_fs_drv_register(&s_fsDrv);
  s_fsReg = true;
}

// Copies the indexed names out of the font data, so the data can go.
uint8_t* packNames(const uint8_t* d, Entry* index, size_t count) {
  size_t total = 0;
  for (size_t i = 0; i < count; i++) total += 1 + d[index[i].name];
  uint8_t* pool = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
  if (!pool) return nullptr;
  size_t at = 0;
  for (size_t i = 0; i < count; i++) {
    const size_t n = 1 + d[index[i].name];
    memcpy(pool + at, d + index[i].name, n);
    index[i].name = at;
    at += n;
  }
  return pool;
}

}  // namespace

const char* iconsPath() { return "/mdi.ttf"; }

bool iconsLoad(const char* path, String& err) {
  if (!deckFsReady() || !LittleFS.exists(path)) {
    err = "no icon font on the device";
    return false;
  }
  File f = LittleFS.open(path, "r");
  const size_t size = f ? f.size() : 0;
  if (!size || size > MAX_FILE) {
    if (f) f.close();
    err = size ? "file too big" : "cannot read the file";
    return false;
  }
  uint8_t* data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!data) {
    f.close();
    err = "out of memory";
    return false;
  }
  size_t got = 0;
  while (got < size) {
    const int r = f.read(data + got, std::min<size_t>(32768, size - got));
    if (r <= 0) break;
    got += r;
    esp_task_wdt_reset();
  }
  f.close();
  size_t count = 0;
  Entry* index = got == size ? buildIndex(data, size, count, err) : nullptr;
  if (got != size) err = "short read";
  uint8_t* names = index ? packNames(data, index, count) : nullptr;
  heap_caps_free(data);
  if (!names) {
    if (index && !err.length()) err = "out of memory";
    heap_caps_free(index);
    return false;
  }
  freeFonts();
  heap_caps_free(s_index);
  heap_caps_free(s_names);
  s_names = names;
  s_size = size;
  s_index = index;
  s_count = count;
  registerFs();
  Serial.printf("Deck: icon font %u bytes, %u icons\n", (unsigned)size, (unsigned)count);
  return true;
}

void iconsUnload() {
  freeFonts();
  heap_caps_free(s_index);
  heap_caps_free(s_names);
  s_index = nullptr;
  s_names = nullptr;
  s_count = 0;
  s_size = 0;
}

bool iconsReady() { return s_index != nullptr; }
size_t iconsCount() { return s_count; }
size_t iconsFileSize() { return s_size; }

uint32_t iconCodepoint(const String& name) {
  if (!s_index) return 0;
  const char* s = name.c_str();
  size_t n = name.length();
  if (name.startsWith("mdi:")) {
    s += 4;
    n -= 4;
  } else if (name.indexOf(':') >= 0) {
    return 0;  // another icon set (hass:, custom) - not in this font
  }
  size_t lo = 0, hi = s_count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = cmpName(s_names, s_index[mid].name, s, n);
    if (c == 0) return s_index[mid].cp;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

const lv_font_t* iconFont(int px) {
  if (!s_index) return nullptr;
  for (const Sized& s : s_fonts)
    if (s.font && s.px == px) return s.font;
  for (Sized& s : s_fonts) {
    if (s.font) continue;
    s.font = lv_tiny_ttf_create_file_ex((String("I:") + iconsPath()).c_str(), px, LV_FONT_KERNING_NONE, 64);
    s.px = px;
    return s.font;
  }
  return nullptr;  // all size slots taken: a caller asked for too many sizes
}

void iconUtf8(uint32_t cp, char out[5]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    out[1] = 0;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | cp >> 6);
    out[1] = (char)(0x80 | (cp & 0x3F));
    out[2] = 0;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = 0;
  } else {
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = 0;
  }
}

}  // namespace deck
