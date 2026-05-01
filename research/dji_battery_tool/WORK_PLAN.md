# DJI Battery Service tool — work plan (post-v0.35.1)

Status as of 2026-05-01: identification works on all 6 packs; 2/6 (PTL
2024 + 2025) unseal cleanly; 4/6 (PTL 2021 batch) blocked on unknown keys.

## Priority A — high-impact, low-risk (do these first)

### A1. HMAC-SHA1 unseal trial on PTL 2021 packs
**Status:** pending
**Why:** if this works, it unlocks 4 of our 6 packs at once.
**How:** `/api/batt/mavic3/unseal_hmac?key=<64-hex-32-byte>` exists from v0.35.0.
TI BQ40Z80 default 32-byte HMAC key derives from the published 16-byte
constant `0123456789ABCDEFFEDCBA9876543210`. Common ways to expand to
32 bytes:
- key32 = key16 + key16 (concat repeat)        ← try first
- key32 = key16 + reverse(key16)
- key32 = key16 padded with zeros
- key32 = key16 padded with 0xFF

If TI default fails, try public DJI keys mined from gvnt's
mavic-air-battery-helper / DJI Battery Killer source.

**Hardware:** any PTL 2021 pack (#1, #3, #4, #5).
**Risk:** low — failed HMAC just NACKs; 4-attempts/60s rate limit
prevents BMS lockout.

### A2. Extract 1.3 MB embedded RGB blob as PNG sequence
**Status:** pending
**Why:** see commercial tool's boot-splash + possibly UI mockups.
**How:** Python script in `research/dji_battery_tool/`:
- input: `post_image_blob.bin` (1,306,624 bytes, RGB888)
- output: 3× PNGs at 480×320 (= 460,800 B/img → 2.84 images), or auto-
  detect resolution by trying common DJI display sizes
**Hardware:** none.
**Risk:** none.

### A3. Decode the 4 mystery DataFlash setup packets
**Status:** pending
**Why:** these are DJI-custom DataFlash subcommands sent during the PF
clear ritual. Knowing what they do tells us what registers DJI cares
about modifying during Permanent Failure recovery.
**How:** for each of `4B 43 14`, `47 43 B4 00`, `46 43 03`, `71 49 07`,
match against TI BQ40Z80 reference manual Manufacturer Block Access
table (sluub69.pdf). Specifically:
- 0x4B43 = 19267 dec — maybe subclass select?
- 0x4347 = 17223 — same range
- 0x4346 = 17222 — same
- 0x4971 = 18801 — in 0x4900-0x49FF range = ?
**Hardware:** none.
**Risk:** none.

## Priority B — medium effort, useful

### B1. Stage 2 on PTL 2024 pack #2
**Why:** verify `clear_pf` + `reset_cycles` work the same way as on
PTL 2025 (#6). Compare opStatus transitions, snapshot blackout duration.
**Hardware:** Battery #2.
**Risk:** low — pack already proven to unseal.

### B2. Cell balancing trial on PTL 2021 #1 (in sealed state)
**Why:** #1 has 47 mV spread — biggest gap. Some TI chips accept
balance command without unseal. If MAC 0x002A + cellMask works while
sealed, we get balancing capability for the locked PTL 2021 packs
(can't unseal them but can rebalance).
**How:** Direct `/api/smbus/xact?op=writeBlock&reg=0x44&data=0x2A,0x00,0x05`
(balance cells 1+3 = mask 0x05). Snapshot every 30 s for 5 minutes,
watch cell voltage spread.
**Hardware:** Battery #1 (sealed).
**Risk:** low — bad write at most NACK, no destructive effect.

### B3. Calibration trigger trial on PTL 2021 #1
**Why:** #1 has 35-point SOC gap (display 69% / abs 34%) — typical
unlearned-pack signature. If MAC 0x0021 LearnCycle works in sealed
state, we can fix this without unseal.
**How:** Direct `/api/smbus/xact?op=macCmd&data=0x0021`. Then user
performs full charge → rest → discharge → rest → charge cycle.
**Hardware:** Battery #1.
**Risk:** low (write only) but NEEDS multi-hour user effort to verify.

## Priority C — high value but high effort

### C1. Cyrillic UI string extraction → English translation table
**Why:** if we want to localize our SC01 UI, we have a ready-made
domain-specific glossary in DROM.
**How:** Python script: extract Cyrillic strings from DROM, output as
JSON `{"original": "...", "translation": "..."}` template.
**Effort:** ~1 hour script + manual translation pass.

### C2. Re-skin our SC01 UI with commercial-tool BMP assets
**Why:** PTL/Glitchtronics' UI looks polished; their 53 BMPs are visually
consistent. Pixel-perfect clone of their UI on our hardware = professional
look without design work.
**How:** copy `hardware/_dji_battery_tool_extract/All Files Placed on SD/`
content to our SC01's SD root, port screen_battery.cpp to TFT_eSPI direct
(instead of LVGL) so BMP-based UI renders.
**Effort:** ~1-2 days. NB: replaces our LVGL-based UI on SC01 — major
arch decision.

### C3. Sparka mode reverse + port
**Why:** "контактный пробой" — useful for users with worn battery contacts
(common on aged Mavic 3 packs).
**How:** disasm function 0x420087E4 (~150 B) in `disasm_irom.asm`. Likely
involves PRES pin GPIO + ADC read, software arc detection.
**Effort:** 2-4 hours reverse + port.

### C4. Decode Patch.bin container format precisely
**Why:** unblocks `flashFirmwareFromBuffer()` for production use.
**How:** disasm full function 0x42008BB0 (1358 B) line-by-line, trace
how it consumes the file structure. Cross-check with BQ40Z80 ROM
bootloader reference (TI sluua64).
**Effort:** 1-2 days; requires logic-analyzer to verify on real pack.

## Priority D — optional / experimental

### D1. Find PTL 2021 keys via Russian Telegram forum mining
**Why:** can't unseal #1, #3, #4, #5 without this.
**How:** search public Telegram channels for "Mavic 3 service", "PTL
unseal", "ключи разблокировки батареи". Time-intensive, mostly Russian
language.
**Effort:** open-ended.

### D2. Logic analyzer trace of commercial tool talking to a real pack
**Why:** ground-truth for Patch.bin format + ROM mode protocol +
PTL 2021 unseal sequence.
**How:** flash commercial firmware to a spare SC01, connect Saleae /
DSLogic Plus, capture I2C traffic during each operation.
**Effort:** half-day setup, instant truth on every protocol question.

## Currently in progress
none

## Done (v0.35.1 closed)
- All 6 batteries identified with unique djiSerial + fingerprint
- PTL 2024 (#2) and PTL 2025 (#6) unseal verified
- 7 firmware fixes (#19, #28-36) shipped
- 18 new snapshot fields + 3 HTTP endpoints
- Stage 2 destructive ops verified safe on #6

## Next actions queue (going to execute in this order)
1. **A1 HMAC unseal trial** — needs PTL 2021 pack connected
2. **A2 RGB blob → PNG extraction** — paper work, no hardware
3. **A3 4 mystery packets decode** — paper work, no hardware
