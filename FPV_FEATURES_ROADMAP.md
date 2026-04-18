# FPV MultiTool — Feature Roadmap

Candidate features for future sprints. Each includes rationale, scope
estimate, and implementation sketch so they can be picked up by anyone.

## Already implemented

- Servo PWM tester (500-2500μs, 50/330Hz)
- Motor DShot tester (DShot150/300/600)
- ELRS receiver flasher (USB/web upload → ROM bootloader)
- CRSF/ELRS telemetry decoder (live channels, RSSI, param tree)
- USB2TTL transparent bridge
- **RC Protocol Sniffer** (SBUS/iBus/PPM auto-detect)
- **BLHeli 4way passthrough** TCP server (skeleton — works for BLHeliSuite32 handshake,
  OneWire commands are stubs)
- Full DJI Battery Lab (see BATTERY_RESEARCH.md)

## High-priority candidates

### A. MSP DisplayPort OSD simulator — ⭐⭐⭐

Most practical follow-on. Betaflight/iNav/ArduPilot use MSP DisplayPort
to paint OSD characters to analog/digital video systems. Our LCD can pretend
to be an OSD and show what FC is drawing — useful for bench-testing without
goggles.

- **Scope**: 1-2 weekends
- **Protocol**: MSP (MultiWii Serial Protocol) over UART — commands 182/183
- **Wire**: connect FC's OSD UART (often UART3) to GPIO 44 RX
- **Render**: map MSP characters to LCD 172×320 with MAX7456 character set
- **Implementation**:
  ```cpp
  // src/fpv/msp_osd.cpp
  void drawChar(int x, int y, uint8_t ch) {
      // Look up 12x18 glyph from MAX7456 ROM
      // Blit to LCD at grid (x*12, y*18)
  }
  ```
- **Why**: free alternative to DJI/Walksnail goggles for testing

### B. ESC telemetry decoder (KISS/BLHeli telemetry) — ⭐⭐⭐

Real-world ESCs broadcast telemetry (RPM, current, voltage, temp) on a
separate UART pin. Decode this and show per-ESC motor health.

- **Scope**: 1 weekend
- **Protocol**: Each ESC sends 10-byte frame at 115200 baud 8N1 every 50ms
- **Frame**: [temp, voltage_hi/lo, current_hi/lo, capacity_hi/lo, eRPM_hi/lo, crc]
- **Wire**: Connect ESC telemetry wire to GPIO (shared UART with tx mode)
- **Implementation**: similar pattern to RCSniffer, add `src/fpv/esc_telem.cpp`

### C. Betaflight/iNav MSP config client — ⭐⭐

Act as a mini BFConfig — read/edit rates, PIDs, modes via MSP.

- **Scope**: 1 week (MSP command catalog is large)
- **Protocol**: MSP v1 or v2 over UART/TCP
- **Commands of interest**: MSP_PID, MSP_RC_TUNING, MSP_STATUS, MSP_BOXIDS, ...
- **UI**: web form to read/write, live status strip
- **Why**: bench-tune FC without laptop

## Medium priority

### D. SBUS/iBus/CRSF protocol bridge — ⭐⭐

Take input in one RC protocol, output in another.

- **Scope**: 1 weekend (protocol framing already done in RCSniffer)
- **Use-case**: old FrSky TX with SBUS FC that wants CRSF to mix newer goggles/RX
- **Config**: web form — "input=SBUS, output=CRSF, channel mapping = identity"
- **Implementation**: RCSniffer input → channel buffer → TX task emitting output protocol

### E. MAVLink sniffer/simulator — ⭐⭐

For ArduPilot/PX4 drones. Different from CRSF path, heavier protocol.

- **Scope**: 2 weekends (MAVLink XML parsing nontrivial)
- **Use pymavlink header generation**: generate C struct defs from mavlink.xml
- **UI**: live display of HEARTBEAT, SYS_STATUS, ATTITUDE, GPS, SERVO_OUTPUT_RAW
- **Why**: own a MAVLink device? useful. FPV racers probably don't care.

### F. Receiver tester (TX side) — ⭐⭐

Flip the RC sniffer — emit SBUS/iBus frames on GPIO 2 with user-controlled
channel values. Useful for testing RX failsafe, servo range, FC input handling.

- **Scope**: 1 day
- **Implementation**: reuse RCSniffer framers but write to Serial1 TX
- **UI**: 16 sliders → channel values, plus "fire failsafe" button

### G. BLHeli OneWire full implementation — ⭐⭐

Extend current 4way skeleton with actual bit-bang onewire read/write so
BLHeliSuite32 can really read/flash ESC firmware through our board.

- **Scope**: 1 week, hardware testing intensive
- **Why stubbed**: requires precise timing (19.2k baud with half-duplex
  switching in <52μs per bit), plus tolerance for each ESC variant
- **Reference**: Betaflight `src/main/io/serial_4way.c`

### H. PWM generator bank — ⭐⭐

4 channels of simultaneous PWM on 4 GPIOs for bench-testing quad ESC setups.

- **Scope**: 1 day
- **Use LEDC peripheral**: 4 independent channels @ 50/330/500Hz
- **UI**: 4 sliders 1000-2000μs

## Low priority / niche

### I. Video OSD character palette editor — ⭐

Let user preview/edit MAX7456 font glyphs. Useful if you're making custom OSD.

- **Scope**: 2 days
- **UI**: 16x16 grid of 256 chars, click to edit 12x18 bitmap

### J. Sensor simulator (baro/airspeed/GPS) — ⭐

Pretend to be a sensor for FC benchtop testing. Emit fake I²C/UART responses.

- **Scope**: 1 week per sensor family
- **Use-case**: debug FC without risking real flight

### K. Failsafe test harness — ⭐

Systematically induce failsafe conditions (drop frames, bad CRC) and verify
FC behaviour.

### L. KISS/BLHeli32 direct configurator — ⭐

Build full replacement for BLHeliSuite32 inside our web UI. Not passthrough,
direct ESC control.

- **Scope**: 2+ weeks
- **Why**: lightweight alternative, browser-based
- **Risk**: ESC protocol variants are complex

## Concrete quick wins (1-2 hours each)

- **Beacon tone** on Motor tab — send DShot beacon 1-5 commands manually
- **Servo range finder** — sweep and record range to check servo mechanical limits
- **Power supply graph** — log 5V rail voltage over time (detect brownouts)
- **CRSF MODEL_ID selector** — useful for multi-model ELRS bind
- **OTA rollback button** — revert to previous firmware slot (we already have 2 slots)
- **NTP time sync** + RTC display — helpful for labeled log exports
- **Web UI layout resizer** — drag cards to reorder, save in localStorage

## User-requested (none yet)

Open issues / PRs welcome at https://github.com/inlarin/fpv-multitool

---

## Recommended order if I were to continue

1. **G** (BLHeli OneWire real implementation) — finishes the 4way passthrough
2. **A** (MSP DisplayPort OSD) — high visual impact, good demo feature
3. **B** (ESC telemetry) — natural pair with G
4. **F** (RX tester) — easy, reuses sniffer code
5. **D** (Protocol bridge) — niche but practical

Items C/E/L are large undertakings that duplicate existing tools (BFConfig,
QGroundControl, BLHeliSuite) — probably not worth the effort unless you have
a specific need our tool can serve better.
