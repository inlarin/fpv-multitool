#pragma once

// =============================================================
// ESP32-S3-LCD-1.47B Pin Configuration (confirmed working)
// Board: Waveshare ESP32-S3-LCD-1.47B / diymore clone
// =============================================================

// --- LCD (SPI) ---
#define LCD_MOSI    45
#define LCD_SCLK    40
#define LCD_CS      42
#define LCD_DC      41
#define LCD_RST     39
#define LCD_BL      46

#define LCD_WIDTH   172
#define LCD_HEIGHT  320
#define LCD_COL_OFFSET 34

// --- I2C Wire0: QMI8658 IMU (onboard, GPIO 48/47 — NOT on pin headers) ---
#define I2C_SDA     48
#define I2C_SCL     47

// --- I2C Wire1: DJI Battery SMBus (on pin headers, internal pullup) ---
// Free GPIOs exposed on Waveshare right-side pin header (silkscreen GP10 + GP11)
#define BATT_SDA    11
#define BATT_SCL    10

// --- BOOT button (navigation) ---
#define BTN_BOOT    0

// --- RGB LED (WS2812) ---
#define RGB_LED_PIN 38
#define RGB_LED_NUM 1

// --- SD Card (SDMMC) ---
#define SD_CMD      15
#define SD_CLK      14
#define SD_D0       16
#define SD_D1       18
#define SD_D2       17
#define SD_D3       21

// --- Battery ADC ---
#define BAT_ADC_PIN 1   // ADC1_CH0, voltage divider

// --- Servo / Motor output (shared pin) ---
#define SIGNAL_OUT  2

// --- UART for ELRS flashing ---
#define ELRS_TX     43
#define ELRS_RX     44
#define ELRS_BOOT   3
