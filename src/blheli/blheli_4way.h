#pragma once
#include <Arduino.h>

// BLHeli 4way-interface TCP server + OneWire bit-bang for ESC bootloader access.
//
// Protocol (4way, used by BLHeliSuite32):
//   Each frame: [0x2F] [cmd] [addr_hi] [addr_lo] [param_len] [params...] [crc_hi] [crc_lo]
//   Response:   [0x2E] [cmd] [addr_hi] [addr_lo] [data_len] [data...] [ack] [crc_hi] [crc_lo]
//   CRC16-CCITT (0x1021, init 0x0000) over entire frame up to CRC bytes.
//
// Connect BLHeliSuite32 → choose Interface: "4way" → TCP → host = board IP,
// port 4321 → Connect. Then select ESC index 0 (we only support one ESC).
//
// Signal wire connects to SIGNAL_OUT (GPIO 2) with proper BEC power to ESC.

namespace BLHeli4Way {

// Start TCP server on port 4321. Spawns task that accepts 4way commands and
// talks to ESC via OneWire on signal pin.
void start(int signalPin = 2);
void stop();
bool isRunning();

// Status / diag
struct Status {
    bool     clientConnected;
    uint32_t commandsHandled;
    uint32_t escReadBytes;
    uint32_t escWriteBytes;
    uint32_t escErrors;
    String   lastCmdName;
    uint8_t  escSignature[4];     // BL2 = [0xC8, 0xxx, 0xxx, 0xxx] typical
};
const Status &status();

} // namespace BLHeli4Way
