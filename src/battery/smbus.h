#pragma once
#include <Arduino.h>
#include <Wire.h>

// SMBus/SBS (Smart Battery System) communication layer
namespace SMBus {

void init(uint8_t sda, uint8_t scl);
bool devicePresent(uint8_t addr);

// Standard SBS reads
uint16_t readWord(uint8_t addr, uint8_t reg);
uint32_t readDword(uint8_t addr, uint8_t reg);          // 4 bytes (status registers)
int readBlock(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t maxLen);
String readString(uint8_t addr, uint8_t reg);

// Standard SBS writes
bool writeWord(uint8_t addr, uint8_t reg, uint16_t value);
bool writeBlock(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len);

// ManufacturerAccess (MAC) sub-commands via register 0x00 (word) or 0x44 (block)
bool macCommand(uint8_t addr, uint16_t subcommand);     // write subcommand to 0x00
int  macBlockRead(uint8_t addr, uint16_t subcommand, uint8_t *buf, uint8_t maxLen);

// I2C preflight diagnostics
struct PreflightResult {
    bool sdaOk;       // SDA line reads HIGH (pull-up working)
    bool sclOk;       // SCL line reads HIGH
    bool busOk;       // Wire begin succeeded
    bool batteryAck;  // 0x0B ACKs
    uint8_t devCount; // total devices found on scan
    uint8_t devAddrs[8]; // first 8 found addresses
};
PreflightResult preflight();

// ===== Transaction log (ring buffer) =====
enum LogOp : uint8_t {
    LOG_READ_WORD, LOG_READ_BLOCK, LOG_WRITE_WORD, LOG_WRITE_BLOCK,
    LOG_MAC_CMD, LOG_MAC_BLOCK_READ, LOG_READ_DWORD
};

struct LogEntry {
    uint32_t ts;       // millis()
    LogOp    op;
    uint8_t  addr;
    uint8_t  reg;
    bool     ok;
    int16_t  len;      // response length or -1
    uint8_t  data[8];  // first 8 bytes of response
};

static const int LOG_SIZE = 64;
uint32_t logSeq();                          // monotonic counter
int      logDump(LogEntry *out, int max);   // copy up to max entries, returns count
void     logEnable(bool on);               // enable/disable logging (default: off for perf)
bool     logEnabled();

} // namespace SMBus
