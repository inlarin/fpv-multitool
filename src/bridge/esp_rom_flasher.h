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

// Erase `size` bytes starting at `offset` on the attached ROM bootloader.
// Size is rounded up to the nearest 4 KB sector internally by the ROM.
// Uses the standard CMD_FLASH_BEGIN path with a zero-body CMD_FLASH_END
// afterwards so the ROM commits the erase and returns control. Useful
// for surgical operations like wiping an OTA-select sector to force boot
// into the secondary app.
Result eraseRegion(const Config &cfg, uint32_t offset, size_t size);

// Tell the stub/ROM to exit and run user code (boots the OTADATA-selected
// app partition). Opcode CMD_RUN_USER_CODE (0xD3). Works on both the ELRS
// in-app stub and ROM bootloader.
Result runUserCode(const Config &cfg);

// Compute MD5 of `size` bytes at `offset` on the attached flash using
// the ROM/stub SPI_FLASH_MD5 command (0x13). Fills 16-byte out digest.
// ~600× faster than full-readback for verifying a flash write integrity.
// Works on ESP32-S2/S3/C3 ROM (and all stubs). Does not touch flash.
Result spiFlashMd5(const Config &cfg, uint32_t offset, uint32_t size, uint8_t out[16]);

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

const char* errorString(Result r);

} // namespace ESPFlasher
