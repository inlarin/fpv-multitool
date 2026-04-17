#!/usr/bin/env python3
"""Read a large region from battery register 0x23 to compare with BK output."""
import hid, time

VID, PID = 0x10C4, 0xEA90
SLAVE = 0x0B


def open_dev():
    path = list(hid.enumerate(VID, PID))[0]['path']
    d = hid.device()
    d.open_path(path)
    return d


def cfg(d):
    p = bytes([0x00, 0x01, 0x86, 0xA0, 0x02, 0x00,
               0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00])
    d.send_feature_report(bytes([0x06]) + p)


def read_chunk(d, reg, nbytes):
    """Single Write-Read with force-send."""
    slave8 = (SLAVE << 1) & 0xFF
    pkt = bytes([0x11, slave8,
                 (nbytes >> 8) & 0xFF, nbytes & 0xFF,
                 1, reg]) + b'\x00' * (64 - 6)
    d.write(pkt)
    time.sleep(0.05)
    d.write(bytes([0x12, (nbytes >> 8) & 0xFF, nbytes & 0xFF]) + b'\x00' * 61)
    r = d.read(64, timeout_ms=300)
    if not r or r[0] != 0x13:
        return b''
    got = r[2]
    return bytes(r[3:3 + got])


def raw_read(d, nbytes):
    """Output 0x10: pure I2C read with no register write."""
    slave8 = (SLAVE << 1) & 0xFF
    pkt = bytes([0x10, slave8, (nbytes >> 8) & 0xFF, nbytes & 0xFF]) + b'\x00' * (64 - 4)
    d.write(pkt)
    time.sleep(0.05)
    d.write(bytes([0x12, (nbytes >> 8) & 0xFF, nbytes & 0xFF]) + b'\x00' * 61)
    r = d.read(64, timeout_ms=300)
    if not r or r[0] != 0x13:
        return b''
    got = r[2]
    return bytes(r[3:3 + got])


def main():
    d = open_dev()
    cfg(d)

    print("=== Pattern 1: write 0x23, then chain raw reads (no re-write) ===")
    # Trigger register access once, then stream
    first = read_chunk(d, 0x23, 61)
    print(f"first chunk ({len(first)}B): {first[:64].hex()}")
    all_bytes = bytearray(first)
    for i in range(3):
        chunk = raw_read(d, 61)
        print(f"raw_read#{i}: {chunk[:32].hex()}")
        all_bytes.extend(chunk)
    print(f"\ntotal ({len(all_bytes)}B):")
    print(all_bytes.hex())
    d.close()


if __name__ == "__main__":
    main()
