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

} // namespace SMBus
