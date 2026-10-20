// Tools: read, write, edit, shell
//
// Every call ends in exactly one result the agent reads. Invalid calls
// are never executed, and their results say how to fix them. File tools
// run synchronously on the engine thread; the shell runs a fresh "sh -c"
// per call under the platform's process layer, with null stdin, pipes,
// and a non-interactive environment.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    TOOL_NONE = -1,
    TOOL_READ,
    TOOL_WRITE,
    TOOL_EDIT,
    TOOL_SHELL,
    TOOL_COUNT,
};

static Str toolnames[] = {"read", "write", "edit", "shell"};
static_assert(countof(toolnames) == TOOL_COUNT);

enum : i64 {
    READ_LINES     = 2000,
    READ_BYTES     = 50 << 10,
    READ_LINEMAX   = 2000,
    FILE_MAX       = (i64)64 << 20,   // largest file read or edited
    SHELL_DEFAULT  = 120,             // seconds
    SHELL_MAX      = 600,
    SHELL_OUTMAX   = (i64)1 << 30,    // output kept in the spill file
    SHELL_HEAD     = 20 << 10,        // inline excerpt
    SHELL_TAIL     = 30 << 10,
};

enum : i32 {
    SHELL_SH,       // argv: SHELL -c COMMAND
    SHELL_BUSYBOX,  // argv: BUSYBOX sh -c COMMAND
    SHELL_CMD,      // cmd.exe /d /s /c "COMMAND"
};

// What the tools need to know about where they run.
struct ToolEnv {
    Str shell;      // absolute path of the shell executable
    i32 shellkind;
    b32 windows;    // path rules and the environment's case
};

static i32 toolfind(Str name)
{
    for (i32 i = 0; i < TOOL_COUNT; i++) {
        if (toolnames[i] == name) {
            return i;
        }
    }
    return TOOL_NONE;
}


// Prompts and schemas, byte-stable for the life of a session

static Str shelldesc(Arena *a, ToolEnv *te)
{
    Buf b(a, 512);
    switch (te->shellkind) {
    case SHELL_SH:
        print(&b, "Run a command with ");
        print(&b, te->shell);
        print(&b, " -c (a POSIX shell)");
        break;
    case SHELL_BUSYBOX:
        print(&b, "Run a command with the busybox-w32 POSIX shell (");
        print(&b, te->shell);
        print(&b, " sh -c), which has the usual Unix tools built in");
        break;
    case SHELL_CMD:
        print(&b, "Run a command with cmd.exe /d /s /c. No POSIX shell was "
                  "found, so use cmd syntax and Windows commands");
        break;
    }
    print(&b, " in the working directory. Each call starts a fresh shell "
              "there, so cd does not carry over between calls. Standard "
              "input is empty and there is no terminal: use non-interactive "
              "flags. GIT_EDITOR is true, so commit with -m. Output (stdout "
              "and stderr together) is cut to its first 20 KiB and last "
              "30 KiB, with the full text saved to a file. The command is "
              "killed after timeout seconds (default 120, at most 600), and "
              "any processes it leaves running are killed when it ends.");
    return finish(&b);
}

// The tools array of a request.
static Str tooljson(Arena *a, ToolEnv *te)
{
    Buf     b(a, 4096);
    JsonOut w = {&b};

    struct Param {
        Str  name;
        Str  type;
        Str  desc;
        b32  required;
    };
    struct Def {
        Str   name;
        Str   desc;
        Param params[4];
    };
    Def defs[] = {
        {
            "read",
            "Read a text file in the working directory, showing each line "
            "with its number and a tab. Long files are shown 2000 lines at "
            "a time; use offset to continue. A directory lists its entries.",
            {
                {"path", "string", "File path, relative to the working directory or absolute", 1},
                {"offset", "integer", "First line to show, from 1", 0},
                {"limit", "integer", "Number of lines to show, at most 2000", 0},
            },
        },
        {
            "write",
            "Create or replace a whole file with the given content, creating "
            "missing directories. Prefer edit for changes to existing files.",
            {
                {"path", "string", "File path, relative to the working directory or absolute", 1},
                {"content", "string", "The complete new file content", 1},
            },
        },
        {
            "edit",
            "Replace text in a file. old_string must match the file exactly, "
            "including indentation (do not include the line numbers that "
            "read shows), and must match exactly one place unless "
            "replace_all is true: include enough surrounding lines to make "
            "it unique. Line endings and any byte order mark are kept.",
            {
                {"path", "string", "File path, relative to the working directory or absolute", 1},
                {"old_string", "string", "The exact text to replace", 1},
                {"new_string", "string", "The replacement text", 1},
                {"replace_all", "boolean", "Replace every match instead of exactly one", 0},
            },
        },
        {
            "shell",
            shelldesc(a, te),
            {
                {"command", "string", "The command to run", 1},
                {"timeout", "integer", "Seconds before the command is killed, 1 to 600 (default 120)", 0},
            },
        },
    };
    static_assert(countof(defs) == TOOL_COUNT);

    putarray(&w);
    for (iz i = 0; i < countof(defs); i++) {
        Def *d = &defs[i];
        putobject(&w);
        putkey(&w, "type");
        putstr(&w, "function");
        putkey(&w, "function");
        putobject(&w);
        putkey(&w, "name");
        putstr(&w, d->name);
        putkey(&w, "description");
        putstr(&w, d->desc);
        putkey(&w, "parameters");
        putobject(&w);
        putkey(&w, "type");
        putstr(&w, "object");
        putkey(&w, "properties");
        putobject(&w);
        for (iz k = 0; k<countof(d->params) && d->params[k].name.len; k++) {
            Param *p = &d->params[k];
            putkey(&w, p->name);
            putobject(&w);
            putkey(&w, "type");
            putstr(&w, p->type);
            putkey(&w, "description");
            putstr(&w, p->desc);
            putend(&w);
        }
        putend(&w);
        putkey(&w, "required");
        putarray(&w);
        for (iz k = 0; k<countof(d->params) && d->params[k].name.len; k++) {
            if (d->params[k].required) {
                putstr(&w, d->params[k].name);
            }
        }
        putend(&w);
        putkey(&w, "additionalProperties");
        putbool(&w, 0);
        putend(&w);
        putend(&w);
        putend(&w);
    }
    putend(&w);
    return finish(&b);
}

static Str systemprompt(Arena *a, Str cwd, ToolEnv *te)
{
    Buf b(a, 2048);
    print(&b,
        "You are a coding agent. You work through tools on the user's "
        "machine: read and change files, run commands, and check your work "
        "by running it. Work until the task is done, then reply with a "
        "short summary of what you did. The user is not watching and "
        "cannot answer questions: make reasonable assumptions and state "
        "them.\n\n"
        "Guidelines:\n"
        "- Read files before editing them, and keep edits small and exact.\n"
        "- Stay inside the working directory; access outside it is denied.\n"
        "- When a tool returns an error, read it, fix the call, and go on. "
        "Never repeat a call that failed in the same way.\n"
        "- Do not narrate what you will do next: do it with a tool call.\n\n"
        "Working directory: ");
    print(&b, cwd);
    print(&b, "\nPlatform: ");
    print(&b, te->windows ? Str("Windows") : Str("POSIX (macOS or Linux)"));
    print(&b, "\n");
    return finish(&b);
}


// Validating arguments
//
// After the syntax repair (repairargs), arguments are checked against
// the tool's parameters with coercions that never invent a value:
// numeric strings become numbers, "true"/"false"/"yes"/"no"/1/0 become
// booleans, and null means absent. Unknown keys are ignored.

struct Args {
    Str        path;
    Str        content;
    Str        oldstr;
    Str        newstr;
    Str        command;
    i64        offset;    // 0 if absent
    i64        limit;     // 0 if absent
    i64        timeout;   // -1 if absent
    b32        replaceall;
    Slice<Str> issues;
    Str        repaired;  // what repairargs fixed, if anything
};

static void issue(Arena *a, Args *r, Str key, Str msg)
{
    Buf b(a, key.len + msg.len + 4);
    print(&b, key);
    print(&b, ": ");
    print(&b, msg);
    r->issues = push(a, r->issues, finish(&b));
}

static Str argstr(Arena *a, Args *r, Json *root, Str key, b32 required)
{
    Json *v = jsonget(root, key);
    if (v->type==JSON_MISSING || v->type==JSON_NULL) {
        if (required) issue(a, r, key, "required string is missing");
        return {};
    } else if (v->type == JSON_STRING) {
        return v->str;
    } else if (v->type==JSON_NUMBER || v->type==JSON_BOOL) {
        return v->raw;  // a scalar written without quotes
    }
    issue(a, r, key, "must be a string");
    return {};
}

// An integer, or dflt if absent.
static i64 argint(Arena *a, Args *r, Json *root, Str key, i64 dflt)
{
    Json *v = jsonget(root, key);
    if (v->type==JSON_MISSING || v->type==JSON_NULL) {
        return dflt;
    }
    Str s = v->type==JSON_STRING ? trim(v->str) :
            v->type==JSON_NUMBER ? v->raw : Str{};
    if (s.len) {
        Float f = parsefloat(s);
        if (f.ok && f.val>=-1e15 && f.val<=1e15 && f.val==(f64)(i64)f.val) {
            return (i64)f.val;
        }
    }
    issue(a, r, key, "must be an integer");
    return dflt;
}

static b32 argbool(Arena *a, Args *r, Json *root, Str key)
{
    Json *v = jsonget(root, key);
    switch (v->type) {
    case JSON_MISSING:
    case JSON_NULL:   return 0;
    case JSON_BOOL:   return v->boolean;
    case JSON_NUMBER:
        if (v->raw=="0" || v->raw=="1") return v->raw=="1";
        break;
    case JSON_STRING: {
        Str s = trim(v->str);
        if (equalfold(s, "true") || equalfold(s, "yes") || s=="1") return 1;
        if (equalfold(s, "false") || equalfold(s, "no") || s=="0") return 0;
    } break;
    }
    issue(a, r, key, "must be true or false");
    return 0;
}

static Args parseargs(Arena *a, i32 tool, Str raw)
{
    Args   r  = {};
    r.timeout = -1;
    Repair rp = repairargs(a, raw);
    r.repaired = rp.how;
    if (rp.root->type != JSON_OBJECT) {
        r.issues = push(a, r.issues, Str("the arguments are not a valid JSON object"));
        return r;
    }
    Json *j = rp.root;
    switch (tool) {
    case TOOL_READ:
        r.path   = argstr(a, &r, j, "path", 1);
        r.offset = argint(a, &r, j, "offset", 0);
        r.limit  = argint(a, &r, j, "limit", 0);
        if (r.offset < 0) issue(a, &r, "offset", "must be 1 or more");
        if (r.limit < 0) issue(a, &r, "limit", "must be 1 or more");
        break;
    case TOOL_WRITE:
        r.path    = argstr(a, &r, j, "path", 1);
        r.content = argstr(a, &r, j, "content", 1);
        break;
    case TOOL_EDIT:
        r.path       = argstr(a, &r, j, "path", 1);
        r.oldstr     = argstr(a, &r, j, "old_string", 1);
        r.newstr     = argstr(a, &r, j, "new_string", 1);
        r.replaceall = argbool(a, &r, j, "replace_all");
        break;
    case TOOL_SHELL:
        r.command = argstr(a, &r, j, "command", 1);
        r.timeout = argint(a, &r, j, "timeout", -1);
        if (r.command.data && !trim(r.command).len) {
            issue(a, &r, "command", "must not be empty");
        }
        if (r.timeout == 0) {
            issue(a, &r, "timeout", "0 is not allowed: give 1 to 600 seconds, "
                                    "or leave it out for the default of 120");
        } else if (r.timeout>SHELL_MAX) {
            r.timeout = SHELL_MAX;  // clamped, not refused
        } else if (r.timeout < -1) {
            issue(a, &r, "timeout", "must be 1 to 600 seconds");
        }
        break;
    }
    if (r.path.data && !r.path.len) {
        issue(a, &r, "path", "must not be empty");
    }
    return r;
}

// The result of an invalid call: what is wrong and how to fix it.
static Str argserror(Arena *a, Str name, Str raw, Args *r)
{
    Buf b(a, 512);
    print(&b, "Invalid arguments for tool \"");
    print(&b, name);
    print(&b, "\", so it was not run:\n");
    for (iz i = 0; i<r->issues.len && i<5; i++) {
        print(&b, "- ");
        print(&b, r->issues[i]);
        putbyte(&b, '\n');
    }
    print(&b, "Arguments received: ");
    print(&b, raw.len>400 ? takehead(raw, 400) : raw);
    if (raw.len > 400) print(&b, "...");
    print(&b, "\nUpdate the arguments and call the tool again.");
    return finish(&b);
}

static Str unknowntool(Arena *a, Str name)
{
    static Str const aliases[][2] = {
        {"bash", "shell"}, {"sh", "shell"}, {"run", "shell"},
        {"exec", "shell"}, {"terminal", "shell"}, {"execute", "shell"},
        {"cat", "read"}, {"view", "read"}, {"open", "read"},
        {"read_file", "read"}, {"create", "write"}, {"write_file", "write"},
        {"str_replace", "edit"}, {"replace", "edit"}, {"patch", "edit"},
        {"edit_file", "edit"}, {"apply_patch", "edit"},
    };
    Buf b(a, 256);
    print(&b, "No tool named \"");
    print(&b, name);
    print(&b, "\" is available, so nothing was run. The tools are: read, "
              "write, edit, shell.");
    for (iz i = 0; i < countof(aliases); i++) {
        Str n = name;
        Cut c = cut(n, '.');  // namespaced, as in "functions.bash"
        if (c.ok) n = c.tail;
        if (equalfold(n, aliases[i][0])) {
            print(&b, " Did you mean \"");
            print(&b, aliases[i][1]);
            print(&b, "\"?");
            break;
        }
    }
    return finish(&b);
}


// Text

static iz countlines(Str s)
{
    iz n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s[i] == '\n';
    }
    return n + (s.len && s[s.len-1]!='\n');
}

// Cut at most max bytes, backing up to a UTF-8 boundary.
static Str utf8head(Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    iz n = max;
    for (; n>0 && (s[n]&0xc0)==0x80; n--) {}
    return takehead(s, n);
}

static Str utf8tail(Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    iz n = s.len - max;
    for (; n<s.len && (s[n]&0xc0)==0x80; n++) {}
    return cuthead(s, n);
}

// Remove ANSI escape sequences and resolve carriage returns the way a
// terminal shows them: within a line, text after the last CR wins. A
// CR before LF is a line ending and simply dropped.
static Str cleanoutput(Arena *a, Str s)
{
    Buf b(a, s.len);
    iz  line = 0;  // where the current line starts in b
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c == 0x1b) {
            if (i+1<s.len && s[i+1]=='[') {
                for (i += 2; i<s.len && (s[i]<0x40 || s[i]>0x7e); i++) {}
            } else if (i+1<s.len && s[i+1]==']') {
                for (i += 2; i < s.len; i++) {
                    if (s[i] == 7) break;
                    if (s[i]==0x1b && i+1<s.len && s[i+1]=='\\') {
                        i++;
                        break;
                    }
                }
            } else {
                i++;  // a two-byte sequence
            }
            continue;
        } else if (c == '\r') {
            if (i+1<s.len && s[i+1]=='\n') {
                continue;
            }
            b.len = line;  // overwrite from the start of the line
            continue;
        }
        putbyte(&b, c);
        if (c == '\n') {
            line = b.len;
        }
    }
    return finish(&b);
}


// Paths

// Display form of a path relative to the working directory, or as is.
static Str showpath(Str cwd, Str path)
{
    if (startswith(path, cwd) && path.len>cwd.len+1 &&
            (path[cwd.len]=='/' || path[cwd.len]=='\\')) {
        return cuthead(path, cwd.len+1);
    }
    return path;
}

static Str parentdir(Str path)
{
    iz i = path.len;
    for (; i>0 && path[i-1]!='/' && path[i-1]!='\\'; i--) {}
    return takehead(path, i>1 ? i-1 : i);
}

static Str basename(Str path)
{
    iz i = path.len;
    for (; i>0 && path[i-1]!='/' && path[i-1]!='\\'; i--) {}
    return cuthead(path, i);
}

static b32 dirless(Dirent **a, Dirent **b)
{
    return compare((*a)->name, (*b)->name) < 0;
}

static Slice<Dirent *> sortedlist(Os *os, Arena *a, Str dir)
{
    Slice<Dirent *> r = {};
    for (Dirent *d = os_list(os, a, dir); d; d = d->next) {
        r = push(a, r, d);
    }
    sort(r.data, r.len, dirless, *a);
    return r;
}

// Edit distance ignoring ASCII case, or limit+1 once past limit.
static iz distance(Arena scratch, Str x, Str y, iz limit)
{
    if (x.len-y.len>limit || y.len-x.len>limit) {
        return limit + 1;
    }
    iz *row = alloc<iz>(&scratch, y.len+1);
    for (iz j = 0; j <= y.len; j++) {
        row[j] = j;
    }
    for (iz i = 1; i <= x.len; i++) {
        iz diag = row[0];
        iz best = row[0] = i;
        for (iz j = 1; j <= y.len; j++) {
            iz up   = row[j];
            iz cost = lowercase(x[i-1]) != lowercase(y[j-1]);
            iz v    = diag + cost;
            v = up+1<v ? up+1 : v;
            v = row[j-1]+1<v ? row[j-1]+1 : v;
            row[j] = v;
            diag   = up;
            best   = v<best ? v : best;
        }
        if (best > limit) {
            return limit + 1;
        }
    }
    return row[y.len];
}

// "File not found" with up to three similar names from its directory:
// close in spelling, or one name a prefix of the other.
static Str notfound(Os *os, Arena *a, Str cwd, Str path)
{
    Str want  = basename(path);
    Str dir   = parentdir(path);
    iz  limit = want.len/4 > 1 ? want.len/4 : 1;
    Slice<Str> like = {};
    Slice<Dirent *> ents = sortedlist(os, a, dir);
    for (iz i = 0; i<ents.len && like.len<3; i++) {
        Str name = ents[i]->name;
        if (distance(*a, name, want, limit) <= limit ||
                (want.len>=3 && startswith(name, want)) ||
                (name.len>=3 && startswith(want, name))) {
            like = push(a, like, showpath(cwd, join(a, dir, name)));
        }
    }

    Buf b(a, 256);
    print(&b, "File not found: ");
    print(&b, showpath(cwd, path));
    for (iz i = 0; i < like.len; i++) {
        print(&b, i ? Str(", ") : Str(". Did you mean: "));
        print(&b, like[i]);
    }
    if (!like.len) {
        print(&b, ". Check the path with the shell (ls) or read the directory.");
    }
    return finish(&b);
}


// read

static Str listdir(Os *os, Arena *a, Str cwd, Str path)
{
    Slice<Dirent *> ents = sortedlist(os, a, path);
    Buf b(a, 1024);
    print(&b, "Directory ");
    print(&b, showpath(cwd, path));
    print(&b, " (");
    print(&b, (i64)ents.len);
    print(&b, " entries):\n");
    for (iz i = 0; i<ents.len && i<500; i++) {
        print(&b, ents[i]->name);
        if (ents[i]->isdir) putbyte(&b, '/');
        putbyte(&b, '\n');
    }
    if (ents.len > 500) {
        print(&b, "... (more entries not shown; use the shell to list them)\n");
    }
    return finish(&b);
}

struct ToolOut {
    Str text;  // what the agent sees
    b32 ok;    // the call did what was asked
};

static ToolOut toolread(Os *os, Arena *a, Str cwd, Str path, Args *args)
{
    ToolOut r  = {};
    Stat    st = os_stat(os, *a, path);
    if (st.type == STAT_MISSING) {
        r.text = notfound(os, a, cwd, path);
        return r;
    } else if (st.type == STAT_DIR) {
        r.text = listdir(os, a, cwd, path);
        r.ok   = 1;
        return r;
    } else if (st.type != STAT_FILE) {
        r.text = "Not a regular file, so it was not read.";
        return r;
    }

    Read rd = os_read(os, a, path, (iz)FILE_MAX);
    if (rd.err == READ_TOOBIG) {
        Buf b(a, 200);
        print(&b, "The file is ");
        print(&b, rd.size);
        print(&b, " bytes, too large to read with this tool. Use the shell "
                  "to look at parts of it (head, tail, sed -n, grep).");
        r.text = finish(&b);
        return r;
    } else if (rd.err) {
        r.text = "The file could not be read.";
        return r;
    }
    Str data = rd.data;
    for (iz i = 0; i<data.len && i<8192; i++) {
        if (!data[i]) {
            Buf b(a, 160);
            print(&b, "This is a binary file (");
            print(&b, (i64)data.len);
            print(&b, " bytes), so it is not shown.");
            r.text = finish(&b);
            return r;
        }
    }
    if (startswith(data, "\xef\xbb\xbf")) {
        data = cuthead(data, 3);
    }
    if (!data.len) {
        r.text = "(empty file)";
        r.ok   = 1;
        return r;
    }

    iz total  = countlines(data);
    iz first  = args->offset ? (iz)args->offset : 1;
    iz limit  = args->limit && args->limit<READ_LINES ? (iz)args->limit : (iz)READ_LINES;
    if (first > total) {
        Buf b(a, 160);
        print(&b, "The file has only ");
        print(&b, (i64)total);
        print(&b, " lines, so offset ");
        print(&b, (i64)first);
        print(&b, " is past its end.");
        r.text = finish(&b);
        r.ok   = 1;
        return r;
    }

    Buf b(a, 4096);
    iz  line = 1;
    iz  last = first - 1;
    b32 full = 0;
    for (Str rest = data; rest.len && line<first+limit; line++) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        if (line < first) {
            continue;
        }
        Str text = c.head;
        if (text.len && text[text.len-1]=='\r') {
            text.len--;
        }
        iz before = b.len;
        i32 width = 1;
        for (iz v = line; v >= 10; v /= 10) width++;
        for (i32 i = width; i < 6; i++) putbyte(&b, ' ');
        print(&b, (i64)line);
        putbyte(&b, '\t');
        if (text.len > READ_LINEMAX) {
            print(&b, utf8head(text, READ_LINEMAX));
            print(&b, "... [line truncated]");
        } else {
            print(&b, text);
        }
        putbyte(&b, '\n');
        if (b.len > READ_BYTES) {
            b.len = before;
            full  = 1;
            break;
        }
        last = line;
    }
    if (last < total) {
        print(&b, "[Showing lines ");
        print(&b, (i64)first);
        putbyte(&b, '-');
        print(&b, (i64)last);
        print(&b, " of ");
        print(&b, (i64)total);
        print(&b, full ? Str(" (output limit reached)") : Str(""));
        print(&b, ". To read more, call read with offset=");
        print(&b, (i64)last+1);
        print(&b, ".]\n");
    }
    r.text = finish(&b);
    r.ok   = 1;
    return r;
}


// write

static ToolOut toolwrite(Os *os, Arena *a, Str cwd, Str path, Args *args)
{
    ToolOut r   = {};
    Stat    st  = os_stat(os, *a, path);
    if (st.type == STAT_DIR) {
        r.text = "That path is a directory: give a file path.";
        return r;
    }
    Str dir = parentdir(path);
    if (dir.len && os_stat(os, *a, dir).type!=STAT_DIR && !os_mkdirs(os, *a, dir)) {
        r.text = "The parent directory could not be created.";
        return r;
    }
    if (!os_write(os, *a, path, args->content)) {
        r.text = "The file could not be written.";
        return r;
    }
    Buf b(a, 160);
    print(&b, st.type==STAT_FILE ? Str("Replaced ") : Str("Created "));
    print(&b, showpath(cwd, path));
    print(&b, " (");
    print(&b, (i64)args->content.len);
    print(&b, " bytes, ");
    print(&b, (i64)countlines(args->content));
    print(&b, " lines).");
    r.text = finish(&b);
    r.ok   = 1;
    return r;
}


// edit
//
// Matching tries, in order: the exact bytes; whole lines compared with
// trailing whitespace ignored; and code points compared after mapping
// typographic quotes, dashes and spaces to ASCII. The first rule that
// matches anything decides.

struct Span {
    iz beg;
    iz end;
};

static Slice<Span> matchexact(Arena *a, Str hay, Str needle)
{
    Slice<Span> r = {};
    for (iz off = 0; off+needle.len <= hay.len;) {
        iz i = find(cuthead(hay, off), needle);
        if (i < 0) break;
        Span s = {off+i, off+i+needle.len};
        r = push(a, r, s);
        off = s.end;
    }
    return r;
}

static Str rtrimline(Str s)
{
    for (; s.len && (s[s.len-1]==' ' || s[s.len-1]=='\t' || s[s.len-1]=='\r'); s.len--) {}
    return s;
}

// Whole lines, ignoring trailing whitespace. A needle ending in a
// newline covers the newline of its last line too.
static Slice<Span> matchlines(Arena *a, Str hay, Str needle)
{
    Slice<Span> r = {};
    b32 nl = needle.len && needle[needle.len-1]=='\n';
    if (nl) needle.len--;
    Slice<Str> want = {};
    for (Str rest = needle;;) {
        Cut c = cut(rest, '\n');
        want = push(a, want, rtrimline(c.head));
        if (!c.ok) break;
        rest = c.tail;
    }

    Slice<Span> lines = {};  // each line without its newline
    for (iz off = 0; off < hay.len;) {
        Cut  c = cut(cuthead(hay, off), '\n');
        Span s = {off, off+c.head.len};
        lines = push(a, lines, s);
        off = s.end + c.ok;
    }
    for (iz i = 0; i+want.len <= lines.len;) {
        b32 ok = 1;
        for (iz k = 0; ok && k<want.len; k++) {
            Span l = lines[i+k];
            ok = rtrimline(Str{hay.data+l.beg, l.end-l.beg}) == want[k];
        }
        if (!ok) {
            i++;
            continue;
        }
        Span s = {lines[i].beg, lines[i+want.len-1].end};
        if (nl && s.end<hay.len) s.end++;
        r = push(a, r, s);
        i += want.len;
    }
    return r;
}

static i32 normcp(i32 c)
{
    switch (c) {
    case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032:
        return '\'';
    case 0x201c: case 0x201d: case 0x201e: case 0x201f: case 0x2033:
        return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212:
        return '-';
    case 0x00a0: case 0x2007: case 0x202f:
        return ' ';
    }
    return c>=0x2000 && c<=0x200a ? ' ' : c;
}

// Code points compared after normcp; spans are bytes of hay.
static Slice<Span> matchnorm(Arena *a, Str hay, Str needle)
{
    Slice<Span> r = {};
    if (!needle.len) return r;
    for (iz beg = 0; beg < hay.len;) {
        iz i = beg, j = 0;
        while (i<hay.len && j<needle.len) {
            iz ii = i, jj = j;
            if (normcp(utf8decode(hay, &ii)) != normcp(utf8decode(needle, &jj))) {
                break;
            }
            i = ii;
            j = jj;
        }
        if (j == needle.len) {
            Span s = {beg, i};
            r = push(a, r, s);
            beg = i;
            continue;
        }
        iz next = beg;
        utf8decode(hay, &next);
        beg = next;
    }
    return r;
}

static iz lineat(Str s, iz off)
{
    iz n = 1;
    for (iz i = 0; i < off; i++) {
        n += s[i] == '\n';
    }
    return n;
}

static Str lfcrlf(Arena *a, Str s)
{
    Buf b(a, s.len + 64);
    for (iz i = 0; i < s.len; i++) {
        if (s[i]=='\n' && (!i || s[i-1]!='\r')) putbyte(&b, '\r');
        putbyte(&b, s[i]);
    }
    return finish(&b);
}

static ToolOut tooledit(Os *os, Arena *a, Str cwd, Str path, Args *args)
{
    ToolOut r    = {};
    Str     show = showpath(cwd, path);

    if (args->oldstr == args->newstr) {
        r.text = "old_string and new_string are the same, so there is "
                 "nothing to change. Give the text to replace in old_string "
                 "and its replacement in new_string.";
        return r;
    }
    Read rd = os_read(os, a, path, (iz)FILE_MAX);
    if (rd.err==READ_NOTFOUND && !args->oldstr.len) {
        Args w = *args;  // a new file
        w.content = args->newstr;
        return toolwrite(os, a, cwd, path, &w);
    }
    if (rd.err == READ_NOTFOUND) {
        r.text = notfound(os, a, cwd, path);
        return r;
    } else if (rd.err == READ_TOOBIG) {
        r.text = "The file is too large to edit with this tool.";
        return r;
    } else if (rd.err) {
        r.text = "The file could not be read.";
        return r;
    }
    if (!args->oldstr.len) {
        r.text = "old_string is empty. Give the exact text to replace; to "
                 "replace the whole file, use write.";
        return r;
    }

    Str data = rd.data;
    Str bom  = {};
    if (startswith(data, "\xef\xbb\xbf")) {
        bom  = takehead(data, 3);
        data = cuthead(data, 3);
    }
    Str oldstr = args->oldstr;
    Str newstr = args->newstr;
    if (find(data, "\r\n")>=0 && find(oldstr, "\r")<0) {
        oldstr = lfcrlf(a, oldstr);
        newstr = lfcrlf(a, newstr);
    }

    Slice<Span> m = matchexact(a, data, oldstr);
    Str how = {};
    if (!m.len) {
        m   = matchlines(a, data, oldstr);
        how = " (matched ignoring trailing whitespace)";
    }
    if (!m.len) {
        m   = matchnorm(a, data, oldstr);
        how = " (matched with typographic quotes and dashes as ASCII)";
    }

    if (!m.len) {
        Buf b(a, 512);
        print(&b, "old_string was not found in ");
        print(&b, show);
        print(&b, ", so nothing was changed.");
        Str first = trim(cut(args->oldstr, '\n').head);
        if (first.len >= 4) {
            for (Str rest = data; rest.len;) {
                Cut c = cut(rest, '\n');
                if (trim(c.head) == first) {
                    print(&b, " Its first line matches line ");
                    print(&b, (i64)lineat(data, c.head.data - data.data));
                    print(&b, ", but the following lines differ.");
                    break;
                }
                rest = c.tail;
            }
        }
        print(&b, " Read the file again and copy the text exactly, without "
                  "line numbers.");
        r.text = finish(&b);
        return r;
    }
    if (m.len>1 && !args->replaceall) {
        Buf b(a, 256);
        print(&b, "old_string matches ");
        print(&b, (i64)m.len);
        print(&b, " places in ");
        print(&b, show);
        print(&b, " (lines");
        for (iz i = 0; i<m.len && i<10; i++) {
            print(&b, i ? Str(", ") : Str(" "));
            print(&b, (i64)lineat(data, m[i].beg));
        }
        print(&b, "), so nothing was changed. Include more surrounding lines "
                  "to make it unique, or set replace_all to true.");
        r.text = finish(&b);
        return r;
    }

    iz  grow = 0;
    for (iz i = 0; i < m.len; i++) {
        grow += newstr.len - (m[i].end - m[i].beg);
    }
    Buf out(a, data.len + bom.len + (grow>0 ? grow : 0) + 64);
    print(&out, bom);
    iz last = 0;
    for (iz i = 0; i < m.len; i++) {
        print(&out, span(data.data+last, data.data+m[i].beg));
        print(&out, newstr);
        last = m[i].end;
    }
    print(&out, cuthead(data, last));
    Str edited = finish(&out);
    if (!os_write(os, *a, path, edited)) {
        r.text = "The file could not be written.";
        return r;
    }

    Buf b(a, 256);
    print(&b, "Edited ");
    print(&b, show);
    print(&b, ": replaced ");
    print(&b, (i64)m.len);
    print(&b, m.len==1 ? Str(" match at line ") : Str(" matches, the first at line "));
    print(&b, (i64)lineat(data, m[0].beg));
    print(&b, how);
    putbyte(&b, '.');
    r.text = finish(&b);
    r.ok   = 1;
    return r;
}


// shell

// The environment overlay for agent commands: never wait for input,
// never page, never color.
static Str const envoverlay[][2] = {
    {"PAGER", "cat"},           {"GIT_PAGER", "cat"},
    {"MANPAGER", "cat"},        {"SYSTEMD_PAGER", "cat"},
    {"BAT_PAGER", "cat"},       {"DELTA_PAGER", "cat"},
    {"GH_PAGER", "cat"},        {"GLAB_PAGER", "cat"},
    {"PSQL_PAGER", "cat"},      {"MYSQL_PAGER", "cat"},
    {"HOMEBREW_PAGER", "cat"},  {"AWS_PAGER", ""},
    {"LESS", "FRX"},            {"TERM", "dumb"},
    {"NO_COLOR", "1"},          {"PYTHONUNBUFFERED", "1"},
    {"GIT_EDITOR", "true"},     {"VISUAL", "true"},
    {"EDITOR", "true"},         {"GIT_TERMINAL_PROMPT", "0"},
    {"CI", "true"},             {"AGENT", "1"},
    {"npm_config_yes", "true"}, {"npm_config_update_notifier", "false"},
    {"npm_config_fund", "false"}, {"npm_config_audit", "false"},
    {"npm_config_progress", "false"},
    {"PNPM_DISABLE_SELF_UPDATE_CHECK", "true"},
    {"PNPM_UPDATE_NOTIFIER", "false"}, {"YARN_ENABLE_TELEMETRY", "0"},
    {"YARN_ENABLE_PROGRESS_BARS", "0"}, {"CARGO_TERM_PROGRESS_WHEN", "never"},
    {"DEBIAN_FRONTEND", "noninteractive"}, {"PIP_NO_INPUT", "1"},
    {"PIP_DISABLE_PIP_VERSION_CHECK", "1"}, {"TF_INPUT", "0"},
    {"TF_IN_AUTOMATION", "1"},  {"GH_PROMPT_DISABLED", "1"},
    {"COMPOSER_NO_INTERACTION", "1"}, {"CLOUDSDK_CORE_DISABLE_PROMPTS", "1"},
    {"HOMEBREW_NO_AUTO_UPDATE", "1"}, {"GCM_INTERACTIVE", "0"},
    {"GIT_SSH_COMMAND", "ssh -o BatchMode=yes"},
    {"SSH_ASKPASS_REQUIRE", "never"}, {"HARNESS", "1"},
};

// Only where unset, and only on Windows.
static Str const envwindows[][2] = {
    {"PYTHONIOENCODING", "utf-8"}, {"PYTHONUTF8", "1"},
    {"LANG", "C.UTF-8"},           {"LC_ALL", "C.UTF-8"},
};

// Inherited variables that would point git at another repository.
static Str const envstrip[] = {
    "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
    "GIT_PREFIX", "GIT_QUARANTINE_PATH",
};

static b32 envnameis(Str entry, Str name, b32 fold)
{
    // Windows keeps per-drive directories as "=C:=C:\x": skip the first =
    iz eq = 1;
    for (; eq<entry.len && entry[eq]!='='; eq++) {}
    Str n = takehead(entry, eq<entry.len ? eq : entry.len);
    return fold ? equalfold(n, name) : n==name;
}

// The complete environment for an agent command.
static Slice<Str> shellenv(Arena *a, Slice<Str> inherited, b32 windows)
{
    Slice<Str> r = {};
    for (iz i = 0; i < inherited.len; i++) {
        Str  e    = inherited[i];
        b32  drop = 0;
        for (iz k = 0; !drop && k<countof(envstrip); k++) {
            drop = envnameis(e, envstrip[k], windows);
        }
        for (iz k = 0; !drop && k<countof(envoverlay); k++) {
            drop = envnameis(e, envoverlay[k][0], windows);
        }
        if (!drop) {
            r = push(a, r, e);
        }
    }
    for (iz k = 0; k < countof(envoverlay); k++) {
        Str e = concat(a, concat(a, envoverlay[k][0], "="), envoverlay[k][1]);
        r = push(a, r, e);
    }
    if (windows) {
        for (iz k = 0; k < countof(envwindows); k++) {
            b32 set = 0;
            for (iz i = 0; !set && i<r.len; i++) {
                set = envnameis(r[i], envwindows[k][0], 1);
            }
            if (!set) {
                Str e = concat(a, concat(a, envwindows[k][0], "="), envwindows[k][1]);
                r = push(a, r, e);
            }
        }
    } else {
        r = push(a, r, Str("SSH_ASKPASS=/usr/bin/false"));
    }
    return r;
}

struct ShellCmd {
    Slice<Str> argv;
    Str        cmdline;  // Windows cmd.exe only
};

static ShellCmd shellcmd(Arena *a, ToolEnv *te, Str command)
{
    ShellCmd r = {};
    switch (te->shellkind) {
    case SHELL_BUSYBOX:
        r.argv = push(a, r.argv, te->shell);
        r.argv = push(a, r.argv, Str("sh"));
        r.argv = push(a, r.argv, Str("-c"));
        r.argv = push(a, r.argv, command);
        break;
    case SHELL_SH:
        r.argv = push(a, r.argv, te->shell);
        r.argv = push(a, r.argv, Str("-c"));
        r.argv = push(a, r.argv, command);
        break;
    case SHELL_CMD: {
        r.argv = push(a, r.argv, te->shell);
        Buf b(a, command.len + te->shell.len + 32);
        putbyte(&b, '"');
        print(&b, te->shell);
        print(&b, "\" /d /s /c \"");
        print(&b, command);
        putbyte(&b, '"');
        r.cmdline = finish(&b);
    } break;
    }
    return r;
}

struct ShellEnd {
    i32  code;      // exit status
    i32  signal;    // terminating signal (POSIX), or 0
    i64  total;     // bytes of output produced
    b32  timedout;
    b32  limited;   // output passed SHELL_OUTMAX
    b32  cut;       // the drain was cut off: descendants held the output
    b32  cancelled;
    i64  timeout;   // seconds
};

// What the agent sees of a finished command: an excerpt of the cleaned
// output and how it ended. Reads the spill file in pieces.
static ToolOut shellresult(Os *os, Arena *a, Str spill, ShellEnd *e)
{
    ToolOut r    = {};
    Read    hd   = os_readat(os, a, spill, 0, SHELL_HEAD+SHELL_TAIL);
    i64     size = hd.err ? 0 : hd.size;
    Str     head = {};
    Str     tail = {};
    if (size <= SHELL_HEAD+SHELL_TAIL) {
        head = cleanoutput(a, hd.data);
    } else {
        Read tl = os_readat(os, a, spill, size-SHELL_TAIL, SHELL_TAIL);
        head = cleanoutput(a, utf8head(hd.data, SHELL_HEAD));
        tail = cleanoutput(a, utf8tail(tl.data, SHELL_TAIL));
    }

    Buf b(a, head.len + tail.len + 512);
    print(&b, head);
    if (size > SHELL_HEAD+SHELL_TAIL) {
        print(&b, "\n... [");
        print(&b, size - SHELL_HEAD - SHELL_TAIL);
        print(&b, " bytes omitted; the full output is in ");
        print(&b, spill);
        print(&b, "] ...\n");
        print(&b, tail);
    }
    if (b.len && b.data[b.len-1]!='\n') {
        putbyte(&b, '\n');
    }

    if (e->cancelled) {
        print(&b, "[cancelled by the user: the command was killed]");
    } else if (e->timedout) {
        print(&b, "[timed out after ");
        print(&b, e->timeout);
        print(&b, " seconds: the command was killed. Run long jobs with a "
                  "larger timeout, or in smaller steps]");
    } else if (e->limited) {
        print(&b, "[the output passed 1 GiB, so the command was killed]");
    } else if (e->signal) {
        print(&b, "[killed by signal ");
        print(&b, (i64)e->signal);
        putbyte(&b, ']');
    } else {
        print(&b, "[exit code ");
        print(&b, (i64)e->code);
        putbyte(&b, ']');
        r.ok = !e->code;
    }
    if (e->cut) {
        print(&b, "\n[processes started by the command kept its output open, "
                  "so they were killed and later output was lost]");
    }
    r.text = finish(&b);
    return r;
}
