"""Sweep RGB888 widths from 200..1024 to find the actual image width.

Renders just the FIRST 80 KB of blob (one ~portion of a frame) at each
candidate width, then saves PNG of that strip. The width that gives a
visually-correct image is the answer.
"""
from PIL import Image
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BLOB = Path(__file__).parent / "post_image_blob.bin"
OUT  = Path(__file__).parent / "blob_imgs" / "width_sweep"
OUT.mkdir(parents=True, exist_ok=True)


def render(blob, width, height):
    """Render `width` x `height` RGB888 image from start of blob."""
    pixels_needed = width * height
    bytes_needed = pixels_needed * 3
    if bytes_needed > len(blob): return None
    img = Image.new("RGB", (width, height))
    pix = img.load()
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 3
            pix[x, y] = (blob[i], blob[i+1], blob[i+2])
    return img


def main() -> None:
    blob = BLOB.read_bytes()
    # Render strips ~80 lines tall at varying widths
    widths = [200, 240, 256, 272, 288, 320, 336, 352, 360, 384, 400, 416,
              432, 448, 464, 472, 480, 488, 496, 512, 528, 544, 560, 576,
              640, 672, 720, 768, 800, 854, 1024]
    H = 80
    for w in widths:
        img = render(blob, w, H)
        if img is None: continue
        name = f"w{w:04d}_h{H}.png"
        img.save(OUT / name)
        print(f"  wrote {name}")


if __name__ == "__main__":
    main()
