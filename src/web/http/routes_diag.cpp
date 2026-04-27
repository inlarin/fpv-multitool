// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_diag.h.
#include "routes_diag.h"

#include "../../usb_emu/cp2112_emu.h"
#include "../../battery/smbus.h"

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
