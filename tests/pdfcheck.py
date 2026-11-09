#!/usr/bin/env python3
"""Check the built-in PDF reader against the generated corpus.

    $ uv run tests/pdfcheck.py --prospero build/prospero [--corpus DIR] [--build DIR]

The corpus (tests/pdfcorpus.py) is built with --no-check into the build
directory's pdfcorpus/, unless --corpus names one already built. Each
file is read with `prospero --pdf FILE --all --images DIR`, with a 20 s
timeout, and its manifest entry checked:
  text, partial  each question's answer regexes match the text, and its
                 '!' regexes do not (questions that need images skipped)
  images         an image file is written for each image the text shows
                 (not under 24 pixels on a side, nor JPEG 2000 or JBIG2),
                 with no image failing to decode; its questions are
                 checked as text's are, against the text and, when
                 tesseract is installed, what it reads in those images
                 (those that need images skipped without it)
  refuse         exit 1, with a reason on standard error
  bounded        finishes in time, exit 0 or 1, without a signal
Known gaps are listed in XFAIL, each with its reason, and those of OCR
in XFAIL_OCR: such a failure does not fail the check, and one that
passes is reported, so that the table shrinks. It exits 0 when nothing unexpected failed, and
77 (a skip) when the generator's tools are missing.

This is free and unencumbered software released into the public domain.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TIMEOUT = 20

# name, or name/qN for one question: why it fails for now
XFAIL = {
    "xelatex-scripts/q1": "a Japanese CID font with no ToUnicode: the Adobe-Japan1 "
                          "tables are left out",
}

# With tesseract: answers read from the images that it cannot find
XFAIL_OCR = {
    "image-jpx/q1": "JPEG 2000 is not decoded, only noted (settled for this round)",
    "image-tiled/q1": "its four strips are shown apart, and read whole only stitched "
                      "(deferred, plan step 16)",
    "scan-image/q2": "tesseract reads the table's 9914 as 'go1d' at the 1 MP the "
                     "page is shown at, though it is legible there",
}

IMAGE = re.compile(r"^\[image (\d+): (\d+)x(\d+)([^\]]*)\]$", re.M)
WRITTEN = re.compile(r"^p(\d+)-(\d+)\.(png|jpg)$")


def build(out):
    """Build the corpus into out, unless built there since the generator
    last changed, or None if its tools are missing."""
    if not shutil.which("pdflatex"):
        return None
    gen = os.path.join(HERE, "pdfcorpus.py")
    manifest = os.path.join(out, "manifest.json")
    if os.path.exists(manifest) and os.path.getmtime(manifest) > os.path.getmtime(gen):
        return out
    subprocess.run([sys.executable, gen, "--no-check", out], check=True,
                   stdout=subprocess.DEVNULL)
    return out


def matches(text, q):
    """Why a question's regexes do not hold for text, or None."""
    for a in q["a"]:
        if a.startswith("!"):
            if re.search(a[1:], text, re.I):
                return f"matches !{a[1:]}"
        elif not re.search(a, text, re.I):
            return f"no match for {a}"
    return None


def shown(text):
    """The numbers of the images the text shows, by its placeholders:
    those not too small and not of a kind left undecoded."""
    ids = set()
    for m in IMAGE.finditer(text):
        w, h = int(m.group(2)), int(m.group(3))
        if w >= 24 and h >= 24 and "not shown" not in m.group(4):
            ids.add(int(m.group(1)))
    return ids


def ocr(paths):
    """What tesseract reads in the images, all together."""
    out = []
    for f in paths:
        p = subprocess.run(["tesseract", f, "-"], capture_output=True, timeout=120)
        out.append(p.stdout.decode("utf-8", "replace"))
    return "\n".join(out)


def check(prospero, corpus, entry, useocr):
    """A list of (key, failure) for one entry, key name or name/qN."""
    with tempfile.TemporaryDirectory() as d:
        return checkin(prospero, corpus, entry, useocr, d)


def checkin(prospero, corpus, entry, useocr, imgdir):
    path = os.path.join(corpus, entry["file"])
    name = entry["name"]
    expect = entry["expect"]
    try:
        p = subprocess.run([prospero, "--pdf", path, "--all", "--images", imgdir],
                           capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return [(name, f"over {TIMEOUT} s")]
    err = p.stderr.decode("utf-8", "replace").strip()
    if p.returncode < 0:
        return [(name, f"signal {-p.returncode}: {err[-300:]}")]
    if expect == "bounded":
        return [] if p.returncode in (0, 1) else [(name, f"exit {p.returncode}: {err}")]
    if expect == "refuse":
        if p.returncode != 1 or not err:
            return [(name, f"not refused: exit {p.returncode}")]
        return []
    if p.returncode != 0:
        return [(name, f"exit {p.returncode}: {err[-300:]}")]
    try:
        text = p.stdout.decode("utf-8")
    except UnicodeDecodeError as e:
        return [(name, f"not UTF-8: {e}")]
    fails = []
    seen = text
    if expect == "images":
        files = sorted(os.listdir(imgdir))
        got = {int(m.group(2)) for m in map(WRITTEN.match, files) if m}
        want = shown(text)
        if got != want:
            fails.append((name, f"images written {sorted(got)}, shown {sorted(want)}"))
        m = re.search(r" failed=(\d+)", err)
        if not m or m.group(1) != "0":
            fails.append((name, f"images failed: {err[-300:]}"))
        if useocr:
            seen = text + "\n" + ocr(os.path.join(imgdir, f) for f in files)
    for i, q in enumerate(entry["questions"], 1):
        if q.get("needs") == "image" and not (useocr and expect == "images"):
            continue
        why = matches(seen, q)
        if why:
            fails.append((f"{name}/q{i}", why))
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prospero", required=True)
    ap.add_argument("--corpus", help="a corpus already built: the directory holding its manifest.json")
    ap.add_argument("--build", help="where to build the corpus: DIR/pdfcorpus",
                    default=".")
    ap.add_argument("-v", action="store_true", help="list every check")
    args = ap.parse_args()

    corpus = args.corpus
    if not corpus:
        corpus = build(os.path.join(os.path.abspath(args.build), "pdfcorpus"))
        if not corpus:
            print("skipped: pdflatex not found, so no corpus to check")
            sys.exit(77)
    with open(os.path.join(corpus, "manifest.json")) as f:
        entries = json.load(f)["files"]

    useocr = bool(shutil.which("tesseract"))
    xfail = {**XFAIL, **(XFAIL_OCR if useocr else {})}
    if not useocr:
        print("tesseract not found: images are checked without OCR")
    passed = failed = xfailed = 0
    fixed = []
    for e in entries:
        fails = dict(check(os.path.abspath(args.prospero), corpus, e, useocr))
        keys = [e["name"]] + [f"{e['name']}/q{i}" for i in range(1, len(e["questions"]) + 1)]
        for k in keys:
            known = xfail.get(k) or (xfail.get(e["name"]) if k != e["name"] else None)
            if k in fails:
                if known:
                    xfailed += 1
                    if args.v:
                        print(f"  xfail {k}: {fails[k]} ({known})")
                else:
                    failed += 1
                    print(f"  FAIL  {k}: {fails[k]}")
            elif k in xfail and (k != e["name"] or not fails):
                fixed.append(k)
        if not any(k in fails for k in keys):
            passed += 1
            if args.v:
                print(f"  ok    {e['name']}")
    for k in fixed:
        print(f"  XPASS {k}: passes now, so take it out of XFAIL ({xfail[k]})")
    print(f"{len(entries)} files: {passed} passed whole, {failed} unexpected "
          f"failures, {xfailed} expected (XFAIL), {len(fixed)} XFAIL passing")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
