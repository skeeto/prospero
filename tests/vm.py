#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""GUI checklists, builds and suites on the Ubuntu VM, from a checkout here.

  uv run tests/vm.py --mode M[,M...] | --default  [--slots N] [--timeout S]
                     [--shots] [--trace] [--keep] [--fresh] [--slow-ok]
                     [--tracked] [--out DIR] [-- GUIPY_ARGS...]
  uv run tests/vm.py --build [--fresh]   sync and build only
  uv run tests/vm.py run -- CMD...       the whole VM, in the tree as in a checkout
  uv run tests/vm.py status              slots, builds, trees, jobs, load, space, flaky log
  uv run tests/vm.py --preflight         up, at normal priority, and not busy?
  uv run tests/vm.py --selftest          no VM needed

The checkout is the one around the working directory. Its files (tracked,
and untracked but not ignored; only tracked with --tracked) go to the VM
as a tarball, and are applied by content to that checkout's tree in
/var/tmp/prospero-vm/v1, which builds in src/build (Debug, Ninja) as a
checkout does. Each mode runs as tests/gui.py there, on its own private
desktop in one of the machine's slots (tests/slots.py), several at once.
A mode whose failures are all listed in tests/vm-known.txt
(MODE<TAB>text<TAB>reason) is "known"; one that fails otherwise runs once
more, alone, and is "flaky" if it passes then. Results land in
$TMPDIR/prospero-vm/KEY/TIME-NONCE (or --out).

The same file is the agent on the VM ("python3 -I vm.py agent DIR"), so
both ends always agree. vm.py never starts or stops a VM: when the VM is
down, runs at background priority, or is busy, it says so and exits 75.

Exit status: 0 passed (known and flaky results are listed), 1 failed (a
build error, a compiler warning, or a failure that happened again on the
rerun), 2 usage, or a checkout without tests/slots.py, 75 the VM is
unavailable, 130 cancelled.

Environment: PROSPERO_VM_HOST (default 192.168.64.11), PROSPERO_VM_NAME
(its UTM name, for the priority check, default ubuntu).
"""

import argparse
import fcntl
import glob
import hashlib
import io
import json
import os
import queue
import re
import secrets
import select
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

HOST = os.environ.get("PROSPERO_VM_HOST", "192.168.64.11")
VMNAME = os.environ.get("PROSPERO_VM_NAME", "ubuntu")
BASE = "/var/tmp/prospero-vm"
ROOT = BASE + "/v1"
LANDING = "/var/tmp/prospero-vm-in-"
UVREL = ".local/share/pipx/venvs/uv/bin"   # under the VM's home: append only
MAXBYTES = 200 << 20
PASSED, FAILED, USAGE, UNAVAILABLE, CANCELLED = 0, 1, 2, 75, 130
SSHOPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
RESULTS = "vm.py results: "
MACFILES = {"cmake/patches/sdl3-clipboard.cmake", "assets/Info.plist",
            "assets/prospero.icns", "cmake/notarize.cmake"}
CORES_BUSY = 3.0       # foreign, not niced, cores over 2 s
MEM_BUSY = 3 << 30     # MemAvailable
NICE_IGNORED = 10      # processes at this nice or above are background work
KEEP_JOBS = 24 * 3600
KEEP_LANDING = 3600
TREE_IDLE = 7 * 86400
MAX_TREES = 10
MIN_FREE = 3 << 30


# ---------------------------------------------------------------------------
# Parsing a mode's output, and classifying its result (pure, self-tested)

TAG = re.compile(r"^\[[^\]\s]{1,16}\] ")
SLOT = re.compile(r"^(slot \d+ of \d+|all \d+ slots|slots held by a parent)"
                  r"(?: after (\d+(?:\.\d+)?) s)?$")
FAILRE = re.compile(r"(?:^|\s)FAIL(?::|\s|$)")
SUMMARY = re.compile(r"drive: (\d+) checks, (\d+) failed, (\d+) stale")
CHECKLIST = re.compile(r"^(?:[\w.-]+: )?checklist: .*\b\d+ checks, \d+ failed, \d+ stale$")
EXITLINE = re.compile(r"^(?:[\w.-]+: )?exit status -?\d+$")


def parsefail(line):
    """A failure line's body without its tag ("[b] "), and whether it is
    gui.py's own ("FAIL: ...") rather than a check's."""
    body = TAG.sub("", line.strip(), count=1)
    return body, body.startswith("FAIL:")


def classify(status, fails, summaries, entries, watchdog=False):
    """A mode run's result from its exit status, its FAIL lines, its
    checklist summaries ({tag: (checks, failed, stale)}) and the known
    entries' texts for its mode. Returns (result, known lines, other
    lines, matched texts), result one of passed, known, failed."""
    if status == 0 and not watchdog:
        return "passed", [], [], set()
    known, other, matched = [], [], set()
    nknown = 0
    for line in fails:
        body, own = parsefail(line)
        hit = {e for e in entries if e in line}
        if hit:
            known.append(line)
            matched |= hit
            nknown += not own
            continue
        if own:
            msg = body[5:].strip()
            if CHECKLIST.match(msg) or EXITLINE.match(msg):
                continue  # a consequence of the checks, counted below
        other.append(line)
    if watchdog:
        other.append("watchdog: no result in time")
    elif status is None:
        other.append("no exit status")
    if other or not known:
        if not other:
            other.append(f"exit status {status}, with no FAIL line")
        return "failed", known, other, matched
    if nknown:
        failed = sum(s[1] for s in summaries.values())
        stale = sum(s[2] for s in summaries.values())
        if not summaries or failed != nknown or stale:
            return "failed", known, [f"counts disagree: {nknown} known checks failed, "
                                     f"summaries {sorted(summaries.values())}"], matched
    return "known", known, [], matched


def readknown(path):
    """tests/vm-known.txt: MODE<TAB>text<TAB>reason lines."""
    entries = []
    try:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) < 2 or not parts[0] or not parts[1]:
                    print(f"{path}:{n}: not MODE<TAB>text<TAB>reason, ignored", flush=True)
                    continue
                entries.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    except FileNotFoundError:
        pass
    return entries


def order(modes, table, times, override=None):
    """Longest first by past times, else by timeout; whole-machine modes
    last."""
    def est(m):
        return -(times.get(m) or override or table[m]["timeout"])
    return (sorted([m for m in modes if not table[m]["whole"]], key=est) +
            sorted([m for m in modes if table[m]["whole"]], key=est))


def share(n, live):
    """How many of one job's modes may run at once: a fair share of n slots."""
    return max(1, n // max(1, live))


def treekey(path):
    real = os.path.realpath(path)
    name = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(real)) or "root"
    return f"{name}-{hashlib.sha256(real.encode()).hexdigest()[:10]}"


# ---------------------------------------------------------------------------
# The Mac hint: changed lines that only the Mac exercises

def appleregions(text):
    """Line numbers (1-based) inside a preprocessor branch for __APPLE__."""
    lines = text.split("\n")
    inside = set()
    stack = []  # [mentions, negated, branch is apple]
    directive = re.compile(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)")

    def cond(kind, rest):
        mentions = "__APPLE__" in rest
        neg = kind == "ifndef" or bool(re.search(r"!\s*defined\s*\(?\s*__APPLE__", rest))
        return mentions, neg
    for i, line in enumerate(lines, 1):
        m = directive.match(line)
        if m:
            kind, rest = m.groups()
            if kind in ("if", "ifdef", "ifndef"):
                mentions, neg = cond(kind, rest)
                stack.append([mentions, neg, mentions and not neg])
            elif kind == "elif" and stack:
                mentions, neg = cond(kind, rest)
                top = stack[-1]
                top[2] = (mentions and not neg) or (top[0] and top[1] and not mentions)
            elif kind == "else" and stack:
                top = stack[-1]
                top[2] = top[0] and top[1]
            elif kind == "endif" and stack:
                if stack.pop()[2]:
                    inside.add(i)
                continue
        if any(f[2] for f in stack):
            inside.add(i)
    return inside


def machint(top):
    """FILE:LINE of changed lines (against the merge base with master,
    uncommitted and untracked included) that are macOS-specific."""
    base = None
    for ref in ("master", "origin/master"):
        r = subprocess.run(["git", "-C", top, "merge-base", "HEAD", ref],
                           capture_output=True, text=True)
        if r.returncode == 0:
            base = r.stdout.strip()
            break
    if not base:
        return []
    changed = {}
    r = subprocess.run(["git", "-C", top, "diff", "-U0", "--no-color", "--no-ext-diff",
                        "--no-renames", base], capture_output=True, text=True,
                       errors="replace")
    path = None
    for line in r.stdout.split("\n"):
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and path:
            m = re.match(r"@@ -\S+ \+(\d+)(?:,(\d+))? @@", line)
            if m:
                start, n = int(m.group(1)), int(m.group(2) or 1)
                changed.setdefault(path, set()).update(range(start, start + max(n, 1)))
    r = subprocess.run(["git", "-C", top, "ls-files", "-z", "--others", "--exclude-standard"],
                       capture_output=True)
    for name in r.stdout.decode(errors="replace").split("\0"):
        if name:
            changed.setdefault(name, set()).add(0)  # every line
    hits = []
    for name, lines in sorted(changed.items()):
        base_ = os.path.basename(name)
        if name in MACFILES or "macos" in base_.lower() or base_.endswith((".m", ".mm")):
            hits.append(f"{name}:{min(lines) or 1}")
            continue
        if not base_.endswith((".c", ".cpp", ".h", ".hpp", ".cc")):
            continue
        try:
            with open(os.path.join(top, name), encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        apple = appleregions(text)
        src = text.split("\n")
        every = 0 in lines
        for i in range(1, len(src) + 1):
            if (every or i in lines) and (i in apple or re.search(r"g->mac\b", src[i - 1])):
                hits.append(f"{name}:{i}")
                break
    return hits


# ---------------------------------------------------------------------------
# The Mac side

def die(code, msg):
    print(msg, flush=True)
    sys.exit(code)


def unavailable(msg):
    die(UNAVAILABLE, "vm.py: " + msg)


def git(top, *args):
    r = subprocess.run(["git", "-C", top, *args], capture_output=True, text=True)
    if r.returncode:
        die(USAGE, f"vm.py: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def checkout():
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if r.returncode:
        die(USAGE, "vm.py: not in a git checkout")
    top = r.stdout.strip()
    head = git(top, "rev-parse", "HEAD").strip()
    subject = git(top, "log", "-1", "--format=%s").strip()
    return top, head, subject


def filelist(top, tracked):
    args = ["ls-files", "-z", "--cached"] + ([] if tracked else ["--others", "--exclude-standard"])
    out = subprocess.run(["git", "-C", top, *args], capture_output=True, check=True).stdout
    names = sorted({n for n in out.decode().split("\0") if n})
    return [n for n in names if os.path.isfile(os.path.join(top, n))]


def maketar(path, top, names):
    """The files as a tarball at path. Returns (digest, bytes): the digest
    covers names, exec bits and contents."""
    h = hashlib.sha256()
    total = 0
    sizes = []
    with tarfile.open(path, "w:gz", compresslevel=1, format=tarfile.PAX_FORMAT) as tf:
        for n in names:
            p = os.path.join(top, n)
            with open(p, "rb") as f:
                data = f.read()
            total += len(data)
            sizes.append((len(data), n))
            if total > MAXBYTES:
                big = ", ".join(f"{n} ({s >> 20} MB)" for s, n in sorted(sizes)[-3:])
                die(USAGE, f"vm.py: over {MAXBYTES >> 20} MB of files, refused; the largest: {big}")
            ex = bool(os.stat(p).st_mode & 0o100)
            ti = tarfile.TarInfo(n)
            ti.size = len(data)
            ti.mode = 0o755 if ex else 0o644
            ti.mtime = 0
            tf.addfile(ti, io.BytesIO(data))
            h.update(n.encode() + b"\0" + (b"x" if ex else b"-") + hashlib.sha256(data).digest())
    return h.hexdigest(), total


def makestage(job, top=None, names=None):
    stage = tempfile.mkdtemp(prefix="prospero-vm-")  # 0700
    if names is not None:
        job["digest"], job["bytes"] = maketar(os.path.join(stage, "files.tar.gz"), top, names)
        job["nfiles"] = len(names)
    shutil.copy(os.path.abspath(__file__), os.path.join(stage, "vm.py"))
    with open(os.path.join(stage, "job.json"), "w") as f:
        json.dump(job, f)
    return stage


def updown(host):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host, "true"],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode != 0:
        why = r.stderr.strip().splitlines()[-1:] or [f"exit {r.returncode}"]
        unavailable(f"the VM ({host}) does not answer ({why[0]}). Ask the user to start "
                    f"the {VMNAME} VM from UTM's window; vm.py never starts or stops it.")


def priority(slowok):
    """The VM's QEMU process here: below PRI 20 it was started from a
    script, at background priority."""
    if sys.platform != "darwin":
        return
    r = subprocess.run(["ps", "-axww", "-o", "pid=,pri=,args="], capture_output=True, text=True)
    name = re.compile(r"\s-name\s+" + re.escape(VMNAME) + r"(?:[\s,]|$)")
    for line in r.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and "qemu" in parts[2] and name.search(parts[2]):
            pri = int(parts[1])
            if pri < 20:
                msg = (f"the {VMNAME} VM's QEMU (pid {parts[0]}) runs at background priority "
                       f"(PRI {pri}): it was started from a script and runs about 6x slower. "
                       f"Ask the user to restart it from UTM's window.")
                if not slowok:
                    unavailable(msg)
                print("vm.py: --slow-ok: " + msg, flush=True)
            return


def upload(host, stage, nonce):
    dst = LANDING + nonce
    r = subprocess.run(["scp", "-q", "-r", "-p", *SSHOPTS, stage, f"{host}:{dst}"],
                       stdin=subprocess.DEVNULL)
    shutil.rmtree(stage, ignore_errors=True)
    if r.returncode:
        unavailable(f"copying to the VM failed (scp exit {r.returncode}): connection lost")
    return dst


def remote(host, landing):
    """Run the agent, relaying its output. Its stdin is a pipe never
    written: closing it (on SIGINT, SIGTERM, SIGHUP) cancels the job.
    Returns (exit status, the job's out directory on the VM)."""
    cmd = ["ssh", *SSHOPTS, "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4",
           host, f"python3 -I {landing}/vm.py agent {landing}"]
    if shutil.which("caffeinate"):
        cmd = ["caffeinate", "-i"] + cmd
    # Its own session: a Ctrl+C at a terminal reaches only this process,
    # which cancels by closing stdin, so the agent can clean up
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         start_new_session=True)
    cancelled = []

    def stop(sig, frame):
        if cancelled:
            return
        cancelled.append(time.monotonic())
        print(f"vm.py: {signal.Signals(sig).name}: cancelling", flush=True)
        try:
            p.stdin.close()
        except OSError:
            pass
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    outdir = None
    buf = b""
    fd = p.stdout.fileno()
    while True:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode(errors="replace")
                if line.startswith(RESULTS):
                    outdir = line[len(RESULTS):].strip()
                sys.stdout.write(line + "\n")
            sys.stdout.flush()
        if cancelled and time.monotonic() - cancelled[0] > 15 and p.poll() is None:
            print("vm.py: the agent did not finish in 15 s; leaving it", flush=True)
            p.kill()  # our own child, not yet reaped
            break
    if buf:
        sys.stdout.write(buf.decode(errors="replace") + "\n")
    rc = p.wait()
    if cancelled:
        return CANCELLED, None
    if rc == 255:
        unavailable("the connection to the VM was lost (ssh exit 255)")
    return rc, outdir


def fetch(host, outdir, local):
    os.makedirs(os.path.dirname(local), exist_ok=True)
    r = subprocess.run(["scp", "-q", "-r", *SSHOPTS, f"{host}:{outdir}", local],
                       stdin=subprocess.DEVNULL)
    if r.returncode == 0:
        print(f"results: {local}", flush=True)
    else:
        print(f"vm.py: could not copy the results from {host}:{outdir}", flush=True)


def preflight(host, slowok):
    updown(host)
    priority(slowok)


def macmain(args, extra):
    host = args.host
    nonce = secrets.token_hex(4)
    job = {"nonce": nonce, "slots": args.slots, "timeout": args.timeout, "shots": args.shots,
           "trace": args.trace, "keep": args.keep, "fresh": args.fresh, "slowok": args.slow_ok}
    if args.command == "status":
        updown(host)
        job["cmd"] = "status"
        rc, _ = remote(host, upload(host, makestage(job), nonce))
        return rc
    if args.preflight:
        preflight(host, args.slow_ok)
        job["cmd"] = "preflight"
        rc, _ = remote(host, upload(host, makestage(job), nonce))
        return rc
    top, head, subject = checkout()
    print(f"checkout: {top} at {head[:12]} {subject}", flush=True)
    if not os.path.exists(os.path.join(top, "tests", "slots.py")):
        die(USAGE, "vm.py: this checkout has no tests/slots.py: rebase onto master first")
    key = treekey(top)
    job.update(key=key, checkout=top, head=head, subject=subject)
    if args.command == "run":
        job.update(cmd="run", argv=extra)
    elif args.build:
        job["cmd"] = "build"
    else:
        job.update(cmd="modes", default=args.default, guipy=extra,
                   modes=[m for m in (args.mode or "").split(",") if m] or None)
    preflight(host, args.slow_ok)
    names = filelist(top, args.tracked)
    stage = makestage(job, top, names)
    landing = upload(host, stage, nonce)
    rc, outdir = remote(host, landing)
    if job["cmd"] == "modes" and outdir and rc in (PASSED, FAILED):
        local = args.out or os.path.join(tempfile.gettempdir(), "prospero-vm", key,
                                         time.strftime("%Y%m%d-%H%M%S") + "-" + nonce)
        fetch(host, outdir, os.path.abspath(local))
        hits = machint(top)
        if hits:
            modes = job["modes"] or []
            more = f" (+{len(hits) - 1} more)" if len(hits) > 1 else ""
            line = ("also run on the Mac: uv run tests/gui.py --prospero build/prospero --mode "
                    + (",".join(modes) if modes else "M"))
            print(f"{line}, for {hits[0]}{more}", flush=True)
    return rc


# ---------------------------------------------------------------------------
# The agent, on the VM

CANCEL = threading.Event()
OUTLOCK = threading.Lock()
UVDIR = None


class Cancelled(Exception):
    pass


def say(s=""):
    with OUTLOCK:
        try:
            sys.stdout.write(s + "\n")
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            CANCEL.set()
            try:
                sys.stdout = open(os.devnull, "w")
            except OSError:
                pass


def check():
    if CANCEL.is_set():
        raise Cancelled()


def atomic(path, text):
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def starttime(pid):
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            data = f.read()
        return int(data[data.rindex(b")") + 2:].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def environ(pid):
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            return set(f.read().split(b"\0"))
    except OSError:
        return None


def pids():
    return [int(p) for p in os.listdir("/proc") if p.isdigit()]


def fdinfopid(fd):
    try:
        with open(f"/proc/self/fdinfo/{fd}") as f:
            for line in f:
                if line.startswith("Pid:"):
                    return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return None


def cmdline(pid):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            args = f.read().rstrip(b"\0").replace(b"\0", b" ")
    except OSError:
        return "?"
    text = args.decode(errors="replace") or "?"
    return text if len(text) <= 120 else text[:117] + "..."


def sweep(marks, sig, names=None):
    """Signal every process whose environment holds all of marks (exact
    NAME=value entries). Each goes through a pidfd, with its environment
    checked again after pidfd_open and the pidfd's process still that
    pid, so no unvalidated pid is ever signaled. Returns what it hit;
    names, if given, gets each one's command line."""
    marks = {m.encode() for m in marks}
    hit = []
    me = os.getpid()
    for pid in pids():
        if pid == me:
            continue
        env = environ(pid)
        if not env or not marks <= env:
            continue
        try:
            fd = os.pidfd_open(pid)
        except OSError:
            continue
        try:
            env = environ(pid)
            if env and marks <= env and fdinfopid(fd) == pid:
                if names is not None:
                    names[pid] = cmdline(pid)
                signal.pidfd_send_signal(fd, sig)
                hit.append(pid)
        except OSError:
            pass
        finally:
            os.close(fd)
    return hit


def cleanup(marks, what):
    """TERM, then KILL, whatever still carries marks."""
    names = {}
    left = sweep(marks, signal.SIGTERM, names)
    if not left:
        return
    say(f"{what}: TERM to {len(left)} left behind: "
        + "; ".join(f"{pid} {names.get(pid, '?')}" for pid in left))
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and sweep(marks, 0):
        time.sleep(0.25)
    left = sweep(marks, signal.SIGKILL)
    if left:
        say(f"{what}: KILL to {left}")


class Lock:
    """An flock on path, taken by polling so a cancel can end the wait.
    After taking it, the path must still be that file: housekeeping may
    have evicted its directory meanwhile."""

    def __init__(self, path):
        self.path = path
        self.f = None

    def take(self, mode, waiting=None, nb=False):
        while True:
            if self.f is None:
                os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
                self.f = open(self.path, "a+")
            said = None
            while True:
                try:
                    fcntl.flock(self.f, mode | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if nb:
                        return False
                    check()
                    now = time.monotonic()
                    if waiting and (said is None or now - said >= 60):
                        said = now
                        say(waiting())
                    time.sleep(0.25)
            try:
                same = os.stat(self.path).st_ino == os.fstat(self.f.fileno()).st_ino
            except FileNotFoundError:
                same = False
            if same:
                return True
            self.close()

    def write(self, text):
        self.f.seek(0)
        self.f.truncate()
        self.f.write(text)
        self.f.flush()

    def unlock(self):
        fcntl.flock(self.f, fcntl.LOCK_UN)

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


def held(path):
    """Whether an flock is held on path, and what the file says."""
    if not os.path.exists(path):
        return False, ""
    with open(path, "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(f, fcntl.LOCK_UN)
            return False, ""
        except BlockingIOError:
            f.seek(0)
            return True, f.read().strip()


def setuproot():
    uid = os.getuid()
    for d in (BASE, ROOT, ROOT + "/trees", ROOT + "/jobs"):
        try:
            os.mkdir(d, 0o700)
        except FileExistsError:
            pass
        st = os.lstat(d)
        if not stat.S_ISDIR(st.st_mode) or st.st_uid != uid or st.st_mode & 0o077:
            say(f"vm.py: {d} is not a 0700 directory of this user's; refusing it")
            sys.exit(FAILED)


def jobs():
    """Each job directory: (nonce, its job, alive)."""
    out = []
    for d in sorted(glob.glob(ROOT + "/jobs/*")):
        try:
            with open(d + "/agent") as f:
                pid, start = map(int, f.read().split())
            alive = starttime(pid) == start
        except (OSError, ValueError):
            alive = False
        try:
            with open(d + "/job.json") as f:
                job = json.load(f)
        except (OSError, ValueError):
            job = {}
        out.append((os.path.basename(d), job, alive, d))
    return out


class Tree:
    def __init__(self, key):
        self.key = key
        self.dir = f"{ROOT}/trees/{key}"
        self.src = self.dir + "/src"
        self.gate = Lock(self.dir + "/gate")
        self.lock = Lock(self.dir + "/lock")

    def state(self):
        try:
            with open(self.dir + "/state.json") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def update(self, fn):
        lk = Lock(self.dir + "/state.lock")
        lk.take(fcntl.LOCK_EX)
        try:
            st = self.state()
            fn(st)
            atomic(self.dir + "/state.json", json.dumps(st, indent=1, sort_keys=True))
        finally:
            lk.close()


def applytree(src, tarpath):
    """Make src hold exactly the tarball's files, writing only those whose
    bytes differ (temp file, then rename) and fixing exec bits. Unlisted
    files go, except in top-level build*/. Returns (changed, deleted, same)."""
    os.makedirs(src, exist_ok=True)
    listed = set()
    changed = same = 0
    with tarfile.open(tarpath, "r:gz") as tf:
        for m in tf:
            check()
            name = m.name
            parts = name.split("/")
            if not m.isfile() or name.startswith("/") or ".." in parts or "" in parts:
                continue
            listed.add(name)
            data = tf.extractfile(m).read()
            mode = 0o755 if m.mode & 0o100 else 0o644
            path = os.path.join(src, name)
            # Every parent must be a real directory
            d = src
            for part in parts[:-1]:
                d = os.path.join(d, part)
                try:
                    st = os.lstat(d)
                    if not stat.S_ISDIR(st.st_mode):
                        os.unlink(d)
                        os.mkdir(d)
                except FileNotFoundError:
                    os.mkdir(d)
            try:
                st = os.lstat(path)
            except FileNotFoundError:
                st = None
            if st and stat.S_ISDIR(st.st_mode):
                shutil.rmtree(path)
                st = None
            if st and stat.S_ISREG(st.st_mode) and st.st_size == len(data):
                with open(path, "rb") as f:
                    if f.read() == data:
                        if stat.S_IMODE(st.st_mode) != mode:
                            os.chmod(path, mode)
                            changed += 1
                        else:
                            same += 1
                        continue
            tmp = os.path.join(os.path.dirname(path), f".vm-tmp-{os.getpid()}")
            with open(tmp, "wb") as f:
                f.write(data)
            os.chmod(tmp, mode)
            os.replace(tmp, path)
            changed += 1
    deleted = 0
    for d, dirs, files in os.walk(src, topdown=True):
        rel = os.path.relpath(d, src)
        if rel == ".":
            dirs[:] = [x for x in dirs if not x.startswith("build")]
            # top-level build* files are left too, as build*/ is
        for f in files:
            name = f if rel == "." else f"{rel}/{f}"
            if rel == "." and f.startswith("build"):
                continue
            if name not in listed:
                os.unlink(os.path.join(d, f))
                deleted += 1
        for x in list(dirs):
            if os.path.islink(os.path.join(d, x)):
                os.unlink(os.path.join(d, x))
                dirs.remove(x)
                deleted += 1
    for d, dirs, files in os.walk(src, topdown=False):
        rel = os.path.relpath(d, src)
        if rel == "." or rel.split("/")[0].startswith("build"):
            continue
        try:
            os.rmdir(d)  # only if empty
        except OSError:
            pass
    return changed, deleted, same


def childenv(job, jobdir, label):
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "PROSPERO_GUI_HELD"):
        env.pop(k, None)
    localbin = os.path.expanduser("~/.local/bin")
    path = [p for p in env.get("PATH", "").split(":")
            if p and os.path.normpath(os.path.expanduser(p)) != localbin and p != UVDIR]
    env["PATH"] = ":".join(path + [UVDIR])
    env["TMPDIR"] = jobdir + "/tmp"
    env["PROSPERO_VM_JOB"] = job["nonce"]
    env["PROSPERO_GUI_LABEL"] = label
    if job.get("slots"):
        env["PROSPERO_GUI_SLOTS"] = str(job["slots"])
    else:
        env.pop("PROSPERO_GUI_SLOTS", None)
    return env


def spawn(cmd, cwd, env):
    """A child of the main thread, in its own session, TERMed if the agent
    dies."""
    return subprocess.Popen(["setpriv", "--pdeathsig", "TERM", *cmd], cwd=cwd, env=env,
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)


def stopgroup(p):
    """TERM, then KILL, p's group: only while p is unreaped, so that its
    group id is still its own."""
    for sig, wait in ((signal.SIGTERM, 10), (signal.SIGKILL, 5)):
        if p.poll() is not None:
            return
        try:
            os.killpg(p.pid, sig)
        except OSError:
            pass
        try:
            p.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            pass


def pump(p, online, log=None):
    """p's output, line by line, to online (and log). Returns its exit
    status; a cancel ends p's group."""
    fd = p.stdout.fileno()
    buf = b""
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.5)
            if r:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for raw in lines:
                    line = raw.decode(errors="replace")
                    if log:
                        log.write(line + "\n")
                    online(line)
                if log:
                    log.flush()
            check()
        if buf:
            online(buf.decode(errors="replace"))
        return p.wait()
    except Cancelled:
        stopgroup(p)
        raise


class Sampler:
    """CPU time of processes that belong to no vm.py job and aren't niced,
    between two samples."""

    def __init__(self):
        self.clk = os.sysconf("SC_CLK_TCK")
        self.jobcache = {}
        self.last = self.read()
        self.t = time.monotonic()

    def read(self):
        out = {}
        for pid in pids():
            try:
                with open(f"/proc/{pid}/stat", "rb") as f:
                    data = f.read()
            except OSError:
                continue
            i = data.rindex(b")")
            comm = data[data.index(b"(") + 1:i].decode(errors="replace")
            f = data[i + 2:].split()
            out[pid] = (int(f[19]), int(f[11]) + int(f[12]), int(f[16]), comm)
        return out

    def isjob(self, pid, start):
        k = (pid, start)
        if k not in self.jobcache:
            env = environ(pid) or set()
            self.jobcache[k] = any(e.startswith(b"PROSPERO_VM_JOB=") for e in env)
        return self.jobcache[k]

    def sample(self):
        """(foreign cores, [(cores, comm, pid)] busiest first)."""
        now = self.read()
        t = time.monotonic()
        dt = max(t - self.t, 0.01)
        use = []
        for pid, (start, ticks, nice, comm) in now.items():
            old = self.last.get(pid)
            if not old or old[0] != start or nice >= NICE_IGNORED or pid == os.getpid():
                continue
            d = (ticks - old[1]) / self.clk / dt
            if d > 0.005 and not self.isjob(pid, start):
                use.append((d, comm, pid))
        self.last, self.t = now, t
        live = {(p, v[0]) for p, v in now.items()}
        self.jobcache = {k: v for k, v in self.jobcache.items() if k in live}
        use.sort(reverse=True)
        return sum(u[0] for u in use), use


def meminfo():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 0


def psi():
    try:
        with open("/proc/pressure/cpu") as f:
            return float(f.readline().split()[1].split("=")[1])
    except (OSError, ValueError, IndexError):
        return -1.0


def loadavg():
    return os.getloadavg()


def busy(slowok):
    """The agent's first check: other work on the VM. Returns the load."""
    s = Sampler()
    time.sleep(2)
    cores, use = s.sample()
    mem = meminfo()
    load = {"foreign_cores": round(cores, 2), "mem_available_gb": round(mem / 2**30, 1),
            "loadavg": loadavg(), "psi_some_avg10": psi()}
    top = ", ".join(f"{c} ({p}) {u:.1f}" for u, c, p in use[:4])
    why = []
    if cores > CORES_BUSY:
        why.append(f"{cores:.1f} cores in use by other work, not niced: {top}")
    if mem < MEM_BUSY:
        why.append(f"MemAvailable {mem / 2**30:.1f} GB")
    if why:
        msg = "the VM is busy: " + "; ".join(why)
        if not slowok:
            say("vm.py: " + msg + ". Wait, or ask the user; --slow-ok goes on anyway.")
            sys.exit(UNAVAILABLE)
        say("vm.py: --slow-ok: " + msg)
    return load


def loadlogger(path, stop):
    s = Sampler()
    with open(path, "a") as f:
        while not stop.wait(5):
            cores, use = s.sample()
            la = loadavg()
            top = " ".join(f"{c}:{u:.1f}" for u, c, p in use[:3])
            f.write(f"{time.strftime('%H:%M:%S')} load {la[0]:.2f} {la[1]:.2f} "
                    f"mem {meminfo() / 2**30:.1f}G psi {psi():.1f} foreign {cores:.2f} {top}\n")
            f.flush()


def buildslot(job):
    """One of two machine-wide build locks."""
    locks = [Lock(f"{ROOT}/build.{k}.lock") for k in range(2)]
    said = None
    while True:
        for lk in locks:
            if lk.take(fcntl.LOCK_EX, nb=True):
                lk.write(f"{os.getpid()} vm:{job['nonce']}:{job.get('key', '')}\n")
                for o in locks:
                    if o is not lk:
                        o.close()
                return lk
        check()
        now = time.monotonic()
        if said is None or now - said >= 60:
            said = now
            say("build: waiting for a build slot (both in use: " +
                "; ".join(held(lk.path)[1] for lk in locks) + ")")
        time.sleep(0.5)


EDGE = re.compile(r"^\[\d+/\d+\] (.*)$")


def build(tree, job, jobdir, target):
    """Configure once, then build target ("prospero" or "all"), under a
    build lock. Records each edge's warnings in state.json, so a no-op
    build still reports the warnings of objects built earlier. Returns
    (ok, warnings)."""
    src = tree.src
    bdir = src + "/build"
    env = childenv(job, jobdir, f"vm:{job['nonce']}:build")
    lk = buildslot(job)
    log = open(jobdir + "/out/build.log", "a")
    tail = []

    def keep(line):
        tail.append(line)
        del tail[:-80]
    try:
        if job.get("fresh") and os.path.isdir(bdir):
            say("build: --fresh, removing build/")
            shutil.rmtree(bdir)
        if job.get("fresh"):
            tree.update(lambda st: st.pop("warnings", None))
        if not os.path.exists(bdir + "/build.ninja"):
            say("build: configuring")
            t0 = time.monotonic()
            p = spawn(["nice", "-n", "10", "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                       "-DCMAKE_BUILD_TYPE=Debug", f"-DUV={UVDIR}/uv"], src, env)
            if pump(p, keep, log):
                say("build: configuring failed:")
                for line in tail[-40:]:
                    say("  " + line)
                return False, []
            say(f"build: configured in {time.monotonic() - t0:.0f} s")
        cmd = ["nice", "-n", "10", "cmake", "--build", "build", "-j4"]
        if target != "all":
            cmd += ["--target", target]
        t0 = time.monotonic()
        edges = {}
        cur = [None]
        steps = [0]
        failed = []

        def online(line):
            keep(line)
            m = EDGE.match(line)
            if m:
                cur[0] = m.group(1)
                edges[cur[0]] = []
                steps[0] += 1
                return
            if line.startswith("FAILED: "):
                failed.append(line)
                cur[0] = None
                return
            if failed and len(failed) < 120:
                failed.append(line)
            if cur[0] is not None:
                edges[cur[0]].append(line)
        p = spawn(cmd, src, env)
        rc = pump(p, online, log)

        def record(st):
            w = st.setdefault("warnings", {})
            for desc, lines in edges.items():
                if "_deps/" in desc:
                    continue
                hits = [l for l in lines if "warning:" in l and "_deps/" not in l]
                if hits:
                    w[desc] = lines
                else:
                    w.pop(desc, None)
            for desc in list(w):
                m = re.match(r"Building \w+ object (\S+)", desc)
                if m and not os.path.exists(os.path.join(bdir, m.group(1))):
                    del w[desc]
        tree.update(record)
        warnings = []
        for desc, lines in sorted(tree.state().get("warnings", {}).items()):
            warnings += [l for l in lines if "warning:" in l and "_deps/" not in l]
        dt = time.monotonic() - t0
        if rc:
            say(f"build: {target} failed in {dt:.0f} s:")
            for line in (failed or tail)[:80]:
                say("  " + line)
            return False, warnings
        say(f"build: {target}, {steps[0]} steps in {dt:.0f} s" if steps[0]
            else f"build: {target} up to date")
        return True, warnings
    finally:
        log.close()
        lk.close()


def prepare(job, jobdir, landing, target):
    """The checkout's tree, its files applied and built, held shared.
    Returns (tree, ok, warnings)."""
    tree = Tree(job["key"])
    others = lambda: "; ".join(f"{n} ({j.get('cmd')}: {','.join(j.get('modes') or []) or '-'})"
                               for n, j, alive, _ in jobs()
                               if alive and j.get("key") == job["key"]
                               and n != job["nonce"]) or "?"
    tree.gate.take(fcntl.LOCK_EX, lambda: f"waiting for this checkout's other job: {others()}")
    try:
        tree.lock.take(fcntl.LOCK_SH)
        st = tree.state()
        built = st.get("built") or {}
        same = st.get("applied") == job["digest"] and os.path.isdir(tree.src)
        current = (same and not job.get("fresh") and built.get("digest") == job["digest"]
                   and built.get("ok") and target in built.get("targets", []))
        if current:
            say(f"files: 0 changed, 0 deleted ({job['nfiles']} as last synced)")
            say("build: up to date")
            warnings = []
            for desc, lines in sorted(st.get("warnings", {}).items()):
                warnings += [l for l in lines if "warning:" in l and "_deps/" not in l]
            return tree, True, warnings
        tree.lock.unlock()
        tree.lock.take(fcntl.LOCK_EX,
                       lambda: f"waiting for this checkout's running job: {others()}")
        if same and not job.get("fresh"):
            say(f"files: 0 changed, 0 deleted ({job['nfiles']} as last synced)")
        else:
            t0 = time.monotonic()
            changed, deleted, nsame = applytree(tree.src, landing + "/files.tar.gz")
            tree.update(lambda st: st.update(applied=job["digest"]))
            say(f"files: {changed} changed, {deleted} deleted, {nsame} the same "
                f"({time.monotonic() - t0:.1f} s)")
        ok, warnings = build(tree, job, jobdir, target)

        def done(st):
            b = st.get("built") or {}
            same = b.get("digest") == job["digest"] and b.get("ok")
            targets = b.get("targets", []) if same else []
            st["built"] = {"digest": job["digest"], "ok": ok,
                           "targets": sorted(set(targets) | ({target, "prospero"}
                                                            if target == "all" else {target}))
                           if ok else []}
        tree.update(done)
        tree.lock.unlock()
        tree.lock.take(fcntl.LOCK_SH)  # no writer can be waiting: they need the gate
        return tree, ok, warnings
    finally:
        tree.gate.close()


def touchtree(tree):
    """Once a day, refresh every file's atime (keeping its mtime), so
    /var/tmp's aging never takes a file from a tree in use."""
    st = tree.state()
    now = time.time()
    if now - st.get("touched", 0) < 86400:
        tree.update(lambda s: s.update(used=now))
        return
    ns = time.time_ns()
    for d, dirs, files in os.walk(tree.dir):
        for f in files + dirs:
            p = os.path.join(d, f)
            try:
                os.utime(p, ns=(ns, os.lstat(p).st_mtime_ns), follow_symlinks=False)
            except OSError:
                pass
    tree.update(lambda s: s.update(used=now, touched=now))


def printwarnings(warnings):
    if warnings:
        say(f"build: {len(warnings)} warnings (GCC), which fail the job:")
        for w in warnings[:40]:
            say("  " + w)


class Mode:
    def __init__(self, name, timeout, whole, rerun=False):
        self.name, self.timeout, self.whole, self.rerun = name, timeout, whole, rerun
        self.fails, self.summ = [], {}
        self.slot = None
        self.wait = 0.0
        self.waiting = False
        self.t0 = self.tslot = self.tend = self.clock = None
        self.status = None
        self.watchdog = False
        self.kill_at = None
        self.shown = 0
        self.p = None
        self.exited = None
        self.swept = False


def runmodes(job, jobdir, tree, warnings, load):
    src, out = tree.src, jobdir + "/out"
    r = subprocess.run(["python3", "-I", "tests/gui.py", "--modes-json"], cwd=src,
                       capture_output=True, text=True, stdin=subprocess.DEVNULL)
    try:
        table = json.loads(r.stdout)
    except ValueError:
        say("vm.py: tests/gui.py --modes-json failed: rebase onto master first\n" + r.stderr)
        return USAGE
    if job.get("default"):
        names = [m for m, v in table.items() if v.get("default")]
    else:
        names = job.get("modes") or []
    bad = [m for m in names if m not in table]
    if bad or not names:
        say(f"vm.py: unknown mode {','.join(bad) or '(none)'}; modes: {','.join(table)}")
        return USAGE
    names = list(dict.fromkeys(names))
    env0 = childenv(job, jobdir, f"vm:{job['nonce']}")
    r = subprocess.run(["python3", "-I", "tests/slots.py", "status"], cwd=src, env=env0,
                       capture_output=True, text=True, stdin=subprocess.DEVNULL)
    n = sum(1 for l in r.stdout.splitlines() if l.startswith("slot ")) or 1
    st = tree.state()
    times = st.get("times", {})
    known = readknown(src + "/tests/vm-known.txt")
    pending = [Mode(m, job.get("timeout") or table[m]["timeout"], table[m]["whole"])
               for m in order(names, table, times, job.get("timeout"))]
    say(f"modes: {' '.join(m.name for m in pending)}, {n} slots")
    open(jobdir + "/modes", "w").close()
    q = queue.Queue()
    running = []
    results = {}   # name -> record
    runs = {}      # name -> [Mode]
    livecache = [0, 1]

    def live():
        now = time.monotonic()
        if now - livecache[0] > 2:
            livecache[:] = [now, sum(1 for nn, j, alive, d in jobs()
                                     if alive and os.path.exists(d + "/modes")) or 1]
        return livecache[1]

    def reader(m, log):
        for raw in iter(m.p.stdout.readline, b""):
            line = raw.decode(errors="replace").rstrip("\n")
            log.write(line + "\n")
            log.flush()
            q.put((m, line))
        log.close()
        q.put((m, None))

    def start(m):
        cmd = ["python3", "-I", "tests/gui.py", "--prospero", "build/prospero", "--mode",
               m.name, "--timeout", f"{m.timeout:g}"]
        if job.get("shots"):
            cmd += ["--shots", f"{out}/shots/{m.name}" + (".rerun" if m.rerun else "")]
        if job.get("trace"):
            cmd.append("--trace")
        if job.get("keep"):
            cmd.append("--keep")
        cmd += job.get("guipy") or []
        log = open(f"{out}/{m.name}{'.rerun' if m.rerun else ''}.log", "w")
        log.write("$ " + shlex.join(cmd) + "\n")
        log.flush()
        env = childenv(job, jobdir, f"vm:{job['nonce']}:{m.name}")
        m.p = spawn(cmd, src, env)
        m.t0 = m.clock = time.monotonic()
        m.started = time.strftime("%Y-%m-%dT%H:%M:%S")
        running.append(m)
        runs.setdefault(m.name, []).append(m)
        threading.Thread(target=reader, args=(m, log), daemon=True).start()
        say(f"{m.name}: started" + (" again, alone" if m.rerun else ""))

    entries = lambda m: [e[1] for e in known if e[0] == m]

    def online(m, line):
        if m not in running:
            return
        body = TAG.sub("", line.strip(), count=1)
        s = SLOT.match(body)
        if s and m.tslot is None:
            m.tslot = m.clock = time.monotonic()
            m.waiting = False
            m.slot = s.group(1)
            m.wait = float(s.group(2) or 0)
            say(f"{m.name}: {m.slot} after {m.wait:.0f} s")
        elif body.startswith("waiting for a GUI checklist slot") and m.tslot is None:
            if not m.waiting:
                say(f"{m.name}: {body}")
            m.waiting = True
        s = SUMMARY.search(body)
        if s:
            tag = line.strip()[:len(line.strip()) - len(body)]
            m.summ[tag] = tuple(int(x) for x in s.groups())
        if FAILRE.search(body):
            m.fails.append(line.strip())
            if m.shown < 20:
                m.shown += 1
                k = any(e in line for e in entries(m.name))
                say(f"{m.name}: {line.strip()}{'  (known)' if k else ''}")

    def finish(m):
        if m not in running:
            return  # given up on its output already
        m.status = m.p.wait()
        m.tend = time.monotonic()
        running.remove(m)
        if m.watchdog:
            cleanup([f"PROSPERO_VM_JOB={job['nonce']}",
                     f"PROSPERO_GUI_LABEL=vm:{job['nonce']}:{m.name}"], m.name)
        res, kl, other, matched = classify(m.status, m.fails, m.summ, entries(m.name),
                                           m.watchdog)
        m.result, m.known, m.other, m.matched = res, kl, other, matched
        dur = m.tend - (m.tslot or m.t0)
        checks = sum(s[0] for s in m.summ.values())
        nfail = sum(s[1] for s in m.summ.values())
        parts = [f"{dur:.0f} s"]
        if m.slot:
            parts.append(f"{m.slot}, waited {m.wait:.0f} s")
        if m.summ:
            parts.append(f"{checks} checks, {nfail} failed")
        if res == "known":
            parts.append(f"{len(kl)} known failures")
        if res == "failed":
            parts.append(f"exit {m.status}" if not m.watchdog else "watchdog")
        word = res
        if m.rerun:
            word = {"passed": "flaky: passed alone", "known": "flaky: known failures alone",
                    "failed": "failed again alone"}[res]
        say(f"{m.name}: {word} in {', '.join(parts)}")
        for line in other[:5]:
            if line not in m.fails[:20]:
                say(f"{m.name}:   {line}")
        if res == "passed" and not m.rerun:
            tree.update(lambda s: s.setdefault("times", {}).__setitem__(m.name, round(dur)))

    def watch():
        now = time.monotonic()
        for m in list(running):
            if m.p.poll() is not None:
                # gui.py is gone, but something it left holds its output
                if m.exited is None:
                    m.exited = now
                elif now - m.exited > 20 and not m.swept:
                    m.swept = True
                    say(f"{m.name}: gui.py exited, but its output is still open")
                    cleanup([f"PROSPERO_VM_JOB={job['nonce']}",
                             f"PROSPERO_GUI_LABEL=vm:{job['nonce']}:{m.name}"], m.name)
                elif now - m.exited > 40:
                    say(f"{m.name}: its output never closed; going on")
                    finish(m)
                continue
            if m.waiting and m.tslot is None:
                m.clock = now  # waiting for a slot: the clock starts at the slot
                continue
            if m.kill_at is not None:
                if now >= m.kill_at and m.p.poll() is None:
                    say(f"{m.name}: watchdog: KILL")
                    m.p.kill()  # unreaped: still our own child
                    m.kill_at = float("inf")
                continue
            if now - m.clock > 2 * m.timeout + 300:
                say(f"{m.name}: watchdog: no result {2 * m.timeout + 300:.0f} s after the "
                    f"slot, TERM")
                m.watchdog = True
                if m.p.poll() is None:
                    m.p.terminate()
                m.kill_at = now + 30

    def drain(cap):
        while pending or running:
            check()
            while pending:
                nxt = pending[0]
                if nxt.whole or nxt.rerun:
                    ok = not running
                else:
                    ok = len(running) < cap() and not any(x.whole for x in running)
                if not ok:
                    break
                start(pending.pop(0))
            try:
                m, line = q.get(timeout=0.5)
                while True:
                    if line is None:
                        finish(m)
                    else:
                        online(m, line)
                    m, line = q.get_nowait()
            except queue.Empty:
                pass
            watch()

    stop = threading.Event()
    threading.Thread(target=loadlogger, args=(out + "/load.log", stop), daemon=True).start()
    try:
        drain(lambda: share(n, live()))
        first = {name: rs[-1] for name, rs in runs.items()}
        again = [Mode(m.name, m.timeout, m.whole, rerun=True)
                 for m in first.values() if m.result == "failed"]
        if again:
            say(f"rerunning alone: {' '.join(m.name for m in again)}")
            pending.extend(again)
            drain(lambda: 1)
    except Cancelled:
        for m in running:
            if m.p.poll() is None:
                m.p.terminate()
        deadline = time.monotonic() + 10
        while any(m.p.poll() is None for m in running) and time.monotonic() < deadline:
            time.sleep(0.25)
        for m in running:
            if m.p.poll() is None:
                m.p.kill()
            m.p.wait()
        raise
    finally:
        stop.set()
        try:
            os.unlink(jobdir + "/modes")
        except OSError:
            pass

    # Results
    counts = {"passed": 0, "known": 0, "flaky": 0, "failed": 0}
    flaky = []
    for name in names:
        rs = runs.get(name, [])
        if not rs:
            continue
        a = rs[0]
        res = a.result
        if res == "failed" and len(rs) > 1:
            res = "flaky" if rs[1].result in ("passed", "known") else "failed"
        if res == "flaky":
            flaky.append(a)
        counts[res] += 1
        results[name] = {
            "result": res,
            "runs": [{"result": m.result, "status": m.status, "started": m.started,
                      "seconds": round((m.tend or 0) - (m.tslot or m.t0 or 0), 1),
                      "wait": m.wait, "slot": m.slot, "watchdog": m.watchdog,
                      "summaries": {k or "-": v for k, v in m.summ.items()},
                      "failures": m.fails, "known": m.known, "other": m.other,
                      "log": f"{name}{'.rerun' if m.rerun else ''}.log"} for m in rs],
        }
    unmatched = []
    for mode, text, reason in known:
        if mode in runs and not any(text in l for m in runs[mode] for l in m.fails):
            unmatched.append((mode, text))
            say(f"{mode}: no longer failing: remove it from tests/vm-known.txt: {text}")
    if flaky:
        with open(ROOT + "/flaky.log", "a") as f:
            for m in flaky:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M')}\t{job.get('checkout')}\t"
                        f"{job.get('head', '')[:12]}\t{m.name}\t"
                        + " | ".join(m.other[:5]) + "\n")
    summary = {"head": job.get("head"), "subject": job.get("subject"),
               "checkout": job.get("checkout"), "slots": n, "preflight": load,
               "warnings": warnings, "modes": results, "counts": counts,
               "no_longer_failing": unmatched}
    atomic(out + "/summary.json", json.dumps(summary, indent=1))
    printwarnings(warnings)
    say("result: " + ", ".join(f"{v} {k}" for k, v in counts.items() if v) +
        (f"; {len(warnings)} warnings" if warnings else ""))
    for name, r in results.items():
        if r["result"] in ("known", "flaky", "failed"):
            say(f"  {name}: {r['result']}")
    return FAILED if counts["failed"] or warnings else PASSED


def runcmd(job, jobdir, tree):
    env = childenv(job, jobdir, f"vm:{job['nonce']}:run")
    cmd = ["python3", "-I", "tests/slots.py", "hold", "--all", "--label",
           f"vm:{job['nonce']}:run", "--", *job["argv"]]
    say("$ " + shlex.join(job["argv"]))
    p = spawn(cmd, tree.src, env)
    return pump(p, say)


def housekeeping(mykey):
    """Under house.lock, never waiting for it: old jobs and landing
    directories, and idle trees. Touches nothing outside
    /var/tmp/prospero-vm*, and sends no signals."""
    lk = Lock(ROOT + "/house.lock")
    if not lk.take(fcntl.LOCK_EX, nb=True):
        return
    try:
        now = time.time()
        uid = os.getuid()
        for nonce, job, alive, d in jobs():
            if alive:
                continue
            t = 0
            for f in ("done", "agent", ""):
                try:
                    t = os.lstat(os.path.join(d, f) if f else d).st_mtime
                    break
                except OSError:
                    pass
            if now - t > KEEP_JOBS:
                shutil.rmtree(d, ignore_errors=True)
        for d in glob.glob(LANDING + "*"):
            try:
                st = os.lstat(d)
            except OSError:
                continue
            if stat.S_ISDIR(st.st_mode) and st.st_uid == uid and now - st.st_mtime > KEEP_LANDING:
                shutil.rmtree(d, ignore_errors=True)
        trees = []
        for d in glob.glob(ROOT + "/trees/*") + glob.glob(ROOT + "/trees/.evict-*"):
            name = os.path.basename(d)
            if name.startswith(".evict-"):
                shutil.rmtree(d, ignore_errors=True)
                continue
            t = Tree(name)
            used = t.state().get("used") or os.lstat(d).st_mtime
            trees.append((used, t))
        trees.sort(key=lambda x: x[0])
        count = len(trees)
        for used, t in trees:
            if t.key == mykey:
                continue
            free = shutil.disk_usage(ROOT).free
            if not (now - used > TREE_IDLE or count > MAX_TREES or free < MIN_FREE):
                continue
            if not t.gate.take(fcntl.LOCK_EX, nb=True):
                continue
            try:
                if not t.lock.take(fcntl.LOCK_EX, nb=True):
                    continue
                dst = f"{ROOT}/trees/.evict-{t.key}-{secrets.token_hex(3)}"
                os.rename(t.dir, dst)
                say(f"housekeeping: evicted the tree {t.key}, idle "
                    f"{(now - used) / 86400:.1f} days")
                shutil.rmtree(dst, ignore_errors=True)
                count -= 1
            finally:
                t.lock.close()
                t.gate.close()
    finally:
        lk.close()


def vmstatus():
    cache = os.path.expanduser("~/.cache")
    files = sorted(glob.glob(cache + "/prospero-gui.slot*.lock"),
                   key=lambda p: int(re.sub(r"\D", "", os.path.basename(p)) or 0))
    say(f"slots ({len(files)} slot files):")
    for p in files:
        h, who = held(p)
        say(f"  {os.path.basename(p)}: {'held by ' + who if h else 'free'}")
    say("builds:")
    for k in range(2):
        h, who = held(f"{ROOT}/build.{k}.lock")
        say(f"  build.{k}: {'held by ' + who if h else 'free'}")
    say("trees:")
    for d in sorted(glob.glob(ROOT + "/trees/*")):
        t = Tree(os.path.basename(d))
        st = t.state()
        inuse = held(t.lock.path)[0]
        b = st.get("built") or {}
        nwarn = sum(len([l for l in v if "warning:" in l])
                    for v in st.get("warnings", {}).values())
        used = st.get("used")
        age = f"used {(time.time() - used) / 3600:.1f} h ago" if used else "never used"
        say(f"  {t.key}: {age}, {'in use' if inuse else 'idle'}, built "
            f"{','.join(b.get('targets', [])) or 'nothing'}"
            f"{'' if b.get('ok', True) else ' (failed)'}, {nwarn} warnings")
    say("jobs:")
    for nonce, job, alive, d in jobs():
        if alive:
            phase = "modes" if os.path.exists(d + "/modes") else "preparing"
            say(f"  {nonce}: {job.get('cmd')} {','.join(job.get('modes') or [])} from "
                f"{job.get('checkout', '?')}, {phase}")
    s = Sampler()
    time.sleep(1)
    cores, use = s.sample()
    la = loadavg()
    say(f"load: {la[0]:.2f} {la[1]:.2f} {la[2]:.2f}, MemAvailable {meminfo() / 2**30:.1f} GB, "
        f"CPU PSI some avg10 {psi():.1f}%, foreign {cores:.1f} cores"
        + (": " + ", ".join(f"{c} {u:.1f}" for u, c, p in use[:3]) if use else ""))
    du = shutil.disk_usage("/var/tmp")
    say(f"space: {du.free / 2**30:.1f} GB free of {du.total / 2**30:.1f} GB in /var/tmp")
    try:
        with open(ROOT + "/flaky.log") as f:
            lines = f.read().splitlines()[-10:]
    except OSError:
        lines = []
    say(f"flaky log ({ROOT}/flaky.log):" + ("" if lines else " empty"))
    for line in lines:
        say("  " + line)
    return PASSED


def agent(landing):
    global UVDIR
    UVDIR = os.path.join(os.path.expanduser("~"), UVREL)
    try:
        with open(landing + "/job.json") as f:
            job = json.load(f)
    except (OSError, ValueError) as e:
        print(f"vm.py agent: {e}", flush=True)
        return USAGE

    def watchstdin():
        try:
            while os.read(0, 4096):
                pass
        except OSError:
            pass
        CANCEL.set()
    threading.Thread(target=watchstdin, daemon=True).start()
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, lambda s, f: CANCEL.set())
    setuproot()
    jobdir = None
    try:
        housekeeping(job.get("key"))
        if job["cmd"] == "status":
            return vmstatus()
        nonce = job["nonce"]
        jobdir = f"{ROOT}/jobs/{nonce}"
        os.makedirs(jobdir + "/out", mode=0o700)
        os.makedirs(jobdir + "/tmp", mode=0o700)
        with open(jobdir + "/job.json", "w") as f:
            json.dump(job, f)
        atomic(jobdir + "/agent", f"{os.getpid()} {starttime(os.getpid())}\n")
        load = busy(job.get("slowok"))
        if job["cmd"] == "preflight":
            say(f"preflight: ok, foreign {load['foreign_cores']} cores, MemAvailable "
                f"{load['mem_available_gb']} GB")
            return PASSED
        target = "all" if job["cmd"] == "run" else "prospero"
        tree, ok, warnings = prepare(job, jobdir, landing, target)
        shutil.rmtree(landing, ignore_errors=True)
        touchtree(tree)
        if job["cmd"] == "build":
            printwarnings(warnings)
            return PASSED if ok and not warnings else FAILED
        if not ok:
            return FAILED
        if job["cmd"] == "run":
            if warnings:
                say(f"build: {len(warnings)} warnings (GCC)")
            return runcmd(job, jobdir, tree)
        say(RESULTS + jobdir + "/out")
        return runmodes(job, jobdir, tree, warnings, load)
    except Cancelled:
        say("vm.py: cancelled")
        return CANCELLED
    finally:
        if jobdir and job.get("nonce"):
            cleanup([f"PROSPERO_VM_JOB={job['nonce']}"], "cleanup")
            try:
                open(jobdir + "/done", "w").close()
            except OSError:
                pass
        shutil.rmtree(landing, ignore_errors=True)


# ---------------------------------------------------------------------------
# Self-test

def selftest():
    problems = []

    def ok(cond, what):
        if not cond:
            problems.append(what)
            print(f"FAIL: {what}", flush=True)

    # treekey
    k = treekey("/tmp")
    ok(re.fullmatch(r"[A-Za-z0-9._-]+-[0-9a-f]{10}", k) and k == treekey("/tmp/"),
       f"treekey: {k}")

    # Slot lines
    for line, want in (("slot 2 of 3", ("slot 2 of 3", None)),
                       ("slot 1 of 1 after 12 s", ("slot 1 of 1", "12")),
                       ("all 4 slots after 3 s", ("all 4 slots", "3"))):
        m = SLOT.match(line)
        ok(m and m.groups() == want, f"slot line {line!r}: {m and m.groups()}")
    ok(not SLOT.match("slot 2 of 3 is free"), "slot line false positive")

    # FAIL lines
    for line, want in (("drive: FAIL CHK_TITLEBAR", True), ("[b] drive: FAIL X", True),
                       ("FAIL: processes left: 12", True), ("drive: ok   CHK_X", False),
                       ("drive: 3 checks, 1 failed, 0 stale", False),
                       ("FAILED: thing", False)):
        ok(bool(FAILRE.search(TAG.sub("", line))) == want, f"FAIL line {line!r}")

    # Classification
    summ = {"": (10, 1, 0)}
    fails = ["drive: FAIL CHK_TRAY", "FAIL: checklist: drive: 10 checks, 1 failed, 0 stale",
             "FAIL: exit status 1"]
    ok(classify(0, [], {}, [])[0] == "passed", "classify: passed")
    ok(classify(1, fails, summ, ["CHK_TRAY"])[0] == "known", "classify: known")
    ok(classify(1, fails, summ, [])[0] == "failed", "classify: unknown check")
    ok(classify(1, fails + ["FAIL: processes left: 1"], summ, ["CHK_TRAY"])[0] == "failed",
       "classify: known check, other problem")
    ok(classify(1, fails, {"": (10, 2, 0)}, ["CHK_TRAY"])[0] == "failed",
       "classify: counts disagree")
    ok(classify(1, fails, {"": (10, 1, 1)}, ["CHK_TRAY"])[0] == "failed", "classify: stale")
    ok(classify(1, [], {}, [])[0] == "failed", "classify: exit, no FAIL")
    ok(classify(0, [], {}, [], watchdog=True)[0] == "failed", "classify: watchdog")
    ok(classify(1, ["FAIL: tray: no item"], {}, ["tray: no item"])[0] == "known",
       "classify: known gui.py failure")
    two = {"[a] ": (5, 1, 0), "[b] ": (5, 1, 0)}
    ok(classify(1, ["[a] drive: FAIL CHK_A", "[b] drive: FAIL CHK_A",
                    "FAIL: a: checklist: 5 checks, 1 failed, 0 stale"],
                two, ["CHK_A"])[0] == "known", "classify: two windows")

    # Known list
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "known")
        with open(p, "w") as f:
            f.write("# comment\n\ndrive\tCHK_TRAY\tno watcher\nbad line\nplan\tX\n")
        got = readknown(p)
        ok(got == [("drive", "CHK_TRAY", "no watcher"), ("plan", "X", "")], f"readknown: {got}")
        ok(readknown(os.path.join(d, "none")) == [], "readknown: missing")

    # Order and share
    table = {"a": {"timeout": 600, "whole": False}, "b": {"timeout": 1200, "whole": False},
             "c": {"timeout": 600, "whole": True}, "d": {"timeout": 600, "whole": False}}
    ok(order(["a", "b", "c", "d"], table, {"a": 900, "d": 50}) == ["b", "a", "d", "c"],
       f"order: {order(['a', 'b', 'c', 'd'], table, {'a': 900, 'd': 50})}")
    ok([share(4, 1), share(4, 2), share(4, 3), share(1, 5), share(3, 0)] == [4, 2, 1, 1, 3],
       "share")

    # Apple regions
    text = "\n".join(["a", "#ifdef __APPLE__", "mac", "#else", "other", "#endif",
                      "#ifndef __APPLE__", "notmac", "#else", "mac2", "#endif",
                      "#if defined(_WIN32)", "win", "#elif defined(__APPLE__)", "mac3",
                      "#endif", "#if X", "#if defined(__APPLE__) && Y", "mac4", "#endif",
                      "#endif"])
    got = sorted(appleregions(text))
    ok(got == [2, 3, 9, 10, 11, 14, 15, 16, 18, 19, 20], f"appleregions: {got}")

    # Files applied by content
    with tempfile.TemporaryDirectory() as d:
        top, src = os.path.join(d, "top"), os.path.join(d, "src")
        for name, data in (("a.txt", "a"), ("sub/b.sh", "b"), ("sub/deep/c", "c")):
            os.makedirs(os.path.dirname(os.path.join(top, name)), exist_ok=True)
            with open(os.path.join(top, name), "w") as f:
                f.write(data)
        os.chmod(os.path.join(top, "sub/b.sh"), 0o755)
        os.makedirs(os.path.join(src, "build/x"))
        os.makedirs(os.path.join(src, "gone/dir"))
        for name, data in (("build/x/obj", "o"), ("gone/dir/f", "f"), ("a.txt", "old"),
                           ("stale", "s"), ("buildinfo", "keep")):
            with open(os.path.join(src, name), "w") as f:
                f.write(data)
        tar = os.path.join(d, "t.tar.gz")
        names = ["a.txt", "sub/b.sh", "sub/deep/c"]
        dig, total = maketar(tar, top, names)
        ok(total == 3, f"maketar bytes {total}")
        r = applytree(src, tar)
        ok(r == (3, 2, 0), f"applytree first: {r}")
        ok(open(os.path.join(src, "a.txt")).read() == "a", "applytree content")
        ok(os.stat(os.path.join(src, "sub/b.sh")).st_mode & 0o100, "applytree exec bit")
        ok(os.path.exists(os.path.join(src, "build/x/obj")), "applytree kept build/")
        ok(os.path.exists(os.path.join(src, "buildinfo")), "applytree kept build*")
        ok(not os.path.exists(os.path.join(src, "gone")), "applytree removed empty dirs")
        m0 = os.stat(os.path.join(src, "a.txt")).st_mtime_ns
        r = applytree(src, tar)
        ok(r == (0, 0, 3), f"applytree again: {r}")
        ok(os.stat(os.path.join(src, "a.txt")).st_mtime_ns == m0, "applytree rewrote a file")
        os.chmod(os.path.join(top, "sub/b.sh"), 0o644)
        dig2, _ = maketar(tar, top, names)
        ok(dig2 != dig, "digest covers exec bits")
        ok(applytree(src, tar) == (1, 0, 2), "applytree exec bit only")
        ok(not os.stat(os.path.join(src, "sub/b.sh")).st_mode & 0o100, "exec bit cleared")

    # Arguments
    me = [sys.executable, os.path.abspath(__file__)]
    for argv in (["--mode", "drive", "--build"], ["run"], [], ["--build", "--", "x"],
                 ["--slots", "0", "--default"]):
        r = subprocess.run(me + argv, capture_output=True, text=True, timeout=30)
        ok(r.returncode == USAGE, f"usage {argv}: exit {r.returncode} {r.stderr[-200:]}")

    # Signals go only to validated processes (Linux)
    if sys.platform.startswith("linux"):
        tag = "selftest-" + secrets.token_hex(4)
        mine = subprocess.Popen(["sleep", "60"], env=dict(os.environ, PROSPERO_VM_JOB=tag))
        other = subprocess.Popen(["sleep", "60"], env=dict(os.environ, PROSPERO_VM_JOB=tag + "x"))
        time.sleep(0.2)
        hit = sweep([f"PROSPERO_VM_JOB={tag}"], signal.SIGTERM)
        ok(hit == [mine.pid], f"sweep hit {hit}, wanted {mine.pid}")
        ok(mine.wait(5) == -signal.SIGTERM, "sweep's process survived")
        ok(other.poll() is None, "sweep signaled another job's process")
        other.terminate()
        other.wait()
        s = Sampler()
        time.sleep(0.3)
        cores, use = s.sample()
        ok(cores >= 0, "sampler")
    else:
        print("vm.py: sweep and sampler: skipped, not Linux", flush=True)

    print(f"vm.py selftest: {'FAIL' if problems else 'ok'}", flush=True)
    return FAILED if problems else PASSED


# ---------------------------------------------------------------------------

def main():
    argv = sys.argv[1:]
    if argv[:1] == ["agent"] and len(argv) == 2:
        return agent(argv[1])
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(prog="vm.py", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", choices=["run", "status"])
    ap.add_argument("--mode", help="modes to run, comma-separated")
    ap.add_argument("--default", action="store_true", help="the default set (gui.py's MODES)")
    ap.add_argument("--build", action="store_true", help="sync and build only")
    ap.add_argument("--preflight", action="store_true", help="check the VM and exit")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--slots", type=int, help="PROSPERO_GUI_SLOTS on the VM")
    ap.add_argument("--timeout", type=float, help="each mode's (default: its own)")
    ap.add_argument("--shots", action="store_true", help="save screenshots to out/shots/M")
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--keep", action="store_true", help="gui.py --keep")
    ap.add_argument("--fresh", action="store_true", help="remove the build directory first")
    ap.add_argument("--slow-ok", action="store_true",
                    help="go on at background priority, or when busy")
    ap.add_argument("--tracked", action="store_true", help="only tracked files")
    ap.add_argument("--out", help="the results here, not under $TMPDIR")
    ap.add_argument("--host", default=HOST, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    what = [bool(args.mode), args.default, args.build, args.preflight, args.selftest,
            args.command is not None]
    if sum(what) != 1:
        ap.error("give one of --mode, --default, --build, run, status, --preflight, --selftest")
    if args.command == "run" and not extra:
        ap.error("run needs -- CMD...")
    if extra and not (args.mode or args.default or args.command == "run"):
        ap.error("arguments after -- go with --mode, --default or run")
    if args.slots is not None and args.slots < 1:
        ap.error("--slots must be at least 1")
    if args.selftest:
        return selftest()
    return macmain(args, extra)


if __name__ == "__main__":
    sys.exit(main())
