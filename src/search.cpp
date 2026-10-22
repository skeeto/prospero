// Search: the grep and glob tools, through ripgrep or a built-in walker
//
// A search runs as a process under the tool layer, like a command, so
// that it costs the engine thread nothing and stops the way a command
// stops: ripgrep when it is on PATH, or else this same executable as
//   prospero --search grep|glob ROOT PATTERN [-i] [-F] [-g GLOB] [-m MAX]
// Either way the output is ripgrep's: "path\0line:text\n" for each
// matching line, or "path\0" for each file, with paths relative to the
// search directory. The engine keeps a bounded amount of it, killing the
// search past that, and shows at most SEARCH_RESULTS.
//
// The built-in walker visits directories depth first, names in byte
// order, and skips .git, symlinks (os_list never lists them), binary
// files (a NUL in the first 8 KiB), files over FILE_MAX, and whatever
// the .gitignore files of the enclosing repository exclude. Ripgrep
// does the same with --hidden, except that a -g glob would override the
// ignore rules there: so glob and grep use ripgrep only for a glob
// without a slash (once a leading "**/" is dropped), given as a file
// type, which matches names at any depth and keeps the rules, and the
// walker for any other. Ripgrep exits 2 when it could not read some of
// the files, which is a failed search only with a message.
//
// Globs follow .gitignore: one without a slash matches a name at any
// depth; one with a slash matches the path from the search directory,
// where "**/" matches any number of directories and a trailing "/**"
// everything inside. * and ? stay within a name; [a-z] and [!a-z] are
// classes; {a,b} are alternatives; \ quotes the next character.
//
// Regular expressions compile to a Thompson NFA, simulated a line at a
// time in time linear in the line: literal text, ., classes, \d \w \s
// and their negations, \b \B, ^ $, * + ? {n,m} (lazy forms accepted and
// treated alike, since only whether a line matches counts), | and
// groups, with (?: ) accepted too. A literal that every match must
// contain lets most lines and files be skipped without the NFA.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    RX_CHAR,   // c
    RX_ANY,    // any code point
    RX_CLASS,  // classes[c]
    RX_SPLIT,  // x, then y
    RX_JMP,    // x
    RX_BOL,
    RX_EOL,
    RX_WORD,   // \b
    RX_NWORD,  // \B
    RX_MATCH,
};

enum : i32 {
    RX_PROGMAX = 20000,  // instructions
    RX_REPMAX  = 1000,   // largest {n,m}
    RX_DEPTH   = 64,     // nested groups
};

struct RxInst {
    i32 op;
    i32 c;
    i32 x;
    i32 y;
};

struct RxRange {
    i32 lo;
    i32 hi;
};

struct RxClass {
    Slice<RxRange> ranges;
    b32            neg;
};

struct Regex {
    Slice<RxInst>  prog;
    Slice<RxClass> classes;
    Str            lit;   // text every match contains, maybe empty
    Str            err;   // why it did not compile, else null
    b32            fold;  // ASCII letters in either case
};

enum : i32 {
    RXN_EMPTY,
    RXN_CHAR,
    RXN_ANY,
    RXN_CLASS,
    RXN_BOL,
    RXN_EOL,
    RXN_WORD,
    RXN_NWORD,
    RXN_CAT,
    RXN_ALT,
    RXN_REP,
};

struct RxNode {
    i32     kind;
    i32     c;    // code point or class
    i32     min;
    i32     max;  // -1 for no limit
    RxNode *a;
    RxNode *b;
};

struct RxParser {
    Arena          *a;
    Str             s;
    iz              i;
    i32             depth;
    Str             err;
    Slice<RxClass>  classes;
};

static RxNode *rxnode(RxParser *p, i32 kind, RxNode *a, RxNode *b)
{
    RxNode *n = alloc<RxNode>(p->a);
    n->kind = kind;
    n->a    = a;
    n->b    = b;
    return n;
}

static void rxerror(RxParser *p, Str msg)
{
    if (!p->err.data) {
        p->err = msg;
    }
}

static b32 rxwordch(i32 c)
{
    return (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') ||
           c=='_';
}

static void rxrange(Arena *a, RxClass *k, i32 lo, i32 hi)
{
    RxRange r = {lo, hi};
    k->ranges = push(a, k->ranges, r);
}

// The ranges of \d, \w or \s, added to a class.
static void rxperl(Arena *a, RxClass *k, u8 e)
{
    switch (e) {
    case 'd': rxrange(a, k, '0', '9'); break;
    case 'w': rxrange(a, k, '0', '9'); rxrange(a, k, 'a', 'z');
              rxrange(a, k, 'A', 'Z'); rxrange(a, k, '_', '_'); break;
    case 's': rxrange(a, k, ' ', ' '); rxrange(a, k, '\t', '\r'); break;
    }
}

static i32 rxaddclass(RxParser *p, RxClass k)
{
    p->classes = push(p->a, p->classes, k);
    return (i32)(p->classes.len - 1);
}

// The character after a backslash that stands for itself, or -1.
static i32 rxescape(u8 e)
{
    switch (e) {
    case 't': return '\t';
    case 'n': return '\n';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    }
    return alnum(e) ? -1 : e;
}

static i32 rxhex(Str s, iz *i)  // \xHH, *i past the x
{
    i32 v = 0;
    for (i32 n = 0; n < 2; n++) {
        if (*i >= s.len) return -1;
        u8 c = s[(*i)++];
        i32 d = digit(c) ? c-'0' : (c|0x20)>='a' && (c|0x20)<='f' ? (c|0x20)-'a'+10 : -1;
        if (d < 0) return -1;
        v = v*16 + d;
    }
    return v;
}

static RxNode *rxclass(RxParser *p)
{
    RxClass k = {};
    Str     s = p->s;
    if (p->i<s.len && s[p->i]=='^') {
        k.neg = 1;
        p->i++;
    }
    for (b32 first = 1;; first = 0) {
        if (p->i >= s.len) {
            rxerror(p, "unclosed [ class");
            return 0;
        }
        u8 c = s[p->i];
        if (c==']' && !first) {
            p->i++;
            break;
        }
        i32 lo = 0;
        if (c == '\\') {
            if (p->i+1 >= s.len) {
                rxerror(p, "a backslash ends the pattern");
                return 0;
            }
            u8 e = s[p->i+1];
            p->i += 2;
            if (e=='d' || e=='w' || e=='s') {
                rxperl(p->a, &k, e);
                continue;
            } else if (e=='D' || e=='W' || e=='S') {
                rxerror(p, "\\D, \\W and \\S are not supported inside [ ]");
                return 0;
            } else if (e == 'x') {
                lo = rxhex(s, &p->i);
            } else {
                lo = rxescape(e);
            }
            if (lo < 0) {
                rxerror(p, "unknown escape in a [ ] class");
                return 0;
            }
        } else {
            lo = utf8decode(s, &p->i);
        }
        i32 hi = lo;
        if (p->i+1<s.len && s[p->i]=='-' && s[p->i+1]!=']') {
            p->i++;
            if (s[p->i] == '\\') {
                if (p->i+1 >= s.len) {
                    rxerror(p, "a backslash ends the pattern");
                    return 0;
                }
                p->i++;
                hi = rxescape(s[p->i++]);
                if (hi < 0) {
                    rxerror(p, "unknown escape in a [ ] class");
                    return 0;
                }
            } else {
                hi = utf8decode(s, &p->i);
            }
            if (hi < lo) {
                rxerror(p, "a range in [ ] runs backward");
                return 0;
            }
        }
        rxrange(p->a, &k, lo, hi);
    }
    RxNode *n = rxnode(p, RXN_CLASS, 0, 0);
    n->c = rxaddclass(p, k);
    return n;
}

static RxNode *rxalt(RxParser *p);

static RxNode *rxatom(RxParser *p)
{
    Str s = p->s;
    u8  c = s[p->i];
    switch (c) {
    case '(': {
        p->i++;
        if (p->i+1<s.len && s[p->i]=='?') {
            if (s[p->i+1] != ':') {
                rxerror(p, "only (?: ) groups are supported, not (?");
                return 0;
            }
            p->i += 2;
        }
        if (++p->depth > RX_DEPTH) {
            rxerror(p, "groups are nested too deeply");
            return 0;
        }
        RxNode *n = rxalt(p);
        p->depth--;
        if (p->i>=s.len || s[p->i]!=')') {
            rxerror(p, "unclosed ( group");
            return 0;
        }
        p->i++;
        return n;
    }
    case ')':
        rxerror(p, "unmatched )");
        return 0;
    case '*': case '+': case '?':
        rxerror(p, "nothing before * + or ? to repeat");
        return 0;
    case '.':
        p->i++;
        return rxnode(p, RXN_ANY, 0, 0);
    case '^':
        p->i++;
        return rxnode(p, RXN_BOL, 0, 0);
    case '$':
        p->i++;
        return rxnode(p, RXN_EOL, 0, 0);
    case '[':
        p->i++;
        return rxclass(p);
    case '\\': {
        if (p->i+1 >= s.len) {
            rxerror(p, "a backslash ends the pattern");
            return 0;
        }
        u8 e = s[p->i+1];
        p->i += 2;
        switch (e) {
        case 'b': return rxnode(p, RXN_WORD, 0, 0);
        case 'B': return rxnode(p, RXN_NWORD, 0, 0);
        case 'd': case 'w': case 's':
        case 'D': case 'W': case 'S': {
            RxClass k = {};
            k.neg = e < 'a';
            rxperl(p->a, &k, (u8)(e | 0x20));
            RxNode *n = rxnode(p, RXN_CLASS, 0, 0);
            n->c = rxaddclass(p, k);
            return n;
        }
        }
        i32 v = e=='x' ? rxhex(s, &p->i) : rxescape(e);
        if (v < 0) {
            rxerror(p, "unknown escape: put a backslash only before "
                       "punctuation, or use \\d \\w \\s \\b");
            return 0;
        }
        RxNode *n = rxnode(p, RXN_CHAR, 0, 0);
        n->c = v;
        return n;
    }
    }
    RxNode *n = rxnode(p, RXN_CHAR, 0, 0);
    n->c = utf8decode(s, &p->i);
    return n;
}

struct RxCount {
    i32 min;
    i32 max;  // -1 for no limit
    b32 ok;
};

// A {n}, {n,} or {n,m} quantifier at p->i, consumed if valid.
static RxCount rxcount(RxParser *p)
{
    RxCount r = {};
    Str     s = p->s;
    iz      i = p->i + 1;
    i64     n = 0;
    i64     m = 0;
    iz      start = i;
    for (; i<s.len && digit(s[i]) && n<=RX_REPMAX; i++) n = n*10 + s[i]-'0';
    if (i == start) return r;
    m = n;
    if (i<s.len && s[i]==',') {
        iz mstart = ++i;
        m = 0;
        for (; i<s.len && digit(s[i]) && m<=RX_REPMAX; i++) m = m*10 + s[i]-'0';
        if (i == mstart) m = -1;
    }
    if (i>=s.len || s[i]!='}') {
        return r;
    } else if (n>RX_REPMAX || m>RX_REPMAX) {
        rxerror(p, "a repetition count is over 1000");
        return r;
    } else if (m>=0 && m<n) {
        rxerror(p, "in {n,m}, m is less than n");
        return r;
    }
    p->i  = i + 1;
    r.min = (i32)n;
    r.max = (i32)m;
    r.ok  = 1;
    return r;
}

static RxNode *rxrep(RxParser *p)
{
    RxNode *n = rxatom(p);
    while (n && p->i<p->s.len) {
        RxCount k = {};
        switch (p->s[p->i]) {
        case '*': k = {0, -1, 1}; p->i++; break;
        case '+': k = {1, -1, 1}; p->i++; break;
        case '?': k = {0,  1, 1}; p->i++; break;
        case '{': k = rxcount(p);         break;
        }
        if (!k.ok) {
            break;
        } else if (n->kind==RXN_BOL || n->kind==RXN_EOL || n->kind==RXN_WORD ||
                   n->kind==RXN_NWORD) {
            rxerror(p, "an anchor cannot be repeated");
            return 0;
        }
        if (p->i<p->s.len && (p->s[p->i]=='?' || p->s[p->i]=='+')) {
            p->i++;  // lazy and possessive forms match the same lines
        }
        RxNode *r = rxnode(p, RXN_REP, n, 0);
        r->min = k.min;
        r->max = k.max;
        n = r;
    }
    return n;
}

static RxNode *rxcat(RxParser *p)
{
    RxNode *n = 0;
    while (p->i<p->s.len && p->s[p->i]!='|' && p->s[p->i]!=')') {
        RxNode *m = rxrep(p);
        if (!m) return 0;
        n = n ? rxnode(p, RXN_CAT, n, m) : m;
    }
    return n ? n : rxnode(p, RXN_EMPTY, 0, 0);
}

static RxNode *rxalt(RxParser *p)
{
    RxNode *n = rxcat(p);
    while (n && p->i<p->s.len && p->s[p->i]=='|') {
        p->i++;
        RxNode *m = rxcat(p);
        n = m ? rxnode(p, RXN_ALT, n, m) : 0;
    }
    return n;
}

struct RxComp {
    Arena         *a;
    Slice<RxInst>  prog;
    b32            full;
};

static iz rxinst(RxComp *c, i32 op, i32 v)
{
    if (c->prog.len >= RX_PROGMAX) {
        c->full = 1;
        return c->prog.len ? c->prog.len-1 : 0;
    }
    RxInst in = {op, v, 0, 0};
    c->prog = push(c->a, c->prog, in);
    return c->prog.len - 1;
}

static void rxemit(RxComp *c, RxNode *n)
{
    if (c->full) return;
    switch (n->kind) {
    case RXN_EMPTY:                                       break;
    case RXN_CHAR:  rxinst(c, RX_CHAR, n->c);             break;
    case RXN_ANY:   rxinst(c, RX_ANY, 0);                 break;
    case RXN_CLASS: rxinst(c, RX_CLASS, n->c);            break;
    case RXN_BOL:   rxinst(c, RX_BOL, 0);                 break;
    case RXN_EOL:   rxinst(c, RX_EOL, 0);                 break;
    case RXN_WORD:  rxinst(c, RX_WORD, 0);                break;
    case RXN_NWORD: rxinst(c, RX_NWORD, 0);               break;
    case RXN_CAT:   rxemit(c, n->a); rxemit(c, n->b);     break;
    case RXN_ALT: {
        iz split = rxinst(c, RX_SPLIT, 0);
        rxemit(c, n->a);
        iz jmp = rxinst(c, RX_JMP, 0);
        iz b   = c->prog.len;
        rxemit(c, n->b);
        if (c->full) return;
        c->prog[split].x = (i32)(split + 1);
        c->prog[split].y = (i32)b;
        c->prog[jmp].x   = (i32)c->prog.len;
    } break;
    case RXN_REP: {
        i32 copies = n->max<0 && n->min>0 ? n->min-1 : n->min;
        for (i32 k = 0; k < copies; k++) {
            rxemit(c, n->a);
        }
        if (n->max < 0) {
            if (n->min > 0) {
                // x+: x, then loop back while it may match again
                iz top = c->prog.len;
                rxemit(c, n->a);
                iz split = rxinst(c, RX_SPLIT, 0);
                if (c->full) return;
                c->prog[split].x = (i32)top;
                c->prog[split].y = (i32)(split + 1);
            } else {
                iz split = rxinst(c, RX_SPLIT, 0);
                rxemit(c, n->a);
                iz jmp = rxinst(c, RX_JMP, 0);
                if (c->full) return;
                c->prog[split].x = (i32)(split + 1);
                c->prog[split].y = (i32)c->prog.len;
                c->prog[jmp].x   = (i32)split;
            }
            return;
        }
        // Optional copies, each skipping to the end
        Slice<iz> splits = {};
        for (i32 k = n->min; k < n->max && !c->full; k++) {
            splits = push(c->a, splits, rxinst(c, RX_SPLIT, 0));
            rxemit(c, n->a);
        }
        if (c->full) return;
        for (iz k = 0; k < splits.len; k++) {
            c->prog[splits[k]].x = (i32)(splits[k] + 1);
            c->prog[splits[k]].y = (i32)c->prog.len;
        }
    } break;
    }
}

// The longest run of literal characters in the top-level sequence of
// the pattern: every match contains it.
static void rxliteral(Arena *a, RxNode *n, Buf *run, Str *best)
{
    if (n->kind == RXN_CAT) {
        rxliteral(a, n->a, run, best);
        rxliteral(a, n->b, run, best);
        return;
    }
    if (n->kind==RXN_CHAR && n->c>0) {
        u8 tmp[4];
        iz len = utf8encode(tmp, n->c);
        print(run, Str{tmp, len});
        if (run->len > best->len) {
            *best = clone(a, finish(run));
        }
        return;
    }
    if (n->kind==RXN_REP && n->min>=1) {
        // x{2,}: the first copy is certain, but not what follows it
        rxliteral(a, n->a, run, best);
    } else if (n->kind==RXN_BOL || n->kind==RXN_EOL || n->kind==RXN_WORD ||
               n->kind==RXN_NWORD || n->kind==RXN_EMPTY) {
        return;  // zero width: the run goes on
    }
    run->len = 0;
}

static u8 *lowerstr(Arena *a, Str s)
{
    u8 *r = allocbytes(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        r[i] = lowercase(s[i]);
    }
    return r;
}

static Regex rxcompile(Arena *a, Str pattern, b32 fold)
{
    Regex    r = {};
    RxParser p = {};
    p.a  = a;
    p.s  = pattern;
    r.fold = fold;
    RxNode *n = rxalt(&p);
    if (!p.err.data && p.i<pattern.len) {
        rxerror(&p, "unmatched )");
    }
    if (p.err.data || !n) {
        r.err = p.err.data ? p.err : Str("invalid pattern");
        return r;
    }
    RxComp c = {};
    c.a = a;
    rxemit(&c, n);
    rxinst(&c, RX_MATCH, 0);
    if (c.full) {
        r.err = "the pattern is too large: use fewer or smaller {n,m} counts";
        return r;
    }
    r.prog    = c.prog;
    r.classes = p.classes;
    Buf run(a, 64);
    Str best = {};
    rxliteral(a, n, &run, &best);
    r.lit = fold ? Str{lowerstr(a, best), best.len} : best;
    return r;
}

static b32 rxinclass(RxClass *k, i32 c, b32 fold)
{
    b32 in = 0;
    for (iz i = 0; i<k->ranges.len && !in; i++) {
        RxRange *r = &k->ranges[i];
        in = c>=r->lo && c<=r->hi;
        if (!in && fold && c<128 && letter((u8)c)) {
            i32 o = c ^ 0x20;
            in = o>=r->lo && o<=r->hi;
        }
    }
    return in != k->neg;
}

// Working space for matching lines against one regex.
struct RxRun {
    Regex *rx;
    i32   *clist;
    i32   *nlist;
    i32   *stack;
    u32   *mark;
    u32    gen;
};

static RxRun rxrun(Arena *a, Regex *rx)
{
    RxRun r = {};
    iz    n = rx->prog.len;
    r.rx    = rx;
    r.clist = alloc<i32>(a, n);
    r.nlist = alloc<i32>(a, n);
    r.stack = alloc<i32>(a, n);
    r.mark  = alloc<u32>(a, n);
    return r;
}

// Add a thread at pc to the list, following jumps and the assertions
// that hold between code points prev and next (-1 at the line's ends).
// Returns 1 if it reaches a match.
static b32 rxadd(RxRun *r, i32 *list, iz *len, i32 pc, i32 prev, i32 next)
{
    RxInst *prog = r->rx->prog.data;
    iz      top  = 0;
    if (r->mark[pc] == r->gen) return 0;
    r->mark[pc]     = r->gen;
    r->stack[top++] = pc;
    while (top) {
        i32     at = r->stack[--top];
        RxInst *in = prog + at;
        i32     go = -1;
        switch (in->op) {
        case RX_MATCH: return 1;
        case RX_JMP:   go = in->x; break;
        case RX_SPLIT:
            // y first on the stack, so that x is explored first
            if (r->mark[in->y] != r->gen) {
                r->mark[in->y]  = r->gen;
                r->stack[top++] = in->y;
            }
            go = in->x;
            break;
        case RX_BOL:   go = prev<0 ? at+1 : -1;  break;
        case RX_EOL:   go = next<0 ? at+1 : -1;  break;
        case RX_WORD:
        case RX_NWORD: {
            b32 edge = (prev>=0 && rxwordch(prev)) != (next>=0 && rxwordch(next));
            go = edge == (in->op==RX_WORD) ? at+1 : -1;
        } break;
        default:
            list[(*len)++] = at;
            break;
        }
        if (go>=0 && r->mark[go]!=r->gen) {
            r->mark[go]     = r->gen;
            r->stack[top++] = go;
        }
    }
    return 0;
}

// Whether the regex matches anywhere in the line.
static b32 rxmatch(RxRun *r, Str line)
{
    Regex  *rx    = r->rx;
    RxInst *prog  = rx->prog.data;
    iz      clen  = 0;
    i32    *clist = r->clist;
    i32    *nlist = r->nlist;
    iz      i     = 0;
    i32     prev  = -1;
    i32     next  = -1;
    if (line.len) {
        iz k = 0;
        next = utf8decode(line, &k);
    }
    r->gen++;
    if (rxadd(r, clist, &clen, 0, prev, next)) return 1;
    while (i < line.len) {
        i32 c = utf8decode(line, &i);
        prev = c;
        next = -1;
        if (i < line.len) {
            iz k = i;
            next = utf8decode(line, &k);
        }
        i32 fc = rx->fold && c<128 ? lowercase((u8)c) : c;
        r->gen++;
        iz nlen = 0;
        for (iz t = 0; t < clen; t++) {
            RxInst *in = prog + clist[t];
            b32     ok = 0;
            switch (in->op) {
            case RX_CHAR:
                ok = in->c==c || (rx->fold && in->c<128 && lowercase((u8)in->c)==fc);
                break;
            case RX_ANY:   ok = 1;                                                  break;
            case RX_CLASS: ok = rxinclass(&rx->classes[in->c], c, rx->fold);        break;
            }
            if (ok && rxadd(r, nlist, &nlen, clist[t]+1, prev, next)) return 1;
        }
        // Unanchored: a match may start at any position
        if (rxadd(r, nlist, &nlen, 0, prev, next)) return 1;
        i32 *tmp = clist;
        clist = nlist;
        nlist = tmp;
        clen  = nlen;
    }
    return 0;
}

// Globs

// Expand {a,b} alternatives, nested or not, into every combination, at
// most 64 of them. A brace without a matching close stays literal.
static Slice<Str> globexpand(Arena *a, Str pat)
{
    Slice<Str> r = {};
    iz open = -1, depth = 0, close = -1;
    for (iz i = 0; i < pat.len; i++) {
        if (pat[i] == '\\') {
            i++;
        } else if (pat[i] == '{') {
            if (!depth++) open = i;
        } else if (pat[i]=='}' && depth) {
            if (!--depth) {
                close = i;
                break;
            }
        }
    }
    if (close < 0) {
        r = push(a, r, pat);
        return r;
    }
    Str head = takehead(pat, open);
    Str tail = cuthead(pat, close+1);
    Str body = span(pat.data+open+1, pat.data+close);
    for (iz i = 0, start = 0, d = 0; i <= body.len; i++) {
        if (i<body.len && body[i]=='\\') {
            i++;
            continue;
        }
        if (i<body.len && body[i]=='{') d++;
        if (i<body.len && body[i]=='}') d--;
        if (i==body.len || (body[i]==',' && !d)) {
            Str alt = span(body.data+start, body.data+(i<body.len ? i : body.len));
            Str one = concat(a, concat(a, head, alt), tail);
            Slice<Str> more = globexpand(a, one);
            for (iz k = 0; k<more.len && r.len<64; k++) {
                r = push(a, r, more[k]);
            }
            start = i + 1;
        }
    }
    return r;
}

// A character of a glob class at p[*i], quoted or not.
static i32 globchar(Str p, iz *i)
{
    if (p[*i]=='\\' && *i+1<p.len) {
        (*i)++;
    }
    return utf8decode(p, i);
}

// One class at p[*pi] (just past the '['), matching c. Advances *pi
// past the ']'. Returns -1 if the class is malformed (no ']').
static i32 globclass(Str p, iz *pi, i32 c)
{
    iz  i   = *pi;
    b32 neg = i<p.len && (p[i]=='!' || p[i]=='^');
    b32 in  = 0;
    i += neg;
    for (b32 first = 1;; first = 0) {
        if (i >= p.len) return -1;
        if (p[i]==']' && !first) break;
        i32 lo = globchar(p, &i);
        i32 hi = lo;
        if (i+1<p.len && p[i]=='-' && p[i+1]!=']') {
            i++;
            hi = globchar(p, &i);
        }
        in |= c>=lo && c<=hi;
    }
    *pi = i + 1;
    return in != neg;
}

static b32 globat(Str p, iz pi, Str s, iz si, i32 *budget)
{
    while (pi < p.len) {
        if (--*budget < 0) return 0;
        u8 c = p[pi];
        if (c == '*') {
            b32 dbl = pi+1<p.len && p[pi+1]=='*' && (!pi || p[pi-1]=='/');
            if (dbl) {
                iz q = pi + 2;
                if (q == p.len) {
                    return 1;  // "/**" at the end: everything inside
                } else if (p[q] == '/') {
                    // "**/": any number of whole directories
                    for (iz k = si; k <= s.len; k++) {
                        if ((k==si || s[k-1]=='/') && globat(p, q+1, s, k, budget)) {
                            return 1;
                        }
                    }
                    return 0;
                }
                pi++;  // any other "**" is a "*"
            }
            for (iz k = si;; k++) {
                if (globat(p, pi+1, s, k, budget)) return 1;
                if (k>=s.len || s[k]=='/') return 0;
            }
        }
        if (si >= s.len) return 0;
        if (c == '?') {
            if (s[si] == '/') return 0;
            utf8decode(s, &si);
            pi++;
        } else if (c == '[') {
            iz  i  = pi + 1;
            iz  sj = si;
            i32 ch = utf8decode(s, &sj);
            i32 in = ch=='/' ? 0 : globclass(p, &i, ch);
            if (in < 0) {
                // No closing bracket: a literal [
                if (s[si] != '[') return 0;
                si++;
                pi++;
            } else if (!in) {
                return 0;
            } else {
                si = sj;
                pi = i;
            }
        } else {
            if (c=='\\' && pi+1<p.len) {
                c = p[++pi];
            }
            if (s[si] != c) return 0;
            si++;
            pi++;
        }
    }
    return si == s.len;
}

// Whether a path (relative, with '/') matches one glob, alternatives
// already expanded: a glob with no slash, but for a trailing one,
// matches the last name, and otherwise the whole path.
static b32 globone(Str glob, Str path)
{
    Str g = glob;
    if (g.len>1 && g[g.len-1]=='/') g.len--;
    b32 slash = 0;
    for (iz i = 0; i < g.len; i++) {
        slash |= g[i] == '/';
    }
    if (slash && g[0]=='/') {
        g = cuthead(g, 1);
    }
    Str s = path;
    if (!slash) {
        iz i = s.len;
        for (; i>0 && s[i-1]!='/'; i--) {}
        s = cuthead(s, i);
    }
    i32 budget = 1 << 16;
    return globat(g, 0, s, 0, &budget);
}

static b32 globmatch(Slice<Str> globs, Str path)
{
    for (iz i = 0; i < globs.len; i++) {
        if (globone(globs[i], path)) {
            return 1;
        }
    }
    return 0;
}


// .gitignore

struct GitIgnoreRule {
    Slice<Str> globs;   // expanded alternatives
    b32        neg;     // !pattern: include again
    b32        dironly; // pattern/: directories only
};

struct GitIgnore {
    GitIgnore           *next;   // the enclosing directory's, or null
    Str                  dir;    // absolute, where the file is
    Slice<GitIgnoreRule> rules;
};

// A .gitignore's rules. Patterns with a slash anywhere but at the end
// are anchored to the file's directory, as globone takes them.
static Slice<GitIgnoreRule> ignoreparse(Arena *a, Str text)
{
    Slice<GitIgnoreRule> r = {};
    for (Str rest = text; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str l = c.head;
        if (l.len && l[l.len-1]=='\r') l.len--;
        // Trailing blanks go, unless quoted by a backslash
        while (l.len && l[l.len-1]==' ' && !(l.len>1 && l[l.len-2]=='\\')) l.len--;
        if (!l.len || l[0]=='#') continue;
        GitIgnoreRule rule = {};
        if (l[0] == '!') {
            rule.neg = 1;
            l = cuthead(l, 1);
        } else if (l.len>1 && l[0]=='\\' && (l[1]=='!' || l[1]=='#')) {
            l = cuthead(l, 1);
        }
        if (l.len && l[l.len-1]=='/') {
            rule.dironly = 1;
            l.len--;
        }
        if (!l.len) continue;
        b32 slash = 0;
        for (iz i = 0; i < l.len; i++) {
            slash |= l[i] == '/';
        }
        if (slash && l[0]!='/') {
            l = concat(a, "/", l);  // anchored, as globone wants it
        }
        rule.globs = globexpand(a, clone(a, l));
        r = push(a, r, rule);
    }
    return r;
}

// Whether the files of the ignore chain exclude a path (absolute), the
// innermost file deciding, and within a file, its last matching rule.
static b32 ignored(GitIgnore *chain, Str path, b32 isdir)
{
    for (GitIgnore *f = chain; f; f = f->next) {
        if (!startswith(path, f->dir) || path.len<=f->dir.len) continue;
        iz  skip = f->dir.len + (f->dir[f->dir.len-1] != '/');
        Str rel  = cuthead(path, skip);
        for (iz i = f->rules.len-1; i >= 0; i--) {
            GitIgnoreRule *r = &f->rules[i];
            if ((!r->dironly || isdir) && globmatch(r->globs, rel)) {
                return !r->neg;
            }
        }
    }
    return 0;
}

// Load a directory's .gitignore onto the chain, if it has one.
static GitIgnore *ignoreload(Os *os, Arena *a, GitIgnore *chain, Str dir)
{
    Read rd = os_read(os, a, join(a, dir, ".gitignore"), 1<<20);
    if (rd.err || !rd.data.len) {
        return chain;
    }
    GitIgnore *f = alloc<GitIgnore>(a);
    f->next  = chain;
    f->dir   = dir;
    f->rules = ignoreparse(a, rd.data);
    return f->rules.len ? f : chain;
}


// The built-in walker

enum : i32 {
    SEARCH_GREP,
    SEARCH_GLOB,
};

struct SearchFound {  // a glob result
    Str rel;
    i64 mtime;
};

struct SearchWalk {
    Os                *os;
    i32                mode;
    RxRun              rx;
    Str                literal;  // grep -F (lower case when folding)
    b32                fold;
    Str                need;     // what a line must contain, if anything
    Slice<Str>         globs;    // grep: the files to search, if any
    iz                 limit;    // results to print, then stop
    iz                 found;
    Buf               *out;
    Slice<SearchFound> newest;   // glob: kept newest first, at most limit
    Arena             *keep;     // for newest
    b32                hold;     // keep output in out, never printed (tests)
};

static void walkflush(SearchWalk *w)
{
    if (w->out->len && !w->hold) {
        os_print(w->os, 1, finish(w->out));
        w->out->len = 0;
    }
}

static b32 walkline(SearchWalk *w, Str line)
{
    if (w->literal.len) {
        return w->fold ? findfold(line, w->literal)>=0 : find(line, w->literal)>=0;
    }
    if (w->need.len) {
        b32 has = w->fold ? findfold(line, w->need)>=0 : find(line, w->need)>=0;
        if (!has) return 0;
    }
    return rxmatch(&w->rx, line);
}

static void walkgrep(SearchWalk *w, Arena scratch, Str path, Str rel)
{
    Read rd = os_read(w->os, &scratch, path, (iz)FILE_MAX);
    if (rd.err || !rd.data.len) return;
    Str data = rd.data;
    for (iz i = 0; i<data.len && i<8192; i++) {
        if (!data[i]) return;  // binary
    }
    Str need = w->literal.len ? w->literal : w->need;
    if (need.len && (w->fold ? findfold(data, need) : find(data, need)) < 0) {
        return;  // not in the file at all
    }
    i64 n = 0;
    for (Str rest = data; rest.len && w->found<w->limit;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        n++;
        Str line = c.head;
        if (line.len && line[line.len-1]=='\r') line.len--;
        if (!walkline(w, line)) continue;
        print(w->out, rel);
        putbyte(w->out, 0);
        print(w->out, n);
        putbyte(w->out, ':');
        if (line.len > SEARCH_LINEMAX) {
            print(w->out, utf8head(line, SEARCH_LINEMAX));
            print(w->out, " [...]");
        } else {
            print(w->out, line);
        }
        putbyte(w->out, '\n');
        w->found++;
    }
    walkflush(w);
}

static void walkglob(SearchWalk *w, Str rel, i64 mtime)
{
    w->found++;
    // Insert by mtime, newest first, keeping at most limit
    Slice<SearchFound> *n = &w->newest;
    if (n->len==w->limit && (!n->len || (*n)[n->len-1].mtime>=mtime)) {
        return;
    }
    SearchFound f = {clone(w->keep, rel), mtime};
    if (n->len < w->limit) {
        *n = push(w->keep, *n, f);
    } else {
        (*n)[n->len-1] = f;
    }
    for (iz i = n->len-1; i>0 && (*n)[i-1].mtime<(*n)[i].mtime; i--) {
        SearchFound t = (*n)[i-1];
        (*n)[i-1] = (*n)[i];
        (*n)[i]   = t;
    }
}

static void walkdir(SearchWalk *w, Arena scratch, GitIgnore *chain, Str dir,
                    Str rel, i32 depth)
{
    if (depth > 128 || (w->mode==SEARCH_GREP && w->found>=w->limit)) {
        return;
    }
    chain = ignoreload(w->os, &scratch, chain, dir);
    Slice<Dirent *> ents = sortedlist(w->os, &scratch, dir);
    for (iz i = 0; i < ents.len; i++) {
        if (w->mode==SEARCH_GREP && w->found>=w->limit) {
            return;
        }
        Arena   t    = scratch;
        Dirent *d    = ents[i];
        if (d->name == ".git") {
            continue;
        }
        Str path = join(&t, dir, d->name);
        Str sub  = rel.len ? join(&t, rel, d->name) : d->name;
        if (ignored(chain, path, d->isdir)) {
            continue;
        }
        if (d->isdir) {
            walkdir(w, t, chain, path, sub, depth+1);
        } else if (w->mode == SEARCH_GLOB) {
            if (globmatch(w->globs, sub)) {
                walkglob(w, sub, d->mtime);
            }
        } else if (!w->globs.len || globmatch(w->globs, sub)) {
            walkgrep(w, t, path, sub);
        }
    }
}

// The .gitignore files above the search directory, out to the root of
// the repository holding it, if any: as ripgrep reads them.
static GitIgnore *ignoreparents(Os *os, Arena *a, Str root, b32 win)
{
    Slice<Str> dirs = {};
    Str        top  = {};
    for (Str d = root;;) {
        Str git = join(a, d, ".git");
        if (os_stat(os, *a, git).type != STAT_MISSING) {
            top = d;
            break;
        }
        Str up = normpath(a, d, "..", win);
        if (up == d) break;
        dirs = push(a, dirs, up);
        d = up;
    }
    GitIgnore *chain = 0;
    if (!top.data) {
        return chain;  // not in a repository: only files below count
    }
    for (iz i = dirs.len-1; i >= 0; i--) {
        if (dirs[i].len >= top.len) {  // at or below the repository root
            chain = ignoreload(os, a, chain, dirs[i]);
        }
    }
    return chain;
}

struct SearchOpts {
    i32 mode;     // SEARCH_GREP or SEARCH_GLOB
    Str root;     // absolute: a directory, or a file for grep
    Str pattern;
    Str glob;     // grep: only files that match
    b32 fold;
    b32 literal;
    iz  limit;    // results, at most
    b32 win;      // Windows paths
};

// Run a search with the walker, its output in ripgrep's form into out,
// which is printed as it fills unless held. Returns the problem with a
// pattern that does not compile, else null; *found counts results.
static Str searchrun(Os *os, Arena *perm, Arena scratch, SearchOpts *o,
                     Buf *out, b32 hold, iz *found)
{
    SearchWalk w = {};
    w.os    = os;
    w.mode  = o->mode;
    w.limit = o->limit>0 ? o->limit : SEARCH_RESULTS+1;
    w.keep  = perm;
    w.out   = out;
    w.hold  = hold;
    w.fold  = o->fold;
    Str root = normpath(perm, o->root, ".", o->win);
    if (w.mode == SEARCH_GLOB) {
        w.globs = globexpand(perm, o->pattern);
    } else {
        if (o->glob.len) {
            w.globs = globexpand(perm, o->glob);
        }
        if (o->literal) {
            Str p = o->pattern;
            w.literal = w.fold ? Str{lowerstr(perm, p), p.len} : p;
        } else {
            Regex *rx = alloc<Regex>(perm);
            *rx = rxcompile(perm, o->pattern, w.fold);
            if (rx->err.data) {
                return rx->err;
            }
            w.rx   = rxrun(perm, rx);
            w.need = rx->lit;
        }
    }

    GitIgnore *chain = ignoreparents(os, perm, root, o->win);
    Stat    st    = os_stat(os, scratch, root);
    if (st.type==STAT_FILE && w.mode==SEARCH_GREP) {
        walkgrep(&w, scratch, root, basename(root));
    } else {
        walkdir(&w, scratch, chain, root, {}, 0);
    }
    if (w.mode == SEARCH_GLOB) {
        for (iz i = 0; i < w.newest.len; i++) {
            print(out, w.newest[i].rel);
            putbyte(out, 0);
        }
    }
    walkflush(&w);
    *found = w.found;
    return {};
}

// prospero --search grep|glob ROOT PATTERN [-i] [-F] [-g GLOB] [-m MAX]
// Exits 0 with results, 1 without, 2 for a bad pattern (explained on
// standard error) or bad usage.
static i32 searchmain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    if (nargs < 5 || (args[2]!="grep" && args[2]!="glob")) {
        os_print(os, 2, "usage: prospero --search grep|glob ROOT PATTERN "
                        "[-i] [-F] [-g GLOB] [-m MAX]\n");
        return 2;
    }
    SearchOpts o = {};
    o.mode    = args[2]=="grep" ? SEARCH_GREP : SEARCH_GLOB;
    o.win     = HOST_WINDOWS;
    o.root    = args[3];
    o.pattern = args[4];
    for (i32 i = 5; i < nargs; i++) {
        if (args[i] == "-i") {
            o.fold = 1;
        } else if (args[i] == "-F") {
            o.literal = 1;
        } else if (args[i]=="-g" && i+1<nargs) {
            o.glob = args[++i];
        } else if (args[i]=="-m" && i+1<nargs) {
            Int n = parseint(args[++i]);
            o.limit = n.ok && n.val>0 ? (iz)n.val : 0;
        } else {
            os_print(os, 2, "prospero --search: unknown option\n");
            return 2;
        }
    }

    Buf out(&perm, 1<<16);
    iz  found = 0;
    Str err   = searchrun(os, &perm, scratch, &o, &out, 0, &found);
    if (err.data) {
        Buf b(&scratch, 256);
        print(&b, "regex parse error: ");
        print(&b, err);
        putbyte(&b, '\n');
        os_print(os, 2, finish(&b));
        return 2;
    }
    return found ? 0 : 1;
}


// Searching from the engine

// The command line of a search: ripgrep's, or the built-in walker's.
// Both run in the search directory, with paths relative to it: target
// is "." there, or the name of the one file to search. The walker takes
// the absolute path, the file's when there is one.
static Slice<Str> searchargv(Arena *a, ToolEnv *te, i32 tool, Str path,
                             Str target, Args *args)
{
    // ripgrep filters by a glob given as a file type, which matches names
    // at any depth and keeps the ignore rules, so it takes only globs
    // without a slash, here once a leading "**/" is gone
    Slice<Str> v    = {};
    Str        glob = tool==TOOL_GLOB ? args->pattern : args->glob;
    while (startswith(glob, "**/") && glob.len>3) {
        glob = cuthead(glob, 3);
    }
    b32 slash = 0;
    for (iz i = 0; i < glob.len; i++) {
        slash |= glob[i] == '/';
    }
    b32 rg = te->rg.len && !slash;
    if (!rg) {
        v = push(a, v, te->exe);
        v = push(a, v, Str("--search"));
        v = push(a, v, tool==TOOL_GREP ? Str("grep") : Str("glob"));
        v = push(a, v, path);
        v = push(a, v, args->pattern);
        if (args->ignorecase) v = push(a, v, Str("-i"));
        if (args->literal)    v = push(a, v, Str("-F"));
        if (args->glob.len) {
            v = push(a, v, Str("-g"));
            v = push(a, v, args->glob);
        }
        return v;
    }
    static Str const common[] = {
        "--no-config", "--hidden", "--glob", "!.git", "--no-messages",
        "--null", "--color", "never",
    };
    v = push(a, v, te->rg);
    for (iz i = 0; i < countof(common); i++) {
        v = push(a, v, common[i]);
    }
    if (te->windows) {
        v = push(a, v, Str("--path-separator"));
        v = push(a, v, Str("/"));
    }
    if (tool == TOOL_GLOB) {
        // The files that match, newest first (a -g glob would include
        // ignored files), which the engine filters again by the whole
        // pattern
        v = push(a, v, Str("--files"));
        v = push(a, v, Str("--sortr"));
        v = push(a, v, Str("modified"));
        v = push(a, v, Str("--type-add"));
        v = push(a, v, concat(a, "prospero:", glob));
        v = push(a, v, Str("--type"));
        v = push(a, v, Str("prospero"));
    } else {
        static Str const grep[] = {
            "--sort", "path", "--line-number", "--no-heading",
            "--with-filename", "--max-columns", "300",
            "--max-columns-preview",
        };
        for (iz i = 0; i < countof(grep); i++) {
            v = push(a, v, grep[i]);
        }
        if (args->ignorecase) v = push(a, v, Str("--ignore-case"));
        if (args->literal)    v = push(a, v, Str("--fixed-strings"));
        if (glob.len) {
            v = push(a, v, Str("--type-add"));
            v = push(a, v, concat(a, "prospero:", glob));
            v = push(a, v, Str("--type"));
            v = push(a, v, Str("prospero"));
        }
        v = push(a, v, Str("--regexp"));
        v = push(a, v, args->pattern);
    }
    v = push(a, v, Str("--"));
    v = push(a, v, target);
    return v;
}

struct SearchEnd {
    i32 tool;
    i32 code;       // exit status
    b32 signaled;
    b32 limited;    // the output passed its limit, so it was cut short
    b32 timedout;
    b32 cancelled;
    Str root;       // the directory searched, normalized
    Str cwd;        // normalized
    Str pattern;    // as the agent gave it
    Str glob;       // glob: the pattern
    b32 ignorecase;
    b32 windows;
    b32 tagged;     // grep lists the tags of its files (hashline edits)
};

// A result path as the agent sees it: relative to the working directory
// when inside it.
static Str searchpath(Arena *a, SearchEnd *e, Str rel)
{
    if (startswith(rel, "./") || startswith(rel, ".\\")) {
        rel = cuthead(rel, 2);
    }
    if (e->windows) {
        Str r = clone(a, rel);
        for (iz i = 0; i < r.len; i++) {
            r[i] = r[i]=='\\' ? '/' : r[i];
        }
        rel = r;
    }
    Str abs = rel.len ? normpath(a, e->root, rel, e->windows) : e->root;
    return showpath(e->cwd, abs);
}

static ToolOut searchresult(Os *os, Arena *a, Str spill, SearchEnd *e)
{
    ToolOut r    = {};
    Read    rd   = os_readat(os, a, spill, 0, SEARCH_OUTMAX + (64<<10));
    Str     data = rd.err ? Str{} : rd.data;
    Str     where = showpath(e->cwd, e->root);
    where = where==e->cwd ? Str("the working directory") : where;

    Buf        b(a, 4096);
    Buf        diag(a, 256);
    iz         shown = 0;
    iz         total = 0;
    Slice<Str> globs = e->tool==TOOL_GLOB ? globexpand(a, e->glob) : Slice<Str>{};
    Slice<Str> files = {};  // grep's, shown, for their tags
    if (e->tool == TOOL_GREP) {
        for (Str rest = data; rest.len;) {
            Cut c = cut(rest, '\n');
            rest = c.tail;
            Cut p = cut(c.head, 0);
            if (!p.ok) {
                if (diag.len < 1024) {
                    print(&diag, c.head);
                    putbyte(&diag, '\n');
                }
                continue;
            }
            if (!c.ok && e->limited) {
                break;  // the last line was cut off
            }
            total++;
            if (shown == SEARCH_RESULTS) {
                continue;
            }
            Cut n    = cut(p.tail, ':');
            Str text = n.tail;
            if (text.len && text[text.len-1]=='\r') text.len--;
            if (text.len > SEARCH_LINEMAX+32) {
                text = concat(a, utf8head(text, SEARCH_LINEMAX), " [...]");
            }
            Str path = searchpath(a, e, p.head);
            print(&b, path);
            putbyte(&b, ':');
            print(&b, n.head);
            print(&b, ": ");
            print(&b, text);
            putbyte(&b, '\n');
            shown++;
            if (e->tagged && files.len<SEARCH_TAGS &&
                    (!files.len || files[files.len-1]!=path)) {
                files = push(a, files, path);
            }
        }
    } else {
        for (Str rest = data; rest.len;) {
            Cut c = cut(rest, 0);
            rest = c.tail;
            if (!c.ok) {
                if (!e->limited) {
                    print(&diag, c.head);  // not a path: an error message
                }
                break;
            }
            Str rel = c.head;
            if (startswith(rel, "./") || startswith(rel, ".\\")) {
                rel = cuthead(rel, 2);
            }
            Str slashed = rel;
            if (e->windows) {
                slashed = clone(a, rel);
                for (iz i = 0; i < slashed.len; i++) {
                    slashed[i] = slashed[i]=='\\' ? '/' : slashed[i];
                }
            }
            if (!globmatch(globs, slashed)) {
                continue;
            }
            total++;
            if (shown < SEARCH_RESULTS) {
                print(&b, searchpath(a, e, rel));
                putbyte(&b, '\n');
                shown++;
            }
            if (total > SEARCH_RESULTS) {
                break;  // newest first: the rest are older
            }
        }
    }

    // Exit status 2 is a failed search only with a message, as of a bad
    // pattern: ripgrep exits 2, silently, when it could not read some
    // file or directory, though it searched the rest
    Str msg     = trim(cleanoutput(a, finish(&diag)));
    b32 unread  = e->code>=2 && !e->signaled && !e->timedout && !msg.len;
    b32 listcut = e->tool==TOOL_GLOB && e->limited;
    if (e->cancelled) {
        print(&b, "[cancelled by the user]");
    } else if (!shown && e->code>=2 && !e->signaled && !e->timedout && msg.len) {
        b.len = 0;
        print(&b, "The search failed, so there are no results");
        print(&b, msg.len ? Str(": ") : Str("."));
        print(&b, msg.len>2000 ? utf8head(msg, 2000) : msg);
        if (e->tool == TOOL_GREP) {
            print(&b, "\nFix the pattern, or set literal to true to search "
                      "for it as plain text.");
        }
    } else if (!shown && listcut) {
        // The newest files only were listed: the older were not checked
        print(&b, "No files matching ");
        print(&b, e->glob);
        print(&b, " were found among the newest files in ");
        print(&b, where);
        print(&b, ", but there are so many that the list was cut at 8 MiB, "
                  "and older files were not checked. Search a smaller "
                  "directory, or narrow the pattern.");
        r.ok = 1;
    } else if (!shown) {
        if (e->tool == TOOL_GREP) {
            print(&b, "No lines match ");
            print(&b, e->pattern);
            print(&b, " in ");
            print(&b, where);
            print(&b, e->ignorecase ? Str(".") :
                      Str(". The search is case-sensitive: set ignore_case "
                          "to true to match letters in either case."));
        } else {
            print(&b, "No files match ");
            print(&b, e->glob);
            print(&b, " in ");
            print(&b, where);
            print(&b, ". A pattern with a slash matches whole paths from "
                      "the search directory; use **/ to match at any depth.");
        }
        r.ok = 1;
    } else {
        if (total>shown || (e->limited && e->tool==TOOL_GREP)) {
            print(&b, e->tool==TOOL_GREP ? Str("[the first 100 matching lines; ")
                                         : Str("[the newest 100 files; "));
            print(&b, "there are more: narrow the pattern or the path]");
        } else if (listcut) {
            print(&b, "[so many files that the list was cut at 8 MiB, and "
                      "older files were not checked: narrow the path]");
        }
        if (files.len) {
            // As read would show them, so that an edit can follow
            if (b.len && b.data[b.len-1]!='\n') putbyte(&b, '\n');
            print(&b, "Tags for edit:");
            for (iz i = 0; i < files.len; i++) {
                // The file is read into the free space past b, which b
                // may grow into, so the tag is copied out first
                u8  tag[4];
                b32 ok = 0;
                {
                    Arena t   = *a;
                    Str   abs = normpath(&t, e->cwd, files[i], e->windows);
                    Read  rd  = os_read(os, &t, abs, (iz)EDIT_LINESMAX);
                    if (!rd.err) {
                        Str s = filetag(&t, rd.data);
                        copybytes(tag, s.data, 4);
                        ok = 1;
                    }
                }
                if (ok) {
                    print(&b, " [");
                    print(&b, files[i]);
                    putbyte(&b, '#');
                    print(&b, Str{tag, 4});
                    putbyte(&b, ']');
                }
            }
            putbyte(&b, '\n');
        }
        r.ok = 1;
    }
    if (unread && !e->cancelled) {
        if (b.len && b.data[b.len-1]!='\n') putbyte(&b, '\n');
        print(&b, "[some files or directories could not be read, so they "
                  "were not searched]");
    }
    if (e->timedout) {
        if (b.len && b.data[b.len-1]!='\n') putbyte(&b, '\n');
        print(&b, "[the search was stopped after 30 seconds, so results may "
                  "be missing: search a smaller directory]");
    }
    r.text = finish(&b);
    return r;
}
