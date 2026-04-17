#!/usr/bin/env python3
"""Direct HID test for ESP32 CP2112 emulator — bypasses SLABHIDtoSMBus.dll / BK.

Requires: pip install hidapi
Run:      python scripts\\cp2112_test.py
"""
import hid, time, sys

VID, PID = 0x10C4, 0xEA90


def open_dev():
    devs = list(hid.enumerate(VID, PID))
    if not devs:
        raise RuntimeError(f"No HID device with {VID:04X}:{PID:04X}")
    for d in devs:
        print(f"  path={d['path']!r}")
    dev = hid.device()
    # Open by path — works on Windows even when VID/PID matches multiple paths
    dev.open_path(devs[0]['path'])
    print(f"Opened: mfr={dev.get_manufacturer_string()}  prod={dev.get_product_string()}")
    return dev


def set_smbus_config(dev, bitrate=100_000, addr=0x02, auto_read=0,
                     write_timeout=200, read_timeout=200, scl_low=0, retries=0):
    """Feature Report 0x06 — 13 bytes payload."""
    payload = bytes([
        (bitrate >> 24) & 0xFF, (bitrate >> 16) & 0xFF,
        (bitrate >> 8) & 0xFF, bitrate & 0xFF,
        addr, auto_read,
        (write_timeout >> 8) & 0xFF, write_timeout & 0xFF,
        (read_timeout >> 8) & 0xFF, read_timeout & 0xFF,
        scl_low,
        (retries >> 8) & 0xFF, retries & 0xFF,
    ])
    n = dev.send_feature_report(bytes([0x06]) + payload)
    print(f"[SetFeature 0x06 SMBus Config] => {n} bytes")


def get_smbus_config(dev):
    d = dev.get_feature_report(0x06, 14)
    print(f"[GetFeature 0x06] <= {bytes(d).hex(' ')}")


def get_version(dev):
    d = dev.get_feature_report(0x05, 3)
    print(f"[GetFeature 0x05 Version] <= {bytes(d).hex(' ')}")


def data_write(dev, slave7, data):
    """Output Report 0x14: slave8 + len + data[]  (canonical AN495)"""
    slave8 = (slave7 << 1) & 0xFF
    pkt = bytes([0x14, slave8, len(data)]) + bytes(data)
    pkt = pkt + b'\x00' * (64 - len(pkt))
    n = dev.write(pkt)
    print(f"[Output 0x14 Write] slave={slave7:#04x} data={bytes(data).hex(' ')} => {n} bytes")


def read_request(dev, slave7, length):
    """Output Report 0x10: slave8 + len_hi + len_lo"""
    slave8 = (slave7 << 1) & 0xFF
    pkt = bytes([0x10, slave8, (length >> 8) & 0xFF, length & 0xFF])
    pkt = pkt + b'\x00' * (63 - len(pkt))
    n = dev.write(pkt)
    print(f"[Output 0x10 Read] slave={slave7:#04x} len={length} => {n} bytes")


def write_read_request(dev, slave7, target, read_len):
    """Output Report 0x11: slave8 + rlen_hi + rlen_lo + tgt_len + tgt[16]"""
    slave8 = (slave7 << 1) & 0xFF
    tgt = bytes(target)
    pkt = bytes([0x11, slave8, (read_len >> 8) & 0xFF, read_len & 0xFF, len(tgt)]) + tgt
    pkt = pkt + b'\x00' * (63 - len(pkt))
    n = dev.write(pkt)
    print(f"[Output 0x11 Write-Read] slave={slave7:#04x} tgt={tgt.hex(' ')} rlen={read_len} => {n} bytes")


def transfer_status(dev):
    """Output 0x15 => expect Input 0x16 (canonical AN495)"""
    dev.write(bytes([0x15, 0x01]) + b'\x00' * 62)
    r = dev.read(64, timeout_ms=200)
    print(f"[0x15 => Input 0x16] <= {bytes(r).hex(' ') if r else '(timeout)'}")
    return r


def force_read_response(dev):
    """Output 0x12 => expect Input 0x13 (canonical AN495)"""
    dev.write(bytes([0x12, 0x00, 0x02]) + b'\x00' * 61)
    r = dev.read(64, timeout_ms=200)
    print(f"[0x12 => Input 0x13] <= {bytes(r).hex(' ') if r else '(timeout)'}")
    return r


def main():
    dev = open_dev()
    print("\n=== Configure ===")
    get_version(dev)
    set_smbus_config(dev)
    get_smbus_config(dev)

    print("\n=== Output 0x17 — simple Write Block to battery 0x0B ===")
    # Write 0x20 (MfrName register) — no data, just the reg
    data_write(dev, 0x0B, [0x20])
    time.sleep(0.1)
    transfer_status(dev)

    print("\n=== Output 0x11 — Write-Read (MfrName from 0x20) ===")
    write_read_request(dev, 0x0B, [0x20], 32)
    time.sleep(0.1)
    transfer_status(dev)
    force_read_response(dev)

    print("\n=== Output 0x10 — raw Read 2 bytes from 0x0B ===")
    read_request(dev, 0x0B, 2)
    time.sleep(0.1)
    transfer_status(dev)
    force_read_response(dev)

    dev.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"ERR: {e}")
        sys.exit(1)
