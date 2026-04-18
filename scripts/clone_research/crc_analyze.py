"""Find which checksum / CRC the clone uses on publish packets.

Captured packets (81 F0 + payload + 4-byte tail):
  BE 01 00 00 4E0F 330F 470F 4B0F | A8 3B 8B 3D
  BF 01 02 00 4E0F 330F 470F 4B0F | A4 3B 8E 3D  (flag differs)
  C1 01 00 00 4E0F 330F 470F 4B0F | A6 3B 8F 3D
  C2 01 00 00 4E0F 330F 470F 4B0F | A8 3B 8B 3D
  C3 01 00 00 4E0F 330F 470F 4B0F | A6 3B 8A 3D

Try every 16-bit CRC polynomial against various payload slices.
"""

# Packets: (header_14_bytes, tail_4_bytes)
PACKETS = [
    (bytes.fromhex('81F0BE010000 4E0F330F470F4B0F'.replace(' ','')), bytes.fromhex('A83B8B3D')),
    (bytes.fromhex('81F0BF010200 4E0F330F470F4B0F'.replace(' ','')), bytes.fromhex('A43B8E3D')),
    (bytes.fromhex('81F0C1010000 4E0F330F470F4B0F'.replace(' ','')), bytes.fromhex('A63B8F3D')),
    (bytes.fromhex('81F0C2010000 4E0F330F470F4B0F'.replace(' ','')), bytes.fromhex('A83B8B3D')),
    (bytes.fromhex('81F0C3010000 4E0F330F470F4B0F'.replace(' ','')), bytes.fromhex('A63B8A3D')),
]

COMMON_POLYS = [
    (0x1021, 'CCITT/XMODEM'),
    (0x8005, 'IBM/ANSI'),
    (0x8408, 'CCITT-reflected'),
    (0xA001, 'MODBUS'),
    (0x3D65, 'DNP'),
    (0x0589, 'DECT'),
    (0x8BB7, 'CRC-16/T10-DIF'),
    (0xC867, 'CDMA2000'),
]


def crc16(data, poly, init=0x0000, reflected=False, xorout=0):
    crc = init
    for b in data:
        if reflected:
            crc ^= b
            for _ in range(8):
                crc = (crc >> 1) ^ poly if (crc & 1) else (crc >> 1)
        else:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ poly) if (crc & 0x8000) else (crc << 1)
                crc &= 0xFFFF
    return crc ^ xorout


def crc8(data, poly=0x07, init=0):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def test_all_polys_against(payload_slice, expected_value_bytes, expected_name):
    """Try all poly/init/reflect/xorout combos, see which produces expected."""
    expected_le = int.from_bytes(expected_value_bytes, 'little')
    expected_be = int.from_bytes(expected_value_bytes, 'big')
    hits = []
    for poly, name in COMMON_POLYS:
        for init in (0x0000, 0xFFFF, 0x1D0F, 0x89EC):
            for refl in (False, True):
                for xorout in (0x0000, 0xFFFF):
                    c = crc16(payload_slice, poly, init, refl, xorout)
                    if c == expected_le:
                        hits.append((poly, name, init, refl, xorout, 'LE'))
                    if c == expected_be:
                        hits.append((poly, name, init, refl, xorout, 'BE'))
    return hits


def main():
    print("Trying CRC-16 polynomials against each packet's 4-byte tail...\n")

    # Two hypotheses:
    # (a) tail[0:2] = CRC of header, tail[2:4] = timestamp/temp/etc.
    # (b) tail[0:2] + tail[2:4] are two separate CRC-16s of different slices
    # (c) tail[0,2] = two 1-byte checksums, tail[1,3] are fixed markers

    # Test: is (pos 14, 15) = CRC-16 of header[0:14]?
    for i, (hdr, tail) in enumerate(PACKETS):
        print(f"Pkt {i}: header={hdr.hex()}  tail={tail.hex()}")

    print("\n=== Hypothesis A: tail[0:2] = CRC-16 of header ===")
    tries = {}
    for i, (hdr, tail) in enumerate(PACKETS):
        h = test_all_polys_against(hdr, tail[0:2], f"Pkt{i}")
        for hit in h:
            key = hit[:5]  # (poly, name, init, refl, xorout)
            tries.setdefault(key, []).append(i)

    # Report polys that match ALL packets
    for key, matched in tries.items():
        if len(matched) == len(PACKETS):
            print(f"  *** MATCH all: poly=0x{key[0]:04X} ({key[1]}) init=0x{key[2]:04X} reflected={key[3]} xorout=0x{key[4]:04X}")
    if not any(len(m) == len(PACKETS) for m in tries.values()):
        print("  (no single config matches all packets for tail[0:2])")

    print("\n=== Hypothesis B: tail[2:4] = CRC-16 of something ===")
    tries2 = {}
    for i, (hdr, tail) in enumerate(PACKETS):
        h = test_all_polys_against(hdr, tail[2:4], f"Pkt{i}")
        for hit in h:
            key = hit[:5]
            tries2.setdefault(key, []).append(i)
    for key, matched in tries2.items():
        if len(matched) == len(PACKETS):
            print(f"  *** MATCH all: poly=0x{key[0]:04X} ({key[1]}) init=0x{key[2]:04X} reflected={key[3]} xorout=0x{key[4]:04X}")
    if not any(len(m) == len(PACKETS) for m in tries2.values()):
        print("  (no single config matches all packets for tail[2:4])")

    print("\n=== Hypothesis C: tail bytes are pairs — e.g., tail[0]=CRC8(header) ===")
    tries3 = {}
    for i, (hdr, tail) in enumerate(PACKETS):
        for init in range(0, 256):
            for poly in (0x07, 0x1D, 0x31, 0x9B, 0xA7, 0x07):
                c = crc8(hdr, poly, init)
                if c == tail[0]:
                    k = (poly, init, 'tail[0]')
                    tries3.setdefault(k, []).append(i)
                if c == tail[2]:
                    k = (poly, init, 'tail[2]')
                    tries3.setdefault(k, []).append(i)
    for k, matched in tries3.items():
        if len(matched) == len(PACKETS):
            print(f"  *** MATCH all: CRC-8 poly=0x{k[0]:02X} init=0x{k[1]:02X} -> {k[2]}")
    if not any(len(m) == len(PACKETS) for m in tries3.values()):
        print("  (no simple CRC-8 matches all)")

    print("\n=== Raw statistics on tail bytes ===")
    tails = [p[1] for p in PACKETS]
    for b in range(4):
        vals = [t[b] for t in tails]
        print(f"  tail[{b}]: values={[f'{v:02X}' for v in vals]}  distinct={len(set(vals))}  range={min(vals):02X}-{max(vals):02X}")


if __name__ == '__main__':
    main()
