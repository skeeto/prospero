#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Real-model smoke test: a small coding task in a temporary git repo.

The agent is asked to add a function and a test for it, then run the
tests. Passes when the turn ends done, the files hold the new function
and its test, the tests pass when run here afterward, and the prompt
cache hits grow from step to step (llama.cpp timings.cache_n, or the
provider's cached tokens, which may grow in blocks), which shows the
request prefix stays byte-stable. Loading a router model may evict the one already loaded.

  uv run tests/smoke.py --harness build/harness [-e http://localhost/] [-m qwen-flash]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

CALC = '''def add(a, b):
    return a + b
'''

TEST = '''from calc import add


def test_add():
    assert add(2, 3) == 5


if __name__ == "__main__":
    test_add()
    print("all tests passed")
'''

PROMPT = ("In this repository, add a function multiply(a, b) to calc.py that "
          "returns the product, add a test_multiply function to test_calc.py "
          "and call it from the main block, then run the tests with "
          "`python3 test_calc.py` and make sure they pass.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--harness", required=True)
    ap.add_argument("-e", default="http://localhost/")
    ap.add_argument("-m", default="qwen-flash")
    ap.add_argument("--keep", action="store_true", help="keep the temporary repo")
    args = ap.parse_args()

    root = tempfile.mkdtemp(prefix="harness-smoke-")
    repo = os.path.join(root, "repo")
    state = os.path.join(root, "state")
    os.makedirs(repo)
    with open(os.path.join(repo, "calc.py"), "w") as f:
        f.write(CALC)
    with open(os.path.join(repo, "test_calc.py"), "w") as f:
        f.write(TEST)
    git = ["git", "-C", repo, "-c", "user.name=smoke", "-c", "user.email=smoke@example.com"]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["add", "."], check=True)
    subprocess.run(git + ["commit", "-qm", "start"], check=True)

    t0 = time.monotonic()
    p = subprocess.run([os.path.abspath(args.harness), "run", "-C", repo,
                        "--state", state, "-e", args.e, "-m", args.m, PROMPT],
                       capture_output=True, text=True, timeout=3600)
    dt = time.monotonic() - t0
    events = [json.loads(line) for line in p.stdout.splitlines() if line.strip()]

    failures = []
    end = [e for e in events if e.get("ev") == "turn.end"]
    if p.returncode != 0 or not end or end[-1].get("why") != "done":
        failures.append(f"the turn did not end done: exit {p.returncode}, {end[-1:]}")
    calc = open(os.path.join(repo, "calc.py")).read()
    test = open(os.path.join(repo, "test_calc.py")).read()
    if "def multiply" not in calc:
        failures.append("calc.py has no multiply")
    if "multiply" not in test:
        failures.append("test_calc.py does not test multiply")
    run = subprocess.run([sys.executable, "test_calc.py"], cwd=repo,
                         capture_output=True, text=True)
    if run.returncode != 0:
        failures.append(f"the tests fail: {run.stdout}{run.stderr}")
    shells = [e for e in events if e.get("ev") == "tool.start" and e.get("name") == "shell"]
    if not any("test_calc" in e.get("args", "") for e in shells):
        failures.append("the agent never ran the tests")

    usage = [e.get("usage") for e in events if e.get("ev") == "assistant" and e.get("usage")]
    cached = [u.get("cached", 0) for u in usage]
    prompts = [u.get("in", 0) for u in usage]
    # llama.cpp reuses its cache token by token; hosted providers in
    # blocks (DeepSeek: 256 tokens), so between them a step may add
    # nothing to the cached prefix, but never lose any of it
    blocks = all(c % 64 == 0 for c in cached)
    if blocks:
        grows = (len(cached) >= 2 and all(c > 0 for c in cached[1:]) and
                 all(b >= a for a, b in zip(cached, cached[1:])) and
                 cached[-1] > cached[1] if len(cached) > 2 else True)
    else:
        grows = len(cached) >= 2 and all(b > a for a, b in zip(cached, cached[1:]))
    if not grows:
        failures.append(f"cached tokens do not grow from step to step: {cached}")

    tools = [e for e in events if e.get("ev") == "tool.end"]
    print(f"model {args.m}: {len(usage)} steps, {len(tools)} tools, {dt:.0f} s")
    print(f"  prompt tokens per step: {prompts}")
    print(f"  cached tokens per step: {cached}" + (" (block-granular)" if blocks else ""))
    for e in events:
        if e.get("ev") in ("retry", "nudge", "note"):
            print(f"  {e['ev']}: {e.get('msg') or e.get('kind') or e.get('text')}")
    for e in tools:
        print(f"  tool {e.get('status')}: {e.get('text', '').splitlines()[:1]}")
    if failures:
        print("FAIL")
        for f in failures:
            print("  " + f)
        print(f"  repo kept at {repo}")
        return 1
    print("ok")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print(f"  repo at {repo}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
