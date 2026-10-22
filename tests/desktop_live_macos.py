#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The macOS desktop helper on this Mac's real desktop, through TextEdit.

Run it only while you watch: it types and presses keys for real. Keep
your hands off the mouse and keyboard until it says it is done.

  uv run tests/desktop_live_macos.py [--app build-signed/Prospero.app]
                                     [--ask] [--check]

It runs Prospero.app's own helper (Contents/MacOS/prospero-desktop) as
the harness does when Prospero is started as an app: under Prospero's
reaper, spawned disclaimed (tests/disclaim.py), so macOS charges the
helper's Screen Recording and Accessibility to Prospero.app, which is
where they are granted, and not to the terminal running this. Then:

1. It checks both permissions, which prompts for nothing, and if one is
   missing, says what to grant to which app, and stops. With --ask, the
   system asks for those missing first, on Prospero.app's behalf. With
   --check, it stops after this step, having touched nothing.
2. It stops if TextEdit is running already, so that the TextEdit it
   starts is its own and nothing of yours is touched.
3. It starts TextEdit (run) with a new document, by arguments for this
   run alone: no open panel (-NSShowAppCentricOpenPanelInsteadOfUntitledFile
   NO), and no windows restored or saved (-ApplePersistenceIgnoreState
   YES).
4. It waits for the document's window and types "Hello from Prospero".
5. It takes a screenshot of the window, unscaled, and of the screen,
   scaled as for Anthropic's models, and checks that each is a PNG of
   the size the helper reported, plausible for this screen, and not of
   one color. Both are saved to a temporary directory to look at.
6. It closes the document without saving: Cmd-W, then the sheet's
   Delete (Cmd-Backspace), or, failing that, Don't Save (Cmd-D).
7. It checks the window is gone, then quits TextEdit with SIGTERM, by
   the pid run printed, confirmed first to be the TextEdit this run
   started. It signals nothing else (tests/pids.py).
8. It ends the helper (the end of its input) and the reaper (the end of
   its lifeline).

Exit status: 0 if every step passed, 1 if one failed, 2 if it could not
start (a permission missing, TextEdit running, no helper).
"""

import argparse
import os
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disclaim  # noqa: E402
import pids  # noqa: E402
from desktop_macos import frame  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT = "Hello from Prospero"
TEXTEDIT = "/TextEdit.app/Contents/MacOS/TextEdit"
ARGS = ["-NSShowAppCentricOpenPanelInsteadOfUntitledFile", "NO",
        "-ApplePersistenceIgnoreState", "YES"]


class Stop(Exception):
    """A step failed, or the test cannot start: (message, status)."""


def say(msg):
    print(msg, flush=True)


class Reaped:
    """The app's helper under the app's reaper, disclaimed, as the
    harness runs it: the reaper's lifeline on fd 3 and its status pipe
    on fd 4, the helper's input and output on pipes here."""

    def __init__(self, app, args):
        exe = os.path.join(app, "Contents", "MacOS", "prospero")
        helper = os.path.join(app, "Contents", "MacOS", "prospero-desktop")
        life_r, self.life = os.pipe()
        self.status, st_w = os.pipe()
        in_r, in_w = os.pipe()
        self.out, out_w = os.pipe()
        argv = disclaim.launcher() + [exe, "--reap-disclaimed", os.getcwd(), helper, *args]
        try:
            # The reaper takes this pid, and it is ours to wait for
            self.pid = os.posix_spawn(argv[0], argv, os.environ, file_actions=[
                (os.POSIX_SPAWN_DUP2, in_r, 0), (os.POSIX_SPAWN_DUP2, out_w, 1),
                (os.POSIX_SPAWN_DUP2, life_r, 3), (os.POSIX_SPAWN_DUP2, st_w, 4)])
        finally:
            for fd in (life_r, st_w, in_r, out_w):
                os.close(fd)
        self.inp = os.fdopen(in_w, "wb", buffering=0)
        self.buf = b""

    def read(self, n, timeout):
        deadline = time.monotonic() + timeout
        while len(self.buf) < n:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.out], [], [], left)[0]:
                raise Stop(("the helper did not answer in time", 1))
            got = os.read(self.out, 1 << 16)
            if not got:
                raise Stop(("the helper ended its output", 1))
            self.buf += got
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def readall(self, timeout):
        data = b""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.out], [], [], left)[0]:
                raise Stop(("the helper did not finish in time", 1))
            got = os.read(self.out, 1 << 16)
            if not got:
                return data
            data += got

    def call(self, *args, timeout=40):
        """One command over the frames: (status, text, data)."""
        self.inp.write(frame(args))
        n, = struct.unpack("<I", self.read(4, timeout))
        body = self.read(n, timeout)
        tlen, = struct.unpack("<I", body[1:5])
        return body[0], body[5:5 + tlen].decode("utf-8", "replace"), body[5 + tlen:]

    def close(self):
        """End the helper by the end of its input, then the reaper by
        the end of its lifeline, and wait for the reaper."""
        if self.inp:
            self.inp.close()
            self.inp = None
        if self.life is not None:
            os.close(self.life)
            self.life = None
        for _ in range(100):
            done, _ = os.waitpid(self.pid, os.WNOHANG)
            if done:
                break
            time.sleep(0.1)
        else:
            # Our own child, unreaped, so its pid is still ours
            os.kill(pids.checked(self.pid, "reaper"), signal.SIGKILL)
            os.waitpid(self.pid, 0)
        os.close(self.out)
        os.close(self.status)


def access(app, ask=False):
    """The helper's access, run once under the reaper."""
    r = Reaped(app, ["access", "-ask"] if ask else ["access"])
    try:
        r.inp.close()
        r.inp = None
        out = r.readall(20).decode("utf-8", "replace")
    finally:
        r.close()
    return dict(line.split("\t", 1) for line in out.splitlines() if "\t" in line)


def running_textedit():
    """The pids of every TextEdit running, by their executable."""
    out = subprocess.run(["ps", "-axww", "-o", "pid=,comm="], capture_output=True,
                         text=True).stdout
    found = []
    for line in out.splitlines():
        pid, _, comm = line.strip().partition(" ")
        if comm.strip().endswith(TEXTEDIT):
            found += pids.parse(pid)
    return found


def png(data):
    """(width, height) of a PNG, and whether it has two colors at
    least, or None if it is no PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    w, h = struct.unpack(">II", data[16:24])
    idat, i = b"", 8
    while i + 12 <= len(data):
        n, = struct.unpack(">I", data[i:i + 4])
        if data[i + 4:i + 8] == b"IDAT":
            idat += data[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    pixels = {raw[y * stride + 1:(y + 1) * stride] for y in range(0, h, max(1, h // 64))}
    varied = len(pixels) > 1 or len(set(next(iter(pixels)))) > 1
    return w, h, varied


def shot(r, t, what, args, outdir, name):
    st, text, data = r.call("shot", *args)
    t.step(st == 0, f"{what}: the helper answered", text)
    geo = text.split()
    if st or len(geo) != 7:
        return None
    ox, oy, w, h, iw, ih = (int(v) for v in geo[:6])
    p = png(data)
    path = os.path.join(outdir, name)
    with open(path, "wb") as f:
        f.write(data)
    t.step(p is not None and p[:2] == (iw, ih),
           f"{what}: a PNG of {iw}x{ih} for {w}x{h} points, saved as {path}", p)
    t.step(p is not None and p[2], f"{what}: not of one color")
    return w, h, iw, ih


class T:
    def __init__(self):
        self.failed = 0

    def step(self, ok, what, detail=None):
        say(("ok    " if ok else "FAIL  ") + what +
            ("" if ok or detail is None else f": {detail!r}"))
        if not ok:
            self.failed += 1
        return ok


def live(app, outdir, ask, check):
    t = T()
    t0 = time.time()

    say(f"Prospero.app: {app}")
    perms = access(app, ask)
    to = perms.get("granted-to", "?")
    t.step(to == f"Prospero.app ({app})", f"permissions are charged to {to}")
    names = {"screen-recording": "Screen Recording", "accessibility": "Accessibility"}
    missing = [k for k in names if perms.get(k) != "granted"]
    for k in names:
        say(f"      {names[k]}: {perms.get(k, '?')}")
    if missing:
        msg = [f"missing: {', '.join(names[k] for k in missing)}. For each, open System",
               "Settings > Privacy & Security > " +
               ", and > ".join(names[k] for k in missing) + ",",
               f"press +, choose {app},",
               "and switch it on (or run this again with --ask, to be asked).",
               "After granting Screen Recording, run this again: the helper starts anew."]
        raise Stop(("\n".join(msg), 2))
    if check:
        return t

    if running_textedit():
        raise Stop(("TextEdit is running: quit it first, so that this test touches "
                    "only the TextEdit it starts", 2))

    r = Reaped(app, ["--serve"])
    textedit = None
    try:
        st, text, _ = r.call("hello")
        t.step(st == 0 and text.strip() == "prospero-desktop 1 macos points", "hello", text)

        st, text, _ = r.call("run", "TextEdit", *ARGS)
        t.step(st == 0, "run TextEdit", text)
        got = pids.parse(text)
        if st or len(got) != 1:
            raise Stop(("TextEdit did not start", 1))
        pid = got[0]

        def ours(p):
            path = disclaim.pidpath(p) or ""
            return pids.since(t0)(p) and path.endswith(TEXTEDIT)
        t.step(ours(pid), f"pid {pid} is a TextEdit this run started")
        if not ours(pid):
            raise Stop(("not our TextEdit: stopping without touching it", 1))
        textedit = (pid, ours)

        st, text, _ = r.call("wait", "-t", "20", f"pid={pid}")
        lines = [x.split("\t") for x in text.splitlines()]
        wins = [x for x in lines if len(x) == 6 and int(x[1]) == pid]
        t.step(st == 0 and wins, "its window appeared", text)
        if not wins:
            raise Stop(("no TextEdit window", 1))
        win = wins[0]
        hwnd = f"hwnd={win[0]}"
        say(f"      window {win[0]}, {win[3]} at {win[2]}, titled {win[5]!r}")
        time.sleep(1)

        st, text, _ = r.call("type", hwnd, TEXT)
        t.step(st == 0, f"typed {TEXT!r}", text)
        time.sleep(0.5)

        ww, wh = (int(v) for v in win[3].split("x"))
        g = shot(r, t, "window shot", [hwnd], outdir, "window.png")
        if g:
            w, h, iw, ih = g
            t.step((w, h) == (ww, wh) and (iw, ih) == (w, h),
                   "window shot: the window's size in points, the image as large", g)
        g = shot(r, t, "screen shot", ["-s", "1568", "1150000", "screen"], outdir, "screen.png")
        if g:
            w, h, iw, ih = g
            t.step(w >= 640 and h >= 400 and iw <= 1568 and ih <= 1568 and
                   iw * ih <= 1150000 and iw <= w and ih <= h,
                   "screen shot: plausible for a screen, within the limits", g)

        st, text, _ = r.call("key", hwnd, "cmd+w")
        t.step(st == 0, "pressed Cmd-W", text)
        time.sleep(1)
        gone = False
        for chord, label in (("cmd+backspace", "Delete"), ("cmd+d", "Don't Save")):
            st, text, _ = r.call("key", hwnd, chord)
            say(f"      pressed {chord} for {label}: {st} {text.strip()}")
            # By pid, among the windows on the screen: hwnd= takes in
            # every window, which may count one closed but kept
            st, text, _ = r.call("wait", "-gone", "-t", "3", f"pid={pid}")
            if st == 0:
                gone = True
                break
        t.step(gone, "the document closed without saving")
        if not gone:
            textedit = None  # an unsaved document: leave it to you
            raise Stop(("TextEdit's window is still open: close it by hand "
                        "(Delete, or Don't Save), then quit TextEdit", 1))
        st, text, _ = r.call("list", f"pid={pid}")
        t.step(st == 0 and not text.strip(), "TextEdit shows no window", text)
    finally:
        r.close()
        if textedit:
            pid, ours = textedit
            if pids.alive(pid):
                pids.signal_ours(pid, signal.SIGTERM, ours, "TextEdit")
                for _ in range(100):
                    if not pids.alive(pid):
                        break
                    time.sleep(0.1)
            t.step(not pids.alive(pid), f"TextEdit {pid} quit")
    t.step(not running_textedit(), "no TextEdit left running")
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--app", default=os.path.join(HERE, "..", "build-signed", "Prospero.app"),
                    help="the Prospero.app whose helper to run")
    ap.add_argument("--ask", action="store_true",
                    help="let the system ask for permissions missing")
    ap.add_argument("--check", action="store_true",
                    help="check the permissions only, and touch nothing")
    args = ap.parse_args()
    if sys.platform != "darwin" or not disclaim.available():
        print("macOS only, with the responsibility functions")
        return 2
    app = os.path.realpath(args.app)
    helper = os.path.join(app, "Contents", "MacOS", "prospero-desktop")
    if not os.path.isfile(helper):
        print(f"no helper at {helper}: build Prospero.app first")
        return 2
    outdir = tempfile.mkdtemp(prefix="prospero-desktop-live-")
    try:
        t = live(app, outdir, args.ask, args.check)
    except Stop as e:
        msg, status = e.args[0]
        print(("FAIL  " if status == 1 else "") + msg)
        return status
    print(f"screenshots: {outdir}")
    print("passed" if not t.failed else f"{t.failed} steps failed")
    return 1 if t.failed else 0


if __name__ == "__main__":
    sys.exit(main())
