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
#include <lvgl.h>

#include "pin_config.h"
#include "board_settings.h"
#include "board_display.h"

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

// LVGL framebuffer in PSRAM (proven path from step 2b sanity).
static constexpr size_t LV_BUF_PIXELS = 480 * 32;
static lv_color_t      *s_buf_a = nullptr;
static lv_display_t    *s_lv_disp = nullptr;
static lv_indev_t      *s_lv_indev = nullptr;

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * h);
    s_display.pushPixels(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void lv_indev_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
    UiTouch t = s_display.readTouch();
    if (t.pressed) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = t.x;
        data->point.y = t.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t lv_tick_cb() { return millis(); }

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

static void buildSkeletonUI() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(title, "WT32-SC01 Plus");

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(hint, "Phase 0 skeleton -- UI in next sprint");

    lv_obj_t *ip_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(0x06A77D), 0);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(ip_lbl, LV_ALIGN_CENTER, 0, 40);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s",
             WiFi.status() == WL_CONNECTED
                 ? WiFi.localIP().toString().c_str()
                 : (WiFi.getMode() & WIFI_AP ? "AP: 192.168.4.1" : "no link"));
    lv_label_set_text(ip_lbl, buf);
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus full app ================"));

    WebState::initMutex();

    // ESP-IDF 5 WDT API: pass a config struct, not (timeout, panic).
    // Same pattern as src/main.cpp on the Waveshare side.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    BoardSettings::begin();
    s_display.begin();    // LCD + touch + NVS rotation/calibration

    PinPort::applyAtBoot();

    // ---- LVGL stack ----
    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    s_buf_a = (lv_color_t *)heap_caps_malloc(
        LV_BUF_PIXELS * sizeof(lv_color_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    Serial.printf("  LVGL buffer        : %u bytes\n",
                  (unsigned)(LV_BUF_PIXELS * sizeof(lv_color_t)));

    s_lv_disp = lv_display_create(s_display.width(), s_display.height());
    lv_display_set_flush_cb(s_lv_disp, lv_flush_cb);
    lv_display_set_buffers(s_lv_disp, s_buf_a, nullptr,
                           LV_BUF_PIXELS * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_lv_indev = lv_indev_create();
    lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_lv_indev, lv_indev_read_cb);

    // ---- WiFi + WebServer (same routes as Waveshare) ----
    autoStartWifi();

    // ---- Battery / SMBus subsystems (Port B in I2C mode by default) ----
    DJIBattery::init();
    AutelBattery::init();
    SMBusBridge::begin();

    // ---- USB descriptor mode (CDC / CP2112 / Vendor) from NVS ----
    UsbMode::applyAtBoot();

    buildSkeletonUI();

    Serial.println(F("======================================================"));
}

void loop() {
    esp_task_wdt_reset();

    // Web stack + motor command pump
    WebServer::loop();
    MotorDispatch::pump(/*inMotorApp=*/false);

    // SMBus bridge / USB pump (no-op when not active)
    SMBusBridge::loop();
    UsbMode::pumpLoop();

    // LVGL tick at ~5 ms cadence
    lv_timer_handler();

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
