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
        uint32_t bytes_at_420 = 0;
        bool dfu_ok = false, stub_ok = false;

        // Test 1: ROM DFU SYNC @ 115200.
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

        // Test 2: in-app stub SYNC @ 420000 (only if DFU didn't match).
        if (!dfu_ok) {
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

        // Test 3: passive sniff @ 420000 for ~700 ms (CRSF telemetry comes at
        // ~10 ms cadence on running ELRS with a linked handset). No reply
        // required — any byte counts.
        if (!dfu_ok && !stub_ok) {
            Serial1.setRxBufferSize(1024);
            Serial1.begin(420000, SERIAL_8N1,
                          PinPort::rx_pin(PinPort::PORT_B),
                          PinPort::tx_pin(PinPort::PORT_B));
            delay(10);
            while (Serial1.available()) Serial1.read();
            uint32_t deadline = millis() + 700;
            while (millis() < deadline) {
                while (Serial1.available()) { Serial1.read(); bytes_at_420++; }
                delay(5);
            }
            Serial1.end();
            if (bytes_at_420 > 0) mode = "app";
        }

        PinPort::release(PinPort::PORT_B);

        JsonDocument d;
        d["mode"]          = mode;
        d["dfu_ok"]        = dfu_ok;
        d["stub_ok"]       = stub_ok;
        d["bytes_at_420"]  = bytes_at_420;
        d["hint"] =
            dfu_ok  ? "ROM DFU — use Flash(DFU) path; Stub-flash unavailable" :
            stub_ok ? "in-app stub already active — SYNC works @ 420000" :
            (bytes_at_420 > 0)
                    ? "running app — Stub-flash available; listen found CRSF telemetry"
                    : "silent — likely WiFi AP mode (UART disabled) OR link dropped; "
                      "reboot RX (physical or HTTP /reboot on 10.0.0.1) to return to app";
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
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

    s_server->on("/api/elrs/enable_wifi", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress || s_dump.running) {
            req->send(409, "text/plain", "flash/dump in progress");
            return;
        }
        bool inverted = req->hasParam("inverted", true)
                        ? req->getParam("inverted", true)->value() == "1"
                        : false;
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_enable_wifi")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        Serial1.end();
        Serial1.begin(420000, SERIAL_8N1,
                      PinPort::rx_pin(PinPort::PORT_B),
                      PinPort::tx_pin(PinPort::PORT_B),
                      inverted);
        delay(20);

        auto crc8d5 = [](const uint8_t *d, size_t n) {
            uint8_t c = 0;
            for (size_t i = 0; i < n; i++) {
                c ^= d[i];
                for (int b = 0; b < 8; b++) c = (c & 0x80) ? ((c << 1) ^ 0xD5) : (c << 1);
            }
            return c;
        };

        // MSP-V2 inner (7 bytes, 0-payload request for func=0x000E):
        //   [0]=0x50 status, [1]=0 flags, [2]=0x0E func_lo, [3]=0x00 func_hi,
        //   [4]=0 len_lo, [5]=0 len_hi, [6]=mspCRC over bytes [1..5]
        uint8_t msp[7] = {0x50, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x00};
        msp[6] = crc8d5(msp + 1, 5);

        // Outer CRSF (13 bytes):
        //   [0]=0xC8 addr, [1]=0x0B frame_size, [2]=0x7A type (MSP_WRITE),
        //   [3]=0xEC dest (Receiver), [4]=0xEA orig (RadioTransmitter),
        //   [5..11]=MSP packet, [12]=crsfCRC over bytes [2..11]
        uint8_t frame[13];
        frame[0] = 0xC8;   // any valid CRSF addr; RX parses dest from extended header
        frame[1] = 0x0B;   // frame_size = type + ext_header(2) + msp(7) + crc(1) = 11
        frame[2] = 0x7A;   // CRSF_FRAMETYPE_MSP_WRITE
        frame[3] = 0xEC;   // CRSF_ADDRESS_CRSF_RECEIVER
        frame[4] = 0xEA;   // CRSF_ADDRESS_RADIO_TRANSMITTER
        memcpy(frame + 5, msp, 7);
        frame[12] = crc8d5(frame + 2, 10);

        Serial1.write(frame, sizeof(frame));
        Serial1.flush();
        delay(60);
        Serial1.end();
        PinPort::release(PinPort::PORT_B);

        char hex[64];
        snprintf(hex, sizeof(hex), "MSP CRC=0x%02x, CRSF CRC=0x%02x", msp[6], frame[12]);
        JsonDocument d;
        d["frame_hex"] = "C8 0B 7A EC EA 50 00 0E 00 00 00 " + String(msp[6], HEX) + " " + String(frame[12], HEX);
        d["note"]      = "MSP_ELRS_SET_RX_WIFI_MODE sent. RX should raise SoftAP in ~700 ms.";
        d["ap_ssid"]   = "ExpressLRS RX";
        d["ap_pwd"]    = "expresslrs";
        d["ap_url"]    = "http://10.0.0.1";
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/flash/exit_dfu", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "exit_dfu")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        ESPFlasher::Result r = ESPFlasher::runUserCode(cfg);
        PinPort::release(PinPort::PORT_B);
        if (r == ESPFlasher::FLASH_OK) {
            req->send(200, "text/plain", "RUN_USER_CODE sent — RX jumping to app");
        } else {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
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

        // 1) Read both OTADATA sectors (32 B records at 0xe000 and 0xf000)
        uint8_t sec[2][32];
        bool read_ok[2] = {false, false};
        for (int i = 0; i < 2; i++) {
            ESPFlasher::Result r = ESPFlasher::readFlash(cfg, 0xe000 + i * 0x1000, 32, sec[i]);
            read_ok[i] = (r == ESPFlasher::FLASH_OK);
        }
        if (!read_ok[0] && !read_ok[1]) {
            PinPort::release(PinPort::PORT_B);
            req->send(500, "text/plain",
                "readFlash failed both sectors — is RX in DFU? (hold BOOT, power-cycle)");
            return;
        }

        // Parse seq from each sector (0xFFFFFFFF if blank/unreadable)
        uint32_t seq0 = read_ok[0] ? *(uint32_t*)&sec[0][0] : 0xFFFFFFFF;
        uint32_t seq1 = read_ok[1] ? *(uint32_t*)&sec[1][0] : 0xFFFFFFFF;
        uint32_t max_seq = 0;
        if (seq0 != 0xFFFFFFFF) max_seq = seq0;
        if (seq1 != 0xFFFFFFFF && seq1 > max_seq) max_seq = seq1;

        // Choose new_seq to select target slot. If desired parity already
        // matches current max, bump by 1; otherwise bump by 2.
        uint32_t new_seq = max_seq + 1;
        int current_selected_slot = ((new_seq - 1) & 1);
        if (current_selected_slot != slot) new_seq++;
        // Safety: never allow wrap-around (near UINT32_MAX = probably corrupt)
        if (new_seq == 0 || new_seq > 0x7fffffff) {
            PinPort::release(PinPort::PORT_B);
            req->send(500, "text/plain", "OTADATA seq out of range");
            return;
        }

        // Build new record: seq (4), label (20 zeros), state=0xFFFFFFFF (4),
        // crc (4) = crc32_le init 0xFFFFFFFF over 4-byte seq.
        uint8_t rec[32];
        memset(rec, 0, 32);
        *(uint32_t*)&rec[0] = new_seq;
        *(uint32_t*)&rec[24] = 0xFFFFFFFF;
        // CRC32-LE with init 0xFFFFFFFF — standard ethernet/zlib polynomial.
        uint32_t crc = 0xFFFFFFFF;
        for (int j = 0; j < 4; j++) {
            crc ^= rec[j];
            for (int k = 0; k < 8; k++) {
                crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
            }
        }
        crc ^= 0xFFFFFFFF;
        *(uint32_t*)&rec[28] = crc;

        // Write to sector that currently holds the LOWER seq (alternating
        // writes = standard ESP-IDF OTA behaviour, preserves rollback).
        uint32_t target_offset = (seq0 <= seq1) ? 0xe000 : 0xf000;
        cfg.flash_offset = target_offset;
        ESPFlasher::Result wr = ESPFlasher::flash(cfg, rec, 32);

        PinPort::release(PinPort::PORT_B);

        if (wr != ESPFlasher::FLASH_OK) {
            req->send(500, "text/plain",
                String("OTADATA write failed: ") + ESPFlasher::errorString(wr));
            return;
        }

        char msg[160];
        snprintf(msg, sizeof(msg),
            "OTADATA updated: seq=%u written to sector @0x%x -> boots app%d on next power-cycle",
            (unsigned)new_seq, (unsigned)target_offset, slot);
        req->send(200, "text/plain", msg);
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

        uint8_t sec[2][32];
        bool ok[2] = {false, false};
        for (int i = 0; i < 2; i++) {
            ok[i] = (ESPFlasher::readFlash(cfg, 0xe000 + i * 0x1000, 32, sec[i])
                     == ESPFlasher::FLASH_OK);
        }
        PinPort::release(PinPort::PORT_B);
        if (!ok[0] && !ok[1]) {
            req->send(500, "text/plain",
                "readFlash failed — RX must be in DFU (hold BOOT, power-cycle)");
            return;
        }

        JsonDocument d;
        uint32_t max_seq = 0; int slot = -1;
        for (int i = 0; i < 2; i++) {
            JsonObject s = d["sectors"][i].to<JsonObject>();
            s["sector"] = i;
            s["offset"] = 0xe000 + i * 0x1000;
            s["read_ok"] = ok[i];
            if (ok[i]) {
                uint32_t seq = *(uint32_t*)&sec[i][0];
                uint32_t state = *(uint32_t*)&sec[i][24];
                uint32_t crc = *(uint32_t*)&sec[i][28];
                s["seq"] = seq;
                s["state"] = state;
                s["crc"] = crc;
                s["blank"] = (seq == 0xFFFFFFFF);
                if (seq != 0xFFFFFFFF && seq > max_seq) { max_seq = seq; }
            }
        }
        d["max_seq"] = max_seq;
        d["active_slot"] = (max_seq == 0) ? 0 : ((max_seq - 1) & 1);
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
