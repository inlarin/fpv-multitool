# PTL BA01WM260 Clone Battery — Research Findings

**Device:** BA01WM260 (PTL brand fake Mavic 3 battery, 4S, sealed)
**Serial:** 4ERPL6QEA1D6Y9 (reg 0xD8)
**Cycles:** 1
**Method:** ESP32 CP2112 HID emulator, Python + web-based probing
**Updated:** 2026-04-18

## Verdict (after Sprint 24)

This clone uses a **custom ASIC that mimics surface SBS only**. It does NOT speak any standard TI protocol. Unsealing via software interface appears infeasible without reverse-engineering the chip firmware directly.

## What we tested

| Test | Result |
|------|--------|
| Full 64K MAC scan (0x0000-0xFFFF) | 0 responses — no TI MAC |
| 24 magic patterns × 48 vendor regs | 4 regs accept writes (0xD4/E5/EB/EE) |
| Seal bypass via writes to 0x17, DF 0x4340 | Rejected (seal enforced) |
| Standard RU/TI unseal keys | All fail |
| BQ9003 boot entry (MAC 0x0033 → read 0x0D) | No special response (normal SBS) |
| BQ30Z55 boot entry (MAC 0x0F00 → read 0x0D) | No special response |
| 0xEE pseudo-random "challenge" analysis | **Not a challenge** — just internal timer state |

## Detailed 0xEE register analysis (from 500 harvested samples)

Format: 16-byte block response `[80+in_lo] 04 00 FC FC XX YY 00 × 9`

- **byte 0** = `(write_val & 0xFF) | 0x80` — simple echo with high bit set
- **bytes 1-4** = `04 00 FC FC` — fixed device-family signature
- **byte 5** = slow counter: increments by ~8-9 per I2C transaction (~1.2ms per unit)
  - NOT dependent on input write value
  - Looks like milliseconds-since-power-on or bus-transaction counter
- **byte 6** = chaotic value, highly varying
  - NOT a deterministic function of input
  - Likely LFSR or CRC over internal state
- **bytes 7-15** = zeros

Conclusion: 0xEE is an **internal-state-exposure register** (debug/diagnostics), NOT an auth challenge. Writing does nothing meaningful — state advances regardless.

### Error pattern
Occasionally (maybe 1-2% of samples) the response returns all `0xFE`:
`FEFEFEFEFEFEFEFEFEFEFEFEFEFEFEFE`

Likely bus timing/arbitration issue during high-rate probing. Not a feature.

## What's still untested

1. **DJI official tool capture** — run DJI Assistant (or Battery Killer) on a REAL Mavic 3 battery, log all SMBus traffic via CP2112 emulator, then replay on this clone
2. **Combinatorial writes** — specific write sequences to `(regA, regB, regC)` may unlock something
3. **Hardware** — open pack, identify chip markings, probe JTAG/SWD/UART
4. **Shim DLL** — intercept SLABHIDtoSMBus.dll calls from DJI tools to extract live keys/challenges

## Vendor register map (high range)

```
0xD0-D3: 0x0000 (unused)
0xD4: writable, word echoes — scratch
0xD5: word=0x0001, block=0x00 (constant)
0xD6: word=0xAA55, block=AA66FFFF... — CHIP FAMILY MARKER
0xD7: 0x0000
0xD8: "4ERPL6QEA1D6Y9" — DJI serial ASCII (14 bytes)
0xD9: block ending in ...55AA — MARKER PAIR
0xDA-DF: 0x0000
0xE0-E4: 0x0000
0xE5: writable scratch
0xE6: 0x0000
0xE7: block 023A...FF
0xE8: block 23F6...FF
0xE9: all 0xFF
0xEA: block 4403...FF
0xEB: writable scratch
0xEE: live internal-state register (see above)
0xEF: all 0xFF
0xF0: block 46F50004 — possibly uptime/tx counter
0xF1: 0x00000000
0xF2: block 00B6FFFF
0xF3: block 8EFF...
0xF5: block 98FFFFFFFF
0xF6: block 8C0C8C00 — two u16 values (3212, 140 — age/health?)
0xF7: block 8074...FF
0xF8: block 00D5FF...
0xFA: block 0095FF...
0xFB: 0x00000000 (unused? writable?)
0xFD-FE: all 0xFF
0xFF: 0x0131 — possibly total_tx_count
```

## Research toolkit (`scripts/clone_research/`)

| File | Purpose |
|------|---------|
| `cp2112.py` | Python CP2112 HID driver (working, tested) |
| `scan_sbs_full.py` | Enumerate 0x00-0xFF SBS regs (~5s) |
| `scan_mac_full.py` | 64K MAC brute-scan (~3.3 min) |
| `bypass_probe.py` | Test seal on standard regs |
| `magic_probe.py` | Find writable vendor regs via magic-pattern writes |
| `nonce_study.py` | Deep-study single register behaviour under repeated writes |
| `challenge_harvester.py` | Bulk harvest (write, response) pairs to CSV |
| `response_analyze.py` | Entropy + periodicity + CRC hypothesis testing |

## Required to switch board for Python toolkit

```bash
# Enable USB2I2C mode (exposes CP2112 HID over USB)
curl -X POST -F mode=2 http://192.168.32.50/api/usb/mode
curl -X POST http://192.168.32.50/api/usb/reboot

# ... run Python scripts ...

# Switch back to CDC+web
curl -X POST -F mode=0 http://192.168.32.50/api/usb/mode
curl -X POST http://192.168.32.50/api/usb/reboot
```

## Web-based harvester (v0.22+)

Battery Lab → DJI Clone sub-tab → Challenge Harvester card:
- Write value + count + read length → collect samples in browser
- On-the-fly entropy + period analysis
- Export to CSV
- Raw block writes to arbitrary regs (with or without length prefix)
