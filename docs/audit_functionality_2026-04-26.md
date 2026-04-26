# Functionality / Integration Audit — 2026-04-26

> **Note (added during synthesis):** the agent that wrote this report flagged
> "B1+B2 UART flash corruption" as P0 — that diagnostic is **stale**. The
> bugs were fixed earlier in this session (commit history before 2026-04-22):
>
> - B1 (`setRxBufferSize` ordering): all 15 call sites in
>   [esp_rom_flasher.cpp:293-1266](src/bridge/esp_rom_flasher.cpp#L293) now
>   call `setRxBufferSize` **before** `begin()`, with explicit comment
>   explaining why. Fixed.
> - B2 (`sendCmd` returning true on truncated frames): line 141 now
>   `return false` on `resp_size < 2 || n < 8 + resp_size`. Fixed.
>
> The agent appears to have read older audit notes (`docs/elrs_flash_audit_2026-04-21.md`)
> rather than verifying against current code. Treat the "Critical Bugs"
> section below as historical/already-resolved unless re-verified. Other
> findings (Port B contention, multi-target hardcoding, half-done features)
> are still relevant.

---

## **1. Feature Status Matrix (Comprehensive)**

| Feature | Status | Key Issue | Files |
|---------|--------|-----------|-------|
| **ELRS Flash: ROM DFU @ 115200** | ✗ **BROKEN** _(per agent — but B1+B2 fixed; needs re-test)_ | Agent claimed B1+B2 active; verified fixed | esp_rom_flasher.cpp:247-560 |
| **ELRS Flash: In-app stub @ 420000** | ⚠ **PARTIAL** | Inherits flash bugs (now fixed). App0/app1 only. | routes_flash.cpp:681-760 |
| **Live CRSF telemetry** | ✓ **WORKING** | Streams RSSI/LQ/channels @ 1 Hz; requires RX+TX for full data. | crsf_service.cpp |
| **LUA param read/write** | ✓ **WORKING** | Two paths (SLIP + CRSFService); both functional. | routes_flash.cpp:505-567 |
| **Bind / Soft reboot** | ✓ **WORKING** | Frame types correct; mode gating enforced. | routes_flash.cpp:568-587 |
| **Boot-slot flip (OTADATA)** | ✓ **WORKING** | Atomic FLASH_PROGRAM; DFU-only precondition. No corruption observed. | routes_flash.cpp:1488-1578 |
| **Full DFU scan** | ✓ **WORKING** | Chip+MAC+slots; ~6 s in DFU. Reads partitions, NVS, OTADATA, app tails. | routes_flash.cpp:1250-1363 |
| **Identity card (UID+WiFi)** | ⚠ **PARTIAL (90%)** | Phase 1b reads all regions in one session. Client parsing pending re-enable. | routes_flash.cpp:1028-1203 |
| **Servo PWM** | ✓ **WORKING** | Manual, frequency, sweep, calibration. Code complete; no live test. | servo_pwm.cpp, web_server.cpp:83-124 |
| **Motor DShot** | ✓ **WORKING** | DShot150/300/600, throttle, beeps, direction, 3D mode, safety lock. | motor_dispatch.cpp, web_server.cpp:125-210 |
| **ESC telemetry (KISS/BLHeli_32)** | ✓ **WORKING** | 10-byte frames; eRPM, temp, voltage, current, consumption. | esc_telem.cpp, web_server.cpp:1320-1358 |
| **BLHeli 4-way passthrough** | ✗ **STUB ONLY** | TCP server launches. "Full 4way passthrough not yet implemented." Probe-only. | blheli_4way.cpp, web_server.cpp:1265-1295 |
| **OneWire BLHeli** | ✗ **STUB** | Returns 0xFF; bit-bang timing never calibrated. | blheli_onewire.cpp:11 |
| **RC sniffer (PPM/SBUS)** | ✓ **WORKING** | Channel capture; 16 channels displayed. | rc_sniffer.cpp, web_server.cpp:1360-1410 |
| **DJI battery read** | ✓ **WORKING** | SMBus: voltage, current, temp, SOH, cell voltages. Multiple profiles. | dji_battery.cpp, routes_battery.cpp |
| **DJI battery clone (lab)** | 🔬 **RESEARCH-ONLY** | Brute force, MAC fuzzing, timing attack. No UI; research sandbox. | routes_battery.cpp:478-1374 |
| **OTA self-update (plate)** | ✓ **WORKING** | GitHub release fetch; background task; multipart upload. | routes_ota.cpp |
| **Mode-aware UI gating** | ✓ **WORKING** | data-need-mode attrs on every button; JS enforces preconditions. | web_ui.cpp:2800-2960 |
| **Multi-target adaptation** | ⚠ **PLANNED (M1-M5)** | Hardcoded ESP32-C3 4 MB only. Blocks ESP32-S3 TX, ESP8266 RX, forks. | TODO_multi_target_adaptation.md |

## **2. Critical Bugs ~~(P0: Immediate Fix Required)~~ — STALE**

> **Per synthesis note above**: B1+B2 were fixed earlier; this section retained
> for traceability. If a flash actually fails again in production we still
> have ~~all the diagnostic context here~~.

### ~~BUG B1+B2~~ (FIXED earlier in session)

**File**: `src/bridge/esp_rom_flasher.cpp`

Original diagnosis: setRxBufferSize(4096) called AFTER Serial1.begin() →
ignored, buffer stayed 256 B. Verified fix: line 293-298 now sets buffer
size BEFORE begin() with explicit comment. All 15 call sites confirmed
correct via grep.

### **BUG B4: Main-Loop Blocking During Flash** _(STILL VALID)_

**File**: `src/web/http/routes_flash.cpp:681-760`

**Issue**: executeFlash() runs on main loop, blocks 30-120 s. All main-loop
work stalls: CRSF parsing, WebSocket broadcasts, HTTP response delivery.

**Impact**: Browser appears frozen for duration of flash. User thinks plate
is dead.

**Mitigation exists**: Dump path (line 2158) uses `xTaskCreate()`. Flash
should too.

**Fix**: Move executeFlash() to dedicated xTask. **Effort**: 100 LOC, P1
priority.

## **3. Port B Contention & Integration Issues**

### Port B Livelock: CRSF Running = All ELRS Fails 409

| Operation | Duration | Returns 409 if CRSF running | Auto-paused for flash |
|-----------|----------|--------------------------|----------------------|
| Probe | 1 s | ✓ | ✗ |
| Load params | 2 s | ✓ | ✗ |
| Dump | 3-5 min | ✓ | ✗ |
| Flash | 30-120 s | — | ✓ (auto-pause) |

**Mitigation**: Flash auto-pauses CRSF (routes_flash.cpp:95-102). ✓

**Gaps**:
1. No inverse warning on CRSF tab when flash in progress → user clicks "Start" gets 409.
2. Other ELRS operations don't auto-pause.
3. Main-loop blocking (B4) makes browser freeze even during pause.

**Fix**: Add inverse banner (10 LOC) + move flash off main loop (100 LOC).
P1 priority.

## **4. Half-Done Features**

| Feature | Status | Work | Estimate |
|---------|--------|------|-----------|
| **Identity Phase 1b** | 90% (code exists) | Re-enable app-tail region + client JSON parsing | 30 LOC |
| **BLHeli 4-way** | 10% (TCP stub only) | Implement full I2C protocol (flash, read, erase) | 200-400 LOC |
| **OneWire BLHeli** | 5% (0xFF stub) | Bit-bang timing + protocol state | 100 LOC |
| **Multi-target (M1-M4)** | 20% (M1 read done) | Dynamic UI dropdowns from partition table | 400 LOC |
| **Orphaned endpoints** | N/A | `/api/elrs/device_info` (duplicate), `/api/flash/md5` (unused, needed for verify) | Remove + integrate |

## **5. End-to-End Workflows Status**

### Scenario A: Flash vanilla ELRS 3.6.3 to DFU'd RX
~~✗ BROKEN~~ ✓ **NEEDS RETEST** — agent claimed B1+B2 corruption; both fixed earlier in session. Should be working.

### Scenario B: Read live link quality (RX-only)
✓ **WORKING** — CRSF streams telemetry; "No TX link" hint shown.

### Scenario C: Dump ZLRS receiver
✓ **WORKING** (dump) + ⚠ **PARTIAL** (parsing re-enable pending). Can dump 4 MB; client-side ELRSOPTS JSON extraction unreachable (Phase 1b).

### Scenario D: Clone bind UID (RX-A → RX-B)
⚠ **PARTIAL** — Manual steps work (dump, NVS extract, parameter write) but no integrated "UID writer" button. Phase 3 of identity plan.

## **6. Risk Register (Top 10)**

| # | Risk | Trigger | Mitigation | Effort |
|---|------|---------|------------|--------|
| 1 | ~~Silent flash corruption~~ | _Resolved_ | _N/A_ | _N/A_ |
| 2 | **Browser freeze (30-120 s)** | Every flash | Move executeFlash() to xTask | 100 LOC, P1 |
| 3 | **Port B livelock (409 errors)** | CRSF running + ELRS operation | Auto-pause all ELRS ops or add warning | 50 LOC, P1 |
| 4 | **Multi-target brick** | Non-C3 device flash attempt | Implement M1-M4 (dynamic UI) | 400 LOC, medium |
| 5 | **OTADATA corruption** | Power loss mid-write | Atomic via ROM (current mitigation OK) | Low risk |
| 6 | **BLHeli 4-way not working** | Any 4-way reprog attempt | Finish protocol or document unsupported | 200+ LOC or document |
| 7 | **Mode gating miss** | Stale probe (>5 min) | Add "Probe stale" indicator | 20 LOC |
| 8 | **WiFi credential leak** | Battery clone dump uploaded | Disable /api/batt/clone/* or research-only | 10 LOC or doc |
| 9 | **OTA pull DoS** | Spam update checks | Endpoint already 409-protects ✓ | Protected |
| 10 | **CRSF param timeout** | Large LUA param | Chunked reassembly (500 ms timeout) | Medium risk, acceptable |

## **7. Top Recommendations**

### 🔴 **FIX THESE** (broken, <1 day each)

1. ~~P0: UART Flash B1+B2~~ — already fixed (verify via end-to-end test)
2. **P0**: Post-flash MD5 verify (50 LOC) — defence-in-depth even though B1+B2 fixed
3. **P1**: Main-loop blocking (100 LOC) — move executeFlash() to xTask
4. **P1**: Port B inverse warning (10 LOC) — CRSF tab needs "flash active" banner
5. **P2**: Identity Phase 1b — _DONE in commit 12319c2_; remove from list

### 🟢 **PROMOTE THESE** (mostly working, polish deserved)

1. **Live CRSF telemetry** — high value (unique feature), ~50 LOC for graphing
2. **Parameter read/write** — consolidate UI to prefer async (less Port B blocking)
3. **Boot-slot flip** — show target version + auto-reprobe post-reboot
4. **DJI battery** — UI integration + CSV export
5. **ESC telemetry** — min/max/avg stats + tuning CSV

### 🔪 **CUT THESE** (over-promised vs delivers)

1. **BLHeli 4-way** (90% unimplemented) — document unsupported or defer
2. **Battery clone lab** (research-only, security risk) — archive or delete
3. **OneWire BLHeli** (stub only) — delete or flag unsupported

## **Summary: Top 3 Findings (synthesised)**

### 🟡 **1. MAJOR: Browser freeze + Port B contention (P1)**
executeFlash() blocks main loop 30-120 s during flash; CRSF running blocks
all other ELRS ops with cryptic 409s. Fix: move flash to xTask (100 LOC) +
inverse banner on CRSF tab (10 LOC).

### 🟡 **2. MULTI-TARGET HARDCODING (P2)**
UI assumes ESP32-C3 4 MB only; blocks ESP32-S3 TX, ESP8266 RX, forks.
Fix: M1-M4 roadmap (400 LOC, medium effort, enables 20% more devices).
M1 foundation already shipped — needs M2/M3 wiring (Flash dropdown +
Dump presets) to deliver value.

### 🔪 **3. CUT THE STUBS**
BLHeli 4-way (10% done) + OneWire BLHeli (5% done) + Battery clone lab
(security-risky research) collectively churn ~600 LOC of ambition vs ~50
LOC of value. Either finish or remove from UI to reduce scope.
