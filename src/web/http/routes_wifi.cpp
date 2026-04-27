// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_wifi.h.
#include "routes_wifi.h"

#include "../wifi_manager.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

namespace RoutesWifi {

void registerRoutesWifi(AsyncWebServer *s_server) {

    s_server->on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            deserializeJson(doc, data, len);
            const char* ssid = doc["ssid"];
            const char* pass = doc["pass"];
            if (!ssid) { req->send(400, "text/plain", "missing ssid"); return; }
            WifiManager::saveCredentials(ssid, pass ? pass : "");
            req->send(200, "text/plain", "Saved. Reboot to connect to: " + String(ssid));
        });

    s_server->on("/api/wifi/clear", HTTP_GET, [](AsyncWebServerRequest *req) {
        WifiManager::clearCredentials();
        req->send(200, "text/plain", "WiFi credentials cleared. Reboot to go to AP mode.");
    });

    // Async WiFi scan — start. Returns immediately; results polled via /scan_results.
    // Note: while scanning the AP radio briefly hops channels, so connected
    // clients see a ~1-3s hiccup but don't fully disconnect.
    s_server->on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest *req) {
        int16_t st = WiFi.scanComplete();
        if (st == WIFI_SCAN_RUNNING) {
            req->send(200, "text/plain", "scan already in progress");
            return;
        }
        // true=async, false=show_hidden
        int16_t n = WiFi.scanNetworks(true, false);
        if (n == WIFI_SCAN_FAILED) {
            req->send(500, "text/plain", "scan start failed");
            return;
        }
        req->send(200, "text/plain", "scan started");
    });

    s_server->on("/api/wifi/scan_results", HTTP_GET, [](AsyncWebServerRequest *req) {
        int16_t n = WiFi.scanComplete();
        JsonDocument doc;
        if (n == WIFI_SCAN_RUNNING) {
            doc["done"] = false;
        } else if (n == WIFI_SCAN_FAILED || n < 0) {
            doc["done"] = true;
            doc["nets"].to<JsonArray>();  // empty
        } else {
            doc["done"] = true;
            JsonArray nets = doc["nets"].to<JsonArray>();
            for (int i = 0; i < n; i++) {
                JsonObject o = nets.add<JsonObject>();
                o["ssid"] = WiFi.SSID(i);
                o["rssi"] = WiFi.RSSI(i);
                o["ch"]   = WiFi.channel(i);
                o["enc"]  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;
            }
            WiFi.scanDelete();  // free results after reporting
        }
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
}

} // namespace RoutesWifi
