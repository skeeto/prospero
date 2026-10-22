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
where they are granted, and not to the terminal (or other program)
running this. Everything here that captures the screen, lists windows
with their titles, or asks Accessibility anything is the helper's or
the probe's (tests/axprobe.py), so runs that way. Before either is sent
anything that needs a permission, it is checked to answer to the
reaper, which must be Prospero.app's prospero and its own responsible
process (Reaped.vouch); if not, the test stops there. Then:

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
4. It waits for the document's window and types "Hello from Prospero",
   then reads the document back through Accessibility (tests/axprobe.py,
   run under Prospero's reaper as the helper is, so that Prospero.app's
   grant applies) and requires exactly that text: the helper's reply
   alone proves nothing, as keys sent to an application with no key
   window are lost without a word. TextEdit's text substitutions apply
   to typed keys (a lone "x" was capitalized to "X"), so the text
   begins with a capital, which leaves capitalization nothing to change.
5. It takes a screenshot of the window, unscaled, and of the screen,
   scaled as for Anthropic's models, and checks that each is a PNG of
   the size the helper reported, plausible for this screen, and not of
   one color. Both are saved to a temporary directory to look at. The
   window's shot may be up to FRAME points larger on each side than
   the window as listed, since the capture takes in the frame's edge.
6. It closes the document without saving: Cmd-W, after which it
   requires, by Accessibility, either the sheet asking to save (with
   Delete or Don't Save) or the document gone; then the sheet's Delete
   (Cmd-Backspace), or, failing that, Don't Save (Cmd-D), until no
   window of TextEdit holds a text area.
7. Only once the document is confirmed gone does it quit TextEdit, with
   SIGTERM, by the pid run printed, confirmed first to be the TextEdit
   this run started. It signals nothing else (tests/pids.py). If the
   document cannot be confirmed closed, or holds text, it leaves
   TextEdit running for you to close, and fails.
8. It ends the helper (the end of its input) and the reaper (the end of
   its lifeline).

Exit status: 0 if every step passed, 1 if one failed, 2 if it could not
start (a permission missing, TextEdit running, no helper).
"""

import argparse
import json
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
TEXT = "Hello from Prospero"  # capitalized already, so autocapitalization keeps it
TEXTEDIT = "/TextEdit.app/Contents/MacOS/TextEdit"
ARGS = ["-NSShowAppCentricOpenPanelInsteadOfUntitledFile", "NO",
        "-ApplePersistenceIgnoreState", "YES"]
AXPROBE = os.path.join(HERE, "axprobe.py")
FRAME = 3  # points of window frame a window's shot may take in, per side
SAVE = ("Delete", "Don't Save")  # the save sheet's buttons that discard


class Stop(Exception):
    """A step failed, or the test cannot start: (message, status)."""


def say(msg):
    print(msg, flush=True)


class Reaped:
    """The app's helper under the app's reaper, disclaimed, as the
    harness runs it: the reaper's lifeline on fd 3 and its status pipe
    on fd 4, the helper's input and output on pipes here. With prog,
    that program instead of the helper, answering to Prospero.app too."""

    def __init__(self, app, args, prog=None):
        exe = os.path.join(app, "Contents", "MacOS", "prospero")
        self.reaper = exe
        prog = prog or [os.path.join(app, "Contents", "MacOS", "prospero-desktop")]
        life_r, self.life = os.pipe()
        self.status, st_w = os.pipe()
        in_r, in_w = os.pipe()
        self.out, out_w = os.pipe()
        argv = disclaim.launcher() + [exe, "--reap-disclaimed", os.getcwd(), *prog, *args]
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

    def readline(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.out], [], [], left)[0]:
                raise Stop(("no line in time", 1))
            got = os.read(self.out, 1 << 16)
            if not got:
                raise Stop(("the output ended", 1))
            self.buf += got
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", "replace")

    def child(self, exe, timeout=10):
        """The pid of the reaper's child running exe, once it runs."""
        deadline = time.monotonic() + timeout
        while True:
            out = subprocess.run(["ps", "-axww", "-o", "pid=,ppid="], capture_output=True,
                                 text=True).stdout
            kids = []
            for line in out.splitlines():
                f = line.split()
                if len(f) == 2 and pids.parse(f[1]) == [self.pid]:
                    kids += [p for p in pids.parse(f[0]) if disclaim.pidpath(p) == exe]
            if len(kids) == 1:
                return kids[0]
            if time.monotonic() >= deadline:
                raise Stop((f"no single {exe} under the reaper (pid {self.pid}): {kids}", 1))
            time.sleep(0.05)

    def vouch(self, pid, what):
        """Stop unless pid, a child of this reaper, answers to it, and it
        is Prospero.app's prospero and its own responsible process: that
        is, unless macOS charges pid's permissions to Prospero.app. Call
        it before pid is sent anything that needs a permission. Returns
        the step's description."""
        parent = pids.parse(pids.psfield(pid, "ppid"))
        resp = disclaim.responsible(pid)
        rresp = disclaim.responsible(self.pid)
        path = disclaim.pidpath(resp) if resp else None
        if parent != [self.pid] or resp != self.pid or rresp != self.pid or path != self.reaper:
            raise Stop((f"{what} (pid {pid}, parent {parent}) answers to {path or '?'} "
                        f"(pid {resp}), not to Prospero.app's reaper (pid {self.pid}, "
                        f"responsible {rresp}): stopping before it touches the screen "
                        "or Accessibility, which macOS would charge to that", 1))
        return (f"{what} (pid {pid}) answers to Prospero.app's reaper (pid {self.pid}, "
                "its own responsible process)")

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


def helper(app, t):
    """The app's helper serving under the reaper, vouched for."""
    r = Reaped(app, ["--serve"])
    try:
        exe = os.path.join(app, "Contents", "MacOS", "prospero-desktop")
        t.step(True, r.vouch(r.child(exe), "the helper"))
    except BaseException:
        r.close()
        raise
    return r


def access(app, t, ask=False):
    """The helper's access, under the reaper."""
    r = helper(app, t)
    try:
        st, out, _ = r.call("access", *(["-ask"] if ask else []), timeout=120 if ask else 20)
    finally:
        r.close()
    if st:
        raise Stop((f"access failed: {out}", 1))
    return dict(line.split("\t", 1) for line in out.splitlines() if "\t" in line)


def probe(app, pid, t):
    """TextEdit as Accessibility shows it (tests/axprobe.py), read under
    the reaper, or None if it could not be read. The probe asks nothing
    until vouched for, and that failing stops the test."""
    r = Reaped(app, [str(pids.checked(pid))], prog=[sys.executable, "-I", AXPROBE])
    try:
        try:
            line = r.readline(20)
        except Stop:
            return None
        got = pids.parse(line.removeprefix("pid "))
        if not line.startswith("pid ") or len(got) != 1:
            return None  # its input closed, it asks nothing
        t.once("probe", r.vouch(got[0], "the Accessibility probe"))
        r.inp.write(b"go\n")
        r.inp.close()
        r.inp = None
        try:
            out = r.readall(20).decode("utf-8", "replace")
        except Stop:
            return None
    finally:
        r.close()
    try:
        p = json.loads(out)
    except ValueError:
        return None
    return p if p.get("trusted") else None


def documents(p):
    """The values of the text areas in its windows, sheets aside."""
    return [v for w in p["windows"] for v in w["texts"]]


def sheet(p):
    """The buttons of a save sheet, if one is shown."""
    for w in p["windows"]:
        for buttons in w["sheets"]:
            if any(b in SAVE for b in buttons):
                return buttons
    return None


def until(app, pid, t, ok, timeout):
    """Probe until ok(probe) or the timeout: the latest probe, or None."""
    deadline = time.monotonic() + timeout
    while True:
        p = probe(app, pid, t)
        if (p and ok(p)) or time.monotonic() >= deadline:
            return p
        time.sleep(0.3)


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
        self.done = set()

    def once(self, key, what):
        """A step that passed, said the first time only."""
        if key not in self.done:
            self.done.add(key)
            self.step(True, what)

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
    perms = access(app, t, ask)
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

    r = helper(app, t)
    textedit = None
    left = False
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
        t.step(st == 0, f"the helper typed {TEXT!r}", text)
        p = until(app, pid, t, lambda p: TEXT in documents(p), 3)
        if p is None:
            raise Stop(("cannot read TextEdit through Accessibility (tests/axprobe.py "
                        "under Prospero's reaper)", 1))
        t.step(documents(p) == [TEXT],
               f"the document holds {TEXT!r}, read back through Accessibility",
               documents(p))
        # Reported only: the system-wide frontmost may fail (-25204), and
        # a key window's own AXFocused read false (Focus, in the helper)
        say(f"      system-wide frontmost pid {p['frontmost']} (AXError "
            f"{p['frontmost_error']}); TextEdit's focused element "
            f"{p['focused_role']}; window main {[w['main'] for w in p['windows']]}, "
            f"focused {[w['focused'] for w in p['windows']]}, its app's focused "
            f"window {[w['focused_window'] for w in p['windows']]}")

        ww, wh = (int(v) for v in win[3].split("x"))
        g = shot(r, t, "window shot", [hwnd], outdir, "window.png")
        if g:
            w, h, iw, ih = g
            t.step(0 <= w - ww <= 2 * FRAME and 0 <= h - wh <= 2 * FRAME and
                   (iw, ih) == (w, h),
                   f"window shot: the window's {ww}x{wh} points, plus at most "
                   f"{FRAME} of frame a side, the image as large", g)
        g = shot(r, t, "screen shot", ["-s", "1568", "1150000", "screen"], outdir, "screen.png")
        if g:
            w, h, iw, ih = g
            t.step(w >= 640 and h >= 400 and iw <= 1568 and ih <= 1568 and
                   iw * ih <= 1150000 and iw <= w and ih <= h,
                   "screen shot: plausible for a screen, within the limits", g)

        st, text, _ = r.call("key", hwnd, "cmd+w")
        t.step(st == 0, "the helper pressed Cmd-W", text)
        p = until(app, pid, t, lambda p: sheet(p) or not documents(p), 5)
        asked = p and sheet(p)
        t.step(p is not None and (asked or not documents(p)),
               "Cmd-W asked to save (a sheet with Delete or Don't Save), "
               "or closed the document, by Accessibility",
               p and [w["sheets"] for w in p["windows"]])
        for chord, label in (("cmd+backspace", "Delete"), ("cmd+d", "Don't Save")):
            if not asked or label not in asked:
                continue
            st, text, _ = r.call("key", hwnd, chord)
            say(f"      pressed {chord} for {label}: {st} {text.strip()}")
            p = until(app, pid, t, lambda p: not documents(p) and not sheet(p), 3)
            if p and not documents(p) and not sheet(p):
                break
        closed = p is not None and not documents(p) and not sheet(p)
        t.step(closed, "the document closed without saving: no text area left, "
                       "by Accessibility")
        if closed:
            st, text, _ = r.call("list", f"pid={pid}")
            t.step(st == 0 and not text.strip(), "TextEdit shows no window", text)
    finally:
        r.close()
        if textedit:
            pid, ours = textedit
            # Quit only what holds no text: a document typed into, and
            # not confirmed closed, is left for you
            try:
                p = probe(app, pid, t) if pids.alive(pid) else None
            except Stop as e:
                t.step(False, e.args[0][0])
                p = None
            if pids.alive(pid) and (p is None or any(documents(p)) or sheet(p)):
                left = True
                t.step(False, f"TextEdit {pid} left running: its document may hold "
                              "text. Close it by hand (Delete, or Don't Save), then "
                              "quit TextEdit")
            elif pids.alive(pid):
                pids.signal_ours(pid, signal.SIGTERM, ours, "TextEdit")
                for _ in range(100):
                    if not pids.alive(pid):
                        break
                    time.sleep(0.1)
            if not left:
                t.step(not pids.alive(pid), f"TextEdit {pid} quit")
    if not left:
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
