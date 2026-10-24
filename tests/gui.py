#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The GUI's scripted checklist against the fake provider.

Runs "prospero gui --drive" with a real engine, in this process or as
"prospero --headless" (--subprocess), in fresh working, state and
configuration directories, against fake_provider.py's "gui" scenario.
It passes when the checklist reports no failed checks and no stale
frames, the harness exits cleanly in time, the settings edit changed one
line of the configuration file, and the commands it cancelled or stopped
by quitting are gone, with no engine process left behind.

  uv run tests/gui.py --prospero build/prospero [--subprocess] [--inbox-cap N]
  uv run tests/gui.py --prospero build/prospero --mode idle      (60 s)
  uv run tests/gui.py --prospero build/prospero --mode streamtest
  uv run tests/gui.py --prospero build/prospero --mode signals   (POSIX)
  uv run tests/gui.py --prospero build/prospero --mode settings [--shots DIR]
  uv run tests/gui.py --prospero build/prospero --mode enginestop
  uv run tests/gui.py --prospero build/prospero --mode sessions [--shots DIR]
  uv run tests/gui.py --prospero build/prospero --fake [--mode idle]   (M1's)
  uv run tests/gui.py --prospero build/prospero --mode bench   (M1's scale)
  uv run tests/gui.py --prospero build/prospero --mode compose [--shots DIR]

The settings mode edits a value longer than a field's slack and picks
the theme, dark then light, from its list over a transcript that shows
most of what one can (screenshots of each with --shots). It goes
through the settings' pages narrow (a combo) and wide (a list), checking
that nothing scrolls sideways, edits the guard's roots one a line,
deletes a section after cancelling once, finds an included one it cannot
delete, and adds a model; the file must then hold those edits and no
other change. The sessions mode goes through the sidebar's tree and its
context menus: folding, renaming, a group, archiving, a model switch,
exports as Markdown and as the raw log (which must hold what they
should), a whole session forked, another forked before its prompt (the
prompt then in the fork's box), a session with its subagents
deleted, and the raw log exported imported as a new session, which
goes on. The enginestop
mode kills the engine process mid-turn and mid-ask: the GUI must stop
counting either, and quit at once when closed. The compose mode runs
the compose box's checklist, with [ui] editor a test script (an editor
that appends a line, one that exits 1, and one cancelled), then the GUI
again, to find each session's draft back from its engine; no draft file
for the editor may be left over.

On Linux without a display it runs under xvfb-run. --serve only runs the
provider and writes the configuration, for a GUI started elsewhere (a
Windows console session): it prints the directories and waits.
"""

import argparse
import difflib
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402
import pids as P  # noqa: E402


def e2econfig(url, model):
    return (
        "# The end-to-end checklist's configuration\n"
        "endpoint = local\n"
        f"model = {model}\n"
        "\n"
        "# The router\n"
        "[endpoint local]\n"
        f"url = {url}\n"
        "idle_timeout = 120\n"
        "\n"
        "# Asks wait a little, and the temporary directory asks too\n"
        "[guard]\n"
        "ask_timeout = 10\n"
        "temp = ask\n"
    )


# A value longer than a settings field's slack of 1024 bytes
LONG_PROMPT = " ".join(f"Rule {i}: keep every change small and tested." for i in range(40))

# The settings mode's sections: the long value, an endpoint to delete
# with the comment above it, and a remote never connected
SETTINGS_GONE = ("# An endpoint to delete, with this comment\n"
                 "[endpoint gone]\n"
                 "url = http://gone.invalid/\n")
SETTINGS_EXTRA = (f"\n# A long value\n[role long]\nprompt = {LONG_PROMPT}\n"
                  f"\n{SETTINGS_GONE}"
                  "\n# A remote host, never connected\n"
                  "[remote far]\nssh = far.invalid\nlist = connected\n")


# The compose mode's editor: it fails for a draft that asks it to, waits
# to be cancelled for one that says sleep, and otherwise appends a line
COMPOSE_EDITOR = """#!/bin/sh
case $(cat "$1") in
*"exit 1"*) echo "the test editor refuses" >&2; exit 1;;
*sleep*) sleep 60;;
esac
sleep 1
printf '\nEdited by the test editor.\n' >> "$1"
"""


def composeconfig(editor, nonce):
    """The compose mode's: the test editor, and a model for /model."""
    quoted = "'" + editor.replace("'", "'\\''") + "'"
    return (
        "\n# The compose box's editor, a test script\n"
        "[ui]\n"
        f"editor = sh {quoted}\n"
        "\n# A model for /model to list\n"
        f"[model fake:gui_b:{nonce}]\n"
        "context = 100000\n"
    )


def config(url, model, ask=3, extra=""):
    return (
        "# The GUI checklist's configuration\n"
        f"endpoint = fake\n"
        f"model = {model}\n"
        "colour = blue  # unknown: the settings window lists the warning\n"
        "\n"
        "# The fake provider: a first byte may take longer than the GUI's\n"
        "# 15 s amber\n"
        "[endpoint fake]\n"
        f"url = {url}/\n"
        "first_byte_timeout = 25\n"
        "idle_timeout = 10\n"
        "progress_timeout = 30\n"
        "connect_timeout = 2\n"
        "\n"
        "# Asks wait briefly, and the temporary directory asks too, as the\n"
        "# checklist's directories are in it on Linux (/tmp)\n"
        "[guard]\n"
        f"ask_timeout = {ask}\n"
        "temp = ask\n"
    ) + extra


def m4config(url, nonce):
    """The M4 part of the checklist's configuration: a router of one
    model for the queue, roles for sessions on other models (small and
    big windows to compact, and one whose limit sends it to a fallback)."""
    return (
        "\n# A router that loads one model at a time\n"
        "[endpoint q]\n"
        f"url = {url}/router1-gui/\n"
        "first_byte_timeout = 25\n"
        "idle_timeout = 10\n"
        "connect_timeout = 2\n"
        "\n"
        "# Two models there, the second queued behind the first\n"
        "[role lead]\n"
        f"model = fake:gui:{nonce}\n"
        "endpoint = q\n"
        "\n"
        "[role rival]\n"
        f"model = fake:gui_b:{nonce}\n"
        "endpoint = q\n"
        "\n"
        "# Windows small enough to compact in\n"
        "[role small]\n"
        f"model = fake:gui_small:{nonce}\n"
        "\n"
        "[role prune]\n"
        f"model = fake:gui_prune:{nonce}\n"
        "\n"
        "# Over its limit at once: the fallback takes over\n"
        "[role failover]\n"
        f"model = fake:guifail:{nonce}\n"
        "\n"
        f"[model fake:guifail:{nonce}]\n"
        f"fallback = fake:guifail_b:{nonce}\n"
    )


DESKROLE = (
    "\n# A subagent's desktop ask, allowed always (the fake helper)\n"
    "[desktop]\n"
    "offer = roles\n"
    "\n"
    "[role desk]\n"
    "description = Uses the desktop\n"
    "tools = desktop\n"
)


M4MODELS = {"gui_b": {}, "guifail": {}, "guifail_b": {},
            "gui_small": {"meta": {"n_ctx": 24000, "n_ctx_train": 262144}},
            "gui_prune": {"meta": {"n_ctx": 100000, "n_ctx_train": 262144}}}


def bigfiles(host, prefix, n, lines):
    """Files PREFIXf0.txt.. of about 78 bytes a line, made on the host
    (a Windows command line is too short to carry them)."""
    fill = "abcdefghij" * 6
    if isinstance(host, Remote):
        host.h.sh("".join(
            f"awk 'BEGIN {{ for (j = 0; j < {lines}; j++) "
            f"printf \"file {i} line %d: {fill}\\n\", j }}' > "
            f"{host.path('work', f'{prefix}f{i}.txt')}\n" for i in range(n)))
        return
    for i in range(n):
        host.write(host.path("work", f"{prefix}f{i}.txt"),
                   "".join(f"file {i} line {j}: {fill}\n" for j in range(lines)))


def alive(pid):
    return P.alive(pid)


def readpid(path):
    found = P.read(path)
    return found[-1] if found else None


def strays(marker):
    """Processes whose command line names the run's directory."""
    try:
        out = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True,
                             text=True).stdout
    except OSError:
        return []
    found = []
    for line in out.splitlines():
        pid, _, args = line.strip().partition(" ")
        if marker in args and int(pid) != os.getpid():
            found.append(line.strip())
    return found


def ours(pid, marker):
    """Whether pid, a plain integer above 1, is a process of this user's
    whose command line names marker (the run's own directory), checked
    right before it is signaled."""
    try:
        P.checked(pid)
    except P.Refused:
        return False
    out = subprocess.run(["ps", "-ww", "-o", "uid=,args=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    uid, _, args = out.partition(" ")
    return uid == str(os.getuid()) and marker in args


def docktype(pid):
    """macOS: how the Dock takes a process, as lsappinfo says: Foreground
    (in the Dock and the app switcher), UIElement (not), or None."""
    asn = subprocess.run(["lsappinfo", "find", f"pid={pid}"], capture_output=True,
                         text=True).stdout.strip()
    if not asn:
        return None
    info = subprocess.run(["lsappinfo", "info", asn], capture_output=True,
                          text=True).stdout
    m = re.search(r'type="(\w+)"', info)
    return m.group(1) if m else None


def guipid(prospero, state):
    """The GUI process: the harness, not xvfb-run, not the engine."""
    try:
        out = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True,
                             text=True).stdout
    except OSError:
        return None
    for line in out.splitlines():
        pid, _, args = line.strip().partition(" ")
        if args.startswith(prospero + " gui ") and state in args:
            return int(pid)
    return None


def cputime(pid, tid=None):
    """Seconds of CPU the process (or on Linux one of its threads) has
    used, or None."""
    try:
        path = f"/proc/{pid}/task/{tid}/stat" if tid else f"/proc/{pid}/stat"
        with open(path) as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        pass
    try:
        t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                           capture_output=True, text=True).stdout.strip()
        secs = 0.0
        for part in t.split(":"):
            secs = secs * 60 + float(part)
        return secs
    except (OSError, ValueError):
        return None


def mainthread(pid):
    """Seconds of CPU the main (GUI) thread has used, or None: Linux
    from /proc, macOS from ps -M, whose first thread is the main one."""
    if sys.platform.startswith("linux"):
        return cputime(pid, pid)
    if sys.platform != "darwin":
        return None
    try:
        out = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True,
                             text=True).stdout.splitlines()
        f = out[1].split()
        secs = 0.0
        for t in (f[6], f[7]):  # STIME, UTIME as m:ss.cc
            m, s = t.split(":")
            secs += int(m) * 60 + float(s)
        return secs
    except (OSError, IndexError, ValueError):
        return None


def switches(pid):
    """Voluntary context switches of every thread (Linux), or None."""
    total = 0
    try:
        for tid in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{tid}/status") as f:
                for line in f:
                    if line.startswith("voluntary_ctxt_switches"):
                        total += int(line.split()[1])
    except OSError:
        return None
    return total


class Meter:
    """The operating system's view of a measured interval."""

    def __init__(self, pid, what):
        self.pid = pid
        self.what = what
        self.t0 = time.monotonic()
        self.cpu0 = cputime(pid)
        self.gui0 = mainthread(pid)
        self.sw0 = switches(pid)
        self.top = None
        if sys.platform == "darwin" and what == "idle":
            # Idle wakeups, sampled across the minute
            self.top = subprocess.Popen(
                ["top", "-l", "2", "-s", "55", "-pid", str(pid),
                 "-stats", "pid,idlew,cpu"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def report(self):
        dt = time.monotonic() - self.t0
        out = [f"{self.what}: {dt:.1f} s"]
        cpu1 = cputime(self.pid)
        if self.cpu0 is not None and cpu1 is not None:
            out.append(f"CPU {100 * (cpu1 - self.cpu0) / dt:.1f}% of a core")
        gui1 = mainthread(self.pid) if self.gui0 is not None else None
        if gui1 is not None:
            out.append(f"GUI thread {100 * (gui1 - self.gui0) / dt:.1f}%")
        sw1 = switches(self.pid)
        if self.sw0 is not None and sw1 is not None:
            out.append(f"{sw1 - self.sw0} voluntary context switches")
        if self.top:
            try:
                text = self.top.communicate(timeout=10)[0]
                rows = [l.split() for l in text.splitlines()
                        if l.strip().startswith(str(self.pid))]
                if rows:
                    out.append(f"top IDLEW {rows[-1][1]}")
            except subprocess.TimeoutExpired:
                self.top.kill()
        return ", ".join(out)


class Local:
    """The GUI here, its directories in a temporary one."""

    def __init__(self, args):
        self.args = args
        self.root = tempfile.mkdtemp(prefix="prospero-gui-")
        self.prospero = os.path.abspath(args.prospero)
        # The system's temporary directory for what this starts, as the
        # sessions' scratch directories go there
        tmp = os.path.join(self.root, "tmp")
        os.makedirs(tmp)
        os.environ.update({"TMP": tmp, "TEMP": tmp} if os.name == "nt" else {"TMPDIR": tmp})

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def mkdir(self, path):
        os.makedirs(path, exist_ok=True)

    def write(self, path, text):
        with open(path, "w") as f:
            f.write(text)

    def read(self, path):
        try:
            with open(path) as f:
                return f.read()
        except OSError:
            return None

    def alive(self, pid):
        return alive(pid)

    def run(self, guiargs, timeout, cwd=None, tag="", env=None):
        """Run the GUI, echoing its log (each line after tag, if any), in
        cwd (default the working directory). Returns (status, lines)."""
        cmd = [self.prospero, "gui"] + guiargs
        if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd
        print(" ".join(cmd), flush=True)
        p = subprocess.Popen(cmd, cwd=cwd or self.path("work"), stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, start_new_session=True,
                             env=dict(os.environ, **(env or {})))
        lines = []
        meter = None
        deadline = time.monotonic() + timeout
        # A GUI that hangs prints nothing more: end it at the deadline. Its
        # group is the new session started for it here, signaled only
        # while p is not reaped, as the wait below may have done already
        # (signal_group checks)
        killer = threading.Timer(timeout + 5, lambda: P.signal_group(p, signal.SIGKILL))
        killer.start()
        try:
            for line in p.stdout:
                lines.append(line)
                sys.stdout.write(tag + line)
                sys.stdout.flush()
                if "drive: mark: " in line:
                    pid = guipid(self.prospero, self.path("state"))
                    if pid:
                        meter = Meter(pid, line.split("drive: mark: ")[1].strip())
                elif line.startswith("prospero: rates: ") and meter:
                    print("measured: " + meter.report(), flush=True)
                    meter = None
                if time.monotonic() > deadline:
                    break
            p.wait(timeout=max(1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
        finally:
            killer.cancel()
        if p.poll() is None:
            # Still running past the deadline: its group, unreaped
            try:
                P.signal_group(p, signal.SIGKILL)
            except OSError:
                pass
            p.wait()
            return None, lines
        if p.returncode == -signal.SIGKILL:
            return None, lines  # the timer's, which killed the whole group
        return p.returncode, lines

    def popen(self, args, stdin=False):
        return subprocess.Popen(
            [self.prospero] + args, cwd=self.path("work"),
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def listdir(self, path):
        try:
            return os.listdir(path)
        except OSError:
            return []

    def sessions(self, ini, state, work):
        return makesessions(self, ini, state, work)

    def strays(self):
        left = strays(self.root)
        for line in left:
            found = P.parse(line.split()[0])
            if found:
                try:
                    P.signal_ours(found[0], signal.SIGKILL, lambda q: ours(q, self.root))
                except (OSError, P.Refused):
                    pass
        return left

    def close(self, keep):
        if keep:
            print(f"kept {self.root}")
        else:
            shutil.rmtree(self.root, ignore_errors=True)


class Remote:
    """The GUI on a Windows host, in its logged-in console session: a
    scheduled task, since DXGI refuses session 0 (OpenSSH). The provider
    stays here, reached through a reverse tunnel."""

    def __init__(self, args, port):
        import scenarios
        self.args = args
        rport = 20000 + random.randrange(20000)
        self.h = scenarios.SshHost(args.host, args.prospero, port, rport)
        self.root = self.h.root
        self.url = self.h.url
        self.task = f"prospero-gui-{random.randrange(1 << 32):08x}"
        self.tasks = []

    def path(self, *parts):
        return "/".join((self.root,) + parts)

    def mkdir(self, path):
        self.h.mkdir(path)

    def write(self, path, text):
        self.h.write(path, text)

    def read(self, path):
        return self.h.read(path)

    def alive(self, pid):
        return self.h.alive(pid)

    def run(self, guiargs, timeout, cwd=None, tag="", env=None):
        win = lambda p: p.replace("/", "\\")
        q = lambda a: '"' + win(a) + '"' if "/" in a or " " in a else a
        name = "run" + tag.strip(" :[]")
        task = self.task + tag.strip(" :[]")
        sets = "".join(f"set {k}={win(v)}\r\n" for k, v in (env or {}).items())
        bat = ("@echo off\r\ncd /d \"%~dp0\"\r\n" + sets +
               q(self.h.exe) + " gui " + " ".join(q(a) for a in guiargs) +
               f" > {name}.log 2>&1\r\necho done %errorlevel% >> {name}.log\r\n")
        self.h.write(self.path(name + ".bat"), bat)
        self.tasks.append(task)
        r = self.h.ssh(f'schtasks /create /tn {task} /tr "{win(self.path(name + ".bat"))}" '
                       f'/sc once /st 23:59 /it /f')
        if r.returncode:
            raise RuntimeError("schtasks /create: " + r.stdout + r.stderr)
        self.h.ssh(f"schtasks /run /tn {task}")
        print(f"{self.args.host}: {bat.splitlines()[-3]}", flush=True)
        seen = 0
        deadline = time.monotonic() + timeout
        status = None
        lines = []
        mark = None  # (pid, seconds of CPU, when) at a measurement's start
        while time.monotonic() < deadline:
            time.sleep(1 if self.args.mode in ("idle", "streamtest") else 3)
            text = self.h.read(self.path(name + ".log")) or ""
            lines = text.splitlines(keepends=True)
            for line in lines[seen:]:
                sys.stdout.write(tag + line)
                if "drive: mark: " in line:
                    pid = self.guipid()
                    mark = (pid, self.cputime(pid), time.monotonic()) if pid else None
                elif line.startswith("prospero: rates: ") and mark:
                    cpu1 = self.cputime(mark[0])
                    if mark[1] is not None and cpu1 is not None:
                        dt = time.monotonic() - mark[2]
                        print(f"measured: CPU {100 * (cpu1 - mark[1]) / dt:.1f}% of a "
                              f"core over {dt:.0f} s (polled: approximate)")
                    mark = None
            sys.stdout.flush()
            seen = len(lines)
            done = [l for l in lines if l.startswith("done ")]
            if done:
                status = int(done[-1].split()[1])
                break
        return status, [l for l in lines if not l.startswith("done ")]

    def popen(self, args, stdin=False):
        return self.h.popen(args, self.path("work"), stdin=stdin)

    def listdir(self, path):
        out = self.h.sh(f"ls {path} 2>/dev/null || true", check=False)
        return out.split()

    def sessions(self, ini, state, work):
        return makesessions(self, ini, state, work)

    def guipid(self):
        ps = ("Get-CimInstance Win32_Process -Filter \\\"Name='prospero.exe'\\\" | Where-Object "
              f"{{ $_.CommandLine -like '*{win_root(self.root)}*' -and $_.CommandLine -like '* gui *' }} | "
              "ForEach-Object { $_.ProcessId }")
        r = self.h.ssh(f'powershell -NoProfile -Command "{ps}"')
        pids = [int(x) for x in r.stdout.split() if x.isdigit()]
        if not pids:
            print(f"(no GUI process found: {r.stdout.strip()} {r.stderr.strip()})")
        return pids[0] if pids else None

    def cputime(self, pid):
        # Through CIM, which reaches into the console session
        ps = (f"Get-CimInstance Win32_Process -Filter \\\"ProcessId={pid}\\\" | "
              "ForEach-Object { $_.KernelModeTime + $_.UserModeTime }")
        r = self.h.ssh(f'powershell -NoProfile -Command "{ps}"')
        try:
            return int(r.stdout.strip()) / 1e7
        except ValueError:
            print(f"(no CPU time for {pid}: {r.stdout.strip()} {r.stderr.strip()})")
            return None

    def strays(self):
        ps = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like "
              f"'*{win_root(self.root)}*' }} | ForEach-Object {{ \"$($_.ProcessId) $($_.CommandLine)\" }}")
        r = self.h.ssh(f'powershell -NoProfile -Command "{ps}"')
        left = [l.strip() for l in r.stdout.splitlines() if l.strip()]
        for line in left:
            for pid in P.parse(line.split()[0]):
                self.h.killpid(pid)  # if still a process of the run's
        return left

    def close(self, keep):
        for task in self.tasks:
            self.h.ssh(f"schtasks /delete /tn {task} /f")
        if keep:
            print(f"kept {self.args.host}:{self.root}")
        else:
            self.h.close()


def makesessions(host, ini, state, work):
    """Two sessions for the GUI to find: "Say hello." run and closed, and
    "Hold me" held open by a headless engine, which is returned, its
    turn running a command until the holder quits. Its record of 5 MiB
    is too big for any mailbox, the holder's own too, so the command's
    start may show only in its log."""
    p = host.popen(["run", "-C", work, "-c", ini, "--state", state, "Say hello."])
    p.communicate(timeout=60)
    if isinstance(host, Local):
        # Screenshots for the transcript's thumbnails, from the fake
        # desktop helper (Python, so on this machine only)
        desk = host.path("desk.ini")
        host.write(desk, (host.read(ini) or "") +
                   "\n# The fake desktop's screenshots\n[desktop]\noffer = all\nask = false\n")
        env = dict(os.environ, PROSPERO_TEST_DESKTOP=json.dumps(
            [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "fake_desktop.py")]))
        r = subprocess.run([host.prospero, "run", "-C", work, "-c", desk, "--state", state,
                            "Look at the desktop."], cwd=work, env=env,
                           stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        shots = r.stdout.count('"image":"out/')
        print(f"the desktop session: exit {r.returncode}, {shots} screenshots", flush=True)
    holder = host.popen(["--headless", "--state", state, "-c", ini], stdin=True)
    cmd = {"cmd": "new", "id": 1, "cwd": work, "prompt": "Hold me: say hello."}
    holder.stdin.write(json.dumps(cmd) + "\n")
    holder.stdin.flush()
    sid = None
    for line in holder.stdout:
        if sid is None and '"ev":"reply"' in line:
            sid = json.loads(line).get("sid")
        if '"ev":"tool.start"' in line:
            break
        if '"ev":"overflow"' in line and sid:
            log = host.path("state", "sessions", sid, "events.jsonl")
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and '"ev":"tool.start"' not in (host.read(log) or ""):
                time.sleep(0.5)
            break
    # Its output must keep flowing, or it would block writing it
    threading.Thread(target=lambda: [None for _ in holder.stdout], daemon=True).start()
    return holder


def win_root(path):
    """The last part of a path, which names the run either way."""
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def signals(args):
    """An idle GUI, with no deadline left to wake it, quits at once on
    SIGTERM and on SIGINT (Ctrl+C in its terminal). POSIX only."""
    problems = []
    prospero = os.path.abspath(args.prospero)
    for sig in (signal.SIGTERM, signal.SIGINT):
        root = tempfile.mkdtemp(prefix="prospero-sig-")
        state = os.path.join(root, "state")
        cmd = [prospero, "gui", "--fake", "--statedir", state]
        if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd
        env = dict(os.environ, SDL_WINDOW_ACTIVATE_WHEN_SHOWN="0")
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        time.sleep(8)  # past the probe and the layout's save: idle
        pid = guipid(prospero, state)
        if not pid:
            problems.append(f"{sig.name}: no GUI process")
            P.signal_group(p, signal.SIGKILL)
            p.wait()
            continue
        t0 = time.monotonic()
        P.signal_ours(pid, sig, lambda q: ours(q, state), "GUI")
        try:
            p.wait(timeout=5)
            print(f"{sig.name}: the idle GUI quit in {time.monotonic() - t0:.1f} s")
        except subprocess.TimeoutExpired:
            problems.append(f"{sig.name}: still running 5 s after the signal")
            P.signal_group(p, signal.SIGKILL)
            p.wait()
        shutil.rmtree(root, ignore_errors=True)
    for msg in problems:
        print("FAIL: " + msg)
    return 1 if problems else 0


def summaryproblems(status, lines, who):
    """What is wrong with a checklist's run: its exit, its summary, stale
    frames."""
    problems = []
    if status is None:
        problems.append(f"{who}: the harness did not exit in time")
    elif status != 0:
        problems.append(f"{who}: exit status {status}")
    summary = [l for l in lines if "drive:" in l and " checks, " in l]
    if not summary:
        problems.append(f"{who}: no checklist summary")
    elif " 0 failed" not in summary[-1] or " 0 stale" not in summary[-1]:
        problems.append(f"{who}: checklist: " + summary[-1].strip())
    if "STALE" in "".join(lines):
        problems.append(f"{who}: stale frames reported")
    return problems


def waitfor(pred, timeout, step=0.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = pred()
        if r:
            return r
        time.sleep(step)
    return None


def sessionlog(host, state, needle):
    """The text of the session log in state that holds needle, if any."""
    for sid in host.listdir(host.path("state", "sessions")):
        text = host.read(host.path("state", "sessions", sid, "events.jsonl")) or ""
        if needle in text:
            return text
    return None


def handoffproblems(status, lines, who):
    """What is wrong with a launch that should have handed off to the
    window holding its state directory, and exited."""
    if status is None:
        return [f"{who}: did not exit in time"]
    if status != 0:
        return [f"{who}: exit status {status}"]
    if not any("brought it forward" in l for l in lines):
        return [f"{who}: did not say it handed off"]
    return []


def background(args, host, guiargs, guicwd):
    """--mode background: the first window keeps its turn running in the
    background and goes, its process keeping the state directory's
    window lock, its GUI thread idle (here, where that can be measured);
    the turn ends with no window; a second launch on that
    state directory hands off to the first, which comes back, and exits;
    a third, the window up, raises it and exits; the first starts another
    turn, goes again, stops it from the tray, and its process ends after
    its idle time. Then "prospero serve" hosts the state directory, and
    a window attaching to it (passing over a stale registration) shows
    the session and prompts it, and quits. Here on POSIX, a window's
    process killed with SIGKILL leaves the lock free. Returns problems
    and the logs."""
    problems = []
    first = {}
    # Long enough to go on between the launches and the first's return,
    # short enough to end it all soon after
    t = threading.Thread(target=lambda: first.update(r=host.run(
        ["--background", "--host-idle", "20"] + guiargs, args.timeout, guicwd,
        tag="[1] ")))
    t.start()
    more = []
    if not waitfor(lambda: host.read(host.path("work", "bg-ready")), 120, 1):
        problems.append("the first window never went to the background")
    else:
        if sys.platform == "darwin" and isinstance(host, Local):
            # Its window gone, the process leaves the Dock: the menu bar's
            # icon is the way back
            pid = guipid(host.prospero, host.path("state"))
            kind = None
            for _ in range(15):
                kind = docktype(pid) if pid else "no process"
                if kind == "UIElement":
                    break
                time.sleep(0.2)
            print(f"the first window's process, in the background: {kind}", flush=True)
            if kind != "UIElement":
                problems.append(f"the first window's process is still in the Dock ({kind})")
        if isinstance(host, Local):
            # With no window, its GUI thread neither draws nor polls (SDL
            # polls every millisecond with no window to wait on)
            pid = guipid(host.prospero, host.path("state"))
            time.sleep(1)  # the window's going settles
            gui0 = mainthread(pid) if pid else None
            time.sleep(4)
            gui1 = mainthread(pid) if gui0 is not None else None
            if gui1 is not None:
                pct = 100 * (gui1 - gui0) / 4
                print(f"the shed window's GUI thread: {pct:.1f}% of a core", flush=True)
                if pct > 1:
                    problems.append(f"the GUI thread works with no window ({pct:.1f}%)")
        regs = host.listdir(host.path("state", "hosts"))
        print(f"registered hosts: {regs}", flush=True)
        if not regs:
            problems.append("no host registered")
        # The turn goes on, and ends, with no window
        done = waitfor(lambda: '"ev":"turn.end"' in (sessionlog(
            host, "state", "Stream in the background") or ""), 60, 1)
        if not done:
            problems.append("the turn did not end in the background")
        else:
            print("the turn ended in the background", flush=True)
        # Launched again: the shed window comes back, and this one goes
        status, lines = host.run(guiargs, 60, guicwd, tag="[2] ")
        problems += handoffproblems(status, lines, "the second launch")
        more += lines
        if not waitfor(lambda: host.read(host.path("work", "bg-back")), 60, 1):
            problems.append("the first window did not come back")
        else:
            # And again, the window up: raised
            status, lines = host.run(guiargs, 60, guicwd, tag="[3] ")
            problems += handoffproblems(status, lines, "the third launch")
            more += lines
    t.join(args.timeout + 60)
    status1, lines1 = first.get("r", (None, []))
    problems += summaryproblems(status1, lines1, "the first window")
    if status1 is not None and host.listdir(host.path("state", "hosts")):
        problems.append("a host is still registered after the first window quit")
    if status1 is not None:
        p, lines = serveattach(args, host, guiargs, guicwd)
        problems += p
        more += lines
    text = sessionlog(host, "state", "Stream in the background") or ""
    if '"via":"prompt"' not in text or text.count('"ev":"turn.end"') != 2:
        problems.append("the log lacks the turn or the attached window's prompt")
    later = sessionlog(host, "state", "Stream slowly for a while") or ""
    if '"why":"cancel"' not in later:
        problems.append("the tray's Stop all did not stop the later turn")
    if isinstance(host, Local) and os.name == "posix":
        problems += crashed(args)
    return problems, lines1 + more


def serveattach(args, host, guiargs, guicwd):
    """"prospero serve" hosting the state directory the first window had,
    and a window attaching to it (attachops), which must pass over a
    stale registration. Returns problems and the window's log."""
    problems = []
    state = host.path("state")
    serve = host.popen(["serve", "--state", state, "-c", host.path("prospero.ini"),
                        "--idle", "30"])
    # Its output must keep flowing, or it would block writing it
    threading.Thread(target=lambda: [None for _ in serve.stdout], daemon=True).start()
    lines = []
    regs = waitfor(lambda: host.listdir(host.path("state", "hosts")), 30, 0.5)
    print(f"prospero serve registered: {regs}", flush=True)
    sid = next((s for s in host.listdir(host.path("state", "sessions"))
                if "Stream in the background" in (host.read(host.path(
                    "state", "sessions", s, "events.jsonl")) or "")), None)
    if not regs:
        problems.append("prospero serve never registered")
    elif not sid:
        problems.append("no session of the first window's")
    elif not serveopen(host, state, sid):
        problems.append("the host would not open the session")
    else:
        # A registration whose host is long gone, which must hold up nothing
        stale = host.path("state", "hosts", "99999999.ini")
        dead = host.path("state", "run", "dead.sock") if not isinstance(host, Remote) \
            else "\\\\.\\pipe\\prospero-test-dead-host"
        host.write(stale, f"pid = 99999999\nendpoint = {dead}\nversion = 0.0.0\n"
                          f"proto = 0\nstarted = 1\n")
        # Probing every second while idle, so that a clock drawn with no
        # deadline (a host's age, say) is caught for sure
        status, lines = host.run(["--attach", "--aggressive"] + guiargs, args.timeout,
                                 guicwd, tag="[4] ")
        problems += summaryproblems(status, lines, "the attached window")
        if host.read(stale) is not None:
            problems.append("the stale registration is still there")
    # Its idle time, with nobody left
    try:
        serve.wait(timeout=90)
    except subprocess.TimeoutExpired:
        problems.append("prospero serve did not exit after its idle time")
        serve.kill()  # the test's own child
        serve.wait()
    return problems, lines


def serveopen(host, state, sid):
    """Have the host of state open sid, through a client that then goes,
    so that the host holds it, as a host whose window went holds what it
    ran. Returns whether the open was answered."""
    c = host.popen(["connect", "--local", "--state", state, "-c",
                    host.path("prospero.ini")], stdin=True)
    c.stdin.write(json.dumps({"cmd": "open", "id": 1, "sid": sid, "from": 0}) + "\n")
    c.stdin.flush()
    ok = [False]

    def read():
        for line in c.stdout:
            if '"ev":"reply"' in line and json.loads(line).get("id") == 1:
                ok[0] = json.loads(line).get("ok") == 1
                break
    r = threading.Thread(target=read, daemon=True)
    r.start()
    r.join(30)
    c.stdin.close()  # the link ends, and the host runs on
    try:
        c.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        c.kill()  # the test's own child
        c.wait()
    print(f"the host opened {sid}: {ok[0]}", flush=True)
    return ok[0]


def crashed(args):
    """A window's process killed with SIGKILL leaves its state
    directory's window lock free: another window then takes it, and
    keeps it. POSIX only, here."""
    import fcntl
    problems = []
    prospero = os.path.abspath(args.prospero)
    root = tempfile.mkdtemp(prefix="prospero-crash-")
    state = os.path.join(root, "state")
    lock = os.path.join(state, "run", "gui.lock")

    def held():
        try:
            fd = os.open(lock, os.O_RDONLY)
        except OSError:
            return False
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            return False
        except BlockingIOError:
            return True
        finally:
            os.close(fd)  # and with it the shared lock, if taken

    def start():
        cmd = [prospero, "gui", "--fake", "--statedir", state]
        if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd
        env = dict(os.environ, SDL_WINDOW_ACTIVATE_WHEN_SHOWN="0")
        return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, env=env, start_new_session=True)

    def stop(p, sig):
        pid = guipid(prospero, state)
        if pid:
            P.signal_ours(pid, sig, lambda q: ours(q, state), "GUI")
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            problems.append(f"still running 10 s after {sig.name}")
            P.signal_group(p, signal.SIGKILL)
            p.wait()

    p = start()
    if not waitfor(held, 30, 0.2):
        problems.append("crash: the window never took the lock")
    stop(p, signal.SIGKILL)
    if held():
        problems.append("crash: the lock is still held after SIGKILL")
    else:
        print("crash: the lock is free after SIGKILL", flush=True)
    p = start()
    if not waitfor(held, 30, 0.2) or p.poll() is not None:
        problems.append("crash: a new window did not take the lock")
    else:
        time.sleep(2)
        if p.poll() is not None:
            problems.append(f"crash: the new window exited ({p.returncode})")
        else:
            print("crash: a new window took the lock", flush=True)
    stop(p, signal.SIGTERM)
    shutil.rmtree(root, ignore_errors=True)
    return problems


def lanaddress(host):
    """This machine's address on the way to host."""
    import socket
    addr = socket.getaddrinfo(host, 22, socket.AF_INET)[0][4][0]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((addr, 9))
        return s.getsockname()[0]
    finally:
        s.close()


FAKESSH = ("#!/bin/sh\n"
           "# ssh, as far as the harness uses it: the command, here\n"
           "while [ $# -gt 1 ]; do\n"
           "  case \"$1\" in -o) shift 2;; -*) shift;; *) shift; break;; esac\n"
           "done\n"
           "exec sh -c \"$1\"\n")

# The same for Windows, built there with w64devkit: the remote command
# is a command line of its own, run as is with standard I/O inherited.
FAKESSH_C = r"""
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
int main(void)
{
    int       argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int       i    = 1;
    for (; i < argc-1; i++) {
        if (!wcscmp(argv[i], L"-o")) {
            i++;
        } else if (argv[i][0] != L'-') {
            break;  // the destination
        }
    }
    if (i+1 >= argc) {
        return 255;
    }
    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(0, argv[i+1], 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
        return 255;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 255;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
"""


class Far:
    """The remote host of --mode remote: over real ssh (--ssh), with the
    harness built there, or this machine itself through a fake ssh, its
    host started here for the test. Either way, in directories of its own
    that go at the end."""

    def __init__(self, args, host, url, model, nonce):
        self.args = args
        self.host = host
        self.local = not args.ssh
        self.windows = isinstance(host, Remote)
        self.server = None
        if self.local:
            self.root = host.path("far")
            self.prospero = host.h.exe if self.windows else host.prospero
        else:
            self.root = f"{args.remote_root}/gui-{nonce}"
            self.prospero = args.remote_prospero
        self.state = self.root + "/state"
        self.work = self.root + "/work"
        self.config = self.root + "/prospero.ini"
        cfg = config(url, model) + "\n# Its host goes soon after its last client\n" \
                                   "[host]\nidle_exit = 5\n" \
                                   "\n# A model only it has, for its sessions' picker\n" \
                                   f"[model fake:far:{nonce}]\ncontext = 100000\n"
        if self.local:
            host.mkdir(self.work)
            host.mkdir(self.state)
            host.write(self.config, cfg)
            host.write(self.work + "/hello.txt", "hello world\n")
            if self.windows:
                self.ssh = host.path("fakessh.exe")
                host.write(host.path("fakessh.c"), FAKESSH_C)
                host.h.sh(f"cd {host.root} && gcc -O2 -o fakessh.exe fakessh.c -lshell32")
            else:
                self.ssh = os.path.join(host.root, "fakessh")
                host.write(self.ssh, FAKESSH)
                os.chmod(self.ssh, 0o755)
            # The host first, here: started through the fake ssh, it would
            # descend from the GUI's reaper (Linux: a subreaper), or stay
            # in its job (Windows), and go with the stream dropped
            serve = ["serve", "--state", self.state, "-c", self.config, "--idle", "600"]
            if self.windows:
                self.server = host.popen(serve)
            else:
                self.server = subprocess.Popen(
                    [self.prospero] + serve, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    start_new_session=True)
        else:
            self.sh(f"mkdir -p {self.work} {self.state} && "
                    f"echo 'hello world' > {self.work}/hello.txt")
            b = cfg.replace("'", "")
            self.sh(f"cat > {self.config} <<'EOF'\n{b}EOF\n")

    def sh(self, script):
        """A script there: its login shell's, else (--ssh-windows, whose
        login shell is cmd.exe) w64devkit's sh, reading it."""
        if self.args.ssh_windows:
            script = 'export PATH="$HOME/w64devkit/bin:$PATH"\n' + script + "\n"
            return subprocess.run(["ssh", "-o", "BatchMode=yes", self.args.ssh,
                                   "w64devkit\\bin\\sh.exe -l -s"], input=script,
                                  capture_output=True, text=True, timeout=120).stdout
        return subprocess.run(["ssh", "-o", "BatchMode=yes", self.args.ssh, script],
                              capture_output=True, text=True, timeout=120).stdout

    def section(self):
        dest = "here" if self.local else self.args.ssh
        return (f"\n# The checklist's remote host\n[remote r]\nssh = {dest}\n"
                f"path = {self.prospero}\nstate = {self.state}\nconfig = {self.config}\n"
                f"\n# One that fails: ssh would take its destination for an option\n"
                f"[remote bad]\nssh = -oProxyCommand=false\n")

    def env(self):
        return {"PROSPERO_TEST_SSH": self.ssh} if self.local else {}

    def hosts(self):
        """The hosts running for its state directory, as JSONL lines."""
        if self.local:
            r = subprocess.run([self.prospero, "hosts", "--state", self.state],
                               capture_output=True, text=True)
            return [l for l in r.stdout.splitlines() if l.strip()]
        out = self.sh(f"'{self.prospero}' hosts --state '{self.state}'")
        return [l for l in out.splitlines() if l.strip()]

    def close(self, keep):
        """Over ssh, its host, left by the GUI, runs on until its idle
        exit: wait for that, and say if it did not. Here, the host started
        for the test stops with it."""
        problems = []
        if not self.local and waitfor(lambda: not self.hosts(), 40, 2) is None:
            problems.append(f"the remote host still runs: {self.hosts()}")
        if self.server and self.windows:
            # Told to quit, through a client of its own
            c = self.host.popen(["connect", "--local", "--state", self.state,
                                 "-c", self.config], stdin=True)
            c.stdin.write('{"cmd":"quit","id":1}\n')
            c.stdin.flush()
            try:
                c.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                c.kill()
            try:
                self.server.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                problems.append("the host started for the test did not quit")
                self.server.kill()
        elif self.server and self.server.poll() is None:
            self.server.terminate()  # ours: started above
            self.server.wait(timeout=10)
        if not keep and not self.windows:
            if self.local:
                shutil.rmtree(self.root, ignore_errors=True)
            else:
                self.sh(f"rm -rf {self.root}")
        return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prospero", required=True,
                    help="the binary, or with --host the .exe on that host")
    ap.add_argument("--subprocess", action="store_true",
                    help="the engine as prospero --headless")
    ap.add_argument("--inbox-cap", type=int,
                    help="the GUI's inbox, small to force lost records (default "
                         "32768, but the GUI's own for --mode bench, which "
                         "measures how fast a history arrives)")
    ap.add_argument("--mode", default="drive",
                    choices=["drive", "idle", "streamtest", "e2e", "signals",
                             "settings", "enginestop", "bench", "background", "remote",
                             "compose", "sessions"])
    ap.add_argument("--ssh", help="with --mode remote: the remote host, over real ssh "
                                  "(default: this machine, through a fake ssh)")
    ap.add_argument("--remote-prospero", help="with --ssh: the harness built there")
    ap.add_argument("--remote-root", default="/tmp",
                    help="with --ssh: where its directories go there")
    ap.add_argument("--ssh-windows", action="store_true",
                    help="with --ssh: a Windows host whose login shell is cmd.exe, "
                         "with w64devkit for the test's scripts")
    ap.add_argument("--model", help="with --mode e2e: the model (required)")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--keep", action="store_true", help="keep the directories")
    ap.add_argument("--url", help="with --mode e2e: the endpoint (default http://localhost/)")
    ap.add_argument("--shots", help="save the checklist's screenshots here")
    ap.add_argument("--host", help="run the GUI on this Windows host's console")
    ap.add_argument("--trace", action="store_true", help="log where presses land")
    ap.add_argument("--fake", action="store_true",
                    help="the fake agent, no engine: M1's checklist and measurements")
    args = ap.parse_args()
    if args.mode == "signals":
        return signals(args)

    if args.mode == "remote" and args.ssh and not args.remote_prospero:
        ap.error("--ssh needs --remote-prospero")
    if args.mode == "e2e" and not args.model:
        ap.error("--mode e2e needs --model")
    # A remote host over ssh reaches the provider on this machine's address
    bind = lanaddress(args.ssh) if args.mode == "remote" and args.ssh else "127.0.0.1"
    srv, url = fake_provider.start(bind)
    nonce = f"{random.randrange(1 << 32):08x}"
    model = f"fake:gui:{nonce}"
    fake_provider.STATE.register(model)
    for name, listing in M4MODELS.items():
        fake_provider.STATE.register(f"fake:{name}:{nonce}", **listing)
    host = Remote(args, srv.server_address[1]) if args.host else Local(args)
    work = host.path("work")
    state = host.path("state")
    ini = host.path("prospero.ini")
    host.mkdir(work)
    host.mkdir(state)
    host.write(host.path("work", "hello.txt"), "hello world\n")
    if args.mode == "enginestop":
        args.subprocess = True  # the GUI's --enginestop implies it
    curl = host.url if args.host else url
    if args.mode == "e2e":
        before = e2econfig(args.url or "http://localhost/", args.model)
    elif args.mode == "settings":
        # Asks wait out the screenshots; a role holds the long value; an
        # included model, which cannot be deleted here, an endpoint to
        # delete, and a remote, never connected
        host.write(host.path("team.ini"), "# Shared\n[model shared]\ntemperature = 0.1\n")
        before = config(curl, model, 120, SETTINGS_EXTRA)
        head, _, rest = before.partition("\n")
        before = f"{head}\ninclude = team.ini\n{rest}"
    elif args.mode == "enginestop":
        before = config(curl, model, 60)
    elif args.mode == "compose":
        if args.host:
            ap.error("--mode compose runs only here: its test editor is a sh script")
        editor = host.path("edit.sh")
        host.write(editor, COMPOSE_EDITOR)
        # A skill of the workspace's, for /name's completion
        host.mkdir(host.path("work", ".claude", "skills", "zzcheck"))
        host.write(host.path("work", ".claude", "skills", "zzcheck", "SKILL.md"),
                   "---\ndescription: A skill of the checklist's\n---\nCheck.\n")
        before = config(curl, model, extra=composeconfig(editor, nonce))
    elif args.mode == "sessions":
        # A model to switch to; a remote listed always, never reached,
        # and one listed only while connected
        before = config(curl, model, 60,
                        f"\n[model fake:gui_b:{nonce}]\ntemperature = 0.5\n"
                        "\n[remote far]\nssh = far.invalid\n"
                        "\n[remote hidden]\nssh = hidden.invalid\nlist = connected\n")
        if isinstance(host, Local):
            # A role with the desktop tool, its helper the fake (which
            # every process of the run inherits), for a subagent's ask
            # allowed always; the checklist skips it without the role.
            # Not in drive mode: there the configuration would outgrow
            # its small inbox.
            os.environ["PROSPERO_TEST_DESKTOP"] = json.dumps(
                [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                              "fake_desktop.py")])
            before += DESKROLE
    elif args.mode == "drive" and not args.fake:
        before = config(curl, model, extra=m4config(curl, nonce))
        # A directory for the ask allowed always
        host.mkdir(host.path("gui-always"))
        # Instruction files for the info popup, and files to compact over
        host.write(host.path("work", "AGENTS.md"), "# Checklist rules\n\nKeep it short.\n")
        bigfiles(host, "small-", 8, 160)
        bigfiles(host, "big-", 10, 512)
    else:
        before = config(curl, model)
    far = None
    if args.mode == "remote":
        far = Far(args, host, curl, model, nonce)
        # An include by URL, to fetch again from the settings window
        head, _, rest = before.partition("\n")
        before = f"{head}\ninclude = {curl}/fake/include.ini\n{rest}" + far.section()
    host.write(ini, before)

    holder = None
    if args.mode == "drive" and not args.fake:
        holder = host.sessions(ini, state, work)

    paths = [ini, state, work]
    guicwd = None
    if args.mode == "drive" and not args.host:
        # Relative to the GUI's directory, which the engine process (in
        # the state directory) does not share
        guicwd = host.root
        paths = [os.path.relpath(p, guicwd) for p in paths]
    guiargs = ["-c", paths[0], "--statedir", paths[1], "--cwd", paths[2]]
    if args.mode != "background":
        guiargs.insert(0, f"--{args.mode}")  # it runs two checklists
    if args.inbox_cap or args.mode not in ("bench", "background"):
        guiargs += ["--inbox-cap", str(args.inbox_cap or 32768)]
    if far:
        guiargs += ["--rcwd", far.work]
    if args.subprocess:
        guiargs.append("--engine=subprocess")
    if args.trace:
        guiargs.append("--trace-drive")
    if args.fake:
        guiargs.append("--fake")
    if args.shots and not args.host:
        os.makedirs(args.shots, exist_ok=True)
        guiargs += ["--shots", os.path.abspath(args.shots)]

    t0 = time.monotonic()
    problems = []
    if args.mode == "background":
        problems, lines = background(args, host, guiargs, guicwd)
        status = 0
    elif args.mode == "compose":
        # Twice: the second finds the drafts the first left
        status, lines = host.run(guiargs, args.timeout, guicwd)
        problems += summaryproblems(status, lines, "the window")
        status = 0
        if not problems:
            again = ["--compose-again" if a == "--compose" else a for a in guiargs]
            st, more = host.run(again, args.timeout, guicwd, tag="again: ")
            problems += summaryproblems(st, more, "the window again")
            lines += more
        left = host.listdir(host.path("state", "edit"))
        if left:
            problems.append("the editor's files are left: " + ", ".join(left))
        if args.shots:
            for name in ("history", "menu", "editing", "editfailed", "slash", "models",
                         "drafts", "compacted", "restored"):
                if not os.path.exists(os.path.join(args.shots, f"compose-{name}.png")):
                    problems.append(f"no compose-{name}.png")
    else:
        status, lines = host.run(guiargs, args.timeout, guicwd,
                                 env=far.env() if far else None)
    dt = time.monotonic() - t0

    if status is None:
        problems.append("the harness did not exit in time")
    elif status != 0:
        problems.append(f"exit status {status}")
    out = "".join(lines)
    if args.mode == "remote":
        problems += summaryproblems(status, lines, "the window")
        with fake_provider.STATE.lock:
            fetched = fake_provider.STATE.includes
        if fetched < 2:
            problems.append(f"the URL include was fetched {fetched} times, not again")
        else:
            print(f"the URL include was fetched {fetched} times")
        problems += far.close(args.keep)
    if args.mode in ("drive", "e2e", "settings", "enginestop", "sessions"):
        summary = [l for l in lines if "drive:" in l and " checks, " in l]
        if not summary:
            problems.append("no checklist summary")
        else:
            s = summary[-1]
            if " 0 failed" not in s or " 0 stale" not in s:
                problems.append("checklist: " + s.strip())
        if "STALE" in out:
            problems.append("stale frames reported")
    if args.mode == "settings":
        after = host.read(ini) or ""
        if f"prompt = {LONG_PROMPT}!\n" not in after:
            problems.append("the long value was not saved whole with its edit")
        if "[ui]\ntheme = light\n" not in after:
            problems.append("the theme was not saved as light under [ui]")
        # The edits, in order: the long value, the theme, the roots, the
        # deleted endpoint with its comment, the new model
        want = before.replace(f"prompt = {LONG_PROMPT}\n", f"prompt = {LONG_PROMPT}!\n")
        want = want.replace("temp = ask\n", "temp = ask\nroots = ~/a; ~/b\n")
        want = want.replace(SETTINGS_GONE + "\n", "")
        want += "\n[ui]\ntheme = light\n\n[model fresh]\nreasoning = reasoning_content\n"
        if after != want:
            diff = difflib.unified_diff(want.splitlines(), after.splitlines(),
                                        "expected", "saved", lineterm="", n=1)
            problems.append("the configuration is not the edits alone:\n" + "\n".join(diff))
        else:
            print("configuration: the long value, the theme, the roots, a section "
                  "deleted and one added, nothing else")
        if host.read(host.path("team.ini")) != "# Shared\n[model shared]\ntemperature = 0.1\n":
            problems.append("the included file changed")
        if args.shots and not args.host:
            for name in ("dark", "light", "settings-narrow", "settings-wide", "delete",
                         "settings-narrowest"):
                if not os.path.exists(os.path.join(args.shots, name + ".png")):
                    problems.append(f"no {name}.png")
    if args.mode == "sessions" and args.shots and not args.host:
        for name in ("sessions-tree", "sessions-folded", "sessions-menu", "sessions-group",
                     "sessions-archived", "sessions-picker", "sessions-export",
                     "sessions-forkhere", "sessions-forked", "sessions-delete",
                     "sessions-import", "sessions-imported"):
            if not os.path.exists(os.path.join(args.shots, name + ".png")):
                problems.append(f"no {name}.png")
    if args.mode == "enginestop":
        time.sleep(0.5)
        found = P.parse(host.read(host.path("work", "close.pid")))
        pid = found[-1] if found else None
        if pid is None:
            problems.append("no close.pid")
        elif host.alive(pid):
            problems.append(f"the command (pid {pid}) outlived its engine")
        else:
            print(f"close: pid {pid} gone with the engine")
    if args.mode in ("drive", "e2e") and not args.fake:
        after = host.read(ini) or ""
        diff = list(difflib.unified_diff(before.splitlines(), after.splitlines(),
                                         lineterm="", n=0))
        body = [l for l in diff if l[:1] in "+-" and l[:3] not in ("+++", "---")]
        added = [l for l in body if l.startswith("+")]
        if len(added) != 1 or len(body) > 2:
            problems.append(f"the configuration diff is not one line: {body}")
        else:
            print("configuration diff: " + " | ".join(body))
        # Gone: the cancelled command, the one quitting stopped, and the
        # job killed from its panel
        time.sleep(0.5)
        names = ("cancel", "close") + (("job",) if args.mode == "drive" else ())
        for name in names:
            found = P.parse(host.read(host.path("work", f"{name}.pid")))
            pid = found[-1] if found else None
            if pid is None:
                problems.append(f"no {name}.pid")
            elif host.alive(pid):
                problems.append(f"the {name} command (pid {pid}) is still running")
            else:
                print(f"{name}: pid {pid} gone")
    if holder:
        # Its input ends, so it stops
        holder.stdin.close()
        try:
            holder.wait(timeout=10)
        except subprocess.TimeoutExpired:
            holder.kill()
    if isinstance(host, Local):
        # A desktop helper's reaper (the sessions mode's subagent) goes
        # a few seconds after the engine that started it
        deadline = time.monotonic() + 5
        while strays(host.root) and time.monotonic() < deadline:
            time.sleep(0.25)
    left = host.strays()
    if left:
        problems.append("processes left: " + "; ".join(left))

    srv.shutdown()
    host.close(args.keep)
    if problems:
        for msg in problems:
            print("FAIL: " + msg)
        return 1
    where = f" on {args.host}" if args.host else ""
    print(f"ok: {args.mode}{' (subprocess)' if args.subprocess else ''}{where} in {dt:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
