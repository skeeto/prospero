#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""URL includes: the harness's configuration against a small fake web server.

Each case runs "harness --headless" with a fresh state directory and a
configuration that includes pages of the server below, then reads what
the harness made of it through config.get, with {refresh:1} where a case
needs the fetches done: diagnostics, settings with their provenance,
and the includes with their fetch times. The server logs every request,
so a case also checks what was asked (conditional or not, how often).

  uv run tests/includes.py --harness build/harness [NAME ...]
  uv run tests/includes.py --harness PATH --host biron.local   (Windows)
  uv run tests/includes.py --harness PATH --xp 192.168.1.106   (XP)

With --host, the server runs here and the harness on the Windows host
over ssh, reaching the server through a reverse tunnel, as in
scenarios.py; PATH is the harness.exe already on that host. With --xp,
the same for Windows XP and the user's own sshd, which forwards no
ports: the server listens on this machine's LAN address, over plain
http, as XP's WinHTTP has no TLS 1.2.
"""

import argparse
import concurrent.futures
import hashlib
import http.server
import json
import os
import queue
import random
import re
import socket
import subprocess
import sys
import threading
import time
import traceback

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenarios import Failure, LocalHost, SshHost, XpHost, lanaddress, settle  # noqa: E402


# The web server

class Web(http.server.ThreadingHTTPServer):
    """Pages by path (with any query), each a dict:
      body      the text (bytes or str)
      etag      sent, and an If-None-Match with it gets 304
      modified  sent as Last-Modified; without an etag, an
                If-Modified-Since with it gets 304
      status    a status other than 200, with a short body
      location  a redirect there, with status (302 by default)
      stall     send the head and half the body, then wait this long
      down      close the connection without a response
    Every request's headers are logged by path, with "_at" the time it
    came (time.monotonic)."""
    daemon_threads = True
    request_queue_size = 128  # cases at once, each fetching several

    def __init__(self, bind="127.0.0.1"):
        super().__init__((bind, 0), Handler)
        self.pages = {}
        self.log = {}
        self.lock = threading.Lock()
        self.stop = threading.Event()

    def page(self, path, **spec):
        with self.lock:
            self.pages[path] = spec

    def change(self, path, **spec):
        with self.lock:
            self.pages[path] = dict(self.pages[path], **spec)

    def requests(self, path):
        with self.lock:
            return list(self.log.get(path, []))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        web = self.server
        with web.lock:
            spec = web.pages.get(self.path)
            web.log.setdefault(self.path, []).append(
                dict(self.headers, _at=time.monotonic()))
        self.close_connection = True
        if spec is None:
            return self.reply(404, b"not found\n")
        if spec.get("down"):
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        if "location" in spec:
            return self.reply(spec.get("status", 302), b"moved\n",
                              {"Location": spec["location"]})
        if spec.get("status", 200) != 200:
            return self.reply(spec["status"], b"no\n")

        head = {}
        etag, mod = spec.get("etag"), spec.get("modified")
        if etag:
            head["ETag"] = etag
        if mod:
            head["Last-Modified"] = mod
        inm = self.headers.get("If-None-Match")
        ims = self.headers.get("If-Modified-Since")
        if (etag and inm == etag) or (not etag and mod and ims == mod):
            return self.reply(304, b"", head)
        body = spec["body"]
        if isinstance(body, str):
            body = body.encode()
        if spec.get("stall"):
            self.send_response(200)
            for k, v in head.items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body[:len(body) // 2])
            self.wfile.flush()
            web.stop.wait(spec["stall"])
            return
        self.reply(200, body, head)

    def reply(self, status, body, head=None):
        self.send_response(status)
        for k, v in (head or {}).items():
            self.send_header(k, v)
        if status != 304:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if status != 304:
            self.wfile.write(body)


# The harness

def sha256(text):
    if isinstance(text, str):
        text = text.encode()
    return hashlib.sha256(text).hexdigest()


class Engine:
    """harness --headless on a case's configuration and state."""

    def __init__(self, t, env=None):
        self.t0 = time.monotonic()
        self.p = t.host.popen(["--headless", "-c", t.config, "--state", t.state],
                              t.base, stdin=True, env=env)
        self.q = queue.Queue()
        self.id = 0
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.p.stdout:
            self.q.put(line)
        self.q.put(None)

    def call(self, cmd, timeout=60, **fields):
        self.id += 1
        i = self.id
        self.p.stdin.write(json.dumps(dict(cmd=cmd, id=i, **fields)) + "\n")
        self.p.stdin.flush()
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failure(f"no reply to {cmd} in {timeout} s")
            try:
                line = self.q.get(timeout=left)
            except queue.Empty:
                continue
            if line is None:
                raise Failure(f"the harness exited: {self.p.stderr.read()[:2000]}")
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("ev") == "reply" and ev.get("id") == i:
                return ev

    def config(self, refresh=False, timeout=60):
        if refresh:
            return Config(self.call("config.get", timeout, refresh=1))
        return Config(self.call("config.get", timeout))

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise Failure("the harness did not exit")
        err = self.p.stderr.read()
        if "ThreadSanitizer" in err or "AddressSanitizer" in err:
            raise Failure("sanitizer report:\n" + err[:4000])
        return self.p.returncode


class Config:
    """A config.get reply."""

    def __init__(self, reply):
        self.r = reply
        if not reply.get("ok"):
            raise Failure(f"config.get failed: {reply}")

    def diags(self):
        out = []
        for d in self.r["diags"]:
            where = ":".join(str(x) for x in (d["file"], d["line"], d["col"]) if x)
            out.append(f"{where}: {d['severity']}: {d['message']}")
        return out

    def setting(self, kind, name, key):
        for s in self.r["settings"]:
            if (s["kind"], s["name"], s["key"]) == (kind, name, key):
                return s
        raise Failure(f"no setting {kind}/{name}/{key}")

    def include(self, url):
        for i in self.r["includes"]:
            if i["url"] == url:
                return i
        raise Failure(f"no include {url} in {[i['url'] for i in self.r['includes']]}")


class T:
    """One case: its directories, its pages, and checks."""

    def __init__(self, host, web, name):
        self.host = host
        self.web = web
        nonce = f"{random.randrange(1 << 32):08x}"
        self.base = host.join(host.root, f"inc-{name}-{nonce}")
        self.state = host.join(self.base, "state")
        self.config = host.join(self.base, "harness.ini")
        host.mkdir(self.state)
        self.prefix = f"/{name}-{nonce}"
        self.url = host.url + self.prefix

    def page(self, rel, **spec):
        self.web.page(f"{self.prefix}/{rel}", **spec)

    def change(self, rel, **spec):
        self.web.change(f"{self.prefix}/{rel}", **spec)

    def requests(self, rel):
        return self.web.requests(f"{self.prefix}/{rel}")

    def write(self, text):
        self.host.write(self.config, text)

    def start(self, env=None):
        return Engine(self, env)

    def run(self, refresh=False, env=None):
        """Start, read the configuration, and stop."""
        e = self.start(env)
        try:
            return e.config(refresh)
        finally:
            rc = e.close()
            self.check(rc == 0, f"exit {rc}")

    def cachepath(self, url):
        return self.host.join(self.state, "includes", sha256(url))

    def cache(self, url):
        return self.host.read(self.cachepath(url))

    def check(self, cond, msg):
        if not cond:
            raise Failure(msg)

    def same(self, got, want, what):
        if got != want:
            raise Failure(f"{what}:\n  got:  {got!r}\n  want: {want!r}")


CASES = {}


def case(bound=20):
    def deco(fn):
        CASES[fn.__name__] = (fn, bound)
        return fn
    return deco


TEAM = ("model = team-model\n"
        "[endpoint team]\n"
        "url = http://team.example/v1\n"
        "idle_timeout = 45\n")

MODIFIED = "Wed, 21 Oct 2026 07:28:00 GMT"


# Cases

@case()
def fresh(t):
    """No cached copy: fetched at startup, then cached with its
    validators; settings name the URL and the fetch time."""
    t.page("team.ini", body=TEAM, etag='"v1"', modified=MODIFIED)
    url = f"{t.url}/team.ini"
    t.write(f"include = {url}\n")
    before = int(time.time() * 1000) - 60000
    c = t.run()
    t.same(c.diags(), [], "diagnostics")
    s = c.setting("", "", "model")
    t.same((s["value"], s["file"], s["line"]), ("team-model", url, 1), "model")
    s = c.setting("endpoint", "team", "idle_timeout")
    t.same((s["value"], s["file"], s["line"]), ("45", url, 4), "idle_timeout")
    t.check(s["fetched"] > before, f"fetched {s['fetched']}")
    i = c.include(url)
    t.same((i["error"], i["sha256"], i["pin"], i["fetching"]),
           ("", sha256(TEAM), "", 0), "include")
    t.check(url in c.r["files"], f"files {c.r['files']}")
    reqs = t.requests("team.ini")
    t.same(len(reqs), 1, "requests")
    t.check("If-None-Match" not in reqs[0], "unconditional")
    text = t.cache(url)
    t.check(text and text.startswith(f"harness include 1\nurl {url}\nfetched "),
            f"cache {text!r}")
    t.check('\netag "v1"\n' in text and f"\nmodified {MODIFIED}\n" in text,
            f"validators {text!r}")
    t.check(text.endswith("\n\n" + TEAM), "the text")


@case()
def not_modified(t):
    """A cached copy loads at once; refreshes ask with its validators
    (ETag, or Last-Modified alone) and keep it on 304."""
    t.page("e.ini", body="model = by-etag\n", etag='"e1"', modified=MODIFIED)
    t.page("m.ini", body="[ui]\ntheme = dark\n", modified=MODIFIED)
    e_url, m_url = f"{t.url}/e.ini", f"{t.url}/m.ini"
    t.write(f"include = {e_url}\ninclude = {m_url}\n")
    first = t.run()
    t.same(len(t.requests("e.ini")) + len(t.requests("m.ini")), 2, "first run")
    time.sleep(0.05)

    e = t.start()
    try:
        t0 = time.monotonic()
        c = e.config()
        t.check(time.monotonic() - t0 < 5, "the cached copies load at once")
        t.same(c.setting("", "", "model")["value"], "by-etag", "model")
        c = e.config(refresh=True)
    finally:
        t.check(e.close() == 0, "exit")
    t.same(c.diags(), [], "diagnostics")
    t.same(c.setting("ui", "", "theme")["file"], m_url, "theme from")
    for rel, header, value in (("e.ini", "If-None-Match", '"e1"'),
                               ("m.ini", "If-Modified-Since", MODIFIED)):
        reqs = t.requests(rel)[1:]
        t.check(reqs, f"{rel} refreshed")
        for r in reqs:
            t.same(r.get(header), value, f"{rel} {header}")
    for url in (e_url, m_url):
        t.check(c.include(url)["fetched"] > first.include(url)["fetched"],
                f"{url} fetched again")
        t.same(c.include(url)["error"], "", "no error")


@case()
def offline(t):
    """A server gone: the cached copy, with a warning of its age; with no
    copy, the include is skipped."""
    t.page("team.ini", body=TEAM, etag='"v1"')
    url, gone = f"{t.url}/team.ini", f"{t.url}/never.ini"
    t.write(f"include = {url}\n")
    t.run()

    # Make the copy three hours old, then take the server down
    path = t.cachepath(url)
    text = t.cache(url)
    old = int(time.time() * 1000) - 3 * 3600 * 1000 - 60000
    t.host.write(path, re.sub(r"\nfetched \d+\n", f"\nfetched {old}\n", text))
    t.change("team.ini", down=True)
    t.page("never.ini", body=TEAM, down=True)
    t.write(f"include = {url}\ninclude = {gone}\n")
    c = t.run(refresh=True)
    d = c.diags()
    t.same(len(d), 2, f"diagnostics {d}")
    t.check(re.fullmatch(rf".*harness\.ini:1:11: warning: cannot fetch "
                         rf"{re.escape(url)} \(.+\); using the copy fetched "
                         rf"3 hours ago", d[0]), d[0])
    t.check(re.fullmatch(rf".*harness\.ini:2:11: warning: cannot fetch "
                         rf"{re.escape(gone)} \(.+\), include skipped", d[1]), d[1])
    t.same(c.setting("", "", "model")["value"], "team-model", "model")
    t.same(c.include(url)["fetched"], old, "fetched")
    t.check(c.include(url)["error"], "an error")
    t.check(t.cache(gone) is None, "nothing cached")


@case()
def pin(t):
    """A pin: a match is used; a mismatch is an error and not cached; a
    changed text under a pin keeps the copy that matches."""
    evil = "model = evil\n"
    t.page("team.ini", body=TEAM, etag='"v1"')
    t.page("other.ini", body=evil)
    url, other = f"{t.url}/team.ini", f"{t.url}/other.ini"
    zeros = "0" * 64
    t.write(f"include = {url} sha256={sha256(TEAM).upper()}\n"
            f"include = {other}   sha256={zeros}  # wrong\n")
    e = t.start()
    try:
        c = e.config()
        t.same(c.diags(), [f"{t.config}:2:11: error: {other} does not match its "
                           f"pin: its sha256 is {sha256(evil)}, include skipped"],
               "diagnostics")
        t.same(c.setting("", "", "model")["value"], "team-model", "model")
        t.same(c.include(url)["pin"], sha256(TEAM), "pin")
        t.check(t.cache(other) is None, "a mismatch is not cached")

        t.change("team.ini", body=evil, etag='"v2"')
        c = e.config(refresh=True)
    finally:
        t.check(e.close() == 0, "exit")
    d = c.diags()
    t.check(re.fullmatch(rf".*:1:11: error: {re.escape(url)} does not match its pin: "
                         rf"its sha256 is {sha256(evil)}; using the copy fetched "
                         rf"\d+ seconds? ago", d[0]), d)
    t.same(c.setting("", "", "model")["value"], "team-model", "model")
    t.check(t.cache(url).endswith("\n\n" + TEAM), "the copy that matches")


@case()
def redirects(t):
    """Five redirects are followed, relative or not; six are too many;
    https never goes to http."""
    hops = ["/r1", f"{t.url}/r2", "r3", f"//{t.url.split('//')[1]}/r4", "./r5?x"]
    codes = [302, 301, 307, 308, 303]
    for i, (loc, code) in enumerate(zip(hops, codes)):
        if loc.startswith("/") and not loc.startswith("//"):
            loc = t.prefix + loc
        t.page(f"r{i}", location=loc, status=code)
    t.page("r5?x", body="model = redirected\n")
    t.page("s", location=f"{t.prefix}/r0")
    url, six = f"{t.url}/r0", f"{t.url}/s"
    t.write(f"include = {url}\n")
    c = t.run()
    t.same(c.diags(), [], "diagnostics")
    s = c.setting("", "", "model")
    t.same((s["value"], s["file"]), ("redirected", url), "five redirects")
    t.check(t.cache(url), "cached under the URL included")

    t.write(f"include = {six}\n")
    c = t.run()
    t.same(c.diags(), [f"{t.config}:1:11: warning: cannot fetch {six} "
                       f"(more than 5 redirects), include skipped"], "six")
    t.same(len(t.requests("r5?x")), 1, "the sixth not followed")


@case()
def size_cap(t):
    """At most 1 MiB is fetched."""
    t.page("big.ini", body="#" * (1 << 20) + "\n")
    t.page("ok.ini", body="# padding\n" * 50000 + "model = large\n")
    big, ok = f"{t.url}/big.ini", f"{t.url}/ok.ini"
    t.write(f"include = {big}\ninclude = {ok}\n")
    c = t.run()
    t.same(c.diags(), [f"{t.config}:1:11: warning: cannot fetch {big} "
                       f"(larger than 1 MiB), include skipped"], "diagnostics")
    t.same(c.setting("", "", "model")["value"], "large", "model")


# What a fetch cut off at its 30 s deadline reports: the time from its
# start until the engine cut it, to the second. A busy machine moves it
# both ways: at startup the deadline counts from the configuration's
# opening, so a fetch that starts late is cut sooner, and an engine
# that comes to the deadline late cuts later. So it is bounded as the
# cases bound the time itself (dt), 25 to 40 s, which still tells a
# wrong unit or a wrong start from the deadline.
CUTOFF = r"(2[5-9]|3\d) seconds"


@case(bound=50)
def stall_fresh(t):
    """A stalled server with no cached copy holds startup 30 s at most."""
    t.page("slow.ini", body=TEAM, stall=60)
    url = f"{t.url}/slow.ini"
    t.write(f"include = {url}\n")
    e = t.start()
    try:
        c = e.config(timeout=45)
        dt = time.monotonic() - e.t0
    finally:
        t.check(e.close() == 0, "exit")
    t.check(25 < dt < 40, f"startup took {dt:.1f} s")
    d = c.diags()
    t.check(len(d) == 1 and re.fullmatch(
        rf"{re.escape(t.config)}:1:11: warning: cannot fetch {re.escape(url)} "
        rf"\(timed out after {CUTOFF}\), include skipped", d[0]), f"diagnostics: {d}")


@case(bound=50)
def stall_cached(t):
    """A stalled server with a cached copy holds nothing: the copy loads
    at once, and the refresh times out in the background."""
    t.page("slow.ini", body=TEAM)
    url = f"{t.url}/slow.ini"
    t.write(f"include = {url}\n")
    t.run()
    t.change("slow.ini", stall=60)
    e = t.start()
    try:
        c = e.config()
        dt = time.monotonic() - e.t0
        t.check(dt < 5, f"startup took {dt:.1f} s")
        t.same(c.diags(), [], "diagnostics at first")
        t.same(c.setting("", "", "model")["value"], "team-model", "model")
        t.same(c.include(url)["fetching"], 1, "fetching")
        c = e.config(refresh=True, timeout=45)
        dt = time.monotonic() - e.t0
    finally:
        t.check(e.close() == 0, "exit")
    t.check(25 < dt < 40, f"the refresh took {dt:.1f} s")
    d = c.diags()
    t.check(len(d) == 1 and re.fullmatch(
        rf".*:1:11: warning: cannot fetch {re.escape(url)} \(timed out after "
        rf"{CUTOFF}\); using the copy fetched \d+ seconds ago", d[0]), d)


@case()
def relative(t):
    """Includes in a file from the web resolve against its URL, so they
    can name only more of the web; the shared file's secrets are ignored."""
    hostport = t.url.split("//")[1].split("/")[0]
    common = "[model common]\ntemperature = 0.2\n[endpoint c]\nurl = http://c/\nkey = sk-plain\n"
    t.page("team/main.ini", body=(
        "include = base.ini\n"
        f"include = ../shared/common.ini sha256={sha256(common)}\n"
        f"include = {t.prefix}/abs.ini\n"
        f"include = //{hostport}{t.prefix}/net.ini#frag\n"
        "include = /etc/passwd\n"
        "include = ./main.ini\n"
        "[model main]\ntemperature = 0.1\n"))
    t.page("team/base.ini", body="[model base]\ntemperature = 0.3\n")
    t.page("shared/common.ini", body=common)
    t.page("abs.ini", body="[model abs]\ntemperature = 0.4\n")
    t.page("net.ini", body="[model net]\ntemperature = 0.5\n")
    main = f"{t.url}/team/main.ini"
    t.write(f"include = {main}\n")
    c = t.run()
    host = t.url.split(t.prefix)[0]
    d = c.diags()
    want = [
        f"{main}:5:11: warning: cannot fetch {host}/etc/passwd (HTTP 404), "
        f"include skipped",
        f"{main}:6:11: warning: include cycle: {main}",
        f"{t.url}/shared/common.ini:5:7: error: a plaintext secret in an "
        f"included file is ignored: write env:NAME and set the variable",
    ]
    t.same(d, want, "diagnostics")
    for name, value, url, line in (
            ("main", "0.1", main, 8),
            ("base", "0.3", f"{t.url}/team/base.ini", 2),
            ("common", "0.2", f"{t.url}/shared/common.ini", 2),
            ("abs", "0.4", f"{t.url}/abs.ini", 2),
            ("net", "0.5", f"{t.url}/net.ini", 2)):
        s = c.setting("model", name, "temperature")
        t.same((s["value"], s["file"], s["line"]), (value, url, line), name)
    t.same(c.include(f"{t.url}/shared/common.ini")["pin"], sha256(common), "pin")


@case()
def interval(t):
    """Each URL is fetched again at most once an interval (10 minutes,
    here 3 s), when the engine has work, or at once on refresh. That no
    fetch came is checked only while the interval truly had not passed
    when the engine answered, by the time the web server saw the last
    fetch: a busy machine may answer later, and then a fetch is right."""
    every = 3.0
    t.page("team.ini", body=TEAM, etag='"v1"')
    url = f"{t.url}/team.ini"
    t.write(f"include = {url}\n")
    e = t.start(env={"HARNESS_TEST_MINUTE_MS": str(int(every * 100))})

    def quiet(what):
        """The fetches so far, after work for the engine, checked to be
        no more than before if it answered within the interval."""
        last = t.requests("team.ini")[-1]["_at"]
        before = len(t.requests("team.ini"))
        e.config()
        soon = time.monotonic() - last < every
        time.sleep(0.3)
        n = len(t.requests("team.ini"))
        if soon:
            t.same(n, before, what)
        return n

    try:
        deadline = time.monotonic() + 10
        while not t.requests("team.ini") and time.monotonic() < deadline:
            time.sleep(0.05)
        t.same(len(t.requests("team.ini")), 1, "fetched once at startup")
        for _ in range(3):
            quiet("fetched once at startup")
        time.sleep(max(0, t.requests("team.ini")[-1]["_at"] + every + 0.5 - time.monotonic()))
        n = len(t.requests("team.ini"))
        e.config()  # work for the engine: the interval is over
        deadline = time.monotonic() + 10
        while len(t.requests("team.ini")) <= n and time.monotonic() < deadline:
            time.sleep(0.05)
        t.same(len(t.requests("team.ini")), n + 1, "fetched after the interval")
        n = quiet("not again so soon")
        c = e.config(refresh=True)
        t.same(len(t.requests("team.ini")), n + 1, "forced")
        t.same(c.diags(), [], "diagnostics")
    finally:
        t.check(e.close() == 0, "exit")
    for r in t.requests("team.ini")[1:]:
        t.same(r.get("If-None-Match"), '"v1"', "conditional")


@case()
def remote_shared(t):
    """An included file's [remote] section chooses nothing: what runs and
    where (ssh, path, state, config, deploy) counts only from the user's
    own file, and each such line of the include is an error."""
    lines = [("ssh", "evil.example"), ("path", "-oProxyCommand=/usr/bin/touch"),
             ("state", "/tmp/x"), ("config", "/tmp/y.ini"), ("deploy", "auto")]
    t.page("team.ini", body="[remote team]\n" + "".join(f"{k} = {v}\n" for k, v in lines))
    url = f"{t.url}/team.ini"
    t.write(f"include = {url}\n[remote team]\nssh = example.invalid\n")
    c = t.run()
    why = ("a [remote] setting in an included file is ignored, since it chooses "
           "what runs where: set it in your own file")
    t.same(c.diags(), [f"{url}:{i+2}:{len(k)+4}: error: {why}" for i, (k, _) in
                       enumerate(lines)], "diagnostics")
    s = c.setting("remote", "team", "ssh")
    t.same((s["value"], s["file"], s["valid"]), ("example.invalid", t.config, 1), "ssh")
    for k, _ in lines[1:]:
        s = c.setting("remote", "team", k)
        t.same((s["file"], s["valid"]), (url, 0), k)


@case(bound=40)
def serve_nowait(t):
    """A host never waits for a URL include with no cached copy: with its
    server stalled, a host started by connect --local answers at once, as
    the one host process, which fetches it in the background."""
    t.page("slow.ini", body=TEAM, stall=60)
    url = f"{t.url}/slow.ini"
    t.write(f"include = {url}\n")
    p = t.host.popen(["connect", "--local", "-c", t.config, "--state", t.state,
                      "--idle", "60"], t.base, stdin=True)
    q = queue.Queue()

    def read():
        for line in p.stdout:
            try:
                q.put(json.loads(line))
            except ValueError:
                pass
        q.put(None)
    threading.Thread(target=read, daemon=True).start()

    def wait(pred, timeout, what):
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Failure(f"no {what} in {timeout} s")
            try:
                r = q.get(timeout=left)
            except queue.Empty:
                continue
            if r is None:
                raise Failure(f"connect ended: {p.stderr.read()[:1000]}")
            if r.get("ev") == "remote" and r.get("state") == "down":
                raise Failure(f"down: {r.get('msg')}")
            if pred(r):
                return r

    t0 = time.monotonic()
    try:
        wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", 15, "up")
        dt = time.monotonic() - t0
        t.check(dt < 8, f"up after {dt:.1f} s")
        p.stdin.write('{"cmd":"config.get","id":1}\n')
        p.stdin.flush()
        r = wait(lambda r: r.get("ev") == "reply" and r.get("id") == 1, 15, "config.get")
        i = [x for x in r.get("includes", []) if x["url"] == url]
        t.check(i and i[0]["fetching"] == 1 and not i[0]["fetched"], f"include {i}")
        p.stdin.write('{"cmd":"quit","id":2}\n')
        p.stdin.flush()
        wait(lambda r: r.get("ev") == "reply" and r.get("id") == 2, 15, "quit")
    finally:
        try:
            p.stdin.close()
        except OSError:
            pass
        try:
            p.wait(30)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    t.same(len(t.requests("slow.ini")), 1, "one fetch, by the one host")


def runone(host, web, name):
    fn, bound = CASES[name]
    t0 = time.monotonic()
    try:
        fn(T(host, web, name))
        dt = time.monotonic() - t0
        if dt > bound:
            return name, "FAIL", dt, f"took {dt:.1f} s, bound {bound} s"
        return name, "ok", dt, ""
    except Failure as e:
        return name, "FAIL", time.monotonic() - t0, str(e)
    except Exception:
        return name, "FAIL", time.monotonic() - t0, traceback.format_exc()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True, help="the harness binary")
    ap.add_argument("--host", help="run the harness on this Windows host over ssh")
    ap.add_argument("--xp", help="run the harness on this Windows XP host over ssh")
    ap.add_argument("-j", type=int, default=0, help="cases at once")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()

    names = args.names or list(CASES)
    for n in names:
        if n not in CASES:
            print(f"unknown case {n}; known: {' '.join(CASES)}")
            return 2
    settle()
    lan = lanaddress(args.xp) if args.xp else None
    web = Web(lan or "127.0.0.1")
    threading.Thread(target=web.serve_forever, daemon=True).start()
    port = web.server_address[1]
    if args.xp:
        host = XpHost(args.xp, args.harness, f"http://{lan}:{port}")
    elif args.host:
        host = SshHost(args.host, args.harness, port, 20000 + port % 20000)
    else:
        host = LocalHost(args.harness, f"http://127.0.0.1:{port}")
    try:
        failed = 0
        with concurrent.futures.ThreadPoolExecutor(args.j or len(names)) as ex:
            futs = [ex.submit(runone, host, web, n) for n in names]
            for f in futs:
                name, status, dt, why = f.result()
                print(f"{status:4} {name:14} {dt:6.1f} s  "
                      f"{why.splitlines()[0] if why else ''}")
                if status == "FAIL" and "\n" in why:  # whole, for a ctest log
                    print("     " + why.replace("\n", "\n     "))
                failed += status == "FAIL"
        print(f"{len(names) - failed}/{len(names)} cases passed")
        return 1 if failed else 0
    finally:
        web.stop.set()
        host.close()
        web.shutdown()


if __name__ == "__main__":
    sys.exit(main())
