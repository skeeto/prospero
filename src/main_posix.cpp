// POSIX platform layer (macOS, Linux)
// $ cmake -B build && cmake --build build
//
// Threads are detached pthreads with 512 KiB stacks that start with every
// signal blocked, so handlers run only on the main thread. Events are
// non-blocking self-pipes, which makes os_signal async-signal-safe. Every
// descriptor is close-on-exec. A path or name holding a null byte names
// nothing, rather than being cut short at the system call.
//
// HTTP requests run through cpp-httplib in a separate translation unit
// (http_httplib.cpp) behind a small C interface, and tool processes run
// under reapers: this same executable started as "harness --reap".
//
// This is free and unencumbered software released into the public domain.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <string.h>  // strerror_r
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "harness.cpp"

#ifdef __APPLE__
extern "C" int _NSGetExecutablePath(char *, u32 *);
#endif

extern char **environ;

struct Os {
    i32   lifer;  // lifeline read end: every reaper's fd 3
    i32   lifew;  // its write end, held open until this process dies
    char *exe;    // this executable, for starting reapers
};

enum : iz {
    IOMAX        = (iz)1 << 30,  // per call: macOS fails past INT_MAX
    THREAD_STACK = (iz)1 << 19,
};

static iz pagesize()
{
    return (iz)sysconf(_SC_PAGESIZE);
}

// True if a string cannot cross the system call boundary intact.
static b32 hasnul(Str s)
{
    return cut(s, 0).ok;
}

static char *cstr(Arena *a, Str s)
{
    char *r = alloc<char>(a, s.len+1);
    copybytes(r, s.data, s.len);
    return r;
}

static b32 writeall(i32 fd, Str s)
{
    for (iz off = 0; off < s.len;) {
        iz len = s.len-off<IOMAX ? s.len-off : IOMAX;
        iz n   = write(fd, s.data+off, (uz)len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        off += n;
    }
    return 1;
}

[[noreturn]] static void os_oom()
{
    writeall(2, "harness: out of memory\n");
    _exit(2);
}

// Reservations are mapped inaccessible and made writable as committed.
static Region *os_reserve(Os *, iz cap)
{
    iz page = pagesize();
    cap += -cap & (page - 1);
    byte *p = (byte *)mmap(0, (uz)cap, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return 0;
    }
    if (mprotect(p, (uz)page, PROT_READ|PROT_WRITE)) {
        munmap(p, (uz)cap);
        return 0;
    }
    iz      hdr = (iz)sizeof(Region) + (-(iz)sizeof(Region) & 63);
    Region *r   = (Region *)p;
    r->base      = p + hdr;
    r->cap       = cap - hdr;
    r->committed = page - hdr;
    return r;
}

static b32 os_commit(Region *r, iz upto)
{
    if (upto <= r->committed) {
        return 1;
    }
    uz    page = (uz)pagesize();
    byte *lo   = (byte *)((uz)(r->base + r->committed) & -page);
    byte *hi   = r->base + upto;
    if (mprotect(lo, (uz)(hi - lo), PROT_READ|PROT_WRITE)) {
        return 0;
    }
    r->committed = upto;
    return 1;
}

static void os_release(Os *, Region *r)
{
    byte *p = (byte *)r;
    munmap(p, (uz)(r->base + r->cap - p));
}


// Time and entropy

static i64 monotonic()  // microseconds
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec*1000000 + ts.tv_nsec/1000;
}

static i64 os_ticks(Os *)
{
    return monotonic();
}

static i64 os_time(Os *)
{
    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static u64 os_random(Os *os)
{
    u64 r = 0;
    if (!getentropy(&r, sizeof(r))) {
        return r;
    }

    // Sandboxes may forbid getrandom(2) but still allow the device.
    i32 fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC);
    if (fd >= 0) {
        iz n = read(fd, &r, sizeof(r));
        close(fd);
        if (n == (iz)sizeof(r)) {
            return r;
        }
    }

    // Last resort: weak, but still differs between calls and processes.
    r = (u64)os_time(os) ^ (u64)os_ticks(os)<<20 ^ (u64)getpid()<<40;
    r ^= (u64)&r;
    r *= 1111111111111111111u;
    return r ^ r>>32;
}


// Files

static i64 mtimems(struct stat *st)
{
    #ifdef __APPLE__
    struct timespec t = st->st_mtimespec;
    #else
    struct timespec t = st->st_mtim;
    #endif
    return (i64)t.tv_sec*1000 + t.tv_nsec/1000000;
}

static Read os_read(Os *, Arena *a, Str path, iz max)
{
    Read  r   = {};
    Arena tmp = *a;
    if (hasnul(path)) {
        r.err = READ_NOTFOUND;
        return r;
    }

    // Non-blocking so that opening a FIFO does not wait for a writer.
    i32 fd = open(cstr(&tmp, path), O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd < 0) {
        r.err = errno==ENOENT || errno==ENOTDIR ? READ_NOTFOUND : READ_IO;
        return r;
    }

    struct stat st;
    if (fstat(fd, &st)) {
        close(fd);
        r.err = READ_IO;
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        r.err = READ_NOTFILE;
        return r;
    }
    r.size = (i64)st.st_size;
    if (r.size > max) {
        close(fd);
        r.err = READ_TOOBIG;
        return r;
    }

    Arena save = *a;
    Str   data = {allocbytes(a, (iz)r.size), 0};
    while (data.len < r.size) {
        iz len = (iz)r.size - data.len;
        len = len<IOMAX ? len : IOMAX;
        iz n = read(fd, data.data+data.len, (uz)len);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            *a = save;
            r.err = READ_IO;
            return r;
        } else if (!n) {
            break;  // shrank while reading
        }
        data.len += n;
    }
    close(fd);
    a->beg = (byte *)(data.data + data.len);
    r.data = data;
    r.size = data.len;
    return r;
}

// The temporary sibling has a random name so that concurrent writers of
// the same path never share one. It takes the permissions of the file
// it replaces, and reaches the disk before the rename makes it visible.
static b32 os_write(Os *os, Arena scratch, Str path, Str data)
{
    if (hasnul(path)) {
        return 0;
    }
    Buf b(&scratch, path.len+32);
    print(&b, path);
    putbyte(&b, '.');
    printhex(&b, os_random(os), 16);
    print(&b, ".tmp");
    char *tmp = cstr(&scratch, finish(&b));
    char *dst = cstr(&scratch, path);

    i32 fd = open(tmp, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0666);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    if (!stat(dst, &st)) {
        fchmod(fd, st.st_mode & 0777);
    }
    b32 ok = writeall(fd, data) && !fsync(fd);
    ok &= !close(fd);
    ok = ok && !rename(tmp, dst);
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

static b32 os_mkdirs(Os *, Arena scratch, Str path)
{
    if (!path.len || hasnul(path)) {
        return 0;
    }
    char *p = cstr(&scratch, path);
    for (iz i = 1; i <= path.len; i++) {
        if (i<path.len && p[i]!='/') {
            continue;
        }
        char save = p[i];
        p[i] = 0;
        if (mkdir(p, 0777)) {
            // Usually EEXIST, but an existing directory may also report
            // EISDIR (macOS "/"), EACCES, or EROFS.
            struct stat st;
            if (stat(p, &st) || !S_ISDIR(st.st_mode)) {
                return 0;
            }
        }
        p[i] = save;
    }
    return 1;
}

static Dirent *os_list(Os *, Arena *a, Str path)
{
    if (hasnul(path)) {
        return 0;
    }
    Arena tmp = *a;
    i32   fd  = open(cstr(&tmp, path), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return 0;
    }

    Dirent *head = 0;
    for (struct dirent *e; (e = readdir(dir));) {
        Str name = {(u8 *)e->d_name, 0};
        while (name.data[name.len]) name.len++;
        if (name == "." || name == "..") {
            continue;
        }
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            continue;
        }
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            continue;
        }
        Dirent *d = alloc<Dirent>(a);
        d->name  = clone(a, name);
        d->size  = (i64)st.st_size;
        d->mtime = mtimems(&st);
        d->isdir = S_ISDIR(st.st_mode);
        d->next  = head;
        head = d;
    }
    closedir(dir);  // also closes fd
    return head;
}

static Stat os_stat(Os *, Arena scratch, Str path)
{
    Stat r = {};
    struct stat st;
    if (hasnul(path) || stat(cstr(&scratch, path), &st)) {
        return r;
    }
    r.size  = (i64)st.st_size;
    r.mtime = mtimems(&st);
    r.type  = S_ISREG(st.st_mode) ? STAT_FILE :
              S_ISDIR(st.st_mode) ? STAT_DIR  :
              STAT_OTHER;
    return r;
}

static b32 os_rename(Os *, Arena scratch, Str from, Str to)
{
    if (hasnul(from) || hasnul(to)) {
        return 0;
    }
    return !rename(cstr(&scratch, from), cstr(&scratch, to));
}

// Also removes an empty directory, like remove(3).
static b32 os_remove(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return 0;
    }
    char *p = cstr(&scratch, path);
    if (!unlink(p)) {
        return 1;
    }
    // Directories fail with EISDIR on Linux, EPERM on macOS.
    return (errno==EISDIR || errno==EPERM) && !rmdir(p);
}

static b32 os_print(Os *, i32 fd, Str s)
{
    return writeall(fd, s);
}


// Process environment

static Str os_getenv(Os *, Arena *a, Str name)
{
    if (hasnul(name)) {
        return {};
    }
    Arena tmp = *a;
    char *v   = getenv(cstr(&tmp, name));
    if (!v) {
        return {};
    }
    Str r = {(u8 *)v, 0};
    while (r.data[r.len]) r.len++;
    return clone(a, r);  // non-null even when empty
}

static Str os_exepath(Os *, Arena *a)
{
    #if defined(__APPLE__)
    // The recorded path may be relative or contain links: resolve it,
    // into storage allocated before the scratch it is resolved from.
    iz    cap = 1024;  // PATH_MAX
    char *out = (char *)allocbytes(a, cap);
    Arena tmp = *a;
    u32   len = 256;
    char *raw = (char *)allocbytes(&tmp, len);
    if (_NSGetExecutablePath(raw, &len)) {
        raw = (char *)allocbytes(&tmp, len);
        if (_NSGetExecutablePath(raw, &len)) {
            a->beg = (byte *)out;
            return {};
        }
    }
    if (!realpath(raw, out)) {
        a->beg = (byte *)out;
        return {};
    }
    Str r = {(u8 *)out, 0};
    while (r.data[r.len]) r.len++;
    a->beg = (byte *)(r.data + r.len);
    return r;

    #elif defined(__linux__)
    // Once the executable is replaced or deleted, as by a rebuild, the
    // link reads "PATH (deleted)". Report PATH, which then names the
    // replacement if any. To run this same binary again, as a reaper
    // must, exec /proc/self/exe instead.
    Str deleted = " (deleted)";
    for (iz cap = 256;; cap *= 4) {
        u8 *buf = allocbytes(a, cap);
        iz  len = readlink("/proc/self/exe", (char *)buf, (uz)cap);
        if (len < 0) {
            a->beg = (byte *)buf;
            return {};
        } else if (len < cap) {
            Str r = {buf, len};
            buf[len] = 0;
            struct stat st;
            if (endswith(r, deleted) && stat((char *)buf, &st)) {
                r.len -= deleted.len;  // unless truly named so
            }
            a->beg = (byte *)(r.data + r.len);
            return r;
        }
        a->beg = (byte *)buf;  // truncated: retry larger
    }

    #else
    return {};
    #endif
}


// Threads, locks, and events

static_assert(sizeof(pthread_mutex_t) <= sizeof(Lock));
static_assert(alignof(pthread_mutex_t) <= alignof(Lock));

struct ThreadStart {
    void (*fn)(void *);
    void  *arg;
};

static void *threadentry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

// The start record is the one heap allocation in this layer: it must
// outlive this call, and the new thread frees it.
static b32 os_thread(Os *, void (*fn)(void *), void *arg)
{
    ThreadStart *s = (ThreadStart *)malloc(sizeof(ThreadStart));
    if (!s) {
        return 0;
    }
    s->fn  = fn;
    s->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, (uz)THREAD_STACK);

    // The new thread inherits this mask: signals belong to the main
    // thread, and a blocked SIGPIPE becomes a plain EPIPE. Faults stay
    // unblocked: on macOS a blocked SIGTRAP from a failed assertion
    // spins on the trap instead of stopping the process.
    sigset_t all, old;
    sigfillset(&all);
    sigdelset(&all, SIGTRAP);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGFPE);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    pthread_t thr;
    b32 ok = !pthread_create(&thr, &attr, threadentry, s);
    pthread_sigmask(SIG_SETMASK, &old, 0);
    pthread_attr_destroy(&attr);

    if (!ok) {
        free(s);
    }
    return ok;
}

static void os_lockinit(Lock *l)
{
    pthread_mutex_init((pthread_mutex_t *)l->opaque, 0);
}

static void os_lock(Lock *l)
{
    pthread_mutex_lock((pthread_mutex_t *)l->opaque);
}

static void os_unlock(Lock *l)
{
    pthread_mutex_unlock((pthread_mutex_t *)l->opaque);
}

// A self-pipe: h[0] is the read end, h[1] the write end.
static b32 os_eventinit(Event *e)
{
    i32 fds[2];
    #ifdef __linux__
    if (pipe2(fds, O_NONBLOCK|O_CLOEXEC)) {
        return 0;
    }
    #else
    if (pipe(fds)) {
        return 0;
    }
    for (i32 i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(fds[i], F_SETFL, O_NONBLOCK);
    }
    #endif
    e->h[0] = fds[0];
    e->h[1] = fds[1];
    return 1;
}

// A full pipe (EAGAIN) is already signaled. Preserves errno, so it is
// safe to call from a signal handler.
static void os_signal(Event *e)
{
    i32 save = errno;
    u8  b    = 0;
    while (write((i32)e->h[1], &b, 1)<0 && errno==EINTR) {}
    errno = save;
}

// Every waiter polls readable after a signal, then they race to drain
// the pipe. The losers find it empty and keep waiting, so that one
// signal wakes one waiter, as with a Win32 auto-reset event.
static b32 os_wait(Event *e, i32 ms)
{
    i32 fd       = (i32)e->h[0];
    i64 deadline = ms<0 ? 0 : monotonic() + (i64)ms*1000;
    for (;;) {
        i32 timeout = -1;
        if (ms >= 0) {
            i64 left = deadline - monotonic();
            timeout = left>0 ? (i32)((left + 999)/1000) : 0;
        }
        struct pollfd p = {};
        p.fd     = fd;
        p.events = POLLIN;
        i32 r = poll(&p, 1, timeout);
        if (r<0 && errno==EINTR) {
            continue;
        } else if (r <= 0) {
            return 0;
        }

        // Consume every signal at once: events coalesce.
        b32 got = 0;
        for (;;) {
            u8 buf[64];
            iz n = read(fd, buf, sizeof(buf));
            if (n<0 && errno==EINTR) {
                continue;
            } else if (n == (iz)sizeof(buf)) {
                got = 1;
                continue;
            } else if (n>0 || got) {
                return 1;
            } else if (n<0 && errno==EAGAIN) {
                break;  // another waiter consumed it
            }
            return 0;
        }
    }
}


// Append-only logs: handles are file descriptors

static iz os_logopen(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return -1;
    }
    i32 flags = O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC;
    return open(cstr(&scratch, path), flags, 0644);
}

static b32 os_logwrite(Os *, iz log, Str record)
{
    return writeall((i32)log, record);
}

// Plain fsync: on macOS it reaches the drive but not necessarily its
// platters, which F_FULLFSYNC would wait for at many times the cost.
static b32 os_logsync(Os *, iz log)
{
    return !fsync((i32)log);
}

static b32 os_logtrunc(Os *, iz log, i64 len)
{
    return !ftruncate((i32)log, (off_t)len);
}

static void os_logclose(Os *, iz log)
{
    close((i32)log);
}


// More files and the environment

static Read os_readat(Os *, Arena *a, Str path, i64 off, iz max)
{
    Read  r   = {};
    Arena tmp = *a;
    if (hasnul(path) || off<0 || max<0) {
        r.err = READ_NOTFOUND;
        return r;
    }
    i32 fd = open(cstr(&tmp, path), O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd < 0) {
        r.err = errno==ENOENT || errno==ENOTDIR ? READ_NOTFOUND : READ_IO;
        return r;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        r.err = READ_NOTFILE;
        return r;
    }
    r.size = (i64)st.st_size;
    i64 avail = r.size>off ? r.size-off : 0;
    iz  want  = avail<max ? (iz)avail : max;

    Str data = {allocbytes(a, want), 0};
    while (data.len < want) {
        iz len = want - data.len;
        len = len<IOMAX ? len : IOMAX;
        iz n = pread(fd, data.data+data.len, (uz)len, (off_t)(off+data.len));
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n < 0) {
            close(fd);
            r.err = READ_IO;
            return r;
        } else if (!n) {
            break;
        }
        data.len += n;
    }
    close(fd);
    a->beg = (byte *)(data.data + data.len);
    r.data = data;
    return r;
}

static Slice<Str> os_environ(Os *, Arena *a)
{
    Slice<Str> r = {};
    for (char **e = environ; *e; e++) {
        Str s = {(u8 *)*e, 0};
        while (s.data[s.len]) s.len++;
        r = push(a, r, clone(a, s));
    }
    return r;
}

static iz os_stdin(Os *, u8 *buf, iz len)
{
    for (;;) {
        iz n = read(0, buf, (uz)(len<IOMAX ? len : IOMAX));
        if (n<0 && errno==EINTR) continue;
        return n<0 ? -1 : n;
    }
}

static Str os_realpath(Os *, Arena *a, Str path)
{
    if (hasnul(path)) {
        return {};
    }
    Arena tmp = *a;
    char *out = (char *)allocbytes(&tmp, 4096);  // PATH_MAX
    if (!realpath(cstr(&tmp, path), out)) {
        return {};
    }
    Str r = {(u8 *)out, 0};
    while (r.data[r.len]) r.len++;
    return clone(a, r);
}

// strerror_r returns int (XSI) or char * (GNU), depending on the libc.
static char *errtext(i32, char *buf, i32 r)
{
    return r ? (char *)"unknown error" : buf;
}

static char *errtext(i32, char *, char *r)
{
    return r;
}

static Str os_lasterr(Os *, Arena *a)
{
    i32  err = errno;
    char buf[256] = {};
    char *msg = errtext(err, buf, strerror_r(err, buf, sizeof(buf)));
    Str  r    = {(u8 *)msg, 0};
    while (r.data[r.len]) r.len++;
    return clone(a, r);
}


// Processes
//
// A tool runs under a reaper, this executable started as
//   harness --reap CWD ARGV...
// with null stdin, the output pipe on fds 1 and 2, the lifeline on fd 3,
// and a status pipe on fd 4. The reaper leads a new session and process
// group and starts the tool in it, then reports the tool's exit status
// on fd 4 and stays alive, so that the group id, its own pid, cannot be
// reused until the harness kills the whole group (os_kill) and reaps
// it. Every reaper shares the lifeline, whose write end only the harness
// holds: when the harness dies, by any means, the lifeline reads EOF and
// each reaper kills its group, TERM and then KILL 2 s later. A process
// that leaves the group (setsid, setpgid) escapes all of this.
//
// Each Proc has a reader thread, pumping output into the file, and a
// waiter thread, reading the status and then reaping the reaper. Both
// hold references, as does the engine until os_procfree.

enum : iz {
    PROC_TAILMAX = 1<<12,
    PROC_BUFLEN  = 1<<16,
};

struct Proc {
    Lock     lock;
    i32      refs;      // atomic
    i32      pid;       // the reaper, leader of the process group
    b32      reaped;    // under lock: pid may be reused
    Mailbox *mbox;
    u64      id;
    i32      outfd;     // output pipe read end, owned by the reader
    i32      statfd;    // status pipe read end, owned by the waiter
    i32      filefd;    // the output file, owned by the reader
    i64      maxbytes;
    i64      total;     // reader only
    iz       tailpos;   // reader only: next write in the tail ring
    u8       tail[PROC_TAILMAX];
    u8       msg[8+PROC_TAILMAX];
    u8       buf[PROC_BUFLEN];
};

static void procrelease(Proc *p)
{
    if (!__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) {
        pthread_mutex_destroy((pthread_mutex_t *)p->lock.opaque);
        free(p);
    }
}

static void put64(u8 *dst, i64 v)
{
    __builtin_memcpy(dst, &v, 8);
}

static void put32(u8 *dst, i32 v)
{
    __builtin_memcpy(dst, &v, 4);
}

static void procreader(void *arg)
{
    Proc *p    = (Proc *)arg;
    i64   last = 0;
    b32   kept = 1;  // still writing to the file
    for (;;) {
        iz n = read(p->outfd, p->buf, PROC_BUFLEN);
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            break;
        }

        if (kept) {
            iz keep = n;
            if (p->total+n > p->maxbytes) {
                keep = (iz)(p->maxbytes - p->total);
                kept = 0;
            }
            writeall(p->filefd, {p->buf, keep});  // a full disk loses output
            if (!kept) {
                u8 msg[8];
                put64(msg, p->total + keep);
                mailpost(p->mbox, PROC_LIMIT, p->id, {msg, 8}, 1);
            }
        }
        p->total += n;

        for (iz i = n<PROC_TAILMAX ? 0 : n-PROC_TAILMAX; i < n; i++) {
            p->tail[p->tailpos++ & (PROC_TAILMAX-1)] = p->buf[i];
        }
        i64 now = monotonic();
        if (now-last >= 50000) {
            last = now;
            iz len = p->total<PROC_TAILMAX ? (iz)p->total : PROC_TAILMAX;
            put64(p->msg, p->total);
            for (iz i = 0; i < len; i++) {
                p->msg[8+i] = p->tail[(p->tailpos - len + i) & (PROC_TAILMAX-1)];
            }
            mailpost(p->mbox, PROC_TAIL, p->id, {p->msg, 8+len}, 0);
        }
    }
    close(p->outfd);
    close(p->filefd);
    u8 msg[8];
    put64(msg, p->total);
    mailpost(p->mbox, PROC_EOF, p->id, {msg, 8}, 1);
    procrelease(p);
}

static void procwaiter(void *arg)
{
    Proc *p   = (Proc *)arg;
    u8    st[8];
    iz    got = 0;
    while (got < 8) {
        iz n = read(p->statfd, st+got, (uz)(8-got));
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            break;
        }
        got += n;
    }
    if (got == 8) {
        mailpost(p->mbox, PROC_EXIT, p->id, {st, 8}, 1);
    }
    close(p->statfd);

    // The reaper lives until the group is killed
    i32 status = 0;
    while (waitpid(p->pid, &status, 0)<0 && errno==EINTR) {}
    os_lock(&p->lock);
    p->reaped = 1;
    os_unlock(&p->lock);

    if (got != 8) {
        // The reaper died first, so the tool was killed with it
        i32 sig = WIFSIGNALED(status) ? WTERMSIG(status) : SIGKILL;
        put32(st, -1);
        put32(st+4, sig);
        mailpost(p->mbox, PROC_EXIT, p->id, {st, 8}, 1);
    }
    procrelease(p);
}

// Move a descriptor to 10 or above, close-on-exec, so that installing it
// at 0-4 in a child is never a no-op dup2 that leaves it close-on-exec.
static i32 highfd(i32 fd)
{
    if (fd < 0) {
        return -1;
    }
    i32 r = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    close(fd);
    return r;
}

static b32 cloexecpipe(i32 fds[2])
{
    #ifdef __linux__
    return !pipe2(fds, O_CLOEXEC);
    #else
    if (pipe(fds)) {
        return 0;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 1;
    #endif
}

static void closeall(i32 *fds, iz n)
{
    for (iz i = 0; i < n; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
}

// Signal state for a child: defaults restored and nothing blocked, since
// spawning threads block every signal and the harness ignores SIGPIPE.
static void childsignals(posix_spawnattr_t *attr, short flags)
{
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    sigdelset(&all, SIGKILL);
    sigdelset(&all, SIGSTOP);
    posix_spawnattr_init(attr);
    posix_spawnattr_setsigmask(attr, &none);
    posix_spawnattr_setsigdefault(attr, &all);
    posix_spawnattr_setflags(attr, (short)(flags|POSIX_SPAWN_SETSIGMASK|
                                         POSIX_SPAWN_SETSIGDEF));
}

static Proc *os_spawn(Os *os, Spawn *sp, Arena scratch)
{
    if (!sp->argv.len || hasnul(sp->cwd) || hasnul(sp->outpath)) {
        errno = EINVAL;
        return 0;
    }

    // Child-side descriptors: null, output write end, status write end
    i32 out[2] = {-1, -1};
    i32 st[2]  = {-1, -1};
    i32 null   = open("/dev/null", O_RDWR|O_CLOEXEC);
    i32 file   = open(cstr(&scratch, sp->outpath),
                      O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
    if (null<0 || file<0 || !cloexecpipe(out) || !cloexecpipe(st)) {
        i32 save = errno;
        i32 fds[] = {null, file, out[0], out[1], st[0], st[1]};
        closeall(fds, countof(fds));
        errno = save;
        return 0;
    }
    null   = highfd(null);
    out[1] = highfd(out[1]);
    st[1]  = highfd(st[1]);

    iz     argc = 3 + sp->argv.len;
    char **argv = alloc<char *>(&scratch, argc+1);
    argv[0] = os->exe;
    argv[1] = (char *)"--reap";
    argv[2] = cstr(&scratch, sp->cwd);
    for (iz i = 0; i < sp->argv.len; i++) {
        argv[3+i] = cstr(&scratch, sp->argv[i]);
    }
    char **envp = alloc<char *>(&scratch, sp->env.len+1);
    for (iz i = 0; i < sp->env.len; i++) {
        envp[i] = cstr(&scratch, sp->env[i]);
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, null, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_adddup2(&fa, os->lifer, 3);
    posix_spawn_file_actions_adddup2(&fa, st[1], 4);
    posix_spawnattr_t attr;
    short flags = 0;
    #ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    #endif
    childsignals(&attr, flags);
    pid_t pid = 0;
    i32   err = null<0 || out[1]<0 || st[1]<0 ? EMFILE :
                posix_spawn(&pid, os->exe, &fa, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    i32 child[] = {null, out[1], st[1]};
    closeall(child, countof(child));
    if (err) {
        i32 fds[] = {file, out[0], st[0]};
        closeall(fds, countof(fds));
        errno = err;
        return 0;
    }

    Proc *p = (Proc *)malloc(sizeof(Proc));
    if (!p) {
        kill(-pid, SIGKILL);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        i32 fds[] = {file, out[0], st[0]};
        closeall(fds, countof(fds));
        errno = ENOMEM;
        return 0;
    }
    __builtin_memset(p, 0, offsetof(Proc, tail));
    os_lockinit(&p->lock);
    p->refs     = 3;
    p->pid      = pid;
    p->mbox     = sp->mbox;
    p->id       = sp->id;
    p->outfd    = out[0];
    p->statfd   = st[0];
    p->filefd   = file;
    p->maxbytes = sp->maxbytes;

    if (!os_thread(os, procreader, p)) {
        kill(-pid, SIGKILL);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        i32 fds[] = {file, out[0], st[0]};
        closeall(fds, countof(fds));
        pthread_mutex_destroy((pthread_mutex_t *)p->lock.opaque);
        free(p);
        errno = EAGAIN;
        return 0;
    }
    if (!os_thread(os, procwaiter, p)) {
        // The reader finishes once the group is dead
        kill(-pid, SIGKILL);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        close(st[0]);
        procrelease(p);  // the waiter's
        procrelease(p);  // the caller's
        errno = EAGAIN;
        return 0;
    }
    return p;
}

static void os_kill(Os *, Proc *p, b32 hard)
{
    os_lock(&p->lock);
    if (!p->reaped) {
        kill(-p->pid, hard ? SIGKILL : SIGTERM);
    }
    os_unlock(&p->lock);
}

static void os_procfree(Os *, Proc *p)
{
    procrelease(p);
}

// Reaper only: the SIGCHLD self-pipe's write end.
static i32 reapchld = -1;

static void onchld(int)
{
    i32 save = errno;
    u8  b    = 0;
    while (write(reapchld, &b, 1)<0 && errno==EINTR) {}
    errno = save;
}

static void reapsay(char const *what, char const *arg)
{
    Str parts[] = {"harness: ", {}, {}, ": ", {}, "\n"};
    parts[1] = {(u8 *)what, (iz)__builtin_strlen(what)};
    parts[2] = {(u8 *)arg, (iz)__builtin_strlen(arg)};
    char const *err = strerror(errno);  // single-threaded here
    parts[4] = {(u8 *)err, (iz)__builtin_strlen(err)};
    for (iz i = 0; i < countof(parts); i++) {
        writeall(2, parts[i]);
    }
}

static void reapstatus(i32 code, i32 sig)
{
    u8 st[8];
    put32(st, code);
    put32(st+4, sig);
    writeall(4, {st, 8});
    close(4);
}

// The reaper process: see "Processes" above. Never returns.
[[noreturn]] static void reap(i32 argc, char **argv)
{
    setsid();
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    fcntl(3, F_SETFD, FD_CLOEXEC);
    fcntl(4, F_SETFD, FD_CLOEXEC);
    #if defined(__linux__) && defined(SYS_close_range)
    syscall(SYS_close_range, 5u, ~0u, 4u);  // CLOSE_RANGE_CLOEXEC
    #endif

    if (argc < 4) {
        errno = EINVAL;
        reapsay("usage", "--reap CWD ARGV...");
        reapstatus(127, 0);
        _exit(2);
    }

    i32 chld[2];
    if (!cloexecpipe(chld)) {
        reapsay("cannot create a pipe", "");
        reapstatus(126, 0);
        _exit(2);
    }
    fcntl(chld[0], F_SETFL, O_NONBLOCK);
    fcntl(chld[1], F_SETFL, O_NONBLOCK);
    reapchld = chld[1];
    struct sigaction sa = {};
    sa.sa_handler = onchld;
    sa.sa_flags   = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, 0);

    if (chdir(argv[2])) {
        reapsay("cannot change directory to", argv[2]);
        reapstatus(126, 0);
        _exit(2);
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_t attr;
    short flags = 0;
    #ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    for (i32 fd = 0; fd < 3; fd++) {
        posix_spawn_file_actions_addinherit_np(&fa, fd);
    }
    #endif
    childsignals(&attr, flags);
    pid_t pid = 0;
    i32   err = posix_spawnp(&pid, argv[3], &fa, &attr, argv+3, environ);
    if (err) {
        errno = err;
        reapsay("cannot run", argv[3]);
        reapstatus(err==ENOENT ? 127 : 126, 0);
        _exit(2);
    }

    // Only the tool's tree holds the output now
    i32 null = open("/dev/null", O_RDWR|O_CLOEXEC);
    dup2(null, 1);
    dup2(null, 2);

    b32 exited = 0;
    for (;;) {
        struct pollfd fds[2] = {};
        fds[0].fd     = 3;
        fds[0].events = POLLIN;
        fds[1].fd     = exited ? -1 : chld[0];
        fds[1].events = POLLIN;
        if (poll(fds, 2, -1) < 0) {
            continue;  // EINTR from SIGCHLD
        }
        if (fds[0].revents) {
            u8 b;
            iz n = read(3, &b, 1);
            if (n==0 || (n<0 && errno!=EINTR && errno!=EAGAIN)) {
                break;  // the harness is gone
            }
        }
        if (!exited) {
            u8 drain[64];
            while (read(chld[0], drain, sizeof(drain)) > 0) {}
            i32 status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) {
                exited = 1;
                if (WIFSIGNALED(status)) {
                    reapstatus(-1, WTERMSIG(status));
                } else {
                    reapstatus(WEXITSTATUS(status), 0);
                }
            }
        }
    }

    kill(0, SIGTERM);
    struct timespec ts = {2, 0};
    while (nanosleep(&ts, &ts)) {}
    kill(0, SIGKILL);
    _exit(1);
}


// HTTP: requests run through http_httplib.cpp

extern "C" {
struct HttpEdgeReq {
    char const *origin;
    char const *path;
    char const *method;
    char const *headers;
    iz          headerslen;
    char const *body;
    iz          bodylen;
    i32         connectms;
    i32         readms;
    void       *ctx;
    i32       (*head)(void *ctx, i32 status, char const *hdrs, iz len);
    i32       (*data)(void *ctx, char const *buf, iz len);
    char       *err;
    iz          errcap;
};
void *httpedge_new();
void  httpedge_free(void *);
void  httpedge_cancel(void *);
i32   httpedge_run(void *, HttpEdgeReq *);
}

enum {
    EDGE_OK,
    EDGE_CANCEL,
    EDGE_CONNECT,
    EDGE_IO,
    EDGE_ABORT,
};

struct Http {
    i32      refs;      // atomic: the engine and the worker
    void    *edge;
    Mailbox *mbox;
    u64      id;
    i64      maxbytes;
    i64      got;       // worker only
    b32      toobig;    // worker only
    char    *origin;
    char    *path;
    char    *method;
    Str      headers;
    Str      body;
    i32      connectms;
    i32      readms;
    char     err[256];
};

static void httprelease(Http *h)
{
    if (!__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL)) {
        httpedge_free(h->edge);
        free(h);
    }
}

static i32 httphead(void *ctx, i32 status, char const *hdrs, iz len)
{
    Http *h   = (Http *)ctx;
    iz    max = mailmax(h->mbox) - 4;
    len = len<max ? len : max;
    u8 *msg = (u8 *)malloc((uz)(4 + len));
    if (!msg) {
        return 0;
    }
    put32(msg, status);
    copybytes(msg+4, (void *)hdrs, len);
    mailpost(h->mbox, HTTP_HEAD, h->id, {msg, 4+len}, 1);
    free(msg);
    return 1;
}

static i32 httpdata(void *ctx, char const *buf, iz len)
{
    Http *h = (Http *)ctx;
    if (len > h->maxbytes - h->got) {
        h->toobig = 1;
        return 0;
    }
    h->got += len;
    iz max = mailmax(h->mbox);
    for (iz off = 0; off < len; off += max) {
        iz n = len-off<max ? len-off : max;
        mailpost(h->mbox, HTTP_BODY, h->id, {(u8 *)buf+off, n}, 1);
    }
    return 1;
}

static void httpworker(void *arg)
{
    Http       *h = (Http *)arg;
    HttpEdgeReq r = {};
    r.origin     = h->origin;
    r.path       = h->path;
    r.method     = h->method;
    r.headers    = (char *)h->headers.data;
    r.headerslen = h->headers.len;
    r.body       = (char *)h->body.data;
    r.bodylen    = h->body.len;
    r.connectms  = h->connectms;
    r.readms     = h->readms;
    r.ctx        = h;
    r.head       = httphead;
    r.data       = httpdata;
    r.err        = h->err;
    r.errcap     = countof(h->err);

    i32 err = HTTP_EIO;
    switch (httpedge_run(h->edge, &r)) {
    case EDGE_OK:      err = HTTP_OK;       break;
    case EDGE_CANCEL:  err = HTTP_ECANCEL;  break;
    case EDGE_CONNECT: err = HTTP_ECONNECT; break;
    case EDGE_IO:      err = HTTP_EIO;      break;
    case EDGE_ABORT:   err = h->toobig ? HTTP_ETOOBIG : HTTP_EIO; break;
    }
    if (err == HTTP_ETOOBIG) {
        __builtin_strcpy(h->err, "response too large");
    }
    u8 msg[4+countof(h->err)];
    put32(msg, err);
    iz len = (iz)__builtin_strlen(h->err);
    copybytes(msg+4, h->err, len);
    mailpost(h->mbox, HTTP_DONE, h->id, {msg, 4+len}, 1);
    httprelease(h);
}

static Http *os_http(Os *os, HttpReq *req, Arena scratch)
{
    Url u = parseurl(req->url);
    if (!u.ok || (u.scheme!="http" && u.scheme!="https") ||
            hasnul(req->method) || hasnul(u.path)) {
        errno = EINVAL;
        return 0;
    }

    // One block: the record, then the null-terminated strings
    Str origin = urlorigin(&scratch, u);
    iz  size   = (iz)sizeof(Http) + origin.len + u.path.len +
              req->method.len + req->headers.len + req->body.len + 3;
    Http *h = (Http *)malloc((uz)size);
    if (!h) {
        errno = ENOMEM;
        return 0;
    }
    __builtin_memset(h, 0, sizeof(Http));
    u8 *p = (u8 *)(h + 1);
    h->origin = (char *)p;
    copybytes(p, origin.data, origin.len);
    p += origin.len;
    *p++ = 0;
    h->path = (char *)p;
    copybytes(p, u.path.data, u.path.len);
    p += u.path.len;
    *p++ = 0;
    h->method = (char *)p;
    copybytes(p, req->method.data, req->method.len);
    p += req->method.len;
    *p++ = 0;
    h->headers = {p, req->headers.len};
    copybytes(p, req->headers.data, req->headers.len);
    p += req->headers.len;
    h->body = {p, req->body.len};
    copybytes(p, req->body.data, req->body.len);

    h->refs      = 2;
    h->edge      = httpedge_new();
    h->mbox      = req->mbox;
    h->id        = req->id;
    h->maxbytes  = req->maxbytes;
    h->connectms = req->connectms;
    h->readms    = req->readms;
    if (!h->edge || !os_thread(os, httpworker, h)) {
        if (h->edge) httpedge_free(h->edge);
        free(h);
        errno = EAGAIN;
        return 0;
    }
    return h;
}

static void os_httpcancel(Os *, Http *h)
{
    httpedge_cancel(h->edge);
}

static void os_httpfree(Os *, Http *h)
{
    httprelease(h);
}


int main(int argc, char **argv)
{
    if (argc>1 && !__builtin_strcmp(argv[1], "--reap")) {
        reap(argc, argv);
    }

    // Standard descriptors must exist, or new ones would take their place
    for (i32 fd = open("/dev/null", O_RDWR); fd>=0 && fd<=2;) {
        fd = dup(fd);
    }
    signal(SIGPIPE, SIG_IGN);

    Os os = {};
    i32 life[2];
    if (!cloexecpipe(life)) {
        writeall(2, "harness: cannot create the lifeline pipe\n");
        return 2;
    }
    os.lifer = life[0];
    os.lifew = life[1];

    iz      cap = sizeof(void *)==8 ? (iz)1<<34 : (iz)1<<28;
    Region *r   = os_reserve(&os, cap);
    if (!r) {
        writeall(2, "harness: out of memory\n");
        return 2;
    }
    Arena a = newarena(r);

    #ifdef __linux__
    os.exe = (char *)"/proc/self/exe";  // this binary, even if replaced
    #else
    Str exe = os_exepath(&os, &a);
    os.exe = exe.len ? cstr(&a, exe) : argv[0];
    #endif

    Str *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i].data = (u8 *)argv[i];
        while (args[i].data[args[i].len]) args[i].len++;
    }
    return app_main(&os, a, args, argc);
}
