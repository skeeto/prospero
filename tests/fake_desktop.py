#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""A fake desktop helper for Prospero's tests: prospero-desktop --serve,
over a pretend desktop.

It speaks the helper's protocol (src/deskhelp.cpp) on standard input and
output: request frames of null-terminated arguments, reply frames of a
status byte, a text and data. The desktop is a 1280x800 screen with an
editor window (id 0x10, pid 77, at 100,50, 640x480), a menu of the same
program (a popup, skipped unless named), and the shell's window. Typing
goes into the editor's text, which shows in its title; close removes a
window; run adds one for the program, with pid 4242. Screenshots are
PNGs of the size the helper would send, scaled down to fit -s as the
real one does.

Every command is appended as a JSON line to the file FAKE_DESKTOP_LOG
names, if set, so that a test can check what reached the desktop, in
which coordinates. Commands that change how it behaves:

  type ... CRASH     exit at once, without a reply
  type ... HANG      never reply

With FAKE_DESKTOP_ANIMATE set, the screen changes between screenshots,
as a clock would, so that no two are the same.

Run as the harness would: uv run tests/fake_desktop.py --serve
"""

import json
import math
import os
import struct
import sys
import zlib

SCREEN = (1280, 800)


class Desk:
    def __init__(self):
        self.windows = [
            {"id": 0x10, "pid": 77, "rect": [100, 50, 740, 530],
             "cls": "FakeEdit", "title": "Untitled - Fake Editor"},
            {"id": 0x20, "pid": 77, "rect": [300, 200, 500, 300],
             "cls": "#32768", "title": ""},
            {"id": 0x30, "pid": 9, "rect": [0, 0, 1280, 800],
             "cls": "Progman", "title": "Program Manager"},
        ]
        self.text = ""
        self.fg = 0x10
        self.next = 0x40
        self.frame = 0

    def line(self, w):
        x0, y0, x1, y1 = w["rect"]
        return (f"0x{w['id']:08x}\t{w['pid']}\t{x0},{y0}\t{x1-x0}x{y1-y0}\t"
                f"{w['cls']}\t{w['title']}\n")

    def select(self, spec):
        """Windows a selector names, as the helper's dkfindall does."""
        if spec == "screen":
            return [{"id": 0, "pid": 0, "rect": [0, 0, *SCREEN], "cls": "", "title": "screen"}]
        if spec == "active":
            return [w for w in self.windows if w["id"] == self.fg]
        crit = {}
        for part in spec.split(","):
            k, eq, v = part.partition("=")
            if not eq or k not in ("title", "class", "pid", "hwnd"):
                crit = {"title": spec}
                break
            crit[k] = v
        out = []
        for w in self.windows:
            if "hwnd" in crit and int(crit["hwnd"], 0) != w["id"]:
                continue
            if "pid" in crit and int(crit["pid"]) != w["pid"]:
                continue
            if "class" in crit and crit["class"].lower() != w["cls"].lower():
                continue
            if "title" in crit and crit["title"].lower() not in w["title"].lower():
                continue
            if "class" not in crit and "hwnd" not in crit and w["cls"] == "#32768":
                continue
            out.append(w)
        return out

    def one(self, spec):
        m = self.select(spec)
        if not m:
            raise Fail(2, f"no window matches '{spec}'")
        if len(m) > 1:
            raise Fail(2, f"{len(m)} windows match '{spec}', narrow it down (e.g. hwnd=)")
        return m[0]


class Fail(Exception):
    def __init__(self, status, text):
        self.status = status
        self.text = text


def fit(w, h, edge, pixels):
    s = 1.0
    if edge > 0 and max(w, h) > edge:
        s = edge / max(w, h)
    if pixels > 0 and w * h * s * s > pixels:
        s = min(s, math.sqrt(pixels / (w * h)))
    if s < 1:
        return max(1, int(w * s)), max(1, int(h * s))
    return w, h


def png(w, h, blue=128):
    """A gradient: red rises to the right, green downward."""
    row = bytes(v for x in range(w) for v in (x & 255, 0, blue))
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(row)
        raw[-3*w+1::3] = bytes([y & 255]) * w

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 1)) + chunk(b"IEND", b""))


def command(d, args):
    """Run one command: (status, text, data)."""
    if not args:
        raise Fail(1, "missing argument: 'command'")
    cmd, a = args[0], args[1:]

    def ints(xs):
        try:
            return [int(x, 0) for x in xs]
        except ValueError:
            raise Fail(1, "invalid number")

    if cmd == "hello":
        return 0, "prospero-desktop 1 fake pixels\n", b""
    if cmd == "list":
        ws = d.windows if not a else d.select(a[0])
        return 0, "".join(d.line(w) for w in ws), b""
    if cmd == "shot":
        edge = pixels = 0
        if a[:1] == ["-s"]:
            edge, pixels = ints(a[1:3])
            a = a[3:]
        w = d.one(a[0] if a else "screen")
        x0, y0, x1, y1 = w["rect"]
        cw, ch = x1 - x0, y1 - y0
        iw, ih = fit(cw, ch, edge, pixels)
        blue = 128
        if os.environ.get("FAKE_DESKTOP_ANIMATE"):
            d.frame += 1
            blue = d.frame & 255
        return 0, f"0 0 {cw} {ch} {iw} {ih} 0x{w['id']:x}\n", png(iw, ih, blue)
    if cmd in ("click", "dclick", "rclick", "hover"):
        w = d.one(a[0])
        ints(a[1:3])
        d.fg = w["id"] or d.fg
        return 0, "", b""
    if cmd == "drag":
        d.one(a[0])
        ints(a[1:5])
        return 0, "", b""
    if cmd == "wheel":
        d.one(a[0])
        ints(a[1:4])
        return 0, "", b""
    if cmd == "key":
        w = d.one(a[0])
        if any(k in ("zz",) for k in a[1:]):
            raise Fail(1, f"unknown key '{a[1]}'")
        d.fg = w["id"] or d.fg
        return 0, "", b""
    if cmd == "type":
        w = d.one(a[0])
        if "CRASH" in a[1]:
            sys.exit(3)
        if "HANG" in a[1]:
            sys.stdin.read()
            sys.exit(0)
        if w["id"] == 0x10:
            d.text += a[1]
            w["title"] = d.text.strip() + " - Fake Editor"
        return 0, "", b""
    if cmd == "raise":
        d.fg = d.one(a[0])["id"]
        return 0, "", b""
    if cmd == "close":
        w = d.one(a[0])
        d.windows.remove(w)
        return 0, "", b""
    if cmd == "wait":
        gone = False
        while a and a[0].startswith("-"):
            if a[0] == "-gone":
                gone = True
                a = a[1:]
            elif a[0] == "-t":
                a = a[2:]
            else:
                a = a[1:]
        m = d.select(a[0])
        if gone:
            if m:
                raise Fail(3, f"timed out waiting for '{a[0]}'")
            return 0, "", b""
        if not m:
            raise Fail(3, f"timed out waiting for '{a[0]}'")
        return 0, "".join(d.line(w) for w in m), b""
    if cmd == "run":
        w = {"id": d.next, "pid": 4242, "rect": [200, 100, 600, 400],
             "cls": "Run", "title": os.path.basename(a[0])}
        d.next += 0x10
        d.windows.insert(0, w)
        return 0, "4242\n", b""
    raise Fail(1, f"unknown command '{cmd}'")


def main():
    if sys.argv[1:] != ["--serve"]:
        print("usage: fake_desktop.py --serve", file=sys.stderr)
        return 1
    log = os.environ.get("FAKE_DESKTOP_LOG")
    d = Desk()
    inp = sys.stdin.buffer
    out = sys.stdout.buffer
    while True:
        head = inp.read(4)
        if len(head) < 4:
            return 0
        n = struct.unpack("<I", head)[0]
        payload = inp.read(n)
        args = [x.decode("utf-8", "surrogateescape") for x in payload.split(b"\0")[:-1]]
        if log:
            with open(log, "a", encoding="utf-8") as f:
                f.write(json.dumps(args) + "\n")
        try:
            status, text, data = command(d, args)
        except Fail as e:
            status, text, data = e.status, e.text, b""
        except (IndexError, ValueError):
            status, text, data = 1, "missing argument", b""
        t = text.encode("utf-8")
        body = bytes([status]) + struct.pack("<I", len(t)) + t + data
        out.write(struct.pack("<I", len(body)) + body)
        out.flush()


if __name__ == "__main__":
    sys.exit(main())
