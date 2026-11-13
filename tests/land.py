#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The merge check: lands this checkout's HEAD on origin/master.

  uv run tests/land.py [--serial] [--slow-ok]      (in the background)

It lands the HEAD of the checkout it runs in: a worktree's branch, or the
main checkout after "git merge --ff-only BRANCH" there. master moves only
after the checks pass, so nothing is ever undone:

1. The checkout has no tracked changes (untracked files don't go to the
   VM: vm.py --tracked).
2. It takes <git common dir>/prospero-land.lock, so landings go one at a
   time, naming the holder every 60 s while it waits.
3. git fetch origin. HEAD must contain origin/master.
4. vm.py's preflight: if the VM is unavailable, exit 75, nothing changed.
5. The Mac build in this checkout's build/, with no warnings, then
   ctest -j16 beside "vm.py --default --tracked" (--serial: one after
   the other).
6. Passed: git push origin HEAD:master, never forced, then the local
   master fast-forwards, in whichever worktree has it checked out. If it
   can't (it diverged, or local changes are in the way), it says so and
   leaves it.
7. Failed: nothing has changed; exit 1 with both reports.

Exit status: 0 landed (or nothing to land), 1 a check failed or the push
was refused, 2 tracked changes or HEAD lacks origin/master, 75 the VM is
unavailable, 130 cancelled.
"""

import argparse
import fcntl
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

REMIND = 60


def say(s):
    print(s, flush=True)


def git(top, *args, check=True):
    r = subprocess.run(["git", "-C", top, *args], capture_output=True, text=True,
                       stdin=subprocess.DEVNULL)
    if check and r.returncode:
        say(f"land.py: git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
        sys.exit(1)
    return r


def out(top, *args):
    return git(top, *args).stdout.strip()


def take(path, holder):
    f = open(path, "a+")
    said = None
    start = time.monotonic()
    while True:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            now = time.monotonic()
            if said is None or now - said >= REMIND:
                said = now
                f.seek(0)
                say(f"waiting for another landing ({f.read().strip() or '?'})")
            time.sleep(1)
    if said is not None:
        say(f"waited {time.monotonic() - start:.0f} s for the landing lock")
    f.seek(0)
    f.truncate()
    f.write(holder + "\n")
    f.flush()
    return f


def stream(cmd, cwd, log, prefix=None, env=None):
    """Run cmd, its output to log (and, with prefix, here). Returns its
    exit status; a Popen kept in PROCS so a signal can end it."""
    with open(log, "w") as f:
        p = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace", env=env)
        PROCS.append(p)
        for line in p.stdout:
            f.write(line)
            f.flush()
            if prefix is not None:
                sys.stdout.write(prefix + line)
                sys.stdout.flush()
        return p.wait()


PROCS = []


def macbuild(top, logdir):
    build = os.path.join(top, "build")
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        say("mac: configuring build/ (Debug)")
        if stream(["cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug"], top,
                  os.path.join(logdir, "configure.log")):
            return ["mac: configuring failed: " + os.path.join(logdir, "configure.log")]
    log = os.path.join(logdir, "build.log")
    t0 = time.monotonic()
    rc = stream(["cmake", "--build", "build", "-j", str(os.cpu_count() or 8)], top, log)
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    warnings = [l for l in lines if "warning:" in l and "_deps/" not in l]
    problems = []
    if rc:
        errs = [l for l in lines if "error:" in l][:20] or lines[-20:]
        problems.append(f"mac: the build failed ({log}):\n  " + "\n  ".join(errs))
    if warnings:
        problems.append(f"mac: {len(warnings)} build warnings ({log}):\n  "
                        + "\n  ".join(warnings[:20]))
    if not problems:
        say(f"mac: built in {time.monotonic() - t0:.0f} s, no warnings")
    return problems


def ctest(top, logdir, report):
    log = os.path.join(logdir, "ctest.log")
    t0 = time.monotonic()
    rc = stream(["ctest", "--test-dir", "build", "-j16"], top, log)
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    tail = [l for l in lines if re.search(r"tests passed|tests failed|\*\*\*|Failed|Timeout", l)]
    say(f"ctest: exit {rc} in {time.monotonic() - t0:.0f} s ({log})")
    for l in tail[-30:]:
        say("ctest: " + l)
    if rc:
        report.append(f"ctest failed, exit {rc}:\n  " + "\n  ".join(tail[-30:]))


def vmcheck(top, logdir, report, slowok):
    log = os.path.join(logdir, "vm.log")
    cmd = [sys.executable, "-I", os.path.join(top, "tests", "vm.py"), "--default", "--tracked"]
    if slowok:
        cmd.append("--slow-ok")
    rc = stream(cmd, top, log, prefix="vm: ")
    if rc == 75:
        report.append("vm.py: the VM became unavailable (exit 75)")
    elif rc:
        with open(log, errors="replace") as f:
            lines = f.read().splitlines()
        res = [l for l in lines if l.startswith(("result:", "  ")) or ": failed" in l
               or "FAIL" in l][-40:]
        report.append(f"vm.py failed, exit {rc} ({log}):\n  " + "\n  ".join(res))
    return rc


def ffmaster(top, new):
    """Fast-forward the local master to new, where it is checked out."""
    wt = None
    cur = None
    for line in out(top, "worktree", "list", "--porcelain").splitlines() + [""]:
        if line.startswith("worktree "):
            cur = line[len("worktree "):]
        elif line == "branch refs/heads/master":
            wt = cur
    if wt:
        r = git(wt, "merge", "--ff-only", "-q", new, check=False)
        if r.returncode:
            why = (r.stderr or r.stdout).strip().splitlines()
            say(f"the local master in {wt} did not fast-forward, left as it is"
                + (f": {why[0]}" if why else ""))
        else:
            say(f"the local master (in {wt}) is at {new[:12]}")
        return
    old = git(top, "rev-parse", "-q", "--verify", "refs/heads/master", check=False).stdout.strip()
    if old and git(top, "merge-base", "--is-ancestor", old, new, check=False).returncode:
        say("the local master has diverged from origin/master, left as it is")
        return
    git(top, "update-ref", "refs/heads/master", new, *([old] if old else []))
    say(f"the local master is at {new[:12]}")


def main():
    ap = argparse.ArgumentParser(prog="land.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", action="store_true", help="ctest, then vm.py, not together")
    ap.add_argument("--slow-ok", action="store_true", help="vm.py --slow-ok")
    ap.add_argument("--checks", help=argparse.SUPPRESS)  # its own tests: sh -c CMD instead
    args = ap.parse_args()

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda s, f: sys.exit(130))

    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if r.returncode:
        say("land.py: not in a git checkout")
        return 2
    top = r.stdout.strip()
    if out(top, "status", "--porcelain", "--untracked-files=no"):
        say("land.py: the checkout has tracked changes: commit them first")
        return 2
    common = out(top, "rev-parse", "--path-format=absolute", "--git-common-dir")
    branch = git(top, "symbolic-ref", "--short", "-q", "HEAD", check=False).stdout.strip() \
        or "(detached)"
    head = out(top, "rev-parse", "HEAD")
    lock = take(os.path.join(common, "prospero-land.lock"),  # noqa: F841, held to exit
                f"pid {os.getpid()}: {top} {branch} {head[:12]} since {time.strftime('%H:%M:%S')}")
    head = out(top, "rev-parse", "HEAD")
    if out(top, "status", "--porcelain", "--untracked-files=no"):
        say("land.py: the checkout has tracked changes: commit them first")
        return 2
    git(top, "fetch", "-q", "origin")
    origin = out(top, "rev-parse", "origin/master")
    if git(top, "merge-base", "--is-ancestor", origin, head, check=False).returncode:
        say(f"land.py: HEAD ({head[:12]}) lacks origin/master ({origin[:12]}): "
            f"rebase onto origin/master first")
        return 2
    if head == origin:
        say(f"land.py: nothing to land, HEAD is origin/master ({head[:12]})")
        ffmaster(top, head)
        return 0
    n = out(top, "rev-list", "--count", f"{origin}..{head}")
    say(f"landing {branch} at {head[:12]}: {n} commits onto origin/master {origin[:12]}")

    base = os.path.join(tempfile.gettempdir(), "prospero-land")
    os.makedirs(base, exist_ok=True)
    logdir = tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=base)
    report = []
    t0 = time.monotonic()
    try:
        if args.checks:
            rc = stream(["sh", "-c", args.checks], top, os.path.join(logdir, "checks.log"),
                        prefix="checks: ")
            if rc == 75:
                say("land.py: unavailable (exit 75); nothing changed")
                return 75
            if rc:
                report.append(f"checks: exit {rc}")
        else:
            pre = [sys.executable, "-I", os.path.join(top, "tests", "vm.py"), "--preflight"]
            if args.slow_ok:
                pre.append("--slow-ok")
            rc = stream(pre, top, os.path.join(logdir, "preflight.log"), prefix="vm: ")
            if rc == 75:
                say("land.py: the VM is unavailable (above); nothing changed")
                return 75
            if rc:
                say(f"land.py: vm.py --preflight failed, exit {rc}; nothing changed")
                return 1
            report += macbuild(top, logdir)
            if not report:
                vmrc = [None]
                if args.serial:
                    ctest(top, logdir, report)
                    vmrc[0] = vmcheck(top, logdir, report, args.slow_ok)
                else:
                    t = threading.Thread(target=lambda: vmrc.__setitem__(
                        0, vmcheck(top, logdir, report, args.slow_ok)))
                    t.start()
                    ctest(top, logdir, report)
                    t.join()
                if vmrc[0] == 75 and len(report) == 1:
                    say("land.py: the VM became unavailable; nothing changed")
                    return 75
    finally:
        for p in PROCS:
            if p.poll() is None:  # our own child, unreaped: still its pid
                p.terminate()
                try:
                    p.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
    dt = time.monotonic() - t0
    if report:
        say(f"land.py: FAILED after {dt:.0f} s; nothing changed (logs: {logdir})")
        for item in report:
            say(item)
        return 1
    say(f"land.py: the checks passed in {dt:.0f} s")
    r = git(top, "push", "-q", "origin", f"{head}:refs/heads/master", check=False)
    if r.returncode:
        say("land.py: the push was refused, origin/master unchanged by this landing "
            "(did it move? rebase and land again):\n  " + (r.stderr or r.stdout).strip())
        return 1
    say(f"landed: origin/master is {head[:12]}")
    ffmaster(top, head)
    return 0


if __name__ == "__main__":
    sys.exit(main())
