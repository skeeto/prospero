// FileWalk: the resumable walk of a working directory, against the fake
// Os: which entries are kept, the .gitignore rules, links, the caps, and a
// walk resumed a step at a time.

// Whether a sorts before b, byte-wise.
static b32 walkbefore(Str a, Str b)
{
    iz  n = a.len<b.len ? a.len : b.len;
    i32 c = n ? __builtin_memcmp(a.data, b.data, (uz)n) : 0;
    return c ? c<0 : a.len<b.len;
}

// A walk's paths, sorted, a folder ending in '/', in a's memory. The
// walk's own order is the filesystem's, which the fake gives in the
// reverse of the order its files were made.
static Slice<Str> walksorted(Arena *a, FileWalk *w)
{
    Slice<Str> list = {};
    for (iz i = 0; i < w->paths.len; i++) {
        list = push(a, list, w->isdirs[i] ? concat(a, w->paths[i], "/") : w->paths[i]);
    }
    for (iz i = 1; i < list.len; i++) {  // insertion sort
        Str v = list[i];
        iz  j = i;
        for (; j>0 && walkbefore(v, list[j-1]); j--) {
            list[j] = list[j-1];
        }
        list[j] = v;
    }
    return list;
}

// The paths as one line.
static Str walkshow(Arena *a, Slice<Str> sorted)
{
    Buf b(a, 256);
    for (iz i = 0; i < sorted.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, sorted[i]);
    }
    return finish(&b);
}

// Whether a path is there twice.
static b32 walktwice(Slice<Str> sorted)
{
    for (iz i = 1; i < sorted.len; i++) {
        if (sorted[i] == sorted[i-1]) {
            return 1;
        }
    }
    return 0;
}

// Walk root to its end, with the caps given, 0 for the constants.
static void walkto(FileWalk *w, Os *os, Arena *mem, Str root,
                   iz maxfiles, iz maxdir, iz maxdepth)
{
    filewalkbegin(w, os, mem, root, 0);
    if (maxfiles) w->maxfiles = maxfiles;
    if (maxdir)   w->maxdir   = maxdir;
    if (maxdepth) w->maxdepth = maxdepth;
    while (filewalkstep(w)) {}
}

static void test_filewalk(Test *t)
{
    Arena *a = &t->perm;

    // Every file and every folder, relative and '/'-separated
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/w");
        os_mkdirs(os, *a, "/w/src");
        os_mkdirs(os, *a, "/w/src/sub");
        os_write(os, *a, "/w/a.txt", "a\n");
        os_write(os, *a, "/w/b.md", "b\n");
        os_write(os, *a, "/w/src/x.c", "x\n");
        os_write(os, *a, "/w/src/sub/y.h", "y\n");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/w", 0, 0, 0);
        expect(t, "walk.files", walkshow(a, walksorted(a, &w)),
               "a.txt b.md src/ src/sub/ src/sub/y.h src/x.c");
        // The mtimes are the tree's own
        iz same = 0;
        for (iz i = 0; i < w.paths.len; i++) {
            Str  p = join(a, "/w", w.paths[i]);
            Stat s = os_stat(os, *a, p);
            same += s.mtime == w.mtimes[i];
        }
        expect(t, "walk.files.mtime", istr(a, same), istr(a, w.paths.len));
        filewalkfree(&w);
    }

    // A .gitignore in the root and a nested one in a subdirectory
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/g");
        os_mkdirs(os, *a, "/g/sub");
        os_write(os, *a, "/g/.gitignore", "*.log\n");
        os_write(os, *a, "/g/keep.txt", "keep\n");
        os_write(os, *a, "/g/noise.log", "noise\n");
        os_write(os, *a, "/g/sub/.gitignore", "keep.txt\n");
        os_write(os, *a, "/g/sub/keep.txt", "sub keep\n");
        os_write(os, *a, "/g/sub/other.txt", "other\n");
        os_write(os, *a, "/g/sub/noise.log", "noise\n");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/g", 0, 0, 0);
        expect(t, "walk.gitignore", walkshow(a, walksorted(a, &w)),
               ".gitignore keep.txt sub/ sub/.gitignore sub/other.txt");
        filewalkfree(&w);
    }

    // .git is never walked
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/d");
        os_mkdirs(os, *a, "/d/.git");
        os_mkdirs(os, *a, "/d/.git/objects");
        os_write(os, *a, "/d/.git/config", "[core]\n");
        os_write(os, *a, "/d/.git/objects/o", "o\n");
        os_write(os, *a, "/d/x.txt", "x\n");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/d", 0, 0, 0);
        expect(t, "walk.dotgit", walkshow(a, walksorted(a, &w)), "x.txt");
        filewalkfree(&w);
    }

    // A link to a directory is not followed: the fake lists links as what
    // they name, and os_realpath tells the walk it is one
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/s");
        os_mkdirs(os, *a, "/s/real");
        os_write(os, *a, "/s/real/x.txt", "x\n");
        vlink(os, "/s/link", "/s/real");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/s", 0, 0, 0);
        expect(t, "walk.symlink", walkshow(a, walksorted(a, &w)), "real/ real/x.txt");
        filewalkfree(&w);
    }

    // The caps: paths kept, entries of one directory, and depth
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/c");
        os_mkdirs(os, *a, "/c/d1");
        os_mkdirs(os, *a, "/c/d1/d2");
        os_mkdirs(os, *a, "/c/d1/d2/d3");
        os_write(os, *a, "/c/a.txt", "a\n");
        os_write(os, *a, "/c/b.txt", "b\n");
        os_write(os, *a, "/c/d1/d2/d3/deep.txt", "deep\n");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/c", 2, 0, 0);
        expect(t, "walk.caps.files", istr(a, w.paths.len), "2");
        expect(t, "walk.caps.files.trunc", istr(a, w.truncated), "1");
        filewalkfree(&w);

        walkto(&w, os, &mem, "/c", 0, 2, 0);  // two entries of a directory
        expect(t, "walk.caps.dir", istr(a, w.paths.len), "2");
        expect(t, "walk.caps.dir.trunc", istr(a, w.truncated), "1");
        filewalkfree(&w);

        walkto(&w, os, &mem, "/c", 0, 0, 2);  // two levels below the root
        expect(t, "walk.caps.depth", walkshow(a, walksorted(a, &w)),
               "a.txt b.txt d1/ d1/d2/");
        expect(t, "walk.caps.depth.trunc", istr(a, w.truncated), "1");
        filewalkfree(&w);
    }

    // A walk resumed a step at a time is the walk to the end, and no path
    // comes twice
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/m");
        for (i32 i = 0; i < 12; i++) {  // more folders than one step lists
            Buf dir(a, 32), file(a, 32);
            print(&dir, "/m/d");
            print(&dir, (i64)i);
            os_mkdirs(os, *a, finish(&dir));
            print(&file, "/m/d");
            print(&file, (i64)i);
            print(&file, "/f.txt");
            os_write(os, *a, finish(&file), "f\n");
        }
        Arena    mem = carve(a, 1<<20);
        FileWalk once = {}, again = {};
        walkto(&once, os, &mem, "/m", 0, 0, 0);
        Slice<Str> all = walksorted(a, &once);
        filewalkfree(&once);

        filewalkbegin(&again, os, &mem, "/m", 0);
        i32 steps = 0;
        b32 more;
        do {  // the last step does work and still says there is none left
            more = filewalkstep(&again);
            steps++;
        } while (more);
        expect(t, "walk.resume", walkshow(a, walksorted(a, &again)), walkshow(a, all));
        expect(t, "walk.resume.count", istr(a, again.paths.len), "24");
        expect(t, "walk.resume.steps", istr(a, steps>1), "1");
        expect(t, "walk.resume.once", istr(a, walktwice(all)), "0");
        expect(t, "walk.resume.done", istr(a, filewalkstep(&again)), "0");
        filewalkfree(&again);
    }

    // An empty folder is a candidate
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/e");
        os_mkdirs(os, *a, "/e/empty");
        Arena    mem = carve(a, 1<<20);
        FileWalk w   = {};
        walkto(&w, os, &mem, "/e", 0, 0, 0);
        expect(t, "walk.empty", walkshow(a, walksorted(a, &w)), "empty/");
        expect(t, "walk.empty.isdir", istr(a, w.isdirs.len ? w.isdirs[0] : 0), "1");
        filewalkfree(&w);
    }

    // A directory is a link only when its real path is not its parent's
    // real path joined with its name. A root under a symlinked ancestor
    // (macOS's /tmp, a symlinked ~/dev) must not read as a tree of links,
    // which would walk nothing at all
    {
        Arena a2 = carve(a, 1<<16);
        expect(t, "walk.linkroot",
               istr(&a2, filewalklinked(&a2, "/private/tmp", "x", "/private/tmp/x")), "0");
        expect(t, "walk.linkleaf",
               istr(&a2, filewalklinked(&a2, "/private/tmp", "x", "/somewhere/else")), "1");
        expect(t, "walk.linknone",
               istr(&a2, filewalklinked(&a2, "/w", "x", {})), "1");
    }
}
