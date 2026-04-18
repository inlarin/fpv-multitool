#pragma once
#include <Arduino.h>

// Dynamic pin-port manager for Port B (GPIO 11/10).
//
// Port B is the single user-accessible signal bus on this board
// (pin headers GP10/GP11 + 5V + GND). It can serve any of the roles below,
// one at a time. PinPort guarantees proper tear-down of the previous mode
// (Wire1.end(), Serial1.end(), ledcDetach, etc.) before reconfiguring.
//
// The user's *preferred* mode is saved in NVS and re-applied on boot.
// Runtime acquire() can temporarily override the preferred mode for the
// duration of a task (e.g. SMBus scan while user is in UART mode) and
// should call release() afterwards to return to the preferred mode.

enum PortMode : uint8_t {
    PORT_IDLE = 0,   // all pins floating (INPUT)
    PORT_I2C  = 1,   // SDA=pin_a (11), SCL=pin_b (10)    — battery SMBus, CP2112 emu
    PORT_UART = 2,   // TX =pin_a (11), RX =pin_b (10)    — ELRS/CRSF/USB2TTL
    PORT_PWM  = 3,   // signal=pin_a (11), pin_b floating — servo, DShot motor, ESC one-wire
    PORT_GPIO = 4,   // pin_a & pin_b as raw GPIO         — sniffer PPM, bit-bang
    PORT_MODE_COUNT = 5,
};

struct PortDef {
    const char *name;
    int pin_a;     // primary   (GPIO 11)
    int pin_b;     // secondary (GPIO 10)
    int pin_c;     // reserved, -1
    int pin_d;     // reserved, -1
};

namespace PinPort {
    enum : int {
        PORT_B = 0,      // the only user port on this board
        PORT_COUNT = 1,
    };

    // Runtime port control -----------------------------------------------

    // Request exclusive access in a specific mode.
    // Returns false if busy (different mode) — caller should release() or wait.
    // `owner` is a short string for debugging.
    bool acquire(int portId, PortMode mode, const char *owner);

    // Release a port: any bus (Wire1/Serial1/PWM) is torn down, pins → INPUT.
    void release(int portId);

    // Query current state
    PortMode currentMode(int portId);
    const char *currentOwner(int portId);
    const PortDef *def(int portId);

    // Human-readable mode name (for UI/logs)
    const char *modeName(PortMode m);

    // Persistence --------------------------------------------------------

    // Read user's preferred boot-time mode from NVS (default PORT_IDLE).
    PortMode preferredMode(int portId);

    // Save user's preferred mode to NVS. Applied on next applyAtBoot()
    // (i.e. next reboot or immediate setPreferredMode() + release()).
    void setPreferredMode(int portId, PortMode mode);

    // Called from setup() once early: releases any owner and acquires
    // the preferred mode with owner="boot" so that sensors/features that
    // the user pre-selected start working immediately.
    void applyAtBoot();
}
