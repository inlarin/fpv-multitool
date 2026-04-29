// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_diag.h.
#include "routes_diag.h"

#include "../../usb_emu/cp2112_emu.h"
#include "../../battery/smbus.h"
#include "safety.h"

// CP2112 logging hooks — defined in cp2112_emu.cpp with extern "C" linkage.
// Not in cp2112_emu.h because the .h is a thin "lifecycle" interface; the
// log/inspector helpers grew later. Same forward decl pattern as
// routes_battery.cpp / web_server.cpp.
extern "C" int      cp2112_log_dump(char *out, int cap);
extern "C" uint32_t cp2112_log_seq();
extern "C" int      cp2112_ep_info(char *out, int cap);

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <esp_system.h>
#include <WiFi.h>

namespace RoutesDiag {

void registerRoutesDiag(AsyncWebServer *s_server) {

    s_server->on("/api/cp2112/log", HTTP_GET, [](AsyncWebServerRequest *req) {
        static char buf[4096];
        int n = cp2112_log_dump(buf, sizeof(buf));
        AsyncResponseStream *r = req->beginResponseStream("text/plain");
        r->printf("seq=%lu\n", (unsigned long)cp2112_log_seq());
        r->write((const uint8_t*)buf, n);
        req->send(r);
    });

    s_server->on("/api/cp2112/info", HTTP_GET, [](AsyncWebServerRequest *req) {
        char buf[128];
        cp2112_ep_info(buf, sizeof(buf));
        req->send(200, "text/plain", buf);
    });

    // POST /api/sys/reboot
    //
    // Normal soft reset. The previous version of this route had a
    // ?mode=bootloader option that wrote RTC_CNTL_FORCE_DOWNLOAD_BOOT
    // before restarting, dropping the chip into ROM download mode. That
    // mode is REMOVED for remote-deployment safety: per ESP-IDF issue
    // #13287, the ESP32-S3 USB-Serial-JTAG cannot exit download mode
    // without an external RST press (core-reset doesn't re-sample boot
    // strapping pins). On a remote board with no physical access this
    // would brick the device.
    //
    // For OTA flashing on remote boards, use POST /api/ota/upload
    // instead -- it stays inside the application and rolls back if the
    // new image fails to validate.
    s_server->on("/api/sys/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        AsyncWebServerResponse *resp = req->beginResponse(
            200, "text/plain", "Rebooting...");
        resp->addHeader("Connection", "close");
        req->send(resp);
        xTaskCreate([](void*) {
            WiFi.disconnect(true, false);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }, "sysReboot", 2048, nullptr, 1, nullptr);
    });

    // GET /api/health -- minimal "device vitals" endpoint for remote
    // monitoring. Stable JSON shape so a poller can detect anomalies
    // without parsing full /api/sys/mem.
    s_server->on("/api/health", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["uptime_s"]     = (uint32_t)(millis() / 1000);
        d["free_heap"]    = (uint32_t)ESP.getFreeHeap();
        d["min_free_heap"]= (uint32_t)ESP.getMinFreeHeap();
        d["free_psram"]   = (uint32_t)ESP.getFreePsram();
        d["wifi_status"]  = (int)WiFi.status();      // WL_CONNECTED == 3
        d["wifi_rssi"]    = (int)WiFi.RSSI();
        d["wifi_ip"]      = WiFi.localIP().toString();
        d["wifi_mode"]    = (int)WiFi.getMode();
        d["ota_state"]    = Safety::otaStateStr();
        d["boot_count"]   = Safety::bootCount();
        d["validated"]    = Safety::wasValidatedThisBoot();
        d["safe_mode"]    = Safety::isSafeMode();
        d["last_reset"]   = Safety::lastResetReasonStr();
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // POST /api/sys/ota_mark_valid -- manual override for the rollback
    // gate. Normally Safety::tickValidation() does this automatically
    // after 30 s of stable runtime. Useful from a Settings UI or for
    // pinning a freshly-flashed image immediately when you know it's OK.
    s_server->on("/api/sys/ota_mark_valid", HTTP_POST, [](AsyncWebServerRequest *req) {
        Safety::markValidNow();
        req->send(200, "text/plain", "OTA marked VALID, boot counter reset");
    });

    // I2C preflight diagnostics
    s_server->on("/api/i2c/preflight", HTTP_GET, [](AsyncWebServerRequest *req) {
        auto r = SMBus::preflight();
        JsonDocument d;
        d["sdaOk"] = r.sdaOk;
        d["sclOk"] = r.sclOk;
        d["busOk"] = r.busOk;
        d["batteryAck"] = r.batteryAck;
        d["devCount"] = r.devCount;
        JsonArray devs = d["devices"].to<JsonArray>();
        for (int i = 0; i < r.devCount && i < 8; i++)
            devs.add(String("0x") + String(r.devAddrs[i], HEX));
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/i2c/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
        // Scan 0x08..0x77 on Wire1 (battery bus)
        AsyncResponseStream *r = req->beginResponseStream("text/plain");
        int found = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            Wire1.beginTransmission(addr);
            uint8_t err = Wire1.endTransmission();
            if (err == 0) {
                r->printf("0x%02X  ACK\n", addr);
                found++;
            }
        }
        r->printf("--\nScanned 0x08..0x77, found %d device(s)\n", found);
        req->send(r);
    });
}

} // namespace RoutesDiag
