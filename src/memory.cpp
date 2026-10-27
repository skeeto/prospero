// Project memory: the agent's notes for later sessions in a project
//
// Instruction files are the user's (instructions.cpp); memory is the
// agent's own. Each project has a directory of it in the state
// directory, <state>/memory/KEY ([memory] path for another), local to
// the machine: MEMORY.md, an index of one line per memory,
//
//     - [NAME](NAME.md) — DESCRIPTION
//
// and NAME.md for each, its text under a little frontmatter. A new
// session in the project gets the index in its system prompt, its
// first 200 lines and 25 KB, and reads a memory's file as its line
// bears on the task. The agent saves and forgets memories with the
// memory tool (engine.cpp), which edits both files under a lock.
//
// KEY is the project's root as Claude Code names its own: the
// repository's root (instrroot), the main checkout's for a worktree, so
// that every worktree of a repository shares one memory, with symlinks
// resolved and each byte but letters, digits, '.', '_' and '-' as '-':
// /home/u/src/app is -home-u-src-app. Windows' is lowercased, since its
// paths are case-blind. A long one keeps its end, after a hash of the
// whole.
//
// The format is Claude Code's, so that either reads the other's.
// Lines the user wrote by hand stay as they are, and so do CRLF line
// endings where the index has them.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    MEM_LINES     = 200,        // index lines in the prompt
    MEM_BYTES     = 25 << 10,   // and bytes
    MEM_NEARLINES = 150,        // past either, saves remind to tidy it
    MEM_NEARBYTES = 20 << 10,
    MEM_READMAX   = 256 << 10,  // the index read, at most
    MEM_KEYMAX    = 200,        // bytes of a project's key
    MEM_LISTMAX   = 50,         // lines of a change listed in a notice
};

// Where a .git file leads: the main checkout of a worktree, else null.
// A worktree's .git file names its git directory, under the main one's
// worktrees/, whose commondir leads back to the main .git; a
// submodule's names one under modules/, with no commondir.
static Str memmain(Os *os, Arena *a, Str root, b32 win)
{
    Str  git = join(a, root, ".git");
    Read rd  = os_read(os, a, git, 4096);
    Str  line = trim(cut(rd.data, '\n').head);
    if (rd.err || !startswith(line, "gitdir:")) {
        return {};
    }
    Str gitdir = normpath(a, root, trim(cuthead(line, 7)), win);
    Read cd    = os_read(os, a, join(a, gitdir, "commondir"), 4096);
    Str  rel   = trim(cut(cd.data, '\n').head);
    if (cd.err || !rel.len) {
        return {};
    }
    Str common = normpath(a, gitdir, rel, win);
    if (basename(common) != ".git") {
        return {};  // a bare repository's: no checkout to name
    }
    return parentdir(common);
}

// The key of the project a directory is in.
static Str memkey(Os *os, Arena *a, Str dir, b32 win)
{
    Str root = instrroot(os, a, dir, win);
    Str git  = join(a, root, ".git");
    if (os_stat(os, *a, git).type == STAT_FILE) {
        Str top = memmain(os, a, root, win);
        root = top.len ? top : root;
    }
    Str real = os_realpath(os, a, root);
    root = real.len ? real : root;

    Str key = clone(a, root);
    for (iz i = 0; i < key.len; i++) {
        u8  c  = key[i];
        b32 ok = (c>='a' && c<='z') || (c>='A' && c<='Z') ||
                 (c>='0' && c<='9') || c=='.' || c=='_' || c=='-';
        c = ok ? c : '-';
        key.data[i] = win && c>='A' && c<='Z' ? (u8)(c + 32) : c;
    }
    if (key.len > MEM_KEYMAX) {
        Buf b(a, MEM_KEYMAX);
        printhex(&b, hash(key), 16);
        putbyte(&b, '-');
        print(&b, cuthead(key, key.len - (MEM_KEYMAX - 17)));
        key = finish(&b);
    }
    return key;
}

// A memory's line in the index.
static Str memline(Arena *a, Str name, Str desc)
{
    Buf b(a, 32 + 2*name.len + desc.len);
    print(&b, "- [");
    print(&b, name);
    print(&b, "](");
    print(&b, name);
    print(&b, ".md) \xe2\x80\x94 ");  // an em dash
    print(&b, desc);
    return finish(&b);
}

// Whether an index line links a memory's file.
static b32 memlinks(Str line, Str name)
{
    for (iz at = 0;;) {
        iz i = find(cuthead(line, at), "](");
        if (i < 0) {
            return 0;
        }
        Str rest = cuthead(line, at+i+2);
        if (startswith(rest, name) && startswith(cuthead(rest, name.len), ".md)")) {
            return 1;
        }
        at += i + 2;
    }
}

struct MemEdit {
    Str text;   // the index after the edit
    b32 found;  // it had a line for the memory
};

// The index with a memory's line put in place of the one linking its
// file, else at the end; with a null line, without any.
static MemEdit memedit(Arena *a, Str index, Str name, Str line)
{
    MemEdit r    = {};
    b32     crlf = find(index, "\r\n") >= 0;
    Buf     b(a, index.len + line.len + 4);
    for (Str rest = index; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        if (memlinks(c.head, name)) {
            if (line.data && !r.found) {
                print(&b, line);
                print(&b, crlf ? Str("\r\n") : Str("\n"));
            }
            r.found = 1;
            continue;
        }
        print(&b, c.head);
        if (c.ok) {
            putbyte(&b, '\n');
        } else {
            print(&b, crlf ? Str("\r\n") : Str("\n"));  // the last line ends
        }
    }
    if (line.data && !r.found) {
        print(&b, line);
        print(&b, crlf ? Str("\r\n") : Str("\n"));
    }
    r.text = finish(&b);
    return r;
}

struct MemCut {
    Str text;   // as much of the index as the prompt takes
    i64 lines;  // in the whole index
    b32 cut;    // text is less than all of it
};

// The index cut to MEM_LINES lines and MEM_BYTES bytes, at a line.
static MemCut memcut(Str index)
{
    MemCut r    = {};
    iz     keep = 0;
    for (Str rest = index; rest.len;) {
        Cut c   = cut(rest, '\n');
        iz  end = c.ok ? c.tail.data - index.data : index.len;
        rest = c.tail;
        r.lines++;
        if (!r.cut && r.lines<=MEM_LINES && end<=MEM_BYTES) {
            keep = end;
        } else {
            r.cut = 1;
        }
    }
    r.text = trimright(takehead(index, keep));
    return r;
}

// Whether the index is past the size a save reminds of (1), or past
// what the prompt takes (2).
static i32 memfull(Str index)
{
    MemCut c = memcut(index);
    if (c.cut) {
        return 2;
    }
    return c.lines>=MEM_NEARLINES || index.len>=MEM_NEARBYTES;
}

static b32 memhasline(Str text, Str line)
{
    for (Str rest = text; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        if (trimright(c.head) == line) {
            return 1;
        }
    }
    return 0;
}

// The lines of one index the other lacks, listed under a heading, at
// most MEM_LISTMAX of them.
static void memlist(Buf *b, Str heading, Str from, Str other)
{
    iz n = 0;
    for (Str rest = from; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str line = trimright(c.head);
        if (!line.len || memhasline(other, line)) {
            continue;
        }
        if (!n) {
            print(b, heading);
        }
        if (n++ < MEM_LISTMAX) {
            print(b, "\n");
            print(b, line);
        }
    }
    if (n > MEM_LISTMAX) {
        print(b, "\n(and ");
        print(b, (i64)(n - MEM_LISTMAX));
        print(b, " more)");
    }
}

// A notice of what changed in the index since the agent knew it as
// before, or null if no line did.
static Str memchanges(Arena *a, Str before, Str after)
{
    Buf b(a, 1024);
    memlist(&b, "\n\nAdded:", after, before);
    memlist(&b, "\n\nRemoved:", before, after);
    if (!b.len) {
        return {};
    }
    Str list = finish(&b);
    return concat(a, "Your memory's index, MEMORY.md, changed since you "
                     "last saw it: another session, or the user, changed "
                     "it. Take it as it is now:", list);
}

// A memory's file: its frontmatter, then its text.
static Str memfile(Arena *a, Str name, Str desc, Str type, i64 ms, Str sid,
                   Str text)
{
    Buf b(a, 256 + name.len + 2*desc.len + text.len);
    print(&b, "---\nname: ");
    print(&b, name);
    print(&b, "\ndescription: \"");
    for (iz i = 0; i < desc.len; i++) {
        if (desc[i]=='"' || desc[i]=='\\') {
            putbyte(&b, '\\');
        }
        putbyte(&b, desc[i]);
    }
    print(&b, "\"\ntype: ");
    print(&b, type);
    print(&b, "\nmodified: ");
    printiso(&b, ms);
    if (sid.len) {
        print(&b, "\nsession: ");
        print(&b, sid);
    }
    print(&b, "\n---\n\n");
    print(&b, trimright(text));
    putbyte(&b, '\n');
    return finish(&b);
}
