# ESP32-S3 FPV Servo/Motor Tester

## Project Goal
Build an application on ESP32-S3-LCD-1.47B for testing FPV drone servos and motors.

## Board: Waveshare ESP32-S3-LCD-1.47B (diymore clone)
- **MCU:** ESP32-S3R8, dual-core LX7 @ 240MHz
- **Memory:** 512KB SRAM, 384KB ROM, 8MB PSRAM, 16MB Flash
- **Display:** ST7789, 172x320 IPS, SPI interface, col_offset=34
- **IMU:** QMI8658 6-axis (accel+gyro), I2C (addr 0x6B)
- **Other:** RGB LED (WS2812), Micro-SD slot (SDMMC), LiPo charging, USB-C

## CONFIRMED Pin Configuration

### LCD (SPI) — CONFIRMED WORKING
| Function | GPIO |
|----------|-------|
| LCD_MOSI | 45 |
| LCD_SCLK | 40 |
| LCD_CS | 42 |
| LCD_DC | 41 |
| LCD_RST | 39 |
| LCD_BL | 46 |

### SD Card (SDMMC) — from GPIO probe (driven high)
| Function | GPIO |
|----------|-------|
| SD_CMD | 15 |
| SD_CLK | 14 |
| SD_D0 | 16 |
| SD_D1 | 18 |
| SD_D2 | 17 |
| SD_D3 | 21 |

### I2C Wire0 — onboard QMI8658 IMU (не выведен на pin headers)
| Function | GPIO |
|----------|-------|
| I2C_SDA | 48 |
| I2C_SCL | 47 |

### I2C Wire1 — DJI Battery SMBus (на pin headers: GP10/GP11)
| Function | GPIO |
|----------|-------|
| BATT_SDA | 11 |
| BATT_SCL | 10 |

### UART1 — ELRS/CRSF/RC sniffer (на pin headers)
| Function | GPIO |
|----------|-------|
| ELRS_TX (ESP → приёмник) | 43 |
| ELRS_RX (приёмник → ESP) | 44 |
| ELRS_BOOT (DFU)          | 3 |

### Signal output — Servo/Motor DShot/ESC (на pin headers)
| Function | GPIO |
|----------|-------|
| SIGNAL_OUT | 2 |

### RGB LED
| Function | GPIO |
|----------|-------|
| RGB_LED | 38 (WS2812, onboard) |

### Battery voltage ADC
| Function | GPIO |
|----------|-------|
| BAT_ADC_PIN | 1 (ADC1_CH0, voltage divider) |

### BOOT button (навигация)
| Function | GPIO |
|----------|-------|
| BTN_BOOT | 0 |

**Источник истины для pins:** [include/pin_config.h](include/pin_config.h)

## Build System
- **Framework:** PlatformIO + Arduino (espressif32, Arduino Core 3.x)
- **Board def:** `esp32-s3-devkitc-1`
- **Display lib:** `GFX Library for Arduino` (Arduino_GFX)
- **Build:** `pio run` / **Upload:** `pio run -t upload` / **Monitor:** `pio device monitor`
- **USB:** Native USB CDC (`ARDUINO_USB_CDC_ON_BOOT=1`)

## Display Init
```cpp
Arduino_DataBus *bus = new Arduino_ESP32SPI(41 /* DC */, 42 /* CS */, 40 /* SCK */, 45 /* MOSI */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 39 /* RST */, 0 /* rotation */, false /* IPS */,
    172, 320, 34 /* col_offset */, 0, 34, 0);
```

## Key Sources
- Waveshare ESP32-S3-LCD-1.47B wiki: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B
- Demo code: https://files.waveshare.com/wiki/ESP32-S3-LCD-1.47B/ESP32-S3-LCD-1.47B-Demo.zip
- Spotpear wiki (same pinout): https://spotpear.com/wiki/ESP32-S3R8-1.47inch-LCD-Screen-172x320-SD-LVGL-USB-Display.html

## Code Style
- C++ Arduino framework
- User communicates in Russian
