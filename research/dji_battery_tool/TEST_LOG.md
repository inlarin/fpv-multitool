# DJI Mavic 3 battery service — live test log

Date: 2026-05-01
Board: Waveshare ESP32-S3-LCD-1.47B (192.168.32.50)
Firmware: commit `da4bd39` (v0.34.2 string -- pre-tag build, but contains all v0.35.0 service code)
Test scope: 5 Mavic-3-class batteries, identification + safe ops only

## Code-improvement notes (to implement back into firmware)

These are observations from running the new code against real packs. Each item
is a planned firmware patch.

### 1. Snapshot endpoint field-name inconsistency

Our `/api/batt/snapshot` JSON uses field names that don't match what `BatteryInfo`
struct calls them:

| BatteryInfo field         | Snapshot JSON key       | Tool expected |
|---------------------------|-------------------------|---------------|
| `manufacturerName`        | `mfrName`               | longer name |
| `firmwareVersion`         | `fwVersion`             | longer name |
| `hardwareVersion`         | `hwVersion`             | longer name |
| `stateOfCharge`           | `soc`                   | longer name |
| `fullCapacity_mAh`        | `fullCap_mAh`           | longer name |
| `designCapacity_mAh`      | `designCap_mAh`         | longer name |
| `remainCapacity_mAh`      | `remainCap_mAh`         | longer name |
| `operationStatus`         | `opStatus`              | longer name |
| `manufacturingStatus`     | `mfgStatus`             | longer name |
| `cellVoltage[4]`          | `cellVoltage_mV` array  | matches |
| `cellVoltSync[4]`         | `cellVoltageSync_mV`    | matches |
| `djiSerial`               | _(missing!)_            | should be added |

**Action**: in `routes_battery.cpp` `/api/batt/snapshot` handler, alias both
short and long names so external clients can use either; OR pick one
convention and stick with it. I'll standardise on the longer struct-field
names since they're already in dji_battery.h.

### 2. Snapshot endpoint missing decoded status strings for opStatus / mfgStatus

Currently only `batteryStatusDecoded` is included. Should also include:
- `opStatusDecoded` (calls `decodeOperationStatus()`)
- `pfStatusDecoded` (calls `decodePFStatus()`)
- `safetyStatusDecoded` (calls `decodeSafetyStatus()`)
- `mfgStatusDecoded` (calls `decodeManufacturingStatus()`)

Decoder functions exist; just not wired to the JSON output. Adds
human-readable strings without bloating bandwidth too much.

### 3. Port B mode auto-switch missing from snapshot path

When the user requested `/api/batt/snapshot` while Port B was in **UART mode**
(owner=boot), the snapshot returned `connected:false` instead of trying to
acquire I2C first.

Looking at `dji_battery.cpp::tryEnsureInit()`, it only acquires Port B if
the current mode is IDLE or already I2C. If port is held by UART/PWM/GPIO,
it returns false and the caller sees `not connected` -- no useful error.

**Action**: add `/api/batt/snapshot?force_acquire=1` query param OR a new
`/api/batt/acquire` endpoint that releases whatever holds Port B and grabs
it as I2C. Otherwise the user has to know about `/api/port/release` first,
which isn't discoverable.

### 4. Port preferred-mode form-param vs query-param mismatch

`POST /api/port/preferred?mode=1` returned **400** "Missing 'mode' form param".
The endpoint expects POST body, not query string. That's surprising for a
service intended to be hit by curl.

**Action**: accept both form param AND query param. Or document explicitly.

### 5. ChipType / FW / HW MAC reads return 0 on sealed Mavic 3 clones

On battery #1 (PTL clone, sealed):
- `chipType` (MAC 0x0001 DeviceType) = 0
- `fwVersion` (MAC 0x0002 FirmwareVersion) = 0
- `hwVersion` (MAC 0x0003 HardwareVersion) = 0

These should work without unseal on a TI BQ40Z80, but PTL clones may
gate them. Worth re-testing after unseal to see if they populate.

If they remain 0 even after unseal, likely the clone implements
ManufacturerData differently. Add a fallback that reads ManufacturerName
+ DeviceName as fallback identification.

### 6. DAStatus1 (sync cell voltages) returns packVoltage_mV = 0

DAStatus1 read at reg 0x71 is supposed to return synchronized cell voltages
+ pack voltage. Cell voltages were correctly populated, but `packVoltage_mV`
is 0. That suggests our parser at offset [10..11] is reading the wrong bytes
on PTL clones -- the layout may differ slightly from TI reference.

**Action**: add the full 32-byte DAStatus1 block as a hex string in the
snapshot so we can decode the actual layout without guessing. Then update
`readDAStatus1()` to detect the variant.

### 7. timeToFull / runTimeToEmpty = 65535 (sentinel "no estimate")

That's NORMAL for an idling pack at 0 current -- 65535 means "infinite"
in SBS. Just suppress them in UI when current == 0.

### 8. UI consideration: Status reg `INIT DSG ` still shown

On a sleeping pack, BatteryStatus has INIT bit set ("BMS still initializing")
and DSG bit ("ready to discharge"). Both are fine; add a UI hint distinguishing
"normal idle" from "fault" rather than showing raw flags.

---

## Battery #1 -- PTL clone Mavic 3 (sealed, 0 cycles, healthy)

### Identification
- **Manufacturer**: PTL (Pacific Tech Logistics -- known DJI clone OEM)
- **DeviceName**: BA01WM260 (= Mavic 3 series codename)
- **Chemistry**: LION
- **Detected model**: Mavic 3 (cells: 4)
- **Serial number**: 58 (low number suggests early clone batch)
- **Manufacture date**: 2021-09-25 (decoded from packed SBS reg 0x1B)
- **ChipType / FW / HW**: all 0 (MAC reads NACKed -- PTL gates them while sealed)

### Security state
- **Sealed**: YES
- **OperationStatus**: `0x1028305` -- decodes to SEC=Sealed, PRES, DSG, WAKE, PF (huh -- PF bit is set in OS but PFStatus reads 0?)
  - Wait, bit 7 of opStatus is PF. opStatus = 0x1028305 -> bit 7 mask = 0x80 -> set!
  - But PFStatus reg 0x53 = 0x0. Contradiction -- the OS PF bit might mean "PF event happened in past, now cleared" rather than "PF currently active".
  - Or PTL clones don't track PFStatus separately; PF flag in OS is the only indicator.
- **ManufacturingStatus**: `0x2B8` -> DSG_T (discharge FET test) | LF_E | PF_E | BB (Black Box enabled)
  - PF_E bit set means BMS is willing to enter PF on next trigger -- normal for production pack.

### Live readings
- **Pack voltage**: 15.509 V (design 15.4 V) -- pack at ~62% of full voltage
- **Current**: 0.000 A (idle, no load)
- **SOC display**: 69% / Absolute: 34% -- 35-point divergence!
  - Display SOC is what the host shows pilots; absolute SOC is the gauge's actual estimate.
  - Big divergence usually means the gauge needs to relearn (run a charge/discharge cycle) -- candidate for `startCalibration()` test.
- **Capacity**: 3311 mAh remaining of 4779 mAh full (design 5000 mAh)
  - Wear: 4.4% (full / design)
- **StateOfHealth**: 100% -- BQ40Z80 reports its self-assessment as healthy
- **Temperature**: +21.2 C -- room temp
- **Cycles**: 0 -- either truly new (unlikely for a PTL with 4.4% wear) or cycle counter was reset before
- **batteryStatus**: 0xC0 = `INIT DSG` (initialising, ready to discharge)

### Cells (4S)
- Cell 1: 3.877 V
- Cell 2: 3.852 V
- Cell 3: 3.881 V
- Cell 4: 3.899 V
- **Spread (max-min)**: 47 mV -- borderline; OK for an unused pack at this SOC, but worth balancing if we go back to test.

### Faults
- PFStatus: 0x0 (no permanent failures)
- SafetyStatus: 0x0 (no latched safety trips)
- DJI PF2: 0x0 (no DJI custom PF flag)

### Charging recommendation (from BMS)
- Charge V: 17.6 V
- Charge I: 2.2 A (= 2200 mA -- = 0.44C for a 5Ah pack, gentle)

### Verdict
- Healthy pack, safe for ALL service operations
- Likely candidate for: capacity edit (display SOC is misaligned), unseal+FAS verification

---

## Battery #2 -- PTL clone Mavic 3 (newer batch 2024-10, different firmware)

### Identification
- **Manufacturer**: PTL
- **DeviceName**: BA01WM260
- **Chemistry**: `"5129"` -- NOT "LION"! Numeric chemistry code, suggests this clone
  reports a custom chemistry-profile ID instead of the standard string. Worth
  decoding -- may be a TI ChemID lookup. (BQ40Z80 ChemID is normally
  reported via MAC 0x0008.)
- **Detected model**: Mavic 3 (cells: 4)
- **Serial**: 3475 (vs #1=58 -- this is a later batch)
- **Manufacture date**: **2024-10-22** (3 years newer than #1)
- **ChipType**: `0x4307` = **BQ40z307** ✓ (works on this pack while sealed!)
- **FW Version**: 1859 (decimal -- raw uint16, decode TBD)
- **HW Version**: 161

### Security state
- **Sealed**: YES
- **OperationStatus**: `0x4E300` -- decodes to SEC=Sealed, no PF bit set
  (cleaner than #1 which had spurious PF bit)
- **ManufacturingStatus**: `0xFFFFFFFF` -- this is a NACK sentinel, not a real
  reading. Newer PTL firmware NACKs reg 0x57 (ManufacturingStatus) while sealed.

### Live readings
- **Pack voltage**: 15.271 V (= 4 x 3.818, perfect match with cell readings)
- **Current**: 0.000 A
- **SOC display**: 35% / Absolute: 33% -- only 2-point divergence (vs #1's 35-point gap),
  suggests fresh learning state or recent calibration cycle
- **Capacity**: 1622 / 4650 mAh full (design 5000)
  - Wear 7% (slightly more degraded than #1)
- **StateOfHealth**: 0xFFFF (NACK) -- newer firmware doesn't expose SoH while sealed
- **Temperature**: +20.5 C (room temp)
- **Cycles**: 0 (also reset, like #1)
- **Status reg**: 0xC0 = `INIT DSG` (same as #1)

### Cells (4S)
- All 4 cells: **3.818 V exactly** -- spread = 0 mV
  - This is suspicious for a real cell pack -- typical packs show at
    least 5-10 mV spread between cells from manufacturing tolerances.
  - HYPOTHESIS A: PTL firmware on this batch reports the average (or one
    cell's value) for all 4 reads. We can verify by physically measuring
    cells separately and comparing.
  - HYPOTHESIS B: cells are all genuinely equal because pack was just
    rebalanced (or never used).
  - HYPOTHESIS C: BMS implements "fake" cell voltage reads when sealed
    (returning the 1/4 of pack voltage = 15271/4 = 3817.75 mV ~ 3818).
    The 1mV rounding match is pretty strong evidence for this.
- **DAStatus1 sync read returned 0** for all cells -- DAStatus1 not supported
  on this PTL variant (or different layout).

### Faults
- PFStatus: 0x0
- SafetyStatus: 0x0
- DJI PF2: empty (snapshot endpoint omits this field on snapshot output -- bug)

### Charging recommendation
- **Empty/zero on this pack** -- newer PTL firmware doesn't expose ChargingVoltage
  / ChargingCurrent regs while sealed (vs #1 which did).

### Code-improvement notes from this pack

**9. Variant-aware identification**

PTL clones come in at least two different "API generations":
- **2021 batch (#1 style)**: exposes Charging V/I, SoH, MfgStatus while sealed,
  but NACKs ChipType/FW/HW MAC reads, populates per-cell voltages with real
  per-cell data
- **2024 batch (#2 style)**: exposes ChipType/FW/HW MAC reads, but NACKs
  Charging V/I, SoH, MfgStatus, DAStatus1, and synthesises per-cell voltages
  as pack/4

**Action**: add a `BatteryFirmwareVariant` field to `BatteryInfo` that detects
which generation we're talking to (probably by trying ChipType MAC read AND
ChargingVoltage SBS read -- whichever combo works tells us the variant).
Then UI can hide fields that are known not to work, instead of showing them
as 0 / "-" which looks like an error.

**10. Suspicious "fake" per-cell readings**

If all 4 cells return identical voltage = pack/4 to within 1 mV, it's almost
certainly synthesised, not actually measured. We should:
- Detect this case (all cells equal AND sum matches pack to <2 mV)
- Mark cells as "synthesised" in UI (display with grey colour)
- Suggest user runs a balance to verify (a real spread will appear if
  cells genuinely differ)

**11. Chemistry field is sometimes numeric not "LION"**

PTL-2024 reports chemistry = "5129" instead of "LION". Add chemistry-string
recognition: if it's a 4-digit number, treat it as a TI ChemID and look up
the meaning. Current code just stores the raw string.

**12. ManufactureDate decoding now in helper, should be in firmware**

I added `decode_mfr_date()` to the Python tool but the firmware doesn't
expose the decoded YYYY-MM-DD string -- only the raw 16-bit packed value.
Add a decoded variant to the snapshot endpoint:
```cpp
d["mfrDateDecoded"] = (year, month, day formatted)
```
Same for serialNumber if it has a known DJI structure (it doesn't, just int).

### Verdict for #2
- Newer firmware (2024 batch), partially-sealed-API
- Cells likely synthesised by sealed-mode firmware -- needs unseal to see real values
- Healthy: no PF, no Safety trips, capacity ~7% degraded, cycles 0
- Safe for: unseal+FAS, PF clear (low risk -- no PF anyway)
- **Recommended next test**: unseal+FAS on THIS pack to compare cell voltages
  pre/post -- will confirm whether cells were synthesised

---

## Battery #3 -- PTL clone Mavic 3 (2021 batch, slightly used)

### Identification
- **Manufacturer**: PTL
- **DeviceName**: BA01WM260
- **Chemistry**: LION
- **Detected model**: Mavic 3 (cells: 4)
- **Serial**: **58** (= IDENTICAL to #1!)
- **Manufacture date**: **2021-09-25** (= IDENTICAL to #1!)
- **ChipType / FW / HW**: 0/0/0 (sealed-NACK -- same firmware generation as #1)

### Security state
- **Sealed**: YES
- **OperationStatus**: `0x1128305` -- bits set differ from #1's `0x1028305`
  - Difference: bit 16 set in #3's value (`0x100000` vs `0x000000` in #1)
  - That's likely a "DSG_FET on" / "CHG_FET on" / similar low-impact bit
- **ManufacturingStatus**: 0x2B8 (same as #1)

### Live readings
- **Pack voltage**: 15.612 V (more charged than #1's 15.51 V)
- **Current**: 0.000 A (idle)
- **SOC display**: 71% / Absolute: NACK ("-")
  - #1 had 69%/34% with absolute working
  - #3's absoluteSOC NACKs even though display SOC works -- inconsistent
- **Capacity**: 3386 / 4776 mAh full -- nearly identical to #1
  - Wear: 4.5% (vs #1's 4.4% -- consistent batch)
- **StateOfHealth**: 99% (vs #1: 100% -- one cycle has elapsed)
- **Temperature**: +21.1 C (room)
- **Cycles**: **1** (vs #1: 0 -- this pack was discharged at least once)
- **Status reg**: 0xC0 (same as #1, #2)

### Cells (4S)
- Cell 1: 3.911 V
- Cell 2: 3.881 V
- Cell 3: 3.905 V
- Cell 4: 3.915 V
- Spread: **34 mV** -- tighter than #1's 47 mV (better balance after 1 cycle)
- Sync values match async -- DAStatus1 works on this pack ✓

### Faults
- All PF / Safety = 0 -- clean

### Code-improvement notes from this pack

**13. Serial-number reuse across PTL clones**

Batteries #1 and #3 both report serial=58. This is the SBS standard
serialNumber register (0x1C, 16-bit). PTL factory doesn't uniquely program
this -- entire 2021-09-25 batch ships with serial=58.

**Implications:**
- Can't use SBS serial to uniquely identify a PTL pack
- Must rely on the DJI custom serial at MAC reg 0xD8 (which our snapshot
  endpoint omits from JSON -- bug #11 from earlier log)
- For session-tracking (e.g. "this is the same pack we tested 5 minutes ago")
  add a fingerprint = (serial, mfrDate, fullCap_mAh, cycles, opStatus) hash

**Action**:
- Add `djiSerial` to snapshot JSON
- Add `fingerprint` field computed server-side as e.g. fnv1a(djiSerial + serialNum + mfrDate + fullCap)
- UI can show fingerprint suffix as a "Pack ID" so user can verify they're
  looking at the same pack between switches

**14. absoluteSOC field NACKs intermittently within same firmware generation**

Battery #1 returned absoluteSOC = 34% successfully.
Battery #3 (same firmware generation) returned absoluteSOC = "-" (NACK).
Battery #2 returned 33%.

Same firmware generation should behave identically. The only difference:
#3 has cycles=1 vs #1 cycles=0. Possibly the BMS only exposes absoluteSOC
once it has enough learning data (>0 cycles).

**Action**: just document this in the snapshot JSON's `absoluteSOC` comment,
no firmware change needed.

**15. PF flag in OperationStatus differs from PFStatus reg readout**

Battery #1: opStatus = `0x1028305` -- bit 7 (PF) is SET, but PFStatus reg = 0
Battery #3: opStatus = `0x1128305` -- bit 7 (PF) is SET, but PFStatus reg = 0
Battery #2: opStatus = `0x4E300` -- bit 7 (PF) is CLEAR, PFStatus reg = 0

**Hypothesis**: opStatus bit 7 means "PF was triggered at some point in past"
(persistent flag), while PFStatus is the current reason for the trigger.
PTL 2021 batch firmware sets the historical flag even when PF was cleared;
PTL 2024 batch doesn't.

**Action**:
- In `decodeOperationStatus()`, label bit 7 as "PF (historical)" not "PF
  (active)"
- The "Has any PF" check in our code should look at PFStatus reg, NOT
  opStatus bit 7. Verify our `hasPF` logic in `dji_battery.cpp:440` --
  yes, it correctly looks at pfStatus and djiPF2, not opStatus bit 7.

### Verdict for #3
- Twin of #1 from same batch (2021-09-25, serial 58)
- 1 cycle of use, slightly better cell balance (34 mV vs 47 mV)
- Safe for ALL service ops
- Good candidate for cell-balance test (still has measurable spread)

---

## Battery #4 -- PTL clone Mavic 3 (third member of 2021-09-25 batch)

### Identification
- Manufacturer: PTL | DeviceName: BA01WM260 | Chemistry: LION
- Serial: **58** | Mfr Date: **2021-09-25** -- (same batch as #1, #3)
- ChipType / FW / HW: 0/0/0 (sealed-NACK, same as #1, #3)

### Live readings
- Pack: 15.444 V (less charged than #1=15.51, #3=15.61)
- SOC display: 64% / absolute: 30% (absoluteSOC works here -- #3 was the
  outlier where it NACKed)
- Capacity: 3047 / 4767 mAh full (design 5000) | wear 4.7%
- SoH: 99% | Temp: +20.8 C | Cycles: 1
- Status reg: 0xC0 (INIT DSG)
- opStatus: `0x1028305` -- IDENTICAL to #1 (vs #3's 0x1128305 with bit 16 set)
  - The bit-16 flag in #3 is correlated with discharging recently?

### Cells (4S, all real readings)
- Cell 1: 3.856 V | Cell 2: 3.837 V | Cell 3: 3.875 V | Cell 4: 3.876 V
- Spread: 39 mV (between #1's 47 and #3's 34)

### Faults
- PFStatus / SafetyStatus / DJI PF2: all 0 -- clean

### Pattern across 2021 batch (#1, #3, #4)

| Metric | #1 | #3 | #4 | Range |
|---|---|---|---|---|
| Wear | 4.4% | 4.5% | 4.7% | 4.4-4.7% (very consistent) |
| SoH | 100% | 99% | 99% | 99-100% |
| Spread | 47 | 34 | 39 mV | 34-47 mV |
| Cycles | 0 | 1 | 1 | 0-1 |
| absoluteSOC works? | yes | NO | yes | inconsistent |

**Consistent batch quality**: degradation, SoH, cell balance all within
narrow ranges -- PTL factory output is well-controlled within a batch.
The absoluteSOC inconsistency might be due to which charge/discharge
state each pack was last left in.

### Code-improvement notes from this pack

**16. opStatus bit 16 correlates with "had non-zero current recently"**

#1 (cycles=0) opStatus = 0x1028305
#3 (cycles=1) opStatus = 0x1128305 -- extra bit 16 set
#4 (cycles=1) opStatus = 0x1028305 -- bit 16 NOT set despite cycles=1

So bit 16 isn't simply "was used". It may be:
- "Discharge in progress in current session"
- "FET state different from default"
- "Last-used flag" that times out

**Action**: research BQ40z307 OperationStatus bit definitions for bit 16.
Reference: TI BQ40Z80 reference manual (sluub69.pdf) section 16.10.
Then update `decodeOperationStatus()` to label it correctly.

### Verdict for #4
- Same batch as #1/#3, same wear/SoH characteristics
- Less charged (15.44 V vs 15.51/15.61 in twins)
- Safe for ALL service ops
- Useful as a "control" pack -- run same test as on a twin and compare

---

## Battery #5 -- PTL clone Mavic 3 (2021 batch, FULLY CHARGED)

### Identification
- PTL | BA01WM260 | LION | Serial: 58 | Mfr: 2021-09-25
- ChipType / FW / HW: 0/0/0 (sealed-NACK same as #1/#3/#4)

### Live readings
- **Pack: 16.293 V** -- fully charged (vs 15.4-15.6 V on partially-charged twins)
- **SOC display: 100% / Absolute: 70%** -- 30-point gap
- Capacity: 4778 / 4778 mAh full (= 100% of full charge capacity)
- SoH: 99% | Wear: 4.4% | Temp: +21.2 C | Cycles: 1
- **Status reg: 0xE0 = INIT DSG FC** (FC = Fully Charged bit SET, ✓)
- opStatus: `0x1008305` -- different bit pattern again from twins:
  - #1: 0x1028305  (bit 17 set)
  - #3: 0x1128305  (bits 16, 17 set)
  - #4: 0x1028305  (bit 17 set)
  - #5: 0x1008305  (bit 17 NOT set, bit 15 IS set?)
  - Bit 17 (mask 0x20000) seems to correlate with "in service mode" / 
    "discharge enabled". When pack is fully charged (#5) BMS clears it.

### Cells (4S)
- Cell 1: 4.072 V | Cell 2: 4.075 V | Cell 3: 4.072 V | Cell 4: 4.074 V
- **Spread: 3 mV** -- BEST of all 5 batteries (vs 34-47 mV on partially charged)

### Faults
- All clean -- PF = 0, Safety = 0

### Important observation: absoluteSOC anomaly

By BQ40Z80 spec:
```
StateOfCharge (0x0D)   = remainCap / fullChgCap * 100
AbsoluteStateOfCharge (0x0E) = remainCap / designCap * 100
```

For this pack:
- remainCap = 4778, fullChgCap = 4778, designCap = 5000
- Expected SoC = 4778/4778 = 100% ✓ (matches reported 100%)
- Expected AbsoluteSoC = 4778/5000 = 95.6% (BUT reported 70%!)

The absoluteSOC reading of 70% on a fully-charged pack is anomalous.
Hypotheses:
- A: PTL firmware computes absoluteSOC = remainCap / (designCap * (1 + wearMargin)) -- 
  with wearMargin ~37% would give 70%
- B: PTL firmware reports absoluteSOC = "true % of brand-new design capacity"
  where designCap includes manufacturing safety margin (the BMS treats
  designCap as "hard maximum" and rated capacity is lower)
- C: Bug in our `info.absoluteSOC = SMBus::readWord(BATT_ADDR, REG_ABS_SOC);`
  -- maybe register 0x0E returns something different on this firmware variant

**Action**: after we unseal one of these packs, compare absoluteSOC reads
sealed vs unsealed. If they differ significantly, the value depends on
internal state we can't see while sealed.

### Cell-balance correlation observation

Across all 4 packs from the 2021-09-25 batch:
| Pack | Pack V | SOC display | Cell spread |
|---|---|---|---|
| #1 | 15.51 V | 69% | 47 mV |
| #3 | 15.61 V | 71% | 34 mV |
| #4 | 15.44 V | 64% | 39 mV |
| #5 | 16.29 V | 100% | **3 mV** |

Strong correlation: higher charge state -> tighter cell balance. This is
expected behaviour -- BMS actively balances cells during constant-voltage
phase of charging (top end).

### Code-improvement notes from this pack

**17. Status reg `FC` bit reliable charge-completion indicator**

When pack is fully charged, the `FC` (Fully Charged, bit 5) flag in
SBS BatteryStatus reg is set. This is the cleanest "is this pack at top
of charge?" signal. Use it instead of comparing SOC to 100%.

**18. opStatus bit pattern correlates with multiple state dimensions**

Bit 16 (0x10000): seen on #3 only (the "warm" used pack? was discharging
recently?)
Bit 17 (0x20000): seen on #1, #3, #4 (discharge-ready) -- CLEARED on #5
which is fully charged

Likely: bit 17 = "DSG FET enabled" (allowed to discharge). When fully
charged, the BMS may temporarily disable DSG FET to prevent over-discharge
risk during high voltage state.

Action: tabulate full bit map by triggering known states (charge full ->
note bits, discharge low -> note bits) and document in
`decodeOperationStatus()`.

---

# Cross-pack analysis & next-step plan

## Identification fingerprint matrix

| # | Date | Serial | Mfr | DevName | ChemStr | ChipMAC | Pack V | SOC% | SoH | Cyc | PF |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2021-09-25 | 58 | PTL | BA01WM260 | LION | 0 | 15.51 V | 69 | 100 | 0 | clean |
| 2 | **2024-10-22** | **3475** | PTL | BA01WM260 | **5129** | **0x4307** | 15.27 V | 35 | NACK | 0 | clean |
| 3 | 2021-09-25 | 58 | PTL | BA01WM260 | LION | 0 | 15.61 V | 71 | 99 | 1 | clean |
| 4 | 2021-09-25 | 58 | PTL | BA01WM260 | LION | 0 | 15.44 V | 64 | 99 | 1 | clean |
| 5 | 2021-09-25 | 58 | PTL | BA01WM260 | LION | 0 | **16.29 V** | **100** | 99 | 1 | clean |

**Summary**:
- 4 packs from 2021-09-25 PTL batch (#1, #3, #4, #5) -- all serial 58, indistinguishable
  by SBS metadata. Distinguishable only by current charge state and minor opStatus bits.
- 1 pack from 2024-10-22 PTL batch (#2) -- newer firmware, different chemistry code,
  exposes ChipType/FW/HW MAC reads but NACKs SoH/ChargingV/I/MfgStatus
- All 5 packs are CLEAN (no PF, no Safety trips) -- safe for any service operation
- All 5 are SEALED -- no MAC subcommands beyond identification work

## Next-step recommendation

Now that we have read-only baselines, the safe-write plan I'd suggest:

### Stage 1 -- low-risk write tests (any pack)
1. **Unseal+FAS test** -- verify keys 0x7EE0/0xCCDF + 0xBF17/0xE0BC actually transition
   the BMS through Sealed -> Unsealed -> FullAccess. Check by reading opStatus before/after
   to see SEC bits change.
2. **Re-Seal** -- write seal MAC, verify back to SEC=03.

### Stage 2 -- compare sealed vs unsealed reads on the SAME pack
On battery #5 (fully-charged, balanced), capture full snapshot, then
unseal+FAS, capture again. Compare:
- ChipType / FW / HW (currently NACK on 2021 batch) -- should populate
- absoluteSOC (currently 70% anomaly) -- should normalise to ~95%
- DAStatus1 (works on #1/#3/#5 sealed, NACKs on #2) -- compare
- packVoltage_mV (currently 0 from DAStatus1) -- should populate

### Stage 3 -- destructive ops on a controlled pack
**Pick #4** (control pack -- same batch as #1/#3 but distinguishable by
voltage), and run:
1. clearPFProper() -- since PF is already 0, expect "clean -> clean"
2. resetCycles() -- verify cycles 1 -> 0
3. clearBlackBox() + resetLifetimeData() -- verify no error

After each, take a fresh snapshot and diff against baseline.

### Stage 4 -- battery-rejuvenation on a "tired" pack
None of these 5 packs is genuinely worn (all SoH 99-100%). Skip Stage 4.

### Stage 5 -- capacity edit (most invasive read-write op)
SKIP for now -- no pack needs it, and a wrong value might trigger BMS
self-shutdown. Defer until we have a confirmed-broken Mavic-3 pack we
can use as a sacrificial test target.

### NOT TO DO
- Patch.bin firmware flash -- experimental, untested, could brick BQ chip.
  Skip entirely on these 5 packs.

---

## Battery #6 -- "спарка" = "сборка" = custom-built pack

**Slang note**: "спарка" in user's vocabulary means "custom assembly"
(short for "сборка"), NOT the DJI Spark drone. This is a **custom build:
Mavic 3 BMS PCB + externally soldered 4S LiHV cells (~10 Ah)**. The BMS
still reports BA01WM260 (Mavic 3 codename) because that's literally what
the BMS firmware says -- it doesn't know what cells are wired to it.

This kind of build is popular for getting longer flight time on Mavic 3
without paying for OEM accessories. The catch: BMS thinks the pack is
stock 5 Ah by default, so DataFlash needs to be edited (DesignCapacity
+ per-cell QMax) for the BMS to learn correctly. On this pack, that's
already been done -- DesignCap shows 10400 mAh.

### Identification
- Manufacturer: PTL | DeviceName: BA01WM260 (claims Mavic 3 codename)
- Chemistry: `"7093"` (numeric like #2 -- non-standard)
- Serial: 1009 | Mfr Date: 2025-06-08 (most recent)
- Cells: 4 (NOT 3 like real Spark would be)
- ChipType / FW / HW: 0/0/0 (sealed-NACK, same generation as 2021 batch)

### Why it's NOT a real Spark battery
- Real DJI Spark battery: 11.4V nominal 3S 1480 mAh
- This pack: **15.4V design, 17.6V charging, 4S, 10400 mAh**
- 4 cell readings present (Spark has only 3)

### Why it's a CUSTOM high-cap pack
- DesignCap 10400 mAh = 2x stock Mavic 3 (5000 mAh)
- Cells reading 4.32 V each at "fully charged" (display 92%)
- Charging voltage 17.6 V / charging current 9 A
- 17.6V / 4 cells = **4.4 V per cell** = **LiHV chemistry** (Lithium HV
  cells, allowed to charge to 4.35-4.4 V vs standard 4.20 V)
- Pack at 17.304 V (= 4 x 4.326) with 92% SOC -- consistent with LiHV
  partially charged

This is a third-party "extended flight time" Mavic 3 pack. Manufacturer
took a Mavic 3 BMS PCB, soldered higher-capacity LiHV cells, and adjusted
DesignCapacity DataFlash to 10400 mAh. From the drone's POV it looks like
a stock Mavic 3 pack with extended capacity.

### Live readings
- Pack: 17.304 V (= 4 x 4.326)
- SOC display 92% / Absolute 92% (matches -- no anomaly here)
- Capacity 9556 / 10400 mAh full (design 10400) | wear 0%
- StateOfHealth: 0 (NACK) | Temp: 24.1 C | Cycles: 0
- Status reg 0xC0 = INIT DSG

### The CRITICAL safety issue: status registers all NACK

- **opStatus = 0xFFFFFFFF (NACK)**
- **mfgStatus = 0xFFFFFFFF (NACK)**
- **pfStatus = 0xFFFFFFFF (NACK)**
- safetyStatus = 0x0 (works)
- chargingStatus / gaugingStatus -- not populated in JSON (separate snapshot bug)

This means we have NO IDEA what the actual PF / operation state is on this
pack. Our Python tool said "Has any PF: NO (clean)" -- but that's based
on the C++ check `pfStatus != 0 && pfStatus != 0xFFFFFFFF` which treats
the NACK sentinel as "no PF". That's a SAFETY BUG.

### Code-improvement notes from this pack

**19. CRITICAL: distinguish "no PF" from "couldn't read PFStatus"**

In `dji_battery.cpp:440`:
```cpp
info.hasPF = (info.pfStatus != 0 && info.pfStatus != 0xFFFFFFFF) ||
             (info.djiPF2 != 0 && info.djiPF2 != 0xFFFFFFFF);
```

This treats `0xFFFFFFFF` as "no PF". It should be three states:
- `pfStatus == 0`         -> "no PF, confirmed"
- `pfStatus == 0xFFFFFFFF` -> "could not read, UNKNOWN"
- `pfStatus == anything else` -> "PF active"

**Action**:
- Add `BatteryInfo::pfStatusKnown` bool (false if read NACKed)
- Same for safetyStatus, opStatus, mfgStatus, djiPF2
- UI should display "?" or "unknown" for unknown reads, NOT "clean"
- Snapshot JSON should include `*Known` flags for client-side handling

**20. LiHV detection**

When chargingVoltage / cellCount > 4.30, the pack uses LiHV chemistry.
Add this detection so UI can warn user that:
- Charging this pack with a regular 4.2V/cell charger will under-charge
- Discharging below LiHV cutoff (3.4V instead of 3.0V) is more important
- BMS firmware is NOT stock DJI -- has custom voltage thresholds

**21. Custom-capacity pack detection**

When designCap_mAh > 1.5 x known stock capacity for the model, it's
a custom pack. Show a warning in UI: "This appears to be a custom
extended-capacity pack. BMS PF thresholds may differ from stock --
service operations may have different effects."

**22. PTL-2025-06 batch reuses 2021 batch firmware (sealed-NACK style)**

Battery #6 (mfr 2025-06) has the SAME firmware behaviour as the 2021
batch (#1, #3, #4, #5):
- ChipType / FW / HW MAC reads NACK while sealed
- ChargingV/I / SoH partially work
- Cell voltages real (not synthesised like #2 2024 batch)

So PTL batches alternate between "old firmware" and "new firmware":
- 2021-09 batch: old firmware
- 2024-10 batch: new firmware (synthesised cells, MAC reads work)
- 2025-06 batch: BACK to old firmware (same NACK pattern as 2021)

This is unusual -- normally newer = improved firmware. Maybe PTL
went back to old firmware because it was more compatible with
DJI hosts? Worth investigating.

### Faults
- PFStatus: 0xFFFFFFFF -- UNKNOWN (NACK), our tool wrongly says clean
- SafetyStatus: 0x0 -- truly clean (this read worked)
- DJI PF2: not exposed in JSON (snapshot bug)

### Verdict for #6
- Custom 4S 10Ah LiHV extended-range pack in Mavic 3 BMS shell
- ~92% charged, cells well balanced (10 mV)
- **DO NOT run destructive ops on this pack until we confirm PFStatus
  reads correctly** -- the NACK on PFStatus makes us blind to whether it
  has latched faults
- After a Stage 1 unseal+FAS test on this pack, status reads should
  start working -- THEN we can re-assess safety
- This is also a great test pack for the LiHV / custom-capacity detection
  features we'd add to firmware

### Updated cross-pack matrix

| # | Date | Cells | Cap | LiHV | PF read OK? | Safe for write ops |
|---|---|---|---|---|---|---|
| 1 | 2021-09 | 4 | 5000 | no | YES (= 0) | YES |
| 2 | 2024-10 | 4 | 5000 | no | YES (= 0) | YES |
| 3 | 2021-09 | 4 | 5000 | no | YES (= 0) | YES |
| 4 | 2021-09 | 4 | 5000 | no | YES (= 0) | YES |
| 5 | 2021-09 | 4 | 5000 | no | YES (= 0) | YES |
| 6 | 2025-06 | 4 | **10400** | **YES** | **NO (NACK)** | **NO** -- need unseal first |

---

## Stage 1 results -- unseal verification on Battery #4 (PTL 2021)

Tested 9 key variants via `/api/batt/diag?unseal=` and PEC endpoint. ALL
returned "still sealed". SEC bits in opStatus stayed at 0x03 throughout.

| Key set | Method | opStatus after | Result |
|---|---|---|---|
| RUS_MAV 0x7EE0/0xCCDF | non-PEC | 0x1008305 | "no i2c" (transient) |
| RUS_MAV swap | non-PEC | 0x1008301 | "still sealed" |
| FAS keys 0xBF17/0xE0BC | non-PEC | 0x1008301 | "still sealed" |
| FAS swap | non-PEC | 0x1028301 | "still sealed" |
| TI default 0x0414/0x3672 | non-PEC | 0x1008305 | "still sealed" |
| TI swap | non-PEC | 0x1028307 | "still sealed" |
| DJI Battery Killer 0x4DF6/0x9F44 | non-PEC | 0x1028307 | "still sealed" |
| RUS_MAV | PEC | 0x1008305 | "still sealed" |
| TI default | PEC | 0x1028307 | "still sealed" |

**Critical conclusion**: PTL 2021 batch packs use unseal keys/protocol that
DIFFER from what the Russian commercial battery-service tool uses.

### Hypotheses for PTL 2021 batch

**A. Different static key pair we haven't tried**

PTL might use yet another key pair. Possible sources:
- DJI Battery Killer source has a longer key list -- check
  `research/dji_battery_killer/` for more candidates
- Per-batch key derived from manufacture date / serial?
- Per-pack key in DataFlash that's only readable when unsealed (chicken-and-egg)

**B. HMAC-SHA1 challenge-response (BQ40Z80 newer firmware)**

BQ40Z80 supports two unseal modes:
1. Two 16-bit MA writes (what we tried)
2. HMAC-SHA1 challenge-response: host requests challenge -> BMS replies
   with 20 bytes -> host computes HMAC-SHA1(secret_key_32B, challenge) ->
   writes 20-byte digest -> BMS verifies

We have `unsealHmac()` implemented but no key documented for PTL clones.
TI's default HMAC key is `0123456789ABCDEFFEDCBA9876543210` (16 bytes)
applied as sk[0..15] = key, sk[16..31] = key reversed (or some variant).
PTL may use this default OR their own.

Worth trying: implement an HTTP endpoint to call `unsealHmac()` with
candidate keys.

**C. Different bus-level protocol (no actual writes landing)**

The opStatus DOES change between attempts (lower bits flip), so the writes
DO land on the BMS. The BMS just doesn't recognise them as valid unseal
attempts. So this hypothesis is unlikely.

**D. Lockout after multiple failed attempts**

BQ40Z80 has an unseal-attempt lockout: after N (default 5) failed unseal
attempts, the BMS goes into a 30-second cooldown OR a permanent block.
We made 9 attempts back-to-back -- if there's a lockout, we may need to
wait or power-cycle the pack.

After our test, snapshot still works (basic SBS reads OK) -- so it's not
permanent block. May or may not be a 30-second cooldown.

### Code-improvement notes from this stage

**23. Add `/api/batt/mavic3/try_keys` endpoint that iterates a key catalog**

Right now we use one key set in `unseal()`. Need an endpoint that takes
a list of keys, tries each, reports which (if any) succeeds. Also
respects rate-limiting to avoid lockout.

**24. Add rate-limiting to unseal attempts**

`unsealWithKey()` should:
- Track timestamps of recent unseal attempts
- After 4 failed attempts in 60 seconds, sleep 35 seconds before next
- Otherwise BMS may permanently lock and require power cycle

**25. Add HMAC unseal HTTP endpoint**

`unsealHmac()` exists in code but no HTTP wrapper. Add:
```
POST /api/batt/mavic3/unseal_hmac?key=<32-hex-bytes>
```
Returns the challenge bytes + computed digest + result.

**26. Add `bmsLockoutDetected` field to BatteryInfo**

Detect when BMS is in unseal-attempt cooldown (writes ACK but reads
NACK transiently) and surface to UI so user knows to wait.

**27. Document that commercial-tool keys don't universally work**

Update REPRODUCIBLE_ALGORITHMS.md and reference_dji_mavic3_battery_keys.md
to note that the recovered 0x7EE0/0xCCDF + 0xBF17/0xE0BC keys work on the
COMMERCIAL TOOL'S TARGET BATCH, not necessarily all PTL Mavic 3 clones.
PTL 2021 batch confirmed NOT to accept these keys.

### Verdict for Stage 1
- **Stage 1 BLOCKED**: cannot unseal PTL 2021 packs with any known static key
- BMS is responsive (writes ACK, reads work), just doesn't accept any of our keys
- Either need to:
  - Find PTL-specific key (likely buried in some Russian Telegram forum)
  - Implement HMAC challenge-response and try various 32-byte secrets
  - Sniff a working tool's I2C traffic to extract the actual key in use
- **Stage 2/3 also blocked** until unseal works

### Recommended pivot
- Try Stage 1 on Battery #2 (2024 PTL batch) -- different firmware, might
  accept different keys (or might accept the keys we already have, since
  it's "newer firmware")
- Try Stage 1 on Battery #6 (2025-06 спарка) -- might use 2024-batch keys
  even though firmware-behavior matches 2021 batch

---

## Stage 1 RETRY -- Battery #2 (PTL 2024) -- via NEW try_keys endpoint

### Result: SUCCESS on first attempt!

```
POST /api/batt/mavic3/try_keys
{
  "ok": true,
  "attempts": 1,
  "lockedOut": false,
  "w1": "0x7ee0",
  "w2": "0xccdf",
  "description": "RUS_MAV / commercial-tool Mavic 3",
  "operationStatus": "0x4e200",
  "sec": 2,
  "state": "Unsealed"
}
```

**The RUS_MAV keys 0x7EE0/0xCCDF DO unseal PTL 2024 batch packs.** Confirmed
on first attempt -- no rate-limit issues.

### Big takeaway: PTL key rotation between batches

| Batch | Unseal keys | Source |
|---|---|---|
| **PTL 2024** | **0x7EE0 / 0xCCDF** ✓ confirmed working | RUS_MAV / commercial tool |
| **PTL 2021** | UNKNOWN | none of our 8 catalog keys work |
| **PTL 2025-06 (#6)** | TBD | not yet tested |
| Genuine DJI | TBD | no genuine pack to test on |

The Russian commercial battery-service tool (which we reverse-engineered)
specifically targets PTL 2024 batch (or a closely related variant). PTL
rotated their keys between 2021 and 2024 batches, leaving our recovered
keys non-universal.

### Surprise finding: even after Unseal, most reads STILL NACK

Snapshot after Unseal on #2:
- **stateOfHealth**: 65535 (NACK) -- still
- **mfgStatus**: 0xFFFFFFFF (NACK) -- still
- **chargingV/I**: 0/0 (NACK) -- still
- **packV (DAStatus1)**: 0 -- still
- **cells**: 3818,3817,3818,3818 -- STILL synthesised even when Unsealed!
- **djiSerial**: empty -- never read attempted (PTL not in DJI mfr list)

The 2024 PTL firmware is more locked-down than 2021: unsealing alone
doesn't open up additional registers. Likely requires FullAccess.

### Surprise finding: FAS keys 0xBF17/0xE0BC DON'T work to transition to FullAccess

`/api/batt/mavic3/unlock` runs unseal + FAS + auth-bypass.
- Unseal: succeeded (state went Sealed -> Unsealed) ✓
- FAS attempt with keys 0xBF17 then 0xE0BC: **did NOT transition Unsealed -> FullAccess**
- Final state: still Unsealed (sec=2)

So our "FAS keys" recovered from commercial tool firmware aren't actually
the FullAccess transition keys, OR PTL 2024 doesn't have a separate FAS
state, OR we're sending them wrong (wrong register / wrong order / wrong
length).

### Surprise finding: MAC 0x0030 = SEAL, not Black Box clear

When I called `/api/smbus/xact?op=macCmd&data=0x0030` (which is what our
`DJIBattery::seal()` sends as MAC_SEAL_DEVICE), pack sealed itself
(sec went 2 -> 3). So 0x0030 IS the Seal command on PTL 2024.

But in COMPLETE_FEATURE_SPEC.md from our reverse, I labeled 0x0030 as
"Black Box clear" because that's where it appeared in the commercial
tool's DRAM ritual table.

The actual interpretation is: the commercial tool's PF clear ritual ENDS
with a Seal command (MAC 0x0030) to put the pack back in production state
after the destructive ops. That's good practice -- our `clearPFProper()`
should also re-seal at the end. And `DJIBattery::clearBlackBox()` is
buggy: calling it would actually re-seal the pack, not clear the event
log. Need to remove or redefine.

### Code-improvement notes from Stage 1 retry

**28. Add PTL to recognized DJI manufacturers**

`detectDeviceType()` checks for "DJI" or "TEXAS" in mfr string. Should
also accept "PTL" (and "PACIFIC TECH") since those are DJI clone
manufacturers and we want to attempt djiSerial / djiPF2 reads on them.

```cpp
if (mfr.indexOf("DJI") >= 0 || mfr.indexOf("TEXAS") >= 0 ||
    mfr.indexOf("PTL") >= 0 || mfr.length() == 0) {
    return DEV_DJI_BATTERY;
}
```

**29. CRITICAL BUG: clearBlackBox() actually seals the pack**

`DJIBattery::clearBlackBox()` calls `SMBus::macCommand(BATT_ADDR, 0x0030)`,
but MAC 0x0030 is Seal, not Black Box clear. Need to:
- Either remove clearBlackBox() entirely
- Or find the actual Black Box clear MAC for BQ40Z80

TI BQ40Z80 reference shows no dedicated "Black Box" subcommand in the
public table. The on-chip event log gets cleared as part of MAC 0x0029
(PFEnable / PF Data Reset), not separately.

Fix: in `clearPFProper()` ritual, the previous "Phase G: Black Box clear"
step actually re-seals the pack. Update the doc + reorder.

**30. UNSEAL_CATALOG should differentiate by FW variant**

Now that we know PTL 2024 needs different keys than PTL 2021, the catalog
should know which keys are likely to work for which fwVariant. Update
`tryAllKnownKeys()` to:
- Detect fwVariant first via snapshot
- Try only keys known to work on that variant
- Fall back to full catalog if variant unknown

**31. Improve unlockForServiceOps() error reporting**

Currently it returns `true` if Unsealed even when FAS failed. The endpoint
returns `state: Unsealed`, but UI users may want to know that FullAccess
was attempted and failed. Add a `fasAchieved` field to the result.

**32. PTL 2024 firmware doesn't seem to have FullAccess state**

After unseal+FAS attempt + auth-bypass, all the "locked-down" reads (SoH,
ChargingV/I, MfgStatus, real cell voltages) STILL NACK. This may mean:
- a. PTL stripped FullAccess support entirely (Unsealed is the deepest state)
- b. The FullAccess keys are different and we don't have them
- c. These reads require a DataFlash subclass change first

Option (a) is consistent with PTL not exposing too much under any state.
Worth one more test: try writing MAC 0x0021 (LearnCycle) and see if SoH
populates after. If it does, the gauge is just unlearned.

### Stage 1 final verdict

| Pack | Unseal works? | Notes |
|---|---|---|
| #1 PTL 2021 | NO | none of our keys match |
| #2 PTL 2024 | **YES** | RUS_MAV keys, first attempt |
| #3 PTL 2021 | NO (twin of #1) | not retested -- would expect same |
| #4 PTL 2021 | NO | tested 9 variants + PEC, all rejected |
| #5 PTL 2021 | NO (twin of #1) | not retested |
| #6 PTL 2025 LiHV custom | NOT YET TESTED | next |

---

## Stage 1 round 2 -- after firmware fixes #28-30

### CRITICAL discovery: readDJIPF2() blocks subsequent unseal attempts

**Note #33 (high-impact!)**

After fixing #28 (PTL recognised as DJI manufacturer), Battery #2 stopped
accepting unseal keys via `tryAllKnownKeys()` -- but the SAME keys still
worked via `/api/batt/diag?unseal=` (direct path). Investigation found:

1. `tryAllKnownKeys()` calls `readAll()` first to detect firmware variant
2. `readAll()` for DEV_DJI_BATTERY runs `readDJIPF2()`
3. `readDJIPF2()` uses `macBlockRead(0x4062, ...)` which writes:
   ```
   addr=0x0B reg=0x44 byte_count=2 sub_lo=0x62 sub_hi=0x40
   ```
4. PTL BMS interprets this as a MALFORMED auth-bypass attempt (the real
   auth-bypass is `MAC 0x4062 + 4-byte magic 0x67452301`, NOT just the
   subcommand alone)
5. PTL BMS blocks subsequent unseal attempts for ~30+ seconds (anti-tamper)

**Symptom**: try_keys after readAll() consistently fails on first attempt
(rate-limit hit at 4 attempts), but direct unseal works fine.

**Fix**: in `tryAllKnownKeys()`, replace `readAll()` with a minimal variant
detection that ONLY reads:
- ChipType MAC (harmless -- standard SBS)
- Cell voltages (harmless -- standard SBS)
- Pack voltage (harmless)

Avoid all DJI-specific MAC reads (`readDJIPF2`, `readDJISerial`) before
attempting unseal. After unseal succeeds, full `readAll()` works as
expected -- the BMS only blocks during sealed state.

**Code-improvement note #33**: Make this guard explicit -- maybe add a
`readBasic()` function that does only standard SBS reads (no DJI extras),
and use it whenever we're about to attempt unseal/destructive ops.

### Verification after fix

| Pack state | Action | Result |
|---|---|---|
| Sealed (fresh from seal cmd) | `POST /api/batt/mavic3/try_keys` | ok:true, attempts:1, key=RUS_MAV |
| Unsealed | `POST /api/batt/mavic3/try_keys` | (no-op, already unsealed) |
| Sealed | `POST /api/batt/diag?unseal=0x7EE0,0xCCDF` | ok, sealed:false |

All paths now consistent.

### Updated cross-pack matrix

| # | Date | Pack | dev | mfr | djiSerial | unseal works | notes |
|---|---|---|---|---|---|---|---|
| 1 | 2021-09 | 5 Ah Mavic 3 | clone | PTL | (was '4ERPL...' once) | NO | PTL 2021 keys unknown |
| 2 | 2024-10 | 5 Ah Mavic 3 | clone | PTL | 4ERPMANEA3R9KT | **YES** | RUS_MAV first try (post-fix) |
| 3 | 2021-09 | 5 Ah Mavic 3 | clone | PTL | (twin of #1) | (NO, untested) | |
| 4 | 2021-09 | 5 Ah Mavic 3 | clone | PTL | 4ERPL6QEA1D71P | NO | 9 keys tried, all rejected |
| 5 | 2021-09 | 5 Ah Mavic 3 | clone | PTL | (twin of #1) | (NO, untested) | |
| 6 | 2025-06 | 10 Ah LiHV custom | custom | PTL | TBD | TBD | NEXT TEST |




