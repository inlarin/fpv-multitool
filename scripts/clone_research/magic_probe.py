"""Probe known backdoor/factory-test magic sequences on vendor SBS registers.

The PTL BA01WM260 clone exposes suspicious paired magic bytes at:
    0xD6 = 0xAA55  (classic backdoor marker)
    0xD9 ends in 0x55AA (reversed pair)

This script tries writing well-known magic sequences to every vendor register
in the 0xD0-0xFF range, then reads back the FULL vendor map to detect any
change that would indicate an activated factory/backdoor command.

Known magic patterns (factory test, bootloader triggers, auth challenges):
    0xAA55, 0x55AA, 0x5AA5, 0xA55A, 0xDEAD, 0xBEEF, 0xFEED, 0xFACE,
    0xCAFE, 0xBABE, 0x1234, 0x4321, 0x5432, 0xC001, 0xC0DE, 0xDJI
"""
import sys
import time
from cp2112 import CP2112

MAGICS = [
    0xAA55, 0x55AA, 0x5AA5, 0xA55A,
    0xDEAD, 0xBEEF, 0xFEED, 0xFACE,
    0xCAFE, 0xBABE, 0x1234, 0x4321,
    0x5432, 0xC001, 0xC0DE, 0x0DEB,
    0x000F, 0x00FF, 0xFF00, 0xF00F,
    0x0F0F, 0xF0F0, 0x0FF0, 0xEC05,  # 0xEC05 = DJI internal command prefix seen in BK logs
]

TARGET_REGS = list(range(0xD0, 0x100))  # 48 vendor regs


def read_map(cp):
    """Read word+block for every vendor reg, return snapshot dict."""
    snap = {}
    for r in TARGET_REGS:
        w = cp.read_word(r)
        b = cp.read_block(r, 16)
        snap[r] = (w if w != 0xFFFF else None, bytes(b) if b else b'')
    return snap


def diff_maps(before, after):
    diffs = []
    for r in TARGET_REGS:
        if before.get(r) != after.get(r):
            diffs.append((r, before.get(r), after.get(r)))
    return diffs


def main():
    with CP2112() as cp:
        print("=== Magic Sequence Probe ===")
        print(f"Targets: 0x{TARGET_REGS[0]:02X}-0x{TARGET_REGS[-1]:02X}")
        print(f"Magics:  {len(MAGICS)} patterns")
        print(f"Total probes: {len(TARGET_REGS)*len(MAGICS)} = {len(TARGET_REGS)*len(MAGICS)} writes + {len(TARGET_REGS)*2*len(MAGICS)} reads")
        print()

        baseline = read_map(cp)
        print(f"Baseline captured: {sum(1 for v in baseline.values() if v[0] is not None or v[1])} responsive regs\n")

        changes = []

        for magic in MAGICS:
            print(f"[Magic 0x{magic:04X}]", end=' ', flush=True)
            for target in TARGET_REGS:
                # Write magic as word to target
                cp.write_word(target, magic)
                time.sleep(0.005)
            # Sample full map after writing all targets
            after = read_map(cp)
            diff = diff_maps(baseline, after)
            if diff:
                print(f"{len(diff)} changes!")
                for reg, b, a in diff:
                    print(f"    0x{reg:02X}: word {b[0]}->{a[0]}  block {(b[1] or b'').hex()} -> {(a[1] or b'').hex()}")
                changes.append((magic, diff))
                # Reset baseline so next magic sees what we just found
                baseline = after
            else:
                print("no change")

        print()
        if changes:
            print(f"=== {len(changes)} magic patterns produced changes ===")
        else:
            print("=== No magic pattern produced any change. Clone is fully sealed for writes. ===")
            print("Next angles to try:")
            print("  * Multi-byte / block writes instead of word writes")
            print("  * Combinatorial sequences (write reg1 then reg2)")
            print("  * DJI internal commands (0xEC05 prefix seen in BK logs)")
            print("  * Observe legitimate DJI tool traffic via CP2112 log while it runs")


if __name__ == '__main__':
    sys.exit(main() or 0)
