// Guition deck: icon fonts. See deck_icons.h.
#include "deck_icons.h"
#include "deck_config.h"
#include "deck_sd.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <algorithm>
#include <vector>

namespace deck {
namespace {

constexpr size_t MAX_FILE = 3 * 1024 * 1024;
constexpr size_t MAX_ICONS = 20000;
constexpr int MAX_FONTS = 8;  // distinct icon sizes in use (6 seen)

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
Sized s_brandFonts[MAX_FONTS] = {};
lv_fs_drv_t s_brandDrv;
bool s_brandReg = false;
constexpr uint32_t BRAND_BASE = 0x100000;
const char* const BRAND_PENDING = "/touchdeck/icons/cbi.pending";

struct BrandData {
  char* names = nullptr;
  uint32_t* offsets = nullptr;
  size_t count = 0;
  size_t size = 0;
  String version;
  ~BrandData() { heap_caps_free(names); heap_caps_free(offsets); }
  void swap(BrandData& other) {
    std::swap(names, other.names);
    std::swap(offsets, other.offsets);
    std::swap(count, other.count);
    std::swap(size, other.size);
    std::swap(version, other.version);
  }
};
BrandData s_brand;
decltype(lv_font_t::get_glyph_dsc) s_mdiGlyphDsc = nullptr;

bool mdiGlyphDsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t cp, uint32_t next) {
  // TinyTTF logs missing glyphs as cache errors, even when a fallback has them.
  if (cp >= BRAND_BASE && cp < BRAND_BASE + s_brand.count) return false;
  return s_mdiGlyphDsc(font, dsc, cp, next);
}

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
  for (Sized& s : s_fonts) if (s.font) s.font->fallback = nullptr;
  for (Sized& s : s_fonts) {
    if (s.font) lv_tiny_ttf_destroy(s.font);
    s = {};
  }
  for (Sized& s : s_brandFonts) {
    if (s.font) lv_tiny_ttf_destroy(s.font);
    s = {};
  }
  s_mdiGlyphDsc = nullptr;
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
void* brandOpen(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD || !wallOnSd()) return nullptr;
  File f = wallFs().open(path, "r");
  if (!f) return nullptr;
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

void registerBrandFs() {
  if (s_brandReg) return;
  lv_fs_drv_init(&s_brandDrv);
  s_brandDrv.letter = 'B';
  s_brandDrv.cache_size = 256;
  s_brandDrv.open_cb = brandOpen;
  s_brandDrv.close_cb = fsClose;
  s_brandDrv.read_cb = fsRead;
  s_brandDrv.seek_cb = fsSeek;
  s_brandDrv.tell_cb = fsTell;
  lv_fs_drv_register(&s_brandDrv);
  s_brandReg = true;
}

uint8_t* readBrandFile(const char* path, size_t limit, size_t& size, String& err) {
  File f = wallFs().open(path, "r");
  size = f ? f.size() : 0;
  if (!size || size > limit) {
    err = "missing, empty or oversized brand file";
    return nullptr;
  }
  f.setBufferSize(128);
  uint8_t* data = (uint8_t*)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
  if (!data) { err = "out of memory"; return nullptr; }
  size_t got = 0;
  while (got < size) {
    const int n = f.read(data + got, std::min<size_t>(32768, size - got));
    if (n <= 0) break;
    got += n;
    esp_task_wdt_reset();
  }
  if (got != size) {
    heap_caps_free(data);
    err = "short read of brand file";
    return nullptr;
  }
  data[size] = 0;
  return data;
}

bool brandNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

bool validBrandName(const char* name) {
  const uint8_t* p = (const uint8_t*)name;
  while (*p) {
    if (*p < 0x80) {
      if (!brandNameChar(*p++)) return false;
      continue;
    }
    const uint8_t lead = *p++;
    const unsigned bytes = lead >= 0xC2 && lead <= 0xDF ? 2 : lead >= 0xE0 && lead <= 0xEF ? 3 :
                           lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
    if (!bytes) return false;
    uint32_t cp = lead & (0x7F >> bytes);
    for (unsigned i = 1; i < bytes; i++) {
      if ((*p & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (*p++ & 0x3F);
    }
    if (cp < 0xA0 || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  }
  return true;
}

bool readBrandNames(const char* path, BrandData& out, String& err) {
  size_t size;
  out.names = (char*)readBrandFile(path, BRAND_MAX_NAMES, size, err);
  if (!out.names) return false;
  if (memchr(out.names, 0, size) || out.names[0] != '#') return false;
  char* first = strchr(out.names, '\n');
  if (!first || first - out.names < 2 || first - out.names > 64) return false;
  *first++ = 0;
  for (const char* p = out.names + 1; *p; p++)
    if (!brandNameChar(*p) && *p != '+') return false;
  out.version = out.names + 1;
  char* end = out.names + size;
  char* prev = nullptr;
  for (char* p = first; p < end;) {
    char* next = strchr(p, '\n');
    if (next) *next++ = 0;
    else next = end;
    const size_t len = strlen(p);
    if (!len || len > 127 || (prev && strcmp(prev, p) >= 0) || ++out.count > MAX_ICONS) return false;
    if (!validBrandName(p)) return false;
    prev = p;
    p = next;
  }
  if (!out.count) return false;
  out.offsets = (uint32_t*)heap_caps_malloc(out.count * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (!out.offsets) { err = "out of memory"; return false; }
  char* p = first;
  for (size_t i = 0; i < out.count; i++) {
    out.offsets[i] = p - out.names;
    p += strlen(p) + 1;
  }
  return true;
}

bool validBrandFont(const uint8_t* d, size_t size, size_t count) {
  if (size < 12 || memcmp(d, "OTTO", 4)) return false;
  const size_t tables = be16(d + 4);
  if (!tables || tables > 64 || 12 + 16 * tables > size) return false;
  for (size_t i = 0; i < tables; i++) {
    const uint8_t* r = d + 12 + 16 * i;
    const uint32_t off = be32(r + 8), len = be32(r + 12);
    if (off > size || len > size - off) return false;
  }
  uint32_t off, len;
  if (!findTable(d, size, "head", off, len) || len < 54 || be16(d + off + 18) != 512) return false;
  if (!findTable(d, size, "maxp", off, len) || len < 6 || be16(d + off + 4) != count + 1) return false;
  if (!findTable(d, size, "hhea", off, len) || len < 36 || be16(d + off + 4) != 448 ||
      (int16_t)be16(d + off + 6) != -64 || be16(d + off + 8) != 0) return false;
  const size_t metrics = be16(d + off + 34);
  if (!metrics || metrics > count + 1 || !findTable(d, size, "hmtx", off, len) ||
      len < 4 * metrics + 2 * (count + 1 - metrics)) return false;
  for (size_t i = 0; i < metrics; i++) if (be16(d + off + 4 * i) != 512) return false;
  if (!findTable(d, size, "CFF ", off, len) || len < 4 || d[off] != 1 || d[off + 2] < 4 ||
      d[off + 2] > len || d[off + 3] < 1 || d[off + 3] > 4) return false;
  if (!findTable(d, size, "cmap", off, len) || len < 4) return false;
  const uint8_t* cmap = d + off;
  const size_t subs = be16(cmap + 2);
  if (4 + 8 * subs > len) return false;
  bool found = false;
  for (size_t i = 0; i < subs; i++) {
    const uint8_t* r = cmap + 4 + 8 * i;
    const uint32_t sub = be32(r + 4);
    if (sub > len || len - sub < 2) return false;
    if (be16(cmap + sub) != 12) continue;
    if (len - sub < 16) return false;
    const uint8_t* p = cmap + sub;
    const uint32_t bytes = be32(p + 4), groups = be32(p + 12);
    if (bytes > len - sub || bytes < 16 || groups > (bytes - 16) / 12) return false;
    uint32_t next = BRAND_BASE;
    for (uint32_t g = 0; g < groups; g++) {
      const uint8_t* gr = p + 16 + 12 * g;
      const uint32_t start = be32(gr), end = be32(gr + 4), gid = be32(gr + 8);
      if (g == 0 && start == 0 && end == 0 && gid == 0) continue;
      if (start != next || end < start || end >= BRAND_BASE + count || gid != start - BRAND_BASE + 1)
        return false;
      next = end + 1;
    }
    if (next != BRAND_BASE + count) return false;
    if (be16(r) == 0 || (be16(r) == 3 && be16(r + 2) == 10)) found = true;
  }
  return found;
}

// full = check the whole font (install); boot only checks its size to keep the
// 1.7 MB SD read out of the boot time.
bool readBrand(const char* font, const char* names, BrandData& out, String& err, bool full) {
  err = "";
  if (!wallOnSd()) { err = "brand icons need a microSD card"; return false; }
  if (!readBrandNames(names, out, err)) {
    if (!err.length()) err = "invalid or unsorted brand names";
    return false;
  }
  if (!full) {
    File f = wallFs().open(font, "r");
    out.size = f ? f.size() : 0;
    if (!out.size || out.size > BRAND_MAX_FILE) { err = "missing or oversized brand font"; return false; }
    registerBrandFs();
    return true;
  }
  uint8_t* data = readBrandFile(font, BRAND_MAX_FILE, out.size, err);
  if (!data) return false;
  const bool valid = validBrandFont(data, out.size, out.count);
  // Validate in memory: TinyTTF does not close its file on an initialization failure.
  lv_font_t* probe = valid ? lv_tiny_ttf_create_data_ex(data, out.size, 24, LV_FONT_KERNING_NONE, 1) : nullptr;
  if (probe) lv_tiny_ttf_destroy(probe);
  heap_caps_free(data);
  if (!valid) { err = "brand font metrics or codepoints do not match the names"; return false; }
  if (!probe) { err = "cannot open brand font"; return false; }
  registerBrandFs();
  return true;
}

bool removeIfPresent(const char* path) {
  return !wallFs().exists(path) || wallFs().remove(path);
}

// A journal makes the two file renames recoverable as a pair after a restart.
bool recoverBrand(String& err) {
  if (!wallFs().exists(BRAND_PENDING)) return true;
  File journal = wallFs().open(BRAND_PENDING, "r");
  const int flags = journal ? journal.read() : -1;
  journal.close();
  if (flags < 0 || flags > 3) { err = "cannot read brand install journal"; return false; }
  const char* paths[] = {brandFontPath(), brandNamesPath()};
  for (int i = 0; i < 2; i++) {
    const String backup = String(paths[i]) + ".bak";
    bool ok = true;
    if (wallFs().exists(backup))
      ok = removeIfPresent(paths[i]) && wallFs().rename(backup, paths[i]);
    else if (!(flags & (1 << i))) ok = removeIfPresent(paths[i]);
    if (!ok) { err = "cannot restore previous brand files"; return false; }
  }
  if (!wallFs().remove(BRAND_PENDING)) { err = "cannot clear brand install journal"; return false; }
  return true;
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

const char* brandFontPath() { return "/touchdeck/icons/cbi.otf"; }
const char* brandNamesPath() { return "/touchdeck/icons/cbi.names"; }
bool brandReady() { return s_brand.offsets != nullptr; }
size_t brandCount() { return s_brand.count; }
size_t brandFileSize() { return s_brand.size; }
const String& brandVersion() { return s_brand.version; }

bool brandLoad(String& err) {
  if (!wallOnSd()) { err = "brand icons need a microSD card"; return false; }
  if (!recoverBrand(err)) return false;
  BrandData next;
  if (!readBrand(brandFontPath(), brandNamesPath(), next, err, false)) return false;
  freeFonts();
  s_brand.swap(next);
  Serial.printf("Deck: brand font %u bytes, %u icons (%s)\n", (unsigned)s_brand.size,
                (unsigned)s_brand.count, s_brand.version.c_str());
  return true;
}

bool brandInstall(const char* font, const char* names, String& err) {
  BrandData next;
  if (!readBrand(font, names, next, err, true)) return false;
  freeFonts();
  if (!recoverBrand(err)) return false;
  const char* paths[] = {brandFontPath(), brandNamesPath()};
  const char* uploads[] = {font, names};
  uint8_t flags = 0;
  for (int i = 0; i < 2; i++) {
    if (!removeIfPresent((String(paths[i]) + ".bak").c_str())) {
      err = "cannot clear old brand backup";
      return false;
    }
    if (wallFs().exists(paths[i])) flags |= 1 << i;
  }
  File journal = wallFs().open(BRAND_PENDING, "w");
  const bool written = journal && journal.write(&flags, 1) == 1;
  journal.close();
  if (!written) {
    wallFs().remove(BRAND_PENDING);
    err = "cannot create brand install journal";
    return false;
  }
  bool ok = true;
  for (int i = 0; i < 2 && ok; i++) {
    if (flags & (1 << i)) ok = wallFs().rename(paths[i], String(paths[i]) + ".bak");
    if (ok) ok = wallFs().rename(uploads[i], paths[i]);
  }
  if (ok) ok = wallFs().remove(BRAND_PENDING);
  if (!ok) {
    err = "cannot store brand files";
    String recovery;
    if (!recoverBrand(recovery)) {
      BrandData empty;
      s_brand.swap(empty);
      err += "; " + recovery;
    }
    return false;
  }
  s_brand.swap(next);
  for (const char* path : paths) wallFs().remove(String(path) + ".bak");
  return true;
}

bool brandRemove(String& err) {
  err = "";
  if (!wallOnSd()) { err = "brand icons need a microSD card"; return false; }
  freeFonts();
  if (!recoverBrand(err)) return false;
  const bool font = removeIfPresent(brandFontPath());
  const bool names = removeIfPresent(brandNamesPath());
  if (font || names) {
    BrandData empty;
    s_brand.swap(empty);
  }
  if (!font || !names) { err = "cannot remove brand files"; return false; }
  return true;
}

uint32_t iconCodepoint(const String& name) {
  if (name.startsWith("phu:")) {
    size_t lo = 0, hi = s_brand.count;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      const int c = strcmp(s_brand.names + s_brand.offsets[mid], name.c_str() + 4);
      if (c == 0) return BRAND_BASE + mid;
      if (c < 0) lo = mid + 1;
      else hi = mid;
    }
    return 0;
  }
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

static lv_font_t* sizedFont(Sized (&fonts)[MAX_FONTS], const char* path, int px) {
  for (const Sized& s : fonts)
    if (s.font && s.px == px) return s.font;
  for (Sized& s : fonts) {
    if (s.font) continue;
    s.font = lv_tiny_ttf_create_file_ex(path, px, LV_FONT_KERNING_NONE, 64);
    s.px = px;
    return s.font;
  }
  return nullptr;  // all size slots taken: a caller asked for too many sizes
}

const lv_font_t* iconFont(int px, uint32_t cp) {
  lv_font_t* mdi = s_index ? sizedFont(s_fonts, "I:/mdi.ttf", px) : nullptr;
  if (cp < BRAND_BASE) return mdi;
  // Brand sizes open only where a brand icon is shown: each one holds a file on the card.
  lv_font_t* brand = brandReady() ? sizedFont(s_brandFonts, "B:/touchdeck/icons/cbi.otf", px) : nullptr;
  if (!brand) return nullptr;
  if (!mdi) return brand;
  mdi->fallback = brand;
  if (mdi->get_glyph_dsc != mdiGlyphDsc) {
    s_mdiGlyphDsc = mdi->get_glyph_dsc;
    mdi->get_glyph_dsc = mdiGlyphDsc;
  }
  return mdi;
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
