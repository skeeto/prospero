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
// A session on another host (a remote, a background host, a container)
// has its shells in that host's engine, which relays the commands and
// records of each client's terminals (host.cpp, cap term.host); a
// subagent on a host apart from its parent's has its own tabs there.
// While that host's link is down, its tabs keep their screens under
// "Reconnecting", typing goes nowhere, and the shells run on, their
// output waiting in the link; a host that comes back new (anew), or
// disconnects, has hung them up, and they end.
//
// A tab may run an editor on a file in place of a shell (termedit, from
// Files' Open in editor), which its host picks and starts (term.open's
// edit, cap term.edit): named 'NAME FILE' once the editor is known (OSC
// 1, or the reply), one per file per root. Its exit with status 0
// closes it and goes back to the file in Files, read again (editdone);
// any other end leaves its banner, whose Restart runs the editor again.
// While open it counts as busy, so quitting asks first.
//
// The transcript's Run button types a code block of the agent's at a
// shell's prompt (termrun, cap term.marks): of a tab of its shell's
// family that waits there with nothing running, the front one, else the
// right-most; else a new tab, asked for a shell of that family
// (term.open's shell), into which it goes at its first prompt. A tab
// knows its prompt by the OSC 133 marks of its hooked shell (ptyhost.cpp),
// else by what was typed last and term.busy (termset.cpp's rules). The
// engine tells the agent how it ended, if its shell is hooked. While it
// runs, a click shows its tab and types nothing.
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
    TERMTAB_TEXT  = 160,       // bytes kept of its shell's name and errors
    TERMTAB_RESIZE = 100*1000,  // µs a new size waits to settle
    TERMTAB_SYNC   = 150*1000,  // µs a synchronized update holds the screen
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
    // A synchronized update (mode 2026) under way: the screen as it was
    // when it began, drawn in place of the live one until it ends, the
    // view moves, or TERMTAB_SYNC passes, so that a program's redraw
    // (brew's progress bars) shows whole, never half done
    Arena       spare;     // the rest of its region, for held
    ariel_Cell *held;      // capcols*caprows cells, made at the first
    b32         holding;
    i64         heldat;    // when it began, µs
    i32         heldcols;
    i32         heldrows;
    i32         heldoff;   // the view's offset
    ariel_Cursor heldcur;
    u32         mheld;     // mouse buttons whose press went to the app
    f32         wheel;     // wheel notches not yet sent, fractions kept
    b32         pasteins;  // Insert, held since it pasted with Shift
    i32         textkey;   // a Ctrl+Alt key that typed (AltGr), while held
    b32         busy;      // a program runs in the shell
    b32         selecting; // the left button drags a local selection
    b32         copyc;     // C, held since Ctrl+C copied (Windows)
    b32         menuopen;  // its context menu drew (termmenu)
    i64         menurow;   // the line it opened on (ariel_lines), else -1
    b32         refocus;   // and closed: the grid takes the keyboard back
    i32         histlines; // its history, as [terminal] scrollback said
    Palette    *pal;       // the colors given ariel
    i32         code;      // the end's exit code, or -1
    i32         signal;
    u8          shell[TERMTAB_TEXT];  // its shell's name
    iz          shelllen;
    u8          err[TERMTAB_TEXT];    // why it failed or ended
    iz          errlen;
    u8          title[TERMTITLECAP];  // its program's, as shown (termtitle),
    iz          titlelen;             //   or an editor tab's editor's name
    // An editor on a file, in place of a shell (termedit): the request,
    // kept through a restart (termfree), and its path's buffer through
    // the tab's reuse (termalloc)
    b32         edit;
    Str         edpath;    // the file, absolute, on the tab's host
    iz          edpathcap;
    i32         edline;    // 1-based, 0 for none
    i32         edcol;     // 0 for none
    b32         edpane;    // the Terminal pane was hidden as it opened
    b32         ednamed;   // title names the editor (OSC 1, or the reply)
    b32         edplan;    // a session's plan, from Edit plan (plantabrun)
    // What Run in Terminal knows of its shell (termstate), times in µs
    // (os_ticks): from term.open's reply, its marks, and what was typed
    i32         kind;      // TERMK_*
    u8          nonce[32]; // its marks', if its shell is hooked
    b32         hooked;
    b32         marked;    // a mark with its nonce came
    i64         liveat;
    i64         outat;     // its last output
    b32         prompted;  // an A since the last typing, and no C
    b32         typed;     // anything typed since it went live
    b32         typedcr;   //   ending in Enter
    i64         typedat;
    u64         runkey;    // the last Run typed into it (termrunkey)
    i64         sentat;
    i64         doneat;    // its prompt came back after it, else 0
    // A Run waiting for this new tab's first prompt (termpend), its
    // text's buffer kept through the tab's reuse; and the language its
    // shell was asked for (term.open's shell), kept through a restart
    Str         pend;
    iz          pendcap;
    u64         pendkey;
    i64         pendat;
    u8          pendfrom[64];  // the session its end is told to
    iz          pendfromlen;
    u8          asklang[16];
    iz          asklanglen;
    // Its command lines (termcmd.cpp), and its shell's folder by OSC 7,
    // in a block kept through its reuse; the folder through a restart
    TermCmds   *cmds;
};

// Files' side of an editor tab (files.cpp): its file shown again, read
// again, as the editor exits; or marked to be checked, as it ends.
static void editdone(Gui *, GuiSession *, GuiHost *, Str, b32, b32);
static void termeditstale(Gui *, GuiSession *, TermTab *);

// Cmd/Ctrl+click on a link or a path in a tab
static b32 termclick(Gui *, Arena, TermTab *, i32, i32, b32);

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

// An editor tab's editor, as named (OSC 1, or the reply), else null.
static Str termeditname(TermTab *tt)
{
    return tt->edit && tt->ednamed ? Str{tt->title, tt->titlelen} : Str{};
}

// A tab's label, to b: its program's title, else its shell's name.
// ConPTY titles a console with its program's path, which names it as a
// shell's would be (C:\...\sh.EXE is sh). An editor's tab is 'NAME
// FILE', FILE its file's name, alone until the editor is named.
static void termlabel(Buf *b, TermTab *tt)
{
    if (tt->edit) {
        Str name = termeditname(tt);
        print(b, name);
        print(b, name.len ? Str(" ") : Str(""));
        print(b, basename(tt->edpath));
        return;
    }
    if (tt->titlelen) {
        Str title = {tt->title, tt->titlelen};
        Str name  = basename(title);
        Str ext   = cuthead(name, name.len<4 ? name.len : name.len-4);
        b32 exe   = ext.len==4 && ext[0]=='.' && (ext[1]|0x20)=='e' &&
                    (ext[2]|0x20)=='x' && (ext[3]|0x20)=='e';
        if (exe && name.len>4 && name.len<title.len) {
            print(b, takehead(name, name.len-4));
            return;
        }
        print(b, title);
        return;
    }
    Str shell = basename(termshellname(tt));
    print(b, shell.len ? shell : Str("Terminal"));
}

// Whether a tab's host takes its commands now: this machine's engine
// always, another host while its link is up. One whose link is down
// keeps its terminals for when it comes back (termhostlost if not).
static b32 termup(TermTab *tt)
{
    GuiHost *h = tt->host;
    return h->kind==GH_ENGINE || (h->r && !h->closing && h->up);
}

// Why a session can have no terminal now, else empty. Another host's
// engine must relay terminals to its clients (term.host), which one
// with only term (0.16) does not.
static Str termwhy(Gui *g, GuiSession *s)
{
    if (!s) {
        return "No session is open";
    } else if ((s->parent.len || (s->view && s->view->parent.len)) && !sessup(g, s)) {
        return "Its parent session isn't listed yet";
    } else if (g->enginekind==ENGINE_FAKE && s->host->kind==GH_ENGINE) {
        return "The fake agent has no terminals";
    }
    GuiHost *h = s->host;
    if (h->kind == GH_ENGINE) {
        return hostwhy(g, s, "term");
    } else if (!h->r || h->closing) {
        return "Its host is not connected";
    } else if (!h->up) {
        return "Waiting for its host to connect";
    }
    return hostwhy(g, s, "term.host");
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
    iz      heldsize = (iz)capcols * caprows * (iz)sizeof(ariel_Cell);
    Region *r        = os_reserve(g->os, size + heldsize + ((iz)1<<16));
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
    tt->spare   = a;
    tt->held    = 0;
    tt->holding = 0;
    return 1;
}

// Let go of a terminal's block and state, keeping its tab, an editor's
// request, so that a restart runs it again (an editor named anew by
// it), and the language its shell was asked for. A Run waiting for it
// is dropped.
static void termfree(Gui *g, TermTab *tt)
{
    if (tt->region) {
        os_release(g->os, tt->region);
        g->termregions--;
    }
    TermTab was = *tt;
    *tt = {};
    tt->host      = was.host;
    tt->serial    = was.serial;
    tt->edit      = was.edit;
    tt->edpath    = was.edpath;
    tt->edpathcap = was.edpathcap;
    tt->edline    = was.edline;
    tt->edcol     = was.edcol;
    tt->edpane    = was.edpane;
    tt->pend       = {was.pend.data, 0};
    tt->pendcap    = was.pendcap;
    tt->asklanglen = was.asklanglen;
    copybytes(tt->asklang, was.asklang, was.asklanglen);
    tt->cmds       = was.cmds;
}

// Begin a terminal's command, which has no reply. Finish it with cmdsend.
static EvOut termbegin(Gui *g, Buf *b, i32 cmd, TermTab *tt)
{
    EvOut w = cmdbegin(b, cmd, 0);
    putintkey(&w.json, "tid", tt->tid);
    g->cmdhost  = tt->host;
    g->cmdquiet = 1;
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
    if (tt->state!=TT_LIVE || !tt->tid || !termup(tt)) {
        return;  // typed while reconnecting: lost, not sent late
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
// top of its parents on its host, and a side chat's original's, in the
// same folder.
// A worktree session, in a folder of its own, has its own.
static Str termcwd(GuiSession *s)
{
    return s->view && s->view->cwd.len ? s->view->cwd : s->cwd;
}

// The top of a session's parents on its own host: a subagent elsewhere,
// on a remote or in a container of its own, has its own tabs there, as
// Open in terminal opens its shell there. So does one in a worktree of
// its own, in its worktree, where its commits go.
static GuiSession *termtop(Gui *g, GuiSession *s)
{
    for (i32 n = 0; n < 16 && !s->wtname.len; n++) {
        GuiSession *up = sessup(g, s);
        if (!up || up==s || up->host!=s->host) {
            break;
        }
        s = up;
    }
    return s;
}

static GuiSession *termroot(Gui *g, GuiSession *s)
{
    s = termtop(g, s);
    for (i32 n = 0; n < 8; n++) {
        Str side = sessside(s);
        if (!side.len || s->wtname.len) {
            break;
        }
        GuiSession *o = findsess(g, s->host, side);
        if (!o || o==s || o->gone || !(termcwd(o) == termcwd(s))) {
            break;
        }
        s = termtop(g, o);
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

// Whether a tab waits (TT_NONE) to start at the pane's size: an
// editor's, or a new shell's for a Run.
static b32 termasks(TermTab *tt)
{
    return tt->edit || tt->pend.len;
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
            open += tt->host==s->host && (tt->state==TT_OPENING || tt->state==TT_LIVE ||
                                          (termasks(tt) && tt->state==TT_NONE));
        }
    }
    return open>=TERMTAB_OPEN ? Str("16 terminals are open already") : Str{};
}

// Close tab i of a root, which takes it out of the set, to the spares.
// An editor's file may have changed: checked once shown.
static void termdrop(Gui *g, GuiSession *s, i32 i, b32 kill)
{
    TermTab *tt = s->terms.tabs[i];
    if (tt->edit) {
        termeditstale(g, s, tt);
    }
    termclose(g, tt, kill);
    termsetdrop(&s->terms, i);
    tt->next     = g->termspare;
    g->termspare = tt;
}

// Whether a tab is an editor's, open or about to be: one the engine
// cannot see busy, as the editor leads the terminal's session.
static b32 termediting(TermTab *tt)
{
    return tt->edit && tt->state<=TT_LIVE;
}

// Tabs whose shell runs a program, or with an editor, which quitting
// would hang up.
static i32 termbusy(Gui *g)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        for (i32 i = 0; i < s->terms.n; i++) {
            TermTab *tt = s->terms.tabs[i];
            n += (tt->state==TT_LIVE && tt->busy) || termediting(tt);
        }
    }
    return n;
}

// Of those, the editors' (termediting).
static i32 termedits(Gui *g)
{
    i32 n = 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        for (i32 i = 0; i < s->terms.n; i++) {
            n += termediting(s->terms.tabs[i]);
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

// A running terminal ends for want of its engine, its screen kept: the
// engine there hung it up already, or will.
static void termlost(Gui *g, TermTab *tt, Str why)
{
    if (tt->state==TT_LIVE || tt->state==TT_OPENING) {
        tt->state  = TT_ENDED;
        tt->tid    = 0;
        tt->openid = 0;
        tt->busy   = 0;
        tt->errlen = termkeep(tt->err, why);
        scheddirty(&g->sched);
    }
}

// A host came up that is not the one the tabs were on: it started over,
// or forgot this window after its grace, hanging their shells up.
static void termhostlost(Gui *g, GuiHost *h)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        for (i32 i = 0; i < s->terms.n; i++) {
            if (s->terms.tabs[i]->host == h) {
                termlost(g, s->terms.tabs[i], "the host there started over");
            }
        }
    }
}

// As the window goes to the background, hang up every terminal it has
// in this process's engine, those whose term.open is still in flight
// too: their replies would go to the host, which never asked for them.
static void termcloseengine(Gui *g)
{
    if (g->enginekind == ENGINE_FAKE) {
        return;
    }
    Arena a = newarena(g->cmdr);
    Buf   b(&a, 64);
    EvOut w = cmdbegin(&b, CMD_TERMCLOSE, 0);
    putkey(&w.json, "all");
    putbool(&w.json, 1);
    g->cmdhost  = g->hosts;  // the engine's
    g->cmdquiet = 1;
    cmdsend(g, &w, a);
}

// What Run in Terminal's rules (termset.cpp) know of a tab now.
static TermState termstate(TermTab *tt)
{
    TermState st = {};
    st.live     = tt->state==TT_LIVE && tt->tid && termup(tt);
    st.kind     = tt->kind;
    st.hooked   = tt->hooked;
    st.marked   = tt->marked;
    st.liveat   = tt->liveat;
    st.busy     = tt->busy;
    st.typed    = tt->typed;
    st.typedcr  = tt->typedcr;
    st.typedat  = tt->typedat;
    st.prompted = tt->prompted;
    st.outat    = tt->outat;
    st.queued   = tt->pend.len > 0;
    st.runkey   = tt->runkey;
    st.sentat   = tt->sentat;
    st.doneat   = tt->doneat;
    st.dir      = tt->cmds ? Str{tt->cmds->cwd, tt->cmds->cwdlen} : Str{};
    return st;
}

// Bytes typed into a tab, by the user or a Run (not replies, nor mouse
// or focus reports): its prompt no longer waits empty, and a Run that
// waited for it gives way.
static void termkeyed(Gui *g, TermTab *tt, Str sent)
{
    if (!sent.len) {
        return;
    }
    u8 last = sent[sent.len-1];
    tt->typed    = 1;
    tt->typedcr  = last=='\r' || last=='\n';
    tt->typedat  = os_ticks(g->os);
    tt->prompted = 0;
    if (tt->pend.len) {
        guilog(g, "terminal: typing came first: the Run waiting for its new tab is dropped");
        tt->pend.len = 0;
    }
}

// A Run's text typed at tab tt's prompt, as one paste and an Enter, in
// one term.input whose run tells the engine whose it is: the top of
// session from, told how it ended (ptyhost.cpp's termrunarm).
static void termrunsend(Gui *g, TermTab *tt, Str text, u64 key, Str from)
{
    if (tt->state!=TT_LIVE || !tt->tid || !termup(tt)) {
        return;
    }
    Arena a   = newarena(g->cmdr);
    iz    cap = text.len + 32;
    u8   *out = allocbytes(&a, cap);
    iz    n   = ariel_paste(tt->t, out, cap-1, {text.data, text.len},
                            ARIEL_PASTE_BEGIN|ARIEL_PASTE_END);
    out[n++] = '\r';
    Str   data  = {out, n};
    i32   lines = 0;
    Str   first = termrunfirst(text, &lines);
    if (data.len > TERM_RUNINPUT) {
        return;  // termrunblock keeps a Run's text under this
    }
    if (ariel_view(tt->t).offset) {
        ariel_scrollto(tt->t, ARIEL_TO_BOTTOM);
    }
    Buf   b(&a, 256 + data.len/3*4 + 4 + first.len*6 + from.len);
    EvOut w = termbegin(g, &b, CMD_TERMINPUT, tt);
    putstrkey(&w.json, "data", base64(&a, data));
    putkey(&w.json, "run");
    putobject(&w.json);
    putstrkey(&w.json, "from", from);
    Buf kb(&a, 16);
    printhex(&kb, key, 16);
    putstrkey(&w.json, "key", finish(&kb));
    putstrkey(&w.json, "first", first);
    putintkey(&w.json, "lines", lines);
    putend(&w.json);
    cmdsend(g, &w, a);
    termkeyed(g, tt, data);
    tt->runkey = key;
    tt->sentat = tt->typedat;
    tt->doneat = 0;
    scheddirty(&g->sched);
}

// A new tab's Run, typed once its shell is ready for it (termpendgo),
// else dropped if it waited too long, or the tab ended.
static void termpend(Gui *g, TermTab *tt)
{
    if (!tt->pend.len) {
        return;
    }
    i64       now   = os_ticks(g->os);
    TermState st    = termstate(tt);
    b32       ended = tt->state==TT_ENDED || tt->state==TT_FAILED;
    switch (termpendgo(&st, ended, tt->pendat, now)) {
    case TERMPEND_WAIT: {
        i64 due = termlookdue(&st, now);
        i64 max = tt->pendat + TERM_PENDMAX;
        schedat(&g->sched, due && due<max ? due : max, DUE_CLOCK);
    } break;
    case TERMPEND_DROP:
        if (!ended) {
            guilog(g, "terminal: its new shell never came to a prompt: the Run is dropped");
        }
        tt->pend.len = 0;
        scheddirty(&g->sched);
        break;
    case TERMPEND_GO: {
        Str text = tt->pend;
        tt->pend.len = 0;
        termrunsend(g, tt, text, tt->pendkey, {tt->pendfrom, tt->pendfromlen});
    } break;
    }
}

static void termlaunch(Gui *, GuiSession *, TermTab *, i32, i32);
static void plantabsweep(Gui *);

// An editor's or a Run's tab that waits to start (TT_NONE), started at
// a size, or failed if its root can have no terminal now.
static void termeditgo(Gui *g, GuiSession *s, TermTab *tt, i32 cols, i32 rows)
{
    Str why = termwhy(g, s);
    if (why.len) {
        tt->host   = s->host;
        tt->state  = TT_FAILED;
        tt->errlen = termkeep(tt->err, why);
        scheddirty(&g->sched);
    } else {
        termlaunch(g, s, tt, cols, rows);
    }
}

// Each frame: close the terminals of sessions deleted or put in the
// Trash, here or by another window (their engine hung them up already),
// and end those of an engine that stopped, which says nothing more, or
// of a host disconnected, which hangs them up as it forgets this window.
// A host whose link is down only waits (termup). Editors' tabs waiting
// for drawterm's size start at the last size once their root is not
// the one it draws.
static void termsweep(Gui *g)
{
    GuiSession *shown = g->sess && g->panes[PANE_TERMINAL] ? termroot(g, g->sess) : 0;
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->gone || s->trashed) {
            while (s->terms.n) {
                termdrop(g, s, s->terms.n-1, 0);
            }
            continue;
        }
        for (i32 i = 0; i < s->terms.n; i++) {
            TermTab *tt = s->terms.tabs[i];
            GuiHost *h  = tt->host;
            if (h->kind==GH_ENGINE && g->enginegone) {
                termlost(g, tt, "the engine stopped");
            } else if (h->kind!=GH_ENGINE && (!h->r || h->closing)) {
                termlost(g, tt, h->msg.len ? h->msg : Str("the host was disconnected"));
            } else if (termasks(tt) && tt->state==TT_NONE && s!=shown) {
                i32 cols = g->termlastcols ? g->termlastcols : 80;
                i32 rows = g->termlastrows ? g->termlastrows : 24;
                termeditgo(g, s, tt, cols, rows);
            }
            termpend(g, tt);
        }
    }
    plantabsweep(g);
}

// Start a shell in a tab of root s, at a size, or an editor tab's
// editor (term.open's edit), which an engine without term.edit would
// take for a shell. A shell asked for by a Run's language (asklang)
// is of that language's family (term.open's shell).
static void termlaunch(Gui *g, GuiSession *s, TermTab *tt, i32 cols, i32 rows)
{
    tt->host = s->host;
    Str why  = tt->edit       ? hostwhy(g, s, "term.edit")  :
               tt->asklanglen ? hostwhy(g, s, "term.marks") : Str{};
    if (why.len) {
        tt->state  = TT_FAILED;
        tt->errlen = termkeep(tt->err, why);
        scheddirty(&g->sched);
        return;
    }
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
    putintkey(&w.json, "scrollback", tt->histlines);  // the engine's screen's
    if (!tt->edit && !tt->asklanglen && tt->cmds->cwdlen) {
        // A restart, in the folder its shell was last in
        putstrkey(&w.json, "cwd", Str{tt->cmds->cwd, tt->cmds->cwdlen});
    }
    termcmdreset(tt->cmds);
    if (tt->edit) {
        // The editor as the settings are now, a Restart's too
        EditAsk ask = editask(g, &a, s->host);
        putkey(&w.json, "edit");
        putobject(&w.json);
        putstrkey(&w.json, "path", tt->edpath);
        if (tt->edline > 0) {
            putintkey(&w.json, "line", tt->edline);
        }
        if (tt->edcol > 0) {
            putintkey(&w.json, "col", tt->edcol);
        }
        if (ask.tmpl.len) {
            putstrkey(&w.json, "template", ask.tmpl);
        } else if (ask.editor.len) {
            putstrkey(&w.json, "editor", ask.editor);
        }
        putend(&w.json);
    }
    if (!tt->edit && tt->asklanglen) {
        putstrkey(&w.json, "shell", Str{tt->asklang, tt->asklanglen});
    }
    tt->openid = g->nextid;
    cmdsend(g, &w, a);
    scheddirty(&g->sched);
}

// A tab, a spare if there is one, with a serial of its own: blank, but
// for an editor's path buffer and a Run's text buffer, used again.
static TermTab *termalloc(Gui *g)
{
    TermTab *tt = g->termspare;
    if (tt) {
        g->termspare = tt->next;
        Str buf  = tt->edpath;
        iz  cap  = tt->edpathcap;
        Str pbuf = tt->pend;
        iz  pcap = tt->pendcap;
        TermCmds *cmds = tt->cmds;
        *tt = {};
        tt->edpath    = {buf.data, 0};
        tt->edpathcap = cap;
        tt->pend      = {pbuf.data, 0};
        tt->pendcap   = pcap;
        tt->cmds      = cmds;
        *cmds = {};
    } else {
        tt = alloc<TermTab>(&g->perm);
        tt->cmds = alloc<TermCmds>(&g->perm);
    }
    tt->serial = ++g->termserial;
    return tt;
}

// A new tab for root s, shown, its shell starting; null if it can have
// none (termwhy, termfull).
static TermTab *termnew(Gui *g, GuiSession *s, i32 cols, i32 rows)
{
    if (termwhy(g, s).len || termfull(g, s).len) {
        return 0;
    }
    TermTab *tt = termalloc(g);
    termsetadd(&s->terms, tt);
    termlaunch(g, s, tt, cols, rows);
    return tt;
}

// A new shell in the same tab, the old one hung up; or its editor
// again. One that cannot start now waits as a tab not started, an
// editor's to fail with why (termeditgo).
static void termrestart(Gui *g, GuiSession *s, TermTab *tt, i32 cols, i32 rows)
{
    termclose(g, tt, 0);
    if (!termwhy(g, s).len) {
        termlaunch(g, s, tt, cols, rows);
    }
}

// Root s's editor tab on path, else null: the same file's, so without
// case on Windows (filesame), as Files has it.
static TermTab *termeditof(GuiSession *s, Str path)
{
    b32 win = s->host ? hostwin(s->host) : (b32)HOST_WINDOWS;
    for (i32 i = 0; i < s->terms.n; i++) {
        TermTab *tt = s->terms.tabs[i];
        if (tt->edit && filesame(tt->edpath, path, win)) {
            return tt;
        }
    }
    return 0;
}

// Open in editor's Terminal tab, in root s, for path, a file of its
// host, at line (1-based) and col (0 for none): the tab already open
// on that file, shown, or run again if it ended; else a new tab, which
// waits for drawterm to size it (as termadd's), or starts at the last
// size for a root not shown. The pane shows it, with the keyboard; if
// the pane was hidden, it hides again as the editor's last tab closes
// (editdone): one fact for all the root's editor tabs, whichever exits
// last. The caller checked it can (fileedit).
static TermTab *termedit(Gui *g, GuiSession *s, Str path, i32 line, i32 col)
{
    b32      hidden = !g->panes[PANE_TERMINAL];  // before setpane
    TermTab *tt     = termeditof(s, path);
    for (i32 i = 0; i < s->terms.n; i++) {
        TermTab *o = s->terms.tabs[i];
        hidden |= o->edit && o->edpane;
    }
    if (tt) {
        termsetshow(&s->terms, termsetfind(&s->terms, tt));
        if (tt->state==TT_ENDED || tt->state==TT_FAILED) {
            tt->edline = line;
            tt->edcol  = col;
            termclose(g, tt, 0);  // waits again, as TT_NONE
        }
    } else {
        tt = termalloc(g);
        if (tt->edpathcap >= path.len) {
            copybytes(tt->edpath.data, path.data, path.len);
            tt->edpath = {tt->edpath.data, path.len};
        } else {
            tt->edpath    = clone(&g->perm, path);
            tt->edpathcap = path.len;
        }
        tt->edit   = 1;
        tt->edline = line;
        tt->edcol  = col;
        tt->host   = s->host;
        termsetadd(&s->terms, tt);
    }
    for (i32 i = 0; i < s->terms.n; i++) {
        TermTab *o = s->terms.tabs[i];
        o->edpane |= o->edit && hidden;
    }
    setpane(g, PANE_TERMINAL, 1);
    g->termfocus = 1;
    b32 shown = g->sess && termroot(g, g->sess)==s;
    if (!shown && tt->state==TT_NONE) {
        i32 cols = g->termlastcols ? g->termlastcols : 80;
        i32 rows = g->termlastrows ? g->termlastrows : 24;
        termeditgo(g, s, tt, cols, rows);
    }
    scheddirty(&g->sched);
    return tt;
}

// The Terminal tab with session s's plan file at path open (Edit plan,
// or Open in editor on it), its editor running or about to, else null.
static TermTab *plantabof(Gui *g, GuiSession *s, Str path)
{
    GuiSession *root = path.len ? termroot(g, s) : 0;
    TermTab    *tt   = root && root->terms.n ? termeditof(root, path) : 0;
    return tt && termediting(tt) ? tt : 0;
}

// Each frame: a plan out in a Terminal tab of Edit plan's (plantabrun),
// its editor started again by Restart too, read again once the editor
// is done, however it went: exited, its tab closed or killed, or its
// host lost. A remote's is read through its engine there
// (planreadstart), as a plan an editor here reached by ssh.
static void plantabsweep(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        View    *v  = s->view;
        TermTab *tt = !s->gone && v && v->planpath.len ? plantabof(g, s, v->planpath) : 0;
        if (tt && tt->edplan) {
            s->plantab = 1;
            continue;
        } else if (!s->plantab) {
            continue;
        }
        s->plantab = 0;
        if (s->gone || !v) {
            continue;
        }
        guilog(g, "a plan's Terminal tab is done");
        if (s->host->kind == GH_REMOTE) {
            planreadstart(g, s, {});
        } else {
            planedited(g, g->frame, s, v->planpath, {});
        }
        scheddirty(&g->sched);
    }
}

// The key of a message's code block, by which a tab knows the last Run
// typed into it: the session's, the message's record and the block's
// index.
static u64 termrunkey(Str sid, i64 seq, i32 block)
{
    u64 h = hash(sid);
    h ^= (u64)seq*0x9e3779b97f4a7c15u;
    h ^= ((u64)block + 1)*0xc2b2ae3d27d4eb4fu;
    return h;
}

// The folder a session's Run runs in, in a new tab: its root's.
static Str termrunfolder(Gui *g, GuiSession *s)
{
    return termcwd(termroot(g, s));
}

enum : i32 {
    TROUTE_NONE,  // it can't run now (why)
    TROUTE_SHOW,  // its Run has a tab still: shown, nothing typed
    TROUTE_SEND,  // typed at the prompt of an available tab
    TROUTE_NEW,   // typed into a new tab, once its shell is ready
};

struct TermRoute {
    i32      how;    // TROUTE_*
    TermTab *tt;     // SHOW's and SEND's
    i32      held;   // SHOW's: TERMHELD_*, or 0 for a new tab's waiting
    Str      why;    // NONE's
    i64      until;  // when it may go otherwise, else 0 (os_ticks µs)
};

// Where Run in Terminal takes a code block of kind (RUN_*), by key,
// from session from: to the tab that has its Run still, shown; else
// typed into an available tab of its shell's family in from's root
// (termsetpick: the front one, else the right-most) whose shell is in
// the root's folder, or says no folder; else a new tab there; else, with
// no room for one, an available tab in another folder.
static TermRoute termroute(Gui *g, GuiSession *from, i32 kind, u64 key)
{
    TermRoute r   = {};
    Str       why = termwhy(g, from);
    why = why.len ? why : hostwhy(g, from, "term.marks");
    if (!why.len && kind==RUN_CMD && !hostwin(from->host)) {
        why = "cmd blocks run only on Windows";
    }
    if (why.len) {
        r.why = why;
        return r;
    }
    GuiSession *s   = termroot(g, from);
    TermSet    *set = &s->terms;
    i64         now = os_ticks(g->os);
    TermState   sts[TERMTABS];
    for (i32 i = 0; i < set->n; i++) {
        sts[i] = termstate(set->tabs[i]);
        i64 due = termlookdue(&sts[i], now);
        r.until = due && (!r.until || due<r.until) ? due : r.until;
    }
    for (i32 i = 0; i < set->n; i++) {
        TermTab *tt   = set->tabs[i];
        i32      held = termrunheld(&sts[i], key, now);
        if (held || (tt->pend.len && tt->pendkey==key)) {
            r.how  = TROUTE_SHOW;
            r.tt   = tt;
            r.held = held;
            return r;
        }
    }
    Str dir = termrunfolder(g, from);
    b32 win = hostwin(s->host);
    i32 i   = termsetpick(set, sts, kind, now, dir, win, 0);
    why = i<0 ? termfull(g, s) : Str{};
    i   = why.len ? termsetpick(set, sts, kind, now) : i;
    if (i >= 0) {
        r.how = TROUTE_SEND;
        r.tt  = set->tabs[i];
        return r;
    } else if (why.len) {
        r.why = why;
        return r;
    }
    r.how = TROUTE_NEW;
    return r;
}

// Why session s cannot run a code block of kind in a terminal now, else
// empty.
static Str termrunwhy(Gui *g, GuiSession *s, i32 kind, u64 key)
{
    return termroute(g, s, kind, key).why;
}

// The Run button's tooltip, for a code block it can run (termrunwhy):
// where it goes, and whether the agent will hear how it ends.
static Str termruntip(Gui *g, Arena *a, GuiSession *from, i32 kind, Str lang, u64 key)
{
    TermRoute r = termroute(g, from, kind, key);
    if (r.until) {
        schedat(&g->sched, r.until, DUE_CLOCK);
    }
    Buf b(a, 512);
    if (r.how==TROUTE_SHOW || r.how==TROUTE_SEND) {
        Buf lb(a, TERMTITLECAP + 8);
        termlabel(&lb, r.tt);
        Str label = finish(&lb);
        if (r.how == TROUTE_SEND) {
            print(&b, "Run at the prompt of ");
        } else if (r.held == TERMHELD_RAN) {
            print(&b, "It just ran: show ");
        } else {
            print(&b, "It runs in ");
        }
        if (r.tt->tid) {
            print(&b, "Terminal ");
            print(&b, r.tt->tid);
        } else {
            print(&b, "a new Terminal tab");
        }
        print(&b, " (");
        print(&b, label);
        print(&b, r.how==TROUTE_SEND || r.held==TERMHELD_RAN ? Str(")") : Str("): show it"));
        TermState st  = termstate(r.tt);
        b32       win = hostwin(termroot(g, from)->host);
        if (r.how==TROUTE_SEND && !termindir(&st, termrunfolder(g, from), win)) {
            print(&b, ", in ");
            print(&b, st.dir);
            print(&b, ", not the session's folder");
        }
        print(&b, ".");
        if (r.how == TROUTE_SEND) {
            if (termhooked(&st, os_ticks(g->os))) {
                print(&b, " The agent hears how it ended at its next request.");
            }
        }
        return finish(&b);
    }
    print(&b, "Run in a new Terminal tab (");
    print(&b, kind==RUN_PS ? Str("PowerShell") : kind==RUN_CMD ? Str("cmd.exe") : lang);
    print(&b, "), in ");
    print(&b, termrunfolder(g, from));
    print(&b, ".");
    // Whether its shell will be hooked is known only once it starts:
    // by another tab of that family on the same host
    GuiSession *s = termroot(g, from);
    for (i32 i = 0; i < s->terms.n; i++) {
        TermTab *tt = s->terms.tabs[i];
        if (tt->kind==kind && tt->hooked && tt->marked) {
            print(&b, " The agent hears how it ended at its next request.");
            break;
        }
    }
    return finish(&b);
}

// The Run button's: a code block of the agent's typed at a shell's
// prompt in a Terminal tab of the root of from, the session whose
// transcript shows it, whose top on its host (termtop) is told how it
// ended (termroute). A new tab waits for drawterm to size it, or starts
// at the last size for a root not shown, and the block waits for its
// shell's first prompt (termpend). The pane shows the tab, with the
// keyboard. Returns the tab, else null if it cannot run now.
static TermTab *termrun(Gui *g, GuiSession *from, Str lang, Str text, u64 key)
{
    TermRoute r = termroute(g, from, termrunlang(lang), key);
    if (r.how == TROUTE_NONE) {
        return 0;
    }
    navjump(g);
    GuiSession *s   = termroot(g, from);
    Str         top = termtop(g, from)->sid;
    TermTab    *tt  = r.tt;
    if (r.how == TROUTE_NEW) {
        tt = termalloc(g);
        if (tt->pendcap >= text.len) {
            copybytes(tt->pend.data, text.data, text.len);
            tt->pend = {tt->pend.data, text.len};
        } else {
            tt->pend    = clone(&g->perm, text);
            tt->pendcap = text.len;
        }
        tt->pendkey     = key;
        tt->pendat      = os_ticks(g->os);
        tt->pendfromlen = top.len<=countof(tt->pendfrom) ? top.len : 0;
        copybytes(tt->pendfrom, top.data, tt->pendfromlen);
        tt->asklanglen  = lang.len<=countof(tt->asklang) ? lang.len : 0;
        copybytes(tt->asklang, lang.data, tt->asklanglen);
        tt->host = s->host;
        termsetadd(&s->terms, tt);
    } else {
        termsetshow(&s->terms, termsetfind(&s->terms, tt));
        if (r.how == TROUTE_SEND) {
            termrunsend(g, tt, text, key, top);
        }
    }
    setpane(g, PANE_TERMINAL, 1);
    g->termfocus = 1;
    b32 shown = g->sess && termroot(g, g->sess)==s;
    if (!shown && tt->state==TT_NONE) {
        i32 cols = g->termlastcols ? g->termlastcols : 80;
        i32 rows = g->termlastrows ? g->termlastrows : 24;
        termeditgo(g, s, tt, cols, rows);
    }
    scheddirty(&g->sched);
    return tt;
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
    tt->liveat   = os_ticks(g->os);
    tt->shelllen = termkeep(tt->shell, jsonstr(jsonget(j, "shell")));
    Str kind     = jsonstr(jsonget(j, "kind"));
    Str nonce    = jsonstr(jsonget(j, "nonce"));
    tt->kind     = kind==Str("sh") ? TERMK_SH : kind==Str("ps") ? TERMK_PS :
                   kind==Str("cmd") ? TERMK_CMD : TERMK_OTHER;
    tt->hooked   = nonce.len == countof(tt->nonce);
    if (tt->hooked) {
        copybytes(tt->nonce, nonce.data, nonce.len);
    }
    Str editor   = jsonstr(jsonget(j, "editor"));  // Windows names it here
    if (tt->edit && editor.len && !tt->ednamed) {
        tt->titlelen = termtitle(tt->title, editor);
        tt->ednamed  = tt->titlelen > 0;
    }
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

// A tab's command run i back, 0 the newest, as the checklist checks
// it: "cmd|code|output", the code "-" if none was given; "-" if none.
static Str termcmddrive(Arena *a, TermTab *tt, i64 i)
{
    TermCmd *c = 0;
    for (i64 k = 0, n = 0; !c && k < TERMCMD_MAX; k++) {
        TermCmd *o = termcmdat(tt->cmds, k);
        c = o && o->state!=TC_PROMPT && n++==i ? o : 0;
    }
    if (!c || !tt->t) {
        return "-";
    }
    b32 gone = 0;
    Str out  = termcmdoutput(a, tt->cmds, tt->t, c, &gone);
    Buf b(a, 256);
    print(&b, termcmdstr(c));
    putbyte(&b, '|');
    if (c->hascode) {
        print(&b, (i64)c->code);
    } else {
        putbyte(&b, '-');
    }
    putbyte(&b, '|');
    print(&b, gone ? Str("(gone)") : out);
    return finish(&b);
}

// A synchronized update begun (on) or ended: the screen as it stands
// held for drawing, its room taken from the region's spare at the first.
static void termhold(Gui *g, TermTab *tt, b32 on)
{
    tt->holding = 0;
    if (!on) {
        return;  // drawn anew, as its record says
    }
    ariel_Modes mo = ariel_modes(tt->t);
    if (mo.cols>tt->capcols || mo.rows>tt->caprows) {
        return;
    }
    if (!tt->held) {
        iz size = (iz)tt->capcols * tt->caprows * (iz)sizeof(ariel_Cell);
        iz pad  = -(uz)tt->spare.beg & (alignof(ariel_Cell) - 1);
        if (!fits(&tt->spare, pad+size)) {
            return;
        }
        allocbytes(&tt->spare, pad);
        tt->held = (ariel_Cell *)allocbytes(&tt->spare, size);
    }
    for (i32 y = 0; y < mo.rows; y++) {
        ariel_row(tt->t, y, tt->held + (iz)y*mo.cols, mo.cols);
    }
    tt->heldcur  = ariel_cursor(tt->t);
    tt->heldcols = mo.cols;
    tt->heldrows = mo.rows;
    tt->heldoff  = ariel_view(tt->t).offset;
    tt->heldat   = os_ticks(g->os);
    tt->holding  = 1;
    schedat(&g->sched, tt->heldat + TERMTAB_SYNC, DUE_APP);  // drawn live then
}

// Whether tt draws its held screen now: its update under way, its time
// not up, and the view as it was.
static b32 termheld(Gui *g, TermTab *tt, ariel_Modes mo)
{
    return tt->holding && g->sched.now<tt->heldat+TERMTAB_SYNC &&
           mo.cols==tt->heldcols && mo.rows==tt->heldrows &&
           ariel_view(tt->t).offset==tt->heldoff;
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
        case ARIEL_EV_SYNC:
            termhold(g, tt, f.ev.arg);
            break;
        case ARIEL_EV_CWD: {
            // Its hooked shell's folder (OSC 7 with its nonce)
            Str nonce = {tt->nonce, tt->hooked ? countof(tt->nonce) : 0};
            termcmdfolder(tt->cmds, {f.ev.data.data, f.ev.data.len}, nonce, hostwin(tt->host));
        } break;
        case ARIEL_EV_MARK: {
            // Any prompt mark counts a line (termcmd.cpp). Its hooked
            // shell's (OSC 133 with its nonce) keep its command lines,
            // and at a prompt (A), and a command line begun (C), Run
            // goes by them
            Str      nonce = {tt->nonce, tt->hooked ? countof(tt->nonce) : 0};
            TermMark m     = termcmdmark(tt->cmds, tt->t, {f.ev.data.data, f.ev.data.len},
                                         nonce, os_ticks(g->os));
            if (!m.ok) {
                break;
            }
            tt->marked = 1;
            if (m.kind == 'A') {
                tt->prompted = 1;
                if (tt->runkey && !tt->doneat) {
                    tt->doneat = os_ticks(g->os);
                }
                scheddirty(&g->sched);
            } else if (m.kind == 'C') {
                // A Run's next line, if nothing was typed since it, as
                // where a shell without bracketed paste (bash 3.2) runs
                // its lines one by one: it runs still
                tt->prompted = 0;
                tt->doneat   = tt->typedat>tt->sentat ? tt->doneat : 0;
                scheddirty(&g->sched);
            }
        } break;
        case ARIEL_EV_TITLE:
            if (tt->edit) {
                // Its editor's name, by OSC 1 (the helper's), once; no
                // window title, cmd.exe's or the editor's, renames it
                if (f.ev.arg==1 && !tt->ednamed) {
                    tt->titlelen = termtitle(tt->title, {f.ev.data.data, f.ev.data.len});
                    tt->ednamed  = tt->titlelen > 0;
                    if (termshown(g, tt, 0)) {
                        scheddirty(&g->sched);
                    }
                }
            } else if (!f.ev.arg) {
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
        tt->fed   += fresh.len;
        tt->outat  = os_ticks(g->os);
        termack(g, tt, tt->fed, 0);
        // Hidden tabs parse, unseen; a Run waiting for one looks again
        return termshown(g, tt, 1) || tt->pend.len;
    }

    case EV_TERMEND: {
        i64 off = jsonint(jsonget(j, "off"), -1);
        if (tt->state==TT_LIVE && off>tt->fed && !jsonstr(jsonget(j, "err")).len) {
            // A gap before the end: the tail again, from where it was
            // lost, then the end again (the host sends it once all is)
            termack(g, tt, tt->fed, 1);
            return 0;
        }
        tt->code   = (i32)jsonint(jsonget(j, "code"), -1);
        tt->signal = (i32)jsonint(jsonget(j, "signal"), 0);
        tt->busy   = 0;
        Str err    = jsonstr(jsonget(j, "err"));
        if (off >= 0) {
            termack(g, tt, off<tt->fed ? off : tt->fed, 0);
        }
        GuiSession *o = termowner(g, tt);
        if (!tt->code && !tt->signal && !err.len && off==tt->fed) {
            // A clean exit closes its tab; an editor's goes back to its
            // file, which has the focus (editdone), not the prompt
            if (tt->edit) {
                GuiHost *eh   = tt->host;
                Str      path = clone(&scratch, tt->edpath);
                b32      pane = tt->edpane;
                b32      plan = tt->edplan;
                termdrop(g, o, termsetfind(&o->terms, tt), 0);
                editdone(g, o, eh, path, pane, !plan);
                return 1;
            }
            termdrop(g, o, termsetfind(&o->terms, tt), 0);
            if (!o->terms.n && g->panes[PANE_TERMINAL] && g->termfocused) {
                g->promptfocus = 1;
            }
            return 1;
        }
        tt->state  = TT_ENDED;
        tt->errlen = termkeep(tt->err, err);
        if (tt->edit && o) {
            termeditstale(g, o, tt);
        }
        return 1;
    }

    case EV_TERMBUSY: {
        b32 was  = tt->busy;
        tt->busy = jsonflag(jsonget(j, "busy"), 0);
        // Its dot, and where Run goes
        return was!=tt->busy && (termshown(g, tt, 0) || tt->pend.len || tt->runkey);
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

// History's scrollbar's strip, at the grid's right edge.
static f32 termbarw()
{
    return ImGui::GetStyle().ScrollbarSize * 0.6f;
}

// The columns that fit w, the scrollbar's strip reserved whether or not
// it shows, so history's coming doesn't resize the terminal. The room
// past the last whole column goes to it first.
static i32 termfitcols(f32 w, TermMetrics *m)
{
    return (i32)((w - termbarw()) / m->cw);
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
    b32         held  = termheld(g, tt, modes);
    ariel_Cell *cells = alloc<ariel_Cell>(&a, cols);
    f32         cw = m->cw, ch = m->ch;
    f32         top = termsnap((ch - m->size)/2, m->density);  // the glyphs' in a cell
    dl->PushDrawFlag(ImDrawFlags_TextNoPixelSnap, true);

    for (i32 y = 0; y < modes.rows; y++) {
        i32 n  = cols;
        f32 cy = o.y + (f32)y*ch;
        if (held) {
            cells = tt->held + (iz)y*cols;
        } else {
            n = ariel_row(tt->t, y, cells, cols);
        }

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
    ariel_Cursor cur = held ? tt->heldcur : ariel_cursor(tt->t);
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
            if (held) {
                cells = tt->held + (iz)cur.y*cols;
            } else {
                ariel_row(tt->t, cur.y, cells, cols);
            }
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
    termkeyed(g, tt, {s.data, s.len});
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
        termkeyed(g, tt, {out, n});
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
    g->termpasteregion = os_reserve(g->os, in.len + 4096);  // and its header
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
// Ctrl+` and Ctrl+Shift+`, copy and paste, zoom (by the character typed,
// zoomkey), and the menus' Ctrl+Shift chords (menukeys), which route over
// the terminal. Every other Ctrl chord goes to the terminal: Ctrl+N,
// Ctrl+F, Ctrl+1 and the rest, and every chord with AltGr, which types
// (German Ctrl+AltGr+ß is Ctrl+\).
static b32 termkept(Gui *g, ImGuiKey k)
{
    ImGuiIO &io = ImGui::GetIO();
    if (g->mac || !io.KeyCtrl || io.KeyAlt || os_guialtgr(g->os)) {
        return 0;
    }
    switch (k) {
    case ImGuiKey_GraveAccent:
        return 1;
    case ImGuiKey_KeypadAdd:
    case ImGuiKey_KeypadSubtract:
    case ImGuiKey_Keypad0:
        return !io.KeyShift;
    case ImGuiKey_0:          // Shift+0 is = on German layouts
        return !io.KeyShift || zoomkey(g, k, 1);
    case ImGuiKey_V:          // Windows pastes with Ctrl+V too
        return io.KeyShift || HOST_WINDOWS;
    case ImGuiKey_C: case ImGuiKey_N:
    case ImGuiKey_S: case ImGuiKey_T: case ImGuiKey_R:
    case ImGuiKey_L:  // Ask about this
        return io.KeyShift;
    default:
        return zoomkey(g, k, io.KeyShift) != 0;
    }
}

// Whether one of the app's chords (termkept) was pressed this frame.
static b32 termkeptpressed(Gui *g)
{
    static ImGuiKey const keys[] = {
        ImGuiKey_GraveAccent, ImGuiKey_KeypadAdd, ImGuiKey_KeypadSubtract,
        ImGuiKey_Keypad0, ImGuiKey_C, ImGuiKey_V, ImGuiKey_N, ImGuiKey_S,
        ImGuiKey_T, ImGuiKey_R, ImGuiKey_L,
    };
    for (iz i = 0; i < countof(keys); i++) {
        if (ImGui::IsKeyPressed(keys[i], false) && termkept(g, keys[i])) {
            return 1;
        }
    }
    for (iz i = 0; i < countof(zoomkeys); i++) {  // ImGuiKey_0 among them
        if (ImGui::IsKeyPressed(zoomkeys[i], false) && termkept(g, zoomkeys[i])) {
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
static b32 termcopy(Gui *g, Arena scratch, TermTab *tt)
{
    iz n = tt->t ? ariel_copy(tt->t, 0, 0) : 0;
    if (n <= 0) {
        return 0;
    }
    Arena a   = scratch;
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
static void termselect(Gui *g, Arena scratch, TermTab *tt, TermMetrics *m, ImVec2 pos, b32 local, f32 my)
{
    ImGuiIO    &io = ImGui::GetIO();
    ariel_Modes mo = ariel_modes(tt->t);
    f32         h  = (f32)mo.rows * m->ch;
    f32         w  = (f32)mo.cols * m->cw;
    f32         dx = io.MousePos.x - pos.x;  // -FLT_MAX off the window
    f32         dy = my - pos.y;  // unclamped: past the grid scrolls
    f32         cy = dy>0 ? (dy<h ? dy : h-1) : 0;  // clamped before any cast
    dx = dx>0 ? (dx<w ? dx : w) : 0;
    i32         bx = (i32)((dx + m->cw/2) / m->cw);  // a column boundary
    i32         y  = (i32)(cy / m->ch);
    i32 block = io.KeyAlt ? ARIEL_SEL_BLOCK : 0;

    // a11y: selection by drag is the mouse's own
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
            termcopy(g, scratch, tt);
        }
    }
}

// when the app asks (mouse modes), unless Shift is held, which keeps it
// local for selection (I6). The wheel, also when reported, otherwise
// sends arrows on the alternate screen (mode 1007, on by default), and
// later scrolls history (L3). Ctrl+wheel stays the app's zoom. Under
// an ended tab's banner, nothing goes to the program, gone: the mouse
// selects and the wheel scrolls history.
static void termmouse(Gui *g, Arena scratch, TermTab *tt, TermMetrics *m, ImVec2 pos, ImGuiID id, b32 hovered)
{
    ImGuiIO    &io   = ImGui::GetIO();
    ariel_Modes mo   = ariel_modes(tt->t);
    b32         live = tt->state == TT_LIVE;
    b32         rep  = live && mo.mouse && !io.KeyShift;
    if (!live) {
        tt->mheld = 0;
    } else if (!rep && tt->mheld) {
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

    // Cmd+click (macOS), else Ctrl+click: a link or a path opens, unless
    // the program has the mouse (Shift takes it back). Over one, the hand
    if (hovered && io.KeyCtrl && !io.KeyAlt && !rep && ImGui::IsMousePosValid() &&
        termclick(g, scratch, tt, e.x, e.y, 0)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // a11y: a shortcut for the mouse: Open file takes the same path
        if (ImGui::IsMouseClicked(0)) {
            termclick(g, scratch, tt, e.x, e.y, 1);
            return;
        }
    }

    if (rep && ImGui::IsMousePosValid()) {
        static i32 const buttons[] = {1, 3, 2};  // ImGui's left, right, middle
        for (i32 b = 0; b < 3; b++) {
            u32 bit = 1u << b;
            // a11y: the mouse's own, reported to the program
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
    termselect(g, scratch, tt, m, pos, hovered && !rep, at.y);

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
        } else if (live && mo.altscreen && mo.altscroll) {
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

// The line (ariel_lines) at viewport row y, else -1: none on the
// alternate screen, which has no history or records.
static i64 termviewline(TermTab *tt, i32 y)
{
    ariel_Lines ls = ariel_lines(tt->t);
    if (ls.cursor<0 || y<0 || y>=tt->rows) {
        return -1;
    }
    i64 row = ls.end - tt->rows - ariel_view(tt->t).offset + y;
    return row>=ls.first && row<ls.end ? row : -1;
}

// What Ask about this attaches from a tab: the selection, else the
// command at line row, else (row -1, or at the prompt waiting now) the
// last one run, else the tab's last lines; its label to *label. A command's output is cut to its
// first and last lines, as read_terminal's is, but more of them.
static Str termasktext(Gui *g, TermTab *tt, i64 row, Arena *a, Str *label)
{
    enum { HEAD = 50, TAIL = 200, MAXBYTES = 64<<10, LINEMAX = 2000 };
    TermCmds *cs   = tt->cmds;
    Buf       b(a, MAXBYTES + 4096);
    print(&b, "Terminal ");
    print(&b, tt->tid);
    if (cs && cs->cwdlen) {
        print(&b, ", in ");
        print(&b, Str{cs->cwd, cs->cwdlen});
    }
    *label = "terminal.txt";
    if (termselected(tt)) {
        iz  n   = ariel_copy(tt->t, 0, 0);
        u8 *buf = alloc<u8>(a, n+1);
        n = ariel_copy(tt->t, buf, n+1);
        Str sel = termclip(a, {buf, n}, HEAD, TAIL, LINEMAX, MAXBYTES);
        print(&b, ", text the user selected:\n\n");
        print(&b, sel);
        *label = "terminal-selection.txt";
        return finish(&b);
    }
    TermCmd *c = 0;
    if (cs && row>=0) {
        b32 inout = 0;
        c = termcmdatrow(cs, tt->t, row, &inout);
    }
    c = c && c->state!=TC_PROMPT ? c : 0;
    for (i64 i = 0; cs && !c && i < cs->n && i < TERMCMD_MAX; i++) {
        TermCmd *r = termcmdat(cs, i);
        c = r && r->state!=TC_PROMPT ? r : 0;
    }
    if (c && c->state!=TC_PROMPT) {
        print(&b, ", a command the user ran:\n\n$ ");
        print(&b, termcmdstr(c));
        print(&b, c->cmdcut ? Str("...\n") : Str("\n"));
        if (c->cwdlen && !(cs->cwdlen && termcmdcwd(c)==Str{cs->cwd, cs->cwdlen})) {
            print(&b, "(in ");
            print(&b, termcmdcwd(c));
            print(&b, ")\n");
        }
        i64 now = os_ticks(g->os);
        if (c->state == TC_RUNNING) {
            print(&b, "\nIt is still running, for ");
            printelapsed(&b, (now - c->startat) / MS);
        } else if (c->hascode) {
            print(&b, "\nIt exited with code ");
            print(&b, (i64)c->code);
            print(&b, " after ");
            printelapsed(&b, (c->endat - c->startat) / MS);
        } else {
            print(&b, "\nIt ended after ");
            printelapsed(&b, (c->endat - c->startat) / MS);
            print(&b, ", its shell giving no status");
        }
        b32 gone = 0;
        Str out  = termcmdoutput(a, cs, tt->t, c, &gone);
        i64 lines = out.len ? 1 : 0;
        for (iz i = 0; i < out.len; i++) {
            lines += out[i] == '\n';
        }
        if (gone) {
            print(&b, ". Its output is no longer in the terminal's history.\n");
        } else if (!out.len) {
            print(&b, c->state==TC_RUNNING ? Str(". It has printed nothing yet.\n")
                                           : Str(". It printed nothing.\n"));
        } else {
            print(&b, c->state==TC_RUNNING ? Str(". Its output so far (")
                                           : Str(". Its output ("));
            print(&b, lines);
            print(&b, lines==1 ? Str(" line):\n\n") : Str(" lines):\n\n"));
            print(&b, termclip(a, out, HEAD, TAIL, LINEMAX, MAXBYTES));
        }
        *label = "terminal-command.txt";
        return finish(&b);
    }
    // No record: the last lines, as the screen and history hold them
    ariel_Lines ls   = ariel_lines(tt->t);
    i64         from = ls.end - TAIL;
    from = from>ls.first ? termlinefirst(tt->t, from) : ls.first;
    Buf      tb(a, 4096);
    TermSink k = {};
    k.b = &tb;
    Str text = ls.cursor<0 ? Str{} : termrowstext(&k, tt->t, from, 0, ls.end);
    finish(&tb);
    print(&b, text.len ? Str(", its last lines:\n\n") : Str(", which shows nothing.\n"));
    print(&b, termclip(a, text, 0, TAIL, LINEMAX, MAXBYTES));
    return finish(&b);
}

// Files' (files.cpp, after this)
static FileTab *filesopen(Gui *, GuiSession *, u64, Str, i32, i32, Str);
static u64      fileuid(GuiHost *);

// A file a tab names (a link, or a path, Cmd/Ctrl+clicked), relative to base,
// a folder on the tab's host: shown in Files at its lines, in the tab's
// root's set, if go; else only whether it would be. Returns why not, or
// null. On this computer it must be a file there.
static Str termopenref(Gui *g, TermTab *tt, Arena *a, FileRef r, Str base, b32 go)
{
    GuiSession *own  = termowner(g, tt);
    GuiHost    *h    = tt->host;
    b32         win  = h ? hostwin(h) : (b32)HOST_WINDOWS;
    Str         home = h && h->home.len ? h->home : g->main ? g->main->home : Str{};
    u64         uid  = fileuid(h);
    if (!own) {
        return "its tab belongs to no session";
    } else if (r.kind!=FREF_PATH || !r.path.len) {
        return "it names no file";
    }
    FilePath p = filepath(a, r, base, home, win, 0);
    if (p.err.len) {
        return p.err;
    } else if (!uid && os_stat(g->os, *a, p.path).type!=STAT_FILE) {
        return "no such file";
    }
    if (go) {
        filesopen(g, own, uid, p.path, r.line0, r.line1, {});
    }
    return {};
}

// A file as typed or printed: PATH[:LINE[:COL]].
static FileRef termfileref(Str text)
{
    FileRef   r = {};
    FrefLines l = {};
    iz        n = frefsuffix(text, &l);
    r.path = cuttail(text, n);
    r.kind = FREF_PATH;
    freflines(&r, l);
    return r;
}

// What Cmd/Ctrl+click opens at viewport cell (x, y): an OSC 8 link's
// URI (*link), else the word there as a path or a URL might be printed,
// bounded by blanks, quotes and brackets, across wrapped rows, its
// trailing punctuation dropped.
static Str termclickword(Arena *a, TermTab *tt, i32 x, i32 y, b32 *link)
{
    enum { ROWS = 8 };
    ariel_Term *t    = tt->t;
    i32         cols = tt->cols;
    *link = 0;
    if (x<0 || y<0 || y>=tt->rows || x>=cols || cols>TERMCMD_ROW) {
        return {};
    }
    ariel_Cell cells[TERMCMD_ROW];
    ariel_row(t, y, cells, countof(cells));
    if (cells[x].link) {
        ariel_Str u = ariel_link(t, cells[x].link);
        *link = u.len > 0;
        return u.len ? clone(a, Str{u.data, u.len}) : Str{};
    }
    i32 y0 = y, y1 = y;
    for (; y0>0 && y-y0<ROWS && (ariel_rowinfo(t, y0-1).flags & ARIEL_R_WRAPPED); y0--) {}
    for (; y1<tt->rows-1 && y1-y0<ROWS && (ariel_rowinfo(t, y1).flags & ARIEL_R_WRAPPED); y1++) {}
    u32 *cps = alloc<u32>(a, (iz)(y1-y0+1)*cols);
    iz   n   = 0;
    iz   at  = -1;
    for (i32 r = y0; r <= y1; r++) {
        i32 w = ariel_row(t, r, cells, countof(cells));
        for (i32 c = 0; c < cols; c++) {
            if (c<w && !cells[c].width) {
                at = r==y && c==x ? n-1 : at;  // a wide character's right half
                continue;
            }
            at = r==y && c==x ? n : at;
            cps[n++] = c<w && cells[c].cp ? cells[c].cp : ' ';
        }
    }
    auto stop = [](u32 c) {
        return c<=' ' || c=='"' || c=='\'' || c=='`' || c=='(' || c==')' || c=='[' ||
               c==']' || c=='{' || c=='}' || c=='<' || c=='>' || c=='|' || c==',' || c==';';
    };
    if (at<0 || stop(cps[at])) {
        return {};
    }
    iz lo = at, hi = at + 1;
    for (; lo>0 && !stop(cps[lo-1]); lo--) {}
    for (; hi<n && !stop(cps[hi]); hi++) {}
    for (; hi>lo && (cps[hi-1]=='.' || cps[hi-1]==':' || cps[hi-1]=='!' ||
                     cps[hi-1]=='?'); hi--) {}
    Buf b(a, (hi - lo)*4);
    for (iz i = lo; i < hi; i++) {
        u8 enc[4];
        print(&b, Str{enc, utf8encode(enc, (i32)cps[i])});
    }
    return finish(&b);
}

static b32 termweburl(Str s)
{
    return startswith(s, "https://") || startswith(s, "http://");
}

// Cmd+click on macOS, else Ctrl+click, at viewport cell (x, y): a link
// or a URL to the browser; a file:// link, or a path, in Files, a
// relative one in the folder of the command whose lines hold it, else
// the shell's now, else the session's. Whether there is one, opened if
// go.
static b32 termclick(Gui *g, Arena scratch, TermTab *tt, i32 x, i32 y, b32 go)
{
    Arena a    = scratch;
    b32   link = 0;
    Str   w    = termclickword(&a, tt, x, y, &link);
    if (!w.len) {
        return 0;
    } else if (termweburl(w)) {
        if (go && !os_guiopenurl(g->os, a, w)) {
            GuiSession *own = termowner(g, tt);
            if (own) {
                setnote(g, own, concat(&a, "the browser could not be opened: visit ", w), 1);
            }
        }
        return 1;
    }
    FileRef r = {};
    if (link) {
        if (w.len<5 || !equalfold(takehead(w, 5), "file:")) {
            return 0;
        }
        // Its host, if named, is the one the program ran on: the tab's
        Str rest = cuthead(w, 5);
        if (startswith(rest, "//")) {
            Cut h = cut(cuthead(rest, 2), '/');
            rest  = cuthead(rest, 2 + h.head.len);
        }
        r = fileref(&a, concat(&a, "file://", rest));
    } else {
        r = termfileref(w);
    }
    TermCmds *cs   = tt->cmds;
    Str       base = cs && cs->cwdlen ? Str{cs->cwd, cs->cwdlen} : Str{};
    i64       row  = termviewline(tt, y);
    b32       out  = 0;
    TermCmd  *c    = cs && row>=0 ? termcmdatrow(cs, tt->t, row, &out) : 0;
    base = c && c->cwdlen ? termcmdcwd(c) : base;
    GuiSession *own = termowner(g, tt);
    base = base.len || !own ? base : termcwd(own);
    return !termopenref(g, tt, &a, r, base, go).len;
}

// Ask about this: attach termasktext to the prompt of the session
// shown, if the tab is among its tabs, else the tab's own, and give the
// box the keyboard.
static void termask(Gui *g, TermTab *tt, i64 row, Arena a)
{
    GuiSession *own = termowner(g, tt);
    GuiSession *s   = g->sess && own && termroot(g, g->sess)==own ? g->sess : own;
    if (!s) {
        return;
    }
    Str label = {};
    Str text  = termasktext(g, tt, row, &a, &label);
    Str err   = attachadd(g, &a, s, text, label);
    if (err.len) {
        setnote(g, s, concat(&a, "not attached: ", err), 1);
        return;
    }
    if (s != g->sess) {
        guiselect(g, s);
    }
    g->promptfocus = 1;
    scheddirty(&g->sched);
}

// The grid's context menu (#66): Copy, Paste and Select all, whose
// keys off macOS are not a text field's. On a right click the program
// doesn't take (no mouse mode, or Shift held, which keeps the mouse
// local as termmouse does), or as an assistive tool asks. Its picks act
// at once, Paste as its keys do (termpaste, which may ask first). The
// grid has the keyboard back as it closes, but for a click elsewhere.
static void termmenu(Gui *g, Arena scratch, TermTab *tt, TermMetrics *m, ImVec2 pos, ImGuiID id,
                     b32 hovered, Arena a)
{
    ImGuiIO    &io   = ImGui::GetIO();
    ariel_Modes mo   = ariel_modes(tt->t);
    b32         live = tt->state == TT_LIVE;
    b32         rep  = live && mo.mouse && !io.KeyShift;
    // a11y: a tool asks for it by axmenuasked (axmenu marks the grid)
    b32         open = (hovered && !rep && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ||
                       axmenuasked(g, id);
    ImGui::PushOverrideID(id);
    if (open) {
        ImGui::OpenPopup("##termmenu");
        f32 dy = io.MousePos.y - pos.y;
        tt->menurow = axmenuasked(g, id) || dy<0 ? -1 : termviewline(tt, (i32)(dy / m->ch));
    }
    b32 shown = ImGui::BeginPopup("##termmenu");
    if (shown) {
        b32 sel  = termselected(tt);
        b32 clip = live && ImGui::GetClipboardText() && *ImGui::GetClipboardText();
        g->cmenushown = 1;
        g->cmenustate = (sel ? 4 : 0) | (clip ? 8 : 0);
        if (ImGui::MenuItem("Copy", g->mac ? "Cmd+C" : "Ctrl+Shift+C", false, sel)) {
            termcopy(g, scratch, tt);
        }
        itemrect(g, RECT_CMCOPY);
        if (ImGui::MenuItem("Paste", g->mac ? "Cmd+V" : "Ctrl+Shift+V", false, clip)) {
            termpaste(g, tt, a);
        }
        itemrect(g, RECT_CMPASTE);
        if (ImGui::MenuItem("Select all")) {
            ariel_select(tt->t, ARIEL_SEL_ALL, 0, 0);
            if (g->termcopysel) {
                termcopy(g, scratch, tt);
            }
        }
        itemrect(g, RECT_CMSELALL);
        ImGui::Separator();
        if (ImGui::MenuItem("Ask about this", g->mac ? "Cmd+Shift+L" : "Ctrl+Shift+L")) {
            termask(g, tt, tt->menurow, a);
        }
        itemrect(g, RECT_CMASK);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    // a11y: no control: only whether a click closed the menu, so the
    // grid doesn't take the keyboard back from where it went
    b32 elsewhere = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) ||
                    ImGui::IsMouseClicked(2);
    if (tt->menuopen && !shown) {
        tt->refocus = live && !elsewhere;
        scheddirty(&g->sched);
    }
    tt->menuopen = shown;
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
            termcopy(g, a, tt);
        } else if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
            termask(g, tt, -1, a);  // Ask about this
        }
        return;
    }
    if (termkept(g, ImGuiKey_L) && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        termask(g, tt, -1, a);  // Ask about this: Ctrl+Shift+L
    }
    // Copy: Ctrl+Shift+C, and on Windows Ctrl+C while there is a
    // selection, which it clears, else Ctrl+C is ^C. The C it took
    // stays the copy's until let go, sending no repeats.
    if (termkept(g, ImGuiKey_C) && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        termcopy(g, a, tt);
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
                termcopy(g, a, tt);
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
                termkeyed(g, tt, {s.data, s.len});
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
    navjump(g);
    if (io.KeyShift) {
        // A new tab, made by drawterm once the pane's size holds
        g->termaddto = g->sess ? termroot(g, g->sess) : 0;
        setpane(g, PANE_TERMINAL, 1);
        g->termfocus = 1;
    } else if (g->panes[PANE_TERMINAL] && g->termfocused) {
        // To the prompt. Docked beside it, the pane stays, since hiding
        // it would reflow the layout; a tab of the prompt's own node (as
        // Compact has it) stays too, the prompt's tab brought forward;
        // covering it otherwise, or floating, it goes
        ImGuiWindow *w     = ImGui::FindWindowByName(panenames[PANE_TERMINAL]);
        ImGuiWindow *p     = ImGui::FindWindowByName(panenames[PANE_PROMPT]);
        b32          docked = w && w->DockIsActive;
        b32          beside = g->panedrawn[PANE_PROMPT] >= g->total.frames-1;
        b32          paired = docked && p && g->panes[PANE_PROMPT] && p->DockNode==w->DockNode;
        if (paired) {
            setpane(g, PANE_PROMPT, 1);  // its tab in front
        } else if (!docked || !beside) {
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


// The prompt's terminal button: the pane shown and the terminal
// focused, as Ctrl+` does, which starts a shell if the session has none.
static void termgo(Gui *g)
{
    navjump(g);
    setpane(g, PANE_TERMINAL, 1);
    g->termfocus = 1;
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
    navjump(g);
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

// What the banner over an ended or failed tab says: for an editor's,
// naming it ("The editor" until it is named).
static Str termbannertext(Arena *a, TermTab *tt)
{
    Str name = termeditname(tt);
    Str who  = tt->edit ? Str("The editor") : Str("The terminal");
    Buf b(a, 256);
    if (tt->state == TT_FAILED) {
        print(&b, who);
        print(&b, " could not start: ");
        print(&b, termerr(tt));
    } else if (tt->errlen) {
        print(&b, who);
        print(&b, " ended: ");
        print(&b, termerr(tt));
    } else if (tt->signal) {
        print(&b, !tt->edit ? Str("The shell") : name.len ? name : who);
        print(&b, " was killed by signal ");
        print(&b, (i64)tt->signal);
    } else {
        print(&b, !tt->edit ? Str("The shell") : name.len ? name : who);
        print(&b, " exited with code ");
        print(&b, (i64)tt->code);
    }
    return finish(&b);
}

// The pane's banner over an ended or failed terminal, with its buttons.
// a11y: the banner's text names the end; Restart and Close are buttons
static void termbanner(Gui *g, GuiSession *s, TermTab *tt, Arena a, i32 cols, i32 rows)
{
    ImGui::PushStyleColor(ImGuiCol_Text, g->pal->amber);
    ImGui::TextWrapped("%s", cstr0(&a, termbannertext(&a, tt)));
    ImGui::PopStyleColor();
    b32 restart = ImGui::Button("Restart");
    itemnamed(g, "term banner restart");
    if (tt->edit) {
        Str name = termeditname(tt);
        Buf b(&a, 64 + tt->edpath.len);
        print(&b, "Run ");
        print(&b, name.len ? name : Str("the editor"));
        print(&b, " on ");
        print(&b, basename(tt->edpath));
        print(&b, " again (Enter)");
        itemtip("%s", cstr0(&a, finish(&b)));
    } else {
        itemtip("Start a new shell (Enter)");
    }
    ImGui::SameLine();
    b32 close = ImGui::Button("Close");
    itemnamed(g, "term banner close");
    restart |= ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (restart) {
        termrestart(g, s, tt, cols, rows);
        g->termfocus = 1;
    } else if (close) {
        termdrop(g, s, termsetfind(&s->terms, tt), 0);
    }
}

// A note over a terminal whose host's link is down, in the grid's top
// right corner, which leaves the screen and its size as they were: the
// shell runs on there, and its output waits for the link.
static void termwaiting(Gui *g, TermTab *tt, ImDrawList *dl, ImRect bb, Arena a)
{
    GuiHost *h = tt->host;
    Buf      b(&a, 128 + h->name.len);
    print(&b, "Reconnecting to ");
    print(&b, h->kind==GH_REMOTE ? h->name : Str("the background host"));
    print(&b, "...");
    char  *text = cstr0(&a, finish(&b));
    ImVec2 pad  = ImGui::GetStyle().FramePadding;
    ImVec2 ts   = ImGui::CalcTextSize(text);
    ImVec2 max  = v2(bb.Max.x - pad.x, bb.Min.y + pad.y + ts.y + 2*pad.y);
    ImVec2 min  = v2(max.x - ts.x - 2*pad.x, bb.Min.y + pad.y);
    min.x = min.x>bb.Min.x ? min.x : bb.Min.x;
    dl->AddRectFilled(min, max, g->pal->panel, ImGui::GetStyle().FrameRounding);
    dl->AddRect(min, max, g->pal->amber, ImGui::GetStyle().FrameRounding);
    dl->PushClipRect(min, max, true);
    dl->AddText(v2(min.x + pad.x, min.y + pad.y), g->pal->amber, text);
    dl->PopClipRect();
    g->termwaitshown = 1;
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
static TermBar termtabs(Gui *g, Arena scratch, GuiSession *s)
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
    Arena   a = scratch;
    ImGuiID ids[TERMTABS] = {};
    for (i32 i = 0; i < set->n; i++) {
        TermTab          *tt    = set->tabs[i];
        bool              open  = true;
        ImGuiTabItemFlags flags = 0;
        if (set->frontset && i==set->front) {
            flags |= ImGuiTabItemFlags_SetSelected;
        }
        if (tt->busy || (termasks(tt) && tt->state==TT_LIVE)) {
            flags |= ImGuiTabItemFlags_UnsavedDocument;
        }
        if (tt->tid) {
            flags |= ImGuiTabItemFlags_NoTooltip;  // its own, below
        }
        Buf b(&a, TERMTITLECAP + tt->edpath.len + 40);
        termlabel(&b, tt);
        print(&b, "###tt");
        print(&b, (i64)tt->serial);
        b32 shown = ImGui::BeginTabItem(cstr0(&a, finish(&b)), &open, flags);
        ids[i] = ImGui::GetItemID();
        if (tt->tid) {
            // The engine's number, by which the agent knows it
            Buf lb(&a, TERMTITLECAP + tt->edpath.len + 32);
            termlabel(&lb, tt);
            Str label = finish(&lb);
            itemtip("%.*s\nTerminal %lld", (int)label.len, label.cdata, (long long)tt->tid);
        }
        if (ImGui::IsItemDeactivated()) {
            // On release, not press: focus takes the active ID, which
            // would end a drag before it started
            g->termfocus = 1;
        }
        Buf nb(&a, 32);
        print(&nb, "term tab ");
        print(&nb, (i64)i + 1);
        itemnamed(g, finish(&nb));
        // a11y: an editor's tab's menu names its editor in each tip
        if (uirclicked(g)) {
            ImGui::OpenPopupEx(ids[i]);  // as BeginPopupContextItem, by the tab's ID
        }
        if (ImGui::BeginPopupEx(ids[i], ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar|
                                ImGuiWindowFlags_NoSavedSettings)) {
            if (tt->edit) {
                Str name = termeditname(tt);
                name = name.len ? name : Str("the editor");
                Str live = tt->state==TT_LIVE ? Str{} : Str("Its editor has ended");
                Str why  = termwhy(g, s);
                why = why.len ? why : hostwhy(g, s, "term.edit");
                if (menutip(g, &a, "Close", {},
                            concat(&a, concat(&a, "Hang up ", name),
                                   ": what it has not saved may be lost"))) {
                    r.closed = i;
                }
                if (menutip(g, &a, "Restart", why,
                            concat(&a, concat(&a, "Hang up ", name),
                                   " and open the file again in this tab"))) {
                    r.restart = i;
                }
                if (menutip(g, &a, "Kill", live,
                            concat(&a, concat(&a, "Close it, ending ", name),
                                   " and what it started"))) {
                    r.closed = i;
                    r.kill   = 1;
                }
            } else {
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
        itemtip("New terminal tab (%s)", cstr0(&a, termkey(g, &a, 1)));
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
// The grid's size in cells, COLS×ROWS, centered over it while the
// pane's size changes and a moment after, as other terminals show it,
// to aim a resize at a width. Not as it first shows, nor for a tab
// switched to, nor a zoom: only the pane resized while in view.
enum : i64 { TERM_SIZESHOW = 1000*1000 };  // µs

static void termsizetip(Gui *g, ImDrawList *dl, ImRect bb, i32 cols, i32 rows, Arena a)
{
    i64 until = g->termsizeat + TERM_SIZESHOW;
    if (!g->termsizeat || g->sched.now>=until) {
        return;
    }
    schedat(&g->sched, until, DUE_CLOCK);
    g->termsizeshown = 1;
    Buf b(&a, 32);
    print(&b, (i64)cols);
    print(&b, "\xc3\x97");  // a multiplication sign
    print(&b, (i64)rows);
    char const *s   = cstr0(&a, finish(&b));
    f32         sc  = g->scale;
    ImVec2      ts  = ImGui::CalcTextSize(s);
    ImVec2      c   = bb.GetCenter();
    ImVec2      pad = v2(12*sc, 6*sc);
    ImVec2      p0  = v2(c.x - ts.x/2 - pad.x, c.y - ts.y/2 - pad.y);
    ImVec2      p1  = v2(c.x + ts.x/2 + pad.x, c.y + ts.y/2 + pad.y);
    dl->AddRectFilled(p0, p1, g->pal->frame, 6*sc);
    dl->AddRect(p0, p1, g->pal->rule, 6*sc, 1*sc);
    dl->AddText(v2(p0.x + pad.x, p0.y + pad.y), g->pal->paneltext, s);
}

static void drawterm(Gui *g)
{
    Arena        a   = g->frame;
    GuiSession  *s   = g->sess ? termroot(g, g->sess) : 0;
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    ImGuiID      id  = win->GetID("##grid");
    g->termid        = id;
    g->termfocused   = 0;
    g->termwaitshown = 0;
    g->termsizeshown = 0;
    g->termgrid      = {};
    if (g->termletgo) {
        // An editor's tab closed, back to its file in Files: the
        // keyboard goes with the focus, not to the tab now in front
        g->termletgo = 0;
        if (GImGui->ActiveId == id) {
            ImGui::ClearActiveID();
        }
    }

    Str why = termwhy(g, s);
    if (why.len && (!s || !s->terms.n)) {
        ImGui::TextDisabled("%s", cstr0(&a, why));
        return;
    }
    TermBar bar = termtabs(g, a, s);
    termpasteask(g, a);

    TermMetrics m     = termmetrics(g);
    ImVec2      avail = ImGui::GetContentRegionAvail();
    i32         cols  = termfitcols(avail.x, &m);
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
    // The pane resized changes it, which termsizetip says; not a zoom,
    // which changes the cells, nor the pane's first showing
    b32 resized = avail.x!=g->termlastavail.x || avail.y!=g->termlastavail.y;
    if (g->termlastcols && !win->Appearing && resized &&
            (cols!=g->termlastcols || rows!=g->termlastrows)) {
        g->termsizeat = g->sched.now;
    }
    g->termlastavail = avail;
    g->termlastcols  = cols;
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
    // Editors' tabs, and new shells' for a Run, waiting to start, as a
    // new shell does, once the size holds
    for (i32 i = 0; held && i<s->terms.n; i++) {
        TermTab *p = s->terms.tabs[i];
        if (termasks(p) && p->state==TT_NONE) {
            termeditgo(g, s, p, cols, rows);
        }
    }

    TermTab *tt = termsetfront(&s->terms);
    if (tt && termasks(tt) && tt->state==TT_NONE) {
        return;  // it starts once the size holds, as asked
    } else if (!tt || tt->state==TT_NONE) {
        if (g->termfocus && !held) {
            return;
        } else if (g->termfocus || ImGui::Button("Start a shell")) {
            g->termfocus = 1;
            tt = termnew(g, s, cols, rows);
            if (!tt) {
                return;
            }
        } else {
            ImGui::TextDisabled("%s starts one, and comes back to it",
                                cstr0(&a, termkey(g, &a, 0)));
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
    axname(g, "Terminal");
    axrole(g, AX_GROUP);  // no field: its text model is to come
    axfocusable(g);
    axmenu(g);  // termmenu
    // History's scrollbar, in the strip reserved at the grid's right edge
    // (termfitcols): the grid hovers apart from it
    ariel_View view  = ariel_view(tt->t);
    b32        sb    = view.total > tt->rows;
    f32        barw  = termbarw();
    ImRect     barbb = ImRect(v2(bb.Max.x - barw, bb.Min.y), bb.Max);
    ImRect     hovbb = sb ? ImRect(bb.Min, v2(barbb.Min.x, bb.Max.y)) : bb;

    ImGuiContext &c       = *GImGui;
    b32           hovered = ImGui::ItemHoverable(hovbb, id, 0);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    // ImGui's activation, a tool's or Enter or Space on it as the
    // keyboard's item, focuses it as Ctrl+` does, once that key is up, so
    // neither it nor its repeats reach the shell. Only this waits: the
    // rest focus it at once, as before, lest the grid take the active ID
    // frames later, when a click meant for another item can't hover it
    g->termnavgo |= c.NavActivateId == id;
    b32 keyup = !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
                !ImGui::IsKeyDown(ImGuiKey_Space);
    // Focused so, or by a click off the scrollbar (a11y: not uiclicked,
    // which takes that in)
    if (g->termfocus || tt->refocus || (g->termnavgo && keyup) ||
            (hovered && ImGui::IsMouseClicked(0))) {
        g->termfocus = 0;
        tt->refocus  = 0;
        g->termnavgo = 0;
        ImGui::SetActiveID(id, win);
        ImGui::SetFocusID(id, win);
        ImGui::FocusWindow(win);
        ImGui::SetActiveIdUsingAllKeyboardKeys();
    } else if (c.ActiveId==id && ImGui::IsMouseClicked(0) && !hovered) {
        // a11y: a click elsewhere lets go: no control of its own
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
                        termcmdresized(tt->cmds);
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
    if (!termup(tt) && (tt->state==TT_LIVE || tt->state==TT_OPENING)) {
        termwaiting(g, tt, dl, bb, a);
    }
    termsizetip(g, dl, bb, cols, rows, a);
    g->termverts = dl->VtxBuffer.Size;
    if (tt->state==TT_LIVE || tt->state==TT_ENDED) {
        termmouse(g, a, tt, &m, pos, id, hovered);
        termmenu(g, a, tt, &m, pos, id, hovered, a);
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
        // An input method's composition, at the cursor until committed
        ImVec2 imepos;
        if (imedraw(g, id, dl, stylefont(&g->fonts, STYLE_CODE), m.size, ime->InputPos,
                    m.ch, bb, 0, bg, termimcolor(ariel_color(tt->t, ARIEL_C_FG)), &imepos)) {
            ime->InputPos = imepos;
        }
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

// The front tab's held screen (termhold) as text, a line a row, while
// it holds one, else null.
static Str termheldscreen(Gui *g, Arena *a)
{
    TermTab *tt = termfront(g);
    if (!tt || !tt->t || !tt->holding) {
        return {};
    }
    Buf b(a, 256);
    for (i32 y = 0; y < tt->heldrows; y++) {
        ariel_Cell *row = tt->held + (iz)y*tt->heldcols;
        for (i32 x = 0; x < tt->heldcols; x++) {
            if (row[x].width) {
                u8 utf8[4];
                i32 c = row[x].cp ? (i32)row[x].cp : ' ';
                print(&b, Str{utf8, utf8encode(utf8, c)});
            }
        }
        for (; b.len>0 && b.data[b.len-1]==' '; b.len--) {}
        putbyte(&b, '\n');
    }
    return finish(&b);
}

// Where the screen last shows text, its first cell's middle, else
// (-1, -1): ASCII text, so that bytes are cells.
static ImVec2 termtextat(Gui *g, Arena scratch, Str text)
{
    Arena a   = scratch;
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
    i32         cols = termfitcols(cw, &m);
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
