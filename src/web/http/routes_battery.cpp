// AUTO-REFACTORED from web_server.cpp 2026-04-21 (step 4).
// Battery / DJI SMBus / dataflash / clone lab endpoints — /api/batt/*,
// /api/smbus/*. Also owns the clone-log mutex and three function-scope
// state blocks (s_brute, s_macResp, s_logger) that previously lived inside
// WebServer::start().
#include "routes_battery.h"

#include "../web_state.h"
#include "../../battery/dji_battery.h"
#include "../../battery/smbus.h"
#include "../../battery/battery_profiles.h"
#include "../../battery/dataflash_map.h"
#include "pin_config.h"

#include <Arduino.h>
#include <Wire.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern "C" int cp2112_log_dump(char *out, int cap);
extern "C" uint32_t cp2112_log_seq();

namespace RoutesBattery {

// Mutex guarding the three clone-lab state structs (s_brute.hits,
// s_macResp.hits / baselineHex, s_logger.entries). The brute / logger
// tasks append to these from their own threads; HTTP handlers read + clear.
static SemaphoreHandle_t s_cloneLogMutex = nullptr;
struct CloneLogGuard {
    CloneLogGuard()  { if (s_cloneLogMutex) xSemaphoreTake(s_cloneLogMutex, portMAX_DELAY); }
    ~CloneLogGuard() { if (s_cloneLogMutex) xSemaphoreGive(s_cloneLogMutex); }
};

void registerRoutesBattery(AsyncWebServer *s_server) {
    if (!s_cloneLogMutex) s_cloneLogMutex = xSemaphoreCreateMutex();


    s_server->on("/api/batt/diag", HTTP_GET, [](AsyncWebServerRequest *req) {
        const uint8_t BATT = 0x0B;
        auto toHex = [](const uint8_t *b, int n) {
            String s; s.reserve(n*3);
            for (int i = 0; i < n; i++) { char h[4]; snprintf(h, sizeof(h), "%02X ", b[i]); s += h; }
            s.trim(); return s;
        };
        auto toAscii = [](const uint8_t *b, int n) {
            String s; s.reserve(n);
            for (int i = 0; i < n; i++) s += (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
            return s;
        };

        if (req->hasParam("mac")) {
            String v = req->getParam("mac")->value();
            uint16_t sub = (uint16_t)strtoul(v.c_str(), nullptr, 0);
            uint8_t buf[32] = {0};
            int n = SMBus::macBlockRead(BATT, sub, buf, 32);
            JsonDocument d;
            d["sub"] = v; d["len"] = n;
            d["hex"] = (n > 0) ? toHex(buf, n) : String("");
            d["ascii"] = (n > 0) ? toAscii(buf, n) : String("");
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
            return;
        }
        if (req->hasParam("sbs")) {
            uint8_t reg = (uint8_t)strtoul(req->getParam("sbs")->value().c_str(), nullptr, 0);
            String t = req->hasParam("type") ? req->getParam("type")->value() : String("word");
            JsonDocument d;
            d["reg"] = String("0x") + String(reg, HEX);
            if (t == "word") {
                uint16_t w = SMBus::readWord(BATT, reg);
                d["word"] = String("0x") + String(w, HEX);
                d["dec"] = w;
            } else if (t == "dword") {
                uint32_t dw = SMBus::readDword(BATT, reg);
                d["dword"] = String("0x") + String(dw, HEX);
            } else if (t == "block" || t == "string") {
                uint8_t buf[32] = {0};
                int n = SMBus::readBlock(BATT, reg, buf, 31);
                d["len"] = n;
                d["hex"] = (n > 0) ? toHex(buf, n) : String("");
                d["ascii"] = (n > 0) ? toAscii(buf, n) : String("");
            } else { d["error"] = "bad type"; }
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
            return;
        }
        if (req->hasParam("unseal")) {
            String v = req->getParam("unseal")->value();
            int comma = v.indexOf(',');
            if (comma <= 0) { req->send(400, "text/plain", "expect unseal=w1,w2"); return; }
            uint16_t w1 = (uint16_t)strtoul(v.substring(0, comma).c_str(), nullptr, 0);
            uint16_t w2 = (uint16_t)strtoul(v.substring(comma + 1).c_str(), nullptr, 0);
            uint32_t combined = ((uint32_t)w2 << 16) | w1;
            UnsealResult r = DJIBattery::unsealWithKey(combined);
            uint32_t op = DJIBattery::readOperationStatus();
            JsonDocument d;
            d["w1"] = String("0x") + String(w1, HEX);
            d["w2"] = String("0x") + String(w2, HEX);
            d["result"] = r == UNSEAL_OK ? "OK" : r == UNSEAL_REJECTED_SEALED ? "still sealed" : r == UNSEAL_NO_RESPONSE ? "no i2c" : "unsupported";
            d["opStatus"] = String("0x") + String(op, HEX);
            d["sealed"] = ((op >> 8) & 0x03) == 0x03;
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
            return;
        }
        req->send(400, "text/plain", "use ?mac= or ?sbs= or ?unseal=");
    });

    // === Mavic-3 service ops (recovered from commercial-tool reverse) ====
    // Each is a single endpoint that runs the unlock+op chain server-side and
    // returns a small JSON status. UI just POSTs to these; no params for the
    // simple ones; capacity takes ?mah=NNNN.
    s_server->on("/api/batt/mavic3/clear_pf", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = DJIBattery::clearPFProper();
        JsonDocument d; d["ok"] = ok;
        d["pfStatus"] = String("0x") + String(DJIBattery::readPFStatus(), HEX);
        d["pf2"]      = String("0x") + String(DJIBattery::readDJIPF2(), HEX);
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/reset_cycles", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = DJIBattery::resetCycles();
        JsonDocument d; d["ok"] = ok;
        d["cycleCount"] = SMBus::readWord(0x0B, 0x17);
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/clear_blackbox", HTTP_POST, [](AsyncWebServerRequest *req) {
        // ⚠ TEST_LOG note #29: clearBlackBox() is now no-op (MAC 0x0030 was
        // actually SEAL, not BB-clear). This endpoint now does ONLY
        // LifetimeData reset (MAC 0x0060). Kept the URL for backward compat.
        bool b = DJIBattery::resetLifetimeData();
        JsonDocument d;
        d["blackBoxOk"] = false;     // explicitly false -- BB-clear not supported
        d["blackBoxNote"] = "MAC 0x0030 is SEAL on PTL 2024; no BQ40Z80 MAC clears event log";
        d["lifetimeOk"] = b;
        d["ok"] = b;                 // success if at least lifetime reset worked
        String out; serializeJson(d, out);
        req->send(b ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/capacity", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("mah")) { req->send(400, "text/plain", "need ?mah=NNNN"); return; }
        uint16_t mah = (uint16_t)strtoul(req->getParam("mah")->value().c_str(), nullptr, 0);
        bool ok = DJIBattery::writeCapacity(mah);
        JsonDocument d;
        d["ok"] = ok; d["requested_mah"] = mah;
        d["readback_full_cap_mah"] = SMBus::readWord(0x0B, 0x10);
        d["readback_design_cap_mah"] = SMBus::readWord(0x0B, 0x18);
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/balance", HTTP_POST, [](AsyncWebServerRequest *req) {
        uint8_t mask = req->hasParam("mask")
            ? (uint8_t)strtoul(req->getParam("mask")->value().c_str(), nullptr, 0)
            : 0x0F;  // default: all 4 cells
        bool ok = DJIBattery::startBalancing(mask);
        JsonDocument d; d["ok"] = ok; d["mask"] = String("0x") + String(mask, HEX);
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/calibrate", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = DJIBattery::startCalibration();
        JsonDocument d; d["ok"] = ok;
        d["instructions"] = "charge full -> rest 30-60min -> discharge full -> rest -> charge full";
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });
    s_server->on("/api/batt/mavic3/unlock", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = DJIBattery::unlockForServiceOps();
        uint32_t op = DJIBattery::readOperationStatus();
        uint8_t sec = (op >> 8) & 0x03;
        JsonDocument d;
        d["ok"] = ok;
        d["operationStatus"] = String("0x") + String(op, HEX);
        d["sec"] = sec;
        d["state"] = sec == 0x00 ? "FullAccess" : sec == 0x02 ? "Unsealed" : "Sealed";
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });

    // MAC command catalog
    s_server->on("/api/batt/mac_catalog", HTTP_GET, [](AsyncWebServerRequest *req) {
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        for (int i = 0; i < MAC_CATALOG_LEN; i++) {
            if (i) r->print(",");
            r->printf("{\"sub\":\"0x%04X\",\"name\":\"%s\",\"desc\":\"%s\",\"rlen\":%u,\"destructive\":%s}",
                      MAC_CATALOG[i].subcommand, MAC_CATALOG[i].name, MAC_CATALOG[i].desc,
                      MAC_CATALOG[i].respLen, MAC_CATALOG[i].destructive ? "true" : "false");
        }
        r->print("]");
        req->send(r);
    });

    // Battery profiles (unseal key sets per model)
    s_server->on("/api/batt/profiles", HTTP_GET, [](AsyncWebServerRequest *req) {
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        for (int i = 0; i < NUM_PROFILES; i++) {
            if (i) r->print(",");
            r->printf("{\"id\":%d,\"name\":\"%s\",\"chip\":\"%s\",\"unsealKeys\":[",
                      i, BATTERY_PROFILES[i].name, BATTERY_PROFILES[i].chipName);
            for (int k = 0; k < BATTERY_PROFILES[i].numUnsealKeys; k++) {
                if (k) r->print(",");
                const UnsealKey &uk = BATTERY_PROFILES[i].unsealKeys[k];
                r->printf("{\"w1\":\"0x%04X\",\"w2\":\"0x%04X\",\"desc\":\"%s\"}", uk.w1, uk.w2, uk.desc);
            }
            r->print("],\"fasKeys\":[");
            for (int k = 0; k < BATTERY_PROFILES[i].numFasKeys; k++) {
                if (k) r->print(",");
                const UnsealKey &fk = BATTERY_PROFILES[i].fasKeys[k];
                r->printf("{\"w1\":\"0x%04X\",\"w2\":\"0x%04X\",\"desc\":\"%s\"}", fk.w1, fk.w2, fk.desc);
            }
            r->print("]}");
        }
        r->print("]");
        req->send(r);
    });

    // Generic SMBus transaction endpoint
    s_server->on("/api/smbus/xact", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("addr") || !req->hasParam("op")) {
            req->send(400, "text/plain", "need addr=0xNN&op=readWord|writeWord|readBlock|writeBlock|macBlockRead|macCmd&reg=0xNN[&data=AA,BB][&len=N]");
            return;
        }
        uint8_t addr = (uint8_t)strtoul(req->getParam("addr")->value().c_str(), nullptr, 0);
        String op = req->getParam("op")->value();
        uint8_t reg = req->hasParam("reg") ? (uint8_t)strtoul(req->getParam("reg")->value().c_str(), nullptr, 0) : 0;
        JsonDocument d;
        d["addr"] = String("0x") + String(addr, HEX);
        d["op"] = op;
        d["reg"] = String("0x") + String(reg, HEX);

        if (op == "readWord") {
            uint16_t w = SMBus::readWord(addr, reg);
            d["value"] = String("0x") + String(w, HEX); d["dec"] = w; d["ok"] = (w != 0xFFFF);
        } else if (op == "readBlock") {
            uint8_t buf[32]; int n = SMBus::readBlock(addr, reg, buf, 32);
            String hex; for (int i = 0; i < n; i++) { char h[4]; snprintf(h,4,"%02X ",buf[i]); hex += h; }
            d["len"] = n; d["hex"] = hex; d["ok"] = (n >= 0);
        } else if (op == "writeWord") {
            uint16_t val = req->hasParam("data") ? (uint16_t)strtoul(req->getParam("data")->value().c_str(), nullptr, 0) : 0;
            bool ok = SMBus::writeWord(addr, reg, val);
            d["ok"] = ok; d["value"] = String("0x") + String(val, HEX);
        } else if (op == "writeBlock") {
            uint8_t buf[32]; uint8_t len = 0;
            if (req->hasParam("data")) {
                String s = req->getParam("data")->value();
                int pos = 0;
                while (pos < (int)s.length() && len < 32) {
                    buf[len++] = (uint8_t)strtoul(s.c_str() + pos, nullptr, 16);
                    int next = s.indexOf(',', pos);
                    if (next < 0) break; pos = next + 1;
                }
            }
            bool ok = SMBus::writeBlock(addr, reg, buf, len);
            d["ok"] = ok; d["len"] = len;
        } else if (op == "macBlockRead") {
            uint16_t sub = (uint16_t)strtoul(req->hasParam("data") ? req->getParam("data")->value().c_str() : "0", nullptr, 0);
            uint8_t buf[32]; int n = SMBus::macBlockRead(addr, sub, buf, 32);
            String hex; for (int i = 0; i < n; i++) { char h[4]; snprintf(h,4,"%02X ",buf[i]); hex += h; }
            d["sub"] = String("0x") + String(sub, HEX); d["len"] = n; d["hex"] = hex; d["ok"] = (n >= 0);
        } else if (op == "macCmd") {
            uint16_t sub = (uint16_t)strtoul(req->hasParam("data") ? req->getParam("data")->value().c_str() : "0", nullptr, 0);
            bool ok = SMBus::macCommand(addr, sub);
            d["sub"] = String("0x") + String(sub, HEX); d["ok"] = ok;
        } else {
            d["error"] = "unknown op"; d["ok"] = false;
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Battery snapshot — all SBS registers + status in one JSON
    s_server->on("/api/batt/snapshot", HTTP_GET, [](AsyncWebServerRequest *req) {
        BatteryInfo info = DJIBattery::readAll();
        JsonDocument d;
        d["connected"]      = info.connected;
        d["deviceType"]     = (int)info.deviceType;
        d["voltage_mV"]     = info.voltage_mV;
        d["current_mA"]     = info.current_mA;
        d["avgCurrent_mA"]  = info.avgCurrent_mA;
        d["temperature_C"]  = info.temperature_C;
        d["soc"]            = info.stateOfCharge;
        d["absoluteSOC"]    = info.absoluteSOC;
        d["stateOfHealth"]  = info.stateOfHealth;
        d["fullCap_mAh"]    = info.fullCapacity_mAh;
        d["designCap_mAh"]  = info.designCapacity_mAh;
        d["designVoltage_mV"] = info.designVoltage_mV;
        d["remainCap_mAh"]  = info.remainCapacity_mAh;
        d["cycleCount"]     = info.cycleCount;
        d["batteryStatus"]  = String("0x") + String(info.batteryStatus, HEX);
        d["batteryStatusDecoded"] = DJIBattery::decodeBatteryStatus(info.batteryStatus);
        d["serialNumber"]   = info.serialNumber;
        d["manufactureDate"]= info.manufactureDate;
        d["mfrName"]        = info.manufacturerName;
        d["deviceName"]     = info.deviceName;
        d["chemistry"]      = info.chemistry;
        d["cellCount"]      = info.cellCount;
        // Time estimates
        d["runTimeToEmpty_min"] = info.runTimeToEmpty_min;
        d["avgTimeToEmpty_min"] = info.avgTimeToEmpty_min;
        d["timeToFull_min"]     = info.timeToFull_min;
        d["chargingCurrent_mA"] = info.chargingCurrent_mA;
        d["chargingVoltage_mV"] = info.chargingVoltage_mV;
        // Cells
        JsonArray cells = d["cellVoltage_mV"].to<JsonArray>();
        for (int i = 0; i < info.cellCount; i++) cells.add(info.cellVoltage[i]);
        if (info.daStatus1Valid) {
            JsonArray syncCells = d["cellVoltageSync_mV"].to<JsonArray>();
            for (int i = 0; i < info.cellCount; i++) syncCells.add(info.cellVoltSync[i]);
            d["packVoltage_mV"] = info.packVoltage;
        }
        // Chip/model
        d["chipType"]       = String("0x") + String(info.chipType, HEX);
        d["chipTypeName"]   = DJIBattery::chipTypeName(info.chipType);
        d["fwVersion"]      = info.firmwareVersion;
        d["hwVersion"]      = info.hardwareVersion;
        d["model"]          = DJIBattery::modelName(info.model);
        d["sealed"]         = info.sealed;
        d["hasPF"]          = info.hasPF;
        // Status registers
        d["opStatus"]       = String("0x") + String(info.operationStatus, HEX);
        d["safetyStatus"]   = String("0x") + String(info.safetyStatus, HEX);
        d["pfStatus"]       = String("0x") + String(info.pfStatus, HEX);
        d["mfgStatus"]      = String("0x") + String(info.manufacturingStatus, HEX);
        // Status decoders (added 2026-05-01 -- TEST_LOG note #2)
        d["opStatusDecoded"]     = info.opStatusKnown
            ? DJIBattery::decodeOperationStatus(info.operationStatus) : "?";
        d["pfStatusDecoded"]     = info.pfStatusKnown
            ? DJIBattery::decodePFStatus(info.pfStatus)              : "?";
        d["safetyStatusDecoded"] = info.safetyStatusKnown
            ? DJIBattery::decodeSafetyStatus(info.safetyStatus)      : "?";
        d["mfgStatusDecoded"]    = info.mfgStatusKnown
            ? DJIBattery::decodeManufacturingStatus(info.manufacturingStatus) : "?";
        // Read-validity flags -- distinguish "really 0" from "NACK sentinel"
        d["pfStatusKnown"]      = info.pfStatusKnown;
        d["safetyStatusKnown"]  = info.safetyStatusKnown;
        d["opStatusKnown"]      = info.opStatusKnown;
        d["mfgStatusKnown"]     = info.mfgStatusKnown;
        d["djiPF2Known"]        = info.djiPF2Known;
        d["sohKnown"]           = info.sohKnown;
        d["chargingRecKnown"]   = info.chargingRecKnown;
        d["dasMacKnown"]        = info.dasMacKnown;
        // Pack-type detection (TEST_LOG notes #9, #10, #20, #21)
        const char *fwVarStr = "unknown";
        switch (info.fwVariant) {
            case BatteryInfo::FW_VARIANT_PTL_OLD:     fwVarStr = "PTL-OLD (2021/2025 batch)"; break;
            case BatteryInfo::FW_VARIANT_PTL_NEW:     fwVarStr = "PTL-NEW (2024 batch)"; break;
            case BatteryInfo::FW_VARIANT_GENUINE_DJI: fwVarStr = "Genuine DJI (unconfirmed)"; break;
            default: break;
        }
        d["fwVariant"]          = fwVarStr;
        d["isLiHV"]             = info.isLiHV;
        d["isCustomCapacity"]   = info.isCustomCapacity;
        d["cellsSynthesised"]   = info.cellsSynthesised;
        // Pack identification (TEST_LOG notes #11, #12, #13)
        d["mfrDateDecoded"]     = info.mfrDateDecoded;
        d["fingerprint"]        = info.fingerprint;
        // Lockout state (TEST_LOG note #26)
        d["bmsLockoutDetected"] = info.bmsLockoutDetected;
        d["unsealCooldownMs"]   = DJIBattery::unsealCooldownRemainingMs();
        // DJI-specific
        if (info.deviceType == DEV_DJI_BATTERY) {
            // Always include djiSerial -- empty string if pack didn't return one
            d["djiSerial"]      = info.djiSerial;
            d["djiPF2"]         = String("0x") + String(info.djiPF2, HEX);
        }
        // Long-name aliases (TEST_LOG note #1) -- legacy clients
        d["manufacturerName"]   = info.manufacturerName;
        d["firmwareVersion"]    = info.firmwareVersion;
        d["hardwareVersion"]    = info.hardwareVersion;
        d["stateOfCharge"]      = info.stateOfCharge;
        d["fullCapacity_mAh"]   = info.fullCapacity_mAh;
        d["designCapacity_mAh"] = info.designCapacity_mAh;
        d["remainCapacity_mAh"] = info.remainCapacity_mAh;
        d["operationStatus"]    = String("0x") + String(info.operationStatus, HEX);
        d["manufacturingStatus"]= String("0x") + String(info.manufacturingStatus, HEX);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // ===== New service endpoints (added 2026-05-01) =====

    // Force-acquire Port B as I2C, releasing whoever holds it. Use this
    // before /api/batt/snapshot if Port B might be in UART/PWM/GPIO mode.
    s_server->on("/api/batt/acquire", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = DJIBattery::forceAcquirePortB();
        JsonDocument d;
        d["ok"] = ok;
        d["connected"] = DJIBattery::isConnected();
        String out; serializeJson(d, out);
        req->send(ok ? 200 : 500, "application/json", out);
    });

    // Try every key in our internal catalog. Honors the rate-limit and
    // returns lockout state. On success, reports which key worked.
    s_server->on("/api/batt/mavic3/try_keys", HTTP_POST, [](AsyncWebServerRequest *req) {
        DJIBattery::KeyTrialResult r = DJIBattery::tryAllKnownKeys();
        JsonDocument d;
        d["ok"]         = r.ok;
        d["attempts"]   = r.attempts;
        d["lockedOut"]  = r.lockedOut;
        d["cooldownMs"] = DJIBattery::unsealCooldownRemainingMs();
        if (r.ok) {
            d["w1"]         = String("0x") + String(r.w1, HEX);
            d["w2"]         = String("0x") + String(r.w2, HEX);
            d["description"]= r.description;
        }
        // Read SEC bits to confirm transition
        uint32_t op = DJIBattery::readOperationStatus();
        uint8_t sec = (op >> 8) & 0x03;
        d["operationStatus"] = String("0x") + String(op, HEX);
        d["sec"]    = sec;
        d["state"]  = sec == 0x00 ? "FullAccess" : sec == 0x02 ? "Unsealed" : "Sealed";
        String out; serializeJson(d, out);
        req->send(r.ok ? 200 : 500, "application/json", out);
    });

    // HMAC-SHA1 challenge-response unseal. Caller provides 32-byte key as
    // ?key=64-hex-chars. The implementation requests a 20-byte challenge from
    // the BMS, computes HMAC-SHA1(key, challenge), writes the digest, and
    // checks SEC bits.
    s_server->on("/api/batt/mavic3/unseal_hmac", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("key")) {
            req->send(400, "text/plain", "need ?key=<64-hex-char-32-byte-key>");
            return;
        }
        String hex = req->getParam("key")->value();
        if (hex.length() != 64) {
            req->send(400, "text/plain", "key must be exactly 64 hex chars (32 bytes)");
            return;
        }
        uint8_t key[32];
        for (int i = 0; i < 32; ++i) {
            char buf[3] = { hex.charAt(i*2), hex.charAt(i*2+1), 0 };
            key[i] = (uint8_t)strtoul(buf, nullptr, 16);
        }
        uint8_t challenge[20] = {0};
        UnsealResult r = DJIBattery::unsealHmac(key, challenge);
        DJIBattery::recordUnsealAttempt(r == UNSEAL_OK);
        JsonDocument d;
        d["result"] = r == UNSEAL_OK ? "OK" : r == UNSEAL_REJECTED_SEALED ? "still sealed" :
                      r == UNSEAL_NO_RESPONSE ? "no i2c" : "unsupported";
        // Show the challenge so user can compute HMAC offline if needed
        String chHex; for (int i = 0; i < 20; ++i) {
            char b[3]; snprintf(b, 3, "%02X", challenge[i]); chHex += b;
        }
        d["challenge"] = chHex;
        uint32_t op = DJIBattery::readOperationStatus();
        d["operationStatus"] = String("0x") + String(op, HEX);
        d["sec"] = (op >> 8) & 0x03;
        String out; serializeJson(d, out);
        req->send(r == UNSEAL_OK ? 200 : 500, "application/json", out);
    });

    // ===== Data Flash editor (must be before /api/batt catch-all) =====

    s_server->on("/api/batt/df/map", HTTP_GET, [](AsyncWebServerRequest *req) {
        static const char *typeNames[] = {"I1","I2","U1","U2","U4","H1"};
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        for (int i = 0; i < DF_MAP_LEN; i++) {
            const auto &e = DF_MAP[i];
            if (i) r->print(",");
            r->printf("{\"addr\":\"0x%04X\",\"type\":\"%s\",\"size\":%d,"
                      "\"min\":%ld,\"max\":%ld,\"def\":%ld,"
                      "\"cat\":\"%s\",\"sub\":\"%s\",\"field\":\"%s\",\"unit\":\"%s\"}",
                      e.addr, typeNames[e.type], dfTypeSize(e.type),
                      e.minVal, e.maxVal, e.defVal,
                      e.category, e.subcat, e.field, e.unit);
        }
        r->print("]");
        req->send(r);
    });

    s_server->on("/api/batt/df/read", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("addr")) { req->send(400, "text/plain", "need addr=0xNNNN"); return; }
        uint16_t addr = (uint16_t)strtoul(req->getParam("addr")->value().c_str(), nullptr, 0);
        uint8_t buf[8] = {0};
        int n = SMBus::macBlockRead(0x0B, addr, buf, 8);
        JsonDocument d;
        d["addr"] = String("0x") + String(addr, HEX);
        d["len"] = n;
        if (n > 0) {
            String hex; for (int i = 0; i < n; i++) { char h[4]; snprintf(h,4,"%02X",buf[i]); hex += h; }
            d["hex"] = hex;
            if (n >= 1) d["u8"]  = buf[0];
            if (n >= 1) d["i8"]  = (int8_t)buf[0];
            if (n >= 2) d["u16"] = (uint16_t)(buf[0] | (buf[1] << 8));
            if (n >= 2) d["i16"] = (int16_t)(buf[0] | (buf[1] << 8));
            if (n >= 4) d["u32"] = (uint32_t)(buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24));
            d["ok"] = true;
        } else {
            d["ok"] = false;
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/batt/df/write", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("addr", true) || !req->hasParam("value", true) || !req->hasParam("size", true)) {
            req->send(400, "text/plain", "need addr, value, size (1/2/4)");
            return;
        }
        uint16_t addr = (uint16_t)strtoul(req->getParam("addr", true)->value().c_str(), nullptr, 0);
        int32_t value = (int32_t)strtol(req->getParam("value", true)->value().c_str(), nullptr, 0);
        uint8_t size = (uint8_t)req->getParam("size", true)->value().toInt();
        if (size < 1 || size > 4) { req->send(400, "text/plain", "size must be 1,2,4"); return; }
        uint8_t payload[6] = {(uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8)};
        for (int i = 0; i < size; i++) payload[2 + i] = (value >> (i * 8)) & 0xFF;
        bool ok = SMBus::writeBlock(0x0B, 0x44, payload, 2 + size);
        JsonDocument d;
        d["addr"] = String("0x") + String(addr, HEX);
        d["value"] = value;
        d["size"] = size;
        d["ok"] = ok;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/batt/df/readall", HTTP_GET, [](AsyncWebServerRequest *req) {
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        for (int i = 0; i < DF_MAP_LEN; i++) {
            const auto &e = DF_MAP[i];
            uint8_t buf[8] = {0};
            int n = SMBus::macBlockRead(0x0B, e.addr, buf, dfTypeSize(e.type));
            if (i) r->print(",");
            r->printf("{\"addr\":\"0x%04X\",\"ok\":%s", e.addr, (n > 0) ? "true" : "false");
            if (n > 0) {
                int32_t val = 0;
                uint8_t sz = dfTypeSize(e.type);
                for (int j = 0; j < sz && j < n; j++) val |= ((int32_t)buf[j] << (j * 8));
                if (e.type == DF_I1 && (val & 0x80)) val |= 0xFFFFFF00;
                if (e.type == DF_I2 && (val & 0x8000)) val |= 0xFFFF0000;
                r->printf(",\"val\":%ld", val);
                if (val != e.defVal) r->print(",\"diff\":true");
            }
            r->print("}");
        }
        r->print("]");
        req->send(r);
    });

    s_server->on("/api/batt/scan/sbs", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint8_t from = 0x00, to = 0xFF;
        if (req->hasParam("from")) from = (uint8_t)strtoul(req->getParam("from")->value().c_str(), nullptr, 0);
        if (req->hasParam("to"))   to   = (uint8_t)strtoul(req->getParam("to")->value().c_str(), nullptr, 0);
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        bool first = true;
        for (int reg = from; reg <= to; reg++) {
            uint16_t w = SMBus::readWord(0x0B, (uint8_t)reg);
            uint8_t blk[16] = {0};
            int bl = SMBus::readBlock(0x0B, (uint8_t)reg, blk, 16);
            bool hasW = (w != 0xFFFF);
            bool hasB = (bl > 0);
            if (!hasW && !hasB) continue;
            if (!first) r->print(",");
            first = false;
            r->printf("{\"reg\":\"0x%02X\"", reg);
            if (hasW) r->printf(",\"word\":\"0x%04X\",\"dec\":%u", w, (unsigned)w);
            if (hasB) {
                r->printf(",\"blen\":%d,\"bhex\":\"", bl);
                for (int j = 0; j < bl && j < 16; j++) r->printf("%02X", blk[j]);
                r->print("\",\"bascii\":\"");
                for (int j = 0; j < bl && j < 16; j++) {
                    char c = blk[j];
                    r->printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
                }
                r->print("\"");
            }
            r->print("}");
        }
        r->print("]");
        req->send(r);
    });

    // Batch scan of MAC subcommand space via ManufacturerBlockAccess.
    // Reports only entries that return non-zero data (filters out boring "not supported").
    s_server->on("/api/batt/scan/mac", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint16_t from = 0x0000, to = 0x00FF;
        if (req->hasParam("from")) from = (uint16_t)strtoul(req->getParam("from")->value().c_str(), nullptr, 0);
        if (req->hasParam("to"))   to   = (uint16_t)strtoul(req->getParam("to")->value().c_str(), nullptr, 0);
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        bool first = true;
        for (uint32_t sub = from; sub <= to; sub++) {
            uint8_t buf[16] = {0};
            int n = SMBus::macBlockRead(0x0B, (uint16_t)sub, buf, 16);
            // Skip empty / non-responsive / all-zero responses
            if (n <= 0) continue;
            bool allZero = true;
            for (int j = 0; j < n; j++) if (buf[j] != 0) { allZero = false; break; }
            if (allZero) continue;
            if (!first) r->print(",");
            first = false;
            r->printf("{\"sub\":\"0x%04X\",\"len\":%d,\"hex\":\"", sub, n);
            for (int j = 0; j < n && j < 16; j++) r->printf("%02X", buf[j]);
            r->print("\",\"ascii\":\"");
            for (int j = 0; j < n && j < 16; j++) {
                char c = buf[j];
                r->printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            r->print("\"}");
        }
        r->print("]");
        req->send(r);
    });

    // Write-verify: try writing value, then read back. Tells us if seal is real or cosmetic.
    // POST addr=0xNN&type=word|block&value=... verifies if write actually changed register.
    s_server->on("/api/batt/scan/wv", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("reg", true) || !req->hasParam("value", true)) {
            req->send(400, "text/plain", "need reg, value"); return;
        }
        uint8_t reg = (uint8_t)strtoul(req->getParam("reg", true)->value().c_str(), nullptr, 0);
        uint16_t val = (uint16_t)strtoul(req->getParam("value", true)->value().c_str(), nullptr, 0);
        uint16_t before = SMBus::readWord(0x0B, reg);
        bool wrote = SMBus::writeWord(0x0B, reg, val);
        delay(50);
        uint16_t after = SMBus::readWord(0x0B, reg);
        JsonDocument d;
        d["reg"] = String("0x") + String(reg, HEX);
        d["before"] = String("0x") + String(before, HEX);
        d["wrote"] = wrote;
        d["target"] = String("0x") + String(val, HEX);
        d["after"] = String("0x") + String(after, HEX);
        d["persisted"] = (after == val);
        d["seal_bypassed"] = (after == val && val != before);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // ====================================================================
    // CLONE-RESEARCH ENDPOINTS (gated behind RESEARCH_MODE)
    //
    // 22 routes for SMBus clone analysis: register harvest, MAC brute,
    // PEC fuzzing, timing attack, async catch, etc. Only useful with a
    // physical clone battery on the bench. Strip from production builds
    // to (a) shrink the binary by ~40 KB and (b) remove the WiFi-creds
    // leak surface flagged by the audit (a clone dump can include AP
    // creds in DF blocks). To enable: add `-D RESEARCH_MODE` to
    // platformio.ini build_flags.
    // ====================================================================
#ifdef RESEARCH_MODE

    s_server->on("/api/batt/clone/harvest", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint8_t reg = req->hasParam("reg") ?
            (uint8_t)strtoul(req->getParam("reg")->value().c_str(), nullptr, 0) : 0xEE;
        uint16_t writeVal = req->hasParam("writeVal") ?
            (uint16_t)strtoul(req->getParam("writeVal")->value().c_str(), nullptr, 0) : 0x0000;
        int count = req->hasParam("count") ? req->getParam("count")->value().toInt() : 100;
        int readLen = req->hasParam("readLen") ? req->getParam("readLen")->value().toInt() : 16;
        if (count < 1) count = 1;
        if (count > 2000) count = 2000;  // cap to avoid WDT + heap pressure
        if (readLen < 1) readLen = 1;
        if (readLen > 32) readLen = 32;

        bool incrementWrite = req->hasParam("inc") &&
            req->getParam("inc")->value() != "0" &&
            req->getParam("inc")->value() != "false";

        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("[");
        uint8_t buf[32];
        uint16_t v = writeVal;
        for (int i = 0; i < count; i++) {
            SMBus::writeWord(0x0B, reg, v);
            delay(5);
            int n = SMBus::readBlock(0x0B, reg, buf, readLen);
            if (i) r->print(",");
            r->printf("{\"w\":\"0x%04X\",\"n\":%d,\"r\":\"", v, n);
            for (int j = 0; j < n && j < readLen; j++) r->printf("%02X", buf[j]);
            r->print("\"}");
            if (incrementWrite) v++;
        }
        r->print("]");
        req->send(r);
    });

    s_server->on("/api/batt/clone/wblock", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("reg", true) || !req->hasParam("data", true)) {
            req->send(400, "text/plain", "need reg & data (hex)"); return;
        }
        uint8_t reg = (uint8_t)strtoul(req->getParam("reg", true)->value().c_str(), nullptr, 0);
        String hex = req->getParam("data", true)->value();
        hex.replace(" ", "");
        hex.replace(":", "");
        if (hex.length() % 2 != 0 || hex.length() > 64) {
            req->send(400, "text/plain", "hex must be even-length, max 32 bytes"); return;
        }
        uint8_t buf[32]; int n = 0;
        for (size_t i = 0; i < hex.length() && n < 32; i += 2) {
            char pair[3] = {hex[i], hex[i+1], 0};
            buf[n++] = (uint8_t)strtol(pair, nullptr, 16);
        }
        bool raw = req->hasParam("raw", true);
        bool ok;
        if (raw) {
            // Raw: [reg, data...] without length prefix
            if (!SMBus::busLock(200)) { req->send(500, "text/plain", "bus busy"); return; }
            Wire1.beginTransmission((uint8_t)0x0B);
            Wire1.write(reg);
            Wire1.write(buf, n);
            ok = Wire1.endTransmission() == 0;
            SMBus::busUnlock();
        } else {
            // Standard block write: [reg, len, data...]
            ok = SMBus::writeBlock(0x0B, reg, buf, n);
        }
        JsonDocument d;
        d["reg"] = String("0x") + String(reg, HEX);
        d["len"] = n;
        d["raw"] = raw;
        d["ok"] = ok;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/batt/clone/wpec", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("reg", true)) { req->send(400, "text/plain", "need reg"); return; }
        uint8_t reg = (uint8_t)strtoul(req->getParam("reg", true)->value().c_str(), nullptr, 0);
        String type = req->hasParam("type", true) ? req->getParam("type", true)->value() : "word";
        bool ok = false;
        JsonDocument d;
        if (type == "word") {
            uint16_t v = (uint16_t)strtoul(req->getParam("value", true)->value().c_str(), nullptr, 0);
            ok = SMBus::writeWordPEC(0x0B, reg, v);
            d["value"] = String("0x") + String(v, HEX);
        } else if (type == "block") {
            String hex = req->getParam("data", true)->value();
            hex.replace(" ", "");
            uint8_t buf[32]; int n = 0;
            for (size_t i = 0; i < hex.length() && n < 32; i += 2) {
                char p[3] = {hex[i], hex[i+1], 0};
                buf[n++] = (uint8_t)strtol(p, nullptr, 16);
            }
            ok = SMBus::writeBlockPEC(0x0B, reg, buf, n);
            d["len"] = n;
        }
        d["ok"] = ok;
        d["reg"] = String("0x") + String(reg, HEX);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Retry unseal attempt with PEC enabled (strict SMBus) — clone may require.
    //   GET /api/batt/clone/unseal_pec?w1=0x7EE0&w2=0xCCDF
    s_server->on("/api/batt/clone/unseal_pec", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("w1") || !req->hasParam("w2")) {
            req->send(400, "text/plain", "need w1,w2"); return;
        }
        uint16_t w1 = (uint16_t)strtoul(req->getParam("w1")->value().c_str(), nullptr, 0);
        uint16_t w2 = (uint16_t)strtoul(req->getParam("w2")->value().c_str(), nullptr, 0);
        // Attempt 1: write to MAC 0x00 as two separate PEC-protected word writes
        bool ok1 = SMBus::writeWordPEC(0x0B, 0x00, w1);
        delay(20);
        bool ok2 = SMBus::writeWordPEC(0x0B, 0x00, w2);
        delay(100);
        // Attempt 2: also write as block-form to 0x44 (BK-style)
        uint8_t key1[2] = {(uint8_t)(w1 & 0xFF), (uint8_t)(w1 >> 8)};
        uint8_t key2[2] = {(uint8_t)(w2 & 0xFF), (uint8_t)(w2 >> 8)};
        bool okB1 = SMBus::writeBlockPEC(0x0B, 0x44, key1, 2);
        delay(20);
        bool okB2 = SMBus::writeBlockPEC(0x0B, 0x44, key2, 2);
        delay(200);
        uint32_t op = DJIBattery::readOperationStatus();
        uint8_t sec = (op >> 8) & 0x03;
        JsonDocument d;
        d["w1"] = String("0x") + String(w1, HEX);
        d["w2"] = String("0x") + String(w2, HEX);
        d["mac_word_pec"] = ok1 && ok2;
        d["mac_block_pec"] = okB1 && okB2;
        d["opStatus"] = String("0x") + String(op, HEX);
        d["sealed"] = (sec == 3);
        d["sec"] = sec;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Background MAC brute: starts a long-running scan task and returns immediately.
    // Use /api/batt/clone/brute_status to poll progress + hits so far.
    static struct {
        volatile bool     running;
        volatile uint32_t pos;
        uint32_t          from, to;
        int               stable;
        uint8_t           probes[16];
        int               nProbes;
        String            hits;       // accumulated JSON-encoded hits
        int               hitCount;
        int               flappyCount;
    } s_brute = {false, 0, 0, 0, 3, {0}, 0, "", 0, 0};

    auto bruteSnapshotReg = [](uint8_t reg, int stable, uint8_t *out) -> int {
        uint8_t buf[8][16]; int lens[8];
        int samples = stable + 2;
        if (samples > 8) samples = 8;
        for (int s = 0; s < samples; s++) {
            lens[s] = SMBus::readBlock(0x0B, reg, buf[s], 16);
            delay(1);
        }
        int best = 0, bestCount = 0;
        for (int i = 0; i < samples; i++) {
            int c = 0;
            for (int j = 0; j < samples; j++) {
                if (lens[i] == lens[j] && memcmp(buf[i], buf[j], lens[i]) == 0) c++;
            }
            if (c > bestCount) { best = i; bestCount = c; }
        }
        if (lens[best] > 0) memcpy(out, buf[best], lens[best]);
        return (bestCount >= (samples + 1) / 2) ? lens[best] : -1;
    };

    s_server->on("/api/batt/clone/brute_start", HTTP_POST, [bruteSnapshotReg](AsyncWebServerRequest *req) {
        if (s_brute.running) { req->send(409, "text/plain", "already running"); return; }
        s_brute.from = req->hasParam("from", true) ? strtoul(req->getParam("from", true)->value().c_str(), nullptr, 0) : 0;
        s_brute.to   = req->hasParam("to", true)   ? strtoul(req->getParam("to", true)->value().c_str(), nullptr, 0)   : 0xFFFF;
        s_brute.stable = req->hasParam("stable", true) ? req->getParam("stable", true)->value().toInt() : 3;
        String probeStr = req->hasParam("probe", true) ? req->getParam("probe", true)->value() : "0xEE,0xF0,0xFF";
        s_brute.nProbes = 0;
        int pos = 0;
        while (pos < (int)probeStr.length() && s_brute.nProbes < 16) {
            s_brute.probes[s_brute.nProbes++] = (uint8_t)strtoul(probeStr.c_str() + pos, nullptr, 0);
            int c = probeStr.indexOf(',', pos);
            if (c < 0) break; pos = c + 1;
        }
        s_brute.hits = "";
        s_brute.hitCount = 0;
        s_brute.flappyCount = 0;
        s_brute.pos = s_brute.from;
        s_brute.running = true;

        xTaskCreate([](void *argSnapRegPtr) {
            auto snapRegPtr = (int (*)(uint8_t, int, uint8_t *))argSnapRegPtr;
            uint8_t baseline[16][16]; int baselineLen[16];
            for (int i = 0; i < s_brute.nProbes; i++) {
                baselineLen[i] = snapRegPtr(s_brute.probes[i], s_brute.stable, baseline[i]);
            }
            uint32_t last = s_brute.from;
            for (uint32_t v = s_brute.from; v <= s_brute.to && s_brute.running; v++) {
                SMBus::writeWord(0x0B, 0x00, (uint16_t)v);
                vTaskDelay(pdMS_TO_TICKS(3));
                for (int i = 0; i < s_brute.nProbes; i++) {
                    uint8_t cur[16];
                    int n = snapRegPtr(s_brute.probes[i], s_brute.stable, cur);
                    if (n < 0) { s_brute.flappyCount++; continue; }
                    if (n != baselineLen[i] || memcmp(cur, baseline[i], n) != 0) {
                        char entry[128];
                        size_t existingLen;
                        { CloneLogGuard g; existingLen = s_brute.hits.length(); }
                        int ofs = snprintf(entry, sizeof(entry), "%s{\"mac\":\"0x%04X\",\"reg\":\"0x%02X\",\"before\":\"",
                                           existingLen ? "," : "", v, s_brute.probes[i]);
                        for (int j = 0; j < baselineLen[i] && j < 16 && ofs < (int)sizeof(entry)-3; j++) {
                            ofs += snprintf(entry+ofs, sizeof(entry)-ofs, "%02X", baseline[i][j]);
                        }
                        ofs += snprintf(entry+ofs, sizeof(entry)-ofs, "\",\"after\":\"");
                        for (int j = 0; j < n && j < 16 && ofs < (int)sizeof(entry)-3; j++) {
                            ofs += snprintf(entry+ofs, sizeof(entry)-ofs, "%02X", cur[j]);
                        }
                        snprintf(entry+ofs, sizeof(entry)-ofs, "\"}");
                        { CloneLogGuard g; s_brute.hits += entry; }
                        s_brute.hitCount++;
                        memcpy(baseline[i], cur, n);
                        baselineLen[i] = n;
                    }
                }
                s_brute.pos = v;
                (void)last;
            }
            s_brute.running = false;
            vTaskDelete(nullptr);
        }, "macbrute", 8192, (void*)(int (*)(uint8_t, int, uint8_t*))bruteSnapshotReg, 1, nullptr);

        req->send(202, "text/plain", "started — poll /api/batt/clone/brute_status");
    });

    s_server->on("/api/batt/clone/brute_status", HTTP_GET, [](AsyncWebServerRequest *req) {
        String hitsCopy;
        { CloneLogGuard g; hitsCopy = s_brute.hits; }
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"running\":%s,\"pos\":\"0x%04X\",\"from\":\"0x%04X\",\"to\":\"0x%04X\",\"hits\":%d,\"flapping\":%d,\"stable\":%d,\"entries\":[%s]}",
                  s_brute.running ? "true" : "false",
                  (unsigned)s_brute.pos, (unsigned)s_brute.from, (unsigned)s_brute.to,
                  s_brute.hitCount, s_brute.flappyCount, s_brute.stable,
                  hitsCopy.c_str());
        req->send(r);
    });

    s_server->on("/api/batt/clone/brute_stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        s_brute.running = false;
        req->send(200, "text/plain", "stop requested");
    });

    static struct {
        volatile bool     running;
        volatile uint32_t pos;
        uint32_t          from, to;
        String            baselineHex;
        String            hits;  // JSON entries
        int               hitCount;
    } s_macResp = {false, 0, 0, 0, "", "", 0};

    s_server->on("/api/batt/clone/macresp_start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (s_macResp.running) { req->send(409, "text/plain", "already running"); return; }
        s_macResp.from = req->hasParam("from", true) ? strtoul(req->getParam("from", true)->value().c_str(), nullptr, 0) : 0;
        s_macResp.to   = req->hasParam("to", true)   ? strtoul(req->getParam("to", true)->value().c_str(), nullptr, 0)   : 0xFFFF;
        s_macResp.hits = "";
        s_macResp.hitCount = 0;
        s_macResp.pos = s_macResp.from;
        s_macResp.running = true;

        xTaskCreate([](void *) {
            // Capture baseline: write 0x0000, read back
            uint8_t bBuf[33] = {0};
            SMBus::writeWord(0x0B, 0x00, 0x0000);
            vTaskDelay(pdMS_TO_TICKS(5));
            int bLen = SMBus::readBlock(0x0B, 0x00, bBuf, 32);
            char hex[128]; hex[0] = 0;
            if (bLen > 0) {
                for (int i = 0; i < bLen && i < 16; i++) sprintf(hex + strlen(hex), "%02X", bBuf[i]);
            }
            s_macResp.baselineHex = String(hex);

            uint8_t cur[33];
            for (uint32_t v = s_macResp.from; v <= s_macResp.to && s_macResp.running; v++) {
                SMBus::writeWord(0x0B, 0x00, (uint16_t)v);
                vTaskDelay(pdMS_TO_TICKS(3));
                int n = SMBus::readBlock(0x0B, 0x00, cur, 32);

                // PTL clone pattern: len = cmd_lo, byte[0] = cmd_hi echo,
                // byte[1] = chaotic state, bytes 2+ = 0xFF filler.
                // Only treat as INTERESTING hit if bytes 2+ contain non-0xFF data.
                // (Means chip actually returned something meaningful for this cmd.)
                bool isHit = false;
                if (n >= 3) {
                    for (int j = 2; j < n && j < 16; j++) {
                        if (cur[j] != 0xFF) { isHit = true; break; }
                    }
                }

                if (isHit && s_macResp.hitCount < 500) {
                    char entry[128];
                    size_t existingLen;
                    { CloneLogGuard g; existingLen = s_macResp.hits.length(); }
                    int ofs = snprintf(entry, sizeof(entry), "%s{\"mac\":\"0x%04X\",\"len\":%d,\"hex\":\"",
                                       existingLen ? "," : "", (unsigned)v, n);
                    for (int j = 0; j < n && j < 16 && ofs < (int)sizeof(entry)-3; j++) {
                        ofs += snprintf(entry+ofs, sizeof(entry)-ofs, "%02X", cur[j]);
                    }
                    snprintf(entry+ofs, sizeof(entry)-ofs, "\"}");
                    { CloneLogGuard g; s_macResp.hits += entry; }
                    s_macResp.hitCount++;
                }
                s_macResp.pos = v;
                if ((v & 0x1F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
            }
            s_macResp.running = false;
            vTaskDelete(nullptr);
        }, "macresp", 8192, nullptr, 1, nullptr);

        req->send(202, "text/plain", "started — poll /api/batt/clone/macresp_status");
    });

    s_server->on("/api/batt/clone/macresp_status", HTTP_GET, [](AsyncWebServerRequest *req) {
        String hitsCopy, baselineCopy;
        { CloneLogGuard g;
          hitsCopy     = s_macResp.hits;
          baselineCopy = s_macResp.baselineHex; }
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"running\":%s,\"pos\":\"0x%04X\",\"from\":\"0x%04X\",\"to\":\"0x%04X\",\"hits\":%d,\"baseline\":\"%s\",\"entries\":[%s]}",
                  s_macResp.running ? "true" : "false",
                  (unsigned)s_macResp.pos, (unsigned)s_macResp.from, (unsigned)s_macResp.to,
                  s_macResp.hitCount, baselineCopy.c_str(), hitsCopy.c_str());
        req->send(r);
    });

    s_server->on("/api/batt/clone/macresp_stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        s_macResp.running = false;
        req->send(200, "text/plain", "stop requested");
    });

    s_server->on("/api/batt/clone/trans_brute", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint16_t macA = req->hasParam("macA") ? (uint16_t)strtoul(req->getParam("macA")->value().c_str(), nullptr, 0) : 0x0000;
        uint16_t from = req->hasParam("from") ? (uint16_t)strtoul(req->getParam("from")->value().c_str(), nullptr, 0) : 0x0100;
        uint16_t to   = req->hasParam("to")   ? (uint16_t)strtoul(req->getParam("to")->value().c_str(), nullptr, 0)   : 0x01FF;
        if ((uint32_t)to - from > 2048) to = from + 2048;
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"macA\":\"0x%04X\",\"from\":\"0x%04X\",\"to\":\"0x%04X\",\"hits\":[", macA, from, to);
        bool first = true;
        int hitCount = 0;
        uint8_t buf[32];
        uint32_t lastYield = millis();
        for (uint32_t v = from; v <= to; v++) {
            // Set baseline with macA
            SMBus::writeWord(0x0B, 0x00, macA);
            delay(1);
            // Now write v and immediately read
            SMBus::writeWord(0x0B, 0x00, (uint16_t)v);
            int n = SMBus::readBlock(0x0B, 0x00, buf, 32);
            if (n >= 3) {
                bool has = false;
                for (int j = 2; j < n && j < 16; j++) if (buf[j] != 0xFF) { has = true; break; }
                if (has) {
                    if (!first) r->print(",");
                    first = false;
                    r->printf("{\"b\":\"0x%04X\",\"len\":%d,\"hex\":\"", (unsigned)v, n);
                    for (int j = 0; j < n && j < 32; j++) r->printf("%02X", buf[j]);
                    r->print("\"}");
                    hitCount++;
                }
            }
            if (millis() - lastYield > 50) { vTaskDelay(pdMS_TO_TICKS(3)); lastYield = millis(); }
        }
        r->printf("],\"count\":%d}", hitCount);
        req->send(r);
    });

    s_server->on("/api/batt/clone/broad_brute", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t aFrom = req->hasParam("macA_from") ? strtoul(req->getParam("macA_from")->value().c_str(), nullptr, 0) : 0x0000;
        uint32_t aTo   = req->hasParam("macA_to")   ? strtoul(req->getParam("macA_to")->value().c_str(), nullptr, 0)   : 0x0100;
        uint32_t aStep = req->hasParam("macA_step") ? strtoul(req->getParam("macA_step")->value().c_str(), nullptr, 0) : 0x20;
        uint32_t bTo   = req->hasParam("macB_to")   ? strtoul(req->getParam("macB_to")->value().c_str(), nullptr, 0)   : 0x01FF;

        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"unique_packets\":[");
        bool first = true;
        int totalPkts = 0;
        String seenSigs;  // signatures of already-reported packets
        uint8_t buf[32];
        uint32_t lastYield = millis();

        for (uint32_t a = aFrom; a <= aTo; a += aStep) {
            for (uint32_t b = 0x0000; b <= bTo; b++) {
                SMBus::writeWord(0x0B, 0x00, (uint16_t)a);
                delay(1);
                SMBus::writeWord(0x0B, 0x00, (uint16_t)b);
                int n = SMBus::readBlock(0x0B, 0x00, buf, 32);
                if (n >= 18) {
                    // Find 81 F0 start
                    int start = -1;
                    for (int j = 0; j <= n - 14; j++) {
                        if (buf[j] == 0x81 && buf[j+1] == 0xF0) { start = j; break; }
                    }
                    if (start >= 0) {
                        // Signature: 81F0 + counter + type (4 bytes). Skip duplicates.
                        char sig[16];
                        snprintf(sig, sizeof(sig), "%02X%02X%02X%02X",
                                 buf[start], buf[start+1], buf[start+2], buf[start+3]);
                        if (seenSigs.indexOf(sig) < 0) {
                            seenSigs += sig; seenSigs += ",";
                            if (!first) r->print(",");
                            first = false;
                            r->printf("{\"macA\":\"0x%04X\",\"macB\":\"0x%04X\",\"hex\":\"", (unsigned)a, (unsigned)b);
                            for (int j = start; j < n; j++) r->printf("%02X", buf[j]);
                            r->print("\"}");
                            totalPkts++;
                            if (totalPkts >= 200) goto done;
                        }
                    }
                }
                if (millis() - lastYield > 50) { vTaskDelay(pdMS_TO_TICKS(2)); lastYield = millis(); }
            }
        }
        done:
        r->printf("],\"scanned_a\":%u,\"scanned_b\":%u,\"unique\":%d}",
                  (unsigned)((aTo - aFrom) / aStep + 1), (unsigned)bTo + 1, totalPkts);
        req->send(r);
    });

    s_server->on("/api/batt/clone/inject", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("reg", true) || !req->hasParam("frame", true)) {
            req->send(400, "text/plain", "need reg, frame"); return;
        }
        uint8_t reg = (uint8_t)strtoul(req->getParam("reg", true)->value().c_str(), nullptr, 0);
        String hex = req->getParam("frame", true)->value();
        hex.replace(" ", "");
        uint8_t buf[32]; int n = 0;
        for (size_t i = 0; i < hex.length() && n < 32; i += 2) {
            char p[3] = {hex[i], hex[i+1], 0};
            buf[n++] = (uint8_t)strtol(p, nullptr, 16);
        }
        bool w_ok = SMBus::writeBlock(0x0B, reg, buf, n);
        delay(5);
        uint8_t rbuf[32]; int rlen = SMBus::readBlock(0x0B, reg, rbuf, 32);
        uint8_t mac0_buf[32]; int mac0_len = SMBus::readBlock(0x0B, 0x00, mac0_buf, 32);

        JsonDocument d;
        d["reg"] = String("0x") + String(reg, HEX);
        d["injected_len"] = n;
        d["write_ok"] = w_ok;
        d["read_len"] = rlen;
        String rhex;
        for (int i = 0; i < rlen; i++) { char b[3]; snprintf(b,3,"%02X", rbuf[i]); rhex += b; }
        d["read_hex"] = rhex;
        String mhex;
        for (int i = 0; i < mac0_len; i++) { char b[3]; snprintf(b,3,"%02X", mac0_buf[i]); mhex += b; }
        d["mac0_len"] = mac0_len;
        d["mac0_hex"] = mhex;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    static struct {
        volatile bool running;
        uint32_t      intervalSec;
        String        entries;    // JSON-encoded log entries
        int           count;
        uint32_t      startMs;
    } s_logger = {false, 30, "", 0, 0};

    s_server->on("/api/batt/clone/logger_start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (s_logger.running) { req->send(409, "text/plain", "already running"); return; }
        s_logger.intervalSec = req->hasParam("pollSec", true) ?
            (uint32_t)req->getParam("pollSec", true)->value().toInt() : 30;
        if (s_logger.intervalSec < 5) s_logger.intervalSec = 5;
        s_logger.entries = "";
        s_logger.count = 0;
        s_logger.startMs = millis();
        s_logger.running = true;

        xTaskCreate([](void *) {
            uint8_t buf[32];
            while (s_logger.running) {
                // Dense transition sweep: known hot-spot range
                for (uint32_t b = 0x0080; b <= 0x01FF && s_logger.running; b++) {
                    SMBus::writeWord(0x0B, 0x00, 0x0000);
                    vTaskDelay(pdMS_TO_TICKS(1));
                    SMBus::writeWord(0x0B, 0x00, (uint16_t)b);
                    int n = SMBus::readBlock(0x0B, 0x00, buf, 32);
                    if (n >= 18) {
                        int start = -1;
                        for (int j = 0; j <= n - 14; j++) {
                            if (buf[j] == 0x81 && buf[j+1] == 0xF0) { start = j; break; }
                        }
                        if (start >= 0 && s_logger.count < 500) {
                            char entry[96];
                            size_t existingLen;
                            { CloneLogGuard g; existingLen = s_logger.entries.length(); }
                            int o = snprintf(entry, sizeof(entry), "%s{\"t\":%u,\"b\":\"0x%04X\",\"hex\":\"",
                                             existingLen ? "," : "",
                                             (unsigned)((millis() - s_logger.startMs) / 1000), (unsigned)b);
                            for (int j = start; j < n && j - start < 26 && o < (int)sizeof(entry)-3; j++) {
                                o += snprintf(entry+o, sizeof(entry)-o, "%02X", buf[j]);
                            }
                            snprintf(entry+o, sizeof(entry)-o, "\"}");
                            { CloneLogGuard g; s_logger.entries += entry; }
                            s_logger.count++;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
                // Wait intervalSec before next sweep
                for (uint32_t w = 0; w < s_logger.intervalSec && s_logger.running; w++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            vTaskDelete(nullptr);
        }, "pubLog", 6144, nullptr, 1, nullptr);

        req->send(202, "text/plain", "logger started");
    });

    s_server->on("/api/batt/clone/logger_dump", HTTP_GET, [](AsyncWebServerRequest *req) {
        String entriesCopy;
        { CloneLogGuard g; entriesCopy = s_logger.entries; }
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"running\":%s,\"count\":%d,\"uptime_s\":%u,\"interval_s\":%u,\"entries\":[%s]}",
                  s_logger.running ? "true" : "false",
                  s_logger.count,
                  (unsigned)((millis() - s_logger.startMs) / 1000),
                  (unsigned)s_logger.intervalSec,
                  entriesCopy.c_str());
        req->send(r);
    });

    s_server->on("/api/batt/clone/logger_stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        s_logger.running = false;
        req->send(200, "text/plain", "stop requested");
    });

    s_server->on("/api/batt/clone/dfdump", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t from = req->hasParam("from") ? strtoul(req->getParam("from")->value().c_str(), nullptr, 0) : 0x4000;
        uint32_t to   = req->hasParam("to")   ? strtoul(req->getParam("to")->value().c_str(), nullptr, 0)   : 0x40FF;
        if (to - from > 512) to = from + 512;  // cap 512 addrs per request

        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"entries\":[");
        bool first = true;
        int hits = 0;
        uint32_t lastYield = millis();
        uint8_t buf[32];
        for (uint32_t addr = from; addr <= to; addr += 4) {  // step 4 = u32 aligned
            int n = SMBus::macBlockRead(0x0B, (uint16_t)addr, buf, 32);
            if (n > 0) {
                // Ignore all-FF or all-zero responses (unused regions)
                bool hasData = false;
                for (int j = 0; j < n && j < 16; j++) {
                    if (buf[j] != 0xFF && buf[j] != 0x00) { hasData = true; break; }
                }
                if (hasData) {
                    if (!first) r->print(",");
                    first = false;
                    r->printf("{\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"", (unsigned)addr, n);
                    for (int j = 0; j < n && j < 32; j++) r->printf("%02X", buf[j]);
                    r->print("\"}");
                    hits++;
                }
            }
            if (millis() - lastYield > 50) { vTaskDelay(pdMS_TO_TICKS(3)); lastYield = millis(); }
        }
        r->printf("],\"hits\":%d,\"from\":\"0x%04X\",\"to\":\"0x%04X\"}", hits, (unsigned)from, (unsigned)to);
        req->send(r);
    });

    s_server->on("/api/batt/clone/sh366000_test", HTTP_GET, [](AsyncWebServerRequest *req) {
        // Known SH366000-family MAC commands (not in TI datasheet)
        static const uint16_t SH_CMDS[] = {
            0xE001,  // SH DeviceName / Vendor ID
            0xE000,  // SH DeviceType
            0x0F0F,  // SH boot enter attempt
            0x5B5B,  // SH unseal magic 1
            0xA5A5,  // SH unseal magic 2
            0xC9C9,  // SH unseal magic 3
            0x7C7C,  // SH factory test
            0x6969,  // SH ROM mode
            0x30B3,  // SH unseal alt (from CP2112_SH366000 tool)
            0x7FA5,  // SH FAS alt
            0xAB5A,  // SH backdoor
            0x5ABB,  // SH test mode
        };
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"results\":[");
        bool first = true;
        uint8_t buf[32];
        for (int i = 0; i < (int)(sizeof(SH_CMDS)/sizeof(SH_CMDS[0])); i++) {
            uint16_t cmd = SH_CMDS[i];
            int n = SMBus::macBlockRead(0x0B, cmd, buf, 32);
            // Also check seal state after this attempt
            uint32_t op = DJIBattery::readOperationStatus();
            uint8_t sec = (op >> 8) & 0x03;
            if (!first) r->print(",");
            first = false;
            r->printf("{\"cmd\":\"0x%04X\",\"len\":%d,\"hex\":\"", cmd, n);
            if (n > 0) for (int j = 0; j < n && j < 16; j++) r->printf("%02X", buf[j]);
            r->printf("\",\"sec\":%d,\"sealed\":%s}", sec, sec == 3 ? "true" : "false");
            delay(50);
        }
        r->print("]}");
        req->send(r);
    });

    s_server->on("/api/batt/clone/timing_attack", HTTP_POST, [](AsyncWebServerRequest *req) {
        int samples = req->hasParam("samples", true) ? req->getParam("samples", true)->value().toInt() : 50;
        if (samples > 200) samples = 200;
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"samples\":%d,\"keys\":[", samples);
        bool first = true;

        // Test keys: zeros, 0xFF, all-zero-with-one-bit-flipped, etc.
        // Goal: see if certain key families produce different timing
        struct TestKey { const char *name; uint8_t k[32]; };
        TestKey tk[] = {
            {"zeros", {0}},
            {"ff_all", {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}},
            {"first_byte_01", {0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
            {"last_byte_01", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01}},
            {"middle_byte", {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
        };
        int nkeys = sizeof(tk) / sizeof(tk[0]);

        for (int ki = 0; ki < nkeys; ki++) {
            // Collect `samples` measurements for this key
            uint32_t min_us = 0xFFFFFFFF, max_us = 0, sum_us = 0;
            int successes = 0;
            for (int s = 0; s < samples; s++) {
                // 1) Get challenge (write 0x0000 subcommand, read 32 bytes)
                uint8_t challenge[32];
                int cn = SMBus::macBlockRead(0x0B, 0x0000, challenge, 32);
                if (cn <= 0) continue;

                // 2) Compute fake HMAC response using this key (all zeros - placeholder)
                //    For timing measurement we just send a fixed payload
                uint8_t payload[22] = {0x00, 0x00};
                for (int i = 0; i < 20 && i < cn; i++) payload[2 + i] = tk[ki].k[i];

                // 3) Measure time: write block + verify
                uint32_t t0 = micros();
                SMBus::writeBlock(0x0B, 0x44, payload, 22);
                delay(10);  // give chip time to verify
                uint32_t op = DJIBattery::readOperationStatus();
                uint32_t dt = micros() - t0;

                if (dt < min_us) min_us = dt;
                if (dt > max_us) max_us = dt;
                sum_us += dt;
                successes++;

                if ((op >> 8) & 0x03) {} // still sealed — expected
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            if (!first) r->print(",");
            first = false;
            r->printf("{\"name\":\"%s\",\"samples\":%d,\"min_us\":%u,\"max_us\":%u,\"avg_us\":%u}",
                      tk[ki].name, successes, (unsigned)min_us, (unsigned)max_us,
                      successes > 0 ? (unsigned)(sum_us / successes) : 0);
        }
        r->print("]}");
        req->send(r);
    });

    s_server->on("/api/batt/clone/watch", HTTP_GET, [](AsyncWebServerRequest *req) {
        int count = req->hasParam("count") ? req->getParam("count")->value().toInt() : 500;
        int interval = req->hasParam("intervalMs") ? req->getParam("intervalMs")->value().toInt() : 50;
        if (count > 3000) count = 3000;
        if (interval < 10) interval = 10;

        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->printf("{\"count\":%d,\"intervalMs\":%d,\"events\":[", count, interval);
        bool first = true;
        int eventCount = 0;
        String seenSigs;
        uint8_t buf[32];
        uint32_t t0 = millis();
        for (int i = 0; i < count; i++) {
            int n = SMBus::readBlock(0x0B, 0x00, buf, 32);
            if (n >= 3) {
                bool hasData = false;
                for (int j = 2; j < n && j < 16; j++) if (buf[j] != 0xFF) { hasData = true; break; }
                if (hasData) {
                    char sig[16];
                    snprintf(sig, sizeof(sig), "%02X%02X%02X%02X",
                             buf[0], buf[1], buf[2], buf[3]);
                    if (seenSigs.indexOf(sig) < 0) {
                        seenSigs += sig; seenSigs += ",";
                        if (!first) r->print(",");
                        first = false;
                        r->printf("{\"t_ms\":%u,\"i\":%d,\"hex\":\"", (unsigned)(millis()-t0), i);
                        for (int j = 0; j < n; j++) r->printf("%02X", buf[j]);
                        r->print("\"}");
                        eventCount++;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(interval));
        }
        r->printf("],\"unique\":%d,\"duration_ms\":%u}", eventCount, (unsigned)(millis() - t0));
        req->send(r);
    });

    s_server->on("/api/batt/clone/async_catch", HTTP_GET, [](AsyncWebServerRequest *req) {
        int count = req->hasParam("count") ? req->getParam("count")->value().toInt() : 1000;
        if (count > 5000) count = 5000;
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"unique\":[");
        uint8_t buf[32];
        String seen;  // csv of first 8-byte hex of unique responses
        bool first = true;
        uint32_t lastYield = millis();
        for (int i = 0; i < count; i++) {
            int n = SMBus::readBlock(0x0B, 0x00, buf, 32);
            if (n >= 3) {
                bool hasData = false;
                for (int j = 2; j < n && j < 16; j++) if (buf[j] != 0xFF) { hasData = true; break; }
                if (hasData) {
                    char sig[33]; int sp = 0;
                    for (int j = 0; j < 8 && j < n; j++) sp += snprintf(sig+sp, sizeof(sig)-sp, "%02X", buf[j]);
                    if (seen.indexOf(sig) < 0) {
                        seen += sig; seen += ",";
                        if (!first) r->print(",");
                        first = false;
                        r->printf("{\"i\":%d,\"len\":%d,\"hex\":\"", i, n);
                        for (int j = 0; j < n && j < 32; j++) r->printf("%02X", buf[j]);
                        r->print("\"}");
                    }
                }
            }
            if (millis() - lastYield > 50) { vTaskDelay(pdMS_TO_TICKS(3)); lastYield = millis(); }
        }
        r->printf("],\"reads\":%d}", count);
        req->send(r);
    });

    s_server->on("/api/batt/clone/mac_brute", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t from = req->hasParam("from") ?
            strtoul(req->getParam("from")->value().c_str(), nullptr, 0) : 0x0000;
        uint32_t to   = req->hasParam("to") ?
            strtoul(req->getParam("to")->value().c_str(), nullptr, 0) : 0x00FF;
        if (to < from) { req->send(400, "text/plain", "bad range"); return; }
        if (to - from > 4096) to = from + 4096;  // cap per-request

        String probe_s = req->hasParam("probe") ? req->getParam("probe")->value() : "0xEE,0xF0,0xFF";
        uint8_t probes[16]; int n_probes = 0;
        int pos = 0;
        while (pos < (int)probe_s.length() && n_probes < 16) {
            probes[n_probes++] = (uint8_t)strtoul(probe_s.c_str() + pos, nullptr, 0);
            int c = probe_s.indexOf(',', pos);
            if (c < 0) break;
            pos = c + 1;
        }

        // Capture baseline snapshot of probe regs
        uint8_t baseline[16][16]; int baselineLen[16];
        for (int i = 0; i < n_probes; i++) {
            baselineLen[i] = SMBus::readBlock(0x0B, probes[i], baseline[i], 16);
        }

        // Noise-filtering: before declaring a probe-reg change a "hit", confirm
        // that N consecutive reads show the new stable value AND that N reads
        // of the baseline showed the old stable value. Prevents false-positive
        // from registers that naturally flap between two values.
        int stable = req->hasParam("stable") ? req->getParam("stable")->value().toInt() : 3;
        if (stable < 1) stable = 1;

        // Re-check baseline: majority-vote over N reads
        auto snapshotReg = [&](uint8_t reg, uint8_t *out) -> int {
            // Take stable+2 reads, pick the MODE (most common). If majority < stable, mark flapping.
            uint8_t buf[8][16]; int lens[8]; int votes[8] = {0};
            int samples = stable + 2;
            if (samples > 8) samples = 8;
            for (int s = 0; s < samples; s++) {
                lens[s] = SMBus::readBlock(0x0B, reg, buf[s], 16);
                delay(1);
            }
            // Find most common among the samples
            int best = 0, bestCount = 0;
            for (int i = 0; i < samples; i++) {
                int c = 0;
                for (int j = 0; j < samples; j++) {
                    if (lens[i] == lens[j] && memcmp(buf[i], buf[j], lens[i]) == 0) c++;
                }
                if (c > bestCount) { best = i; bestCount = c; }
            }
            if (lens[best] > 0) memcpy(out, buf[best], lens[best]);
            return (bestCount >= (samples + 1) / 2) ? lens[best] : -1;  // -1 = flapping
        };

        for (int i = 0; i < n_probes; i++) {
            baselineLen[i] = snapshotReg(probes[i], baseline[i]);
        }

        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"hits\":[");
        bool first = true;
        int hitCount = 0, flappyCount = 0;
        uint32_t lastYield = millis();
        for (uint32_t v = from; v <= to; v++) {
            SMBus::writeWord(0x0B, 0x00, (uint16_t)v);
            delay(3);
            for (int i = 0; i < n_probes; i++) {
                uint8_t cur[16];
                int n = snapshotReg(probes[i], cur);
                if (n < 0) { flappyCount++; continue; }  // register is currently flapping, skip
                if (n != baselineLen[i] || memcmp(cur, baseline[i], n) != 0) {
                    if (!first) r->print(",");
                    first = false;
                    r->printf("{\"mac\":\"0x%04X\",\"reg\":\"0x%02X\",\"before\":\"", v, probes[i]);
                    for (int j = 0; j < baselineLen[i] && j < 16; j++) r->printf("%02X", baseline[i][j]);
                    r->print("\",\"after\":\"");
                    for (int j = 0; j < n && j < 16; j++) r->printf("%02X", cur[j]);
                    r->print("\"}");
                    hitCount++;
                    memcpy(baseline[i], cur, n);
                    baselineLen[i] = n;
                }
            }
            if (millis() - lastYield > 50) {
                vTaskDelay(pdMS_TO_TICKS(5));
                lastYield = millis();
            }
        }
        r->printf("],\"from\":\"0x%04X\",\"to\":\"0x%04X\",\"probes\":%d,\"stable\":%d,\"hits\":%d,\"flapping_skipped\":%d}",
                  from, to, n_probes, stable, hitCount, flappyCount);
        req->send(r);
    });

#endif // RESEARCH_MODE — end clone-research block

    // Battery service actions
    s_server->on("/api/batt", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("action")) { req->send(400, "text/plain", "missing action"); return; }
        String action = req->getParam("action")->value();

        if (action == "unseal") {
            UnsealResult r = DJIBattery::unseal();
            const char* msg = r == UNSEAL_OK ? "Unsealed OK" :
                              r == UNSEAL_UNSUPPORTED_MODEL ? "No key for this model (Mavic 3/4?)" :
                              r == UNSEAL_REJECTED_SEALED ? "Rejected — still sealed" :
                              "No I2C response";
            req->send(200, "text/plain", msg);
        } else if (action == "clearpf") {
            bool ok = DJIBattery::clearPFProper();
            req->send(200, "text/plain", ok ? "PF cleared OK" : "PF clear failed");
        } else if (action == "seal") {
            req->send(200, "text/plain", DJIBattery::seal() ? "Sealed" : "Seal failed");
        } else if (action == "reset") {
            req->send(200, "text/plain", DJIBattery::softReset() ? "Reset sent" : "Reset failed");
        } else if (action == "fullservice") {
            UnsealResult r = DJIBattery::unseal();
            if (r != UNSEAL_OK) {
                const char* msg = r == UNSEAL_UNSUPPORTED_MODEL ? "Aborted: no key for this model" : "Aborted: unseal failed";
                req->send(200, "text/plain", msg);
                return;
            }
            bool pfOk = DJIBattery::clearPFProper();
            DJIBattery::seal();
            req->send(200, "text/plain", pfOk ? "Full service OK" : "Unseal OK, PF clear failed, sealed again");
        } else {
            req->send(400, "text/plain", "unknown action");
        }
    });

    s_server->on("/api/batt/unseal_hmac", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("key", true)) { req->send(400, "text/plain", "need key=<64 hex chars>"); return; }
        String hex = req->getParam("key", true)->value();
        hex.trim();
        if (hex.length() != 64) { req->send(400, "text/plain", "key must be 64 hex chars (32 bytes)"); return; }

        uint8_t key[32] = {0};
        for (int i = 0; i < 32; i++) {
            char pair[3] = { hex[i*2], hex[i*2+1], 0 };
            char *end = nullptr;
            long v = strtol(pair, &end, 16);
            if (end != pair + 2) { req->send(400, "text/plain", "bad hex at byte " + String(i)); return; }
            key[i] = (uint8_t)v;
        }

        uint8_t challenge[20] = {0};
        UnsealResult r = DJIBattery::unsealHmac(key, challenge);

        JsonDocument d;
        d["result"] = r == UNSEAL_OK ? "Unsealed OK" :
                      r == UNSEAL_REJECTED_SEALED ? "Rejected (still sealed)" :
                      "No I2C response";
        d["ok"] = (r == UNSEAL_OK);
        String ch;
        for (int i = 0; i < 20; i++) { char h[3]; snprintf(h,3,"%02X", challenge[i]); ch += h; }
        d["challenge"] = ch;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/smbus/log", HTTP_GET, [](AsyncWebServerRequest *req) {
        static const char *opNames[] = {"RW","RB","WW","WB","MC","MR","RD"};
        SMBus::LogEntry buf[64];
        int n = SMBus::logDump(buf, 64);
        AsyncResponseStream *r = req->beginResponseStream("text/plain");
        r->printf("seq=%lu logging=%s entries=%d\n", (unsigned long)SMBus::logSeq(),
                  SMBus::logEnabled() ? "on" : "off", n);
        for (int i = 0; i < n; i++) {
            auto &e = buf[i];
            r->printf("%08lu %s s=%02X r=%02X %s len=%d ",
                      e.ts, opNames[e.op], e.addr, e.reg,
                      e.ok ? "OK" : "ERR", e.len);
            if (e.ok && e.len > 0) {
                int show = e.len < 8 ? e.len : 8;
                for (int j = 0; j < show; j++) r->printf("%02X", e.data[j]);
            }
            r->print('\n');
        }
        req->send(r);
    });

    // SMBus log control (enable/disable)
    s_server->on("/api/smbus/log/toggle", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool now = !SMBus::logEnabled();
        SMBus::logEnable(now);
        req->send(200, "text/plain", now ? "logging ON" : "logging OFF");
    });

}  // registerRoutesBattery
}  // namespace RoutesBattery
