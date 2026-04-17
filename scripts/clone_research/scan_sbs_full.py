"""Scan full SBS register space (0x00-0xFF) on a battery via CP2112.

Reads each register as both word (2B) and block (up to 32B), prints a
table of responsive registers. Useful for clone batteries that expose
vendor-specific registers beyond the TI BQ standard.
"""
import sys
from cp2112 import CP2112


def ascii_of(data):
    return ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in data)


def main():
    results = []
    with CP2112() as cp:
        for reg in range(0x00, 0x100):
            w = cp.read_word(reg)
            b = cp.read_block(reg, 32)
            has_w = w is not None and w != 0xFFFF
            has_b = b is not None and len(b) > 0
            if not (has_w or has_b):
                continue
            results.append({
                'reg': reg,
                'word': w if has_w else None,
                'block': b if has_b else b'',
            })
            # Print live so long scans are visible
            if has_b:
                hx = b.hex()[:32]
                print(f"0x{reg:02X} word={w:>5} (0x{w:04X}) len={len(b):2} hex={hx:<32} '{ascii_of(b[:16])}'"
                      if has_w else
                      f"0x{reg:02X}                 len={len(b):2} hex={hx:<32} '{ascii_of(b[:16])}'")
            else:
                print(f"0x{reg:02X} word={w:>5} (0x{w:04X})")

    print(f"\n=== Summary: {len(results)}/256 registers responded ===")
    # Highlight non-standard ranges
    hi_range = [r for r in results if r['reg'] >= 0xD0]
    if hi_range:
        print(f"Vendor range (0xD0-0xFF): {len(hi_range)} entries — likely clone-specific:")
        for r in hi_range:
            print(f"  0x{r['reg']:02X}: {r['block'].hex() if r['block'] else ''}")


if __name__ == '__main__':
    sys.exit(main() or 0)
