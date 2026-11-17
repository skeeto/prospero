// A root session's terminal tabs as a set, and the titles shown on them,
// apart from ImGui and the engine (term.cpp draws and runs them).
//
// The set's array is in the tab bar's order, front the index shown. A
// tab added goes last and shows; a tab dropped shows its right
// neighbour, else its left. The bar may reorder the tabs, by drag, and
// the set follows.
//
// This is free and unencumbered software released into the public domain.

struct TermTab;

enum : i32 {
    TERMTABS     = 8,    // tabs a root may have
    TERMTITLE    = 40,   // code points of a title shown
    TERMTITLECAP = TERMTITLE*4 + 4,  // its bytes, with an ellipsis
};

struct TermSet {
    TermTab *tabs[TERMTABS];
    i32      n;
    i32      front;      // the tab shown, while n > 0
    b32      frontset;   // front changed here: the bar must select it
};

static i32 termsetfind(TermSet *s, TermTab *tt)
{
    for (i32 i = 0; i < s->n; i++) {
        if (s->tabs[i] == tt) {
            return i;
        }
    }
    return -1;
}

static TermTab *termsetfront(TermSet *s)
{
    return s->n ? s->tabs[s->front] : 0;
}

static void termsetshow(TermSet *s, i32 i)
{
    if (i>=0 && i<s->n && (i!=s->front || !s->frontset)) {
        s->front    = i;
        s->frontset = 1;
    }
}

// Add a tab last, shown. False when the set is full.
static b32 termsetadd(TermSet *s, TermTab *tt)
{
    if (s->n == TERMTABS) {
        return 0;
    }
    s->tabs[s->n++] = tt;
    s->front    = s->n - 1;
    s->frontset = 1;
    return 1;
}

// Remove tab i, returning it.
static TermTab *termsetdrop(TermSet *s, i32 i)
{
    assert(i>=0 && i<s->n);
    TermTab *tt = s->tabs[i];
    for (i32 k = i; k < s->n-1; k++) {
        s->tabs[k] = s->tabs[k+1];
    }
    s->tabs[--s->n] = 0;
    if (!s->n) {
        s->front    = 0;
        s->frontset = 0;
    } else if (i < s->front) {
        s->front--;  // the same tab, moved down
    } else if (i == s->front) {
        // Its right neighbour, now at i, else its left
        s->front    = i<s->n ? i : s->n-1;
        s->frontset = 1;
    }
    return tt;
}

// Take the bar's order, n tabs as it has them: the set's tabs in that
// order, then any it doesn't have yet (not yet drawn) as they were.
// Tabs not in the set are ignored. The front tab stays front.
static void termsetorder(TermSet *s, TermTab **order, i32 n)
{
    TermTab *front = termsetfront(s);
    TermTab *was[TERMTABS];
    i32      nwas = s->n;
    for (i32 i = 0; i < nwas; i++) {
        was[i] = s->tabs[i];
    }
    i32 k = 0;
    for (i32 i = 0; i < n; i++) {
        i32 at = -1;
        for (i32 j = 0; j < nwas; j++) {
            if (was[j] == order[i]) {
                at = j;
                break;
            }
        }
        if (at >= 0) {
            s->tabs[k++] = was[at];
            was[at] = 0;
        }
    }
    for (i32 j = 0; j < nwas; j++) {
        if (was[j]) {
            s->tabs[k++] = was[j];
        }
    }
    assert(k == s->n);
    if (front) {
        s->front = termsetfind(s, front);
    }
}

// Run in Terminal's rules: which tab of a set a code block may be typed
// into, when a Run still has its tab, and when a new tab is ready for
// the block it opened for. A tab is available when it waits at its
// shell's prompt with nothing running: by its shell's marks where its
// shell is hooked (ptyhost.cpp), else by what was typed last (a line
// entered, or nothing) and the engine's busy, which it polls each
// second. Times are µs (os_ticks).

enum : i32 {
    TERMK_OTHER,  // a shell the rules know nothing of (fish), an editor
    TERMK_SH,     // as RUN_SH: a POSIX shell
    TERMK_PS,     // PowerShell
    TERMK_CMD,    // cmd.exe
};

enum : i64 {
    TERM_HOOKWAIT = 10*1000*1000,  // for a hooked shell's first mark
    TERM_HOLD     = 1500*1000,     // after typing: busy may not be in yet
    TERM_RAN      = 1000*1000,     // a Run just over still has its tab
    TERM_PENDHOOK = 100*1000,      // quiet after the first prompt's mark
    TERM_PENDQUIET = 500*1000,     // quiet before an unhooked tab's Run
    TERM_PENDMAX  = 15*1000*1000,  // a Run waits for its new tab no more
};

// What the rules know of a tab (term.cpp keeps it, from term.open's
// reply, the shell's output, and what the user types).
struct TermState {
    b32 live;      // its shell runs, and its host takes input
    i32 kind;      // TERMK_*
    b32 hooked;    // its reply had a nonce
    b32 marked;    // a mark with it came
    i64 liveat;
    b32 busy;      // a program has the terminal (term.busy)
    b32 typed;     // anything typed since it went live
    b32 typedcr;   // the last typed input ended in Enter
    i64 typedat;
    b32 prompted;  // a prompt's A since the last typing, and no C
    i64 outat;     // its last output
    b32 queued;    // a Run waits to be typed into it
    u64 runkey;    // the last Run typed into it, else 0
    i64 sentat;
    i64 doneat;    // its prompt came back after that Run, else 0
    Str dir;       // its shell's folder (OSC 7), else empty: not known
};

// Whether folders a and b, as a shell or a session gives them, are the
// same: a trailing separator aside, on Windows of any case and either
// slash.
static b32 termsamedir(Str a, Str b, b32 win)
{
    for (i32 k = 0; k < 2; k++) {
        Str *p = k ? &b : &a;
        while (p->len>1 && ((*p)[p->len-1]=='/' || (win && (*p)[p->len-1]=='\\')) &&
               !(win && p->len==3 && (*p)[1]==':')) {
            p->len--;
        }
    }
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        u8 x = a[i];
        u8 y = b[i];
        if (win) {
            x = x=='\\' ? '/' : x>='A' && x<='Z' ? (u8)(x + 32) : x;
            y = y=='\\' ? '/' : y>='A' && y<='Z' ? (u8)(y + 32) : y;
        }
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

// Whether its marks can be trusted to come: a hooked shell that marked
// a prompt, or whose first prompt may yet.
static b32 termhooked(TermState *st, i64 now)
{
    return st->hooked && (st->marked || now<st->liveat+TERM_HOOKWAIT);
}

// Whether a code block of kind may be typed into the tab now.
static b32 termavail(TermState *st, i32 kind, i64 now)
{
    if (!st->live || !kind || st->kind!=kind || st->busy || st->queued) {
        return 0;
    } else if (termhooked(st, now)) {
        // At a prompt, whatever the last key: a ^C there redraws it, but
        // a quote left open (PS2) never brings an A
        return st->prompted;
    }
    i64 since = st->typedat>st->liveat ? st->typedat : st->liveat;
    return (!st->typed || st->typedcr) && now>=since+TERM_HOLD;
}

// Whether a block for folder dir (empty: any) may go to the tab: its
// shell's folder is that one, or not known.
static b32 termindir(TermState *st, Str dir, b32 win)
{
    return !dir.len || !st->dir.len || termsamedir(st->dir, dir, win);
}

// Of a set's tabs, whose states are sts in the set's order, the one a
// block of kind for folder dir goes to: the front one if available,
// else the right-most available; -1 if none. A tab known to be in
// another folder is passed over, unless any.
static i32 termsetpick(TermSet *s, TermState *sts, i32 kind, i64 now, Str dir = {},
                       b32 win = 0, b32 any = 1)
{
    if (s->n && termavail(&sts[s->front], kind, now) &&
        (any || termindir(&sts[s->front], dir, win))) {
        return s->front;
    }
    for (i32 i = s->n-1; i >= 0; i--) {
        if (termavail(&sts[i], kind, now) && (any || termindir(&sts[i], dir, win))) {
            return i;
        }
    }
    return -1;
}

enum : i32 {
    TERMHELD_NONE,
    TERMHELD_RUNS,  // the block was typed into it and runs still
    TERMHELD_RAN,   // it was, and its prompt just came back
};

// Whether a tab still has a block's Run (by its key): a click on its
// Run button then shows the tab, and runs nothing again. Typing into
// the tab since lets it go.
static i32 termrunheld(TermState *st, u64 key, i64 now)
{
    if (!key || st->runkey!=key || !st->live || (st->typed && st->typedat>st->sentat)) {
        return TERMHELD_NONE;
    } else if (termhooked(st, now)) {
        return !st->doneat ? TERMHELD_RUNS :
               now<st->doneat+TERM_RAN ? TERMHELD_RAN : TERMHELD_NONE;
    }
    return st->busy || now<st->sentat+TERM_HOLD ? TERMHELD_RUNS : TERMHELD_NONE;
}

enum : i32 {
    TERMPEND_WAIT,
    TERMPEND_GO,    // type it now
    TERMPEND_DROP,  // it waited too long, or the tab is no longer for it
};

// Whether a new tab is ready for the Run it opened for, queued at
// pendat: at its first prompt, once its output has paused (zsh turns on
// bracketed paste just after drawing its prompt), or, unhooked, live,
// idle and quiet. Typing first, or the tab's end, drops it.
static i32 termpendgo(TermState *st, b32 ended, i64 pendat, i64 now)
{
    if (ended || st->typed) {
        return TERMPEND_DROP;
    } else if (now >= pendat+TERM_PENDMAX) {
        return TERMPEND_DROP;
    } else if (!st->live) {
        return TERMPEND_WAIT;
    } else if (termhooked(st, now)) {
        return st->prompted && now>=st->outat+TERM_PENDHOOK ? TERMPEND_GO : TERMPEND_WAIT;
    }
    i64 since = st->outat>st->liveat ? st->outat : st->liveat;
    return !st->busy && now>=since+TERM_PENDQUIET ? TERMPEND_GO : TERMPEND_WAIT;
}

// The next time, after now, at which the rules may answer otherwise for
// a tab with nothing new from its shell, else 0: for a tooltip or a
// waiting Run to look again.
static i64 termlookdue(TermState *st, i64 now)
{
    i64 at[] = {
        st->liveat + TERM_HOOKWAIT,
        (st->typedat>st->liveat ? st->typedat : st->liveat) + TERM_HOLD,
        st->sentat + TERM_HOLD,
        st->doneat ? st->doneat+TERM_RAN : 0,
        st->outat + TERM_PENDHOOK,
        (st->outat>st->liveat ? st->outat : st->liveat) + TERM_PENDQUIET,
    };
    i64 next = 0;
    for (iz i = 0; i < countof(at); i++) {
        if (at[i]>now && (!next || at[i]<next)) {
            next = at[i];
        }
    }
    return next;
}

// [terminal] scrollback as a tab's history lines, 0 to 100000, the
// default for what isn't a number.
static i32 termscrollparse(Str v)
{
    Int n = parseint(v);
    if (!n.ok) {
        return 10000;
    }
    return n.val<0 ? 0 : n.val>100000 ? 100000 : (i32)n.val;
}

// History for that many lines: ariel's ring in proportion to its
// default (10000 lines in 4 MiB), at least 64 KiB; none for 0.
static iz termscrollbytes(i32 lines)
{
    iz n = (iz)lines * 420;
    return !lines ? 0 : n<((iz)1<<16) ? (iz)1<<16 : n;
}

// A title as a tab shows it, from the bytes a program set (OSC 0 or 2),
// into dst (TERMTITLECAP bytes): what the eye can't see dropped (the
// bidi and zero-width controls, C0 and C1), blank runs as one space,
// no blank at either end, cut at TERMTITLE code points with an
// ellipsis, and "##" broken up, which ImGui would take as an ID.
static iz termtitle(u8 *dst, Str raw)
{
    iz  len   = 0;
    i32 count = 0;
    b32 blank = 0;
    i32 prev  = 0;
    for (iz i = 0; i < raw.len;) {
        i32 c = utf8decode(raw, &i);
        if (c=='\t' || c=='\n' || c=='\r' || c==' ' || c==0x3000 ||
            (c>=0x2000 && c<=0x200a) || c==0xa0) {
            blank = len > 0;
            continue;
        } else if (webinvisible(c) || (c>=0xd800 && c<=0xdfff)) {
            continue;
        }
        i32 add = blank + 1 + (c=='#' && prev=='#' && !blank);
        if (count+add > TERMTITLE) {
            len += utf8encode(dst+len, 0x2026);
            return len;
        }
        if (blank) {
            dst[len++] = ' ';
            count++;
            prev = ' ';
        } else if (c=='#' && prev=='#') {
            dst[len++] = ' ';
            count++;
        }
        blank  = 0;
        len   += utf8encode(dst+len, c);
        count++;
        prev   = c;
    }
    return len;
}
