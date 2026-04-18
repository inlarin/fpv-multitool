#include "servo_pwm.h"
#include "core/pin_port.h"

static const int PWM_RES = 16;
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
    if (!ledcAttach(pin, freq, PWM_RES)) {
        PinPort::release(PinPort::PORT_B);
        return false;
    }
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
