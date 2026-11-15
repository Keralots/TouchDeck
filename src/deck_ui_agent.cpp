// Deck UI: the AI Agent tile. Its icon, "on" look and second line follow the
// conversation state; a tap starts or ends the conversation (ACT_AGENT).
#include "deck_ui_int.h"
#include "agent.h"

namespace deckui {

namespace {

AgentState s_shown = AGENT_IDLE;
String s_shownSub;
uint32_t s_lastSync = 0;
uint32_t s_shownGen = UINT32_MAX;  // pages rebuilt: repaint even without a state change

const char* stateIcon(AgentState st) {
  switch (st) {
    case AGENT_CONNECTING: return "mdi:access-point-network";
    case AGENT_LISTENING:
    case AGENT_MICTEST: return "mdi:microphone";
    case AGENT_THINKING: return "mdi:dots-horizontal";
    case AGENT_SPEAKING: return "mdi:account-voice";
    case AGENT_ERROR: return "mdi:alert-circle-outline";
    default: return "mdi:robot-outline";
  }
}

void showStateIcon(TileRt& r, AgentState st) {
  if (!r.icon || r.def->icon.length()) return;  // a chosen icon stays
  const uint32_t cp = iconCodepoint(stateIcon(st));
  const lv_font_t* f = cp ? iconFont(r.iconPx, cp) : nullptr;
  if (!f || cp == r.iconCp) return;
  char u[5];
  iconUtf8(cp, u);
  lv_obj_set_style_text_font(r.icon, f, 0);
  lv_label_set_text(r.icon, u);
  r.iconCp = cp;
}

bool isAgentTile(const TileRt& r) { return r.def->type == TILE_AGENT || r.def->tap.kind == ACT_AGENT; }

}  // namespace

String agentTileSub() {
  if (!agentAvailable()) return "Off";
  switch (agentState()) {
    case AGENT_CONNECTING: return "Connecting";
    case AGENT_LISTENING: return "Listening";
    case AGENT_THINKING: return "Thinking";
    case AGENT_SPEAKING: return "Speaking";
    case AGENT_MICTEST: return "Mic test";
    case AGENT_ERROR: return "Error";
    default: return "Tap to talk";
  }
}

// Main loop: repaint agent tiles when the state changes.
void agentTileSync() {
  const uint32_t now = millis();
  if (now - s_lastSync < 100) return;
  s_lastSync = now;
  const AgentState st = agentState();
  const String sub = agentTileSub();
  if (st == s_shown && sub == s_shownSub && s_gen == s_shownGen) return;
  s_shownGen = s_gen;
  s_shown = st;
  s_shownSub = sub;
  const bool on = st != AGENT_IDLE && st != AGENT_ERROR;
  for (TileRt& r : s_tiles) {
    if (!isAgentTile(r) || !r.obj) continue;
    if (r.def->type == TILE_AGENT) {
      showStateIcon(r, st);
      if (r.sub && !r.def->sub.length()) lv_label_set_text(r.sub, sub.c_str());
    }
    setTileOn(r, on);
    markPageStale(r.page);
  }
}

}  // namespace deckui
