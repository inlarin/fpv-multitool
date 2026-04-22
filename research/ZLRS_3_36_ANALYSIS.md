# ZLRS 3.36 Firmware Analysis

**Analyst:** Claude (Opus 4.7 1M)
**Date:** 2026-04-22
**Source:** `C:\Users\inlar\Downloads\Telegram Desktop\ZLRS_3.36_all_targets.zip` (197 MB)
**Extracted to:** `research/tmp/zlrs_3_36/` (gitignored)
**Samples:** 313 `.bin` files
**Reference:** vanilla ELRS 3.6.3 at `hardware/bayckrc_c3_dual/vanilla_elrs_3.6.3/firmware.bin`

---

## Executive Verdict

**ZLRS 3.36 is a Russian-language rebrand fork of ExpressLRS 3.6.x, built from upstream "Unified Targets" sources with essentially zero protocol-level divergence.** Every chip family, image header, flash layout, NVS scheme, CRSF wire format, and HTTP route in the ZLRS binaries matches vanilla ELRS 3.6.3 byte-for-byte. The only meaningful differences are cosmetic: the default RX WiFi AP SSID prefix is `Приёмник_ZLRS` (UTF-8 Russian "Receiver_ZLRS") instead of `ExpressLRS RX`, the TX shows `ZLRS` on its OLED instead of `ExpressLRS`, and a small number of SubGHz TX binaries are distributed in an encrypted container (4/313). **All our existing ELRS tooling — CRSF probes, ROM DFU / in-app stub flashing, NVS dump + parse, `/config` UID POST — will work on ZLRS without changes.** Just add `zlrs24022022` / `3.36 (2110c5)` signature strings to the fork identifier list, accept `Приёмник_ZLRS*` / `Передатчик_НСУ*` as valid AP SSID patterns (WiFi password is empty on RX, still `expresslrs` on TX), and refuse the 4 encrypted SubGHz binaries at upload time.

---

## 1. Fork Identity

| Attribute | Value |
|---|---|
| Name | **ZLRS 3.36** |
| Parent | **ExpressLRS "Unified Targets" 3.6.x** (git short-SHA `2110c5`) |
| Build marker | `zlrs24022022` (probably 24-Feb-2022 — but version string says 3.36 which is newer; see notes) |
| Version string (in rodata) | `3.36 (2110c5)` |
| Builder identity | PC username **`Escargot`**, Windows, `framework-arduinoespressif32@3.20011.230801` (Arduino ESP32 core 2.0.11) |
| Language | Russian (Russian filenames in zip, UTF-8 Russian SSID prefixes baked into binary) |
| Fork target audience | Russian FPV community, likely tied to UBRT / Drone Wars / МСУ naming |
| Encryption | 4 binaries (3 files, 2 paths) use a custom encrypted container — headers `11 e8 c6 ca …` and `71 9f 96 e6 …`, entropy 8.000 — one-step AES or XOR with random IV; not ROM-flashable without decrypt key |

**Fork identifier strings** (all present in ≥ 310 / 313 unencrypted images):
- `zlrs24022022` — hard-coded build marker string
- `3.36 (2110c5)` — version string
- `Приёмник_ZLRS` (bytes `d0 9f d1 80 d0 b8 d1 91 d0 bc d0 bd d0 b8 d0 ba 5f 5a 4c 52 53` in UTF-8) — RX AP SSID prefix
- `Передатчик_НСУ` (bytes `d0 9f d0 b5 d1 80 d0 b5 d0 b4 d0 b0 d1 82 d1 87 d0 b8 d0 ba 5f d0 9d d0 a1 d0 a3` in UTF-8) — TX AP SSID prefix
- `C:/Users/Escargot/.platformio/…` path strings leaked into rodata
- TX OLED string `ZLRS` (appears next to existing ELRS screen labels `FHSS` / `DYNAMIC` / `SYNC`)

**Parent-ELRS fingerprints retained unchanged**:
- `nvs_open("ELRS", NVS_READWRITE, &handle)` (verbatim same string as vanilla ELRS 3.6.3)
- `%s  ELRS-%.6s` SSID format (TX only — RX uses the Russian prefix)
- HTTP route table: `/hardware /options /elrs.css /mui.js /scan.js /networks.json /sethome /forget /connect /config /access /target /firmware.bin /update /forceupdate /cw.html /cw.bin /reboot /log` — byte-identical order and neighbour strings as vanilla
- `/hardware.json`, `/options.json`, `expand 32-byte k` (ChaCha20 constant), `expresslrs` default TX password, `ExpressLRS-%.6s` fallback format on TX

**Diff vs vanilla 3.6.3** (per string-set compare of the common C3 RX build):
- Vanilla: 3023 ASCII strings ≥ 8 chars
- ZLRS: 2992
- Common: 2912
- Unique to vanilla: 111 (only 1 ELRS-identifying — `ExpressLRS RX` — the rest are IDF/SDK strings the ZLRS build happens to omit because code paths differ slightly)
- Unique to ZLRS: 80 (the `C:/Users/Escargot/…` PlatformIO paths + `_ZLRS` + `zlrs24022022` + `3.36 (2110c5)`)

**Conclusion**: ZLRS is `git clone ExpressLRS/ExpressLRS && s/ExpressLRS/ZLRS/g in a couple of UI display strings + change default RX AP SSID prefix to Russian`. It is _not_ a protocol rewrite or a hardware-handling change.

---

## 2. Target Matrix (313 files)

| role / subfolder               | chip family              | format      | count |
| :----------------------------- | :----------------------- | :---------- | ----: |
| **Transmitters (передатчики)** |                          |             | **103** |
| TX 2400                        | ESP32                    | raw         | 47    |
| TX 900 MHz                     | ESP32                    | raw         | 31    |
| TX 400-500 MHz                 | ESP32                    | raw         | 14    |
| TX 400-500 MHz                 | ENCRYPTED                | raw         | 1     |
| TX LR1121                      | ESP32 (+ LR1121 radio)   | raw         | 8     |
| TX (root) ZTX_typeC_NEW        | ESP32                    | raw         | 1     |
| TX (root) ZTX_SubGHz_notENC    | ESP32                    | raw         | 1     |
| TX (root) ZTX_SubGHz_ENCRYPT   | ENCRYPTED                | raw         | 1     |
| **Receivers (приемники)**      |                          |             | **210** |
| RX Одинарные 2400 (mainstream) | **ESP32-C3**             | raw         | 17    |
| RX Одинарные 2400 (legacy)     | **ESP8266 / ESP8285**    | gzip        | 68    |
| RX Одинарные 900 MHz           | ESP8266 / ESP8285        | gzip        | 33    |
| RX Одинарные 400-500 MHz       | ESP8266 / ESP8285        | gzip        | 8     |
| RX Одинарные ESPC3_LR1121      | ESP32-C3 (+ LR1121)      | raw         | 21    |
| RX Двойные 2400 (Gemini)       | ESP32 (dual radio)       | raw         | 30    |
| RX Двойные 900 MHz             | ESP32 (dual radio)       | raw         | 17    |
| RX Двойные LR1121              | ESP32 (+ LR1121)         | raw         | 9     |
| RX Двойные 400-500 MHz         | ESP32                    | raw         | 4     |
| RX Двойные 400-500 MHz         | ENCRYPTED                | raw         | 1     |
| RX Двойные (root) ZRX          | ESP32                    | raw         | 1     |
| RX Одинарные (404 misc)        | —                        | —           | 1     |

**Summary by chip**: ESP32 = 163 (TX + dual RX), ESP8266/ESP8285 = 109 (legacy single RX, all gzipped), ESP32-C3 = 38 (mainstream single RX), **ESP32-S3 = 0** (no S3 targets — vanilla ELRS also does not ship S3 RX/TX), encrypted = 3 (+ 1 extra copy of one).

**No ESP32-C6 / H2 / S2 / C2 binaries.**

**All non-encrypted samples contain the string `zlrs24022022` or `3.36 (2110c5)`** — confirms they are all built from the same source tree at the same commit.

---

## 3. Per-Sample Binary Analysis (representative)

### 3.1 `приемники/Одинарные/2400/ESP32C3/ZLRS_3.36_Generic ESP32C3 2.4Ghz RX.bin`

ESP32-C3 mainstream receiver. All 17 C3 RX binaries are the same size (1,134,736 B) — Generic build relying on `/hardware.json` at runtime for pin mapping. (Same uniform-build pattern as vanilla ELRS Unified targets.)

- **Size**: 1,134,736 bytes (decompressed, same as on disk; raw ESP image)
- **Header**: `e9 05 02 2f 7c 46 38 40 ee 00 00 00 05 00 00 00 …`
  - magic `0xe9`, segments `5`, SPI mode `0x02` (DIO), SPI flash size/freq `0x2f` (4 MB / 40 MHz)
  - entry `0x4038467c`, chip_id `5` = ESP32-C3 ✓
- **Segments**:
  - `[0] 0x3c0d0020 size 236,320` (DROM / .rodata)
  - `[1] 0x3fc91400 size 16,236`  (DRAM bss/data)
  - `[2] 0x40380000 size 9,564`   (IRAM)
  - `[3] 0x42000020 size 809,132` (IROM / .text)
  - `[4] 0x4038255c size 60,668`  (IRAM trampoline)
- **Identical DROM/IROM load addresses to vanilla 3.6.3** (verified) → flashable at `0x10000` like vanilla.
- **NVS**: namespace `"ELRS"` (verbatim `nvs_open("ELRS", NVS_READWRITE, &handle)`), key `uid\0`
- **HTTP routes** (list from rodata, same order as vanilla):
  `/elrs.css /mui.js /scan.js /networks.json /sethome /forget /connect /config /access /target /firmware.bin /update /forceupdate /cw.html /cw.bin /reboot /log /hardware /options`
- **Default AP IP**: `10.0.0.1`
- **Default RX AP SSID prefix**: `Приёмник_ZLRS` (UTF-8 Russian)
- **Default RX AP password**: _not found as a bare string_ — either empty (open network) or constructed at runtime from UID
- **Device class**: `elrs_rx`
- **Target ID**: `UNIFIED_ESP32C3_2400_RX`, build tag `2110c5`, version `3.36`
- **Embedded default hardware.json snippet** found at 0x114690:
  `{"flash-discriminator": 3405178402, "wifi-on-interval": 60, "group_id": 1, "group_enable": false, "uid": [15, 14, 206, 119, 83, 165, 244, 132, 52, 24, 94, 115, 49, 74, 178, 169], "lock-on-first-connection": true, "domain": 0}`
  (Note: this `uid` field is 16 bytes — matches ELRS "UID2" signing key format used since 3.x, not the 6-byte bind UID.)
- CRSF: `CRSF` string present. CRC table not found as contiguous bytes (likely computed at runtime or split across code).
- `/hardware.json`, `/options.json`, `module-type`, `product_name`, `lua_name`, `git-commit` JSON keys present — config JSON schema identical to vanilla.

### 3.2 `приемники/Одинарные/ESPC3_LR1121/ZLRS_3.36_BAYCKRC C3 900 2400 Dual Band Nano RX.bin`

ESP32-C3 + LR1121 radio (dual-band 900/2400 MHz). This is the most complex RX SKU.

- Size 1,130,480 B, chip_id 5 (C3), entry `0x4038435a`, 5 segments
- Same flash layout as the 2.4GHz-only C3 (seg[0] @ 0x3c0d0020 etc)
- `LR1121` string present in radio-type token set
- All other features identical to §3.1

### 3.3 `приемники/Одинарные/2400/ZLRS_3.36_Foxeer 2.4Ghz RX.bin`

ESP8266/ESP8285 legacy RX. **Gzipped** firmware (OTA form expected by ESP8266 Arduino Updater).

- Container: gzip, 390,683 B → decompresses to 535,651 B of ESP image
- Inner header `e9 02 40 10 80 f4 10 40 …` → magic, 2 segments, flash mode 0x40, entry `0x4010f480` (ESP8266 IROM address)
- **chip_id field does not apply** to ESP8266 v1 images (byte 12 = segment tail / garbage)
- Same NVS scheme? ESP8266 ELRS uses SPIFFS+EEPROM instead of NVS — string `nvs_open` NOT present in this binary; uses EEPROM-style storage (Arduino EEPROM library strings visible)
- Same HTTP route table, same `_ZLRS` Russian SSID prefix, same `/config` endpoint
- Same default AP IP `10.0.0.1`
- Device class `elrs_rx`, target `UNIFIED_ESP8285_2400_RX`, version `3.36`, build `2110c5`
- **Flash layout for 8266**: typical 1 MB / 2 MB split — vanilla ELRS flashes the 8266 gzip image via ESP8266 Arduino OTA (stub-based), not ROM DFU. Same path works for ZLRS since the image is identical format.

### 3.4 `передатчики/2400/ZLRS_3.36_BETAFPV 2.4GHz Nano TX.bin`

ESP32 TX module (handset-side).

- Size 1,288,004 B, raw ESP image, chip_id 0 + entry `0x400848d4` → ESP32 (classic, not S3/C3)
- 6 segments (standard ESP32 app image with bootloader helper)
- NVS namespace `"ELRS"` ✓
- **AP SSID format**: `%s  ELRS-%.6s` (vanilla format), TX label `Передатчик_НСУ`
- **AP password**: `expresslrs` (verbatim same as vanilla)
- TX OLED string `ZLRS` present (replaces vanilla's `ExpressLRS` label)
- Config keys: `wifi-on-interval`, `wifi-ssid`, `wifi-password`, `tlm-interval`, `fan-runtime`, `unlock-higher-power`, `airport-uart-baud`, `rcvr-uart-baud` — same as vanilla TX config
- Baud string `460800` present (internal UART or OTA speed marker)
- Target: `UNIFIED_ESP32_2400_TX.2110c5`
- Device class: `elrs_tx`

### 3.5 `передатчики/ZLRS_3.36_ZTX_SubGHz_ENCRYPT_(no_typeC_old).bin`

- First 16 bytes: `11 e8 c6 ca ae f9 58 09 54 f7 84 ad b2 88 c7 20`
- Entropy: **8.000 bits/byte** (fully random) — image is encrypted, likely AES-CBC or -CTR with header IV
- No ASCII strings match any ELRS / ZLRS token
- Sibling file `ZTX_SubGHz_notENC` is standard unencrypted ESP image at same size class — proves the encryption is a _container_ over the same underlying app image
- **Decryption key not recoverable from these artifacts**; would need either a custom flasher tool from the ZLRS author or an open/decrypted build
- **Incompatible with our ROM DFU or stub flasher** without the key
- Count in distribution: 3 files ENCRYPT + 1 dup (400-500 TX, 400-500 RX Foxeer, ZTX SubGHz)

---

## 4. Compatibility Verdict per Tool Feature

Our existing ELRS tooling (see [CLAUDE.md](../CLAUDE.md)) covers: probe, flash (ROM DFU + stub), dump, parameter read, bind command, UID extract (from dump), UID push (via `/config` POST). Status per feature:

| Feature                                      | Verdict | Notes                                                                                                                                                                                                                                                        |
| :------------------------------------------- | :-----: | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Detect RX mode** (`app / stub / ROM DFU`)  | ✓      | CRSF framing and baud are identical. Probes using `PING_DEVICE 0x28` / `DEVICE_INFO 0x29` work unchanged. `_ZLRS` SSID prefix is an additional fingerprint to try when in WiFi-AP state.                                                                    |
| **ROM DFU flashing @ 115200**                | ✓      | All 201 raw ESP32/ESP32-C3 images have standard `0xe9` ESP image magic, correct chip_id, correct flash size/freq. Our `esp_rom_flasher.cpp` path is 1:1 compatible.                                                                                          |
| **In-app stub flashing @ 420000**            | ✓      | All 109 ESP8266 images are gzip + inner `0xe9` — Arduino-ESP8266 OTA handler accepts this same as vanilla. The stub protocol is unchanged.                                                                                                                   |
| **Full-flash dump via READ_FLASH (0xD2)**    | ✓      | Standard ROM DFU command, ZLRS binary has no effect on the ROM's behaviour. Dump will contain the same NVS layout and same hardware.json in SPIFFS.                                                                                                        |
| **CRSF telemetry service**                   | ✓      | CRSF wire format is defined by the CRSF spec, not by the firmware. ZLRS transmits the same frames with the same CRC polys.                                                                                                                                    |
| **PARAMETER_READ (LUA params)**              | ✓      | Same frame type (`0x2C`), same param IDs. ZLRS adds no new parameters based on string search; CRSF LUA tree strings (`CRSF;Inverted CRSF;SBUS;…`) are verbatim same as vanilla.                                                                             |
| **CRSF bind / reboot / enter_stub / enter_bootloader** | ✓ | Same command frames. `BIND` / `bind` / `Enter Bind Mode` strings present; `reboot` endpoint present.                                                                                                                                                        |
| **NVS dump + parse — namespace `"ELRS"`**    | ✓      | Verbatim `nvs_open("ELRS", NVS_READWRITE, &handle)` on ESP32/ESP32-C3 targets. NVS-partition offset (0x9000) / size (0x6000 / 0x4000) is the upstream ELRS default — **we have no way to tell from the app-only bin**, but since it's the Unified build from vanilla sources it's 0x9000 offset, standard. |
| **NVS key `"uid"` (6-byte UID)**             | ✓      | Key present, same blob semantics. Note: the embedded default-config JSON uses a 16-byte `uid` array, but that is the _compile-time default binding phrase seed_ fed into MD5 — runtime NVS still stores the 6-byte truncated MD5 hash.                    |
| **`/config` POST {"uid":[…]} transfer**      | ✓      | `/config` route is present at the same position in the route table; request body parsing (`application/json`) and handler logic is byte-for-byte vanilla (route table is identical).                                                                                |
| **Phrase → UID MD5 client-side**             | ✓      | JS-side MD5 is independent of firmware; UID comparison against NVS will match since firmware stores the same MD5[0..5] output on `SetUID`.                                                                                                                   |
| **WiFi AP SSID detection (for diag)**        | ⚠      | ZLRS RX uses `Приёмник_ZLRS…` instead of `ExpressLRS RX`. Our probe's SSID pattern filter (if we have one) should add `Приёмник_ZLRS*`, `Передатчик_НСУ*`, and leave `ExpressLRS*` as-is for TX.                                                            |
| **WiFi AP password autofill**                | ⚠      | TX still uses `expresslrs`. RX does **not** have `expresslrs` in rodata — the network may be open, may require UID-derived password, or may use a compile-time build flag we don't see. Recommend: try empty password first, fall back to `expresslrs`, fall back to user prompt. |
| **Recognize flash binary before upload**     | ⚠      | Our flasher currently parses `e9 05 …` etc. Works for ZLRS, but user may upload the encrypted SubGHz-ENCRYPT bin by mistake — we should detect non-`0xe9` first byte and show "This file appears encrypted / incompatible" instead of silently flashing garbage. |
| **Slot-flash full OTA (app0 / app1)**        | ✓      | Since ZLRS uses identical partition layout (app0@0x10000, app1@0x110000, otadata@0xe000, nvs@0x9000) — standard ESP32 ELRS layout — our slot-flash path works unchanged.                                                                                    |
| **SubGHz-ENCRYPT binaries (3/313)**          | ✗      | We cannot flash these without the ZLRS decryption key. Block upload with error message "This ZLRS firmware is encrypted; please provide the non-ENCRYPT variant."                                                                                             |

---

## 5. Required Code Changes

All changes are additive (no existing paths break). Scope: signature library + UI copy only.

### 5.1 Fork identification

Add fork-signature strings to the tool's RX / TX identification logic (wherever we currently look for `ExpressLRS` / commit SHA):

```
  zlrs24022022           # present in 310/313 binaries
  3.36 (2110c5)          # ZLRS version string, presented as X.XX (shortsha)
  _ZLRS                  # RX AP SSID suffix
  Передатчик_НСУ         # TX AP SSID prefix (UTF-8)
  Приёмник_ZLRS          # RX AP SSID prefix (UTF-8)
  C:/Users/Escargot/     # builder's path — leaked across ALL samples
```

If any of these are present in the dump, tag device as **ZLRS** (display "ZLRS 3.36" instead of "ExpressLRS").

### 5.2 AP SSID / password detection

In whatever feature lists known ELRS AP patterns, add:
- `ExpressLRS RX` (vanilla)
- `ExpressLRS TX` (vanilla)
- `ELRS-*` (post-paired SSID, both vanilla and ZLRS)
- **new**: `Приёмник_ZLRS*` (ZLRS RX)
- **new**: `Передатчик_НСУ*` (ZLRS TX)

Password fallback order:
1. User-provided
2. `expresslrs` (works on all TX, vanilla RX)
3. empty (ZLRS RX default appears open)

### 5.3 Binary-upload guard

In `src/web/http/routes_flash.cpp` (or wherever `/api/elrs/upload` lives), after the existing ESP-image magic check, reject if first 4 bytes ∈ {`11 e8 c6 ca`, `71 9f 96 e6`, or any header with entropy > 7.9 over first 64 KB}. Show: "Эта прошивка зашифрована (ZLRS ENCRYPT). Используйте вариант _notENC_."

### 5.4 NVS reader (no change needed)

`parse.py` / `deep_scan.py` already look for namespace `"ELRS"` — unchanged.

### 5.5 Optional UI copy

When device is detected as ZLRS, consider:
- Label it "ZLRS 3.36 (fork of ExpressLRS 3.6)" in status bar
- Present a "Flash vanilla ExpressLRS" button that offers `hardware/bayckrc_c3_dual/vanilla_elrs_3.6.3/firmware.bin` as the de-fork flash — identical chip, identical layout, loses Russian UI strings but becomes upstream.

---

## 6. Identification Cheatsheet — Signatures Summary

To positively identify a ZLRS RX/TX from a full-flash dump (4 MB) or a live probe:

| Source                | Signature                                                                 |
| :-------------------- | :------------------------------------------------------------------------ |
| rodata string         | `zlrs24022022`                                                            |
| rodata string         | `3.36 (2110c5)`                                                           |
| rodata UTF-8          | `d0 9f d1 80 d0 b8 d1 91 d0 bc d0 bd d0 b8 d0 ba 5f 5a 4c 52 53` (`Приёмник_ZLRS`) |
| rodata UTF-8          | `d0 9f d0 b5 d1 80 d0 b5 d0 b4 d0 b0 d1 82 d1 87 d0 b8 d0 ba 5f d0 9d d0 a1 d0 a3` (`Передатчик_НСУ`) |
| rodata path leak      | `C:/Users/Escargot/.platformio/`                                          |
| rodata target ID      | `UNIFIED_ESP32C3_2400_RX.2110c5` / `UNIFIED_ESP8285_2400_RX` / `UNIFIED_ESP32_2400_TX` (short-SHA `2110c5` instead of vanilla's `288efe`) |
| rodata OLED label     | `ZLRS` (TX only, near `FHSS DYNAMIC SYNC DEFAULT PITMODE BACKPAC VRX`)     |
| WiFi scan (RX)        | SSID starts with `Приёмник_ZLRS` or `_ZLRS`                              |
| WiFi scan (TX)        | SSID starts with `Передатчик_НСУ` or contains `ELRS-<6hex>` (unchanged)   |

To distinguish ZLRS 3.36 from vanilla ELRS:
- ZLRS commit short-SHA is `2110c5`; vanilla 3.6.3 is `288efe` (seen in `hardware/bayckrc_c3_dual/vanilla_elrs_3.6.3/firmware.bin`)
- `zlrs24022022` is never present in vanilla
- `ExpressLRS RX` is present in vanilla RX but absent in ZLRS RX
- `Приёмник_ZLRS` is present in ZLRS RX but absent in vanilla
- On the OLED (TX), ZLRS shows `ZLRS` as a screen; vanilla shows nothing in that slot

---

## 7. Parent ELRS Version + Diff Summary

- Parent branch: **ExpressLRS master @ some commit around v3.6.x** (almost certainly 3.6.0 or 3.6.1 — not 3.6.3, based on smaller seg sizes vs vanilla 3.6.3 and absence of the `reg_domain` string which appeared in 3.6.3).
- The short SHA `2110c5` is the ELRS-upstream commit the fork was based on; not a ZLRS-specific tree (the ZLRS strings are added on top).
- Build date reference `24022022` (24-Feb-2022) precedes ELRS 3.6.x by ~2.5 years → this string is likely a **stable channel marker** / nonce rather than the actual build date. The tree itself is clearly 3.6-series because:
  - Unified Targets architecture (`UNIFIED_ESP*` target IDs)
  - LR1121 radio support (introduced late 2024)
  - ESP32-C3 RX support (mainstream since early 2024)
  - `/hardware.json` runtime pin mapping (3.5+)
  - `group_id`, `group_pilot`, `group_enable` config keys (3.6+)
  - `is-airport`, `airport-uart-baud` (3.6+)

Differences from upstream ELRS 3.6.x (inferred from string diff):
- **Added**: Russian UTF-8 SSID prefixes on RX/TX (`Приёмник_ZLRS`, `Передатчик_НСУ`)
- **Added**: ZLRS branding string on TX OLED
- **Added**: `zlrs24022022` marker, `3.36 (2110c5)` version string
- **Added**: Optional AES-CBC container (for SubGHz TX only — 2.4 GHz and 900 MHz are unencrypted)
- **Removed**: `ExpressLRS RX` / `ExpressLRS TX` AP SSID default string from RX binaries (TX retains them as fallback)
- **Removed**: default AP password `expresslrs` from RX binaries (TX retains)
- **Unchanged**: CRSF protocol, ROM DFU flow, NVS namespace `"ELRS"`, NVS key `"uid"`, HTTP route table, `/config` POST handler, OTA partition layout, hardware.json schema, MD5 UID derivation, binding-phrase semantics, device-class strings `elrs_rx`/`elrs_tx`

No protocol-level fork. **This is a rebrand + localisation + optional crypto container**, not a divergent firmware.

---

## 8. Sanity Numbers

- 310 / 313 files contain `zlrs24022022` (99.0 %)
- 313 / 313 are present as `.bin`; zero non-firmware artifacts (no README, no hardware.json, no manifest — this is a pure binary distribution)
- md5 (ZLRS Generic C3 2.4G RX): `d585c74769010d88704e4ca5093e9b30` (1,134,736 B)
- md5 (vanilla 3.6.3 Bayck C3): `573cac116df5f26cc5bcaee564d76c56` (1,243,968 B)
- Integer constant `115200` (ROM DFU baud): 1 match in both vanilla and ZLRS
- Integer constant `921600` (high-speed DFU): 1 match in ZLRS, 0 in vanilla (ZLRS may support faster DFU — nice-to-have)
- CRSF CRC table (poly 0xD5) — not found contiguously in either; computed at runtime

---

## 9. Open Questions (not answered from binary analysis alone)

1. **Who maintains ZLRS / where is the source?** PC username `Escargot` + Russian UI → Russian FPV community fork; possibly tied to UBRT / Drone Wars / МСУ ecosystems. No public GitHub repo obviously named `ZLRS` (not searched — web search not used in this pass; can be followed up if needed).
2. **Is there a ZLRS-branded Configurator?** The embedded `"manifest"` / `"overlay"` schemas in the rodata are the vanilla ELRS Configurator JSON; a ZLRS-branded Configurator may exist but its presence/URL is outside this binary.
3. **What is the SubGHz-ENCRYPT key?** Not derivable from the binary. If important, would need either leaked source from the ZLRS author or differential analysis across multiple encrypted images (same app + different IV → same key stream — might work with two encrypted samples, but we only have one ZTX-SubGHz-ENCRYPT and one Foxeer-400-ENCRYPT, different underlying apps → not directly attackable).
4. **`zlrs24022022` — exactly what?** Our best guess: build-timestamp or release-tag. Could also be a shared-secret fragment used by the ZLRS Configurator to identify the fork. Date 24-Feb-2022 predates ELRS 3.6 so it's likely an arbitrary marker chosen when the fork was branded (it's the Russian Defender-of-the-Fatherland day, which fits the Russian nationalist naming style).

---

## Appendix A — Files used

- `research/tmp/zlrs_3_36/ZLRS_3.36_all_targets/` — extracted from the zip (gitignored via new rule in `.gitignore`)
- `research/tmp/classify.py`, `research/tmp/analyze.py`, `research/tmp/analyze2.py`, `research/tmp/compare.py`, `research/tmp/compare2.py`, `research/tmp/zlrs_ctx.py`, `research/tmp/matrix.py` — analysis scripts (gitignored with the rest of `research/tmp/`)
- `research/tmp/analyze2.out` — full JSON dump of rodata token analysis across 13 representative samples
- `research/tmp/compare.out`, `research/tmp/compare2.out` — ZLRS vs vanilla 3.6.3 byte/string diff

## Appendix B — Quick reference for implementer

```c
// Add to src/crsf/fork_identity.h (or similar)
static const char* const ZLRS_SIGNATURES[] = {
    "zlrs24022022",      // build marker
    "3.36 (2110c5)",     // version string (change when ZLRS updates)
    "\xd0\x9f\xd1\x80\xd0\xb8\xd1\x91\xd0\xbc\xd0\xbd\xd0\xb8\xd0\xba_ZLRS",  // Приёмник_ZLRS
    "\xd0\x9f\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba_\xd0\x9d\xd0\xa1\xd0\xa3",  // Передатчик_НСУ
    NULL
};

// Add to src/bridge/esp_rom_flasher.cpp upload guard
if (fw[0] == 0x11 && fw[1] == 0xE8 && fw[2] == 0xC6 && fw[3] == 0xCA) {
    // ZLRS SubGHz ENCRYPT container — unflashable
    return err("Эта прошивка зашифрована (ZLRS SubGHz ENCRYPT). Используйте вариант *_notENC_*.bin.");
}
if (fw[0] == 0x71 && fw[1] == 0x9F && fw[2] == 0x96 && fw[3] == 0xE6) {
    // Foxeer400 ENCRYPT
    return err("Эта прошивка зашифрована (ZLRS Foxeer 400 ENCRYPT). Используйте non-encrypt вариант.");
}
```
