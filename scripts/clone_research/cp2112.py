"""CP2112 HID-to-SMBus Python driver — talks to the ESP32 emulator on the PC.

Uses Silicon Labs' SLABHIDtoSMBus.dll when available, otherwise pure-Python
via hidapi. Keep it minimal: all we need is Write, WriteRead, Read.

Requires ESP32 board in USB2I2C mode (sets USB descriptor to CP2112 HID).
Switch mode via web UI "USB" tab → USB2I2C → Save & Reboot, or by calling:
    curl -X POST -F mode=2 http://192.168.32.50/api/usb/mode
    curl -X POST http://192.168.32.50/api/usb/reboot
"""
import hid
import time

CP2112_VID = 0x10C4
CP2112_PID = 0xEA90


class CP2112:
    """Minimal CP2112 HID driver — sync, blocking."""
    def __init__(self, slave_addr=0x0B, rate_hz=100000):
        self.slave = slave_addr
        devices = [d for d in hid.enumerate(CP2112_VID, CP2112_PID)]
        if not devices:
            raise RuntimeError(f"No CP2112 device found (VID:{CP2112_VID:04X} PID:{CP2112_PID:04X})")
        self.dev = hid.device()
        self.dev.open_path(devices[0]['path'])
        self.dev.set_nonblocking(False)
        self._config_smbus(rate_hz)

    def close(self):
        self.dev.close()

    def _config_smbus(self, rate_hz):
        # Feature report 0x06: SMBus Configuration
        # [0x06, rate[4], addr, autoSendRead, writeTimeout[2], readTimeout[2],
        #  sclLowTimeout, retryLimit[2]]
        rate = rate_hz.to_bytes(4, 'big')
        cfg = bytes([0x06]) + rate + bytes([0x02, 0x00]) + (0).to_bytes(2,'big') + (0).to_bytes(2,'big') + bytes([0x00]) + (0).to_bytes(2,'big')
        self.dev.send_feature_report(cfg + bytes(14 - (len(cfg) - 1)))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # ===== Raw transactions =====

    def write(self, data: bytes):
        """Write N bytes to slave (SBS command + data). Returns True on ACK."""
        if len(data) > 61:
            raise ValueError("max 61 bytes per write")
        # Output 0x14 Data Write: [0x14, slave, len, data[61]]
        report = bytes([0x14, self.slave, len(data)]) + data + bytes(61 - len(data))
        self.dev.write(report)
        return self._wait_status()

    def write_read(self, target: bytes, read_len: int) -> bytes:
        """Repeated-start read: write target (SBS cmd), then read N bytes."""
        if read_len > 512 or len(target) > 16:
            raise ValueError("bad lengths")
        # Output 0x11 Write-Read: [0x11, slave, rLen[2 BE], tgtLen, tgt[16]]
        report = bytes([0x11, self.slave, (read_len >> 8) & 0xFF, read_len & 0xFF,
                        len(target)]) + target + bytes(16 - len(target))
        self.dev.write(report)
        return self._read_response(read_len)

    def read(self, length: int) -> bytes:
        """Read N bytes (no command first)."""
        report = bytes([0x10, self.slave, (length >> 8) & 0xFF, length & 0xFF])
        self.dev.write(report)
        return self._read_response(length)

    def _wait_status(self, timeout_s=0.5) -> bool:
        # Transfer Status Request (0x15) → Transfer Status (0x16)
        self.dev.write(bytes([0x15, 0x01]))
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            r = self.dev.read(8, timeout_ms=50)
            if r and r[0] == 0x16:
                return r[1] == 0x05  # 5 = completed
        return False

    def _read_response(self, expected_len: int, timeout_s=0.3) -> bytes:
        data = bytearray()
        deadline = time.time() + timeout_s
        while len(data) < expected_len and time.time() < deadline:
            r = self.dev.read(64, timeout_ms=50)
            if not r or r[0] != 0x13:
                continue
            status, cnt = r[1], r[2]
            if status != 0x01:  # 1 = idle/ok
                break
            data.extend(r[3:3+cnt])
        return bytes(data)

    # ===== SMBus/SBS convenience =====

    def read_word(self, reg: int) -> int | None:
        r = self.write_read(bytes([reg]), 2)
        if len(r) != 2:
            return None
        return r[0] | (r[1] << 8)

    def read_block(self, reg: int, max_len: int = 32) -> bytes | None:
        # SMBus block read: first byte is length
        r = self.write_read(bytes([reg]), max_len + 1)
        if not r:
            return None
        n = r[0]
        if n > max_len:
            n = max_len
        return r[1:1+n]

    def write_word(self, reg: int, val: int) -> bool:
        return self.write(bytes([reg, val & 0xFF, (val >> 8) & 0xFF]))

    def write_block(self, reg: int, data: bytes) -> bool:
        if len(data) > 32:
            raise ValueError("block > 32")
        return self.write(bytes([reg, len(data)]) + data)

    def mac_block_read(self, subcommand: int, max_len: int = 32) -> bytes | None:
        """ManufacturerBlockAccess: write 2B subcommand to 0x44, then block-read 0x44."""
        cmd = bytes([subcommand & 0xFF, (subcommand >> 8) & 0xFF])
        if not self.write_block(0x44, cmd):
            return None
        time.sleep(0.01)
        r = self.read_block(0x44, max_len)
        # Strip 2-byte subcommand echo
        if r and len(r) >= 2:
            return bytes(r[2:])
        return r

    def mac_command(self, subcommand: int) -> bool:
        return self.write_word(0x00, subcommand)


if __name__ == '__main__':
    # Self-test: read battery basics
    with CP2112() as cp:
        v = cp.read_word(0x09)
        print(f'Voltage: {v} mV' if v else 'no response')
        soc = cp.read_word(0x0D)
        print(f'SOC: {soc}%' if soc else 'no response')
        name = cp.read_block(0x21, 20)
        print(f'DeviceName: {name}' if name else 'no response')
