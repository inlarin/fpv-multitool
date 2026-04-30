#include "servo_pwm.h"
#include "core/pin_port.h"
#include "safety.h"
#include <driver/gpio.h>

// 16-bit at 50 Hz worked on the Waveshare board but ledcAttach returns
// false on the SC01 Plus -- LovyanGFX already holds LEDC channel 7
// (timer 3) for the LCD backlight. 14-bit lets the LEDC timer divider
// land cleanly.
//
// Crucially we MUST bind to a channel that doesn't share its timer with
// LovyanGFX's channel 7. ESP32-S3 LEDC channel/timer pairing:
//   channels 0,1 -> timer 0    <-- safe, our servo lives here
//   channels 2,3 -> timer 1
//   channels 4,5 -> timer 2
//   channels 6,7 -> timer 3    <-- LovyanGFX backlight on 7 -- AVOID
// If we let Arduino-ESP32 v3's auto-allocator pick, it can land on
// channel 6 (timer 3) and reconfigure the timer to 50 Hz / 14-bit,
// which makes the LCD backlight flicker / dim because its 8-bit duty
// values now run on a 14-bit timer at 50 Hz instead of ~1 kHz.
// ledcAttachChannel pins us to channel 0.
static const int PWM_RES     = 14;
static const int PWM_CHANNEL = 0;   // timer 0, disjoint from LCD backlight
static uint8_t s_pin = 0;
static int s_freq = 50;
static int s_pulseUs = 1500;
static bool s_active = false;

static void applyPulse() {
    if (!s_active) return;
    uint32_t period_us = 1000000 / s_freq;
    uint32_t max_duty = (1 << PWM_RES) - 1;
    uint32_t duty = (uint32_t)s_pulseUs * max_duty / period_us;
    ledcWrite(s_pin, duty);
}

bool ServoPWM::start(uint8_t pin, int freq) {
    if (s_active) stop();
    if (!PinPort::acquire(PinPort::PORT_B, PORT_PWM, "servo")) {
        return false;
    }
    s_pin = pin;
    s_freq = freq;
    // Full GPIO matrix reset -- without this, ledcAttach silently fails
    // when the pin was just torn down from a Wire1 (I2C) session.
    gpio_reset_pin((gpio_num_t)pin);
    pinMode(pin, OUTPUT);          // explicit OUTPUT before ledcAttach
                                    // (some Arduino-ESP32 v3 builds need
                                    // this; harmless when not needed)
    if (!ledcAttachChannel(pin, freq, PWM_RES, PWM_CHANNEL)) {
        Safety::logf("[ServoPWM] ledcAttachChannel(pin=%d ch=%d freq=%d res=%d) FAILED",
                     pin, PWM_CHANNEL, freq, PWM_RES);
        PinPort::release(PinPort::PORT_B);
        return false;
    }
    Safety::logf("[ServoPWM] ledcAttachChannel OK pin=%d ch=%d freq=%d res=%d",
                 pin, PWM_CHANNEL, freq, PWM_RES);
    s_active = true;
    applyPulse();
    return true;
}

void ServoPWM::stop() {
    if (!s_active) return;
    ledcWrite(s_pin, 0);
    ledcDetach(s_pin);
    s_active = false;
    PinPort::release(PinPort::PORT_B);
}

void ServoPWM::setFrequency(int hz) {
    s_freq = hz;
    if (s_active) {
        ledcDetach(s_pin);
        ledcAttachChannel(s_pin, hz, PWM_RES, PWM_CHANNEL);
        applyPulse();
    }
}

void ServoPWM::setPulse(int us) {
    s_pulseUs = us;
    applyPulse();
}

bool ServoPWM::isActive() { return s_active; }
