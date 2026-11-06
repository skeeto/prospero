#!/usr/bin/env python3
"""Unpack Debian packages, by name, into a cross compiler's sysroot.

  python3 sysroot.py PACKAGES.xz ROOTS SYSROOT NAME...

PACKAGES.xz is an archive's index for one architecture, which the
Dockerfile pins by SHA-256 (as its signed InRelease lists it), and ROOTS
the archive's root URLs, separated by commas, tried in order. Each
NAME's .deb comes from one of them at the index's Filename, checked
against the index's SHA256, so every file is pinned through the index
whichever mirror serves it. Absolute symbolic links, which point into
the host, are made to point within SYSROOT.
"""

import hashlib
import lzma
import os
import subprocess
import sys
import time
import urllib.request


def index(path):
    """The index's entries by package name."""
    entries = {}
    with lzma.open(path, "rt", encoding="utf-8") as f:
        for para in f.read().split("\n\n"):
            fields = {}
            for line in para.splitlines():
                if line and not line[0].isspace() and ":" in line:
                    k, v = line.split(":", 1)
                    fields[k] = v.strip()
            if "Package" in fields:
                entries[fields["Package"]] = fields
    return entries


def fetch(roots, path, sha256):
    """The file at path under one of roots, with that SHA-256."""
    errs = []
    for attempt in range(4):
        for root in roots:
            url = f"{root.rstrip('/')}/{path}"
            try:
                with urllib.request.urlopen(url, timeout=120) as r:
                    data = r.read()
            except OSError as e:
                errs.append(f"{url}: {e}")
                continue
            got = hashlib.sha256(data).hexdigest()
            if got == sha256:
                return data
            errs.append(f"{url}: SHA-256 {got}, but the index says {sha256}")
        time.sleep(2 ** attempt)
    sys.exit("sysroot: " + "\nsysroot: ".join(errs[-len(roots):]))


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    packages, roots, sysroot, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    roots = roots.split(",")
    entries = index(packages)
    os.makedirs(sysroot, exist_ok=True)
    for name in names:
        e = entries.get(name)
        if not e:
            sys.exit(f"sysroot: {name} is not in {packages}")
        data = fetch(roots, e["Filename"], e["SHA256"])
        deb = f"/tmp/{name}.deb"
        with open(deb, "wb") as f:
            f.write(data)
        subprocess.run(["dpkg-deb", "-x", deb, sysroot], check=True)
        os.remove(deb)
        print(f"sysroot: {name} {e['Version']}", flush=True)

    for root, dirs, files in os.walk(sysroot):
        for n in dirs + files:
            p = os.path.join(root, n)
            if os.path.islink(p):
                to = os.readlink(p)
                if to.startswith("/") and not to.startswith(sysroot):
                    os.remove(p)
                    os.symlink(sysroot + to, p)


main()
