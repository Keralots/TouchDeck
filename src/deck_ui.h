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
// Portal save: validate, store /deck.json and rebuild the pages in place.
bool deckApplyConfig(const String& text, String& err);
// Portal upload: load the MDI font stored at uploaded, keep it as the icon
// font and rebuild the pages with it. A bad file is deleted, the old font stays.
bool deckInstallIcons(const char* uploaded, String& err);
// Top bar (page name, WiFi, clock) on the tiles pages; persisted.
void deckSetHeader(bool on);
bool deckHeaderVisible();
// Bench hook (/api/deck/swipe): run one full page slide toward dir as if a
// finger had flung it. False when that page is not reachable right now.
bool deckTestSwipe(int dir);
// Bench hook (/api/deck/card): open the detail card of the tile with this id.
bool deckTestCard(const String& tileId);
// Bench hook (/api/deck/panel): open or close the quick panel with its slide.
bool deckTestPanel(bool open);

namespace deck {
// Touch task: claim a swipe toward dir (+1 next page, -1 previous page).
// Fills the strip sources and the starting offset; false when there is no
// page that way, its image is not ready, or swiping is locked.
// pageA is the pager index of the strip's left page.
bool uiTryBeginSwipe(int dir, Src& a, Src& b, int& startOff, int& pageA);
// The compositor refused the drag after a uiTryBegin*() call succeeded.
void uiSwipeAborted();
// Touch task: claim a quick-panel drag (opening = pull down, else push up).
// Fills the panel image, the page under it, the starting offset and the
// panel height in rows.
bool uiTryBeginPanel(bool opening, Src& panel, Src& page, int& startOff, int& panelH);
bool uiPanelIsOpen();
}  // namespace deck

#endif  // DECK_UI_H
