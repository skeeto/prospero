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
import hashlib
import json
import os
import random
import shlex
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
        self.env = dict(os.environ)
        self.procs = []
        self.write_config(args.url)

    def write_config(self, url, extra=""):
        with open(self.config, "w") as f:
            f.write(f"endpoint = fake\nmodel = fake:remote_slow:{self.nonce}\n"
                    f"[endpoint fake]\nurl = {url}/\nfirst_byte_timeout = 5\n"
                    f"idle_timeout = 5\nprogress_timeout = 10\nconnect_timeout = 2\n"
                    + extra)

    def model(self, scenario):
        return f"fake:{scenario}:{self.nonce}"

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
        cmd = psfield(pid, "command")
        return (psfield(pid, "uid") == str(os.getuid()) and " serve " in cmd and
                os.path.realpath(self.state) in cmd)

    def cleanup(self):
        for pid in self.hostpids():
            try:
                signal_ours(pid, signal.SIGKILL, self.ownhost, "host")
            except (Failure, OSError) as e:
                print(f"     cleanup: {e}", flush=True)
        for p in self.procs:
            try:
                p.kill()
            except OSError:
                pass
        shutil.rmtree(self.root, ignore_errors=True)

    def log(self, sid):
        path = os.path.join(self.state, "sessions", sid, "events.jsonl")
        with open(path) as f:
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


psfield = P.psfield


def grandchild(pid, root):
    """Whether root is the parent of pid's parent: an ssh that prospero
    connect (root, a process of ours) started under its reaper."""
    try:
        parent = int(psfield(pid, "ppid"))
        return parent > 1 and int(psfield(parent, "ppid")) == root
    except (ValueError, P.Refused):
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
    signal_ours(pids[0], signal.SIGKILL, t.ownhost, "host")
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
    with open(os.path.join(t.state, "run", "host.log")) as f:
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


@scenario
def quit_signals(t):
    """prospero serve asked to quit by a signal (SIGTERM, SIGINT, SIGHUP;
    on Windows Ctrl+Break at its console) quits as a client's quit does:
    exit 0, its registration and endpoint gone."""
    win = os.name == "nt"
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
        with open(os.path.join(t.work, name), "w") as f:
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
            with open(os.path.join(t.state, "run", "host.log")) as f:
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


@scenario
def connect_local(t):
    """The client library against this machine's host: a turn, then the
    host restarted under it: the session comes back from its log."""
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "turn.end", what="turn.end")
    lst = c.call("list")
    ss = [r for r in c.records if r.get("ev") == "sessions"]
    t.check(ss and ss[-1].get("host") == "local", f"host in the list: {ss[-1:]}")

    # The host goes away hard: a new one starts, and the session reopens
    pid = t.hostpids()[0]
    signal_ours(pid, signal.SIGKILL, t.ownhost, "host")
    t.check(gone(pid), "killed")
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "down", what="down")
    n = len([r for r in c.records if r.get("ev") == "remote" and r.get("state") == "up"])
    held = c.send("list")        # while down: it waits for the new host
    c.wait(lambda r: len([x for x in c.records if x.get("ev") == "remote"
                          and x.get("state") == "up"]) > n, what="up again", timeout=40)
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


@scenario
def remote_option(t):
    """A remote's path that ssh would take for an option: ignored from an
    included file, and from the user's own quoted, after --, so that a
    real ssh (to a name that never resolves) runs nothing here."""
    local = os.path.join(t.root, "local")
    inc = os.path.join(t.root, "team.ini")
    with open(inc, "w") as f:
        f.write("[remote team]\npath = -oProxyCommand=/usr/bin/touch\n")
    for own in ("", "path = -oProxyCommand=/usr/bin/touch\n"):
        with open(t.config, "w") as f:
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
    t.write_config(t.args.url, "[guard]\nask_timeout = 60\n")
    c = connect(t, ["--local", "--state", t.state, "-c", t.config])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, model=t.model("ask"))
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    pid = t.hostpids()[0]
    signal_ours(pid, signal.SIGKILL, t.ownhost, "host")
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
    os.symlink(t.config, os.path.join(sdir, "out", "link.ini"))
    for bad in ("../../prospero.ini", "/etc/hosts", "out/../../" + sid + "/events.jsonl",
                "out/link.ini", "", "C:x", "out\\..\\..\\x"):
        x = c.call("file", sid=sid, path=bad)
        t.check(x.get("ok") == 0, f"refused {bad!r}: {x}")
    e = c.call("file", sid=sid, path="events.jsonl", max=64)
    t.check(e.get("ok") == 1 and e.get("more") == 1 and e.get("off") == 64 and
            e.get("text", "").startswith('{"sid":"' + sid), f"its log, in part: {e}")
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


def fakessh(t):
    """An ssh that runs the command here, recording its pid."""
    path = os.path.join(t.root, "fakessh")
    pids = os.path.join(t.root, "ssh.pids")
    with open(path, "w") as f:
        f.write("#!/bin/sh\n"
                f"echo $$ >> {shlex.quote(pids)}\n"
                "while [ $# -gt 1 ]; do\n"
                "  case \"$1\" in -o) shift 2;; -*) shift;; *) shift; break;; esac\n"
                "done\n"
                "exec sh -c \"$1\"\n")
    os.chmod(path, 0o755)
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
    signal_ours(pids[-1], signal.SIGKILL, lambda p: grandchild(p, c.p.pid), "ssh")
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
    path = os.path.join(t.root, "slowssh")
    count = os.path.join(t.root, "ssh.count")
    with open(path, "w") as f:
        f.write("#!/bin/sh\n"
                f"n=$(cat {shlex.quote(count)} 2>/dev/null || echo 0)\n"
                "n=$((n+1))\n"
                f"echo $n > {shlex.quote(count)}\n"
                "while [ $# -gt 1 ]; do\n"
                "  case \"$1\" in -o) shift 2;; -*) shift;; *) shift; break;; esac\n"
                "done\n"
                f"if [ $n -eq {slow} ]; then sleep {delay}; fi\n"
                "exec sh -c \"$1\"\n")
    os.chmod(path, 0o755)
    return path


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
    prog = os.path.join(t.root, "slowssh")
    with open(prog, "w") as f:
        f.write(f"#!{sys.executable}\n" + THROTTLE.replace("RATE", "40000"))
    os.chmod(prog, 0o755)
    c = connect(t, ["--ssh", "here", "--ssh-program", prog, "--path", t.prospero,
                    "--remote-state", t.state, "--remote-config", t.config,
                    "--state", os.path.join(t.root, "local")])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", attended=1)
    sid = r["sid"]
    c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid, what="turn.end")
    line = "x" * 99 + "\n"
    size = (1 << 20) - 4096
    with open(os.path.join(t.state, "sessions", sid, "big.txt"), "w") as f:
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
    prog = os.path.join(t.root, "sshwrap")
    pidfile = os.path.join(t.root, "ssh.pids")
    with open(prog, "w") as f:
        f.write(f"#!/bin/sh\necho $$ >> {shlex.quote(pidfile)}\nexec ssh \"$@\"\n")
    os.chmod(prog, 0o755)
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
        signal_ours(sshpids(pidfile)[-1], signal.SIGKILL,
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
    prog = os.path.join(t.root, "sshwrap")
    pidfile = os.path.join(t.root, "ssh.pids")
    with open(prog, "w") as f:
        f.write(f"#!/bin/sh\necho $$ >> {shlex.quote(pidfile)}\nexec ssh \"$@\"\n")
    os.chmod(prog, 0o755)
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
        signal_ours(sshpids(pidfile)[-1], signal.SIGKILL,
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
