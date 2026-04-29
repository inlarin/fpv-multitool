#pragma once

// Wireless-Tag WT32-SC01 Plus pin map (ESP32-S3-WROOM-1-N16R2).
//
// Source of truth: hardware/wt32_sc01_plus/notes/upstream_pin_references.md,
// cross-verified against the openHASP user_setups/esp32s3/wt32-sc01-plus.ini
// production config. Not yet probed empirically -- Sprint 32 sanity sketch
// will I2C-scan and LCD-init to validate.

// ============================================================================
// LCD -- ST7796 320x480, 8-bit 8080 parallel
// ============================================================================
#define SC01P_LCD_WIDTH      320
#define SC01P_LCD_HEIGHT     480

#define SC01P_LCD_WR         47
#define SC01P_LCD_DC          0   // shared with BOOT button -- never wire anything else here
#define SC01P_LCD_CS         -1   // tied low (single device on the bus)
#define SC01P_LCD_RST         4   // shared with touch RST
#define SC01P_LCD_BL         45
#define SC01P_LCD_RD         -1   // not wired

#define SC01P_LCD_D0          9
#define SC01P_LCD_D1         46
#define SC01P_LCD_D2          3
#define SC01P_LCD_D3          8
#define SC01P_LCD_D4         18
#define SC01P_LCD_D5         17
#define SC01P_LCD_D6         16
#define SC01P_LCD_D7         15

// ============================================================================
// Touch -- FT6336 (older revision) OR GT911 (newer revision), I2C bus
// ============================================================================
// Both controllers live on the same physical I2C; addresses differ:
//   FT6336 -> 0x38
//   GT911  -> 0x5D or 0x14 (depends on INT/RST strap at boot)
// Sanity sketch detects which one by I2C scan at boot.
#define SC01P_TOUCH_SDA       6
#define SC01P_TOUCH_SCL       5
#define SC01P_TOUCH_RST       4   // shared with LCD RST
#define SC01P_TOUCH_PORT      1   // Wire1
#define SC01P_TOUCH_FREQ_HZ   400000
#define SC01P_TOUCH_ADDR_FT6336  0x38
#define SC01P_TOUCH_ADDR_GT911A  0x5D
#define SC01P_TOUCH_ADDR_GT911B  0x14

// ============================================================================
// SD card -- factory app uses SDMMC controller, not SPI
// ============================================================================
// 1-bit SDMMC mode on these GPIOs (4-bit not exposed on this board).
// SPI fallback uses the same physical lines.
#define SC01P_SD_CLK         39
#define SC01P_SD_CMD         40   // = MOSI in SPI mode
#define SC01P_SD_D0          38   // = MISO in SPI mode
#define SC01P_SD_D3          41   // = CS in SPI mode (1-bit mode keeps it high)

// ============================================================================
// Misc
// ============================================================================
#define SC01P_BTN_BOOT        0   // shared with LCD_DC -- "press" only when DC is idle high
#define SC01P_USB_DM         19
#define SC01P_USB_DP         20

// I2S audio amp (MAX98357 onboard) -- not used in current scope
#define SC01P_I2S_BCLK       36
#define SC01P_I2S_LRCK       35
#define SC01P_I2S_DOUT       37

// ============================================================================
// API-compatible aliases (mirror of pin_config.h on Waveshare)
// ============================================================================
// Subsystem code (battery/, motor/, servo/, crsf/, rc_sniffer/) references
// the unprefixed names below. We define them here as SC01-Plus-specific
// values so that code compiles cleanly on either board with no #ifdef.

// --- I2C Wire0 (onboard FT6336 touch on SC01 Plus, replaces QMI8658 IMU) ---
// Waveshare's I2C_SDA/SCL was for the onboard IMU; on SC01 Plus the touch
// chip lives on Wire0 instead. Subsystem code that scans Wire0 will find
// the FT6336 instead of an IMU -- harmless, but worth knowing.
#define I2C_SDA              SC01P_TOUCH_SDA
#define I2C_SCL              SC01P_TOUCH_SCL

// --- Port B (EXT header pins 3 & 4): same convention as Waveshare ---
// EXT IO interface from the WT32-SC01 Plus datasheet:
//   pin 3 -> EXT_IO1 = GPIO 10  (Port B pin B)
//   pin 4 -> EXT_IO2 = GPIO 11  (Port B pin A)
// One physical 4-wire cable to the device-under-test (red +5V, black GND,
// plus two data lines on EXT pins 3/4). PinPort mode-switches between
// I2C / UART / PWM / GPIO depending on which protocol the user picked.
#define PORT_B_PIN_A         11   // EXT pin 4
#define PORT_B_PIN_B         10   // EXT pin 3

// --- Legacy aliases (mapped to Port B, identical to Waveshare) ---
#define BATT_SDA             PORT_B_PIN_A
#define BATT_SCL             PORT_B_PIN_B
#define SIGNAL_OUT           PORT_B_PIN_A   // Servo/Motor/DShot signal
#define ELRS_TX              PORT_B_PIN_A   // ESP TX -> receiver RX
#define ELRS_RX              PORT_B_PIN_B   // ESP RX <- receiver TX
#define ELRS_BOOT            -1             // CRSF protocol DFU instead of GPIO

// --- BOOT button (shared with TFT_DC on this board, do NOT use as nav) ---
// GPIO 0 = TFT data/command line. Pressing it during LCD writes corrupts
// the display. Touchscreen replaces button navigation here, so we surface
// BTN_BOOT as -1 to disable any code that polls it.
#define BTN_BOOT             -1

// --- RGB LED: not populated on SC01 Plus ---
// Status feedback comes from the LCD instead.
#define RGB_LED_PIN          -1
#define RGB_LED_NUM          0

// --- SD Card: 1-bit SDMMC (D1/D2 not exposed on this board) ---
#define SD_CMD               SC01P_SD_CMD   // 40
#define SD_CLK               SC01P_SD_CLK   // 39
#define SD_D0                SC01P_SD_D0    // 38
#define SD_D3                SC01P_SD_D3    // 41
// SD_D1 / SD_D2 deliberately undefined -- 1-bit mode only on this board.

// --- Battery ADC: SC01 Plus has no battery / no voltage-divider ---
#define BAT_ADC_PIN          -1

// --- Free EXT pins reserved for future expansion ---
// (GPIO 12/13/14 free on EXT pins 5/6/7; GPIO 21 on EXT pin 8 reserved.)
#define SC01P_EXT_FREE_1     12   // EXT pin 5
#define SC01P_EXT_FREE_2     13   // EXT pin 6
#define SC01P_EXT_FREE_3     14   // EXT pin 7
// EXT pin 8 (GPIO 21) is intentionally not defined here -- reserved.
