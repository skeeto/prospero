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

On Linux without a display it runs under xvfb-run. --serve only runs the
provider and writes the configuration, for a GUI started elsewhere (a
Windows console session): it prints the directories and waits.
"""

import argparse
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402


def config(url, model):
    return (
        "# The GUI checklist's configuration\n"
        f"endpoint = fake\n"
        f"model = {model}\n"
        "\n"
        "# The fake provider\n"
        "[endpoint fake]\n"
        f"url = {url}/\n"
        "first_byte_timeout = 10\n"
        "idle_timeout = 10\n"
        "progress_timeout = 20\n"
        "connect_timeout = 2\n"
        "\n"
        "# Asks wait briefly\n"
        "[guard]\n"
        "ask_timeout = 3\n"
    )


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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True)
    ap.add_argument("--subprocess", action="store_true",
                    help="the engine as harness --headless")
    ap.add_argument("--inbox-cap", type=int, default=65536,
                    help="the GUI's inbox, small to force lost records")
    ap.add_argument("--mode", default="drive", choices=["drive", "idle", "streamtest"])
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--keep", action="store_true", help="keep the directories")
    ap.add_argument("--serve", action="store_true",
                    help="only run the provider and write the files")
    ap.add_argument("--url", help="the provider's URL as the GUI reaches it")
    args = ap.parse_args()

    srv, url = fake_provider.start()
    nonce = f"{random.randrange(1 << 32):08x}"
    model = f"fake:gui:{nonce}"
    fake_provider.STATE.register(model)
    root = tempfile.mkdtemp(prefix="harness-gui-")
    work = os.path.join(root, "work")
    state = os.path.join(root, "state")
    ini = os.path.join(root, "harness.ini")
    os.makedirs(work)
    os.makedirs(state)
    with open(os.path.join(work, "hello.txt"), "w") as f:
        f.write("hello world\n")
    with open(ini, "w") as f:
        f.write(config(args.url or url, model))
    before = open(ini).read()

    if args.serve:
        print(f"url {url}\nmodel {model}\nroot {root}", flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
        return 0

    cmd = [os.path.abspath(args.harness), "gui", f"--{args.mode}",
           "-c", ini, "--statedir", state, "--cwd", work,
           "--inbox-cap", str(args.inbox_cap)]
    if args.subprocess:
        cmd.append("--engine=subprocess")
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") \
            and not os.environ.get("WAYLAND_DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1600x1000x24"] + cmd

    print(" ".join(cmd), flush=True)
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, cwd=work, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    lines = []
    try:
        deadline = t0 + args.timeout
        for line in p.stdout:
            lines.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()
            if time.monotonic() > deadline:
                break
        p.wait(timeout=max(1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        pass
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        print("FAIL: the harness did not exit in time")
        return 1
    dt = time.monotonic() - t0

    problems = []
    if p.returncode != 0:
        problems.append(f"exit status {p.returncode}")
    out = "".join(lines)
    if args.mode == "drive":
        summary = [l for l in lines if "drive:" in l and " checks, " in l]
        if not summary:
            problems.append("no checklist summary")
        else:
            s = summary[-1]
            if " 0 failed" not in s or " 0 stale" not in s:
                problems.append("checklist: " + s.strip())
        if "STALE" in out:
            problems.append("stale frames reported")
        after = open(ini).read()
        changed = [(a, b) for a, b in zip(before.splitlines(), after.splitlines()) if a != b]
        if len(before.splitlines()) != len(after.splitlines()) or len(changed) != 1:
            problems.append(f"the configuration changed in {len(changed)} lines")
        else:
            print(f"configuration diff: {changed[0][0]!r} -> {changed[0][1]!r}")
        # Gone: the cancelled command, and the one quitting stopped
        time.sleep(0.5)
        for name in ("cancel", "close"):
            pid = readpid(os.path.join(work, f"{name}.pid"))
            if pid is None:
                problems.append(f"no {name}.pid")
            elif alive(pid):
                problems.append(f"the {name} command (pid {pid}) is still running")
            else:
                print(f"{name}: pid {pid} gone")
    left = strays(root)
    if left:
        problems.append("processes left: " + "; ".join(left))
        for line in left:
            try:
                os.kill(int(line.split()[0]), signal.SIGKILL)
            except (OSError, ValueError):
                pass

    srv.shutdown()
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print(f"kept {root}")
    if problems:
        for msg in problems:
            print("FAIL: " + msg)
        return 1
    print(f"ok: {args.mode}{' (subprocess)' if args.subprocess else ''} in {dt:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
