#pragma once
#include <Arduino.h>

// DShot protocol implementation via ESP32-S3 RMT peripheral
// Supports DShot150, DShot300, DShot600

enum DShotSpeed {
    DSHOT150 = 150,
    DSHOT300 = 300,
    DSHOT600 = 600,
};

namespace DShot {

bool init(uint8_t pin, DShotSpeed speed);
void stop();
void sendThrottle(uint16_t throttle, bool telemetry = false);
void sendCommand(uint8_t cmd);  // DShot commands 0-47
void arm();                     // Send 0 throttle for arming sequence

} // namespace DShot
