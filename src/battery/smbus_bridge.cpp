#include "smbus_bridge.h"   // also pulls BridgeStats namespace declarations
#include "smbus.h"
#include <Wire.h>
#include "pin_config.h"
#include "core/pin_port.h"

// Storage for BridgeStats counters declared in smbus_bridge.h. Defined
// here in the shared business-logic .cpp so they link on every board
// regardless of whether the local-LCD stats screen is built.
namespace BridgeStats {
    volatile uint32_t cmdWrite    = 0;
    volatile uint32_t cmdRead     = 0;
    volatile uint32_t cmdAddrRead = 0;
    volatile uint32_t cmdPing     = 0;
    volatile uint32_t errCrc      = 0;
    volatile uint32_t errI2C      = 0;
    volatile uint32_t lastSlave   = 0;
    volatile uint32_t lastReg     = 0;
    volatile uint32_t lastStatus  = 0xFF;
    void reset() {
        cmdWrite = cmdRead = cmdAddrRead = cmdPing = 0;
        errCrc = errI2C = 0;
        lastSlave = lastReg = 0;
        lastStatus = 0xFF;
    }
}

namespace {

constexpr uint8_t FRAME_PC   = 0xAA;
constexpr uint8_t FRAME_ESP  = 0x55;

constexpr uint8_t CMD_WRITE       = 0x01;
constexpr uint8_t CMD_READ_WORD   = 0x02;
constexpr uint8_t CMD_READ_BLOCK  = 0x03;
constexpr uint8_t CMD_READ_RAW    = 0x04;
constexpr uint8_t CMD_ADDR_READ   = 0x05;
constexpr uint8_t CMD_PING        = 0x10;
constexpr uint8_t CMD_RESCAN      = 0x11;

constexpr uint8_t ST_OK           = 0x00;
constexpr uint8_t ST_I2C_NACK     = 0x01;
constexpr uint8_t ST_I2C_TIMEOUT  = 0x02;
constexpr uint8_t ST_INVALID_CMD  = 0x03;
constexpr uint8_t ST_CRC_ERROR    = 0x04;
constexpr uint8_t ST_OVERFLOW     = 0x05;

bool s_active = false;

uint8_t crc8(const uint8_t *b, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int j = 0; j < 8; j++) c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
    }
    return c;
}

void sendResp(uint8_t status, const uint8_t *data, uint8_t len) {
    uint8_t hdr[3] = {FRAME_ESP, status, len};
    Serial.write(hdr, 3);
    if (len) Serial.write(data, len);
    uint8_t c = crc8(hdr, 3);
    if (len) c = crc8(data, len) ^ c;  // simple continuation; PC recomputes over hdr+data
    // Simpler: compute CRC over full (hdr+data) in one pass
    uint8_t full[3 + 255];
    memcpy(full, hdr, 3);
    if (len) memcpy(full + 3, data, len);
    c = crc8(full, 3 + len);
    Serial.write(c);
}

void handleCmd(uint8_t cmd, const uint8_t *args, uint8_t argLen) {
    uint8_t out[256];
    switch (cmd) {
    case CMD_PING: {
        BridgeStats::cmdPing++;
        out[0] = 'P'; out[1] = 'O'; out[2] = 'N'; out[3] = 'G';
        sendResp(ST_OK, out, 4);
        return;
    }
    case CMD_WRITE: {
        if (argLen < 3) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t slave = args[0];
        uint8_t reg   = args[1];
        uint8_t dLen  = args[2];
        if (argLen < 3 + dLen) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        BridgeStats::cmdWrite++;
        BridgeStats::lastSlave = slave;
        BridgeStats::lastReg = reg;
        Wire1.beginTransmission(slave);
        Wire1.write(reg);
        for (uint8_t i = 0; i < dLen; i++) Wire1.write(args[3 + i]);
        uint8_t err = Wire1.endTransmission();
        BridgeStats::lastStatus = err;
        if (err != 0) BridgeStats::errI2C++;
        sendResp(err == 0 ? ST_OK : ST_I2C_NACK, nullptr, 0);
        return;
    }
    case CMD_READ_WORD: {
        if (argLen < 2) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t slave = args[0];
        uint8_t reg   = args[1];
        BridgeStats::cmdRead++;
        BridgeStats::lastSlave = slave;
        BridgeStats::lastReg = reg;
        Wire1.beginTransmission(slave);
        Wire1.write(reg);
        if (Wire1.endTransmission(false) != 0) { sendResp(ST_I2C_NACK, nullptr, 0); return; }
        Wire1.requestFrom(slave, (uint8_t)2);
        uint32_t t0 = millis();
        while (Wire1.available() < 2) {
            if (millis() - t0 > 100) { sendResp(ST_I2C_TIMEOUT, nullptr, 0); return; }
            delay(1);
        }
        out[0] = Wire1.read();
        out[1] = Wire1.read();
        sendResp(ST_OK, out, 2);
        return;
    }
    case CMD_READ_BLOCK: {
        if (argLen < 3) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t slave  = args[0];
        uint8_t reg    = args[1];
        uint8_t maxLen = args[2];
        if (maxLen > 250) maxLen = 250;
        Wire1.beginTransmission(slave);
        Wire1.write(reg);
        if (Wire1.endTransmission(false) != 0) { sendResp(ST_I2C_NACK, nullptr, 0); return; }
        // Block read: first byte is length, then data
        Wire1.requestFrom(slave, (uint8_t)(maxLen + 1));
        uint32_t t0 = millis();
        while (Wire1.available() < 1) {
            if (millis() - t0 > 150) { sendResp(ST_I2C_TIMEOUT, nullptr, 0); return; }
            delay(1);
        }
        uint8_t blkLen = Wire1.read();
        if (blkLen > maxLen) blkLen = maxLen;
        uint8_t readCnt = 0;
        t0 = millis();
        while (readCnt < blkLen) {
            if (Wire1.available()) out[readCnt++] = Wire1.read();
            else if (millis() - t0 > 100) break;
        }
        sendResp(ST_OK, out, readCnt);
        return;
    }
    case CMD_READ_RAW: {
        // Raw I2C read with no preceding register write
        if (argLen < 2) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t slave = args[0];
        uint8_t rLen  = args[1];
        if (rLen > 250) rLen = 250;
        Wire1.requestFrom(slave, rLen);
        uint32_t t0 = millis();
        uint8_t rd = 0;
        while (rd < rLen) {
            if (Wire1.available()) out[rd++] = Wire1.read();
            else if (millis() - t0 > 100) break;
        }
        sendResp(ST_OK, out, rd);
        return;
    }
    case CMD_ADDR_READ: {
        // Addressed read: write N target bytes, then repeated start + read
        if (argLen < 2) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t slave = args[0];
        uint8_t tLen  = args[1];
        BridgeStats::cmdAddrRead++;
        BridgeStats::lastSlave = slave;
        if (tLen > 0) BridgeStats::lastReg = args[2];
        if (argLen < 2 + tLen + 1) { sendResp(ST_INVALID_CMD, nullptr, 0); return; }
        uint8_t rLen  = args[2 + tLen];
        if (rLen > 250) rLen = 250;
        Wire1.beginTransmission(slave);
        for (uint8_t i = 0; i < tLen; i++) Wire1.write(args[2 + i]);
        if (Wire1.endTransmission(false) != 0) { sendResp(ST_I2C_NACK, nullptr, 0); return; }
        Wire1.requestFrom(slave, rLen);
        uint32_t t0 = millis();
        uint8_t rd = 0;
        while (rd < rLen) {
            if (Wire1.available()) out[rd++] = Wire1.read();
            else if (millis() - t0 > 100) break;
        }
        sendResp(ST_OK, out, rd);
        return;
    }
    case CMD_RESCAN: {
        Wire1.end();
        delay(10);
        int sda = PinPort::sda_pin(PinPort::PORT_B);
        int scl = PinPort::scl_pin(PinPort::PORT_B);
        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        Wire1.begin(sda, scl);
        Wire1.setClock(100000);
        Wire1.setTimeOut(50);
        sendResp(ST_OK, nullptr, 0);
        return;
    }
    default:
        sendResp(ST_INVALID_CMD, nullptr, 0);
        return;
    }
}

// State machine for incoming frame: [0xAA][CMD][LEN][ARGS...][CRC]
enum RxState { R_SYNC, R_CMD, R_LEN, R_DATA, R_CRC };
RxState rxs = R_SYNC;
uint8_t rxCmd = 0, rxLen = 0, rxPos = 0;
uint8_t rxBuf[258];

} // namespace

void SMBusBridge::begin() {
    // Wire1 already initialized by DJIBattery::init() on BATT_SDA/BATT_SCL
}

bool SMBusBridge::isActive() { return s_active; }

void SMBusBridge::loop() {
    while (Serial.available()) {
        uint8_t b = Serial.read();
        switch (rxs) {
        case R_SYNC:
            if (b == FRAME_PC) { rxs = R_CMD; rxBuf[0] = b; }
            break;
        case R_CMD:
            rxCmd = b; rxBuf[1] = b; rxs = R_LEN;
            break;
        case R_LEN:
            rxLen = b; rxBuf[2] = b; rxPos = 0;
            rxs = (rxLen == 0) ? R_CRC : R_DATA;
            break;
        case R_DATA:
            rxBuf[3 + rxPos++] = b;
            if (rxPos >= rxLen) rxs = R_CRC;
            break;
        case R_CRC: {
            uint8_t c = crc8(rxBuf, 3 + rxLen);
            if (c == b) {
                s_active = true;
                handleCmd(rxCmd, rxBuf + 3, rxLen);
            } else {
                uint8_t dummy = 0;
                // send CRC error response
                uint8_t resp[4] = {FRAME_ESP, ST_CRC_ERROR, 0, 0};
                resp[3] = crc8(resp, 3);
                Serial.write(resp, 4);
            }
            rxs = R_SYNC;
            break;
        }
        }
    }
}
