// The paths an '@' mention picker offers: a walk of a working directory,
// one directory at a time, without a half-listed directory. Each step
// lists one directory and pushes the subdirectories it must visit onto a
// stack, so a walk may be stopped anywhere and resumed.
//
// It runs on a file worker (fswork.cpp) as a job of its own
// (filewalkrun): a folder on a share that stops answering, or a link
// into one, stops that worker alone. Only when the pool has no thread at
// all, and none can start, does the job run inline on the window's
// thread: then it has FILEWALK_INLINE of work, one frame's time, and the
// index is what that found, truncated. Before each
// folder it lists, and before each subfolder's real path, it asks its
// gate, which beats and refuses a path under a mark (fsat): that folder
// is passed over, and the rest is walked. os_listall beats per entry.
// So a big tree that keeps answering is never declared stuck (FS_STUCK
// counts silence, not the walk's length); its own budget, FILEWALK_TIME
// of work, ends it as the caps do, truncated, with what it found. A call
// silent past FS_STUCK gives the job up, and marks the path it was on
// alone (the job is exact): the folder being listed, or the link being
// resolved, never the root's whole tree.
//
// It reads the tree through Os and reuses search.cpp's .gitignore rules,
// which are static in the same translation unit. Nothing here knows of the
// GUI: mention.cpp's index puts a FileWalkJob on the window's pool, and
// sorts and ranks its paths on the window's thread once it is answered.
//
// The caps are fields, set by filewalkbegin to the constants below, so
// that a test can drive them without a tree of ten thousand files.
//
// A FileWalk is zeroed by its holder (alloc<> does it) before
// filewalkbegin, which takes the arena everything retained is cloned into.
// The mem arena is the caller's, as the index keeps the paths; only the
// scratch arena is the walk's own, and filewalkfree releases it.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    FILEWALK_DIRS   = 1,      // directories listed per step: one, so that the
                              // budget and the job are checked between
                              // listings
    FILEWALK_FILES  = 30000,  // paths kept, at most
    FILEWALK_DIRMAX = 10000,  // entries listed in one directory, at most
    FILEWALK_DEPTH  = 128,    // levels below the root, at most
};

enum : iz {
    FILEWALK_SCRATCH = 4<<20,  // bytes for one step's listings
    FILEWALK_ENTRY   = 512,    // bytes one listed entry can take: a name's
                               // worst case, its clone, and its Dirent
    FILEWALK_REAL    = 4096,   // and the PATH_MAX os_realpath takes as it runs
    FILEWALK_RULES   = 64<<10, // and a level's .gitignore, which is read whole
};

enum : i64 {
    FILEWALK_TIME   = 30000000,  // os_ticks (µs) a walk on a worker takes at most
    FILEWALK_INLINE = 16000,     // and one run inline, on the window's thread
};

// A walk in progress: the stack of directories still to list, with each
// one's path relative to the root and the .gitignore chain in force there,
// and the results so far. Paths are relative to the root and '/'
// separated, files and folders alike, in the order the tree was walked.
struct FileWalk {
    Os                *os;
    Str                root;      // absolute, as given
    Arena              mem;       // retained: paths, ignore rules, the stack
    Arena              scratch;   // the step's listings, reset each step
    Region            *scratchreg;
    Slice<Str>         dirs;      // the stack: absolute paths still to list
    Slice<Str>         rels;      // and each one's path relative to root
    Slice<GitIgnore *> chains;    // and the ignore chain at that level
    Slice<Str>         reals;     // and each one's real path, links resolved
    Slice<i32>         depths;
    Slice<Str>         paths;     // results: relative, '/' separated
    Slice<i64>         mtimes;
    Slice<u8>          isdirs;
    iz                 maxfiles;  // caps, FILEWALK_* unless a test says so
    iz                 maxdir;
    iz                 maxdepth;
    b32                win;       // Windows paths
    b32                done, truncated;
    // On a worker (filewalkrun): whether it may go to a path, and a beat
    // per entry listed, both with ctx; neither in a walk without them
    b32              (*gate)(void *ctx, Str path);
    void             (*beat)(void *ctx);
    void              *ctx;
};

// Start a walk of root, an absolute directory: the root joins the stack at
// once, with the ignore chain of the directories above it.
static void filewalkbegin(FileWalk *w, Os *os, Arena *mem, Str root, b32 win)
{
    // A walk begun again over a struct that still holds one — the index
    // rebuilds in its own slot — must not leave the old scratch
    // reservation behind: it is the walk's own, and nothing else releases
    // it. Released through the Os it was taken from, before that changes.
    if (w->scratchreg) {
        os_release(w->os ? w->os : os, w->scratchreg);
        w->scratchreg = 0;
        w->scratch    = {};
    }
    w->os         = os;
    w->mem        = *mem;
    w->scratchreg = os_reserve(os, FILEWALK_SCRATCH);
    w->scratch    = newarena(w->scratchreg);
    w->win        = win;
    w->maxfiles   = FILEWALK_FILES;
    w->maxdir     = FILEWALK_DIRMAX;
    w->maxdepth   = FILEWALK_DEPTH;
    w->root       = clone(&w->mem, root);
    // A walk started again over the same struct forgets the last one's
    // results and stack; what it held stays in mem.
    w->dirs       = {};
    w->rels       = {};
    w->chains     = {};
    w->reals      = {};
    w->depths     = {};
    w->paths      = {};
    w->mtimes     = {};
    w->isdirs     = {};
    w->done       = w->truncated = 0;
    Str rootreal = os_realpath(os, &w->mem, w->root);
    if (!w->scratchreg) {  // no room to list anything: an empty walk
        w->done = 1;
        return;
    }
    w->dirs       = push(&w->mem, w->dirs, w->root);
    w->rels       = push(&w->mem, w->rels, Str{});
    w->chains     = push(&w->mem, w->chains, ignoreparents(os, &w->mem, w->root, win));
    w->reals      = push(&w->mem, w->reals, rootreal.len ? rootreal : w->root);
    w->depths     = push(&w->mem, w->depths, 0);
}

// Whether a directory named name in dir, whose real path is dreal, is a
// link rather than a directory: os lists with fstatat's flags 0, which
// follows links, so a link to a directory reads as one, and walking it
// would both leave the tree and recurse into whatever it names, without
// bound but for the depth cap. Its real path is the link's target, not
// the link's parent's real path joined with its name. Comparing against
// the parent's real path, not the path itself, keeps a root reached
// through a symlinked ancestor — /tmp on macOS, a symlinked ~/dev —
// from looking like a tree of links, which would walk nothing at all.
//
// The two are compared with '/' and '\' alike: join always writes '/',
// while os_realpath on Windows answers a backslash path, and a strict
// compare would read every directory there as a link and record only the
// root's files.
static b32 filewalklinked(Arena *a, Str dreal, Str name, Str real)
{
    Str want = join(a, dreal, name);
    if (want.len != real.len) {
        return 1;
    }
    for (iz i = 0; i < real.len; i++) {
        u8 x = want[i]=='\\' ? '/' : want[i];
        u8 y = real[i]=='\\' ? '/' : real[i];
        if (x != y) {
            return 1;
        }
    }
    return 0;
}

// The bytes an arena has left, committed or not: a region may still be
// committed up to its cap, and an arena without one is as big as its end.
static iz filewalkleft(Arena *a)
{
    byte *limit = a->region ? a->region->base+a->region->cap : a->end;
    return limit - a->beg;
}

// End the walk where it is, truncated: what it found stays.
static void filewalkstop(FileWalk *w)
{
    w->dirs.len = w->rels.len = w->chains.len = w->reals.len = w->depths.len = 0;
    w->truncated = 1;
    w->done      = 1;
}

// Whether the retained arena has room for one more entry: its cloned
// strings, and — for a slice it fills — the copy and the doubling a push
// makes in one go, which is the most one entry can ever ask for.
static b32 filewalkroom(FileWalk *w, iz strs)
{
    iz pcap = w->paths.cap ? w->paths.cap : 8;  // push's first extension
    iz dcap = w->dirs.cap ? w->dirs.cap : 8;
    iz grow = 0;
    if (w->paths.len == w->paths.cap) {
        grow += 2*pcap*(sizeof(Str) + sizeof(i64) + sizeof(u8));
    }
    if (w->dirs.len == w->dirs.cap) {
        grow += 2*dcap*(3*sizeof(Str) + sizeof(GitIgnore*) + sizeof(i32));
    }
    return fits(&w->mem, strs + grow + 64);  // and their alignment
}

// One step of the walk: list the next directory off the stack, recording
// what it holds and pushing the subdirectories to visit. Returns 1 while
// there is more to do. The step is one directory's listing and nothing more,
// which is what lets filewalkrun check its budget and its job between
// directories. The caps are entry counts, so a tree of very long paths
// fills the arenas before it fills them: a step with no room left ends the
// walk as truncated, as the caps do, rather than reaching os_oom, which would
// take the GUI down with it.
static b32 filewalkstep(FileWalk *w)
{
    if (w->done || !w->dirs.len) {
        w->done = 1;
        return 0;
    }
    w->scratch = newarena(w->scratchreg);  // the step's listings go here
    b32 stop = 0;
    for (i32 n = 0; n < FILEWALK_DIRS && w->dirs.len && !stop; n++) {
        iz         top   = w->dirs.len - 1;
        Str        dir   = w->dirs[top];
        Str        rel   = w->rels[top];
        Str        dreal = w->reals[top];
        GitIgnore *chain = w->chains[top];
        i32        depth = w->depths[top];
        w->dirs.len--;
        w->rels.len--;
        w->reals.len--;
        w->chains.len--;
        w->depths.len--;
        // A folder its gate refuses (under a mark: its file system stopped
        // answering) is passed over, the rest walked
        if (w->gate && !w->gate(w->ctx, dir)) {
            continue;
        }
        // The level's .gitignore, in mem with the rest of its chain: a
        // level's rules must outlive the step that loaded them, or nested
        // files stop applying once a walk is resumed. Its size is the
        // file's, so the room for it is checked here, not in filewalkroom.
        if (!fits(&w->mem, FILEWALK_RULES)) {
            w->truncated = 1;
            stop = 1;
            break;
        }
        chain = ignoreload(w->os, &w->mem, chain, dir);
        // What the listing may be given: the scratch's room, at an entry's
        // worst case, so that os_listall cannot reach os_oom either
        iz      room = filewalkleft(&w->scratch) / FILEWALK_ENTRY;
        iz      max  = w->maxdir<room ? w->maxdir : room;
        if (!max) {
            w->truncated = 1;
            stop = 1;
            break;
        }
        Listing l = os_listall(w->os, &w->scratch, dir, 0, max, w->beat, w->ctx);
        for (Dirent *d = l.head; d && !stop; d = d->next) {
            if (d->name == ".git") {
                continue;
            }
            // The scratch's room for this entry: both of its paths and
            // the copies the listing and os_realpath make of them, and the
            // PATH_MAX os_realpath takes while it runs
            if (!fits(&w->scratch, 2*(dir.len + rel.len + d->name.len + 8) + FILEWALK_REAL)) {
                w->truncated = 1;
                stop = 1;
                break;
            }
            Str path = join(&w->scratch, dir, d->name);
            Str sub  = rel.len ? join(&w->scratch, rel, d->name) : d->name;
            if (ignored(chain, path, d->isdir)) {
                continue;
            }
            Str real = {};  // a folder's real path, resolved once below
            if (d->isdir) {
                if (w->gate && !w->gate(w->ctx, path)) {
                    continue;  // a link under a mark is not resolved
                }
                real = os_realpath(w->os, &w->scratch, path);
                if (filewalklinked(&w->scratch, dreal, d->name, real)) {
                    continue;
                }
                if (depth+1 > w->maxdepth) {
                    w->truncated = 1;
                    continue;
                }
            }
            if (w->paths.len >= w->maxfiles) {
                w->truncated = 1;
                stop = 1;
                break;
            }
            // And the retained arena's room for it: its cloned strings and
            // the growth of the slices it is pushed onto
            if (!filewalkroom(w, path.len + sub.len + real.len)) {
                w->truncated = 1;
                stop = 1;
                break;
            }
            w->paths  = push(&w->mem, w->paths, clone(&w->mem, sub));
            w->mtimes = push(&w->mem, w->mtimes, d->mtime);
            w->isdirs = push(&w->mem, w->isdirs, (u8)d->isdir);
            if (d->isdir) {  // folders are candidates, empty or not
                w->dirs   = push(&w->mem, w->dirs, clone(&w->mem, path));
                w->rels   = push(&w->mem, w->rels, clone(&w->mem, sub));
                w->chains = push(&w->mem, w->chains, chain);
                w->reals  = push(&w->mem, w->reals, clone(&w->mem, real));
                w->depths = push(&w->mem, w->depths, depth+1);
            }
        }
        if (l.more) {  // entries past the per-directory cap
            w->truncated = 1;
            stop = 1;
        }
    }
    if (stop) {  // truncated: the walk ends here, cleanly
        filewalkstop(w);
    } else if (!w->dirs.len) {
        w->done = 1;
    }
    return !w->done;
}

// Release the walk's own reservation. The mem arena stays: it is the
// caller's, and holds the paths the index keeps.
static void filewalkfree(FileWalk *w)
{
    if (w->scratchreg) {
        os_release(w->os, w->scratchreg);
    }
    w->scratchreg = 0;
    w->scratch    = {};
    w->done       = 1;
}

// A walk as a job on a file pool (FsPool): at the start of a Region of
// its own, set up by its owner before the job is put (FsPut.ctx) and
// read by the owner again only once the job is answered. The walk's
// arena, w.mem, is the rest of the Region. root and win are never
// changed after the put. A job given up leaves the Region to the pool,
// whose drop (the owner's) runs filewalkfree and releases it once the
// worker is out of it.
struct FileWalkJob {
    FileWalk w;
    Region  *r;
    Str      root;   // absolute, as given, in r
    b32      win;
    i64      limit;  // os_ticks of work at most (FILEWALK_TIME)
};

// The gate of a walk on a worker: a beat, and 0 for a path under a mark,
// or once the job was given up, so a big folder stops at once.
static b32 filewalkgate(void *ctx, Str path)
{
    FsCall *c = (FsCall *)ctx;
    return fsat(c, path) && !fsgone(c);
}

// A walk's job, on its worker: the walk to its end, its caps or its
// budget (FILEWALK_INLINE at most inline, its worker never started),
// c->res the FileWalk; c->err for a root under a mark, and for
// no room to list anything. The scratch reservation goes as it ends.
static void filewalkrun(FsCall *c)
{
    FileWalkJob *j = (FileWalkJob *)c->ctx;
    FileWalk    *w = &j->w;
    c->res = w;
    if (!fsat(c, j->root)) {
        c->err = fsstuck;
        return;
    }
    Arena mem = w->mem;
    filewalkbegin(w, c->os, &mem, j->root, j->win);
    w->gate = filewalkgate;
    w->beat = fsbeat;
    w->ctx  = c;
    if (!w->scratchreg) {
        c->err = "out of memory";
        return;
    }
    i64 limit = j->limit;
    if (!c->w->started && limit>FILEWALK_INLINE) {
        limit = FILEWALK_INLINE;  // on the caller's thread: one frame's time
    }
    i64 end = os_ticks(c->os) + limit;
    while (!w->done) {
        if (fsgone(c)) {
            break;
        } else if (os_ticks(c->os) >= end) {
            filewalkstop(w);
            break;
        }
        filewalkstep(w);
    }
    filewalkfree(w);
    w->gate = 0;
    w->beat = 0;
    w->ctx  = 0;
}

// The '@' picker's index of a working directory: the walk, what its paths
// are relative to, when it began, and the rank of the last query it was
// asked. The walk is a job on the window's pool (mention.cpp), in its own
// Region: before it is put (held), and once it is answered, it is the
// window's; while walkid is set it is the worker's, and nothing here
// reads it.
struct MentionIdx {
    FileWalkJob *job;     // the walk and its Region, else null
    FileWalk    *w;       // job's walk, done once served
    i64          walkid;  // the job out on the pool, else 0
    Str          root;    // what the paths are relative to, job's
    i64          born;    // µs: rebuilt once older than MENTION_TTL
    // The picks the last query was ranked to, kept so that a frame asking
    // the same word again does not rank the whole index again: the popup
    // asks twice a frame, and an index holds up to 30,000 paths. A pick is
    // an index into w->paths, which does not move once the walk is done and
    // sorted, so the cache is only ever read for the walk it was made in:
    // gen is bumped whenever a walk starts, and the picks of another
    // generation are no picks at all.
    b32          ranked;   // the picks below may be read
    u32          gen;      // the walk this index is on
    u32          rankgen;  // the walk the picks were made in
    iz           qlen;     // the query they were ranked for, kept here since
    u8           qtext[MENTION_QUERY];  // the frame's copy of it does not outlive
    MentionPick  picks[MENTION_PICKS];  // what mentionpicks truncates to
    iz           npicks;
};

// A session's two indexes: the one the picker is served from, its walk
// done, and the one being walked to replace it, so that a rebuild never
// leaves the picker with only one folder's listing (mention.cpp).
struct MentionIdxs {
    MentionIdx slot[2];
    i32        cur;      // the slot served, -1 for none yet
    i32        next;     // the slot being walked, -1 for none: held
                         // (OP_MENTIONHOLD) until its job is put
    i64        again;    // µs: no new walk before, after one refused or
                         // given up
    b32        stale;    // rebuilt when the popup next opens
    b32        wasbusy;  // the view was busy when last seen: stale as it ends
    b32        nomem;    // no index could be had: the listing answers
};
