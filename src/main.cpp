// ESP32-S3 FPV MultiTool
// Main entry: menu + app switching
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "ui/menu.h"
#include "bridge/usb2ttl.h"
#include "servo/servo_tester.h"
#include "motor/motor_tester.h"
#include "battery/battery_ui.h"
#include "web/wifi_app.h"
#include "web/wifi_manager.h"
#include "web/web_server.h"
#include "web/web_state.h"
#include "battery/dji_battery.h"
#include "motor/dshot.h"

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
    int dshotSpeed;
    uint16_t throttle;
    {
        WebState::Lock lock;
        armReq    = WebState::motor.armRequest;
        disarmReq = WebState::motor.disarmRequest;
        beepReq   = WebState::motor.beepRequest;
        armed     = WebState::motor.armed;
        dshotSpeed = WebState::motor.dshotSpeed;
        throttle  = WebState::motor.throttle;
        WebState::motor.armRequest = false;
        WebState::motor.disarmRequest = false;
        WebState::motor.beepRequest = false;
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
    if (beepReq && armed) DShot::sendCommand(1);

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

    // Auto-start WiFi + web server in background
    autoStartWifi();
    DJIBattery::init(); // I2C for battery telemetry via web

    Menu::draw();
    Serial.println("FPV MultiTool ready");
}

void loop() {
    esp_task_wdt_reset();  // feed watchdog
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
                case APP_SERVO:      runServoTester(); break;
                case APP_MOTOR:      runMotorTester(); break;
                case APP_BATTERY:    runBatteryTool(); break;
                case APP_WIFI:       runWifiApp(); break;
                default: break;
            }

            currentApp = APP_NONE;
            Menu::draw();
            Serial.println("Back to menu");
        }
    }

    delay(2);
}
