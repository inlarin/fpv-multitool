# Board Parity Audit — 2026-05-01 (v0.30.0)

Snapshot of feature parity between Waveshare ESP32-S3-LCD-1.47B and
WT32-SC01 Plus across the three UI surfaces (web, Waveshare local LCD,
SC01 Plus LVGL touchscreen).

Each row is one logical feature. Columns:

| col | meaning |
|---|---|
| **Web** | Tab in the AsyncWebServer-served UI (`data/tabs/*.html` + JS) |
| **Wsh local** | Waveshare local-LCD screen via BTN_BOOT menu (`src/board/wsh_s3_lcd_147b/ui/`) |
| **SC01 LVGL** | SC01 Plus touchscreen (`src/board/wt32_sc01_plus/ui/screen_*.cpp`) |
| **Source** | Where the feature's logic lives (almost all under `src/<feature>/`, shared) |

Symbols:

* ✅ — fully implemented
* 🟡 — partial / behind feature flag / read-only
* ❌ — missing
* ➖ — n/a (board can't physically do it)

---

## A. Servo / Motor / Battery — bench-tester core

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| Servo PWM (pulse + freq + start/stop) | ✅ `tabs/servo.html` | ✅ `servo_tester.cpp` | ✅ `screen_servo.cpp` | `src/servo/` |
| Servo sweep (triangular wave) | ✅ | 🟡 manual | ❌ | `src/motor/motor_dispatch.cpp` |
| Motor DShot ARM/DISARM/throttle | ✅ `tabs/motor.html` | ✅ `motor_tester.cpp` | ✅ `screen_motor.cpp` (capped 0..200/2000) | `src/motor/` |
| Motor beep / direction / 3D mode | ✅ | ✅ | 🟡 beep only | `src/motor/motor_dispatch.cpp` |
| ESC telemetry (KISS / BLHeli32) | ✅ `/api/esc/telem/*` | ❌ | ❌ | `src/fpv/esc_telem.cpp` |
| Battery quick read (V, I, SoC, T, cycles) | ✅ `tabs/battery.html` | ✅ `battery_ui.cpp` | ✅ `screen_battery.cpp` | `src/battery/dji_battery.cpp` |
| Battery cell voltages + balance view | ✅ | ✅ BP_CELLS page | ✅ Cell 1-4 rows | shared |
| Battery PF / Safety / OperationStatus decode | ✅ | ✅ BP_STATUS page | ❌ | shared |
| Battery lifetime info (chip/FW/serial) | ✅ | ✅ BP_LIFETIME page | ❌ | shared |
| Battery service: unseal/clearPF/seal | ✅ | ✅ BP_SERVICE page | ❌ | shared |
| Battery cycle-count reset (DF 0x4340) | ✅ | ✅ BP_CYCLE page | ❌ | shared |
| Battery I2C scanner | ✅ `/api/i2c/scan` | ✅ BP_SCAN page | ❌ | shared |
| Battery DataFlash editor (read/write) | ✅ `/api/batt/df/*` | ❌ | ❌ | shared |
| Battery MAC catalog (known battery list) | ✅ `/api/batt/mac_catalog` | ❌ | ❌ | shared |
| Battery clone-research suite (~25 endpoints) | 🟡 RESEARCH_MODE | ❌ | ❌ | shared `routes_battery.cpp` |

**Gap on SC01 LVGL:** the entire "Service ops" + "DataFlash editor" + "MAC catalog" set. Battery quick-read works; everything beyond it is web-only on the touchscreen.

---

## B. ELRS / CRSF / RC

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| CRSF live link monitor (RSSI / LQ / SNR) | ✅ `tabs/receiver.html` | ✅ `crsf_tester.cpp` | ✅ `screen_elrs.cpp` | `src/crsf/crsf_service.cpp` |
| CRSF channel display (16 channels) | ✅ | ✅ | 🟡 first 8 only | shared |
| CRSF battery / GPS / attitude telem | 🟡 partial | 🟡 partial | ❌ | shared |
| CRSF service start/stop/reboot | ✅ `/api/crsf/*` | implicit | ✅ button | shared |
| ELRS RX bind | ✅ `/api/elrs/bind` | ❌ | ✅ button | shared `routes_flash.cpp` |
| ELRS device info / chip identification | ✅ `/api/elrs/{device_info,chip_info}` | ❌ | ❌ | shared |
| ELRS parameter edit | ✅ `/api/elrs/params/*` | ❌ | ❌ | `src/crsf/crsf_config.cpp` |
| ELRS sticky DFU session | ✅ `/api/elrs/dfu/*` | ❌ | ❌ | shared |
| ELRS firmware flash (vanilla → vendor) | ✅ `tabs/receiver.html` | ❌ | ❌ | shared `routes_flash.cpp` |
| ELRS firmware patcher (in-place) | ✅ `/api/elrs/firmware/patch` | ❌ | ❌ | `src/bridge/firmware_patch.cpp` |
| ELRS identity / model match / TX info | ✅ `/api/elrs/{identity,modelmatch,tx_info}` | ❌ | ❌ | shared |
| ELRS NVS info / diagnostics | ✅ `/api/elrs/{nvs/info,diag/full}` | ❌ | ❌ | shared |
| RC Sniff (SBUS / iBus / PPM auto-detect) | ✅ `tabs/rcsniff.html` | ❌ | ✅ `screen_sniff.cpp` | `src/rc_sniffer/` |

**Gap on SC01 LVGL:** everything ELRS beyond bind+monitor — params, flash, patcher, dfu — is web-only.
**Gap on Waveshare local:** RC Sniff has no menu entry; all ELRS beyond live monitor is web-only.

---

## C. Bridge modes (USB physical-layer modes)

These are different USB descriptor modes the board enumerates as. Not "features" in the LVGL/menu sense — they replace the entire normal app loop.

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| USB-CDC normal mode | ✅ default | ✅ default | ✅ default | `src/core/usb_mode.cpp` |
| USB2TTL transparent serial bridge | ➖ | ✅ `usb2ttl.cpp` (full app) | ❌ | Waveshare-only port |
| USB2SMBus bridge (custom protocol) | 🟡 mode toggle | ✅ `smbus_bridge_ui.cpp` | ❌ | `src/battery/smbus_bridge.cpp` |
| CP2112 HID I2C bridge (Vendor mode) | 🟡 mode toggle `tabs/usb.html` | implicit | ❌ | `src/usb_emu/cp2112_emu.cpp` |
| USB descriptor mode selector | ✅ `/api/usb/mode` | ✅ menu nav | ❌ | shared |
| Setup wizard (device + role → preset) | ✅ `tabs/setup.html` | ❌ | ❌ | `src/web/http/routes_port.cpp` |

**Gap on SC01 LVGL:** the entire bridge-mode UI. The board can be in CP2112 mode (Vendor descriptor), but switching is web-only — no "Bridge mode" tile on the springboard. Most users won't notice because the SC01 Plus touchscreen is the local UI; bridge modes are PC-side workflows.

---

## D. WiFi / network

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| WiFi STA credentials editor | ✅ `tabs/setup.html` | ✅ `wifi_app.cpp` (with QR) | ✅ Settings card + lv_keyboard modal | `src/core/board_settings.cpp` + `web/wifi_manager.cpp` |
| WiFi network scanner | ✅ `/api/wifi/scan` | ❌ | ❌ | shared |
| WiFi clear creds | ✅ `/api/wifi/clear` | ✅ | ❌ (only via Serial CLI / wifi clear) | shared |
| AP fallback mode | ✅ shows AP banner | ✅ (QR code on LCD) | 🟡 status bar only | shared |
| Outbound health beacon | ✅ `/api/sys/beacon[/now]` | ❌ | ❌ (configurable web-only) | `src/core/safety.cpp` |

**Gap on SC01 LVGL:** WiFi scan picker (currently SSID/password is typed by hand). Beacon URL/interval edit.
**Gap on Waveshare local:** WiFi scan, beacon config.

---

## E. System / display / settings

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| Display rotation (0/90/180/270) | ✅ `/api/sys/rotation` | ➖ fixed | ✅ Settings 4 buttons + reboot | `BoardDisplay::setRotation` |
| Display brightness | ❌ | ➖ | 🟡 slider, no NVS persist | LovyanGFX `setBrightness` |
| Touch calibration | ➖ | ➖ | ✅ Settings → wipe + reboot | `BoardDisplay::calibrateTouch` |
| Port B mode picker (I2C/UART/PWM/GPIO) | ✅ `tabs/setup.html` `/api/port/preferred` | ❌ | ❌ Settings card missing | `src/core/pin_port.cpp` |
| Port B autodetect (CRSF/SBUS/iBus/I2C) | ✅ `/api/port/autodetect` | ❌ | ❌ | shared |
| Soft reboot | ✅ `tabs/sys.html` `/api/sys/reboot` | hold-button | ✅ Settings → Reboot | shared |
| Free heap / uptime / IP / OTA state | ✅ `tabs/sys.html` + WS | 🟡 RGB LED color | ✅ Status bar + Settings About | shared |
| In-memory log ring | ✅ `/api/sys/log` | ❌ | ❌ (only via web) | `src/core/safety.cpp` |
| Coredump download / erase | ✅ `/api/sys/coredump[/erase]` | ❌ | ❌ | shared |
| Synthetic UI tap (autonomous test) | ✅ `/api/sys/ui/tap` | ➖ (no tap input) | ✅ indev queue | `src/board/wt32_sc01_plus/ui/board_app.cpp` |
| Screenshot to BMP | ✅ `/api/sys/screenshot.bmp` | ➖ | ✅ via lvLock | shared |
| FW version display | ✅ `tabs/sys.html` | ❌ | ✅ status bar right edge | `src/core/build_info.h` |

**Gap on SC01 LVGL:** Port B mode picker is the most-requested missing feature (we hit this several times in this session). Brightness NVS persist. Beacon config.

---

## F. Storage / catalog (SC01-Plus-only physical capability)

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| SD card mount / status | ❌ no web endpoint | ➖ no SD wiring used | ✅ Catalog screen | `main_sc01_plus.cpp` boot |
| SD `/catalog/` browser (read-only) | ❌ | ➖ | ✅ Phase 1 | `screen_catalog.cpp` |
| One-tap firmware flash from catalog | ❌ | ➖ | ❌ Phase 3 deferred | shared (when ready) |
| Catalog mirror / sync | ❌ | ➖ | ❌ | — |

Waveshare has SD wired but it's not currently used by any feature in the project. SC01 Plus boot mounts SD on every start.

---

## G. OTA / safety net

| Feature | Web | Wsh local | SC01 LVGL | Source |
|---|:-:|:-:|:-:|---|
| OTA upload (multipart) | ✅ `/api/ota/upload` + `tabs/ota.html` | ❌ | ❌ | `src/web/http/routes_ota.cpp` |
| OTA pull from GitHub release | ✅ `/api/ota/{check,pull}` | ❌ | ❌ | shared |
| GitHub repo configured at build time | ✅ Wsh build flag | n/a | **❌ SC01 Plus build flag missing** | `platformio.ini` |
| OTA rollback (`esp_ota_mark_app_valid_cancel_rollback`) | ✅ | ✅ | ✅ | `src/core/safety.cpp` |
| Boot-loop counter + safe mode | ✅ | ✅ | ✅ | shared |
| Network watchdog (auto-restart on STA loss) | ✅ | ✅ | ✅ | shared |
| Coredump capture | ✅ | ✅ | ✅ | shared |
| Factory + safeboot recovery firmware | ❌ no factory partition | ❌ | ✅ `wt32_sc01_plus_safe` env | `src/board/wt32_sc01_plus/safeboot.cpp` |

**Gap:** GitHub OTA only works for Waveshare. Needs `-DGITHUB_REPO=...` added to `[env:wt32_sc01_plus]` build flags + multi-asset support per release (see "GitHub OTA flow" below).

---

## H. Partition tables / build envs

| Env | Board | Partition | Purpose |
|---|---|---|---|
| `esp32s3` | Waveshare | `default_16MB.csv` | full app |
| `wt32_sc01_plus` | SC01 Plus | `default_16MB.csv` | full app, OTA-only safety net |
| `wt32_sc01_plus_safe` | SC01 Plus | `partitions/sc01_plus_safeboot_16MB.csv` | full app + factory partition for safeboot recovery |
| `wt32_sc01_plus_safeboot` | SC01 Plus | `partitions/sc01_plus_safeboot_16MB.csv` | safeboot recovery shell only (factory partition target) |
| `wt32_sc01_plus_{sanity,lcd,lvgl,sdmmc,wifi}` | SC01 Plus | `default_16MB.csv` | Sprint 32 bring-up sanity sketches (deletable post-Sprint 32) |

---

## GitHub OTA flow — current state

```
HTTPS https://api.github.com/repos/<GITHUB_REPO>/releases/latest
    └─ JSON.tag_name        -> latest version
    └─ JSON.assets[]        -> hardcoded match: name == "firmware.bin"
        └─ download_url     -> stored in s_latestAssetUrl

POST /api/ota/check         -> queries above, returns {current, latest, asset, outdated}
POST /api/ota/pull          -> downloads asset to PSRAM, Update.write, reboot
                               (s_latestAssetUrl from check; 90s timeout for ~1.5 MB)
```

Issues with the current setup:

1. **`GITHUB_REPO` is only set on `[env:esp32s3]`** (Waveshare). The
   SC01 Plus build doesn't have the flag — `/api/ota/check` returns
   `400 GITHUB_REPO not configured`. Easy fix: copy the build flag
   into the SC01 Plus env(s).

2. **Asset name is hard-coded `firmware.bin`.** Both boards need
   different binaries (R8/OPI vs R2/QSPI PSRAM, `ARDUINO_USB_MODE 0`
   vs `1`, different display drivers). A single `firmware.bin` per
   release can't serve both. If SC01 Plus pulls a Waveshare-built
   binary, it bricks (PSRAM mismatch panics at first OTA-bound
   peripheral init).

   **Fix needed:** rename the asset look-up to be board-aware. Two
   options:

   - **Per-board release asset name.** CI uploads
     `firmware-esp32s3.bin` and `firmware-wt32_sc01_plus.bin` to each
     release. Build flag tells the firmware which asset name to
     match: `-DGITHUB_OTA_ASSET=\"firmware-esp32s3.bin\"`.
   - **Per-board release tag prefix.** `v0.30.0-esp32s3` and
     `v0.30.0-wt32_sc01_plus` as separate releases. Cleaner for
     consumers but doubles the release count.

   Option 1 is less invasive and is what the existing patch comment
   in `routes_ota.cpp:108-113` is already half-set-up for.

3. **Latest pushed tag is `v0.28.6`.** Local tags `v0.29.0`,
   `v0.29.1`, `v0.30.0` are not on GitHub. If a Waveshare board
   running v0.29.x clicks "Update from GitHub" right now, it pulls
   v0.28.6 — i.e. **downgrades** to a build before the back-button
   hang fix, the Port B grab fix, and the symmetric layout. Need to
   `git push origin --tags` and create a Release with the matching
   `firmware.bin` attached.

4. **No CI for releases.** Pushing the tag doesn't automatically
   create a GitHub Release with the firmware binary attached. We'd
   need a GitHub Actions workflow (or do it by hand: upload
   `firmware.bin` to the Release via the GitHub web UI / `gh release
   upload`). Without this, even after pushing tags, `releases/latest`
   responds 404 because no Release object exists.

---

## Top gaps to close

By estimated user impact:

1. **SC01 LVGL: Port B mode picker** in Settings. Right now Servo /
   Motor / Battery / ELRS each release-and-acquire Port B
   transparently, but if they fight (one screen leaves it in PWM
   when user enters Battery), there's no LVGL surface to manually
   reset. Currently only `/api/port/preferred` over web.

2. **GitHub OTA for SC01 Plus.** `-DGITHUB_REPO` + per-board asset
   name. Without this, only Waveshare can self-update from GitHub.

3. **Push v0.30.0 + create Release.** Current users are stuck on
   v0.28.6 if they trust the GitHub OTA path.

4. **SC01 LVGL: ELRS firmware flash.** Largest ELRS feature gap —
   currently web-only. Maps to the Phase 3 catalog flasher work
   already on the roadmap.

5. **SC01 LVGL: battery service ops** (unseal/clearPF/cycle reset).
   Waveshare local has them; SC01 LVGL doesn't. Most-requested
   battery feature after quick-read.

6. **Waveshare local: ELRS bind / device info.** `crsf_tester.cpp`
   is read-only. Adding a single "Bind" hold-press would close most
   of the Waveshare-side ELRS gap.

7. **SC01 LVGL: USB descriptor mode + Setup wizard.** Right now you
   can put SC01 Plus into CP2112 mode only via web. If someone takes
   the touchscreen unit on the road without a laptop, they can't
   switch modes.

Items 4-7 are deferred features. Items 1-3 are pure plumbing /
release-process and unblock everything else.

---

## What's not under any UI yet

These exist as web routes but no LVGL or local-LCD entry exposes them:

* `/api/sys/log` — diagnostic ring buffer
* `/api/sys/coredump` — saved panic dump
* `/api/i2c/{scan,preflight}` — I2C bus inspection
* `/api/port/{autodetect,probe_rx,swap}` — Port B diagnostics
* `/api/elrs/{nvs/info,diag/full}` — ELRS deep-dive
* `/api/batt/clone/*` — clone-research suite (intentional —
  RESEARCH_MODE-gated, web-only)

These are operator / developer tools rather than end-user features,
so leaving them web-only is reasonable.
