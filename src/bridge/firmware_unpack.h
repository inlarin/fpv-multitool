#pragma once
#include <Arduino.h>

// Firmware format detection & decompression
namespace FirmwareUnpack {

enum Format {
    FMT_UNKNOWN,
    FMT_RAW_BIN,    // starts with ESP image magic 0xE9
    FMT_GZIP,       // starts with 0x1F 0x8B
    FMT_ELRS,       // ELRS container (starts with "ELRS" magic)
};

Format detect(const uint8_t *data, size_t size);
const char* formatName(Format f);

// Decompress gzip in-place or to new buffer
// Returns new buffer (allocated in PSRAM) and its size via out_size
// Caller must free() the returned pointer
// Returns nullptr on failure
uint8_t* gunzip(const uint8_t *gz_data, size_t gz_size, size_t *out_size);

// Extract firmware from .elrs container
// Returns pointer INSIDE data (no allocation) and size
// Returns nullptr on failure
const uint8_t* extractELRS(const uint8_t *data, size_t size, size_t *out_size);

} // namespace FirmwareUnpack
