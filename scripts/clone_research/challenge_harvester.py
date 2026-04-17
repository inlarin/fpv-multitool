"""Collect a large number of challenge samples from the clone's active register.

After each write to the target register, reads the response block. Saves
(write_input, response_bytes) pairs to CSV. Companion to response_analyze.py.

Usage:
    python challenge_harvester.py --count 10000 --out samples.csv
    python challenge_harvester.py --reg 0xEE --write 0x0000 --count 5000 --inc
"""
import argparse
import csv
import sys
import time
from cp2112 import CP2112


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reg', type=lambda s: int(s, 0), default=0xEE)
    ap.add_argument('--write', type=lambda s: int(s, 0), default=0x0000)
    ap.add_argument('--count', type=int, default=10000)
    ap.add_argument('--readlen', type=int, default=16)
    ap.add_argument('--inc', action='store_true', help='increment write value each iteration')
    ap.add_argument('--out', default='challenge_samples.csv')
    args = ap.parse_args()

    with CP2112() as cp, open(args.out, 'w', newline='') as fp:
        print(f"Harvesting {args.count} samples from reg 0x{args.reg:02X}")
        print(f"  write pattern: {'increment from' if args.inc else 'constant'} 0x{args.write:04X}")
        print(f"  read length:   {args.readlen} bytes")
        print(f"  output:        {args.out}")

        w = csv.writer(fp)
        w.writerow(['i', 't_us', 'write', 'read_hex'])
        t0 = time.time()
        val = args.write
        for i in range(args.count):
            cp.write_word(args.reg, val)
            r = cp.read_block(args.reg, args.readlen)
            t_us = int((time.time() - t0) * 1e6)
            w.writerow([i, t_us, f'0x{val:04X}', (r or b'').hex()])
            if args.inc:
                val = (val + 1) & 0xFFFF
            if i % 1000 == 0 and i:
                elapsed = time.time() - t0
                rate = i / elapsed
                eta = (args.count - i) / rate
                print(f"  {i}/{args.count} ({100*i/args.count:.0f}%) — {rate:.0f}/s, ~{eta:.0f}s ETA")

        elapsed = time.time() - t0
        print(f"\nDone. {args.count} samples in {elapsed:.1f}s ({args.count/elapsed:.0f} tx/s)")


if __name__ == '__main__':
    sys.exit(main() or 0)
