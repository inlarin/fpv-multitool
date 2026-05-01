"""Battery identify + safe-read summary helper.

Usage:
  python scripts/battery_identify.py [board_ip]    (default = Waveshare from .board_ip)

Calls /api/batt/snapshot on the target board, prints a structured summary
focused on identification + safety state. NO write operations are issued.
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def get_ip() -> str:
    if len(sys.argv) >= 2:
        return sys.argv[1]
    return (ROOT / "scripts" / ".board_ip").read_text(encoding="utf-8").strip()


def fetch(url: str, timeout: float = 10) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def fmt_v(mv) -> str:
    if not mv or mv == 0xFFFF: return "-"
    return f"{mv/1000:.3f} V"


def fmt_a(ma) -> str:
    if ma is None or ma == 0xFFFF or ma == -1: return "-"
    return f"{ma/1000:+.3f} A"


def fmt_t(c) -> str:
    if c is None: return "-"
    try: return f"{float(c):+.1f} C"
    except Exception: return str(c)


def fmt_pct(p) -> str:
    if p is None or p == 0xFFFF: return "-"
    return f"{p}%"


def fmt_min(m) -> str:
    if m is None or m == 0xFFFF or m == 0: return "-"
    return f"{m} min"


def banner(title: str, char="="):
    print()
    print(char * 70)
    print(f"  {title}")
    print(char * 70)


def main() -> None:
    ip = get_ip()
    print(f"Querying battery on {ip} ...")
    try:
        s = fetch(f"http://{ip}/api/batt/snapshot")
    except Exception as e:
        print(f"FAILED: {e}")
        sys.exit(1)

    if not s.get("connected"):
        banner("NO BATTERY DETECTED", "!")
        print("  - I2C scan returned no response at 0x0B")
        print("  - Check: SCL=GP10, SDA=GP11, GND tied, battery voltage > 6V")
        print(f"  - deviceType = {s.get('deviceType')}")
        sys.exit(2)

    banner("BATTERY IDENTIFICATION")
    print(f"  Manufacturer  : {str(s.get('mfrName', '?')):>20s}")
    print(f"  Device Name   : {str(s.get('deviceName', '?')):>20s}")
    print(f"  Chemistry     : {str(s.get('chemistry', '?')):>20s}")
    print(f"  Detected model: {str(s.get('model', '?')):>20s}  (cells: {s.get('cellCount', '?')})")
    print(f"  Serial        : {s.get('serialNumber', '?'):>20}")
    print(f"  Mfr Date      : {s.get('manufactureDate', '?'):>20}  (decoded: {decode_mfr_date(s.get('manufactureDate'))})")
    print(f"  ChipType      : {str(s.get('chipType', '-')):>20s}")
    print(f"  FW Version    : {s.get('fwVersion', '-'):>20}")
    print(f"  HW Version    : {s.get('hwVersion', '-'):>20}")

    banner("SECURITY STATE")
    print(f"  Sealed        : {('YES (sealed)' if s.get('sealed') else 'NO (unsealed)'):>25s}")
    print(f"  OperationStatus     : {s.get('opStatus', '-')}")
    print(f"  ManufacturingStatus : {s.get('mfgStatus', '-')}")

    banner("LIVE READINGS")
    print(f"  Pack Voltage  : {fmt_v(s.get('voltage_mV', 0)):>20s}    Design: {fmt_v(s.get('designVoltage_mV', 0))}")
    print(f"  Current       : {fmt_a(s.get('current_mA')):>20s}    Avg: {fmt_a(s.get('avgCurrent_mA'))}")
    print(f"  SOC (display) : {fmt_pct(s.get('soc')):>20s}    Absolute: {fmt_pct(s.get('absoluteSOC'))}")
    print(f"  Capacity rem  : {s.get('remainCap_mAh', '?')} mAh of {s.get('fullCap_mAh', '?')} mAh full  (design: {s.get('designCap_mAh', '?')} mAh)")
    if s.get("fullCap_mAh") and s.get("designCap_mAh"):
        wear = (1.0 - s["fullCap_mAh"] / s["designCap_mAh"]) * 100
        print(f"  Wear          : {wear:.1f}% (full / design)")
    print(f"  StateOfHealth : {fmt_pct(s.get('stateOfHealth'))}")
    print(f"  Temperature   : {fmt_t(s.get('temperature_C'))}")
    print(f"  Cycles        : {s.get('cycleCount', '?')}")
    print(f"  Status reg    : {s.get('batteryStatus', '?')} -> {s.get('batteryStatusDecoded', '?')}")

    banner("CELLS")
    cells = s.get("cellVoltage_mV", [])
    cell_sync = s.get("cellVoltageSync_mV", [])
    cnt = s.get("cellCount", len([v for v in cells if v]))
    for i in range(min(cnt, 4)):
        async_v = fmt_v(cells[i]) if i < len(cells) else "-"
        sync_v  = fmt_v(cell_sync[i]) if i < len(cell_sync) else "-"
        print(f"  Cell {i+1}        : {async_v:>15s}    sync: {sync_v}")
    if cnt > 0 and cells:
        non_zero = [v for v in cells[:cnt] if v]
        if non_zero:
            spread = max(non_zero) - min(non_zero)
            tag = '(ok)' if spread < 30 else '(WORN)' if spread < 100 else '(BAD)'
            print(f"  Spread (max-min): {spread} mV  {tag}")

    banner("FAULT FLAGS")
    print(f"  PFStatus      : {s.get('pfStatus', '-')}")
    print(f"  SafetyStatus  : {s.get('safetyStatus', '-')}")
    print(f"  Has any PF    : {'YES (FAULT)' if s.get('hasPF') else 'NO (clean)'}")

    banner("CHARGING RECOMMENDATION (from BMS)")
    print(f"  Charging V    : {fmt_v(s.get('chargingVoltage_mV'))}")
    cc = s.get('chargingCurrent_mA')
    print(f"  Charging I    : {f'{cc/1000:.3f} A ({cc} mA)' if cc else '-'}")
    print(f"  Time to full  : {fmt_min(s.get('timeToFull_min'))}")
    print(f"  Run time empty: {fmt_min(s.get('runTimeToEmpty_min'))}")

    banner("ASSESSMENT")
    issues = []
    if s.get("hasPF"): issues.append("! Permanent Failure flag set")
    sf = s.get("safetyStatus", "0x0")
    if sf and sf not in ("0x0", "0xffffffff", "0xFFFFFFFF"):
        issues.append(f"! Safety trip latched: {sf}")
    if isinstance(s.get("cycleCount"), int) and s["cycleCount"] > 200:
        issues.append(f"! High cycle count ({s['cycleCount']})")
    if isinstance(s.get("stateOfHealth"), int) and 0 < s["stateOfHealth"] < 80:
        issues.append(f"! Low SoH ({s['stateOfHealth']}%)")
    if cells:
        non_zero = [v for v in cells[:cnt] if v]
        if non_zero and (max(non_zero) - min(non_zero)) > 100:
            issues.append("! Cells imbalanced > 100 mV")
    t = s.get("temperature_C")
    if t is not None:
        try:
            tf = float(t)
            if tf > 50: issues.append(f"! Hot ({tf:+.1f} C)")
            if tf < 0:  issues.append(f"! Cold ({tf:+.1f} C)")
        except Exception: pass
    if not issues:
        print("  OK All checks pass - battery looks healthy")
    else:
        for i in issues:
            print(f"  {i}")
    print()


def decode_mfr_date(d):
    """SBS Manufacture Date (reg 0x1B): 16-bit packed
       (year - 1980) << 9 | month << 5 | day"""
    if not d or d == 0xFFFF: return "-"
    try:
        d = int(d)
        year = ((d >> 9) & 0x7F) + 1980
        month = (d >> 5) & 0x0F
        day = d & 0x1F
        return f"{year:04d}-{month:02d}-{day:02d}"
    except Exception:
        return str(d)


if __name__ == "__main__":
    main()
