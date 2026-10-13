#ifndef PC_METRICS_H
#define PC_METRICS_H

#include <Arduino.h>
#include <IPAddress.h>
#include "config.h"

// One metric as pushed by the PC companion (V2.1 protocol). Metric tiles bind
// to it by id.
struct PcMetric {
  uint8_t id;            // 1-based stable id
  char    name[16];      // short sensor name from the companion ("CPU","GPUT")
  char    unit[8];       // "%", "C", "W", "RPM", "MB", ...
  float   value;
};

// Latest decoded packet + liveness.
struct PcMetricData {
  PcMetric metrics[MAX_METRICS];
  uint8_t  count;
  char     timestamp[6];   // "HH:MM" from the companion
  uint8_t  status;         // PC_STATUS_* (LHM health reported by the companion)
  bool     online;         // a fresh packet arrived within PC_STALE_TIMEOUT_MS
  uint32_t lastReceived;   // millis() of the last valid packet
};

// Value history for the chart tiles, one sample per received packet, keyed by
// metric id. Every metric in the packet is sampled, so a new chart tile starts
// warm.
#define PC_HISTORY_LEN 60

struct SlotHistory {
  uint8_t metricId;   // metric id the samples belong to; 0 for the empty ring
  uint8_t count;      // valid samples (0..PC_HISTORY_LEN)
  uint8_t head;       // next write index
  float   samples[PC_HISTORY_LEN];
};

// Sample oldest->newest: i in [0, count).
static inline float pcHistoryAt(const SlotHistory& h, uint8_t i) {
  return h.samples[(uint8_t)((h.head + PC_HISTORY_LEN - h.count + i) % PC_HISTORY_LEN)];
}

// One PC running the companion. Packets carry "pc_id" and "pc_name"; a
// companion without them is the PC with id "". The list of known PCs (id,
// name, in the order first seen) is kept in NVS "deckpc", so tiles bound to
// a PC survive reboots; the first one is the default PC.
#define MAX_PCS 4

struct PcSource {
  bool     used;
  char     id[17];
  char     name[24];
  IPAddress ip;          // where its last packet came from
  uint16_t cmdPort;      // its command listener ("cmd_port"), 0 = none
  bool     viaSerial;    // its latest packet came over the USB cable
  char     nonces[4][17];  // one-time nonces from its latest packet, "" = used
  bool     paired;
  uint8_t  key[32];      // shared HMAC key from pairing (NVS only, never in the API)
  PcMetricData data;
  SlotHistory hist[MAX_METRICS + 1];  // by metric id; index 0 stays empty
};

// MAX_PCS entries, in PSRAM (each holds ~8 KB of history).
extern PcSource* pcSources;

// Start the UDP listener on UDP_PORT and load the known PCs.
void pcMetricsBegin();
// Service the socket: parse any pending packet, refresh the online flags.
// Returns true if a new packet was parsed this call.
bool pcMetricsHandle();
// Re-bind the socket after a WiFi reconnect (old fd goes stale).
void pcMetricsRestartUdp();

// The PC a tile names: id "" = the default (first known) PC. Null when none.
PcSource* pcSource(const String& id);
bool pcAnyOnline();
int pcOnlineCount();
// Bumps on every packet and online change of any PC.
uint32_t pcVersion();
// Drop a PC from the known list (portal). False when it is not known.
bool pcForget(const String& id);

// Lookup for the metric tiles. Null when the PC is offline or lacks the id.
const PcMetric* pcMetricFind(const PcSource* pc, uint8_t id);

// --- PC control -------------------------------------------------------------
// A command goes to the PC's cmd_port as {"pc","cmd","nonce","sig"}, sig =
// hex HMAC-SHA256(key, "pc_id|nonce|cmd") over one of the "nonces" from its
// latest metrics packet (single use on the PC side). The PC answers {"ack": nonce,
// "ok", "msg"} to the device's port 4210.
//
// Pairing (the companion calls it): pcPairStart shows a 6-digit code on the
// device; pcPairConfirm with that code returns the new key once.
// Over the USB cable the same JSON travels as "@TD <json>" lines on Serial.
bool pcSendCommand(const String& pcId, const char* cmd, String& err);
struct PcCmdResult {
  String pcName, cmd, msg;
  bool ok;
};
// Next finished command (acked, refused or timed out), if any.
bool pcTakeCmdResult(PcCmdResult& out);

bool pcPairStart(const String& id, String& err);
// keyHex is filled on success. A wrong code counts toward a limit of 5.
bool pcPairConfirm(const String& id, const String& code, String& keyHex, String& err);
void pcPairCancel();
// For the device's pairing card: true while a pairing waits for its code.
bool pcPairPending(String& pcName, String& code);

#endif // PC_METRICS_H
