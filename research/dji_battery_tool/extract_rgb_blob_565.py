"""Try RGB565 interpretation of the embedded blob.

RGB565 layout: 2 bytes per pixel
  byte 0 = ggg_bbbbb (LE bits)
  byte 1 = rrrrr_ggg
Or with byte-swap (depends on stack).
"""
from __future__ import annotations

from pathlib import Path
from PIL import Image
import struct

ROOT = Path(__file__).resolve().parents[2]
BLOB = Path(__file__).parent / "post_image_blob.bin"
OUT = Path(__file__).parent / "blob_imgs"
OUT.mkdir(exist_ok=True)


def rgb565_to_rgb888(b0: int, b1: int) -> tuple[int, int, int]:
    """Default LE RGB565: pixel = (b1 << 8) | b0; bits = rrrrr_gggggg_bbbbb"""
    px = (b1 << 8) | b0
    r = (px >> 11) & 0x1F
    g = (px >> 5) & 0x3F
    b = px & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def rgb565_be_to_rgb888(b0: int, b1: int) -> tuple[int, int, int]:
    """Byte-swapped variant: pixel = (b0 << 8) | b1"""
    return rgb565_to_rgb888(b1, b0)


def render_565(blob: bytes, w: int, h: int, byte_swap: bool = False, frame_idx: int = 0):
    """Render one (w x h) frame_idx-th frame from blob as RGB565."""
    bytes_per_frame = w * h * 2
    start = frame_idx * bytes_per_frame
    chunk = blob[start : start + bytes_per_frame]
    if len(chunk) < bytes_per_frame:
        return None
    img = Image.new("RGB", (w, h))
    pixels = img.load()
    cv = rgb565_be_to_rgb888 if byte_swap else rgb565_to_rgb888
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 2
            pixels[x, y] = cv(chunk[i], chunk[i+1])
    return img


def main() -> None:
    blob = BLOB.read_bytes()
    print(f"Blob: {len(blob):,} bytes")
    print(f"  / RGB565 480x320 = 307,200 B/frame -> {len(blob) // 307200} frames")
    print(f"  / RGB565 320x480 = 307,200 B/frame -> {len(blob) // 307200} frames")
    print(f"  / RGB565 480x800 = 768,000 B/frame -> {len(blob) // 768000} frames")
    print(f"  / RGB565 320x240 = 153,600 B/frame -> {len(blob) // 153600} frames")
    print(f"  / RGB565 240x320 = 153,600 B/frame -> {len(blob) // 153600} frames")
    print()

    sizes = [
        (480, 320), (320, 480), (480, 800),
        (320, 240), (240, 320), (800, 480),
    ]
    for w, h in sizes:
        for swap in (False, True):
            for idx in range(min(4, len(blob) // (w*h*2))):
                img = render_565(blob, w, h, swap, idx)
                if img is None: continue
                tag = "be" if swap else "le"
                name = f"565{tag}_{idx}_{w}x{h}.png"
                img.save(OUT / name)
                print(f"  wrote {name}")


if __name__ == "__main__":
    main()
