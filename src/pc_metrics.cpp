// PC stats ingest: a WiFiUDP listener that decodes the companions' V2.1 JSON
// push packets, one PcSource per PC. Parse logic ported from SmallOLED-PCMonitor
// network.cpp parseStatsV2, slimmed to the color project's metric model.
//
// WiFiUDP (not AsyncUDP) on purpose: AsyncUDP froze on ESP32 in the sibling
// projects. The socket is polled from loop() via pcMetricsHandle().
#include "pc_metrics.h"
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <vector>
#include "improv_setup.h"
#include "deck_viz.h"

PcSource* pcSources = nullptr;

static WiFiUDP udp;
static uint32_t s_version = 1;

static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = h[b[i] >> 4];
    out[2 * i + 1] = h[b[i] & 15];
  }
  out[2 * n] = 0;
}

static bool fromHex(const char* s, uint8_t* b, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    char t[3] = {s[2 * i], s[2 * i + 1], 0};
    char* end = nullptr;
    b[i] = (uint8_t)strtoul(t, &end, 16);
    if (!end || *end) return false;
  }
  return true;
}

// Known PCs in NVS: "deckpc"/"list" = [{"id","name"}], first = default.
static void saveKnown() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < MAX_PCS; i++) {
    if (!pcSources[i].used) continue;
    JsonObject o = a.add<JsonObject>();
    o["id"] = pcSources[i].id;
    o["name"] = pcSources[i].name;
    if (pcSources[i].paired) {
      char k[65];
      toHex(pcSources[i].key, 32, k);
      o["key"] = k;
    }
  }
  String out;
  serializeJson(doc, out);
  Preferences p;
  if (p.begin("deckpc", false)) {
    p.putString("list", out);
    p.end();
  }
}

static void loadKnown() {
  Preferences p;
  String s;
  if (p.begin("deckpc", true)) {
    s = p.getString("list", "");
    p.end();
  }
  JsonDocument doc;
  if (!s.length() || deserializeJson(doc, s)) return;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n == MAX_PCS) break;
    PcSource& pc = pcSources[n++];
    pc.used = true;
    strlcpy(pc.id, o["id"] | "", sizeof(pc.id));
    strlcpy(pc.name, o["name"] | "PC", sizeof(pc.name));
    pc.paired = fromHex(o["key"] | (const char*)nullptr, pc.key, 32);
  }
}

static QueueHandle_t s_lineQ = nullptr;  // "@TD " payloads (heap copies) for the loop
static void serialTask(void*);
static void arenaBegin();

void pcMetricsBegin() {
  pcSources = (PcSource*)heap_caps_calloc(MAX_PCS, sizeof(PcSource), MALLOC_CAP_SPIRAM);
  if (!pcSources) pcSources = (PcSource*)calloc(MAX_PCS, sizeof(PcSource));
  for (int i = 0; i < MAX_PCS; i++) pcSources[i].data.status = PC_STATUS_OK;
  loadKnown();
  arenaBegin();
  s_lineQ = xQueueCreate(4, sizeof(char*));
  if (!s_lineQ || xTaskCreatePinnedToCore(serialTask, "pc_serial", 3072, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("pc_metrics: no USB reader task, the cable carries no metrics");
  udp.begin(UDP_PORT);
  Serial.printf("pc_metrics: UDP listening on port %u\n", (unsigned)UDP_PORT);
}

void pcMetricsRestartUdp() {
  udp.stop();
  udp.begin(UDP_PORT);
  Serial.println("pc_metrics: UDP socket re-bound");
}

// Track names use typographic quotes, dashes and ellipses the deck fonts lack
// (they stop at Latin Extended-A): turn them into ASCII, in place.
static void asciiPunct(char* s) {
  char* out = s;
  for (const char* p = s; *p;) {
    const uint8_t* u = (const uint8_t*)p;
    if (u[0] == 0xE2 && u[1] == 0x80 && u[2]) {
      const char* rep = nullptr;
      switch (u[2]) {
        case 0x98: case 0x99: rep = "'"; break;
        case 0x9C: case 0x9D: rep = "\""; break;
        case 0x93: case 0x94: rep = "-"; break;
        case 0xA6: rep = "..."; break;
      }
      if (rep) {
        // "..." takes three bytes, as many as it replaces: out never passes p.
        while (*rep) *out++ = *rep++;
        p += 3;
        continue;
      }
    }
    *out++ = *p++;
  }
  *out = 0;
}

static void trimTrailingSpaces(char* s) {
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

// The PC a packet belongs to; a new id takes a free slot (and is remembered).
static PcSource* sourceForPacket(const char* id, const char* name) {
  PcSource* free = nullptr;
  for (int i = 0; i < MAX_PCS; i++) {
    PcSource& pc = pcSources[i];
    if (!pc.used) {
      if (!free) free = &pc;
      continue;
    }
    if (strcmp(pc.id, id)) continue;
    if (name[0] && strcmp(pc.name, name)) {
      strlcpy(pc.name, name, sizeof(pc.name));
      saveKnown();
    }
    return &pc;
  }
  if (!free) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 60000) {
      lastLog = millis();
      Serial.printf("pc_metrics: no room for PC '%s' (%d known), forget one in the portal\n", id, MAX_PCS);
    }
    return nullptr;
  }
  memset(free, 0, sizeof(PcSource));
  free->used = true;
  free->data.status = PC_STATUS_OK;
  strlcpy(free->id, id, sizeof(free->id));
  strlcpy(free->name, name[0] ? name : "PC", sizeof(free->name));
  saveKnown();
  Serial.printf("pc_metrics: new PC '%s' (%s)\n", free->name, free->id);
  return free;
}

// --- Commands in flight and their results (all on the loop task) -----------
struct PendingCmd {
  String pcId, nonce, cmd;
  uint32_t sentMs;
};
static std::vector<PendingCmd> s_pending;
static std::vector<PcCmdResult> s_results;
constexpr uint32_t CMD_TIMEOUT_MS = 3000;

static void addResult(const String& pcId, const String& cmd, bool ok, const String& msg) {
  const PcSource* pc = pcSource(pcId);
  if (s_results.size() < 8) s_results.push_back({pc ? String(pc->name) : String("PC"), cmd, msg, ok});
}

static void handleAck(JsonDocument& doc) {
  const String nonce = doc["ack"] | "";
  for (size_t i = 0; i < s_pending.size(); i++) {
    if (s_pending[i].nonce != nonce) continue;
    addResult(s_pending[i].pcId, s_pending[i].cmd, doc["ok"] | false, doc["msg"] | "");
    s_pending.erase(s_pending.begin() + i);
    return;
  }
}

// Bump allocator for the once-a-second packet document: the heap costs about
// a millisecond per packet. Blocks carry their size in front; nothing is freed.
struct PacketArena : ArduinoJson::Allocator {
  uint8_t* base = nullptr;
  uint8_t* last = nullptr;
  size_t cap = 0, used = 0;
  void* allocate(size_t n) override {
    n = (n + 3) & ~(size_t)3;
    if (used + 4 + n > cap) return nullptr;
    memcpy(base + used, &n, 4);
    last = base + used + 4;
    used += 4 + n;
    return last;
  }
  void deallocate(void*) override {}
  void* reallocate(void* p, size_t n) override {
    if (!p) return allocate(n);
    n = (n + 3) & ~(size_t)3;
    uint32_t old;
    memcpy(&old, (uint8_t*)p - 4, 4);
    if (p == last) {  // the newest block grows or shrinks in place
      const size_t start = (uint8_t*)p - base;
      if (start + n > cap) return nullptr;
      memcpy((uint8_t*)p - 4, &n, 4);
      used = start + n;
      return p;
    }
    if (n <= old) return p;
    void* q = allocate(n);
    if (q) memcpy(q, p, old);
    return q;
  }
};
static PacketArena s_arena;
constexpr size_t PACKET_ARENA_BYTES = 8192;  // a 1.4 KB packet takes 3.2 KB

static void arenaBegin() {
  s_arena.base = (uint8_t*)heap_caps_malloc(PACKET_ARENA_BYTES, MALLOC_CAP_SPIRAM);
  s_arena.cap = s_arena.base ? PACKET_ARENA_BYTES : 0;
}

static void parsePacket(const char* json, IPAddress from, bool serial) {
  s_arena.used = 0;
  s_arena.last = nullptr;
  JsonDocument doc(&s_arena);
  DeserializationError err = deserializeJson(doc, json);
  if (err == DeserializationError::NoMemory) {  // no arena, or a packet too rich for it
    doc = JsonDocument();
    err = deserializeJson(doc, json);
  }
  if (err) {
    Serial.printf("pc_metrics: JSON parse error: %s\n", err.c_str());
    return;
  }
  if (!doc["ack"].isNull()) {
    handleAck(doc);
    return;
  }
  PcSource* pc = sourceForPacket(doc["pc_id"] | "", doc["pc_name"] | "");
  if (!pc) return;
  pc->viaSerial = serial;
  // Over USB the companion names its LAN address: covers still come over WiFi.
  if (!serial || !pc->ip.fromString(doc["ip"] | "")) pc->ip = from;
  pc->cmdPort = doc["cmd_port"] | 0;
  pc->artPort = (uint16_t)constrain(doc["art_port"] | 0, 0, 65535);
  // A few nonces per packet, so quick taps (volume) need not wait for the next one.
  memset(pc->nonces, 0, sizeof(pc->nonces));
  int n = 0;
  for (JsonVariantConst v : doc["nonces"].as<JsonArrayConst>())
    if (n < 4) strlcpy(pc->nonces[n++], v | "", sizeof(pc->nonces[0]));
  PcMetricData& d = pc->data;

  PcState& st = pc->state;
  JsonObjectConst ps = doc["pcstate"];
  st.vol = ps["vol"].is<int>() ? (int8_t)constrain(ps["vol"].as<int>(), 0, 100) : -1;
  st.mute = ps["mute"].is<bool>() ? (int8_t)ps["mute"].as<bool>() : -1;
  JsonObjectConst media = ps["media"];
  const char* ms = media["status"] | "";
  st.media = media.isNull() ? -1 : !strcmp(ms, "playing") ? PC_MEDIA_PLAYING
                                  : !strcmp(ms, "paused") ? PC_MEDIA_PAUSED : PC_MEDIA_STOPPED;
  strlcpy(st.title, media["title"] | "", sizeof(st.title));
  strlcpy(st.artist, media["artist"] | "", sizeof(st.artist));
  st.dur = max((int32_t)0, media["dur"] | (int32_t)0);
  st.pos = max((int32_t)0, media["pos"] | (int32_t)0);
  st.t0 = max((int32_t)0, media["t0"] | (int32_t)0);
  uint8_t art[4];
  st.art[0] = 0;
  if (fromHex(media["art"] | (const char*)nullptr, art, sizeof(art))) toHex(art, sizeof(art), st.art);
  asciiPunct(st.title);
  asciiPunct(st.artist);
  // A state-only packet (sent when the state changes between metrics
  // packets) leaves the metrics and the chart history alone.
  if (doc["metrics"].isNull() && !ps.isNull()) {
    d.lastReceived = millis();
    d.online = true;
    s_version++;
    return;
  }

  d.status = doc["status"] | PC_STATUS_OK;

  const char* ts = doc["timestamp"];
  if (ts && ts[0] && d.status == PC_STATUS_OK) {
    strncpy(d.timestamp, ts, sizeof(d.timestamp) - 1);
    d.timestamp[sizeof(d.timestamp) - 1] = '\0';
  }
  // Empty timestamp (stale frame) or an unhealthy status: keep the last-OK time
  // rather than tracking the current one.

  JsonArray arr = doc["metrics"];
  d.count = 0;
  for (JsonObject mo : arr) {
    if (d.count >= MAX_METRICS) break;
    PcMetric& m = d.metrics[d.count];

    m.id = mo["id"] | 0;

    const char* name = mo["name"];
    if (name) {
      strncpy(m.name, name, sizeof(m.name) - 1);
      m.name[sizeof(m.name) - 1] = '\0';
      trimTrailingSpaces(m.name);
    } else {
      m.name[0] = '\0';
    }

    const char* unit = mo["unit"];
    if (unit) {
      strncpy(m.unit, unit, sizeof(m.unit) - 1);
      m.unit[sizeof(m.unit) - 1] = '\0';
    } else {
      m.unit[0] = '\0';
    }

    m.value = mo["value"] | 0.0f;

    // Throughput arrives PRE-MULTIPLIED BY TEN. The Windows companion packs
    // NIC readings as an integer number of tenths of a KB/s so one decimal
    // survives the wire (see _parse_rest_value in touchdeck_companion.py: "the
    // ESP32 divides by 10 when showing"), and this firmware never did - so a
    // 366.8 KB/s link rendered as "3668 KB/s". Undo it once, here, so the
    // history rings and the metric tiles see real KB/s.
    //
    // Keyed on the exact "KB/s" string because that scaling belongs to that
    // one unit: the Linux companion reports "MB/s" unscaled, and both must
    // stay correct.
    if (strcmp(m.unit, "KB/s") == 0) m.value /= 10.0f;

    d.count++;
  }

  d.lastReceived = millis();
  d.sampleMs = d.lastReceived;
  d.online = true;
  s_version++;

  // One history sample per METRIC per packet. Rings are keyed by metric id, so
  // nothing here needs invalidating when a slot is rebound or the metric page
  // changes - the ring follows the metric. A metric missing from this packet
  // simply does not advance, leaving a gap rather than a fake sample.
  for (uint8_t i = 0; i < d.count; i++) {
    const PcMetric& m = d.metrics[i];
    if (m.id == 0 || m.id > MAX_METRICS) continue;
    SlotHistory& h = pc->hist[m.id];
    h.metricId = m.id;
    h.samples[h.head] = m.value;
    h.head = (uint8_t)((h.head + 1) % PC_HISTORY_LEN);
    if (h.count < PC_HISTORY_LEN) h.count++;
  }
}

// USB cable: the companion writes "@TD <json>" lines (the same JSON as UDP)
// between the device's log lines; "@TD {"hello":1}" asks who is there.
// Read on a core 0 task (the core's RX costs ~3 us a byte); the loop parses.
static void serialTask(void*) {
  static char line[PC_PACKET_BUF_SIZE];
  size_t len = 0;
  bool over = false;
  uint8_t chunk[256];
  for (;;) {
    if (improvSetupActive()) {  // Improv owns the port in setup mode
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const size_t got = Serial.read(chunk, sizeof(chunk));
    if (!got) {
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }
    for (size_t i = 0; i < got; i++) {
      const char c = (char)chunk[i];
      if (c != '\n') {
        if (len < sizeof(line) - 1) line[len++] = c;
        else over = true;
        continue;
      }
      if (len && line[len - 1] == '\r') len--;
      line[len] = 0;
      const bool ours = !over && len > 4 && !strncmp(line, "@TD ", 4);
      const size_t n = len - 4;
      len = 0;
      over = false;
      if (!ours) continue;
      if (!strncmp(line + 4, "{\"hello\":", 9)) {
        Serial.printf("\n@TD {\"hello\":\"%s\",\"fw\":\"%s\"}\n", PRODUCT_NAME, FW_VERSION);
        continue;
      }
      char* copy = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
      if (!copy) continue;
      memcpy(copy, line + 4, n + 1);
      if (xQueueSend(s_lineQ, &copy, 0) != pdTRUE) heap_caps_free(copy);
    }
  }
}

static bool serialPoll() {
  bool parsed = false;
  char* payload;
  while (s_lineQ && xQueueReceive(s_lineQ, &payload, 0) == pdTRUE) {
    parsePacket(payload, IPAddress(), true);
    heap_caps_free(payload);
    parsed = true;
  }
  return parsed;
}

static bool s_paused = false;

void pcMetricsPause(bool pause) {
  if (pause == s_paused) return;
  s_paused = pause;
  if (pause) {
    udp.stop();
    return;
  }
  udp.begin(UDP_PORT);
  for (int i = 0; i < MAX_PCS; i++)
    if (pcSources[i].used) pcSources[i].data.lastReceived = millis();
}

bool pcMetricsHandle() {
  bool parsed = serialPoll();
  // Spectrum packets come ~50/s: drain a few per pass so a stalled loop shows
  // the newest audio, not a backlog.
  for (int n = 0; n < 8 && !s_paused; n++) {
    const int packetSize = udp.parsePacket();
    if (!packetSize) break;
    static char buffer[PC_PACKET_BUF_SIZE];
    // Reject oversize before reading so a truncated body is never parsed.
    if (packetSize > (int)sizeof(buffer) - 1) {
      Serial.printf("pc_metrics: packet %d bytes exceeds buffer %d, discarding\n",
                    packetSize, (int)sizeof(buffer));
      udp.flush();
      continue;
    }
    int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) continue;
    if (vizIngest((const uint8_t*)buffer, len)) continue;
    buffer[len] = '\0';
    parsePacket(buffer, udp.remoteIP(), false);
    parsed = true;
  }

  for (size_t i = 0; i < s_pending.size();) {
    if (millis() - s_pending[i].sentMs < CMD_TIMEOUT_MS) {
      i++;
      continue;
    }
    addResult(s_pending[i].pcId, s_pending[i].cmd, false, "no answer");
    s_pending.erase(s_pending.begin() + i);
  }

  // Liveness: drop to offline when no packet has arrived for the stale window.
  for (int i = 0; i < MAX_PCS; i++) {
    PcMetricData& d = pcSources[i].data;
    if (!s_paused && pcSources[i].used && d.online && millis() - d.lastReceived > PC_STALE_TIMEOUT_MS) {
      d.online = false;
      s_version++;
      Serial.printf("pc_metrics: PC '%s' offline (no packets within stale window)\n", pcSources[i].name);
    }
  }
  return parsed;
}

PcSource* pcSource(const String& id) {
  if (!pcSources) return nullptr;
  if (!id.length()) return pcSources[0].used ? &pcSources[0] : nullptr;
  for (int i = 0; i < MAX_PCS; i++)
    if (pcSources[i].used && id == pcSources[i].id) return &pcSources[i];
  return nullptr;
}

int pcOnlineCount() {
  int n = 0;
  for (int i = 0; pcSources && i < MAX_PCS; i++) n += pcSources[i].used && pcSources[i].data.online;
  return n;
}

bool pcAnyOnline() { return pcOnlineCount() > 0; }

uint32_t pcVersion() { return s_version; }

bool pcForget(const String& id) {
  for (int i = 0; i < MAX_PCS; i++) {
    if (!pcSources[i].used || id != pcSources[i].id) continue;
    // Keep the order (the first one is the default): close the gap.
    for (int j = i; j + 1 < MAX_PCS; j++) memcpy(&pcSources[j], &pcSources[j + 1], sizeof(PcSource));
    memset(&pcSources[MAX_PCS - 1], 0, sizeof(PcSource));
    saveKnown();
    s_version++;
    return true;
  }
  return false;
}

bool pcSendCommand(const String& pcId, const char* cmd, String& err) {
  PcSource* pc = pcSource(pcId);
  if (!pc) {
    err = "unknown PC";
    return false;
  }
  if (!pc->paired) {
    err = String(pc->name) + " is not paired";
    return false;
  }
  if (!pc->data.online || (!pc->cmdPort && !pc->viaSerial)) {
    err = String(pc->name) + (pc->data.online ? " does not take commands" : " is offline");
    return false;
  }
  char* nonce = nullptr;
  for (auto& n : pc->nonces)
    if (n[0]) nonce = n;
  if (!nonce) {
    err = "too fast, try again";
    return false;
  }
  if (s_pending.size() >= 4) {
    err = "busy, try again";
    return false;
  }
  const String msg = String(pc->id) + "|" + nonce + "|" + cmd;
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), pc->key, 32,
                  (const uint8_t*)msg.c_str(), msg.length(), mac);
  char sig[65];
  toHex(mac, 32, sig);
  JsonDocument doc;
  doc["pc"] = pc->id;
  doc["cmd"] = cmd;
  doc["nonce"] = nonce;
  doc["sig"] = sig;
  String out;
  serializeJson(doc, out);
  if (pc->viaSerial) {
    // A leading newline ends any half-written log line first.
    Serial.printf("\n@TD %s\n", out.c_str());
  } else {
    udp.beginPacket(pc->ip, pc->cmdPort);
    udp.write((const uint8_t*)out.c_str(), out.length());
    if (!udp.endPacket()) {
      err = "send failed";
      return false;
    }
  }
  s_pending.push_back({pc->id, nonce, cmd, millis()});
  nonce[0] = 0;  // single use; the next metrics packet brings new ones
  return true;
}

bool pcTakeCmdResult(PcCmdResult& out) {
  if (s_results.empty()) return false;
  out = s_results.front();
  s_results.erase(s_results.begin());
  return true;
}

// --- Pairing -----------------------------------------------------------------
static struct {
  String id;
  char code[7];    // code mode; "" in tap mode
  char token[33];  // tap mode: only the companion that started it may collect the key
  bool allowed;    // tap mode: the user tapped Allow
  uint8_t key[32];
  uint32_t until;  // millis deadline, 0 = no pairing
  uint8_t tries;
} s_pair = {};
constexpr uint32_t PAIR_MS = 120000;

bool pcPairStart(const String& id, bool tap, String& token, String& err) {
  PcSource* pc = pcSource(id);
  if (!pc || id != pc->id) {
    err = "unknown PC: start the companion's metric stream to this device first";
    return false;
  }
  s_pair.id = id;
  s_pair.code[0] = s_pair.token[0] = 0;
  if (tap) {
    uint8_t t[16];
    esp_fill_random(t, sizeof(t));
    toHex(t, sizeof(t), s_pair.token);
    token = s_pair.token;
  } else {
    snprintf(s_pair.code, sizeof(s_pair.code), "%06lu", (unsigned long)(esp_random() % 1000000));
  }
  s_pair.allowed = false;
  esp_fill_random(s_pair.key, sizeof(s_pair.key));
  s_pair.until = millis() + PAIR_MS;
  if (!s_pair.until) s_pair.until = 1;
  s_pair.tries = 0;
  Serial.printf("pc_metrics: pairing '%s' started (%s)\n", pc->name, tap ? "tap" : "code");
  return true;
}

bool pcPairConfirm(const String& id, const String& code, const String& token, String& keyHex,
                   bool& waiting, String& err) {
  waiting = false;
  if (!s_pair.until || (int32_t)(millis() - s_pair.until) >= 0 || id != s_pair.id) {
    err = "no pairing in progress for this PC";
    return false;
  }
  const bool tap = s_pair.token[0] != 0;
  if (tap ? token != s_pair.token : code != s_pair.code) {
    if (++s_pair.tries >= 5) pcPairCancel();
    err = tap ? "wrong token" : "wrong code";
    return false;
  }
  if (tap && !s_pair.allowed) {
    waiting = true;
    err = "waiting for Allow on the device";
    return false;
  }
  PcSource* pc = pcSource(id);
  if (!pc) {
    pcPairCancel();
    err = "unknown PC";
    return false;
  }
  memcpy(pc->key, s_pair.key, 32);
  pc->paired = true;
  saveKnown();
  char k[65];
  toHex(s_pair.key, 32, k);
  keyHex = k;
  pcPairCancel();
  s_version++;
  Serial.printf("pc_metrics: '%s' paired\n", pc->name);
  return true;
}

void pcPairAllow() {
  if (s_pair.until && s_pair.token[0]) s_pair.allowed = true;
}

void pcPairCancel() {
  s_pair.until = 0;
  s_pair.id = "";
  s_pair.allowed = false;
  s_pair.code[0] = s_pair.token[0] = 0;
  memset(s_pair.key, 0, sizeof(s_pair.key));
}

bool pcPairPending(String& pcName, String& code) {
  if (!s_pair.until) return false;
  if ((int32_t)(millis() - s_pair.until) >= 0) {
    pcPairCancel();
    return false;
  }
  if (s_pair.allowed) return false;  // answered; the companion collects the key
  const PcSource* pc = pcSource(s_pair.id);
  pcName = pc ? pc->name : "PC";
  code = s_pair.code;
  return true;
}

bool pcPairedState(const String& id, bool& paired) {
  const PcSource* pc = id.length() ? pcSource(id) : nullptr;
  if (!pc || id != pc->id) return false;
  paired = pc->paired;
  return true;
}

const PcState* pcStateOf(const PcSource* pc) {
  return pc && pc->data.online ? &pc->state : nullptr;
}

const PcMetric* pcMetricFind(const PcSource* pc, uint8_t id) {
  if (!pc || !pc->data.online) return nullptr;
  for (uint8_t i = 0; i < pc->data.count; i++)
    if (pc->data.metrics[i].id == id) return &pc->data.metrics[i];
  return nullptr;
}
