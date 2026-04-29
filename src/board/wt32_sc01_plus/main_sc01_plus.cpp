// WT32-SC01 Plus -- main entry for the full app build.
//
// Built ONLY by [env:wt32_sc01_plus]. Boots WiFi + WebServer + the same
// subsystem stack as the Waveshare board (servo / motor / battery /
// CRSF / sniffer / USB), then layers the LVGL touchscreen UI on top via
// BoardDisplay (LovyanGFX i80 + FT6336 touch + NVS-backed calibration).
//
// At Phase 0 the LVGL UI is just a skeleton ("Hello SC01 Plus" label).
// Real screens (servo / motor / battery / catalog / settings) come in
// Phase 1+ -- the goal of Phase 0 is to prove the whole stack compiles
// and links cleanly with all subsystems pulled in.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "pin_config.h"
#include "safety.h"
#include "board_settings.h"
#include "board_display.h"
#include "serial_cli.h"
#include "ui/board_app.h"

#include "web/wifi_manager.h"
#include "web/web_server.h"
#include "web/web_state.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"
#include "battery/dji_battery.h"
#include "battery/autel_battery.h"
#include "battery/smbus_bridge.h"
#include "motor/motor_dispatch.h"

static BoardDisplay s_display;
static BoardApp     s_app;

// Same WiFi auto-start logic as the Waveshare main.cpp -- saved STA
// credentials first, fall back to AP. WebServer comes up after either.
static void autoStartWifi() {
    WiFi.persistent(false);
    String ssid, pass;
    bool connected = false;
    if (WifiManager::loadCredentials(ssid, pass) && ssid.length() > 0) {
        Serial.printf("[WiFi] STA from saved creds: %s\n", ssid.c_str());
        connected = WifiManager::startSTA(ssid.c_str(), pass.c_str(), 10000);
    }
    if (!connected) {
        Serial.println("[WiFi] AP fallback: FPV-MultiTool / fpv12345");
        WifiManager::startAP("FPV-MultiTool", "fpv12345");
    }
    WebServer::start();
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus full app ================"));

    // Boot-loop guard + OTA rollback bookkeeping. Must run BEFORE any
    // subsystem that could crash, otherwise the counter never increments
    // and we lose the safety net.
    Safety::earlyBootCheck();

    WebState::initMutex();

    // ESP-IDF 5 WDT API: pass a config struct, not (timeout, panic).
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    BoardSettings::begin();

    if (Safety::isSafeMode()) {
        // Safe mode: skip ALL display / touch / LVGL / battery / USB inits.
        // Bring up just enough to reach OTA -- WiFi + WebServer + CLI.
        // The board sits dark-screened, but reflashable.
        Serial.println(F("[safe-mode] skipping: BoardDisplay, BoardApp, batteries, USB"));
        autoStartWifi();
        SerialCli::begin();
        Serial.println(F("======================================================"));
        return;
    }

    s_display.begin();    // LCD + touch + NVS rotation/calibration
    PinPort::applyAtBoot();

    // ---- LVGL UI (BoardApp manages display/indev/tick + tabview) ----
    s_app.begin(s_display);

    // ---- WiFi + WebServer (same routes as Waveshare) ----
    autoStartWifi();

    // ---- Battery / SMBus subsystems (Port B in I2C mode by default) ----
    DJIBattery::init();
    AutelBattery::init();
    SMBusBridge::begin();

    // ---- USB descriptor mode (CDC / CP2112 / Vendor) from NVS ----
    UsbMode::applyAtBoot();

    SerialCli::begin();
    Serial.println(F("======================================================"));
}

void loop() {
    esp_task_wdt_reset();

    // OTA rollback gate: after we've been alive 30 s without panic, mark
    // the running app as VALID so the bootloader stops watching for
    // rollback AND zero the boot counter so the next boot starts clean.
    // Cheap noop after the first call.
    Safety::tickValidation();

    // Web stack stays alive in BOTH normal mode and safe mode -- it's
    // our recovery channel for OTA reflash if anything goes wrong.
    WebServer::loop();

    if (Safety::isSafeMode()) {
        SerialCli::poll();
        delay(50);
        return;
    }

    MotorDispatch::pump(/*inMotorApp=*/false);

    // SMBus bridge / USB pump (no-op when not active)
    SMBusBridge::loop();
    UsbMode::pumpLoop();

    // LVGL tick + UI refresh at ~5 ms cadence
    s_app.loop();

    // Serial CLI -- the only `reboot ...` here is the normal soft reset.
    // Direct ROM-bootloader entry was deliberately removed for remote-
    // deployment safety (USB-Serial-JTAG can't exit download mode
    // without an external reset, ESP-IDF #13287).
    SerialCli::poll();

    static uint32_t last = 0;
    if (millis() - last >= 5000) {
        last = millis();
        Serial.printf("alive  free heap=%u  free psram=%u  wifi=%d\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getFreePsram(),
                      (int)WiFi.status());
    }

    delay(2);
}
