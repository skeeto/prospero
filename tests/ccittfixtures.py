#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The CCITT fixtures in src/test/pdfimage_test.cpp, made again.

Draws the test's pattern (ccpattern there) as an uncompressed bilevel
TIFF, MinIsWhite with 1 for black in one strip, compresses it with
libtiff's tiffcp as G4, G3 1-D and G3 2-D, with and without fill, and
prints each strip as the test's hex literal. Given the corpus's
scan-ccitt.pdf, its image stream (object 7) too, through qpdf.

  uv run tests/ccittfixtures.py OUTDIR [scan-ccitt.pdf]
"""
import os
import struct
import subprocess
import sys

COLS, ROWS = 2700, 24


def black(x, y):
    if y == 5:
        return 1
    if y == 6:
        return 0
    if y == 20:
        return int(x >= 1)
    if x < 160:
        return (x + y % 4) // 7 % 2
    if x < 600:
        return (x + y*5 % 11) // 37 % 2
    if x < 2650:
        dx, dy = x - 1900, y*27 - 320
        d = dx*dx + dy*dy
        return int(d < 600*600 and d >= 100*100)
    return 0


def writetiff(path):
    stride = (COLS + 7) // 8
    data = bytearray()
    for y in range(ROWS):
        row = bytearray(stride)
        for x in range(COLS):
            if black(x, y):
                row[x >> 3] |= 0x80 >> (x & 7)
        data += row
    tags = [(256, 3, COLS), (257, 3, ROWS), (258, 3, 1), (259, 3, 1), (262, 3, 0),
            (273, 4, 0), (277, 3, 1), (278, 3, ROWS), (279, 4, len(data))]
    off = 8 + 2 + 12*len(tags) + 4
    out = bytearray(b"II*\0" + struct.pack("<IH", 8, len(tags)))
    for tag, kind, val in tags:
        val = off if tag == 273 else val
        out += struct.pack("<HHI", tag, kind, 1)
        out += struct.pack("<HH", val, 0) if kind == 3 else struct.pack("<I", val)
    out += b"\0\0\0\0" + data
    with open(path, "wb") as f:
        f.write(out)


def strip(path):
    with open(path, "rb") as f:
        d = f.read()
    ifd = struct.unpack("<I", d[4:8])[0]
    vals = {}
    for i in range(struct.unpack("<H", d[ifd:ifd + 2])[0]):
        tag, kind, count, val = struct.unpack("<HHII", d[ifd + 2 + 12*i:ifd + 14 + 12*i])
        vals[tag] = (count, val & 0xffff if kind == 3 else val)
    assert vals[273][0] == 1, "one strip"
    return d[vals[273][1]:vals[273][1] + vals[279][1]]


def literal(name, data):
    h = data.hex()
    lines = [f'    "{h[i:i + 72]}"' for i in range(0, len(h), 72)]
    return f"static char const {name}[] =  // {len(data)} bytes\n" + "\n".join(lines) + ";"


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    src = os.path.join(out, "src.tif")
    writetiff(src)
    for name, opt in [("ccg4", "g4"), ("ccg31d", "g3:1d"), ("ccg31dfill", "g3:1d:fill"),
                      ("ccg32d", "g3:2d"), ("ccg32dfill", "g3:2d:fill")]:
        tif = os.path.join(out, name + ".tif")
        subprocess.run(["tiffcp", "-c", opt, "-r", str(ROWS), src, tif], check=True)
        print(literal(name, strip(tif)))
    if len(sys.argv) > 2:
        scan = subprocess.run(["qpdf", "--show-object=7", "--raw-stream-data", sys.argv[2]],
                              check=True, capture_output=True).stdout
        print(literal("ccscan", scan))


if __name__ == "__main__":
    main()
