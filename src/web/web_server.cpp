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

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

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
        doc["voltage"] = info.voltage_mV;
        doc["current"] = info.current_mA;
        doc["temp"] = info.temperature_C;
        doc["soc"] = info.stateOfCharge;
        doc["remain"] = info.remainCapacity_mAh;
        doc["full"] = info.fullCapacity_mAh;
        doc["design"] = info.designCapacity_mAh;
        doc["cycles"] = info.cycleCount;
        doc["status"] = info.batteryStatus;
        doc["sn"] = info.serialNumber;
        doc["mfr"] = info.manufacturerName;
        doc["dev"] = info.deviceName;
        doc["chem"] = info.chemistry;

        // Cells (sync if available)
        JsonArray cells = doc["cells"].to<JsonArray>();
        for (int i = 0; i < 4; i++) {
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

    // WiFi credentials save
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
