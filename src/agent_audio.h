// Full-duplex voice audio for the AI agent: ES7210 mics in, speaker out, one
// 16 kHz TDM channel run on the audio task while a conversation is open.
#ifndef AGENT_AUDIO_H
#define AGENT_AUDIO_H

#include <Arduino.h>

constexpr uint32_t AGENT_RATE = 16000;
constexpr uint32_t AGENT_FRAME = AGENT_RATE * 60 / 1000;  // samples per 60 ms frame

// Takes over the audio task; false when the board has no microphones or the
// task is busy closing a previous run. The voice plays at audioSettings.volume.
bool agentAudioStart(uint8_t pga, uint8_t boostDb);
void agentAudioStop();
// Mic gains while running (portal sliders during a mic test).
void agentAudioSetGain(uint8_t pga, uint8_t boostDb);
bool agentAudioRunning();

// Mic frames are queued only while capture is on.
void agentAudioCapture(bool on);
// One 60 ms mono frame, oldest first; dbfs = its RMS level.
bool agentAudioPopFrame(int16_t* dst, float& dbfs);
// Level of the latest mic frame, captured or not.
float agentAudioMicLevel();
// Room noise floor: 10th percentile of the last 3 s of frame levels.
float agentAudioFloor();
// The floor has seen enough frames to judge voice against.
bool agentAudioFloorReady();

// Speaker: mono 16 kHz samples; returns how many fitted.
size_t agentAudioPush(const int16_t* pcm, size_t n);
uint32_t agentAudioQueued();
// Free speaker ring space, samples.
uint32_t agentAudioRoom();
void agentAudioFlush();
// millis() of the last speaker chunk that carried queued samples.
uint32_t agentAudioLastPlayMs();

// Debug: the last 30 s of a session as stereo frames [mic, speaker], oldest first.
uint32_t agentAudioRecFrames();
void agentAudioRecRead(uint32_t from, int16_t* dst, uint32_t n);

#endif  // AGENT_AUDIO_H
