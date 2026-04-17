#pragma once
#include <Arduino.h>

// CP2112 USB HID-to-SMBus Bridge emulator.
// When USB descriptor includes HID, this module registers a HID interface
// with VID=0x10C4 PID=0xEA90 and emulates Silicon Labs CP2112 report
// protocol (per AN495). Windows loads the standard Silicon Labs driver
// and tools like DJI Battery Killer / bqStudio-lite / custom scripts talk
// transparently — no DLL replacement needed.

void CP2112_attach();     // called from UsbMode::applyAtBoot on init
void CP2112_loop();       // service pending I2C transfers — call from main loop
bool CP2112_isActive();   // true when host opened + configured
