#pragma once
#include <Arduino.h>

// MAC sub-command catalog: known ManufacturerAccess commands for bq40z307/bq30z55
struct MacCommand {
    uint16_t    subcommand;
    const char *name;
    const char *desc;
    uint8_t     respLen;    // expected response length (0 = no response / write-only)
    bool        destructive;
};

// Unseal key pair
struct UnsealKey {
    uint16_t    w1;
    uint16_t    w2;
    const char *desc;
};

// DJI battery profile
struct BatteryProfile {
    const char       *name;       // e.g. "Mavic 3 (WM260)"
    const char       *chipName;   // e.g. "BQ9003 (BQ40Z307)"
    const UnsealKey  *unsealKeys;
    uint8_t           numUnsealKeys;
    const UnsealKey  *fasKeys;    // Full-Access keys
    uint8_t           numFasKeys;
};

// ============ MAC Command Catalog ============
// Common for bq40z307 / bq30z55 family (AN495, TI docs, DJI BK reverse)
static const MacCommand MAC_CATALOG[] PROGMEM = {
    // Read-only info (safe)
    { 0x0001, "DeviceType",        "Chip type ID (e.g. 0x4307)",           2, false },
    { 0x0002, "FirmwareVersion",   "BMS firmware version",                  6, false },
    { 0x0003, "HardwareVersion",   "BMS hardware revision",                 2, false },
    { 0x0006, "ChemicalID",        "Chemistry table ID",                    2, false },
    { 0x0008, "StaticChemDFSig",   "Static data-flash chem signature",      4, false },
    { 0x0009, "AllChemDFSig",      "All data-flash chem signature",         4, false },
    { 0x0070, "SafetyAlert",       "Current safety alert flags",            4, false },
    { 0x0071, "SafetyStatus",      "Latched safety status flags",           4, false },
    { 0x0072, "PFAlert",           "Pending permanent-failure alert",       4, false },
    { 0x0073, "PFStatus",          "Active permanent-failure flags",        4, false },
    { 0x0074, "OperationStatus",   "SEC bits, gauging, FETs",               4, false },
    { 0x0075, "ChargingStatus",    "Charger communication status",          4, false },
    { 0x0076, "GaugingStatus",     "Gas-gauge algorithm state",             4, false },
    { 0x0077, "ManufacturingStatus","MFG FET control, gauge enable",        4, false },

    // Seal / Unseal (REQUIRE unseal first for some)
    { 0x0020, "SealDevice",        "Transition to Sealed mode",             0, true  },
    { 0x0030, "SealDevice2",       "Seal (alternate code)",                 0, true  },

    // Service / maintenance (destructive!)
    { 0x0029, "PFDataReset",       "Clear PF flags in data flash (step 1)", 0, true  },
    { 0x002A, "PFDataReset2",      "Clear PF flags (step 2 / lifetime)",    0, true  },
    { 0x0041, "DeviceReset",       "Soft-reset BMS chip",                   0, true  },
    { 0x0054, "OperationStatusWr", "Write OperationStatus (clear PF bits)", 0, true  },

    // DJI specific from BK / RUS_MAV
    { 0x6A28, "DJI_CycleReset1",   "DJI cycle-count reset step 1",         0, true  },
    { 0x6A2A, "DJI_CycleReset2",   "DJI cycle-count reset step 2",         0, true  },
    { 0x0033, "BootROMEnter_BQ9003","Enter boot ROM (BQ9003)",             0, true  },
    { 0x0F00, "BootROMEnter_BQ30Z", "Enter boot ROM (BQ30Z55)",            0, true  },

    // Data Flash read/write via block access
    { 0x4000, "DFBlock_0x4000",    "DF read at address 0x4000",            32, false },
    { 0x4020, "DFBlock_0x4020",    "DF read at address 0x4020",            32, false },
    { 0x4040, "DFBlock_0x4040",    "DF read at address 0x4040",            32, false },
};
static const int MAC_CATALOG_LEN = sizeof(MAC_CATALOG) / sizeof(MAC_CATALOG[0]);

// ============ Unseal Keys ============
// From RUS_MAV.bin reverse, BK ini, DJI community (memory: reference_dji_battery_keys.md)
static const UnsealKey UNSEAL_KEYS_DJI_RU[] PROGMEM = {
    { 0x7EE0, 0xCCDF, "DJI RU Mavic 2/3" },
    { 0x0414, 0x3672, "TI default" },
    { 0xFFFF, 0xFFFF, "TI factory" },
};
static const UnsealKey FAS_KEYS_DJI_RU[] PROGMEM = {
    { 0xBF17, 0xE0BC, "DJI RU FAS" },
    { 0x0414, 0x3672, "TI default FAS" },
    { 0xFFFF, 0xFFFF, "TI factory FAS" },
};

static const UnsealKey UNSEAL_KEYS_GENERIC[] PROGMEM = {
    { 0x0414, 0x3672, "TI default" },
    { 0xFFFF, 0xFFFF, "TI factory" },
};
static const UnsealKey FAS_KEYS_GENERIC[] PROGMEM = {
    { 0x0414, 0x3672, "TI default FAS" },
    { 0xFFFF, 0xFFFF, "TI factory FAS" },
};

// ============ Battery Profiles ============
static const BatteryProfile BATTERY_PROFILES[] PROGMEM = {
    {
        "DJI Mavic 3 / Air 2S / Mini 3 (RU keys)",
        "BQ9003 (BQ40Z307)",
        UNSEAL_KEYS_DJI_RU, 3,
        FAS_KEYS_DJI_RU, 3
    },
    {
        "DJI Mavic 2 Pro/Zoom (RU keys)",
        "BQ30Z55",
        UNSEAL_KEYS_DJI_RU, 3,
        FAS_KEYS_DJI_RU, 3
    },
    {
        "Generic TI BQ40Z / BQ30Z",
        "BQ40Zxxx / BQ30Zxxx",
        UNSEAL_KEYS_GENERIC, 2,
        FAS_KEYS_GENERIC, 2
    },
};
static const int NUM_PROFILES = sizeof(BATTERY_PROFILES) / sizeof(BATTERY_PROFILES[0]);
