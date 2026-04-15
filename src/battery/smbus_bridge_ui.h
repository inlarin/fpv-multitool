#pragma once
#include <stdint.h>

// USB2SMBus app — runs the serial bridge + shows live stats on LCD.
// Exits on BOOT long-press.
void runUSB2SMBus();

// Shared counters (populated by bridge handler)
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
