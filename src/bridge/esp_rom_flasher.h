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
    ProgressCallback progress = nullptr;
};

Result flash(const Config &cfg, const uint8_t *data, size_t size);

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

const char* errorString(Result r);

} // namespace ESPFlasher
