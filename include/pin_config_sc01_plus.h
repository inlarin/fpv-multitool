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
