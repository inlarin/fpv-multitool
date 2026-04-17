#pragma once
#include <Arduino.h>

// Dynamic pin-port manager.
// Each "port" is a group of GPIOs that can serve different bus roles at runtime.
// Prevents conflicts (only one owner at a time) and correctly tears down
// the previous mode (Wire.end(), Serial.end()) before reconfiguring.

enum PortMode {
    PORT_IDLE = 0,
    PORT_UART,       // TX/RX on pin_a/pin_b
    PORT_I2C,        // SDA=pin_a, SCL=pin_b
    PORT_SPI,        // MOSI=pin_a, SCLK=pin_b, (MISO=pin_c, CS=pin_d if present)
    PORT_GPIO,       // bare GPIOs for user code
    PORT_PWM,        // hardware PWM (one channel)
};

struct PortDef {
    const char *name;
    int pin_a;     // primary
    int pin_b;     // secondary
    int pin_c;     // tertiary (may be -1)
    int pin_d;     // quaternary (may be -1)
};

namespace PinPort {
    // Request exclusive access to a port in a specific mode.
    // Returns false if already owned by someone else.
    // `owner` is a short string for debugging/logs.
    bool acquire(int portId, PortMode mode, const char *owner);

    // Release a port. Any protocol object (Wire/Serial/SPI) is torn down.
    void release(int portId);

    // Query current state
    PortMode currentMode(int portId);
    const char *currentOwner(int portId);
    const PortDef *def(int portId);

    // Initial list of ports (board-specific, defined in pin_port.cpp)
    enum {
        PORT_A = 0,   // GPIO 43/44 (+3) — UART or I2C bus alternate
        PORT_B = 1,   // GPIO 11/10 — DJI battery I2C / secondary
        // PORT_C reserved for onboard QMI8658 (48/47) — managed separately
        PORT_COUNT
    };
}
