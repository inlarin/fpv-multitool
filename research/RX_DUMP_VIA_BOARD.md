# RX dump via board (no host-side esptool)

**Goal:** identify an unknown ELRS-class receiver and dump its full flash image
using ONLY the Waveshare ESP32-S3 host board (Port B / GP10 + GP11 / Serial1)
plus the existing web API at `192.168.32.50`. No host-side `esptool.py`, no
USB2TTL bridge mode — the board itself is the flasher.

**Outcome:** ✅ 1 MB flash dumped through the web API in one session, identified
as **BetaFPV 900 MHz ELRS Nano RX** (`UNIFIED_ESP8285_900_RX`, ELRS 2.2.2-dev).
See [`hardware/betafpv_900_nano_rx/`](../hardware/betafpv_900_nano_rx/).

This document is the action log + spec for the eventual first-class feature
(`/api/elrs/identify_and_dump?stub=1`).

## Hardware setup

- Host: **Waveshare ESP32-S3-LCD-1.47B** (board #1, MAC `3C:DC:75:6E:CE:A8`,
  IP `192.168.32.50`).
- Receiver: BetaFPV 900 MHz Nano RX (ESP8285 + SX127x), in ROM DFU, wired to
  Port B (GP10, GP11) + 3.3 V + GND.
- Reflashing the host over WiFi (`/api/ota/upload`) does NOT drop the RX out
  of DFU on this board. Port B 3.3 V regulator is always-on while USB-C is
  plugged, even across `ESP.restart()`. **OTA upload is the right path**;
  the `pio run -t upload` USB DTR/RTS path failed to hit ROM (`No serial data
  received`) because the AsyncWebServer was holding CDC traffic.

## Findings (chip + why the existing dump path failed)

### Chip
```
POST /api/setup/apply       device=receiver role=host    # frees Port B
POST /api/elrs/dfu/begin?stub=1
→ chip:"ESP8266", magic_hex:"0xfff0c101"  (this is the family magic for ESP8285 too)
```

### ESP8266 ROM does not implement READ_FLASH_SLOW (0x0E)
The original `/api/flash/dump/start` calls `ESPFlasher::readFlash(cfg, …)`
which opens its own ROM session and issues `CMD_READ_FLASH_SLOW (0x0E)` for
every 64 B chunk. ESP32 family ROMs implement `0x0E`; **ESP8266 / ESP8285 ROMs
do not — the stub does**. So the dump aborted at 3 % with
`READ_FLASH rejected or bad response`.

The sticky DFU session opened with `?stub=1` was the right primitive — but the
dump task ignored it and opened its own session. Two patches were needed.

## Patches applied this session

### 1. `src/web/http/routes_flash.cpp` — sticky-aware dump path

Added `?use_sticky=1` to `/api/flash/dump/start`:

- When set, the route validates that a sticky DFU session is open (rejects
  with 409 otherwise), skips the `PinPort::acquire` + CRSF pause (the sticky
  session already owns both), and the xTask reads via
  `readFlashMultiInOpenSession({{offset, size, buf}}, 1)` in 32 KB chunks.
  No `Serial1.begin/end`, no second sync — the stub is already running.
- Chunk progress shown in `/api/flash/dump/status`. After completion the
  task does NOT release Port B or resume CRSF — sticky session keeps owning
  them until `/api/elrs/dfu/end`.

### 2. `src/bridge/esp_rom_flasher.cpp` — `loadStub` short-block fix

The first stub-load attempt on ESP8266 returned
`stub load failed: FLASH_DATA failed`. Cause: `loadStub`'s `upload_segment`
helper was padding every `MEM_DATA` block to exactly 1024 B, including the
final short block (40 B for `stub_8266_text` of 8232 B over 9 blocks).
ESP32-family ROMs tolerate this. **ESP8266 ROM rejects it** because the
checksum the ROM computes covers `block_size` bytes from the header but the
header itself still says `block_size = actual_payload`. esptool.py never pads.

Fixed by sending `actual_size` for the final block (`memData(blk, sz, i)` in
place of `memData(blk, BLOCK, i)`) and dropping the `memset(blk + sz, 0)`.
Safe across all chips — esptool.py does the same.

After this, `dfu/begin?stub=1` returned `stub_loaded:true, stub_msg:"stub
running"` and the `?use_sticky=1` dump completed at ~6 KB/s wall-clock
(115200 baud, CMD_READ_FLASH 0xD2 streaming).

## Action log (verbatim)

| # | Step | Outcome |
|---|------|---------|
| 1 | `GET /api/setup/status` | Port B owner = `autel_battery` (I2C) — must switch |
| 2 | `POST /api/setup/apply device=receiver role=host` | Port B → UART, no reboot |
| 3 | `POST /api/elrs/dfu/begin?stub=1` (build A) | chip=ESP8266, magic=0xfff0c101; **stub upload failed** (FLASH_DATA) |
| 4 | `POST /api/flash/read_bytes offset=0 size=16` | rejected — single-shot endpoint opens own session w/o stub |
| 5 | Patch dump: `?use_sticky=1` | `routes_flash.cpp` — sticky-aware xTask |
| 6 | `pio run -e esp32s3` + `curl POST /api/ota/upload` | OTA push over WiFi (USB DTR/RTS path failed) |
| 7 | Wait reboot ~50 s, board online | Setup state preserved (NVS) |
| 8 | `POST /api/elrs/dfu/begin?stub=1` (build B) | sync OK, **stub still failed** — same FLASH_DATA |
| 9 | Patch `loadStub`: drop final-block padding | `esp_rom_flasher.cpp` |
| 10 | `pio run -e esp32s3` + OTA push | Build B+ flashed |
| 11 | `POST /api/elrs/dfu/begin?stub=1` (build B+) | **stub_loaded:true, stub running** ✅ |
| 12 | `POST /api/flash/dump/start offset=0 size=0x100000 use_sticky=1` | Started |
| 13 | Poll `/api/flash/dump/status` | Reading (stub) 28 → 40 → 53 → 68 → 81 → 96 → 100 |
| 14 | `GET /api/flash/dump/download` → 1 048 576 B | MD5 `9f87f4e370ac0df8c15e2e25ddb9a712` |
| 15 | Identify (Python `re` strings sweep) | `UNIFIED_ESP8285_900_RX`, ELRS `2.2.2-dev(38a443e)`, BETAFPV 900MHz RX, SX127X |
| 16 | `mv hardware/_unknown_rx_dfu hardware/betafpv_900_nano_rx` | + README.md with full details |
| 17 | `POST /api/elrs/dfu/end` | (already auto-closed by idle watchdog) |

## Identification cheat sheet (post-dump)

For the next unknown RX, after the dump is on disk:

1. **Header @ 0x0** — first byte 0xE9 (ESP image magic) + flash_size byte
   (high nibble of byte 3): `0=512 KB, 1=256 KB, 2=1 MB, 3=2 MB, 4=4 MB`.
2. **Strings sweep** (Python — Git Bash has no `strings`):
   `re.findall(rb'[\x20-\x7e]{6,}', data)` then bucket by keyword:
   `expresslrs|elrs|target|HW_NAME|VERSION|GIT_HASH|REG_REGION` for the firmware
   family; vendor list `foxeer|bayck|happymodel|matek|radiomaster|jumper|frsky|crossfire|axisflying|geprc|namimno|emax|betafpv` for the product.
3. **ELRS target_name + git hash** sit near each other in app rodata. Look for
   `UNIFIED_<chip>_<band>_RX|TX` plus a 6-7-char hex token like `38a443e`.
4. **options.json blob** — find with `re.finditer(rb'\{\"[\x20-\x7e]+\}')` and
   filter for `serial_rx`, `domain`, `wifi-ssid`. ELRS embeds it twice — once
   as the hardware spec, once as user config.
5. **Product string** like `BETAFPV 900MHz RX` is the mDNS friendly name and
   appears alongside `expresslrs` and the captive-portal IP `10.0.0.1`.

## Recommendations for the eventual feature

The patch added today is the seed. The first-class feature should:

1. **Single endpoint `/api/elrs/identify_and_dump?stub=1`** that runs the full
   pipeline atomically: openSession → loadStub → chipInfo (with proper ESP8266
   MAC path from `0x3FF00050/0x54`) → JEDEC → readFlashMultiInOpenSession over
   the entire flash size derived from JEDEC → close. Returns identification
   JSON immediately, dump ready for download.
2. **Auto flash-size derivation** — the existing dump endpoint takes `size`
   from the caller; the feature should read JEDEC ID once stub is loaded and
   default to the actual chip size.
3. **`chipInfoInOpenSession()` ESP8266 MAC fix** — currently returns all-zero
   MAC because it reads ESP32-family eFuse offsets. Add a magic-keyed branch
   that reads `0x3FF00050/0x3FF00054` for ESP8266.
4. **Optional SD streaming** — full 4 MB+ dumps shouldn't have to fit in
   PSRAM. Stream straight to `/sd/dumps/<chip>_<mac>_<unix>.bin`.
5. **ROM-direct fallback** — if `loadStub` fails (older ROM, forked esptool
   stubs, etc.), fall back to `CMD_READ_FLASH (0xD2)` against the ROM directly
   for ESP8266. Slower but always works.

The block-size padding bug in `loadStub` is now fixed for all chips —
keep the actual-size form, do not regress it.

## Files left in the tree by this session

- `src/web/http/routes_flash.cpp` — `?use_sticky=1` branch in `/api/flash/dump/start`.
- `src/bridge/esp_rom_flasher.cpp` — actual-size `memData` in `loadStub`.
- `hardware/betafpv_900_nano_rx/dump_2026-04-30.bin` — the dump itself.
- `hardware/betafpv_900_nano_rx/README.md` — identification record.
- `research/RX_DUMP_VIA_BOARD.md` — this document.
