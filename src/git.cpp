// Git: a session's branch at a glance, and its changes on request
//
// The branch comes from the repository's files, never from running git:
// githead walks up from the session's directory to a .git, a directory
// or the file of a worktree or submodule that names one, and reads its
// HEAD, as a shell prompt does, along with any operation under way (a
// rebase, a merge). That costs a few stats on the engine thread, as
// instrroot's walk does. A session in a folder of its own looks in that
// folder only, so that a repository around it, say one in the home
// directory, never counts.
//
// The changes come from git itself, run by one worker thread, started
// at the first request for them: git status --porcelain=v2, then git
// diff-index --numstat against HEAD for the lines added and removed,
// and for new files, lines counted by reading them. Neither writes the
// index, so that neither contends with the user's own git: status runs
// with no optional locks, and diff-index, being plumbing, never
// refreshes it, as git diff does even then. git runs in the
// repository's root with fsmonitor off, the agent's environment
// (engineenv: no prompts, no pager, no inherited GIT_DIR), its output
// bounded, and a timeout. Requests queue in the worker's
// mailbox. Each batch takes them all and runs git once for each
// repository, so that every request is answered by a run that began
// after it, and posts the answers to the engine's mailbox (MAIL_GIT),
// whose thread replies to each request in turn.
//
// [tools] git chooses the program, by its path or its name on PATH,
// else git on PATH. On macOS, /usr/bin/git without the developer tools
// counts as missing, since running it offers to install them.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    GIT_WALKMAX    = 256,        // directories walked up to a repository
    GIT_HEADMAX    = 1 << 10,    // bytes of HEAD and the files beside it
    GIT_PACKEDMAX  = 64 << 20,   // bytes of packed-refs
    GIT_OUTMAX     = 4 << 20,    // bytes of git's output kept
    GIT_FILESMAX   = 100000,     // files taken from it, which bounds memory
    GIT_ERRMAX     = 16 << 10,   // bytes of its standard error read
    GIT_STATUSWAIT = 10*SEC,     // longest git status
    GIT_DIFFWAIT   = 5*SEC,      // longest git diff-index
    GIT_DRAIN      = 2*SEC,      // for output after an exit or a kill
    GIT_COUNTFILES = 200,        // new files whose lines are counted
    GIT_COUNTMAX   = 1 << 20,    // bytes of one of them
    GIT_COUNTALL   = 8 << 20,    // and of all of them
    GIT_BINPEEK    = 8 << 10,    // a NUL this early: a binary file
    GIT_REPLYMAX   = 56 << 10,   // bytes of a reply, which a 64 KiB inbox takes
    GIT_PENDING    = 32,         // requests waiting, at most
    GIT_REQBOX     = 512 << 10,  // the worker's mailbox of requests
    GIT_PROCBOX    = 256 << 10,  // and of its children's records
    GIT_REGION     = 64 << 20,   // a batch's arenas, each: room for the most
                                 // output and files, with their index
};

#ifdef __APPLE__
enum { GIT_MACOS = 1 };  // where /usr/bin/git may be a shim
#else
enum { GIT_MACOS = 0 };
#endif


// The branch

struct GitHead {
    b32 repo;
    Str root;      // the working tree's top
    Str gitdir;    // its git directory, a worktree's own
    Str branch;    // without refs/heads/, or another ref whole
    Str oid;       // detached: the commit
    b32 detached;
    b32 reftable;  // the branch is in a reftable, not read here
    Str op;        // under way: rebase, am, merge, cherry-pick, revert, bisect
    i64 step;      // a rebase's or am's: this one of steps
    i64 steps;
};

// A name in a directory, which may be a root ending in '/'.
static Str gitjoin(Arena *a, Str dir, Str name)
{
    return endswith(dir, "/") ? concat(a, dir, name) : join(a, dir, name);
}

// The first line of a file's text, cut at any control character.
static Str gitclean(Str s)
{
    iz n = 0;
    for (; n<s.len && s[n]>=0x20 && s[n]!=0x7f; n++) {}
    return trim(takehead(s, n));
}

// The first line of a small file, cleaned, or null if it cannot be read.
static Str gitline(Os *os, Arena *a, Str path)
{
    Read rd = os_read(os, a, path, GIT_HEADMAX);
    return rd.err ? Str{} : gitclean(rd.data);
}

// A commit's id: SHA-1 or SHA-256, in hex.
static b32 gitoid(Str s)
{
    b32 ok = s.len==40 || s.len==64;
    for (iz i = 0; ok && i<s.len; i++) {
        ok = digit(s[i]) || (s[i]>='a' && s[i]<='f');
    }
    return ok;
}

// The git directory a directory's .git leads to: itself, or the one the
// .git file of a worktree or a submodule names. Null if none.
static Str gitdirat(Os *os, Arena *a, Str dir, b32 win)
{
    Str  git = gitjoin(a, dir, ".git");
    Stat st  = os_stat(os, *a, git);
    if (st.type == STAT_DIR) {
        return git;
    } else if (st.type != STAT_FILE) {
        return {};
    }
    Read rd   = os_read(os, a, git, GIT_HEADMAX);
    Str  line = gitclean(rd.data);
    if (rd.err || !startswith(line, "gitdir:")) {
        return {};
    }
    Str to = trim(cuthead(line, 7));
    return to.len ? normpath(a, dir, to, win) : Str{};
}

// What HEAD says: a branch, another ref, or a detached commit. Anything
// else names nothing.
static void githeadref(GitHead *h, Str head)
{
    Str line = gitclean(head);
    if (startswith(line, "ref:")) {
        Str ref = trim(cuthead(line, 4));
        if (ref == "refs/heads/.invalid") {
            h->reftable = 1;
        } else if (startswith(ref, "refs/heads/")) {
            h->branch = cuthead(ref, 11);
        } else {
            h->branch = ref;
        }
    } else if (gitoid(line)) {
        h->detached = 1;
        h->oid      = line;
    }
}

// A rebase's branch, which its HEAD, detached meanwhile, no longer says.
static void gitrebased(Os *os, Arena *a, GitHead *h, Str dir)
{
    Str name = gitline(os, a, join(a, dir, "head-name"));
    if (startswith(name, "refs/heads/") && name.len>11) {
        h->branch   = cuthead(name, 11);
        h->detached = 0;
        h->oid      = {};
    }
}

static void gitsteps(Os *os, Arena *a, GitHead *h, Str dir, Str at, Str of)
{
    Int step  = parseint(gitline(os, a, join(a, dir, at)));
    Int steps = parseint(gitline(os, a, join(a, dir, of)));
    if (step.ok && steps.ok && step.val>0 && step.val<=steps.val) {
        h->step  = step.val;
        h->steps = steps.val;
    }
}

// An operation under way in the working tree, as git's own prompt
// tells them, with a rebase's branch and steps.
static void gitop(Os *os, Arena *a, GitHead *h)
{
    Str merge = join(a, h->gitdir, "rebase-merge");
    Str apply = join(a, h->gitdir, "rebase-apply");
    if (os_stat(os, *a, merge).type == STAT_DIR) {
        h->op = "rebase";
        gitsteps(os, a, h, merge, "msgnum", "end");
        gitrebased(os, a, h, merge);
    } else if (os_stat(os, *a, apply).type == STAT_DIR) {
        b32 am = os_stat(os, *a, join(a, apply, "applying")).type != STAT_MISSING;
        h->op = am ? Str("am") : Str("rebase");
        gitsteps(os, a, h, apply, "next", "last");
        if (!am) {
            gitrebased(os, a, h, apply);
        }
    } else {
        static Str const marks[][2] = {
            {"MERGE_HEAD", "merge"},   {"CHERRY_PICK_HEAD", "cherry-pick"},
            {"REVERT_HEAD", "revert"}, {"BISECT_LOG", "bisect"},
        };
        for (iz i = 0; i < countof(marks); i++) {
            Str path = join(a, h->gitdir, marks[i][0]);
            if (os_stat(os, *a, path).type == STAT_FILE) {
                h->op = marks[i][1];
                break;
            }
        }
    }
}

// The repository a directory is in, from the directory up to the
// ceiling, if any, else to the root, at most GIT_WALKMAX levels: the
// first whose git directory has a HEAD to read. Its root is written
// with the directory's own separators, so a Windows session's matches
// its cwd. A directory with no drive or share is walked as on POSIX:
// Windows has none, but tests do.
static GitHead githead(Os *os, Arena *a, Str cwd, Str ceiling, b32 win)
{
    GitHead h = {};
    if (!cwd.len) {
        return h;
    }
    win = win && pathroot(cwd, 1);
    Str d   = normpath(a, cwd, ".", win);
    Str top = ceiling.len ? normpath(a, ceiling, ".", win) : Str{};
    for (i32 i = 0; i < GIT_WALKMAX; i++) {
        Str gitdir = gitdirat(os, a, d, win);
        if (gitdir.len) {
            Read rd = os_read(os, a, join(a, gitdir, "HEAD"), GIT_HEADMAX);
            if (rd.err == READ_OK) {
                h.repo   = 1;
                h.root   = d;
                if (win && find(cwd, "/")<0) {
                    h.root = clone(a, d);
                    for (iz j = 0; j < h.root.len; j++) {
                        h.root.data[j] = h.root[j]=='/' ? '\\' : h.root[j];
                    }
                }
                h.gitdir = gitdir;
                githeadref(&h, rd.data);
                gitop(os, a, &h);
                return h;
            }
        }
        if (top.len && (win ? equalfold(d, top) : d==top)) {
            break;
        }
        Str up = normpath(a, d, "..", win);
        if (up == d) {
            break;
        }
        d = up;
    }
    return h;
}

static Str githeadjson(Arena *a, GitHead *h)
{
    Buf     b(a, 256 + h->root.len + h->branch.len);
    JsonOut w = {&b};
    putobject(&w);
    putintkey(&w, "repo", h->repo);
    if (h->repo) {
        putstrkey(&w, "root", h->root);
        if (h->branch.len) {
            putstrkey(&w, "branch", h->branch);
        }
        if (h->detached) {
            putintkey(&w, "detached", 1);
            putstrkey(&w, "oid", h->oid);
        }
        if (h->reftable) {
            putintkey(&w, "reftable", 1);
        }
        if (h->op.len) {
            putstrkey(&w, "op", h->op);
        }
        if (h->steps) {
            putintkey(&w, "step", h->step);
            putintkey(&w, "steps", h->steps);
        }
    }
    putend(&w);
    return finish(&b);
}

// The git directory a repository's worktrees share: the one a worktree's
// own names in its commondir, else the git directory itself.
static Str gitcommon(Os *os, Arena *a, Str gitdir, b32 win)
{
    Str rel = gitline(os, a, join(a, gitdir, "commondir"));
    return rel.len ? normpath(a, gitdir, rel, win) : gitdir;
}

// The commit a ref names (refs/heads/NAME), from the files as git keeps
// them, loose, else packed, or null if neither has it. A reftable's
// refs are not read here.
static Str gitrefoid(Os *os, Arena *a, Str common, Str ref)
{
    Str loose = gitline(os, a, join(a, common, ref));
    if (gitoid(loose)) {
        return loose;
    }
    Read rd = os_read(os, a, join(a, common, "packed-refs"), GIT_PACKEDMAX);
    for (Str rest = rd.err ? Str{} : rd.data; rest.len;) {
        Cut c    = cut(rest, '\n');
        Cut line = cut(gitclean(c.head), ' ');  // "OID REF", or "^OID" peeled
        rest = c.tail;
        if (line.ok && line.tail==ref && gitoid(line.head)) {
            return line.head;
        }
    }
    return {};
}

// One of git worktree list --porcelain's entries.
struct GitTree {
    Str path;
    Str oid;
    Str branch;    // refs/heads/..., null if detached or bare
    b32 bare;
    b32 locked;
    b32 prunable;  // its directory is gone
};

// git worktree list --porcelain: attribute lines, an entry's ended by a
// blank line. Paths point into out.
static Slice<GitTree> gitparsetrees(Arena *a, Str out)
{
    Slice<GitTree> r  = {};
    GitTree        t  = {};
    b32            in = 0;
    for (Str rest = out;;) {
        Cut c    = cut(rest, '\n');
        Str line = c.head;
        rest = c.tail;
        line = endswith(line, "\r") ? takehead(line, line.len-1) : line;
        if (!line.len) {
            if (in) {
                r  = push(a, r, t);
                t  = {};
                in = 0;
            }
            if (!c.ok) {
                return r;
            }
            continue;
        }
        Cut kv = cut(line, ' ');
        if (kv.head == "worktree") {
            if (in) {
                r = push(a, r, t);  // no blank line between
                t = {};
            }
            t.path = kv.tail;
            in     = 1;
        } else if (kv.head == "HEAD") {
            t.oid = kv.tail;
        } else if (kv.head == "branch") {
            t.branch = kv.tail;
        } else if (kv.head == "bare") {
            t.bare = 1;
        } else if (kv.head == "locked") {
            t.locked = 1;
        } else if (kv.head == "prunable") {
            t.prunable = 1;
        }
        if (!c.ok) {
            if (in) {
                r = push(a, r, t);
            }
            return r;
        }
    }
}


// The changes

struct GitFile {
    u8  x, y;      // staged and unstaged status, '.' for none, '?' if new
    u8  kind;      // '1' changed, '2' renamed or copied, 'u' unmerged, '?' new
    Str path;
    Str orig;      // renamed or copied from
    i64 add, del;  // lines, once counted
    b32 counted;
    b32 bin;       // binary: no lines
};

struct GitStatus {
    Str            branch;
    Str            upstream;
    i64            ahead, behind;
    b32            ab;        // ahead and behind are known
    b32            unborn;    // no commit yet
    b32            detached;
    Slice<GitFile> files;     // in git's order
    b32            cut;       // the output was cut short: files missing
    b32            partial;   // some lines were not counted
};

// The rest of a record after n fields, each ended by a space, or null if
// there are fewer.
static Str gitfields(Str rec, i32 n)
{
    iz i = 0;
    for (i32 k = 0; k < n; k++) {
        for (; i<rec.len && rec[i]!=' '; i++) {}
        if (i == rec.len) {
            return {};
        }
        i++;
    }
    return cuthead(rec, i);
}

// git status --porcelain=v2 --branch -z: header lines, then a record for
// each file. A record cut off at the end, as by GIT_OUTMAX, is dropped,
// and files past GIT_FILESMAX are cut. Paths point into out.
static GitStatus gitparsestatus(Arena *a, Str out)
{
    GitStatus r = {};
    for (Str rest = out;;) {
        Cut c = cut(rest, 0);
        if (!c.ok) {
            break;
        } else if (r.files.len == GIT_FILESMAX) {
            r.cut = 1;
            break;
        }
        Str rec = c.head;
        rest = c.tail;

        if (startswith(rec, "# ")) {
            Cut kv = cut(cuthead(rec, 2), ' ');
            Str v  = kv.tail;
            if (kv.head == "branch.oid") {
                r.unborn = v == "(initial)";
            } else if (kv.head == "branch.head") {
                r.detached = v == "(detached)";
                r.branch   = r.detached ? Str{} : v;
            } else if (kv.head == "branch.upstream") {
                r.upstream = v;
            } else if (kv.head == "branch.ab") {
                Cut ab     = cut(v, ' ');
                b32 marked = startswith(ab.head, "+") && startswith(ab.tail, "-");
                Int ahead  = parseint(marked ? cuthead(ab.head, 1) : Str{});
                Int behind = parseint(marked ? cuthead(ab.tail, 1) : Str{});
                if (ahead.ok && behind.ok) {
                    r.ahead  = ahead.val;
                    r.behind = behind.val;
                    r.ab     = 1;
                }
            }
            continue;
        } else if (rec.len<3 || rec[1]!=' ') {
            continue;
        }

        GitFile f = {};
        f.kind = rec[0];
        switch (f.kind) {
        case '1': f.path = gitfields(rec, 8);  break;
        case '2': f.path = gitfields(rec, 9);  break;
        case 'u': f.path = gitfields(rec, 10); break;
        case '?': f.path = cuthead(rec, 2);    break;
        default:  continue;  // '!' ignored, and anything newer
        }
        if (f.kind == '2') {
            Cut o = cut(rest, 0);  // the path it came from
            if (!o.ok) {
                break;
            }
            f.orig = o.head;
            rest   = o.tail;
        }
        if (f.kind == '?') {
            f.x = f.y = '?';
        } else if (rec.len >= 4) {
            f.x = rec[2];
            f.y = rec[3];
        }
        if (f.path.len && f.x && f.y) {
            r.files = push(a, r.files, f);
        }
    }
    return r;
}

// git diff-index --numstat -z: "A\tD\tpath\0", or for a rename
// "A\tD\t\0old\0new\0", with "-\t-" for a binary file. The counts go to
// the status's files by their paths.
static void gitparsenumstat(Arena scratch, Str out, GitStatus *st)
{
    Map<iz> *index = 0;
    for (iz i = 0; i < st->files.len; i++) {
        *upsert(&index, st->files[i].path, &scratch) = i;
    }
    for (Str rest = out;;) {
        Cut add = cut(rest, '\t');
        Cut del = cut(add.tail, '\t');
        if (!add.ok || !del.ok) {
            break;
        }
        rest = del.tail;
        if (rest.len && !rest[0]) {
            Cut from = cut(cuthead(rest, 1), 0);
            rest = from.tail;
        }
        Cut path = cut(rest, 0);
        if (!path.ok) {
            break;
        }
        rest = path.tail;

        iz *i = upsert(&index, path.head, (Arena *)0);
        if (!i) {
            continue;
        }
        GitFile *f = &st->files[*i];
        if (add.head=="-" && del.head=="-") {
            f->bin     = 1;
            f->counted = 1;
            continue;
        }
        Int a = parseint(add.head);
        Int d = parseint(del.head);
        if (a.ok && d.ok) {
            f->add     = a.val;
            f->del     = d.val;
            f->counted = 1;
        }
    }
}

// The lines of a file's text, the last counted without its newline too,
// unless it is binary: a NUL among its first bytes, as git judges.
static i64 gitlines(Str s, b32 *bin)
{
    iz peek = s.len<GIT_BINPEEK ? s.len : (iz)GIT_BINPEEK;
    for (iz i = 0; i < peek; i++) {
        if (!s[i]) {
            *bin = 1;
            return 0;
        }
    }
    *bin = 0;
    i64 n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s[i] == '\n';
    }
    return n + (s.len && s[s.len-1]!='\n');
}

// Lines of the new files, which a diff against HEAD leaves out (all
// of them before the first commit), read from the working tree: regular
// files only, up to GIT_COUNTFILES of them, GIT_COUNTMAX each and
// GIT_COUNTALL in all. A file left uncounted, a new directory included,
// leaves the totals partial.
static void gitcount(Os *os, Arena scratch, Str root, GitStatus *st)
{
    iz  files = 0;
    i64 bytes = 0;
    for (iz i = 0; i < st->files.len; i++) {
        GitFile *f = &st->files[i];
        if (f->counted || f->kind=='u' || (f->kind!='?' && !st->unborn)) {
            continue;
        }
        Arena a    = scratch;
        Str   path = join(&a, root, f->path);
        Stat  s    = os_stat(os, a, path);
        if (s.type==STAT_MISSING && f->kind!='?') {
            f->counted = 1;  // added, then removed: nothing to count
            continue;
        } else if (s.type!=STAT_FILE || files==GIT_COUNTFILES ||
                   s.size>GIT_COUNTMAX || bytes+s.size>GIT_COUNTALL) {
            st->partial = 1;
            continue;
        }
        Read rd = os_read(os, &a, path, GIT_COUNTMAX);
        if (rd.err) {
            st->partial = 1;
            continue;
        }
        files++;
        bytes     += rd.data.len;
        f->add     = gitlines(rd.data, &f->bin);
        f->counted = 1;
    }
}

// The status as JSON, in at most budget bytes: totals over every file,
// then as many of the files as fit, conflicts first, then the other
// tracked ones, then new ones, and how many more there are.
static Str gitstatusjson(Arena *a, Arena scratch, GitStatus *st, i64 ms, iz budget)
{
    i64 adds = 0, dels = 0;
    for (iz i = 0; i < st->files.len; i++) {
        GitFile *f = &st->files[i];
        adds += f->counted ? f->add : 0;
        dels += f->counted ? f->del : 0;
    }
    Buf     b(a, 1024);
    JsonOut w = {&b};
    putobject(&w);
    putintkey(&w, "ok", 1);
    if (st->branch.len) {
        putstrkey(&w, "branch", st->branch);
    }
    if (st->detached) {
        putintkey(&w, "detached", 1);
    }
    if (st->unborn) {
        putintkey(&w, "unborn", 1);
    }
    if (st->upstream.len) {
        putstrkey(&w, "upstream", st->upstream);
    }
    if (st->ab) {
        putintkey(&w, "ahead", st->ahead);
        putintkey(&w, "behind", st->behind);
    }
    putintkey(&w, "count", st->files.len);
    putintkey(&w, "add", adds);
    putintkey(&w, "del", dels);
    if (st->partial) {
        putintkey(&w, "partial", 1);
    }
    if (st->cut) {
        putintkey(&w, "cut", 1);
    }
    putintkey(&w, "ms", ms);

    putkey(&w, "files");
    putarray(&w);
    iz  shown = 0;
    b32 full  = 0;
    for (i32 pass = 0; pass<3 && !full; pass++) {
        for (iz i = 0; i<st->files.len && !full; i++) {
            GitFile *f     = &st->files[i];
            i32      group = f->kind=='u' ? 0 : f->kind=='?' ? 2 : 1;
            if (group != pass) {
                continue;
            }
            Arena   t = scratch;
            Buf     eb(&t, 128 + f->path.len + f->orig.len);
            JsonOut e = {&eb};
            u8      xy[2] = {f->x, f->y};
            putobject(&e);
            putstrkey(&e, "xy", {xy, 2});
            putstrkey(&e, "path", f->path);
            if (f->orig.len) {
                putstrkey(&e, "orig", f->orig);
            }
            if (f->bin) {
                putintkey(&e, "bin", 1);
            } else if (f->counted) {
                putintkey(&e, "add", f->add);
                putintkey(&e, "del", f->del);
            }
            putend(&e);
            Str entry = finish(&eb);
            if (b.len + 1 + entry.len + 32 > budget) {
                full = 1;  // the 32 for the end, more and its count
            } else {
                putraw(&w, entry);
                shown++;
            }
        }
    }
    putend(&w);
    putintkey(&w, "more", st->files.len - shown);
    putend(&w);
    return finish(&b);
}

static Str gitfailjson(Arena *a, Str err)
{
    Buf     b(a, 64 + err.len);
    JsonOut w = {&b};
    putobject(&w);
    putintkey(&w, "ok", 0);
    putstrkey(&w, "err", err);
    putend(&w);
    return finish(&b);
}


// Running git

struct GitProg {
    Str path;  // null if not found
    b32 shim;  // macOS: /usr/bin/git without the developer tools
};

// The git to run: [tools] git by its path, or by its name on path,
// else git on path. On Windows a name without an extension is an .exe.
static GitProg gitprog(Os *os, Arena *perm, Arena scratch, Str setting,
                       Str path, Str home, b32 win, b32 mac)
{
    GitProg r    = {};
    Str     want = trim(setting);
    want = want.len ? expandhome(&scratch, want, home, win) : Str("git");
    if (basename(want) != want) {
        if (os_stat(os, scratch, want).type == STAT_FILE) {
            r.path = clone(perm, want);
        }
    } else {
        if (win && find(want, ".")<0) {
            want = concat(&scratch, want, ".exe");
        }
        r.path = findpath(os, perm, scratch, path, want, win);
    }
    if (mac && r.path=="/usr/bin/git") {
        r.shim = os_stat(os, scratch, "/var/db/xcode_select_link/usr/bin/git").type!=STAT_FILE &&
                 os_stat(os, scratch, "/Library/Developer/CommandLineTools/usr/bin/git").type!=STAT_FILE;
    }
    return r;
}

// Why there is no git to run.
static Str gitmissing(Arena *a, Str setting, b32 shim)
{
    setting = trim(setting);
    if (shim) {
        return "git needs the Command Line Tools (xcode-select --install), "
               "or set [tools] git";
    } else if (!setting.len) {
        return "git was not found on PATH: set [tools] git, or add its "
               "folder to [tools] path_prepend";
    }
    return concat(a, concat(a, "[tools] git, ", setting), ", was not found");
}

// The line of git's standard error that tells why it failed: its first
// fatal or error, else its first line.
static Str gitsaid(Str err)
{
    Str first = {};
    Str error = {};
    for (Str rest = err; rest.len;) {
        Cut c    = cut(rest, '\n');
        Str line = gitclean(c.head);
        rest = c.tail;
        if (startswith(line, "fatal: ")) {
            return trim(cuthead(line, 7));
        } else if (!error.len && startswith(line, "error: ")) {
            error = trim(cuthead(line, 7));
        } else if (!first.len) {
            first = line;
        }
    }
    return error.len ? error : first;
}

struct GitRun {
    Str out;      // standard output, at most GIT_OUTMAX
    Str err;      // standard error, or why it could not start
    i32 code;     // its exit code, -1 if killed or lost
    i32 sig;      // POSIX: the signal that killed it
    b32 spawned;
    b32 timeout;
    b32 cut;      // its output went past GIT_OUTMAX: killed, and out cut
    b32 quit;     // the engine stopped
};

// Why a run of git failed, null if it did not. Output cut short is no
// failure: what came is shown.
static Str giterror(Arena *a, GitRun *r, Str prog, Str what, i64 wait)
{
    if (!r->spawned) {
        Str msg = concat(a, "cannot run ", prog);
        return r->err.len ? concat(a, concat(a, msg, ": "), r->err) : msg;
    } else if (r->timeout) {
        Buf b(a, 64);
        print(&b, "git ");
        print(&b, what);
        print(&b, " took over ");
        print(&b, wait/SEC);
        print(&b, " s: Refresh to try again");
        return finish(&b);
    } else if (r->cut || !r->code) {
        return {};
    } else if (r->code==126 || r->code==127) {
        return concat(a, "cannot run ", prog);
    } else if (r->code == 129) {
        return "git 2.15 or newer is needed";
    }
    Str said = gitsaid(r->err);
    if (said.len) {
        return said;
    }
    Buf b(a, 64);
    if (r->code<0 && r->sig) {
        print(&b, "git was killed by signal ");
        print(&b, (i64)r->sig);
    } else {
        print(&b, "git exited with code ");
        print(&b, (i64)r->code);
    }
    return finish(&b);
}

// git's arguments for its status, else for its lines changed since
// HEAD. Optional locks off, so that status never writes the index for
// its own sake, and fsmonitor off, which would start a daemon. The
// lines come from diff-index, plumbing, which neither refreshes the
// index nor runs external diffs or textconv.
static Slice<Str> gitargv(Arena *a, Str prog, b32 diff)
{
    static Str const status[] = {
        "--no-optional-locks", "-c", "core.fsmonitor=false",
        "status", "--porcelain=v2", "--branch", "-z",
    };
    static Str const numstat[] = {
        "--no-optional-locks", "-c", "core.fsmonitor=false",
        "diff-index", "--numstat", "-z", "-M", "HEAD", "--",
    };
    Str const *args = diff ? numstat : status;
    iz         n    = diff ? countof(numstat) : countof(status);
    Slice<Str> r    = {};
    r = push(a, r, prog);
    for (iz i = 0; i < n; i++) {
        r = push(a, r, args[i]);
    }
    return r;
}

struct GitWork {
    Os      *os;
    Mailbox *req;      // requests, from the engine thread
    Mailbox *procs;    // the records of its children
    Mailbox *out;      // the engine's: answers, as MAIL_GIT
    Region  *perm;     // a batch's, reset for each
    Region  *scratch;
    i32      quit;     // atomic: the engine is stopping
    u64      procid;   // the latest child's
    i32      pending;  // engine thread: requests not yet answered
    b32      started;  // engine thread: its thread runs
    i32      running;  // atomic: a job runs (worktree.cpp)
};

static b32 gitquitting(GitWork *w)
{
    return __atomic_load_n(&w->quit, __ATOMIC_ACQUIRE);
}

// Run git to its end, with its input closed, keeping its output: given
// up on past wait, or once the engine stops, and killed then, or when
// its output passes GIT_OUTMAX. Whatever it leaves behind is killed too.
// Gently, for git that changes things (worktree.cpp): asked to stop
// first, so that it can clean up after itself, then killed GIT_DRAIN
// later, also when the engine stops, which then waits for it.
static GitRun gitrun(GitWork *w, Arena *a, Arena scratch, Slice<Str> argv,
                     Str cwd, Slice<Str> env, Str errpath, i64 wait,
                     b32 gently = 0)
{
    GitRun r  = {};
    Os    *os = w->os;
    r.code = -1;

    Spawn sp = {};
    sp.argv    = argv;
    sp.cwd     = cwd;
    sp.env     = env;
    sp.outpath = errpath;
    sp.mbox    = w->procs;
    sp.id      = ++w->procid;
    Proc *p = os_spawnio(os, &sp, scratch);
    if (!p) {
        r.err = os_lasterr(os, a);
        return r;
    }
    r.spawned = 1;
    os_procwrite(os, p, {});

    Buf out(a, 1<<16);
    i32 code     = -1;
    b32 exited   = 0;
    b32 eof      = 0;
    i32 killed   = 0;  // 1: asked to stop (gently), 2: killed
    i64 deadline = os_ticks(os) + wait;
    while (!exited || !eof) {
        i64 now  = os_ticks(os);
        b32 quit = gitquitting(w);
        r.quit |= quit;
        if (quit && !gently) {
            break;
        } else if (!killed && (now>=deadline || r.cut || quit)) {
            killed    = gently ? 1 : 2;
            r.timeout = !r.cut && !quit;
            os_kill(os, p, killed==2);
            deadline  = now + GIT_DRAIN;  // for the kill to show
        } else if (killed==1 && now>=deadline) {
            killed   = 2;
            os_kill(os, p, 1);
            deadline = now + GIT_DRAIN;
        } else if (now >= deadline) {
            break;  // given up on: its late records are skipped
        }
        i64 left = deadline - now;
        mailwait(w->procs, left>200*MS ? 200 : (i32)(left/MS + 1));
        Mail m = mailtake(w->procs);
        for (Record rec; (rec = mailnext(&m)).ok;) {
            if (rec.id != sp.id) {
                continue;  // an earlier child's
            }
            switch (rec.type) {
            case PROC_DATA: {
                iz room = GIT_OUTMAX - out.len;
                iz take = rec.data.len<room ? rec.data.len : room;
                print(&out, takehead(rec.data, take));
                r.cut |= take < rec.data.len;
            } break;
            case PROC_EXIT:
                exited = 1;
                code   = get32(rec.data);
                r.sig  = rec.data.len>=8 ? get32(cuthead(rec.data, 4)) : 0;
                if (!killed && deadline-os_ticks(os) > GIT_DRAIN) {
                    deadline = os_ticks(os) + GIT_DRAIN;  // for its output
                }
                break;
            case PROC_EOF:
                eof = 1;
                break;
            }
        }
    }
    os_kill(os, p, 1);  // the reaper, and anything git started
    os_procfree(os, p);
    r.code = exited && !killed ? code : -1;
    r.out  = finish(&out);
    Read rd = os_readat(os, a, errpath, 0, GIT_ERRMAX);
    r.err = rd.err ? Str{} : rd.data;
    return r;
}

// A request, as the worker takes it.
struct GitAsk {
    i64        id;
    Str        sid;
    Str        cwd;
    Str        ceiling;
    Str        root;     // as the engine found it, which requests share
    Str        git;      // [tools] git
    Str        home;
    Str        errpath;  // for git's standard error
    Slice<Str> env;
    b32        done;     // answered in this batch
};

static GitAsk gitparseask(Arena *a, Str record)
{
    GitAsk  q = {};
    JsonDoc d = jsonparse(a, clone(a, record));
    q.id      = jsonint(jsonget(d.root, "id"), 0);
    q.sid     = jsonstr(jsonget(d.root, "sid"));
    q.cwd     = jsonstr(jsonget(d.root, "cwd"));
    q.ceiling = jsonstr(jsonget(d.root, "ceiling"));
    q.root    = jsonstr(jsonget(d.root, "root"));
    q.git     = jsonstr(jsonget(d.root, "git"));
    q.home    = jsonstr(jsonget(d.root, "home"));
    q.errpath = jsonstr(jsonget(d.root, "errpath"));
    for (Json *e = jsonget(d.root, "env")->child; e; e = e->next) {
        q.env = push(a, q.env, jsonstr(e));
    }
    return q;
}

// git status, then git diff-index for the lines, in a repository's
// root: the status as JSON in at most budget bytes, or why there is
// none. Null if the engine stopped meanwhile.
static Str gitstatus(GitWork *w, Arena *a, Arena scratch, GitAsk *q, Str root,
                     iz budget)
{
    Os     *os    = w->os;
    i64     start = os_ticks(os);
    Str     path  = envpath(q->env, HOST_WINDOWS);
    GitProg prog  = gitprog(os, a, scratch, q->git, path, q->home, HOST_WINDOWS, GIT_MACOS);
    if (!prog.path.len || prog.shim) {
        return gitfailjson(a, gitmissing(a, q->git, prog.shim));
    }

    Slice<Str> argv = gitargv(a, prog.path, 0);
    GitRun     r    = gitrun(w, a, scratch, argv, root, q->env, q->errpath, GIT_STATUSWAIT);
    if (r.quit) {
        return {};
    }
    Str err = giterror(a, &r, prog.path, "status", GIT_STATUSWAIT);
    if (err.len) {
        return gitfailjson(a, err);
    }
    GitStatus st = gitparsestatus(a, r.out);
    st.cut |= r.cut;

    b32 tracked = 0;
    for (iz i = 0; i < st.files.len; i++) {
        tracked |= st.files[i].kind != '?';
    }
    if (tracked && !st.unborn) {
        argv = gitargv(a, prog.path, 1);
        GitRun d = gitrun(w, a, scratch, argv, root, q->env, q->errpath, GIT_DIFFWAIT);
        if (d.quit) {
            return {};
        } else if (!giterror(a, &d, prog.path, "diff-index", GIT_DIFFWAIT).len) {
            gitparsenumstat(scratch, d.out, &st);
        }
        st.partial |= d.cut;
    }
    gitcount(os, scratch, root, &st);
    for (iz i = 0; i < st.files.len; i++) {
        st.partial |= !st.files[i].counted;
    }
    i64 ms = (os_ticks(os) - start) / MS;
    return gitstatusjson(a, scratch, &st, ms, budget);
}

// Answer the requests of a batch for one repository, that of request
// first and those after it with the same root, with one run of git:
// the head and the status, for the engine to reply to each, under the
// latest request's settings. Null if the engine stopped meanwhile.
static Str gitanswer(GitWork *w, Arena *a, Arena scratch, Slice<GitAsk> asks, iz first)
{
    Str     root = asks[first].root;
    GitAsk *q    = &asks[first];
    for (iz i = first+1; i < asks.len; i++) {
        q = asks[i].root==root ? &asks[i] : q;
    }
    GitHead h      = githead(w->os, a, q->cwd, q->ceiling, HOST_WINDOWS);
    Str     head   = githeadjson(a, &h);
    Str     status = {};
    if (!h.repo) {
        status = gitfailjson(a, "not in a git repository");
    } else {
        status = gitstatus(w, a, scratch, q, h.root, GIT_REPLYMAX - head.len - 256);
        if (!status.data) {
            return {};
        }
    }

    Buf     b(a, 256 + head.len + status.len);
    JsonOut o = {&b};
    putobject(&o);
    putkey(&o, "waiters");
    putarray(&o);
    for (iz i = first; i < asks.len; i++) {
        if (!asks[i].done && asks[i].root==root) {
            asks[i].done = 1;
            putarray(&o);
            putint(&o, asks[i].id);
            putstr(&o, asks[i].sid);
            putend(&o);
        }
    }
    putend(&o);
    putkey(&o, "head");
    putraw(&o, head);
    putkey(&o, "status");
    putraw(&o, status);
    putend(&o);
    return finish(&b);
}

// The worker: idle until a request comes, then everything queued at
// once, a repository at a time.
static void gitthread(void *arg)
{
    GitWork *w = (GitWork *)arg;
    for (;;) {
        mailwait(w->req, -1);
        if (gitquitting(w)) {
            return;
        }
        Arena         perm    = newarena(w->perm);
        Arena         scratch = newarena(w->scratch);
        Slice<GitAsk> asks    = {};
        Mail          m       = mailtake(w->req);
        for (Record r; (r = mailnext(&m)).ok;) {
            asks = push(&perm, asks, gitparseask(&perm, r.data));
        }
        for (iz i = 0; i < asks.len; i++) {
            if (asks[i].done) {
                continue;
            }
            Arena a      = perm;
            Str   answer = gitanswer(w, &a, scratch, asks, i);
            if (!answer.data) {
                return;  // the engine stopped
            }
            mailpost(w->out, MAIL_GIT, 0, answer, 1);
        }
    }
}


// The engine's side

static GitWork *gitwork(Engine *e)
{
    GitWork *w = e->git;
    if (!w) {
        w = e->git = alloc<GitWork>(&e->perm);
        w->os  = e->os;
        w->out = e->in;
    }
    w->req     = w->req     ? w->req     : newmailbox(&e->perm, GIT_REQBOX, 0, 0);
    w->procs   = w->procs   ? w->procs   : newmailbox(&e->perm, GIT_PROCBOX, 0, 0);
    w->perm    = w->perm    ? w->perm    : os_reserve(e->os, GIT_REGION);
    w->scratch = w->scratch ? w->scratch : os_reserve(e->os, GIT_REGION);
    if (!w->started && w->req && w->procs && w->perm && w->scratch) {
        w->started = os_thread(e->os, gitthread, w);
    }
    if (!w->started) {
        // Reserved again by the next request, which tries again
        if (w->perm) os_release(e->os, w->perm);
        if (w->scratch) os_release(e->os, w->scratch);
        w->perm = w->scratch = 0;
    }
    return w->started ? w : 0;
}

// A session's directory, open here or not, and whether it works in a
// folder of its own; null if there is no such session.
static Str gitsessdir(Engine *e, Arena *a, Str sid, b32 *nofolder)
{
    Session *s = findsession(e, sid);
    if (s) {
        *nofolder = s->nofolder;
        return s->cwd;
    }
    Str   path = sidecarpath(a, e, sid, "events.jsonl");
    Read  rd   = os_readat(e->os, a, path, 0, 64<<10);
    Cut   line = cut(rd.data, '\n');
    EvRec rec  = evparse(a, line.head);
    if (!line.ok || rec.ev!=EV_SESSION) {
        return {};
    }
    *nofolder = jsonint(jsonget(rec.root, "nofolder"), 0) == 1;
    return jsonstr(jsonget(rec.root, "cwd"));
}

// Queue a request for the worker: null if queued, else why not.
static Str gitqueue(Engine *e, Arena scratch, i64 id, Str sid, Str cwd,
                    Str ceiling, Str root)
{
    GitWork *w = gitwork(e);
    if (!w) {
        return "cannot start a thread for git";
    } else if (w->pending >= GIT_PENDING) {
        return "git is busy: try again";
    }
    Str tmp = join(&scratch, e->statedir, "tmp");
    os_mkdirs(e->os, scratch, tmp);
    Buf name(&scratch, 32);
    print(&name, "git-");
    print(&name, (i64)os_pid(e->os));
    print(&name, ".err");
    Str        errpath = join(&scratch, tmp, finish(&name));
    Slice<Str> env     = engineenv(e, &scratch, HOST_WINDOWS);

    Buf     b(&scratch, 1024);
    JsonOut o = {&b};
    putobject(&o);
    putintkey(&o, "id", id);
    putstrkey(&o, "sid", sid);
    putstrkey(&o, "cwd", cwd);
    putstrkey(&o, "ceiling", ceiling);
    putstrkey(&o, "root", root);
    putstrkey(&o, "git", trim(cfgget(e->cfg, "tools", "", "git", 0)));
    putstrkey(&o, "home", e->home);
    putstrkey(&o, "errpath", errpath);
    putkey(&o, "env");
    putarray(&o);
    for (iz i = 0; i < env.len; i++) {
        putstr(&o, env[i]);
    }
    putend(&o);
    putend(&o);
    if (!mailpost(w->req, 0, 0, finish(&b), 0)) {
        return "git is busy: try again";
    }
    w->pending++;
    return {};
}

// git {sid, status?}: the branch of a session's working tree, open here
// or not, read from its files, and with status, its changes as git
// tells them, in a reply once git is done (late, as models' is).
static void cmdgit(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str sid = jsonstr(jsonget(j, "sid"));
    if (!validsid(sid)) {
        reply(e, scratch, id, 0, "invalid session id", {});
        return;
    }
    b32 nofolder = 0;
    Str cwd      = gitsessdir(e, &scratch, sid, &nofolder);
    if (!cwd.len) {
        reply(e, scratch, id, 0, "no such session", sid);
        return;
    }
    Str     ceiling = nofolder ? cwd : Str{};
    GitHead h       = githead(e->os, &scratch, cwd, ceiling, HOST_WINDOWS);
    Str     status  = {};
    if (h.repo && jsonflag(jsonget(j, "status"), 0)) {
        Str err = gitqueue(e, scratch, id, sid, cwd, ceiling, h.root);
        if (!err.len) {
            return;  // gitdone replies
        }
        status = gitfailjson(&scratch, err);
    }

    Str   head = githeadjson(&scratch, &h);
    Buf   b(&scratch, 256 + head.len + status.len);
    EvOut w = evbegin(&b, sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putkey(&w.json, "head");
    putraw(&w.json, head);
    if (status.len) {
        putkey(&w.json, "status");
        putraw(&w.json, status);
    }
    emit(e, evend(&w));
}

// The worker's answer for one repository: a reply to each request.
static void gitdone(Engine *e, Arena scratch, Str answer)
{
    JsonDoc d      = jsonparse(&scratch, answer);
    Json   *head   = jsonget(d.root, "head");
    Json   *status = jsonget(d.root, "status");
    for (Json *k = jsonget(d.root, "waiters")->child; k; k = k->next) {
        e->git->pending--;
        Arena t   = scratch;
        Str   sid = jsonstr(jsonat(k, 1));
        Buf   b(&t, 256 + head->raw.len + status->raw.len);
        EvOut w = evbegin(&b, sid, 0, os_time(e->os), EV_REPLY);
        putintkey(&w.json, "id", jsonint(jsonat(k, 0), 0));
        putintkey(&w.json, "ok", 1);
        putkey(&w.json, "head");
        putjson(&w.json, head);
        putkey(&w.json, "status");
        putjson(&w.json, status);
        emit(e, evend(&w));
    }
}

// The engine stops: the worker gives up on any run, killing git, and
// ends without answering.
static void gitquit(Engine *e)
{
    GitWork *w = e->git;
    if (w && w->started) {
        __atomic_store_n(&w->quit, 1, __ATOMIC_RELEASE);
        mailpoke(w->req);
    }
}
