#include "button.h"

static uint8_t s_pin;
static bool s_lastState = HIGH;
static uint32_t s_pressTime = 0;
static uint32_t s_releaseTime = 0;
static uint8_t s_clickCount = 0;
static bool s_longFired = false;

// Tuned for comfortable single-button use:
//   DEBOUNCE: filter out contact bounce
//   LONG_PRESS: hold longer to avoid accidental long-press during double-tap
//   DOUBLE_CLICK: wider window tolerates slower users
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_PRESS_MS = 800;
static const uint32_t DOUBLE_CLICK_MS = 500;

void Button::init(uint8_t pin) {
    s_pin = pin;
    pinMode(s_pin, INPUT_PULLUP);
}

ButtonEvent Button::poll() {
    bool state = digitalRead(s_pin);
    uint32_t now = millis();

    // Button pressed (active LOW)
    if (state == LOW && s_lastState == HIGH) {
        s_pressTime = now;
        s_longFired = false;
    }

    // Button held — long press
    if (state == LOW && !s_longFired && (now - s_pressTime > LONG_PRESS_MS)) {
        s_longFired = true;
        s_clickCount = 0;
        s_lastState = state;
        return BTN_LONG_PRESS;
    }

    // Button released
    if (state == HIGH && s_lastState == LOW) {
        if (!s_longFired && (now - s_pressTime > DEBOUNCE_MS)) {
            s_clickCount++;
            s_releaseTime = now;
        }
    }

    // Evaluate clicks after double-click window expires
    if (s_clickCount > 0 && (now - s_releaseTime > DOUBLE_CLICK_MS)) {
        ButtonEvent evt = (s_clickCount >= 2) ? BTN_DOUBLE_CLICK : BTN_CLICK;
        s_clickCount = 0;
        s_lastState = state;
        return evt;
    }

    s_lastState = state;
    return BTN_NONE;
}
