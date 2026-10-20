// POSIX platform layer (macOS, Linux)
// $ c++ -std=c++20 -pthread -o harness src/main_posix.cpp
//
// Threads are detached pthreads with 512 KiB stacks that start with every
// signal blocked, so handlers run only on the main thread. Events are
// non-blocking self-pipes, which makes os_signal async-signal-safe. Every
// descriptor is close-on-exec. A path or name holding a null byte names
// nothing, rather than being cut short at the system call.
//
// This is free and unencumbered software released into the public domain.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if HARNESS_GUI
#  include <SDL3/SDL.h>
#  include "imgui.h"
#  include "imgui_internal.h"
#  include "imgui_impl_sdl3.h"
#  include "imgui_impl_opengl3.h"
#  include "misc/freetype/imgui_freetype.h"
#  include "md4c.h"
#endif
#include "harness.cpp"

#ifdef __APPLE__
extern "C" int _NSGetExecutablePath(char *, u32 *);
#endif

struct Os {
    void *gui;  // GUI builds: the window's platform state (gui_sdl.cpp)
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
    // thread, and a blocked SIGPIPE becomes a plain EPIPE.
    sigset_t all, old;
    sigfillset(&all);
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


#if HARNESS_GUI
#  include "app.cpp"
#  include "gui_sdl.cpp"
#endif


int main(int argc, char **argv)
{
    Os      os  = {};
    iz      cap = sizeof(void *)==8 ? (iz)1<<34 : (iz)1<<28;
    Region *r   = os_reserve(&os, cap);
    if (!r) {
        writeall(2, "harness: out of memory\n");
        return 2;
    }
    Arena a = newarena(r);

    Str *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i].data = (u8 *)argv[i];
        while (args[i].data[args[i].len]) args[i].len++;
    }
    #if HARNESS_GUI
    if (guiwanted(args, argc)) {
        return gui_main(&os, a, args, argc);
    }
    #endif
    return app_main(&os, a, args, argc);
}
