// AUTO-REFACTORED from web_server.cpp 2026-04-21. Keep endpoint
// registration + executeFlash() + flashProgress() + dump-session state
// for ELRS RX flashing (/api/flash/*, /api/elrs/*, /api/otadata/*,
// /api/crsf/reboot_to_bl, /api/bridge/listen).
//
// Everything here runs inside the single AsyncWebServer. The public entry
// point is registerRoutesFlash(s_server), called once from WebServer::start().
// executeFlash() is invoked from the main loop whenever
// WebState::flashState.flash_request flips true.
#include "routes_flash.h"

#include "../web_state.h"
#include "../../bridge/esp_rom_flasher.h"
#include "../../bridge/firmware_unpack.h"
#include "../../core/pin_port.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Static state used by the dump endpoints. Inside a function-scope static so
// lambdas capture it; moved out here verbatim from WebServer::start().
namespace RoutesFlash {

// Max firmware upload size — larger than any realistic ELRS image but well
// within our 8 MB PSRAM. 2 MB covers 4.0.0 + future builds with room to spare.
static const size_t MAX_FW_SIZE = 2 * 1024 * 1024;

static void flashProgress(int pct, const char* stage) {
    WebState::flashState.progress_pct = pct;
    WebState::flashState.stage = stage;
}

// Non-static — declared in routes_flash.h as RoutesFlash::executeFlash()
// for the main-loop dispatcher. File-local would clash with namespace decl.
void executeFlash() {
    WebState::flashState.in_progress = true;
    WebState::flashState.progress_pct = 0;
    WebState::flashState.stage = "Detecting format";
    WebState::flashState.lastResult = "";

    const uint8_t *fw_ptr = WebState::flashState.fw_data;
    size_t fw_size = WebState::flashState.fw_size;
    uint8_t *decompressed = nullptr; // temp buffer to free after

    // Detect format and unpack if needed. `flash_raw` bypasses detection
    // entirely — used when writing partition tables, OTADATA blobs, or
    // any raw byte sequence that isn't a standalone ESP image.
    FirmwareUnpack::Format fmt = WebState::flashState.flash_raw
        ? FirmwareUnpack::FMT_RAW_BIN
        : FirmwareUnpack::detect(fw_ptr, fw_size);
    Serial.printf("[Flash] Format: %s (raw=%d)\n",
                  FirmwareUnpack::formatName(fmt),
                  WebState::flashState.flash_raw ? 1 : 0);

    if (fmt == FirmwareUnpack::FMT_GZIP) {
        WebState::flashState.stage = "Decompressing";
        size_t out_size = 0;
        decompressed = FirmwareUnpack::gunzip(fw_ptr, fw_size, &out_size);
        if (!decompressed) {
            WebState::flashState.stage = "Failed";
            WebState::flashState.progress_pct = 0;
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
            WebState::flashState.stage = "Failed";
            WebState::flashState.progress_pct = 0;
            WebState::flashState.in_progress = false;
            WebState::flashState.lastResult = "ELRS container parse failed";
            return;
        }
        fw_ptr = extracted;
        fw_size = out_size;
    } else if (fmt != FirmwareUnpack::FMT_RAW_BIN) {
        WebState::flashState.stage = "Failed";
        WebState::flashState.progress_pct = 0;
        WebState::flashState.in_progress = false;
        WebState::flashState.lastResult = "Unknown firmware format (not .bin/.gz/.elrs)";
        return;
    }

    Serial.printf("[Flash] Flashing %u bytes\n", fw_size);
    WebState::flashState.stage = "Starting";

    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_flash")) {
        WebState::flashState.stage = "Failed";
        WebState::flashState.progress_pct = 0;
        WebState::flashState.in_progress = false;
        WebState::flashState.lastResult = "Port B busy — switch to UART";
        if (decompressed) free(decompressed);
        return;
    }

    ESPFlasher::Config cfg;
    cfg.uart = &Serial1;
    cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
    cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
    // Two flash paths:
    //   via_stub=false (default) — talk ROM DFU at 115200. User must have
    //   physically put RX in DFU (BOOT + power-cycle). Required for full
    //   bootloader/partition flash.
    //   via_stub=true — send CRSF 'bl' frame at 420000, RX transitions into
    //   its in-app esptool stub on the same UART at 420000. No physical
    //   buttons required. Only reaches app0/app1 partitions (stub cannot
    //   touch bootloader @ 0x0 or partition table @ 0x8000).
    bool via_stub = WebState::flashState.flash_via_stub;
    cfg.baud_rate = via_stub ? 420000 : 115200;
    cfg.flash_offset = WebState::flashState.flash_offset;
    cfg.stay_in_loader = WebState::flashState.flash_stay;
    cfg.progress = flashProgress;

    if (via_stub) {
        WebState::flashState.stage = "CRSF bl";
        Serial.printf("[Flash] via stub — sending CRSF 'bl' @ 420000\n");
        ESPFlasher::sendCrsfReboot(cfg);
        // After sendCrsfReboot returns, Serial1 is closed; ESPFlasher::flash()
        // re-opens at 420000 and does its own SYNC against the stub.
    }

    // 5-point sample verify in-session. Spread across the image so any
    // silent-drop past some internal ROM boundary is caught. 256 B per
    // sample keeps stack modest (< 2 KB total).
    const int N_SAMPLES = 5;
    ESPFlasher::Sample samples[N_SAMPLES];
    uint32_t sample_file_offs[N_SAMPLES];
    {
        uint32_t last = (fw_size >= 256) ? (fw_size - 256) : 0;
        sample_file_offs[0] = 0;
        sample_file_offs[1] = (uint32_t)(fw_size / 4);
        sample_file_offs[2] = (uint32_t)(fw_size / 2);
        sample_file_offs[3] = (uint32_t)(fw_size * 3 / 4);
        sample_file_offs[4] = last;
        for (int i = 0; i < N_SAMPLES; i++) {
            samples[i].offset = cfg.flash_offset + sample_file_offs[i];
            samples[i].size = (fw_size >= 256) ? 256 : (uint32_t)fw_size;
            samples[i].ok = false;
        }
    }

    ESPFlasher::Result r = ESPFlasher::flash(cfg, fw_ptr, fw_size, samples, N_SAMPLES);

    PinPort::release(PinPort::PORT_B);

    // Compare samples against local image on success.
    String verifyMsg;
    if (r == ESPFlasher::FLASH_OK) {
        int mismatches = 0;
        int read_fail = 0;
        uint32_t first_mismatch_file_off = 0xFFFFFFFF;
        for (int i = 0; i < N_SAMPLES; i++) {
            if (!samples[i].ok) { read_fail++; continue; }
            if (memcmp(samples[i].data, fw_ptr + sample_file_offs[i], samples[i].size) != 0) {
                mismatches++;
                if (first_mismatch_file_off == 0xFFFFFFFF) {
                    first_mismatch_file_off = sample_file_offs[i];
                }
            }
        }
        if (mismatches == 0 && read_fail == 0) {
            verifyMsg = " (verified " + String(N_SAMPLES) + "/" + String(N_SAMPLES) + " samples)";
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf),
                " VERIFY FAILED: %d mismatch, %d read_fail, first bad @file+0x%x",
                mismatches, read_fail, (unsigned)first_mismatch_file_off);
            verifyMsg = buf;
            r = ESPFlasher::FLASH_ERR_WRITE_FAILED;
        }
    }

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
        WebState::flashState.lastResult = String(ESPFlasher::errorString(r)) + verifyMsg;
    }
    Serial.printf("[Flash] Result: %s%s\n", ESPFlasher::errorString(r), verifyMsg.c_str());
}

// Wire up all ELRS / flash / otadata endpoints. Called once from
// WebServer::start(). `s_server` parameter mirrors the global used by the
// registration lambdas below — preserving captures / behaviour verbatim.
void registerRoutesFlash(AsyncWebServer *s_server) {

    // Probe the current operational mode of a connected RX via Port B. Tries,
    // in order: ROM DFU sync @ 115200, ELRS in-app stub sync @ 420000, then
    // best-effort app-telemetry sniff @ 420000. Returns one of:
    //   "dfu"     — ROM bootloader answered SYNC at 115200
    //   "stub"    — ELRS in-app stub answered SYNC at 420000 (RX in stub mode)
    //   "app"     — UART bytes observed on 420000 (CRSF telemetry heuristic)
    //   "silent"  — no response and no bytes on either baud (likely in WiFi AP)
    // UI uses this to enable/disable the right flash path buttons.
    //   POST /api/elrs/rx_mode
    s_server->on("/api/elrs/rx_mode", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_rx_mode")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        const char *mode = "silent";
        bool dfu_ok = false, stub_ok = false;
        ESPFlasher::ElrsDeviceInfo devInfo;
        devInfo.ok = false;

        // IMPORTANT: order matters because ESP32-C3 ROM has an autobauder that
        // latches to whichever baud sees activity first. If we send CRSF @
        // 420000 first (even a polite DEVICE_PING), a just-booted ROM DFU will
        // lock onto 420000 and later 115200 SYNC attempts fail. So we must
        // test DFU @ 115200 FIRST — no 420000 traffic on the wire beforehand.
        //
        // Test 1 (ROM DFU @ 115200): SLIP SYNC. If RX is in app, the 115200
        // bytes arrive at its 420000 CRSF parser as garbage and are ignored.
        // If RX is in ROM DFU, ROM autobauder locks to 115200 and replies.
        {
            ESPFlasher::Config cfg;
            cfg.uart = &Serial1;
            cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
            cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
            cfg.baud_rate = 115200;
            ESPFlasher::ChipInfo ci;
            if (ESPFlasher::chipInfo(cfg, &ci) == ESPFlasher::FLASH_OK && ci.ok) {
                dfu_ok = true;
                mode = "dfu";
            }
        }

        // Test 2 (CRSF DEVICE_PING @ 420000) — only if DFU didn't answer.
        if (!dfu_ok) {
            delay(50);
            ESPFlasher::Config cfg;
            cfg.uart = &Serial1;
            cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
            cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
            cfg.baud_rate = 420000;
            if (ESPFlasher::crsfDevicePing(cfg, 250, &devInfo) == ESPFlasher::FLASH_OK && devInfo.ok) {
                mode = "app";
            }
        }

        // Test 3: in-app stub SYNC @ 420000 (RX just post-'bl' frame — app
        // paused, stub waiting for SLIP). Runs only if app DEVICE_PING failed.
        if (!dfu_ok && !devInfo.ok) {
            delay(50);
            ESPFlasher::Config cfg;
            cfg.uart = &Serial1;
            cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
            cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
            cfg.baud_rate = 420000;
            ESPFlasher::ChipInfo ci;
            if (ESPFlasher::chipInfo(cfg, &ci) == ESPFlasher::FLASH_OK && ci.ok) {
                stub_ok = true;
                mode = "stub";
            }
        }

        PinPort::release(PinPort::PORT_B);

        JsonDocument d;
        d["mode"]    = mode;
        d["dfu_ok"]  = dfu_ok;
        d["stub_ok"] = stub_ok;
        d["app_ok"]  = devInfo.ok;
        if (devInfo.ok) {
            JsonObject a = d["app"].to<JsonObject>();
            a["name"]             = devInfo.name;
            a["serial_no"]        = devInfo.serial_no;
            a["hw_id"]            = devInfo.hw_id;
            a["sw_version"]       = devInfo.sw_version;
            a["field_count"]      = devInfo.field_count;
            a["parameter_version"] = devInfo.parameter_version;
        }
        d["hint"] =
            devInfo.ok ? (String("RX running ") + devInfo.name + " — Stub-flash available; DFU flash disabled") :
            dfu_ok     ? "ROM DFU — use Flash(DFU) path; Stub-flash unavailable" :
            stub_ok    ? "in-app stub already active — SYNC works @ 420000" :
                         "silent — WiFi AP mode (UART disabled), halted, or wiring lost. "
                         "To recover: power-cycle RX (no BOOT) — app starts for ~60s before "
                         "auto-wifi kicks in (only if wifi-on-interval > 0).";
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Read ELRS device identity via DEVICE_PING/INFO. Use when Probe returns
    // mode='app' but you want full details (name, version, field count).
    //   POST /api/elrs/device_info
    s_server->on("/api/elrs/device_info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_device_info")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;
        ESPFlasher::ElrsDeviceInfo info;
        ESPFlasher::Result r = ESPFlasher::crsfDevicePing(cfg, 300, &info);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK || !info.ok) {
            req->send(500, "text/plain",
                "No DEVICE_INFO reply — RX not in app (may be wifi/stub/dfu/silent)");
            return;
        }
        JsonDocument d;
        d["name"]              = info.name;
        d["serial_no"]         = info.serial_no;
        d["hw_id"]             = info.hw_id;
        char ver[16];
        snprintf(ver, sizeof(ver), "%u.%u.%u.%u",
                 (unsigned)(info.sw_version >> 24) & 0xff,
                 (unsigned)(info.sw_version >> 16) & 0xff,
                 (unsigned)(info.sw_version >>  8) & 0xff,
                 (unsigned)(info.sw_version      ) & 0xff);
        d["sw_version"]        = ver;
        d["sw_version_raw"]    = info.sw_version;
        d["field_count"]       = info.field_count;
        d["parameter_version"] = info.parameter_version;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // List all LUA parameters. Iterates field_id=1..N from DEVICE_INFO,
    // reads chunk 0 per field via PARAMETER_READ (0x2C). Parses the
    // PARAMETER_SETTINGS_ENTRY (0x2B) body into JSON. ~50 ms per field
    // (~2 s total for a typical 40-field RX), so frontend should show a
    // spinner. Multi-chunk params (rare — only very long TEXT_SELECTION
    // option lists) return their first chunk only.
    //
    //   GET /api/elrs/params
    s_server->on("/api/elrs/params", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_params")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;

        // First probe device to get field count.
        ESPFlasher::ElrsDeviceInfo di;
        if (ESPFlasher::crsfDevicePing(cfg, 250, &di) != ESPFlasher::FLASH_OK || !di.ok) {
            PinPort::release(PinPort::PORT_B);
            req->send(500, "text/plain",
                "DEVICE_PING failed — RX must be running the app (not DFU/WiFi)");
            return;
        }

        AsyncResponseStream *rs = req->beginResponseStream("application/json");
        rs->print("{\"device\":{\"name\":\"");
        rs->print(di.name);
        rs->printf("\",\"field_count\":%u},\"params\":[", (unsigned)di.field_count);

        uint8_t buf[192];
        size_t n = 0;
        uint8_t chunks_left = 0;
        bool first = true;
        for (uint8_t id = 1; id <= di.field_count; id++) {
            if (ESPFlasher::crsfParamRead(cfg, id, 0, buf, sizeof(buf), &n, &chunks_left)
                != ESPFlasher::FLASH_OK || n < 2) continue;
            uint8_t parent = buf[0];
            uint8_t raw_type = buf[1];
            uint8_t type = raw_type & 0x3F;                     // mask hidden bits
            bool hidden = (raw_type & 0xC0) != 0;
            // Name null-terminated starting at buf[2]
            size_t ni = 2;
            while (ni < n && buf[ni] != 0) ni++;
            if (ni >= n) continue;  // malformed

            if (!first) rs->print(",");
            first = false;
            rs->printf("{\"id\":%u,\"parent\":%u,\"type\":%u,\"hidden\":%s,\"chunks_left\":%u,\"name\":\"",
                       id, parent, type, hidden ? "true" : "false", chunks_left);
            for (size_t i = 2; i < ni; i++) {
                char c = (char)buf[i];
                if (c == '"' || c == '\\') rs->print("\\");
                rs->print(c);
            }
            rs->print("\"");

            // Type-specific payload starts at buf[ni+1]
            size_t p = ni + 1;
            switch (type) {
                case 0: case 1:  // UINT8 / INT8
                    if (p + 4 <= n) {
                        int val = (int8_t)buf[p]; if (type == 0) val = buf[p];
                        int mn  = (int8_t)buf[p+1]; if (type == 0) mn = buf[p+1];
                        int mx  = (int8_t)buf[p+2]; if (type == 0) mx = buf[p+2];
                        int def = (int8_t)buf[p+3]; if (type == 0) def = buf[p+3];
                        rs->printf(",\"value\":%d,\"min\":%d,\"max\":%d,\"default\":%d", val, mn, mx, def);
                    }
                    break;
                case 2: case 3: {  // UINT16 / INT16 (big-endian)
                    auto be16 = [](const uint8_t *p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); };
                    if (p + 8 <= n) {
                        int val = (type == 2) ? (uint16_t)((buf[p]<<8)|buf[p+1]) : be16(buf + p);
                        int mn  = (type == 2) ? (uint16_t)((buf[p+2]<<8)|buf[p+3]) : be16(buf + p + 2);
                        int mx  = (type == 2) ? (uint16_t)((buf[p+4]<<8)|buf[p+5]) : be16(buf + p + 4);
                        int def = (type == 2) ? (uint16_t)((buf[p+6]<<8)|buf[p+7]) : be16(buf + p + 6);
                        rs->printf(",\"value\":%d,\"min\":%d,\"max\":%d,\"default\":%d", val, mn, mx, def);
                    }
                    break;
                }
                case 9: {  // TEXT_SELECTION
                    // [options-string null-separated? actually semicolon-separated ending with \0]
                    // Current value = 1 byte after options string. Then min/max/default bytes + unit-string.
                    size_t opts_start = p;
                    size_t opts_end = opts_start;
                    while (opts_end < n && buf[opts_end] != 0) opts_end++;
                    rs->print(",\"options\":\"");
                    for (size_t i = opts_start; i < opts_end; i++) {
                        char c = (char)buf[i];
                        if (c == '"' || c == '\\') rs->print("\\");
                        rs->print(c);
                    }
                    rs->print("\"");
                    if (opts_end + 4 <= n) {
                        rs->printf(",\"value\":%u,\"min\":%u,\"max\":%u,\"default\":%u",
                                   buf[opts_end + 1], buf[opts_end + 2],
                                   buf[opts_end + 3], buf[opts_end + 4]);
                    }
                    break;
                }
                case 10: case 12: {  // STRING / INFO — null-terminated
                    size_t se = p;
                    while (se < n && buf[se] != 0) se++;
                    rs->print(",\"value\":\"");
                    for (size_t i = p; i < se; i++) {
                        char c = (char)buf[i];
                        if (c == '"' || c == '\\') rs->print("\\");
                        else if (c < 0x20 || c > 0x7e) c = '?';
                        rs->print(c);
                    }
                    rs->print("\"");
                    break;
                }
                case 11:  // FOLDER — name is enough, no value
                    break;
                case 13:  // COMMAND
                    if (p < n) rs->printf(",\"command_state\":%u", buf[p]);
                    break;
                default: break;
            }
            rs->print("}");
            // Small gap between reads — RX needs time to queue the next reply
            // after previous UART burst. 10 ms suffices.
            delay(10);
        }
        rs->print("]}");
        req->send(rs);
        PinPort::release(PinPort::PORT_B);
    });

    // Write one LUA parameter. Value encoded per type:
    //   UINT8/INT8/TEXT_SELECTION/COMMAND: form field 'value' as int, 1 byte
    //   UINT16/INT16: 'value' int, 2 bytes big-endian
    //   STRING: 'value' as UTF-8 text (up to 32 bytes including null)
    //
    //   POST /api/elrs/params/write  form: id=N&type=T&value=V
    s_server->on("/api/elrs/params/write", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress");
            return;
        }
        if (!req->hasParam("id", true) || !req->hasParam("type", true) || !req->hasParam("value", true)) {
            req->send(400, "text/plain", "need id + type + value");
            return;
        }
        uint8_t id   = (uint8_t)req->getParam("id",   true)->value().toInt();
        uint8_t type = (uint8_t)req->getParam("type", true)->value().toInt();
        String  vraw =          req->getParam("value",true)->value();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_param_w")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;

        uint8_t payload[34];
        uint8_t payload_len = 0;
        switch (type) {
            case 0: case 1: case 9: case 13: {
                int v = vraw.toInt();
                payload[0] = (uint8_t)(v & 0xff);
                payload_len = 1;
                break;
            }
            case 2: case 3: {
                int v = vraw.toInt();
                payload[0] = (uint8_t)((v >> 8) & 0xff);
                payload[1] = (uint8_t)(v & 0xff);
                payload_len = 2;
                break;
            }
            case 10: {  // STRING — write raw UTF-8 up to 32 B including null
                size_t L = vraw.length();
                if (L > 31) L = 31;
                memcpy(payload, vraw.c_str(), L);
                payload[L] = 0;
                payload_len = L + 1;
                break;
            }
            default:
                PinPort::release(PinPort::PORT_B);
                req->send(400, "text/plain", "unsupported param type for write");
                return;
        }
        ESPFlasher::crsfParamWrite(cfg, id, payload, payload_len);
        PinPort::release(PinPort::PORT_B);
        char msg[96];
        snprintf(msg, sizeof(msg), "WRITE id=%u type=%u len=%u value=%s",
                 id, type, payload_len, vraw.c_str());
        req->send(200, "text/plain", msg);
    });

    // CRSF "enter binding" — puts RX into 60s bind-wait. Safe runtime op,
    // no flash destruction. Useful for pairing to a new handset without phones.
    //   POST /api/elrs/bind
    s_server->on("/api/elrs/bind", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_bind")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;
        ESPFlasher::sendCrsfBind(cfg);
        PinPort::release(PinPort::PORT_B);
        req->send(200, "text/plain", "CRSF 'bd' frame sent — RX in bind mode for 60 s");
    });

    s_server->on("/api/flash/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            // If upload handler flagged an out-of-memory or overflow condition,
            // surface it to the client rather than pretending the upload succeeded.
            if (WebState::flashState.lastResult.length() > 0 &&
                WebState::flashState.fw_size == 0) {
                req->send(413, "text/plain", WebState::flashState.lastResult);
                return;
            }
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

                // Determine content length from Content-Length header.
                // Multipart adds boundary overhead — 1 KB is plenty, and we
                // clamp to MAX_FW_SIZE so cl just under the cap can't push
                // us past it.
                size_t alloc_size = MAX_FW_SIZE;
                if (req->hasHeader("Content-Length")) {
                    size_t cl = req->header("Content-Length").toInt();
                    if (cl > 0 && cl < MAX_FW_SIZE) {
                        alloc_size = cl + 1024;
                        if (alloc_size > MAX_FW_SIZE) alloc_size = MAX_FW_SIZE;
                    }
                }

                // Try PSRAM first via heap_caps (ps_malloc sometimes reports
                // unavailable even though MALLOC_CAP_SPIRAM has plenty).
                // Fall back to regular heap if PSRAM path fails.
                WebState::flashState.fw_data =
                    (uint8_t*)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
                if (!WebState::flashState.fw_data) {
                    WebState::flashState.fw_data = (uint8_t*)malloc(alloc_size);
                }
                if (!WebState::flashState.fw_data) {
                    Serial.printf("[Flash] alloc(%u) failed — no PSRAM or heap\n", (unsigned)alloc_size);
                    WebState::flashState.lastResult = "Out of memory";
                    return;
                }
                Serial.printf("[Flash] allocated %u bytes for upload\n", (unsigned)alloc_size);
            }
            if (!WebState::flashState.fw_data) return;
            if (index + len > MAX_FW_SIZE) {
                // Truncated upload — mark explicitly so POST handler returns 413
                // instead of reporting a short success.
                WebState::Lock lock;
                WebState::flashState.fw_size = 0;
                WebState::flashState.lastResult = "Upload exceeds MAX_FW_SIZE — file too large";
                return;
            }
            memcpy(WebState::flashState.fw_data + index, data, len);
            WebState::flashState.fw_received = index + len;
            if (final) {
                WebState::flashState.fw_size = index + len;
                Serial.printf("[Flash] Upload complete: %u bytes\n", WebState::flashState.fw_size);
            }
        });

    s_server->on("/api/flash/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.fw_size == 0) {
            req->send(400, "text/plain", "No firmware uploaded");
            return;
        }
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "Already in progress");
            return;
        }
        // Target offset — default 0 (full image with bootloader).
        // For writing into a specific partition (e.g. app0 at 0x10000
        // of an ESP32 layout) pass ?offset=0x10000 (form field "offset").
        uint32_t offset = 0;
        if (req->hasParam("offset", true)) {
            offset = strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0);
        } else if (req->hasParam("offset")) {
            offset = strtoul(req->getParam("offset")->value().c_str(), nullptr, 0);
        }
        // Guardrail: don't allow offsets that would obviously brick a 4 MB
        // receiver layout (write past end). 8 MB max is generous.
        if (offset > 0x800000) {
            req->send(400, "text/plain", "offset too large (>8 MB)");
            return;
        }
        // Set request atomically so main-loop dispatch sees a consistent
        // offset/raw/stay tuple even on dual-core reorderings.
        bool raw = req->hasParam("raw", true)
            ? req->getParam("raw", true)->value() == "1"
            : (req->hasParam("raw") && req->getParam("raw")->value() == "1");
        bool stay = req->hasParam("stay", true)
            ? req->getParam("stay", true)->value() == "1"
            : (req->hasParam("stay") && req->getParam("stay")->value() == "1");
        // via=stub → plate sends CRSF 'bl' at 420000 first, then talks SLIP
        // at 420000 to the RX's in-app esptool stub. No physical BOOT req'd.
        // Only works if RX is currently running the app (not in real ROM DFU
        // via hardware-strapped BOOT). Cannot reach bootloader @ 0x0 or
        // partition table @ 0x8000 — stub writes via esp_ota_*.
        String via;
        if (req->hasParam("via", true)) via = req->getParam("via", true)->value();
        else if (req->hasParam("via")) via = req->getParam("via")->value();
        bool via_stub = (via == "stub");
        {
            WebState::Lock lock;
            WebState::flashState.flash_offset = offset;
            WebState::flashState.flash_raw = raw;
            WebState::flashState.flash_stay = stay;
            WebState::flashState.flash_via_stub = via_stub;
            WebState::flashState.stage = "Queued";
            WebState::flashState.progress_pct = 0;
            WebState::flashState.lastResult = "";
            WebState::flashState.flash_request = true;
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "Flashing started @ 0x%x%s%s via %s",
                 (unsigned)offset,
                 raw ? " (raw)" : "",
                 stay ? " (stay-in-DFU)" : "",
                 via_stub ? "in-app stub @420000" : "ROM DFU @115200");
        req->send(200, "text/plain", msg);
    });

    s_server->on("/api/flash/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["fw_size"]      = WebState::flashState.fw_size;
        d["fw_received"]  = WebState::flashState.fw_received;
        d["in_progress"]  = WebState::flashState.in_progress;
        d["progress"]     = WebState::flashState.progress_pct;
        // Mirror under both keys — frontend uses progress_pct for slot-flash
        // polling, progress for dump polling. Keep both to avoid breaking either.
        d["progress_pct"] = WebState::flashState.progress_pct;
        d["stage"]        = WebState::flashState.stage;
        d["lastResult"]   = WebState::flashState.lastResult;
        d["result"]       = WebState::flashState.lastResult;
        d["offset"]       = WebState::flashState.flash_offset;
        d["raw"]          = WebState::flashState.flash_raw;
        d["stay"]         = WebState::flashState.flash_stay;
        d["via_stub"]     = WebState::flashState.flash_via_stub;
        d["requested"]    = WebState::flashState.flash_request;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    static struct {
        volatile bool     running;
        volatile int      progress;   // 0..100
        String            stage;
        String            error;
        uint8_t          *buf;        // PSRAM
        size_t            size;
        uint32_t          offset;
    } s_dump = {false, 0, "", "", nullptr, 0, 0};

    s_server->on("/api/flash/dump/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (s_dump.running) { req->send(409, "text/plain", "already running"); return; }
        uint32_t offset = req->hasParam("offset", true)
            ? strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0) : 0;
        size_t size = req->hasParam("size", true)
            ? strtoul(req->getParam("size", true)->value().c_str(), nullptr, 0) : 0x400000;
        if (size == 0 || size > 0x800000) {
            req->send(400, "text/plain", "size must be 1..8 MB");
            return;
        }

        // Free previous dump buffer if any
        if (s_dump.buf) { free(s_dump.buf); s_dump.buf = nullptr; }

        // Allocate in PSRAM (huge-allocate, fallback to heap if PSRAM full)
        s_dump.buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!s_dump.buf) s_dump.buf = (uint8_t*)malloc(size);
        if (!s_dump.buf) {
            req->send(500, "text/plain", "alloc failed — no PSRAM/heap for dump");
            return;
        }
        // Acquire Port B BEFORE flipping `running` so a failed acquire can't
        // leave the status endpoint reporting a non-existent dump as live.
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_dump")) {
            free(s_dump.buf); s_dump.buf = nullptr;
            s_dump.error = "Port B busy — switch Setup to Receiver";
            req->send(409, "text/plain", s_dump.error);
            return;
        }
        s_dump.size    = size;
        s_dump.offset  = offset;
        s_dump.progress = 0;
        s_dump.stage   = "Queued";
        s_dump.error   = "";
        s_dump.running = true;

        xTaskCreate([](void *) {
            ESPFlasher::Config cfg;
            cfg.uart = &Serial1;
            cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
            cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
            cfg.baud_rate = 115200;
            cfg.progress = [](int pct, const char *stage) {
                s_dump.progress = pct;
                s_dump.stage = stage;
            };
            ESPFlasher::Result r = ESPFlasher::readFlash(
                cfg, s_dump.offset, s_dump.size, s_dump.buf);
            if (r != ESPFlasher::FLASH_OK) {
                s_dump.error = ESPFlasher::errorString(r);
                s_dump.stage = "Failed";
            } else {
                s_dump.stage = "Done";
                s_dump.progress = 100;
            }
            PinPort::release(PinPort::PORT_B);
            s_dump.running = false;
            vTaskDelete(nullptr);
        }, "elrs_dump", 6144, nullptr, 1, nullptr);

        req->send(202, "text/plain", "dump started — poll /api/flash/dump/status");
    });

    s_server->on("/api/flash/dump/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["running"]   = s_dump.running;
        d["progress"]  = s_dump.progress;
        d["stage"]     = s_dump.stage;
        d["error"]     = s_dump.error;
        d["size"]      = s_dump.size;
        d["offset"]    = s_dump.offset;
        d["ready"]     = (!s_dump.running && s_dump.buf != nullptr && s_dump.error.length() == 0);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/flash/dump/download", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (s_dump.running || !s_dump.buf || s_dump.error.length() > 0) {
            req->send(409, "text/plain", "no dump available");
            return;
        }
        // Stream PSRAM buffer as attachment
        AsyncWebServerResponse *r = req->beginResponse_P(
            200, "application/octet-stream",
            (const uint8_t*)s_dump.buf, s_dump.size);
        r->addHeader("Content-Disposition", "attachment; filename=elrs_dump.bin");
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });

    s_server->on("/api/flash/erase_region", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("offset", true) || !req->hasParam("size", true)) {
            req->send(400, "text/plain", "need offset + size (both hex or dec)");
            return;
        }
        uint32_t offset = strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0);
        uint32_t size   = strtoul(req->getParam("size", true)->value().c_str(), nullptr, 0);
        // Safety cap — don't let a typo brick the whole flash.
        // Allow up to 2 MB in one call so a full app partition (1.88 MB)
        // can be erased in one SYNC session without loop-induced drops.
        if (size > 0x200000) { req->send(400, "text/plain", "size capped at 0x200000 (2 MB)"); return; }
        if (offset > 0x400000 || offset + size > 0x400000) {
            req->send(400, "text/plain", "range outside 4 MB flash");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "erase_region")) {
            req->send(409, "text/plain", "Port B busy — switch Setup to Receiver");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        ESPFlasher::Result r = ESPFlasher::eraseRegion(cfg, offset, size);
        PinPort::release(PinPort::PORT_B);
        if (r == ESPFlasher::FLASH_OK) {
            req->send(200, "text/plain",
                String("Erased 0x") + String(size, HEX) +
                " bytes @ 0x" + String(offset, HEX) +
                " — power-cycle the RX to boot.");
        } else {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
        }
    });

    s_server->on("/api/flash/dump/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (s_dump.running) { req->send(409, "text/plain", "still running"); return; }
        if (s_dump.buf) { free(s_dump.buf); s_dump.buf = nullptr; }
        s_dump.size = 0; s_dump.progress = 0; s_dump.stage = ""; s_dump.error = "";
        req->send(200, "text/plain", "cleared");
    });

    s_server->on("/api/bridge/listen", HTTP_POST, [](AsyncWebServerRequest *req) {
        uint32_t baud = req->hasParam("baud", true)
            ? strtoul(req->getParam("baud", true)->value().c_str(), nullptr, 0) : 74880;
        uint32_t ms = req->hasParam("ms", true)
            ? strtoul(req->getParam("ms", true)->value().c_str(), nullptr, 0) : 3000;
        if (ms > 8000) ms = 8000;
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "bridge_listen")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        Serial1.setRxBufferSize(4096);
        Serial1.begin(baud, SERIAL_8N1,
                      PinPort::rx_pin(PinPort::PORT_B),
                      PinPort::tx_pin(PinPort::PORT_B));
        delay(20);
        while (Serial1.available()) Serial1.read();

        // Read loop
        static uint8_t buf[3072];
        size_t got = 0;
        uint32_t deadline = millis() + ms;
        while (millis() < deadline && got < sizeof(buf)) {
            while (Serial1.available() && got < sizeof(buf)) buf[got++] = Serial1.read();
            delay(5);
        }
        Serial1.end();
        PinPort::release(PinPort::PORT_B);

        // Build response: hex followed by ASCII render (non-printable → .)
        String out;
        out.reserve(got * 4 + 64);
        out += "bytes=" + String((unsigned)got) + " baud=" + String((unsigned)baud) + "\n--- HEX ---\n";
        char h[4];
        for (size_t i = 0; i < got; i++) {
            snprintf(h, sizeof(h), "%02x ", buf[i]);
            out += h;
            if ((i % 32) == 31) out += "\n";
        }
        out += "\n--- ASCII ---\n";
        for (size_t i = 0; i < got; i++) {
            char c = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
            out += c;
        }
        req->send(200, "text/plain", out);
    });

    s_server->on("/api/flash/read_bytes", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("offset", true) || !req->hasParam("size", true)) {
            req->send(400, "text/plain", "need offset + size");
            return;
        }
        uint32_t offset = strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0);
        uint32_t size   = strtoul(req->getParam("size", true)->value().c_str(), nullptr, 0);
        if (size == 0 || size > 2048) {
            req->send(400, "text/plain", "size must be 1..2048");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "flash_read_bytes")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        uint8_t buf[2048];
        ESPFlasher::Result r = ESPFlasher::readFlash(cfg, offset, size, buf);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK) {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
            return;
        }
        String hex;
        hex.reserve(size * 2 + 16);
        char h[3];
        for (uint32_t i = 0; i < size; i++) {
            snprintf(h, sizeof(h), "%02x", buf[i]);
            hex += h;
        }
        req->send(200, "text/plain", hex);
    });

    s_server->on("/api/flash/md5", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("offset", true) || !req->hasParam("size", true)) {
            req->send(400, "text/plain", "need offset + size");
            return;
        }
        uint32_t offset = strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0);
        uint32_t size   = strtoul(req->getParam("size", true)->value().c_str(), nullptr, 0);
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "flash_md5")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        uint8_t digest[16];
        ESPFlasher::Result r = ESPFlasher::spiFlashMd5(cfg, offset, size, digest);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK) {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
            return;
        }
        char hex[33];
        for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", digest[i]);
        hex[32] = 0;
        JsonDocument d;
        d["offset"] = offset;
        d["size"]   = size;
        d["md5"]    = hex;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/flash/erase_partition", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("offset", true) || !req->hasParam("size", true)) {
            req->send(400, "text/plain", "need offset + size");
            return;
        }
        uint32_t offset = strtoul(req->getParam("offset", true)->value().c_str(), nullptr, 0);
        uint32_t size   = strtoul(req->getParam("size", true)->value().c_str(), nullptr, 0);
        uint32_t chunk  = req->hasParam("chunk", true)
            ? strtoul(req->getParam("chunk", true)->value().c_str(), nullptr, 0) : 0x10000;
        if (size > 0x200000) {
            req->send(400, "text/plain", "size > 2 MB refused");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "erase_part")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        uint32_t done = 0;
        bool ok = true;
        const char *err = nullptr;
        while (done < size && ok) {
            uint32_t n = chunk;
            if (done + n > size) n = size - done;
            ESPFlasher::Result r = ESPFlasher::eraseRegion(cfg, offset + done, n);
            if (r != ESPFlasher::FLASH_OK) { ok = false; err = ESPFlasher::errorString(r); break; }
            done += n;
            delay(5);
        }
        PinPort::release(PinPort::PORT_B);
        if (ok) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Erased %u bytes @ 0x%x in %u chunks",
                     (unsigned)done, (unsigned)offset, (unsigned)((done + chunk - 1) / chunk));
            req->send(200, "text/plain", msg);
        } else {
            req->send(500, "text/plain", err ? err : "partition erase failed");
        }
    });

    s_server->on("/api/elrs/chip_info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress || s_dump.running) {
            req->send(409, "text/plain", "flash/dump in progress — wait for it to finish");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_chip_info")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        ESPFlasher::ChipInfo info;
        ESPFlasher::Result r = ESPFlasher::chipInfo(cfg, &info);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK) {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
            return;
        }
        JsonDocument d;
        d["chip"]         = info.chip_name;
        d["magic_hex"]    = String("0x") + String(info.magic_value, HEX);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        d["mac"]          = macStr;
        bool macBlank = true;
        for (int i = 0; i < 6; i++) if (info.mac[i]) { macBlank = false; break; }
        d["mac_ok"]       = !macBlank;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/elrs/receiver_info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress || s_dump.running) {
            req->send(409, "text/plain", "flash/dump in progress");
            return;
        }
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_recv_info")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        ESPFlasher::ReceiverInfo info;
        ESPFlasher::Result r = ESPFlasher::receiverInfo(cfg, &info);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK) {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
            return;
        }

        JsonDocument d;
        // Chip
        JsonObject chip = d["chip"].to<JsonObject>();
        chip["ok"]       = info.chip.ok;
        chip["name"]     = info.chip.chip_name ? info.chip.chip_name : "";
        chip["magic_hex"] = String("0x") + String(info.chip.magic_value, HEX);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 info.chip.mac[0], info.chip.mac[1], info.chip.mac[2],
                 info.chip.mac[3], info.chip.mac[4], info.chip.mac[5]);
        chip["mac"] = macStr;
        bool macBlank = true;
        for (int i = 0; i < 6; i++) if (info.chip.mac[i]) { macBlank = false; break; }
        chip["mac_ok"] = !macBlank;

        // OTADATA
        JsonObject ota = d["otadata"].to<JsonObject>();
        ota["ok"] = info.otadata_ok;
        ota["max_seq"] = info.max_seq;
        ota["active_slot"] = info.active_slot;
        for (int i = 0; i < 2; i++) {
            JsonObject s = ota["sectors"][i].to<JsonObject>();
            s["read_ok"] = info.otadata[i].read_ok;
            s["blank"]   = info.otadata[i].blank;
            s["seq"]     = info.otadata[i].seq;
            s["state"]   = info.otadata[i].state;
            s["crc"]     = info.otadata[i].crc;
        }

        // Slots
        for (int i = 0; i < 2; i++) {
            JsonObject s = d["slots"][i].to<JsonObject>();
            s["slot"]    = i;
            s["offset"]  = String("0x") + String(info.slot[i].offset, HEX);
            s["present"] = info.slot[i].present;
            s["entry"]   = String("0x") + String(info.slot[i].entry_point, HEX);
            s["target"]  = info.slot[i].target;
            s["version_or_lua"] = info.slot[i].version_or_lua;
            s["git"]     = info.slot[i].git;
            s["product"] = info.slot[i].product;
            s["active"]  = (info.active_slot == i);
        }

        String out;
        serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // /api/elrs/enable_wifi was removed — vanilla ELRS doesn't dispatch
    // UART-received MSP locally (see docs/elrs_state_machine_research.md),
    // so MSP_ELRS_SET_RX_WIFI_MODE over this wire is architecturally a no-op.
    // To enter WiFi on the RX: 3× rapid BOOT button-press, 60 s auto-wifi
    // timer, or command via a linked handset.

    s_server->on("/api/flash/exit_dfu", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "exit_dfu")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        // ESP32-C3 ROM DFU's autobauder may have locked to either 115 200
        // (if user booted cleanly with BOOT held) or 420 000 (if an earlier
        // CRSF 'bl' frame put the RX into stub flasher). RUN_USER_CODE (0xD3)
        // is transport-identical between ROM and stub, so we try 115 200
        // first; on failure, retry at 420 000. First hit wins.
        const uint32_t bauds[] = {115200, 420000};
        ESPFlasher::Result r = ESPFlasher::FLASH_ERR_NO_SYNC;
        uint32_t used_baud = 0;
        for (uint32_t b : bauds) {
            cfg.baud_rate = b;
            r = ESPFlasher::runUserCode(cfg);
            if (r == ESPFlasher::FLASH_OK) { used_baud = b; break; }
            delay(50);
        }
        PinPort::release(PinPort::PORT_B);
        if (r == ESPFlasher::FLASH_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                "RUN_USER_CODE sent @%u — RX jumping to app", (unsigned)used_baud);
            req->send(200, "text/plain", msg);
        } else {
            req->send(500, "text/plain",
                "RUN_USER_CODE failed at both 115 200 and 420 000 — RX not in DFU/stub, "
                "or ROM autobauder wedged. Power-cycle RX to recover.");
        }
    });

    s_server->on("/api/crsf/reboot_to_bl", HTTP_POST, [](AsyncWebServerRequest *req) {
        uint32_t baud = req->hasParam("baud", true)
            ? strtoul(req->getParam("baud", true)->value().c_str(), nullptr, 0)
            : 420000;  // vanilla ELRS default; MILELRS may use 115200 — caller picks

        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf_bl")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        Serial1.end();
        Serial1.begin(baud, SERIAL_8N1,
                      PinPort::rx_pin(PinPort::PORT_B),
                      PinPort::tx_pin(PinPort::PORT_B));
        delay(50);
        // Frame: 0xEC (addr) 0x04 (len) 0x32 (type=COMMAND) 'b' 'l' CRC8(D5 over type..)
        const uint8_t frame_core[3] = { 0x32, 0x62, 0x6C };
        // CRC8 DVB-S2 poly 0xD5 over type + 2 payload bytes
        uint8_t crc = 0;
        for (int i = 0; i < 3; i++) {
            crc ^= frame_core[i];
            for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : (crc << 1);
        }
        uint8_t packet[6] = { 0xEC, 0x04, 0x32, 0x62, 0x6C, crc };
        Serial1.write(packet, 6);
        Serial1.flush();

        // Give ELRS time to process the frame + switch into serialUpdate state.
        // Telemetry task fires about every 10 ms; 150 ms is plenty. Per the
        // research doc, the in-app stub spins at `devSerialUpdate.cpp:52`
        // immediately after.
        delay(200);

        // Keep Serial1 open — caller should now POST /api/flash/* commands.
        // We don't release Port B here; the next acquire() call with same
        // mode will be a transfer, not a busy.
        JsonDocument d;
        d["sent_packet_hex"]  = "EC 04 32 62 6C";
        d["sent_crc_hex"]     = String("0x") + String(crc, HEX);
        d["baud"]             = baud;
        d["note"]             = "RX should now be in in-app esptool stub. "
                                "Try /api/otadata/status to verify SYNC works.";
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/otadata/select", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("slot", true)) {
            req->send(400, "text/plain", "need form param 'slot' = 0 or 1");
            return;
        }
        int slot = req->getParam("slot", true)->value().toInt();
        if (slot != 0 && slot != 1) {
            req->send(400, "text/plain", "slot must be 0 or 1");
            return;
        }

        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "otadata_select")) {
            req->send(409, "text/plain", "Port B busy — switch Setup to Receiver");
            return;
        }

        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        // All-in-one session: read + compute + write. Doing read (via
        // receiverInfo) and write (via flash) as SEPARATE ESPFlasher calls
        // glitches the ROM autobauder between Serial1.end/begin cycles and
        // causes a spurious "No sync" on the write's FLASH_BEGIN. This helper
        // keeps one session open for the whole transaction.
        uint32_t new_seq = 0, target_offset = 0;
        ESPFlasher::Result r = ESPFlasher::otadataSelect(cfg, slot, &new_seq, &target_offset);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK) {
            const char *msg =
                r == ESPFlasher::FLASH_ERR_NO_SYNC      ? "No sync — is RX in DFU? (hold BOOT + power-cycle)" :
                r == ESPFlasher::FLASH_ERR_READ_FAILED  ? "OTADATA read failed — RX flash chip unresponsive" :
                r == ESPFlasher::FLASH_ERR_BEGIN_FAILED ? "FLASH_BEGIN rejected during write" :
                r == ESPFlasher::FLASH_ERR_WRITE_FAILED ? "FLASH_DATA rejected during write" :
                                                          ESPFlasher::errorString(r);
            req->send(500, "text/plain", msg);
            return;
        }
        char msg[160];
        snprintf(msg, sizeof(msg),
            "OTADATA updated: seq=%u written to sector @0x%x -> boots app%d on next power-cycle",
            (unsigned)new_seq, (unsigned)target_offset, slot);
        req->send(200, "text/plain", msg);
        return;
    });

    s_server->on("/api/otadata/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "otadata_status")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        // Same fix as /api/otadata/select — read via one-session receiverInfo.
        // Avoids the inter-readFlash Serial1.end/begin glitch that desyncs the
        // ROM autobauder and falsely reports "both sectors failed".
        ESPFlasher::ReceiverInfo rinfo;
        ESPFlasher::Result r = ESPFlasher::receiverInfo(cfg, &rinfo);
        PinPort::release(PinPort::PORT_B);
        if (r != ESPFlasher::FLASH_OK || !rinfo.otadata_ok) {
            req->send(500, "text/plain",
                "readFlash failed — RX must be in DFU (hold BOOT, power-cycle)");
            return;
        }

        JsonDocument d;
        for (int i = 0; i < 2; i++) {
            JsonObject s = d["sectors"][i].to<JsonObject>();
            s["sector"]  = i;
            s["offset"]  = 0xe000 + i * 0x1000;
            s["read_ok"] = rinfo.otadata[i].read_ok;
            if (rinfo.otadata[i].read_ok) {
                s["seq"]   = rinfo.otadata[i].seq;
                s["state"] = rinfo.otadata[i].state;
                s["crc"]   = rinfo.otadata[i].crc;
                s["blank"] = rinfo.otadata[i].blank;
            }
        }
        d["max_seq"]     = rinfo.max_seq;
        d["active_slot"] = rinfo.active_slot;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

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

}  // registerRoutesFlash

}  // namespace RoutesFlash
