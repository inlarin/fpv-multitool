#include "pin_port.h"
#include <Wire.h>
#include <HardwareSerial.h>
#include "pin_config.h"

namespace {

struct PortState {
    PortDef def;
    PortMode mode;
    const char *owner;
};

static PortState g_ports[PinPort::PORT_COUNT] = {
    // Port A: ELRS UART pins / alternate bus
    { {"A", 43, 44, 3, -1},  PORT_IDLE, nullptr },
    // Port B: DJI battery I2C pins
    { {"B", BATT_SDA, BATT_SCL, -1, -1}, PORT_IDLE, nullptr },
};

static void tearDown(int portId) {
    const PortState &p = g_ports[portId];
    switch (p.mode) {
    case PORT_I2C:
        // Port B uses Wire1, Port A (if configured as I2C) would use Wire
        if (portId == PinPort::PORT_B) Wire1.end();
        else Wire.end();
        break;
    case PORT_UART:
        if (portId == PinPort::PORT_A) Serial1.end();
        break;
    case PORT_SPI:
        // SPI handled by caller
        break;
    default:
        break;
    }
    // Return pins to floating inputs
    if (p.def.pin_a >= 0) pinMode(p.def.pin_a, INPUT);
    if (p.def.pin_b >= 0) pinMode(p.def.pin_b, INPUT);
}

} // namespace

bool PinPort::acquire(int portId, PortMode mode, const char *owner) {
    if (portId < 0 || portId >= PORT_COUNT) return false;
    PortState &p = g_ports[portId];
    if (p.mode != PORT_IDLE && p.mode != mode) {
        // already in use in different mode — caller should release first
        Serial.printf("[PinPort] port %s busy (mode=%d owner=%s), requested %d by %s\n",
                      p.def.name, (int)p.mode, p.owner ? p.owner : "?", (int)mode, owner);
        return false;
    }
    if (p.mode == mode) {
        // already acquired in same mode — ownership transfer
        p.owner = owner;
        return true;
    }
    tearDown(portId);
    p.mode = mode;
    p.owner = owner;
    Serial.printf("[PinPort] acquired %s in mode %d by %s (pins %d/%d)\n",
                  p.def.name, (int)mode, owner, p.def.pin_a, p.def.pin_b);
    return true;
}

void PinPort::release(int portId) {
    if (portId < 0 || portId >= PORT_COUNT) return;
    PortState &p = g_ports[portId];
    if (p.mode == PORT_IDLE) return;
    tearDown(portId);
    p.mode = PORT_IDLE;
    p.owner = nullptr;
}

PortMode PinPort::currentMode(int portId) {
    if (portId < 0 || portId >= PORT_COUNT) return PORT_IDLE;
    return g_ports[portId].mode;
}

const char *PinPort::currentOwner(int portId) {
    if (portId < 0 || portId >= PORT_COUNT) return nullptr;
    return g_ports[portId].owner;
}

const PortDef *PinPort::def(int portId) {
    if (portId < 0 || portId >= PORT_COUNT) return nullptr;
    return &g_ports[portId].def;
}
