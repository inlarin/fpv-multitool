#pragma once
#include <Arduino.h>

namespace CRSFService {

// =====================================================================
// Live telemetry state
// =====================================================================
struct LinkStats {
    uint8_t  uplink_rssi1;
    uint8_t  uplink_rssi2;
    uint8_t  uplink_link_quality;
    int8_t   uplink_snr;
    uint8_t  active_antenna;
    uint8_t  rf_mode;
    uint8_t  uplink_tx_power;
    uint8_t  downlink_rssi;
    uint8_t  downlink_link_quality;
    int8_t   downlink_snr;
    bool     valid;
};

struct BatteryTelem {
    uint16_t voltage_dV;   // decivolts (10mV) — scale 0.1V
    uint16_t current_dA;   // deciamps — 0.1A
    uint32_t capacity_mAh;
    uint8_t  remaining_pct;
    bool     valid;
};

struct GPSTelem {
    int32_t  latitude_e7;
    int32_t  longitude_e7;
    uint16_t speed_cms;    // cm/s × 36 = km/h × 100
    uint16_t heading_cd;   // centi-degrees
    uint16_t altitude_m;   // meters + 1000 offset
    uint8_t  satellites;
    bool     valid;
};

struct AttitudeTelem {
    int16_t pitch_10krad;  // radians × 10000
    int16_t roll_10krad;
    int16_t yaw_10krad;
    bool    valid;
};

struct RxChannels {
    uint16_t ch[16];       // 172..1811 (1500 = center)
    bool     valid;
    uint32_t last_update_ms;
};

struct FlightMode {
    char name[16];
    bool valid;
};

struct State {
    bool connected;        // any frame received in last 2s
    uint32_t last_rx_ms;
    uint32_t total_frames;
    uint32_t bad_crc;

    LinkStats link;
    BatteryTelem battery;
    GPSTelem gps;
    AttitudeTelem attitude;
    RxChannels channels;
    FlightMode mode;
};

// Public API ===========================================================

void begin(HardwareSerial *uart, int rx_pin, int tx_pin,
           uint32_t baud = 420000, bool inverted = false);
void end();
bool isRunning();

// Call from loop(); reads UART and updates state
void loop();

// Snapshot current state
const State& state();

// Reset counters
void resetStats();

// Send a raw command frame (ext frame, dest=RX)
// Returns true if queued
bool sendCommand(uint8_t cmd_subcmd, uint8_t cmd_id, const uint8_t *args, uint8_t args_len);

// Convenience wrappers
bool cmdRxBind();
bool cmdReboot();  // ELRS: enter bootloader

// Send RC channels (16 channels, 172-1811 range)
bool sendChannels(const uint16_t *channels);

// Configurator operations (raw UART access)
bool sendDevicePing();
bool sendParameterRead(uint8_t param_id, uint8_t chunk_index = 0);
bool sendParameterWrite(uint8_t param_id, const uint8_t *value, uint8_t value_len);

} // namespace CRSFService
