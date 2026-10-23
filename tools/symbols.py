#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["fonttools==4.67.0"]
# ///
"""Make the symbol fonts' subsets that the GUI embeds (assets/).

  uv run tools/symbols.py [--check]

Roboto and JetBrains Mono, merged as the transcript's fonts, lack some
symbols that models commonly write: heavy check marks and crosses,
ballot boxes, stars, and arrows such as the return arrow. These come
from Noto Sans Symbols 2 and Noto Sans Math (both SIL OFL 1.1), as
google/fonts has them at pinned commits, checked by SHA-256, cut down to
the code points below. The subsets are deterministic: the same inputs
and fontTools version make the same bytes. --check makes them in memory
and compares them with assets/, writing nothing.
"""

import argparse
import hashlib
import io
import os
import sys
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GF = "https://raw.githubusercontent.com/google/fonts"

FONTS = [
    {
        "url": f"{GF}/7b6724ac7ececc713e9ba93af309f7520c9a80a3/ofl/"
               "notosanssymbols2/NotoSansSymbols2-Regular.ttf",
        "sha256": "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21",
        "out": "NotoSansSymbols2-subset.ttf",
        # Dingbats, ballot boxes, stars, hourglasses, media controls, suits
        "text": "✔✘✖✕★☆✦✧✱✳❖☐☑☒⏳⌛✎✏✂➤⮕⏵⏷⏸⏹⏺⚬♠♣♥♦",
    },
    {
        "url": f"{GF}/dbd1ab6e65dc59bcda3ca8de9fd372f58f98e0af/ofl/"
               "notosansmath/NotoSansMath-Regular.ttf",
        "sha256": "3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d",
        "out": "NotoSansMath-subset.ttf",
        # Arrows with corners, circles, pairs; long double arrows; bullets
        "text": "↵↳↲↰↱⤷⤶↻↺⟳⟲⇄⇆⇅⟹⟸⟺⁃⊨∝⌀",
    },
]


def fetch(url, sha256):
    with urllib.request.urlopen(url) as r:
        data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        sys.exit(f"{url}: SHA-256 {got}, expected {sha256}")
    return data


def make(data, text):
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    opts = subset.Options()
    opts.name_IDs = ["*"]       # the copyright and license stay
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.layout_features = []   # symbols: no shaping
    opts.hinting = False        # FreeType's autohinter, as for the rest
    opts.drop_tables += ["MATH", "DSIG"]
    s = subset.Subsetter(opts)
    s.populate(text=text)
    s.subset(font)
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="compare with assets/, writing nothing")
    args = ap.parse_args()
    bad = 0
    for f in FONTS:
        data = make(fetch(f["url"], f["sha256"]), f["text"])
        path = os.path.join(ROOT, "assets", f["out"])
        digest = hashlib.sha256(data).hexdigest()
        if args.check:
            try:
                with open(path, "rb") as fp:
                    same = fp.read() == data
            except OSError:
                same = False
            print(f"{f['out']}: {'ok' if same else 'DIFFERS'} {digest}")
            bad |= not same
        else:
            with open(path, "wb") as fp:
                fp.write(data)
            print(f"{f['out']}: {len(data)} bytes, SHA-256 {digest}")
    return bad


if __name__ == "__main__":
    sys.exit(main())
