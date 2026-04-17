#!/usr/bin/env python3
"""Read Manufacturer Data (SBS 0x23) from battery via our CP2112 emulator.
Compares raw Write-Read result vs what BK reported."""
import hid, time

VID, PID = 0x10C4, 0xEA90
SLAVE = 0x0B


def open_dev():
    path = list(hid.enumerate(VID, PID))[0]['path']
    dev = hid.device()
    dev.open_path(path)
    return dev


def set_smbus_config(dev):
    payload = bytes([
        0x00, 0x01, 0x86, 0xA0,  # bitRate 100kHz
        0x02,                     # addr
        0x00,                     # autoRead
        0x00, 0xC8, 0x00, 0xC8,   # wto=200, rto=200
        0x00,                     # sclLow
        0x00, 0x00,               # retries
    ])
    dev.send_feature_report(bytes([0x06]) + payload)


def read_block(dev, reg, nbytes=64):
    """Write-Read via Output 0x11, poll status via 0x15, force read via 0x12."""
    slave8 = (SLAVE << 1) & 0xFF
    pkt = bytes([0x11, slave8,
                 (nbytes >> 8) & 0xFF, nbytes & 0xFF,
                 1, reg]) + b'\x00' * (64 - 6)
    dev.write(pkt)
    time.sleep(0.05)
    # force read response
    dev.write(bytes([0x12, 0x00, nbytes & 0xFF]) + b'\x00' * 61)
    r = dev.read(64, timeout_ms=300)
    return bytes(r) if r else b''


def main():
    dev = open_dev()
    set_smbus_config(dev)

    print("=== SBS 0x23 Manufacturer Data (32 byte SMBus block) ===")
    r = read_block(dev, 0x23, 34)
    print(f"raw ({len(r)}B): {r.hex(' ')}")
    if len(r) > 2:
        # format: [13 report-id, status, length, data...]
        print(f"  status={r[1]:#04x}  len={r[2]}  data={r[3:3 + r[2]].hex(' ')}")

    print("\n=== SBS 0x20 Manufacturer Name ===")
    r = read_block(dev, 0x20, 22)
    print(f"raw: {r.hex(' ')}")
    if len(r) > 2:
        print(f"  status={r[1]:#04x}  len={r[2]}  str={r[3:3 + r[2]]!r}")

    print("\n=== Full 64-byte read from 0x23 (like BK does) ===")
    r = read_block(dev, 0x23, 62)
    print(f"raw ({len(r)}B): {r.hex(' ')}")

    dev.close()


if __name__ == "__main__":
    main()
