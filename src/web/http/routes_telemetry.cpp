// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_telemetry.h.
#include "routes_telemetry.h"

#include "../web_state.h"
#include "../../fpv/esc_telem.h"
#include "../../rc_sniffer/rc_sniffer.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

namespace RoutesTelemetry {

void registerRoutesTelemetry(AsyncWebServer *s_server) {

    // ===== Servo state + sweep recorder =====
    s_server->on("/api/servo/state", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        WebState::Lock lock;
        d["active"]        = WebState::servo.active;
        d["sweep"]         = WebState::servo.sweep;
        d["pulseUs"]       = WebState::servo.pulseUs;
        d["freq"]          = WebState::servo.freq;
        d["sweepMinUs"]    = WebState::servo.sweepMinUs;
        d["sweepMaxUs"]    = WebState::servo.sweepMaxUs;
        d["sweepPeriodMs"] = WebState::servo.sweepPeriodMs;
        d["markedMinUs"]   = WebState::servo.markedMinUs;
        d["markedMaxUs"]   = WebState::servo.markedMaxUs;
        d["observedMinUs"] = WebState::servo.observedMinUs;
        d["observedMaxUs"] = WebState::servo.observedMaxUs;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // ===== ESC Telemetry (KISS / BLHeli_32 10-byte frame on ELRS_RX UART) =====
    s_server->on("/api/esc/telem/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        int poles = 14;
        if (req->hasParam("poles", true)) poles = req->getParam("poles", true)->value().toInt();
        if (poles < 2 || poles > 40) poles = 14;
        ESCTelem::start((uint8_t)poles);
        req->send(200, "text/plain", String("ESC telem started, polePairs=") + ESCTelem::polePairs());
    });
    s_server->on("/api/esc/telem/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        ESCTelem::stop();
        req->send(200, "text/plain", "stopped");
    });
    s_server->on("/api/esc/telem/state", HTTP_GET, [](AsyncWebServerRequest *req) {
        const auto &st = ESCTelem::state();
        JsonDocument d;
        d["running"]     = st.running;
        d["connected"]   = st.connected;
        d["frameCount"]  = st.frameCount;
        d["crcErrors"]   = st.crcErrors;
        d["frameRateHz"] = st.frameRateHz;
        d["polePairs"]   = ESCTelem::polePairs();
        if (st.frameCount > 0) {
            d["temp_c"]          = st.last.temp_c;
            d["voltage_V"]       = st.last.voltage_cV / 100.0f;
            d["current_A"]       = st.last.current_cA / 100.0f;
            d["consumption_mAh"] = st.last.consumption_mAh;
            uint32_t erpm = (uint32_t)st.last.erpm * 100;
            d["erpm"]            = erpm;
            uint8_t pp = ESCTelem::polePairs();
            d["rpm"]             = pp > 0 ? erpm / pp : 0;
        }
        d["maxTemp"]         = st.maxTemp;
        d["maxCurrent_A"]    = st.maxCurrent_cA / 100.0f;
        d["maxErpm"]         = (uint32_t)st.maxErpm * 100;
        d["peakVoltage_V"]   = st.peakVoltage_cV / 100.0f;
        d["minVoltage_V"]    = st.minVoltage_cV / 100.0f;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // ===== RC Protocol Sniffer (SBUS / iBus / PPM) =====
    s_server->on("/api/rc/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("proto", true)) { req->send(400, "text/plain", "need proto"); return; }
        String p = req->getParam("proto", true)->value();
        RCProto proto = RC_PROTO_NONE;
        if (p == "sbus") proto = RC_PROTO_SBUS;
        else if (p == "ibus") proto = RC_PROTO_IBUS;
        else if (p == "ppm")  proto = RC_PROTO_PPM;
        else if (p == "auto") { RCSniffer::autoDetect(); req->send(200, "text/plain",
            String("Auto-detected: ") + RCSniffer::protoName(RCSniffer::state().proto)); return; }
        else { req->send(400, "text/plain", "proto must be sbus/ibus/ppm/auto"); return; }
        RCSniffer::start(proto);
        req->send(200, "text/plain", String("Started ") + RCSniffer::protoName(proto));
    });

    s_server->on("/api/rc/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        RCSniffer::stop();
        req->send(200, "text/plain", "stopped");
    });

    s_server->on("/api/rc/state", HTTP_GET, [](AsyncWebServerRequest *req) {
        const auto &st = RCSniffer::state();
        JsonDocument d;
        d["proto"]        = RCSniffer::protoName(st.proto);
        d["running"]      = RCSniffer::isRunning();
        d["connected"]    = st.connected;
        d["channelCount"] = st.channelCount;
        d["frameCount"]   = st.frameCount;
        d["crcErrors"]    = st.crcErrors;
        d["frameRateHz"]  = st.frameRateHz;
        d["failsafe"]     = st.failsafe;
        d["lostFrame"]    = st.lostFrame;
        JsonArray ch = d["channels"].to<JsonArray>();
        for (uint8_t i = 0; i < st.channelCount && i < 16; i++) ch.add(st.channels[i]);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });
}

} // namespace RoutesTelemetry
