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

constexpr const char *NVS_NS       = "pinport";
constexpr const char *NVS_KEY      = "modeB";      // per-port key; only Port B for now
constexpr const char *NVS_KEY_SWAP = "swapB";      // pin_a/pin_b swap flag

// In-memory cache of the swap flag so every pin query doesn't hit NVS.
// Loaded lazily on first call; written through setSwapped().
static bool g_swapB = false;
static bool g_swapBLoaded = false;

static bool loadSwap() {
    if (g_swapBLoaded) return g_swapB;
    Preferences p;
    p.begin(NVS_NS, true);
    g_swapB = p.getBool(NVS_KEY_SWAP, false);
    p.end();
    g_swapBLoaded = true;
    return g_swapB;
}

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
    loadSwap();  // warm cache
    if (pref == PORT_IDLE) {
        Serial.println("[PinPort] boot: Port B idle (no preferred mode)");
        return;
    }
    // Force-release anything stale (shouldn't happen on boot, but safe)
    release(PORT_B);
    if (acquire(PORT_B, pref, "boot")) {
        Serial.printf("[PinPort] boot: Port B pre-acquired as %s (swap=%d)\n",
                      modeName(pref), g_swapB);
    }
}

bool PinPort::swapped(int /*portId*/) {
    return loadSwap();
}

void PinPort::setSwapped(int portId, bool swap) {
    loadSwap();
    if (g_swapB == swap) return;
    g_swapB = swap;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBool(NVS_KEY_SWAP, swap);
    p.end();
    Serial.printf("[PinPort] swap=%d persisted\n", (int)swap);

    // If port is currently acquired, re-acquire so pins get re-applied.
    PortMode cur = currentMode(portId);
    if (cur != PORT_IDLE) {
        const char *owner = currentOwner(portId);
        // tearDown + re-acquire with same owner/mode (pin assignment
        // is read fresh from the swap flag by the consumer on re-init).
        release(portId);
        acquire(portId, cur, owner ? owner : "swap-re-apply");
    }
}

int PinPort::tx_pin(int portId) {
    const PortDef *d = def(portId); if (!d) return -1;
    return loadSwap() ? d->pin_b : d->pin_a;
}
int PinPort::rx_pin(int portId) {
    const PortDef *d = def(portId); if (!d) return -1;
    return loadSwap() ? d->pin_a : d->pin_b;
}
int PinPort::sda_pin(int portId) {
    const PortDef *d = def(portId); if (!d) return -1;
    return loadSwap() ? d->pin_b : d->pin_a;
}
int PinPort::scl_pin(int portId) {
    const PortDef *d = def(portId); if (!d) return -1;
    return loadSwap() ? d->pin_a : d->pin_b;
}
int PinPort::signal_pin(int portId) {
    const PortDef *d = def(portId); if (!d) return -1;
    // PWM has only one signal — always on pin_a regardless of swap.
    return d->pin_a;
}
