#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine hosts and remote clients: prospero serve, attach and connect.

Each scenario runs the real binary against tests/fake_provider.py in a
fresh state directory, speaking frames (src/frame.cpp) to "prospero
attach" through a client written here, or protocol v0 lines to "prospero
connect", and checks what arrives: every durable record of a session
once and in order (the same bytes as its log), and the deltas of a
response exactly once even when the stream breaks in the middle.

  uv run tests/remote.py --prospero build/prospero [NAME ...]
  uv run tests/remote.py --prospero build/prospero --ssh HOST \\
      --remote-prospero /tmp/prospero-remote/build/prospero

With --ssh the ssh scenarios run too: "prospero connect --ssh HOST" against
the harness already built on that host, which reaches the fake provider
through a reverse tunnel. Its ssh is a wrapper that records its pid, so
that a scenario can kill it in the middle of a stream. A host that cannot
forward a port back, like skeeto/sshd on XP, takes --no-tunnel (the
provider then listens on this machine's LAN address), and --shell for
its scripts:

  uv run tests/remote.py --prospero build/prospero --ssh XPHOST \\
      --no-tunnel --shell "sh -s" --remote-prospero C:/.../prospero.exe \\
      --remote-root C:/DOCUME~1/USER/LOCALS~1/Temp/prospero-remote \\
      ssh_reconnect ssh_offline
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import random
import re
import shutil
import signal
import socket
import struct
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
from fake_provider import S, chunk, finish  # noqa: E402


# Scenarios of the fake provider for these tests

def sc_remote_slow(model, n, req):
    """A long answer in many small pieces, over about 4 s."""
    yield S.head()
    for i in range(40):
        yield S.data(chunk(model, {"content": f"w{i} "}))
        yield S.sleep(0.1)
    yield from finish(model)


def sc_remote_tool(model, n, req):
    """A command, then a slow answer: several durable records."""
    if n == 0:
        yield from fake_provider.tool(model, n, "shell", {"command": "echo hi"})
    else:
        yield from sc_remote_slow(model, n, req)


def sc_remote_big(model, n, req):
    """A command, then a long answer in 2 kB pieces, over about 3 s: far
    more than a small queue holds."""
    if n == 0:
        yield from fake_provider.tool(model, n, "shell", {"command": "echo hi"})
        return
    yield S.head()
    for i in range(300):
        words = " ".join(hashlib.sha256(f"{i}.{k}".encode()).hexdigest() for k in range(30))
        yield S.data(chunk(model, {"content": f"<{i}> {words}\n"}))
        if i % 10 == 0:
            yield S.sleep(0.1)
    yield from finish(model)


fake_provider.SCENARIOS["remote_slow"] = sc_remote_slow
fake_provider.SCENARIOS["remote_tool"] = sc_remote_tool
fake_provider.SCENARIOS["remote_big"] = sc_remote_big


# Frames

REGULAR, ACK, DISCONNECT, REPLAY, KEEPALIVE = 1, 3, 5, 6, 9
HEAD = struct.Struct(">BIII")


class Failure(Exception):
    pass


class Link:
    """The client's side of a link, as frame.cpp's: it outlives streams."""

    def __init__(self):
        self.token = random.getrandbits(64) | 1
        self.nextid = 1
        self.kept = []       # [(id, payload)] not yet acknowledged
        self.recvd = 0
        self.peer = None
        self.dups = 0        # regular frames received twice
        self.events = []     # "ready", "lost", "fresh"

    def frame(self, typ, fid, payload=b""):
        return HEAD.pack(typ, fid & 0xffffffff, self.recvd & 0xffffffff,
                         len(payload)) + payload

    def replay(self):
        first = self.kept[0][0] if self.kept else self.nextid
        return self.frame(REPLAY, first, struct.pack(">Q", self.token))

    def trim(self, ack):
        self.kept = [(i, p) for i, p in self.kept if i > ack]


class Stream:
    """One stream of a link: attach's standard I/O."""

    def __init__(self, link, argv, env=None):
        self.link = link
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.ready = False
        self.bye = False
        self.closed = False
        self.records = []    # parsed records, in order
        self.lines = []      # their raw lines
        self.unacked = 0
        self.wlock = threading.Lock()
        self.send(link.replay())
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        self.keeper = threading.Thread(target=self.keepalive, daemon=True)
        self.keeper.start()

    def send(self, data):
        with self.wlock:
            try:
                self.p.stdin.write(data)
                self.p.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                pass

    def keepalive(self):
        while True:
            time.sleep(2)
            with self.lock:
                if self.closed:
                    return
                data = self.link.frame(ACK, 0)
                self.unacked = 0
            self.send(data)

    def read(self):
        f = self.p.stdout
        while True:
            head = f.read(13)
            if len(head) < 13:
                break
            typ, fid, ack, n = HEAD.unpack(head)
            payload = f.read(n)
            if len(payload) < n:
                break
            with self.lock:
                self.take(typ, fid, ack, payload)
                self.cond.notify_all()
        with self.lock:
            self.closed = True
            self.cond.notify_all()

    def take(self, typ, fid, ack, payload):
        link = self.link
        if typ == REPLAY:
            token = struct.unpack(">Q", payload)[0]
            if link.peer is None or token != link.peer:
                link.events.append("fresh" if link.peer is not None else "ready")
                link.recvd = fid - 1
            else:
                link.trim(ack)
                if fid > link.recvd + 1:
                    link.events.append("lost")
                    link.recvd = fid - 1
                else:
                    link.events.append("ready")
            link.peer = token
            self.ready = True
            out = b"".join(link.frame(REGULAR, i, p) for i, p in link.kept)
            threading.Thread(target=self.send, args=(out,), daemon=True).start()
            return
        if not self.ready:
            raise Failure("a frame before the replay request")
        link.trim(ack)
        if typ == REGULAR:
            if fid <= link.recvd:
                link.dups += 1
                return
            if fid != link.recvd + 1:
                raise Failure(f"frame {fid} after {link.recvd}")
            link.recvd = fid
            line = payload.decode("utf-8", "replace")
            self.lines.append(line)
            try:
                self.records.append(json.loads(line))
            except ValueError:
                self.records.append({"ev": "?", "raw": line})
            self.unacked += 1
            if self.unacked >= 20:
                self.unacked = 0
                threading.Thread(target=self.send, args=(link.frame(ACK, 0),),
                                 daemon=True).start()
        elif typ == DISCONNECT:
            self.bye = True

    def command(self, obj):
        data = (json.dumps(obj) + "\n").encode()
        with self.lock:
            fid = self.link.nextid
            self.link.nextid += 1
            self.link.kept.append((fid, data))
            frame = self.link.frame(REGULAR, fid, data) if self.ready else None
        if frame:
            self.send(frame)

    def wait(self, pred, timeout=20, what="a record"):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for r in self.records:
                    if pred(r):
                        return r
                left = deadline - time.monotonic()
                if left <= 0 or self.closed:
                    names = [r.get("ev") for r in self.records][-12:]
                    raise Failure(f"no {what} ({'closed' if self.closed else 'timeout'}); "
                                  f"last: {names}; stderr: {self.stderr()[:400]!r}")
                self.cond.wait(min(left, 0.5))

    def stderr(self):
        if self.p.poll() is None:
            return ""
        try:
            return self.p.stderr.read().decode("utf-8", "replace")
        except (OSError, ValueError):
            return ""

    def kill(self):
        with self.lock:
            self.closed = True
        try:
            self.p.kill()
        except OSError:
            pass
        self.p.wait()

    def disconnect(self):
        """Leave for good, then end the stream."""
        self.send(self.link.frame(DISCONNECT, 0))
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.kill()
            raise Failure("attach did not exit after a disconnect")
        with self.lock:
            self.closed = True

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            return self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.kill()
            return None


class Client:
    """A link and its streams, through "prospero attach"."""

    def __init__(self, t):
        self.t = t
        self.link = Link()
        self.stream = None
        self.records = []
        self.lines = []
        self.nextcmd = 0

    def attach(self, reconnect=False, extra=(), env=None):
        if self.stream:
            self.records += self.stream.records
            self.lines += self.stream.lines
        argv = [self.t.prospero, "attach", "--state", self.t.state, "-c", self.t.config]
        if reconnect:
            argv.append("--reconnect")
        self.stream = Stream(self.link, argv + list(extra), env=env or self.t.env)
        return self.stream

    def all(self):
        return self.records + (self.stream.records if self.stream else [])

    def alllines(self):
        return self.lines + (self.stream.lines if self.stream else [])

    def call(self, cmd, timeout=20, **fields):
        self.nextcmd += 1
        i = self.nextcmd
        obj = {"cmd": cmd, "id": i}
        obj.update(fields)
        self.stream.command(obj)
        return self.stream.wait(lambda r: r.get("ev") == "reply" and r.get("id") == i,
                                timeout, f"reply to {cmd}")


# Scenarios

class T:
    def __init__(self, args, name):
        self.prospero = os.path.abspath(args.prospero)
        self.args = args
        self.name = name
        self.nonce = f"{random.randrange(1 << 32):08x}"
        # A short name, so that a host's socket fits a socket address
        # (about 100 bytes) without a link in /tmp (os_endpoint)
        self.root = tempfile.mkdtemp(prefix="prem-")
        self.state = os.path.join(self.root, "state")
        self.work = os.path.join(self.root, "work")
        self.config = os.path.join(self.root, "prospero.ini")
        os.makedirs(self.work)
        # The system's temporary directory, the scenario's own, for the
        # sessions' scratch directories
        tmp = os.path.join(self.root, "tmp")
        os.makedirs(tmp)
        self.env = dict(os.environ)
        self.env.update({"TMP": tmp, "TEMP": tmp} if os.name == "nt" else {"TMPDIR": tmp})
        self.procs = []
        self.write_config(args.url)

    def write_config(self, url, extra=""):
        with open(self.config, "w", newline="") as f:
            f.write(f"endpoint = fake\nmodel = fake:remote_slow:{self.nonce}\n"
                    f"[endpoint fake]\nurl = {url}/\nfirst_byte_timeout = 5\n"
                    f"idle_timeout = 5\nprogress_timeout = 10\nconnect_timeout = 2\n"
                    + extra)

    def model(self, scenario):
        """The scenario's model, listed with the one for its subagents
        (NAME_b), as a server lists the models it has: a subagent's model
        must be among them."""
        name = f"fake:{scenario}:{self.nonce}"
        with fake_provider.STATE.lock:
            for m in (name, f"fake:{scenario}_b:{self.nonce}"):
                fake_provider.STATE.models.setdefault(m, {})
        return name

    def check(self, cond, msg):
        if not cond:
            raise Failure(msg)

    def hostpids(self):
        out = subprocess.run([self.prospero, "hosts", "--state", self.state],
                             capture_output=True, text=True).stdout
        return [json.loads(x)["pid"] for x in out.splitlines() if x.strip()]

    def ownhost(self, pid):
        """Whether pid is a host of this scenario's own state directory,
        a fresh temporary one: its command line names it."""
        cmd, state = P.command(pid), os.path.realpath(self.state)
        if os.name == "nt":  # either slash, any case
            cmd, state = (x.replace("\\", "/").lower() for x in (cmd, state))
        return P.mine(pid) and " serve " in cmd and state in cmd

    def cleanup(self):
        if os.environ.get("RELAYDEBUG"):
            for root, _, files in os.walk(self.state):
                for f in files:
                    if f.endswith(".log"):
                        print(open(os.path.join(root, f), errors="replace").read()[-3000:])
        for pid in self.hostpids():
            try:
                signal_ours(pid, P.KILL, self.ownhost, "host")
            except (Failure, OSError) as e:
                print(f"     cleanup: {e}", flush=True)
        for p in self.procs:
            try:
                p.kill()
            except OSError:
                pass
        shutil.rmtree(self.root, ignore_errors=True)
        # Windows removes no file a process has open, nor a program that
        # runs, and a host killed or quit may take a moment to let go
        deadline = time.monotonic() + 15
        while os.path.exists(self.root) and time.monotonic() < deadline:
            time.sleep(0.2)
            shutil.rmtree(self.root, ignore_errors=True)

    def log(self, sid):
        path = os.path.join(self.state, "sessions", sid, "events.jsonl")
        with open(path, encoding="utf-8") as f:
            return f.read().splitlines(keepends=True)


def durable(lines, sid):
    """The durable records of a session among lines, in order."""
    out = []
    for ln in lines:
        try:
            r = json.loads(ln)
        except ValueError:
            continue
        if r.get("sid") == sid and "seq" in r:
            out.append(ln)
    return out


# The last line of a subagent's call: how long it worked.
WORKED = re.compile(r"\n\n\[The subagent worked for [0-9][0-9. a-z]*\.\]\Z")


def report(text):
    """A subagent's call's text without its last line, how long the
    subagent worked, or None if that line is missing."""
    m = WORKED.search(text)
    return text[:m.start()] if m else None


def checklog(t, lines, sid, what):
    """Every durable record of the session once, in order, as logged."""
    got = durable(lines, sid)
    want = t.log(sid)
    seqs = [json.loads(x)["seq"] for x in got]
    t.check(seqs == list(range(1, len(seqs) + 1)),
            f"{what}: seqs not 1..N once each: {seqs}")
    t.check(got == want, f"{what}: {len(got)} records differ from the log's {len(want)}")


def deltatext(records, sid):
    """The text deltas of a session's last attempt, joined."""
    out = []
    for r in records:
        if r.get("sid") != sid:
            continue
        if r.get("ev") == "attempt":
            out = []
        elif r.get("ev") == "delta" and r.get("kind") == "text":
            out.append(r.get("text", ""))
    return "".join(out)


def finaltext(records, sid):
    for r in reversed(records):
        if r.get("sid") == sid and r.get("ev") == "assistant":
            return "".join(b.get("text", "") for b in r.get("blocks", [])
                           if b.get("t") == "text")
    return None


SCENARIOS = {}


def scenario(fn):
    SCENARIOS[fn.__name__] = fn
    return fn


def grandchild(pid, root):
    """Whether root is the parent of pid's parent: an ssh that prospero
    connect (root, a process of ours) started under its reaper. Windows
    has no reaper there: its parent, then."""
    try:
        if os.name == "nt":
            return P.descends(pid, root, 1)
        return P.parent(pid) != root and P.descends(pid, root, 2)
    except P.Refused:
        return False


def signal_ours(pid, sig, owned, what):
    """Signal a pid that came back from output or a file rather than from
    a spawn of our own: only a plain integer above 1 that owned(pid)
    confirms, right before the signal, to be a process of ours."""
    try:
        P.signal_ours(pid, sig, owned, what)
    except P.Refused as e:
        raise Failure(str(e))


def gone(pid, within=10.0):
    try:
        P.checked(pid, "pid to watch")
    except P.Refused as e:
        raise Failure(str(e))
    deadline = time.monotonic() + within
    while time.monotonic() < deadline:
        if not P.alive(pid):  # a zombie counts as gone
            return True
        time.sleep(0.1)
    return False


@scenario
def no_host(t):
    """attach --reconnect with no host: status 90 at once, nothing started."""
    t0 = time.monotonic()
    p = subprocess.run([t.prospero, "attach", "--reconnect", "--state", t.state],
                       capture_output=True, text=True, timeout=10)
    t.check(p.returncode == 90, f"exit {p.returncode}: {p.stderr}")
    t.check("no host" in p.stderr, f"stderr: {p.stderr!r}")
    t.check(time.monotonic() - t0 < 2, "at once")
    t.check(not t.hostpids(), "no host started")


def tmplinks(target):
    """This user's links in /tmp that the harness names, leading to target."""
    out = []
    for name in os.listdir("/tmp"):
        path = os.path.join("/tmp", name)
        if name.startswith(f"prospero-{os.geteuid()}-") and os.path.islink(path) and \
                os.readlink(path) == target:
            out.append(path)
    return out


@scenario
def stale_links(t):
    """A state directory too deep for a socket address is reached through
    a link in /tmp (os_endpoint), which a client makes as well as a host,
    and which outlives the directory when a host was killed, or never
    ran. The next host to start removes this user's links that lead
    nowhere, and only those."""
    if os.name == "nt":
        return "skip"  # pipe names, no links
    short = t.state
    deep = os.path.join(t.root, "deep-" + "x" * 100)
    t.state = os.path.join(deep, "state")
    run = os.path.join(os.path.realpath(t.root), os.path.basename(deep), "state", "run")

    # A client alone makes one, then a host killed leaves it
    p = subprocess.run([t.prospero, "attach", "--reconnect", "--state", t.state],
                       capture_output=True, text=True, timeout=10)
    t.check(p.returncode == 90, f"attach --reconnect: exit {p.returncode}: {p.stderr}")
    links = tmplinks(run)
    t.check(len(links) == 1, f"a client's link to {run}: {links}")
    c = Client(t)
    s = c.attach()
    c.call("hello")
    pids = t.hostpids()
    t.check(len(pids) == 1, f"one host: {pids}")
    t.check(tmplinks(run) == links, f"the host's link is the client's: {tmplinks(run)}")
    signal_ours(pids[0], P.KILL, t.ownhost, "host")
    t.check(gone(pids[0]), "the host was killed")
    s.kill()
    link = links[0]
    t.check(os.path.exists(link), "the link leads to the run directory")
    shutil.rmtree(deep)  # now it leads nowhere

    # Others: one that leads nowhere, one that leads somewhere, and one
    # leading nowhere by a name not the harness's
    def name(tail=""):
        return f"/tmp/prospero-{os.geteuid()}-{random.randrange(1 << 64):016x}{tail}"
    dead, live, other = name(), name(), name("x")
    planted = [link, dead, live, other]  # all removed after, whatever happens
    try:
        os.symlink(os.path.join(t.root, "gone", "run"), dead)
        os.symlink(t.root, live)
        os.symlink(os.path.join(t.root, "gone", "run"), other)
        t.state = short
        c = Client(t)
        s = c.attach()
        c.call("hello")
        t.check(not os.path.lexists(link), f"the host's dead link stays: {link}")
        t.check(not os.path.lexists(dead), "the other dead link stays")
        t.check(os.path.islink(live), "a live link was removed")
        t.check(os.path.islink(other), "a link by another name was removed")
        c.call("quit")
        s.close()
    finally:
        for path in planted:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass


@scenario
def attach_reconnect(t):
    """A host started by attach; the attach killed mid-stream; attach
    --reconnect: nothing lost, nothing twice, deltas included."""
    c = Client(t)
    s = c.attach()
    hello = c.call("hello", proto=0)
    t.check(hello.get("ok") == 1, f"hello: {hello}")
    h = s.wait(lambda r: r.get("ev") == "hello", what="hello")
    t.check(h.get("proto") == 0 and "host" in h.get("caps", []), f"hello: {h}")
    t.check({"list", "open", "file", "draft"} <= set(h.get("cmds", [])) and
            "subagents" in h.get("caps", []), f"hello names its commands: {h}")
    pids = t.hostpids()
    t.check(len(pids) == 1, f"one host registered: {pids}")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "delta" and "w5 " in r.get("text", ""),
           what="some deltas")
    s.kill()                     # mid-stream
    time.sleep(0.5)              # the turn goes on without a client
    s = c.attach(reconnect=True)
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
           timeout=30, what="turn.end")
    t.check(c.link.events[:1] == ["ready"] and c.link.events[1:] == ["ready"],
            f"link events {c.link.events}")
    t.check(c.link.dups == 0, f"{c.link.dups} frames came twice")
    checklog(t, c.alllines(), sid, "records")
    want = finaltext(c.all(), sid)
    t.check(want and deltatext(c.all(), sid) == want,
            f"deltas {deltatext(c.all(), sid)!r} != text {want!r}")

    # Quit: the host stops, attach exits, the registration goes
    q = c.call("quit")
    t.check(q.get("ok") == 1, f"quit: {q}")
    rc = s.close()
    t.check(rc == 0, f"attach exit {rc}")
    t.check(gone(pids[0]), "the host exited")
    t.check(not os.listdir(os.path.join(t.state, "hosts")), "registration removed")


@scenario
def unattended_turn(t):
    """A turn that ends while no client is there: a new client opens the
    session from 0 and gets it all from the log, through turn.end."""
    c = Client(t)
    s = c.attach()
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="go", model=t.model("remote_tool"))
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "tool.end", what="the tool")
    s.disconnect()
    time.sleep(6)
    d = Client(t)
    s2 = d.attach(reconnect=True)
    d.call("hello")
    o = d.call("open", sid=sid, **{"from": 0})
    t.check(o.get("ok") == 1, f"open: {o}")
    end = s2.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                  what="turn.end")
    t.check(end.get("why") == "done", f"turn.end {end}")
    time.sleep(0.5)
    checklog(t, d.alllines(), sid, "replayed")
    # From a later seq: only what follows
    e = Client(t)
    s3 = e.attach(reconnect=True)
    e.call("open", sid=sid, **{"from": 5})
    s3.wait(lambda r: r.get("ev") == "turn.end", what="turn.end")
    time.sleep(0.3)
    seqs = [x["seq"] for x in s3.records if x.get("sid") == sid and "seq" in x]
    t.check(seqs and seqs[0] == 6 and seqs == list(range(6, 6 + len(seqs))),
            f"from 5: {seqs}")
    for x in (s2, s3):
        x.disconnect()


@scenario
def two_clients(t):
    """Two clients at once, each getting what it asked for under its own
    ids; one closing leaves the session to the other."""
    a = Client(t)
    sa = a.attach()
    a.call("hello")
    r = a.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    sa.wait(lambda r: r.get("ev") == "delta", what="a delta")
    b = Client(t)
    sb = b.attach(reconnect=True)
    b.call("hello")                               # id 1, as A's hello was
    lst = b.call("list")
    t.check(lst.get("ok") == 1, f"list: {lst}")
    t.check(any(r.get("ev") == "sessions" for r in sb.records), "B got the list")
    t.check(not any(r.get("ev") == "sessions" for r in sa.records),
            "A did not get B's list")
    o = b.call("open", sid=sid, **{"from": 0})
    t.check(o.get("ok") == 1, f"open: {o}")
    for s in (sa, sb):
        s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
               timeout=30, what="turn.end")
    time.sleep(0.3)
    checklog(t, a.alllines(), sid, "A")
    checklog(t, b.alllines(), sid, "B")
    # B sees what A asks for, but not A's replies
    n = len(sb.records)
    p = a.call("prompt", sid=sid, text="again")
    t.check(p.get("ok") == 1, f"prompt: {p}")
    sb.wait(lambda r: r.get("ev") == "user" and r.get("text") == "again", what="A's prompt")
    t.check(not any(r.get("ev") == "reply" and r.get("id") == p["id"] and r is not p
                    for r in sb.records[n:]), "B got A's reply")
    cl = b.call("close", sid=sid)
    t.check(cl.get("ok") == 1, f"close: {cl}")
    sa.wait(lambda r: r.get("ev") == "turn.end" and r.get("turn") == 2, timeout=30,
            what="A's second turn")
    # B's close neither stopped the session nor brought B more of it
    later = [r for r in sb.records if r.get("sid") == sid and r.get("turn") == 2
             and r.get("ev") == "turn.end"]
    t.check(not later, "B, closed, still got the session's records")
    for s in (sa, sb):
        s.disconnect()


@scenario
def queue_clients(t):
    """A steer queued through one client reaches another that opens the
    session after it (the open's queue), and that one withdraws it: both
    see the queue empty, and the steer never becomes a message."""
    a = Client(t)
    sa = a.attach()
    a.call("hello")
    sid = a.call("new", cwd=t.work, prompt="go", attended=1)["sid"]
    sa.wait(lambda r: r.get("ev") == "delta", what="a delta")
    t.check(a.call("steer", sid=sid, text="take me back").get("ok") == 1, "steered")
    q = sa.wait(lambda r: r.get("ev") == "queue" and r.get("items"), what="A's queue")
    item = q["items"][0]
    t.check(item["kind"] == "steer" and item["text"] == "take me back", f"queue: {q}")
    b = Client(t)
    sb = b.attach(reconnect=True)
    b.call("hello")
    t.check(b.call("open", sid=sid, **{"from": 0}).get("ok") == 1, "B opened")
    qb = sb.wait(lambda r: r.get("ev") == "queue" and r.get("sid") == sid,
                 what="B's queue")
    t.check(qb["items"] == q["items"], f"B's queue: {qb}")
    r = b.call("unqueue", sid=sid, item=item["id"])
    t.check(r.get("ok") == 1 and r.get("text") == "take me back", f"unqueue: {r}")
    for s in (sa, sb):
        s.wait(lambda r: r.get("ev") == "queue" and r.get("items") == [],
               what="the queue emptied")
    sa.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
            timeout=30, what="turn.end")
    r = a.call("unqueue", sid=sid, item=item["id"])
    t.check(r.get("ok") == 0, f"withdrawn already: {r}")
    users = [r for r in sa.records if r.get("ev") == "user" and r.get("sid") == sid]
    t.check([u.get("text") for u in users] == ["go"], f"messages: {users}")
    for s in (sa, sb):
        s.disconnect()


@scenario
def backlog(t):
    """A host that keeps little for each client (a test hook), its client
    away while an answer larger than that comes: the answer comes from
    the session's log, every durable record once, in order."""
    env = dict(t.env, PROSPERO_TEST_KEEPMAX=str(640 << 10))
    c = Client(t)
    s = c.attach(env=env)
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="go", model=t.model("remote_big"), attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "delta" and "<20>" in r.get("text", ""),
           what="some deltas")
    s.kill()                     # away while the rest comes
    deadline = time.monotonic() + 30
    while not any('"ev":"turn.end"' in x for x in t.log(sid)):
        t.check(time.monotonic() < deadline, "the turn ends")
        time.sleep(0.2)
    s = c.attach(reconnect=True, env=env)
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
           timeout=30, what="turn.end")
    time.sleep(0.5)
    t.check(c.link.dups == 0, f"{c.link.dups} frames came twice")
    checklog(t, c.alllines(), sid, "records")
    final = finaltext(c.all(), sid) or ""
    t.check("<299>" in final and len(final) > 300 * 1900, f"the answer: {len(final)} bytes")
    q = c.call("quit")
    t.check(q.get("ok") == 1, f"quit: {q}")
    s.close()
    with open(os.path.join(t.state, "run", "host.log"), encoding="utf-8") as f:
        log = f.read()
    t.check("fell behind" in log, f"it never fell behind: {log!r}")


@scenario
def idle_exit(t):
    """A host with no client and nothing running exits after its idle time."""
    c = Client(t)
    s = c.attach(extra=("--idle", "2"))
    c.call("hello")
    pids = t.hostpids()
    t.check(len(pids) == 1, f"one host: {pids}")
    t.check(not gone(pids[0], 1), "the host stays while a client is attached")
    s.disconnect()
    t0 = time.monotonic()
    t.check(gone(pids[0], 15), "the host exited")
    dt = time.monotonic() - t0
    t.check(dt > 1.5, f"exited after {dt:.1f} s, before its idle time")
    p = subprocess.run([t.prospero, "attach", "--reconnect", "--state", t.state],
                       capture_output=True, text=True, timeout=10)
    t.check(p.returncode == 90, f"then no host: exit {p.returncode}")
    t.check(not os.listdir(os.path.join(t.state, "hosts")), "registration removed")


def pesubsystem(path):
    """A PE program's subsystem: 2 for a GUI program, 3 for a console's."""
    with open(path, "rb") as f:
        data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    return struct.unpack_from("<H", data, pe + 24 + 68)[0]


@scenario
def quit_signals(t):
    """prospero serve asked to quit by a signal (SIGTERM, SIGINT, SIGHUP;
    on Windows Ctrl+Break at its console) quits as a client's quit does:
    exit 0, its registration and endpoint gone."""
    win = os.name == "nt"
    if win and pesubsystem(t.prospero) == 2:
        return "skip"  # a GUI program, with no console for Ctrl+Break to reach
    sigs = [signal.CTRL_BREAK_EVENT] if win else [signal.SIGTERM, signal.SIGINT, signal.SIGHUP]
    for sig in sigs:
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if win else 0
        p = subprocess.Popen([t.prospero, "serve", "--state", t.state, "-c", t.config],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, creationflags=flags)
        t.procs.append(p)
        deadline = time.monotonic() + 10
        while p.pid not in t.hostpids() and time.monotonic() < deadline:
            time.sleep(0.1)
        t.check(p.pid in t.hostpids(), f"{sig!r}: the host registered")
        p.send_signal(sig)  # a process of our own, by its Popen
        try:
            rc = p.wait(10)
        except subprocess.TimeoutExpired:
            raise Failure(f"{sig!r}: the host did not quit")
        err = p.stderr.read().decode("utf-8", "replace")
        t.check(rc == 0, f"{sig!r}: exit {rc}: {err}")
        t.check("asked to stop" in err, f"{sig!r}: it said why: {err!r}")
        hosts = os.path.join(t.state, "hosts")
        t.check(not os.path.isdir(hosts) or not os.listdir(hosts),
                f"{sig!r}: registration removed: {os.listdir(hosts)}")
        r = subprocess.run([t.prospero, "attach", "--reconnect", "--state", t.state],
                           capture_output=True, text=True, timeout=10)
        t.check(r.returncode == 90, f"{sig!r}: then no host: exit {r.returncode}")


@scenario
def idle_busy(t):
    """No client, but a turn running: the host waits for it to end."""
    c = Client(t)
    s = c.attach(extra=("--idle", "1"))
    c.call("hello")
    pid = t.hostpids()[0]
    r = c.call("new", cwd=t.work, prompt="go")
    sid = r["sid"]
    s.disconnect()
    t.check(not gone(pid, 2.5), "the host exited while a turn ran")
    t.check(gone(pid, 15), "the host exited once the turn ended")
    lines = t.log(sid)
    t.check(json.loads(lines[-1]).get("ev") == "turn.end" and
            json.loads(lines[-1]).get("why") == "done", f"the turn finished: {lines[-1]}")


@scenario
def garbage(t):
    """Garbage on a stream drops that stream, not the host."""
    c = Client(t)
    s = c.attach()
    c.call("hello")
    pid = t.hostpids()[0]
    p = subprocess.Popen([t.prospero, "attach", "--reconnect", "--state", t.state],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    out, _ = p.communicate(b"\x02" + os.urandom(64), timeout=10)
    t.check(p.returncode == 0, f"attach exit {p.returncode}")
    q = subprocess.Popen([t.prospero, "attach", "--reconnect", "--state", t.state],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    big = struct.pack(">BIII", REGULAR, 1, 0, 0xffffffff)
    q.communicate(Link().replay() + big, timeout=10)
    t.check(not gone(pid, 0.5), "the host survives")
    r = c.call("hello")
    t.check(r.get("ok") == 1, "the first client still works")
    s.disconnect()


@scenario
def close_subagents(t):
    """Closing a session whose subagents the host gave this client too:
    the close is answered, and the host and the client go on."""
    for name, text in (("a.txt", "1\n2\n3\n"), ("b.txt", "1\n2\n")):
        with open(os.path.join(t.work, name), "w", newline="") as f:
            f.write(text)
    c = Client(t)
    s = c.attach()
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="go", model=t.model("task_parallel"), attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, timeout=60,
           what="the parent's turn.end")
    kids = {r["sid"] for r in c.all() if r.get("ev") == "session" and r.get("parent") == sid}
    t.check(len(kids) == 2, f"the subagents' records came: {kids}")
    pid = t.hostpids()[0]
    x = c.call("close", sid=sid)
    t.check(x.get("ok") == 1, f"close: {x}")
    t.check(not gone(pid, 0.5), "the host lives on")
    o = c.call("open", sid=sid, **{"from": 0})
    t.check(o.get("ok") == 1, f"open again: {o}")
    s.disconnect()


@scenario
def host_ops(t):
    """delete and trash through a host: refused while another client has
    the session, or a subagent of it, open, and done for the one that has
    it, the Trash closing it there; fork through a host, its new session
    then opened like any other."""
    c = Client(t)
    s = c.attach()
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="task", model=t.model("delete"), attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, timeout=30,
           what="the parent's turn.end")
    kids = [r["sid"] for r in c.all() if r.get("ev") == "session" and r.get("parent") == sid]
    t.check(len(kids) == 1, f"a subagent: {kids}")

    b = Client(t)
    sb = b.attach(reconnect=True)
    b.call("hello")
    o = b.call("open", sid=kids[0])
    t.check(o.get("ok") == 1, f"B opens the subagent: {o}")
    d = c.call("delete", sid=sid)
    t.check(d.get("ok") == 0 and d.get("err") == "open in another client", f"refused: {d}")
    d = c.call("trash", sid=sid)
    t.check(d.get("ok") == 0 and d.get("err") == "open in another client", f"no Trash: {d}")
    t.check(os.path.isdir(os.path.join(t.state, "sessions", sid)), "still there")

    f = c.call("fork", sid=sid, turn=1)
    t.check(f.get("ok") == 1 and f.get("from") == sid and f.get("sid") != sid, f"fork: {f}")
    fork = f["sid"]
    o = c.call("open", sid=fork, attended=1)
    t.check(o.get("ok") == 1, f"open the fork: {o}")
    first = s.wait(lambda r: r.get("sid") == fork and r.get("seq") == 1, what="its record")
    t.check(first.get("ev") == "session" and first.get("forked_from") == sid, f"{first}")
    p = c.call("prompt", sid=fork, text="more")
    t.check(p.get("ok") == 1, f"prompt the fork: {p}")
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == fork and r.get("turn") == 2,
           timeout=30, what="the fork's turn")
    d = b.call("delete", sid=fork)
    t.check(d.get("ok") == 0 and d.get("err") == "open in another client", f"B: {d}")

    x = b.call("close", sid=kids[0])
    t.check(x.get("ok") == 1, f"B closes: {x}")
    d = c.call("trash", sid=sid)
    t.check(d.get("ok") == 1 and d.get("trashed") == [kids[0], sid], f"trashed: {d}")
    p = c.call("prompt", sid=sid, text="still?")
    t.check(p.get("ok") == 0, f"closed by the Trash: {p}")
    d = c.call("delete", sid=sid)
    t.check(d.get("ok") == 1 and d.get("deleted") == [kids[0], sid], f"deleted: {d}")
    d = c.call("delete", sid=fork)
    t.check(d.get("ok") == 1 and d.get("deleted") == [fork], f"the fork, open here: {d}")
    for gonesid in (sid, kids[0], fork):
        t.check(not os.path.exists(os.path.join(t.state, "sessions", gonesid)), "gone")
    p = c.call("prompt", sid=fork, text="still?")
    t.check(p.get("ok") == 0, f"no more: {p}")
    s.disconnect()
    sb.disconnect()


class Raw:
    """attach's standard I/O, frames written by hand: regular frames that
    acknowledge nothing (ack 0), so that the host keeps all it sends."""

    def __init__(self, t, env):
        self.p = subprocess.Popen([t.prospero, "attach", "--state", t.state, "-c", t.config],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        self.frames = []
        self.cond = threading.Condition()
        self.closed = False
        self.nextid = 1
        threading.Thread(target=self.read, daemon=True).start()
        self.write(HEAD.pack(REPLAY, 1, 0, 8) + struct.pack(">Q", random.getrandbits(64) | 1))

    def read(self):
        f = self.p.stdout
        while True:
            h = f.read(13)
            if len(h) < 13:
                break
            typ, fid, ack, n = HEAD.unpack(h)
            data = f.read(n)
            with self.cond:
                self.frames.append((typ, data))
                self.cond.notify_all()
        with self.cond:
            self.closed = True
            self.cond.notify_all()

    def write(self, data):
        try:
            self.p.stdin.write(data)
            self.p.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            pass

    def regular(self, obj):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        self.nextid += 1
        return HEAD.pack(REGULAR, self.nextid - 1, 0, len(data)) + data

    def wait(self, n, timeout=10):
        """The regular frames once there are n, or all there are."""
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                regs = [d for typ, d in self.frames if typ == REGULAR]
                left = deadline - time.monotonic()
                if len(regs) >= n or self.closed or left <= 0:
                    return regs
                self.cond.wait(left)

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


@scenario
def behind_drop(t):
    """A client with no room left on its link for even a reply, whose
    command (a hello in two pieces, gathered in the stream's buffer)
    meets that: dropped, as too far behind, while the host goes on."""
    keep = 4096
    env = dict(t.env, PROSPERO_TEST_KEEPMAX=str(keep))
    r = Raw(t, env)
    try:
        r.write(r.regular({"cmd": "hello", "id": 1, "proto": 0}))
        regs = r.wait(2)
        t.check(len(regs) == 2, f"hello: {regs}")
        hello = 13 + len([d for d in regs if b'"ev":"hello"' in d][0])
        pid = t.hostpids()[0]
        # Fill its queue with replies to closes of no session, until less
        # room is left than the hello record needs
        r.write(r.regular({"cmd": "close", "id": 2, "sid": "a"}))
        regs = r.wait(3)
        base = 13 + len(regs[-1]) - 1
        room = keep - sum(13 + len(d) for d in regs)
        cid = 2
        while room >= hello:
            k = max(1, min(room - (hello - 1) - base, 60))
            if base + k > room:
                break
            cid += 1
            r.write(r.regular({"cmd": "close", "id": cid, "sid": "a" * k}))
            regs = r.wait(len(regs) + 1)
            room = keep - sum(13 + len(d) for d in regs)
        t.check(room < hello, f"room left {room}, the hello needs {hello}")
        frame = r.regular({"cmd": "hello", "id": cid + 1, "proto": 0})
        r.write(frame[:20])
        time.sleep(0.5)
        r.write(frame[20:])
        deadline = time.monotonic() + 10
        log = ""
        while "fell too far behind" not in log and time.monotonic() < deadline:
            time.sleep(0.2)
            with open(os.path.join(t.state, "run", "host.log"), encoding="utf-8") as f:
                log = f.read()
        t.check("fell too far behind" in log, f"dropped: {log[-300:]!r}")
        t.check(not gone(pid, 0.5), "the host lives on")
        c = Client(t)
        s = c.attach(reconnect=True, env=env)
        x = c.call("hello")
        t.check(x.get("ok") == 1, f"another client's hello: {x}")
        s.disconnect()
    finally:
        r.close()


def connect(t, args, env=None):
    p = subprocess.Popen([t.prospero, "connect"] + args, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=env or t.env, text=True, bufsize=1)
    t.procs.append(p)
    return Lines(p)


class Lines:
    """Protocol v0 lines on standard I/O: prospero connect."""

    def __init__(self, p):
        self.p = p
        self.records = []
        self.cond = threading.Condition()
        self.id = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.p.stdout:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            with self.cond:
                self.records.append(r)
                self.cond.notify_all()
        with self.cond:
            self.cond.notify_all()

    def send(self, cmd, **fields):
        self.id += 1
        obj = {"cmd": cmd, "id": self.id}
        obj.update(fields)
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()
        return self.id

    def wait(self, pred, timeout=30, what="a record"):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for r in self.records:
                    if pred(r):
                        return r
                left = deadline - time.monotonic()
                if left <= 0 or self.p.poll() is not None and not self.p.stdout.readable():
                    names = [r.get("ev") for r in self.records][-15:]
                    raise Failure(f"no {what}; last {names}")
                self.cond.wait(min(left, 0.5))

    def call(self, cmd, timeout=30, **fields):
        i = self.send(cmd, **fields)
        return self.wait(lambda r: r.get("ev") == "reply" and r.get("id") == i,
                         timeout, f"reply to {cmd}")

    def close(self, timeout=15):
        self.p.stdin.close()
        try:
            rc = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            raise Failure("connect did not exit")
        return rc


def checkup(t, c, up):
    """A link's "up" record passes on what the host's hello said of its
    engine, as the library keeps the hello to itself."""
    caps = up.get("caps", [])
    cmds = up.get("cmds", [])
    t.check(up.get("version") and "host" in caps and "file.base64" in caps,
            f"up: {up}")
    t.check({"hello", "list", "new", "open", "rename", "fork", "draft"} <= set(cmds),
            f"up names the commands: {up}")
    t.check(not [r for r in c.records if r.get("ev") == "hello"], "no hello passed on")


@scenario
def connect_local(t):
    """The client library against this machine's host: a turn, then the
    host restarted under it: the session comes back from its log."""
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    up = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    checkup(t, c, up)
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "turn.end", what="turn.end")
    lst = c.call("list")
    ss = [r for r in c.records if r.get("ev") == "sessions"]
    t.check(ss and ss[-1].get("host") == "local", f"host in the list: {ss[-1:]}")

    # The host goes away hard: a new one starts, and the session reopens
    pid = t.hostpids()[0]
    signal_ours(pid, P.KILL, t.ownhost, "host")
    t.check(gone(pid), "killed")
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "down", what="down")
    n = len([r for r in c.records if r.get("ev") == "remote" and r.get("state") == "up"])
    held = c.send("list")        # while down: it waits for the new host
    c.wait(lambda r: len([x for x in c.records if x.get("ev") == "remote"
                          and x.get("state") == "up"]) > n, what="up again", timeout=40)
    ups = [x for x in c.records if x.get("ev") == "remote" and x.get("state") == "up"]
    checkup(t, c, ups[-1])
    h = c.wait(lambda r: r.get("ev") == "reply" and r.get("id") == held, what="the held list")
    t.check(h.get("ok") == 1, f"a command held while down: {h}")
    p = c.call("prompt", sid=sid, text="more")
    t.check(p.get("ok") == 1, f"prompt after the restart: {p}")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("turn") == 2, what="turn 2")
    seqs = [r["seq"] for r in c.records if r.get("sid") == sid and "seq" in r]
    t.check(seqs == sorted(set(seqs)), f"no record twice: {seqs}")
    lines = t.log(sid)
    t.check(seqs[-1] == json.loads(lines[-1])["seq"], "up to the log's end")
    ids = [r["id"] for r in c.records if r.get("ev") == "reply"]
    t.check(sorted(ids) == list(range(1, c.id + 1)), f"one reply per command: {ids}")
    rc = c.close()
    t.check(rc == 0, f"connect exit {rc}")
    newpid = t.hostpids()
    t.check(newpid and newpid[0] != pid, "a new host runs on")


def exportlog(sid, pad):
    """A raw log as another machine exported it: a turn, then pad notes
    of 500 kB, so that an upload of 512 KiB parts takes pad of them."""
    recs = [
        {"ev": "session", "proto": 0, "version": "0.0.0", "cwd": "/nowhere/else",
         "role": "", "model": "elsewhere", "endpoint": "http://192.0.2.1/",
         "roots": ["/"]},
        {"ev": "instructions", "text": "Be brief."},  # left out: a gap
        {"ev": "user", "text": "Hello from elsewhere.", "via": "prompt"},
        {"ev": "turn", "n": 1},
        {"ev": "assistant", "blocks": [{"t": "text", "text": "Hi there."}]},
        {"ev": "turn.end", "turn": 1, "why": "done"},
        {"ev": "title", "text": "Elsewhere", "by": "auto"},
    ] + [{"ev": "note", "level": "info", "text": f"{i}" * 500000} for i in range(pad)]
    out = ""
    for seq, r in enumerate(recs, 1):
        out += json.dumps(dict({"sid": sid, "seq": seq, "ts": seq}, **r)) + "\n"
    return out.encode()


def importthrough(t, c, cwd, timeout=30):
    """Upload a log from elsewhere through a client library's link, in
    parts as large as a part may be, import it there, open it and go on."""
    old = "20260101-000000-else"
    data = exportlog(old, 3)
    token, off, parts = None, 0, 0
    while off < len(data):
        chunk = data[off:off + (1 << 19)]
        args = {"data": base64.b64encode(chunk).decode(), "off": off}
        if token:
            args["token"] = token
        if off + len(chunk) == len(data):
            args["done"] = 1
        r = c.call("upload", timeout=timeout, **args)
        t.check(r.get("ok") == 1 and r.get("off") == off + len(chunk), f"upload {off}: {r}")
        token, off, parts = r["token"], r["off"], parts + 1
    t.check(parts == 3, f"in {parts} parts")
    r = c.call("import", token=token, cwd=cwd, timeout=timeout)
    sid = r.get("sid")
    t.check(r.get("ok") == 1 and r.get("from") == old and sid and sid != old,
            f"import: {r}")
    o = c.call("open", sid=sid, timeout=timeout)
    t.check(o.get("ok") == 1 and o.get("readonly") == 0, f"open: {o}")
    first = c.wait(lambda r: r.get("ev") == "session" and r.get("sid") == sid,
                   what="its session record")
    t.check(first.get("imported_from") == old and "roots" not in first and
            first.get("cwd") and first.get("cwd") != "/nowhere/else", f"session: {first}")
    p = c.call("prompt", sid=sid, text="more", timeout=timeout)
    t.check(p.get("ok") == 1, f"prompt: {p}")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid and
           r.get("turn") == 2, what="its turn 2", timeout=60)
    t.check(finaltext(c.records, sid), "an answer")
    titles = [r.get("text") for r in c.records if r.get("ev") == "title" and
              r.get("sid") == sid]
    t.check("Elsewhere (imported)" in titles, f"titled: {titles}")
    seqs = [r["seq"] for r in c.records if r.get("sid") == sid and "seq" in r]
    t.check(seqs == sorted(set(seqs)), f"no record twice: {seqs}")
    # The whole log from its start, after the open's reply, skipping the
    # numbers of records the import left out, which a view must take as
    # no loss (the GUI's once showed nothing of it)
    t.check(seqs[:1] == [1] and any(b > a + 1 for a, b in zip(seqs, seqs[1:])),
            f"from 1, skipping numbers: {seqs}")
    users = [r.get("text") for r in c.records if r.get("ev") == "user" and
             r.get("sid") == sid]
    t.check(users[:1] == ["Hello from elsewhere."], f"its prompts: {users}")
    return sid


@scenario
def import_host(t):
    """A raw log from elsewhere imported through this machine's host, by
    the client library, in parts near its command limit."""
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    up = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    t.check("import" in up.get("caps", []) and
            {"upload", "import"} <= set(up.get("cmds", [])), f"up: {up}")
    sid = importthrough(t, c, t.work)
    t.check(t.log(sid)[0].startswith('{"sid":"' + sid), "its log")
    rc = c.close()
    t.check(rc == 0, f"connect exit {rc}")


@scenario
def image_host(t):
    """An image attached through this machine's host, by the client
    library: uploaded in parts, kept in the session, and sent with the
    prompt that names it (fake_provider.sc_attach)."""
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    up = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    t.check("images" in up.get("caps", []), f"up: {up}")
    model = t.model("attach")
    sid = c.call("new", cwd=t.work, model=model, attended=1)["sid"]
    def pngchunk(kind, data):
        return (len(data).to_bytes(4, "big") + kind + data +
                zlib.crc32(kind + data).to_bytes(4, "big"))
    raw = b"".join(b"\x00" + bytes(range(y, y + 15)) for y in range(40))  # 5x40 RGB
    png = (b"\x89PNG\r\n\x1a\n" +
           pngchunk(b"IHDR", struct.pack(">IIBBBBB", 5, 40, 8, 2, 0, 0, 0)) +
           pngchunk(b"IDAT", zlib.compress(raw, 0)) + pngchunk(b"IEND", b""))
    token, off = None, 0
    while off < len(png):
        part = png[off:off + 256]
        args = {"kind": "image", "sid": sid, "data": base64.b64encode(part).decode(),
                "off": off}
        if token:
            args["token"] = token
        if off + len(part) == len(png):
            args["done"] = 1
        r = c.call("upload", **args)
        t.check(r.get("ok") == 1 and r.get("off") == off + len(part), f"upload {off}: {r}")
        token, off = r["token"], r["off"]
    t.check(r.get("name") == "img/1.png" and (r.get("w"), r.get("h")) == (5, 40),
            f"attached: {r}")
    p = c.call("prompt", sid=sid, text="look", images=[r["name"]])
    t.check(p.get("ok") == 1, f"prompt: {p}")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    user = next(r for r in c.records if r.get("ev") == "user" and r.get("sid") == sid)
    t.check(user.get("images") == [{"name": "img/1.png", "mime": "image/png", "w": 5,
                                    "h": 40}], f"the user record: {user}")
    sent = fake_provider.user_images(fake_provider.STATE.log(model)[-1]["body"])
    t.check(sent == [("image/png", base64.b64encode(png).decode())], f"sent: {sent}")
    t.check(finaltext(c.records, sid) == "I see 1 images.", "answered")
    rc = c.close()
    t.check(rc == 0, f"connect exit {rc}")


@scenario
def ssh_import(t):
    """connect --ssh to a real host: a raw log from this machine
    imported there, and continued."""
    if not t.args.ssh:
        return "skip"
    a = t.args
    rstate = f"{a.remote_root}/state-{t.nonce}"
    rconf = f"{a.remote_root}/prospero-{t.nonce}.ini"
    remote_write(a, rconf, open(t.config).read().replace(a.url, a.remote_url))
    try:
        c = connect(t, ["--ssh", a.ssh, "--path", a.remote_prospero,
                        "--remote-state", rstate, "--remote-config", rconf,
                        "--state", os.path.join(t.root, "local")])
        c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up",
               timeout=60)
        importthrough(t, c, a.remote_work, timeout=120)
        q = c.call("quit", timeout=30)
        t.check(q.get("ok") == 1, f"quit {q}")
        t.check(c.close(timeout=30) == 0, "connect exits 0")
    finally:
        remote_sh(a, f"rm -rf {rstate} {rconf}")


@scenario
def remote_option(t):
    """A remote's path that ssh would take for an option: ignored from an
    included file, and from the user's own quoted, after --, so that a
    real ssh (to a name that never resolves) runs nothing here."""
    local = os.path.join(t.root, "local")
    inc = os.path.join(t.root, "team.ini")
    with open(inc, "w", newline="") as f:
        f.write("[remote team]\npath = -oProxyCommand=/usr/bin/touch\n")
    for own in ("", "path = -oProxyCommand=/usr/bin/touch\n"):
        with open(t.config, "w", newline="") as f:
            f.write(f"include = {inc}\n[remote team]\nssh = example.invalid\n" + own)
        p = subprocess.run([t.prospero, "connect", "team", "-c", t.config, "--state", local],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           timeout=60)
        t.check(not os.path.exists(os.path.join(local, "attach")),
                f"ssh ran a command here ({own.strip() or 'included'}): {p.stderr!r}")
        if not own:
            t.check("[remote] setting in an included file is ignored" in p.stderr,
                    f"the include's path is an error: {p.stderr!r}")


@scenario
def restart_unattended(t):
    """A session made unattended (new without attended), then its host
    restarted under the client library: opened again on the new host,
    it is still unattended, so an ask there is denied at once."""
    # Asked about even in /tmp, where the directories may be (Linux)
    t.write_config(t.args.url, "[guard]\nask_timeout = 60\ntemp = ask\n")
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, model=t.model("ask"))
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    pid = t.hostpids()[0]
    signal_ours(pid, P.KILL, t.ownhost, "host")
    t.check(gone(pid), "killed")
    c.wait(lambda r: ups(c) >= 2, what="up again", timeout=40)
    p = c.call("prompt", sid=sid, text="write outside")
    t.check(p.get("ok") == 1, f"prompt: {p}")
    a = c.wait(lambda r: r.get("ev") == "answer" and r.get("sid") == sid, what="the answer",
               timeout=20)
    t.check(a.get("decision") == "deny" and a.get("by") == "policy", f"answer: {a}")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    x = c.call("quit")
    t.check(x.get("ok") == 1, f"quit: {x}")
    t.check(c.close() == 0, "connect exits 0")


@scenario
def reopen_unattended(t):
    """Opened again unattended by a client that attended it, a session is
    no longer attended by that client: while another attends it, an ask
    there waits; once none does, the ask is denied at once."""
    t.write_config(t.args.url, "[guard]\nask_timeout = 60\ntemp = ask\n")
    a = Client(t)
    sa = a.attach()
    a.call("hello")
    r = a.call("new", cwd=t.work, model=t.model("ask"), attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    b = Client(t)
    sb = b.attach(reconnect=True)
    b.call("hello")
    t.check(b.call("open", sid=sid, attended=1).get("ok") == 1, "B opened it attended")
    t.check(b.call("open", sid=sid, attended=0).get("ok") == 1, "B opened it unattended")
    t.check(a.call("prompt", sid=sid, text="write outside").get("ok") == 1, "prompted")
    sa.wait(lambda r: r.get("ev") == "ask" and r.get("sid") == sid, what="the ask")
    time.sleep(1)
    t.check(not any(r.get("ev") == "answer" and r.get("sid") == sid for r in sa.records),
            "the ask was answered while A attends the session")
    t.check(a.call("open", sid=sid, attended=0).get("ok") == 1, "A opened it unattended")
    ans = sa.wait(lambda r: r.get("ev") == "answer" and r.get("sid") == sid,
                  what="the answer", timeout=10)
    t.check(ans.get("decision") == "deny" and ans.get("by") == "policy", f"answer: {ans}")
    sa.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    for s in (sa, sb):
        s.disconnect()


@scenario
def version_mismatch(t):
    """A host of another protocol: a clear error, and the end."""
    env = dict(t.env, PROSPERO_TEST_PROTO="7")
    c = connect(t, ["--local", "--state", t.state, "-c", t.config], env=env)
    try:
        rc = c.p.wait(20)
    except subprocess.TimeoutExpired:
        c.p.kill()
        raise Failure("connect did not give up")
    err = c.p.stderr.read()
    t.check(rc == 2, f"exit {rc}: {err}")
    t.check("speaks protocol 7" in err and "install a matching version" in err,
            f"stderr {err!r}")
    fails = [r for r in c.records if r.get("ev") == "remote" and r.get("state") == "failed"]
    t.check(fails, "a failed record")


@scenario
def file_cmd(t):
    """The file command: a call's spill file through the host, in parts,
    and nothing outside the session's directory, a link out included."""
    c = Client(t)
    s = c.attach()
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="go", model=t.model("remote_tool"), attended=1)
    sid = r["sid"]
    end = s.wait(lambda r: r.get("ev") == "tool.end" and r.get("sid") == sid,
                 what="the tool")
    spill = end.get("spill")
    t.check(spill, f"a spill file: {end}")
    f = c.call("file", sid=sid, path=spill)
    t.check(f.get("ok") == 1 and f.get("text") == "hi\n" and f.get("more") == 0 and
            f.get("size") == 3 and f.get("off") == 3, f"the whole file: {f}")
    p = c.call("file", sid=sid, path=spill, off=1, max=1)
    t.check(p.get("ok") == 1 and p.get("text") == "i" and p.get("more") == 1 and
            p.get("off") == 2, f"a part: {p}")
    sdir = os.path.join(t.state, "sessions", sid)
    link = ["out/link.ini"]
    try:
        os.symlink(t.config, os.path.join(sdir, "out", "link.ini"))
    except OSError:
        if os.name != "nt":
            raise
        link = []  # Windows, without the privilege to make links
    for bad in ["../../prospero.ini", "/etc/hosts", "out/../../" + sid + "/events.jsonl",
                "", "C:x", "out\\..\\..\\x"] + link:
        x = c.call("file", sid=sid, path=bad)
        t.check(x.get("ok") == 0, f"refused {bad!r}: {x}")
    e = c.call("file", sid=sid, path="events.jsonl", max=64)
    t.check(e.get("ok") == 1 and e.get("more") == 1 and e.get("off") == 64 and
            e.get("text", "").startswith('{"sid":"' + sid), f"its log, in part: {e}")
    # A binary file, as a screenshot is: in base64, exactly, cut anywhere
    blob = bytes(range(256)) * 3 + b"\x89PNG\r\n\x1a\n\xe2\x82"
    with open(os.path.join(sdir, "out", "shot.png"), "wb") as f:
        f.write(blob)
    b = c.call("file", sid=sid, path="out/shot.png", encoding="base64")
    t.check(b.get("ok") == 1 and "text" not in b and b.get("more") == 0 and
            b.get("size") == len(blob) and base64.b64decode(b.get("data", "")) == blob,
            f"the binary file whole: { {k: v for k, v in b.items() if k != 'data'} }")
    got, off = b"", 0
    while True:
        p = c.call("file", sid=sid, path="out/shot.png", encoding="base64", off=off, max=100)
        part = base64.b64decode(p.get("data", ""))
        t.check(p.get("ok") == 1 and p.get("off") == off + len(part) and len(part) == min(100, len(blob) - off),
                f"a part at {off}: { {k: v for k, v in p.items() if k != 'data'} }")
        got, off = got + part, p["off"]
        if not p.get("more"):
            break
    t.check(got == blob, "the binary file in parts")
    s.disconnect()


@scenario
def stop_cmd(t):
    """The stop command, as the tray's "Stop all": the turn ends, and the
    engine runs on."""
    c = Client(t)
    s = c.attach()
    c.call("hello")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    s.wait(lambda r: r.get("ev") == "delta" and r.get("sid") == sid, what="a delta")
    x = c.call("stop")
    t.check(x.get("ok") == 1, f"stop: {x}")
    end = s.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                 what="turn.end")
    t.check(end.get("why") == "cancel", f"turn.end {end}")
    st = c.call("status")
    t.check(st.get("ok") == 1 and st.get("turns") == 0, f"status after: {st}")
    p = c.call("prompt", sid=sid, text="again")
    t.check(p.get("ok") == 1, f"a prompt after: {p}")
    s.wait(lambda r: r.get("ev") == "turn.end" and r.get("turn") == 2, timeout=30,
           what="turn 2")
    s.disconnect()


@scenario
def connect_piped(t):
    """Commands piped to connect, its input ending at once: each is sent
    and answered before the link ends, a quit included."""
    a = Client(t)
    a.attach()
    a.call("hello")
    a.stream.disconnect()
    cmds = '{"cmd":"list","id":1}\n{"cmd":"quit","id":2}\n'
    p = subprocess.run([t.prospero, "connect", "--local", "--state", t.state, "-c", t.config],
                       input=cmds, capture_output=True, text=True, timeout=60, env=t.env)
    replies = {}
    for line in p.stdout.splitlines():
        r = json.loads(line)
        if r.get("ev") == "reply":
            replies[r["id"]] = r
    t.check(p.returncode == 0, f"connect exit {p.returncode}: {p.stderr}")
    t.check(replies.get(1, {}).get("ok") == 1 and replies.get(2, {}).get("ok") == 1,
            f"both answered: {replies}")
    t.check(waitgone(t), f"the host quit: {t.hostpids()}")


def waitgone(t, within=15.0):
    deadline = time.monotonic() + within
    while time.monotonic() < deadline:
        if not t.hostpids():
            return True
        time.sleep(0.2)
    return False


@scenario
def connect_endpoint(t):
    """The client library at a registered endpoint, as the GUI reaches a
    background host: it starts no host, and once the host is gone it
    ends, closed, rather than try forever."""
    a = Client(t)
    sa = a.attach()
    a.call("hello")
    hosts = subprocess.run([t.prospero, "hosts", "--state", t.state],
                           capture_output=True, text=True).stdout.splitlines()
    t.check(len(hosts) == 1, f"one host: {hosts}")
    ep = json.loads(hosts[0])["endpoint"]
    c = connect(t, ["--endpoint", ep, "--state", os.path.join(t.root, "local")])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    c.wait(lambda r: r.get("ev") == "turn.end", what="turn.end")
    q = a.call("quit")
    t.check(q.get("ok") == 1, f"quit: {q}")
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "closed",
           what="closed", timeout=30)
    closed = [r for r in c.records if r.get("ev") == "remote" and r.get("state") == "closed"]
    t.check("gone" in closed[-1].get("msg", "") or "stopped" in closed[-1].get("msg", ""),
            f"why: {closed[-1]}")
    try:
        rc = c.p.wait(10)
    except subprocess.TimeoutExpired:
        raise Failure("connect did not end with its host")
    t.check(rc == 0, f"connect exit {rc}")
    t.check(not t.hostpids(), "no host started")
    sa.disconnect()


# The parts of a fake ssh, in Python, so that it runs on Windows too
FAKE = r'''
import os, subprocess, sys, time

def record(path):
    """This ssh's pid, the one the harness started: on Windows the shim's
    (shim), which runs this script."""
    with open(path, "a") as f:
        f.write(f"{os.environ['PROSPERO_SHIM_PID'] if os.name == 'nt' else os.getpid()}\n")

def command(args):
    """ssh's command, after its options and destination."""
    while len(args) > 1:
        if args[0] == "-o":
            args = args[2:]
        elif args[0].startswith("-"):
            args = args[1:]
        else:
            return args[1]
    return args[0] if args else ""

def run(argv):
    """argv in place of this process, or on Windows, which cannot, as its
    child, in the shim's job, which the shim's death takes down."""
    if os.name != "nt":
        os.execvp(argv[0], argv)
    sys.exit(subprocess.call(argv))
'''


def program(t, name, source):
    """A program of the scenario's own in Python, for the harness to run
    as its ssh: a #! script, or on Windows, which runs no scripts as
    programs, the shim running this Python on it (shim)."""
    if os.name != "nt":
        path = os.path.join(t.root, name)
        with open(path, "w", newline="") as f:
            f.write(f"#!{sys.executable}\n" + source)
        os.chmod(path, 0o755)
        return path
    script = os.path.join(t.root, name + ".py")
    with open(script, "w", newline="") as f:
        f.write(source)
    return shim(t.args, os.path.join(t.root, name + ".exe"), [sys.executable, "-I", script])


def shim(args, path, argv):
    """Windows: a program at path that runs argv followed by its own
    arguments: a copy of the shim (tests/shim_win32.c, built beside the
    harness and given as --shim) with that command line appended."""
    if not args.shim:
        raise Failure("on Windows this needs --shim, the program of tests/shim_win32.c")
    with open(args.shim, "rb") as f:
        data = f.read()
    cmd = subprocess.list2cmdline(argv).encode()
    with open(path, "wb") as f:
        f.write(data + cmd + struct.pack("<I", len(cmd)) + b"SHIM")
    return path


def fakessh(t):
    """An ssh that runs the command here, recording its pid."""
    pids = os.path.join(t.root, "ssh.pids")
    path = program(t, "fakessh", FAKE + f"record({pids!r})\n"
                   "run(['sh', '-c', command(sys.argv[1:])])\n")
    return path, pids


def sshpids(path):
    """The pids the fake ssh wrote, one per line."""
    return P.read(path)


@scenario
def connect_reconnect(t):
    """The client library over a fake ssh killed mid-stream: it reconnects
    with --reconnect and nothing is lost or repeated, deltas included."""
    prog, pidfile = fakessh(t)
    # The host starts first, here: started through the fake ssh, it would
    # descend from the library's reaper, which on Linux, a subreaper,
    # would take it down with the ssh it kills (a real ssh's attach runs
    # on another machine)
    first = Client(t)
    first.attach()
    first.call("hello")
    first.stream.disconnect()
    c = connect(t, ["--ssh", "here", "--ssh-program", prog, "--path", t.prospero,
                    "--remote-state", t.state, "--remote-config", t.config,
                    "--state", os.path.join(t.root, "local")])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "delta" and "w5 " in r.get("text", ""), what="deltas")
    pids = sshpids(pidfile)
    signal_ours(pids[-1], P.KILL, lambda p: grandchild(p, c.p.pid), "ssh")
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "down", what="down")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end",
           timeout=40)
    states = [(r.get("state"), r.get("msg")) for r in c.records if r.get("ev") == "remote"]
    t.check(len(sshpids(pidfile)) == 2, f"one reconnect: {sshpids(pidfile)} {states}")
    seqs = [r["seq"] for r in c.records if r.get("sid") == sid and "seq" in r]
    t.check(seqs == list(range(1, len(seqs) + 1)), f"seqs {seqs}")
    want = finaltext(c.records, sid)
    t.check(want and deltatext(c.records, sid) == want,
            f"deltas {deltatext(c.records, sid)!r} != {want!r}")
    t.check(c.close() == 0, "connect exits 0")


def slowssh(t, slow, delay):
    """A fake ssh, as fakessh's, whose invocation number slow (from 1)
    first sleeps, with what the client writes waiting in its input."""
    count = os.path.join(t.root, "ssh.count")
    return program(t, "slowssh", FAKE + f"""
try:
    with open({count!r}) as f:
        n = int(f.read()) + 1
except OSError:
    n = 1
with open({count!r}, "w") as f:
    f.write(f"{{n}}\\n")
if n == {slow}:
    time.sleep({delay})
run(["sh", "-c", command(sys.argv[1:])])
""")


def ups(c):
    return len([r for r in c.records if r.get("ev") == "remote" and r.get("state") == "up"])


@scenario
def connect_slowstart(t):
    """The host quits under the client library, and the stream starting
    the next one takes over 5 s to come up, the library's replay request
    waiting in it: the link comes up on that stream, as nothing else
    went before the new host's own request."""
    first = Client(t)
    first.attach()
    first.call("hello")
    first.stream.disconnect()
    prog = slowssh(t, 2, 6)
    c = connect(t, ["--ssh", "here", "--ssh-program", prog, "--path", t.prospero,
                    "--remote-state", t.state, "--remote-config", t.config,
                    "--state", os.path.join(t.root, "local")])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    q = Client(t)
    q.attach(reconnect=True)
    q.call("hello")
    q.call("quit")
    q.stream.close()
    t0 = time.monotonic()
    c.wait(lambda r: ups(c) >= 2, what="up again", timeout=40)
    dt = time.monotonic() - t0
    downs = [r.get("msg") for r in c.records if r.get("ev") == "remote"
             and r.get("state") == "down"]
    t.check(len(downs) == 1, f"down once, for the quit: {downs}")
    t.check(dt < 20, f"up again after {dt:.1f} s")
    p = c.call("prompt", sid=sid, text="more")
    t.check(p.get("ok") == 1, f"a prompt on the new host: {p}")
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("turn") == 2, what="turn 2")
    x = c.call("quit")
    t.check(x.get("ok") == 1, f"quit: {x}")
    t.check(c.close() == 0, "connect exits 0")


THROTTLE = r'''
# A fake ssh: runs the command here, passing its output on at RATE
# bytes a second, its input straight through.
import subprocess, sys, time
args = sys.argv[1:]
while len(args) > 1:
    if args[0] == "-o":
        args = args[2:]
    elif args[0].startswith("-"):
        args = args[1:]
    else:
        args = args[1:]
        break
p = subprocess.Popen(["sh", "-c", args[0]], stdout=subprocess.PIPE)
out = sys.stdout.buffer
t0 = time.monotonic()
sent = 0
while True:
    data = p.stdout.read1(4096)
    if not data:
        break
    for i in range(0, len(data), 1024):
        piece = data[i:i+1024]
        sent += len(piece)
        wait = t0 + sent/RATE - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        try:
            out.write(piece)
            out.flush()
        except OSError:
            sys.exit(1)
sys.exit(p.wait())
'''


@scenario
def slow_link(t):
    """A link that carries 40 KB/s, and a reply (the file command's, of
    1 MB) that takes it 26 s to bring, longer than a silent stream is
    given: its bytes, still arriving, keep the stream, and it comes."""
    first = Client(t)
    first.attach()
    first.call("hello")
    first.stream.disconnect()
    prog = program(t, "slowssh", THROTTLE.replace("RATE", "40000"))
    c = connect(t, ["--ssh", "here", "--ssh-program", prog, "--path", t.prospero,
                    "--remote-state", t.state, "--remote-config", t.config,
                    "--state", os.path.join(t.root, "local")])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    line = "x" * 99 + "\n"
    size = (1 << 20) - 4096
    with open(os.path.join(t.state, "sessions", sid, "big.txt"), "w", newline="") as f:
        f.write(line * (size // len(line)))
    t0 = time.monotonic()
    got = c.call("file", sid=sid, path="big.txt", timeout=60)
    dt = time.monotonic() - t0
    t.check(got.get("ok") == 1 and len(got.get("text", "")) == size // len(line) * len(line),
            f"the file: {got.get('ok')} {len(got.get('text', ''))} {got.get('err')}")
    t.check(dt > 21, f"slow enough to matter: {dt:.1f} s")
    downs = [r.get("msg") for r in c.records if r.get("ev") == "remote"
             and r.get("state") == "down"]
    t.check(not downs, f"never down: {downs}")
    x = c.call("quit", timeout=60)
    t.check(x.get("ok") == 1, f"quit: {x}")
    t.check(c.close(timeout=60) == 0, "connect exits 0")


# Over real ssh

@scenario
def ssh_reconnect(t):
    """connect --ssh to a real host, its ssh killed mid-stream."""
    if not t.args.ssh:
        return "skip"
    a = t.args
    pidfile = os.path.join(t.root, "ssh.pids")
    prog = program(t, "sshwrap", FAKE + f"record({pidfile!r})\n"
                   "run(['ssh'] + sys.argv[1:])\n")
    rstate = f"{a.remote_root}/state-{t.nonce}"
    rconf = f"{a.remote_root}/prospero-{t.nonce}.ini"
    remote_write(a, rconf, open(t.config).read().replace(a.url, a.remote_url))
    try:
        c = connect(t, ["--ssh", a.ssh, "--ssh-program", prog, "--path", a.remote_prospero,
                        "--remote-state", rstate, "--remote-config", rconf,
                        "--state", os.path.join(t.root, "local")])
        c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up",
               timeout=60)
        r = c.call("new", cwd=a.remote_work, prompt="go", attended=1, timeout=60)
        t.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        c.wait(lambda r: r.get("ev") == "delta" and "w5 " in r.get("text", ""),
               what="deltas", timeout=60)
        signal_ours(sshpids(pidfile)[-1], P.KILL,
                    lambda p: grandchild(p, c.p.pid), "ssh")
        c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "down", what="down")
        c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
               what="turn.end", timeout=90)
        seqs = [r["seq"] for r in c.records if r.get("sid") == sid and "seq" in r]
        t.check(seqs == list(range(1, len(seqs) + 1)), f"seqs {seqs}")
        want = finaltext(c.records, sid)
        t.check(want and deltatext(c.records, sid) == want,
                f"deltas {deltatext(c.records, sid)!r} != {want!r}")
        q = c.call("quit", timeout=30)
        t.check(q.get("ok") == 1, f"quit {q}")
        t.check(c.close(timeout=30) == 0, "connect exits 0")
    finally:
        remote_sh(a, f"rm -rf {rstate} {rconf}")


@scenario
def ssh_offline(t):
    """connect --ssh to a real host whose turn retries against a dead
    endpoint, so that records keep coming without a provider (for a host
    that cannot forward a port back, like the XP one): its ssh killed in
    the middle, the records come through once each, in order."""
    if not t.args.ssh:
        return "skip"
    a = t.args
    pidfile = os.path.join(t.root, "ssh.pids")
    prog = program(t, "sshwrap", FAKE + f"record({pidfile!r})\n"
                   "run(['ssh'] + sys.argv[1:])\n")
    rstate = f"{a.remote_root}/state-{t.nonce}"
    rconf = f"{a.remote_root}/prospero-{t.nonce}.ini"
    remote_write(a, rconf, "endpoint = dead\nmodel = m\n[endpoint dead]\n"
                           "url = http://127.0.0.1:9/\nconnect_timeout = 1\n")
    try:
        c = connect(t, ["--ssh", a.ssh, "--ssh-program", prog, "--path", a.remote_prospero,
                        "--remote-state", rstate, "--remote-config", rconf,
                        "--state", os.path.join(t.root, "local")])
        c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up",
               timeout=120)
        r = c.call("new", cwd=a.remote_root, prompt="go", attended=1, timeout=120)
        t.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        c.wait(lambda r: r.get("ev") == "retry" and r.get("sid") == sid, what="a retry",
               timeout=120)
        signal_ours(sshpids(pidfile)[-1], P.KILL,
                    lambda p: grandchild(p, c.p.pid), "ssh")
        c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "down", what="down")
        ups = lambda: len([x for x in c.records if x.get("ev") == "remote"
                           and x.get("state") == "up"])
        c.wait(lambda r: ups() >= 2, what="up again", timeout=180)
        n = len([x for x in c.records if x.get("ev") == "retry"])
        c.wait(lambda r: len([x for x in c.records if x.get("ev") == "retry"]) > n + 1,
               what="more retries", timeout=120)
        x = c.call("cancel", sid=sid, timeout=120)
        t.check(x.get("ok") == 1, f"cancel: {x}")
        end = c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                     what="turn.end", timeout=120)
        t.check(end.get("why") == "cancel", f"turn.end {end}")
        seqs = [r["seq"] for r in c.records if r.get("sid") == sid and "seq" in r]
        t.check(seqs == list(range(1, len(seqs) + 1)), f"seqs {seqs}")
        q = c.call("quit", timeout=120)
        t.check(q.get("ok") == 1, f"quit {q}")
        t.check(c.close(timeout=120) == 0, "connect exits 0")
    finally:
        remote_sh(a, f"rm -rf {rstate} {rconf}")


# Subagents on other hosts: a headless engine here (X), whose agent
# starts subagents on y, a host of this machine's (Y, the scenario's
# own state and configuration) reached through a fake ssh, or a fake
# wsl.exe. Y starts first, as for connect_reconnect.

F = fake_provider


FARHOST = ["y"]  # the host the scripts name, for the scenario running


def sc_far_task(model, n, req):
    """Two subagents on y in one response, then what the parent heard."""
    if F.step(req) == 0:
        yield from F.calls(model, n, [
            ("task", {"description": "One", "prompt": "First job.", "host": FARHOST[0],
                      "cwd": os.environ.get("FAR_WORK", "~"), "model": F.second(model)}),
            ("task", {"description": "Two", "prompt": "Second job.", "host": FARHOST[0],
                      "cwd": os.environ.get("FAR_WORK", "~"), "model": F.second(model)})])
    else:
        yield from F.answer(model, "Parent heard: " + " / ".join(F.tool_results(req)))


def sc_far_task_b(model, n, req):
    where = "wsl" if "Windows on this machine" in F.system_of(req) else "ssh"
    yield from F.answer(model, f"Done ({where}): {F.task_prompt(req)}")


def sc_far_fallback(model, n, req):
    """No model given: the parent's own, which y has, runs it there."""
    if "# Subagent" in F.system_of(req):
        yield from F.answer(model, "On the parent's model.")
    elif F.step(req) == 0:
        yield from F.tool(model, n, "task", {"description": "Plain", "prompt": "Go.",
                                             "host": "y", "cwd": os.environ["FAR_WORK"]})
    else:
        yield from F.answer(model, "Heard: " + F.tool_results(req)[0])


def sc_far_sleep(model, n, req):
    """One subagent on y, which sleeps; the parent says what it heard."""
    if F.step(req) == 0:
        yield from F.tool(model, n, "task", {"description": "Sleep", "prompt": "Sleep.",
                                             "host": "y", "cwd": os.environ["FAR_WORK"],
                                             "model": F.second(model)})
    else:
        yield from F.answer(model, "Heard: " + F.last_user(req)[:300])


def sc_far_sleep_b(model, n, req):
    if F.step(req) == 0:
        yield from F.tool(model, n, "shell", {"command": "echo $$ > far.pid; exec sleep 30",
                                              "timeout": 60})
    else:
        yield from F.answer(model, "Slept.")


def sc_far_flag_b(model, n, req):
    """Waits for a flag file, then reports."""
    if F.step(req) == 0:
        yield from F.tool(model, n, "shell", {
            "command": "while [ ! -f flag ]; do sleep 0.1; done; echo flagged",
            "timeout": 60})
    else:
        yield from F.answer(model, "Saw the flag.")


def sc_far_ask_b(model, n, req):
    """Writes outside its directory, which asks."""
    if F.step(req) == 0:
        yield from F.tool(model, n, "write", {"path": "../far-outside.txt", "content": "x"})
    else:
        yield from F.answer(model, "Wrote: " + F.tool_results(req)[0][:80])


for _name in ("far_task", "far_task_b", "far_fallback", "far_sleep", "far_sleep_b"):
    fake_provider.SCENARIOS[_name] = globals()["sc_" + _name]
for _name in ("far_cancel", "far_quit", "far_restart"):
    fake_provider.SCENARIOS[_name] = sc_far_sleep
    fake_provider.SCENARIOS[_name + "_b"] = sc_far_sleep_b
fake_provider.SCENARIOS["far_background"] = sc_far_sleep
fake_provider.SCENARIOS["far_background_b"] = sc_far_flag_b
fake_provider.SCENARIOS["far_attend"] = sc_far_sleep
fake_provider.SCENARIOS["far_attend_b"] = sc_far_ask_b
for _name in ("far_old", "far_never", "far_wsl", "relay_far"):
    fake_provider.SCENARIOS[_name] = sc_far_task
    fake_provider.SCENARIOS[_name + "_b"] = sc_far_task_b

FAKEWSL = FAKE + r"""
# wsl.exe: -l -q lists Ubuntu in UTF-16, as it does by default; -d
# Ubuntu --cd ~ --exec /bin/sh -c CMD runs CMD here
args = sys.argv[1:]
if args == ["-l", "-q"]:
    sys.stdout.buffer.write("Ubuntu\r\n".encode("utf-16-le"))
    sys.exit(0)
if len(args) != 8 or args[:2] != ["-d", "Ubuntu"] or args[6] != "-c":
    sys.stderr.write(f"fakewsl: unexpected {args}\n")
    sys.exit(2)
run(["/bin/sh", "-c", args[7]])
"""


class Far:
    """Y, the host of this scenario's state directory, then X, a headless
    engine of its own whose [remote y] reaches Y through a fake ssh."""

    def __init__(self, t, scenario, yenv=None, ypath=None, wsl=False):
        self.t = t
        self.model = t.model(scenario)
        y = Client(t)
        y.attach(env=yenv)
        y.call("hello")
        y.stream.disconnect()
        self.xstate = os.path.join(t.root, "xstate")
        self.xconfig = os.path.join(t.root, "x.ini")
        os.makedirs(os.path.join(self.xstate, "remote"))
        prog, self.sshpids = fakessh(t)
        section = "[remote Ubuntu]\n" if wsl else "[remote y]\nssh = here\n"
        with open(self.xconfig, "w", newline="") as f:
            f.write(f"endpoint = fake\nmodel = {self.model}\n"
                    f"[endpoint fake]\nurl = {t.args.url}/\nfirst_byte_timeout = 5\n"
                    f"idle_timeout = 5\nprogress_timeout = 10\nconnect_timeout = 2\n"
                    f"{section}path = {ypath or t.prospero}\nstate = {t.state}\n"
                    f"config = {t.config}\n")
        FARHOST[0] = "Ubuntu" if wsl else "y"
        env = dict(t.env, PROSPERO_TEST_SSH=prog, FAR_WORK=t.work)
        if wsl:
            env["PROSPERO_TEST_WSL"] = program(t, "fakewsl", FAKEWSL)
            with open(os.path.join(self.xstate, "remote", "wsl.list"), "w") as f:
                f.write("Ubuntu\n")  # as listed when it last ran
        p = subprocess.Popen([t.prospero, "--headless", "-C", t.work, "--state", self.xstate,
                              "-c", self.xconfig, "-e", "fake", "-m", self.model],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
        t.procs.append(p)
        self.x = Lines(p)
        os.environ["FAR_WORK"] = t.work  # the provider's scripts, in this process

    def parent(self, attended=1, prompt="go"):
        r = self.x.call("new", cwd=self.t.work, model=self.model, endpoint="fake",
                        attended=attended)
        self.t.check(r.get("ok") == 1, f"new: {r}")
        self.x.send("prompt", sid=r["sid"], text=prompt)
        return r["sid"]

    def ends(self, sid, n=1, timeout=40):
        return self.x.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid
                           and r.get("turn") == n, timeout, f"turn {n} end")

    def ylog(self, sid):
        return [json.loads(x) for x in self.t.log(sid)]

    def ykids(self):
        out = []
        root = os.path.join(self.t.state, "sessions")
        for sid in sorted(os.listdir(root)) if os.path.isdir(root) else []:
            path = os.path.join(root, sid, "events.jsonl")
            if os.path.exists(path):
                first = self.ylog(sid)[0]
                if first.get("remote") == 1:
                    out.append(first)
        return out

    def quit(self):
        if self.x.p.poll() is not None:
            return  # quit already
        try:
            self.x.call("quit", timeout=10)
            self.x.close(timeout=15)
        except (Failure, OSError, ValueError):
            pass


@scenario
def far_task(t):
    """Two subagents on another host in one response: each is a session
    there, a subagent of the parent here, whose report is its call's
    result; the parent's log says where each ran (task.host)."""
    f = Far(t, "far_task")
    try:
        sid = f.parent()
        end = f.ends(sid)
        t.check(end.get("why") == "done", f"turn end {end}")
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(len(te) == 2 and all(r["status"] == "ok" and r.get("host") == "y"
                                     for r in te), f"tool.ends {te}")
        t.check([report(r["text"]) for r in te] == ["Done (ssh): First job.",
                                                    "Done (ssh): Second job."],
                f"reports {[r['text'] for r in te]}")
        kids = f.ykids()
        t.check(len(kids) == 2 and all(k["parent"] == sid and k["via"] == "ssh"
                                       for k in kids), f"y's sessions {kids}")
        t.check({r.get("child") for r in te} == {k["sid"] for k in kids}, "named")
        hosts = [r for r in f.x.records if r.get("ev") == "task.host" and r.get("sid") == sid]
        t.check(len(hosts) == 2 and {h["child"] for h in hosts} == {k["sid"] for k in kids},
                f"task.host {hosts}")
        for k in kids:
            reps = [r for r in f.ylog(k["sid"]) if r.get("ev") == "task.report"]
            t.check(len(reps) == 1 and reps[0]["status"] == "ok", f"y's report {reps}")
        tools = [x for q in fake_provider.STATE.log(f.model)[:1]
                 for x in q["body"].get("tools", []) if x["function"]["name"] == "task"]
        desc = tools[0]["function"]["description"] if tools else ""
        t.check("- y: another machine over ssh" in desc and
                "host" in tools[0]["function"]["parameters"]["properties"], f"told {desc!r}")
    finally:
        f.quit()


@scenario
def far_fallback(t):
    """No model in the call: the parent's own, as y has its endpoint."""
    f = Far(t, "far_fallback")
    try:
        sid = f.parent()
        f.ends(sid)
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(te and report(te[0]["text"]) == "On the parent's model.", f"tool.end {te}")
        kids = f.ykids()
        t.check(kids and kids[0]["model"] == f.model, f"y's model {kids}")
    finally:
        f.quit()


def farpid(t, within=20):
    path = os.path.join(t.work, "far.pid")
    deadline = time.monotonic() + within
    while time.monotonic() < deadline:
        pids = P.read(path) if os.path.exists(path) else []
        if pids:
            return pids[0]
        time.sleep(0.1)
    raise Failure("the subagent there never started its command")


def ycommand(t, pid):
    """Whether pid is the sleep a subagent on Y started: ours, a sleep."""
    return P.mine(pid) and "sleep" in P.command(pid)


@scenario
def far_cancel(t):
    """Cancelling the parent ends the call at once, and the subagent
    there is cancelled: its command stops."""
    f = Far(t, "far_cancel")
    try:
        sid = f.parent()
        pid = farpid(t)
        f.x.call("cancel", sid=sid)
        te = f.x.wait(lambda r: r.get("ev") == "tool.end" and r.get("sid") == sid,
                      10, "the call's end")
        t.check(te["status"] == "cancelled" and te.get("host") == "y", f"tool.end {te}")
        t.check(gone(pid, 10), "the command there stopped")
        kid = f.ykids()[0]
        rep = None
        deadline = time.monotonic() + 10
        while not rep and time.monotonic() < deadline:
            rep = [r for r in f.ylog(kid["sid"]) if r.get("ev") == "task.report"]
            time.sleep(0.1)
        t.check(rep and rep[0]["status"] == "cancelled", f"y's report {rep}")
    finally:
        f.quit()


@scenario
def far_background(t):
    """Moved to the background, the call ends; once the subagent there
    finishes, its report comes to the parent as a notice, naming the
    host, which starts its turn."""
    f = Far(t, "far_background")
    try:
        sid = f.parent()
        f.x.wait(lambda r: r.get("ev") == "task.host" and r.get("sid") == sid, 20, "named")
        r = f.x.call("background", sid=sid)
        t.check(r.get("ok") == 1, f"background {r}")
        f.ends(sid, 1)
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(te and te[0]["status"] == "background" and "continues on y" in te[0]["text"],
                f"tool.end {te}")
        with open(os.path.join(t.work, "flag"), "w") as fl:
            fl.write("go\n")
        n = f.x.wait(lambda r: r.get("ev") == "notice" and r.get("sid") == sid
                     and r.get("kind") == "task", 30, "the notice")
        t.check(n.get("host") == "y" and "Saw the flag." in n["text"] and " on y)" in n["text"],
                f"notice {n}")
        end = f.ends(sid, 2)
        t.check(end.get("why") == "done", f"turn 2 {end}")
    finally:
        f.quit()


@scenario
def far_old(t):
    """A host whose engine lacks task.remote: the calls fail, saying to
    update it, and nothing is made there."""
    f = Far(t, "far_old", yenv=dict(t.env, PROSPERO_TEST_DROPCAP="task.remote"))
    try:
        sid = f.parent()
        f.ends(sid)
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(len(te) == 2 and all(r["status"] == "error" and "too old" in r["text"]
                                     and "update the engine" in r["text"] for r in te),
                f"tool.ends {te}")
        t.check(not f.ykids(), "nothing made there")
    finally:
        f.quit()


@scenario
def far_never(t):
    """A host never reached: the calls fail with why, and a call soon
    after fails at once."""
    f = Far(t, "far_never", ypath=os.path.join(t.root, "nonexistent", "prospero"))
    try:
        sid = f.parent()
        f.ends(sid, timeout=90)
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(len(te) == 2 and all(r["status"] == "error" and
                                     r["text"].startswith("No subagent was started on y")
                                     for r in te), f"tool.ends {te}")
    finally:
        f.quit()


@scenario
def far_restart(t):
    """The host there killed while the subagent works: the link starts
    another, which opens the subagent again, and its report says it was
    interrupted."""
    f = Far(t, "far_restart")
    try:
        sid = f.parent()
        pid = farpid(t)
        f.x.wait(lambda r: r.get("ev") == "task.host" and r.get("sid") == sid, 20, "named")
        for h in t.hostpids():
            signal_ours(h, P.KILL, t.ownhost, "host")
        te = f.x.wait(lambda r: r.get("ev") == "tool.end" and r.get("sid") == sid,
                      60, "the call's end")
        t.check(te["status"] == "error" and "stopped while it worked" in te["text"],
                f"tool.end {te}")
        if not gone(pid, 1):
            signal_ours(pid, P.KILL, lambda p: ycommand(t, p), "sleep")
    finally:
        f.quit()


@scenario
def far_quit(t):
    """Quitting the engine here cancels the subagent there, and does not
    wait long for it."""
    f = Far(t, "far_quit")
    try:
        f.parent()
        pid = farpid(t)
        kid = f.ykids()[0]["sid"]
        t0 = time.monotonic()
        f.x.call("quit", timeout=10)
        t.check(f.x.close(timeout=15) == 0, "exits 0")
        t.check(time.monotonic() - t0 < 8, f"quit in {time.monotonic() - t0:.1f} s")
        t.check(gone(pid, 10), "the command there stopped")
        ends = [r for r in f.ylog(kid) if r.get("ev") == "turn.end"]
        t.check(ends and ends[-1]["why"] == "cancel", f"y's turn end {ends}")
    finally:
        f.quit()


@scenario
def far_attend(t):
    """Attended while its parent is: the subagent's ask waits there; once
    the parent is closed here, it is denied at once."""
    t.write_config(t.args.url, "[guard]\nask_timeout = 60\ntemp = ask\n")
    f = Far(t, "far_attend")
    try:
        sid = f.parent(attended=1)
        f.x.wait(lambda r: r.get("ev") == "task.host" and r.get("sid") == sid, 20, "named")
        kid = f.ykids()[0]["sid"]
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not any(
                r.get("ev") == "ask" for r in f.ylog(kid)):
            time.sleep(0.1)
        time.sleep(1)
        log = f.ylog(kid)
        t.check(any(r.get("ev") == "ask" for r in log) and
                not any(r.get("ev") == "answer" for r in log), "the ask waits")
        f.x.call("close", sid=sid)
        deadline = time.monotonic() + 10
        ans = []
        while time.monotonic() < deadline and not ans:
            ans = [r for r in f.ylog(kid) if r.get("ev") == "answer"]
            time.sleep(0.1)
        t.check(ans and ans[0]["by"] == "policy" and ans[0]["decision"] == "deny",
                f"answered {ans}")
    finally:
        f.quit()


@scenario
def far_wsl(t):
    """A WSL distro as listed by wsl.exe (here a stand-in), a host with no
    ssh: the tool lists it, the subagent there is told it works for an
    agent in Windows, and its session names the distro."""
    if os.name == "nt":
        return "skip"
    f = Far(t, "far_wsl", wsl=True)
    try:
        sid = f.parent()
        f.ends(sid)
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(len(te) == 2 and all(r["status"] == "ok" and r.get("host") == "Ubuntu"
                                     for r in te), f"tool.ends {te}")
        t.check(report(te[0]["text"]) == "Done (wsl): First job.", f"report {te[0]['text']}")
        kids = f.ykids()
        t.check(len(kids) == 2 and all(k["via"] == "wsl" and k["distro"] == "Ubuntu"
                                       for k in kids), f"y's sessions {kids}")
        tools = [x for q in fake_provider.STATE.log(f.model)[:1]
                 for x in q["body"].get("tools", []) if x["function"]["name"] == "task"]
        desc = tools[0]["function"]["description"] if tools else ""
        t.check("- Ubuntu: Linux (WSL) on this machine" in desc and
                "\\\\wsl.localhost\\Ubuntu\\" in desc, f"told {desc!r}")
    finally:
        f.quit()


# Thin hosts: model calls relayed through a client (relay.cpp)

def deltaapply(base, ops, n):
    """delta.cpp's ops applied to base: n bytes, or None if unsound."""
    out = bytearray()
    i = 0

    def getv():
        nonlocal i
        v, shift = 0, 0
        while True:
            c = ops[i]
            i += 1
            v |= (c & 0x7f) << shift
            shift += 7
            if not c & 0x80:
                return v
    try:
        while i < len(ops):
            v = getv()
            m = v >> 1
            if v & 1:
                off = getv()
                if off + m > len(base):
                    return None
                out += base[off:off + m]
            else:
                out += ops[i:i + m]
                i += m
    except IndexError:
        return None
    return bytes(out) if len(out) == n else None


def thinconfig(t, extra=""):
    """A thin host's configuration: no endpoint of its own."""
    with open(t.config, "w", newline="") as f:
        f.write("[host]\n" + extra)


def offer(t, model, machine="testbox", budget=None, name="fake"):
    ep = {"name": name, "kind": "openai", "first_byte_timeout": "5",
          "idle_timeout": "5", "progress_timeout": "10", "models": []}
    if budget:
        ep["slow_retry_budget"] = str(budget)
    return {"machine": machine, "endpoint": name, "model": model, "endpoints": [ep],
            "models": [{"name": model, "settings": {"context": "100000"}}]}


class Relayer:
    """A client relaying a thin host's calls: each relay record's call made
    here, at the fake provider, its response sent back as relay.data."""

    def __init__(self, t, client):
        self.t, self.c = t, client
        self.parts = {}       # rid: (first part, [data, ...])
        self.bases = {}       # session: the last turn body
        self.calls = []       # (first part, body), as made
        self.cancelled = set()
        self.lock = threading.Lock()
        self.seen = 0
        self.stop = False
        self.mute = False     # takes calls, answers none
        threading.Thread(target=self.run, daemon=True).start()

    def send(self, obj):
        obj["cmd"] = "relay.data"
        self.c.stream.command(obj)

    def run(self):
        while not self.stop:
            recs = self.c.all()
            new, self.seen = recs[self.seen:], len(recs)
            for r in new:
                if r.get("ev") != "relay":
                    continue
                rid = r["rid"]
                if r.get("cancel"):
                    self.cancelled.add(rid)
                    continue
                if r["part"] == 0:
                    self.parts[rid] = (r, [])
                head, data = self.parts.get(rid, (None, None))
                if head is None:
                    continue
                data.append(base64.b64decode(r["data"]))
                if r["part"] == r["parts"] - 1:
                    del self.parts[rid]
                    threading.Thread(target=self.make, args=(rid, head, b"".join(data)),
                                     daemon=True).start()
            time.sleep(0.02)

    def make(self, rid, head, payload):
        if head.get("zlen"):
            payload = zlib.decompress(payload, -15)
        sid = head.get("session", "")
        if head.get("base"):
            base = self.bases.get(sid)
            body = deltaapply(base, payload, head["len"]) if base else None
            if body is None or "%016x" % fnv(base) != head["base"]:
                self.send({"rid": rid, "done": {"err": "nobase", "text": "no base"}})
                return
        else:
            body = payload
        if head.get("keep"):
            self.bases[sid] = body
        with self.lock:
            self.calls.append((head, body))
        if self.mute:
            return
        host, port = self.t.args.url.split("//")[1].rstrip("/").split(":")
        try:
            conn = http.client.HTTPConnection(host, int(port), timeout=30)
            conn.request(head["method"], "/" + head["path"],
                         body if head["method"] == "POST" else None,
                         {"Content-Type": "application/json", "Accept": head["accept"]})
            resp = conn.getresponse()
            hdrs = "".join(f"{k}: {v}\r\n" for k, v in resp.getheaders())
            self.send({"rid": rid, "head": {"status": resp.status, "headers": hdrs}})
            while rid not in self.cancelled:
                piece = resp.read1(1 << 16)
                if not piece:
                    break
                self.send({"rid": rid, "body": base64.b64encode(piece).decode()})
            conn.close()
            if rid not in self.cancelled:
                self.send({"rid": rid, "done": {"err": "ok", "text": ""}})
        except OSError as e:
            self.send({"rid": rid, "done": {"err": "connect", "text": str(e)}})


def fnv(data):
    """base.cpp's hash."""
    h = 0x100
    for c in data:
        h = ((h ^ c) * 1111111111111111111) & 0xffffffffffffffff
    return h


def relayclient(t, model, **kw):
    c = Client(t)
    c.attach()
    c.call("hello")
    r = c.call("relay.offer", offer=offer(t, model, **kw))
    t.check(r.get("ok") == 1 and r.get("thin") == 1, f"relay.offer: {r}")
    return c, Relayer(t, c)


@scenario
def relay_turn(t):
    """A thin host's turn, its calls relayed through the client: listing,
    then a turn body whole, then the next as a delta against it, its
    live records naming the computer relaying."""
    thinconfig(t)
    model = t.model("remote_tool")
    c, rl = relayclient(t, model)
    try:
        r = c.call("new", cwd=t.work, prompt="go")
        t.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        end = c.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                            timeout=30, what="turn.end")
        t.check(end.get("why") == "done", f"turn.end {end}")
        t.check(finaltext(c.all(), sid).startswith("w0 w1"), "the answer")
        posts = [(h, b) for h, b in rl.calls if h["method"] == "POST"]
        t.check(any(h["method"] == "GET" and h["path"].startswith("v1/models")
                    for h, b in rl.calls), "a listing relayed")
        t.check(len(posts) == 2 and not posts[0][0].get("base") and posts[1][0].get("base"),
                f"bodies: {[h.get('base') for h, b in posts]}")
        h1 = posts[1][0]
        t.check(h1["size"] < h1["len"] // 4, f"a delta of {h1['size']} for {h1['len']}")
        t.check(json.loads(posts[1][1])["model"] == model, "the model sent")
        live = [x for x in c.all() if x.get("ev") == "live" and x.get("sid") == sid
                and x.get("relay")]
        t.check(live and live[0]["relay"] == "testbox", "live names the relay")
    finally:
        rl.stop = True


@scenario
def relay_pause(t):
    """No computer relays: the turn pauses, and goes on once one offers.
    An offer whose computer went still names the endpoint and model."""
    thinconfig(t)
    model = t.model("remote_slow")
    a, rla = relayclient(t, model)
    rla.stop = True
    a.stream.disconnect()  # gone: its offer stays, chosen by none
    b = Client(t)
    b.attach(reconnect=True)
    b.call("hello")
    r = b.call("new", cwd=t.work, prompt="go")
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    b.stream.wait(lambda r: r.get("ev") == "live" and r.get("sid") == sid and
                  r.get("state") == "relay", what="the pause")
    note = b.stream.wait(lambda r: r.get("ev") == "note" and r.get("sid") == sid,
                         what="its note")
    t.check("no connected computer" in note.get("text", ""), f"note {note}")
    r = b.call("relay.offer", offer=offer(t, model, machine="second"))
    t.check(r.get("thin") == 1, f"relay.offer {r}")
    rlb = Relayer(t, b)
    try:
        end = b.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                            timeout=30, what="turn.end")
        t.check(end.get("why") == "done", f"turn.end {end}")
    finally:
        rlb.stop = True


@scenario
def relay_deadline(t):
    """Paused past the endpoint's slow_retry_budget: the turn ends."""
    thinconfig(t)
    model = t.model("remote_slow")
    a, rla = relayclient(t, model, budget=2)
    rla.stop = True
    a.stream.disconnect()
    b = Client(t)
    b.attach(reconnect=True)
    b.call("hello")
    sid = b.call("new", cwd=t.work, prompt="go")["sid"]
    t0 = time.monotonic()
    end = b.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                        timeout=15, what="turn.end")
    t.check(end.get("why") == "error" and "relayed" in end.get("err", ""), f"turn.end {end}")
    t.check(1.5 < time.monotonic() - t0 < 6, "after its budget")


@scenario
def relay_lost(t):
    """The computer relaying goes in the middle of a response: the turn
    goes on through another, continuing what was cut off."""
    thinconfig(t)
    model = t.model("remote_slow")
    a, rla = relayclient(t, model, machine="first")
    b, rlb = relayclient(t, model, machine="second")  # the newest: chosen
    try:
        sid = a.call("new", cwd=t.work, prompt="go")["sid"]
        a.stream.wait(lambda r: r.get("ev") == "delta" and r.get("sid") == sid,
                      what="output")
        t.check(not rla.calls or all(h["method"] == "GET" for h, _ in rla.calls),
                "the newest relays")
        rlb.stop = True
        b.stream.disconnect()
        end = a.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                            timeout=40, what="turn.end")
        t.check(end.get("why") == "done", f"turn.end {end}")
        t.check(any(h["method"] == "POST" for h, _ in rla.calls), "the other took over")
        nudged = [x for x in a.all() if x.get("ev") == "nudge" and x.get("sid") == sid]
        t.check(nudged, "a continue after the interruption")
    finally:
        rla.stop = rlb.stop = True


@scenario
def relay_big(t):
    """A body of many parts goes one part at a time, each as the host makes
    room for it, through a client whose link holds little (test hooks
    shrink parts and the link)."""
    thinconfig(t)
    model = t.model("remote_tool")
    c = Client(t)
    c.attach(env=dict(t.env, PROSPERO_TEST_RELAY_PART="2048",
                      PROSPERO_TEST_KEEPMAX=str(256 << 10)))
    c.call("hello")
    r = c.call("relay.offer", offer=offer(t, model))
    t.check(r.get("thin") == 1, f"relay.offer {r}")
    rl = Relayer(t, c)
    try:
        sid = c.call("new", cwd=t.work, prompt="go " * 3000)["sid"]
        end = c.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                            timeout=40, what="turn.end")
        t.check(end.get("why") == "done", f"turn.end {end}")
        posts = [h for h, b in rl.calls if h["method"] == "POST"]
        t.check(posts and posts[0]["parts"] > 2,
                f"parts: {[(h['parts'], h['len'], h['size'], h.get('zlen')) for h in posts]}")
        q = fake_provider.STATE.log(model)
        t.check(q and "go go go" in json.dumps(q[0]["body"]), "the prompt arrived whole")
    finally:
        rl.stop = True


@scenario
def relay_queued(t):
    """The call waits in line at the computer relaying it: no watchdog runs
    meanwhile, live tells the place, and it goes on once sent."""
    thinconfig(t)
    model = t.model("remote_slow")
    c, rl = relayclient(t, model)
    rl.mute = True
    try:
        sid = c.call("new", cwd=t.work, prompt="go")["sid"]
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not any(h["method"] == "POST" for h, _ in rl.calls):
            time.sleep(0.05)
        head = [h for h, _ in rl.calls if h["method"] == "POST"][0]
        rid = [r for r in c.all() if r.get("ev") == "relay" and r.get("part") == 0
               and r.get("method") == "POST"][0]["rid"]
        rl.send({"rid": rid, "queued": {"pos": 2, "len": 3}})
        live = c.stream.wait(lambda r: r.get("ev") == "live" and r.get("sid") == sid
                             and r.get("queue_pos") == 2, what="the place in line")
        t.check(live.get("queue_len") == 3 and live.get("relay") == "testbox", f"live {live}")
        time.sleep(6)  # past first_byte_timeout
        t.check(not [r for r in c.all() if r.get("ev") == "retry" and r.get("sid") == sid],
                "no watchdog fired while queued")
        rl.send({"rid": rid, "sent": 1})
        rl.mute = False
        body = [b for h, b in rl.calls if h["rid"] == rid][0]
        threading.Thread(target=rl.make, args=(rid, dict(head, base=None, zlen=None), body),
                         daemon=True).start()
        end = c.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                            timeout=30, what="turn.end")
        t.check(end.get("why") == "done", f"turn.end {end}")
    finally:
        rl.stop = True


@scenario
def relay_restart(t):
    """A thin host started again, with no offers yet: a session on a
    relayed endpoint keeps it, pauses, and goes on once one is offered."""
    thinconfig(t)
    model = t.model("remote_slow")
    c, rl = relayclient(t, model)
    sid = c.call("new", cwd=t.work, prompt="go")["sid"]
    c.stream.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                  timeout=30, what="the first turn")
    rl.stop = True
    t.check(c.call("quit").get("ok") == 1, "quit")
    c.stream.close()
    time.sleep(1)
    d = Client(t)
    d.attach()
    d.call("hello")
    t.check(d.call("open", sid=sid).get("ok") == 1, "open")
    d.call("prompt", sid=sid, text="again")
    d.stream.wait(lambda r: r.get("ev") == "live" and r.get("sid") == sid and
                  r.get("state") == "relay", what="the pause")
    r = d.call("relay.offer", offer=offer(t, model))
    t.check(r.get("thin") == 1, f"relay.offer {r}")
    rl2 = Relayer(t, d)
    try:
        ends = lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and len([r for r in d.all() if ends(r)]) < 2:
            time.sleep(0.1)
        end = [r for r in d.all() if ends(r)][-1]
        t.check(end.get("why") == "done", f"turn.end {end}")
        att = [r for r in d.all() if r.get("ev") == "attempt" and r.get("sid") == sid]
        t.check(att and att[-1]["endpoint"] == "relay://fake", f"attempts {att[-1:]}")
    finally:
        rl2.stop = True


def varint(v):
    out = bytearray()
    while True:
        c = v & 0x7f
        v >>= 7
        out.append(c | (0x80 if v else 0))
        if not v:
            return bytes(out)


def simpledelta(base, body):
    """Ops that copy the common prefix and add the rest."""
    p = 0
    while p < min(len(base), len(body)) and base[p] == body[p]:
        p += 1
    ops = varint(p << 1 | 1) + varint(0) if p else b""
    if len(body) > p:
        ops += varint((len(body) - p) << 1) + body[p:]
    return ops


class Served:
    """relay.call, as a client passes a thin engine's call on to its own
    engine, and the relay.data records that answer it."""

    def __init__(self, t, c):
        self.t, self.c = t, c
        self.next = 0

    def call(self, body, path="v1/chat/completions", base=None, keep=1,
             endpoint="fake", model=None, session="s1", deflate=False, parts=1):
        self.next += 1
        cid = self.next
        data = body if base is None else simpledelta(base, body)
        size = len(data)
        z = None
        if deflate:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            z = co.compress(data) + co.flush()
            data = z
        n = (len(data) + parts - 1) // parts if parts > 1 else len(data)
        pieces = [data[i:i + n] for i in range(0, len(data), n)] if parts > 1 else [data]
        for k, piece in enumerate(pieces):
            obj = {"cmd": "relay.call", "cid": cid, "origin": "thin", "part": k,
                   "parts": len(pieces), "data": base64.b64encode(piece).decode()}
            if k == 0:
                obj.update(session=session, endpoint=endpoint, anthropic=0, path=path,
                           method="POST", accept="text/event-stream", model=model or "",
                           readms=0, keep=keep, len=len(body), size=size)
                if z is not None:
                    obj["zlen"] = len(z)
                if base is not None:
                    obj["base"] = "%016x" % fnv(base)
            self.c.stream.command(obj)
        return cid

    def records(self, cid):
        return [r for r in self.c.all() if r.get("ev") == "relay.data" and r.get("cid") == cid]

    def done(self, cid, timeout=20):
        r = self.c.stream.wait(lambda r: r.get("ev") == "relay.data" and r.get("cid") == cid
                               and "done" in r, timeout, f"call {cid}'s end")
        return r["done"]

    def text(self, cid):
        body = b"".join(base64.b64decode(r["body"]) for r in self.records(cid) if "body" in r)
        return body.decode("utf-8", "replace")


def chatbody(model, text):
    return json.dumps({"model": model, "stream": True,
                       "messages": [{"role": "user", "content": text}]}).encode()


@scenario
def relay_serve(t):
    """An engine makes a thin engine's calls at its own endpoint: whole,
    as a delta against its last body for the session, deflated, in
    parts; a delta without its base is refused as nobase, and calls
    outside the API's, or to endpoints it lacks, are refused."""
    c = Client(t)
    c.attach()
    c.call("hello")
    sv = Served(t, c)
    model = t.model("remote_slow")
    b1 = chatbody(model, "first")
    a = sv.call(b1, model=model)
    d = sv.done(a)
    t.check(d["err"] == "ok", f"whole: {d}")
    heads = [r for r in sv.records(a) if "head" in r]
    t.check(heads and heads[0]["head"]["status"] == 200, f"head {heads}")
    t.check("w39" in sv.text(a), "the response")
    cid = c.alllines() and c.all()[-1].get("origin")
    t.check(cid == "thin", f"its origin as given: {cid!r}")

    b2 = chatbody(model, "first, then more")
    d = sv.done(sv.call(b2, base=b1, model=model, deflate=True))
    t.check(d["err"] == "ok", f"delta: {d}")
    q = fake_provider.STATE.log(model)
    t.check(q and q[-1]["body"]["messages"][0]["content"] == "first, then more",
            "the delta made the body")

    big = chatbody(model, "x" * 1500000)
    d = sv.done(sv.call(big, model=model, parts=3))
    t.check(d["err"] == "ok", f"parts: {d}")

    d = sv.done(sv.call(b2, base=b"something else", model=model))
    t.check(d["err"] == "nobase", f"no base: {d}")
    d = sv.done(sv.call(b2, path="v1/embeddings", model=model))
    t.check(d["err"] == "connect" and "may make" in d["text"], f"path: {d}")
    d = sv.done(sv.call(b2, endpoint="nowhere", model=model))
    t.check(d["err"] == "connect" and "nowhere" in d["text"], f"endpoint: {d}")

    # Cancelled while it streams
    e = sv.call(chatbody(model, "slow"), model=model)
    c.stream.wait(lambda r: r.get("ev") == "relay.data" and r.get("cid") == e
                  and "body" in r, what="a body")
    c.stream.command({"cmd": "relay.call", "cid": e, "origin": "thin", "cancel": 1})
    d = sv.done(e)
    t.check(d["err"] == "cancel", f"cancel: {d}")


@scenario
def relay_offer(t):
    """offer: this engine's endpoints, with the settings a thin engine
    needs and never a URL or key, its models flattened through like."""
    with open(t.config, "a") as f:
        f.write("key = sk-secret-1234\n[model base]\ncontext = 1000\n"
                "template.enable_thinking = false\n"
                "[model kid]\nlike = base\nreasoning_budget = 50\nmax_concurrent = 3\n")
    c = Client(t)
    c.attach()
    c.call("hello")
    r = c.call("offer")
    t.check(r.get("ok") == 1, f"offer {r}")
    o = r["offer"]
    text = json.dumps(o)
    t.check("sk-secret" not in text and t.args.url.split("//")[1] not in text,
            "no key, no URL")
    eps = o["endpoints"]
    t.check(len(eps) == 1 and eps[0]["name"] == "fake" and eps[0]["kind"] == "openai"
            and eps[0]["first_byte_timeout"] == "5", f"endpoints {eps}")
    kid = [m for m in o["models"] if m["name"] == "kid"][0]["settings"]
    t.check(kid.get("context") == "1000" and kid.get("reasoning_budget") == "50" and
            kid.get("template.enable_thinking") == "false" and "max_concurrent" not in kid
            and "like" not in kid, f"kid {kid}")
    t.check(o.get("endpoint") == "fake", f"default {o.get('endpoint')}")


@scenario
def relay_far(t):
    """A subagent on a thin host runs on its parent's models: the link
    offers them for its own subagents alone, and makes their calls. A
    session there that the link did not make pauses."""
    thinconfig(t)
    f = Far(t, "relay_far")
    try:
        sid = f.parent()
        end = f.ends(sid)
        t.check(end.get("why") == "done", f"turn end {end}")
        te = [r for r in f.x.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check([report(r.get("text", "")) for r in te] == ["Done (ssh): First job.",
                                                           "Done (ssh): Second job."],
                f"reports {[r.get('text') for r in te]}")
        kids = f.ykids()
        t.check(len(kids) == 2 and all(len(k.get("relay", "")) == 16 for k in kids),
                f"y's sessions {kids}")
        sent = fake_provider.STATE.log(fake_provider.second(f.model))
        t.check(len(sent) == 2, f"the subagents' calls, made here: {len(sent)}")

        c = Client(t)
        c.attach(reconnect=True)
        c.call("hello")
        r = c.call("new", cwd=t.work, prompt="go")
        t.check(r.get("ok") == 1, f"new: {r}")
        c.stream.wait(lambda x: x.get("ev") == "live" and x.get("sid") == r["sid"] and
                      x.get("state") == "relay", what="not the link's: paused")
        c.call("cancel", sid=r["sid"])
    finally:
        f.quit()


def lanaddress(host):
    """This machine's address on the network that reaches host."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((socket.gethostbyname(host), 9))
        return s.getsockname()[0]
    finally:
        s.close()


def remote_sh(a, script):
    if a.shell:
        return subprocess.run(["ssh", a.ssh, a.shell], input=script,
                              capture_output=True, text=True)
    if a.windows:
        return subprocess.run(["ssh", a.ssh, "w64devkit\\bin\\sh.exe -l -s"],
                              input="export PATH=\"$HOME/w64devkit/bin:$PATH\"\n" + script,
                              capture_output=True, text=True)
    return subprocess.run(["ssh", a.ssh, "sh -s"], input=script, capture_output=True,
                          text=True)


def remote_write(a, path, text):
    remote_sh(a, f"cat > {path} <<'EOF'\n{text}EOF\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prospero", required=True)
    ap.add_argument("--ssh", help="a host for the ssh scenarios")
    ap.add_argument("--remote-prospero", help="the harness built there")
    ap.add_argument("--remote-root", help="a directory of ours there")
    ap.add_argument("--windows", action="store_true", help="the ssh host is Windows")
    ap.add_argument("--shell", help="the ssh host's shell for scripts (default: sh -s)")
    ap.add_argument("--no-tunnel", action="store_true",
                    help="the ssh host cannot forward a port back")
    ap.add_argument("--shim", help="Windows: the program of tests/shim_win32.c")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()

    # Without a tunnel the provider listens where the ssh host reaches
    # this machine, its LAN address
    lan = args.ssh and args.no_tunnel
    srv, url = fake_provider.start(lanaddress(args.ssh) if lan else "127.0.0.1")
    args.url = url
    tunnel = None
    if args.ssh:
        args.remote_url = url
        args.remote_work = f"{args.remote_root}/work"
        remote_sh(args, f"mkdir -p {args.remote_work}")
    if args.ssh and not args.no_tunnel:
        port = srv.server_address[1]
        rport = 21000 + port % 20000
        tunnel = subprocess.Popen(["ssh", "-N", "-o", "ExitOnForwardFailure=yes",
                                   "-R", f"{rport}:127.0.0.1:{port}", args.ssh])
        args.remote_url = f"http://127.0.0.1:{rport}"
        time.sleep(2)
    names = args.names or list(SCENARIOS)
    failed = 0
    try:
        for name in names:
            t = T(args, name)
            t0 = time.monotonic()
            try:
                r = SCENARIOS[name](t)
                status, why = ("skip", "") if r == "skip" else ("ok", "")
            except Failure as e:
                status, why = "FAIL", str(e)
            except Exception:
                status, why = "FAIL", traceback.format_exc()
            finally:
                t.cleanup()
            failed += status == "FAIL"
            print(f"{status:4} {name:20} {time.monotonic() - t0:6.1f} s  "
                  f"{why.splitlines()[0] if why else ''}", flush=True)
            if args.v and status == "FAIL":
                print("     " + why.replace("\n", "\n     "))
    finally:
        fake_provider.STATE.stop.set()
        if tunnel:
            tunnel.terminate()
        srv.shutdown()
    print(f"{len(names) - failed}/{len(names)} remote scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
