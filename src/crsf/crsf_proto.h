#pragma once
#include <Arduino.h>

// CRSF (Crossfire) protocol definitions
// Used by TBS Crossfire and ExpressLRS
// Frame format: [SYNC] [LEN] [TYPE] [PAYLOAD...] [CRC8]

namespace CRSF {

// Addresses (destinations/origins for extended frames)
static const uint8_t ADDR_BROADCAST      = 0x00;
static const uint8_t ADDR_CLOUD          = 0x0E;
static const uint8_t ADDR_USB            = 0x10;
static const uint8_t ADDR_TBS_CORE       = 0x80;
static const uint8_t ADDR_CURRENT_SENSOR = 0xC0;
static const uint8_t ADDR_GPS            = 0xC2;
static const uint8_t ADDR_TBS_BLACKBOX   = 0xC4;
static const uint8_t ADDR_FLIGHT_CTRL    = 0xC8;  // FC (default sync byte)
static const uint8_t ADDR_RACE_TAG       = 0xCC;
static const uint8_t ADDR_RADIO          = 0xEA;  // Radio (TX16S, etc.)
static const uint8_t ADDR_RECEIVER       = 0xEC;  // ELRS RX
static const uint8_t ADDR_TRANSMITTER    = 0xEE;  // ELRS TX module
static const uint8_t ADDR_ELRS_LUA       = 0xEF;

// Frame types
static const uint8_t FRAME_GPS                  = 0x02;
static const uint8_t FRAME_VARIO                = 0x07;
static const uint8_t FRAME_BATTERY_SENSOR       = 0x08;
static const uint8_t FRAME_BARO_ALTITUDE        = 0x09;
static const uint8_t FRAME_HEARTBEAT            = 0x0B;
static const uint8_t FRAME_LINK_STATISTICS      = 0x14;
static const uint8_t FRAME_OPENTX_SYNC          = 0x10;
static const uint8_t FRAME_RC_CHANNELS_PACKED   = 0x16;
static const uint8_t FRAME_LINK_RX_ID           = 0x1C;
static const uint8_t FRAME_LINK_TX_ID           = 0x1D;
static const uint8_t FRAME_ATTITUDE             = 0x1E;
static const uint8_t FRAME_FLIGHT_MODE          = 0x21;

// Extended (with addressing: [dest][origin][payload])
static const uint8_t FRAME_DEVICE_PING          = 0x28;
static const uint8_t FRAME_DEVICE_INFO          = 0x29;
static const uint8_t FRAME_PARAMETER_SETTINGS_ENTRY = 0x2B;
static const uint8_t FRAME_PARAMETER_READ       = 0x2C;
static const uint8_t FRAME_PARAMETER_WRITE      = 0x2D;
static const uint8_t FRAME_ELRS_STATUS          = 0x2E;
static const uint8_t FRAME_COMMAND              = 0x32;
static const uint8_t FRAME_RADIO                = 0x3A;
static const uint8_t FRAME_MSP_REQ              = 0x7A;
static const uint8_t FRAME_MSP_RESP             = 0x7B;

// Max frame length
static const size_t FRAME_SIZE_MAX = 64;  // CRSF max is 64 bytes total

// Parameter types (for Configurator)
enum ParamType {
    PARAM_UINT8      = 0,
    PARAM_INT8       = 1,
    PARAM_UINT16     = 2,
    PARAM_INT16      = 3,
    PARAM_FLOAT      = 8,
    PARAM_TEXT_SELECTION = 9,  // dropdown
    PARAM_STRING     = 10,
    PARAM_FOLDER     = 11,
    PARAM_INFO       = 12,
    PARAM_COMMAND    = 13,
    PARAM_OUT_OF_RANGE = 127,
};

// Command subcommands (frame type 0x32)
static const uint8_t CMD_SUBCMD_RX      = 0x10;
static const uint8_t CMD_RX_BIND        = 0x01;
static const uint8_t CMD_SUBCMD_GENERAL = 0x05;
static const uint8_t CMD_PROTOCOL_SPEED = 0x01;

// CRC8 DVB-S2 polynomial 0xD5
uint8_t crc8(const uint8_t *data, size_t len);

// Parsed frame (view into bytes)
struct Frame {
    uint8_t sync;
    uint8_t length;          // LEN field (payload + type + crc)
    uint8_t type;
    const uint8_t *payload;  // points into raw buffer
    uint8_t payload_len;     // length - 2 (type + crc)
    uint8_t crc;
    bool    valid;
};

// Parse a complete frame from buffer
bool parseFrame(const uint8_t *buf, size_t len, Frame &out);

// Build a frame into buffer, returns total bytes written
size_t buildFrame(uint8_t *buf, uint8_t sync, uint8_t type,
                  const uint8_t *payload, uint8_t payload_len);

// Build extended frame (with dest + origin)
size_t buildExtFrame(uint8_t *buf, uint8_t sync, uint8_t type,
                     uint8_t dest, uint8_t origin,
                     const uint8_t *payload, uint8_t payload_len);

} // namespace CRSF
