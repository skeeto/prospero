#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check Windows executables for release: architecture, stripped, timestamp.

  uv run tools/pecheck.py [--machine ARCH] [--subsystem gui|console]
                          [--signed [--unsigned DIR] [--signer TEXT]] EXE...

Each EXE must be a PE image with a zero timestamp, in its COFF header and
any debug directory entry, so that the same sources link the same bytes,
and stripped: no COFF symbol table and no .debug sections. --machine
(x86_64 or i686) and --subsystem require those.

--signed requires an Authenticode signature, checked here: one PKCS#7
certificate, last in the file, whose signed content's digest is the
file's Authenticode digest, recomputed; whose signed attributes hold the
digest of that content; and whose signature, RSA as Azure Artifact
Signing makes it, verifies with the signer's certificate. Nothing here
checks that certificate's chain to a root, nor the timestamp.
--signer requires TEXT in the signer's subject (as "CN=Name, O=..."),
and --unsigned that the program, its signature taken out (cut at the
certificate table, with that table's directory entry and the CheckSum
zeroed), is the file of its name in DIR, as built before signing, but
for the CheckSum and the zeros the signer pads with: the signer changed
no byte of the program.

The PE file is parsed here, with no objdump or other tool, so this runs
on any host. Exits 1 on any violation, listing each.
"""

import argparse
import hashlib
import os
import struct
import sys

MACHINES = {0x14c: "i686", 0x8664: "x86_64", 0xaa64: "aarch64"}
SUBSYSTEMS = {2: "gui", 3: "console"}

OID_SIGNED_DATA = "1.2.840.113549.1.7.2"
OID_SPC_INDIRECT = "1.3.6.1.4.1.311.2.1.4"
OID_CONTENT_TYPE = "1.2.840.113549.1.9.3"
OID_MESSAGE_DIGEST = "1.2.840.113549.1.9.4"
OID_RSA = "1.2.840.113549.1.1.1"
DIGESTS = {
    "1.3.14.3.2.26": "sha1",
    "2.16.840.1.101.3.4.2.1": "sha256",
    "2.16.840.1.101.3.4.2.2": "sha384",
    "2.16.840.1.101.3.4.2.3": "sha512",
}
RSA_WITH = {  # signature algorithms that name their digest too
    "1.2.840.113549.1.1.5": "sha1",
    "1.2.840.113549.1.1.11": "sha256",
    "1.2.840.113549.1.1.12": "sha384",
    "1.2.840.113549.1.1.13": "sha512",
}
NAMES = {"2.5.4.3": "CN", "2.5.4.6": "C", "2.5.4.7": "L", "2.5.4.8": "ST",
         "2.5.4.10": "O", "2.5.4.11": "OU", "2.5.4.5": "serialNumber"}


class Bad(Exception):
    pass


# DER, as much of it as signatures need

def tlv(d, i, end=None):
    """The element at i of d: its tag, where its contents begin and end."""
    end = len(d) if end is None else end
    if i + 2 > end:
        raise Bad("the signature is cut short")
    tag, n = d[i], d[i+1]
    i += 2
    if n & 0x80:
        k = n & 0x7f
        if not 0 < k <= 4 or i + k > end:
            raise Bad("the signature has a bad length")
        n = int.from_bytes(d[i:i+k], "big")
        i += k
    if i + n > end:
        raise Bad("the signature is cut short")
    return tag, i, i + n


def items(d, beg, end):
    """The elements of a constructed value's contents: (tag, start of the
    element, start of its contents, end)."""
    out = []
    while beg < end:
        tag, cbeg, cend = tlv(d, beg, end)
        out.append((tag, beg, cbeg, cend))
        beg = cend
    return out


def expect(item, tag, what):
    if item[0] != tag:
        raise Bad(f"the signature's {what} is not where it should be")
    return item


def oid(d, item):
    expect(item, 0x06, "object identifier")
    b = d[item[2]:item[3]]
    if not b:
        raise Bad("an empty object identifier")
    parts, n = [], 0
    for x in b:
        n = n << 7 | (x & 0x7f)
        if not x & 0x80:
            parts.append(n)
            n = 0
    first = min(parts[0] // 40, 2)
    return ".".join(map(str, [first, parts[0] - 40*first] + parts[1:]))


def algorithm(d, item):
    """An AlgorithmIdentifier's object identifier."""
    expect(item, 0x30, "algorithm")
    return oid(d, items(d, item[2], item[3])[0])


def name(d, item):
    """A Name as text: "CN=Name, O=Org"."""
    out = []
    for rdn in items(d, item[2], item[3]):
        for atv in items(d, rdn[2], rdn[3]):
            k, v = items(d, atv[2], atv[3])[:2]
            raw = d[v[2]:v[3]]
            text = (raw.decode("utf-16-be", "replace") if v[0] == 0x1e else
                    raw.decode("latin-1") if v[0] == 0x14 else
                    raw.decode("utf-8", "replace"))
            key = oid(d, k)
            out.append(f"{NAMES.get(key, key)}={text}")
    return ", ".join(out)


def authenticode(d, pe, dirs, alg):
    """The file's Authenticode digest: all of it but the CheckSum, the
    certificate table's directory entry, and the table itself."""
    off, size = struct.unpack_from("<II", d, dirs + 32)
    h = hashlib.new(alg)
    h.update(d[:pe + 88])
    h.update(d[pe + 92:dirs + 32])
    h.update(d[dirs + 40:off])
    h.update(d[off + size:])
    return h.digest()


# DigestInfo up to the digest (RFC 8017, 9.2), with the algorithm's NULL
# parameter and without, which signers write either way
PREFIXES = {
    "sha1": ["3021300906052b0e03021a05000414", "301f300706052b0e03021a0414"],
    "sha256": ["3031300d060960864801650304020105000420",
               "302f300b06096086480165030402010420"],
    "sha384": ["3041300d060960864801650304020205000430",
               "303f300b06096086480165030402020430"],
    "sha512": ["3051300d060960864801650304020305000440",
               "304f300b06096086480165030402030440"],
}


def rsaverify(d, cert, alg, signed, sig):
    """Whether sig is cert's RSA PKCS#1 v1.5 signature of signed."""
    tbs = items(d, cert[2], cert[3])[0]
    fields = items(d, tbs[2], tbs[3])
    k = 1 if fields[0][0] == 0xa0 else 0  # the version, if present
    spki = fields[k + 5]
    keyalg, bits = items(d, spki[2], spki[3])[:2]
    if algorithm(d, keyalg) != OID_RSA:
        raise Bad("the signer's key is not RSA, which this cannot check")
    expect(bits, 0x03, "public key")
    rsakey = tlv(d, bits[2] + 1)
    n, e = items(d, rsakey[1], rsakey[2])[:2]
    n = int.from_bytes(d[n[2]:n[3]], "big")
    e = int.from_bytes(d[e[2]:e[3]], "big")
    size = (n.bit_length() + 7) // 8
    s = int.from_bytes(sig, "big")
    if len(sig) != size or s >= n:
        return False
    em = pow(s, e, n).to_bytes(size, "big")
    digest = hashlib.new(alg, signed).digest()
    for prefix in PREFIXES[alg]:
        t = bytes.fromhex(prefix) + digest
        if em == b"\0\1" + b"\xff"*(size - len(t) - 3) + b"\0" + t:
            return True
    return False


def signature(d, pe, dirs, cert):
    """Check the PKCS#7 signature cert (bytes of d), returning its
    signer's subject."""
    ci = expect(tlv(cert, 0), 0x30, "content info")
    ctype, content = items(cert, ci[1], ci[2])[:2]
    if oid(cert, ctype) != OID_SIGNED_DATA:
        raise Bad("the certificate is not PKCS#7 signed data")
    sd = expect(items(cert, content[2], content[3])[0], 0x30, "signed data")
    parts = items(cert, sd[2], sd[3])
    encap = expect(parts[2], 0x30, "content")
    etype, econtent = items(cert, encap[2], encap[3])[:2]
    if oid(cert, etype) != OID_SPC_INDIRECT:
        raise Bad("the signed content is not Authenticode's (SpcIndirectDataContent)")
    spc = expect(items(cert, econtent[2], econtent[3])[0], 0x30, "Authenticode content")
    dinfo = expect(items(cert, spc[2], spc[3])[1], 0x30, "digest")
    dalg, dvalue = items(cert, dinfo[2], dinfo[3])[:2]
    alg = DIGESTS.get(algorithm(cert, dalg))
    if not alg:
        raise Bad("the signed digest's algorithm is unknown")
    expect(dvalue, 0x04, "digest")
    if cert[dvalue[2]:dvalue[3]] != authenticode(d, pe, dirs, alg):
        raise Bad("the signed digest is not the file's: the signature is for "
                  "other bytes, or the file changed after signing")

    certs = [c for p in parts[3:] if p[0] == 0xa0 for c in items(cert, p[2], p[3])]
    infos = [p for p in parts[3:] if p[0] == 0x31]
    signers = items(cert, infos[-1][2], infos[-1][3]) if infos else []
    if len(signers) != 1:
        raise Bad(f"{len(signers)} signers, not one")
    si = items(cert, signers[0][2], signers[0][3])
    sid = expect(si[1], 0x30, "signer")
    issuer, serial = items(cert, sid[2], sid[3])[:2]
    signer = None
    for c in certs:
        tbs = items(cert, c[2], c[3])[0]
        fields = items(cert, tbs[2], tbs[3])
        k = 1 if fields[0][0] == 0xa0 else 0
        if (cert[fields[k][1]:fields[k][3]] == cert[serial[1]:serial[3]] and
                cert[fields[k+2][1]:fields[k+2][3]] == cert[issuer[1]:issuer[3]]):
            signer = (c, name(cert, fields[k+4]))
    if not signer:
        raise Bad("the signer's certificate is not in the signature")
    alg = DIGESTS.get(algorithm(cert, si[2]))
    attrs = expect(si[3], 0xa0, "signed attributes")
    found = {}
    for a in items(cert, attrs[2], attrs[3]):
        k, v = items(cert, a[2], a[3])[:2]
        values = items(cert, v[2], v[3])
        found[oid(cert, k)] = values[0] if values else None
    want = hashlib.new(alg or "sha256", cert[spc[2]:spc[3]]).digest()
    md = found.get(OID_MESSAGE_DIGEST)
    if not alg or not md or cert[md[2]:md[3]] != want:
        raise Bad("the signed attributes do not hold the digest of the signed content")
    ct = found.get(OID_CONTENT_TYPE)
    if not ct or oid(cert, ct) != OID_SPC_INDIRECT:
        raise Bad("the signed attributes name another content type")
    salg = algorithm(cert, si[4])
    if salg != OID_RSA and RSA_WITH.get(salg) != alg:
        raise Bad("the signature's algorithm is not RSA, which this cannot check")
    sig = expect(si[5], 0x04, "signature value")
    signed = b"\x31" + cert[attrs[1]+1:attrs[3]]  # as a SET, not [0]
    if not rsaverify(cert, signer[0], alg, signed, cert[sig[2]:sig[3]]):
        raise Bad("the signature does not verify with the signer's certificate")
    return signer[1]


def stripped(d, pe, dirs):
    """The program without its signature: cut at the certificate table,
    the table's directory entry and the CheckSum zeroed."""
    off, size = struct.unpack_from("<II", d, dirs + 32)
    r = bytearray(d[:off] if size else d)
    struct.pack_into("<I", r, pe + 88, 0)
    struct.pack_into("<II", r, dirs + 32, 0, 0)
    return bytes(r)


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
    elif off + size != len(d) or off % 8 or size < 8:
        bad.append(f"security directory {off:#x}+{size:#x} is not the file's end")
    else:
        length, revision, kind = struct.unpack_from("<IHH", d, off)
        info["signature"] = f"signed ({size} bytes)"
        if kind != 2 or revision != 0x200:
            bad.append(f"certificate type {kind}, revision {revision:#x}, "
                       "not PKCS#7 (2) and 0x200")
        elif length < 8 or length > size or size - length >= 8:
            bad.append(f"the certificate's length {length} does not fill the "
                       f"security directory's {size} bytes")
        elif args.signed:
            try:
                subject = signature(d, pe, dirs, d[off+8:off+length])
                info["signature"] = f"signed by {subject}"
                if args.signer and args.signer not in subject:
                    bad.append(f"signed by {subject}, not {args.signer}")
            except (Bad, IndexError, ValueError, struct.error) as e:
                bad.append(f"bad signature: {e}")
    if args.signed and not size:
        bad.append("unsigned: the security directory is empty")
    if args.signed and size and args.unsigned:
        before = os.path.join(args.unsigned, os.path.basename(path))
        try:
            with open(before, "rb") as f:
                u = f.read()
        except OSError as e:
            bad.append(f"no unsigned build to compare: {e}")
        else:
            program = stripped(d, pe, dirs)
            u = stripped(u, pe, dirs) if len(u) >= dirs + 40 else u
            pad = program[len(u):]
            if program[:len(u)] != u or len(pad) >= 8 or pad.strip(b"\0"):
                bad.append(f"with its signature taken out, it is not {before}: "
                           "the signer changed the program")
    return info, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--machine", choices=["x86_64", "i686"])
    ap.add_argument("--subsystem", choices=["gui", "console"])
    ap.add_argument("--signed", action="store_true",
                    help="require an Authenticode signature, and check it")
    ap.add_argument("--signer", help="text the signer's subject must hold")
    ap.add_argument("--unsigned", metavar="DIR",
                    help="the builds before signing, by the same names")
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
