// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_usb.h.
#include "routes_usb.h"

#include "../../core/usb_mode.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

namespace RoutesUsb {

void registerRoutesUsb(AsyncWebServer *s_server) {

    s_server->on("/api/usb/mode", HTTP_GET, [](AsyncWebServerRequest *req) {
        UsbDescriptorMode prefer = UsbMode::load();
        UsbDescriptorMode act    = UsbMode::active();
        JsonDocument j;
        j["active"]         = (int)act;
        j["active_name"]    = UsbMode::name(act);
        j["preferred"]      = (int)prefer;
        j["preferred_name"] = UsbMode::name(prefer);
        j["reboot_pending"] = (act != prefer);
        // Back-compat: older UI builds still read `current` / `current_name`.
        j["current"]        = (int)act;
        j["current_name"]   = UsbMode::name(act);
        JsonArray arr = j["modes"].to<JsonArray>();
        for (int i = 0; i <= USB_MODE_USB2I2C; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]   = i;
            o["name"] = UsbMode::name((UsbDescriptorMode)i);
        }
        String out; serializeJson(j, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/usb/mode", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("mode", true)) {
            req->send(400, "text/plain", "Missing 'mode' form param");
            return;
        }
        int v = req->getParam("mode", true)->value().toInt();
        if (v < 0 || v > USB_MODE_USB2I2C) {
            req->send(400, "text/plain", "Invalid mode");
            return;
        }
        UsbMode::save((UsbDescriptorMode)v);
        req->send(200, "text/plain", "Saved. Reboot to apply.");
    });

    s_server->on("/api/usb/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Rebooting...");
        delay(200);
        ESP.restart();
    });
}

} // namespace RoutesUsb
