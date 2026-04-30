#pragma once
#include <Arduino.h>

// Serial-to-SMBus bridge for PC-side SLABHIDtoSMBus shim.
//
// Binary protocol over USB CDC @ 115200:
//   PC→ESP32:   [0xAA] [CMD] [ARG_LEN:u8] [ARG_BYTES...] [CRC8]
//   ESP32→PC:   [0x55] [STATUS] [RESP_LEN:u8] [RESP_BYTES...] [CRC8]
//
// Commands:
//   0x01 WRITE       args: [slave, reg, len, data...]
//   0x02 READ_WORD   args: [slave, reg]                    → resp: 2 bytes
//   0x03 READ_BLOCK  args: [slave, reg, maxLen]            → resp: N bytes
//   0x04 READ_RAW    args: [slave, len]                    → resp: N bytes (no reg)
//   0x05 ADDR_READ   args: [slave, targetLen, target..., readLen]
//                                                          → resp: N bytes
//   0x10 PING        args: []                               → resp: 'P','O','N','G'
//   0x11 RESCAN      args: []                               → reinit I2C
//
// Status bytes:
//   0x00 OK
//   0x01 I2C_NACK
//   0x02 I2C_TIMEOUT
//   0x03 INVALID_CMD
//   0x04 CRC_ERROR
//   0x05 OVERFLOW

namespace SMBusBridge {
    void begin();     // call from main setup after Serial.begin
    void loop();      // call from main loop
    bool isActive();  // true if bridge mode enabled (first valid packet received)
}

// Live counters incremented by the bridge as it processes packets.
// Defined in smbus_bridge.cpp (shared); the Waveshare smbus_bridge_ui.cpp
// reads these for the on-LCD stats screen.
//
// Lives in the shared header (not the Waveshare-only UI header) so that
// other consumers (web stats endpoint, future SC01 Plus screen) can read
// them without dragging in a board-specific header.
namespace BridgeStats {
    extern volatile uint32_t cmdWrite;
    extern volatile uint32_t cmdRead;
    extern volatile uint32_t cmdAddrRead;
    extern volatile uint32_t cmdPing;
    extern volatile uint32_t errCrc;
    extern volatile uint32_t errI2C;
    extern volatile uint32_t lastSlave;
    extern volatile uint32_t lastReg;
    extern volatile uint32_t lastStatus;
    void reset();
}
