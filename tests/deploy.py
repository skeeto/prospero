#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine deployment: harness deploy, and connect deploying on its own.

The local scenarios run the real binary through an ssh and scp of their
own that work here, in a fresh fake home directory, with an engines
directory holding a copy of the binary under its deployed name: deploy
uploads, verifies and records it, tries again when the upload breaks or
arrives damaged, refuses another build of the same version without
--force, and connect then runs the engine deployed, or deploys one
itself (--deploy, or [remote NAME] deploy = auto) when it finds none.

  uv run tests/deploy.py --harness build/harness [NAME ...]

With --ssh the "real" scenario deploys to that host the engine for its
platform from the engines directory next to the binary (build/engines,
from tools/engines.py), deploys again (nothing to upload), then connects
to it with the fake provider reached as in tests/remote.py, and runs a
turn with a shell command in it. Engines it put in ~/.harness there, and
~/.harness itself if it made it, are removed at the end:

  uv run tests/deploy.py --harness build/harness --ssh rat.local \\
      --remote-root /tmp/harness-m5-deploy real
  uv run tests/deploy.py --harness build/harness --ssh biron.local --windows \\
      --remote-root C:/Users/wellons/AppData/Local/Temp/harness-m5-deploy real
  uv run tests/deploy.py --harness build/harness --ssh 192.168.1.106 \\
      --no-tunnel --shell "sh -s" \\
      --remote-root C:/DOCUME~1/wellons/LOCALS~1/Temp/harness-m5-deploy real
"""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402
import remote as R  # noqa: E402
from remote import Failure  # noqa: E402

SCENARIOS = {}


def scenario(fn):
    SCENARIOS[fn.__name__] = fn
    return fn


FAKESSH = r"""#!/bin/sh
# ssh here: the command runs in $FAKEHOME, as a remote shell's would in
# the home directory; FAKEUNAME makes the probe's uname say otherwise,
# and FAKEPATH is the only PATH there
echo "$*" >> "$FAKELOG/ssh.log"
while [ $# -gt 1 ]; do
  case "$1" in -o) shift 2;; -*) shift;; *) shift; break;; esac
done
if [ -n "$FAKEUNAME" ] && [ "${1#uname}" != "$1" ]; then
  printf '%s\n%%OS%%\n' "$FAKEUNAME"
  exit 0
fi
if [ -n "$FAKEPATH" ]; then
  PATH="$FAKEPATH"
  export PATH
fi
cd "$FAKEHOME" && exec /bin/sh -c "$1"
"""

FAKESCP = r"""#!/bin/sh
# scp here, into $FAKEHOME: the first $FAKESCP_FAIL calls fail as a
# dropped link would, and the first $FAKESCP_CORRUPT arrive damaged
echo "$*" >> "$FAKELOG/scp.log"
n=$(cat "$FAKELOG/scp.count" 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" > "$FAKELOG/scp.count"
if [ "$n" -le "${FAKESCP_FAIL:-0}" ]; then
  echo "Connection closed by 10.0.0.1 port 22" >&2
  exit 255
fi
while [ $# -gt 2 ]; do
  case "$1" in -o) shift 2;; -*) shift;; *) break;; esac
done
dst="$FAKEHOME/${2#*:}"
cp "$1" "$dst" || exit 1
if [ "$n" -le "${FAKESCP_CORRUPT:-0}" ]; then
  printf 'x' >> "$dst"
fi
"""


def localplatform():
    sysname, mach = os.uname().sysname, os.uname().machine
    osname = {"Darwin": "macos", "Linux": "linux"}[sysname]
    arch = {"arm64": "arm64" if osname == "macos" else "aarch64",
            "aarch64": "aarch64", "x86_64": "x86_64"}[mach]
    return f"{osname}-{arch}"


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class D(R.T):
    """A scenario with a fake home, a fake ssh and scp, and engines."""

    def __init__(self, args, name):
        super().__init__(args, name)
        self.home = os.path.join(self.root, "home")
        self.local = os.path.join(self.root, "local")
        self.engines = os.path.join(self.root, "engines")
        self.fakelog = os.path.join(self.root, "log")
        for d in (self.home, self.local, self.engines, self.fakelog):
            os.makedirs(d)
        self.ssh = os.path.join(self.root, "fakessh")
        self.scp = os.path.join(self.root, "fakescp")
        for path, text in ((self.ssh, FAKESSH), (self.scp, FAKESCP)):
            with open(path, "w") as f:
                f.write(text)
            os.chmod(path, 0o755)
        out = subprocess.run([self.harness, "--version"], capture_output=True,
                             text=True).stdout.split()
        self.version = out[1]
        self.platform = localplatform()
        self.ename = f"harness-engine-{self.version}-{self.platform}"
        self.rname = f"engine-0-{self.version}-{self.platform}"
        self.env = dict(self.env, FAKEHOME=self.home, FAKELOG=self.fakelog)
        # No harness on the remote shell's PATH, as on a new machine
        self.env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"

    def engine(self, extra=b""):
        """An engine of the local platform in the engines directory: this
        binary, perhaps with bytes after it (another build)."""
        dst = os.path.join(self.engines, self.ename)
        with open(self.harness, "rb") as f:
            data = f.read() + extra
        with open(dst, "wb") as f:
            f.write(data)
        os.chmod(dst, 0o755)
        with open(dst + ".sha256", "w") as f:
            f.write(f"{hashlib.sha256(data).hexdigest()}  {self.ename}\n")
        return dst

    def deploy(self, *extra, env=None, target="here"):
        argv = [self.harness, "deploy", target, "--ssh-program", self.ssh,
                "--scp-program", self.scp, "--engines", self.engines,
                "--state", self.local] + list(extra)
        return subprocess.run(argv, capture_output=True, text=True, timeout=120,
                              env=env or self.env)

    def deployed(self):
        return os.path.join(self.home, ".harness", self.rname)

    def scps(self):
        try:
            with open(os.path.join(self.fakelog, "scp.count")) as f:
                return int(f.read())
        except OSError:
            return 0

    def sshlog(self):
        try:
            with open(os.path.join(self.fakelog, "ssh.log")) as f:
                return f.read()
        except OSError:
            return ""

    def record(self):
        path = os.path.join(self.local, "deploy", "here.ini")
        out = {}
        with open(path) as f:
            for line in f:
                k, _, v = line.partition("=")
                out[k.strip()] = v.strip()
        return out

    def connect(self, *extra, env=None):
        return R.connect(self, ["--ssh", "here", "--ssh-program", self.ssh,
                                "--scp-program", self.scp, "--engines", self.engines,
                                "--remote-state", self.state, "--remote-config",
                                self.config, "--state", self.local] + list(extra),
                         env=env)

    def turn(self, c):
        """A turn with a shell command in it, through a connect."""
        r = c.call("new", cwd=self.work, prompt="go", model=self.model("remote_tool"),
                   attended=1)
        self.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        end = c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                     what="turn.end", timeout=60)
        self.check(end.get("why") == "done", f"turn.end {end}")
        tools = [r for r in c.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        self.check(tools and tools[0].get("status") == "ok", f"the command: {tools}")
        return sid


@scenario
def fresh(t):
    """Deploy to a machine without one: uploaded, verified, in place,
    runnable, recorded; then again: nothing to upload."""
    src = t.engine()
    p = t.deploy()
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(p.stdout == f"~/.harness/{t.rname}\n", f"stdout {p.stdout!r}")
    dst = t.deployed()
    t.check(os.path.isfile(dst) and sha256(dst) == sha256(src), "the engine is there")
    t.check(os.stat(dst).st_mode & 0o111, "executable")
    t.check(oct(os.stat(os.path.dirname(dst)).st_mode & 0o777) == "0o700", "~/.harness 0700")
    t.check(not os.path.exists(dst + ".part"), "no .part left")
    rec = t.record()
    t.check(rec.get("host") == "here" and rec.get("platform") == t.platform and
            rec.get("path") == f".harness/{t.rname}" and rec.get("sha256") == sha256(src)
            and rec.get("proto") == "0" and rec.get("version") == t.version,
            f"record {rec}")
    t.check(t.scps() == 1, f"{t.scps()} uploads")
    for step in ("probing here", f"here is {t.platform}", "uploading",
                 "verifying", "deployed: "):
        t.check(step in p.stderr, f"no {step!r} in {p.stderr!r}")
    q = t.deploy()
    t.check(q.returncode == 0 and "already deployed" in q.stderr, f"again: {q.stderr}")
    t.check(t.scps() == 1, "uploaded again")


@scenario
def retry(t):
    """A link that drops twice: the third upload goes through."""
    t.engine()
    p = t.deploy(env=dict(t.env, FAKESCP_FAIL="2"))
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(t.scps() == 3, f"{t.scps()} uploads")
    t.check(p.stderr.count("trying again") == 2 and "Connection closed" in p.stderr,
            f"stderr {p.stderr}")
    t.check("try 3" in p.stderr, "the third try is named")


@scenario
def damaged(t):
    """An upload that arrives damaged is caught by its hash, and sent
    again; one that never arrives whole fails, with nothing in place."""
    src = t.engine()
    p = t.deploy(env=dict(t.env, FAKESCP_CORRUPT="1"))
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check("arrived damaged" in p.stderr and t.scps() == 2, f"stderr {p.stderr}")
    t.check(sha256(t.deployed()) == sha256(src), "the good copy is in place")
    # Always damaged: six tries, then the end, its waits about 67 s
    # (an engine that never arrives need not run: a small one will do)
    os.remove(t.deployed())
    os.remove(os.path.join(t.fakelog, "scp.count"))
    small = b"not an engine\n" * 64
    with open(src, "wb") as f:
        f.write(small)
    with open(src + ".sha256", "w") as f:
        f.write(f"{hashlib.sha256(small).hexdigest()}  {t.ename}\n")
    env = dict(t.env, FAKESCP_CORRUPT="99")
    start = time.monotonic()
    p = t.deploy(env=env)
    t.check(p.returncode == 1 and "arrived damaged" in p.stderr, f"stderr {p.stderr}")
    t.check(t.scps() == 6, f"{t.scps()} uploads")
    t.check(not os.path.exists(t.deployed()), "a damaged engine in place")
    t.check(time.monotonic() - start < 120, "too slow")


@scenario
def other_build(t):
    """Another build of the same version there: refused, then replaced
    with --force while a host runs from it, which keeps running."""
    old = sha256(t.engine(b"old"))
    p = t.deploy()
    t.check(p.returncode == 0, f"first: {p.stderr}")
    # A host of the old build runs from its place
    c = t.connect()
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    pid = t.hostpids()[0]
    new = sha256(t.engine(b"new"))
    p = t.deploy()
    t.check(p.returncode == 1 and "another build" in p.stderr and "--force" in p.stderr,
            f"without --force: {p.stderr}")
    t.check(sha256(t.deployed()) == old, "replaced without --force")
    p = t.deploy("--force")
    t.check(p.returncode == 0, f"--force: {p.stderr}")
    t.check(sha256(t.deployed()) == new, "not replaced")
    t.check(t.record().get("sha256") == new, "record")
    t.check(not R.gone(pid, 1), "the old build's host stopped")
    r = c.call("list")
    t.check(r.get("ok") == 1, f"the old host answers: {r}")
    t.check(c.close() == 0, "connect exits 0")


@scenario
def no_engine(t):
    """A platform the engines directory has nothing for: a clear error."""
    p = t.deploy(env=dict(t.env, FAKEUNAME="Linux sparc64"))
    t.check(p.returncode == 1, f"exit {p.returncode}")
    t.check("no engine for linux-sparc64" in p.stderr and "tools/engines.py" in p.stderr,
            f"stderr {p.stderr}")
    t.check(not os.path.exists(os.path.join(t.local, "deploy", "here.ini")), "recorded")


@scenario
def relative_engines(t):
    """An engines directory given relative to where deploy runs, which is
    not where scp runs."""
    src = t.engine()
    argv = [t.harness, "deploy", "here", "--ssh-program", t.ssh, "--scp-program", t.scp,
            "--engines", "engines", "--state", t.local]
    p = subprocess.run(argv, capture_output=True, text=True, timeout=120, env=t.env,
                       cwd=t.root)
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(sha256(t.deployed()) == sha256(src), "the engine is there")


@scenario
def no_hash(t):
    """No tool there to compute a SHA-256: deploying fails at once, for
    good, before uploading anything."""
    t.engine()
    bindir = os.path.join(t.root, "bin")
    os.makedirs(bindir)
    for tool in ("sh", "uname", "mkdir", "chmod", "mv", "rm", "cat"):
        for d in ("/bin", "/usr/bin"):
            if os.path.exists(os.path.join(d, tool)):
                os.symlink(os.path.join(d, tool), os.path.join(bindir, tool))
                break
    start = time.monotonic()
    p = t.deploy(env=dict(t.env, FAKEPATH=bindir))
    t.check(p.returncode == 1, f"exit {p.returncode}: {p.stderr}")
    t.check("cannot compute a SHA-256 on here: no sha256sum, shasum or openssl"
            in p.stderr, f"stderr {p.stderr}")
    t.check(t.scps() == 0, f"{t.scps()} uploads")
    t.check(time.monotonic() - start < 20, "not at once")


@scenario
def dash_dest(t):
    """An ssh destination that ssh and scp would take for an option, as
    connect refuses it: refused before either runs."""
    t.engine()
    with open(t.config, "a") as f:
        f.write("[remote evil]\nssh = -oProxyCommand=false\n")
    p = t.deploy("-c", t.config, target="evil")
    t.check(p.returncode == 1, f"exit {p.returncode}: {p.stderr}")
    t.check("an ssh destination cannot begin with -" in p.stderr, f"stderr {p.stderr}")
    t.check(not t.sshlog() and t.scps() == 0, "ssh or scp ran")


@scenario
def connect_deployed(t):
    """connect runs the engine deploy put there, and a turn with a shell
    command works through it."""
    t.engine()
    p = t.deploy()
    t.check(p.returncode == 0, f"deploy: {p.stderr}")
    c = t.connect()
    up = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    t.check(up.get("version") == t.version, f"up {up}")
    t.turn(c)
    t.check(f".harness/{t.rname} attach" in t.sshlog(), f"ssh ran {t.sshlog()!r}")
    q = c.call("quit")
    t.check(q.get("ok") == 1, f"quit {q}")
    t.check(c.close() == 0, "connect exits 0")


@scenario
def connect_missing(t):
    """No engine there: connect fails at once, saying deploying would help."""
    c = t.connect()
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=20)
    t.check(f.get("deploy") == "missing", f"failed {f}")
    t.check("harness deploy here" in f.get("msg", ""), f"msg {f}")
    rc = c.p.wait(10)
    t.check(rc == 2, f"exit {rc}")
    t.check(t.sshlog().count(" attach") == 1, "tried more than once")


@scenario
def connect_deploy(t):
    """connect --deploy where there is none: it deploys, then connects."""
    src = t.engine()
    c = t.connect("--deploy")
    held = c.send("list")  # waits for the engine
    dep = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "deploying",
                 what="deploying")
    t.check("no engine on here" in dep.get("msg", ""), f"deploying {dep}")
    done = c.wait(lambda r: r.get("ev") == "deploy" and r.get("step") == "done",
                  what="deployed", timeout=60)
    t.check(done.get("path") == f"~/.harness/{t.rname}" and
            done.get("platform") == t.platform, f"done {done}")
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    h = c.wait(lambda r: r.get("ev") == "reply" and r.get("id") == held, what="held list")
    t.check(h.get("ok") == 1, f"the command held while deploying: {h}")
    steps = [r.get("step") for r in c.records if r.get("ev") == "deploy"]
    t.check(steps[0] == "probe" and steps[-1] == "done" and "upload" in steps,
            f"steps {steps}")
    t.check(sha256(t.deployed()) == sha256(src), "deployed")
    t.turn(c)
    c.call("quit")
    t.check(c.close() == 0, "connect exits 0")


@scenario
def auto_config(t):
    """[remote NAME] deploy = auto deploys as --deploy does, and a failed
    deployment ends the remote with why."""
    t.engine()
    with open(t.config, "a") as f:
        f.write("[remote here]\nssh = here\ndeploy = auto\n")
    c = R.connect(t, ["here", "-c", t.config, "--ssh-program", t.ssh, "--scp-program",
                      t.scp, "--engines", t.engines, "--remote-state", t.state,
                      "--remote-config", t.config, "--state", t.local])
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up",
           timeout=60)
    t.check(os.path.isfile(t.deployed()), "deployed")
    c.call("quit")
    t.check(c.close() == 0, "connect exits 0")
    # Nothing to deploy for this platform: the end, saying why
    shutil.rmtree(os.path.join(t.home, ".harness"))
    os.remove(os.path.join(t.local, "deploy", "here.ini"))
    for n in os.listdir(t.engines):
        os.remove(os.path.join(t.engines, n))
    c = R.connect(t, ["here", "-c", t.config, "--ssh-program", t.ssh, "--scp-program",
                      t.scp, "--engines", t.engines, "--remote-state", t.state,
                      "--remote-config", t.config, "--state", t.local],
                  env=dict(t.env, FAKEUNAME="Linux sparc64"))
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=60)
    t.check("cannot deploy an engine: no engine for linux-sparc64" in f.get("msg", ""),
            f"failed {f}")
    t.check(c.p.wait(10) == 2, "exit 2")


@scenario
def mismatch(t):
    """A host of another protocol: without --deploy, failed with the hint;
    with it, goodbye, the engine deployed (already there), and since that
    host still holds the place, failed again, never deploying twice; the
    host runs on throughout."""
    t.engine()
    p = t.deploy()
    t.check(p.returncode == 0, f"deploy: {p.stderr}")
    # The host starts first, from here: started through the fake ssh it
    # would descend from connect's reaper, which on Linux, a subreaper,
    # takes it down when that connect ends (a real ssh's host is on
    # another machine)
    env = dict(t.env, HARNESS_TEST_PROTO="7")
    first = R.Client(t)
    first.attach(env=env)
    first.call("hello")
    first.stream.disconnect()
    pid = t.hostpids()[0]
    c = t.connect(env=env)
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=20)
    t.check(f.get("deploy") == "mismatch" and "speaks protocol 7" in f.get("msg", "")
            and "harness deploy here" in f.get("msg", ""), f"failed {f}")
    t.check(c.p.wait(10) == 2, "exit 2")
    c = t.connect("--deploy", env=env)
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=60)
    states = [r.get("state") for r in c.records if r.get("ev") == "remote"]
    t.check(states == ["connecting", "deploying", "connecting", "failed"],
            f"states {states}")
    done = [r for r in c.records if r.get("ev") == "deploy" and r.get("step") == "done"]
    t.check(done and "already deployed" in done[0].get("msg", ""), f"deploy {done}")
    t.check(c.p.wait(10) == 2, "exit 2")
    t.check(t.hostpids() == [pid] and not R.gone(pid, 0.5), "the host stopped")


# Over real ssh

def rsh(a, script):
    """A script in the remote host's sh: its own, or w64devkit's."""
    p = R.remote_sh(a, script)
    return p.stdout


REALMADE = []   # (path, made .harness too) put there by "real"


@scenario
def real(t):
    """Deploy to a real host, deploy again, then connect and run a turn."""
    a = t.args
    if not a.ssh:
        return "skip"
    engines = os.path.join(os.path.dirname(t.harness), "engines")
    before = rsh(a, "ls -a .harness 2>/dev/null; echo end")
    madedir = "end" in before and not before.strip().startswith(".")
    argv = [t.harness, "deploy", a.ssh, "--engines", engines, "--state", t.local]
    t0 = time.monotonic()
    p = subprocess.run(argv, capture_output=True, text=True, timeout=900)
    took = time.monotonic() - t0
    t.check(p.returncode == 0, f"deploy exit {p.returncode}: {p.stderr}")
    shown = p.stdout.strip()
    name = shown.rsplit("/", 1)[-1]
    if name not in before:
        REALMADE.append((name, madedir and not REALMADE))
    print(f"     deployed {shown} in {took:.1f} s", flush=True)
    rec = {}
    with open(os.path.join(t.local, "deploy", f"{a.ssh}.ini")) as f:
        for line in f:
            k, _, v = line.partition("=")
            rec[k.strip()] = v.strip()
    plat = rec.get("platform", "")
    local = os.path.join(engines, f"harness-engine-{t.version}-{plat}")
    local += ".exe" if plat.startswith("windows") else ""
    t.check(rec.get("sha256") == sha256(local), f"record {rec}")
    there = rsh(a, f"sha256sum .harness/{name}")
    t.check(there.split()[:1] == [sha256(local)], f"there: {there!r}")
    q = subprocess.run(argv, capture_output=True, text=True, timeout=600)
    t.check(q.returncode == 0 and "already deployed" in q.stderr, f"again: {q.stderr}")
    t.check("uploading" not in q.stderr, "uploaded again")

    # Connect to it, through an ssh that logs what it runs
    prog = os.path.join(t.root, "sshwrap")
    with open(prog, "w") as f:
        f.write(f"#!/bin/sh\necho \"$*\" >> {shlex.quote(t.fakelog)}/ssh.log\n"
                "exec ssh \"$@\"\n")
    os.chmod(prog, 0o755)
    rstate = f"{a.remote_root}/state-{t.nonce}"
    rconf = f"{a.remote_root}/harness-{t.nonce}.ini"
    R.remote_write(a, rconf, open(t.config).read().replace(a.url, a.remote_url))
    try:
        c = R.connect(t, ["--ssh", a.ssh, "--ssh-program", prog, "--remote-state", rstate,
                          "--remote-config", rconf, "--state", t.local])
        up = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up",
                    what="up", timeout=120)
        t.check(up.get("version") == t.version, f"up {up}")
        # The path as the remote shell takes it: in quotes, with a backslash
        path = rec.get("path")
        t.check(f"{path} attach" in t.sshlog() or f'"{path}" attach' in t.sshlog(),
                f"ssh ran {t.sshlog()!r}")
        r = c.call("new", cwd=a.remote_work, prompt="go", model=t.model("remote_tool"),
                   attended=1, timeout=120)
        t.check(r.get("ok") == 1, f"new: {r}")
        sid = r["sid"]
        end = c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                     what="turn.end", timeout=180)
        t.check(end.get("why") == "done", f"turn.end {end}")
        tools = [r for r in c.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
        t.check(tools and tools[0].get("status") == "ok", f"the command: {tools}")

        # Another build of this version, with --force, while the host runs
        # from the engine it replaces: on Windows the old one is renamed
        # aside, and either way the host runs on
        other = os.path.join(t.root, "other")
        os.makedirs(other)
        base = os.path.basename(local)
        with open(local, "rb") as f:
            data = f.read() + b"another build"
        with open(os.path.join(other, base), "wb") as f:
            f.write(data)
        with open(os.path.join(other, base + ".sha256"), "w") as f:
            f.write(f"{hashlib.sha256(data).hexdigest()}  {base}\n")
        fargv = [t.harness, "deploy", a.ssh, "--engines", other, "--state", t.local]
        p = subprocess.run(fargv, capture_output=True, text=True, timeout=900)
        t.check(p.returncode == 1 and "another build" in p.stderr, f"no --force: {p.stderr}")
        p = subprocess.run(fargv + ["--force"], capture_output=True, text=True, timeout=900)
        t.check(p.returncode == 0, f"--force: {p.stderr}")
        there = rsh(a, f"sha256sum .harness/{name}; ls .harness")
        t.check(there.split()[:1] == [hashlib.sha256(data).hexdigest()], f"there: {there!r}")
        aside = [x for x in there.split() if ".old-" in x]
        print(f"     --force: {'renamed aside' if aside else 'replaced in place'}", flush=True)
        r = c.call("list", timeout=60)
        t.check(r.get("ok") == 1, f"the host after --force: {r}")
        q = c.call("quit", timeout=60)
        t.check(q.get("ok") == 1, f"quit {q}")
        t.check(c.close(timeout=60) == 0, "connect exits 0")
    finally:
        R.remote_sh(a, f"rm -rf {rstate} {rconf}")


def realcleanup(a):
    """Remove what "real" deployed, and ~/.harness if it made it."""
    for name, madedir in REALMADE:
        # A host that quit may take a moment to let go of its program,
        # which Windows will not remove while it runs
        script = (f"for i in 1 2 3 4 5; do rm -f .harness/{name} .harness/{name}.part "
                  f".harness/{name}.old-* && [ ! -e .harness/{name} ] && break; sleep 2; done\n")
        if madedir:
            script += "rmdir .harness\n"
        script += "ls -a .harness 2>/dev/null; echo end\n"
        left = R.remote_sh(a, script).stdout
        print(f"     removed .harness/{name}" + (" and .harness" if madedir else "")
              + f"; left: {left.split()}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True)
    ap.add_argument("--ssh", help="a host for the real scenario")
    ap.add_argument("--remote-root", help="a directory of ours there")
    ap.add_argument("--windows", action="store_true", help="the ssh host is Windows")
    ap.add_argument("--shell", help="the ssh host's shell for scripts (default: sh -s)")
    ap.add_argument("--no-tunnel", action="store_true",
                    help="the ssh host cannot forward a port back")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()

    lan = args.ssh and args.no_tunnel
    srv, url = fake_provider.start(R.lanaddress(args.ssh) if lan else "127.0.0.1")
    args.url = url
    tunnel = None
    if args.ssh:
        if not args.remote_root:
            sys.exit("deploy.py: --ssh needs --remote-root")
        args.remote_url = url
        args.remote_work = f"{args.remote_root}/work"
        R.remote_sh(args, f"mkdir -p {args.remote_work}")
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
            t = D(args, name)
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
        if args.ssh:
            realcleanup(args)
        fake_provider.STATE.stop.set()
        if tunnel:
            tunnel.terminate()
        srv.shutdown()
    print(f"{len(names) - failed}/{len(names)} deploy scenarios passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
