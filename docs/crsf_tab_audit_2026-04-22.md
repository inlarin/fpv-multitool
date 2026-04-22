# CRSF tab — full audit (2026-04-22)

Companion to [elrs_state_machine_research.md](elrs_state_machine_research.md) and [elrs_flash_audit_2026-04-21.md](elrs_flash_audit_2026-04-21.md).

## TL;DR

- **CRSF tab has unique value** that the ELRS tab can't replace — live telemetry (RSSI / LQ / SNR / channels / battery / GPS / attitude / flight-mode) delivered via WebSocket push every 1 s while `CRSFService` runs in the background.
- **~6 endpoints overlap with ELRS tab** (ping, params read/write, bind, reboot), but with a different architectural model: ELRS endpoints are one-shot probes (acquire Port B, act, release), CRSF endpoints are service-dependent (require `CRSFService::isRunning()`). They coexist, not compete.
- **Broken / fragile**: `/api/crsf/wifi` depends on RX firmware exporting an "Enable WiFi" LUA COMMAND param — absent on some forks, fails 404 with no fallback. `/api/crsf/enter_wifi` analog of the removed `/api/elrs/enable_wifi` is equally architecturally-limited.
- **Port B contention**: CRSFService running → holds Port B → every ELRS-tab probe / flash returns 409. No UI gating; users hit this silently.
- **Works without TX**: Start/Stop, Device Info, Parameters read/write, Bind/Reboot commands. Needs TX: Link Stats, Channels, FC Telemetry.

## Architecture

```
┌─ Web browser ──────────────────────────────────────────────┐
│   7 cards: Start/Stop · LinkStats · Channels · FC telem    │
│            · Device info · Parameters · Commands           │
│   JS: crsfStart/Stop, crsfPing, crsfReadParams, crsfBind,  │
│       crsfReboot, crsfEnterWifi, crsfWriteParam            │
└──────────────────────┬───────────────────┬─────────────────┘
        HTTP REST      │   WebSocket push (1 Hz broadcastTelemetry)
                       │                   │
┌─ Plate web_server.cpp ─────────────────────────────────────┐
│   /api/crsf/{start,stop,state,ping,params,params_list,     │
│               read_params_blind,write,bind,reboot,wifi}    │
│   All acquire PinPort PORT_B · UART                         │
└──────────────────────┬─────────────────────────────────────┘
                       │
┌─ CRSFService (crsf_service.cpp) ───────────────────────────┐
│   loop() ← Serial1 @420000 parse CRSF frames               │
│     FRAME_LINK_STATS (0x14)  → st.link.{rssi,lq,snr,...}   │
│     FRAME_BATTERY_SENSOR    → st.battery                   │
│     FRAME_GPS (0x02)        → st.gps                       │
│     FRAME_ATTITUDE (0x1E)   → st.attitude                  │
│     FRAME_FLIGHT_MODE (0x21)→ st.mode                      │
│     FRAME_RC_CHANNELS (0x16)→ 16 × uint11 channels         │
│     FRAME_DEVICE_INFO (0x29)/PARAM_ENTRY (0x2B) → CRSFConfig│
│   Connection timeout: no frame for 2000 ms → disconnected  │
│   TX: cmdRxBind / cmdReboot / sendDevicePing / PARAM_READ  │
└──────────────────────┬─────────────────────────────────────┘
                       │
┌─ CRSFConfig (crsf_config.cpp) ─────────────────────────────┐
│   Param s_params[64]  — cached with chunked reassembly     │
│   handleParamEntry() reassembles up to 512 B chunks, then  │
│     parses per type (UINT8/INT16/TEXT_SEL/STRING/COMMAND…) │
│   writeParamAuto(id, value_str) auto-dispatches by type    │
│   findCommandParamByName("wifi") for WiFi command trigger  │
└────────────────────────────────────────────────────────────┘
```

## Per-card reference (compact)

| Card | DOM | Endpoints used | Works without TX? |
|---|---|---|---|
| CRSF / Telemetry | `#crsfStatus`, `#crsfConnected` | `/api/crsf/start?inverted=0\|1`, `/api/crsf/stop` | ✓ |
| Link Stats | `#crsfRssi`, `#crsfLQ`, `#crsfLqBar`, `#crsfSnr`, `#crsfRf`, `#crsfPower`, `#crsfDl`, `#crsfFrames` | WebSocket broadcast | ✗ needs full stack |
| Channels | `#channelsGrid` | WS broadcast | ✗ needs RX getting RC |
| FC Telemetry | `#crsfMode`, `#crsfBatt`, `#crsfAtt`, `#crsfGPS` | WS broadcast | ✗ needs FC attached |
| Device Info | `#crsfDevName/Fw/Serial/Fields` | `/api/crsf/ping`, WS | ✓ |
| Parameters | `#crsfParams` (tree) | `/api/crsf/{params,read_params_blind,params_list,write}` | ✓ |
| Commands | — | `/api/crsf/{bind,reboot,wifi}` | ✓ (wifi: only if firmware has param) |

## Overlap with ELRS tab

| Feature | ELRS tab path | CRSF tab path | Model |
|---|---|---|---|
| Device identity | `/api/elrs/device_info` (one-shot DEVICE_PING, no service) | `/api/crsf/ping` + cached via CRSFConfig | One-shot vs async |
| Parameter read | `/api/elrs/params` (blocking, full read + release) | `/api/crsf/params` (async), `/api/crsf/params_list` (cached) | Blocking vs streaming |
| Parameter write | `/api/elrs/params/write?id&type&value` | `/api/crsf/write?id&value` (type from cache) | Explicit vs inferred |
| Bind | `/api/elrs/bind` (works in stub/DFU via wire) | `/api/crsf/bind` (via CRSFService, app-only) | Multi-mode vs app-only |
| Reboot to BL | `/api/crsf/reboot_to_bl` in routes_flash (direct 'bl' frame) | `/api/crsf/reboot` (ELRS-specific 0x0A/0x0B via service) | Multi-mode vs app-only |
| WiFi mode | `/api/elrs/enable_wifi` **REMOVED** (UART-MSP arch. bug) | `/api/crsf/wifi` (via "Enable WiFi" LUA param) | Different mechanism; CRSF path more robust |

## Unique CRSF-tab capabilities (NOT replaceable by ELRS tab)

1. **Live Link Stats** — RSSI1/2, LQ%, SNR, RF mode, TX power, DL stats streaming in real time (1 Hz). Requires full RX + TX link.
2. **Live 16 × RC channels** — 11-bit each, 172..1811 CRSF range, graphical bars.
3. **FC Telemetry** — battery (V/A/mAh/%), GPS (lat/lon/alt/sats), attitude (P/R/Y), flight mode string.
4. **Continuous param cache** — async, doesn't block Port B per-param.
5. **State-aware COMMAND params** — render buttons with PROGRESS/CONFIRM/ABORT lifecycle (CRSFConfig handles the state machine).
6. **Connection timeout indicator** — LINK / NO LINK badge, 2 s timeout heuristic.

## Broken / impossible paths

1. **`/api/crsf/wifi` firmware dependency** — uses `CRSFConfig::findCommandParamByName("wifi")`. If param absent (older/fork firmware), 404. No fallback (e.g. MSP-over-CRSF) because that's the same architectural limitation removed from ELRS tab.
2. **Commands require running app** — bind/reboot/wifi all error if `CRSFService::isRunning() == false`. Can't be issued to stub/DFU/silent RX.
3. **Link Stats / Channels / FC Telem** — silent (zeros / `—` in UI) on plate+RX-only setup. Expected; needs UI clarification that this is normal without a paired TX.

## Port B contention

`CRSFService::begin()` acquires Port B for its entire lifetime. While running:
- `/api/elrs/rx_mode` → 409 busy
- `/api/elrs/device_info` → 409
- `/api/elrs/params` → 409
- `/api/flash/*` → 409
- `/api/otadata/*` → 409
- Every other Port B consumer → 409

Currently **no UI warning** when user switches from running-CRSF to ELRS tab. User sees cryptic "Port B busy" toasts until they realize. This is the highest-value UX fix.

## Testability matrix

| Test | Needs plate | Needs RX | Needs TX | Needs FC |
|---|---|---|---|---|
| Start/Stop button | ✓ | — | — | — |
| Device Info ping | ✓ | ✓ (app) | — | — |
| Read/Write Parameters | ✓ | ✓ (app) | — | — |
| Bind / Reboot commands | ✓ | ✓ (app) | — | — |
| Link Stats populated | ✓ | ✓ | ✓ | — |
| Channels populated | ✓ | ✓ | ✓ | — |
| FC Telemetry populated | ✓ | ✓ | ✓ | ✓ |

## Recommendations (prioritised)

### Do now (P0, high-value UX)

1. **Add "CRSF is running" banner on ELRS tab** — if `crsf.enabled == true` in the WebState broadcast, ELRS tab shows red banner "CRSF telemetry service is active — Port B locked. Click Stop in CRSF tab before using flash/probe actions." Inverse banner on CRSF tab when ELRS flash is in progress.
2. **Auto-pause CRSF during flash operations** — when `WebState::flashState.flash_request == true`, temporarily stop CRSFService, do flash, resume after. Complex but eliminates the whole class of 409 errors.
3. **Clarify "offline" state** — Link Stats / Channels / FC Telem cards show "No TX link detected (plate + RX only)" when disconnected. Currently just "—" with no explanation.

### Investigate (P1)

4. **`/api/crsf/wifi` param-name variation** — some forks call it "Enable WiFi", "WiFi Mode", "Wifi Update". Current code only matches exact "wifi". Extend `findCommandParamByName()` to a case-insensitive substring match.
5. **Parameter cache flush on stop** — `CRSFConfig::reset()` not called on `/api/crsf/stop`. If user switches RX, old params linger. Call reset on stop.
6. **`read_params_blind` default 30** — modern ELRS has 40+ params. Raise to 50.

### Maybe (P2)

7. **Dedup with ELRS tab** — keep CRSF tab for live telemetry only; remove Device Info / Parameters / Commands cards; point users to ELRS tab for those. BUT loses the async param-cache advantage. Probably not worth it.

## File citations

Web UI:
- [web_ui.cpp CRSF tab HTML](../src/web/web_ui.cpp) (search `<div id="tab-crsf"`) — lines 927..1011 approx
- [web_ui.cpp CRSF JS](../src/web/web_ui.cpp) — `crsfStart`, `crsfPing`, `crsfReadParams`, `crsfWriteParam`, `crsfBind`, `crsfReboot`, `crsfEnterWifi`, `handleMsg` WebSocket path

Backend:
- [web_server.cpp `/api/crsf/*` endpoints](../src/web/web_server.cpp) — ~lines 725..888
- [crsf/crsf_service.cpp](../src/crsf/crsf_service.cpp) — service loop, frame parser, TX commands
- [crsf/crsf_config.cpp](../src/crsf/crsf_config.cpp) — param cache + chunked reassembly
- [crsf/crsf_proto.h](../src/crsf/crsf_proto.h) — frame type constants

Related:
- [routes_flash.cpp `/api/crsf/reboot_to_bl`](../src/web/http/routes_flash.cpp) — separate path for "bl" frame, multi-mode
- `routes_flash.cpp` `/api/elrs/enable_wifi` — **removed**, see commit 7d965f6
