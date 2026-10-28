// Instruction files: AGENTS.md and CLAUDE.md
//
// At session start the harness gathers the user's and the project's
// instructions for the agent: the user's own file in the configuration
// directory (~/.config/prospero/AGENTS.md), then one file for each
// directory from the repository root (the nearest ancestor of the
// working directory holding .git, else the working directory itself)
// down to the working directory: the first of the configured names
// found there ([instructions] files, AGENTS.md before CLAUDE.md).
//
// A file may import others with @path, relative to itself or with ~ for
// the home directory, through at most 5 hops, each file once, and never
// from inside a code span or fenced code. A project's files, and what
// they import, must lie within the project with symlinks resolved, so
// that a repository cannot have the user's other files sent to the
// model; the user's own file imports anything.
// The whole text is capped at 64 KiB, cut at a line, with a note.
//
// Files in directories below the working directory load later, when a
// file tool works under one (instrnested): the engine gives them to the
// agent as a note at the next request, so that the system prompt stays
// as it began, byte for byte, for the prompt cache.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    INSTR_MAX   = 64 << 10,  // bytes of instructions in all
    INSTR_HOPS  = 5,         // imports of imports
    INSTR_FILES = 64,        // files at most, imports included
};

struct InstrFile {
    Str path;      // normalized
    Str text;
    Str importer;  // null unless imported
    i64 size;
    i64 mtime;
};

struct Instructions {
    Slice<InstrFile> files;
    Slice<Str>       skipped;  // imports refused: outside the project
    Str              text;     // for the prompt, empty if no files
    b32              cut;      // truncated at INSTR_MAX
};

// The file names to look for, from a list separated by blanks or commas.
static Slice<Str> instrnames(Arena *a, Str list)
{
    Slice<Str> r = {};
    for (iz i = 0; i < list.len;) {
        for (; i<list.len && (whitespace(list[i]) || list[i]==','); i++) {}
        iz j = i;
        for (; j<list.len && !whitespace(list[j]) && list[j]!=','; j++) {}
        if (j > i) {
            Str name = span(list.data+i, list.data+j);
            if (!cut(name, '/').ok && !cut(name, '\\').ok && name!="." && name!="..") {
                r = push(a, r, name);
            }
        }
        i = j;
    }
    return r;
}

// The nearest ancestor of a directory, or itself, holding .git, or the
// directory itself when there is none.
static Str instrroot(Os *os, Arena *a, Str dir, b32 win)
{
    for (Str d = dir;;) {
        Str git = join(a, d, ".git");
        if (os_stat(os, *a, git).type != STAT_MISSING) {
            return d;
        }
        Str up = normpath(a, d, "..", win);
        if (up == d) {
            return dir;
        }
        d = up;
    }
}

// The @path tokens of a text, outside code: each word starting with @,
// with trailing punctuation dropped, that names a path (has a slash or
// a dot, or starts with ~) and holds no other @.
static Slice<Str> instrimports(Arena *a, Str text)
{
    Slice<Str> r     = {};
    u8         fence = 0;  // ` or ~ while inside fenced code
    iz         flen  = 0;
    for (Str rest = text; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str line = c.head;

        // Fences: ``` or ~~~, indented at most 3 spaces
        iz ind = 0;
        for (; ind<line.len && ind<4 && line[ind]==' '; ind++) {}
        iz run = 0;
        if (ind < 4) {
            for (; ind+run<line.len && (line[ind+run]=='`' || line[ind+run]=='~') &&
                   line[ind+run]==line[ind]; run++) {}
        }
        if (run >= 3) {
            if (!fence) {
                fence = line[ind];
                flen  = run;
            } else if (line[ind]==fence && run>=flen) {
                fence = 0;
            }
            continue;
        }
        if (fence) {
            continue;
        }

        for (iz i = 0; i < line.len;) {
            if (line[i] == '`') {
                // A code span runs to the next run of as many backticks;
                // without one, the backticks are plain text
                iz n = 0;
                for (; i+n<line.len && line[i+n]=='`'; n++) {}
                iz  j      = i + n;
                b32 closed = 0;
                while (!closed && j<line.len) {
                    if (line[j] != '`') {
                        j++;
                        continue;
                    }
                    iz m = 0;
                    for (; j+m<line.len && line[j+m]=='`'; m++) {}
                    closed = m == n;
                    j += m;
                }
                i = closed ? j : i+n;
                continue;
            }
            if (line[i]!='@' || (i && !whitespace(line[i-1]) && line[i-1]!='(')) {
                i++;
                continue;
            }
            iz j = i + 1;
            for (; j<line.len && !whitespace(line[j]) && line[j]!='`'; j++) {}
            Str tok = span(line.data+i+1, line.data+j);
            static Str const trail = ".,;:!?)]}>\"'";
            for (; tok.len; tok.len--) {
                u8  last = tok[tok.len-1];
                b32 drop = 0;
                for (iz k = 0; k<trail.len && !drop; k++) {
                    drop = last == trail.data[k];
                }
                if (!drop) break;
            }
            b32 pathy = tok.len && tok[0]=='~';
            b32 at    = 0;
            for (iz k = 0; k < tok.len; k++) {
                pathy |= tok[k]=='/' || tok[k]=='\\' || tok[k]=='.';
                at    |= tok[k] == '@';
            }
            if (pathy && !at) {
                r = push(a, r, tok);
            }
            i = j;
        }
    }
    return r;
}

static b32 instrhas(Instructions *r, Str path)
{
    for (iz i = 0; i < r->files.len; i++) {
        if (r->files[i].path == path) {
            return 1;
        }
    }
    return 0;
}

// Whether a file inside a directory, as its path says, is inside it for
// real, rather than a symlink to somewhere else.
static b32 instrinside(Os *os, Arena scratch, Str inside, Str path, b32 win)
{
    Str real = os_realpath(os, &scratch, path);
    Str top  = os_realpath(os, &scratch, inside);
    if (!real.data) {
        return 0;
    }
    real = normpath(&scratch, path, real, win);
    top  = top.data ? normpath(&scratch, inside, top, win) : inside;
    return within(top, real, win);
}

// Load a file and, after it, what it imports. The file and its imports
// must lie inside that directory, symlinks resolved, unless it is empty.
static void instrload(Os *os, Arena *a, Instructions *r, Str path,
                      Str importer, Str inside, Str home, i32 hops, b32 win)
{
    if (instrhas(r, path) || r->files.len>=INSTR_FILES) {
        return;
    }
    Stat st = os_stat(os, *a, path);
    if (st.type != STAT_FILE) {
        return;
    } else if (inside.len && !instrinside(os, *a, inside, path, win)) {
        r->skipped = push(a, r->skipped, path);
        return;
    }
    Read rd = os_readat(os, a, path, 0, (iz)INSTR_MAX+1);  // more is cut
    if (rd.err) {
        return;
    }
    InstrFile f = {};
    f.path     = path;
    f.text     = rd.data;
    f.importer = importer;
    f.size     = st.size;
    f.mtime    = st.mtime;
    r->files = push(a, r->files, f);
    if (hops >= INSTR_HOPS) {
        return;
    }

    Slice<Str> imports = instrimports(a, rd.data);
    Str        dir     = normpath(a, path, "..", win);
    for (iz i = 0; i < imports.len; i++) {
        Str tok = imports[i];
        Str p   = tok;
        if (tok[0]=='~' && (tok.len==1 || issep(tok[1], win))) {
            if (!home.len) continue;
            p = concat(a, home, cuthead(tok, 1));
        }
        p = normpath(a, dir, p, win);
        if (inside.len && !within(inside, p, win)) {
            r->skipped = push(a, r->skipped, p);
            continue;
        }
        instrload(os, a, r, p, path, inside, home, hops+1, win);
    }
}

// Each file under a heading, capped at INSTR_MAX in all.
static Str instrcompose(Arena *a, Slice<InstrFile> files, Str global, b32 *cut)
{
    Buf b(a, 4096);
    for (iz i = 0; i < files.len; i++) {
        InstrFile *f = &files[i];
        print(&b, "\n\n## ");
        print(&b, f->path);
        if (f->importer.data) {
            print(&b, " (imported by ");
            print(&b, f->importer);
            putbyte(&b, ')');
        } else if (f->path == global) {
            print(&b, " (the user's own instructions)");
        }
        print(&b, "\n\n");
        print(&b, trimright(f->text));
    }
    Str s = finish(&b);
    *cut = 0;
    if (s.len > INSTR_MAX) {
        iz end = INSTR_MAX;
        for (; end>0 && s[end-1]!='\n'; end--) {}
        Buf t(a, end + 128);
        print(&t, takehead(s, end));
        print(&t, "[The instructions were cut here: ");
        print(&t, (i64)(s.len - end));
        print(&t, " more bytes were not loaded.]");
        s    = finish(&t);
        *cut = 1;
    }
    return s;
}

// Gather the instructions for a session in cwd (normalized): global is
// the user's own file, names the file names to look for.
static Instructions instructions(Os *os, Arena *a, Str cwd, Str global,
                                 Slice<Str> names, Str home, b32 win)
{
    Instructions r = {};
    if (global.len) {
        instrload(os, a, &r, global, {}, {}, home, 0, win);
    }

    // The directories from the repository root down to cwd
    Str        root = instrroot(os, a, cwd, win);
    Slice<Str> dirs = {};
    for (Str d = cwd;;) {
        dirs = push(a, dirs, d);
        if (d.len <= root.len) break;
        Str up = normpath(a, d, "..", win);
        if (up == d) break;
        d = up;
    }
    for (iz i = dirs.len-1; i >= 0; i--) {
        for (iz k = 0; k < names.len; k++) {
            Str path = join(a, dirs[i], names[k]);
            if (os_stat(os, *a, path).type == STAT_FILE) {
                instrload(os, a, &r, path, {}, root, home, 0, win);
                break;
            }
        }
    }
    if (r.files.len) {
        r.text = instrcompose(a, r.files, global, &r.cut);
    }
    return r;
}

// The instructions for a file below cwd that are not loaded yet: those
// of each directory from just below cwd down to the file's own, as for
// instructions(), imports included. Loaded lists every file loaded so
// far, which these extend.
static Instructions instrnested(Os *os, Arena *a, Str cwd, Str path,
                                Slice<Str> names, Slice<Str> loaded,
                                Str home, b32 win)
{
    Instructions r = {};
    Str          root  = instrroot(os, a, cwd, win);
    Str          trees = normpath(a, root, ".prospero/worktrees", win);
    if (!within(cwd, path, win) || path.len==cwd.len ||
            (within(trees, path, win) && !within(trees, cwd, win))) {
        return r;  // nor in other sessions' worktrees (worktree.cpp)
    }
    for (iz i = 0; i < loaded.len; i++) {
        InstrFile f = {};
        f.path  = loaded[i];
        r.files = push(a, r.files, f);  // placeholders: never twice
    }
    iz known = r.files.len;

    Slice<Str> dirs = {};
    for (Str d = normpath(a, path, "..", win); d.len>cwd.len && within(cwd, d, win);) {
        dirs = push(a, dirs, d);
        Str up = normpath(a, d, "..", win);
        if (up == d) break;
        d = up;
    }
    for (iz i = dirs.len-1; i >= 0; i--) {
        for (iz k = 0; k < names.len; k++) {
            Str file = join(a, dirs[i], names[k]);
            if (os_stat(os, *a, file).type == STAT_FILE) {
                instrload(os, a, &r, file, {}, root, home, 0, win);
                break;
            }
        }
    }
    Instructions n = {};
    n.skipped = r.skipped;
    n.files   = r.files;
    n.files.data += known;
    n.files.len  -= known;
    n.files.cap   = n.files.len;
    if (n.files.len) {
        n.text = instrcompose(a, n.files, {}, &n.cut);
    }
    return n;
}
