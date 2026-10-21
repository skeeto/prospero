#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Process cleanup: crashes, and processes that escape their group.

A shell tool starts three levels of processes, each recording its pid.
Then the harness is killed with SIGKILL (POSIX) or taskkill /F (Windows),
so nothing in the harness gets to clean up: the reapers' lifeline (POSIX)
or the kill-on-close jobs (Windows) must take every process down within
5 s. On POSIX, a process that calls setsid() leaves its process group,
and still the reaper's sweep must find it: after a crash, and when its
command ends normally. So must daemons that detach through a double
fork, a chain of forks, fifty at once, or from a harness run as a tool,
which the Linux subreaper finds as its children. A daemon in a group of
its own, instead of a session, stays in the reaper's session. An escapee
after thousands of short-lived processes checks that the macOS reaper's
snapshots forget the dead.

On macOS the reaper finds daemons by what they keep of their origin
(reap.cpp): the unique id of the parent that forked them, which leads
back to the tool if the reaper saw that parent, as it usually does,
watching for forks; and when the harness answers for itself, as an app
that LaunchServices started does, responsibility, which leads back
however short-lived the parents were. So there the cases run twice: as
the tests run, where whatever runs them is responsible for the harness,
and with the harness started disclaimed (disclaim.py), checked by
asking, with the identity its tools then answer to, and the selftest.
Expected failures (XFAIL): as the tests run, a daemon whose parents
were all gone before the reaper saw them, as a chain of forks may be on
a busy machine; and in both modes, one that disclaims itself after such
parents. Nothing here reads a place that privacy protects.

Survivors are killed afterward, by pids read one per line and
confirmed to be ours first (pids.py).

  uv run tests/cleanup.py --harness build/harness
  uv run tests/cleanup.py --harness PATH --host biron.local
  uv run tests/cleanup.py --harness PATH --xp 192.168.1.106
"""

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disclaim  # noqa: E402
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
    r = t.run(script, watch=watch, timeout=60)
    if "killed" not in state:
        return [], None, 0, f"the tool never started: {scenarios.compact(r.names())}"
    return state["pids"], state.get("harness"), state["killed"], None


def crashes(host, mode):
    """The harness killed hard while its tools run. Returns failures."""
    failed = 0
    t = scenarios.T(host, "tree", None)
    pids, hpid, killed, err = killtree(t, "tree", 3)
    if err:
        print(f"FAIL tree{mode}: {err}")
        failed += 1
    elif len(pids) != 3:
        print(f"FAIL tree{mode}: expected 3 pids, got {pids}")
        failed += 1
    else:
        gone = t.gone(pids + ([hpid] if hpid else []), within=5.0)
        dt = time.monotonic() - killed
        left = [p for p in pids if host.alive(p)]
        if gone:
            print(f"ok   tree{mode}: harness {hpid} and pids {pids} gone within {dt:.2f} s")
        else:
            print(f"FAIL tree{mode}: still alive after 5 s: {left}")
            failed += 1
            for p in left:
                host.killpid(p)

    if host.windows or not shutil.which("perl"):
        return failed
    t = scenarios.T(host, "escapee", None)
    pids, hpid, killed, err = killtree(t, "escapee", 2)
    if err or len(pids) != 2:
        print(f"FAIL escapee{mode}: {err or pids}")
        return failed + 1
    group, escapee = pids[0], pids[1]
    gone = t.gone([group, escapee], within=5.0)
    dt = time.monotonic() - killed
    if host.alive(group):
        print(f"FAIL escapee{mode}: the group member {group} survived")
        failed += 1
    elif not gone:
        print(f"FAIL escapee{mode}: pid {escapee} left its group with "
              f"setsid and survived")
        failed += 1
    else:
        print(f"ok   escapee{mode}: the setsid escapee {escapee} is gone "
              f"within {dt:.2f} s")
    for p in [group, escapee]:
        if host.alive(p):
            host.killpid(p)
    return failed


# Daemons left behind by commands that end normally, each recording its
# pid in tree.pids. Those with a script run it through the fake model
# "script", as script.sh, with these replaced: @PERL@ the body of a
# daemon, @SCRIPTS@ this directory, @PYTHON@ this Python.

DAEMON = r'open(F, ">>tree.pids"); print F "$$\n"; close F; sleep 300'

# Four middle processes, each gone as soon as it forks the next
CHAIN = r"""perl -e 'use POSIX; exit if fork; setsid(); for (1..4) { exit if fork } @PERL@' >/dev/null 2>&1
sleep 0.5; echo forked
"""

# Fifty double forks, one after another, from one process
RAPID = r"""perl -e 'use POSIX; for (1..50) { my $p = fork; if (!$p) { setsid(); exit if fork; @PERL@; exit } waitpid($p, 0) }' >/dev/null 2>&1
sleep 0.5; echo forked
"""

# The chain, whose end then starts the daemon disclaimed: its own
# responsible process, and its parent's id unknown to the reaper
SELF = r"""echo '@PERL@' >daemon.pl
perl -e 'use POSIX; exit if fork; setsid(); for (1..4) { exit if fork } exec @ARGV' @PYTHON@ -I @SCRIPTS@/disclaim.py spawn perl daemon.pl >/dev/null 2>&1
sleep 1; echo forked
"""


def cases():
    """(name, the script, or None for a fake script of that name, the
    daemons expected, why an escape is a known hole as the tests run, or
    None, and why with the harness disclaimed). Holes are macOS's."""
    mac = sys.platform == "darwin"
    unseen = "its parents were gone before the reaper saw them" if mac else None
    r = [("escapee_end", None, 1, None, None),
         ("escapee_many", None, 1, None, None),
         ("doublefork", None, 1, unseen, None),
         ("groupfork", None, 1, None, None),
         ("chain", CHAIN, 1, unseen, None),
         ("rapid", RAPID, 50, unseen, None),
         ("nested", None, 1, unseen, None)]
    if mac:
        why = "it disclaimed itself after parents gone unseen"
        r.append(("selfdisclaim", SELF, 1, why, why))
    return r


def nested(t, host):
    """script.sh for a harness run as a tool, whose own tool leaves the
    chain's daemon behind. Returns the files where the outer and inner
    reapers' pids go."""
    inner = host.join(t.work, "inner")
    host.mkdir(inner)
    model = f"fake:script:{t.nonce}x"
    fake_provider.STATE.register(model)
    host.write(host.join(inner, "script.sh"),
               "echo $PPID >../inner.reaper\nsleep 2\n" +
               CHAIN.replace("@PERL@", DAEMON.replace(">>tree.pids", ">>../tree.pids")))
    args = ["run", "-C", inner, "--state", host.join(t.base, "inner-state"),
            "-c", t.config, "-e", "fake", "-m", model, "go"]
    t.put("script.sh", "echo $PPID >outer.reaper\n" +
          " ".join(shlex.quote(a) for a in [host.harness] + args) +
          " >inner.out 2>&1\necho inner exit $?\n")
    return "outer.reaper", "inner.reaper"


def responsibility(t, files, disclaimed):
    """A watch for t.run: once the tool starts, the pids in files as soon
    as all are there, each with the pid of its responsible process, then
    the harness's. Returns the watch and the dict that it fills in."""
    seen = {}

    def watch(ev, p):
        if ev.get("ev") != "tool.start" or seen:
            return False
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            pids = [t.pids(f) for f in files]
            if all(pids):
                break
            time.sleep(0.05)
        seen["harness"] = (p.pid, disclaim.responsible(p.pid))
        for f in files:
            pid = (t.pids(f) or [0])[0]
            seen[f] = (pid, disclaim.responsible(pid) if pid else 0)
        if disclaimed:
            pid = seen.get("reaper.pid", (0, 0))[0]
            seen["path"] = disclaim.pidpath(pid) if pid else None
            seen["cdhash"] = disclaim.cdhash(pid) if pid else None
        return False
    return watch, seen


def identity(host, disclaimed, mode):
    """Whom a tool answers to, by responsibility: as the tests run, what
    the harness answers to, as Terminal's grants are its tools' then;
    disclaimed, its reaper, the harness binary itself, as the harness
    answers for itself. Returns failures, and 1 for each in a wrong
    mode, after which the mode's cases would test nothing."""
    t = scenarios.T(host, "script", None)
    t.put("script.sh", "echo $PPID >reaper.pid; echo $$ >tool.pid; sleep 3; echo done\n")
    watch, seen = responsibility(t, ["reaper.pid", "tool.pid"], disclaimed)
    r = t.run(watch=watch, timeout=60)
    hpid, hresp = seen.get("harness", (0, 0))
    reaper, rresp = seen.get("reaper.pid", (0, 0))
    tool, tresp = seen.get("tool.pid", (0, 0))
    problems = []
    if r.rc or not reaper or not tool:
        problems.append(f"exit {r.rc}, reaper {reaper}, tool {tool}: {r.stderr[-500:]}")
    elif disclaimed:
        if hresp != hpid:
            problems.append(f"the harness {hpid} answers to {hresp}, not itself")
        if rresp != reaper or tresp != reaper:
            problems.append(f"the reaper {reaper} answers to {rresp}, its tool to {tresp}")
        exe = os.path.realpath(host.harness)
        if seen.get("path") != exe:
            problems.append(f"the reaper runs {seen.get('path')}, not {exe}")
        want = disclaim.cdhash(exe)
        if not want or seen.get("cdhash") != want:
            problems.append(f"the reaper's code is {seen.get('cdhash')}, the harness's {want}")
    else:
        if not hresp or hresp == hpid:
            problems.append(f"the harness {hpid} answers to {hresp}")
        if rresp != hresp or tresp != hresp:
            problems.append(f"the reaper {reaper} answers to {rresp}, its tool to "
                            f"{tresp}, not to the harness's {hresp}")
    if problems:
        print(f"FAIL identity{mode}: " + "; ".join(problems))
        return 1
    if disclaimed:
        print(f"ok   identity{mode}: the harness {hpid} answers for itself, its "
              f"reaper {reaper} for its tool {tool}, as {seen['cdhash'][:12]}, the "
              f"harness's own code")
    else:
        print(f"ok   identity{mode}: the harness {hpid}, its reaper {reaper} and "
              f"tool {tool} answer to {hresp}")
    return 0


def ends(host, disclaimed, mode):
    """Daemons left by commands that end normally. Returns failures."""
    failed = 0
    for name, script, want, hole, holed in cases():
        hole = holed if disclaimed else hole
        files = []
        if name == "nested" or script:
            t = scenarios.T(host, "script", None)
        else:
            t = scenarios.T(host, name, None)
        if name == "nested":
            files = nested(t, host)
        elif script:
            body = (script.replace("@PERL@", DAEMON)
                    .replace("@SCRIPTS@", shlex.quote(os.path.dirname(disclaim.__file__)))
                    .replace("@PYTHON@", shlex.quote(sys.executable)))
            t.put("script.sh", body)
        t.put("tree.pids", "")
        watch, seen = (responsibility(t, files, disclaimed) if files and
                       disclaim.available() else (None, {}))
        r = t.run(timeout=120, watch=watch)
        pids = t.pids("tree.pids")
        if r.rc != 0 or len(pids) != want:
            print(f"FAIL {name}{mode}: exit {r.rc}, {len(pids)} pids of {want}: "
                  f"{pids[:8]} {r.stderr[-500:]}")
            failed += 1
            for p in pids:
                if host.alive(p):
                    host.killpid(p)
            continue

        nest = ""
        if name == "nested" and seen:
            # A nested harness answers for nothing, as its parent harness
            # would lose its sweep of the inner tools otherwise
            outer, oresp = seen.get(files[0], (0, 0))
            inner, iresp = seen.get(files[1], (0, 0))
            hresp = seen["harness"][1]
            want_resp = outer if disclaimed else hresp
            if not inner or iresp != want_resp:
                print(f"FAIL {name}{mode}: the inner reaper {inner} answers to "
                      f"{iresp}, not {want_resp}")
                failed += 1
            nest = f", the inner reaper {inner} answering to {iresp}"

        gone = t.gone(pids, within=5.0)
        left = [p for p in pids if host.alive(p)]
        if gone:
            what = f"pid {pids[0]} is" if want == 1 else f"all {want} pids are"
            print(f"ok   {name}{mode}: {what} gone after the command{nest}")
        elif hole:
            print(f"XFAIL {name}{mode}: {len(left)} of {want} survived, as "
                  f"{left[0]}: {hole} (a known hole)")
        else:
            print(f"FAIL {name}{mode}: {len(left)} of {want} survived the "
                  f"command, as {left[:8]}{nest}")
            failed += 1
        for p in left:
            host.killpid(p)
    return failed


def selftest(host, mode):
    """The selftest with the harness disclaimed: every process test, and
    its own check of whom its tools answer to."""
    try:
        r = subprocess.run(host.prefix + [host.harness, "--selftest"],
                           capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        print(f"FAIL selftest{mode}: still running after 300 s")
        return 1
    fails = [x for x in r.stderr.splitlines() if "FAIL" in x]
    if r.returncode or fails or "each reaper answers for its tool" not in r.stderr:
        print(f"FAIL selftest{mode}: exit {r.returncode}: {fails[:4] or r.stderr[-500:]}")
        return 1
    print(f"ok   selftest{mode}: every check passed, each reaper answering for its tool")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True)
    ap.add_argument("--host", help="a Windows host to run on over ssh")
    ap.add_argument("--xp", help="a Windows XP host to run on over ssh")
    args = ap.parse_args()

    if args.xp:
        srv, url = fake_provider.start(scenarios.lanaddress(args.xp))
    else:
        srv, url = fake_provider.start()
    port = srv.server_address[1]
    if args.xp:
        host = scenarios.XpHost(args.xp, args.harness, url)
    elif args.host:
        host = scenarios.SshHost(args.host, args.harness, port, 20000 + port % 20000)
    else:
        host = scenarios.LocalHost(args.harness, url)

    failed = 0
    try:
        failed += crashes(host, "")
        if not host.windows and shutil.which("perl"):
            mac = disclaim.available()
            if mac:
                failed += identity(host, False, "")
            failed += ends(host, False, "")
            if mac:
                # As an app that LaunchServices started: answering for itself
                host.prefix = disclaim.launcher()
                mode = ", disclaimed"
                if identity(host, True, mode):
                    failed += 1
                else:
                    failed += crashes(host, mode)
                    failed += ends(host, True, mode)
                    failed += selftest(host, mode)
    finally:
        fake_provider.STATE.stop.set()
        host.close()
        srv.shutdown()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
