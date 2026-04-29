// SC01 Plus safeboot firmware -- the immutable recovery shell that
// lives in the `factory` partition. Built ONLY by [env:wt32_sc01_plus_safeboot].
//
// What it does:
//   - WiFi STA from BoardSettings creds (or AP fallback FPV-Recovery)
//   - WebServer on port 80 with /api/ota/upload + /api/health + /api/sys/reboot
//   - LovyanGFX init enough to print a status banner on the LCD so a
//     field tech can see the device fell back into recovery mode
//   - Serial CLI for `wifi set / show / clear / reconnect / reboot`
//
// What it does NOT do:
//   - LVGL widgets / touch / battery / motor / catalog / anything else
//
// The factory app should NEVER be the boot target during normal
// operation. The bootloader picks it only if BOTH ota_0 and ota_1
// become INVALID (boot validation failed twice in a row). When the
// field tech sees the recovery banner, they just OTA a fresh good
// firmware via /api/ota/upload and the device is back.
//
// Size budget: factory partition is 1 MB. Current image: ~700 KB
// (LovyanGFX + WiFi + WebServer + AsyncTCP + minimal helpers). Lots
// of headroom; if it grows past 900 KB consider stripping LovyanGFX.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <esp_task_wdt.h>

#include "pin_config.h"
#include "board_settings.h"
#include "lgfx_sc01_plus.h"

static LGFX_SC01Plus s_lcd;
static AsyncWebServer s_server(80);

// ---- LCD banner -------------------------------------------------------------

static void lcdBanner(const char *line2) {
    s_lcd.fillScreen(TFT_BLACK);
    s_lcd.setTextColor(TFT_RED, TFT_BLACK);
    s_lcd.setCursor(8, 8);
    s_lcd.setTextSize(3);
    s_lcd.println("SAFEBOOT");

    s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    s_lcd.setTextSize(2);
    s_lcd.setCursor(8, 56);
    s_lcd.println("recovery mode");
    s_lcd.setCursor(8, 80);
    s_lcd.println("OTA via:");
    s_lcd.setCursor(8, 104);
    s_lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    s_lcd.println(line2);
    s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    s_lcd.setCursor(8, 144);
    s_lcd.setTextSize(1);
    s_lcd.println("POST firmware to /api/ota/upload");
    s_lcd.println("then it reboots into the new app.");
}

// ---- WiFi -------------------------------------------------------------------

static bool s_sta_connected = false;

static bool tryStaFromNvs() {
    String ssid = BoardSettings::wifiSsid();
    String pass = BoardSettings::wifiPass();
    if (ssid.length() == 0) return false;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000) {
        delay(250);
    }
    return WiFi.status() == WL_CONNECTED;
}

static void startApFallback() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("FPV-Recovery", "fpvrecovery");
}

// ---- HTTP routes ------------------------------------------------------------

static void registerRoutes() {
    // Minimal landing page so a phone browser knows where they are.
    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        String html =
            "<!DOCTYPE html><html><head><meta charset=utf-8>"
            "<meta name=viewport content=\"width=device-width\">"
            "<title>WT32-SC01 Plus Safeboot</title>"
            "<style>body{font-family:sans-serif;background:#101418;color:#fff;"
            "padding:16px;max-width:480px;margin:auto}"
            "h1{color:#e63946}h2{color:#06a77d}code{background:#222;padding:2px 6px}"
            "input[type=file]{margin:8px 0}button{padding:10px 20px;background:#06a77d;"
            "color:white;border:none;border-radius:6px;font-size:16px}</style>"
            "</head><body>"
            "<h1>SAFEBOOT</h1><p>Recovery shell. Both OTA slots failed validation, "
            "or this is the first boot.</p>"
            "<h2>Upload firmware</h2>"
            "<form method=POST action=\"/api/ota/upload\" enctype=multipart/form-data>"
            "<input type=file name=firmware accept=\".bin\"><br>"
            "<button type=submit>Flash</button></form>"
            "<h2>Status</h2><p><a href=\"/api/health\" style=color:#fff>/api/health</a></p>"
            "</body></html>";
        req->send(200, "text/html", html);
    });

    // Same /api/health as the main app (smaller fields though). Lets a
    // poller distinguish "main app down, safeboot up" from "fully dead".
    s_server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *req) {
        String j = "{\"safeboot\":true";
        j += ",\"uptime_s\":" + String((uint32_t)(millis() / 1000));
        j += ",\"free_heap\":" + String((uint32_t)ESP.getFreeHeap());
        j += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
        j += ",\"wifi_status\":" + String((int)WiFi.status());
        j += "}";
        req->send(200, "application/json", j);
    });

    // /api/ota/upload -- multipart POST, writes inactive OTA slot via
    // Update.h. Same upload protocol as the main app, so the same
    // scripts/ota_flash.sh works against either.
    s_server.on("/api/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError() && Update.isFinished();
            AsyncWebServerResponse *resp = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK -- rebooting into new image" :
                     (String("FAIL: ") + Update.errorString()));
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                xTaskCreate([](void*) {
                    WiFi.disconnect(true, false);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    ESP.restart();
                }, "safebootReboot", 2048, nullptr, 1, nullptr);
            }
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                size_t content_len = req->contentLength();
                Update.begin(content_len > 0 ? content_len : UPDATE_SIZE_UNKNOWN, U_FLASH);
            }
            if (Update.isRunning()) Update.write(data, len);
            if (final) Update.end(true);
        });

    s_server.on("/api/sys/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        AsyncWebServerResponse *resp = req->beginResponse(200, "text/plain", "Rebooting...");
        resp->addHeader("Connection", "close");
        req->send(resp);
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP.restart();
        }, "rb", 2048, nullptr, 1, nullptr);
    });
}

// ---- Setup / loop -----------------------------------------------------------

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) { delay(10); }

    Serial.println();
    Serial.println(F("===== WT32-SC01 Plus SAFEBOOT (recovery firmware) ====="));

    // Watchdog -- safeboot stays alive forever waiting for OTA, so
    // we genuinely want a panic-reset if the WiFi/web stack hangs.
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    BoardSettings::begin();

    // Bring the LCD up just enough to show a status banner. We do NOT
    // run LVGL or touch -- safeboot is intentionally minimal.
    s_lcd.init();
    s_lcd.setRotation(0);
    s_lcd.setBrightness(255);
    lcdBanner("connecting WiFi...");

    bool sta = tryStaFromNvs();
    if (sta) {
        s_sta_connected = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "http://%s/", WiFi.localIP().toString().c_str());
        lcdBanner(buf);
    } else {
        startApFallback();
        lcdBanner("AP FPV-Recovery / fpvrecovery\nhttp://192.168.4.1/");
    }

    registerRoutes();
    s_server.begin();

    Serial.printf("[safeboot] ready  ip=%s ap=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.softAPIP().toString().c_str());
}

void loop() {
    esp_task_wdt_reset();
    delay(50);
}
