#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["jeepney>=0.8"]
# ///
"""The Linux desktop helper on a real Wayland desktop, through GNOME
Text Editor (GNOME) or Kate (KDE Plasma).

Run it inside the desktop session (or with its environment: WAYLAND_DISPLAY,
DBUS_SESSION_BUS_ADDRESS, XDG_RUNTIME_DIR, XDG_CURRENT_DESKTOP), only on
a desktop nobody is using: it types and clicks for real.

  uv run tests/desktop_live_wayland.py --helper build/prospero-desktop
                                       [--ask] [--portal]

--helper is a command line, split as the shell would: "build/prospero
desktop" for a build that holds the helper.

1. It asks the helper what it may do (access). Without the portal's
   approval saved, it stops, unless --ask, when the helper asks for it
   and the desktop shows its remote desktop dialog for the user to
   approve, once. Without a way to name windows (GNOME without the
   Window Calls extension), it stops too: the steps below name windows.
2. It stops if the editor is running already, so that what it starts
   is its own and nothing of yours is touched.
3. It starts the editor (run), on its own and with no session restored,
   and waits for its window by the pid run printed.
4. It types TEXT into the window, then reads the document back through
   AT-SPI (the accessibility bus, over D-Bus) and requires exactly that:
   the helper's reply alone proves nothing.
5. It clicks at the start of the text, types "X", and requires the text
   to begin with it: so a click lands where the window's coordinates
   say. The point comes from AT-SPI's extents of the text, relative to
   the window, if the toolkit gives them, else a fixed spot near the
   window's top left, below its header bar.
6. It takes a shot of the window, unscaled, and of the screen, scaled,
   and checks each PNG's size against what the helper said, and that it
   is not of one color. Both are saved to a temporary directory.
7. It closes the window (close), which asks to save; it presses the
   dialog's Discard through AT-SPI's extents and a click, or else the
   keyboard, and requires the window gone and the process ended. Only
   if the process outlives that does it get SIGTERM, by the pid run
   printed, confirmed to be the editor this run started (pids.py).

--portal sets PROSPERO_DESKTOP_EI=0, so input goes through the portal's
Notify* calls rather than libei.

Exit status: 0 if every step passed, 1 if one failed, 2 if it could not
start (no Wayland, no approval, the editor running already).
"""

import argparse
import os
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pids  # noqa: E402
from desktop_macos import Serve  # noqa: E402

from jeepney import DBusAddress, new_method_call  # noqa: E402
from jeepney.io.blocking import open_dbus_connection  # noqa: E402

TEXT = "Hello from Prospero on Wayland"


class Stop(Exception):
    """A step failed, or the test cannot start: (message, status)."""


def say(msg):
    print(msg, flush=True)


# AT-SPI, to read what the editor holds and find its dialog's button

class Atspi:
    def __init__(self):
        sess = open_dbus_connection(bus="SESSION")
        bus = DBusAddress("/org/a11y/bus", bus_name="org.a11y.Bus", interface="org.a11y.Bus")
        addr = sess.send_and_get_reply(new_method_call(bus, "GetAddress")).body[0]
        sess.close()
        self.conn = open_dbus_connection(bus=addr)

    def call(self, name, path, iface, method, sig=None, args=()):
        a = DBusAddress(path, bus_name=name, interface=iface)
        msg = new_method_call(a, method, sig, tuple(args))
        reply = self.conn.send_and_get_reply(msg, timeout=5)
        if reply.header.message_type.name == "error":
            raise Stop((f"AT-SPI {method}: {reply.body}", 1))
        return reply.body

    def prop(self, name, path, prop):
        return self.call(name, path, "org.freedesktop.DBus.Properties", "Get", "ss",
                         ("org.a11y.atspi.Accessible", prop))[0][1]

    def pid(self, name):
        return self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "GetConnectionUnixProcessID", "s", (name,))[0]

    def app(self, pid):
        root = "/org/a11y/atspi/accessible/root"
        kids = self.call("org.a11y.atspi.Registry", root, "org.a11y.atspi.Accessible",
                         "GetChildren")[0]
        for name, path in kids:
            try:
                if self.pid(name) == pid:
                    return name, path
            except Stop:
                pass
        return None

    def walk(self, node, depth=0):
        """Every object under node, depth first."""
        yield node
        if depth > 40:
            return
        name, path = node
        try:
            kids = self.call(name, path, "org.a11y.atspi.Accessible", "GetChildren")[0]
        except Stop:
            return
        for k in kids:
            yield from self.walk(tuple(k), depth + 1)

    def interfaces(self, node):
        return self.call(*node, "org.a11y.atspi.Accessible", "GetInterfaces")[0]

    def editables(self, pid):
        """Every editable text of pid's application, as (node, text): the
        document's, and any other (Kate's output pane)."""
        app = self.app(pid)
        r = []
        for node in self.walk(app) if app else ():
            try:
                if "org.a11y.atspi.EditableText" in self.interfaces(node):
                    t = self.call(*node, "org.a11y.atspi.Text", "GetText", "ii", (0, -1))[0]
                    r.append((node, t))
            except Stop:
                pass
        return r

    def holds(self, pid, want, tries=20):
        """The editable text holding exactly want, waiting a while for
        it, or None; and the texts seen last."""
        texts = []
        for _ in range(tries):
            found = self.editables(pid)
            texts = [t for _, t in found]
            for node, t in found:
                if t == want:
                    return node, texts
            time.sleep(0.25)
        return None, texts

    def find(self, pid, label):
        """Window-relative extents (x, y, w, h) of the object named label."""
        app = self.app(pid)
        for node in self.walk(app) if app else ():
            try:
                if self.prop(*node, "Name") != label:
                    continue
                return self.call(*node, "org.a11y.atspi.Component", "GetExtents", "u", (1,))[0]
            except Stop:
                continue
        return None

    def extents(self, pid, role):
        """Window-relative extents of the first object with this role."""
        app = self.app(pid)
        for node in self.walk(app) if app else ():
            try:
                if self.call(*node, "org.a11y.atspi.Accessible", "GetRoleName")[0] == role:
                    return self.call(*node, "org.a11y.atspi.Component", "GetExtents",
                                     "u", (1,))[0]
            except Stop:
                continue
        return None

    def textextents(self, node):
        """The first character's extents in the window, or None."""
        try:
            return self.call(*node, "org.a11y.atspi.Text", "GetCharacterExtents", "iu", (0, 1))
        except Stop:
            return None


def pngcheck(data, w, h, what):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise Stop((f"{what}: not a PNG", 1))
    pw, ph = struct.unpack(">II", data[16:24])
    if (pw, ph) != (w, h):
        raise Stop((f"{what}: PNG is {pw}x{ph}, the helper said {w}x{h}", 1))
    idat, pos = b"", 8
    while pos + 8 <= len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        if data[pos + 4:pos + 8] == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    if len(set(raw[1:1 + w * 3:3])) < 2 and len(set(raw)) < 3:
        raise Stop((f"{what}: one color", 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--helper", required=True)
    ap.add_argument("--ask", action="store_true")
    ap.add_argument("--portal", action="store_true")
    opt = ap.parse_args()

    if not os.environ.get("WAYLAND_DISPLAY"):
        say("SKIP: not in a Wayland session (WAYLAND_DISPLAY is not set)")
        return 2
    desktop = os.environ.get("XDG_CURRENT_DESKTOP", "")
    kde = "KDE" in desktop
    out = tempfile.mkdtemp(prefix="prospero-wl-shots-")
    # Kate, on a file that is not there, opens an empty document by it
    editor = (["kate", "--startanon", os.path.join(out, "new.txt")] if kde
              else ["gnome-text-editor", "--standalone"])
    exe = editor[0]
    env = dict(os.environ)
    env["QT_LINUX_ACCESSIBILITY_ALWAYS_ON"] = "1"  # Qt's AT-SPI, for reading back
    if opt.portal:
        env["PROSPERO_DESKTOP_EI"] = "0"
    h = Serve(shlex.split(opt.helper), env=env, timeout=30)
    started = None
    try:
        st, text, _ = h.call("hello")
        say(f"hello: {text.strip()}")
        st, text, _ = h.call("access", *(["-ask"] if opt.ask else []))
        say("access:\n  " + text.strip().replace("\n", "\n  "))
        fields = dict(line.split("\t", 1) for line in text.splitlines() if "\t" in line)
        if fields.get("remote-desktop") != "granted":
            raise Stop(("the portal's approval is missing: run with --ask and approve "
                        "the desktop's remote desktop dialog", 2))
        if fields.get("windows") != "granted":
            raise Stop((f"windows cannot be named here: {fields.get('windows-by')}", 2))
        # By the command line: comm stops at 15 bytes, short of the name
        mine = ["pgrep", "-u", str(os.getuid()), "-f", f"^(\\S*/)?{exe}( |$)"]
        if subprocess.run(mine, capture_output=True).returncode == 0:
            raise Stop((f"{exe} is running already: close it first", 2))

        # 3. Start the editor, find its window
        t0 = time.time()
        st, text, _ = h.call("run", *editor)
        if st:
            raise Stop((f"run: {text}", 1))
        started = pids.checked(int(text.strip()), "the editor's pid")
        # Kate forks, so its window is found by class; the window then
        # names the process, which must have started since (pids.since)
        cls = "org.kde.kate" if kde else "org.gnome.TextEditor"
        st, text, _ = h.call("wait", "-t", "20", f"pid={started}" if not kde else f"class={cls}")
        if st:
            raise Stop((f"wait for the window: {text}", 1))
        say(f"window: {text.strip()}")
        f = text.split("\n")[0].split("\t")
        started = pids.checked(int(f[1]), "the editor window's pid")
        if not pids.since(t0)(started):
            started = None
            raise Stop((f"the window's process {f[1]} is not one this run started", 1))
        sel = f"hwnd={f[0]}"
        time.sleep(1)  # let it settle, and its accessibility register
        st, text, _ = h.call("list", sel)  # its size once laid out
        f = text.split("\n")[0].split("\t") if not st and "\t" in text else f

        # 4. Type two lines, and read them back
        for step in (("type", sel, TEXT), ("key", sel, "enter"), ("type", sel, "second")):
            st, text, _ = h.call(*step)
            if st:
                raise Stop((f"{step[0]}: {text}", 1))
        want = TEXT + "\nsecond"
        at = Atspi()
        doc, texts = at.holds(started, want)
        if not doc:
            raise Stop((f"typed {want!r}, the editor's texts hold {texts!r}", 1))
        say(f"typed and read back: {want!r}")

        # 5. Click on the first line, right of its text, which puts the
        # caret at its end, and type X there. AT-SPI's coordinates in a
        # window are its toolkit's: inside the server's decorations (Qt
        # on KWin), so they are moved by the difference of the sizes.
        fw = int(f[3].split("x")[0])
        fh = int(f[3].split("x")[1])
        frame = at.extents(started, "frame") or (0, 0, fw, fh)
        dy = fh - frame[3]
        ext = at.textextents(doc)
        if ext and ext[2] > 0 and 0 <= ext[1] < 4000:
            y = ext[1] + max(ext[3], 14) // 2 + dy  # Kate's heights are 0
            how = "the first line by AT-SPI"
        else:
            y, how = 70, "a fixed spot"
        x = fw * 3 // 4
        st, text, _ = h.call("click", sel, str(x), str(y))
        if st:
            raise Stop((f"click: {text}", 1))
        h.call("type", sel, "X")
        want = TEXT + "X\nsecond"
        node, texts = at.holds(started, want)
        if not node:
            raise Stop((f"clicked at {x},{y} ({how}) and typed X: the editor's "
                        f"texts hold {texts!r}", 1))
        say(f"clicked at {x},{y} ({how}), typed X: {want!r}")

        # 6. Screenshots
        for target, args in ((sel, []), ("screen", ["-s", "1568", "1150000"])):
            st, text, png = h.call("shot", *args, target)
            if st:
                raise Stop((f"shot {target}: {text}", 1))
            f = text.split()
            iw, ih = int(f[4]), int(f[5])
            pngcheck(png, iw, ih, f"shot {target}")
            name = "window.png" if target == sel else "screen.png"
            with open(os.path.join(out, name), "wb") as fp:
                fp.write(png)
            say(f"shot {target}: {text.strip()} -> {os.path.join(out, name)}")

        # 7. Close without saving
        st, text, _ = h.call("close", sel)
        if st:
            raise Stop((f"close: {text}", 1))
        gone = False
        for attempt in range(3):
            time.sleep(1)
            st, text, _ = h.call("wait", "-gone", "-t", "1", sel)
            if st == 0:
                gone = True
                break
            # The question is in the window (GNOME's), or in a window of
            # its own (Kate's), whose extents AT-SPI gives relative to it
            st, text, _ = h.call("list", f"pid={started}")
            wins = [line.split("\t") for line in text.splitlines() if "\t" in line]
            other = [w for w in wins if f"hwnd={w[0]}" != sel]
            target = f"hwnd={other[0][0]}" if other else sel
            ddy = dy
            if other:
                dlg = at.extents(started, "dialog")
                ddy = int(other[0][3].split("x")[1]) - dlg[3] if dlg else 0
            ext = at.find(started, "Discard")
            if ext and ext[2] > 0:
                bx, by = ext[0] + ext[2] // 2, ext[1] + ext[3] // 2 + ddy
                st, text, _ = h.call("click", target, str(bx), str(by))
                say(f"clicked Discard at {bx},{by} in {target}: {st} {text.strip()}")
            else:
                st, text, _ = h.call("key", target, "alt+d")
                say(f"pressed alt+d for Discard in {target}: {st} {text.strip()}")
        if not gone:
            raise Stop(("the editor's window is still there after close and Discard", 1))
        say("window closed without saving")
        for _ in range(50):
            if not pids.alive(started):
                break
            time.sleep(0.1)
        if pids.alive(started):
            say("the editor outlived its window: SIGTERM to the pid run printed")
            pids.signal_ours(started, signal.SIGTERM, owned=pids.since(t0), what="the editor")
        else:
            say("the editor exited")
        started = None
        say("PASS")
        return 0
    except Stop as e:
        msg, status = e.args[0]
        say(("SKIP: " if status == 2 else "FAIL: ") + msg)
        if started and pids.alive(started):
            say(f"left the editor (pid {started}) running for you to look at and close")
        return status
    finally:
        try:
            h.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
