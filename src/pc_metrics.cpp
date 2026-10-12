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

PcSource* pcSources = nullptr;

static WiFiUDP udp;
static uint32_t s_version = 1;

// Known PCs in NVS: "deckpc"/"list" = [{"id","name"}], first = default.
static void saveKnown() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < MAX_PCS; i++) {
    if (!pcSources[i].used) continue;
    JsonObject o = a.add<JsonObject>();
    o["id"] = pcSources[i].id;
    o["name"] = pcSources[i].name;
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

static void parsePacket(const char* json, IPAddress from) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("pc_metrics: JSON parse error: %s\n", err.c_str());
    return;
  }
  PcSource* pc = sourceForPacket(doc["pc_id"] | "", doc["pc_name"] | "");
  if (!pc) return;
  pc->ip = from;
  PcMetricData& d = pc->data;

  d.status = doc["status"] | PC_STATUS_OK;

  const char* ts = doc["timestamp"];
  if (ts && ts[0]) {
    strncpy(d.timestamp, ts, sizeof(d.timestamp) - 1);
    d.timestamp[sizeof(d.timestamp) - 1] = '\0';
  }
  // Empty timestamp = stale frame from the companion (LHM recovering); keep the
  // previous one rather than blanking it.

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

bool pcMetricsHandle() {
  bool parsed = false;
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
        parsePacket(buffer, udp.remoteIP());
        parsed = true;
      }
    }
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

const PcMetric* pcMetricFind(const PcSource* pc, uint8_t id) {
  if (!pc || !pc->data.online) return nullptr;
  for (uint8_t i = 0; i < pc->data.count; i++)
    if (pc->data.metrics[i].id == id) return &pc->data.metrics[i];
  return nullptr;
}
