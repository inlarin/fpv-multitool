# RX serial-protocol support roadmap

Currently we only talk **CRSF** to a connected receiver (`src/crsf/`).
ELRS receivers can be configured to output any of the standard FPV
serial protocols — we want to be able to auto-detect and auto-adapt.

## Protocols ELRS exposes (from vanilla 3.6.3 target tree)

From `options.json` / rodata string `"Off;CRSF;Inverted CRSF;SBUS;Inverted SBUS;SUMD;DJI RS Pro;HoTT Telemetry;Tramp;SmartAudio"` and
`"CRSF;Inverted CRSF;SBUS;Inverted SBUS;SUMD;DJI RS Pro;HoTT Telemetry;MAVLink"`:

| # | Protocol | Baud | Framing | Direction |
|---|----------|------|---------|-----------|
| 1 | **CRSF** | 420000 | 8N1 | RX↔FC bidirectional |
| 2 | **Inverted CRSF** | 420000 | 8N1 (inverted) | as CRSF but for F3/F4 FCs |
| 3 | **SBUS** | 100000 | 8E2 inverted | FC→RX (telemetry), RX→FC (channels) |
| 4 | **Inverted SBUS** | 100000 | 8E2 | for boards that need non-inverted SBUS |
| 5 | **SUMD** | 115200 | 8N1 | channels, Graupner spec |
| 6 | **DJI RS Pro** | custom | — | gimbal control |
| 7 | **HoTT Telemetry** | 19200 | 8N1 | bidirectional, Graupner |
| 8 | **MAVLink** | 57600 | 8N1 | mavlink v1/v2, bidirectional |
| 9 | **Tramp / SmartAudio** | 9600 | 8N1 | VTX control only |

## Auto-probing sequence

Extend the existing `/api/port/autodetect` logic. Per signal, try both
pin swap directions, listening for protocol-specific sync bytes:

| Signal | Sync byte(s) | Baud | Framing |
|--------|-------------|------|---------|
| `crsf` | `0xC8` (RX→FC broadcast) | 420000 | 8N1 |
| `crsf_inv` | `0xC8` | 420000 | 8N1 inverted |
| `sbus` | `0x0F` | 100000 | 8E2 inverted |
| `sbus_noninv` | `0x0F` | 100000 | 8E2 |
| `ibus` | `0x20 0x40` | 115200 | 8N1 |
| `sumd` | `0xA8 0x01` / `0xA8 0x81` | 115200 | 8N1 |
| `hott` | `0x7C / 0x7D` (module responses) | 19200 | 8N1 |
| `mavlink` | `0xFE` (v1) / `0xFD` (v2) | 57600 | 8N1 |
| `elrs_rom` | any byte after SYNC | 115200 | 8N1 |

Already implemented: `crsf`, `sbus`, `ibus`, `i2c`, `elrs_rom`.
To add: `crsf_inv`, `sbus_noninv`, `sumd`, `hott`, `mavlink`.

## New protocol parsers to implement in `src/rc_sniffer/` or `src/fc/`

Reuse existing module structure. Each protocol needs:

1. `start(proto)` / `stop()` / `loop()` — run inside main loop, poll
   Serial1 bytes, reassemble frames, update state struct.
2. State struct with: connected bool, channels[], telemetry fields
   (voltage, current, GPS, attitude as applicable), bad_crc counter,
   frame_count.
3. Web endpoint: `GET /api/<proto>/state`, `POST /api/<proto>/start`,
   `POST /api/<proto>/stop`.
4. UI card per protocol OR one unified "RX Telemetry" card that
   auto-switches display based on active protocol.

### Per-protocol parsing TL;DR

- **CRSF** — already done (`src/crsf/`).
- **Inverted CRSF** — CRSF parser + `Serial1.begin(420000, SERIAL_8N1, rx, tx, /*invert=*/true)`. Same frame logic.
- **SBUS** — already in `src/rc_sniffer/` (100000 8E2 inverted, 25-byte frame). Expose via new /api/sbus/*.
- **SBUS non-inverted** — same parser, different invert flag.
- **iBus** — already in rc_sniffer (115200 8N1, 32-byte frame).
- **SUMD** — 115200 8N1, variable-length frame, Graupner CRC-16 (poly 0x1021).
- **HoTT** — half-duplex 19200 8N1, bidirectional. Receiver polls modules, each module responds with a specific packet type. Complex — 2-week effort.
- **MAVLink** — 57600 8N1, MAVLink v1/v2 frame (magic 0xFE / 0xFD + length + seq + sysid + compid + msgid + payload + crc). Library available (mavlink_helpers.h). Heartbeat message (msgid=0) gives us "alive" signal instantly.

## Auto-probing UI (Setup tab)

Extend current Auto-detect card with **"Auto-probe ALL protocols"**
button that runs all signals in sequence until one hits:

1. Loop over [`crsf`, `crsf_inv`, `sbus`, `sbus_noninv`, `ibus`,
   `sumd`, `mavlink`, `hott`, `i2c`] — each gets 1.5 s per pin-direction.
2. On first hit: save protocol + swap state to NVS, show toast.
3. Start the matching service automatically.
4. If nothing found after all signals: report "no signal — check
   wiring, GND, power, and receiver mode".

Total worst-case scan time: ~27 s (9 protocols × 1.5 s × 2 swap dirs).

## Priority

- **High**: SBUS + iBus expose via /api/sbus/*, /api/ibus/* (parsers
  already exist in rc_sniffer). Cheapest win.
- **Medium**: MAVLink — library is free, lots of downstream value
  (can talk to ArduPilot / PX4 autopilots directly).
- **Medium**: CRSF inverted variant — one-liner on existing code.
- **Low**: SUMD, HoTT — niche; implement only if a real user asks.

## Test hardware availability

We have Bayck RC C3 Dual running MILELRS, configurable via the web
UI (once we get into it) to emit any of the above protocols. After
flashing vanilla ELRS 3.6.3 into app0 we'll have both MILELRS + vanilla
as test targets on one device — good coverage.

---

## ROM dump auto-detection (new, 2026-04-19)

Currently `/api/flash/dump/start` requires user to guess `size` (1/2/4/8 MB).
We should auto-detect:

### What's connected
- After SYNC + SPI_ATTACH, read chip ID via `READ_REG` on the SPI Flash
  Configuration Register (address 0x60002000 on ESP32-C3). Bottom byte is
  manufacturer ID, next bytes are memory-type + capacity code.
- Look up JEDEC ID table: common values — WinBond W25Q32 (0xef4016 = 4 MB),
  W25Q64 (0xef4017 = 8 MB), W25Q128 (0xef4018 = 16 MB), XM25QU64A (0x204017 = 8 MB),
  GigaDevice GD25Q32 (0xc84016 = 4 MB) etc.
- Report: vendor, part number, capacity → user sees "Detected: WinBond W25Q32 (4 MB)".

### Chip identification
- `READ_REG 0x6000200C` → EFUSE_BLK0_RD_DATA_REG on ESP32-C3 (gives chip rev).
- Or use CMD_SPI_FLASH_ID (0x9F) over the ROM's SPI passthrough — this is what
  esptool.py does with `esptool.py chip_id`.
- Additional: dump the efuse summary for the chip family (ESP32-C3 rev v1.x,
  which eFuses are burned, MAC address).

### UI changes (add to `docs/web_ui_rx_flash_todo.md`)

In the **Dump Receiver Firmware** card, add a new subsection ABOVE the
offset/size picker:

```
┌─────────────────────────────────────────────────┐
│ Detect receiver                                  │
│  [Detect] → GET /api/flash/probe                 │
│                                                   │
│ Chip:       ESP32-C3 rev v0.4 (JEDEC ef4016)     │
│ Flash:      WinBond W25Q32  4 MB                  │
│ MAC:        24:0a:c4:01:02:03                    │
│ eFuses:     boot_sec=0, jtag_dis=0, ...           │
│                                                   │
│ [Use detected size (4 MB)] ← fills offset=0/size=0x400000 │
└─────────────────────────────────────────────────┘
```

### New backend endpoint
`GET /api/flash/probe` (RX must be in DFU):
- SYNC + SPI_ATTACH (no SPI_SET_PARAMS — we're detecting size first)
- Read JEDEC ID via chip-specific register read
- Read efuse block 0 for chip rev + mac
- Return JSON:
  ```json
  {
    "chip": "ESP32-C3",
    "chip_rev": "v0.4",
    "flash_jedec": "0xef4016",
    "flash_vendor": "WinBond",
    "flash_part": "W25Q32",
    "flash_size_bytes": 4194304,
    "flash_size_human": "4 MB",
    "mac": "24:0a:c4:01:02:03",
    "efuse_raw": "..."
  }
  ```
- 404 if not in DFU.

### Implementation notes
- JEDEC ID table: keep small (~30 common parts) as static const in
  `src/bridge/esp_rom_flasher.cpp`.
- Chip-detect register addresses are per-family — ESP32-C3 vs -S2 vs
  -S3 differ. Use the SYNC response hardware-ID bytes to pick the
  right probing approach.
- Error: if chip family can't be identified, fall back to asking user
  manually (current behaviour).

---

## Unified "RX Flash Station" UI (2026-04-19)

Collapse Dump / OTADATA / Slot-Flash / Auto-detect into **one** tab section
with a left-panel "Detected RX" header that drives everything below.

### Top header (sticky, always visible)

```
┌──────────────────────────────────────────────────────────────┐
│ [ Detect ] → GET /api/flash/probe                             │
│                                                                │
│ Detected:  ESP32-C3 rev v0.4, 4 MB flash, 1x app-slot layout   │
│ Active:    app1 (seq=2) → MILELRS v3.48                        │
│ Slots:     app0 (1.88 MB, vacant? / has ELRS 3.5.3)            │
│            app1 (1.88 MB, has MILELRS v3.48) ACTIVE            │
└──────────────────────────────────────────────────────────────┘
```

Populated from three endpoints merged client-side:
- `/api/flash/probe` → chip + flash size
- `/api/otadata/status` → active slot + sector states
- (new) `/api/flash/slot_info?slot=N` → reads the 32-byte ESP image header
  at slot-offset and reports image magic, load address, hash, approximate
  "looks like ELRS?" based on string presence.

### Per-slot row (one row per app partition)

```
┌──────────────────────────────────────────────────────────────┐
│ app0 @ 0x10000                                                │
│   Status: 🟢 valid image ("ExpressLRS 3.6.3" detected)        │
│   Size:   1.24 MB of 1.88 MB partition                         │
│   Actions:                                                      │
│     [Activate]      POST /api/otadata/select?slot=0            │
│     [Dump this slot] POST /api/flash/dump/start?offset=0x10000&size=0x1e0000 │
│     [Overwrite]     opens file picker, flashes at 0x10000      │
│     [Erase]         POST /api/flash/erase_region (DANGER confirm) │
└──────────────────────────────────────────────────────────────┘
```

Same card repeats for app1. The "Activate" button is greyed out on the
currently active slot. "Erase" requires typed confirmation for a slot
that's active (prevents accidental brick).

### "Full dump" button (separate card below)

Keeps current `/api/flash/dump/*` behaviour for whole-flash backup:
- Uses detected size (4 MB) from `/api/flash/probe`.
- Downloads as `dump_YYYY-MM-DD_HHMM.bin` with a md5 printed next to
  the button.
- "Restore from dump" inverts: pick a .bin file, flashes at offset 0
  with full 4 MB.

### Risk controls

- Any action that targets the CURRENTLY ACTIVE slot (flash / erase)
  requires confirmation dialog that types the slot name.
- "Erase OTADATA" button is below a collapsed `<details>` "Advanced".
- "Reflash bootloader + partition table" is even deeper — typed
  confirmation + double-check with current partition table
  signature.

### Implementation order

1. `/api/flash/probe` endpoint + JS handler to populate the header.
2. `/api/flash/slot_info?slot=N` endpoint — tiny wrapper that
   `readFlash` 0x10..0x200 from the slot, parses ESP image header
   + string-scans first page for version.
3. Unified card layout in web_ui.cpp (replaces the three separate
   "Slot-targeted flash" / "OTADATA" / "Dump firmware" cards added
   by the UI agent earlier).
4. Pre-fill offsets and sizes from detected data — user no longer
   enters hex by hand.

The existing cards the UI agent added become expert-mode fallbacks
behind `<details>`.

---

## Programmatic DFU entry/exit (2026-04-19)

Right now user must physically hold BOOT on the receiver and power-cycle
it to enter DFU. This is tedious (we did it 4+ times in one flash session).
We want the plate to do it electrically.

### Physical constraints (from Bayck RC C3 Dual dump)

- BOOT button wired to **GPIO 9** of the ESP32-C3 (from hardware.json
  `"button": 9`). LOW on boot = enter ROM bootloader.
- Chip's RESET pin is a dedicated EN pin on most modules. We'd need to
  drive it low then high to force a hardware reset.
- Power wire is VCC/GND — gating power electrically from the plate would
  need a MOSFET or load switch we don't have in current hardware.

### Options

**Option A — Three-wire DFU (requires new pin)**
Add a THIRD signal wire from plate to the RX's GPIO 9 pad. On-demand
pull LOW, then toggle power, then release HIGH. We only have 2
Port B signal pins; would need:
- Modify hardware to expose a 3rd signal (e.g. repurpose GP11 briefly
  as "BOOT" signal during DFU entry — but that breaks simultaneous UART).
- OR require user to add an extra wire to a free ESP32-S3 GPIO
  (e.g. use an unused GPIO on the pin header as DFU-strap output).

**Option B — CRSF reboot-to-bootloader command**
ELRS firmware supports the standard CRSF command `COMMAND_REBOOT_BOOTLOADER`
(realm 0x0A, subCommand 0x0B). We already have `/api/crsf/reboot` which
sends it. Problem: works only if:
1. RX is running an app firmware that HAS a CRSF listener (ELRS does,
   MILELRS may or may not — we saw it was silent when we tried).
2. The RX is configured to use CRSF on the wire (not SBUS/iBus/etc.).
3. The ELRS firmware sets its "boot pin strap" handler BEFORE the
   soft-reset so GPIO 9 ends up LOW during reboot. Vanilla ELRS does
   this.

**Option C — ESP32-ROM soft-reset command**
After SYNC'ing to the ROM bootloader, there's no single "exit DFU"
command. The closest is `FLASH_END` with reboot=1 (which we already
do) — rebooting into app mode.

**For exiting DFU programmatically (= going back to app without
power-cycle):** just send FLASH_END(reboot=1). This is what our
flash() and otadata/select already do after a write. For reads
(readFlash doesn't trigger reboot), we could add a "soft reboot"
endpoint:
```
POST /api/flash/exit_dfu → sends FLASH_END(reboot=1)
```

### Recommended implementation plan

Priority 1 — **exit DFU** (easier):
- Add `/api/flash/exit_dfu` endpoint that calls SYNC + FLASH_END(true).
  No hardware changes. Lets us flip RX back to app mode after dump or
  verify operations.

Priority 2 — **enter DFU via CRSF**:
- Improve `/api/crsf/reboot`: use `realm=0x0A cmd=0x0B` extended command
  (reboot to bootloader), then pause, then our DFU-mode ops.
- Add fallback: if CRSF no longer responds, fall back to user-prompt
  "please hold BOOT + power-cycle".
- Add a UI button "Put RX in DFU" that tries CRSF-reboot first, waits
  2 s, then probes for SYNC response; if no SYNC, shows the manual
  prompt.

Priority 3 — **enter DFU via hardware strap** (requires extra wire):
- Document in hardware/README.md a new "DFU strap" wire from ESP32-S3
  GPIO X to receiver GPIO 9.
- Add a small GPIO driver that pulls LOW, waits, releases.
- Combined with power-cycle (which still needs user), we could do full
  DFU entry without any user interaction.

### Notes on ELRS 3.x specifics

ELRS exposes `COMMAND_RECEIVER_REBOOT = 0x0A 0x0B` in its CRSF param
handler. Sending this triggers `delay(500); ESP.restart();` which if
combined with the right strap state would enter DFU. Need to trace
ELRS source to see if it also pulls the boot strap LOW before reboot.
