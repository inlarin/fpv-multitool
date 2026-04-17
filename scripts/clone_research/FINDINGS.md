# PTL BA01WM260 Clone Battery — Research Findings

**Device:** BA01WM260 (PTL brand fake Mavic 3 battery, 4S, sealed)
**Serial:** 4ERPL6QEA1D6Y9 (from reg 0xD8)
**Cycles:** 1
**Method:** ESP32 CP2112 HID emulator, Python client via `hidapi`
**Date:** 2026-04-17

## Summary

| Test | Result |
|------|--------|
| TI MAC (0x0000-0xFFFF, 65536 subcmds) | **0 responses** — clone doesn't implement MAC |
| SBS word writes to protected regs (0x17, 0x18, 0x1C, 0x3C-0x3F, 0xD6-0xDB) | **Rejected** — seal enforced |
| SBS vendor range 0xD0-0xFF read | **48/48 respond**, includes paired magic AA55/55AA markers |
| SBS writable vendor regs 0xD4, 0xE5, 0xEB, 0xEE | **ACCEPT WRITES without unseal** |

## Vendor register signatures

```
0xD5: 00           — status byte, always 0x01 word
0xD6: AA66FFFFFFFFFFFFFFFFFFFFFFFFFFFF  — starts with 0xAA55 word marker
0xD7: 00           — zero
0xD8: 344552504c365145413144365939  "4ERPL6QEA1D6Y9"  (DJI serial ASCII)
0xD9: E6B78FDD55AA   — ends with 0x55AA (paired backdoor marker)
0xE7: 023A...        — 2-byte data, rest FF
0xE8: 23F6...        — 2-byte data, rest FF
0xEA: 4403...
0xEE: 800400FC00A572000000000000000000   — 16-byte active register, byte 6-7 changes each read (nonce!)
0xF0: 46F50004       — 4-byte data, word=0x4604 possibly a counter
0xF2: 00B6FFFF
0xF3: 8EFF...
0xF5: 98FFFFFFFF
0xF6: 8C0C8C00       — 4 bytes, 2 words: 0x0C8C=3212, 0x008C=140
0xF7: 8074FFFFFFFFFF...
0xF8: 00D5FFFF...
0xFA: 0095FFFF...
0xFB: 00000000
0xFF: 0131
```

## Seal-bypass findings

Tested 19 standard protected registers with `writeWord(reg, 0xABCD)`:
- **All rejected**, value didn't change
- OpStatus returned `07030201` — NOT standard TI format (SEC bits don't decode cleanly)

**BUT:** testing 24 magic patterns against 48 vendor regs revealed 4 writable registers:

- **0xD4** — accepts arbitrary word writes, reads back different value (echo with high-byte mutation)
- **0xE5** — same behaviour as 0xD4
- **0xEB** — same behaviour
- **0xEE** — ACTIVE register with nonce:
  - Bytes 0-1: echoes last write (on some values only)
  - Bytes 2-3: fixed `0400`
  - Bytes 4-5: fixed `00FC` or `00FF`
  - Bytes 6-7: **changes on every read** (suspected nonce/challenge/counter)
  - Bytes 8+: zeros

## Interpretation

The clone chip:
1. **Has no TI MAC support** — entire ManufacturerAccess protocol absent
2. **Enforces seal** on standard SBS registers (cycles, capacity, cells, etc.)
3. **Has a partial vendor/debug interface** at 0xD4/0xE5/0xEB/0xEE
4. **Register 0xEE contains a live nonce** — indicates authentication challenge

Hypothesis: the 0xEE register implements a custom challenge-response scheme. Writing the correct response (based on the nonce) to 0xEE may unlock the device. The 0xAA55/0x55AA markers at 0xD6/0xD9 may be MAGIC_PRESENT markers that confirm the chip family.

## Next steps

1. **Study 0xEE behavior in depth:**
   - Read 0xEE 100 times, check nonce pattern (linear counter? random? LFSR?)
   - Write value X → read 0xEE → see if bytes change predictably
   - Try block-write to 0xEE (vs word write) — maybe need 16-byte response

2. **Replay DJI official tool traffic:**
   - Run official DJI Assistant or Battery Killer against a real Mavic 3 battery
   - Capture CP2112 log via `/api/cp2112/log`
   - Look for the sequence it uses to authenticate/unseal
   - Replay same bytes on the PTL clone

3. **Brute-force 0xEE combinations:**
   - Write pairs (reg1, reg2, reg3...) before reading 0xEE
   - Look for state changes (SEC bits, F0/F6 vendor regs)

4. **BQ9003 boot-mode entry** (separate vector):
   - Write 0x0033 to MAC, read reg 0x0D, expect 0x0502 response
   - If supported, opens flash programming interface

5. **Hardware approach:**
   - Open pack, probe JTAG/SWD pins on PCB
   - Check for UART debug port

## Files in this directory
- `cp2112.py` — HID driver (working, tested)
- `scan_sbs_full.py` — 256-reg enumerate (~5s, all 256 respond)
- `scan_mac_full.py` — 65536-subcmd brute-scan (~3.3 min, 0 hits confirmed)
- `bypass_probe.py` — write-verify on standard regs (all rejected)
- `magic_probe.py` — magic-sequence probe on vendor range (found writable regs)
- `mac_full.csv` — empty (no MAC responses)
- `FINDINGS.md` — this file

## Raw scan snippets

### Full SBS scan (high range only, register + block)
```
0xD0: 00 00
0xD1: 00 00
0xD2: 00 00
0xD3: 00 00
0xD4: 00 00
0xD5: 01 00
0xD6: 55 AA  block: AA66FFFFFFFFFFFFFFFFFFFFFFFFFFFF
0xD7: 00 00
0xD8: 0E 34  block: 344552504C365145413144365939        "4ERPL6QEA1D6Y9"
0xD9: 06 E6  block: E6B78FDD55AA
0xE7: 3A 02  block: 023AFFFF...FF (24 bytes)
0xE8: F6 23  block: 23F6FFFF...FF (32 bytes)
0xEA: 03 44  block: 4403FFFF...FF (32 bytes)
0xEE: 04 00  block: FF0400FC0004E2000000000000000000  <-- NONCE HERE
0xF0: 45 46  block: 46F50004                             <-- 4-byte counter?
0xF1: 00 00  block: 00000000
0xF2: FF 00  block: 00B6FFFF
0xF3: FF 00  block: 8EFF...
0xF5: FF 00  block: 98FFFFFFFF
0xF6: 0C 8C  block: 8C0C8C00                             <-- 2 words, probably battery age/health
0xFA: FF 00  block: 0095FFFF...
0xFB: 00 00  block: 00000000
0xFF: 31 01  block: 0131
```
