#include "crsf_proto.h"

namespace CRSF {

// CRC8 DVB-S2, polynomial 0xD5
static const uint8_t CRC_POLY = 0xD5;

uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) crc = (crc << 1) ^ CRC_POLY;
            else            crc = crc << 1;
        }
    }
    return crc;
}

bool parseFrame(const uint8_t *buf, size_t len, Frame &out) {
    if (len < 4) return false;
    uint8_t frame_len = buf[1];
    if (frame_len < 2 || frame_len > FRAME_SIZE_MAX - 2) return false;
    if (len < (size_t)(frame_len + 2)) return false;

    out.sync = buf[0];
    out.length = frame_len;
    out.type = buf[2];
    out.payload = buf + 3;
    out.payload_len = frame_len - 2;  // subtract type + crc
    out.crc = buf[2 + frame_len - 1];

    // Verify CRC (computed over type + payload, excluding sync and length)
    uint8_t computed = crc8(buf + 2, frame_len - 1);
    out.valid = (computed == out.crc);
    return out.valid;
}

size_t buildFrame(uint8_t *buf, uint8_t sync, uint8_t type,
                  const uint8_t *payload, uint8_t payload_len) {
    // Layout: [SYNC] [LEN] [TYPE] [PAYLOAD...] [CRC8]
    // LEN = type + payload + crc
    buf[0] = sync;
    buf[1] = payload_len + 2;  // type + crc
    buf[2] = type;
    if (payload_len > 0) {
        memcpy(buf + 3, payload, payload_len);
    }
    buf[3 + payload_len] = crc8(buf + 2, payload_len + 1);
    return 4 + payload_len;
}

size_t buildExtFrame(uint8_t *buf, uint8_t sync, uint8_t type,
                     uint8_t dest, uint8_t origin,
                     const uint8_t *payload, uint8_t payload_len) {
    // Extended: [SYNC] [LEN] [TYPE] [DEST] [ORIGIN] [PAYLOAD...] [CRC8]
    uint8_t ext_payload[64];
    if (payload_len + 2 > (int)sizeof(ext_payload)) return 0;
    ext_payload[0] = dest;
    ext_payload[1] = origin;
    if (payload_len > 0) memcpy(ext_payload + 2, payload, payload_len);
    return buildFrame(buf, sync, type, ext_payload, payload_len + 2);
}

} // namespace CRSF
