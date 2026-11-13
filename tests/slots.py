#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Turns for GUI checklists on one machine.

On a desktop (macOS, Windows, Linux with a display) checklists share the
screen, the clipboard and the pointer, so one runs at a time: the
exclusive ~/.cache/prospero-gui.lock, as before slots existed, which
older checkouts take too. Headless on Linux each checklist has its own
display (tests/headless.py), so N run at once, each holding one slot
file, ~/.cache/prospero-gui.slot<k>.lock, which names its holder ("pid
label"). A run that measures, or adopts what the machine runs
(containers), takes every slot, in index order.

  uv run tests/slots.py                          self-test
  uv run tests/slots.py status                   each slot: free, or its holder
  uv run tests/slots.py hold [--all] [--label L] -- CMD...

hold runs CMD holding a slot (or all of them) and passes on its exit
status, with PROSPERO_GUI_HELD=1, so that a checklist under it takes no
slot of its own (tests/vm.py's run).

Environment: PROSPERO_GUI_SLOTS (N), PROSPERO_GUI_LABEL (the holder's
name), PROSPERO_GUI_HELD, PROSPERO_GUI_LOCKDIR (instead of ~/.cache).
"""

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

POLL = 0.25   # seconds between tries for a free slot
REMIND = 60   # seconds between "waiting" lines


def lockdir():
    return os.environ.get("PROSPERO_GUI_LOCKDIR") or \
        os.path.join(os.path.expanduser("~"), ".cache")


def headless():
    return sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
        and not os.environ.get("WAYLAND_DISPLAY")


def default():
    """Slots for this machine when PROSPERO_GUI_SLOTS doesn't say: one
    until measured (decisions.md)."""
    return 1


def count():
    try:
        n = int(os.environ.get("PROSPERO_GUI_SLOTS", ""))
    except ValueError:
        n = 0
    return n if n >= 1 else default()


def slotpath(d, k):
    return os.path.join(d, f"prospero-gui.slot{k}.lock")


class Held:
    """Locks held until closed or the process exits."""

    def __init__(self, files, what):
        self.files = files
        self.what = what

    def close(self):
        for f in self.files:
            f.close()  # closing releases its lock
        self.files = []


def take(label, whole=False, out=None):
    """Wait for this machine's turn, then hold it until exit. Returns a
    Held, or None when an ancestor holds it (PROSPERO_GUI_HELD)."""
    out = out or (lambda s: print(s, flush=True))
    if os.environ.get("PROSPERO_GUI_HELD"):
        if headless():
            out("slots held by a parent")
        return None
    if not headless():
        return desktop(lockdir(), out)
    label = os.environ.get("PROSPERO_GUI_LABEL") or label
    return slots(lockdir(), count(), label, whole, out)


def desktop(d, out):
    """The desktop's one turn: today's lock and messages, unchanged."""
    path = os.path.join(d, "prospero-gui.lock")
    os.makedirs(d, exist_ok=True)
    f = open(path, "a+")
    if sys.platform == "win32":
        import msvcrt

        def take1(wait):
            f.seek(0)
            try:
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                return True
            except OSError:
                if wait:
                    time.sleep(1)
                return False
    else:
        import fcntl

        def take1(wait):
            try:
                fcntl.flock(f, fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB))
                return True
            except BlockingIOError:
                return False
    if not take1(False):
        start = time.monotonic()
        out(f"waiting for another GUI checklist on this desktop ({path})")
        while not take1(True):
            pass
        out(f"waited {time.monotonic()-start:.0f} s for the desktop")
    return Held([f], "the desktop")


def trylock(f):
    import fcntl
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def claim(f, label):
    f.seek(0)
    f.truncate()
    f.write(f"{os.getpid()} {label}\n")
    f.flush()


def holders(d, n):
    """What each slot file names: its holder, or its last one."""
    names = []
    for k in range(n):
        try:
            with open(slotpath(d, k)) as f:
                names.append(f.read().strip() or "?")
        except OSError:
            names.append("?")
    return names


def slots(d, n, label, whole, out):
    """One free slot of n, or with whole every slot, in index order (so
    that whole-machine takers never deadlock each other)."""
    os.makedirs(d, exist_ok=True)
    files = [open(slotpath(d, k), "a+") for k in range(n)]
    start = time.monotonic()
    said = None
    got = []

    def remind():
        nonlocal said
        now = time.monotonic()
        if said is None or now - said >= REMIND:
            said = now
            busy = n if not whole else n - len(got)
            out(f"waiting for a GUI checklist slot ({busy} of {n} in use: "
                + "; ".join(holders(d, n)) + ")")

    while True:
        if whole:
            k = len(got)
            if trylock(files[k]):
                got.append(k)
                if len(got) == n:
                    break
                continue
        else:
            k = next((k for k in range(n) if trylock(files[k])), None)
            if k is not None:
                got.append(k)
                break
        remind()
        time.sleep(POLL)
    for k in got:
        claim(files[k], label)
    for k in range(n):
        if k not in got:
            files[k].close()
    waited = time.monotonic() - start
    after = f" after {waited:.0f} s" if said is not None else ""
    what = f"all {n} slots" if whole else f"slot {got[0]+1} of {n}"
    out(what + after)
    return Held([files[k] for k in got], what)


def status(d, n):
    """Each slot as free or held, by whom. A shared try, let go at once,
    tells: a taker meanwhile just tries again."""
    import fcntl
    lines = []
    for k in range(n):
        path = slotpath(d, k)
        state = "free"
        if os.path.exists(path):
            with open(path, "a+") as f:
                try:
                    fcntl.flock(f, fcntl.LOCK_SH | fcntl.LOCK_NB)
                    fcntl.flock(f, fcntl.LOCK_UN)
                except BlockingIOError:
                    f.seek(0)
                    state = "held by " + (f.read().strip() or "?")
        lines.append(f"slot {k+1} of {n}: {state}")
    return lines


def hold(argv):
    whole, label, cmd = False, "slots.py hold", None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--all":
            whole = True
        elif a == "--label" and i + 1 < len(argv):
            i += 1
            label = argv[i]
        elif a == "--":
            cmd = argv[i+1:]
            break
        else:
            break
        i += 1
    if not cmd:
        print("usage: slots.py hold [--all] [--label L] -- CMD...", file=sys.stderr)
        return 2
    for sig in (signal.SIGTERM, getattr(signal, "SIGHUP", None)):
        if sig is not None:
            signal.signal(sig, lambda s, _: sys.exit(128 + s))
    held = take(label, whole)  # noqa: F841, held while CMD runs
    p = subprocess.Popen(cmd, env=dict(os.environ, PROSPERO_GUI_HELD="1"))
    try:
        return p.wait()
    finally:
        if p.poll() is None:  # our own child, not yet reaped
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()


def selftest():
    if sys.platform == "win32":
        print("slots: skipped on Windows (msvcrt, the desktop lock alone)")
        return 77
    problems = []
    lines = []
    out = lines.append

    def check(ok, what):
        if not ok:
            problems.append(what)
            print(f"FAIL: {what}", flush=True)

    with tempfile.TemporaryDirectory(prefix="prospero-slots-") as d:
        # Two slots: two holders, a third waits until one goes
        a = slots(d, 2, "a", False, out)
        b = slots(d, 2, "b", False, out)
        check(lines[-2:] == ["slot 1 of 2", "slot 2 of 2"], f"first two slots: {lines}")
        st = status(d, 2)
        check(st == [f"slot 1 of 2: held by {os.getpid()} a",
                     f"slot 2 of 2: held by {os.getpid()} b"], f"status: {st}")
        got = {}
        t = threading.Thread(target=lambda: got.update(c=slots(d, 2, "c", False, out)))
        t.start()
        time.sleep(0.5)
        check("c" not in got, "a third took a slot while both were held")
        a.close()
        t.join(30)
        check("c" in got and got["c"].what == "slot 1 of 2", f"the third's slot: {lines[-1]}")
        check(any(l.startswith("waiting for a GUI checklist slot (2 of 2 in use: ")
                  and " b" in l for l in lines), f"waiting line: {lines}")
        check(lines[-1].startswith("slot 1 of 2 after "), f"after line: {lines[-1]}")

        # The whole machine waits for every holder, then holds every slot
        t = threading.Thread(target=lambda: got.update(w=slots(d, 2, "w", True, out)))
        t.start()
        time.sleep(0.5)
        check("w" not in got, "whole took the machine while slots were held")
        b.close()
        time.sleep(0.5)
        check("w" not in got, "whole took the machine while one slot was held")
        got["c"].close()
        t.join(30)
        check("w" in got and got["w"].what == "all 2 slots", f"whole: {lines[-1]}")
        check(status(d, 2) == [f"slot {k} of 2: held by {os.getpid()} w" for k in (1, 2)],
              f"status under whole: {status(d, 2)}")
        got["w"].close()
        check(status(d, 2) == ["slot 1 of 2: free", "slot 2 of 2: free"],
              f"status after: {status(d, 2)}")

        # The desktop: one at a time, with its messages
        x = desktop(d, out)
        t = threading.Thread(target=lambda: got.update(y=desktop(d, out)))
        t.start()
        time.sleep(0.5)
        check("y" not in got, "two desktop turns at once")
        x.close()
        t.join(30)
        check("y" in got, "the desktop's second turn never came")
        check(any(l.startswith("waiting for another GUI checklist on this desktop")
                  for l in lines), f"desktop waiting line: {lines}")
        got["y"].close()

        # hold: the exit status, and no slot of its own under a holder
        env = dict(os.environ, PROSPERO_GUI_LOCKDIR=d, PROSPERO_GUI_SLOTS="1")
        env.pop("PROSPERO_GUI_HELD", None)
        me = [sys.executable, os.path.abspath(__file__)]
        r = subprocess.run(me + ["hold", "--", sys.executable, "-c", "raise SystemExit(3)"],
                           env=env, capture_output=True, text=True, timeout=60)
        check(r.returncode == 3, f"hold's exit status {r.returncode}: {r.stdout}{r.stderr}")
        inner = (f"import subprocess, sys; sys.exit(subprocess.run({me + ['hold', '--', sys.executable, '-c', 'pass']!r}, "
                 "timeout=30).returncode)")
        r = subprocess.run(me + ["hold", "--all", "--", sys.executable, "-c", inner],
                           env=env, capture_output=True, text=True, timeout=60)
        check(r.returncode == 0, f"nested hold: {r.returncode}: {r.stdout}{r.stderr}")
        r = subprocess.run(me + ["hold"], env=env, capture_output=True, text=True, timeout=60)
        check(r.returncode == 2, f"hold without a command: {r.returncode}")

    print(f"slots: {'FAIL' if problems else 'ok'}", flush=True)
    return 1 if problems else 0


def main():
    argv = sys.argv[1:]
    if not argv:
        return selftest()
    if argv[0] == "status":
        if not headless():
            print("a desktop: one checklist at a time, " +
                  os.path.join(lockdir(), "prospero-gui.lock"))
            return 0
        for line in status(lockdir(), count()):
            print(line)
        return 0
    if argv[0] == "hold":
        return hold(argv[1:])
    print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
