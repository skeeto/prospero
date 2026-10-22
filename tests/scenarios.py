#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine scenarios: the harness against the fault-injecting fake provider.

Each scenario runs the real binary ("harness run", or "harness --headless"
for cancels) in a fresh working and state directory, with a model name
that selects a script in fake_provider.py, then checks the event sequence,
the files left behind, the exit code and a wall-clock bound, which counts
from after the directories are made, without the time spent waiting on
ssh for a remote host's files and processes.

  uv run tests/scenarios.py --harness build/harness [NAME ...]
  uv run tests/scenarios.py --harness PATH --host biron.local   (Windows)
  uv run tests/scenarios.py --harness build/harness --soak 30

With --host, the fake provider runs here and the harness runs on the
Windows host over ssh, reaching the provider through a reverse tunnel;
PATH is the harness.exe already on that host. busybox.exe is copied next
to it from the host's w64devkit, which is how the harness finds its shell.

  uv run tests/scenarios.py --harness 'C:/DOCUME~1/u/LOCALS~1/Temp/harness.exe' \\
      --xp 192.168.1.106 [NAME ...]

With --xp, the same for Windows XP running the user's own sshd: no
tunnel, so the provider listens on this machine's LAN address instead.
"""

import argparse
import base64
import concurrent.futures
import json
import os
import random
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402
import pids as P  # noqa: E402


# Hosts: where the harness runs
#
# A host kills by pid only what pids.py lets through, checked again on
# the host, in the command that kills, to be a process of the run's:
# here one of this user's started since the host was made; on Windows
# one whose command line names the run's own directory, through a handle
# held from the check to the kill; on XP, which shows less, one of this
# user's whose own line shows the run's own copy of the harness, by its
# name unique to the run, or the run's directory. Never by an ancestor:
# Windows does not reparent, so a dead creator's pid may be anyone's.

# Seconds spent waiting on ssh, by the thread that asked, for runone:
# over ssh each file operation, liveness check or kill is a connection
# of its own, which a stalled link (XP's Wi-Fi) can hold for half a
# minute, and that is no time of the harness's.
LINK = threading.local()


def linktime(fn):
    """Count a method's time as the link's (LINK)."""
    def timed(*args, **kw):
        t0 = time.monotonic()
        try:
            return fn(*args, **kw)
        finally:
            LINK.secs = getattr(LINK, "secs", 0.0) + time.monotonic() - t0
    return timed


class LocalHost:
    windows = os.name == "nt"
    prefix = []  # what starts the harness, as cleanup.py's disclaim.py

    def __init__(self, harness, url):
        self.harness = os.path.abspath(harness)
        self.url = url
        self.root = tempfile.mkdtemp(prefix="harness-scen-")
        self.t0 = time.time()

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def join(self, *parts):
        return os.path.join(*parts)

    def mkdir(self, path):
        os.makedirs(path, exist_ok=True)

    def write(self, path, data):
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(path, mode) as f:
            f.write(data)

    def read(self, path):
        try:
            with open(path, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return None

    def touch(self, path, mtime):
        os.utime(path, (mtime, mtime))

    def bulk(self, base, files, mtimes=None):
        """Write many files under base at once: {relative path: data},
        with {relative path: modification time} for some."""
        for rel, data in files.items():
            path = os.path.join(base, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.write(path, data)
        for rel, t in (mtimes or {}).items():
            self.touch(os.path.join(base, *rel.split("/")), t)

    def popen(self, args, cwd, stdin=False, env=None):
        return subprocess.Popen(
            self.prefix + [self.harness] + args, cwd=cwd,
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, **env) if env else None,
            text=True, encoding="utf-8", errors="replace", bufsize=1)

    def alive(self, pid):
        return P.alive(pid)

    def killhard(self, proc, pid=None):
        # Here proc is the harness itself, whatever pid a file gives
        proc.kill()

    def killpid(self, pid):
        """Kill a leftover process by a pid from a file, if it is ours."""
        try:
            P.signal_ours(pid, signal.SIGKILL, P.since(self.t0))
        except (OSError, P.Refused) as e:
            print(f"     kill: {e}", flush=True)


class SshHost:
    """A Windows host reached over ssh, with busybox from w64devkit."""
    windows = True

    def __init__(self, host, harness, port, rport):
        self.host = host
        self.harness = harness
        self.ctl = os.path.join(tempfile.mkdtemp(prefix="harness-ssh-"), "ctl")
        subprocess.run(["ssh", "-M", "-S", self.ctl, "-fN", "-o", "ExitOnForwardFailure=yes",
                        "-R", f"{rport}:127.0.0.1:{port}", host], check=True)
        self.url = f"http://127.0.0.1:{rport}"
        tmp = self.sh("echo \"$TMP\"").strip()
        self.root = f"{tmp}/harness-scen-{random.randrange(1 << 32):08x}"
        self.sh(f"mkdir -p {self.root} && cp $HOME/w64devkit/bin/busybox.exe "
                f"{self.root}/busybox.exe && cp {shlex.quote(harness)} {self.root}/harness.exe")
        self.exe = f"{self.root}/harness.exe"

    def close(self):
        self.sh(cleanroot(self.root))
        subprocess.run(["ssh", "-S", self.ctl, "-O", "exit", self.host],
                       capture_output=True)

    @linktime
    def ssh(self, command, **kw):
        return subprocess.run(["ssh", "-S", self.ctl, self.host, command],
                              capture_output=True, text=True, **kw)

    def sh(self, script, check=True):
        # The script goes base64-encoded through cmd.exe, which mangles
        # quotes and percent signs
        b = base64.b64encode(script.encode()).decode()
        cmd = (f"w64devkit\\bin\\sh.exe -c \"export PATH=$HOME/w64devkit/bin:$PATH; "
               f"echo {b} | base64 -d | sh\"")
        r = self.ssh(cmd)
        if check and r.returncode:
            raise RuntimeError(f"remote script failed: {script}\n{r.stderr}")
        return r.stdout

    def join(self, *parts):
        return "/".join(parts)

    def mkdir(self, path):
        self.sh(f"mkdir -p {shlex.quote(path)}")

    def write(self, path, data):
        if isinstance(data, str):
            data = data.encode()
        b = base64.b64encode(data).decode()
        self.sh(f"echo {b} | base64 -d > {shlex.quote(path)}")

    def read(self, path):
        out = self.sh(f"if [ -f {shlex.quote(path)} ]; then base64 {shlex.quote(path)}; "
                      f"else echo MISSING; fi")
        if out.strip() == "MISSING":
            return None
        return base64.b64decode("".join(out.split())).decode("utf-8", "replace")

    def touch(self, path, mtime):
        self.sh(f"touch -d @{int(mtime)} {shlex.quote(path)}")

    def bulk(self, base, files, mtimes=None):
        lines = []
        for rel, data in files.items():
            if isinstance(data, str):
                data = data.encode()
            path = shlex.quote(f"{base}/{rel}")
            b = base64.b64encode(data).decode()
            lines.append(f"mkdir -p $(dirname {path}) && echo {b} | base64 -d > {path}")
        for rel, t in (mtimes or {}).items():
            lines.append(f"touch -d @{int(t)} {shlex.quote(f'{base}/{rel}')}")
        # In pieces: cmd.exe takes command lines of 8191 characters at most
        piece = []
        for line in lines + [None]:
            if line is None or sum(len(x) + 1 for x in piece) + len(line) > 4000:
                if piece:
                    self.sh("\n".join(piece))
                piece = []
            if line is not None:
                piece.append(line)

    def popen(self, args, cwd, stdin=False, env=None):
        def q(a):
            return '"' + a + '"' if (" " in a or not a) else a
        win = lambda p: p.replace("/", "\\")
        # cmd.exe keeps a blank before && in the value, hence the quotes
        sets = "".join(f'set "{k}={v}"&& ' for k, v in (env or {}).items())
        cmd = (f"{sets}cd /d {win(cwd)} && {win(self.exe)} " +
               " ".join(q(a) for a in args))
        return subprocess.Popen(
            ["ssh", "-S", self.ctl, self.host, cmd],
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1)

    def alive(self, pid):
        r = self.ssh(f"tasklist /FI \"PID eq {P.arg(pid)}\" /NH")
        return str(pid) in r.stdout

    def killhard(self, proc, pid=None):
        if pid:
            self.killpid(pid)
        else:
            proc.kill()

    def killpid(self, pid):
        """Kill a process by a pid from a file, if its command line names
        the run's own directory, as the harness's and its tools' do. One
        PowerShell checks and kills through one handle, which it opens
        first and which holds the pid for that process meanwhile: the
        command line it checks is that process's (the creation time it
        reports, the handle's), and Kill() uses that handle."""
        name = self.root.rsplit("/", 1)[-1]
        n = P.arg(pid)
        ps = ("$ErrorActionPreference = 'Stop'; try { "
              f"$p = Get-Process -Id {n}; $null = $p.Handle; "
              f"$c = Get-CimInstance Win32_Process -Filter 'ProcessId={n}'; "
              f"if ($c -and $c.CommandLine -like '*{name}*' -and "
              "[Math]::Abs(($c.CreationDate - $p.StartTime).TotalSeconds) -lt 0.1) "
              "{ $p.Kill(); exit 0 } else { exit 1 } } catch { exit 2 }")
        r = self.ssh(f'powershell -NoProfile -Command "{ps}"')
        if r.returncode:
            why = "refused" if r.returncode == 1 else "failed"
            print(f"     kill: {why} for pid {pid}: {r.stdout.strip()} "
                  f"{r.stderr.strip()}", flush=True)


class XpHost(SshHost):
    """Windows XP over the user's own sshd, whose commands run in busybox
    sh. It takes one channel per connection and forwards no ports, and
    XP's firewall blocks a new program's listening, so the harness there
    connects out to the fake provider on this machine's LAN address.
    Each command is a connection of its own, retried when the link (Wi-Fi,
    which stalls now and then) drops it."""

    def __init__(self, host, harness, url):
        self.host = host
        self.harness = harness
        self.url = url
        tmp = self.sh("echo \"$TMP\"").strip()
        run = f"{random.randrange(1 << 32):08x}"
        self.root = f"{tmp}/harness-scen-{run}"
        # A name of the run's own, which no other run's harness, nor any
        # engine of the user's, has
        self.exename = f"h-{run}.exe"
        self.sh(f"mkdir -p {self.root} && "
                f"cp \"$HOME/w64devkit/bin/busybox.exe\" {self.root}/busybox.exe && "
                f"cp {shlex.quote(harness)} {self.root}/{self.exename}")
        self.exe = f"{self.root}/{self.exename}"

    def close(self):
        self.sh(cleanroot(self.root), check=False)

    @linktime
    def ssh(self, command, input=None, **kw):
        for attempt in range(4):
            r = subprocess.run(["ssh", "-o", "ConnectTimeout=30", self.host, command],
                               input=input, capture_output=True, text=True, **kw)
            if r.returncode != 255 or attempt == 3:
                return r
            time.sleep(2 + 3*attempt)

    def sh(self, script, check=True):
        r = self.ssh("sh -s", input=script)
        if check and r.returncode:
            raise RuntimeError(f"remote script failed: {script[:2000]}\n{r.stderr}")
        return r.stdout

    def popen(self, args, cwd, stdin=False, env=None):
        sets = "".join(f"{k}={shlex.quote(v)} " for k, v in (env or {}).items())
        cmd = (f"cd {shlex.quote(cwd)} && {sets}{shlex.quote(self.exe)} " +
               " ".join(shlex.quote(a) for a in args))
        return subprocess.Popen(
            ["ssh", "-o", "ConnectTimeout=30", self.host, cmd],
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1)

    def alive(self, pid):
        r = self.ssh(f"tasklist /FI \"PID eq {P.arg(pid)}\" /NH")
        return any(line.split()[1:2] == [str(pid)] for line in r.stdout.splitlines())

    def killhard(self, proc, pid=None):
        if pid:
            self.killpid(pid)
        else:
            proc.kill()

    def killpid(self, pid):
        """Kill a process by a pid from a file, if busybox's ps shows it
        to be this user's, with the run's own copy of the harness (its
        unique name) or the run's directory in its own line: not a parent
        of it, whose pid could be another process's by now. (Here ps shows
        command lines of busybox's processes alone, and paths of none, so
        the tools' own processes are refused, and die with the harness's
        job instead.) No taskkill: busybox kill (TerminateProcess)."""
        name = self.root.rsplit("/", 1)[-1]
        r = self.ssh("sh -s", input=f"""pid={P.arg(pid)}
ps -o pid,user,comm,args | awk -v p="$pid" -v exe={shlex.quote(self.exename)} \\
    -v r={shlex.quote(name)} -v me="$(id -un)" '
NR > 1 && $1 == p && $2 == me && ($3 == exe || index($0, r)) {{ ok = 1 }}
END {{ exit ok ? 0 : 1 }}' && kill -9 "$pid"
""")
        if r.returncode:
            print(f"     kill: refused or failed for pid {pid}: {r.stderr.strip()}",
                  flush=True)


def cleanroot(root):
    """A script removing a Windows host's scenario root. cmd's rd, unlike
    busybox rm, removes paths past MAX_PATH and names outside the ANSI
    code page (deep_unicode), through a \\\\?\\ path."""
    win = "\\\\?\\" + root.replace("/", "\\")
    return f"cd / && cmd /c 'rd /s /q {win}'; rm -rf {root}"


def settle():
    """Before timed work: write out what other programs, as another suite
    under ctest, left in the page cache, which would otherwise go out in
    the middle of it. A Raspberry Pi's SD card holds up every process
    that writes for several seconds while it takes a burst."""
    if hasattr(os, "sync"):
        os.sync()


def lanaddress(host):
    """This machine's address on the network that reaches host."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((socket.gethostbyname(host), 9))
        return s.getsockname()[0]
    finally:
        s.close()


# Running the harness

class Result:
    def __init__(self, rc, lines, stderr, elapsed):
        self.rc = rc
        self.stderr = stderr
        self.elapsed = elapsed
        self.events = []
        for line in lines:
            line = line.strip()
            if line:
                try:
                    self.events.append(json.loads(line))
                except ValueError:
                    self.events.append({"ev": "?", "raw": line})

    def names(self):
        return [e.get("ev") for e in self.events]

    def all(self, ev, sid=None):
        return [e for e in self.events
                if e.get("ev") == ev and (sid is None or e.get("sid") == sid)]

    def last(self, ev, sid=None):
        found = self.all(ev, sid)
        return found[-1] if found else {}

    def sessions(self):
        """The run's own session id, and its subagents' session records."""
        own = [e for e in self.all("session") if not e.get("parent")]
        kids = [e for e in self.all("session") if e.get("parent")]
        return (own[0]["sid"] if own else None), kids

    def text(self):
        """Visible text of every committed assistant message, joined."""
        out = []
        for e in self.all("assistant"):
            for b in e.get("blocks", []):
                if b.get("t") == "text":
                    out.append(b.get("text", ""))
        return "".join(out)


class Failure(Exception):
    pass


class T:
    """One scenario run: its dirs, model, and helpers."""

    def __init__(self, host, name, conf):
        self.host = host
        self.name = name
        self.nonce = f"{random.randrange(1 << 32):08x}"
        self.model = f"fake:{name}:{self.nonce}"
        # A second model, for scenarios of two: scenario NAME_b
        self.model2 = f"fake:{name}_b:{self.nonce}"
        conf = dict(conf or {})
        listing = conf.pop("_listing", {})
        fake_provider.STATE.register(self.model, **listing)
        fake_provider.STATE.register(self.model2, **listing)
        base = host.join(host.root, f"{name}-{self.nonce}")
        self.base = base
        self.work = host.join(base, "work")
        self.state = host.join(base, "state")
        self.config = host.join(base, "harness.ini")
        host.mkdir(self.work)
        host.mkdir(self.state)
        # Endpoint settings, with "_top" settings before any section,
        # "_path" after the endpoint's URL, "_extra" as more sections,
        # where {model} and {model2} are the scenario's model names, and
        # any other "_name" as the section [name] ("_guard" in [guard],
        # "_tools" in [tools] and so on)
        top = conf.pop("_top", {})
        path = conf.pop("_path", "")
        extra = conf.pop("_extra", "")
        sections = {k[1:]: conf.pop(k) for k in list(conf) if k.startswith("_")}
        ini = {"first_byte_timeout": 5, "idle_timeout": 3,
               "progress_timeout": 6, "connect_timeout": 2}
        ini.update(conf)
        body = "".join(f"{k} = {v}\n" for k, v in top.items())
        body += f"[endpoint fake]\nurl = {host.url}/{path}\n" + "".join(
            f"{k} = {v}\n" for k, v in ini.items())
        for name, settings in sections.items():
            body += f"[{name}]\n" + "".join(f"{k} = {v}\n" for k, v in settings.items())
        body += extra.format(model=self.model, model2=self.model2)
        host.write(self.config, body)
        self.notes = []

    def args(self, extra=()):
        return ["-C", self.work, "--state", self.state, "-c", self.config,
                "-e", "fake", "-m", self.model] + list(extra)

    def run(self, prompt="go", extra=(), timeout=60, resume=None, watch=None,
            env=None):
        args = ["run"] + self.args(extra)
        if resume:
            args += ["--resume", resume]
        if prompt:
            args.append(prompt)
        t0 = time.monotonic()
        p = self.host.popen(args, self.work, env=env)
        lines = []
        killer = threading.Timer(timeout, p.kill)
        killer.start()
        try:
            for line in p.stdout:
                lines.append(line)
                if watch:
                    try:
                        ev = json.loads(line)
                    except ValueError:
                        continue
                    if watch(ev, p):
                        break
            if watch:
                # The watcher killed it; collect what remains
                for line in p.stdout:
                    lines.append(line)
            p.wait()
        finally:
            killer.cancel()
        err = p.stderr.read()
        if "ThreadSanitizer" in err or "AddressSanitizer" in err:
            raise Failure("sanitizer report:\n" + err[:4000])
        return Result(p.returncode, lines, err, time.monotonic() - t0)

    def headless(self, env=None):
        return Headless(self, env)

    def file(self, rel):
        return self.host.read(self.host.join(self.work, rel))

    def put(self, rel, data):
        self.host.write(self.host.join(self.work, rel), data)

    def requests(self, model=None):
        return fake_provider.STATE.log(model or self.model)

    def compactions(self):
        return fake_provider.STATE.log(self.model + "#compact")

    def check(self, cond, msg):
        if not cond:
            raise Failure(msg)

    def seq(self, r, names):
        """The events appear in this order, not necessarily adjacent."""
        got = r.names()
        i = 0
        for ev in got:
            if i < len(names) and ev == names[i]:
                i += 1
        if i < len(names):
            raise Failure(f"expected the sequence {names}, missing {names[i]!r} "
                          f"in {compact(got)}")

    def pids(self, rel):
        """The pids in a file of the work directory, one per line."""
        return P.parse(self.file(rel))

    def gone(self, pids, within=5.0):
        deadline = time.monotonic() + within
        while time.monotonic() < deadline:
            left = [p for p in pids if self.host.alive(p)]
            if not left:
                return True
            time.sleep(0.2)
        return False


class Headless:
    def __init__(self, t, env=None):
        self.t = t
        self.p = t.host.popen(["--headless"] + t.args(), t.work, stdin=True, env=env)
        self.events = []
        self.cond = threading.Condition()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        self.id = 0

    def read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            with self.cond:
                self.events.append(ev)
                self.cond.notify_all()

    def send(self, cmd, **fields):
        self.id += 1
        obj = {"cmd": cmd, "id": self.id}
        obj.update(fields)
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()
        return self.id

    def wait(self, pred, timeout=20):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for ev in self.events:
                    if pred(ev):
                        return ev
                left = deadline - time.monotonic()
                if left <= 0:
                    raise Failure(f"timed out waiting; saw {compact([e.get('ev') for e in self.events])}")
                self.cond.wait(left)

    def call(self, cmd, timeout=20, **fields):
        """Send a command and wait for its reply."""
        i = self.send(cmd, **fields)
        return self.wait(lambda e: e.get("ev") == "reply" and e.get("id") == i, timeout)

    def all(self, ev, sid=None):
        with self.cond:
            return [e for e in self.events
                    if e.get("ev") == ev and (sid is None or e.get("sid") == sid)]

    def names(self, sid):
        with self.cond:
            return [e.get("ev") for e in self.events if e.get("sid") == sid]

    def exit(self, timeout=10):
        """Wait for the process to end by itself (after a quit)."""
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise Failure("the harness did not exit")
        self.reader.join(2)
        return self.p.returncode

    def close(self, timeout=10):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.reader.join(2)
        err = self.p.stderr.read()
        if "ThreadSanitizer" in err or "AddressSanitizer" in err:
            raise Failure("sanitizer report:\n" + err[:4000])
        return self.p.returncode


def compact(names):
    out = []
    for n in names:
        if n in ("delta", "live", "progress", "tool.out"):
            continue
        out.append(n)
    return out


# Scenarios

SCENARIOS = {}


def scenario(conf=None, bound=30, posix=False, alone=False):
    """alone: a heavy scenario (big files, a stream of requests), run by
    itself after the rest, so that it slows no timed one on a small host."""
    def deco(fn):
        SCENARIOS[fn.__name__] = (fn, conf, bound, posix, alone)
        return fn
    return deco


@scenario(bound=5)
def plain(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["session", "user", "title", "turn", "attempt", "assistant", "turn.end"])
    t.check(r.last("title").get("text") == "go", f"titled: {r.last('title')}")
    t.check(r.last("turn.end").get("why") == "done", "the turn ends done")
    t.check(r.text() == "Hello! This is a plain answer.", f"text {r.text()!r}")
    t.check(not r.all("retry"), "no retries")
    req = t.requests()[0]["body"]
    t.check(req["stream_options"] == {"include_usage": True}, "include_usage sent")
    t.check(req.get("return_progress") is True, "return_progress for llama.cpp")
    t.check("temperature" not in req, "no sampling parameters unless configured")


@scenario(bound=10)
def read_edit_shell(t):
    t.put("hello.txt", "hello world\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["tool.start", "tool.end", "tool.start", "tool.end",
              "tool.start", "tool.end", "assistant", "turn.end"])
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok", "ok", "ok"],
            f"statuses {[e['status'] for e in ends]}")
    t.check(t.file("hello.txt") == "hello there\n", "the edit landed")
    t.check("hello there" in ends[2]["text"] and "[exit code 0]" in ends[2]["text"],
            "the shell saw the edit")
    t.check(ends[2].get("spill", "").startswith("out/"), "shell output spilled")
    # The prompt prefix is byte-stable: each request extends the last
    reqs = [x["body"]["messages"] for x in t.requests()]
    for a, b in zip(reqs, reqs[1:]):
        t.check(b[:len(a)] == a, "each request extends the previous one")


@scenario(bound=10)
def bytewise(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")
    t.check(end and end[0]["status"] == "ok" and "split" in end[0]["text"],
            "a call assembled from single bytes runs")
    t.check(r.text().endswith("Every byte arrived on its own."), f"text {r.text()!r}")
    t.check(not r.all("retry"), "no retries")


@scenario(conf={"idle_timeout": 3, "progress_timeout": 5}, bound=20)
def pings(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(not r.all("retry"), "pings and progress keep the request alive")
    t.check(r.all("progress"), "prompt progress is reported")
    t.check(r.elapsed >= 8.5, f"the prefill took its time ({r.elapsed:.1f} s)")
    t.check(r.text() == "Prefill finished.", f"text {r.text()!r}")


@scenario(conf={"first_byte_timeout": 2}, bound=10)
def stall_head(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["attempt", "retry", "attempt", "assistant", "turn.end"])
    rt = r.last("retry")
    t.check("no response" in rt.get("msg", ""), f"retry msg {rt.get('msg')!r}")
    t.check(len(t.requests()) == 2, "two requests")


@scenario(conf={"idle_timeout": 2}, bound=10)
def stall_think(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["attempt", "retry", "attempt", "assistant", "turn.end"])
    t.check(not r.all("nudge"), "reasoning alone is retried, not continued")
    t.check(len(r.all("assistant")) == 1, "only the second attempt is committed")
    t.check(r.text() == "Recovered from a stall.", f"text {r.text()!r}")


@scenario(conf={"idle_timeout": 2}, bound=10)
def stall_text(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["assistant", "nudge", "attempt", "assistant", "turn.end"])
    first = r.all("assistant")[0]
    t.check(first["finish"] == "interrupted", "the partial is committed as interrupted")
    t.check(r.last("nudge").get("kind") == "continue", "a continue nudge")
    t.check(not r.all("retry"), "visible text is never retried")
    t.check(r.text() == "The answer is forty-two.", f"no duplicated text: {r.text()!r}")
    second = t.requests()[1]["body"]["messages"]
    t.check(any(m.get("role") == "assistant" and m.get("content") == "The answer is"
                for m in second), "the continuation sees the partial text")


@scenario(bound=10)
def drop_args(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(t.file("count.txt") == "once\n", f"executed exactly once: {t.file('count.txt')!r}")
    t.check(len(r.all("tool.start")) == 1, "one tool run")


@scenario(bound=10)
def retry_after_2(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    rt = r.last("retry")
    t.check(rt.get("status") == 429 and rt.get("delay_ms", 0) >= 2000,
            f"waited as asked: {rt}")
    reqs = t.requests()
    t.check(len(reqs) == 2 and reqs[1]["t"] - reqs[0]["t"] >= 1.95,
            "the second request came 2 s later")


@scenario(conf={"slow_retry_budget": 600}, bound=5)
def retry_after_3600(t):
    r = t.run()
    t.check(r.rc == 1, f"exit {r.rc}")
    end = r.last("turn.end")
    t.check(end.get("why") == "error" and "3600" in end.get("err", ""),
            f"fails fast: {end}")
    t.check(not r.all("retry") and len(t.requests()) == 1, "no retry")


@scenario(bound=5)
def parse500(t):
    r = t.run()
    t.check(r.rc == 1, f"exit {r.rc}")
    end = r.last("turn.end")
    t.check(end.get("err", "").startswith("deterministic"), f"deterministic: {end}")
    t.check(len(t.requests()) == 1, "never retried")


@scenario(bound=10)
def x503x3(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    delays = [e["delay_ms"] for e in r.all("retry")]
    t.check(len(delays) == 3, f"three retries: {delays}")
    for d, base in zip(delays, [500, 1000, 2000]):
        t.check(0.75 * base <= d <= base, f"delay {d} within [{0.75*base}, {base}]")
    times = [x["t"] for x in t.requests()]
    gaps = [b - a for a, b in zip(times, times[1:])]
    for g, d in zip(gaps, delays):
        t.check(d / 1000 - 0.05 <= g <= d / 1000 + 0.5, f"gap {g:.3f} s for delay {d} ms")


@scenario(bound=8)
def midstream_error(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    rt = r.last("retry")
    t.check("Context size has been exceeded" in rt.get("msg", ""), f"retry {rt}")
    t.check(r.text() == "Second try worked.", f"text {r.text()!r}")


@scenario(bound=5)
def overflow(t):
    # A lone prompt too long, by the server's count: nothing to compact
    r = t.run()
    t.check(r.rc == 1, f"exit {r.rc}")
    end = r.last("turn.end")
    t.check(end.get("err", "").startswith("overflow"), f"classified as overflow: {end}")
    t.check("compacting cannot" in end.get("err", ""), f"explained: {end}")
    t.check(len(t.requests()) == 1, "not retried")
    t.check(not t.compactions(), "nothing to summarize")


@scenario(bound=8)
def malformed(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    rt = r.last("retry")
    t.check("malformed" in rt.get("msg", ""), f"retry {rt}")
    t.check(r.text() == "Clean this time.", f"text {r.text()!r}")


@scenario(bound=10)
def repair(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [e["status"] for e in r.all("tool.end")]
    t.check(st == ["ok", "ok", "error"], f"statuses {st}")
    t.check(t.file("r.txt") == "line1\nline2\n", f"raw newline repaired: {t.file('r.txt')!r}")
    t.check(t.file("s.txt") == "ok\n", "double-encoded arguments repaired")
    t.check(t.file("x.txt") is None, "truncated arguments not acted on")
    err = r.all("tool.end")[2]["text"]
    t.check("Invalid arguments" in err and "call the tool again" in err, f"how to fix: {err}")
    # Later requests carry valid JSON arguments, never the broken text
    for m in t.requests()[-1]["body"]["messages"]:
        for c in m.get("tool_calls", []):
            json.loads(c["function"]["arguments"])


@scenario(bound=8)
def unknown_tool(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "error" and 'Did you mean "shell"' in end["text"],
            f"suggests the right tool: {end['text']}")


@scenario(bound=10)
def same_call(t):
    t.put("loop.txt", "same\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [e["status"] for e in r.all("tool.end")]
    t.check(st == ["ok"] * 4 + ["error"] * 2, f"the guard trips at 5: {st}")
    t.check([n["kind"] for n in r.all("nudge")] == ["guard", "guard"],
            f"redirects: {[n['kind'] for n in r.all('nudge')]}")
    t.check(r.last("turn.end").get("why") == "done", "the turn still ends well")


@scenario(bound=10)
def cycle(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.last("nudge").get("kind") == "loop", "a loop nudge")
    first = r.all("assistant")[0]
    text = "".join(b.get("text", "") for b in first["blocks"])
    t.check(first["finish"] == "interrupted" and text.count("I will check") <= 2,
            f"the cycle is cut away: {text!r}")
    t.check(r.text().endswith("Checked it."), f"text {r.text()!r}")


@scenario(bound=8)
def empty(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    kinds = [n["kind"] for n in r.all("nudge")]
    t.check(kinds == ["empty", "empty"], f"nudges {kinds}")
    t.check(r.text() == "Finally something.", f"text {r.text()!r}")


@scenario(bound=8)
def length_think(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check([n["kind"] for n in r.all("nudge")] == ["length"], "a length nudge")
    t.check(r.all("assistant")[0]["finish"] == "length", "the stop is recorded")


@scenario(conf={"_tools": {"job_timeout": 3}}, bound=15)
def shell_timeout(t):
    """A command still running at its timeout moves to the background as
    a job; at the job's limit it is stopped, which starts a turn to tell
    the agent, and run exits after that turn."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "background" and end.get("job") == 1 and
            "first" in end["text"] and "background as job 1" in end["text"],
            f"moved to the background: {end}")
    t.check(end["ms"] < 2500, f"returned at its timeout: {end['ms']} ms")
    js = r.last("job.start")
    t.check(js.get("job") == 1 and js.get("call") == end["call"] and
            js.get("spill") == end.get("spill"), f"job.start {js}")
    je = r.last("job.end")
    t.check(je.get("status") == "timeout" and je.get("resume") == 1 and
            je.get("notice") == 1, f"job.end {je}")
    t.seq(r, ["tool.end", "assistant", "turn.end", "job.end", "turn", "notice",
              "attempt", "assistant", "turn.end"])
    turns = r.all("turn")
    t.check(len(turns) == 2 and turns[1].get("via") == "job", f"turns {turns}")
    notice = r.last("notice")
    t.check(notice.get("kind") == "job" and notice.get("job") == 1 and
            "stopped at its time limit" in notice.get("text", "") and
            "first" in notice.get("text", ""), f"notice {notice}")
    msgs = t.requests()[-1]["body"]["messages"]
    t.check(msgs[-1]["role"] == "user" and
            msgs[-1]["content"].startswith("Background job 1 ended"),
            f"the agent is told: {msgs[-1]}")
    pids = t.pids("pid1") + t.pids("pid2")
    t.check(len(pids) == 2, f"pids recorded: {pids}")
    t.check(t.gone(pids), f"the tree is gone: {pids}")
    t.check(r.elapsed < 10, f"stopped at the job limit: {r.elapsed:.1f} s")


@scenario(bound=10)
def job_notice(t):
    """A job started with background ends after the turn: its end starts
    a turn by itself, with the end of its output."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "ok" and end.get("job") == 1 and
            end["text"].startswith("Started background job 1"), f"started: {end}")
    t.seq(r, ["job.start", "tool.end", "assistant", "turn.end", "job.end", "turn",
              "notice", "attempt", "assistant", "turn.end"])
    first = r.all("turn.end")[0]
    t.check(not first.get("resume"), f"the job still ran at the turn's end: {first}")
    je = r.last("job.end")
    t.check(je.get("status") == "exited" and je.get("exit") == 0 and je.get("resume") == 1,
            f"job.end {je}")
    text = r.last("notice").get("text", "")
    t.check("exited with code 0" in text and "done-42" in text, f"notice {text!r}")
    t.check("Saw: Background job 1 ended: it exited with code 0" in r.text(),
            f"text {r.text()!r}")


@scenario(bound=10)
def job_tools(t):
    """jobs, job_output and job_kill; a job the agent stops is not
    reported to it again."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    st = [e["status"] for e in ends]
    t.check(st == ["ok", "ok", "ok", "error", "ok"], f"statuses {st}")
    t.check("job 1: running for" in ends[1]["text"] and "jobpid" in ends[1]["text"],
            f"jobs: {ends[1]['text']}")
    out = ends[2]["text"]
    t.check("lines 1-2:\nline1\nline2\n" in out and "offset=3" in out, f"job_output: {out!r}")
    t.check("There is no job 7" in ends[3]["text"], f"no such job: {ends[3]['text']}")
    t.check("asked to stop" in ends[4]["text"], f"job_kill: {ends[4]['text']}")
    je = r.last("job.end")
    t.check(je.get("status") == "killed" and je.get("notice") == 0 and
            not je.get("resume"), f"job.end {je}")
    t.check(not r.all("notice") and len(r.all("turn")) == 1, "nothing more to tell")
    t.check(t.gone(t.pids("jobpid")), "the job's process is gone")


@scenario(bound=10)
def job_busy(t):
    """A job that ends while its turn still runs is told at the next step
    of that turn."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.seq(r, ["job.start", "tool.end", "job.end", "tool.end", "notice", "attempt",
              "assistant", "turn.end"])
    t.check(len(r.all("turn")) == 1, "one turn")
    t.check(not r.last("job.end").get("resume"), "no turn of its own")
    t.check("Saw: Background job 1 ended: it exited with code 0" in r.text(),
            f"text {r.text()!r}")


@scenario(conf={"_tools": {"stop_grace": 5}}, bound=15)
def soft_stop(t):
    """Cancelling asks the command to stop (SIGTERM, or CTRL_BREAK on
    Windows), which ends it long before the kill would."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("softpid"):
            time.sleep(0.05)
        pids = t.pids("softpid")
        t0 = time.monotonic()
        h.call("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "cancel", f"cancelled: {end}")
        t.check(time.monotonic() - t0 < 1, "the turn ends at once")
        t.check(pids and t.gone(pids, within=3.0), f"gone before the kill: {pids}")
        dt = time.monotonic() - t0
        t.check(dt < 4, f"stopped by the soft stop: {dt:.1f} s")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_tools": {"stop_grace": 1}}, bound=10, posix=True)
def stop_grace(t):
    """A command that ignores SIGTERM is killed when the grace period is
    over."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("gracepid"):
            time.sleep(0.05)
        pids = t.pids("gracepid")
        time.sleep(0.3)
        t0 = time.monotonic()
        h.call("cancel", sid=sid)
        time.sleep(0.6)
        t.check(pids and t.host.alive(pids[0]), "it survives the TERM")
        t.check(t.gone(pids, within=3.0), f"killed after the grace: {pids}")
        dt = time.monotonic() - t0
        t.check(0.9 < dt < 3, f"at the grace period: {dt:.1f} s")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def crash_job(t):
    """A job dies with the harness; opening the session again closes it
    as unknown, and the agent hears of it at the next prompt."""
    state = {}

    def watch(ev, p):
        if ev.get("ev") != "job.start":
            return False
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("jobpid"):
            time.sleep(0.05)
        state["pids"] = t.pids("jobpid")
        ppid = t.pids("jobppid")
        if t.host.windows and ppid:
            t.host.killhard(p, ppid[0])
        else:
            t.host.killhard(p)
        return True

    t.put("jobppid", "")
    r1 = t.run(watch=watch)
    sid = r1.events[0].get("sid")
    t.check(state.get("pids") and t.gone(state["pids"]), "the job died with the harness")
    r2 = t.run("continue", resume=sid)
    t.check(r2.rc == 0, f"exit {r2.rc}: {r2.stderr}")
    je = r2.all("job.end")
    t.check(je and je[0].get("status") == "unknown", f"closed as unknown: {je}")
    t.seq(r2, ["job.end", "user", "turn", "notice", "attempt", "assistant", "turn.end"])
    t.check("the harness stopped while it ran" in r2.last("notice").get("text", ""),
            f"notice {r2.last('notice')}")


@scenario(bound=15)
def job_cmds(t):
    """job.kill from the frontend stops a job, of which the agent is
    told; closing the session stops the rest, quietly."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not (t.pids("job1pid") and t.pids("job2pid")):
            time.sleep(0.05)
        p1, p2 = t.pids("job1pid"), t.pids("job2pid")
        t.check(len(h.all("job.start", sid)) == 2, "two jobs")
        r = h.call("list")
        entry = [s for s in h.all("sessions")[-1]["list"] if s["sid"] == sid]
        t.check(entry and entry[0].get("jobs") == 2, f"listed with its jobs: {entry}")

        r = h.call("job.kill", sid=sid, job=1)
        t.check(r.get("ok") == 1, f"job.kill {r}")
        je = h.wait(lambda e: e.get("ev") == "job.end" and e.get("job") == 1)
        t.check(je["status"] == "killed" and je.get("notice") == 1 and je.get("resume") == 1,
                f"job 1 ended {je}")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        n = h.all("notice", sid)
        t.check(n and "the user stopped it" in n[-1]["text"], f"told: {n}")
        t.check(t.gone(p1), "job 1's process is gone")
        r = h.call("job.kill", sid=sid, job=9)
        t.check(r.get("ok") == 0, f"no job 9: {r}")

        h.call("close", sid=sid)
        je = h.wait(lambda e: e.get("ev") == "job.end" and e.get("job") == 2)
        t.check(je["status"] == "closed" and je.get("notice") == 0, f"job 2 ended {je}")
        t.check(t.gone(p2), "job 2's process is gone")
        time.sleep(0.3)
        t.check(len(h.all("turn", sid)) == 2, "no turn after closing")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def job_quit(t):
    """Quitting kills the jobs."""
    h = t.headless()
    model = t.model.replace(":job_quit:", ":job_cmds:")
    fake_provider.STATE.register(model)
    sid = h.call("new", cwd=t.work, model=model, endpoint="fake")["sid"]
    h.send("prompt", sid=sid, text="go")
    h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not (t.pids("job1pid") and t.pids("job2pid")):
        time.sleep(0.05)
    pids = t.pids("job1pid") + t.pids("job2pid")
    rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    t.check(len(pids) == 2 and t.gone(pids), f"the jobs are gone: {pids}")


@scenario(conf={"_tools": {"auto_resume": 1}}, bound=15)
def resume_cap(t):
    """Turns a session starts by itself are capped: past the cap, the
    end of a job waits for the next prompt."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        je = h.wait(lambda e: e.get("ev") == "job.end" and e.get("job") == 1)
        t.check(je.get("resume") == 1, f"the first resumes: {je}")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        je = h.wait(lambda e: e.get("ev") == "job.end" and e.get("job") == 2)
        t.check(not je.get("resume"), f"the second does not: {je}")
        note = h.wait(lambda e: e.get("ev") == "note" and e.get("sid") == sid)
        t.check("auto_resume" in note["text"], f"note {note}")
        time.sleep(0.3)
        t.check(len(h.all("turn", sid)) == 2, "no third turn by itself")
        h.send("prompt", sid=sid, text="what happened?")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 3)
        last = t.requests()[-1]["body"]["messages"]
        t.check("Background job 2 ended" in last[-1]["content"] and
                last[-2]["content"] == "what happened?", f"told at the prompt: {last[-2:]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def rewind_reopen(t):
    """Snapshots survive the process: a session opened again rewinds what
    the last one changed."""
    model = t.model.replace(":rewind_reopen:", ":rewind:")
    fake_provider.STATE.register(model)
    r = t.run("first", extra=["-m", model])
    t.check(r.rc == 0 and t.file("a.txt") == "two\n" and t.file("b.txt") == "new\n",
            f"written: {r.rc} {r.stderr}")
    sid = r.events[0]["sid"]
    h = t.headless()
    try:
        o = h.call("open", sid=sid, **{"from": 0})
        t.check(o.get("ok") == 1, f"opened {o}")
        rr = h.call("rewind", sid=sid, to=1)
        got = [(os.path.basename(f["path"]), f["result"]) for f in rr.get("files", [])]
        t.check(got == [("b.txt", "removed"), ("a.txt", "removed")], f"rewound {rr}")
        t.check(t.file("a.txt") is None and t.file("b.txt") is None, "both gone")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def instructions_changed(t):
    """An instruction file changed between turns: the agent gets its new
    text as a note, and the system prompt stays as it was."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("AGENTS.md", "Rule one.\n")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="first")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.put("AGENTS.md", "Rule two, longer.\n")
        t.host.touch(t.host.join(t.work, "AGENTS.md"), time.time() + 10)
        h.send("prompt", sid=sid, text="second")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        reqs = [x["body"]["messages"] for x in t.requests()]
        t.check(reqs[0][0]["content"] == reqs[1][0]["content"] and
                "Rule one." in reqs[0][0]["content"], "the system prompt stays")
        last = reqs[1][-1]["content"]
        t.check(last.startswith("Instruction files changed") and "Rule two, longer." in last,
                f"told: {last!r}")
        ins = h.all("instructions", sid)
        t.check([i["why"] for i in ins] == ["start", "changed"], f"records {ins}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


SEARCH_FILES = {
    ".gitignore": "ignored/\n*.log\n",
    "src/a.c": "int main(void)\n{\n    // TODO: fix\n}\n",
    "src/b.h": "void hello(void);\n",
    "src/c.c": "static int helper(void) { return 0; }\n",
    "ignored/x.c": "int main(void) {}\n",
    "trace.log": "main(\n",
    "deep/a/b/n.txt": "needle\n",
}

SEARCH_WANT = [
    ("ok", "src/a.c:1: int main(void)\n"),
    ("ok", "src/a.c:3:     // TODO: fix\n"),
    ("ok", "src/b.h:1: void hello(void);\n"),
    ("error", "The search failed"),
    ("ok", "No lines match a+b in the working directory."),
    ("ok", "src/a.c\nsrc/c.c\n"),
    ("ok", "src/a.c\nsrc/c.c\nsrc/b.h\n"),
    ("ok", "No files match *.nothing in the working directory."),
    ("ok", "deep/a/b/n.txt:1: needle\n"),
    ("denied", ""),
]


def searchsetup(t):
    t.host.mkdir(t.host.join(t.work, ".git"))
    files = dict(SEARCH_FILES)
    files["bin.dat"] = b"int main(\0\0\n"
    files["../outside"] = "x outside\n"
    # Newest first: a.c, then c.c, then b.h
    now = time.time()
    times = {rel: now - age for rel, age in [("src/a.c", 10), ("src/c.c", 20),
                                             ("src/b.h", 30)]}
    t.host.bulk(t.work, files, times)


def searchcheck(t, r):
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    t.check(len(ends) == len(SEARCH_WANT), f"steps {compact(r.names())}")
    for i, (e, (status, want)) in enumerate(zip(ends, SEARCH_WANT)):
        t.check(e["status"] == status, f"step {i}: status {e['status']}: {e['text']}")
        if status == "ok":
            t.check(e["text"].startswith(want) if want.endswith(".") else e["text"] == want,
                    f"step {i}: {e['text']!r}, want {want!r}")
        else:
            t.check(want in e["text"], f"step {i}: {e['text']!r}, want {want!r}")
        t.check(not e.get("spill") or e["spill"].startswith("out/"), "spilled")


@scenario(bound=15)
def search(t):
    """grep and glob through ripgrep where it is found: .gitignore and
    binary files skipped, globs as .gitignore takes them, newest first."""
    searchsetup(t)
    searchcheck(t, t.run())


@scenario(conf={"_tools": {"use_rg": "false"}}, bound=15)
def search_builtin(t):
    """The same searches through the built-in walker, with the same
    results."""
    searchsetup(t)
    searchcheck(t, t.run())


def manysetup(t):
    now = time.time()
    files = {f"f{i:03}.txt": "line\n" for i in range(150)}
    times = {f"f{i:03}.txt": now - 1000 + i for i in range(150)}
    t.host.bulk(t.work, files, times)


def manycheck(t, r):
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    g, f = r.all("tool.end")[:2]
    lines = g["text"].splitlines()
    t.check(len(lines) == 101 and lines[0] == "f000.txt:1: line" and
            lines[99] == "f099.txt:1: line" and "there are more" in lines[100],
            f"grep: {lines[:2]} ... {lines[-2:]}")
    lines = f["text"].splitlines()
    t.check(len(lines) == 101 and lines[0] == "f149.txt" and lines[99] == "f050.txt" and
            "there are more" in lines[100], f"glob: {lines[:2]} ... {lines[-2:]}")


@scenario(bound=15)
def search_many(t):
    """Results stop at 100, saying that there are more."""
    manysetup(t)
    manycheck(t, t.run())


@scenario(conf={"_tools": {"use_rg": "false"}}, bound=15)
def search_many_builtin(t):
    manysetup(t)
    manycheck(t, t.run())


@scenario(bound=10)
def todo(t):
    """A turn that ends with todo items left is nudged to go on; the list
    given as a JSON string is taken too."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    kinds = [n["kind"] for n in r.all("nudge")]
    t.check(kinds == ["todo"], f"nudges {kinds}")
    text = r.last("nudge")["text"]
    t.check("[>] write it" in text and "[ ] test it" in text, f"nudge {text!r}")
    todos = r.all("todo")
    t.check(len(todos) == 2 and all(i["status"] == "done" for i in todos[1]["items"]),
            f"todo records {todos}")
    ends = r.all("tool.end")
    t.check(ends[0]["text"].startswith("Todo list, 0 of 2 done") and
            ends[1]["text"].startswith("Todo list, 2 of 2 done"), "the lists shown")
    t.check(r.last("turn.end").get("why") == "done", "the turn ends")


@scenario(bound=10)
def todo_job(t):
    """Ending a turn to wait for a background job is not nudged about
    the todo list: the job's end starts the next turn."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(not r.all("nudge"), f"no nudges: {r.all('nudge')}")
    turns = r.all("turn")
    t.check(len(turns) == 2 and turns[1].get("via") == "job", f"turns {turns}")
    t.check(r.text().endswith("It ran."), f"text {r.text()!r}")


@scenario(bound=10)
def todo_cap(t):
    """Never finishing the list: three nudges, then the turn ends."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    kinds = [n["kind"] for n in r.all("nudge")]
    t.check(kinds == ["todo"] * 3, f"nudges {kinds}")
    t.check(r.last("turn.end").get("why") == "done", "the turn ends anyway")


@scenario(bound=15)
def todo_remind(t):
    """Twelve changes without the list changing: one reminder."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    kinds = [n["kind"] for n in r.all("nudge")]
    t.check(kinds == ["todo_reminder"], f"nudges {kinds}")
    names = r.names()
    at = names.index("nudge")
    t.check(sum(1 for e in r.events[:at] if e.get("ev") == "tool.end") == 13,
            "after the twelfth change")


@scenario(bound=20)
def rewind(t):
    """rewind puts back what write and edit changed, newest first, and
    leaves alone a file changed since, unless forced; the agent hears of
    it at its next request."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]

        def turn(text, n):
            h.send("prompt", sid=sid, text=text)
            h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid
                   and e.get("turn") == n)

        turn("first", 1)
        t.check(t.file("a.txt") == "two\n" and t.file("b.txt") == "new\n", "written")
        snaps = h.all("snap", sid)
        t.check([(s["snap"], s["existed"]) for s in snaps] ==
                [("snap/1", 0), ("snap/2", 1), ("snap/3", 0)], f"snaps {snaps}")
        t.check(snaps[1]["hash_before"] and snaps[0]["hash_before"] == "", "hashes")

        r = h.call("rewind", sid=sid, to="call_1")
        got = [(os.path.basename(f["path"]), f["result"]) for f in r.get("files", [])]
        t.check(r.get("ok") == 1 and got == [("b.txt", "removed"), ("a.txt", "restored")],
                f"rewound: {r}")
        t.check("shell commands" in r.get("note", ""), f"note {r.get('note')}")
        t.check(t.file("a.txt") == "one\n" and t.file("b.txt") is None, "files put back")
        rw = h.all("rewind", sid)[-1]
        t.check(rw.get("undone") == ["snap/3", "snap/2"] and rw.get("to") == "call_1",
                f"rewind record {rw}")
        r = h.call("rewind", sid=sid, to="call_1")
        t.check(r.get("ok") == 1 and r.get("files") == [], f"nothing left: {r}")

        turn("second", 2)
        notes = [m for m in t.requests()[-2]["body"]["messages"] if m.get("role") == "user"]
        t.check(notes[-1]["content"].startswith("The user rewound the files") and
                "a.txt" in notes[-1]["content"], f"the agent is told: {notes[-1]}")
        t.check(t.file("c.txt") == "made in turn 2\n", "c.txt written")
        t.put("c.txt", "changed by hand\n")
        r = h.call("rewind", sid=sid, to=2)
        got = [(os.path.basename(f["path"]), f["result"]) for f in r.get("files", [])]
        t.check(got == [("c.txt", "conflict")], f"conflict: {r}")
        t.check(t.file("c.txt") == "changed by hand\n", "left alone")
        r = h.call("rewind", sid=sid, to=2, force=1)
        got = [(os.path.basename(f["path"]), f["result"]) for f in r.get("files", [])]
        t.check(got == [("c.txt", "removed")] and t.file("c.txt") is None, f"forced: {r}")

        for to, why in [(9, "no turn"), ("call_x", "no call"), (None, "call id or a turn")]:
            r = h.call("rewind", sid=sid, to=to)
            t.check(r.get("ok") == 0 and why in r.get("err", ""), f"refused: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def instructions(t):
    """AGENTS.md from the user and from the repository root down, with
    imports, in the system prompt; a subdirectory's when the agent
    works there, as a note."""
    xdg = t.host.join(t.base, "xdg")
    t.host.mkdir(t.host.join(xdg, "harness"))
    t.host.write(t.host.join(xdg, "harness", "AGENTS.md"), "Global rule: be brief.\n")
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("AGENTS.md", "Root rule: use tabs.\nSee @docs/style.md, not "
          "`@docs/nope.md`, nor @../outside.md.\n")
    t.put("CLAUDE.md", "Not loaded: AGENTS.md comes first.\n")
    t.host.mkdir(t.host.join(t.work, "docs"))
    t.put("docs/style.md", "Style rule: short names.\n")
    t.put("docs/nope.md", "Should not load.\n")
    t.host.write(t.host.join(t.base, "outside.md"), "Outside rule.\n")
    t.host.mkdir(t.host.join(t.work, "pkg"))
    t.put("pkg/CLAUDE.md", "Pkg rule: no globals.\n")
    t.put("pkg/x.txt", "hello\n")

    r = t.run(env={"XDG_CONFIG_HOME": xdg})
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    reqs = [x["body"]["messages"] for x in t.requests()]
    system = reqs[0][0]["content"]
    for want in ("Global rule", "Root rule", "Style rule"):
        t.check(want in system, f"{want} in the system prompt")
    for unwanted in ("Should not load", "Outside rule", "Not loaded", "Pkg rule"):
        t.check(unwanted not in system, f"{unwanted} not in the system prompt")
    t.check(all(m[0]["content"] == system for m in reqs), "the system prompt stays")
    ins = r.all("instructions")
    t.check(len(ins) == 2 and ins[0]["why"] == "start" and ins[1]["why"] == "nested",
            f"records {ins}")
    names = [os.path.basename(f["path"]) for f in ins[0]["files"]]
    t.check(names == ["AGENTS.md", "AGENTS.md", "style.md"], f"loaded {names}")
    t.check(any(s.endswith("outside.md") for s in ins[0].get("skipped", [])), "skipped")
    t.check([os.path.basename(f["path"]) for f in ins[1]["files"]] == ["CLAUDE.md"],
            f"nested {ins[1]}")
    t.check(reqs[1][-1]["role"] == "user" and "Pkg rule" in reqs[1][-1]["content"],
            f"told at the next request: {reqs[1][-1]}")
    t.check(sum("Pkg rule" in (m.get("content") or "") for m in reqs[2]) == 1, "once")


@scenario(bound=10)
def grandchild(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "ok" and "started" in end["text"], f"output kept: {end}")
    t.check(end["ms"] < 3000, f"returned after the drain: {end['ms']} ms")
    t.check("kept its output open" in end["text"], "the cut is explained")
    t.check(t.gone(t.pids("pid3")), "the holder is killed")


@scenario(bound=8)
def outside(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [e["status"] for e in r.all("tool.end")]
    t.check(st == ["denied", "ok"], f"statuses {st}")
    t.check(r.all("ask") and r.last("answer").get("by") == "policy", "asked and denied")
    deny = r.all("tool.end")[0]["text"]
    t.check("outside the working directory" in deny and "x.txt" in deny, f"hint: {deny}")
    t.check(t.host.read(t.host.join(t.work, "..", "x.txt")) is None, "nothing written outside")
    t.check(t.file("x.txt") == "yes\n", "the corrected call succeeds")


@scenario(bound=25)
def crash(t):
    state = {}

    def watch(ev, p):
        if ev.get("ev") == "tool.start":
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not t.pids("crashpid"):
                time.sleep(0.05)
            state["shell"] = t.pids("crashpid")
            # POSIX: the shell's parent is its reaper; Windows: the harness
            ppid = t.pids("crashppid")
            if t.host.windows and ppid:
                t.host.killhard(p, ppid[0])
            else:
                t.host.killhard(p)
            return True
        return False

    t.put("crashppid", "")
    r1 = t.run(watch=watch)
    t.check(r1.all("tool.start"), "the tool started")
    sid = r1.events[0].get("sid")
    t.check(t.gone(state.get("shell", [])), "the tool died with the harness")

    r2 = t.run("continue", resume=sid)
    t.check(r2.rc == 0, f"exit {r2.rc}: {r2.stderr}")
    t.seq(r2, ["tool.end", "turn.end", "user", "turn", "assistant", "turn.end"])
    first = r2.events[0]
    t.check(first.get("ev") == "tool.end" and first.get("status") == "unknown",
            f"the tool is closed as unknown: {first}")
    t.check(r2.all("turn.end")[0].get("why") == "interrupted", "the turn as interrupted")
    msgs = t.requests()[-1]["body"]["messages"]
    t.check(any(m.get("role") == "tool" and "outcome unknown" in m.get("content", "")
                for m in msgs), "the agent sees the unknown outcome")


@scenario(bound=15)
def cancel(t):
    h = t.headless()
    try:
        h.send("new", cwd=t.work, model=t.model, endpoint="fake")
        sid = h.wait(lambda e: e.get("ev") == "reply" and e.get("id") == 1)["sid"]
        h.send("prompt", sid=sid, text="stream")
        h.wait(lambda e: e.get("ev") == "delta")
        time.sleep(0.5)
        h.wait(lambda e: e.get("ev") == "live" and e.get("tps", 0) > 0, 5)
        h.send("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end")
        t.check(end["why"] == "cancel", f"cancelled while streaming: {end}")
        a = [e for e in h.events if e.get("ev") == "assistant"]
        t.check(a and a[-1]["finish"] == "cancelled", "the partial text is kept")
        lives = [e for e in h.all("live", sid) if e.get("state") == "streaming"]
        t.check(len(lives) >= 2 and all("idle_ms" in e and "tps" in e for e in lives),
                f"live while streaming, every second: {lives}")
        tps = max(e["tps"] for e in lives)
        t.check(5 <= tps <= 15, f"about 10 tokens per second: {tps}")

        # Then during a tool, in a second session
        model = t.model.replace(":cancel:", ":cancel_tool:")
        fake_provider.STATE.register(model)
        h.send("new", cwd=t.work, model=model, endpoint="fake")
        sid2 = h.wait(lambda e: e.get("ev") == "reply" and e.get("id") == h.id)["sid"]
        h.send("prompt", sid=sid2, text="tool")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid2)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("cancelpid"):
            time.sleep(0.05)
        pids = t.pids("cancelpid")
        h.send("cancel", sid=sid2)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid2)
        t.check(end["why"] == "cancel", f"cancelled during a tool: {end}")
        te = [e for e in h.events if e.get("ev") == "tool.end" and e.get("sid") == sid2]
        t.check(te and te[-1]["status"] == "cancelled", "the tool is cancelled")
        t.check(pids and t.gone(pids), f"its process is gone: {pids}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def asked(t, h, prompt="write outside"):
    """An attended session whose agent asks to write outside: (sid, ask)."""
    sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
    h.send("prompt", sid=sid, text=prompt)
    ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
    t.check(ask.get("kind") == "path" and ask.get("dflt") == "deny" and
            ask.get("tool") == "write" and ask.get("path", "").endswith("asked.txt"),
            f"the ask: {ask}")
    t.check("outside the working directory" in ask.get("detail", ""), f"detail: {ask}")
    h.wait(lambda e: e.get("ev") == "live" and e.get("state") == "ask" and e.get("sid") == sid)
    t.check(not h.all("tool.start", sid), "nothing starts while asking")
    return sid, ask


def outside_file(t):
    return t.host.read(t.host.join(t.work, "..", "asked.txt"))


@scenario(bound=10)
def ask_allow(t):
    h = t.headless()
    try:
        sid, ask = asked(t, h)
        t.check(29000 <= ask["deadline"] - ask["ts"] <= 31000, f"a 30 s deadline: {ask}")
        r = h.call("answer", sid=sid, req=ask["req"], decision="allow", note="ok")
        t.check(r.get("ok") == 1, f"answer accepted: {r}")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn ends done: {end}")
        names = [n for n in h.names(sid) if n in ("ask", "answer", "tool.start", "tool.end")]
        t.check(names == ["ask", "answer", "tool.start", "tool.end"], f"order {names}")
        ans = h.all("answer", sid)[0]
        t.check(ans["decision"] == "allow" and ans["by"] == "user" and ans["note"] == "ok",
                f"answer {ans}")
        te = h.all("tool.end", sid)[0]
        t.check(te["status"] == "ok", f"the write ran: {te}")
        t.check(outside_file(t) == "outside\n", "the file was written outside")
        r = h.call("answer", sid=sid, req=ask["req"], decision="allow")
        t.check(r.get("ok") == 0 and "nothing is waiting" in r.get("err", ""),
                f"a late answer is refused: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def ask_deny(t):
    h = t.headless()
    try:
        sid, ask = asked(t, h)
        r = h.call("answer", sid=sid, req="nope", decision="deny")
        t.check(r.get("ok") == 0 and "no such request" in r.get("err", ""), f"wrong req: {r}")
        r = h.call("answer", sid=sid, req=ask["req"], decision="maybe")
        t.check(r.get("ok") == 0, f"bad decision refused: {r}")
        h.call("answer", sid=sid, req=ask["req"], decision="deny", note="keep it inside")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"a denial does not end the turn: {end}")
        ans = h.all("answer", sid)[0]
        t.check(ans["decision"] == "deny" and ans["by"] == "user", f"answer {ans}")
        st = [e["status"] for e in h.all("tool.end", sid)]
        t.check(st == ["denied", "ok"], f"denied, then the agent went on: {st}")
        deny = h.all("tool.end", sid)[0]["text"]
        t.check("denied by the user" in deny and "keep it inside" in deny, f"why: {deny}")
        t.check(outside_file(t) is None, "nothing written outside")
        t.check(t.file("inside.txt") == "inside\n", "the agent wrote inside instead")
        msgs = t.requests()[1]["body"]["messages"]
        t.check(msgs[-1].get("role") == "tool" and "keep it inside" in msgs[-1].get("content", ""),
                "the agent read the denial")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_guard": {"ask_timeout": 2}}, bound=10)
def ask_timeout(t):
    h = t.headless()
    try:
        sid, ask = asked(t, h)
        t.check(1900 <= ask["deadline"] - ask["ts"] <= 2100, f"the configured deadline: {ask}")
        t0 = time.monotonic()
        ans = h.wait(lambda e: e.get("ev") == "answer" and e.get("sid") == sid, 10)
        waited = time.monotonic() - t0
        t.check(ans["decision"] == "deny" and ans["by"] == "timeout", f"answer {ans}")
        t.check(1.5 <= waited <= 4, f"denied at the deadline: {waited:.1f} s")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn goes on: {end}")
        deny = h.all("tool.end", sid)[0]
        t.check(deny["status"] == "denied" and "nobody answered within 2 seconds" in deny["text"],
                f"why: {deny}")
        t.check(t.file("inside.txt") == "inside\n", "the agent wrote inside instead")
        r = h.call("answer", sid=sid, req=ask["req"], decision="allow")
        t.check(r.get("ok") == 0, f"an answer after the deadline is refused: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def ask_steer(t):
    h = t.headless()
    try:
        sid, ask = asked(t, h)
        h.call("steer", sid=sid, text="no, put it in steered.txt")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn goes on: {end}")
        ans = h.all("answer", sid)[0]
        t.check(ans["decision"] == "deny" and ans["by"] == "steer", f"answer {ans}")
        deny = h.all("tool.end", sid)[0]
        t.check(deny["status"] == "denied" and "sent a new message" in deny["text"],
                f"why: {deny}")
        users = [e for e in h.all("user", sid) if e.get("via") == "steer"]
        t.check(users and users[0]["text"] == "no, put it in steered.txt", "the steer is logged")
        msgs = t.requests()[1]["body"]["messages"]
        t.check(msgs[-2].get("role") == "tool" and msgs[-1].get("role") == "user" and
                msgs[-1].get("content") == "no, put it in steered.txt",
                "the agent reads the denial, then the steer")
        t.check(t.file("steered.txt") == "no, put it in steered.txt\n", "and acts on it")
        t.check(outside_file(t) is None, "nothing written outside")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def ask_close(t):
    h = t.headless()
    try:
        sid, ask = asked(t, h)
        h.call("close", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn goes on unattended: {end}")
        ans = h.all("answer", sid)[0]
        t.check(ans["decision"] == "deny" and ans["by"] == "policy", f"answer {ans}")
        t.check(t.file("inside.txt") == "inside\n", "the agent wrote inside instead")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def readonly(t):
    a = t.headless()
    b = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        a.send("prompt", sid=sid, text="first question for the title")
        a.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        title = a.all("title", sid)
        t.check(title and title[0]["text"] == "first question for the title", f"title {title}")

        # A second process, while the first holds the session
        b = t.headless()
        b.call("list")
        entry = [s for s in b.all("sessions")[-1]["list"] if s["sid"] == sid]
        t.check(entry and entry[0]["locked"] == 1 and
                entry[0]["title"] == "first question for the title", f"listed {entry}")
        r = b.call("open", sid=sid)
        t.check(r.get("ok") == 1 and r.get("readonly") == 1, f"opened read-only: {r}")
        seen = [e.get("seq") for e in b.events if e.get("sid") == sid and e.get("seq")]
        logged = [e.get("seq") for e in a.events if e.get("sid") == sid and e.get("seq")]
        t.check(seen == logged, f"the replay is the log: {seen} vs {logged}")
        r = b.call("prompt", sid=sid, text="second")
        t.check(r.get("ok") == 0 and "read-only" in r.get("err", ""), f"refused: {r}")
        for cmd in ("steer", "cancel", "answer"):
            r = b.call(cmd, sid=sid, text="x", req="x", decision="deny")
            t.check(r.get("ok") == 0 and "read-only" in r.get("err", ""), f"{cmd} refused: {r}")

        # Once the first lets go, the session opens for real
        t.check(a.close() == 0, "the first exits")
        b.call("list")
        entry = [s for s in b.all("sessions")[-1]["list"] if s["sid"] == sid]
        t.check(entry and entry[0]["locked"] == 0, f"unlocked: {entry}")
        r = b.call("open", sid=sid)
        t.check(r.get("ok") == 1 and r.get("readonly") == 0, f"opened for writing: {r}")
        r = b.call("prompt", sid=sid, text="second")
        t.check(r.get("ok") == 1, f"prompt accepted: {r}")
        end = b.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid
                     and e.get("turn") == 2)
        t.check(end["why"] == "done", f"the second turn: {end}")
        t.check(len({e["seq"] for e in b.all("title", sid)}) == 1, "the title is set once")
    finally:
        a.close()
        if b:
            rc = b.close()
            t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def quit_stop(t):
    h = t.headless()
    sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
    h.send("prompt", sid=sid, text="run something long")
    h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not t.pids("quitpid"):
        time.sleep(0.05)
    pids = t.pids("quitpid")
    t0 = time.monotonic()
    # A prompt right behind the quit, in the same write, starts nothing
    qid, late = h.id + 1, h.id + 2
    h.id += 2
    h.p.stdin.write(json.dumps({"cmd": "quit", "mode": "stop", "id": qid}) + "\n" +
                    json.dumps({"cmd": "prompt", "id": late, "sid": sid,
                                "text": "too late"}) + "\n")
    h.p.stdin.flush()
    rc = h.exit(10)
    dt = time.monotonic() - t0
    r = [e for e in h.events if e.get("ev") == "reply" and e.get("id") == qid]
    t.check(r and r[0].get("ok") == 1, f"quit accepted: {r}")
    r = [e for e in h.events if e.get("ev") == "reply" and e.get("id") == late]
    t.check(not r or (r[0].get("ok") == 0 and "stopping" in r[0].get("err", "")),
            f"the late prompt is refused: {r}")
    t.check(not [e for e in h.all("turn", sid) if e.get("n") == 2], "no turn after quit")
    t.check(rc == 0, f"exit {rc}")
    t.check(dt < 3, f"quit within bounds: {dt:.1f} s")
    te = h.all("tool.end", sid)
    t.check(te and te[-1]["status"] == "cancelled", f"the tool was cancelled: {te}")
    end = h.all("turn.end", sid)
    t.check(end and end[-1]["why"] == "cancel", f"the turn ended: {end}")
    t.check(pids and t.gone(pids), f"its process is gone: {pids}")


@scenario(bound=20)
def config(t):
    """config.get and config.set: one-line edits that keep comments, an
    include never touched, refusals, and settings reaching a running
    session, from an edit and from the file changing underneath."""
    team = t.host.join(t.base, "team.ini")
    shared = "# shared\n[model shared]\ntemperature = 0.1\n"
    t.host.write(team, shared)
    lines = ["# scenario settings", "include = team.ini", "",
             "# the fake provider", "[endpoint fake]",
             f"url = {t.host.url}/   # local", "first_byte_timeout = 5",
             "idle_timeout = 3", "progress_timeout = 6", "connect_timeout = 2",
             "colour = blue"]
    t.host.write(t.config, "\n".join(lines) + "\n")
    model = t.model.replace(":config:", ":plain:")
    fake_provider.STATE.register(model)

    def text():
        return t.host.read(t.config).split("\n")[:-1]

    h = t.headless()
    try:
        def call(cmd, **fields):
            i = h.send(cmd, **fields)
            return h.wait(lambda e: e.get("ev") == "reply" and e.get("id") == i)

        def turn(prompt):
            with h.cond:
                done = sum(e.get("ev") == "turn.end" for e in h.events)
            call("prompt", sid=sid, text=prompt)
            deadline = time.monotonic() + 10
            with h.cond:
                while sum(e.get("ev") == "turn.end" for e in h.events) <= done:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise Failure("the turn did not end")
                    h.cond.wait(left)
            return fake_provider.STATE.log(model)[-1]["body"]

        r = call("config.get")
        t.check(r["ok"] == 1 and r["exists"] == 1, f"config.get: {r}")
        t.check(r["path"].endswith("harness.ini") and len(r["files"]) == 2
                and r["files"][1].endswith("team.ini"), f"files {r['files']}")
        d = r["diags"]
        t.check(len(d) == 1 and (d[0]["line"], d[0]["col"], d[0]["severity"])
                == (11, 1, "warning") and "colour" in d[0]["message"],
                f"diagnostics {d}")
        got = {(s["kind"], s["name"], s["key"]): s for s in r["settings"]}
        idle = got[("endpoint", "fake", "idle_timeout")]
        t.check((idle["value"], idle["line"], idle["default"]) == ("3", 8, 0),
                f"idle_timeout {idle}")
        temp = got[("model", "shared", "temperature")]
        t.check(temp["file"].endswith("team.ini") and temp["line"] == 3,
                f"provenance {temp}")
        key = got[("endpoint", "fake", "key")]
        t.check(key["default"] == 1 and key["file"] == "", f"default {key}")
        kinds = {k["kind"] for k in r["kinds"]}
        t.check(len(r["schema"]) >= 20 and
                {"", "endpoint", "model", "role", "guard", "tools",
                 "instructions", "ui"} <= kinds, "schema")

        # One edit, one changed line
        r = call("config.set", kind="endpoint", name="fake",
                 key="idle_timeout", value="4")
        t.check(r["ok"] == 1 and r["line"] == 8, f"config.set: {r}")
        after = text()
        diff = [i for i in range(len(lines)) if lines[i] != after[i]]
        t.check(len(after) == len(lines) and diff == [7] and
                after[7] == "idle_timeout = 4", f"one-line diff: {after}")

        # Refusals change nothing
        for fields, why in [({"key": "colour", "value": "red"}, "unknown key"),
                            ({"key": "idle_timeout", "value": "soon"}, "seconds"),
                            ({"key": "key", "value": "sk-plain"}, "env:NAME")]:
            r = call("config.set", kind="endpoint", name="fake", **fields)
            t.check(r["ok"] == 0 and why in r.get("err", ""), f"refusal: {r}")
            t.check(len(r["diags"]) == 1, "diagnostics come with refusals")
        t.check(text() == after, "refusals leave the file alone")

        # A new section, then a session using it
        r = call("config.set", kind="model", name=model, key="temperature",
                 value="0.25")
        t.check(r["ok"] == 1 and r["line"] == len(lines) + 3, f"new section {r}")
        t.check(text()[len(lines):] == ["", f"[model {model}]",
                                         "temperature = 0.25"], "appended")
        r = call("new", cwd=t.work, model=model, endpoint="fake")
        sid = r["sid"]
        body = turn("one")
        t.check(body.get("temperature") == 0.25, f"edited: {body}")

        # Edited by hand while the session lives: the fingerprint changes
        time.sleep(1.1)
        edited = text()
        edited[-1] = "temperature = 0.75   # by hand"
        t.host.write(t.config, "\n".join(edited) + "\n")
        time.sleep(1.1)
        body = turn("two")
        t.check(body.get("temperature") == 0.75, f"reloaded: {body}")

        r = call("config.set", kind="model", name=model, key="temperature",
                 value=None)
        t.check(r["ok"] == 1 and r["line"] == 0, f"removed: {r}")
        t.check(text() == edited[:-1], "the line is gone")
        body = turn("three")
        t.check("temperature" not in body, f"no temperature: {body}")
        t.check(t.host.read(team) == shared, "the include is never edited")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def config_seed(t):
    """Without a configuration file, the first run writes the seed byte for
    byte, and it loads without a single diagnostic."""
    xdg = t.host.join(t.base, "xdg")
    p = t.host.popen(["--headless", "--state", t.state], t.work, stdin=True,
                     env={"XDG_CONFIG_HOME": xdg})
    out, err = p.communicate('{"cmd":"config.get","id":1}\n', timeout=10)
    replies = [json.loads(x) for x in out.splitlines() if x.strip()]
    r = next((e for e in replies if e.get("id") == 1), {})
    seedpath = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "assets", "harness.ini")
    with open(seedpath, "rb") as f:
        seed = f.read().decode()
    written = t.host.read(t.host.join(xdg, "harness", "harness.ini"))
    t.check(written == seed, "the seed is written unchanged")
    t.check(r.get("ok") == 1 and r.get("diags") == [] and r.get("rev") == 1,
            f"config.get {r} {err}")
    t.check(r["path"].endswith("harness/harness.ini"), f"path {r['path']}")
    t.check(p.returncode == 0, f"exit {p.returncode}: {err}")


@scenario(bound=10)
def long_path(t):
    """Made-up paths cost bounded work: one of 64 KiB is refused as an
    invalid argument, and one of 252 names outside is denied with a hint,
    each within a second of the response."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    asst = r.all("assistant")
    ends = r.all("tool.end")
    t.check(len(asst) == 3 and len(ends) == 2, f"steps {compact(r.names())}")
    t.check(ends[0]["status"] == "error" and "too long" in ends[0]["text"],
            f"refused: {ends[0]['text'][:200]}")
    t.check(ends[1]["status"] == "denied" and "x.txt" in ends[1]["text"],
            f"denied: {ends[1]['text'][:300]}")
    for a, e in zip(asst, ends):
        t.check(e["ts"] - a["ts"] < 1000, f"judged in {e['ts'] - a['ts']} ms")
    t.check(len(r.all("ask")) == 1, "only the valid path is asked about")


@scenario(bound=10)
def deep_unicode(t):
    """A file past MAX_PATH under directories named beyond Latin-1 is
    written, edited, read and found, judged inside the workspace."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok"] * 4,
            f"statuses {[(e['status'], e.get('text', '')[:200]) for e in ends]}")
    t.check(not r.all("ask"), "no ask: the path is inside")
    t.check("deeper €" in ends[2].get("text", ""), f"read: {ends[2].get('text')}")
    leaf = fake_provider.DEEP.rsplit("/", 2)[-2]
    t.check(leaf in ends[3].get("text", ""), f"glob: {ends[3].get('text', '')[:400]}")


@scenario(bound=15)
def steer_tool(t):
    """A steer sent while a command runs answers an ask later in the same
    step at once, by steer, rather than at its deadline."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        t0 = time.monotonic()
        h.call("steer", sid=sid, text="no, put it in steered.txt")
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["deadline"] - ask["ts"] < 100, f"no wait for an answer: {ask}")
        ans = h.wait(lambda e: e.get("ev") == "answer" and e.get("sid") == sid)
        waited = time.monotonic() - t0
        t.check(ans["by"] == "steer" and ans["decision"] == "deny", f"answer {ans}")
        t.check(waited < 5, f"answered right after the command: {waited:.1f} s")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn goes on: {end}")
        st = [e["status"] for e in h.all("tool.end", sid)]
        t.check(st == ["ok", "denied", "ok"], f"statuses {st}")
        t.check(t.file("steered.txt") == "no, put it in steered.txt\n", "the steer is acted on")
        t.check(outside_file(t) is None, "nothing written outside")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def crash_ask(t):
    """The harness dies while asking: the next open closes the call in
    the order a live session would, answer, tool.start, tool.end, and
    only then the turn."""
    a = t.headless()
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        a.send("prompt", sid=sid, text="go")
        a.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        if t.host.windows:
            t.host.killhard(a.p, t.pids("harnesspid")[0])
        else:
            t.host.killhard(a.p)
        a.p.wait()
    finally:
        a.close()
    b = t.headless()
    try:
        r = b.call("open", sid=sid, attended=1, **{"from": 0})
        t.check(r.get("ok") == 1 and r.get("readonly") == 0, f"opened: {r}")
        log = [e for e in b.events if e.get("sid") == sid and e.get("seq")]
        tail = [(e["ev"], e.get("by") or e.get("status") or e.get("why"))
                for e in log[-6:]]
        t.check(tail == [("assistant", None), ("ask", None), ("answer", "cancel"),
                         ("tool.start", None), ("tool.end", "cancelled"),
                         ("turn.end", "interrupted")], f"repaired: {tail}")
        t.check([e["seq"] for e in log] == list(range(1, len(log) + 1)), "in order")
        b.call("prompt", sid=sid, text="go on")
        end = b.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid
                     and e.get("why") != "interrupted")
        t.check(end["why"] == "done", f"the next turn: {end}")
    finally:
        rc = b.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def snapshot(t):
    """Opening a session again while it streams sends a snapshot of the
    response so far, which with the deltas after it makes the response."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "delta" and "word5 " in e.get("text", ""))
        with h.cond:
            seq = max(e.get("seq", 0) for e in h.events if e.get("sid") == sid)
        oid = h.send("open", sid=sid, **{"from": seq})
        h.wait(lambda e: e.get("ev") == "reply" and e.get("id") == oid)
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        with h.cond:
            events = [e for e in h.events if e.get("sid") == sid or e.get("id") == oid]
        at = next(i for i, e in enumerate(events) if e.get("ev") == "snapshot")
        reply = next(i for i, e in enumerate(events) if e.get("id") == oid)
        t.check(at < reply, "the snapshot comes before the reply")
        snap = events[at]
        asst = next(e for e in events if e.get("ev") == "assistant")
        t.check((snap["turn"], snap["attempt"]) == (asst["turn"], asst["attempt"]),
                f"the snapshot's response: {snap}")
        got = {}
        for b in snap["blocks"]:
            got[b["block"]] = [b["kind"], b["text"]]
        for e in events[at + 1:]:
            if e.get("ev") == "delta" and e.get("attempt") == snap["attempt"]:
                got.setdefault(e["block"], [e["kind"], ""])[1] += e["text"]
        want = {}
        for i, b in enumerate(asst["blocks"]):
            kind = {"think": "think", "text": "text", "call": "args"}[b["t"]]
            want[i] = [kind, b.get("args") if b["t"] == "call" else b["text"]]
        t.check(got == want, f"snapshot and deltas {got} vs committed {want}")
        t.check(any(b["kind"] == "text" and b["text"] for b in snap["blocks"]),
                "the snapshot holds text")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def long_command(t):
    """A command line too long to take still gets its reply, an error,
    and the next command is answered as usual."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        r = h.call("prompt", sid=sid, text="x" * (2 << 20))
        t.check(r.get("ok") == 0 and "too long" in r.get("err", ""), f"refused: {r}")
        r = h.call("list")
        t.check(r.get("ok") == 1, f"the next command: {r}")
        t.check(not h.all("turn", sid), "no turn started")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def config_roots(t):
    """[guard] roots set while a session is open reach it: a write there
    no longer asks."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        r = h.call("config.set", kind="guard", name="", key="roots", value=t.base)
        t.check(r.get("ok") == 1, f"config.set: {r}")
        h.send("prompt", sid=sid, text="write outside")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the turn: {end}")
        t.check(not h.all("ask", sid), f"no ask: {h.all('ask', sid)}")
        st = [e["status"] for e in h.all("tool.end", sid)]
        t.check(st == ["ok"], f"written: {st}")
        t.check(outside_file(t) == "outside\n", "the file is in the new root")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10, posix=True)
def config_link(t):
    """config.set edits the file a symlinked configuration points to, and
    the link stays a link."""
    dot = t.host.join(t.base, "dotfiles")
    t.host.mkdir(dot)
    target = t.host.join(dot, "harness.ini")
    os.rename(t.config, target)
    os.symlink(target, t.config)
    h = t.headless()
    try:
        r = h.call("config.set", kind="endpoint", name="fake", key="idle_timeout",
                   value="77")
        t.check(r.get("ok") == 1, f"config.set: {r}")
        t.check(os.path.islink(t.config), "the link is still a link")
        t.check("idle_timeout = 77" in t.host.read(target), "the target has the edit")
        t.check(r.get("path") == t.config, f"the path as given: {r.get('path')}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# M4: compaction, admission, slow retries and fallbacks, reasoning_end,
# unexpected stops

def bigfiles(t, n, lines):
    """Files f0.txt.. of about 78 bytes a line, each different, made on
    the host (a Windows command line is too short to carry them)."""
    fill = "abcdefghij" * 6
    if isinstance(t.host, SshHost):
        t.host.sh("".join(
            f"awk 'BEGIN {{ for (j = 0; j < {lines}; j++) "
            f"printf \"file {i} line %d: {fill}\\n\", j }}' > "
            f"{shlex.quote(t.host.join(t.work, f'f{i}.txt'))}\n" for i in range(n)))
        return
    for i in range(n):
        t.put(f"f{i}.txt", "".join(f"file {i} line {j}: {fill}\n"
                                   for j in range(lines)))


def chat(req):
    return req["body"]["messages"]


def checkpointed(msgs):
    return len(msgs) > 1 and "<conversation-checkpoint>" in str(msgs[1].get("content"))


SMALL = {"_listing": {"meta": {"n_ctx": 24000, "n_ctx_train": 262144}}}


@scenario(conf=SMALL, bound=25, alone=True)
def compaction(t):
    """A small window: the older reads are summarized, the newest stay
    verbatim, the turn carries on, and a resumed session replays the
    compaction to the same prompt."""
    bigfiles(t, 8, 160)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.text() == "All files read.", f"text {r.text()!r}")
    cps = r.all("compact")
    t.check(len(cps) == 1 and cps[0]["kind"] == "summary", f"one summary: {cps}")
    cp = cps[0]
    t.check(cp["why"] == "threshold" and cp["after"] < cp["before"], f"sizes: {cp}")
    t.check("SUMMARY-0" in cp["summary"] and "- read f0.txt" in cp["summary"],
            f"summary with files: {cp['summary']!r}")

    # The summarizer's request: text only, the template, the old messages
    sreq = t.compactions()
    t.check(len(sreq) == 1, f"one summary request: {len(sreq)}")
    body = sreq[0]["body"]
    t.check("tools" not in body, "no tools offered")
    user = body["messages"][1]["content"]
    for want in ("do not call tools", "## Goal", "## Constraints and preferences",
                 "### In progress", "## Critical context", "<conversation>",
                 "[tool call: read]", "file 0 line 1"):
        t.check(want in user, f"summarizer input has {want!r}")
    t.check("file 7 line" not in user, "the newest are not summarized")

    # The checkpoint, then the newest messages verbatim
    reqs = t.requests()
    k = next(i for i, x in enumerate(reqs) if checkpointed(chat(x)))
    post, prev = chat(reqs[k]), chat(reqs[k - 1])
    t.check(f"<summary>\n{cp['summary']}\n</summary>" in post[1]["content"],
            "the checkpoint holds the summary")
    tail = post[2:]
    t.check(len(tail) >= 4 and tail[0]["role"] == "assistant" and
            tail[-1]["role"] == "tool", f"cut at a call: {[m['role'] for m in tail]}")
    t.check(tail[:-2] == prev[len(prev) - len(tail) + 2:], "kept verbatim")
    for a, b in zip(reqs[k:], reqs[k + 1:]):
        t.check(chat(b)[:len(chat(a))] == chat(a), "the prefix is stable again")

    # Resumed: the replayed history is the live one
    sid = r.events[0].get("sid")
    last = chat(reqs[-1])
    r2 = t.run("again", resume=sid)
    t.check(r2.rc == 0, f"resume exit {r2.rc}: {r2.stderr}")
    again = chat(t.requests()[len(reqs)])
    t.check(again[:len(last)] == last, "the replay rebuilds the compacted history")
    t.check(again[len(last):] == [{"role": "assistant", "content": "All files read."},
                                  {"role": "user", "content": "again"}],
            f"then the rest: {again[len(last):]}")


@scenario(conf=SMALL, bound=20, alone=True)
def compact_guard(t):
    """A summary that leaves too little room ends the turn, rather than
    compact again and again."""
    bigfiles(t, 8, 160)
    r = t.run()
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    t.check(len(r.all("compact")) == 1, f"compacted once: {r.all('compact')}")
    notes = [n["text"] for n in r.all("note") if "above 80%" in n.get("text", "")]
    t.check(notes, f"explained: {[n['text'] for n in r.all('note')]}")
    end = r.last("turn.end")
    t.check(end.get("why") == "error" and "enough room" in end.get("err", ""),
            f"turn end: {end}")
    reqs = t.requests()
    t.check(not any(checkpointed(chat(x)) for x in reqs), "no request after it")


@scenario(conf={"_listing": {"meta": {"n_ctx": 100000, "n_ctx_train": 262144}}},
          bound=20, alone=True)
def compact_prune(t):
    """Old tool outputs give way to a line saying where they are, with
    no summary, and the newest 40k tokens of output stay."""
    bigfiles(t, 10, 512)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    cps = r.all("compact")
    t.check(len(cps) == 1 and cps[0]["kind"] == "prune", f"one prune: {cps}")
    el = cps[0]["elided"]
    t.check(len(el) >= 3, f"several elided: {len(el)}")
    t.check(not t.compactions(), "no summary")
    text = el[0]["text"]
    t.check(text.startswith("[output elided: ") and "out/" in text, f"elision: {text}")
    path = text.split("full output in ", 1)[1].rstrip("]")
    full = t.host.read(path)
    t.check(full and "file " in full and " line 0: " in full, f"the output is kept in {path}")
    reqs = t.requests()
    k = next(i for i, x in enumerate(reqs)
             if any(str(m.get("content", "")).startswith("[output elided")
                    for m in chat(x)))
    tools = [m for m in chat(reqs[k]) if m.get("role") == "tool"]
    t.check(not tools[-1]["content"].startswith("[output elided"),
            "the newest output stays")
    t.check(r.text() == "Pruned and done.", f"text {r.text()!r}")


@scenario(bound=20, alone=True)
def overflow_compact(t):
    """The server rejects a prompt as too long: compact to 70% of it,
    then retry the step."""
    bigfiles(t, 4, 100)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    cps = r.all("compact")
    t.check(cps and cps[-1]["why"] == "overflow", f"compacted for it: {cps}")
    reqs = t.requests()
    t.check(len(reqs) == 6, f"the step retried once: {len(reqs)}")
    t.check(len(json.dumps(chat(reqs[5]))) < len(json.dumps(chat(reqs[4]))),
            "the retry is smaller")
    t.check(checkpointed(chat(reqs[5])), "with a checkpoint")
    t.check(r.text() == "Fits now.", f"text {r.text()!r}")


@scenario(conf={"_path": "router1/"}, bound=20)
def queue(t):
    """A router with --models-max 1: a session on another model waits its
    turn, queued, while one on the same model joins the first."""
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        c = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "streaming")
        h.send("prompt", sid=b, text="go")
        q = h.wait(lambda e: e.get("sid") == b and e.get("ev") == "live" and
                   e.get("state") == "queued")
        t.check(q.get("queue_pos") == 1 and q.get("queue_len") == 1, f"queued: {q}")
        h.send("prompt", sid=c, text="go")
        for s in (a, b, c):
            end = h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
            t.check(end["why"] == "done", f"{s}: {end}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    one = t.requests()
    two = t.requests(t.model2)
    t.check(len(one) == 2 and len(two) == 1, f"requests {len(one)} {len(two)}")
    t.check(one[1]["t"] < one[0]["end"], "the same model ran side by side")
    t.check(two[0]["t"] >= max(x["end"] for x in one) - 0.05,
            "the other model waited for both")
    with fake_provider.STATE.lock:
        peak = fake_provider.STATE.peak.get("router1")
    t.check(peak == 1, f"one model at a time: {peak}")


MINUTE = {"HARNESS_TEST_MINUTE_MS": "60"}  # a minute in 60 ms


@scenario(bound=15)
def slow_retry(t):
    """After the quick retries, slow ones wait minutes (here, scaled),
    with the quick ones again after each."""
    r = t.run(env=MINUTE)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    slow = [e["delay_ms"] for e in r.all("retry") if e.get("slow")]
    quick = [e for e in r.all("retry") if not e.get("slow")]
    t.check(slow == [60, 120], f"slow delays {slow}")
    t.check(len(quick) == 20, f"quick retries {len(quick)}")
    t.check(all(e.get("model") == t.model for e in r.all("retry")), "the model named")
    t.check(r.text() == "Up again.", f"text {r.text()!r}")


@scenario(bound=25, alone=True)
def slow_budget(t):
    """The default schedule, 1 2 5 10 15 15... minutes, until the 2 hour
    budget is spent (here, scaled)."""
    r = t.run(env=MINUTE)
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    slow = [e["delay_ms"] for e in r.all("retry") if e.get("slow")]
    t.check(slow[:5] == [60, 120, 300, 600, 900] and set(slow[5:]) <= {900},
            f"the schedule {slow}")
    # Quick retries take time too, more of it over a tunnel to Windows
    t.check(sum(slow) <= 7200 and len(slow) >= 6, f"the budget {slow}")
    end = r.last("turn.end")
    t.check("gave up after" in end.get("err", ""), f"turn end {end}")


@scenario(conf={"_extra": "[model {model}]\nfallback = {model2}\n"}, bound=15)
def fallback(t):
    """Quick retries spent, the fallback takes over, and after the
    cooldown (scaled) the primary is tried again."""
    r = t.run(env=MINUTE)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    sw = [e for e in r.all("retry") if e.get("fallback")]
    t.check(len(sw) == 1 and sw[0]["model"] == t.model2, f"switched: {sw}")
    models = [e["model"] for e in r.all("attempt")]
    t.check(models == [t.model] * 11 + [t.model2, t.model], f"attempts {models}")
    t.check(any("trying the primary model" in n.get("text", "") for n in r.all("note")),
            "the return is noted")
    t.check([e["status"] for e in r.all("tool.end")] == ["ok"], "the fallback worked")
    t.check(r.text() == "Back on the primary.", f"text {r.text()!r}")


@scenario(conf={"_extra": "[model {model}]\nfallback = fake/{model2}\n"}, bound=8)
def limit(t):
    """A usage limit goes straight to the fallback, here given as
    ENDPOINT/MODEL."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    rt = r.all("retry")
    t.check(len(rt) == 1 and rt[0]["class"] == "limit" and rt[0]["fallback"] == 1,
            f"retries {rt}")
    t.check(len(t.requests()) == 1, "no quick retries")
    t.check(r.text() == "The fallback answered.", f"text {r.text()!r}")


def controls(t):
    with fake_provider.STATE.lock:
        return [c for c in fake_provider.STATE.controls if c.get("model") == t.model]


@scenario(bound=10)
def reasoning_end(t):
    """Looping reasoning on llama.cpp is ended early, and the model
    answers in the same response."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(t.requests()[0]["body"].get("reasoning_control") is True,
            "reasoning_control sent")
    cs = controls(t)
    t.check(len(cs) == 1 and cs[0] == {"id": f"chatcmpl-{t.model}-0",
                                       "action": "reasoning_end", "model": t.model},
            f"control {cs}")
    t.check(not r.all("nudge") and len(t.requests()) == 1, "no cancel, no retry")
    t.check(any("reasoning_end" in n.get("text", "") for n in r.all("note")), "noted")
    t.check(r.text() == "The answer is 42.", f"text {r.text()!r}")


@scenario(conf={"_extra": "[model {model}]\nreasoning_budget = 100\n"}, bound=10)
def reasoning_budget(t):
    """Reasoning past [model] reasoning_budget is ended likewise."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    cs = controls(t)
    t.check(len(cs) == 1, f"control {cs}")
    asst = r.all("assistant")[0]
    think = "".join(b.get("text", "") for b in asst["blocks"] if b["t"] == "think")
    t.check(400 <= len(think) < 2000, f"reasoning cut near its budget: {len(think)}")
    t.check(any("budget" in n.get("text", "") for n in r.all("note")), "noted")
    t.check(r.text() == "The answer is 42.", f"text {r.text()!r}")


@scenario(bound=10)
def reasoning_404(t):
    """Where the server has no control endpoint, looping reasoning is
    cut off as before."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(len(controls(t)) == 1, "tried once")
    t.check([n["kind"] for n in r.all("nudge")] == ["loop"], f"nudges {r.all('nudge')}")
    t.check(len(t.requests()) == 2, "then a new attempt")
    t.check(r.text() == "Second attempt.", f"text {r.text()!r}")


@scenario(bound=8)
def stop_nudge(t):
    """A final response that announces work is nudged to go on, three
    times at most, and then the turn ends as the agent chose."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check([n["kind"] for n in r.all("nudge")] == ["stop"] * 3,
            f"nudges {[n['kind'] for n in r.all('nudge')]}")
    t.check(len(t.requests()) == 4, f"requests {len(t.requests())}")
    t.check(r.last("turn.end").get("why") == "done", "ends done")
    t.check(any("announcing" in n.get("text", "") for n in r.all("note")), "noted")


@scenario(bound=8)
def stop_progress(t):
    """The count starts over after a tool succeeds, and a closing offer
    ("Let me know...") is no announcement."""
    t.put("hello.txt", "hello\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ns = r.all("nudge")
    t.check([(n["kind"], n["n"]) for n in ns] == [("stop", 1), ("stop", 1)],
            f"nudges {[(n['kind'], n['n']) for n in ns]}")
    t.check(len(t.requests()) == 4 and r.last("turn.end").get("why") == "done",
            "done after four")


# Subagents and roles

def first_steps(reqs):
    """Each conversation's first request, from a model's request log."""
    return [q for q in reqs if fake_provider.step(q["body"]) == 0]


@scenario(conf={"_path": "router1-task/"}, bound=20)
def task_parallel(t):
    """Two task calls in one response run at once, on a router that loads
    one model at a time: the parent's model rests while they run. Their
    reports end the calls in order, and the sessions list shows them
    under their parent."""
    t.put("a.txt", "1\n2\n3\n")
    t.put("b.txt", "1\n2\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    t.check(len(kids) == 2, f"two subagents: {kids}")
    calls = [c["id"] for c in r.all("assistant", own)[0]["blocks"] if c["t"] == "call"]
    t.check(sorted(k["call"] for k in kids) == sorted(calls), f"their calls {kids}")
    t.check(all(k["parent"] == own and k["depth"] == 1 and k["role"] == "general" and
                k["model"] == t.model2 for k in kids), f"records {kids}")
    ends = r.all("tool.end", own)
    t.check([e["status"] for e in ends] == ["ok", "ok"], f"statuses {ends}")
    t.check([e["call"] for e in ends] == calls, "in call order")
    t.check([e["text"] for e in ends] == ["Report: a.txt has 3 lines.",
                                          "Report: b.txt has 2 lines."], f"reports {ends}")
    bysid = {k["sid"]: k["call"] for k in kids}
    t.check(all(bysid.get(e.get("child")) == e["call"] for e in ends), "each names its child")
    t.check(r.last("turn.end", own).get("why") == "done", "the parent ends done")
    t.check("Both reported: Report: a.txt has 3 lines. / Report: b.txt has 2 lines."
            == r.last("assistant", own)["blocks"][0]["text"], "the parent read both")

    firsts = first_steps(t.requests(t.model2))
    t.check(len(firsts) == 2, f"two conversations {len(firsts)}, retries "
            f"{[(e.get('sid'), e.get('msg')) for e in r.all('retry')]}")
    t.check(firsts[1]["t"] < firsts[0]["end"], "the subagents ran side by side")
    parent = t.requests()
    t.check(all(q["t"] >= parent[0]["end"] - 0.05 for q in t.requests(t.model2)) and
            parent[1]["t"] >= max(q["end"] for q in t.requests(t.model2)) - 0.05,
            "the parent's model rested meanwhile")
    with fake_provider.STATE.lock:
        peak = fake_provider.STATE.peak.get("router1-task")
    t.check(peak == 1, f"one model at a time: {peak}")

    # The list: each subagent under its parent
    h = t.headless()
    try:
        h.call("list")
        listing = h.wait(lambda e: e.get("ev") == "sessions")["list"]
    finally:
        h.close()
    order = [(e["sid"], e.get("parent"), e.get("depth")) for e in listing]
    kidsids = sorted(k["sid"] for k in kids)
    t.check(order == [(own, None, None)] + [(k, own, 1) for k in kidsids],
            f"the list {order}")


@scenario(bound=20)
def task_depth(t):
    """A subagent may start one of its own, but that one gets no task
    tool, and a call to it anyway is refused."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    t.check(sorted(k["depth"] for k in kids) == [1, 2], f"depths {kids}")
    one = [k for k in kids if k["depth"] == 1][0]
    two = [k for k in kids if k["depth"] == 2][0]
    t.check(one["parent"] == own and two["parent"] == one["sid"], "parents")
    t.check("tools" not in one and "task" not in two.get("tools", ["task"]) and
            "read" in two["tools"], f"tools {one} {two}")
    deep = [q["body"] for q in t.requests(t.model2)]
    t.check(any("task" in fake_provider.tool_names(q) for q in deep) and
            any("task" not in fake_provider.tool_names(q) for q in deep),
            "offered at depth 1, not at depth 2")
    refused = r.all("tool.end", two["sid"])
    t.check(len(refused) == 1 and refused[0]["status"] == "error" and
            "not available in this session" in refused[0]["text"], f"refused {refused}")
    t.check(r.last("assistant", own)["blocks"][0]["text"] ==
            "Top heard: Level 1 heard: Level 2 done; its task was refused.",
            f"reports {r.last('assistant', own)}")


@scenario(conf={"_tools": {"task_budget": 4}}, bound=20)
def task_budget(t):
    """At its budget a subagent is told to wrap up; one that does reports
    as usual, and one that goes on is stopped at half as many again,
    reporting what it said last."""
    t.put("hello.txt", "".join(f"line {i}\n" for i in range(10)))
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    t.check(len(kids) == 2, f"two subagents {kids}")
    reqs = t.requests(t.model2)
    obey = [q for q in reqs if "obedient" in fake_provider.task_prompt(q["body"])]
    defy = [q for q in reqs if "stubborn" in fake_provider.task_prompt(q["body"])]
    t.check(len(obey) == 4 and len(defy) == 6, f"requests {len(obey)} {len(defy)}")
    wrap = [m for m in obey[3]["body"]["messages"] if "Wrap up now" in str(m.get("content"))]
    t.check(len(wrap) == 1, "the fourth request carries the notice")
    for k in kids:
        notes = [n for n in r.all("notice", k["sid"]) if n.get("kind") == "budget"]
        t.check(len(notes) == 1, f"one wrap-up notice: {notes}")
    ends = r.all("tool.end", own)
    t.check([e["status"] for e in ends] == ["ok", "error"], f"statuses {ends}")
    t.check(ends[0]["text"] == "Obedient final report.", f"obeyed {ends[0]}")
    t.check("budget of 6 model requests" in ends[1]["text"] and
            ends[1]["text"].endswith("Working on step 5."), f"stopped {ends[1]['text']!r}")
    stub = [k["sid"] for k in kids if k["call"] == ends[1]["call"]][0]
    t.check(r.last("turn.end", stub).get("why") == "budget", "its turn ended at the budget")
    t.check(r.last("turn.end", own).get("why") == "done", "the parent goes on")


@scenario(bound=20)
def task_cancel(t):
    """Cancelling the parent cancels its subagent, killing its command,
    and the parent's call ends cancelled."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        kid = h.wait(lambda e: e.get("ev") == "session" and e.get("parent") == sid)
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == kid["sid"])
        deadline = time.monotonic() + 10
        while not t.pids("tree.pids") and time.monotonic() < deadline:
            time.sleep(0.1)
        pids = t.pids("tree.pids")
        t.check(pids, "the subagent's command started")
        live = h.wait(lambda e: e.get("ev") == "live" and e.get("sid") == sid and
                      e.get("state") == "task")
        t.check(live.get("tasks") == 1, f"one at work: {live}")
        t0 = time.monotonic()
        h.call("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "cancel", f"the parent's turn {end}")
        t.check(time.monotonic() - t0 < 5, "at once")
        kend = h.all("turn.end", kid["sid"])
        t.check(kend and kend[-1]["why"] == "cancel", f"the subagent's turn {kend}")
        te = h.all("tool.end", sid)
        t.check(len(te) == 1 and te[0]["status"] == "cancelled" and
                "cancelled" in te[0]["text"], f"the call {te}")
        t.check(t.gone(pids), f"the command is gone: {pids}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


ROLES = """[role reader]
description = Reads files
prompt = You only read files. ROLE-MARK
tools = read glob
[role fast]
model = {model2}
"""


@scenario(conf={"_extra": ROLES}, bound=20)
def role_tools(t):
    """A role's prompt is in the system prompt and only its tools are
    offered, others refused; a task names a role, a built-in or the
    configuration's, with its model."""
    r = t.run(extra=["-r", "reader"])
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, _ = r.sessions()
    rec = r.all("session", own)[0]
    t.check(rec["role"] == "reader" and rec["tools"] == ["read", "glob"] and
            rec["prompt"] == "You only read files. ROLE-MARK", f"record {rec}")
    first = [q["body"] for q in t.requests() if "# Your role: reader" in
             fake_provider.system_of(q["body"])][0]
    t.check(fake_provider.tool_names(first) == ["read", "glob"],
            f"offered {fake_provider.tool_names(first)}")
    t.check("ROLE-MARK" in fake_provider.system_of(first), "the role's prompt")
    te = r.all("tool.end", own)
    t.check(te and te[0]["status"] == "error" and "not available" in te[0]["text"],
            f"write refused {te}")
    t.check(t.file("x.txt") is None, "nothing written")

    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    roles = sorted(k["role"] for k in kids)
    t.check(roles == ["explore", "fast"], f"roles {roles}")
    ex = [k for k in kids if k["role"] == "explore"][0]
    fast = [k for k in kids if k["role"] == "fast"][0]
    t.check(ex["tools"] == ["read", "grep", "glob"] and ex["model"] == t.model, f"explore {ex}")
    t.check(fast["model"] == t.model2 and "tools" not in fast, f"fast {fast}")
    exreq = [q["body"] for q in t.requests() if "# Your role: explore" in
             fake_provider.system_of(q["body"])]
    t.check(exreq and fake_provider.tool_names(exreq[0]) == ["read", "grep", "glob"],
            "explore's tools")
    t.check(t.file("shell.txt") is None, "explore ran no command")
    ends = r.all("tool.end", own)
    t.check([e["status"] for e in ends] == ["ok", "ok", "error"], f"statuses {ends}")
    t.check(ends[0]["text"].startswith("Explore report: The tool \"shell\" is not "
                                       "available"), f"explore {ends[0]['text']!r}")
    t.check(ends[1]["text"] == "Fast report.", f"fast {ends[1]}")
    t.check("No role is named \"nope\"" in ends[2]["text"] and "child" not in ends[2],
            f"nope {ends[2]}")


PARK = {"HARNESS_TEST_MINUTE_MS": "20"}  # parked after 140 ms idle


@scenario(bound=20)
def park_revive(t):
    """An idle session is parked, keeping its lock, and the next command
    revives it from its log as it was."""
    h = t.headless(env=PARK)
    other = None
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="first question")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)

        def entry():
            h.call("list")  # its reply follows the list
            return [e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid][0]

        deadline = time.monotonic() + 5
        while not entry().get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        en = entry()
        t.check(en.get("parked") == 1 and en["state"] == "idle" and en["locked"] == 0,
                f"parked: {en}")

        # Its lock is still held: another process gets it read-only
        other = t.headless()
        r = other.call("open", sid=sid, attended=1)
        t.check(r.get("ok") == 1 and r.get("readonly") == 1, f"read-only elsewhere: {r}")
        other.close()
        other = None

        h.send("prompt", sid=sid, text="second question")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
                     e.get("turn") == 2)
        t.check(end["why"] == "done", f"the second turn {end}")
        msgs = t.requests()[1]["body"]["messages"]
        said = [(m["role"], m.get("content")) for m in msgs if m["role"] != "system"]
        t.check(said == [("user", "first question"), ("assistant", "First answer."),
                         ("user", "second question")], f"the history {said}")
        seqs = [e["seq"] for e in h.events if e.get("sid") == sid and "seq" in e]
        t.check(seqs == list(range(1, len(seqs) + 1)), f"seq in order: {seqs}")
        t.check(len(h.all("title", sid)) == 1, "titled once")

        # Parked again, then opened: replayed from where asked
        deadline = time.monotonic() + 5
        while not entry().get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        n = len(h.events)
        r = h.call("open", sid=sid, attended=1, **{"from": seqs[-1] - 2})
        t.check(r.get("ok") == 1 and r.get("readonly") == 0, f"open {r}")
        again = [e for e in h.events[n:] if e.get("sid") == sid and "seq" in e]
        t.check([e["seq"] for e in again] == seqs[-2:], f"replayed {again}")
    finally:
        if other:
            other.close()
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_ask(t):
    """A subagent's ask reaches the frontend that attends its parent,
    which answers it as any session's."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = h.wait(lambda e: e.get("ev") == "ask")
        kid = [e for e in h.all("session") if e["sid"] == ask["sid"]]
        t.check(kid and kid[0].get("parent") == sid, f"the subagent asks: {ask}")
        t.check(ask["deadline"] - ask["ts"] >= 29000, f"with a deadline: {ask}")
        r = h.call("answer", sid=ask["sid"], req=ask["req"], decision="allow")
        t.check(r.get("ok") == 1, f"answered {r}")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the parent ends {end}")
        ans = h.all("answer", ask["sid"])
        t.check(ans and ans[0]["by"] == "user" and ans[0]["decision"] == "allow", f"{ans}")
        t.check(outside_file(t) == "from the child\n", "written outside")
        te = h.all("tool.end", sid)
        t.check(te and te[0]["text"] == "The child wrote it.", f"report {te}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_close(t):
    """A frontend that closes the parent leaves its subagent unattended:
    the ask waiting is denied, and the subagent goes on."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = h.wait(lambda e: e.get("ev") == "ask")
        t.check(ask["sid"] != sid, f"the subagent asks: {ask}")
        h.call("close", sid=sid)
        ans = h.wait(lambda e: e.get("ev") == "answer" and e.get("sid") == ask["sid"])
        t.check(ans["by"] == "policy" and ans["decision"] == "deny" and
                "no longer attended" in ans["note"], f"denied: {ans}")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the parent ends {end}")
        te = h.all("tool.end", sid)
        t.check(te and te[0]["text"] == "The child was refused.", f"report {te}")
        t.check(outside_file(t) is None, "nothing written outside")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_long(t):
    """A report over 50 KiB is cut in the result, which says where all of
    it is: a file in the parent's session the agent may read."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    full = "".join(f"Finding {i:04d}: {'x' * 20}\n" for i in range(2000)).strip()
    te = r.all("tool.end", own)[0]
    t.check(te["status"] == "ok" and te.get("child") == kids[0]["sid"], f"the call {te['status']}")
    t.check(te["text"].startswith(full[:51200]) and len(te["text"]) < 52 * 1024,
            f"cut: {len(te['text'])} bytes")
    path = t.host.join(t.state, "sessions", own, *te["spill"].split("/"))
    said = te["text"].replace("\\", "/")
    t.check("All of it is in " in said and f"{own}/{te['spill']}" in said,
            f"says where: {te['text'][-300:]!r}")
    t.check(te["bytes"] == len(full), f"bytes {te['bytes']}")
    t.check(t.host.read(path) == full, "the whole report kept")
    line = full[:51200].count("\n") + 1
    t.check(te["text"].endswith(f"where this part ends at line {line}.]"),
            f"its line: {te['text'][-80:]!r}")
    msgs = t.requests()[1]["body"]["messages"]
    t.check(msgs[-1]["role"] == "tool" and msgs[-1]["content"] == te["text"],
            "the agent read the cut report")


@scenario(bound=15)
def task_unattended(t):
    """Unattended, a subagent's ask is denied by policy, and it goes on."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    ans = r.all("answer", kids[0]["sid"])
    t.check(ans and ans[0]["by"] == "policy" and ans[0]["decision"] == "deny", f"{ans}")
    t.check(outside_file(t) is None, "nothing written outside")
    t.check(r.last("tool.end", own).get("text") == "The child was refused.",
            f"report {r.last('tool.end', own)}")


@scenario(conf={"_extra": "[model {model}]\nedit_format = patch\n"}, bound=15)
def edit_patch(t):
    """edit_format patch: an apply_patch envelope changes, adds and
    deletes files at once, each snapshotted, so that rewind puts all of
    them back; a hunk that does not match changes nothing, and a file
    outside the working directory is asked about."""
    t.put("hello.txt", "greet\nhello\nworld\n")
    t.put("old.txt", "bye\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.text().endswith("Patched."), f"text {r.text()!r}")
    t.check(r.events[0].get("edit_format") == "patch", f"session {r.events[0]}")
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok", "error", "denied"],
            f"statuses {[(e['status'], e['text'][:80]) for e in ends]}")
    t.check(ends[0]["text"] == "Applied the patch: updated hello.txt, added sub/new.txt, "
            "deleted old.txt.", f"result {ends[0]['text']!r}")
    t.check(ends[1]["text"].startswith("The patch was not applied, and no file was changed: "
                                       "hunk 1 of hello.txt"), f"error {ends[1]['text']!r}")
    t.check(t.file("hello.txt") == "greet\nhello\nthere\n" and
            t.file("sub/new.txt") == "fresh\n" and t.file("old.txt") is None,
            f"files {t.file('hello.txt')!r} {t.file('sub/new.txt')!r} {t.file('old.txt')!r}")
    t.check(outside_file(t) is None, "nothing written outside")
    ans = r.all("answer")
    t.check(ans and ans[0]["by"] == "policy", f"asked and denied: {ans}")
    snaps = r.all("snap")
    t.check([(os.path.basename(s["path"]), s["existed"], s["hash_after"] == "") for s in snaps] ==
            [("hello.txt", 1, False), ("new.txt", 0, False), ("old.txt", 1, True)],
            f"snaps {snaps}")

    sid = r.events[0]["sid"]
    h = t.headless()
    try:
        o = h.call("open", sid=sid, **{"from": 0})
        t.check(o.get("ok") == 1, f"opened {o}")
        rr = h.call("rewind", sid=sid, to=1)
        got = [(os.path.basename(f["path"]), f["result"]) for f in rr.get("files", [])]
        t.check(got == [("old.txt", "restored"), ("new.txt", "removed"),
                        ("hello.txt", "restored")], f"rewound {rr}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    t.check(t.file("hello.txt") == "greet\nhello\nworld\n" and t.file("old.txt") == "bye\n" and
            t.file("sub/new.txt") is None, "all put back")


@scenario(conf={"_extra": "[model {model}]\nedit_format = hashline\n"}, bound=15)
def edit_hashline(t):
    """edit_format hashline: read shows a tag, edits by line number check
    it, and a session opened again keeps its format whatever the
    configuration says by then."""
    t.put("list.txt", "one\ntwo\nthree\nfour\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.text().endswith("Edited by line."), f"text {r.text()!r}")
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok", "ok", "error", "ok", "ok", "ok"],
            f"statuses {[(e['status'], e['text'][:80]) for e in ends]}")
    t.check(ends[0]["text"].startswith("[list.txt#") and "     1\tone\n" in ends[0]["text"],
            f"read {ends[0]['text']!r}")
    t.check("does not match list.txt" in ends[2]["text"], f"stale {ends[2]['text']!r}")
    t.check("Tags for edit: [list.txt#" in ends[4]["text"], f"grep {ends[4]['text']!r}")
    t.check(t.file("list.txt") == "zero\none\ntwo\nthree\nend\n", f"file {t.file('list.txt')!r}")
    t.check(r.events[0].get("edit_format") == "hashline", f"session {r.events[0]}")

    # The format stays with the session
    sid = r.events[0]["sid"]
    t.host.write(t.config, t.host.read(t.config).replace("edit_format = hashline", ""))
    r2 = t.run("again", resume=sid)
    t.check(r2.rc == 0, f"resumed: {r2.rc} {r2.stderr}")
    tools = t.requests()[-1]["body"]["tools"]
    edit = [x for x in tools if x["function"]["name"] == "edit"][0]
    t.check(sorted(edit["function"]["parameters"]["properties"]) == ["edits", "path", "tag"],
            f"kept {edit['function']['parameters']['properties'].keys()}")


# M4 review: the engine

@scenario(bound=20)
def task_closewait(t):
    """A subagent waits for its background job, and a frontend closes the
    subagent's session: it reports all the same, and the parent goes on."""
    h = t.headless()
    try:
        p = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=p, text="go")
        kid = h.wait(lambda e: e.get("ev") == "session" and e.get("parent") == p)["sid"]
        h.wait(lambda e: e.get("sid") == kid and e.get("ev") == "turn.end")
        time.sleep(0.3)
        h.call("close", sid=kid)
        end = h.wait(lambda e: e.get("sid") == p and e.get("ev") == "turn.end",
                     timeout=10)
        t.check(end["why"] == "done", f"parent: {end}")
        res = [e for e in h.all("tool.end", p)]
        t.check(res and res[0]["status"] == "ok" and
                "Waiting for my job." in res[0]["text"], f"the report: {res}")
    finally:
        h.call("quit")
        h.close()


@scenario(conf={"_tools": {"stop_grace": 1}}, bound=15, posix=True)
def task_killwait(t):
    """A subagent stops its job, which ignores TERM, and ends its turn
    while the job is still stopping: once the job is gone, it reports."""
    r = t.run(timeout=14)
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')}")
    t.check("Stopped my job; done." in r.text(), f"the parent heard: {r.text()!r}")


@scenario(conf={"_extra": "[model {model}]\nfallback = {model2}\n"}, bound=15)
def fallback_limit(t):
    """The primary is down a while, and its fallback is out of credit: the
    slow retries go back to the primary rather than end the turn."""
    r = t.run(env=MINUTE)
    end = r.last("turn.end")
    t.check(r.rc == 0 and end.get("why") == "done",
            f"exit {r.rc}, turn end {end}, retries "
            f"{[(e['class'], e.get('fallback'), e.get('slow')) for e in r.all('retry')][-4:]}")
    rt = r.all("retry")
    t.check(any(e.get("slow") and e.get("model") == t.model for e in rt),
            f"a slow retry on the primary: {rt[-2:]}")
    t.check(r.text() == "Back on the primary.", f"text {r.text()!r}")


@scenario(bound=30)
def kv_full(t):
    """Two sessions on one model whose KV pool holds one context: the
    first error from the full pool halves the model's concurrency, so
    both turns end well, rather than fail together again and again."""
    h = t.headless(env=MINUTE)
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.send("prompt", sid=b, text="go")
        ends = [h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end",
                       timeout=25) for s in (a, b)]
    finally:
        h.close()
    n = len(t.requests())
    t.check(all(e["why"] == "done" for e in ends) and n <= 4,
            f"{n} requests; ends {[(e['why'], e.get('err', '')[:60]) for e in ends]}")
    notes = [x["text"] for x in h.all("note") if "memory for contexts" in x.get("text", "")]
    t.check(len(notes) == 1 and "1 request at once" in notes[0], f"noted once: {notes}")


@scenario(conf=SMALL, bound=30, alone=True)
def compact_notice(t):
    """A job's notice before a compaction's cut: the session opened again
    rebuilds the history the live one had, without the notice."""
    bigfiles(t, 8, 160)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    cps = [c for c in r.all("compact") if c["kind"] == "summary"]
    t.check(len(cps) >= 1, f"a summary: {r.all('compact')}")
    notices = r.all("notice")
    t.check(notices and notices[0]["seq"] < cps[0]["cutseq"],
            f"the notice before the cut: {[x['seq'] for x in notices]}, "
            f"cut at {cps[0]['cutseq']}")
    reqs = t.requests()
    last = chat(reqs[-1])
    sid = r.events[0].get("sid")
    r2 = t.run("again", resume=sid)
    t.check(r2.rc == 0, f"resume exit {r2.rc}: {r2.stderr}")
    again = chat(t.requests()[len(reqs)])
    roles = lambda ms: [(m["role"], str(m.get("content"))[:40]) for m in ms]
    t.check(again[:len(last)] == last,
            f"the replay differs:\n live {roles(last)[:4]}\n replay {roles(again)[:4]}")


@scenario(conf=SMALL, bound=30, alone=True)
def rate_reopen(t):
    """The server counts 8 bytes a token, so the session is far below its
    threshold: opened again, it keeps that calibration, and does not
    compact as if a token were 4 bytes."""
    bigfiles(t, 7, 160)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(not r.all("compact"), f"no compaction live: {r.all('compact')}")
    t.check(all(e["usage"].get("bytes", 0) > 0 for e in r.all("assistant")),
            "usage records the prompt's bytes")
    sid = r.events[0].get("sid")
    r2 = t.run("again", resume=sid)
    t.check(r2.rc == 0, f"resume exit {r2.rc}: {r2.stderr}")
    t.check(not r2.all("compact"), f"compacted on reopening: "
            f"{[(c['kind'], c['before'], c['after']) for c in r2.all('compact')]}")


@scenario(bound=20)
def revive_prefix(t):
    """A session revived from parking, or opened again, after the
    configuration changed sends the same prefix as before: its log holds
    it, and only a compaction may break the prompt cache."""
    h = t.headless(env=PARK)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="first question")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        r = h.call("config.set", kind="guard", name="", key="roots", value=t.base)
        t.check(r.get("ok") == 1, f"config.set: {r}")
        r = h.call("config.set", kind="tools", name="", key="job_timeout", value="7")
        t.check(r.get("ok") == 1, f"config.set: {r}")

        def entry():
            h.call("list")
            return [e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid][0]
        deadline = time.monotonic() + 5
        while not entry().get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        t.check(entry().get("parked") == 1, "parked")
        h.send("prompt", sid=sid, text="second question")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
               e.get("turn") == 2)
    finally:
        h.close()
    t.check(len(h.all("prefix", sid)) == 1, f"one prefix record: {len(h.all('prefix', sid))}")
    r = t.run("third question", resume=sid)
    t.check(r.rc == 0, f"resume exit {r.rc}: {r.stderr}")
    t.check(not r.all("prefix"), "no second prefix record")
    reqs = t.requests()
    t.check(len(reqs) == 3, f"three requests: {len(reqs)}")
    for k in (1, 2):
        for part in ("messages", "tools"):
            a, b = reqs[0]["body"][part], reqs[k]["body"][part]
            if part == "messages":
                a, b = a[0], b[0]
            t.check(a == b, f"request {k}'s {part} changed: {str(b)[-120:]!r}")


@scenario(conf={"_path": "router1-linger/"}, bound=20)
def queue_linger(t):
    """Two sessions on model a, one on b, at a router of one model: the end
    of A's turn leaves C's model lingering while C runs a tool, so that b
    does not evict it mid-turn."""
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        c = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        h.send("prompt", sid=c, text="slowtool")
        h.wait(lambda e: e.get("sid") == c and e.get("ev") == "live" and
               e.get("state") == "waiting")
        h.send("prompt", sid=a, text="quick")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "waiting")
        h.send("prompt", sid=b, text="other")
        h.wait(lambda e: e.get("sid") == b and e.get("ev") == "live" and
               e.get("state") == "queued")
        for s in (a, b, c):
            end = h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
            t.check(end["why"] == "done", f"{s}: {end}")
    finally:
        h.close()
    one = t.requests()
    two = t.requests(t.model2)
    csecond = [x for x in one if any(m.get("role") == "tool"
                                     for m in x["body"]["messages"])]
    t.check(len(csecond) == 1 and len(two) == 1, f"requests {len(one)} {len(two)}")
    t.check(two[0]["t"] >= csecond[0]["t"],
            f"b went at {two[0]['t'] - one[0]['t']:.2f} s, before C's second step "
            f"at {csecond[0]['t'] - one[0]['t']:.2f} s, while C's model lingered")


@scenario(conf={"_path": "router1-remote/"}, bound=20)
def queue_remote(t):
    """A router of one model: a remote model that it only relays takes no
    model place, so it does not wait behind a local model's swap."""
    remote = f"fake:queue_remote_c:{t.nonce}"
    fake_provider.STATE.register(remote, owned_by="remote", status={"value": "loaded"})
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        c = h.call("new", cwd=t.work, model=remote, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "streaming")
        h.send("prompt", sid=b, text="go")
        h.wait(lambda e: e.get("sid") == b and e.get("ev") == "live" and
               e.get("state") == "queued")
        h.send("prompt", sid=c, text="go")
        for s in (a, b, c):
            h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
    finally:
        h.close()
    one = t.requests()
    rem = t.requests(remote)
    t.check(rem and rem[0]["t"] < one[0]["end"],
            f"the remote model waited {rem[0]['t'] - one[0]['t']:.2f} s, until the "
            f"local model's turn ended at {one[0]['end'] - one[0]['t']:.2f} s")


@scenario(conf={"_path": "router1-probe-flaky/"}, bound=20)
def queue_probe(t):
    """A router of one model whose first /props fails, as one starting
    up: still one model at a time, and the next request asks again."""
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "streaming")
        time.sleep(0.3)
        h.send("prompt", sid=b, text="go")
        for s in (a, b):
            end = h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
            t.check(end["why"] == "done", f"{s}: {end}")
    finally:
        h.close()
    with fake_provider.STATE.lock:
        peak = fake_provider.STATE.peak.get("router1-probe-flaky")
        asked = fake_provider.STATE.counts.get("props router1-probe-flaky", 0)
    t.check(peak == 1, f"models at once: {peak}")
    t.check(asked >= 2, f"asked again: {asked}")


@scenario(conf={"_path": "router1-leave/"}, bound=20)
def queue_leave(t):
    """Sessions waiting in line hear of every change in their places: one
    joining behind them, and one ahead of them leaving the line."""
    third = f"fake:queue_leave_c:{t.nonce}"
    fake_provider.STATE.register(third)
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        c = h.call("new", cwd=t.work, model=third, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "streaming")
        h.send("prompt", sid=b, text="go")
        h.wait(lambda e: e.get("sid") == b and e.get("ev") == "live" and
               e.get("state") == "queued")
        h.send("prompt", sid=c, text="go")
        h.wait(lambda e: e.get("sid") == c and e.get("ev") == "live" and
               e.get("state") == "queued")
        h.wait(lambda e: e.get("sid") == b and e.get("ev") == "live" and
               e.get("queue_len") == 2, timeout=5)
        h.call("cancel", sid=b)
        h.wait(lambda e: e.get("sid") == c and e.get("ev") == "live" and
               e.get("state") == "queued" and e.get("queue_pos") == 1, timeout=5)
        for s in (a, c):
            end = h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
            t.check(end["why"] == "done", f"{s}: {end}")
    finally:
        h.close()
    places = [(e.get("queue_pos"), e.get("queue_len")) for e in h.all("live", c)
              if e.get("state") == "queued"]
    t.check(places == [(2, 2), (1, 1)], f"c was told {places}")


@scenario(conf={"_extra": "[model {model}]\nfallback = {model2}\n"}, bound=15)
def slow_target(t):
    """A slow retry names the next attempt's target: the primary, since
    the chain starts over from it after a slow wait."""
    r = t.run(env=MINUTE)
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')}")
    slow = [e for e in r.all("retry") if e.get("slow")]
    t.check(slow, "a slow retry")
    i = next(k for k, e in enumerate(r.events) if e is slow[0])
    nxt = next(e for e in r.events[i:] if e.get("ev") == "attempt")
    t.check(slow[0]["model"] == nxt["model"] == t.model,
            f"the slow retry names {slow[0]['model'][-20:]}, "
            f"the next attempt is on {nxt['model'][-20:]}")


# M4 review: the tools

@scenario(bound=15)
def rewind_dupid(t):
    """A model that gives every call the same id: each call still gets an
    id of its own in the session, so that rewinding to the later one, as
    the GUI's Undo on its row does, leaves the earlier one's file."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        starts = [e["call"] for e in h.all("tool.start", sid)]
        t.check(len(starts) == 2 and starts[0] == "dup" and starts[1] != "dup",
                f"call ids {starts}")
        r = h.call("rewind", sid=sid, to=starts[1])
        got = [(os.path.basename(f["path"]), f["result"]) for f in r.get("files", [])]
        t.check(got == [("b.txt", "removed")], f"rewound {got}")
        t.check(t.file("a.txt") == "A\n" and t.file("b.txt") is None,
                f"a.txt {t.file('a.txt')!r}, b.txt {t.file('b.txt')!r}")
    finally:
        h.close()


@scenario(bound=15)
def rewind_partial(t):
    """A file the harness wrote, then someone else changed, then the
    harness edited: rewinding past both puts back the later change only,
    and says so, rather than call it a conflict left as it was."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]

        def turn(text, n):
            h.send("prompt", sid=sid, text=text)
            h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid
                   and e.get("turn") == n)
        turn("first", 1)
        t.put("f.txt", "ext\n")
        turn("second", 2)
        t.check(t.file("f.txt") == "ext2\n", f"edited: {t.file('f.txt')!r}")
        r = h.call("rewind", sid=sid, to=1)
        got = [(os.path.basename(f["path"]), f["result"]) for f in r.get("files", [])]
        t.check(got == [("f.txt", "partial")], f"rewound {got}")
        t.check(t.file("f.txt") == "ext\n", f"put back in part: {t.file('f.txt')!r}")
        turn("third", 3)
        note = [x for x in h.all("notice", sid) if x.get("kind") == "rewind"]
        t.check(note and "only to before the later changes" in note[-1]["text"],
                f"the agent is told: {note[-1]['text'] if note else None!r}")
    finally:
        h.close()


@scenario(conf={"_extra": "[model {model}]\nedit_format = patch\n"}, bound=10)
def patch_twice(t):
    """A patch that names one file twice, spelled two ways, is refused
    rather than lose the first section's changes."""
    t.put("f.txt", "one\ntwo\nthree\n")
    r = t.run()
    end = r.all("tool.end")[0]
    t.check(end["status"] == "error" and "a file appears twice" in end["text"],
            f"tool.end {end['status']}: {end['text'][:160]!r}")
    t.check(t.file("f.txt") == "one\ntwo\nthree\n", f"unchanged: {t.file('f.txt')!r}")


@scenario(bound=15)
def job_eof(t):
    """A command that closed its output and is still running at its
    timeout: its job ends when it exits, without the wait for output
    that only leftovers holding the pipe would explain."""
    r = t.run()
    je = r.last("job.end")
    note = r.last("notice").get("text", "")
    t.check(je.get("status") == "exited" and je.get("ms", 0) < 4500,
            f"job.end {je}")
    t.check("kept its output open" not in note, f"the notice: {note[-160:]!r}")


@scenario(bound=10, posix=True)
def instructions_link(t):
    """An instruction file that imports, through a symlink, a file from
    outside the project: refused, as any import from outside is."""
    secret = t.host.join(t.base, "secret.txt")
    t.host.write(secret, "SECRET-TOKEN-1234\n")
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("AGENTS.md", "Follow @docs/style.md please.\n")
    t.host.mkdir(t.host.join(t.work, "docs"))
    os.symlink("../../secret.txt", t.host.join(t.work, "docs", "style.md"))
    os.symlink("../secret.txt", t.host.join(t.work, "CLAUDE.md"))
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    sysmsg = t.requests()[0]["body"]["messages"][0]["content"]
    ins = r.last("instructions")
    t.check("SECRET-TOKEN-1234" not in sysmsg,
            "a file outside the project reached the system prompt through a symlink")
    t.check([os.path.basename(f["path"]) for f in ins.get("files", [])] == ["AGENTS.md"] and
            any(s.endswith("docs/style.md") for s in ins.get("skipped", [])),
            f"files {ins.get('files')}, skipped {ins.get('skipped')}")


@scenario(bound=60, posix=True, alone=True)
def glob_many(t):
    """A tree whose list of files passes 8 MiB: glob still finds an old
    file, since only the files that match are listed."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    name = "x" * 180
    for d in range(50):
        dd = t.host.join(t.work, f"dir{d:03d}")
        os.makedirs(dd, exist_ok=True)
        for f in range(1000):
            open(os.path.join(dd, f"{name}{f:05d}.txt"), "w").close()
    old = t.host.join(t.work, "zzz")
    os.makedirs(old)
    p = os.path.join(old, "target.special")
    open(p, "w").close()
    os.utime(p, (time.time() - 86400, time.time() - 86400))
    r = t.run(timeout=50)
    end = r.all("tool.end")[0]
    t.check(end["status"] == "ok" and "zzz/target.special" in end["text"],
            f"glob: {end['status']}: {end['text'][:200]!r}")


@scenario(bound=15, posix=True)
def grep_unreadable(t):
    """A directory that cannot be read (ripgrep exits 2 for it, silently)
    makes no failed search of a valid pattern."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("main.c", "int main(void) { return 0; }\n")
    d = t.host.join(t.work, "pgdata")
    t.host.mkdir(d)
    t.host.write(t.host.join(d, "x"), "x\n")
    os.chmod(d, 0)
    try:
        r = t.run()
    finally:
        os.chmod(d, 0o755)
    end = r.all("tool.end")[0]
    t.check(end["status"] == "ok" and end["text"].startswith("No lines match"),
            f"grep: {end['status']}: {end['text']!r}")


@scenario(conf={"_extra": "[model {model}]\nedit_format = hashline\n"}, bound=10)
def edit_hashlong(t):
    """A hashline replace of a line that read cut short is refused, since
    it would lose the rest of the line."""
    long = "var table = [" + ",".join(str(i) for i in range(900)) + "];"
    t.put("data.js", long + "\nconsole.log(table.length);\n")
    r = t.run()
    ends = r.all("tool.end")
    t.check(ends[1]["status"] == "error" and "bytes long" in ends[1]["text"],
            f"edit: {ends[1]['status']}: {ends[1]['text'][:200]!r}")
    t.check(t.file("data.js").split("\n")[0] == long, "the line is whole")


def rawlines(r):
    return [e["raw"] for e in r.events if e.get("ev") == "?"]


@scenario(conf=SMALL, bound=30, alone=True)
def pretty(t):
    """--pretty tells of retries and compactions, and of instruction files
    only when there are some."""
    bigfiles(t, 8, 160)
    r = t.run(extra=("--pretty",))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    out = rawlines(r)
    t.check(any(x.startswith("[retry in ") and "HTTP 503: busy a moment" in x
                for x in out), f"the retry: {out[:6]}")
    cps = [x for x in out if x.startswith("[compacted: ")]
    t.check(len(cps) == 1 and "summarized by " + t.model in cps[0] and " -> " in cps[0],
            f"the compaction: {cps}")
    t.check(not any(x.startswith("[instructions") for x in out),
            f"no empty instructions line: {out[:4]}")


@scenario(conf={"_extra": "[model {model}]\nfallback = {model2}\n"}, bound=15)
def pretty_fallback(t):
    """--pretty names a fallback switched to, and a slow retry's target."""
    r = t.run(extra=("--pretty",), env=MINUTE)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    out = rawlines(r)
    t.check(any(x.startswith(f"[switching to fallback 1, {t.model2}: ") for x in out),
            f"the switch: {[x for x in out if 'fallback' in x][:2]}")
    t.check(any(x.startswith("[slow retry in ") and f" on {t.model}: " in x
                for x in out), f"the slow retry: {[x for x in out if 'slow' in x][:2]}")


@scenario(bound=15)
def final_newline(t):
    """A file written whole ends in a newline though the agent's content
    does not: a new one gains it in the content's line endings, one that
    replaces a file ending in CRLF gains a CRLF, and one that replaces a
    file deliberately without stays without. Rewind puts back the bytes
    exactly, so what each call wrote is what it says it wrote."""
    t.put("plain.txt", "deliberately without a newline")
    t.put("crlf.txt", "one\r\ntwo\r\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.text() == "Wrote them all.", f"text {r.text()!r}")
    ends = [e["text"] for e in r.all("tool.end")]
    t.check(ends == ["Created main.c (29 bytes, 1 lines; added a final newline).",
                     "Replaced plain.txt (23 bytes, 1 lines).",
                     "Replaced crlf.txt (11 bytes, 1 lines; added a final newline).",
                     "Created win.txt (6 bytes, 2 lines; added a final newline).",
                     "Created made.txt (13 bytes, 1 lines; added a final newline)."],
            f"results {ends}")
    want = {"main.c": "int main(void) { return 0; }\n",
            "plain.txt": "replaced, still without",
            "crlf.txt": "only line\r\n",
            "win.txt": "a\r\nb\r\n",
            "made.txt": "made by edit\n"}
    got = {k: t.file(k) for k in want}
    t.check(got == want, f"files {got}")
    write = [x["function"] for x in t.requests()[0]["body"]["tools"]
             if x["function"]["name"] == "write"]
    t.check("without a final newline gets one" in write[0]["description"],
            f"the write tool says so: {write[0]['description']!r}")

    sid = r.events[0]["sid"]
    h = t.headless()
    try:
        o = h.call("open", sid=sid, **{"from": 0})
        t.check(o.get("ok") == 1, f"opened {o}")
        rr = h.call("rewind", sid=sid, to=1)
        got = sorted((os.path.basename(f["path"]), f["result"]) for f in rr.get("files", []))
        t.check(got == [("crlf.txt", "restored"), ("made.txt", "removed"),
                        ("main.c", "removed"), ("plain.txt", "restored"),
                        ("win.txt", "removed")], f"rewound {rr}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    left = {k: t.file(k) for k in want}
    t.check(left == {"main.c": None, "plain.txt": "deliberately without a newline",
                     "crlf.txt": "one\r\ntwo\r\n", "win.txt": None, "made.txt": None},
            f"put back {left}")


SCENARIOS["503x3"] = SCENARIOS.pop("x503x3")


def runone(host, name, verbose):
    """A scenario's bound covers the harness, not the link: the clock
    starts once its directories and configuration are made, and the time
    of its file operations and checks over ssh (linktime) is left out."""
    fn, conf, bound, posix, alone = SCENARIOS[name]
    if posix and host.windows:
        return name, "skip", 0.0, "POSIX only"
    t0 = time.monotonic()
    LINK.secs = 0.0
    try:
        t = T(host, "503x3" if name == "503x3" else name, conf)
        t0 = time.monotonic()
        LINK.secs = 0.0
        fn(t)
        link = LINK.secs
        dt = time.monotonic() - t0 - link
        over = f"(and {link:.1f} s over ssh)" if link >= 0.5 else ""
        if dt > bound:
            return name, "FAIL", dt, f"took {dt:.1f} s, bound {bound} s {over}"
        return name, "ok", dt, over
    except Failure as e:
        return name, "FAIL", time.monotonic() - t0, str(e)
    except Exception:
        return name, "FAIL", time.monotonic() - t0, traceback.format_exc()


# The soak: sessions under 10% random faults for a while

def soak(host, minutes, workers):
    deadline = time.monotonic() + minutes * 60
    stats = {"turns": 0, "done": 0, "error": 0, "retries": 0, "nudges": 0,
             "tools": 0, "max_gap": 0.0}
    lock = threading.Lock()
    problems = []

    def worker(k):
        t = T(host, "soak", {"first_byte_timeout": 3, "idle_timeout": 2,
                             "progress_timeout": 4})
        while time.monotonic() < deadline:
            last = [time.monotonic()]
            gaps = [0.0]

            def watch(ev, p):
                now = time.monotonic()
                gaps[0] = max(gaps[0], now - last[0])
                last[0] = now
                return False
            # A turn may take many steps and retries, but output never
            # stops for longer than a watchdog plus the longest backoff
            r = t.run("soak", timeout=240, watch=watch)
            end = r.last("turn.end")
            with lock:
                stats["turns"] += 1
                stats["retries"] += len(r.all("retry"))
                stats["nudges"] += len(r.all("nudge"))
                stats["tools"] += len(r.all("tool.end"))
                stats["max_gap"] = max(stats["max_gap"], gaps[0])
                why = end.get("why")
                if why == "done":
                    stats["done"] += 1
                elif why == "error":
                    stats["error"] += 1
                if not end:
                    problems.append(f"worker {k}: no turn.end (rc {r.rc}): "
                                    f"{compact(r.names())[-12:]}")
                if gaps[0] > 15:
                    problems.append(f"worker {k}: {gaps[0]:.1f} s without output")
                if r.all("?"):
                    problems.append(f"worker {k}: unparsable output")

    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        list(ex.map(worker, range(workers)))
    print(f"soak: {minutes} min, {workers} sessions: {stats}")
    for p in problems[:20]:
        print("  " + p)
    return not problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True, help="the harness binary")
    ap.add_argument("--host", help="run the harness on this Windows host over ssh")
    ap.add_argument("--xp", help="run the harness on this Windows XP host over "
                    "ssh, reaching the provider at this machine's LAN address")
    ap.add_argument("-j", type=int, default=0, help="scenarios at once")
    ap.add_argument("--soak", type=float, help="soak for this many minutes")
    ap.add_argument("--workers", type=int, default=4, help="soak sessions at once")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()

    settle()
    if args.xp:
        lan = lanaddress(args.xp)
        srv, url = fake_provider.start(lan)
    else:
        srv, url = fake_provider.start()
    port = srv.server_address[1]
    if args.xp:
        host = XpHost(args.xp, args.harness, url)
    elif args.host:
        host = SshHost(args.host, args.harness, port, 20000 + port % 20000)
    else:
        host = LocalHost(args.harness, url)
    try:
        if args.soak:
            ok = soak(host, args.soak, args.workers)
            return 0 if ok else 1
        names = args.names or list(SCENARIOS)
        for n in names:
            if n not in SCENARIOS:
                print(f"unknown scenario {n}; known: {' '.join(SCENARIOS)}")
                return 2
        # No more at once than processors here, which a small machine has
        # fewer of than the eight that suit a big one
        jobs = args.j or (2 if args.host or args.xp else min(8, os.cpu_count() or 8))
        failed = 0

        def report(name, status, dt, why):
            print(f"{status:4} {name:18} {dt:6.1f} s  {why.splitlines()[0] if why else ''}")
            if status == "FAIL" and "\n" in why:  # whole, for a ctest log
                print("     " + why.replace("\n", "\n     "))
            return status == "FAIL"

        together = [n for n in names if not SCENARIOS[n][4]]
        alone = [n for n in names if SCENARIOS[n][4]]
        with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
            futs = [ex.submit(runone, host, n, args.v) for n in together]
            for f in futs:
                failed += report(*f.result())
        for n in alone:
            failed += report(*runone(host, n, args.v))
        print(f"{len(names) - failed}/{len(names)} scenarios passed")
        return 1 if failed else 0
    finally:
        fake_provider.STATE.stop.set()
        host.close()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
