// DRAFT — not in src/ yet. Move to src/battery/autel_battery.cpp when ready.
//
// Architecture: thin application layer over existing SMBus:: transport.
// Does NOT duplicate DJI-specific crypto (A1006/ManufacturerBlockAccess wrappers).
// Plain SBS 1.1 + TI BQ40Z/BQ3055 MAC commands.
//
// Move plan:
//   1. Copy this file to src/battery/autel_battery.cpp
//   2. Copy autel_battery.h to src/battery/
//   3. Call AutelBattery::init() after DJIBattery::init() in main.cpp
//   4. Add web UI tab (see battery_wizard_routes.cpp draft)

#include "autel_battery.h"
#include "smbus.h"
#include "pin_config.h"
#include "core/pin_port.h"

static const uint8_t BATT_ADDR = 0x0B;

// Standard SBS registers (identical to DJI — could share via sbs_common.h later)
static const uint8_t REG_TEMPERATURE        = 0x08;
static const uint8_t REG_VOLTAGE            = 0x09;
static const uint8_t REG_CURRENT            = 0x0A;
static const uint8_t REG_AVG_CURRENT        = 0x0B;
static const uint8_t REG_REL_SOC            = 0x0D;
static const uint8_t REG_ABS_SOC            = 0x0E;
static const uint8_t REG_REMAIN_CAP         = 0x0F;
static const uint8_t REG_FULL_CHARGE_CAP    = 0x10;
static const uint8_t REG_BATTERY_STATUS     = 0x16;
static const uint8_t REG_CYCLE_COUNT        = 0x17;
static const uint8_t REG_DESIGN_CAP         = 0x18;
static const uint8_t REG_DESIGN_VOLTAGE     = 0x19;
static const uint8_t REG_MANUFACTURE_DATE   = 0x1B;
static const uint8_t REG_SERIAL_NUMBER      = 0x1C;
static const uint8_t REG_MANUFACTURER_NAME  = 0x20;
static const uint8_t REG_DEVICE_NAME        = 0x21;
static const uint8_t REG_DEVICE_CHEMISTRY   = 0x22;
static const uint8_t REG_CELL4              = 0x3C;
static const uint8_t REG_CELL3              = 0x3D;
static const uint8_t REG_CELL2              = 0x3E;
static const uint8_t REG_CELL1              = 0x3F;
static const uint8_t REG_MFR_ACCESS         = 0x00;
static const uint8_t REG_MFR_BLOCK_ACCESS   = 0x44;
static const uint8_t REG_STATE_OF_HEALTH    = 0x4F;

// MAC subcommands (via 0x44 block access)
static const uint16_t MAC_DEVICE_TYPE       = 0x0001;
static const uint16_t MAC_FIRMWARE_VER      = 0x0002;
static const uint16_t MAC_HARDWARE_VER      = 0x0003;
static const uint16_t MAC_SAFETY_ALERT      = 0x0050;
static const uint16_t MAC_SAFETY_STATUS     = 0x0051;
static const uint16_t MAC_PF_ALERT          = 0x0052;
static const uint16_t MAC_PF_STATUS         = 0x0053;
static const uint16_t MAC_OPERATION_STATUS  = 0x0054;
static const uint16_t MAC_CHARGING_STATUS   = 0x0055;
static const uint16_t MAC_GAUGING_STATUS    = 0x0056;
static const uint16_t MAC_MFG_STATUS        = 0x0057;
static const uint16_t MAC_DA_STATUS1        = 0x0071;
static const uint16_t MAC_DA_STATUS2        = 0x0072;
static const uint16_t MAC_DEVICE_RESET      = 0x0041;
static const uint16_t MAC_PF_DATA_RESET     = 0x0029;
static const uint16_t MAC_SEAL_DEVICE       = 0x0030;

// ---- Unseal keys table ----
// Legacy Autel (X-Star, EVO Gen1, EVO II v1.x) accept default TI.
// Modern (EVO Max 4T, EVO II Pro V3) need custom Autel key — TBD via firmware RE.
struct UnsealAttempt { uint16_t w1, w2; const char *desc; };
static const UnsealAttempt KEYS_LEGACY[] = {
    { 0x0414, 0x3672, "TI default" },
    { 0x3672, 0x0414, "TI default reversed" },
    { 0xFFFF, 0xFFFF, "TI factory" },
};
static const int KEYS_LEGACY_COUNT = sizeof(KEYS_LEGACY) / sizeof(KEYS_LEGACY[0]);

// =====================================================================
// Init
// =====================================================================
void AutelBattery::init() {
    // Share Wire1/Port B with DJIBattery — both use same 0x0B address.
    // PinPort::acquire returns true if already acquired by us or free.
    if (!PinPort::acquire(PinPort::PORT_B, PORT_I2C, "autel_battery")) {
        Serial.println("[AutelBattery] Port B busy, skipping");
        return;
    }
    // SMBus::init is idempotent if already inited — safe to call again.
    SMBus::init(PinPort::sda_pin(PinPort::PORT_B),
                PinPort::scl_pin(PinPort::PORT_B));
}

bool AutelBattery::isConnected() {
    return SMBus::devicePresent(BATT_ADDR);
}

// =====================================================================
// Chip detection via MAC 0x0001
// =====================================================================
AutelChip AutelBattery::detectChip() {
    uint8_t buf[8];
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_DEVICE_TYPE, buf, sizeof(buf));
    if (n < 2) return AUTEL_CHIP_UNKNOWN;
    // First 2 bytes of block = echo of subcommand; actual data starts at buf[2]
    // (but SMBus::macBlockRead already strips the echo — check impl).
    uint16_t dt = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    switch (dt) {
        case 0x4500: case 0x4550: case 0x4580: case 0x3055: case 0x4050:
            return (AutelChip)dt;
        default:
            return AUTEL_CHIP_UNKNOWN;
    }
}

const char* AutelBattery::chipName(AutelChip c) {
    switch (c) {
        case AUTEL_CHIP_BQ40Z50:    return "BQ40Z50";
        case AUTEL_CHIP_BQ40Z50_R4: return "BQ40Z50-R4";
        case AUTEL_CHIP_BQ40Z80:    return "BQ40Z80";
        case AUTEL_CHIP_BQ3055:     return "BQ3055";
        case AUTEL_CHIP_BQ4050:     return "BQ4050";
        default:                    return "Unknown";
    }
}

// =====================================================================
// Model guessing (heuristic — not authoritative)
// =====================================================================
AutelModel AutelBattery::guessModel(const AutelBatteryInfo &info) {
    // Priority: deviceName substring > cellCount+capacity > chip
    if (info.deviceName.indexOf("L884") >= 0) return AUTEL_MODEL_EVO_MAX_4T;
    if (info.deviceName.indexOf("EVO2") >= 0) return AUTEL_MODEL_EVO_II;
    if (info.deviceName.indexOf("XSTAR") >= 0) return AUTEL_MODEL_X_STAR;

    if (info.chip == AUTEL_CHIP_BQ3055) return AUTEL_MODEL_X_STAR;
    if (info.chip == AUTEL_CHIP_BQ40Z80 && info.cellCount == 4) return AUTEL_MODEL_EVO_MAX_4T;

    if (info.cellCount == 3) {
        if (info.designCapacity_mAh > 6800)  return AUTEL_MODEL_EVO_II;        // 7100
        if (info.designCapacity_mAh > 6200)  return AUTEL_MODEL_EVO_LITE;      // 6350
        if (info.designCapacity_mAh > 2000)  return AUTEL_MODEL_EVO_NANO;      // 2250
        if (info.designCapacity_mAh > 4000)  return AUTEL_MODEL_EVO_GEN1;      // 4300
    }
    return AUTEL_MODEL_UNKNOWN;
}

const char* AutelBattery::modelName(AutelModel m) {
    switch (m) {
        case AUTEL_MODEL_X_STAR:        return "Autel X-Star Premium";
        case AUTEL_MODEL_EVO_GEN1:      return "Autel EVO (Gen 1)";
        case AUTEL_MODEL_EVO_II:        return "Autel EVO II / Pro / Dual";
        case AUTEL_MODEL_EVO_II_PRO_V3: return "Autel EVO II Pro V3";
        case AUTEL_MODEL_EVO_LITE:      return "Autel EVO Lite";
        case AUTEL_MODEL_EVO_LITE_PLUS: return "Autel EVO Lite+";
        case AUTEL_MODEL_EVO_NANO:      return "Autel EVO Nano";
        case AUTEL_MODEL_EVO_MAX_4T:    return "Autel EVO Max 4T";
        case AUTEL_MODEL_EVO_MAX_4N:    return "Autel EVO Max 4N";
        default:                        return "Unknown Autel";
    }
}

// =====================================================================
// Cell voltage auto-detection
// =====================================================================
static uint8_t detectCellCount(const uint16_t cells[7]) {
    uint8_t count = 0;
    for (int i = 0; i < 7; i++) {
        // Valid cell: 2.5V-4.5V range
        if (cells[i] >= 2500 && cells[i] <= 4500) count++;
    }
    return count;
}

// =====================================================================
// Full snapshot (sealed-mode safe)
// =====================================================================
AutelBatteryInfo AutelBattery::readAll() {
    AutelBatteryInfo info = {};
    if (!isConnected()) {
        info.connected = false;
        return info;
    }
    info.connected = true;

    // --- Identity ---
    info.chip            = detectChip();
    info.chipType        = (uint16_t)info.chip;
    info.manufacturerName = SMBus::readString(BATT_ADDR, REG_MANUFACTURER_NAME);
    info.deviceName       = SMBus::readString(BATT_ADDR, REG_DEVICE_NAME);
    info.chemistry        = SMBus::readString(BATT_ADDR, REG_DEVICE_CHEMISTRY);
    info.serialNumber     = SMBus::readWord(BATT_ADDR, REG_SERIAL_NUMBER);
    info.manufactureDate  = SMBus::readWord(BATT_ADDR, REG_MANUFACTURE_DATE);

    // --- Basic SBS ---
    info.voltage_mV           = SMBus::readWord(BATT_ADDR, REG_VOLTAGE);
    info.current_mA           = (int16_t)SMBus::readWord(BATT_ADDR, REG_CURRENT);
    info.avgCurrent_mA        = (int16_t)SMBus::readWord(BATT_ADDR, REG_AVG_CURRENT);
    uint16_t tempK10          = SMBus::readWord(BATT_ADDR, REG_TEMPERATURE);
    info.temperature_C        = (tempK10 - 2731) / 10.0f;
    info.stateOfCharge        = SMBus::readWord(BATT_ADDR, REG_REL_SOC);
    info.absoluteSOC          = SMBus::readWord(BATT_ADDR, REG_ABS_SOC);
    info.remainCapacity_mAh   = SMBus::readWord(BATT_ADDR, REG_REMAIN_CAP);
    info.fullCapacity_mAh     = SMBus::readWord(BATT_ADDR, REG_FULL_CHARGE_CAP);
    info.designCapacity_mAh   = SMBus::readWord(BATT_ADDR, REG_DESIGN_CAP);
    info.designVoltage_mV     = SMBus::readWord(BATT_ADDR, REG_DESIGN_VOLTAGE);
    info.cycleCount           = SMBus::readWord(BATT_ADDR, REG_CYCLE_COUNT);
    info.batteryStatus        = SMBus::readWord(BATT_ADDR, REG_BATTERY_STATUS);
    info.stateOfHealth        = SMBus::readWord(BATT_ADDR, REG_STATE_OF_HEALTH);

    // --- Cell voltages 0x3C-0x3F (cells 4-1 on BQ40Z50, cells 7-4 on BQ40Z80) ---
    info.cellVoltage[3] = SMBus::readWord(BATT_ADDR, REG_CELL1);  // hi to lo
    info.cellVoltage[2] = SMBus::readWord(BATT_ADDR, REG_CELL2);
    info.cellVoltage[1] = SMBus::readWord(BATT_ADDR, REG_CELL3);
    info.cellVoltage[0] = SMBus::readWord(BATT_ADDR, REG_CELL4);
    // BQ40Z80 cells 1-3 only via DASTATUS1 (below)
    info.cellCount = detectCellCount(info.cellVoltage);

    // --- Extended 32-bit status via MAC ---
    info.safetyAlert         = SMBus::readDword(BATT_ADDR, 0x50);
    info.safetyStatus        = SMBus::readDword(BATT_ADDR, 0x51);
    info.pfAlert             = SMBus::readDword(BATT_ADDR, 0x52);
    info.pfStatus            = SMBus::readDword(BATT_ADDR, 0x53);
    info.operationStatus     = SMBus::readDword(BATT_ADDR, 0x54);
    info.chargingStatus      = SMBus::readDword(BATT_ADDR, 0x55);
    info.gaugingStatus       = SMBus::readDword(BATT_ADDR, 0x56);
    info.manufacturingStatus = SMBus::readDword(BATT_ADDR, 0x57);

    // Decode seal state from OperationStatus bits [9:8]
    uint8_t sec = (info.operationStatus >> 8) & 0x3;
    info.sealed     = (sec == 0b11);
    info.unsealed   = (sec == 0b10);
    info.fullAccess = (sec == 0b01);

    // --- DASTATUS1 (synchronized sample) ---
    info.daStatus1Valid = readDAStatus1(info);

    // --- Derived ---
    info.hasPF = (info.pfStatus != 0);

    // --- Model guess (last, after everything is filled) ---
    info.model = guessModel(info);

    return info;
}

// =====================================================================
// DASTATUS1 via MAC 0x0071 — 32-byte block with synchronized cells+V+I+T
// =====================================================================
bool AutelBattery::readDAStatus1(AutelBatteryInfo &info) {
    uint8_t buf[32];
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_DA_STATUS1, buf, sizeof(buf));
    if (n < 16) return false;
    // BQ40Z50 layout (16 bytes): cellV[4]×2B, packV×2B, packI×2B, ...
    // BQ40Z80 layout differs — TBD.
    info.cellVoltSync[0] = (uint16_t)buf[0]  | ((uint16_t)buf[1]  << 8);
    info.cellVoltSync[1] = (uint16_t)buf[2]  | ((uint16_t)buf[3]  << 8);
    info.cellVoltSync[2] = (uint16_t)buf[4]  | ((uint16_t)buf[5]  << 8);
    info.cellVoltSync[3] = (uint16_t)buf[6]  | ((uint16_t)buf[7]  << 8);
    info.packVoltage     = (uint16_t)buf[8]  | ((uint16_t)buf[9]  << 8);
    info.packCurrent     = (int16_t)((uint16_t)buf[12] | ((uint16_t)buf[13] << 8));
    return true;
}

// =====================================================================
// Individual status reads
// =====================================================================
uint32_t AutelBattery::readPFStatus()         { return SMBus::readDword(BATT_ADDR, 0x53); }
uint32_t AutelBattery::readOperationStatus()  { return SMBus::readDword(BATT_ADDR, 0x54); }

// =====================================================================
// Unseal flow — try all legacy keys
// =====================================================================
AutelUnsealResult AutelBattery::unseal() {
    // Pre-check: maybe already unsealed
    uint32_t opStatus = readOperationStatus();
    uint8_t sec = (opStatus >> 8) & 0x3;
    if (sec == 0b10 || sec == 0b01) return AUTEL_UNSEAL_ALREADY_UNSEALED;

    for (int i = 0; i < KEYS_LEGACY_COUNT; i++) {
        if (unsealWithKey(((uint32_t)KEYS_LEGACY[i].w2 << 16) | KEYS_LEGACY[i].w1) == AUTEL_UNSEAL_OK) {
            Serial.printf("[AutelBattery] Unsealed with key '%s'\n", KEYS_LEGACY[i].desc);
            return AUTEL_UNSEAL_OK;
        }
    }
    return AUTEL_UNSEAL_REJECTED;
}

AutelUnsealResult AutelBattery::unsealWithKey(uint32_t key) {
    uint16_t w1 = (uint16_t)(key & 0xFFFF);
    uint16_t w2 = (uint16_t)((key >> 16) & 0xFFFF);

    // Bus must be quiet for >=4s before and both writes within 4s window
    if (!SMBus::busLock(1000)) return AUTEL_UNSEAL_NO_RESPONSE;
    bool ok1 = SMBus::writeWord(BATT_ADDR, REG_MFR_ACCESS, w1);
    bool ok2 = SMBus::writeWord(BATT_ADDR, REG_MFR_ACCESS, w2);
    SMBus::busUnlock();
    if (!ok1 || !ok2) return AUTEL_UNSEAL_NO_RESPONSE;

    delay(50);
    uint32_t opStatus = readOperationStatus();
    uint8_t sec = (opStatus >> 8) & 0x3;
    if (sec == 0b10 || sec == 0b01) return AUTEL_UNSEAL_OK;
    return AUTEL_UNSEAL_REJECTED;
}

// =====================================================================
// Service ops — require unseal
// =====================================================================
bool AutelBattery::clearPF() {
    if (!SMBus::macCommand(BATT_ADDR, MAC_PF_DATA_RESET)) return false;
    delay(100);
    if (!SMBus::macCommand(BATT_ADDR, MAC_OPERATION_STATUS)) return false;
    delay(50);
    if (!SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET)) return false;
    delay(1000);
    return readPFStatus() == 0;
}

bool AutelBattery::resetCycleCount() {
    // Requires FullAccess + DataFlash write at chip-specific offset.
    // TODO: implement after live chip probe (offset varies by BQ revision).
    Serial.println("[AutelBattery] resetCycleCount not yet implemented");
    return false;
}

bool AutelBattery::seal()       { return SMBus::macCommand(BATT_ADDR, MAC_SEAL_DEVICE); }
bool AutelBattery::softReset()  { return SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET); }

// =====================================================================
// Status decoders — thin wrappers; heavy lifting in shared formatter
// =====================================================================
String AutelBattery::decodeOperationStatus(uint32_t st) {
    String s;
    uint8_t sec = (st >> 8) & 0x3;
    switch (sec) {
        case 0b11: s += "SEALED "; break;
        case 0b10: s += "UNSEALED "; break;
        case 0b01: s += "FULL_ACCESS "; break;
    }
    if (st & (1 << 0))  s += "PRES ";
    if (st & (1 << 2))  s += "DSG ";        // discharge FET
    if (st & (1 << 3))  s += "CHG ";        // charge FET
    if (st & (1 << 4))  s += "PCHG ";       // pre-charge FET
    if (st & (1 << 14)) s += "AUTH ";
    if (st & (1 << 20)) s += "PF ";
    return s;
}

String AutelBattery::decodePFStatus(uint32_t pf) {
    if (pf == 0) return "CLEAN";
    String s;
    if (pf & (1 << 0))  s += "SUV ";          // Safety Under Voltage
    if (pf & (1 << 1))  s += "SOV ";          // Safety Over Voltage
    if (pf & (1 << 2))  s += "SOCC ";         // Charge OC
    if (pf & (1 << 3))  s += "SOCD ";         // Discharge OC
    if (pf & (1 << 4))  s += "SOT ";          // Over Temp
    if (pf & (1 << 5))  s += "SOTF ";         // FET Over Temp
    if (pf & (1 << 12)) s += "CIM ";          // Cell Imbalance
    if (pf & (1 << 14)) s += "CHGC ";         // Charge FET fail
    if (pf & (1 << 15)) s += "DSGF ";         // Discharge FET fail
    return s;
}

String AutelBattery::decodeSafetyStatus(uint32_t ss) {
    if (ss == 0) return "OK";
    String s;
    if (ss & (1 << 0))  s += "CUV ";
    if (ss & (1 << 1))  s += "COV ";
    if (ss & (1 << 2))  s += "OCC ";
    if (ss & (1 << 3))  s += "OCD ";
    if (ss & (1 << 4))  s += "AOLD ";
    if (ss & (1 << 6))  s += "ASCD ";
    return s;
}
