#include "crsf_service.h"
#include "crsf_proto.h"
#include "crsf_config.h"

namespace CRSFService {

static HardwareSerial *s_uart = nullptr;
static bool s_running = false;
static State s_state = {};

// Parser state
static uint8_t s_rx_buf[CRSF::FRAME_SIZE_MAX];
static size_t  s_rx_pos = 0;
static uint32_t s_last_byte_ms = 0;

// ---------- Frame parsing ----------
static void handleFrame(const CRSF::Frame &f) {
    s_state.total_frames++;
    s_state.last_rx_ms = millis();
    s_state.connected = true;

    switch (f.type) {
        case CRSF::FRAME_LINK_STATISTICS: {
            if (f.payload_len < 10) break;
            auto &L = s_state.link;
            L.uplink_rssi1        = f.payload[0];
            L.uplink_rssi2        = f.payload[1];
            L.uplink_link_quality = f.payload[2];
            L.uplink_snr          = (int8_t)f.payload[3];
            L.active_antenna      = f.payload[4];
            L.rf_mode             = f.payload[5];
            L.uplink_tx_power     = f.payload[6];
            L.downlink_rssi       = f.payload[7];
            L.downlink_link_quality = f.payload[8];
            L.downlink_snr        = (int8_t)f.payload[9];
            L.valid = true;
            break;
        }

        case CRSF::FRAME_BATTERY_SENSOR: {
            if (f.payload_len < 8) break;
            auto &B = s_state.battery;
            B.voltage_dV = (uint16_t)f.payload[0] << 8 | f.payload[1];
            B.current_dA = (uint16_t)f.payload[2] << 8 | f.payload[3];
            B.capacity_mAh = ((uint32_t)f.payload[4] << 16) |
                             ((uint32_t)f.payload[5] << 8) |
                             (uint32_t)f.payload[6];
            B.remaining_pct = f.payload[7];
            B.valid = true;
            break;
        }

        case CRSF::FRAME_GPS: {
            if (f.payload_len < 15) break;
            auto &G = s_state.gps;
            G.latitude_e7  = ((int32_t)f.payload[0] << 24) | ((int32_t)f.payload[1] << 16) |
                             ((int32_t)f.payload[2] << 8) | f.payload[3];
            G.longitude_e7 = ((int32_t)f.payload[4] << 24) | ((int32_t)f.payload[5] << 16) |
                             ((int32_t)f.payload[6] << 8) | f.payload[7];
            G.speed_cms    = (uint16_t)f.payload[8] << 8 | f.payload[9];
            G.heading_cd   = (uint16_t)f.payload[10] << 8 | f.payload[11];
            G.altitude_m   = (uint16_t)f.payload[12] << 8 | f.payload[13];
            G.satellites   = f.payload[14];
            G.valid = true;
            break;
        }

        case CRSF::FRAME_ATTITUDE: {
            if (f.payload_len < 6) break;
            auto &A = s_state.attitude;
            A.pitch_10krad = (int16_t)((f.payload[0] << 8) | f.payload[1]);
            A.roll_10krad  = (int16_t)((f.payload[2] << 8) | f.payload[3]);
            A.yaw_10krad   = (int16_t)((f.payload[4] << 8) | f.payload[5]);
            A.valid = true;
            break;
        }

        case CRSF::FRAME_FLIGHT_MODE: {
            auto &M = s_state.mode;
            size_t n = f.payload_len < sizeof(M.name) - 1 ? f.payload_len : sizeof(M.name) - 1;
            memcpy(M.name, f.payload, n);
            M.name[n] = 0;
            M.valid = true;
            break;
        }

        case CRSF::FRAME_RC_CHANNELS_PACKED: {
            // 22 bytes = 16 × 11 bits packed
            if (f.payload_len < 22) break;
            auto &C = s_state.channels;
            const uint8_t *p = f.payload;
            C.ch[0]  = (p[0]       | p[1]  << 8)                       & 0x07FF;
            C.ch[1]  = (p[1]  >> 3 | p[2]  << 5)                       & 0x07FF;
            C.ch[2]  = (p[2]  >> 6 | p[3]  << 2 | p[4]  << 10)         & 0x07FF;
            C.ch[3]  = (p[4]  >> 1 | p[5]  << 7)                       & 0x07FF;
            C.ch[4]  = (p[5]  >> 4 | p[6]  << 4)                       & 0x07FF;
            C.ch[5]  = (p[6]  >> 7 | p[7]  << 1 | p[8]  << 9)          & 0x07FF;
            C.ch[6]  = (p[8]  >> 2 | p[9]  << 6)                       & 0x07FF;
            C.ch[7]  = (p[9]  >> 5 | p[10] << 3)                       & 0x07FF;
            C.ch[8]  = (p[11]      | p[12] << 8)                       & 0x07FF;
            C.ch[9]  = (p[12] >> 3 | p[13] << 5)                       & 0x07FF;
            C.ch[10] = (p[13] >> 6 | p[14] << 2 | p[15] << 10)         & 0x07FF;
            C.ch[11] = (p[15] >> 1 | p[16] << 7)                       & 0x07FF;
            C.ch[12] = (p[16] >> 4 | p[17] << 4)                       & 0x07FF;
            C.ch[13] = (p[17] >> 7 | p[18] << 1 | p[19] << 9)          & 0x07FF;
            C.ch[14] = (p[19] >> 2 | p[20] << 6)                       & 0x07FF;
            C.ch[15] = (p[20] >> 5 | p[21] << 3)                       & 0x07FF;
            C.valid = true;
            C.last_update_ms = millis();
            break;
        }

        case CRSF::FRAME_DEVICE_INFO:
        case CRSF::FRAME_PARAMETER_SETTINGS_ENTRY:
            // Dispatch extended frames to configurator
            CRSFConfig::handleFrame(f.type, f.payload, f.payload_len);
            break;

        default:
            break;
    }
}

// ---------- Parser state machine ----------
static void feed(uint8_t b) {
    uint32_t now = millis();

    // Timeout reset: if > 5ms since last byte, start over
    if (s_rx_pos > 0 && (now - s_last_byte_ms) > 5) {
        s_rx_pos = 0;
    }
    s_last_byte_ms = now;

    // Looking for sync byte
    if (s_rx_pos == 0) {
        if (b != CRSF::ADDR_FLIGHT_CTRL &&
            b != CRSF::ADDR_RADIO &&
            b != CRSF::ADDR_RECEIVER &&
            b != CRSF::ADDR_TRANSMITTER &&
            b != CRSF::ADDR_ELRS_LUA) {
            return;
        }
    }

    if (s_rx_pos < sizeof(s_rx_buf)) {
        s_rx_buf[s_rx_pos++] = b;
    } else {
        s_rx_pos = 0;
        return;
    }

    // Have length byte?
    if (s_rx_pos >= 2) {
        uint8_t expected_len = s_rx_buf[1] + 2;
        if (expected_len > sizeof(s_rx_buf)) {
            s_rx_pos = 0;
            return;
        }
        if (s_rx_pos >= expected_len) {
            CRSF::Frame frame;
            if (CRSF::parseFrame(s_rx_buf, s_rx_pos, frame) && frame.valid) {
                handleFrame(frame);
            } else {
                s_state.bad_crc++;
            }
            s_rx_pos = 0;
        }
    }
}

// ---------- Public API ----------
void begin(HardwareSerial *uart, int rx_pin, int tx_pin, uint32_t baud, bool inverted) {
    end();
    s_uart = uart;
    s_uart->setRxBufferSize(1024);
    // ESP32 Arduino Core 3 begin signature:
    //   begin(baud, config, rx, tx, invert, timeout, rxfifo_full_thrhd)
    s_uart->begin(baud, SERIAL_8N1, rx_pin, tx_pin, inverted);
    s_running = true;
    memset(&s_state, 0, sizeof(s_state));
    s_rx_pos = 0;
}

void end() {
    if (s_uart && s_running) s_uart->end();
    s_running = false;
}

bool isRunning() { return s_running; }

void loop() {
    if (!s_running || !s_uart) return;

    while (s_uart->available()) {
        feed(s_uart->read());
    }

    // Connection timeout check
    if (s_state.connected && (millis() - s_state.last_rx_ms) > 2000) {
        s_state.connected = false;
    }
}

const State& state() { return s_state; }

void resetStats() {
    s_state.total_frames = 0;
    s_state.bad_crc = 0;
}

// ---------- Commands ----------
bool sendCommand(uint8_t cmd_subcmd, uint8_t cmd_id, const uint8_t *args, uint8_t args_len) {
    if (!s_running || !s_uart) return false;

    uint8_t payload[32];
    if (args_len + 2 > (int)sizeof(payload)) return false;
    payload[0] = cmd_subcmd;
    payload[1] = cmd_id;
    if (args && args_len > 0) memcpy(payload + 2, args, args_len);

    uint8_t frame_buf[64];
    size_t n = CRSF::buildExtFrame(frame_buf, CRSF::ADDR_FLIGHT_CTRL,
        CRSF::FRAME_COMMAND, CRSF::ADDR_RECEIVER, CRSF::ADDR_FLIGHT_CTRL,
        payload, args_len + 2);
    if (n == 0) return false;

    s_uart->write(frame_buf, n);
    s_uart->flush();
    return true;
}

bool cmdRxBind() {
    return sendCommand(CRSF::CMD_SUBCMD_RX, CRSF::CMD_RX_BIND, nullptr, 0);
}

bool cmdReboot() {
    // ELRS: enter bootloader command
    // Subcommand 0x06 (RESET), action 0x00 = reboot to bootloader
    return sendCommand(0x06, 0x00, nullptr, 0);
}

bool sendDevicePing() {
    if (!s_running || !s_uart) return false;
    uint8_t frame[8];
    size_t n = CRSF::buildExtFrame(frame, CRSF::ADDR_RECEIVER,
        CRSF::FRAME_DEVICE_PING,
        CRSF::ADDR_RECEIVER, CRSF::ADDR_FLIGHT_CTRL,
        nullptr, 0);
    s_uart->write(frame, n);
    return true;
}

bool sendParameterRead(uint8_t param_id, uint8_t chunk_index) {
    if (!s_running || !s_uart) return false;
    uint8_t args[2] = { param_id, chunk_index };
    uint8_t frame[16];
    size_t n = CRSF::buildExtFrame(frame, CRSF::ADDR_RECEIVER,
        CRSF::FRAME_PARAMETER_READ,
        CRSF::ADDR_RECEIVER, CRSF::ADDR_FLIGHT_CTRL,
        args, 2);
    s_uart->write(frame, n);
    return true;
}

bool sendParameterWrite(uint8_t param_id, const uint8_t *value, uint8_t value_len) {
    if (!s_running || !s_uart) return false;
    uint8_t args[32];
    if (value_len + 1 > (int)sizeof(args)) return false;
    args[0] = param_id;
    if (value && value_len > 0) memcpy(args + 1, value, value_len);
    uint8_t frame[40];
    size_t n = CRSF::buildExtFrame(frame, CRSF::ADDR_RECEIVER,
        CRSF::FRAME_PARAMETER_WRITE,
        CRSF::ADDR_RECEIVER, CRSF::ADDR_FLIGHT_CTRL,
        args, value_len + 1);
    s_uart->write(frame, n);
    return true;
}

bool sendChannels(const uint16_t *channels) {
    if (!s_running || !s_uart) return false;

    // Pack 16x11-bit channels into 22 bytes
    uint8_t payload[22];
    memset(payload, 0, 22);

    payload[0]  = (channels[0]       & 0x07FF);
    payload[1]  = (channels[0] >> 8  & 0x07) | ((channels[1] & 0x07FF) << 3);
    payload[2]  = (channels[1] >> 5  & 0x3F) | ((channels[2] & 0x07FF) << 6);
    payload[3]  = (channels[2] >> 2  & 0xFF);
    payload[4]  = (channels[2] >> 10 & 0x01) | ((channels[3] & 0x07FF) << 1);
    payload[5]  = (channels[3] >> 7  & 0x0F) | ((channels[4] & 0x07FF) << 4);
    payload[6]  = (channels[4] >> 4  & 0x7F) | ((channels[5] & 0x07FF) << 7);
    payload[7]  = (channels[5] >> 1  & 0xFF);
    payload[8]  = (channels[5] >> 9  & 0x03) | ((channels[6] & 0x07FF) << 2);
    payload[9]  = (channels[6] >> 6  & 0x1F) | ((channels[7] & 0x07FF) << 5);
    payload[10] = (channels[7] >> 3  & 0xFF);
    payload[11] = (channels[8]       & 0x07FF);
    payload[12] = (channels[8] >> 8  & 0x07) | ((channels[9] & 0x07FF) << 3);
    payload[13] = (channels[9] >> 5  & 0x3F) | ((channels[10] & 0x07FF) << 6);
    payload[14] = (channels[10] >> 2 & 0xFF);
    payload[15] = (channels[10] >> 10 & 0x01) | ((channels[11] & 0x07FF) << 1);
    payload[16] = (channels[11] >> 7 & 0x0F) | ((channels[12] & 0x07FF) << 4);
    payload[17] = (channels[12] >> 4 & 0x7F) | ((channels[13] & 0x07FF) << 7);
    payload[18] = (channels[13] >> 1 & 0xFF);
    payload[19] = (channels[13] >> 9 & 0x03) | ((channels[14] & 0x07FF) << 2);
    payload[20] = (channels[14] >> 6 & 0x1F) | ((channels[15] & 0x07FF) << 5);
    payload[21] = (channels[15] >> 3 & 0xFF);

    uint8_t frame_buf[32];
    size_t n = CRSF::buildFrame(frame_buf, CRSF::ADDR_FLIGHT_CTRL,
        CRSF::FRAME_RC_CHANNELS_PACKED, payload, 22);

    s_uart->write(frame_buf, n);
    return true;
}

} // namespace CRSFService
