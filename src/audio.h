// NS4168 I2S speaker: built-in sounds and WAV clips from the gallery storage,
// played by one task on core 0.
#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>
#include <ArduinoJson.h>

enum AudioSound : uint8_t { SND_FINISHED = 0, SND_ERROR, SND_COOLED, SND_CLICK, SND_COUNT };

// Starts the task when the speaker is enabled; the save handler calls it again.
void audioInit();
// Event sounds, gated by audioSettings (enabled, per-event switch, night mute).
void audioEvent(AudioSound s);
// Tile tap; dropped while another sound plays.
void audioClick();
// Portal Test button: ignores the enabled switch and the night. file "" = built-in.
bool audioTest(AudioSound s, uint8_t vol, const String& file, String& err);
bool audioPlayTone(uint16_t hz, uint16_t ms, uint8_t vol);
void audioStop();

const char* audioDir();
bool audioValidName(const String& name);
// Opens a WAV and checks its header; ms = duration.
bool audioProbe(const String& name, uint32_t& rate, uint8_t& channels, uint32_t& ms, String& err);
bool audioProbeFile(const String& path, uint32_t& rate, uint8_t& channels, uint32_t& ms, String& err);
// After an upload or delete: cached file clips are dropped.
void audioFilesChanged();
void audioStatusJson(JsonObject o);
void audioFilesJson(JsonArray a);

#endif  // AUDIO_H
