// Deck UI: the PIN pad, and the lock on changing the interface from the screen.
#include "deck_ui_int.h"
#include "pin_lock.h"

namespace deckui {

namespace {
constexpr uint32_t PAD_IDLE_MS = 20000;
constexpr uint32_t UNLOCK_IDLE_MS = 120000;
constexpr int KEY_BACK = 10, KEY_OK = 11;

std::function<void()> s_then;  // runs once the PIN is right; set = the open card is the pad
char s_typed[9];
lv_obj_t* s_dots = nullptr;
lv_obj_t* s_note = nullptr;
lv_obj_t* s_keys[12];
uint32_t s_openMs = 0;
uint32_t s_lockSecs = 0, s_lockMs = 0;  // the pad shows this many seconds from then
bool s_unlocked = false;

uint32_t padIdleMs() { return millis() - touchLastActivity(); }

void showTyped() {
  char dots[2 * 8 + 1] = "";
  for (size_t i = 0; i < strlen(s_typed); i++) strcat(dots, "* ");
  lv_label_set_text(s_dots, dots);
}

void submit() {
  if (!s_typed[0] || s_lockSecs) return;
  uint32_t left;
  const PinResult r = pinTry(PIN_TOUCH, s_typed, left);
  s_typed[0] = 0;
  showTyped();
  if (r == PIN_OK) {
    static std::function<void()> then;
    then = std::move(s_then);
    s_then = nullptr;
    // The pad's own button is running this event.
    lv_async_call([](void*) {
      closeModal();
      const auto go = std::move(then);
      then = nullptr;
      if (go) go();
    }, nullptr);
    return;
  }
  hapticTap();
  s_lockSecs = left;
  s_lockMs = millis();
  if (!left) lv_label_set_text(s_note, "Wrong PIN");
}

void onKey(lv_event_t* e) {
  if (!s_then) return;
  const int k = (int)(intptr_t)lv_event_get_user_data(e);
  if (k == KEY_OK) return submit();
  const size_t n = strlen(s_typed);
  if (k == KEY_BACK) {
    if (n) s_typed[n - 1] = 0;
  } else if (n + 1 < sizeof(s_typed)) {
    s_typed[n] = '0' + k;
    s_typed[n + 1] = 0;
  }
  if (!s_lockSecs) lv_label_set_text(s_note, "");
  showTyped();
}
}  // namespace

bool pinPadOpen() { return (bool)s_then; }

// From closeModal, whoever closed the card.
void pinPadClosed() {
  s_then = nullptr;
  memset(s_typed, 0, sizeof(s_typed));
  s_dots = s_note = nullptr;
  s_lockSecs = 0;
}

void pinPadAsk(const char* title, std::function<void()> then) {
  hidePanelNow();
  closeModal();
  cleanPage();
  s_then = std::move(then);
  s_openMs = millis();
  const int w = logicalW(), h = logicalH();
  s_modalScrim = makeScrim();
  const int cw = w - 24 < 300 ? w - 24 : 300;
  const int ch = h - 16 < 380 ? h - 16 : 380;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS && (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &deck_font_20, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(t, cw - 32, lv_font_get_line_height(&deck_font_20));
  lv_obj_set_pos(t, 16, 12);
  s_dots = lv_label_create(c);
  lv_obj_set_style_text_font(s_dots, &deck_font_28, 0);
  lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, 42);
  s_note = lv_label_create(c);
  lv_label_set_text(s_note, "Enter the PIN");
  lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 72);
  showTyped();

  constexpr int TOP = 96, GAP = 8, PAD = 12;
  const int bw = (cw - 2 * PAD - 2 * GAP) / 3;
  const int bh = (ch - TOP - PAD - 3 * GAP) / 4;
  static const char* const kText[12] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                        LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK};
  for (int k = 0; k < 12; k++) {
    const int cell = k == 0 ? 10 : k == KEY_BACK ? 9 : k == KEY_OK ? 11 : k - 1;  // 1..9, then back, 0, OK
    lv_obj_t* b = qpButton(c, PAD + (cell % 3) * (bw + GAP), TOP + (cell / 3) * (bh + GAP), bw, kText[k], false);
    lv_obj_set_height(b, bh);
    lv_obj_set_style_text_font(b, &deck_font_20, 0);
    if (k == KEY_OK) lv_obj_add_state(b, LV_STATE_CHECKED);  // the accent look
    lv_obj_add_event_cb(b, onKey, LV_EVENT_CLICKED, (void*)(intptr_t)k);
    s_keys[k] = b;
  }
  setModalOpen(true);
}

// --- The lock on editing -------------------------------------------------------

bool editAllowed() { return !pinIsSet() || pinEditLock() == EDIT_ALLOWED || (s_unlocked && pinEditLock() == EDIT_ASK); }

void editGuard(std::function<void()> then) {
  if (editAllowed()) return then();
  if (pinEditLock() == EDIT_OFF) {
    deckToastShow("Changes on this screen are locked", 2500);
    return;
  }
  pinPadAsk("Change this screen", [then]() {
    s_unlocked = true;
    then();
  });
}

void pinTick() {
  const uint32_t now = millis();
  const bool dark = manualDisplayOff() || !currentBacklightLevel();
  if (s_then) {
    if (s_lockSecs) {
      const uint32_t past = (now - s_lockMs) / 1000;
      if (past >= s_lockSecs) {
        s_lockSecs = 0;
        lv_label_set_text(s_note, "Enter the PIN");
      } else {
        const String text = "Too many wrong tries: " + String(s_lockSecs - past) + " s";
        if (text != lv_label_get_text(s_note)) lv_label_set_text(s_note, text.c_str());
      }
    }
    if (dark || (now - s_openMs >= PAD_IDLE_MS && padIdleMs() >= PAD_IDLE_MS)) closeModal();
  }
  if (s_unlocked && (dark || frameActive() || padIdleMs() >= UNLOCK_IDLE_MS || !pinIsSet())) {
    s_unlocked = false;
    // An editor left open would stay usable for the next person.
    if (!editAllowed()) edClose();
  }
}

bool deckTestPinKeys(const String& keys) {
  if (!s_then) return false;
  for (size_t i = 0; i < keys.length() && s_then; i++) {
    const char k = keys[i];
    const int at = k >= '0' && k <= '9' ? k - '0' : k == '<' ? KEY_BACK : k == 'k' ? KEY_OK : -1;
    if (at >= 0) lv_obj_send_event(s_keys[at], LV_EVENT_CLICKED, nullptr);
  }
  return true;
}

}  // namespace deckui
