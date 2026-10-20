#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine scenarios: the harness against the fault-injecting fake provider.

Each scenario runs the real binary ("harness run", or "harness --headless"
for cancels) in a fresh working and state directory, with a model name
that selects a script in fake_provider.py, then checks the event sequence,
the files left behind, the exit code and a wall-clock bound.

  uv run tests/scenarios.py --harness build/harness [NAME ...]
  uv run tests/scenarios.py --harness PATH --host biron.local   (Windows)
  uv run tests/scenarios.py --harness build/harness --soak 30

With --host, the fake provider runs here and the harness runs on the
Windows host over ssh, reaching the provider through a reverse tunnel;
PATH is the harness.exe already on that host. busybox.exe is copied next
to it from the host's w64devkit, which is how the harness finds its shell.
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
import subprocess
import sys
import tempfile
import threading
import time
import traceback

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402


# Hosts: where the harness runs

class LocalHost:
    windows = os.name == "nt"

    def __init__(self, harness, url):
        self.harness = os.path.abspath(harness)
        self.url = url
        self.root = tempfile.mkdtemp(prefix="harness-scen-")

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

    def popen(self, args, cwd, stdin=False):
        return subprocess.Popen(
            [self.harness] + args, cwd=cwd,
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1)

    def alive(self, pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        # A zombie still answers: look at its state
        try:
            out = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                                 capture_output=True, text=True).stdout
            return bool(out.strip()) and not out.strip().startswith("Z")
        except OSError:
            return True

    def killhard(self, proc, pid=None):
        if pid:
            os.kill(pid, signal.SIGKILL)
        else:
            proc.kill()

    def killpid(self, pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


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
        self.sh(f"cd / && rm -rf {self.root}")
        subprocess.run(["ssh", "-S", self.ctl, "-O", "exit", self.host],
                       capture_output=True)

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

    def popen(self, args, cwd, stdin=False):
        def q(a):
            return '"' + a + '"' if (" " in a or not a) else a
        win = lambda p: p.replace("/", "\\")
        cmd = f"cd /d {win(cwd)} && {win(self.exe)} " + " ".join(q(a) for a in args)
        return subprocess.Popen(
            ["ssh", "-S", self.ctl, self.host, cmd],
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1)

    def alive(self, pid):
        r = self.ssh(f"tasklist /FI \"PID eq {pid}\" /NH")
        return str(pid) in r.stdout

    def killhard(self, proc, pid=None):
        if pid:
            self.ssh(f"taskkill /F /PID {pid}")
        else:
            proc.kill()

    def killpid(self, pid):
        self.ssh(f"taskkill /F /PID {pid}")


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

    def all(self, ev):
        return [e for e in self.events if e.get("ev") == ev]

    def last(self, ev):
        found = self.all(ev)
        return found[-1] if found else {}

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
        fake_provider.STATE.register(self.model)
        base = host.join(host.root, f"{name}-{self.nonce}")
        self.work = host.join(base, "work")
        self.state = host.join(base, "state")
        self.config = host.join(base, "harness.ini")
        host.mkdir(self.work)
        host.mkdir(self.state)
        # Endpoint settings, and with "_top" settings before any section
        conf = dict(conf or {})
        top = conf.pop("_top", {})
        ini = {"first_byte_timeout": 5, "idle_timeout": 3,
               "progress_timeout": 6, "connect_timeout": 2}
        ini.update(conf)
        body = "".join(f"{k} = {v}\n" for k, v in top.items())
        body += f"[endpoint fake]\nurl = {host.url}/\n" + "".join(
            f"{k} = {v}\n" for k, v in ini.items())
        host.write(self.config, body)
        self.notes = []

    def args(self, extra=()):
        return ["-C", self.work, "--state", self.state, "-c", self.config,
                "-e", "fake", "-m", self.model] + list(extra)

    def run(self, prompt="go", extra=(), timeout=60, resume=None, watch=None):
        args = ["run"] + self.args(extra)
        if resume:
            args += ["--resume", resume]
        if prompt:
            args.append(prompt)
        t0 = time.monotonic()
        p = self.host.popen(args, self.work)
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

    def headless(self):
        return Headless(self)

    def file(self, rel):
        return self.host.read(self.host.join(self.work, rel))

    def put(self, rel, data):
        self.host.write(self.host.join(self.work, rel), data)

    def requests(self):
        return fake_provider.STATE.log(self.model)

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
        text = self.file(rel) or ""
        return [int(x) for x in text.split() if x.strip().isdigit()]

    def gone(self, pids, within=5.0):
        deadline = time.monotonic() + within
        while time.monotonic() < deadline:
            left = [p for p in pids if self.host.alive(p)]
            if not left:
                return True
            time.sleep(0.2)
        return False


class Headless:
    def __init__(self, t):
        self.t = t
        self.p = t.host.popen(["--headless"] + t.args(), t.work, stdin=True)
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


def scenario(conf=None, bound=30, posix=False):
    def deco(fn):
        SCENARIOS[fn.__name__] = (fn, conf, bound, posix)
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


@scenario(bound=5)
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
    r = t.run()
    t.check(r.rc == 1, f"exit {r.rc}")
    end = r.last("turn.end")
    t.check(end.get("err", "").startswith("overflow"), f"classified as overflow: {end}")
    t.check(len(t.requests()) == 1, "not retried")


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
    t.check(t.file("r.txt") == "line1\nline2", f"raw newline repaired: {t.file('r.txt')!r}")
    t.check(t.file("s.txt") == "ok", "double-encoded arguments repaired")
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


@scenario(bound=10)
def shell_timeout(t):
    r = t.run()
    t.check(r.rc == 0, f"exit {r.rc}: {r.stderr}")
    end = r.all("tool.end")[0]
    t.check(end["status"] == "timeout" and "timed out after 1 seconds" in end["text"],
            f"timed out: {end}")
    t.check(end["ms"] < 4500, f"TERM then KILL within bounds: {end['ms']} ms")
    pids = t.pids("pid1") + t.pids("pid2")
    t.check(len(pids) == 2, f"pids recorded: {pids}")
    t.check(t.gone(pids), f"the tree is gone: {pids}")


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
    t.check(t.file("x.txt") == "yes", "the corrected call succeeds")


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
        t.check(outside_file(t) == "outside", "the file was written outside")
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
        t.check(t.file("inside.txt") == "inside", "the agent wrote inside instead")
        msgs = t.requests()[1]["body"]["messages"]
        t.check(msgs[-1].get("role") == "tool" and "keep it inside" in msgs[-1].get("content", ""),
                "the agent read the denial")
    finally:
        rc = h.close()
    t.check(rc == 0, f"headless exit {rc}")


@scenario(conf={"_top": {"ask_timeout": 2}}, bound=10)
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
        t.check(t.file("inside.txt") == "inside", "the agent wrote inside instead")
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
        t.check(t.file("steered.txt") == "no, put it in steered.txt", "and acts on it")
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
        t.check(t.file("inside.txt") == "inside", "the agent wrote inside instead")
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
    r = h.call("quit", mode="stop")
    t.check(r.get("ok") == 1, f"quit accepted: {r}")
    rc = h.exit(10)
    dt = time.monotonic() - t0
    t.check(rc == 0, f"exit {rc}")
    t.check(dt < 3, f"quit within bounds: {dt:.1f} s")
    te = h.all("tool.end", sid)
    t.check(te and te[-1]["status"] == "cancelled", f"the tool was cancelled: {te}")
    end = h.all("turn.end", sid)
    t.check(end and end[-1]["why"] == "cancel", f"the turn ended: {end}")
    t.check(pids and t.gone(pids), f"its process is gone: {pids}")


SCENARIOS["503x3"] = SCENARIOS.pop("x503x3")


def runone(host, name, verbose):
    fn, conf, bound, posix = SCENARIOS[name]
    if posix and host.windows:
        return name, "skip", 0.0, "POSIX only"
    t0 = time.monotonic()
    try:
        t = T(host, "503x3" if name == "503x3" else name, conf)
        fn(t)
        dt = time.monotonic() - t0
        if dt > bound:
            return name, "FAIL", dt, f"took {dt:.1f} s, bound {bound} s"
        return name, "ok", dt, ""
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
    ap.add_argument("-j", type=int, default=0, help="scenarios at once")
    ap.add_argument("--soak", type=float, help="soak for this many minutes")
    ap.add_argument("--workers", type=int, default=4, help="soak sessions at once")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()

    srv, url = fake_provider.start()
    port = srv.server_address[1]
    if args.host:
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
        jobs = args.j or (2 if args.host else 8)
        failed = 0
        with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
            futs = [ex.submit(runone, host, n, args.v) for n in names]
            for f in futs:
                name, status, dt, why = f.result()
                print(f"{status:4} {name:18} {dt:6.1f} s  {why.splitlines()[0] if why else ''}")
                if status == "FAIL":
                    failed += 1
                    if args.v and why:
                        print("     " + why.replace("\n", "\n     "))
        print(f"{len(names) - failed}/{len(names)} scenarios passed")
        return 1 if failed else 0
    finally:
        fake_provider.STATE.stop.set()
        host.close()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
