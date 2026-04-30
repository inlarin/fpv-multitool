// ESP32-S3 FPV MultiTool
// Main entry: menu + app switching
#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "pin_config.h"
#include "safety.h"
#include "board_settings.h"
#include "board/wsh_s3_lcd_147b/display.h"
#include "board/wsh_s3_lcd_147b/button.h"
#include "board/wsh_s3_lcd_147b/ui/menu.h"
#include "board/wsh_s3_lcd_147b/status_led.h"
#include "board/wsh_s3_lcd_147b/usb2ttl.h"
#include "board/wsh_s3_lcd_147b/ui/servo_tester.h"
#include "board/wsh_s3_lcd_147b/ui/motor_tester.h"
#include "board/wsh_s3_lcd_147b/ui/battery_ui.h"
#include "board/wsh_s3_lcd_147b/ui/wifi_app.h"
#include "web/wifi_manager.h"
#include "web/web_server.h"
#include "web/web_state.h"
#include "battery/dji_battery.h"
#include "battery/autel_battery.h"
#include "battery/smbus_bridge.h"
#include "board/wsh_s3_lcd_147b/ui/smbus_bridge_ui.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"
#include "rc_sniffer/rc_sniffer.h"
#include "usb_emu/cp2112_emu.h"
#include "motor/dshot.h"
#include "motor/motor_dispatch.h"
#include "board/wsh_s3_lcd_147b/ui/crsf_tester.h"
#include "fpv/esc_telem.h"
#include "servo/servo_pwm.h"

static AppId currentApp = APP_NONE;

// Try STA from saved creds, otherwise start AP
static void autoStartWifi() {
    // Don't write creds to ESP-IDF WiFi NVS namespace on every begin() â€”
    // we keep them in our own Preferences "wifi" store. Without this, each
    // boot rewrites WiFi NVS and slows reconnect; without persistent=false
    // there's also a benign warning about "saving WiFi config" every boot.
    WiFi.persistent(false);
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

// Background task: keep webserver alive and service motor/servo from web commands
static void webBackgroundTask() {
    WebServer::loop();
    MotorDispatch::pump(/*inMotorApp=*/currentApp == APP_MOTOR);
}

void setup() {
    Serial.begin(115200);

    // Boot-loop guard + OTA rollback bookkeeping. Must run BEFORE any
    // potentially-crashy subsystem init so the counter still increments
    // if init crashes. Same pattern as on the SC01 Plus (main_sc01_plus.cpp).
    Safety::earlyBootCheck();

    // BoardSettings owns the "boardcfg" NVS namespace -- shared by both
    // boards now (originally SC01-only, moved to src/core/ when the
    // health-beacon settings became universal).
    BoardSettings::begin();

    WebState::initMutex();  // Must be first â€” protects shared state

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
    AutelBattery::init(); // Autel read-only support (shares Wire1/Port B, 0x0B)
    SMBusBridge::begin();
    UsbMode::applyAtBoot();   // attach HID/Vendor interfaces if enabled in NVS

    Menu::draw();
    Serial.println("FPV MultiTool ready");
}

void loop() {
    esp_task_wdt_reset();  // feed watchdog

    // OTA rollback gate + network watchdog (cheap noops after first hit).
    Safety::tickValidation();
    Safety::tickNetworkWatchdog();
    Safety::tickBeacon(BoardSettings::beaconUrl().c_str(),
                       BoardSettings::beaconIntervalMs());

    StatusLed::loop();
    SMBusBridge::loop();    // serialâ†’SMBus proxy for PC-side tools
    UsbMode::pumpLoop();    // USB2TTL transparent CDCâ†”UART1 bridge (no-op in other modes)
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
