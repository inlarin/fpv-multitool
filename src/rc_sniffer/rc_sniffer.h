#pragma once
#include <Arduino.h>

// RC Protocol Sniffer — detects and parses SBUS, iBus, PPM on ELRS_RX (GPIO 44).
// Exposes live channel values + frame rate + detected protocol to web UI.

enum RCProto : uint8_t {
    RC_PROTO_NONE = 0,
    RC_PROTO_SBUS,       // 100000 baud, 8E2, inverted UART, 25-byte frame
    RC_PROTO_IBUS,       // 115200 baud, 8N1, 32-byte frame with checksum
    RC_PROTO_PPM,        // PWM pulse train on single pin, channels encoded as widths
};

struct RCState {
    RCProto  proto;
    bool     connected;         // frame received within last 500ms
    uint8_t  channelCount;      // 16 for SBUS, 14 for iBus, varies for PPM
    uint16_t channels[16];      // channel values in microseconds (988-2012 for CRSF-compatible)
    uint32_t frameCount;        // total frames received
    uint32_t crcErrors;         // bad frames
    uint32_t lastFrameMs;       // millis() of last good frame
    uint32_t frameRateHz;       // computed from recent frame timing
    bool     failsafe;          // SBUS failsafe bit
    bool     lostFrame;         // SBUS lost-frame bit
};

namespace RCSniffer {
    // Start with explicit protocol (SBUS / iBus / PPM).
    // For PPM: use GPIO interrupt. For UART protocols: use Serial1.
    void start(RCProto proto);

    // Attempt auto-detection by trying each protocol for 500ms.
    // On success sets internal protocol + starts sniffing.
    void autoDetect();

    void stop();
    bool isRunning();

    const RCState &state();
    const char *protoName(RCProto p);

    // Periodic tick — call from main loop. Polls UART / updates rate / failsafe timer.
    void loop();
}
