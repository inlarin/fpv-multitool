// Waveshare ESP32-S3-LCD-1.47B boot.
//
// The Waveshare LCD is too small (172x320) to host a full interactive
// UI, so this board is "headless" from the user's perspective: all
// control happens via the web UI on whichever IP the status screen
// reports. The local LCD is just a status indicator (WiFi/IP/uptime/
// heap/USB mode/Port B mode/OTA state/version) -- conceptually the
// same payload the SC01 Plus puts in its 24px status bar, just laid
// out vertically because there's space.
//
// The BOOT button (GPIO 0) is mostly unused now -- a 3-second long
// press triggers a soft reboot, useful when the network is wedged.
//
// Bridge modes (USB2TTL, USB2SMBus, CP2112) keep working without
// any LCD presence: they're driven from main loop pumps in
// core/usb_mode.cpp::pumpLoop(), battery/smbus_bridge.cpp::loop(),
// and usb_emu/cp2112_emu.cpp's TinyUSB callbacks.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "pin_config.h"
#include "safety.h"
#include "board_settings.h"

#include "board/wsh_s3_lcd_147b/display.h"
#include "board/wsh_s3_lcd_147b/button.h"
#include "board/wsh_s3_lcd_147b/status_led.h"
#include "board/wsh_s3_lcd_147b/status_screen.h"
#include "board/wsh_s3_lcd_147b/imu.h"

#include "web/wifi_manager.h"
#include "web/web_server.h"
#include "web/web_state.h"
#include "battery/smbus_bridge.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"
#include "rc_sniffer/rc_sniffer.h"
#include "motor/motor_dispatch.h"
#include "fpv/esc_telem.h"

// Default AP credentials when STA fails / first boot. Same as before
// the menu-stripping refactor.
static constexpr const char *AP_SSID = "FPV-MultiTool";
static constexpr const char *AP_PASS = "fpv12345";

static void autoStartWifi() {
    WiFi.persistent(false);
    String ssid, pass;
    bool connected = false;
    if (WifiManager::loadCredentials(ssid, pass) && ssid.length() > 0) {
        Serial.printf("[WiFi] Trying saved STA: %s\n", ssid.c_str());
        connected = WifiManager::startSTA(ssid.c_str(), pass.c_str(), 10000);
    }
    if (!connected) {
        Serial.println("[WiFi] Starting AP mode");
        WifiManager::startAP(AP_SSID, AP_PASS);
    }
    WebServer::start();
}

void setup() {
    Serial.begin(115200);

    // Boot-loop guard + OTA rollback bookkeeping. Must run BEFORE any
    // potentially-crashy init so the counter still increments if init
    // panics. Same pattern as SC01 Plus (main_sc01_plus.cpp).
    Safety::earlyBootCheck();

    BoardSettings::begin();
    WebState::initMutex();

    // 30s task watchdog, panic+reset on timeout.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    Display::init();
    Button::init(BTN_BOOT);
    StatusLed::init();

    // Onboard QMI8658 IMU on Wire0 (SDA=48, SCL=47). We use it only for
    // accelerometer-based screen auto-rotate; gyro stays disabled.
    // No hard dependency: status_screen falls back to a fixed rotation
    // if init() fails.
    IMU::init();

    // Restore user-preferred Port B mode from NVS (IDLE by default).
    PinPort::applyAtBoot();

    autoStartWifi();
    SMBusBridge::begin();
    UsbMode::applyAtBoot();   // attach HID/Vendor interfaces if NVS-selected

    StatusScreen::init();     // draws static chrome, paints first values

    Serial.println("FPV MultiTool ready (status-only LCD)");
}

void loop() {
    esp_task_wdt_reset();

    // Safety net ticks (cheap noops after their first effective hit).
    Safety::tickValidation();
    Safety::tickNetworkWatchdog();
    Safety::tickBeacon(BoardSettings::beaconUrl().c_str(),
                       BoardSettings::beaconIntervalMs());

    StatusLed::loop();

    // Always-on web server + dispatchers.
    WebServer::loop();
    MotorDispatch::pump(/*inMotorApp=*/false);   // no local motor app anymore
    SMBusBridge::loop();         // serial -> SMBus proxy for PC tools
    UsbMode::pumpLoop();         // USB2TTL transparent CDC <-> UART1
    RCSniffer::loop();           // SBUS/iBus/PPM parser (no-op when stopped)
    ESCTelem::loop();            // KISS / BLHeli_32 telem decoder

    StatusScreen::tick();        // 1 Hz LCD refresh

    // BOOT button: long-press (3 s) -> soft reboot. The button is
    // otherwise unused -- there's no menu to navigate.
    ButtonEvent evt = Button::poll();
    if (evt == BTN_LONG_PRESS) {
        Serial.println("[boot button] long-press -> reboot");
        delay(50);
        esp_restart();
    }

    delay(2);
}
