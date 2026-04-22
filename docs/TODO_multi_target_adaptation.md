# TODO — multi-target adaptation (2026-04-22)

Captured from user feedback during the Identity-card work. These touch
Flash Firmware + Dump flows, which currently bake ESP32-C3 / 4 MB /
ELRS-standard partition offsets into both the UI and backend. They
need to become *layout-aware* based on a probe of the attached device.

## Problem statement

Right now the UI hardcodes:

| UI control | Hardcoded value | Breaks when device isn't |
|---|---|---|
| Flash → Target | `app0 @ 0x10000`, `app1 @ 0x1F0000`, `full @ 0x0` | not ESP32-C3 4 MB, or not dual-slot |
| Dump → Size   | 1/2/4/8 MB | not 4 MB |
| Dump → Offset | default `0x0` | n/a |
| OTADATA raw   | `@0xe000/0xf000` | standard ELRS partition table |
| Slot-targeted | `app0 @0x10000 (1.88 MB)` / `app1 @0x1F0000 (1.88 MB)` | bigger slots on S3 TX |

These are correct for **~80% of modern ELRS RX** (ESP32-C3 4 MB single-radio
vanilla layout), but **break** on:

- **ESP8266 RX** (legacy) — 4 MB flash but NO dual-slot OTA. Usually single
  `sketch` partition at 0x0, `fs` (LittleFS/SPIFFS) at end, no OTADATA. Flash
  full image only. `app0 @ 0x10000` literally doesn't exist.
- **ESP32-S3 TX modules** (Radiomaster Ranger / Bandit / Nano TX) — 8 to
  16 MB flash, app0 slot is 2.5-3 MB (not 1.88), offsets shift. Default
  ELRS TX partition has app0 @ 0x10000 but size can be 0x300000 or larger.
- **Forks** (MILELRS, Bayck custom, ZLRS ...) with non-standard partition
  offsets/sizes.
- **Radios**: LR1121 (dual-band) devices may flash the second radio's
  configuration from a different slot, but that's higher-level.

## Target matrix (22 devices — see convo 2026-04-22 for full list)

Three flash layouts to support:

### Layout A — ESP32-C3 RX, 4 MB, dual-slot vanilla ELRS (current default)
- boot 0x0..0x8000, pt 0x8000, nvs 0x9000..0xe000, otadata 0xe000..0x10000,
- app0 0x10000..0x1f0000 (1.88 MB), app1 0x1f0000..0x3d0000 (1.88 MB),
- spiffs 0x3d0000..0x3f0000, coredump 0x3f0000..0x400000.
- Devices: BetaFPV SuperD, Matek R24-S/R900-S, Radiomaster RP1/RP2,
  Jumper AION, Axisflying Thor, BayckRC C3 Dual, Flywoo EL24E, Happymodel ES24TX RX.

### Layout B — ESP32-S3 TX, 8-16 MB, dual-slot (scaled-up)
- boot/pt/nvs/otadata at same small offsets,
- app0/app1 slots ~2.5-3 MB each,
- SPIFFS much larger, coredump present.
- Devices: Radiomaster Ranger / Bandit, BetaFPV Nano TX v2 / Micro TX,
  Happymodel ES24TX Slim Pro, Jumper AION Nano TX.
- Action: **read partition table live** rather than hardcoding.

### Layout C — ESP8266 RX, 4 MB, single-sketch (no OTA)
- sketch at 0x0 (~1 MB), LittleFS/SPIFFS at 0x100000+, no OTADATA, no dual slot.
- Devices: Happymodel EP1/EP2, BetaFPV Lite RX, Matek ELRS-R24-P (old rev).
- Action: Flash target dropdown should show only "Full image" (the standard
  ESP8266 ELRS flash path), hide app0/app1 options entirely.

## Plan (split into discrete commits)

### Phase M1 — Read-before-offer
- Extend `/api/elrs/receiver_info` (already scans both slots) to also return:
  - total flash size
  - list of partitions with name/type/subtype/offset/size
  - chip family (ESP8266 / ESP32-C3 / ESP32-S3)
- Cache last-seen in a shared JS object `window._rxProfile`.
- Invalidate on exit DFU, re-probe, flash, etc.

### Phase M2 — Dynamic Flash Firmware UI
- `#fwTarget` select is rebuilt dynamically from `_rxProfile`:
  - If only 1 app slot (ESP8266) → offer "Full image only".
  - If 2 app slots (C3/S3 dual) → offer `app0 @ <offset>` / `app1 @ <offset>` / `full @ 0x0`.
  - Include real slot size in the label: `app0 @ 0x10000 (1.88 MB)`.
- `fwPathUpdate()` already checks mode; add profile check to disable targets
  whose slot doesn't exist.

### Phase M3 — Dynamic Dump UI (user feedback 2026-04-22)
**Current:** fixed dropdown offsets/sizes; user has to know the layout.

**Goal:** after probe, **recommend what to dump** based on detected device:
- Profile known → show "presets" row:
  - `[Active app only]` — just the running partition (~1.88 MB on C3,
    ~2.5-3 MB on S3 TX). ~2 min. Enough for firmware version / UID /
    WiFi creds if in ELRSOPTS.
  - `[NVS + active app]` — ~24 KB + app. ~2 min. The common "full
    identity + config" dump.
  - `[Full flash]` — whole 4/8/16 MB. ~5-15 min. For deep analysis
    or clone-to-same-target backup.
  - `[Custom offset/size]` — current behaviour, expert mode.
- Each preset shows estimated time + what it'll reveal.
- Profile unknown (never probed) → fall back to current "full 4 MB" default
  but show "Probe first to get size-aware presets" hint.
- `#dumpOffset` → dropdown instead of text input: `0x0 (full)`, `0x10000 (app0)`,
  `0x1F0000 (app1)`, `0x9000 (NVS)`.
- On "active app only" → pulls offset+size from `_rxProfile`.

### Phase M4 — Slot-targeted flash in Advanced
- Same slot offsets as dynamic Flash Firmware.
- Label changes based on profile.

### Phase M5 — ESP8266 path
- Requires separate flasher codepath — ESP8266 ROM DFU has slightly
  different SLIP protocol + no stub (we'd need its own stub binary).
- `esp_rom_flasher.cpp` currently assumes ESP32 family; need to detect chip
  from `chip_info` (already returns family) and route to ESP8266 path.
- **Out of scope for M1-M4**; flag as "ESP8266 RX not yet supported" in UI
  if detected.

## Identity-card follow-ons from same research

Current fast-read hardcodes:
- NVS @ 0x9000, size 0x5000
- OTADATA @ 0xe000, size 0x2000
- app0 end = 0x1f0000, app1 end = 0x3d0000

These are Layout A only. Need to:
- Read partition table first (already done by rxScan), resolve actual NVS + OTADATA + app offsets.
- Make `/api/elrs/identity/fast` accept partition offsets as form params,
  or derive them from a just-taken partition-table snapshot.

## Ordering

M1 first (single endpoint change, no UI break). Then M2 (flash UI) + M3 (dump UI)
can go in parallel. M4 cosmetic. M5 = own mini-project.
