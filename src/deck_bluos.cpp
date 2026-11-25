// BluOS player client. See deck_bluos.h.
#include "deck_bluos.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <deque>

namespace deck {
namespace {

constexpr uint32_t POLL_FAST_MS = 2000, POLL_SLOW_MS = 8000;
constexpr int QUEUE_MAX = 60;           // tracks in one window
constexpr size_t REPLY_MAX = 48 * 1024;  // a 60-track window is about 30 KB

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
String s_host;
bool s_fast = false;
uint32_t s_version = 1;
BluosStatus s_status;
BluosQueue s_queue;
BluosTrack s_next;
int s_nextFor = -2;                 // the song s_next was read for
int s_wantStart = -1, s_wantCount = 0;  // queue window to load
std::deque<String> s_cmds;
bool s_pollNow = true;

struct Lock {
  Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_lock); }
};

bool get(const String& host, const String& path, String& body) {
  if (WiFi.status() != WL_CONNECTED || !host.length()) return false;
  const String url = "http://" + host + (host.indexOf(':') < 0 ? ":11000" : "") + path;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  const int rc = http.GET();
  bool ok = rc == 200;
  if (ok) {
    const int size = http.getSize();
    if (size > (int)REPLY_MAX) ok = false;
    else body = http.getString();
  }
  http.end();
  if (!ok) Serial.printf("[bluos] %s -> %d\n", path.c_str(), rc);
  return ok && body.length() < REPLY_MAX;
}

String unescape(const String& s) {
  if (s.indexOf('&') < 0) return s;
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const int end = s.indexOf(';', i);
    if (end < 0 || end - (int)i > 8) {
      out += '&';
      continue;
    }
    const String e = s.substring(i + 1, end);
    if (e == "amp") out += '&';
    else if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else if (e.startsWith("#")) {
      const long cp = e[1] == 'x' ? strtol(e.c_str() + 2, nullptr, 16) : strtol(e.c_str() + 1, nullptr, 10);
      if (cp < 0x80) out += (char)cp;
      else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
      } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
      }
    } else {
      out += s.substring(i, end + 1);
    }
    i = end;
  }
  return out;
}

// Text of the first <tag>...</tag> in [from, to).
String tagText(const String& xml, const char* tag, int from = 0, int to = -1) {
  const String open = String('<') + tag + '>';
  const int a = xml.indexOf(open, from);
  if (a < 0 || (to >= 0 && a >= to)) return "";
  const int b = xml.indexOf(String("</") + tag + '>', a);
  if (b < 0 || (to >= 0 && b > to)) return "";
  String s = unescape(xml.substring(a + open.length(), b));
  // Thin and no-break spaces ("256 kb/s") are not in the deck fonts.
  s.replace("\xE2\x80\x89", " ");
  s.replace("\xE2\x80\xAF", " ");
  s.replace("\xC2\xA0", " ");
  return s;
}

// Attribute value inside the element that starts at `at`.
String attr(const String& xml, int at, const char* name) {
  const int end = xml.indexOf('>', at);
  const String key = String(' ') + name + "=\"";
  const int a = xml.indexOf(key, at);
  if (a < 0 || (end >= 0 && a > end)) return "";
  const int b = xml.indexOf('"', a + key.length());
  return b < 0 ? "" : unescape(xml.substring(a + key.length(), b));
}

void readStatus(const String& host) {
  String xml;
  BluosStatus st;
  if (get(host, "/Status", xml) && xml.indexOf("<status") >= 0) {
    st.ok = true;
    st.queue = xml.indexOf("<streamUrl>") < 0;
    const String song = tagText(xml, "song");
    st.song = song.length() ? song.toInt() : -1;
    st.pid = (uint32_t)tagText(xml, "pid").toInt();
    st.shuffle = tagText(xml, "shuffle") == "1";
    const String rep = tagText(xml, "repeat");
    st.repeat = rep.length() ? (uint8_t)constrain(rep.toInt(), 0, 2) : 2;
    st.service = tagText(xml, "serviceName");
    st.format = tagText(xml, "streamFormat");
    st.album = tagText(xml, "album");
    st.secs = tagText(xml, "secs").toInt();
    st.totlen = tagText(xml, "totlen").toInt();
    st.secsAt = millis();
    for (int at = xml.indexOf("<action "); at >= 0; at = xml.indexOf("<action ", at + 1)) {
      const String name = attr(xml, at, "name");
      if (name == "love") {
        st.loveUrl = attr(xml, at, "url");
        const String s = attr(xml, at, "state");
        st.love = s.length() ? (int8_t)s.toInt() : -1;
      } else if (name == "ban") {
        st.banUrl = attr(xml, at, "url");
      }
    }
  }
  Lock g;
  const bool same = st.ok == s_status.ok && st.queue == s_status.queue && st.song == s_status.song &&
                    st.pid == s_status.pid && st.shuffle == s_status.shuffle && st.repeat == s_status.repeat &&
                    st.service == s_status.service && st.format == s_status.format && st.album == s_status.album &&
                    st.loveUrl == s_status.loveUrl && st.banUrl == s_status.banUrl && st.love == s_status.love;
  // The position moves every poll: kept without a version bump.
  s_status.secs = st.secs;
  s_status.totlen = st.totlen;
  s_status.secsAt = st.secsAt;
  if (same) return;
  if (st.pid != s_status.pid) s_queue.tracks.clear();  // another queue: the window is stale
  s_status = st;
  s_version++;
}

// Tracks of a /Playlist reply; start = the first one's queue index.
void parseQueue(const String& xml, BluosQueue& q) {
  const int root = xml.indexOf("<playlist");
  if (root < 0) return;
  q.name = attr(xml, root, "name");
  q.length = attr(xml, root, "length").toInt();
  q.pid = (uint32_t)attr(xml, root, "id").toInt();
  q.start = -1;
  for (int at = xml.indexOf("<song ", root); at >= 0;) {
    const int end = xml.indexOf("</song>", at);
    if (end < 0) break;
    const int id = attr(xml, at, "id").toInt();
    if (q.start < 0) q.start = id;
    if (id != q.start + (int)q.tracks.size()) break;  // the window must be contiguous
    BluosTrack t;
    t.title = tagText(xml, "title", at, end);
    t.artist = tagText(xml, "art", at, end);
    t.secs = (uint16_t)tagText(xml, "time", at, end).toInt();
    q.tracks.push_back(t);
    at = xml.indexOf("<song ", end);
  }
  if (q.start < 0) q.start = 0;
}

void readQueue(const String& host, int start, int count) {
  String xml;
  BluosQueue q;
  if (count > 0 && get(host, "/Playlist?start=" + String(start) + "&end=" + String(start + count - 1), xml))
    parseQueue(xml, q);
  Lock g;
  s_queue = q;
  s_version++;
}

void readNext(const String& host, int song) {
  String xml;
  BluosQueue q;
  if (get(host, "/Playlist?start=" + String(song + 1) + "&end=" + String(song + 1), xml)) parseQueue(xml, q);
  Lock g;
  s_next = q.tracks.empty() ? BluosTrack() : q.tracks[0];
  s_nextFor = song;
  s_version++;
}

void task(void*) {
  uint32_t lastPoll = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    String host, cmd;
    int start = -1, count = 0, nextFor = -2;
    bool fast, pollNow;
    BluosStatus st;
    {
      Lock g;
      host = s_host;
      fast = s_fast;
      if (!s_cmds.empty()) {
        cmd = s_cmds.front();
        s_cmds.pop_front();
      }
      if (s_wantStart >= 0) {
        start = s_wantStart;
        count = s_wantCount;
        s_wantStart = -1;
      }
      pollNow = s_pollNow;
      s_pollNow = false;
      st = s_status;
      nextFor = s_nextFor;
    }
    if (!host.length()) continue;
    if (cmd.length()) {
      String reply;
      get(host, cmd, reply);
      readStatus(host);
      lastPoll = millis();
      if (cmd.startsWith("/Shuffle")) {  // same queue id, new order
        Lock g;
        s_queue.tracks.clear();
        s_nextFor = -2;
        s_version++;
      }
      continue;
    }
    if (count > 0) {
      readQueue(host, start, count);
      continue;
    }
    if (pollNow || millis() - lastPoll >= (fast ? POLL_FAST_MS : POLL_SLOW_MS)) {
      readStatus(host);
      lastPoll = millis();
      continue;
    }
    if (st.ok && st.queue && st.song >= 0 && nextFor != st.song) readNext(host, st.song);
  }
}

bool ensureTask() {
  if (s_task) return true;
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return false;
  // Stack in PSRAM: this task never touches flash.
  return xTaskCreatePinnedToCoreWithCaps(task, "deck_bluos", 6144, nullptr, 2, &s_task, 0, MALLOC_CAP_SPIRAM) ==
         pdPASS;
}

void wake() {
  if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

void bluosFollow(const String& host, bool fast) {
  if (!host.length() && !s_task) return;
  if (!ensureTask()) return;
  {
    Lock g;
    if (host == s_host && fast == s_fast) return;
    if (host != s_host) {
      s_status = BluosStatus();
      s_queue = BluosQueue();
      s_next = BluosTrack();
      s_nextFor = -2;
      s_cmds.clear();
      s_version++;
    }
    s_pollNow = s_pollNow || (fast && !s_fast) || host != s_host;
    s_host = host;
    s_fast = fast;
  }
  wake();
}

const String& bluosHost() { return s_host; }

uint32_t bluosVersion() {
  if (!s_lock) return 0;
  Lock g;
  return s_version;
}

bool bluosStatus(BluosStatus& out) {
  if (!s_lock) return false;
  Lock g;
  out = s_status;
  return s_status.ok;
}

void bluosLoadQueue(int start, int count) {
  if (!s_lock) return;
  {
    Lock g;
    s_wantStart = max(0, start);
    s_wantCount = constrain(count, 1, QUEUE_MAX);
  }
  wake();
}

bool bluosQueue(BluosQueue& out) {
  if (!s_lock) return false;
  Lock g;
  out = s_queue;
  return !s_queue.tracks.empty();
}

bool bluosNext(BluosTrack& out) {
  if (!s_lock) return false;
  Lock g;
  if (s_nextFor != s_status.song) return false;
  out = s_next;
  return out.title.length() > 0;
}

bool bluosCommand(const String& path) {
  if (!s_lock || !s_host.length() || !path.startsWith("/")) return false;
  {
    Lock g;
    if (s_cmds.size() >= 4) return false;
    s_cmds.push_back(path);
  }
  wake();
  return true;
}

}  // namespace deck
