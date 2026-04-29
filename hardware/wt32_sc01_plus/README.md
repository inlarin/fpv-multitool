# Wireless-Tag WT32-SC01 Plus

Second hardware target for the ESP32-S3 ELRS flasher project. The plan is to
build a **standalone touch-screen catalog flasher** here while the existing
Waveshare 1.47B keeps its role as a browser-driven sandbox.

## Board profile

| Item            | Value                                                |
|-----------------|------------------------------------------------------|
| Module          | ESP32-S3-WROOM-1-N16R2                               |
| MCU             | ESP32-S3 QFN56 rev v0.2, dual-core LX7 @ 240 MHz     |
| Flash           | 16 MB, **quad SPI (QIO)**                            |
| PSRAM           | 2 MB embedded, **quad SPI** (AP_3v3) — NOT octal     |
| Display         | 3.5" ST7796 320×480, 8080 8-bit parallel             |
| Touch           | GT911 capacitive, I2C                                |
| SD              | SPI slot, FAT32 (we'll mount a 64 GB card)           |
| Audio           | I2S out + MAX98357 amp                               |
| USB             | Native USB-Serial-JTAG (no CH340/CP2102 bridge)      |
| BOOT/EN buttons | GPIO 0 (BOOT), hardware EN                           |
| MAC             | 88:56:A6:80:EB:48 (this specific board)              |

USB enumeration on host: `VID:PID=303A:1001`, COM13. esptool drives DTR/RTS
over CDC for auto-reset — no physical BOOT press required to upload.

## Factory partition layout

Single-app, no OTA, no filesystem partition. Only 4 MB of the 16 MB flash is
allocated; 12 MB is free for our use (catalog mirror staging, log buffer,
self-OTA slot, etc).

| Name      | Type | SubType | Offset    | Size     |
|-----------|------|---------|-----------|----------|
| nvs       | data | nvs     | 0x009000  | 24 KB    |
| phy_init  | data | phy     | 0x00f000  | 4 KB     |
| factory   | app  | factory | 0x010000  | 4 MB     |

See [notes/partition_table.csv](notes/partition_table.csv) for the raw CSV
suitable for `gen_esp32part.py`.

## Files in this directory

```
factory_dump/
  full_16mb.bin           16 MB   — complete flash backup, restore w/ write_flash 0x0
  partition_table.bin      4 KB   — raw partition table from offset 0x8000
  bootloader.bin          32 KB   — extracted 0x0..0x8000
  nvs.bin                 24 KB   — extracted 0x9000..0xf000 (100% 0xFF, empty)
  phy_init.bin             4 KB   — extracted 0xf000..0x10000
  factory_app.bin          4 MB   — extracted 0x10000..0x410000 (real app ~1.94 MB)
  free_space.bin          12 MB   — extracted 0x410000..0x1000000 (100% 0xFF)
notes/
  chip_info.txt                   — esptool flash_id + USB enumeration details
  partition_table.csv             — gen_esp32part.py-compatible CSV
  factory_app_analysis.txt        — string scan, build origin, subsystems
```

## Key findings from the dump

- Stock app is **ESP-IDF, not Arduino**, ~1.94 MB inside a 4 MB slot
- Build origin string: `/home/sorz/code_repository/esp32-8ms-v3/` — this is
  the **official Wireless-Tag demo project**. Hunt down its GitHub repo before
  reverse-engineering pin map; it's the canonical reference
- LCD driver uses **custom 8080 parallel bus** (`8080_lcd_esp32s3.c`)
- SD is wired to the **SDMMC controller**, not SPI — use SDMMC mode for our
  code (4-bit, faster)
- **NVS is empty** and the free 12 MB is virgin 0xFF — we can erase the entire
  flash without losing any factory calibration data

## Restore procedure (if we brick the board)

```sh
# Full restore from the factory backup we just took:
pio pkg exec --package tool-esptoolpy -- esptool.py --port COM13 \
  --baud 921600 write_flash 0x0 hardware/wt32_sc01_plus/factory_dump/full_16mb.bin
```

The dump is taken as a single contiguous 16 MB read so a `write_flash 0x0`
restores byte-for-byte (including bootloader, partition table, NVS phy_init,
and the factory app). MAC eFuse is never touched by `write_flash` — it stays
at 88:56:A6:80:EB:48.

## What's next

This directory is the staging area for Sprint 32 (hardware bring-up). After
the dump completes we'll:
1. Split out per-partition .bin files for inspection
2. Reverse the factory app a bit — find what demo/library Wireless-Tag ships,
   harvest LCD init sequence + touch I2C address if useful
3. Probe GPIO map empirically to confirm the classic SC01 Plus pinout
4. Add `[env:wt32_sc01_plus]` to platformio.ini and produce the first
   blink+Serial test build
