// File work off the owner's thread: a small pool of workers (FsPool)
//
// A read, a stat or a listing on a dead mount (NFS, SMB, sshfs, a
// stopped mapped drive) waits in the kernel for as long as the mount
// says, which may be forever. The engine's fs.read, fs.stat and dir run
// here, so that one dead path never stops its thread, and from it every
// session; so do the window's own reads and stats of this computer's
// files (files.cpp), each owner with a pool of its own.
//
// Threads. Up to FS_THREADS detached workers, started as they are
// needed: a queued job that is not held starts one when no worker is
// idle, and idle workers park on their event. Only when the pool has no
// thread at all and none can start (os_eventinit or os_thread fails, as
// always in main_test.cpp) does a job run inline, finishing before fsput
// returns. Otherwise a job with no idle worker and no thread to start
// waits in the queue, a ring of FS_QUEUE jobs whose copies (argument and
// paths) live in FS_QUEUEMEM bytes. Every job leaves the queue within
// FS_STUCK of queueing: handed to a worker, expired as "the file system
// is busy: try again", or refused.
//
// Answers. A worker writes its answer only through FsCall.mem, a pointer
// to its own arena (never through an Arena copy, which the arena checker
// poisons once the copy ends), and posts an empty completion of the
// owner's type, with its index as the id, to the owner's mailbox, which
// stays mapped while the process lives (as GitWork's posts rely on).
// The owner reads the answer there, in fsdone, and is the only one to
// answer: so the engine stays e->out's single writer. A completion that
// was given up, or posted before a quit, finishes nothing.
//
// Heartbeats. fsat(c, path) says where a worker is and that it lives:
// called before each path it touches, after realpath with the real path,
// and between the 64 KiB pieces of a read; os_listall beats per entry.
// A worker silent past FS_SLOW holds new jobs whose paths all lie under
// its given path or its `at`: they wait in the queue, and go as usual
// once it beats or answers. Silent past FS_STUCK it is stuck: its job,
// and the jobs held behind it, finish with "the file system is not
// responding", and its given path and `at`, normalized here on the
// owner's thread, become marks. A job whose paths all lie under marks
// is refused at once; one with no path at all never is. Marks clear when
// the stuck call returns. So a silent job is answered within about 2 x
// FS_STUCK; a job that keeps beating has no total bound. A limit: a
// hang inside realpath, through a link from a local folder, marks the
// link's path alone, since the real path is not yet known; other paths
// into the same dead target are neither held nor refused.
//
// Far mounts. A dead share fails every path on it, not just the one
// that hung, so a share is held and marked whole: when a worker first
// goes silent past FS_SLOW, the owner asks os_farmount, once, which
// network or FUSE mount its given path and its `at` are on (the mount
// table alone, never the share), and jobs under either mount are held
// with its paths; when it is stuck, each mount becomes a mark beside
// its paths, by the same worker, so both kinds clear when the stuck
// call returns. Local volumes are never marked whole: a slow disk, a
// privacy prompt or a cloud file being fetched marks its own path.
//
// Paths are compared as normpath gives them, '/' throughout; a worker's
// own spelling (Win32's '\\' real paths) is compared with either
// separator, and without case on Windows. A listing's paths are exact:
// it holds and marks its folder alone, not what lies below, nor its
// mount.
//
// Locks. Dispatch decides under the pool's lock, then lets go of it to
// run an inline job and to call finish and drop, which may call fsput
// again. fsdone, fstick and fsquit call finish with the lock released.
// fsabandon on the job being finished returns 0 from fsdone (the caller
// frees what it owns) and 1 from fstick or fsquit (the pool drops it).
// A worker's beat and place change only under the lock, so i686 needs
// no 64-bit atomics; quit and gone are i32 atomics.
//
// Test hooks: PROSPERO_TEST_FSSTALL="TEXT=MS[;TEXT=MS]" makes fsat wait
// MS milliseconds, the first time in a job that it sees a path holding
// TEXT, after its checks; PROSPERO_TEST_FSSTUCK_MS (100 to 600000) sets
// FS_STUCK, and FS_SLOW to a tenth of it.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    FS_MS       = 1000,          // os_ticks units: engine.cpp's MS comes later
    FS_SLOW     = 1000*FS_MS,    // silent this long, a worker holds its paths
    FS_STUCK    = 10000*FS_MS,   // and this long, it is stuck, its job given up
    FS_PIECE    = 64 << 10,      // bytes read between heartbeats
    FS_ARGMAX   = 1 << 20,       // bytes of a command taken
    FS_PATHMAX  = 128 << 10,     // bytes of a path taken (Win32's longest is
                                 // 32767 UTF-16 units)
    FS_QUEUEMEM = 4 << 20,       // queued jobs' copies, a ring
    FS_REGION   = (i64)(sizeof(void *)==8 ? 32 : 8) << 20,  // each worker's
};

enum : i32 {
    FS_THREADS = 8,
    FS_QUEUE   = 64,
    FS_JOBS    = FS_QUEUE + FS_THREADS,
    FS_MARKS   = 4*FS_THREADS,   // a stuck job's given path and `at`, and
                                 // their mounts (never full)
    FS_MARKMAX = 4096,           // a longer path is cut at a separator
    FS_STALLS  = 8,
};

static Str const fsstuck = "the file system is not responding";
static Str const fsbusy  = "the file system is busy: try again";

struct FsPool;
struct FsWorker;
struct FsJob;

// A job as its worker runs it. Everything it allocates, its answer
// included, goes through mem.
struct FsCall {
    Os        *os;
    void      *ctx;
    Arena     *mem;
    i64        id;
    i32        kind;
    Str        arg;
    Slice<Str> paths;
    Str        out;
    void      *res;
    Str        err;
    FsWorker  *w;
    FsJob     *job;
};

typedef void FsRun(FsCall *);
typedef void FsFinish(void *owner, FsJob *, Arena scratch);
typedef void FsDrop(Os *, void *own);

struct FsPut {
    i64        id;
    i32        kind;
    FsRun     *run;
    void      *ctx;
    Str        arg;
    Slice<Str> paths;  // where it goes, normalized, for holds and marks
    b32        exact;  // a listing's: its paths alone, not what is below
    void      *own;    // the owner's, dropped once the pool holds it
    FsDrop    *drop;
};

enum : i32 {
    FSJ_FREE,
    FSJ_QUEUED,
    FSJ_RUNNING,   // or inline, or finished (given up) while it runs
    FSJ_DONE,      // answered, its completion posted
    FSJ_ENDING,    // out of the queue, being finished, then free
};

struct FsJob {
    FsPut put;
    i32   state;
    i64   queued;     // ticks
    i64   seq;        // queue order
    i64   qoff;       // its copies in the ring, while queued
    i32   gone;       // atomic: given up or abandoned, for fsgone
    b32   finished;   // finish was called before it was answered
    b32   abandoned;  // fsabandon returned 1: drop at its end
    i32   finishing;  // 1: fsdone's finish, 2: fstick's or fsquit's
    Str   out;        // the worker's answer
    void *res;
    Str   err;
};

struct FsWorker {
    FsPool *pool;
    i32     index;
    b32     started;   // its thread runs (manual: a slot in use)
    b32     stuck;     // declared stuck, until its call returns
    b32     holding;   // silent past slow, its paths held
    b32     evok;      // go is made
    Event   go;
    Region *r;
    Arena   mem;       // its job's, re-pointed at each handover
    FsJob  *job;
    i64     beat;      // ticks
    u32     stalled;   // the stall hooks applied in this job
    Str     given;     // the path given that it is on
    Str     at;        // where it is
    Str     mounts[2]; // the far mounts of given and at, looked up as it
                       // began holding (normalized, perhaps empty)
    u8      givenbuf[FS_MARKMAX];
    u8      atbuf[FS_MARKMAX];
    u8      mountbuf[2][FS_MARKMAX];
};

struct FsMark {
    Str path;
    b32 exact;
    i32 by;    // the worker stuck on it
    u8  buf[FS_MARKMAX];
};

struct FsStall {
    Str text;
    i32 ms;
};

struct FsPool {
    Os       *os;
    Mailbox  *done;
    u32       type;
    void     *owner;
    FsFinish *finish;
    Lock      lock;
    i32       quit;     // atomic
    Str       quitwhy;
    u8        quitbuf[128];
    b32       manual;   // tests: fsbegin and fsrun in place of threads
    b32       win;      // paths are Windows' (HOST_WINDOWS, or a test's)
    i32       threads;
    i32       nqueued;
    i64       seq;
    FsWorker  workers[FS_THREADS];
    FsJob     jobs[FS_JOBS];
    Region   *qr;
    Arena     qmem;
    u8       *ring;     // FS_QUEUEMEM bytes
    i64       qtail;    // where the next copies go, counting every wrap
    FsMark    marks[FS_MARKS];
    i64       slow;
    i64       stuck;
    FsStall   stalls[FS_STALLS];
    i32       nstalls;
    Event     stall;    // never signaled: the stall hook waits on it
};

// PROSPERO_TEST_FSSTALL's entries, "TEXT=MS" with 0 < MS <= 600000, into
// the pool; anything else is passed over.
static void fsstallparse(FsPool *p, Arena *perm, Str spec)
{
    for (Str rest = spec; rest.len && p->nstalls<FS_STALLS;) {
        Cut e = cut(rest, ';');
        rest = e.tail;
        Cut kv = cut(e.head, '=');
        Int ms = parseint(kv.tail);
        if (kv.ok && kv.head.len && ms.ok && ms.val>0 && ms.val<=600000) {
            FsStall *s = &p->stalls[p->nstalls++];
            s->text = clone(perm, kv.head);
            s->ms   = (i32)ms.val;
        }
    }
}

// PROSPERO_TEST_FSSTUCK_MS as ticks, clamped to 100..600000 ms, else 0.
static i64 fsstuckparse(Str s)
{
    Int ms = parseint(s);
    if (!ms.ok) {
        return 0;
    }
    i64 v = ms.val<100 ? 100 : ms.val>600000 ? 600000 : ms.val;
    return v * FS_MS;
}

static FsPool *fsnew(Os *os, Arena *perm, Mailbox *done, u32 type, void *owner,
                     FsFinish *finish)
{
    FsPool *p = alloc<FsPool>(perm);
    p->os     = os;
    p->done   = done;
    p->type   = type;
    p->owner  = owner;
    p->finish = finish;
    p->win    = HOST_WINDOWS;
    p->slow   = FS_SLOW;
    p->stuck  = FS_STUCK;
    os_lockinit(&p->lock);
    for (i32 i = 0; i < FS_THREADS; i++) {
        p->workers[i].pool  = p;
        p->workers[i].index = i;
    }
    fsstallparse(p, perm, os_getenv(os, perm, "PROSPERO_TEST_FSSTALL"));
    if (p->nstalls && !os_eventinit(&p->stall)) {
        p->nstalls = 0;
    }
    i64 stuck = fsstuckparse(os_getenv(os, perm, "PROSPERO_TEST_FSSTUCK_MS"));
    if (stuck) {
        p->stuck = stuck;
        p->slow  = stuck / 10;
    }
    return p;
}

// Whether path lies under a mark (or is it, exactly): mark normalized,
// path in either spelling, with '\\' a separator and case folded on
// Windows.
static b32 fsunder(Str mark, b32 exact, Str path, b32 win)
{
    iz n = mark.len;
    if (!n) {
        return 0;
    }
    iz m = path.len;
    if (exact) {
        for (; m>n && issep(path[m-1], win); m--) {}
        if (m != n) {
            return 0;
        }
    } else if (m < n) {
        return 0;
    }
    for (iz i = 0; i < n; i++) {
        u8 a = mark[i];
        u8 b = path[i];
        if (win) {
            a = a=='\\' ? '/' : lowercase(a);
            b = b=='\\' ? '/' : lowercase(b);
        }
        if (a != b) {
            return 0;
        }
    }
    return m==n || mark[n-1]=='/' || issep(path[n], win);
}

// Whether a file system type, as Linux's mountinfo or macOS's getfsstat
// names it, reaches another computer or a user's process: a network
// file system, or FUSE (sshfs and the like, "fuse.sshfs"), whose server
// can stop answering. Not fuseblk (a local disk through FUSE, as NTFS),
// nor autofs, whose mount point is a trigger above many shares: the
// share mounted beneath it is the one that counts.
static b32 fsfartype(Str type)
{
    static Str const far[] = {
        "nfs", "nfs4", "cifs", "smb3", "smbfs", "ncpfs", "9p", "afs", "ceph",
        "glusterfs", "lustre", "davfs", "vboxsf", "prl_fs", "afpfs", "webdav",
        "fuse",
    };
    for (iz i = 0; i < countof(far); i++) {
        if (type == far[i]) {
            return 1;
        }
    }
    return startswith(type, "fuse.") || startswith(type, "macfuse") ||
           startswith(type, "osxfuse");
}

// A mountinfo field with its \ooo octal escapes (space, tab, newline
// and backslash) decoded, a copy only when it has one.
static Str fsunoctal(Arena *a, Str s)
{
    if (find(s, "\\") < 0) {
        return s;
    }
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c=='\\' && i+3<s.len && s[i+1]>='0' && s[i+1]<='3' &&
                s[i+2]>='0' && s[i+2]<='7' && s[i+3]>='0' && s[i+3]<='7') {
            c  = (u8)((s[i+1]-'0')<<6 | (s[i+2]-'0')<<3 | (s[i+3]-'0'));
            i += 3;
        }
        putbyte(&b, c);
    }
    return finish(&b);
}

// The far mount that holds an absolute path, by Linux's
// /proc/self/mountinfo: the longest mount point that is the path or
// lies above it at a separator, autofs passed over (later mounts win a
// tie, being on top), returned when its type is far (fsfartype), else
// empty, as for a path on a local disk below a share. In each line the
// fifth field is the mount point and the field after " - " the type;
// a line cut short or malformed, or without its newline (a table cut
// off at its end), is passed over.
static Str fsmountinfo(Arena *a, Str text, Str path)
{
    Str best    = {};
    b32 bestfar = 0;
    for (Str rest = text; rest.len;) {
        Cut line = cut(rest, '\n');
        rest = line.tail;
        if (!line.ok) {
            break;
        }
        Str fields[5] = {};
        Str tail      = line.head;
        i32 n         = 0;
        for (; n<5 && tail.len; n++) {
            Cut f = cut(tail, ' ');
            fields[n] = f.head;
            tail      = f.tail;
        }
        iz dash = find(tail, " - ");
        if (n<5 || !fields[4].len || dash<0) {
            continue;
        }
        Str type = cut(cuthead(tail, dash+3), ' ').head;
        if (!type.len || type=="autofs") {
            continue;
        }
        Str point = fsunoctal(a, fields[4]);
        if (point.len>=best.len && fsunder(point, 0, path, 0)) {
            best    = point;
            bestfar = fsfartype(type);
        }
    }
    return bestfar ? best : Str{};
}

// Where a normalized Windows path's drive or share would be mapped:
// "C:/" of "C:/x", "//h/s/" of "//h/s/x", else empty (the device
// namespace, "//?/" and "//./", and anything relative).
static Str fswinroot(Str norm)
{
    iz n = pathroot(norm, 1);
    if (n==3 && isdrive(norm)) {
        return takehead(norm, 3);
    } else if (n<6 || norm[n-1]!='/' || norm[2]=='?' || norm[2]=='.') {
        return {};
    }
    iz j = 2;
    for (; j<n && norm[j]!='/'; j++) {}
    return j>2 && j+2<n ? takehead(norm, n) : Str{};
}

static b32 fsmarked(FsPool *p, Str path)
{
    for (i32 i = 0; i < FS_MARKS; i++) {
        FsMark *k = &p->marks[i];
        if (k->path.len && fsunder(k->path, k->exact, path, p->win)) {
            return 1;
        }
    }
    return 0;
}

// Refused: it has paths, and every one lies under a mark.
static b32 fsrefused(FsPool *p, FsPut *put)
{
    for (iz i = 0; i < put->paths.len; i++) {
        if (!fsmarked(p, put->paths[i])) {
            return 0;
        }
    }
    return put->paths.len > 0;
}

// The worker on a job still answerable: running, neither stuck nor
// given up.
static b32 fsbusyon(FsWorker *w)
{
    FsJob *j = w->job;
    return j && j->state==FSJ_RUNNING && !w->stuck && !j->finished;
}

// Held: it has paths, and every one lies under the given path or `at`
// of a worker silent past slow, or under their far mounts.
static b32 fsheldback(FsPool *p, FsPut *put)
{
    for (iz i = 0; i < put->paths.len; i++) {
        b32 held = 0;
        for (i32 k = 0; k<FS_THREADS && !held; k++) {
            FsWorker *w = &p->workers[k];
            if (fsbusyon(w) && w->holding) {
                b32 exact = w->job->put.exact;
                Str path  = put->paths[i];
                held = fsunder(w->given, exact, path, p->win) ||
                       fsunder(w->at, exact, path, p->win) ||
                       fsunder(w->mounts[0], 0, path, p->win) ||
                       fsunder(w->mounts[1], 0, path, p->win);
            }
        }
        if (!held) {
            return 0;
        }
    }
    return put->paths.len > 0;
}

// A path into a fixed buffer, cut at a separator when too long.
static Str fskeep(u8 *buf, Str path, b32 win)
{
    iz n = path.len;
    if (n > FS_MARKMAX) {
        for (n = FS_MARKMAX; n>0 && !issep(path[n-1], win); n--) {}
        n -= n > 1;  // the separator itself goes, unless it is the root
    }
    copybytes(buf, path.data, n);
    return {buf, n};
}

// A worker's path as a mark, normalized: by its index, until it returns.
static void fsmark(FsPool *p, Arena scratch, Str path, b32 exact, i32 by)
{
    if (!path.len || !pathroot(path, p->win)) {
        return;
    }
    Str norm = normpath(&scratch, path, ".", p->win);
    for (i32 i = 0; i < FS_MARKS; i++) {
        FsMark *k = &p->marks[i];
        if (k->path.len && k->by==by && k->exact==exact && k->path==norm) {
            return;  // its given path and `at` are one
        }
    }
    for (i32 i = 0; i < FS_MARKS; i++) {
        FsMark *k = &p->marks[i];
        if (!k->path.len) {
            k->path  = fskeep(k->buf, norm, p->win);
            k->exact = exact;
            k->by    = by;
            return;
        }
    }
}

static void fsunmark(FsPool *p, i32 by)
{
    for (i32 i = 0; i < FS_MARKS; i++) {
        if (p->marks[i].path.len && p->marks[i].by==by) {
            p->marks[i].path = {};
        }
    }
}

// Room for n bytes of copies in the ring, aligned, never split across
// its end: past the oldest queued job's, else 0.
static u8 *fsringtake(FsPool *p, iz n, i64 *off)
{
    if (!p->ring) {
        p->qr = p->qr ? p->qr : os_reserve(p->os, FS_QUEUEMEM + ((iz)1<<16));
        if (!p->qr) {
            return 0;
        }
        p->qmem = newarena(p->qr);
        p->ring = allocbytes(&p->qmem, FS_QUEUEMEM);
    }
    i64 head = p->qtail;
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        head = j->state==FSJ_QUEUED && j->qoff<head ? j->qoff : head;
    }
    n += -n & 7;
    i64 at  = p->qtail + (-p->qtail & 7);
    i64 pos = at % FS_QUEUEMEM;
    if (pos+n > FS_QUEUEMEM) {
        at += FS_QUEUEMEM - pos;
    }
    if (n>FS_QUEUEMEM || at+n-head>FS_QUEUEMEM) {
        return 0;
    }
    p->qtail = at + n;
    *off     = at;
    return p->ring + at%FS_QUEUEMEM;
}

static void fsdequeue(FsPool *p, FsJob *j, i32 state)
{
    j->state = state;
    p->nqueued--;
}

static void fsfree(FsJob *j)
{
    *j = {};
}

// Hand a job to a worker, under the lock: its copies cloned into the
// worker's arena, so that the ring's space frees.
static void fshandover(FsPool *p, FsJob *j, FsWorker *w)
{
    fsdequeue(p, j, FSJ_RUNNING);
    w->mem = newarena(w->r);
    Slice<Str> paths = {};
    paths.data = alloc<Str>(&w->mem, j->put.paths.len);
    paths.len  = paths.cap = j->put.paths.len;
    for (iz i = 0; i < paths.len; i++) {
        paths[i] = clone(&w->mem, j->put.paths[i]);
    }
    j->put.arg   = clone(&w->mem, j->put.arg);
    j->put.paths = paths;
    w->job     = j;
    w->beat    = os_ticks(p->os);
    w->holding = 0;
    w->stalled = 0;
    w->given   = {};
    w->at      = {};
    w->mounts[0] = w->mounts[1] = {};
}

// Run a worker's job, on its thread or inline, then store its answer;
// posted unless it runs inline or the pool quit. Returns whether it did.
static b32 fsdo(FsWorker *w, b32 post)
{
    FsPool *p = w->pool;
    FsJob  *j = w->job;
    FsCall  c = {};
    c.os    = p->os;
    c.ctx   = j->put.ctx;
    c.mem   = &w->mem;
    c.id    = j->put.id;
    c.kind  = j->put.kind;
    c.arg   = j->put.arg;
    c.paths = j->put.paths;
    c.w     = w;
    c.job   = j;
    j->put.run(&c);

    os_lock(&p->lock);
    j->out   = c.out;
    j->res   = c.res;
    j->err   = c.err;
    j->state = FSJ_DONE;
    b32 quit = __atomic_load_n(&p->quit, __ATOMIC_ACQUIRE);
    os_unlock(&p->lock);
    if (post && !quit && p->done) {
        mailpost(p->done, p->type, (u64)w->index, {}, 1);
    }
    return quit;
}

static void fsthread(void *arg)
{
    FsWorker *w = (FsWorker *)arg;
    FsPool   *p = w->pool;
    for (;;) {
        os_wait(&w->go, -1);
        os_lock(&p->lock);
        b32    quit = __atomic_load_n(&p->quit, __ATOMIC_ACQUIRE);
        FsJob *j    = w->job;
        b32    run  = !quit && j && j->state==FSJ_RUNNING;
        os_unlock(&p->lock);
        if (quit || (run && fsdo(w, 1))) {
            return;
        }
    }
}

// A worker free for a job, started or able to start: idle and started,
// else (start set) a slot never started, with its region, and its event
// if it can be made (evok): a thread needs it, an inline job does not.
static FsWorker *fsidle(FsPool *p, b32 start)
{
    for (i32 i = 0; i < FS_THREADS; i++) {
        FsWorker *w = &p->workers[i];
        if (!start && w->started && !w->job && !w->stuck) {
            return w;
        } else if (start && !w->started && !w->job) {
            w->r    = w->r ? w->r : os_reserve(p->os, FS_REGION);
            w->evok = w->evok || os_eventinit(&w->go);
            return w->r ? w : 0;
        }
    }
    return 0;
}

// A worker's job slot freed, under the lock: if it was declared stuck,
// its call returned, so its marks clear; and after a quit, a thread
// parked on it ends.
static void fsrelease(FsPool *p, FsWorker *w)
{
    if (w->stuck) {
        fsunmark(p, w->index);
        w->stuck = 0;
    }
    w->job     = 0;
    w->holding = 0;
    if (w->started && !p->manual && __atomic_load_n(&p->quit, __ATOMIC_ACQUIRE)) {
        os_signal(&w->go);
    }
}

static void fsdropnow(FsPool *p, FsDrop *drop, void *own)
{
    if (drop) {
        drop(p->os, own);
    }
}

static void fscomplete(FsPool *, FsWorker *, Arena);

// Dispatch, in queue order, until nothing more can go: under the lock
// it decides one thing at a time, then lets go of it to finish a job
// refused by a mark or to run one inline.
static void fsrunqueue(FsPool *p, Arena scratch)
{
    for (;;) {
        os_lock(&p->lock);
        FsJob    *end = 0;
        FsWorker *inl = 0;
        i64       last = -1;
        for (;;) {
            FsJob *j = 0;
            for (i32 i = 0; i < FS_JOBS; i++) {
                FsJob *q = &p->jobs[i];
                if (q->state==FSJ_QUEUED && q->seq>last && (!j || q->seq<j->seq)) {
                    j = q;
                }
            }
            if (!j) {
                break;
            }
            last = j->seq;
            if (fsrefused(p, &j->put)) {
                fsdequeue(p, j, FSJ_ENDING);
                j->finishing = 2;
                end = j;
                break;
            } else if (p->manual || fsheldback(p, &j->put)) {
                continue;
            }
            FsWorker *w = fsidle(p, 0);
            if (w) {
                fshandover(p, j, w);
                os_signal(&w->go);
                continue;
            }
            w = p->threads<FS_THREADS ? fsidle(p, 1) : 0;
            if (w && w->evok && os_thread(p->os, fsthread, w)) {
                w->started = 1;
                p->threads++;
                fshandover(p, j, w);
                os_signal(&w->go);
            } else if (w && !p->threads) {
                fshandover(p, j, w);
                inl = w;
                break;
            }
        }
        os_unlock(&p->lock);

        if (end) {
            FsJob e = *end;
            e.out = {};
            e.res = 0;
            e.err = fsstuck;
            p->finish(p->owner, &e, scratch);
            os_lock(&p->lock);
            FsDrop *drop = end->abandoned ? end->put.drop : 0;
            void   *own  = end->put.own;
            fsfree(end);
            os_unlock(&p->lock);
            fsdropnow(p, drop, own);
        } else if (inl) {
            fsdo(inl, 0);
            fscomplete(p, inl, scratch);
        } else {
            return;
        }
    }
}

// Queue a job, and dispatch: 0 with why when refused (quitting, its
// paths all marked, or no room). An inline job finishes inside this
// call, so the owner records put->id first.
static b32 fsput(FsPool *p, FsPut *put, Str *why, Arena scratch)
{
    os_lock(&p->lock);
    FsJob *j = 0;
    for (i32 i = 0; i<FS_JOBS && !j; i++) {
        j = p->jobs[i].state==FSJ_FREE ? &p->jobs[i] : 0;
    }
    iz need = put->arg.len + put->paths.len*(iz)sizeof(Str);
    for (iz i = 0; i < put->paths.len; i++) {
        need += put->paths[i].len;
    }
    i64 off = 0;
    u8 *at  = 0;
    if (__atomic_load_n(&p->quit, __ATOMIC_ACQUIRE)) {
        *why = p->quitwhy;
    } else if (fsrefused(p, put)) {
        *why = fsstuck;
    } else if (!j || p->nqueued>=FS_QUEUE || !(at = fsringtake(p, need, &off))) {
        *why = fsbusy;
    }
    if (!at) {
        os_unlock(&p->lock);
        return 0;
    }

    *j = {};
    j->put   = *put;
    j->state = FSJ_QUEUED;
    j->queued = os_ticks(p->os);
    j->seq   = ++p->seq;
    j->qoff  = off;
    p->nqueued++;
    Str *paths = (Str *)at;
    u8  *data  = at + put->paths.len*(iz)sizeof(Str);
    for (iz i = 0; i < put->paths.len; i++) {
        copybytes(data, put->paths[i].data, put->paths[i].len);
        paths[i] = {data, put->paths[i].len};
        data += put->paths[i].len;
    }
    copybytes(data, put->arg.data, put->arg.len);
    j->put.arg   = {data, put->arg.len};
    j->put.paths = {};
    j->put.paths.data = paths;
    j->put.paths.len  = j->put.paths.cap = put->paths.len;
    os_unlock(&p->lock);

    fsrunqueue(p, scratch);
    return 1;
}

// A worker's job answered: finished unless it was given up or abandoned
// (then dropped where the pool holds it), the worker idle again.
static void fscomplete(FsPool *p, FsWorker *w, Arena scratch)
{
    os_lock(&p->lock);
    FsJob *j = w->job;
    if (!j || j->state!=FSJ_DONE) {
        os_unlock(&p->lock);
        return;
    }
    b32 fin = !j->finished && !j->abandoned;
    j->finishing = fin;
    if (w->stuck) {
        fsunmark(p, w->index);
        w->stuck = 0;
    }
    os_unlock(&p->lock);

    if (fin) {
        p->finish(p->owner, j, scratch);
    }

    os_lock(&p->lock);
    FsDrop *drop = j->abandoned ? j->put.drop : 0;
    void   *own  = j->put.own;
    fsfree(j);
    fsrelease(p, w);
    os_unlock(&p->lock);
    fsdropnow(p, drop, own);
}

// A completion from the owner's mailbox: worker r.id's job answered, or
// past FS_THREADS, a worker that beat again after holding, so that the
// jobs it held may go.
static void fsdone(FsPool *p, Record r, Arena scratch)
{
    if (r.id < (u64)FS_THREADS) {
        fscomplete(p, &p->workers[r.id], scratch);
    }
    fsrunqueue(p, scratch);
}

// A copy of a slot for its finish, made under the lock: with why in
// err, or the slot's own answer when why is null.
static FsJob fscopy(FsJob *j, Str why)
{
    FsJob e = *j;
    if (why.data) {
        e.out = {};
        e.res = 0;
        e.err = why;
    }
    return e;
}

// Finish each copy (lock released), then free the slots that left the
// queue or were answered, and drop what the pool holds of them.
static void fsfinishall(FsPool *p, FsJob **slots, FsJob *copies, i32 n, Arena scratch)
{
    for (i32 i = 0; i < n; i++) {
        p->finish(p->owner, &copies[i], scratch);
    }
    FsDrop *drops[FS_JOBS];
    void   *owns[FS_JOBS];
    i32     ndrops = 0;
    os_lock(&p->lock);
    for (i32 i = 0; i < n; i++) {
        FsJob *j = slots[i];
        if (j->state == FSJ_RUNNING) {
            j->finishing = 0;  // dropped, if abandoned, once it returns
            continue;
        }
        if (j->abandoned && j->put.drop) {
            drops[ndrops]  = j->put.drop;
            owns[ndrops++] = j->put.own;
        }
        for (i32 k = 0; k < FS_THREADS; k++) {
            if (p->workers[k].job == j) {
                fsrelease(p, &p->workers[k]);  // perhaps it returned stuck
            }
        }
        fsfree(j);
    }
    os_unlock(&p->lock);
    for (i32 i = 0; i < ndrops; i++) {
        drops[i](p->os, owns[i]);
    }
}

// The far mount a worker's path is on (os_farmount), normalized: empty
// for none, and for a path that is not absolute.
static Str fsfarmount(FsPool *p, Arena *scratch, Str path)
{
    if (!path.len || !pathroot(path, p->win)) {
        return {};
    }
    Str m = os_farmount(p->os, scratch, path);
    return m.len && pathroot(m, p->win) ? normpath(scratch, m, ".", p->win) : Str{};
}

// Time passes, now in ticks: workers silent past slow hold their paths
// and their far mounts, looked up as they begin to, and past stuck are
// declared stuck, once, their jobs and those held behind them finished,
// their paths and mounts marked; queued jobs past stuck expire as busy.
static void fstick(FsPool *p, Arena scratch, i64 now)
{
    FsJob *ends[FS_JOBS];
    FsJob *copies = alloc<FsJob>(&scratch, FS_JOBS);
    i32    n      = 0;

    // Workers going silent past slow: their mounts, looked up with the
    // lock let go, as the system's mount table is read. Only the owner
    // starts or ends a job, so each is still on the job it was.
    FsWorker *look[FS_THREADS];
    Str       paths[2*FS_THREADS];
    Str       mounts[2*FS_THREADS];
    i32       nlook = 0;
    os_lock(&p->lock);
    for (i32 i = 0; i < FS_THREADS; i++) {
        FsWorker *w = &p->workers[i];
        if (fsbusyon(w) && !w->holding && !w->job->put.exact && now-w->beat>=p->slow) {
            paths[2*nlook]   = clone(&scratch, w->given);
            paths[2*nlook+1] = clone(&scratch, w->at);
            look[nlook++]    = w;
        }
    }
    os_unlock(&p->lock);
    for (i32 i = 0; i < 2*nlook; i++) {
        b32 same  = i%2 && paths[i]==paths[i-1];
        mounts[i] = same ? mounts[i-1] : fsfarmount(p, &scratch, paths[i]);
    }

    os_lock(&p->lock);
    for (i32 i = 0; i < nlook; i++) {
        FsWorker *w = look[i];
        for (i32 k = 0; k < 2; k++) {
            w->mounts[k] = fskeep(w->mountbuf[k], mounts[2*i+k], p->win);
        }
    }
    for (i32 i = 0; i < FS_THREADS; i++) {
        FsWorker *w = &p->workers[i];
        if (!fsbusyon(w)) {
            continue;
        }
        FsJob *j = w->job;
        w->holding |= now-w->beat >= p->slow;
        if (now-w->beat >= p->stuck) {
            w->stuck    = 1;
            j->finished = 1;
            __atomic_store_n(&j->gone, 1, __ATOMIC_RELEASE);
            fsmark(p, scratch, w->given, j->put.exact, w->index);
            fsmark(p, scratch, w->at, j->put.exact, w->index);
            fsmark(p, scratch, w->mounts[0], 0, w->index);
            fsmark(p, scratch, w->mounts[1], 0, w->index);
            if (!j->abandoned) {  // else nobody waits: dropped at its end
                j->finishing = 2;
                copies[n] = fscopy(j, fsstuck);
                ends[n++] = j;
            }
        }
    }
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j   = &p->jobs[i];
        Str    why = {};
        if (j->state != FSJ_QUEUED) {
            continue;
        } else if (fsrefused(p, &j->put)) {
            why = fsstuck;
        } else if (now-j->queued >= p->stuck) {
            why = fsbusy;
        } else {
            continue;
        }
        fsdequeue(p, j, FSJ_ENDING);
        j->finishing = 2;
        copies[n] = fscopy(j, why);
        ends[n++] = j;
    }
    os_unlock(&p->lock);
    fsfinishall(p, ends, copies, n, scratch);
    fsrunqueue(p, scratch);
}

// The earliest deadline for fstick, or I64MAX: beat+slow of a busy
// worker not yet holding, beat+stuck of a busy worker, queued+stuck of
// a queued job. Workers stuck and jobs given up count no more.
static i64 fsnext(FsPool *p)
{
    i64 r = I64MAX;
    os_lock(&p->lock);
    for (i32 i = 0; i < FS_THREADS; i++) {
        FsWorker *w = &p->workers[i];
        if (fsbusyon(w)) {
            r = !w->holding && w->beat+p->slow<r ? w->beat+p->slow : r;
            r = w->beat+p->stuck<r ? w->beat+p->stuck : r;
        }
    }
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        r = j->state==FSJ_QUEUED && j->queued+p->stuck<r ? j->queued+p->stuck : r;
    }
    os_unlock(&p->lock);
    return r;
}

// Whether the job of that id is outstanding: queued (held or not),
// running and not given up, or answered and not yet finished.
static b32 fsheld(FsPool *p, i64 id)
{
    b32 r = 0;
    os_lock(&p->lock);
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        r |= j->state!=FSJ_FREE && j->put.id==id && !j->finished;
    }
    os_unlock(&p->lock);
    return r;
}

// Whether any job is outstanding, as fsheld counts them: for a
// checklist's wait.
static b32 fspending(FsPool *p)
{
    b32 r = 0;
    os_lock(&p->lock);
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        r |= j->state!=FSJ_FREE && !j->finished && !j->abandoned;
    }
    os_unlock(&p->lock);
    return r;
}

// The owner no longer wants the job of that id. 1 when the pool keeps
// its own and drops it (on the owner's thread) when the worker returns,
// or after the finish under way; 0 when the caller frees own itself.
static b32 fsabandon(FsPool *p, i64 id)
{
    b32 r = 0;
    os_lock(&p->lock);
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        if (j->state==FSJ_FREE || j->put.id!=id) {
            continue;
        } else if (j->finishing == 1) {
            r = 0;  // its answer, being finished: the caller frees own
        } else if (j->state==FSJ_QUEUED && !j->finishing) {
            fsdequeue(p, j, FSJ_FREE);
            fsfree(j);
        } else {
            __atomic_store_n(&j->gone, 1, __ATOMIC_RELEASE);
            j->abandoned = 1;
            r = 1;
        }
        break;
    }
    os_unlock(&p->lock);
    return r;
}

// The owner stops: every job not given up is finished, queued and
// running ones with why, answered ones with their answers; jobs given up
// that were answered are dropped; idle workers end. Later completions
// are not posted, and fsput refuses with why.
static void fsquit(FsPool *p, Str why, Arena scratch)
{
    FsJob  *ends[FS_JOBS];
    FsJob  *copies = alloc<FsJob>(&scratch, FS_JOBS);
    FsDrop *drops[FS_JOBS];
    void   *owns[FS_JOBS];
    i32     n = 0, ndrops = 0;
    os_lock(&p->lock);
    if (__atomic_load_n(&p->quit, __ATOMIC_ACQUIRE)) {
        os_unlock(&p->lock);
        return;
    }
    p->quitwhy = fskeep(p->quitbuf, takehead(why, why.len<countof(p->quitbuf)
                                                   ? why.len : countof(p->quitbuf)), 0);
    __atomic_store_n(&p->quit, 1, __ATOMIC_RELEASE);
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *j = &p->jobs[i];
        if (j->finishing) {
            continue;  // a finish under way, from fsquit's caller
        } else if (j->state == FSJ_QUEUED) {
            fsdequeue(p, j, FSJ_ENDING);
            copies[n] = fscopy(j, p->quitwhy);
        } else if (j->state==FSJ_RUNNING && !j->finished && !j->abandoned) {
            j->finished = 1;
            __atomic_store_n(&j->gone, 1, __ATOMIC_RELEASE);
            copies[n] = fscopy(j, p->quitwhy);
        } else if (j->state==FSJ_DONE && !j->finished && !j->abandoned) {
            copies[n] = fscopy(j, {});  // its own answer
        } else {
            // Given up or abandoned: one running is left to its drop
            if (j->state==FSJ_DONE && j->abandoned && j->put.drop) {
                drops[ndrops]  = j->put.drop;
                owns[ndrops++] = j->put.own;
            }
            for (i32 k = 0; j->state==FSJ_DONE && k<FS_THREADS; k++) {
                if (p->workers[k].job == j) {
                    fsrelease(p, &p->workers[k]);
                }
            }
            if (j->state == FSJ_DONE) {
                fsfree(j);
            }
            continue;
        }
        j->finishing = 2;
        ends[n++] = j;
    }
    for (i32 i = 0; i < FS_THREADS; i++) {
        FsWorker *w = &p->workers[i];
        if (w->started && !w->job && !p->manual) {
            os_signal(&w->go);
        }
    }
    os_unlock(&p->lock);
    for (i32 i = 0; i < ndrops; i++) {
        drops[i](p->os, owns[i]);
    }
    fsfinishall(p, ends, copies, n, scratch);
}


// The worker's side

static void fspoke(FsPool *p, FsWorker *w)
{
    if (p->done && !__atomic_load_n(&p->quit, __ATOMIC_ACQUIRE)) {
        mailpost(p->done, p->type, (u64)(FS_THREADS + w->index), {}, 1);
    }
}

// A heartbeat, where the worker is (given: also the path given that it
// is on), and whether it may go there: 0 for a path under a mark, at
// once. Then the stall hook, if any.
static b32 fsat(FsCall *c, Str path, b32 given = 0)
{
    FsWorker *w = c->w;
    FsPool   *p = w->pool;
    os_lock(&p->lock);
    b32 marked = fsmarked(p, path);
    b32 poke   = w->holding && p->nqueued>0;
    w->beat    = os_ticks(p->os);
    w->holding = 0;
    if (!marked) {
        w->at = fskeep(w->atbuf, path, p->win);
        if (given) {
            w->given = fskeep(w->givenbuf, path, p->win);
        }
    }
    os_unlock(&p->lock);
    if (poke) {
        fspoke(p, w);
    }
    if (marked) {
        return 0;
    }
    for (i32 i = 0; i < p->nstalls; i++) {
        FsStall *s = &p->stalls[i];
        u32      bit = 1u << i;
        if (!(w->stalled & bit) && find(path, s->text)>=0) {
            w->stalled |= bit;
            os_wait(&p->stall, s->ms);
        }
    }
    return 1;
}

// A heartbeat alone, as os_listall's per entry: its ctx an FsCall.
static void fsbeat(void *ctx)
{
    FsCall   *c = (FsCall *)ctx;
    FsWorker *w = c->w;
    FsPool   *p = w->pool;
    os_lock(&p->lock);
    b32 poke   = w->holding && p->nqueued>0;
    w->beat    = os_ticks(p->os);
    w->holding = 0;
    os_unlock(&p->lock);
    if (poke) {
        fspoke(p, w);
    }
}

// Whether the owner gave the job up, or quit, for a long job to stop.
static b32 fsgone(FsCall *c)
{
    return __atomic_load_n(&c->job->gone, __ATOMIC_ACQUIRE);
}


// Manual mode, for tests: no threads; fsbegin hands the next job that
// may go to a worker, its index returned (-1 for none), and fsrun runs
// it on the caller, posting as a worker does.

static i32 fsbegin(FsPool *p)
{
    i32 r = -1;
    os_lock(&p->lock);
    FsJob *j = 0;
    for (i32 i = 0; i < FS_JOBS; i++) {
        FsJob *q = &p->jobs[i];
        if (q->state==FSJ_QUEUED && !fsrefused(p, &q->put) && !fsheldback(p, &q->put) &&
                (!j || q->seq<j->seq)) {
            j = q;
        }
    }
    FsWorker *w = j ? fsidle(p, 0) : 0;
    if (j && !w && p->threads<FS_THREADS) {
        w = fsidle(p, 1);
        if (w) {
            w->started = 1;
            p->threads++;
        }
    }
    if (w) {
        fshandover(p, j, w);
        r = w->index;
    }
    os_unlock(&p->lock);
    return r;
}

static void fsrun(FsPool *p, i32 worker)
{
    fsdo(&p->workers[worker], 1);
}
