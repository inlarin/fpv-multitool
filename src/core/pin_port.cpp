#include "pin_port.h"
#include <Wire.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include "pin_config.h"

namespace {

struct PortState {
    PortDef def;
    PortMode mode;
    const char *owner;
};

static PortState g_ports[PinPort::PORT_COUNT] = {
    // Port B — GPIO 11 (pin_a) / GPIO 10 (pin_b). The only user port.
    { {"B", PORT_B_PIN_A, PORT_B_PIN_B, -1, -1}, PORT_IDLE, nullptr },
};

constexpr const char *NVS_NS  = "pinport";
constexpr const char *NVS_KEY = "modeB";      // per-port key; only Port B for now

static void tearDown(int portId) {
    const PortState &p = g_ports[portId];
    switch (p.mode) {
    case PORT_I2C:
        // Port B uses Wire1 exclusively
        Wire1.end();
        break;
    case PORT_UART:
        Serial1.end();
        break;
    case PORT_PWM:
        // ledcDetach on pin_a (consumer normally does this; no-op if already detached)
        if (p.def.pin_a >= 0) {
            ledcDetach(p.def.pin_a);
        }
        break;
    case PORT_GPIO:
    case PORT_IDLE:
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
        // Already owned in a different mode — caller must release first.
        // (Web UI shows this as "port busy" to the user.)
        Serial.printf("[PinPort] %s busy: mode=%s owner=%s, requested %s by %s\n",
                      p.def.name, modeName(p.mode), p.owner ? p.owner : "?",
                      modeName(mode), owner);
        return false;
    }
    if (p.mode == mode) {
        // Ownership transfer — same mode, different consumer.
        p.owner = owner;
        return true;
    }
    tearDown(portId);
    p.mode = mode;
    p.owner = owner;
    Serial.printf("[PinPort] %s acquired: mode=%s owner=%s (pins %d/%d)\n",
                  p.def.name, modeName(mode), owner, p.def.pin_a, p.def.pin_b);
    return true;
}

void PinPort::release(int portId) {
    if (portId < 0 || portId >= PORT_COUNT) return;
    PortState &p = g_ports[portId];
    if (p.mode == PORT_IDLE) return;
    Serial.printf("[PinPort] %s released (was mode=%s owner=%s)\n",
                  p.def.name, modeName(p.mode), p.owner ? p.owner : "?");
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

const char *PinPort::modeName(PortMode m) {
    switch (m) {
    case PORT_IDLE: return "IDLE";
    case PORT_I2C:  return "I2C";
    case PORT_UART: return "UART";
    case PORT_PWM:  return "PWM";
    case PORT_GPIO: return "GPIO";
    default:        return "?";
    }
}

PortMode PinPort::preferredMode(int /*portId*/) {
    Preferences p;
    p.begin(NVS_NS, true);
    uint8_t v = p.getUChar(NVS_KEY, (uint8_t)PORT_IDLE);
    p.end();
    if (v >= PORT_MODE_COUNT) v = PORT_IDLE;
    return (PortMode)v;
}

void PinPort::setPreferredMode(int /*portId*/, PortMode mode) {
    if (mode >= PORT_MODE_COUNT) mode = PORT_IDLE;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY, (uint8_t)mode);
    p.end();
}

void PinPort::applyAtBoot() {
    PortMode pref = preferredMode(PORT_B);
    if (pref == PORT_IDLE) {
        Serial.println("[PinPort] boot: Port B idle (no preferred mode)");
        return;
    }
    // Force-release anything stale (shouldn't happen on boot, but safe)
    release(PORT_B);
    if (acquire(PORT_B, pref, "boot")) {
        Serial.printf("[PinPort] boot: Port B pre-acquired as %s\n", modeName(pref));
    }
}
