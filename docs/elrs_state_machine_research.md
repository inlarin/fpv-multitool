# ELRS runtime state machine — 3.5.3 / 3.6.3 / 4.0.0

Deep dive across three tags of <https://github.com/ExpressLRS/ExpressLRS>, cross-checked with local `hardware/bayckrc_c3_dual/elrs_3_6_3_src/`. Complements [`reference_elrs_rx_modes.md`](../../.claude/projects/c--Users-inlar-PycharmProjects-esp32/memory/reference_elrs_rx_modes.md) — that doc has byte-exact CRSF `bl/bd/mm` + MSP `0x0E` frames and the ESP32-C3 ROM notes; this one adds state-machine/boot/diff context.

## TL;DR

- **11 `connectionState` values, unchanged 3.5 → 4.0** (`common.h:37-53`). All pure RAM — none persist in NVS.
- **No "start in WiFi" persistent flag** anywhere. Boot always starts app-side. WiFi entry = (a) `AUTO_WIFI_ON_INTERVAL` timer elapses with zero link, (b) MSP-over-CRSF `0x0E`, (c) LUA "Enable WiFi" param. **Three-power-cycles-without-connect → BINDING mode, not wifi** (`rx_main.cpp:1740-1744`).
- **Power-on counter is the only persistent boot-decision input** (`rx_main.cpp:1510-1523`): NVS/EEPROM-stored, ++ on each boot, cleared to 0 if link ≥2 s. **Not readable via CRSF** — the plate can't query it.
- **3.6.3 forces LR1121 FW upgrade to v1.0.4 (type 0xF3)** on first boot (`LR1121.cpp:56-91`). Bypassed by tag file `/lr1121.txt` on FS. 3.5.3 has no upgrade logic. 4.0 identical to 3.6.
- **CRSF/MSP API is byte-stable across all three versions.** `msptypes.h` diff = 0. Only 4.0 breaking change is SPIFFS → LittleFS and config version bumps.

## Version diffs that matter

| Topic | 3.5.3 | 3.6.3 | 4.0.0 |
|---|---|---|---|
| `RX_CONFIG_VERSION` / `TX_CONFIG_VERSION` | 9 / 7 | 9 / 7 | **11 / 8** (`config.h:18-19`) |
| Filesystem | SPIFFS | SPIFFS | **LittleFS** (`options.cpp:264`) |
| LR1121 forced-upgrade FW | none | `lr1121_transceiver_F30104.h` | same as 3.6 |
| CRSF command handler | `telemetry.cpp:243-280` | `rx_main.cpp` via `RXEndpoint` | **`rx-crsf/RXEndpoint.cpp:23-84`** (split) |
| STM32 reset_into_bootloader | yes | yes | **removed** (`rx_main.cpp:2166-2177`) |
| `MSP_ELRS_SET_RX_LOAN_MODE` 0x0F | defined | defined | **commented out** |
| MSP op codes | identical | identical | identical |
| CRSF frame types | identical | identical | identical |

## `connectionState_e` full enum (`common.h:36-53`, all versions)

| # | Name | Meaning |
|---|---|---|
| 0 | `connected` | RX: telem bi-dir. TX: link flowing |
| 1 | `tentative` | RX only — just synced, waiting for 2nd packet |
| 2 | `awaitingModelId` | TX only — handset attached, waiting model-match |
| 3 | `disconnected` | no link (default post-setup on RX) |
| 4 | `MODE_STATES` | sentinel |
| 5 | `noCrossfire` | TX only; no handset UART detected (default TX) |
| 6 | `bleJoystick` | TX only |
| 7 | `NO_CONFIG_SAVE_STATES` | sentinel |
| 8 | `wifiUpdate` | SoftAP/STA running, radio off |
| 9 | `serialUpdate` | in-app stub flasher on Serial |
| 10 | `FAILURE_STATES` | sentinel |
| 11 | `radioFailed` | `Radio.Begin()` returned false |
| 12 | `hardwareUndefined` | `options_init()` failed — WiFi-only |

## RX boot decision tree (4.0 canonical)

```
power-on
  ├── options_init() — loads hardware.json (FS overlay + flash-trailer fallback by "flash-discriminator") [options.cpp:258-294]
  │     └── fail → hardwareUndefined + WiFi auto-start
  ├── eeprom.Begin(); config.Load()                                    [rx_main.cpp:1505-1507]
  ├── poc = GetPowerOnCounter()
  │     ├── bound && poc<3 → poc++; Commit()                           [rx_main.cpp:1510-1513]
  │     └── else fall through
  ├── setupTarget; setupSerial; setupRadio
  ├── connectionState = disconnected (default)
  ├── defer 2000 ms: if (!connected) poc=0                             [rx_main.cpp:1517-1522]
  └── loop() — possible transitions:
        • packet received → tentative → connected → poc=0              [rx_main.cpp:2201-2204]
        • !bound & !binding → EnterBindingMode()
        • poc>=3 & !binding → EnterBindingMode() + poc latched to 3    [rx_main.cpp:1740-1744, 1908]
        • disconnected + wifi-interval timer → wifiUpdate               [devWIFI.cpp:1301-1312]
```

## TX boot (4.0; 3.5/3.6 virtually identical)

```
power-on
  ├── setupHardwareFromOptions()
  ├── eeprom.Begin(); config.Load()                                    [tx_main.cpp:1429-1431]
  ├── Radio.Begin() ok → noCrossfire                                   [tx_main.cpp:1462]
  ├── crsfTransmitter.begin(); handset→registerCallbacks(UARTconnected)
  └── loop — handset UART sync → disconnected → awaitingModelId → connected
```

Default TX state: `noCrossfire`. `wifi_auto_on_interval` timer also fires if `connectionState < wifiUpdate && !wifiStarted` (`devWIFI.cpp:1293-1298`).

## Transitions we can trigger from the plate

```
                       RX (ESP32-C3, our target)
 setup() ─► disconnected ◄─────── wifiUpdate
              │                       ▲
              │   pkt RX              │ MSP 0x0E (SET_RX_WIFI)
              ▼                       │   or auto-wifi timer
           tentative ─► connected     │
              │                       │
  CRSF `bl`   ▼                       │ HTTP POST /reboot (our plate cannot reach; needs phone on RX SoftAP)
          serialUpdate                │
          (stub @420000)              │
                                      │
  Physical BOOT+power → ROM DFU (115200, external only — plate has no RESET)
```

## CRSF frames (all @420 000 baud, CRC8 poly 0xD5 over `[type..last_payload]`)

Authoritative source: `src/python/bootloader.py` in each tag.

| Action | Bytes | Source |
|---|---|---|
| Reboot RX to stub flasher | `EC 04 32 62 6C 0A` | `bootloader.py:1` |
| Enter binding | `EC 04 32 62 64 <crc>` | `bootloader.py:3` |
| Set model-match | `EC 05 32 6D 6D <id> <crc>` | `bootloader.py:5` |
| Official CRSF bind (TX path) | `C8 07 32 EC C8 10 01 <crc>` | `RXEndpoint.cpp:52` |
| DEVICE_PING probe | `EC 04 28 00 EC <crc>` | `CRSFEndpoint.cpp:402-414` |
| PARAMETER_READ (LUA enum) | `EC 04 2C <fieldId> <chunk> <crc>` | `CRSFEndpoint.cpp:264` |
| PARAMETER_WRITE (LUA set) | `EC ?? 2D <fieldId> <data…> <crc>` | `TXModuleEndpoint.cpp:62-69` |

Responses: DEVICE_INFO `0x29` (name, serialNo, HW, SW u32 BE, fieldCnt), PARAMETER_SETTINGS_ENTRY `0x2B` (chunked).

**No CRSF getter for LR1121 FW version, connectionState, or powerOnCounter.**

## MSP-over-CRSF opcodes (identical across all 3 versions, `msptypes.h`)

Wrapping: CRSF `0x7C` (WRITE) carries MSP-V2 (`flags, func_lo, func_hi, size_lo, size_hi, payload, crc8-dvb-s2`), fragmented in 8-byte chunks.

| Func | Name | Effect on RX |
|---|---|---|
| 0x09 | `MSP_ELRS_BIND` | one-time bind phrase seed (`rx_main.cpp:963`) |
| 0x0A | `MSP_ELRS_MODEL_ID` (via `MSP_SET_RX_CONFIG=45`) | set model-match; same as CRSF `mm` |
| 0x0C/0x0D | `SET_TX/VRX_BACKPACK_WIFI_MODE` | backpack only |
| **0x0E** | **`MSP_ELRS_SET_RX_WIFI_MODE`** | **`deferExecutionMillis(500, setWifiUpdateMode)`** (`rx_main.cpp:1224-1230`) — the way to put RX into wifi |
| 0x10 | `GET_BACKPACK_VERSION` | backpack |
| 0x20/0x21 | `POWER_CALI_GET/SET` | TX only |
| 0xFD | `MAVLINK_TLM` | mavlink encapsulation either direction |

**There is no "exit WiFi" MSP command.** Return from wifi = HTTP POST `/reboot` to 10.0.0.1 or power-cycle.

## Persistence map (ESP32)

| Storage | Key/path | Contents |
|---|---|---|
| NVS `"ELRS"` (`config.cpp:151`) | `rx_version`/`tx_version` (magic+version), `vtx`, `fanthresh`, `fan`, `motion`, `dvr*`, `button1/2`, `backpackdisable`, per-model keys | scalar config |
| EEPROM `Put(0, m_config)` | raw `rx_config_t`/`tx_config_t`: UID, bindStorage, PWM map, **powerOnCounter**, model-match | bulk config |
| LittleFS (4.0) / SPIFFS (3.5-3.6) | `/hardware.json` | pin overlay; flash-discriminator check |
| same | `/options.json` | wifi-on-interval, wifi-ssid/pw, rcvr-uart-baud, is-airport, uid override |
| same | `/lr1121.txt` | empty tag file — if present, skip forced LR1121 upgrade (3.6+) |
| RTC SRAM (TX) | `rtcModelId` (`TXModuleEndpoint.cpp:12`) | survives warm reset |
| Flash EMPTY_SECTOR (ESP8266 only) | byte-count sentinel | powerOnCounter hack |

Reset-to-defaults: `POST /reset` (wifi) nukes FS + NVS. Button ACTION_RESET_REBOOT. `poc>=3` latch is NOT a full reset — just binding mode.

## Auto-WiFi timer

`firmwareOptions.wifi_auto_on_interval` loads from `/options.json` field `wifi-on-interval` × 1000 ms (`options.cpp:178-179`). **Default `-1` (disabled).** Set via compile `-DAUTO_WIFI_ON_INTERVAL=N`, baked into `options.json` trailer appended to `.bin`, editable at runtime by `POST /options.json`.

Timer only fires when:
- RX: `connectionState==disconnected` && `!webserverPreventAutoStart`
- TX: `connectionState < wifiUpdate && !wifiStarted`

Any successful link sets `webserverPreventAutoStart=true` (`rx_main.cpp:870`) → timer disarmed for session. Binding mode has a 60 s minimum delay before auto-wifi (`devWIFI.cpp:1305-1311`), regardless of interval.

Defaults per version: all three default to -1. Our `--auto-wifi 60` flag to `binary_configurator.py` is not a runtime override — it bakes into the firmware's trailer `options.json`.

## LR1121 transceiver FW management

- **3.5.3**: no upgrade code; factory v1.0.3 stays.
- **3.6.3+**: on `Radio.Begin()`, if `version.type != 0xF3 || version.version != 0x0104` AND `!FS.exists("/lr1121.txt")` → auto-flash bundled `lr11xx_firmware_image` with 32-bit bswap (`LR1121.cpp:59-84`). Failure → `Begin()` returns false → `radioFailed`. Dual-radio boards upgrade both.
- **Read version at runtime**: `Radio.GetFirmwareVersion(radioNum)` returns `{hardware, type, version}`. **No CRSF getter** — only HTTP GET `/lr1121.json` in wifi mode (`lr1121.cpp:81-97`).
- **Rollback / custom FW**: HTTP POST `/lr1121` with `X-Radio: 1|2` + `X-FileSize` headers + raw blob body (`lr1121.cpp:54-70`). Success → auto-creates `/lr1121.txt` → disables future auto-upgrade. Delete via `POST /reset`.

## Safety boundaries

| Action | Consequence | Recovery |
|---|---|---|
| CRSF `bl` to bootlooping RX | silent no-op @420 k | power-cycle |
| MSP `0x0E` while armed | RX drops link mid-flight | reboot |
| `POST /update` with mismatched options trailer | falls back to flash-embedded options | reboot |
| `POST /hardware.json` bad pinout | `hardwareUndefined` → SoftAP only | `POST /reset` from SoftAP |
| Stub flasher touching bootloader@0x0 or part-table@0x8000 | **impossible** — stub uses `esp_ota_*`, bounds-checked | — |
| `POST /reset` with FC connected | wipes NVS+FS → unbound RX | re-bind |
| Bad `PARAMETER_WRITE` fieldId | silently ignored | — |
| `SUBCMD_RX_BIND` to TX in `awaitingModelId` | TX enters bind regardless (`TXModuleEndpoint.cpp:44`) | power-cycle |

Nothing sendable over CRSF can brick an RX — the stub flasher is bounded to inactive OTA slot. Bricking requires ROM DFU + bad write, which needs the physical RESET line our plate lacks.

## What THIS plate CAN / CANNOT do programmatically

### CAN (via Port B CRSF @420 000)

- **Probe app alive** — `DEVICE_PING` → 50 ms wait for DEVICE_INFO 0x29. Distinguishes "app running CRSF" from "everything else".
- **Force into stub flasher** — `EC 04 32 62 6C 0A`. Then talk ESP-SLIP @420 000 (NOT 115 200) to write ota_0/ota_1.
- **Force into WiFi AP** — MSP-over-CRSF `0x0E` (wrapped frame in memory doc), ~700 ms settle.
- **Enter binding / set model-match** — `bd`/`mm` bytes or MSP 0x09/0x0A.
- **Read firmware name + version u32** — from DEVICE_INFO reply.
- **Enumerate & write LUA params** — PARAMETER_READ/WRITE chunked protocol; covers every RX-exposed config field.
- **Drive a connected TX** — same frame set addressed to TX if plate emulates handset CRSF timing.
- **Return from stub flasher to app** — ESP-SLIP `FLASH_END` payload `00 00 00 00` (reboot=yes). Already implemented.

### CANNOT

- **Force ROM DFU from app side** — no RESET wire, only BOOT (GPIO3→RX GPIO9). User must physically press BOOT + power-cycle. No persistent "re-enter BL" flag on ESP32-C3 to work around this.
- **Return from wifiUpdate via CRSF** — no exit-wifi opcode. Only paths: user's phone hits `/reboot` on 10.0.0.1, user power-cycles, or auto-wifi timer expires — which it never does once wifi started.
- **Read/write LR1121 FW over CRSF** — HTTP only, wifi-mode only.
- **HTTP-probe 10.0.0.1 directly** — our plate is an AP our phone connects to, not a STA joined to RX. Changing that requires a firmware redesign (STA-join mode) and UI flow.
- **Know the exact `connectionState`** — DEVICE_PING reply only tells "app CRSF alive"; `serialUpdate`/`wifiUpdate`/`radioFailed` all look identical (no reply).
- **Distinguish MILELRS vs vanilla** without a flash dump — device name is build-provided and MILELRS spoofs it.
- **Read or reset the power-on counter** over CRSF — no getter, clearing only happens as side-effect of a real link connect (which needs handset-emulation).
- **Pick boot partition (app0 vs app1)** over CRSF — OTADATA is bootloader-scope; writable only via stub flasher or ROM DFU.

## Cross-refs

- Byte-exact CRSF + MSP wrapping, ESP32-C3 ROM quirks, stub flasher opcodes → [`reference_elrs_rx_modes.md`](../../.claude/projects/c--Users-inlar-PycharmProjects-esp32/memory/reference_elrs_rx_modes.md)
- Current implementation status + `/api/*` → [`rx_programmatic_control_research.md`](rx_programmatic_control_research.md)
- Flash audit (app0 vs vanilla) → [`elrs_flash_audit_2026-04-21.md`](elrs_flash_audit_2026-04-21.md)
- MILELRS forensics → [`hardware/bayckrc_c3_dual/ANALYSIS.md`](../hardware/bayckrc_c3_dual/ANALYSIS.md)

Research 2026-04-22; vanilla tags only. MILELRS forks inherit the same state machine but may add proprietary MSP opcodes not reversed here.
