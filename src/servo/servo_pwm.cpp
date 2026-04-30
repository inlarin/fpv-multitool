#include "servo_pwm.h"
#include "core/pin_port.h"
#include "safety.h"
#include <driver/gpio.h>

// 16-bit at 50 Hz worked on the Waveshare board but ledcAttach returns
// false on the SC01 Plus -- LovyanGFX already holds LEDC channel 7
// (timer 3) for the LCD backlight, and Arduino-ESP32 v3's auto-channel
// allocator gets confused enough that the attach silently fails.
//
// 14-bit gives 16384 steps over a 20 ms period = ~1.2 us per step, way
// finer than the ~5 us we actually care about for hobby servos, and
// it's small enough that the LEDC timer divider lands cleanly without
// fighting LovyanGFX's timer 3 reservation.
static const int PWM_RES = 14;
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
    if (!ledcAttach(pin, freq, PWM_RES)) {
        Safety::logf("[ServoPWM] ledcAttach(pin=%d freq=%d res=%d) FAILED",
                     pin, freq, PWM_RES);
        PinPort::release(PinPort::PORT_B);
        return false;
    }
    Safety::logf("[ServoPWM] ledcAttach OK pin=%d freq=%d res=%d",
                 pin, freq, PWM_RES);
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
        ledcAttach(s_pin, hz, PWM_RES);
        applyPulse();
    }
}

void ServoPWM::setPulse(int us) {
    s_pulseUs = us;
    applyPulse();
}

bool ServoPWM::isActive() { return s_active; }
