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

#include "board_settings.h"

// LVGL snapshot is only available on the SC01 Plus full-app build (the
// only one that actually runs LVGL). Guard the include + endpoint.
#if defined(BOARD_WT32_SC01_PLUS)
  #include <lvgl.h>
  #include "ui/board_app.h"
#endif

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
    // GET /api/sys/log -- dump the in-memory diagnostic ring (4 KB).
    // Stand-in for `pio device monitor` when COM-port behavior is flaky.
    s_server->on("/api/sys/log", HTTP_GET, [](AsyncWebServerRequest *req) {
        static char buf[4200];
        size_t n = Safety::logCopy(buf, sizeof(buf) - 1);
        buf[n] = 0;
        req->send(200, "text/plain", buf);
    });

    s_server->on("/api/sys/coredump/erase", HTTP_POST, [](AsyncWebServerRequest *req) {
        esp_err_t err = esp_core_dump_image_erase();
        if (err == ESP_OK) {
            req->send(200, "text/plain", "coredump erased");
        } else {
            req->send(500, "text/plain", esp_err_to_name(err));
        }
    });

    // GET /api/sys/board -- per-board hardware identity. The web sys
    // tab uses this to render the right board name / display / SD /
    // optional peripherals (IMU, RGB LED, battery ADC) so a single
    // shared sys.html serves both boards.
    s_server->on("/api/sys/board", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
#if defined(BOARD_WT32_SC01_PLUS)
        d["board"]   = "Wireless-Tag WT32-SC01 Plus";
        d["codename"] = "wt32_sc01_plus";
        d["mcu"]     = "ESP32-S3R2 \xC2\xB7 240 MHz";
        d["memory"]  = "512 KB SRAM \xC2\xB7 2 MB QSPI PSRAM \xC2\xB7 16 MB flash";
        d["display"] = "ST7796 320x480 (LovyanGFX i80)";
        d["touch"]   = "FT6336 \xC2\xB7 0x38";
        d["sd"]      = "SDMMC \xC2\xB7 1-bit";
        d["imu"]     = nullptr;       // no IMU exposed on this board
        d["rgb_led"] = nullptr;       // no RGB LED
        d["battery_adc"] = nullptr;   // external power only
#else
        d["board"]   = "Waveshare ESP32-S3-LCD-1.47B";
        d["codename"] = "wsh_s3_lcd_147b";
        d["mcu"]     = "ESP32-S3R8 \xC2\xB7 240 MHz";
        d["memory"]  = "512 KB SRAM \xC2\xB7 8 MB OPI PSRAM \xC2\xB7 16 MB flash";
        d["display"] = "ST7789 172x320 IPS (Arduino_GFX SPI)";
        d["touch"]   = nullptr;       // no touch
        d["sd"]      = "SDMMC \xC2\xB7 1-bit (mounted on demand)";
        d["imu"]     = "QMI8658 \xC2\xB7 0x6B";
        d["rgb_led"] = "WS2812 \xC2\xB7 GPIO 38";
        d["battery_adc"] = "GPIO 1 (LiPo divider)";
#endif
        d["sdk"]      = ESP.getSdkVersion();
        d["chip_rev"] = (int)ESP.getChipRevision();
        d["flash_mb"] = (int)(ESP.getFlashChipSize() / (1024 * 1024));
        d["psram_mb"] = (int)(ESP.getPsramSize() / (1024 * 1024));
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

#if defined(BOARD_WT32_SC01_PLUS)
    // GET /api/sys/screenshot.bmp -- capture the current LVGL screen
    // and stream it back as a 24-bit BMP. Used to inspect the UI from
    // off-board (no need for the user to describe what's on screen).
    //
    // Memory: 320 * 480 * 3 = 460800 bytes for the snapshot buffer
    // (RGB888) + ~50 bytes for the BMP headers. We allocate from PSRAM
    // (~2 MB free on this board) so heap stays available.
    s_server->on("/api/sys/screenshot.bmp", HTTP_GET, [](AsyncWebServerRequest *req) {
        // Cross-task LVGL access. AsyncWebServer runs us on the AsyncTCP
        // task; loopTask is concurrently running lv_timer_handler. Grab
        // BoardApp::lvLock to block timer ticks during the snapshot, or
        // bail with 503 if we can't get it within 500 ms.
        if (!BoardApp::lvLock()) {
            req->send(503, "text/plain", "LVGL busy (couldn't grab lock in 500ms)");
            return;
        }
        struct LockGuard { ~LockGuard() { BoardApp::lvUnlock(); } } _lock_guard;

        lv_obj_t *scr = lv_screen_active();
        if (!scr) { req->send(503, "text/plain", "no LVGL screen"); return; }

        const uint32_t W = lv_obj_get_width(scr);
        const uint32_t H = lv_obj_get_height(scr);
        // 16-bit RGB565 BMP: 2 bytes/pixel. For W = 320 or 480, row size
        // (W*2) is already 4-byte aligned, no padding needed. We pick
        // RGB565 over RGB888 to halve PSRAM peak (307 KB vs 460 KB) so
        // screenshots still work when an LVGL modal/keyboard is open and
        // the heap is fragmented.
        const uint32_t row_bytes   = W * 2;
        const uint32_t pixel_bytes = row_bytes * H;

        // 16-bit RGB565 BMP via BI_BITFIELDS compression. Single PSRAM
        // allocation (snapshot writes directly into the BMP buffer at
        // header_size offset). Header layout:
        //   14B file header
        //   40B DIB header (BITMAPINFOHEADER) with biCompression=BI_BITFIELDS=3
        //   12B color masks (R=0xF800, G=0x07E0, B=0x001F = RGB565)
        // = 66 byte header.
        const uint32_t header_size = 14 + 40 + 12;
        const uint32_t total_size  = header_size + pixel_bytes;
        uint8_t *bmp = (uint8_t *)heap_caps_malloc(total_size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bmp) {
            req->send(503, "text/plain", "PSRAM alloc (bmp) failed");
            return;
        }

        memset(bmp, 0, header_size);
        bmp[0] = 'B'; bmp[1] = 'M';
        *(uint32_t *)(bmp + 2)  = total_size;
        *(uint32_t *)(bmp + 10) = header_size;
        *(uint32_t *)(bmp + 14) = 40;                    // DIB header size
        *(int32_t  *)(bmp + 18) = (int32_t)W;
        *(int32_t  *)(bmp + 22) = (int32_t)H;            // positive = bottom-up
        *(uint16_t *)(bmp + 26) = 1;                     // planes
        *(uint16_t *)(bmp + 28) = 16;                    // bits/pixel
        *(uint32_t *)(bmp + 30) = 3;                     // BI_BITFIELDS
        *(uint32_t *)(bmp + 34) = pixel_bytes;
        *(int32_t  *)(bmp + 38) = 2835;                  // 72 dpi x
        *(int32_t  *)(bmp + 42) = 2835;                  // 72 dpi y
        *(uint32_t *)(bmp + 54) = 0xF800;                // R mask
        *(uint32_t *)(bmp + 58) = 0x07E0;                // G mask
        *(uint32_t *)(bmp + 62) = 0x001F;                // B mask

        // Snapshot directly into the BMP pixel area as RGB565.
        lv_image_dsc_t dsc;
        lv_result_t res = lv_snapshot_take_to_buf(
            scr, LV_COLOR_FORMAT_RGB565, &dsc,
            bmp + header_size, pixel_bytes);
        if (res != LV_RESULT_OK) {
            heap_caps_free(bmp);
            req->send(500, "text/plain", "lv_snapshot_take_to_buf failed");
            return;
        }

        // In-place row reverse (BMP is bottom-up; snapshot is top-down).
        // Stack temp = one row = max 480*2 = 960 bytes.
        uint8_t *pix = bmp + header_size;
        uint8_t row_tmp[960];
        for (uint32_t i = 0; i < H / 2; i++) {
            uint8_t *a = pix + i * row_bytes;
            uint8_t *b = pix + (H - 1 - i) * row_bytes;
            memcpy(row_tmp, a, row_bytes);
            memcpy(a, b, row_bytes);
            memcpy(b, row_tmp, row_bytes);
        }

        // Stream chunked from the assembled BMP buffer; free the buffer
        // after the response handler is done. Captured by-value into a
        // shared_ptr-like struct so the lambda owns the lifetime.
        struct Owner { uint8_t *p; uint32_t sz; ~Owner() { if (p) heap_caps_free(p); } };
        auto *owner = new Owner{ bmp, total_size };

        AsyncWebServerResponse *resp = req->beginResponse(
            "image/bmp", total_size,
            [owner](uint8_t *out, size_t maxLen, size_t index) -> size_t {
                if (index >= owner->sz) { delete owner; return 0; }
                size_t n = owner->sz - index;
                if (n > maxLen) n = maxLen;
                memcpy(out, owner->p + index, n);
                return n;
            });
        resp->addHeader("Content-Disposition",
                        "inline; filename=\"sc01_screenshot.bmp\"");
        req->send(resp);
    });

    // POST /api/sys/rotation?val=N -- emergency rotation reset.
    // If the user accidentally rotated into landscape and the home grid
    // hasn't been adapted yet, the Settings tile may scroll off-screen
    // and there's no way back from the touch UI. This endpoint sets
    // rotation in NVS directly + reboots; on next boot BoardDisplay
    // applies the new rotation.
    s_server->on("/api/sys/rotation", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("val")) {
            req->send(400, "text/plain", "missing val=N (0..3)");
            return;
        }
        int v = req->getParam("val")->value().toInt();
        if (v < 0 || v > 3) {
            req->send(400, "text/plain", "val must be 0..3");
            return;
        }
        BoardSettings::setRotation((uint8_t)v);
        AsyncWebServerResponse *resp = req->beginResponse(
            200, "text/plain",
            String("rotation set to ") + v + ", rebooting...");
        resp->addHeader("Connection", "close");
        req->send(resp);
        xTaskCreate([](void*) {
            WiFi.disconnect(true, false);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }, "rotReset", 2048, nullptr, 1, nullptr);
    });

    // POST /api/sys/ui/tap?x=NN&y=NN -- synthesize a tap at (x,y).
    // The pair (PRESS + RELEASE @60ms) is enqueued for the LVGL indev
    // callback to deliver, so all normal click semantics fire. Returns
    // 200 OK if queued, 503 if the synthetic queue is saturated.
    s_server->on("/api/sys/ui/tap", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("x") || !req->hasParam("y")) {
            req->send(400, "text/plain", "missing x/y query params");
            return;
        }
        int x = req->getParam("x")->value().toInt();
        int y = req->getParam("y")->value().toInt();
        if (BoardApp::injectTap((int16_t)x, (int16_t)y)) {
            req->send(200, "text/plain",
                      String("tap queued @ (") + x + "," + y + ")");
        } else {
            req->send(503, "text/plain", "synthetic touch queue full");
        }
    });
#endif  // BOARD_WT32_SC01_PLUS

    // POST /api/sys/beacon/now -- send one beacon RIGHT NOW (synchronous,
    // 5 s timeout). Returns the HTTP status code from the beacon target,
    // or a negative HTTPClient error.
    //
    // CRITICAL: registered BEFORE /api/sys/beacon because
    // AsyncCallbackWebHandler does prefix-match -- if the broader route
    // is registered first, it shadows /now (same trap that bit the DFU
    // routes; see project_dfu_sticky_session.md in memory).
    s_server->on("/api/sys/beacon/now", HTTP_POST, [](AsyncWebServerRequest *req) {
        String url = BoardSettings::beaconUrl();
        if (url.length() == 0) {
            req->send(400, "text/plain", "no beacon URL configured");
            return;
        }
        int code = Safety::beaconSendNow(url.c_str());
        req->send(code >= 200 && code < 400 ? 200 : 502, "text/plain",
                  String("beacon -> ") + String(code));
    });

    // GET /api/sys/beacon -- return current beacon config.
    s_server->on("/api/sys/beacon", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["url"]              = BoardSettings::beaconUrl();
        d["interval_ms"]      = BoardSettings::beaconIntervalMs();
        d["interval_minutes"] = BoardSettings::beaconIntervalMs() / 60000;
        d["enabled"]          = BoardSettings::beaconUrl().length() > 0
                              && BoardSettings::beaconIntervalMs() > 0;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // POST /api/sys/beacon?url=...&minutes=N -- save config.
    // Pass empty url + minutes=0 to disable.
    s_server->on("/api/sys/beacon", HTTP_POST, [](AsyncWebServerRequest *req) {
        String url     = req->hasParam("url",     true) ? req->getParam("url",     true)->value()
                       : req->hasParam("url")           ? req->getParam("url")          ->value() : "";
        String minutes = req->hasParam("minutes", true) ? req->getParam("minutes", true)->value()
                       : req->hasParam("minutes")       ? req->getParam("minutes")      ->value() : "0";
        uint32_t mins  = (uint32_t)minutes.toInt();
        BoardSettings::setBeacon(url, mins * 60UL * 1000UL);
        String resp = "saved url=\"" + url + "\" interval=" + String(mins) + " min";
        req->send(200, "text/plain", resp);
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
