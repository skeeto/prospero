// Terminals: the shells of the Terminal pane's tabs, on this machine
//
// A frontend's terminal tab is a shell that this engine runs on a
// pseudo-terminal (os_ptyspawn), in the session's folder and with its
// shell's settings, while the frontend emulates the terminal (ariel).
// The engine is only a flow-controlled pipe of bytes between the two:
//
//   term.open {sid, cols, rows, xpx?, ypx?, cwd?, scrollback?,
//              edit? | run? | shell?}
//       replies {tid, shell, kind, nonce?, argv, cwd, editor?} before
//       any output
//   term.input {tid, data, run?}      typed bytes, base64, 16 KiB at most
//                                     (TERM_RUNINPUT with run)
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
// A profile can test PROSPERO_TAB, edit while it runs (or
// PROSPERO_EDIT_FILE), to skip what would keep an editor from starting
// (exec tmux).
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
// A code block of the agent's run for the user (cap term.run, the Run
// button), in place of the shell:
//
//   run {text, lang, from?}
//
// lang is the block's (termrunlang: sh, bash, zsh, ksh, powershell,
// pwsh, cmd and their aliases), text what runs (termruntext: at most 16
// KiB, nothing the window would draw invisibly, cmd's one line and only
// on Windows), from the session told of its end. Being the agent's, it
// runs without the variables secret settings come from (shellenv's
// rule), and with PROSPERO_TAB=run. On POSIX this program runs again as
// "--tty-run" (main_posix.cpp), the request in PROSPERO_RUN_TEXT, _LANG,
// _SHELL (the terminal's shell) and _LOGIN: it shows the text, dimmed,
// then a POSIX sh runs it as an interactive shell would (-i -c, its rc
// files read, as for a paste), and another shell (fish) runs phase two,
// "--tty-run-in", the block's own shell if here, else /bin/sh, while
// PowerShell's runs in pwsh by psrunboot. On Windows cmd.exe runs the
// line by /s /c with NoDefaultCurrentDirectoryInExePath set, PowerShell
// (the session's, else pwsh, else powershell.exe) by psrunhead, and sh
// in the session's sh or busybox.
//
// A tab's shell, unless edit or run, takes hooks where it is zsh or bash
// on POSIX (alone or with -l) or PowerShell (alone or with -NoLogo), and
// [terminal] integration is on: scripts from assets/terminal, written
// once to <state>/terminal/hooks/HASH (termhookdir), which run the
// shell's usual startup files and then print OSC 133 marks with the
// tab's nonce (termmarkparse), given in PROSPERO_TERM_NONCE, which they
// unset first. zsh reads them by ZDOTDIR (the user's own, if any, in
// PROSPERO_USER_ZDOTDIR), bash by --init-file (PROSPERO_TERM_LOGIN=1 for
// -l, whose files it runs itself), and PowerShell runs psmarkboot with
// -NoProfile, hooks.ps1 named by PROSPERO_TERM_HOOKS. The reply's nonce
// is the tab's, if hooked, and its kind the program's family (termkind:
// sh, ps, cmd, else empty). With shell, a code block's language
// (termrunlang), the tab runs an interactive shell of its family
// (termrunpick), and the reply's shell names that program.
//
// A Run (the Run button) is typed at a hooked shell's prompt, a paste
// and Enter in one term.input whose run {from, key, first, lines} names
// the session told of its end, the block's hash in 16 hex digits, its
// first line and how many it has. It is tracked (PromptRun) if the tab's
// screen is kept ([terminal] agent), its last mark was A, and no other
// Run of the tab's still goes, else it is only typing: running from its
// C, its own screen fed from there; ended at D (or A); over TRUN_SETTLE
// seconds after D, or TRUN_PROMPT after the prompt's A, or at once when
// a line is typed, before the session's next request, or when its tab
// closes or shell ends. A C soon after its end, nothing typed, is more
// of it: a shell without bracketed paste runs each line on its own. Its
// notice "terminal" (termrunend) says how it ended and what it printed.
//
// A terminal that ends, its tab still open, keeps its slot and screen
// (kept) for read_terminal, until term.close, a hang-up of all, or a
// term.open finding no free slot, which takes the kept one unused
// longest. One hung up while it ran keeps nothing. A Run that ends, or
// whose tab closed while it ran, leaves a notice "terminal" for from
// (termnotice): how it ended and its screen's last lines, masked, at
// that session's next request, never starting a turn.
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
    TERM_RUNTEXT  = 16 << 10,   // a Run's text, in bytes, at most
    TERM_RUNCMD   = 8191,       // a cmd line's UTF-16 units: cmd.exe's most
    TERM_RUNLINE  = 120,        // bytes of its first line kept, for labels
    TERM_RUNINPUT = TERM_RUNTEXT + 64,  // a Run typed: its text, pasted
    TERM_RUNHIST  = 500,        // lines a Run's own screen keeps
};

// A Run typed at a hooked shell's prompt (term.input's run), as its
// marks tell it: armed once sent, running from its C, ended at its D
// (or an A with none), and over once it settles, when the next
// request, the tab's end, or another Run would not wait for it.
enum : i32 {
    TRUN_NONE,
    TRUN_ARMED,    // sent at a prompt, not yet begun
    TRUN_RUNNING,
    TRUN_ENDED,    // over at settleat, unless more of it runs first
};

enum : i32 {
    TRUN_MARK,     // a verified mark
    TRUN_TYPED,    // a line typed (CR or LF) other than the Run's
};

enum : i32 {
    TRUN_SETTLE  = 2,  // seconds after D for more of a block to run
    TRUN_PROMPT  = 1,  // or after the prompt's A, if sooner
};

struct PromptRun {
    i32 state;
    b32 hascode;   // its status, if the shell gave it
    i32 code;
    b32 unrun;     // a prompt came back with nothing run (bare D or A)
    b32 typed;     // a line typed since it ended: what runs is not it
    i64 armedat;   // ticks
    i64 startat;   // its first C, else when it ended
    i64 endat;
    i64 settleat;
};

// How a Run ended, as its notice tells (termrunend): as its marks said,
// or not knowing how.
enum : i32 {
    TEND_DONE,
    TEND_CUT,    // its tab closed
    TEND_EXIT,   // its shell ended
};

// A screen of the engine's, in a block of its own.
struct TermScreen {
    Region     *region;
    ariel_Term *screen;
    i32         capcols;
    i32         caprows;
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
    TermScreen  scr;
    ariel_Term *screen;   // scr's
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
    u8          nonce[32];  // its hooks' marks', when hooked
    b32         hooked;
    b32         win;        // its shell is on Windows: OSC 7's paths are
    i32         lastmark;   // the last verified mark's letter but B, 0 for none
    TermCmds    cmds;       // its command lines, while its screen is kept

    // A session waiting for its running command to end (read_terminal's
    // wait): that record, by seq, or with an unhooked shell, until no
    // program but the shell has the terminal (busy)
    u8  waitsid[64];
    iz  waitsidlen;  // 0 for none
    i64 waitseq;     // 0 for busy
    i64 waitat;      // ticks

    // A Run's (term.open's run): what ran, for labels and its notice
    b32 run;
    b32 keep;       // hung up, its screen kept once it ends
    b32 kept;       // ended, its screen kept for read_terminal until
                    // term.close, eviction, or a hang-up of all
    b32 told;       // its end notice went
    b32 cut;        // hung up while it ran
    i64 startat;    // ticks
    u64 runkey;     // its text's hash: a newer run's notice replaces it
    u8  runline[TERM_RUNLINE];
    iz  runlinelen;
    i32 runlines;
    u8  from[64];   // the session its notice goes to
    iz  fromlen;

    // A Run typed at the shell's prompt (term.input's run), with the
    // run fields above but run, and its own screen, fed from its C on,
    // so that its notice holds only what it printed
    PromptRun     srun;
    TermScreen  rscreen;
    i32         cols;  // the terminal's size, for a Run's screen
    i32         rows;
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

// Runs: a code block of the agent's run in a terminal (cap term.run)

enum : i32 {
    RUN_NONE,   // not a shell's command: no Run
    RUN_SH,     // a POSIX sh's
    RUN_PS,     // PowerShell's
    RUN_CMD,    // cmd.exe's, one line, on Windows
};

// A fence's language as a Run's kind, case-insensitive: RUN_NONE for
// any other, and for console and shellsession, which name transcripts.
static i32 termrunlang(Str lang)
{
    static struct {
        Str name;
        i32 kind;
    } langs[] = {
        {"sh", RUN_SH}, {"shell", RUN_SH}, {"bash", RUN_SH}, {"zsh", RUN_SH},
        {"ksh", RUN_SH}, {"powershell", RUN_PS}, {"pwsh", RUN_PS}, {"ps1", RUN_PS},
        {"cmd", RUN_CMD}, {"bat", RUN_CMD}, {"batch", RUN_CMD},
    };
    lang = trim(lang);
    for (iz i = 0; i < countof(langs); i++) {
        if (equalfold(lang, langs[i].name)) {
            return langs[i].kind;
        }
    }
    return RUN_NONE;
}

// Whether a code point shows as nothing, or as other than it is, in the
// window: a control but TAB and LF, DEL, C1, a format character (bidi
// controls, zero widths, joiners, the BOM, tags), or a line or paragraph
// separator. What runs must be what shows.
static b32 termruninvisible(i32 c)
{
    static i32 const ranges[][2] = {
        {0x00ad, 0x00ad}, {0x0600, 0x0605}, {0x061c, 0x061c}, {0x06dd, 0x06dd},
        {0x070f, 0x070f}, {0x08e2, 0x08e2}, {0x180e, 0x180e}, {0x200b, 0x200f},
        {0x2028, 0x202e}, {0x2060, 0x2064}, {0x2066, 0x206f}, {0xfeff, 0xfeff},
        {0xfff9, 0xfffb}, {0x110bd, 0x110bd}, {0x110cd, 0x110cd},
        {0x1d173, 0x1d17a}, {0xe0000, 0xe007f},
    };
    if ((c<0x20 && c!='\t' && c!='\n') || (c>=0x7f && c<0xa0)) {
        return 1;
    }
    for (iz i = 0; i < countof(ranges); i++) {
        if (c>=ranges[i][0] && c<=ranges[i][1]) {
            return 1;
        }
    }
    return 0;
}

// A Run's text, checked: CRLF as LF, blank lines before it and blanks
// after it dropped. Null, and *why, if it may not run: empty, over
// TERM_RUNTEXT, not UTF-8, holding what termruninvisible hides, or for
// cmd, more than one line or longer than cmd.exe takes.
static Str termruntext(Arena *a, Str text, i32 kind, Str *why)
{
    Buf b(a, text.len + 1);
    for (iz i = 0; i < text.len; i++) {
        if (!(text[i]=='\r' && i+1<text.len && text[i+1]=='\n')) {
            putbyte(&b, text[i]);
        }
    }
    Str r = trimright(finish(&b));
    iz  lead = 0;
    for (iz i = 0; i < r.len && (r[i]==' ' || r[i]=='\t' || r[i]=='\n'); i++) {
        lead = r[i]=='\n' ? i+1 : lead;
    }
    r = cuthead(r, lead);
    if (!r.len) {
        *why = "the command is empty";
        return {};
    } else if (r.len > TERM_RUNTEXT) {
        *why = "the command is over 16 KiB";
        return {};
    }
    iz units = 0;
    for (iz i = 0; i < r.len;) {
        i32 c = utf8strict(r, &i);
        if (c<0 || (c>=0xd800 && c<=0xdfff)) {
            *why = "the command is not UTF-8";
            return {};
        } else if (termruninvisible(c)) {
            *why = "the command holds a control or invisible character, "
                   "so what runs would not be what shows";
            return {};
        }
        units += c>0xffff ? 2 : 1;
    }
    if (kind==RUN_CMD && cut(r, '\n').ok) {
        *why = "cmd.exe runs one line, and this is more";
        return {};
    } else if (kind==RUN_CMD && units>TERM_RUNCMD) {
        *why = "the command is longer than cmd.exe takes (8191 characters)";
        return {};
    }
    return r;
}

struct TermRunBlock {
    i32 kind;   // RUN_NONE: no Run at all
    Str text;   // what runs, prompts stripped
    Str why;    // a Run, but one that may not: why
};

// Whether a code block's line begins with a prompt: "$ " for sh, "PS> "
// or "PS FOLDER> " for PowerShell. Returns its length, else 0.
static iz termrunprompt(Str line, i32 kind)
{
    if (kind==RUN_SH && startswith(line, "$ ")) {
        return 2;
    } else if (kind==RUN_PS && startswith(line, "PS> ")) {
        return 4;
    } else if (kind==RUN_PS && startswith(line, "PS ")) {
        for (iz i = 3; i+1 < line.len; i++) {
            if (line[i]=='>' && line[i+1]==' ') {
                return i + 2;
            }
        }
    }
    return 0;
}

// A fenced code block of the agent's as a Run (the Run button): its kind
// by its language, its text as termruntext has it. A block whose every
// line (but blank ones, and those continuing a line that ends in \ or,
// for PowerShell, `) begins with a prompt runs without them; one where
// only some do is a transcript, with output, and has no Run, as has an
// empty block.
static TermRunBlock termrunblock(Arena *a, Str lang, Str code)
{
    TermRunBlock r    = {};
    i32          kind = termrunlang(lang);
    if (!kind) {
        return r;
    }
    i32 with = 0, without = 0;
    b32 cont = 0;
    for (Cut c = {{}, code, 1}; c.ok;) {
        c = cut(c.tail, '\n');
        Str line = trimright(c.head);
        if (line.len && !cont) {
            with    += termrunprompt(line, kind) > 0;
            without += !termrunprompt(line, kind);
        }
        u8 last = line.len ? line[line.len-1] : 0;
        cont = last=='\\' || (kind==RUN_PS && last=='`');
    }
    if (!with && !without) {
        return r;
    } else if (with && without) {
        return r;  // a transcript
    }
    Str text = code;
    if (with) {
        Buf b(a, code.len + 1);
        cont = 0;
        for (Cut c = {{}, code, 1}; c.ok;) {
            c = cut(c.tail, '\n');
            Str line = c.head;
            Str bare = trimright(line);
            if (bare.len && !cont) {
                line = cuthead(line, termrunprompt(bare, kind));
            }
            print(&b, line);
            if (c.ok) {
                putbyte(&b, '\n');
            }
            u8 last = bare.len ? bare[bare.len-1] : 0;
            cont = last=='\\' || (kind==RUN_PS && last=='`');
        }
        text = finish(&b);
    }
    r.kind = kind;
    r.text = termruntext(a, text, kind, &r.why);
    r.text = r.text.len ? r.text : text;
    return r;
}

struct TermRunAsk {
    b32 on;     // term.open has run
    i32 kind;
    Str text;
    Str lang;
    Str from;   // the session its notice goes to, else empty
    Str why;    // a refusal
};

// term.open's run, checked: absent, or an object with a text that may
// run (termruntext), a lang naming a shell (termrunlang), cmd's only on
// Windows, and from, if any, a session id. Never with edit.
static TermRunAsk termrunask(Arena *a, Json *j, b32 win)
{
    TermRunAsk r  = {};
    Json      *rj = jsonget(j, "run");
    if (rj->type == JSON_MISSING) {
        return r;
    }
    r.on = 1;
    if (rj->type != JSON_OBJECT) {
        r.why = "run must be an object";
        return r;
    } else if (jsonget(j, "edit")->type != JSON_MISSING) {
        r.why = "term.open takes run or edit, not both";
        return r;
    }
    Json *tj = jsonget(rj, "text");
    Json *lj = jsonget(rj, "lang");
    Json *fj = jsonget(rj, "from");
    if (tj->type!=JSON_STRING || lj->type!=JSON_STRING) {
        r.why = "run.text and run.lang must be strings";
        return r;
    } else if (fj->type!=JSON_MISSING && (fj->type!=JSON_STRING || !validsid(jsonstr(fj)))) {
        r.why = "run.from must be a session id";
        return r;
    }
    r.lang = jsonstr(lj);
    r.from = jsonstr(fj);
    r.kind = termrunlang(r.lang);
    if (!r.kind) {
        r.why = "run.lang names no shell this runs: sh, bash, zsh, ksh, "
                "powershell, pwsh or cmd";
        return r;
    } else if (r.kind==RUN_CMD && !win) {
        r.why = "run: cmd runs only on Windows";
        return r;
    }
    Str why = {};
    r.text = termruntext(a, jsonstr(tj), r.kind, &why);
    if (!r.text.len) {
        r.why = concat(a, "run: ", why);
    }
    return r;
}

// How PowerShell runs a Run's text, which it takes from the environment
// rather than its command line, which has no quoting to get wrong, as
// the agent's commands do (psboot): parsed alone first, so that a
// syntax error shows as one, then run as a script whose exit is a
// native program's status, if it ran one, else 0 or 1. Unlike psboot it
// runs interactively, for Read-Host and Get-Credential, and on Windows
// first shows the text, dimmed (psrunhead), as --tty-run does on POSIX.
#define PSRUNVAR "PROSPERO_RUN_TEXT"
#define PSRUNREAD "$__c=$env:" PSRUNVAR ";Remove-Item Env:" PSRUNVAR ";"
#define PSRUNBODY \
    "try{$__b=[ScriptBlock]::Create($__c)}" \
    "catch{$__e=$_.Exception;if($__e.InnerException){$__e=$__e.InnerException};" \
    "[Console]::Error.WriteLine($__e.Message);exit 1};" \
    "$__b=[ScriptBlock]::Create($__c+[char]10+'$global:__ok=$?');" \
    "$global:__ok=$false;$global:LASTEXITCODE=0;" \
    "try{. $__b|Out-Default}catch{[Console]::Error.WriteLine(($_|Out-String).Trim());exit 1};" \
    "if($global:__ok){exit 0};if($LASTEXITCODE){exit $LASTEXITCODE};exit 1"
static char const psrunboot[] = PSRUNREAD PSRUNBODY;
static char const psrunhead[] =
    PSRUNREAD "[Console]::WriteLine([char]27+'[2m'+$__c+[char]27+'[0m');" PSRUNBODY;

// A Run's first line, cut to TERM_RUNLINE bytes on a code point, and how
// many lines it has.
static Str termrunfirst(Str text, i32 *lines)
{
    *lines = 1;
    for (iz i = 0; i < text.len; i++) {
        *lines += text[i] == '\n';
    }
    Str first = cut(text, '\n').head;
    return first.len>TERM_RUNLINE ? utf8head(first, TERM_RUNLINE) : first;
}

// Shell integration: hooks that the engine puts in a tab's zsh, bash or
// PowerShell print OSC 133 marks and OSC 7, each with the tab's nonce
// (termcmd.cpp), so that a mark a program prints, not knowing it, never
// counts.

enum : i32 {
    HOOK_NONE,
    HOOK_ZSH,
    HOOK_BASH,
    HOOK_PS,
};

// Whether ConPTY passes OSC 133 through, in order, as it does on
// Windows 11 build 26200 (a spike's probe, 2026-11-16).
#define TERM_HOOKWIN 1

// The hooks' scripts (assets/terminal), which termhookdir writes out
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const hookzshenv[] = {
    #embed "../assets/terminal/zshenv"
};
static u8 const hookzprofile[] = {
    #embed "../assets/terminal/zprofile"
};
static u8 const hookzshrc[] = {
    #embed "../assets/terminal/zshrc"
};
static u8 const hookzlogin[] = {
    #embed "../assets/terminal/zlogin"
};
static u8 const hookbashrc[] = {
    #embed "../assets/terminal/bashrc"
};
static u8 const hookps1[] = {
    #embed "../assets/terminal/hooks.ps1"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

// How a hooked PowerShell starts (-NoProfile -NoExit -Command): it runs
// hooks.ps1, named by PROSPERO_TERM_HOOKS, as a script block, which no
// execution policy stops, and which loads the profiles; failing that,
// it loads them itself.
static char const psmarkboot[] =
    "try{. ([ScriptBlock]::Create([IO.File]::ReadAllText($env:PROSPERO_TERM_HOOKS)))}"
    "catch{Remove-Item Env:PROSPERO_TERM_NONCE,Env:PROSPERO_TERM_HOOKS -EA 0;"
    "foreach($__p in $PROFILE.AllUsersAllHosts,$PROFILE.AllUsersCurrentHost,"
    "$PROFILE.CurrentUserAllHosts,$PROFILE.CurrentUserCurrentHost)"
    "{if($__p -and (Test-Path -LiteralPath $__p)){. $__p}}}";

// A program's file name, without .exe and the like, compared as the
// platform would.
static b32 termprogis(Str prog, Str name, b32 win)
{
    Str base = termeditname(prog);
    return win ? equalfold(base, name) : base==name;
}

// A terminal program's family, as term.open's reply gives it (kind):
// "sh" for a POSIX shell, "ps" for PowerShell, "cmd" for cmd.exe on
// Windows, else empty.
static Str termkind(Slice<Str> argv, b32 win)
{
    if (!argv.len) {
        return {};
    }
    Str shs[] = {"sh", "bash", "zsh", "dash", "ksh", "mksh", "ash", "yash"};
    for (iz i = 0; i < countof(shs); i++) {
        if (termprogis(argv[0], shs[i], win)) {
            return "sh";
        }
    }
    if (termprogis(argv[0], "busybox", win)) {
        return argv.len>1 && argv[1]=="sh" ? Str("sh") : Str{};
    } else if (termprogis(argv[0], "pwsh", win) || termprogis(argv[0], "powershell", win)) {
        return "ps";
    } else if (win && termprogis(argv[0], "cmd", win)) {
        return "cmd";
    }
    return {};
}

// Which hooks a terminal's argv takes: zsh or bash on POSIX, alone or
// with -l (*login), or PowerShell, alone or with -NoLogo. Any other
// argument may change how it starts, so it gets none.
static i32 termhookable(Slice<Str> argv, b32 win, b32 *login)
{
    *login = 0;
    if (!argv.len || argv.len>2) {
        return HOOK_NONE;
    }
    Str arg  = argv.len>1 ? argv[1] : Str{};
    Str base = basename(argv[0]);
    if (!win && (base=="zsh" || base=="bash")) {
        if (argv.len==1 || arg=="-l") {
            *login = argv.len > 1;
            return base=="zsh" ? HOOK_ZSH : HOOK_BASH;
        }
    } else if (termprogis(argv[0], "pwsh", win) || termprogis(argv[0], "powershell", win)) {
        b32 ok = argv.len==1 || (win ? equalfold(arg, "-NoLogo") : arg=="-NoLogo");
        if (ok && (!win || TERM_HOOKWIN)) {
            return HOOK_PS;
        }
    }
    return HOOK_NONE;
}

// One step of a Run's: a mark, or a line typed. Returns 1 when its C
// begins it, so that what it prints is all its screen holds.
static i32 termrunstep(PromptRun *r, i32 ev, TermMark m, i64 now)
{
    i64 settle = (i64)TRUN_SETTLE * SEC;
    i64 prompt = (i64)TRUN_PROMPT * SEC;
    if (ev == TRUN_TYPED) {
        if (r->state == TRUN_ENDED) {
            r->typed    = 1;
            r->settleat = now;
        }
        return 0;
    }
    switch (r->state) {
    case TRUN_ARMED:
        if (m.kind == 'C') {
            r->state   = TRUN_RUNNING;
            r->startat = now;
            return 1;
        } else if (m.kind=='D' || m.kind=='A') {
            // Nothing ran: a syntax error, or a line taken as none
            r->state    = TRUN_ENDED;
            r->hascode  = m.hascode;
            r->code     = m.code;
            r->unrun    = !m.hascode;
            r->startat  = r->endat = now;
            r->settleat = now + (m.hascode ? settle : prompt);
        }
        return 0;
    case TRUN_RUNNING:
        if (m.kind=='D' || m.kind=='A') {
            r->state    = TRUN_ENDED;
            r->hascode  = m.kind=='D' && m.hascode;
            r->code     = r->hascode ? m.code : 0;
            r->endat    = now;
            r->settleat = now + (m.kind=='D' ? settle : prompt);
        }
        return 0;  // a C: the next line of the block
    case TRUN_ENDED:
        if (m.kind == 'A') {
            r->settleat = r->settleat<now+prompt ? r->settleat : now+prompt;
        } else if (m.kind=='C' && !r->typed && now<r->settleat) {
            // The block's next line, which a shell without bracketed
            // paste reads as a command of its own
            r->state   = TRUN_RUNNING;
            r->startat = r->unrun ? now : r->startat;
            r->unrun   = 0;
        } else if (m.kind == 'C') {
            r->settleat = now;  // the user's own command
        }
        return 0;
    }
    return 0;
}

// Whether a Run is over, its notice due.
static b32 termrundue(PromptRun *r, i64 now)
{
    return r->state==TRUN_ENDED && now>=r->settleat;
}

// A program's path, by name on a PATH, else null: findprog on Windows,
// findpathext elsewhere.
typedef Str TermWhich(void *ctx, Arena *, Str name);

// The PowerShell a terminal runs: the session's if that is its shell,
// else pwsh, else powershell(.exe), else null.
static Str termpsprog(Arena *a, Str shkind, Str shpath, b32 win, TermWhich *which, void *ctx)
{
    if ((shkind=="pwsh" || shkind=="powershell") && shpath.len) {
        return shpath;
    }
    Str names[] = {win ? Str("pwsh.exe") : Str("pwsh"),
                   win ? Str("powershell.exe") : Str("powershell")};
    for (iz i = 0; i < countof(names); i++) {
        Str p = which(ctx, a, names[i]);
        if (p.len) {
            return p;
        }
    }
    return {};
}

// cmd.exe by a full path: COMSPEC if it is one, else on PATH, else null.
static Str termcmdprog(Arena *a, Str comspec, TermWhich *which, void *ctx)
{
    comspec = trim(comspec);
    return pathroot(comspec, 1) ? comspec : which(ctx, a, Str("cmd.exe"));
}

struct TermPickAsk {
    Str        lang;     // term.open's shell: a fence language
    Slice<Str> dflt;     // the terminal's usual argv (termargv)
    Str        shkind;   // the session's shell
    Str        shpath;
    Str        comspec;  // the environment's
    b32        win;
    b32        login;    // a POSIX sh as a login shell
    TermWhich *which;
    void      *ctx;
};

struct TermPick {
    Slice<Str> argv;
    Str        why;  // a refusal
};

// The interactive shell of a terminal for a block of lang (term.open's
// shell), by family: the usual one if it is of it, else for sh the
// block's own bash, zsh or ksh, then bash, then zsh, else /bin/sh (on
// Windows the session's sh or busybox); for PowerShell termpsprog; for
// cmd termcmdprog, only on Windows.
static TermPick termrunpick(Arena *a, TermPickAsk *k)
{
    TermPick r    = {};
    i32      kind = termrunlang(k->lang);
    Str      want = kind==RUN_SH ? Str("sh") : kind==RUN_PS ? Str("ps") : Str("cmd");
    if (!kind) {
        r.why = "shell names no shell: sh, bash, zsh, ksh, powershell, pwsh or cmd";
        return r;
    } else if (kind==RUN_CMD && !k->win) {
        r.why = "shell: cmd runs only on Windows";
        return r;
    } else if (termkind(k->dflt, k->win) == want) {
        r.argv = k->dflt;
        return r;
    }
    switch (kind) {
    case RUN_SH:
        if (k->win) {
            if (k->shkind!="sh" && k->shkind!="busybox") {
                r.why = concat(a, "shell: sh blocks run in this session's POSIX shell, sh "
                                  "or busybox, and its shell is ", k->shkind);
                return r;
            }
            r.argv = termshell(a, k->shkind, k->shpath, 1);
            r.why  = r.argv.len ? Str{} : Str("shell: this session's shell has no path");
        } else {
            Str lang  = trim(k->lang);
            Str names[] = {
                equalfold(lang, "bash") ? Str("bash") : equalfold(lang, "zsh") ? Str("zsh")
                    : equalfold(lang, "ksh") ? Str("ksh") : Str{},
                "bash", "zsh",
            };
            Str prog = {};
            for (iz i = 0; !prog.len && i < countof(names); i++) {
                prog = names[i].len ? k->which(k->ctx, a, names[i]) : Str{};
            }
            r.argv = push(a, r.argv, prog.len ? prog : Str("/bin/sh"));
            if (k->login) {
                r.argv = push(a, r.argv, Str("-l"));
            }
        }
        return r;
    case RUN_PS: {
        Str prog = termpsprog(a, k->shkind, k->shpath, k->win, k->which, k->ctx);
        if (!prog.len) {
            r.why = "shell: no PowerShell on this computer";
            return r;
        }
        r.argv = push(a, r.argv, prog);
        r.argv = push(a, r.argv, Str("-NoLogo"));
    } return r;
    case RUN_CMD: {
        Str prog = termcmdprog(a, k->comspec, k->which, k->ctx);
        if (!prog.len) {
            r.why = "shell: cmd.exe is not on PATH";
            return r;
        }
        r.argv = push(a, r.argv, prog);
    } return r;
    }
    return r;
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
// worktree session, in a folder of its own, has its own, and so does a
// subagent in a worktree of its own, for its subagents too.
static Str termrootwalk(Arena *a, Str sid, TermLook *look, void *ctx)
{
    for (i32 side = 0; side < 8; side++) {
        TermNode n = look(ctx, a, sid);
        for (i32 up = 0; up<16 && n.found && n.parent.len && !n.worktree &&
                         !(n.parent==sid); up++) {
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

// Whether tabs' shells take the hooks: [terminal] integration.
static b32 termintegrate(Engine *e)
{
    return cfgbool(cfgget(e->cfg, "terminal", {}, "integration", 0)) != 0;
}

// The hooks' folder, <state>/terminal/hooks/HASH, written once for its
// scripts' content (as skillcopy writes a skill), older ones removed.
// Null if it cannot be had.
static Str termhookdir(Engine *e, Arena *a)
{
    static struct {
        Str        path;
        u8 const  *data;
        iz         len;
    } const scripts[] = {
        {"zsh/.zshenv", hookzshenv, countof(hookzshenv)},
        {"zsh/.zprofile", hookzprofile, countof(hookzprofile)},
        {"zsh/.zshrc", hookzshrc, countof(hookzshrc)},
        {"zsh/.zlogin", hookzlogin, countof(hookzlogin)},
        {"bashrc", hookbashrc, countof(hookbashrc)},
        {"hooks.ps1", hookps1, countof(hookps1)},  // last: written, all are
    };
    Slice<SkillFile> files = {};
    for (iz i = 0; i < countof(scripts); i++) {
        SkillFile f = {scripts[i].path, Str{(u8 *)scripts[i].data, scripts[i].len}};
        files = push(a, files, f);
    }
    Str   top  = join(a, join(a, e->statedir, "terminal"), "hooks");
    Str   hash = skillhash(a, files);
    Str   dir  = join(a, top, hash);
    Arena t    = *a;
    Str   last = join(&t, dir, "hooks.ps1");
    if (os_stat(e->os, t, last).type == STAT_FILE) {
        return dir;
    }
    b32 ok = os_mkdirs(e->os, t, top) && dirwhole(e->os, t, top, dir, files);
    if (!ok && os_stat(e->os, t, last).type!=STAT_FILE) {
        return {};  // not even another engine's
    }
    skillprune(e->os, t, top, hash);
    return dir;
}

// For status: the screens, the output they were fed, and those kept
// after their terminals ended, for tests.
static void termstatus(Engine *e, JsonOut *w)
{
    i64 screens = 0, fed = 0, kept = 0, runs = 0;
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        screens += t->tid && t->screen;
        fed     += t->tid ? t->fed : 0;
        kept    += t->tid && t->kept;
        runs    += t->tid && t->srun.state;
    }
    putintkey(w, "screens", screens);
    putintkey(w, "fed", fed);
    putintkey(w, "kept", kept);
    putintkey(w, "runs", runs);
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
static void termblockfree(Engine *e, TermScreen *sc)
{
    if (sc->region) {
        os_release(e->os, sc->region);
    }
    *sc = {};
}

// Free a slot, and its screen.
static void termfree(Engine *e, Term *t)
{
    termblockfree(e, &t->scr);
    termblockfree(e, &t->rscreen);
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
static b32 termblock(Engine *e, TermScreen *sc, i32 hist, i32 cols, i32 rows)
{
    static char  name[] = "Prospero(" PROSPERO_VERSION ")";
    ariel_Config c      = {};
    c.cols      = cols;
    c.rows      = rows;
    c.capcols   = termscreencap(cols, 200);
    c.caprows   = termscreencap(rows, 80);
    c.histlines = hist ? hist : -1;  // ariel's 0 is its default
    c.histbytes = termscrollbytes(hist);
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
    ariel_Term *st  = sc->screen ? ariel_move(sc->screen, mem, size, &c)
                                 : ariel_init(mem, size, &c);
    if (!st) {
        os_release(e->os, r);
        return 0;
    }
    if (sc->region) {
        os_release(e->os, sc->region);
    }
    sc->region  = r;
    sc->screen  = st;
    sc->capcols = c.capcols;
    sc->caprows = c.caprows;
    return 1;
}

static b32 termscreenblock(Engine *e, Term *t, i32 cols, i32 rows)
{
    b32 ok = termblock(e, &t->scr, t->hist, cols, rows);
    t->screen = t->scr.screen;
    return ok;
}

// A screen's size changed: room first, if it needs more.
static void termblocksize(Engine *e, TermScreen *sc, i32 hist, PtySize z)
{
    if (z.cols>sc->capcols || z.rows>sc->caprows) {
        termblock(e, sc, hist, z.cols, z.rows);
    }
    ariel_resize(sc->screen, z.cols, z.rows, z.xpx/z.cols, z.ypx/z.rows);
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

// Output to a screen whose events nobody wants.
static void termfeedquiet(ariel_Term *screen, Str data)
{
    ariel_Str in = {data.data, data.len};
    while (in.len) {
        in = ariel_feed(screen, in).rest;
    }
}

// Output to the screen, and to a Run's while it runs: its title, and a
// hooked shell's marks, which move its Run along.
static void termfeedbytes(Engine *e, Term *t, Str data)
{
    ariel_Str in = {data.data, data.len};
    for (;;) {
        ariel_Fed f = ariel_feed(t->screen, in);
        i32 st = t->srun.state;
        if (t->rscreen.screen && (st==TRUN_ARMED || st==TRUN_RUNNING)) {
            termfeedquiet(t->rscreen.screen, Str{in.data, in.len - f.rest.len});
        }
        in = f.rest;
        if (f.ev.kind == ARIEL_EV_NONE) {
            return;
        } else if (f.ev.kind==ARIEL_EV_TITLE && !f.ev.arg) {
            t->titlelen = termtitle(t->title, {f.ev.data.data, f.ev.data.len});
        } else if (f.ev.kind == ARIEL_EV_CWD) {
            // Its folder, as the hooks say it at each prompt
            Str nonce = {t->nonce, t->hooked ? countof(t->nonce) : 0};
            if (termcmdfolder(&t->cmds, {f.ev.data.data, f.ev.data.len}, nonce, t->win)) {
                iz n = t->cmds.cwdlen<TERM_CWDCAP ? t->cmds.cwdlen : 0;
                t->cwdlen = n ? n : t->cwdlen;
                copybytes(t->cwd, t->cmds.cwd, n);
            }
        } else if (f.ev.kind == ARIEL_EV_MARK) {
            // Any prompt mark counts a line (termcmd.cpp); its own move
            // its records, and its Run
            Str      nonce = {t->nonce, t->hooked ? countof(t->nonce) : 0};
            TermMark m     = termcmdmark(&t->cmds, t->screen, {f.ev.data.data, f.ev.data.len},
                                         nonce, os_ticks(e->os));
            if (!m.ok) {
                continue;
            }
            t->lastmark = m.kind=='B' ? t->lastmark : m.kind;  // B: at its prompt still
            if (st!=TRUN_NONE && termrunstep(&t->srun, TRUN_MARK, m, os_ticks(e->os))) {
                ariel_reset(t->rscreen.screen, ARIEL_RESET_FULL);
            }
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
        termfeedbytes(e, t, data);
        return;
    }
    u8 row[1024];
    for (iz i = 0; t->candlen;) {
        iz j = i;
        for (; j<data.len && data[j]!='\r' && data[j]!='\n'; j++) {}
        termfeedbytes(e, t, Str{data.data+i, j-i});
        if (termprompt(termcursorrow(t, row, countof(row)))) {
            termsecretadd(&e->terms->secrets, {t->cand, t->candlen}, 2);
            t->candlen = 0;
        }
        if (j == data.len) {
            return;
        }
        termfeedbytes(e, t, Str{data.data+j, 1});
        i = j + 1;
        if (i == data.len) {
            return;
        }
        if (!t->candlen) {
            termfeedbytes(e, t, cuthead(data, i));
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

// Once hung up, and the shell's exit and EOF both in: the slot goes, or
// with keep stays, kept, its screen readable until term.close, its
// slot's eviction (termstart), or a hang-up of all.
static void termsettle(Engine *e, Term *t)
{
    if (t->closed && t->exited && t->eof) {
        if (t->keep && t->screen) {
            t->kept = 1;
            t->used = os_ticks(e->os);
        } else {
            termfree(e, t);
        }
    }
}

// Hang up, or with kill sweep the tree. The terminal says nothing more,
// and its slot goes once the shell's exit and EOF are in, or with keep
// is kept (termsettle). A kept one goes now.
static void termhangup(Engine *e, Term *t, b32 kill, b32 keep)
{
    if (t->kept) {
        termfree(e, t);
        return;
    }
    termscan(e, t);  // before the ring goes
    if (!t->closed) {
        t->closed = 1;
        t->keep   = keep;
        t->cut    = !t->exited;
        if (kill) {
            os_kill(e->os, t->proc, 1);
        } else {
            os_letgo(e->os, t->proc);
        }
        os_procfree(e->os, t->proc);  // the hang-up: SIGHUP to the shell
        t->proc = 0;
    } else {
        t->keep &= keep;
    }
    termsettle(e, t);
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
        termhangup(e, t, 0, 1);  // nothing left to hang up, but its reaper
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
    termhangup(e, t, 0, 1);
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

// A shell's program on PATH (TermWhich): on Windows by findprog, else
// in an absolute directory of it.
struct TermWhere {
    Os *os;
    Str path;
    b32 win;
};

static Str termwhere(void *ctx, Arena *a, Str name)
{
    TermWhere *w = (TermWhere *)ctx;
    if (w->win) {
        return findprog(w->os, a, carve(a, 1<<20), w->path, name, {}, 1);
    }
    Str        none[] = {{}};
    Slice<Str> exts   = {none, 1, 1};
    return findpathext(w->os, a, carve(a, 1<<20), w->path, name, exts, 0, 1);
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

// A Run's program on Windows (term.open's run), by its kind. cmd.exe
// runs the line as it is (/s), with its AutoRun, as a cmd tab would,
// but without the current folder in its program lookup, so that a
// git.cmd the agent left there never runs for "git status". PowerShell
// is the session's, if that is its shell, else pwsh, else
// powershell.exe, running psrunhead with the text in the environment.
// sh is the session's sh or busybox, with its login flags; never WSL's.
// Returns why it cannot run, else null.
static Str termrunwin(Engine *e, Arena *a, TermRunAsk *run, Shell *sh, Slice<Str> *env,
                      Slice<Str> *argv, Str *cmdline)
{
    TermWhere w    = {e->os, envpath(*env, 1), 1};
    Str       kind = shellkinds[sh->kind];
    *argv = {};
    switch (run->kind) {
    case RUN_CMD: {
        Str comspec = termcmdprog(a, envget(*env, "COMSPEC", 1), termwhere, &w);
        if (!comspec.len) {
            return "cannot run the command: cmd.exe is not on PATH";
        }
        Buf b(a, 64 + comspec.len + run->text.len);
        putbyte(&b, '"');
        print(&b, comspec);
        print(&b, "\" /s /c \"");
        print(&b, run->text);
        putbyte(&b, '"');
        *cmdline = finish(&b);
        *argv    = push(a, *argv, comspec);
        *env     = envset(a, *env, "NoDefaultCurrentDirectoryInExePath", "1", 1);
    } break;
    case RUN_PS: {
        Str prog = termpsprog(a, kind, sh->path, 1, termwhere, &w);
        if (!prog.len) {
            return "cannot run the command: neither pwsh nor powershell.exe is on PATH";
        }
        *argv = push(a, *argv, prog);
        *argv = push(a, *argv, Str("-NoLogo"));
        *argv = push(a, *argv, Str("-Command"));
        *argv = push(a, *argv, Str(psrunhead));
        *env  = envset(a, *env, PSRUNVAR, run->text, 1);
    } break;
    case RUN_SH:
        if (kind!="sh" && kind!="busybox") {
            return concat(a, "run: sh blocks run in this session's POSIX shell, sh or "
                             "busybox, and its shell is ", kind);
        }
        *argv = termshell(a, kind, sh->path, 1);
        if (!argv->len) {
            return "run: this session's shell has no path";
        }
        *argv = push(a, *argv, Str("-c"));
        *argv = push(a, *argv, run->text);
        break;
    }
    return {};
}

// Start a terminal's shell, or an edit's editor, and reply with its tid,
// before its output.
static Str termstart(Engine *e, Arena *scratch, i64 id, Json *j, Session *s, Str sid)
{
    i64   peer = jsonint(jsonget(j, "peer"), 0);
    Term *t    = 0;
    for (i32 i = 0; !t && i < TERM_MAX; i++) {
        t = e->terms->slots[i].tid ? 0 : &e->terms->slots[i];
    }
    Term *old = 0;  // else the slot of the kept screen longest unused
    for (i32 i = 0; !t && i < TERM_MAX; i++) {
        Term *k = &e->terms->slots[i];
        old = k->kept && (!old || k->used<old->used) ? k : old;
    }
    if (!t && old) {
        t = old;  // freed only once the terminal surely opens, below
    }
    if (!t || termcount(e, peer)>=TERM_PERPEER) {
        return "too many terminals are open";
    }
    b32         win = s->te.windows;
    TermRunAsk  run = termrunask(scratch, j, win);
    TermEditAsk ed  = termeditask(j, win);
    Json       *shj = jsonget(j, "shell");  // a fence language, for Run
    Str         ask = jsonstr(shj);
    if (run.why.len) {
        return run.why;
    } else if (ed.why.len) {
        return ed.why;
    } else if (shj->type!=JSON_MISSING && (shj->type!=JSON_STRING || !ask.len)) {
        return "shell must be a code block's language";
    } else if (ask.len && (ed.on || run.on)) {
        return "term.open takes one of shell, run and edit";
    } else if ((ed.on || run.on || ask.len) && win!=HOST_WINDOWS) {
        // A container's kept subagent on Windows: Linux's paths, in a
        // folder gone, with no editor here to take them
        return ed.on  ? Str("edit: this session's files are not on this machine") :
               run.on ? Str("run: this session's files are not on this machine") :
                        Str("shell: this session's files are not on this machine");
    }

    // In its folder, or the one asked for if it is one
    Str cwd = jsonstr(jsonget(j, "cwd"));
    cwd = cwd.len && os_stat(e->os, *scratch, cwd).type==STAT_DIR ? cwd : s->cwd;

    // A login shell runs the profile itself, so it starts from what
    // this process started with, as a terminal's would: not the login
    // environment a desktop-started window took (macOS), whose EDITOR
    // would start zsh in vi mode, its PATH entries doubled
    Shell      sh    = s->te.shell;
    b32        login = !(PTY_LINUX && e->gui);
    Slice<Str> env   = login && !win ? os_launchenv(e->os, scratch) : e->env;
    Str        path  = cfgsearchpath(e->cfg, scratch, envpath(env, win), e->home, win);
    env = envsetpath(scratch, env, path, win);
    env = shellover(s, scratch, &sh, env, 1);
    if (run.on) {
        // Agent-written, so no more than the agent's own commands get
        // (shellenv): none of the variables secret settings come from
        Slice<Str> secrets = cfgsecrets(e->cfg, scratch);
        for (iz i = 0; i < secrets.len; i++) {
            env = envdel(scratch, env, secrets[i], win);
        }
    }
    Slice<Str> argv    = {};
    Str        cmdline = {};
    Str        editor  = {};  // the reply's, on Windows
    Str        picked  = {};  // the program shell picked, by name
    if (run.on && win) {
        Str why = termrunwin(e, scratch, &run, &sh, &env, &argv, &cmdline);
        if (why.len) {
            return why;
        }
    } else if (run.on) {
        // This program again, which shows the command and becomes the
        // terminal's shell running it (main_posix.cpp)
        Str self = PTY_LINUX ? Str("/proc/self/exe") : e->te.exe;
        if (!self.len) {
            return "cannot run the command: this program's path is not known";
        }
        Str conf  = trim(cfgget(e->cfg, "terminal", {}, "shell", 0));
        Slice<Str> shv = termargv(scratch, conf, e->home, shellkinds[sh.kind], sh.path,
                                  sh.distro, envget(env, "SHELL", win), win, login);
        argv = push(scratch, argv, self);
        argv = push(scratch, argv, Str("--tty-run"));
        env  = envset(scratch, env, "PROSPERO_RUN_TEXT", run.text, 0);
        env  = envset(scratch, env, "PROSPERO_RUN_LANG", run.lang, 0);
        env  = envset(scratch, env, "PROSPERO_RUN_SHELL", shv.len ? shv[0] : Str{}, 0);
        env  = envset(scratch, env, "PROSPERO_RUN_LOGIN", login ? Str("1") : Str("0"), 0);
    } else if (ed.on && win) {
        Str why = {};
        cmdline = termeditwin(e, scratch, &ed, env, &argv, &editor, &why);
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
        argv = push(scratch, argv, self);
        argv = push(scratch, argv, Str("--tty-edit"));
    } else {
        Str conf = trim(cfgget(e->cfg, "terminal", {}, "shell", 0));
        argv = termargv(scratch, conf, e->home, shellkinds[sh.kind], sh.path, sh.distro,
                        envget(env, "SHELL", win), win, login);
        if (!argv.len) {
            return "[terminal] shell: a quote is left open";
        }
        if (ask.len) {
            // The block's family's interactive shell, labeled by its name
            TermWhere   w = {e->os, envpath(env, win), win};
            TermPickAsk k = {};
            k.lang    = ask;
            k.dflt    = argv;
            k.shkind  = shellkinds[sh.kind];
            k.shpath  = sh.path;
            k.comspec = win ? envget(env, "COMSPEC", 1) : Str{};
            k.win     = win;
            k.login   = login;
            k.which   = termwhere;
            k.ctx     = &w;
            TermPick p = termrunpick(scratch, &k);
            if (p.why.len) {
                return p.why;
            } else if (!(p.argv[0] == argv[0])) {
                picked = termeditname(p.argv[0]);
            }
            argv = p.argv;
        }
    }
    Str term = termname(e, *scratch, win);  // a literal
    env = termenv(scratch, env, win, PTY_MAC, term);
    // What a profile tests to skip what would keep a tab's program from
    // starting (exec tmux): edit or run, else unset
    env = envdel(scratch, env, "PROSPERO_TAB", win);
    if (ed.on || run.on) {
        env = envset(scratch, env, "PROSPERO_TAB", ed.on ? Str("edit") : Str("run"), win);
    }
    if (!run.on) {
        Str names[] = {
            "PROSPERO_RUN_TEXT", "PROSPERO_RUN_LANG", "PROSPERO_RUN_SHELL", "PROSPERO_RUN_LOGIN",
        };
        for (iz i = 0; i < countof(names); i++) {
            env = envdel(scratch, env, names[i], win);
        }
    }
    if (ed.on && !win) {
        // Never quoted: no shell parses these
        Str names[] = {
            "PROSPERO_EDIT_FILE", "PROSPERO_EDIT_LINE", "PROSPERO_EDIT_COL",
            "PROSPERO_EDIT_EDITOR", "PROSPERO_EDIT_TEMPLATE", "PROSPERO_EDIT_LOGIN",
        };
        for (iz i = 0; i < countof(names); i++) {
            env = envdel(scratch, env, names[i], 0);
        }
        env = envset(scratch, env, names[0], ed.path, 0);
        if (ed.line) {
            env = envset(scratch, env, names[1], deskint(scratch, ed.line), 0);
        }
        if (ed.col) {
            env = envset(scratch, env, names[2], deskint(scratch, ed.col), 0);
        }
        if (ed.editor.len) {
            env = envset(scratch, env, names[3], ed.editor, 0);
        }
        if (ed.tmpl.len) {
            env = envset(scratch, env, names[4], ed.tmpl, 0);
        }
        env = envset(scratch, env, names[5], login ? Str("1") : Str("0"), 0);
    }

    // The hooks, in a tab's zsh, bash or PowerShell as it would start,
    // with a nonce of its own
    Str hookvars[] = {
        "PROSPERO_TERM_NONCE", "PROSPERO_TERM_HOOKS", "PROSPERO_TERM_LOGIN",
        "PROSPERO_USER_ZDOTDIR",
    };
    for (iz i = 0; i < countof(hookvars); i++) {
        env = envdel(scratch, env, hookvars[i], win);
    }
    Str kind  = ed.on || run.on ? Str{} : termkind(argv, win);
    b32 hlog  = 0;
    i32 hook  = ed.on || run.on || !termintegrate(e) ? (i32)HOOK_NONE
                                                    : termhookable(argv, win, &hlog);
    Str hdir  = hook ? termhookdir(e, scratch) : Str{};
    Str nonce = {};
    if (hdir.len) {
        Buf nb(scratch, 32);
        printhex(&nb, os_random(e->os), 16);
        printhex(&nb, os_random(e->os), 16);
        nonce = finish(&nb);
        env   = envset(scratch, env, hookvars[0], nonce, win);
        switch (hook) {
        case HOOK_ZSH: {
            // Its startup files are ours, each running the user's
            Str uzd = envget(env, "ZDOTDIR", win);
            if (uzd.len) {
                env = envset(scratch, env, hookvars[3], uzd, win);
            }
            env = envset(scratch, env, "ZDOTDIR", join(scratch, hdir, "zsh"), win);
        } break;
        case HOOK_BASH: {
            // Ours in place of ~/.bashrc, running a login's files
            Slice<Str> v = {};
            v = push(scratch, v, argv[0]);
            v = push(scratch, v, Str("--init-file"));
            v = push(scratch, v, join(scratch, hdir, "bashrc"));
            argv = v;
            if (hlog) {
                env = envset(scratch, env, hookvars[2], Str("1"), win);
            }
        } break;
        case HOOK_PS: {
            Str        words[] = {"-NoLogo", "-NoProfile", "-NoExit", "-Command", psmarkboot};
            Slice<Str> v       = {};
            v = push(scratch, v, argv[0]);
            for (iz i = 0; i < countof(words); i++) {
                v = push(scratch, v, words[i]);
            }
            argv = v;
            env  = envset(scratch, env, hookvars[1], join(scratch, hdir, "hooks.ps1"), win);
        } break;
        }
    }

    Spawn sp   = {};
    sp.argv    = argv;
    sp.cmdline = cmdline;
    sp.cwd     = cwd;
    sp.env     = env;
    sp.mbox    = e->in;
    sp.id      = ++e->nextid;
    Proc *p    = os_ptyspawn(e->os, &sp, termsize(j), TERM_RING, *scratch);
    if (!p) {
        Str what = ed.on  ? Str("cannot start the editor: ") :
                   run.on ? Str("cannot run the command: ") :
                   Str("cannot start the shell: ");
        return concat(scratch, what, os_lasterr(e->os, scratch));
    }
    if (t == old) {
        termfree(e, old);  // the kept screen longest unused makes room
    }
    *t = {};
    t->tid    = ++e->terms->nexttid;
    t->peer   = peer;
    t->sidlen = sid.len;
    copybytes(t->sid, sid.data, sid.len);
    t->proc   = p;
    t->procid = sp.id;
    t->used   = os_ticks(e->os);
    Str label = ed.on && editor.len ? editor : picked.len ? picked : sh.name;
    t->shelllen = label.len<TERM_SHELLCAP ? label.len : 0;
    copybytes(t->shell, label.data, t->shelllen);
    t->cwdlen = cwd.len<TERM_CWDCAP ? cwd.len : 0;
    copybytes(t->cwd, cwd.data, t->cwdlen);
    t->hooked = nonce.len > 0;
    t->win    = win;
    copybytes(t->nonce, nonce.data, nonce.len);
    termcmdreset(&t->cmds);
    t->cols   = termsize(j).cols;
    t->rows   = termsize(j).rows;
    if (run.on) {
        Str first = termrunfirst(run.text, &t->runlines);
        t->run        = 1;
        t->startat    = t->used;
        t->runkey     = hash(run.text);
        t->runlinelen = first.len;
        copybytes(t->runline, first.data, first.len);
        t->fromlen = run.from.len<=countof(t->from) ? run.from.len : 0;
        copybytes(t->from, run.from.data, t->fromlen);
    }
    if (termagent(e)) {
        termconfsecrets(e, *scratch, env, win);
        // As much history as the frontend keeps, within TERM_HIST
        PtySize z  = termsize(j);
        i64     sb = jsonint(jsonget(j, "scrollback"), 1000);
        t->hist = (i32)(sb<0 ? 0 : sb>TERM_HIST ? (i64)TERM_HIST : sb);
        termscreenblock(e, t, z.cols, z.rows);  // else no screen, no reads
    }

    Buf   b(scratch, 1024);
    EvOut w = evbegin(&b, sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putintkey(&w.json, "tid", t->tid);
    putstrkey(&w.json, "shell", picked.len ? picked : sh.name);
    putstrkey(&w.json, "kind", kind);
    if (nonce.len) {
        putstrkey(&w.json, "nonce", nonce);
    }
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
    Str err = termstart(e, &scratch, id, j, s, sid);
    if (transient) {
        forget(e, s);
    }
    if (err.len) {
        reply(e, scratch, id, 0, err, sid);
    }
}

static void termrunend(Engine *, Arena, Term *, i32);
static void termrunheed(Engine *, Arena, Term *, i64);
static void termwaitend(Engine *, Arena, Term *, i32);
static void termwaitheed(Engine *, Arena, Term *);

// A Run typed at the prompt (term.input's run {from, key, first,
// lines}): tracked by its marks, if the shell is hooked, its screen
// kept, it is no term.open run, and the last mark was a prompt with
// nothing of another Run's still going; one of them over is told
// first. Else it is only typing.
static void termrunarm(Engine *e, Arena scratch, Term *t, Json *rj)
{
    Str from  = jsonstr(jsonget(rj, "from"));
    Str key   = jsonstr(jsonget(rj, "key"));
    Str first = jsonstr(jsonget(rj, "first"));
    i64 lines = jsonint(jsonget(rj, "lines"), 0);
    u64 k     = 0;
    b32 ok    = key.len==16 && validsid(from) && from.len<=countof(t->from) &&
                lines>=1 && lines<=TERM_RUNTEXT;
    for (iz i = 0; ok && i < key.len; i++) {
        i32 v = hexval(key[i]);
        ok = v >= 0;
        k  = k<<4 | (u64)(v<0 ? 0 : v);
    }
    if (!ok || !t->hooked || !t->screen || t->run || t->exited || t->lastmark!='A') {
        return;
    } else if (t->srun.state==TRUN_ARMED || t->srun.state==TRUN_RUNNING) {
        return;
    } else if (t->srun.state == TRUN_ENDED) {
        termrunend(e, scratch, t, TEND_DONE);
    }
    if (!termblock(e, &t->rscreen, TERM_RUNHIST, t->cols, t->rows)) {
        return;
    }
    first = first.len>TERM_RUNLINE ? utf8head(first, TERM_RUNLINE) : first;
    first = cut(first, '\n').head;
    t->srun         = {};
    t->srun.state   = TRUN_ARMED;
    t->srun.armedat = os_ticks(e->os);
    t->runkey       = k;
    t->runlines     = (i32)lines;
    t->runlinelen   = first.len;
    copybytes(t->runline, first.data, first.len);
    t->fromlen      = from.len;
    copybytes(t->from, from.data, from.len);
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
    if (t && t->closed && cmd==CMD_TERMCLOSE) {
        // Its tab closed after its end: nothing to keep for
        t->keep = 0;
        if (t->kept) {
            termfree(e, t);
        }
        return;
    } else if (!t || t->closed) {
        return;
    }
    switch (cmd) {
    case CMD_TERMINPUT: {
        Json    *rj  = jsonget(j, "run");
        b32      run = rj->type == JSON_OBJECT;
        Unbase64 u   = unbase64(&scratch, jsonstr(jsonget(j, "data")));
        if (u.ok && u.data.len && u.data.len<=(run ? TERM_RUNINPUT : TERM_INPUT)) {
            termtyped(e, scratch, t, u.data);  // scans first: the last mark
            t->used = os_ticks(e->os);
            if (run) {
                termrunarm(e, scratch, t, rj);
            } else if (cut(u.data, '\r').ok || cut(u.data, '\n').ok) {
                termrunstep(&t->srun, TRUN_TYPED, {}, t->used);
            }
            os_procwrite(e->os, t->proc, u.data);
        }
    } break;
    case CMD_TERMRESIZE: {
        // The screen takes the output written at the old size first
        PtySize z = termsize(j);
        if (t->screen) {
            termscan(e, t);
            termblocksize(e, &t->scr, t->hist, z);
            t->screen = t->scr.screen;
            termcmdresized(&t->cmds);
        }
        if (t->rscreen.screen) {
            termblocksize(e, &t->rscreen, TERM_RUNHIST, z);
        }
        t->cols = z.cols;
        t->rows = z.rows;
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
        termhangup(e, t, jsonflag(jsonget(j, "kill"), 0), 0);
        break;
    }
}


// Hooks: records, the loop, and what hangs terminals up

static void termnotice(Engine *, Arena, Term *);

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
    if (t->run && !t->told && t->exited && t->eof) {
        termscan(e, t);
        termnotice(e, scratch, t);
    }
    if (t->srun.state && t->exited && t->eof) {
        // Hung up (closed), the screen was read up to its end already
        termscan(e, t);
        i32 why = t->srun.state==TRUN_ENDED ? TEND_DONE : t->closed ? TEND_CUT : TEND_EXIT;
        termrunend(e, scratch, t, why);
    }
    if (t->waitsidlen && t->exited && t->eof) {
        termscan(e, t);
        termwaitheed(e, scratch, t);  // its D may have come in last
        if (t->waitsidlen) {
            termwaitend(e, scratch, t, t->closed ? TEND_CUT : TEND_EXIT);
        }
    }
    if (t->closed) {
        termsettle(e, t);  // frees or keeps it once both are in
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
        termrunheed(e, scratch, t, now);
        if (t->waitseq) {
            termwaitheed(e, scratch, t);
        }
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
            if (!t->waitseq) {
                termwaitheed(e, scratch, t);
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
        if (t->tid && t->srun.state==TRUN_ENDED && t->srun.settleat<next) {
            next = t->srun.settleat;
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
            termhangup(e, t, 0, 0);
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
    Cut   line = loghead(e->os, a, path);
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

// A hooked tab's command running now, else null.
static TermCmd *termcmdrunning(Term *t)
{
    TermCmd *c = t->screen && !t->exited ? termcmdat(&t->cmds, 0) : 0;
    return c && c->state==TC_RUNNING ? c : 0;
}

// A record's line in read_terminal's commands: its command line, folder,
// status and times, then its output, its secrets hidden, clipped to its
// first and last lines within max bytes.
static void termcmdread(Buf *b, Arena *a, Term *t, TermCmd *c, i64 now, iz max,
                        TermSecrets *ss, iz *hidden)
{
    iz h = 0;
    print(b, "`");
    print(b, termmask(a, termcmdstr(c), ss, &h));
    *hidden += h;
    print(b, c->cmdcut ? Str("...`") : Str("`"));
    if (c->cwdlen) {
        print(b, ", in ");
        print(b, termcmdcwd(c));
    }
    if (c->state == TC_RUNNING) {
        print(b, ": still running, for ");
        printelapsed(b, (now - c->startat) / MS);
    } else {
        print(b, c->hascode ? Str(": exited with code ") : Str(": ended"));
        if (c->hascode) {
            print(b, (i64)c->code);
        } else {
            print(b, " (its shell gave no status)");
        }
        print(b, " after ");
        printelapsed(b, (c->endat - c->startat) / MS);
        print(b, ", ");
        printelapsed(b, (now - c->endat) / MS);
        print(b, " ago");
    }
    b32 gone = 0;
    Str out  = termcmdoutput(a, &t->cmds, t->screen, c, &gone);
    if (gone) {
        print(b, ". Its output is no longer in the terminal's history.\n");
        return;
    } else if (!out.len) {
        print(b, c->state==TC_RUNNING ? Str(". Nothing printed yet.\n")
                                      : Str(". It printed nothing.\n"));
        return;
    }
    out = termmask(a, out, ss, &h);
    *hidden += h;
    i64 lines = 1;
    for (iz i = 0; i < out.len; i++) {
        lines += out[i] == '\n';
    }
    Str clip = termclip(a, out, 20, 80, READ_LINEMAX, max);
    print(b, c->state==TC_RUNNING ? Str(". Its output so far (") : Str(". Its output ("));
    print(b, lines);
    print(b, lines==1 ? Str(" line):\n") : Str(" lines):\n"));
    print(b, clip);
}

// read_terminal's commands: the last n records run, oldest first, in
// TERM_READBYTES shared among them.
static void termcmdsread(Buf *b, Arena *a, Term *t, i64 n, i64 now, TermSecrets *ss,
                         iz *hidden)
{
    TermCmd *list[TERMCMD_MAX];
    iz       k = 0;
    for (i64 i = 0; k<n && i<TERMCMD_MAX; i++) {
        TermCmd *c = termcmdat(&t->cmds, i);
        if (c && c->state!=TC_PROMPT) {
            list[k++] = c;
        }
    }
    print(b, "Terminal ");
    print(b, t->tid);
    if (!k) {
        print(b, "'s shell has run no command yet that it still has a record of.\n");
        return;
    }
    print(b, k==1 ? Str("'s last command:\n") : Str("'s last commands, oldest first:\n"));
    iz  max = TERM_READBYTES / k;
    max = max<(2<<10) ? 2<<10 : max;
    for (i64 i = k-1; i >= 0; i--) {
        putbyte(b, '\n');
        if (k > 1) {
            print(b, k-i);
            print(b, ". ");
        }
        termcmdread(b, a, t, list[i], now, max, ss, hidden);
    }
}

// A terminal's line in the list: its number, label, state and folder.
static void termlistline(Buf *b, Arena *a, Term *t, i64 now, TermSecrets *ss, iz *hidden)
{
    iz  h     = 0;
    Str label = t->run      ? termmask(a, {t->runline, t->runlinelen}, ss, &h)
              : t->titlelen ? termmask(a, {t->title, t->titlelen}, ss, &h)
              : Str{t->shell, t->shelllen};
    *hidden += h;
    print(b, "terminal ");
    print(b, t->tid);
    print(b, ": ");
    if (t->run) {
        print(b, "`");
        print(b, label);
        print(b, t->runlines>1 ? Str("`... (the user ran it from your message)")
                               : Str("` (the user ran it from your message)"));
    } else {
        print(b, label.len ? label : Str("shell"));
    }
    if (t->cut) {
        print(b, ", its tab closed while it ran");
    } else if (t->exited && t->signal) {
        print(b, ", killed by signal ");
        print(b, (i64)t->signal);
    } else if (t->exited) {
        print(b, ", exited with code ");
        print(b, (i64)t->code);
    } else if (t->run) {
        print(b, ", running");
    } else if (t->srun.state==TRUN_ARMED || t->srun.state==TRUN_RUNNING) {
        print(b, ", running `");
        print(b, termmask(a, {t->runline, t->runlinelen}, ss, hidden));
        print(b, t->runlines>1 ? Str("`... (the user ran it from your message)")
                               : Str("` (the user ran it from your message)"));
    } else if (termcmdrunning(t)) {
        TermCmd *c = termcmdrunning(t);
        iz       h = 0;
        print(b, ", running `");
        print(b, termmask(a, termcmdstr(c), ss, &h));
        print(b, "` for ");
        printelapsed(b, (now - c->startat) / MS);
        *hidden += h;
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

// A screen's whole text, wrapped rows joined, with the secrets hidden
// before anything cuts it, so that none is cut in two.
static Str termscreentext(Arena *a, ariel_Term *screen, TermSecrets *ss, iz *hidden)
{
    ariel_select(screen, ARIEL_SEL_ALL, 0, 0);
    iz  len = ariel_copy(screen, 0, 0);
    u8 *raw = len>0 ? alloc<u8>(a, len) : 0;
    len     = len>0 ? ariel_copy(screen, raw, len) : 0;
    ariel_select(screen, ARIEL_SEL_CLEAR, 0, 0);
    return termmask(a, trimright(Str{raw, len}), ss, hidden);
}

// The session a Run's notice goes to, as a message would (toolmessage):
// in memory, revived, or loaded as closed; none if it is gone, in the
// Trash, archived, or another process's.
static Session *termnoticeto(Engine *e, Arena scratch, Str sid)
{
    SessScan sc = scansessions(e, &scratch);
    iz       at = -1;
    for (iz i = 0; i < sc.list.len; i++) {
        at = sc.list[i].sid==sid ? i : at;
    }
    if (at<0 || scanaside(&sc, at) || sc.list[at].locked) {
        return 0;
    }
    i64      first = 0;
    b32      fresh = 0;
    Session *s     = findsession(e, sid);
    s = s ? s : revive(e, scratch, sid, {}, {}, &first);
    if (!s) {
        s     = loadsession(e, scratch, sid, {}, {}, &first, -1);
        fresh = s != 0;
    }
    if (s && s->readonly) {
        if (fresh) {
            forget(e, s);
        }
        return 0;
    } else if (s && fresh) {
        s->closed = 1;  // as it was: no frontend has it open
    }
    return s;
}

// A notice "terminal" for a session, at its next request, never
// starting a turn: one of the same Run not yet delivered gives way.
static void termnoticeput(Session *s, Notice n)
{
    n.text = clone(&s->perm, n.text);
    for (iz i = 0; i < s->notices.len; i++) {
        if (s->notices[i].kind=="terminal" && s->notices[i].key==n.key) {
            s->notices[i] = n;
            return;
        }
    }
    s->notices = push(&s->perm, s->notices, n);
}

// A Run ended, or its tab closed while it ran: tell the session whose
// message it came from, at its next request, never starting a turn. A
// notice of the same text not yet delivered gives way to this one.
static void termnotice(Engine *e, Arena scratch, Term *t)
{
    t->told = 1;
    Str sid = {t->from, t->fromlen};
    if (!sid.len || !termagent(e)) {
        return;
    }
    Session *s = termnoticeto(e, scratch, sid);
    if (!s) {
        return;
    }
    TermSecrets *ss     = &e->terms->secrets;
    iz           hidden = 0;
    i64          ms     = (os_ticks(e->os) - t->startat) / MS;
    Buf          b(&scratch, 4096);
    print(&b, "The user ran `");
    print(&b, termmask(&scratch, {t->runline, t->runlinelen}, ss, &hidden));
    print(&b, "`");
    if (t->runlines > 1) {
        print(&b, "... (");
        print(&b, (i64)t->runlines);
        print(&b, " lines)");
    }
    print(&b, " from your message, in terminal ");
    print(&b, t->tid);
    if (t->cwdlen) {
        print(&b, ", in ");
        print(&b, Str{t->cwd, t->cwdlen});
    }
    print(&b, ". ");
    if (t->cut) {
        print(&b, "Its tab was closed while it ran, after ");
    } else if (t->signal) {
        print(&b, "It was killed by signal ");
        print(&b, (i64)t->signal);
        print(&b, ", after ");
    } else {
        print(&b, "It exited with code ");
        print(&b, (i64)t->code);
        print(&b, " after ");
    }
    printelapsed(&b, ms);
    print(&b, ".");
    Str  text = t->screen ? termscreentext(&scratch, t->screen, ss, &hidden) : Str{};
    b32  more = 0;
    Str  last = termlast(&scratch, text, 20, 2<<10, &more);
    if (last.len) {
        print(&b, " Its terminal ends with:\n");
        print(&b, last);
    } else {
        print(&b, "\n");
    }
    if (hidden) {
        print(&b, "[secrets typed at password prompts are hidden as [hidden]]\n");
    }
    if (t->screen && (more || t->kept || !t->closed)) {
        print(&b, "read_terminal reads it while its tab is open.\n");
    }
    Notice n = {};
    n.kind = "terminal";
    n.text = finish(&b);
    n.tid  = t->tid;
    n.code = t->cut ? -1 : t->signal ? 128+t->signal : t->code;
    n.ms   = ms;
    n.key  = t->runkey;
    termnoticeput(s, n);
}

// A Run typed at the shell's prompt is over, as its marks said (why
// TEND_DONE), or not knowing how: its tab closed, or its shell ended.
// Tell the session it came from, as termnotice does, with what it
// printed (its own screen), and let it go.
static void termrunend(Engine *e, Arena scratch, Term *t, i32 why)
{
    PromptRun r = t->srun;
    t->srun = {};
    if (r.state==TRUN_NONE || !termagent(e)) {
        termblockfree(e, &t->rscreen);
        return;
    }
    Session *s = termnoticeto(e, scratch, {t->from, t->fromlen});
    if (!s) {
        termblockfree(e, &t->rscreen);
        return;
    }
    TermSecrets *ss     = &e->terms->secrets;
    iz           hidden = 0;
    i64          now    = os_ticks(e->os);
    b32          begun  = r.state==TRUN_RUNNING || (r.state==TRUN_ENDED && !r.unrun);
    i64          ms     = ((r.state==TRUN_ENDED ? r.endat : now) -
                           (begun ? r.startat : r.armedat)) / MS;
    Buf          b(&scratch, 4096);
    print(&b, "The user ran `");
    print(&b, termmask(&scratch, {t->runline, t->runlinelen}, ss, &hidden));
    print(&b, "`");
    if (t->runlines > 1) {
        print(&b, "... (");
        print(&b, (i64)t->runlines);
        print(&b, " lines)");
    }
    print(&b, " from your message at the prompt of terminal ");
    print(&b, t->tid);
    print(&b, "'s shell. ");
    i32 code = -1;
    if (why==TEND_CUT && !begun) {
        print(&b, "Its tab was closed before it ran.");
    } else if (why==TEND_EXIT && !begun) {
        print(&b, "Its shell ended before it ran.");
    } else if (why == TEND_CUT) {
        print(&b, "Its tab was closed while it ran, after ");
        printelapsed(&b, ms);
        print(&b, ".");
    } else if (why==TEND_EXIT && r.state!=TRUN_ENDED) {
        print(&b, "Its shell ended while it ran, after ");
        printelapsed(&b, ms);
        if (t->exited && !t->signal) {
            print(&b, ", with code ");
            print(&b, (i64)t->code);
        }
        print(&b, ".");
    } else if (r.unrun) {
        print(&b, "The shell ran nothing of it: a syntax error, or a line it took as no "
                  "command.");
    } else if (r.hascode) {
        code = r.code;
        print(&b, "It exited with code ");
        print(&b, (i64)r.code);
        print(&b, " after ");
        printelapsed(&b, ms);
        print(&b, ".");
    } else {
        print(&b, "It ended after ");
        printelapsed(&b, ms);
        print(&b, ", but its shell did not say with what status.");
    }
    Str  text = t->rscreen.screen ? termscreentext(&scratch, t->rscreen.screen, ss, &hidden)
                                  : Str{};
    b32  more = 0;
    Str  last = termlast(&scratch, text, 20, 2<<10, &more);
    if (last.len) {
        print(&b, r.unrun ? Str(" Its terminal shows:\n") : Str(" Its output ends with:\n"));
        print(&b, last);
    } else if (begun) {
        print(&b, " It printed nothing.\n");
    } else {
        print(&b, "\n");
    }
    if (hidden) {
        print(&b, "[secrets typed at password prompts are hidden as [hidden]]\n");
    }
    if (why==TEND_DONE && t->screen && !t->closed) {
        print(&b, "read_terminal reads the terminal while its tab is open.\n");
    }
    termblockfree(e, &t->rscreen);
    Notice n = {};
    n.kind = "terminal";
    n.text = finish(&b);
    n.tid  = t->tid;
    n.code = code;
    n.ms   = ms;
    n.key  = t->runkey;
    termnoticeput(s, n);
}

// A Run's end, if it is over.
static void termrunheed(Engine *e, Arena scratch, Term *t, i64 now)
{
    if (termrundue(&t->srun, now)) {
        termrunend(e, scratch, t, TEND_DONE);
    }
}

// A command a session waited for is over (TEND_DONE), or its shell
// ended, or its tab closed: tell it, and wake it as a job's end does.
static void termwaitend(Engine *e, Arena scratch, Term *t, i32 why)
{
    Str sid = {t->waitsid, t->waitsidlen};
    i64 seq = t->waitseq;
    t->waitsidlen = 0;
    if (!sid.len || !termagent(e)) {
        return;
    }
    Session *s = termnoticeto(e, scratch, sid);
    if (!s) {
        return;
    }
    TermSecrets *ss     = &e->terms->secrets;
    iz           hidden = 0;
    i64          now    = os_ticks(e->os);
    TermCmd     *c      = 0;
    for (iz i = 0; seq && i < t->cmds.n; i++) {
        TermCmd *r = termcmdat(&t->cmds, i);
        c = r->seq==seq ? r : c;
    }
    i32 code = -1;
    Buf b(&scratch, TERM_READBYTES + 2048);
    print(&b, "The command you waited for in terminal ");
    print(&b, t->tid);
    if (why == TEND_CUT) {
        print(&b, " did not finish: its tab was closed while it ran.\n");
    } else if (why == TEND_EXIT) {
        print(&b, " did not finish: its shell ended while it ran");
        if (t->exited && !t->signal) {
            print(&b, ", with code ");
            print(&b, (i64)t->code);
        }
        print(&b, ".\n");
    } else if (c && c->state==TC_DONE) {
        print(&b, " is over: ");
        termcmdread(&b, &scratch, t, c, now, TERM_READBYTES, ss, &hidden);
        code = c->hascode ? c->code : -1;
    } else if (seq) {
        print(&b, " is over, but its record is gone, and with it how it ended.\n");
    } else {
        print(&b, " is over: the terminal is back at its shell's prompt after ");
        printelapsed(&b, (now - t->waitat) / MS);
        print(&b, ". Its shell does not mark its commands, so how it ended is not known.");
    }
    if (t->screen && (why!=TEND_DONE || !(c && c->state==TC_DONE))) {
        iz  h    = 0;
        Str text = termscreentext(&scratch, t->screen, ss, &h);
        hidden  += h;
        b32 more = 0;
        Str last = termlast(&scratch, text, 20, 2<<10, &more);
        if (last.len) {
            print(&b, " Its terminal ends with:\n");
            print(&b, last);
        } else {
            putbyte(&b, '\n');
        }
    }
    if (hidden) {
        print(&b, "[secrets typed at password prompts are hidden as [hidden]]\n");
    }
    if (why==TEND_DONE && !t->closed) {
        print(&b, "read_terminal reads the terminal while its tab is open.\n");
    }
    Notice n = {};
    n.kind = "terminal";
    n.text = finish(&b);
    n.tid  = t->tid;
    n.code = code;
    n.ms   = (now - t->waitat) / MS;
    n.key  = (u64)1<<63 | (u64)t->tid<<32 | (u32)seq;
    termnoticeput(s, n);
    s->wake = 1;
    if (!busy(s) && !s->closed && canresume(s)) {
        autoresume(s, scratch, "terminal");
    } else if (!busy(s) && !s->closed && !e->quitting) {
        Str why = resumespent(s, &scratch, "a terminal command it waited for");
        note(s, scratch, "info", why);
    }
}

// A wait's command, if it is over.
static void termwaitheed(Engine *e, Arena scratch, Term *t)
{
    if (!t->waitsidlen) {
        return;
    } else if (!t->waitseq) {
        if (!t->busy) {
            termwaitend(e, scratch, t, TEND_DONE);
        }
        return;
    }
    TermCmd *c = 0;
    for (iz i = 0; i < t->cmds.n; i++) {
        TermCmd *r = termcmdat(&t->cmds, i);
        c = r->seq==t->waitseq ? r : c;
    }
    if (!c || c->state!=TC_RUNNING) {
        termwaitend(e, scratch, t, TEND_DONE);
    }
}

// The session waits for a terminal's command: it may end its turn.
static b32 termwaiting(Session *s)
{
    Engine *e = s->e;
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (t->tid && t->waitsidlen && Str{t->waitsid, t->waitsidlen}==s->sid) {
            return 1;
        }
    }
    return 0;
}

// Before a request: every Run of the session's that ended goes in now,
// settled or not, so that none misses it.
static void termheed(Session *s, Arena scratch)
{
    Engine *e = s->e;
    for (i32 i = 0; e->terms && i < TERM_MAX; i++) {
        Term *t = &e->terms->slots[i];
        if (!t->tid || !t->srun.state || !(Str{t->from, t->fromlen}==s->sid)) {
            continue;
        }
        termscan(e, t);  // its prompt may be in, not yet read
        if (t->srun.state == TRUN_ENDED) {
            termrunend(e, scratch, t, TEND_DONE);
        }
    }
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
        if (t->tid && (!t->closed || t->kept) && Str{t->sid, t->sidlen}==root) {
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
        termlistline(&b, &scratch, list[i], os_ticks(e->os), ss, &hidden);
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
    if (args->wait) {
        termscan(e, pick);
        TermCmd *c   = termcmdrunning(pick);
        b32      run = pick->screen && !pick->exited && !pick->closed &&
                       (c || (!pick->cmds.hooked && pick->busy));
        if (s->intask) {
            print(&b, "A subagent cannot wait for a terminal: read it instead, "
                      "or report, and leave the waiting to the main agent.");
            Str text = finish(&b);
            toolend(s, scratch, "error", text, 0, 0, {}, 0);
            return;
        } else if (!run) {
            print(&b, "Nothing is running in terminal ");
            print(&b, pick->tid);
            print(&b, pick->exited || pick->closed ? Str(" (its shell has ended)")
                                                   : Str(""));
            print(&b, ", so there is nothing to wait for.\n");
            if (pick->screen && pick->cmds.hooked && !pick->exited) {
                termcmdsread(&b, &scratch, pick, 1, os_ticks(e->os), ss, &hidden);
            }
        } else {
            if (pick->waitsidlen && !(Str{pick->waitsid, pick->waitsidlen}==s->sid)) {
                // Another session waited: it hears nothing now, as if it
                // had not, but this one does
                pick->waitsidlen = 0;
            }
            pick->waitsidlen = s->sid.len<countof(pick->waitsid) ? s->sid.len : 0;
            copybytes(pick->waitsid, s->sid.data, pick->waitsidlen);
            pick->waitseq = c ? c->seq : 0;
            pick->waitat  = os_ticks(e->os);
            print(&b, "Waiting for terminal ");
            print(&b, pick->tid);
            if (c) {
                iz h = 0;
                print(&b, "'s `");
                print(&b, termmask(&scratch, termcmdstr(c), ss, &h));
                hidden += h;
                print(&b, "`, running for ");
                printelapsed(&b, (pick->waitat - c->startat) / MS);
            } else {
                print(&b, "'s program (its shell does not mark its commands, so "
                          "until the terminal is back at its prompt)");
            }
            print(&b, canresume(s)
                      ? Str(". End your turn now, without sleeping or reading it "
                            "again: its end starts a new turn, with how it went.\n")
                      : Str(". This session cannot start a turn by itself now "
                            "([tools] auto_resume), so you hear how it went at "
                            "the user's next prompt: end your turn.\n"));
        }
    } else if (!pick->screen) {
        print(&b, "Terminal ");
        print(&b, pick->tid);
        print(&b, " cannot be read: it opened while [terminal] agent was off.\n");
    } else if (args->commands && pick->cmds.hooked) {
        termscan(e, pick);
        termcmdsread(&b, &scratch, pick, args->commands, os_ticks(e->os), ss, &hidden);
        if (ariel_modes(pick->screen).altscreen) {
            print(&b, "[a full-screen program has the terminal: a command's output "
                      "is what it printed before that program took the screen]\n");
        }
        pick->readfed = pick->fed;
    } else {
        if (args->commands) {
            print(&b, "Terminal ");
            print(&b, pick->tid);
            print(&b, "'s shell does not mark its commands (zsh, bash and PowerShell "
                      "do, started as themselves, with [terminal] integration on), so "
                      "here are its lines instead.\n");
        }
        // Catch up, take the whole text, wrapped rows joined, then hide
        // the secrets before it is cut, so that none is cut in two
        termscan(e, pick);
        iz  h    = 0;
        Str text = termscreentext(&scratch, pick->screen, ss, &h);
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
        i32  st  = pick->srun.state;
        b32  ours = (pick->run || st==TRUN_ARMED || st==TRUN_RUNNING) && pick->fromlen;
        if (ours && !pick->exited && !pick->closed) {
            print(&b, "[it still runs: you are told when it ends, at your next "
                      "request, so do not poll it]\n");
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
