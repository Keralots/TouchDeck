// Guition deck: MDI on LittleFS and optional Custom Brand Icons on microSD.
// TinyTTF reads glyphs on demand; name -> codepoint indexes live in PSRAM.
#ifndef DECK_ICONS_H
#define DECK_ICONS_H

#include <Arduino.h>
#include <lvgl.h>

namespace deck {

// Where the font lives on LittleFS.
const char* iconsPath();
// Index the font at path; iconFont() then reads iconsPath(). The current
// index stays when the new font fails. Fonts from iconFont() die here: free
// their labels first.
bool iconsLoad(const char* path, String& err);
// Close the fonts (they hold iconsPath() open) and drop the index.
void iconsUnload();
bool iconsReady();
size_t iconsCount();
size_t iconsFileSize();

constexpr size_t BRAND_MAX_FILE = 3 * 1024 * 1024;
constexpr size_t BRAND_MAX_NAMES = 512 * 1024;
const char* brandFontPath();
const char* brandNamesPath();
// Call after SD mount and LVGL init. Missing or invalid files leave icons unavailable.
bool brandLoad(String& err);
// As with iconsLoad(), callers must release labels before changing fonts.
bool brandInstall(const char* font, const char* names, String& err);
bool brandRemove(String& err);
bool brandReady();
size_t brandCount();
size_t brandFileSize();
const String& brandVersion();

// "mdi:name", "name", or "phu:name" -> codepoint; 0 when unknown or unavailable.
uint32_t iconCodepoint(const String& name);
// TinyTTF font for this pixel size (cached); nullptr when no font is loaded.
// Pass the codepoint to draw: a brand one gets the brand font as fallback.
const lv_font_t* iconFont(int px, uint32_t cp = 0);
// UTF-8 text of one codepoint, for lv_label_set_text.
void iconUtf8(uint32_t cp, char out[5]);

}  // namespace deck

#endif  // DECK_ICONS_H
