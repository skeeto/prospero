#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["fonttools==4.67.0"]
# ///
"""Make the symbol and emoji fonts' subsets that the GUI embeds (assets/).

  uv run tools/symbols.py [--check] [--cache DIR] [--only OUT]

Roboto and JetBrains Mono, merged as the transcript's fonts, lack some
symbols that models commonly write: heavy check marks and crosses,
ballot boxes, stars, and arrows such as the return arrow. These come
from Noto Sans Symbols 2 and Noto Sans Math (both SIL OFL 1.1), as
google/fonts has them at pinned commits, checked by SHA-256, cut down to
the code points below.

Emoji come from Twemoji as Mozilla builds it, COLRv0 (color layers of
outlines, which FreeType draws itself), from its v0.7.0 release: whole
blocks of the common ones, and a pick of the two large pictograph
blocks, less what the fonts ahead of it have (which stay text, as in a
terminal), and with no sequences (flags, skin tones, joined families),
which ImGui, without shaping, cannot form.

The subsets are deterministic: the same inputs
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
    {
        "url": "https://github.com/mozilla/twemoji-colr/releases/download/"
               "v0.7.0/Twemoji.Mozilla.ttf",
        "sha256": "6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e",
        "out": "TwemojiMozilla-subset.ttf",
        # Every emoji below U+FFFF, the emoticons, transport and colored
        # shapes, then the pictographs models use: status, work, objects
        "ranges": [(0x00a0, 0xffff), (0x1f600, 0x1f64f), (0x1f680, 0x1f6ff),
                   (0x1f780, 0x1f7ff)],
        "text": "🌐🌙🌟🌈🍕🎉🎊🎨🎯🏁🏆🏗🐛🐞👀👋👍👎👏💡💥💪💬💭💯💰💻💾📁📂📄📅📈📉📊📋📌📍📎📏"
                "📐📝📣📦📱📷🔀🔁🔄🔍🔎🔐🔒🔓🔑🔔🔕🔗🔥🔧🔨🔩🔬🔴🔵🕐🕒🖥🖱🗂🗑🗒🗓🗝"
                "🤔🤖🤝🤞🤷🥇🥈🥉🦀🦄🧠🧩🧪🧭🧮🧯🧰🧱🧲🧵🧹🧾🫡🫠🪄🪛🪜🪝🪲",
        "skip": ["Roboto-Medium.ttf", "JetBrainsMono-Regular.ttf",
                 "NotoSansSymbols2-subset.ttf", "NotoSansMath-subset.ttf"],
        "drop": ["GSUB", "GPOS", "GDEF", "FFTM"],
    },
]


def fetch(url, sha256, cache=None):
    """The file at url, or its copy of the same name in cache, if there."""
    path = cache and os.path.join(cache, url.rsplit("/", 1)[-1])
    if path and os.path.exists(path):
        with open(path, "rb") as fp:
            data = fp.read()
    else:
        with urllib.request.urlopen(url) as r:
            data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        sys.exit(f"{url}: SHA-256 {got}, expected {sha256}")
    return data


def codepoints(f, font):
    """The font's code points the entry asks for, less those the fonts
    ahead of it in the chain have, and the joiners and selectors."""
    have = font.getBestCmap()
    want = {ord(c) for c in f.get("text", "")}
    for lo, hi in f.get("ranges", []):
        want |= {c for c in have if lo <= c <= hi}
    for name in f.get("skip", []):
        want -= set(TTFont(os.path.join(ROOT, "assets", name)).getBestCmap())
    want -= {0x200d, 0x20e3, *range(0xfe00, 0xfe10)}
    return sorted(c for c in want if c in have)


def make(data, f):
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    opts = subset.Options()
    opts.name_IDs = ["*"]       # the copyright and license stay
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.layout_features = []   # symbols: no shaping
    opts.hinting = False        # FreeType's autohinter, as for the rest
    opts.drop_tables += ["MATH", "DSIG"] + f.get("drop", [])
    s = subset.Subsetter(opts)
    s.populate(unicodes=codepoints(f, font))
    s.subset(font)
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="compare with assets/, writing nothing")
    ap.add_argument("--cache", metavar="DIR",
                    help="take the source fonts from DIR where it has them")
    ap.add_argument("--only", metavar="OUT", help="make only this subset")
    args = ap.parse_args()
    bad = 0
    for f in FONTS:
        if args.only and f["out"] != args.only:
            continue
        data = make(fetch(f["url"], f["sha256"], args.cache), f)
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
