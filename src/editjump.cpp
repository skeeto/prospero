// An editor on a file, at a line: the presets by which known editors
// take one, the command lines that run them, and the login shell that
// starts one in a terminal, apart from the window and the engine, so
// that both build their commands alike.
//
// Open in editor (Files) runs the draft's editor ([ui] editor, $VISUAL,
// then $EDITOR) with the arguments its editor takes for a line, or [ui]
// editor_line, a template of the whole command, through sh -c or
// cmd.exe (editorjump). An editor that runs in a terminal is one kind
// among them (EditJump.term, editorterm). Run in a terminal by the
// engine, the file's path never reaches a shell parser: it is sh -c's
// positional word (editorrun), the command the user's own trusted text.
//
// This is free and unencumbered software released into the public domain.

// A string quoted for sh: one word, whatever it holds.
static Str shquote(Arena *a, Str s)
{
    Buf b(a, s.len + 16);
    putbyte(&b, '\'');
    for (iz i = 0; i < s.len; i++) {
        if (s[i] == '\'') {
            print(&b, "'\\''");
        } else {
            putbyte(&b, s[i]);
        }
    }
    putbyte(&b, '\'');
    return finish(&b);
}

// A command for cmd.exe, which leaves ~ alone, its program in the home
// directory where it starts ~/ or ~\, quoted, since a home directory may
// hold blanks. sh expands ~ itself, so elsewhere this is not needed.
static Str cmdhome(Arena *a, Str cmd, Str home)
{
    cmd = trim(cmd);
    if (!home.len || cmd.len<2 || cmd[0]!='~' || (cmd[1]!='/' && cmd[1]!='\\')) {
        return cmd;
    }
    iz n = 0;
    while (n<cmd.len && !whitespace(cmd[n])) n++;
    Buf b(a, cmd.len + home.len + 4);
    putbyte(&b, '"');
    print(&b, home);
    print(&b, cuthead(takehead(cmd, n), 1));
    putbyte(&b, '"');
    print(&b, cuthead(cmd, n));
    return finish(&b);
}

// How an editor takes a line, after its command
enum : i32 {
    JUMP_NONE,   // unknown: the path alone
    JUMP_PLUS,   // +N F
    JUMP_GOTO,   // -g F:N:C
    JUMP_COLON,  // F:N:C
    JUMP_LINE,   // --line N F
    JUMP_ELL,    // -l N F
    JUMP_NPP,    // -nN F
};

struct JumpKind {
    Str name;  // the program's file name, lowercase, without .exe
    i32 jump;  // JUMP_*
    b32 term;  // it runs in a terminal
};

static JumpKind jumpkinds[] = {
    {"gvim", JUMP_PLUS, 0}, {"mvim", JUMP_PLUS, 0}, {"emacs", JUMP_PLUS, 0},
    {"emacsclient", JUMP_PLUS, 0}, {"gedit", JUMP_PLUS, 0},
    {"code", JUMP_GOTO, 0}, {"code-insiders", JUMP_GOTO, 0},
    {"codium", JUMP_GOTO, 0}, {"cursor", JUMP_GOTO, 0}, {"windsurf", JUMP_GOTO, 0},
    {"zed", JUMP_COLON, 0}, {"zeditor", JUMP_COLON, 0}, {"subl", JUMP_COLON, 0},
    {"idea", JUMP_LINE, 0}, {"idea64", JUMP_LINE, 0}, {"pycharm", JUMP_LINE, 0},
    {"pycharm64", JUMP_LINE, 0}, {"charm", JUMP_LINE, 0}, {"clion", JUMP_LINE, 0},
    {"clion64", JUMP_LINE, 0}, {"goland", JUMP_LINE, 0}, {"goland64", JUMP_LINE, 0},
    {"webstorm", JUMP_LINE, 0}, {"webstorm64", JUMP_LINE, 0},
    {"phpstorm", JUMP_LINE, 0}, {"phpstorm64", JUMP_LINE, 0},
    {"rider", JUMP_LINE, 0}, {"rider64", JUMP_LINE, 0}, {"rubymine", JUMP_LINE, 0},
    {"rubymine64", JUMP_LINE, 0}, {"rustrover", JUMP_LINE, 0},
    {"rustrover64", JUMP_LINE, 0}, {"datagrip", JUMP_LINE, 0},
    {"datagrip64", JUMP_LINE, 0}, {"studio", JUMP_LINE, 0}, {"studio64", JUMP_LINE, 0},
    {"mate", JUMP_ELL, 0}, {"kate", JUMP_ELL, 0},
    {"notepad++", JUMP_NPP, 0},
    {"vi", JUMP_PLUS, 1}, {"vim", JUMP_PLUS, 1}, {"nvim", JUMP_PLUS, 1},
    {"nano", JUMP_PLUS, 1}, {"micro", JUMP_PLUS, 1}, {"kak", JUMP_PLUS, 1},
    {"joe", JUMP_PLUS, 1}, {"mg", JUMP_PLUS, 1}, {"hx", JUMP_COLON, 1},
    {"helix", JUMP_COLON, 1}, {"ne", JUMP_PLUS, 1}, {"nvi", JUMP_PLUS, 1},
    {"vis", JUMP_PLUS, 1}, {"mcedit", JUMP_PLUS, 1},
};

// The next word of a command line, its quotes removed where it is
// quoted whole (as editorname takes it), and *rest past it. Empty at
// the end.
static Str cmdword(Str *rest)
{
    Str t = *rest;
    while (t.len && whitespace(t[0])) {
        t = cuthead(t, 1);
    }
    iz  n = 0;
    Str w = {};
    if (t.len && (t[0]=='"' || t[0]=='\'')) {
        u8 q = t[0];
        for (n = 1; n<t.len && t[n]!=q; n++) {}
        w = takehead(cuthead(t, 1), n-1);
        n += n < t.len;
    }
    for (; n<t.len && !whitespace(t[n]); n++) {}
    *rest = cuthead(t, n);
    return w.data ? w : takehead(t, n);
}

// Whether a word is a VAR=value assignment ahead of a command.
static b32 cmdassign(Str w)
{
    iz i = 0;
    for (; i < w.len; i++) {
        u8 c = w[i];
        if (c == '=') {
            return i > 0;
        } else if (!(c=='_' || (c>='a' && c<='z') || (c>='A' && c<='Z') || (i && c>='0' && c<='9'))) {
            return 0;
        }
    }
    return 0;
}

// The program a command line runs: its first word past any VAR=value
// words, unquoted, and *rest past it.
static Str cmdprog(Str cmd, Str *rest)
{
    Str w = cmdword(&cmd);
    while (cmdassign(w)) {
        w = cmdword(&cmd);
    }
    if (rest) {
        *rest = cmd;
    }
    return w;
}

// An editor by a word's file name, without case, .exe, .cmd or .bat;
// null if it is none known.
static JumpKind *jumpkind(Str word)
{
    Str name = basename(word);
    Str exts[] = {".exe", ".cmd", ".bat"};
    for (iz i = 0; i < countof(exts); i++) {
        if (name.len>exts[i].len && equalfold(cuthead(name, name.len-exts[i].len), exts[i])) {
            name = takehead(name, name.len-exts[i].len);
        }
    }
    for (iz i = 0; i < countof(jumpkinds); i++) {
        if (equalfold(name, jumpkinds[i].name)) {
            return jumpkinds + i;
        }
    }
    return 0;
}

// Whether an editor, known by its program's kind, runs in a terminal,
// given the words after its program: emacs and emacsclient do with a
// terminal's option.
static b32 jumpterm(JumpKind *k, Str rest)
{
    b32 tty = k && k->term;
    if (k && (k->name=="emacs" || k->name=="emacsclient")) {
        for (Str t = rest, w = cmdword(&t); w.len; w = cmdword(&t)) {
            tty |= w=="-nw" || w=="-t" || w=="--tty" || w=="--no-window-system";
        }
    }
    return tty;
}

// Whether an editor's command runs it in a terminal, judged by its
// program past any VAR=value words: "VAR=x vim" and "emacs -nw" do, and
// "alacritty -e nvim" opens a window of its own.
static b32 editorterm(Str cmd)
{
    Str rest = {};
    Str prog = cmdprog(trim(cmd), &rest);
    return jumpterm(jumpkind(prog), rest);
}

// The preset a command takes a line by: its first word naming a known
// editor (so "alacritty -e nvim" gets nvim's), unless its program is
// macOS's open, whose later words are an application's name and files,
// never arguments an editor takes. Null for none.
static JumpKind *jumppreset(Str cmd)
{
    Str t = {};
    Str p = cmdprog(cmd, &t);
    JumpKind *k = 0;
    for (Str w = p; w.len && !k && !equalfold(basename(p), "open"); w = cmdword(&t)) {
        k = jumpkind(w);
    }
    return k;
}

struct EditJump {
    Str line;  // the shell command, or null
    Str prog;  // the program it runs, unquoted, to look for
    Str name;  // its file name, as the user wrote it
    b32 known; // its editor takes a line (else the path alone)
    b32 term;  // its editor runs in a terminal
    Str why;   // why not, else null
};

// A word for the shell: sh's quotes, or cmd.exe's double quotes.
static Str jumpquote(Arena *a, Str s, b32 windows)
{
    return windows ? concat(a, concat(a, "\"", s), "\"") : shquote(a, s);
}

// A template, [ui] editor_line, to b with {file} as file (a word quoted
// already), {line} and {col} as numbers (col 1 for none), and file
// appended when it has no {file}. The template's own quotes followed,
// since {file} inside them would undo its quoting: sh's ' and " (a
// backslash escaping outside '), cmd.exe's ". Why not, else null.
static Str jumpfill(Buf *b, Str tmpl, Str file, i32 line, i32 col, b32 windows)
{
    b32 hasfile = 0;
    b32 sq = 0, dq = 0;
    for (iz i = 0; i < tmpl.len;) {
        Str t = cuthead(tmpl, i);
        if (startswith(t, "{file}") && (sq || dq)) {
            return "[ui] editor_line must not put {file} in quotes: it is quoted already";
        } else if (startswith(t, "{file}")) {
            print(b, file);
            hasfile = 1;
            i += 6;
        } else if (startswith(t, "{line}")) {
            print(b, (i64)line);
            i += 6;
        } else if (startswith(t, "{col}")) {
            print(b, (i64)(col>0 ? col : 1));
            i += 5;
        } else {
            u8 c = tmpl[i++];
            putbyte(b, c);
            if (c=='"' && !sq) {
                dq = !dq;
            } else if (!windows && c=='\'' && !dq) {
                sq = !sq;
            } else if (!windows && c=='\\' && !sq && i<tmpl.len) {
                putbyte(b, tmpl[i++]);
            }
        }
    }
    if (!hasfile) {
        putbyte(b, ' ');
        print(b, file);
    }
    return {};
}

// The shell command that opens path at line (1-based) and col (0 for
// none) in the editor: cmd's preset arguments (jumppreset), else the
// path alone; or tmpl, if not empty, with {file} (a quoted word),
// {line} and {col} in it, the path appended when it has no {file}. An
// editor that runs in a terminal gets its line too, marked term. A path
// cmd.exe cannot pass is refused.
static EditJump editorjump(Arena *a, Str cmd, Str tmpl, Str path, i32 line, i32 col,
                           b32 windows)
{
    EditJump r = {};
    cmd  = trim(cmd);
    tmpl = trim(tmpl);
    Str use = tmpl.len ? tmpl : cmd;
    if (!use.len) {
        r.why = "No editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)";
        return r;
    }
    for (iz i = 0; windows && i < path.len; i++) {
        if (path[i]=='%' || path[i]=='"' || path[i]<0x20 || path[i]==0x7f) {
            r.why = "Its path holds %, \" or a control character, which cmd.exe cannot pass on";
            return r;
        }
    }
    // cmd.exe takes no VAR=value words: its program is the first word
    Str rest = use;
    r.prog = windows ? cmdword(&rest) : cmdprog(use, &rest);
    r.name = basename(r.prog);
    r.term = jumpterm(jumpkind(r.prog), rest);

    Str file = jumpquote(a, path, windows);
    Buf at(a, path.len + 32);  // F:N, or F:N:C
    print(&at, path);
    putbyte(&at, ':');
    print(&at, (i64)line);
    if (col > 0) {
        putbyte(&at, ':');
        print(&at, (i64)col);
    }
    Str fat = jumpquote(a, finish(&at), windows);
    Buf b(a, use.len + 2*path.len + 64);
    if (tmpl.len) {
        r.why = jumpfill(&b, tmpl, file, line, col, windows);
        if (r.why.len) {
            return r;
        }
        r.known = 1;
        r.line  = finish(&b);
        return r;
    }

    if (cmd == "open -W -n -t") {
        // macOS's default text editor, which takes no line
        print(&b, "open -t ");
        print(&b, file);
        r.prog = "open";
        r.name = "the default text editor";
        r.line = finish(&b);
        return r;
    }
    JumpKind *k = jumppreset(cmd);
    print(&b, cmd);
    putbyte(&b, ' ');
    switch (k ? k->jump : (i32)JUMP_NONE) {
    case JUMP_NONE:
        break;
    case JUMP_PLUS:
        putbyte(&b, '+');
        print(&b, (i64)line);
        putbyte(&b, ' ');
        break;
    case JUMP_GOTO:
        print(&b, "-g ");
        file = fat;
        break;
    case JUMP_COLON:
        file = fat;
        break;
    case JUMP_LINE:
        print(&b, "--line ");
        print(&b, (i64)line);
        putbyte(&b, ' ');
        break;
    case JUMP_ELL:
        print(&b, "-l ");
        print(&b, (i64)line);
        putbyte(&b, ' ');
        break;
    case JUMP_NPP:
        print(&b, "-n");
        print(&b, (i64)line);
        putbyte(&b, ' ');
        break;
    }
    print(&b, file);
    r.known = k != 0;
    r.line  = finish(&b);
    return r;
}

// An editorjump for the window, which runs its line with no terminal:
// an editor that runs in one is refused, ahead of any other why its
// template gets.
static EditJump editorwindow(Arena *a, EditJump j)
{
    if (j.term) {
        Buf b(a, 80 + j.name.len);
        print(&b, "[ui] editor must open a window of its own: ");
        print(&b, j.name);
        print(&b, " runs in a terminal");
        j.why = finish(&b);
    }
    return j;
}

// A line for cmd.exe with its first word, the program, replaced by exe,
// the full path found for it, in double quotes and with backslashes:
// given a bare name, cmd.exe looks in the current folder before PATH,
// where anyone who can write a file there picks what runs. Null if exe
// is not a full path, or holds %, " or a control character, which
// cmd.exe cannot pass on.
static Str cmdabs(Arena *a, Str line, Str exe)
{
    if (!pathroot(exe, 1)) {
        return {};
    }
    for (iz i = 0; i < exe.len; i++) {
        if (exe[i]=='%' || exe[i]=='"' || exe[i]<0x20 || exe[i]==0x7f) {
            return {};
        }
    }
    Str rest = line;
    cmdword(&rest);
    Buf b(a, exe.len + rest.len + 2);
    putbyte(&b, '"');
    for (iz i = 0; i < exe.len; i++) {
        putbyte(&b, exe[i]=='/' ? '\\' : exe[i]);
    }
    putbyte(&b, '"');
    print(&b, rest);
    return finish(&b);
}

// The command line by which comspec, cmd.exe, runs a line: /d skips
// AutoRun, and /s takes the line from inside the outer quotes as it is.
static Str cmdcline(Arena *a, Str comspec, Str line)
{
    Buf b(a, 64 + comspec.len + line.len);
    putbyte(&b, '"');
    print(&b, comspec);
    print(&b, "\" /d /s /c \"");
    print(&b, line);
    putbyte(&b, '"');
    return finish(&b);
}

// A terminal editor's command for a host of another OS family, where
// its path and arguments may mean nothing: its known name alone,
// lowercase, without .exe, .cmd or .bat, emacs keeping -nw and
// emacsclient -t. Null for an editor that opens a window, or one not
// known.
static Str editorbare(Str cmd)
{
    Str       rest = {};
    JumpKind *k    = jumpkind(cmdprog(trim(cmd), &rest));
    if (!jumpterm(k, rest)) {
        return {};
    } else if (k->name == "emacs") {
        return "emacs -nw";
    } else if (k->name == "emacsclient") {
        return "emacsclient -t";
    }
    return k->name;
}

// Whether a word reads the same to sh unquoted.
static b32 editorplain(Str w)
{
    for (iz i = 0; i < w.len; i++) {
        u8 c = w[i];
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
              c=='.' || c=='_' || c=='+' || c=='-')) {
            return 0;
        }
    }
    return w.len > 0;
}

// The command with its program word, past any VAR=value words, quoted
// or not, cut to its file name, to look for on PATH: "/opt/x/nvim -p"
// as "nvim -p". Quoted for sh where the name needs it. The command as
// it is when it names no program.
static Str editorrebase(Arena *a, Str cmd)
{
    cmd = trim(cmd);
    Str t = cmd;
    for (;;) {
        Str s = t;
        while (s.len && whitespace(s[0])) {
            s = cuthead(s, 1);
        }
        iz  beg = s.data - cmd.data;  // the word's start, and its end past t
        Str w   = cmdword(&t);
        if (!w.len) {
            return cmd;
        } else if (cmdassign(w)) {
            continue;
        }
        iz end = t.data - cmd.data;
        Str name = basename(w);
        if (!editorplain(name)) {
            name = shquote(a, name);
        }
        Buf b(a, cmd.len + name.len);
        print(&b, takehead(cmd, beg));
        print(&b, name);
        print(&b, cuthead(cmd, end));
        return finish(&b);
    }
}

struct EditRun {
    Str        script;  // for sh -c, the path as $1
    Slice<Str> args;    // its words after $0: the preset's, then the path
    Str        name;    // the editor's file name, as written
    Str        why;     // why not, else null
};

// An editor, cmd or the template tmpl, on path at line (1-based, 0 for
// none) and col (0 for none), as sh -c SCRIPT NAME ARGS..., so that the
// path stays a word no shell parses: without a template, CMD "$@" with
// the preset's words ahead of the path (+N then the path; path:N:C; or
// the path alone for an editor not known, with no line, or with
// nojump, as for busybox's vi, which takes no +N); with a template,
// {file} as "$1", {line} and {col} as numbers, and the path the only
// argument.
static EditRun editorrun(Arena *a, Str cmd, Str tmpl, Str path, i32 line, i32 col,
                         b32 nojump)
{
    EditRun r = {};
    cmd  = trim(cmd);
    tmpl = trim(tmpl);
    Str use = tmpl.len ? tmpl : cmd;
    if (!use.len) {
        r.why = "No editor: set [ui] editor in the settings (or $VISUAL or $EDITOR)";
        return r;
    }
    r.name = basename(cmdprog(use, 0));
    Buf b(a, use.len + 16);
    if (tmpl.len) {
        r.why = jumpfill(&b, tmpl, "\"$1\"", line>0 ? line : 1, col, 0);
        if (r.why.len) {
            return r;
        }
        r.script = finish(&b);
        r.args   = push(a, r.args, path);
        return r;
    }

    print(&b, cmd);
    print(&b, " \"$@\"");
    r.script = finish(&b);
    JumpKind *k = !nojump && line>0 ? jumppreset(cmd) : 0;
    Buf at(a, path.len + 32);  // F:N, or F:N:C
    print(&at, path);
    putbyte(&at, ':');
    print(&at, (i64)line);
    if (col > 0) {
        putbyte(&at, ':');
        print(&at, (i64)col);
    }
    Str fat  = finish(&at);
    Str file = path;
    Buf nb(a, 24);
    print(&nb, (i64)line);
    Str n = finish(&nb);
    switch (k ? k->jump : (i32)JUMP_NONE) {
    case JUMP_NONE:
        break;
    case JUMP_PLUS:
        r.args = push(a, r.args, concat(a, "+", n));
        break;
    case JUMP_GOTO:
        r.args = push(a, r.args, Str("-g"));
        file = fat;
        break;
    case JUMP_COLON:
        file = fat;
        break;
    case JUMP_LINE:
        r.args = push(a, r.args, Str("--line"));
        r.args = push(a, r.args, n);
        break;
    case JUMP_ELL:
        r.args = push(a, r.args, Str("-l"));
        r.args = push(a, r.args, n);
        break;
    case JUMP_NPP:
        r.args = push(a, r.args, concat(a, "-n", n));
        break;
    }
    r.args = push(a, r.args, file);
    return r;
}

// Whether a host has an editor's program, as found answers it
enum : i32 {
    EDITOR_MISSING,  // not there
    EDITOR_FOUND,    // there
    EDITOR_BOX,      // there, as busybox or toybox, whose vi takes no +N
};

// Looks for a program, prog, as a command names it: EDITOR_*.
typedef i32 EditFind(void *ctx, Arena *a, Str prog);

struct EditPick {
    Str cmd;     // the editor's command, else null
    Str tmpl;    // or its template, else null
    b32 nojump;  // it takes the path alone (EDITOR_BOX)
};

// The editor that runs on a host: the window's, its template tmpl if
// set else cmd, then $VISUAL, $EDITOR and vi, the first whose program
// found locates, as written, else by its file name (editorrebase). Both
// null if none is there.
static EditPick editpick(Arena *a, Str cmd, Str tmpl, Str visual, Str editor,
                         EditFind *found, void *ctx)
{
    EditPick r = {};
    tmpl = trim(tmpl);
    Str cands[] = {tmpl.len ? tmpl : trim(cmd), trim(visual), trim(editor), "vi"};
    for (iz i = 0; i < countof(cands); i++) {
        if (!cands[i].len) {
            continue;
        }
        Str ways[] = {cands[i], editorrebase(a, cands[i])};
        for (iz w = 0; w < countof(ways); w++) {
            if (w && ways[w]==ways[0]) {
                break;
            }
            i32 got = found(ctx, a, cmdprog(ways[w], 0));
            if (got != EDITOR_MISSING) {
                if (i==0 && tmpl.len) {
                    r.tmpl = ways[w];
                } else {
                    r.cmd = ways[w];
                }
                r.nojump = got == EDITOR_BOX;
                return r;
            }
        }
    }
    return r;
}

// The login shell's arguments that run cmd, by $SHELL's file name, as
// loginargv takes them, with mksh, ash and yash too: -l -i -c for the
// sh family and fish (-l only with login), interactive as a terminal's
// shell is, since many profiles set PATH in .zshrc or .bashrc; -c alone
// for csh and tcsh, where -l must be the only flag; and pwsh's own,
// which runs it with & and exits with its status. A shell not known,
// or not named by a full path, is /bin/sh. cmd runs in place of the
// shell (exec), so it must mean the same in all of them: words in
// single quotes holding nothing editshellsafe refuses.
static Slice<Str> editshell(Arena *a, Str shell, Str cmd, b32 login)
{
    enum : i32 { ESH_SH, ESH_CSH, ESH_PWSH };
    static struct {
        Str name;
        i32 kind;
    } shells[] = {
        {"sh", ESH_SH}, {"bash", ESH_SH}, {"zsh", ESH_SH}, {"dash", ESH_SH}, {"ksh", ESH_SH},
        {"mksh", ESH_SH}, {"ash", ESH_SH}, {"yash", ESH_SH}, {"fish", ESH_SH},
        {"csh", ESH_CSH}, {"tcsh", ESH_CSH}, {"pwsh", ESH_PWSH},
    };
    Str name = basename(shell);
    i32 kind = -1;
    for (iz i = 0; kind<0 && i<countof(shells); i++) {
        kind = name==shells[i].name ? shells[i].kind : -1;
    }
    if (kind<0 || !shell.len || shell[0]!='/') {
        shell = "/bin/sh";
        kind  = ESH_SH;
    }
    Slice<Str> r = {};
    r = push(a, r, shell);
    switch (kind) {
    case ESH_SH:
        if (login) {
            r = push(a, r, Str("-l"));
        }
        r = push(a, r, Str("-i"));
        r = push(a, r, Str("-c"));
        r = push(a, r, concat(a, "exec ", cmd));
        break;
    case ESH_CSH:
        r = push(a, r, Str("-c"));
        r = push(a, r, concat(a, "exec ", cmd));
        break;
    case ESH_PWSH:
        if (login) {
            r = push(a, r, Str("-Login"));  // it must come first
        }
        r = push(a, r, Str("-NoLogo"));
        r = push(a, r, Str("-Command"));
        r = push(a, r, concat(a, concat(a, "& ", cmd), "; exit $LASTEXITCODE"));
        break;
    }
    return r;
}

// Whether a word, a path, means the same in single quotes to sh, fish,
// csh and pwsh: no single quote (nor pwsh's curly ones, U+2018 to
// U+201B), no backslash, which fish reads, no !, which csh reads, and
// no newline or other control character.
static b32 editshellsafe(Str s)
{
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c=='\'' || c=='\\' || c=='!' || c<0x20 || c==0x7f) {
            return 0;
        } else if (c==0xe2 && i+2<s.len && s[i+1]==0x80 && s[i+2]>=0x98 && s[i+2]<=0x9b) {
            return 0;
        }
    }
    return s.len > 0;
}
