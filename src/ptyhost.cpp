// Terminals: the shells of the Terminal pane's tabs, on this machine
//
// A frontend's terminal tab is a shell that this engine runs on a
// pseudo-terminal (os_ptyspawn), in the session's folder and with its
// shell's settings, while the frontend emulates the terminal (ariel).
// The engine is only a flow-controlled pipe of bytes between the two:
//
//   term.open {sid, cols, rows, xpx?, ypx?, cwd?, edit?}
//       replies {tid, shell, argv, cwd, editor?} before any output
//   term.input {tid, data}            typed bytes, base64, 16 KiB at most
//   term.resize {tid, cols, rows, xpx?, ypx?}
//   term.ack {tid, off, resend?}      output taken up to off
//   term.close {tid, kill?}           a hang-up, or with kill a sweep
//   term.close {all}                  a hang-up of all the frontend's
//
// None but term.open has an id or a reply. Records, transient, with no
// sid, and the peer of a host's client (host.cpp stamps its commands,
// and gives each record to the client its peer names):
//
//   term {tid, off, data}             output at byte offset off, base64
//   term.end {tid, off, code, signal, err?}   the shell exited, its
//                                     output ended at off
//   term.busy {tid, busy}             a job of the shell's has the
//                                     terminal, or no longer
//
// Output goes in records of TERM_LINE bytes while less than TERM_WINDOW
// is unacknowledged, and only while the engine's outbox has room for
// them (mailroom), else again after TERM_RETRY ms. What is unacked stays
// in the platform's ring (TERM_RING) until acked, so a frontend that
// finds a gap acks what it has with resend, and output goes again from
// there (go-back-N), as it does after TERM_STALL seconds of no ack at
// all, TERM_TRIES times, before the terminal is given up as stalled.
// While a host's client has no stream (relay.peer away), its terminals
// count no stalls: their output waits in the client's link, and the
// count starts over once it is back. A
// full ring stops reading the terminal, holding the shell back: a slow
// frontend slows the shell, and typing still goes through.
//
// Closing hangs up, as a terminal's window closing does: the shell gets
// SIGHUP, and what ignores it (nohup) lives on, as does what it
// disowned; only close {kill} sweeps the tree. A quit hangs up every
// terminal, a client of a host gone for good hangs up its own (as does
// a window going to the background, by close {all}, so a term.open
// still in flight leaves no shell behind), and a
// session deleted or trashed hangs up its own. A terminal outlives its
// session's closing, parking, or archiving.
//
// The shell: [terminal] shell, words with "quotes" for a word holding
// blanks, an unquoted ~/ for home; else the session's shell, unless it is a POSIX system's
// default (termshell); else $SHELL; else that default; else /bin/sh. A
// POSIX sh starts as a login shell, but in the GUI's own engine on
// Linux, whose desktop session ran the profile already. Its environment
// is the session shell's (shellover), with TERM (xterm-256color, or
// xterm where the system lacks that terminfo), COLORTERM, TERM_PROGRAM
// and its version, without another terminal's variables, nor COLUMNS
// and LINES.
//
// An editor on a file (cap term.edit), in place of the shell:
//
//   edit {path, line?, col?, editor? | template?}
//
// path is absolute for this machine, line (1 to 1e9) and col (0 to 1e9)
// are clamped, editor is the window's terminal editor's command and
// template its [ui] editor_line; with neither, this host's own editor
// runs. The command is built here, where the editor is: only this host
// knows its login shell, its $VISUAL and $EDITOR, its PATH and how its
// shell quotes. Without the cap an engine would ignore edit and open a
// shell.
//
// On POSIX this program runs again as "--tty-edit" (main_posix.cpp),
// /proc/self/exe on Linux, with the request in the environment, never
// quoted: PROSPERO_EDIT_FILE, _LINE, _COL, _EDITOR or _TEMPLATE, and
// _LOGIN (0 where a terminal's shell would not be a login shell). Phase
// one becomes the login shell ($SHELL, else the account's, else
// /bin/sh) with editshell's flags, which runs "exec SELF --tty-edit-in";
// phase two picks the editor there (editpick: the window's, $VISUAL,
// $EDITOR, vi, by the shell's PATH, less its empty and relative
// entries, which sh would take as the session's folder), names it by
// OSC 1, drops PROSPERO_EDIT_*, and becomes sh -c 'CMD "$@"' NAME ARGS,
// the path a positional word (editorrun). The editor is then the
// terminal's own process, its exit the terminal's; with none, it says so
// and exits 127.
// A profile can test PROSPERO_EDIT_FILE, set while it runs, to skip what
// would keep an editor from starting (exec tmux).
//
// On Windows there is no login shell: the window's editor, %VISUAL%,
// %EDITOR%, then vi, the first found on PATH with PATHEXT (findprog), or
// a refusal before anything starts; cmd.exe runs its line (editorjump)
// with the program, its first word, by its full path, quoted (cmdabs),
// so that cmd.exe's own lookup, which tries the current folder first,
// never picks it; an editor that starts with VAR=value is refused, and
// so is a path an editor reading the ANSI code page might get as more
// than one argument (cmdwordok). The reply's editor is its name. Either
// way the editor runs in the session's folder, and a session whose
// files are another OS's (a container's kept subagent) is refused.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    TERM_MAX     = 32,          // terminals at once
    TERM_PERPEER = 16,          // of them for one frontend
    TERM_RING    = 512 << 10,   // output read ahead of the frontend
    TERM_WINDOW  = 256 << 10,   // output sent and not yet acked
    TERM_LINE    = 16 << 10,    // output bytes per term record
    TERM_INPUT   = 16 << 10,    // input bytes per term.input
    TERM_STALL   = 3,           // seconds without an ack: send again
    TERM_TRIES   = 3,           // that many times, then give up
    TERM_RETRY   = 5,           // ms between tries while held back
    TERM_BUSYMS  = 1000,        // between looks at who has the terminal
    TERM_DIMMAX  = 4096,        // columns or rows
    TERM_EDITPATH = 32 << 10,   // an edit's path, in bytes, under this
    TERM_EDITCMD  = 4 << 10,    // its editor or template
    TERM_HIST     = 2000,       // history lines a screen keeps, at most
    TERM_TYPEAHEAD = 2,         // seconds a line typed may precede echo off
    TERM_CWDCAP   = 1024,       // bytes of a terminal's folder kept
    TERM_SHELLCAP = 64,         // of its shell's name
};

struct Term {
    i64   tid;      // 0 for a free slot
    i64   peer;     // the host's number for its client, 0 for none
    u8    sid[64];
    iz    sidlen;
    Proc *proc;
    u64   procid;
    i64   sent;     // output sent, as a byte offset
    i64   acked;    // output acked, and so taken from the ring
    i64   total;    // output read, once eof
    i32   code;
    i32   signal;
    b32   exited;
    b32   eof;
    b32   ended;    // term.end sent
    b32   closed;   // hung up or killed: waits for exit and EOF, silent
    b32   busy;     // as last told
    b32   away;     // its frontend's link is down: no stalls meanwhile
    i64   stallat;  // ticks: send again, 0 while nothing is unacked
    i32   tries;

    // The screen read_terminal reads, while [terminal] agent is on, fed
    // the output ahead of what is sent (termscan): fed >= sent >= acked
    Region     *region;   // the screen's block, alone in it
    ariel_Term *screen;
    i32         capcols;
    i32         caprows;
    i32         hist;
    i64         fed;
    i64         readfed;  // fed as of the agent's last read of it
    i64         used;     // ticks: opened, or typed into
    i32         mode;     // os_ptymode as last looked at
    TermLine    line;     // the line being typed (termsecret.cpp)
    u8          cand[TERMLINE_CAP];  // the latest echoed line, and when
    i32         candlen;
    i64         candat;
    u8          title[TERMTITLECAP];
    iz          titlelen;
    u8          shell[TERM_SHELLCAP];
    iz          shelllen;
    u8          cwd[TERM_CWDCAP];
    iz          cwdlen;
};

struct Terms {
    Term slots[TERM_MAX];
    i64  nexttid;
    b32  held;      // a record waited for room in the outbox
    i64  busyat;    // ticks: look at who has each terminal
    TermSecrets secrets;  // typed at prompts, in any terminal
    i64  lastread;  // the tid read_terminal read last
};

static Terms *newterms(Arena *perm)
{
    return alloc<Terms>(perm);
}


// Pure parts, for the tests

// How many bytes may go in the next record: no more than a line, nor
// past the window. Zero if none.
static iz termcan(i64 sent, i64 acked, iz window, iz line)
{
    i64 room = window - (sent - acked);
    room = room<line ? room : line;
    return room>0 ? (iz)room : 0;
}

struct TermAck {
    i64 take;  // bytes the ring may drop
    i64 sent;  // where output goes on from
    b32 ok;
};

// A frontend's ack of output up to off, which must lie within what was
// sent and not before what it acked already; with resend, output goes
// again from there.
static TermAck termack(i64 sent, i64 acked, i64 off, b32 resend)
{
    TermAck r = {};
    if (off<acked || off>sent) {
        return r;
    }
    r.take = off - acked;
    r.sent = resend ? off : sent;
    r.ok   = 1;
    return r;
}

// Words, blank-separated, a word in double quotes holding anything but
// a quote. An unquoted word starting with ~/ is in home, as a shell
// would have it. Null if a quote is left open.
static Slice<Str> termsplit(Arena *a, Str s, Str home, b32 win)
{
    Slice<Str> r = {};
    for (iz i = 0; i < s.len;) {
        if (whitespace(s[i])) {
            i++;
            continue;
        } else if (s[i] == '"') {
            iz j = i + 1;
            for (; j<s.len && s[j]!='"'; j++) {}
            if (j == s.len) {
                return {};
            }
            r = push(a, r, Str{s.data+i+1, j-i-1});
            i = j + 1;
            continue;
        }
        iz j = i;
        for (; j<s.len && !whitespace(s[j]); j++) {}
        r = push(a, r, expandhome(a, Str{s.data+i, j-i}, home, win));
        i = j;
    }
    return r;
}

// Other terminals' variables, which would mislead programs here.
static b32 termforeign(Str name)
{
    static Str exact[] = {
        "COLUMNS", "LINES", "TERM", "COLORTERM", "TERM_PROGRAM",
        "TERM_PROGRAM_VERSION", "TERM_SESSION_ID", "TERMINAL_EMULATOR",
        "WT_SESSION", "WT_PROFILE_ID", "TMUX", "TMUX_PANE", "STY",
        "VTE_VERSION", "WINDOWID", "COLORFGBG", "KONSOLE_VERSION",
        "KONSOLE_DBUS_SESSION", "KONSOLE_DBUS_SERVICE", "KONSOLE_DBUS_WINDOW",
        "KONSOLE_PROFILE_NAME", "LC_TERMINAL", "LC_TERMINAL_VERSION",
        "SHELL_SESSION_ID",
    };
    static Str prefix[] = {
        "ITERM_", "KITTY_", "ALACRITTY_", "WEZTERM_", "GHOSTTY_", "WARP_",
    };
    for (iz i = 0; i < countof(exact); i++) {
        if (name == exact[i]) {
            return 1;
        }
    }
    for (iz i = 0; i < countof(prefix); i++) {
        if (startswith(name, prefix[i])) {
            return 1;
        }
    }
    return 0;
}

// A terminal's environment from its shell's: other terminals' variables
// out, this one's in, and on macOS a UTF-8 locale where none is set (a
// GUI that the Finder started has none).
static Slice<Str> termenv(Arena *a, Slice<Str> env, b32 win, b32 mac, Str term)
{
    Slice<Str> r      = {};
    b32        locale = 0;
    for (iz i = 0; i < env.len; i++) {
        Str name = cut(env[i], '=').head;
        locale |= name=="LANG" || name=="LC_ALL" || name=="LC_CTYPE";
        if (!termforeign(name)) {
            r = push(a, r, env[i]);
        }
    }
    r = envset(a, r, "TERM", term, win);
    r = envset(a, r, "COLORTERM", "truecolor", win);
    r = envset(a, r, "TERM_PROGRAM", "Prospero", win);
    r = envset(a, r, "TERM_PROGRAM_VERSION", PROSPERO_VERSION, win);
    if (mac && !locale) {
        r = envset(a, r, "LANG", "en_US.UTF-8", win);
    }
    return r;
}

// The shell's argv: the configured words, ~ for home, else the session's shell as
// Open in external terminal opens it, unless it is the system's default, else
// $SHELL, else that default, else /bin/sh; a POSIX sh as a login shell
// where login.
static Slice<Str> termargv(Arena *a, Str conf, Str home, Str kind, Str path,
                           Str distro, Str envshell, b32 win, b32 login)
{
    Slice<Str> r = termsplit(a, conf, home, win);
    if (r.len) {
        return r;
    }
    r = termshell(a, kind, path, win, distro);
    if (r.len) {
        if (!login && kind=="sh" && r.len==2 && r[1]=="-l") {
            r.len = 1;
        }
        return r;
    }
    Str sh = envshell.len ? envshell : path.len ? path : Str("/bin/sh");
    r = push(a, r, sh);
    if (login && !win) {
        r = push(a, r, Str("-l"));
    }
    return r;
}

struct TermEditAsk {
    b32 on;      // term.open has edit
    Str path;
    i32 line;    // 1-based, 0 for none
    i32 col;     // 0 for none
    Str editor;  // the window's editor, else null
    Str tmpl;    // or its template, else null
    Str why;     // why refused, else null
};

// A number of edit's, clamped to lo..1e9, or *why.
static i32 termeditnum(Json *j, i32 lo, Str name, Str *why)
{
    if (j->type == JSON_MISSING) {
        return 0;
    } else if (j->type != JSON_NUMBER) {
        *why = name;
        return 0;
    }
    f64 v = jsonfloat(j, lo);
    v = v<lo ? lo : v>1e9 ? 1e9 : v;
    return (i32)v;
}

// term.open's edit, checked: absent, or an object whose path is
// absolute for this machine (win: Windows), with no NUL and under
// TERM_EDITPATH, and on Windows nothing cmd.exe cannot pass on (C10);
// editor or template, not both, each a string under TERM_EDITCMD with
// no NUL (empty is none).
static TermEditAsk termeditask(Json *j, b32 win)
{
    TermEditAsk r  = {};
    Json       *ed = jsonget(j, "edit");
    if (ed->type == JSON_MISSING) {
        return r;
    }
    r.on = 1;
    if (ed->type != JSON_OBJECT) {
        r.why = "edit must be an object";
        return r;
    }
    Json *p = jsonget(ed, "path");
    r.path = jsonstr(p);
    if (p->type!=JSON_STRING || !pathroot(r.path, win) || cut(r.path, 0).ok) {
        r.why = "edit.path must be an absolute path";
        return r;
    } else if (r.path.len >= TERM_EDITPATH) {
        r.why = "edit.path is too long";
        return r;
    }
    switch (win ? cmdwordok(r.path) : (i32)CMDWORD_OK) {
    case CMDWORD_CMD:
        r.why = "edit.path holds %, \" or a control character, which cmd.exe cannot pass on";
        return r;
    case CMDWORD_BESTFIT:
        r.why = "edit.path holds a character Windows may pass to the editor as \" or \\";
        return r;
    }
    r.line = termeditnum(jsonget(ed, "line"), 1, "edit.line must be a number", &r.why);
    r.col  = termeditnum(jsonget(ed, "col"), 0, "edit.col must be a number", &r.why);
    if (r.why.len) {
        return r;
    }
    Str  names[] = {"editor", "template"};
    Str *dsts[]  = {&r.editor, &r.tmpl};
    for (iz i = 0; i < countof(names); i++) {
        Json *v = jsonget(ed, names[i]);
        if (v->type == JSON_MISSING) {
            continue;
        } else if (v->type != JSON_STRING) {
            r.why = i ? Str("edit.template must be a string") : Str("edit.editor must be a string");
            return r;
        } else if (v->str.len>=TERM_EDITCMD || cut(v->str, 0).ok) {
            r.why = i ? Str("edit.template must be under 4 KiB, with no NUL")
                      : Str("edit.editor must be under 4 KiB, with no NUL");
            return r;
        }
        *dsts[i] = trim(v->str);
    }
    if (r.editor.len && r.tmpl.len) {
        r.why = "edit: give editor or template, not both";
    }
    return r;
}

// An editor's name, as the reply gives it: its program's file name
// without .exe, .com, .cmd or .bat.
static Str termeditname(Str prog)
{
    Str name = basename(prog);
    Str exts[] = {".exe", ".com", ".cmd", ".bat"};
    for (iz i = 0; i < countof(exts); i++) {
        if (name.len>exts[i].len && equalfold(cuthead(name, name.len-exts[i].len), exts[i])) {
            return takehead(name, name.len-exts[i].len);
        }
    }
    return name;
}

// A session as the walk to its terminals' owner sees it.
struct TermNode {
    b32 found;
    Str parent;    // a subagent's on this host, else empty
    Str sideof;    // a side chat's original, else empty
    b32 worktree;  // in a worktree of its own
    Str cwd;
};

typedef TermNode TermLook(void *ctx, Arena *, Str sid);

// The session whose terminals a session shares, as the frontend files
// them (term.cpp's termroot): the top of its parents on this host, then
// a side chat's original's, if in the same folder, and so on. A
// worktree session, in a folder of its own, has its own.
static Str termrootwalk(Arena *a, Str sid, TermLook *look, void *ctx)
{
    for (i32 side = 0; side < 8; side++) {
        TermNode n = look(ctx, a, sid);
        for (i32 up = 0; up<16 && n.found && n.parent.len && !(n.parent==sid); up++) {
            TermNode p = look(ctx, a, n.parent);
            if (!p.found) {
                break;
            }
            sid = n.parent;
            n   = p;
        }
        if (!n.found || !n.sideof.len || n.worktree || n.sideof==sid) {
            break;
        }
        TermNode o = look(ctx, a, n.sideof);
        if (!o.found || !(o.cwd == n.cwd)) {
            break;
        }
        sid = n.sideof;
    }
    return sid;
}


// The table

// Whether the agent may read terminals, and so the engine keeps their
// screens: [terminal] agent.
static b32 termagent(Engine *e)
{
    return cfgbool(cfgget(e->cfg, "terminal", {}, "agent", 0)) != 0;
}

// For status: the screens kept and the output they were fed, for tests.
static void termstatus(Engine *e, JsonOut *w)
{
    i64 screens = 0, fed = 0;
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        screens += t->tid && t->screen;
        fed     += t->tid ? t->fed : 0;
    }
    putintkey(w, "screens", screens);
    putintkey(w, "fed", fed);
}

static Term *termbytid(Engine *e, i64 tid, i64 peer)
{
    for (i32 i = 0; e->terms && tid && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (t->tid==tid && t->peer==peer) {
            return t;
        }
    }
    return 0;
}

static Term *termbyproc(Engine *e, u64 id)
{
    for (i32 i = 0; e->terms && id && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (t->tid && t->procid==id) {
            return t;
        }
    }
    return 0;
}

// Terminals open, not hung up, for a frontend (peer), or for all (-1).
static i32 termcount(Engine *e, i64 peer)
{
    i32 n = 0;
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        n += t->tid && !t->closed && (peer<0 || t->peer==peer);
    }
    return n;
}

// Hang up, or with kill sweep the tree. The terminal says nothing more,
// and its slot goes once the shell's exit and EOF are in.
// Free a slot, and its screen.
static void termfree(Engine *e, Term *t)
{
    if (t->region) {
        os_release(e->os, t->region);
    }
    *t = {};
}

// The capacity for a screen's size: room to grow before a move.
static i32 termscreencap(i32 n, i32 least)
{
    i32 r = n + n/2;
    r = r<least ? least : r;
    return r<TERM_DIMMAX ? r : TERM_DIMMAX;
}

// A screen's block, fresh or moved into from the one it had, with room
// for cols by rows. Returns 0, keeping what it had, if it cannot be had.
static b32 termscreenblock(Engine *e, Term *t, i32 cols, i32 rows)
{
    static char  name[] = "Prospero(" PROSPERO_VERSION ")";
    ariel_Config c      = {};
    c.cols      = cols;
    c.rows      = rows;
    c.capcols   = termscreencap(cols, 200);
    c.caprows   = termscreencap(rows, 80);
    c.histlines = t->hist ? t->hist : -1;  // ariel's 0 is its default
    c.histbytes = termscrollbytes(t->hist);
    c.flags     = ARIEL_F_NOREPLY;
    c.name.data = (u8 *)name;
    c.name.len  = countof(name) - 1;
    iz size = ariel_size(&c);
    if (!size) {
        return 0;
    }
    Region *r = os_reserve(e->os, size + ((iz)1<<16));
    if (!r) {
        return 0;
    }
    Arena a = newarena(r);
    allocbytes(&a, -(uz)a.beg & 15);
    void       *mem = allocbytes(&a, size);
    ariel_Term *st  = t->screen ? ariel_move(t->screen, mem, size, &c)
                                : ariel_init(mem, size, &c);
    if (!st) {
        os_release(e->os, r);
        return 0;
    }
    if (t->region) {
        os_release(e->os, t->region);
    }
    t->region  = r;
    t->screen  = st;
    t->capcols = c.capcols;
    t->caprows = c.caprows;
    return 1;
}

// The screen's text on the cursor's row, left of the cursor, into buf.
static Str termcursorrow(Term *t, u8 *buf, iz cap)
{
    ariel_Cursor cur = ariel_cursor(t->screen);
    if (cur.y < 0) {
        return {};
    }
    ariel_Cell cells[256];
    i32        n = ariel_row(t->screen, cur.y, cells, countof(cells));
    n = n<countof(cells) ? n : countof(cells);
    n = cur.x<n ? cur.x : n;
    iz len = 0;
    for (i32 x = 0; x<n && len+4<=cap; x++) {
        if (cells[x].width) {
            len += utf8encode(buf+len, cells[x].cp ? (i32)cells[x].cp : ' ');
        }
    }
    return {buf, len};
}

static void termfeedbytes(Term *t, Str data)
{
    ariel_Str in = {data.data, data.len};
    for (;;) {
        ariel_Fed f = ariel_feed(t->screen, in);
        in = f.rest;
        if (f.ev.kind == ARIEL_EV_NONE) {
            return;
        } else if (f.ev.kind==ARIEL_EV_TITLE && !f.ev.arg) {
            t->titlelen = termtitle(t->title, {f.ev.data.data, f.ev.data.len});
        }
    }
}

// Output to the screen: its title is the only event kept. While a line
// typed with echo on is fresh, a password prompt that shows meanwhile,
// at any line's end or where the output stops, makes it a secret: the
// program reads a line typed ahead of its prompt at once, too soon for
// echo off to be seen (termscan).
static void termfeedscreen(Engine *e, Term *t, Str data)
{
    if (!t->candlen || os_ticks(e->os)-t->candat>(i64)TERM_TYPEAHEAD*SEC) {
        termfeedbytes(t, data);
        return;
    }
    u8 row[1024];
    for (iz i = 0; t->candlen;) {
        iz j = i;
        for (; j<data.len && data[j]!='\r' && data[j]!='\n'; j++) {}
        termfeedbytes(t, Str{data.data+i, j-i});
        if (termprompt(termcursorrow(t, row, countof(row)))) {
            termsecretadd(&e->terms->secrets, {t->cand, t->candlen}, 2);
            t->candlen = 0;
        }
        if (j == data.len) {
            return;
        }
        termfeedbytes(t, Str{data.data+j, 1});
        i = j + 1;
        if (i == data.len) {
            return;
        }
        if (!t->candlen) {
            termfeedbytes(t, cuthead(data, i));
            return;
        }
    }
}

// Feed the screen what the ring holds past what it was fed, and look at
// how the terminal reads typing: echo going off soon after a line was
// typed with echo on makes that line a secret, typed ahead of the
// prompt (sudo keeps such a line, and reads it).
static void termscan(Engine *e, Term *t)
{
    if (!t->screen || !t->proc) {
        return;
    }
    for (;;) {
        Str s = os_ptypeek(e->os, t->proc, (iz)(t->fed - t->acked));
        if (!s.len) {
            break;
        }
        termfeedscreen(e, t, s);
        t->fed += s.len;
    }
    i32 mode = os_ptymode(e->os, t->proc);
    if (mode != t->mode) {
        i64 now = os_ticks(e->os);
        if (mode==PTY_SECRET && t->candlen && now-t->candat<=(i64)TERM_TYPEAHEAD*SEC) {
            termsecretadd(&e->terms->secrets, {t->cand, t->candlen}, 2);
        }
        if (mode == PTY_SECRET) {
            t->candlen = 0;
        }
        t->mode = mode;
    }
}

// Typed input, before it is written: the line it continues, and the
// secrets that line teaches once entered.
static void termtyped(Engine *e, Arena scratch, Term *t, Str data)
{
    if (!t->screen) {
        return;
    }
    termscan(e, t);
    i32 kind = TL_SKIP;  // of a line begun in this input
    u8 row[1024];
    if (t->mode==PTY_SECRET || termprompt(termcursorrow(t, row, countof(row)))) {
        kind = TL_SECRET;
    } else if (t->mode == PTY_ECHO) {
        kind = TL_CAND;
    }
    Slice<TermCommit> m = termlinefeed(&scratch, &t->line, data, kind);
    for (iz i = 0; i < m.len; i++) {
        if (m[i].kind == TL_SECRET) {
            termsecretadd(&e->terms->secrets, m[i].text, t->mode==PTY_SECRET ? 2 : 1);
        } else {
            t->candlen = (i32)m[i].text.len;
            t->candat  = os_ticks(e->os);
            copybytes(t->cand, m[i].text.data, m[i].text.len);
        }
    }
}

// The values of secret settings, as secrets to hide: each definition
// of any section of any file loaded, an env:NAME read from env (the
// terminal's) or else the engine's own, a file:PATH read. Short ones,
// which common words would match, are left out.
static void termconfsecrets(Engine *e, Arena scratch, Slice<Str> env, b32 win)
{
    for (IniSection *sec = e->cfg->ini->sections; sec; sec = sec->next) {
        for (IniEntry *k = sec->keys; k; k = k->next) {
            CfgKey *key = cfgkey(sec->kind, k->key);
            if (!key || key->type!=CFG_SECRET) {
                continue;
            }
            for (IniEntry *d = k; d; d = d->older) {
                Arena a = scratch;
                Str   v = trim(d->value);
                if (startswith(v, "env:")) {
                    Str name = trim(cuthead(v, 4));
                    Str got  = envget(env, name, win);
                    v = got.len ? got : envget(e->env, name, win);
                } else if (startswith(v, "file:")) {
                    Str path = cfgsecretfile(e->os, &a, v);
                    v = path.len ? cfgreadsecret(e->os, &a, path) : Str{};
                }
                if (v.len >= 8) {
                    termsecretadd(&e->terms->secrets, v, 1);
                }
            }
        }
    }
}

static void termhangup(Engine *e, Term *t, b32 kill)
{
    termscan(e, t);  // before the ring goes
    if (!t->closed) {
        t->closed = 1;
        if (kill) {
            os_kill(e->os, t->proc, 1);
        } else {
            os_letgo(e->os, t->proc);
        }
        os_procfree(e->os, t->proc);  // the hang-up: SIGHUP to the shell
        t->proc = 0;
    }
    if (t->exited && t->eof) {
        termfree(e, t);
    }
}


// Output

static b32 termemit(Engine *e, Str line)
{
    if (!mailroom(e->out, line.len)) {
        e->terms->held = 1;
        return 0;
    }
    emit(e, line);
    return 1;
}

static EvOut termbegin(Engine *e, Buf *b, Term *t, i32 ev)
{
    EvOut w = evbegin(b, {}, 0, os_time(e->os), ev);
    putintkey(&w.json, "tid", t->tid);
    if (t->peer) {
        putintkey(&w.json, "peer", t->peer);
    }
    return w;
}

// Send what the window and the outbox allow, then the end once all of
// it is sent. A terminal whose output is all acked after its end goes.
static void termpump(Engine *e, Arena scratch, Term *t)
{
    if (t->closed || !t->proc) {
        return;
    }
    termscan(e, t);  // what goes out is on the screen first
    while (!e->terms->held) {
        iz can = termcan(t->sent, t->acked, TERM_WINDOW, TERM_LINE);
        Str s  = can ? os_ptypeek(e->os, t->proc, (iz)(t->sent - t->acked)) : Str{};
        if (!s.len) {
            break;
        }
        s = s.len<can ? s : takehead(s, can);
        Arena a = scratch;
        Buf   b(&a, 256 + base64len(s.len));
        EvOut w = termbegin(e, &b, t, EV_TERM);
        putintkey(&w.json, "off", t->sent);
        putstrkey(&w.json, "data", base64(&a, s));
        if (!termemit(e, evend(&w))) {
            break;
        }
        t->sent    += s.len;
        if (!t->away && !t->stallat) {
            t->stallat = os_ticks(e->os) + (i64)TERM_STALL*SEC;
        }
    }
    if (t->exited && t->eof && !t->ended && t->sent==t->total && !e->terms->held) {
        Buf   b(&scratch, 256);
        EvOut w = termbegin(e, &b, t, EV_TERMEND);
        putintkey(&w.json, "off", t->total);
        putintkey(&w.json, "code", t->code);
        putintkey(&w.json, "signal", t->signal);
        t->ended = termemit(e, evend(&w));
    }
    if (t->ended && t->acked==t->total) {
        termhangup(e, t, 0);  // nothing left to hang up, but its reaper
    }
}

// The frontend stopped acking: send again from what it acked, or after
// the last try, give up on it, as stalled.
static void termstall(Engine *e, Arena scratch, Term *t)
{
    if (++t->tries <= TERM_TRIES) {
        t->sent    = t->acked;
        t->stallat = os_ticks(e->os) + (i64)TERM_STALL*SEC;
        termpump(e, scratch, t);
        return;
    }
    Buf   b(&scratch, 256);
    EvOut w = termbegin(e, &b, t, EV_TERMEND);
    putintkey(&w.json, "off", t->acked);
    putintkey(&w.json, "code", -1);
    putintkey(&w.json, "signal", 0);
    putstrkey(&w.json, "err", "stalled");
    emit(e, evend(&w));  // once, so whatever the room
    termhangup(e, t, 0);
}


// Commands

static PtySize termsize(Json *j)
{
    PtySize z = {};
    z.cols = (i32)jsonint(jsonget(j, "cols"), 80);
    z.rows = (i32)jsonint(jsonget(j, "rows"), 24);
    z.xpx  = (i32)jsonint(jsonget(j, "xpx"), 0);
    z.ypx  = (i32)jsonint(jsonget(j, "ypx"), 0);
    z.cols = z.cols<1 ? 1 : z.cols>TERM_DIMMAX ? TERM_DIMMAX : z.cols;
    z.rows = z.rows<1 ? 1 : z.rows>TERM_DIMMAX ? TERM_DIMMAX : z.rows;
    z.xpx  = z.xpx<0 ? 0 : z.xpx>0xffff ? 0xffff : z.xpx;
    z.ypx  = z.ypx<0 ? 0 : z.ypx>0xffff ? 0xffff : z.ypx;
    return z;
}

// TERM's value: xterm-256color where the system has its terminfo, as
// most do, else plain xterm, as some container images have only that.
static Str termname(Engine *e, Arena scratch, b32 win)
{
    if (win) {
        return "xterm-256color";
    }
    Str dirs[] = {
        "/usr/share/terminfo", "/lib/terminfo", "/etc/terminfo",
        "/usr/lib/terminfo", "/usr/share/lib/terminfo",
    };
    Str subs[] = {"x/xterm-256color", "78/xterm-256color"};
    for (iz i = 0; i < countof(dirs); i++) {
        for (iz k = 0; k < countof(subs); k++) {
            Str path = join(&scratch, dirs[i], subs[k]);
            if (os_stat(e->os, scratch, path).type != STAT_MISSING) {
                return "xterm-256color";
            }
        }
    }
    Str home = e->home;
    for (iz k = 0; home.len && k < countof(subs); k++) {
        Str p = join(&scratch, join(&scratch, home, ".terminfo"), subs[k]);
        if (os_stat(e->os, scratch, p).type != STAT_MISSING) {
            return "xterm-256color";
        }
    }
    return "xterm";
}

// Where editpick looks for an editor's program on Windows (findprog).
struct TermFind {
    Os *os;
    Str path;
    Str pathext;
};

static i32 termfound(void *ctx, Arena *a, Str prog)
{
    TermFind *f    = (TermFind *)ctx;
    Arena     perm = *a;
    Arena     tmp  = carve(&perm, 1<<20);
    Str       exe  = findprog(f->os, &perm, tmp, f->path, prog, f->pathext, 1);
    return exe.len ? EDITOR_FOUND : EDITOR_MISSING;
}

// The editor's command line on Windows, an edit's, by cmd.exe in env:
// the first of the window's editor, %VISUAL%, %EDITOR% and vi found on
// PATH (editpick), its line (editorjump) with the program by its full
// path (cmdabs), *argv and *name set. Null and *why if there is none.
static Str termeditwin(Engine *e, Arena *a, TermEditAsk *ed, Slice<Str> env,
                       Slice<Str> *argv, Str *name, Str *why)
{
    // cmd.exe takes no VAR=value words: a line's program is its first
    // word (editorjump), so one that starts with an assignment is
    // refused if asked for, else passed over
    Str asks[] = {ed->editor, ed->tmpl};
    for (iz i = 0; i < countof(asks); i++) {
        Str rest = asks[i];
        if (cmdassign(cmdword(&rest))) {
            *why = "edit: cmd.exe takes no VAR=value words ahead of the editor";
            return {};
        }
    }
    Str home   = e->home;
    Str envs[] = {envget(env, "VISUAL", 1), envget(env, "EDITOR", 1)};
    for (iz i = 0; i < countof(envs); i++) {
        Str rest = envs[i];
        envs[i]  = cmdassign(cmdword(&rest)) ? Str{} : cmdhome(a, envs[i], home);
    }
    TermFind f    = {e->os, envpath(env, 1), envget(env, "PATHEXT", 1)};
    EditPick pick = editpick(a, cmdhome(a, ed->editor, home), cmdhome(a, ed->tmpl, home),
                             envs[0], envs[1], termfound, &f);
    Str use = pick.tmpl.len ? pick.tmpl : pick.cmd;
    if (!use.len) {
        Str asked = cmdprog(ed->tmpl.len ? ed->tmpl : ed->editor, 0);
        if (asked.len) {
            Buf b(a, 96 + asked.len);
            print(&b, "no editor found here: ");
            print(&b, basename(asked));
            print(&b, " is not on PATH, nor are VISUAL, EDITOR or vi");
            *why = finish(&b);
        } else {
            *why = "no editor found here: VISUAL, EDITOR and vi are not on PATH";
        }
        return {};
    }
    Str  rest  = use;
    Str  prog  = cmdword(&rest);  // as editorjump takes it
    Str  exe   = findprog(e->os, a, carve(a, 1<<20), f.path, prog, f.pathext, 1);
    Str  file  = clone(a, ed->path);
    for (iz i = 0; i < file.len; i++) {
        file.data[i] = file[i]=='/' ? '\\' : file[i];
    }
    EditJump j = editorjump(a, pick.cmd, pick.tmpl, file, ed->line>0 ? ed->line : 1,
                            ed->col, 1);
    if (j.why.len) {
        *why = j.why;
        return {};
    }
    Str line = cmdabs(a, j.line, exe);
    if (!line.len) {
        *why = "cannot start the editor: its path holds %, \", a control character "
               "or another character cmd.exe cannot pass on";
        return {};
    }
    // cmd.exe itself by a full path, as CreateProcess would try the
    // current folder for a bare name
    Str comspec = trim(envget(env, "COMSPEC", 1));
    if (!pathroot(comspec, 1)) {
        comspec = findprog(e->os, a, carve(a, 1<<20), f.path, "cmd.exe", {}, 1);
    }
    if (!comspec.len) {
        *why = "cannot start the editor: cmd.exe is not on PATH";
        return {};
    }
    *argv = {};
    *argv = push(a, *argv, comspec);
    *name = termeditname(prog);
    return cmdcline(a, comspec, line);
}

// Start a terminal's shell, or an edit's editor, and reply with its tid,
// before its output.
static Str termstart(Engine *e, Arena scratch, i64 id, Json *j, Session *s, Str sid)
{
    i64   peer = jsonint(jsonget(j, "peer"), 0);
    Term *t    = 0;
    for (i32 i = 0; !t && i < TERM_MAX; i++) {
        t = e->terms->slots[i].tid ? 0 : &e->terms->slots[i];
    }
    if (!t || termcount(e, peer)>=TERM_PERPEER) {
        return "too many terminals are open";
    }
    b32         win = s->te.windows;
    TermEditAsk ed  = termeditask(j, win);
    if (ed.why.len) {
        return ed.why;
    } else if (ed.on && win!=HOST_WINDOWS) {
        // A container's kept subagent on Windows: Linux's paths, in a
        // folder gone, with no editor here to take them
        return "edit: this session's files are not on this machine";
    }

    // In its folder, or the one asked for if it is one
    Str cwd = jsonstr(jsonget(j, "cwd"));
    cwd = cwd.len && os_stat(e->os, scratch, cwd).type==STAT_DIR ? cwd : s->cwd;

    // A login shell runs the profile itself, so it starts from what
    // this process started with, as a terminal's would: not the login
    // environment a desktop-started window took (macOS), whose EDITOR
    // would start zsh in vi mode, its PATH entries doubled
    Shell      sh    = s->te.shell;
    b32        login = !(PTY_LINUX && e->gui);
    Slice<Str> env   = login && !win ? os_launchenv(e->os, &scratch) : e->env;
    Str        path  = cfgsearchpath(e->cfg, &scratch, envpath(env, win), e->home, win);
    env = envsetpath(&scratch, env, path, win);
    env = shellover(s, &scratch, &sh, env, 1);
    Slice<Str> argv    = {};
    Str        cmdline = {};
    Str        editor  = {};  // the reply's, on Windows
    if (ed.on && win) {
        Str why = {};
        cmdline = termeditwin(e, &scratch, &ed, env, &argv, &editor, &why);
        if (!cmdline.len) {
            return why;
        }
    } else if (ed.on) {
        // This program again, which becomes the login shell and then the
        // editor (main_posix.cpp): on Linux the very file this runs,
        // even once a rebuild replaced it
        Str self = PTY_LINUX ? Str("/proc/self/exe") : e->te.exe;
        if (!self.len) {
            return "cannot start the editor: this program's path is not known";
        }
        argv = push(&scratch, argv, self);
        argv = push(&scratch, argv, Str("--tty-edit"));
    } else {
        Str conf = trim(cfgget(e->cfg, "terminal", {}, "shell", 0));
        argv = termargv(&scratch, conf, e->home, shellkinds[sh.kind], sh.path, sh.distro,
                        envget(env, "SHELL", win), win, login);
        if (!argv.len) {
            return "[terminal] shell: a quote is left open";
        }
    }
    Str term = termname(e, scratch, win);  // a literal
    env = termenv(&scratch, env, win, PTY_MAC, term);
    if (ed.on && !win) {
        // Never quoted: no shell parses these
        Str names[] = {
            "PROSPERO_EDIT_FILE", "PROSPERO_EDIT_LINE", "PROSPERO_EDIT_COL",
            "PROSPERO_EDIT_EDITOR", "PROSPERO_EDIT_TEMPLATE", "PROSPERO_EDIT_LOGIN",
        };
        for (iz i = 0; i < countof(names); i++) {
            env = envdel(&scratch, env, names[i], 0);
        }
        env = envset(&scratch, env, names[0], ed.path, 0);
        if (ed.line) {
            env = envset(&scratch, env, names[1], deskint(&scratch, ed.line), 0);
        }
        if (ed.col) {
            env = envset(&scratch, env, names[2], deskint(&scratch, ed.col), 0);
        }
        if (ed.editor.len) {
            env = envset(&scratch, env, names[3], ed.editor, 0);
        }
        if (ed.tmpl.len) {
            env = envset(&scratch, env, names[4], ed.tmpl, 0);
        }
        env = envset(&scratch, env, names[5], login ? Str("1") : Str("0"), 0);
    }

    Spawn sp   = {};
    sp.argv    = argv;
    sp.cmdline = cmdline;
    sp.cwd     = cwd;
    sp.env     = env;
    sp.mbox    = e->in;
    sp.id      = ++e->nextid;
    Proc *p    = os_ptyspawn(e->os, &sp, termsize(j), TERM_RING, scratch);
    if (!p) {
        Str what = ed.on ? Str("cannot start the editor: ") : Str("cannot start the shell: ");
        return concat(&scratch, what, os_lasterr(e->os, &scratch));
    }
    *t = {};
    t->tid    = ++e->terms->nexttid;
    t->peer   = peer;
    t->sidlen = sid.len;
    copybytes(t->sid, sid.data, sid.len);
    t->proc   = p;
    t->procid = sp.id;
    t->used   = os_ticks(e->os);
    Str label = ed.on && editor.len ? editor : sh.name;
    t->shelllen = label.len<TERM_SHELLCAP ? label.len : 0;
    copybytes(t->shell, label.data, t->shelllen);
    t->cwdlen = cwd.len<TERM_CWDCAP ? cwd.len : 0;
    copybytes(t->cwd, cwd.data, t->cwdlen);
    if (termagent(e)) {
        termconfsecrets(e, scratch, env, win);
        // As much history as the frontend keeps, within TERM_HIST
        PtySize z  = termsize(j);
        i64     sb = jsonint(jsonget(j, "scrollback"), 1000);
        t->hist = (i32)(sb<0 ? 0 : sb>TERM_HIST ? (i64)TERM_HIST : sb);
        termscreenblock(e, t, z.cols, z.rows);  // else no screen, no reads
    }

    Buf   b(&scratch, 1024);
    EvOut w = evbegin(&b, sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putintkey(&w.json, "tid", t->tid);
    putstrkey(&w.json, "shell", sh.name);
    putkey(&w.json, "argv");
    putarray(&w.json);
    for (iz i = 0; i < argv.len; i++) {
        putstr(&w.json, argv[i]);
    }
    putend(&w.json);
    putstrkey(&w.json, "cwd", cwd);
    if (editor.len) {
        putstrkey(&w.json, "editor", editor);
    }
    emit(e, evend(&w));
    return {};
}

static void termopen(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str sid = jsonstr(jsonget(j, "sid"));
    if (!validsid(sid)) {
        reply(e, scratch, id, 0, "invalid session id", {});
        return;
    } else if (!os_ptyable(e->os)) {
        reply(e, scratch, id, 0, "terminals need Windows 10 version 1809 or later", sid);
        return;
    } else if (termcount(e, -1) >= TERM_MAX) {
        reply(e, scratch, id, 0, "too many terminals are open", sid);
        return;
    }
    b32      transient = 0;
    Session *s         = borrow(e, scratch, sid, &transient);
    if (!s) {
        reply(e, scratch, id, 0, "no such session", sid);
        return;
    }
    Str err = termstart(e, scratch, id, j, s, sid);
    if (err.len) {
        err = clone(&scratch, err);
    }
    if (transient) {
        forget(e, s);
    }
    if (err.len) {
        reply(e, scratch, id, 0, err, sid);
    }
}

// A command of a terminal's (term.input, term.resize, term.ack and
// term.close), none of them with a reply: one naming no terminal of the
// frontend's is dropped, as after its end.
static void termcmd(Engine *e, Arena scratch, i32 cmd, Json *j)
{
    i64 peer = jsonint(jsonget(j, "peer"), 0);
    if (cmd==CMD_TERMCLOSE && jsonflag(jsonget(j, "all"), 0)) {
        termhangupall(e, peer<0 ? 0 : peer, {});  // a frontend's every one
        return;
    }
    Term *t = termbytid(e, jsonint(jsonget(j, "tid"), 0), peer);
    if (!t || t->closed) {
        return;
    }
    switch (cmd) {
    case CMD_TERMINPUT: {
        Unbase64 u = unbase64(&scratch, jsonstr(jsonget(j, "data")));
        if (u.ok && u.data.len && u.data.len<=TERM_INPUT) {
            termtyped(e, scratch, t, u.data);
            t->used = os_ticks(e->os);
            os_procwrite(e->os, t->proc, u.data);
        }
    } break;
    case CMD_TERMRESIZE: {
        // The screen takes the output written at the old size first
        PtySize z = termsize(j);
        if (t->screen) {
            termscan(e, t);
            if (z.cols>t->capcols || z.rows>t->caprows) {
                termscreenblock(e, t, z.cols, z.rows);
            }
            ariel_resize(t->screen, z.cols, z.rows, z.xpx/z.cols, z.ypx/z.rows);
        }
        os_ptyresize(e->os, t->proc, z);
    } break;
    case CMD_TERMACK: {
        i64     off = jsonint(jsonget(j, "off"), -1);
        TermAck k   = termack(t->sent, t->acked, off, jsonflag(jsonget(j, "resend"), 0));
        if (!k.ok) {
            break;
        }
        os_ptytake(e->os, t->proc, (iz)k.take);
        t->acked   = off;
        t->sent    = k.sent;
        t->tries   = k.take ? 0 : t->tries;
        t->stallat = t->sent>t->acked && !t->away ? os_ticks(e->os) + (i64)TERM_STALL*SEC : 0;
        termpump(e, scratch, t);
    } break;
    case CMD_TERMCLOSE:
        termhangup(e, t, jsonflag(jsonget(j, "kill"), 0));
        break;
    }
}


// Hooks: records, the loop, and what hangs terminals up

// A record of a terminal's shell. Returns 0 if it is not one.
static b32 termrecord(Engine *e, Arena scratch, Record r)
{
    b32   proc = r.type==PROC_READY || r.type==PROC_EXIT || r.type==PROC_EOF ||
                 r.type==PROC_TAIL || r.type==PROC_LIMIT || r.type==PROC_DATA;
    Term *t    = proc ? termbyproc(e, r.id) : 0;
    if (!t) {
        return 0;
    }
    switch (r.type) {
    case PROC_READY:
        break;
    case PROC_EXIT:
        t->exited = 1;
        t->code   = get32(r.data);
        t->signal = get32(cuthead(r.data, 4));
        break;
    case PROC_EOF:
        t->eof   = 1;
        t->total = get64(r.data);
        break;
    default:
        return 1;
    }
    if (t->closed) {
        termhangup(e, t, 0);  // frees it once both are in
    } else {
        termpump(e, scratch, t);
    }
    return 1;
}

// Each pass: output that waited for room, stalls, and who has the
// terminal, as often as TERM_BUSYMS.
static void termtick(Engine *e, Arena scratch)
{
    if (!termcount(e, -1)) {
        return;
    }
    e->terms->held = 0;
    i64 now  = os_ticks(e->os);
    b32 look = now >= e->terms->busyat;
    for (i32 i = 0; i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (!t->tid || t->closed) {
            continue;
        }
        if (t->stallat && now>=t->stallat) {
            termstall(e, scratch, t);
            continue;
        }
        termpump(e, scratch, t);
        if (look && t->proc && !t->exited) {
            b32 busy = os_ptybusy(e->os, t->proc);
            if (busy != t->busy) {
                Arena a = scratch;
                Buf   b(&a, 256);
                EvOut w = termbegin(e, &b, t, EV_TERMBUSY);
                putintkey(&w.json, "busy", busy);
                if (termemit(e, evend(&w))) {
                    t->busy = busy;
                }
            }
        }
    }
    if (look) {
        e->terms->busyat = now + (i64)TERM_BUSYMS*MS;
    }
}

// When termtick has work: soon while output waits for room, else at the
// next stall or look.
static i64 termnext(Engine *e, i64 now)
{
    if (!termcount(e, -1)) {
        return I64MAX;
    } else if (e->terms->held) {
        return now + (i64)TERM_RETRY*MS;
    }
    i64 next = e->terms->busyat;
    for (i32 i = 0; i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (t->tid && !t->closed && t->stallat && t->stallat<next) {
            next = t->stallat;
        }
    }
    return next;
}

// A frontend's link went down (away), or came back: its terminals stop
// counting stalls while it is away, and what it sends meanwhile waits
// in its host's link; back, they count from now, and a gap the link
// left (frames lost) is filled once the frontend finds it or stalls.
static void termaway(Engine *e, i64 peer, b32 away)
{
    i64 now = os_ticks(e->os);
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (!t->tid || t->peer!=peer || t->closed) {
            continue;
        }
        t->away    = away;
        t->tries   = 0;
        t->stallat = !away && t->sent>t->acked ? now + (i64)TERM_STALL*SEC : 0;
    }
}

// Hang up every terminal (a quit), a frontend's (peer, gone for good),
// or a session's (sid, deleted or trashed).
static void termhangupall(Engine *e, i64 peer, Str sid)
{
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        b32 match = peer<0 ? !sid.len || Str{t->sid, t->sidlen}==sid : t->peer==peer;
        if (t->tid && match) {
            termhangup(e, t, 0);
        }
    }
}


// read_terminal: the agent reads the user's terminals

enum : i32 {
    TERM_READBYTES = 50 << 10,  // of a terminal's text shown at once
    TERM_READLINES = 100,       // lines shown by default
};

// A session as termrootwalk sees it: from memory, else from the first
// record of its log.
static TermNode termlookup(void *ctx, Arena *a, Str sid)
{
    Engine  *e = (Engine *)ctx;
    TermNode n = {};
    Session *s = findsession(e, sid);
    if (s) {
        n.found    = 1;
        n.parent   = s->psid;
        n.sideof   = s->wt.from;
        n.worktree = s->wt.name.len > 0;
        n.cwd      = s->cwd;
        return n;
    } else if (!validsid(sid)) {
        return n;
    }
    Str   path = join(a, join(a, join(a, e->statedir, "sessions"), sid), "events.jsonl");
    Read  rd   = os_readat(e->os, a, path, 0, 4096);
    Cut   line = cut(rd.data, '\n');
    EvRec rec  = evparse(a, line.head);
    if (!line.ok || rec.ev!=EV_SESSION) {
        return n;
    }
    n.found    = 1;
    n.parent   = jsonint(jsonget(rec.root, "remote"), 0)==1 ? Str{}
                                                             : jsonstr(jsonget(rec.root, "parent"));
    n.sideof   = jsonstr(jsonget(rec.root, "side_of"));
    n.worktree = wtfrom(a, jsonget(rec.root, "worktree")).name.len > 0;
    n.cwd      = jsonstr(jsonget(rec.root, "cwd"));
    return n;
}

// A terminal's line in the list: its number, label, state and folder.
static void termlistline(Buf *b, Arena *a, Term *t, TermSecrets *ss, iz *hidden)
{
    iz  h     = 0;
    Str label = t->titlelen ? termmask(a, {t->title, t->titlelen}, ss, &h)
                            : Str{t->shell, t->shelllen};
    *hidden += h;
    print(b, "terminal ");
    print(b, t->tid);
    print(b, ": ");
    print(b, label.len ? label : Str("shell"));
    if (t->exited && t->signal) {
        print(b, ", killed by signal ");
        print(b, (i64)t->signal);
    } else if (t->exited) {
        print(b, ", exited with code ");
        print(b, (i64)t->code);
    } else if (t->busy) {
        print(b, ", a program is running");
    } else {
        print(b, ", at its prompt");
    }
    if (t->cwdlen) {
        print(b, ", in ");
        print(b, Str{t->cwd, t->cwdlen});
    }
    if (!t->screen) {
        print(b, " (not readable: it opened while [terminal] agent was off)");
    }
    putbyte(b, '\n');
}

// The last n lines of text, each cut at READ_LINEMAX bytes, and no more
// of them than fit in max bytes, the newest kept. *more says lines were
// left out before them.
static Str termlast(Arena *a, Str text, i64 n, iz max, b32 *more)
{
    Slice<Str> lines = {};
    for (Cut c = {{}, text, text.len>0}; c.ok;) {
        c     = cut(c.tail, '\n');
        lines = push(a, lines, c.head.len>READ_LINEMAX ? utf8head(c.head, READ_LINEMAX)
                                                       : c.head);
    }
    iz first = lines.len>n ? lines.len-(iz)n : 0;
    iz total = 0;
    iz from  = lines.len;
    while (from > first && total+lines[from-1].len+1 <= max) {
        total += lines[--from].len + 1;
    }
    *more = from > 0;
    Buf b(a, total + 1);
    for (iz i = from; i < lines.len; i++) {
        print(&b, lines[i]);
        putbyte(&b, '\n');
    }
    return finish(&b);
}

static void toolreadterm(Session *s, Arena scratch, Args *args)
{
    Engine *e = s->e;
    if (!termagent(e)) {
        toolend(s, scratch, "error", "Reading terminals is turned off ([terminal] "
                "agent), so nothing was read.", 0, 0, {}, 0);
        return;
    }

    // The session's terminals, as the frontend files them, newest first
    Str   root = termrootwalk(&scratch, s->sid, termlookup, e);
    Term *list[TERM_MAX];
    i32   n = 0;
    for (i32 i = 0; i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (t->tid && !t->closed && Str{t->sid, t->sidlen}==root) {
            i32 k = n++;
            for (; k>0 && list[k-1]->tid<t->tid; k--) {
                list[k] = list[k-1];
            }
            list[k] = t;
        }
    }
    if (!n) {
        toolend(s, scratch, "ok", "The user has no Terminal tabs open for this "
                "session.", 0, 0, {}, 0);
        s->termseen = 0;
        return;
    }

    // The one asked for, else the one opened or typed into last
    Term *pick = 0;
    for (i32 i = 0; i < n; i++) {
        if (args->tab ? list[i]->tid==args->tab : !pick || list[i]->used>pick->used) {
            pick = list[i];
        }
    }

    TermSecrets *ss     = &e->terms->secrets;
    iz           hidden = 0;
    Buf          b(&scratch, 1024);
    print(&b, n==1 ? Str("The session's terminal:\n") : Str("The session's terminals, newest first:\n"));
    for (i32 i = 0; i < n; i++) {
        termlistline(&b, &scratch, list[i], ss, &hidden);
    }
    if (!pick) {
        print(&b, "\nNo terminal ");
        print(&b, args->tab);
        print(&b, " is among them.");
        Str text = finish(&b);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return;
    }

    putbyte(&b, '\n');
    if (!pick->screen) {
        print(&b, "Terminal ");
        print(&b, pick->tid);
        print(&b, " cannot be read: it opened while [terminal] agent was off.\n");
    } else {
        // Catch up, take the whole text, wrapped rows joined, then hide
        // the secrets before it is cut, so that none is cut in two
        termscan(e, pick);
        ariel_select(pick->screen, ARIEL_SEL_ALL, 0, 0);
        iz  len  = ariel_copy(pick->screen, 0, 0);
        u8 *raw  = len>0 ? alloc<u8>(&scratch, len) : 0;
        len      = len>0 ? ariel_copy(pick->screen, raw, len) : 0;
        ariel_select(pick->screen, ARIEL_SEL_CLEAR, 0, 0);
        iz  h    = 0;
        Str text = termmask(&scratch, trimright(Str{raw, len}), ss, &h);
        hidden  += h;
        b32 more = 0;
        Str last = termlast(&scratch, text, args->lines ? args->lines : (i64)TERM_READLINES,
                            TERM_READBYTES, &more);
        b32 alt  = ariel_modes(pick->screen).altscreen;
        print(&b, "Terminal ");
        print(&b, pick->tid);
        if (!text.len) {
            print(&b, " shows nothing yet.\n");
        } else {
            print(&b, more ? Str(", its last lines:\n") : Str(", all of it:\n"));
            print(&b, last);
        }
        if (alt) {
            print(&b, "[a full-screen program has the terminal: this is its "
                      "screen, without the history above it]\n");
        }
        if (pick->readfed && pick->fed==pick->readfed) {
            print(&b, "[no new output since you last read it]\n");
        }
        pick->readfed = pick->fed;
    }
    if (hidden) {
        print(&b, "[");
        print(&b, (i64)hidden);
        print(&b, hidden==1 ? Str(" secret is") : Str(" secrets are"));
        print(&b, " hidden as [hidden]: typed at password prompts, or "
                  "settings' secret values]\n");
    }
    Str text = finish(&b);
    e->terms->lastread = pick->tid;
    s->termseen = hash(text);
    toolend(s, scratch, "ok", text, 0, 0, {}, 0);
}
