#include "smbus.h"

static TwoWire *s_wire = &Wire;

void SMBus::init(uint8_t sda, uint8_t scl) {
    s_wire->begin(sda, scl);
    s_wire->setClock(100000);
    s_wire->setTimeOut(50);
}

bool SMBus::devicePresent(uint8_t addr) {
    s_wire->beginTransmission(addr);
    return s_wire->endTransmission() == 0;
}

uint16_t SMBus::readWord(uint8_t addr, uint8_t reg) {
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    if (s_wire->endTransmission(false) != 0) return 0xFFFF;
    s_wire->requestFrom(addr, (uint8_t)2);
    if (s_wire->available() < 2) return 0xFFFF;
    uint16_t lo = s_wire->read();
    uint16_t hi = s_wire->read();
    return (hi << 8) | lo;
}

uint32_t SMBus::readDword(uint8_t addr, uint8_t reg) {
    uint8_t buf[8] = {0};
    int len = readBlock(addr, reg, buf, 8);
    if (len < 4) return 0xFFFFFFFF;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

int SMBus::readBlock(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t maxLen) {
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    if (s_wire->endTransmission(false) != 0) return -1;
    s_wire->requestFrom(addr, (uint8_t)(maxLen + 1));
    if (!s_wire->available()) return -1;
    uint8_t len = s_wire->read();
    if (len > maxLen) len = maxLen;
    for (int i = 0; i < len && s_wire->available(); i++) {
        buf[i] = s_wire->read();
    }
    return len;
}

String SMBus::readString(uint8_t addr, uint8_t reg) {
    uint8_t buf[32] = {0};
    int len = readBlock(addr, reg, buf, 31);
    if (len <= 0) return "";
    buf[len] = '\0';
    return String((char*)buf);
}

bool SMBus::writeWord(uint8_t addr, uint8_t reg, uint16_t value) {
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(value & 0xFF);
    s_wire->write((value >> 8) & 0xFF);
    return s_wire->endTransmission() == 0;
}

bool SMBus::writeBlock(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len) {
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(len);
    for (uint8_t i = 0; i < len; i++) s_wire->write(data[i]);
    return s_wire->endTransmission() == 0;
}

// Write MAC subcommand to register 0x00 (for simple commands)
bool SMBus::macCommand(uint8_t addr, uint16_t subcommand) {
    return writeWord(addr, 0x00, subcommand);
}

// Write subcommand to MAC 0x44 (ManufacturerBlockAccess) then read block response
int SMBus::macBlockRead(uint8_t addr, uint16_t subcommand, uint8_t *buf, uint8_t maxLen) {
    // Write subcommand as 2-byte block to 0x44
    uint8_t cmd[2] = { (uint8_t)(subcommand & 0xFF), (uint8_t)((subcommand >> 8) & 0xFF) };
    if (!writeBlock(addr, 0x44, cmd, 2)) return -1;
    delay(10);

    // Read block from 0x44 — first 2 bytes echo subcommand, then data
    uint8_t tmp[40] = {0};
    int total = readBlock(addr, 0x44, tmp, sizeof(tmp) - 1);
    if (total < 2) return -1;

    // Strip the 2-byte subcommand echo
    int dataLen = total - 2;
    if (dataLen < 0) dataLen = 0;
    if (dataLen > maxLen) dataLen = maxLen;
    for (int i = 0; i < dataLen; i++) buf[i] = tmp[i + 2];
    return dataLen;
}
