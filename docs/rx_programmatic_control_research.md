# Programmatic control of ESP32-C3 ELRS receivers — research report

**Target hardware:** Bayck RC C3 Dual Band Gemini RX (ESP32-C3 + LR1121) running either
vanilla ExpressLRS 3.x / 3.6.3 or the MILELRS fork (`MILELRS_v348`).

**Host:** ESP32-S3 "FPV MultiTool" plate. Port B = GPIO 10 (SCL/TX) / GPIO 11 (SDA/RX);
ELRS pads on GPIO 43 (TX → RX-RX), 44 (RX ← RX-TX), 3 (BOOT/DFU).

**Goal:** eliminate every "hold BOOT + power-cycle" step from the flash / dump / verify
cycle.

**TL;DR** — the single most important finding: **ExpressLRS 3.x on ESP32 does NOT jump
to the ROM bootloader**. Instead, in response to the CRSF "bl" command it enters an
**in-application esptool-stub flasher** (`connectionState = serialUpdate`,
`start_esp_upload()`) that speaks the exact same SLIP protocol as the ROM — same op
codes (`FLASH_BEGIN=0x02`, `FLASH_DATA=0x03`, `FLASH_END=0x04`, `READ_FLASH=0xD2`,
`SPI_FLASH_MD5=0x13`, `ERASE_REGION=0xD1`, `RUN_USER_CODE=0xD3` …). **Everything our
plate needs already works over a 5-byte CRSF frame on the live UART — no DFU strap, no
power-cycle, no touch.** The section-by-section details follow.

---

## 1. Programmatic DFU entry

### 1.1 CRSF "reboot-to-bootloader" (THE ANSWER, verified in ELRS 3.6.3 source)

ELRS 3.x watches every CRSF telemetry frame it receives on its RX pad for a 5-byte
"Non-CRSF command" with `dest='b'`, `src='l'`:

```
Byte 0: 0xEC  (CRSF_ADDRESS_CRSF_RECEIVER, full-duplex / CRSF variant)
Byte 1: 0x04  (frame length = 4)
Byte 2: 0x32  (CRSF_FRAMETYPE_COMMAND)
Byte 3: 'b'   (0x62, dest)
Byte 4: 'l'   (0x6C, src)
Byte 5: CRC8  (poly 0xD5 over bytes 2..4; extended with any key-append bytes)
```

Half-duplex / GHST variant starts with `0x89` instead of `0xEC`.

**ELRS source evidence** (`hardware/bayckrc_c3_dual/elrs_3_6_3_src/src/`):

| File | Role |
|------|------|
| `python/bootloader.py:2-5` | canonical `INIT_SEQ` bytes and `calc_crc8(poly=0xD5)` |
| `lib/Telemetry/telemetry.cpp:226-231` | RX-side detector: `if (package[3]=='b' && package[4]=='l') callBootloader = true;` |
| `src/rx-serial/SerialCRSF.cpp:129-132` | `if (telemetry.ShouldCallBootloader()) reset_into_bootloader();` |
| `src/rx_main.cpp:2279-2307` | `reset_into_bootloader()` → `connectionState = serialUpdate` on ESP32 (NOT ROM reboot) |
| `lib/SerialUpdate/devSerialUpdate.cpp:51` | `start_esp_upload();` then tight loop feeding bytes to `stub_handle_rx_byte` |
| `lib/SerialUpdate/stub_flasher.cpp` | full `execute_command` switch — the ROM protocol, re-implemented inside the app |

So the sequence of events on the wire, once the RX is running ELRS:

1. Plate sends `EC 04 32 62 6C <crc>` at configured RC baud (420 000 or 115 200).
2. ELRS app switches `connectionState` to `serialUpdate`, stops the hw-timer, parks the
   LR1121 in idle, lowers TX power to MinPower.
3. Plate **immediately** begins speaking ESPtool SLIP on the same UART — no retrain,
   no baud renegotiation needed (the stub just mirrors whatever the CRSF serial port
   is already set to; no auto-baud training sequence required). That is why
   `BFinitPassthrough.py:128` sends `\x07\x07\x12\x20 + 32 × \x55` only when going to
   the *ROM* bootloader (via BF passthrough), and NOT when hitting the in-app stub.
4. The stub answers `FLASH_BEGIN`, `FLASH_DATA`, `FLASH_END`, `READ_FLASH`,
   `SPI_FLASH_MD5`, `ERASE_REGION`, etc. exactly like the ROM. `ERASE_FLASH=0xD0`,
   `ERASE_REGION=0xD1`, `RUN_USER_CODE=0xD3`, `FLASH_ENCRYPT_DATA=0xD4` are all in the
   switch table.

**Note on the strap pin:** `reset_into_bootloader()` does **NOT** touch GPIO 9 —
there is no ROM DFU involved at all. The entire flasher runs from app code. That is
why our earlier `ESP.rebootIntoUartDownloadMode()` theory was wrong for ESP32 (that
call exists only on ESP8266 path).

### 1.2 Does MILELRS handle this?

Partial. The dump shows MILELRS v3.48 is a fork of 3.5.x branch with proprietary
additions (see §4). It still contains the full `telemetry.cpp` logic (`b'`,`'l'` is
a near-universal ELRS entry point used by every Configurator / Betaflight-passthrough
/ UARTupload workflow — impossible to remove without breaking their own OTA tooling).
**Recommendation: assume it works, test on device.** Fallback plan is CRSF passthrough
hitting the ROM via GPIO 9 strap (see 1.4).

### 1.3 Magic UART boot pattern (ESP32-C3 ROM)

Does not exist on ESP32-C3. The C3 ROM samples only strapping pin GPIO 9 at
reset. The `07 07 12 20 + 32×0x55` sequence in `BFinitPassthrough.py` is the ROM
**baud-training** sequence (sent AFTER the chip is already in DFU via GPIO 9), not a
magic-word entry. Sources:
[ESP32-C3 boot mode docs](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html),
[esptool serial protocol](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/serial-protocol.html).

### 1.4 Hardware DFU-strap multiplex via GP10/GP11

Theoretically possible but **not worth it** given that 1.1 gives us everything we
need. The C3 has a 45 kΩ internal pull-up on GPIO 9, so a ~10 kΩ series resistor from
GP11 (or GP10) to RX_GPIO9 would let us pull low during reset. Risk: back-feeding
3.3 V onto a running C3 GPIO is mostly fine (clamp diodes), but you also have to
assert EN low to actually reset — we'd need a 3rd wire. Verdict: **skip**.

### 1.5 "Force DFU on next boot" via RTC register

On ESP32-C3 there is a trick:

```c
REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);  // bit 0
esp_restart();
```

This forces the *next* boot into the ROM download mode regardless of GPIO 9 state.
The reset persists through one cycle then clears. This is what ESP-IDF's
`rebootIntoUartDownloadMode()` does internally on S2/S3/C3. **But this requires the
running app to cooperate** — it needs a code path that executes `REG_WRITE + restart`.
Vanilla ELRS does not expose such a path on ESP32-C3. MILELRS won't either.

However, WE control the in-app stub (it's part of the ELRS app). The stub *does* have
an `ESP_RUN_USER_CODE=0xD3` that calls `ESP.restart()` — but that boots back into
the app, not into DFU. We could **patch a custom build** of ELRS that interprets one
of the unimplemented opcodes as `REG_WRITE + restart` and from there talks to the ROM
directly — but this only matters if we can't trust the in-app stub. In practice the
in-app stub IS the ROM from our point of view, so this is premature optimization.

### 1.6 esptool DTR/RTS dance

`esptool --before default_reset --after hard_reset` uses DTR tied to GPIO 9 and RTS
tied to EN, toggling them in sequence. **We have neither line** — Port B only has
TX/RX. Equivalent for us is §1.4 (don't). Source:
[esptool entering bootloader](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/entering-bootloader.html).

### 1.7 Practical recommendation

**Implement CRSF "bl" as primary DFU-entry path.** Code is trivial:

```cpp
// Assume ELRS is running and our UART is already at CRSF baud (420000 for vanilla,
// 115200 for MILELRS default).
void sendCrsfBootloaderCmd() {
    const uint8_t seq[5] = { 0xEC, 0x04, 0x32, 'b', 'l' };
    uint8_t crc = calc_crc8_poly_d5(&seq[2], 3);
    uart.write(seq, 5);
    uart.write(&crc, 1);
    uart.flush();
    delay(150);   // let ELRS event() fire and swap into serialUpdate state
    // now we can speak esptool SLIP at the SAME baud
}
```

**GHST variant:** same bytes with `0x89` sync and `0x89` as the half-duplex address.

**Fallback path (when the app is bricked / unknown baud):** require user to hold BOOT
+ power-cycle — same as today. We already handle this with the "Setup → auto-detect
elrs_rom" flow.

### 1.8 Effort

~2 h: add `sendCrsfBootloaderCmd()` to `crsf_proto.cpp`, wire a button
in the ELRS tab ("Reboot to flasher (no DFU)"), and make `/api/flash/*` try it
before asking for DFU.

---

## 2. Programmatic DFU exit

### 2.1 Staying in DFU after a read

`FLASH_END(reboot=1)` is the standard way out (reboots into app). `FLASH_END(reboot=0)`
is specified in the ROM/stub but the return-to-caller behavior is undefined from
CRSF-triggered in-app stub (the stub's `while(1)` loop in `devSerialUpdate.cpp:52` is
infinite and never returns — only a reset breaks it). In practice:

- **ROM bootloader:** `FLASH_END(0)` leaves you in DFU indefinitely. Useful.
- **ELRS in-app stub:** no clean exit. `ESP_RUN_USER_CODE=0xD3` (→ `ESP.restart()`)
  is the only way to leave, and it always reboots. The plate must treat each CRSF-"bl"
  session as a single transaction.

There is no "noop exit" command. If you want to stay, simply don't send `FLASH_END`
or `ESP_RUN_USER_CODE` — just keep sending reads / writes / NOPs. The stub has no
inactivity timeout in the ELRS implementation (ROM has ~several-second timeout; stub
has none — `while(true)` at `devSerialUpdate.cpp:52`).

### 2.2 Verify which app booted, without asking user to reconnect BOOT

After reboot, ELRS resumes normal CRSF telemetry output on its TX line. The plate
should **listen passively** for:

1. `CRSF_FRAMETYPE_DEVICE_INFO (0x29)` frame emitted by ELRS on startup —
   contains firmware name/version string. See `lib/CrsfProtocol/crsf_protocol.h`.
2. `CRSF_FRAMETYPE_LINK_STATISTICS_RX (0x1C)` — proves the radio loop is running.

If nothing arrives within ~5 s, send `CRSF_FRAMETYPE_DEVICE_PING (0x28)` — ELRS
responds with `DEVICE_INFO`. We can parse `"MILELRS_v348"` vs `"ExpressLRS RX"` out of
the name field to distinguish partitions.

Alternatively, probe WiFi: vanilla ELRS on boot (after the 60-s autostart window)
goes into STA, then AP (`ExpressLRS RX` / `expresslrs`). MILELRS does the same but
SSID=`MILELRS_v348`, pw=`8bced9`.

### 2.3 Effort

~3 h: implement passive listener state-machine in `crsf_service.cpp`; 1 h for WiFi
probe using the S3's STA-scan API.

---

## 3. ELRS bring-up without hardware.json

### 3.1 What vanilla ELRS does with no `hardware.json`

`src/rx_main.cpp:2062-2080` is the key:

```cpp
void setup() {
#if defined(TARGET_UNIFIED_RX)
    hardwareConfigured = options_init();
    if (!hardwareConfigured) {
        // fall back to bare WiFi-only mode
        SerialLogger = new NullStream();
        devicesRegister({&WIFI_device, 1});
        devicesInit();
        connectionState = hardwareUndefined;   // LED = solid-on-failure, but
                                                // WiFi AP comes up with SSID
                                                // "ExpressLRS RX" / "expresslrs"
    }
    ...
```

So **`TARGET_UNIFIED_RX` firmware DOES emit a WiFi AP on first boot** even without
`hardware.json` in SPIFFS — it goes straight into "hardware-undefined" mode, starts
WiFi AP (`lib/WIFI/devWIFI.cpp:624`: "Access Point starting, please connect to
'ExpressLRS RX' with password 'expresslrs'"), and serves `/hardware.html` over
10.0.0.1.

**This contradicts our earlier observation** that the bare-built firmware "doesn't
emit WiFi". Likely causes in our case:

1. We flashed the **non-unified** target (`BAYCKRC_C3_Dual_Band_Gemini_RX_via_WIFI`
   is a pre-baked target — NOT `TARGET_UNIFIED_RX`). The non-unified path has
   `hardwareConfigured = true` hard-coded (`src/rx_main.cpp:128`).
2. The SHA-256 hash-append bug (see §7 of the analysis). A bare-built firmware with
   zero-placeholder hash will `image_verify()` fail, the 2nd-stage bootloader skips
   it, and the MILELRS partition boots instead.
3. OTADATA points to MILELRS.

### 3.2 How Configurator uploads hardware.json to a bare RX

From `lib/WIFI/devWIFI.cpp:1095-1098`:

```cpp
server.on("/hardware.html", WebUpdateSendContent);
server.on("/hardware.js",   WebUpdateSendContent);
server.on("/hardware.json", getFile).onBody(putFile);   // ← PUT/POST upload
```

- `GET /hardware.json` returns the built-in config (from end-of-firmware magic region
  `\xBE\xEF\xCA\xFE` followed by ProductName + DeviceName + options + hardware.json —
  see `lib/OPTIONS/hardware.cpp:231-254`) or the SPIFFS override if present.
- `POST /hardware.json` with the JSON body saves it to SPIFFS.
- `GET /reset?hardware=1` wipes it.

So the Configurator bring-up flow is WiFi-only:
1. User connects to `ExpressLRS RX` AP.
2. Browser opens `http://10.0.0.1/hardware.html`, form pushes the JSON to
   `POST /hardware.json`.
3. `POST /reboot`.

**There is no UART path for uploading hardware.json in the ELRS protocol** — only
WiFi HTTP.

### 3.3 Our plate replicating the bring-up flow

**Option A (recommended): our plate joins the RX's AP.**

Since our S3 has full WiFi, we can:
1. After flash-verify + reboot, scan for `ExpressLRS RX` AP for up to 60 s.
2. Connect (pw = `expresslrs`).
3. HTTP-POST the `hardware.json` blob we extracted from the MILELRS dump (stored as
   embedded PROGMEM in our firmware) to `http://10.0.0.1/hardware.json`.
4. GET `/reboot`.

Flow runs entirely from our plate. User presses one button; no phone.

**Option B: bake hardware.json into the uploaded firmware image.**

The ELRS build pipeline supports appending `hardware.json` to the firmware blob via
the magic `\xBE\xEF\xCA\xFE` delimiter + 2048-byte slots for ProductName, DeviceName,
OptionsJSON, HardwareJSON (see `lib/OPTIONS/hardware.cpp:233` and
`python/binary_configurator.py`). We can patch the stock `firmware.bin` in-place on
the S3 before flashing: seek the magic, write our JSON into the hardware slot,
re-compute SHA-256, re-append. This is the same thing Configurator does server-side
when user clicks "Build Firmware".

Option B is cleaner (single-step flash, no WiFi roundtrip) and saves us ~12 s.

### 3.4 Recommendation

Do both. Option B for the "flash-a-known-blessed-target" path (vanilla 3.6.3 +
hardware.json baked in). Option A as automation safety net for the case where user
supplies a generic `Unified_ESP32C3_LR1121_RX.bin` from the Configurator without any
target baked in.

### 3.5 Effort

- Option B (patch `firmware.bin` in S3 RAM before flashing): ~4 h. Involves parsing
  the `\xBE\xEF\xCA\xFE` header, computing SHA-256 over the image, and re-appending.
- Option A (S3 → RX AP → POST): ~3 h. `WiFiClient` + `HTTPClient` from ESP32 core.

---

## 4. MILELRS fork internals

### 4.1 Public evidence

MILELRS is the **Russian-affiliated "FPV_VYZOV"** ecosystem fork. It adds four
pillars on top of vanilla ELRS:

- **Encryption:** per-unit TX_KEY / RX_KEY pair, server-generated `TX_LOCK` bound to
  a hardware ID `TX_ID`. Replaces the public 6-char BindingPhrase with cryptographic
  auth. Meant to harden against hijack / spoofing.
- **Multi-band redundancy:** 900 MHz + 2.4 GHz links operated in parallel; one drops,
  the other keeps control.
- **Swarm:** `SWARM_ID` — multi-drone switching.
- **EW scanning:** `ew_scanner` mode — listens to the RF floor and reports / auto-hops.

Source: [cybershafarat: Russian FPV Drone Tactical Ecosystem – MILELRS – MILBETA
– FPV_VYZOV](https://cybershafarat.com/2025/10/05/russian-fpv-drone-tactical-ecosystem-milelrs-milbeta-fpv_vyzov/),
distributed via portals `milpilots.com`.

### 4.2 What our dump confirms

(from `hardware/bayckrc_c3_dual/dump_2026-04-19_1528.deepscan.md`)

- Strings: `ew_scanner`, `swarm_id`, `encryption_key`, `custom_freq`, `vx_control`
  (VX seems to be VTX control), `tx_id`, `tx_lock`, `tx_key`, `rx_key`.
- Build paths contain `danko` — likely the original fork author (not mentioned on
  GitHub — this is a **private / semi-private repo**, probably on a Russian git host
  like gitflic.ru or a pay-per-download Telegram).
- AP SSID `MILELRS_v348` / pw `8bced9` — note the 6-hex password suggests it's
  derived from the last 3 bytes of the WiFi MAC or from `flash_discriminator`.
- HTML blobs in app1 rodata at `0x001fcda9` — `<title>MILELRS - ...`.

### 4.3 Custom commands — standard CRSF subcommands or new transport?

From the deepscan, the custom keys look like **ELRS parameter-tree (CRSF Param settings)
entries**, NOT a new subcommand family. That is: MILELRS stuck to the standard
CRSF `PARAMETER_READ (0x2C)` / `PARAMETER_WRITE (0x2D)` / `PARAMETER_SETTINGS_ENTRY
(0x2B)` tree and just added new `devXXX.cpp` tree entries (like `devScan`, `devSwarm`)
with names such as `ew_scanner` under the top-level device. This is exactly how
vanilla ELRS does `devBackpack`, `devBaro`, `devVTX`, etc. So the wire format is
standard — a standard CRSF Param-tree walker on the plate will enumerate them.

### 4.4 Web UI override

`MILELRS` HTML blobs appear gzipped in rodata (magic `1F 8B 08 00`). Worth unpacking
to see if they call non-standard endpoints — but my guess is no, they just rebranded
the ELRS configurator HTML.

### 4.5 Community repo

**Searched; nothing public.** The fork lives off-GitHub. Our dump is effectively the
primary source.

### 4.6 Recommendation

Treat MILELRS as "ELRS 3.5.x + extra param-tree entries + custom bootloader logic
near-identical to vanilla". For our tester:

- CRSF `bl` entry should work (we test).
- Param tree enumeration yields the extra settings "for free" — no special code path.
- When re-flashing a MILELRS RX to vanilla, preserve the original flash dump as a
  safety-net restore.

### 4.7 Effort

0 h for protocol compat (leverages existing CRSF param code).
3 h for "MILELRS detected" badge in UI (show `SWARM_ID`, `TX_ID`, extra param
summary).

---

## 5. Alternative receivers / market survey (2026)

ESP32-C3 + LR1121 dual-band is the **dominant 2026 architecture** for sub-G + 2.4 GHz
ELRS RX. Published vanilla ELRS 3.6.3 target list (from
[ExpressLRS/Targets](https://github.com/ExpressLRS/targets/blob/master/targets.json),
2026-02):

| Brand | Model | Layout file |
|-------|-------|-------------|
| RadioMaster | XR1 / XR3 Nano / XR4 | `Generic C3 LR1121 True Diversity.json` |
| BetaFPV | ELRS Lite C3 LR11 RX | same |
| HGLRC | C3 900/2400 Dual Band Nano / PRO | same |
| GEPRC | C3 LR1121 RX | same |
| Sub250 | C3 DualBand | same |
| BAYCKRC | C3 900/2400 Dual Band Gemini RX | same base, uses `dualc3` preset |
| Spedix / THOBBY / Flywoo | various C3+LR1121 clones | all converge on Generic layout |

Sources: [Oscar Liang review of XR1/2/3/4](https://oscarliang.com/radiomaster-xr1-xr2-xr3-xr4-elrs-receivers/),
[ExpressLRS Gemini doc](https://www.expresslrs.org/software/gemini/),
[multirotorguide HW list](https://www.multirotorguide.com/news/list-of-expresslrs-hardware-transmitters-and-receivers/).

**The BAYCK is a relabel of the generic C3+LR1121 layout** — our extracted
`hardware.json` is drop-in compatible with the other clones. Official Bayck-published
version exists as an ELRS Configurator preset named `BAYCKRC_C3_Dual_Band_Gemini_RX`
(min version 3.5.0) — reachable via `GET https://raw.githubusercontent.com/ExpressLRS/targets/master/<preset>.json`.

### Recommendation

Ship with 5-10 pre-baked `hardware.json` presets in our firmware (all of the above
plus a generic fallback). ~1 KB each compressed. Total ~10 KB PROGMEM. Effort: 2 h
to collect + test.

---

## 6. Full-flash verify strategy

### 6.1 Bit-error rate over 115200

With good wiring and no inverters, 115200 N81 UART over ~10 cm breadboard jumpers
has BER < 1e-9 in practice. For 1.24 MB (~9.9 Mbit) at BER 1e-9 that's ~0.01 bits of
expected error — rare, but still means one byte flip every few hundred flashes. We
MUST verify.

Separately, the stub flasher has its own checksum on each FLASH_DATA packet — so
single-byte UART errors are caught already. But a failed checksum retries the whole
packet; it doesn't protect against a silent write to the wrong flash offset, or a
programmer/bookkeeping bug on our side.

### 6.2 SHA-256 full-readback

Read-and-hash: 1.24 MB @ 115200 = ~120 s (SLIP overhead × 1.12). Total flash+verify
cycle ≈ 260 s. Acceptable but slow.

### 6.3 SPI_FLASH_MD5 (command 0x13) — THE FIX

Per [esptool serial-protocol docs](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/serial-protocol.html)
and `stub_flasher.cpp:84,154-156`:

```
REQ:  op=0x13, data_len=16, data = [addr:u32_LE, size:u32_LE, 0:u32, 0:u32]
RESP: 16-byte raw MD5 (stub) / 32-byte ASCII-hex (ROM) + 2 status bytes
```

The in-app ELRS stub **includes full MD5 support** (see
`lib/SerialUpdate/stub_flasher.cpp:84` with `resp.len_ret = 16 + 2;`). So we can
compute MD5 locally over the source buffer on our S3 (mbedtls has an MD5 impl that's
already pulled in by WiFi), ask the RX for the MD5 of the newly-flashed partition,
and compare in ~200 ms. This is a **~600× speedup** over full readback verify.

### 6.4 Recommendation

- Always send `SPI_FLASH_MD5` after `FLASH_DATA` block sequence is done.
- Compare to mbedtls-computed MD5 on S3.
- On mismatch: retry the partition once, then fall back to full SHA-256 readback
  (to pinpoint which block corrupted).

### 6.5 Effort

~3 h. Add `flashVerifyMd5(offset, size, expectedMd5[16])` to
`src/bridge/esp_rom_flasher.cpp`. MD5 side can use `mbedtls_md5_context` (present in
Arduino-ESP32).

---

## 7. Rate-limit / power-cycle bypass — other tricks

### 7.1 Keep-alive NOPs

The ROM bootloader has a ~3-s inactivity timeout before it bails to boot the app.
Send `READ_REG(0)` or `SYNC` every ~2 s during long operations (e.g. user is typing
at a config prompt between flash steps). The ELRS in-app stub has NO timeout — it
sits in `while (true)` — so keep-alives are only needed for the ROM path.

### 7.2 "Stay in bootloader on next boot" via eFuse / RTC

- **eFuse:** ESP32-C3 has `DIS_DOWNLOAD_MODE` (burn once, permanent) — do NOT touch.
  There is no "force next boot into DFU" eFuse.
- **RTC register:** `RTC_CNTL_OPTION1_REG` bit 0 =
  `RTC_CNTL_FORCE_DOWNLOAD_BOOT`. Write-clear, survives one reset. This is precisely
  what `esp_rom_uart_download_mode()` from ESP-IDF does internally. But to exploit
  it we need running app code — see §1.5. The ELRS in-app stub's custom opcodes
  would have to be patched-in to set this.

### 7.3 Live-state reflash (a.k.a. "don't kill what's already working")

If the plate is already in CRSF-app mode (not DFU) and wants to re-verify the flash
WITHOUT disrupting RC control, we can:

- Send `CRSF_FRAMETYPE_DEVICE_PING` and parse `DEVICE_INFO` (sw version). No flash
  access, no interruption.
- If version doesn't match expected: only then send `bl` + flash. That is: **verify
  by identity, not by hash, unless we care about tampering.**

### 7.4 Recommendation

- Add 2-s ping-NOP timer during ROM-DFU dump / long reads.
- Use `DEVICE_INFO` ping as cheap "still the right firmware?" check before every
  session.

### 7.5 Effort

~2 h.

---

## Implementation priority for the plate

| Feature | Enables | Effort | Priority |
|---------|---------|--------|----------|
| CRSF `bl` DFU entry (§1.1/1.7) | Flash WITHOUT holding BOOT | 2 h | **P0** |
| SPI_FLASH_MD5 verify (§6.3) | 600× faster verify | 3 h | **P0** |
| `DEVICE_INFO` listener post-reboot (§2.2) | Confirm app boot without hint | 3 h | **P1** |
| Patch hardware.json into firmware.bin before flash (§3.3 B) | First-boot works headless | 4 h | **P1** |
| S3 joins RX AP and POSTs hardware.json (§3.3 A) | Recovery from bare flash | 3 h | P2 |
| CRSF keep-alive NOPs (§7.4) | Safety during slow dump | 2 h | P2 |
| Pre-baked target presets (§5) | 10+ RX brands work OOB | 2 h | P2 |
| MILELRS param-tree badge (§4.7) | UX polish | 3 h | P3 |

**Total P0+P1: ~15 h.** After those land, every flash/verify cycle is
power-cycle-free for any live vanilla or MILELRS RX, and first-boot is hands-off.

---

## Sources

- [ExpressLRS source — telemetry.cpp `b/l` handler](https://github.com/ExpressLRS/ExpressLRS/blob/master/src/lib/Telemetry/telemetry.cpp) (local copy: `hardware/bayckrc_c3_dual/elrs_3_6_3_src/src/lib/Telemetry/telemetry.cpp:226-231`)
- [ExpressLRS source — SerialUpdate in-app esptool stub](https://github.com/ExpressLRS/ExpressLRS/blob/master/src/lib/SerialUpdate/stub_flasher.cpp) (local copy: `hardware/bayckrc_c3_dual/elrs_3_6_3_src/src/lib/SerialUpdate/`)
- [ExpressLRS WiFi / hardware.json HTTP route](https://github.com/ExpressLRS/ExpressLRS/blob/master/src/lib/WIFI/devWIFI.cpp)
- [ELRS python bootloader init seq](https://github.com/ExpressLRS/ExpressLRS/blob/master/src/python/bootloader.py) (local copy: `hardware/bayckrc_c3_dual/elrs_3_6_3_src/src/python/bootloader.py`)
- [ExpressLRS Targets catalogue](https://github.com/ExpressLRS/targets/blob/master/targets.json)
- [Web Configuration Interface — DeepWiki ExpressLRS](https://deepwiki.com/expresslrs/expresslrs/5.1-web-configuration-interface)
- [Esptool boot mode selection — ESP32-C3](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html)
- [Esptool serial protocol — ESP32-C3](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/serial-protocol.html)
- [Esptool entering bootloader — ESP32-C3](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/entering-bootloader.html)
- [Esptool flasher stub](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/flasher-stub.html)
- [ESP-IDF ESP32-C3 bootloader](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/bootloader.html)
- [esp-serial-flasher (Espressif, MCU-to-MCU flashing reference)](https://github.com/espressif/esp-serial-flasher)
- [ESP32 strapping pins 2026 guide](https://www.espboards.dev/blog/esp32-strapping-pins/)
- [Cybershafarat: MILELRS – MILBETA – FPV_VYZOV ecosystem](https://cybershafarat.com/2025/10/05/russian-fpv-drone-tactical-ecosystem-milelrs-milbeta-fpv_vyzov/)
- [ExpressLRS Gemini docs](https://www.expresslrs.org/software/gemini/)
- [Oscar Liang — RadioMaster XR1/XR2/XR3/XR4 review](https://oscarliang.com/radiomaster-xr1-xr2-xr3-xr4-elrs-receivers/)
- [Oscar Liang — Flash ExpressLRS via UART](https://oscarliang.com/flash-expresslrs-receivers-uart/)
- [multirotorguide — ExpressLRS hardware list](https://www.multirotorguide.com/news/list-of-expresslrs-hardware-transmitters-and-receivers/)
- [BAYCKRC Amazon listing](https://www.amazon.com/BAYCKRC-2400Mhz-receiver-Band-Receiver/dp/B0F438YNHZ)
- [ESP32-C3 trigger download boot from software — forum](https://esp32.com/viewtopic.php?t=33238)
- [ESP32-C3 can't leave download mode — forum](https://www.esp32.com/viewtopic.php?t=35145)
- [Local: Bayck RC C3 dump analysis](../hardware/bayckrc_c3_dual/ANALYSIS.md)
- [Local: deep-scan of dump](../hardware/bayckrc_c3_dual/dump_2026-04-19_1528.deepscan.md)
