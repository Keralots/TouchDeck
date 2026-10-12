// Guition deck: Material Design Icons (the set Home Assistant uses) from a
// TTF the owner uploads to LittleFS. The font is held in PSRAM, LVGL TinyTTF
// renders it at any size, and the name -> codepoint index is built from the
// font's own glyph names (post table), so it always matches the uploaded file.
#ifndef DECK_ICONS_H
#define DECK_ICONS_H

#include <Arduino.h>
#include <lvgl.h>

namespace deck {

// Where the font lives on LittleFS.
const char* iconsPath();
// Load (or replace) the font from path. The current font stays when the new
// one fails. Fonts from iconFont() die here: free their labels first.
bool iconsLoad(const char* path, String& err);
bool iconsReady();
size_t iconsCount();
size_t iconsFileSize();
// The loaded font file (PSRAM), for serving it to the portal.
const uint8_t* iconsData();

// "mdi:name" or "name" -> codepoint; 0 when unknown or no font is loaded.
uint32_t iconCodepoint(const String& name);
// TinyTTF font for this pixel size (cached); nullptr when no font is loaded.
const lv_font_t* iconFont(int px);
// UTF-8 text of one codepoint, for lv_label_set_text.
void iconUtf8(uint32_t cp, char out[5]);

}  // namespace deck

#endif  // DECK_ICONS_H
