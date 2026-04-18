// ESP32-S3 FPV MultiTool
// Main entry: menu + app switching
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "ui/menu.h"
#include "ui/status_led.h"
#include "bridge/usb2ttl.h"
#include "servo/servo_tester.h"
#include "motor/motor_tester.h"
#include "battery/battery_ui.h"
#include "web/wifi_app.h"
#include "web/wifi_manager.h"
#include "web/web_server.h"
#include "web/web_state.h"
#include "battery/dji_battery.h"
#include "battery/smbus_bridge.h"
#include "battery/smbus_bridge_ui.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"
#include "rc_sniffer/rc_sniffer.h"
#include "usb_emu/cp2112_emu.h"
#include "motor/dshot.h"
#include "crsf/crsf_tester.h"
#include "fpv/esc_telem.h"
#include "servo/servo_pwm.h"

static AppId currentApp = APP_NONE;

// Try STA from saved creds, otherwise start AP
static void autoStartWifi() {
    String ssid, pass;
    bool connected = false;
    if (WifiManager::loadCredentials(ssid, pass) && ssid.length() > 0) {
        Serial.printf("[WiFi] Trying saved STA: %s\n", ssid.c_str());
        connected = WifiManager::startSTA(ssid.c_str(), pass.c_str(), 10000);
    }
    if (!connected) {
        Serial.println("[WiFi] Starting AP mode");
        WifiManager::startAP("FPV-MultiTool", "fpv12345");
    }
    WebServer::start();
}

// Background task: keep webserver alive and service motor/battery from web commands
static void webBackgroundTask() {
    WebServer::loop();

    // Snapshot motor state under lock, then execute heavy ops outside lock
    bool armReq, disarmReq, beepReq, armed;
    bool dirCwReq, dirCcwReq, mode3DOnReq, mode3DOffReq;
    int dshotSpeed;
    uint16_t throttle;
    int maxThrottle;
    {
        WebState::Lock lock;
        armReq    = WebState::motor.armRequest;
        disarmReq = WebState::motor.disarmRequest;
        beepReq   = WebState::motor.beepRequest;
        dirCwReq  = WebState::motor.dirCwRequest;
        dirCcwReq = WebState::motor.dirCcwRequest;
        mode3DOnReq  = WebState::motor.mode3DOnRequest;
        mode3DOffReq = WebState::motor.mode3DOffRequest;
        armed     = WebState::motor.armed;
        dshotSpeed = WebState::motor.dshotSpeed;
        throttle  = WebState::motor.throttle;
        maxThrottle = WebState::motor.maxThrottle;
        WebState::motor.armRequest = false;
        WebState::motor.disarmRequest = false;
        WebState::motor.beepRequest = false;
        WebState::motor.dirCwRequest = false;
        WebState::motor.dirCcwRequest = false;
        WebState::motor.mode3DOnRequest = false;
        WebState::motor.mode3DOffRequest = false;
        // Clamp throttle to cap right here (also makes telemetry consistent)
        if (throttle > maxThrottle) {
            throttle = maxThrottle;
            WebState::motor.throttle = maxThrottle;
        }
    }

    if (armReq && !armed && currentApp != APP_MOTOR) {
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
        int cmd = WebState::motor.beepCmd;
        if (cmd < 1 || cmd > 5) cmd = 1;
        DShot::sendCommand((uint8_t)cmd);
    }

    // Direction & 3D mode require ESC armed. Send command >=6 times to latch.
    auto sendLatchCmd = [](uint8_t cmd) {
        for (int i = 0; i < 10; i++) {
            DShot::sendCommand(cmd);
            delay(1);
        }
    };
    if (armed) {
        if (dirCwReq)     sendLatchCmd(7);   // SPIN_DIR_1
        if (dirCcwReq)    sendLatchCmd(8);   // SPIN_DIR_2
        if (mode3DOnReq)  sendLatchCmd(10);  // 3D_MODE_ON
        if (mode3DOffReq) sendLatchCmd(9);   // 3D_MODE_OFF
    }

    // Servo sweep executor: triangular wave between sweepMinUs..sweepMaxUs
    {
        bool sw, act;
        int mn, mx, pr;
        {
            WebState::Lock lock;
            sw  = WebState::servo.sweep;
            act = WebState::servo.active;
            mn  = WebState::servo.sweepMinUs;
            mx  = WebState::servo.sweepMaxUs;
            pr  = WebState::servo.sweepPeriodMs;
        }
        if (sw && act && mx > mn && pr > 0) {
            uint32_t phase = millis() % (uint32_t)pr;
            uint32_t half  = (uint32_t)pr / 2;
            int range = mx - mn;
            int us = (phase < half)
                ? mn + (int)((int64_t)phase * range / half)
                : mx - (int)((int64_t)(phase - half) * range / half);
            ServoPWM::setPulse(us);
            WebState::Lock lock;
            WebState::servo.pulseUs = us;
            if (WebState::servo.observedMinUs == 0 || us < WebState::servo.observedMinUs)
                WebState::servo.observedMinUs = us;
            if (us > WebState::servo.observedMaxUs) WebState::servo.observedMaxUs = us;
        }
    }

    // Continuous DShot frame when armed via web
    if (armed) {
        static uint32_t lastSend = 0;
        if (micros() - lastSend > 2000) {
            uint16_t dsVal = (throttle == 0) ? 0 : constrain(throttle + 47, 48, 2047);
            DShot::sendThrottle(dsVal);
            lastSend = micros();
        }
    }
}

void setup() {
    Serial.begin(115200);

    WebState::initMutex();  // Must be first — protects shared state

    // Watchdog: 30s max task block, panic+reset on timeout
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);  // monitor main task

    Display::init();
    Button::init(BTN_BOOT);
    StatusLed::init();

    // Restore user-preferred Port B mode from NVS (IDLE by default).
    // Feature consumers (DJIBattery/SMBusBridge/CRSF/...) will acquire Port B
    // when their mode matches; otherwise they stay inactive until user
    // switches modes via Web UI.
    PinPort::applyAtBoot();

    // Auto-start WiFi + web server in background
    autoStartWifi();
    DJIBattery::init(); // I2C for battery telemetry via web
    SMBusBridge::begin();
    UsbMode::applyAtBoot();   // attach HID/Vendor interfaces if enabled in NVS

    Menu::draw();
    Serial.println("FPV MultiTool ready");
}

void loop() {
    esp_task_wdt_reset();  // feed watchdog
    StatusLed::loop();
    SMBusBridge::loop();    // serial→SMBus proxy for PC-side tools
    UsbMode::pumpLoop();    // USB2TTL transparent CDC↔UART1 bridge (no-op in other modes)
    RCSniffer::loop();      // SBUS/iBus/PPM frame parser (no-op when not running)
    ESCTelem::loop();       // KISS/BLHeli_32 ESC telemetry (no-op when not running)
    ButtonEvent evt = Button::poll();

    if (currentApp == APP_NONE) {
        // Keep web server running in background while in menu
        webBackgroundTask();

        AppId selected = Menu::update(evt);
        if (selected != APP_NONE) {
            currentApp = selected;
            Serial.printf("Launching app %d\n", currentApp);

            switch (currentApp) {
                case APP_USB2TTL:    runUSB2TTL(); break;
                case APP_USB2SMBUS:  runUSB2SMBus(); break;
                case APP_SERVO:      runServoTester(); break;
                case APP_MOTOR:      runMotorTester(); break;
                case APP_BATTERY:    runBatteryTool(); break;
                case APP_WIFI:       runWifiApp(); break;
                case APP_CRSF:       runCRSFTester(); break;
                default: break;
            }

            currentApp = APP_NONE;
            Menu::draw();
            Serial.println("Back to menu");
        }
    }

    delay(2);
}
