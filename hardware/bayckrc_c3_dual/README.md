# Bayck RC C3 Dual — ELRS Receiver Workbench

**Target:** Bayck RC C3 Dual receiver (ESP32-C3, dual antenna/dual-band ELRS).

This folder holds dumped firmware images, extracted secrets, and analysis
artefacts for one specific receiver. Do not commit raw `.bin` dumps
unless the user opts in — they contain the user's WiFi password.

## Workflow

### 1. Physical wiring

Connect the receiver's UART pads to the FPV MultiTool's **Port B**:

| Receiver pad | MultiTool pin | Note |
|---|---|---|
| RX            | GP11 (Port B pin_a) | ESP TX → receiver RX |
| TX            | GP10 (Port B pin_b) | receiver TX → ESP RX |
| GND           | GND | common ground required |
| 5V / 3.3V     | 5V (or 3V3) | powers receiver BEC |

### 2. Put receiver in ROM bootloader

Hold the DFU/BOOT button on the receiver while applying power (or while
pressing the receiver's reset). On ESP32-C3 this pulls GPIO9 low at boot.

### 3. Dump the flash

**Option A — on-device via Web UI (ELRS tab → Dump firmware):**

1. System → Port B Mode → **UART** (Apply — reboot if prompted)
2. ELRS Flash → pick size (usually 4 MB for C3) → **Dump firmware**
3. Wait for progress bar. Once done, click **Download dump.bin**.
4. Save alongside this README as `dump_YYYY-MM-DD.bin`.

**Option B — PC-assisted via USB2TTL bridge (faster, uses esptool.py):**

1. System → USB Mode → USB2TTL → Apply & reboot
2. System → Port B Mode → **UART** → Apply
3. From PC:

```bash
# Adjust COM port to match whatever Windows assigned
esptool.py --chip esp32c3 --port COM5 --baud 460800 \
    read_flash 0 0x400000 hardware/bayckrc_c3_dual/dump_$(date +%F).bin
```

### 4. Parse the dump

```bash
python hardware/bayckrc_c3_dual/parse.py hardware/bayckrc_c3_dual/dump_2026-04-19.bin
```

The parser searches the image for:

- **WiFi credentials** — ELRS stores the current SSID/password (for its
  own built-in AP used during web-UI firmware updates) in the **NVS
  partition** and sometimes in the config area. The stock AP SSID is
  usually `ExpressLRS RX <id>` with a default password, but if the user
  configured WiFi updates over home WiFi, those credentials land here
  too.
- **Firmware version string** — `ExpressLRS vX.Y.Z (branch, hash)`.
- **Target model token** — e.g. `Bayck_RC_C3_Dual_2G4`.
- **UID/binding-phrase hash** — 6-byte binding phrase SHA256 lower 6 bytes.
- **FCC/CE region flag** / TX power cap.
- **Serial numbers, MAC addresses** — embedded in NVS.

Output goes to `parsed_<dumpname>.txt` (and `.json`).

## What to keep in this folder

| File | Purpose | Commit? |
|---|---|---|
| `README.md` | this file | yes |
| `parse.py` | extractor script | yes |
| `dump_YYYY-MM-DD.bin` | raw flash dump | **no** — contains secrets |
| `parsed_YYYY-MM-DD.txt` | human-readable extract | your call |
| `parsed_YYYY-MM-DD.json` | machine-readable extract | your call |
| `annotations.md` | manual findings / RE notes | yes |

Use `.gitignore` in this folder to skip `dump_*.bin` by default.
