# WT32-SC01 Plus — upstream pin references

Pulled from production-grade upstream projects so we don't have to reverse
the pinout from a flash dump. Verify with a GPIO probe before trusting.

## Sources (in confidence order)

1. **openHASP** — [user_setups/esp32s3/wt32-sc01-plus.ini](https://github.com/HASwitchPlate/openHASP/blob/master/user_setups/esp32s3/wt32-sc01-plus.ini)
   Production firmware, runs on real boards. Highest confidence for LCD bus
   and touch.
2. **homeding** — [boards/esp32s3/sc01-plus.htm](https://homeding.github.io/boards/esp32s3/sc01-plus.htm)
   Concise reference, agrees with openHASP on common pins. Gives SD-SPI map.
3. **Wireless-Tag datasheet** — [WT32-SC01_Plus_ESP32-S3.pdf](https://github.com/fingernose/WT32-SC01/blob/master/datasheet/WT32-SC01_Plus_ESP32-S3.pdf)
4. **Other community refs** — [Cesarbautista10/WT32-SC01-Plus-ESP32](https://github.com/Cesarbautista10/WT32-SC01-Plus-ESP32), [fritsjan/WT32-SC01-PLUS-PLATFORMIO](https://github.com/fritsjan/WT32-SC01-PLUS-PLATFORMIO)

## Pin map (consensus)

### LCD — ST7796 320×480, 8-bit 8080 parallel

| Function    | GPIO  |
|-------------|-------|
| TFT_WR      | 47    |
| TFT_DC (RS) | 0     |
| TFT_CS      | -1    | (tied low — single-device bus)
| TFT_RST     | 4     |
| TFT_BL      | 45    |
| TFT_RD      | -1    | (not used; reads not supported in this wiring)
| TFT_D0      | 9     |
| TFT_D1      | 46    |
| TFT_D2      | 3     |
| TFT_D3      | 8     |
| TFT_D4      | 18    |
| TFT_D5      | 17    |
| TFT_D6      | 16    |
| TFT_D7      | 15    |

### Touch — FT6336 (older revisions) or GT911 (newer)

| Function     | GPIO  |
|--------------|-------|
| TOUCH_SDA    | 6     |
| TOUCH_SCL    | 5     |
| I2C port     | 1     |
| FT6336 addr  | 0x38  |
| GT911 addr   | 0x5D or 0x14 (depends on INT/RST strap) |
| Frequency    | 400 kHz |

**Detect at runtime:** scan I2C bus 1 for 0x38 → FT6336, else 0x5D/0x14 → GT911.
Don't assume from board revision; same SC01 Plus PCB ships with either chip
depending on production batch.

### SD card

The factory ESP-IDF app uses **SDMMC** (not SPI), per `sdmmc_*.c` strings in
the dump. homeding documents SPI mode pins:

| SPI mode    | GPIO  |
|-------------|-------|
| SD_SCK      | 39    |
| SD_MISO     | 38    |
| SD_MOSI     | 40    |
| SD_CS       | 41    |

For SDMMC mode (faster, supports 4-bit) on the same physical pins:

| SDMMC mode  | GPIO  | (mapping inferred — verify with probe) |
|-------------|-------|----------------------------------------|
| CLK         | 39    |
| CMD         | 40    |
| D0          | 38    |
| D3 (CS in SPI mode) | 41 | for 1-bit mode this can stay GPIO  |

ESP32-S3 SDMMC supports flexible GPIO assignment via GPIO matrix, so these
work. For 4-bit mode we'd need D1/D2 routed somewhere — likely not exposed
on this board, so **expect 1-bit mode** in practice (still ~6× faster than
SPI in our case).

### Misc

| Function    | GPIO  | Notes |
|-------------|-------|-------|
| BOOT button | 0     | shared with TFT_DC — pressing BOOT pulls DC low |
| USB D-      | 19    | native USB-Serial-JTAG |
| USB D+      | 20    | native USB-Serial-JTAG |
| RGB LED     | ?     | not standard on bare WT32-SC01 Plus |
| I2S BCLK    | 36    | for the onboard MAX98357 audio amp |
| I2S LRCK    | 35    | |
| I2S DOUT    | 37    | |

### Pin sharing concern

**TFT_DC = GPIO 0 = BOOT button.** Holding BOOT during normal operation
forces DC low, which corrupts LCD writes. Don't physically wire anything
to GPIO 0 beyond the existing button + LCD DC trace.

## PlatformIO env (draft — for Sprint 32, not yet committed)

```ini
[env:wt32_sc01_plus]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
upload_port = COM13
monitor_port = COM13
monitor_speed = 115200
upload_speed = 921600
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.psram_type = qspi          ; CRITICAL: NOT opi — this board's R2 is QSPI
board_build.partitions = default_16MB.csv
build_flags =
  -DBOARD_WT32_SC01_PLUS=1
  -DBOARD_HAS_PSRAM
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1
  -DCORE_DEBUG_LEVEL=3
  -DTFT_WIDTH=320
  -DTFT_HEIGHT=480
  ; LCD pins follow openHASP convention — see notes for full list
```
