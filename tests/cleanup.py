#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Crash cleanup: kill the harness hard while a tool runs a process tree.

A shell tool starts three levels of processes, each recording its pid.
Then the harness is killed with SIGKILL (POSIX) or taskkill /F (Windows),
so nothing in the harness gets to clean up: the reapers' lifeline (POSIX)
or the kill-on-close jobs (Windows) must take every process down within
5 s. A process that calls setsid() leaves its process group, which no
POSIX mechanism here can follow until M4: that case is an expected
failure, reported as XFAIL, and its process is killed afterward.

  uv run tests/cleanup.py --harness build/harness
  uv run tests/cleanup.py --harness PATH --host biron.local
"""

import argparse
import os
import shutil
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_provider  # noqa: E402
import scenarios  # noqa: E402


def killtree(t, script, want):
    """Run the tree script until want pids are recorded, then kill the
    harness hard. Returns (pids, the harness pid if known, the time of
    the kill, error)."""
    state = {}

    def watch(ev, p):
        if ev.get("ev") != "tool.start":
            return False
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and len(t.pids("tree.pids")) < want:
            time.sleep(0.05)
        state["pids"] = t.pids("tree.pids")
        if t.host.windows:
            # The first shell's parent is harness.exe itself
            ppid = t.pids("tree.ppid")
            state["harness"] = ppid[0] if ppid else None
            t.host.killhard(p, state["harness"])
        else:
            state["harness"] = p.pid
            t.host.killhard(p)
        state["killed"] = time.monotonic()
        return True

    t.put("tree.pids", "")
    r = t.run("tree", watch=watch, timeout=60)
    if "killed" not in state:
        return [], None, 0, f"the tool never started: {scenarios.compact(r.names())}"
    return state["pids"], state.get("harness"), state["killed"], None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True)
    ap.add_argument("--host", help="a Windows host to run on over ssh")
    args = ap.parse_args()

    srv, url = fake_provider.start()
    port = srv.server_address[1]
    if args.host:
        host = scenarios.SshHost(args.host, args.harness, port, 20000 + port % 20000)
    else:
        host = scenarios.LocalHost(args.harness, url)

    failed = 0
    try:
        t = scenarios.T(host, "tree", None)
        pids, hpid, killed, err = killtree(t, "tree", 3)
        if err:
            print(f"FAIL tree: {err}")
            failed += 1
        elif len(pids) != 3:
            print(f"FAIL tree: expected 3 pids, got {pids}")
            failed += 1
        else:
            gone = t.gone(pids + ([hpid] if hpid else []), within=5.0)
            dt = time.monotonic() - killed
            left = [p for p in pids if host.alive(p)]
            if gone:
                print(f"ok   tree: harness {hpid} and pids {pids} gone within {dt:.2f} s")
            else:
                print(f"FAIL tree: still alive after 5 s: {left}")
                failed += 1
                for p in left:
                    host.killpid(p)

        if not host.windows and shutil.which("perl"):
            t = scenarios.T(host, "escapee", None)
            pids, hpid, _, err = killtree(t, "escapee", 2)
            if err or len(pids) != 2:
                print(f"FAIL escapee: {err or pids}")
                failed += 1
            else:
                group, escapee = pids[0], pids[1]
                t.gone([group], within=5.0)
                if host.alive(group):
                    print(f"FAIL escapee: the group member {group} survived")
                    failed += 1
                elif host.alive(escapee):
                    print(f"XFAIL escapee: pid {escapee} left its group with "
                          f"setsid and survived (expected until M4)")
                    host.killpid(escapee)
                else:
                    print("ok   escapee: even the setsid escapee is gone")
    finally:
        fake_provider.STATE.stop.set()
        host.close()
        srv.shutdown()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
