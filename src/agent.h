// AI agent: a voice conversation with a xiaozhi-protocol server (WebSocket,
// Opus 16 kHz up, Opus down). Tap to start; the deck closes each turn on its
// own VAD and keeps listening after every reply until nobody speaks.
#ifndef AGENT_H
#define AGENT_H

#include <Arduino.h>
#include <ArduinoJson.h>

enum AgentState : uint8_t {
  AGENT_IDLE = 0,
  AGENT_CONNECTING,  // socket + hello
  AGENT_LISTENING,
  AGENT_THINKING,    // turn sent, no reply audio yet
  AGENT_SPEAKING,
  AGENT_ERROR,       // last session failed; shown until the next start
};

// The board has microphones and the feature is switched on.
bool agentAvailable();
void agentStart();
void agentStop();
// Tile tap: start when idle, otherwise end the conversation.
void agentToggle();
AgentState agentState();
// Mic level while listening, dBFS.
float agentLevel();
// Latest user and reply lines (copies).
void agentTexts(String& user, String& reply);
void agentStatusJson(JsonObject o);

#endif  // AGENT_H
