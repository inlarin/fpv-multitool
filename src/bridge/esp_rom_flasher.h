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

const char* errorString(Result r);

} // namespace ESPFlasher
