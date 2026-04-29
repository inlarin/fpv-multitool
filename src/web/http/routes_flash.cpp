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
#include "../../crsf/crsf_service.h"
#include "../../crsf/crsf_config.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <MD5Builder.h>

// Cross-TU debug ring — global scope (NOT inside RoutesFlash namespace) so
// esp_rom_flasher.cpp's `extern char *g_dfu_debug_buf;` finds it. Used by
// the stub-protocol diagnostics in readFlashStream(). Static buffer below.
static char s_dfu_debug_storage[256] = {0};
char *g_dfu_debug_buf = s_dfu_debug_storage;
size_t g_dfu_debug_len = 0;

// Static state used by the dump endpoints. Inside a function-scope static so
// lambdas capture it; moved out here verbatim from WebServer::start().
namespace RoutesFlash {

// Max firmware upload size — larger than any realistic ELRS image but well
// within our 8 MB PSRAM. 2 MB covers 4.0.0 + future builds with room to spare.
static const size_t MAX_FW_SIZE = 2 * 1024 * 1024;

static void flashProgress(int pct, const char* stage) {
    // Atomic 2-field update — WS broadcast snapshot used to pull pct + stage
    // separately and could observe (new pct, old stage). Now in one Lock.
    WebState::flashState.setProgress(pct, stage);
}

// =====================================================================
// CRSF auto-pause / resume helpers — every ELRS-or-OTADATA op that needs
// Port B in UART mode must call crsfPauseForPortB() before acquire() and
// crsfResumeAfterPortB(was_running) after release() (or on acquire fail).
// State-changing ops (flash, dump, otadata_select, erase_region) skip the
// resume — RX has rebooted/changed mode and stale CRSF would just confuse
// the live monitor's "connected" state. Quick read-only ops (probe,
// device_info, params, chip_info, recv_info, otadata_status) DO resume so
// the user's live monitor session survives an incidental probe.
//
// Baud (420000) and inversion (WebState::crsf.isInverted()) match what
// /api/crsf/start would do, so resume is bit-for-bit equivalent.
// =====================================================================
static bool crsfPauseForPortB() {
    if (!CRSFService::isRunning()) return false;
    Serial.println("[ELRS] CRSF service running — pausing for Port B");
    CRSFService::end();
    CRSFConfig::reset();
    WebState::crsf.markStopped();
    PinPort::release(PinPort::PORT_B);
    delay(50);  // settle
    return true;
}

static void crsfResumeAfterPortB(bool was_running) {
    if (!was_running) return;
    // Best-effort: if Port B got grabbed by another consumer between
    // release() above and here, log and leave CRSF stopped rather than
    // block the response. User can hit Start again from the CRSF tab.
    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf_resume")) {
        Serial.println("[ELRS] CRSF resume: Port B busy — leaving stopped");
        return;
    }
    bool inv = WebState::crsf.isInverted();
    CRSFService::begin(&Serial1,
                       PinPort::rx_pin(PinPort::PORT_B),
                       PinPort::tx_pin(PinPort::PORT_B),
                       420000, inv);
    CRSFConfig::init();
    WebState::crsf.markStarted(inv);
    Serial.println("[ELRS] CRSF service resumed after Port B op");
}

// =====================================================================
// Sticky DFU session — shared by /api/elrs/dfu/begin, /end, /status and
// any DFU-mode endpoint (chip_info, identity/fast, erase_partition,
// flash/md5). When a session is open, the session owns Port B and the
// ESPFlasher Serial1+sync state across HTTP calls — so the user can chain
// chip_info → identity/fast → erase_partition without cycling the RX into
// DFU between each call (BUG-ID1: ESP32-C3 ROM autobauder fails the second
// sync after a Serial1.end()/begin() cycle).
//
// State is owned here, not in ESPFlasher: ESPFlasher just exposes
// open/close/InOpenSession primitives. The route layer tracks PinPort
// ownership + whether CRSF needs to resume on /end + when the session
// started (for the idle watchdog).
// =====================================================================
static bool     s_dfu_session_active        = false;
static bool     s_dfu_session_crsf_was_run  = false;
static uint32_t s_dfu_session_opened_ms     = 0;
// Cached chip identity captured at /dfu/begin. ESP32-C3 ROM autobauder
// gets unhappy when READ_REG (chip_info) is issued AFTER a long
// READ_FLASH_SLOW chain inside the same Serial1 session — sync() inside
// the session also fails to recover. So we read chip_info once at
// /dfu/begin (right after the fresh sync) and serve it from cache for the
// rest of the session. This keeps /api/elrs/chip_info responsive even if
// the user already pulled identity/fast.
static ESPFlasher::ChipInfo s_dfu_session_chip = {};

// (Debug ring globals defined in this file's global-scope block above.)

// 60 s of inactivity → auto-close. Long enough to cover human reaction
// time between successive curl/UI calls but short enough that an abandoned
// session frees Port B for CRSF before the user gets confused.
static const uint32_t DFU_SESSION_IDLE_TIMEOUT_MS = 60000;

// Acquire Port B + open ESPFlasher session for an endpoint. If a sticky
// /dfu/begin session is already in effect, reuse it (no PinPort/CRSF churn).
// On false, the response has already been sent.
//
// `*out_was_paused` indicates whether THIS CALL paused CRSF and is therefore
// responsible for resuming it on its own release. When sticky is in effect,
// it's set to false — the /dfu/begin handler owns CRSF resume.
static bool dfuPortAcquireOrSticky(AsyncWebServerRequest *req, const char *who,
                                   bool *out_was_paused) {
    if (s_dfu_session_active) {
        ESPFlasher::touchSession();
        *out_was_paused = false;
        return true;
    }
    *out_was_paused = crsfPauseForPortB();
    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, who)) {
        crsfResumeAfterPortB(*out_was_paused);
        req->send(409, "text/plain", "Port B busy");
        return false;
    }
    return true;
}

static void dfuPortReleaseOrSticky(bool was_paused) {
    if (s_dfu_session_active) return;
    PinPort::release(PinPort::PORT_B);
    crsfResumeAfterPortB(was_paused);
}

// Idle watchdog — called from RoutesFlash::tick() in the web loop.
static void dfuSessionTimeoutCheck() {
    if (!s_dfu_session_active) return;
    if (!ESPFlasher::sessionIdleSince(DFU_SESSION_IDLE_TIMEOUT_MS)) return;
    Serial.printf("[DFU] sticky session idle %u ms — auto-closing\n",
                  (unsigned)DFU_SESSION_IDLE_TIMEOUT_MS);
    ESPFlasher::closeSession();
    s_dfu_session_active = false;
    PinPort::release(PinPort::PORT_B);
    crsfResumeAfterPortB(s_dfu_session_crsf_was_run);
    s_dfu_session_crsf_was_run = false;
}

void tick() {
    dfuSessionTimeoutCheck();
}

// Non-static — declared in routes_flash.h as RoutesFlash::executeFlash()
// for the main-loop dispatcher. File-local would clash with namespace decl.
void executeFlash() {
    WebState::flashState.markFlashStart();

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
        WebState::flashState.setProgress(0, "Decompressing");
        size_t out_size = 0;
        decompressed = FirmwareUnpack::gunzip(fw_ptr, fw_size, &out_size);
        if (!decompressed) {
            WebState::flashState.markFlashFailed("Gzip decompress failed");
            return;
        }
        fw_ptr = decompressed;
        fw_size = out_size;
    } else if (fmt == FirmwareUnpack::FMT_ELRS) {
        size_t out_size = 0;
        const uint8_t *extracted = FirmwareUnpack::extractELRS(fw_ptr, fw_size, &out_size);
        if (!extracted) {
            WebState::flashState.markFlashFailed("ELRS container parse failed");
            return;
        }
        fw_ptr = extracted;
        fw_size = out_size;
    } else if (fmt != FirmwareUnpack::FMT_RAW_BIN) {
        WebState::flashState.markFlashFailed("Unknown firmware format (not .bin/.gz/.elrs)");
        return;
    }

    Serial.printf("[Flash] Flashing %u bytes\n", fw_size);
    WebState::flashState.setProgress(0, "Starting");

    // RX state changes during flash (reboot into new firmware) — discard the
    // resume signal. User restarts CRSF Live monitor manually after flash.
    (void)crsfPauseForPortB();

    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_flash")) {
        WebState::flashState.markFlashFailed("Port B busy — switch to UART");
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

    // The 5/5 sample verify happens INSIDE flash() before FLASH_END, so it
    // works on bare ROM regardless of post-write quirks. Post-write MD5 is a
    // bonus check that ESP32-C3 ROM doesn't reliably support (FLASH_END
    // auto-resets, SPI re-init doesn't recover, stub-on-flash rejects
    // FLASH_BEGIN). Soft-warn on MD5 read failure; rely on the sample check.
    ESPFlasher::Result r = ESPFlasher::flash(cfg, fw_ptr, fw_size, samples, N_SAMPLES);

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

    // Full-image MD5 verify on top of the 5-sample spot-check. Catches
    // corruption between samples that the spot-check would miss. Reuses the
    // open ROM session — must run BEFORE PinPort::release. ~3 s extra at
    // 115200 for 1.2 MB; we already paid 30-60 s on the flash itself, so the
    // overhead is in noise. On stub flash (via_stub=true) the SPI_FLASH_MD5
    // command works the same — stub passes it through to ROM.
    if (r == ESPFlasher::FLASH_OK) {
        WebState::flashState.stage = "Verifying MD5";
        uint8_t remote_md5[16];
        ESPFlasher::Result mr = ESPFlasher::spiFlashMd5(
            cfg, cfg.flash_offset, (uint32_t)fw_size, remote_md5);
        if (mr == ESPFlasher::FLASH_OK) {
            MD5Builder md5;
            md5.begin();
            md5.add((uint8_t*)fw_ptr, fw_size);
            md5.calculate();
            uint8_t local_md5[16];
            md5.getBytes(local_md5);
            if (memcmp(remote_md5, local_md5, 16) == 0) {
                verifyMsg += " · MD5 ✓";
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    " · MD5 MISMATCH (local %02x%02x%02x… vs remote %02x%02x%02x…)",
                    local_md5[0], local_md5[1], local_md5[2],
                    remote_md5[0], remote_md5[1], remote_md5[2]);
                verifyMsg += buf;
                r = ESPFlasher::FLASH_ERR_WRITE_FAILED;
            }
        } else {
            // MD5 read failed but samples passed — soft warning, don't fail.
            verifyMsg += String(" · MD5 read failed (") +
                         ESPFlasher::errorString(mr) + ")";
        }
    }

    PinPort::release(PinPort::PORT_B);

    if (decompressed) free(decompressed);

    // Free the uploaded firmware buffer after flashing — successful or not
    // (user can re-upload to try again). markFlashComplete is atomic so the
    // WS broadcast can't observe (in_progress=false, fw_data=stale).
    WebState::flashState.markFlashComplete(
        String(ESPFlasher::errorString(r)) + verifyMsg);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_rx_mode")) {
            crsfResumeAfterPortB(_crsf);
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
        // Try non-inverted first (most ELRS forks/MILELRS), then inverted
        // (vanilla 3.5.x default — RX outputs inverted CRSF for FC-side).
        if (!dfu_ok) {
            delay(50);
            ESPFlasher::Config cfg;
            cfg.uart = &Serial1;
            cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
            cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
            cfg.baud_rate = 420000;
            cfg.invert_uart = false;
            if (ESPFlasher::crsfDevicePing(cfg, 250, &devInfo) == ESPFlasher::FLASH_OK && devInfo.ok) {
                mode = "app";
            } else {
                // Retry with inverted CRSF
                delay(50);
                cfg.invert_uart = true;
                if (ESPFlasher::crsfDevicePing(cfg, 250, &devInfo) == ESPFlasher::FLASH_OK && devInfo.ok) {
                    mode = "app";
                }
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
        crsfResumeAfterPortB(_crsf);

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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_device_info")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_params")) {
            crsfResumeAfterPortB(_crsf);
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
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_param_w")) {
            crsfResumeAfterPortB(_crsf);
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
                crsfResumeAfterPortB(_crsf);
                req->send(400, "text/plain", "unsupported param type for write");
                return;
        }
        ESPFlasher::crsfParamWrite(cfg, id, payload, payload_len);
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(_crsf);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_bind")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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
                // Atomic: free old fw_data + clear fw_size/received/lastResult.
                WebState::flashState.resetForUpload();

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
                WebState::flashState.markUploadRejected(
                    "Upload exceeds MAX_FW_SIZE — file too large");
                return;
            }
            // Fork-specific format rejects — first chunk of a new upload.
            // These headers are known non-flashable containers we've
            // classified in research/. We reject here rather than letting
            // ESPFlasher discover the problem 30 s into a flash.
            //   0x11 0xE8 0xC6 0xCA — ZLRS SubGHz AES-encrypted variant
            //   0x71 0x9F 0x96 0xE6 — Foxeer 400 MHz AES-encrypted variant
            // Standard ESP image magic is 0xE9; neither ZLRS RX/TX nor
            // vanilla ELRS normal flows start with these bytes.
            if (index == 0 && len >= 4) {
                const uint8_t b0 = data[0], b1 = data[1], b2 = data[2], b3 = data[3];
                bool zlrs_enc = (b0 == 0x11 && b1 == 0xE8 && b2 == 0xC6 && b3 == 0xCA);
                bool foxeer_enc = (b0 == 0x71 && b1 == 0x9F && b2 == 0x96 && b3 == 0xE6);
                if (zlrs_enc || foxeer_enc) {
                    WebState::flashState.markUploadRejected(
                        zlrs_enc
                          ? "ZLRS SubGHz encrypted container — used non-ENC variant"
                          : "ZLRS/Foxeer 400 MHz encrypted container — use non-ENC variant");
                    return;  // quietly drop further chunks; POST handler returns 413
                }
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
        // Atomic — sets flash_* params + queue trio (stage/progress/lastResult)
        // + flash_request=true in one Lock so the main-loop dispatcher can't
        // pick up flash_request before the offset is set.
        WebState::flashState.queueFlash(offset, raw, stay, via_stub);
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
        // Pause CRSF before acquiring Port B. Long-running async op via xTask:
        // resume only if acquire fails — once xTask owns the port, lambda
        // returns and we can't safely resume CRSF until the dump completes.
        // User restarts CRSF Live monitor manually after dump.
        bool _crsf = crsfPauseForPortB();
        // Acquire Port B BEFORE flipping `running` so a failed acquire can't
        // leave the status endpoint reporting a non-existent dump as live.
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_dump")) {
            crsfResumeAfterPortB(_crsf);
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
        // Erase reaches into RX flash — RX is in DFU and will need power-cycle
        // afterwards. Pause CRSF (no resume — RX state changes).
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "erase_region")) {
            crsfResumeAfterPortB(_crsf);  // restore on the early-fail path only
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "bridge_listen")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);

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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "flash_read_bytes")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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
        bool _crsf;
        if (!dfuPortAcquireOrSticky(req, "flash_md5", &_crsf)) return;
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        uint8_t digest[16];
        ESPFlasher::Result r = ESPFlasher::spiFlashMd5(cfg, offset, size, digest);
        dfuPortReleaseOrSticky(_crsf);
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

    // ==== ELRS Identity: fast-read (NVS 20 KB + OTADATA 8 KB + active-app tail 8 KB) ====
    // One DFU session, returns concatenated hex blobs as text/plain with a
    // section header per blob. Client-side JS parses NVS v2 format + searches
    // app-tail rodata for the ELRSOPTS JSON block. Total wire: ~73 KB hex.
    // Takes ~4-6 s at 115200 via CMD_READ_FLASH_SLOW.
    s_server->on("/api/elrs/identity/fast", HTTP_POST, [](AsyncWebServerRequest *req) {
        // Optional ?spiffs=1 — adds the SPIFFS partition region (default
        // ELRS layout offset 0x3d0000, 192 KB) to the same readFlashMulti
        // call. Adds ~16 s at 12 KB/s but extracts options.json (TX UID,
        // runtime WiFi STA creds) — Phase 1c stretch. Adding it to the
        // SAME readFlashMulti rather than a separate call avoids BUG-ID1
        // (the ROM autobauder won't re-sync after Serial1 cycles within
        // the same RX boot session).
        bool wantSpiffs = req->hasParam("spiffs", true)
            ? (req->getParam("spiffs", true)->value().toInt() != 0)
            : false;
        bool _crsf;
        if (!dfuPortAcquireOrSticky(req, "identity_fast", &_crsf)) return;
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        // Phase 1b: single-session multi-region read via readFlashMulti().
        //   Region 1: partition table @ 0x8000,  4 KB  (for Phase M1 layout decode)
        //   Region 2: NVS + OTADATA   @ 0x9000, 28 KB  (contiguous on standard ELRS)
        //   Region 3: active-app tail             8 KB  (for ELRSOPTS JSON + ZLRS sigs)
        // Active slot decoded from a preliminary 8-byte peek of OTADATA sectors,
        // but we don't have that yet — so we always read BOTH app tails. Cost:
        // 16 KB extra, ~1.3 s at 12 KB/s. Worth it to keep the endpoint
        // agnostic of which slot is active.
        const uint32_t PT_OFF    = 0x8000;
        const uint32_t PT_SZ     = 0x1000;
        const uint32_t NVS_OFF   = 0x9000;
        const uint32_t NVS_SZ    = 0x5000;
        const uint32_t OTA_OFF   = 0xe000;   (void)OTA_OFF;
        const uint32_t OTA_SZ    = 0x2000;
        const uint32_t NVS_OTA   = 0x7000;   // 28 KB contiguous (NVS + OTADATA)
        const uint32_t TAIL_SZ   = 0x2000;   // 8 KB
        const uint32_t APP0_END  = 0x1f0000; // Layout A — Phase M1 will replace
        const uint32_t APP1_END  = 0x3d0000; //            from partition table
        // Default ELRS RX (ESP32-C3 4 MB) places SPIFFS / LittleFS right
        // after app1. 192 KB covers the typical layout; if the partition
        // is smaller we just over-read empty 0xFF bytes (no harm). For
        // non-default layouts the extracted JSON parser will simply find
        // nothing — frontend falls back to "options.json not found".
        const uint32_t SPIFFS_OFF = 0x3d0000;
        const uint32_t SPIFFS_SZ  = wantSpiffs ? 0x30000 : 0;  // 192 KB or off
        const uint32_t BUF_SZ    = PT_SZ + NVS_OTA + 2 * TAIL_SZ + SPIFFS_SZ;
        uint8_t *buf = (uint8_t *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM);
        if (!buf) {
            dfuPortReleaseOrSticky(_crsf);
            req->send(500, "text/plain", "PSRAM alloc failed");
            return;
        }

        ESPFlasher::ReadRegion regions[5] = {
            { PT_OFF,            PT_SZ,   buf },
            { NVS_OFF,           NVS_OTA, buf + PT_SZ },
            { APP0_END - TAIL_SZ, TAIL_SZ, buf + PT_SZ + NVS_OTA },
            { APP1_END - TAIL_SZ, TAIL_SZ, buf + PT_SZ + NVS_OTA + TAIL_SZ },
            { SPIFFS_OFF,        SPIFFS_SZ, buf + PT_SZ + NVS_OTA + 2 * TAIL_SZ },
        };
        size_t n_regions = wantSpiffs ? 5 : 4;
        ESPFlasher::Result r = ESPFlasher::readFlashMulti(cfg, regions, n_regions);
        if (r != ESPFlasher::FLASH_OK) {
            heap_caps_free(buf);
            dfuPortReleaseOrSticky(_crsf);
            req->send(500, "text/plain",
                String("Flash read failed: ") + ESPFlasher::errorString(r));
            return;
        }
        // OTADATA slot decode (first 4 bytes of each 4 KB sector = seq u32).
        uint8_t *ota_base = buf + PT_SZ + NVS_SZ;
        uint32_t seq0 = 0, seq1 = 0;
        memcpy(&seq0, ota_base,          4);
        memcpy(&seq1, ota_base + 0x1000, 4);
        int active_slot = 0;
        uint32_t max_seq = 0;
        if (seq0 != 0xFFFFFFFF) { max_seq = seq0; active_slot = (seq0 - 1) & 1; }
        if (seq1 != 0xFFFFFFFF && seq1 > max_seq) {
            max_seq = seq1; active_slot = (seq1 - 1) & 1;
        }
        dfuPortReleaseOrSticky(_crsf);

        // Build chunked text response: header line + hex per section. 5 sections:
        //   PT       @ 0x8000  — 4 KB  partition table (for Phase M1)
        //   NVS      @ 0x9000  — 20 KB NVS partition
        //   OTADATA  @ 0xe000  — 8 KB
        //   APP0TAIL @ 0x1e8000 — 8 KB (for ELRSOPTS + ZLRS sigs)
        //   APP1TAIL @ 0x3c8000 — 8 KB
        //
        // Chunked generator: each call emits up to max_len bytes starting at
        // `index`. Section boundaries are computed per-call; state lives only
        // in `index` + Ctx (the PSRAM buf). When we emit the final byte, we
        // free the Ctx inside the generator.
        struct Ctx { uint8_t *buf; int active_slot; uint32_t max_seq;
                     uint32_t app0_tail_off; uint32_t app1_tail_off;
                     uint32_t spiffs_off; uint32_t spiffs_sz; };
        Ctx *ctx = new Ctx{ buf, active_slot, max_seq,
                            APP0_END - TAIL_SZ, APP1_END - TAIL_SZ,
                            SPIFFS_OFF, SPIFFS_SZ };
        AsyncWebServerResponse *resp = req->beginChunkedResponse("text/plain",
            [ctx](uint8_t *dst, size_t max_len, size_t index) -> size_t {
                const uint32_t PT_SZ_L   = 0x1000;
                const uint32_t NVS_SZ_L  = 0x5000;
                const uint32_t OTA_SZ_L  = 0x2000;
                const uint32_t TAIL_SZ_L = 0x2000;
                const uint32_t SP_SZ_L   = ctx->spiffs_sz;  // 0 when ?spiffs not requested
                char hdr_pt[96], hdr_nvs[96], hdr_ota[96], hdr_a0[96], hdr_a1[96], hdr_sp[96];
                int hp = snprintf(hdr_pt,  sizeof(hdr_pt),
                    "# section=PT off=0x8000 size=0x1000\n");
                int hn = snprintf(hdr_nvs, sizeof(hdr_nvs),
                    "\n# section=NVS off=0x9000 size=0x5000 active_slot=%d max_seq=%u\n",
                    ctx->active_slot, (unsigned)ctx->max_seq);
                int ho = snprintf(hdr_ota, sizeof(hdr_ota),
                    "\n# section=OTADATA off=0xe000 size=0x2000\n");
                int ha0 = snprintf(hdr_a0, sizeof(hdr_a0),
                    "\n# section=APP0TAIL off=0x%x size=0x2000\n",
                    (unsigned)ctx->app0_tail_off);
                int ha1 = snprintf(hdr_a1, sizeof(hdr_a1),
                    "\n# section=APP1TAIL off=0x%x size=0x2000\n",
                    (unsigned)ctx->app1_tail_off);
                int hsp = SP_SZ_L ? snprintf(hdr_sp, sizeof(hdr_sp),
                    "\n# section=SPIFFS off=0x%x size=0x%x\n",
                    (unsigned)ctx->spiffs_off, (unsigned)SP_SZ_L) : 0;
                // Running offsets through the response stream.
                const size_t E_pt_h   = hp;
                const size_t E_pt     = E_pt_h  + PT_SZ_L   * 2;
                const size_t E_nvs_h  = E_pt    + hn;
                const size_t E_nvs    = E_nvs_h + NVS_SZ_L  * 2;
                const size_t E_ota_h  = E_nvs   + ho;
                const size_t E_ota    = E_ota_h + OTA_SZ_L  * 2;
                const size_t E_a0_h   = E_ota   + ha0;
                const size_t E_a0     = E_a0_h  + TAIL_SZ_L * 2;
                const size_t E_a1_h   = E_a0    + ha1;
                const size_t E_a1     = E_a1_h  + TAIL_SZ_L * 2;
                const size_t E_sp_h   = E_a1    + hsp;
                const size_t E_sp     = E_sp_h  + SP_SZ_L   * 2;
                const size_t TOTAL    = (SP_SZ_L ? E_sp : E_a1) + 1;

                // Buffer byte offsets per section:
                //   PT      @ buf + 0
                //   NVS     @ buf + PT_SZ_L
                //   OTADATA @ buf + PT_SZ_L + NVS_SZ_L
                //   APP0    @ buf + PT_SZ_L + NVS_SZ_L + OTA_SZ_L
                //   APP1    @ buf + PT_SZ_L + NVS_SZ_L + OTA_SZ_L + TAIL_SZ_L
                //   SPIFFS  @ buf + PT_SZ_L + NVS_SZ_L + OTA_SZ_L + 2*TAIL_SZ_L
                const size_t B_nvs = PT_SZ_L;
                const size_t B_ota = B_nvs + NVS_SZ_L;
                const size_t B_a0  = B_ota + OTA_SZ_L;
                const size_t B_a1  = B_a0  + TAIL_SZ_L;
                const size_t B_sp  = B_a1  + TAIL_SZ_L;

                if (index >= TOTAL) {
                    heap_caps_free(ctx->buf);
                    delete ctx;
                    return 0;
                }
                size_t out = 0;
                static const char HEX_TAB[] = "0123456789abcdef";
                while (out < max_len && index + out < TOTAL) {
                    size_t pos = index + out;
                    size_t remaining = max_len - out;
                    if (pos < E_pt_h) {
                        size_t take = min(E_pt_h - pos, remaining);
                        memcpy(dst + out, hdr_pt + pos, take); out += take;
                    } else if (pos < E_pt) {
                        size_t hp2 = pos - E_pt_h;
                        uint8_t b = ctx->buf[hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else if (pos < E_nvs_h) {
                        size_t take = min(E_nvs_h - pos, remaining);
                        memcpy(dst + out, hdr_nvs + (pos - E_pt), take); out += take;
                    } else if (pos < E_nvs) {
                        size_t hp2 = pos - E_nvs_h;
                        uint8_t b = ctx->buf[B_nvs + hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else if (pos < E_ota_h) {
                        size_t take = min(E_ota_h - pos, remaining);
                        memcpy(dst + out, hdr_ota + (pos - E_nvs), take); out += take;
                    } else if (pos < E_ota) {
                        size_t hp2 = pos - E_ota_h;
                        uint8_t b = ctx->buf[B_ota + hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else if (pos < E_a0_h) {
                        size_t take = min(E_a0_h - pos, remaining);
                        memcpy(dst + out, hdr_a0 + (pos - E_ota), take); out += take;
                    } else if (pos < E_a0) {
                        size_t hp2 = pos - E_a0_h;
                        uint8_t b = ctx->buf[B_a0 + hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else if (pos < E_a1_h) {
                        size_t take = min(E_a1_h - pos, remaining);
                        memcpy(dst + out, hdr_a1 + (pos - E_a0), take); out += take;
                    } else if (pos < E_a1) {
                        size_t hp2 = pos - E_a1_h;
                        uint8_t b = ctx->buf[B_a1 + hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else if (SP_SZ_L && pos < E_sp_h) {
                        size_t take = min(E_sp_h - pos, remaining);
                        memcpy(dst + out, hdr_sp + (pos - E_a1), take); out += take;
                    } else if (SP_SZ_L && pos < E_sp) {
                        size_t hp2 = pos - E_sp_h;
                        uint8_t b = ctx->buf[B_sp + hp2/2];
                        dst[out++] = (hp2 & 1) ? HEX_TAB[b & 0xf] : HEX_TAB[b >> 4];
                    } else {
                        dst[out++] = '\n';
                    }
                }
                return out;
            });
        req->send(resp);
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
        // Erases an entire partition — RX state changes; don't resume CRSF.
        bool _crsf;
        if (!dfuPortAcquireOrSticky(req, "erase_part", &_crsf)) return;
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        // Build chunk list and erase via single ROM session — was a loop of
        // eraseRegion() calls (one Serial1 cycle per chunk) which would have
        // hit the autobauder-latch issue from BUG-ID1 on the second iteration
        // for partitions > one chunk. eraseRegionMulti syncs once and runs
        // FLASH_BEGIN+FLASH_END pairs per chunk inside the open session.
        const size_t MAX_CHUNKS = 64;  // 64 × 64 KB = 4 MB upper bound
        ESPFlasher::EraseRegion regions[MAX_CHUNKS];
        size_t n_regions = 0;
        for (uint32_t done = 0; done < size && n_regions < MAX_CHUNKS; ) {
            uint32_t n = chunk;
            if (done + n > size) n = size - done;
            regions[n_regions++] = { offset + done, n };
            done += n;
        }
        ESPFlasher::Result r = ESPFlasher::eraseRegionMulti(cfg, regions, n_regions);
        // Erase changes RX state — drop CRSF resume by passing `false`. If
        // a sticky session is in effect, dfuPortReleaseOrSticky is a no-op
        // (the sticky owner stays in DFU until /dfu/end).
        dfuPortReleaseOrSticky(false);
        if (r == ESPFlasher::FLASH_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Erased %u bytes @ 0x%x in %u chunks",
                     (unsigned)size, (unsigned)offset, (unsigned)n_regions);
            req->send(200, "text/plain", msg);
        } else {
            req->send(500, "text/plain", ESPFlasher::errorString(r));
        }
    });

    // ---------- Sticky DFU session ----------
    // POST /api/elrs/dfu/begin  → opens ROM DFU session, holds Port B + Serial1
    //                              across subsequent DFU-mode endpoints. Returns
    //                              chip_info JSON. Idempotent if already open.
    // POST /api/elrs/dfu/end    → closes session, releases Port B, resumes CRSF.
    // GET  /api/elrs/dfu/status → JSON: open/uptime_ms.
    //
    // Session auto-closes after DFU_SESSION_IDLE_TIMEOUT_MS of inactivity (the
    // ESPFlasher last-use timestamp is bumped on every InOpenSession call).
    s_server->on("/api/elrs/dfu/begin", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress || s_dump.running) {
            req->send(409, "text/plain", "flash/dump in progress");
            return;
        }
        // ?stub=1 → upload esptool stub after sync. Stub is robust to mixed
        // READ_REG/READ_FLASH/MD5 chains (BUG-ID2 workaround) — recommended
        // for the sticky-session pattern. Default OFF for backward compat
        // with callers that don't need the stub.
        bool wantStub = req->hasParam("stub", true)
            ? (req->getParam("stub", true)->value().toInt() != 0)
            : false;

        if (s_dfu_session_active) {
            // Idempotent: report state of the already-open session. Use
            // cached chip_info — fresh chipInfoInOpenSession() against ROM
            // might fail if a READ_FLASH chain has run since /dfu/begin.
            const ESPFlasher::ChipInfo &ci = s_dfu_session_chip;
            JsonDocument d;
            d["session"]    = "already_open";
            d["uptime_ms"]  = millis() - s_dfu_session_opened_ms;
            d["chip"]       = ci.chip_name ? ci.chip_name : "unknown";
            d["magic_hex"]  = String("0x") + String(ci.magic_value, HEX);
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                     ci.mac[0], ci.mac[1], ci.mac[2], ci.mac[3], ci.mac[4], ci.mac[5]);
            d["mac"] = macStr;
            d["stub_loaded"] = ESPFlasher::sessionStubLoaded();
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
            return;
        }
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "dfu_session")) {
            crsfResumeAfterPortB(_crsf);
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;
        ESPFlasher::Result r = ESPFlasher::openSession(cfg);
        if (r != ESPFlasher::FLASH_OK) {
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(500, "text/plain",
                String("DFU sync failed: ") + ESPFlasher::errorString(r));
            return;
        }
        s_dfu_session_active       = true;
        s_dfu_session_crsf_was_run = _crsf;
        s_dfu_session_opened_ms    = millis();
        Serial.println("[DFU] sticky session opened");

        ESPFlasher::ChipInfo ci;
        ESPFlasher::chipInfoInOpenSession(&ci);
        s_dfu_session_chip = ci;

        // Optional stub upload. Done BEFORE any READ_FLASH chain so the
        // ROM is still in a clean post-sync state. If no embedded stub
        // matches the detected chip, return a hint but keep the session.
        bool stub_attempted = false, stub_ok = false;
        String stub_msg;
        if (wantStub) {
            stub_attempted = true;
            ESPFlasher::Result sr = ESPFlasher::loadStub(ci.magic_value);
            stub_ok = (sr == ESPFlasher::FLASH_OK);
            if (sr == ESPFlasher::FLASH_ERR_INVALID_INPUT) {
                stub_msg = "no embedded stub for this chip";
            } else if (!stub_ok) {
                stub_msg = String("stub load failed: ") + ESPFlasher::errorString(sr);
            } else {
                stub_msg = "stub running";
                Serial.println("[DFU] stub flasher loaded");
            }
        }

        JsonDocument d;
        d["session"]    = "opened";
        d["uptime_ms"]  = 0;
        d["chip"]       = ci.chip_name;
        d["magic_hex"]  = String("0x") + String(ci.magic_value, HEX);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ci.mac[0], ci.mac[1], ci.mac[2], ci.mac[3], ci.mac[4], ci.mac[5]);
        d["mac"] = macStr;
        bool macBlank = true;
        for (int i = 0; i < 6; i++) if (ci.mac[i]) { macBlank = false; break; }
        d["mac_ok"] = !macBlank;
        d["stub_loaded"]    = stub_ok;
        if (stub_attempted) d["stub_msg"] = stub_msg;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/elrs/dfu/end", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!s_dfu_session_active) {
            req->send(200, "text/plain", "no session open");
            return;
        }
        ESPFlasher::closeSession();
        s_dfu_session_active = false;
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(s_dfu_session_crsf_was_run);
        s_dfu_session_crsf_was_run = false;
        Serial.println("[DFU] sticky session closed by /end");
        req->send(200, "text/plain", "session closed");
    });

    s_server->on("/api/elrs/dfu/debug", HTTP_GET, [](AsyncWebServerRequest *req) {
        String s = "";
        if (g_dfu_debug_len > 0)
            s = String((const char *)g_dfu_debug_buf).substring(0, g_dfu_debug_len);
        req->send(200, "text/plain", s.length() ? s : String("(no debug data)"));
    });

    s_server->on("/api/elrs/dfu/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument d;
        d["open"] = s_dfu_session_active;
        if (s_dfu_session_active) {
            d["uptime_ms"]       = millis() - s_dfu_session_opened_ms;
            d["idle_timeout_ms"] = DFU_SESSION_IDLE_TIMEOUT_MS;
            d["stub_loaded"]     = ESPFlasher::sessionStubLoaded();
            d["chip"]            = s_dfu_session_chip.chip_name
                                       ? s_dfu_session_chip.chip_name : "unknown";
        }
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/elrs/chip_info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress || s_dump.running) {
            req->send(409, "text/plain", "flash/dump in progress — wait for it to finish");
            return;
        }
        // Sticky session WITHOUT stub: serve cached chip_info captured at
        // /dfu/begin. Live READ_REG against the ROM after a READ_FLASH_SLOW
        // chain is unreliable on ESP32-C3 — see s_dfu_session_chip comment.
        // With stub loaded, the stub responds reliably to all commands, so
        // we fall through to the live path for fresh data.
        if (s_dfu_session_active && !ESPFlasher::sessionStubLoaded()) {
            ESPFlasher::touchSession();
            const ESPFlasher::ChipInfo &info = s_dfu_session_chip;
            JsonDocument d;
            d["chip"]      = info.chip_name ? info.chip_name : "unknown";
            d["magic_hex"] = String("0x") + String(info.magic_value, HEX);
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                     info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
            d["mac"]       = macStr;
            bool macBlank = true;
            for (int i = 0; i < 6; i++) if (info.mac[i]) { macBlank = false; break; }
            d["mac_ok"]    = !macBlank;
            d["cached"]    = true;  // hint for the UI / debugging
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
            return;
        }
        bool _crsf;
        if (!dfuPortAcquireOrSticky(req, "elrs_chip_info", &_crsf)) return;
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        ESPFlasher::ChipInfo info;
        ESPFlasher::Result r = ESPFlasher::chipInfo(cfg, &info);
        dfuPortReleaseOrSticky(_crsf);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_recv_info")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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

    // /api/elrs/wifi_mode_enter NOT available — verified against ELRS 3.6.3
    // src/lib/Telemetry/telemetry.cpp lines 270-289: UART-received MSP frames
    // are routed ONLY to wifi2tcp/mspVtx forwarding, NOT dispatched locally
    // to MspReceiveComplete() where MSP_ELRS_SET_RX_WIFI_MODE handler lives.
    // Path is OTA-only — radio uplink from TX module to RX, then RX-internal
    // dispatch. From the FC UART side, this MSP is architecturally a no-op.
    // Workaround: 3× rapid BOOT button-press, 60 s auto-wifi timer (if
    // wifi-on-interval > 0), or send via a linked handset.

    // POST /api/elrs/modelmatch?id=N — sets active model-match ID. id=255
    // (0xFF) = match any handset model (default ELRS behaviour). id=0..63 =
    // restrict to handset model with that exact ID. Setting id=0 was a bug
    // earlier — that means "match only model 0" not "match all".
    s_server->on("/api/elrs/modelmatch", HTTP_POST, [](AsyncWebServerRequest *req) {
        uint8_t id = 0;
        if (req->hasParam("id", true)) id = (uint8_t)req->getParam("id", true)->value().toInt();
        else if (req->hasParam("id")) id = (uint8_t)req->getParam("id")->value().toInt();
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_modelmatch")) {
            crsfResumeAfterPortB(_crsf);
            req->send(409, "text/plain", "Port B busy"); return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;
        ESPFlasher::sendCrsfModelMatch(cfg, id);
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(_crsf);
        char msg[64]; snprintf(msg, sizeof(msg), "model-match id set to %u", id);
        req->send(200, "text/plain", msg);
    });

    // POST /api/elrs/tx_info — reads the bound TX module's DEVICE_INFO via
    // radio link (PING dest=0xEE → forwarded to TX → reply forwarded back).
    s_server->on("/api/elrs/tx_info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress"); return;
        }
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_tx_info")) {
            crsfResumeAfterPortB(_crsf);
            req->send(409, "text/plain", "Port B busy"); return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;
        ESPFlasher::ElrsDeviceInfo info;
        ESPFlasher::Result r = ESPFlasher::crsfPingTxModule(cfg, 800, &info);
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(_crsf);
        if (r != ESPFlasher::FLASH_OK || !info.ok) {
            req->send(502, "text/plain", "TX did not reply — check link is up");
            return;
        }
        JsonDocument d;
        d["ok"] = true;
        d["name"] = info.name;
        d["serial_no"] = info.serial_no;
        d["hw_id"] = info.hw_id;
        d["sw_version"] = info.sw_version;
        d["field_count"] = info.field_count;
        d["parameter_version"] = info.parameter_version;
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // POST /api/elrs/inject_telemetry?type=battery|gps|attitude — synthesizes
    // a telemetry frame from the FC side. RX forwards it over the OTA uplink
    // to TX → handset → OSD. Useful for handset-side display testing without
    // a real flight controller.
    s_server->on("/api/elrs/inject_telemetry", HTTP_POST, [](AsyncWebServerRequest *req) {
        String t = req->hasParam("type", true) ? req->getParam("type", true)->value()
                  : req->hasParam("type") ? req->getParam("type")->value() : "battery";
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_inject")) {
            crsfResumeAfterPortB(_crsf);
            req->send(409, "text/plain", "Port B busy"); return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 420000;
        auto pi = [&](const char *k, int def) {
            if (req->hasParam(k, true)) return (int)req->getParam(k, true)->value().toInt();
            if (req->hasParam(k))       return (int)req->getParam(k)->value().toInt();
            return def;
        };
        const char *msg = "ok";
        if (t == "battery") {
            ESPFlasher::sendBatteryTelemetry(cfg,
                (uint16_t)pi("voltage_mv", 16800),
                (uint16_t)pi("current_ma", 5000),
                (uint32_t)pi("consumed_mah", 0),
                (uint8_t)pi("pct", 75));
            msg = "battery frame queued for handset OSD";
        } else if (t == "gps") {
            ESPFlasher::sendGpsTelemetry(cfg,
                pi("lat_e7", 555512345),
                pi("lon_e7", 376123456),
                (uint16_t)pi("gnd_speed", 0),
                (uint16_t)pi("heading", 0),
                (uint16_t)pi("alt_m", 100),
                (uint8_t)pi("sats", 12));
            msg = "GPS frame queued";
        } else if (t == "attitude") {
            ESPFlasher::sendAttitudeTelemetry(cfg,
                (int16_t)pi("pitch_e4", 0),
                (int16_t)pi("roll_e4",  0),
                (int16_t)pi("yaw_e4",   0));
            msg = "attitude frame queued";
        } else {
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(400, "text/plain", "type must be battery|gps|attitude"); return;
        }
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(_crsf);
        req->send(200, "text/plain", msg);
    });

    // POST /api/elrs/nvs/info — single-session DFU read of the partition
    // table at 0x8000 (4 KB) + auto-located NVS partition contents. Returns
    // partitions JSON + NVS bytes as base64. Frontend parses NVS pages,
    // finds 'eeprom' blob, decodes ELRS rx_config_t. RX must be in DFU.
    s_server->on("/api/elrs/nvs/info", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (WebState::flashState.in_progress) {
            req->send(409, "text/plain", "flash in progress"); return;
        }
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_nvs_info")) {
            crsfResumeAfterPortB(_crsf);
            req->send(409, "text/plain", "Port B busy"); return;
        }
        ESPFlasher::Config cfg;
        cfg.uart = &Serial1;
        cfg.tx_pin = PinPort::tx_pin(PinPort::PORT_B);
        cfg.rx_pin = PinPort::rx_pin(PinPort::PORT_B);
        cfg.baud_rate = 115200;

        // Open one DFU session, do both reads, close.
        ESPFlasher::Result r = ESPFlasher::openSession(cfg);
        if (r != ESPFlasher::FLASH_OK) {
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(502, "text/plain", "DFU sync failed — RX must be in DFU (BOOT + power-cycle)");
            return;
        }

        // Read partition table 4 KB at 0x8000 + a temporary buffer for NVS.
        // We need to know NVS size before alloc — but standard partition table
        // is bounded. Read partition table first.
        uint8_t parttable[4096];
        ESPFlasher::ReadRegion r1 = { 0x8000, sizeof(parttable), parttable };
        if (ESPFlasher::readFlashMultiInOpenSession(&r1, 1) != ESPFlasher::FLASH_OK) {
            ESPFlasher::closeSession();
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(502, "text/plain", "Failed reading partition table");
            return;
        }

        // Parse to find NVS (type=DATA=1, subtype=NVS=2). Magic is 0x50AA LE.
        uint32_t nvs_off = 0, nvs_size = 0;
        JsonDocument parts_doc;
        JsonArray parts_arr = parts_doc.to<JsonArray>();
        for (int i = 0; i < 16; i++) {
            const uint8_t *e = parttable + i * 32;
            uint16_t magic = e[0] | (e[1] << 8);
            if (magic != 0x50AA) {
                if (magic == 0xEBEB) break;  // MD5 trailer
                continue;
            }
            uint8_t  type    = e[2];
            uint8_t  subtype = e[3];
            uint32_t off     = e[4] | (e[5] << 8) | (e[6] << 16) | (e[7] << 24);
            uint32_t size    = e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24);
            char label[17] = {0};
            memcpy(label, e + 12, 16);
            JsonObject p = parts_arr.add<JsonObject>();
            p["idx"] = i;
            p["type"] = type; p["subtype"] = subtype;
            p["offset"] = off; p["size"] = size;
            p["label"] = label;
            if (type == 1 && subtype == 2) {  // DATA / NVS
                nvs_off = off; nvs_size = size;
            }
        }
        if (!nvs_off || nvs_size == 0 || nvs_size > 64 * 1024) {
            ESPFlasher::closeSession();
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(502, "text/plain", "NVS partition not found in table");
            return;
        }

        // Read full NVS partition into PSRAM (up to 64 KB worth of buffer).
        uint8_t *nvs = (uint8_t*)heap_caps_malloc(nvs_size, MALLOC_CAP_SPIRAM);
        if (!nvs) nvs = (uint8_t*)malloc(nvs_size);
        if (!nvs) {
            ESPFlasher::closeSession();
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(500, "text/plain", "Out of memory");
            return;
        }
        ESPFlasher::ReadRegion r2 = { nvs_off, nvs_size, nvs };
        if (ESPFlasher::readFlashMultiInOpenSession(&r2, 1) != ESPFlasher::FLASH_OK) {
            free(nvs);
            ESPFlasher::closeSession();
            PinPort::release(PinPort::PORT_B);
            crsfResumeAfterPortB(_crsf);
            req->send(502, "text/plain", "Failed reading NVS");
            return;
        }
        ESPFlasher::closeSession();
        PinPort::release(PinPort::PORT_B);
        crsfResumeAfterPortB(_crsf);

        // Build JSON: partitions + NVS bytes as base64.
        // Pre-compute base64 buffer size (4 chars per 3 bytes, padded).
        size_t b64_len = ((nvs_size + 2) / 3) * 4;
        char *b64 = (char*)malloc(b64_len + 1);
        if (!b64) { free(nvs); req->send(500, "text/plain", "OOM"); return; }
        static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t bi = 0;
        for (size_t i = 0; i < nvs_size; i += 3) {
            uint32_t n = (uint32_t)nvs[i] << 16;
            if (i + 1 < nvs_size) n |= (uint32_t)nvs[i + 1] << 8;
            if (i + 2 < nvs_size) n |= (uint32_t)nvs[i + 2];
            b64[bi++] = b64chars[(n >> 18) & 0x3F];
            b64[bi++] = b64chars[(n >> 12) & 0x3F];
            b64[bi++] = (i + 1 < nvs_size) ? b64chars[(n >> 6) & 0x3F] : '=';
            b64[bi++] = (i + 2 < nvs_size) ? b64chars[n & 0x3F] : '=';
        }
        b64[b64_len] = 0;
        free(nvs);

        // Build response. Use AsyncResponseStream for big strings.
        AsyncResponseStream *resp = req->beginResponseStream("application/json");
        resp->print("{\"partitions\":");
        String partsJson; serializeJson(parts_doc, partsJson);
        resp->print(partsJson);
        resp->print(",\"nvs_offset\":"); resp->print(nvs_off);
        resp->print(",\"nvs_size\":"); resp->print(nvs_size);
        resp->print(",\"nvs_b64\":\"");
        resp->print(b64);
        resp->print("\"}");
        free(b64);
        req->send(resp);
    });

    s_server->on("/api/flash/exit_dfu", HTTP_POST, [](AsyncWebServerRequest *req) {
        // RX state changes (DFU → app jump). Don't resume CRSF — user
        // restarts Live monitor manually once the new app boots.
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "exit_dfu")) {
            crsfResumeAfterPortB(_crsf);
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

        // Sends RX into in-app stub flasher — RX state changes; don't resume.
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf_bl")) {
            crsfResumeAfterPortB(_crsf);
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

    // "Soft reboot RX while running app". Works in BOTH states:
    //  - If live monitor is already running, route through the service
    //    (it owns Port B; don't contend).
    //  - Otherwise acquire Port B one-shot, send the CRSF COMMAND, release.
    // Frame: 0xEC 0x06 0x32 0xEC 0xEA 0x0A 0x0B CRC8(D5 over type+dst+src+subcmd+action)
    s_server->on("/api/crsf/reboot_app", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (CRSFService::isRunning()) {
            CRSFService::cmdReboot();
            req->send(200, "text/plain", "Reboot command sent via live monitor");
            return;
        }
        uint32_t baud = req->hasParam("baud", true)
            ? strtoul(req->getParam("baud", true)->value().c_str(), nullptr, 0)
            : 420000;
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf_reboot")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        Serial1.end();
        Serial1.begin(baud, SERIAL_8N1,
                      PinPort::rx_pin(PinPort::PORT_B),
                      PinPort::tx_pin(PinPort::PORT_B));
        delay(50);
        // ELRS ENTER_BOOTLOADER action. ELRS firmware interprets this as
        // "restart into bootloader"; for a plain "soft reboot of running
        // app" we send the same COMMAND — the app naturally cycles through
        // its reset handler on the way to bl and most forks return to app
        // if no further traffic arrives within ~2 s.
        // Ext frame: dst=ADDR_RECEIVER(0xEC), src=ADDR_RADIO(0xEA)
        // type=0x32 COMMAND, subcmd=0x0A ELRS_FUNC, action=0x0B REBOOT
        const uint8_t payload[] = { 0x32, 0xEC, 0xEA, 0x0A, 0x0B };
        uint8_t crc = 0;
        for (size_t i = 0; i < sizeof(payload); i++) {
            crc ^= payload[i];
            for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : (crc << 1);
        }
        uint8_t frame[8] = { 0xEC, 0x06, 0x32, 0xEC, 0xEA, 0x0A, 0x0B, crc };
        Serial1.write(frame, sizeof(frame));
        Serial1.flush();
        delay(120);
        PinPort::release(PinPort::PORT_B);
        req->send(200, "text/plain", "Reboot command sent (RX will drop link briefly)");
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

        // Boot-slot flip — on next power-cycle RX boots a different app;
        // CRSF baud / mode may change. Don't auto-resume.
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "otadata_select")) {
            crsfResumeAfterPortB(_crsf);
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
        bool _crsf = crsfPauseForPortB();
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "otadata_status")) {
            crsfResumeAfterPortB(_crsf);
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
        crsfResumeAfterPortB(_crsf);
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
