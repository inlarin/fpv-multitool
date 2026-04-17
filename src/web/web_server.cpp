#include "web_server.h"
#include "web_ui.h"
#include "web_state.h"
#include "wifi_manager.h"
#include "pin_config.h"
#include "battery/dji_battery.h"
#include "battery/smbus.h"
#include "motor/dshot.h"
#include "servo/servo_pwm.h"
#include "bridge/esp_rom_flasher.h"
#include "bridge/firmware_unpack.h"
#include "crsf/crsf_service.h"
#include "crsf/crsf_config.h"
#include "core/usb_mode.h"
#include "battery/battery_profiles.h"

extern "C" int cp2112_log_dump(char *out, int cap);
extern "C" uint32_t cp2112_log_seq();
extern "C" int cp2112_ep_info(char *out, int cap);

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#ifndef GITHUB_REPO
#define GITHUB_REPO ""
#endif

// State for background OTA pull (runs in its own task so HTTP request returns fast)
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

static AsyncWebServer *s_server = nullptr;
static AsyncWebSocket *s_ws = nullptr;
static bool s_running = false;
static uint32_t s_lastBroadcast = 0;

static void handleWsMsg(AsyncWebSocketClient *client, uint8_t *data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    WebState::Lock lock;  // protect all writes to shared state

    if (strcmp(cmd, "servo") == 0) {
        WebState::servo.pulseUs = doc["us"] | 1500;
        WebState::servo.active = true;
        ServoPWM::setPulse(WebState::servo.pulseUs);
    } else if (strcmp(cmd, "servoFreq") == 0) {
        int hz = doc["hz"] | 50;
        if (hz != WebState::servo.freq) {
            WebState::servo.freq = hz;
            ServoPWM::setFrequency(hz);
        }
    } else if (strcmp(cmd, "servoStop") == 0) {
        WebState::servo.active = false;
        WebState::servo.sweep = false;
        ServoPWM::stop();
    } else if (strcmp(cmd, "servoSweep") == 0) {
        WebState::servo.sweep = doc["on"] | false;
        if (WebState::servo.sweep && !WebState::servo.active) {
            ServoPWM::start(SIGNAL_OUT, WebState::servo.freq);
            WebState::servo.active = true;
        }
    } else if (strcmp(cmd, "motorArm") == 0) {
        WebState::motor.armRequest = true;
    } else if (strcmp(cmd, "motorDisarm") == 0) {
        WebState::motor.disarmRequest = true;
    } else if (strcmp(cmd, "throttle") == 0) {
        // DShot range 48-2047; UI sends 0-2000 (we add 47 offset later)
        int req = constrain((int)(doc["value"] | 0), 0, 2000);
        if (req > WebState::motor.maxThrottle) req = WebState::motor.maxThrottle;
        WebState::motor.throttle = req;
    } else if (strcmp(cmd, "dshotSpeed") == 0) {
        WebState::motor.dshotSpeed = doc["speed"] | 300;
    } else if (strcmp(cmd, "motorBeep") == 0) {
        WebState::motor.beepRequest = true;
    } else if (strcmp(cmd, "motorMaxThrottle") == 0) {
        int v = constrain((int)(doc["value"] | 2000), 100, 2000);
        WebState::motor.maxThrottle = v;
        if (WebState::motor.throttle > v) WebState::motor.throttle = v;
    } else if (strcmp(cmd, "motorDirCW") == 0) {
        WebState::motor.dirCwRequest = true;
    } else if (strcmp(cmd, "motorDirCCW") == 0) {
        WebState::motor.dirCcwRequest = true;
    } else if (strcmp(cmd, "motor3DOn") == 0) {
        WebState::motor.mode3DOnRequest = true;
    } else if (strcmp(cmd, "motor3DOff") == 0) {
        WebState::motor.mode3DOffRequest = true;
    }
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] client #%u connected\n", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            handleWsMsg(client, data, len);
        }
    }
}

static void broadcastTelemetry() {
    if (s_ws->count() == 0) return;

    // Battery data
    BatteryInfo info = DJIBattery::readAll();
    JsonDocument doc;
    doc["type"] = "batt";
    doc["connected"] = info.connected;
    if (info.connected) {
        doc["devType"] = (int)info.deviceType;  // 0=none,1=DJI,2=generic SBS
        doc["voltage"] = info.voltage_mV;
        doc["current"] = info.current_mA;
        doc["avgCurrent"] = info.avgCurrent_mA;
        doc["temp"] = info.temperature_C;
        doc["soc"] = info.stateOfCharge;
        doc["absSOC"] = info.absoluteSOC;
        doc["soh"] = info.stateOfHealth;
        doc["remain"] = info.remainCapacity_mAh;
        doc["full"] = info.fullCapacity_mAh;
        doc["design"] = info.designCapacity_mAh;
        doc["designV"] = info.designVoltage_mV;
        doc["cycles"] = info.cycleCount;
        doc["status"] = info.batteryStatus;
        doc["statusDecoded"] = DJIBattery::decodeBatteryStatus(info.batteryStatus);
        doc["sn"] = info.serialNumber;
        doc["mfgDate"] = info.manufactureDate;
        doc["mfr"] = info.manufacturerName;
        doc["dev"] = info.deviceName;
        doc["chem"] = info.chemistry;
        doc["cellCount"] = info.cellCount;

        // Time estimates
        doc["rte"] = info.runTimeToEmpty_min;
        doc["ate"] = info.avgTimeToEmpty_min;
        doc["ttf"] = info.timeToFull_min;
        doc["chgI"] = info.chargingCurrent_mA;
        doc["chgV"] = info.chargingVoltage_mV;

        // DJI-specific
        if (info.deviceType == DEV_DJI_BATTERY) {
            if (info.djiSerial.length() > 0) doc["djiSN"] = info.djiSerial;
            doc["djiPF2"] = info.djiPF2;
        }

        // Cells (sync if available)
        JsonArray cells = doc["cells"].to<JsonArray>();
        for (int i = 0; i < info.cellCount; i++) {
            uint16_t v = info.daStatus1Valid ? info.cellVoltSync[i] : info.cellVoltage[i];
            cells.add(v);
        }
        doc["cellsSync"] = info.daStatus1Valid;
        doc["packV"] = info.packVoltage;

        // Extended status
        doc["safetyStatus"] = info.safetyStatus;
        doc["pfStatus"] = info.pfStatus;
        doc["operationStatus"] = info.operationStatus;
        doc["manufacturingStatus"] = info.manufacturingStatus;
        doc["sealed"] = info.sealed;
        doc["hasPF"] = info.hasPF;

        // Decoded
        doc["pfDecoded"] = DJIBattery::decodePFStatus(info.pfStatus);
        doc["safetyDecoded"] = DJIBattery::decodeSafetyStatus(info.safetyStatus);
        doc["opDecoded"] = DJIBattery::decodeOperationStatus(info.operationStatus);
        doc["mfgDecoded"] = DJIBattery::decodeManufacturingStatus(info.manufacturingStatus);

        // Chip/model
        doc["chipType"] = info.chipType;
        doc["fwVer"] = info.firmwareVersion;
        doc["hwVer"] = info.hardwareVersion;
        doc["model"] = DJIBattery::modelName(info.model);
        doc["needsKey"] = DJIBattery::modelNeedsDjiKey(info.model);
    }
    String msg;
    serializeJson(doc, msg);
    s_ws->textAll(msg);

    // System info
    doc.clear();
    doc["type"] = "sys";
    doc["ip"] = WifiManager::getIP();
    doc["uptime"] = millis();
    doc["heap"] = ESP.getFreeHeap();
    doc["clients"] = s_ws->count();
    msg = "";
    serializeJson(doc, msg);
    s_ws->textAll(msg);

    // Motor status
    doc.clear();
    doc["type"] = "motor";
    doc["armed"] = WebState::motor.armed;
    doc["throttle"] = WebState::motor.throttle;
    msg = "";
    serializeJson(doc, msg);
    s_ws->textAll(msg);

    // Flash status
    doc.clear();
    doc["type"] = "flash";
    doc["size"] = WebState::flashState.fw_size;
    doc["in_progress"] = WebState::flashState.in_progress;
    doc["progress"] = WebState::flashState.progress_pct;
    doc["stage"] = WebState::flashState.stage;
    doc["result"] = WebState::flashState.lastResult;
    msg = "";
    serializeJson(doc, msg);
    s_ws->textAll(msg);

    // CRSF telemetry (if enabled)
    if (WebState::crsf.enabled && CRSFService::isRunning()) {
        const auto &st = CRSFService::state();
        doc.clear();
        doc["type"] = "crsf";
        doc["enabled"] = true;
        doc["connected"] = st.connected;
        doc["frames"] = st.total_frames;
        doc["badCrc"] = st.bad_crc;

        // Link stats
        if (st.link.valid) {
            JsonObject link = doc["link"].to<JsonObject>();
            link["rssi1"] = (int8_t)st.link.uplink_rssi1 * -1;
            link["rssi2"] = (int8_t)st.link.uplink_rssi2 * -1;
            link["lq"]    = st.link.uplink_link_quality;
            link["snr"]   = st.link.uplink_snr;
            link["rf"]    = st.link.rf_mode;
            link["power"] = st.link.uplink_tx_power;
            link["dlRssi"] = (int8_t)st.link.downlink_rssi * -1;
            link["dlLq"] = st.link.downlink_link_quality;
        }

        // Battery telemetry
        if (st.battery.valid) {
            JsonObject b = doc["battery"].to<JsonObject>();
            b["v"]    = st.battery.voltage_dV / 10.0f;
            b["i"]    = st.battery.current_dA / 10.0f;
            b["mah"]  = st.battery.capacity_mAh;
            b["pct"]  = st.battery.remaining_pct;
        }

        // GPS
        if (st.gps.valid) {
            JsonObject g = doc["gps"].to<JsonObject>();
            g["lat"] = st.gps.latitude_e7 / 1e7;
            g["lon"] = st.gps.longitude_e7 / 1e7;
            g["spd"] = st.gps.speed_cms / 27.78f; // cm/s → km/h
            g["hdg"] = st.gps.heading_cd / 100.0f;
            g["alt"] = (int)st.gps.altitude_m - 1000;
            g["sats"] = st.gps.satellites;
        }

        // Attitude
        if (st.attitude.valid) {
            JsonObject a = doc["att"].to<JsonObject>();
            a["p"] = st.attitude.pitch_10krad / 174.53f; // rad*10000 → degrees
            a["r"] = st.attitude.roll_10krad  / 174.53f;
            a["y"] = st.attitude.yaw_10krad   / 174.53f;
        }

        if (st.mode.valid) doc["mode"] = st.mode.name;

        // Channels
        if (st.channels.valid) {
            JsonArray ch = doc["ch"].to<JsonArray>();
            for (int i = 0; i < 16; i++) ch.add(st.channels.ch[i]);
        }

        // Device info
        const auto &dev = CRSFConfig::deviceInfo();
        if (dev.valid) {
            JsonObject d = doc["device"].to<JsonObject>();
            d["name"]   = dev.name;
            d["fw"]     = dev.sw_ver;
            d["hw"]     = dev.hw_ver;
            d["serial"] = dev.serial;
            d["fields"] = dev.field_count;
        }

        // Parameters
        if (CRSFConfig::paramCount() > 0) {
            JsonArray params = doc["params"].to<JsonArray>();
            for (int i = 0; i < CRSFConfig::paramCount(); i++) {
                const auto &p = CRSFConfig::param(i);
                if (!p.complete) continue;
                if (p.hidden) continue;  // skip debug/internal entries
                JsonObject o = params.add<JsonObject>();
                o["id"]     = p.id;
                o["parent"] = p.parent_id;
                o["type"]   = p.type;
                o["name"]   = p.name;
                o["unit"]   = p.unit;
                if (p.type == 9) {  // TEXT_SELECTION
                    o["opts"] = p.options;
                    o["idx"]  = p.option_index;
                } else if (p.type == 10 || p.type == 12) {  // STRING/INFO
                    o["value"] = p.value_text;
                } else if (p.type == 11) {  // FOLDER
                    // no value
                } else if (p.type == 13) {  // COMMAND
                    // ELRS lifecycle: status + info, rendered as state-aware button(s).
                    o["status"]  = p.value_num;   // 0=READY,2=PROG,3=CONFIRM_NEEDED,...
                    o["timeout"] = p.max_val;     // in 10ms units
                    o["info"]    = p.value_text;
                } else {
                    o["value"] = p.value_num;
                    o["min"] = p.min_val;
                    o["max"] = p.max_val;
                }
            }
        }

        msg = "";
        serializeJson(doc, msg);
        s_ws->textAll(msg);
    }
}

// Flash progress callback (called from flasher)
static void flashProgress(int pct, const char* stage) {
    WebState::flashState.progress_pct = pct;
    WebState::flashState.stage = stage;
}

// Execute flashing (called from main loop when flash_request = true)
static void executeFlash() {
    WebState::flashState.in_progress = true;
    WebState::flashState.progress_pct = 0;
    WebState::flashState.stage = "Detecting format";
    WebState::flashState.lastResult = "";

    const uint8_t *fw_ptr = WebState::flashState.fw_data;
    size_t fw_size = WebState::flashState.fw_size;
    uint8_t *decompressed = nullptr; // temp buffer to free after

    // Detect format and unpack if needed
    auto fmt = FirmwareUnpack::detect(fw_ptr, fw_size);
    Serial.printf("[Flash] Format: %s\n", FirmwareUnpack::formatName(fmt));

    if (fmt == FirmwareUnpack::FMT_GZIP) {
        WebState::flashState.stage = "Decompressing";
        size_t out_size = 0;
        decompressed = FirmwareUnpack::gunzip(fw_ptr, fw_size, &out_size);
        if (!decompressed) {
            WebState::flashState.in_progress = false;
            WebState::flashState.lastResult = "Gzip decompress failed";
            return;
        }
        fw_ptr = decompressed;
        fw_size = out_size;
    } else if (fmt == FirmwareUnpack::FMT_ELRS) {
        size_t out_size = 0;
        const uint8_t *extracted = FirmwareUnpack::extractELRS(fw_ptr, fw_size, &out_size);
        if (!extracted) {
            WebState::flashState.in_progress = false;
            WebState::flashState.lastResult = "ELRS container parse failed";
            return;
        }
        fw_ptr = extracted;
        fw_size = out_size;
    } else if (fmt != FirmwareUnpack::FMT_RAW_BIN) {
        WebState::flashState.in_progress = false;
        WebState::flashState.lastResult = "Unknown firmware format (not .bin/.gz/.elrs)";
        return;
    }

    Serial.printf("[Flash] Flashing %u bytes\n", fw_size);
    WebState::flashState.stage = "Starting";

    ESPFlasher::Config cfg;
    cfg.uart = &Serial1;
    cfg.tx_pin = ELRS_TX;
    cfg.rx_pin = ELRS_RX;
    cfg.baud_rate = 115200;
    cfg.flash_offset = 0;
    cfg.progress = flashProgress;

    ESPFlasher::Result r = ESPFlasher::flash(cfg, fw_ptr, fw_size);

    if (decompressed) free(decompressed);

    // Free the uploaded firmware buffer after flashing — successful or not
    // (user can re-upload to try again)
    {
        WebState::Lock lock;
        if (WebState::flashState.fw_data) {
            free(WebState::flashState.fw_data);
            WebState::flashState.fw_data = nullptr;
        }
        WebState::flashState.fw_size = 0;
        WebState::flashState.fw_received = 0;
        WebState::flashState.in_progress = false;
        WebState::flashState.lastResult = ESPFlasher::errorString(r);
    }
    Serial.printf("[Flash] Result: %s (buffer freed)\n", ESPFlasher::errorString(r));
}

void WebServer::start() {
    if (s_running) return;

    s_server = new AsyncWebServer(80);
    s_ws = new AsyncWebSocket("/ws");
    s_ws->onEvent(onWsEvent);
    s_server->addHandler(s_ws);

    // Root
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", WEB_INDEX_HTML);
    });

    // Battery service actions
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
        // DJI-specific
        if (info.deviceType == DEV_DJI_BATTERY) {
            if (info.djiSerial.length() > 0) d["djiSerialNumber"] = info.djiSerial;
            d["djiPF2"] = String("0x") + String(info.djiPF2, HEX);
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

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

    // Diagnostic endpoint — raw SBS / MAC reads + arbitrary unseal keys.
    // Examples:
    //   /api/batt/diag?mac=0x0070                     -> MAC block read (ChemID)
    //   /api/batt/diag?sbs=0x20&type=string           -> ManufacturerName
    //   /api/batt/diag?sbs=0x54&type=dword            -> OperationStatus
    //   /api/batt/diag?unseal=0xABCD,0x1234           -> custom key pair
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

    s_server->on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["mode"] = WifiManager::currentMode();
        doc["ssid"] = WifiManager::getSSID();
        doc["ip"] = WifiManager::getIP();
        doc["heap"] = ESP.getFreeHeap();
        doc["uptime"] = millis();
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ELRS firmware upload
    // Max accepted firmware size (ELRS is ~400KB, headroom for big builds)
    static const size_t MAX_FW_SIZE = 2 * 1024 * 1024;

    s_server->on("/api/flash/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            if (WebState::flashState.fw_size > 0) {
                req->send(200, "text/plain", "Uploaded " + String(WebState::flashState.fw_size) + " bytes");
            } else {
                req->send(400, "text/plain", "No data");
            }
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                WebState::Lock lock;
                // Free previous buffer (critical — prevents PSRAM leak)
                if (WebState::flashState.fw_data) {
                    free(WebState::flashState.fw_data);
                    WebState::flashState.fw_data = nullptr;
                }
                WebState::flashState.fw_size = 0;
                WebState::flashState.fw_received = 0;
                WebState::flashState.lastResult = "";

                // Determine content length from Content-Length header
                size_t alloc_size = MAX_FW_SIZE;
                if (req->hasHeader("Content-Length")) {
                    size_t cl = req->header("Content-Length").toInt();
                    // Multipart has some overhead, add small margin
                    if (cl > 0 && cl < MAX_FW_SIZE) alloc_size = cl + 256;
                }

                WebState::flashState.fw_data = (uint8_t*)ps_malloc(alloc_size);
                if (!WebState::flashState.fw_data) {
                    Serial.printf("[Flash] ps_malloc(%u) failed\n", alloc_size);
                    WebState::flashState.lastResult = "Out of PSRAM";
                    return;
                }
            }
            if (!WebState::flashState.fw_data) return;
            if (index + len > MAX_FW_SIZE) return;  // overflow guard
            memcpy(WebState::flashState.fw_data + index, data, len);
            WebState::flashState.fw_received = index + len;
            if (final) {
                WebState::flashState.fw_size = index + len;
                Serial.printf("[Flash] Upload complete: %u bytes\n", WebState::flashState.fw_size);
            }
        });

    // Start flashing (after upload)
    s_server->on("/api/flash/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.fw_size == 0) {
            req->send(400, "text/plain", "No firmware uploaded");
            return;
        }
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "Already in progress");
            return;
        }
        WebState::flashState.flash_request = true;
        req->send(200, "text/plain", "Flashing started");
    });

    // ===== CRSF endpoints =====
    s_server->on("/api/crsf/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool inv = req->hasParam("inverted") ? req->getParam("inverted")->value() == "1" : false;
        WebState::crsf.inverted = inv;
        CRSFService::begin(&Serial1, ELRS_RX, ELRS_TX, 420000, inv);
        CRSFConfig::init();
        WebState::crsf.enabled = true;
        req->send(200, "text/plain", inv ? "CRSF started (inverted)" : "CRSF started");
    });

    s_server->on("/api/crsf/stop", HTTP_POST, [](AsyncWebServerRequest *req) {
        CRSFService::end();
        WebState::crsf.enabled = false;
        req->send(200, "text/plain", "CRSF stopped");
    });

    s_server->on("/api/crsf/ping", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        CRSFConfig::reset();
        CRSFConfig::requestDeviceInfo();
        req->send(200, "text/plain", "Ping sent");
    });

    s_server->on("/api/crsf/params", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        CRSFConfig::requestAllParameters();
        req->send(200, "text/plain", "Requesting parameters...");
    });

    s_server->on("/api/crsf/write", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        if (!req->hasParam("id") || !req->hasParam("value")) {
            req->send(400, "text/plain", "missing id/value"); return;
        }
        uint8_t id = req->getParam("id")->value().toInt();
        String v = req->getParam("value")->value();
        const auto *p = CRSFConfig::paramById(id);
        if (!p) { req->send(404, "text/plain", "unknown param id"); return; }
        bool ok = CRSFConfig::writeParamAuto(id, v);
        if (!ok) {
            req->send(400, "text/plain", "param type not writable");
            return;
        }
        req->send(200, "text/plain", "OK");
    });

    s_server->on("/api/crsf/bind", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        CRSFService::cmdRxBind();
        req->send(200, "text/plain", "Bind command sent");
    });

    s_server->on("/api/crsf/wifi", HTTP_POST, [](AsyncWebServerRequest *req) {
        // ELRS exposes "Enable WiFi" as a PARAM_COMMAND on the receiver.
        // To trigger it we need the param tree loaded (via /api/crsf/params),
        // then write value=1 (click/start) to its id.
        if (!CRSFService::isRunning()) {
            req->send(400, "text/plain", "CRSF not running");
            return;
        }
        if (CRSFConfig::paramCount() == 0) {
            req->send(400, "text/plain", "Read parameters first (Read Params)");
            return;
        }
        const CRSFConfig::Param *p = CRSFConfig::findCommandParamByName("wifi");
        if (!p) {
            req->send(404, "text/plain", "WiFi command param not found — check RX firmware");
            return;
        }
        bool ok = CRSFConfig::writeParamByte(p->id, 1);  // 1 = start/click
        if (!ok) {
            req->send(500, "text/plain", "Failed to send param write");
            return;
        }
        String msg = "Triggered: ";
        msg += p->name;
        msg += " (id=" + String(p->id) + ")";
        req->send(200, "text/plain", msg);
    });

    s_server->on("/api/crsf/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!CRSFService::isRunning()) { req->send(400, "text/plain", "CRSF not running"); return; }
        CRSFService::cmdReboot();
        req->send(200, "text/plain", "Reboot command sent");
    });

    // Clear uploaded firmware
    s_server->on("/api/flash/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.fw_data) {
            free(WebState::flashState.fw_data);
            WebState::flashState.fw_data = nullptr;
        }
        WebState::flashState.fw_size = 0;
        WebState::flashState.fw_received = 0;
        WebState::flashState.progress_pct = 0;
        WebState::flashState.stage = "";
        WebState::flashState.lastResult = "";
        req->send(200, "text/plain", "Cleared");
    });

    // ===== OTA firmware update =====
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

    // Query GitHub releases/latest API; cache asset URL + tag for subsequent pull.
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

    // Start background download + flash from the previously checked asset URL.
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
            bool ok = !Update.hasError();
            AsyncWebServerResponse *resp = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK — rebooting" : String("FAIL: ") + Update.errorString());
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                Serial.println("[OTA] Success — rebooting in 500 ms");
                // Schedule reboot after response flush
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
                if (!Update.begin(content_len > 0 ? content_len : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Update.printError(Serial);
                    return;
                }
            }
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

    // ===== USB mode (descriptor selection) =====
    s_server->on("/api/usb/mode", HTTP_GET, [](AsyncWebServerRequest *req) {
        UsbDescriptorMode m = UsbMode::load();
        JsonDocument j;
        j["current"]      = (int)m;
        j["current_name"] = UsbMode::name(m);
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

    // SMBus transaction log (ring buffer)
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

    s_server->on("/api/usb/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Rebooting...");
        delay(200);
        ESP.restart();
    });

    s_server->onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "Not found");
    });

    s_server->begin();
    s_running = true;
    Serial.println("[Web] Server started on :80");
}

void WebServer::stop() {
    if (!s_running) return;
    if (s_server) { s_server->end(); delete s_server; s_server = nullptr; }
    if (s_ws) { delete s_ws; s_ws = nullptr; }
    s_running = false;
    Serial.println("[Web] Server stopped");
}

bool WebServer::isRunning() { return s_running; }

void WebServer::loop() {
    if (!s_running) return;

    // CRSF loop (parse incoming, handle params)
    if (CRSFService::isRunning()) {
        CRSFService::loop();
        CRSFConfig::loop();
    }

    uint32_t now = millis();
    if (now - s_lastBroadcast > 1000) {
        broadcastTelemetry();
        s_lastBroadcast = now;
        s_ws->cleanupClients();
    }

    // Execute flash request
    if (WebState::flashState.flash_request && !WebState::flashState.in_progress) {
        WebState::flashState.flash_request = false;
        executeFlash();
    }

    // Execute pending battery service actions from web thread
    if (WebState::battSvc.pending != WebState::BS_NONE) {
        bool ok = false;
        switch (WebState::battSvc.pending) {
            case WebState::BS_UNSEAL:  ok = DJIBattery::unseal(); break;
            case WebState::BS_CLEARPF: ok = DJIBattery::clearPFProper(); break;
            case WebState::BS_SEAL:    ok = DJIBattery::seal(); break;
            default: break;
        }
        WebState::battSvc.lastResult = ok ? "OK" : "FAILED";
        WebState::battSvc.pending = WebState::BS_NONE;
    }
}
