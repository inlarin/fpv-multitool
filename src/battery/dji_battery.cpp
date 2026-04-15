#include "dji_battery.h"
#include "smbus.h"
#include "pin_config.h"

static const uint8_t BATT_ADDR = 0x0B;

// Standard SBS registers
static const uint8_t REG_TEMPERATURE        = 0x08;
static const uint8_t REG_VOLTAGE            = 0x09;
static const uint8_t REG_CURRENT            = 0x0A;
static const uint8_t REG_REL_SOC            = 0x0D;
static const uint8_t REG_REMAIN_CAP         = 0x0F;
static const uint8_t REG_FULL_CHARGE_CAP    = 0x10;
static const uint8_t REG_BATTERY_STATUS     = 0x16;
static const uint8_t REG_CYCLE_COUNT        = 0x17;
static const uint8_t REG_DESIGN_CAP         = 0x18;
static const uint8_t REG_MANUFACTURE_DATE   = 0x1B;
static const uint8_t REG_SERIAL_NUMBER      = 0x1C;
static const uint8_t REG_MANUFACTURER_NAME  = 0x20;
static const uint8_t REG_DEVICE_NAME        = 0x21;
static const uint8_t REG_DEVICE_CHEMISTRY   = 0x22;

static const uint8_t REG_CELL4              = 0x3C;
static const uint8_t REG_CELL3              = 0x3D;
static const uint8_t REG_CELL2              = 0x3E;
static const uint8_t REG_CELL1              = 0x3F;

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
void DJIBattery::init() {
    SMBus::init(I2C_SDA, I2C_SCL);
}

bool DJIBattery::isConnected() {
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
// Full battery info read (everything that doesn't require unseal)
// =====================================================================
BatteryInfo DJIBattery::readAll() {
    BatteryInfo info = {};
    info.connected = isConnected();
    if (!info.connected) return info;

    // Standard SBS
    info.voltage_mV       = SMBus::readWord(BATT_ADDR, REG_VOLTAGE);
    info.current_mA       = (int16_t)SMBus::readWord(BATT_ADDR, REG_CURRENT);
    info.stateOfCharge    = SMBus::readWord(BATT_ADDR, REG_REL_SOC);
    info.remainCapacity_mAh = SMBus::readWord(BATT_ADDR, REG_REMAIN_CAP);
    info.fullCapacity_mAh = SMBus::readWord(BATT_ADDR, REG_FULL_CHARGE_CAP);
    info.designCapacity_mAh = SMBus::readWord(BATT_ADDR, REG_DESIGN_CAP);
    info.cycleCount       = SMBus::readWord(BATT_ADDR, REG_CYCLE_COUNT);
    info.batteryStatus    = SMBus::readWord(BATT_ADDR, REG_BATTERY_STATUS);
    info.serialNumber     = SMBus::readWord(BATT_ADDR, REG_SERIAL_NUMBER);
    info.manufactureDate  = SMBus::readWord(BATT_ADDR, REG_MANUFACTURE_DATE);

    uint16_t tempRaw = SMBus::readWord(BATT_ADDR, REG_TEMPERATURE);
    if (tempRaw != 0xFFFF) info.temperature_C = tempRaw / 10.0f - 273.15f;

    info.cellVoltage[0] = SMBus::readWord(BATT_ADDR, REG_CELL1);
    info.cellVoltage[1] = SMBus::readWord(BATT_ADDR, REG_CELL2);
    info.cellVoltage[2] = SMBus::readWord(BATT_ADDR, REG_CELL3);
    info.cellVoltage[3] = SMBus::readWord(BATT_ADDR, REG_CELL4);

    info.manufacturerName = SMBus::readString(BATT_ADDR, REG_MANUFACTURER_NAME);
    info.deviceName       = SMBus::readString(BATT_ADDR, REG_DEVICE_NAME);
    info.chemistry        = SMBus::readString(BATT_ADDR, REG_DEVICE_CHEMISTRY);

    // Extended status
    info.safetyAlert         = SMBus::readDword(BATT_ADDR, REG_SAFETY_ALERT);
    info.safetyStatus        = readSafetyStatus();
    info.pfAlert             = SMBus::readDword(BATT_ADDR, REG_PF_ALERT);
    info.pfStatus            = readPFStatus();
    info.operationStatus     = readOperationStatus();
    info.chargingStatus      = SMBus::readDword(BATT_ADDR, REG_CHARGING_STATUS);
    info.gaugingStatus       = SMBus::readDword(BATT_ADDR, REG_GAUGING_STATUS);
    info.manufacturingStatus = readManufacturingStatus();

    // DAStatus1 (synchronized)
    readDAStatus1(info);

    // Chip/FW (may fail if sealed for some chips)
    info.chipType = readChipType();
    info.firmwareVersion = readFirmwareVer();
    info.hardwareVersion = readHardwareVer();

    // Detection
    info.model = detectModel(info.deviceName, info.manufacturerName, info.chipType);
    info.supportedForService = !modelNeedsDjiKey(info.model);

    // Decode seal state from OperationStatus SEC bits
    // SEC[1:0] in BQ40z307: bits 8-9 typically; 11 = sealed, 10 = unsealed, 01 = full access
    uint8_t sec = (info.operationStatus >> 8) & 0x03;
    info.sealed = (sec == 0x03);

    info.hasPF = (info.pfStatus != 0 && info.pfStatus != 0xFFFFFFFF);

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

bool DJIBattery::seal() {
    return SMBus::macCommand(BATT_ADDR, MAC_SEAL_DEVICE);
}

bool DJIBattery::softReset() {
    return SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET);
}

// Proper PF clear sequence
//   1. unseal (must be done separately first)
//   2. MAC 0x0029 PermanentFailDataReset
//   3. MAC 0x0054 ClearPF (DJI wrapper)
//   4. verify PFStatus == 0
//   5. MAC 0x0041 DeviceReset
bool DJIBattery::clearPFProper() {
    // Step 1: must already be unsealed — verify
    uint32_t op = readOperationStatus();
    uint8_t sec = (op >> 8) & 0x03;
    if (sec == 0x03) {
        Serial.println("[Battery] Cannot clear PF — battery is sealed. Unseal first.");
        return false;
    }

    // Step 2: MAC 0x29
    if (!SMBus::macCommand(BATT_ADDR, MAC_PF_DATA_RESET)) return false;
    delay(200);

    // Step 3: MAC 0x54
    if (!SMBus::macCommand(BATT_ADDR, MAC_CLEAR_PF)) return false;
    delay(500);

    // Step 4: verify
    uint32_t pf = readPFStatus();
    if (pf != 0 && pf != 0xFFFFFFFF) {
        Serial.printf("[Battery] PF clear incomplete: 0x%08X\n", pf);
        // still try reset
    }

    // Step 5: soft reset
    SMBus::macCommand(BATT_ADDR, MAC_DEVICE_RESET);
    delay(1000);

    return (pf == 0 || pf == 0xFFFFFFFF);
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
