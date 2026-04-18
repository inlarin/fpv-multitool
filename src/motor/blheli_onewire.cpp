#include "blheli_onewire.h"

// BLHeli onewire protocol (minimal):
// - Half-duplex serial on signal wire
// - BLHeli_S: 19200 baud
// - BLHeli_32: 19200 for bootloader entry, then 115200 for data
// - Handshake: send BLHeliSync byte (0x56), ESC responds with its bootloader ID
// - Read signature: 0x43 command + address + length
//
// This is a skeleton — full implementation would need SoftwareSerial (half-duplex)
// with microsecond timing. Current stub reads back what's on the wire as diagnostic.

static const uint32_t BLHELI_BAUD = 19200;

bool BLHeli::probeInfo(int signalPin, BLHeliInfo *out, uint32_t timeoutMs) {
    if (!out) return false;
    *out = {};

    // Minimum viable probe: pin float + check pullup level.
    // If ESC present, it holds signal high via internal pullup + BEC.
    pinMode(signalPin, INPUT);
    delay(10);
    int highSamples = 0;
    uint32_t until = millis() + 100;
    while (millis() < until) {
        if (digitalRead(signalPin) == HIGH) highSamples++;
        delayMicroseconds(200);
    }
    // >80% high suggests ESC is connected with BEC pulling signal high
    if (highSamples < 400) {
        out->detected = false;
        return false;
    }

    // Placeholder: real BLHeli onewire handshake would:
    //   1. Drive signalPin low for 1ms as WAKE pulse
    //   2. Switch to OUTPUT, send 0x56 sync byte at 19200 baud (bit-banged)
    //   3. Switch to INPUT, wait for response (4 bytes: ID + version)
    //   4. Parse 4-byte response into signature
    //
    // Proper impl requires microsecond-precision bit-bang Serial on GPIO,
    // half-duplex switching, and tolerance for ESC variants.
    //
    // For now: return "detected but unknown" status.
    out->detected = true;
    strncpy(out->firmwareName, "unknown", sizeof(out->firmwareName) - 1);
    return true;
}
