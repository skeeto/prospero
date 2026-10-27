// Guard: where the file tools may go
//
// A tool's path is resolved twice. Lexically: relative to the working
// directory, with "~" expanded and "." and ".." folded away. And for
// real: with symlinks resolved (os_realpath) as far as the path exists,
// then the missing rest appended. Both must lie within a workspace root:
// the working directory, any extra roots from the configuration, and for
// reads only, the session's own directory, where shell output is
// spilled. Hidden roots, which the agent is never told of nor hinted
// at, add the session's scratch directory, the system's temporary
// directories, and what the user allowed for good (guardgrant), which
// may be a directory not made yet, or a single file. A path inside
// lexically whose real path leaves is a symlink pointing out, and is
// reported as such.
//
// A path outside is not an error of the harness: it becomes a request
// for permission (an ask), which an attended session puts to the user
// and an unattended one denies at once. Either way a denial is a tool
// result the agent can act on, with a hint: the nearest path inside the
// workspace with the same trailing components, when one exists.
//
// Windows paths take either separator and compare without case. Drive
// letters, drive-relative paths ("C:x"), UNC paths, and the
// extended-length forms ("\\?\C:\", "\\?\UNC\") are understood. Win32
// drops trailing dots from names, and trailing spaces too from the last
// one; here both go from every name, and a name of nothing else drops
// out ("..." and ".. " are not parents). The tools then use the
// normalized path itself, which Win32 changes no further, so what was
// judged is what is opened. The normalized form uses '/' throughout and
// keeps the root: "/" on POSIX, "C:/" or "//server/share/" on Windows.
// Device paths ("\\.\x") keep a root of their own, outside any
// workspace.
//
// This is free and unencumbered software released into the public domain.

#ifdef _WIN32
enum { HOST_WINDOWS = 1 };
#else
enum { HOST_WINDOWS = 0 };
#endif

static b32 issep(u8 c, b32 win)
{
    return c=='/' || (win && c=='\\');
}

static b32 isdrive(Str p)  // "C:" and more
{
    return p.len>=2 && letter(p[0]) && p[1]==':';
}

// Length of the root of a path, 0 if relative: "/", "C:/", "//h/s/".
// On Windows, "\x" is rooted but has no root of its own: it takes the
// working directory's drive.
static iz pathroot(Str p, b32 win)
{
    if (!win) {
        return p.len && p[0]=='/';
    }
    if (p.len>=3 && isdrive(p) && issep(p[2], 1)) {
        return 3;
    }
    if (p.len>=2 && issep(p[0], 1) && issep(p[1], 1)) {
        iz i = 2;
        for (i32 parts = 0; parts < 2; parts++) {
            for (; i<p.len && !issep(p[i], 1); i++) {}
            i += i < p.len;
        }
        return i;
    }
    return 0;
}

// The Win32 form of an extended-length path: "\\?\C:\x" is "C:\x" and
// "\\?\UNC\h\s" is "\\h\s". Anything else after "\\?\" stays as it is.
// Repeated until nothing changes, so that the result never has another
// such prefix to strip.
static Str dosform(Arena *a, Str p)
{
    for (;;) {
        if (p.len<4 || !issep(p[0], 1) || !issep(p[1], 1) || p[2]!='?' ||
                !issep(p[3], 1)) {
            return p;
        }
        Str rest = cuthead(p, 4);
        if (rest.len>=3 && isdrive(rest) && issep(rest[2], 1)) {
            p = rest;
        } else if (rest.len>=4 && equalfold(takehead(rest, 3), "UNC") &&
                   issep(rest[3], 1)) {
            p = concat(a, "//", cuthead(rest, 4));
        } else {
            return p;
        }
    }
}

// A path component as Win32 opens it: trailing dots and spaces dropped,
// except from "." and "..". The result may be empty.
static Str wincomp(Str c)
{
    if (c=="." || c=="..") {
        return c;
    }
    for (; c.len && (c[c.len-1]=='.' || c[c.len-1]==' '); c.len--) {}
    return c;
}

// Absolute, with "." and ".." folded and '/' separators. The working
// directory must be absolute. A ".." at the root stays at the root.
static Str normpath(Arena *a, Str cwd, Str path, b32 win)
{
    Str full = win ? dosform(a, path) : path;
    cwd = win ? dosform(a, cwd) : cwd;
    if (!pathroot(full, win)) {
        if (win && isdrive(full)) {
            // Drive-relative: the working directory if on that drive,
            // else the drive's root (other drives' directories are not
            // known here)
            Str rest  = cuthead(full, 2);
            iz  croot = pathroot(cwd, 1);
            if (croot==3 && equalfold(takehead(cwd, 2), takehead(full, 2))) {
                full = concat(a, concat(a, cwd, "/"), rest);
            } else {
                full = concat(a, concat(a, takehead(full, 2), "/"), rest);
            }
        } else if (win && full.len>=1 && issep(full[0], 1)) {
            // Rooted on the working directory's drive
            full = concat(a, takehead(cwd, pathroot(cwd, 1) - 1), full);
        } else {
            full = concat(a, concat(a, cwd, "/"), full);
        }
    }
    iz root = pathroot(full, win);

    Buf b(a, full.len + 2);
    for (iz i = 0; i < root; i++) {
        putbyte(&b, issep(full[i], win) ? '/' : full[i]);
    }
    if (win && root==3 && isdrive(full)) {
        b.data[0] = (u8)(full[0] & ~0x20);  // drive letters in upper case
    } else if (win && root>2 && b.data[root-1]!='/') {
        putbyte(&b, '/');  // "//h/s" as "//h/s/"
    }
    iz        broot = b.len;
    Slice<iz> kept  = {};  // length of b before each kept component
    for (iz i = root; i < full.len;) {
        iz j = i;
        for (; j<full.len && !issep(full[j], win); j++) {}
        Str comp = span(full.data+i, full.data+j);
        i = j + (j < full.len);
        if (win) {
            comp = wincomp(comp);
        }
        if (!comp.len || comp==".") {
            continue;
        } else if (comp == "..") {
            if (kept.len) {
                b.len = kept[kept.len-1];
                kept.len--;
            }
            continue;
        }
        kept = push(a, kept, b.len);
        if (b.len > broot) {
            putbyte(&b, '/');
        }
        print(&b, comp);
    }
    return finish(&b);
}

// Whether path is root itself or lies below it, both normalized.
static b32 within(Str root, Str path, b32 win)
{
    if (root.len > path.len) {
        return 0;
    }
    Str head = takehead(path, root.len);
    if (win ? !equalfold(head, root) : head!=root) {
        return 0;
    }
    return path.len==root.len || root[root.len-1]=='/' || path[root.len]=='/';
}

// "~" and "~/x" in the home directory, given one; anything else as is.
static Str expandhome(Arena *a, Str path, Str home, b32 win)
{
    if (!home.len || !path.len || path[0]!='~') {
        return path;
    }
    if (path.len==1 || issep(path[1], win)) {
        return concat(a, home, cuthead(path, 1));
    }
    return path;  // "~user" is a plain name
}

// A drive as MSYS2 and Git Bash print it, "/c/x" or "/c", or as Cygwin
// does, "/cygdrive/c/x", in Windows' form, "c:/x". Win32 would root
// these on the current drive instead, in a folder named c. Anything
// else, "/mnt/c" or "\c" among them, is as it is.
static Str msysdrive(Arena *a, Str path)
{
    Str p = startswith(path, "/cygdrive/") ? cuthead(path, 9) : path;
    if (p.len<2 || p[0]!='/' || !letter(p[1]) || (p.len>2 && p[2]!='/')) {
        return path;
    }
    Buf b(a, p.len + 2);
    putbyte(&b, p[1]);
    putbyte(&b, ':');
    print(&b, p.len>2 ? cuthead(p, 2) : Str("/"));
    return finish(&b);
}

// The other way: a path in the home directory as "~" or "~/x", else as
// it is. A home with a drive (C:) is a Windows path, compared without
// case, its separators either. The path keeps its own separators.
static Str tildeform(Arena *a, Str path, Str home)
{
    b32 win  = home.len>=2 && home[1]==':';
    iz  root = pathroot(home, win);
    for (; home.len>root && issep(home[home.len-1], win); home.len--) {}
    if (!root || root==home.len || path.len<home.len) {
        return path;  // no home, or a root: the whole file system
    }
    for (iz i = 0; i < home.len; i++) {
        u8  x    = path[i];
        u8  y    = home[i];
        b32 same = win ? (issep(x, 1) && issep(y, 1)) || equalfold({&x, 1}, {&y, 1}) : x==y;
        if (!same) {
            return path;
        }
    }
    if (path.len>home.len && !issep(path[home.len], win)) {
        return path;
    }
    return concat(a, "~", cuthead(path, home.len));
}

// The real path of a normalized one: symlinks resolved through the
// deepest ancestor that exists, then the missing rest appended. Null if
// not even the root resolves. The deepest few ancestors are tried one
// by one, since the answer is usually among them, and then the rest by
// binary search, as an ancestor resolves only if its own ancestors do:
// however deep a path the agent makes up, the lookups stay few.
static Str realpathof(Os *os, Arena *a, Str path, b32 win)
{
    // Where each ancestor ends, from the root down to the path itself
    iz        root = pathroot(path, win);
    Slice<iz> ends = {};
    ends = push(a, ends, root);
    for (iz i = root; i < path.len; i++) {
        if (path[i] == '/') {
            ends = push(a, ends, i);
        }
    }
    if (path.len > root) {
        ends = push(a, ends, path.len);
    }

    Str real = {};
    iz  lo   = -1;        // ends[lo] resolves
    iz  hi   = ends.len;  // ends[hi] does not
    for (i32 n = 0; n<8 && hi-lo>1; n++) {
        Str r = os_realpath(os, a, takehead(path, ends[hi-1]));
        if (r.data) {
            lo   = hi - 1;
            real = r;
        } else {
            hi--;
        }
    }
    while (hi-lo > 1) {
        iz  mid = lo + (hi-lo)/2;
        Str r   = os_realpath(os, a, takehead(path, ends[mid]));
        if (r.data) {
            lo   = mid;
            real = r;
        } else {
            hi = mid;
        }
    }
    if (lo < 0) {
        return {};
    }
    Str rest = cuthead(path, ends[lo]);
    if (rest.len && !issep(rest[0], win) && real.len &&
            !issep(real[real.len-1], win)) {
        real = concat(a, real, "/");
    }
    return normpath(a, takehead(path, root), concat(a, real, rest), win);
}

struct GuardRoot {
    Str lex;     // normalized
    Str real;    // its real path, normalized
    b32 read;    // only for reads
    b32 hidden;  // never named to the agent, nor hinted at: the system's
                 // temporary directories, and grants from the user
};

// The workspace of one session.
struct Guard {
    Slice<GuardRoot> roots;
    Str              cwd;   // normalized
    Str              home;  // for "~", empty if unknown
    b32              win;
};

enum : i32 {
    ROOT_READ    = 1<<0,  // only for reads
    ROOT_HIDDEN  = 1<<1,  // GuardRoot hidden
    ROOT_MISSING = 1<<2,  // kept though it does not exist (yet)
};

// Add a root, as a path relative to the working directory or absolute.
// A root that does not exist is ignored, unless ROOT_MISSING keeps it,
// with the real path of its deepest existing ancestor (realpathof): a
// directory the user allowed before it was made, or a file to be.
static void guardadd(Os *os, Arena *perm, Guard *g, Str path, i32 flags)
{
    if (!path.len || cut(path, 0).ok) {
        return;
    }
    GuardRoot r = {};
    r.lex    = normpath(perm, g->cwd, expandhome(perm, path, g->home, g->win), g->win);
    r.read   = !!(flags & ROOT_READ);
    r.hidden = !!(flags & ROOT_HIDDEN);
    if (flags & ROOT_MISSING) {
        r.real = realpathof(os, perm, r.lex, g->win);
    } else {
        r.real = os_realpath(os, perm, r.lex);
        r.real = r.real.data ? normpath(perm, r.lex, r.real, g->win) : Str{};
    }
    if (r.real.data) {
        g->roots = push(perm, g->roots, r);
    }
}

static void guardroot(Os *os, Arena *perm, Guard *g, Str path, b32 read)
{
    guardadd(os, perm, g, path, read ? ROOT_READ : 0);
}

// The workspace: the working directory (absolute), extra roots, and a
// directory open to reads (the session's own, or empty for none).
static Guard newguard(Os *os, Arena *perm, Str cwd, Str home, Slice<Str> extra,
                      Str readdir, b32 win)
{
    Guard g = {};
    g.win  = win;
    g.home = home;
    g.cwd  = normpath(perm, cwd, ".", win);
    guardroot(os, perm, &g, g.cwd, 0);
    for (iz i = 0; i < extra.len; i++) {
        guardroot(os, perm, &g, extra[i], 0);
    }
    guardroot(os, perm, &g, readdir, 1);
    return g;
}

struct GuardPath {
    Str path;      // lexical: absolute and normalized, what the tool uses
    Str real;      // symlinks resolved; null unless lexically inside
    b32 inside;    // allowed without asking
    b32 diverged;  // inside lexically, but a symlink leads out
};

// Resolve a tool's path argument and judge it.
static GuardPath guardpath(Os *os, Arena *a, Guard *g, Str path, b32 read)
{
    GuardPath r = {};
    if (cut(path, 0).ok) {
        r.path = path;
        return r;  // names nothing: never inside
    }
    path   = g->win ? msysdrive(a, path) : path;
    r.path = normpath(a, g->cwd, expandhome(a, path, g->home, g->win), g->win);

    // Lexically, against either form of each root: the real path of a
    // root is the same place
    b32 lexok = 0;
    for (iz i = 0; i<g->roots.len && !lexok; i++) {
        GuardRoot *t = &g->roots[i];
        lexok = (!t->read || read) &&
                (within(t->lex, r.path, g->win) || within(t->real, r.path, g->win));
    }
    if (!lexok) {
        return r;  // the real path is not needed, nor looked for
    }

    r.real = realpathof(os, a, r.path, g->win);
    for (iz i = 0; i<g->roots.len && !r.inside; i++) {
        GuardRoot *t = &g->roots[i];
        r.inside = (!t->read || read) && r.real.data &&
                   within(t->real, r.real, g->win);
    }
    r.diverged = !r.inside;
    return r;
}

// What "always allow" grants with an ask's answer: the directory of
// the path asked about, or with isdir (a search) the path itself, on
// its real path where a symlink leads out. A filesystem root or the
// home directory is too much to give at once: then the path alone, or
// nothing if it is one of those itself. Null if nothing.
static Str guardgrant(Arena *a, Guard *g, GuardPath *gp, b32 isdir)
{
    Str path = gp->diverged ? gp->real : gp->path;
    if (!path.data) {
        return {};
    }
    Str home = g->home.len ? normpath(a, g->cwd, g->home, g->win) : Str{};
    Str dir  = isdir ? path : normpath(a, path, "..", g->win);
    for (i32 pass = 0; pass < 2; pass++) {
        b32 big = pathroot(dir, g->win)==dir.len ||
                  (home.len && (g->win ? equalfold(dir, home) : dir==home));
        if (!big) {
            return dir;
        } else if (isdir || pass) {
            return {};
        }
        dir = path;
    }
    return {};
}

struct GuardHint {
    Str path;    // null if no hint
    b32 exists;  // names something now, rather than a likely place
};

enum : i32 {
    HINT_PARTS = 8,  // trailing components a hint keeps, at most
};

// The path inside the workspace that the agent likely meant: the
// longest trailing part of path, of at most HINT_PARTS components, that
// exists under a root. Failing that, when the tool creates files, the
// longest whose directory exists there, which is at least the file name
// in the root itself. A candidate costs a stat, and the full judgment
// only once it matches, so the work is bounded however long the path.
static GuardHint guardhint(Os *os, Arena *a, Guard *g, Str path, b32 read,
                           b32 create)
{
    GuardHint r     = {};
    Str       rel   = cuthead(path, pathroot(path, g->win));
    iz        first = rel.len;  // where the trailing components begin
    for (i32 n = 0; n<HINT_PARTS && first>0; n++) {
        for (first--; first>0 && rel[first-1]!='/'; first--) {}
    }
    for (i32 pass = 0; pass < 1+create; pass++) {
        for (iz i = first; i < rel.len;) {
            Str suffix = cuthead(rel, i);
            for (iz k = 0; k < g->roots.len; k++) {
                GuardRoot *t = &g->roots[k];
                if ((t->read && !read) || t->hidden) {
                    continue;
                }
                Str cand = normpath(a, t->lex, suffix, g->win);
                if (cand==path || !within(t->lex, cand, g->win)) {
                    continue;
                }
                Str probe = pass ? normpath(a, cand, "..", g->win) : cand;
                i32 type  = os_stat(os, *a, probe).type;
                if (pass ? type!=STAT_DIR : type==STAT_MISSING) {
                    continue;
                }
                if (guardpath(os, a, g, cand, read).inside) {
                    r.path   = cand;
                    r.exists = !pass;
                    return r;
                }
            }
            for (; i<rel.len && rel[i]!='/'; i++) {}
            i++;
        }
    }
    return r;
}

enum : i32 {
    DENY_POLICY,   // unattended: nobody can answer
    DENY_USER,     // the user refused
    DENY_TIMEOUT,  // nobody answered in time
    DENY_STEER,    // the user sent a message instead of answering
    DENY_CANCEL,   // the turn was cancelled while asking
    DENY_BYPASS,   // not denied: the user turned on Bypass mode
    DENY_COUNT,
};

struct GuardDenial {
    Str       tool;
    GuardPath gp;
    Str       cwd;
    GuardHint hint;
    i32       by;     // DENY_*
    Str       note;   // the user's, if any
    i64       wait;   // seconds the ask was open, for DENY_TIMEOUT
};

// Why a path needs the user's approval, in a clause: the ask's detail.
static Str guardwhy(Arena *a, Str tool, GuardPath *gp, Str cwd)
{
    Buf b(a, 256);
    print(&b, tool);
    print(&b, " of ");
    print(&b, gp->path);
    if (gp->diverged) {
        print(&b, " leads through a symbolic link to ");
        print(&b, gp->real.data ? gp->real : Str("an unknown place"));
        print(&b, ", which is outside the working directory ");
    } else {
        print(&b, " is outside the working directory ");
    }
    print(&b, cwd);
    return finish(&b);
}

// The tool result for a denied path: what happened, why, and what to do
// instead, so that the agent can carry on.
static Str guarddenial(Arena *a, GuardDenial *d)
{
    Buf b(a, 1024);
    Str why = guardwhy(a, d->tool, &d->gp, d->cwd);
    switch (d->by) {
    case DENY_POLICY:
        print(&b, "Permission denied: ");
        print(&b, why);
        print(&b, ", which needs the user's approval. This non-interactive "
                  "run cannot ask the user, so the request was rejected and "
                  "nothing was done.");
        break;
    case DENY_USER:
        print(&b, "Permission denied by the user: ");
        print(&b, why);
        print(&b, ". Nothing was done.");
        if (d->note.len) {
            print(&b, " The user's note: ");
            print(&b, d->note);
        }
        break;
    case DENY_TIMEOUT:
        print(&b, "Permission denied: ");
        print(&b, why);
        print(&b, ", which needs the user's approval, and nobody answered "
                  "within ");
        print(&b, d->wait);
        print(&b, " seconds, so the request was rejected and nothing was "
                  "done.");
        break;
    case DENY_STEER:
        print(&b, "Not done: ");
        print(&b, why);
        print(&b, ", which needs the user's approval, and the user sent a "
                  "new message instead of answering. Read that message "
                  "(it follows these results) and do what it says.");
        return finish(&b);
    case DENY_CANCEL:
        return "Not run: the user cancelled the turn.";
    }

    print(&b, " Do not retry this path, and do not reach it or anything in "
              "it another way, as with the shell or through another path "
              "to it. Continue without it, working inside ");
    print(&b, d->cwd);
    if (d->hint.path.data && d->hint.exists) {
        print(&b, ". Did you mean ");
        print(&b, d->hint.path);
        print(&b, "? It exists inside the working directory.");
    } else if (d->hint.path.data) {
        print(&b, " (for example ");
        print(&b, d->hint.path);
        print(&b, ").");
    } else {
        putbyte(&b, '.');
    }
    print(&b, " If you cannot finish without it, finish what you can, then "
              "ask the user for it in your final response.");
    return finish(&b);
}

// The tool result for a write under the harness's state directory,
// which Bypass mode refuses, though it allows writes elsewhere outside
// the working directory: the sessions' logs, locks and caches are there.
static Str guardstatedenial(Arena *a, Str tool, Str path, Str statedir, Str cwd)
{
    Buf b(a, 512 + path.len + statedir.len + cwd.len);
    print(&b, "Permission denied: ");
    print(&b, tool);
    print(&b, " of ");
    print(&b, path);
    print(&b, " would write under the harness's state directory ");
    print(&b, statedir);
    print(&b, ", where it keeps its sessions' logs, locks and caches. Bypass "
              "mode allows writes outside the working directory, but never "
              "there, so nothing was done. Do not retry this path. Write "
              "somewhere else instead, for example inside ");
    print(&b, cwd);
    putbyte(&b, '.');
    return finish(&b);
}
