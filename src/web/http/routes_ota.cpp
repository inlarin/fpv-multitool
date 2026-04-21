// AUTO-REFACTORED from web_server.cpp 2026-04-21. Plate self-update (OTA)
// endpoints — /api/ota/{info,check,pull,abort} plus multipart upload at
// /api/ota. Uses ESP32 Arduino Update + HTTPUpdate over WiFiClientSecure
// for the GitHub-release pull path.
#include "routes_ota.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>

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
            WiFiClientSecure client;
            client.setInsecure();

            httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            httpUpdate.rebootOnUpdate(false);
            httpUpdate.onProgress([](int cur, int total) {
                if (total > 0) s_otaPull.progress = (int)((int64_t)cur * 100 / total);
            });
            httpUpdate.onStart([]() { s_otaPull.message = "downloading"; });
            httpUpdate.onEnd  ([]() { s_otaPull.message = "verifying"; });
            httpUpdate.onError([](int err) {
                s_otaPull.message = String("error ") + err + ": " + httpUpdate.getLastErrorString();
            });

            t_httpUpdate_return ret = httpUpdate.update(client, s_otaPull.url);
            switch (ret) {
                case HTTP_UPDATE_FAILED:
                    s_otaPull.message = String("FAILED: ") + httpUpdate.getLastErrorString();
                    s_otaPull.running = false;
                    break;
                case HTTP_UPDATE_NO_UPDATES:
                    s_otaPull.message = "no update";
                    s_otaPull.running = false;
                    break;
                case HTTP_UPDATE_OK:
                    s_otaPull.message = "OK — rebooting";
                    s_otaPull.progress = 100;
                    vTaskDelay(pdMS_TO_TICKS(500));
                    ESP.restart();
                    break;
            }
            vTaskDelete(nullptr);
        }, "otaPull", 8192, nullptr, 1, nullptr);

        req->send(202, "text/plain", "Pull started — poll /api/ota/info");
    });

    s_server->on("/api/ota", HTTP_POST,
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
                Serial.println("[OTA] Success — rebooting in 500 ms");
                xTaskCreate([](void*) {
                    vTaskDelay(pdMS_TO_TICKS(500));
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
