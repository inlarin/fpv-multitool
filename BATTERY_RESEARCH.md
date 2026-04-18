# DJI Mavic 3 Battery Research: Notes from an ESP32 Toolkit

*April 2026. Compiled from ~28 sprints of empirical research.*

This document summarizes what I've learned trying to understand, clone, and
service DJI Mavic 3 smart batteries (and their PTL third-party clones) using
an ESP32-S3 board as a research platform.

**TL;DR**: Public community has no working Mavic 3 cycle-count reset as of
April 2026. Both genuine DJI packs and PTL clones resist all known software
unseal attacks. Here's what *does* work, what doesn't, and where the limits are.

If you have corrections, additional data points, or have successfully
unsealed one of these packs — please open an issue. Especially interested in
firmware dumps from fresh PTL batches and any ChipWhisperer voltage-glitching
results on BQ40Z307.

## Contents

1. [Battery hardware landscape](#1-battery-hardware-landscape)
2. [The authentication architecture](#2-the-authentication-architecture)
3. [PTL clone variants discovered](#3-ptl-clone-variants-discovered)
4. [Clone publish protocol (Variant A)](#4-clone-publish-protocol-variant-a)
5. [Real BQ40Z307 HMAC challenge (Variant B)](#5-real-bq40z307-hmac-challenge-variant-b)
6. [Attacks attempted and why they failed](#6-attacks-attempted-and-why-they-failed)
7. [What *does* work without unseal](#7-what-does-work-without-unseal)
8. [Realistic paths forward](#8-realistic-paths-forward)
9. [Open questions](#9-open-questions)
10. [Toolkit reference](#10-toolkit-reference)

---

## 1. Battery hardware landscape

DJI smart batteries across drone generations:

| Gen | Drones | BMS chip | Protocol | Public unseal? |
|---|---|---|---|---|
| 1-2 | Phantom 3/4, Mavic Pro, Spark | TI BQ30Z55 | SMBus test pads | ✅ TI default keys |
| 3 | Mavic 2, Air 1 | TI BQ40Z50 | SMBus test pads | ✅ SHA FET reset |
| 4 | Mini 1/2, Air 2/2S | TI BQ40Z307 | I²C on connector | ✅ RUS_MAV keys |
| **5** | **Mavic 3** (WM260) | **BQ40Z307 + NXP A1006** | I²C + ECC auth | ❌ **HMAC key private** |
| 6 | Mini 3/4, Air 3, Avata 2 | AES-encrypted custom | Proprietary | ❌ No progress |

The Mavic 3 generation is where the door slammed. It added a dedicated secure
authenticator chip (NXP A1006) alongside the gauge (BQ40Z307), and reset community
progress to zero.

## 2. The authentication architecture

Full Mavic 3 V01.00.0600 firmware was decrypted and reverse engineered. The
auth chain as implemented:

```
DJI drone SoC (MediaTek MTK)
  └── Hardware Secure Element (ROM/fuse key, not extractable)
       └── Trusted App e91c9402-…ta derives "SEC ROOT KEYV1"
            └── derives "ACCESSORY AUTH" key
                 └── verifies NXP A1006 cert from battery pack
                      └── if valid → drone accepts battery
```

Key firmware findings in `/system/lib64/libdji_secure.so` (398 KB ARM64 ELF):

| Function (offset) | Size | Purpose |
|---|---|---|
| `a1006_auth_challenge` (0x3fe40) | 904 | generate challenge to send to A1006 |
| `a1006_auth_verify` (0x40348) | 256 | verify A1006 signed response |
| `a100x_verifyDERSig` (0x3e298) | 1244 | ECDSA DER signature verify |
| `a1006_oneway_authentication` (0x3f9f8) | 436 | wrapper for one-shot auth |
| `a100x_v1_challenge` (0x3e068) | 384 | v1 challenge generation |
| `auth_chip_init` (0x43840) | 400 | A1006 I²C init |

Crypto parameters (from decompilation):
- **Curve**: `sect163r2` (B-163 binary field ECC). OID `1.3.132.0.15`.
- **Signature**: `ecdsa-with-SHA224` (OID `1.2.840.10045.4.3.1`)
- **Cert format**: 260-byte X.509 with placeholder fields, pubkey = 43 bytes
  (1 byte uncompressed marker + 21 byte X + 21 byte Y), signature = 20 bytes

The **root public key is NOT in libdji_secure.so**. It's either:
1. Inside the TA2 `e91c9402-…ta` trusted application binary, or
2. Derived from hardware SE fuses at boot

Either way — **not extractable via firmware analysis alone**. Would need
hardware attack on the drone SoC.

**Leaked source path** in strings: `hardware/dji/common/tools/dji_secure/vendor/a1006/a1006_v1.c`.

## 3. PTL clone variants discovered

"PTL" is a Chinese manufacturer of aftermarket Mavic 3 battery packs, sold
under the DeviceName `BA01WM260` mimicking genuine `BWX260` packs. I tested
two batteries labeled identically but internally very different:

### Variant A: fake ASIC (serial #58)

- `chipType` via MAC 0x0001 returns **0** (MAC not implemented)
- DJI serial number present at register 0xD8 (`4ERPL6QEA1D6Y9`)
- No NXP A1006 on board (no valid cert response)
- Implements basic SBS read protocol for surface telemetry only
- Backdoor markers at vendor registers: `0xAA55` at reg 0xD6, `...55AA` at reg 0xD9

This variant uses a custom ASIC that **pretends to be** a smart battery by
implementing just enough of the SBS standard for surface reads (voltage,
SOC, cell voltages). It doesn't speak TI ManufacturerAccess at all. I
scanned the full 65536-subcommand MAC space — zero responses.

### Variant B: genuine TI BQ40Z307 (serial #3475)

- `chipType` via MAC 0x0001 returns **0x4307** (real BQ40Z307)
- `FirmwareVersion` via MAC 0x0002 = full metadata: FW 1.01, build 0x0027, IC rev 0x0285
- `HardwareVersion` via MAC 0x0003 = 0x00A1
- MAC 0x0000 AUTHENTICATE returns **dynamic 20-32 byte HMAC-SHA1 challenge**
  (different bytes per read → real challenge, not cached)
- All standard SBS MAC commands (0x54 OpStatus, 0x55 ChargingStatus,
  0x56 GaugingStatus, 0x60 DAStatus, 0x70 ChemID) work
- Sealed, rejects all known public keys
- **Does NOT have NXP A1006** — PTL apparently skipped the real DJI auth chip

So PTL made Variant B using genuine TI parts but **didn't replicate DJI's
full auth stack**. They bet on drones with lax verification (older firmware?
soft-enforced paths?). PTL programmed their own HMAC-SHA1 key at manufacture
— unrelated to DJI's A1006 keys.

This variant is actually MORE interesting than genuine DJI: it has a real
sealed BQ40Z307 that would fully service if we had the unseal key.

## 4. Clone publish protocol (Variant A)

Despite lacking TI MAC, Variant A leaks live cell voltages through a
custom publish mechanism:

**Trigger**: rapid write transition to register 0x00 with specific MAC values
followed immediately by a block read.

**Packet format** (26-32 bytes):
```
[0x81] [0xF0] [counter] [0x01] [flag] [0x00]
[cell1_lo cell1_hi] [cell2_lo cell2_hi] [cell3_lo cell3_hi] [cell4_lo cell4_hi]
[tail_byte0] [0x3B] [tail_byte2] [0x3D]
[zero fill...]
```

- `0x81 0xF0` — fixed chip family marker
- `counter` — increments per publish (8-bit, seen wrapping)
- `0x01` — packet type (cell voltages)
- `flag` — varies 0x00/0x02/0x04, possibly status bits
- Cell voltages as LE u16 millivolts (matches register 0x3C-0x3F reads)
- Tail: `0x3B` and `0x3D` are constants (type markers),
  `tail_byte0/tail_byte2` vary in narrow range (likely two 1-byte sensor readings)

**Queue behavior**: publishes accumulate in a small buffer, drain on
consumption. Refill rate ~1 packet per 30-60 seconds. After a full 64K
brute, the queue sits empty for minutes.

**Not a control channel**: writing 0x81F0-formatted frames back to reg 0x00
or 0x44 produces no effect. The chip doesn't interpret its own publish
format as commands.

## 5. Real BQ40Z307 HMAC challenge (Variant B)

The BQ40Z307 uses TI's documented HMAC-SHA1 challenge-response for unseal:

```
1. Host → Device: read MAC 0x0000 (32-byte block)
   Device → Host: 20-byte random challenge

2. Host computes: HMAC-SHA1(auth_key_32_bytes, challenge) = 20-byte digest

3. Host → Device: write block [0x00, 0x00, digest...] to MAC 0x44

4. Device compares received digest to its own computation
   If match → unseal accepted
```

I implemented this in `unsealHmac()` using mbedTLS. Confirmed chip responds
to protocol (processes our writes, reg 0x07 command counter increments)
but rejects all tested keys.

**Keys tried** (all rejected):
- 32 × 0x00
- 32 × 0xFF
- 32 × 0x04 (TI default for HMAC region per some datasheets)
- Sequential 0x01..0x20
- `0x0414367204143672...` (repeated TI unseal keys)
- `DEADBEEF...` + ASCII patterns

**Why no progress**: PTL's HMAC key is a 2^256 space. Without a key leak or
side-channel attack, brute force is infeasible.

## 6. Attacks attempted and why they failed

Comprehensive log of software approaches and outcomes:

| Attack | Outcome | Notes |
|---|---|---|
| Standard TI/RU unseal keys | ❌ | 12+ combos tried via MAC 0x00 word writes |
| TI unseal via MAC 0x44 block write | ❌ | BK v0.5 flow, still sealed |
| PEC-enabled writes (SMBus strict spec) | ❌ | Clone actually REJECTS PEC — writes silently no-op |
| BQ9003 boot-mode entry (MAC 0x0033) | ❌ | No effect on reg 0x0D |
| BQ30Z55 boot-mode entry (MAC 0x0F00) | ❌ | No effect |
| MAC brute 0x0000-0xFFFF (Variant A) | ❌ | Zero responses — no TI MAC at all |
| MAC brute 0x0000-0xFFFF (Variant B) | ✅* | Many commands work, but none unseal |
| MAC response brute (non-FF filter) | ✅* | Found clone publish protocol (telemetry leak only) |
| SH366000-family magic values | ❌ | 0xE000/0xE001 are aliases for TI commands; others no-op |
| Data Flash dump 0x4000-0x70FF | ❌ | Seal fully blocks; only DeviceType/FW echoes leak |
| HMAC-SHA1 with public keys | ❌ | All zeros / 0xFF / TI / sequential all rejected |
| Write 81F0-formatted frames (inject) | ❌ | Chip doesn't consume its publish format as input |
| Reg 0x54 OpStatus byte-toggle brute | ❌ | False positives from natural bit-flap noise |
| Timing attack (50 samples) | ⚠️ | Delta 2-6ms visible but noise 15-246ms swamps signal |
| Vendor register magic (24 patterns × 48 regs) | ❌ | 4 writable scratch regs found, no unlock behavior |

\* = **worked**, but didn't lead to unseal.

**The fundamental barrier**: BQ40Z307's seal protection is cryptographic.
No amount of clever MAC sequencing or register manipulation bypasses it.
You need either:
1. The programmed HMAC key (private, not publicly available)
2. Hardware fault injection to bypass the key compare
3. Chip replacement

## 7. What *does* work without unseal

Despite not being able to unseal, significant capabilities remain:

### Read-only
- Full SBS: voltage, current, temp, SOC, cycle count, capacity, design capacity
- Individual cell voltages (both unsynchronized 0x3C-0x3F and synchronized DAStatus1)
- All status registers: Safety, PF, Operation, Manufacturing, Charging, Gauging
- MAC info reads: DeviceType, FW, HW, ChemID, SN, DAStatus
- Vendor range 0xD0-0xFF on clones (for fingerprinting, debug register exposure)

### Monitoring
- Live telemetry via WebSocket (~1 Hz)
- SVG chart: voltage + current ring buffer, 3-minute window
- Clone publish protocol capture (cell voltage leak on Variant A)
- I²C preflight diagnostics (SDA/SCL integrity, device scan)

### Fleet management
- JSON snapshot export per battery
- Multi-snapshot compare table with color-coded outlier detection
  (SOC/SOH/cycles/wear/cell delta)
- Data Flash editor UI (disabled on sealed packs but ready for unsealable ones)

### For genuinely unsealable packs (older DJI)
- Unseal wizard with profile dropdown
- MAC Command Runner with 40+ entry catalog + destructive warnings
- DF editor for 100+ BQ9003/BQ40z307 parameters
- Full service chain: unseal → clear PF → clear DJI PF2 (MAC 0x4062) → seal

## 8. Realistic paths forward

Given software unseal is infeasible on Mavic 3, here's what community recommends:

### Cell transplant (most practical, ~$30-60)

Buy a Mavic 3 battery with dead cells but low cycle count and healthy BMS.
Transplant new LiPo cells onto that BMS. Keeps original auth chain intact.
Skill required: decent SMD soldering, balance-connector rework.

**Don't** swap just the cells keeping them separate from BMS connector — the
BMS needs to see proper voltage ramp-up to initialize correctly.

### Firmware downgrade on drone (free, sometimes works)

Some older Mavic 3 firmware versions do less strict battery verification.
ARB (Anti-Rollback) on recent drones blocks this. Check MavicPilots forums
for current status on your specific firmware chain.

### Voltage glitching ($50-250)

Target: bypass the HMAC compare instruction in BQ40Z307 firmware.

- **PicoEMP** (~$50) or **ChipWhisperer-Lite** (~$250)
- Attach MOSFET crowbar across BQ40Z307 Vcc
- Trigger glitch during `seal → unseal transition` window
- Automate with 10K+ glitch parameter combos
- Success rate: likely 1-10% of chips (unpublished)

Nobody has publicly dumped a BQ40Z307 via glitching yet — you'd be first.

### Chip swap ($3 + skill)

Desolder existing BQ40Z307 (28-pin QFN), solder fresh chip ($3 DigiKey).
Program with TI BQStudio + EV2300 ($40 adapter) using default keys.
Requires: QFN rework gear, TI software, patience.

### MITM proxy approach (for pros, weeks of work)

If you have an original Mavic 3 battery with living auth chip but dead cells:
- ESP32 between drone and auth chip
- Forward A1006 I²C challenge-response
- Replace cells underneath
- Open source project does not exist — **huge opportunity**

## 9. Open questions

Areas where new data would be very welcome:

1. **Does any PTL batch use NXP A1006?** My two variants don't have one.
   Maybe newer PTL batches do. A fingerprint scan for A1006 on I²C address
   0x65 or 0x35 would tell.

2. **What's PTL's HMAC key format?** Possibly derived from serial number?
   I tested simple derivations (serial as key, XOR patterns) — no hit.
   Anyone with access to multiple PTL packs could diff-attack the key.

3. **Does a newer BK / Drone-Hacks / UBRT release support Mavic 3?** Reports
   are inconsistent. If someone with working tool can dump SBS transaction
   log, we can replay.

4. **Is there a v2 Mavic 3 firmware with different auth?** I only analyzed
   V01.00.0600. Versions 0100, 0500, 0800+ might have variations.

5. **The 4-byte trailer on Variant A publish packets** (`A? 3B ?? 3D`) —
   what are the two varying bytes? Temperature? Cell balance state?
   Current readings? Anyone with same hardware could replicate and correlate.

## 10. Toolkit reference

All code open source at: https://github.com/inlarin/fpv-multitool

### Firmware APIs (web UI, v0.24+)

Live endpoints on `http://<board-ip>/`:

- `/api/batt/snapshot` — full JSON of current battery state
- `/api/batt/diag?mac=0xNNNN` — MAC block read
- `/api/batt/diag?sbs=0xNN&type=word/dword/block` — SBS register read
- `/api/batt/diag?unseal=w1,w2` — custom unseal attempt
- `/api/batt/unseal_hmac` — POST 32B hex key → HMAC-SHA1 unseal try
- `/api/batt/clone/harvest` — batch write-read with configurable params
- `/api/batt/clone/trans_brute` — transition write macA→macB scanner
- `/api/batt/clone/broad_brute` — sweep (macA, macB) space
- `/api/batt/clone/macresp_start` — background 64K MAC brute, filter non-FF
- `/api/batt/clone/dfdump` — try DF reads via MAC 0x44
- `/api/batt/clone/sh366000_test` — SH366000-family magic commands
- `/api/batt/clone/timing_attack` — HMAC verify timing measurement
- `/api/batt/clone/logger_start` — persistent background publish capture
- `/api/batt/clone/inject` — write arbitrary frame to register
- `/api/batt/clone/watch` — passive periodic poll for publishes
- `/api/batt/clone/wpec` — PEC-enabled writes
- `/api/batt/df/map` / `/read` / `/write` / `/readall` — DF editor
- `/api/i2c/preflight` — SDA/SCL health + bus scan

### Python toolkit (USB2I2C mode, 1000+ tx/sec)

In `scripts/clone_research/`:

- `cp2112.py` — HID driver (slave shift, autoSendRead, 64-byte padding)
- `scan_sbs_full.py` — enumerate 0x00-0xFF SBS registers
- `scan_mac_full.py` — brute 0x0000-0xFFFF MAC subs, filter non-zero, CSV output
- `bypass_probe.py` — test seal enforcement on 19 standard regs
- `magic_probe.py` — 24 magic patterns × 48 vendor regs, find writable ones
- `nonce_study.py` — deep-study single-register behavior under repeated writes
- `challenge_harvester.py` — bulk collect (write, response) to CSV
- `response_analyze.py` — entropy + period + CRC-16 hypothesis tests
- `crc_analyze.py` — offline analysis of captured clone publish packets

### Hardware

- Waveshare ESP32-S3-LCD-1.47B (or diymore clone)
- Battery connection: I²C SDA/SCL on GPIO 11/10 (Wire1)
- QMI8658 IMU on primary Wire (GPIO 48/47) — doesn't conflict with battery
- RGB WS2812 status LED on GPIO 38
- Case: OpenSCAD source in `hardware/case.scad`

---

## Contributing

If you:

- Have a working Mavic 3 unseal key (genuine or PTL) → please open PR or issue
- Successfully voltage-glitched BQ40Z307 → write-up would be hugely valuable
- Have multiple PTL batteries to diff against mine → contact
- See factual errors in the above → issue please

This is personal research, not a commercial effort. Everything here is
informational for security research and battery service learning purposes.

## Credits

Builds on work by:

- [o-gs/dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools) — THE reference for DJI battery internals
- DJI Battery Killer team — CP2112 workflow pioneer
- Charlie Miller's BlackHat 2011 battery firmware hacking paper
- Nozomi Networks Mavic 3 firmware analysis series
- mavicpilots.com / elektroda.com / laptopu.ro community threads
- Russian-speaking research community (yuglink.ru, dji-club.ru, rcopen.com)

Extensive thanks to everyone who has published their findings openly.
