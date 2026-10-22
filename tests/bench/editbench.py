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
asks the agent to fix the bug that one sentence describes. The run
succeeds when the file is byte for byte the original afterward. The
agent gets only read, edit, grep and glob, so that every change goes
through the edit tool, in the format under test.

This script makes the tasks; "prospero eval" runs them, as the suite in
tests/bench/editbench (one task file and patch each, and a copy of each
source file as it was when the suite was made):

  uv run tests/bench/editbench.py tasks [--count 40] [--seed 1]
  uv run tests/bench/editbench.py suite [--count 40] [--seed 1] [--out DIR]
  prospero eval -m MODEL,... -f all -n 2 -j 4 -o results.jsonl tests/bench/editbench

The suite is kept as made, so that results stay comparable: making it
again from the sources as they are now changes the tasks.
"""

import argparse
import collections
import os
import random
import re
import shutil

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
        for num in sorted(set(nums)):  # sorted: sets follow the hash seed
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
        for name in sorted(set(idents)):
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


# The suite: each task as prospero eval runs it

SUITE_INI = """# The edit-format benchmark as a suite for prospero eval, made by
# tests/bench/editbench.py suite from the sources as they were then,
# which files/ keeps: regenerate it only to change the benchmark.
#
# Each task restores one line of one file that its patch broke, with
# the file tools alone, so that every change goes through the edit tool.
tools   = read edit grep glob
timeout = 600
steps   = 100
"""


def stem(path):
    return os.path.splitext(os.path.basename(path))[0]


def lines_of(text):
    """The lines of a text that ends in a newline, without it."""
    lines = text.split("\n")
    return lines[:-1] if lines and lines[-1] == "" else lines


def runs_at(lines, block):
    return sum(1 for i in range(len(lines) - len(block) + 1)
               if lines[i:i + len(block)] == block)


def patch_of(task):
    """The mutation as a patch in the edit tool's format (*** Begin Patch),
    with context enough around it to fit one place only."""
    orig = lines_of(task["original"])
    i = task["line"] - 1
    new = {"delete": [], "duplicate": [orig[i], orig[i]]}.get(task["kind"])
    if new is None:
        new = [lines_of(task["mutated"])[i]]
    for ctx in range(3, len(orig)):
        lo, hi = max(0, i - ctx), min(len(orig), i + 1 + ctx)
        if runs_at(orig, orig[lo:hi]) == 1:
            break
    else:
        raise SystemExit(f"{task['id']}: no unique context")
    body = ([" " + l for l in orig[lo:i]] + ["-" + orig[i]] + ["+" + l for l in new] +
            [" " + l for l in orig[i + 1:hi]])
    # Applied as the harness will, it must give the mutated file
    got = orig[:lo] + orig[lo:i] + new + orig[i + 1:hi] + orig[hi:]
    if "\n".join(got) + "\n" != task["mutated"]:
        raise SystemExit(f"{task['id']}: the patch does not give the mutated file")
    return ("*** Begin Patch\n*** Update File: " + task["path"] + "\n@@\n" +
            "\n".join(body) + "\n*** End Patch\n")


def cmd_suite(args):
    """Write the suite: suite.ini, a task file and a patch for each task,
    and files/STEM/PATH, a copy of each file the tasks change."""
    out = args.out
    if os.path.isdir(out):
        for name in os.listdir(out):
            if name.endswith((".ini", ".patch")):
                os.remove(os.path.join(out, name))
        shutil.rmtree(os.path.join(out, "files"), ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "suite.ini"), "w", newline="\n") as f:
        f.write(SUITE_INI)
    for t in make_tasks(args.count, args.seed):
        files = f"files/{stem(t['path'])}"
        dest = os.path.join(out, files, *t["path"].split("/"))
        if not os.path.exists(dest):
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(t["original"].encode("utf-8"))
        with open(os.path.join(out, t["id"] + ".patch"), "w", newline="\n") as f:
            f.write(patch_of(t))
        with open(os.path.join(out, t["id"] + ".ini"), "w", newline="\n") as f:
            f.write(f"# {t['kind']}: {t['path']}, line {t['line']}\n"
                    f"prompt  = \"{ini_quote(prompt_of(t))}\"\n"
                    f"fixture = {files}\n"
                    f"patch   = {t['id']}.patch\n"
                    f"expect  = {files}\n")
    print(f"wrote {args.count} tasks to {out}")


def ini_quote(s):
    """A value for a quoted INI value: only \\ and \" are escaped."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def cmd_tasks(args):
    for t in make_tasks(args.count, args.seed):
        print(f"{t['id']}  {t['path']}:{t['line']}\n  {prompt_of(t)}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tasks")
    s = sub.add_parser("suite")
    for a in (t, s):
        a.add_argument("--count", type=int, default=40)
        a.add_argument("--seed", type=int, default=1)
    s.add_argument("--out", default=os.path.join(HERE, "editbench"))
    args = ap.parse_args()
    {"tasks": cmd_tasks, "suite": cmd_suite}[args.cmd](args)


if __name__ == "__main__":
    main()
