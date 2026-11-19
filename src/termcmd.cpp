// Shell integration's marks, and the commands they set apart, apart
// from the engine and the window, which both follow a tab's output:
// the engine for read_terminal and its waits (ptyhost.cpp), the window
// for Ask about this and links (term.cpp).
//
// A hooked shell (ptyhost.cpp) prints these, each with its tab's nonce,
// so that one a program prints, not knowing it, never counts:
//
//   ESC ] 7 ; file://HOST/PATH?nonce=N BEL  its folder, at each prompt
//   ESC ] 133;A;nonce=N BEL       at each prompt
//   ESC ] 133;B;nonce=N BEL       the prompt's end: typing goes here
//   ESC ] 133;C;nonce=N BEL       a command line is about to run
//   ESC ] 133;D;CODE;nonce=N BEL  it ran, and its status
//   ESC ] 133;D;nonce=N BEL       a prompt with nothing run before it
//
// OSC 7's PATH is percent-encoded where it must be (%, ?, #, controls):
// the query is the nonce's. A tab keeps a record of each command line
// run (TermCmd), the newest TERMCMD_MAX: its text, folder, times and
// status, and where it is in ariel's main screen and history. That is
// its prompt line, by number among the lines ariel flagged as prompts
// (OSC 133 A of any nonce, one a line), counted from the bottom, which
// a resize's reflow keeps, and how many lines follow it before the
// output's first. A prompt with nothing run gives way to the next.
//
// This is free and unencumbered software released into the public domain.

struct TermMark {
    i32 kind;     // 'A' to 'D', 0 for none
    b32 hascode;  // D's status is given
    i32 code;
    b32 ok;       // its nonce is the tab's
};

// An OSC 133 mark's payload (all after "133;"), checked against a tab's
// nonce: a field "nonce=N" must equal it, and no nonce matches none.
static TermMark termmarkparse(Str payload, Str nonce)
{
    TermMark r = {};
    if (!payload.len || payload[0]<'A' || payload[0]>'D' ||
        (payload.len>1 && payload[1]!=';')) {
        return r;
    }
    r.kind = payload[0];
    Cut c  = cut(payload, ';');
    for (i32 field = 0; c.ok; field++) {
        c = cut(c.tail, ';');
        Str f = c.head;
        if (startswith(f, "nonce=")) {
            r.ok |= nonce.len>0 && cuthead(f, 6)==nonce;
        } else if (field==0 && r.kind=='D') {
            b32 neg = f.len && f[0]=='-';
            Str ds  = neg ? cuthead(f, 1) : f;
            b32 num = ds.len>0 && ds.len<=10;
            i64 v   = 0;
            for (iz i = 0; num && i < ds.len; i++) {
                num = digit(ds[i]);
                v   = v*10 + (ds[i] - '0');
            }
            if (num) {
                v = neg ? -v : v;
                i64 hi = 0x7fffffff;
                v = v<-hi-1 ? -hi-1 : v>hi ? hi : v;
                r.hascode = 1;
                r.code    = (i32)v;
            }
        }
    }
    return r;
}

// An OSC 7 URI's folder (file://HOST/PATH?nonce=N), decoded into buf,
// if its nonce is the tab's; else null. On Windows "/C:/x" is "C:/x".
// A folder with a NUL, or none, is refused.
static Str termcwdparse(Str uri, Str nonce, b32 win, u8 *buf, iz cap)
{
    if (!nonce.len || !startswith(uri, "file://")) {
        return {};
    }
    uri = cuthead(uri, 7);
    iz q = -1;
    for (iz i = uri.len-1; q<0 && i >= 0; i--) {
        q = uri[i]=='?' ? i : q;
    }
    if (q < 0) {
        return {};
    }
    b32 ok = 0;
    for (Cut c = {{}, cuthead(uri, q+1), 1}; c.ok;) {
        c = cut(c.tail, '&');
        ok |= startswith(c.head, "nonce=") && cuthead(c.head, 6)==nonce;
    }
    Str path = {uri.data, q};
    iz  at   = 0;
    for (; at<path.len && path[at]!='/'; at++) {}  // the host
    path = cuthead(path, at);
    iz len = 0;
    for (iz i = 0; ok && i < path.len; i++) {
        i32 c = path[i];
        if (c=='%' && i+2<path.len && hexval(path[i+1])>=0 && hexval(path[i+2])>=0) {
            c  = hexval(path[i+1])<<4 | hexval(path[i+2]);
            i += 2;
        }
        ok = c && len<cap;
        if (ok) {
            buf[len++] = (u8)c;
        }
    }
    Str r = ok && len ? Str{buf, len} : Str{};
    if (win && r.len>=3 && r[0]=='/' && r[2]==':') {
        r = cuthead(r, 1);  // a drive's
    }
    return r;
}

enum : i32 {
    TERMCMD_MAX  = 32,    // records a tab keeps
    TERMCMD_TEXT = 512,   // bytes of a command line kept
    TERMCMD_CWD  = 1024,  // of a folder
    TERMCMD_ROW  = 1024,  // cells read of a row
    TERMCMD_OWN  = 256,   // the shell's own prompt lines remembered
};

enum : i32 {
    TC_NONE,
    TC_PROMPT,   // at its prompt, nothing run yet
    TC_RUNNING,  // from its C
    TC_DONE,     // its D, or the next prompt without one
};

static_assert(TERMCMD_MAX == 32, "read_terminal's commands say 1 to 32 (tools.cpp)");

struct TermCmd {
    i64 seq;       // the tab's count of records at its making, from 1
    i64 prompt;    // its prompt line's number (TermCmds' prompts)
    i32 state;     // TC_*
    i32 lines;     // lines from its prompt's first to its output's, 0: unknown
    b32 hascode;   // its status, as D gave it
    i32 code;
    i64 startat;   // ticks: its C
    i64 endat;     // its D (or the next A)
    u8  cmd[TERMCMD_TEXT];  // its command line, as the screen showed it
    i32 cmdlen;
    b32 cmdcut;    // longer than that
    u8  cwd[TERMCMD_CWD];   // the folder it ran in, as OSC 7 last said
    i32 cwdlen;
};

struct TermCmds {
    TermCmd ring[TERMCMD_MAX];
    i64     n;       // records made: the newest is ring[(n-1) % MAX]
    i64     prompts; // lines flagged as prompts so far, by any A
    i64     aid;     // the last A's line's first row's id, 0 for none
    i64     arow;    // and its row (ariel_lines), -1 if unknown
    i64     brow;    // this prompt's B: row and column, brow -1 if none
    i32     bx;
    b32     hooked;  // a mark of ours came
    i64     own[TERMCMD_OWN];  // the numbers of its own prompt lines, a
    i64     nown;              //   ring, so that others' are output
    u8      cwd[TERMCMD_CWD];  // the shell's folder, by OSC 7
    i32     cwdlen;
};

static Str termcmdstr(TermCmd *c)
{
    return {c->cmd, c->cmdlen};
}

static Str termcmdcwd(TermCmd *c)
{
    return {c->cwd, c->cwdlen};
}

// Record i, 0 the newest, else null.
static TermCmd *termcmdat(TermCmds *cs, i64 i)
{
    if (i<0 || i>=cs->n || i>=TERMCMD_MAX) {
        return 0;
    }
    return &cs->ring[(cs->n - 1 - i) % TERMCMD_MAX];
}

static void termcmdreset(TermCmds *cs)
{
    b32 hooked = cs->hooked;
    *cs = {};
    cs->hooked = hooked;
    cs->arow   = cs->brow = -1;
}

// Row numbers change as a resize reflows: forget this prompt's.
static void termcmdresized(TermCmds *cs)
{
    cs->arow = cs->brow = -1;
}

// The first row of row's line: up past the rows that wrap into it.
static i64 termlinefirst(ariel_Term *t, i64 row)
{
    ariel_Lines ls = ariel_lines(t);
    for (; row>ls.first && ariel_lineinfo(t, row-1).flags&ARIEL_R_WRAPPED; row--) {}
    return row;
}

// Where row text goes: a Buf, else a buffer of cap bytes, cut at the
// first code point that won't fit.
struct TermSink {
    Buf *b;
    u8  *buf;
    iz   len;
    iz   cap;
    b32  cut;
};

static void termsinkput(TermSink *k, Str s)
{
    if (k->b) {
        print(k->b, s);
        return;
    }
    if (k->cut || s.len>k->cap-k->len) {
        k->cut = 1;  // whole code points only
        return;
    }
    copybytes(k->buf+k->len, s.data, s.len);
    k->len += s.len;
}

// Row's text from column x0, wide characters' right halves and
// clusters' marks left out, blanks as spaces, trailing ones dropped
// unless it wraps.
static void termrowtext(TermSink *k, ariel_Term *t, i64 row, i32 x0)
{
    ariel_Cell cells[TERMCMD_ROW];
    i32 n   = ariel_line(t, row, cells, countof(cells));
    // ariel_line returns the row's column count, which may be more
    n = n<(i32)countof(cells) ? n : (i32)countof(cells);
    b32 wrp = (ariel_lineinfo(t, row).flags & ARIEL_R_WRAPPED) != 0;
    i32 end = n;
    for (; !wrp && end>x0 && !cells[end-1].cp; end--) {}
    for (i32 x = x0<0 ? 0 : x0; x < end; x++) {
        if (cells[x].width) {
            u8 enc[4];
            iz len = utf8encode(enc, cells[x].cp ? (i32)cells[x].cp : ' ');
            termsinkput(k, {enc, len});
        }
    }
    if (!wrp) {
        termsinkput(k, "\n");
    }
}

// The text from (row, x) to the start of row end, wrapped rows joined,
// blanks and newlines at its end dropped.
static Str termrowstext(TermSink *k, ariel_Term *t, i64 row, i32 x, i64 end)
{
    for (; row<end && !k->cut; row++, x = 0) {
        termrowtext(k, t, row, x);
    }
    Str s = k->b ? Str{k->b->data, k->b->len} : Str{k->buf, k->len};
    for (; s.len && (s[s.len-1]=='\n' || s[s.len-1]==' '); s.len--) {}
    return s;
}

// A mark, any of OSC 133 (ev's data its payload), seen as ariel gave
// it: A flags a line as a prompt, whoever printed it, and those count;
// the tab's own make and move its records. Returns the mark, ok if its.
static TermMark termcmdmark(TermCmds *cs, ariel_Term *t, Str payload, Str nonce, i64 now)
{
    TermMark    m  = termmarkparse(payload, nonce);
    ariel_Lines ls = ariel_lines(t);
    TermCmd    *c  = termcmdat(cs, 0);
    if (m.kind=='A' && ls.cursor>=0) {
        i64 first = termlinefirst(t, ls.cursor);
        i64 id    = ariel_lineinfo(t, first).id;
        b32 again = cs->aid && id==cs->aid;  // a second on its line
        cs->prompts += !again;
        cs->aid      = id;
        if (!m.ok) {
            return m;
        }
        cs->hooked = 1;
        cs->arow   = first;
        cs->brow   = -1;
        if (!again) {
            cs->own[cs->nown++ % TERMCMD_OWN] = cs->prompts;
        }
        if (c && c->state==TC_RUNNING) {
            c->state = TC_DONE;  // no D came
            c->endat = now;
        }
        if (!c || c->state!=TC_PROMPT) {
            c = &cs->ring[cs->n++ % TERMCMD_MAX];
        }
        *c = {};
        c->seq    = cs->n;
        c->prompt = cs->prompts;
        c->state  = TC_PROMPT;
        c->cwdlen = cs->cwdlen;
        copybytes(c->cwd, cs->cwd, cs->cwdlen);
        return m;
    } else if (!m.ok) {
        return m;
    }
    cs->hooked = 1;
    switch (m.kind) {
    case 'B':
        if (ls.cursor>=0 && c && c->state==TC_PROMPT) {
            cs->brow = ls.cursor;
            cs->bx   = ariel_cursor(t).x;
        }
        break;
    case 'C':
        if (c && c->state==TC_PROMPT) {
            c->state   = TC_RUNNING;
            c->startat = now;
            if (ls.cursor>=0 && cs->arow>=ls.first && cs->arow<=ls.cursor) {
                // Lines from the prompt's to the cursor's, which starts
                // the output's; the command line from B, else the line
                c->lines = 0;
                for (i64 r = cs->arow+1; r <= ls.cursor; r++) {
                    c->lines += !(ariel_lineinfo(t, r-1).flags & ARIEL_R_WRAPPED);
                }
                b32      b = cs->brow>=cs->arow && cs->brow<=ls.cursor;
                TermSink k = {};
                k.buf = c->cmd;
                k.cap = TERMCMD_TEXT;
                Str cmd   = termrowstext(&k, t, b ? cs->brow : cs->arow, b ? cs->bx : 0,
                                         ls.cursor);
                c->cmdcut = k.cut;
                c->cmdlen = (i32)cmd.len;
            }
        }
        break;
    case 'D':
        if (c && c->state==TC_RUNNING) {
            c->state   = TC_DONE;
            c->hascode = m.hascode;
            c->code    = m.code;
            c->endat   = now;
        }
        break;
    }
    return m;
}

// An OSC 7 (ev's data the URI): the shell's folder, if its nonce is the
// tab's. True if it changed.
static b32 termcmdfolder(TermCmds *cs, Str uri, Str nonce, b32 win)
{
    u8  buf[TERMCMD_CWD];
    Str cwd = termcwdparse(uri, nonce, win, buf, countof(buf));
    if (!cwd.len || cwd==Str{cs->cwd, cs->cwdlen}) {
        return 0;
    }
    cs->hooked = 1;
    cs->cwdlen = (i32)cwd.len;
    copybytes(cs->cwd, cwd.data, cwd.len);
    return 1;
}

struct TermPlace {
    i64 prompt;  // its prompt line's first row, -1 if gone
    i64 out;     // its output's first row
    i64 end;     // past its last: the next prompt's, else the end
};

// Where record c is in the main screen and history, by its prompt
// line, counted from the bottom, up to the shell's next prompt line:
// another program's prompt lines (OSC 133 A of no nonce) are output.
static TermPlace termcmdplace(TermCmds *cs, ariel_Term *t, TermCmd *c)
{
    TermPlace p    = {-1, -1, -1};
    i64       next = 0;  // the shell's next prompt line's number, 0 for none
    for (i64 i = 0; i < cs->nown && i < TERMCMD_OWN; i++) {
        i64 o = cs->own[i];
        next = o>c->prompt && (!next || o<next) ? o : next;
    }
    ariel_Lines ls   = ariel_lines(t);
    i64         want = cs->prompts - c->prompt;  // newer prompt lines
    i64         stop = next ? cs->prompts - next : -1;
    i64         seen = 0;
    i64         end  = ls.end;
    for (i64 r = ls.end-1; r >= ls.first; r--) {
        if (!(ariel_lineinfo(t, r).flags & ARIEL_R_PROMPT)) {
            continue;
        }
        i64 n = seen++;
        if (n == stop) {
            end = r;
        }
        if (n < want) {
            continue;
        }
        p.prompt = r;
        p.end    = end;
        p.out    = r;
        for (i32 k = c->lines>0 ? c->lines : 1; k>0 && p.out<end; p.out++) {
            k -= !(ariel_lineinfo(t, p.out).flags & ARIEL_R_WRAPPED);
        }
        return p;
    }
    return p;
}

// The record whose lines hold row, from its prompt to the next one's,
// else null; *inout says row is in its output.
static TermCmd *termcmdatrow(TermCmds *cs, ariel_Term *t, i64 row, b32 *inout)
{
    *inout = 0;
    ariel_Lines ls = ariel_lines(t);
    if (row<ls.first || row>=ls.end) {
        return 0;
    }
    i64 above = 0;  // prompt lines at row or above it, to its own
    i64 own   = -1;
    for (i64 r = row; r >= ls.first; r--) {
        if (ariel_lineinfo(t, r).flags & ARIEL_R_PROMPT) {
            own = r;
            break;
        }
    }
    if (own < 0) {
        return 0;
    }
    for (i64 r = own+1; r < ls.end; r++) {
        above += (ariel_lineinfo(t, r).flags & ARIEL_R_PROMPT) != 0;
    }
    i64 number = cs->prompts - above;
    for (i64 i = 0; i < TERMCMD_MAX; i++) {
        TermCmd *c = termcmdat(cs, i);
        if (c && c->prompt==number) {
            TermPlace p = termcmdplace(cs, t, c);
            *inout = c->state!=TC_PROMPT && p.out>=0 && row>=p.out;
            return c;
        }
    }
    return 0;
}

// A record's output, as the screen and history hold it now: wrapped
// rows joined, blank lines at its end dropped. *gone says its prompt
// line is no longer kept (cleared, or past the history), so that none
// of it is known.
static Str termcmdoutput(Arena *a, TermCmds *cs, ariel_Term *t, TermCmd *c, b32 *gone)
{
    *gone = 0;
    if (c->state == TC_PROMPT) {
        return {};
    }
    TermPlace p = termcmdplace(cs, t, c);
    if (p.prompt < 0) {
        *gone = 1;
        return {};
    }
    Buf      b(a, 256);
    TermSink k = {};
    k.b = &b;
    Str out = termrowstext(&k, t, p.out, 0, p.end);
    finish(&b);
    return out;
}

// Text cut to its first head and last tail lines, each at most linemax
// bytes, and to maxbytes in all, from the head first, then from the
// tail's oldest, saying how many lines were left out between.
static Str termclip(Arena *a, Str text, i32 head, i32 tail, iz linemax, iz maxbytes)
{
    Slice<Str> lines = {};
    for (Cut c = {{}, text, text.len>0}; c.ok;) {
        c     = cut(c.tail, '\n');
        lines = push(a, lines, c.head.len>linemax ? utf8head(c.head, linemax) : c.head);
    }
    iz h    = head<lines.len ? head : lines.len;  // [0, h) and [from, len) kept
    iz from = lines.len-tail>h ? lines.len-tail : h;
    iz total = 0;
    for (iz i = 0; i < lines.len; i++) {
        total += i<h || i>=from ? lines[i].len+1 : 0;
    }
    while (total>maxbytes && h>1) {
        total -= lines[--h].len + 1;
    }
    while (total>maxbytes && from<lines.len-1) {
        total -= lines[from++].len + 1;
    }
    Buf b(a, total + 64);
    for (iz i = 0; i < h; i++) {
        print(&b, lines[i]);
        putbyte(&b, '\n');
    }
    if (from > h) {
        print(&b, "[... ");
        print(&b, (i64)(from - h));
        print(&b, from-h==1 ? Str(" line") : Str(" lines"));
        print(&b, " left out ...]\n");
    }
    for (iz i = from; i < lines.len; i++) {
        print(&b, lines[i]);
        putbyte(&b, '\n');
    }
    return finish(&b);
}
