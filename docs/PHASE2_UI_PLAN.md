# Phase 2 UI Plan — SC01 Plus touchscreen screens

Synthesized 2026-04-30 from a research pass over EdgeTX, KlipperScreen,
CYD-Klipper, BTT TFT35, openHASP, ISA-101 industrial HMI conventions,
and the LVGL widgets demo.

This is a planning doc — every screen has an ASCII mockup, the widget
choices, and the data sources. Once approved, each screen lands as its
own commit replacing one of the placeholder builders in
`src/board/wt32_sc01_plus/ui/screens_placeholder.cpp`.

---

## Global design rules

These come from the research and apply to every screen.

### Layout chrome (88 px total)

```
┌────────────────────────────────────┐  ─┬─ y=0
│ status bar  [WiFi][IP][batt][beep] │   │  32 px
├────────────────────────────────────┤  ─┤  y=32
│                                    │   │
│         CONTENT  (W=320, H=392)    │   │  392 px
│                                    │   │
├────────────────────────────────────┤  ─┤  y=424
│ HOME  SRV  MTR  BAT  RX SNF CAT SET│   │  56 px (existing tabview)
└────────────────────────────────────┘  ─┴─ y=480
```

Top status bar (32 px) = WiFi icon + IP + connection state pill +
optional inline progress (spinner / bar). Implemented once in BoardApp
and visible on every tab. Bottom 56 px is the existing tabview.

### Color discipline (ISA-101 inspired)

- **Background**: `#101418` (near-black, dark theme — bench tool)
- **Surface**: `#1A1F24` (slightly lighter for cards / panels)
- **Border / divider**: `#394150` (dark slate)
- **Primary text**: `#E0E0E0` (not pure white — reduces glow)
- **Secondary text**: `#A0A0A0` (labels, units)
- **Accent (live data flowing)**: `#2E86AB` teal
- **Healthy / running**: `#06A77D` green — used sparingly, color is for *deviation*
- **Warning**: `#F1C453` amber
- **Alarm / armed / danger**: `#E63946` red — RESERVED for explicit danger states
- **Disabled**: `#555` gray

Do NOT use green for "enabled" toggles — that floods the eye and makes
real alarms invisible. Teal (#2E86AB) for "alive" / "data flowing".

### Typography (built-in Montserrat at LVGL 9.2.2)

| Role | Font | Pixel height | Use |
|------|------|-------------|-----|
| Hero numerics | `montserrat_28`* | 28 | Voltages, RSSI, RPM — read at a glance |
| Section headings | `montserrat_24` | 24 | Tab title, "Cell balance" etc |
| Primary label | `montserrat_18` | 18 | Button labels, list rows |
| Body / values | `montserrat_14` | 14 | Most things |
| Caption / units | `montserrat_10` (built-in) | 10 | "dBm", "ms", timestamps |

\* requires `LV_FONT_MONTSERRAT_28 1` in lv_conf.h (not currently enabled).

### Tap targets

- Primary action: 56 px tall (`Arm`, `Flash`, `Connect`)
- Secondary action: 48 px (toggles, toolbar buttons)
- List row: 48-56 px
- Min spacing: 8 px between tappable elements
- Edge-of-screen safe zone: 4 px

### Danger actions (motor arm, OTA flash, RX flash)

**Two-step gating**, ISA-101 style:
1. **SAFE/ARM latch**: 56 px toggle. Idle gray; armed has red border + red
   accent. Stays red until explicitly disarmed.
2. **Hold-to-confirm action**: only enabled while latch is ARMED.
   Implemented as `lv_arc` (~120 px) wrapping a centered icon — release
   before complete = cancels. Hold time:
   - **Reversible (motor disarm, abort flash)**: 1.0 s
   - **Destructive (RX firmware flash)**: 2.5 s

Color rule: red appears ONLY on armed-state borders + hold-arc fill.
Nowhere else on the screen.

---

## Screen 1: Home / Dashboard

**Goal**: at-a-glance "is everything OK?" view. The screen the user lands on.

```
┌────────────────────────────────────┐  status bar
│ ▣ 192.168.32.51   ↗ -42 dBm    📡 │
├────────────────────────────────────┤
│ ╭───────────╮ ╭───────────╮       │
│ │  PORT B   │ │  WIFI     │       │
│ │   IDLE    │ │ SilentPlc │       │
│ │           │ │  -42 dBm  │       │
│ ╰───────────╯ ╰───────────╯       │
│ ╭───────────╮ ╭───────────╮       │
│ │  BATTERY  │ │ ELRS RX   │       │
│ │  not conn │ │ idle      │       │
│ │           │ │           │       │
│ ╰───────────╯ ╰───────────╯       │
│                                    │
│ uptime  12m 34s                    │
│ heap    58 KB free                 │
│ ota     VALID                      │
│ ip      192.168.32.51              │
│                                    │
└────────────────────────────────────┘
```

### Widget breakdown

- **Status bar (32 px)**: 4 icons + IP. Already part of BoardApp chrome.
- **2x2 tile grid (each 144x100 px)**: Port B mode / WiFi / Battery / ELRS.
  Each tile uses `lv_obj` with `flex` + heading (montserrat_18) + subtitle
  (montserrat_14). Tap → jumps to corresponding tab.
- **System info block**: 4 rows, 14 px font, key:value layout. Updated
  every 1 s via `lv_timer`.

### Data sources

- Port B mode: `PinPort::currentMode()`
- WiFi: `WiFi.SSID()`, `WiFi.RSSI()`, `WiFi.localIP()`
- Battery: `WebState` (via getter — DJI/Autel)
- ELRS: `CRSFService::isConnected()`, last RSSI
- Heap: `ESP.getFreeHeap()`
- OTA: `Safety::otaStateStr()`

---

## Screen 2: Servo

**Goal**: drive a servo with PWM, find its range, run sweep.

```
┌────────────────────────────────────┐
│  Pulse                  1500 µs    │  ← hero numeric (28 px)
│                                    │
│  ┌──────────────●──────────────┐   │  ← lv_slider, 40 px tall
│  └──────────────────────────────┘   │
│   500           1500          2500  │  ← tick markers (10 px)
│                                    │
│  [ -10 ] [  -1 ] [  +1 ] [ +10 ]   │  ← step buttons, 56 px each
│                                    │
│  ┌────────────╮ ┌────────────╮    │
│  │  MARK MIN  │ │  MARK MAX  │     │  ← endpoint capture
│  └────────────╯ └────────────╯     │
│  min: 1100   max: 1900             │
│                                    │
│  ╭────────────────────────────╮    │
│  │  ▶ START SWEEP             │    │  ← 56 px primary action
│  ╰────────────────────────────╯    │
│  freq: ◐ 50 Hz   ○ 330 Hz          │  ← radio buttons (48 px row)
└────────────────────────────────────┘
```

### Widget breakdown

- **Hero pulse readout**: `lv_label` with `montserrat_28`, updates 30 Hz.
- **Slider**: `lv_slider`, range 500-2500, step 1. Knob bumped to 36 px so
  finger taps work.
- **Step buttons**: 4x `lv_button` 56x56, in a flex row.
- **Endpoint markers**: 2x `lv_button` 140x48; labels show captured value.
- **Sweep button**: 56 px primary, color teal when running, gray idle.
- **Frequency selector**: `lv_btnmatrix` with 2 cells, exclusive.

### Data sources / sinks

- Read: `WebState::Servo::pulseUs`, `markedMin`, `markedMax`, `frequency`
- Write: `WebState::Servo::*` (lock-protected via the existing mutex).

---

## Screen 3: Motor

**Goal**: spin a brushless motor via DShot, see ESC telemetry. SAFETY-CRITICAL.

```
┌────────────────────────────────────┐
│            Throttle      0%        │
│                                    │
│  ┌──────────────────────────────┐  │  ← throttle slider DISABLED
│  │ ░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  │     while not armed
│  └──────────────────────────────┘  │
│   0                          2000   │
│                                    │
│  ╭──────────────╮ ╭──────────────╮ │
│  │ ❌  SAFE     │ │      ★       │ │  ← SAFE/ARM latch (56 px)
│  ╰──────────────╯ ╰──────────────╯ │
│   tap to arm    [HOLD 2.5s to disarm]
│                                    │
│  ── ESC TELEM ────────────────     │
│  RPM       0          ▁▁▁▁▁▁▁▁    │  ← sparkline 100 px wide
│  TEMP      —      °C   ▁▁▁▁▁▁▁▁    │
│  CURR      —       A   ▁▁▁▁▁▁▁▁    │
│  VOLT      —       V                │
│                                    │
│  [ direction ▶ CW ] [ 3D mode ○ ]  │
│  [ beep 1 ][ 2 ][ 3 ][ 4 ][ 5 ]    │
└────────────────────────────────────┘
```

### Widget breakdown

- **Throttle slider**: `lv_slider`, disabled (gray) while latch is SAFE.
  When ARMED, throttles ramp via `MotorDispatch` to avoid step-change jolt.
- **SAFE/ARM latch**: large 50/50 split. Idle: text "SAFE", border green.
  Tap → instantly arms (status bar shows red ARMED pill globally).
  When armed: hold-to-disarm arc (1.0 s) on right.
- **ESC telemetry**: 4 rows of label + value + 64 px sparkline. Sparkline
  uses `lv_chart` with 60-sample circular buffer, update every 50 ms.
- **Direction / 3D / beep**: secondary controls below telemetry.

### Critical safety behaviors

- Slider locked at 0 when latch is SAFE (can't pre-set throttle).
- Disconnecting Port B (mode change in another tab) → instant disarm,
  red toast in status bar.
- ESC telem temperature exceeds threshold → status bar warning.
- WDT panic during armed = motor naturally spins down (DShot timeout
  interpreted by ESC as "no signal").

---

## Screen 4: Battery (multi-page)

**Goal**: read DJI/Autel batteries. Several views packed onto one tab via
horizontal `lv_tileview` swipes (or top-level pill nav inside the tab).

```
PAGE 1 — Quick read                    PAGE 2 — Cells (balance)
┌────────────────────────────────────┐  ┌────────────────────────────────────┐
│  ◀ ●○○○                             │  │  ◀○●○○                             │
│                                    │  │                                    │
│      24.6 V                         │  │  Pack imbalance:    18 mV ✓        │
│      ━━━━━ 87%                      │  │                                    │
│                                    │  │  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕            │
│  cycles    12                       │  │  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕            │
│  health   100%                      │  │  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕            │
│  temp     24°C                      │  │  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕            │
│  vendor   DJI                       │  │  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕  ▕▕            │
│  serial   84A...                    │  │ 4.10 4.09 4.11 4.12 4.10 4.09     │
│                                    │  │  c1   c2   c3   c4   c5   c6      │
│  [refresh]   [reset cycles ⚠]      │  │                                    │
└────────────────────────────────────┘  └────────────────────────────────────┘

PAGE 3 — Forensics (DF dump)            PAGE 4 — Catalog (DJI MAC lookup)
┌────────────────────────────────────┐  ┌────────────────────────────────────┐
│  ◀○○●○                             │  │  ◀○○○●                             │
│  Data flash dump                    │  │  Library                           │
│  block ◀ 0x0000 ▶                  │  │                                    │
│  00 11 22 33 44 55 66 77            │  │  Tap a profile to compare:         │
│  88 99 AA BB CC DD EE FF            │  │  ┌────────────────────────────╮   │
│  ...                                │  │  │ DJI Mavic 2 Pro 3850mAh    │   │
│                                    │  │  │ TB55 / 22.05 V             │   │
│  [save to SD] [hex/ascii]           │  │  ╰────────────────────────────╯   │
└────────────────────────────────────┘  └────────────────────────────────────┘
```

### Widget breakdown

- **Tileview** (`lv_tileview`) with 4 horizontal pages, swipe to navigate.
- Page header has small dot pagination (●○○○).
- **Quick read**: hero voltage (28 px) + bar (8 px, color by SOC) + 5-row
  key/value list. "Reset cycles" button is hold-to-confirm 2.5 s.
- **Cells**: 6x `lv_bar` vertical, 24 px wide each, color green/amber/red
  by deviation from pack mean. Hero numeric: imbalance in mV.
- **DF dump**: hex grid with arrows for block navigation. `lv_textarea`
  read-only. Save-to-SD button writes to `/sd/df_<serial>.bin`.
- **Catalog**: `lv_list` with tap → detail view. Data from
  `data/dji_battery_catalog.json` on SD.

### Data sources

- DJIBattery / AutelBattery / clones — same APIs as web.
- All via `WebState::Battery` mutex.

---

## Screen 5: ELRS RX

**Goal**: monitor link, bind, flash firmware. The catalog flasher's other half.

```
┌────────────────────────────────────┐
│ Link Monitor                        │
│                                    │
│ ┌──────────────╮ ┌──────────────╮  │
│ │   LQ          │ │   RSSI       │  │
│ │   95          │ │  -42         │  │  ← 28 px hero
│ │   %           │ │  dBm         │  │
│ ╰───━━━━━━━━━━╯ ╰───━━━━━━━━━━╯  │  ← 8 px bar
│                                    │
│ ┌──────────────╮ ┌──────────────╮  │
│ │   SNR         │ │   PKT/s      │  │
│ │   12          │ │   500        │  │
│ │   dB          │ │              │  │
│ ╰───━━━━━━━━━━╯ ╰───━━━━━━━━━━╯  │
│                                    │
│ Channels (1-8)                      │
│ ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  │  ← mini bars
│ 1   2   3   4   5   6   7   8       │
│                                    │
│  ╭──────────╮ ╭──────────╮ ╭──────╮│
│  │  BIND   │ │ FLASH... │ │ INFO ││
│  ╰──────────╯ ╰──────────╯ ╰──────╯│
└────────────────────────────────────┘
```

### Widget breakdown

- **2x2 hero metric grid**: each cell is a card with label + 28 px value +
  unit + colored bar. Color by ELRS thresholds (LQ green >70, amber 50-70,
  red <50; RSSI green >-80, amber, red <-95).
- **Channel strip**: 16 vertical mini-bars (or two rows of 8 to fit width).
  Each 16 px wide x 60 px tall.
- **Action buttons**: BIND (hold 1 s to enter 60 s bind window),
  FLASH (jumps to Catalog tab pre-filtered), INFO (modal with chip / MAC /
  flash layout).

### Data sources

- `CRSFService` running, RSSI/LQ/SNR streamed at 50 Hz from RX.
- Channel values from CRSF channel frame.
- Update only when values change to save redraw cost.

---

## Screen 6: RC Sniff

**Goal**: SBUS/iBus/PPM frame sniffer. Pure read-only diagnostic.

```
┌────────────────────────────────────┐
│  Mode  [auto ▼] [SBUS] [iBus] [PPM]│  ← toggle row
│                                    │
│  Detected: SBUS @ 100 kbps inv     │
│  Frame rate: 96 Hz                  │
│  CRC errors: 0   Lost: 0   FS: ✓   │
│                                    │
│  Channels                           │
│ ┌────────────────────────────────┐ │
│ │ 01  ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  1500    │ │
│ │ 02  ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  1500    │ │  ← horizontal mini-bars
│ │ 03  ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  1500    │ │     1 row each, 16 px
│ │ 04  ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  1500    │ │
│ │ 05  ▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕▕  1500    │ │
│ │ ... scroll to see 6-16 ...      │ │
│ └────────────────────────────────┘ │
└────────────────────────────────────┘
```

### Widget breakdown

- **Mode toolbar**: `lv_btnmatrix` 4 cells, exclusive selection.
- **Detection / status panel**: 3-row info block, font 14 px.
- **Channel list**: scrollable `lv_obj` with 16 child rows, each row =
  channel# + horizontal mini-bar + numeric value. List scrolls if can't
  fit 16 rows in 280 px height.

---

## Screen 7: Catalog flasher

**Goal**: pick a firmware preset → flash to connected RX. The original raison d'être.

```
┌────────────────────────────────────┐
│  Catalog                            │
│  Filter: [ search... ]              │  ← lv_textarea search
│                                    │
│ ┌────────────────────────────────┐ │
│ │ 📦  Bayck RC C3 Dual            │ │  ← 56 px row, tappable
│ │     ELRS 3.5.3   2.4 GHz   1MB │ │     icon + 2-line text
│ ├────────────────────────────────┤ │
│ │ 📦  HappyModel EP1              │ │
│ │     ELRS 3.5.3   2.4 GHz   1MB │ │
│ ├────────────────────────────────┤ │
│ │ 📦  Radiomaster RP3              │ │
│ │     ELRS 3.5.3   915 MHz   1MB │ │
│ ├────────────────────────────────┤ │
│ │ ... up to 7 visible, scroll ... │ │
│ └────────────────────────────────┘ │
│                                    │
│  Selected: <none>                   │
│  ╭────────────────────────────╮    │
│  │  ▶ FLASH SELECTED          │    │  ← 56 px primary, disabled
│  ╰────────────────────────────╯    │     until selection
└────────────────────────────────────┘

Detail screen (when selected):
┌────────────────────────────────────┐
│  ◀ Back                             │
│  Bayck RC C3 Dual                   │
│                                    │
│  Vendor: Bayck                      │
│  Chip:   ESP32-C3 dual              │
│  Version: ELRS 3.5.3                │
│  Band: 2.4 GHz                      │
│  Size: 1.04 MB                      │
│                                    │
│  Bind phrase                        │
│  ┌──────────────────────────────╮   │
│  │ SilentPlace                  │   │
│  ╰──────────────────────────────╯   │
│  UID will be computed from phrase   │
│                                    │
│  ╭────────────────────────────╮    │
│  │  ◀ HOLD 2.5s TO FLASH      │    │  ← danger action
│  ╰────────────────────────────╯    │
└────────────────────────────────────┘
```

### Widget breakdown

- **Search**: `lv_textarea` filters list by substring.
- **List**: `lv_list` with custom 56 px rows, icon + 2-line text.
- **Detail**: secondary screen via `lv_obj_t` swap (not a separate tab).
  Bind phrase = `lv_textarea` (use existing on-device keyboard from LVGL).
- **Hold-to-flash**: 2.5 s hold-arc, fills with red. Releasing early
  cancels. Once filled, kicks off the existing patch+flash flow over CRSF.
- **Progress**: status bar shows "Flashing... NN%" + spinner during op.

### Data sources

- Catalog: JSON metadata on SD card under `/catalog/index.json` plus
  one `.bin` per preset.
- Bind phrase storage: NVS-backed, last-used remembered.

---

## Screen 8: Settings

**Goal**: device configuration. Single scrollable list.

```
┌────────────────────────────────────┐
│  Settings                           │
│                                    │
│  WiFi                               │
│  ┌────────────────────────────────╮│
│  │ SSID:  SilentPlace             ││ ← lv_list row, tap → editor
│  │                          ▶     ││
│  └────────────────────────────────┘│
│                                    │
│  Display                            │
│  ┌────────────────────────────────╮│
│  │ Rotation: [0] [1] [2] [3]      ││ ← 4 buttons 60x40
│  │ Brightness:  ●━━━━━━           ││ ← lv_slider
│  │ Recalibrate touch          ▶   ││
│  └────────────────────────────────┘│
│                                    │
│  Health beacon                      │
│  ┌────────────────────────────────╮│
│  │ URL: <not set>                 ││
│  │ Interval: 0 min                ││
│  │                          ▶     ││
│  └────────────────────────────────┘│
│                                    │
│  USB Mode                           │
│  ┌────────────────────────────────╮│
│  │ ◐ CDC  ○ CP2112  ○ Vendor      ││
│  └────────────────────────────────┘│
│                                    │
│  About                              │
│  fw v0.28.6+...   ip 192.168.32.51 │
│  uptime 12m   heap 58 KB           │
│  ota_state VALID    boot_count 0    │
│  [factory reset NVS  ⚠]             │
└────────────────────────────────────┘
```

### Widget breakdown

- Each section is a card (16 px padding, `#1A1F24` background, 8 px radius).
- WiFi → tap → modal with `lv_keyboard` for SSID + password entry.
- Rotation: 4 button group, exclusive.
- Brightness: `lv_slider` 0-255, writes via `BoardDisplay::setBrightness`.
- Recalibrate: tap → calls `BoardDisplay::recalibrateTouch()`.
- Beacon: tap → modal for URL + interval entry.
- USB Mode: radio buttons → `UsbMode::set*` (requires reboot, modal warns).
- About: read-only stats + factory-reset (hold-to-confirm 5 s, wipes NVS).

---

## Implementation order (Phase 2 commits)

Roughly easy → hard, plus prioritized by user-visible value:

1. **Status bar chrome** in `BoardApp` (status icons + IP + working pill).
   Needed by every screen. ~1 commit.
2. **Settings** (almost no business logic — exposes things we already
   have). Replaces today's most-painful gap (Serial CLI for WiFi/beacon).
3. **Home dashboard** (read-only, easy).
4. **Servo** (existing servo_pwm has clean API; mostly UI work).
5. **ELRS RX link monitor** (CRSFService existing; live data plumbing).
6. **Battery quick-read** (page 1 only at first; pages 2-4 later).
7. **RC Sniff** (similar to channels view in ELRS).
8. **Motor** (most safety-critical; do AFTER pattern is proven on Servo).
9. **Catalog flasher** (most complex; SD I/O + patcher + progress UX).
10. **Battery forensics + library** (pages 3-4, deep features).

Each commit replaces one placeholder builder, adds the screen's logic,
and gets OTA-tested before moving on.

## Architecture decisions (resolved 2026-04-30)

1. **Catalog format on SD** — JSON manifest per device + raw `.bin`.
   Directory hierarchy: `protocol / vendor / side / device /`:

   ```
   /catalog/
   ├── ELRS/
   │   ├── Bayck/
   │   │   ├── RX/
   │   │   │   └── C3-Dual/
   │   │   │       ├── manifest.json
   │   │   │       └── firmware.bin
   │   │   └── TX/
   │   │       └── ...
   │   └── HappyModel/
   │       └── RX/
   │           └── EP1/  ...
   ├── TLRS/
   │   ├── <vendor>/
   │   │   ├── RX/
   │   │   └── TX/
   ```

   Vendor before side groups all of one manufacturer's devices in one
   place. `manifest.json` schema sketched in the original commit
   message; carries vendor / device / version / chip / band / md5 /
   flash_layout (offset map) / bind_uid (offsets + preset defaults) /
   notes. The patcher in `routes_flash.cpp` already speaks this format
   internally; Phase 3 just externalizes the parameters from code to
   per-device JSON.

2. **Bind phrase**: persist last-entered in NVS, prefill the field on
   next entry, with an explicit "Clear bind phrase memory" in Settings.
   UX target: flashing 5 receivers in a row to bind to one TX -> type
   the phrase once.

3. **Touch keyboard**: use the built-in `lv_keyboard` as-is. Don't
   roll a custom one. The ~10 px keys on 320 px width are tight but
   usable per the LVGL widgets demo.

4. **Status bar refresh**: 1 Hz default for normal stats (uptime,
   heap, RSSI, IP). 100 ms for the inline "working" indicator
   (spinner / progress bar during OTA / flash). RECHECK this once we
   can interact with the device live -- if 1 Hz feels sluggish in
   practice, drop to 500 ms; if it feels noisy, stretch to 5 s.
