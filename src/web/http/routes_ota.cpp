// AUTO-REFACTORED from web_server.cpp 2026-04-21. Plate self-update (OTA)
// endpoints — /api/ota/{info,check,pull,abort,upload}. Multipart upload
// MUST live under a sub-path: ESPAsyncWebServer's AsyncCallbackWebHandler
// matches `/api/ota` against ANY URL starting with `/api/ota/` (line 305 of
// WebHandlers.cpp — kept for `/users/{id}` style routes), so registering
// `/api/ota` first would shadow `/api/ota/abort`, `/api/ota/pull`, etc.
// The pull path runs the download manually instead of httpUpdate.update():
// httpUpdate's redirect-following + chunked TE handling corrupts the last
// bytes when GitHub redirects to objects.githubusercontent.com — the
// resulting image fails esp_ota_set_boot_partition() with "Could Not
// Activate The Firmware" even though byte-for-byte the upload of the same
// file works. We download into PSRAM then feed Update.write() in one go.
#include "routes_ota.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#if __has_include("core/build_info.h")
  #include "core/build_info.h"
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef GITHUB_REPO
#define GITHUB_REPO ""
#endif

namespace RoutesOta {

// Background OTA-pull state used by the /api/ota/pull xTask to publish
// progress into /api/ota/info without blocking the HTTP handler.
struct OtaPullState {
    volatile bool     running = false;
    volatile int      progress = 0;      // 0..100
    volatile int      http_code = 0;
    String            message;
    String            url;
};
static OtaPullState s_otaPull;
static String       s_latestVersion;
static String       s_latestAssetUrl;
static uint32_t     s_latestCheckedMs = 0;

void registerRoutesOta(AsyncWebServer *s_server) {

    s_server->on("/api/ota/info", HTTP_GET, [](AsyncWebServerRequest *req) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *next    = esp_ota_get_next_update_partition(nullptr);
        JsonDocument j;
        j["running"]   = running ? running->label : "?";
        j["next"]      = next    ? next->label    : "?";
        j["app_size"]  = ESP.getSketchSize();
        j["app_free"]  = ESP.getFreeSketchSpace();
        j["sdk"]       = ESP.getSdkVersion();
        j["fw_version"] = FW_VERSION;
        j["github_repo"] = GITHUB_REPO;
        j["latest"]    = s_latestVersion;
        j["latest_url"] = s_latestAssetUrl;
        j["pull_running"]  = s_otaPull.running;
        j["pull_progress"] = s_otaPull.progress;
        j["pull_message"]  = s_otaPull.message;
        String out; serializeJson(j, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/ota/check", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (String(GITHUB_REPO).isEmpty()) {
            req->send(400, "text/plain", "GITHUB_REPO not configured at build time");
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            req->send(503, "text/plain", "WiFi not in STA mode — connect to internet first");
            return;
        }

        WiFiClientSecure client;
        client.setInsecure();  // GitHub has a valid cert chain; we skip pinning to keep flash small
        HTTPClient https;
        https.setUserAgent("esp32-fpv-multitool");
        https.setTimeout(10000);
        String api = String("https://api.github.com/repos/") + GITHUB_REPO + "/releases/latest";
        if (!https.begin(client, api)) { req->send(500, "text/plain", "HTTP begin failed"); return; }

        int code = https.GET();
        if (code != 200) {
            String err = "GitHub API HTTP " + String(code);
            https.end();
            req->send(502, "text/plain", err);
            return;
        }
        String payload = https.getString();
        https.end();

        JsonDocument doc;
        DeserializationError derr = deserializeJson(doc, payload);
        if (derr) { req->send(500, "text/plain", String("JSON err: ") + derr.c_str()); return; }

        s_latestVersion  = doc["tag_name"] | "";
        s_latestAssetUrl = "";
        for (JsonObject a : doc["assets"].as<JsonArray>()) {
            String name = a["name"] | "";
            if (name == "firmware.bin") {
                s_latestAssetUrl = a["browser_download_url"].as<const char*>();
                break;
            }
        }
        s_latestCheckedMs = millis();

        JsonDocument out;
        out["current"]  = FW_VERSION;
        out["latest"]   = s_latestVersion;
        out["asset"]    = s_latestAssetUrl;
        out["outdated"] = (s_latestVersion.length() > 0 && s_latestVersion != String(FW_VERSION));
        String outs; serializeJson(out, outs);
        req->send(200, "application/json", outs);
    });

    s_server->on("/api/ota/pull", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (s_otaPull.running) { req->send(409, "text/plain", "Already in progress"); return; }
        if (s_latestAssetUrl.isEmpty()) { req->send(400, "text/plain", "Run /api/ota/check first"); return; }
        if (WiFi.status() != WL_CONNECTED) { req->send(503, "text/plain", "WiFi STA not connected"); return; }

        s_otaPull.running  = true;
        s_otaPull.progress = 0;
        s_otaPull.http_code = 0;
        s_otaPull.message  = "starting";
        s_otaPull.url      = s_latestAssetUrl;

        xTaskCreate([](void *) {
            // Two-stage download:
            //   1. HTTPClient::GET → buffer entire .bin in PSRAM
            //   2. Update.begin() + Update.write(buf, size) + Update.end()
            // This avoids httpUpdate.update()'s streaming-with-redirect path
            // which empirically corrupts the last few bytes when the
            // GitHub redirect lands on objects.githubusercontent.com — the
            // image then fails esp_ota_set_boot_partition() at activation
            // even though SHA256 of the served file is correct.
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient https;
            https.setUserAgent("esp32-fpv-multitool");
            https.setTimeout(15000);
            https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

            s_otaPull.message = "connecting";
            if (!https.begin(client, s_otaPull.url)) {
                s_otaPull.message = "FAILED: HTTP begin";
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            int code = https.GET();
            s_otaPull.http_code = code;
            if (code != 200) {
                s_otaPull.message = String("FAILED: HTTP ") + code;
                https.end();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            int len = https.getSize();
            if (len <= 0 || len > 4 * 1024 * 1024) {
                s_otaPull.message = String("FAILED: bad Content-Length=") + len;
                https.end();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            uint8_t *buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
            if (!buf) {
                s_otaPull.message = "FAILED: PSRAM alloc";
                https.end();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            s_otaPull.message = "downloading";
            WiFiClient *stream = https.getStreamPtr();
            size_t got = 0;
            uint32_t deadline = millis() + 90000;  // 90s for 1.5 MB on flaky links
            while (got < (size_t)len && millis() < deadline) {
                int avail = stream->available();
                if (avail <= 0) { delay(2); continue; }
                size_t want = (size_t)len - got;
                if ((size_t)avail < want) want = avail;
                if (want > 4096) want = 4096;
                int n = stream->readBytes(buf + got, want);
                if (n > 0) got += n;
                s_otaPull.progress = (int)((int64_t)got * 100 / len);
            }
            https.end();

            if (got != (size_t)len) {
                heap_caps_free(buf);
                char m[64];
                snprintf(m, sizeof(m), "FAILED: short read %u/%d", (unsigned)got, len);
                s_otaPull.message = m;
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            s_otaPull.message = "writing";
            if (!Update.begin(got, U_FLASH)) {
                heap_caps_free(buf);
                s_otaPull.message = String("FAILED begin: ") + Update.errorString();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }
            size_t written = Update.write(buf, got);
            heap_caps_free(buf);
            if (written != got) {
                s_otaPull.message = String("FAILED write: ") + Update.errorString();
                Update.abort();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            s_otaPull.message = "activating";
            if (!Update.end(true)) {
                s_otaPull.message = String("FAILED activate: ") + Update.errorString();
                s_otaPull.running = false;
                vTaskDelete(nullptr);
                return;
            }

            s_otaPull.message = "OK — rebooting";
            s_otaPull.progress = 100;
            // Clean STA disassoc — sends 802.11 deauth so the AP drops the
            // old association immediately. Without this, AP keeps the stale
            // session for ~ap_max_inactivity (240 s on most consumer routers),
            // and the just-rebooted plate then tries to re-associate while
            // the AP still considers it connected → reassoc NACKed, plate
            // sits silently in AP fallback for 4 minutes. Verified empirically
            // on v0.28.4 OTA pulls.
            WiFi.disconnect(true, false);  // disassoc, keep saved creds
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP.restart();
            vTaskDelete(nullptr);
        }, "otaPull", 8192, nullptr, 1, nullptr);

        req->send(202, "text/plain", "Pull started — poll /api/ota/info");
    });

    // POST /api/ota/upload — multipart firmware.bin → flash + reboot.
    // (NOT `/api/ota` — that path would shadow every other /api/ota/* route
    // because of AsyncCallbackWebHandler's prefix-match rule.)
    s_server->on("/api/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            // `otaBeginOk` is set in the upload handler — if begin() failed
            // we must report FAIL even if `Update.hasError()` happens to be
            // clear (e.g. ESP32 Arduino variants where never-started Update
            // state leaves _error at 0).
            bool beginOk = !!req->_tempObject;  // reused as bool flag
            bool ok = beginOk && !Update.hasError() && Update.isFinished();
            AsyncWebServerResponse *resp = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK — rebooting" :
                     (String("FAIL: ") +
                      (!beginOk ? "Update.begin() rejected (partition full? run /api/ota/abort)"
                                : Update.errorString())));
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                Serial.println("[OTA] Success — rebooting in 1500 ms (after deauth)");
                xTaskCreate([](void*) {
                    // Same deauth-then-restart dance as the pull path —
                    // see /api/ota/pull comment for why 1.5 s + WiFi.disconnect.
                    WiFi.disconnect(true, false);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    ESP.restart();
                }, "otaReboot", 2048, nullptr, 1, nullptr);
            }
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                Serial.printf("[OTA] Upload start: %s\n", filename.c_str());
                size_t content_len = req->contentLength();
                if (Update.begin(content_len > 0 ? content_len : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    req->_tempObject = (void*)1;  // begin-ok marker
                } else {
                    Update.printError(Serial);
                    req->_tempObject = nullptr;
                }
            }
            if (!req->_tempObject) return;  // skip writes when begin failed
            if (Update.isRunning() && Update.write(data, len) != len) {
                Update.printError(Serial);
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Upload complete: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        });

    s_server->on("/api/ota/abort", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (Update.isRunning()) Update.abort();
        req->send(200, "text/plain", "Aborted");
    });

}  // registerRoutesOta
}  // namespace RoutesOta
