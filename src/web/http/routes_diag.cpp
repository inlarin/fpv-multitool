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
#include <esp_core_dump.h>
#include <esp_partition.h>
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

    // GET /api/sys/coredump -- read the persisted ELF core dump from
    // the `coredump` partition (default_16MB.csv has it at 0xFF0000,
    // 64 KB). On a remote device this is how we extract a crash post-
    // mortem without serial access. Returns:
    //   404 "no coredump"        -- partition empty or invalid header
    //   200 application/octet-stream  -- ELF dump (parse with espcoredump.py)
    s_server->on("/api/sys/coredump", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (esp_core_dump_image_check() != ESP_OK) {
            req->send(404, "text/plain", "no coredump");
            return;
        }
        size_t addr = 0, size = 0;
        if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
            req->send(404, "text/plain", "no coredump (image_get failed)");
            return;
        }
        const esp_partition_t *p = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        if (!p) {
            req->send(404, "text/plain", "no coredump partition");
            return;
        }
        // Stream chunked from flash so we don't allocate a 64 KB buffer
        // in heap (free heap is tight on this board).
        const uint32_t dump_offset = (uint32_t)addr - p->address;
        const uint32_t dump_size   = (uint32_t)size;
        AsyncWebServerResponse *resp = req->beginResponse(
            "application/octet-stream", dump_size,
            [dump_offset, dump_size](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
                if (index >= dump_size) return 0;
                const esp_partition_t *p = esp_partition_find_first(
                    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
                if (!p) return 0;
                size_t to_read = dump_size - index;
                if (to_read > maxLen) to_read = maxLen;
                if (esp_partition_read(p, dump_offset + index, buf, to_read) != ESP_OK) return 0;
                return to_read;
            });
        resp->addHeader("Content-Disposition", "attachment; filename=\"coredump.elf\"");
        req->send(resp);
    });

    // POST /api/sys/coredump/erase -- clear the coredump partition so
    // the next /api/sys/coredump returns 404 until a fresh crash. Use
    // after you've pulled the dump for analysis.
    s_server->on("/api/sys/coredump/erase", HTTP_POST, [](AsyncWebServerRequest *req) {
        esp_err_t err = esp_core_dump_image_erase();
        if (err == ESP_OK) {
            req->send(200, "text/plain", "coredump erased");
        } else {
            req->send(500, "text/plain", esp_err_to_name(err));
        }
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
