#include "status_led.h"
#include "pin_config.h"
#include "web/wifi_manager.h"
#include "web/web_state.h"
#include "crsf/crsf_service.h"

namespace StatusLed {

static uint8_t s_brightness = 24;   // dim default — WS2812 at full blinds
static uint32_t s_last_push_ms = 0;
static uint8_t  s_last_r = 0, s_last_g = 0, s_last_b = 0;

void setBrightness(uint8_t b) { s_brightness = b; }

void init() {
    pinMode(RGB_LED_PIN, OUTPUT);
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
}

// Triangle wave 0..255..0 with period ms.
static uint8_t breathe(uint32_t now, uint32_t period_ms) {
    uint32_t t = now % period_ms;
    uint32_t half = period_ms / 2;
    uint32_t v = (t < half) ? (t * 255 / half) : ((period_ms - t) * 255 / half);
    return (uint8_t)v;
}

// Apply global brightness as a simple 8-bit multiply.
static uint8_t scale(uint8_t c, uint8_t k) {
    return (uint16_t)c * k / 255;
}

static void push(uint8_t r, uint8_t g, uint8_t b) {
    r = scale(r, s_brightness);
    g = scale(g, s_brightness);
    b = scale(b, s_brightness);
    if (r == s_last_r && g == s_last_g && b == s_last_b) return;
    neopixelWrite(RGB_LED_PIN, r, g, b);
    s_last_r = r; s_last_g = g; s_last_b = b;
}

void loop() {
    uint32_t now = millis();
    // Throttle to ~50 Hz — WS2812 update is cheap but the reset frame blocks
    // IRQs briefly and we don't want to interfere with RMT / UART work.
    if (now - s_last_push_ms < 20) return;
    s_last_push_ms = now;

    // Priority order: flash → CRSF linked → CRSF searching → STA → AP → idle.
    if (WebState::flashState.in_progress) {
        uint8_t v = breathe(now, 600);        // fast yellow pulse during flashing
        push(v, v, 0);
        return;
    }

    bool crsf_running = CRSFService::isRunning();
    if (crsf_running) {
        if (CRSFService::state().connected) {
            uint8_t v = breathe(now, 2500);   // calm cyan breathe when linked
            push(0, v, v);
        } else {
            uint8_t v = (now / 250) & 1 ? 255 : 0;  // red blink — no link yet
            push(v, 0, 0);
        }
        return;
    }

    switch (WifiManager::currentMode()) {
        case WifiManager::MODE_STA: {
            uint8_t v = breathe(now, 4000);   // slow green breathe — STA online
            push(0, v, 0);
            return;
        }
        case WifiManager::MODE_AP: {
            uint8_t v = breathe(now, 5000);   // slow blue/purple — AP waiting
            push(v / 3, 0, v);
            return;
        }
        default: break;
    }

    // Idle: barely-there white ember so the board still feels "on".
    uint8_t v = breathe(now, 6000) / 6;
    push(v, v, v);
}

} // namespace StatusLed
