# Vanilla ExpressLRS 3.6.3 build — reproducibility notes

## Target

`Unified_ESP32C3_LR1121_RX_via_UART` — generic ESP32-C3 + LR1121 receiver
target. Compiled "bare" (no hardware.json baked in) — the user uploads
the pin map after first boot through the ELRS web UI.

## Source

- Upstream: <https://github.com/ExpressLRS/ExpressLRS>
- Tag: **3.6.3** (commit `288efe1acf223e479f81349d68dda5505135301a`)

## Build-time user-defines (src/user_defines.txt deltas)

The default file ships with all regulatory domains commented out,
which makes PlatformIO abort. We enable the two that cover the
bands this receiver has been used on:

```
-DRegulatory_Domain_FCC_915     (was: #-D...)
-DRegulatory_Domain_ISM_2400    (was: #-D...)
```

`FCC_915` (902–928 MHz) is the closest ELRS preset to the 890–930 MHz
range observed in the MILELRS `custom_freq` config the user had saved.
`ISM_2400` adds the 2.4 GHz band for the LR1121's second radio.

Nothing else is customised. No binding phrase, no home-WiFi creds baked
in — those are set by the user at first-boot config.

## Build command

```
cd elrs_3_6_3_src/src
echo "" | pio run -e Unified_ESP32C3_LR1121_RX_via_UART
```

The empty stdin feeds past ELRS's post-build "pick a target config"
prompt (the 22 pre-seeded hardware.json presets, e.g. `BK DB 100 GRX`)
so we get a bare binary. User configures hardware.json after first
boot via the receiver's own web UI at `http://10.0.0.1`.

## Output layout

| File | Size | Offset in final flash |
|------|------|-----------------------|
| `bootloader.bin` | 13,216 B | 0x0000 |
| `partitions.bin` | 3,072 B | 0x8000 |
| `boot_app0.bin` | 8,192 B | 0xe000 (OTADATA, seq=0 pointing to app0) |
| `firmware.bin` | 1,243,968 B | 0x10000 (app0) |

## MD5 (for integrity verification)

```
bootloader.bin  7a398b769069efa9687971217685b937
partitions.bin  2ddc2d5f85b2c5288013adbe45d9cb7b
boot_app0.bin   e6327541e2dc394ca2c3b3280ac0f39f
firmware.bin    573cac116df5f26cc5bcaee564d76c56
```

Built 2026-04-19 on Windows 11 with PlatformIO core 6.x + Espressif32
platform 6.6.x.

## Flashing via the FPV MultiTool (receiver in DFU, Port B=UART)

### Option A — overwrite app0 only, keep MILELRS as rollback

Recommended for first flash — conservative, reversible.

```bash
# 1. Erase app0 partition (1.88 MB)
curl -X POST -F "offset=0x10000" -F "size=0x1e0000" \
  http://192.168.32.50/api/flash/erase_region

# 2. Upload the binary
curl -X POST -F "firmware=@firmware.bin" \
  http://192.168.32.50/api/flash/upload

# 3. Flash at offset 0x10000
curl -X POST -F "offset=0x10000" \
  http://192.168.32.50/api/flash/start

# 4. Poll /api/flash/status until in_progress=false

# 5. Point OTADATA at slot 0
curl -X POST -F "slot=0" \
  http://192.168.32.50/api/otadata/select

# 6. Remove power from RX, re-apply (no BOOT hold)
# 7. WiFi should show SSID "ExpressLRS RX" with password "expresslrs"
```

### Option B — full flash from scratch (replaces MILELRS entirely)

**Only if option A didn't stick** (MILELRS auto-flipped OTADATA back).

```bash
# 1. Full erase (kills both apps + OTADATA + partition table)
curl -X POST -F "offset=0x0" -F "size=0x10000" \
  http://192.168.32.50/api/flash/erase_region

# 2. Write bootloader
curl -X POST -F "firmware=@bootloader.bin" \
  http://192.168.32.50/api/flash/upload
curl -X POST -F "offset=0x0" \
  http://192.168.32.50/api/flash/start
# wait for completion

# 3. Write partition table
curl -X POST -F "firmware=@partitions.bin" \
  http://192.168.32.50/api/flash/upload
curl -X POST -F "offset=0x8000" \
  http://192.168.32.50/api/flash/start
# wait

# 4. Write OTADATA (seq=0 → blank → app0 wins)
curl -X POST -F "firmware=@boot_app0.bin" \
  http://192.168.32.50/api/flash/upload
curl -X POST -F "offset=0xe000" \
  http://192.168.32.50/api/flash/start
# wait

# 5. Write firmware app0
curl -X POST -F "firmware=@firmware.bin" \
  http://192.168.32.50/api/flash/upload
curl -X POST -F "offset=0x10000" \
  http://192.168.32.50/api/flash/start
# wait, power-cycle
```

## Rollback

Full MILELRS backup at
`hardware/bayckrc_c3_dual/dump_2026-04-19_1528.bin` (md5
`55f56ed5a17bf4ed1a2b85814e812a6b`). Use option B's sequence with
the dump as `firmware` and offset `0x0` (size 4 MB) to restore the
original dual-firmware layout byte-for-byte.
