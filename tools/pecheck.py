#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check Windows executables for release: architecture, stripped, timestamp.

  uv run tools/pecheck.py [--machine ARCH] [--subsystem gui|console] [--signed] EXE...

Each EXE must be a PE image with a zero timestamp, in its COFF header and
any debug directory entry, so that the same sources link the same bytes,
and stripped: no COFF symbol table and no .debug sections. --machine
(x86_64 or i686) and --subsystem require those, and --signed requires an
Authenticode signature: a nonempty security directory (data directory 4)
holding a PKCS#7 certificate. The PE file is parsed here, with no
objdump, so this runs on any host. Exits 1 on any violation, listing each.
"""

import argparse
import struct
import sys

MACHINES = {0x14c: "i686", 0x8664: "x86_64", 0xaa64: "aarch64"}
SUBSYSTEMS = {2: "gui", 3: "console"}


def check(path, args):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"MZ":
        return {}, ["not an executable"]
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    if d[pe:pe+4] != b"PE\0\0":
        return {}, ["no PE header"]
    machine, nsections, stamp, symptr, nsyms, optsize = \
        struct.unpack_from("<HHIIIH", d, pe+4)
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    wide = magic == 0x20b
    subsystem = struct.unpack_from("<H", d, opt+68)[0]
    ndirs = struct.unpack_from("<I", d, opt + (108 if wide else 92))[0]
    dirs = opt + (112 if wide else 96)
    directory = [struct.unpack_from("<II", d, dirs + 8*i) for i in range(ndirs)]
    sections = []
    for i in range(nsections):
        s = opt + optsize + 40*i
        name = d[s:s+8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, s+8)
        sections.append((name, va, max(vsize, rawsize), raw))

    info = {
        "machine": MACHINES.get(machine, f"{machine:#x}"),
        "subsystem": SUBSYSTEMS.get(subsystem, str(subsystem)),
    }
    bad = []
    if args.machine and info["machine"] != args.machine:
        bad.append(f"machine {info['machine']}, not {args.machine}")
    if args.subsystem and info["subsystem"] != args.subsystem:
        bad.append(f"subsystem {info['subsystem']}, not {args.subsystem}")
    if stamp:
        bad.append(f"timestamp {stamp:#x}, not zero")
    if symptr or nsyms:
        bad.append(f"not stripped: {nsyms} COFF symbols")
    # Long section names ("/4") index the string table after the symbols,
    # which in practice means DWARF
    debug = [n for n, _, _, _ in sections if n.startswith((".debug", "/"))]
    if debug:
        bad.append(f"not stripped: sections {' '.join(debug)}")

    rva, size = directory[6] if len(directory) > 6 else (0, 0)
    for k in range(size // 28):
        off = None
        for _, va, vsize, raw in sections:
            if va <= rva + 28*k < va + vsize:
                off = raw + rva + 28*k - va
        if off is not None and struct.unpack_from("<I", d, off+4)[0]:
            bad.append(f"debug directory entry {k} has a timestamp")

    off, size = directory[4] if len(directory) > 4 else (0, 0)
    if not size:
        info["signature"] = "unsigned"
    elif off + size > len(d) or size < 8:
        bad.append(f"security directory {off:#x}+{size:#x} outside the file")
    else:
        length, revision, kind = struct.unpack_from("<IHH", d, off)
        info["signature"] = f"signed ({size} bytes)"
        if kind != 2:
            bad.append(f"certificate type {kind}, not PKCS#7 (2)")
    if args.signed and not size:
        bad.append("unsigned: the security directory is empty")
    return info, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--machine", choices=["x86_64", "i686"])
    ap.add_argument("--subsystem", choices=["gui", "console"])
    ap.add_argument("--signed", action="store_true",
                    help="require an Authenticode signature")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    status = 0
    for path in args.files:
        try:
            info, bad = check(path, args)
        except (OSError, struct.error) as e:
            info, bad = {}, [str(e)]
        for b in bad:
            print(f"{path}: {b}")
        desc = ", ".join(info[k] for k in ("machine", "subsystem", "signature") if k in info)
        print(f"{path}: {desc}: {'ok' if not bad else f'{len(bad)} problems'}")
        status |= 1 if bad else 0
    return status


if __name__ == "__main__":
    sys.exit(main())
