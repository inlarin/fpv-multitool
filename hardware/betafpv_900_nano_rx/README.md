# BetaFPV 900 MHz ELRS Nano RX

Identified live from a flash dump pulled by the host board (Waveshare
ESP32-S3-LCD-1.47B) over the existing web API on 2026-04-30. The receiver was
attached to Port B (GP10/GP11 + 3.3 V + GND) in ROM DFU.

Process and patches that made the dump possible are in
[`research/RX_DUMP_VIA_BOARD.md`](../../research/RX_DUMP_VIA_BOARD.md).

## Identity

| Field | Value |
|---|---|
| Chip | ESP8285 (ESP8266 family, 1 MB embedded flash) |
| ROM magic (UART_DATE) | `0xfff0c101` |
| ELRS target | `UNIFIED_ESP8285_900_RX` |
| Product string | `BETAFPV 900MHz RX` |
| Radio | `SX127X` (sub-GHz, 900 MHz band) |
| Firmware | ExpressLRS Unified, version `2.2.2-dev(38a443e)` |
| Captive portal IP | `10.0.0.1` |
| mDNS / hostname | `elrs_rx` |
| Domain | `1` (FCC915 in ELRS reg table) |

ELRS 2.2.2-dev is from early 2022 — multiple major bumps since. The RX is
field-flashable from web flasher / WiFi captive without any new hardware.

## Image header (offset 0)

```
e9  02  03  20  80 f4 10 40    00 f0 10 40 60 0d 00 00
└┬┘ ├┘ ├┘ ├┘   └─ entry pt ─┘
 │  │  │  └── flash params:
 │  │  │      high nibble 0x2 = 1 MB, low nibble 0x0 = 40 MHz
 │  │  └── SPI mode 0x03 = DOUT
 │  └── 2 segments
 └── ESP image magic 0xE9
```

Last non-`0xFF` byte at `0xff01f` — the entire 1 MB is in active use (image,
config sectors, options.json blob).

## Pin map (extracted options.json @ 0x81700)

```json
{
  "serial_rx": 3,  "serial_tx": 1,
  "radio_dio0": 4, "radio_dio1": 5,
  "radio_miso": 12, "radio_mosi": 13, "radio_nss": 15,
  "radio_rst": 2, "radio_sck": 14,
  "power_min": 0, "power_high": 2, "power_max": 2,
  "power_default": 2, "power_control": 0,
  "power_values": [120, 124, 127],
  "led": 16, "button": 0
}
```

## Hardware config (@ 0x81500)

```json
{
  "flash-discriminator": 1202975067,
  "wifi-on-interval": 60,
  "lock-on-first-connection": true,
  "domain": 1
}
```

## Files

- `dump_2026-04-30.bin` — full 1 MB flash of the original firmware (ELRS
  2.2.2-dev), MD5 `9f87f4e370ac0df8c15e2e25ddb9a712`. Use as restore source
  if a future flash bricks the RX.
- `vanilla_3.6.3/firmware.bin` — vanilla ELRS 3.6.3 UNIFIED_ESP8285_900_RX
  FCC build, pulled from `artifactory.expresslrs.org`. MD5
  `b474b5b650fa85ff47dece32f76dbe47`.
- `vanilla_3.6.3/hardware.json` — pin map (`Generic 900.json` from ELRS
  hardware repo, byte-identical to what was extracted from the original
  dump's appendix slot).

## Currently flashed (2026-04-30)

ELRS **3.6.3** (target `UNIFIED_ESP8285_900_RX`, FCC915 region) with the
appendix patched in via the host board's `/api/elrs/firmware/patch`:

| Field | Value |
|---|---|
| Bind phrase | `Morozov` |
| UID | `07:23:27:7C:CC:13` (= `MD5("-DMY_BINDING_PHRASE=\"Morozov\"")[0:6]`) |
| product_name | `BETAFPV 900MHz Nano RX` |
| lua_name | `BFPV Nano 900RX` |
| domain | `1` (FCC915) |
| wifi-on-interval | 60 s |
| lock-on-first-connection | true |

Verified live via `POST /api/elrs/device_info` after `RUN_USER_CODE`:
```
{"name":"BFPV Nano 900RX","sw_version":"0.3.6.3", ...}
```

**Pair-side requirement:** the bound TX module must be configured with the
same phrase `Morozov` (NOT `morozov`, NOT `мороз`) — the binding side
computes the same MD5 and must arrive at the same 6-byte UID.

## How the dump was produced

```
POST /api/setup/apply       device=receiver role=host
POST /api/elrs/dfu/begin?stub=1
POST /api/flash/dump/start  offset=0&size=0x100000&use_sticky=1
GET  /api/flash/dump/status (poll until running:false)
GET  /api/flash/dump/download → dump_2026-04-30.bin
POST /api/elrs/dfu/end
```

The `use_sticky=1` flag was added to `dump/start` in this same session — see
`src/web/http/routes_flash.cpp` and `src/bridge/esp_rom_flasher.cpp` for the
short-final-block padding fix that lets the ESP8266 stub upload succeed.
