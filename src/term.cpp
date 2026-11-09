// The Terminal pane: a shell on a pseudo-terminal in the session's
// engine (ptyhost.cpp), emulated here by ariel, in tabs: a set of up to
// eight per root session (termset.cpp), which its subagents and side
// chats share.
//
// The engine streams the shell's output as term records, each at its
// offset; the pane feeds them to ariel and acks the offset, which opens
// the engine's window for more. A gap (a record lost) acks with resend,
// and the engine sends again from there. Keys and pastes go out encoded
// by ariel as term.input. Ctrl+` (physical Ctrl on macOS too) shows the
// pane and focuses the terminal, starting one if there is none, and
// from the terminal goes back to the prompt, hiding the pane only where
// it covers the prompt or floats.
//
// For now only the local engine runs terminals: the routing of remote
// hosts' term records comes later.
//
// This is free and unencumbered software released into the public domain.

#include "../ariel/ariel.h"

enum : i32 {
    TT_NONE,      // not started
    TT_OPENING,   // term.open in flight
    TT_LIVE,
    TT_ENDED,     // the shell exited, its screen kept under a banner
    TT_FAILED,    // could not start
};

enum : i32 {
    TERMTAB_TEXT   = 160,       // bytes kept of its shell's name and errors
    TERMTAB_RESIZE = 100*1000,  // µs a new size waits to settle
    TERMTAB_INPUT  = 16 << 10,  // bytes per term.input (ptyhost's TERM_INPUT)
    TERMTAB_OPEN   = 16,        // open on one engine (ptyhost's TERM_PERPEER)
};

struct TermTab {
    TermTab    *next;      // on the spare list, once closed
    u32         serial;    // its tab's ID, kept through a restart
    GuiHost    *host;
    i64         tid;       // the engine's, 0 until term.open's reply
    i64         openid;    // term.open in flight, else 0
    i32         state;     // TT_*
    Region     *region;    // ariel's block, alone in it
    ariel_Term *t;
    i32         capcols;   // the block's capacity
    i32         caprows;
    i32         cols;      // its size, as the engine has it
    i32         rows;
    i32         wantcols;  // a new size, waiting to settle, else 0
    i32         wantrows;
    i64         wantat;    // when it last changed, sched µs
    i64         fed;       // output offset fed to ariel
    u32         mheld;     // mouse buttons whose press went to the app
    f32         wheel;     // wheel notches not yet sent, fractions kept
    b32         pasteins;  // Insert, held since it pasted with Shift
    i32         textkey;   // a Ctrl+Alt key that typed (AltGr), while held
    b32         busy;      // a program runs in the shell
    b32         selecting; // the left button drags a local selection
    b32         copyc;     // C, held since Ctrl+C copied (Windows)
    i32         histlines; // its history, as [terminal] scrollback said
    Palette    *pal;       // the colors given ariel
    i32         code;      // the end's exit code, or -1
    i32         signal;
    u8          shell[TERMTAB_TEXT];  // its shell's name
    iz          shelllen;
    u8          err[TERMTAB_TEXT];    // why it failed or ended
    iz          errlen;
    u8          title[TERMTITLECAP];  // its program's, as shown (termtitle)
    iz          titlelen;
};

static iz termkeep(u8 *dst, Str s)
{
    iz n = s.len<TERMTAB_TEXT ? s.len : (iz)TERMTAB_TEXT;
    copybytes(dst, s.data, n);
    return n;
}

static Str termshellname(TermTab *tt)
{
    return {tt->shell, tt->shelllen};
}

static Str termerr(TermTab *tt)
{
    return {tt->err, tt->errlen};
}

// A tab's label: its program's title, else its shell's name. ConPTY
// titles a console with its program's path, which names it as a shell's
// would be (C:\...\sh.EXE is sh).
static Str termlabel(TermTab *tt)
{
    if (tt->titlelen) {
        Str title = {tt->title, tt->titlelen};
        Str name  = basename(title);
        Str ext   = cuthead(name, name.len<4 ? name.len : name.len-4);
        b32 exe   = ext.len==4 && ext[0]=='.' && (ext[1]|0x20)=='e' &&
                    (ext[2]|0x20)=='x' && (ext[3]|0x20)=='e';
        if (exe && name.len>4 && name.len<title.len) {
            return takehead(name, name.len-4);
        }
        return title;
    }
    Str shell = basename(termshellname(tt));
    return shell.len ? shell : Str("Terminal");
}

// Why a session can have no terminal now, else empty.
static Str termwhy(Gui *g, GuiSession *s)
{
    if (!s) {
        return "No session is open";
    } else if ((s->parent.len || (s->view && s->view->parent.len)) && !sessup(g, s)) {
        return "Its parent session isn't listed yet";
    } else if (s->host->kind != GH_ENGINE) {
        return "Terminals run on this machine's sessions for now";
    } else if (g->enginekind == ENGINE_FAKE) {
        return "The fake agent has no terminals";
    }
    return hostwhy(g, s, "term");
}

// ariel's colors in its own form, 0xRRGGBB, from a palette's ABGR.
static u32 termrgb(u32 abgr)
{
    return (abgr & 0xff)<<16 | (abgr & 0xff00) | (abgr>>16 & 0xff);
}

// ImGui's color, ABGR, from ariel's.
static u32 termimcolor(u32 rgb)
{
    return 0xff000000u | (rgb & 0xff)<<16 | (rgb & 0xff00) | (rgb>>16 & 0xff);
}

static void termcolors(Gui *g, TermTab *tt)
{
    Palette *p = g->pal;
    tt->pal = p;
    for (i32 i = 0; i < 16; i++) {
        ariel_setcolor(tt->t, i, termrgb(p->ansi[i]));
    }
    ariel_setcolor(tt->t, ARIEL_C_FG, termrgb(p->termfg));
    ariel_setcolor(tt->t, ARIEL_C_BG, termrgb(p->termbg));
    ariel_setcolor(tt->t, ARIEL_C_CURSOR, termrgb(p->termcursor));
}

// ariel's configuration for a tab, history lines (0 for none) given.
static ariel_Config termconfig(i32 cols, i32 rows, i32 capcols, i32 caprows, i32 hist)
{
    static char name[] = "Prospero(" PROSPERO_VERSION ")";
    ariel_Config c = {};
    c.cols      = cols;
    c.rows      = rows;
    c.capcols   = capcols;
    c.caprows   = caprows;
    c.histlines = hist ? hist : -1;  // ariel's 0 is its default
    c.histbytes = termscrollbytes(hist);
    c.name.data = (u8 *)name;
    c.name.len  = countof(name) - 1;
    return c;
}

// The capacity for a size: some room to grow before a move.
static i32 termcap(i32 n, i32 least)
{
    i32 r = n + n/2;
    r = r<least ? least : r;
    return r<4096 ? r : 4096;
}

// A block for the terminal, fresh or moved into from the one it had,
// with the capacity for cols by rows. Returns 0 if it cannot be had.
static b32 termblock(Gui *g, TermTab *tt, i32 cols, i32 rows)
{
    i32          capcols = termcap(cols, 200);
    i32          caprows = termcap(rows, 80);
    ariel_Config c       = termconfig(cols, rows, capcols, caprows, tt->histlines);
    iz           size    = ariel_size(&c);
    if (!size) {
        return 0;
    }
    Region *r = os_reserve(g->os, size + ((iz)1<<16));
    if (!r) {
        return 0;
    }
    g->termregions++;
    Arena a = newarena(r);
    allocbytes(&a, -(uz)a.beg & 15);
    void       *mem = allocbytes(&a, size);
    ariel_Term *t   = tt->t ? ariel_move(tt->t, mem, size, &c) : ariel_init(mem, size, &c);
    if (!t) {
        os_release(g->os, r);
        g->termregions--;
        return 0;
    }
    if (tt->region) {
        os_release(g->os, tt->region);
        g->termregions--;
    }
    tt->region  = r;
    tt->t       = t;
    tt->capcols = capcols;
    tt->caprows = caprows;
    return 1;
}

// Let go of a terminal's block and state, keeping its tab.
static void termfree(Gui *g, TermTab *tt)
{
    if (tt->region) {
        os_release(g->os, tt->region);
        g->termregions--;
    }
    GuiHost *h      = tt->host;
    u32      serial = tt->serial;
    *tt = {};
    tt->host   = h;
    tt->serial = serial;
}

// Begin a terminal's command, which has no reply. Finish it with cmdsend.
static EvOut termbegin(Gui *g, Buf *b, i32 cmd, TermTab *tt)
{
    EvOut w = cmdbegin(b, cmd, 0);
    putintkey(&w.json, "tid", tt->tid);
    g->cmdhost = tt->host;
    return w;
}

static void termack(Gui *g, TermTab *tt, i64 off, b32 resend)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = termbegin(g, &b, CMD_TERMACK, tt);
    putintkey(&w.json, "off", off);
    if (resend) {
        putkey(&w.json, "resend");
        putbool(&w.json, 1);
    }
    cmdsend(g, &w, a);
}

// Bytes for the shell, in as many term.input as they need.
static void terminput(Gui *g, TermTab *tt, Str data)
{
    if (tt->state!=TT_LIVE || !tt->tid) {
        return;
    }
    while (data.len) {
        Str   part = takehead(data, data.len<TERMTAB_INPUT ? data.len : (iz)TERMTAB_INPUT);
        Arena a    = newarena(g->cmdr);
        Buf   b(&a, 128 + part.len/3*4 + 4);
        EvOut w    = termbegin(g, &b, CMD_TERMINPUT, tt);
        putstrkey(&w.json, "data", base64(&a, part));
        cmdsend(g, &w, a);
        data = cuthead(data, part.len);
    }
}

// Bytes the user sent, typed, pasted or clicked: the view goes back to
// the bottom first, where they land. Replies go through terminput.
static void termsend(Gui *g, TermTab *tt, Str data)
{
    if (data.len && tt->t && ariel_view(tt->t).offset) {
        ariel_scrollto(tt->t, ARIEL_TO_BOTTOM);
        scheddirty(&g->sched);
    }
    terminput(g, tt, data);
}

static void termsize(Gui *g, TermTab *tt, i32 cols, i32 rows, f32 cw, f32 ch)
{
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 128);
    EvOut w = termbegin(g, &b, CMD_TERMRESIZE, tt);
    putintkey(&w.json, "cols", cols);
    putintkey(&w.json, "rows", rows);
    putintkey(&w.json, "xpx", (i64)roundf((f32)cols*cw));
    putintkey(&w.json, "ypx", (i64)roundf((f32)rows*ch));
    cmdsend(g, &w, a);
}

// Hang up a terminal and forget it: its shell gets SIGHUP, as a closed
// terminal window's would. With kill, its whole process tree goes too.
// An ended terminal is closed as well, so the engine lets go of it now
// rather than when its unacked output gives up.
static void termclose(Gui *g, TermTab *tt, b32 kill)
{
    if (tt->tid && tt->state>=TT_OPENING && tt->state<=TT_ENDED) {
        Arena a = newarena(g->cmdr);
        Buf   b(&a, 128);
        EvOut w = termbegin(g, &b, CMD_TERMCLOSE, tt);
        if (kill) {
            putkey(&w.json, "kill");
            putbool(&w.json, 1);
        }
        cmdsend(g, &w, a);
    }
    termfree(g, tt);
    scheddirty(&g->sched);
}

// The root whose terminal tabs a session shows: a subagent's, at the
// top of its parents, and a side chat's original's, in the same folder.
// A worktree session, in a folder of its own, has its own.
static Str termcwd(GuiSession *s)
{
    return s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
}

static GuiSession *termroot(Gui *g, GuiSession *s)
{
    s = sessroot(g, s);
    for (i32 n = 0; n < 8; n++) {
        Str side = sessside(s);
        if (!side.len || s->wtname.len) {
            break;
        }
        GuiSession *o = findsess(g, s->host, side);
        if (!o || o==s || o->gone || !(termcwd(o) == termcwd(s))) {
            break;
        }
        s = sessroot(g, o);
    }
    return s;
}

// The shown session's front tab, else null.
static TermTab *termfront(Gui *g)
{
    GuiSession *s = g->sess ? termroot(g, g->sess) : 0;
    return s ? termsetfront(&s->terms) : 0;
}

// The root whose set holds a tab.
static GuiSession *termowner(Gui *g, TermTab *tt)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (termsetfind(&s->terms, tt) >= 0) {
            return s;
        }
    }
    return 0;
}

// Why a root can have no new tab now, else empty.
static Str termfull(Gui *g, GuiSession *s)
{
    if (s->terms.n == TERMTABS) {
        return "A session has at most 8 terminal tabs";
    }
    i32 open = 0;
    for (GuiSession *o = g->sessions; o; o = o->next) {
        for (i32 i = 0; i < o->terms.n; i++) {
            TermTab *tt = o->terms.tabs[i];
            open += tt->host==s->host && (tt->state==TT_OPENING || tt->state==TT_LIVE);
        }
    }
    return open>=TERMTAB_OPEN ? Str("16 terminals are open already") : Str{};
}

// Close tab i of a root, which takes it out of the set, to the spares.
static void termdrop(Gui *g, GuiSession *s, i32 i, b32 kill)
{
    TermTab *tt = s->terms.tabs[i];
    termclose(g, tt, kill);
    termsetdrop(&s->terms, i);
    tt->next     = g->termspare;
    g->termspare = tt;
}

// Tabs whose shell runs a program, which quitting would hang up.
static i32 termbusy(Gui *g)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        for (i32 i = 0; i < s->terms.n; i++) {
            TermTab *tt = s->terms.tabs[i];
            n += tt->state==TT_LIVE && tt->busy;
        }
    }
    return n;
}

// Hang up every terminal, as the window goes.
static void termcloseall(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        while (s->terms.n) {
            termdrop(g, s, s->terms.n-1, 0);
        }
    }
}

// Each frame: close the terminals of sessions deleted or put in the
// Trash, here or by another window (their engine hung them up already),
// and end those of an engine that stopped, which says nothing more.
static void termsweep(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->gone || s->trashed) {
            while (s->terms.n) {
                termdrop(g, s, s->terms.n-1, 0);
            }
            continue;
        }
        for (i32 i = 0; i < s->terms.n; i++) {
            TermTab *tt = s->terms.tabs[i];
            if (g->enginegone && tt->host->kind==GH_ENGINE &&
                (tt->state==TT_LIVE || tt->state==TT_OPENING)) {
                tt->state  = TT_ENDED;
                tt->tid    = 0;
                tt->openid = 0;
                tt->busy   = 0;
                tt->errlen = termkeep(tt->err, "the engine stopped");
                scheddirty(&g->sched);
            }
        }
    }
}

// Start a shell in a tab of root s, at a size.
static void termlaunch(Gui *g, GuiSession *s, TermTab *tt, i32 cols, i32 rows)
{
    tt->host = s->host;
    if (!tt->t) {
        tt->histlines = g->termscroll;  // kept through moves and restarts
    }
    cols = cols>0 ? cols : 80;
    rows = rows>0 ? rows : 24;
    if (!termblock(g, tt, cols, rows)) {
        tt->state  = TT_FAILED;
        tt->errlen = termkeep(tt->err, "out of memory for the terminal");
        return;
    }
    termcolors(g, tt);
    tt->cols   = cols;
    tt->rows   = rows;
    tt->code   = -1;
    tt->state  = TT_OPENING;

    Arena a = newarena(g->cmdr);
    Buf   b(&a, 256);
    EvOut w = cmdopen(g, &b, CMD_TERMOPEN, s);
    putintkey(&w.json, "cols", cols);
    putintkey(&w.json, "rows", rows);
    tt->openid = g->nextid;
    cmdsend(g, &w, a);
    scheddirty(&g->sched);
}

// A new tab for root s, shown, its shell starting; null if it can have
// none (termwhy, termfull).
static TermTab *termnew(Gui *g, GuiSession *s, i32 cols, i32 rows)
{
    if (termwhy(g, s).len || termfull(g, s).len) {
        return 0;
    }
    TermTab *tt = g->termspare;
    if (tt) {
        g->termspare = tt->next;
        *tt = {};
    } else {
        tt = alloc<TermTab>(&g->perm);
    }
    tt->serial = ++g->termserial;
    termsetadd(&s->terms, tt);
    termlaunch(g, s, tt, cols, rows);
    return tt;
}

// A new shell in the same tab, the old one hung up.
static void termrestart(Gui *g, GuiSession *s, TermTab *tt, i32 cols, i32 rows)
{
    termclose(g, tt, 0);
    if (!termwhy(g, s).len) {
        termlaunch(g, s, tt, cols, rows);
    }
}

// term.open's reply.
static void termopened(Gui *g, GuiSession *s, i64 id, Json *j, b32 ok, Str err)
{
    TermTab *tt = 0;
    for (i32 i = 0; s && i<s->terms.n; i++) {
        if (s->terms.tabs[i]->openid == id) {
            tt = s->terms.tabs[i];
        }
    }
    if (!tt || !id) {
        // Abandoned while opening: hang it up
        i64 tid = jsonint(jsonget(j, "tid"), 0);
        if (ok && tid && s) {
            TermTab gone = {};
            gone.host  = s->host;
            gone.tid   = tid;
            gone.state = TT_LIVE;
            termclose(g, &gone, 0);
        }
        return;
    }
    tt->openid = 0;
    if (!ok) {
        tt->state  = TT_FAILED;
        tt->errlen = termkeep(tt->err, err);
        return;
    }
    tt->tid      = jsonint(jsonget(j, "tid"), 0);
    tt->state    = TT_LIVE;
    tt->shelllen = termkeep(tt->shell, jsonstr(jsonget(j, "shell")));
    ariel_Modes m = ariel_modes(tt->t);
    if (m.cols!=tt->cols || m.rows!=tt->rows) {
        // Resized while opening
        tt->cols = m.cols;
        tt->rows = m.rows;
        termsize(g, tt, m.cols, m.rows, 0, 0);
    }
}

static TermTab *termbytid(Gui *g, GuiHost *h, i64 tid)
{
    for (GuiSession *s = g->sessions; tid && s; s = s->next) {
        for (i32 i = 0; i < s->terms.n; i++) {
            TermTab *tt = s->terms.tabs[i];
            if (tt->host==h && tt->tid==tid) {
                return tt;
            }
        }
    }
    return 0;
}

// Whether tab tt shows: as the shown root's front tab with the pane
// visible (front), else as a tab in its bar.
static b32 termshown(Gui *g, TermTab *tt, b32 front)
{
    GuiSession *s = g->sess ? termroot(g, g->sess) : 0;
    if (!s || !g->panes[PANE_TERMINAL]) {
        return 0;
    }
    return front ? termsetfront(&s->terms)==tt : termsetfind(&s->terms, tt)>=0;
}

// Feed the shell's output to ariel, sending its replies back.
static void termfeed(Gui *g, TermTab *tt, Str data)
{
    ariel_Str in = {data.data, data.len};
    for (;;) {
        ariel_Fed f = ariel_feed(tt->t, in);
        in = f.rest;
        switch (f.ev.kind) {
        case ARIEL_EV_NONE:
            return;
        case ARIEL_EV_REPLY:
            terminput(g, tt, {f.ev.data.data, f.ev.data.len});
            break;
        case ARIEL_EV_TITLE:
            if (!f.ev.arg) {
                // The window's title names its tab, drawn again if shown
                u8 was[TERMTITLECAP];
                iz waslen = tt->titlelen;
                __builtin_memcpy(was, tt->title, (uz)waslen);
                tt->titlelen = termtitle(tt->title, {f.ev.data.data, f.ev.data.len});
                if (!(Str{was, waslen} == Str{tt->title, tt->titlelen}) &&
                    termshown(g, tt, 0)) {
                    scheddirty(&g->sched);
                }
            }
            break;
        }
    }
}

// A term, term.end or term.busy record from a host.
// A record from a host, applied; true when it changed what shows.
static b32 termapply(Gui *g, GuiHost *h, Arena scratch, EvRec *r)
{
    Json    *j   = r->root;
    i64      tid = jsonint(jsonget(j, "tid"), 0);
    TermTab *tt  = termbytid(g, h, tid);
    if (!tt) {
        if (tid && r->ev!=EV_TERMBUSY) {
            // A terminal not known here, say one of a window before
            TermTab gone = {};
            gone.host  = h;
            gone.tid   = tid;
            gone.state = TT_LIVE;
            termclose(g, &gone, 0);
        }
        return 0;
    }

    switch (r->ev) {
    case EV_TERM: {
        if (tt->state != TT_LIVE) {
            return 0;
        }
        i64      off = jsonint(jsonget(j, "off"), -1);
        Unbase64 u   = unbase64(&scratch, jsonstr(jsonget(j, "data")));
        if (!u.ok || off<0) {
            return 0;
        } else if (off > tt->fed) {
            termack(g, tt, tt->fed, 1);  // a gap: from where it was lost
            return 0;
        } else if (off+u.data.len <= tt->fed) {
            return 0;  // sent again, already had
        }
        Str fresh = cuthead(u.data, (iz)(tt->fed - off));
        termfeed(g, tt, fresh);
        tt->fed += fresh.len;
        termack(g, tt, tt->fed, 0);
        return termshown(g, tt, 1);  // hidden tabs parse, unseen
    }

    case EV_TERMEND: {
        i64 off = jsonint(jsonget(j, "off"), -1);
        tt->code   = (i32)jsonint(jsonget(j, "code"), -1);
        tt->signal = (i32)jsonint(jsonget(j, "signal"), 0);
        tt->busy   = 0;
        Str err    = jsonstr(jsonget(j, "err"));
        if (off >= 0) {
            termack(g, tt, off<tt->fed ? off : tt->fed, 0);
        }
        if (!tt->code && !tt->signal && !err.len && off==tt->fed) {
            // A clean exit closes its tab
            GuiSession *o = termowner(g, tt);
            termdrop(g, o, termsetfind(&o->terms, tt), 0);
            if (!o->terms.n && g->panes[PANE_TERMINAL] && g->termfocused) {
                g->promptfocus = 1;
            }
            return 1;
        }
        tt->state  = TT_ENDED;
        tt->errlen = termkeep(tt->err, err);
        return 1;
    }

    case EV_TERMBUSY: {
        b32 was  = tt->busy;
        tt->busy = jsonflag(jsonget(j, "busy"), 0);
        return was!=tt->busy && termshown(g, tt, 0);  // its dot
    }
    }
    return 0;
}


// Drawing

struct TermMetrics {
    ImFontBaked *regular;
    ImFontBaked *bold;
    f32          size;     // the font's
    f32          cw, ch;   // a cell, in points, whole device pixels
    f32          density;
};

static f32 termsnap(f32 v, f32 density)
{
    return roundf(v*density) / density;
}

static TermMetrics termmetrics(Gui *g)
{
    TermMetrics m = {};
    f32 density = ImGui::GetIO().DisplayFramebufferScale.x;
    m.density = density>0 ? density : 1;
    m.regular = stylebaked(&g->fonts, STYLE_CODE);
    m.bold    = stylebaked(&g->fonts, STYLE_CODE|STYLE_BOLD);
    m.size    = stylesize(&g->fonts, STYLE_CODE);
    f32 cw = termsnap(m.regular->GetCharAdvance('M'), m.density);
    f32 ch = termsnap(m.size*1.25f, m.density);
    m.cw = cw>0 ? cw : 1;
    m.ch = ch>0 ? ch : 1;
    return m;
}

// One glyph's quad, its top left at x, y.
static void termglyph(ImDrawList *dl, ImFontBaked *f, f32 x, f32 y, u32 cp, u32 col)
{
    ImFontGlyph *glyph = f->FindGlyph((ImWchar)cp);
    if (!glyph || !glyph->Visible) {
        return;
    }
    col |= glyph->Colored ? ~IM_COL32_A_MASK : 0;
    dl->PrimReserve(6, 4);
    dl->PrimRectUV(v2(x + glyph->X0, y + glyph->Y0), v2(x + glyph->X1, y + glyph->Y1),
                   v2(glyph->U0, glyph->V0), v2(glyph->U1, glyph->V1), col);
}

// The cells of the screen at o: backgrounds, glyphs, lines, cursor.
// A cell's background as drawn, ImGui's color, 0 for the default's.
static u32 termcellbg(Gui *g, ariel_Cell *c)
{
    if (c->attr & ARIEL_A_SELECTED) {
        return g->pal->sel;
    }
    return c->attr&ARIEL_A_DEFBG ? 0 : termimcolor(c->bg);
}

static void termcells(Gui *g, TermTab *tt, TermMetrics *m, ImVec2 o, b32 focused, Arena a)
{
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    ariel_Modes modes = ariel_modes(tt->t);
    i32         cols  = modes.cols;
    ariel_Cell *cells = alloc<ariel_Cell>(&a, cols);
    f32         cw = m->cw, ch = m->ch;
    f32         top = termsnap((ch - m->size)/2, m->density);  // the glyphs' in a cell
    dl->PushDrawFlag(ImDrawFlags_TextNoPixelSnap, true);

    for (i32 y = 0; y < modes.rows; y++) {
        i32 n  = ariel_row(tt->t, y, cells, cols);
        f32 cy = o.y + (f32)y*ch;

        // Backgrounds, in runs of one color, the default left as it is
        for (i32 x = 0; x < n;) {
            u32 bg  = termcellbg(g, cells + x);
            i32 end = x + 1;
            for (; end<n && termcellbg(g, cells+end)==bg; end++) {}
            if (bg) {
                dl->AddRectFilled(v2(o.x + (f32)x*cw, cy), v2(o.x + (f32)end*cw, cy + ch), bg);
            }
            x = end;
        }

        for (i32 x = 0; x < n; x++) {
            ariel_Cell *c = cells + x;
            if (!c->width) {
                continue;  // the right half of a wide character
            }
            f32 cx  = o.x + (f32)x*cw;
            u32 fg  = c->fg;
            if (c->attr & ARIEL_A_FAINT) {
                // Halfway to the background
                u32 bg = c->bg;
                fg = ((fg>>1 & 0x7f7f7f) + (bg>>1 & 0x7f7f7f));
            }
            u32 col = c->attr&ARIEL_A_SELECTED ? g->pal->seltext : termimcolor(fg);
            if (c->cp && c->cp!=' ' && !(c->attr & ARIEL_A_HIDDEN)) {
                ImFontBaked *f = c->attr&ARIEL_A_BOLD ? m->bold : m->regular;
                termglyph(dl, f, cx, cy + top, c->cp, col);
            }
            f32 w = (f32)c->width * cw;
            f32 t = termsnap(m->size/14 > 1 ? m->size/14 : 1, m->density);
            if (ARIEL_ULINE(c->attr)) {
                f32 uy = termsnap(cy + top + m->regular->Ascent + t, m->density);
                dl->AddRectFilled(v2(cx, uy), v2(cx + w, uy + t), termimcolor(c->ul));
            }
            if (c->attr & ARIEL_A_STRIKE) {
                f32 sy = termsnap(cy + ch/2, m->density);
                dl->AddRectFilled(v2(cx, sy), v2(cx + w, sy + t), col);
            }
        }
    }

    // The cursor: a block, else a bar or underline; hollow unfocused
    ariel_Cursor cur = ariel_cursor(tt->t);
    if (cur.visible && cur.y>=0 && cur.y<modes.rows && cur.x>=0 && cur.x<cols &&
            tt->state==TT_LIVE) {
        f32 cx  = o.x + (f32)cur.x*cw;
        f32 cy  = o.y + (f32)cur.y*ch;
        f32 w   = (cur.wide ? 2 : 1) * cw;
        u32 col = termimcolor(cur.color);
        f32 t   = termsnap(m->size/8 > 1 ? m->size/8 : 1, m->density);
        if (!focused) {
            dl->AddRect(v2(cx, cy), v2(cx + w, cy + ch), col, 0.0f, 1.0f);
        } else if (cur.shape == ARIEL_CUR_BAR) {
            dl->AddRectFilled(v2(cx, cy), v2(cx + t, cy + ch), col);
        } else if (cur.shape == ARIEL_CUR_UNDERLINE) {
            dl->AddRectFilled(v2(cx, cy + ch - t), v2(cx + w, cy + ch), col);
        } else {
            dl->AddRectFilled(v2(cx, cy), v2(cx + w, cy + ch), col);
            ariel_row(tt->t, cur.y, cells, cols);
            ariel_Cell *c = cells + cur.x;
            if (c->cp && c->cp!=' ' && !(c->attr & ARIEL_A_HIDDEN)) {
                ImFontBaked *f  = c->attr&ARIEL_A_BOLD ? m->bold : m->regular;
                u32          bg = c->attr&ARIEL_A_DEFBG ? ariel_color(tt->t, ARIEL_C_BG) : c->bg;
                termglyph(dl, f, cx, cy + top, c->cp, termimcolor(bg));
            }
        }
    }
    dl->PopDrawFlag();
}


// Input

// The physical Ctrl key, which ImGui calls Super on macOS, swapping it
// with Cmd.
static b32 termctrl(Gui *g)
{
    ImGuiIO &io = ImGui::GetIO();
    return g->mac ? io.KeySuper : io.KeyCtrl;
}

static i32 termmods(Gui *g)
{
    ImGuiIO &io = ImGui::GetIO();
    i32      r  = 0;
    r |= io.KeyShift ? ARIEL_SHIFT : 0;
    r |= io.KeyAlt ? ARIEL_ALT : 0;
    r |= termctrl(g) ? ARIEL_CTRL : 0;
    return r;
}

// ariel's key for one of ImGui's named keys, else 0.
static i32 termnamed(ImGuiKey k)
{
    if (k>=ImGuiKey_F1 && k<=ImGuiKey_F12) {
        return ARIEL_K_F1 + (k - ImGuiKey_F1);
    } else if (k>=ImGuiKey_Keypad0 && k<=ImGuiKey_Keypad9) {
        return ARIEL_K_KP0 + (k - ImGuiKey_Keypad0);
    }
    switch (k) {
    case ImGuiKey_Enter:          return ARIEL_K_ENTER;
    case ImGuiKey_Tab:            return ARIEL_K_TAB;
    case ImGuiKey_Backspace:      return ARIEL_K_BACKSPACE;
    case ImGuiKey_Escape:         return ARIEL_K_ESCAPE;
    case ImGuiKey_Insert:         return ARIEL_K_INSERT;
    case ImGuiKey_Delete:         return ARIEL_K_DELETE;
    case ImGuiKey_Home:           return ARIEL_K_HOME;
    case ImGuiKey_End:            return ARIEL_K_END;
    case ImGuiKey_PageUp:         return ARIEL_K_PAGEUP;
    case ImGuiKey_PageDown:       return ARIEL_K_PAGEDOWN;
    case ImGuiKey_UpArrow:        return ARIEL_K_UP;
    case ImGuiKey_DownArrow:      return ARIEL_K_DOWN;
    case ImGuiKey_LeftArrow:      return ARIEL_K_LEFT;
    case ImGuiKey_RightArrow:     return ARIEL_K_RIGHT;
    case ImGuiKey_KeypadDecimal:  return ARIEL_K_KPDECIMAL;
    case ImGuiKey_KeypadDivide:   return ARIEL_K_KPDIVIDE;
    case ImGuiKey_KeypadMultiply: return ARIEL_K_KPMULTIPLY;
    case ImGuiKey_KeypadSubtract: return ARIEL_K_KPSUBTRACT;
    case ImGuiKey_KeypadAdd:      return ARIEL_K_KPADD;
    case ImGuiKey_KeypadEnter:    return ARIEL_K_KPENTER;
    case ImGuiKey_KeypadEqual:    return ARIEL_K_KPEQUAL;
    case ImGuiKey_Menu:           return ARIEL_K_MENU;
    default:                      return 0;
    }
}

// A text key's code point on a US layout, by its position, else 0: the
// fallback for Ctrl and Alt chords, which bring no text.
static u32 termtextkey(ImGuiKey k)
{
    if (k>=ImGuiKey_A && k<=ImGuiKey_Z) {
        return 'a' + (u32)(k - ImGuiKey_A);
    } else if (k>=ImGuiKey_0 && k<=ImGuiKey_9) {
        return '0' + (u32)(k - ImGuiKey_0);
    }
    switch (k) {
    case ImGuiKey_Space:        return ' ';
    case ImGuiKey_Apostrophe:   return '\'';
    case ImGuiKey_Comma:        return ',';
    case ImGuiKey_Minus:        return '-';
    case ImGuiKey_Period:       return '.';
    case ImGuiKey_Slash:        return '/';
    case ImGuiKey_Semicolon:    return ';';
    case ImGuiKey_Equal:        return '=';
    case ImGuiKey_LeftBracket:  return '[';
    case ImGuiKey_Backslash:    return '\\';
    case ImGuiKey_RightBracket: return ']';
    case ImGuiKey_GraveAccent:  return '`';
    case ImGuiKey_Oem102:       return '\\';  // ISO's extra key: \ in UK
    default:                    return 0;
    }
}

// A key pressed or repeated, as the shell should have it.
static void termpress(Gui *g, TermTab *tt, ImGuiKey k, b32 repeat)
{
    ariel_Key key = {};
    key.mods   = termmods(g);
    key.action = repeat ? ARIEL_REPEAT : ARIEL_PRESS;
    key.key    = termnamed(k);
    if (!key.key) {
        // Text keys go as their text, but for Ctrl and Alt chords, which
        // have none: as the key's code point under the layout, AltGr
        // included, so Ctrl+AltGr+8 is Ctrl+[ on a German layout. Where
        // that isn't ASCII (ü, Cyrillic) Ctrl goes by the US position,
        // so Ctrl+ü is Ctrl+[, as in VTE. On macOS, Option composes.
        b32 ctrl = key.mods & ARIEL_CTRL;
        b32 alt  = !g->mac && (key.mods & ARIEL_ALT);
        u32 us   = termtextkey(k);
        if (!(ctrl || alt) || !us || (ctrl && k==ImGuiKey_GraveAccent)) {
            return;
        }
        key.cp      = os_guikeycp(g->os, k, 0);
        key.shifted = os_guikeycp(g->os, k, 1);
        if (!key.cp) {
            key.cp = us;
        } else if (ctrl && key.cp>=0x80) {
            key.base = us;
        }
    }
    ariel_Str s = ariel_key(tt->t, &key);
    termsend(g, tt, {s.data, s.len});
    if (HOST_WINDOWS && !key.key) {
        // Windows translated the chord's key too, and a dead key's stays
        // pending (Alt+^ on German): the next key would compose with it.
        os_guiimereset(g->os);
    }
}

// Text for the shell as a paste, in pieces cut between code points, so
// that no piece ends partway into one ariel's filter would then pass.
static void termpastenow(Gui *g, TermTab *tt, Str text, Arena a)
{
    for (Str in = text; in.len;) {
        iz cut = in.len<TERMTAB_INPUT ? in.len : (iz)TERMTAB_INPUT;
        for (iz back = cut; cut<in.len && back>cut-4 && back>0; back--) {
            if ((in.data[back] & 0xc0) != 0x80) {
                cut = back;  // a code point starts here
                break;
            }
        }
        Str part  = takehead(in, cut);
        i32 flags = (part.len==in.len ? ARIEL_PASTE_END : 0) |
                    (in.data==text.data ? ARIEL_PASTE_BEGIN : 0);
        Arena t   = a;
        iz    cap = part.len + 16;
        u8   *out = allocbytes(&t, cap);
        iz    n   = ariel_paste(tt->t, out, cap, {part.data, part.len}, flags);
        termsend(g, tt, {out, n});
        in = cuthead(in, part.len);
    }
}

// The clipboard into tab tt, unless the shell isn't bracketing pastes
// and it has more than one line, each of which might then run: that
// waits in a block of its own for the user to say (termpasteask).
static void termpaste(Gui *g, TermTab *tt, Arena a)
{
    char const *clip = ImGui::GetClipboardText();
    Str         in   = {(u8 *)clip, clip ? (iz)__builtin_strlen(clip) : 0};
    if (g->drive && !(in==g->driveclip) && g->driveclip.len &&
            (os_guilimits(g->os) & GUI_NOSERIAL)) {
        // As driveclipboard says
        guilog(g, "drive: the window system refused the selection made by synthetic "
                  "input (no real input's serial): pasting what the GUI set");
        in = g->driveclip;
    }
    if (!in.len) {
        return;
    } else if (!ariel_pastecheck(tt->t, {in.data, in.len})) {
        termpastenow(g, tt, in, a);
        return;
    }
    if (g->termpasteregion) {
        os_release(g->os, g->termpasteregion);
    }
    g->termpasteregion = os_reserve(g->os, in.len);
    if (!g->termpasteregion) {
        return;
    }
    Arena r = newarena(g->termpasteregion);
    u8   *p = allocbytes(&r, in.len);
    __builtin_memcpy(p, in.data, (uz)in.len);
    g->termpastetext   = {p, in.len};
    g->termpastetab    = tt;
    g->termpasteserial = tt->serial;
    g->termpasteopen   = 1;
}

// The question a paste waits on, while it does.
static void termpasteask(Gui *g, Arena a)
{
    char const *title = "Paste into the terminal?";
    if (g->termpasteopen) {
        g->termpasteopen = 0;
        ImGui::ClearActiveID();
        ImGui::OpenPopup(title);
    }
    if (!ImGui::BeginPopupModal(title, 0, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Str text  = g->termpastetext;
    i64 lines = 1;
    for (iz i = 0; i < text.len; i++) {
        lines += text.data[i]=='\n' && i+1<text.len;
    }
    Buf b(&a, 160);
    print(&b, "The text has ");
    print(&b, lines);
    print(&b, " lines, and the program in the terminal doesn't take pastes "
              "as such: each line may run as a command.");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(cstr0(&a, finish(&b)));
    ImGui::PopTextWrapPos();
    b32 ok     = ImGui::Button("Paste");
    itemnamed(g, "term paste ok");
    ImGui::SameLine();
    b32 cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    itemnamed(g, "term paste cancel");
    if (ok || cancel) {
        TermTab *tt = g->termpastetab;
        if (ok && tt && tt->serial==g->termpasteserial && termowner(g, tt) &&
                tt->state==TT_LIVE) {
            termpastenow(g, tt, text, a);  // the tab may have gone meanwhile
        }
        os_release(g->os, g->termpasteregion);
        g->termpasteregion = 0;
        g->termpastetext   = {};
        g->termpastetab    = 0;
        g->termfocus       = 1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Whether a chord on Windows and Linux is the app's, not the terminal's:
// Ctrl+` and Ctrl+Shift+`, copy and paste, zoom, and the menus' Ctrl+Shift
// chords (menukeys), which route over the terminal. Every other Ctrl
// chord goes to the terminal: Ctrl+N, Ctrl+F, Ctrl+1 and the rest, and
// every chord with AltGr, which types (German Ctrl+AltGr+ß is Ctrl+\).
static b32 termkept(Gui *g, ImGuiKey k)
{
    ImGuiIO &io = ImGui::GetIO();
    if (g->mac || !io.KeyCtrl || io.KeyAlt || os_guialtgr(g->os)) {
        return 0;
    }
    switch (k) {
    case ImGuiKey_GraveAccent:
    case ImGuiKey_Equal:      // Ctrl++ is Ctrl+Shift+= on US layouts
        return 1;
    case ImGuiKey_KeypadAdd:
    case ImGuiKey_Minus:
    case ImGuiKey_0:
    case ImGuiKey_KeypadSubtract:
    case ImGuiKey_Keypad0:
        return !io.KeyShift;
    case ImGuiKey_V:          // Windows pastes with Ctrl+V too
        return io.KeyShift || HOST_WINDOWS;
    case ImGuiKey_C: case ImGuiKey_N:
    case ImGuiKey_S: case ImGuiKey_T: case ImGuiKey_R:
        return io.KeyShift;
    default:
        return 0;
    }
}

// Whether one of the app's chords (termkept) was pressed this frame.
static b32 termkeptpressed(Gui *g)
{
    static ImGuiKey const keys[] = {
        ImGuiKey_GraveAccent, ImGuiKey_Equal, ImGuiKey_KeypadAdd,
        ImGuiKey_Minus, ImGuiKey_0, ImGuiKey_KeypadSubtract, ImGuiKey_Keypad0,
        ImGuiKey_C, ImGuiKey_V, ImGuiKey_N, ImGuiKey_S, ImGuiKey_T, ImGuiKey_R,
    };
    for (iz i = 0; i < countof(keys); i++) {
        if (ImGui::IsKeyPressed(keys[i], false) && termkept(g, keys[i])) {
            return 1;
        }
    }
    return 0;
}

// The mouse over the grid, or holding a button pressed there: reported
// A tab leaving the front lets go of what it held: the app sees its
// mouse buttons released, and its partial wheel and held keys forgotten.
static void termletgo(Gui *g, TermTab *tt)
{
    static i32 const buttons[] = {1, 3, 2};  // ImGui's left, right, middle
    for (i32 b = 0; b<3 && tt->t && tt->state==TT_LIVE; b++) {
        if (tt->mheld & 1u<<b) {
            ariel_Mouse e = {};
            e.action = ARIEL_RELEASE;
            e.button = buttons[b];
            ariel_Str s = ariel_mouse(tt->t, &e);
            terminput(g, tt, {s.data, s.len});
        }
    }
    tt->mheld    = 0;
    tt->wheel    = 0;
    tt->pasteins  = 0;
    tt->textkey   = 0;
    tt->selecting = 0;
    tt->copyc     = 0;
}

// Whether tab tt has text selected.
static b32 termselected(TermTab *tt)
{
    return tt->t && ariel_copy(tt->t, 0, 0)>0;
}

// Copy tab tt's selection to the clipboard, if it has one.
static b32 termcopy(Gui *g, TermTab *tt)
{
    iz n = tt->t ? ariel_copy(tt->t, 0, 0) : 0;
    if (n <= 0) {
        return 0;
    }
    Arena a   = g->frame;
    u8   *buf = allocbytes(&a, n+1);
    ariel_copy(tt->t, buf, n);
    buf[n] = 0;
    ImGui::SetClipboardText((char *)buf);
    return 1;
}

// Local selection with the left button, when the app doesn't take the
// mouse or Shift keeps it here (local): by character, word or line as
// clicked once, twice or thrice, Shift extending (unless it is what
// keeps the mouse local), Alt a block. A drag past the grid's top or
// bottom scrolls history.
static void termselect(Gui *g, TermTab *tt, TermMetrics *m, ImVec2 pos, b32 local, f32 my)
{
    ImGuiIO    &io = ImGui::GetIO();
    ariel_Modes mo = ariel_modes(tt->t);
    f32         h  = (f32)mo.rows * m->ch;
    f32         w  = (f32)mo.cols * m->cw;
    f32         dx = io.MousePos.x - pos.x;  // -FLT_MAX off the window
    f32         dy = my - pos.y;
    dx = dx>0 ? (dx<w ? dx : w) : 0;  // clamped before any cast
    dy = dy>0 ? (dy<h ? dy : h-1) : 0;
    i32         bx = (i32)((dx + m->cw/2) / m->cw);  // a column boundary
    i32         y  = (i32)(dy / m->ch);
    i32 block = io.KeyAlt ? ARIEL_SEL_BLOCK : 0;

    if (local && ImGui::IsMousePosValid() && ImGui::IsMouseClicked(0)) {
        i32 clicks = ImGui::GetMouseClickedCount(0);
        i32 op     = clicks>=3 ? ARIEL_SEL_LINE : clicks==2 ? ARIEL_SEL_WORD : ARIEL_SEL_CHAR;
        if (clicks==1 && io.KeyShift && !mo.mouse && termselected(tt)) {
            op = ARIEL_SEL_EXTEND;
        }
        ariel_select(tt->t, op|block, bx, y);
        tt->selecting = 1;
        scheddirty(&g->sched);
    } else if (tt->selecting && io.MouseDown[0]) {
        if (dy < 0) {
            ariel_scroll(tt->t, -1);  // past the top: older
        } else if (dy >= h) {
            ariel_scroll(tt->t, 1);
        }
        if (dy<0 || dy>=h) {
            schedat(&g->sched, g->sched.now + 16000, DUE_APP);
        }
        if (dy<0 || dy>=h || io.MouseDelta.x!=0 || io.MouseDelta.y!=0) {
            ariel_select(tt->t, ARIEL_SEL_EXTEND|block, bx, y);
            scheddirty(&g->sched);
        }
    } else if (tt->selecting) {
        tt->selecting = 0;
        if (g->termcopysel) {
            termcopy(g, tt);
        }
    }
}

// when the app asks (mouse modes), unless Shift is held, which keeps it
// local for selection (I6). The wheel, also when reported, otherwise
// sends arrows on the alternate screen (mode 1007, on by default), and
// later scrolls history (L3). Ctrl+wheel stays the app's zoom.
static void termmouse(Gui *g, TermTab *tt, TermMetrics *m, ImVec2 pos, ImGuiID id, b32 hovered)
{
    ImGuiIO    &io  = ImGui::GetIO();
    ariel_Modes mo  = ariel_modes(tt->t);
    b32         rep = mo.mouse && !io.KeyShift;
    if (!rep && tt->mheld) {
        // Shift came down mid-drag, or the app stopped asking: the
        // releases still go, so it sees no button stuck
        rep = mo.mouse != 0;
    }
    // Clamped to the grid as floats first: off the window, the position
    // is -FLT_MAX, and a drag or the slack past the last cell goes beyond
    ImVec2 at = io.MousePos;
    f32    w  = (f32)mo.cols * m->cw;
    f32    h  = (f32)mo.rows * m->ch;
    f32    dx = at.x - pos.x;
    f32    dy = at.y - pos.y;
    dx = dx>0 ? (dx<w ? dx : w) : 0;
    dy = dy>0 ? (dy<h ? dy : h) : 0;
    ariel_Mouse e = {};
    e.mods = termmods(g) & ~ARIEL_SHIFT;
    e.x    = (i32)(dx / m->cw);
    e.y    = (i32)(dy / m->ch);
    e.x    = e.x<mo.cols ? e.x : mo.cols-1;
    e.y    = e.y<mo.rows ? e.y : mo.rows-1;
    e.px   = (i32)(dx * m->density);
    e.py   = (i32)(dy * m->density);
    i32 pw = (i32)roundf(w * m->density);
    i32 ph = (i32)roundf(h * m->density);
    e.px   = e.px<pw ? e.px : pw-1;
    e.py   = e.py<ph ? e.py : ph-1;

    if (rep && ImGui::IsMousePosValid()) {
        static i32 const buttons[] = {1, 3, 2};  // ImGui's left, right, middle
        for (i32 b = 0; b < 3; b++) {
            u32 bit = 1u << b;
            if (hovered && !io.KeyShift && ImGui::IsMouseClicked(b)) {
                e.action = ARIEL_PRESS;
                e.button = buttons[b];
                tt->mheld |= bit;
                ariel_Str s = ariel_mouse(tt->t, &e);
                termsend(g, tt, {s.data, s.len});
            } else if ((tt->mheld & bit) && !io.MouseDown[b]) {
                e.action = ARIEL_RELEASE;
                e.button = buttons[b];
                tt->mheld &= ~bit;
                ariel_Str s = ariel_mouse(tt->t, &e);
                termsend(g, tt, {s.data, s.len});
            }
        }
        if ((hovered || tt->mheld) && (io.MouseDelta.x!=0 || io.MouseDelta.y!=0)) {
            e.action = ARIEL_MOTION;
            e.button = 0;  // ariel knows which are held
            ariel_Str s = ariel_mouse(tt->t, &e);
            termsend(g, tt, {s.data, s.len});
        }
    } else if (!mo.mouse) {
        tt->mheld = 0;
    }
    termselect(g, tt, m, pos, hovered && !rep, at.y);

    if (!hovered || io.KeyCtrl) {
        tt->wheel = 0;
        return;
    }
    ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, id);
    tt->wheel += io.MouseWheel;
    i32 notches = (i32)tt->wheel;
    tt->wheel -= (f32)notches;
    for (; notches; notches -= notches>0 ? 1 : -1) {
        if (rep) {
            e.action = ARIEL_PRESS;
            e.button = notches>0 ? 4 : 5;
            ariel_Str s = ariel_mouse(tt->t, &e);
            termsend(g, tt, {s.data, s.len});
        } else if (mo.altscreen && mo.altscroll) {
            ariel_Key k = {};
            k.key = notches>0 ? ARIEL_K_UP : ARIEL_K_DOWN;
            for (i32 i = 0; i < 3; i++) {
                ariel_Str s = ariel_key(tt->t, &k);
                terminput(g, tt, {s.data, s.len});
            }
        } else {
            ariel_scroll(tt->t, notches>0 ? -3 : 3);  // history
            scheddirty(&g->sched);
        }
    }
}

// Whether the key event at i in the trail typed text: a character comes
// after it, before another key goes down. The character may wait in the
// queue for the next frame: ImGui trickles text that follows a modifier
// change in the same frame, as Ctrl+Alt+key typed at once does.
static b32 termtyped(ImGuiContext &c, i32 i)
{
    i32 trail = c.InputEventsTrail.Size;
    i32 total = trail + c.InputEventsQueue.Size;
    for (i++; i < total; i++) {
        ImGuiInputEvent *e = i<trail ? &c.InputEventsTrail[i]
                                     : &c.InputEventsQueue[i-trail];
        if (e->Type==ImGuiInputEventType_Text) {
            return e->Text.Char>=0x20 && e->Text.Char!=0x7f;
        } else if (e->Type==ImGuiInputEventType_Key && e->Key.Down &&
                   !(e->Key.Key & ImGuiMod_Mask_) && !ImGui::IsLRModKey(e->Key.Key)) {
            return 0;
        }
    }
    return 0;
}

// The keys of a frame, in order: text as typed, named keys and Ctrl
// chords through ariel's encoder. ImGui keeps no key repeats in its
// trail, so those come from its repeat timing.
static void termkeys(Gui *g, TermTab *tt, Arena a)
{
    ImGuiIO      &io   = ImGui::GetIO();
    ImGuiContext &c    = *GImGui;
    b32           cmd  = g->mac && io.KeyCtrl;  // Cmd: the app's
    b32           ctrl = termctrl(g);
    if (cmd) {
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            termpaste(g, tt, a);
        } else if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            termcopy(g, tt);
        }
        return;
    }
    // Copy: Ctrl+Shift+C, and on Windows Ctrl+C while there is a
    // selection, which it clears, else Ctrl+C is ^C. The C it took
    // stays the copy's until let go, sending no repeats.
    if (termkept(g, ImGuiKey_C) && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        termcopy(g, tt);
    }
    tt->copyc = tt->copyc && ImGui::IsKeyDown(ImGuiKey_C);
    // Paste: Ctrl+Shift+V, and Shift+Insert as in GNOME Terminal and
    // Konsole (the clipboard, not the primary selection). It doesn't
    // repeat, and the Insert it took stays the paste's until let go.
    b32 shiftins = !g->mac && io.KeyShift && !io.KeyCtrl && !io.KeyAlt &&
                   ImGui::IsKeyPressed(ImGuiKey_Insert, false);
    tt->pasteins = shiftins || (tt->pasteins && ImGui::IsKeyDown(ImGuiKey_Insert));
    if (shiftins || (termkept(g, ImGuiKey_V) && ImGui::IsKeyPressed(ImGuiKey_V, false))) {
        termpaste(g, tt, a);
    }

    // On Windows AltGr is Ctrl+Alt, as ImGui sees it, and so is Ctrl+Alt
    // itself where the layout gives it a character: a Ctrl+Alt key whose
    // text follows typed that text, its repeats too. So did an AltGr key
    // with a character in the layout and none following: a dead key
    // (~ is AltGr+1 on Croatian), whose text comes with the next key.
    b32 alttext = !g->mac && io.KeyCtrl && io.KeyAlt;
    tt->textkey = tt->textkey && ImGui::IsKeyDown((ImGuiKey)tt->textkey) ? tt->textkey : 0;
    b32 pressed[ImGuiKey_NamedKey_COUNT] = {};
    for (i32 i = 0; i < c.InputEventsTrail.Size; i++) {
        ImGuiInputEvent *e = &c.InputEventsTrail[i];
        if (e->Type==ImGuiInputEventType_Key && e->Key.Down &&
                ImGui::IsNamedKey(e->Key.Key) && !(e->Key.Key & ImGuiMod_Mask_)) {
            pressed[e->Key.Key - ImGuiKey_NamedKey_BEGIN] = 1;
            if (HOST_WINDOWS && e->Key.Key==ImGuiKey_C && io.KeyCtrl && !io.KeyShift &&
                    !io.KeyAlt && termselected(tt)) {
                termcopy(g, tt);
                ariel_select(tt->t, ARIEL_SEL_CLEAR, 0, 0);
                tt->copyc = 1;
                scheddirty(&g->sched);
            } else if (alttext && termtextkey(e->Key.Key) &&
                    (termtyped(c, i) || (os_guialtgr(g->os) &&
                                         os_guikeycp(g->os, e->Key.Key, io.KeyShift)))) {
                tt->textkey = e->Key.Key;
            } else if (!termkept(g, e->Key.Key) && !(tt->pasteins && e->Key.Key==ImGuiKey_Insert)) {
                termpress(g, tt, e->Key.Key, 0);
            }
        } else if (e->Type==ImGuiInputEventType_Text && (!ctrl || alttext)) {
            u32 cp = e->Text.Char;
            if (cp>=0x20 && cp!=0x7f) {
                u8        buf[4];
                iz        n = utf8encode(buf, (i32)cp);
                ariel_Str s = ariel_text(tt->t, {buf, n});
                termsend(g, tt, {s.data, s.len});
            }
        }
    }
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        ImGuiKey key = (ImGuiKey)k;
        if (!pressed[k - ImGuiKey_NamedKey_BEGIN] && (termnamed(key) || termtextkey(key)) &&
                !termkept(g, key) && !(tt->pasteins && key==ImGuiKey_Insert) &&
                key!=tt->textkey && !(tt->copyc && key==ImGuiKey_C) &&
                ImGui::GetKeyData(key)->DownDuration>0 &&
                ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, ImGuiKeyOwner_Any)) {
            termpress(g, tt, key, 1);
        }
    }
}

// Ctrl+`, at the top of the frame, before any shortcut: shows the pane
// and focuses the terminal, or from the terminal, focuses the prompt,
// hiding the pane unless it is docked beside the prompt. While
// the terminal has the keyboard on macOS, it lets go of it with Cmd held,
// so the app keeps every Cmd chord. Elsewhere it keeps the keyboard but
// for the app's few chords (termkept): on the frame one is pressed it
// lets go until termgrab, after menukeys, since with the keyboard held no
// Shortcut() fires, RouteOverActive or not (its owner isn't the item).
static void termhotkeys(Gui *g)
{
    ImGuiIO      &io = ImGui::GetIO();
    ImGuiContext &c  = *GImGui;
    if (c.ActiveId && c.ActiveId==g->termid) {
        b32 app = g->mac ? io.KeyCtrl : termkeptpressed(g);
        if (!app) {
            ImGui::SetActiveIdUsingAllKeyboardKeys();
        } else {
            c.ActiveIdUsingAllKeyboardKeys = false;
            c.ActiveIdUsingNavDirMask      = 0;
        }
    }
    if (!termctrl(g) || !ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
        return;
    }
    os_guiimereset(g->os);  // on German, ^ is a dead key
    if (io.KeyShift) {
        // A new tab, made by drawterm once the pane's size holds
        g->termaddto = g->sess ? termroot(g, g->sess) : 0;
        setpane(g, PANE_TERMINAL, 1);
        g->termfocus = 1;
    } else if (g->panes[PANE_TERMINAL] && g->termfocused) {
        // To the prompt. Docked beside it, the pane stays, since hiding
        // it would reflow the layout; covering it, as a tab of its node,
        // or floating, it goes
        ImGuiWindow *w     = ImGui::FindWindowByName(panenames[PANE_TERMINAL]);
        b32          docked = w && w->DockIsActive;
        b32          beside = g->panedrawn[PANE_PROMPT] >= g->total.frames-1;
        if (!docked || !beside) {
            setpane(g, PANE_TERMINAL, 0);
        }
        g->promptfocus = 1;
        if (c.ActiveId == g->termid) {
            ImGui::ClearActiveID();
        }
    } else {
        setpane(g, PANE_TERMINAL, 1);
        g->termfocus = 1;
    }
    scheddirty(&g->sched);
}


// Why session s can have no new tab now, else empty.
static Str termaddwhy(Gui *g, GuiSession *s)
{
    Str why = termwhy(g, s);
    return why.len ? why : termfull(g, termroot(g, s));
}

// A new tab for session s's root, shown in the pane: the shown root's
// waits for drawterm to size it, another's starts at the last size and
// follows the pane once shown.
static void termadd(Gui *g, GuiSession *s)
{
    GuiSession *r = termroot(g, s);
    setpane(g, PANE_TERMINAL, 1);
    g->termfocus = 1;
    if (g->sess && termroot(g, g->sess)==r) {
        g->termaddto = r;
    } else {
        i32 cols = g->termlastcols ? g->termlastcols : 80;
        i32 rows = g->termlastrows ? g->termlastrows : 24;
        termnew(g, r, cols, rows);
    }
    scheddirty(&g->sched);
}


// Takes back the keyboard termhotkeys let go of for an app chord, once
// menukeys has seen it.
static void termgrab(Gui *g)
{
    ImGuiContext &c = *GImGui;
    if (!g->mac && c.ActiveId && c.ActiveId==g->termid) {
        ImGui::SetActiveIdUsingAllKeyboardKeys();
    }
}


// The pane

// The pane's banner over an ended or failed terminal, with its buttons.
static void termbanner(Gui *g, GuiSession *s, TermTab *tt, Arena a, i32 cols, i32 rows)
{
    Buf b(&a, 256);
    if (tt->state == TT_FAILED) {
        print(&b, "The terminal could not start: ");
        print(&b, termerr(tt));
    } else if (tt->errlen) {
        print(&b, "The terminal ended: ");
        print(&b, termerr(tt));
    } else if (tt->signal) {
        print(&b, "The shell was killed by signal ");
        print(&b, (i64)tt->signal);
    } else {
        print(&b, "The shell exited with code ");
        print(&b, (i64)tt->code);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
    ImGui::TextWrapped("%s", cstr0(&a, finish(&b)));
    ImGui::PopStyleColor();
    b32 restart = ImGui::Button("Restart");
    itemtip("Start a new shell (Enter)");
    ImGui::SameLine();
    b32 close = ImGui::Button("Close");
    restart |= ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (restart) {
        termrestart(g, s, tt, cols, rows);
        g->termfocus = 1;
    } else if (close) {
        termdrop(g, s, termsetfind(&s->terms, tt), 0);
    }
}

// What the tab bar asks of its caller, which knows the size.
struct TermBar {
    i32 closed;   // the tab to close, else -1
    b32 kill;     //   sweeping its tree
    i32 restart;  // the tab to restart, else -1
    b32 plus;     // a new tab
};

// The tab bar over the grid, drawn while the root can have terminals,
// even with none, so the grid keeps its height as the first starts. It
// selects its tab when front changed here (frontset), else front follows
// the tab the user picked, and the set follows the bar's order. Each
// tab's context menu closes, restarts or kills it.
static TermBar termtabs(Gui *g, GuiSession *s)
{
    TermBar  r   = {-1, 0, -1, 0};
    TermSet *set = &s->terms;
    if (g->termbarroot != s) {
        g->termbarroot = s;  // another root: its own front, selected
        set->frontset  = set->n > 0;
    }
    ImGuiTabBarFlags bf = ImGuiTabBarFlags_Reorderable |
                          ImGuiTabBarFlags_FittingPolicyShrink;
    if (!ImGui::BeginTabBar("##termtabs", bf)) {
        return r;
    }
    Arena   a = g->frame;
    ImGuiID ids[TERMTABS] = {};
    for (i32 i = 0; i < set->n; i++) {
        TermTab          *tt    = set->tabs[i];
        bool              open  = true;
        ImGuiTabItemFlags flags = 0;
        if (set->frontset && i==set->front) {
            flags |= ImGuiTabItemFlags_SetSelected;
        }
        if (tt->busy) {
            flags |= ImGuiTabItemFlags_UnsavedDocument;
        }
        Buf b(&a, TERMTITLECAP + 32);
        print(&b, termlabel(tt));
        print(&b, "###tt");
        print(&b, (i64)tt->serial);
        b32 shown = ImGui::BeginTabItem(cstr0(&a, finish(&b)), &open, flags);
        ids[i] = ImGui::GetItemID();
        if (ImGui::IsItemDeactivated()) {
            // On release, not press: focus takes the active ID, which
            // would end a drag before it started
            g->termfocus = 1;
        }
        Buf nb(&a, 32);
        print(&nb, "term tab ");
        print(&nb, (i64)i + 1);
        itemnamed(g, finish(&nb));
        if (ImGui::BeginPopupContextItem()) {
            Str live = tt->state==TT_LIVE ? Str{} : Str("Its shell has ended");
            if (menutip(g, &a, "Close", {},
                        "Hang up its shell; jobs it disowned live on")) {
                r.closed = i;
            }
            if (menutip(g, &a, "Restart", termwhy(g, s),
                        "Hang up its shell and start another in this tab")) {
                r.restart = i;
            }
            if (menutip(g, &a, "Kill", live,
                        "Close it, ending every process its shell started")) {
                r.closed = i;
                r.kill   = 1;
            }
            ImGui::EndPopup();
        }
        if (shown) {
            ImGui::EndTabItem();
        }
        if (!open && r.closed<0) {
            r.closed = i;
        }
    }
    Str why = termfull(g, s);
    ImGui::BeginDisabled(why.len > 0);
    if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                  ImGuiTabItemFlags_NoTooltip)) {
        r.plus       = 1;
        g->termfocus = 1;
    }
    ImGui::EndDisabled();
    if (why.len) {
        itemtip("%s", cstr0(&a, why));
    } else {
        itemtip("New terminal tab (Ctrl+Shift+`)");
    }
    itemnamed(g, "term tab +");

    // The bar's order and selection back into the set
    ImGuiTabBar *bar = ImGui::GetCurrentTabBar();
    TermTab     *order[TERMTABS];
    i32          norder = 0;
    for (i32 k = 0; k < bar->Tabs.Size; k++) {
        for (i32 i = 0; i < set->n; i++) {
            if (ids[i] && bar->Tabs[k].ID==ids[i] && norder<TERMTABS) {
                order[norder++] = set->tabs[i];
            }
        }
    }
    TermTab *selected = 0;
    for (i32 i = 0; i < set->n; i++) {
        if (ids[i] && ids[i]==bar->SelectedTabId) {
            selected = set->tabs[i];
        }
    }
    if (set->frontset) {
        if (set->n && selected==set->tabs[set->front]) {
            set->frontset = 0;
        }
    } else if (selected && selected!=set->tabs[set->front]) {
        set->front = termsetfind(set, selected);
        scheddirty(&g->sched);
    }
    termsetorder(set, order, norder);
    ImGui::EndTabBar();
    return r;
}

// The Terminal pane's contents: the current session's tabs, the front
// one's grid filling the pane, which takes the keyboard once clicked.
static void drawterm(Gui *g)
{
    Arena        a   = g->frame;
    GuiSession  *s   = g->sess ? termroot(g, g->sess) : 0;
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    ImGuiID      id  = win->GetID("##grid");
    g->termid      = id;
    g->termfocused = 0;
    g->termgrid    = {};

    Str why = termwhy(g, s);
    if (why.len && (!s || !s->terms.n)) {
        ImGui::TextDisabled("%s", cstr0(&a, why));
        return;
    }
    TermBar bar = termtabs(g, s);
    termpasteask(g, a);

    TermMetrics m     = termmetrics(g);
    ImVec2      avail = ImGui::GetContentRegionAvail();
    i32         cols  = (i32)(avail.x / m.cw);
    i32         rows  = (i32)(avail.y / m.ch);
    cols = cols<2 ? 2 : cols;
    rows = rows<1 ? 1 : rows;
    if (g->termforcecols) {
        cols = g->termforcecols;
        rows = g->termforcerows;
    }
    if (bar.restart >= 0) {
        termrestart(g, s, s->terms.tabs[bar.restart], cols, rows);
        termsetshow(&s->terms, bar.restart);
        g->termfocus = 1;
    } else if (bar.closed >= 0) {
        termdrop(g, s, bar.closed, bar.kill);
        if (!s->terms.n && GImGui->ActiveId==id) {
            ImGui::ClearActiveID();
            g->promptfocus = 1;
        }
        scheddirty(&g->sched);
    }
    // The pane takes its place in the dock over frames, through a
    // layout (drawpanes), and a shell started smaller loses what
    // scrolled away, as ConPTY keeps no history: a shell started by key
    // waits until the size holds for a frame.
    b32 held = !win->Appearing && !g->layoutpending && !g->layoutnow &&
               cols==g->termlastcols && rows==g->termlastrows;
    g->termlastcols = cols;
    g->termlastrows = rows;
    if (bar.plus) {
        termnew(g, s, cols, rows);
    }
    if (g->termaddto != s) {
        g->termaddto = 0;  // the shown root changed
    } else if (held) {
        g->termaddto = 0;
        termnew(g, s, cols, rows);
    }

    TermTab *tt = termsetfront(&s->terms);
    if (!tt || tt->state==TT_NONE) {
        if (g->termfocus && !held) {
            return;
        } else if (g->termfocus || ImGui::Button("Start a shell")) {
            g->termfocus = 1;
            tt = termnew(g, s, cols, rows);
            if (!tt) {
                return;
            }
        } else {
            ImGui::TextDisabled("Ctrl+` starts one, and comes back to it");
            return;
        }
    }
    if (tt->state==TT_ENDED || tt->state==TT_FAILED) {
        termbanner(g, s, tt, a, cols, rows);
        if (!tt->t || tt->state==TT_FAILED) {
            return;
        }
        avail = ImGui::GetContentRegionAvail();
    }

    if (tt->pal != g->pal) {
        termcolors(g, tt);  // the theme changed
    }
    // Another tab in front: the last lets go, and a size that differs
    // applies at once rather than settling
    TermTab *was   = g->termshowing;
    b32      fresh = was!=tt || g->termshowingserial!=tt->serial;
    if (fresh) {
        if (was && was->serial==g->termshowingserial && termowner(g, was)) {
            termletgo(g, was);
        }
        g->termshowing       = tt;
        g->termshowingserial = tt->serial;
    }

    // The grid, an item that takes the keyboard once clicked
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos = v2(termsnap(pos.x, m.density), termsnap(pos.y, m.density));
    ImVec2 size = v2(avail.x>1 ? avail.x : 1, avail.y>1 ? avail.y : 1);
    ImRect bb(pos, v2(pos.x + size.x, pos.y + size.y));
    g->termgrid = bb;
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) {
        return;
    }
    // History's scrollbar, over the grid's right edge, taking no column:
    // the grid hovers apart from it
    ariel_View view  = ariel_view(tt->t);
    b32        sb    = view.total > tt->rows;
    f32        barw  = ImGui::GetStyle().ScrollbarSize * 0.6f;
    ImRect     barbb = ImRect(v2(bb.Max.x - barw, bb.Min.y), bb.Max);
    ImRect     hovbb = sb ? ImRect(bb.Min, v2(barbb.Min.x, bb.Max.y)) : bb;

    ImGuiContext &c       = *GImGui;
    b32           hovered = ImGui::ItemHoverable(hovbb, id, 0);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    if (g->termfocus || (hovered && ImGui::IsMouseClicked(0))) {
        g->termfocus = 0;
        ImGui::SetActiveID(id, win);
        ImGui::SetFocusID(id, win);
        ImGui::FocusWindow(win);
        ImGui::SetActiveIdUsingAllKeyboardKeys();
    } else if (c.ActiveId==id && ImGui::IsMouseClicked(0) && !hovered) {
        ImGui::ClearActiveID();
    }
    b32 active = c.ActiveId==id && tt->state==TT_LIVE;
    g->termfocused = c.ActiveId == id;

    // A new size, once it settles
    if (tt->state==TT_LIVE || tt->state==TT_OPENING) {
        b32 differ = cols!=tt->cols || rows!=tt->rows;
        if (!differ) {
            tt->wantcols = 0;
        } else if (cols!=tt->wantcols || rows!=tt->wantrows) {
            tt->wantcols = cols;
            tt->wantrows = rows;
            tt->wantat   = g->sched.now - (fresh ? TERMTAB_RESIZE : 0);
        }
        if (tt->wantcols) {
            i64 at = tt->wantat + TERMTAB_RESIZE;
            if (g->sched.now >= at) {
                b32 fits = cols<=tt->capcols && rows<=tt->caprows;
                if (fits || termblock(g, tt, cols, rows)) {
                    if (ariel_resize(tt->t, cols, rows, (i32)roundf(m.cw*m.density),
                                     (i32)roundf(m.ch*m.density))) {
                        tt->cols = cols;
                        tt->rows = rows;
                        if (tt->tid) {
                            termsize(g, tt, cols, rows, m.cw*m.density, m.ch*m.density);
                        }
                    }
                }
                tt->wantcols = 0;
            } else {
                schedat(&g->sched, at, DUE_APP);
            }
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    u32         bg = termimcolor(ariel_color(tt->t, ARIEL_C_BG));
    dl->AddRectFilled(bb.Min, bb.Max, bg);
    termcells(g, tt, &m, pos, active, a);
    if (sb) {
        ImS64 hist = view.total - tt->rows;
        ImS64 at   = hist - view.offset;  // rows of history above the view
        ImS64 was  = at;
        ImGui::ScrollbarEx(barbb, win->GetID("##termbar"), ImGuiAxis_Y, &at,
                           tt->rows, view.total, 0);
        if (at != was) {
            ariel_scroll(tt->t, (i32)(at - was));  // positive: newer
            scheddirty(&g->sched);
        }
    }
    g->termverts = dl->VtxBuffer.Size;
    if (tt->state == TT_LIVE) {
        termmouse(g, tt, &m, pos, id, hovered);
    }

    if (active) {
        termkeys(g, tt, a);
        c.ActiveIdAllowOverlap = !ImGui::GetIO().MouseDown[0];
        ariel_Cursor cur = ariel_cursor(tt->t);
        ImGuiPlatformImeData *ime = &c.PlatformImeData;
        ime->WantVisible     = true;
        ime->WantTextInput   = true;
        f32 cy = (f32)(cur.y<0 ? tt->rows-1 : cur.y);  // scrolled off
        ime->InputPos        = v2(pos.x + (f32)cur.x*m.cw, pos.y + cy*m.ch);
        ime->InputLineHeight = m.ch;
        ime->ViewportId      = win->Viewport->ID;
    }
}


// For the checklist (--drive)

// The shown session's terminal's screen as text, a line a row, else
// empty.
static Str termscreen(Gui *g, Arena *a)
{
    TermTab *tt = termfront(g);
    if (!tt || !tt->t) {
        return {};
    }
    iz  len = ariel_dump(tt->t, 0, 0, 0);
    u8 *buf = allocbytes(a, len);
    ariel_dump(tt->t, 0, buf, len);
    // Without blanks at the ends of lines, which ConPTY writes or not
    // as it repaints
    iz n = 0;
    for (iz i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            for (; n>0 && buf[n-1]==' '; n--) {}
        }
        buf[n++] = buf[i];
    }
    for (; n>0 && buf[n-1]==' '; n--) {}
    return {buf, n};
}

// Where the screen last shows text, its first cell's middle, else
// (-1, -1): ASCII text, so that bytes are cells.
static ImVec2 termtextat(Gui *g, Str text)
{
    Arena a   = g->frame;
    Str   scr = termscreen(g, &a);
    iz    at  = -1;
    for (iz i = find(scr, text); i >= 0;) {
        at = i;
        iz next = find(cuthead(scr, i+1), text);
        i = next<0 ? -1 : i+1+next;
    }
    if (at<0 || g->termgrid.GetWidth()<=0) {
        return v2(-1, -1);
    }
    i32 row = 0, col = 0;
    for (iz i = 0; i < at; i++) {
        col = scr.data[i]=='\n' ? 0 : col+1;
        row += scr.data[i] == '\n';
    }
    TermMetrics m = termmetrics(g);
    return v2(g->termgrid.Min.x + ((f32)col + 0.5f)*m.cw,
              g->termgrid.Min.y + ((f32)row + 0.5f)*m.ch);
}

// Whether the terminal fills its pane, as last drawn, at the size the
// engine was told, with no new size waiting.
static b32 termsizecheck(Gui *g)
{
    TermTab *tt = termfront(g);
    if (!tt || !tt->t || tt->state!=TT_LIVE || g->termgrid.GetWidth()<=0) {
        guilog(g, "drive: no live terminal shown");
        return 0;
    }
    TermMetrics m    = termmetrics(g);
    ariel_Modes mo   = ariel_modes(tt->t);
    f32         cw   = g->termgrid.GetWidth();
    f32         ch   = g->termgrid.GetHeight();
    i32         cols = (i32)(cw / m.cw);
    i32         rows = (i32)(ch / m.ch);
    Arena       a    = g->frame;
    Buf         b(&a, 128);
    print(&b, "drive: the terminal is ");
    print(&b, (i64)mo.cols);
    print(&b, "x");
    print(&b, (i64)mo.rows);
    print(&b, ", its pane fits ");
    print(&b, (i64)cols);
    print(&b, "x");
    print(&b, (i64)rows);
    guilog(g, finish(&b));
    return !tt->wantcols && mo.cols==tt->cols && mo.rows==tt->rows &&
           cols-mo.cols<=1 && cols>=mo.cols && rows-mo.rows<=1 && rows>=mo.rows;
}
