#include "dji_battery.h"
#include "smbus.h"
#include "pin_config.h"
#include "core/pin_port.h"
#include <mbedtls/md.h>  // for HMAC-SHA1 auth unseal

static const uint8_t BATT_ADDR = 0x0B;

// Standard SBS registers
static const uint8_t REG_TEMPERATURE        = 0x08;
static const uint8_t REG_VOLTAGE            = 0x09;
static const uint8_t REG_CURRENT            = 0x0A;
static const uint8_t REG_AVG_CURRENT        = 0x0B;
static const uint8_t REG_REL_SOC            = 0x0D;
static const uint8_t REG_ABS_SOC            = 0x0E;
static const uint8_t REG_REMAIN_CAP         = 0x0F;
static const uint8_t REG_FULL_CHARGE_CAP    = 0x10;
static const uint8_t REG_RUN_TIME_EMPTY     = 0x11;
static const uint8_t REG_AVG_TIME_EMPTY     = 0x12;
static const uint8_t REG_TIME_TO_FULL       = 0x13;
static const uint8_t REG_CHARGING_CURRENT   = 0x14;
static const uint8_t REG_CHARGING_VOLTAGE   = 0x15;
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
static const uint8_t REG_STATE_OF_HEALTH    = 0x4F;

// DJI-specific
static const uint8_t  REG_DJI_SERIAL        = 0xD8;
static const uint16_t MAC_DJI_PF2_READ      = 0x4062;

// Extended status registers (32-bit, read via block)
static const uint8_t REG_MFR_ACCESS         = 0x00;
static const uint8_t REG_MFR_BLOCK_ACCESS   = 0x44;
static const uint8_t REG_SAFETY_ALERT       = 0x50;
static const uint8_t REG_SAFETY_STATUS      = 0x51;
static const uint8_t REG_PF_ALERT           = 0x52;
static const uint8_t REG_PF_STATUS          = 0x53;
static const uint8_t REG_OPERATION_STATUS   = 0x54;
static const uint8_t REG_CHARGING_STATUS    = 0x55;
static const uint8_t REG_GAUGING_STATUS     = 0x56;
static const uint8_t REG_MANUFACTURING_STATUS = 0x57;
static const uint8_t REG_DA_STATUS1         = 0x71;

// MAC subcommands
static const uint16_t MAC_DEVICE_TYPE       = 0x0001;
static const uint16_t MAC_FIRMWARE_VER      = 0x0002;
static const uint16_t MAC_HARDWARE_VER      = 0x0003;
static const uint16_t MAC_DEVICE_RESET      = 0x0041;
static const uint16_t MAC_CLEAR_PF          = 0x0054;
static const uint16_t MAC_PF_DATA_RESET     = 0x0029;
static const uint16_t MAC_SEAL_DEVICE       = 0x0030;

// =====================================================================
// Known unseal keys
// =====================================================================
struct UnsealKey {
    BatteryModel model;
    uint16_t word1;
    uint16_t word2;
    const char* description;
};

// Keys table — tried in order on unseal().
// TI defaults cover older BQ30Z55 packs (Mavic Pro / Phantom / Spark / Mavic Air).
// RUS_MAV keys come from reverse-engineering hardware/RUS_MAV.bin and cover
// BQ9003 / bq40z307 packs (Mavic 2 Pro / Zoom / Air 2 / Mini 2, and per the tool
// re-brander, possibly pre-SHA256 Mavic 3 batches).
// Mavic 3 (WM260) packs with per-serial SHA-256 keys cannot be unsealed this way.
static const UnsealKey UNSEAL_KEYS[] = {
    // TI default — BQ30Z55 family
    { MODEL_UNKNOWN,     0x0414, 0x3672, "TI default" },
    { MODEL_SPARK,       0x0414, 0x3672, "TI default" },
    { MODEL_MAVIC_PRO,   0x0414, 0x3672, "TI default" },
    { MODEL_PHANTOM_3,   0x0414, 0x3672, "TI default" },
    { MODEL_PHANTOM_4,   0x0414, 0x3672, "TI default" },
    { MODEL_MAVIC_AIR,   0x0414, 0x3672, "TI default" },
    // RUS_MAV custom — BQ9003 / bq40z307 family
    { MODEL_UNKNOWN,     0x7EE0, 0xCCDF, "RUS_MAV Unseal" },
    { MODEL_MAVIC_3,     0x7EE0, 0xCCDF, "RUS_MAV Unseal" },
    { MODEL_MAVIC_4,     0x7EE0, 0xCCDF, "RUS_MAV Unseal" },
    // BQ30Z55 HMAC-SHA1 default (TI bqStudio): 0123456789ABCDEFFEDCBA9876543210
    // Not a simple key pair — requires challenge-response flow, TODO.
};
static const int UNSEAL_KEYS_COUNT = sizeof(UNSEAL_KEYS) / sizeof(UNSEAL_KEYS[0]);

// =====================================================================
// Init / probing
// =====================================================================
//
// Lazy init pattern (2026-04-30): historically `init()` was called at
// boot from main_*.cpp, which unconditionally grabbed Port B in I2C
// mode and quietly broke the Servo / Motor screens (they need PWM and
// silently failed PinPort::acquire). Now boot only inits if the user
// preselected I2C as their preferred Port B mode in NVS; otherwise
// every public DJIBattery API calls `tryEnsureInit()` which is safe
// to call from any thread / any port state.

static bool s_initialized = false;

static bool tryEnsureInit() {
    PortMode cur = PinPort::currentMode(PinPort::PORT_B);
    if (cur == PORT_I2C && s_initialized) return true;   // already good
    if (cur != PORT_IDLE && cur != PORT_I2C) {
        // Port held in UART/PWM/GPIO -- can't read I2C without disturbing
        // whoever is using it (servo, ELRS, etc). Caller will see
        // !connected and surface that to the user.
        s_initialized = false;
        return false;
    }
    if (!PinPort::acquire(PinPort::PORT_B, PORT_I2C, "battery")) return false;
    SMBus::init(PinPort::sda_pin(PinPort::PORT_B),
                PinPort::scl_pin(PinPort::PORT_B));
    s_initialized = true;
    return true;
}

void DJIBattery::init() { tryEnsureInit(); }

bool DJIBattery::isConnected() {
    if (!tryEnsureInit()) return false;
    return SMBus::devicePresent(BATT_ADDR);
}

// =====================================================================
// Chip / FW identification via MAC 0x0001/0x0002/0x0003
// Returns the MAC subcommand's 2-byte response
// =====================================================================
uint16_t DJIBattery::readChipType() {
    uint8_t buf[8] = {0};
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_DEVICE_TYPE, buf, 8);
    if (n < 2) return 0;
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

uint16_t DJIBattery::readFirmwareVer() {
    uint8_t buf[16] = {0};
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_FIRMWARE_VER, buf, 16);
    if (n < 2) return 0;
    // BQ40z307 firmware version format varies — typically bytes 2-3 are "VV.vv"
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

uint16_t DJIBattery::readHardwareVer() {
    uint8_t buf[8] = {0};
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_HARDWARE_VER, buf, 8);
    if (n < 2) return 0;
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

// =====================================================================
// Status register reads
// =====================================================================
uint32_t DJIBattery::readPFStatus() {
    return SMBus::readDword(BATT_ADDR, REG_PF_STATUS);
}

uint32_t DJIBattery::readSafetyStatus() {
    return SMBus::readDword(BATT_ADDR, REG_SAFETY_STATUS);
}

uint32_t DJIBattery::readOperationStatus() {
    return SMBus::readDword(BATT_ADDR, REG_OPERATION_STATUS);
}

uint32_t DJIBattery::readManufacturingStatus() {
    return SMBus::readDword(BATT_ADDR, REG_MANUFACTURING_STATUS);
}

// DAStatus1: 32 bytes (16 words) — synchronized cell voltages, pack, current, temps
// Layout (per BQ40z50-R2 datasheet):
//   [0-1] Cell1 voltage (mV)
//   [2-3] Cell2
//   [4-5] Cell3
//   [6-7] Cell4
//   [8-9] BAT voltage (mV)
//   [10-11] PACK voltage (mV)
//   [12-13] Cell1 current
//   [14-15] Cell2 current
//   ... varies per chip
bool DJIBattery::readDAStatus1(BatteryInfo &info) {
    uint8_t buf[32] = {0};
    int n = SMBus::readBlock(BATT_ADDR, REG_DA_STATUS1, buf, 32);
    if (n < 12) return false;

    for (int i = 0; i < 4; i++) {
        info.cellVoltSync[i] = (uint16_t)buf[i*2] | ((uint16_t)buf[i*2+1] << 8);
    }
    info.packVoltage = (uint16_t)buf[10] | ((uint16_t)buf[11] << 8);
    info.daStatus1Valid = true;
    return true;
}

// =====================================================================
// Model detection from DeviceName
// =====================================================================
BatteryModel DJIBattery::detectModel(const String &deviceName, const String &mfrName, uint16_t chipType) {
    String dn = deviceName;
    dn.toUpperCase();

    // By DeviceName patterns (DJI uses internal codenames)
    if (dn.indexOf("WM100") >= 0 || dn.indexOf("SPARK") >= 0) return MODEL_SPARK;
    if (dn.indexOf("WM220") >= 0 || dn.indexOf("MAVIC") >= 0 && dn.indexOf("PRO") >= 0) return MODEL_MAVIC_PRO;
    if (dn.indexOf("WM230") >= 0) return MODEL_MAVIC_AIR;
    if (dn.indexOf("WM231") >= 0) return MODEL_MAVIC_AIR2;
    if (dn.indexOf("WM245") >= 0) return MODEL_MAVIC_AIR2S;
    if (dn.indexOf("WM240") >= 0) return MODEL_MAVIC_2;
    if (dn.indexOf("WM260") >= 0 || dn.indexOf("BWX260") >= 0) return MODEL_MAVIC_3;
    if (dn.indexOf("WM460") >= 0 || dn.indexOf("BWX4") >= 0) return MODEL_MAVIC_4;
    if (dn.indexOf("WM160") >= 0) return MODEL_MINI;
    if (dn.indexOf("WM161") >= 0) return MODEL_MINI_2;
    if (dn.indexOf("WM163") >= 0) return MODEL_MINI_3;
    if (dn.indexOf("P330") >= 0) return MODEL_PHANTOM_3;
    if (dn.indexOf("WM331") >= 0 || dn.indexOf("WM332") >= 0) return MODEL_PHANTOM_4;

    return MODEL_UNKNOWN;
}

const char* DJIBattery::modelName(BatteryModel m) {
    switch (m) {
        case MODEL_SPARK:       return "Spark";
        case MODEL_MAVIC_PRO:   return "Mavic Pro";
        case MODEL_MAVIC_AIR:   return "Mavic Air";
        case MODEL_MAVIC_AIR2:  return "Mavic Air 2";
        case MODEL_MAVIC_AIR2S: return "Mavic Air 2S";
        case MODEL_MAVIC_2:     return "Mavic 2";
        case MODEL_MAVIC_3:     return "Mavic 3";
        case MODEL_MAVIC_4:     return "Mavic 4";
        case MODEL_MINI:        return "Mini";
        case MODEL_MINI_2:      return "Mini 2";
        case MODEL_MINI_3:      return "Mini 3";
        case MODEL_PHANTOM_3:   return "Phantom 3";
        case MODEL_PHANTOM_4:   return "Phantom 4";
        default:                return "Unknown";
    }
}

bool DJIBattery::modelNeedsDjiKey(BatteryModel m) {
    // These models use BQ40z307 with per-pack DJI key — TI defaults fail
    switch (m) {
        case MODEL_MAVIC_AIR2:
        case MODEL_MAVIC_AIR2S:
        case MODEL_MAVIC_2:      // some batches
        case MODEL_MAVIC_3:
        case MODEL_MAVIC_4:
        case MODEL_MINI_2:
        case MODEL_MINI_3:
            return true;
        default:
            return false;
    }
}

// =====================================================================
// Auto-detect device type from ManufacturerName
// =====================================================================
BatteryDeviceType DJIBattery::detectDeviceType() {
    if (!isConnected()) return DEV_NONE;
    String mfr = SMBus::readString(BATT_ADDR, REG_MANUFACTURER_NAME);
    mfr.toUpperCase();
    // Recognised DJI / DJI-clone manufacturer strings:
    //   - "DJI"          -- genuine DJI packs
    //   - "TEXAS ..."    -- TI BQ gauge ICs report this when sealed
    //   - "PTL"          -- Pacific Tech Logistics, major DJI clone OEM
    //                      (TEST_LOG note #28: 5/6 of our test packs are PTL)
    //   - "PACIFIC TECH" -- alt PTL string
    //   - empty          -- some sealed chips return nothing; assume DJI
    if (mfr.indexOf("DJI") >= 0 || mfr.indexOf("TEXAS") >= 0 ||
        mfr.indexOf("PTL") >= 0 || mfr.indexOf("PACIFIC TECH") >= 0 ||
        mfr.length() == 0) {
        return DEV_DJI_BATTERY;
    }
    return DEV_GENERIC_SBS;
}

// =====================================================================
// DJI Serial (register 0xD8 — custom DJI block register)
// =====================================================================
String DJIBattery::readDJISerial() {
    uint8_t buf[32] = {0};
    int len = SMBus::readBlock(BATT_ADDR, REG_DJI_SERIAL, buf, 31);
    if (len <= 0) return "";
    buf[len] = '\0';
    return String((char*)buf);
}

// =====================================================================
// DJI PF2 — custom Permanent Failure register at 0x4062 via MAC
// This is a DJI-specific extension not in standard TI BQ datasheet.
// Found in mavic-air-battery-helper (gvnt) and DJI Battery Killer.
// =====================================================================
uint32_t DJIBattery::readDJIPF2() {
    uint8_t buf[8] = {0};
    int n = SMBus::macBlockRead(BATT_ADDR, MAC_DJI_PF2_READ, buf, 8);
    if (n < 4) return 0;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

bool DJIBattery::clearDJIPF2() {
    // Write known-good value to 0x4062 via ManufacturerBlockAccess.
    // From mavic-air-battery-helper: write {0x01,0x23,0x45,0x67} to clear.
    uint8_t clearPayload[] = {
        (uint8_t)(MAC_DJI_PF2_READ & 0xFF), (uint8_t)(MAC_DJI_PF2_READ >> 8),
        0x01, 0x23, 0x45, 0x67
    };
    return SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS, clearPayload, 6);
}

// =====================================================================
// Cell count auto-detection
// =====================================================================
uint8_t DJIBattery::detectCellCount(const String &deviceName, const uint16_t cellV[4]) {
    // 1. Try parsing from DeviceName (DJI uses e.g. "WM160-2S1P", "WM220-3S1P")
    String dn = deviceName;
    int sIdx = dn.indexOf('S');
    if (sIdx > 0) {
        char ch = dn.charAt(sIdx - 1);
        if (ch >= '2' && ch <= '6') return ch - '0';
    }

    // 2. Known models
    dn.toUpperCase();
    if (dn.indexOf("WM160") >= 0 || dn.indexOf("WM161") >= 0) return 2;  // Mini 1/2: 2S
    if (dn.indexOf("WM100") >= 0) return 3;  // Spark: 3S
    if (dn.indexOf("WM220") >= 0 || dn.indexOf("WM230") >= 0) return 3;  // Mavic Pro/Air: 3S
    if (dn.indexOf("WM240") >= 0 || dn.indexOf("WM231") >= 0) return 4;  // Mavic 2/Air2: 4S
    if (dn.indexOf("WM260") >= 0 || dn.indexOf("WM460") >= 0) return 4;  // Mavic 3/4
    if (dn.indexOf("P330") >= 0) return 4;   // Phantom 3: 4S
    if (dn.indexOf("WM331") >= 0 || dn.indexOf("WM332") >= 0) return 4;  // Phantom 4

    // 3. Count non-zero cell voltages (generic)
    uint8_t count = 0;
    for (int i = 0; i < 4; i++) {
        if (cellV[i] > 100 && cellV[i] < 5000) count++;
    }
    return count ? count : 1;
}

// =====================================================================
// BatteryStatus decoder (standard SBS 0x16 bit meanings)
// =====================================================================
String DJIBattery::decodeBatteryStatus(uint16_t bs) {
    String r;
    if (bs & (1 << 15)) r += "OCA ";    // Over Charged Alarm
    if (bs & (1 << 14)) r += "TCA ";    // Terminate Charge Alarm
    if (bs & (1 << 12)) r += "OTA ";    // Over Temperature Alarm
    if (bs & (1 << 11)) r += "TDA ";    // Terminate Discharge Alarm
    if (bs & (1 << 9))  r += "RCA ";    // Remaining Capacity Alarm
    if (bs & (1 << 8))  r += "RTA ";    // Remaining Time Alarm
    // Status bits
    if (bs & (1 << 7))  r += "INIT ";
    if (bs & (1 << 6))  r += "DSG ";    // Discharging
    if (bs & (1 << 5))  r += "FC ";     // Fully Charged
    if (bs & (1 << 4))  r += "FD ";     // Fully Discharged
    if (!(bs & 0x000F) && r.length() == 0) r = "OK";
    return r;
}

// =====================================================================
// Full battery info read
// =====================================================================
BatteryInfo DJIBattery::readAll() {
    BatteryInfo info = {};
    info.connected = isConnected();
    if (!info.connected) { info.deviceType = DEV_NONE; return info; }

    info.deviceType = detectDeviceType();

    // ── Standard SBS (works on ANY SBS battery) ──
    info.voltage_mV         = SMBus::readWord(BATT_ADDR, REG_VOLTAGE);
    info.current_mA         = (int16_t)SMBus::readWord(BATT_ADDR, REG_CURRENT);
    info.avgCurrent_mA      = (int16_t)SMBus::readWord(BATT_ADDR, REG_AVG_CURRENT);
    info.stateOfCharge      = SMBus::readWord(BATT_ADDR, REG_REL_SOC);
    info.absoluteSOC        = SMBus::readWord(BATT_ADDR, REG_ABS_SOC);
    info.remainCapacity_mAh = SMBus::readWord(BATT_ADDR, REG_REMAIN_CAP);
    info.fullCapacity_mAh   = SMBus::readWord(BATT_ADDR, REG_FULL_CHARGE_CAP);
    info.designCapacity_mAh = SMBus::readWord(BATT_ADDR, REG_DESIGN_CAP);
    info.designVoltage_mV   = SMBus::readWord(BATT_ADDR, REG_DESIGN_VOLTAGE);
    info.cycleCount         = SMBus::readWord(BATT_ADDR, REG_CYCLE_COUNT);
    info.batteryStatus      = SMBus::readWord(BATT_ADDR, REG_BATTERY_STATUS);
    info.serialNumber       = SMBus::readWord(BATT_ADDR, REG_SERIAL_NUMBER);
    info.manufactureDate    = SMBus::readWord(BATT_ADDR, REG_MANUFACTURE_DATE);

    info.runTimeToEmpty_min = SMBus::readWord(BATT_ADDR, REG_RUN_TIME_EMPTY);
    info.avgTimeToEmpty_min = SMBus::readWord(BATT_ADDR, REG_AVG_TIME_EMPTY);
    info.timeToFull_min     = SMBus::readWord(BATT_ADDR, REG_TIME_TO_FULL);
    info.chargingCurrent_mA = SMBus::readWord(BATT_ADDR, REG_CHARGING_CURRENT);
    info.chargingVoltage_mV = SMBus::readWord(BATT_ADDR, REG_CHARGING_VOLTAGE);

    uint16_t tempRaw = SMBus::readWord(BATT_ADDR, REG_TEMPERATURE);
    if (tempRaw != 0xFFFF) info.temperature_C = tempRaw / 10.0f - 273.15f;

    info.cellVoltage[0] = SMBus::readWord(BATT_ADDR, REG_CELL1);
    info.cellVoltage[1] = SMBus::readWord(BATT_ADDR, REG_CELL2);
    info.cellVoltage[2] = SMBus::readWord(BATT_ADDR, REG_CELL3);
    info.cellVoltage[3] = SMBus::readWord(BATT_ADDR, REG_CELL4);

    info.manufacturerName = SMBus::readString(BATT_ADDR, REG_MANUFACTURER_NAME);
    info.deviceName       = SMBus::readString(BATT_ADDR, REG_DEVICE_NAME);
    info.chemistry        = SMBus::readString(BATT_ADDR, REG_DEVICE_CHEMISTRY);

    info.stateOfHealth = SMBus::readWord(BATT_ADDR, REG_STATE_OF_HEALTH);

    info.cellCount = detectCellCount(info.deviceName, info.cellVoltage);

    // ── Extended TI BQ registers — read for all batteries (fail gracefully) ──
    info.safetyAlert         = SMBus::readDword(BATT_ADDR, REG_SAFETY_ALERT);
    info.safetyStatus        = readSafetyStatus();
    info.pfAlert             = SMBus::readDword(BATT_ADDR, REG_PF_ALERT);
    info.pfStatus            = readPFStatus();
    info.operationStatus     = readOperationStatus();
    info.chargingStatus      = SMBus::readDword(BATT_ADDR, REG_CHARGING_STATUS);
    info.gaugingStatus       = SMBus::readDword(BATT_ADDR, REG_GAUGING_STATUS);
    info.manufacturingStatus = readManufacturingStatus();
    readDAStatus1(info);

    // ── DJI-specific (only if DJI battery detected) ──
    if (info.deviceType == DEV_DJI_BATTERY) {
        info.djiSerial = readDJISerial();
        info.djiPF2 = readDJIPF2();
    }

    // ── Chip/FW identification via MAC ──
    info.chipType = readChipType();
    info.firmwareVersion = readFirmwareVer();
    info.hardwareVersion = readHardwareVer();

    // ── Detection / state ──
    info.model = detectModel(info.deviceName, info.manufacturerName, info.chipType);
    info.supportedForService = !modelNeedsDjiKey(info.model);

    uint8_t sec = (info.operationStatus >> 8) & 0x03;
    info.sealed = (sec == 0x03);

    // Validity flags -- treat 0xFFFFFFFF / 0xFFFF as NACK sentinel (read failed)
    // rather than as a real value. CRITICAL safety: hasPF must use these,
    // otherwise a battery whose PFStatus read NACKed will be reported as
    // "no PF" when in reality we don't know its state.
    info.pfStatusKnown     = (info.pfStatus     != 0xFFFFFFFF);
    info.safetyStatusKnown = (info.safetyStatus != 0xFFFFFFFF);
    info.opStatusKnown     = (info.operationStatus != 0xFFFFFFFF);
    info.mfgStatusKnown    = (info.manufacturingStatus != 0xFFFFFFFF);
    info.djiPF2Known       = (info.djiPF2       != 0xFFFFFFFF);
    info.sohKnown          = (info.stateOfHealth != 0xFFFF && info.stateOfHealth != 0);
    info.chargingRecKnown  = (info.chargingVoltage_mV != 0 && info.chargingVoltage_mV != 0xFFFF);
    info.dasMacKnown       = (info.chipType != 0);

    // hasPF is "true if we KNOW there's a PF". If status reads NACKed, hasPF
    // stays false but pfStatusKnown=false signals to UI that we don't actually
    // know -- so UI can show "?" instead of "clean".
    info.hasPF = (info.pfStatusKnown && info.pfStatus != 0) ||
                 (info.djiPF2Known   && info.djiPF2   != 0);

    // Pack-type detection (run after all reads populated)
    info.cellsSynthesised  = detectSynthesisedCells(info);
    info.isLiHV            = detectLiHV(info);
    info.isCustomCapacity  = detectCustomCapacity(info);
    info.fwVariant         = detectFirmwareVariant(info);
    info.mfrDateDecoded    = decodeMfrDate(info.manufactureDate);
    info.fingerprint       = computeFingerprint(info);
    info.bmsLockoutDetected = isUnsealLockedOut();

    return info;
}

// Send one key word via ManufacturerBlockAccess (reg 0x44) — this is the
// method RUS_MAV uses for bq9003/bq40z307 packs. Frame on wire:
//   S addr 44 02 <lo> <hi> P
static bool sendKeyWordBlock(uint16_t word) {
    uint8_t bytes[2] = { (uint8_t)(word & 0xFF), (uint8_t)(word >> 8) };
    return SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS, bytes, 2);
}

// =====================================================================
// Unseal with specified key — tries both MA methods (word @ 0x00 for
// BQ30Z55-style packs, block @ 0x44 for bq9003/bq40z307-style packs).
// =====================================================================
UnsealResult DJIBattery::unsealWithKey(uint32_t key_combined) {
    uint16_t w1 = key_combined & 0xFFFF;
    uint16_t w2 = (key_combined >> 16) & 0xFFFF;

    // Method A: word writes to 0x00 (legacy / BQ30Z55)
    if (SMBus::writeWord(BATT_ADDR, REG_MFR_ACCESS, w1)) {
        delay(10);
        SMBus::writeWord(BATT_ADDR, REG_MFR_ACCESS, w2);
        delay(100);
        uint32_t op = readOperationStatus();
        if (((op >> 8) & 0x03) != 0x03) return UNSEAL_OK;
    }

    // Method B: block writes to 0x44 (bq9003 / bq40z307 / RUS_MAV style).
    // Per RUS_MAV disasm there's a 300 ms delay between the two sub-commands.
    if (sendKeyWordBlock(w1)) {
        delay(300);
        sendKeyWordBlock(w2);
        delay(100);
        uint32_t op = readOperationStatus();
        if (((op >> 8) & 0x03) != 0x03) return UNSEAL_OK;
    }

    // Still sealed after both attempts
    uint32_t op = readOperationStatus();
    if (op == 0 || op == 0xFFFFFFFF) return UNSEAL_NO_RESPONSE;
    return UNSEAL_REJECTED_SEALED;
}

// Try all keys for this model, then TI default
UnsealResult DJIBattery::unseal() {
    BatteryModel model = MODEL_UNKNOWN;
    String dn = SMBus::readString(BATT_ADDR, REG_DEVICE_NAME);
    model = detectModel(dn, "", 0);

    if (modelNeedsDjiKey(model)) {
        // For Mavic 3/4 and similar — we don't have keys, warn but try anyway
        Serial.printf("[Battery] Model %s requires DJI-specific key (not publicly available)\n",
                      modelName(model));
    }

    // Try all keys for this model, and model-agnostic keys
    for (int i = 0; i < UNSEAL_KEYS_COUNT; i++) {
        const UnsealKey &k = UNSEAL_KEYS[i];
        if (k.model != MODEL_UNKNOWN && k.model != model) continue;

        uint32_t combined = ((uint32_t)k.word2 << 16) | k.word1;
        Serial.printf("[Battery] Trying key %s: 0x%04X 0x%04X\n",
                      k.description, k.word1, k.word2);

        UnsealResult r = unsealWithKey(combined);
        if (r == UNSEAL_OK) {
            Serial.printf("[Battery] Unsealed with %s!\n", k.description);
            return UNSEAL_OK;
        }
        delay(100);
    }

    if (modelNeedsDjiKey(model)) return UNSEAL_UNSUPPORTED_MODEL;
    return UNSEAL_REJECTED_SEALED;
}

// =====================================================================
// HMAC-SHA1 challenge-response unseal (TI BQ40Z50/BQ40Z307 authenticated flow)
// =====================================================================
// Protocol per TI datasheet:
//   1. Request UNSEAL (MAC 0x0000) or AUTHENTICATE (some FWs use different code)
//   2. Device returns 20 bytes of random challenge via MAC block read 0x0000
//   3. Host computes HMAC-SHA1(key[0..31], challenge[0..19]) → 20B digest
//   4. Host writes digest as 20-byte block to MAC 0x0000
//   5. Device unseals if digest matches its own computation
//
// This is separate from the static 2x16-bit key unseal (unsealWithKey).
// DJI firmware may use either flow depending on chip/firmware version.
UnsealResult DJIBattery::unsealHmac(const uint8_t key[32], uint8_t challenge_out[20]) {
    if (!key) return UNSEAL_NO_RESPONSE;

    // Step 1: request challenge — write 0x0000 to MAC, then block-read to get random
    uint8_t challenge[20] = {0};
    int n = SMBus::macBlockRead(BATT_ADDR, 0x0000, challenge, sizeof(challenge));
    if (n < 20) {
        Serial.printf("[Battery] HMAC: failed to get challenge (len=%d)\n", n);
        return UNSEAL_NO_RESPONSE;
    }
    if (challenge_out) memcpy(challenge_out, challenge, 20);

    Serial.print("[Battery] HMAC challenge: ");
    for (int i = 0; i < 20; i++) Serial.printf("%02X", challenge[i]);
    Serial.println();

    // Step 2: compute HMAC-SHA1(key, challenge)
    uint8_t digest[20] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!info) {
        Serial.println("[Battery] HMAC: SHA1 info unavailable");
        return UNSEAL_NO_RESPONSE;
    }
    int rc = mbedtls_md_hmac(info, key, 32, challenge, 20, digest);
    if (rc != 0) {
        Serial.printf("[Battery] HMAC compute failed: %d\n", rc);
        return UNSEAL_NO_RESPONSE;
    }

    Serial.print("[Battery] HMAC digest: ");
    for (int i = 0; i < 20; i++) Serial.printf("%02X", digest[i]);
    Serial.println();

    // Step 3: write digest as 20-byte block to MAC 0x44 (ManufacturerBlockAccess)
    // The subcommand prefix (0x0000) goes first, then the 20-byte HMAC response.
    uint8_t payload[22] = {0x00, 0x00};
    memcpy(payload + 2, digest, 20);
    if (!SMBus::writeBlock(BATT_ADDR, 0x44, payload, 22)) {
        return UNSEAL_NO_RESPONSE;
    }

    delay(200);

    // Step 4: verify seal state
    uint32_t op = readOperationStatus();
    uint8_t sec = (op >> 8) & 0x03;
    if (sec != 0x03) {
        Serial.printf("[Battery] HMAC unseal OK (SEC=%d)\n", sec);
        return UNSEAL_OK;
    }
    return UNSEAL_REJECTED_SEALED;
}

bool DJIBattery::seal() {
    return SMBus::macCommand(BATT_ADDR, MAC_SEAL_DEVICE);
}

bool DJIBattery::softReset() {
    return SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET);
}

// =====================================================================
// Mavic-3 service operations (recovered 2026-05-01 from commercial
// battery-service tool firmware reverse-engineering).
// See research/dji_battery_tool/COMPARISON_VS_OUR_CODE.md.
// =====================================================================

// FAS (Full Access State) keys -- same byte order convention as the unseal
// keys: each half is sent as a separate MAC block-write to register 0x44.
static const uint16_t MAVIC3_FAS_KEY_LO = 0xBF17;
static const uint16_t MAVIC3_FAS_KEY_HI = 0xE0BC;

// Auth-bypass packet payload: MAC subcommand 0x4062 + 4-byte magic
// 0x67452301 (= first word of MD5 IV). Mavic-3 firmware checks for this
// magic before allowing destructive operations like cycle reset.
static const uint8_t DJI_AUTH_BYPASS_PAYLOAD[6] = {
    0x62, 0x40,             // subcommand 0x4062 (LE)
    0x01, 0x23, 0x45, 0x67  // magic = MD5 IV[0]
};

// LifetimeData reset payload: MAC subcommand 0x0060 + 4-byte zero arg.
static const uint8_t DJI_LIFETIME_RESET_PAYLOAD[6] = {
    0x60, 0x00,             // subcommand 0x0060 (LE)
    0x00, 0x00, 0x00, 0x00  // 4-byte zero
};

bool DJIBattery::enterFullAccess() {
    // Two block-writes to ManufacturerBlockAccess (reg 0x44), 100-300 ms
    // between, exactly mirroring the commercial tool's FAS flow.
    if (!sendKeyWordBlock(MAVIC3_FAS_KEY_LO)) return false;
    delay(300);
    if (!sendKeyWordBlock(MAVIC3_FAS_KEY_HI)) return false;
    delay(100);
    uint32_t op = readOperationStatus();
    uint8_t sec = (op >> 8) & 0x03;
    // SEC bits: 0x00 = FullAccess, 0x02 = Unsealed, 0x03 = Sealed
    return sec == 0x00;
}

bool DJIBattery::sendAuthBypass() {
    // Block write of 6 bytes to ManufacturerBlockAccess (reg 0x44).
    return SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS,
                             DJI_AUTH_BYPASS_PAYLOAD,
                             sizeof(DJI_AUTH_BYPASS_PAYLOAD));
}

bool DJIBattery::unlockForServiceOps() {
    // Step 1: try existing unseal flow (cycles through known key pairs)
    UnsealResult r = unseal();
    if (r != UNSEAL_OK) {
        Serial.printf("[Battery] unlock: unseal failed (%d)\n", r);
        return false;
    }
    delay(200);

    // Step 2: FAS
    if (!enterFullAccess()) {
        // Some packs (older Mavic Pro / Phantom) don't have a separate
        // FullAccess state -- Unsealed is enough. Verify by checking SEC.
        uint32_t op = readOperationStatus();
        uint8_t sec = (op >> 8) & 0x03;
        if (sec == 0x03) {
            Serial.println("[Battery] unlock: FAS rejected and pack still Sealed");
            return false;
        }
        // SEC = Unsealed -- treat as good enough for non-Mavic-3 packs.
        Serial.printf("[Battery] unlock: FAS rejected, but pack is Unsealed (SEC=%d)\n", sec);
    }
    delay(200);

    // Step 3: auth-bypass (silently best-effort; older packs may not need it)
    sendAuthBypass();
    delay(100);

    return true;
}

bool DJIBattery::clearBlackBox() {
    // ⚠ DEPRECATED / NO-OP since 2026-05-01 (TEST_LOG note #29).
    //
    // Earlier commit thought MAC 0x0030 was Black Box clear (based on its
    // appearance in the commercial-tool's PF clear ritual). Live test on
    // PTL 2024 batch (Battery #2) revealed MAC 0x0030 is actually SEAL --
    // sending it AT MID-OPERATION re-seals the pack and aborts the
    // remaining steps.
    //
    // BQ40Z80 has no dedicated Black Box clear MAC subcommand. The on-chip
    // event log gets cleared as part of MAC 0x0029 PFEnable (which we
    // already send in clearPFProper).
    //
    // This function now does nothing -- kept for API compatibility so
    // existing callers don't break. Will be removed in a future commit
    // once we confirm no clients rely on it.
    Serial.println("[Battery] clearBlackBox() is now a no-op -- BQ40Z80 has no dedicated BB-clear MAC");
    return true;
}

bool DJIBattery::resetLifetimeData() {
    // 6-byte block write: subcommand 0x0060 + 4-byte zero arg
    return SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS,
                             DJI_LIFETIME_RESET_PAYLOAD,
                             sizeof(DJI_LIFETIME_RESET_PAYLOAD));
}

// Full 14-step Mavic-3 PF clear ritual reverse-engineered from
// commercial-tool function 0x42002670. Sequence:
//   1-2.  Unseal (u1, u2)
//   3-4.  FAS (f1, f2)
//   5-8.  Unseal + FAS REPEATED (commercial tool does this for redundancy
//         on flaky links, since each MAC write can NACK if the bus glitches)
//   9.    PFEnable (MAC 0x0029) -- TI standard PF data reset
//   10.   Singleton 0x7F write (no register prefix, no length byte; purpose
//         undocumented but present in commercial tool's binary)
//   11.   Auth-bypass packet (MAC 0x4062 + magic 0x67452301)
//   12.   MAC readback (MAC 0x0000)
//   13.   Black Box clear (MAC 0x0030)
//   14.   LifetimeData reset (MAC 0x0060 + zero)
//
// Verifies PFStatus == 0 at the end. Returns true if clean.
bool DJIBattery::clearPFProper() {
    uint32_t pf_before = readPFStatus();
    uint32_t pf2_before = readDJIPF2();

    // === Phase A: Unseal (twice for redundancy, like commercial tool) ===
    for (int pass = 0; pass < 2; ++pass) {
        // Try the Mavic-3 keys directly; unseal() would also work but
        // takes longer because it iterates the key table.
        sendKeyWordBlock(0x7EE0); delay(500);
        sendKeyWordBlock(0xCCDF); delay(500);
        // FAS step
        sendKeyWordBlock(MAVIC3_FAS_KEY_LO); delay(500);
        sendKeyWordBlock(MAVIC3_FAS_KEY_HI); delay(500);
    }

    // Verify we're at SEC=00 (FullAccess) or at least SEC=02 (Unsealed)
    uint32_t op = readOperationStatus();
    uint8_t sec = (op >> 8) & 0x03;
    if (sec == 0x03) {
        Serial.println("[Battery] clearPF: still Sealed after 2x unseal+FAS, aborting");
        return false;
    }

    // === Phase B: PFEnable + ritual writes ===

    // 9. PFEnable / PF data reset
    SMBus::macCommand(BATT_ADDR, MAC_PF_DATA_RESET);  // 0x0029
    delay(50);

    // 10. Singleton 0x7F write -- raw byte at no register, purpose undocumented.
    //     Reproduces the commercial tool's behaviour byte-for-byte.
    {
        if (SMBus::busLock(200)) {
            Wire1.beginTransmission(BATT_ADDR);
            Wire1.write((uint8_t)0x7F);
            Wire1.endTransmission(true);
            SMBus::busUnlock();
        }
    }
    delay(50);

    // 11. Auth-bypass packet
    sendAuthBypass();
    delay(50);

    // 12. MA readback (writes MAC 0x0000)
    SMBus::macCommand(BATT_ADDR, 0x0000);
    delay(50);

    // 13. (REMOVED -- was "Black Box clear" via MAC 0x0030, but that's actually
    //     SEAL on PTL 2024 firmware. The on-chip event log is cleared by the
    //     PFEnable in step 9 above.)

    // 14. LifetimeData reset (MAC 0x0060 + 4-byte zero arg)
    resetLifetimeData();
    delay(500);

    // === Verify ===
    uint32_t pf_after  = readPFStatus();
    uint32_t pf2_after = readDJIPF2();

    Serial.printf("[Battery] clearPF: PF 0x%08X -> 0x%08X, PF2 0x%08X -> 0x%08X\n",
                  pf_before, pf_after, pf2_before, pf2_after);

    bool pfOk  = (pf_after  == 0 || pf_after  == 0xFFFFFFFF);
    bool pf2Ok = (pf2_after == 0 || pf2_after == 0xFFFFFFFF);

    // Soft reset to make the new state stick
    SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET);
    delay(1000);

    return pfOk && pf2Ok;
}

bool DJIBattery::resetCycles() {
    if (!unlockForServiceOps()) return false;

    // MAC 0x9013 = ResetLearnedData (BQ40Z80). Wipes the cycle counter +
    // chemical-capacity tracker. Commercial tool issues this after auth-bypass.
    SMBus::macCommand(BATT_ADDR, 0x9013);
    delay(500);

    // (Black Box clear was removed -- MAC 0x0030 is actually SEAL,
    //  not BB-clear, per TEST_LOG note #29.)

    // Soft reset for the BMS to re-read its parameter store
    SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET);
    delay(1000);

    uint16_t cycles = SMBus::readWord(BATT_ADDR, REG_CYCLE_COUNT);
    Serial.printf("[Battery] resetCycles: CycleCount = %u\n", cycles);
    return cycles == 0;
}

// =====================================================================
// DataFlash write (BQ40Z80 protocol)
// =====================================================================
// Standard TI DF write sequence:
//   1. write subclass to REG_DFCLASS (0x3E)
//   2. write block index (offset / 32) to REG_DFBLOCK (0x3F)
//   3. read 32-byte block at REG_BLOCKDATA (0x44) -- baseline for checksum
//   4. write the new bytes back to REG_BLOCKDATA
//   5. compute checksum: 0xFF - (subclass + block + sum_of_bytes) & 0xFF
//   6. write checksum to REG_BLOCKCHKSUM (0x60)
// Pack must be Unsealed + FullAccess for the write to land.
bool DJIBattery::writeDataFlashU16(uint8_t subclass, uint8_t offset, uint16_t value) {
    static const uint8_t REG_DFCLASS     = 0x3E;
    static const uint8_t REG_DFBLOCK     = 0x3F;
    static const uint8_t REG_BLOCKCHKSUM = 0x60;

    uint8_t block_idx  = offset / 32;
    uint8_t in_block   = offset % 32;

    // 1. Subclass select
    if (!SMBus::writeBlock(BATT_ADDR, REG_DFCLASS, &subclass, 1)) return false;
    delay(10);
    // 2. Block index select
    if (!SMBus::writeBlock(BATT_ADDR, REG_DFBLOCK, &block_idx, 1)) return false;
    delay(10);
    // 3. Read existing 32-byte block (baseline)
    uint8_t block[32] = {0};
    int n = SMBus::readBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS, block, 32);
    if (n != 32) return false;
    delay(10);
    // 4. Patch in the new value (LE)
    block[in_block]     = value & 0xFF;
    if (in_block + 1 < 32) block[in_block + 1] = value >> 8;
    if (!SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS, block, 32)) return false;
    delay(10);
    // 5. Compute checksum
    uint8_t sum = subclass + block_idx;
    for (int i = 0; i < 32; ++i) sum += block[i];
    uint8_t chksum = 0xFF - sum;
    // 6. Write checksum to commit
    if (!SMBus::writeBlock(BATT_ADDR, REG_BLOCKCHKSUM, &chksum, 1)) return false;
    delay(50);
    return true;
}

bool DJIBattery::writeCapacity(uint16_t new_mah) {
    // Commercial tool's UI exposes 5000-15000 mAh in 1000 steps; the BMS
    // itself accepts arbitrary values but Impedance Track may misbehave
    // outside that range. Enforce the same constraint to be safe.
    if (new_mah < 5000 || new_mah > 15000) {
        Serial.printf("[Battery] writeCapacity: %u out of range [5000..15000]\n", new_mah);
        return false;
    }

    if (!unlockForServiceOps()) return false;

    // BQ40Z80 GasGauging subclass = 0x52, layout (per BQ40Z80 datasheet):
    //   offset 0x06 = Design Capacity mAh (uint16)
    //   offset 0x08 = Design Capacity cWh (uint16)
    //   offset 0x0A..0x10 = per-cell Q Max (4 cells x 2 bytes)
    constexpr uint8_t SUBCLASS_GAS_GAUGING = 0x52;

    bool ok = true;
    ok &= writeDataFlashU16(SUBCLASS_GAS_GAUGING, 0x06, new_mah);

    // cWh = mAh * nominal_voltage_V / 100; Mavic 3 nominal pack V ~15.4
    uint16_t new_cwh = (uint32_t)new_mah * 154 / 1000;
    ok &= writeDataFlashU16(SUBCLASS_GAS_GAUGING, 0x08, new_cwh);

    // Per-cell Q Max -- each cell holds new_mah (we don't divide; the
    // per-cell field IS already the per-cell capacity in DJI 4S packs)
    for (uint8_t cell = 0; cell < 4; ++cell) {
        ok &= writeDataFlashU16(SUBCLASS_GAS_GAUGING, 0x0A + cell * 2, new_mah);
    }

    // Trigger LearnCycle so BMS re-reads DataFlash + recalibrates IT
    SMBus::macCommand(BATT_ADDR, 0x0021);
    delay(500);

    Serial.printf("[Battery] writeCapacity %u mAh: %s\n", new_mah, ok ? "OK" : "FAIL");
    return ok;
}

bool DJIBattery::startBalancing(uint8_t cellMask) {
    if (!unlockForServiceOps()) return false;
    // MAC subcommand 0x002A + 1-byte cellMask, sent as 3-byte block
    uint8_t payload[3] = { 0x2A, 0x00, cellMask };
    bool ok = SMBus::writeBlock(BATT_ADDR, REG_MFR_BLOCK_ACCESS, payload, 3);
    Serial.printf("[Battery] startBalancing(0x%02X): %s\n", cellMask, ok ? "OK" : "FAIL");
    return ok;
}

bool DJIBattery::startCalibration() {
    if (!unlockForServiceOps()) return false;
    // MAC 0x0021 LearnCycle -- the same MAC writeCapacity uses; here
    // it doubles as "begin Impedance Track learning". User must drive
    // the pack through a charge/discharge cycle for it to capture data.
    bool ok = SMBus::macCommand(BATT_ADDR, 0x0021);
    Serial.printf("[Battery] startCalibration: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// =====================================================================
// Patch.bin firmware flash via BQ ROM bootloader mode
// =====================================================================
// Best-effort reconstruction from commercial tool function 0x42008BB0.
// The Patch.bin container format isn't fully documented (vendor-custom
// header `03 FF 04 00 05 00 ...` was inferred from entropy zones, not
// from a complete parser walkthrough). Treat as experimental until
// validated against a real Mavic-3 pack with a logic analyser.
//
// ROM mode protocol:
//   - Entry: MAC 0x0F00 -> chip restarts at I2C addr 0x0B
//   - Commands at 0x0B (per TI sluua64):
//       0x35 + addr_lo + addr_hi -- mass erase request
//       0x40 + addr_lo + addr_hi + payload[N] + checksum -- write block
//       0x80 + 0x00 -- read status (returns 0x01 = OK)
//   - Exit: send 0x55 + 0x00 + 0x00 (chip restart)
bool DJIBattery::flashFirmwareFromBuffer(const uint8_t *patch_data, uint32_t patch_size,
                                         FlashProgressCb progress) {
    if (!patch_data || patch_size < 64) return false;
    if (!unlockForServiceOps()) return false;

    Serial.printf("[Battery] flashFirmware: starting, %u bytes\n", patch_size);

    // 1. Switch BQ40Z80 to ROM mode via MAC 0x0F00
    if (!SMBus::macCommand(BATT_ADDR, 0x0F00)) {
        Serial.println("[Battery] flashFirmware: MAC 0x0F00 NACKed, aborting");
        return false;
    }
    delay(2000);  // chip needs time to restart in ROM mode

    // 2. From here all I2C transactions go to 0x0B (the ROM bootloader address)
    static const uint8_t ROM_ADDR = 0x0B;

    // 3. Sync + mass erase
    if (SMBus::busLock(500)) {
        Wire1.beginTransmission(ROM_ADDR);
        Wire1.write(0x35); Wire1.write(0x00); Wire1.write(0x00);
        Wire1.endTransmission();
        SMBus::busUnlock();
    }
    delay(200);

    // 4. Stream the patch
    //
    // The container format (as best we can tell from the entropy zones):
    //   - bytes 0x0000..0x5000 (~20 KB) = DataFlash image
    //   - bytes 0x5000..end   (~64 KB) = Instruction Flash image
    // The actual record framing we DON'T know precisely. As a best-effort
    // first cut, treat the file as a flat byte stream and write 32-byte
    // chunks at sequential addresses. Real Mavic-3 hardware testing will
    // tell us if the framing is per-record (in which case this function
    // needs the parser added).
    constexpr uint32_t CHUNK = 32;
    uint32_t addr = 0;
    uint32_t pos  = 0;
    bool ok = true;

    while (pos < patch_size) {
        uint32_t n = (patch_size - pos > CHUNK) ? CHUNK : (patch_size - pos);

        if (!SMBus::busLock(500)) { ok = false; break; }
        Wire1.beginTransmission(ROM_ADDR);
        Wire1.write(0x40);                              // write-block command
        Wire1.write(addr & 0xFF);
        Wire1.write((addr >> 8) & 0xFF);
        for (uint32_t i = 0; i < n; ++i) Wire1.write(patch_data[pos + i]);
        // XOR checksum
        uint8_t cksum = 0;
        for (uint32_t i = 0; i < n; ++i) cksum ^= patch_data[pos + i];
        Wire1.write(cksum);
        if (Wire1.endTransmission() != 0) { ok = false; }
        SMBus::busUnlock();

        delay(5);

        if (!ok) break;
        pos  += n;
        addr += n;

        if (progress && (pos % 4096 < CHUNK)) {
            if (!progress(pos, patch_size)) { ok = false; break; }
        }
    }

    // 5. Exit ROM mode (restart command 0x55)
    if (SMBus::busLock(500)) {
        Wire1.beginTransmission(ROM_ADDR);
        Wire1.write(0x55); Wire1.write(0x00); Wire1.write(0x00);
        Wire1.endTransmission();
        SMBus::busUnlock();
    }
    delay(2000);

    Serial.printf("[Battery] flashFirmware: %s, %u/%u bytes written\n",
                  ok ? "OK" : "FAIL", pos, patch_size);
    return ok;
}

// =====================================================================
// Status decoders
// =====================================================================
String DJIBattery::decodeOperationStatus(uint32_t st) {
    String r;
    uint8_t sec = (st >> 8) & 0x03;
    r += "SEC:";
    r += (sec == 0x03) ? "Sealed" : (sec == 0x02) ? "Unsealed" : "FullAccess";
    if (st & (1 << 0)) r += " PRES";
    if (st & (1 << 2)) r += " DSG";
    if (st & (1 << 3)) r += " WAKE";
    if (st & (1 << 7)) r += " PF";
    if (st & (1 << 10)) r += " XDSG";
    if (st & (1 << 11)) r += " XCHG";
    return r;
}

String DJIBattery::decodePFStatus(uint32_t pf) {
    if (pf == 0 || pf == 0xFFFFFFFF) return "none";
    String r;
    const char* names[] = {
        "SUV","SOV","SOCC","SOCD","SOT","SOTF","QIM","CB",
        "IMP","CD","VIMR","VIMA","CHGC","CHGV","PTC","FUSE",
        "AFE_R","AFE_C","2LVL","IFC","DFETF","CFETF","ADC","DATA",
        "FUS","AFE_F","PTO","IRC","OPN","UTS","UTD","SBF"
    };
    for (int i = 0; i < 32; i++) {
        if (pf & (1UL << i)) { r += names[i]; r += " "; }
    }
    return r.length() ? r : "none";
}

String DJIBattery::decodeSafetyStatus(uint32_t ss) {
    if (ss == 0 || ss == 0xFFFFFFFF) return "none";
    String r;
    const char* names[] = {
        "CUV","COV","OCC1","OCC2","OCD1","OCD2","AOLD","AOLDL",
        "ASCC","ASCCL","ASCD","ASCDL","OTC","OTD","CUVC","UTC",
        "UTD","PRECHG_TO","CHG_TO","OC_CHG_L","OC_DSG_L","OVLD","CHGV","SERR",
        "PCHGC","PCHGV","ASCD2","ASCD3","ASCC2","ASCC3","UTS","COV2"
    };
    for (int i = 0; i < 32; i++) {
        if (ss & (1UL << i)) { r += names[i]; r += " "; }
    }
    return r.length() ? r : "none";
}

String DJIBattery::decodeManufacturingStatus(uint32_t ms) {
    String r;
    if (ms & (1 << 0)) r += "PCHG_T ";
    if (ms & (1 << 1)) r += "CHG_T ";
    if (ms & (1 << 2)) r += "DSG_T ";
    if (ms & (1 << 3)) r += "GAUGE_E ";
    if (ms & (1 << 4)) r += "FET_E ";
    if (ms & (1 << 5)) r += "LF_E ";
    if (ms & (1 << 6)) r += "PF_E ";
    if (ms & (1 << 7)) r += "BB ";
    if (ms & (1 << 8)) r += "CAL ";
    if (ms & (1 << 9)) r += "SLEEP ";
    if (ms & (1 << 10)) r += "SHTDN ";
    return r.length() ? r : "(default)";
}

// =====================================================================
// Pack identification helpers (added 2026-05-01)
// =====================================================================

String DJIBattery::decodeMfrDate(uint16_t raw) {
    if (!raw || raw == 0xFFFF) return "?";
    uint16_t year = ((raw >> 9) & 0x7F) + 1980;
    uint8_t  month = (raw >> 5) & 0x0F;
    uint8_t  day = raw & 0x1F;
    if (month < 1 || month > 12 || day < 1 || day > 31) return "?";
    char buf[12];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", year, month, day);
    return String(buf);
}

String DJIBattery::computeFingerprint(const BatteryInfo &info) {
    // 64-bit FNV-1a over (djiSerial + serial + mfrDate + fullCap + cycleCount).
    // Distinguishes PTL clones that share SBS serial number.
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
    };
    if (info.djiSerial.length() > 0) {
        mix(reinterpret_cast<const uint8_t*>(info.djiSerial.c_str()), info.djiSerial.length());
    }
    uint16_t s = info.serialNumber;       mix((uint8_t*)&s, 2);
    uint16_t d = info.manufactureDate;    mix((uint8_t*)&d, 2);
    uint16_t f = info.fullCapacity_mAh;   mix((uint8_t*)&f, 2);
    uint16_t c = info.cycleCount;         mix((uint8_t*)&c, 2);
    char buf[20];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)h);
    return String(buf);
}

bool DJIBattery::detectSynthesisedCells(const BatteryInfo &info) {
    // Newer PTL firmware (2024 batch) returns pack/N for every cell read while
    // sealed. Detect by: all cells equal to within 2 mV AND sum within 2 mV
    // of pack voltage. Real measurements always have some per-cell variance.
    if (info.cellCount < 2) return false;
    int n = info.cellCount > 4 ? 4 : info.cellCount;
    uint16_t mn = info.cellVoltage[0], mx = info.cellVoltage[0];
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) {
        uint16_t v = info.cellVoltage[i];
        if (v < 1000 || v > 5000) return false;  // out-of-range
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    if ((mx - mn) > 2) return false;  // any spread = real cells
    int32_t diff = (int32_t)sum - (int32_t)info.voltage_mV;
    if (diff < 0) diff = -diff;
    return diff < 4;  // sum matches pack within 4 mV -> synthesised
}

bool DJIBattery::detectLiHV(const BatteryInfo &info) {
    // LiHV cells charge to 4.35-4.40V vs standard Li-ion 4.20V.
    // chargingVoltage_mV / cellCount > 4300 -> LiHV
    if (!info.chargingRecKnown || info.cellCount == 0) return false;
    uint16_t per_cell = info.chargingVoltage_mV / info.cellCount;
    return per_cell > 4300;
}

bool DJIBattery::detectCustomCapacity(const BatteryInfo &info) {
    // Stock capacities for known DJI models (mAh):
    //   Mavic 3       5000
    //   Mavic Air 2   3500
    //   Mini 2/3      2250
    //   Spark         1480
    // If designCap > 1.5x the stock for the detected model, it's custom.
    if (info.designCapacity_mAh == 0 || info.designCapacity_mAh == 0xFFFF) return false;
    uint16_t stock = 0;
    switch (info.model) {
        case MODEL_MAVIC_3:  stock = 5000; break;
        case MODEL_MAVIC_4:  stock = 7388; break;  // Mavic 4 Pro stock
        case MODEL_MAVIC_AIR2:
        case MODEL_MAVIC_AIR2S: stock = 3500; break;
        case MODEL_MINI_2:
        case MODEL_MINI_3:   stock = 2250; break;
        case MODEL_SPARK:    stock = 1480; break;
        case MODEL_MAVIC_PRO:
        case MODEL_MAVIC_AIR:stock = 3830; break;
        default: return false;
    }
    return info.designCapacity_mAh > (stock * 3) / 2;
}

BatteryInfo::FirmwareVariant DJIBattery::detectFirmwareVariant(const BatteryInfo &info) {
    if (!info.connected || info.deviceType == DEV_NONE) return BatteryInfo::FW_VARIANT_UNKNOWN;
    // Generic SBS pack -> not a DJI variant
    if (info.deviceType == DEV_GENERIC_SBS) return BatteryInfo::FW_VARIANT_UNKNOWN;

    // PTL_NEW (2024 batch): synthesised cells AND ChipType MAC works
    if (info.cellsSynthesised && info.dasMacKnown) return BatteryInfo::FW_VARIANT_PTL_NEW;
    // PTL_OLD (2021/2025 batch): real cells AND ChipType NACK
    if (!info.cellsSynthesised && !info.dasMacKnown) return BatteryInfo::FW_VARIANT_PTL_OLD;
    // Mixed (genuine DJI?): real cells AND MAC works
    if (!info.cellsSynthesised && info.dasMacKnown)  return BatteryInfo::FW_VARIANT_GENUINE_DJI;
    return BatteryInfo::FW_VARIANT_UNKNOWN;
}

// =====================================================================
// Unseal lockout protection (added 2026-05-01 from Stage-1 testing)
// =====================================================================
// BQ40Z80 enters cooldown after 5 failed unseal attempts in 60 s. Hammering
// the BMS may also trigger a permanent block. We keep timestamps of the
// last 8 attempts and refuse further attempts at >4 within 60 s.

static constexpr int  UNSEAL_HISTORY = 8;
static uint32_t       s_unseal_attempts[UNSEAL_HISTORY] = {0};
static int            s_unseal_idx = 0;
static int            s_unseal_failures_in_window = 0;

bool DJIBattery::isUnsealLockedOut() {
    return unsealCooldownRemainingMs() > 0;
}

uint32_t DJIBattery::unsealCooldownRemainingMs() {
    uint32_t now = millis();
    int recent_failures = 0;
    uint32_t oldest_in_window = 0;
    for (int i = 0; i < UNSEAL_HISTORY; ++i) {
        uint32_t t = s_unseal_attempts[i];
        if (t == 0) continue;
        if ((now - t) < 60000) {
            recent_failures++;
            if (oldest_in_window == 0 || t < oldest_in_window) oldest_in_window = t;
        }
    }
    if (recent_failures >= 4) {
        // Cooldown for 35 s from oldest attempt in window
        uint32_t expire = oldest_in_window + 35000;
        if (now < expire) return expire - now;
    }
    return 0;
}

void DJIBattery::recordUnsealAttempt(bool ok) {
    if (!ok) {
        s_unseal_attempts[s_unseal_idx] = millis();
        s_unseal_idx = (s_unseal_idx + 1) % UNSEAL_HISTORY;
        s_unseal_failures_in_window++;
    } else {
        // Successful unseal -- clear the history
        clearUnsealHistory();
    }
}

void DJIBattery::clearUnsealHistory() {
    for (int i = 0; i < UNSEAL_HISTORY; ++i) s_unseal_attempts[i] = 0;
    s_unseal_idx = 0;
    s_unseal_failures_in_window = 0;
}

// =====================================================================
// Bulk key-trial (try a catalog of known unseal keys with rate-limit)
// =====================================================================
struct CatalogKey {
    uint16_t w1;
    uint16_t w2;
    const char *desc;
    // Bitmask of fwVariants this key is KNOWN to work on. 0 = unknown, try always.
    // Bit 0 = PTL_OLD (2021/2025), Bit 1 = PTL_NEW (2024), Bit 2 = GENUINE_DJI
    uint8_t known_variants;
};
static const CatalogKey UNSEAL_CATALOG[] = {
    // Order: most-likely-to-work first. known_variants flags from real-pack
    // testing (TEST_LOG / observation history).
    {0x7EE0, 0xCCDF, "RUS_MAV / commercial-tool Mavic 3", 0x02},  // confirmed PTL_NEW
    {0xCCDF, 0x7EE0, "RUS_MAV reversed",                  0x00},
    {0xBF17, 0xE0BC, "FAS keys (rare match for unseal)",  0x00},
    {0xE0BC, 0xBF17, "FAS reversed",                      0x00},
    {0x0414, 0x3672, "TI default (BQ30Z55 family)",       0x00},
    {0x3672, 0x0414, "TI default reversed",               0x00},
    {0x4DF6, 0x9F44, "DJI Battery Killer",                0x00},
    {0x9F44, 0x4DF6, "DJI BK reversed",                   0x00},
    // Add more as discovered (PTL_OLD keys still pending discovery)
};
static constexpr int UNSEAL_CATALOG_LEN = sizeof(UNSEAL_CATALOG) / sizeof(UNSEAL_CATALOG[0]);

static uint8_t variantBit(BatteryInfo::FirmwareVariant v) {
    switch (v) {
        case BatteryInfo::FW_VARIANT_PTL_OLD:     return 0x01;
        case BatteryInfo::FW_VARIANT_PTL_NEW:     return 0x02;
        case BatteryInfo::FW_VARIANT_GENUINE_DJI: return 0x04;
        default: return 0;
    }
}

DJIBattery::KeyTrialResult DJIBattery::tryAllKnownKeys() {
    KeyTrialResult res = {};
    res.attempts = 0;
    if (isUnsealLockedOut()) {
        res.lockedOut = true;
        return res;
    }

    // Detect the pack's firmware variant to prioritize matching keys first
    // (TEST_LOG note #30: PTL 2021 and 2024 use different keys -- avoid wasting
    // attempts on known-wrong keys).
    //
    // CRITICAL: do NOT call readAll() here -- it runs readDJIPF2() which writes
    // MAC subcommand 0x4062 (the DJI auth-bypass subcommand) with no data, and
    // PTL BMS interprets this as a MALFORMED auth-bypass packet and BLOCKS
    // the next unseal attempt (TEST_LOG note #33). We use a minimal variant
    // detection: just read ChipType MAC (which is harmless) + cell voltages
    // (also harmless).
    uint16_t chipType = readChipType();
    bool dasMacWorks = (chipType != 0);

    uint16_t cells[4] = {
        SMBus::readWord(BATT_ADDR, REG_CELL1),
        SMBus::readWord(BATT_ADDR, REG_CELL2),
        SMBus::readWord(BATT_ADDR, REG_CELL3),
        SMBus::readWord(BATT_ADDR, REG_CELL4),
    };
    uint16_t pack_v = SMBus::readWord(BATT_ADDR, REG_VOLTAGE);

    bool cellsSynth = false;
    if (cells[0] > 1000 && cells[0] < 5000) {
        uint16_t mn = cells[0], mx = cells[0]; uint32_t sum = 0;
        for (int i = 0; i < 4; ++i) {
            if (cells[i] < mn) mn = cells[i];
            if (cells[i] > mx) mx = cells[i];
            sum += cells[i];
        }
        int32_t diff = (int32_t)sum - (int32_t)pack_v;
        if (diff < 0) diff = -diff;
        cellsSynth = ((mx - mn) <= 2) && (diff < 4);
    }

    // Reproduce detectFirmwareVariant logic without triggering DJI-specific reads
    BatteryInfo::FirmwareVariant fwVar = BatteryInfo::FW_VARIANT_UNKNOWN;
    if (cellsSynth && dasMacWorks)        fwVar = BatteryInfo::FW_VARIANT_PTL_NEW;
    else if (!cellsSynth && !dasMacWorks) fwVar = BatteryInfo::FW_VARIANT_PTL_OLD;
    else if (!cellsSynth && dasMacWorks)  fwVar = BatteryInfo::FW_VARIANT_GENUINE_DJI;
    uint8_t myBit = variantBit(fwVar);

    // Two-pass try: first the keys flagged for this variant, then the rest.
    // Skip already-tried keys via a bitmap.
    bool tried[UNSEAL_CATALOG_LEN] = {0};

    auto trySlot = [&](int i) -> bool {
        if (tried[i]) return false;
        if (isUnsealLockedOut()) {
            res.lockedOut = true;
            return true;  // bail out of caller loops
        }
        const CatalogKey &k = UNSEAL_CATALOG[i];
        uint32_t combined = ((uint32_t)k.w2 << 16) | k.w1;
        UnsealResult r = unsealWithKey(combined);
        res.attempts++;
        tried[i] = true;
        recordUnsealAttempt(r == UNSEAL_OK);
        if (r == UNSEAL_OK) {
            res.ok = true;
            res.w1 = k.w1;
            res.w2 = k.w2;
            res.description = k.desc;
            return true;
        }
        delay(150);
        return false;
    };

    // Pass 1: ALL keys with any known-variant flag set (highest priority).
    // We try every variant-tagged key first, regardless of which variant we
    // currently think we're talking to -- variant detection is brittle (a
    // single transient ChipType NACK flips PTL_NEW -> PTL_OLD), so risking
    // a missed key trial because of variant misclassification is worse
    // than spending 1 extra attempt. Variant-matched keys go FIRST within
    // this pass.
    auto tryFlagged = [&](bool only_my_variant) -> bool {
        for (int i = 0; i < UNSEAL_CATALOG_LEN; ++i) {
            if (UNSEAL_CATALOG[i].known_variants == 0) continue;
            if (only_my_variant && myBit && !(UNSEAL_CATALOG[i].known_variants & myBit)) continue;
            if (trySlot(i)) return true;
        }
        return false;
    };

    // Pass 1a: variant-matched + flagged (may match nothing if myBit==0)
    if (myBit && tryFlagged(true)) return res;

    // Pass 1b: ALL flagged keys (catches missed variants from misclassification)
    if (tryFlagged(false)) return res;

    // Pass 2: untagged keys (we don't know which variant they fit -- try
    // anyway, may discover a new working key).
    for (int i = 0; i < UNSEAL_CATALOG_LEN; ++i) {
        if (UNSEAL_CATALOG[i].known_variants == 0) {
            if (trySlot(i)) return res;
        }
    }

    return res;  // ok=false, no key matched
}

// =====================================================================
// Force-acquire Port B as I2C
// =====================================================================
bool DJIBattery::forceAcquirePortB() {
    PortMode cur = PinPort::currentMode(PinPort::PORT_B);
    if (cur != PORT_IDLE && cur != PORT_I2C) {
        // Release whoever holds it
        PinPort::release(PinPort::PORT_B);
        delay(50);
    }
    if (!PinPort::acquire(PinPort::PORT_B, PORT_I2C, "battery-force")) return false;
    SMBus::init(PinPort::sda_pin(PinPort::PORT_B),
                PinPort::scl_pin(PinPort::PORT_B));
    return true;
}
