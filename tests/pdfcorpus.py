#!/usr/bin/env python3
"""Build a corpus of PDFs for testing PDF reading: real producers'
output, structural variants of it, and hand-made oddities and damage.

    $ uv run tests/pdfcorpus.py OUT
    $ prospero eval -m MODEL -o pdf.jsonl OUT/eval
    $ uv run tests/pdfcorpus.py --seeds SEEDS OUT


OUT gets pdfs/ (the corpus), manifest.json (what each file is, what a
reader should make of it, and facts only its content states), and
eval/, a suite for prospero eval: each task asks for one file's facts,
with read and write alone, so that the answer comes through read's own
handling of PDFs and not the shell. A model is chosen when running it.

Producers are this machine's programs, each skipped with a note if
missing: pdflatex, xelatex and lualatex (TeX Live), Google Chrome
(headless printing), cupsfilter (macOS Quartz), Ghostscript, qpdf, and
tesseract through Ghostscript's pdfocr8. Poppler's pdftotext and pdfinfo
check every file after (--check). Python's standard library only.

With --seeds, it writes seeds for the PDF fuzz target (src/main_fuzz.cpp)
into SEEDS from the corpus in OUT, building it first if OUT has no
manifest.json: each file of 64 KiB or less, each file's qpdf --qdf form
with streams uncompressed when that fits (its first page alone when the
whole does not), so that AFL++ mutates structure rather than DEFLATE,
and small hostile and one-filter files made here. Run afl-cmin on them.

Facts are made-up words and numbers, seeded by file name, so no model
knows them beforehand and no two files share them.

This is free and unencumbered software released into the public domain.
"""

import argparse
import base64
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

WORDS = """amber anchor apricot badger basalt beacon bramble cactus caldera
canary cinder cobalt copper cormorant crocus cypress dahlia delta dingo
ember falcon fennel fjord garnet geyser glacier granite heron hickory
indigo jackal jasper juniper kestrel lagoon lantern larch lichen lotus
magpie mallow marlin meadow mistral nectar nimbus nutmeg obsidian ocelot
orchid osprey otter paprika pelican pewter plover quartz quince raven
saffron salmon sorrel sparrow spruce tamarind tangelo thistle topaz
tundra umber velvet walnut willow wombat yarrow zephyr""".split()


class Corpus:
    """The files made so far and what each should read as."""

    def __init__(self, out, only=None):
        self.out = out
        self.pdfs = os.path.join(out, "pdfs")
        self.work = os.path.join(out, "work")
        self.only = only
        self.entries = []
        self.skipped = []
        os.makedirs(self.pdfs, exist_ok=True)
        os.makedirs(self.work, exist_ok=True)

    def wanted(self, name):
        return not self.only or any(re.search(p, name) for p in self.only)

    def facts(self, name, n=4):
        """n made-up facts for a file: (word, number) pairs."""
        r = random.Random(hashlib.sha256(name.encode()).digest())
        words = r.sample(WORDS, n)
        return [(w, r.randrange(1000, 9999)) for w in words]

    def add(self, name, data, family, about, expect="text", pages=None,
            questions=(), producer=None, notes=None):
        """Keep a file. expect: what a reader should do with it.
          text     read its text (and questions are answerable)
          partial  read what it can of a damaged file
          refuse   refuse it, saying why (encrypted, not a PDF)
          bounded  give up cleanly, within time and memory limits
        questions: (question, [regex, ...]) answerable from its content,
        a regex starting with ! one that must not match, and a third
        item "image" for one only its pictures answer."""
        path = os.path.join(self.pdfs, name + ".pdf")
        with open(path, "wb") as f:
            f.write(data)
        self.entries.append({
            "name": name, "file": f"pdfs/{name}.pdf", "family": family,
            "producer": producer, "about": about, "expect": expect,
            "pages": pages, "bytes": len(data),
            "questions": [{"q": q[0], "a": q[1], **({"needs": q[2]} if len(q) > 2 else {})}
                          for q in questions],
            **({"notes": notes} if notes else {}),
        })
        print(f"  {name}.pdf  {len(data)} bytes", flush=True)

    def skip(self, what, why):
        self.skipped.append({"what": what, "why": why})
        print(f"  skipped {what}: {why}", flush=True)


def have(prog):
    return shutil.which(prog) is not None or os.path.exists(prog)


def run(argv, cwd=None, timeout=300, input=None):
    """Run a program, returning its standard output, raising with its
    output and errors if it fails."""
    p = subprocess.run(argv, cwd=cwd, timeout=timeout, input=input,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        tail = (p.stdout + p.stderr).decode("utf-8", "replace")[-2000:]
        raise RuntimeError(f"{argv[0]} failed ({p.returncode}):\n{tail}")
    return p.stdout


# ---------------------------------------------------------------------
# Writing PDFs by hand: objects as bytes, a classic xref table after

def pdfstr(s):
    """A literal string, escaped."""
    if isinstance(s, str):
        s = s.encode("latin-1")
    s = s.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")
    return b"(" + s + b")"


def pdfhex(b):
    return b"<" + b.hex().upper().encode() + b">"


class Doc:
    """A PDF built object by object. Objects are bytes (a dictionary,
    array or number) or (dict, data) for a stream, whose /Length is
    added. Numbers come from new()."""

    def __init__(self, version="1.4"):
        self.version = version
        self.objs = {}
        self.next = 1

    def new(self):
        n = self.next
        self.next += 1
        return n

    def put(self, n, obj):
        self.objs[n] = obj
        return n

    def add(self, obj):
        return self.put(self.new(), obj)

    def stream(self, d, data, filters=()):
        """A stream object's parts, data encoded by filters in order of
        decoding (as /Filter lists them)."""
        for f in reversed(filters):
            data = ENCODERS[f](data)
        if filters:
            fl = b" ".join(b"/" + f.encode() for f in filters)
            d = d + (b" /Filter [" + fl + b"]" if len(filters) > 1 else b" /Filter " + fl)
        return (d, data)

    def body(self, n, obj):
        """An object as written, from 'n 0 obj' through 'endobj'."""
        head = f"{n} 0 obj\n".encode()
        if isinstance(obj, tuple):
            d, data = obj
            return (head + b"<< " + d + b" /Length " + str(len(data)).encode() +
                    b" >>\nstream\n" + data + b"\nendstream\nendobj\n")
        return head + obj + b"\nendobj\n"

    def write(self, root, info=None, extra_trailer=b"", header=None):
        out = bytearray(header if header is not None else
                        f"%PDF-{self.version}\n%\xe2\xe3\xcf\xd3\n".encode("latin-1"))
        offs = {}
        for n in sorted(self.objs):
            offs[n] = len(out)
            out += self.body(n, self.objs[n])
        size = max(self.objs) + 1
        x = len(out)
        out += f"xref\n0 {size}\n0000000000 65535 f \n".encode()
        for n in range(1, size):
            out += (f"{offs[n]:010d} 00000 n \n" if n in offs else
                    "0000000000 65535 f \n").encode()
        tr = f"<< /Size {size} /Root {root} 0 R".encode()
        if info:
            tr += f" /Info {info} 0 R".encode()
        out += b"trailer\n" + tr + extra_trailer + b" >>\nstartxref\n"
        out += f"{x}\n%%EOF\n".encode()
        return bytes(out)


def helvetica(doc, name=b"Helvetica", enc=b"/WinAnsiEncoding"):
    return doc.add(b"<< /Type /Font /Subtype /Type1 /BaseFont /" + name +
                   b" /Encoding " + enc + b" >>")


def simple(pages, fonts=None, version="1.4", rotate=None, annots=None,
           xobjects=None, resources=None, catalog_extra=b"", size=(612, 792)):
    """A document of pages, each a content stream (bytes), with
    Helvetica as /F1 unless fonts maps names to font objects' bytes.
    Returns (Doc, catalog number)."""
    doc = Doc(version)
    root, tree = doc.new(), doc.new()
    fontrefs = {}
    for k, v in (fonts or {b"F1": None}).items():
        fontrefs[k] = helvetica(doc) if v is None else doc.add(v)
    fd = b" ".join(b"/" + k + b" " + str(v).encode() + b" 0 R" for k, v in fontrefs.items())
    res = b"/Font << " + fd + b" >>"
    if xobjects:
        res += b" /XObject << " + b" ".join(
            b"/" + k + b" " + str(v).encode() + b" 0 R" for k, v in xobjects(doc).items()) + b" >>"
    if resources:
        res += b" " + resources
    kids = []
    for i, c in enumerate(pages):
        cs = doc.add(doc.stream(b"", c))
        extra = b""
        if rotate and rotate[i]:
            extra += f" /Rotate {rotate[i]}".encode()
        if annots and annots[i]:
            extra += b" /Annots [" + b" ".join(f"{a} 0 R".encode() for a in annots[i](doc)) + b"]"
        kids.append(doc.add(
            f"<< /Type /Page /Parent {tree} 0 R /MediaBox [0 0 {size[0]} {size[1]}] ".encode() +
            b"/Resources << " + res + b" >> /Contents " + f"{cs} 0 R".encode() + extra + b" >>"))
    doc.put(tree, f"<< /Type /Pages /Kids [{' '.join(f'{k} 0 R' for k in kids)}] "
                  f"/Count {len(kids)} >>".encode())
    doc.put(root, f"<< /Type /Catalog /Pages {tree} 0 R".encode() + catalog_extra + b" >>")
    return doc, root


def text(lines, x=72, y=720, size=12, lead=16, font=b"F1"):
    """A content stream showing lines of text, top down."""
    out = [b"BT /" + font + f" {size} Tf {lead} TL {x} {y} Td".encode()]
    for ln in lines:
        out.append(pdfstr(ln) + b" '")
    out.append(b"ET")
    return b"\n".join(out)


# ---------------------------------------------------------------------
# Filters, as an encoder for each, for streams made or rewritten here

def lzw(data, early=1):
    """LZW as PDF's LZWDecode reads it: 9 to 12 bit codes, MSB first, a
    clear code first, the code width growing one code early by
    default (EarlyChange 1)."""
    out, acc, nbits = bytearray(), 0, 0

    def emit(code, width):
        nonlocal acc, nbits
        acc = acc << width | code
        nbits += width
        while nbits >= 8:
            nbits -= 8
            out.append(acc >> nbits & 0xff)

    table = {bytes([i]): i for i in range(256)}
    nxt, width = 258, 9
    emit(256, width)
    w = b""
    for c in data:
        wc = w + bytes([c])
        if wc in table:
            w = wc
            continue
        emit(table[w], width)
        table[wc] = nxt
        nxt += 1
        if nxt + early > (1 << width):
            if width == 12:
                emit(256, width)
                table = {bytes([i]): i for i in range(256)}
                nxt, width = 258, 9
            else:
                width += 1
        w = bytes([c])
    if w:
        emit(table[w], width)
    emit(257, width)
    if nbits:
        out.append(acc << (8 - nbits) & 0xff)
    return bytes(out)


def runlength(data):
    out, i = bytearray(), 0
    while i < len(data):
        j = i
        while j < len(data) and j - i < 128 and data[j] == data[i]:
            j += 1
        if j - i >= 2:
            out += bytes([257 - (j - i), data[i]])
            i = j
            continue
        j = i
        while j < len(data) and j - i < 128 and not (
                j + 1 < len(data) and data[j] == data[j + 1]):
            j += 1
        j = max(j, i + 1)
        out += bytes([j - i - 1]) + data[i:j]
        i = j
    return bytes(out + b"\x80")


def ascii85(data):
    return base64.a85encode(data) + b"~>"


ENCODERS = {
    "FlateDecode": zlib.compress,
    "LZWDecode": lzw,
    "ASCIIHexDecode": lambda d: d.hex().upper().encode() + b">",
    "ASCII85Decode": ascii85,
    "RunLengthDecode": runlength,
}


# ---------------------------------------------------------------------
# Reading PDFs back, as far as rewriting this corpus's own files needs:
# a file whose objects are all at top level (no object streams), with
# direct /Length values, as qpdf --object-streams=disable writes them

OBJRE = re.compile(rb"(?<![0-9])(\d+) (\d+) obj\b")


def dictend(data, i):
    """The index just past the dictionary or array starting at i."""
    depth, j = 0, i
    while j < len(data):
        c = data[j:j + 2]
        if data[j:j + 1] == b"(":
            # A literal string: skip it, with its escapes and nesting
            k, nest = j + 1, 1
            while k < len(data) and nest:
                if data[k:k + 1] == b"\\":
                    k += 2
                    continue
                nest += {b"(": 1, b")": -1}.get(data[k:k + 1], 0)
                k += 1
            j = k
            continue
        if c == b"<<":
            depth += 1
            j += 2
        elif c == b">>":
            depth -= 1
            j += 2
            if depth == 0:
                return j
        elif data[j:j + 1] == b"[" and depth == 0:
            k = j
            while data[k:k + 1] != b"]":
                k += 1
            return k + 1
        else:
            j += 1
    raise ValueError("unterminated dictionary")


def parse(data):
    """The objects of a file, {n: obj} as Doc takes them, and its
    trailer dictionary's text."""
    objs = {}
    for m in OBJRE.finditer(data):
        n = int(m.group(1))
        i = m.end()
        while data[i:i + 1] in b" \r\n\t":
            i += 1
        if data[i:i + 2] == b"<<":
            e = dictend(data, i)
            d = data[i:e]
            k = e
            while data[k:k + 1] in b" \r\n\t":
                k += 1
            if data[k:k + 6] == b"stream":
                k += 6
                k += data[k:k + 1] == b"\r"
                k += data[k:k + 1] == b"\n"
                ln = re.search(rb"/Length (\d+)(?! \d+ R)", d)
                if not ln:
                    raise ValueError(f"object {n}: no direct /Length")
                body = data[k:k + int(ln.group(1))]
                inner = re.sub(rb"/Length \d+", b"", d[2:-2]).strip()
                objs[n] = (inner, body)
                continue
            objs[n] = d
        else:
            e = data.index(b"endobj", i)
            objs[n] = data[i:e].strip()
    tr = data.rfind(b"trailer")
    trailer = data[tr + 7:dictend(data, data.index(b"<<", tr))] if tr >= 0 else b""
    return objs, trailer


def refile(objs, trailer, header=b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"):
    """Objects and a trailer written anew with a classic xref table."""
    doc = Doc()
    doc.objs = dict(objs)
    root = int(re.search(rb"/Root (\d+)", trailer).group(1))
    info = re.search(rb"/Info (\d+)", trailer)
    return doc.write(root, int(info.group(1)) if info else None, header=header)


def decoded(objs):
    """Objects with every stream's filters undone that this script can
    undo (Flate, as qpdf leaves it), others left as they are."""
    out = {}
    for n, o in objs.items():
        if isinstance(o, tuple) and re.search(rb"/Filter\s*/FlateDecode", o[0]) \
                and b"/DecodeParms" not in o[0]:
            d = re.sub(rb"/Filter\s*/FlateDecode", b"", o[0]).strip()
            out[n] = (d, zlib.decompress(o[1]))
        else:
            out[n] = o
    return out


def classic(src, dst):
    """src rewritten by qpdf with no object streams and every stream
    uncompressed where qpdf can, as parse() reads it."""
    run(["qpdf", "--object-streams=disable", "--stream-data=uncompress",
         "--decode-level=generalized", "--compress-streams=n", src, dst])
    with open(dst, "rb") as f:
        return f.read()


# ---------------------------------------------------------------------
# Producers: documents whose facts sit in particular places

def num(n):
    """A regex for a number as a model may write it: 1234 or 1,234."""
    s = str(n)
    return f"(^|[^0-9]){s[0]},?{s[1:]}([^0-9]|$)"


LIGATURES = ["affluent", "baffled", "efficient", "officer", "raffle",
             "shuffle", "waffle", "fluffy", "scaffold", "difficult"]


def ligword(name):
    r = random.Random(hashlib.sha256(b"lig" + name.encode()).digest())
    return r.choice(LIGATURES)


def article_tex(c, name, fontenc=None):
    """The base article: three pages, a fact in the body, in a table in
    the second of two columns, in a footnote, in a word with ligatures,
    and on the last page."""
    (w0, n0), (w1, n1), (w2, n2), (w3, n3) = c.facts(name)
    lig = ligword(name)
    enc = f"\\usepackage[{fontenc}]{{fontenc}}\n" if fontenc else ""
    tex = rf"""\documentclass[11pt,letterpaper]{{article}}
{enc}\usepackage[letterpaper,pass]{{geometry}}  % the PDF too, A4 or not by default
\usepackage{{multicol}}
\usepackage{{booktabs}}
\pagestyle{{plain}}
\begin{{document}}
\section*{{Survey of the northern coast}}
The lighthouse keeper's name is {w0.capitalize()}, and she has kept the light
for {n0} nights. The survey began in spring and ended before the first
storms, following the shore from the estuary to the cliffs, and it
recorded every beacon, jetty and mooring on the way.

The surveyors agreed that the station's manner of keeping records was
remarkably {lig}, and the secret adjective is {lig}. Office staff
filed the affidavits in the final week, after the difficult crossing.
\newpage
\begin{{multicols}}{{2}}
\section*{{Reservoirs}}
The first column describes the hills above the coast, where rain
gathers in a chain of reservoirs fed by streams from the moor. Their
levels are measured weekly, and the measurements go to the town.
The keepers walk the dams at dawn, noting cracks and seepage, and
report anything unusual to the engineer, who lives in the valley.
They also clear the spillways of leaves in autumn and of ice in
winter, so that the water can pass freely in a storm.
\columnbreak

The second column holds the table of capacities.

\begin{{tabular}}{{lr}}
\toprule
Reservoir & Capacity \\
\midrule
Upper Moor & 2210 \\
{w1.capitalize()} & {n1} \\
Low Field & 1875 \\
\bottomrule
\end{{tabular}}

Capacities are in megalitres.
\end{{multicols}}
\newpage
\section*{{Notes}}
The harbour master keeps a garden.\footnote{{In it grows a rare
{w2} that flowers once every {n2} days.}} The garden is open on Sundays.

On this last page, the closing code word is {w3} and the closing
number is {n3}.
\end{{document}}
"""
    qs = [
        ("What is the lighthouse keeper's name, and for how many nights has "
         "she kept the light?", [w0, num(n0)]),
        (f"What is the capacity of the {w1.capitalize()} reservoir?", [num(n1)]),
        ("According to the footnote, what grows in the harbour master's "
         "garden, and how often does it flower?", [w2, num(n2)]),
        ("What is the secret adjective?", [lig]),
        ("What are the closing code word and the closing number on the "
         "last page?", [w3, num(n3)]),
    ]
    return tex, qs


def tex(c, engine, name, source):
    d = os.path.join(c.work, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "doc.tex"), "w") as f:
        f.write(source)
    run([engine, "-interaction=nonstopmode", "-halt-on-error", "doc.tex"], cwd=d)
    if "multicols" in source or "footnote" in source:
        run([engine, "-interaction=nonstopmode", "-halt-on-error", "doc.tex"], cwd=d)
    with open(os.path.join(d, "doc.pdf"), "rb") as f:
        return f.read()


def producers(c):
    """Real producers' output, kept in c.base for the variants."""
    base = {}
    if have("pdflatex"):
        if c.wanted("tex-article"):
            src, qs = article_tex(c, "tex-article")
            data = tex(c, "pdflatex", "tex-article", src)
            c.add("tex-article", data, "producer", "pdfLaTeX article: Type 1 "
                  "fonts, ligatures, two columns, a table, a footnote", pages=3,
                  questions=qs, producer="pdflatex")
            base["article"] = (data, qs)
        if c.wanted("tex-type3"):
            src, qs = article_tex(c, "tex-type3", fontenc="T1")
            data = tex(c, "pdflatex", "tex-type3", src)
            c.add("tex-type3", data, "producer", "pdfLaTeX with T1 encoding and "
                  "no Type 1 EC fonts: Type 3 bitmap fonts (PK)", pages=3,
                  questions=qs, producer="pdflatex")
    else:
        c.skip("pdflatex documents", "pdflatex not found")
    if have("lualatex") and c.wanted("lualatex-article"):
        src, qs = article_tex(c, "lualatex-article")
        src = src.replace(r"\documentclass[11pt,letterpaper]{article}",
                          "\\documentclass[11pt,letterpaper]{article}\n\\usepackage{fontspec}")
        data = tex(c, "lualatex", "lualatex-article", src)
        c.add("lualatex-article", data, "producer", "LuaLaTeX article: OpenType "
              "Latin Modern as CID fonts (Identity-H) with ToUnicode", pages=3,
              questions=qs, producer="lualatex")
    if have("xelatex") and c.wanted("xelatex-scripts"):
        unicode_tex(c)
    if os.path.exists(CHROME):
        chrome(c)
    else:
        c.skip("Chrome documents", "Google Chrome not found")
    if sys.platform == "darwin" and have("cupsfilter") and c.wanted("quartz-text"):
        quartz(c)  # CUPS elsewhere has cupsfilter too, but not Quartz
    if have("gs"):
        postscript(c)
        if "article" in base:
            scans(c, base["article"][0], base["article"][1])
    else:
        c.skip("Ghostscript documents", "gs not found")
    return base


def unicode_tex(c):
    name = "xelatex-scripts"
    (w0, n0), (w1, n1), (w2, n2), (w3, n3) = c.facts(name)
    src = rf"""\documentclass[12pt,letterpaper]{{article}}
\usepackage[letterpaper,pass]{{geometry}}
\usepackage{{fontspec}}
\setmainfont{{Times New Roman}}
\newfontfamily\jp{{Hiragino Mincho ProN}}
\newfontfamily\he[Script=Hebrew]{{Arial Hebrew}}
\newfontfamily\ar[Script=Arabic]{{Times New Roman}}
\TeXXeTstate=1
\begin{{document}}
\section*{{Many scripts}}
Japanese: {{\jp 会議室の番号は{n0}です。}}

Russian: Пароль для склада — {n1}.

Greek: Ο κωδικός της αποθήκης είναι {w0}.

Hebrew: {{\he\beginR הקוד הוא \beginL{n2}\endL\endR}}

Arabic: {{\ar\beginR الرمز هو \beginL{n3}\endL\endR}}

German: Die Straße heißt Größenweg, Hausnummer {w1}.
\end{{document}}
"""
    try:
        data = tex(c, "xelatex", name, src)
    except RuntimeError as e:
        if "cannot be found" not in str(e):
            raise
        # Its fonts are macOS's own (Times New Roman, Hiragino, Arial Hebrew)
        c.skip(name, "a font it uses is not installed")
        return
    qs = [("What room number does the Japanese sentence give?", [num(n0)]),
          ("What number does the Russian sentence give as the warehouse password?",
           [num(n1)]),
          ("What code does the Greek sentence give?", [w0]),
          ("What number does the Hebrew sentence give?", [num(n2)]),
          ("What number does the Arabic sentence give?", [num(n3)]),
          ("What street name and house 'number' does the German sentence give?",
           ["gr(ö|oe)(ß|ss)enweg", w1])]
    c.add(name, data, "producer", "XeLaTeX: system OpenType fonts as CID "
          "fonts, Japanese, Cyrillic, Greek, Hebrew and Arabic (right to left)",
          pages=1, questions=qs, producer="xelatex")


def chrome(c):
    for name, html, qs, about in chrome_docs(c):
        if not c.wanted(name):
            continue
        d = os.path.join(c.work, name)
        os.makedirs(d, exist_ok=True)
        src = os.path.join(d, "doc.html")
        with open(src, "w") as f:
            f.write(html)
        out = os.path.join(d, "doc.pdf")
        run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
             f"--user-data-dir={os.path.join(d, 'profile')}",
             f"--print-to-pdf={out}", "file://" + src], timeout=120)
        with open(out, "rb") as f:
            c.add(name, f.read(), "producer", about, questions=qs, producer="chrome")


def chrome_docs(c):
    name = "chrome-page"
    (w0, n0), (w1, n1), (w2, n2), (w3, n3) = c.facts(name)
    filler = " ".join(["The archive keeps letters, maps and ledgers from the "
                       "harbour's early years, sorted by decade."] * 30)
    html = f"""<!doctype html><meta charset="utf-8">
<style>
body {{ font-family: Helvetica, Arial, sans-serif; font-size: 12pt; }}
.cols {{ column-count: 3; column-gap: 2em; }}
.spaced {{ letter-spacing: 0.4em; }}
.caps {{ text-transform: uppercase; }}
table {{ border-collapse: collapse; }} td, th {{ border: 1px solid #888; padding: 4px; }}
</style>
<h1>Harbour archive</h1>
<p class="cols">{filler} In the third column, the archivist's cat is
called {w0} and is {n0} days old. {filler}</p>
<h2>Shelves</h2>
<table><tr><th>Shelf</th><th>Boxes</th><th>Keeper</th></tr>
<tr><td>A</td><td>12</td><td>Morrow</td></tr>
<tr><td>B</td><td>{n1}</td><td>{w1.capitalize()}</td></tr>
<tr><td>C</td><td>7</td><td>Hale</td></tr></table>
<p>The fox emoji count: 🦊 × {n2}.</p>
<p class="spaced">Spaced out, the word is {w2}.</p>
<p class="caps">shouted, the word is {w3}.</p>
<p style="break-before: page">On the last page, the number is {n3}.</p>
"""
    yield name, html, [
        ("What is the archivist's cat called, and how old is it?", [w0, num(n0)]),
        ("How many boxes does shelf B hold, and who keeps it?", [num(n1), w1]),
        ("What count follows the fox emoji?", [num(n2)]),
        ("What word is written spaced out?", [w2]),
        ("What word is shouted?", [w3]),
        ("What number is on the last page?", [num(n3)]),
    ], "Chrome (Skia): subset TrueType as CID fonts, CSS columns, a table, " \
       "emoji, letter spacing, upper case by CSS"
    name = "chrome-vertical"
    (w0, n0), (w1, n1) = c.facts(name, 2)
    html = f"""<!doctype html><meta charset="utf-8">
<style>body {{ font-family: "Hiragino Mincho ProN", serif; font-size: 16pt; }}
.v {{ writing-mode: vertical-rl; height: 20em; }}</style>
<div class="v">縦書きの文章です。倉庫の番号は{n0}番です。合言葉は「{w0}」です。</div>
"""
    yield name, html, [
        ("What warehouse number does the vertical Japanese text give?", [num(n0)]),
        ("What password does it give?", [w0]),
    ], "Chrome: vertical Japanese (writing-mode: vertical-rl)"


def quartz(c):
    name = "quartz-text"
    (w0, n0), (w1, n1) = c.facts(name, 2)
    d = os.path.join(c.work, name)
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "doc.txt")
    lines = [f"Inventory note {i:02d}: nothing unusual on shelf {i}." for i in range(1, 80)]
    lines[10] = f"Inventory note 11: the missing crate is labelled {w0}."
    lines[70] = f"Inventory note 71: it weighs {n0} grams."
    with open(src, "w") as f:
        f.write("\n".join(lines) + "\n")
    data = run(["cupsfilter", "-m", "application/pdf", src])
    c.add(name, data, "producer", "macOS Quartz (cupsfilter) from plain text: "
          "a monospaced listing over two pages", questions=[
              ("What is the missing crate labelled, and what does it weigh?",
               [w0, num(n0)])], producer="quartz")


def gs(*args, timeout=300):
    return run(["gs", "-q", "-dNOPAUSE", "-dBATCH", "-dSAFER", *args], timeout=timeout)


def postscript(c):
    """Ghostscript's pdfwrite from PostScript written here: glyphs placed
    one by one (no spaces in the text), a reencoded font with accents,
    rotated text, and a Type 3 font defined in PostScript."""
    name = "gs-postscript"
    if not c.wanted(name):
        return
    (w0, n0), (w1, n1), (w2, n2), (w3, n3) = c.facts(name)

    def placed(s, x, y):
        out = []
        for ch in s:
            if ch != " ":
                out.append(f"{x:.1f} {y} moveto ({ch}) show")
            x += 7.5
        return "\n".join(out)

    ps = f"""%!PS
/Helvetica findfont dup length dict begin
  {{1 index /FID ne {{def}} {{pop pop}} ifelse}} forall
  /Encoding ISOLatin1Encoding def
  currentdict end /HelvLatin1 exch definefont pop
/HelvLatin1 findfont 12 scalefont setfont
72 720 moveto (Caf\\351 \\334berlingen, \\305ngstr\\366m: the word is {w0}.) show
/Courier findfont 12 scalefont setfont
{placed(f"Placed one by one, the number is {n0} and the word is {w1}.", 72, 690)}
gsave 300 400 translate 90 rotate
/Helvetica findfont 14 scalefont setfont
0 0 moveto (Turned on its side, the number is {n1}.) show grestore
/Boxes 8 dict def Boxes begin
  /FontType 3 def /FontMatrix [0.001 0 0 0.001 0 0] def /FontBBox [0 0 600 700] def
  /Encoding 256 array def 0 1 255 {{Encoding exch /.notdef put}} for
  Encoding 65 /A put Encoding 66 /B put Encoding 67 /C put
  /CharProcs 4 dict def CharProcs begin
    /.notdef {{}} def
    /A {{0 0 moveto 300 700 lineto 600 0 lineto closepath fill}} def
    /B {{0 0 600 700 rectfill}} def
    /C {{300 350 300 0 360 arc fill}} def
  end
  /BuildGlyph {{1000 0 0 0 600 700 setcachedevice exch /CharProcs get exch
    2 copy known not {{pop /.notdef}} if get exec}} def
  /BuildChar {{1 index /Encoding get exch get 1 index /BuildGlyph get exec}} def
end
/BoxFont Boxes definefont pop
/BoxFont findfont 24 scalefont setfont
72 200 moveto (ABCBA) show
/Helvetica findfont 12 scalefont setfont
72 170 moveto (Above, five shapes drawn in a Type 3 font. The last number is {n2}.) show
showpage
"""
    d = os.path.join(c.work, name)
    os.makedirs(d, exist_ok=True)
    src, out = os.path.join(d, "doc.ps"), os.path.join(d, "doc.pdf")
    with open(src, "w", encoding="latin-1") as f:
        f.write(ps)
    gs("-sDEVICE=pdfwrite", f"-sOutputFile={out}", src)
    with open(out, "rb") as f:
        c.add(name, f.read(), "producer", "Ghostscript pdfwrite from PostScript: "
              "an ISO Latin-1 reencoded font, glyphs placed one by one with no "
              "spaces, rotated text, a Type 3 font", pages=1, producer="gs",
              questions=[
                  ("What word ends the sentence with accented words?", [w0]),
                  ("What number and word are given in the line placed one glyph "
                   "at a time?", [num(n0), w1]),
                  ("What number is given in the text turned on its side?", [num(n1)]),
                  ("What is the last number?", [num(n2)])])


def scans(c, article, qs):
    """The article as a scan: page images only, and with an OCR layer of
    invisible text (tesseract through pdfocr8), and in black and white
    (CCITT G4, by Ghostscript's pdfwrite)."""
    d = os.path.join(c.work, "scans")
    os.makedirs(d, exist_ok=True)
    pics = [(q[0], q[1], "image") for q in qs]  # only the pixels hold it
    src = os.path.join(d, "article.pdf")
    with open(src, "wb") as f:
        f.write(article)
    if c.wanted("scan-image"):
        out = os.path.join(d, "image.pdf")
        gs("-sDEVICE=pdfimage24", "-r150", f"-sOutputFile={out}", src)
        with open(out, "rb") as f:
            c.add("scan-image", f.read(), "scan", "The article as page images "
                  "only (Flate RGB, 150 dpi): no text at all", expect="images", pages=3,
                  questions=pics, producer="gs pdfimage24")
    if c.wanted("scan-ocr"):
        out = os.path.join(d, "ocr.pdf")
        try:
            gs("-sDEVICE=pdfocr8", "-r300", "-sOCRLanguage=eng",
               f"-sOutputFile={out}", src, timeout=900)
            with open(out, "rb") as f:
                c.add("scan-ocr", f.read(), "scan", "The article as page images "
                      "with tesseract's invisible text over them (render mode 3)",
                      pages=3, questions=qs, producer="gs pdfocr8")
        except RuntimeError as e:
            c.skip("scan-ocr", str(e).splitlines()[0])
    if c.wanted("scan-ccitt") or c.wanted("scan-ccitt-g3"):
        pbm = os.path.join(d, "page.pbm")
        gs("-sDEVICE=pbmraw", "-r200", "-dFirstPage=1", "-dLastPage=1",
           f"-sOutputFile={pbm}", src)
        w, h, bits = readpnm(pbm)
        doc, root = simple([f"q {w * 72 / 200:.2f} 0 0 {h * 72 / 200:.2f} 0 0 cm "
                            "/Im0 Do Q".encode()],
                           xobjects=lambda dd: {b"Im0": dd.add(dd.stream(
                               f"/Type /XObject /Subtype /Image /Width {w} /Height {h} "
                               "/ColorSpace /DeviceGray /BitsPerComponent 1 "
                               "/Decode [1 0]".encode(), bits, ["FlateDecode"]))})
        tmp = os.path.join(d, "mono.pdf")
        with open(tmp, "wb") as f:
            f.write(doc.write(root))
        if c.wanted("scan-ccitt"):
            out = os.path.join(d, "ccitt.pdf")
            gs("-sDEVICE=pdfwrite", "-dEncodeMonoImages=true",
               "-dMonoImageFilter=/CCITTFaxEncode", "-dAutoFilterMonoImages=false",
               f"-sOutputFile={out}", tmp)
            with open(out, "rb") as f:
                c.add("scan-ccitt", f.read(), "scan", "The article's first page as a "
                      "black and white scan, CCITT G4 encoded: no text", expect="images",
                      pages=1, questions=pics[:1] + pics[3:4], producer="gs pdfwrite")
        if c.wanted("scan-ccitt-g3"):
            # The same, G3 1-D (K 0) by the distiller parameter
            out = os.path.join(d, "ccitt-g3.pdf")
            gs("-sDEVICE=pdfwrite", "-dEncodeMonoImages=true",
               "-dMonoImageFilter=/CCITTFaxEncode", "-dAutoFilterMonoImages=false",
               f"-sOutputFile={out}", "-c",
               "<< /MonoImageDict << /K 0 >> >> setdistillerparams", "-f", tmp)
            with open(out, "rb") as f:
                data = f.read()
            if b"/K 0" not in data and b"/K -1" in data:
                raise RuntimeError("gs wrote G4 for scan-ccitt-g3, not G3")
            c.add("scan-ccitt-g3", data, "scan", "The article's first page as a "
                  "black and white scan, CCITT G3 1-D encoded (K 0): no text",
                  expect="images", pages=1, questions=pics[:1] + pics[3:4],
                  producer="gs pdfwrite")


def readpnm(path):
    """A raw PNM (P4, P5 or P6): width, height and the pixel bytes."""
    with open(path, "rb") as f:
        data = f.read()
    toks, i = [], 0
    while len(toks) < (3 if data[:2] == b"P4" else 4):
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            i = data.index(b"\n", i)
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        toks.append(data[i:j])
        i = j
    return int(toks[1]), int(toks[2]), data[i + 1:]


# ---------------------------------------------------------------------
# Variants of the article: the same content, written differently

def variants(c, article, qs):
    d = os.path.join(c.work, "variants")
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "article.pdf")
    with open(src, "wb") as f:
        f.write(article)

    def qpdf(name, about, *args):
        if not c.wanted(name):
            return
        out = os.path.join(d, name + ".pdf")
        run(["qpdf", *args, src, out])
        with open(out, "rb") as f:
            c.add(name, f.read(), "structure", about, pages=3, questions=qs,
                  producer="qpdf")

    if have("qpdf"):
        qpdf("qpdf-objstm", "Object streams and a cross-reference stream with "
             "PNG predictors (PDF 1.5)", "--object-streams=generate")
        qpdf("qpdf-qdf", "QDF: uncompressed, indirect /Length objects, "
             "comments between objects", "--qdf", "--object-streams=disable")
        qpdf("qpdf-linearized", "Linearized (fast web view): hint streams, two "
             "cross-reference sections", "--linearize")
        qpdf("qpdf-uncompressed", "Every stream uncompressed, classic "
             "cross-reference table", "--object-streams=disable",
             "--stream-data=uncompress", "--compress-streams=n")
        try:
            plain = classic(src, os.path.join(d, "classic.pdf"))
        except RuntimeError as e:
            c.skip("filter variants", str(e).splitlines()[0])
            plain = None
    else:
        c.skip("qpdf variants", "qpdf not found")
        plain = None
    if have("gs") and c.wanted("gs-rewritten"):
        out = os.path.join(d, "gs.pdf")
        gs("-sDEVICE=pdfwrite", f"-sOutputFile={out}", src)
        with open(out, "rb") as f:
            c.add("gs-rewritten", f.read(), "structure", "The article through "
                  "Ghostscript's pdfwrite: its own font subsets and structure",
                  pages=3, questions=qs, producer="gs")
    if have("pdftk") and c.wanted("pdftk-compressed"):
        out = os.path.join(d, "pdftk.pdf")
        run(["pdftk", src, "output", out, "compress"])
        with open(out, "rb") as f:
            c.add("pdftk-compressed", f.read(), "structure", "The article through "
                  "pdftk (iText)", pages=3, questions=qs, producer="pdftk")
    if plain is None:
        return
    objs, trailer = parse(plain)
    filters(c, objs, trailer, qs)
    incremental(c, plain, objs, trailer, qs)
    damage(c, plain, objs, trailer, qs)


def filters(c, objs, trailer, qs):
    """Every stream of the article encoded with a filter qpdf does not
    write, or a chain of them."""
    cases = [
        ("filter-lzw", ["LZWDecode"], "LZW, the default early change"),
        ("filter-lzw-early0", ["LZWDecode"], "LZW with /EarlyChange 0"),
        ("filter-asciihex", ["ASCIIHexDecode"], "ASCIIHex"),
        ("filter-ascii85", ["ASCII85Decode"], "ASCII85"),
        ("filter-runlength", ["RunLengthDecode"], "RunLength"),
        ("filter-chain", ["ASCII85Decode", "FlateDecode"], "a chain, ASCII85 "
         "over Flate"),
        ("filter-chain-lzw", ["ASCIIHexDecode", "LZWDecode"], "a chain, "
         "ASCIIHex over LZW"),
    ]
    for name, fl, about in cases:
        if not c.wanted(name):
            continue
        out = {}
        early0 = name.endswith("early0")
        for n, o in objs.items():
            if isinstance(o, tuple) and b"/Filter" not in o[0]:
                data = o[1]
                for f in reversed(fl):
                    data = lzw(data, 0) if early0 and f == "LZWDecode" else ENCODERS[f](data)
                names = [b"/" + f.encode() for f in fl]
                fd = names[0] if len(names) == 1 else b"[" + b" ".join(names) + b"]"
                d = o[0] + b" /Filter " + fd
                if early0:
                    d += b" /DecodeParms << /EarlyChange 0 >>"
                out[n] = (d, data)
            else:
                out[n] = o
        c.add(name, refile(out, trailer), "filter", f"Every stream in {about}",
              pages=3, questions=qs, producer="pdfcorpus")


def incremental(c, plain, objs, trailer, qs):
    """An update appended to the article, as an editor saves one: page
    1's text replaced, the old revision still in the file before it."""
    name = "incremental-update"
    if not c.wanted(name):
        return
    (w, n), = c.facts(name, 1)
    first = re.search(rb"/Kids \[ ?(\d+) 0 R", plain)
    page = objs[int(first.group(1))]
    cs = int(re.search(rb"/Contents (\d+) 0 R", page).group(1))
    size = int(re.search(rb"/Size (\d+)", trailer).group(1))
    fnt = size
    newcs = size + 1
    parent = re.search(rb"/Parent (\d+) 0 R", page).group(1).decode()
    box = re.search(rb"/MediaBox \[[^\]]*\]", page).group(0).decode()
    newpage = (f"<< /Type /Page /Parent {parent} 0 R {box} /Resources << /Font << "
               f"/FX {fnt} 0 R >> >> /Contents {newcs} 0 R >>").encode()
    upd = bytearray(b"\n")
    offs = {}
    doc = Doc()
    for k, o in [(int(first.group(1)), newpage),
                 (fnt, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                       b"/Encoding /WinAnsiEncoding >>"),
                 (newcs, doc.stream(b"", text([
                     "This page was revised after the survey.",
                     f"The revised keeper's name is {w.capitalize()}, and the "
                     f"revised count is {n} nights."], font=b"FX")))]:
        offs[k] = len(plain) + len(upd)
        upd += doc.body(k, o)
    prev = int(re.search(rb"startxref\s+(\d+)", plain[plain.rfind(b"startxref"):]).group(1))
    x = len(plain) + len(upd)
    upd += b"xref\n0 1\n0000000000 65535 f \n"
    for k in sorted(offs):
        upd += f"{k} 1\n{offs[k]:010d} 00000 n \n".encode()
    root = re.search(rb"/Root (\d+ \d+ R)", trailer).group(1)
    upd += (f"trailer\n<< /Size {newcs + 1} /Root ".encode() + root +
            f" /Prev {prev} >>\nstartxref\n{x}\n%%EOF\n".encode())
    old = qs[0][1]
    c.add(name, plain + bytes(upd), "structure", "An incremental update: page 1 "
          "replaced by a new revision appended with its own cross-reference "
          "section (/Prev); the old page's objects are still in the file",
          pages=3, producer="pdfcorpus", questions=[
              ("What is the revised keeper's name, and the revised count of nights?",
               [w, num(n)] + ["!" + a for a in old]),
          ] + qs[1:3] + qs[4:], notes="The old revision's answers must not be "
          "given. The secret adjective was on the replaced page, so it is not asked.")


def damage(c, plain, objs, trailer, qs):
    """The article damaged in the ways real files are."""
    xref = plain.rfind(b"\nxref") + 1
    sx = plain.rfind(b"startxref")

    def keep(name, data, about, expect="partial", questions=qs):
        if c.wanted(name):
            c.add(name, data, "damage", about, expect=expect, pages=3,
                  questions=questions, producer="pdfcorpus")

    # Every offset off by the bytes inserted after the header
    hdr = plain.index(b"\n", plain.index(b"%PDF")) + 1
    keep("xref-offsets-shifted", plain[:hdr] + b"% shifted\n" + plain[hdr:],
         "Ten bytes inserted after the header, so every xref offset is off: "
         "objects are found by scanning", expect="text")
    keep("xref-missing", plain[:xref], "No cross-reference table, trailer or "
         "startxref: the objects and the catalog must be found by scanning",
         expect="text")
    keep("startxref-wrong", plain[:sx] + b"startxref\n999999999\n%%EOF\n",
         "startxref points past the end of the file", expect="text")
    keep("startxref-zero", plain[:sx] + b"startxref\n0\n%%EOF\n",
         "startxref is 0, at the header", expect="text")
    lf = re.sub(rb"(\d{10} \d{5} [nf]) \n", rb"\1\n", plain[xref:sx])
    keep("xref-entries-19byte", plain[:xref] + lf + plain[sx:],
         "Cross-reference entries of 19 bytes (a bare LF), against the spec's "
         "20", expect="text")
    keep("junk-before-header", b"\x00" * 3 + b"Junk from a mail gateway.\r\n" * 30 +
         plain, "800-odd bytes of junk before %PDF-, which offsets do not count",
         expect="text")
    keep("truncated", plain[:len(plain) * 6 // 10], "Cut off at 60%: no xref, "
         "the last objects lost; what remains should be read")
    keep("trailing-garbage", plain + b"\x1a" + os.urandom(512),
         "Random bytes after %%EOF", expect="text")

    # Stream lengths that lie, against endstream
    def lengths(delta):
        body = refile(objs, trailer)
        return re.sub(rb"/Length (\d+)",
                      lambda m: f"/Length {max(0, int(m.group(1)) + delta)}".encode(),
                      body)
    keep("length-short", lengths(-12), "Every /Length 12 bytes too short (the "
         "xref stays right): stream data runs to endstream", expect="text")
    keep("length-long", lengths(40), "Every /Length 40 bytes too long, past "
         "endstream", expect="text")

    # Lies in the structure
    tree = re.search(rb"/Pages (\d+) 0 R", objs[int(re.search(rb"/Root (\d+)", trailer)
                                                     .group(1))]).group(1)
    lied = dict(objs)
    lied[int(tree)] = re.sub(rb"/Count \d+", b"/Count 99", objs[int(tree)])
    keep("count-lies", refile(lied, trailer), "The page tree's /Count says 99 "
         "of its 3 pages", expect="text")
    # An object renumbered in place, its digits as many as before, so
    # that no offset moves: the xref entry finds another number there
    swapped = refile(objs, trailer)
    m = list(OBJRE.finditer(swapped))[2]
    other = "9" * len(m.group(1))
    swapped = swapped[:m.start(1)] + other.encode() + swapped[m.end(1):]
    keep("objnum-mismatch", swapped, "An xref entry points at an object "
         f"numbered differently ('{other} 0 obj')", expect="text")
    dup = refile(objs, trailer)
    dx = dup.rfind(b"\nxref") + 1
    extra = Doc().body(int(tree), b"<< /Type /Pages /Kids [] /Count 0 >>")
    dup = dup[:dx] + extra + dup[dx:]
    dup = re.sub(rb"startxref\n\d+", f"startxref\n{dx + len(extra)}".encode(), dup)
    keep("duplicate-object", dup, "A second, empty copy of the page tree's "
         "object after the others, which the xref does not point to",
         expect="text")
    loop = plain[:sx].replace(b"trailer\n<<", f"trailer\n<< /Prev {xref}".encode(), 1)
    keep("prev-loop", loop + plain[sx:], "The trailer's /Prev points at its "
         "own cross-reference section", expect="text")


# ---------------------------------------------------------------------
# Oddities of text, each in a small file of its own

def tounicode(pairs, ranges=()):
    """A ToUnicode CMap: pairs of (code, text) for two-byte codes, and
    (lo, hi, [text, ...]) ranges in the array form."""
    def u16(s):
        return s.encode("utf-16-be").hex().upper()
    out = ["/CIDInit /ProcSet findresource begin 12 dict begin begincmap",
           "/CMapName /Corpus-UCS def /CMapType 2 def",
           "1 begincodespacerange <0000> <FFFF> endcodespacerange",
           f"{len(pairs)} beginbfchar"]
    out += [f"<{code:04X}> <{u16(t)}>" for code, t in pairs]
    out.append("endbfchar")
    if ranges:
        out.append(f"{len(ranges)} beginbfrange")
        for lo, hi, ts in ranges:
            out.append(f"<{lo:04X}> <{hi:04X}> [" + " ".join(f"<{u16(t)}>" for t in ts) + "]")
        out.append("endbfrange")
    out += ["endcmap CMapName currentdict /CMap defineresource pop end end"]
    return "\n".join(out).encode()


def oddities(c):
    def doc(name, about, pages, questions, expect="text", notes=None, **kw):
        if not c.wanted(name):
            return
        d, root = simple(pages, **kw)
        c.add(name, d.write(root), "text", about, expect=expect, pages=len(pages),
              questions=questions, producer="pdfcorpus", notes=notes)

    def facts(name, n=2):
        return c.facts(name, n)

    (w0, n0), (w1, n1) = facts("rotated")
    doc("rotated", "A page with /Rotate 90, and text turned by its text matrix "
        "on another", [
            text([f"This page is rotated a quarter turn; its word is {w0}."]),
            b"BT /F1 14 Tf 0 1 -1 0 300 100 Tm " +
            pdfstr(f"Written upward, the number is {n1}.") + b" Tj ET"],
        [("What word is on the rotated page?", [w0]),
         ("What number is written upward?", [num(n1)])], rotate=[90, 0])

    (w0, n0), = facts("form-xobject", 1)

    def forms(dd):
        f = helvetica(dd)
        inner = dd.add(dd.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                                 f"/Resources << /Font << /F1 {f} 0 R >> >>".encode(),
                                 text([f"Inside the inner form, the word is {w0} and "
                                       f"the number is {n0}."], y=500)))
        outer = dd.add(dd.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                                 f"/Resources << /XObject << /Fm1 {inner} 0 R >> >>".encode(),
                                 b"q 1 0 0 1 0 -50 cm /Fm1 Do Q"))
        return {b"Fm0": outer}
    doc("form-xobject", "Text drawn only inside a Form XObject inside another",
        [text(["The page itself says little."]) + b"\nq /Fm0 Do Q"],
        [("What word and number are given inside the form?", [w0, num(n0)])],
        xobjects=forms)

    (w0, n0), = facts("fake-bold", 1)
    line = f"In bold, the word is {w0} and the number is {n0}."
    doc("fake-bold", "Fake bold: the same line drawn twice, half a point apart",
        [text([line]) + b"\n" + text([line], x=72.5)],
        [("What word and number are given in bold?", [w0, num(n0)])],
        notes="A reader should give the line once, not twice.")

    (w0, n0), = facts("actualtext", 1)
    doc("actualtext", "Marked content with /ActualText over glyphs that say "
        "otherwise (as for a ligature or a drawn symbol)",
        [b"BT /F1 12 Tf 72 700 Td (The code word is ) Tj /Span << /ActualText " +
         pdfstr(f"{w0}") + b" >> BDC (\\267\\267\\267\\267) Tj EMC ( and the number is " +
         str(n0).encode() + b".) Tj ET"],
        [("What is the code word, and the number?", [w0, num(n0)])])

    (w0, n0), = facts("tounicode", 1)
    msg = f"Through ToUnicode: {w0} {n0} 🦊"
    codes = list(range(0x0101, 0x0101 + len(msg)))
    pairs = [(cd, ch) for cd, ch in zip(codes, msg) if not ("a" <= ch <= "z")]
    lows = [(cd, ch) for cd, ch in zip(codes, msg) if "a" <= ch <= "z"]
    ranges = [(cd, cd, [ch]) for cd, ch in lows]

    def t0(dd):
        cmap = dd.add(dd.stream(b"", tounicode(pairs, ranges)))
        cid = dd.add(b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ArialMT "
                     b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
                     b"/Supplement 0 >> /DW 600 >>")
        return (b"<< /Type /Font /Subtype /Type0 /BaseFont /ArialMT /Encoding "
                b"/Identity-H /DescendantFonts [" + f"{cid} 0 R".encode() +
                b"] /ToUnicode " + f"{cmap} 0 R".encode() + b" >>")
    if c.wanted("tounicode"):
        d, root = simple([b""])
        font = d.add(t0(d))
        page = [k for k, v in d.objs.items() if isinstance(v, bytes) and b"/Type /Page " in v][0]
        cs = int(re.search(rb"/Contents (\d+)", d.objs[page]).group(1))
        d.objs[page] = d.objs[page].replace(b"/Font << ", f"/Font << /T0 {font} 0 R ".encode())
        d.objs[cs] = d.stream(b"", b"BT /T0 14 Tf 72 700 Td " +
                              pdfhex(b"".join(struct.pack(">H", x) for x in codes)) +
                              b" Tj ET")
        c.add("tounicode", d.write(root), "text", "A Type 0 font (Identity-H, "
              "not embedded) whose text is only in its ToUnicode CMap: bfchar, "
              "bfrange arrays, and a surrogate pair (an emoji)", pages=1,
              producer="pdfcorpus", questions=[
                  ("What word, number and emoji are given through ToUnicode?",
                   [w0, num(n0), "🦊|fox"])])

    (w0, n0), = facts("differences", 1)
    word = w0
    names = sorted(set(word))
    codes = {ch: 0x80 + i for i, ch in enumerate(names)}
    diff = b"[128 " + b" ".join(b"/" + ch.encode() for ch in names) + \
           b" 200 /Euro /afii10017 /uni2192 /g123]"
    enc = bytes(codes[ch] for ch in word)
    doc("differences", "A simple font with a /Differences encoding: a word in "
        "remapped codes, glyph names Euro, afii10017 (Cyrillic A), uni2192 "
        "(an arrow), and an unknown g123",
        [b"BT /F1 12 Tf 72 700 Td (The remapped word is ) Tj " + pdfstr(enc) +
         b" Tj (. Price: ) Tj (\\310) Tj (" + str(n0).encode() +
         b" ) Tj (\\311 \\312 \\313) Tj ET"],
        [("What is the remapped word, and the price?", [w0, num(n0)])],
        fonts={b"F1": b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                      b"/Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding "
                      b"/Differences " + diff + b" >> >>"})

    (w0, n0), (w1, n1) = facts("word-spacing")
    words = f"Placed word by word, the word is {w0} and the number is {n0}.".split()
    tds, x = [], 72
    for wd in words:
        tds.append(f"1 0 0 1 {x} 700 Tm ".encode() + pdfstr(wd) + b" Tj")
        x += 6.2 * len(wd) + 5
    tj = b"[" + b"-280".join(pdfstr(wd) for wd in
                             f"Kerned apart, the word is {w1} and the number is {n1}."
                             .split()) + b"] TJ"
    doc("word-spacing", "Words with no space characters: placed one by one "
        "with Tm, and set apart by TJ kerning alone",
        [b"BT /F1 11 Tf " + b"\n".join(tds) + b" ET\nBT /F1 11 Tf 72 660 Td " + tj + b" ET"],
        [("What word and number are placed word by word?", [w0, num(n0)]),
         ("What word and number are kerned apart?", [w1, num(n1)])])

    (w0, n0), = facts("columns-interleaved", 1)
    left = ["The left column is about", "the weather on the coast,", "which was mild",
            "through most of", "the season."]
    right = ["The right column holds", f"the vault number, {n0},", "and the guard's",
             f"name, {w0.capitalize()}.", "Nothing else."]
    parts = []
    for i, (lt, rt) in enumerate(zip(left, right)):
        parts.append(f"BT /F1 12 Tf 72 {700 - 16 * i} Td ".encode() + pdfstr(lt) + b" Tj ET")
        parts.append(f"BT /F1 12 Tf 330 {700 - 16 * i} Td ".encode() + pdfstr(rt) + b" Tj ET")
    doc("columns-interleaved", "Two columns drawn line by line across both, "
        "left then right, so content order is not reading order",
        [b"\n".join(parts)],
        [("According to the right column, what is the vault number and the "
          "guard's name?", [num(n0), w0])])

    (w0, n0), (w1, n1) = facts("acroform")
    if c.wanted("acroform"):
        d = Doc()
        root, tree, pg = d.new(), d.new(), d.new()
        f = helvetica(d)
        ap = d.add(d.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 200 20] "
                            f"/Resources << /Font << /F1 {f} 0 R >> >>".encode(),
                            b"/Tx BMC BT /F1 10 Tf 2 6 Td " + pdfstr(w0) + b" Tj ET EMC"))
        fld = d.add(f"<< /Type /Annot /Subtype /Widget /FT /Tx /T (keeper) /TU (Keeper's "
                    f"name) /V ({w0}) /Rect [200 600 400 620] /P {pg} 0 R "
                    f"/AP << /N {ap} 0 R >> /DA (/F1 10 Tf 0 g) >>".encode())
        fld2 = d.add(f"<< /Type /Annot /Subtype /Widget /FT /Tx /T (count) /V ({n0}) "
                     f"/Rect [200 560 400 580] /P {pg} 0 R /DA (/F1 10 Tf 0 g) >>".encode())
        note = d.add(f"<< /Type /Annot /Subtype /Text /Rect [72 400 92 420] "
                     f"/Contents (A sticky note: the inspector is {w1}.) >>".encode())
        free = d.add(f"<< /Type /Annot /Subtype /FreeText /Rect [72 300 400 330] "
                     f"/DA (/F1 12 Tf 0 g) /Contents (Typed on top: {n1}.) >>".encode())
        cs = d.add(d.stream(b"", text(["Keeper's name:", "", "", "Count:"], y=605,
                                      lead=20)))
        d.put(pg, f"<< /Type /Page /Parent {tree} 0 R /MediaBox [0 0 612 792] /Resources "
                  f"<< /Font << /F1 {f} 0 R >> >> /Contents {cs} 0 R /Annots "
                  f"[{fld} 0 R {fld2} 0 R {note} 0 R {free} 0 R] >>".encode())
        d.put(tree, f"<< /Type /Pages /Kids [{pg} 0 R] /Count 1 >>".encode())
        d.put(root, f"<< /Type /Catalog /Pages {tree} 0 R /AcroForm << /Fields "
                    f"[{fld} 0 R {fld2} 0 R] /DA (/F1 10 Tf 0 g) /DR << /Font << "
                    f"/F1 {f} 0 R >> >> >> >>".encode())
        c.add("acroform", d.write(root), "text", "A filled form: two text fields "
              "(one without an appearance stream), a sticky note and a FreeText "
              "annotation, none of it in the page's content", pages=1,
              producer="pdfcorpus", questions=[
                  ("What are the form's keeper's name and count?", [w0, num(n0)]),
                  ("What does the sticky note say the inspector is called?", [w1]),
                  ("What number is typed on top of the page?", [num(n1)])])

    (w0, n0), = facts("type3", 1)
    if c.wanted("type3"):
        word = w0
        glyphs = sorted(set(word))
        d, root = simple([b""])
        procs = {}
        for i, ch in enumerate(glyphs):
            procs[ch] = d.add(d.stream(b"", f"{600} 0 0 0 500 700 d1 0 0 "
                                            f"{100 + 40 * i} 700 re f".encode()))
        cmap_simple = ("/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                       "/CMapName /Corpus-T3 def /CMapType 2 def\n"
                       "1 begincodespacerange <00> <FF> endcodespacerange\n"
                       f"{len(glyphs)} beginbfchar\n" +
                       "".join(f"<{0x41 + i:02X}> <{ch.encode('utf-16-be').hex().upper()}>\n"
                               for i, ch in enumerate(glyphs)) +
                       "endbfchar\nendcmap CMapName currentdict /CMap defineresource "
                       "pop end end").encode()
        cmap = d.add(d.stream(b"", cmap_simple))
        diffs = " ".join(f"/g{i}" for i in range(len(glyphs)))
        cp = " ".join(f"/g{i} {procs[ch]} 0 R" for i, ch in enumerate(glyphs))
        t3 = d.add(f"<< /Type /Font /Subtype /Type3 /FontBBox [0 0 600 700] /FontMatrix "
                   f"[0.001 0 0 0.001 0 0] /FirstChar 65 /LastChar {64 + len(glyphs)} "
                   f"/Widths [{' '.join(['600'] * len(glyphs))}] /Encoding << /Type "
                   f"/Encoding /Differences [65 {diffs}] >> /CharProcs << {cp} >> "
                   f"/ToUnicode {cmap} 0 R >>".encode())
        page = [k for k, v in d.objs.items() if isinstance(v, bytes) and b"/Type /Page " in v][0]
        cs = int(re.search(rb"/Contents (\d+)", d.objs[page]).group(1))
        d.objs[page] = d.objs[page].replace(b"/Font << ", f"/Font << /T3 {t3} 0 R ".encode())
        enc = bytes(0x41 + glyphs.index(ch) for ch in word)
        d.objs[cs] = d.stream(b"", b"BT /F1 12 Tf 72 720 Td (In bars of a Type 3 font, "
                              b"the word is:) Tj ET BT /T3 24 Tf 72 680 Td " + pdfstr(enc) +
                              b" Tj ET BT /F1 12 Tf 72 640 Td (The number is " +
                              str(n0).encode() + b".) Tj ET")
        c.add("type3", d.write(root), "text", "A Type 3 font of bars, glyph names "
              "that say nothing (g0, g1...), its text only in a ToUnicode CMap",
              pages=1, producer="pdfcorpus", questions=[
                  ("What word is written in the Type 3 font, and what is the number?",
                   [w0, num(n0)])])

    (w0, n0), = facts("symbol-fonts", 1)
    doc("symbol-fonts", "The standard Symbol and ZapfDingbats fonts, whose codes "
        "mean Greek letters and dingbats",
        [b"BT /F1 12 Tf 72 700 Td (Greek letters follow:) Tj /F2 14 Tf ( abg) Tj "
         b"/F1 12 Tf (. Dingbats follow:) Tj /F3 14 Tf ( 4\\(n) Tj /F1 12 Tf ( The number is " +
         str(n0).encode() + b".) Tj ET"],
        [("Which Greek letters follow 'Greek letters follow'?",
          ["(αβγ|α ?β ?γ|alpha.*beta.*gamma)"]),
         ("What number is given?", [num(n0)])],
        fonts={b"F1": None,
               b"F2": b"<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>",
               b"F3": b"<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>"})

    (w0, n0), = facts("invisible-text", 1)
    doc("invisible-text", "Invisible text (render mode 3) over nothing, as an "
        "OCR layer is, beside visible text",
        [text(["This line is visible."]) + b"\nBT 3 Tr /F1 12 Tf 72 680 Td " +
         pdfstr(f"Invisible, the word is {w0} and the number is {n0}.") + b" Tj ET"],
        [("What word and number does the invisible text give?", [w0, num(n0)])])

    (w0, n0), = facts("outline", 1)
    if c.wanted("outline"):
        d, root = simple([text(["A page with bookmarks."]), text(["Another page."])])
        title = ("﻿" + f"Ünïcødé bookmark: {w0} {n0}").encode("utf-16-be")
        first = d.new()
        second = d.new()
        outl = d.new()
        pages = [k for k, v in d.objs.items() if isinstance(v, bytes) and b"/Type /Page " in v]
        d.put(first, f"<< /Title (Introduction) /Parent {outl} 0 R /Next {second} 0 R "
                     f"/Dest [{pages[0]} 0 R /Fit] >>".encode())
        d.put(second, b"<< /Title " + pdfhex(title) + f" /Parent {outl} 0 R /Prev {first} 0 R "
                      f"/Dest [{pages[1]} 0 R /Fit] >>".encode())
        d.put(outl, f"<< /Type /Outlines /First {first} 0 R /Last {second} 0 R /Count 2 >>"
              .encode())
        d.objs[root] = d.objs[root].replace(b" >>", f" /Outlines {outl} 0 R >>".encode())
        info = d.add(b"<< /Title " + pdfhex(("﻿" + "Ωmega report").encode("utf-16-be")) +
                     b" /Author (Corpus) >>")
        c.add("outline", d.write(root, info), "text", "Bookmarks (an outline) with "
              "a UTF-16 title, and a UTF-16 document title, which the pages "
              "never show", pages=2, producer="pdfcorpus", questions=[
                  ("What word and number are in the second bookmark's title?",
                   [w0, num(n0)])])

    (w0, n0), = facts("header-footer", 1)
    pages = []
    for i in range(1, 5):
        body = [f"Body of page {i}."] + ([f"On this page, the word is {w0} and the "
                                         f"number is {n0}."] if i == 3 else [])
        pages.append(text(["Harbour Report - Confidential"], y=760, size=9) + b"\n" +
                     text(body) + b"\n" + text([f"Page {i} of 4"], x=280, y=40, size=9))
    doc("header-footer", "Four pages each with the same header and a footer "
        "numbering it", pages,
        [("What word and number are given on page 3?", [w0, num(n0)])])

    (w0, n0), = facts("many-pages", 1)
    pages = [text([f"Page {i}: an ordinary page of the ledger."]) for i in range(1, 301)]
    pages[249] = text([f"Page 250: the ledger's missing entry is {w0}, number {n0}."])
    doc("many-pages", "300 pages, a fact on page 250", pages,
        [("What is the ledger's missing entry, and its number?", [w0, num(n0)])])

    (w0, n0), = facts("pdf20", 1)
    if c.wanted("pdf20"):
        d, root = simple([text([f"A PDF 2.0 file: the word is {w0}, the number {n0}."])],
                         version="2.0")
        c.add("pdf20", d.write(root), "text", "A PDF 2.0 header", pages=1,
              producer="pdfcorpus", questions=[("What word and number are given?",
                                                [w0, num(n0)])])


# ---------------------------------------------------------------------
# Files a reader should refuse, or give up on cleanly

def hostile(c):
    def keep(name, data, about, expect="bounded", questions=()):
        if c.wanted(name):
            c.add(name, data, "hostile", about, expect=expect, producer="pdfcorpus",
                  questions=questions)

    keep("not-pdf-html", b"<!doctype html><title>Not a PDF</title><p>Saved as .pdf.\n",
         "An HTML page named .pdf", expect="refuse")
    keep("empty", b"", "An empty file", expect="refuse")
    keep("header-only", b"%PDF-1.7\n", "A header and nothing else")

    d, root = simple([text(["A page."])])
    tree = int(re.search(rb"/Pages (\d+)", d.objs[root]).group(1))
    d.objs[tree] = d.objs[tree].replace(b"/Kids [", f"/Kids [{tree} 0 R ".encode()) \
        .replace(b"/Count 1", b"/Count 2")
    keep("page-tree-cycle", d.write(root), "The page tree lists itself among "
         "its kids")

    def selfform(dd):
        n = dd.new()
        dd.put(n, dd.stream(f"/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                            f"/Resources << /XObject << /Fm0 {n} 0 R >> >>".encode(),
                            b"q 0.9 0 0 0.9 10 10 cm /Fm0 Do Q"))
        return {b"Fm0": n}
    d, root = simple([text(["A page with a form that draws itself."]) + b"\n/Fm0 Do"],
                     xobjects=selfform)
    keep("form-recursion", d.write(root), "A Form XObject that draws itself")

    d, root = simple([text(["Deep nesting follows."]) + b"\n" + b"[" * 100000 +
                      b"]" * 100000])
    deep = d.add(b"<< /A " * 20000 + b"1" + b" >>" * 20000)
    d.objs[root] = d.objs[root].replace(b" >>", f" /Deep {deep} 0 R >>".encode())
    keep("deep-nesting", d.write(root), "Arrays nested 100000 deep in the "
         "content, dictionaries 20000 deep in an object")

    # A content stream that inflates to 1 GiB of spaces
    z = zlib.compressobj(9)
    chunk = b" " * (1 << 20)
    bomb = bytearray(z.compress(text(["Before the bomb."])))
    for _ in range(1024):
        bomb += z.compress(chunk)
    bomb += z.compress(text(["After the bomb."], y=680)) + z.flush()
    d, root = simple([b""])
    cs = [k for k, v in d.objs.items() if isinstance(v, tuple)][0]
    d.objs[cs] = (b"/Filter /FlateDecode", bytes(bomb))
    keep("flate-bomb", d.write(root), f"A content stream of {len(bomb)} bytes "
         "that inflates to over 1 GiB", expect="text", questions=[
             ("What does the page say, and is all of it read?",
              ["Before the bomb", "rest of this page is not shown"])])

    d, root = simple([text(["An object stream that contains itself."])], version="1.5")
    n = d.new()
    inner = f"{n} 0 << /Type /ObjStm >>".encode()
    d.put(n, d.stream(f"/Type /ObjStm /N 1 /First {len(str(n)) + 3} /Extends {n} 0 R"
                      .encode(), inner, ["FlateDecode"]))
    keep("objstm-self", d.write(root), "An object stream that extends itself "
         "and holds its own number")

    d, root = simple([text(["A stream that claims to be huge."])])
    data = d.write(root)
    keep("length-huge", re.sub(rb"/Length \d+", b"/Length 999999999999", data, count=1),
         "A /Length of nearly a terabyte", expect="text")
    keep("length-negative", re.sub(rb"/Length \d+", b"/Length -5", data, count=1),
         "A /Length of -5", expect="text")
    big = re.sub(rb"xref\n0 \d+", b"xref\n0 99999999", data)
    keep("xref-size-huge", re.sub(rb"/Size \d+", b"/Size 99999999", big),
         "An xref subsection and /Size of 99999999 entries in a tiny file")


# ---------------------------------------------------------------------
# Images: a word drawn only in pixels, in each way PDF encodes them

def raster(c, word, device, w=480, h=96, color=False):
    """A word rendered by Ghostscript on a device: raw PNM or JPEG bytes."""
    d = os.path.join(c.work, "rasters")
    os.makedirs(d, exist_ok=True)
    ps = os.path.join(d, f"{word}-{device}.ps")
    out = os.path.join(d, f"{word}-{device}.out")
    fill = "1 0.95 0.6 setrgbcolor 0 0 %d %d rectfill 0.1 0.2 0.7 setrgbcolor" % (w, h) \
        if color else ""
    with open(ps, "w") as f:
        f.write(f"%!PS\n<< /PageSize [{w} {h}] >> setpagedevice {fill}\n"
                f"/Helvetica-Bold 56 selectfont 16 26 moveto ({word.upper()}) show showpage\n")
    gs(f"-sDEVICE={device}", "-r72", f"-sOutputFile={out}", ps)
    with open(out, "rb") as f:
        return f.read() if device.startswith("jpeg") else None, out


def pngfile(path, w, h, rgb):
    """A plain 8-bit RGB PNG of the pixels."""
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def images(c):
    def one(name, about, xobj, w=480, h=96, draw=None, extra_q=()):
        if not c.wanted(name):
            return
        (word, n), = c.facts(name, 1)
        content = text([f"The picture below shows a code word. Caption number: {n}."]) + \
            (draw or f"\nq {w} 0 0 {h} 72 560 cm /Im0 Do Q".encode())
        d, root = simple([content], xobjects=lambda dd: xobj(dd, word))
        c.add(name, d.write(root), "image", about, expect="images", pages=1,
              producer="pdfcorpus", questions=[
                  ("What code word does the picture show?", [word], "image"),
                  ("What is the caption number?", [num(n)])] + list(extra_q))

    def img(dict_, data, filters=(), parms=None):
        return lambda dd, word: {b"Im0": dd.add(dd.stream(
            b"/Type /XObject /Subtype /Image " + dict_(word) +
            (b" /DecodeParms " + parms if parms else b""), data(word), list(filters)))}

    def ppm(word):
        return readpnm(raster(c, word, "ppmraw", color=True)[1])[2]

    def pgm(word):
        return readpnm(raster(c, word, "pgmraw")[1])[2]

    def pbm(word):
        return readpnm(raster(c, word, "pbmraw")[1])[2]

    def jpeg(device):
        return lambda word: raster(c, word, device, color=device == "jpeg")[0]

    def dims(cs, bpc=8, more=b""):
        return lambda word: f"/Width 480 /Height 96 /ColorSpace {cs} /BitsPerComponent {bpc} "\
            .encode() + more

    one("image-dct-rgb", "A JPEG (DCTDecode), RGB", lambda dd, word: {b"Im0": dd.add((
        dims("/DeviceRGB")(word) + b"/Type /XObject /Subtype /Image /Filter /DCTDecode",
        jpeg("jpeg")(word)))})
    one("image-dct-gray", "A JPEG, grayscale", lambda dd, word: {b"Im0": dd.add((
        dims("/DeviceGray")(word) + b"/Type /XObject /Subtype /Image /Filter /DCTDecode",
        jpeg("jpeggray")(word)))})
    one("image-dct-cmyk", "A JPEG, CMYK as Ghostscript writes it (Adobe "
        "APP14, inverted)", lambda dd, word: {b"Im0": dd.add((
            dims("/DeviceCMYK", more=b"/Decode [1 0 1 0 1 0 1 0] ")(word) +
            b"/Type /XObject /Subtype /Image /Filter /DCTDecode", jpeg("jpegcmyk")(word)))})
    one("image-flate-rgb", "Flate RGB pixels", img(dims("/DeviceRGB"), ppm, ["FlateDecode"]))

    def predicted(word):
        px, w, out, prev = ppm(word), 480, bytearray(), bytes(480 * 3)
        for y in range(96):
            row = px[y * w * 3:(y + 1) * w * 3]
            out += b"\x02" + bytes((a - b) & 0xff for a, b in zip(row, prev))
            prev = row
        return bytes(out)
    one("image-flate-predictor", "Flate RGB with PNG predictors (Up on every row)",
        img(dims("/DeviceRGB"), predicted, ["FlateDecode"],
            b"<< /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns 480 >>"))
    one("image-gray16", "16 bits per component, grayscale",
        img(dims("/DeviceGray", 16), lambda word: bytes(b for g in pgm(word) for b in (g, g)),
            ["FlateDecode"]))

    def indexed(word):
        g, out = pgm(word), bytearray()
        for y in range(96):
            row = [min(3, g[y * 480 + x] // 64) for x in range(480)]
            for x in range(0, 480, 4):
                out.append(row[x] << 6 | row[x + 1] << 4 | row[x + 2] << 2 | row[x + 3])
        return bytes(out)
    one("image-indexed", "An indexed palette of four colors, 2 bits a pixel",
        img(dims("[/Indexed /DeviceRGB 3 <2020A0 6060C0 A0A0E0 FFFFFF>]", 2), indexed,
            ["FlateDecode"]))
    one("image-mono", "1 bit a pixel, Decode [1 0]",
        img(dims("/DeviceGray", 1, b"/Decode [1 0] "), pbm, ["FlateDecode"]))
    one("image-mask", "A stencil mask (ImageMask) painted red",
        lambda dd, word: {b"Im0": dd.add(dd.stream(
            b"/Type /XObject /Subtype /Image /Width 480 /Height 96 /ImageMask true "
            b"/BitsPerComponent 1 /Decode [1 0]", pbm(word), ["FlateDecode"]))},
        draw=b"\nq 0.8 0 0 rg 480 0 0 96 72 560 cm /Im0 Do Q")

    def smask(dd, word):
        a = dd.add(dd.stream(b"/Type /XObject /Subtype /Image /Width 480 /Height 96 "
                             b"/ColorSpace /DeviceGray /BitsPerComponent 8",
                             bytes(255 - g for g in pgm(word)), ["FlateDecode"]))
        return {b"Im0": dd.add(dd.stream(
            f"/Type /XObject /Subtype /Image /Width 480 /Height 96 /ColorSpace /DeviceRGB "
            f"/BitsPerComponent 8 /SMask {a} 0 R".encode(), bytes([20, 110, 40]) * (480 * 96),
            ["FlateDecode"]))}
    one("image-smask", "A flat green image whose soft mask (SMask) holds the word",
        smask)
    one("image-decode-inverted", "Grayscale stored inverted, Decode [1 0] "
        "turning it back", img(dims("/DeviceGray", 8, b"/Decode [1 0] "),
                               lambda word: bytes(255 - g for g in pgm(word)), ["FlateDecode"]))

    if c.wanted("image-inline"):
        (word, n), = c.facts("image-inline", 1)
        bits = readpnm(raster(c, word, "pbmraw")[1])[2]
        inline = (b"\nq 480 0 0 96 72 560 cm BI /W 480 /H 96 /CS /G /BPC 1 /D [1 0] /F /AHx ID\n" +
                  bits.hex().encode() + b">\nEI Q")
        d, root = simple([text([f"The picture below shows a code word. Caption number: {n}."]) +
                          inline])
        c.add("image-inline", d.write(root), "image", "An inline image (BI ID EI) "
              "in the content stream, ASCIIHex encoded", expect="images", pages=1,
              producer="pdfcorpus", questions=[
                  ("What code word does the picture show?", [word], "image"),
                  ("What is the caption number?", [num(n)])])

    if c.wanted("image-jpx") and have("sips"):
        (word, n), = c.facts("image-jpx", 1)
        d = os.path.join(c.work, "rasters")
        png, jp2 = os.path.join(d, "jpx.png"), os.path.join(d, "jpx.jp2")
        pngfile(png, 480, 96, ppm(word))
        run(["sips", "-s", "format", "jp2", png, "--out", jp2])
        with open(jp2, "rb") as f:
            j = f.read()
        dd, root = simple([text([f"The picture below shows a code word. Caption number: {n}."]) +
                           b"\nq 480 0 0 96 72 560 cm /Im0 Do Q"],
                          xobjects=lambda x: {b"Im0": x.add((
                              b"/Type /XObject /Subtype /Image /Width 480 /Height 96 "
                              b"/Filter /JPXDecode", j))})
        c.add("image-jpx", dd.write(root), "image", "JPEG 2000 (JPXDecode), by "
              "macOS's sips", expect="images", pages=1, producer="pdfcorpus",
              questions=[("What code word does the picture show?", [word], "image"),
                         ("What is the caption number?", [num(n)])])

    if c.wanted("image-logo-pages"):
        (word, n), = c.facts("image-logo-pages", 1)
        nums = c.facts("image-logo-pages-n", 5)
        logo = pgm(word)
        pages = [text([f"Page {i + 1} of the newsletter: its number is {nums[i][1]}."]) +
                 b"\nq 240 0 0 48 300 740 cm /Im0 Do Q" for i in range(5)]
        dd, root = simple(pages, xobjects=lambda x: {b"Im0": x.add(x.stream(
            b"/Type /XObject /Subtype /Image /Width 480 /Height 96 /ColorSpace "
            b"/DeviceGray /BitsPerComponent 8", logo, ["FlateDecode"]))})
        c.add("image-logo-pages", dd.write(root), "image", "One logo image drawn on "
              "each of five pages: a reader should show it once", expect="images",
              pages=5, producer="pdfcorpus", questions=[
                  ("What word does the logo show?", [word], "image"),
                  ("What number is on page 4?", [num(nums[3][1])])])

    if c.wanted("image-tiled"):
        (word, n), = c.facts("image-tiled", 1)
        g = pgm(word)
        strips = {}

        def tiles(x):
            for i in range(4):
                part = b"".join(g[y * 480 + i * 120:y * 480 + (i + 1) * 120] for y in range(96))
                strips[f"T{i}".encode()] = x.add(x.stream(
                    b"/Type /XObject /Subtype /Image /Width 120 /Height 96 /ColorSpace "
                    b"/DeviceGray /BitsPerComponent 8", part, ["FlateDecode"]))
            return strips
        draw = b"".join(f"\nq 120 0 0 96 {72 + 120 * i} 560 cm /T{i} Do Q".encode()
                        for i in range(4))
        dd, root = simple([text([f"The picture below shows a code word. Caption number: {n}."]) +
                           draw], xobjects=tiles)
        c.add("image-tiled", dd.write(root), "image", "One picture drawn as four "
              "strips side by side, each its own image: the word only reads whole",
              expect="images", pages=1, producer="pdfcorpus", questions=[
                  ("What code word does the picture show?", [word], "image"),
                  ("What is the caption number?", [num(n)])])


def encrypted(c, article):
    if not have("qpdf"):
        return c.skip("encrypted files", "qpdf not found")
    d = os.path.join(c.work, "encrypted")
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "article.pdf")
    with open(src, "wb") as f:
        f.write(article)
    for name, user, args, about in [
        ("encrypted-rc4-40", "u", ["40", "--allow-weak-crypto"], "RC4, 40-bit (R2)"),
        ("encrypted-rc4-128", "u", ["128", "--use-aes=n", "--allow-weak-crypto"], "RC4, 128-bit"),
        ("encrypted-aes-128", "u", ["128", "--use-aes=y"], "AES, 128-bit"),
        ("encrypted-aes-256", "u", ["256"], "AES, 256-bit"),
        ("encrypted-no-password", "", ["256"], "AES 256-bit with an empty user "
         "password: opens without one, but is still encrypted"),
    ]:
        if not c.wanted(name):
            continue
        out = os.path.join(d, name + ".pdf")
        weak = [a for a in args if a == "--allow-weak-crypto"]
        args = [a for a in args if a not in weak]
        run(["qpdf", *weak, "--encrypt", user, "owner", *args, "--", src, out])
        with open(out, "rb") as f:
            c.add(name, f.read(), "encrypted", about, expect="refuse", pages=3,
                  producer="qpdf")


# ---------------------------------------------------------------------
# The manifest, the eval suite, and the check against Poppler

CHECK_SH = r"""#!/bin/sh
# Passes if answer.txt matches every pattern in this task's answers file
# (extended regular expressions, case ignored), and none of the patterns
# that start with '!'. Made by tests/pdfcorpus.py.
f="$PROSPERO_EVAL_SUITE/answers/$PROSPERO_EVAL_TASK.txt"
if [ ! -f answer.txt ]; then
    echo "no answer.txt"
    exit 1
fi
fail=0
while IFS= read -r p; do
    case "$p" in
    '!'*)
        if grep -E -i -q -- "${p#!}" answer.txt; then
            echo "should not match: ${p#!}"
            fail=1
        fi;;
    ?*)
        if ! grep -E -i -q -- "$p" answer.txt; then
            echo "missing: $p"
            fail=1
        fi;;
    esac
done < "$f"
exit $fail
"""


def inistr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def suite(c):
    """An eval suite: a task for each question about a file a reader
    should read."""
    d = os.path.join(c.out, "eval")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "answers"))
    os.makedirs(os.path.join(d, "files"))
    with open(os.path.join(d, "check.sh"), "w", newline="\n") as f:
        f.write(CHECK_SH)
    with open(os.path.join(d, "suite.ini"), "w", newline="\n") as f:
        f.write("# PDF reading, made by tests/pdfcorpus.py: each task asks one\n"
                "# question about one file, to answer through read alone.\n"
                "tools   = read write glob\n"
                "timeout = 600\n"
                "steps   = 30\n"
                'check   = sh "$PROSPERO_EVAL_SUITE/check.sh"\n')
    n = 0
    for e in c.entries:
        if e["expect"] in ("refuse", "bounded") or not e["questions"]:
            continue
        fix = os.path.join(d, "files", e["name"])
        os.makedirs(fix)
        shutil.copy(os.path.join(c.out, e["file"]), os.path.join(fix, e["name"] + ".pdf"))
        for i, q in enumerate(e["questions"], 1):
            task = f"{e['name']}-q{i}"
            prompt = (f"Read the PDF file {e['name']}.pdf in this directory with the "
                      f"read tool, and answer this question about it: {q['q']} "
                      "Write the answer, briefly, to answer.txt. If the file cannot "
                      "be read well enough to answer, write what you could make out.")
            with open(os.path.join(d, task + ".ini"), "w", newline="\n") as f:
                f.write(f"# {e['about']}\n")
                if q.get("needs"):
                    f.write(f"# Needs the file's {q['needs']}s, not only its text.\n")
                f.write(f"prompt  = {inistr(prompt)}\nfixture = files/{e['name']}\n")
            with open(os.path.join(d, "answers", task + ".txt"), "w", newline="\n") as f:
                f.write("\n".join(q["a"]) + "\n")
            n += 1
    print(f"eval suite: {n} tasks in {d}")


def check(c):
    """What Poppler makes of each file: its page count, and whether its
    text answers each question; for questions that need images, whether
    tesseract finds the answer in the page rendered by pdftoppm."""
    if not have("pdftotext"):
        return c.skip("check", "pdftotext not found")
    ocr = have("tesseract") and have("pdftoppm")
    for e in c.entries:
        path = os.path.join(c.out, e["file"])
        chk = {}
        try:
            info = subprocess.run(["pdfinfo", path], capture_output=True, timeout=60)
            m = re.search(rb"^Pages:\s+(\d+)", info.stdout, re.M)
            chk["pages"] = int(m.group(1)) if m else None
            if info.returncode:
                chk["pdfinfo_error"] = info.stderr.decode("utf-8", "replace").strip()[-300:]
            t = subprocess.run(["pdftotext", "-enc", "UTF-8", path, "-"],
                               capture_output=True, timeout=120)
            body = t.stdout.decode("utf-8", "replace")
            chk["text_chars"] = len(body.strip())
            if t.returncode:
                chk["pdftotext_error"] = t.stderr.decode("utf-8", "replace").strip()[-300:]
        except subprocess.TimeoutExpired:
            chk["timeout"] = True
            body = ""
        rendered = None
        for q in e["questions"]:
            pos = [a for a in q["a"] if not a.startswith("!")]
            q["poppler_text"] = all(re.search(a, body, re.I) for a in pos)
            if q.get("needs") == "image" and ocr:
                if rendered is None:
                    png = os.path.join(c.work, "check-" + e["name"])
                    subprocess.run(["pdftoppm", "-r", "150", "-gray", "-png", "-f", "1", "-l", "1",
                                    "-singlefile", path, png], capture_output=True, timeout=120)
                    r = subprocess.run(["tesseract", png + ".png", "-"], capture_output=True,
                                       timeout=120)
                    rendered = r.stdout.decode("utf-8", "replace")
                q["ocr_render"] = all(re.search(a, rendered, re.I) for a in pos)
        e["poppler"] = chk
        flags = "".join("T" if q["poppler_text"] else
                        ("I" if q.get("ocr_render") else "-") for q in e["questions"])
        print(f"  {e['name']:28} {e['expect']:8} pages={chk.get('pages')!s:5} "
              f"text={chk.get('text_chars', 0):<7} answers={flags}")


SEEDMAX = 64 << 10  # the fuzz target's largest input


def seeds(out, dst):
    """Fuzz seeds from the corpus in out, into dst."""
    os.makedirs(dst, exist_ok=True)
    with open(os.path.join(out, "manifest.json")) as f:
        entries = json.load(f)["files"]
    n = 0

    def put(name, data):
        nonlocal n
        if len(data) <= SEEDMAX:
            with open(os.path.join(dst, name), "wb") as f:
                f.write(data)
            n += 1
            return True
        # Corpus files may be too large, but a hand-made seed must fit
        assert not name.startswith("seed-"), f"{name}: {len(data)} bytes, over SEEDMAX"
        return False

    tmp = tempfile.mkdtemp()
    try:
        for e in entries:
            src = os.path.join(out, e["file"])
            with open(src, "rb") as f:
                put(e["name"] + ".pdf", f.read())
            if not have("qpdf"):
                continue
            # qpdf exits 3 for warnings, as damaged files give, its
            # output still written
            qdf = os.path.join(tmp, e["name"] + ".qdf.pdf")
            for pages in ([], ["--pages", ".", "1", "--"]):
                p = subprocess.run(["qpdf", "--qdf", "--object-streams=disable",
                                    "--stream-data=uncompress", src, *pages, qdf],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if p.returncode in (0, 3) and os.path.exists(qdf):
                    with open(qdf, "rb") as f:
                        if put(e["name"] + (".qdf1" if pages else ".qdf") + ".pdf", f.read()):
                            break
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not have("qpdf"):
        print("  qpdf not found: no --qdf forms", flush=True)

    # Hostile files shrunk to fit: a bomb of 32 MiB, nesting 30000 deep,
    # 200 pages of a word each
    z = zlib.compressobj(9)
    bomb = bytearray(z.compress(text(["Before the bomb."])))
    for _ in range(32):
        bomb += z.compress(b" " * (1 << 20))
    bomb += z.compress(text(["After the bomb."], y=680)) + z.flush()
    d, root = simple([b""])
    cs = [k for k, v in d.objs.items() if isinstance(v, tuple)][0]
    d.objs[cs] = (b"/Filter /FlateDecode", bytes(bomb))
    put("seed-bomb.pdf", d.write(root))
    d, root = simple([text(["Deep."]) + b"\n" + b"[" * 30000 + b"]" * 30000])
    put("seed-deep.pdf", d.write(root))
    d, root = simple([text(["Dicts."])])
    deep = d.add(b"<< /A " * 4000 + b"1" + b" >>" * 4000)
    d.objs[root] = d.objs[root].replace(b" >>", f" /Deep {deep} 0 R >>".encode())
    put("seed-deepdict.pdf", d.write(root))
    d, root = simple([pdfstr(WORDS[i % len(WORDS)]) + b" Tj" for i in range(200)])
    put("seed-pages.pdf", d.write(root))

    # One filter each, or a chain, over a page of text
    page = text([" ".join(WORDS[i:i + 8]) for i in range(0, 64, 8)])
    for name, fl in [("lzw", ["LZWDecode"]), ("ahx", ["ASCIIHexDecode"]),
                     ("a85", ["ASCII85Decode"]), ("rl", ["RunLengthDecode"]),
                     ("flate", ["FlateDecode"]), ("chain", ["ASCII85Decode", "LZWDecode"])]:
        d, root = simple([b""])
        cs = [k for k, v in d.objs.items() if isinstance(v, tuple)][0]
        d.objs[cs] = d.stream(b"", page, fl)
        put(f"seed-{name}.pdf", d.write(root))
    d, root = simple([b""])
    cs = [k for k, v in d.objs.items() if isinstance(v, tuple)][0]
    d.objs[cs] = (b"/Filter /LZWDecode /DecodeParms << /EarlyChange 0 >>", lzw(page, 0))
    put("seed-lzw0.pdf", d.write(root))

    # Object and xref streams, as qpdf makes them, over a small file
    if have("qpdf"):
        d, root = simple([text(["Object streams."]), text(["Page two."])], version="1.5")
        src = os.path.join(tmp, "small.pdf")
        os.makedirs(tmp, exist_ok=True)
        with open(src, "wb") as f:
            f.write(d.write(root))
        for name, args in [("objstm", ["--object-streams=generate"]),
                           ("objstm-plain", ["--object-streams=generate",
                                             "--compress-streams=n"])]:
            dst2 = os.path.join(tmp, name + ".pdf")
            if subprocess.run(["qpdf", *args, src, dst2]).returncode in (0, 3):
                with open(dst2, "rb") as f:
                    put(f"seed-{name}.pdf", f.read())
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"{n} seeds in {dst}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", help="the directory to build in")
    ap.add_argument("--only", action="append", help="build only names matching "
                    "this regex (repeatable)")
    ap.add_argument("--no-check", action="store_true", help="skip the Poppler check")
    ap.add_argument("--seeds", metavar="DIR", help="write fuzz seeds into DIR from "
                    "the corpus in OUT, building it first if it is not there")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if args.seeds and os.path.exists(os.path.join(out, "manifest.json")):
        seeds(out, args.seeds)
        return
    shutil.rmtree(os.path.join(out, "pdfs"), ignore_errors=True)
    c = Corpus(out, args.only)
    print("producers")
    base = producers(c)
    if "article" in base:
        print("variants and damage")
        variants(c, *base["article"])
        print("encrypted")
        encrypted(c, base["article"][0])
    else:
        c.skip("variants, damage and encrypted files", "no pdflatex article to vary")
    print("text oddities")
    oddities(c)
    print("images")
    if have("gs"):
        images(c)
    else:
        c.skip("images", "gs not found")
    print("hostile")
    hostile(c)
    if not args.no_check:
        print("check (Poppler; answers: T in its text, I by OCR of the render, - neither)")
        check(c)
    suite(c)
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump({"files": c.entries, "skipped": c.skipped}, f, indent=1, ensure_ascii=False)
    print(f"{len(c.entries)} files in {c.pdfs}; manifest.json; {len(c.skipped)} skipped")
    if args.seeds:
        seeds(out, args.seeds)


if __name__ == "__main__":
    main()
