// Board selection. Each env in platformio.ini defines one BOARD_IS_<name>;
// everything board-specific (geometry, pins, which drivers) lives in
// include/boards/<name>.h. A new board = a new header + an entry here, plus a
// driver file only for hardware no existing driver covers.
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#if defined(BOARD_IS_JC3248W535)
#include "boards/jc3248w535.h"
#elif defined(BOARD_IS_WS_P4_7B)
#include "boards/ws_p4_7b.h"
#else
#error "No board selected: define BOARD_IS_<name> in the platformio.ini env"
#endif

// Every board header also defines boardPinReserved(pin, sdCard), the pins an
// optional module (LED, vibration motor) may not claim, and boardSdPin(pin),
// card pins that are free while the card is off.

#endif  // BOARD_H
