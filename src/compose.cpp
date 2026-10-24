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
    }
    return r;
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
    Str path;    // its SKILL.md
    b32 hidden;  // never offered to the agent: the user's alone
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
