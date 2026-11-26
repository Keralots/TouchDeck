// BluOS players (Bluesound, NAD): play queue, shuffle, repeat and love/ban
// straight from the player's HTTP API on port 11000. One player is followed.
#ifndef DECK_BLUOS_H
#define DECK_BLUOS_H

#include <Arduino.h>
#include <vector>

namespace deck {

struct BluosStatus {
  bool ok = false;      // the last poll answered
  bool queue = false;   // the play queue is the source (no streamUrl): queue, shuffle, repeat apply
  int song = -1;        // queue index playing
  uint32_t pid = 0;     // play queue id, changes with the queue
  bool shuffle = false;
  uint8_t repeat = 2;   // 0 = queue, 1 = track, 2 = off
  String service;       // "YouTube Music"
  String format;        // "AAC 256 kb/s"
  String album;
  String loveUrl, banUrl;  // empty = not offered for this track
  int8_t love = -1;        // state of the love action
  int32_t secs = 0, totlen = 0;  // position and length; secs was read at secsAt (millis)
  uint32_t secsAt = 0;
};

struct BluosTrack {
  String title, artist;
  uint16_t secs = 0;
};

struct BluosQueue {
  uint32_t pid = 0;
  String name;   // "Dance Hits"
  int length = 0;
  int start = 0;  // queue index of tracks[0]
  std::vector<BluosTrack> tracks;
};

// host: "192.168.0.64" or "host:port", "" = none. fast polls every 2 s.
void bluosFollow(const String& host, bool fast);
const String& bluosHost();
// Bumps when the status, the queue window or the next track changes.
uint32_t bluosVersion();
bool bluosStatus(BluosStatus& out);
// Loads tracks [start, start + count) into the window.
void bluosLoadQueue(int start, int count);
bool bluosQueue(BluosQueue& out);
// The track after the playing one, title empty while unknown.
bool bluosNext(BluosTrack& out);
// A GET on the player ("/Play?id=5"); the status is read again after it.
bool bluosCommand(const String& path);

}  // namespace deck

#endif  // DECK_BLUOS_H
