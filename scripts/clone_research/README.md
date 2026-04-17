# Clone Battery Research Toolkit

Python scripts for deep analysis of clone DJI smart batteries via the ESP32
CP2112 HID emulator. Much faster than web UI (1000+ tx/sec vs ~30/sec over HTTP).

## Prerequisites

1. **Board in USB2I2C mode** — switch via web UI tab or:
   ```
   curl -X POST -F mode=2 http://192.168.32.50/api/usb/mode
   curl -X POST http://192.168.32.50/api/usb/reboot
   ```
2. **Python deps:**
   ```
   pip install hidapi
   ```
3. **Windows:** may need the SiLabs CP210x driver; hidapi uses raw HID.
4. **USB-connected board** — scripts talk directly to CP2112 HID; web/WiFi not used.

To return board to web UI mode:
```
curl -X POST -F mode=0 http://192.168.32.50/api/usb/mode
(reboot via button since CDC is off)
```

## Scripts

| File | Purpose |
|------|---------|
| `cp2112.py` | Driver library — sync CP2112 HID ↔ SMBus |
| `scan_sbs_full.py` | Enumerate 0x00-0xFF SBS registers, report responsive ones |
| `scan_mac_full.py` | Brute-scan 0x0000-0xFFFF MAC subcommands, log non-zero responses to CSV |
| `bypass_probe.py` | Try to write sealed registers directly; detect cosmetic seals |

## Usage

```bash
cd scripts/clone_research

# 1. Self-test the driver first
python cp2112.py
# Should print voltage, SOC, device name.

# 2. Enumerate SBS registers (quick, 256 probes)
python scan_sbs_full.py

# 3. Probe seal enforcement (safe, reads and restores values)
python bypass_probe.py

# 4. Brute-scan MAC (takes 15-20 min for full 64K)
python scan_mac_full.py --from 0x0000 --to 0xFFFF --out mac_full.csv

# Narrower scans for specific ranges:
python scan_mac_full.py --from 0x0000 --to 0x00FF     # TI standard range
python scan_mac_full.py --from 0x4000 --to 0x4FFF     # DF range
python scan_mac_full.py --from 0xDA00 --to 0xDA01     # vendor-specific hint
```

## What we found on PTL BA01WM260 (Mavic 3 clone)

Initial web-based scan showed interesting vendor register signatures:
- **Reg 0x00** returns `0xCCDF` — matches RU_MAV unseal key word2 (suspicious echo)
- **Reg 0xD6** = `0xAA55`, **Reg 0xD9** ends in `0x55AA` — paired backdoor markers
- **Reg 0xD8** = `4ERPL6QEA1D6Y9` — DJI serial as ASCII (14 bytes)
- MAC 0x0000-0x00FF: nothing responds — clone doesn't implement TI MAC

Next steps: run `scan_mac_full.py` to find non-TI subcommands this chip accepts.
