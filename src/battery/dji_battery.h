#pragma once
#include <Arduino.h>

// DJI Smart Battery (Mavic 3 / Mavic 2 / Air / Mini) via SMBus
// BMS: TI BQ9003 (BQ40z307 FW), address 0x0B

// =====================================================================
// Battery model — detected from DeviceName/chip
// =====================================================================
enum BatteryModel {
    MODEL_UNKNOWN,
    MODEL_SPARK,         // Spark (WM100)
    MODEL_MAVIC_PRO,     // Mavic Pro (WM220)
    MODEL_MAVIC_AIR,     // Mavic Air (WM230)
    MODEL_MAVIC_AIR2,    // Mavic Air 2 (WM231)
    MODEL_MAVIC_AIR2S,   // Mavic Air 2S (WM245)
    MODEL_MAVIC_2,       // Mavic 2 Pro/Zoom (WM240)
    MODEL_MAVIC_3,       // Mavic 3 (WM260)  <-- hard to unseal
    MODEL_MAVIC_4,       // Mavic 4 (WM460)  <-- hard to unseal
    MODEL_MINI,          // Mavic Mini (WM160)
    MODEL_MINI_2,        // Mini 2 (WM161)
    MODEL_MINI_3,        // Mini 3
    MODEL_PHANTOM_3,
    MODEL_PHANTOM_4,
};

// =====================================================================
// Battery info — all readable data from SBS (no unseal required)
// =====================================================================
struct BatteryInfo {
    bool connected;

    // Basic SBS
    uint16_t voltage_mV;
    int16_t  current_mA;
    float    temperature_C;
    uint16_t stateOfCharge;
    uint16_t fullCapacity_mAh;
    uint16_t designCapacity_mAh;
    uint16_t remainCapacity_mAh;
    uint16_t cycleCount;
    uint16_t batteryStatus;         // 0x16
    uint16_t cellVoltage[4];        // 0x3C-0x3F (unsynced)
    uint16_t serialNumber;
    uint16_t manufactureDate;
    String   manufacturerName;
    String   deviceName;
    String   chemistry;

    // Extended status (32-bit registers)
    uint32_t safetyAlert;           // 0x50
    uint32_t safetyStatus;          // 0x51 — latched trip flags
    uint32_t pfAlert;               // 0x52 — pending PF
    uint32_t pfStatus;              // 0x53 — current PF (CRITICAL)
    uint32_t operationStatus;       // 0x54 — contains SEC bits (seal state)
    uint32_t chargingStatus;        // 0x55
    uint32_t gaugingStatus;         // 0x56
    uint32_t manufacturingStatus;   // 0x57 — FETs, gauging enable

    // DAStatus1 — synchronized cell voltages + pack V + current + temps
    bool     daStatus1Valid;
    uint16_t cellVoltSync[4];       // synchronized per-cell (from 0x71)
    uint16_t packVoltage;           // synchronized pack voltage
    int16_t  packCurrent;           // synchronized current
    int16_t  avgCurrent;
    float    cellTemp[4];           // per-cell temperatures (if available)
    float    fetTemp;

    // Chip identification
    uint16_t chipType;              // 0x4307 = BQ40z307, 0x0550 = BQ30z55
    uint16_t firmwareVersion;       // from MAC 0x0002
    uint16_t hardwareVersion;       // from MAC 0x0003

    // Detection
    BatteryModel model;
    bool sealed;                    // decoded from operationStatus SEC bits
    bool hasPF;                     // PFStatus != 0
    bool supportedForService;       // unseal keys available for this model
};

// =====================================================================
// Unseal result
// =====================================================================
enum UnsealResult {
    UNSEAL_OK,
    UNSEAL_REJECTED_SEALED,         // command sent but still sealed — wrong key
    UNSEAL_NO_RESPONSE,             // I2C failure
    UNSEAL_UNSUPPORTED_MODEL,       // model requires unavailable key
};

namespace DJIBattery {

void init();
bool isConnected();

// Main info read (safe, no unseal)
BatteryInfo readAll();

// Individual reads (for incremental UI updates)
uint32_t readPFStatus();
uint32_t readSafetyStatus();
uint32_t readOperationStatus();
uint32_t readManufacturingStatus();
bool readDAStatus1(BatteryInfo &info);

// Model detection
BatteryModel detectModel(const String &deviceName, const String &mfrName, uint16_t chipType);
const char* modelName(BatteryModel m);
bool modelNeedsDjiKey(BatteryModel m);

// Chip/FW detection via ManufacturerBlockAccess
uint16_t readChipType();            // MAC 0x0001
uint16_t readFirmwareVer();         // MAC 0x0002
uint16_t readHardwareVer();         // MAC 0x0003

// ============= Service operations =============
// Try unsealing with multiple known keys
UnsealResult unseal();
UnsealResult unsealWithKey(uint32_t key_combined);

// Proper PF clear sequence (unseal → 0x29 → 0x54 → verify → 0x41 reset)
bool clearPFProper();

// Seal after service
bool seal();

// Soft reset (MAC 0x0041)
bool softReset();

// ============= Status decoding =============
String decodeOperationStatus(uint32_t st);
String decodePFStatus(uint32_t pf);
String decodeSafetyStatus(uint32_t ss);
String decodeManufacturingStatus(uint32_t ms);

} // namespace DJIBattery
