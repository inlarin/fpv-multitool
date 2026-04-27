// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_crsf.h.
#include "routes_crsf.h"

#include "../web_state.h"
#include "../../core/pin_port.h"
#include "../../crsf/crsf_service.h"
#include "../../crsf/crsf_config.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

namespace RoutesCrsf {

void registerRoutesCrsf(AsyncWebServer *s_server) {

    s_server->on("/api/crsf/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf")) {
            req->send(409, "text/plain", "Port B busy — switch to UART in System → Port B Mode");
            return;
        }
        bool inv = req->hasParam("inverted") ? req->getParam("inverted")->value() == "1" : false;
        WebState::crsf.inverted = inv;
        CRSFService::begin(&Serial1,
                           PinPort::rx_pin(PinPort::PORT_B),
                           PinPort::tx_pin(PinPort::PORT_B),
                           420000, inv);
        CRSFConfig::init();
        WebState::crsf.enabled = true;
        req->send(200, "text/plain", inv ? "CRSF started (inverted)" : "CRSF started");
    });

    s_server->on("/api/crsf/state", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["running"]   = CRSFService::isRunning();
        if (CRSFService::isRunning()) {
            const auto &st = CRSFService::state();
            d["connected"]    = st.connected;
            d["total_frames"] = st.total_frames;
            d["bad_crc"]      = st.bad_crc;
            if (st.link.valid) {
                JsonObject l = d["link"].to<JsonObject>();
                l["rssi1"] = (int8_t)st.link.uplink_rssi1 * -1;
                l["lq"]    = st.link.uplink_link_quality;
                l["snr"]   = st.link.uplink_snr;
                l["rf"]    = st.link.rf_mode;
            }
            const auto &dev = CRSFConfig::deviceInfo();
            if (dev.valid) {
                JsonObject dv = d["device"].to<JsonObject>();
                dv["name"]   = dev.name;
                dv["fw"]     = dev.sw_ver;
                dv["hw"]     = dev.hw_ver;
                dv["serial"] = dev.serial;
                dv["fields"] = dev.field_count;
            }
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/crsf/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        CRSFService::end();
        // Flush the cached parameter table — previously cached params from
        // the just-stopped RX would otherwise survive a subsequent /start
        // against a different RX and leak into the new session's UI.
        CRSFConfig::reset();
        WebState::crsf.enabled = false;
        PinPort::release(PinPort::PORT_B);
        req->send(200, "text/plain", "CRSF stopped");
    });

    // NOTE: /api/crsf/{ping,params,read_params_blind,params_list,write,bind,wifi}
    // removed in Option A receiver-tab merge (2026-04-22). Their UI callers were
    // deleted as duplicates of the ELRS-tab equivalents (/api/elrs/device_info,
    // /api/elrs/params, /api/elrs/params/write, /api/elrs/bind). The live CRSF
    // telemetry service still uses /api/crsf/{start,stop,state,reboot}.

    s_server->on("/api/crsf/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        CRSFService::cmdReboot();
        req->send(200, "text/plain", "Reboot command sent");
    });
}

} // namespace RoutesCrsf
