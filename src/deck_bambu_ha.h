// Guition deck: Bambu printers read from Home Assistant's Bambu Lab
// integration (ha-bambulab) instead of the printer's LAN MQTT. Entities are
// matched by their registry unique_id ("<serial>_<key>"), which does not
// depend on entity ids or the HA language. Fills the same PrinterState as
// deck_bambu; deck_bambu's read functions ask this module first.
#ifndef DECK_BAMBU_HA_H
#define DECK_BAMBU_HA_H

#include <Arduino.h>
#include <vector>
#include "deck_bambu.h"

namespace deck {

struct PrinterHaDef {
  String id;
  String serial;
};

void printersHaSet(const std::vector<PrinterHaDef>& list);
void printerHaPower(const String& id, bool on);
bool printerHaKnown(const String& id);
bool printerHaPowered(const String& id);
bool printerHaRead(const String& id, PrinterState& out);
bool printerHaLight(const String& id, bool on);
uint32_t printersHaVersion();
// Loop task: starts the entity lookup of a powered printer once HA is up.
void printersHaTick();
// Printers the integration knows, for the portal: {"printers": [{"serial",
// "name", "model"}]}. Blocking (about a second), loop task only.
bool printersHaFind(String& json, String& err);

}  // namespace deck

#endif  // DECK_BAMBU_HA_H
