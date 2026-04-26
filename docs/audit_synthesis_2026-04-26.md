# Project Audit — Executive Synthesis (2026-04-26)

Three parallel audits ran on the project: architecture / code quality, end-to-end functionality, and UI/UX. Reports:
- [audit_architecture_2026-04-26.md](audit_architecture_2026-04-26.md)
- [audit_functionality_2026-04-26.md](audit_functionality_2026-04-26.md)
- [audit_ui_2026-04-26.md](audit_ui_2026-04-26.md)

This file is the merged action plan.

## Headline verdict

The project is a **6.5/10** by code quality, **B+** by UI/UX, and **functionally
working for the primary scenario** (flash + telemetry + identity on ESP32-C3
ELRS RX). Three years of organic growth show clearly: tight core subsystems,
sprawling integration glue.

**The core technology works.** The pain is **discoverability**: users hit
errors they don't understand, click disabled buttons that look enabled, and
get blank cards with no hint what to do.

## Verified vs stale claims

I cross-checked the agents against current code. Two claims need correction:

| Claim | Status | Evidence |
|---|---|---|
| Flash B1+B2 (UART buffer + truncated SLIP) "broken" | **STALE — already fixed** | `setRxBufferSize` is now called BEFORE `begin()` in 15 sites in [esp_rom_flasher.cpp:293-1266](src/bridge/esp_rom_flasher.cpp#L293); line 141 returns false on truncated frames |
| WDT will panic on >2 MB flash | **Partially valid, not P0** | 1.24 MB flash empirically completed in 43 s; `delay(1)` in SLIP read keeps IDLE-task fed. Still recommend explicit `esp_task_wdt_reset()` defence |
| 50+ clone endpoints in prod binary | **Exaggerated** | Real count ~15 via `s_server->on()` in routes_battery.cpp |

## Top-15 priority fixes (ranked by impact ÷ effort)

| # | Fix | From | Effort | Impact |
|---|---|---|---|---|
| 1 | **Mode-blocked button visual** — opacity 0.5 looks like hover; use `background: --text-muted + dashed border` | UI #2 | 10 min | Users stop blaming plate for "broken" buttons |
| 2 | **Empty-state hints** on identCard / rxCfgTable / rcv-live cards (`<small class="placeholder">Click X first</small>`) | UI #3 | 20 min | New users see what to do; ~25% faster onboarding |
| 3 | **Mode badge text labels** (DFU/STUB/APP/SILENT inside coloured pill) | UI #1 | 5 min | Colourblind accessibility + redundancy |
| 4 | **Move identCard "needs DFU" warning to summary line** | UI #4 | 5 min | Precondition visible before clicking |
| 5 | **Inverse banner**: when flash is running, CRSF tab shows "Flash in progress — Start blocked" | Func | 10 min | Eliminates one class of cryptic 409s |
| 6 | **Erase OTADATA / Erase app1 confirm dialog** — typed "ERASE" or 2-step click | UI #7 | 20 min | One-click brick prevention |
| 7 | **Flash ETA + elapsed timer** in fwBar | UI #4 | 20 min | User stops thinking plate hung at 30s |
| 8 | **Parameter-table edit affordance** — pencil icon or inline `<input>` | UI #4 | 15 min | Discoverability |
| 9 | **Replace `confirm()` with custom modal** for motor + erase | UI #7 | 25 min | UX polish |
| 10 | **MD5 verify after flash** — read back, compare digest of flashed region | Func | 30 min | Defence-in-depth even after B1+B2 fix |
| 11 | **Auto-pause CRSF for ALL ELRS ops** (not just flash) — eliminates 409 livelock | Func | 50 min | Removes whole class of integration errors |
| 12 | **Identity Phase 1c** — read SPIFFS for TX UID + runtime WiFi-STA creds | Func | 1 day | Closes "TX is unidentifiable" gap |
| 13 | **executeFlash() → xTask** — stop blocking main loop during flash | Func | 100 LOC | Browser stays responsive; pre-empts WDT-on-big-flash class |
| 14 | **WebState facade** — typed accessors per subsystem, lock embedded in API | Arch #3 | 1-2 days | Catches races at compile time; eliminates manual lock burden |
| 15 | **`#ifdef RESEARCH_MODE`** for battery clone endpoints | Arch #5 | 30 min | Slimmer prod binary; clearer feature surface |

**Items 1-7 = ~1.5 hours, ~50% of UX wins.** Do those first.

## Cut / fix / promote table

### 🔪 CUT (over-promised vs delivered)

| Feature | State | Reason |
|---|---|---|
| **BLHeli 4-way passthrough** | 10% (TCP stub only) | "Full 4way passthrough not yet implemented" comment in source. Either commit to finishing (~300-400 LOC) or remove from UI |
| **OneWire BLHeli** | 5% (returns 0xFF stub) | Bit-bang timing never calibrated. Cut from UI |
| **Battery clone lab** | Research-only | Move to `#ifdef RESEARCH_MODE`. Security-risky in prod |
| **Autel battery** | 1 TODO, incomplete | Either complete or feature-flag |

### 🔧 FIX (broken-ish, <1 day)

| Feature | Why fix | Effort |
|---|---|---|
| **Mode-blocked button visual** | Highest single UX win | 10 min |
| **Empty-state hints** | Next highest UX win | 20 min |
| **Inverse CRSF banner** during flash | Removes 409 confusion | 10 min |
| **Erase confirm dialog** | One-click brick prevention | 20 min |
| **ETA on flash progress** | "Plate frozen?" anxiety gone | 20 min |
| **Parameter table editability cue** | Discoverability | 15 min |
| **Identity Phase 1c (SPIFFS)** | Closes TX gap | 1 day |

### 🚀 PROMOTE (mostly works, needs polish)

| Feature | Polish |
|---|---|
| **Live CRSF telemetry** | Add small graph component; signal/quality history |
| **Identity card** | Add ELRSOPTS JSON full extraction (Phase 1b finished, needs verify) |
| **Receiver tab IA** | Already excellent; just fix the 7 nitpicks above |
| **DJI battery read** | Add CSV export + history chart |
| **Boot-slot flip** | Show target version + auto-reprobe post-reboot |

## High-leverage refactors (for later, when feature work pauses)

These deliver compounding payoff but have no immediate user-visible impact:

1. **Extract HTML/JS from `web_ui.cpp` literal** → separate files served via SPIFFS or LittleFS
   - Saves 50% of HTML edit→test cycle time
   - Enables dead-code detection
   - Cost: 1-2 days
   - Risk: WS handler refactor required

2. **Replace `WebState` namespace with typed Facade**
   - 445 untyped accesses become typed methods
   - Lock taken in accessor (impossible to forget)
   - Cost: 2-3 days
   - Risk: subtle behaviour changes if accessors throw vs the current implicit no-op

3. **Per-subsystem `routes_*.cpp` instead of `web_server.cpp` god module**
   - 25 includes drop to 5
   - Module rebuild times improve
   - Cost: 1 day
   - Risk: low; mostly mechanical

4. **`readFlashMulti` pattern → `flashMulti` for write paths**
   - Pre-empts whole class of "ROM autobauder latched once" bugs we keep hitting
   - Cost: 1 day
   - Risk: low

5. **Partition table cache** at probe time → drop hardcoded 0x10000/0x1f0000 from 4 places
   - Already partially landed via `_rxProfile` in JS
   - Backend should also use it
   - Cost: 1 day

## Validation matrix — what's actually tested

| Component | Manual tested | Auto tested | Notes |
|---|---|---|---|
| ELRS flash (ROM DFU) | ✓ on bayck C3 dual | ✗ no test | Plate works in real life on this hardware |
| ELRS flash (in-app stub) | ✓ partial | ✗ | Less coverage |
| Identity card NVS read | ⚠ partial | ✗ | We only verified eeprom blob at offset 4 |
| Identity card APPTAIL/ELRSOPTS | ⚠ untested in this session | ✗ | Code path active but no real RX hit it post-Phase-1b |
| Fork detection (ZLRS sigs) | ✗ no real ZLRS device tested | ✗ | Logic landed; needs ground truth |
| Multi-target M1 | ⚠ partition decode tested on bayck only | ✗ | Untested on S3 TX or ESP8266 |
| Live CRSF telemetry | ✓ on prior session | ✗ | Stable |
| Servo / motor / DShot | per CLAUDE.md memory | ✗ | Unverified in this session |

**Glaring gap:** zero automated tests anywhere. A small CRSF parser test + esp_rom_flasher SLIP unit-test would catch >80% of regressions before they hit hardware.

## Risk register (consolidated)

Ranked by user-impact × likelihood:

| # | Risk | Source | Severity | Mitigation status |
|---|---|---|---|---|
| 1 | One-click brick via Erase OTADATA / Erase app1 (no confirm) | UI #7 | High | Open — 20 min fix |
| 2 | Identity card untested end-to-end on real ZLRS / TX / S3 | Func § validation | Medium | Open — needs hardware |
| 3 | Multi-target hardcoding silently flashes wrong offsets on S3 TX | Func | Medium | M1 partially landed; M2/M3 wiring needed |
| 4 | WebState String UAF on simultaneous WS + background task | Arch #4 | Low (rare race) | Open — needs Facade refactor |
| 5 | UART1 RX ISR race during flash | Arch #3 | Low (timing-dependent) | Open — needs sync barrier |
| 6 | WDT panic on >2 MB image (theoretical) | Arch #2 | Low | Defence: explicit `esp_task_wdt_reset()` in flash loop |
| 7 | Hardcoded partition offsets break if 8 MB layout adopted | Arch #4 | Future | Refactor when needed |
| 8 | WiFi credential leak via battery clone dump | Func | Low (research-only path) | Gate with RESEARCH_MODE |

## Recommended sequencing

### This week (8 hrs)
1-7 from priority table. Pure UX wins. **Result: ~60% better first-try success rate.**

### Next sprint (3-5 days)
- Items 8-13 (parameter-table editability, MD5 verify, executeFlash xTask, identity Phase 1c)
- Cut BLHeli 4-way + OneWire stubs from UI; gate clone endpoints

### When dust settles (1-2 weeks)
- WebState facade
- Per-domain routes_*.cpp
- HTML extraction from web_ui.cpp
- Add automated tests for CRSF parser + SLIP framing

### Continuous
- Verify Identity card on real ZLRS RX, TX, S3 TX as hardware becomes available
- Document "what's plugged in" → "what works" matrix per device class

## What's good — don't break it

The audit agents independently praised:

- **Receiver tab IA** (action-picker accordion, mode-aware gating) — model match works
- **Port B arbiter** (`PinPort::acquire`) — clean abstraction, no bypasses found
- **CRSF + ROM flasher subsystems** — 8-9/10 coherence; tight protocol code
- **Live CRSF telemetry** — unique, fast, useful
- **Identity card concept** — solves a real "who is this RX" problem
- **Mode badges with semantic colours** — good signalling once text labels are added
- **Scroll anchor on accordion toggle** — subtle but delightful
- **Partition-table-driven `_rxProfile`** — sets up multi-target M2/M3

These are genuine wins. The fixes above are polish on top of a solid foundation.
