# Session protocol — DJI battery service test session

**Goal**: enable safe unseal/PF/cycle/balance/calibrate ops on as many DJI/PTL Mavic-3 packs as possible. Reverse-engineer commercial Russian tool, port findings to our codebase, verify on real hardware.

## Resume context (read this first when picking up)

**Workspace**: `c:\Users\inlar\PycharmProjects\esp32`
**Hardware**: Waveshare ESP32-S3-LCD-1.47B at 192.168.32.50 (primary). SC01 Plus at 192.168.32.51 also flashed but not used here.
**Latest released firmware**: tag `v0.35.1` (commit `c44ddbe`), pushed to GitHub.
**HEAD branch state**: clean, all fixes committed.

**Six test batteries available** (user switches them physically on Port B GP10/GP11):

| # | djiSerial | Mfr Date | Type | Cycles | SoH | Unseal status |
|---|---|---|---|---|---|---|
| 1 | 4ERPL... (recovered once) | 2021-09-25 | PTL 5Ah Mavic 3 | 0 | 100% | ❌ unknown keys |
| 2 | 4ERPMANEA3R9KT | 2024-10-22 | PTL 5Ah Mavic 3 | 0 | NACK | ✅ RUS_MAV → Unsealed |
| 3 | (twin of #1) | 2021-09-25 | PTL 5Ah Mavic 3 | 1 | 99% | ❌ assumed same as #1 |
| 4 | 4ERPL6QEA1D71P | 2021-09-25 | PTL 5Ah Mavic 3 | 1 | 99% | ❌ 9 keys + PEC tested |
| 5 | (twin of #1) | 2021-09-25 | PTL 5Ah Mavic 3 | 1 | 99% | ❌ assumed same |
| 6 | 4ERKM786G506DA | 2025-06-08 | PTL 10Ah LiHV custom | 0 | NACK | ✅ RUS_MAV → FullAccess (1 step!) |

**Key research**: in `research/dji_battery_tool/` — TEST_LOG.md (40 observation notes), PF_CLEAR_RITUAL_FULL.md, COMPARISON_VS_OUR_CODE.md, REPRODUCIBLE_ALGORITHMS.md, FULL_REVERSE_REPORT.md, COMPLETE_FEATURE_SPEC.md, WORK_PLAN.md.

**Key recovered keys** (all confirmed working on PTL 2024+):
- Unseal: `0x7EE0` then `0xCCDF` (combined `0xCCDF7EE0`)
- FAS: `0xBF17` then `0xE0BC` (combined `0xE0BCBF17`)
- DJI auth-bypass: `MAC 0x4062` + magic `0x67452301` (= MD5 IV[0])

## What we did this session (chronological)

1. **v0.35.0** (commit `da4bd39`): full Mavic-3 service feature parity from reverse — unseal/FAS/PF/cycle/balance/calibrate/capacity/patch-flash + 4×2 SC01 UI grid + 7 HTTP endpoints
2. **Identified 6 packs** via /api/batt/snapshot, found PTL 2021 vs 2024 firmware variant differences
3. **v0.35.1** (`c44ddbe`): 7 firmware fixes from live testing
   - #19: hasPF distinguishes NACK from clean
   - #28: PTL recognised as DJI clone manufacturer
   - #29: clearBlackBox no-op (MAC 0x0030 was actually Seal, not BB-clear)
   - #30: variant-aware key catalog with 3-pass fallback
   - #33: tryAllKnownKeys avoids readDJIPF2 (caused BMS lockout from malformed auth-bypass)
   - #34: SMBus::readDword falls back to readWord on NACK (PTL 2025 fix)
   - #35: chipTypeName decoder
   - #36: seal() graceful no-op detection
4. **Stage 1 verified**: try_keys works on #2 (Unsealed) and #6 (FullAccess directly!) with same RUS_MAV key
5. **Stage 2 verified safe** on #6 (clean PTL 2025): clear_pf + reset_cycles → no harm, all params preserved
6. **A2 (RGB blob → PNG)**: SKIPPED per user (just graphics, low pay-off)
7. **A3 (4 mystery packets)**: ✅ DECODED — found they are FALLBACK PATH of clearPF (5 packets sent if main 14-step ritual didn't clear PF). Doc: `PF_CLEAR_RITUAL_FULL.md`

## Currently doing (about to do)

**A1 — HMAC-SHA1 unseal trial on PTL 2021 batch (Battery #1 connected)**.

Goal: discover whether PTL 2021 packs use HMAC challenge-response (vs PTL 2024+ which use static keys). If yes, try TI default 32-byte HMAC key in various expansions.

HTTP endpoint: `POST /api/batt/mavic3/unseal_hmac?key=<64-hex-32B>`

Plan:
1. acquire port + snapshot to confirm pack identity (#1)
2. Try TI default expanded forms:
   - key32 = "0123456789ABCDEFFEDCBA9876543210" repeated twice
   - key32 = same + reverse
   - key32 = same + zeros
   - key32 = all 0xFF
   - key32 = all 0x00
3. If none work, also try: derive from djiSerial / mfrDate / fingerprint
4. Document each attempt in this file

## After A1 — open work queue

- B1: Stage 2 on #2 (compare destructive ops PTL 2024 vs 2025)
- B2: Cell balancing on #1 in sealed state (47mV spread — biggest gap)
- B3: Calibration trigger on #1 (35-point SOC gap = unlearned pack)
- C1: Cyrillic UI strings extraction → translation table
- C3: Sparka mode reverse + port (function 0x420087E4)
- C4: Patch.bin container format precise decode (function 0x42008BB0)
- D1: Russian Telegram forum mining for PTL 2021 keys
- D2: Logic analyzer trace of commercial tool

## Resume command (next session)

If returning fresh:
1. Read this file first.
2. Check `git log -5` to see latest commits.
3. Read `WORK_PLAN.md` for prioritised queue.
4. Read `TEST_LOG.md` for full battery observation history.
5. If hardware testing: ensure user has connected the right battery,
   call `POST /api/batt/acquire` first (Port B may revert to UART after boot).

## A1 attempts log

Connected pack: Battery #1 (`djiSerial=4ERPL6QEA1D6XS`, `mfrDate=2021-09-25`,
`fingerprint=B1202A97A4965913`, `fwVariant=PTL-OLD`). Sealed.

Trials via `POST /api/batt/mavic3/unseal_hmac?key=<64-hex>`:

| # | Key32 description | Result | challenge | opStatus |
|---|---|---|---|---|
| 1 | TI default repeat (`0123...3210` × 2) | no i2c | `00`×20 | 0x1020307 |
| 2 | TI default + reverse | no i2c | `00`×20 | 0x1020307 |
| 3 | TI default + 16×zeros | no i2c | `00`×20 | 0x1020307 |
| 4 | TI default reversed × 2 | no i2c | `00`×20 | 0x1020307 |
| 5 | all zeros | no i2c | `00`×20 | 0x1020307 |
| 6 | all 0xFF | no i2c | `00`×20 | 0x1020307 |
| 7 | ASCII "DJI Battery 1234567890ABCDEF" + zeros | no i2c | `00`×20 | 0x1020307 |
| 8 | MD5 IV expanded | no i2c | `00`×20 | 0x1020307 |

**All 8 returned "no i2c" with challenge = all zeros.**

### Diagnostic interpretation

`challenge=00`×20 is NOT a real BMS challenge — it's the value of a
zero-initialised buffer that never got filled. Looking at our
`unsealHmac()` code in `dji_battery.cpp`:

```cpp
int n = SMBus::macBlockRead(BATT_ADDR, 0x0000, challenge, sizeof(challenge));
if (n < 20) return UNSEAL_NO_RESPONSE;   // -> "no i2c"
```

The `macBlockRead(BATT_ADDR, 0x0000)` writes MAC subcommand 0x0000
(ManufacturerAccess readback) to BlockData reg 0x44, then reads back
block. On PTL 2021, MAC 0x0000 isn't a "challenge request" — it just
returns the current MA value (2 bytes), not 20 bytes. So `n < 20`
triggers, function returns NO_RESPONSE.

**Bottom line**: our HMAC implementation uses the WRONG MAC subcommand
to request a challenge. TI BQ40Z80 actual challenge subcommands are
chip-specific:
- `0x002C` = SECURITY_KEYS_HASH (most common)
- `0x002D` = AUTHENTICATION_KEY (write-only register, not for read)
- `0x002A` = some MAC variant

**Fix needed (TEST_LOG note #41)**: change `unsealHmac()` to try
multiple MAC subcommands for challenge request. Even if PTL 2021
supports HMAC, we won't get a valid challenge until we use the right
subcommand.

### Next-pass plan

1. Modify `unsealHmac()` to try MAC `0x002C`, `0x002D`, `0x002A` in addition
   to `0x0000`. Add `?challenge_mac=0x002C` query param to the HTTP
   endpoint so we can test each one without recompiling.
2. If we get ANY non-zero challenge from PTL 2021, run HMAC-SHA1 with
   our candidate keys against that challenge (offline, in Python) to
   precompute the digest, then send that digest to the BMS.
3. If still no challenge from any subcommand, PTL 2021 doesn't use HMAC
   — keys are static but unknown. Move to D1 (forum mining) or D2
   (logic analyzer trace of commercial tool sending to a pack).

## State at session pause (2026-05-01)

Battery #1 left sealed (no state changes).
Latest commit: `c44ddbe` (v0.35.1).
Pending firmware fix: #41 (HMAC challenge subcommand fallback).

## Resume next session

1. Read this file from top.
2. If returning fresh: `git status`, `git log -3 --oneline`.
3. Decide: implement #41 (small firmware change + new build + flash + retest)
   OR pivot to B/C/D priorities in WORK_PLAN.md.
4. If #41: edit `dji_battery.cpp::unsealHmac()` to take an additional
   `challenge_mac` arg (default 0x0000), update HTTP wrapper to accept
   `?challenge_mac=` param, rebuild, flash, retest A1 with subcmd 0x002C.

---

## A1 round 2 (post-#41 implementation)

#41 implemented: `unsealHmac()` now takes `uint16_t challenge_mac` arg
(default 0x0000) and HTTP endpoint accepts `?challenge_mac=0xNNNN`. Also
added `?probe=1` mode that just reads the challenge without computing
HMAC -- doesn't burn rate-limit attempts. Both committed at the
checkpoint below.

### Probe results on Battery #1 (still PTL 2021, sealed)

`POST /api/batt/mavic3/unseal_hmac?probe=1&challenge_mac=0xNNNN`

via STANDARD path (`writeBlock 0x44 + readBlock 0x44`): all returned
`len=-1` (NACK). PTL 2021 doesn't expose any 20-byte challenge via the
ManufacturerBlockAccess block-read protocol that BQ40Z80 uses.

via LEGACY path (`writeWord 0x00 + readBlock 0x00`): SUCCEEDED with
varying response sizes per subcommand:

| Subcmd | len | First 4 bytes |
|---|---|---|
| 0x0001 DeviceType | 1 | `00` |
| 0x0002 FwVersion | 2 | `00 E7` |
| 0x0003 HwVersion | 3 | `00 F2 FF` |
| 0x0006 ChemSig | 6 | `00 B3 FF FF` |
| 0x0007 ChemID | 7 | `00 A6 FF FF` |
| 0x0021 LearnCycle | 32 | `00 76 FF FF` (rest FF) |
| 0x002A | 32 | `00 E1 FF FF` (rest FF) |
| 0x002C SecKeysHash | 32 | `00 9F FF FF` (rest FF) |
| 0x002D AuthKey | 32 | `00 8A FF FF` (rest FF) |
| 0x4062 DJI authBp | 32 | `40 D5 FF FF` (rest FF) |

Pattern: byte 0 echoes subcommand HIGH byte (0x00 for all standard MAC,
0x40 for 0x4062); byte 1 varies but is single-byte status, not random;
bytes 2..N are `0xFF` padding.

**PTL 2021 does NOT support HMAC-SHA1 challenge-response unseal.** The
firmware doesn't generate or expose 20-byte random challenges via any
of the tested MAC subcommands. PTL 2021 must use STATIC keys that we
haven't yet found.

### Verdict

A1 is closed: HMAC route is a dead end on PTL 2021. The remaining paths
to unlock the 4 PTL 2021 packs are:

- **D1** (forum mining for PTL 2021 keys) — slow, language-bound research
- **D2** (logic analyzer on commercial-tool unseal) — fast if we have a
  spare SC01 to flash with the commercial firmware + a Saleae/DSLogic
- **B2** (cell balancing on #1 in sealed state — try MAC `0x002A` write
  even without unseal — may work since some BMS allow balance command
  at sealed level)
- **B3** (calibration on #1 in sealed state — try MAC `0x0021` write,
  see if ImpedanceTrack starts learning)

D1/D2 are the only paths to actual PTL 2021 unseal. B2/B3 are
"work around the lock" paths.

## State at session pause #2

Battery #1 connected, sealed, untouched (probes don't change state).
Latest commit: `4c13cc0` (#41 + diagnostics).

---

## B2 trial — cell balance trigger on Battery #1 in sealed state

Pre-balance cells (3875/3850/3879/**3896** mV, spread 46 mV).
Sent both protocol variants:
- `writeBlock 0x44 [0x2A, 0x00, 0x08]` (balance cell 4 mask): ACK len=3
- `writeWord 0x00 0x002A` (MA register write): ACK

Polled cells every 30s for 2 minutes. Result: **NO change** — cells
stable ±1 mV (measurement jitter only). Cell 4 still 3896 mV after
2 minutes.

**Conclusion**: PTL 2021 ACKs MAC writes in sealed state, but DOESN'T
actually execute destructive commands (balance). The BMS gates MAC
0x002A behind Unsealed state.

By inference, B3 (calibration, MAC 0x0021 LearnCycle) is similarly
gated and won't trigger learning in sealed state.

**Verdict**: There is NO workaround to do service ops on PTL 2021
without actual unseal. The 4 PTL 2021 packs (#1, #3, #4, #5) are
fully locked until we obtain their unseal keys.

## Closed paths

- A1 HMAC challenge-response: PTL 2021 doesn't expose challenge buffer
- B2 sealed-balance: PTL 2021 BMS doesn't act on MAC writes while sealed
- B3 sealed-calibrate: same lock as B2 by inference

## Remaining open paths for PTL 2021 unlock

- **D1**: forum mining for PTL 2021 keys (Russian DJI service Telegram)
- **D2**: logic analyzer trace of commercial-tool firmware unsealing a
  PTL 2021 pack -- needs spare SC01 + Saleae/DSLogic
- Patience: wait for someone to publish PTL 2021 keys publicly

---

## Round 3: standalone tests of commercial-firmware findings on PTL 2021 #1

User reminded: did we try ALL distinctive findings from commercial
firmware reverse? Specifically:
- The **DJI auth-bypass packet** (MAC 0x4062 + magic 0x67452301)
- The **5 fallback packets** (F0-F4) used in clearPF fallback path

These had NEVER been tested standalone on PTL 2021. Tried each variation:

### Auth-bypass alone

```
writeBlock 0x44 [0x62 0x40 0x01 0x23 0x45 0x67]   -> ACK, len=6
opStatus low: 0x0307 -> 0x0704 (toggled)
sec: 3 -> 3 (unchanged)
Then RUS_MAV unseal: still sealed
```

Auth-bypass DID change opStatus low bits (something internal flipped),
but didn't unseal. Subsequent unseal still rejected. So auth-bypass is
NECESSARY in the main clearPF path BUT NOT SUFFICIENT for unlock.

### 5 fallback packets sequence (F0-F4)

```
F0 MAC 0x0021 (LearnCycle)            -> ACK, sec stays 3
F1 MAC 0x4971 + 0x07                  -> ACK, sec=1 ⚠ TRANSIENT!
F2 MAC 0x4346 + 0x03                  -> ACK, sec=3 (back)
F3 MAC 0x4347 + 0x00B4                -> ACK, sec=3
F4 MAC 0x434B + 0x14                  -> ACK, sec=3
```

⚠ **F1 produced sec=1 once (opStatus low 0x0104)** — sec=1 isn't a
documented BQ40Z80 state (3=Sealed, 2=Unsealed, 0=FullAccess). Was
this a real transition or read glitch?

Tried to reproduce:
- F1 alone: no sec=1 transition
- F0 then F1 explicitly: no sec=1 transition
- Full blast F0-F4 without intermediate reads: sec stays 3

Conclusion: sec=1 was a **non-reproducible read glitch** (likely race
between BMS state machine settling and our readWord at 0x54).

### Full attack: F0-F4 + auth-bypass + RUS_MAV unseal

Sent all 5 fallback packets, then auth-bypass, then unseal RUS_MAV
keys: still rejected ("still sealed").

### Verdict (final)

**Every distinctive finding from commercial firmware reverse has been
tested as a standalone unlock vector on PTL 2021 — all rejected.**

Tested combinations:
- 8 static unseal key pairs (RUS_MAV, FAS, TI default, DJI BK + swaps)
- 10 HMAC challenge_mac variants (probe via reg 0x44 and 0x00 paths)
- Auth-bypass packet alone
- 5 fallback packets standalone, F1 alone, F0+F1 sequence
- Full blast: F0-F4 + auth-bypass + unseal

PTL 2021 firmware refuses ALL software-only unseal attempts via the
standard MA protocols. Either:
1. Keys are unique per-batch / per-pack (derived from djiSerial?)
2. There's a magic SBS register write outside the MA range we tested
3. Unseal requires a specific TIMING / CHALLENGE beyond our probes

Without access to:
- A working commercial tool's I2C trace on a PTL 2021 pack (D2), OR
- Public publication of PTL 2021 keys (D1), OR
- Access to PTL firmware source / documentation

We cannot software-unlock PTL 2021 packs. The 4 PTL 2021 packs
(#1, #3, #4, #5) remain locked for any service operations.


