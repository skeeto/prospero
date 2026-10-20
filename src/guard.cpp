// Guard: where the file tools may go
//
// The minimal guard of M2 is lexical. A path is taken relative to the
// working directory, "." and ".." are folded away, and the result must
// lie within the working directory, or for reads within the session's
// own output directory, where shell output is spilled. A path outside
// is not an error of the harness: it becomes a request for permission
// (an ask), which an unattended run denies at once with a hint the
// agent can act on. Symlinks leading out are not caught until M3 checks
// real paths as well.
//
// Windows paths take either separator and compare without case. The
// normalized form uses '/' throughout and keeps the root: "/" on POSIX,
// "C:/" or "//server/share/" on Windows.
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

// Length of the root of a path, 0 if relative: "/", "C:/", "//h/s/".
// On Windows, "\x" is rooted but has no root of its own: it takes the
// working directory's drive.
static iz pathroot(Str p, b32 win)
{
    if (!win) {
        return p.len && p[0]=='/';
    }
    if (p.len>=3 && letter(p[0]) && p[1]==':' && issep(p[2], 1)) {
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

// Absolute, with "." and ".." folded and '/' separators. The working
// directory must be absolute. A ".." at the root stays at the root.
static Str normpath(Arena *a, Str cwd, Str path, b32 win)
{
    Str full = path;
    if (!pathroot(path, win)) {
        if (win && path.len>=1 && issep(path[0], 1)) {
            // Rooted on the working directory's drive
            full = concat(a, takehead(cwd, pathroot(cwd, 1) - 1), path);
        } else {
            full = concat(a, concat(a, cwd, "/"), path);
        }
    }
    iz root = pathroot(full, win);

    Buf b(a, full.len + 1);
    for (iz i = 0; i < root; i++) {
        putbyte(&b, issep(full[i], win) ? '/' : full[i]);
    }
    if (win && root==3 && letter(full[0]) && full[1]==':') {
        b.data[0] = (u8)(full[0] & ~0x20);  // drive letters in upper case
    }
    Slice<iz> kept = {};  // length of b before each kept component
    for (iz i = root; i < full.len;) {
        iz j = i;
        for (; j<full.len && !issep(full[j], win); j++) {}
        Str comp = span(full.data+i, full.data+j);
        i = j + (j < full.len);
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
        if (b.len > root) {
            putbyte(&b, '/');
        }
        print(&b, comp);
    }
    return finish(&b);
}

// Whether path is root itself or lies below it.
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

struct GuardPath {
    Str path;    // absolute and normalized
    b32 inside;  // allowed without asking
};

// Resolve a tool's path argument. Reads may also see outdir.
static GuardPath guardpath(Arena *a, Str cwd, Str outdir, Str path, b32 read, b32 win)
{
    GuardPath r = {};
    if (cut(path, 0).ok) {
        r.path = path;
        return r;  // names nothing: never inside
    }
    Str ncwd = normpath(a, cwd, ".", win);
    r.path   = normpath(a, ncwd, path, win);
    r.inside = within(ncwd, r.path, win) ||
               (read && outdir.len && within(normpath(a, ncwd, outdir, win), r.path, win));
    return r;
}

// The denial an unattended run gives for a path outside, with the
// nearest path inside: the same name in the working directory.
static Str guarddenial(Arena *a, Str cwd, Str path, Str tool)
{
    Buf b(a, 512);
    print(&b, "Permission denied: ");
    print(&b, tool);
    print(&b, " of ");
    print(&b, path);
    print(&b, " needs the user's approval, because it is outside the "
              "working directory ");
    print(&b, cwd);
    print(&b, ". This non-interactive run cannot ask the user, so the "
              "request was rejected and nothing was done. Continue without "
              "it, working inside ");
    print(&b, cwd);
    Str name = path;
    for (iz i = name.len; i > 0; i--) {
        if (name[i-1]=='/' || name[i-1]=='\\') {
            name = cuthead(name, i);
            break;
        }
    }
    if (name.len && name!=".." && name!=".") {
        print(&b, " (for example ");
        print(&b, join(a, cwd, name));
        putbyte(&b, ')');
    }
    putbyte(&b, '.');
    return finish(&b);
}
