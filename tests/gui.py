#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The GUI's scripted checklist against the fake provider.

Runs "harness gui --drive" with a real engine, in this process or as
"harness --headless" (--subprocess), in fresh working, state and
configuration directories, against fake_provider.py's "gui" scenario.
It passes when the checklist reports no failed checks and no stale
frames, the harness exits cleanly in time, the settings edit changed one
line of the configuration file, and the commands it cancelled or stopped
by quitting are gone, with no engine process left behind.

  uv run tests/gui.py --harness build/harness [--subprocess] [--inbox-cap N]
  uv run tests/gui.py --harness build/harness --mode idle      (60 s)
  uv run tests/gui.py --harness build/harness --mode streamtest
  uv run tests/gui.py --harness build/harness --mode signals   (POSIX)
  uv run tests/gui.py --harness build/harness --mode settings [--shots DIR]
  uv run tests/gui.py --harness build/harness --mode enginestop
  uv run tests/gui.py --harness build/harness --fake [--mode idle]   (M1's)
  uv run tests/gui.py --harness build/harness --mode bench   (M1's scale)

The settings mode edits a value longer than a field's slack and picks
the theme, dark then light, from its list over a transcript that shows
most of what one can (screenshots of each with --shots). The enginestop
mode kills the engine process mid-turn and mid-ask: the GUI must stop
counting either, and quit at once when closed.

On Linux without a display it runs under xvfb-run. --serve only runs the
provider and writes the configuration, for a GUI started elsewhere (a
Windows console session): it prints the directories and waits.
"""

import argparse
import difflib
import json
import os
import random
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
        "# Asks wait a little\n"
        "[guard]\n"
        "ask_timeout = 10\n"
    )


# A value longer than a settings field's slack of 1024 bytes
LONG_PROMPT = " ".join(f"Rule {i}: keep every change small and tested." for i in range(40))


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
        "# Asks wait briefly\n"
        "[guard]\n"
        f"ask_timeout = {ask}\n"
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
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def readpid(path):
    try:
        with open(path) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


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


def guipid(harness, state):
    """The GUI process: the harness, not xvfb-run, not the engine."""
    try:
        out = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True,
                             text=True).stdout
    except OSError:
        return None
    for line in out.splitlines():
        pid, _, args = line.strip().partition(" ")
        if args.startswith(harness + " gui ") and state in args:
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
        self.root = tempfile.mkdtemp(prefix="harness-gui-")
        self.harness = os.path.abspath(args.harness)

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

    def run(self, guiargs, timeout, cwd=None):
        """Run the GUI, echoing its log, in cwd (default the working
        directory). Returns (status, lines)."""
        cmd = [self.harness, "gui"] + guiargs
        if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd
        print(" ".join(cmd), flush=True)
        p = subprocess.Popen(cmd, cwd=cwd or self.path("work"), stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, start_new_session=True)
        lines = []
        meter = None
        deadline = time.monotonic() + timeout
        # A GUI that hangs prints nothing more: end it at the deadline
        killer = threading.Timer(timeout + 5, lambda: os.killpg(p.pid, signal.SIGKILL))
        killer.start()
        try:
            for line in p.stdout:
                lines.append(line)
                sys.stdout.write(line)
                sys.stdout.flush()
                if "drive: mark: " in line:
                    pid = guipid(self.harness, self.path("state"))
                    if pid:
                        meter = Meter(pid, line.split("drive: mark: ")[1].strip())
                elif line.startswith("harness: rates: ") and meter:
                    print("measured: " + meter.report(), flush=True)
                    meter = None
                if time.monotonic() > deadline:
                    break
            p.wait(timeout=max(1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
        finally:
            killer.cancel()
        if p.poll() is None or p.returncode == -signal.SIGKILL:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            p.wait()
            return None, lines
        return p.returncode, lines

    def popen(self, args, stdin=False):
        return subprocess.Popen(
            [self.harness] + args, cwd=self.path("work"),
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def sessions(self, ini, state, work):
        return makesessions(self, ini, state, work)

    def strays(self):
        left = strays(self.root)
        for line in left:
            try:
                os.kill(int(line.split()[0]), signal.SIGKILL)
            except (OSError, ValueError):
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
        self.h = scenarios.SshHost(args.host, args.harness, port, rport)
        self.root = self.h.root
        self.url = self.h.url
        self.task = f"harness-gui-{random.randrange(1 << 32):08x}"

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

    def run(self, guiargs, timeout, cwd=None):
        win = lambda p: p.replace("/", "\\")
        q = lambda a: '"' + win(a) + '"' if "/" in a or " " in a else a
        bat = ("@echo off\r\ncd /d \"%~dp0\"\r\n" +
               q(self.h.exe) + " gui " + " ".join(q(a) for a in guiargs) +
               " > run.log 2>&1\r\necho done %errorlevel% >> run.log\r\n")
        self.h.write(self.path("run.bat"), bat)
        r = self.h.ssh(f'schtasks /create /tn {self.task} /tr "{win(self.path("run.bat"))}" '
                       f'/sc once /st 23:59 /it /f')
        if r.returncode:
            raise RuntimeError("schtasks /create: " + r.stdout + r.stderr)
        self.h.ssh(f"schtasks /run /tn {self.task}")
        print(f"{self.args.host}: {bat.splitlines()[2]}", flush=True)
        seen = 0
        deadline = time.monotonic() + timeout
        status = None
        lines = []
        mark = None  # (pid, seconds of CPU, when) at a measurement's start
        while time.monotonic() < deadline:
            time.sleep(1 if self.args.mode in ("idle", "streamtest") else 3)
            text = self.h.read(self.path("run.log")) or ""
            lines = text.splitlines(keepends=True)
            for line in lines[seen:]:
                sys.stdout.write(line)
                if "drive: mark: " in line:
                    pid = self.guipid()
                    mark = (pid, self.cputime(pid), time.monotonic()) if pid else None
                elif line.startswith("harness: rates: ") and mark:
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

    def sessions(self, ini, state, work):
        return makesessions(self, ini, state, work)

    def guipid(self):
        ps = ("Get-CimInstance Win32_Process -Filter \\\"Name='harness.exe'\\\" | Where-Object "
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
            self.h.ssh(f"taskkill /F /PID {line.split()[0]}")
        return left

    def close(self, keep):
        self.h.ssh(f"schtasks /delete /tn {self.task} /f")
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
    harness = os.path.abspath(args.harness)
    for sig in (signal.SIGTERM, signal.SIGINT):
        root = tempfile.mkdtemp(prefix="harness-sig-")
        state = os.path.join(root, "state")
        cmd = [harness, "gui", "--fake", "--statedir", state]
        if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
                and not os.environ.get("WAYLAND_DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd
        env = dict(os.environ, SDL_WINDOW_ACTIVATE_WHEN_SHOWN="0")
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        time.sleep(8)  # past the probe and the layout's save: idle
        pid = guipid(harness, state)
        if not pid:
            problems.append(f"{sig.name}: no GUI process")
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            continue
        t0 = time.monotonic()
        os.kill(pid, sig)
        try:
            p.wait(timeout=5)
            print(f"{sig.name}: the idle GUI quit in {time.monotonic() - t0:.1f} s")
        except subprocess.TimeoutExpired:
            problems.append(f"{sig.name}: still running 5 s after the signal")
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        shutil.rmtree(root, ignore_errors=True)
    for msg in problems:
        print("FAIL: " + msg)
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True,
                    help="the binary, or with --host the .exe on that host")
    ap.add_argument("--subprocess", action="store_true",
                    help="the engine as harness --headless")
    ap.add_argument("--inbox-cap", type=int,
                    help="the GUI's inbox, small to force lost records (default "
                         "32768, but the GUI's own for --mode bench, which "
                         "measures how fast a history arrives)")
    ap.add_argument("--mode", default="drive",
                    choices=["drive", "idle", "streamtest", "e2e", "signals",
                             "settings", "enginestop", "bench"])
    ap.add_argument("--model", default="qwen-flash", help="with --mode e2e")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--keep", action="store_true", help="keep the directories")
    ap.add_argument("--url", help="with --mode e2e: the router (default http://localhost/)")
    ap.add_argument("--shots", help="save the checklist's screenshots here")
    ap.add_argument("--host", help="run the GUI on this Windows host's console")
    ap.add_argument("--trace", action="store_true", help="log where presses land")
    ap.add_argument("--fake", action="store_true",
                    help="the fake agent, no engine: M1's checklist and measurements")
    args = ap.parse_args()
    if args.mode == "signals":
        return signals(args)

    srv, url = fake_provider.start()
    nonce = f"{random.randrange(1 << 32):08x}"
    model = f"fake:gui:{nonce}"
    fake_provider.STATE.register(model)
    for name, listing in M4MODELS.items():
        fake_provider.STATE.register(f"fake:{name}:{nonce}", **listing)
    host = Remote(args, srv.server_address[1]) if args.host else Local(args)
    work = host.path("work")
    state = host.path("state")
    ini = host.path("harness.ini")
    host.mkdir(work)
    host.mkdir(state)
    host.write(host.path("work", "hello.txt"), "hello world\n")
    if args.mode == "enginestop":
        args.subprocess = True  # the GUI's --enginestop implies it
    curl = host.url if args.host else url
    if args.mode == "e2e":
        before = e2econfig(args.url or "http://localhost/", args.model)
    elif args.mode == "settings":
        # Asks wait out the screenshots; a role holds the long value
        before = config(curl, model, 120,
                        f"\n# A long value\n[role long]\nprompt = {LONG_PROMPT}\n")
    elif args.mode == "enginestop":
        before = config(curl, model, 60)
    elif args.mode == "drive" and not args.fake:
        before = config(curl, model, extra=m4config(curl, nonce))
        # Instruction files for the info popup, and files to compact over
        host.write(host.path("work", "AGENTS.md"), "# Checklist rules\n\nKeep it short.\n")
        bigfiles(host, "small-", 8, 160)
        bigfiles(host, "big-", 10, 512)
    else:
        before = config(curl, model)
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
    guiargs = [f"--{args.mode}", "-c", paths[0], "--statedir", paths[1],
               "--cwd", paths[2]]
    if args.inbox_cap or args.mode != "bench":
        guiargs += ["--inbox-cap", str(args.inbox_cap or 32768)]
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
    status, lines = host.run(guiargs, args.timeout, guicwd)
    dt = time.monotonic() - t0

    problems = []
    if status is None:
        problems.append("the harness did not exit in time")
    elif status != 0:
        problems.append(f"exit status {status}")
    out = "".join(lines)
    if args.mode in ("drive", "e2e", "settings", "enginestop"):
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
        rest = after.replace(f"prompt = {LONG_PROMPT}!\n", f"prompt = {LONG_PROMPT}\n")
        rest = rest.replace("\n[ui]\ntheme = light\n", "")
        if rest != before:
            problems.append("the configuration changed more than the two edits")
        else:
            print("configuration: the long value and the theme, nothing else")
        if args.shots and not args.host:
            for name in ("dark", "light"):
                if not os.path.exists(os.path.join(args.shots, name + ".png")):
                    problems.append(f"no {name}.png")
    if args.mode == "enginestop":
        time.sleep(0.5)
        text = host.read(host.path("work", "close.pid"))
        pid = int(text.strip()) if text and text.strip().isdigit() else None
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
            text = host.read(host.path("work", f"{name}.pid"))
            pid = int(text.strip()) if text and text.strip().isdigit() else None
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
