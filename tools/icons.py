#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow"]
# ///
"""Build the program's icons from the artwork, assets/icon.png.

  uv run tools/icons.py [--art assets/icon.png] [--out assets]

The artwork is a square RGBA image, transparent around the subject. It
was rendered with Qwen-Image 2.1 on a flat white background, keyed to
transparency, and padded to a square. From it:

- prospero.ico: the Windows icon (src/gui_win32.cpp), 32-bit BMPs from
  16 to 64 pixels, then a 256-pixel PNG, in that order, as the .incbin
  slices there expect (rerun with --offsets to print them);
- prospero.png: the window icon on X11 and Wayland (src/gui_sdl.cpp),
  256 pixels;
- prospero.icns: Prospero.app's icon, the artwork on a rounded navy
  tile on Apple's grid (an 824-pixel tile in 1024), since macOS puts a
  shape of its own behind an icon that has none. macOS only (iconutil).
"""

import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter

BMPS = [16, 20, 24, 32, 40, 48, 64]
PNG = 256


def scaled(art, n):
    im = art.resize((n, n), Image.LANCZOS)
    if n <= 32:
        # Small sizes lose the outlines to the filter; sharpen them back
        rgb = im.convert("RGB").filter(ImageFilter.UnsharpMask(1, 60, 2))
        rgb.putalpha(im.getchannel("A"))
        im = rgb
    return im


def dib(im):
    """A 32-bit BMP icon image: header, bottom-up BGRA, then AND mask."""
    n = im.width
    hdr = struct.pack("<IiiHHIIiiII", 40, n, 2*n, 1, 32, 0, 0, 0, 0, 0, 0)
    px = im.tobytes("raw", "BGRA", 0, -1)
    stride = (n + 31)//32*4
    mask = bytearray()
    a = im.getchannel("A").transpose(Image.FLIP_TOP_BOTTOM).tobytes()
    for y in range(n):
        row = bytearray(stride)
        for x in range(n):
            if a[y*n + x] == 0:
                row[x//8] |= 0x80 >> (x % 8)
        mask += row
    return hdr + px + bytes(mask)


def png(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


def ico(art):
    images = [dib(scaled(art, n)) for n in BMPS] + [png(scaled(art, PNG))]
    sizes = BMPS + [PNG]
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16*len(images)
    for n, d in zip(sizes, images):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(d), off)
        off += len(d)
    return out + b"".join(images)


def tile(art, n):
    """The artwork on a rounded navy tile, on Apple's 1024 grid."""
    s = 1024
    t, r = 100, 185  # the tile's inset and its corner radius
    grad = Image.new("RGBA", (s, s))
    top, bot = (38, 52, 104), (10, 16, 44)
    for y in range(s):
        f = y/(s - 1)
        grad.paste(tuple(round(a + (b - a)*f) for a, b in zip(top, bot)) + (255,),
                   (0, y, s, y + 1))
    mask = Image.new("L", (s, s))
    ImageDraw.Draw(mask).rounded_rectangle((t, t, s - t - 1, s - t - 1), r, fill=255)
    out = Image.new("RGBA", (s, s))
    out.paste(grad, (0, 0), mask)
    a = 700
    out.alpha_composite(art.resize((a, a), Image.LANCZOS), ((s - a)//2, (s - a)//2))
    return out.resize((n, n), Image.LANCZOS)


def icns(art, path):
    if not shutil.which("iconutil"):
        print(f"{path}: skipped, no iconutil", file=sys.stderr)
        return
    big = tile(art, 1024)
    with tempfile.TemporaryDirectory() as d:
        iconset = os.path.join(d, "prospero.iconset")
        os.mkdir(iconset)
        for n in [16, 32, 128, 256, 512]:
            big.resize((n, n), Image.LANCZOS).save(f"{iconset}/icon_{n}x{n}.png")
            big.resize((2*n, 2*n), Image.LANCZOS).save(f"{iconset}/icon_{n}x{n}@2x.png")
        subprocess.run(["iconutil", "-c", "icns", "-o", path, iconset], check=True)


def offsets(path):
    with open(path, "rb") as f:
        data = f.read()
    _, _, count = struct.unpack_from("<HHH", data)
    for i in range(count):
        e = 6 + 16*i
        w = data[e] or 256
        size, off = struct.unpack_from("<II", data, e + 8)
        print(f"entry {e}, 12; {w}x{w} at {off}, {size} bytes")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--art", default="assets/icon.png")
    p.add_argument("--out", default="assets")
    p.add_argument("--offsets", action="store_true",
                   help="print prospero.ico's layout, build nothing")
    args = p.parse_args()
    if args.offsets:
        offsets(f"{args.out}/prospero.ico")
        return
    art = Image.open(args.art).convert("RGBA")
    if art.width != art.height:
        sys.exit(f"{args.art}: {art.width}x{art.height}, not square")
    with open(f"{args.out}/prospero.ico", "wb") as f:
        f.write(ico(art))
    with open(f"{args.out}/prospero.png", "wb") as f:
        f.write(png(scaled(art, PNG)))
    icns(art, f"{args.out}/prospero.icns")
    offsets(f"{args.out}/prospero.ico")


if __name__ == "__main__":
    main()
