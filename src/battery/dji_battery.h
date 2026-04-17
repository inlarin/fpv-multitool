#pragma once
#include <Arduino.h>

// Smart Battery via SMBus/SBS — supports DJI and generic SBS batteries
// DJI BMS: TI BQ9003 (BQ40z307 FW), address 0x0B
// Generic: any SBS-compliant battery at 0x0B

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
// Device type — auto-detected on I2C scan
// =====================================================================
enum BatteryDeviceType {
    DEV_NONE,           // nothing on I2C
    DEV_DJI_BATTERY,    // DJI Smart Battery (ManufacturerName contains "DJI" or "Texas Instruments")
    DEV_GENERIC_SBS,    // any other SBS-compliant battery at 0x0B
};

// =====================================================================
// Battery info — all readable data from SBS (no unseal required)
// =====================================================================
struct BatteryInfo {
    bool connected;
    BatteryDeviceType deviceType;

    // Basic SBS
    uint16_t voltage_mV;
    int16_t  current_mA;
    int16_t  avgCurrent_mA;         // 0x0B — smoothed current
    float    temperature_C;
    uint16_t stateOfCharge;         // 0x0D — relative SOC %
    uint16_t absoluteSOC;           // 0x0E — absolute SOC %
    uint16_t fullCapacity_mAh;
    uint16_t designCapacity_mAh;
    uint16_t remainCapacity_mAh;
    uint16_t cycleCount;
    uint16_t batteryStatus;         // 0x16
    uint16_t designVoltage_mV;      // 0x19
    uint16_t cellVoltage[4];        // 0x3C-0x3F (unsynced)
    uint8_t  cellCount;             // auto-detected from voltages or name
    uint16_t serialNumber;
    uint16_t manufactureDate;
    String   manufacturerName;
    String   deviceName;
    String   chemistry;

    // Computed health / estimates (standard SBS)
    uint16_t stateOfHealth;         // 0x4F — health %
    uint16_t runTimeToEmpty_min;    // 0x11
    uint16_t avgTimeToEmpty_min;    // 0x12
    uint16_t timeToFull_min;        // 0x13
    uint16_t chargingCurrent_mA;    // 0x14 — recommended
    uint16_t chargingVoltage_mV;    // 0x15 — recommended

    // DJI-specific
    String   djiSerial;             // 0xD8 — DJI custom serial string
    uint32_t djiPF2;                // 0x4062 via ManufacturerBlockAccess

    // Extended status (32-bit registers — TI BQ-specific, may fail on non-DJI)
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
    float    cellTemp[4];           // per-cell temperatures (if available)
    float    fetTemp;

    // Chip identification
    uint16_t chipType;              // 0x4307 = BQ40z307, 0x0550 = BQ30z55
    uint16_t firmwareVersion;       // from MAC 0x0002
    uint16_t hardwareVersion;       // from MAC 0x0003

    // Detection
    BatteryModel model;
    bool sealed;                    // decoded from operationStatus SEC bits
    bool hasPF;                     // PFStatus != 0 or PF2 != 0
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

// Auto-detect what is on the I2C bus (DJI / generic SBS / nothing)
BatteryDeviceType detectDeviceType();

// Main info read (safe, no unseal). Reads SBS-standard for any battery,
// plus DJI-extended registers if deviceType == DEV_DJI_BATTERY.
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

// Cell count auto-detection from DeviceName or non-zero cell voltages
uint8_t detectCellCount(const String &deviceName, const uint16_t cellV[4]);

// Chip/FW detection via ManufacturerBlockAccess
uint16_t readChipType();            // MAC 0x0001
uint16_t readFirmwareVer();         // MAC 0x0002
uint16_t readHardwareVer();         // MAC 0x0003

// DJI-specific reads
String   readDJISerial();           // 0xD8
uint32_t readDJIPF2();              // ManufacturerBlockAccess 0x4062

// ============= Service operations =============
UnsealResult unseal();
UnsealResult unsealWithKey(uint32_t key_combined);
bool clearPFProper();               // standard PF (0x29 + 0x54 + 0x41)
bool clearDJIPF2();                 // DJI custom PF2 at 0x4062
bool seal();
bool softReset();

// ============= Status decoding =============
String decodeOperationStatus(uint32_t st);
String decodePFStatus(uint32_t pf);
String decodeSafetyStatus(uint32_t ss);
String decodeManufacturingStatus(uint32_t ms);
String decodeBatteryStatus(uint16_t bs);

} // namespace DJIBattery
