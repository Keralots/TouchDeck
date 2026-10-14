// Deck UI: LVGL 9 on top of deck_panel. Swiping between pages is done by the
// compositor in deck_panel from cached page images, so LVGL never renders a
// full-screen transition. System screens (deck_sys) sit above the pages.
//
// Threading: every LVGL call happens in the Arduino loop task (deckTick and
// the functions below), the same task that serves the portal, so no LVGL
// locking is needed. The touch task only calls the thread-safe functions in
// namespace deck at the bottom.
#ifndef DECK_UI_H
#define DECK_UI_H

#include "deck_panel.h"

// Brings up the panel, LVGL, the pages and touch at the given rotation.
bool deckBegin(uint8_t rotation);
// Once per loop pass (displayTick()): slide landings, the touch wake, system
// screens, LVGL, page-image refresh.
void deckTick();
// Short message pill above the page dots (loop task only).
void deckToast(const char* text);
// Render pending LVGL changes now, from inside a long web handler (OTA).
void deckRefreshNow();
// Rotation changed in the portal: re-layout everything at the new size.
void deckApplyRotation(uint8_t rotation);
// Switch the colour theme (THEME_MIDNIGHT / THEME_DAYLIGHT / THEME_GLASS);
// rebuilds the pages.
void deckApplyTheme(uint8_t theme);
// A wallpaper file was replaced or removed (fileChanged), or the Glass
// sliders moved: Glass rebuilds the pages, re-reading the file if needed.
void deckReloadWallpaper(bool fileChanged);
// Bench hook (/api/deck/bench): render the shown page into its page image n
// times and time it.
bool deckBenchRender(int n, uint32_t& avgUs, uint32_t& maxUs);
// Portal save: validate, store /deck.json and rebuild the pages in place.
bool deckApplyConfig(const String& text, String& err);
// Portal upload: load the MDI font stored at uploaded, keep it as the icon
// font and rebuild the pages with it. A bad file is deleted, the old font stays.
bool deckInstallIcons(const char* uploaded, String& err);
// Rebuild the pages without icons, so the font file can be deleted.
void deckReleaseIcons();
// Top bar (page name, WiFi, clock) on the tiles pages; persisted.
void deckSetHeader(bool on);
bool deckHeaderVisible();
// Bench hook (/api/deck/swipe): run one full slide toward dir (to the next
// column, or vertical to the next page of the column) as if a finger had
// flung it. False when that page is not reachable right now.
bool deckTestSwipe(int dir, bool vertical);
// Bench hook (/api/deck/card): open the detail card of the tile with this id.
bool deckTestCard(const String& tileId);
// Bench hooks (/api/deck/tap, /api/deck/confirm): tap or hold a tile, answer
// its confirm card.
bool deckTestTap(const String& tileId, bool hold);
bool deckTestConfirm(bool ok);
// Bench hook (/api/deck/slide): a slider tile drag ending at pct.
bool deckTestSlide(const String& tileId, int pct);
// Bench hook (/api/deck/panel): open or close the quick panel with its slide.
bool deckTestPanel(bool open);
// Bench hook (/api/deck/edit): drive the on-device edit mode. op = open |
// move (tile = index on the page, cell x/y, size w/h, 0 = keep) | remove |
// done | cancel. False when the step is not possible.
bool deckTestEdit(const String& op, int tile, int x, int y, int w, int h);

namespace deck {
// Touch task: claim a swipe toward dir (+1 next page, -1 previous page).
// Fills the strip sources and the starting offset; false when there is no
// page that way, its image is not ready, or swiping is locked. vertical =
// within the column. idxA is the dot index of the strip's first page.
bool uiTryBeginSwipe(int dir, bool vertical, Src& a, Src& b, int& startOff, int& idxA);
// The compositor refused the drag after a uiTryBegin*() call succeeded.
void uiSwipeAborted();
// Touch task: true when (x, y) is on a slider tile of the shown page, where a
// drag moves the slider instead of the page.
bool uiNoSwipeAt(int x, int y);
// Touch task: claim a quick-panel drag (opening = pull down, else push up).
// Fills the panel image, the page under it, the starting offset and the
// panel height in rows.
bool uiTryBeginPanel(bool opening, Src& panel, Src& page, int& startOff, int& panelH);
bool uiPanelIsOpen();
}  // namespace deck

#endif  // DECK_UI_H
