#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Print the PDF reader's encoding, glyph name and width tables (pdfenc.cpp).

  uv run tools/pdftables.py >tables.txt

Every table comes from files on this machine, paths below, and nothing
is downloaded. The output is pasted into src/pdfenc.cpp between its
"generated" markers.

  Glyph names   TeX Live's copy of the Adobe Glyph List 2.0 (glyphlist.txt),
                plus lcdf-typetools' texglyphlist.txt for TeX's names
  Standard      dvips 8a.enc (Adobe StandardEncoding), names through the AGL
  TeX T1/Cork   dvips ec.enc, names through the AGL
  MacRoman      Python's mac_roman codec, with the PDF spec's 0xDB
  WinAnsi       Python's cp1252 codec, holes as the spec's bullet
  PDFDoc        Latin-1 plus the deltas in PDF 32000-1 Annex D, below
  Symbol        Tcl's symbol.enc, limited to the codes psyr.afm has, with
                some glyphs by their psyr.afm names (SYMBYNAME)
  ZapfDingbats  Tcl's dingbats.enc, limited to the codes pzdr.afm has
  Widths        Adobe ptm{r,b,ri,bi}8a.afm (Times), and URW n01900{3,4}l.afm
                (Nimbus Sans L, Helvetica's metric twin) from Ghostscript;
                Helvetica's obliques have its upright widths

The glyph name subset is every AGL name for a code point in the wanted
ranges (Latin, Greek, Cyrillic, punctuation, letterlike, arrows, math)
or in any encoding table, less the afii100NN Cyrillic names that the
reader computes, plus the TeX names, and ligature names as their letters.
"""

import sys

TL = "/usr/local/texlive/2026basic/texmf-dist/fonts"
GLYPHLIST = TL + "/map/glyphlist/glyphlist.txt"
TEXGLYPHLIST = TL + "/map/glyphlist/texglyphlist.txt"
STDENC = TL + "/enc/dvips/base/8a.enc"
CORKENC = TL + "/enc/dvips/base/ec.enc"
SYMBOLAFM = TL + "/afm/adobe/symbol/psyr.afm"
ZAPFAFM = TL + "/afm/adobe/zapfding/pzdr.afm"
TIMES = TL + "/afm/adobe/times/"
URW = "/opt/homebrew/Cellar/ghostscript/10.08.0/share/ghostscript/fonts/"
TCL = "/System/Library/Frameworks/Tcl.framework/Versions/8.5/Resources/Scripts/encoding"

# PDF 32000-1 Annex D: where PDFDocEncoding departs from Latin-1
PDFDOC = {
    0x18: "breve", 0x19: "caron", 0x1a: "circumflex", 0x1b: "dotaccent",
    0x1c: "hungarumlaut", 0x1d: "ogonek", 0x1e: "ring", 0x1f: "tilde",
    0x80: "bullet", 0x81: "dagger", 0x82: "daggerdbl", 0x83: "ellipsis",
    0x84: "emdash", 0x85: "endash", 0x86: "florin", 0x87: "fraction",
    0x88: "guilsinglleft", 0x89: "guilsinglright", 0x8a: "minus",
    0x8b: "perthousand", 0x8c: "quotedblbase", 0x8d: "quotedblleft",
    0x8e: "quotedblright", 0x8f: "quoteleft", 0x90: "quoteright",
    0x91: "quotesinglbase", 0x92: "trademark", 0x93: "fi", 0x94: "fl",
    0x95: "Lslash", 0x96: "OE", 0x97: "Scaron", 0x98: "Ydieresis",
    0x99: "Zcaron", 0x9a: "dotlessi", 0x9b: "lslash", 0x9c: "oe",
    0x9d: "scaron", 0x9e: "zcaron", 0xa0: "Euro",
}

# TeX names whose texglyphlist meaning beats the AGL's, and Greek
# letters over their AGL symbol meanings (Ohm, increment)
PREFER = {"phi1": [0x3c6], "Delta": [0x394], "Omega": [0x3a9]}

# Symbol's extension pieces, private use in the AGL, as the Unicode 3.2
# pieces that came later
SYMPIECES = {
    "arrowvertex": 0x23d0, "arrowhorizex": 0x23af,
    "parenlefttp": 0x239b, "parenleftex": 0x239c, "parenleftbt": 0x239d,
    "parenrighttp": 0x239e, "parenrightex": 0x239f, "parenrightbt": 0x23a0,
    "bracketlefttp": 0x23a1, "bracketleftex": 0x23a2, "bracketleftbt": 0x23a3,
    "bracketrighttp": 0x23a4, "bracketrightex": 0x23a5, "bracketrightbt": 0x23a6,
    "bracelefttp": 0x23a7, "braceleftmid": 0x23a8, "braceleftbt": 0x23a9,
    "braceex": 0x23aa, "bracerighttp": 0x23ab, "bracerightmid": 0x23ac,
    "bracerightbt": 0x23ad, "integralex": 0x23ae,
}

# Symbol glyphs taken by name, where Tcl's symbol.enc disagrees with the
# AGL (suchthat, lozenge) or uses the private use area
SYMBYNAME = set(SYMPIECES) | {"suchthat", "lozenge"}

# Ligature names read as their letters
LIGATURES = {
    "ff": "ff", "fi": "fi", "fl": "fl", "ffi": "ffi", "ffl": "ffl",
    "st": "st", "ct": "ct", "SS": "SS", "Germandbls": "SS",
}

WANTED = [
    (0x0020, 0x007e), (0x00a0, 0x017f), (0x0218, 0x021b),
    (0x0370, 0x03ff), (0x0400, 0x045f), (0x0490, 0x0491),
    (0x2000, 0x206f), (0x20a0, 0x20bf), (0x2100, 0x2135),
    (0x2190, 0x2199), (0x21d0, 0x21d5), (0x2200, 0x2265),
    (0x2282, 0x22a5), (0x22c4, 0x22c5), (0x2308, 0x230b),
    (0xfb00, 0xfb06),
]


def wanted(c):
    return any(lo <= c <= hi for lo, hi in WANTED)


def pua(c):
    return 0xe000 <= c <= 0xf8ff or 0xd800 <= c <= 0xdfff


def readagl():
    agl = {}
    for line in open(GLYPHLIST):
        if line.startswith("#"):
            continue
        name, vals = line.strip().split(";")
        cps = [int(v, 16) for v in vals.split()]
        if len(cps) == 1:
            agl[name] = cps
    for line in open(TEXGLYPHLIST):
        if line.startswith("#") or ";" not in line:
            continue
        name, vals = line.strip().split(";")
        cps = [int(v, 16) for v in vals.split(",")[0].split()]
        if len(cps)!=1 or pua(cps[0]) or cps[0]>0xffff:
            continue
        if name not in agl or pua(agl[name][0]):
            agl[name] = cps
    agl.update(PREFER)
    agl.update((n, [c]) for n, c in SYMPIECES.items())
    for name, letters in LIGATURES.items():
        agl[name] = [ord(c) for c in letters]
    return agl


def encnames(path):
    names = []
    body = open(path).read().split("[", 1)[1].split("]", 1)[0]
    for line in body.splitlines():
        line = line.split("%", 1)[0]
        names += [w[1:] for w in line.split() if w.startswith("/")]
    assert len(names) == 256, path
    return names


def bynames(names, agl):
    t = [0] * 256
    for i, n in enumerate(names):
        if n in ("ff", "fi", "fl", "ffi", "ffl"):
            t[i] = 0xfb00 + ["ff", "fi", "fl", "ffi", "ffl"].index(n)
        elif n in agl and len(agl[n]) == 1 and not pua(agl[n][0]):
            t[i] = agl[n][0]
        elif n in agl and len(agl[n]) > 1:
            t[i] = 0  # a letter pair, such as SS, has no one code point
        elif n not in (".notdef", "perthousandzero"):
            sys.exit(f"pdftables: no code point for /{n}")
    return t


def afmcodes(path, unencoded=False):
    r = {}
    for line in open(path, encoding="latin-1"):
        if line.startswith("C "):
            f = [p.strip() for p in line.split(";")]
            code = int(f[0].split()[1])
            wx = int(f[1].split()[1])
            name = f[2].split()[1]
            if code >= 0 or unencoded:
                r[code if code>=0 else name] = (wx, name)
    return r


def tclenc(path):
    lines = open(path).read().splitlines()
    rows = [l for l in lines[4:] if l.strip()]
    hexs = "".join(rows[:16])
    return [int(hexs[i:i+4], 16) for i in range(0, 256*4, 4)]


def tables(agl):
    t = {}
    t["PDF_STDENC"] = bynames(encnames(STDENC), agl)
    t["PDF_CORK"] = bynames(encnames(CORKENC), agl)

    mac = list(range(128)) + [ord(c) for c in bytes(range(128, 256)).decode("mac_roman")]
    mac[0xdb] = 0xa4  # currency in PDF, Euro in later Mac OS
    mac[0xf0] = 0     # the Apple logo, in the private use area
    mac[0x7f] = 0
    t["PDF_MACROMAN"] = mac

    win = []
    for c in range(256):
        try:
            win.append(ord(bytes([c]).decode("cp1252")))
        except UnicodeDecodeError:
            win.append(0x2022)
    win[0x7f] = 0x2022
    t["PDF_WINANSI"] = win

    doc = list(range(256))
    for c, n in PDFDOC.items():
        doc[c] = agl[n][0] if n not in ("fi", "fl") else 0xfb01 + (n == "fl")
    doc[0x9f] = doc[0xad] = doc[0x7f] = 0
    t["PDF_PDFDOC"] = doc

    sym = tclenc(TCL + "/symbol.enc")
    codes = afmcodes(SYMBOLAFM)
    fix = {0xf8e8: 0xae, 0xf8e9: 0xa9, 0xf8ea: 0x2122}  # sans serif forms
    for c in range(256):
        v = fix.get(sym[c], sym[c])
        sym[c] = v if c in codes and not pua(v) else 0
    for c, (_, name) in codes.items():
        if name in SYMBYNAME:
            sym[c] = agl[name][0]
    sym[0xa0] = 0x20ac  # added in later versions of the font
    t["PDF_SYMBOL"] = sym

    zapf = tclenc(TCL + "/dingbats.enc")
    codes = afmcodes(ZAPFAFM)
    for c in range(256):
        zapf[c] = zapf[c] if c in codes and not pua(zapf[c]) else 0
    t["PDF_ZAPF"] = zapf

    for name in ("PDF_STDENC", "PDF_MACROMAN", "PDF_PDFDOC", "PDF_WINANSI"):
        for c in range(32):  # controls, except PDFDoc's glyphs and its
            if t[name][c] == c and not (name == "PDF_PDFDOC" and c in (9, 10, 13)):
                t[name][c] = 0
    return t


def subset(agl, tabs):
    inenc = set()
    for name, tab in tabs.items():
        if name != "PDF_ZAPF":  # whose glyphs are named a1 to a191
            inenc.update(v for v in tab if v)
    r = {}
    for name, cps in agl.items():
        if name.startswith("afii100") and len(name) == 9:
            n = int(name[4:])
            if 10017<=n<=10049 or 10065<=n<=10097:
                continue  # computed
        if name in LIGATURES or all(wanted(c) or c in inenc for c in cps):
            if not any(pua(c) for c in cps):
                r[name] = cps
    return r


def emittable(name, tab, comment):
    print(f"// {comment}")
    print(f"static u16 const {name}[256] = {{")
    for i in range(0, 256, 8):
        print("    " + " ".join(f"0x{v:04x}," for v in tab[i:i+8]))
    print("};")
    print()


def emitwidths(name, path, comment):
    # The AFMs are in StandardEncoding, where 39 and 96 are curly quotes,
    # so take each width by its ASCII glyph's name.
    names = encnames(STDENC)
    names[39] = "quotesingle"
    names[96] = "grave"
    byname = {n: wx for wx, n in afmcodes(path, True).values()}
    w = [byname[names[c]] for c in range(32, 127)]
    print(f"// {comment}")
    print(f"static u16 const {name}[95] = {{")
    for i in range(0, 95, 12):
        print("    " + " ".join(f"{v:3d}," for v in w[i:i+12]))
    print("};")
    print()


def main():
    agl = readagl()
    tabs = tables(agl)
    emittable("PDF_STDENC", tabs["PDF_STDENC"], "StandardEncoding")
    emittable("PDF_MACROMAN", tabs["PDF_MACROMAN"], "MacRomanEncoding")
    emittable("PDF_WINANSI", tabs["PDF_WINANSI"], "WinAnsiEncoding, its holes the spec's bullet")
    emittable("PDF_PDFDOC", tabs["PDF_PDFDOC"], "PDFDocEncoding")
    emittable("PDF_SYMBOL", tabs["PDF_SYMBOL"], "Symbol's built-in encoding")
    emittable("PDF_ZAPF", tabs["PDF_ZAPF"], "ZapfDingbats' built-in encoding")
    emittable("PDF_CORK", tabs["PDF_CORK"], "TeX T1 (Cork), for Type3 fonts named by code")

    names = sorted(subset(agl, tabs).items(), key=lambda kv: kv[0].encode())
    assert all(len(cps) <= 3 and max(cps) <= 0xffff for _, cps in names)
    assert max(len(n) for n, _ in names) < 256
    print(f"// {len(names)} glyph names, sorted bytewise, packed without separators")
    print("static char const PDF_AGLNAMES[] =")
    line = ""
    for n, _ in names:
        if len(line) + len(n) > 70:
            print(f'    "{line}"')
            line = ""
        line += n
    print(f'    "{line}";')
    print()
    print(f"static PdfAgl const PDF_AGL[{len(names)}] = {{")
    off = 0
    row = []
    for n, cps in names:
        cp = ",".join(f"0x{c:04x}" for c in cps)
        row.append(f"{{{off},{len(n)},{{{cp}}}}},")
        off += len(n)
        if len(" ".join(row)) > 66:
            print("    " + " ".join(row))
            row = []
    if row:
        print("    " + " ".join(row))
    print("};")
    print()
    assert off < 1<<16

    emitwidths("PDF_HELVW", URW + "n019003l.afm", "Helvetica's widths, 32 to 126, in 1/1000 em")
    emitwidths("PDF_HELVBW", URW + "n019004l.afm", "Helvetica-Bold's widths")
    emitwidths("PDF_TIMESW", TIMES + "ptmr8a.afm", "Times-Roman's widths")
    emitwidths("PDF_TIMESBW", TIMES + "ptmb8a.afm", "Times-Bold's widths")
    emitwidths("PDF_TIMESIW", TIMES + "ptmri8a.afm", "Times-Italic's widths")
    emitwidths("PDF_TIMESBIW", TIMES + "ptmbi8a.afm", "Times-BoldItalic's widths")


main()
