#pragma once
#include <Arduino.h>

// Minimal ESP ROM bootloader flasher for ESP8266/ESP8285 (ELRS receivers)
// Implements esptool.py SYNC + FLASH_BEGIN + FLASH_DATA + FLASH_END protocol over UART
//
// Target device must already be in bootloader mode (GPIO0 low during boot)
// User puts receiver in DFU manually via button

namespace ESPFlasher {

typedef void (*ProgressCallback)(int percent, const char* stage);

enum Result {
    FLASH_OK = 0,
    FLASH_ERR_NO_SYNC,       // failed to sync with bootloader
    FLASH_ERR_BEGIN_FAILED,  // FLASH_BEGIN rejected
    FLASH_ERR_WRITE_FAILED,  // FLASH_DATA rejected
    FLASH_ERR_END_FAILED,    // FLASH_END rejected
    FLASH_ERR_TIMEOUT,
    FLASH_ERR_INVALID_INPUT,
    FLASH_ERR_READ_FAILED,   // READ_FLASH rejected or malformed response
};

struct Config {
    HardwareSerial *uart;    // UART to receiver
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate = 115200;   // initial baud for sync
    uint32_t flash_baud = 460800;  // higher baud after sync (optional)
    uint32_t flash_offset = 0;     // flash offset address (0x0 for full image)
    bool stay_in_loader = false;   // true = send FLASH_END(1) to stay in DFU so caller can chain more operations
    bool invert_uart   = false;    // true = inverted CRSF (vanilla ELRS RX default for FC-side UART)
    ProgressCallback progress = nullptr;
};

// Sample region for in-session readback verify. Populated by flash() when
// provided — read back AFTER the FLASH_DATA loop but BEFORE Serial1.end(),
// so the RX never loses DFU between write and verify.
struct Sample {
    uint32_t offset;    // flash offset (absolute)
    uint32_t size;      // bytes to read (<=256)
    uint8_t  data[256]; // filled by flash() on success
    bool     ok;        // true if this sample was read without error
};

Result flash(const Config &cfg, const uint8_t *data, size_t size,
             Sample *samples = nullptr, size_t n_samples = 0);

// Read `size` bytes from flash at `offset` on an attached ESP32-C3 / S2 / S3
// receiver that's already in ROM bootloader mode. Destination buffer must
// be at least `size` bytes (typically in PSRAM for multi-MB dumps).
// Uses CMD_READ_FLASH (0xD2) — streams block_size packets with flow-control
// acks, terminated by a 16-byte MD5 trailer (ignored here).
Result readFlash(const Config &cfg, uint32_t offset, size_t size, uint8_t *out);

// Sticky DFU session ---------------------------------------------------------
// Lets multiple operations share a single ROM DFU session, avoiding the
// ESP32-C3 ROM autobauder quirk where Serial1.end()+begin() between calls
// fails the second SLIP sync (BUG-ID1).
//
// Lifecycle:
//   1. openSession(cfg, flash_size) — UART begin + sync + spiAttach + spiSetParams
//   2. issue *InOpenSession() helpers in any order
//   3. closeSession() — UART end (or auto-close on idle timeout)
//
// All non-session helpers (readFlash, chipInfo, etc.) keep their self-
// contained begin/sync/end semantics — they're refactored to call open/close
// internally so existing callers don't change. The InOpenSession variants
// just send SLIP commands assuming the session is live.
Result openSession(const Config &cfg, uint32_t flash_size_bytes = 0x400000);
void   closeSession();
bool   sessionOpen();
void   touchSession();                 // mark recent activity
bool   sessionIdleSince(uint32_t ms);  // true if open AND inactive ≥ ms

// Upload the embedded esptool stub flasher matching the chip identified by
// `chip_magic` (return value from chipInfoInOpenSession). After this, the
// session is "stub-mode" — the same SLIP protocol still works, but commands
// hit the in-RAM stub which is robust to mixed READ_REG/READ_FLASH chains
// where bare ROM falls over (BUG-ID2 on ESP32-C3).
//
// Must be called inside an open session, BEFORE any READ_FLASH chains. Stubs
// are embedded in src/bridge/esp_rom_stubs.h (auto-generated from esptool
// JSONs by tools/gen_stubs.py).
//
// Returns FLASH_OK on success and sets sessionStubLoaded()=true. Returns
// FLASH_ERR_INVALID_INPUT if no stub matches the magic.
Result loadStub(uint32_t chip_magic);
bool   sessionStubLoaded();

// Read multiple non-contiguous regions in a SINGLE ROM DFU session.
//
// Rationale: ESP32-C3 ROM's autobauder latches on first sync; subsequent
// Serial1.end()+begin()+sync cycles (which plain readFlash() does per call)
// fail with spurious FLASH_ERR_NO_SYNC on the second call. This helper
// opens Serial1 + syncs + spiAttach/spiSetParams ONCE, then issues a
// READ_FLASH_SLOW loop per region, then closes Serial1 once at the end.
//
// Same semantics as readFlash: each region filled into region.dst, CHUNK=64,
// ROM CMD_READ_FLASH_SLOW (0x0E), PSRAM-safe.
struct ReadRegion {
    uint32_t offset;
    uint32_t size;
    uint8_t *dst;
};
Result readFlashMulti(const Config &cfg, const ReadRegion *regions, size_t n);

// Same as readFlashMulti, but assumes openSession() is already in effect.
// Caller does NOT pass a Config — session is global. Used by the sticky-
// session route handlers so chip_info / identity / md5 / erase chain on
// the same Serial1 + sync.
Result readFlashMultiInOpenSession(const ReadRegion *regions, size_t n);

// Erase `size` bytes starting at `offset` on the attached ROM bootloader.
// Size is rounded up to the nearest 4 KB sector internally by the ROM.
// Uses the standard CMD_FLASH_BEGIN path with a zero-body CMD_FLASH_END
// afterwards so the ROM commits the erase and returns control. Useful
// for surgical operations like wiping an OTA-select sector to force boot
// into the secondary app.
Result eraseRegion(const Config &cfg, uint32_t offset, size_t size);

// Multi-region variant of eraseRegion. Syncs ONCE, erases N regions inside
// a single ROM session, ends ONCE. Same defence-in-depth as readFlashMulti
// against the ESP32-C3 ROM autobauder latching after the first sync —
// looped eraseRegion() calls would Serial1.end()/begin() between each, and
// the second sync historically fails. Use this when erasing >1 region in
// the same operation (e.g. erasing a full app partition in 64 KB chunks).
struct EraseRegion {
    uint32_t offset;
    uint32_t size;
};
Result eraseRegionMulti(const Config &cfg, const EraseRegion *regions, size_t n);
Result eraseRegionMultiInOpenSession(const EraseRegion *regions, size_t n);

// Tell the stub/ROM to exit and run user code (boots the OTADATA-selected
// app partition). Opcode CMD_RUN_USER_CODE (0xD3). Works on both the ELRS
// in-app stub and ROM bootloader.
Result runUserCode(const Config &cfg);

// Compute MD5 of `size` bytes at `offset` on the attached flash using
// the ROM/stub SPI_FLASH_MD5 command (0x13). Fills 16-byte out digest.
// ~600× faster than full-readback for verifying a flash write integrity.
// Works on ESP32-S2/S3/C3 ROM (and all stubs). Does not touch flash.
Result spiFlashMd5(const Config &cfg, uint32_t offset, uint32_t size, uint8_t out[16]);
Result spiFlashMd5InOpenSession(uint32_t offset, uint32_t size, uint8_t out[16]);

// Detect attached chip. Syncs + issues READ_REG against the SoC-identifying
// registers (UART_DATE + EFUSE_BASE + MAC words) to derive chip family, MAC,
// and a best-effort flash size (via manufacturer ID).
// All fields except `ok` are zero-initialised; unknown fields stay 0.
struct ChipInfo {
    bool     ok;
    uint32_t magic_value;      // UART_DATE register — family discriminator
    const char *chip_name;     // "ESP32-C3" / "ESP32-S3" / "ESP8266" / "unknown"
    uint8_t  mac[6];           // base MAC from EFUSE (all 0 if read failed)
    uint32_t flash_id;         // JEDEC id from SPI flash (0 if read failed)
    uint32_t flash_size;       // derived from JEDEC (0 if unknown)
};
Result chipInfo(const Config &cfg, ChipInfo *out);
Result chipInfoInOpenSession(ChipInfo *out);

// Read a single 32-bit register over the open SLIP session. Convenience
// for callers that want to peek eFuse / GPIO / specific MMIO without
// rolling their own CMD_READ_REG. Must be called inside an open session.
bool readRegInOpenSession(uint32_t addr, uint32_t *val);

// Send FLASH_END(reboot=true) inside an open session — used to make RX
// boot into the freshly-flashed image without closing the session in
// flash() itself. Idempotent failure (some ROMs don't ack FLASH_END).
void flashEndInOpenSession(bool reboot);

// Send the CRSF "reboot to bootloader" command frame (EC 04 32 62 6C 0A) at
// the given baud. On ESP32-C3 + ELRS 3.x this switches the RX from the app
// into its in-app esptool stub flasher on the SAME UART at the SAME baud —
// no physical BOOT + power-cycle required. Caller must then talk SLIP on
// the configured pins at that baud.
// Returns FLASH_OK unconditionally (TX-only, no ack).
Result sendCrsfReboot(const Config &cfg);

// DEVICE_PING (CRSF 0x28) → DEVICE_INFO (0x29) probe. Sends a broadcast ping
// at cfg.baud_rate and waits up to `timeout_ms` for the RX to reply with its
// identity packet. Cleanest way to detect "ELRS app is alive" — responds only
// if serialUpdate/wifiUpdate/radioFailed aren't active.
//
// On success, fills the caller's ElrsDeviceInfo struct:
//   - name: product name string (e.g. "BAYCKRC C3 …")
//   - serial_no: u32 from payload (often 0)
//   - hw_id: u32
//   - sw_version: u32 (big-endian in payload; parsed)
//   - field_count: number of LUA parameter entries available
// Returns FLASH_OK on reply, FLASH_ERR_READ_FAILED on timeout.
struct ElrsDeviceInfo {
    bool     ok;
    char     name[64];
    uint32_t serial_no;
    uint32_t hw_id;
    uint32_t sw_version;
    uint8_t  field_count;
    uint8_t  parameter_version;
};
Result crsfDevicePing(const Config &cfg, uint32_t timeout_ms, ElrsDeviceInfo *out);

// PING the TX module via the radio link. CRSF DEVICE_PING with dest=0xEE
// (handset address). RX forwards to its bound TX module over the OTA uplink;
// TX module replies with DEVICE_INFO src=0xEE which RX forwards back to FC
// UART. Returns the TX-side device info (name, version, etc).
Result crsfPingTxModule(const Config &cfg, uint32_t timeout_ms, ElrsDeviceInfo *out);

// MSP_ELRS_SET_RX_WIFI_MODE — wraps MSP function 0x0E in MSP-over-CRSF
// (frame type 0x7C MSP_WRITE). ELRS RX defers 500 ms then calls
// setWifiUpdateMode() which broadcasts WiFi AP "ExpressLRS RX". Lets the
// plate trigger WiFi mode without 60 s auto-AP timeout or 3× BOOT-press.
Result sendMspWifiMode(const Config &cfg);

// CRSF COMMAND m,m,<id> — sets the active model-match ID. id=255 (0xFF)
// means "match any handset model" — RX accepts link from any model.
// id=0..63 = restrict to handset model with that exact ID (mismatch
// rejects link). Default ELRS behaviour is 0xFF.
Result sendCrsfModelMatch(const Config &cfg, uint8_t modelId);

// Synthesize standard CRSF telemetry frames. RX forwards them over the
// radio link to TX → handset → OSD. Useful for handset-side display
// testing without a real flight controller.
Result sendBatteryTelemetry(const Config &cfg, uint16_t voltage_mV,
                             uint16_t current_mA, uint32_t consumed_mAh,
                             uint8_t pct);
Result sendGpsTelemetry(const Config &cfg, int32_t lat_e7, int32_t lon_e7,
                         uint16_t gnd_speed_e2, uint16_t heading_e2,
                         uint16_t alt_m, uint8_t sats);
Result sendAttitudeTelemetry(const Config &cfg, int16_t pitch_e4,
                              int16_t roll_e4, int16_t yaw_e4);

// Send CRSF "enter binding" frame (EC 04 32 62 64 <crc>). Safe runtime op —
// puts RX into bind mode for 60 s. TX-only, no ack expected.
Result sendCrsfBind(const Config &cfg);

// LUA parameter read (CRSF PARAMETER_READ 0x2C). Sends the 8-byte request
// frame for (field_id, chunk=0) and captures up to max_bytes of the
// PARAMETER_SETTINGS_ENTRY (0x2B) reply starting from the body of the first
// chunk (i.e. starting right after field_id + chunks_remaining — so
// out_buf[0]=parent, out_buf[1]=type, out_buf[2..]=name\0+typedata).
// *out_len = bytes written, *chunks_remaining = chunks still to fetch.
// Timeout ~200 ms. Returns FLASH_OK if reply matched, FLASH_ERR_READ_FAILED
// otherwise.
Result crsfParamRead(const Config &cfg, uint8_t field_id, uint8_t chunk,
                     uint8_t *out_buf, size_t max_bytes, size_t *out_len,
                     uint8_t *chunks_remaining);

// LUA parameter write (CRSF PARAMETER_WRITE 0x2D). Sends the field_id +
// data_len payload bytes, no reply expected. For numeric fields data is
// 1-4 bytes big-endian; for TEXT_SELECTION 1 byte index; for COMMAND 1
// byte (lcsClick=1 fires the action).
Result crsfParamWrite(const Config &cfg, uint8_t field_id, const uint8_t *data, uint8_t data_len);

// Full dual-slot identity read + OTADATA in one Serial1 session. RX must be
// in DFU. Reads OTADATA sectors, then the first 16 KB of app0 (@0x10000) and
// app1 (@0x1f0000) to extract target/version/git/etc baked into seg0 rodata.
// Avoids losing DFU between separate readFlash calls. One PinPort+begin per
// request.
struct SlotIdentity {
    bool     present;               // first byte is 0xE9 (valid ESP image magic)
    uint32_t offset;                // absolute flash offset (e.g. 0x10000)
    uint32_t entry_point;           // ESP image entry addr
    char     target[48];            // e.g. "UNIFIED_ESP32C3_LR1121_RX"
    char     version_or_lua[32];    // varies: "3.5.3" / "MILELRS_v348" / etc
    char     git[12];               // short hash like "40555e"
    char     product[96];           // "ExpressLRS RX" / "Unified" / etc — before magic
    uint32_t first_nonff_byte;      // end-of-image estimate (partition-relative, 0 if unknown)
};
struct OtadataSector {
    uint32_t seq;
    uint32_t state;
    uint32_t crc;
    bool     read_ok;
    bool     blank;                 // all 0xFF (uninitialised)
};
struct ReceiverInfo {
    bool          chip_ok;
    ChipInfo      chip;
    bool          otadata_ok;
    OtadataSector otadata[2];
    int           active_slot;      // -1 if none / both blank; else 0 or 1
    uint32_t      max_seq;
    SlotIdentity  slot[2];          // [0] = app0 @ 0x10000, [1] = app1 @ 0x1f0000
};
Result receiverInfo(const Config &cfg, ReceiverInfo *out);

// Read + modify + write OTADATA in a SINGLE Serial1 session. RX must be in
// ROM DFU (or in-app stub at matching baud). Reads both 32 B sectors at
// 0xe000/0xf000, computes a new seq so that `desired_slot` boots next,
// writes the new record into the lower-seq sector (alternating-sector
// algorithm per esp-idf OTA). Returns the seq actually written plus the
// sector offset for diagnostics.
//
// Combines read + write in one session because doing two separate
// ESPFlasher::readFlash + ESPFlasher::flash calls glitches the ROM
// autobauder (Serial1.end/begin cycle between them) and causes a spurious
// "No sync" on the second SLIP handshake.
Result otadataSelect(const Config &cfg, int desired_slot,
                     uint32_t *out_new_seq, uint32_t *out_target_offset);

const char* errorString(Result r);

} // namespace ESPFlasher
