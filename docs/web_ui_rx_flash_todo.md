# Web UI TODO — RX slot-flash + OTADATA + WiFi AP probe

Backend endpoints are shipped (`HEAD` after this doc). What the UI still
needs so a user can flash vanilla ELRS into the free slot without touching
the command line.

## 1. New card in **ELRS Flash** tab: "Slot-targeted flash"

Sits between the existing "Flash firmware" and "Dump firmware" cards.

**Fields:**

| Control | Purpose | Default |
|---------|---------|---------|
| File input | firmware.bin | none (required) |
| Slot selector (radio) | app0 / app1 / custom offset | app0 |
| Custom offset input | hex, e.g. `0x10000` | `0x10000` (app0) |
| "Erase partition first" toggle | call `/api/flash/erase_region?offset=N&size=0x1E0000` before flash | on |
| Progress bar | polls `/api/flash/status` during flash | — |
| Warning banner | "RX must be in DFU (hold BOOT, power-cycle)" | always visible |

**Actions:**

1. User selects firmware.bin + slot.
2. POST `/api/flash/upload` (multipart, same as existing flow).
3. If "Erase first" is on: POST `/api/flash/erase_region` with
   `offset=<app0_offset>` (`0x10000` for app0, `0x1F0000` for app1),
   `size=0x1E0000`. Poll status.
4. POST `/api/flash/start` with form field `offset=<chosen>`.
5. Poll `/api/flash/status` until complete.
6. Prompt user: "Flash complete. Use OTADATA card below to select this
   slot as active."

**Wire partition offsets client-side** (inferred from our dump, but make
them detectable via `/api/otadata/status` first-call):

```js
const RX_PARTITIONS = {
  app0: { offset: 0x10000, size: 0x1e0000 },
  app1: { offset: 0x1f0000, size: 0x1e0000 },
  otadata: { offset: 0xe000, size: 0x2000 },
};
```

## 2. New card: "OTADATA / Active slot"

Shows which app slot the RX will boot next and lets the user flip it.

**Fields:**

| Element | Data source |
|---------|-------------|
| Current active slot (read-only) | `GET /api/otadata/status` → `active_slot` |
| Max ota_seq | `status.max_seq` |
| Sector 0 (@0xe000) state | `status.sectors[0]` |
| Sector 1 (@0xf000) state | `status.sectors[1]` |
| Buttons: "Boot app0", "Boot app1" | POST `/api/otadata/select?slot=0|1` |
| Button: "Erase OTADATA (fall to app0)" | POST `/api/flash/erase_region?offset=0xe000&size=0x2000` |
| Power-cycle reminder | "Remove power from RX, re-apply, watch for new WiFi SSID." |

**Caveat** to surface in the card text: if you flashed a MILELRS-family
firmware into app1, that app tends to auto-rewrite OTADATA on each boot
to keep itself active. To make a vanilla-ELRS flip stick, do BOTH:

1. Erase OTADATA **and** write new record pointing to app0.
2. Optionally erase app1 entirely (big red button in Advanced, behind
   a confirm dialog) — this way MILELRS never runs again.

## 3. WiFi AP probe (deferred — not implemented backend-side yet)

**Problem:** We want to test whether the RX's hotspot (e.g.
`MILELRS v3.48 RX 1016`) uses the ELRS default password `expresslrs` or
something custom. Plate would need to briefly disconnect its own STA
connection to attempt STA-connect to the RX's AP.

**Risk:** disrupts the user's web UI session.

**Planned endpoint** (not built yet):

```
POST /api/wifi/probe_ap?ssid=<ssid>&pass=<pass>&timeout=6000
GET  /api/wifi/probe_ap/status
POST /api/wifi/probe_ap/cancel
```

Behaviour:
- Saves current STA creds to RAM.
- `WiFi.begin(ssid, pass)` with 6 s timeout in a background task.
- Reports `ok|fail|timeout|not_found` to status endpoint.
- Regardless of outcome, restores original STA creds + reconnects.
- **UI must warn "you will lose connection for ~10 s, press F5 afterwards"
  before issuing the request.**

**Interim (today):** just use `/api/wifi/scan` + `/api/wifi/scan_results`
in a new "WiFi visibility" card. Show each RX-class SSID with its
`enc` code (0=OPEN, 2=WPA_PSK, 3=WPA2_PSK). User can manually try to
connect from their phone.

## 4. Copy/paste helper: hardware.json upload

After a successful vanilla-ELRS flash + boot into app0, the receiver
comes up in "bare" configuration mode. The user needs to upload the
pin map (`hardware.json` from the dump) via ELRS's own web UI at
`http://10.0.0.1`. Add a card with:

- **Download button** — serves the extracted hardware.json blob (we
  already have it at `hardware/bayckrc_c3_dual/` via `parse.py`).
- **Instructions**:
  1. Connect your phone/PC to the `ExpressLRS RX` WiFi (password:
     `expresslrs`).
  2. Open `http://10.0.0.1`.
  3. Upload the hardware.json from the download above.
  4. Click "Update" — receiver reboots with the correct pin map.

New backend endpoint needed:

```
GET /api/rx/hardware_json
```

Returns the hardware.json blob from the last parsed dump (we can keep
it pre-extracted in PSRAM, or hard-code the specific Bayck RC C3 Dual
pin map into firmware since this is a one-off device).

## 5. End-to-end flow the user should see (dashboard card)

A Markdown mini-wizard at the top of the ELRS Flash tab:

```
[ 1/5 ]  Put RX in DFU (hold BOOT button while re-applying power)
[ 2/5 ]  Read OTADATA status — if OK, RX is responding
[ 3/5 ]  Erase app0 (1.88 MB) — takes ~5 s
[ 4/5 ]  Upload vanilla ELRS 3.6.3 firmware.bin and flash at 0x10000
[ 5/5 ]  Write OTADATA pointing to app0, remove power, re-apply
```

Each step is a button that enables after the previous completed. Status
colour per step (gray / spinner / green / red). Every error shows the
raw server response below the step + a retry button.

## 6. Built assets to keep close

`hardware/bayckrc_c3_dual/vanilla_elrs_3.6.3/firmware.bin` already
exists in the repo (committed). The UI can upload it directly if the
user doesn't have their own .bin, e.g. via a "Use bundled ELRS 3.6.3"
button that fetches from `/api/rx/bundled_firmware` — another new
endpoint that streams `hardware/bayckrc_c3_dual/vanilla_elrs_3.6.3/firmware.bin`
from SPIFFS / PROGMEM. (For now: users download the .bin from
GitHub and upload it themselves.)

## 7. UI polish checklist

- [ ] All API error responses surfaced inline — no silent failures.
- [ ] "Dangerous" actions (erase app1, overwrite bootloader) behind
      `confirm()` dialogs with typed-confirmation for catastrophic cases.
- [ ] All slot/offset constants in one JS const so it's easy to adapt
      for other receivers later.
- [ ] Status auto-refresh disabled while user is typing in text fields
      (common trap — debounce or pause polling).
- [ ] Each HTTP call has a `.catch(...)` that at minimum logs + shows
      a toast; silent 404/5xx is strictly forbidden (already flagged
      in Sprint-31 audit).

## Backend API reference (what's available right now)

| Endpoint | Verb | Params | Returns |
|----------|------|--------|---------|
| `/api/flash/upload` | POST | multipart `firmware` | 200 OK after upload |
| `/api/flash/start` | POST | form `offset=<hex>` (default 0) | 200 OK with offset echo |
| `/api/flash/status` | GET | — | `{stage, progress_pct, in_progress, lastResult}` |
| `/api/flash/clear` | POST | — | frees buffer |
| `/api/flash/erase_region` | POST | `offset`, `size` (both hex or dec) | 200/500 |
| `/api/flash/dump/start` | POST | `offset` (default 0), `size` (default 4 MB) | 202 |
| `/api/flash/dump/status` | GET | — | `{running, progress, stage, error, ready}` |
| `/api/flash/dump/download` | GET | — | binary |
| `/api/flash/dump/clear` | POST | — | — |
| `/api/otadata/status` | GET | — | `{sectors: [...], max_seq, active_slot}` |
| `/api/otadata/select` | POST | `slot=0|1` | 200/500 |

All endpoints require the RX to be in **DFU mode** (hold BOOT button
during power-on). Port B is acquired as UART automatically.

## Safety rollback

Full 4 MB backup at `hardware/bayckrc_c3_dual/dump_2026-04-19_1528.bin`
(md5 `55f56ed5a17bf4ed1a2b85814e812a6b`). To restore:
1. RX in DFU.
2. POST `/api/flash/upload` with the dump.bin.
3. POST `/api/flash/start?offset=0`.
4. Power-cycle.
Original MILELRS + vanilla-ELRS-3.5.3 dual-firmware layout is restored
byte-for-byte.
