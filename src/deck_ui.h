// Guition JC3248W535 deck UI: LVGL 9 on top of deck_panel. Page 0 is the
// StatNode metric screen (the existing faces, rendered by renderer.cpp into a
// LovyanGFX sprite that LVGL shows as a canvas); the pages after it are deck
// pages. Swiping between pages is done by the compositor in deck_panel from
// cached page images, so LVGL never renders a full-screen transition.
//
// Threading: every LVGL call happens in the Arduino loop task (deckTick and
// the functions below), the same task that renders the faces and serves the
// portal, so no LVGL locking is needed. The touch task only calls the two
// thread-safe swipe functions at the bottom.
#ifndef DECK_UI_H
#define DECK_UI_H

#if defined(BOARD_IS_JC3248W535)

#include <LovyanGFX.hpp>
#include "deck_panel.h"

// Brings up the panel, the face canvas, LVGL, the pages and touch. `face` is
// the sprite the renderer draws into (tft_ptr); it is (re)created here at the
// logical size of the rotation, always with LovyanGFX rotation 0.
bool deckBegin(lgfx::LGFX_Sprite* face, uint8_t rotation);
// Once per loop pass (flushFrame() on this board): slide landings, the touch
// wake, the face canvas, LVGL, page-image refresh.
void deckTick();
// The renderer drew into the face sprite.
void deckFaceDirty();
// Short message pill above the page dots (loop task only).
void deckToast(const char* text);
// Gate for updateDisplay(): false while a swipe owns the screen (the face
// sprite may be on screen as a page image) and, while another page is shown,
// except about once a second so the face stays current for the next swipe.
bool deckFaceMayRender();
bool deckFacePageActive();
// Rotation changed in the portal: re-layout everything at the new size.
void deckApplyRotation(uint8_t rotation);
// Portal save: validate, store /deck.json and rebuild the pages in place.
bool deckApplyConfig(const String& text, String& err);
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

#endif  // BOARD_IS_JC3248W535
#endif  // DECK_UI_H
