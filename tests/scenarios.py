#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine scenarios: the harness against the fault-injecting fake provider.

Each scenario runs the real binary ("prospero run", or "prospero --headless"
for cancels) in a fresh working and state directory, with a model name
that selects a script in fake_provider.py, then checks the event sequence,
the files left behind, the exit code and a wall-clock bound, which counts
from after the directories are made, without the time spent waiting on
ssh for a remote host's files and processes.

  uv run tests/scenarios.py --prospero build/prospero [NAME ...]
  uv run tests/scenarios.py --prospero PATH --host WINHOST   (Windows)
  uv run tests/scenarios.py --prospero build/prospero --soak 30

With --host, the fake provider runs here and the harness runs on the
Windows host over ssh, reaching the provider through a reverse tunnel;
PATH is the prospero.exe already on that host. busybox.exe is copied next
to it from the host's w64devkit, which is how the harness finds its shell.

  uv run tests/scenarios.py --prospero 'C:/DOCUME~1/u/LOCALS~1/Temp/prospero.exe' \\
      --xp XPHOST [NAME ...]

With --xp, the same for Windows XP running skeeto/sshd
(https://github.com/skeeto/sshd): no tunnel, so the provider listens
on this machine's LAN address instead.
"""

import argparse
import base64
import concurrent.futures
import json
import os
import random
import re
import select
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
import zlib

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

    def __init__(self, prospero, url):
        self.prospero = os.path.abspath(prospero)
        self.url = url
        self.root = tempfile.mkdtemp(prefix="prospero-scen-")
        if self.windows:
            # Paths as the harness reports them, as over ssh (SshHost)
            self.root = self.root.replace("\\", "/")
        self.t0 = time.time()

    def close(self):
        for link in tmplinks(self.root):  # what hosts killed, or clients, left
            try:
                os.unlink(link)
            except FileNotFoundError:
                pass
        root = self.root
        if self.windows:
            # Through a \\?\ path, which reaches past MAX_PATH (deep_unicode)
            root = "\\\\?\\" + os.path.abspath(root)
        shutil.rmtree(root, ignore_errors=True)

    def leftovers(self):
        """What close left: the root, or links in /tmp into it."""
        left = [self.root] if os.path.lexists(self.root) else []
        return left + tmplinks(self.root)

    def join(self, *parts):
        path = os.path.join(*parts)
        return path.replace("\\", "/") if self.windows else path

    def mkdir(self, path):
        os.makedirs(path, exist_ok=True)

    def write(self, path, data):
        # The bytes given, without Windows' text mode, which would make
        # each newline CRLF
        with open(path, "wb") as f:
            f.write(data.encode() if isinstance(data, str) else data)

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
            self.prefix + [self.prospero] + args, cwd=cwd,
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
            P.signal_ours(pid, P.KILL, P.since(self.t0))
        except (OSError, P.Refused) as e:
            print(f"     kill: {e}", flush=True)


class SshHost:
    """A Windows host reached over ssh, with busybox from w64devkit."""
    windows = True

    def __init__(self, host, prospero, port, rport):
        self.host = host
        self.prospero = prospero
        # The master connection's socket, in a directory of the run's own
        self.ctldir = tempfile.mkdtemp(prefix="prospero-ssh-")
        self.ctl = os.path.join(self.ctldir, "ctl")
        self.root = None
        try:
            subprocess.run(["ssh", "-M", "-S", self.ctl, "-fN", "-o",
                            "ExitOnForwardFailure=yes", "-R",
                            f"{rport}:127.0.0.1:{port}", host], check=True)
            self.url = f"http://127.0.0.1:{rport}"
            tmp = self.sh("echo \"$TMP\"").strip()
            self.root = f"{tmp}/prospero-scen-{random.randrange(1 << 32):08x}"
            self.sh(f"mkdir -p {self.root} && cp $HOME/w64devkit/bin/busybox.exe "
                    f"{self.root}/busybox.exe && cp {shlex.quote(prospero)} "
                    f"{self.root}/prospero.exe")
        except BaseException:
            self.close()
            raise
        self.exe = f"{self.root}/prospero.exe"

    def close(self):
        """Remove the root there, then stop the master connection, and
        remove its directory here once it is gone."""
        try:
            if self.root:
                self.sh(cleanroot(self.root))
        finally:
            subprocess.run(["ssh", "-S", self.ctl, "-O", "exit", self.host],
                           capture_output=True)
            # The master removes its socket as it exits, which may be a
            # moment after "-O exit" returns. One still there after that
            # stays with its directory, for whoever must stop the master.
            deadline = time.monotonic() + 10
            while os.path.exists(self.ctl) and time.monotonic() < deadline:
                time.sleep(0.1)
            if not os.path.exists(self.ctl):
                shutil.rmtree(self.ctldir, ignore_errors=True)

    def leftovers(self):
        """What close left here."""
        return [self.ctldir] if os.path.lexists(self.ctldir) else []

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
    """Windows XP over skeeto/sshd, whose commands run in busybox sh.
    It takes one channel per connection and forwards no ports, and XP's
    firewall blocks a new program's listening, so the harness there
    connects out to the fake provider on this machine's LAN address.
    Each command is a connection of its own, retried when the link (Wi-Fi,
    which stalls now and then) drops it."""

    def __init__(self, host, prospero, url):
        self.host = host
        self.prospero = prospero
        self.url = url
        tmp = self.sh("echo \"$TMP\"").strip()
        run = f"{random.randrange(1 << 32):08x}"
        self.root = f"{tmp}/prospero-scen-{run}"
        # A name of the run's own, which no other run's harness, nor any
        # engine of the user's, has
        self.exename = f"p-{run}.exe"
        self.sh(f"mkdir -p {self.root} && "
                f"cp \"$HOME/w64devkit/bin/busybox.exe\" {self.root}/busybox.exe && "
                f"cp {shlex.quote(prospero)} {self.root}/{self.exename}")
        self.exe = f"{self.root}/{self.exename}"

    def close(self):
        self.sh(cleanroot(self.root), check=False)

    def leftovers(self):
        return []  # nothing here

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


def tmplinks(root):
    """This user's links in /tmp, by the harness's names for them
    (os_endpoint, main_posix.cpp), that lead into root."""
    if not hasattr(os, "geteuid"):
        return []  # Windows: pipe names
    real = os.path.realpath(root) + os.sep
    out = []
    for name in os.listdir("/tmp"):
        path = os.path.join("/tmp", name)
        if not name.startswith(f"prospero-{os.geteuid()}-"):
            continue
        try:
            if os.lstat(path).st_uid == os.geteuid() and os.readlink(path).startswith(real):
                out.append(path)
        except OSError:
            pass  # gone meanwhile, or no link
    return out


def leftbehind(host):
    """After host.close(): fail the run if it left anything here."""
    left = host.leftovers()
    if left:
        print(f"FAIL left behind here: {' '.join(left)}", flush=True)
        sys.exit(1)


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
        self.config = host.join(base, "prospero.ini")
        # The system's temporary directory, the run's own, for the
        # sessions' scratch directories (os_scratchdir)
        self.tmp = host.join(base, "tmp")
        # A home of its own, so that the user's configuration, skills and
        # instructions never reach a scenario
        self.home = host.join(base, "home")
        host.mkdir(self.work)
        host.mkdir(self.state)
        host.mkdir(self.tmp)
        host.mkdir(self.home)
        if host.windows:
            win = self.tmp.replace("/", "\\")
            self.tmpenv = {"TMP": win, "TEMP": win, "HOME": self.home,
                           "APPDATA": host.join(self.home, "AppData").replace("/", "\\")}
        else:
            self.tmpenv = {"TMPDIR": self.tmp, "HOME": self.home,
                           "XDG_CONFIG_HOME": host.join(self.home, ".config")}
        # Endpoint settings, with "_top" settings before any section,
        # "_path" after the endpoint's URL, "_extra" as more sections,
        # where {model} and {model2} are the scenario's model names, and
        # {nonce} its own part of a model name (evalmodel), and
        # any other "_name" as the section [name] ("_guard" in [guard],
        # "_tools" in [tools] and so on)
        top = conf.pop("_top", {})
        path = conf.pop("_path", "")
        extra = conf.pop("_extra", "")
        sections = {k[1:]: conf.pop(k) for k in list(conf) if k.startswith("_")}
        # The run's directories may be in /tmp (Linux), where the paths
        # "outside" that scenarios ask about would need no asking with
        # [guard] temp = allow: that only where a scenario says so
        sections.setdefault("guard", {}).setdefault("temp", "ask")
        ini = {"first_byte_timeout": 5, "idle_timeout": 3,
               "progress_timeout": 6, "connect_timeout": 2}
        ini.update(conf)
        body = "".join(f"{k} = {v}\n" for k, v in top.items())
        body += f"[endpoint fake]\nurl = {host.url}/{path}\n" + "".join(
            f"{k} = {v}\n" for k, v in ini.items())
        for name, settings in sections.items():
            body += f"[{name}]\n" + "".join(f"{k} = {v}\n" for k, v in settings.items())
        body += extra.format(model=self.model, model2=self.model2, nonce=self.nonce)
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
        p = self.host.popen(args, self.work, env=dict(self.tmpenv, **(env or {})))
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

    def note(self, msg):
        self.notes.append(msg)

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
        self.p = t.host.popen(["--headless"] + t.args(), t.work, stdin=True,
                              env=dict(t.tmpenv, **(env or {})))
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


# The last line of a subagent's call: how long it worked.
WORKED = re.compile(r"\n\n\[The subagent worked for [0-9][0-9. a-z]*\.\]\Z")


def report(text):
    """A subagent's call's text without its last line, how long the
    subagent worked, or None if that line is missing."""
    m = WORKED.search(text)
    return text[:m.start()] if m else None


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
    t.check("hello there" in ends[2]["text"] and "[exit code 0, after " in ends[2]["text"],
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
    # The record says what was repaired, and why the last call failed
    ends = r.all("tool.end")
    t.check([e.get("repair") for e in ends] ==
            ["raw control characters escaped",
             "arguments were a JSON string holding the object", None] and
            [e.get("fail") for e in ends] == [None, None, "args"],
            f"repair and fail: {[(e.get('repair'), e.get('fail')) for e in ends]}")
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
    t.check(end.get("fail") == "unknown", f"fail {end.get('fail')}")


@scenario(bound=10)
def same_call(t):
    t.put("loop.txt", "same\n")
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [e["status"] for e in r.all("tool.end")]
    t.check(st == ["ok"] * 4 + ["error"] * 2, f"the guard trips at 5: {st}")
    fails = [e.get("fail") for e in r.all("tool.end")]
    t.check(fails == [None] * 4 + ["loop"] * 2, f"fail {fails}")
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


@scenario(conf={"_tools": {"turn_budget": 3}}, bound=8)
def turn_budget(t):
    """[tools] turn_budget ends a turn at that many model requests."""
    r = t.run()
    end = r.last("turn.end")
    t.check(r.rc == 3 and end.get("why") == "budget" and
            end.get("err") == "the turn reached its limit of 3 model requests",
            f"exit {r.rc}, end {end}")
    t.check(len(t.requests()) == 3, f"{len(t.requests())} requests")


@scenario(conf={"_tools": {"turn_budget": 0}}, bound=60)
def turn_unlimited(t):
    """turn_budget = 0: no limit, past the default's 300 requests, and no
    note at 200 of an end to come."""
    r = t.run(timeout=120)
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')}")
    t.check(len(t.requests()) == 306, f"{len(t.requests())} requests")
    t.check(not any("model requests" in n.get("text", "") for n in r.all("note")),
            "no budget note")
    t.check(r.text() == "No limit.", f"text {r.text()!r}")


@scenario(conf={"_extra": "[model {model}]\nempty_retries = 3\n"}, bound=8)
def empty_more(t):
    """empty_retries: three reminders after empty responses, not two."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    nudges = [(n["kind"], n["cap"]) for n in r.all("nudge")]
    t.check(nudges == [("empty", 3)] * 3, f"nudges {nudges}")
    t.check(r.text() == "Third time lucky.", f"text {r.text()!r}")


@scenario(conf={"_extra": "[model {model}]\nempty_retries = 0\n"}, bound=8)
def empty_none(t):
    """empty_retries = 0: the turn ends at the first empty response."""
    r = t.run()
    end = r.last("turn.end")
    t.check(r.rc == 1 and end.get("why") == "error" and
            "kept returning empty responses" in end.get("err", ""), f"end {end}")
    t.check(not r.all("nudge"), "no reminders")
    t.check(len(t.requests()) == 1, "one request")


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


def waitpids(t, rel, n=1, within=5):
    deadline = time.monotonic() + within
    while time.monotonic() < deadline and len(t.pids(rel)) < n:
        time.sleep(0.05)
    return t.pids(rel)


@scenario(bound=20)
def bg_shell(t):
    """The user moves a running command to the background: its call ends
    at once, as a job, and the agent reads that in its next request, with
    a steer sent while the command ran (which a steer sent after the move
    may miss, that request being under way); the job's end comes as its
    notice."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        r = h.call("background", sid=sid)
        t.check(r.get("ok") == 0 and
                r.get("err") == "nothing running that can go to the background",
                f"idle: {r}")
        h.send("prompt", sid=sid, text="go")
        start = h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        pids = waitpids(t, "bgpid")
        t.check(pids, "the command started")
        r = h.call("background", sid=sid, call="nope")
        t.check(r.get("ok") == 0 and "not the one running" in r.get("err", ""),
                f"another call: {r}")
        h.call("steer", sid=sid, text="steer-after-bg")
        time.sleep(1)
        r = h.call("background", sid=sid, call=start["call"])
        t.check(r.get("ok") == 1, f"moved: {r}")
        end = h.wait(lambda e: e.get("ev") == "tool.end" and e.get("sid") == sid)
        t.check(end["status"] == "background" and end.get("job") == 1 and
                "started" in end["text"] and end["ms"] < 3000, f"the call ended: {end}")
        t.check(re.search(r"\[the user moved it to the background after [12](\.[0-9])? s, so "
                          r"it continues as job 1\. You will be told when it ends: do not "
                          r"poll it or wait for it\.", end["text"]), f"its text: {end['text']!r}")
        js = h.all("job.start", sid)
        t.check(js and js[0]["call"] == start["call"], f"job.start {js}")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        msgs = t.requests()[1]["body"]["messages"]
        tool = [m for m in msgs if m["role"] == "tool"]
        t.check(tool and "the user moved it to the background" in tool[-1]["content"],
                f"the agent read the result: {tool}")
        t.check(msgs[-1]["role"] == "user" and msgs[-1]["content"] == "steer-after-bg",
                f"and the steer, in the same request: {msgs[-1]}")
        je = h.wait(lambda e: e.get("ev") == "job.end" and e.get("sid") == sid)
        t.check(je["status"] == "exited" and je.get("exit") == 0 and je.get("resume") == 1,
                f"job.end {je}")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
               e.get("turn") == 2)
        n = h.all("notice", sid)
        t.check(n and n[-1]["kind"] == "job" and "finished" in n[-1]["text"], f"notice {n}")
        turns = h.all("turn", sid)
        t.check(turns[-1].get("via") == "job", f"turns {turns}")
        last = t.requests()[-1]["body"]["messages"]
        t.check(last[-1]["content"].startswith("Background job 1 ended: it exited with code 0"),
                f"told: {last[-1]}")
        t.check(t.gone(pids), "the job's process is gone")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def bg_full(t):
    """With 16 jobs running, a command cannot be moved to the background."""
    h = t.headless()
    pids = []
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        pids = waitpids(t, "full.pids", 17, within=10)
        t.check(len(pids) == 17, f"16 jobs and a command: {pids}")
        r = h.call("background", sid=sid)
        t.check(r.get("ok") == 0 and r.get("err") == "16 jobs are already running: stop "
                "one first", f"refused: {r}")
        h.call("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "cancel", f"cancelled: {end}")
        t.check(len(h.all("job.start", sid)) == 16, "no 17th job")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    t.check(t.gone(pids), f"all gone: {pids}")


def bgtask(t, h, sid):
    """Start the task, move it to the background mid-turn, and check the
    parent's call: the subagent's session."""
    h.send("prompt", sid=sid, text="go")
    kid = h.wait(lambda e: e.get("ev") == "session" and e.get("parent") == sid)["sid"]
    h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == kid)
    r = h.call("background", sid=sid, call="nope")
    t.check(r.get("ok") == 0, f"no such call: {r}")
    r = h.call("background", sid=sid)
    t.check(r.get("ok") == 1, f"moved: {r}")
    end = h.wait(lambda e: e.get("ev") == "tool.end" and e.get("sid") == sid)
    t.check(end["status"] == "background" and end.get("child") == kid and
            end["text"] == f"The user moved this subagent to the background. It continues "
                           f"in session {kid}, and its report comes to you as a notice "
                           f"when it finishes: do not wait for it or poll it.",
            f"the call: {end}")
    return kid


def bgheard(t, h, sid, kid, turn):
    """The subagent's report, as a notice, began the parent's turn."""
    t.put("go.flag", "")
    kend = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == kid)
    t.check(kend["why"] == "done", f"the subagent finished: {kend}")
    pend = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
                  e.get("turn") == turn)
    t.check(pend["why"] == "done", f"the parent's turn {pend}")
    tn = h.all("turn", sid)[-1]
    t.check(tn.get("via") == "task", f"via task: {tn}")
    n = [e for e in h.all("notice", sid) if e.get("kind") == "task"]
    t.check(len(n) == 1 and n[0].get("child") == kid and
            n[0].get("child_title") == "Slow child" and
            n[0]["text"] == f"The subagent \"Slow child\" (session {kid}), which the user "
                            f"moved to the background, finished. Its report:\n\n"
                            f"Child report: flagged", f"notice {n}")
    last = t.requests()[-1]["body"]["messages"]
    t.check(last[-1]["role"] == "user" and "Child report: flagged" in last[-1]["content"],
            f"the agent read it: {last[-1]}")


@scenario(bound=25)
def bg_task(t):
    """A subagent moved to the background mid-turn: the parent's call
    ends at once and its turn goes on and ends; the subagent's report
    comes later as a notice, which starts a turn of the parent's."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        kid = bgtask(t, h, sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"the parent's turn ended: {end}")
        msgs = t.requests()[1]["body"]["messages"]
        t.check(msgs[-1]["role"] == "tool" and
                msgs[-1]["content"].startswith("The user moved this subagent"),
                f"the parent read it: {msgs[-1]}")
        t.check(not h.all("turn.end", kid), "the subagent is still at work")
        note = [e for e in h.all("note", kid) if "background" in e.get("text", "")]
        t.check(note, "the subagent's transcript says so")
        bgheard(t, h, sid, kid, 2)
        r = h.call("background", sid=sid)
        t.check(r.get("ok") == 0, f"nothing left: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def bg_task_park(t):
    """The parent parks while its subagent works in the background: the
    report revives it from its log."""
    h = t.headless(env=PARK)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        kid = bgtask(t, h, sid)
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)

        def entry():
            h.call("list")
            return [e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid][0]

        deadline = time.monotonic() + 5
        while not entry().get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        t.check(entry().get("parked") == 1, "the parent parked")
        bgheard(t, h, sid, kid, 2)
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def bg_task_closed(t):
    """The parent is closed and forgotten while its subagent works in the
    background: the report opens it again from its log."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        kid = bgtask(t, h, sid)
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        h.call("close", sid=sid, forget=1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            h.call("status")
            if h.all("reply")[-1].get("open") == 1:  # the subagent alone
                break
            time.sleep(0.1)
        t.check(h.all("reply")[-1].get("open") == 1, "the parent left memory")
        bgheard(t, h, sid, kid, 2)
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def bg_task_cancel(t):
    """Cancelling the parent's turn leaves its subagent in the background
    at work, whose report starts the parent's next turn."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        kid = bgtask(t, h, sid)
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid and
               e.get("name") == "shell")
        pids = waitpids(t, "parent.pid")
        h.call("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "cancel", f"the parent cancelled: {end}")
        t.check(t.gone(pids), "its command is gone")
        time.sleep(0.3)
        t.check(not h.all("turn.end", kid), "the subagent goes on")
        bgheard(t, h, sid, kid, 2)
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


@scenario(bound=15)
def instructions_own(t):
    """The agent's own write to an instruction file: logged as changed,
    and the agent is not told its own change back."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("AGENTS.md", "Rule one.\n")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="first")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        h.send("prompt", sid=sid, text="second")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        t.check(t.file("AGENTS.md") == "Rule two.\n", "written")
        reqs = [x["body"]["messages"] for x in t.requests()]
        told = [m for r in reqs for m in r if "Instruction files changed" in str(m.get("content"))]
        t.check(not told, f"not told: {told}")
        ins = h.all("instructions", sid)
        t.check([i["why"] for i in ins] == ["start", "changed"], f"records {ins}")
        t.check(ins[1]["files"][0]["bytes"] == len("Rule two.\n"), f"restamped {ins[1]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def instructions_gone_reopen(t):
    """An instruction file removed: the agent is told once, and not again
    after the session is opened in another process."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.put("AGENTS.md", "Rule one.\n")
    gone = "This file is gone"
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="first")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        h.send("prompt", sid=sid, text="second")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        ins = h.all("instructions", sid)
        t.check([i["why"] for i in ins] == ["start", "changed"], f"records {ins}")
        t.check(ins[1]["files"][0]["mtime"] == -1, f"gone: {ins[1]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    t.check(t.file("AGENTS.md") is None, "removed")
    h = t.headless()
    try:
        t.check(h.call("open", sid=sid).get("ok") == 1, "reopened")
        h.send("prompt", sid=sid, text="third")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 3)
        ins = h.all("instructions", sid)
        t.check([i["why"] for i in ins] == ["start", "changed"], f"no new record: {ins}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    reqs = [x["body"]["messages"] for x in t.requests()]
    told = [i for i, r in enumerate(reqs) if gone in str(r[-1].get("content"))]
    t.check(len(reqs) == 4 and told == [2], f"told once, in turn 2: {told} of {len(reqs)}")


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
    t.host.mkdir(t.host.join(xdg, "prospero"))
    t.host.write(t.host.join(xdg, "prospero", "AGENTS.md"), "Global rule: be brief.\n")
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


def skillhome(t):
    """A home and configuration directory of the scenario's own, where
    the user's skills are, never the real ones: the environment to run
    with."""
    home = t.host.join(t.base, "home")
    xdg = t.host.join(t.base, "xdg")
    t.host.mkdir(home)
    t.host.mkdir(t.host.join(xdg, "prospero", "skills"))
    env = {"HOME": home, "XDG_CONFIG_HOME": xdg}
    if t.host.windows:
        env["USERPROFILE"] = home.replace("/", "\\")
    return env


@scenario(conf={"_tools": {"shell": "powershell"}}, bound=30)
def powershell(t):
    """[tools] shell = powershell on Windows: the command goes through an
    environment variable, output is UTF-8, and the exit status is the last
    statement's, or its native program's."""
    if not t.host.windows:
        t.note("Windows only")
        return
    r = t.run(timeout=60)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = [e["text"] for e in r.all("tool.end")]
    t.check(len(ends) == 5, f"{len(ends)} shell calls")
    if len(ends) < 5:
        return
    t.check("h\u00e9llo" in ends[0] and "w\u00e9ll" in ends[0]
            and "[exit code 0, after " in ends[0], f"UTF-8 output: {ends[0]!r}")
    t.check("[exit code 1, after " in ends[1], f"a failed cmdlet: {ends[1]!r}")
    t.check("[exit code 3, after " in ends[2], f"a native status: {ends[2]!r}")
    t.check("PROSPERO_COMMAND" not in ends[3] and "end" in ends[3]
            and "[exit code 0, after " in ends[3], f"variable removed: {ends[3]!r}")
    t.check("[exit code 1, after " in ends[4] and "line:1" in ends[4]
            and "__ok" not in ends[4], f"a parse error: {ends[4]!r}")
    tools = json.dumps(t.requests()[0]["body"].get("tools"))
    t.check("PowerShell" in tools, "the shell tool describes PowerShell")


@scenario(bound=20)
def path_settings(t):
    """[tools] path_prepend and path_append: the agent's shell finds a
    program by its bare name in a directory added before PATH, and one in
    a directory added after; PATH starts with the first and ends with the
    second, the inherited one between, so that a name in both is the
    prepended one's, and a system program (cat) is not the appended
    directory's."""
    pre = t.host.join(t.base, "pre")
    post = t.host.join(t.base, "post")
    t.host.mkdir(pre)
    t.host.mkdir(post)

    def tool(d, name, says):
        if t.host.windows:
            t.host.write(t.host.join(d, name + ".bat"), f"@echo {says}\r\n")
        else:
            p = t.host.join(d, name)
            t.host.write(p, f"#!/bin/sh\necho {says}\n")
            os.chmod(p, 0o755)
    tool(pre, "prospero_pre_tool", "pre-tool-ran")
    tool(post, "prospero_post_tool", "post-tool-ran")
    tool(pre, "prospero_both_tool", "both-from-pre")
    tool(post, "prospero_both_tool", "both-from-post")
    tool(post, "cat", "appended-cat")
    t.put("seen.txt", "the real cat\n")
    t.host.write(t.config, t.host.read(t.config) +
                 f"[tools]\npath_prepend = {pre}\npath_append = ; {post}\n")

    r = t.run(timeout=60)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = [e["text"] for e in r.all("tool.end")]
    t.check(len(ends) == 5, f"{len(ends)} shell calls")
    if len(ends) < 5:
        return
    t.check("pre-tool-ran" in ends[0] and "[exit code 0, after " in ends[0],
            f"prepended: {ends[0]!r}")
    t.check("post-tool-ran" in ends[1] and "[exit code 0, after " in ends[1],
            f"appended: {ends[1]!r}")
    t.check("both-from-pre" in ends[2], f"prepended first: {ends[2]!r}")
    t.check("the real cat" in ends[3] and "appended-cat" not in ends[3],
            f"the system's cat first: {ends[3]!r}")
    line = next((x for x in ends[4].splitlines() if x.startswith("PATH=")), "")
    sep = ";" if t.host.windows and ";" in line else ":"

    def norm(p):
        p = p.replace("\\", "/").rstrip("/")
        return p.lower() if t.host.windows else p
    dirs = [norm(d) for d in line[5:].split(sep) if d]
    t.check(len(dirs) > 2 and dirs[0] == norm(pre) and dirs[-1] == norm(post),
            f"PATH from {norm(pre)} to {norm(post)}: {line!r}")


def skillput(t, parent, name, text):
    d = t.host.join(parent, name)
    t.host.mkdir(d)
    t.host.write(t.host.join(d, "SKILL.md"), text)
    return d


@scenario(bound=15)
def skills(t):
    """Skills of the user and of the project, the nearer of a name first,
    listed in the system prompt; the skill tool gives one's instructions,
    after which the agent reads in its directory without asking; a hidden
    skill is neither listed nor loaded, a malformed one left out with a
    warning."""
    env = skillhome(t)
    user = t.host.join(env["XDG_CONFIG_HOME"], "prospero", "skills")
    notes = skillput(t, user, "notes", "---\nname: notes\n"
                     "description: \"Take notes: carefully\"  # quoted\n---\nNOTES-BODY\n")
    t.host.write(t.host.join(notes, "ref.txt"), "reference text\n")
    skillput(t, user, "pdf", "---\ndescription: User PDF\n---\nUSER-PDF\n")
    skillput(t, t.host.join(env["HOME"], ".claude", "skills"), "review",
             "---\r\nname: review\r\ndescription: Review\r\n"
             "disable-model-invocation: true\r\n---\r\nREVIEW\r\n")
    # The repository is the run's directory, the work directory in it
    t.host.mkdir(t.host.join(t.base, ".git"))
    skillput(t, t.host.join(t.base, ".claude", "skills"), "lint", "---\ndescription: Far lint\n---\n")
    skillput(t, t.host.join(t.work, ".agents", "skills"), "lint", "---\ndescription: Near lint\n---\n")
    proj = t.host.join(t.work, ".claude", "skills")
    skillput(t, proj, "pdf", "---\ndescription: Project PDF\n---\nPROJECT-PDF\n")
    skillput(t, proj, "broken", "---\nname: broken\ndescription: never closed\n")
    # Cursor's, below a folder that groups them, and Codex's in the home
    skillput(t, t.host.join(t.base, ".cursor", "skills", "team"), "deploy",
             "---\ndescription: Cursor deploy\n---\n")
    skillput(t, t.host.join(env["HOME"], ".codex", "skills"), "codex",
             "---\ndescription: Codex skill\n---\n")

    r = t.run(env=env)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    reqs = [q["body"] for q in t.requests()]
    system = fake_provider.system_of(reqs[0])
    t.check("# Skills" in system and "skill" in fake_provider.tool_names(reqs[0]), "offered")
    listed = [x for x in system.split("\n") if x.startswith("- ")]
    for want in ("- lint: Near lint", "- notes: Take notes: carefully", "- pdf: Project PDF",
                 "- deploy: Cursor deploy", "- codex: Codex skill"):
        t.check(want in listed, f"{want!r} in {listed}")
    for unwanted in ("User PDF", "Far lint", "- review:", "- broken:"):
        t.check(unwanted not in system, f"{unwanted} not in the system prompt")
    t.check(all(fake_provider.system_of(q) == system for q in reqs), "the system prompt stays")
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok", "ok", "error"], f"statuses {ends}")
    body = ends[0]["text"]
    t.check("NOTES-BODY" in body and "Base directory for this skill: " in body and
            body.endswith("ARGUMENTS: the args") and "description" not in body, f"body {body!r}")
    t.check("reference text" in ends[1]["text"], f"read {ends[1]}")
    t.check(not r.all("ask"), "no ask for the skill's directory")
    grants = [g for g in r.all("grant") if g.get("by") == "skill"]
    t.check(len(grants) == 1 and grants[0]["path"].replace("\\", "/").endswith("/notes") and
            grants[0]["access"] == "read", f"grants {grants}")
    t.check("No skill is named \"review\"" in ends[2]["text"], f"hidden {ends[2]}")
    warns = [n["text"] for n in r.all("note")
             if n.get("level") == "warn" and "skill left out" in n["text"]]
    t.check(len(warns) == 1 and "broken" in warns[0] and "closing ---" in warns[0],
            f"warned {warns}")


@scenario(bound=15)
def skillsdir(t):
    """--skillsdir, given twice, the second relative to the current
    directory: both directories' skills listed and loaded, after [skills]
    paths and before the user's own."""
    env = skillhome(t)
    user = t.host.join(env["XDG_CONFIG_HOME"], "prospero", "skills")
    skillput(t, user, "pdf", "---\ndescription: User PDF\n---\n")
    one = t.host.join(t.base, "sd1")
    two = t.host.join(t.base, "sd2")
    conf = t.host.join(t.base, "sdconf")
    skillput(t, one, "alpha", "---\ndescription: Alpha\n---\n")
    skillput(t, one, "pdf", "---\ndescription: Flag PDF\n---\n")
    skillput(t, one, "csv", "---\ndescription: Flag CSV\n---\n")
    skillput(t, two, "beta", "---\ndescription: Beta\n---\nBETA-BODY\n")
    skillput(t, conf, "csv", "---\ndescription: Config CSV\n---\n")
    t.host.write(t.config, t.host.read(t.config) + f"[skills]\npaths = {conf}\n")

    r = t.run(env=env, extra=["--skillsdir", one, "--skillsdir", "../sd2"])
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    system = fake_provider.system_of(t.requests()[0]["body"])
    listed = [x for x in system.split("\n") if x.startswith("- ")]
    for want in ("- alpha: Alpha", "- beta: Beta", "- pdf: Flag PDF", "- csv: Config CSV"):
        t.check(want in listed, f"{want!r} in {listed}")
    ends = r.all("tool.end")
    t.check(len(ends) == 1 and "BETA-BODY" in ends[0]["text"], f"loaded {ends}")


@scenario(bound=20)
def skill_prompt(t):
    """/name from the user invokes a skill, a hidden one too: its body in
    the prompt, the user record naming it; skills lists them all; one
    added mid-session is told as a notice, which reopening does not
    repeat."""
    env = skillhome(t)
    t.host.mkdir(t.host.join(t.work, ".git"))
    proj = t.host.join(t.work, ".claude", "skills")
    skillput(t, proj, "review", "---\nname: review\ndescription: Review a change\n"
             "disable-model-invocation: true\n---\nREVIEW-BODY\n")
    skillput(t, proj, "pdf", "---\ndescription: Read PDFs\n---\nPDF-BODY\n")
    told = "The skills changed"
    h = t.headless(env)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        sk = h.call("skills", sid=sid)
        got = [(k["name"], k["description"], k["hidden"]) for k in sk.get("skills", [])]
        t.check(sk.get("ok") == 1 and sk.get("sid") == sid and
                got == [("pdf", "Read PDFs", False), ("review", "Review a change", True)] and
                all(k["path"].endswith("SKILL.md") for k in sk["skills"]), f"skills {sk}")
        bad = h.call("prompt", sid=sid, text="x", skill="nope")
        t.check(bad.get("ok") == 0 and "no such skill" in bad.get("err", ""), f"nope {bad}")
        h.send("prompt", sid=sid, text="src/a.c", skill="review")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        reqs = [q["body"] for q in t.requests()]
        last = reqs[0]["messages"][-1]["content"]
        t.check(last.startswith("<skill name=\"review\" dir=\"") and "REVIEW-BODY" in last and
                last.endswith("</skill>\n\nsrc/a.c"), f"prompt {last!r}")
        system = fake_provider.system_of(reqs[0])
        t.check("- pdf: Read PDFs" in system and "- review:" not in system, "listed")
        user = h.all("user", sid)
        t.check(len(user) == 1 and user[0].get("skill") == "review" and
                user[0].get("args") == "src/a.c", f"user {user}")

        skillput(t, proj, "fresh", "---\ndescription: Fresh skill\n---\nFRESH\n")
        h.send("prompt", sid=sid, text="second")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 2)
        reqs = [q["body"] for q in t.requests()]
        msgs = [str(m.get("content")) for m in reqs[1]["messages"] if told in str(m.get("content"))]
        t.check(len(msgs) == 1 and msgs[0].endswith("\n- fresh: Fresh skill (new)"),
                f"told {msgs}")
        t.check(fake_provider.system_of(reqs[1]) == system, "the system prompt stays")
        notices = [n for n in h.all("notice", sid) if n.get("kind") == "skills"]
        t.check(len(notices) == 1 and [k["name"] for k in notices[0].get("skills", [])] ==
                ["fresh", "pdf"], f"notices {notices}")
        seen = notices[0]["seq"]
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")

    h = t.headless(env)
    try:
        o = h.call("open", sid=sid)
        t.check(o.get("ok") == 1, f"opened {o}")
        h.send("prompt", sid=sid, text="third")
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and e.get("turn") == 3)
        reqs = [q["body"] for q in t.requests()]
        t.check(len(reqs) == 3, f"requests {len(reqs)}")
        msgs = [m for m in reqs[2]["messages"] if told in str(m.get("content"))]
        t.check(len(msgs) == 1, f"told once: {len(msgs)}")
        again = [n for n in h.all("notice", sid) if n.get("kind") == "skills" and n["seq"] > seen]
        t.check(not again, f"no notice after reopening: {again}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def skill_task(t):
    """A subagent has the skill tool and the listing too."""
    env = skillhome(t)
    t.host.mkdir(t.host.join(t.work, ".git"))
    skillput(t, t.host.join(t.work, ".claude", "skills"), "pdf",
             "---\ndescription: Read PDFs\n---\nPDF-BODY\n")
    r = t.run(env=env)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    t.check(len(kids) == 1, f"kids {kids}")
    kreqs = [q["body"] for q in t.requests(t.model2)]
    t.check(kreqs and "skill" in fake_provider.tool_names(kreqs[0]) and
            "- pdf: Read PDFs" in fake_provider.system_of(kreqs[0]), "the child's tools")
    kend = r.all("tool.end", kids[0]["sid"])
    t.check(len(kend) == 1 and kend[0]["status"] == "ok" and "PDF-BODY" in kend[0]["text"],
            f"child {kend}")
    ends = r.all("tool.end", own)
    t.check(len(ends) == 1 and "Child read: " in ends[0]["text"] and
            "PDF-BODY" in ends[0]["text"], f"parent {ends}")


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


@scenario(bound=15)
def unqueue(t):
    """Steers and prompts sent while a response streams wait in a queue,
    each record of it listing them with their ids; one withdrawn never
    reaches the agent, and comes back whole. Reopening shows the queue;
    a withdrawal once it was taken in, or twice, is refused."""
    h = t.headless()

    def queues(n):
        """The queue records, once there are n: each follows its reply."""
        deadline = time.monotonic() + 5
        while len(h.all("queue", sid)) < n and time.monotonic() < deadline:
            time.sleep(0.02)
        return h.all("queue", sid)

    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "delta" and e.get("sid") == sid)
        sent = [("steer", "withdraw this steer"), ("steer", "keep this steer"),
                ("prompt", "withdraw this prompt"), ("prompt", "keep this prompt")]
        for cmd, text in sent:
            t.check(h.call(cmd, sid=sid, text=text).get("ok") == 1, f"{cmd} {text}")
        q = queues(4)
        t.check(len(q) == 4, f"a queue record each: {q}")
        items = q[-1]["items"]
        t.check([(i["kind"], i["text"]) for i in items] == sent and
                len({i["id"] for i in items}) == 4, f"the queue: {items}")
        ids = {i["text"]: i["id"] for i in items}

        r = h.call("unqueue", sid=sid, item=ids["withdraw this steer"])
        t.check(r.get("ok") == 1 and r.get("text") == "withdraw this steer", f"unqueue: {r}")
        r = h.call("unqueue", sid=sid, item=ids["withdraw this prompt"])
        t.check(r.get("ok") == 1 and r.get("text") == "withdraw this prompt", f"unqueue: {r}")
        r = h.call("unqueue", sid=sid, item=ids["withdraw this prompt"])
        t.check(r.get("ok") == 0 and r.get("err"), f"twice: {r}")
        left = [i["text"] for i in queues(6)[-1]["items"]]
        t.check(left == ["keep this steer", "keep this prompt"], f"left: {left}")

        # Opened again, as a frontend coming back would: the queue as it is
        n = len(h.all("queue", sid))
        t.check(h.call("open", sid=sid, **{"from": 1 << 40}).get("ok") == 1, "reopened")
        q = queues(n+1)
        t.check(len(q) == n+1 and [i["text"] for i in q[-1]["items"]] == left,
                f"the queue on open: {q[n:]}")

        # The prompt starts the next turn, then the steer, too late for
        # the one-step turn, as a prompt; neither can be withdrawn once
        # taken in
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid
               and e.get("turn") == 3, timeout=10)
        r = h.call("unqueue", sid=sid, item=ids["keep this steer"])
        t.check(r.get("ok") == 0, f"taken in already: {r}")
        users = [(e["text"], e.get("via")) for e in h.all("user", sid)]
        t.check(users[1:] == [("keep this prompt", "queue"), ("keep this steer", "queue")]
                and users[0][0] == "go", f"messages: {users}")
        t.check(h.all("queue", sid)[-1]["items"] == [], "the queue ends empty")
        # Frontends show the queue as its records say, through turn ends:
        # it empties only at the last, and each prompt taken in comes as
        # a message while still queued, never shown in neither
        t.check(all(q["items"] for q in h.all("queue", sid)[:-1]), "emptied only once")
        mine = [e for e in h.events if e.get("sid") == sid and e.get("ev") in ("user", "queue")]
        for i, e in enumerate(mine):
            if e["ev"] == "user" and e.get("via") == "queue":
                last = [q for q in mine[:i] if q["ev"] == "queue"][-1:]
                t.check(last and e["text"] in [q["text"] for q in last[0]["items"]],
                        f"queued until taken in: {e['text']}")
        said = json.dumps([r["body"]["messages"] for r in t.requests()])
        t.check("withdraw this" not in said, "nothing withdrawn reached the agent")
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
def hello_caps(t):
    """hello names every command (cmds) and the engine's features (caps);
    the commands of M8 refuse a session that is not there, or a turn that
    is not, without touching the session named."""
    h = t.headless()
    try:
        r = h.call("hello", proto=0, client="scenarios")
        t.check(r.get("ok") == 1, f"hello: {r}")
        hello = h.all("hello")
        t.check(len(hello) == 1, f"one hello: {hello}")
        hello = hello[0]
        t.check(hello.get("proto") == 0 and hello.get("version"), f"hello: {hello}")
        m8 = ["rename", "delete", "archive", "group", "fork", "compact", "model",
              "draft", "upload", "import"]
        cmds = hello.get("cmds", [])
        want = ["hello", "list", "new", "open", "close", "prompt", "steer", "cancel",
                "answer", "config.get", "config.set", "quit", "rewind", "job.kill",
                "status", "stop", "file"] + m8 + ["skills", "mode", "unqueue", "dir",
                                                   "background", "models", "mcp",
                                                   "mcp.auth", "mcp.code", "mcp.logout",
                                                   "mcp.restart"]
        t.check(cmds == want, f"cmds: {cmds}")
        caps = hello.get("caps", [])
        t.check(isinstance(caps, list) and all(isinstance(c, str) for c in caps) and
                {"close.forget", "config.refresh", "file.base64", "desktop",
                 "subagents", "rename", "title.by", "archive", "group", "draft",
                 "compact", "model", "gauge", "answer.always", "scratch", "delete",
                 "fork", "answer.always.desktop", "skills", "plan", "queue",
                 "message", "bypass", "nofolder", "dir", "config.create", "side", "background", "models",
                 "mcp", "mcp.auth", "task.remote", "task.host"} <= set(caps)
                and "host" not in caps,
                f"caps: {caps}")

        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        local = isinstance(t.host, LocalHost)
        state = t.host.join(t.state, "sessions", sid)
        before = sorted(os.listdir(state)) if local else None
        log = t.host.read(t.host.join(state, "events.jsonl"))
        seen = len([e for e in h.events if e.get("sid") == sid and "seq" in e])
        r = h.call("fork", sid=sid, turn=1)
        t.check(r.get("ok") == 0 and r.get("err") == "no turn 1 in this session" and
                r.get("sid") == sid, f"fork: {r}")
        for cmd in ("delete", "archive", "group", "fork", "draft", "rename", "model"):
            # Commands on a session open here or not
            r = h.call(cmd, sid="20260101-000000-none", title="t", model="m", name="g")
            t.check(r.get("ok") == 0 and r.get("err") == "no such session",
                    f"{cmd} on another: {r}")
            for bad in ("../" + sid, "a\\b", "", "C:x", ".x"):
                r = h.call(cmd, sid=bad)
                t.check(r.get("ok") == 0 and r.get("err") == "invalid session id",
                        f"{cmd} {bad!r}: {r}")
        r = h.call("open", sid="C:x")
        t.check(r.get("ok") == 0 and r.get("err") == "invalid session id", f"open: {r}")
        t.check(t.host.read(t.host.join(state, "events.jsonl")) == log,
                "the log is untouched")
        if local:
            t.check(sorted(os.listdir(state)) == before, f"no sidecars: {os.listdir(state)}")
        records = [e for e in h.events if e.get("sid") == sid and "seq" in e]
        t.check(len(records) == seen, f"no records: {records[seen:]}")
    finally:
        rc = h.close()
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
        t.check(r["path"].endswith("prospero.ini") and len(r["files"]) == 2
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

        # Create, for an editor, leaves a file that exists alone
        r = call("config.set", create=1)
        t.check(r["ok"] == 1 and r["line"] == 0, f"create: {r}")
        r = call("config.set", create=1, key="model")
        t.check(r["ok"] == 0 and "takes no" in r.get("err", ""), f"create refused: {r}")
        t.check(text() == after, "create leaves the file alone")

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


@scenario(bound=20)
def config_delete(t):
    """config.set {delete}: a section of the user's file goes with the
    comment above it, the rest byte for byte; one an include defines is
    refused, saying where; and a session using a deleted endpoint and a
    deleted model, a variant with an id and a temperature, goes on with
    the settings it had."""
    team = t.host.join(t.base, "team.ini")
    shared = "[endpoint team]\nurl = http://team.invalid/\n\n[model shared]\ntemperature = 0.1\n"
    t.host.write(team, shared)
    model = t.model.replace(":config_delete:", ":plain:")
    fake_provider.STATE.register(model)
    text = ("# My settings\n"
            "include = team.ini\n"
            "\n"
            "# The fake provider\n"
            "[endpoint fake]\n"
            f"url = {t.host.url}/\n"
            "first_byte_timeout = 5\n"
            "idle_timeout = 3\n"
            "connect_timeout = 2\n"
            "\n"
            "# A variant of the test's model, by another name\n"
            "[model nick]\n"
            f"id = {model}\n"
            "temperature = 0.25\n"
            "\n"
            "# Notes about roles\n"
            "\n"
            "[role r]  # a role\n"
            "prompt = Be brief.\n"
            "\n"
            "[remote far]\n"
            "ssh = far.invalid\n"
            "list = connected\n")
    t.host.write(t.config, text)

    h = t.headless()
    try:
        r = h.call("config.get")
        heads = {(x["kind"], x["name"]): (x["line"], x["own"]) for x in r["headers"]}
        t.check(heads.get(("endpoint", "fake")) == (5, 1) and
                heads.get(("model", "nick")) == (12, 1) and
                heads.get(("role", "r")) == (18, 1) and
                heads.get(("remote", "far")) == (21, 1) and
                heads.get(("endpoint", "team")) == (1, 0) and
                heads.get(("model", "shared")) == (4, 0), f"headers {r['headers']}")
        got = {(s["kind"], s["name"], s["key"]): s for s in r["settings"]}
        t.check(got[("remote", "far", "list")]["value"] == "connected" and
                got[("guard", "", "temp")]["value"] == "allow" and
                got[("guard", "", "temp")]["default"] == 1, "the new keys")

        sid = h.call("new", cwd=t.work, model="nick", endpoint="fake")["sid"]

        def turn(prompt):
            done = len(h.all("turn.end", sid))
            h.call("prompt", sid=sid, text=prompt)
            h.wait(lambda e: len(h.all("turn.end", sid)) > done)
            body = t.requests(model)[-1]["body"]
            t.check(h.all("turn.end", sid)[-1].get("why") == "done",
                    f"the turn ended done: {h.all('turn.end', sid)[-1]}")
            return body

        body = turn("one")
        t.check(body.get("model") == model and body.get("temperature") == 0.25,
                f"the variant: {body}")

        # Refusals change nothing
        for fields, why in [({"kind": "model", "name": "shared"}, "team.ini:4"),
                            ({"kind": "endpoint", "name": "none"}, "is not in"),
                            ({"kind": "", "name": ""}, "top level"),
                            ({"kind": "endpoint", "name": "fake", "key": "url"},
                             "not a key"),
                            ({"kind": "guard", "name": "g"}, "has no name")]:
            r = h.call("config.set", delete=True, **fields)
            t.check(r["ok"] == 0 and why in r.get("err", ""), f"refusal: {r}")
        t.check(t.host.read(t.config) == text, "refusals leave the file alone")

        # The model and the endpoint the session uses, and a role
        for kind, name in [("model", "nick"), ("endpoint", "fake"), ("role", "r")]:
            r = h.call("config.set", kind=kind, name=name, delete=1)
            t.check(r["ok"] == 1 and r["line"] == 0, f"deleted [{kind} {name}]: {r}")
        want = ("# My settings\n"
                "include = team.ini\n"
                "\n"
                "# Notes about roles\n"
                "\n"
                "[remote far]\n"
                "ssh = far.invalid\n"
                "list = connected\n")
        after = t.host.read(t.config)
        t.check(after == want, f"the file after: {after!r}")
        r = h.call("config.get")
        names = {(x["kind"], x["name"]) for x in r["headers"]}
        t.check(("model", "nick") not in names and ("endpoint", "fake") not in names
                and ("remote", "far") in names, f"headers after {r['headers']}")

        # The session goes on as it was
        body = turn("two")
        t.check(body.get("model") == model and body.get("temperature") == 0.25,
                f"kept: {body}")
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
                     env=dict(t.tmpenv, XDG_CONFIG_HOME=xdg))
    out, err = p.communicate('{"cmd":"config.get","id":1}\n', timeout=10)
    replies = [json.loads(x) for x in out.splitlines() if x.strip()]
    r = next((e for e in replies if e.get("id") == 1), {})
    seedpath = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "assets", "prospero.ini")
    with open(seedpath, "rb") as f:
        seed = f.read().decode()
    written = t.host.read(t.host.join(xdg, "prospero", "prospero.ini"))
    t.check(written == seed, "the seed is written unchanged")
    t.check(r.get("ok") == 1 and r.get("diags") == [] and r.get("rev") == 1,
            f"config.get {r} {err}")
    t.check(r["path"].endswith("prospero/prospero.ini"), f"path {r['path']}")
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
            t.host.killhard(a.p, t.pids("prosperopid")[0])
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
    target = t.host.join(dot, "prospero.ini")
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


SMALL = {"_listing": {"meta": {"n_ctx": 25500, "n_ctx_train": 262144}}}


@scenario(conf=SMALL, bound=25, alone=True)
def compaction(t):
    """A small window: the older reads are summarized, the newest stay
    verbatim, the turn carries on, and a resumed session replays the
    compaction to the same prompt."""
    bigfiles(t, 8, 150)
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


@scenario(conf={"_path": "router1-tpl/", "_extra": (
    "[model {model}]\n"
    "template.thinking = true\n"
    "template.reasoning_effort = 25\n"
    "template.label = \"42\"\n"
    "[model deep-{nonce}]\n"
    "like = {model}\n"
    "template.reasoning_effort = 100\n"
    "template.label =\n")}, bound=20)
def template_variant(t):
    """A model's template arguments go in chat_template_kwargs, and a
    variant (like) is the same model to the server: sent as its id, it
    runs side by side with it on a router with --models-max 1."""
    variant = f"deep-{t.nonce}"
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        b = h.call("new", cwd=t.work, model=variant, endpoint="fake")["sid"]
        h.send("prompt", sid=a, text="go")
        h.wait(lambda e: e.get("sid") == a and e.get("ev") == "live" and
               e.get("state") == "streaming")
        h.send("prompt", sid=b, text="go")
        for s in (a, b):
            end = h.wait(lambda e: e.get("sid") == s and e.get("ev") == "turn.end")
            t.check(end["why"] == "done", f"{s}: {end}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    reqs = t.requests()
    t.check(len(reqs) == 2, f"both sent as the model's id: {len(reqs)}")
    t.check(not t.requests(variant), "nothing sent as the variant's name")
    base, deep = (x["body"] for x in reqs)
    t.check(base["model"] == t.model and deep["model"] == t.model, "the id sent")
    kw = list(base.get("chat_template_kwargs", {}).items())
    t.check(kw == [("thinking", True), ("reasoning_effort", 25), ("label", "42")],
            f"the model's arguments: {kw}")
    kw = list(deep.get("chat_template_kwargs", {}).items())
    t.check(kw == [("thinking", True), ("reasoning_effort", 100)],
            f"the variant's arguments: {kw}")
    t.check(reqs[1]["t"] < reqs[0]["end"], "the variant ran side by side")
    with fake_provider.STATE.lock:
        peak = fake_provider.STATE.peak.get("router1-tpl")
    t.check(peak == 1, f"one model at a time: {peak}")


MINUTE = {"PROSPERO_TEST_MINUTE_MS": "60"}  # a minute in 60 ms


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


@scenario(bound=8)
def default_qualified(t):
    """A default model named with its endpoint, ENDPOINT/MODEL, goes to
    that endpoint, over a default endpoint that names another, which is
    warned about."""
    t.host.write(t.config,
                 "endpoint = dead\n"
                 f"model = live/{t.model}\n"
                 "[endpoint dead]\nurl = http://127.0.0.1:9/\nconnect_timeout = 1\n"
                 f"[endpoint live]\nurl = {t.host.url}/\n"
                 "[guard]\ntemp = ask\n")
    t.args = lambda extra=(): ["-C", t.work, "--state", t.state, "-c", t.config] + list(extra)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.events[0].get("model") == t.model, f"session {r.events[0]}")
    t.check(r.text() == "Default qualified.", f"text {r.text()!r}")
    t.check("model names endpoint live, so this is ignored" in r.stderr,
            f"warned: {r.stderr!r}")


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
    t.check([report(e["text"]) for e in ends] == ["Report: a.txt has 3 lines.",
                                                  "Report: b.txt has 2 lines."],
            f"reports {ends}")
    bysid = {k["sid"]: k["call"] for k in kids}
    t.check(all(bysid.get(e.get("child")) == e["call"] for e in ends), "each names its child")
    t.check(r.last("turn.end", own).get("why") == "done", "the parent ends done")
    heard = re.sub(r"\n\n\[The subagent worked for [^]]*\]", "",
                   r.last("assistant", own)["blocks"][0]["text"])
    t.check(heard == "Both reported: Report: a.txt has 3 lines. / Report: b.txt has 2 lines.",
            "the parent read both")

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
    # Each level's report ends with how long it worked
    heard = re.sub(r"\n\n\[The subagent worked for [^]]*\]", "",
                   r.last("assistant", own)["blocks"][0]["text"])
    t.check(heard == "Top heard: Level 1 heard: Level 2 done; its task was refused.",
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
    t.check(report(ends[0]["text"]) == "Obedient final report.", f"obeyed {ends[0]}")
    t.check("budget of 6 model requests" in ends[1]["text"] and
            report(ends[1]["text"]).endswith("Working on step 5."), f"stopped {ends[1]['text']!r}")
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


ROLEWORDS = """[role every]
tools = *
[role gui]
description = Builds programs and looks at them
tools = default desktop
[role lean]
tools = * -shell, -job_kill
[role odd]
tools = read -bogus
"""


@scenario(conf={"_extra": ROLEWORDS}, bound=15)
def role_words(t):
    """A role's tools read left to right: * is every tool, default those
    of a role that names none, and -NAME takes one away. config.get lists
    the roles, built-in ones first, with the tools each gives, and a new
    session takes one."""
    h = t.headless(env=deskenv(t))
    try:
        h.call("hello", proto=0, client="scenarios")
        cfg = h.call("config.get")
        roles = {r["name"]: r for r in cfg.get("roles", [])}
        t.check(list(roles) == ["explore", "general", "every", "gui", "lean", "odd"],
                f"roles {list(roles)}")
        default = ["read", "write", "edit", "shell", "grep", "glob", "todo_write", "jobs",
                   "job_output", "job_kill", "task", "skill", "sessions", "send_message"]
        every = default[:11] + ["desktop"] + default[11:]
        t.check(roles["general"]["tools"] == default, f"general {roles['general']}")
        t.check(roles["every"]["tools"] == every, f"* {roles['every']}")
        t.check(roles["gui"]["tools"] == every and
                roles["gui"]["description"] == "Builds programs and looks at them",
                f"default desktop {roles['gui']}")
        t.check(roles["lean"]["tools"] == [x for x in every if x not in ("shell", "job_kill")],
                f"* -shell, -job_kill {roles['lean']}")
        t.check(roles["odd"]["tools"] == ["read"], f"read -bogus {roles['odd']}")
        diags = [d["message"] for d in cfg.get("diags", [])]
        t.check(any('unknown value "-bogus"' in d for d in diags) and
                not any("-shell" in d or "-job_kill" in d for d in diags), f"diags {diags}")

        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", role="gui")
        t.check("sid" in r, f"new {r}")
        sid = r["sid"]
        rec = h.all("session", sid)[0]
        t.check(rec["role"] == "gui" and "desktop" in rec["tools"] and
                "enter_plan_mode" not in rec["tools"], f"session {rec}")
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", tools="* -bogus")
        t.check(r.get("ok") == 0 and r.get("err") == "no such tool: -bogus", f"bad word {r}")
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
    t.check(ex["tools"] == ["read", "grep", "glob", "skill"] and ex["model"] == t.model, f"explore {ex}")
    t.check(fast["model"] == t.model2 and "tools" not in fast, f"fast {fast}")
    exreq = [q["body"] for q in t.requests() if "# Your role: explore" in
             fake_provider.system_of(q["body"])]
    t.check(exreq and fake_provider.tool_names(exreq[0]) == ["read", "grep", "glob", "skill"],
            "explore's tools")
    t.check(t.file("shell.txt") is None, "explore ran no command")
    ends = r.all("tool.end", own)
    t.check([e["status"] for e in ends] == ["ok", "ok", "error"], f"statuses {ends}")
    t.check(ends[0]["text"].startswith("Explore report: The tool \"shell\" is not "
                                       "available"), f"explore {ends[0]['text']!r}")
    t.check(report(ends[1]["text"]) == "Fast report.", f"fast {ends[1]}")
    t.check("No role is named \"nope\"" in ends[2]["text"] and "child" not in ends[2],
            f"nope {ends[2]}")


PARK = {"PROSPERO_TEST_MINUTE_MS": "20"}  # parked after 140 ms idle


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
        t.check(te and report(te[0]["text"]) == "The child wrote it.", f"report {te}")
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
        t.check(te and report(te[0]["text"]) == "The child was refused.", f"report {te}")
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
    t.check(report(te["text"]).endswith(f"where this part ends at line {line}.]"),
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
    t.check(report(r.last("tool.end", own).get("text")) == "The child was refused.",
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
    t.check([(e.get("fail"), e.get("repair")) for e in ends] ==
            [(None, "text around the patch, kept lines without their space"),
             ("miss", None), (None, None)],
            f"fail and repair {[(e.get('fail'), e.get('repair')) for e in ends]}")
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
    t.check([e["status"] for e in ends] == ["ok", "ok", "error", "ok", "ok", "ok", "ok", "ok"],
            f"statuses {[(e['status'], e['text'][:80]) for e in ends]}")
    t.check(ends[0]["text"].startswith("[list.txt#") and "     1\tone\n" in ends[0]["text"],
            f"read {ends[0]['text']!r}")
    t.check("does not match list.txt" in ends[2]["text"], f"stale {ends[2]['text']!r}")
    t.check("Tags for edit: [list.txt#" in ends[4]["text"], f"grep {ends[4]['text']!r}")
    t.check(t.file("list.txt") == "zero\none\ntwo\nthree\nend\n", f"file {t.file('list.txt')!r}")
    t.check("[new.txt#" in ends[6]["text"], f"write {ends[6]['text']!r}")
    t.check(t.file("new.txt") == "a\r\nB\r\n", f"written {t.file('new.txt')!r}")
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


# Subagents of a session on another host: new {task}, as that host's
# engine sends it, here alone (tests/remote.py has both engines)

FARPARENT = "20260101-000000-aaaa"


def fartask(**over):
    task = {"parent": FARPARENT, "call": "call_7_0", "depth": 1, "hop": 0,
            "title": "Remote job", "via": "ssh", "prompt": "Do the thing."}
    task.update(over)
    return task


def farreport(h, sid, timeout=20):
    return h.wait(lambda e: e.get("ev") == "task.report" and e.get("sid") == sid,
                  timeout)


@scenario(bound=15)
def task_remote(t):
    """A subagent of a session on another host: its session record names
    the parent and call there, its title is the task's, its prompt says
    where its parent works, its turn is the task's, and its report is a
    durable task.report. The list shows it as a root here, marked
    remote, and its user may put it in Bypass mode."""
    h = t.headless()
    try:
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1,
                   task=fartask(via="wsl", distro="Ubuntu"))
        t.check(r.get("ok") == 1, f"new {r}")
        sid = r["sid"]
        rep = farreport(h, sid)
        t.check(rep["status"] == "ok" and
                rep["text"] == "Remote report (wsl): Do the thing.", f"report {rep}")
        ses = h.all("session", sid)[0]
        want = {"parent": FARPARENT, "call": "call_7_0", "depth": 1, "remote": 1,
                "via": "wsl", "distro": "Ubuntu", "role": "general"}
        t.check(all(ses.get(k) == v for k, v in want.items()), f"session {ses}")
        title = h.all("title", sid)
        t.check(title and title[0]["text"] == "Remote job" and title[0]["by"] == "auto",
                f"title {title}")
        user = h.all("user", sid)
        t.check(user and user[0].get("via") == "task", f"user {user}")
        sysp = t.requests()[0]["body"]["messages"][0]["content"]
        t.check("# Subagent" in sysp and "Windows on this machine" in sysp, "told of WSL")
        tools = [x["function"]["name"] for x in t.requests()[0]["body"]["tools"]]
        t.check("task" in tools and "exit_plan_mode" not in tools, f"tools {tools}")
        h.call("list")
        entry = [e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid]
        t.check(entry and entry[0].get("parent") == FARPARENT and
                entry[0].get("remote") == 1 and "depth" not in entry[0], f"listed {entry}")
        r = h.call("mode", sid=sid, mode="bypass")
        t.check(r.get("ok") == 1 and r.get("mode") == "bypass", f"bypass {r}")

        # Over ssh, the prompt says nothing is shared
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", task=fartask())
        rep = farreport(h, r["sid"])
        t.check(rep["text"] == "Remote report (ssh): Do the thing.", f"report {rep}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_remote_plan(t):
    """A task from a parent in plan mode starts in plan mode, by its
    parent; on this host its user may still take it out."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                     task=fartask(plan=1))["sid"]
        rep = farreport(h, sid)
        t.check(rep["text"] == "Planned.", f"report {rep}")
        t.check(modes(h, sid) == ["plan/parent"], f"modes {modes(h, sid)}")
        r = h.call("mode", sid=sid, mode="default")
        t.check(r.get("ok") == 1 and r.get("mode") == "default", f"off {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_remote_depth(t):
    """A subagent as deep as subagents go has no task tool."""
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                   task=fartask(depth=2))["sid"]
        t.check(farreport(h, a)["text"] == "Deep.", "no task tool at depth 2")
        b = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                   task=fartask(depth=1))["sid"]
        t.check(farreport(h, b)["text"] == "Can delegate.", "a task tool at depth 1")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_remote_errors(t):
    """What new refuses of a task: a role this host lacks (naming those
    it has), a malformed task, a model not listed, a missing directory."""
    h = t.headless()
    try:
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", role="nope",
                   task=fartask())
        t.check(r.get("ok") == 0 and r["err"].startswith('no role is named "nope" there') and
                "explore" in r["err"], f"role {r}")
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                   task=fartask(via="carrier pigeon"))
        t.check(r.get("ok") == 0 and r["err"] == "task.via must be wsl or ssh", f"via {r}")
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                   task=fartask(prompt=""))
        t.check(r.get("ok") == 0 and r["err"] == "task.prompt is empty", f"prompt {r}")
        r = h.call("new", cwd=t.host.join(t.work, "nowhere"), model=t.model,
                   endpoint="fake", task=fartask())
        t.check(r.get("ok") == 0 and "cwd" in r["err"], f"cwd {r}")
        h.call("models")  # this host's listing, which new checks
        r = h.call("new", cwd=t.work, model="fake:nothing:here", endpoint="fake",
                   task=fartask())
        t.check(r.get("ok") == 0 and "no model" in r.get("err", ""), f"model {r}")
        t.check(not h.all("task.report"), "nothing started")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def task_remote_prefer(t):
    """With no model from the call or the role, the parent's model, where
    this host has its endpoint; else this host's default."""
    h = t.headless()
    try:
        a = h.call("new", cwd=t.work, task=fartask(prefer=f"fake/{t.model2}"))["sid"]
        t.check(farreport(h, a)["text"] == "The parent's model.", "the parent's")
        b = h.call("new", cwd=t.work, task=fartask(prefer=f"far/{t.model2}"))["sid"]
        ses = h.all("session", b)[0]
        t.check(farreport(h, b) and ses["model"] != t.model2, f"the default {ses['model']}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15, posix=True)
def task_remote_cancel_idle(t):
    """A subagent that waits for its background job, cancelled: it
    reports cancelled, and its job stops."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                     task=fartask())["sid"]
        h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("job.pid"):
            time.sleep(0.05)
        pids = t.pids("job.pid")
        t.check(pids and not h.all("task.report"), "waiting for its job")
        h.call("cancel", sid=sid)
        rep = farreport(h, sid)
        t.check(rep["status"] == "cancelled" and "Waiting for my job." in rep["text"],
                f"report {rep}")
        t.check(t.gone(pids), "its job stopped")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20, posix=True)
def task_remote_repair(t):
    """The engine killed while a subagent works: opened again, it reports
    that it was interrupted, once."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                     task=fartask())["sid"]
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("fg.pid"):
            time.sleep(0.05)
        pids = t.pids("fg.pid")
    finally:
        t.host.killhard(h.p)
        h.close()
    t.check(pids and t.gone(pids), "its command died with the harness")
    for n in range(2):
        h = t.headless()
        try:
            o = h.call("open", sid=sid, **{"from": 0})
            t.check(o.get("ok") == 1, f"open {o}")
            h.wait(lambda e: e.get("ev") == "session" and e.get("sid") == sid)
            time.sleep(0.3)
            reps = list({e["seq"]: e for e in h.all("task.report", sid)}.values())
            t.check(len(reps) == 1 and reps[0]["status"] == "error" and
                    "stopped while it worked" in reps[0]["text"], f"open {n}: {reps}")
        finally:
            h.call("quit")
            h.close()


@scenario(bound=20, posix=True)
def task_remote_quit(t):
    """The engine quits while a subagent works: it writes no report then,
    since it did not finish, and opened again it reports interrupted."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                     task=fartask())["sid"]
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not t.pids("fg.pid"):
            time.sleep(0.05)
        pids = t.pids("fg.pid")
        h.send("quit")
        rc = h.exit()
    finally:
        h.close()
    t.check(rc == 0 and pids and t.gone(pids), f"quit {rc}, its command stopped")
    t.check(not h.all("task.report"), "no report as it quit")
    h = t.headless()
    try:
        h.call("open", sid=sid, **{"from": 0})
        h.wait(lambda e: e.get("ev") == "task.report" and e.get("sid") == sid)
        reps = list({e["seq"]: e for e in h.all("task.report", sid)}.values())
        t.check(len(reps) == 1 and reps[0]["status"] == "error", f"report {reps}")
    finally:
        h.call("quit")
        h.close()


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
    bigfiles(t, 8, 150)
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


# M6: Anthropic's Messages API, against the fake's Anthropic mode, which
# rejects (400) any request the real API would, and reasoning returned
# other than verbatim and still bound to the conversation before it

AN = {"kind": "anthropic", "key": "env:PROSPERO_TEST_KEY"}
ANENV = {"PROSPERO_TEST_KEY": fake_provider.AN_KEY}


def anblocks(msg, kind):
    return [b for b in msg["content"] if b.get("type") == kind]


def anok(t, r):
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {r.stderr}")


@scenario(conf=AN, bound=5)
def an_plain(t):
    """A plain answer: the key as x-api-key, the API version, a request
    with its breakpoints, and max_tokens within the listing's limit."""
    r = t.run(env=ANENV)
    anok(t, r)
    t.seq(r, ["session", "user", "title", "turn", "attempt", "assistant", "turn.end"])
    t.check(r.text() == "Hello from the Messages API.", f"text {r.text()!r}")
    t.check(not r.all("retry"), "no retries")
    reqs = t.requests()
    t.check(len(reqs) == 1, f"one request: {len(reqs)}")
    h = reqs[0]["headers"]
    t.check(h.get("anthropic-version") == "2023-06-01" and "authorization" not in h,
            f"headers {sorted(h)}")
    body = reqs[0]["body"]
    t.check(body["max_tokens"] == 16000, f"max_tokens {body['max_tokens']}")
    t.check(body["system"][0].get("cache_control") == {"type": "ephemeral"},
            "a breakpoint after the system prompt")
    tools = body["tools"]
    t.check("cache_control" in tools[-1] and
            not any("cache_control" in x for x in tools[:-1]),
            "a breakpoint after the tools")
    t.check(all(x.get("eager_input_streaming") is True and "input_schema" in x
                for x in tools), "tools as the API takes them")
    t.check(body["tool_choice"] == {"type": "auto"}, f"tool_choice {body['tool_choice']}")
    t.check(body["thinking"] == {"type": "adaptive", "display": "summarized"},
            f"thinking {body['thinking']}")
    t.check(body["messages"] == [{"role": "user", "content": [
        {"type": "text", "text": "go", "cache_control": {"type": "ephemeral"}}]}],
            f"messages {body['messages']}")
    with fake_provider.STATE.lock:
        listings = list(fake_provider.STATE.listings)
    t.check(any(x["query"] == "limit=1000" and x["key"] for x in listings),
            f"the listing asked for: {listings}")
    u = r.last("assistant").get("usage", {})
    t.check(u.get("cached") == 0 and u.get("written", 0) > 0 and
            u.get("in", 0) > u["written"] and u.get("out") == 10, f"usage {u}")


@scenario(conf=AN, bound=10)
def an_attach(t):
    """Images attached to a prompt go before its text as image blocks, of
    their media type, the breakpoint on the text; images alone, the
    breakpoint on the last. The fake checks each as the API would."""
    h = t.headless(env=ANENV)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a = attached(t, h, sid, PNG)
        b = attached(t, h, sid, JPG)
        h.call("prompt", sid=sid, text="what are these?", images=[a, b])
        t.check(ended(h, sid, 1)["why"] == "done", "answered")
        t.check(lastsaid(h, sid) == "I see 2 images.", f"said: {lastsaid(h, sid)}")
        msg = t.requests()[-1]["body"]["messages"][0]
        t.check([x["type"] for x in msg["content"]] == ["image", "image", "text"] and
                msg["content"][1]["source"] == {"type": "base64", "media_type": "image/jpeg",
                                                "data": base64.b64encode(JPG).decode()} and
                "cache_control" in msg["content"][2], f"the message: {str(msg)[:400]}")
        h.call("prompt", sid=sid, text=" ", images=[a])
        t.check(ended(h, sid, 2)["why"] == "done", "answered again")
        msg = t.requests()[-1]["body"]["messages"][-1]
        t.check([x["type"] for x in msg["content"]] == ["image"] and
                "cache_control" in msg["content"][0], f"alone: {str(msg)[:400]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=AN, bound=10)
def an_tools(t):
    """read, edit, shell: each step's reasoning goes back verbatim with its
    signature (redacted reasoning too), each result as a tool_result, and
    every request extends the one before."""
    t.put("hello.txt", "hello world\n")
    r = t.run(env=ANENV)
    anok(t, r)
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok", "ok", "ok"],
            f"statuses {[e['status'] for e in ends]}")
    t.check(t.file("hello.txt") == "hello there\n", "the edit landed")
    t.check(r.text() == "On it.Done: hello.txt now greets there.", f"text {r.text()!r}")
    reqs = [x["body"] for x in t.requests()]
    t.check(len(reqs) == 4, f"four requests: {len(reqs)}")
    last = reqs[-1]["messages"]
    t.check([m["role"] for m in last] == ["user", "assistant"] * 3 + ["user"],
            f"roles {[m['role'] for m in last]}")
    asst = [m for m in last if m["role"] == "assistant"]
    kinds = [[b["type"] for b in m["content"]] for m in asst]
    t.check(kinds == [["thinking", "text", "tool_use"],
                      ["thinking", "redacted_thinking", "tool_use"],
                      ["thinking", "tool_use"]], f"blocks {kinds}")
    recs = r.all("assistant")
    t.check(recs[0]["blocks"][0].get("sig") == asst[0]["content"][0]["signature"],
            "the record keeps the signature")
    t.check(any(b["t"] == "redacted" and b.get("data", "").startswith("RedactedFake")
                for b in recs[1]["blocks"]), f"redacted kept: {recs[1]['blocks']}")
    res = anblocks(last[2], "tool_result")
    t.check(len(res) == 1 and res[0]["tool_use_id"] == asst[0]["content"][2]["id"] and
            "is_error" not in res[0], f"result {res}")
    for a, b in zip(reqs, reqs[1:]):
        pa = fake_provider.plain(a["messages"])
        pb = fake_provider.plain(b["messages"])
        t.check(pb[:len(pa)] == pa, "each request extends the previous one")


@scenario(conf=AN, bound=10)
def an_think(t):
    """Reasoning with its signature goes back verbatim next turn, and from
    the log after the session is opened again."""
    t.put("a.txt", "hello\n")
    r = t.run(env=ANENV)
    anok(t, r)
    t.check(r.text() == "It says hello.", f"text {r.text()!r}")
    reqs = t.requests()
    t.check(len(reqs) == 2, f"two requests: {len(reqs)}")
    sent = reqs[1]["body"]["messages"][1]["content"][0]
    rec = r.all("assistant")[0]["blocks"][0]
    t.check(sent["type"] == "thinking" and
            sent["thinking"] == "The file will say what to do." and
            sent["signature"] == rec.get("sig"), f"sent {sent}, recorded {rec}")
    r2 = t.run("again", resume=r.events[0]["sid"], env=ANENV)
    anok(t, r2)
    again = t.requests()[2]["body"]["messages"]
    t.check(fake_provider.plain(again[1]["content"][0]) == sent,
            f"replayed from the log: {again[1]['content'][0]}")


@scenario(conf=AN, bound=12)
def an_stall(t):
    """Headers, then not one event: the idle watchdog retries."""
    r = t.run(env=ANENV)
    anok(t, r)
    rs = r.all("retry")
    t.check(len(rs) == 1 and rs[0]["class"] == "transient", f"retries {rs}")
    t.check(r.text() == "Answered after a stall.", f"text {r.text()!r}")


@scenario(conf=AN, bound=8)
def an_529(t):
    """overloaded_error, a 529, backs off and retries."""
    r = t.run(env=ANENV)
    anok(t, r)
    rs = r.all("retry")
    t.check(len(rs) == 1 and rs[0]["status"] == 529 and rs[0]["class"] == "transient"
            and "overloaded_error" in rs[0]["msg"], f"retries {rs}")
    t.check(r.text() == "Not overloaded now.", f"text {r.text()!r}")


@scenario(conf=AN, bound=8)
def an_429(t):
    """rate_limit_error with retry-after: 2 waits as asked."""
    r = t.run(env=ANENV)
    anok(t, r)
    rs = r.all("retry")
    t.check(len(rs) == 1 and rs[0]["status"] == 429 and rs[0]["delay_ms"] >= 2000,
            f"retries {rs}")
    reqs = t.requests()
    t.check(reqs[1]["t"] - reqs[0]["t"] >= 1.9, "waited")
    t.check(r.text() == "Waited as asked.", f"text {r.text()!r}")


@scenario(conf=AN, bound=8)
def an_midstream(t):
    """An error event after text: the text is kept and the agent asked to
    continue; the reasoning, cut off before its signature, stays out."""
    r = t.run(env=ANENV)
    anok(t, r)
    t.check(r.text() == "The answer is forty-two.", f"text {r.text()!r}")
    t.check([n["kind"] for n in r.all("nudge")] == ["continue"], f"nudges {r.all('nudge')}")
    t.check(r.all("assistant")[0]["finish"] == "interrupted", "committed as interrupted")
    second = t.requests()[1]["body"]["messages"]
    t.check([b["type"] for b in second[1]["content"]] == ["text"] and
            second[1]["content"][0]["text"] == "The answer is",
            f"the partial went back without its reasoning: {second[1]}")


@scenario(conf=AN, bound=8)
def an_max_tokens(t):
    """The output limit cuts a call's input short: the call fails, saying
    why, and goes back as an error."""
    r = t.run(env=ANENV)
    anok(t, r)
    end = r.all("tool.end")
    t.check(len(end) == 1 and end[0]["status"] == "error" and
            "length limit" in end[0]["text"], f"tool.end {end}")
    t.check(r.all("assistant")[0]["finish"] == "length", "finish length")
    t.check(t.file("big.txt") is None, "nothing written")
    res = anblocks(t.requests()[1]["body"]["messages"][2], "tool_result")
    t.check(len(res) == 1 and res[0].get("is_error") is True, f"result {res}")
    t.check(r.text() == "Wrote less at once.", f"text {r.text()!r}")


@scenario(conf=AN, bound=8)
def an_pause(t):
    """pause_turn: the paused response goes back last, as it is, and the
    turn goes on with no nudge."""
    r = t.run(env=ANENV)
    anok(t, r)
    t.check(r.text() == "Working on it, and done.", f"text {r.text()!r}")
    t.check(not r.all("nudge"), f"no nudges: {r.all('nudge')}")
    t.check([e["finish"] for e in r.all("assistant")] == ["pause_turn", "stop"],
            f"finishes {[e['finish'] for e in r.all('assistant')]}")
    reqs = t.requests()
    t.check(len(reqs) == 2, f"two requests: {len(reqs)}")
    last = reqs[1]["body"]["messages"][-1]
    t.check(last["role"] == "assistant" and
            last["content"][0]["text"] == "Working on it, ", f"last {last}")
    t.check(len(r.all("turn.end")) == 1, "one turn")


@scenario(conf=AN, bound=8)
def an_refusal(t):
    """refusal ends the turn with a note that says why. The declined
    response is shown and logged, but never sent back, its reasoning
    neither: the next turn goes on from the prompt before it."""
    r = t.run(env=ANENV)
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    end = r.last("turn.end")
    t.check(end.get("why") == "error" and "refusal" in end.get("err", ""),
            f"turn end {end}")
    notes = [n["text"] for n in r.all("note") if n.get("level") == "error"]
    t.check(any("category cyber: This request was declined." in n for n in notes),
            f"notes {notes}")
    rec = r.all("assistant")[0]
    t.check(rec["finish"] == "refusal" and
            [b["t"] for b in rec["blocks"]] == ["think", "text"], f"recorded {rec}")
    t.check(len(t.requests()) == 1, "no retry")
    r2 = t.run("next", resume=r.events[0]["sid"], env=ANENV)
    anok(t, r2)
    msgs = t.requests()[1]["body"]["messages"]
    t.check([m["role"] for m in msgs] == ["user"] and
            [b.get("text") for b in msgs[0]["content"]] == ["go", "next"],
            f"sent again: {msgs}")
    t.check(r2.text() == "Another time, then.", f"text {r2.text()!r}")


@scenario(conf=AN, bound=20, alone=True)
def an_overflow(t):
    """prompt is too long: compact (the summary over the Messages API too),
    then retry the step, with no reasoning from before the compaction."""
    bigfiles(t, 4, 100)
    r = t.run(env=ANENV)
    anok(t, r)
    cps = r.all("compact")
    t.check(cps and cps[-1]["why"] == "overflow", f"compacted for it: {cps}")
    reqs = t.requests()
    t.check(len(reqs) == 6, f"the step retried once: {len(reqs)}")
    post = reqs[5]["body"]["messages"]
    t.check("<conversation-checkpoint>" in post[0]["content"][0]["text"],
            "with a checkpoint")
    t.check(not any(b["type"] in ("thinking", "redacted_thinking")
                    for m in post for b in m["content"]),
            "no reasoning from before the compaction")
    sreq = t.compactions()
    t.check(sreq and fake_provider.is_compaction(sreq[0]["body"]) and
            "tools" not in sreq[0]["body"], "the summary over the Messages API")
    t.check(r.text() == "Fits now.", f"text {r.text()!r}")


@scenario(conf=AN, bound=8)
def an_rebind(t):
    """Reasoning the API finds bound to another conversation: the step
    goes again once, with no reasoning sent back."""
    t.put("a.txt", "hello\n")
    r = t.run(env=ANENV)
    anok(t, r)
    rs = r.all("retry")
    t.check(len(rs) == 1 and rs[0]["class"] == "deterministic" and
            "bound to a different conversation" in rs[0]["msg"], f"retries {rs}")
    reqs = [x["body"]["messages"] for x in t.requests()]
    t.check(len(reqs) == 3, f"three requests: {len(reqs)}")
    t.check([b["type"] for b in reqs[1][1]["content"]] == ["thinking", "tool_use"] and
            [b["type"] for b in reqs[2][1]["content"]] == ["tool_use"],
            "sent with its reasoning, then without")
    t.check(r.text() == "Read it.", f"text {r.text()!r}")


@scenario(conf=dict(AN, _listing={"max_input_tokens": 100000}), bound=20,
          alone=True)
def an_prune(t):
    """Old tool outputs elided in place: reasoning from before goes no
    more, and reasoning after it goes back again."""
    bigfiles(t, 10, 512)
    r = t.run(env=ANENV)
    anok(t, r)
    cps = r.all("compact")
    t.check(len(cps) == 1 and cps[0]["kind"] == "prune", f"one prune: {cps}")
    reqs = [x["body"]["messages"] for x in t.requests()]
    k = next(i for i, m in enumerate(reqs)
             if any(str(b.get("content", "")).startswith("[output elided")
                    for x in m for b in x["content"]))
    count = [sum(b["type"] == "thinking" for x in m for b in x["content"]) for m in reqs]
    t.check(count[k] == 0 and count[k - 1] == k - 1 and count[-1] == len(reqs) - 1 - k,
            f"reasoning sent: {count}, pruned at {k}")
    t.check(r.text() == "Pruned and done.", f"text {r.text()!r}")


@scenario(conf=AN, bound=10)
def an_cache(t):
    """Cache reads grow turn over turn: each request's prefix is the last
    one's, with a breakpoint where that one wrote."""
    bigfiles(t, 4, 50)
    r = t.run(env=ANENV)
    anok(t, r)
    us = [e["usage"] for e in r.all("assistant")]
    cached = [u["cached"] for u in us]
    t.check(len(us) == 5 and cached[1] > 0 and
            all(b > a for a, b in zip(cached[1:], cached[2:])),
            f"cache reads grow: {cached}")
    t.check(all(u.get("written", 0) > 0 for u in us), f"written: {us}")
    t.check(all(u["in"] > u["cached"] + u.get("written", 0) for u in us), f"in: {us}")


@scenario(conf=dict(AN, key="file:~/fake.key"), bound=8)
def an_keyfile(t):
    """A key read from a file where the engine runs (key = file:PATH),
    as on a WSL or ssh host whose environment cannot be set, trimmed
    of its newline; a missing file is named in the hint."""
    r = t.run()
    end = r.last("turn.end")
    t.check(r.rc == 1 and "fake.key, which cannot be read where the engine runs"
            in end.get("err", ""), f"missing: {end}")
    keyfile = t.host.join(t.home, "fake.key")
    t.host.write(keyfile, fake_provider.AN_KEY + "\n")
    r = t.run()
    anok(t, r)
    t.check(r.text() == "Keyed from a file.", f"text {r.text()!r}")
    t.check(fake_provider.AN_KEY not in json.dumps(r.events) and
            fake_provider.AN_KEY not in r.stderr, "the key is never shown")


@scenario(conf=AN, bound=8)
def an_auth(t):
    """A key refused is fatal at once, with a hint naming the setting and
    the variable, never the key."""
    r = t.run(env={"PROSPERO_TEST_KEY": "wrong-key-123"})
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    end = r.last("turn.end")
    err = end.get("err", "")
    t.check(end.get("why") == "error" and err.startswith("fatal: HTTP 401") and
            "invalid x-api-key" in err and
            "[endpoint fake] key = env:PROSPERO_TEST_KEY, which is set" in err,
            f"turn end {end}")
    t.check("wrong-key-123" not in json.dumps(r.events) and
            "wrong-key-123" not in r.stderr, "the key is never shown")
    t.check(not r.all("retry"), "no retries")
    r = t.run()
    end = r.last("turn.end")
    t.check(r.rc == 1 and "which is not set where the engine runs" in end.get("err", ""),
            f"unset: {end}")


@scenario(conf=AN, bound=8)
def an_env(t):
    """A command the agent runs never has the variable that the endpoint's
    key comes from (key = env:NAME), so neither its output nor any
    request holds the key; other variables pass."""
    r = t.run(env=dict(ANENV, PROSPERO_TEST_OTHER="passes-through"))
    anok(t, r)
    ends = r.all("tool.end")
    t.check(len(ends) == 1 and ends[0]["status"] == "ok" and
            "PROSPERO_TEST_OTHER=passes-through" in ends[0]["text"], f"env ran: {ends}")
    t.check("PROSPERO_TEST_KEY" not in ends[0]["text"] and
            fake_provider.AN_KEY not in json.dumps(r.events), "the key is not in the output")
    reqs = t.requests()
    t.check(len(reqs) == 2 and
            not any(fake_provider.AN_KEY in json.dumps(x["body"]) for x in reqs),
            "nor in a request")


@scenario(conf=AN, bound=20, alone=True)
def an_compact_refusal(t):
    """The summarizer declines part way through: its partial summary is
    never used, and nothing is asked again, which would decline alike;
    the turn ends saying why."""
    bigfiles(t, 4, 100)
    r = t.run(env=ANENV)
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    end = r.last("turn.end")
    t.check(end.get("why") == "error" and "summarizer declined (refusal)" in end.get("err", ""),
            f"turn end {end}")
    t.check(not r.all("compact") and not r.all("retry"),
            f"no summary, no retry: {r.all('compact')} {r.all('retry')}")
    t.check(len(t.compactions()) == 1 and len(t.requests()) == 5,
            f"{len(t.compactions())} summaries, {len(t.requests())} requests")
    notes = [n["text"] for n in r.all("note") if n.get("level") == "error"]
    t.check(any("category cyber: This request was declined." in n and "compactor" in n
                for n in notes), f"notes {notes}")


@scenario(conf=AN, bound=20, alone=True)
def an_compact_length(t):
    """A summary cut off at the output limit is never used: the turn ends
    saying so."""
    bigfiles(t, 4, 100)
    r = t.run(env=ANENV)
    t.check(r.rc == 1, f"exit {r.rc}: {r.stderr}")
    end = r.last("turn.end")
    t.check(end.get("why") == "error" and "cut off at the length limit" in end.get("err", ""),
            f"turn end {end}")
    t.check(not r.all("compact") and not r.all("retry") and len(t.compactions()) == 1,
            f"no summary, no retry: {r.all('compact')} {r.all('retry')}")


HIDDEN = dict(AN, _extra="[model {model}]\nthinking_display = omitted\n")


@scenario(conf=HIDDEN, bound=15)
def an_hidden(t):
    """Reasoning not shown streams pings alone until its signature: they
    are progress, so 9 s of it pass the 6 s progress watchdog."""
    r = t.run(env=ANENV)
    anok(t, r)
    t.check(not r.all("retry"), f"no retries: {r.all('retry')}")
    t.check(t.requests()[0]["body"]["thinking"] ==
            {"type": "adaptive", "display": "omitted"}, "omitted")
    t.check(r.text() == "Thought it through.", f"text {r.text()!r}")
    blocks = r.all("assistant")[0]["blocks"]
    t.check(blocks[0]["t"] == "think" and blocks[0].get("sig"), f"blocks {blocks}")


@scenario(conf=dict(HIDDEN, _extra="[model {model}]\nthinking_display = omitted\n"
                                   "max_tokens = 20\n"), bound=20)
def an_hidden_bound(t):
    """Pings of unseen reasoning are progress only for as long as
    max_tokens would take at 10 tokens a second, here 2 s: then the
    progress watchdog ends a stream that pings forever."""
    r = t.run(env=ANENV)
    anok(t, r)
    rs = r.all("retry")
    t.check(len(rs) == 1 and rs[0]["class"] == "transient" and
            "no progress for 6 s" in rs[0]["msg"], f"retries {rs}")
    reqs = t.requests()
    took = reqs[1]["t"] - reqs[0]["t"]
    t.check(len(reqs) == 2 and 7 < took < 14, f"{len(reqs)} requests, {took:.1f} s apart")
    t.check(r.text() == "Answered at last.", f"text {r.text()!r}")


@scenario(conf=dict(AN, _extra="[model {model}]\nthinking = between_tools\n"
                               "thinking_display = omitted\n"), bound=5)
def an_between(t):
    """thinking = between_tools, Sonnet 5.5's off, goes alone, with no
    display, which the API refuses with it."""
    r = t.run(env=ANENV)
    anok(t, r)
    t.check(t.requests()[0]["body"]["thinking"] == {"type": "between_tools"},
            f"thinking {t.requests()[0]['body'].get('thinking')}")


# prospero eval: suites of tasks run against the scripted agents of
# fake_provider.py (sc_eval_*), whose numbers are known

EVAL_SUITE = {
    "suite.ini": "# Every task\ntimeout = 30\nsteps = 10\n",
    "base/greet.txt": "Hello, world!\nBye.\n",
    "count/count.txt": "one\n",
    "typo.ini": "prompt = Change wrold to world in greet.txt.\nfixture = base\n"
                "patch = typo.patch\nexpect = base\n",
    "typo.patch": "*** Begin Patch\n*** Update File: greet.txt\n@@\n-Hello, world!\n"
                  "+Hello, wrold!\n*** End Patch\n",
    "count.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                 "check = grep -qx two count.txt\n",
}


def evalmodel(t, agent):
    """The model of a scripted eval agent, for this scenario alone."""
    name = f"fake:{agent}:{t.nonce}"
    fake_provider.STATE.register(name)
    return name


def evalrun(t, suite, models, extra=(), out="results.jsonl", timeout=60):
    """Run prospero eval on a suite (files written under the scenario's
    directory), returning its exit status, the records of its results
    file, its summary and its progress."""
    root = t.host.join(t.base, suite[0])
    t.host.bulk(root, suite[1])
    path = t.host.join(t.base, out)
    args = ["eval", "-c", t.config, "--state", t.state, "-e", "fake", "-o", path,
            "-m", ",".join(models)] + list(extra) + [root]
    p = t.host.popen(args, t.work, env=t.tmpenv)
    try:
        stdout, stderr = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        stdout, stderr = p.communicate()
        raise Failure(f"eval took over {timeout} s: {stderr[-2000:]}")
    text = t.host.read(path) or ""
    recs = [json.loads(line) for line in text.splitlines() if line.strip()]
    return p.returncode, recs, stdout, stderr


def evalcell(recs, model, fmt):
    return [r for r in recs if r["model"] == model and r["format"] == fmt]


@scenario(bound=40, alone=True)
def eval_formats(t):
    """Four scripted agents in every edit format, twice: the numbers each
    run records, the summary's table, and the format chosen for each, on
    known behavior. A failed edit counts the same in every format, how
    each format's edits typically fail aside. Run again, nothing is made
    twice."""
    good = evalmodel(t, "eval_good")
    flaky = evalmodel(t, "eval_flaky")
    bad = evalmodel(t, "eval_bad")
    badpatch = evalmodel(t, "eval_badpatch")
    suite = ("suite", EVAL_SUITE)
    models = [good, flaky, bad, badpatch]
    rc, recs, out, err = evalrun(t, suite, models, ["-f", "all", "-n", "2", "-j", "3"])
    t.check(rc == 0, f"exit {rc}: {err[-2000:]}")
    t.check(len(recs) == 48 and not any("error" in r for r in recs),
            f"{len(recs)} records: {[r for r in recs if 'error' in r][:2]}")
    t.check({(r["task"], r["rep"]) for r in recs} ==
            {("typo", 1), ("typo", 2), ("count", 1), ("count", 2)}, "every task twice")
    # (model, format): passes, steps, failed edits, of them misses, of 4 runs
    want = {(good, f): (4, 3, 0, 0) for f in ("replace", "patch", "hashline")}
    want.update({(flaky, "hashline"): (4, 3, 0, 0), (flaky, "replace"): (4, 4, 1, 1),
                 (flaky, "patch"): (0, 3, 1, 1)})
    want.update({(bad, f): (0, 3, 1, 1) for f in ("replace", "patch", "hashline")})
    want.update({(badpatch, f): (4, 4, 1, int(f != "patch"))
                 for f in ("replace", "patch", "hashline")})
    for (model, fmt), (passes, steps, fails, miss) in want.items():
        rs = evalcell(recs, model, fmt)
        got = (sum(r["pass"] for r in rs), {r["steps"] for r in rs},
               {r["edit_fails"] for r in rs}, {r["errors"]["miss"] for r in rs},
               {r["errors"]["args"] for r in rs})
        t.check(len(rs) == 4 and got == (passes, {steps}, {fails}, {miss}, {fails - miss}),
                f"{model} {fmt}: {got}, want {(passes, steps, fails, miss)}")
        t.check(all(r["tokens"] == {"in": 100 * steps, "out": 10 * steps,
                                    "cached": 40 * steps, "written": 0} and
                    r["edits"] == steps - 2 and r["reads"] == 1 and
                    r["calls"] == steps - 1 and not r["own"] and r["why"] == "done"
                    for r in rs), f"{model} {fmt} counts: {rs[0]}")
    typo = [r for r in evalcell(recs, good, "patch") if r["task"] == "typo"][0]
    t.check(typo["diff"] == "--- a/greet.txt\n+++ b/greet.txt\n@@ -1,2 +1,2 @@\n"
                            "-Hello, wrold!\n+Hello, world!\n Bye.\n", f"diff {typo['diff']!r}")
    count = [r for r in evalcell(recs, bad, "replace") if r["task"] == "count"][0]
    t.check(count["check_exit"] == 1 and not count["check"] and count["diff"] == "",
            f"failed check: {count}")
    typo = [r for r in evalcell(recs, bad, "hashline") if r["task"] == "typo"][0]
    t.check(typo["expect"] == "greet.txt differs", f"failed expect: {typo}")

    # (Up to the retries, which a slow disk's stalls can cause)
    t.check(f"| {flaky} | replace | 4 | 100.0% (4) | 4.0 | 3.0 | 4 | 0 | 0 | 0 | 400 | 160 "
            f"| 40 |" in out, f"table:\n{out}")
    for fmt, bad_ in (("replace", 0), ("patch", 4), ("hashline", 0)):
        t.check(f"| {badpatch} | {fmt} | 4 | 100.0% (4) | 4.0 | 3.0 | 4 | {bad_} |" in out,
                f"{fmt}'s failed edits alike:\n{out}")
    # Two tasks tell nothing apart beyond chance: each keeps its own
    # format, replace, as the configuration has none
    for m in (good, flaky, badpatch):
        t.check(f"{m}: keep replace, its own format: none did measurably better "
                f"at this many runs\n" in out and f"[model {m}]" not in out,
                f"{m}'s format:\n{out}")
    t.check(f"- hashline and patch: no measurable difference (the pass rates p = 0.5, "
            in out, f"flaky's verdicts:\n{out}")
    t.check(f"{bad}: none, since no run passed" in out, f"bad's format:\n{out}")
    t.check("[48/48]" in err, f"progress: {err[-500:]}")

    # Again: every run is in the results file already
    rc, again, out2, err = evalrun(t, suite, models, ["-f", "all", "-n", "2"])
    t.check(rc == 0 and len(again) == 48 and "0 runs to make, 48 made before" in err,
            f"again: {rc} {len(again)} {err[-500:]}")
    t.check(out2 == out, "the same summary")
    p = t.host.popen(["eval", "--report", "-c", t.config,
                      t.host.join(t.base, "results.jsonl")], t.work, env=t.tmpenv)
    report, _ = p.communicate(timeout=20)
    t.check(p.returncode == 0 and report == out, f"--report:\n{report}")


@scenario(conf={"_extra": "[model fake:eval_limit:{nonce}]\n"
                          "fallback = fake:eval_limit_b:{nonce}\n"}, bound=30)
def eval_limits(t):
    """A run's time and step limits, and what it records of retries,
    denials and repairs on its way. A model out of quota answers nothing,
    so its run could not be made: the model's fallback, which would have
    answered, is never taken in a run."""
    suite = {"typo.ini": "prompt = Change wrold to world in greet.txt.\nfixture = base\n"
                         "patch = typo.patch\nexpect = base\ntimeout = 3\nsteps = 6\n",
             "base/greet.txt": EVAL_SUITE["base/greet.txt"],
             "typo.patch": EVAL_SUITE["typo.patch"]}
    mixed = evalmodel(t, "eval_mixed")
    slow = evalmodel(t, "eval_slow")
    busy = evalmodel(t, "eval_busy")
    limit = evalmodel(t, "eval_limit")
    fallback = evalmodel(t, "eval_limit_b")
    rc, recs, out, err = evalrun(t, ("lim", suite), [mixed, slow, busy, limit],
                                 ["-f", "replace", "-j", "4"])
    t.check(rc == 0 and len(recs) == 4, f"exit {rc}, {len(recs)} records: {err[-2000:]}")
    by = {r["model"]: r for r in recs}
    m = by[mixed]
    t.check(m["pass"] and m["why"] == "done" and m["steps"] == 4 and m["retries"] == 1 and
            m["denied"] == 1 and m["repairs"] == 1 and m["reads"] == 2 and
            m["repaired"] == {"arguments were a JSON string holding the object": 1,
                              "matched ignoring trailing whitespace": 1} and
            m["retried"] == {"transient": 1}, f"mixed: {m}")
    s = by[slow]
    t.check(not s["pass"] and s["why"] == "timeout" and s["steps"] == 0 and
            "longer than its limit of 3 s" in s["err"] and 2900 <= s["wall_ms"] < 6000,
            f"slow: {s}")
    q = by[limit]
    t.check(q.get("error", "").startswith("the model never answered: limit: HTTP 429") and
            len(t.requests(limit)) == 1 and not t.requests(fallback),
            f"limit: {q}, {len(t.requests(limit))} and {len(t.requests(fallback))} requests")
    t.check("1 run could not be made" in out, f"summary:\n{out}")
    b = by[busy]
    t.check(not b["pass"] and b["why"] == "budget" and b["steps"] == 6 and
            b["err"] == "the turn reached its limit of 6 model requests", f"busy: {b}")
    t.check(f"- {mixed} replace: repaired 1 x arguments were a JSON string holding "
            "the object, 1 x matched ignoring trailing whitespace; retried 1 x "
            "transient\n" in out, f"repairs:\n{out}")


@scenario(conf={"max_concurrent": 1}, bound=40, alone=True)
def eval_queue(t):
    """Runs at once share an endpoint that takes one request at a time:
    the waits are not the runs' time, which would pass their limit. Each
    run takes three responses of 1.5 s, and four take 18 s in all, but
    their limit of 12 s leaves room for a slow disk's syncs (a Raspberry
    Pi's SD card stalls for seconds), which are the run's own time."""
    suite = {"suite.ini": "timeout = 12\n", **{k: v for k, v in EVAL_SUITE.items()
                                              if k != "suite.ini"}}
    patient = evalmodel(t, "eval_patient")
    rc, recs, out, err = evalrun(t, ("queue", suite), [patient],
                                 ["-f", "replace,hashline", "-j", "4"])
    t.check(rc == 0 and len(recs) == 4, f"exit {rc}, {len(recs)} records: {err[-2000:]}")
    t.check(all(r["pass"] for r in recs), f"failed: {[r for r in recs if not r['pass']][:2]}")
    own = [r["wall_ms"] - r["queued_ms"] for r in recs]
    t.check(all(4400 <= ms < 12000 for ms in own), f"own time {own}")
    t.check(sum(r["queued_ms"] for r in recs) > 10000 and
            max(r["wall_ms"] for r in recs) > 12000, f"waits {[r['queued_ms'] for r in recs]}")


@scenario(bound=15)
def new_options(t):
    """new's options for runs that must not depend on the configuration,
    as prospero eval's: an edit format, the tools offered, a limit of
    model requests, and the project's instruction files without the
    user's own; each refused when wrong. close with forget lets a session
    go, and its log opens again."""
    t.put("hello.txt", "hi\n")
    cfg = t.host.join(t.base, "cfg")
    t.host.bulk(cfg, {"prospero/AGENTS.md": "GLOBAL-RULE-7\n"})
    h = t.headless(env={"XDG_CONFIG_HOME": cfg})
    try:
        bad = [({"edit_format": "sloppy"}, "no such edit format: sloppy"),
               ({"tools": "read frobnicate"}, "no such tool: frobnicate"),
               ({"steps": 301}, "steps must be 1 to 300"),
               ({"instructions": "none"}, "instructions must be all or project")]
        for fields, err in bad:
            rep = h.call("new", cwd=t.work, model=t.model, endpoint="fake", **fields)
            t.check(rep.get("ok") == 0 and rep.get("err") == err, f"{fields}: {rep}")

        rep = h.call("new", cwd=t.work, model=t.model, endpoint="fake", prompt="go",
                     edit_format="Hashline", tools="read, edit", steps=1,
                     instructions="project")
        sid = rep.get("sid")
        t.check(rep.get("ok") == 1, f"new {rep}")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end.get("why") == "budget" and
                end.get("err") == "the turn reached its limit of 1 model request",
                f"turn end {end}")
        sess = h.all("session", sid)[0]
        t.check(sess.get("edit_format") == "hashline" and sess.get("steps") == 1 and
                sess.get("tools") == ["read", "edit"], f"session {sess}")
        req = t.requests()[0]["body"]
        t.check(sorted(tool_names_of(req)) == ["edit", "read"] and
                edit_params_of(req) == ["edits", "path", "tag"], f"tools {tool_names_of(req)}")
        t.check("GLOBAL-RULE-7" not in req["messages"][0]["content"],
                "the user's own instructions left out")

        # Without the options: the user's file, every tool, no limit
        rep = h.call("new", cwd=t.work, model=t.model, endpoint="fake", prompt="go")
        sid2 = rep.get("sid")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid2)
        t.check(end.get("why") == "done", f"turn end {end}")
        req = t.requests()[-1]["body"]
        t.check("GLOBAL-RULE-7" in req["messages"][0]["content"] and
                "shell" in tool_names_of(req), "the defaults")

        t.check(h.call("close", sid=sid, forget=1).get("ok") == 1, "closed")
        st = h.call("status")
        t.check(st.get("open") == 1, f"one session left in memory: {st}")
        o = h.call("open", sid=sid, **{"from": 0})
        t.check(o.get("ok") == 1, f"opened again from its log: {o}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def tool_names_of(req):
    return [x.get("function", {}).get("name") for x in req.get("tools", [])]


def edit_params_of(req):
    for x in req.get("tools", []):
        f = x.get("function", {})
        if f.get("name") == "edit":
            return sorted(f.get("parameters", {}).get("properties", {}))
    return []


@scenario(bound=20)
def eval_checks(t):
    """A check that runs a script of the suite's the agent never sees, and
    one past its time limit, killed."""
    suite = {"hidden.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                           "check = sh \"$PROSPERO_EVAL_SUITE/hidden.sh\" $PROSPERO_EVAL_TASK\n",
             "hidden.sh": "test \"$1\" = hidden && grep -qx two count.txt\n",
             "slow.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                         "check = echo checking; sleep 30\ncheck_timeout = 1\n",
             "count/count.txt": "one\n"}
    good = evalmodel(t, "eval_good")
    t0 = time.monotonic()
    rc, recs, out, err = evalrun(t, ("checks", suite), [good])
    t.check(rc == 0 and len(recs) == 2, f"exit {rc}: {recs} {err[-2000:]}")
    by = {r["task"]: r for r in recs}
    t.check(by["hidden"]["pass"] and by["hidden"]["check_exit"] == 0, f"hidden: {by['hidden']}")
    s = by["slow"]
    t.check(not s["pass"] and s["check_exit"] == -1 and s["check_out"] == "checking" and
            time.monotonic() - t0 < 15, f"slow: {s}")


@scenario(bound=20)
def eval_errors(t):
    """Runs that cannot be made are recorded so, and made again next
    time; a suite with a bad task does not run at all."""
    suite = {"setup.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                          "setup = echo broken; exit 3\ncheck = true\n",
             "patch.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                          "patch = bad.patch\ncheck = true\n",
             "bad.patch": "*** Begin Patch\n*** Update File: count.txt\n@@\n-nothing\n+x\n"
                          "*** End Patch\n",
             "out.ini": "prompt = Change one to two in count.txt.\nfixture = count\n"
                        "patch = out.patch\ncheck = true\n",
             "out.patch": "*** Begin Patch\n*** Add File: ../escaped.txt\n+no\n*** End Patch\n",
             "count/count.txt": "one\n"}
    good = evalmodel(t, "eval_good")
    rc, recs, out, err = evalrun(t, ("errs", suite), [good])
    t.check(rc == 0 and len(recs) == 3 and all("error" in r for r in recs),
            f"exit {rc}: {recs} {err[-2000:]}")
    by = {r["task"]: r["error"] for r in recs}
    t.check(by["setup"].startswith("the setup command exited with status 3: broken"),
            f"setup: {by['setup']}")
    t.check("was not applied" in by["patch"], f"patch: {by['patch']}")
    t.check("outside the working directory" in by["out"], f"out: {by['out']}")
    t.check("3 runs could not be made" in out, f"summary:\n{out}")
    rc, recs, out, err = evalrun(t, ("errs", suite), [good])
    t.check(len(recs) == 6 and "3 runs to make" in err, f"made again: {err[-500:]}")

    bad = {"x.ini": "prompt = hi\ntools = read frobnicate\ncheck = true\n"}
    rc, recs, out, err = evalrun(t, ("badsuite", bad), [good], out="bad.jsonl")
    t.check(rc == 2 and "no such tool: frobnicate" in err and not recs, f"{rc}: {err}")

    # One file with both prompt and prompt_file; and a fixture with a .git
    # file, a worktree's, which a copy would share with its repository
    both = {"x.ini": "prompt = hi\nprompt_file = p.md\ncheck = true\n", "p.md": "hi\n"}
    rc, recs, out, err = evalrun(t, ("both", both), [good], out="both.jsonl")
    t.check(rc == 2 and "both prompt and prompt_file are set" in err and not recs,
            f"{rc}: {err}")
    wt = {"x.ini": "prompt = Change one to two in count.txt.\nfixture = count\ncheck = true\n",
          "count/count.txt": "one\n", "count/.git": "gitdir: /elsewhere/.git/worktrees/x\n"}
    rc, recs, out, err = evalrun(t, ("wt", wt), [good], out="wt.jsonl")
    t.check(rc == 2 and ".git is a file, which points at the git directory of another "
            "repository" in err and not recs, f"{rc}: {err}")


@scenario(bound=20)
def eval_prompts(t):
    """A task's own prompt, of either key, over suite.ini's."""
    suite = {"suite.ini": "prompt_file = common.txt\n",
             "common.txt": "Change one to two in count.txt.\n",
             "typo.ini": "prompt = Change wrold to world in greet.txt.\nfixture = base\n"
                         "patch = typo.patch\nexpect = base\n",
             "typo.patch": EVAL_SUITE["typo.patch"],
             "base/greet.txt": EVAL_SUITE["base/greet.txt"],
             "count.ini": "fixture = count\ncheck = grep -qx two count.txt\n",
             "count/count.txt": "one\n"}
    good = evalmodel(t, "eval_good")
    rc, recs, out, err = evalrun(t, ("prompts", suite), [good])
    t.check(rc == 0 and len(recs) == 2 and all(r["pass"] for r in recs),
            f"exit {rc}: {recs} {err[-2000:]}")


@scenario(conf={"_extra": "[endpoint dead]\nurl = http://127.0.0.1:9/\n"
                          "connect_timeout = 1\n"}, bound=40)
def eval_dead(t):
    """An endpoint that never answers measures no model: its runs could
    not be made, and an eval run again makes them. After three in a
    row, a model's runs not started are left out."""
    suite = {"suite.ini": "timeout = 3\n", **{k: v for k, v in EVAL_SUITE.items()
                                              if k != "suite.ini"},
             "count2.ini": EVAL_SUITE["count.ini"], "count3.ini": EVAL_SUITE["count.ini"]}
    auth = evalmodel(t, "eval_auth")
    rc, recs, out, err = evalrun(t, ("dead", suite), [auth], ["-f", "replace", "-j", "1"])
    t.check(rc == 1 and len(recs) == 3 and
            all(r.get("error", "").startswith("the model never answered: fatal: HTTP 401")
                for r in recs), f"exit {rc}: {recs} {err[-2000:]}")
    t.check("3 runs in a row got no answer" in err and "1 run was left out" in err and
            "3 runs could not be made" in out, f"left out: {err[-1000:]}")
    rc, recs, out, err = evalrun(t, ("dead", suite), [auth], ["-f", "replace", "-t", "typo"])
    t.check("1 run to make," in err and len(recs) == 4, f"made again: {err[-500:]}")

    # Refused connections, retried until the time limit, longer here: on
    # Windows each refusal takes 2 s to come, the listing's first
    suite = dict(suite, **{"suite.ini": "timeout = 8\n"})
    rc, recs, out, err = evalrun(t, ("dead2", suite), ["dead/whatever"],
                                 ["-f", "replace", "-t", "typo,count", "-j", "2"],
                                 out="dead.jsonl")
    t.check(rc == 0 and len(recs) == 2 and
            all(r.get("error") == "the model never answered: the run took longer than "
                "its limit of 8 s" for r in recs), f"exit {rc}: {recs} {err[-2000:]}")


@scenario(bound=30)
def eval_mixed(t):
    """Formats are compared on the tasks every one of them ran alone: the
    runs of a format on more tasks than another's are left out of the
    comparison."""
    half = evalmodel(t, "eval_half")
    suite = ("mixed", EVAL_SUITE)
    rc, recs, out, err = evalrun(t, suite, [half], ["-f", "replace"])
    t.check(rc == 0 and [r["pass"] for r in sorted(recs, key=lambda r: r["task"])] ==
            [False, True], f"replace: {recs}")
    rc, recs, out, err = evalrun(t, suite, [half], ["-f", "patch", "-t", "typo"])
    t.check(rc == 0 and len(recs) == 3, f"patch: {rc} {recs}")
    t.check(f"{half}: keep replace, its own format" in out and
            "- patch: 1/1 passed" in out and "- replace: 1/1 passed" in out and
            "1 run that not every format made left out" in out, f"summary:\n{out}")


@scenario(conf={"_extra": "[model {model}]\nedit_format = hashline\n"}, bound=30)
def eval_own(t):
    """Formats that tie keep the model's own format, which its own runs
    show: no change to make."""
    suite = ("own", EVAL_SUITE)
    rc, recs, out, err = evalrun(t, suite, [t.model])
    t.check(rc == 0 and len(recs) == 2 and all(r["own"] and r["format"] == "hashline"
                                               for r in recs), f"own: {rc} {recs}")
    rc, recs, out, err = evalrun(t, suite, [t.model], ["-f", "replace"])
    t.check(rc == 0 and len(recs) == 4, f"replace: {rc} {recs}")
    t.check(f"{t.model}: keep hashline, its own format: none did measurably better" in out
            and "[model" not in out, f"summary:\n{out}")


# M8: session operations, on a session open here, parked, closed, or
# held by another process

def listentry(h, sid):
    h.call("list")  # its reply follows the list
    return next((e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid), None)


def sidecar(t, sid, name):
    """A file beside a session's log, None if there is none."""
    return t.host.read(t.host.join(t.state, "sessions", sid, name))


def logged(t, sid):
    data = t.host.read(t.host.join(t.state, "sessions", sid, "events.jsonl")) or ""
    return [json.loads(x) for x in data.splitlines() if x.strip()]


def ended(h, sid, turn):
    return h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
                  e.get("turn") == turn)


@scenario(bound=15)
def session_ops(t):
    """rename, archive and group: a title from the user, also while a turn
    runs, and sidecars the list shows, written by whoever holds the
    session, refused while another process does, and on a closed session
    for the moment, which stays closed."""
    a = t.headless()
    b = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a.send("prompt", sid=sid, text="first question")
        ended(a, sid, 1)
        auto = a.all("title", sid)
        t.check(len(auto) == 1 and auto[0].get("by") == "auto", f"auto title {auto}")
        r = a.call("rename", sid=sid, title="  My \t new\ntitle  ")
        t.check(r.get("ok") == 1 and r.get("title") == "My new title", f"rename {r}")
        last = a.all("title", sid)[-1]
        t.check(last["text"] == "My new title" and last.get("by") == "user" and
                last.get("seq"), f"title record {last}")
        t.check(sidecar(t, sid, "title") == "My new title", "the title's sidecar")
        r = a.call("rename", sid=sid, title=" \n\t ")
        t.check(r.get("ok") == 0 and r.get("err") == "the title is empty", f"blank {r}")
        r = a.call("rename", sid=sid, title="x" * 200)
        t.check(r.get("ok") == 1 and r["title"] == "x" * 120 + "…", f"long {r}")

        # While a turn runs
        a.send("prompt", sid=sid, text="slow")
        a.wait(lambda e: e.get("ev") == "attempt" and e.get("sid") == sid and
               e.get("turn") == 2)
        r = a.call("rename", sid=sid, title="Renamed while busy")
        t.check(r.get("ok") == 1, f"rename while busy {r}")
        t.check(ended(a, sid, 2)["why"] == "done", "the turn goes on")

        # Another process: refused, as this one holds the session
        b = t.headless()
        for cmd, f in (("rename", {"title": "theirs"}), ("archive", {"on": 1}),
                       ("group", {"name": "g"})):
            r = b.call(cmd, sid=sid, **f)
            t.check(r.get("ok") == 0 and r.get("err") == "open in another process",
                    f"{cmd} elsewhere: {r}")
        t.check(a.call("archive", sid=sid, on=1).get("ok") == 1, "archived")
        t.check(a.call("group", sid=sid, name="  Work stuff ").get("ok") == 1, "grouped")
        for name in ("a\nb", "x" * 121, 5):
            r = a.call("group", sid=sid, name=name)
            t.check(r.get("ok") == 0 and r.get("err"), f"group {name!r}: {r}")
        en = listentry(b, sid)
        t.check(en.get("archived") == 1 and en.get("group") == "Work stuff" and
                en["title"] == "Renamed while busy" and en["locked"] == 1, f"listed {en}")
        t.check(a.close() == 0, "the first exits")
        a = None

        # Closed: changed for the moment, and it stays closed
        r = b.call("rename", sid=sid, title="Closed rename")
        t.check(r.get("ok") == 1 and r.get("title") == "Closed rename", f"closed rename {r}")
        r = b.call("archive", sid=sid, on=0)
        t.check(r.get("ok") == 1, f"unarchived {r}")
        r = b.call("group", sid=sid, name=None)
        t.check(r.get("ok") == 1, f"ungrouped {r}")
        en = listentry(b, sid)
        t.check(en["state"] == "closed" and en["locked"] == 0 and
                en["title"] == "Closed rename" and "archived" not in en and
                "group" not in en, f"listed closed {en}")
        t.check(sidecar(t, sid, "archived") is None and sidecar(t, sid, "group") is None,
                "the sidecars are gone")
        recs = logged(t, sid)
        t.check([e["seq"] for e in recs] == list(range(1, len(recs) + 1)), "the log in order")
        titles = [e for e in recs if e["ev"] == "title"]
        t.check([e.get("by") for e in titles] == ["auto"] + ["user"] * 4 and
                titles[-1]["text"] == "Closed rename", f"titles {titles}")
        r = b.call("open", sid=sid)
        t.check(r.get("ok") == 1 and r.get("readonly") == 0, f"free to open {r}")
    finally:
        if a:
            a.close()
        rc = b.close() if b else 0
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def draft(t):
    """draft: an unsent prompt beside the log, which whoever writes last
    sets, open returns, prompt clears, and which never wakes a parked
    session."""
    a = t.headless(env=PARK)
    b = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        r = a.call("draft", sid=sid, text="half a thought")
        t.check(r.get("ok") == 1 and sidecar(t, sid, "draft") == "half a thought",
                f"drafted {r}")
        deadline = time.monotonic() + 5
        while not listentry(a, sid).get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        r = a.call("draft", sid=sid, text="the whole thought, über alles")
        t.check(r.get("ok") == 1, f"drafted parked {r}")
        t.check(listentry(a, sid).get("parked") == 1, "still parked")

        # Another process sees it, and may write it too
        b = t.headless()
        o = b.call("open", sid=sid)
        t.check(o.get("ok") == 1 and o.get("readonly") == 1 and
                o.get("draft") == "the whole thought, über alles", f"open elsewhere {o}")
        t.check(b.call("draft", sid=sid, text="their version").get("ok") == 1,
                "the last writer wins")
        o = a.call("open", sid=sid)
        t.check(o.get("ok") == 1 and o.get("draft") == "their version", f"open {o}")
        r = a.call("draft", sid=sid, text="x" * ((1 << 20) + 16))
        t.check(r.get("ok") == 0 and "too long" in r.get("err", ""), f"too long {r}")
        t.check(sidecar(t, sid, "draft") == "their version", "unchanged")
        r = a.call("draft", sid=sid, text=5)
        t.check(r.get("ok") == 0, f"not text {r}")

        a.send("prompt", sid=sid, text="sent now")
        ended(a, sid, 1)
        t.check(sidecar(t, sid, "draft") is None, "the prompt clears it")
        o = a.call("open", sid=sid)
        t.check(o.get("ok") == 1 and "draft" not in o, f"none {o}")
        t.check(a.call("draft", sid=sid, text="again").get("ok") == 1, "again")
        t.check(a.call("draft", sid=sid, text="").get("ok") == 1 and
                sidecar(t, sid, "draft") is None, "empty removes it")
        r = a.call("draft", sid="20260101-000000-none", text="x")
        t.check(r.get("ok") == 0 and r.get("err") == "no such session", f"none {r}")
    finally:
        if b:
            b.close()
        rc = a.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=SMALL, bound=15)
def gauge(t):
    """The context's gauge: the window, the size at which it compacts and
    the prompt's size, on attempt and turn.end, for a replay, and on live,
    also for a session opened again before its listing is read."""
    a = t.headless()
    b = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a.send("prompt", sid=sid, text="measure")
        end = ended(a, sid, 1)
        at = a.all("attempt", sid)[-1]
        t.check(at.get("ctx") == 25500 and at.get("compact_at") == 21675 and
                at.get("tokens", 0) > 500, f"attempt {at}")
        t.check(end.get("ctx") == 25500 and end.get("compact_at") == 21675 and
                end.get("tokens", 0) > 500, f"turn.end {end}")
        running = [e for e in a.all("live", sid) if e["state"] in ("waiting", "streaming")]
        t.check(running and all(e.get("tokens") == at["tokens"] and e.get("ctx") == 25500
                                for e in running), f"live {running}")
        t.check(a.close() == 0, "the first exits")
        a = None

        b = t.headless()
        o = b.call("open", sid=sid)
        t.check(o.get("ok") == 1, f"open {o}")
        # live follows open's reply (cmdopen), so wait for it
        live = b.wait(lambda e: e.get("ev") == "live" and e.get("sid") == sid)
        t.check(live.get("ctx") == 25500 and live.get("compact_at") == 21675 and
                live.get("tokens", 0) > 500, f"live after open {live}")
    finally:
        if a:
            a.close()
        rc = b.close() if b else 0
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_extra": "[model {model2}]\nedit_format = patch\n"}, bound=20)
def model_switch(t):
    """model: another model from the next request on, while idle, kept
    in the log and listed; the frozen prefix stays, which the reply's
    note points out; an open naming a model still wins."""
    a = t.headless()
    b = c = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a.send("prompt", sid=sid, text="one")
        ended(a, sid, 1)
        url = a.all("attempt", sid)[-1]["endpoint"]
        r = a.call("model", sid=sid, model=t.model2)
        t.check(r.get("ok") == 1 and r.get("model") == t.model2 and
                r.get("endpoint") == url, f"switched {r}")
        t.check("patch" in r.get("note", "") and "replace" in r.get("note", ""),
                f"the note {r}")
        m = a.all("model", sid)[-1]
        t.check(m.get("model") == t.model2 and m.get("endpoint") == url and m.get("seq"),
                f"model record {m}")
        t.check(sidecar(t, sid, "model") == t.model2, "the model's sidecar")
        t.check(listentry(a, sid)["model"] == t.model2, "listed")
        r = a.call("model", sid=sid, model="m", endpoint="nowhere")
        t.check(r.get("ok") == 0 and "endpoint" in r.get("err", ""), f"no endpoint {r}")

        a.send("prompt", sid=sid, text="two")
        ended(a, sid, 2)
        reqs = t.requests(t.model2)
        t.check(len(reqs) == 1 and reqs[0]["body"]["model"] == t.model2, "sent to it")
        t.check(reqs[0]["body"]["tools"] == t.requests()[0]["body"]["tools"],
                "the tools as they were")
        t.check(a.all("attempt", sid)[-1].get("model") == t.model2, "its attempt")

        # Busy: refused
        a.send("prompt", sid=sid, text="slow")
        a.wait(lambda e: e.get("ev") == "attempt" and e.get("sid") == sid and
               e.get("turn") == 3)
        r = a.call("model", sid=sid, model=t.model)
        t.check(r.get("ok") == 0 and "busy" in r.get("err", ""), f"busy {r}")
        ended(a, sid, 3)
        t.check(a.close() == 0, "the first exits")
        a = None

        # Opened again: still the new model; but an open naming one wins
        b = t.headless()
        b.call("open", sid=sid)
        b.send("prompt", sid=sid, text="three")
        ended(b, sid, 4)
        t.check(len(t.requests(t.model2)) == 3 and len(t.requests()) == 1, "kept")
        t.check(b.close() == 0, "the second exits")
        b = None
        c = t.headless()
        c.call("open", sid=sid, model=t.model)
        c.send("prompt", sid=sid, text="four")
        ended(c, sid, 5)
        t.check(len(t.requests()) == 2, "the open's model")
    finally:
        for h in (a, b, c):
            if h:
                h.close()


@scenario(conf=AN, bound=15)
def an_model_switch(t):
    """A model switch on Anthropic: no reasoning from before it goes to
    the new model, which would refuse another model's signature (400)."""
    t.put("a.txt", "hello\n")
    h = t.headless(env=ANENV)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="first")
        t.check(ended(h, sid, 1)["why"] == "done", "the first turn")
        r = h.call("model", sid=sid, model=t.model2)
        t.check(r.get("ok") == 1 and r.get("model") == t.model2, f"switched {r}")
        h.send("prompt", sid=sid, text="second")
        t.check(ended(h, sid, 2)["why"] == "done", "the second turn")
        t.check(not h.all("retry", sid), f"no retries {h.all('retry', sid)}")
        reqs = [x["body"]["messages"] for x in t.requests(t.model2)]
        t.check(len(reqs) == 2, f"two requests {len(reqs)}")

        def thinking(msgs):
            return [b for m in msgs for b in m["content"]
                    if b.get("type") in ("thinking", "redacted_thinking")]
        old = t.requests()[1]["body"]["messages"]
        t.check(len(thinking(old)) == 1, f"the first model's own went back {old}")
        t.check(not thinking(reqs[0]), f"none from before {reqs[0]}")
        t.check(len(thinking(reqs[1])) == 1 and "model_switch_b" in
                thinking(reqs[1])[0]["thinking"], f"the new model's own {reqs[1]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


BIG = {"_listing": {"meta": {"n_ctx": 262144, "n_ctx_train": 262144}}}


@scenario(conf=BIG, bound=25, alone=True)
def compact_manual(t):
    """compact: refused with too little to summarize, logging nothing;
    then a turn of its own summarizes all but the newest messages, with
    the user's focus, and ends; the next prompt carries the checkpoint."""
    bigfiles(t, 8, 256)
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        n = len(logged(t, sid))
        r = h.call("compact", sid=sid)
        t.check(r.get("ok") == 0 and "too little to compact" in r.get("err", ""),
                f"too little {r}")
        t.check(len(logged(t, sid)) == n, "nothing logged")
        h.send("prompt", sid=sid, text="read them")
        t.check(ended(h, sid, 1)["why"] == "done", "the reads")
        chats = len(t.requests())

        r = h.call("compact", sid=sid, focus="the numbers in f3.txt")
        t.check(r.get("ok") == 1, f"compact {r}")
        end = ended(h, sid, 2)
        t.check(end["why"] == "done", f"ends done {end}")
        turn = [e for e in h.all("turn", sid) if e.get("n") == 2]
        t.check(turn and turn[0].get("via") == "compact", f"its turn {turn}")
        cps = h.all("compact", sid)
        t.check(len(cps) == 1 and cps[0]["kind"] == "summary" and cps[0]["why"] == "manual"
                and cps[0].get("focus") == "the numbers in f3.txt" and
                cps[0]["after"] < cps[0]["before"], f"compacted {cps}")
        sreq = t.compactions()
        t.check(len(sreq) == 1, f"one summary {len(sreq)}")
        user = sreq[0]["body"]["messages"][1]["content"]
        t.check("<focus>\nthe numbers in f3.txt\n</focus>" in user, "the focus asked for")
        t.check(len(t.requests()) == chats, "no chat request in its turn")

        h.send("prompt", sid=sid, text="again")
        ended(h, sid, 3)
        reqs = t.requests()
        t.check(len(reqs) == chats + 1 and checkpointed(chat(reqs[-1])),
                "the next request has the checkpoint")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=BIG, bound=25, alone=True)
def compact_cancel(t):
    """A compaction cancelled ends its turn with nothing compacted; one
    asked for while one runs is refused; a prompt queued meanwhile runs
    after it."""
    bigfiles(t, 8, 256)
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="read them")
        ended(h, sid, 1)
        t.check(h.call("compact", sid=sid).get("ok") == 1, "compacting")
        h.wait(lambda e: e.get("ev") == "live" and e.get("sid") == sid and
               e.get("state") == "compacting")
        r = h.call("compact", sid=sid)
        t.check(r.get("ok") == 0 and "busy" in r.get("err", ""), f"busy {r}")
        h.call("cancel", sid=sid)
        t.check(ended(h, sid, 2)["why"] == "cancel", "cancelled")
        t.check(not h.all("compact", sid), "nothing compacted")

        t.check(h.call("compact", sid=sid).get("ok") == 1, "again")
        h.send("prompt", sid=sid, text="queued")
        t.check(ended(h, sid, 3)["why"] == "done", "compacted")
        end = ended(h, sid, 4)
        t.check(end["why"] == "done", f"then the prompt {end}")
        cps = h.all("compact", sid)
        t.check(len(cps) == 1 and cps[0]["why"] == "manual" and "focus" not in cps[0],
                f"compacted {cps}")
        t.check(checkpointed(chat(t.requests()[-1])), "the prompt after it")
        t.check([e.get("via") for e in h.all("user", sid)][-1] == "queue", "queued")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_extra": "[model {model}]\nsubagent_model = {model2}\n"
                          "[role pinned]\nmodel = fake/{model}\n"}, bound=20)
def task_model(t):
    """A subagent's model: the task's, else its role's (ENDPOINT/MODEL),
    else what the parent's model names for its subagents; one neither
    configured nor listed is a tool error naming the choices, with no
    session made."""
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    own, kids = r.sessions()
    t.check([k["model"] for k in kids] == [t.model2, t.model, t.model2],
            f"their models {[k['model'] for k in kids]}")
    ends = r.all("tool.end", own)
    t.check([e["status"] for e in ends] == ["ok", "ok", "ok", "error"], f"statuses {ends}")
    t.check([report(e["text"]) for e in ends[:3]] ==
            [f"Child on {t.model2}.", f"Child on {t.model}.", f"Child on {t.model2}."],
            f"reports {ends}")
    err = ends[3]["text"]
    t.check('no model "fake:no_such_model"' in err and t.model in err and
            "Give one of these" in err, f"the error {err}")
    t.check(not ends[3].get("child"), "no child")
    if isinstance(t.host, LocalHost):
        made = os.listdir(t.host.join(t.state, "sessions"))
        t.check(len(made) == 4, f"four sessions {made}")


# M8, session operations, part 2

def nextask(h, sid=None, after=0):
    """The first ask, of sid or of any session, logged after seq after."""
    return h.wait(lambda e: e.get("ev") == "ask" and (sid is None or e.get("sid") == sid)
                  and e.get("seq", 0) > after)


def gone(t, path, within=10.0):
    """Whether a path on the host is gone, or goes within the time."""
    deadline = time.monotonic() + within
    while True:
        if isinstance(t.host, LocalHost):
            there = os.path.lexists(path)
        else:
            there = t.host.sh(f"test -e {shlex.quote(path)} && echo y || echo n").strip() == "y"
        if not there:
            return True
        if time.monotonic() > deadline:
            return False
        time.sleep(0.2)


@scenario(bound=20)
def always(t):
    """answer always: the ask offers its directory (a search's, itself),
    the answer and a grant are logged before the call starts, the next
    call there does not ask, nor one in a later process; a read grant
    still asks for writes."""
    for d in ("ext", "ro", "gl"):
        t.host.mkdir(t.host.join(t.base, d))
    t.host.write(t.host.join(t.base, "ro", "x.txt"), "x\n")
    t.host.write(t.host.join(t.base, "gl", "g.txt"), "hello\n")
    a = t.headless()
    b = None
    try:
        sid = a.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        a.send("prompt", sid=sid, text="write")
        ask = nextask(a, sid)
        ext = ask.get("always", "")
        t.check(ext.endswith("/ext") and ask.get("access") == "write" and
                ask.get("path", "").endswith("/ext/a.txt"), f"the ask offers its directory: {ask}")
        r = a.call("answer", sid=sid, req=ask["req"], decision="always", note="go ahead")
        t.check(r.get("ok") == 1, f"answered {r}")
        t.check(ended(a, sid, 1)["why"] == "done", "the turn ends")
        names = [n for n in a.names(sid)
                 if n in ("ask", "answer", "grant", "tool.start", "tool.end")]
        t.check(names == ["ask", "answer", "grant", "tool.start", "tool.end", "tool.start",
                          "tool.end"], f"one ask, then none beside it: {names}")
        ans = a.all("answer", sid)[0]
        t.check(ans["decision"] == "allow" and ans["by"] == "user" and ans.get("always") == 1,
                f"the answer: {ans}")
        g = a.all("grant", sid)[0]
        t.check(g["path"] == ext and g["access"] == "write" and g["by"] == "user" and
                g["req"] == ask["req"] and g["seq"] == ans["seq"] + 1, f"the grant: {g}")
        for f, want in (("a.txt", "a\n"), ("b.txt", "b\n")):
            got = t.host.read(t.host.join(t.base, "ext", f))
            t.check(got == want, f"{f}: {got!r}")

        # A read grant asks for a write
        a.send("prompt", sid=sid, text="read")
        ask = nextask(a, sid, ask["seq"])
        t.check(ask.get("tool") == "read" and ask.get("access") == "read" and
                ask.get("always", "").endswith("/ro"), f"a read's ask: {ask}")
        a.call("answer", sid=sid, req=ask["req"], decision="always")
        ask = nextask(a, sid, ask["seq"])
        t.check(ask.get("tool") == "write" and ask.get("access") == "write", f"asked: {ask}")
        a.call("answer", sid=sid, req=ask["req"], decision="deny")
        ended(a, sid, 2)
        t.check(t.host.read(t.host.join(t.base, "ro", "y.txt")) is None, "not written")
        g = a.all("grant", sid)[-1]
        t.check(g["access"] == "read" and g["path"].endswith("/ro"), f"a read grant: {g}")

        # A search's directory is granted itself
        a.send("prompt", sid=sid, text="search")
        ask = nextask(a, sid, ask["seq"])
        t.check(ask.get("tool") == "glob" and ask.get("always", "").endswith("/gl") and
                ask.get("access") == "read", f"a search's ask: {ask}")
        r = a.call("answer", sid=sid, req="nope", decision="always")
        t.check(r.get("ok") == 0, f"the wrong request: {r}")
        a.call("answer", sid=sid, req=ask["req"], decision="always")
        ended(a, sid, 3)
        te = [e for e in a.all("tool.end", sid)][-2:]
        t.check([e["status"] for e in te] == ["ok", "ok"] and "g.txt" in te[1]["text"],
                f"searched: {te}")
        t.check(len(a.all("ask", sid)) == 4, f"no more asks: {len(a.all('ask', sid))}")
        t.check(a.close() == 0, "the first exits")
        a = None

        # Another process: the grants hold
        b = t.headless()
        o = b.call("open", sid=sid, attended=1)
        t.check(o.get("ok") == 1, f"open {o}")
        last = max(e["seq"] for e in b.all("grant", sid))
        b.send("prompt", sid=sid, text="again")
        t.check(ended(b, sid, 4)["why"] == "done", "the turn ends")
        new = [e for e in b.all("ask", sid) if e["seq"] > last]
        t.check(not new, f"no ask: {new}")
        t.check(t.host.read(t.host.join(t.base, "ext", "c.txt")) == "c\n", "written")
    finally:
        if a:
            a.close()
        rc = b.close() if b else 0
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def always_task(t):
    """A subagent starts with its parent's grants (logged in it, by
    "parent"); one it is given is logged in it and in its root (by
    "subagent"), and holds for the whole tree from then on; a later one
    the parent gets is logged in the subagent, by "parent"."""
    t.host.mkdir(t.host.join(t.base, "ext"))
    t.host.mkdir(t.host.join(t.base, "sub"))
    t.host.write(t.host.join(t.base, "sub", "s.txt"), "s\n")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = nextask(h, sid)
        h.call("answer", sid=sid, req=ask["req"], decision="always")
        kask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") != sid)
        kid = kask["sid"]
        t.check(kask.get("tool") == "read" and kask.get("always", "").endswith("/sub"),
                f"the subagent's ask: {kask}")
        h.call("answer", sid=kid, req=kask["req"], decision="always")
        t.check(ended(h, sid, 1)["why"] == "done", "the parent ends")
        t.check(len(h.all("ask", sid)) == 1 and len(h.all("ask", kid)) == 1,
                f"no more asks: {len(h.all('ask', sid))} {len(h.all('ask', kid))}")
        kg = [(g["by"], g["access"], g["path"].rsplit("/", 1)[-1]) for g in logged(t, kid)
              if g["ev"] == "grant"]
        t.check(kg == [("parent", "write", "ext"), ("user", "read", "sub")],
                f"the subagent's grants: {kg}")
        pg = [g for g in logged(t, sid) if g["ev"] == "grant"]
        t.check([(g["by"], g["access"]) for g in pg] == [("user", "write"),
                                                         ("subagent", "read")] and
                pg[1].get("from") == kid and pg[1].get("req") == kask["req"],
                f"the root's grants: {pg}")
        t.check(t.host.read(t.host.join(t.base, "ext", "child.txt")) == "c\n",
                "the subagent wrote without asking")
        te = h.all("tool.end", sid)
        t.check(te[-1]["status"] == "ok" and te[-1]["text"].rstrip().endswith("s"),
                f"read {te[-1]}")

        # A later grant of the parent's is logged in the live subagent
        t.host.mkdir(t.host.join(t.base, "more"))
        h.send("prompt", sid=sid, text="more")
        ask = nextask(h, sid, ask["seq"])
        h.call("answer", sid=sid, req=ask["req"], decision="always")
        t.check(ended(h, sid, 2)["why"] == "done", "the parent's second turn")
        kg = [(g["by"], g["access"], g["path"].rsplit("/", 1)[-1]) for g in logged(t, kid)
              if g["ev"] == "grant"]
        t.check(kg == [("parent", "write", "ext"), ("user", "read", "sub"),
                       ("parent", "write", "more")], f"the subagent's grants now: {kg}")
        pg = [g["by"] for g in logged(t, sid) if g["ev"] == "grant"]
        t.check(pg == ["user", "subagent", "user"], f"the root's grants now: {pg}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def scratchof(h, sid):
    return h.all("session", sid)[0].get("scratch", "")


@scenario(conf={"_guard": {"temp": "allow"}}, bound=20)
def temp(t):
    """A session's scratch directory, named in its system prompt, made in
    the temporary directory (the run's own TMPDIR here) and the user's
    alone; the temporary directory needs no asking, but a link there
    leading elsewhere does, and /tmp is allowed as it really is."""
    h = t.headless()
    local = isinstance(t.host, LocalHost) and not t.host.windows
    mark = f"/tmp/prospero-scen-{t.nonce}.txt"
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        scratch = scratchof(h, sid)
        base = scratch.rsplit("/", 1)[0]
        t.check(scratch.endswith("/" + sid) and
                base.rsplit("/", 1)[-1] in ("prospero", f"prospero-{os.geteuid() if local else ''}"),
                f"the scratch directory: {scratch}")
        if local:
            st = os.stat(base)
            t.check(st.st_mode & 0o777 == 0o700 and st.st_uid == os.geteuid() and
                    os.path.realpath(base) == os.path.realpath(
                        os.path.join(t.tmp, f"prospero-{os.geteuid()}")) and
                    os.path.isdir(scratch), f"its base: {oct(st.st_mode)}")
        h.send("prompt", sid=sid, text="files")
        t.check(ended(h, sid, 1)["why"] == "done", "the turn ends")
        t.check(not h.all("ask", sid), f"no asks: {h.all('ask', sid)}")
        system = t.requests()[0]["body"]["messages"][0]["content"]
        t.check(f"Put scratch files (notes, test inputs, output you will not keep) in "
                f"{scratch}, which is this session's own" in system, f"the prompt: {system}")
        t.check(t.host.read(t.host.join(scratch, "s.txt")) == "s\n" and
                t.host.read(t.host.join(base.rsplit("/", 1)[0], "t.txt")) == "t\n",
                "written in the scratch and temporary directories")
        if local:
            # To a directory outside every temporary one (the run's own
            # are in /tmp on Linux), which the write is never let into
            os.symlink("/etc", os.path.join(base.rsplit("/", 1)[0], "link"))
            h.send("prompt", sid=sid, text="link")
            ask = nextask(h, sid)
            t.check("symbolic link" in ask.get("detail", ""), f"through the link: {ask}")
            h.call("answer", sid=sid, req=ask["req"], decision="deny")
            ended(h, sid, 2)
            te = h.all("tool.end", sid)[-1]
            t.check(te["status"] == "denied", f"not written: {te}")
            h.send("prompt", sid=sid, text=mark)
            ended(h, sid, 3)
            t.check(len(h.all("ask", sid)) == 1, f"/tmp is allowed: {h.all('ask', sid)}")
            with open(mark) as f:
                t.check(f.read() == "slash tmp\n", "written in /tmp")
    finally:
        if local and os.path.lexists(mark):
            os.unlink(mark)
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15, posix=True)
def temp_planted(t):
    """The scratch directories' base is the user's alone: a link planted
    in its place gets no session's files, and a base open to others is
    closed; a scratch directory purged is made again on reopening."""
    if not isinstance(t.host, LocalHost):
        raise Skip("plants a link here")
    base = os.path.join(t.tmp, f"prospero-{os.geteuid()}")
    elsewhere = os.path.join(t.base, "elsewhere")
    os.mkdir(elsewhere)
    os.symlink(elsewhere, base)
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        t.check(not scratchof(h, sid), f"no scratch directory: {h.all('session', sid)}")
        t.check(not os.listdir(elsewhere), "nothing made through the link")
        os.unlink(base)
        os.mkdir(base, 0o755)
        os.chmod(base, 0o755)
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        scratch = scratchof(h, sid)
        t.check(os.path.isdir(scratch) and os.stat(base).st_mode & 0o777 == 0o700,
                f"made private: {oct(os.stat(base).st_mode)}")
        h.call("close", sid=sid)
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")
    os.rmdir(scratch)
    h = t.headless()
    try:
        t.check(h.call("open", sid=sid).get("ok") == 1, "reopened")
        t.check(os.path.isdir(scratch), "the scratch directory is made again")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_guard": {"temp": "ask"}}, bound=15)
def temp_ask(t):
    """[guard] temp = ask: the temporary directory needs asking, the
    session's scratch directory never."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        scratch = scratchof(h, sid)
        h.send("prompt", sid=sid, text="files")
        ask = nextask(h, sid)
        t.check(ask.get("path", "").endswith("/t.txt"), f"the temporary directory asks: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="deny")
        ended(h, sid, 1)
        t.check(len(h.all("ask", sid)) == 1, "the scratch directory did not ask")
        t.check(t.host.read(t.host.join(scratch, "s.txt")) == "s\n", "written")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def delete(t):
    """delete: a session and its subagent, gone with the scratch directory,
    children first; a link in out/ is removed, its target left alone; a
    subagent cannot be forked."""
    h = t.headless()
    local = isinstance(t.host, LocalHost) and not t.host.windows
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="task")
        ended(h, sid, 1)
        kids = [e["sid"] for e in h.all("session") if e.get("parent") == sid]
        t.check(len(kids) == 1, f"one subagent: {kids}")
        kid = kids[0]
        scratch = scratchof(h, sid)
        t.check(scratchof(h, kid) == scratch, "the subagent shares it")
        t.host.write(t.host.join(scratch, "note.txt"), "n")
        sdir = t.host.join(t.state, "sessions", sid)
        kdir = t.host.join(t.state, "sessions", kid)
        if local:
            target = os.path.join(t.base, "target.txt")
            tdir = os.path.join(t.base, "targetdir")
            os.mkdir(tdir)
            for p in (target, os.path.join(tdir, "inner.txt")):
                with open(p, "w") as f:
                    f.write("keep")
            os.symlink(target, os.path.join(sdir, "out", "link"))
            os.symlink(tdir, os.path.join(sdir, "out", "dirlink"))
            ro = os.path.join(sdir, "out", "ro")
            os.mkdir(ro)
            with open(os.path.join(ro, "f"), "w") as f:
                f.write("r")
            os.chmod(os.path.join(ro, "f"), 0o400)
            os.chmod(ro, 0o500)
        r = h.call("fork", sid=kid)
        t.check(r.get("ok") == 0 and "subagent cannot be forked" in r.get("err", ""),
                f"a subagent's fork: {r}")
        r = h.call("delete", sid=sid)
        t.check(r.get("ok") == 1 and r.get("deleted") == [kid, sid], f"deleted: {r}")
        t.check(gone(t, sdir) and gone(t, kdir), "the directories are gone")
        t.check(gone(t, scratch), "the scratch directory is gone")
        deadline = time.monotonic() + 10
        trash = t.host.join(t.state, "trash")
        while local and os.listdir(trash) and time.monotonic() < deadline:
            time.sleep(0.1)
        if local:
            t.check(not os.listdir(trash), f"the trash empties: {os.listdir(trash)}")
            t.check(open(target).read() == "keep" and
                    open(os.path.join(tdir, "inner.txt")).read() == "keep",
                    "the links' targets are left alone")
        h.call("list")
        listed = [e["sid"] for e in h.all("sessions")[-1]["list"]]
        t.check(sid not in listed and kid not in listed, f"not listed: {listed}")
        r = h.call("delete", sid=sid)
        t.check(r.get("ok") == 0 and r.get("err") == "no such session", f"again: {r}")
        r = h.call("prompt", sid=sid, text="still there?")
        t.check(r.get("ok") == 0, f"gone from memory: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def delete_refused(t):
    """delete is refused while a turn runs, while a job does, and while
    another process has the session open; a parked one may go."""
    a = t.headless(env=PARK)
    b = None
    try:
        s1 = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a.send("prompt", sid=s1, text="slow")
        a.wait(lambda e: e.get("ev") == "attempt" and e.get("sid") == s1)
        r = a.call("delete", sid=s1)
        t.check(r.get("ok") == 0 and r.get("err", "").startswith("busy") and
                not r.get("deleted"), f"busy: {r}")
        ended(a, s1, 1)

        s2 = a.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a.send("prompt", sid=s2, text="job")
        ended(a, s2, 1)
        r = a.call("delete", sid=s2)
        t.check(r.get("ok") == 0 and r.get("err", "").startswith("busy"), f"a job: {r}")
        a.call("job.kill", sid=s2, job=1)
        a.wait(lambda e: e.get("ev") == "job.end" and e.get("sid") == s2)
        deadline = time.monotonic() + 10
        while True:
            r = a.call("delete", sid=s2)
            if r.get("ok") == 1 or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        t.check(r.get("ok") == 1 and r.get("deleted") == [s2], f"once the job ended: {r}")

        b = t.headless()
        s3 = b.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        r = a.call("delete", sid=s3)
        t.check(r.get("ok") == 0 and r.get("err") == "open in another process", f"elsewhere: {r}")
        t.check(gone(t, t.host.join(t.state, "sessions", s3), 0) is False, "still there")

        deadline = time.monotonic() + 10
        while not (listentry(a, s1) or {}).get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        t.check(listentry(a, s1).get("parked") == 1, "parked")
        r = a.call("delete", sid=s1)
        t.check(r.get("ok") == 1 and r.get("deleted") == [s1], f"parked: {r}")
        t.check(gone(t, t.host.join(t.state, "sessions", s1)), "gone")
    finally:
        if b:
            b.close()
        rc = a.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=30)
def fork(t):
    """fork: a copy of the log cut after a turn (with what came between
    turns), under a new sid and directory, with only the files its log
    names, every path to the old directory rewritten; the next prompt in
    the reply; rewind in the fork from its own snapshots; the original
    untouched."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.call("group", sid=sid, name="Forks")
        h.send("prompt", sid=sid, text="one")
        ended(h, sid, 1)
        resumed = h.wait(lambda e: e.get("ev") == "turn" and e.get("sid") == sid and
                         e.get("n") == 2, 15)
        t.check(resumed.get("via") == "job", f"a job's end resumes it: {resumed}")
        ended(h, sid, 2)
        h.send("prompt", sid=sid, text="two")
        h.wait(lambda e: e.get("ev") == "attempt" and e.get("sid") == sid and
               e.get("turn") == 3)
        h.send("prompt", sid=sid, text="three")
        ended(h, sid, 4)
        t.check(t.file("f.txt") == "v2\n", "changed twice")
        src = logged(t, sid)
        old = t.host.join(t.state, "sessions", sid)
        spilled = [e for e in src if e["ev"] == "tool.end" and
                   "the full output is in" in e.get("text", "")]
        t.check(len(spilled) == 1 and old in spilled[0]["text"], f"a spill: {spilled}")

        def forked(turn, nxt):
            args = {} if turn is None else {"turn": turn}
            r = h.call("fork", sid=sid, **args)
            t.check(r.get("ok") == 1 and r.get("from") == sid and r.get("sid") != sid and
                    r.get("next") == nxt and r.get("turn") == (4 if turn is None else turn),
                    f"fork {turn}: {r}")
            recs = logged(t, r["sid"])
            t.check([e["seq"] for e in recs] == list(range(1, len(recs) + 1)) and
                    all(e["sid"] == r["sid"] for e in recs), f"fork {turn}: seq and sid")
            first = recs[0]
            t.check(list(first)[:6] == ["sid", "seq", "ts", "ev", "forked_from", "fork_turn"]
                    and first["forked_from"] == sid and first["fork_turn"] == r["turn"],
                    f"its session record: {first}")
            t.check(recs[-1]["ev"] == "title" and recs[-1]["text"].endswith(" (fork)") and
                    sidecar(t, r["sid"], "title") == recs[-1]["text"] and
                    sidecar(t, r["sid"], "group") == "Forks", f"titled {recs[-1]}")
            return r, recs

        r1, recs = forked(1, None)
        t.check([e["ev"] for e in recs[-3:]] == ["turn.end", "job.end", "title"] and
                not any(e["ev"] == "turn" and e["n"] == 2 for e in recs),
                f"cut after turn 1, its job's end kept: {[e['ev'] for e in recs[-4:]]}")
        r, recs = forked(2, "two")
        t.check(not any(e["ev"] == "user" and e["text"] == "two" for e in recs), "cut")
        r3, recs = forked(3, "three")
        new = t.host.join(t.state, "sessions", r3["sid"])
        text = json.dumps(recs)
        t.check(old not in text and new in text, "the paths name the fork")
        spill = [e for e in recs if e["ev"] == "tool.end" and e.get("spill")]
        snaps = [e["snap"] for e in recs if e["ev"] == "snap"]
        t.check(spill and snaps and all(sidecar(t, r3["sid"], e["spill"]) ==
                                        sidecar(t, sid, e["spill"]) for e in spill) and
                all(sidecar(t, r3["sid"], s) == sidecar(t, sid, s) for s in snaps) and
                any(sidecar(t, r3["sid"], s) for s in snaps),
                f"its files: {[e['spill'] for e in spill]} {snaps}")
        later = [e["spill"] for e in spill if e["spill"] not in json.dumps(logged(t, r1["sid"]))]
        t.check(later and all(sidecar(t, r1["sid"], x) is None for x in later),
                f"only the files its log names: {later}")
        forked(None, None)
        r = h.call("fork", sid=sid, turn=5)
        t.check(r.get("ok") == 0 and r.get("err") == "no turn 5 in this session", f"{r}")
        r = h.call("fork", sid=sid, turn="x")
        t.check(r.get("ok") == 0, f"not a number: {r}")
        t.check(logged(t, sid) == src, "the original untouched")

        # Before each prompt, as the transcript's menu forks (viewforkat):
        # the prompt's record, a title at most, then its turn's record,
        # whose n less one is the fork's turn, a turn begun by a job's end
        # counted, and that prompt comes back as next
        prompts = 0
        for i, e in enumerate(src):
            if e["ev"] != "user" or e.get("via") == "steer":
                continue
            nxt = next(x for x in src[i+1:] if x["ev"] != "title")
            t.check(nxt["ev"] == "turn", f"{e['text']} begins a turn: {nxt}")
            r = h.call("fork", sid=sid, turn=nxt["n"] - 1)
            t.check(r.get("ok") == 1 and r.get("next") == e["text"],
                    f"fork before {e['text']} (turn {nxt['n']}): {r}")
            prompts += 1
        t.check(prompts == 3, f"three prompts: {prompts}")

        # The fork at turn 0: settings, no history
        r0, recs = forked(0, "one")
        t.check(not any(e["ev"] in ("user", "turn", "assistant") for e in recs), "no history")

        # The fork goes on by itself: rewound from its own snapshots
        o = h.call("open", sid=r3["sid"])
        t.check(o.get("ok") == 1 and o.get("readonly") == 0, f"open the fork: {o}")
        rw = h.call("rewind", sid=r3["sid"], to=3)
        t.check(rw.get("ok") == 1 and [f["result"] for f in rw.get("files", [])] == ["restored"],
                f"rewound: {rw}")
        t.check(t.file("f.txt") == "v1\n", "as turn 3 found it")
        h.send("prompt", sid=r3["sid"], text="three")
        t.check(ended(h, r3["sid"], 4)["why"] == "done", "the fork goes on")
        h.call("list")
        listed = {e["sid"]: e for e in h.all("sessions")[-1]["list"]}
        t.check(r3["sid"] in listed and listed[r3["sid"]]["title"].endswith(" (fork)"),
                f"listed: {listed.get(r3['sid'])}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def fork_scratch(t):
    """A fork names its original's scratch directory, so deleting the
    original leaves it while a fork remains; deleting the last of them
    removes it."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="one")
        ended(h, sid, 1)
        scratch = scratchof(h, sid)
        t.check(scratch, "a scratch directory")
        t.host.write(t.host.join(scratch, "note.txt"), "n")
        f1 = h.call("fork", sid=sid)["sid"]
        f2 = h.call("fork", sid=sid, turn=1)["sid"]
        t.check(logged(t, f1)[0].get("scratch") == scratch and
                logged(t, f2)[0].get("scratch") == scratch, "the forks name it")
        r = h.call("delete", sid=sid)
        t.check(r.get("ok") == 1 and r.get("deleted") == [sid], f"deleted: {r}")
        t.check(gone(t, t.host.join(t.state, "sessions", sid)), "the original is gone")
        time.sleep(0.5)  # removed by a thread, if it were
        t.check(t.host.read(t.host.join(scratch, "note.txt")) == "n",
                "its scratch directory stays for the forks")
        h.call("open", sid=f1)
        h.send("prompt", sid=f1, text="two")
        t.check(ended(h, f1, 2)["why"] == "done", "the fork goes on")
        r = h.call("delete", sid=f1)
        t.check(r.get("ok") == 1, f"deleted: {r}")
        time.sleep(0.5)
        t.check(t.host.read(t.host.join(scratch, "note.txt")) == "n",
                "still there for the other fork")
        r = h.call("delete", sid=f2)
        t.check(r.get("ok") == 1, f"deleted: {r}")
        t.check(gone(t, scratch), "gone with the last fork")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


SIDE_RUNNING = "[still running in the main session when this side chat began]"
SIDE_NOTICE = "This is a side chat with the user, forked from the session "


def sidecheck(t, h, sid, side):
    """A side chat's copy, of sid: its session record, the open turn
    closed with the side wording, the notice and title at its end; it
    loads with nothing to repair; its first request has it all, every
    call answered; and it goes on, while sid is left alone. Returns its
    records."""
    recs = logged(t, side)
    first = recs[0]
    t.check(first.get("forked_from") == sid and first.get("side_of") == sid and
            not first.get("parent"), f"its session record: {first}")
    t.check([e["seq"] for e in recs] == list(range(1, len(recs) + 1)) and
            all(e["sid"] == side for e in recs), "seq and sid")
    closed = [e for e in recs if e["ev"] == "tool.end" and e.get("text") == SIDE_RUNNING]
    t.check(len(closed) == 1 and closed[0]["status"] == "cancelled", f"closed: {closed}")
    t.check([e["ev"] for e in recs[-3:]] == ["turn.end", "notice", "title"] and
            recs[-3]["why"] == "side" and recs[-2]["kind"] == "side" and
            recs[-2]["text"].startswith(SIDE_NOTICE) and sid in recs[-2]["text"] and
            "Do not change files unless the user asks you to." in recs[-2]["text"] and
            recs[-1]["text"].endswith(" (side chat)"), f"its end: {recs[-3:]}")
    t.check(listentry(h, side).get("side_of") == sid, f"listed: {listentry(h, side)}")
    t.check(not listentry(h, sid).get("side_of"), "the original is no side chat")

    o = h.call("open", sid=side)
    t.check(o.get("ok") == 1 and o.get("readonly") == 0, f"open: {o}")
    later = logged(t, side)[len(recs):]
    t.check(not [e for e in later if e["ev"] in ("tool.end", "turn.end", "job.end")],
            f"nothing to repair: {later}")
    t.check(not [e for e in h.all("turn", side) if e.get("seq", 0) > len(recs)],
            "idle: no turn of its own")
    h.send("prompt", sid=side, text="aside")
    end = ended(h, side, 2)
    t.check(end["why"] == "done", f"the side chat's turn: {end}")
    req = next((r["body"] for r in t.requests()
                if (fake_provider.last_user_text(r["body"]) or "").endswith("aside")), None)
    t.check(req, "the side chat's request")
    msgs = req["messages"]
    tools = [m for m in msgs if m.get("role") == "tool"]
    t.check(any(SIDE_RUNNING in str(m.get("content")) for m in tools),
            "its request has the closed call")
    t.check(any(m.get("role") == "user" and SIDE_NOTICE in str(m.get("content"))
                for m in msgs), "its request has the notice")
    ids = [c["id"] for m in msgs if m.get("role") == "assistant"
           for c in m.get("tool_calls") or []]
    t.check(ids and sorted(ids) == sorted(m.get("tool_call_id") for m in tools),
            f"every call answered: {ids}")
    return recs


@scenario(bound=25)
def side_chat(t):
    """A side chat (fork {side:1}) of a session running a command: the
    copy has the call closed, then the notice; the original's turn goes
    on to its end unaffected; a plain fork is still refused meanwhile,
    and a side chat takes no turn."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="work")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        r = h.call("fork", sid=sid)
        t.check(r.get("ok") == 0 and r.get("err", "").startswith("busy"), f"plain: {r}")
        r = h.call("fork", sid=sid, side=1, turn=1)
        t.check(r.get("ok") == 0 and "whole session" in r.get("err", ""), f"a turn: {r}")
        r = h.call("fork", sid=sid, side=1)
        side = r.get("sid")
        t.check(r.get("ok") == 1 and r.get("side") == 1 and r.get("from") == sid and
                side and side != sid and "next" not in r, f"side: {r}")
        recs = sidecheck(t, h, sid, side)
        t.check(not any(e["ev"] == "tool.end" and "worked" in e.get("text", "")
                        for e in recs), "the command's own end is not in the copy")

        end = ended(h, sid, 1)
        t.check(end["why"] == "done", f"the original's turn: {end}")
        src = logged(t, sid)
        te = [e for e in src if e["ev"] == "tool.end"]
        t.check(len(te) == 1 and te[0]["status"] == "ok" and "worked" in te[0]["text"],
                f"its command ran to its end: {te}")
        t.check(not any(e["ev"] == "notice" for e in src) and "side_of" not in src[0],
                "the original knows nothing of it")
        t.check(h.all("assistant", sid)[-1]["blocks"][-1].get("text") == "Main done.",
                "and answers")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def side_task(t):
    """A side chat of a session waiting on a subagent: its task call
    closed in the copy; the subagent itself has none, and the original
    goes on."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="work")
        kid = h.wait(lambda e: e.get("ev") == "session" and e.get("parent") == sid)["sid"]
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == kid)
        r = h.call("fork", sid=kid, side=1)
        t.check(r.get("ok") == 0 and "subagent cannot be forked" in r.get("err", ""),
                f"a subagent's: {r}")
        r = h.call("fork", sid=sid, side=1)
        t.check(r.get("ok") == 1 and r.get("side") == 1, f"side: {r}")
        recs = sidecheck(t, h, sid, r["sid"])
        task = [e for e in recs if e["ev"] == "tool.start" and e.get("name") == "task"]
        closed = [e for e in recs if e["ev"] == "tool.end" and e.get("text") == SIDE_RUNNING]
        t.check(task and closed and closed[0]["call"] == task[0]["call"],
                f"the task call is the one closed: {task} {closed}")
        listed = listentry(h, r["sid"])
        t.check(not listed.get("parent"), f"no subagent: {listed}")

        end = ended(h, sid, 1)
        t.check(end["why"] == "done", f"the original's turn: {end}")
        te = h.all("tool.end", sid)
        t.check(len(te) == 1 and te[0]["status"] == "ok" and "Slept." in te[0]["text"],
                f"its subagent reported: {te}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def fork_job_quiet(t):
    """A plain fork of an idle session whose job still runs: the copy
    ends the job as forked, without a notice, and the fork's agent never
    hears of it, while the original's job runs on."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        ended(h, sid, 1)
        r = h.call("fork", sid=sid)
        t.check(r.get("ok") == 1 and "side" not in r, f"fork: {r}")
        recs = logged(t, r["sid"])
        je = [e for e in recs if e["ev"] == "job.end"]
        t.check(len(je) == 1 and je[0]["status"] == "forked" and je[0]["notice"] == 0 and
                not recs[0].get("side_of"), f"its job: {je}")
        h.call("open", sid=r["sid"])
        h.send("prompt", sid=r["sid"], text="next")
        end = ended(h, r["sid"], 2)
        t.check(end["why"] == "done", f"the fork's turn: {end}")
        said = h.all("assistant", r["sid"])[-1]["blocks"][-1].get("text")
        t.check(said == "Not told.", f"no notice of the job: {said}")
        t.check(not [e for e in h.all("notice", r["sid"]) if e.get("kind") == "job"],
                "no job notice")
        t.check(not h.all("job.end", sid), "the original's job runs on")
        h.call("job.kill", sid=sid, job=1)
        h.wait(lambda e: e.get("ev") == "job.end" and e.get("sid") == sid)
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def upload(t, h, data, part=1 << 19):
    """Stage a raw log by upload, part by part, the last with done;
    returns its token."""
    if isinstance(data, str):
        data = data.encode()
    token, off = None, 0
    while True:
        chunk = data[off:off + part]
        last = off + len(chunk) >= len(data)
        args = {"data": base64.b64encode(chunk).decode(), "off": off}
        if token:
            args["token"] = token
        if last:
            args["done"] = 1
        r = h.call("upload", **args)
        t.check(r.get("ok") == 1 and r.get("off") == off + len(chunk) and
                (token is None or r.get("token") == token), f"upload at {off}: {r}")
        token, off = r["token"], r["off"]
        if last:
            return token


def staged(t, token):
    """An upload's staged files there are."""
    tmp = t.host.join(t.state, "tmp")
    return [x for x in (f"import-{token}.part", f"import-{token}.jsonl")
            if t.host.read(t.host.join(tmp, x)) is not None]


def importlog(t, h, data, part=1 << 19, **fields):
    token = upload(t, h, data, part)
    fields.setdefault("cwd", t.work)
    r = h.call("import", token=token, **fields)
    t.check(not staged(t, token), f"the staged log removed: {staged(t, token)}")
    return r


# What an import keeps: the conversation
IMPORTED = {"session", "title", "user", "turn", "attempt", "assistant", "tool.start",
            "tool.end", "retry", "nudge", "turn.end", "note", "compact", "todo",
            "notice"}


@scenario(bound=15)
def new_tilde(t):
    """new and import take a cwd beginning with ~, the home directory of
    the engine's host (HOME, else on Windows USERPROFILE), which hello
    names; ~user is refused, as is ~ with no home known."""
    env = {"HOME": t.base}
    if t.host.windows:
        env["USERPROFILE"] = t.base.replace("/", "\\")
    h = t.headless(env=env)
    try:
        h.call("hello", proto=0, client="scenarios")
        hello = h.all("hello")[0]
        t.check("home" in hello.get("caps", []) and hello.get("home") == t.base,
                f"hello: {hello.get('home')!r}")
        plain = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        want = h.wait(lambda e: e.get("ev") == "session" and e.get("sid") == plain)["cwd"]
        tildes = ["~/work", "~/work/"] + (["~\\work"] if t.host.windows else [])
        for cwd in tildes:
            r = h.call("new", cwd=cwd, model=t.model, endpoint="fake")
            t.check(r.get("ok") == 1, f"new {cwd}: {r}")
            sess = h.wait(lambda e: e.get("ev") == "session" and e.get("sid") == r.get("sid"))
            t.check(sess["cwd"] == want, f"{cwd} is {sess['cwd']}, not {want}")
        r = h.call("new", cwd="~nobody/work", model=t.model, endpoint="fake")
        t.check(r.get("ok") == 0 and "~user is not supported" in r.get("err", ""),
                f"~user: {r}")
        log = t.host.read(t.host.join(t.state, "sessions", plain, "events.jsonl"))
        r = importlog(t, h, log, cwd="~/work", model=t.model, endpoint="fake")
        t.check(r.get("ok") == 1, f"import ~/work: {r}")
        made = [e for e in logged(t, r.get("sid", "")) if e["ev"] == "session"]
        t.check(made and made[0]["cwd"] == want, f"imported to {made[:1]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")

    # No home: refused, saying why
    h = t.headless(env={"HOME": "", "USERPROFILE": ""})
    try:
        h.call("hello", proto=0, client="scenarios")
        t.check("home" not in h.all("hello")[0], f"a home: {h.all('hello')[0]}")
        r = h.call("new", cwd="~/work", model=t.model, endpoint="fake")
        t.check(r.get("ok") == 0 and "no home directory for ~" in r.get("err", ""),
                f"no home: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def rmtree(t, path):
    if isinstance(t.host, LocalHost):
        shutil.rmtree(path)
    else:
        t.host.sh(f"rm -rf {shlex.quote(path)}")


def isdir(t, path):
    if isinstance(t.host, LocalHost):
        return os.path.isdir(path)
    return t.host.sh(f"test -d {shlex.quote(path)} && echo y || echo n").strip() == "y"


@scenario(bound=30)
def nofolder(t):
    """new {nofolder}: a session working in a folder of its own, its
    directory's work, under the state directory, which is in a home that
    is a git repository (dotfiles): the agent writes there without asking;
    the system prompt says it is no project, with the user's instructions
    and skills but not the home's; listed with nofolder; its folder made
    again on opening if gone; a fork has a copy of its own; a delete takes
    it; an import may have one too. new with neither cwd nor nofolder is
    still refused."""
    env = skillhome(t)
    home = t.base  # the state directory is in it, as ~/.local/state is
    env["HOME"] = home
    if t.host.windows:
        env["USERPROFILE"] = home.replace("/", "\\")
    xdg = env["XDG_CONFIG_HOME"]
    t.host.write(t.host.join(xdg, "prospero", "AGENTS.md"), "Global rule: be brief.\n")
    skillput(t, t.host.join(xdg, "prospero", "skills"), "usr",
             "---\ndescription: User skill\n---\nUSR\n")
    t.host.mkdir(t.host.join(home, ".git"))
    t.host.write(t.host.join(home, "AGENTS.md"), "Home project rule.\n")
    skillput(t, t.host.join(t.state, ".claude", "skills"), "proj",
             "---\ndescription: Project skill\n---\nPROJ\n")
    sessions = t.host.join(t.state, "sessions")
    h = t.headless(env=env)
    try:
        h.call("hello", proto=0, client="scenarios")
        t.check("nofolder" in h.all("hello")[0].get("caps", []), "the nofolder cap")
        r = h.call("new", model=t.model, endpoint="fake")
        t.check(r.get("ok") == 0 and r.get("err") == "cwd must name an existing directory",
                f"neither cwd nor nofolder: {r}")
        r = h.call("new", nofolder=1, model=t.model, endpoint="fake", attended=1)
        t.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        rec = h.wait(lambda e: e.get("ev") == "session" and e.get("sid") == sid)
        cwd = rec.get("cwd", "")
        t.check(cwd.replace("\\", "/").endswith(f"/sessions/{sid}/work") and
                rec.get("nofolder") == 1, f"its session record: {rec}")
        work = t.host.join(sessions, sid, "work")

        h.send("prompt", sid=sid, text="write")
        end = ended(h, sid, 1)
        t.check(end["why"] == "done", f"the turn: {end}")
        t.check(not h.all("ask", sid), f"asked: {h.all('ask', sid)}")
        t.check([e["status"] for e in h.all("tool.end", sid)] == ["ok"],
                f"the write: {h.all('tool.end', sid)}")
        t.check(t.host.read(t.host.join(work, "notes.txt")) == "kept\n", "written in its folder")
        system = fake_provider.system_of(t.requests()[0]["body"])
        t.check(f"Working directory: {cwd} (a private folder for this session; "
                "there is no project)" in system, "the prompt says it is no project")
        t.check("Global rule" in system and "Home project rule" not in system,
                "the user's instructions, not the home's")
        t.check("- usr: User skill" in system and "Project skill" not in system,
                "the user's skills, not those up the tree")
        ins = [e for e in h.all("instructions", sid) if e.get("why") == "start"]
        t.check(len(ins) == 1 and len(ins[0]["files"]) == 1, f"instructions: {ins}")

        h.call("list")
        entry = next((e for e in h.all("sessions")[-1]["list"] if e["sid"] == sid), {})
        t.check(entry.get("nofolder") == 1 and entry.get("cwd") == cwd, f"listed: {entry}")

        # A fork works in a copy of its own
        f = h.call("fork", sid=sid)
        t.check(f.get("ok") == 1, f"fork: {f}")
        fork = f.get("sid", "")
        frec = logged(t, fork)[0]
        fwork = frec.get("cwd", "")
        t.check(fwork.replace("\\", "/").endswith(f"/sessions/{fork}/work") and
                frec.get("nofolder") == 1, f"the fork's record: {frec}")
        t.check(t.host.read(t.host.join(sessions, fork, "work", "notes.txt")) == "kept\n",
                "the fork has a copy of the folder")
        h.call("open", sid=fork)
        h.send("prompt", sid=fork, text="read")
        t.check(ended(h, fork, 2)["why"] == "done", "the fork goes on")
        last = h.all("tool.end", fork)[-1]
        t.check(last["status"] == "ok" and "kept" in last["text"] and
                not h.all("ask", fork), f"read in its folder: {last}")

        # Opened again with its folder gone: made again
        r = h.call("close", sid=sid, forget=1)
        t.check(r.get("ok") == 1, f"close: {r}")
        rmtree(t, work)
        r = h.call("open", sid=sid)
        t.check(r.get("ok") == 1, f"open: {r}")
        t.check(isdir(t, work) and t.host.read(t.host.join(work, "notes.txt")) is None,
                "made again, empty")
        h.send("prompt", sid=sid, text="write")
        t.check(ended(h, sid, 2)["why"] == "done", "and written again")
        t.check(t.host.read(t.host.join(work, "notes.txt")) == "kept\n", "its folder is back")

        # A delete takes it
        r = h.call("delete", sid=fork)
        t.check(r.get("ok") == 1, f"delete: {r}")
        t.check(gone(t, t.host.join(sessions, fork)), "the fork and its folder are gone")

        # An import in a folder of its own
        log = t.host.read(t.host.join(sessions, sid, "events.jsonl"))
        r = importlog(t, h, log, cwd="", nofolder=1, model=t.model, endpoint="fake")
        t.check(r.get("ok") == 1, f"import: {r}")
        imp = r.get("sid", "")
        irec = logged(t, imp)[0]
        t.check(irec.get("cwd", "").replace("\\", "/").endswith(f"/sessions/{imp}/work") and
                irec.get("nofolder") == 1, f"imported: {irec}")
        t.check(t.host.read(t.host.join(sessions, imp, "work", "notes.txt")) is None,
                "an import's folder starts empty")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=30)
def dir_listing(t):
    """dir {path, dirs?}: a directory's entries on the engine's machine,
    for completing a path typed for it: files and directories, a link to
    a directory one, a junction too (Windows), only directories with dirs; ~ in the home
    directory, kept as typed in the reply; a missing directory, a file,
    and a relative path refused; at most 2000 entries, with more."""
    env = skillhome(t)
    home = env["HOME"]
    base = t.host.join(home, "pick")
    t.host.mkdir(t.host.join(base, "alpha"))
    t.host.mkdir(t.host.join(base, "Beta"))
    t.host.write(t.host.join(base, "notes.txt"), "n\n")
    want = {"alpha": 1, "Beta": 1, "notes.txt": 0}
    if not t.host.windows:
        target = t.host.join(t.base, "elsewhere")
        t.host.mkdir(target)
        os.symlink(target, t.host.join(base, "linked"))
        os.symlink(t.host.join(t.base, "missing"), t.host.join(base, "broken"))
        os.symlink(t.host.join(base, "notes.txt"), t.host.join(base, "notelink"))
        want.update(linked=1, notelink=0)
    else:
        # Junctions, listed after enough entries that a link's name, once
        # written over them while looking where it goes, broke the listing
        t.host.bulk(base, {f"f{i:02}": "" for i in range(24)})
        want.update({f"f{i:02}": 0 for i in range(24)})
        target = t.host.join(t.base, "elsewhere")
        t.host.mkdir(target)
        win = lambda p: p.replace("/", "\\")
        for name, to in [("linked", target), ("lost", t.host.join(t.base, "missing"))]:
            link, to = win(t.host.join(base, name)), win(to)
            if isinstance(t.host, LocalHost):
                subprocess.run(["cmd", "/c", "mklink", "/J", link, to], check=True,
                               stdout=subprocess.DEVNULL)
            else:
                t.host.sh(f'cmd /c mklink /J "{link}" "{to}"')
        want.update(linked=1)
    big = t.host.join(t.base, "big")
    t.host.bulk(big, {f"f{i:04}": "" for i in range(2001)})
    t.host.mkdir(t.host.join(big, "sub"))
    h = t.headless(env=env)
    try:
        h.call("hello", proto=0, client="scenarios")
        t.check("dir" in h.all("hello")[0].get("caps", []), "the dir cap")

        def entries(r):
            return {e["name"]: e["dir"] for e in r.get("entries", [])}

        r = h.call("dir", path=base)
        t.check(r.get("ok") == 1 and r.get("path") == base and r.get("more") == 0,
                f"dir: {r}")
        t.check(entries(r) == want, f"entries: {entries(r)}, not {want}")
        r = h.call("dir", path=base + "/", dirs=1)
        dirs = {k: v for k, v in want.items() if v}
        t.check(r.get("ok") == 1 and entries(r) == dirs, f"dirs only: {r}")
        if t.host.windows:
            r = h.call("dir", path=base.replace("/", "\\") + "\\", dirs=1)
            t.check(r.get("ok") == 1 and entries(r) == dirs, f"with backslashes: {r}")

        # ~ is the home directory, the path kept as typed
        r = h.call("dir", path="~/pick", dirs=1)
        t.check(r.get("ok") == 1 and r.get("path") == "~/pick" and entries(r) == dirs,
                f"~/pick: {r}")
        r = h.call("dir", path="~", dirs=1)
        t.check(r.get("ok") == 1 and "pick" in entries(r), f"~: {r}")
        r = h.call("dir", path="~nobody/x")
        t.check(r.get("ok") == 0 and "~user" in r.get("err", ""), f"~user: {r}")

        # Refused
        for path, err in [(t.host.join(base, "nothere"), "no such directory"),
                          (t.host.join(base, "notes.txt"), "not a directory"),
                          ("pick/alpha", "the path must be absolute, or begin with ~"),
                          ("", "path is required")]:
            r = h.call("dir", path=path)
            t.check(r.get("ok") == 0 and r.get("err") == err, f"{path!r}: {r}")

        # The cap
        r = h.call("dir", path=big)
        got = entries(r)
        t.check(r.get("ok") == 1 and len(got) == 2000 and r.get("more") == 1,
                f"capped: {len(got)} entries, more {r.get('more')}")
        r = h.call("dir", path=big, dirs=1)
        t.check(r.get("ok") == 1 and entries(r) == {"sub": 1} and r.get("more") == 0,
                f"its one directory: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# An endpoint where nothing listens, after the fake one
CLOSED = "[endpoint closed]\nurl = http://127.0.0.1:9/\nconnect_timeout = 2\n"


@scenario(conf={"_extra": CLOSED + "[model {model}]\ncontext = 100000\n"
                          "[model {model2}]\ncontext = 100000\n"}, bound=15)
def models_defaults(t):
    """With no defaults configured, new takes the first endpoint with a
    URL and the first [model]; models lists each endpoint, the fake one
    with its ids, the closed one failed; new splits ENDPOINT/MODEL."""
    h = t.headless()
    try:
        h.call("hello", proto=0, client="scenarios")
        hello = h.all("hello")[0]
        t.check("models" in hello.get("caps", []) and "models" in hello.get("cmds", []),
                f"the models cap: {hello}")
        url = f"{t.host.url}/"

        r = h.call("new", cwd=t.work)
        t.check(r.get("ok") == 1, f"new with nothing named: {r}")
        s = h.all("session", r["sid"])[0]
        t.check(s.get("model") == t.model and s.get("endpoint") == url,
                f"the first endpoint and [model]: {s}")

        r = h.call("new", cwd=t.work, model=f"fake/{t.model2}")
        t.check(r.get("ok") == 1, f"new with fake/MODEL: {r}")
        s = h.all("session", r["sid"])[0]
        t.check(s.get("model") == t.model2 and s.get("endpoint") == url,
                f"split at the endpoint's name: {s}")

        r = h.call("models", timeout=15)
        eps = r.get("endpoints", [])
        t.check(r.get("ok") == 1 and [e.get("name") for e in eps] == ["fake", "closed"],
                f"models: {r}")
        fake, closed = eps
        t.check(fake.get("ok") == 1 and fake.get("url") == url and
                fake.get("kind") == "openai" and t.model in fake.get("models", []) and
                "err" not in fake, f"the fake endpoint: {fake}")
        t.check(closed.get("ok") == 0 and closed.get("err") and
                closed.get("models") == [], f"the closed endpoint: {closed}")
        t.check(r.get("endpoint") == "fake" and r.get("model") == t.model,
                f"the defaults: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def models_listed(t):
    """With no [model] and no default, new has no model until models
    has listed the endpoint's, then takes the first it listed."""
    h = t.headless()
    try:
        h.call("hello", proto=0, client="scenarios")
        r = h.call("new", cwd=t.work)
        t.check(r.get("ok") == 0 and r.get("err", "").startswith("no model:"),
                f"no model yet: {r}")
        r = h.call("models", timeout=15)
        eps = r.get("endpoints", [])
        t.check(r.get("ok") == 1 and len(eps) == 1 and eps[0].get("ok") == 1 and
                t.model in eps[0].get("models", []), f"models: {r}")
        first = eps[0]["models"][0]
        t.check(r.get("endpoint") == "fake" and r.get("model") == first,
                f"the default is the first listed: {r}")
        r = h.call("new", cwd=t.work)
        t.check(r.get("ok") == 1, f"new after models: {r}")
        s = h.all("session", r["sid"])[0]
        t.check(s.get("model") == first, f"the listed model: {s}")
        # A model named alone is the default endpoint's, as it was
        r = h.call("new", cwd=t.work, model=t.model)
        s = h.all("session", r.get("sid"))
        t.check(r.get("ok") == 1 and s and s[0].get("model") == t.model,
                f"a model by name: {r} {s}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def import_roundtrip(t):
    """import: a session's raw log, uploaded in parts, becomes a new
    session where it is imported, with the conversation's records under
    a new sid and their seqs, a session record of its own (imported_from)
    and the title "TITLE (imported)", and goes on from its history."""
    h = t.headless()
    try:
        r = h.call("hello", proto=0, client="scenarios")
        caps = h.all("hello")[0].get("caps", [])
        t.check("import" in caps, f"caps: {caps}")
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="one")
        ended(h, sid, 1)
        src = logged(t, sid)
        raw = t.host.read(t.host.join(t.state, "sessions", sid, "events.jsonl"))
        t.check(any(e["ev"] == "snap" for e in src), "a snapshot to leave behind")

        r = importlog(t, h, raw, part=500, model=t.model, endpoint="fake")
        new = r.get("sid")
        t.check(r.get("ok") == 1 and r.get("from") == sid and new and new != sid,
                f"imported: {r}")
        recs = logged(t, new)
        first = recs[0]
        t.check(list(first)[:5] == ["sid", "seq", "ts", "ev", "imported_from"] and
                first["imported_from"] == sid and first["model"] == t.model and
                "forked_from" not in first and "roots" not in first,
                f"its session record: {first}")
        t.check(first["scratch"] != src[0]["scratch"] and first["scratch"].endswith(new),
                f"a scratch directory of its own: {first['scratch']}")
        kept = [e for e in src if e["ev"] in IMPORTED]
        t.check([(e["seq"], e["ev"]) for e in recs[:-1]] ==
                [(e["seq"], e["ev"]) for e in kept] and
                all(e["sid"] == new for e in recs), "the conversation, as it was")
        for a, b in zip(recs[1:-1], kept[1:]):
            a = {k: v for k, v in a.items() if k != "sid"}
            b = {k: v for k, v in b.items() if k != "sid"}
            if b["ev"] == "tool.end":
                b = {k: v for k, v in b.items() if k not in ("spill", "bytes")}
            t.check(a == b, f"kept as it was: {a} {b}")
        t.check(recs[-1]["ev"] == "title" and recs[-1]["text"].endswith(" (imported)") and
                sidecar(t, new, "title") == recs[-1]["text"], f"titled {recs[-1]}")
        t.check(listentry(h, new)["title"].endswith(" (imported)"), "listed")

        o = h.call("open", sid=new)
        t.check(o.get("ok") == 1 and o.get("readonly") == 0, f"open: {o}")
        h.send("prompt", sid=new, text="two")
        t.check(ended(h, new, 2)["why"] == "done", "it goes on")
        said = h.all("assistant", new)[-1]
        text = "".join(b.get("text", "") for b in said.get("blocks", []))
        t.check(text == "Answer to two after 2 prompts.", f"with its history: {text!r}")
        t.check(any(e["ev"] == "prefix" for e in logged(t, new)), "a prefix of its own")
        t.check(logged(t, sid) == src, "the original untouched")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


def tinypng():
    """A PNG of one red pixel."""
    def chunk(kind, data):
        crc = zlib.crc32(kind + data)
        return len(data).to_bytes(4, "big") + kind + data + crc.to_bytes(4, "big")
    ihdr = (1).to_bytes(4, "big") * 2 + bytes([8, 2, 0, 0, 0])
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00")) + chunk(b"IEND", b""))


@scenario(bound=20)
def import_hostile(t):
    """A log from elsewhere is not trusted: an import drops its snapshots
    (whose paths a rewind would write), grants and roots (which widen the
    guard), and a tool result's image and spill (read from the session's
    directory), keeps only complete lines, and the session goes on."""
    victim = t.host.join(t.base, "victim.txt")
    t.host.write(victim, "safe\n")
    # Where an image ../../x.png of the import's directory would be read
    t.host.write(t.host.join(t.state, "x.png"), tinypng())
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="one")
        ended(h, sid, 1)
        recs = logged(t, sid)
        for e in recs:
            if e["ev"] == "session":
                e.update(roots=["/"], temp="allow", scratch=t.base, parent_x=1)
            elif e["ev"] == "snap":
                e.update(path=victim, existed=1)
            elif e["ev"] == "tool.end":
                e.update(image="../../x.png", width=8, height=8, spill="../../victim.txt",
                         child="../x")
        last = recs[-1]["seq"]
        recs.append({"sid": sid, "seq": last + 1, "ts": 1, "ev": "grant", "path": "/",
                     "access": "write", "by": "user"})
        recs.append({"sid": sid, "seq": last + 2, "ts": 1, "ev": "rewind",
                     "undone": ["snap/1"]})
        raw = "".join(json.dumps(e, separators=(",", ":")) + "\n" for e in recs)
        raw += '{"sid":"%s","seq":%d,"ts":1,"ev":"user","te' % (sid, last + 3)

        r = importlog(t, h, raw, model=t.model, endpoint="fake")
        new = r.get("sid")
        t.check(r.get("ok") == 1 and r.get("from") == sid, f"imported: {r}")
        got = logged(t, new)
        evs = {e["ev"] for e in got}
        t.check(evs <= IMPORTED and "snap" not in evs and "grant" not in evs,
                f"only the conversation: {sorted(evs)}")
        first = got[0]
        t.check(not {"roots", "temp", "parent_x"} & set(first) and
                first["scratch"] != t.base, f"the session record anew: {first}")
        ends = [e for e in got if e["ev"] == "tool.end"]
        t.check(ends and not any({"image", "spill", "child", "width"} & set(e) for e in ends),
                f"no files named: {ends}")
        t.check(not any(e["seq"] == last + 3 and e["ev"] == "user" for e in got),
                "the torn line left out")

        h.call("open", sid=new)
        rw = h.call("rewind", sid=new, to=1)
        t.check(rw.get("ok") == 0 or not rw.get("files"), f"nothing to rewind: {rw}")
        t.check(t.host.read(victim) == "safe\n", "the file named by a snapshot untouched")
        h.send("prompt", sid=new, text="two")
        t.check(ended(h, new, 2)["why"] == "done", "it goes on")
        body = json.dumps(t.requests()[-1]["body"])
        t.check("image_url" not in body and "base64" not in body, "no image in the request")
        t.check(t.host.read(victim) == "safe\n", "untouched still")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def import_refused(t):
    """upload refuses a part out of order, too large, or not base64, and a
    log over 1 GiB; import refuses an upload not done, a subagent's log,
    one not a session's, and a cwd that does not exist, and removes the
    staged log whatever comes of it. Stale staged files are swept when
    the engine starts."""
    tmp = t.host.join(t.state, "tmp")
    t.host.mkdir(tmp)
    stale = t.host.join(tmp, "import-00000000000000aa.part")
    fresh = t.host.join(tmp, "import-00000000000000bb.jsonl")
    t.host.write(stale, "x")
    t.host.write(fresh, "y")
    t.host.touch(stale, time.time() - 7200)
    h = t.headless()
    try:
        h.call("hello", proto=0, client="scenarios")
        t.check(t.host.read(stale) is None and t.host.read(fresh) == "y",
                "the stale upload swept, the fresh one left")

        def refused(cmd, err, **fields):
            r = h.call(cmd, **fields)
            t.check(r.get("ok") == 0 and r.get("err") == err, f"{cmd} {fields}: {r}")

        b64 = base64.b64encode
        refused("upload", "too large to import: a log is at most 1 GiB",
                data=b64(b"xx").decode(), off=(1 << 30) - 1)
        refused("upload", "a part is at most 512 KiB",
                data=b64(b"x" * ((1 << 19) + 1)).decode(), off=0)
        refused("upload", "data must be base64", data="!!!!", off=0)
        refused("upload", "off must be 0 to begin an upload", data="", off=5)
        refused("upload", "invalid token", data="", off=0, token="../x")
        refused("upload", "no such upload", data="", off=3, token="0123456789abcdef")
        r = h.call("upload", data=b64(b"abc").decode(), off=0)
        token = r.get("token")
        t.check(r.get("ok") == 1 and r.get("off") == 3 and len(token or "") == 16,
                f"begun: {r}")
        refused("upload", "off must be 3, the bytes staged so far",
                data=b64(b"abc").decode(), off=0, token=token)
        refused("import", "no such upload: upload the log first, ending with done",
                token=token, cwd=t.work)
        refused("import", "invalid token", token="x", cwd=t.work)

        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="two")
        ended(h, sid, 1)
        raw = t.host.read(t.host.join(t.state, "sessions", sid, "events.jsonl"))
        r = importlog(t, h, raw, cwd=t.host.join(t.work, "nowhere"))
        t.check(r.get("ok") == 0 and r.get("err") == "cwd must name an existing directory",
                f"no such cwd: {r}")
        recs = logged(t, sid)
        recs[0]["parent"] = "20260101-000000-dad"
        kid = "".join(json.dumps(e) + "\n" for e in recs)
        r = importlog(t, h, kid, model=t.model, endpoint="fake")
        t.check(r.get("ok") == 0 and r.get("err") ==
                "a subagent's log cannot be imported, only its parent's", f"a subagent: {r}")
        r = importlog(t, h, "hello\nworld\n", model=t.model, endpoint="fake")
        t.check(r.get("ok") == 0 and r.get("err") ==
                "not a session's raw log: it must begin with its session record",
                f"not a log: {r}")
        h.call("list")
        listed = [e["sid"] for e in h.all("sessions")[-1]["list"]]
        t.check(listed == [sid], f"nothing imported: {listed}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# Images attached to prompts: uploaded (upload {kind: "image", sid}),
# kept in the session's img/, named by prompt and steer {images}, sent
# as parts of the user message (fake_provider.sc_attach answers with how
# many it sees).

def rgbpng(w, h, pixel=lambda x, y: (x & 255, y & 255, 128), level=6):
    """A PNG of w by h RGB pixels."""
    def chunk(kind, data):
        crc = zlib.crc32(kind + data)
        return len(data).to_bytes(4, "big") + kind + data + crc.to_bytes(4, "big")
    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    raw = b"".join(b"\x00" + bytes(c for x in range(w) for c in pixel(x, y))
                   for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, level)) + chunk(b"IEND", b""))


def fakejpeg(w, h):
    """A JPEG's header, as far as its frame, which is all the engine reads."""
    app0 = b"\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    sof = b"\xff\xc0\x00\x11\x08" + h.to_bytes(2, "big") + w.to_bytes(2, "big") + \
        b"\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01"
    return b"\xff\xd8" + app0 + sof + b"\xff\xd9"


def flatjpeg(w, h):
    """A baseline grayscale JPEG of w by h mid-gray pixels: each block's
    codes, under one-symbol Huffman tables, a bit for its DC and one for
    its end, both zero."""
    dqt = b"\xff\xdb\x00\x43\x00" + bytes([1] * 64)
    sof = b"\xff\xc0\x00\x0b\x08" + h.to_bytes(2, "big") + w.to_bytes(2, "big") + \
        b"\x01\x01\x11\x00"
    def dht(cls):
        return b"\xff\xc4\x00\x14" + bytes([cls, 1]) + bytes(15) + b"\x00"
    sos = b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00"
    bits = 2 * ((w + 7) // 8) * ((h + 7) // 8)
    data = bytes(bits // 8) + (bytes([0xff >> bits % 8]) if bits % 8 else b"")
    return b"\xff\xd8" + dqt + sof + dht(0x00) + dht(0x10) + sos + data + b"\xff\xd9"


def rgbbmp(w, h, pixel):
    """A 24-bit BMP of w by h pixels, its rows bottom up, padded."""
    pad = -3 * w % 4
    data = b"".join(b"".join(bytes(reversed(pixel(x, y))) for x in range(w)) + bytes(pad)
                    for y in reversed(range(h)))
    le = lambda v, n: v.to_bytes(n, "little")
    return (b"BM" + le(54 + len(data), 4) + le(0, 4) + le(54, 4) + le(40, 4) +
            le(w, 4) + le(h, 4) + le(1, 2) + le(24, 2) + le(0, 4) + le(len(data), 4) +
            le(2835, 4) * 2 + le(0, 4) * 2 + data)


# Pillow's, 6x4, two frames: the first in four colors by (x + y) % 4, the
# second all green
GIF = base64.b64decode(
    "R0lGODlhBgAEAIEAAP8AAAD/AAAA/wkJCSH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAABgAE"
    "AAAIEQABBBAwQODAggMJCiSIUGFAACH5BAAKAAAALAAAAAAGAAQAgf8AAAD/AAAA/wkJCQgLAAMIHEiw"
    "oMGBAQEAOw==")
GIFPAL = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (9, 9, 9)]
# cwebp's: 5x3 lossless, (x*50, y*80, 200); 16x16 lossy
WEBP = base64.b64decode(
    "UklGRjwAAABXRUJQVlA4TC8AAAAvBIAAAHdAkG0zxCLcH+QMIzPI89+DDLC8AAYBIBGoWFJIIbgUVhg1"
    "ov/hIce+AAA=")
WEBPLOSSY = base64.b64decode(
    "UklGRl4AAABXRUJQVlA4IFIAAABQAgCdASoQABAAAMASJbACdLoAAwYBSFzN8gAA/v4sCk78k78g1Sof"
    "zGPAvUZRnebmI8/X/+Im70GL5gO5X3vwU3/9MbXgB/7xvwUp/8+AAAAA")


def upimage(t, h, sid, data, part=1 << 19):
    """Upload an image to a session, part by part; the last reply."""
    token, off = None, 0
    while True:
        chunk = data[off:off + part]
        last = off + len(chunk) >= len(data)
        args = {"kind": "image", "sid": sid, "data": base64.b64encode(chunk).decode(),
                "off": off}
        if token:
            args["token"] = token
        if last:
            args["done"] = 1
        r = h.call("upload", timeout=60, **args)
        if not last or r.get("ok") != 1:
            t.check(r.get("ok") == 1 or last, f"upload at {off}: {r}")
        if last or r.get("ok") != 1:
            return r
        token, off = r["token"], r["off"]


def attached(t, hd, sid, data, **want):
    """Upload an image, which must be taken as want says; its name."""
    r = upimage(t, hd, sid, data)
    t.check(r.get("ok") == 1 and all(r.get(k) == v for k, v in want.items()),
            f"attached: {r}")
    return r["name"]


def lastimages(req):
    return fake_provider.user_images(req["body"])


def rmfile(t, path):
    if isinstance(t.host, LocalHost):
        os.remove(path)
    else:
        t.host.sh(f"rm -f {shlex.quote(path)}")


PNG = rgbpng(3, 2)
JPG = fakejpeg(640, 480)


@scenario(bound=15)
def attach(t):
    """An image is uploaded in parts and kept in its session's img/,
    numbered; a prompt names it, and the user record lists it, with its
    type and size; the request carries it as an image_url part after the
    text, as a data URL. Images alone need no text."""
    h = t.headless()
    try:
        h.call("hello", proto=0, client="scenarios")
        caps = h.all("hello")[0].get("caps", [])
        t.check("images" in caps, f"caps: {caps}")
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        r = upimage(t, h, sid, PNG, part=20)
        t.check(r.get("ok") == 1 and r.get("name") == "img/1.png" and
                r.get("mime") == "image/png" and (r.get("w"), r.get("h")) == (3, 2),
                f"the first: {r}")
        name = attached(t, h, sid, JPG, name="img/2.jpg", mime="image/jpeg", w=640, h=480)
        r = h.call("prompt", sid=sid, text="what are these?", images=["img/1.png", name])
        t.check(r.get("ok") == 1, f"prompt: {r}")
        ended(h, sid, 1)
        user = h.all("user", sid)[0]
        t.check(user["text"] == "what are these?" and user["images"] == [
            {"name": "img/1.png", "mime": "image/png", "w": 3, "h": 2},
            {"name": "img/2.jpg", "mime": "image/jpeg", "w": 640, "h": 480}],
            f"the user record: {user}")
        body = t.requests()[-1]["body"]
        content = body["messages"][-1]["content"]
        t.check(content[0] == {"type": "text", "text": "what are these?"} and
                lastimages(t.requests()[-1]) ==
                [("image/png", base64.b64encode(PNG).decode()),
                 ("image/jpeg", base64.b64encode(JPG).decode())],
                f"the request: {str(content)[:300]}")
        t.check(lastsaid(h, sid) == "I see 2 images.", f"said: {lastsaid(h, sid)}")
        t.check(t.host.read(t.host.join(t.state, "sessions", sid, "img", "1.png")) is not None,
                "kept in img/")

        # Images alone, the history's too
        r = h.call("prompt", sid=sid, text="", images=["img/1.png"])
        t.check(r.get("ok") == 1, f"images alone: {r}")
        ended(h, sid, 2)
        t.check(lastsaid(h, sid) == "I see 3 images.", f"said: {lastsaid(h, sid)}")
        t.check(t.requests()[-1]["body"]["messages"][-1]["content"][0]["type"] == "image_url",
                "no text part")
        r = h.call("prompt", sid=sid, text="")
        t.check(r.get("ok") == 0 and r.get("err") == "text is required", f"nothing: {r}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_extra": "[model {model2}]\nvision = false\n"}, bound=15)
def attach_refused(t):
    """upload refuses what is not a PNG, JPEG, GIF, WebP or BMP, a damaged
    image, one too large that does not decode to shrink, a session that
    does not exist; prompt refuses more than 10 images,
    names not made by upload, images that are not there, and any image
    for a model with vision off."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]

        def refused(r, err):
            t.check(r.get("ok") == 0 and r.get("err", "").startswith(err), f"{err}: {r}")

        refused(upimage(t, h, sid, b"GIF89a" + b"\x00" * 64),
                "a damaged GIF: its size cannot be read: ")
        refused(upimage(t, h, sid, b"plain text, not an image"),
                "not an image: only PNG, JPEG, GIF, WebP and BMP can be attached")
        refused(upimage(t, h, sid, GIF[:70]),
                "the image cannot be converted to a PNG: ")
        refused(upimage(t, h, "20260101-000000-none", PNG), "no such session")
        refused(upimage(t, h, "../x", PNG), "an image needs its session's sid")
        refused(h.call("upload", kind="text", data="", off=0), "kind must be")
        refused(upimage(t, h, sid, fakejpeg(60000, 60000)),
                "the image is too large, and cannot be shrunk: too many pixels")
        refused(upimage(t, h, sid, rgbpng(9000, 50)[:-40]),
                "the image is too large, and cannot be shrunk: it ends early")
        tmp = t.host.join(t.state, "tmp")
        if isinstance(t.host, LocalHost) and os.path.isdir(tmp):
            t.check(not os.listdir(tmp), f"nothing left staged: {os.listdir(tmp)}")
        t.check(t.host.read(t.host.join(t.state, "sessions", sid, "img", "1.png")) is None,
                "nothing kept")

        name = attached(t, h, sid, PNG, name="img/1.png")
        refused(h.call("prompt", sid=sid, text="x", images=[name] * 11),
                "too many images: at most 10 in a message")
        refused(h.call("prompt", sid=sid, text="x", images=["../events.jsonl"]),
                "an image must be named as upload named it")
        refused(h.call("prompt", sid=sid, text="x", images=["img/7.png"]),
                "no such image in the session")
        refused(h.call("prompt", sid=sid, text="x", images="img/1.png"),
                "images must be an array")
        r = h.call("prompt", sid=sid, text="ten", images=[name] * 10)
        t.check(r.get("ok") == 1, f"ten: {r}")
        ended(h, sid, 1)
        t.check(lastsaid(h, sid) == "I see 10 images.", f"said: {lastsaid(h, sid)}")
        t.check(len(h.all("user", sid)) == 1, "only the one taken")

        # A model without vision takes none
        other = h.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        n2 = attached(t, h, other, PNG)
        refused(h.call("prompt", sid=other, text="x", images=[n2]),
                "this model takes no images")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def attach_convert(t):
    """A GIF (its first frame), a BMP and a WebP, lossless or lossy, are
    each kept, and sent, as a PNG of their pixels."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        rows53 = [bytes(c for x in range(5) for c in (x * 50, y * 80, 200)) for y in range(3)]
        cases = [
            (GIF, 6, 4, [bytes(c for x in range(6) for c in GIFPAL[(x + y) % 4])
                         for y in range(4)]),
            (rgbbmp(5, 3, lambda x, y: (x * 50, y * 80, 200)), 5, 3, rows53),
            (WEBP, 5, 3, rows53),
            (WEBPLOSSY, 16, 16, None),  # its size only
        ]
        names = []
        for i, (data, w, hh, rows) in enumerate(cases, 1):
            names.append(attached(t, h, sid, data, name=f"img/{i}.png", mime="image/png",
                                  w=w, h=hh))
        h.call("prompt", sid=sid, text="look", images=names)
        ended(h, sid, 1)
        t.check(lastsaid(h, sid) == "I see 4 images.", f"said: {lastsaid(h, sid)}")
        imgs = lastimages(t.requests()[-1])
        t.check([m for m, _ in imgs] == ["image/png"] * 4, f"as sent: {imgs}")
        pngs = [base64.b64decode(b) for _, b in imgs]
        for (data, w, hh, rows), png in zip(cases, pngs):
            got = pngpixels(png)
            t.check(got[:2] == (w, hh) and (rows is None or got[2] == rows),
                    f"pixels of {data[:4]}: {got}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=30)
def attach_shrink(t):
    """A PNG or JPEG too large for the APIs, over 8000 pixels on a side or
    5 MiB as base64, is shrunk until it fits, and kept as a PNG."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        wide = rgbpng(9000, 3)
        r = upimage(t, h, sid, wide)
        t.check(r.get("ok") == 1 and (r.get("w"), r.get("h")) == (8000, 2) and
                r.get("mime") == "image/png", f"wide: {r}")
        rnd = random.Random(1)
        noise = bytes(rnd.getrandbits(8) for _ in range(1200 * 1200 * 3))
        big = rgbpng(1200, 1200, lambda x, y: noise[(y*1200 + x)*3:(y*1200 + x)*3 + 3],
                     level=0)
        t.check(len(big) * 4 // 3 > 5 << 20, f"big enough: {len(big)}")
        r = upimage(t, h, sid, big)
        t.check(r.get("ok") == 1 and 0 < r.get("w", 0) < 1200 and r.get("w") == r.get("h"),
                f"big: {r}")
        jpg = upimage(t, h, sid, flatjpeg(9000, 16))
        t.check(jpg.get("ok") == 1 and jpg.get("name") == "img/3.png" and
                (jpg.get("w"), jpg.get("h")) == (8000, 14) and jpg.get("mime") == "image/png",
                f"wide JPEG: {jpg}")
        h.call("prompt", sid=sid, text="look", images=["img/1.png", r["name"], jpg["name"]])
        ended(h, sid, 1)
        imgs = lastimages(t.requests()[-1])
        sizes = [int.from_bytes(base64.b64decode(b)[16:20], "big") for _, b in imgs]
        t.check(sizes == [8000, r["w"], 8000] and all(len(b) <= 5 << 20 for _, b in imgs),
                f"as sent: {sizes} {[len(b) for _, b in imgs]}")
        _, _, rows = pngpixels(base64.b64decode(imgs[2][1]))
        t.check(all(abs(v - 128) <= 1 for row in rows for v in row),
                f"mid-gray: {set(v for row in rows for v in row)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def attach_replay(t):
    """Opened again, a session sends its images again from their files;
    one whose file is gone becomes a note in its place, the turn going
    on."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a = attached(t, h, sid, PNG)
        b = attached(t, h, sid, JPG)
        h.call("prompt", sid=sid, text="one", images=[a, b])
        ended(h, sid, 1)
    finally:
        h.close()
    h = t.headless()
    try:
        t.check(h.call("open", sid=sid).get("ok") == 1, "reopened")
        h.call("prompt", sid=sid, text="two")
        ended(h, sid, 2)
        t.check(lastsaid(h, sid) == "I see 2 images.", f"said: {lastsaid(h, sid)}")
        rmfile(t, t.host.join(t.state, "sessions", sid, "img", "2.jpg"))
        h.call("prompt", sid=sid, text="three")
        ended(h, sid, 3)
        t.check(lastsaid(h, sid) == "I see 1 images.", f"said: {lastsaid(h, sid)}")
        first = t.requests()[-1]["body"]["messages"][1]["content"]
        t.check([p["type"] for p in first] == ["text", "image_url", "text"] and
                first[2]["text"].startswith("[image lost: img/2.jpg"), f"lost: {first}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def attach_copies(t):
    """A fork copies the images its log names, and goes on with them; an
    import, which brings no files, drops them from the user records."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        a = attached(t, h, sid, PNG)
        h.call("prompt", sid=sid, text="one", images=[a])
        ended(h, sid, 1)
        attached(t, h, sid, JPG)  # not named in the log
        r = h.call("fork", sid=sid)
        new = r.get("sid")
        t.check(r.get("ok") == 1 and new, f"fork: {r}")
        img = t.host.join(t.state, "sessions", new, "img")
        t.check(t.host.read(t.host.join(img, "1.png")) is not None and
                t.host.read(t.host.join(img, "2.jpg")) is None, "the image named, copied")
        h.call("open", sid=new)
        h.call("prompt", sid=new, text="two")
        ended(h, new, 2)
        t.check(lastsaid(h, new) == "I see 1 images.", f"forked: {lastsaid(h, new)}")

        raw = t.host.read(t.host.join(t.state, "sessions", sid, "events.jsonl"))
        r = importlog(t, h, raw, model=t.model, endpoint="fake")
        imp = r.get("sid")
        t.check(r.get("ok") == 1, f"imported: {r}")
        users = [e for e in logged(t, imp) if e["ev"] == "user"]
        t.check(users and not any("images" in e for e in users), f"stripped: {users}")
        h.call("open", sid=imp)
        h.call("prompt", sid=imp, text="two")
        ended(h, imp, 2)
        t.check(lastsaid(h, imp) == "I see 0 images.", f"imported: {lastsaid(h, imp)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def attach_steer(t):
    """A steer and a prompt with images, sent while a response streams,
    queue with them (the queue counts them); one withdrawn comes back
    with its images, the other is taken in with them."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        a = attached(t, h, sid, PNG)
        b = attached(t, h, sid, JPG)
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "delta" and e.get("sid") == sid)
        t.check(h.call("steer", sid=sid, text="see", images=[a]).get("ok") == 1, "steer")
        t.check(h.call("prompt", sid=sid, text="later", images=[a, b]).get("ok") == 1,
                "prompt")
        q = h.wait(lambda e: e.get("ev") == "queue" and e.get("sid") == sid and
                   len(e.get("items", [])) == 2)
        t.check([i.get("images") for i in q["items"]] == [1, 2], f"queued: {q}")
        r = h.call("unqueue", sid=sid, item=q["items"][1]["id"])
        t.check(r.get("ok") == 1 and r.get("text") == "later" and
                [i["name"] for i in r.get("images", [])] == [a, b], f"unqueue: {r}")
        # Too late for the one-step turn, the steer is the next prompt
        ended(h, sid, 2)
        users = [(e["text"], [i["name"] for i in e.get("images", [])])
                 for e in h.all("user", sid)]
        t.check(users == [("go", []), ("see", [a])], f"taken in: {users}")
        t.check(lastsaid(h, sid) == "I see 1 images.", f"said: {lastsaid(h, sid)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# Plan mode: fake_provider.sc_plan tries to write inside.txt, is refused,
# asks for an empty plan's approval, writes the plan file and asks again;
# approved, it writes inside.txt.

def planask(h, sid, after=0):
    """The next ask of a plan's approval in sid."""
    return h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid and
                  e.get("kind") == "plan" and e.get("seq", 0) > after)


def modes(h, sid):
    return [f"{e['mode']}/{e['by']}" for e in h.all("mode", sid)]


def lastsaid(h, sid):
    said = [b.get("text", "") for e in h.all("assistant", sid)
            for b in e.get("blocks", []) if b.get("t") == "text"]
    return said[-1] if said else ""


@scenario(bound=15)
def plan_new(t):
    """A session new in plan mode: its agent told so, a write outside the
    plan file refused, an empty plan not put to the user, the plan file
    written without an ask, then the plan asked about with no deadline:
    kept planning with a note, which the agent reads, then approved, which
    ends plan mode, and the write goes ahead."""
    h = t.headless()
    try:
        r = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1,
                   tools="read write")
        t.check(r.get("ok") == 0 and "exit_plan_mode" in r.get("err", ""),
                f"no plan mode without its way out: {r}")
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1,
                     plan=1)["sid"]
        mode = h.wait(lambda e: e.get("ev") == "mode" and e.get("sid") == sid)
        plan = mode.get("path", "")
        t.check(mode["mode"] == "plan" and mode["by"] == "new" and
                plan.endswith(f"/{sid}/plan.md"), f"mode: {mode}")
        h.send("prompt", sid=sid, text="Write inside.")
        ask = planask(h, sid)
        t.check(ask["deadline"] == 0 and ask["path"] == plan and ask["tool"] == "exit_plan_mode"
                and "Write **inside.txt**." in ask["detail"], f"the plan's ask: {ask}")
        ends = h.all("tool.end", sid)
        t.check([e["status"] for e in ends] == ["denied", "error", "ok"],
                f"refused, no plan yet, the plan written: {ends}")
        t.check("plan mode is on" in ends[0]["text"] and plan in ends[0]["text"],
                f"why: {ends[0]['text']}")
        t.check("no plan to approve yet" in ends[1]["text"], f"empty: {ends[1]['text']}")
        t.check(not [e for e in h.all("ask", sid) if e.get("kind") != "plan"],
                "the plan file needs no ask")
        t.check(t.file("inside.txt") is None, "nothing written while planning")
        first = t.requests()[0]["body"]
        msgs = first["messages"]
        t.check(msgs[-1]["role"] == "user" and msgs[-1]["content"].startswith("Plan mode is on")
                and plan in msgs[-1]["content"], f"told: {msgs[-1]}")
        t.check("except to approve a plan in plan mode" in msgs[0]["content"],
                "the system prompt says plan mode exists")
        names = fake_provider.tool_names(first)
        t.check("enter_plan_mode" in names and "exit_plan_mode" in names, f"tools: {names}")

        h.call("answer", sid=sid, req=ask["req"], decision="deny", note="Also test it.")
        again = planask(h, sid, ask["seq"])
        t.check(again["detail"] == "# Plan v2\n\n- Also test it.", f"revised: {again}")
        t.check(t.host.read(plan) == "# Plan v2\n\n- Also test it.\n", "the plan file")
        t.check(modes(h, sid) == ["plan/new"], f"still planning: {modes(h, sid)}")
        h.call("answer", sid=sid, req=again["req"], decision="allow")
        end = ended(h, sid, 1)
        t.check(end["why"] == "done", f"done: {end}")
        t.check(t.file("inside.txt") == "done\n", "written once approved")
        t.check(modes(h, sid) == ["plan/new", "default/approved"], f"modes: {modes(h, sid)}")
        res = [e["text"] for e in h.all("tool.end", sid)]
        t.check(any("keep planning" in x and "Also test it." in x for x in res) and
                any("approved your plan" in x and "Plan v2" in x and "todo_write" in x
                    for x in res), f"results: {res}")
        t.check(lastsaid(h, sid) == "Done.", f"said: {lastsaid(h, sid)}")
        # A fork has the plan file too, where its log now names it
        r = h.call("fork", sid=sid, turn=1)
        t.check(r.get("ok") == 1, f"forked: {r}")
        fk = [e for e in logged(t, r["sid"]) if e["ev"] == "mode"]
        path = fk[0].get("path", "") if fk else ""
        t.check(len(fk) == 2 and path.endswith(f"/{r['sid']}/plan.md") and
                t.host.read(path) == "# Plan v2\n\n- Also test it.\n", f"the fork's plan: {fk}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_reopen(t):
    """Plan mode survives a session opened again, as does its notice,
    never sent before; unattended, nobody can approve the plan, so plan
    mode stays on and the turn ends with the plan left for review."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1)["sid"]
    finally:
        h.close()
    r = t.run("Write inside.", resume=sid)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    log = logged(t, sid)
    t.check([e["by"] for e in log if e["ev"] == "mode"] == ["new"], "one mode record")
    msgs = t.requests()[0]["body"]["messages"]
    t.check(msgs[-1]["content"].startswith("Plan mode is on"), f"told: {msgs[-1]}")
    st = [e["status"] for e in r.all("tool.end")]
    t.check(st == ["denied", "error", "ok", "denied"], f"statuses {st}")
    ask = r.last("ask")
    ans = r.last("answer")
    t.check(ask.get("kind") == "plan" and ans.get("by") == "policy", f"{ask} {ans}")
    t.check("Nobody can approve the plan" in r.all("tool.end")[-1]["text"], "why")
    t.check(r.text().endswith("Plan left for review."), f"text {r.text()!r}")
    t.check(t.file("inside.txt") is None, "nothing written")
    t.check((t.host.read(ask.get("path", "")) or "").startswith("# Plan"), "the plan stays")


@scenario(bound=15)
def plan_mode(t):
    """The mode command: on and off again before a prompt, which tells the
    agent nothing; on, then refused while the plan is asked about; a steer
    instead of an answer keeps plan mode; off while idle, told."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        r = h.call("mode", sid=sid, mode="maybe")
        t.check(r.get("ok") == 0, f"an unknown mode: {r}")
        r = h.call("mode", sid=sid, mode="plan")
        t.check(r.get("ok") == 1 and r.get("mode") == "plan", f"on: {r}")
        r = h.call("mode", sid=sid, mode="default")
        t.check(r.get("ok") == 1 and r.get("mode") == "default", f"off: {r}")
        t.check(modes(h, sid) == ["plan/user", "default/user"], f"modes {modes(h, sid)}")
        h.send("prompt", sid=sid, text="Write inside.")
        ended(h, sid, 1)
        t.check(t.file("inside.txt") == "done\n", "written")
        told = [m for q in t.requests() for m in q["body"]["messages"]
                if str(m.get("content", "")).startswith("Plan mode")]
        t.check(not told, f"nothing to tell: {told}")

        h.call("mode", sid=sid, mode="plan")
        h.send("prompt", sid=sid, text="Write inside again.")
        ask = planask(h, sid)
        r = h.call("mode", sid=sid, mode="default")
        t.check(r.get("ok") == 0 and "answer" in r.get("err", ""), f"refused: {r}")
        h.call("steer", sid=sid, text="Stop.")
        ended(h, sid, 2)
        ans = h.all("answer", sid)[-1]
        t.check(ans["by"] == "steer", f"answered by the steer: {ans}")
        t.check(lastsaid(h, sid) == "Stopped.", f"said {lastsaid(h, sid)}")
        t.check(modes(h, sid)[-1] == "plan/user", "still planning")

        r = h.call("mode", sid=sid, mode="default")
        t.check(r.get("ok") == 1, f"off: {r}")
        h.send("prompt", sid=sid, text="Write it now.")
        ended(h, sid, 3)
        msgs = t.requests()[-2]["body"]["messages"]
        t.check(msgs[-1]["content"].startswith("Plan mode is off"), f"told: {msgs[-1]}")
        t.check(lastsaid(h, sid) == "Done.", f"said {lastsaid(h, sid)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_revised(t):
    """The plan file revised by hand while its ask waits (the GUI's Edit
    plan): approved, the agent carries out the file as it is then, told
    that the user revised it; emptied before approval, the result says
    so, and to carry out the plan as the agent wrote it."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1,
                     plan=1)["sid"]
        h.send("prompt", sid=sid, text="Write inside.")
        ask = planask(h, sid)
        revised = "# Plan, revised\n\n- Write **inside.txt**, by hand.\n"
        t.host.write(ask["path"], revised)
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        ended(h, sid, 1)
        res = [e["text"] for e in h.all("tool.end", sid) if "approved your plan" in e["text"]]
        t.check(len(res) == 1 and "revised the plan before approving it" in res[0] and
                res[0].endswith(revised.rstrip()) and ask["path"] in res[0] and
                "as approved" not in res[0], f"the revised plan: {res}")
        t.check(t.file("inside.txt") == "done\n", "carried out")

        # Again, the plan file emptied before approval
        h.call("mode", sid=sid, mode="plan")
        h.send("prompt", sid=sid, text="Write inside again.")
        again = planask(h, sid, ask["seq"])
        t.check(again["detail"] == revised.rstrip(), f"asked as it is: {again['detail']!r}")
        t.host.write(again["path"], "")
        h.call("answer", sid=sid, req=again["req"], decision="allow")
        ended(h, sid, 2)
        res = [e["text"] for e in h.all("tool.end", sid) if "approved your plan" in e["text"]]
        t.check(len(res) == 2 and "was empty or unreadable" in res[1] and
                "as you wrote it" in res[1], f"emptied: {res[-1:]}")
        t.check(modes(h, sid)[-1] == "default/approved", f"modes: {modes(h, sid)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_enter(t):
    """enter_plan_mode: unattended, denied by policy; attended, allowed,
    which the call's result tells, with no notice, then the plan approved."""
    r = t.run("Plan first.")
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(r.last("ask").get("kind") == "plan.enter" and r.last("answer").get("by") == "policy",
            f"denied: {r.last('answer')}")
    t.check(not r.all("mode") and r.text() == "No plan mode.", f"text {r.text()!r}")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="Plan first.")
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["kind"] == "plan.enter" and 29000 <= ask["deadline"] - ask["ts"] <= 31000,
                f"the ask: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        plan = planask(h, sid)
        t.check(modes(h, sid) == ["plan/agent"], f"modes {modes(h, sid)}")
        res = h.all("tool.end", sid)[0]
        t.check(res["status"] == "ok" and "The user agreed" in res["text"] and
                plan["path"] in res["text"], f"told: {res}")
        notices = [e for e in h.all("notice", sid) if e.get("kind") == "mode"]
        t.check(not notices, f"no notice: {notices}")
        h.call("answer", sid=sid, req=plan["req"], decision="allow")
        ended(h, sid, 1)
        t.check(t.file("inside.txt") == "done\n", "written once approved")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_model(t):
    """The model may change while the plan is asked about, for its doing,
    though not while a turn runs otherwise."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1,
                     plan=1)["sid"]
        h.send("prompt", sid=sid, text="Write inside.")
        ask = planask(h, sid)
        n = len(t.requests())
        r = h.call("model", sid=sid, model=t.model2)
        t.check(r.get("ok") == 1 and r.get("model") == t.model2, f"switched: {r}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        ended(h, sid, 1)
        t.check(len(t.requests()) == n, "the old model is asked no more")
        after = t.requests(t.model2)
        t.check(after and "approved your plan" in after[0]["body"]["messages"][-1]["content"],
                "the new model carries the plan out")
        t.check(t.file("inside.txt") == "done\n", "written")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_patch(t):
    """In plan mode, a patch may change the plan file alone."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1,
                     edit_format="patch")["sid"]
        plan = h.wait(lambda e: e.get("ev") == "mode" and e.get("sid") == sid)["path"]
        h.send("prompt", sid=sid, text="go")
        ended(h, sid, 1)
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["denied", "ok"], f"statuses: {te}")
        t.check("plan mode is on" in te[0]["text"], f"why: {te[0]['text']}")
        t.check(t.file("inside.txt") is None, "nothing written")
        t.check(t.host.read(plan) == "# Patched\n", "the plan file patched")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def plan_task(t):
    """A subagent started in plan mode is in it too: told, without plan
    mode's tools, and refused its write."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ended(h, sid, 1)
        kids = [e for e in h.all("session") if e.get("parent") == sid]
        t.check(len(kids) == 1, f"one subagent: {kids}")
        kid = kids[0]["sid"]
        t.check(modes(h, kid) == ["plan/parent"], f"its mode: {modes(h, kid)}")
        te = h.all("tool.end", kid)
        t.check(te and te[0]["status"] == "denied" and "must not change anything" in te[0]["text"],
                f"refused: {te}")
        t.check(t.file("child.txt") is None, "nothing written")
        req = t.requests(t.model2)[0]["body"]
        names = fake_provider.tool_names(req)
        t.check("exit_plan_mode" not in names and "enter_plan_mode" not in names, f"tools {names}")
        t.check(req["messages"][-1]["content"].startswith("Plan mode is on: the agent you work for"),
                f"told: {req['messages'][-1]}")
        t.check("except to approve a plan" not in req["messages"][0]["content"], "its prompt")
        t.check(lastsaid(h, sid).startswith("Parent heard: Child: Not done"), lastsaid(h, sid))
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# Bypass mode: fake_provider.sc_bypass makes the calls its prompt names
# (do), and the scenarios check what was asked, run and refused

def do(*items):
    return "do " + json.dumps([[name, args] for name, args in items])


def modetold(h, sid):
    """The mode notices a session's agent was given: (mode, text)."""
    return [(e.get("mode"), e["text"]) for e in h.all("notice", sid)
            if e.get("kind") == "mode"]


def beside(t, rel):
    return t.host.read(t.host.join(t.base, rel))


BYPASS_PATCH = ("*** Begin Patch\n*** Add File: ../p1.txt\n+one\n"
                "*** Add File: ../p2.txt\n+two\n*** Add File: inside.txt\n+in\n"
                "*** End Patch\n")


@scenario(bound=15)
def bypass_mode(t):
    """Bypass mode, the user's pick: a read and a write outside the
    working directory and a patch of two files there, none asked about,
    the agent told once that this overrides its system prompt; then back
    in Build, told so, and a write outside asks again."""
    t.host.write(t.host.join(t.base, "in.txt"), "secret\n")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1,
                     edit_format="patch")["sid"]
        r = h.call("mode", sid=sid, mode="bypass")
        t.check(r.get("ok") == 1 and r.get("mode") == "bypass", f"on: {r}")
        h.send("prompt", sid=sid, text=do(("read", {"path": "../in.txt"}),
                                          ("write", {"path": "../out.txt", "content": "out"}),
                                          ("edit", {"patch": BYPASS_PATCH})))
        ended(h, sid, 1)
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["ok"] * 3,
                f"all ran: {[(e['status'], e['text'][:80]) for e in te]}")
        t.check("secret" in te[0]["text"], f"read: {te[0]['text']}")
        t.check(not h.all("ask", sid), f"nothing asked: {h.all('ask', sid)}")
        t.check(beside(t, "out.txt") == "out\n" and beside(t, "p1.txt") == "one\n" and
                beside(t, "p2.txt") == "two\n" and t.file("inside.txt") == "in\n",
                "written outside, and inside")
        msgs = t.requests()[0]["body"]["messages"]
        said = msgs[-1].get("content", "")
        t.check(msgs[-1]["role"] == "user" and said.startswith("Bypass mode is on") and
                "overrides what you were told before" in said and
                "state directory" in said and "never the shell" in
                msgs[0]["content"], f"told: {said}")

        # Again: nothing more to tell
        h.send("prompt", sid=sid, text=do(("write", {"path": "../again.txt", "content": "a"})))
        ended(h, sid, 2)
        t.check(beside(t, "again.txt") == "a\n" and not h.all("ask", sid), "again unasked")
        t.check([m for m, _ in modetold(h, sid)] == ["bypass"], f"told once: {modetold(h, sid)}")

        # Build again: told, and asked
        t.check(h.call("mode", sid=sid, mode="default").get("mode") == "default", "off")
        h.send("prompt", sid=sid, text=do(("write", {"path": "../no.txt", "content": "no"})))
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["kind"] == "path" and ask["path"].endswith("no.txt"), f"asked: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="deny")
        ended(h, sid, 3)
        told = modetold(h, sid)
        t.check([m for m, _ in told] == ["bypass", "default"] and
                told[1][1].startswith("Bypass mode is off"), f"told: {told}")
        t.check(beside(t, "no.txt") is None, "nothing written outside")
        t.check(modes(h, sid) == ["bypass/user", "default/user"], f"modes {modes(h, sid)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def bypass_ask(t):
    """Bypass mode picked while a write outside is asked about allows
    it, by "bypass"; an unknown mode is refused."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        r = h.call("mode", sid=sid, mode="yolo")
        t.check(r.get("ok") == 0 and "bypass" in r.get("err", ""), f"refused: {r}")
        h.send("prompt", sid=sid, text=do(("write", {"path": "../asked.txt", "content": "x"})))
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        r = h.call("mode", sid=sid, mode="bypass")
        t.check(r.get("ok") == 1 and r.get("mode") == "bypass", f"on: {r}")
        ended(h, sid, 1)
        ans = h.all("answer", sid)
        t.check([(a["req"], a["decision"], a["by"]) for a in ans] ==
                [(ask["req"], "allow", "bypass")] and "Bypass" in ans[0]["note"],
                f"answered: {ans}")
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["ok"], f"ran: {te}")
        t.check(beside(t, "asked.txt") == "x\n", "written outside")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def bypass_unattended(t):
    """Unattended, in another process (as reopened or revived), Bypass
    mode still allows reads, writes and a patch outside, which would be
    denied by policy; the agent is told once, not again in a later run."""
    t.host.write(t.host.join(t.base, "in.txt"), "secret\n")
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake",
                     edit_format="patch")["sid"]
        t.check(h.call("mode", sid=sid, mode="bypass").get("ok") == 1, "on")
    finally:
        h.close()
    r = t.run(do(("read", {"path": "../in.txt"}),
                 ("write", {"path": "../out.txt", "content": "out"}),
                 ("edit", {"patch": BYPASS_PATCH})), resume=sid)
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    te = r.all("tool.end")
    t.check([e["status"] for e in te] == ["ok"] * 3,
            f"all ran: {[(e['status'], e['text'][:80]) for e in te]}")
    t.check(not r.all("ask"), f"nothing asked: {r.all('ask')}")
    t.check(beside(t, "out.txt") == "out\n" and beside(t, "p1.txt") == "one\n" and
            beside(t, "p2.txt") == "two\n", "written outside")
    r = t.run(do(("write", {"path": "../again.txt", "content": "a"})), resume=sid)
    t.check(r.rc == 0 and r.last("tool.end").get("status") == "ok" and not r.all("ask"),
            f"again: {r.all('tool.end')} {r.stderr}")
    log = logged(t, sid)
    told = [e["mode"] for e in log if e["ev"] == "notice" and e.get("kind") == "mode"]
    t.check(told == ["bypass"], f"told once: {told}")
    t.check([e["by"] for e in log if e["ev"] == "mode"] == ["user"], "one mode record")
    msgs = t.requests()[-1]["body"]["messages"]
    t.check(sum(str(m.get("content", "")).startswith("Bypass mode is on") for m in msgs) == 1,
            "the notice once in the history")


@scenario(conf={"_desktop": {"offer": "all"}}, bound=15)
def bypass_desktop(t):
    """With [desktop] ask on, unattended, Bypass mode runs desktop
    actions unasked, run's among them, which is never granted for good."""
    env = deskenv(t)
    h = t.headless(env=env)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.call("mode", sid=sid, mode="bypass")
        h.send("prompt", sid=sid, text=do(
            ("desktop", {"action": "screenshot"}),
            ("desktop", {"action": "run", "program": "notepad.exe", "args": ["a b"]})))
        ended(h, sid, 1)
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["ok", "ok"],
                f"ran: {[(e['status'], e['text'][:80]) for e in te]}")
        t.check(not h.all("ask", sid), "nothing asked")
        t.check(["run", "notepad.exe", "a b"] in desklog(t), f"the helper took {desklog(t)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def bypass_state(t):
    """Bypass mode refuses writes under the state directory, but for the
    plan file; Build still asks about them, and Plan writes the plan file."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        sdir = t.host.join(t.state, "sessions", sid)
        plan = t.host.join(sdir, "plan.md")
        junk = t.host.join(t.state, "junk.txt")
        h.call("mode", sid=sid, mode="bypass")
        h.send("prompt", sid=sid, text=do(
            ("write", {"path": junk, "content": "junk"}),
            ("write", {"path": plan, "content": "# Plan"}),
            ("write", {"path": t.host.join(sdir, "events.jsonl"), "content": "{}"}),
            ("read", {"path": t.host.join(sdir, "events.jsonl")})))
        ended(h, sid, 1)
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["denied", "ok", "denied", "ok"],
                f"statuses: {[(e['status'], e['text'][:80]) for e in te]}")
        t.check("under the harness's state directory" in te[0]["text"] and
                "Write somewhere else" in te[0]["text"], f"why: {te[0]['text']}")
        t.check(not h.all("ask", sid), "nothing asked")
        t.check(t.host.read(junk) is None, "nothing written there")
        t.check(t.host.read(plan) == "# Plan\n", "the plan file written")
        t.check(logged(t, sid)[0]["ev"] == "session", "the log intact")

        # Build asks, as before
        h.call("mode", sid=sid, mode="default")
        h.send("prompt", sid=sid, text=do(("write", {"path": junk, "content": "junk"})))
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["kind"] == "path" and ask["path"].endswith("junk.txt"), f"asked: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="deny")
        ended(h, sid, 2)

        # Plan writes its plan file, as named to the agent
        h.call("mode", sid=sid, mode="plan")
        named = h.all("mode", sid)[-1]["path"]
        h.send("prompt", sid=sid, text=do(("write", {"path": named, "content": "# Plan 2"})))
        ended(h, sid, 3)
        t.check(h.all("tool.end", sid)[-1]["status"] == "ok" and
                t.host.read(plan) == "# Plan 2\n", "the plan file, in plan mode")
        t.check(len(h.all("ask", sid)) == 1, "asked once in all")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def bypass_fork(t):
    """A fork of a session in Bypass mode starts in Build, by "fork", its
    agent told, and asks; an import has no mode records, so Build too."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.call("mode", sid=sid, mode="bypass")
        h.send("prompt", sid=sid, text=do(("write", {"path": "../a.txt", "content": "a"})))
        ended(h, sid, 1)
        r = h.call("fork", sid=sid)
        t.check(r.get("ok") == 1, f"forked: {r}")
        fk = r["sid"]
        fm = [f"{e['mode']}/{e['by']}" for e in logged(t, fk) if e["ev"] == "mode"]
        t.check(fm == ["bypass/user", "default/fork"], f"the fork's modes: {fm}")
        t.check(h.call("open", sid=fk, attended=1).get("ok") == 1, "opened")
        h.send("prompt", sid=fk, text=do(("write", {"path": "../b.txt", "content": "b"})))
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == fk)
        h.call("answer", sid=fk, req=ask["req"], decision="deny")
        ended(h, fk, 2)
        told = modetold(h, fk)  # the original's, then its own
        t.check([m for m, _ in told] == ["bypass", "default"] and
                told[1][1].startswith("Bypass mode is off"), f"told: {told}")
        t.check(beside(t, "b.txt") is None, "nothing written")
        t.check(modes(h, sid) == ["bypass/user"], "the original stays in Bypass")

        raw = t.host.read(t.host.join(t.state, "sessions", sid, "events.jsonl"))
        r = importlog(t, h, raw, model=t.model, endpoint="fake")
        t.check(r.get("ok") == 1, f"imported: {r}")
        t.check(not [e for e in logged(t, r["sid"]) if e["ev"] == "mode"], "no mode records")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=25)
def bypass_task(t):
    """A subagent follows its parent's mode as it is now: in Bypass, it
    writes outside unasked, told so; its own mode cannot be set; the
    parent back in Build while it is idle, a message wakes it, told, and
    its write asks; the parent in Bypass again allows that ask."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.call("mode", sid=sid, mode="bypass")
        task = {"description": "Write", "model": t.model2,
                "prompt": do(("write", {"path": "../kid1.txt", "content": "1"}))}
        h.send("prompt", sid=sid, text=do(("task", task)))
        ended(h, sid, 1)
        kids = [e["sid"] for e in h.all("session") if e.get("parent") == sid]
        t.check(len(kids) == 1, f"one subagent: {kids}")
        kid = kids[0]
        t.check(beside(t, "kid1.txt") == "1\n" and not h.all("ask", kid), "written unasked")
        told = modetold(h, kid)
        t.check([m for m, _ in told] == ["bypass"] and
                told[0][1].startswith("Bypass mode is on: the user allows the agent you work for"),
                f"told: {told}")
        t.check(not modes(h, kid), f"no mode of its own: {modes(h, kid)}")
        r = h.call("mode", sid=kid, mode="bypass")
        t.check(r.get("ok") == 0 and "parent's" in r.get("err", ""), f"refused: {r}")

        # Build while it is idle, then a message wakes it
        h.call("mode", sid=sid, mode="default")
        msg = do(("write", {"path": "../kid2.txt", "content": "2"}))
        h.send("prompt", sid=sid, text=do(("send_message", {"to": kid, "text": msg})))
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == kid)
        told = modetold(h, kid)
        t.check([m for m, _ in told] == ["bypass", "default"], f"told: {told}")
        r = h.call("mode", sid=sid, mode="bypass")
        t.check(r.get("ok") == 1, f"on again: {r}")
        ans = h.wait(lambda e: e.get("ev") == "answer" and e.get("sid") == kid)
        t.check(ans["req"] == ask["req"] and ans["decision"] == "allow" and ans["by"] == "bypass",
                f"answered: {ans}")
        turnend(h, kid, 2)
        t.check(beside(t, "kid2.txt") == "2\n", "written")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def bypass_plan(t):
    """In Bypass mode the agent's plan asks are still asked, and an
    approved plan goes to Build."""
    h = t.headless()
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.call("mode", sid=sid, mode="bypass")
        h.send("prompt", sid=sid, text="Get to work.")  # and read the notice
        ended(h, sid, 1)
        h.send("prompt", sid=sid, text="Plan first.")
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["kind"] == "plan.enter", f"asked: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        plan = planask(h, sid)
        h.call("answer", sid=sid, req=plan["req"], decision="allow")
        ended(h, sid, 2)
        t.check(modes(h, sid) == ["bypass/user", "plan/agent", "default/approved"],
                f"modes {modes(h, sid)}")
        te = h.all("tool.end", sid)
        t.check(te[-1]["status"] == "ok" and te[-1]["text"].startswith("Replaced"),
                f"carried out: {te[-1]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# MCP servers, with tests/fake_mcp.py as the server "fake" (on this
# machine only, as the fake desktop helper)

FAKE_MCP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_mcp.py")


def mcpconf(**kw):
    # Single quotes: a value that begins with a double quote is the INI's
    sec = {"command": f"'{sys.executable}' '{FAKE_MCP}'", **kw}
    return {"_mcp fake": {k: v for k, v in sec.items() if v is not None}}


def mcpenv(t, **kw):
    if not isinstance(t.host, LocalHost):
        raise Skip("the fake MCP server runs on this machine only")
    t.mcplog = t.host.join(t.base, "mcp.log")
    return {"FAKE_MCP_LOG": t.mcplog, **kw}


def mcplog(t):
    text = t.host.read(t.mcplog) or ""
    return [json.loads(x) for x in text.splitlines()]


def mcpcalls(t):
    return [m["params"] for m in mcplog(t) if m.get("method") == "tools/call"]


def tooldef(req, name):
    for x in req.get("tools", []):
        if x["function"]["name"] == name:
            return x["function"]
    return None


@scenario(conf=mcpconf(trust="yes", **{"env.FAKE_MCP_WORD": "env:MCP_WORD"}), bound=20)
def mcp_flow(t):
    """A trusted server's tools, offered beside the built-ins, called with
    no ask: text, an image for the next request, a failure, structured
    content, a refusal, big text spilled, and where it runs, with the
    section's environment. The protocol as it went: initialize, the
    tools listed page by page, the server's ping answered and its other
    request refused."""
    r = t.run(env=mcpenv(t, MCP_WORD="hello"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {r.stderr}")
    t.check(not r.all("ask"), "nothing asked: trusted")
    ends = r.all("tool.end")
    st = [(e["status"], e["text"][:70]) for e in ends]
    t.check([s for s, _ in st] == ["ok", "ok", "error", "ok", "error", "ok", "ok"],
            f"statuses {st}")
    t.check(ends[0]["text"] == "echo: hi", f"echo {ends[0]['text']!r}")
    t.check(ends[1]["text"].startswith("A red square.") and
            ends[1].get("image", "").endswith(".png"), f"image {ends[1]}")
    t.check(ends[2]["text"] == "it went wrong", f"fail {ends[2]['text']!r}")
    t.check(json.loads(ends[3]["text"]) == {"answer": 42}, f"structured {ends[3]['text']!r}")
    t.check("refused the call" in ends[4]["text"] and "no, thanks" in ends[4]["text"],
            f"refuse {ends[4]['text']!r}")
    t.check(ends[5].get("spill", "").startswith("out/") and len(ends[5]["text"]) < 60000,
            f"big spilled: {ends[5].get('spill')} {len(ends[5]['text'])}")
    where = ends[6]["text"].split("\n")
    t.check(os.path.realpath(where[0]) == os.path.realpath(t.work) and where[1] == "hello",
            f"where {where}")

    reqs = [q["body"] for q in t.requests()]
    names = fake_provider.tool_names(reqs[0])
    want = ["mcp__fake__" + x for x in ("echo", "image", "big", "slow", "fail", "refuse",
                                       "crash", "structured", "where")]
    t.check(names[-len(want):] == want and "read" in names, f"offered {names}")
    echo = tooldef(reqs[0], "mcp__fake__echo")
    t.check(echo["description"] == "(The MCP server fake) Repeat the text." and
            "$schema" not in echo["parameters"] and echo["parameters"]["required"] == ["text"],
            f"echo's definition {echo}")
    img = tooldef(reqs[0], "mcp__fake__image")
    t.check(img["parameters"] == {"type": "object", "properties": {}}, f"image's {img}")
    sysp = fake_provider.system_of(reqs[0])
    t.check("# MCP server instructions" in sysp and "## fake" in sysp and
            "Use echo to repeat things." in sysp, "the server's instructions")
    t.check(len(fake_provider.desk_images(reqs[2])) == 1, "the image reached the next request")
    for a, b in zip(reqs, reqs[1:]):
        t.check(a["tools"] == b["tools"], "the tools hold still")

    log = mcplog(t)
    methods = [m.get("method") for m in log]
    t.check(methods[:2] == ["initialize", "notifications/initialized"] and
            methods.count("tools/list") == 5, f"methods {methods}")
    init = log[0]["params"]
    t.check(init["protocolVersion"] == "2025-06-18" and
            init["clientInfo"]["name"] == "prospero", f"initialize {init}")
    resp = {m["id"]: m for m in log if "method" not in m}
    t.check(resp.get("s1", {}).get("result") == {} and
            resp.get("s2", {}).get("error", {}).get("code") == -32601,
            f"answers to the server: {resp}")
    calls = mcpcalls(t)
    t.check(calls[0] == {"name": "echo", "arguments": {"text": "hi"}}, f"calls {calls[:1]}")


@scenario(conf=mcpconf(), bound=15)
def mcp_policy(t):
    """Untrusted, unattended: the call is asked, denied by policy, and
    never reaches the server."""
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ask = r.last("ask")
    t.check(ask.get("kind") == "mcp" and ask.get("tool") == "mcp__fake__echo" and
            ask.get("server") == "fake" and ask.get("scope") == "mcp" and
            ask.get("always") == "mcp__fake__echo" and
            ask.get("detail") == 'MCP fake: echo {"text": "hi"}', f"ask {ask}")
    ans = r.last("answer")
    t.check(ans.get("decision") == "deny" and ans.get("by") == "policy", f"answer {ans}")
    end = r.last("tool.end")
    t.check(end["status"] == "denied" and "need the user's approval" in end["text"],
            f"denied {end}")
    t.check(not mcpcalls(t), "nothing called")


@scenario(conf=mcpconf(command="/nonexistent/mcp-server"), bound=15)
def mcp_broken(t):
    """A server that cannot start: the session says so once and goes on
    without its tools."""
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    warns = [n["text"] for n in r.all("note") if "MCP server fake failed" in n["text"]]
    t.check(len(warns) == 1, f"warned once: {[n['text'] for n in r.all('note')]}")
    req = t.requests()[0]["body"]
    t.check(not [x for x in fake_provider.tool_names(req) if x.startswith("mcp__")],
            "nothing offered")
    end = r.last("tool.end")
    t.check(end["status"] == "error", f"an unknown tool {end}")


@scenario(conf=mcpconf(enabled="no"), bound=10)
def mcp_off(t):
    """A disabled server is not started or offered."""
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    req = t.requests()[0]["body"]
    t.check(not [x for x in fake_provider.tool_names(req) if x.startswith("mcp__")],
            "nothing offered")
    t.check(not mcplog(t), "never started")


@scenario(conf=mcpconf(tools="echo, image"), bound=15)
def mcp_plan(t):
    """The tools key offers only those named; in plan mode a tool that
    does not say it only reads is refused, and one that does is asked."""
    h = t.headless(env=mcpenv(t))
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1,
                     attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = nextask(h, sid)
        t.check(ask["tool"] == "mcp__fake__echo", f"echo asked {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        ended(h, sid, 1)
        st = [(e["status"], e["text"][:60]) for e in h.all("tool.end", sid)]
        t.check([s for s, _ in st[:3]] == ["ok", "denied", "error"], f"statuses {st}")
        t.check("plan mode" in st[1][1], f"why {st[1]}")
        names = fake_provider.tool_names(t.requests()[0]["body"])
        t.check([x for x in names if x.startswith("mcp__")] ==
                ["mcp__fake__echo", "mcp__fake__image"], f"offered {names}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# A long path to /bin/sh: reading the prefix again once stat a path
# that long over the record's own parse, losing its MCP tools
LONGSH = {"_tools": {"shell": "/bin" + "/../bin"*48 + "/sh"}} if os.name != "nt" else {}


@scenario(conf={**mcpconf(), **LONGSH}, bound=30)
def mcp_ask(t):
    """Attended: a call allowed once, then always for that tool, which
    stops its asks, but not another tool's, denied with a note. Another
    process keeps the grant and the tools, from the session's prefix,
    whatever its shell's path."""
    env = mcpenv(t)
    h = t.headless(env=env)
    b = None
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        one = nextask(h, sid)
        h.call("answer", sid=sid, req=one["req"], decision="allow")
        two = nextask(h, sid, one["seq"])
        t.check(two["detail"] == 'MCP fake: echo {"text": "two"}', f"two {two}")
        r = h.call("answer", sid=sid, req=two["req"], decision="always")
        t.check(r.get("ok") == 1, f"always {r}")
        img = nextask(h, sid, two["seq"])
        t.check(img["tool"] == "mcp__fake__image" and img["detail"] == "MCP fake: image",
                f"the image asked, three not: {img}")
        h.call("answer", sid=sid, req=img["req"], decision="deny", note="no pictures")
        t.check(ended(h, sid, 1)["why"] == "done", "the first turn ends")
        st = [(e["status"], e["text"][:60]) for e in h.all("tool.end", sid)]
        t.check([s for s, _ in st] == ["ok", "ok", "ok", "denied"], f"statuses {st}")
        t.check("no pictures" in h.all("tool.end", sid)[-1]["text"], "the note")
        g = [e for e in logged(t, sid) if e["ev"] == "grant"]
        t.check(len(g) == 1 and g[0].get("mcp") == "mcp__fake__echo" and g[0]["by"] == "user",
                f"the grant {g}")
        pre = [e for e in logged(t, sid) if e["ev"] == "prefix"]
        t.check(pre and [x["name"] for x in pre[-1].get("mcp", [])][:2] ==
                ["mcp__fake__echo", "mcp__fake__image"], f"the prefix's table {pre[-1:]}")
        t.check(h.close() == 0, "the first exits")
        h = None

        b = t.headless(env=env)
        o = b.call("open", sid=sid, attended=1)
        t.check(o.get("ok") == 1, f"open {o}")
        b.send("prompt", sid=sid, text="again")
        fail = nextask(b, sid, max(e["seq"] for e in logged(t, sid)))
        t.check(fail["tool"] == "mcp__fake__fail", f"only fail asks: {fail}")
        b.call("answer", sid=sid, req=fail["req"], decision="allow")
        t.check(ended(b, sid, 2)["why"] == "done", "the second turn ends")
        texts = [c["arguments"].get("text") for c in mcpcalls(t) if c["name"] == "echo"]
        t.check(texts == ["one", "two", "three", "four"], f"echoed {texts}")
        reqs = [q["body"] for q in t.requests()]
        t.check(reqs[0]["tools"] == reqs[-1]["tools"], "the same tools in the later process")
        inits = [m for m in mcplog(t) if m.get("method") == "initialize"]
        t.check(len(inits) == 2, f"a server for each process: {len(inits)}")
    finally:
        rc = h.close() if h else 0
        rc2 = b.close() if b else 0
    t.check(rc == 0 and rc2 == 0, f"headless exit {rc} {rc2}")


@scenario(conf=mcpconf(trust="yes", timeout=1), bound=15)
def mcp_timeout(t):
    """A call past the section's timeout is cancelled, and the server told."""
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.last("tool.end")
    t.check(end["status"] == "timeout" and "did not answer within" in end["text"],
            f"timed out {end}")
    can = [m for m in mcplog(t) if m.get("method") == "notifications/cancelled"]
    call = [m for m in mcplog(t) if m.get("method") == "tools/call"]
    t.check(len(can) == 1 and can[0]["params"]["requestId"] == call[0]["id"],
            f"cancelled {can}")


@scenario(conf=mcpconf(trust="yes"), bound=15)
def mcp_cancel(t):
    """A turn cancelled during a call tells the server."""
    h = t.headless(env=mcpenv(t))
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="go")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        deadline = time.monotonic() + 10
        while not mcpcalls(t) and time.monotonic() < deadline:
            time.sleep(0.05)
        h.call("cancel", sid=sid)
        end = ended(h, sid, 1)
        t.check(end["why"] == "cancel", f"cancelled {end}")
        te = h.all("tool.end", sid)
        t.check(te and te[-1]["status"] == "cancelled", f"the call {te}")
        deadline = time.monotonic() + 5
        while (not [m for m in mcplog(t) if m.get("method") == "notifications/cancelled"]
               and time.monotonic() < deadline):
            time.sleep(0.05)
        can = [m for m in mcplog(t) if m.get("method") == "notifications/cancelled"]
        t.check(len(can) == 1 and can[0]["params"]["reason"] == "the user cancelled",
                f"told {can}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=mcpconf(trust="yes"), bound=20)
def mcp_crash(t):
    """A server that exits fails its call with what it wrote to standard
    error, and the next call too while it may not start again; after
    that, a call starts it again."""
    r = t.run(env=mcpenv(t, PROSPERO_TEST_MINUTE_MS="2000"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [(e["status"], e["text"]) for e in r.all("tool.end")]
    t.check([s for s, _ in st] == ["error", "error", "ok"], f"statuses {st}")
    t.check("it exited" in st[0][1] and "crashing as asked" in st[0][1], f"crash {st[0]}")
    t.check("failed, so the call was not made" in st[1][1], f"too soon {st[1]}")
    t.check(st[2][1] == "echo: again", f"again {st[2]}")
    inits = [m for m in mcplog(t) if m.get("method") == "initialize"]
    t.check(len(inits) == 2, f"started twice: {len(inits)}")


@scenario(conf={**mcpconf(trust="yes"),
                "_extra": "[role picky]\ndescription = Picks\ntools = read, mcp__fake__echo\n"},
          bound=20)
def mcp_subagent(t):
    """Subagents have their parent's MCP tools, a role with a tools list
    only those it names, and they share the parent's server."""
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    sid, _ = r.sessions()
    said = [report(e["text"]) for e in r.all("tool.end", sid)]
    every = ",".join("mcp__fake__" + x for x in ("echo", "image", "big", "slow", "fail",
                                                  "refuse", "crash", "structured", "where"))
    t.check(said == [every, "mcp__fake__echo"], f"offered: {said}")
    texts = sorted(c["arguments"]["text"] for c in mcpcalls(t))
    t.check(texts == ["general", "picky"], f"both called it: {texts}")
    inits = [m for m in mcplog(t) if m.get("method") == "initialize"]
    t.check(len(inits) == 1, f"one server for all: {len(inits)}")


def mcpproject(t, rel=".mcp.json", **extra):
    """A .mcp.json naming the fake server as "proj", with extra servers."""
    servers = {"proj": {"command": sys.executable,
                        "args": [FAKE_MCP, "${NOT_SET_ANYWHERE:-x}"],
                        "env": {"FAKE_MCP_WORD": "${MCP_WORD}"}}, **extra}
    t.put(rel, json.dumps({"mcpServers": servers}))


def projcalls(h, sid):
    return [(e["status"], e["text"]) for e in h.all("tool.end", sid)]


@scenario(bound=15)
def mcp_project_policy(t):
    """Unattended, a project's servers are asked about, denied by policy
    with a warning, never started, and their tools not offered; one of a
    transport not supported is left out, with a warning of its own."""
    mcpproject(t, old={"type": "sse", "url": "http://127.0.0.1:1/sse"})
    r = t.run(env=mcpenv(t, MCP_WORD="w"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ask = r.last("ask")
    t.check(ask.get("kind") == "mcp.project" and ask.get("scope") == "mcp.project" and
            ask.get("path", "").endswith(".mcp.json") and "call" not in ask and
            ask.get("detail", "").startswith("proj: ") and FAKE_MCP in ask.get("detail", ""),
            f"ask {ask}")
    ans = r.last("answer")
    t.check(ans.get("decision") == "deny" and ans.get("by") == "policy", f"answer {ans}")
    warns = [n["text"] for n in r.all("note") if n.get("level") == "warn"]
    t.check(any("SSE transport" in w for w in warns) and
            any("were not started" in w for w in warns), f"warnings {warns}")
    names = fake_provider.tool_names(t.requests()[0]["body"])
    t.check(not [x for x in names if x.startswith("mcp__")], f"nothing offered {names}")
    t.check(not mcplog(t), "never started")


@scenario(bound=30)
def mcp_project(t):
    """Attended: allowed for the session, the server runs in the folder of
    its .mcp.json (the repository's root, above the session's folder),
    with the file's environment expanded, and its calls ask as any MCP
    call. Another process keeps the approval, from the session's log."""
    t.host.mkdir(t.host.join(t.work, ".git"))
    t.host.mkdir(t.host.join(t.work, "sub"))
    mcpproject(t)
    env = mcpenv(t, MCP_WORD="expanded")
    sub = t.host.join(t.work, "sub")
    h = t.headless(env=env)
    b = None
    try:
        sid = h.call("new", cwd=sub, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = nextask(h, sid)
        t.check(ask["kind"] == "mcp.project", f"the project's ask first: {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        for _ in range(2):
            a = nextask(h, sid, ask["seq"])
            t.check(a["kind"] == "mcp" and a["server"] == "proj", f"a call's ask {a}")
            h.call("answer", sid=sid, req=a["req"], decision="allow")
            ask = a
        t.check(ended(h, sid, 1)["why"] == "done", "the first turn ends")
        st = projcalls(h, sid)
        t.check([x for x, _ in st] == ["ok", "ok"], f"statuses {st}")
        where = st[0][1].split("\n")
        t.check(os.path.realpath(where[0]) == os.path.realpath(t.work) and
                where[1] == "expanded", f"where {where}")
        g = [e for e in logged(t, sid) if e["ev"] == "grant"]
        t.check(len(g) == 1 and g[0].get("mcp", "").startswith("project:"), f"grant {g}")
        t.check(not os.path.exists(os.path.join(t.state, "mcp", "projects.json")),
                "nothing kept for later sessions")
        t.check(h.close() == 0, "the first exits")
        h = None

        b = t.headless(env=env)
        t.check(b.call("open", sid=sid, attended=1).get("ok") == 1, "open")
        b.send("prompt", sid=sid, text="again")
        a = nextask(b, sid, max(e["seq"] for e in logged(t, sid)))
        t.check(a["kind"] == "mcp", f"only the call asks: {a}")
        b.call("answer", sid=sid, req=a["req"], decision="allow")
        t.check(ended(b, sid, 2)["why"] == "done", "the second turn ends")
    finally:
        rc = h.close() if h else 0
        rc2 = b.close() if b else 0
    t.check(rc == 0 and rc2 == 0, f"headless exit {rc} {rc2}")


@scenario(bound=30)
def mcp_project_always(t):
    """Always holds for later sessions in the folder, until the file
    changes the server; deny holds for the session."""
    mcpproject(t)
    env = mcpenv(t, MCP_WORD="w")
    h = t.headless(env=env)
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ask = nextask(h, sid)
        r = h.call("answer", sid=sid, req=ask["req"], decision="always")
        t.check(r.get("ok") == 1, f"always {r}")
        h.call("cancel", sid=sid)
        ended(h, sid, 1)
        kept = json.loads(t.host.read(t.host.join(t.state, "mcp", "projects.json")))
        t.check([x["name"] for x in kept["approved"]] == ["proj"], f"kept {kept}")

        # A new session: no ask for the server
        s2 = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=s2, text="go")
        a = nextask(h, s2)
        t.check(a["kind"] == "mcp", f"a later session asks only for calls: {a}")
        h.call("cancel", sid=s2)
        ended(h, s2, 1)

        # The file changes the server: asked again, and denied
        mcpproject(t, more={"command": sys.executable, "args": [FAKE_MCP]})
        servers = json.loads(t.file(".mcp.json"))["mcpServers"]
        servers["proj"]["args"].append("--changed")
        t.put(".mcp.json", json.dumps({"mcpServers": servers}))
        s3 = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=s3, text="go")
        a = nextask(h, s3)
        t.check(a["kind"] == "mcp.project" and a["detail"].count("\n") == 1 and
                "--changed" in a["detail"], f"asked again for both: {a}")
        h.call("answer", sid=s3, req=a["req"], decision="deny")
        t.check(ended(h, s3, 1)["why"] == "done", "the turn goes on without them")
        names = fake_provider.tool_names(t.requests()[-1]["body"])
        t.check(not [x for x in names if x.startswith("mcp__")], f"none offered {names}")
        h.send("prompt", sid=s3, text="again")
        t.check(ended(h, s3, 2)["why"] == "done", "the second turn")
        t.check(len([e for e in h.all("ask", s3) if e["kind"] == "mcp.project"]) == 1,
                "not asked again in the session")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=mcpconf(trust="yes"), bound=15)
def mcp_project_config(t):
    """A configuration section of the same name wins over the project's
    server, with no ask."""
    mcpproject(t, proj={"command": "/nonexistent/server"})
    servers = json.loads(t.file(".mcp.json"))["mcpServers"]
    servers["fake"] = servers.pop("proj")
    t.put(".mcp.json", json.dumps({"mcpServers": servers}))
    r = t.run(env=mcpenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    t.check(not r.all("ask"), f"nothing asked: {r.all('ask')}")
    names = fake_provider.tool_names(t.requests()[0]["body"])
    t.check("mcp__fake__echo" in names, f"the configuration's server {names}")


class McpHttpServer:
    """tests/fake_mcp.py --http, run for a scenario, logging to t.mcplog."""

    def __init__(self, t, oauth=False, **env):
        flags = ["--http"] + (["--oauth"] if oauth else [])
        self.p = subprocess.Popen([sys.executable, FAKE_MCP] + flags,
                                  stdout=subprocess.PIPE, text=True,
                                  env={**os.environ, "FAKE_MCP_LOG": t.mcplog, **env})
        line = self.p.stdout.readline()
        if not line.strip().isdigit():
            self.close()
            raise Failure(f"the fake HTTP server did not start: {line!r}")
        self.url = f"http://127.0.0.1:{int(line)}/mcp"

    def close(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def addconf(t, text):
    t.host.write(t.config, t.host.read(t.config) + text)


@scenario(bound=30)
def mcp_http(t):
    """A server over Streamable HTTP: JSON responses and streams of events,
    its session id on every request after initialize, the protocol
    version and the section's headers; the server's ping answered from
    within a stream; a 503 retried once; a session the server forgot
    fails that call, deletes it, and the next call starts again."""
    env = mcpenv(t, EXTRA_VAL="extra!")
    srv = McpHttpServer(t)
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\nheader.X-Extra = env:EXTRA_VAL\ntrust = yes\n")
        r = t.run(env=env)
    finally:
        srv.close()
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {r.stderr}")
    st = [(e["status"], e["text"][:90]) for e in r.all("tool.end")]
    t.check([x for x, _ in st] == ["ok", "ok", "error", "ok", "ok", "error", "ok"],
            f"statuses {st}")
    t.check(st[0][1] == "echo: over http" and st[3][1] == "steady now", f"texts {st}")
    t.check(r.all("tool.end")[1].get("image", "").endswith(".png"), "the image")
    t.check("no, thanks" in st[2][1], f"refused {st[2]}")
    t.check("no longer knows this client's session" in st[5][1], f"forgotten {st[5]}")
    t.check(st[6][1] == "echo: again", f"again {st[6]}")
    log = mcplog(t)
    posts = [m for m in log if "_delete" not in m]
    sids = [m["_http"]["Mcp-Session-Id"] for m in posts if m.get("method") != "initialize"]
    t.check(sids and all(sids), f"a session id on each later request: {sids[:3]}")
    t.check(all(m["_http"]["X-Extra"] == "extra!" for m in posts), "the section's header")
    t.check(all(m["_http"]["MCP-Protocol-Version"] == "2025-06-18" for m in posts
                if m.get("method") not in ("initialize",)), "the protocol version after")
    t.check(all("text/event-stream" in m["_http"]["Accept"] for m in posts), "accepts both")
    pings = [m for m in posts if str(m.get("id", "")).startswith("ping-") and "result" in m]
    t.check(len(pings) == 6, f"pings answered: {len(pings)}")
    flaky = [m for m in posts if (m.get("params") or {}).get("name") == "flaky"]
    t.check(len(flaky) == 2 and flaky[0]["id"] == flaky[1]["id"], f"retried once {flaky}")
    inits = [m for m in posts if m.get("method") == "initialize"]
    t.check(len(inits) == 2, f"initialized again: {len(inits)}")
    t.check(len([m for m in log if "_delete" in m]) == 1, "the forgotten session deleted")


@scenario(bound=15)
def mcp_http_auth(t):
    """A server that wants a token, without one: it fails, saying so."""
    env = mcpenv(t)
    srv = McpHttpServer(t, FAKE_MCP_TOKEN="s3cret")
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\ntrust = yes\n")
        r = t.run(env=env)
    finally:
        srv.close()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    warns = [n["text"] for n in r.all("note") if "MCP server web failed" in n["text"]]
    t.check(len(warns) == 1 and "needs you to sign in" in warns[0] and
            "prospero mcp auth web" in warns[0], f"warned {warns}")


def prun(t, args, env=None, timeout=30):
    """prospero ARGS, run to its end: exit code, output and errors."""
    p = t.host.popen(args, t.work, env=dict(t.tmpenv, **(env or {})))
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    return p.returncode, out, err


def authfiles(t, prefix=""):
    d = os.path.join(t.state, "mcp", "auth")
    names = sorted(os.listdir(d)) if os.path.isdir(d) else []
    return [os.path.join(d, n) for n in names if n.startswith(prefix)]


def mcpauth(t, name, env=None, browse=True):
    """prospero mcp auth NAME, its URL visited as a browser would, the
    redirect to its loopback listener followed: its exit code, output
    and errors."""
    import urllib.request
    p = t.host.popen(["mcp", "auth", name, "--state", t.state, "-c", t.config], t.work,
                     env=dict(t.tmpenv, **(env or {})))
    killer = threading.Timer(30, p.kill)
    killer.start()
    try:
        out = []
        for line in p.stdout:
            out.append(line)
            m = re.search(r"(https?://\S+/authorize\?\S+)", line)
            if m and browse:
                urllib.request.urlopen(m.group(1), timeout=10).read()
                break
        rest, err = p.communicate()
    finally:
        killer.cancel()
    return p.returncode, "".join(out) + rest, err


@scenario(bound=40)
def mcp_oauth(t):
    """Signing in to a server through prospero mcp auth: its protected
    resource's metadata, the authorization server's, a client registered,
    the browser sent to sign in with PKCE and back to the loopback; its
    tokens kept privately, sent with each request; a token the server
    revoked refreshed while the call waits; signed out, the server needs
    signing in again."""
    env = mcpenv(t)
    srv = McpHttpServer(t, oauth=True)
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\ntrust = yes\n")
        rc, out, err = mcpauth(t, "web", env)
        t.check(rc == 0 and "Signed in to web." in out, f"signed in: {rc} {out} {err}")
        files = authfiles(t, "web-")
        t.check(len(files) == 1, f"tokens kept: {files}")
        if files and os.name != "nt":
            mode = os.stat(files[0]).st_mode & 0o777
            t.check(mode == 0o600, f"privately: {oct(mode)}")
        keep = json.loads(open(files[0]).read()) if files else {}
        t.check(keep.get("url") == srv.url and keep.get("refresh_token"), f"kept {keep}")
        _, ls, _ = prun(t, ["mcp", "list", "--state", t.state, "-c", t.config])
        t.check(f"web: {srv.url}, signed in" in ls, f"listed {ls!r}")
        r = t.run(env=env)
        rc, out, err = prun(t, ["mcp", "logout", "web", "--state", t.state, "-c", t.config])
        t.check(rc == 0 and "Signed out of web." in out, f"signed out: {rc} {out} {err}")
        t.check(not authfiles(t), "forgotten")
    finally:
        srv.close()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    st = [(e["status"], e["text"][:90]) for e in r.all("tool.end")]
    t.check(st == [("ok", "echo: one"), ("ok", "expired"), ("ok", "echo: two")],
            f"calls {st}")
    log = mcplog(t)
    oauth = [m["_oauth"] for m in log if "_oauth" in m]
    t.check(oauth[:5] == ["/.well-known/oauth-protected-resource/mcp",
                          "/.well-known/oauth-authorization-server/as", "/as/register",
                          "/as/authorize", "/as/token"], f"the flow {oauth}")
    grants = [m.get("grant_type") for m in log if m.get("_oauth") == "/as/token"]
    t.check(grants == ["authorization_code", "refresh_token"], f"grants {grants}")
    reg = [m for m in log if m.get("_oauth") == "/as/register"]
    t.check(reg and reg[0]["redirect_uris"][0].startswith("http://127.0.0.1:") and
            reg[0]["redirect_uris"][0].endswith("/callback"), f"registered {reg}")
    auths = [m["_http"]["Authorization"] for m in log if "_http" in m and "_delete" not in m]
    t.check(auths and auths[0] == "Bearer " + keep.get("access_token", "?"),
            f"the token sent {auths[:2]}")
    t.check(auths and auths[-1] != auths[0] and auths[-1].startswith("Bearer at-"),
            f"the refreshed token sent {auths[-3:]}")


@scenario(bound=20)
def mcp_oauth_denied(t):
    """A sign-in the user refused, and one never finished elsewhere:
    prospero mcp auth says so; no tokens are kept."""
    mcpenv(t)
    srv = McpHttpServer(t, oauth=True, FAKE_MCP_DENY="1")
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\n")
        rc, out, err = mcpauth(t, "web")
        t.check(rc == 1 and "access_denied" in err, f"refused: {rc} {out} {err}")
        rc, out, err = mcpauth(t, "nosuch")
        t.check(rc == 1 and "no MCP server over HTTP is named nosuch" in err,
                f"unknown: {rc} {err}")
    finally:
        srv.close()
    t.check(not authfiles(t), "nothing kept")


@scenario(bound=15)
def mcp_http_token(t):
    """The section's token, from the environment, as a bearer token."""
    env = mcpenv(t, WEB_TOKEN="s3cret")
    srv = McpHttpServer(t, FAKE_MCP_TOKEN="s3cret")
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\ntoken = env:WEB_TOKEN\ntrust = yes\n")
        r = t.run(env=env)
    finally:
        srv.close()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.last("tool.end")
    t.check(end["status"] == "ok" and end["text"] == "echo: hi", f"called {end}")
    t.check(all(m["_http"]["Authorization"] == "Bearer s3cret" for m in mcplog(t)), "the token")


@scenario(bound=15)
def mcp_http_timeout(t):
    """A call over HTTP past its timeout: its request given up, the
    server told."""
    env = mcpenv(t)
    srv = McpHttpServer(t)
    try:
        addconf(t, f"[mcp web]\nurl = {srv.url}\ntrust = yes\ntimeout = 1\n")
        r = t.run(env=env)
    finally:
        srv.close()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.last("tool.end")
    t.check(end["status"] == "timeout", f"timed out {end}")
    can = [m for m in mcplog(t) if m.get("method") == "notifications/cancelled"]
    t.check(len(can) == 1, f"told {can}")


@scenario(bound=20)
def mcp_project_http(t):
    """A project's HTTP server, with its headers expanded, once allowed."""
    env = mcpenv(t, EXTRA_VAL="from env")
    srv = McpHttpServer(t)
    try:
        t.put(".mcp.json", json.dumps({"mcpServers": {"web": {
            "type": "http", "url": srv.url, "headers": {"X-Extra": "${EXTRA_VAL}"}}}}))
        h = t.headless(env=env)
        try:
            sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
            h.send("prompt", sid=sid, text="go")
            ask = nextask(h, sid)
            t.check(ask["kind"] == "mcp.project" and ask["detail"] == "web: " + srv.url,
                    f"the project's ask {ask}")
            h.call("answer", sid=sid, req=ask["req"], decision="allow")
            call = nextask(h, sid, ask["seq"])
            h.call("answer", sid=sid, req=call["req"], decision="allow")
            t.check(ended(h, sid, 1)["why"] == "done", "the turn ends")
            end = h.all("tool.end", sid)[-1]
            t.check(end["status"] == "ok" and end["text"] == "echo: hi", f"called {end}")
        finally:
            rc = h.close()
        t.check(rc == 0, f"headless exit {rc}")
    finally:
        srv.close()
    t.check(all(m["_http"]["X-Extra"] == "from env" for m in mcplog(t) if "_http" in m),
            "the file's header")


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
        return name, "ok", dt, " ".join([over] + t.notes).strip()
    except Failure as e:
        return name, "FAIL", time.monotonic() - t0, str(e)
    except Skip as e:
        return name, "skip", 0.0, str(e)
    except Exception:
        return name, "FAIL", time.monotonic() - t0, traceback.format_exc()


# M7, from M6's review: a server whose certificate is refused fails the
# turn at once, with the reason, rather than retrying for hours

TLS_CERT = """-----BEGIN CERTIFICATE-----
MIIBejCCASCgAwIBAgIUJCO4zezdv3ZN727dLBcsRdRcQjswCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAyMjEyMDkyMloYDzIxMjYwOTI4
MTIwOTIyWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAARtP2DsWYgmp4laukm0w8WYYWdFlKm8qb6057T23tJjWnkrQ2UNqdB1
98emu5BtgIYi17jn+21QFY5jxNvMRtJTo04wTDAdBgNVHQ4EFgQUM7KDabc0iLEN
7x5lQqgXRX+TxJswDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhv
c3SHBH8AAAEwCgYIKoZIzj0EAwIDSAAwRQIgQuV3bnY7J5FpYJjc2iQAlSNwkMuV
kmJrPKwnp2A7q4kCIQCAtsvWZcYwoARqme6O/rQkoK6pt+BNAOtYhzd+nMV2PQ==
-----END CERTIFICATE-----
"""
# A throwaway key for the certificate above, self-signed, which nothing
# trusts: it exists for this test alone
TLS_KEY = """-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZHK5n3Kl+mrxAoZb
RRMjybAIqkDkxh3zFj7ju2mJAgGhRANCAARtP2DsWYgmp4laukm0w8WYYWdFlKm8
qb6057T23tJjWnkrQ2UNqdB198emu5BtgIYi17jn+21QFY5jxNvMRtJT
-----END PRIVATE KEY-----
"""


class TlsServer:
    """Accepts TLS connections with a certificate nobody trusts, and
    counts the handshakes tried."""

    def __init__(self, base):
        import ssl
        cert = os.path.join(base, "tls.pem")
        with open(cert, "w") as f:
            f.write(TLS_CERT + TLS_KEY)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.tries = 0
        self.stop = False
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while not self.stop:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.tries += 1
            c.settimeout(5)
            try:
                with self.ctx.wrap_socket(c, server_side=True) as s:
                    s.recv(1)
            except OSError:
                pass  # the client refused the certificate
            finally:
                c.close()

    def close(self):
        self.stop = True
        self.sock.close()


@scenario(bound=10)
def tls_refused(t):
    if not isinstance(t.host, LocalHost):
        raise Skip("the TLS server listens on this machine only")
    srv = TlsServer(t.base)
    try:
        t.host.write(t.config, f"[endpoint fake]\nurl = https://127.0.0.1:{srv.port}/\n"
                     "first_byte_timeout = 5\nidle_timeout = 3\nconnect_timeout = 2\n")
        r = t.run(timeout=30)
    finally:
        srv.close()
    end = r.last("turn.end")
    t.check(r.rc == 1 and end.get("why") == "error", f"exit {r.rc}: {end} {r.stderr}")
    t.check("certificate" in end.get("err", "").lower() or
            "security" in end.get("err", "").lower(), f"says why: {end}")
    t.check(not r.all("retry"), f"no retries: {r.all('retry')}")
    t.check(r.elapsed < 5, f"at once: {r.elapsed:.1f} s")
    t.check(srv.tries <= 2, f"tried {srv.tries} times")


# Messages between sessions: sessions and send_message, sc_msg_* in
# fake_provider.py, with the target's sid in the sender's prompt

def msgnew(t, h, model=None, **kw):
    return h.call("new", cwd=t.work, model=model or t.model, endpoint="fake", **kw)["sid"]


def turnend(h, sid, n=None, timeout=20):
    return h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid and
                  (n is None or e.get("turn") == n), timeout)


def sent(h, sid):
    """The send_message results of a session, in order."""
    return [e for e in h.all("tool.end", sid) if "to_title" in e or e.get("fail") == "message"]


def quiet(h, secs=1.0, timeout=30):
    """Wait until no turn runs, and none starts for a while."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        n = len(h.events)
        if h.call("status").get("turns") == 0:
            time.sleep(secs)
            if h.call("status").get("turns") == 0 and \
                    not [e for e in h.events[n:] if e.get("ev") == "turn"]:
                return
        time.sleep(0.1)
    raise Failure("the sessions never went quiet")


@scenario(bound=15)
def msg_busy(t):
    """A message to a session at work is read at its next step, in the
    same turn, labelled as another session's, with how to reply."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2)
        h.send("prompt", sid=b, text="be busy")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == b)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b}")
        turnend(h, a)
        res = sent(h, a)
        t.check(len(res) == 1 and res[0]["status"] == "ok" and res[0]["to"] == b and
                res[0]["delivery"] == "busy" and res[0]["to_title"] == "be busy" and
                "at its next step" in res[0]["text"], f"sent {res}")
        turnend(h, b)
        t.check(len(h.all("turn", b)) == 1, "read in its own turn")
        note = h.all("notice", b)
        t.check(len(note) == 1 and note[0]["kind"] == "message" and note[0]["from"] == a and
                note[0]["hop"] == 1 and note[0]["from_title"] == f"send to: {b}", f"notice {note}")
        text = note[0]["text"]
        t.check(text.startswith(f"Message from another session, {a}") and
                "hello 0 from the sender" in text and "not from the user" in text and
                text.endswith(f"reply with send_message to {a}."), f"text {text!r}")
        last = t.requests(t.model2)[-1]["body"]["messages"]
        t.check(last[-2]["role"] == "tool" and last[-1]["content"] == text,
                f"after the command's result: {last[-2:]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=15)
def msg_wake(t):
    """A message wakes an idle session, its turn via "message" at hop 1;
    a session in plan mode may send one."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2)
        a = msgnew(t, h, plan=1)
        h.send("prompt", sid=a, text=f"send to: {b}")
        turnend(h, b)
        turn = h.all("turn", b)
        t.check(len(turn) == 1 and turn[0].get("via") == "message" and turn[0].get("hop") == 1,
                f"woken {turn}")
        turnend(h, a)
        res = sent(h, a)
        t.check(len(res) == 1 and res[0]["delivery"] == "woke" and
                "started a turn" in res[0]["text"], f"sent {res}")
        msgs = t.requests(t.model2)[0]["body"]["messages"]
        t.check([m["role"] for m in msgs] == ["system", "user"] and
                MSG_MARK in msgs[1]["content"], f"the notice alone: {msgs}")
        t.check(not h.all("user", b), "no user record")
        # The tools are offered, with a line in the rules
        req = t.requests()[0]["body"]
        names = [x["function"]["name"] for x in req["tools"]]
        t.check("sessions" in names and "send_message" in names, f"tools {names}")
        t.check("Messages marked as from another session" in req["messages"][0]["content"],
                "the rules say so")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


MSG_MARK = fake_provider.MSG_MARK


@scenario(bound=20)
def msg_roundtrip(t):
    """A sends, B replies as the notice says, and A reads the reply: in
    its turn still running, or in a turn the reply starts."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b}")
        h.wait(lambda e: e.get("ev") == "assistant" and e.get("sid") == a and
               any(MSG_MARK in x.get("text", "") or "Got back" in x.get("text", "")
                   for x in e.get("blocks", [])))
        quiet(h)
        got = [x["text"] for e in h.all("assistant", a) for x in e["blocks"]
               if x.get("t") == "text" and x["text"].startswith("Got back")]
        t.check(got and f"Message from another session, {b}" in got[0] and "pong" in got[0],
                f"got {got}")
        rb = sent(h, b)
        t.check(len(rb) == 1 and rb[0]["to"] == a and rb[0]["status"] == "ok", f"reply {rb}")
        hops = [e["hop"] for e in h.all("notice") if e.get("kind") == "message"]
        t.check(hops == [1, 2], f"hops {hops}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=40)
def msg_pingpong(t):
    """Two agents that always reply: the message at hop 8 wakes nobody,
    and its sender is told why; the user's prompt starts the count over."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b}")
        last = h.wait(lambda e: e.get("ev") == "tool.end" and e.get("hop") == 8)
        quiet(h)
        woke = [e["hop"] for e in h.all("turn") if e.get("via") == "message"]
        t.check(woke and max(woke) < 8, f"woken at hops {woke}")
        res = sorted(sent(h, a) + sent(h, b), key=lambda e: e["hop"])
        t.check([e["hop"] for e in res] == list(range(1, 9)), f"one chain {res}")
        t.check(all(e["delivery"] != "waiting" for e in res[:7]), f"all woken {res}")
        why = {"waiting": "was not woken", "busy": "Should its turn end first"}
        t.check(why[last["delivery"]] in last["text"], f"told why: {last}")
        notes = [e for e in h.all("notice") if e.get("kind") == "message"]
        t.check([e["hop"] for e in notes] == list(range(1, 8)), f"notices {notes}")
        t.check("a reply would not wake the sender" in notes[-1]["text"],
                f"told as it nears the end: {notes[-1]}")
        # The message left waits for the user's prompt, which resets the count
        n0 = len(h.events)
        h.send("prompt", sid=last["to"], text="go on")
        h.wait(lambda e: e.get("ev") == "tool.end" and e.get("hop") == 8 and e is not last)
        quiet(h)
        later = h.events[n0:]
        hops = [e["hop"] for e in later if e.get("ev") == "notice"]
        t.check(hops == list(range(0, 8)), f"the waiting message, reset, then on: {hops}")
        woke = [e["hop"] for e in later if e.get("ev") == "turn" and e.get("via") == "message"]
        t.check(woke and woke[0] == 1 and max(woke) < 8, f"counted from 0 again: {woke}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def msg_refused(t):
    """Messages to oneself, an archived session, a subagent of one, an
    unknown target, an ambiguous title, and a session another process
    holds are refused, and so is an invalid one; an exact title reaches
    its session."""
    h = t.headless()
    other = None
    try:
        arch = msgnew(t, h, t.model2)
        named = msgnew(t, h, t.model2)
        twin1 = msgnew(t, h, t.model2)
        twin2 = msgnew(t, h, t.model2)
        for sid, title in ((named, "The Named One"), (twin1, "Twin"), (twin2, "Twin")):
            r = h.call("rename", sid=sid, title=title)
            t.check(r.get("ok") == 1, f"renamed {r}")
        r = h.call("archive", sid=arch, on=1)
        t.check(r.get("ok") == 1, f"archived {r}")
        other = t.headless()
        held = other.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        a = msgnew(t, h)
        targets = [a, arch, "nobody-at-all", "Twin", held, "", "The Named One"]
        h.send("prompt", sid=a, text="send to: " + " | ".join(targets))
        turnend(h, a)
        res = [e for e in h.all("tool.end", a)]
        t.check(len(res) == 7, f"results {res}")
        want = ["your own session", "is archived", "no session has the id or title",
                "2 sessions have that title", "open in another process",
                "Invalid arguments", None]
        for e, w in zip(res, want):
            if w:
                t.check(e["status"] == "error" and w in e["text"] and
                        "to" not in e, f"refused, {w}: {e}")
        last = res[-1]
        t.check(last["status"] == "ok" and last["to"] == named and
                last["to_title"] == "The Named One" and last["delivery"] == "woke",
                f"by its title {last}")
        turnend(h, named)
        t.check(not h.all("notice", arch) and not h.all("turn", arch), "nothing archived")
        t.check(not other.all("notice", held), "nothing to the other process's")
        t.check([e["fail"] for e in res[:6]] == ["message"] * 5 + ["args"], f"fail {res}")
    finally:
        rc = h.close()
        if other:
            other.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def msg_parked(t):
    """A parked target is revived and woken, and so is one only on disk,
    which a process that ended left; both run unattended."""
    h = t.headless(env=PARK)
    try:
        other = t.headless()
        gone = other.call("new", cwd=t.work, model=t.model2, endpoint="fake")["sid"]
        other.send("prompt", sid=gone, text="earlier work")
        turnend(other, gone)
        t.check(other.close() == 0, "the other process exits")

        b = msgnew(t, h, t.model2, attended=1)
        h.send("prompt", sid=b, text="first")
        turnend(h, b)
        deadline = time.monotonic() + 5
        while not (listentry(h, b) or {}).get("parked") and time.monotonic() < deadline:
            time.sleep(0.2)
        t.check(listentry(h, b).get("parked") == 1, "parked")
        t.check(listentry(h, gone).get("state") == "closed", "on disk only")

        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b} | {gone}")
        for sid in (b, gone):
            end = turnend(h, sid, 2)
            t.check(end["why"] == "done", f"woken {end}")
            turn = h.all("turn", sid)[-1]
            t.check(turn.get("via") == "message" and turn.get("hop") == 1, f"turn {turn}")
        turnend(h, a)
        res = sent(h, a)
        t.check([e["delivery"] for e in res] == ["woke", "woke"], f"sent {res}")
        msgs = t.requests(t.model2)[-1]["body"]["messages"]
        said = [m["role"] for m in msgs if m["role"] != "system"]
        t.check(said == ["user", "assistant", "user"] and MSG_MARK in msgs[-1]["content"],
                f"its history, then the message: {said}")
        seqs = [e["seq"] for e in h.events if e.get("sid") == b and "seq" in e]
        t.check(seqs == sorted(seqs) and len(seqs) == len(set(seqs)), f"seq {seqs}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def msg_siblings(t):
    """Two subagents of one parent: one finds the other with sessions and
    sends it a message, which it reads while at work; the parent's
    sessions lists its subagents as children, and another session as
    other, but not the archived one."""
    h = t.headless()
    try:
        u = msgnew(t, h, t.model2)
        h.call("rename", sid=u, title="Unrelated")
        arch = msgnew(t, h, t.model2)
        h.call("archive", sid=arch, on=1)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text="go")
        turnend(h, a)
        kids = {e["sid"]: e for e in h.all("session") if e.get("parent") == a}
        t.check(len(kids) == 2, f"two subagents {kids}")
        sender = next(s for s in kids if h.all("user", s)[0]["text"] == "You send.")
        receiver = next(s for s in kids if s != sender)
        listing = h.all("tool.end", sender)[0]["text"]
        t.check(listing.startswith(f"You are session {sender} (\"Send\").") and
                f"{receiver}  busy  sibling  " in listing and
                f"{a}  busy  parent  " in listing and f"{u}  idle  other  " in listing and
                arch not in listing, f"the sender's listing {listing!r}")
        res = sent(h, sender)
        t.check(len(res) == 1 and res[0]["to"] == receiver and res[0]["delivery"] == "busy",
                f"sent {res}")
        note = h.all("notice", receiver)
        t.check(len(note) == 1 and note[0]["from"] == sender and note[0]["hop"] == 1, f"{note}")
        ends = h.all("tool.end", a)
        reports = " ".join(e["text"] for e in ends)
        t.check("Receiver report: Message from another session" in reports and
                "hi sibling" in reports, f"the receiver read it: {reports!r}")
        mine = ends[-1]["text"]
        t.check(all(f"{k}  idle  child  " in mine for k in kids) and
                f"{u}  idle  other  " in mine, f"the parent's listing {mine!r}")
        t.check(mine.index("child") < mine.index("other"), "relatives first")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def msg_rate(t):
    """A turn sends at most 20 messages; the target reads them all, the
    ones after its first step in a turn of their own."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b}")
        turnend(h, a)
        res = h.all("tool.end", a)
        t.check(len(res) == 21, f"21 results {len(res)}")
        t.check(all(e["status"] == "ok" for e in res[:20]), "20 sent")
        t.check(res[20]["status"] == "error" and "at most 20" in res[20]["text"] and
                res[20].get("fail") == "message", f"the 21st {res[20]}")
        quiet(h)
        note = h.all("notice", b)
        t.check(len(note) == 20, f"20 notices {len(note)}")
        t.check(all(e.get("via") == "message" and e.get("hop") == 1
                    for e in h.all("turn", b)), "woken by them")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=20)
def msg_ask(t):
    """A message to a session waiting for the user's answer leaves the
    ask alone, unlike a steer: it is read after the answer."""
    h = t.headless()
    try:
        b = msgnew(t, h, t.model2, attended=1)
        h.send("prompt", sid=b, text="write outside")
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == b)
        a = msgnew(t, h)
        h.send("prompt", sid=a, text=f"send to: {b}")
        turnend(h, a)
        res = sent(h, a)
        t.check(len(res) == 1 and res[0]["delivery"] == "busy" and
                "waits for the user to answer" in res[0]["text"], f"sent {res}")
        time.sleep(0.3)
        t.check(not h.all("answer", b) and not h.all("notice", b), "the ask still waits")
        r = h.call("answer", sid=b, req=ask["req"], decision="allow")
        t.check(r.get("ok") == 1, f"answered {r}")
        turnend(h, b)
        ans = h.all("answer", b)
        t.check(len(ans) == 1 and ans[0]["by"] == "user", f"answered by the user {ans}")
        t.check(outside_file(t) == "outside\n", "the write ran")
        msgs = t.requests(t.model2)[-1]["body"]["messages"]
        t.check(msgs[-2]["role"] == "tool" and MSG_MARK in msgs[-1]["content"],
                f"read after the answer {msgs[-2:]}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


# M7: the desktop tool, with tests/fake_desktop.py as the helper (on
# this machine only: the remote hosts have no Python for it)

FAKE_DESKTOP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "fake_desktop.py")
DESKALL = {"_desktop": {"offer": "all", "ask": "false"}}


class Skip(Exception):
    pass


def deskenv(t):
    """The environment that makes the fake the helper, logging to a file."""
    if not isinstance(t.host, LocalHost):
        raise Skip("the fake helper runs on this machine only")
    t.desklog = t.host.join(t.base, "desktop.log")
    return {"PROSPERO_TEST_DESKTOP": json.dumps([sys.executable, FAKE_DESKTOP]),
            "FAKE_DESKTOP_LOG": t.desklog}


def desklog(t):
    """The commands the helper took, each a list of its arguments."""
    text = t.host.read(t.desklog) or ""
    return [json.loads(line) for line in text.splitlines()]


def pngdims(data):
    t_ = data[12:16]
    return (int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")) \
        if data[:8] == b"\x89PNG\r\n\x1a\n" and t_ == b"IHDR" else None


def sessdir(t, r):
    sid, _ = r.sessions()
    return t.host.join(t.state, "sessions", sid)


@scenario(conf={**DESKALL, "_extra": "[model {model}]\nimage_edge = 640\n"}, bound=30)
def desktop_flow(t):
    """Screenshots scaled to the model's limit, points mapped back from
    them, input to the screenshot's window, and the rest of the actions,
    unattended as configured, through one helper for the turn."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {r.stderr}")
    ends = r.all("tool.end")
    t.check([e["status"] for e in ends] == ["ok"] * 10,
            f"all done: {[(e['status'], e['text'][:80]) for e in ends]}")
    t.check(not r.all("ask"), "nothing asked: ask is false")
    got = desklog(t)
    want = [["hello"],
            ["shot", "-s", "640", "1048576", "screen"],
            ["click", "screen", "640", "400"],
            ["type", "active", "hello\n"],
            ["key", "active", "ctrl+s", "esc"],
            ["list"],
            ["shot", "-s", "640", "1048576", "Fake Editor"],
            ["click", "hwnd=0x00000010", "10", "20"],
            ["run", "notepad.exe", "a b"],
            ["wait", "-t", "10", "pid=4242"],
            ["close", "pid=4242"]]
    t.check(got == want, f"the helper took {got}")
    shot = ends[0]
    t.check(shot.get("image", "").endswith(".png") and shot["width"] == 640 and
            shot["height"] == 400 and shot["shot"] == "0 0 1280 800 640 400 0x0",
            f"the screenshot's record: { {k: shot.get(k) for k in ('image', 'width', 'height', 'shot')} }")
    t.check("1280x800 pixels, shown to you scaled down to 640x400" in shot["text"] and
            "x from 0 to 639, y from 0 to 399" in shot["text"], f"text {shot['text']}")
    with open(os.path.join(sessdir(t, r), shot["image"]), "rb") as f:
        t.check(pngdims(f.read()) == (640, 400), "the screenshot is kept as it was sent")
    t.check("Clicked at (320, 200) in the screenshot of the screen" in ends[1]["text"],
            f"click {ends[1]['text']}")
    t.check('id=0x00000010 pid=77 640x480 FakeEdit "hello - Fake Editor"' in ends[4]["text"]
            and "#32768" in ends[4]["text"], f"windows {ends[4]['text']}")
    t.check("process id 4242" in ends[7]["text"], f"run {ends[7]['text']}")
    reqs = [q["body"] for q in t.requests()]
    imgs = fake_provider.desk_images(reqs[1])
    t.check(len(imgs) == 1 and imgs[0][1].startswith("data:image/png;base64,") and
            imgs[0][0].startswith("The screenshot of tool call "), f"the image sent: {imgs[:1]}")
    png = base64.b64decode(imgs[0][1].split(",", 1)[1])
    t.check(pngdims(png) == (640, 400), "the image as scaled")
    msgs = reqs[1]["messages"]
    t.check(msgs[-2]["role"] == "tool" and msgs[-1]["role"] == "user",
            "the image after the tool result, from the user")
    desc = [x for x in reqs[0]["tools"] if x["function"]["name"] == "desktop"]
    t.check(desc and "in pixels of its image" in desc[0]["function"]["description"],
            "offered, with points in pixels")
    t.check("screenshot before acting and after" in desc[0]["function"]["description"],
            "look before and after")
    # Points in a window's screenshot: the latest screenshot was the
    # editor's, so a click went to it by its id, as later requests keep
    t.check(len(fake_provider.desk_images(reqs[-1])) == 2, "both screenshots kept")


@scenario(conf={"_desktop": {"offer": "none"}}, bound=10)
def desktop_off(t):
    """With offer none the tool is offered to nobody, and a call to it is
    an unknown tool's."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    req = t.requests()[0]["body"]
    t.check("desktop" not in fake_provider.tool_names(req), "not offered")
    end = r.last("tool.end")
    t.check(end["status"] == "error" and "not available in this session" in end["text"],
            f"refused {end}")
    t.check(not desklog(t), "no helper started")


@scenario(bound=10)
def desktop_nodisplay(t):
    """Offered by default only where it could work: on X11 and Wayland, not
    to an engine with no display, as one reached over ssh."""
    if not sys.platform.startswith("linux") or not isinstance(t.host, LocalHost):
        raise Skip("a display is X11's or Wayland's: Linux, on this machine")
    r = t.run(env={"DISPLAY": "", "WAYLAND_DISPLAY": ""})
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    req = t.requests()[0]["body"]
    t.check("desktop" not in fake_provider.tool_names(req), "not offered")


@scenario(conf=DESKALL, bound=15)
def desktop_plan(t):
    """In plan mode the desktop tool may only look."""
    h = t.headless(env=deskenv(t))
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", plan=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        ended(h, sid, 1)
        te = h.all("tool.end", sid)
        t.check([e["status"] for e in te] == ["denied", "ok"], f"statuses: {te}")
        t.check("may only look" in te[0]["text"], f"why: {te[0]['text']}")
        t.check(not [c for c in desklog(t) if "click" in json.dumps(c)], "no click made")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(bound=10)
def desktop_policy(t):
    """By default every session is offered the tool, and asks: unattended,
    denied by policy, and the turn goes on."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ask = r.last("ask")
    t.check(ask.get("kind") == "desktop" and ask.get("tool") == "desktop" and
            ask.get("detail") == "desktop: take a screenshot of the screen" and
            ask.get("dflt") == "deny", f"ask {ask}")
    ans = r.last("answer")
    t.check(ans.get("decision") == "deny" and ans.get("by") == "policy", f"answer {ans}")
    end = r.last("tool.end")
    t.check(end["status"] == "denied" and "need the user's approval" in end["text"],
            f"denied {end}")
    t.check(r.names().index("answer") < r.names().index("tool.start"),
            "asked before it starts")
    t.check("Heard: Not done" in r.text(), f"the agent read it: {r.text()!r}")
    t.check(not desklog(t), "no helper started")


@scenario(conf={"_desktop": {"offer": "all", "ask_timeout": 20}}, bound=20)
def desktop_ask(t):
    """Attended: each action is asked, with the configured deadline; one
    allowed runs, one denied with a note tells the agent so."""
    h = t.headless(env=deskenv(t))
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="look")
        ask = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid)
        t.check(ask["kind"] == "desktop" and 19000 <= ask["deadline"] - ask["ts"] <= 21000,
                f"ask {ask}")
        h.call("answer", sid=sid, req=ask["req"], decision="allow")
        ask2 = h.wait(lambda e: e.get("ev") == "ask" and e.get("sid") == sid and
                      e["req"] != ask["req"])
        t.check(ask2["detail"] == "desktop: click at (1, 1) in the screenshot of the screen",
                f"ask {ask2}")
        h.call("answer", sid=sid, req=ask2["req"], decision="deny", note="not there")
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        t.check(end["why"] == "done", f"done {end}")
        st = [(e["status"], e["text"][:60]) for e in h.all("tool.end", sid)]
        t.check([s for s, _ in st] == ["ok", "denied"], f"statuses {st}")
        last = h.all("tool.end", sid)[-1]["text"]
        t.check("the user denied" in last and "not there" in last, f"denial {last}")
        t.check(desklog(t) == [["hello"], ["shot", "-s", "1280", "1048576", "screen"]],
                f"only the allowed one ran: {desklog(t)}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_desktop": {"offer": "all", "ask_timeout": 20},
                "_extra": "[role desk]\ndescription = Types\ntools = desktop\nmodel = {model2}\n"},
          bound=30)
def desktop_always(t):
    """A desktop ask offers its action's class for good: always for input
    stops the asks for typing, clicking and looking, but not for closing
    a window or running a program, whose always is refused; a subagent
    has the grant, logged by "parent", and so does the session in a
    later process."""
    env = deskenv(t)
    h = t.headless(env=env)
    b = None
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake", attended=1)["sid"]
        h.send("prompt", sid=sid, text="go")
        look = nextask(h, sid)
        t.check(look.get("always") == "observe" and look.get("scope") == "desktop" and
                "access" not in look and "path" not in look, f"a look's ask: {look}")
        h.call("answer", sid=sid, req=look["req"], decision="allow")
        typ = nextask(h, sid, look["seq"])
        t.check(typ.get("always") == "input" and typ.get("scope") == "desktop",
                f"typing's ask: {typ}")
        r = h.call("answer", sid=sid, req=typ["req"], decision="always")
        t.check(r.get("ok") == 1, f"answered {r}")
        close = nextask(h, sid, typ["seq"])
        t.check(close.get("always") == "windows" and "to close" in close["detail"],
                f"close's ask: {close}")
        h.call("answer", sid=sid, req=close["req"], decision="allow")
        run = nextask(h, sid, close["seq"])
        t.check("always" not in run and "scope" not in run, f"run's ask offers no always: {run}")
        r = h.call("answer", sid=sid, req=run["req"], decision="always")
        t.check(r.get("ok") == 0 and "offers no always" in r.get("err", ""),
                f"always refused for run: {r}")
        h.call("answer", sid=sid, req=run["req"], decision="deny")
        t.check(ended(h, sid, 1)["why"] == "done", "the first turn ends")
        asks = h.all("ask", sid)
        t.check(len(asks) == 4, f"four asks: {[a['detail'] for a in asks]}")
        ans = h.all("answer", sid)
        t.check([(a["decision"], a.get("always")) for a in ans] ==
                [("allow", None), ("allow", 1), ("allow", None), ("deny", None)],
                f"the answers: {ans}")
        g = [e for e in logged(t, sid) if e["ev"] == "grant"]
        t.check(len(g) == 1 and g[0].get("desktop") == "input" and g[0]["by"] == "user" and
                g[0]["req"] == typ["req"] and "path" not in g[0] and "access" not in g[0],
                f"the grant: {g}")
        st = [e["status"] for e in h.all("tool.end", sid)]
        t.check(st == ["ok"] * 5 + ["denied", "ok"], f"statuses: {st}")

        kid = [s for s in {e.get("sid") for e in h.all("session")} if s and s != sid]
        t.check(len(kid) == 1, f"one subagent: {kid}")
        kid = kid[0]
        kl = logged(t, kid)
        kg = [(e["by"], e.get("desktop")) for e in kl if e["ev"] == "grant"]
        t.check(kg == [("parent", "input")], f"the subagent's grants: {kg}")
        t.check(not [e for e in kl if e["ev"] == "ask"], "the subagent never asked")
        ke = [(e["status"], e["text"][:80]) for e in kl if e["ev"] == "tool.end"]
        t.check([st for st, _ in ke] == ["ok"], f"it typed: {ke}")
        t.check(h.close() == 0, "the first exits")
        h = None

        # Another process: the grant holds for input, and windows still asks
        b = t.headless(env=env)
        o = b.call("open", sid=sid, attended=1)
        t.check(o.get("ok") == 1, f"open {o}")
        b.send("prompt", sid=sid, text="again")
        focus = nextask(b, sid, max(e["seq"] for e in logged(t, sid)))
        t.check("to the front" in focus["detail"] and
                focus.get("always") == "windows", f"only focus asks: {focus}")
        b.call("answer", sid=sid, req=focus["req"], decision="deny")
        t.check(ended(b, sid, 2)["why"] == "done", "the second turn ends")
        typed = [c for c in desklog(t) if c[:1] == ["type"]]
        t.check([c[-1] for c in typed] == ["a", "kid", "b"], f"typed: {typed}")
        acts = [c[0] for c in desklog(t) if c[:1] in (["run"], ["close"], ["focus"])]
        t.check(acts == ["close"], f"closed once, nothing run or focused: {acts}")
    finally:
        rc = h.close() if h else 0
        rc2 = b.close() if b else 0
    t.check(rc == 0 and rc2 == 0, f"headless exit {rc} {rc2}")


@scenario(conf={**DESKALL, "_extra": "[model {model}]\ncoordinates = norm1000\n"
                "image_edge = 320\n"}, bound=20)
def desktop_norm(t):
    """Points in thousandths: described so, mapped to the pixels captured,
    and refused outside 0 to 1000."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    got = desklog(t)
    want = [["hello"], ["shot", "-s", "320", "1048576", "screen"],
            ["click", "screen", "640", "400"],
            ["drag", "screen", "0", "0", "1279", "799"],
            ["wheel", "screen", "320", "600", "-3"]]
    t.check(got == want, f"the helper took {got}")
    ends = r.all("tool.end")
    t.check(ends[2]["status"] == "error" and "x and y go from 0 to 1000" in ends[2]["text"],
            f"outside {ends[2]}")
    t.check("thousandths" in ends[0]["text"], f"shot text {ends[0]['text']}")
    desc = [x for x in t.requests()[0]["body"]["tools"] if x["function"]["name"] == "desktop"]
    t.check("thousandths of its width and height" in desc[0]["function"]["description"],
            "described in thousandths")


@scenario(conf={"_desktop": {"offer": "all", "ask": "false", "images": 1,
                            "images_slack": 0},
                "_extra": "[model {model}]\nimage_edge = 64\n"}, bound=20)
def desktop_images(t):
    """Only the latest screenshots go with a request; a short text naming
    the call stands in for each older one, so the request keeps its
    shape."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    last = t.requests()[-1]["body"]
    imgs = fake_provider.desk_images(last)
    tools = [m for m in last["messages"] if m["role"] == "tool"]
    t.check(len(imgs) == 1 and len(tools) == 3, f"one image of three: {len(imgs)}")
    t.check(tools[2]["tool_call_id"] in imgs[0][0], "the latest one")
    alts = desk_alts(last)
    t.check(alts == [f"[screenshot omitted: {m['tool_call_id']}]" for m in tools[:2]],
            f"the others stand in: {alts}")
    t.check(all(m["content"].startswith("Took a screenshot") for m in tools),
            "the results unchanged")


READIMG = "[desktop]\nimages = 1\nimages_slack = 0\n[model {model}]\nimage_edge = 64\n"


def readimages(t):
    """The work directory's images for read_image, and a text file."""
    t.host.write(t.host.join(t.work, "red.png"), tinypng())
    t.host.write(t.host.join(t.work, "four.gif"), GIF)
    t.host.write(t.host.join(t.work, "wide.bmp"),
                 rgbbmp(200, 100, lambda x, y: (x, y, 7)))
    t.host.write(t.host.join(t.work, "notes.txt"), "plain\n")


@scenario(conf={"_extra": READIMG}, bound=20)
def read_image(t):
    """read shows an image file to a model that takes images: as a PNG,
    scaled to the model's limits, sent as a screenshot is and left out
    as screenshots are ([desktop] images), with a short text naming the
    call in its place; text files read as they always have."""
    readimages(t)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    texts = [e["text"] for e in ends]
    t.check(texts[:3] == ["red.png: a PNG image, 1x1 pixels, shown to you as it is.",
                          "four.gif: a GIF image, 6x4 pixels, shown to you as it is.",
                          "wide.bmp: a BMP image, 200x100 pixels, shown to you scaled "
                          "down to 64x32."], f"results {texts}")
    t.check("plain" in texts[3] and "image" not in ends[3], f"text {ends[3]}")
    t.check([(e.get("width"), e.get("height")) for e in ends[:3]] ==
            [(1, 1), (6, 4), (64, 32)] and all("shot" not in e for e in ends), f"ends {ends}")
    tools = t.requests()[0]["body"]["tools"]
    desc = [x["function"]["description"] for x in tools if x["function"]["name"] == "read"][0]
    t.check("shown as an image" in desc, f"read says so: {desc}")

    # Each image in the request after its read, then only the latest
    reqs = [q["body"] for q in t.requests()]
    first = fake_provider.desk_images(reqs[1])
    t.check(len(first) == 1 and first[0][0].startswith("The image read by tool call ") and
            first[0][1].startswith("data:image/png;base64,"), f"sent {first}")
    gif = base64.b64decode(fake_provider.desk_images(reqs[2])[-1][1].split(",", 1)[1])
    t.check(pngpixels(gif) == (6, 4, [bytes(c for x in range(6) for c in GIFPAL[(x + y) % 4])
                                      for y in range(4)]), "the GIF's pixels")
    last = reqs[-1]
    imgs = fake_provider.desk_images(last)
    calls = [m["tool_call_id"] for m in last["messages"] if m["role"] == "tool"]
    t.check(len(imgs) == 1 and calls[2] in imgs[0][0], f"the latest alone {imgs}")
    t.check(pngdims(base64.b64decode(imgs[0][1].split(",", 1)[1])) == (64, 32), "scaled")
    alts = [p["text"] for m in last["messages"]
            if m["role"] == "user" and isinstance(m["content"], list)
            for p in m["content"] if p["type"] == "text" and p["text"].startswith("[image")]
    t.check(alts == [f"[image omitted: {c}; read the file again to see it]" for c in calls[:2]],
            f"the others stand in: {alts}")


@scenario(conf={"_extra": "[model {model}]\nvision = false\n"}, bound=15)
def read_image_blind(t):
    """A model with vision off is told what an image file is, not shown
    it, and read's description does not offer images."""
    readimages(t)
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "ok" and "image" not in end and
            end["text"].startswith("red.png: a PNG image, 1x1 pixels. This model takes no "
                                   "images"), f"told {end}")
    reqs = [q["body"] for q in t.requests()]
    t.check(not fake_provider.desk_images(reqs[-1]), "no image sent")
    desc = [x["function"]["description"] for x in reqs[0]["tools"]
            if x["function"]["name"] == "read"][0]
    t.check("image" not in desc, f"read's description: {desc}")


def desk_alts(req):
    """The texts in place of screenshots left out, in order."""
    return [p["text"] for m in req["messages"]
            if m["role"] == "user" and isinstance(m["content"], list)
            for p in m["content"]
            if p["type"] == "text" and p["text"].startswith("[screenshot")]


def prefix_breaks(reqs):
    """Requests whose messages do not begin with the previous request's:
    where a prompt cache (llama.cpp's too) stops matching."""
    n = 0
    for a, b in zip(reqs, reqs[1:]):
        n += b["messages"][:len(a["messages"])] != a["messages"]
    return n


@scenario(conf={**DESKALL, "_extra": "[model {model}]\nimage_edge = 64\n"}, bound=30)
def desktop_cache(t):
    """Over twelve screenshots, the default 3 kept with 4 slack: older ones
    leave in two steps, so the prefix changes twice, not at each one."""
    r = t.run(env=dict(deskenv(t), FAKE_DESKTOP_ANIMATE="1"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    reqs = [q["body"] for q in t.requests()]
    t.check(len(reqs) == 13, f"requests {len(reqs)}")
    shown = [len(fake_provider.desk_images(q)) for q in reqs]
    t.check(shown == [0, 1, 2, 3, 4, 5, 6, 3, 4, 5, 6, 3, 4], f"shown {shown}")
    alts = [len(desk_alts(q)) for q in reqs]
    t.check(alts == [0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 8, 8], f"left out {alts}")
    breaks = prefix_breaks(reqs)
    t.check(breaks == 2, f"prefix changes {breaks}")
    t.note(f"prefix changes: {breaks} of 12")


@scenario(conf={"_desktop": {"offer": "all", "ask": "false", "images_slack": 0},
                "_extra": "[model {model}]\nimage_edge = 64\n"}, bound=30)
def desktop_cache_before(t):
    """As desktop_cache, without slack, as before it: the prefix changes
    at each screenshot past the third."""
    r = t.run(env=dict(deskenv(t), FAKE_DESKTOP_ANIMATE="1"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    reqs = [q["body"] for q in t.requests()]
    shown = [len(fake_provider.desk_images(q)) for q in reqs]
    t.check(shown == [0, 1, 2] + [3] * 10, f"shown {shown}")
    breaks = prefix_breaks(reqs)
    t.check(breaks == 9, f"prefix changes {breaks}")
    t.note(f"prefix changes: {breaks} of 12")


@scenario(conf=DESKALL, bound=20)
def desktop_looks(t):
    """Six identical screenshot calls on a screen that changes: looking
    again is progress, so the loop guard lets each run."""
    r = t.run(env=dict(deskenv(t), FAKE_DESKTOP_ANIMATE="1"))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    t.check(len(ends) == 6 and all(e["status"] == "ok" for e in ends),
            f"all six ran: {[(e['status'], e.get('fail')) for e in ends]}")
    t.check(len([c for c in desklog(t) if c[0] == "shot"]) == 6, "six shots")


@scenario(conf=DESKALL, bound=20)
def desktop_looks_still(t):
    """Six identical screenshot calls on a screen that stays put: the
    sixth repeats what five saw, and the loop guard stops it."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = r.all("tool.end")
    got = [(e["status"], e.get("fail")) for e in ends]
    t.check(got == [("ok", None)] * 5 + [("error", "loop")], f"the sixth stopped: {got}")
    t.check(len([c for c in desklog(t) if c[0] == "shot"]) == 5, "five shots")


@scenario(conf=DESKALL, bound=20)
def desktop_crash(t):
    """A helper that dies fails its call, and the next call starts another."""
    r = t.run(env=deskenv(t))
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    ends = [(e["status"], e["text"]) for e in r.all("tool.end")]
    t.check([s for s, _ in ends] == ["ok", "error", "ok", "ok"], f"statuses {ends}")
    t.check("helper stopped" in ends[1][1], f"why {ends[1][1]}")
    hellos = [c for c in desklog(t) if c == ["hello"]]
    t.check(len(hellos) == 2, f"a second helper: {desklog(t)}")
    t.check("Clicked at (5, 5)" in ends[2][1], "the screenshot still counts")


@scenario(conf=DESKALL, bound=15)
def desktop_cancel(t):
    """A cancel while the helper works stops it at once."""
    h = t.headless(env=deskenv(t))
    try:
        sid = h.call("new", cwd=t.work, model=t.model, endpoint="fake")["sid"]
        h.send("prompt", sid=sid, text="type")
        h.wait(lambda e: e.get("ev") == "tool.start" and e.get("sid") == sid)
        time.sleep(0.5)
        t0 = time.monotonic()
        h.call("cancel", sid=sid)
        end = h.wait(lambda e: e.get("ev") == "turn.end" and e.get("sid") == sid)
        dt = time.monotonic() - t0
        t.check(end["why"] == "cancel" and dt < 3, f"cancelled in {dt:.1f} s: {end}")
        te = h.all("tool.end", sid)
        t.check(te and te[-1]["status"] == "cancelled", f"the call: {te}")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf=DESKALL, bound=180)
def desktop_xp(t):
    """The real helper, the engine itself ("prospero desktop"), on Windows
    XP's desktop, with --xp, run by name with PROSPERO_TEST_XP_DESKTOP=1:
    Notepad is started, typed into in Unicode, clicked, captured, and
    closed without saving. The screenshots it sent go to
    PROSPERO_TEST_XP_SHOTS, a directory here, to look at."""
    if not isinstance(t.host, XpHost) or not os.environ.get("PROSPERO_TEST_XP_DESKTOP"):
        raise Skip("drives XP's desktop: needs --xp and PROSPERO_TEST_XP_DESKTOP=1")
    r = t.run(timeout=170)
    ends = [(e["status"], e["text"][:160]) for e in r.all("tool.end")]
    t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {ends}")
    t.check(len(ends) == 13 and all(s == "ok" for s, _ in ends), f"every step: {ends}")
    shots = os.environ.get("PROSPERO_TEST_XP_SHOTS")
    n = 0
    for q in t.requests():
        for label, url in fake_provider.desk_images(q["body"]):
            png = base64.b64decode(url.split(",", 1)[1])
            t.check(pngdims(png), f"a PNG: {label}")
            if shots:
                name = label.split()[-1].rstrip(":")
                with open(os.path.join(shots, f"{name}.png"), "wb") as f:
                    f.write(png)
            n += 1
    t.check(n >= 4, f"screenshots sent: {n}")


# The real helper on X11, where Xvfb is installed: a display of the
# test's own, and tests/xprobe.py's window on it, which logs what it is
# sent, and with _wm, its stand-in window manager, which speaks EWMH

XPROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xprobe.py")


class Xvfb:
    """An X server on a display it picks itself, stopped by its group,
    which its own spawn leads (pids.py)."""

    def __init__(self):
        r, w = os.pipe()
        try:
            self.proc = subprocess.Popen(
                ["Xvfb", "-displayfd", str(w), "-screen", "0", "1024x768x24",
                 "-nolisten", "tcp"],
                pass_fds=(w,), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True)
        finally:
            os.close(w)
        try:
            digits = b""
            deadline = time.monotonic() + 15
            while not digits.endswith(b"\n") and time.monotonic() < deadline:
                ready, _, _ = select.select([r], [], [], 0.5)
                chunk = os.read(r, 16) if ready else b""
                if ready and not chunk:
                    break
                digits += chunk
        finally:
            os.close(r)
        if not digits.strip().isdigit():
            self.close()
            raise Failure("Xvfb did not start")
        self.display = ":" + digits.strip().decode()

    def close(self):
        if P.signal_group(self.proc, signal.SIGTERM):
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                P.signal_group(self.proc, signal.SIGKILL)
                self.proc.wait()


def pngpixels(data):
    """The rows of an 8-bit RGB PNG, as the helper writes them: (width,
    height, rows of bytes)."""
    w, h = pngdims(data)
    idat, pos = b"", 8
    while pos + 8 <= len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        if data[pos + 4:pos + 8] == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride, prev, rows = w * 3, bytearray(w * 3), []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def darkpixels(png):
    _, _, rows = pngpixels(png)
    return sum(1 for row in rows for i in range(0, len(row), 3)
               if row[i] + row[i + 1] + row[i + 2] < 200)


def x11helper(t):
    """The helper's command line, where it can drive an X display: the
    harness's own desktop command, in a build that is not static, else
    the helper next to it."""
    import ctypes.util
    if not isinstance(t.host, LocalHost) or not sys.platform.startswith("linux"):
        raise Skip("drives an X display: Linux, on this machine")
    if not shutil.which("Xvfb") or not ctypes.util.find_library("X11"):
        raise Skip("needs Xvfb and libX11")
    own = [t.host.prospero, "desktop"]
    p = subprocess.run(own + ["--version"], capture_output=True, text=True, timeout=30)
    if p.stdout.startswith("prospero-desktop "):
        return own
    helper = t.host.prospero + "-desktop"
    if not os.path.exists(helper):
        raise Skip(f"no helper at {helper}")
    return [helper]


def xrun(env, *args, data=None):
    """A command with the display, its exit status, output and errors: a
    list among args, such as a helper's command line, is spliced in."""
    argv = [x for a in args for x in (a if isinstance(a, list) else [a])]
    p = subprocess.run(argv, env=env, input=data, capture_output=True, timeout=60)
    return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace")


def waylandbus(t, helper, wl):
    """The helper on Wayland against a session bus of the test's own
    (dbus-daemon, if installed), on which nothing answers for the portal
    or the compositor, nor is started on demand: each command fails, and
    says why. Its D-Bus client authenticates, says Hello, and reads the
    errors of calls to names that are not there."""
    daemon = shutil.which("dbus-daemon")
    if not daemon:
        return
    sock = t.host.join(t.base, "bus")
    conf = t.host.join(t.base, "bus.conf")
    with open(conf, "w") as f:
        f.write(f"""<busconfig><type>session</type><listen>unix:path={sock}</listen>
<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*" eavesdrop="true"/>
<allow eavesdrop="true"/><allow own="*"/></policy>
<limit name="reply_timeout">60000</limit></busconfig>""")
    proc = subprocess.Popen([daemon, "--config-file", conf, "--nofork", "--print-address"],
                            stdout=subprocess.PIPE, stdin=subprocess.DEVNULL,
                            start_new_session=True)
    try:
        ready, _, _ = select.select([proc.stdout], [], [], 10)
        addr = proc.stdout.readline().decode().strip() if ready else ""
        t.check(addr.startswith("unix:"), f"dbus-daemon address: {addr!r}")
        env = {k: v for k, v in wl.items() if k != "DISPLAY"}
        env["DBUS_SESSION_BUS_ADDRESS"] = addr
        env["PROSPERO_STATE"] = t.host.join(t.base, "state")
        rc, _, err = xrun(env, helper, "list")
        t.check(rc == 1 and "Window Calls extension" in err, f"wayland, gnome list: {rc} {err}")
        rc, _, err = xrun(dict(env, XDG_CURRENT_DESKTOP="Hyprland"), helper, "list")
        t.check(rc == 1 and "'Hyprland') offers no way" in err, f"wayland, other list: {rc} {err}")
        rc, _, err = xrun(env, helper, "shot")
        t.check(rc == 1 and "no remote desktop portal" in err, f"wayland, shot: {rc} {err}")
        rc, _, err = xrun(env, helper, "type", "active", "x")
        t.check(rc == 1 and "no remote desktop portal" in err, f"wayland, type: {rc} {err}")
        rc, _, err = xrun(env, helper, "close", "active")
        t.check(rc == 1 and "cannot be named" in err, f"wayland, close active: {rc} {err}")
        rc, out, err = xrun(env, helper, "access")
        out = out.decode()
        t.check(rc == 0 and "remote-desktop\tmissing" in out and "windows\tmissing" in out,
                f"wayland, access: {rc} {out!r} {err}")
    finally:
        if P.signal_group(proc, signal.SIGTERM):
            proc.wait(5)


def x11cli(t, helper, env, wm):
    """The helper's own command line: errors without a display, and a
    window started, moved and resized, captured, typed into, and closed."""
    plain = {k: v for k, v in env.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    rc, _, err = xrun(plain, helper, "list")
    t.check(rc == 1 and "DISPLAY is not set" in err, f"no display: {rc} {err}")
    # A Wayland session goes through D-Bus, here to no bus at all, or one
    # of the test's own with no portal and no compositor on it; and
    # PROSPERO_DESKTOP_SESSION picks X11 even so
    wl = dict(env, WAYLAND_DISPLAY="wayland-x", XDG_CURRENT_DESKTOP="GNOME",
              DBUS_SESSION_BUS_ADDRESS="unix:path=" + t.host.join(t.base, "no-bus"))
    rc, _, err = xrun(wl, helper, "list")
    t.check(rc == 1 and "cannot connect to the session bus" in err, f"wayland, no bus: {rc} {err}")
    rc, out, err = xrun(dict(wl, PROSPERO_DESKTOP_SESSION="x11"), helper, "list")
    t.check(rc == 0, f"wayland, forced x11: {rc} {err}")
    waylandbus(t, helper, wl)

    rc, _, err = xrun(env, helper, "run", t.host.join(t.base, "no-such-program"))
    t.check(rc == 1 and "no-such-program': No such file" in err, f"run fails: {rc} {err}")

    log = t.host.join(t.base, "cli.log")
    rc, out, err = xrun(env, helper, "run", sys.executable, "-I", XPROBE, "window", log)
    pids = P.parse(out.decode())
    t.check(rc == 0 and len(pids) == 1, f"run: {rc} {out} {err}")
    win = f"pid={pids[0] if pids else 0}"
    rc, out, err = xrun(env, helper, "wait", "-t", "20", win)
    t.check(rc == 0 and b"Prospero probe" in out and b"XProbe" in out, f"wait: {out} {err}")
    rc, _, err = xrun(env, helper, "wait", "-idle", "-t", "20", win)
    t.check(rc == 0, f"idle: {rc} {err}")
    # A manager moves the window when it gets to the request
    rc, _, err = xrun(env, helper, "move", win, "100", "120", "300", "150")
    for _ in range(50):
        rc2, out, _ = xrun(env, helper, "list", win)
        if b"\t100,120\t300x150\t" in out:
            break
        time.sleep(0.1)
    t.check(rc == 0 and rc2 == 0 and b"\t100,120\t300x150\t" in out, f"moved: {out} {err}")
    rc, png, err = xrun(env, helper, "shot", win)
    t.check(rc == 0 and pngdims(png) == (300, 150), f"shot: {rc} {pngdims(png)} {err}")
    rc, _, err = xrun(env, helper, "type", win, "-", data="é€".encode())
    t.check(rc == 0, f"type from stdin: {err}")
    rc, _, err = xrun(env, helper, "wheel", win, "5", "40", "-2")
    t.check(rc == 0, f"wheel: {err}")
    rc, _, err = xrun(env, helper, "key", win, "ctrl+zz")
    t.check(rc == 1 and "unknown key 'ctrl+zz'" in err, f"bad key: {rc} {err}")
    rc, _, err = xrun(env, helper, "close", win)
    t.check(rc == 0, f"close: {err}")
    rc, _, err = xrun(env, helper, "wait", "-gone", "-t", "20", win)
    t.check(rc == 0, f"gone: {rc} {err}")
    lines = [json.loads(x) for x in (t.host.read(log) or "").splitlines()]
    t.check({"close": True, "text": "é€"} in lines, f"typed from stdin: {lines[-3:]}")
    fx, _, fy, _ = FRAME_X11 if wm else (0, 0, 0, 0)
    wheel = [x for x in lines if x.get("button") == 5 and x.get("press")]
    t.check(len(wheel) == 2 and wheel[0]["x"] == 5 - fx and wheel[0]["y"] == 40 - fy,
            f"wheel down twice at 5,40: {wheel}")
    if not wm:
        return

    # A window the manager will not activate gets no input, and the
    # command says so, whatever X's own focus did meanwhile
    log = t.host.join(t.base, "stubborn.log")
    rc, out, err = xrun(env, helper, "run", sys.executable, "-I", XPROBE, "window", log,
                        "Stubborn probe")
    pids = P.parse(out.decode())
    win = f"pid={pids[0] if pids else 0}"
    rc, _, err = xrun(env, helper, "wait", "-t", "20", win)
    t.check(rc == 0, f"stubborn wait: {rc} {err}")
    for cmd in (["type", win, "nope"], ["key", win, "enter"], ["click", win, "5", "40"]):
        rc, _, err = xrun(env, helper, *cmd)
        t.check(rc == 1 and "cannot give the keyboard to the window 'Stubborn probe'" in err
                and "no input was sent" in err, f"refused {cmd[0]}: {rc} {err}")
    rc, _, err = xrun(env, helper, "close", win)
    rc2, _, _ = xrun(env, helper, "wait", "-gone", "-t", "20", win)
    t.check(rc == 0 and rc2 == 0, f"stubborn closed: {err}")
    lines = [json.loads(x) for x in (t.host.read(log) or "").splitlines()]
    lines = [x for x in lines if "mapped" not in x]
    t.check(lines == [{"close": True, "text": ""}], f"nothing reached it: {lines}")


FRAME_X11 = (2, 2, 20, 2)  # xprobe.py's FRAME


def x11desk(t, wm):
    helper = x11helper(t)
    x = Xvfb()
    wmproc = None
    try:
        env = {"DISPLAY": x.display, "XDG_SESSION_TYPE": "x11", "WAYLAND_DISPLAY": ""}
        xenv = dict(os.environ, **env)
        wmlog = t.host.join(t.base, "wm.log")
        if wm:
            wmproc = subprocess.Popen([sys.executable, "-I", XPROBE, "wm", wmlog], env=xenv,
                                      stdin=subprocess.DEVNULL, start_new_session=True)
            for _ in range(100):
                if "ready" in (t.host.read(wmlog) or ""):
                    break
                time.sleep(0.1)
            t.check("ready" in (t.host.read(wmlog) or ""), "the window manager started")
        keys0 = xrun(xenv, sys.executable, "-I", XPROBE, "keys")[1]

        x11cli(t, helper, xenv, wm)

        # Without the window manager, the harness finds the helper as a
        # user's does, itself or next to itself, unless it has another name
        log = t.host.join(t.base, "probe.log")
        prompt = json.dumps({"program": sys.executable, "args": ["-I", XPROBE, "window", log]})
        if wm or os.path.basename(t.host.prospero) != "prospero":
            env = dict(env, PROSPERO_TEST_DESKTOP=json.dumps(helper))
        r = t.run(prompt=prompt, timeout=80, env=env)
        ends = [(e["status"], e["text"][:160]) for e in r.all("tool.end")]
        t.check(r.rc == 0, f"exit {r.rc}: {r.last('turn.end')} {ends}")
        t.check(len(ends) == 12 and all(s == "ok" for s, _ in ends), f"every step: {ends}")

        lines = [json.loads(x) for x in (t.host.read(log) or "").splitlines()]
        want = fake_provider.DESK_X11_TEXT[0] + "Ok!" + fake_provider.DESK_X11_TEXT[1]
        t.check({"close": True, "text": want} in lines, f"typed: {lines[-1:]}")
        # The click in the middle of the window's screenshot, in pixels of
        # its frame, which the client's origin is inside of
        size = re.findall(r"x from 0 to (\d+), y from 0 to (\d+)", "\n".join(e for _, e in ends))
        fx, _, fy, _ = FRAME_X11 if wm else (0, 0, 0, 0)
        cw, ch = 480 + fx + FRAME_X11[1] * wm, 200 + fy + FRAME_X11[3] * wm
        t.check(size and size[-1] == (str(cw - 1), str(ch - 1)), f"the frame {size}")
        clicks = [{k: x[k] for k in ("button", "press", "x", "y")}
                  for x in lines if x.get("button") == 1 and x.get("press")]
        t.check(clicks == [{"button": 1, "press": True, "x": (cw - 1) // 2 - fx,
                            "y": (ch - 1) // 2 - fy}], f"clicked {clicks}")

        shots = [base64.b64decode(url.split(",", 1)[1])
                 for q in t.requests()[-1:]
                 for _, url in fake_provider.desk_images(q["body"])]
        dims = [pngdims(s) for s in shots]
        t.check(dims[-2:] == [(cw, ch), (cw, ch)], f"window shots {dims}")
        if len(shots) >= 2:
            before, after = darkpixels(shots[-2]), darkpixels(shots[-1])
            t.check(after > before + 100, f"text drawn: {before} dark pixels, then {after}")
        # The helper unbinds them as it exits, a second after the last
        # key, or at once when the harness's end asks it to quit
        for _ in range(30):
            keys1 = xrun(xenv, sys.executable, "-I", XPROBE, "keys")[1]
            if keys1 == keys0:
                break
            time.sleep(0.2)
        t.check(keys0 and keys0 == keys1, "the spare keycodes are unbound again")
        if wm:
            acts = [json.loads(x) for x in (t.host.read(wmlog) or "").splitlines()]
            t.check(any("active" in a for a in acts) and any("close" in a for a in acts) and
                    any(a.get("gravity") == 10 for a in acts), f"through EWMH: {acts}")
    finally:
        if wmproc:
            P.signal_group(wmproc, signal.SIGTERM)
            wmproc.wait()
        x.close()


@scenario(conf=DESKALL, bound=120)
def desktop_x11(t):
    """The X11 helper without a window manager, as on a bare Xvfb."""
    x11desk(t, wm=False)


@scenario(conf=DESKALL, bound=120)
def desktop_x11_wm(t):
    """The X11 helper through a window manager's EWMH."""
    x11desk(t, wm=True)


def helperframe(proc, *args):
    """One command to a helper in serve mode, and its reply: (status,
    text)."""
    payload = b"".join(a.encode() + b"\0" for a in args)
    proc.stdin.write(len(payload).to_bytes(4, "little") + payload)
    proc.stdin.flush()
    n = int.from_bytes(proc.stdout.read(4), "little")
    reply = proc.stdout.read(n)
    tlen = int.from_bytes(reply[1:5], "little")
    return reply[0], reply[5:5 + tlen].decode("utf-8", "replace")


@scenario(bound=60)
def desktop_x11_keys(t):
    """The keycodes a helper binds for typing stay bound while it lives,
    whatever other helpers do, and when it is killed outright, the next
    helper's command unbinds them."""
    helper = x11helper(t)
    x = Xvfb()
    probe = None
    try:
        env = dict(os.environ, DISPLAY=x.display, XDG_SESSION_TYPE="x11", WAYLAND_DISPLAY="")
        log = t.host.join(t.base, "keys.log")
        probe = subprocess.Popen([sys.executable, "-I", XPROBE, "window", log], env=env,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        win = f"pid={probe.pid}"
        rc, _, err = xrun(env, helper, "wait", "-t", "20", win)
        t.check(rc == 0, f"the probe's window: {err}")

        def keys():
            return xrun(env, sys.executable, "-I", XPROBE, "keys")[1]

        def text():
            texts = [json.loads(x).get("text") for x in (t.host.read(log) or "").splitlines()]
            texts = [s for s in texts if s is not None]
            return texts[-1] if texts else ""
        keys0 = keys()

        # A live helper's keys are its own
        live = subprocess.Popen(helper + ["--serve"], env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            t.check(helperframe(live, "hello")[0] == 0, "hello")
            st, msg = helperframe(live, "type", win, "é€")
            t.check(st == 0 and waitfor(lambda: text() == "é€", 5), f"typed: {msg} {text()!r}")
            bound = keys()
            t.check(bound != keys0, "keys bound for typing")
            rc, _, err = xrun(env, helper, "list")
            t.check(rc == 0 and keys() == bound, f"another helper leaves them: {err}")
        finally:
            live.stdin.close()
            live.wait(10)
        t.check(waitfor(lambda: keys() == keys0, 5), "unbound as it exits")

        # One killed outright leaves them, for the next to undo
        dead = subprocess.Popen(helper + ["--serve"], env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            helperframe(dead, "hello")
            st, msg = helperframe(dead, "type", win, "✓")
            t.check(st == 0 and waitfor(lambda: text() == "é€✓", 5), f"typed: {msg} {text()!r}")
        finally:
            dead.kill()  # our own child, not yet reaped
            dead.wait()
        t.check(keys() != keys0, "a killed helper leaves its keys bound")
        rc, _, err = xrun(env, helper, "list")
        t.check(rc == 0 and keys() == keys0, f"the next helper unbinds them: {err}")
    finally:
        if probe and P.signal_group(probe, signal.SIGTERM):
            probe.wait()
        x.close()


# Real window managers, those installed, each on a display of its own
X11WMS = [
    ["openbox"],
    ["xfwm4", "--compositor=off"],
    ["metacity", "--replace"],
    ["kwin_x11", "--replace"],
    ["icewm"],
    ["fluxbox"],
]


def listed(out):
    """The windows of the helper's list: (id, pid, x, y, w, h, title)."""
    r = []
    for line in out.decode("utf-8", "replace").splitlines():
        f = line.split("\t")
        if len(f) == 6:
            x, y = map(int, f[2].split(","))
            w, h = map(int, f[3].split("x"))
            r.append((f[0], int(f[1]), x, y, w, h, f[5]))
    return r


def x11wm(t, helper, x, argv):
    """Two probes under a real window manager: run, focused in turn and
    typed into, clicked where the frame says, captured at its size,
    restored from minimized, moved, and closed; and a GTK dialog that
    draws its own decorations, typed into until it prints the text."""
    env = dict(os.environ, DISPLAY=x.display, XDG_SESSION_TYPE="x11", WAYLAND_DISPLAY="",
               GDK_BACKEND="x11", NO_AT_BRIDGE="1")
    name = argv[0]
    wm = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        t.check(waitfor(lambda: xrun(env, sys.executable, "-I", XPROBE, "wmname")[1].strip(),
                        15), f"{name}: the window manager started")
        rc, out, err = xrun(env, helper, "list")
        t.check(rc == 0 and not out, f"{name}: none of its own windows listed: {out} {err}")
        logs, pids = {}, {}
        for k in ("A", "B"):
            logs[k] = t.host.join(t.base, f"{name}-{k}.log")
            rc, out, err = xrun(env, helper, "run", sys.executable, "-I", XPROBE, "window",
                                logs[k], f"Probe {k} {name}")
            got = P.parse(out.decode())
            t.check(rc == 0 and len(got) == 1, f"{name}: run {k}: {rc} {out} {err}")
            pids[k] = got[0]
            rc, _, err = xrun(env, helper, "wait", "-t", "20", f"pid={pids[k]}")
            t.check(rc == 0, f"{name}: wait {k}: {err}")

        def logged(k):
            return [json.loads(x) for x in (t.host.read(logs[k]) or "").splitlines()]

        def text(k):
            texts = [x["text"] for x in logged(k) if "text" in x]
            return texts[-1] if texts else ""

        # Focus goes back and forth, through the manager
        for k, s in (("A", "Grüße ✓"), ("B", "é€"), ("A", " again")):
            rc, _, err = xrun(env, helper, "type", f"pid={pids[k]}", s)
            t.check(rc == 0, f"{name}: type into {k}: {err}")
        t.check(waitfor(lambda: text("A") == "Grüße ✓ again" and text("B") == "é€", 5),
                f"{name}: typed {text('A')!r} and {text('B')!r}")

        # A click lands where the window's frame, as listed, puts it, and
        # a shot is the frame's size
        rc, out, _ = xrun(env, helper, "list", f"pid={pids['A']}")
        wins = listed(out)
        t.check(rc == 0 and len(wins) == 1, f"{name}: list A: {out}")
        _, _, fx, fy, fw, fh, _ = wins[0]
        rc, _, err = xrun(env, helper, "click", f"pid={pids['A']}", str(fw // 2), str(fh - 20))
        t.check(rc == 0, f"{name}: click: {err}")
        t.check(waitfor(lambda: any(c.get("press") for c in logged("A") if "button" in c), 5),
                f"{name}: clicked")
        press = [c for c in logged("A") if c.get("button") == 1 and c.get("press")]
        t.check(press and (press[-1]["rx"], press[-1]["ry"]) == (fx + fw // 2, fy + fh - 20),
                f"{name}: the click at {fx + fw // 2},{fy + fh - 20}: {press}")
        rc, png, err = xrun(env, helper, "shot", f"pid={pids['A']}")
        t.check(rc == 0 and pngdims(png) == (min(fw, 1024 - fx), min(fh, 768 - fy)),
                f"{name}: shot {pngdims(png)} of {wins[0]}: {err}")

        # Minimized, then typed into: the manager restores it first
        rc, out, _ = xrun(env, helper, "list", f"pid={pids['B']}")
        wins = listed(out)
        t.check(len(wins) == 1, f"{name}: list B: {out}")
        def mapped(k):
            maps = [x["mapped"] for x in logged(k) if "mapped" in x]
            return maps[-1] if maps else None

        xrun(env, sys.executable, "-I", XPROBE, "iconify", wins[0][0])
        t.check(waitfor(lambda: mapped("B") is False, 5), f"{name}: B minimized")
        rc, _, err = xrun(env, helper, "type", f"pid={pids['B']}", "!")
        t.check(rc == 0 and mapped("B") and waitfor(lambda: text("B") == "é€!", 5),
                f"{name}: typed into B restored: {rc} {err} {text('B')!r}")

        rc, _, err = xrun(env, helper, "move", f"pid={pids['B']}", "100", "120", "300", "150")
        t.check(rc == 0, f"{name}: move: {err}")
        t.check(waitfor(lambda: [w[2:6] for w in listed(xrun(env, helper, "list",
                                                               f"pid={pids['B']}")[1])]
                        == [(100, 120, 300, 150)], 5), f"{name}: moved")

        for k in ("A", "B"):
            rc, _, err = xrun(env, helper, "close", f"pid={pids[k]}")
            rc2, _, _ = xrun(env, helper, "wait", "-gone", "-t", "20", f"pid={pids[k]}")
            t.check(rc == 0 and rc2 == 0, f"{name}: close {k}: {err}")
            t.check(logged(k)[-1].get("close"), f"{name}: {k} asked to close")

        if shutil.which("zenity") and shutil.which("dbus-run-session"):
            x11csd(t, helper, env, name)
    finally:
        P.signal_group(wm, signal.SIGTERM)
        try:
            wm.wait(5)
        except subprocess.TimeoutExpired:
            P.signal_group(wm, signal.SIGKILL)
            wm.wait()


def x11csd(t, helper, env, name):
    """zenity's entry, a GTK dialog, on a session bus of its own: its
    frame without the shadow it draws (_GTK_FRAME_EXTENTS), typed into,
    and the text it prints."""
    z = subprocess.Popen(["dbus-run-session", "zenity", "--entry", "--title=Csd probe",
                          "--text=Type here"], env=env, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    try:
        rc, _, err = xrun(env, helper, "wait", "-t", "30", "title=Csd probe")
        t.check(rc == 0, f"{name}: zenity's window: {err}")
        time.sleep(1)  # GTK shows its window before it can take keys
        rc, out, _ = xrun(env, helper, "list", "title=Csd probe")
        wins = listed(out)
        t.check(len(wins) == 1, f"{name}: list zenity: {out}")
        rc, png, err = xrun(env, helper, "shot", "title=Csd probe")
        t.check(rc == 0 and pngdims(png) == wins[0][4:6],
                f"{name}: zenity's shot {pngdims(png)} of {wins[0]}: {err}")
        rc, _, err = xrun(env, helper, "type", "title=Csd probe", "Grüße ✓")
        rc2, _, err2 = xrun(env, helper, "key", "title=Csd probe", "enter")
        t.check(rc == 0 and rc2 == 0, f"{name}: typed into zenity: {err} {err2}")
        out, _ = z.communicate(timeout=20)
        t.check(out.decode("utf-8", "replace").splitlines()[-1:] == ["Grüße ✓"],
                f"{name}: zenity printed {out!r}")
    finally:
        if P.signal_group(z, signal.SIGTERM):
            z.wait()


@scenario(bound=300)
def desktop_x11_wms(t):
    """The X11 helper under each real window manager installed."""
    helper = x11helper(t)
    wms = [w for w in X11WMS if shutil.which(w[0])]
    if not wms:
        raise Skip("no window manager installed: " + ", ".join(w[0] for w in X11WMS))
    for argv in wms:
        x = Xvfb()
        try:
            x11wm(t, helper, x, argv)
        finally:
            x.close()
    t.note("window managers: " + " ".join(w[0] for w in wms))


def usermanager():
    """Whether the user's systemd manager takes transient services."""
    if not shutil.which("systemd-run") or not shutil.which("systemctl"):
        return False
    p = subprocess.run(["systemctl", "--user", "is-system-running"],
                       capture_output=True, text=True, timeout=30)
    return p.stdout.strip() in ("running", "degraded")


def waitfor(cond, secs):
    deadline = time.monotonic() + secs
    while not cond() and time.monotonic() < deadline:
        time.sleep(0.1)
    return cond()


@scenario(conf=DESKALL, bound=120)
def desktop_x11_run(t):
    """A program the desktop tool runs outlives the turn and the harness,
    as a service of the user's manager, out of the reaper's tree, while
    a shell command's escapee is still swept. Where the manager will not
    start it (a systemd-run that fails, here), the tool says that it
    ends with the harness, and it does."""
    helper = x11helper(t)
    managed = usermanager()
    fake = t.host.join(t.base, "fakebin")
    os.makedirs(fake, exist_ok=True)
    with open(os.path.join(fake, "systemd-run"), "w") as f:
        f.write("#!/bin/sh\necho 'Failed to connect to bus: no manager here' >&2\nexit 1\n")
    os.chmod(os.path.join(fake, "systemd-run"), 0o755)
    x = Xvfb()
    try:
        env = {"DISPLAY": x.display, "XDG_SESSION_TYPE": "x11", "WAYLAND_DISPLAY": "",
               "PROSPERO_TEST_DESKTOP": json.dumps(helper)}
        xenv = dict(os.environ, **env)
        for apart in ([True, False] if managed else [False]):
            name = "apart" if apart else "attached"
            if not apart:
                env = dict(env, PATH=fake + os.pathsep + os.environ.get("PATH", ""))
            log = t.host.join(t.base, f"{name}.log")
            prompt = json.dumps({"program": sys.executable,
                                 "args": ["-I", XPROBE, "window", log, f"Probe {name}"]})
            pidfile = t.host.join(t.work, "tree.pids")
            if os.path.exists(pidfile):
                os.remove(pidfile)
            r = t.run(prompt=prompt, timeout=80, env=env)
            ends = [(e["status"], e["text"][:800]) for e in r.all("tool.end")]
            t.check(r.rc == 0 and len(ends) == 3 and all(s == "ok" for s, _ in ends),
                    f"{name}: exit {r.rc}: {ends}")
            m = re.search(r"process id (\d+)", ends[0][1] if ends else "")
            pids = P.parse(m.group(1) if m else "")
            t.check(len(pids) == 1, f"{name}: the program's pid: {ends[:1]}")
            pid = pids[0]
            noted = "cannot detach the program" in ends[0][1]
            t.check(noted != apart, f"{name}: what run said: {ends[0][1]}")
            escapees = P.read(pidfile)
            t.check(len(escapees) == 1, f"{name}: the escapee's pid: {escapees}")

            # The harness has exited: its reaper sweeps what the shell
            # left, TERM and then KILL 2 s later
            t.check(waitfor(lambda: not any(P.alive(e) for e in escapees), 10),
                    f"{name}: the escapee is swept: {escapees}")
            win = f"pid={pid}"
            if not apart:
                t.check(waitfor(lambda: not P.alive(pid), 10),
                        f"attached: the program ended with the harness: {pid}")
                continue
            t.check(P.alive(pid), f"apart: the program outlived the harness: {pid}")
            try:
                with open(f"/proc/{P.arg(pid)}/cgroup") as f:
                    cgroup = f.read()
            except OSError:
                cgroup = ""
            t.check("/app.slice/app-prospero-" in cgroup,
                    f"apart: in a service of the user's manager: {cgroup!r}")
            rc, out, err = xrun(xenv, helper, "list", win)
            t.check(rc == 0 and f"Probe {name}".encode() in out, f"apart: its window: {out} {err}")
            rc, _, err = xrun(xenv, helper, "close", win)
            rc2, _, _ = xrun(xenv, helper, "wait", "-gone", "-t", "20", win)
            t.check(rc == 0 and rc2 == 0 and waitfor(lambda: not P.alive(pid), 10),
                    f"apart: closed: {err}")
    finally:
        x.close()


@scenario(conf={**AN, **DESKALL, "_extra": "[model {model}]\nimage_edge = 100\n"}, bound=15)
def an_desktop(t):
    """A screenshot goes back to Anthropic in its tool result, as an image
    block after the text, which the fake checks as the API would, with
    the image limits of kind anthropic."""
    env = dict(ANENV, **deskenv(t))
    r = t.run(env=env)
    anok(t, r)
    reqs = [q["body"] for q in t.requests()]
    result = [b for b in reqs[1]["messages"][-1]["content"] if b["type"] == "tool_result"][0]
    kinds = [c["type"] for c in result["content"]]
    t.check(kinds == ["text", "image"], f"the result {kinds}")
    src = result["content"][1]["source"]
    t.check(src["type"] == "base64" and src["media_type"] == "image/png" and
            pngdims(base64.b64decode(src["data"])) == (100, 62), "the image")
    t.check(desklog(t)[1] == ["shot", "-s", "100", "1150000", "screen"],
            f"anthropic's pixel limit {desklog(t)[1]}")
    t.check(desklog(t)[2] == ["click", "screen", "32", "45"], f"mapped {desklog(t)[2]}")


@scenario(conf={**AN, **DESKALL, "_extra": "[model {model}]\nimage_edge = 64\n"}, bound=30)
def an_desktop_cache(t):
    """Over twelve screenshots on Anthropic: cache reads grow between the
    steps where older screenshots leave, and at those steps the request
    still reads up to the oldest one left out, where the step before
    wrote a breakpoint, so reads never fall back to the system prompt."""
    env = dict(ANENV, **deskenv(t), FAKE_DESKTOP_ANIMATE="1")
    r = t.run(env=env)
    anok(t, r)
    reqs = [q["body"] for q in t.requests()]
    t.check(len(reqs) == 13, f"requests {len(reqs)}")
    cached = [e["usage"]["cached"] for e in r.all("assistant")]
    falls = [i for i in range(1, len(cached)) if cached[i] <= cached[i - 1]]
    t.check(falls == [7, 11], f"reads fall only at the two steps: {cached}")
    floor = cached[1]  # the system prompt and tools, at least
    t.check(all(cached[i] > floor for i in falls), f"read past the start at steps: {cached}")
    t.check(all(cached[i] > cached[i - 1] for i in range(2, len(cached)) if i not in falls),
            f"reads grow between steps: {cached}")
    tools = [x for x in reqs[7]["tools"] if "cache_control" in x]
    t.check(not tools, "the tools' breakpoint gave way at a step")
    alts = [c["text"] for m in reqs[-1]["messages"] for b in m["content"]
            if b.get("type") == "tool_result" and isinstance(b.get("content"), list)
            for c in b["content"] if c["type"] == "text" and c["text"].startswith("[screenshot")]
    t.check(len(alts) == 8, f"left out {len(alts)}")
    us = [e["usage"] for e in r.all("assistant")]
    t.note(f"cache reads: {cached}; in all, read {sum(cached)}, written "
           f"{sum(u.get('written', 0) for u in us)}, prompt {sum(u['in'] for u in us)}")


@scenario(conf={**AN, "_desktop": {"offer": "all", "ask": "false", "images_slack": 0},
                "_extra": "[model {model}]\nimage_edge = 64\n"}, bound=30)
def an_desktop_cache_before(t):
    """As an_desktop_cache, without slack, as before it: past the third
    screenshot, every request reads only up to the one it leaves out, so
    the screenshots shown are written anew each time."""
    env = dict(ANENV, **deskenv(t), FAKE_DESKTOP_ANIMATE="1")
    r = t.run(env=env)
    anok(t, r)
    cached = [e["usage"]["cached"] for e in r.all("assistant")]
    t.check(len(cached) == 13 and all(c < cached[3] for c in cached[4:]),
            f"reads stay low past the third: {cached}")
    us = [e["usage"] for e in r.all("assistant")]
    t.note(f"cache reads: {cached}; in all, read {sum(cached)}, written "
           f"{sum(u.get('written', 0) for u in us)}, prompt {sum(u['in'] for u in us)}")


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
    ap.add_argument("--prospero", required=True, help="the harness binary")
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
        host = XpHost(args.xp, args.prospero, url)
    elif args.host:
        host = SshHost(args.host, args.prospero, port, 20000 + port % 20000)
    else:
        host = LocalHost(args.prospero, url)
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
        leftbehind(host)


if __name__ == "__main__":
    sys.exit(main())
