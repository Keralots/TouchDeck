// Guition deck: glue between deck.json printers, their secrets (NVS) and
// deck_bambu. A printer is powered while its HA plug is on (or always, with
// no plug); only powered printers connect and get their pages.
#ifndef DECK_PRINTERS_H
#define DECK_PRINTERS_H

#include <Arduino.h>
#include "deck_config.h"

namespace deck {

// Hand the configured printers, with their stored secrets, to deck_bambu.
void printersApply(const DeckConfig& cfg);
// Store a printer's serial and access code; an empty one keeps what is stored.
bool printerSecretSet(const String& id, const String& serial, const String& code);
bool printerHasSecret(const String& id);
// Follow the plugs; true when a printer was powered on or off.
bool printersPowerSync(const DeckConfig& cfg);

}  // namespace deck

#endif  // DECK_PRINTERS_H
