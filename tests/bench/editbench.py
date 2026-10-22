#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Edit-format benchmark: does a model fix a one-line bug, byte for byte?

Each task takes a real source file (some of this repository's src/*.cpp,
and the Python and JavaScript fixtures here), applies one mutation to it
(an operator swapped, a literal changed, an identifier renamed at one
site, a line deleted, a line duplicated, or two arguments swapped), and
asks the agent, through "prospero run", to fix the bug that one sentence
describes. The run succeeds when the file is byte for byte the original
afterward. The agent gets only read, edit, grep and glob (role "bench"),
so that every change goes through the edit tool, in the format that
[model] edit_format selects.

  uv run tests/bench/editbench.py tasks [--count 40] [--seed 1]
  uv run tests/bench/editbench.py run --prospero build/prospero \\
      --models MODEL,... --formats replace,patch,hashline \\
      --count 40 --runs 2 --jobs 4 --out results.jsonl
  uv run tests/bench/editbench.py report --out results.jsonl
  uv run tests/bench/editbench.py run ... --only t15-delete,t21-delete

run appends one JSON line per run to --out and skips runs already there,
so an interrupted run continues where it stopped. --keep DIR keeps each
run's JSONL output.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Files mutated: this repository's own code, and the fixtures
SOURCES = [
    "src/wide.cpp", "src/mailbox.cpp", "src/sse.cpp", "src/admit.cpp",
    "src/sched.cpp", "src/instructions.cpp", "src/compact.cpp", "src/guard.cpp",
    "src/base.cpp", "src/json.cpp", "src/doc.cpp", "src/layout.cpp",
    "tests/bench/fixtures/ledger.py", "tests/bench/fixtures/textutil.py",
    "tests/bench/fixtures/caches.py", "tests/bench/fixtures/router.js",
    "tests/bench/fixtures/jobs.js", "tests/bench/fixtures/csv.js",
]

KINDS = ["operator", "literal", "identifier", "delete", "duplicate", "swap"]

KEYWORDS = {
    "cpp": set("""alignas alignof auto bool break case catch char class const
        constexpr continue decltype default delete do double else enum explicit
        extern false float for goto if inline int long namespace new noexcept
        operator private protected public register return short signed sizeof
        static static_assert struct switch template this throw true try typedef
        typename union unsigned using virtual void volatile while u8 i32 u32 i64
        u64 iz uz b32 f32 f64 byte Str""".split()),
    "py": set("""False None True and as assert async await break class continue
        def del elif else except finally for from global if import in is lambda
        nonlocal not or pass raise return try while with yield self cls len
        str int list dict set tuple range print""".split()),
    "js": set("""break case catch class const continue debugger default delete do
        else export extends false finally for function if import in instanceof
        let new null return super switch this throw true try typeof undefined
        var void while with yield async await of""".split()),
}

# Operators swapped: (pattern, replacement) of what the original has
OPS = {
    "cpp": [(r"(?<![=!<>])==(?!=)", "!="), (r"(?<![=!])!=(?!=)", "=="),
            (r"(?<![<>=!])<=(?![=>])", "<"), (r"(?<![<>=!-])>=(?!=)", ">"),
            (r"(?<=\s)<(?=\s)", "<="), (r"(?<=\s)>(?=\s)", ">="),
            (r"&&", "||"), (r"\|\|", "&&"),
            (r"(?<=\s)\+(?=\s)", "-"), (r"(?<=\s)-(?=\s)", "+")],
    "js": [(r"===", "!=="), (r"!==", "==="),
           (r"(?<![<>=!])<=(?![=>])", "<"), (r"(?<![<>=!=])>=(?!=)", ">"),
           (r"(?<=\s)<(?=\s)", "<="), (r"(?<=\s)>(?=\s)", ">="),
           (r"&&", "||"), (r"\|\|", "&&"),
           (r"(?<=\s)\+(?=\s)", "-"), (r"(?<=\s)-(?=\s)", "+")],
    "py": [(r"(?<=\s)==(?=\s)", "!="), (r"(?<=\s)!=(?=\s)", "=="),
           (r"(?<=\s)<=(?=\s)", "<"), (r"(?<=\s)>=(?=\s)", ">"),
           (r"(?<=\s)<(?=\s)", "<="), (r"(?<=\s)>(?=\s)", ">="),
           (r"(?<=\s)and(?=\s)", "or"), (r"(?<=\s)or(?=\s)", "and"),
           (r"(?<=\s)\+(?=\s)", "-"), (r"(?<=\s)-(?=\s)", "+")],
}


def language(path):
    return {".py": "py", ".js": "js"}.get(os.path.splitext(path)[1], "cpp")


# Finding functions

def function_of(lang, lines, i):
    """The name of the function line i is in, or None."""
    indent = len(lines[i]) - len(lines[i].lstrip())
    for k in range(i - 1, -1, -1):
        line = lines[k]
        if lang == "cpp":
            if line.startswith("}"):
                return None  # between functions
            m = re.match(r"^(?:static |template)[^;]*?\b(\w+)\s*\(", line)
            if m and not line.rstrip().endswith(";"):
                return m.group(1)
        elif lang == "py":
            m = re.match(r"^(\s*)def (\w+)\(", line)
            if m and len(m.group(1)) < indent:
                return m.group(2)
        else:
            m = (re.match(r"^\s*function (\w+)\(", line) or
                 re.match(r"^\s*(?:async )?(\w+)\s*\([^)]*\)\s*\{\s*$", line) or
                 re.match(r"^\s*(?:get |set )(\w+)\s*\(", line))
            if m and m.group(1) not in KEYWORDS["js"]:
                lead = len(line) - len(line.lstrip())
                if lead < indent:
                    return m.group(1)
            if re.match(r"^(class|module)\b", line):
                return None
    return None


# Mutations: each candidate is (kind, line index, new lines for that
# index, the sentence describing the bug)

def plain(line):
    """A line fit to quote and mutate: code, not a comment or a string."""
    s = line.strip()
    return (8 <= len(s) <= 100 and not s.startswith(("//", "#", "*", "/*")) and
            '"' not in s and "'" not in s and "`" not in s and "//" not in s and
            " # " not in s)


def count_tok(pattern, s):
    return len(re.findall(pattern, s))


def statement(lang, line):
    s = line.strip()
    if not plain(line) or s.startswith(("for ", "for(", "if ", "if(", "else", "case ")):
        return False
    if lang == "py":
        return not s.endswith((":", ",", "(", "[", "{")) and ("=" in s or "(" in s) \
            and not s.startswith(("def ", "class ", "@", "return", ")", "]", "}"))
    return s.endswith(";") and not s.startswith(("return", "}", "break", "continue"))


def candidates(path, text, rng):
    lang = language(path)
    lines = text.split("\n")
    stripped = collections.Counter(l.strip() for l in lines)
    out = []

    def unique_after(newlines, i):
        new = lines[:i] + newlines + lines[i + 1:]
        counts = collections.Counter(l.strip() for l in new)
        return all(counts[l.strip()] == 1 for l in newlines)

    for i, line in enumerate(lines):
        func = function_of(lang, lines, i)
        if func is None or not plain(line) or line.startswith("#"):
            continue
        indent = line[:len(line) - len(line.lstrip())]
        where = f"In `{path}`, in `{func}`,"

        # An operator swapped
        for pat, rep in OPS[lang]:
            if count_tok(pat, line) != 1:
                continue
            orig = re.search(pat, line).group(0)
            new = re.sub(pat, rep, line, count=1)
            back = [p for p, r in OPS[lang] if r == orig]
            if new == line or not back or count_tok(back[0], new) != 1:
                continue
            if unique_after([new], i):
                out.append(("operator", i, [new],
                            f"{where} the line `{new.strip()}` uses `{rep}` where the "
                            f"correct code uses `{orig}`."))

        # A literal changed
        nums = re.findall(r"(?<![\w.])(\d+)(?![\w.])", line)
        for num in set(nums):
            if nums.count(num) != 1 or len(num) > 6:
                continue
            new = re.sub(rf"(?<![\w.]){num}(?![\w.])", str(int(num) + 1), line, count=1)
            if str(int(num) + 1) in re.findall(r"(?<![\w.])(\d+)(?![\w.])", line):
                continue
            if unique_after([new], i):
                out.append(("literal", i, [new],
                            f"{where} the line `{new.strip()}` has the number "
                            f"`{int(num) + 1}` where the correct code has `{num}`."))

        # An identifier renamed at this one site, to another of the function's
        idents = re.findall(r"\b[A-Za-z_]\w*\b", line)
        body = []
        for k in range(max(0, i - 40), min(len(lines), i + 40)):
            if function_of(lang, lines, k) == func:
                body.extend(re.findall(r"\b[A-Za-z_]\w{2,}\b", lines[k]))
        pool = sorted(set(w for w in body if w not in KEYWORDS[lang]))
        for name in set(idents):
            if (len(name) < 3 or name in KEYWORDS[lang] or idents.count(name) != 1 or
                    name == func):
                continue
            others = [w for w in pool if w != name and w not in idents]
            if not others:
                continue
            other = rng.choice(others)
            new = re.sub(rf"\b{name}\b", other, line, count=1)
            if unique_after([new], i):
                out.append(("identifier", i, [new],
                            f"{where} the line `{new.strip()}` uses `{other}` where the "
                            f"correct code uses `{name}`."))

        # A statement deleted, or duplicated
        if statement(lang, line) and stripped[line.strip()] == 1:
            prev = lines[i - 1] if i > 0 else ""
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            lead = lambda s: s[:len(s) - len(s.lstrip())]
            if (prev.strip() and stripped[prev.strip()] == 1 and
                    indent in (lead(prev), lead(nxt)) and not
                    (lang == "py" and prev.rstrip().endswith(":") and
                     len(lead(nxt)) < len(indent))):
                out.append(("delete", i, [],
                            f"{where} a line is missing: `{line.strip()}` belongs right "
                            f"after the line `{prev.strip()}`."))
            out.append(("duplicate", i, [line, line],
                        f"{where} the line `{line.strip()}` appears twice in a row, "
                        f"where the correct code has it once."))

        # The first two arguments of a call swapped
        if indent and not line.lstrip().startswith(("def ", "function ", "static ")):
            for m in re.finditer(r"\b(\w+)\(([^(),]+),(\s*)([^(),]+)([,)])", line):
                name, a, sp, b = m.group(1), m.group(2), m.group(3), m.group(4)
                if (name in KEYWORDS[lang] or name in ("if", "for", "while", "switch") or
                        a.strip() == b.strip() or line.count(name + "(") != 1 or
                        not a.strip() or not b.strip() or a != a.strip()):
                    continue
                swapped = f"{name}({b.strip()},{sp}{a}{b[len(b.rstrip()):]}{m.group(5)}"
                new = line[:m.start()] + swapped + line[m.end():]
                if new != line and unique_after([new], i):
                    out.append(("swap", i, [new],
                                f"{where} the line `{new.strip()}` passes the first two "
                                f"arguments of `{name}` in the wrong order."))
    return out


def make_tasks(count, seed):
    """count tasks, the kinds in turn, each from a file picked at random."""
    rng = random.Random(seed)
    texts = {}
    for p in SOURCES:
        with open(os.path.join(REPO, p), "rb") as f:
            texts[p] = f.read().decode("utf-8")
    pools = {p: candidates(p, texts[p], random.Random(f"{seed}:{p}")) for p in SOURCES}
    tasks = []
    used = set()
    for n in range(count):
        kind = KINDS[n % len(KINDS)]
        for _ in range(1000):
            p = rng.choice(SOURCES)
            pool = [c for c in pools[p] if c[0] == kind and (p, c[1]) not in used]
            if pool:
                break
        k, i, newlines, desc = rng.choice(pool)
        used.add((p, i))
        lines = texts[p].split("\n")
        mutated = "\n".join(lines[:i] + newlines + lines[i + 1:])
        rel = p if p.startswith("src/") else os.path.basename(p)
        desc = desc.replace(f"`{p}`", f"`{rel}`")
        tasks.append({"id": f"t{n:02d}-{kind}", "kind": kind, "source": p, "path": rel,
                      "line": i + 1, "desc": desc, "original": texts[p], "mutated": mutated})
    return tasks


def prompt_of(task):
    return (f"There is a bug in this project. {task['desc']} Fix it with the smallest "
            f"change, and leave the rest of the file exactly as it is. You do not "
            f"need to build, run or test anything.")


# Running

CONFIG = """endpoint = bench

[endpoint bench]
url = {url}

[model {model}]
edit_format = {format}

[role bench]
description = Fixes one bug with the file tools
tools = read edit grep glob

[instructions]
files = NO-INSTRUCTIONS-FOR-THE-BENCHMARK.md
"""


def run_one(args, root, task, model, fmt, run):
    base = tempfile.mkdtemp(prefix=f"{task['id']}-", dir=root)
    work = os.path.join(base, "work")
    state = os.path.join(base, "state")
    os.makedirs(os.path.join(work, os.path.dirname(task["path"])), exist_ok=True)
    os.makedirs(state)
    target = os.path.join(work, task["path"])
    with open(target, "wb") as f:
        f.write(task["mutated"].encode("utf-8"))
    config = os.path.join(base, "prospero.ini")
    with open(config, "w") as f:
        f.write(CONFIG.format(url=args.endpoint, model=model, format=fmt))
    cmd = [os.path.abspath(args.prospero), "run", "-C", work, "--state", state,
           "-c", config, "-e", "bench", "-m", model, "-r", "bench", prompt_of(task)]
    t0 = time.monotonic()
    timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
        out, rc = p.stdout, p.returncode
    except subprocess.TimeoutExpired as e:
        out, rc, timed_out = e.stdout or b"", -1, True
    wall = time.monotonic() - t0
    with open(target, "rb") as f:
        final = f.read()

    rec = {"model": model, "format": fmt, "task": task["id"], "kind": task["kind"],
           "lang": language(task["path"]), "run": run, "rc": rc, "timeout": timed_out,
           "wall": round(wall, 1), "ok": final == task["original"].encode("utf-8"),
           "unchanged": final == task["mutated"].encode("utf-8"),
           "calls": 0, "edits": 0, "edit_errors": 0, "reads": 0, "steps": 0,
           "in": 0, "out": 0, "cached": 0, "why": None}
    for line in out.decode("utf-8", "replace").splitlines():
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        name = ev.get("ev")
        if name == "tool.start":
            rec["calls"] += 1
            rec["edits"] += ev.get("name") == "edit"
            rec["reads"] += ev.get("name") == "read"
        elif name == "tool.end" and ev.get("status") != "ok":
            # Which call it ends: edits are the ones that matter
            rec.setdefault("_bad", []).append(ev.get("call"))
        elif name == "assistant":
            rec["steps"] += 1
            u = ev.get("usage") or {}
            rec["in"] += u.get("in", 0)
            rec["out"] += u.get("out", 0)
            rec["cached"] += u.get("cached", 0)
        elif name == "turn.end":
            rec["why"] = ev.get("why")
    starts = {}
    for line in out.decode("utf-8", "replace").splitlines():
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        if ev.get("ev") == "tool.start":
            starts[ev.get("call")] = ev.get("name")
    rec["edit_errors"] = sum(1 for c in rec.pop("_bad", []) if starts.get(c) == "edit")
    if args.keep:
        d = os.path.join(args.keep, model, fmt)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f"{task['id']}-{run}.jsonl"), "wb") as f:
            f.write(out)
    shutil.rmtree(base, ignore_errors=True)
    return rec


def cmd_run(args):
    tasks = make_tasks(args.count, args.seed)
    if args.only:
        tasks = [t for t in tasks if t["id"] in args.only.split(",")]
    done = set()
    if os.path.exists(args.out):
        with open(args.out) as f:
            for line in f:
                r = json.loads(line)
                done.add((r["model"], r["format"], r["task"], r["run"]))
    todo = []
    for run in range(args.runs):
        for model in args.models.split(","):
            for fmt in args.formats.split(","):
                for t in tasks:
                    if (model, fmt, t["id"], run) not in done:
                        todo.append((t, model, fmt, run))
    # Interleave models and formats, so that a partial run is balanced
    random.Random(args.seed).shuffle(todo)
    print(f"{len(todo)} runs to do, {len(done)} done before", file=sys.stderr)
    root = tempfile.mkdtemp(prefix="editbench-")
    lock = threading.Lock()
    finished = 0
    t0 = time.monotonic()
    try:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futs = [pool.submit(run_one, args, root, *x) for x in todo]
            for fut in concurrent.futures.as_completed(futs):
                rec = fut.result()
                with lock:
                    with open(args.out, "a") as f:
                        f.write(json.dumps(rec) + "\n")
                    finished += 1
                    rate = (time.monotonic() - t0) / finished
                    print(f"[{finished}/{len(todo)}] {rec['model']:<12} {rec['format']:<9} "
                          f"{rec['task']:<16} {'ok  ' if rec['ok'] else 'FAIL'} "
                          f"{rec['wall']:6.1f} s  {rec['calls']} calls  "
                          f"(~{rate * (len(todo) - finished) / 60:.0f} min left)",
                          file=sys.stderr, flush=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def cmd_tasks(args):
    for t in make_tasks(args.count, args.seed):
        print(f"{t['id']}  {t['path']}:{t['line']}\n  {prompt_of(t)}\n")


# Reporting

def mean(xs):
    xs = list(xs)
    return sum(xs) / len(xs) if xs else 0.0


def cmd_report(args):
    recs = []
    with open(args.out) as f:
        for line in f:
            recs.append(json.loads(line))
    cells = collections.defaultdict(list)
    for r in recs:
        cells[(r["model"], r["format"])].append(r)
    models = sorted({r["model"] for r in recs}, key=lambda m: (m != "large", m))
    formats = [f for f in ("replace", "patch", "hashline") if any(r["format"] == f for r in recs)]

    # As RESULTS.md has it: tool calls, reads, tokens and wall time are
    # means per run, failed edits totals per cell
    print("| model | format | runs | success | tool calls | reads | failed edits "
          "| tokens in | tokens out | wall s |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for m in models:
        for fmt in formats:
            rs = cells.get((m, fmt), [])
            if not rs:
                continue
            ok = sum(r["ok"] for r in rs)
            print(f"| {m} | {fmt} | {len(rs)} | {100 * ok / len(rs):.1f}% ({ok}) "
                  f"| {mean(r['calls'] for r in rs):.1f} | {mean(r['reads'] for r in rs):.2f} "
                  f"| {sum(r['edit_errors'] for r in rs)} "
                  f"| {mean(r['in'] for r in rs):,.0f} | {mean(r['out'] for r in rs):,.0f} "
                  f"| {mean(r['wall'] for r in rs):.0f} |")
    print()
    print("Success by mutation kind:\n")
    print("| model | format | " + " | ".join(KINDS) + " |")
    print("|---|---|" + "---:|" * len(KINDS))
    for m in models:
        for fmt in formats:
            rs = cells.get((m, fmt), [])
            if not rs:
                continue
            row = []
            for k in KINDS:
                kr = [r for r in rs if r["kind"] == k]
                row.append(f"{sum(r['ok'] for r in kr)}/{len(kr)}" if kr else "-")
            print(f"| {m} | {fmt} | " + " | ".join(row) + " |")
    print()
    print("Success by language:\n")
    langs = [l for l in ("cpp", "py", "js") if any(r["lang"] == l for r in recs)]
    print("| model | format | " + " | ".join(langs) + " |")
    print("|---|---|" + "---:|" * len(langs))
    for m in models:
        for fmt in formats:
            rs = cells.get((m, fmt), [])
            if not rs:
                continue
            row = []
            for l in langs:
                lr = [r for r in rs if r["lang"] == l]
                row.append(f"{sum(r['ok'] for r in lr)}/{len(lr)}" if lr else "-")
            print(f"| {m} | {fmt} | " + " | ".join(row) + " |")
    print()
    print("Failures: file left as mutated (no fix) / changed but wrong / timed out:\n")
    for m in models:
        for fmt in formats:
            rs = [r for r in cells.get((m, fmt), []) if not r["ok"]]
            if not rs:
                continue
            un = sum(r["unchanged"] for r in rs)
            to = sum(r["timeout"] for r in rs)
            print(f"- {m} {fmt}: {un} / {len(rs) - un - to} / {to}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tasks")
    r = sub.add_parser("run")
    p = sub.add_parser("report")
    for a in (t, r):
        a.add_argument("--count", type=int, default=40)
        a.add_argument("--seed", type=int, default=1)
    r.add_argument("--prospero", required=True)
    r.add_argument("--models", required=True, help="models, separated by commas")
    r.add_argument("--formats", default="replace,patch,hashline")
    r.add_argument("--runs", type=int, default=2)
    r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--timeout", type=float, default=600)
    r.add_argument("--endpoint", default="http://localhost/")
    r.add_argument("--keep", help="keep each run's output under this directory")
    r.add_argument("--only", help="only these tasks, by id, separated by commas")
    for a in (r, p):
        a.add_argument("--out", default="editbench.jsonl")
    args = ap.parse_args()
    {"tasks": cmd_tasks, "run": cmd_run, "report": cmd_report}[args.cmd](args)


if __name__ == "__main__":
    main()
