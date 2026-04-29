#include "firmware_patch.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include <esp_system.h>
#include <string.h>

namespace FirmwarePatch {

const char* platformName(Platform p) {
    switch (p) {
        case Platform::ESP32:   return "esp32";
        case Platform::ESP32S2: return "esp32-s2";
        case Platform::ESP32C3: return "esp32-c3";
        case Platform::ESP32S3: return "esp32-s3";
        case Platform::ESP32C2: return "esp32-c2";
        default:                return "unknown";
    }
}

void uidFromBindPhrase(const char* phrase, uint8_t out_uid[6]) {
    if (!phrase) phrase = "";
    String s = "-DMY_BINDING_PHRASE=\"";
    s += phrase;
    s += "\"";
    MD5Builder md5;
    md5.begin();
    md5.add((uint8_t*)s.c_str(), s.length());
    md5.calculate();
    uint8_t digest[16];
    md5.getBytes(digest);
    memcpy(out_uid, digest, 6);
}

Platform detectPlatform(const uint8_t* bin, size_t binLen) {
    if (!bin || binLen < 24) return Platform::Unknown;
    if (bin[0] != 0xE9) return Platform::Unknown;
    switch (bin[12]) {
        case 0x00: return Platform::ESP32;
        case 0x02: return Platform::ESP32S2;
        case 0x05: return Platform::ESP32C3;
        case 0x09: return Platform::ESP32S3;
        case 0x0C: return Platform::ESP32C2;
        default:   return Platform::Unknown;
    }
}

size_t findFirmwareEnd(const uint8_t* bin, size_t binLen, Platform plat) {
    if (plat == Platform::Unknown) return 0;
    if (binLen < 24) return 0;

    uint8_t segs = bin[1];
    if (segs == 0 || segs > 32) return 0;

    size_t pos = 24;  // ESP32-family extended header is 24 bytes
    for (uint8_t i = 0; i < segs; i++) {
        if (pos + 8 > binLen) return 0;
        uint32_t segSize = (uint32_t)bin[pos + 4]
                         | ((uint32_t)bin[pos + 5] << 8)
                         | ((uint32_t)bin[pos + 6] << 16)
                         | ((uint32_t)bin[pos + 7] << 24);
        pos += 8 + segSize;
        if (pos > binLen) return 0;
    }
    pos = (pos + 16) & ~(size_t)15;  // 16-byte align
    pos += 32;                        // SHA256 trailer (always present on ESP32-family)
    if (pos > binLen) return 0;
    return pos;
}

static size_t writePadded(uint8_t* buf, size_t pos, const char* src, size_t padLen) {
    if (!src) {
        memset(buf + pos, 0, padLen);
        return pos + padLen;
    }
    size_t srcLen = strlen(src);
    if (srcLen > padLen) srcLen = padLen;
    memcpy(buf + pos, src, srcLen);
    if (srcLen < padLen) memset(buf + pos + srcLen, 0, padLen - srcLen);
    return pos + padLen;
}

Result patchFirmware(uint8_t* buf, size_t bufCapacity, size_t firmwareLen,
                     const Options& opts) {
    Result r = {};
    r.platform = Platform::Unknown;
    r.old_size = firmwareLen;

    if (!opts.bind_phrase || !*opts.bind_phrase) {
        r.error = "bind_phrase is required";
        return r;
    }
    if (!opts.product_name || !opts.lua_name) {
        r.error = "product_name and lua_name are required";
        return r;
    }

    r.platform = detectPlatform(buf, firmwareLen);
    if (r.platform == Platform::Unknown) {
        r.error = "Not an ESP32-family image (bad magic or unknown chip_id)";
        return r;
    }

    size_t fwEnd = findFirmwareEnd(buf, firmwareLen, r.platform);
    if (fwEnd == 0) {
        r.error = "Failed to parse ESP image header (corrupt segment table?)";
        return r;
    }
    r.firmware_end = fwEnd;

    const size_t kAppendSize = 128 + 16 + 512 + 2048;  // 2704
    if (fwEnd + kAppendSize > bufCapacity) {
        r.error = "Buffer too small for patched firmware (increase upload allocation)";
        return r;
    }

    uidFromBindPhrase(opts.bind_phrase, r.uid);

    JsonDocument doc;
    JsonArray uidArr = doc["uid"].to<JsonArray>();
    for (int i = 0; i < 6; i++) uidArr.add(r.uid[i]);

    if (opts.domain >= 0)              doc["domain"] = opts.domain;
    if (opts.wifi_on_interval > 0)     doc["wifi-on-interval"] = opts.wifi_on_interval;
    if (opts.wifi_ssid)                doc["wifi-ssid"] = opts.wifi_ssid;
    if (opts.wifi_password)            doc["wifi-password"] = opts.wifi_password;
    if (opts.rcvr_uart_baud > 0)       doc["rcvr-uart-baud"] = opts.rcvr_uart_baud;
    if (opts.tlm_interval > 0)         doc["tlm-interval"] = opts.tlm_interval;
    if (opts.lock_on_first_connection) doc["lock-on-first-connection"] = true;
    if (opts.unlock_higher_power)      doc["unlock-higher-power"] = true;

    // flash-discriminator: random per-flash so the firmware invalidates the
    // previous NVS config on first boot (otherwise stale settings persist).
    uint32_t disc = opts.flash_discriminator;
    if (disc == 0) disc = esp_random() & 0x7FFFFFFFu;
    doc["flash-discriminator"] = disc;

    char optionsJson[512];
    size_t jsonLen = serializeJson(doc, optionsJson, sizeof(optionsJson));
    if (jsonLen == 0 || jsonLen >= sizeof(optionsJson)) {
        r.error = "Options JSON serialization overflowed 512 bytes";
        return r;
    }
    optionsJson[jsonLen] = '\0';

    size_t pos = fwEnd;
    pos = writePadded(buf, pos, opts.product_name,  128);
    pos = writePadded(buf, pos, opts.lua_name,       16);
    pos = writePadded(buf, pos, optionsJson,        512);
    pos = writePadded(buf, pos, opts.hardware_json, 2048);

    r.new_size = pos;
    r.ok = true;
    r.error = nullptr;
    return r;
}

} // namespace FirmwarePatch
