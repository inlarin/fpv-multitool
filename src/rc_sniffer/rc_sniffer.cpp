#include "rc_sniffer.h"
#include "pin_config.h"
#include <HardwareSerial.h>
#include <driver/gpio.h>

namespace RCSniffer {

static RCState s_state = {};
static bool    s_running = false;
static uint32_t s_lastFrameMs_for_rate = 0;
static uint32_t s_framesInWindow = 0;
static uint32_t s_rateWindowStart = 0;

// ===== SBUS parser =====
// Frame: 25 bytes. [0]=0x0F header, [24]=0x00 footer.
// 16 × 11-bit channels packed into bytes 1-22 (LSB first).
// Byte 23: bit0=ch17, bit1=ch18, bit2=frameLost, bit3=failsafe.
static uint8_t s_sbusBuf[25];
static int     s_sbusPos = 0;

static bool parseSbusFrame() {
    if (s_sbusBuf[0] != 0x0F || s_sbusBuf[24] != 0x00) return false;
    const uint8_t *b = s_sbusBuf + 1;  // skip header
    // Unpack 16 channels from 22 bytes
    uint16_t raw[16];
    raw[0]  = ((b[0]    | (b[1] << 8))                         & 0x07FF);
    raw[1]  = ((b[1]>>3 | (b[2] << 5))                         & 0x07FF);
    raw[2]  = ((b[2]>>6 | (b[3] << 2) | (b[4] << 10))          & 0x07FF);
    raw[3]  = ((b[4]>>1 | (b[5] << 7))                         & 0x07FF);
    raw[4]  = ((b[5]>>4 | (b[6] << 4))                         & 0x07FF);
    raw[5]  = ((b[6]>>7 | (b[7] << 1) | (b[8] << 9))           & 0x07FF);
    raw[6]  = ((b[8]>>2 | (b[9] << 6))                         & 0x07FF);
    raw[7]  = ((b[9]>>5 | (b[10] << 3))                        & 0x07FF);
    raw[8]  = ((b[11]   | (b[12] << 8))                        & 0x07FF);
    raw[9]  = ((b[12]>>3| (b[13] << 5))                        & 0x07FF);
    raw[10] = ((b[13]>>6| (b[14] << 2) | (b[15] << 10))        & 0x07FF);
    raw[11] = ((b[15]>>1| (b[16] << 7))                        & 0x07FF);
    raw[12] = ((b[16]>>4| (b[17] << 4))                        & 0x07FF);
    raw[13] = ((b[17]>>7| (b[18] << 1) | (b[19] << 9))         & 0x07FF);
    raw[14] = ((b[19]>>2| (b[20] << 6))                        & 0x07FF);
    raw[15] = ((b[20]>>5| (b[21] << 3))                        & 0x07FF);

    // Convert SBUS raw (0..2047) to microseconds: 172..1811 → 988..2012 μs
    // Formula: us = (raw - 172) * (2012 - 988) / (1811 - 172) + 988
    for (int i = 0; i < 16; i++) {
        long r = (long)raw[i];
        long us = (r - 172L) * 1024L / 1639L + 988L;
        if (us < 800) us = 800;
        if (us > 2200) us = 2200;
        s_state.channels[i] = (uint16_t)us;
    }
    s_state.channelCount = 16;
    s_state.lostFrame = (b[22] & 0x04) != 0;
    s_state.failsafe  = (b[22] & 0x08) != 0;
    return true;
}

static void sbusByte(uint8_t ch) {
    if (s_sbusPos == 0 && ch != 0x0F) return;  // wait for header
    s_sbusBuf[s_sbusPos++] = ch;
    if (s_sbusPos >= 25) {
        if (parseSbusFrame()) {
            s_state.frameCount++;
            s_state.lastFrameMs = millis();
        } else {
            s_state.crcErrors++;
        }
        s_sbusPos = 0;
    }
}

// ===== iBus parser =====
// Frame: 32 bytes. [0]=0x20 [1]=0x40 header. 14 × u16 LE channels. Last 2 bytes = checksum.
// Checksum: 0xFFFF - sum(first 30 bytes).
static uint8_t s_ibusBuf[32];
static int     s_ibusPos = 0;

static bool parseIbusFrame() {
    if (s_ibusBuf[0] != 0x20 || s_ibusBuf[1] != 0x40) return false;
    uint16_t sum = 0xFFFF;
    for (int i = 0; i < 30; i++) sum -= s_ibusBuf[i];
    uint16_t got = s_ibusBuf[30] | (s_ibusBuf[31] << 8);
    if (sum != got) return false;
    for (int i = 0; i < 14; i++) {
        uint16_t val = s_ibusBuf[2 + i*2] | (s_ibusBuf[3 + i*2] << 8);
        s_state.channels[i] = val;  // iBus already in microseconds
    }
    s_state.channelCount = 14;
    return true;
}

static void ibusByte(uint8_t ch) {
    // Look for frame start 0x20
    if (s_ibusPos == 0 && ch != 0x20) return;
    if (s_ibusPos == 1 && ch != 0x40) { s_ibusPos = 0; return; }
    s_ibusBuf[s_ibusPos++] = ch;
    if (s_ibusPos >= 32) {
        if (parseIbusFrame()) {
            s_state.frameCount++;
            s_state.lastFrameMs = millis();
        } else {
            s_state.crcErrors++;
        }
        s_ibusPos = 0;
    }
}

// ===== PPM decoder =====
// GPIO interrupt captures timestamps of falling edges. Delta between edges
// = pulse gap → channel width. Long gap (>3ms) = sync, frame boundary.
static volatile uint32_t s_ppmLastEdgeUs = 0;
static volatile uint8_t  s_ppmChIdx = 0;
static volatile uint16_t s_ppmTmp[16];
static volatile bool     s_ppmFrameReady = false;
static volatile uint8_t  s_ppmFrameChCount = 0;

static void IRAM_ATTR ppmIsr() {
    uint32_t now = micros();
    uint32_t dt  = now - s_ppmLastEdgeUs;
    s_ppmLastEdgeUs = now;
    if (dt > 3000) {
        // Sync gap — end of frame
        s_ppmFrameChCount = s_ppmChIdx;
        s_ppmChIdx = 0;
        s_ppmFrameReady = true;
    } else if (s_ppmChIdx < 16) {
        s_ppmTmp[s_ppmChIdx++] = (uint16_t)dt;
    }
}

static void ppmPoll() {
    if (!s_ppmFrameReady) return;
    noInterrupts();
    uint8_t n = s_ppmFrameChCount;
    uint16_t snap[16];
    for (int i = 0; i < n && i < 16; i++) snap[i] = s_ppmTmp[i];
    s_ppmFrameReady = false;
    interrupts();
    if (n < 4 || n > 16) return;  // sanity
    for (int i = 0; i < n; i++) s_state.channels[i] = snap[i];
    s_state.channelCount = n;
    s_state.frameCount++;
    s_state.lastFrameMs = millis();
}

// ===== Lifecycle =====
void start(RCProto proto) {
    stop();
    s_state = {};
    s_state.proto = proto;

    switch (proto) {
        case RC_PROTO_SBUS:
            // 100000 baud 8E2 inverted — ESP32 HardwareSerial supports invert arg
            Serial1.end();
            Serial1.begin(100000, SERIAL_8E2, ELRS_RX, ELRS_TX, /*invert=*/true);
            s_sbusPos = 0;
            break;
        case RC_PROTO_IBUS:
            Serial1.end();
            Serial1.begin(115200, SERIAL_8N1, ELRS_RX, ELRS_TX);
            s_ibusPos = 0;
            break;
        case RC_PROTO_PPM:
            pinMode(ELRS_RX, INPUT_PULLUP);
            attachInterrupt(digitalPinToInterrupt(ELRS_RX), ppmIsr, FALLING);
            s_ppmChIdx = 0;
            s_ppmFrameReady = false;
            break;
        default:
            return;
    }
    s_running = true;
    s_rateWindowStart = millis();
    s_framesInWindow = 0;
    Serial.printf("[RCSniff] Started %s\n", protoName(proto));
}

void stop() {
    if (!s_running) return;
    switch (s_state.proto) {
        case RC_PROTO_SBUS:
        case RC_PROTO_IBUS:
            Serial1.end();
            break;
        case RC_PROTO_PPM:
            detachInterrupt(digitalPinToInterrupt(ELRS_RX));
            break;
        default: break;
    }
    s_running = false;
    s_state.proto = RC_PROTO_NONE;
    s_state.connected = false;
}

bool isRunning() { return s_running; }
const RCState &state() { return s_state; }

const char *protoName(RCProto p) {
    switch (p) {
        case RC_PROTO_SBUS: return "SBUS";
        case RC_PROTO_IBUS: return "iBus";
        case RC_PROTO_PPM:  return "PPM";
        default: return "none";
    }
}

// Auto-detect: try each protocol for 500ms, pick first that produces frames.
// Called from a blocking one-shot web endpoint — user polls result separately.
static uint32_t s_autoStartMs = 0;
static RCProto  s_autoCur = RC_PROTO_NONE;

void autoDetect() {
    // Try SBUS first (most common on modern receivers)
    start(RC_PROTO_SBUS);
    uint32_t until = millis() + 500;
    uint32_t startFrames = s_state.frameCount;
    while (millis() < until) {
        loop();
        delay(1);
    }
    if (s_state.frameCount > startFrames) return;

    start(RC_PROTO_IBUS);
    until = millis() + 500;
    startFrames = s_state.frameCount;
    while (millis() < until) {
        loop();
        delay(1);
    }
    if (s_state.frameCount > startFrames) return;

    start(RC_PROTO_PPM);
    until = millis() + 500;
    startFrames = s_state.frameCount;
    while (millis() < until) {
        loop();
        delay(1);
    }
    if (s_state.frameCount > startFrames) return;

    stop();
    s_state.proto = RC_PROTO_NONE;
}

void loop() {
    if (!s_running) return;
    switch (s_state.proto) {
        case RC_PROTO_SBUS:
            while (Serial1.available()) sbusByte(Serial1.read());
            break;
        case RC_PROTO_IBUS:
            while (Serial1.available()) ibusByte(Serial1.read());
            break;
        case RC_PROTO_PPM:
            ppmPoll();
            break;
        default: break;
    }
    // Connection timeout: 500ms without a good frame → disconnected
    s_state.connected = (millis() - s_state.lastFrameMs < 500) && s_state.frameCount > 0;
    // Compute frame rate every 1 second
    uint32_t now = millis();
    if (now - s_rateWindowStart >= 1000) {
        s_state.frameRateHz = (s_state.frameCount - s_framesInWindow) * 1000 / (now - s_rateWindowStart);
        s_framesInWindow = s_state.frameCount;
        s_rateWindowStart = now;
    }
}

} // namespace RCSniffer
