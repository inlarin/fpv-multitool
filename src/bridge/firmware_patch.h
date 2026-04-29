#pragma once

#include <stddef.h>
#include <stdint.h>

// ExpressLRS firmware customization, the same way the official web-flasher
// patches a vanilla .bin before flashing. Reference:
// https://github.com/ExpressLRS/web-flasher/blob/main/src/js/configure.js
//
// The vanilla ELRS firmware reads a configuration appendix from the very end
// of its app image: product_name(128) | lua_name(16) | options_json(512) |
// hardware_json(2048). We rewrite that appendix in-place so a single binary
// can be customized for any bind phrase / target without rebuilding.
namespace FirmwarePatch {

// ESP32-family chip_id from byte 12 of the extended image header. ESP8285
// has a different image layout (legacy 8-byte header, app starts at flash
// 0x1000 in the merged image) and is intentionally NOT supported here.
enum class Platform : uint8_t {
    Unknown = 0,
    ESP32   = 1,   // chip_id 0x00
    ESP32S2 = 2,   // chip_id 0x02
    ESP32C3 = 3,   // chip_id 0x05 — BAYCK RC C3 Dual
    ESP32S3 = 4,   // chip_id 0x09
    ESP32C2 = 5,   // chip_id 0x0C
};

const char* platformName(Platform p);

struct Options {
    const char* bind_phrase;          // REQUIRED — UID = MD5("-DMY_BINDING_PHRASE=\"<phrase>\"")[0:6]
    const char* product_name;         // REQUIRED — must match the target's expected name
    const char* lua_name;             // REQUIRED — short name shown in the radio's Lua menu
    const char* hardware_json;        // NULL → 2048 zero bytes (firmware uses built-in defaults)

    int         domain;               // -1 = omit; only meaningful for sx127x (900 MHz)
    const char* wifi_ssid;            // NULL = omit
    const char* wifi_password;        // NULL = omit
    int         rcvr_uart_baud;       // 0 = omit (default 420000)
    int         wifi_on_interval;     // 0 = omit (firmware default 60s)
    int         tlm_interval;         // 0 = omit
    bool        lock_on_first_connection;
    bool        unlock_higher_power;
    uint32_t    flash_discriminator;  // 0 → generated via esp_random()
};

struct Result {
    bool        ok;
    const char* error;                // NULL if ok
    Platform    platform;
    size_t      old_size;             // input firmware length
    size_t      new_size;             // total written length (firmware + appendix)
    size_t      firmware_end;         // offset where appendix starts
    uint8_t     uid[6];               // computed UID — echoed back to the client
};

// Compute 6-byte UID from a bind phrase, identical to the official web-flasher.
void uidFromBindPhrase(const char* phrase, uint8_t out_uid[6]);

// Inspect the ESP image header. Returns Platform::Unknown for non-ESP32-family
// images (ESP8285 legacy header, ZLRS/Foxeer encrypted containers, etc).
Platform detectPlatform(const uint8_t* bin, size_t binLen);

// Walk the segment table and return the offset where the firmware ends, with
// 16-byte alignment + 32-byte SHA256 trailer accounted for. Returns 0 on parse
// error (corrupt segment count / table runs past binLen / etc).
size_t findFirmwareEnd(const uint8_t* bin, size_t binLen, Platform plat);

// Rewrite the configuration appendix in-place at the end of `firmwareLen`.
// `bufCapacity` must be at least `firmwareLen + 2704` (128+16+512+2048).
Result patchFirmware(uint8_t* buf, size_t bufCapacity, size_t firmwareLen,
                     const Options& opts);

} // namespace FirmwarePatch
