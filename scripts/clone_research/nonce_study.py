"""Deep-study the 'active' vendor register 0xEE on clone batteries.

Findings so far: bytes 6-7 change on each read even without any writes.
This could be:
  a) Monotonic counter -> predict next value
  b) LFSR pseudo-random -> reverse-engineer polynomial
  c) True random (dead end)
  d) Timestamp or checksum over internal state

This script captures N consecutive reads + N controlled writes to detect the
pattern. Save raw samples to CSV for offline analysis (matplotlib, etc).
"""
import argparse
import csv
import sys
import time
from cp2112 import CP2112


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reg', type=lambda s: int(s, 0), default=0xEE)
    ap.add_argument('--samples', type=int, default=50)
    ap.add_argument('--delay', type=float, default=0.1, help='seconds between reads')
    ap.add_argument('--out', default='nonce_samples.csv')
    ap.add_argument('--write', type=lambda s: int(s, 0), default=None,
                    help='write this word before each read (to test write-then-read)')
    args = ap.parse_args()

    with CP2112() as cp:
        print(f"Studying reg 0x{args.reg:02X} — {args.samples} samples, {args.delay}s delay")
        if args.write is not None:
            print(f"  Writing 0x{args.write:04X} before each read")

        with open(args.out, 'w', newline='') as fp:
            w = csv.writer(fp)
            w.writerow(['sample', 't_ms', 'word_hex', 'block_hex'])
            t0 = time.time()
            prev = None
            for i in range(args.samples):
                if args.write is not None:
                    cp.write_word(args.reg, args.write)
                    time.sleep(0.01)
                word = cp.read_word(args.reg)
                block = cp.read_block(args.reg, 16) or b''
                t_ms = int((time.time() - t0) * 1000)
                word_hex = f'0x{word:04X}' if word is not None else 'None'
                block_hex = block.hex()
                w.writerow([i, t_ms, word_hex, block_hex])
                mark = ''
                if prev and prev != block_hex:
                    mark = '  <-- changed'
                print(f"  [{i:3d}] @{t_ms:5d}ms  w={word_hex}  b={block_hex}{mark}")
                prev = block_hex
                time.sleep(args.delay)

        print(f"\nSaved to {args.out}")


if __name__ == '__main__':
    sys.exit(main() or 0)
