"""Statistical analysis of harvested challenge samples.

Looks for:
  - Byte-level entropy (fixed bytes vs. varying)
  - Periodicity (is it a counter? LFSR?)
  - Autocorrelation (does byte depend on index?)
  - Known LFSR polynomial test (CRC-16 variants)
  - Input-output correlation (does write value influence response?)

Usage:
    python response_analyze.py samples.csv
"""
import argparse
import csv
import sys
from collections import Counter


def load_samples(path):
    rows = []
    with open(path) as fp:
        r = csv.DictReader(fp)
        for line in r:
            rows.append({
                'i': int(line['i']),
                'write': int(line['write'], 16),
                'read': bytes.fromhex(line['read_hex']) if line['read_hex'] else b'',
            })
    return rows


def byte_entropy(samples, byte_idx):
    """Shannon entropy of a byte across samples."""
    import math
    cnt = Counter(s['read'][byte_idx] for s in samples if len(s['read']) > byte_idx)
    n = sum(cnt.values())
    if n < 2:
        return 0
    ent = 0
    for v in cnt.values():
        p = v / n
        ent -= p * math.log2(p)
    return ent


def find_period(seq, max_period=4096):
    """Return smallest period p such that seq[i] == seq[i+p] for all i."""
    if len(seq) < 4:
        return None
    for p in range(1, min(max_period, len(seq) // 2)):
        ok = True
        for i in range(len(seq) - p):
            if seq[i] != seq[i+p]:
                ok = False
                break
        if ok:
            return p
    return None


def test_lfsr_crc16(seq):
    """Simulate a CRC-16 (poly 0x1021 CCITT and 0x8005 IBM) and see if
    response matches. If the counter increments but response follows CRC of
    index or write value, that's a smoking gun."""
    polys = [(0x1021, 'CCITT'), (0x8005, 'IBM/ANSI')]
    def crc16(data, poly, init=0xFFFF):
        crc = init
        for b in data:
            crc ^= b << 8
            for _ in range(8):
                if crc & 0x8000:
                    crc = (crc << 1) ^ poly
                else:
                    crc <<= 1
                crc &= 0xFFFF
        return crc
    return polys, crc16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv_path')
    ap.add_argument('--byte', type=int, default=None, help='analyze specific byte index only')
    args = ap.parse_args()

    samples = load_samples(args.csv_path)
    print(f"Loaded {len(samples)} samples")
    if not samples:
        return 1

    max_len = max(len(s['read']) for s in samples)
    print(f"Max read length: {max_len} bytes\n")

    # Per-byte entropy
    print("=== Byte-level entropy (bits, max=8 for uniform random) ===")
    varying_bytes = []
    for b in range(max_len):
        e = byte_entropy(samples, b)
        distinct = len({s['read'][b] for s in samples if len(s['read']) > b})
        mark = '  CONST' if distinct == 1 else ('  high-entropy' if e > 6 else '')
        print(f"  byte[{b:2d}]: entropy={e:.3f}  distinct={distinct:4d}{mark}")
        if distinct > 1:
            varying_bytes.append(b)

    # Periodicity per varying byte
    print(f"\n=== Periodicity (among {len(varying_bytes)} varying bytes) ===")
    for b in varying_bytes[:5]:  # only first 5 to keep output short
        seq = [s['read'][b] for s in samples if len(s['read']) > b]
        p = find_period(seq)
        print(f"  byte[{b}]: period = {p if p else 'not found (aperiodic or > ' + str(len(seq)//2) + ')'}")

    # Input-output correlation for byte pair 5-6 (our PTL clone case)
    if max_len >= 7:
        print("\n=== Input -> Output correlation (bytes 5-6 as uint16 LE) ===")
        pairs = [(s['write'], s['read'][5] | (s['read'][6] << 8)) for s in samples if len(s['read']) >= 7]
        # Look for simple linear relations
        diffs = [b - a for a, b in zip(pairs, pairs[1:])]
        in_diffs = set(d[0] for d in diffs)
        out_diffs = set(d[1] for d in diffs)
        print(f"  Distinct input steps:  {len(in_diffs)} (write pattern)")
        print(f"  Distinct output steps: {len(out_diffs)}")
        # Autocorrelation: if response[i] depends on previous response, show coverage
        if len(pairs) > 10:
            r_vals = [p[1] for p in pairs]
            uniq = len(set(r_vals))
            print(f"  Response range: {len(r_vals)} samples, {uniq} unique values ({100*uniq/len(r_vals):.1f}% uniqueness)")

    # Test CRC hypotheses
    print("\n=== CRC-16 hypothesis test (input write -> response byte 5-6) ===")
    polys, crc16 = test_lfsr_crc16(None)
    if max_len >= 7:
        hits = {}
        for p, name in polys:
            matches = 0
            for s in samples[:200]:  # sample first 200
                if len(s['read']) < 7:
                    continue
                target = s['read'][5] | (s['read'][6] << 8)
                write_bytes = bytes([s['write'] & 0xFF, (s['write'] >> 8) & 0xFF])
                for init in (0x0000, 0xFFFF, 0x1D0F):
                    if crc16(write_bytes, p, init) == target:
                        matches += 1
                        break
            hits[name] = matches
            print(f"  {name} (poly=0x{p:04X}): {matches}/200 matches")
        if max(hits.values(), default=0) > 30:
            print("  *** Likely CRC-based challenge — try matching polys on full sample ***")
        else:
            print("  No simple CRC hit. May be HMAC/AES/custom.")

    # Summary
    print("\n=== Summary ===")
    high_ent = [b for b in varying_bytes if byte_entropy(samples, b) > 6]
    print(f"  Varying bytes: {varying_bytes}")
    print(f"  High-entropy bytes (>6 bits): {high_ent}")
    if high_ent:
        print(f"  -> These bytes contain the challenge/nonce. Focus reverse-engineering here.")


if __name__ == '__main__':
    sys.exit(main() or 0)
