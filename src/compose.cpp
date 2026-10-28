// The compose box's logic, apart from ImGui: slash commands, recalling
// the session's own prompts, a draft's debounce, and the command line
// that opens the draft in an editor
//
// Slash commands are recognized only as the message's first word, and
// only these exact words: anything else that starts with a slash, such
// as a path, is sent as text. So are the session's skills (skills.cpp),
// /name invoking one, unless a command has the name. History counts
// back from the newest of the session's prompts and steers, 1 being the
// newest and 0 the draft itself. A draft goes to the engine (draft) once
// typing has paused for DRAFT_IDLE, and at once when the session is left
// or the GUI quits.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    SLASH_NONE,  // not a command: sent as text
    SLASH_COMPACT,
    SLASH_MODEL,
    SLASH_RENAME,
    SLASH_FORK,
    SLASH_EXPORT,
    SLASH_PLAN,
    SLASH_SIDE,
    SLASH_BACKGROUND,
    SLASH_SHELL,
    SLASH_COUNT,
};

struct SlashInfo {
    Str name;
    Str args;  // as the popup shows them
    Str desc;
};

static SlashInfo slashinfo[] = {
    {},
    {"/compact", "[focus]", "Summarize older messages now, keeping what the focus names"},
    {"/model",   "[NAME]",  "Switch this session's model, or pick one from a list"},
    {"/rename",  "TITLE",   "Give this session a title"},
    {"/fork",    "[TURN]",  "Start a new session from a copy of this one, or of its first turns"},
    {"/export",  "[raw]",   "Save the transcript as Markdown, or its raw log"},
    {"/plan",    "[off]",   "Plan mode: the agent plans for you to approve before it changes anything"},
    {"/side",    "",        "Start a side chat: a copy of this session to ask about it, "
                            "while its agent works on"},
    {"/background", "",     "Let the running command or subagents go on in the background, and the agent with them"},
    {"/shell",   "[NAME]",  "Switch this session's shell, or pick one from a list"},
};
static_assert(countof(slashinfo) == SLASH_COUNT);

struct Slash {
    i32 cmd;    // SLASH_*
    Str skill;  // or the skill it invokes (parseskill)
    Str arg;    // the rest, trimmed
    Str err;    // why the command cannot run as typed, else null
    i64 turn;   // /fork's turn, else -1: the whole session
    b32 raw;    // /export's raw log, not Markdown
    b32 off;    // /plan off: leave plan mode
};

// The command a message is, if its first word is exactly one.
static Slash parseslash(Str text)
{
    Slash r    = {};
    r.turn     = -1;
    Str   t    = trim(text);
    iz    word = 0;
    while (word<t.len && !whitespace(t[word])) word++;
    Str head = takehead(t, word);
    if (!startswith(head, "/")) {
        return r;
    }
    for (i32 i = 1; i < SLASH_COUNT; i++) {
        if (head == slashinfo[i].name) {
            r.cmd = i;
        }
    }
    if (!r.cmd) {
        return r;
    }
    r.arg = trim(cuthead(t, word));
    switch (r.cmd) {
    case SLASH_RENAME:
        if (!r.arg.len) {
            r.err = "/rename needs a title: /rename TITLE";
        }
        break;
    case SLASH_FORK:
        if (r.arg.len) {
            Int n = parseint(r.arg);
            r.turn = n.ok && n.val>=0 ? n.val : -1;
            r.err  = r.turn<0 ? Str("/fork takes a turn's number, or nothing") : Str{};
        }
        break;
    case SLASH_EXPORT:
        r.raw = r.arg=="raw";
        if (r.arg.len && !r.raw && !(r.arg=="md")) {
            r.err = "/export takes raw (the log) or md, or nothing (Markdown)";
        }
        break;
    case SLASH_PLAN:
        r.off = r.arg=="off";
        if (r.arg.len && !r.off && !(r.arg=="on")) {
            r.err = "/plan takes off, or nothing (plan mode on)";
        }
        break;
    case SLASH_SIDE:
        if (r.arg.len) {
            r.err = "/side takes nothing: select text in the transcript to quote it";
        }
        break;
    case SLASH_BACKGROUND:
        if (r.arg.len) {
            r.err = "/background takes nothing";
        }
        break;
    }
    return r;
}

// A side chat's first draft from the text selected: each line quoted
// with "> " (an empty one ">"), then a blank line to write below.
// Nothing selected, nothing.
static Str sidequote(Arena *a, Str text)
{
    for (; text.len && (text[text.len-1]=='\n' || text[text.len-1]=='\r'); text.len--) {}
    if (!text.len) {
        return {};
    }
    Buf b(a, text.len + text.len/4 + 16);
    for (Str rest = text;;) {
        Cut c    = cut(rest, '\n');
        Str line = c.head;
        if (line.len && line[line.len-1]=='\r') {
            line.len--;
        }
        print(&b, line.len ? Str("> ") : Str(">"));
        print(&b, line);
        putbyte(&b, '\n');
        if (!c.ok) {
            break;
        }
        rest = c.tail;
    }
    putbyte(&b, '\n');
    return finish(&b);
}

// The commands a partly typed first word may become, as a mask of bits
// 1<<SLASH_*: while the text is a slash and the start of a name, with
// nothing after it yet. Zero once the word is done or matches nothing.
static u32 slashmatches(Str text)
{
    if (!startswith(text, "/")) {
        return 0;
    }
    for (iz i = 0; i < text.len; i++) {
        if (whitespace(text[i])) {
            return 0;
        }
    }
    u32 m = 0;
    for (i32 i = 1; i < SLASH_COUNT; i++) {
        m |= startswith(slashinfo[i].name, text) ? 1u<<i : 0;
    }
    return m;
}

// The nth command in a mask, in their order (0 the first), else SLASH_NONE.
static i32 slashnth(u32 mask, i32 n)
{
    for (i32 i = 1; i < SLASH_COUNT; i++) {
        if (mask & (1u<<i)) {
            if (!n--) {
                return i;
            }
        }
    }
    return SLASH_NONE;
}

static i32 slashcount(u32 mask)
{
    return __builtin_popcount(mask);
}

// Whether completing the typed word to cmd would change it: Tab and
// Enter complete a word typed in part, and Enter sends a whole one.
static b32 slashcompletes(Str text, i32 cmd)
{
    return cmd && text!=slashinfo[cmd].name;
}


// Skills: /name, as the session's engine lists them (skills)

struct SkillItem {
    Str name;
    Str desc;
    Str path;     // its SKILL.md, empty if built in
    b32 hidden;   // never offered to the agent: the user's alone
    b32 builtin;  // Prospero's own
};

// Whether a command has a skill's name, which it then keeps.
static b32 slashtaken(Str name)
{
    for (i32 i = 1; i < SLASH_COUNT; i++) {
        if (cuthead(slashinfo[i].name, 1) == name) {
            return 1;
        }
    }
    return 0;
}

// The skill a message invokes, by its first word: exactly /name. Its
// arg is the rest, trimmed, and skill null if it invokes none.
static Slash parseskill(Str text, Slice<SkillItem> skills)
{
    Slash r    = {};
    r.turn     = -1;
    Str   t    = trim(text);
    iz    word = 0;
    while (word<t.len && !whitespace(t[word])) word++;
    Str head = takehead(t, word);
    if (!startswith(head, "/") || slashtaken(cuthead(head, 1))) {
        return r;
    }
    for (iz i = 0; i < skills.len; i++) {
        if (cuthead(head, 1) == skills[i].name) {
            r.skill = skills[i].name;
            r.arg   = trim(cuthead(t, word));
        }
    }
    return r;
}

// The slash popup's entries for a partly typed first word, as for
// slashmatches: the commands (SLASH_*), then the skills, each as
// SLASH_COUNT plus its index.
static Slice<i32> slashpicks(Arena *a, Str text, Slice<SkillItem> skills)
{
    Slice<i32> r = {};
    u32        m = slashmatches(text);
    for (i32 i = 1; i < SLASH_COUNT; i++) {
        if (m>>i & 1) {
            r = push(a, r, i);
        }
    }
    if (!startswith(text, "/")) {
        return r;
    }
    Str word = cuthead(text, 1);
    for (iz i = 0; i < word.len; i++) {
        if (whitespace(word[i])) {
            return r;
        }
    }
    for (iz i = 0; i < skills.len; i++) {
        if (startswith(skills[i].name, word) && !slashtaken(skills[i].name)) {
            r = push(a, r, SLASH_COUNT + (i32)i);
        }
    }
    return r;
}

// An entry's word, what it takes, and what it does.
static SlashInfo slashpickinfo(Arena *a, i32 pick, Slice<SkillItem> skills)
{
    if (pick < SLASH_COUNT) {
        return slashinfo[pick];
    }
    SkillItem *k = &skills[pick-SLASH_COUNT];
    SlashInfo  r = {};
    r.name = concat(a, "/", k->name);
    r.args = "[args]";
    r.desc = k->desc.len ? k->desc : Str("(a skill)");
    return r;
}


// History: Up and Down in the compose box

enum : i32 {
    HK_NONE,    // the key does as it would anyway
    HK_START,   // the caret goes to the start of the text
    HK_END,     // or to its end
    HK_RECALL,  // the text becomes history entry pos (0: the empty draft)
};

struct HistKey {
    i32 act;
    i32 pos;
};

// What Up (else Down) does, given the history position (0 the draft,
// count the oldest entry), whether the key could not move the caret
// (edge: on the first visual line for Up, the last for Down), and
// whether the box is empty. On an empty box Up begins recalling the
// session's messages; while recalling, Up at the top goes back further
// and Down at the bottom forward, past the newest to the empty draft.
// Otherwise an edge line's Up goes to the start, and Down to the end.
static HistKey histkey(b32 up, b32 edge, i32 pos, i32 count, b32 empty)
{
    HistKey r = {};
    r.pos = pos;
    if (pos > 0) {
        if (edge && up && pos<count) {
            r.act = HK_RECALL;
            r.pos = pos + 1;
        } else if (edge && !up) {
            r.act = HK_RECALL;
            r.pos = pos - 1;
        }
    } else if (up && empty && count>0) {
        r.act = HK_RECALL;
        r.pos = 1;
    } else if (edge) {
        r.act = up ? HK_START : HK_END;
    }
    return r;
}


// Drafts: the debounce

enum : i64 {
    DRAFT_IDLE = 1000000,  // µs without typing before a draft goes out
};

struct DraftClock {
    i64 due;    // µs: when it goes out, 0 if nothing waits
    b32 dirty;  // changed since it last went out
    b32 typed;  // edited here since the GUI started: no reply loads over it
};

// The text changed here.
static void drafttyped(DraftClock *c, i64 now)
{
    c->dirty = 1;
    c->typed = 1;
    c->due   = now + DRAFT_IDLE;
}

// Whether the draft should go out now: typing paused long enough.
static b32 draftdue(DraftClock *c, i64 now)
{
    return c->dirty && c->due && now>=c->due;
}

// It went out, or need not: a prompt or steer clears the engine's.
static void draftdone(DraftClock *c)
{
    c->dirty = 0;
    c->due   = 0;
}

// Whether open's reply may replace the text: nothing typed over it.
static b32 draftloads(DraftClock *c)
{
    return !c->typed;
}


// The external editor

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

// The command that edits the draft: [ui] editor, else $VISUAL, else
// $EDITOR, else on macOS the default text editor in a new instance,
// which open waits for (it must quit). Null if none.
static Str editorpick(Str setting, Str visual, Str editor, b32 mac)
{
    Str pick[] = {trim(setting), trim(visual), trim(editor)};
    for (iz i = 0; i < countof(pick); i++) {
        if (pick[i].len) {
            return pick[i];
        }
    }
    return mac ? Str("open -W -n -t") : Str{};
}

// The editor as the compose box names it while it runs: its program's
// file name, or the default for open's fallback.
static Str editorname(Str cmd)
{
    if (startswith(cmd, "open -W -n -t")) {
        return "the default text editor";
    }
    Str t = trim(cmd);
    iz  n = 0;
    if (t.len && (t[0]=='"' || t[0]=='\'')) {
        u8 q = t[0];
        t = cuthead(t, 1);
        while (n<t.len && t[n]!=q) n++;
    } else {
        while (n<t.len && !whitespace(t[n])) n++;
    }
    Str prog = takehead(t, n);
    for (iz i = prog.len-1; i >= 0; i--) {
        if (prog[i]=='/' || prog[i]=='\\') {
            prog = cuthead(prog, i+1);
            break;
        }
    }
    return prog.len ? prog : Str("the editor");
}

// The shell command that runs the editor on a file: the command, then
// the path as one more word, quoted for sh, or for cmd.exe in double
// quotes (a Windows path holds none).
static Str editorline(Arena *a, Str cmd, Str path, b32 windows)
{
    Str word = windows ? path : shquote(a, path);
    Buf b(a, cmd.len + word.len + 8);
    print(&b, trim(cmd));
    putbyte(&b, ' ');
    if (windows) {
        putbyte(&b, '"');
    }
    print(&b, word);
    if (windows) {
        putbyte(&b, '"');
    }
    return finish(&b);
}

// An edited file as the new draft: without the newlines editors leave
// at its end.
static Str editortext(Str text)
{
    while (text.len && (text[text.len-1]=='\n' || text[text.len-1]=='\r')) {
        text.len--;
    }
    return text;
}


// An external terminal, opened in a session's folder

enum : i32 {
    TERM_MAC,    // an application's name, for open -a
    TERM_WIN,    // Windows Terminal, else a console of its own
    TERM_LINUX,  // an X11 or Wayland terminal program
};

// How a terminal program takes its folder and the command to run there,
// by its file name. Those not listed take "-e" and only the working
// directory they start in.
struct TermKind {
    Str name;
    Str dir;    // its folder option, "=" ending it when joined
    Str start;  // words ahead of everything else, or null
    Str run;    // ahead of the command's words, or null for none
};

static TermKind termkinds[] = {
    {"ptyxis",         "--working-directory=", "--new-window", "--"},
    {"gnome-terminal", "--working-directory=", {},             "--"},
    {"kgx",            "--working-directory=", {},             "--"},
    {"konsole",        "--workdir",            {},             "-e"},
    {"xfce4-terminal", "--working-directory=", {},             "-x"},
    {"alacritty",      "--working-directory",  {},             "-e"},
    {"kitty",          "--directory",          {},             {}},
    {"wezterm",        "--cwd",                "start",        "--"},
    {"foot",           "--working-directory=", {},             {}},
    {"ghostty",        "--working-directory=", {},             "-e"},
};

struct TermRun {
    Slice<Str> argv;    // to launch, in the folder
    Str        script;  // TERM_MAC with a command: a .command file's text,
                        // whose path goes last in argv
};

// What opens a terminal in dir: prog the terminal (TERM_MAC an
// application, TERM_WIN Windows Terminal's path or null for a console),
// cmd what it runs there, if not the user's shell (ssh, wsl). On macOS
// a command goes in a script that removes itself, which Terminal runs
// without asking for control of it.
static TermRun termargv(Arena *a, i32 os, Str prog, Str dir, Slice<Str> cmd)
{
    TermRun r = {};
    if (os == TERM_MAC) {
        r.argv = push(a, r.argv, Str("open"));
        r.argv = push(a, r.argv, Str("-a"));
        r.argv = push(a, r.argv, prog.len ? prog : Str("Terminal"));
        if (!cmd.len) {
            r.argv = push(a, r.argv, dir);
            return r;
        }
        Buf b(a, 256);
        print(&b, "#!/bin/sh\nrm -f \"$0\"\nexec");
        for (iz i = 0; i < cmd.len; i++) {
            putbyte(&b, ' ');
            print(&b, shquote(a, cmd[i]));
        }
        putbyte(&b, '\n');
        r.script = finish(&b);
        return r;
    }

    Str name = basename(prog);
    if (os==TERM_WIN && name.len>4 && equalfold(cuthead(name, name.len-4), ".exe")) {
        name = takehead(name, name.len-4);
    }
    if (os==TERM_WIN && !prog.len) {
        // A console of its own: the command, else cmd.exe, in dir
        if (cmd.len) {
            r.argv = cmd;
        } else {
            r.argv = push(a, r.argv, Str("cmd.exe"));
        }
        return r;
    } else if (os==TERM_WIN && equalfold(name, "wt")) {
        // Windows Terminal splits its own command line on semicolons
        r.argv = push(a, r.argv, prog);
        r.argv = push(a, r.argv, Str("-d"));
        r.argv = push(a, r.argv, dir);
        for (iz i = 0; i < cmd.len; i++) {
            Buf b(a, cmd[i].len + 8);
            for (iz k = 0; k < cmd[i].len; k++) {
                if (cmd[i][k] == ';') {
                    putbyte(&b, '\\');
                }
                putbyte(&b, cmd[i][k]);
            }
            r.argv = push(a, r.argv, finish(&b));
        }
        return r;
    }

    TermKind k = {name, {}, {}, "-e"};
    for (iz i = 0; i < countof(termkinds); i++) {
        if (termkinds[i].name == name) {
            k = termkinds[i];
        }
    }
    r.argv = push(a, r.argv, prog);
    if (k.start.len) {
        r.argv = push(a, r.argv, k.start);
    }
    if (k.dir.len && k.dir[k.dir.len-1]=='=') {
        r.argv = push(a, r.argv, concat(a, k.dir, dir));
    } else if (k.dir.len) {
        r.argv = push(a, r.argv, k.dir);
        r.argv = push(a, r.argv, dir);
    }
    if (cmd.len && k.run.len) {
        r.argv = push(a, r.argv, k.run);
    }
    for (iz i = 0; i < cmd.len; i++) {
        r.argv = push(a, r.argv, cmd[i]);
    }
    return r;
}

// The command that opens a shell in dir on a remote over ssh: a login
// shell there, or on Windows cmd.exe, in a terminal of its own (-t).
static Slice<Str> termssh(Arena *a, Str ssh, Str dest, Str dir, b32 windows)
{
    Slice<Str> r = {};
    r = push(a, r, ssh);
    r = push(a, r, Str("-t"));
    r = push(a, r, Str("--"));
    r = push(a, r, dest);
    if (windows) {
        r = push(a, r, concat(a, concat(a, "cmd /k cd /d \"", dir), "\""));
    } else {
        r = push(a, r, concat(a, concat(a, "cd ", shquote(a, dir)), " && exec \"$SHELL\" -l"));
    }
    return r;
}

// The command that opens a shell in dir in a WSL distro.
static Slice<Str> termwsl(Arena *a, Str wsl, Str distro, Str dir)
{
    Slice<Str> r = {};
    r = push(a, r, wsl);
    r = push(a, r, Str("-d"));
    r = push(a, r, distro);
    r = push(a, r, Str("--cd"));
    r = push(a, r, dir);
    return r;
}
