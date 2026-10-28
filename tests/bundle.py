#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""macOS: Prospero.app's identifiers, as built, never the real app's in
a test build.

  uv run tests/bundle.py --app build/Prospero.app --helper build/prospero-desktop
                         --version VERSION [--signed] [--bundle-id ID]

Privacy grants name a bundle identifier, so an ad-hoc test build that
claimed com.nullprogram.prospero could leave a code requirement on the
real app's grants that then refused the real app. The identifier is
com.nullprogram.prospero only for a signed build (--signed: built with
PROSPERO_SIGN_IDENTITY), else com.nullprogram.prospero.dev, unless
PROSPERO_BUNDLE_ID (--bundle-id) names one; the desktop helper's is that
plus ".desktop". This checks the bundle's Info.plist, its build number
the build's time in seconds, its icon, and the signatures
of the bundle, of the helper in it, and of the helper beside it, by
codesign -d, which only reads.
"""

import argparse
import os
import plistlib
import re
import subprocess
import sys

REAL = "com.nullprogram.prospero"


def signature(path):
    """codesign -d's Identifier and Signature fields of path."""
    p = subprocess.run(["codesign", "-d", "-vv", path], capture_output=True, text=True)
    out = p.stdout + p.stderr
    ident = re.search(r"^Identifier=(.*)$", out, re.M)
    sig = re.search(r"^Signature=(.*)$", out, re.M)
    return (ident.group(1) if ident else None, sig.group(1) if sig else None, out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--app", required=True)
    ap.add_argument("--helper", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--signed", action="store_true")
    ap.add_argument("--bundle-id", default="")
    args = ap.parse_args()

    want = args.bundle_id or (REAL if args.signed else REAL + ".dev")
    desk = want + ".desktop"
    problems = []
    with open(f"{args.app}/Contents/Info.plist", "rb") as f:
        info = plistlib.load(f)
    if info.get("CFBundleIdentifier") != want:
        problems.append(f"Info.plist: CFBundleIdentifier {info.get('CFBundleIdentifier')}, "
                        f"not {want}")
    if info.get("CFBundleShortVersionString") != args.version:
        problems.append(f"Info.plist: version {info.get('CFBundleShortVersionString')}, "
                        f"not {args.version}")
    build = str(info.get("CFBundleVersion", ""))
    if not build.isdigit() or int(build) < 1700000000:
        problems.append(f"Info.plist: CFBundleVersion {build!r}, not the build's time")
    icon = f"{args.app}/Contents/Resources/{info.get('CFBundleIconFile')}.icns"
    if not os.path.isfile(icon):
        problems.append(f"Info.plist: CFBundleIconFile names no {icon}")
    for path, ident in ((args.app, want),
                        (f"{args.app}/Contents/MacOS/prospero-desktop", desk),
                        (args.helper, desk)):
        got, sig, out = signature(path)
        if got != ident:
            problems.append(f"{path}: identifier {got}, not {ident}: {out.strip()}")
        if not args.signed and sig != "adhoc":
            problems.append(f"{path}: signature {sig}, not ad hoc")
        if not args.signed and got in (REAL, REAL + ".desktop"):
            problems.append(f"{path}: a test build claims the real app's {got}")
    for p in problems:
        print(f"FAIL: {p}")
    if not problems:
        print(f"ok: {want}, helper {desk}" + ("" if args.signed else ", ad hoc"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
