#pragma once
#include <Arduino.h>

// BOOT button handler with click/double-click/long-press detection
enum ButtonEvent {
    BTN_NONE,
    BTN_CLICK,
    BTN_DOUBLE_CLICK,
    BTN_LONG_PRESS,
};

namespace Button {

void init(uint8_t pin);
ButtonEvent poll();  // Call in loop(), returns event when detected

} // namespace Button
