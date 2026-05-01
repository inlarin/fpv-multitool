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

// HMAC-SHA1 challenge-response unseal (BQ40Z50/BQ40Z307 newer firmware).
// key = exactly 32 bytes (BQ authentication key).
// Flow: host sends 2 challenge requests, device replies with 20B random data;
// host computes HMAC-SHA1(key, challenge) and writes 20B response.
// If challenge_out != nullptr, captures the 20B challenge for debugging.
UnsealResult unsealHmac(const uint8_t key[32], uint8_t challenge_out[20] = nullptr);

// Mavic-3 Full Access State (Unsealed -> FullAccess). Required after
// unseal() before destructive ops -- without FAS the BMS silently
// rejects PF clear / cycle reset / DataFlash writes. Keys 0xBF17/0xE0BC
// recovered from the commercial battery-service tool firmware
// (research/dji_battery_tool/FULL_REVERSE_REPORT.md).
//
// Returns true if BMS reports SEC=00 (FullAccess) after the writes.
bool enterFullAccess();

// DJI auth-bypass packet (MAC 0x4062 + magic 0x67452301 = MD5 IV[0]).
// Mavic-3 firmware gates destructive operations behind this packet --
// without it, cycle reset / capacity write / black-box clear silently
// fail. Send between FAS and the destructive MAC subcommand.
bool sendAuthBypass();

// Composite "do everything required to unlock destructive ops" --
// unseal + FAS + auth-bypass with retries and verification at each step.
// Returns true only if all three succeed and BMS reports SEC=00.
bool unlockForServiceOps();

// Permanent Failure clear -- full 14-step ritual reverse-engineered from
// the commercial tool's function 0x42002670. Includes Unseal+FAS x2
// (redundancy), PFEnable, the inline 0x7F write, auth-bypass,
// MA readback, Black Box clear, and LifetimeData reset. Replaces the
// older 3-step (0x29 + 0x54 + 0x41) which only worked on pre-Mavic-3
// packs.
bool clearPFProper();
bool clearDJIPF2();                 // legacy DJI custom PF2 at 0x4062

// Black Box (event log) clear -- MAC subcommand 0x0030. Wipes the BMS's
// on-chip event log so accumulated trip records don't keep flagging the
// pack as "service required" on a host that reads it.
bool clearBlackBox();

// LifetimeData reset -- MAC subcommand 0x0060 with 4-byte zero argument.
// Resets the running cumulative counters (total mAh charged/discharged,
// max temp seen, etc) that some hosts read out for warranty checks.
bool resetLifetimeData();

// Cycle Count + chemical-capacity reset. Issues MAC 0x9013 ResetLearnedData
// after auth-bypass. Returns true if CycleCount reads 0 afterwards.
// This is the "make the pack look new" operation -- DJI hosts use cycles
// as one input to their "this pack is worn" decision.
bool resetCycles();

// DataFlash write primitive: writes a 16-bit value at (subclass, offset).
// Subclass 0x52 = GasGauging on BQ40Z80. Implements the standard TI
// DF write protocol: REG_DFCLASS -> REG_DFBLOCK -> read existing 32B ->
// patch the byte -> write back -> compute checksum -> write to BLOCKCHKSUM.
bool writeDataFlashU16(uint8_t subclass, uint8_t offset, uint16_t value);

// Mavic-3 Design-Capacity edit: writes new nominal mAh into the pack's
// GasGauging DataFlash subclass. Updates DesignCapacity (mAh + cWh) and
// per-cell Q Max (4 cells). Triggers MAC 0x0021 LearnCycle so the BMS
// re-reads its DataFlash. Range 5000-15000 mAh, 1000-step (commercial
// tool's UI constraint -- the BMS itself accepts arbitrary values but
// out-of-range may cause Impedance Track to fail).
bool writeCapacity(uint16_t new_mah);

// Trigger active cell balancing for the cells in cellMask (bit 0 = cell1
// ... bit 3 = cell4). MAC subcommand 0x002A + 1-byte mask. Pack must be
// unsealed; balancing runs autonomously after.
bool startBalancing(uint8_t cellMask);

// Trigger Impedance Track learning cycle. MAC subcommand 0x0021. Pack
// must then go through a full charge -> rest -> discharge -> rest -> charge
// sequence (user-driven) for the BMS to capture chemistry parameters.
bool startCalibration();

// BQ40Z80 firmware (Patch.bin) flash via ROM bootloader mode.
//   1. Unseal + FAS + auth-bypass
//   2. MAC 0x0F00 -> chip restarts in ROM mode at I2C address 0x0B
//   3. Stream the patch file content with per-record verify
//   4. MAC 0x0F01 (or restart) -> chip exits ROM mode
//
// Container format inferred from research/dji_battery_tool/REPRODUCIBLE_ALGORITHMS.md.
// Best-effort reconstruction -- verify with logic-analyser before flashing
// a real Mavic-3 pack.
//
// `progress` callback is invoked every 4 KB with (bytes_done, total_bytes).
typedef bool (*FlashProgressCb)(uint32_t bytes_done, uint32_t total_bytes);
bool flashFirmwareFromBuffer(const uint8_t *patch_data, uint32_t patch_size,
                             FlashProgressCb progress);

bool seal();
bool softReset();

// ============= Status decoding =============
String decodeOperationStatus(uint32_t st);
String decodePFStatus(uint32_t pf);
String decodeSafetyStatus(uint32_t ss);
String decodeManufacturingStatus(uint32_t ms);
String decodeBatteryStatus(uint16_t bs);

} // namespace DJIBattery
