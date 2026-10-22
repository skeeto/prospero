#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The macOS desktop helper, prospero-desktop (src/main_desktop_macos.cpp),
as the harness runs it, without ever touching the screen.

Every run treats Screen Recording and Accessibility as missing
(PROSPERO_TEST_DESKTOP_DENY), so each command that would capture the
screen, post input or act on a window fails before it does, and these
check that it fails at once, naming the permission and what to grant it
to. What needs no permission runs for real: hello, the window list, a
wait that times out, access. Both as one-shot commands and as --serve
frames. With the responsibility functions (tests/disclaim.py), the
helper started disclaimed answers for itself, so it names itself.

  uv run tests/desktop_macos.py --helper build/prospero-desktop
"""

import argparse
import os
import struct
import subprocess
import sys

DENY = {"PROSPERO_TEST_DESKTOP_DENY": "screen,accessibility"}
SCREEN = "Screen Recording permission is missing: grant it to "
AX = "Accessibility permission is missing: grant it to "
WHERE = " in System Settings > Privacy & Security > "


def frame(args):
    """A request frame: a u32 length, then each argument and a null."""
    body = b"".join(a.encode() + b"\0" for a in args)
    return struct.pack("<I", len(body)) + body


def readexact(f, n):
    data = b""
    while len(data) < n:
        got = f.read(n - len(data))
        if not got:
            raise EOFError("the helper ended its output")
        data += got
    return data


def reply(f):
    """One reply frame: (status, text, data)."""
    n, = struct.unpack("<I", readexact(f, 4))
    body = readexact(f, n)
    status = body[0]
    tlen, = struct.unpack("<I", body[1:5])
    return status, body[5:5 + tlen].decode("utf-8", "replace"), body[5 + tlen:]


class Serve:
    """The helper in --serve mode, over pipes, from the argv given."""

    def __init__(self, argv, env=None, timeout=30):
        self.timeout = timeout
        self.proc = subprocess.Popen(argv + ["--serve"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, env=env)

    def call(self, *args):
        self.proc.stdin.write(frame(args))
        self.proc.stdin.flush()
        return reply(self.proc.stdout)

    def close(self):
        """End its input, which ends it."""
        self.proc.stdin.close()
        return self.proc.wait(timeout=self.timeout)


class T:
    def __init__(self, helper):
        self.helper = helper
        self.checks = 0
        self.failed = 0

    def check(self, ok, what):
        self.checks += 1
        if not ok:
            self.failed += 1
            print(f"FAIL {what}")

    def run(self, *args, env=DENY, timeout=20):
        r = subprocess.run([self.helper, *args], capture_output=True, timeout=timeout,
                           env=dict(os.environ, **env))
        return r.returncode, r.stdout.decode("utf-8", "replace"), \
            r.stderr.decode("utf-8", "replace")


def windowline(line):
    """Whether a list line is well formed: id, pid, x,y, wxh, class, title."""
    f = line.split("\t")
    if len(f) != 6 or not f[0].startswith("0x") or not f[1].isdigit():
        return False
    try:
        int(f[0], 16)
        x, y = (int(v) for v in f[2].split(","))
        w, h = (int(v) for v in f[3].split("x"))
    except ValueError:
        return False
    return w >= 0 and h >= 0


def oneshot(t):
    rc, out, err = t.run()
    t.check(rc == 1 and err.startswith("usage: prospero-desktop"), f"usage: {rc} {err[:60]!r}")

    rc, out, err = t.run("access")
    lines = out.splitlines()
    t.check(rc == 0 and lines[:2] == ["screen-recording\tmissing", "accessibility\tmissing"]
            and lines[2].startswith("granted-to\t") and len(lines) == 3,
            f"access: {rc} {out!r}")
    rc, out2, err = t.run("access", "-ask")
    t.check(rc == 0 and out2 == out, f"access -ask asks nothing when denied: {out2!r}")
    grantee = lines[2].split("\t", 1)[1] if len(lines) > 2 else "?"

    rc, out, err = t.run("shot", "screen")
    t.check(rc == 1 and not out and
            err == f"prospero-desktop: {SCREEN}{grantee}{WHERE}Screen Recording, "
                   "then quit and reopen it\n", f"shot: {rc} {err!r}")
    for args in (["click", "screen", "10", "10"], ["dclick", "screen", "1", "1"],
                 ["rclick", "screen", "1", "1"], ["hover", "screen", "1", "1"],
                 ["drag", "screen", "1", "1", "40", "40"], ["wheel", "screen", "1", "1", "2"],
                 ["key", "screen", "cmd+w"], ["type", "screen", "hello"]):
        rc, out, err = t.run(*args)
        t.check(rc == 1 and err == f"prospero-desktop: {AX}{grantee}{WHERE}Accessibility\n",
                f"{args[0]}: {rc} {err!r}")

    rc, out, err = t.run("list")
    t.check(rc == 0 and all(windowline(x) for x in out.splitlines()), f"list: {rc} {out[:200]!r}")
    rc, out, err = t.run("list", "-a")
    t.check(rc == 0 and all(windowline(x) for x in out.splitlines()), f"list -a: {rc}")
    rc, out, err = t.run("wait", "-t", "0.2", "title=no window is called this, 7f3a")
    t.check(rc == 3 and "timed out waiting" in err, f"wait: {rc} {err!r}")
    rc, out, err = t.run("raise", "hwnd=0xfffffff0")
    t.check(rc == 2 and "no window matches" in err, f"raise: {rc} {err!r}")
    rc, out, err = t.run("close", "screen")
    t.check(rc == 1 and "cannot close the screen" in err, f"close: {rc} {err!r}")
    return grantee


def served(t, grantee):
    s = Serve([t.helper], env=dict(os.environ, **DENY))
    try:
        st, text, data = s.call("hello")
        t.check(st == 0 and text == "prospero-desktop 1 macos points\n", f"hello: {text!r}")
        st, text, data = s.call("list")
        t.check(st == 0 and all(windowline(x) for x in text.splitlines()), f"list: {st}")
        st, text, data = s.call("shot", "-s", "1568", "1150000", "screen")
        t.check(st == 1 and text.startswith(SCREEN + grantee) and not data,
                f"shot: {st} {text!r}")
        st, text, data = s.call("key", "screen", "cmd+shift+z")
        t.check(st == 1 and text.startswith(AX + grantee), f"key: {st} {text!r}")
        st, text, data = s.call("type", "screen", "héllo\n")
        t.check(st == 1 and text.startswith(AX), f"type: {st} {text!r}")
        st, text, data = s.call("key", "screen", "cmd+nosuchkey")
        t.check(st == 1 and text == "unknown key 'cmd+nosuchkey'", f"bad key: {text!r}")
        st, text, data = s.call("wait", "-t", "0.1", "title=nothing here, 7f3a")
        t.check(st == 3, f"wait: {st} {text!r}")
        st, text, data = s.call("run")
        t.check(st == 1 and text == "missing argument: 'program'", f"run: {text!r}")
        st, text, data = s.call("fly")
        t.check(st == 1 and text == "unknown command 'fly'", f"unknown: {text!r}")
        st, text, data = s.call("access")
        t.check(st == 0 and "screen-recording\tmissing" in text, f"access: {text!r}")
    finally:
        rc = s.close()
    t.check(rc == 0, f"serve exits at the end of its input: {rc}")


def disclaimed(t):
    """Started as its own responsible process, the helper names itself."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import disclaim
    if not disclaim.available():
        print("skip: no responsibility functions")
        return
    r = subprocess.run(disclaim.launcher() + [t.helper, "access"], capture_output=True,
                       text=True, timeout=20, env=dict(os.environ, **DENY))
    want = f"granted-to\t{os.path.realpath(t.helper)}"
    t.check(r.returncode == 0 and want in r.stdout.splitlines(),
            f"disclaimed: {r.returncode} {r.stdout!r} {r.stderr!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--helper", required=True, help="prospero-desktop")
    args = ap.parse_args()
    if sys.platform != "darwin":
        print("skip: macOS only")
        return 0
    t = T(os.path.abspath(args.helper))
    grantee = oneshot(t)
    served(t, grantee)
    disclaimed(t)
    print(f"{t.checks - t.failed}/{t.checks} desktop helper checks passed")
    return 1 if t.failed else 0


if __name__ == "__main__":
    sys.exit(main())
