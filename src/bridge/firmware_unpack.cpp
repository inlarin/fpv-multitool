#include "firmware_unpack.h"
#include <miniz.h>

namespace FirmwareUnpack {

Format detect(const uint8_t *data, size_t size) {
    if (!data || size < 4) return FMT_UNKNOWN;

    // ESP image magic byte
    if (data[0] == 0xE9) return FMT_RAW_BIN;

    // Gzip magic
    if (data[0] == 0x1F && data[1] == 0x8B) return FMT_GZIP;

    // ELRS container magic "ELRS"
    if (data[0] == 'E' && data[1] == 'L' && data[2] == 'R' && data[3] == 'S')
        return FMT_ELRS;

    return FMT_UNKNOWN;
}

const char* formatName(Format f) {
    switch (f) {
        case FMT_RAW_BIN: return "Raw ESP binary";
        case FMT_GZIP:    return "Gzip compressed";
        case FMT_ELRS:    return "ELRS container";
        default:          return "Unknown";
    }
}

// Gunzip using miniz tinfl (tiny inflate, no zlib wrapper)
uint8_t* gunzip(const uint8_t *gz_data, size_t gz_size, size_t *out_size) {
    if (!gz_data || gz_size < 18) return nullptr;

    // Gzip header (RFC 1952):
    //   [0-1] 0x1F 0x8B
    //   [2]   compression method (8 = deflate)
    //   [3]   flags
    //   [4-7] mtime
    //   [8]   extra flags
    //   [9]   OS
    //   optional: extra field, filename, comment, CRC16
    //   deflate stream
    //   [-8..-5] CRC32
    //   [-4..-1] ISIZE (uncompressed size mod 2^32)

    if (gz_data[0] != 0x1F || gz_data[1] != 0x8B) return nullptr;
    if (gz_data[2] != 8) return nullptr; // only deflate

    uint8_t flags = gz_data[3];
    size_t hdr = 10;

    if (flags & 0x04) { // FEXTRA
        if (hdr + 2 > gz_size) return nullptr;
        uint16_t xlen = gz_data[hdr] | (gz_data[hdr+1] << 8);
        hdr += 2 + xlen;
    }
    if (flags & 0x08) { // FNAME
        while (hdr < gz_size && gz_data[hdr] != 0) hdr++;
        hdr++;
    }
    if (flags & 0x10) { // FCOMMENT
        while (hdr < gz_size && gz_data[hdr] != 0) hdr++;
        hdr++;
    }
    if (flags & 0x02) hdr += 2; // FHCRC

    if (hdr >= gz_size - 8) return nullptr;

    // Decompressed size from ISIZE (last 4 bytes)
    uint32_t isize = gz_data[gz_size-4] |
                     (gz_data[gz_size-3] << 8) |
                     (gz_data[gz_size-2] << 16) |
                     (gz_data[gz_size-1] << 24);

    // Sanity: ISIZE must be reasonable for an ELRS firmware
    // Typical ELRS RX firmware: 200-500 KB. Cap at 4 MB to catch corruption.
    static const uint32_t MAX_DECOMPRESSED = 4 * 1024 * 1024;
    if (isize == 0 || isize > MAX_DECOMPRESSED) {
        Serial.printf("[gunzip] suspicious ISIZE %u — refusing to allocate\n", isize);
        return nullptr;
    }

    // Allocate output in PSRAM with overflow-safe size calculation
    if (isize > SIZE_MAX - 512) return nullptr;
    size_t alloc_size = isize + 512; // +512 safety for tinfl overread
    uint8_t *out = (uint8_t*)ps_malloc(alloc_size);
    if (!out) {
        Serial.printf("[gunzip] ps_malloc(%u) failed\n", alloc_size);
        return nullptr;
    }

    // Deflate stream starts at hdr, ends at gz_size-8
    size_t deflate_size = gz_size - 8 - hdr;

    // Bound the decompression to allocated size (defense against ISIZE lie)
    mz_ulong out_len = alloc_size;
    int r = tinfl_decompress_mem_to_mem(out, out_len, gz_data + hdr, deflate_size, 0);

    if (r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        Serial.println("[gunzip] tinfl decompress failed");
        free(out);
        return nullptr;
    }

    // Verify actual decompressed size matches ISIZE (detect tampered gz)
    if ((uint32_t)r != isize) {
        Serial.printf("[gunzip] size mismatch: expected %u, got %d\n", isize, r);
        free(out);
        return nullptr;
    }

    *out_size = r;
    Serial.printf("[gunzip] %u bytes -> %u bytes\n", gz_size, *out_size);
    return out;
}

// ELRS container format:
//   "ELRS" (4)
//   [4-5] version
//   [6-7] header_size
//   [8-11] firmware_size
//   ... metadata ...
//   firmware data
//   signature
// (format details vary; this is a best-effort extractor)
const uint8_t* extractELRS(const uint8_t *data, size_t size, size_t *out_size) {
    if (!data || size < 16) return nullptr;
    if (!(data[0] == 'E' && data[1] == 'L' && data[2] == 'R' && data[3] == 'S')) return nullptr;

    uint16_t hdr_size = data[6] | (data[7] << 8);
    uint32_t fw_size = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);

    if (hdr_size >= size || fw_size > size) return nullptr;
    if (hdr_size + fw_size > size) return nullptr;

    *out_size = fw_size;
    return data + hdr_size;
}

} // namespace FirmwareUnpack
