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
import signal
import subprocess
import sys
import time

PIDMAX = (1 << 31) - 1  # pid_t, and what a 32-bit atoi reads intact

# The signal that kills. Windows has none: there signal_ours ignores the
# signal and terminates the process through a handle (WinProc).
KILL = getattr(signal, "SIGKILL", 9)


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
    """One ps field of a process, or "" if there is no such process
    (POSIX)."""
    out = subprocess.run(["ps", "-ww", "-o", f"{field}=", "-p", arg(pid)],
                         capture_output=True, text=True,
                         env=dict(os.environ, LC_ALL="C")).stdout
    return out.strip()


def started(pid):
    """When a process started, in seconds since the epoch, to the second
    (finer on Windows), or None if there is no such process."""
    if os.name == "nt":
        with WinProc(pid) as h:
            return h.started() if h else None
    try:
        return time.mktime(time.strptime(psfield(pid, "lstart"), "%a %b %d %H:%M:%S %Y"))
    except ValueError:
        return None


def mine(pid):
    """Whether a process is this user's: its uid, or on Windows its
    token's user."""
    if os.name == "nt":
        with WinProc(pid) as h:
            return bool(h) and h.mine()
    return psfield(pid, "uid") == str(os.getuid())


def command(pid):
    """A process's command line, or "" if there is no such process."""
    if os.name == "nt":
        with WinProc(pid) as h:
            return h.command() if h else ""
    return psfield(pid, "command")


def parent(pid):
    """A process's parent's pid, or None. Windows does not reparent, so
    there a dead parent's pid may be another process's by now: descends
    checks for that."""
    if os.name == "nt":
        with WinProc(pid) as h:
            return h.parent() if h else None
    try:
        return int(psfield(pid, "ppid"))
    except ValueError:
        return None


def descends(pid, root, depth):
    """Whether root, a process the test started and has not reaped, is
    pid's parent, or its ancestor at most depth generations up, each one
    between started no later than the one after it, so that a pid reused
    since its process died (Windows) is no link."""
    t = started(pid)
    for _ in range(depth):
        up = parent(pid)
        tu = started(up) if up and up > 1 else None
        if t is None or tu is None or tu > t + 1:
            return False
        if up == root:
            return True
        pid, t = up, tu
    return False


def since(t0):
    """An owned() test: a process of this user's that started at t0 (from
    time.time()) or later, so not one that had its pid before."""
    def owned(pid):
        t = started(pid)
        return t is not None and t >= int(t0) - 1 and mine(pid)
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
    k32 = winapi()[0]
    h = k32.OpenProcess(WinProc.SYNCHRONIZE, False, checked(pid))
    if not h:
        return ctypes.get_last_error() == 5  # ERROR_ACCESS_DENIED: someone's
    try:
        return k32.WaitForSingleObject(h, 0) == 0x102  # WAIT_TIMEOUT
    finally:
        k32.CloseHandle(h)


WINAPI = []


def winapi():
    """Windows: kernel32, advapi32 and ntdll through ctypes, declared once."""
    if WINAPI:
        return WINAPI
    import ctypes
    from ctypes import wintypes as w
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    nt = ctypes.WinDLL("ntdll")
    ptr = ctypes.POINTER
    k32.OpenProcess.restype = w.HANDLE
    k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k32.CloseHandle.argtypes = [w.HANDLE]
    k32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    k32.WaitForSingleObject.restype = w.DWORD
    k32.GetProcessTimes.argtypes = [w.HANDLE] + [ptr(w.FILETIME)] * 4
    k32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
    k32.GetCurrentProcess.restype = w.HANDLE
    adv.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ptr(w.HANDLE)]
    adv.GetTokenInformation.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                        w.DWORD, ptr(w.DWORD)]
    nt.NtQueryInformationProcess.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                             w.ULONG, ptr(w.ULONG)]
    nt.NtQueryInformationProcess.restype = ctypes.c_long
    WINAPI[:] = [k32, adv, nt]
    return WINAPI


class WinProc:
    """Windows: a handle to a process by its pid, false if it cannot be
    opened. While open it holds the pid for that process, dead or alive,
    so what is learned through it and what is done through it concern
    one process. A context manager that closes it."""

    QUERY = 0x1000  # PROCESS_QUERY_LIMITED_INFORMATION
    TERMINATE = 0x0001
    SYNCHRONIZE = 0x00100000

    def __init__(self, pid, access=QUERY):
        self.h = winapi()[0].OpenProcess(access, False, checked(pid)) or None

    def __bool__(self):
        return self.h is not None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self.h:
            winapi()[0].CloseHandle(self.h)
            self.h = None

    def started(self):
        """Its creation time, in seconds since the epoch, or None."""
        from ctypes import byref, wintypes as w
        ft = [w.FILETIME() for _ in range(4)]
        if not winapi()[0].GetProcessTimes(self.h, *(byref(f) for f in ft)):
            return None
        t = ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime
        return t / 1e7 - 11644473600  # from 1601, in 100 ns

    def mine(self):
        """Whether its token's user is this process's."""
        own = tokenuser(winapi()[0].GetCurrentProcess())
        return bool(own) and tokenuser(self.h) == own

    def command(self):
        """Its command line (ProcessCommandLineInformation, from Windows
        8.1), or ""."""
        import ctypes
        nt = winapi()[2]
        n = ctypes.c_ulong(0)
        size = 4096
        for _ in range(3):
            buf = ctypes.create_string_buffer(size)
            if nt.NtQueryInformationProcess(self.h, 60, buf, size, ctypes.byref(n)) == 0:
                # A UNICODE_STRING, its buffer after it
                length = ctypes.c_ushort.from_buffer(buf, 0).value
                at = ctypes.c_void_p.from_buffer(buf, ctypes.sizeof(ctypes.c_void_p)).value
                return ctypes.wstring_at(at, length // 2) if at else ""
            size = max(n.value, size * 2)
        return ""

    def parent(self):
        """Its parent's pid (ProcessBasicInformation), or None."""
        import ctypes
        word = ctypes.sizeof(ctypes.c_void_p)
        buf = ctypes.create_string_buffer(6 * word)
        n = ctypes.c_ulong(0)
        if winapi()[2].NtQueryInformationProcess(self.h, 0, buf, len(buf), ctypes.byref(n)):
            return None
        # ExitStatus, PebBaseAddress, AffinityMask, BasePriority,
        # UniqueProcessId, InheritedFromUniqueProcessId, a word each
        return ctypes.c_size_t.from_buffer(buf, 5 * word).value

    def terminate(self):
        if not winapi()[0].TerminateProcess(self.h, 1):
            import ctypes
            raise ctypes.WinError(ctypes.get_last_error())


def tokenuser(h):
    """Windows: the SID of the user of a process's token, as bytes, or b""."""
    import ctypes
    from ctypes import wintypes as w
    k32, adv, _ = winapi()
    tok = w.HANDLE()
    if not adv.OpenProcessToken(h, 0x0008, ctypes.byref(tok)):  # TOKEN_QUERY
        return b""
    try:
        buf = ctypes.create_string_buffer(256)
        n = w.DWORD(0)
        if not adv.GetTokenInformation(tok, 1, buf, len(buf), ctypes.byref(n)):  # TokenUser
            return b""
        sid = ctypes.c_void_p.from_buffer(buf, 0).value
        # Revision, a count of subauthorities, a 6-byte authority, then
        # the subauthorities, 4 bytes each
        count = ctypes.c_ubyte.from_address(sid + 1).value
        return ctypes.string_at(sid, 8 + 4*count)
    finally:
        k32.CloseHandle(tok)


def signal_ours(pid, sig, owned, what="pid"):
    """Signal one process by a pid that came from output or a file, not
    from a spawn of the test's own: only if owned(pid) confirms, right
    before the signal, that it is a process of ours. Windows has no
    signals: there it is TerminateProcess (sig ignored) through a handle
    opened before the check and held until the kill, so that the process
    checked is the one killed."""
    checked(pid, what)
    if os.name != "nt":
        if not owned(pid):
            raise Refused(f"refusing to signal {what} {pid}: not ours")
        os.kill(pid, sig)
        return
    h = WINOPEN(pid)
    if not h:
        raise ProcessLookupError(f"cannot open process {pid}")
    try:
        if not owned(pid):
            raise Refused(f"refusing to signal {what} {pid}: not ours")
        h.terminate()
    finally:
        h.close()


def winopen(pid):
    return WinProc(pid, WinProc.QUERY | WinProc.TERMINATE | WinProc.SYNCHRONIZE)


WINOPEN = winopen  # the selftest's recorder instead, while it runs


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

    class Handle:  # Windows: a handle that records its kill instead
        def __init__(self, pid):
            self.pid = pid

        def terminate(self):
            sent.append(("kill", self.pid, 9))

        def close(self):
            pass
    global WINOPEN
    WINOPEN = Handle
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
        WINOPEN = winopen
        if not real[1]:
            del os.killpg
    assert sent == [("kill", 4242, 9), ("killpg", 4243, 9)], sent
    # Reading this process, which signals nothing
    me = os.getpid()
    assert abs(started(me) - time.time()) < 3600 and mine(me), started(me)
    assert since(time.time() - 60)(me) and not since(time.time() + 60)(me)
    assert "pids.py" in command(me), command(me)
    assert parent(me) == os.getppid(), (parent(me), os.getppid())
    assert descends(me, os.getppid(), 1)
    print("pids: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(selftest())
