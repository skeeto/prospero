#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Engine deployment: prospero deploy, and connect deploying on its own.

The local scenarios run the real binary through an ssh and scp of their
own that work here, in a fresh fake home directory, with an engines
directory holding a copy of the binary under its deployed name: deploy
uploads, verifies and records it, tries again when the upload breaks or
arrives damaged, refuses another build of the same version without
--force, and connect then runs the engine deployed, or deploys one
itself (--deploy, or [remote NAME] deploy = auto) when it finds none.
A desktop helper in the engines directory (tests/fake_desktop.py behind
a script) goes beside the engine, where the engine then runs it for its
desktop tool; one that does not run there is only a warning.

  uv run tests/deploy.py --prospero build/prospero [NAME ...]

With --ssh the "real" scenario deploys to that host the engine for its
platform from the engines directory next to the binary (build/engines,
from tools/engines.py), deploys again (nothing to upload), then connects
to it with the fake provider reached as in tests/remote.py, and runs a
turn with a shell command in it. Engines it put in ~/.prospero there, and
~/.prospero itself if it made it, are removed at the end:

  uv run tests/deploy.py --prospero build/prospero --ssh HOST \\
      --remote-root /tmp/prospero-deploy real
  uv run tests/deploy.py --prospero build/prospero --ssh WINHOST --windows \\
      --remote-root C:/Users/USER/AppData/Local/Temp/prospero-deploy real
  uv run tests/deploy.py --prospero build/prospero --ssh XPHOST \\
      --no-tunnel --shell "sh -s" \\
      --remote-root C:/DOCUME~1/USER/LOCALS~1/Temp/prospero-deploy real
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
FAKE_DESKTOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_desktop.py")


def scenario(fn):
    SCENARIOS[fn.__name__] = fn
    return fn


# Python, as remote.py's fakes (R.program), so that they run on Windows
# too, where the shell there is busybox-w32's sh, from PATH
FAKESSH = R.FAKE + r"""
# ssh here: the command runs in $FAKEHOME, as a remote shell's would in
# the home directory; FAKEUNAME makes the probe's uname say otherwise,
# and FAKEPATH is the only PATH there
args = sys.argv[1:]
with open(os.path.join(os.environ["FAKELOG"], "ssh.log"), "a", newline="") as f:
    f.write(" ".join(args) + "\n")
cmd = command(args)
if os.environ.get("FAKEUNAME") and cmd.startswith("uname"):
    sys.stdout.buffer.write(os.environ["FAKEUNAME"].encode() + b"\n%OS%\n")
    sys.exit(0)
if os.environ.get("FAKEPATH"):
    os.environ["PATH"] = os.environ["FAKEPATH"]
os.chdir(os.environ["FAKEHOME"])
run(["sh" if os.name == "nt" else "/bin/sh", "-c", cmd])
"""

FAKESCP = r"""
# scp here, into $FAKEHOME: the first $FAKESCP_FAIL calls fail as a
# dropped link would, and the first $FAKESCP_CORRUPT arrive damaged;
# each takes $FAKESCP_SLEEP seconds
import os, shutil, sys, time
args = sys.argv[1:]
log = os.environ["FAKELOG"]
with open(os.path.join(log, "scp.log"), "a", newline="") as f:
    f.write(" ".join(args) + "\n")
try:
    with open(os.path.join(log, "scp.count")) as f:
        n = int(f.read()) + 1
except OSError:
    n = 1
with open(os.path.join(log, "scp.count"), "w", newline="") as f:
    f.write(f"{n}\n")
time.sleep(float(os.environ.get("FAKESCP_SLEEP", "0")))
if n <= int(os.environ.get("FAKESCP_FAIL", "0")):
    sys.stderr.write("Connection closed by 10.0.0.1 port 22\n")
    sys.exit(255)
while len(args) > 2:
    if args[0] == "-o":
        args = args[2:]
    elif args[0].startswith("-"):
        args = args[1:]
    else:
        break
dst = os.path.join(os.environ["FAKEHOME"], args[1].split(":", 1)[-1])
try:
    shutil.copyfile(args[0], dst)
except OSError as e:
    sys.stderr.write(f"fakescp: {e}\n")
    sys.exit(1)
if n <= int(os.environ.get("FAKESCP_CORRUPT", "0")):
    with open(dst, "ab") as f:
        f.write(b"x")
"""

FAKEWSL = R.FAKE + r"""
# wsl.exe here: -l -q lists $FAKEDISTROS (comma-separated) in UTF-16 as
# it does by default; -d DISTRO --cd ~ --exec /bin/sh -c CMD runs CMD in
# $FAKEHOME, its input passed through (cat, for uploads), for one of
# them, else says what wsl.exe says of an unknown one, on its output
args = sys.argv[1:]
with open(os.path.join(os.environ["FAKELOG"], "wsl.log"), "a", newline="") as f:
    f.write(" ".join(args) + f" WSL_UTF8={os.environ.get('WSL_UTF8')}\n")
distros = [d for d in os.environ.get("FAKEDISTROS", "").split(",") if d]
if args == ["-l", "-q"]:
    sys.stdout.buffer.write("".join(d + "\r\n" for d in distros).encode("utf-16-le"))
    sys.exit(0)
if len(args) != 8 or args[0] != "-d" or args[2:7] != ["--cd", "~", "--exec", "/bin/sh", "-c"]:
    sys.stderr.write(f"fakewsl: unexpected {args}\n")
    sys.exit(2)
if args[1] not in distros:
    sys.stdout.buffer.write(b"There is no distribution with the supplied name.\r\n"
                            b"Error code: Wsl/Service/WSL_E_DISTRO_NOT_FOUND\r\n")
    sys.exit(-1)
os.chdir(os.environ["FAKEHOME"])
run(["sh" if os.name == "nt" else "/bin/sh", "-c", args[7]])
"""


def localplatform():
    if os.name == "nt":
        # As the harness names Windows: x86-64 on Arm too, which runs it
        import platform
        return "windows-" + ("i686" if platform.machine() == "x86" else "x86_64")
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
        self.ssh = R.program(self, "fakessh", FAKESSH)
        self.scp = R.program(self, "fakescp", FAKESCP)
        self.wsl = R.program(self, "fakewsl", FAKEWSL)
        out = subprocess.run([self.prospero, "--version"], capture_output=True,
                             text=True).stdout.split()
        self.version = out[1]
        self.platform = localplatform()
        self.exe = ".exe" if os.name == "nt" else ""
        self.ename = f"prospero-engine-{self.version}-{self.platform}{self.exe}"
        self.rname = f"engine-0-{self.version}-{self.platform}{self.exe}"
        self.env = dict(self.env, FAKEHOME=self.home, FAKELOG=self.fakelog,
                        FAKEDISTROS="Deb,docker-desktop")
        # An empty configuration, never the user's own
        conf = os.path.join(self.root, "config")
        os.makedirs(conf)
        self.env.update(XDG_CONFIG_HOME=conf, APPDATA=conf)
        # No prospero on the remote shell's PATH, as on a new machine (on
        # Windows PATH has the shell, busybox-w32's, so only directories
        # with a prospero go)
        if os.name != "nt":
            self.env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        else:
            self.env["PATH"] = os.pathsep.join(
                d for d in self.env.get("PATH", "").split(os.pathsep)
                if not os.path.exists(os.path.join(d, "prospero.exe")))

    def engine(self, extra=b""):
        """An engine of the local platform in the engines directory: this
        binary, perhaps with bytes after it (another build)."""
        dst = os.path.join(self.engines, self.ename)
        with open(self.prospero, "rb") as f:
            data = f.read() + extra
        with open(dst, "wb") as f:
            f.write(data)
        os.chmod(dst, 0o755)
        with open(dst + ".sha256", "w", newline="") as f:
            f.write(f"{hashlib.sha256(data).hexdigest()}  {self.ename}\n")
        return dst

    def helper(self, works=True, extra=""):
        """A desktop helper of the local platform in the engines
        directory: tests/fake_desktop.py behind a script that answers
        --version as the real one does, or, unless works, fails it."""
        name = f"prospero-desktop-{self.version}-{self.platform}"
        dst = os.path.join(self.engines, name)
        version = (f'echo "prospero-desktop {self.version}"; exit 0' if works
                   else 'echo "cannot load libc" >&2; exit 1')
        text = (f"#!/bin/sh\n# {extra}\n"
                f'if [ "$1" = --version ]; then {version}; fi\n'
                f"exec {shlex.quote(sys.executable)} {shlex.quote(FAKE_DESKTOP)} \"$@\"\n")
        with open(dst, "w", newline="") as f:
            f.write(text)
        os.chmod(dst, 0o755)
        with open(dst + ".sha256", "w", newline="") as f:
            f.write(f"{sha256(dst)}  {name}\n")
        return dst

    def helperdeployed(self):
        return os.path.join(self.home, ".prospero",
                            f"desktop-0-{self.version}-{self.platform}")

    def deploy(self, *extra, env=None, target="here"):
        argv = [self.prospero, "deploy", target, "--ssh-program", self.ssh,
                "--scp-program", self.scp, "--engines", self.engines,
                "--state", self.local] + list(extra)
        return subprocess.run(argv, capture_output=True, text=True, timeout=120,
                              env=env or self.env)

    def deployed(self):
        return os.path.join(self.home, ".prospero", self.rname)

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

    def wsldeploy(self, *extra, env=None, target=None):
        """Deploy into the fake distro Deb, or to a target by name."""
        argv = [self.prospero, "deploy"] + ([target] if target else ["--wsl", "Deb"])
        argv += ["--wsl-program", self.wsl, "--ssh-program", self.ssh, "--scp-program",
                 self.scp, "--engines", self.engines, "--state", self.local] + list(extra)
        return subprocess.run(argv, capture_output=True, text=True, timeout=120,
                              env=env or self.env)

    def wsllog(self):
        try:
            with open(os.path.join(self.fakelog, "wsl.log")) as f:
                return f.read()
        except OSError:
            return ""

    def record(self, name="here"):
        path = os.path.join(self.local, "deploy", name + ".ini")
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
    t.check(p.stdout == f"~/.prospero/{t.rname}\n", f"stdout {p.stdout!r}")
    dst = t.deployed()
    t.check(os.path.isfile(dst) and sha256(dst) == sha256(src), "the engine is there")
    if os.name != "nt":  # no modes there
        t.check(os.stat(dst).st_mode & 0o111, "executable")
        t.check(oct(os.stat(os.path.dirname(dst)).st_mode & 0o777) == "0o700",
                "~/.prospero 0700")
    t.check(not os.path.exists(dst + ".part"), "no .part left")
    rec = t.record()
    t.check(rec.get("host") == "here" and rec.get("platform") == t.platform and
            rec.get("path") == f".prospero/{t.rname}" and rec.get("sha256") == sha256(src)
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
def concurrent(t):
    """Two deploys to one destination at once: the second waits for the
    first, then finds it there, so there is one upload."""
    t.engine()
    env = dict(t.env, FAKESCP_SLEEP="2")
    argv = [t.prospero, "deploy", "here", "--ssh-program", t.ssh, "--scp-program",
            t.scp, "--engines", t.engines, "--state", t.local]
    first = subprocess.Popen(argv, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             text=True, env=env)
    try:
        log = os.path.join(t.fakelog, "scp.log")
        deadline = time.monotonic() + 30
        while not os.path.exists(log):  # the first is uploading
            t.check(time.monotonic() < deadline and first.poll() is None,
                    "the first deploy never uploaded")
            time.sleep(0.05)
        q = t.deploy(env=env)
        _, err = first.communicate(timeout=60)
    finally:
        if first.poll() is None:
            first.kill()
            first.wait()
    t.check(first.returncode == 0, f"first: exit {first.returncode}: {err}")
    t.check(q.returncode == 0, f"second: exit {q.returncode}: {q.stderr}")
    t.check("waiting for another deploy to here" in q.stderr, f"second: {q.stderr}")
    t.check("already deployed" in q.stderr, f"second: {q.stderr}")
    t.check("waiting" not in err, f"first: {err}")
    t.check(t.scps() == 1, f"{t.scps()} uploads")


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
    with open(src + ".sha256", "w", newline="") as f:
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
    argv = [t.prospero, "deploy", "here", "--ssh-program", t.ssh, "--scp-program", t.scp,
            "--engines", "engines", "--state", t.local]
    p = subprocess.run(argv, capture_output=True, text=True, timeout=120, env=t.env,
                       cwd=t.root)
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(sha256(t.deployed()) == sha256(src), "the engine is there")


@scenario
def config_engines(t):
    """With no engines directory next to the program, the one beside the
    default prospero.ini, where the engines zip unpacks wherever the
    program is installed (a macOS app in /Applications). Another build,
    so that this program serving instead would show."""
    src = t.engine(extra=b"another build")
    there = os.path.join(t.root, "config", "prospero", "engines")
    shutil.copytree(t.engines, there)
    bindir = os.path.join(t.root, "bin")
    os.makedirs(bindir)
    exe = os.path.join(bindir, os.path.basename(t.prospero))
    shutil.copy2(t.prospero, exe)
    argv = [exe, "deploy", "here", "--ssh-program", t.ssh, "--scp-program", t.scp,
            "--state", t.local]
    p = subprocess.run(argv, capture_output=True, text=True, timeout=120, env=t.env)
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(sha256(t.deployed()) == sha256(src), "the engine beside prospero.ini is there")
    # An engines directory next to the program without this platform's
    # engine: the one beside prospero.ini still serves, and where neither
    # has one (nor is this program built for it), the failure names both
    os.makedirs(os.path.join(bindir, "engines"))
    q = subprocess.run(argv + ["--force"], capture_output=True, text=True, timeout=120,
                       env=t.env)
    t.check(q.returncode == 0, f"exit {q.returncode}: {q.stderr}")
    q = subprocess.run(argv, capture_output=True, text=True, timeout=120,
                       env=dict(t.env, FAKEUNAME="Linux sparc64"))
    said = q.stderr.replace("\\", "/")  # Windows' paths, joined with /
    t.check(q.returncode != 0 and "is in neither" in said and
            os.path.join(bindir, "engines").replace("\\", "/") in said and
            there.replace("\\", "/") in said, f"neither: {q.stderr}")


@scenario
def no_hash(t):
    """No tool there to compute a SHA-256: deploying fails at once, for
    good, before uploading anything."""
    if os.name == "nt":
        return "skip"  # busybox-w32's sh runs its applets, sha256sum too, whatever PATH has
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
    t.check(f".prospero/{t.rname} attach" in t.sshlog(), f"ssh ran {t.sshlog()!r}")
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
    t.check("prospero deploy here" in f.get("msg", ""), f"msg {f}")
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
    t.check(done.get("path") == f"~/.prospero/{t.rname}" and
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
    # Nothing to deploy for this platform: the end, saying why. (Windows
    # removes no running program, so the host quit must first be gone.)
    deadline = time.monotonic() + 10
    while True:
        try:
            shutil.rmtree(os.path.join(t.home, ".prospero"))
            break
        except PermissionError:
            if os.name != "nt" or time.monotonic() > deadline:
                raise
            time.sleep(0.2)
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
    env = dict(t.env, PROSPERO_TEST_PROTO="7")
    first = R.Client(t)
    first.attach(env=env)
    first.call("hello")
    first.stream.disconnect()
    pid = t.hostpids()[0]
    c = t.connect(env=env)
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=20)
    t.check(f.get("deploy") == "mismatch" and "speaks protocol 7" in f.get("msg", "")
            and "prospero deploy here" in f.get("msg", ""), f"failed {f}")
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


@scenario
def desktop(t):
    """A desktop helper in the engines directory goes beside the engine,
    as desktop-PROTO-VERSION-OS-ARCH, is recorded, and is uploaded again
    only when missing there; the engine connected to then runs it as its
    desktop tool's helper. Not on Windows, whose engine is its own."""
    if os.name == "nt":
        return "skip"
    t.engine()
    src = t.helper()
    p = t.deploy()
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(p.stdout == f"~/.prospero/{t.rname}\n", f"stdout {p.stdout!r}")
    dst = t.helperdeployed()
    t.check(os.path.isfile(dst) and sha256(dst) == sha256(src), "the helper is there")
    t.check(not os.path.exists(dst + ".part"), "no .part left")
    t.check(t.scps() == 2, f"{t.scps()} uploads")
    t.check(t.record().get("desktop") == f".prospero/{os.path.basename(dst)}",
            f"record {t.record()}")
    t.check("renaming the desktop helper into place" in p.stderr and
            "with the desktop helper" in p.stderr, f"stderr {p.stderr}")
    q = t.deploy()
    t.check(q.returncode == 0 and "already deployed" in q.stderr and t.scps() == 2,
            f"again: {q.stderr}")
    os.remove(dst)
    q = t.deploy()
    t.check(q.returncode == 0 and "already deployed" not in q.stderr and t.scps() == 3
            and sha256(dst) == sha256(src), f"the helper missing: {q.stderr}")

    # The deployed engine's desktop tool: its helper is the one beside it
    log = os.path.join(t.root, "desktop.log")
    t.write_config(t.args.url, "[desktop]\noffer = all\nask = false\n")
    env = dict(t.env, FAKE_DESKTOP_LOG=log)
    env.pop("PROSPERO_TEST_DESKTOP", None)
    if sys.platform.startswith("linux") and not (env.get("DISPLAY") or
                                                 env.get("WAYLAND_DISPLAY")):
        # The engine offers the tool only with a display, which the fake
        # helper does not need, and ctest over ssh has none
        env["DISPLAY"] = ":99"
    c = t.connect(env=env)
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up")
    r = c.call("new", cwd=t.work, prompt="go", model=t.model("desktop_off"), attended=1)
    t.check(r.get("ok") == 1, f"new: {r}")
    sid = r["sid"]
    end = c.wait(lambda r: r.get("ev") == "turn.end" and r.get("sid") == sid,
                 what="turn.end", timeout=60)
    t.check(end.get("why") == "done", f"turn.end {end}")
    tools = [r for r in c.records if r.get("ev") == "tool.end" and r.get("sid") == sid]
    t.check(tools and tools[0].get("status") == "ok", f"the screenshot: {tools}")
    try:
        with open(log, encoding="utf-8") as f:
            took = [json.loads(x) for x in f.read().splitlines()]
    except OSError:
        took = []
    t.check(took[:1] == [["hello"]] and took[1:2] and took[1][0] == "shot",
            f"the helper took {took}")
    c.call("quit")
    t.check(c.close() == 0, "connect exits 0")


@scenario
def desktop_warn(t):
    """A helper that does not run there, or another build of it, is only
    a warning: the engine is deployed and recorded all the same, and a
    helper that does not run is removed again; --force replaces another
    build. Not on Windows, whose engine is its own helper."""
    if os.name == "nt":
        return "skip"
    t.engine()
    t.helper(works=False)
    p = t.deploy()
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check("warning: the desktop helper is not deployed: the desktop helper does "
            "not run on here: cannot load libc" in p.stderr, f"stderr {p.stderr}")
    t.check(os.path.isfile(t.deployed()) and not os.path.exists(t.helperdeployed()),
            "the engine alone is there")
    t.check("desktop" not in t.record(), f"record {t.record()}")
    good = sha256(t.helper())
    p = t.deploy()
    t.check(p.returncode == 0 and "warning" not in p.stderr and
            sha256(t.helperdeployed()) == good, f"a good one: {p.stderr}")
    new = sha256(t.helper(extra="another build"))
    p = t.deploy()
    t.check(p.returncode == 0 and "another build of prospero-desktop" in p.stderr and
            sha256(t.helperdeployed()) == good, f"without --force: {p.stderr}")
    p = t.deploy("--force")
    t.check(p.returncode == 0 and "warning" not in p.stderr and
            sha256(t.helperdeployed()) == new, f"--force: {p.stderr}")


@scenario
def desktop_self(t):
    """No engine in the engines directory, so this program serves, and
    its own helper next to it goes with it."""
    mine = os.path.join(os.path.dirname(t.prospero), "prospero-desktop")
    if not os.path.isfile(mine):
        return "skip"
    p = t.deploy()
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(sha256(t.deployed()) == sha256(t.prospero), "this program is the engine")
    t.check(sha256(t.helperdeployed()) == sha256(mine), "its helper went with it")


# Over real ssh

@scenario
def wsl_fresh(t):
    """Deploy into a WSL distro: through wsl.exe, the upload on cat's
    input (no scp), recorded as wsl:DISTRO; then again: nothing to
    upload."""
    src = t.engine()
    p = t.wsldeploy()
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check(p.stdout == f"~/.prospero/{t.rname}\n", f"stdout {p.stdout!r}")
    t.check(sha256(t.deployed()) == sha256(src), "the engine is there")
    t.check(t.scps() == 0 and not t.sshlog(), "ssh or scp ran")
    log = t.wsllog()
    t.check(f"cat > .prospero/{t.rname}.part" in log, f"no upload in {log!r}")
    t.check("WSL_UTF8=None" not in log, f"WSL_UTF8 unset: {log!r}")
    rec = t.record("wsl_Deb")
    t.check(rec.get("host") == "wsl:Deb" and rec.get("sha256") == sha256(src),
            f"record {rec}")
    t.check("probing Deb" in p.stderr, f"stderr {p.stderr!r}")
    q = t.wsldeploy()
    t.check(q.returncode == 0 and "already deployed" in q.stderr, f"again: {q.stderr}")
    t.check(t.wsllog().count(".part") == log.count(".part"), "uploaded again")


@scenario
def wsl_name(t):
    """A distro by name, with no section, is found by wsl -l -q; Docker
    Desktop's own distro is not one, so its name is an ssh destination."""
    t.engine()
    p = t.wsldeploy(target="Deb")
    t.check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    t.check("-l -q" in t.wsllog() and not t.sshlog(), f"wsl {t.wsllog()!r}")
    t.check(t.record("wsl_Deb").get("host") == "wsl:Deb", "recorded")
    q = t.wsldeploy(target="docker-desktop")
    t.check("-d docker-desktop" not in t.wsllog() and " docker-desktop uname" in t.sshlog(),
            f"docker-desktop: {t.wsllog()!r} {t.sshlog()!r}")


@scenario
def wsl_missing_distro(t):
    """A distro that is not installed: deploying fails at once, with
    wsl.exe's own message, and no second try."""
    t.engine()
    p = t.wsldeploy(env=dict(t.env, FAKEDISTROS=""))
    t.check(p.returncode != 0, f"exit {p.returncode}")
    t.check("There is no distribution with the supplied name" in p.stderr,
            f"stderr {p.stderr!r}")
    t.check(t.wsllog().count("-d Deb") == 1, f"tries {t.wsllog()!r}")


@scenario
def wsl_connect_deploy(t):
    """[remote w] wsl = Deb with deploy = auto: connect deploys into the
    distro, then a turn runs through it; a section of a missing distro
    fails for good, saying so."""
    t.engine()
    with open(t.config, "a") as f:
        f.write("[remote w]\nwsl = Deb\ndeploy = auto\n"
                "[remote gone]\nwsl = Nope\n")
    common = ["-c", t.config, "--wsl-program", t.wsl, "--engines", t.engines,
              "--remote-state", t.state, "--remote-config", t.config, "--state", t.local]
    c = R.connect(t, ["w"] + common)
    c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "up", what="up",
           timeout=60)
    t.check(os.path.isfile(t.deployed()), "deployed")
    t.check(f".prospero/{t.rname} attach" in t.wsllog(), f"wsl ran {t.wsllog()!r}")
    t.turn(c)
    c.call("quit")
    t.check(c.close() == 0, "connect exits 0")
    c = R.connect(t, ["gone"] + common)
    f = c.wait(lambda r: r.get("ev") == "remote" and r.get("state") == "failed",
               what="failed", timeout=20)
    t.check("There is no distribution" in f.get("msg", ""), f"failed {f}")
    t.check(c.p.wait(10) == 2, "exit 2")
    t.check(t.wsllog().count("-d Nope") == 1, f"tries {t.wsllog()!r}")


def rsh(a, script):
    """A script in the remote host's sh: its own, or w64devkit's."""
    p = R.remote_sh(a, script)
    return p.stdout


REALMADE = []   # (path, made .prospero too) put there by "real"


@scenario
def real(t):
    """Deploy to a real host, deploy again, then connect and run a turn."""
    a = t.args
    if not a.ssh:
        return "skip"
    engines = os.path.join(os.path.dirname(t.prospero), "engines")
    before = rsh(a, "ls -a .prospero 2>/dev/null; echo end")
    madedir = "end" in before and not before.strip().startswith(".")
    argv = [t.prospero, "deploy", a.ssh, "--engines", engines, "--state", t.local]
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
    print(f"     desktop helper: {rec.get('desktop', 'none')}", flush=True)
    plat = rec.get("platform", "")
    local = os.path.join(engines, f"prospero-engine-{t.version}-{plat}")
    local += ".exe" if plat.startswith("windows") else ""
    t.check(rec.get("sha256") == sha256(local), f"record {rec}")
    there = rsh(a, f"sha256sum .prospero/{name}")
    t.check(there.split()[:1] == [sha256(local)], f"there: {there!r}")
    q = subprocess.run(argv, capture_output=True, text=True, timeout=600)
    t.check(q.returncode == 0 and "already deployed" in q.stderr, f"again: {q.stderr}")
    t.check("uploading" not in q.stderr, "uploaded again")

    # Connect to it, through an ssh that logs what it runs
    prog = R.program(t, "sshwrap", R.FAKE + f"""
with open({os.path.join(t.fakelog, "ssh.log")!r}, "a", newline="") as f:
    f.write(" ".join(sys.argv[1:]) + "\\n")
run(["ssh"] + sys.argv[1:])
""")
    rstate = f"{a.remote_root}/state-{t.nonce}"
    rconf = f"{a.remote_root}/prospero-{t.nonce}.ini"
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
        with open(os.path.join(other, base + ".sha256"), "w", newline="") as f:
            f.write(f"{hashlib.sha256(data).hexdigest()}  {base}\n")
        fargv = [t.prospero, "deploy", a.ssh, "--engines", other, "--state", t.local]
        p = subprocess.run(fargv, capture_output=True, text=True, timeout=900)
        t.check(p.returncode == 1 and "another build" in p.stderr, f"no --force: {p.stderr}")
        p = subprocess.run(fargv + ["--force"], capture_output=True, text=True, timeout=900)
        t.check(p.returncode == 0, f"--force: {p.stderr}")
        there = rsh(a, f"sha256sum .prospero/{name}; ls .prospero")
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
    """Remove what "real" deployed, and ~/.prospero if it made it."""
    for name, madedir in REALMADE:
        # A host that quit may take a moment to let go of its program,
        # which Windows will not remove while it runs; the desktop
        # helper deployed beside it goes too
        script = ""
        for n in (name, "desktop-" + name.removeprefix("engine-")):
            script += (f"for i in 1 2 3 4 5; do rm -f .prospero/{n} .prospero/{n}.part "
                       f".prospero/{n}.old-* && [ ! -e .prospero/{n} ] && break; "
                       f"sleep 2; done\n")
        if madedir:
            script += "rmdir .prospero\n"
        script += "ls -a .prospero 2>/dev/null; echo end\n"
        left = R.remote_sh(a, script).stdout
        print(f"     removed .prospero/{name}" + (" and .prospero" if madedir else "")
              + f"; left: {left.split()}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prospero", required=True)
    ap.add_argument("--ssh", help="a host for the real scenario")
    ap.add_argument("--remote-root", help="a directory of ours there")
    ap.add_argument("--windows", action="store_true", help="the ssh host is Windows")
    ap.add_argument("--shell", help="the ssh host's shell for scripts (default: sh -s)")
    ap.add_argument("--no-tunnel", action="store_true",
                    help="the ssh host cannot forward a port back")
    ap.add_argument("--shim", help="Windows: the program of tests/shim_win32.c")
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
