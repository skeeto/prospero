#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Arena aliasing that the sources show: one arena handed over twice.

A callee taking (Arena *perm, Arena scratch) and given one arena as
both, f(&a, a) or f(p, *p), starts its scratch where its results go, so
its first allocation in scratch writes over results it made before (as
3814c05 fixed). Likewise f(*p, g(p, ...)) or f(a, g(&a, ...)): the order
of arguments is unspecified, so the copy may be taken before g
allocates, and the callee's scratch then writes over what g returned,
whether g's call is the argument or either result of a conditional,
c ? x : g(&a, ...). A Buf in a struct bound to a local arena that is then
copied into that struct grows, once past its first block, through the
dead local (base.cpp's reserve).

The GUI's frame arena, g->frame, is a root: each copy of it starts at
its base, so two live copies share their bytes. A function given a
string (Str, char *, u8 *, a Slice) that copies the root for scratch
writes over that string when its caller made it in its own copy (the
find bar's count read "find previ"). Such a function takes its scratch
from its caller instead, unless the comment above it says
"arenalint: frame ok" and why: its strings are never in the frame
(literals, the platform's), or are done with first. A function holding
a copy of the root likewise uses that copy, not the root, while the
copy is in scope: where the root has moved since (a string put in the
frame itself, &g->frame, by a callee), the copy is done with, so it
ends its block first. Nor does it call, while the copy is in scope, a
function that copies the root or allocates in it in place (&g->frame),
itself or through its own calls: allocating in the root starts at its
base, under the copy's bytes, so it passes its copy down instead
(matched by name across the sources). For the same reason a function
given a string puts it in the root itself, &g->frame, only where the
comment above says "arenalint: frame ok" and why.

A carve is a fixed block: what it is given past its cap exits "out of
memory". A carve given to a function whose arena, at that position,
reaches a reader through its calls (matched by name and parameter
position across src/) is sized for that reader: os_read and os_readat
up to their max, os_listall its max entries, os_list a whole directory,
which no fixed size holds. Where a carve is sized otherwise, a comment
on its line, or just above, says "arenalint: carve ok" and why.

Each finding in src/ and src/test/ is printed, as FILE:LINE: and why,
and any fails the check. Nothing is built: the sources are read with
comments and literals blanked out. The matcher first checks itself on
snippets.

  uv run tests/arenalint.py
"""

import os
import bisect
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


def toplevel(args):
    """Split an argument list at its top-level commas, blanks removed."""
    out, depth, cur = [], 0, ""
    for c in args:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    out.append(cur)
    return [unparen(a) for a in out]


NESTED = re.compile(r"([A-Za-z_]\w*)(?:<[^<>()]*>)?\((.*)\)((?:\.|->)[A-Za-z_]\w*)*")


def branches(arg):
    """A conditional's two results, c ? x : y, else none: either may be
    the argument."""
    depth, q = 0, -1
    for i, c in enumerate(arg):
        depth += c in "([{"
        depth -= c in ")]}"
        if depth == 0 and c == "?" and q < 0:
            q, nest = i, 0
        elif depth == 0 and c == "?" and q >= 0:
            nest += 1
        elif depth == 0 and c == ":" and q >= 0 and not arg.startswith(":", i+1) \
                and not arg[i-1] == ":":
            if nest == 0:
                return [unparen(arg[q+1:i]), unparen(arg[i+1:])]
            nest -= 1
    return []


def allocates(arg, given):
    """Whether arg is a call given the arena as given, or a conditional
    with one as either result: its result lives in that arena."""
    if any(allocates(b, given) for b in branches(arg)):
        return True
    m = NESTED.fullmatch(arg)
    return bool(m) and balanced(m.group(2)) and given in toplevel(m.group(2))


def last(path):
    return re.split(r"\.|->", path)[-1]


def callee(stripped, open_):
    m = re.search(r"([A-Za-z_]\w*)\s*(?:<[^<>()]*>)?\s*$", stripped[max(0, open_-200):open_])
    return m.group(1) if m else "a call"


# The frame root by value: not &g->frame, g->frame.beg, or an assignment
ROOT  = re.compile(r"(&\s*)?(?:\b\w+(?:->|\.))*\b(?:g|gui)->frame\b(?!\s*(?:[.(\[]|=(?!=)))")
FUNC  = re.compile(r"^(?![ \t#}])[^\n;{}()=]*?\b([A-Za-z_]\w*)\s*\(((?:[^()]|\([^()]*\))*)\)"
                   r"\s*(?:const\s*)?\{", re.M)
STRS  = re.compile(r"\bStr\b|\bchar\b[^,]*\*|\bu8\b[^,]*\*|\bSlice\s*<")
COPY  = re.compile(r"\bArena\s+([A-Za-z_]\w*)\s*=\s*$")
CALL  = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
EXEMPT = "arenalint: frame ok"


def functions(s):
    """Each function definition, as (name, parameters, body start, end)."""
    for m in FUNC.finditer(s):
        if m.group(1) in ("if", "for", "while", "switch", "return"):
            continue
        i, depth = m.end(), 1
        while depth and i < len(s):
            depth += {"{": 1, "}": -1}.get(s[i], 0)
            i += 1
        yield m, m.group(2), m.end(), i


def leading(text, off):
    """The comment lines just above offset off's line."""
    lines = text[:off].split("\n")[:-1]
    out = []
    while lines and lines[-1].lstrip().startswith("//"):
        out.append(lines.pop())
    return "\n".join(out)


INROOT = re.compile(r"&\s*(?:\w+(?:->|\.))*(?:g|gui)->frame")


def intoroot(s, line, name, params, beg, end):
    """A string parameter put in the frame itself, &g->frame: allocating
    there starts at the root's base, under every copy, so where the
    caller made the string in a copy, its bytes are written over as
    they are read (the find bar's count)."""
    strs = []
    for p in toplevel(params) if params.strip() else []:
        q = re.search(r"([A-Za-z_]\w*)(?:=.*)?$", p)
        if STRS.search(p) and q:
            strs.append(q.group(1))
    found = []
    if not strs or not INROOT.search(s, beg, end):
        return found
    for open_, args in calls(s[beg:end]):
        open_ += beg
        if not args or not INROOT.fullmatch(args[0]):
            continue
        for a in args[1:]:
            hit = [x for x in strs if re.search(r"\b" + x + r"\b", a)]
            if hit:
                found.append((line(open_), f"{name}() puts {hit[0]}, which its caller "
                              f"may have made in a copy of the frame, in the frame "
                              f"itself, over that copy: take the caller's arena"))
                break
    return found


def framelint(text, s, line, copiers):
    found = []
    for m, params, beg, end in functions(s):
        uses = [u for u in ROOT.finditer(s, beg, end) if not u.group(1)]
        if not uses:
            if EXEMPT not in leading(text, m.start()):
                found += intoroot(s, line, m.group(1), params, beg, end)
            continue
        strs = [p.strip() for p in params.split(",") if STRS.search(p)]
        if strs and EXEMPT not in leading(text, m.start()):
            found.append((line(m.start()), f"{m.group(1)}() is given {strs[0]} yet copies "
                          f"the frame for scratch, where its caller may have made it: "
                          f"take the caller's scratch"))
        if EXEMPT not in leading(text, m.start()):
            found += intoroot(s, line, m.group(1), params, beg, end)
        copies = []  # (name, offset after the declaration)
        for u in uses:
            for name, at in copies:
                depth = 0
                for c in s[at:u.start()]:
                    depth += {"{": 1, "}": -1}.get(c, 0)
                    if depth < 0:
                        break
                if depth >= 0:
                    found.append((line(u.start()), f"{m.group(1)}() holds {name}, a copy "
                                  f"of the frame, yet uses the frame for scratch here, "
                                  f"over it: use {name}, or end its block first"))
                    break
            d = COPY.search(s, beg, u.start())
            if d and d.end() == u.start():
                copies.append((d.group(1), u.end()))
        for name, at in copies:
            depth, i = 0, at
            while i < end and depth >= 0:
                depth += {"{": 1, "}": -1}.get(s[i], 0)
                i += 1
            for c in CALL.finditer(s, at, i):
                if c.group(1) in copiers and c.group(1) != m.group(1):
                    found.append((line(c.start()), f"{m.group(1)}() holds {name}, a copy "
                                  f"of the frame, yet calls {c.group(1)}(), which copies "
                                  f"the frame or allocates in it over it"
                                  f"{via(copiers, c.group(1))}: pass it {name}"))
    return found


def copying(texts):
    """The functions in the stripped texts that copy the frame or allocate
    in it in place, themselves or through a call, each mapped to the one
    it calls that does, or None."""
    calls, found = {}, {}
    for s in texts:
        for m, params, beg, end in functions(s):
            if ROOT.search(s, beg, end):
                found[m.group(1)] = None
            calls.setdefault(m.group(1), set()).update(
                c.group(1) for c in CALL.finditer(s, beg, end))
    while True:
        new = {n: min(cs & found.keys()) for n, cs in calls.items()
               if n not in found and cs & found.keys()}
        if not new:
            return found
        found |= new


def via(copiers, name):
    """How name copies the frame, as " (through f(), g())"."""
    chain = []
    while copiers.get(name) and len(chain) < 8 and copiers[name] not in chain:
        name = copiers[name]
        chain.append(f"{name}()")
    return f" (through {', '.join(chain)})" if chain else ""



# Carves: a fixed block, whose cap no growth can pass, so a read larger
# than it exits "out of memory". A carve is followed into the function it
# is given to, by the position of the parameter it lands in, through
# that function's calls, to the readers at the bottom: os_read and
# os_readat read up to their max, os_listall up to its max entries, and
# os_list a whole directory, however large. Sizes are evaluated from
# literals and upper-case constants: a read whose max is not is passed
# over, and a carve whose size is not, such as one sized from its input,
# is held only to os_list. src/test is passed over: its fixtures are
# sized for what they hold.
READERS = ("os_read", "os_readat", "os_list", "os_listall")
DIRENT  = 64 + 256  # a Dirent and its name, at most
PARAM   = re.compile(r"\bArena\b\s*(?:const\s*)?[*&]?\s*(?:const\s*)?([A-Za-z_]\w*)\s*$")
ALIAS   = re.compile(r"\bArena\s+([A-Za-z_]\w*)\s*=\s*(?:\*\s*)?([A-Za-z_]\w*)\s*;")
CARVED  = re.compile(r"\bArena\s+([A-Za-z_]\w*)\s*=\s*(carve\((?:[^()]|\([^()]*\))*\))")
CONST   = re.compile(r"^[ \t]*(?:(?:static\s+)?constexpr\s+\w+\s+|#define\s+)?"
                     r"([A-Z][A-Z0-9_]+)\s*=?\s*([^,;\n]+)", re.M)
CARVEOK = "arenalint: carve ok"
INF     = float("inf")


def constants(texts):
    found = {}
    for s in texts:
        for m in CONST.finditer(s):
            found.setdefault(m.group(1), m.group(2).strip())
    return found


def evaluate(e, consts, depth=0):
    """An integer constant expression's value, else None."""
    e = re.sub(r"\(\s*(?:iz|uz|i64|u64|i32|u32)\s*\)", "", e)
    e = re.sub(r"(\d)(?:ull|ul|ll|u|l)\b", r"\1", e, flags=re.I)
    names = re.findall(r"\b[A-Za-z_]\w*\b", e)
    if depth > 8 or any(n not in consts for n in names):
        return None
    for n in names:
        v = evaluate(consts[n], consts, depth+1)
        if v is None:
            return None
        e = re.sub(r"\b" + n + r"\b", str(v), e)
    if not re.fullmatch(r"[\d\s+\-*/()<>]+", e) or "<<<" in e:
        return None
    try:
        return int(eval(e.replace("/", "//"), {"__builtins__": {}}))
    except Exception:
        return None


def readsize(g, args, consts):
    """Bytes a reader may put in the arena it is given, if g is one."""
    if g in ("os_read", "os_readat") and len(args) >= 4:
        return evaluate(args[-1], consts)
    if g == "os_list":
        return INF
    if g == "os_listall" and len(args) >= 5:
        v = evaluate(args[4], consts)
        return None if v is None else v*DIRENT
    return None


def bodies(s):
    """Each function as (match, parameters, body start, end, its calls),
    the file's calls found once."""
    found = sorted(calls(s))
    offs  = [o for o, args in found]
    for m, params, beg, end in functions(s):
        lo, hi = bisect.bisect_left(offs, beg), bisect.bisect_left(offs, end)
        yield m, params, beg, end, found[lo:hi]


def argname(a):
    a = a.lstrip("&*")
    return a if re.fullmatch(r"[A-Za-z_]\w*", a) else None


def carveflows(stripped):
    """Each (function, parameter index) whose arena reaches a reader,
    mapped to (most bytes read, the call it goes through)."""
    stripped = list(stripped)
    consts = constants(stripped)
    funcs = []
    for s in stripped:
        for m, params, beg, end, cs in bodies(s):
            ps, ar = toplevel(params) if params.strip() else [], {}
            for i, p in enumerate(ps):
                q = PARAM.search(p)
                if q:
                    ar[q.group(1)] = i
            names = {k: {k} for k in ar}
            for a in ALIAS.finditer(s, beg, end):
                for k in ar:
                    if a.group(2) in names[k]:
                        names[k].add(a.group(1))
            cl = [(callee(s, o), args) for o, args in cs]
            funcs.append((m.group(1), ar, names, cl))
    flows = {}
    def size(g, j, args):
        r = readsize(g, args, consts) if j == 1 else None
        return r if r is not None else flows.get((g, j), (None,))[0]
    changed = True
    while changed:
        changed = False
        for f, ar, names, cl in funcs:
            for k, i in ar.items():
                best = flows.get((f, i), (0,))[0]
                for g, args in cl:
                    for j, a in enumerate(args):
                        v = size(g, j, args) if argname(a) in names[k] else None
                        if v is not None and v > best:
                            best = v
                            flows[(f, i)] = (v, (g, j))
                            changed = True
    return flows, consts


def carvelint(text, s, line, flows, consts):
    found = []
    for m, params, beg, end, cs in bodies(s):
        carves = {c.group(1): (c.group(2), c.start()) for c in CARVED.finditer(s, beg, end)}
        if not carves and "carve(" not in s[beg:end]:
            continue
        for open_, args in cs:
            g = callee(s, open_)
            for j, a in enumerate(args):
                if a.startswith("carve("):
                    c, at = a, open_
                elif argname(a) in carves:
                    c, at = carves[argname(a)]
                else:
                    continue
                r = readsize(g, args, consts) if j == 1 else None
                v, via = (r, None) if r is not None else flows.get((g, j), (None, None))
                v = v if v else None
                cargs = toplevel(c[6:-1])
                cap = evaluate(cargs[1], consts) if len(cargs) > 1 else None
                if v is None or (v < INF and (cap is None or v < cap)):
                    continue
                if any(CARVEOK in text.split("\n")[line(o)-1] or CARVEOK in leading(text, o)
                       for o in (at, open_)):
                    continue
                what = "a whole directory" if v == INF else f"up to {v} bytes"
                chain = []
                while via and len(chain) < 8:
                    chain.append(f"{via[0]}()")
                    via = flows.get(via, (None, None))[1]
                path = f" (through {', '.join(chain)})" if chain else ""
                found.append((line(open_), f"{m.group(1)}() gives a carve of "
                              f"{cap if cap is not None else 'its own size'} to {g}(), "
                              f"which reads {what} into it{path}: it exits out of "
                              f"memory once that is more; size it for the read, or "
                              f"\"{CARVEOK}\" and why"))
    return found


def lint(text, values, pointers, copiers=None, carves=None):
    """Findings in one file's text, as (line, message)."""
    s = strip(text)
    found = []
    def line(off):
        return s.count("\n", 0, off) + 1
    found += framelint(text, s, line, copying([s]) if copiers is None else copiers)
    if carves is not False:
        found += carvelint(text, s, line, *(carveflows([s]) if carves is None else carves))
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
            if a.startswith("*") and PATH.fullmatch(a[1:]) and last(a[1:]) in pointers:
                if any(allocates(b, a[1:]) for b in args if b != a):
                    found.append((line(open_), f"{callee(s, open_)}() is given *{a[1:]} "
                                  f"beside an argument allocated in {a[1:]}"))
            elif PATH.fullmatch(a) and last(a) in values:
                if any(allocates(b, "&" + a) for b in args if b != a):
                    found.append((line(open_), f"{callee(s, open_)}() is given {a} "
                                  f"beside an argument allocated in &{a}"))
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
    ("Arena *a; i64 n = os_stat(os, *a, join(a, dir, name)).size;", 1),
    ("Arena s; f(os, s, concat(&s, x, y));", 1),
    ("Arena *a; f(os, *a, join(a, d,\n    n).data);", 1),
    ("Arena *a; Str p = join(a, d, n); f(os, *a, p);", 0),
    ("Arena *a; f(os, *a, g(b, join(a, d, n)));", 0),
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
    # The frame root
    ("static void f(Gui *g, Str s)\n{\n    Arena a = g->frame;\n}\n", 1),
    ("static void f(Gui *g, char const *id, i32 n)\n{\n    h(g->os, g->frame, n);\n}\n", 1),
    ("static b32 f(Gui *g, Slice<Item> items)\n{\n    Arena a = p->gui->frame;\n}\n", 1),
    ("// Opens it.\n// arenalint: frame ok, as s is SDL's\n"
     "void f(Gui *g, Str s)\n{\n    Arena a = g->frame;\n}\n", 0),
    ("static void f(Gui *g, Arena scratch, Str s)\n{\n    Arena a = scratch;\n}\n", 0),
    ("static void f(Gui *g, Session *s)\n{\n    Arena a = g->frame;\n}\n", 0),
    ("static void f(Gui *g, Str s)\n{\n    g->frame.beg = g->framebase;\n"
     "    g->frame = newarena(r);\n    Str t = concat(&g->frame, x, x);\n"
     "    i32 c = g->pal->frame;\n}\n", 0),
    ("static void f(Gui *g)\n{\n    Arena a = g->frame;\n    h(g, g->frame, x);\n}\n", 1),
    ("static void f(Gui *g)\n{\n    Arena a = g->frame;\n    Arena t = g->frame;\n}\n", 1),
    ("static void f(Gui *g)\n{\n    if (x) {\n        Arena a = g->frame;\n    }\n"
     "    Arena t = g->frame;\n    h(g, t);\n}\n", 0),
    ("static void f(Gui *g)\n{\n    switch (x) {\n    case 1: {\n        Arena a = g->frame;\n"
     "    } break;\n    case 2: {\n        Arena a = g->frame;\n    } break;\n    }\n}\n", 0),
    ("static void f(Gui *g)\n{\n    Arena a = g->frame;\n    if (x) {\n"
     "        h(g->os, g->frame);\n    }\n}\n", 1),
    # A callee copying the frame, itself or further down
    ("static void k(Gui *g)\n{\n    Arena t = g->frame;\n}\n"
     "static void f(Gui *g)\n{\n    Arena a = g->frame;\n    k(g);\n}\n", 1),
    ("static void k(Gui *g)\n{\n    Arena t = g->frame;\n}\n"
     "static void j(Gui *g)\n{\n    k(g);\n}\n"
     "static void f(Gui *g)\n{\n    Arena a = g->frame;\n    j(g);\n}\n", 1),
    ("static void k(Gui *g, Arena scratch)\n{\n    Arena t = scratch;\n}\n"
     "static void f(Gui *g)\n{\n    Arena a = g->frame;\n    k(g, a);\n}\n", 0),
    ("static void k(Gui *g)\n{\n    Arena t = g->frame;\n}\n"
     "static void f(Gui *g)\n{\n    {\n        Arena a = g->frame;\n    }\n    k(g);\n}\n", 0),
    ("static void f(Gui *g, i32 n)\n{\n    Arena a = g->frame;\n    if (n) f(g, n-1);\n}\n", 0),
    # A callee allocating in the frame itself, under the caller's copy
    ("static void k(Gui *g)\n{\n    setnote(g, concat(&g->frame, x, y));\n}\n"
     "static void f(Gui *g)\n{\n    Arena a = g->frame;\n    k(g);\n}\n", 1),
    ("static void k(Gui *g, Arena a)\n{\n    setnote(g, concat(&a, x, y));\n}\n"
     "static void f(Gui *g)\n{\n    Arena a = g->frame;\n    k(g, a);\n}\n", 0),
    # A string parameter put in the frame itself
    ("static void f(Gui *g, Str err)\n{\n    setnote(g, concat(&g->frame, \"x: \", err));\n}\n", 1),
    ("static Str f(Gui *g, Str path)\n{\n    return clone(&g->frame, path);\n}\n", 1),
    ("static void f(Gui *g, Str err)\n{\n    setnote(g, concat(&g->frame, \"x: \", why));\n}\n", 0),
    ("// arenalint: frame ok, as err is a record's\n"
     "static void f(Gui *g, Str err)\n{\n    setnote(g, concat(&g->frame, \"x: \", err));\n}\n", 0),
    # A carve given to a read larger than it
    ("enum : iz {\n    MAX = (iz)64 << 20,  // bytes\n};\n"
     "static Str k(Os *os, Arena *a, Arena scratch, Str p)\n{\n"
     "    Read rd = os_read(os, &scratch, p, MAX);\n    return clone(a, rd.data);\n}\n"
     "static void f(Os *os, Arena *a)\n{\n    Arena t = carve(a, 1<<16);\n"
     "    Arena o = carve(&t, 128);\n    k(os, &o, *a, p);\n    k(os, a, t, p);\n}\n", 1),
    ("static void k(Os *os, Arena *a, Str p)\n{\n    os_read(os, a, p, 1<<10);\n}\n"
     "static void f(Os *os, Arena *a)\n{\n    Arena t = carve(a, 1<<16);\n    k(os, &t, p);\n}\n", 0),
    ("static void k(Os *os, Arena a, Str p)\n{\n    Arena b = a;\n    os_list(os, &b, p);\n}\n"
     "static void f(Os *os, Arena s)\n{\n    k(os, carve(&s, 3*n), p);\n}\n", 1),
    ("static void f(Os *os, Arena s)\n{\n"
     "    Arena t = carve(&s, 1<<20);  // arenalint: carve ok, a few entries\n"
     "    os_list(os, &t, p);\n}\n", 0),
    # Unsequenced inside a conditional
    ("Arena a; os_launch(os, a, argv, env.len ? env : guienv(g, &a));", 1),
    ("Arena *a; f(*a, x ? join(a, d, n) : p);", 1),
    ("Arena a; os_launch(os, a, argv, env.len ? env : other);", 0),
    ("Arena a; f(a, x ? y : z ? concat(&a, p, q) : w);", 1),
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
    copiers = copying(strip(t) for t in texts.values())
    carves  = carveflows(strip(t) for p, t in texts.items()
                         if os.path.dirname(p) == SRC and
                         not p.endswith(("main_fuzz.cpp", "main_test.cpp")))
    count = 0
    for path in paths:
        rel = os.path.relpath(path, os.path.join(SRC, ".."))
        intest = os.path.dirname(path) != SRC
        for line, why in lint(texts[path], values, pointers, copiers,
                              False if intest else carves):
            print(f"{rel}:{line}: {why}")
            count += 1
    if count:
        print(f"arenalint: {count} finding{'s' * (count != 1)}: give no arena's bytes "
              f"to two at once (carve, or the caller's scratch)")
        return 1
    print(f"arenalint: {len(paths)} files, no arena given twice, no frame copied over, "
          f"no carve read past")
    return 0


if __name__ == "__main__":
    sys.exit(main())
