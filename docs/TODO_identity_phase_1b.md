# TODO — Identity Phase 1b + bugfixes (2026-04-22)

Captured from live testing of Phase 1a Identity card (commit e9e027b).

## BUG-ID1: Multi-read fails — "No sync" on second readFlash()

**Symptom** (live, 2026-04-22): RX in DFU, mode probe confirms
"DFU (ROM @115200) probed 8 s ago". First press of "Read identity from
device" → first `readFlash()` (NVS region) succeeds, second `readFlash()`
(OTADATA) fails with `FLASH_ERR_NO_SYNC` ("No sync — is device in
bootloader mode?").

**Root cause**:
[esp_rom_flasher.cpp:452-524](../src/bridge/esp_rom_flasher.cpp#L452)
`readFlash()` does `Serial1.begin() → sync → read → Serial1.end()` around
each call. After the first successful sync the ESP32-C3 ROM autobauder is
latched; when we `Serial1.end()` then `.begin()` again at the same baud
the second sync packet doesn't round-trip (empirically confirmed in this
session and referenced in commit 4da0230's dump-protocol research).

**Hotfix landed**: combine NVS + OTADATA (contiguous 28 KB at 0x9000) into
a single `readFlash()` call — one sync, one read. Eliminates the second
failing sync. App-tail read (8 KB from end of active slot, for ELRSOPTS
JSON WiFi-creds extraction) was DROPPED from Phase 1a for the same
reason — a third read would need another sync and fail again.

**Proper fix (Phase 1b)**:

Add a multi-region `readFlashMulti()` to esp_rom_flasher.cpp that:
1. `begin() + sync + spiAttach + spiSetParams` ONCE at start
2. Issues N READ_FLASH_SLOW commands at the caller-supplied offset/size
   tuples without tearing down Serial1 between them
3. `end()` ONCE at finish

Rough API:
```cpp
struct ReadRegion { uint32_t offset; uint32_t size; uint8_t *dst; };
Result readFlashMulti(const Config &cfg, const ReadRegion *regions, size_t n);
```

Then re-enable the app-tail read in `/api/elrs/identity/fast`.

## BUG-ID2: Status card shows "—" for Firmware/Version/Serial/LUA when in DFU

**Symptom** (live): probe succeeds and reports "DFU (ROM @115200)", but the
lines Firmware / Version / Serial / HW ID / LUA param count stay at `—`.
User perceives as broken.

**Root cause**: `rxProbeMode()` sends CRSF DEVICE_PING which only works
when RX is in app. In DFU the ROM bootloader doesn't speak CRSF, so
`d.app_ok` is false and
[web_ui.cpp:2576](../src/web/web_ui.cpp#L2576) `if (d.app_ok && d.app)` skips
the identity-field fill. Correct behaviour, bad UX: the user doesn't know
why the fields are empty.

**Fix options** (pick one for Phase 1b):

1. **Hint line** (cheap): when mode is `dfu`, set rxModeHint to
   "Firmware identity not available in DFU — use Advanced → Diagnose → Scan receiver".
2. **Auto-fill from chip_info + Scan cache**: if user already ran rxScan
   (Full DFU scan) the result has chip/MAC/both slots' target+version. Reuse
   that in the Status card fields instead of leaving `—`. Requires caching
   scan result in a shared JS object (see `_rxProfile` in
   [TODO_multi_target_adaptation.md](TODO_multi_target_adaptation.md) Phase M1).
3. **Auto-trigger Scan when probe detects DFU** (eager but expensive; adds
   ~10-15 s of flash reads on every DFU probe). Reject — too noisy.

Recommendation: **1 + 2 combined**. Always show hint; if `_rxProfile`
has a matching fresh scan, use it.

## Phase 1b concrete work list

- [ ] `esp_rom_flasher::readFlashMulti()` helper
- [ ] Wire it into `/api/elrs/identity/fast` — re-enable APPTAIL region
- [ ] Client-side: parse ELRSOPTS JSON from APPTAIL again (code already
  exists, was un-reachable after Phase 1a combine)
- [ ] Status card DFU hint (fix BUG-ID2 option 1)
- [ ] Shared `_rxProfile` cache (overlap with multi-target M1) — fill
  Status identity fields from it when DFU (BUG-ID2 option 2)

## Stretch (Phase 1c)

- Read SPIFFS partition (~128 KB) for `/options.json` file. That's where
  TX UID lives and where runtime RX WiFi STA creds live. Parse SPIFFS
  directory structure (or just grep the region for `{"uid":[`). Adds ~30 s
  to a fast-read — should be an opt-in "Thorough scan" button, not default.
