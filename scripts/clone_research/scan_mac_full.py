"""Brute-scan the full 16-bit MAC subcommand space (0x0000-0xFFFF) looking
for undocumented vendor commands on clone batteries.

Only reports subcommands that return non-zero, non-all-FF data. Writes a
CSV log for deeper analysis. Each probe takes ~10-15ms, so the full scan
is ~15-20 minutes on CP2112 direct — much faster than the web UI.

Args:
    --from 0x0000     start subcommand (hex/dec)
    --to 0xFFFF       end subcommand (inclusive)
    --out results.csv output file
"""
import argparse
import sys
import time
import csv
from cp2112 import CP2112


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--from', dest='frm', type=lambda s: int(s, 0), default=0x0000)
    ap.add_argument('--to', dest='to', type=lambda s: int(s, 0), default=0xFFFF)
    ap.add_argument('--out', default='mac_scan.csv')
    args = ap.parse_args()

    print(f"Scanning MAC 0x{args.frm:04X} - 0x{args.to:04X}, writing to {args.out}")
    t0 = time.time()
    hits = 0
    total = args.to - args.frm + 1

    with CP2112() as cp, open(args.out, 'w', newline='') as fp:
        writer = csv.writer(fp)
        writer.writerow(['sub', 'len', 'hex', 'ascii'])
        for i, sub in enumerate(range(args.frm, args.to + 1)):
            if i % 512 == 0 and i:
                elapsed = time.time() - t0
                eta = elapsed * (total - i) / i
                print(f"  {i}/{total} ({100*i/total:.0f}%) — {hits} hits, {elapsed:.0f}s elapsed, ~{eta:.0f}s ETA")
            data = cp.mac_block_read(sub, 32)
            if not data:
                continue
            if all(b == 0 for b in data) or all(b == 0xFF for b in data):
                continue
            ascii_s = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in data)
            writer.writerow([f"0x{sub:04X}", len(data), data.hex(), ascii_s])
            print(f"  HIT 0x{sub:04X}: len={len(data)} {data.hex()[:32]} '{ascii_s[:16]}'")
            hits += 1

    print(f"\nDone in {time.time()-t0:.1f}s, {hits} hits, output: {args.out}")


if __name__ == '__main__':
    sys.exit(main() or 0)
