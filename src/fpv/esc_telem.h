#pragma once
#include <Arduino.h>

// ESC telemetry decoder (KISS / BLHeli_32 10-byte format on 115200 8N1 UART).
// Connect ESC's telemetry wire to ELRS_RX (GPIO 44), ESC GND + 5V BEC.
//
// Frame (10 bytes):
//   [0]       temperature (°C, unsigned int8)
//   [1..2]    voltage (centivolts, big-endian u16)  → V = value / 100.0
//   [3..4]    current (centiamps, big-endian u16)
//   [5..6]    consumption (mAh, big-endian u16)
//   [7..8]    eRPM (rpm/100, big-endian u16)       → RPM = value * 100
//   [9]       CRC-8 (poly 0xD5, init 0) over bytes 0-8
//
// Frame rate: typically 1 frame / 20ms (single ESC), more sparse if shared bus.
// eRPM is electrical — divide by motor pole pairs for mechanical RPM.

struct ESCTelemFrame {
    uint8_t  temp_c;
    uint16_t voltage_cV;      // centivolts (divide by 100 for V)
    uint16_t current_cA;      // centiamps  (divide by 100 for A)
    uint16_t consumption_mAh;
    uint16_t erpm;            // raw eRPM / 100 (multiply by 100 for actual eRPM)
    bool     valid;
};

struct ESCTelemState {
    bool     running;
    bool     connected;          // got valid frame within last 500ms
    uint32_t frameCount;
    uint32_t crcErrors;
    uint32_t lastFrameMs;
    uint32_t frameRateHz;
    ESCTelemFrame last;          // most recent valid frame
    // Aggregates over session
    uint16_t maxTemp;
    uint16_t maxCurrent_cA;
    uint16_t maxErpm;
    uint32_t peakVoltage_cV;
    uint32_t minVoltage_cV;
};

namespace ESCTelem {
    void start(uint8_t poleCount = 14);  // default 14 = 7 pole pairs for typical FPV motors
    void stop();
    bool isRunning();
    const ESCTelemState &state();
    void loop();  // call from main loop — reads Serial1, parses frames
    uint8_t polePairs();
}
