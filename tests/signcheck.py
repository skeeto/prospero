#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""tools/pecheck.py's signature checks, on a program signed here.

A small PE image made here is signed as Azure Artifact Signing signs
with skeeto/aas-sign (an Authenticode PKCS#7 signature: SHA-256, RSA),
by a key and certificate that openssl makes in a temporary directory,
never installed anywhere. pecheck must take it, and refuse each of
these: a byte of the program changed after signing; the signature
changed; another signer than the one required; a program that the
signer changed before signing; the signature cut short; signed
attributes with the wrong digest; and no signature at all.

  uv run tests/signcheck.py
"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PECHECK = os.path.join(HERE, "..", "tools", "pecheck.py")


def derlen(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def der(tag, *parts):
    body = b"".join(parts)
    return bytes([tag]) + derlen(len(body)) + body


def oid(text):
    p = [int(x) for x in text.split(".")]
    out = bytes([40*p[0] + p[1]])
    for n in p[2:]:
        enc = [n & 0x7f]
        n >>= 7
        while n:
            enc.append(0x80 | (n & 0x7f))
            n >>= 7
        out += bytes(reversed(enc))
    return der(0x06, out)


SHA256 = der(0x30, oid("2.16.840.1.101.3.4.2.1"), b"\x05\x00")


def program(code=b"\xc3"):
    """A PE32+ image for x86_64, console, stripped, with no timestamp: its
    headers, then one section holding code, 0x400 bytes in all."""
    d = bytearray(0x400)
    d[0:2] = b"MZ"
    struct.pack_into("<I", d, 0x3c, 0x40)
    d[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", d, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 0x58
    struct.pack_into("<H", d, opt, 0x20b)
    struct.pack_into("<I", d, opt + 32, 0x1000)  # section alignment
    struct.pack_into("<I", d, opt + 36, 0x200)   # file alignment
    struct.pack_into("<I", d, opt + 56, 0x2000)  # size of image
    struct.pack_into("<I", d, opt + 60, 0x200)   # size of headers
    struct.pack_into("<H", d, opt + 68, 3)       # console
    struct.pack_into("<I", d, opt + 108, 16)     # data directories
    sec = opt + 240
    d[sec:sec+8] = b".text\0\0\0"
    struct.pack_into("<IIII", d, sec + 8, 0x1000, 0x1000, 0x200, 0x200)
    d[0x200:0x200+len(code)] = code
    return bytes(d)


def authenticode(d):
    """As aas-sign hashes an unsigned image (src/pe.cpp)."""
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    entry = pe + 24 + 112 + 32
    h = hashlib.sha256()
    h.update(d[:pe + 88])
    h.update(d[pe + 92:entry])
    h.update(d[entry + 8:])
    h.update(bytes((8 - len(d) % 8) % 8))
    return h.digest()


class Signer:
    def __init__(self, dir):
        self.dir = dir
        self.openssl = shutil.which("openssl")
        if not self.openssl:
            raise SystemExit("no openssl to make a key with")
        self.key = os.path.join(dir, "key.pem")
        cert = os.path.join(dir, "cert.pem")
        self.run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", self.key,
                 "-subj", "/CN=Prospero signcheck test/O=Prospero", "-days", "30",
                 "-sha256", "-out", cert)
        self.cert = self.run("x509", "-in", cert, "-outform", "DER")

    def run(self, *args, data=None):
        r = subprocess.run([self.openssl, *args], input=data, capture_output=True)
        if r.returncode:
            raise SystemExit(f"openssl {args[0]}: {r.stderr.decode()[:400]}")
        return r.stdout

    def rsa(self, data):
        path = os.path.join(self.dir, "attributes.der")
        with open(path, "wb") as f:
            f.write(data)
        return self.run("dgst", "-sha256", "-sign", self.key, path)

    def sign(self, d, baddigest=False):
        """d with an Authenticode signature appended, as aas-sign makes."""
        digest = authenticode(d)
        image = der(0x30, b"\x03\x01\x00", b"\xa0\x04\xa2\x02\x80\x00")
        spc = der(0x30, der(0x30, oid("1.3.6.1.4.1.311.2.1.15"), image),
                  der(0x30, SHA256, der(0x04, digest)))
        inner = spc[2:] if spc[1] < 0x80 else spc[2 + (spc[1] & 0x7f):]
        md = hashlib.sha256(inner).digest()
        if baddigest:
            md = bytes(32)
        attrs = [
            der(0x30, oid("1.2.840.113549.1.9.3"), der(0x31, oid("1.3.6.1.4.1.311.2.1.4"))),
            der(0x30, oid("1.2.840.113549.1.9.4"), der(0x31, der(0x04, md))),
            der(0x30, oid("1.3.6.1.4.1.311.2.1.11"),
                der(0x31, der(0x30, oid("1.3.6.1.4.1.311.2.1.21")))),
        ]
        attrset = der(0x31, *attrs)
        sig = self.rsa(attrset)
        tbs = self.items(self.cert)[0]
        fields = self.items(tbs)
        k = 1 if fields[0][0] == 0xa0 else 0
        sid = der(0x30, fields[k+2], fields[k])
        signer = der(0x30, der(0x02, b"\x01"), sid, SHA256,
                     b"\xa0" + attrset[1:],
                     der(0x30, oid("1.2.840.113549.1.1.1"), b"\x05\x00"),
                     der(0x04, sig))
        sd = der(0x30, der(0x02, b"\x01"), der(0x31, SHA256),
                 der(0x30, oid("1.3.6.1.4.1.311.2.1.4"), der(0xa0, spc)),
                 der(0xa0, self.cert), der(0x31, signer))
        cms = der(0x30, oid("1.2.840.113549.1.7.2"), der(0xa0, sd))
        cms += bytes((8 - len(cms) % 8) % 8)
        out = bytearray(d + bytes((8 - len(d) % 8) % 8))
        off = len(out)
        out += struct.pack("<IHH", 8 + len(cms), 0x200, 2) + cms
        pe = struct.unpack_from("<I", out, 0x3c)[0]
        struct.pack_into("<II", out, pe + 24 + 112 + 32, off, 8 + len(cms))
        struct.pack_into("<I", out, pe + 88, 0x1234567)  # a CheckSum
        return bytes(out)

    @staticmethod
    def items(d):
        """The whole elements inside the element d."""
        n = d[1]
        i = 2 + (n & 0x7f if n & 0x80 else 0)
        out = []
        while i < len(d):
            m = d[i+1]
            h = 2 + (m & 0x7f if m & 0x80 else 0)
            size = int.from_bytes(d[i+2:i+h], "big") if m & 0x80 else m
            out.append(d[i:i+h+size])
            i += h + size
        return out


def main():
    dir = tempfile.mkdtemp(prefix="prospero-signcheck-")
    failed = 0
    try:
        s = Signer(dir)
        unsigned = os.path.join(dir, "unsigned")
        os.makedirs(unsigned)
        signed = os.path.join(dir, "signed")
        os.makedirs(signed)
        plain = program()
        with open(os.path.join(unsigned, "prospero.exe"), "wb") as f:
            f.write(plain)
        good = s.sign(plain)
        flip = bytearray(good)
        flip[0x200] ^= 1
        badsig = bytearray(good)
        badsig[-40] ^= 1
        cases = [
            ("signed", good, [], True, "signed by CN=Prospero signcheck test, O=Prospero"),
            ("changed after signing", bytes(flip), [], False, "is not the file's"),
            ("the signature changed", bytes(badsig), [], False, "does not verify"),
            ("another signer", good, ["--signer", "CN=Someone Else"], False,
             "not CN=Someone Else"),
            ("changed before signing", s.sign(program(b"\xcc\xc3")), [], False,
             "the signer changed the program"),
            ("cut short", good[:-16], [], False, "is not the file's end"),
            ("signed attributes", s.sign(plain, baddigest=True), [], False,
             "do not hold the digest"),
            ("unsigned", plain, [], False, "unsigned: the security directory is empty"),
        ]
        for what, data, extra, ok, says in cases:
            path = os.path.join(signed, "prospero.exe")
            with open(path, "wb") as f:
                f.write(data)
            r = subprocess.run([sys.executable, "-I", PECHECK, "--machine", "x86_64",
                                "--subsystem", "console", "--signed", "--unsigned",
                                unsigned, "--signer", "CN=Prospero signcheck", *extra,
                                path], capture_output=True, text=True)
            out = r.stdout.strip().replace(path, "EXE")
            good_ = (r.returncode == 0) == ok and says in out
            failed += not good_
            print(f"{'ok' if good_ else 'FAIL':4} {what}: {out}", flush=True)
    finally:
        shutil.rmtree(dir, ignore_errors=True)
    print(f"{8 - failed}/8 signature checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
