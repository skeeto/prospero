#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Arena aliasing that the sources show: one arena handed over twice.

A callee taking (Arena *perm, Arena scratch) and given one arena as
both, f(&a, a) or f(p, *p), starts its scratch where its results go, so
its first allocation in scratch writes over results it made before (as
3814c05 fixed). A Buf in a struct bound to a local arena that is then
copied into that struct grows, once past its first block, through the
dead local (base.cpp's reserve). Each such call in src/ and src/test/ is
printed, as FILE:LINE: and why, and any fails the check. Nothing is
built: the sources are read with comments and literals blanked out.
The matcher first checks itself on snippets.

  uv run tests/arenalint.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))

# Words that can follow "Arena" in a declaration without naming it
NOTNAMES = {"const", "operator", "struct", "return", "volatile"}

DECL = re.compile(r"\bArena\b(?:\s+const\b)?\s*([*&]?)\s*(?:const\b\s*)?"
                  r"([A-Za-z_]\w*)\b(?!\s*\()")
PATH = re.compile(r"[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*")
BUFSET = re.compile(r"(?:->|\.)\s*\w+\s*=\s*Buf\s*\(\s*&\s*([A-Za-z_]\w*)\s*[,)]")


def strip(text):
    """Blank out comments and the insides of string and character
    literals, keeping every newline, so offsets keep their lines."""
    out = []
    i, n = 0, len(text)
    def blank(s):
        return re.sub(r"[^\n]", " ", s)
    while i < n:
        c = text[i]
        prev = text[i-1] if i else "\n"
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i+2)
            j = n if j < 0 else j+2
            out.append(blank(text[i:j]))
            i = j
        elif c == "R" and text.startswith('R"', i) and not (prev.isalnum() or prev == "_"):
            k = text.find("(", i+2)
            delim = text[i+2:k]
            j = text.find(")" + delim + '"', k)
            j = n if j < 0 else j + len(delim) + 2
            out.append('R"' + blank(text[i+2:j-1]) + '"')
            i = j
        elif c == '"' or (c == "'" and not prev.isdigit()):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + blank(text[i+1:j-1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def arenanames(stripped):
    """Names declared as an Arena (values and references) and as an
    Arena pointer, members included."""
    values, pointers = set(), set()
    for m in DECL.finditer(stripped):
        kind, name = m.groups()
        if name in NOTNAMES:
            continue
        (pointers if kind == "*" else values).add(name)
    return values, pointers


def calls(stripped):
    """Each parenthesized list, as (offset of its opening paren, its
    top-level arguments with blanks removed)."""
    stack = []
    for i, c in enumerate(stripped):
        if c in "([{":
            stack.append([c, i, [i+1]])
        elif c in ")]}":
            if not stack:
                continue
            kind, open_, starts = stack.pop()
            if kind == "(" and c == ")":
                bounds = starts + [i+1]
                args = [stripped[bounds[k]:bounds[k+1]-1] for k in range(len(bounds)-1)]
                yield open_, [unparen(re.sub(r"\s+", "", a)) for a in args]
        elif c == "," and stack:
            stack[-1][2].append(i+1)


def unparen(arg):
    while arg.startswith("(") and arg.endswith(")") and balanced(arg[1:-1]):
        arg = arg[1:-1]
    return arg


def balanced(s):
    depth = 0
    for c in s:
        depth += c == "("
        depth -= c == ")"
        if depth < 0:
            return False
    return depth == 0


def last(path):
    return re.split(r"\.|->", path)[-1]


def callee(stripped, open_):
    m = re.search(r"([A-Za-z_]\w*)\s*(?:<[^<>()]*>)?\s*$", stripped[max(0, open_-200):open_])
    return m.group(1) if m else "a call"


def lint(text, values, pointers):
    """Findings in one file's text, as (line, message)."""
    s = strip(text)
    found = []
    def line(off):
        return s.count("\n", 0, off) + 1
    for open_, args in calls(s):
        given = set(args)
        for a in args:
            if a.startswith("&") and a[1:] in given:
                e = a[1:]
                if PATH.fullmatch(e) and last(e) in values:
                    found.append((line(open_), f"{callee(s, open_)}() is given {e} both "
                                  f"as &{e} and by value"))
            elif a.startswith("*") and a[1:] in given:
                p = a[1:]
                if PATH.fullmatch(p) and last(p) in pointers:
                    found.append((line(open_), f"{callee(s, open_)}() is given {p} both "
                                  f"as {p} and as *{p}"))
    for m in BUFSET.finditer(s):
        local = m.group(1)
        if local not in values:
            continue
        beg = s.rfind("\n{\n", 0, m.start())
        end = s.find("\n}\n", m.end())
        beg = 0 if beg < 0 else beg
        end = len(s) if end < 0 else end
        copy = re.compile(r"(?:->|\.)\s*\w+\s*=\s*" + re.escape(local) + r"\s*;")
        c = copy.search(s, beg, end)
        if c:
            found.append((line(m.start()), f"a Buf in a struct is bound to the local "
                          f"arena {local}, which line {line(c.start())} copies into a "
                          f"struct: it grows through the dead local"))
    return sorted(set(found))


SNIPPETS = [
    # (snippet, number of findings)
    ("Arena a; f(&a, a);", 1),
    ("Arena *a; Str s = findpath(os, a,\n    *a, path, name, 1);", 1),
    ("Arena perm; g(&s->perm, x, s->perm);", 1),
    ("Arena *a; g(b->a, *b->a);", 1),
    ("Arena a; f((&a), (a));", 1),
    ("Arena x; farhosts(&x, o, farwsl(e, x));", 0),
    ("Arena scratch; shellat(os, scratch, clone(perm, concat(&scratch, s)));", 0),
    ("i32 n; parse(&n, n);", 0),
    ("Arena a; // f(&a, a);\n/* f(&a, a); */", 0),
    ('Arena a; puts("f(&a, a)"); c = \'(\';', 0),
    ("static Str f(Arena *perm, Arena scratch);", 0),
    ("Arena newarena(Region *r); f(&newarena, newarena);", 0),
    ("static void f()\n{\n    Arena a = newarena(r);\n    q->in = Buf(&a, 1<<12);\n"
     "    q->arena = a;\n}\n", 1),
    ("static void f()\n{\n    Arena a = newarena(r);\n    q->arena = a;\n"
     "    q->in = Buf(&q->arena, 1<<12);\n}\n", 0),
    ("static void f()\n{\n    Arena a = newarena(r);\n    Buf b(&a, 64);\n"
     "    x->mem = a;\n}\n", 0),
]


def selftest():
    failed = 0
    for text, want in SNIPPETS:
        values, pointers = arenanames(strip(text))
        got = lint(text, values, pointers)
        if len(got) != want:
            failed += 1
            print(f"FAIL selftest: {text!r}: {len(got)} findings, want {want}: {got}")
    return failed


def main():
    if selftest():
        return 1
    paths = []
    for dir in (SRC, os.path.join(SRC, "test")):
        for name in sorted(os.listdir(dir)):
            if name.endswith(".cpp"):
                paths.append(os.path.join(dir, name))
    texts = {}
    values, pointers = set(), set()
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            texts[path] = f.read()
        v, p = arenanames(strip(texts[path]))
        values |= v
        pointers |= p
    count = 0
    for path in paths:
        rel = os.path.relpath(path, os.path.join(SRC, ".."))
        for line, why in lint(texts[path], values, pointers):
            print(f"{rel}:{line}: {why}")
            count += 1
    if count:
        print(f"arenalint: {count} finding{'s' * (count != 1)}: give the callee's "
              f"results and its scratch separate arenas (carve, or the caller's scratch)")
        return 1
    print(f"arenalint: {len(paths)} files, no arena given twice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
