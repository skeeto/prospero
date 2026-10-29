#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Pack the Windows program, signed, as a zip holding Prospero.exe.

  uv run tools/winzip.py EXE ZIP

Prospero.exe sits at the zip's top, so that it unpacks over an older
copy, from either edition, as Prospero.app is dragged over the old one
on macOS. The zip is deterministic: one member, deflated, at a fixed
date (as tools/engines.py --pack writes its zip).
"""

import os
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: winzip.py EXE ZIP")
    exe, dst = sys.argv[1:]
    with open(exe, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        sys.exit(f"winzip: {exe}: not a Windows program")
    with zipfile.ZipFile(dst + ".tmp", "w") as z:
        info = zipfile.ZipInfo("Prospero.exe", date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100755 << 16
        z.writestr(info, data, compresslevel=9)
    os.replace(dst + ".tmp", dst)
    print(f"winzip: {dst}: {os.path.getsize(dst)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
