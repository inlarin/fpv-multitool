#pragma once
#include <Arduino.h>

// Data Flash map for TI BQ9003/BQ40z307 (DJI Smart Batteries)
// Parsed from DJI Battery Killer Killer.ini — only entries with known addresses.
// Values read/written via ManufacturerBlockAccess (reg 0x44).

enum DFType : uint8_t {
    DF_I1 = 0,  // int8_t
    DF_I2 = 1,  // int16_t
    DF_U1 = 2,  // uint8_t
    DF_U2 = 3,  // uint16_t
    DF_U4 = 4,  // uint32_t
    DF_H1 = 5,  // hex uint8_t (display as 0xNN)
};

static inline uint8_t dfTypeSize(DFType t) {
    switch (t) {
        case DF_I1: case DF_U1: case DF_H1: return 1;
        case DF_I2: case DF_U2: return 2;
        case DF_U4: return 4;
    }
    return 2;
}

struct DFEntry {
    uint16_t    addr;
    DFType      type;
    int32_t     minVal;
    int32_t     maxVal;
    int32_t     defVal;
    const char *category;    // "Protections", "Gas Gauging", etc.
    const char *subcat;      // "CUV", "COV", etc.
    const char *field;       // "Threshold", "Delay", etc.
    const char *unit;        // "mV", "mA", "s", etc.
};

// =====================================================================
// PROGMEM Data Flash map — BQ9003 / BQ40z307
// =====================================================================

static const DFEntry DF_MAP[] PROGMEM = {
    // ── Protections ──
    {0x4764, DF_I2, 0, 32767, 2500, "Protections", "CUV", "Threshold", "mV"},
    {0x4766, DF_U1, 0, 255, 2, "Protections", "CUV", "Delay", "s"},
    {0x4767, DF_I2, 0, 32767, 3000, "Protections", "CUV", "Recovery", "mV"},
    {0x4769, DF_I2, 0, 32767, 4300, "Protections", "COV", "Thresh Low Temp", "mV"},
    {0x476B, DF_I2, 0, 32767, 4300, "Protections", "COV", "Thresh Std Temp", "mV"},
    {0x476D, DF_I2, 0, 32767, 4300, "Protections", "COV", "Thresh High Temp", "mV"},
    {0x476F, DF_I2, 0, 32767, 4300, "Protections", "COV", "Thresh Rec Temp", "mV"},
    {0x4771, DF_U1, 0, 255, 2, "Protections", "COV", "Delay", "s"},
    {0x4772, DF_I2, 0, 32767, 3900, "Protections", "COV", "Recov Low Temp", "mV"},
    {0x4774, DF_I2, 0, 32767, 3900, "Protections", "COV", "Recov Std Temp", "mV"},
    {0x4776, DF_I2, 0, 32767, 3900, "Protections", "COV", "Recov High Temp", "mV"},
    {0x4778, DF_I2, 0, 32767, 3900, "Protections", "COV", "Recov Rec Temp", "mV"},
    {0x477A, DF_I2, -32768, 32767, 8000, "Protections", "OCC2", "Threshold", "mA"},
    {0x477C, DF_U1, 0, 255, 3, "Protections", "OCC2", "Delay", "s"},
    {0x47A6, DF_I2, -32768, 32767, 2000, "Protections", "PTO", "Chg Threshold", "mA"},
    {0x47A8, DF_I2, -32768, 32767, 1800, "Protections", "PTO", "Susp Threshold", "mA"},
    {0x47AA, DF_U2, 0, 65535, 1800, "Protections", "PTO", "Delay", "s"},
    {0x47AC, DF_I2, 0, 32767, 2, "Protections", "PTO", "Reset", "mAh"},
    {0x47AE, DF_I2, -32768, 32767, 2500, "Protections", "CTO", "Chg Threshold", "mA"},
    {0x47B0, DF_I2, -32768, 32767, 2000, "Protections", "CTO", "Susp Threshold", "mA"},
    {0x47B2, DF_U2, 0, 65535, 54000, "Protections", "CTO", "Delay", "s"},
    {0x47B4, DF_I2, 0, 32767, 2, "Protections", "CTO", "Reset", "mAh"},
    {0x47B6, DF_I2, -32768, 32767, 300, "Protections", "OC", "Threshold", "mAh"},
    {0x47B8, DF_I2, -32768, 32767, 2, "Protections", "OC", "Recovery", "mAh"},
    {0x47BA, DF_U1, 0, 100, 90, "Protections", "OC", "RSOC Recovery", "%"},

    // ── Permanent Fail ──
    {0x47BF, DF_I2, 0, 32767, 2200, "Permanent Fail", "SUV", "Threshold", "mV"},
    {0x47C1, DF_U1, 0, 255, 5, "Permanent Fail", "SUV", "Delay", "s"},
    {0x47C2, DF_I2, 0, 32767, 4500, "Permanent Fail", "SOV", "Threshold", "mV"},
    {0x47C4, DF_U1, 0, 255, 5, "Permanent Fail", "SOV", "Delay", "s"},
    {0x47C5, DF_I2, -32768, 32767, 10000, "Permanent Fail", "SOCC", "Threshold", "mA"},
    {0x47C7, DF_U1, 0, 255, 5, "Permanent Fail", "SOCC", "Delay", "s"},
    {0x47C8, DF_I2, -32768, 32767, -10000, "Permanent Fail", "SOCD", "Threshold", "mA"},
    {0x47CA, DF_U1, 0, 255, 5, "Permanent Fail", "SOCD", "Delay", "s"},
    {0x47CB, DF_I2, -400, 1500, 650, "Permanent Fail", "SOT", "Threshold", "0.1C"},
    {0x47CD, DF_U1, 0, 255, 5, "Permanent Fail", "SOT", "Delay", "s"},
    {0x47CE, DF_I2, 0, 5000, 3500, "Permanent Fail", "VIMR", "Check Voltage", "mV"},
    {0x47D0, DF_I2, 0, 32767, 10, "Permanent Fail", "VIMR", "Check Current", "mA"},
    {0x47D2, DF_I2, 0, 5000, 500, "Permanent Fail", "VIMR", "Delta Threshold", "mV"},
    {0x47D4, DF_U1, 0, 255, 5, "Permanent Fail", "VIMR", "Delta Delay", "s"},
    {0x47D5, DF_U2, 0, 65535, 100, "Permanent Fail", "VIMR", "Duration", "s"},
    {0x47D7, DF_I2, 0, 5000, 3700, "Permanent Fail", "VIMA", "Check Voltage", "mV"},
    {0x47D9, DF_I2, 0, 32767, 50, "Permanent Fail", "VIMA", "Check Current", "mA"},
    {0x47DB, DF_I2, 0, 5000, 200, "Permanent Fail", "VIMA", "Delta Threshold", "mV"},
    {0x47DD, DF_U1, 0, 255, 2, "Permanent Fail", "VIMA", "Delay", "s"},
    {0x47EA, DF_I2, 0, 32767, 5000, "Permanent Fail", "OPNCELL", "Threshold", "mV"},
    {0x47EC, DF_U1, 0, 255, 5, "Permanent Fail", "OPNCELL", "Delay", "s"},

    // ── Adv. Charge Algorithm ──
    {0x47ED, DF_I1, -128, 127, 0, "Charge Algo", "Temp Ranges", "T1 Temp", "C"},
    {0x47EE, DF_I1, -128, 127, 12, "Charge Algo", "Temp Ranges", "T2 Temp", "C"},
    {0x47EF, DF_I1, -128, 127, 20, "Charge Algo", "Temp Ranges", "T3 Temp", "C"},
    {0x47F0, DF_I1, -128, 127, 25, "Charge Algo", "Temp Ranges", "T4 Temp", "C"},
    {0x47F1, DF_I1, -128, 127, 30, "Charge Algo", "Temp Ranges", "T5 Temp", "C"},
    {0x47F2, DF_I1, -128, 127, 55, "Charge Algo", "Temp Ranges", "T6 Temp", "C"},
    {0x47F3, DF_I1, -128, 127, 55, "Charge Algo", "Temp Ranges", "T7 Temp", "C"},
    {0x47F4, DF_I1, 0, 127, 1, "Charge Algo", "Temp Ranges", "Hysteresis", "C"},
    {0x47F5, DF_I2, 0, 32767, 4000, "Charge Algo", "Low Temp Chg", "Voltage", "mV"},
    {0x47F7, DF_I2, 0, 32767, 132, "Charge Algo", "Low Temp Chg", "Current Low", "mA"},
    {0x47F9, DF_I2, 0, 32767, 352, "Charge Algo", "Low Temp Chg", "Current Med", "mA"},
    {0x47FB, DF_I2, 0, 32767, 264, "Charge Algo", "Low Temp Chg", "Current High", "mA"},
    {0x47FD, DF_I2, 0, 32767, 4200, "Charge Algo", "Std Temp Chg", "Voltage", "mV"},
    {0x47FF, DF_I2, 0, 32767, 1980, "Charge Algo", "Std Temp Chg", "Current Low", "mA"},
    {0x4801, DF_I2, 0, 32767, 4004, "Charge Algo", "Std Temp Chg", "Current Med", "mA"},
    {0x4803, DF_I2, 0, 32767, 2992, "Charge Algo", "Std Temp Chg", "Current High", "mA"},
    {0x4805, DF_I2, 0, 32767, 4000, "Charge Algo", "High Temp Chg", "Voltage", "mV"},
    {0x4807, DF_I2, 0, 32767, 1012, "Charge Algo", "High Temp Chg", "Current Low", "mA"},
    {0x4809, DF_I2, 0, 32767, 1980, "Charge Algo", "High Temp Chg", "Current Med", "mA"},
    {0x480B, DF_I2, 0, 32767, 1496, "Charge Algo", "High Temp Chg", "Current High", "mA"},
    {0x480D, DF_I2, 0, 32767, 4100, "Charge Algo", "Rec Temp Chg", "Voltage", "mV"},
    {0x480F, DF_I2, 0, 32767, 2508, "Charge Algo", "Rec Temp Chg", "Current Low", "mA"},
    {0x4811, DF_I2, 0, 32767, 4488, "Charge Algo", "Rec Temp Chg", "Current Med", "mA"},
    {0x4813, DF_I2, 0, 32767, 3520, "Charge Algo", "Rec Temp Chg", "Current High", "mA"},
    {0x4815, DF_I2, 0, 32767, 88, "Charge Algo", "Pre-Charging", "Current", "mA"},
    {0x4817, DF_I2, 0, 32767, 44, "Charge Algo", "Maintenance", "Current", "mA"},
    {0x4819, DF_I2, 0, 32767, 2500, "Charge Algo", "Voltage Range", "Precharge Start", "mV"},
    {0x481B, DF_I2, 0, 32767, 2900, "Charge Algo", "Voltage Range", "Chg V Low", "mV"},
    {0x481D, DF_I2, 0, 32767, 3600, "Charge Algo", "Voltage Range", "Chg V Med", "mV"},
    {0x481F, DF_I2, 0, 32767, 4000, "Charge Algo", "Voltage Range", "Chg V High", "mV"},
    {0x4821, DF_U1, 0, 255, 0, "Charge Algo", "Voltage Range", "Chg Hysteresis", "mV"},
    {0x4822, DF_U1, 0, 100, 50, "Charge Algo", "SoC Range", "Chg SoC Med", "%"},
    {0x4823, DF_U1, 0, 100, 75, "Charge Algo", "SoC Range", "Chg SoC High", "%"},
    {0x4824, DF_U1, 0, 100, 1, "Charge Algo", "SoC Range", "Chg SoC Hyst", "%"},
    {0x4825, DF_I2, 0, 32767, 250, "Charge Algo", "Termination", "Taper Current", "mA"},
    {0x4827, DF_I2, 0, 32767, 75, "Charge Algo", "Termination", "Charge Term V", "mV"},
    {0x4842, DF_U2, 0, 65535, 367, "Charge Algo", "Cell Balance", "Time/mAh cell0", "s/mAh"},
    {0x4844, DF_U2, 0, 65535, 514, "Charge Algo", "Cell Balance", "Time/mAh cell1-3", "s/mAh"},
    {0x4846, DF_U1, 0, 255, 3, "Charge Algo", "Cell Balance", "Min Start Delta", "mV"},
    {0x4847, DF_U4, 0, 2147483647, 18000, "Charge Algo", "Cell Balance", "Relax Interval", "s"},
    {0x484B, DF_U1, 0, 100, 80, "Charge Algo", "Cell Balance", "Min RSOC Bal", "%"},
    {0x484C, DF_U1, 0, 100, 80, "Charge Algo", "Cell Balance", "Min RSOC Sleep", "%"},

    // ── Gas Gauging ──
    {0x4724, DF_I2, -32768, 32767, 100, "Gas Gauging", "Current Thresh", "Dsg Current", "mA"},
    {0x4725, DF_I2, -32768, 32767, 50, "Gas Gauging", "Current Thresh", "Chg Current", "mA"},
    {0x4726, DF_I2, 0, 32767, 4400, "Gas Gauging", "Design", "Capacity mAh", "mAh"},
    {0x4728, DF_I2, 0, 32767, 6336, "Gas Gauging", "Design", "Capacity cWh", "cWh"},
    {0x472A, DF_I2, 0, 32767, 14400, "Gas Gauging", "Design", "Voltage", "mV"},
    {0x472C, DF_U1, 0, 100, 90, "Gas Gauging", "Cycle", "Count Pct", "%"},
    {0x4731, DF_I2, 0, 5000, 3000, "Gas Gauging", "FD", "Set V Thresh", "mV"},
    {0x4733, DF_I2, 0, 5000, 3100, "Gas Gauging", "FD", "Clear V Thresh", "mV"},
    {0x4735, DF_U1, 0, 100, 0, "Gas Gauging", "FD", "Set % RSOC", "%"},
    {0x4736, DF_U1, 0, 100, 5, "Gas Gauging", "FD", "Clear % RSOC", "%"},
    {0x4737, DF_I2, 0, 5000, 4200, "Gas Gauging", "FC", "Set V Thresh", "mV"},
    {0x4739, DF_I2, 0, 5000, 4100, "Gas Gauging", "FC", "Clear V Thresh", "mV"},
    {0x473B, DF_U1, 0, 100, 100, "Gas Gauging", "FC", "Set % RSOC", "%"},
    {0x473C, DF_U1, 0, 100, 95, "Gas Gauging", "FC", "Clear % RSOC", "%"},
    {0x473D, DF_I2, 0, 5000, 3200, "Gas Gauging", "TD", "Set V Thresh", "mV"},
    {0x473F, DF_I2, 0, 5000, 3300, "Gas Gauging", "TD", "Clear V Thresh", "mV"},
    {0x4741, DF_U1, 0, 100, 6, "Gas Gauging", "TD", "Set % RSOC", "%"},
    {0x4742, DF_U1, 0, 100, 8, "Gas Gauging", "TD", "Clear % RSOC", "%"},
    {0x4743, DF_I2, 0, 5000, 4200, "Gas Gauging", "TC", "Set V Thresh", "mV"},
    {0x4745, DF_I2, 0, 5000, 4100, "Gas Gauging", "TC", "Clear V Thresh", "mV"},
    {0x4000, DF_U1, 0, 100, 100, "Gas Gauging", "TC", "Set % RSOC", "%"},
    {0x4748, DF_U1, 0, 100, 95, "Gas Gauging", "TC", "Clear % RSOC", "%"},
    {0x4340, DF_U2, 0, 65535, 0, "Gas Gauging", "State", "Cycle Count", "-"},
};

static const int DF_MAP_LEN = sizeof(DF_MAP) / sizeof(DF_MAP[0]);
