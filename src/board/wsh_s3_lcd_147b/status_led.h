#pragma once
#include <Arduino.h>

// Ambient status indicator for the onboard WS2812.
// Polled from main loop — reads shared system state each tick and picks a
// color/pattern by priority. No per-module notification plumbing needed.
namespace StatusLed {

void init();
void loop();

// Global brightness scale, 0..255. Defaults to a dim level — the raw LED is
// blindingly bright at full intensity, especially on a dev board.
void setBrightness(uint8_t b);

} // namespace StatusLed
