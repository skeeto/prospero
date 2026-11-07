# /// script
# requires-python = ">=3.10"
# ///
# gentables: Unicode tables for ariel, spliced into ariel.c
#
#   $ uv run ariel/tools/gentables.py
#
# Downloads the pinned UCD files into a cache (~/.cache/ariel-ucd/VERSION,
# or --cache DIR), checks them by SHA-256, and rewrites the block between
# "// BEGIN UNICODE" and "// END UNICODE" in ariel.c. Also writes the
# grapheme and emoji test data under ariel/tests/. Standard library only.
#
# Each code point gets a property: its width class, grapheme break
# class, Extended_Pictographic, Indic_Conjunct_Break and whether it takes
# an emoji presentation with VS16. Properties are interned, and the
# per-code-point index goes into a three-stage table whose bin sizes are
# searched for the fewest bytes.
#
# The Unicode data is under the Unicode License V3; see README.md.
#
# This is free and unencumbered software released into the public domain.

import argparse
import hashlib
import os
import sys
import urllib.request

VERSION = "18.0.0"
BASE = f"https://www.unicode.org/Public/{VERSION}/"
FILES = {
    "EastAsianWidth.txt":       ("ucd/EastAsianWidth.txt",
        "a0cf29eacd00cfcaec4381c6b7c281685f18dbb4e7ff82b4076ccb342ca839aa"),
    "DerivedGeneralCategory.txt": ("ucd/extracted/DerivedGeneralCategory.txt",
        "d6b151d2d40ee9b1876d26f417980f45ffae47b6055ccf7203cb31f07a030f94"),
    "DerivedCoreProperties.txt": ("ucd/DerivedCoreProperties.txt",
        "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9"),
    "PropList.txt":             ("ucd/PropList.txt",
        "f438f532e8737bb8a2702126cdf9c4af5e357c58c7acf9d9eb2fc7c1a1d955d6"),
    "emoji-data.txt":           ("ucd/emoji/emoji-data.txt",
        "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1"),
    "emoji-variation-sequences.txt": ("ucd/emoji/emoji-variation-sequences.txt",
        "ff1707564aa1f1b2fcf4ec92d609d4cb26940bc0d4dcf07f5328ad6879e84da3"),
    "GraphemeBreakProperty.txt": ("ucd/auxiliary/GraphemeBreakProperty.txt",
        "0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a"),
    "GraphemeBreakTest.txt":    ("ucd/auxiliary/GraphemeBreakTest.txt",
        "b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31"),
    "emoji-test.txt":           ("emoji/emoji-test.txt",
        "8f3735cda1f92a779d78af67cf86066bb1f07143dc22f2ac29394d9bc57ab21a"),
}

NCP = 0x110000

# Must match ariel.c
WIDTH = {"zero": 0, "narrow": 1, "wide": 2, "ambig": 3}
GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
       "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
INCB = ["None", "Linker", "Consonant", "Extend"]


def fetch(cache, name):
    path, digest = FILES[name]
    local = os.path.join(cache, name)
    if not os.path.exists(local):
        os.makedirs(cache, exist_ok=True)
        print(f"fetching {path}", file=sys.stderr)
        with urllib.request.urlopen(BASE + path) as r:
            data = r.read()
        with open(local + ".tmp", "wb") as f:
            f.write(data)
        os.replace(local + ".tmp", local)
    with open(local, "rb") as f:
        data = f.read()
    got = hashlib.sha256(data).hexdigest()
    if got != digest:
        sys.exit(f"gentables: {name}: SHA-256 {got}, want {digest}")
    return data.decode("utf-8")


def records(text):
    """(first, last, fields) for each data line."""
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        r = fields[0].split("..")
        lo = int(r[0], 16)
        hi = int(r[1], 16) if len(r) > 1 else lo
        yield lo, hi, fields[1:]


def build(cache):
    gc = ["Cn"] * NCP
    for lo, hi, f in records(fetch(cache, "DerivedGeneralCategory.txt")):
        for c in range(lo, hi + 1):
            gc[c] = f[0]

    eaw = ["N"] * NCP
    for lo, hi, f in records(fetch(cache, "EastAsianWidth.txt")):
        for c in range(lo, hi + 1):
            eaw[c] = f[0]

    ignorable = [False] * NCP
    incb = [0] * NCP
    for lo, hi, f in records(fetch(cache, "DerivedCoreProperties.txt")):
        if f[0] == "Default_Ignorable_Code_Point":
            for c in range(lo, hi + 1):
                ignorable[c] = True
        elif f[0] == "InCB":
            for c in range(lo, hi + 1):
                incb[c] = INCB.index(f[1])

    prepended = [False] * NCP
    for lo, hi, f in records(fetch(cache, "PropList.txt")):
        if f[0] == "Prepended_Concatenation_Mark":
            for c in range(lo, hi + 1):
                prepended[c] = True

    pict = [False] * NCP
    for lo, hi, f in records(fetch(cache, "emoji-data.txt")):
        if f[0] == "Extended_Pictographic":
            for c in range(lo, hi + 1):
                pict[c] = True

    vs16 = [False] * NCP
    for line in fetch(cache, "emoji-variation-sequences.txt").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        seq, style = [f.strip() for f in line.split(";")[:2]]
        cps = [int(x, 16) for x in seq.split()]
        if cps[1] == 0xFE0F and style == "emoji style":
            vs16[cps[0]] = True

    gcb = [0] * NCP
    for lo, hi, f in records(fetch(cache, "GraphemeBreakProperty.txt")):
        for c in range(lo, hi + 1):
            gcb[c] = GCB.index(f[0])

    props = {}
    index = bytearray(NCP)
    for c in range(NCP):
        g = gc[c]
        if g in ("Cc", "Cs"):
            w = WIDTH["zero"]
        elif c == 0x00AD or prepended[c]:
            w = WIDTH["narrow"]
        elif g in ("Mn", "Me", "Cf") or ignorable[c]:
            w = WIDTH["zero"]
        elif 0x1160 <= c <= 0x11FF or 0xD7B0 <= c <= 0xD7FF:
            w = WIDTH["zero"]
        elif eaw[c] in ("W", "F"):
            w = WIDTH["wide"]
        elif eaw[c] == "A" and g != "Co":
            w = WIDTH["ambig"]
        else:
            w = WIDTH["narrow"]
        p = w | gcb[c] << 2 | pict[c] << 6 | incb[c] << 7 | vs16[c] << 9
        index[c] = props.setdefault(p, len(props))
        if len(props) > 256:
            sys.exit("gentables: more than 256 properties")
    return [p for p, _ in sorted(props.items(), key=lambda kv: kv[1])], index


def split(t, elem):
    """Best (t1, t2, shift) such that t[i] = t2[(t1[i>>shift]<<shift) + (i&mask)]."""
    best = None
    for shift in range(1, 13):
        size = 1 << shift
        t1, t2, cache = [], [], {}
        for i in range(0, len(t), size):
            b = bytes(t[i:i+size]) if elem == 1 else tuple(t[i:i+size])
            j = cache.get(b)
            if j is None:
                j = len(t2) >> shift
                cache[b] = j
                t2.extend(t[i:i+size])
            t1.append(j)
        cost = len(t2)*elem + len(t1)*width(max(t1))
        if best is None or cost < best[0]:
            best = cost, t1, t2, shift
    return best[1:]


def width(n):
    return 1 if n < 256 else 2 if n < 65536 else 4


def carray(name, ctype, values):
    out = [f"static {ctype} const {name}[{len(values)}] = {{"]
    line = "   "
    for v in values:
        s = f" {v},"
        if len(line) + len(s) > 78:
            out.append(line)
            line = "   "
        line += s
    out.append(line)
    out.append("};")
    return out


def ctype(n):
    return {1: "u8", 2: "u16", 4: "u32"}[width(n)]


def generate(props, index):
    t1, t2, s1 = split(index, 1)
    t0, t1b, s2 = split(t1, width(max(t1)))
    total = len(t2) + len(t1b)*width(max(t1b)) + len(t0)*width(max(t0)) + 2*len(props)
    lines = [
        "// BEGIN UNICODE",
        f"// Generated by tools/gentables.py from Unicode {VERSION}: do not edit.",
        "// Unicode data: Copyright (c) 1991-2026 Unicode, Inc. Unicode License V3",
        "// (see README.md). A property: width class (2 bits), grapheme break",
        "// class (4), Extended_Pictographic (1), InCB (2), VS16 base (1).",
        f"// {len(props)} properties, {total} bytes.",
        "enum {",
        f"    UNI_SHIFT1 = {s1},",
        f"    UNI_SHIFT2 = {s2},",
        "};",
    ]
    lines += carray("uniprops", "u16", props)
    lines += carray("unistage0", ctype(max(t0)), t0)
    lines += carray("unistage1", ctype(max(t1b)), t1b)
    lines += carray("unistage2", "u8", list(t2))
    lines.append("// END UNICODE")
    print(f"gentables: {len(props)} properties, {total} bytes "
          f"(shifts {s1}, {s2})", file=sys.stderr)
    return "\n".join(lines) + "\n"


def splice(path, block):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    beg = text.index("// BEGIN UNICODE")
    end = text.index("// END UNICODE")
    end = text.index("\n", end) + 1
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text[:beg] + block + text[end:])


NOTICE = (
    "# Unicode {v} test data, derived from {src}.\n"
    "# Copyright (c) 1991-2026 Unicode, Inc. Unicode License V3 (see README.md).\n"
)


def testdata(cache, dst):
    os.makedirs(dst, exist_ok=True)
    out = [NOTICE.format(v=VERSION, src="GraphemeBreakTest.txt"),
           "# Code points with / (break) or + (no break) between them.\n"]
    for line in fetch(cache, "GraphemeBreakTest.txt").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        line = line.replace("÷", "/").replace("×", "+")
        out.append(" ".join(line.split()) + "\n")
    with open(os.path.join(dst, "grapheme.txt"), "w", newline="\n") as f:
        f.writelines(out)

    out = [NOTICE.format(v=VERSION, src="emoji-test.txt"),
           "# Fully-qualified RGI emoji sequences: one cluster, two cells.\n"]
    for line in fetch(cache, "emoji-test.txt").splitlines():
        if "; fully-qualified" in line:
            out.append(line.split(";")[0].strip() + "\n")
    with open(os.path.join(dst, "emoji.txt"), "w", newline="\n") as f:
        f.writelines(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(
        os.path.expanduser("~"), ".cache", "ariel-ucd", VERSION))
    args = ap.parse_args()
    props, index = build(args.cache)
    splice(os.path.join(root, "ariel.c"), generate(props, index))
    testdata(args.cache, os.path.join(root, "tests"))


if __name__ == "__main__":
    main()
