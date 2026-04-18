#pragma once
#include <Arduino.h>

// BLHeli OneWire / 4way passthrough — minimal ESC info reader.
// WIP — full BLHeliSuite32 integration requires 4way-interface protocol
// implementation over TCP/CDC (see blheli-configurator source).
//
// Current support: detect ESC presence, read signature bytes via
// onewire protocol (half-duplex 19200 baud on signal wire).

struct BLHeliInfo {
    bool     detected;
    uint8_t  signature[3];      // signature bytes from ESC
    char     bootloaderId[32];  // ID string if BLHeli bootloader present
    uint16_t flashSize;
    char     firmwareName[16];
};

namespace BLHeli {
    // Probe ESC connected to signalPin (GPIO). Attempts BLHeli onewire handshake.
    // Returns true if ESC detected and info read successfully.
    bool probeInfo(int signalPin, BLHeliInfo *out, uint32_t timeoutMs = 2000);

    // Future:
    // - enterBootloader(pin)
    // - readEEPROM(pin, buf, len)
    // - writeEEPROM(pin, buf, len)
    // - 4way passthrough server over TCP port 4720 for BLHeliSuite32
}
