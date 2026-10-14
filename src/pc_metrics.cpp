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

void pcMetricsBegin() {
  pcSources = (PcSource*)heap_caps_calloc(MAX_PCS, sizeof(PcSource), MALLOC_CAP_SPIRAM);
  if (!pcSources) pcSources = (PcSource*)calloc(MAX_PCS, sizeof(PcSource));
  for (int i = 0; i < MAX_PCS; i++) pcSources[i].data.status = PC_STATUS_OK;
  loadKnown();
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

static void parsePacket(const char* json, IPAddress from, bool serial) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
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
  if (!serial) pc->ip = from;
  pc->cmdPort = doc["cmd_port"] | 0;
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
static char s_line[PC_PACKET_BUF_SIZE];
static size_t s_lineLen = 0;
static bool s_lineOver = false;

static bool serialPoll() {
  if (improvSetupActive()) return false;  // Improv owns the port in setup mode
  bool parsed = false;
  int budget = 4096;  // bytes per loop pass, so a flood cannot stall the UI
  while (budget-- > 0 && Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c != '\n') {
      if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = (char)c;
      else s_lineOver = true;
      continue;
    }
    if (s_lineLen && s_line[s_lineLen - 1] == '\r') s_lineLen--;
    s_line[s_lineLen] = 0;
    const bool ours = !s_lineOver && s_lineLen > 4 && !strncmp(s_line, "@TD ", 4);
    s_lineLen = 0;
    s_lineOver = false;
    if (!ours) continue;
    if (!strncmp(s_line + 4, "{\"hello\":", 9)) {
      Serial.printf("\n@TD {\"hello\":\"%s\",\"fw\":\"%s\"}\n", PRODUCT_NAME, FW_VERSION);
      continue;
    }
    parsePacket(s_line + 4, IPAddress(), true);
    parsed = true;
  }
  return parsed;
}

bool pcMetricsHandle() {
  bool parsed = serialPoll();
  int packetSize = udp.parsePacket();
  if (packetSize) {
    static char buffer[PC_PACKET_BUF_SIZE];
    // Reject oversize before reading so a truncated body is never parsed.
    if (packetSize > (int)sizeof(buffer) - 1) {
      Serial.printf("pc_metrics: packet %d bytes exceeds buffer %d, discarding\n",
                    packetSize, (int)sizeof(buffer));
      udp.flush();
    } else {
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        parsePacket(buffer, udp.remoteIP(), false);
        parsed = true;
      }
    }
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
    if (pcSources[i].used && d.online && millis() - d.lastReceived > PC_STALE_TIMEOUT_MS) {
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
  char code[7];
  uint8_t key[32];
  uint32_t until;  // millis deadline, 0 = no pairing
  uint8_t tries;
} s_pair = {};
constexpr uint32_t PAIR_MS = 120000;

bool pcPairStart(const String& id, String& err) {
  PcSource* pc = pcSource(id);
  if (!pc || id != pc->id) {
    err = "unknown PC: start the companion's metric stream to this device first";
    return false;
  }
  s_pair.id = id;
  snprintf(s_pair.code, sizeof(s_pair.code), "%06lu", (unsigned long)(esp_random() % 1000000));
  esp_fill_random(s_pair.key, sizeof(s_pair.key));
  s_pair.until = millis() + PAIR_MS;
  if (!s_pair.until) s_pair.until = 1;
  s_pair.tries = 0;
  Serial.printf("pc_metrics: pairing '%s' started\n", pc->name);
  return true;
}

bool pcPairConfirm(const String& id, const String& code, String& keyHex, String& err) {
  if (!s_pair.until || (int32_t)(millis() - s_pair.until) >= 0 || id != s_pair.id) {
    err = "no pairing in progress for this PC";
    return false;
  }
  if (code != s_pair.code) {
    if (++s_pair.tries >= 5) pcPairCancel();
    err = "wrong code";
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

void pcPairCancel() {
  s_pair.until = 0;
  s_pair.id = "";
  memset(s_pair.key, 0, sizeof(s_pair.key));
}

bool pcPairPending(String& pcName, String& code) {
  if (!s_pair.until) return false;
  if ((int32_t)(millis() - s_pair.until) >= 0) {
    pcPairCancel();
    return false;
  }
  const PcSource* pc = pcSource(s_pair.id);
  pcName = pc ? pc->name : "PC";
  code = s_pair.code;
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
