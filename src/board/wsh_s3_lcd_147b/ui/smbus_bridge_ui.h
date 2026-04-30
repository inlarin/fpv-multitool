#pragma once

// USB2SMBus app — runs the serial bridge + shows live stats on LCD.
// Exits on BOOT long-press.
//
// Waveshare-only screen flow. The BridgeStats counters this UI reads
// from now live in the shared battery/smbus_bridge.h so non-Waveshare
// consumers (web stats endpoint, SC01 Plus future screen) can read
// them without pulling in this board-specific header.
void runUSB2SMBus();
