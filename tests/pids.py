#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Process ids in the tests: reading them, and signaling only our own.

On 2026-10-21 a cleanup loop sent kill(-1, SIGTERM) to a whole desktop
session: its pid files had no trailing newlines, so 50 pids ran
together into one token, which zsh's atoi made -1. So a pid the tests
signal, or put in a command that kills, goes through here first: a
plain int above 1 and within a pid's range, read one per line, and,
unless it came from a spawn of the test's own, confirmed to be a
process of ours right before the signal. Nothing here ever signals 0,
-1, a negative pid, or a process group not started by the test.

  uv run tests/pids.py   (checks of its own, none of which signal)
"""

import os
import subprocess
import sys
import time

PIDMAX = (1 << 31) - 1  # pid_t, and what a 32-bit atoi reads intact


class Refused(Exception):
    """A pid not to signal."""


def checked(pid, what="pid"):
    """pid itself if it is a plain int (not a bool, nor a str) above 1
    and within PIDMAX, else Refused."""
    if type(pid) is not int or not 1 < pid <= PIDMAX:
        raise Refused(f"refusing to signal {what} {pid!r}")
    return pid


def arg(pid, what="pid"):
    """A checked pid as a command's argument, for a kill on another host."""
    return str(checked(pid, what))


def parse(text):
    """The pids in text, one per line: a line that is not a plain decimal
    number above 1 and within PIDMAX, alone, is skipped."""
    pids = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line.isascii() and line.isdigit() and len(line) <= 10:
            pid = int(line)
            if 1 < pid <= PIDMAX:
                pids.append(pid)
    return pids


def read(path):
    """The pids in a file, one per line (parse), or none if missing."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return parse(f.read())
    except OSError:
        return []


def psfield(pid, field):
    """One ps field of a process, or "" if there is no such process."""
    out = subprocess.run(["ps", "-ww", "-o", f"{field}=", "-p", arg(pid)],
                         capture_output=True, text=True,
                         env=dict(os.environ, LC_ALL="C")).stdout
    return out.strip()


def started(pid):
    """When a process started, in seconds since the epoch, to the second,
    or None if there is no such process."""
    try:
        return time.mktime(time.strptime(psfield(pid, "lstart"), "%a %b %d %H:%M:%S %Y"))
    except ValueError:
        return None


def since(t0):
    """An owned() test: a process of this user's that started at t0 (from
    time.time()) or later, so not one that had its pid before."""
    def owned(pid):
        t = started(pid)
        return (t is not None and t >= int(t0) - 1 and
                psfield(pid, "uid") == str(os.getuid()))
    return owned


def alive(pid):
    """Whether a process exists, and is not a zombie: the null signal,
    which signals nothing, to a checked pid. Not on Windows, where
    os.kill with any signal but the console's two, 0 included, is
    TerminateProcess: there a handle that may only wait asks."""
    checked(pid)
    if os.name == "nt":
        return winalive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    st = psfield(pid, "stat")
    return bool(st) and not st.startswith("Z")


def winalive(pid):
    """Windows: whether a process runs, by a handle with SYNCHRONIZE
    access alone, which can do nothing to it but wait."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k32.OpenProcess(0x00100000, False, checked(pid))  # SYNCHRONIZE
    if not h:
        return ctypes.get_last_error() == 5  # ERROR_ACCESS_DENIED: someone's
    try:
        return k32.WaitForSingleObject(h, 0) == 0x102  # WAIT_TIMEOUT
    finally:
        k32.CloseHandle(h)


def signal_ours(pid, sig, owned, what="pid"):
    """Signal one process by a pid that came from output or a file, not
    from a spawn of the test's own: only if owned(pid) confirms, right
    before the signal, that it is a process of ours."""
    checked(pid, what)
    if not owned(pid):
        raise Refused(f"refusing to signal {what} {pid}: not ours")
    os.kill(pid, sig)


def signal_group(proc, sig):
    """Signal the process group that proc, a Popen of the test's own
    started with start_new_session=True, leads: by the pid from the
    spawn, and only while proc is not reaped (its returncode unset),
    since until then no other process can have its pid, nor a group its
    id. Once reaped, the group may be gone and its id another's: then
    nothing. Returns whether it signaled. (Another thread of the test
    reaping proc meanwhile, as Popen.send_signal allows too, could still
    race this.)"""
    if proc.returncode is not None:
        return False
    os.killpg(checked(proc.pid, "group"), sig)
    return True


def selftest():
    """The checks run here, with os.kill and os.killpg replaced by a
    recorder, so that no signal goes out, whatever the result."""
    bad = [0, 1, -1, -1234, PIDMAX + 1, 1 << 32, (1 << 32) - 1, True, "1234", 12.0, None]
    for pid in bad:
        try:
            checked(pid)
        except Refused:
            continue
        raise AssertionError(f"checked({pid!r}) passed")
    assert checked(2) == 2 and checked(PIDMAX) == PIDMAX
    assert arg(4242) == "4242"
    run = "".join(str(p) for p in range(4242, 4292))  # 50 pids, no newlines
    assert parse(run) == [], parse(run)
    assert parse("4242\n4243\n") == [4242, 4243]
    assert parse("4242 4243\n-1\n0\n1\n+5\n٤٢\n 77 \n") == [77]
    assert parse(None) == [] and parse("") == []

    class Fake:
        pid = 0
        returncode = None
    sent = []
    real = os.kill, getattr(os, "killpg", None)  # none on Windows
    os.kill = lambda pid, sig: sent.append(("kill", pid, sig))
    os.killpg = lambda pgid, sig: sent.append(("killpg", pgid, sig))
    try:
        for pid in bad:
            try:
                signal_ours(pid, 9, lambda p: True)
            except Refused:
                continue
            raise AssertionError(f"signal_ours({pid!r}) passed")
        for pid in (0, 1, -1, -4242):
            Fake.pid = pid
            try:
                signal_group(Fake, 9)
            except Refused:
                continue
            raise AssertionError(f"signal_group of pid {pid} passed")
        try:
            signal_ours(4242, 9, lambda p: False)
            raise AssertionError("signal_ours of a process not ours passed")
        except Refused:
            pass
        signal_ours(4242, 9, lambda p: True)
        Fake.pid = 4243
        assert signal_group(Fake, 9)
        # Reaped, so its pid and group id may be another's by now
        for code in (0, 1, -9):
            Fake.returncode = code
            assert not signal_group(Fake, 9), f"signal_group of a reaped proc ({code})"
    finally:
        os.kill, os.killpg = real
        if not real[1]:
            del os.killpg
    assert sent == [("kill", 4242, 9), ("killpg", 4243, 9)], sent
    print("pids: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(selftest())
