#include "motor_dispatch.h"
#include "dshot.h"
#include "servo/servo_pwm.h"
#include "web/web_state.h"
#include "pin_config.h"

namespace MotorDispatch {

void pump(bool inMotorApp) {
    // Snapshot shared state under lock. We clear request flags here too so
    // requests are consumed once regardless of which caller runs the pump.
    bool armReq, disarmReq, beepReq, armed;
    bool dirCwReq, dirCcwReq, mode3DOnReq, mode3DOffReq;
    int beepCmd, dshotSpeed, maxThrottle;
    uint16_t throttle;
    bool servoSweep, servoActive;
    int sweepMinUs, sweepMaxUs, sweepPeriodMs;
    {
        WebState::Lock lock;
        armReq       = WebState::motor.armRequest;
        disarmReq    = WebState::motor.disarmRequest;
        beepReq      = WebState::motor.beepRequest;
        beepCmd      = WebState::motor.beepCmd;
        dirCwReq     = WebState::motor.dirCwRequest;
        dirCcwReq    = WebState::motor.dirCcwRequest;
        mode3DOnReq  = WebState::motor.mode3DOnRequest;
        mode3DOffReq = WebState::motor.mode3DOffRequest;
        armed        = WebState::motor.armed;
        dshotSpeed   = WebState::motor.dshotSpeed;
        throttle     = WebState::motor.throttle;
        maxThrottle  = WebState::motor.maxThrottle;
        WebState::motor.armRequest       = false;
        WebState::motor.disarmRequest    = false;
        WebState::motor.beepRequest      = false;
        WebState::motor.dirCwRequest     = false;
        WebState::motor.dirCcwRequest    = false;
        WebState::motor.mode3DOnRequest  = false;
        WebState::motor.mode3DOffRequest = false;
        if (throttle > maxThrottle) {
            throttle = maxThrottle;
            WebState::motor.throttle = maxThrottle;
        }
        servoSweep    = WebState::servo.sweep;
        servoActive   = WebState::servo.active;
        sweepMinUs    = WebState::servo.sweepMinUs;
        sweepMaxUs    = WebState::servo.sweepMaxUs;
        sweepPeriodMs = WebState::servo.sweepPeriodMs;
    }

    // Arm via web is ignored when the motor tester app owns the UI — avoids
    // two sources driving DShot at once.
    if (armReq && !armed && !inMotorApp) {
        DShotSpeed s = (dshotSpeed == 150) ? DSHOT150 :
                       (dshotSpeed == 600) ? DSHOT600 : DSHOT300;
        if (DShot::init(SIGNAL_OUT, s)) {
            DShot::arm();
            WebState::Lock lock;
            WebState::motor.armed = true;
        }
    }
    if (disarmReq) {
        for (int i = 0; i < 50; i++) {
            DShot::sendThrottle(0);
            delayMicroseconds(2000);
        }
        DShot::stop();
        WebState::Lock lock;
        WebState::motor.throttle = 0;
        WebState::motor.armed = false;
    }
    if (beepReq && armed) {
        int cmd = beepCmd;
        if (cmd < 1 || cmd > 5) cmd = 1;
        DShot::sendCommand((uint8_t)cmd);
    }

    // Direction & 3D latch commands — send >=6 times to commit to ESC.
    auto sendLatchCmd = [](uint8_t c) {
        for (int i = 0; i < 10; i++) {
            DShot::sendCommand(c);
            delay(1);
        }
    };
    if (armed) {
        if (dirCwReq)     sendLatchCmd(7);   // SPIN_DIR_1
        if (dirCcwReq)    sendLatchCmd(8);   // SPIN_DIR_2
        if (mode3DOnReq)  sendLatchCmd(10);  // 3D_MODE_ON
        if (mode3DOffReq) sendLatchCmd(9);   // 3D_MODE_OFF
    }

    // Servo sweep — triangular wave min→max→min over sweepPeriodMs.
    if (servoSweep && servoActive && sweepMaxUs > sweepMinUs && sweepPeriodMs > 0) {
        uint32_t pr    = (uint32_t)sweepPeriodMs;
        uint32_t phase = millis() % pr;
        uint32_t half  = pr / 2;
        if (half == 0) half = 1;  // guard against pr==1
        int range = sweepMaxUs - sweepMinUs;
        int us = (phase < half)
            ? sweepMinUs + (int)((int64_t)phase * range / half)
            : sweepMaxUs - (int)((int64_t)(phase - half) * range / half);
        ServoPWM::setPulse(us);
        WebState::Lock lock;
        WebState::servo.pulseUs = us;
        if (WebState::servo.observedMinUs == 0 || us < WebState::servo.observedMinUs)
            WebState::servo.observedMinUs = us;
        if (us > WebState::servo.observedMaxUs) WebState::servo.observedMaxUs = us;
    }

    // Continuous DShot frame when armed via web. 2 ms period (500 Hz).
    if (armed) {
        static uint32_t lastSend = 0;
        if (micros() - lastSend > 2000) {
            uint16_t dsVal = (throttle == 0) ? 0 : constrain(throttle + 47, 48, 2047);
            DShot::sendThrottle(dsVal);
            lastSend = micros();
        }
    }
}

} // namespace MotorDispatch
