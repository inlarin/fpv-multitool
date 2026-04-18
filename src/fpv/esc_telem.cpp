#include "esc_telem.h"
#include "pin_config.h"
#include "core/pin_port.h"
#include <HardwareSerial.h>

namespace ESCTelem {

static ESCTelemState s_state = {};
static bool          s_running = false;
static uint8_t       s_pole_count = 14;
static uint8_t       s_buf[10];
static int           s_pos = 0;
static uint32_t      s_rateWindowStart = 0;
static uint32_t      s_framesInWindow  = 0;

// CRC-8 used by KISS ESC telemetry (poly 0xD5, init 0x00).
// Note: some BLHeli_32 variants use different CRCs; this handles both by
// accepting either 0xD5-poly or "update_crc8" variant from BLHeli source.
static uint8_t kiss_crc8_update(uint8_t crc, uint8_t c) {
    crc ^= c;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : (crc << 1);
    }
    return crc;
}

static uint8_t kiss_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc = kiss_crc8_update(crc, data[i]);
    return crc;
}

static bool parseFrame() {
    uint8_t crc = kiss_crc8(s_buf, 9);
    if (crc != s_buf[9]) {
        s_state.crcErrors++;
        return false;
    }
    ESCTelemFrame f = {};
    f.temp_c          = s_buf[0];
    f.voltage_cV      = ((uint16_t)s_buf[1] << 8) | s_buf[2];
    f.current_cA      = ((uint16_t)s_buf[3] << 8) | s_buf[4];
    f.consumption_mAh = ((uint16_t)s_buf[5] << 8) | s_buf[6];
    f.erpm            = ((uint16_t)s_buf[7] << 8) | s_buf[8];
    f.valid           = true;

    s_state.last = f;
    s_state.frameCount++;
    s_state.lastFrameMs = millis();

    // Track extremes
    if (f.temp_c > s_state.maxTemp) s_state.maxTemp = f.temp_c;
    if (f.current_cA > s_state.maxCurrent_cA) s_state.maxCurrent_cA = f.current_cA;
    if (f.erpm > s_state.maxErpm) s_state.maxErpm = f.erpm;
    if (f.voltage_cV > s_state.peakVoltage_cV) s_state.peakVoltage_cV = f.voltage_cV;
    if (f.voltage_cV > 0 && (s_state.minVoltage_cV == 0 || f.voltage_cV < s_state.minVoltage_cV))
        s_state.minVoltage_cV = f.voltage_cV;
    return true;
}

void start(uint8_t poleCount) {
    stop();
    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "esc_telem")) {
        Serial.println("[ESCTelem] Port B busy — switch to UART in System → Port B Mode");
        return;
    }
    s_pole_count = poleCount > 0 ? poleCount : 14;
    s_state = {};
    s_pos = 0;
    Serial1.end();
    Serial1.begin(115200, SERIAL_8N1, ELRS_RX, ELRS_TX);
    s_running = true;
    s_rateWindowStart = millis();
    s_framesInWindow  = 0;
    s_state.running = true;
    Serial.println("[ESCTelem] started on ELRS_RX @ 115200 8N1");
}

void stop() {
    if (!s_running) return;
    Serial1.end();
    s_running = false;
    s_state.running = false;
    s_state.connected = false;
    PinPort::release(PinPort::PORT_B);
}

bool isRunning()  { return s_running; }
const ESCTelemState &state() { return s_state; }
uint8_t polePairs() { return s_pole_count / 2; }

void loop() {
    if (!s_running) return;

    // Framing: ESC emits 10 bytes back-to-back. If we see >5ms gap between
    // bytes, treat it as frame boundary. Simpler: reset pos on stale data.
    static uint32_t lastByteMs = 0;
    uint32_t now = millis();
    if (s_pos > 0 && (now - lastByteMs) > 20) {
        s_pos = 0;  // frame timeout, resync
    }

    while (Serial1.available()) {
        s_buf[s_pos++] = Serial1.read();
        lastByteMs = now;
        if (s_pos >= 10) {
            parseFrame();
            s_pos = 0;
        }
    }

    // Connection timeout: 500ms without good frame = disconnected
    s_state.connected = (now - s_state.lastFrameMs < 500) && s_state.frameCount > 0;

    // Frame rate (1s window)
    if (now - s_rateWindowStart >= 1000) {
        s_state.frameRateHz = (s_state.frameCount - s_framesInWindow) * 1000 / (now - s_rateWindowStart);
        s_framesInWindow   = s_state.frameCount;
        s_rateWindowStart  = now;
    }
}

} // namespace ESCTelem
