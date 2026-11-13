"""A private desktop for a GUI checklist on Linux with no display: an X
server (Xvfb), a window manager (openbox) and a session bus of its own
(dbus-daemon), so that checklists run side by side (tests/slots.py)
without sharing a screen, a clipboard or GNOME's bus, where a tray item
or a notification would show on the real desktop.

Each starts in its own session under setpriv --pdeathsig, which ends it
when this process does, however it ends. Their files live in
/tmp/prospero-gui-x-*, named with this process's pid and start time, so
that a later run can remove one whose owner is gone, without signals.
"""

import os
import select
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

PREFIX = "prospero-gui-x-"
XPROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xprobe.py")

# A session bus like session.conf's, but with no services to activate,
# which could outlive the run, and listening only in the run's directory
BUSCONF = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={path}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


class Failed(Exception):
    pass


def starttime(pid):
    """A process's start time in clock ticks since boot, or None if it is
    gone: with the pid, it names one process."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            stat = f.read()
    except OSError:
        return None
    return stat[stat.rindex(")") + 2:].split()[19]


def sweep():
    """Remove the directories of runs that are gone. Signals nothing: what
    they started ended with them (pdeathsig)."""
    tmp = tempfile.gettempdir()
    for name in os.listdir(tmp):
        path = os.path.join(tmp, name)
        if not name.startswith(PREFIX) or os.path.islink(path):
            continue
        try:
            if os.stat(path).st_uid != os.getuid():
                continue
            with open(os.path.join(path, "owner")) as f:
                pid, start = f.read().split()
        except (OSError, ValueError):
            # Being made, or unreadable: removed only once old
            try:
                if time.time() - os.stat(path).st_mtime < 3600:
                    continue
            except OSError:
                continue
            pid, start = "0", ""
        if starttime(int(pid)) != start:
            shutil.rmtree(path, ignore_errors=True)


def readline(r, timeout):
    """A line from pipe r within timeout seconds, or what came."""
    got = b""
    deadline = time.monotonic() + timeout
    while not got.endswith(b"\n") and time.monotonic() < deadline:
        ready, _, _ = select.select([r], [], [], 0.25)
        if ready:
            chunk = os.read(r, 256)
            if not chunk:
                break
            got += chunk
    return got.decode(errors="replace").strip()


class Desktop:
    """Start with start(), which sets this process's environment for
    what it runs; stop() ends it all. wm is "openbox", "none", or a
    command line; slots is how many checklists share the machine."""

    def __init__(self, screen="1920x1080", wm="openbox", slots=1, keepruntime=False):
        self.screen = screen
        self.wm = wm
        self.slots = max(1, slots)
        self.keepruntime = keepruntime
        self.procs = []
        self.dir = None

    def spawn(self, argv, env=None, fds=()):
        pre = ["setpriv", "--pdeathsig", "KILL"] if shutil.which("setpriv") else []
        p = subprocess.Popen(pre + argv, env=env, pass_fds=fds, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
        self.procs.append(p)
        return p

    def start(self):
        sweep()
        self.dir = tempfile.mkdtemp(prefix=PREFIX)
        with open(os.path.join(self.dir, "owner"), "w") as f:
            f.write(f"{os.getpid()} {starttime(os.getpid())}\n")
        try:
            self.startall()
        except BaseException:
            self.stop()
            raise
        return self

    def startall(self):
        r, w = os.pipe()
        try:
            self.spawn(["Xvfb", "-displayfd", str(w), "-screen", "0", f"{self.screen}x24",
                        "-nolisten", "tcp", "-noreset"], fds=(w,))
        finally:
            os.close(w)
        try:
            digits = readline(r, 15)
        finally:
            os.close(r)
        if not digits.isdigit():
            raise Failed("headless desktop: Xvfb did not start")
        display = ":" + digits

        conf = os.path.join(self.dir, "bus.conf")
        with open(conf, "w") as f:
            f.write(BUSCONF.format(path=os.path.join(self.dir, "bus")))
        r, w = os.pipe()
        try:
            self.spawn(["dbus-daemon", f"--config-file={conf}", "--nofork",
                        f"--print-address={w}"], fds=(w,))
        finally:
            os.close(w)
        try:
            bus = readline(r, 15)
        finally:
            os.close(r)
        if not bus.startswith("unix:"):
            raise Failed("headless desktop: dbus-daemon did not start")

        runtime = os.environ.get("XDG_RUNTIME_DIR", "")
        if not self.keepruntime:
            runtime = os.path.join(self.dir, "run")
            os.mkdir(runtime, 0o700)
        env = os.environ
        for k in ("WAYLAND_DISPLAY", "XAUTHORITY"):
            env.pop(k, None)
        env.update(DISPLAY=display, XDG_SESSION_TYPE="x11", SDL_VIDEO_DRIVER="x11",
                   GDK_BACKEND="x11", NO_AT_BRIDGE="1", DBUS_SESSION_BUS_ADDRESS=bus,
                   XDG_RUNTIME_DIR=runtime)
        # llvmpipe's threads, one per processor each by default, times N
        env.setdefault("LP_NUM_THREADS", str(max(2, (os.cpu_count() or 2) // self.slots)))

        if self.wm != "none":
            argv = (["openbox", "--config-file", "/etc/xdg/openbox/rc.xml", "--sm-disable"]
                    if self.wm == "openbox" else shlex.split(self.wm))
            self.spawn(argv, env=dict(env))
            deadline = time.monotonic() + 15
            while not self.wmname():
                if time.monotonic() > deadline or self.procs[-1].poll() is not None:
                    raise Failed(f"headless desktop: {argv[0]} did not start")
                time.sleep(0.2)
        print(f"headless desktop: {display} {self.screen}, window manager "
              f"{self.wmname() or 'none'}, bus {bus}", flush=True)

    def wmname(self):
        r = subprocess.run([sys.executable, "-I", XPROBE, "wmname"], capture_output=True,
                           text=True, timeout=10)
        return r.stdout.strip()

    def stop(self):
        # Our own children, not yet reaped, so their pids are theirs
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()
        for p in reversed(self.procs):
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        self.procs = []
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir = None
