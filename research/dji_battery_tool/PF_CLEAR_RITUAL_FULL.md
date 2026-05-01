# PF clear ritual — full byte-level reconstruction (function 0x42002670)

Decoded from commercial Russian DJI Battery Service Tool firmware,
2026-05-01. This supersedes the partial reconstruction in
COMPLETE_FEATURE_SPEC.md / REPRODUCIBLE_ALGORITHMS.md.

## Two-path structure

The PF clear function has a MAIN path (always run) and a FALLBACK
path (run only if PF didn't clear after main).

```
   ┌─────────────────────────────┐
   │     MAIN PATH (always)      │
   │  14 packets: unseal x2, FAS │
   │  x2, repeat, PFEnable, 0x7F │
   │  write, auth-bypass, MA-rdb │
   │  MAC 0x0030, LifetimeData   │
   └────────────┬────────────────┘
                │
                ▼
       verify_pf_cleared()
        (helper at 0x42001b40,
         arg 83 = something
         related to PFStatus
         verification)
                │
        ┌───────┴───────┐
        ▼               ▼
    success          failure
        │               │
        ▼               ▼
   "PF cleared      "PF clear
    successfully"   failed, showing
                    diagnostics"
        │               │
        ▼               ▼
       done       FALLBACK PATH
                  (5 more packets,
                   sent in REVERSE
                   order)
                       │
                       ▼
                      done
```

## Main path (14 packets, in sent order)

Each packet is a block-write to BlockData register 0x44, length-byte +
payload follows. Delays are between successive packets.

| # | Wire bytes | MAC subcommand | Purpose | Delay after |
|---|---|---|---|---|
| 1 | `44 02 E0 7E` | 0x7EE0 | Unseal step 1 | 500 ms |
| 2 | `44 02 DF CC` | 0xCCDF | Unseal step 2 | 500 ms |
| 3 | `44 02 17 BF` | 0xBF17 | FAS step 1 | 500 ms |
| 4 | `44 02 BC E0` | 0xE0BC | FAS step 2 | 500 ms |
| 5-8 | (repeat 1-4) | -- | redundancy for flaky links | 500 ms each |
| 9 | `44 02 29 00` | 0x0029 | PFEnable / PF data reset (TI standard) | 50 ms |
| 10 | (raw byte 0x7F at reg 0x7F, no length prefix) | -- | DJI-undocumented "tickle" | 50 ms |
| 11 | `44 06 62 40 01 23 45 67` | 0x4062 + magic 0x67452301 | DJI auth-bypass | 50 ms |
| 12 | `44 02 00 00` | 0x0000 | MA readback (clear pending command) | 50 ms |
| 13 | `44 02 30 00` | 0x0030 | (Seal on PTL 2024, no-op on PTL 2025) | 50 ms |
| 14 | `44 06 60 00 00 00 00 00` | 0x0060 + 4-byte zero | LifetimeData reset (TI standard) | 50 ms |

Total runtime ~5 seconds (8 × 500 ms + 6 × 50 ms).

## Verify step

```
movi a10, 83                  ; arg = 0x53 (PFStatus reg, or some related ID)
call 0x42001b40               ; verify-pf-cleared helper
beqz a10, 0x42002769          ; if helper returns 0 -> failure -> fallback
                              ; if non-zero -> success -> log + return
```

The helper at `0x42001B40` reads back state from the BMS (likely PFStatus
register or a derived "is_clean" flag). If the read indicates PF is
still latched, the helper returns 0 and we enter the fallback path.

## Fallback path (5 packets, in REVERSE address order from DRAM table)

If main path didn't clear PF, the firmware sends 5 additional packets
that perform deeper-magic DataFlash modifications. These packets are
DJI-proprietary and not in TI's published MAC subcommand table.

| # | DRAM addr | Wire bytes | MAC | Inferred purpose |
|---|---|---|---|---|
| F0 | 0x3FC94231 | `44 02 21 00` | **0x0021** | LearnCycle / DeviceName trigger -- start IT learning |
| F1 | 0x3FC94210 | `44 03 71 49 07` | **0x4971 + arg 0x07** | DJI custom DataFlash setup (subclass 0x49, offset 0x71, value 0x07?) |
| F2 | 0x3FC9420B | `44 03 46 43 03` | **0x4346 + arg 0x03** | DJI custom (subclass 0x43, offset 0x46, value 0x03?) |
| F3 | 0x3FC94205 | `44 04 47 43 B4 00` | **0x4347 + arg 0x00B4** | DJI custom (subclass 0x43, offset 0x47, 16-bit value 0x00B4?) |
| F4 | 0x3FC94200 | `44 03 4B 43 14` | **0x434B + arg 0x14** | DJI custom (subclass 0x43, offset 0x4B, value 0x14?) |

Delay: 200 ms between each.

The 0x43xx subcommands cluster suggests **DJI custom subclass 0x43 in
DataFlash** — possibly a "service work registers" area where:
- offset 0x46 stores a 1-byte counter/flag (cleared to 0x03)
- offset 0x47 stores a 2-byte field (set to 0x00B4 = 180)
- offset 0x4B stores a 1-byte field (set to 0x14 = 20)

The 0x4971 might be a different subclass (0x49) or a "commit/checksum"
operation.

After the 5 fallback packets, the function writes `status=2` (success
marker) regardless — meaning the BMS is left in whatever state it ended
up in. UI shows the user "PF clear failed, showing diagnostics" but
the operation continues internally.

## Why this matters for our port

Our `clearPFProper()` currently does ONLY the main path (14 packets).
On stubborn PF flags it would log "incomplete" without trying the
fallback. To match commercial-tool behavior:

```cpp
bool DJIBattery::clearPFProper() {
    // ... main path (14 packets, already implemented) ...

    // Verify
    if (verifyPfCleared()) return true;

    // Fallback path
    SMBus::macCommand(BATT_ADDR, 0x0021);  // F0: LearnCycle
    delay(200);
    sendDjiCustomMac(0x4971, 0x07);        // F1
    delay(200);
    sendDjiCustomMac(0x4346, 0x03);        // F2
    delay(200);
    sendDjiCustomMac16(0x4347, 0x00B4);    // F3 (16-bit arg)
    delay(200);
    sendDjiCustomMac(0x434B, 0x14);        // F4
    delay(200);

    return verifyPfCleared();  // best effort
}
```

The fallback can stay disabled by default for safety -- it writes to
DJI-proprietary DataFlash subclasses we don't fully understand. Enable
only when the user opts in via UI ("Try aggressive PF clear"
checkbox).

## Risks of the fallback packets

- They write to DataFlash → values persist across reboots
- We don't know what subclasses 0x43 / 0x49 contain, so a wrong write
  could affect: cell-balance thresholds, OCV table, ChemID, or other
  calibration data
- BUT: the commercial tool sends these EXACT bytes successfully, so on
  PTL packs they're at least non-bricking
- On genuine DJI packs (which we don't have), they may not work or may
  break things

Recommendation: implement, but gate behind a config flag. Default
behavior = main path only. Aggressive mode = enable fallback for
field-tested PTL clones.
