#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""macOS responsibility, for the cleanup tests: ask it, and disclaim it.

Every process has a responsible process, to which TCC charges its
privacy access: an app started by LaunchServices is its own, and what
it starts answers to it, as what Terminal starts answers to Terminal.
A process spawned "disclaimed" is its own (reap.cpp). The harness
disclaims its reapers only when it answers for itself, so to test that
mode faithfully it must really be started that way, as here:

  disclaim.py exec PROGRAM ARGS...    become PROGRAM, disclaimed: same
                                      pid, descriptors and environment
  disclaim.py spawn PROGRAM ARGS...   start PROGRAM disclaimed, and exit

Both reset every signal's disposition and mask for the program. The
queries never touch anything that privacy protects.
"""

import ctypes
import os
import re
import subprocess
import sys

POSIX_SPAWN_SETSIGDEF = 0x04
POSIX_SPAWN_SETSIGMASK = 0x08
POSIX_SPAWN_SETEXEC = 0x40

_libc = None


def libc():
    global _libc
    if _libc is None:
        _libc = ctypes.CDLL(None, use_errno=True)  # libSystem, libquarantine in it
        _libc.responsibility_get_pid_responsible_for_pid.restype = ctypes.c_int
        _libc.responsibility_get_pid_responsible_for_pid.argtypes = [ctypes.c_int]
        _libc.responsibility_spawnattrs_setdisclaim.restype = ctypes.c_int
        _libc.responsibility_spawnattrs_setdisclaim.argtypes = [ctypes.c_void_p, ctypes.c_int]
        _libc.proc_pidpath.restype = ctypes.c_int
        _libc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        _libc.proc_pidinfo.restype = ctypes.c_int
        _libc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                       ctypes.c_void_p, ctypes.c_int]
    return _libc


def available():
    """Whether this is macOS with the responsibility functions."""
    if sys.platform != "darwin":
        return False
    try:
        libc()
    except (OSError, AttributeError):
        return False
    return True


def responsible(pid):
    """The pid of the process responsible for pid, or 0 if unknown."""
    r = libc().responsibility_get_pid_responsible_for_pid(pid)
    return r if r > 0 else 0


def parentid(pid):
    """The unique id a process keeps of its parent at its fork or last
    exec (p_puniqueid, reap.cpp), 1 for launchd, or None if gone."""
    buf = ctypes.create_string_buffer(56)  # PROC_PIDUNIQIDENTIFIERINFO
    n = libc().proc_pidinfo(pid, 17, 0, buf, len(buf))
    return int.from_bytes(buf.raw[24:32], "little") if n == len(buf) else None


def pidpath(pid):
    """The executable of a running process, or None."""
    buf = ctypes.create_string_buffer(4096)
    n = libc().proc_pidpath(pid, buf, len(buf))
    return os.fsdecode(buf.raw[:n]) if n > 0 else None


def cdhash(target):
    """The code directory hash of a file, or of a running process by its
    pid (an int), as codesign shows it, or None if unsigned or gone."""
    arg = str(target) if isinstance(target, int) else os.path.abspath(target)
    r = subprocess.run(["codesign", "-d", "-vvv", arg], capture_output=True, text=True)
    m = re.search(r"^CDHash=([0-9a-f]+)$", r.stderr + r.stdout, re.M)
    return m.group(1) if m else None


def launcher():
    """The command prefix that starts a program disclaimed in place."""
    return [sys.executable, "-I", os.path.abspath(__file__), "exec"]


def spawn(argv, setexec):
    c = libc()
    attr = ctypes.c_void_p()
    if c.posix_spawnattr_init(ctypes.byref(attr)):
        raise OSError("posix_spawnattr_init failed")
    if c.responsibility_spawnattrs_setdisclaim(ctypes.byref(attr), 1):
        raise OSError("responsibility_spawnattrs_setdisclaim failed")
    every = ctypes.c_uint32(0xffffffff)
    none = ctypes.c_uint32(0)
    c.posix_spawnattr_setsigdefault(ctypes.byref(attr), ctypes.byref(every))
    c.posix_spawnattr_setsigmask(ctypes.byref(attr), ctypes.byref(none))
    flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK
    if setexec:
        flags |= POSIX_SPAWN_SETEXEC
    c.posix_spawnattr_setflags(ctypes.byref(attr), ctypes.c_short(flags))
    args = (ctypes.c_char_p * (len(argv) + 1))(*[os.fsencode(a) for a in argv], None)
    env = [os.fsencode(k) + b"=" + v for k, v in os.environb.items()]
    envp = (ctypes.c_char_p * (len(env) + 1))(*env, None)
    pid = ctypes.c_int(0)
    err = c.posix_spawnp(ctypes.byref(pid), os.fsencode(argv[0]), None,
                         ctypes.byref(attr), args, envp)
    if err:
        raise OSError(err, f"cannot run {argv[0]}: {os.strerror(err)}")
    return pid.value


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("exec", "spawn"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    if not available():
        print("disclaim.py: no responsibility here", file=sys.stderr)
        return 2
    try:
        spawn(sys.argv[2:], sys.argv[1] == "exec")
    except OSError as e:
        print(f"disclaim.py: {e}", file=sys.stderr)
        return 127
    return 0


if __name__ == "__main__":
    sys.exit(main())
