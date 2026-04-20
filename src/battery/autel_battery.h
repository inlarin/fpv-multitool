// DRAFT — not in src/ yet. Move to src/battery/autel_battery.h when ready.
// See research/AUTEL_BATTERY_IMPL_DRAFT.md for design doc.
#pragma once
#include <Arduino.h>

// Autel drone battery via SMBus/SBS.
// Chips: TI BQ40Z50 (EVO/EVO II/Lite/Nano), BQ40Z80 (EVO Max 4T hypothesis),
//        BQ3055 (X-Star, with MSP430 bus contention).
// No external auth chip (unlike DJI A1006) — plain SBS + internal SHA.

enum AutelChip : uint16_t {
    AUTEL_CHIP_UNKNOWN    = 0x0000,
    AUTEL_CHIP_BQ40Z50    = 0x4500,
    AUTEL_CHIP_BQ40Z50_R4 = 0x4550,
    AUTEL_CHIP_BQ40Z80    = 0x4580,
    AUTEL_CHIP_BQ3055     = 0x3055,
    AUTEL_CHIP_BQ4050     = 0x4050,
};

enum AutelModel : uint8_t {
    AUTEL_MODEL_UNKNOWN,
    AUTEL_MODEL_X_STAR,
    AUTEL_MODEL_EVO_GEN1,
    AUTEL_MODEL_EVO_II,
    AUTEL_MODEL_EVO_II_PRO_V3,
    AUTEL_MODEL_EVO_LITE,
    AUTEL_MODEL_EVO_LITE_PLUS,
    AUTEL_MODEL_EVO_NANO,
    AUTEL_MODEL_EVO_MAX_4T,
    AUTEL_MODEL_EVO_MAX_4N,
};

struct AutelBatteryInfo {
    bool      connected;
    AutelChip chip;
    AutelModel model;
    uint8_t   cellCount;            // 3 or 4

    // Standard SBS (sealed-mode readable)
    uint16_t voltage_mV;
    int16_t  current_mA;
    int16_t  avgCurrent_mA;
    float    temperature_C;
    uint16_t stateOfCharge;
    uint16_t absoluteSOC;
    uint16_t fullCapacity_mAh;
    uint16_t designCapacity_mAh;
    uint16_t designVoltage_mV;
    uint16_t remainCapacity_mAh;
    uint16_t cycleCount;
    uint16_t batteryStatus;
    uint16_t cellVoltage[7];        // BQ40Z80 supports up to 7S
    uint16_t serialNumber;
    uint16_t manufactureDate;
    String   manufacturerName;
    String   deviceName;
    String   chemistry;
    uint16_t stateOfHealth;

    // Decoded seal state (from OperationStatus SEC bits)
    bool sealed;
    bool unsealed;
    bool fullAccess;

    // BQ40-extended status via ManufacturerBlockAccess 0x44
    uint32_t safetyAlert;
    uint32_t safetyStatus;
    uint32_t pfAlert;
    uint32_t pfStatus;              // CRITICAL
    uint32_t operationStatus;
    uint32_t chargingStatus;
    uint32_t gaugingStatus;
    uint32_t manufacturingStatus;

    // DAStatus1 — synchronized cell V + pack V + current + temps
    bool     daStatus1Valid;
    uint16_t cellVoltSync[4];
    uint16_t packVoltage;
    int16_t  packCurrent;
    float    cellTemp[4];
    float    fetTemp;

    // Chip identification
    uint16_t chipType;              // echo of MAC 0x0001
    uint16_t firmwareVersion;
    uint16_t hardwareVersion;

    bool hasPF;
};

enum AutelUnsealResult : uint8_t {
    AUTEL_UNSEAL_OK,
    AUTEL_UNSEAL_ALREADY_UNSEALED,
    AUTEL_UNSEAL_REJECTED,          // key not accepted
    AUTEL_UNSEAL_NO_RESPONSE,
    AUTEL_UNSEAL_NO_KEY_AVAILABLE,
};

namespace AutelBattery {

void init();
bool isConnected();

// Detect chip via ManufacturerAccess 0x0001.
// Returns AUTEL_CHIP_UNKNOWN if nothing on bus or non-Autel chip.
AutelChip detectChip();

// Guess model from cell count + pack capacity + chip + deviceName heuristics.
AutelModel guessModel(const AutelBatteryInfo &info);
const char* modelName(AutelModel m);
const char* chipName(AutelChip c);

// Full snapshot — sealed-mode safe (read-only, no writes).
AutelBatteryInfo readAll();

// Individual reads for incremental UI updates
uint32_t readPFStatus();
uint32_t readOperationStatus();
bool     readDAStatus1(AutelBatteryInfo &info);

// ============= Service operations (REQUIRE UNSEALED STATE) =============
// All return REJECTED if sealed. User must unseal() first.

AutelUnsealResult unseal();                         // try all known Autel keys
AutelUnsealResult unsealWithKey(uint32_t key);      // manual 32-bit key

bool clearPF();                                     // MAC 0x29 + 0x54 + 0x41
bool resetCycleCount();                             // via DataFlash write
bool seal();
bool softReset();

// ============= Status decoders (reuse DJIBattery formatters where possible) =============
String decodeOperationStatus(uint32_t st);
String decodePFStatus(uint32_t pf);
String decodeSafetyStatus(uint32_t ss);

} // namespace AutelBattery
