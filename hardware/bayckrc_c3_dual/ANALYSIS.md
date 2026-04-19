# Bayck RC C3 Dual — dump analysis

**Device:** Bayck RC C3 Dual Band 100 mW Gemini RX
**MCU:** ESP32-C3 (riscv 32-bit)
**Radio:** Semtech **LR1121** (dual-band: 2.4 GHz + sub-GHz 900 MHz)
**Flash size:** 4 MB (SPI)
**Dump:** `dump_2026-04-19_1528.bin`, md5 `55f56ed5a17bf4ed1a2b85814e812a6b`
**Method:** ESP32-S3 FPV MultiTool → Port B UART → ROM bootloader READ_FLASH_SLOW (0x0E), 4 194 304 bytes in ~9.5 min

## Partition map

| Name     | Type/Sub | Offset     | Size      |
|----------|----------|------------|-----------|
| nvs      | 01/02    | 0x0009000  | 0x05000 (20 KB)  |
| otadata  | 01/00    | 0x000e000  | 0x02000 (8 KB)   |
| app0     | 00/10    | 0x0010000  | 0x1E0000 (≈1.88 MB) |
| app1     | 00/11    | 0x01F0000  | 0x1E0000 (≈1.88 MB) |
| spiffs   | 01/82    | 0x03D0000  | 0x20000 (128 KB) |
| coredump | 01/03    | 0x03F0000  | 0x10000 (64 KB)  |

## Dual-firmware layout

**This RX dual-boots two different ELRS variants** (OTA slots):

### app0 — **vanilla ExpressLRS 3.5.3**
- Identifier strings (from rodata):
  - `ExpressLRS RX` • `elrs_rx`
  - `UNIFIED_ESP32C3_LR1121_RX` (vanilla ELRS target name)
  - git hash `40555e` • version `3.5.3`
  - product name: `BAYCKRC C3 900/2400 Dual Band 100mW Gemini RX`
- Default AP config: SSID `ExpressLRS RX`, password `expresslrs`, IP `10.0.0.1`
- Web UI paths include `/lr1121.html` (spectrum analyzer), `/cw.html` (continuous wave), `/scan.js`

### app1 — **MILELRS v3.48** (proprietary fork)
- Identifier strings:
  - `MILELRS_v348` • git `8bced9`
  - Receiver advertises CRSF DEVICE_INFO name: `BK DB 100 GRX`
  - Build by user `danko` (path leaked via `HardwareSerial.cpp` error string)
  - Built with `framework-arduinoespressif32@3.20011.230801` (Aug 2023)
- **Unusual config fields** (not in vanilla ELRS):
  - `ew_scanner` / `ew_rssi` — electronic-warfare scanner
  - `multi_band` / `custom_freq` / `custom_freq2` — arbitrary frequency programming
  - `swarm_id` / `swarm_vx` — swarm coordination
  - `encryption_key` / `binding_rate` — link encryption
  - `vrx_control` / `vx_bands` — VTX control
  - `fast_switch` / `detector` / `power_key` / `relay` / `lifetime` / `brand`
  - ESP-IDF v4.4.5 (older than app0's)

### Active slot

From `otadata` at 0x0e000–0x0f020:
- Sector 0 (@0xe000): `seq=1`, crc `0x4743989a`
- Sector 1 (@0xf000): `seq=2`, crc `0x55f63774` ← higher, wins

Active partition = `(max_seq − 1) mod 2` = `(2 − 1) mod 2` = **1 → app1 (MILELRS)**.

To flip to app0 (vanilla ELRS) **non-destructively**: erase the `otadata`
partition. When otadata is blank, ESP-IDF bootloader defaults to `app0`.
This is implemented by POST `/api/flash/erase_region?offset=0xe000&size=0x2000`
from the multitool, then power-cycling the RX.

## Hardware pinout (extracted from app0's embedded JSON)

```json
{
  "serial_rx": 20, "serial_tx": 21,
  "radio_miso": 5, "radio_mosi": 4, "radio_sck": 6,
  "radio_busy": 3, "radio_dio1": 1,
  "radio_nss": 0, "radio_rst": 2,
  "radio_busy_2": 8, "radio_dio1_2": 18,
  "radio_nss_2": 7, "radio_rst_2": 10,
  "power_min": 0, "power_high": 3,
  "power_max": 3, "power_default": 0,
  "power_control": 0,
  "power_values": [12, 16, 19, 22],
  "power_values_dual": [-12, -9, -6, -2],
  "led_rgb": 19, "led_rgb_isgrb": true,
  "radio_dcdc": true,
  "button": 9,
  "radio_rfsw_ctrl": [31, 0, 4, 8, 8, 18, 0, 17]
}
```

- Dual SX128x/LR1121 radio config (`_2` suffix = second band)
- LED on GPIO 19 (GRB order)
- BOOT button on GPIO 9 (hold during power-on to enter ROM bootloader)
- RF switch on 8 pins via `radio_rfsw_ctrl` table

## NVS contents

NVS partition (20 KB) parsed as ESP-IDF v2 format. Most entries are in
namespace `eeprom` (ELRS config blobs) and numbered `ns#40`, `ns#102`, etc.
No WiFi STA credentials found — receiver never connected to a home network.

## Security observations

The MILELRS fork contains fields suggesting a security-oriented design
(encryption_key, custom_freq, ew_scanner). Any UID / binding phrase would
live in the `eeprom` namespace blobs — currently not decoded by parse.py
(needs an ELRS-eeprom-specific parser).

## Reflash attempt log

### Attempt 1 — OTADATA erase (2026-04-19) — **FAILED to flip**

Theory: wipe the 8 KB `otadata` partition (0x0e000–0x10000). ESP-IDF
bootloader should fall back to `app0` when otadata is blank, which would
activate the vanilla ExpressLRS 3.5.3 copy already present.

Execution:
1. Put RX in DFU (hold BOOT during power-up).
2. `POST /api/flash/erase_region?offset=0xe000&size=0x2000` → succeeded:
   `Erased 0x2000 bytes @ 0xe000`.
3. Power-cycled RX (no BOOT hold).
4. Started CRSF service on plate, pinged RX — device info returned
   `name="BK DB 100 GRX"` (same as before erase — inconclusive, since
   both apps share the same `lua_name`/`product_name` from
   hardware.json).
5. Waited 65 s for RX to enable its auto-WiFi AP (`wifi-on-interval: 60`
   from hardware.json).
6. Scanned WiFi from the plate:
   **SSID = "MILELRS v3.48 RX 1016"** — MILELRS still active.

Conclusion: **MILELRS fork has a custom bootloader that ignores / overrides
the standard ESP-IDF otadata fallback.** Erasing otadata was insufficient.
Possible mechanisms:
- Custom 2nd-stage bootloader that hard-codes app1 preference
- Modified partition table loader that flips ota_0 ↔ ota_1 subtypes
- Anti-downgrade / anti-revert protection baked into bootloader

### Next session — attempt 2 plan

**Path A — full reflash of app partition with vanilla ELRS 3.x:**
1. Download the latest ExpressLRS 3.x `UNIFIED_ESP32C3_LR1121_RX`
   firmware.bin from https://github.com/ExpressLRS/ExpressLRS/releases
   (current latest as of April 2026 is likely v3.5.x — check releases).
2. Put RX in DFU.
3. `POST /api/flash/upload` with the firmware.bin, then
   `POST /api/flash/start` with `offset=0x10000` (overwrites app0).
   (Our `/api/flash/upload` currently sends with offset=0 by default;
   may need to extend `executeFlash` / `ESPFlasher::Config.flash_offset`.)
4. Erase otadata again so app0 is selected.
5. Power-cycle — this time vanilla ELRS 3.x should boot because app0
   bytes are different and a custom bootloader can't map "app1's UID"
   onto them.

Risk: if the MILELRS bootloader itself is at offset 0x0000 (replaces
standard ESP-IDF bootloader), it may hard-block vanilla ELRS booting.
In that case need to erase + reflash **bootloader too** (offset 0x0000,
0x1000 bytes) with a stock ESP32-C3 bootloader — binary available in
ESP-IDF releases.

**Path B — reflash everything back to MILELRS backup then start over:**
We have the full 4 MB dump. Writing the whole image back via chunked
FLASH_BEGIN/DATA from offset 0 restores the original state. Useful as
a "known-good" safety net if Path A bricks the RX.

**Path C — glitch / electrical reset approach:**
Outside scope of this multitool; requires ChipWhisperer / dedicated
tooling. Not planned.

### Hardware target identification for vanilla

From app0's rodata: target token `UNIFIED_ESP32C3_LR1121_RX` with the
hardware.json blob already captured above. When downloading vanilla
ELRS 3.x:
- Choose **"UNIFIED"** target (ESP32-C3 + LR1121 generic)
- At first boot, RX will enter Bluetooth LE / WiFi AP config mode and
  expect hardware.json upload via the ELRS Configurator — **we have
  that JSON in the dump**, can replay it.

