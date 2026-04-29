#pragma once

// =============================================================
// Pin Configuration -- chooses board variant via build flags.
//
// BOARD_WT32_SC01_PLUS=1   -> include pin_config_sc01_plus.h
// (default, no flag)        -> Waveshare ESP32-S3-LCD-1.47B definitions below
//
// Both headers expose the same API surface (PORT_B_PIN_A/B + BATT_SDA,
// SIGNAL_OUT, ELRS_TX/RX, ELRS_BOOT, RGB_LED_PIN, BAT_ADC_PIN, etc.).
// Subsystem code only ever includes this header and gets the right
// values per build env -- no #ifdef sprinkled across the codebase.
// =============================================================

#if defined(BOARD_WT32_SC01_PLUS)

#include "pin_config_sc01_plus.h"

#else

// =============================================================
// ESP32-S3-LCD-1.47B (Waveshare / diymore clone) -- the original board.
//
// All user-accessible features are wired through Port B (GPIO 10/11)
// + power (5V, GND). See PinPort for dynamic mode switching.
// =============================================================

// --- LCD (SPI, onboard) ---
#define LCD_MOSI    45
#define LCD_SCLK    40
#define LCD_CS      42
#define LCD_DC      41
#define LCD_RST     39
#define LCD_BL      46

#define LCD_WIDTH   172
#define LCD_HEIGHT  320
#define LCD_COL_OFFSET 34

// --- I2C Wire0: QMI8658 IMU (onboard, NOT on pin headers) ---
#define I2C_SDA     48
#define I2C_SCL     47

// --- Port B (pin headers GP10/GP11): universal signal bus ---
// Dynamic mode selection via PinPort:
//   PORT_I2C   — SDA=pin_a (11), SCL=pin_b (10)  — DJI battery SMBus, CP2112 emu
//   PORT_UART  — TX=pin_a  (11), RX=pin_b  (10)  — ELRS/CRSF/USB2TTL
//   PORT_PWM   — signal=pin_a (11)               — Servo, DShot motor, ESC one-wire
//   PORT_GPIO  — pin_a (11) free, pin_b (10) free — bit-bang, sniffer PPM/SBUS
#define PORT_B_PIN_A  11
#define PORT_B_PIN_B  10

// --- Legacy pin aliases (mapped to Port B) ---
// Old code continues to work without changes. New code should use PinPort.
#define BATT_SDA    PORT_B_PIN_A    // was 11
#define BATT_SCL    PORT_B_PIN_B    // was 10
#define SIGNAL_OUT  PORT_B_PIN_A    // was 2 — Servo/Motor/ESC on Port B pin_a
#define ELRS_TX     PORT_B_PIN_A    // was 43 — ESP TX (→ receiver RX)
#define ELRS_RX     PORT_B_PIN_B    // was 44 — ESP RX (← receiver TX)
#define ELRS_BOOT   -1              // was 3  — no longer used; receiver DFU via its own button

// --- BOOT button (navigation, onboard) ---
#define BTN_BOOT    0

// --- RGB LED (WS2812, onboard) ---
#define RGB_LED_PIN 38
#define RGB_LED_NUM 1

// --- SD Card (SDMMC, onboard) ---
#define SD_CMD      15
#define SD_CLK      14
#define SD_D0       16
#define SD_D1       18
#define SD_D2       17
#define SD_D3       21

// --- Battery ADC (onboard) ---
#define BAT_ADC_PIN 1   // ADC1_CH0, voltage divider

// --- Reserved / not used ---
// GPIO 2, 3, 43, 44 — legacy fields, currently unused (kept for future expansion)

#endif  // BOARD_WT32_SC01_PLUS
