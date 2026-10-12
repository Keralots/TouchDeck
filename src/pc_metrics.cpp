// PC stats ingest: a WiFiUDP listener that decodes the companion's V2.1 JSON
// push packets into pcData. Parse logic ported from SmallOLED-PCMonitor
// network.cpp parseStatsV2, slimmed to the color project's metric model.
//
// WiFiUDP (not AsyncUDP) on purpose: AsyncUDP froze on ESP32 in the sibling
// projects. The socket is polled from loop() via pcMetricsHandle().
#include "pc_metrics.h"
#include <WiFiUdp.h>
#include <ArduinoJson.h>

PcMetricData pcData = {};
SlotHistory pcHistoryById[MAX_METRICS + 1] = {};

static WiFiUDP udp;

void pcMetricsBegin() {
  pcData.count = 0;
  pcData.online = false;
  pcData.status = PC_STATUS_OK;
  pcData.lastReceived = 0;
  pcData.timestamp[0] = '\0';
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

static void parsePacket(const char* json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("pc_metrics: JSON parse error: %s\n", err.c_str());
    return;
  }

  pcData.status = doc["status"] | PC_STATUS_OK;

  const char* ts = doc["timestamp"];
  if (ts && ts[0]) {
    strncpy(pcData.timestamp, ts, sizeof(pcData.timestamp) - 1);
    pcData.timestamp[sizeof(pcData.timestamp) - 1] = '\0';
  }
  // Empty timestamp = stale frame from the companion (LHM recovering); keep the
  // previous one rather than blanking it.

  JsonArray arr = doc["metrics"];
  pcData.count = 0;
  for (JsonObject mo : arr) {
    if (pcData.count >= MAX_METRICS) break;
    PcMetric& m = pcData.metrics[pcData.count];

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
    // survives the wire (see _parse_rest_value in statnode_companion.py: "the
    // ESP32 divides by 10 when showing"), and this firmware never did - so a
    // 366.8 KB/s link rendered as "3668 KB/s". Undo it once, here, so the
    // history rings and the metric tiles see real KB/s.
    //
    // Keyed on the exact "KB/s" string because that scaling belongs to that
    // one unit: the Linux companion reports "MB/s" unscaled, and both must
    // stay correct.
    if (strcmp(m.unit, "KB/s") == 0) m.value /= 10.0f;

    pcData.count++;
  }

  pcData.lastReceived = millis();
  pcData.online = true;

  // One history sample per METRIC per packet. Rings are keyed by metric id, so
  // nothing here needs invalidating when a slot is rebound or the metric page
  // changes - the ring follows the metric. A metric missing from this packet
  // simply does not advance, leaving a gap rather than a fake sample.
  for (uint8_t i = 0; i < pcData.count; i++) {
    const PcMetric& m = pcData.metrics[i];
    if (m.id == 0 || m.id > MAX_METRICS) continue;
    SlotHistory& h = pcHistoryById[m.id];
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
        parsePacket(buffer);
        parsed = true;
      }
    }
  }

  // Liveness: drop to offline when no packet has arrived for the stale window.
  if (pcData.online && (millis() - pcData.lastReceived > PC_STALE_TIMEOUT_MS)) {
    pcData.online = false;
    Serial.println("pc_metrics: PC offline (no packets within stale window)");
  }
  return parsed;
}

const PcMetric* pcMetricFindById(uint8_t id) {
  for (uint8_t i = 0; i < pcData.count; i++) {
    if (pcData.metrics[i].id == id) return &pcData.metrics[i];
  }
  return nullptr;
}

const PcMetric* pcMetricFindByName(const char* name) {
  if (!name) return nullptr;
  for (uint8_t i = 0; i < pcData.count; i++) {
    if (strcmp(pcData.metrics[i].name, name) == 0) return &pcData.metrics[i];
  }
  return nullptr;
}
