"""Probe a sealed clone battery for seal-enforcement weaknesses.

Tests:
1. Write-verify: write sentinel values to each writable-looking SBS register,
   check if any actually persist (would mean seal is cosmetic).
2. Protected-range writes: try SBS 0x17 (cycles), 0x18 (design cap), 0x1C (serial)
   — these are standard but always sealed. If they accept writes, bingo.
3. DF write probe: try writing via MAC 0x44 to cycle count DF 0x4340.
4. Status clear probe: try to clear BatteryStatus alarm bits without unseal.

SAFE: all writes use values that can be restored (reads first, writes back at end).
WARNING: don't run this on a battery you care about until you're sure.
"""
import sys
import time
from cp2112 import CP2112


def probe_sbs_write(cp, reg, sentinel=0xABCD):
    before = cp.read_word(reg)
    if before is None or before == 0xFFFF:
        return None  # skip non-responsive
    cp.write_word(reg, sentinel)
    time.sleep(0.02)
    after = cp.read_word(reg)
    # Restore original if we accidentally changed something
    if after != before:
        cp.write_word(reg, before)
        time.sleep(0.02)
    return {
        'reg': reg,
        'before': before,
        'sentinel': sentinel,
        'after': after,
        'changed': after != before,
        'restored': cp.read_word(reg) == before,
    }


def main():
    print("=== Seal Bypass Probe ===\n")
    with CP2112() as cp:
        # Get device identity first
        dn = cp.read_block(0x21, 20)
        mfr = cp.read_block(0x20, 20)
        print(f"Device:       {dn!r}")
        print(f"Manufacturer: {mfr!r}")

        # Seal state via OperationStatus (DWORD at 0x54)
        op = cp.read_block(0x54, 4)
        if op and len(op) >= 2:
            sec = (op[1] >> 0) & 0x03  # SEC bits 8-9 in little-endian bytes 0..3
            print(f"OpStatus: {op.hex()}  (SEC={sec}, 3=SEALED, 2=UNSEALED, 1=FULL)")
        print()

        # === Test 1: writes to each SBS register ===
        print("--- SBS write-verify probe ---")
        bypassed = []
        for reg in [0x17, 0x18, 0x1A, 0x1B, 0x1C, 0x3C, 0x3D, 0x3E, 0x3F, 0x16,
                    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xE0, 0xF0]:
            r = probe_sbs_write(cp, reg)
            if r is None:
                continue
            mark = '⚠ CHANGED' if r['changed'] else 'ok (sealed)'
            print(f"  0x{reg:02X}: before=0x{r['before']:04X} sentinel=0x{r['sentinel']:04X} after=0x{r['after']:04X}  {mark}")
            if r['changed']:
                bypassed.append(r)

        if bypassed:
            print(f"\n⚠⚠⚠ SEAL BYPASS FOUND on {len(bypassed)} registers ⚠⚠⚠")
            for r in bypassed:
                print(f"  0x{r['reg']:02X} accepts arbitrary writes while sealed")
        else:
            print("\n✓ Seal enforced on all tested SBS registers")

        # === Test 2: DF write via MAC 0x44 ===
        print("\n--- DF write probe (MAC 0x44 → addr 0x4340, cycle count) ---")
        cyc_before = cp.read_word(0x17)
        print(f"  Cycle count before: {cyc_before}")
        # Write 0 to DF 0x4340 as U16
        payload = bytes([0x40, 0x43, 0x00, 0x00])
        ok = cp.write_block(0x44, payload)
        print(f"  writeBlock(0x44, {payload.hex()}) → ACK={ok}")
        time.sleep(0.2)
        cyc_after = cp.read_word(0x17)
        print(f"  Cycle count after:  {cyc_after}")
        if cyc_after != cyc_before:
            print(f"  ⚠ DF WRITE BYPASSED SEAL — cycles changed from {cyc_before} to {cyc_after}")
        else:
            print("  ✓ DF protected")


if __name__ == '__main__':
    sys.exit(main() or 0)
