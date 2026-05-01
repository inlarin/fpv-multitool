"""Extract the 1.3 MB embedded RGB blob from app0+0xB3000 as PNG image(s).

The blob has high lag-3 autocorrelation (0.95 from earlier analysis) ->
RGB888 raw bitmap data. Total size 1,306,624 bytes / 3 bytes per pixel
= 435,541 pixels.

Common DJI panel sizes to try:
  480x320 = 153,600 px -> 460,800 B per image (2.84 frames in blob)
  320x480 = same
  480x272 = 130,560 px -> 391,680 B (3.34 frames)
  800x480 = 384,000 px -> 1,152,000 B (1.13 frames)
  854x480 = 409,920 px -> 1,229,760 B (1.06 frames)

Output:
  research/dji_battery_tool/blob_imgs/<idx>_<WxH>_off<HEX>.png

We try multiple sizes and write all attempts; user can pick the one
that looks right.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BLOB = Path(__file__).parent / "post_image_blob.bin"
OUT  = Path(__file__).parent / "blob_imgs"
OUT.mkdir(exist_ok=True)


def write_bmp_rgb888(path: Path, w: int, h: int, pixels: bytes, top_down: bool = True):
    """Write a 24-bit BMPv3 file. BMP scan order is bottom-up by default;
    we flip to top-down by setting negative height in DIB header."""
    row_size = ((w * 3 + 3) // 4) * 4   # padded to 4-byte alignment
    pad = row_size - w * 3
    image_size = row_size * h
    file_size = 14 + 40 + image_size

    out = bytearray()
    # BMP file header (14 bytes)
    out += b"BM"
    out += struct.pack("<I", file_size)
    out += struct.pack("<HH", 0, 0)
    out += struct.pack("<I", 14 + 40)  # pixel data offset
    # DIB header (40 bytes, BITMAPINFOHEADER)
    out += struct.pack("<I", 40)
    out += struct.pack("<i", w)
    out += struct.pack("<i", -h if top_down else h)
    out += struct.pack("<HH", 1, 24)
    out += struct.pack("<I", 0)        # BI_RGB
    out += struct.pack("<I", image_size)
    out += struct.pack("<i", 2835)
    out += struct.pack("<i", 2835)
    out += struct.pack("<I", 0)
    out += struct.pack("<I", 0)
    # Pixel rows -- BMP uses BGR byte order, not RGB
    for y in range(h):
        row_start = y * w * 3
        for x in range(w):
            r = pixels[row_start + x*3 + 0]
            g = pixels[row_start + x*3 + 1]
            b = pixels[row_start + x*3 + 2]
            out += bytes((b, g, r))
        out += b"\x00" * pad
    path.write_bytes(out)


def variance(window: bytes) -> float:
    """Sum-of-squared-deviations / N. Higher = less monotone region."""
    if not window: return 0.0
    n = len(window)
    mean = sum(window) / n
    return sum((b - mean) ** 2 for b in window) / n


def main() -> None:
    blob = BLOB.read_bytes()
    print(f"Loaded blob: {len(blob):,} bytes")

    sizes = [
        (480, 320),
        (320, 480),
        (480, 272),
        (480, 800),  # SC01 Plus / Panlee 5"
        (320, 240),
        (240, 320),
        (800, 480),
        (854, 480),
    ]

    print("\nDumping each candidate resolution as 3 sequential frames...")
    for w, h in sizes:
        frame_bytes = w * h * 3
        n_frames = len(blob) // frame_bytes
        if n_frames < 1:
            continue
        print(f"  {w}x{h}: {frame_bytes:,} B/frame, {n_frames} frame(s) fit")
        for i in range(min(n_frames, 4)):
            start = i * frame_bytes
            chunk = blob[start : start + frame_bytes]
            # Skip frames that are >85% identical bytes (usually padding)
            if len(set(chunk[::97])) < 5:
                print(f"    skip frame {i} (mostly constant)")
                continue
            name = f"{i}_{w}x{h}_off{start:06X}.bmp"
            write_bmp_rgb888(OUT / name, w, h, chunk)
            print(f"    wrote {name} ({len(chunk):,} B)")

    # Also dump windowed entropy across the blob -- helps see boundaries
    print("\nByte-variance per 16 KB block (low = monotone, high = active picture):")
    for off in range(0, len(blob), 16384):
        v = variance(blob[off:off+16384])
        bar = "#" * int(v / 1000)
        print(f"  0x{off:06X}: {v:7.0f} {bar}")


if __name__ == "__main__":
    main()
