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
// under reapers: this same executable started as "prospero --reap".
//
// This is free and unencumbered software released into the public domain.
#include <dirent.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <string.h>  // strerror_r
#include <sys/file.h>  // flock
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <dlfcn.h>       // dlsym, for the responsibility functions
#  include <pwd.h>         // getpwuid, for loginenviron
#  include <libproc.h>     // proc_listpids, proc_pidinfo
#  include <sys/event.h>   // EVFILT_PROC
#  include <sys/sysctl.h>  // KERN_PROC_UID
#endif
#ifdef __linux__
#  include <sys/prctl.h>   // PR_SET_CHILD_SUBREAPER
#endif
#if PROSPERO_GUI
#  include <SDL3/SDL.h>
#  include "imgui.h"
#  include "imgui_internal.h"
#  include "imgui_impl_sdl3.h"
#  include "imgui_impl_opengl3.h"
#  include "misc/freetype/imgui_freetype.h"
#  include "md4c.h"
#endif
#include "prospero.cpp"
#include "reap.cpp"

#ifdef __APPLE__
extern "C" int _NSGetExecutablePath(char *, u32 *);
#endif

extern char **environ;

// macOS: responsibility (reap.cpp), private but long stable, in
// libquarantine, which every process loads with libSystem: LLDB and
// Chromium use it the same way. Looked up at run time, and any missing
// means none of it. Null elsewhere.
struct Resp {
    i32 (*pidfor)(i32);                         // its responsible's pid
    u64 (*uniqfor)(i32);                        // its responsible's unique id
    i32 (*disclaim)(posix_spawnattr_t *, i32);  // spawn as its own
};

static Resp respspi()
{
    Resp r = {};
    #ifdef __APPLE__
    void *pidfor   = dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
    void *uniqfor  = dlsym(RTLD_DEFAULT, "responsibility_get_uniqueid_responsible_for_pid");
    void *disclaim = dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim");
    if (pidfor && uniqfor && disclaim) {
        r.pidfor   = (i32 (*)(i32))pidfor;
        r.uniqfor  = (u64 (*)(i32))uniqfor;
        r.disclaim = (i32 (*)(posix_spawnattr_t *, i32))disclaim;
    }
    #endif
    return r;
}

// What tells one file from another put in its place, or from itself
// rewritten since: for the executable that reapers run.
struct FileId {
    i64 dev;
    i64 ino;
    i64 size;
    i64 mtime;  // ns
    i64 ctime;  // ns
};

struct Os {
    i32    lifer;     // lifeline read end: every reaper's fd 3
    i32    lifew;     // its write end, held open until this process dies
    char  *exe;       // this executable, for starting reapers
    void  *gui;       // GUI builds: the window's platform state (gui_sdl.cpp)
    Resp   resp;      // macOS: responsibility, if there
    i32    respself;  // macOS: the pid responsible for this process, or 0
    FileId exeid;     // macOS: exe as it was at startup, zero if unknown
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
    writeall(2, "prospero: out of memory\n");
    _exit(2);
}

#if PROSPERO_ARENACHECK
static void os_arenafault(Str msg)
{
    writeall(2, "prospero: ");
    writeall(2, msg);
}
#endif

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
#if PROSPERO_ARENACHECK
    arenaforget(p, r->base+r->cap);
#endif
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
// it replaces, or mode if given, and reaches the disk before the rename
// makes it visible.
static b32 writeas(Os *os, Arena scratch, Str path, Str data, i32 mode)
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

    i32 fd = open(tmp, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, mode ? mode : 0666);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    if (mode) {
        fchmod(fd, (mode_t)mode);
    } else if (!stat(dst, &st)) {
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

static b32 os_write(Os *os, Arena scratch, Str path, Str data)
{
    return writeas(os, scratch, path, data, 0);
}

static b32 os_writemode(Os *os, Arena scratch, Str path, Str data, i32 mode)
{
    return writeas(os, scratch, path, data, mode);
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

static Listing os_listall(Os *, Arena *a, Str path, b32 dirs, iz max)
{
    Listing r = {};
    if (hasnul(path)) {
        return r;
    }
    Arena tmp = *a;
    i32   fd  = open(cstr(&tmp, path), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) {
        return r;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return r;
    }
    r.ok = 1;
    for (struct dirent *e; (e = readdir(dir));) {
        Str name = {(u8 *)e->d_name, 0};
        while (name.data[name.len]) name.len++;
        if (name == "." || name == ".." || (dirs && e->d_type==DT_REG)) {
            continue;
        }
        struct stat st;
        if (fstatat(fd, e->d_name, &st, 0)) {
            continue;  // a broken link
        }
        if (!S_ISDIR(st.st_mode) && (dirs || !S_ISREG(st.st_mode))) {
            continue;
        }
        if (r.len == max) {
            r.more = 1;
            break;
        }
        Dirent *d = alloc<Dirent>(a);
        d->name  = clone(a, name);
        d->size  = (i64)st.st_size;
        d->mtime = mtimems(&st);
        d->isdir = S_ISDIR(st.st_mode);
        d->next  = r.head;
        r.head = d;
        r.len++;
    }
    closedir(dir);  // also closes fd
    return r;
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

// A directory being removed (os_removetree): its descriptor, its name in
// its parent's, and the names in it, read at once so that only the one
// descriptor stays open while its subdirectories are.
struct RmDir {
    i32    fd;
    char  *name;
    char **names;
    iz     len;
    iz     next;
};

// Open a directory in dir, never through a link, made this user's to
// change (rwx) if it was not, so that its entries can go. -1 on failure.
static i32 rmopen(i32 dir, char *name)
{
    i32 flags = O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC;
    i32 fd    = openat(dir, name, flags);
    // Unreadable: AT_SYMLINK_NOFOLLOW fails rather than follow
    if (fd<0 && errno==EACCES && !fchmodat(dir, name, 0700, AT_SYMLINK_NOFOLLOW)) {
        fd = openat(dir, name, flags);
    }
    struct stat st;
    if (fd>=0 && !fstat(fd, &st) && (st.st_mode&0700)!=0700) {
        fchmod(fd, (st.st_mode & 07777) | 0700);
    }
    return fd;
}

// The names in a directory, but "." and "..", or null names on failure.
static RmDir rmread(Arena *a, i32 fd, char *name)
{
    RmDir r = {};
    r.fd   = fd;
    r.name = name;
    i32  dup  = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    DIR *dir  = dup<0 ? 0 : fdopendir(dup);
    if (!dir) {
        if (dup >= 0) close(dup);
        return r;
    }
    Slice<char *> names = {};
    for (struct dirent *e; (e = readdir(dir));) {
        Str n = {(u8 *)e->d_name, 0};
        while (n.data[n.len]) n.len++;
        if (!(n == ".") && !(n == "..")) {
            names = push(a, names, cstr(a, n));
        }
    }
    closedir(dir);
    r.names = names.data ? names.data : alloc<char *>(a);
    r.len   = names.len;
    return r;
}

// Depth first with a stack of its own, descending only into directories
// found by fstatat without following, each opened with O_NOFOLLOW, every
// name taken relative to its directory's descriptor: a link anywhere in
// the tree, or one put in place of a directory meanwhile, is unlinked,
// never followed.
static b32 os_removetree(Os *, Arena scratch, Str path)
{
    if (!path.len || hasnul(path)) {
        return 0;
    }
    char *p = cstr(&scratch, path);
    struct stat st;
    if (lstat(p, &st)) {
        return errno == ENOENT;
    } else if (!S_ISDIR(st.st_mode)) {
        return !unlink(p) || errno==ENOENT;
    }
    i32 fd = rmopen(AT_FDCWD, p);
    if (fd < 0) {
        return errno == ENOENT;
    }

    b32          ok    = 1;
    Slice<RmDir> stack = {};
    RmDir        top   = rmread(&scratch, fd, p);
    ok &= !!top.names;
    stack = push(&scratch, stack, top);
    while (stack.len) {
        RmDir *d = &stack[stack.len-1];
        if (d->next < d->len) {
            char *name = d->names[d->next++];
            if (fstatat(d->fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
                ok &= errno == ENOENT;
            } else if (!S_ISDIR(st.st_mode)) {
                ok &= !unlinkat(d->fd, name, 0) || errno==ENOENT;
            } else if ((fd = rmopen(d->fd, name)) < 0) {
                ok &= errno == ENOENT;
            } else {
                RmDir sub = rmread(&scratch, fd, name);
                ok &= !!sub.names;
                stack = push(&scratch, stack, sub);
            }
            continue;
        }
        RmDir done = *d;
        stack.len--;
        close(done.fd);
        if (stack.len) {
            RmDir *up = &stack[stack.len-1];
            ok &= !unlinkat(up->fd, done.name, AT_REMOVEDIR) || errno==ENOENT;
        } else {
            ok &= !rmdir(p) || errno==ENOENT;
        }
    }
    return ok;
}

static b32 os_copyfile(Os *, Arena scratch, Str from, Str to)
{
    if (hasnul(from) || hasnul(to)) {
        return 0;
    }
    i32 in = open(cstr(&scratch, from), O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (in < 0) {
        return 0;
    }
    struct stat st;
    if (fstat(in, &st) || !S_ISREG(st.st_mode)) {
        close(in);
        return 0;
    }
    char *dst = cstr(&scratch, to);
    i32   out = open(dst, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0666);
    if (out < 0) {
        close(in);
        return 0;
    }
    iz  cap = (iz)1 << 16;
    u8 *buf = allocbytes(&scratch, cap);
    b32 ok  = 1;
    for (;;) {
        iz n = read(in, buf, (uz)cap);
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            ok = !n;
            break;
        } else if (!writeall(out, {buf, n})) {
            ok = 0;
            break;
        }
    }
    close(in);
    ok &= !close(out);
    if (!ok) {
        unlink(dst);
    }
    return ok;
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
    char *out = (char *)allocbytes(a, 4096);  // PATH_MAX
    Arena tmp = *a;
    if (!realpath(cstr(&tmp, path), out)) {
        a->beg = (byte *)out;
        return {};
    }
    Str r = {(u8 *)out, 0};
    while (r.data[r.len]) r.len++;
    a->beg = (byte *)(r.data + r.len);
    return r;
}

// Session locks: flock, which the kernel releases when the descriptor
// closes, as it does when the process dies. The descriptor is
// close-on-exec, so no child inherits the lock. A test takes a shared
// lock for a moment, so an exclusive lock that fails where a shared one
// succeeds met only a test, and tries again.
static iz os_lockfile(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return -1;
    }
    i32 fd = open(cstr(&scratch, path), O_RDWR|O_CREAT|O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    for (i32 n = 0; n < 200; n++) {
        if (!flock(fd, LOCK_EX|LOCK_NB)) {
            return fd;
        } else if (errno == EINTR) {
            continue;
        } else if (errno!=EWOULDBLOCK || flock(fd, LOCK_SH|LOCK_NB)) {
            break;  // held for real
        }
        flock(fd, LOCK_UN);
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, 0);
    }
    close(fd);
    return -1;
}

static void os_unlockfile(Os *, iz lock)
{
    close((i32)lock);
}

static b32 os_lockheld(Os *, Arena scratch, Str path)
{
    if (hasnul(path)) {
        return 0;
    }
    i32 fd = open(cstr(&scratch, path), O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return 0;  // no file, no lock
    }
    b32 held = flock(fd, LOCK_SH|LOCK_NB) && errno==EWOULDBLOCK;
    close(fd);  // releasing the test's own lock
    return held;
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
//   prospero --reap CWD ARGV...
// with null stdin (or an input pipe), the output pipe on fds 1 and 2 (or
// fd 1, with errors to a file), the lifeline on fd 3, and a status pipe
// on fd 4. The reaper leads a new session and process
// group and starts the tool in it, then reports the tool's exit status
// on fd 4 and stays alive, so that the group id, its own pid, cannot be
// reused until the tree is gone. Every reaper shares the lifeline, whose
// write end only the harness holds: when the harness dies, by any means,
// the lifeline reads EOF and each reaper ends its tree, TERM and then
// KILL 2 s later.
//
// A hard kill (os_kill) asks the reaper, with SIGUSR1, to sweep: it
// kills every process it can find that came from the tool, round after
// round until none is left, then its group with itself in it. That
// covers the processes that left the group (setsid, setpgid). On Linux
// the reaper is a child subreaper, so such escapees become its children
// once their parents die, and the sweep finds the whole tree through
// /proc. On macOS it records the tool's descendants, pid, start time
// and unique id, every 250 ms and as soon as one it watches forks
// (EVFILT_PROC), and the sweep kills those still alive as recorded,
// whatever is left in its session, and what a scan of every process
// finds by origin (reaporigin): orphans of the processes recorded,
// which keep their parents' unique ids until they exec, and when the
// reaper was spawned disclaimed, as "prospero --reap-disclaimed",
// whatever it answers for as the responsible process of the tool's
// tree. A reaper is disclaimed only when the harness answers for
// itself, as an app does, and its executable is still the one it runs,
// so that no tool loses the privacy grants it would have had
// (reap.cpp). A process a debugger attached to is the debugger's child
// meanwhile, and is not the tool's for that. What still gets away: a
// process that disclaims itself, one that launchd starts for the tool
// (XPC, open), and without responsibility, an orphan that the reaper
// never saw, if its parents were all gone before the reaper saw them,
// or if it exec'd after they were.
//
// Each Proc has a reader thread, pumping output into the file, and a
// waiter thread, reading the status and then reaping the reaper. Both
// hold references, as does the engine until os_procfree. A child from
// os_spawnio has its standard input on fd 0 instead of null, and a
// writer thread feeding it from a queue, with a reference of its own;
// its reader passes output on as records instead of into a file, and
// its errors go into the file directly.
//
// Every signal goes out through sendsignal, which refuses a pid or a
// group id that would reach more than meant (reap.cpp). A reaper signals
// its own group only if setsid made it the group's leader.

// Signal one process, or with group one process group, by its positive
// id: never the caller's own group (0), every process (-1), or init.
// Debug builds trap on such an id; others refuse it (EINVAL).
static b32 sendsignal(i64 id, i32 sig, b32 group)
{
    if (!signalable(id)) {
        #ifndef NDEBUG
        __builtin_trap();
        #endif
        errno = EINVAL;
        return 0;
    }
    return group ? !killpg((pid_t)id, sig) : !kill((pid_t)id, sig);
}

enum : iz {
    PROC_TAILMAX  = 1<<12,
    PROC_BUFLEN   = 1<<16,
    PROC_QUEUEMAX = (iz)64 << 20,  // bytes waiting for a child's input
};

struct ProcChunk {  // queued input, its bytes following
    ProcChunk *next;
    iz         len;
};

struct Proc {
    Lock       lock;
    i32        refs;      // atomic
    i32        pid;       // the reaper, leader of the process group
    b32        reaped;    // under lock: pid may be reused
    Mailbox   *mbox;
    u64        id;
    i32        outfd;     // output pipe read end, owned by the reader
    i32        statfd;    // status pipe read end, owned by the waiter
    i32        filefd;    // the output file, owned by the reader
    i64        maxbytes;
    i64        total;     // reader only
    iz         tailpos;   // reader only: next write in the tail ring
    // os_spawnio
    b32        io;
    i32        infd;      // input pipe write end, owned by the writer
    Event      wake;      // for the writer
    ProcChunk *head;      // under lock: the input queue
    ProcChunk *last;      // under lock
    iz         queued;    // under lock: bytes in the queue
    b32        closing;   // under lock: close the input after the queue
    b32        broken;    // under lock: the input is closed
    u8         tail[PROC_TAILMAX];
    u8         msg[8+PROC_TAILMAX];
    u8         buf[PROC_BUFLEN];
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

// The reader of a child from os_spawnio: output as records, in order,
// waiting for room in the mailbox.
static void procpipe(void *arg)
{
    Proc *p   = (Proc *)arg;
    iz    max = mailmax(p->mbox);
    for (;;) {
        iz n = read(p->outfd, p->buf, PROC_BUFLEN);
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            break;
        }
        for (iz off = 0; off < n; off += max) {
            iz len = n-off<max ? n-off : max;
            mailpost(p->mbox, PROC_DATA, p->id, {p->buf+off, len}, 1);
        }
        p->total += n;
    }
    close(p->outfd);
    u8 msg[8];
    put64(msg, p->total);
    mailpost(p->mbox, PROC_EOF, p->id, {msg, 8}, 1);
    procrelease(p);
}

static void freequeue(Proc *p)  // under lock
{
    for (ProcChunk *c = p->head; c;) {
        ProcChunk *next = c->next;
        free(c);
        c = next;
    }
    p->head   = 0;
    p->last   = 0;
    p->queued = 0;
}

// Feed queued input to the child until told to close it, or until the
// child stops reading (EPIPE). Signals to wake it come only under the
// lock while neither closing nor broken, so once it sees either, nobody
// touches its event again and it may close it.
static void procwriter(void *arg)
{
    Proc *p = (Proc *)arg;
    for (;;) {
        os_lock(&p->lock);
        ProcChunk *c = p->head;
        if (c) {
            p->head = c->next;
            p->last = p->head ? p->last : 0;
        }
        b32 done = !c && p->closing;
        os_unlock(&p->lock);
        if (done) {
            break;
        } else if (!c) {
            os_wait(&p->wake, -1);
            continue;
        }

        b32 ok = writeall(p->infd, {(u8 *)(c + 1), c->len});
        os_lock(&p->lock);
        p->queued -= c->len;
        if (!ok) {
            p->broken = 1;
            freequeue(p);
        }
        os_unlock(&p->lock);
        free(c);
        if (!ok) {
            break;
        }
    }
    close(p->infd);
    close((i32)p->wake.h[0]);
    close((i32)p->wake.h[1]);
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

    // The reaper lives until the group is killed. Wait for its death
    // without reaping it, then reap it under the lock, as reaped is set,
    // so that os_kill, under the lock, sees reaped unset only while the
    // reaper's pid is still held, by it or by its zombie.
    siginfo_t si = {};
    i32       r  = 0;
    while ((r = waitid(P_PID, (id_t)p->pid, &si, WEXITED|WNOWAIT))<0 && errno==EINTR) {}
    i32 status = 0;
    os_lock(&p->lock);
    if (!r) {
        while (waitpid(p->pid, &status, 0)<0 && errno==EINTR) {}  // at once
    }
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

// Signal state for a child: nothing blocked, since spawning threads
// block every signal. Handlers reset on exec by themselves, and only
// ignored signals carry over, so only those need SETSIGDEF: none for a
// reaper, which sets up its own, and for a tool the four its reaper
// ignores. Restoring every signal costs a sigaction call each in the
// child, a noticeable part of a spawn on a slow machine.
static void childsignals(posix_spawnattr_t *attr, short flags, b32 tool)
{
    sigset_t none, ignored;
    sigemptyset(&none);
    sigemptyset(&ignored);
    sigaddset(&ignored, SIGTERM);
    sigaddset(&ignored, SIGINT);
    sigaddset(&ignored, SIGHUP);
    sigaddset(&ignored, SIGPIPE);
    posix_spawnattr_init(attr);
    posix_spawnattr_setsigmask(attr, &none);
    flags |= POSIX_SPAWN_SETSIGMASK;
    if (tool) {
        posix_spawnattr_setsigdefault(attr, &ignored);
        flags |= POSIX_SPAWN_SETSIGDEF;
    }
    posix_spawnattr_setflags(attr, flags);
}

// Undo a spawn: kill the reaper, which then starts nothing more, and
// its group, which holds the tool if it was started. The group alone
// would miss a reaper yet to call setsid, and leave it to run the tool.
// Only while its pid is held: before its waiter starts (no Proc), this
// thread reaps it afterward, and once it has a waiter, which reaps it
// under the Proc's lock, only while not reaped.
static void spawnkill(i32 pid, Proc *p)
{
    if (p) {
        os_lock(&p->lock);
    }
    if (!p || !p->reaped) {
        sendsignal(pid, SIGKILL, 0);
        sendsignal(pid, SIGKILL, 1);
    }
    if (p) {
        os_unlock(&p->lock);
    }
}

static FileId fileid(struct stat *st)
{
    #ifdef __APPLE__
    struct timespec m = st->st_mtimespec;
    struct timespec c = st->st_ctimespec;
    #else
    struct timespec m = st->st_mtim;
    struct timespec c = st->st_ctim;
    #endif
    FileId r = {};
    r.dev   = (i64)st->st_dev;
    r.ino   = (i64)st->st_ino;
    r.size  = (i64)st->st_size;
    r.mtime = (i64)m.tv_sec*1000000000 + m.tv_nsec;
    r.ctime = (i64)c.tv_sec*1000000000 + c.tv_nsec;
    return r;
}

// Whether the executable that reapers run is still the file this
// process started from, as it was then: neither replaced, as a rebuild
// replaces it, nor rewritten in place. Unknown is no.
static b32 sameexe(Os *os)
{
    struct stat st;
    if (!os->exeid.ino || stat(os->exe, &st)) {
        return 0;
    }
    FileId now = fileid(&st);
    return now.dev==os->exeid.dev && now.ino==os->exeid.ino &&
           now.size==os->exeid.size && now.mtime==os->exeid.mtime &&
           now.ctime==os->exeid.ctime;
}

// Spawn under a reaper. With io, input is a pipe from the writer
// thread, errors go to outpath (or /dev/null) directly, and the reader
// passes output on as records instead of into the file. On macOS the
// reaper is spawned disclaimed, its tool's responsible process, only
// while that changes nothing privacy grants answer to (reapdisclaim).
static Proc *spawnproc(Os *os, Spawn *sp, Arena scratch, b32 io)
{
    if (!sp->argv.len || hasnul(sp->cwd) || hasnul(sp->outpath)) {
        errno = EINVAL;
        return 0;
    }

    // Child-side descriptors: input (null, or a pipe's read end), output
    // write end, errors (the same, or the file), status write end
    i32   in[2]  = {-1, -1};
    i32   out[2] = {-1, -1};
    i32   st[2]  = {-1, -1};
    Event wake   = {{-1, -1}};
    i32   null   = open("/dev/null", O_RDWR|O_CLOEXEC);
    i32   file   = -1;
    b32   ok     = null >= 0;
    if (ok && (!io || sp->outpath.len)) {
        file = open(cstr(&scratch, sp->outpath),
                    O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
        ok   = file >= 0;
    }
    ok = ok && cloexecpipe(out) && cloexecpipe(st) &&
         (!io || (cloexecpipe(in) && os_eventinit(&wake)));
    if (!ok) {
        i32 save = errno;
        i32 fds[] = {null, file, in[0], in[1], out[0], out[1], st[0], st[1]};
        closeall(fds, countof(fds));
        errno = save;
        return 0;
    }
    null   = highfd(null);
    out[1] = highfd(out[1]);
    st[1]  = highfd(st[1]);
    if (io) {
        in[0] = highfd(in[0]);
        file  = highfd(file);
    }
    i32 fd0 = io ? in[0] : null;
    i32 fd2 = !io ? out[1] : sp->outpath.len ? file : null;
    b32 disclaim = os->resp.disclaim &&
                   reapdisclaim(getpid(), os->respself, sameexe(os));

    iz     argc = 3 + sp->argv.len;
    char **argv = alloc<char *>(&scratch, argc+1);
    argv[0] = os->exe;
    argv[1] = disclaim ? (char *)"--reap-disclaimed" : (char *)"--reap";
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
    posix_spawn_file_actions_adddup2(&fa, fd0, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd2, 2);
    posix_spawn_file_actions_adddup2(&fa, os->lifer, 3);
    posix_spawn_file_actions_adddup2(&fa, st[1], 4);
    posix_spawnattr_t attr;
    short flags = 0;
    #ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    #endif
    childsignals(&attr, flags, 0);
    if (disclaim) {
        os->resp.disclaim(&attr, 1);
    }
    pid_t pid = 0;
    i32   err = fd0<0 || out[1]<0 || fd2<0 || st[1]<0 ? EMFILE :
                posix_spawn(&pid, os->exe, &fa, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    i32 child[] = {null, out[1], st[1], in[0], io ? file : -1};
    closeall(child, countof(child));
    file = io ? -1 : file;
    i32 mine[] = {file, out[0], st[0], in[1], (i32)wake.h[0], (i32)wake.h[1]};
    if (err) {
        closeall(mine, countof(mine));
        errno = err;
        return 0;
    }

    Proc *p = (Proc *)malloc(sizeof(Proc));
    if (!p) {
        spawnkill(pid, 0);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        closeall(mine, countof(mine));
        errno = ENOMEM;
        return 0;
    }
    __builtin_memset(p, 0, __builtin_offsetof(Proc, tail));
    os_lockinit(&p->lock);
    p->refs     = io ? 4 : 3;
    p->pid      = pid;
    p->mbox     = sp->mbox;
    p->id       = sp->id;
    p->outfd    = out[0];
    p->statfd   = st[0];
    p->filefd   = file;
    p->maxbytes = sp->maxbytes;
    p->io       = io;
    p->infd     = in[1];
    p->wake     = wake;

    if (!os_thread(os, io ? procpipe : procreader, p)) {
        spawnkill(pid, 0);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        closeall(mine, countof(mine));
        pthread_mutex_destroy((pthread_mutex_t *)p->lock.opaque);
        free(p);
        errno = EAGAIN;
        return 0;
    }
    i32 input[] = {in[1], (i32)wake.h[0], (i32)wake.h[1]};
    if (!os_thread(os, procwaiter, p)) {
        // The reader finishes once the group is dead
        spawnkill(pid, 0);  // no waiter
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        close(st[0]);
        closeall(input, countof(input));
        if (io) procrelease(p);  // the writer's
        procrelease(p);  // the waiter's
        procrelease(p);  // the caller's
        errno = EAGAIN;
        return 0;
    }
    if (io && !os_thread(os, procwriter, p)) {
        spawnkill(pid, p);  // the reader and waiter then finish
        closeall(input, countof(input));
        procrelease(p);  // the writer's
        procrelease(p);  // the caller's
        errno = EAGAIN;
        return 0;
    }
    return p;
}

static Proc *os_spawn(Os *os, Spawn *sp, Arena scratch)
{
    return spawnproc(os, sp, scratch, 0);
}

static Proc *os_spawnio(Os *os, Spawn *sp, Arena scratch)
{
    return spawnproc(os, sp, scratch, 1);
}

// A grandchild, so nothing waits for it and no reaper sweeps it: the
// child starts a session, forks it, and exits at once. Between fork and
// exec only async-signal-safe calls, everything prepared ahead.
static b32 os_launch(Os *, Arena scratch, Slice<Str> argv, Str cwd, Slice<Str> env)
{
    if (!argv.len || hasnul(cwd)) {
        errno = EINVAL;
        return 0;
    }
    Str prog = argv[0];
    if (find(prog, "/") < 0) {
        Str path = {};
        for (iz i = 0; i < env.len; i++) {
            if (startswith(env[i], "PATH=")) {
                path = cuthead(env[i], 5);
            }
        }
        for (Str rest = path; rest.len && prog==argv[0];) {
            Cut c = cut(rest, ':');
            rest = c.tail;
            Arena a = scratch;
            Str   p = join(&a, c.head.len ? c.head : Str("."), argv[0]);
            if (!access(cstr(&a, p), X_OK)) {
                prog = clone(&scratch, p);
            }
        }
    }
    char **av = alloc<char *>(&scratch, argv.len+1);
    for (iz i = 0; i < argv.len; i++) {
        av[i] = cstr(&scratch, argv[i]);
    }
    char **ev = alloc<char *>(&scratch, env.len+1);
    for (iz i = 0; i < env.len; i++) {
        ev[i] = cstr(&scratch, env[i]);
    }
    char *file = cstr(&scratch, prog);
    char *dir  = cstr(&scratch, cwd);
    if (access(file, X_OK)) {
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    } else if (!pid) {
        setsid();
        if (fork()) {
            _exit(0);
        }
        i32 null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        for (i32 fd = 3; fd < 1024; fd++) {
            close(fd);
        }
        if (dir[0] && chdir(dir)) {
            _exit(127);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, 0);
        execve(file, av, ev);
        _exit(127);
    }
    i32 status = 0;
    while (waitpid(pid, &status, 0)<0 && errno==EINTR) {}
    return 1;
}

static b32 os_procwrite(Os *, Proc *p, Str data)
{
    if (!p->io || data.len>PROC_QUEUEMAX) {
        return 0;
    }
    ProcChunk *c = 0;
    if (data.len) {
        c = (ProcChunk *)malloc(sizeof(ProcChunk) + (uz)data.len);
        if (!c) {
            return 0;
        }
        c->next = 0;
        c->len  = data.len;
        copybytes(c+1, data.data, data.len);
    }

    os_lock(&p->lock);
    b32 ok = !p->closing && !p->broken && data.len<=PROC_QUEUEMAX-p->queued;
    if (ok && c) {
        *(p->last ? &p->last->next : &p->head) = c;
        p->last    = c;
        p->queued += c->len;
        c = 0;
    }
    if (ok && !data.data) {
        p->closing = 1;
    }
    if (ok) {
        os_signal(&p->wake);
    }
    os_unlock(&p->lock);
    free(c);  // unless queued
    return ok;
}

// Nothing for a Proc without a reaper's pid, or once the reaper is
// reaped and its pid free for another process (reapkill). The waiter
// reaps it under the lock, so under the lock here its pid is held while
// reaped is unset.
static void os_kill(Os *, Proc *p, b32 hard)
{
    os_lock(&p->lock);
    i32 act = reapkill(p->pid, p->reaped, hard);
    if (act == REAPKILL_HARD) {
        siginfo_t si = {};
        i32       r  = waitid(P_PID, (id_t)p->pid, &si, WEXITED|WNOHANG|WNOWAIT);
        act = reaphard(!r, si.si_pid);
    }
    switch (act) {
    case REAPKILL_TERM:
        sendsignal(p->pid, SIGTERM, 1);  // the reaper ignores it
        break;
    case REAPKILL_SWEEP:
        sendsignal(p->pid, SIGUSR1, 0);
        sendsignal(p->pid, SIGCONT, 0);  // should something have stopped it
        break;
    case REAPKILL_GROUP:
        sendsignal(p->pid, SIGKILL, 1);  // the zombie holds the group's id
        break;
    }
    os_unlock(&p->lock);
}

static void os_procfree(Os *os, Proc *p)
{
    if (p->io) {
        os_procwrite(os, p, {});  // close the input after the queue
    }
    procrelease(p);
}

// Reaper only: the self-pipe's write end, for SIGCHLD and SIGUSR1, and
// whether a sweep was asked for. The reaper is single-threaded.
static i32          reapchld = -1;
static volatile i32 reapasked;

static void onchld(int)
{
    i32 save = errno;
    u8  b    = 0;
    while (write(reapchld, &b, 1)<0 && errno==EINTR) {}
    errno = save;
}

static void onsweep(int sig)
{
    reapasked = 1;
    onchld(sig);
}

static void reapsay(char const *what, char const *arg)
{
    Str parts[] = {"prospero: ", {}, " ", {}, ": ", {}, "\n"};
    parts[1] = {(u8 *)what, (iz)__builtin_strlen(what)};
    parts[3] = {(u8 *)arg, (iz)__builtin_strlen(arg)};
    parts[2] = parts[3].len ? parts[2] : Str{};
    char const *err = strerror(errno);  // single-threaded here
    parts[5] = {(u8 *)err, (iz)__builtin_strlen(err)};
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

enum : iz {
    REAP_PROCS = 1<<15,  // processes on the system, as much as is looked at
    REAP_SEEN  = 1<<12,  // descendants recorded (macOS)
    REAP_IDS   = 1<<14,  // unique ids of the tool's processes kept (macOS)
};

struct Reaper {
    i32      self;
    i32      group;     // its own group, to signal at the end, or 0 (reapgroup)
    i32      tool;
    b32      reported;  // the tool's status went out on fd 4
    ReapProc seen[REAP_SEEN];
    iz       nseen;
    // macOS
    Resp     spi;
    u64      uniq;           // its own unique id
    u64      resp;           // the responsible id to sweep for, or 0 (reaprespid)
    i32      kq;             // the forks of what is recorded, or -1
    u64      ids[REAP_IDS];  // the tool's processes', alive or gone (reapaddid)
    iz       nids;
    iz       idcap;          // REAP_IDS, or 0 to find none by forking parent
};

static void reapnap(i64 ms)
{
    struct timespec ts = {(time_t)(ms/1000), (long)(ms%1000*1000000)};
    while (nanosleep(&ts, &ts)) {}
}

// Collect exited children: the tool, whose status is reported once, and
// on Linux the orphans that came to the subreaper.
static void reapchildren(Reaper *r)
{
    for (;;) {
        i32 status = 0;
        i32 pid    = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            return;
        } else if (pid==r->tool && !r->reported) {
            r->reported = 1;
            if (WIFSIGNALED(status)) {
                reapstatus(-1, WTERMSIG(status));
            } else {
                reapstatus(WEXITSTATUS(status), 0);
            }
        }
    }
}

#if defined(__linux__)
// One live process from /proc, its pid, parent, group, session and start
// time (reapstat), or a zero pid if gone, a zombie, or not to signal.
static ReapProc reapproc(i64 pid)
{
    ReapProc r = {};
    if (!signalable(pid)) {
        return r;
    }
    char path[32] = "/proc/";
    char digits[16];
    iz   len = 6;
    iz   n   = 0;
    for (i64 v = pid; v; v /= 10) {
        digits[n++] = (char)('0' + v%10);
    }
    while (n) {
        path[len++] = digits[--n];
    }
    copybytes(path+len, (void *)"/stat", 6);
    i32 fd = open(path, O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return r;
    }
    char buf[512];
    iz   got = read(fd, buf, sizeof(buf));
    close(fd);
    if (got <= 0) {
        return r;
    }
    r = reapstat({(u8 *)buf, got});
    return r.pid==pid ? r : ReapProc{};
}

// Every live process, from /proc.
static iz reapscan(ReapProc *out, iz cap)
{
    DIR *d = opendir("/proc");
    if (!d) {
        return 0;
    }
    iz n = 0;
    for (struct dirent *e; n<cap && (e = readdir(d));) {
        Int pid = parseint(Str{(u8 *)e->d_name, (iz)__builtin_strlen(e->d_name)});
        if (pid.ok) {
            ReapProc p = reapproc(pid.val);
            if (p.pid) {
                out[n++] = p;
            }
        }
    }
    closedir(d);
    return n;
}

// Signal a process a scan found, if it is still the one found: the same
// start time, read again after a pidfd takes hold of it, where the
// kernel has them, so that no other process given its pid meanwhile can
// get the signal; else just before the kill, which leaves a moment.
static void reapsignalone(ReapProc *p, i32 sig)
{
    #if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    i32 fd = (i32)syscall(SYS_pidfd_open, p->pid, 0u);
    if (fd<0 && errno==ESRCH) {
        return;  // gone
    } else if (fd >= 0) {
        ReapProc now = reapproc(p->pid);
        if (now.pid && now.start==p->start) {
            syscall(SYS_pidfd_send_signal, fd, sig, (siginfo_t *)0, 0u);
        }
        close(fd);
        return;
    }
    #endif
    ReapProc now = reapproc(p->pid);
    if (now.pid && now.start==p->start) {
        sendsignal(p->pid, sig, 0);
    }
}

static b32 ppidless(ReapProc *a, ReapProc *b)
{
    return a->ppid < b->ppid;
}

// Signal what came from the tool: the reaper's descendants, everything
// orphaned having come to it, and anything left in its session, if it
// leads one. Returns how many were found to signal.
static iz reapsignal(Reaper *r, i32 sig)
{
    static ReapProc procs[REAP_PROCS];
    static ReapProc tmp[REAP_PROCS+1];
    static u8       mine[REAP_PROCS];
    static i32      queue[REAP_PROCS];
    iz n = reapscan(procs, REAP_PROCS);

    // Breadth first from the reaper, children found by parent
    Arena a = {(byte *)tmp, (byte *)(tmp + REAP_PROCS + 1)};
    sort(procs, n, ppidless, a);
    for (iz i = 0; i < n; i++) {
        mine[i] = 0;
    }
    iz head = 0, tail = 0;
    queue[tail++] = r->self;
    while (head < tail) {
        i32 parent = queue[head++];
        iz  lo = 0, hi = n;
        while (lo < hi) {
            iz mid = lo + (hi-lo)/2;
            if (procs[mid].ppid < parent) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        for (iz i = lo; i<n && procs[i].ppid==parent && tail<REAP_PROCS; i++) {
            if (!mine[i]) {
                mine[i] = 1;
                queue[tail++] = procs[i].pid;
            }
        }
    }

    iz count = 0;
    for (iz i = 0; i < n; i++) {
        if (reapmine(&procs[i], r->self, r->group, mine[i])) {
            reapsignalone(&procs[i], sig);
            count++;
        }
    }
    return count;
}

static void reapsnapshot(Reaper *) {}
static void reapforks(Reaper *) {}

#elif defined(__APPLE__)
// proc_pidinfo's PROC_PIDUNIQIDENTIFIERINFO, from xnu's
// proc_info_private.h, which the SDK leaves out
enum : i32 { PIDUNIQINFO = 17 };

struct UniqInfo {
    u8  uuid[16];
    u64 uniq;         // p_uniqueid
    u64 puniq;        // p_puniqueid, its parent's at its fork or last exec
    i32 idversion;
    i32 origversion;  // p_orig_ppidversion
    u64 reserved[2];
};
static_assert(sizeof(UniqInfo) == 56);

// Start time in microseconds of a live process, or 0 if gone, a zombie,
// or not to be seen.
static i64 reapstart(i32 pid)
{
    struct proc_bsdinfo bi = {};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, PROC_PIDTBSDINFO_SIZE) !=
            PROC_PIDTBSDINFO_SIZE || bi.pbi_status==SZOMB) {
        return 0;
    }
    return (i64)bi.pbi_start_tvsec*1000000 + (i64)bi.pbi_start_tvusec;
}

// The parent a traced process had when its debugger attached, p_oppid,
// which only the sysctl reports, or 0 if unknown.
static i32 reapoppid(i32 pid)
{
    struct kinfo_proc kp     = {};
    uz                len    = sizeof(kp);
    i32               mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &kp, &len, 0, 0) || len!=sizeof(kp)) {
        return 0;
    }
    return kp.kp_proc.p_oppid;
}

// A live process, its parent, group, start time, unique ids, and if
// traced, its parent before (reap.cpp), and with resp its responsible
// process's, or a zero pid if gone, a zombie, or not to be seen.
static ReapProc reapinfo(Reaper *r, i32 pid, b32 resp)
{
    ReapProc            p  = {};
    struct proc_bsdinfo bi = {};
    UniqInfo            u  = {};
    if (!signalable(pid) ||
            proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, PROC_PIDTBSDINFO_SIZE) !=
                PROC_PIDTBSDINFO_SIZE || bi.pbi_status==SZOMB ||
            proc_pidinfo(pid, PIDUNIQINFO, 0, &u, sizeof(u)) != (i32)sizeof(u)) {
        return p;
    }
    p.pid    = pid;
    p.ppid   = (i32)bi.pbi_ppid;
    p.pgrp   = (i32)bi.pbi_pgid;
    p.start  = (i64)bi.pbi_start_tvsec*1000000 + (i64)bi.pbi_start_tvusec;
    p.uniq   = u.uniq;
    p.puniq  = u.puniq;
    p.resp   = resp ? r->spi.uniqfor(pid) : 0;
    p.traced = !!(bi.pbi_flags & PROC_FLAG_TRACED);
    p.oppid  = p.traced ? reapoppid(pid) : 0;
    return p;
}

static b32 reapknown(Reaper *r, i32 pid)
{
    for (iz i = 0; i < r->nseen; i++) {
        if (r->seen[i].pid == pid) {
            return 1;
        }
    }
    return 0;
}

// Watch a process recorded for its forks, to list its children at once,
// before a short-lived one can fork in turn and be gone unseen, as the
// middle of a double fork is. A watch ends with its process, and its
// events carry the unique id of the process it was for.
static void reapwatch(Reaper *r, ReapProc *p)
{
    static_assert(sizeof(void *) == sizeof(u64));
    struct kevent ev = {};
    EV_SET(&ev, (uz)p->pid, EVFILT_PROC, EV_ADD, NOTE_FORK, 0, (void *)(uz)p->uniq);
    if (r->kq >= 0) {
        kevent(r->kq, &ev, 1, 0, 0, 0);  // fails for one gone already
    }
}

// Record the children of a process, by its pid and unique id, that are
// not recorded yet, with their ids among the tool's, and watch them:
// only those it forked (reapforked), not the user's process that a
// debugger among the tool's attached to, nor the children of another
// process given its pid since.
static void reaplist(Reaper *r, i32 parent, u64 parentuniq)
{
    static i32 kids[4096];
    i32 bytes = proc_listpids(PROC_PPID_ONLY, (u32)parent, kids, sizeof(kids));
    for (iz i = 0; i < bytes/(iz)sizeof(i32); i++) {
        i32 pid = kids[i];
        if (!signalable(pid) || reapknown(r, pid) || r->nseen==REAP_SEEN) {
            continue;
        }
        ReapProc p = reapinfo(r, pid, 0);
        if (p.pid && reapforked(&p, parent, parentuniq)) {
            r->seen[r->nseen++] = p;
            r->nids = reapaddid(r->ids, r->nids, r->idcap, p.uniq);
            reapwatch(r, &p);
        }
    }
}

// Record the descendants of the reaper and of every descendant seen
// before, which may have left for launchd, by pid, start time and
// unique id. Those gone since are forgotten first, so that the table
// holds only the live (a dead parent's later children cannot be found
// through it anyway), and a pid used again is recorded again, but their
// ids stay among the tool's, for their orphans.
static void reapsnapshot(Reaper *r)
{
    iz live = 0;
    for (iz i = 0; i < r->nseen; i++) {
        ReapProc *p = &r->seen[i];
        if (p->start && reapstart(p->pid)==p->start) {
            r->seen[live++] = *p;
        }
    }
    r->nseen = live;
    reaplist(r, r->self, r->uniq);
    for (iz at = 0; at < r->nseen; at++) {
        reaplist(r, r->seen[at].pid, r->seen[at].uniq);
    }
}

// The forks of the processes watched (reapwatch): list the children of
// each that forked, and theirs in turn, as they are found.
static void reapforks(Reaper *r)
{
    struct kevent   evs[64];
    struct timespec now = {};
    while (r->kq >= 0) {
        i32 n = kevent(r->kq, 0, 0, evs, (i32)countof(evs), &now);
        for (i32 i = 0; i < n; i++) {
            if (evs[i].fflags & NOTE_FORK) {
                iz at = r->nseen;
                reaplist(r, (i32)evs[i].ident, (u64)(uz)evs[i].udata);
                for (; at < r->nseen; at++) {
                    reaplist(r, r->seen[at].pid, r->seen[at].uniq);
                }
            }
        }
        if (n < countof(evs)) {
            return;
        }
    }
}

// Signal what came from the tool: the descendants recorded; what a scan
// of every process finds of the tool's by origin (reaporigin), recorded
// in turn; and what is left in the reaper's session, if it leads one.
// Each only while its start time is still the one recorded, or listed.
// The session's are found among this user's processes by getsid, since
// the kernel has no KERN_PROC_SESSION to ask (ENOENT), and only
// descendants can be in it.
static iz reapsignal(Reaper *r, i32 sig)
{
    reapsnapshot(r);
    iz count = 0;
    iz live  = 0;
    for (iz i = 0; i < r->nseen; i++) {
        ReapProc *p = &r->seen[i];
        if (p->start && reapstart(p->pid)==p->start &&
                reapmine(p, r->self, r->group, 1)) {
            sendsignal(p->pid, sig, 0);
            count++;
            r->seen[live++] = *p;
        }
    }
    r->nseen = live;  // gone for good, or another's pid: forgotten

    static i32      pids[REAP_PROCS];
    static ReapProc procs[REAP_PROCS];
    static u8       mine[REAP_PROCS];
    i32 total = proc_listallpids(pids, (i32)sizeof(pids));
    iz  n     = 0;
    for (i32 i = 0; i < total; i++) {
        if (pids[i]==r->self || reapknown(r, pids[i])) {
            continue;  // signaled above
        }
        ReapProc p = reapinfo(r, pids[i], r->resp!=0);
        if (p.pid) {
            mine[n]    = 0;
            procs[n++] = p;
        }
    }
    r->nids = reaporigin(procs, n, mine, r->ids, r->nids, r->idcap, r->resp);
    for (iz i = 0; i < n; i++) {
        ReapProc *p = &procs[i];
        if (mine[i] && reapmine(p, r->self, r->group, 1) &&
                reapstart(p->pid)==p->start) {
            sendsignal(p->pid, sig, 0);
            count++;
            if (r->nseen < REAP_SEEN) {
                r->seen[r->nseen++] = *p;
                reapwatch(r, p);
            }
        }
    }

    if (!r->group) {
        return count;
    }
    i32 mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_UID, (i32)getuid()};
    uz  len    = 0;
    if (sysctl(mib, 4, 0, &len, 0, 0) || !len) {
        return count;
    }
    len += len/2;  // room for growth since
    struct kinfo_proc *kp = (struct kinfo_proc *)malloc(len);
    if (kp && !sysctl(mib, 4, kp, &len, 0, 0)) {
        for (iz i = 0; i < (iz)(len/sizeof(*kp)); i++) {
            struct extern_proc *e = &kp[i].kp_proc;
            ReapProc p = {};
            p.pid   = e->p_pid;
            p.start = (i64)e->p_starttime.tv_sec*1000000 + e->p_starttime.tv_usec;
            if (e->p_stat==SZOMB || !signalable(p.pid)) {
                continue;
            }
            p.sid = getsid(p.pid);
            if (reapmine(&p, r->self, r->group, 0) && reapstart(p.pid)==p.start) {
                sendsignal(p.pid, sig, 0);
                count++;
            }
        }
    }
    free(kp);
    return count;
}

#else
static void reapsnapshot(Reaper *) {}
static void reapforks(Reaper *) {}
static iz   reapsignal(Reaper *, i32) { return 0; }
#endif

// Kill everything that came from the tool, round after round, until a
// round finds nothing left: processes forked meanwhile are found in the
// next. Then its group, the reaper with it, if the group is its own.
// Never returns.
[[noreturn]] static void reapsweep(Reaper *r)
{
    for (i32 round = 0; round < 50; round++) {
        reapchildren(r);
        if (!reapsignal(r, SIGKILL)) {
            break;
        }
        reapnap(10);
    }
    if (r->group) {
        sendsignal(r->group, SIGKILL, 1);
    }
    _exit(1);
}

// The reaper process: see "Processes" above. Never returns.
[[noreturn]] static void reap(i32 argc, char **argv)
{
    // A session of its own, and so a group: the one it may signal. Should
    // setsid fail, it is still in its parent's group, which it must never
    // signal, so then it kills only the processes it finds of the tool's.
    static Reaper r;  // large, and the reaper has only this one
    r.self  = getpid();
    i32 sid = setsid();
    r.group = reapgroup(r.self, getpgrp(), sid<0 ? getsid(0) : sid);
    if (!r.group) {
        reapsay("cannot lead a session of its own", "");
    }
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    fcntl(3, F_SETFD, FD_CLOEXEC);
    fcntl(4, F_SETFD, FD_CLOEXEC);
    #if defined(__linux__) && defined(SYS_close_range)
    syscall(SYS_close_range, 5u, ~0u, 4u);  // CLOSE_RANGE_CLOEXEC
    #endif
    #ifdef __linux__
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);  // escapees come here
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
    sa.sa_handler = onsweep;
    sa.sa_flags   = 0;
    sigaction(SIGUSR1, &sa, 0);  // before there is anything to sweep

    if (chdir(argv[2])) {
        reapsay("cannot change directory to", argv[2]);
        reapstatus(126, 0);
        _exit(2);
    }

    // macOS: the ids of the tool's processes start from the reaper's
    // own, and the sweep looks for what answers to it, if it was spawned
    // disclaimed (--reap-disclaimed) and that took (reaprespid). A test
    // hook, PROSPERO_TEST_REAP=noorigin, leaves responsibility alone to
    // find what left the tree: no fork watch, and no forking parents.
    r.kq = -1;
    #ifdef __APPLE__
    if (!__builtin_strcmp(argv[1], "--reap-disclaimed")) {
        r.spi = respspi();
    }
    char *hook   = getenv("PROSPERO_TEST_REAP");
    b32   origin = !hook || __builtin_strcmp(hook, "noorigin");
    i32   resp   = r.spi.pidfor ? r.spi.pidfor(r.self) : 0;
    r.uniq  = reapinfo(&r, r.self, 0).uniq;
    r.resp  = reaprespid(r.spi.pidfor!=0, r.self, resp, r.uniq);
    r.idcap = origin ? REAP_IDS : 0;
    r.nids  = reapaddid(r.ids, r.nids, r.idcap, r.uniq);
    r.kq    = origin ? kqueue() : -1;
    #endif

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
    childsignals(&attr, flags, 1);
    pid_t pid = 0;
    i32   err = posix_spawnp(&pid, argv[3], &fa, &attr, argv+3, environ);
    if (err) {
        errno = err;
        reapsay("cannot run", argv[3]);
        reapstatus(err==ENOENT ? 127 : 126, 0);
        _exit(2);
    }

    // Only the tool's tree holds its standard descriptors now, so that
    // its input reads EOF, and its output closes, with the tool alone
    i32 null = open("/dev/null", O_RDWR|O_CLOEXEC);
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);

    r.tool = pid;
    reapsnapshot(&r);  // the tool, watched from now on
    #ifdef __APPLE__
    i64 every = 250000;  // µs between snapshots of the tree
    #else
    i64 every = -1;
    #endif
    i64 due = monotonic() + every;
    for (;;) {
        struct pollfd fds[3] = {};
        fds[0].fd     = 3;
        fds[0].events = POLLIN;
        fds[1].fd     = chld[0];
        fds[1].events = POLLIN;
        fds[2].fd     = r.kq;  // ignored if -1
        fds[2].events = POLLIN;
        i32 ms = -1;
        if (every > 0) {
            i64 left = due - monotonic();
            ms = left>0 ? (i32)((left + 999)/1000) : 0;
        }
        i32 n = poll(fds, 3, ms);
        u8  drain[64];
        while (read(chld[0], drain, sizeof(drain)) > 0) {}
        reapchildren(&r);
        reapforks(&r);
        if (every>0 && monotonic()>=due) {
            reapsnapshot(&r);
            due = monotonic() + every;
        }
        if (reapasked) {
            reapsweep(&r);
        }
        if (n>0 && fds[0].revents) {
            u8 b;
            iz got = read(3, &b, 1);
            if (got==0 || (got<0 && errno!=EINTR && errno!=EAGAIN)) {
                break;  // the harness is gone
            }
        }
    }

    // Ask everything to stop, then sweep
    if (r.group) {
        sendsignal(r.group, SIGTERM, 1);
    }
    reapsignal(&r, SIGTERM);
    for (i32 i = 0; i < 200; i++) {
        reapnap(10);
        reapchildren(&r);
    }
    reapsweep(&r);
}


// Local endpoints: AF_UNIX stream sockets
//
// A host's socket lives in its state directory's run directory, which
// only its owner may enter, and that is the whole of the access control.
// Where that path is too long for a socket address (about 100 bytes),
// the address goes through a link /tmp/prospero-UID-HASH to the run
// directory instead, which must be this user's and point there. A host
// removes its link when it stops listening, and as it starts, any link
// of this user's that leads nowhere, which a host killed, or a client,
// left behind when the state directory went.
//
// Each Conn has a reader thread, posting what arrives, and a writer
// thread, draining the queue, each with a reference, as has the owner
// until os_connclose. Breaking a connection shuts its socket down, which
// ends a blocked read or write at once; the descriptor closes with the
// last reference. A listener's thread polls its socket and a stop event.

enum : iz {
    CONN_BUFLEN   = 1<<16,
    CONN_QUEUEMAX = (iz)64 << 20,  // bytes waiting to be written
    SOCK_PATHMAX  = 100,           // sun_path: 104 bytes on macOS
};

struct ConnChunk {  // queued output, its bytes following
    ConnChunk *next;
    iz         len;
};

struct Conn {
    Lock       lock;
    i32        refs;     // atomic
    i32        fd;
    Mailbox   *mbox;
    u64        id;
    Event      wake;     // for the writer
    ConnChunk *head;     // under lock: the output queue
    ConnChunk *last;     // under lock
    iz         queued;   // under lock: bytes in the queue
    b32        closing;  // under lock: end the output after the queue
    b32        broken;   // under lock: the output failed, or was dropped
    u8         buf[CONN_BUFLEN];
};

struct Listener {
    i32                refs;   // atomic: the owner, the thread
    i32                fd;
    Event              stop;
    Mailbox           *mbox;
    u64                nextid;
    struct sockaddr_un addr;   // as given, through any link
};

static void connrelease(Conn *c)
{
    if (!__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL)) {
        close(c->fd);
        close((i32)c->wake.h[0]);
        close((i32)c->wake.h[1]);
        pthread_mutex_destroy((pthread_mutex_t *)c->lock.opaque);
        free(c);
    }
}

static void connfreequeue(Conn *c)  // under lock
{
    for (ConnChunk *k = c->head; k;) {
        ConnChunk *next = k->next;
        free(k);
        k = next;
    }
    c->head   = 0;
    c->last   = 0;
    c->queued = 0;
}

static void connreader(void *arg)
{
    Conn *c   = (Conn *)arg;
    iz    max = mailmax(c->mbox);
    for (;;) {
        iz n = read(c->fd, c->buf, CONN_BUFLEN);
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            break;
        }
        for (iz off = 0; off < n; off += max) {
            iz len = n-off<max ? n-off : max;
            mailpost(c->mbox, CONN_DATA, c->id, {c->buf+off, len}, 1);
        }
    }
    mailpost(c->mbox, CONN_EOF, c->id, {}, 1);
    connrelease(c);
}

static void connwriter(void *arg)
{
    Conn *c  = (Conn *)arg;
    b32   ok = 1;
    for (;;) {
        os_lock(&c->lock);
        ConnChunk *k = c->head;
        if (k) {
            c->head = k->next;
            c->last = c->head ? c->last : 0;
        }
        b32 done = !k && (c->closing || c->broken);
        ok = !c->broken;
        os_unlock(&c->lock);
        if (done) {
            break;
        } else if (!k) {
            os_wait(&c->wake, -1);
            continue;
        }

        ok = writeall(c->fd, {(u8 *)(k + 1), k->len});
        os_lock(&c->lock);
        c->queued -= k->len;
        if (!ok) {
            c->broken = 1;
            connfreequeue(c);
        }
        os_unlock(&c->lock);
        free(k);
        if (!ok) {
            break;
        }
    }
    if (ok) {
        shutdown(c->fd, SHUT_WR);  // the peer reads the end
    }
    connrelease(c);
}

// A connection on a connected socket, its threads started. Announced, it
// is posted as CONN_NEW first, before any record of its own; should its
// threads then fail to start, its CONN_EOF follows at once, and the
// owner has a connection that is over. Otherwise null on failure, the
// socket closed.
static Conn *newconn(Os *os, i32 fd, Mailbox *mbox, u64 id, b32 announce)
{
    Conn *c = (Conn *)malloc(sizeof(Conn));
    if (!c) {
        close(fd);
        errno = ENOMEM;
        return 0;
    }
    __builtin_memset(c, 0, __builtin_offsetof(Conn, buf));
    if (!os_eventinit(&c->wake)) {
        close(fd);
        free(c);
        return 0;
    }
    os_lockinit(&c->lock);
    c->refs = 3;
    c->fd   = fd;
    c->mbox = mbox;
    c->id   = id;
    if (announce) {
        mailpost(mbox, CONN_NEW, id, {(u8 *)&c, sizeof(c)}, 1);
    }
    b32 reader = os_thread(os, connreader, c);
    b32 writer = reader && os_thread(os, connwriter, c);
    if (writer) {
        return c;
    }
    __atomic_sub_fetch(&c->refs, 2-reader, __ATOMIC_ACQ_REL);  // not started
    os_lock(&c->lock);
    c->broken = 1;
    os_unlock(&c->lock);
    shutdown(fd, SHUT_RDWR);  // a reader that started ends, with CONN_EOF
    if (announce && !reader) {
        mailpost(mbox, CONN_EOF, id, {}, 1);
    }
    if (announce) {
        return c;
    }
    connrelease(c);
    errno = EAGAIN;
    return 0;
}

static b32 os_connwrite(Os *, Conn *c, Str data)
{
    if (data.len > CONN_QUEUEMAX) {
        return 0;
    }
    ConnChunk *k = 0;
    if (data.len) {
        k = (ConnChunk *)malloc(sizeof(ConnChunk) + (uz)data.len);
        if (!k) {
            return 0;
        }
        k->next = 0;
        k->len  = data.len;
        copybytes(k+1, data.data, data.len);
    }

    os_lock(&c->lock);
    b32 ok = !c->closing && !c->broken && data.len<=CONN_QUEUEMAX-c->queued;
    if (ok && k) {
        *(c->last ? &c->last->next : &c->head) = k;
        c->last    = k;
        c->queued += k->len;
        k = 0;
    }
    if (ok && !data.data) {
        c->closing = 1;
    }
    if (ok) {
        os_signal(&c->wake);
    }
    os_unlock(&c->lock);
    free(k);  // unless queued
    return ok;
}

static void os_connclose(Os *, Conn *c)
{
    os_lock(&c->lock);
    c->broken = 1;
    connfreequeue(c);
    os_signal(&c->wake);
    os_unlock(&c->lock);
    shutdown(c->fd, SHUT_RDWR);  // ends the reader, and any write
    connrelease(c);
}

// An address for a socket path, or 0 if it cannot be one.
static b32 sockaddr(struct sockaddr_un *sa, Str path)
{
    *sa = {};
    sa->sun_family = AF_UNIX;
    if (path.len>=(iz)sizeof(sa->sun_path) || hasnul(path)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    copybytes(sa->sun_path, path.data, path.len);
    return 1;
}

static i32 cloexecsocket()
{
    i32 fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        #ifdef SO_NOSIGPIPE
        i32 one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        #endif
    }
    return fd;
}

// The directory exists, is this user's alone, and is no link.
static b32 privatedir(char *path)
{
    if (mkdir(path, 0700) && errno!=EEXIST) {
        return 0;
    }
    struct stat st;
    if (lstat(path, &st)) {
        return 0;
    } else if (!S_ISDIR(st.st_mode) || st.st_uid!=geteuid()) {
        errno = EACCES;
        return 0;
    }
    return !(st.st_mode & 077) || !chmod(path, 0700);
}

// $TMPDIR if it is an absolute path, without trailing slashes, else null.
static Str tmpdirenv()
{
    char *env = getenv("TMPDIR");
    if (!env || env[0]!='/') {
        return {};
    }
    Str dir = {(u8 *)env, 0};
    while (dir.data[dir.len]) dir.len++;
    for (; dir.len>1 && dir[dir.len-1]=='/'; dir.len--) {}
    return dir;
}

static Str os_scratchdir(Os *, Arena *a)
{
    Str base = tmpdirenv();
    base = base.len ? base : Str("/tmp");
    Arena save = *a;
    Buf   b(a, base.len + 32);
    print(&b, base);
    print(&b, base.len>1 ? Str("/prospero-") : Str("prospero-"));
    print(&b, (i64)geteuid());
    Str   dir = finish(&b);
    Arena tmp = *a;
    if (!privatedir(cstr(&tmp, dir))) {
        *a = save;
        return {};
    }
    return dir;
}

static Slice<Str> os_tempdirs(Os *, Arena *a)
{
    Slice<Str> r   = {};
    Str        env = tmpdirenv();
    if (env.len && !(env == "/tmp")) {
        r = push(a, r, clone(a, env));
    }
    return push(a, r, Str("/tmp"));
}

static Str os_endpoint(Os *, Arena *a, Str statedir, Str name)
{
    if (hasnul(statedir) || hasnul(name)) {
        errno = EINVAL;
        return {};
    }
    Arena tmp  = *a;
    char *real = (char *)allocbytes(&tmp, 4096);  // PATH_MAX
    if (!realpath(cstr(&tmp, statedir), real)) {
        return {};
    }
    Str dir = {(u8 *)real, 0};
    while (dir.data[dir.len]) dir.len++;
    Str run = concat(&tmp, dir, "/run");
    if (!privatedir(cstr(&tmp, run))) {
        return {};
    }
    Str file = concat(&tmp, concat(&tmp, Str("/"), name), ".sock");
    if (run.len+file.len <= SOCK_PATHMAX) {
        Str r = clone(a, run);
        return concat(a, r, file);
    }

    // Too long for an address: through a link of this user's
    Buf b(&tmp, 64);
    print(&b, "/tmp/prospero-");
    print(&b, (i64)geteuid());
    putbyte(&b, '-');
    printhex(&b, hash(run), 16);
    Str   link  = finish(&b);
    char *clink = cstr(&tmp, link);
    char *crun  = cstr(&tmp, run);
    struct stat st;
    if (lstat(clink, &st)) {
        if (errno!=ENOENT || (symlink(crun, clink) && errno!=EEXIST)) {
            return {};  // else made meanwhile: check it as any other
        }
    }
    char got[4096];
    iz   len = 0;
    errno = 0;
    if (lstat(clink, &st) || !S_ISLNK(st.st_mode) || st.st_uid!=geteuid() ||
            (len = readlink(clink, got, sizeof(got)))<0 ||
            !(Str{(u8 *)got, len} == run)) {
        errno = errno ? errno : EACCES;
        return {};
    }
    Str r = clone(a, link);
    return concat(a, r, file);
}

// Remove this user's links in /tmp, as os_endpoint names them, that lead
// nowhere. A link outlives its run directory when that is removed with
// no os_unlisten first: its host was killed, or only a client made the
// link. A link that leads somewhere stays, for whoever uses it.
static void sweeplinks(Arena scratch)
{
    i32 fd = open("/tmp", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return;
    }
    Buf b(&scratch, 32);
    print(&b, "prospero-");
    print(&b, (i64)geteuid());
    putbyte(&b, '-');
    Str prefix = finish(&b);
    for (struct dirent *e; (e = readdir(dir));) {
        Str name = {(u8 *)e->d_name, 0};
        while (name.data[name.len]) name.len++;
        if (!startswith(name, prefix) || name.len!=prefix.len+16) {
            continue;
        }
        b32 hex = 1;
        for (iz i = prefix.len; i < name.len; i++) {
            u8 c = name[i];
            hex &= (c>='0' && c<='9') || (c>='a' && c<='f');
        }
        struct stat st;
        if (!hex || fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
                !S_ISLNK(st.st_mode) || st.st_uid!=geteuid()) {
            continue;
        }
        if (fstatat(fd, e->d_name, &st, 0) && (errno==ENOENT || errno==ENOTDIR)) {
            unlinkat(fd, e->d_name, 0);
        }
    }
    closedir(dir);  // also closes fd
}

static void listenrelease(Listener *l)
{
    if (!__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL)) {
        close(l->fd);
        close((i32)l->stop.h[0]);
        close((i32)l->stop.h[1]);
        free(l);
    }
}

static void listenthread(void *arg)
{
    Listener *l  = (Listener *)arg;
    Os        os = {};
    for (;;) {
        struct pollfd fds[2] = {};
        fds[0].fd     = l->fd;
        fds[0].events = POLLIN;
        fds[1].fd     = (i32)l->stop.h[0];
        fds[1].events = POLLIN;
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        } else if (fds[1].revents) {
            break;
        } else if (!fds[0].revents) {
            continue;
        }
        i32 fd = accept(l->fd, 0, 0);
        if (fd < 0) {
            if (errno==EINTR || errno==ECONNABORTED || errno==EAGAIN) {
                continue;
            } else if (errno==EMFILE || errno==ENFILE) {
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, 0);  // until descriptors free up
                continue;
            }
            break;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        #ifdef SO_NOSIGPIPE
        i32 one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        #endif
        newconn(&os, fd, l->mbox, ++l->nextid, 1);
    }
    listenrelease(l);
}

static Listener *os_listen(Os *os, Arena scratch, Str endpoint, Mailbox *mbox)
{
    struct sockaddr_un sa;
    if (!sockaddr(&sa, endpoint)) {
        return 0;
    }
    sweeplinks(scratch);  // a host starting clears what dead ones left
    i32 fd = cloexecsocket();
    if (fd < 0) {
        return 0;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        // A socket left by a process that died refuses: replace it
        i32 probe = errno==EADDRINUSE ? cloexecsocket() : -1;
        b32 stale = probe>=0 && connect(probe, (struct sockaddr *)&sa, sizeof(sa)) &&
                    (errno==ECONNREFUSED || errno==ENOENT);
        if (probe >= 0) {
            close(probe);
        }
        if (!stale || (unlink(sa.sun_path) && errno!=ENOENT) ||
                bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
            i32 save = stale ? errno : EADDRINUSE;
            close(fd);
            errno = save;
            return 0;
        }
    }
    chmod(sa.sun_path, 0600);
    Listener *l = (Listener *)malloc(sizeof(Listener));
    if (listen(fd, 64) || !l || !os_eventinit(&l->stop)) {
        i32 save = l ? errno : ENOMEM;
        unlink(sa.sun_path);
        close(fd);
        free(l);
        errno = save;
        return 0;
    }
    l->refs   = 2;
    l->fd     = fd;
    l->addr   = sa;
    l->mbox   = mbox;
    l->nextid = 0;
    if (!os_thread(os, listenthread, l)) {
        unlink(sa.sun_path);
        l->refs = 1;
        listenrelease(l);
        errno = EAGAIN;
        return 0;
    }
    return l;
}

// The socket file goes first, so that nobody new finds this listener,
// and with it any link in /tmp that led there (os_endpoint).
struct Loopback {
    i32      fd;
    Mailbox *mbox;
    u32      type;
    u64      id;
    i64      by;  // ticks
};

static i64 loopleft(Loopback *l)  // ms
{
    i64 now = monotonic();
    return l->by>now ? (l->by-now+999)/1000 : 0;
}

static void loopthread(void *arg)
{
    Loopback *l      = (Loopback *)arg;
    Str       target = {};
    char      buf[1<<13];
    while (!target.data) {
        struct pollfd p = {l->fd, POLLIN, 0};
        i64 left = loopleft(l);
        if (!left || poll(&p, 1, (int)left)<=0) {
            break;
        }
        i32 c = accept(l->fd, 0, 0);
        if (c < 0) {
            continue;
        }
        iz len = 0;
        while (len < (iz)sizeof(buf)) {
            struct pollfd q = {c, POLLIN, 0};
            if (poll(&q, 1, 5000) <= 0) {
                break;
            }
            ssize_t n = read(c, buf+len, sizeof(buf)-(uz)len);
            if (n <= 0) {
                break;
            }
            len += n;
            if (find(Str{(u8 *)buf, len}, "\r\n\r\n") >= 0) {
                break;
            }
        }
        Str t   = loopbacktarget(Str{(u8 *)buf, len});
        Str ans = t.data ? loopbackpage : loopbackmissing;
        (void)!write(c, ans.data, (uz)ans.len);
        close(c);
        target = t;
    }
    close(l->fd);
    mailpost(l->mbox, l->type, l->id, target.data ? target : Str{}, 1);
    free(l);
}

static b32 os_loopback(Os *os, Mailbox *mbox, u32 type, u64 id, i32 ms, i32 *port)
{
    i32 fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_in sa = {};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t salen    = sizeof(sa);
    Loopback *l        = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 4) ||
            getsockname(fd, (struct sockaddr *)&sa, &salen) ||
            !(l = (Loopback *)malloc(sizeof(Loopback)))) {
        close(fd);
        return 0;
    }
    l->fd   = fd;
    l->mbox = mbox;
    l->type = type;
    l->id   = id;
    l->by   = monotonic() + (i64)ms*1000;
    if (!os_thread(os, loopthread, l)) {
        close(fd);
        free(l);
        return 0;
    }
    *port = ntohs(sa.sin_port);
    return 1;
}

static void os_unlisten(Os *, Listener *l)
{
    struct sockaddr_un sa = l->addr;
    if (sa.sun_path[0]) {
        unlink(sa.sun_path);
        char *path  = sa.sun_path;
        Str   tmp   = "/tmp/prospero-";
        iz    slash = 0;
        for (iz i = 0; path[i]; i++) {
            slash = path[i]=='/' ? i : slash;
        }
        struct stat st;
        if (slash>tmp.len && !__builtin_memcmp(path, tmp.data, (uz)tmp.len)) {
            path[slash] = 0;
            if (!lstat(path, &st) && S_ISLNK(st.st_mode) && st.st_uid==geteuid()) {
                unlink(path);
            }
        }
    }
    os_signal(&l->stop);
    listenrelease(l);
}

static Conn *os_connect(Os *os, Arena, Str endpoint, Mailbox *mbox, u64 id)
{
    struct sockaddr_un sa;
    if (!sockaddr(&sa, endpoint)) {
        return 0;
    }
    i32 fd = cloexecsocket();
    if (fd < 0) {
        return 0;
    }
    while (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
        if (errno != EINTR) {
            i32 save = errno;
            close(fd);
            errno = save;
            return 0;
        }
    }
    return newconn(os, fd, mbox, id, 0);
}

static b32 os_listening(Os *, Arena, Str endpoint)
{
    struct sockaddr_un sa;
    if (!sockaddr(&sa, endpoint)) {
        return 0;
    }
    i32 fd = cloexecsocket();
    b32 ok = fd>=0 && !connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

static i32 os_pid(Os *)
{
    return (i32)getpid();
}

struct QuitWatch {
    Mailbox *mbox;
    u32      type;
};

static void quitthread(void *arg)
{
    QuitWatch *q = (QuitWatch *)arg;
    sigset_t   set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    for (;;) {
        int sig = 0;
        if (!sigwait(&set, &sig)) {
            mailpost(q->mbox, q->type, 0, {}, 1);
        }
    }
}

// The signals are blocked in this thread, and so in every thread that
// it starts from then on, and one thread takes them with sigwait. Child
// processes start with nothing blocked (childsignals).
static b32 os_onquit(Os *os, Mailbox *mb, u32 type)
{
    static QuitWatch watch;
    watch.mbox = mb;
    watch.type = type;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    return !pthread_sigmask(SIG_BLOCK, &set, 0) && os_thread(os, quitthread, &watch);
}

static i32 os_responsible(Os *os, i32 pid)
{
    return os->resp.pidfor && signalable(pid) ? os->resp.pidfor(pid) : 0;
}

// Two forks with a new session between them, so that the program is
// nobody's child here, and outside this terminal's session and process
// group. The children do only what is safe after a fork in a process
// with threads, with everything prepared before it. The middle child
// passes back the grandchild's pid, and the grandchild errno should exec
// fail, through a close-on-exec pipe that closes when exec succeeds.
static Detached os_detach(Os *os, Arena scratch, Slice<Str> argv, Str cwd, Str errpath)
{
    Detached r = {};
    if (!argv.len || hasnul(cwd) || hasnul(errpath)) {
        errno = EINVAL;
        return r;
    }
    for (iz i = 0; i < argv.len; i++) {
        if (hasnul(argv[i])) {
            errno = EINVAL;
            return r;
        }
    }
    char **cargv = alloc<char *>(&scratch, argv.len+1);
    for (iz i = 0; i < argv.len; i++) {
        cargv[i] = cstr(&scratch, argv[i]);
    }
    char *ccwd = cstr(&scratch, cwd);
    char *cerr = cstr(&scratch, errpath);
    i32   p[2];
    if (!cloexecpipe(p)) {
        return r;
    }

    pid_t mid = fork();
    if (mid < 0) {
        i32 save = errno;
        close(p[0]);
        close(p[1]);
        errno = save;
        return r;
    } else if (!mid) {
        setsid();
        pid_t pid = fork();
        if (pid) {
            i32 v[2] = {pid, pid<0 ? errno : 0};
            while (write(p[1], v, sizeof(v))<0 && errno==EINTR) {}
            _exit(0);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, 0);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        i32 null = open("/dev/null", O_RDWR);
        i32 err  = open(cerr, O_WRONLY|O_CREAT|O_APPEND, 0600);
        dup2(null, 0);
        dup2(null, 1);
        dup2(err>=0 ? err : null, 2);
        if (chdir(ccwd)) {
            // It starts where this process is, then
        }
        execv(os->exe, cargv);  // this executable, whatever argv[0] says
        i32 v[2] = {0, errno};
        while (write(p[1], v, sizeof(v))<0 && errno==EINTR) {}
        _exit(127);
    }
    close(p[1]);
    while (waitpid(mid, 0, 0)<0 && errno==EINTR) {}

    // The pid from the middle child, then an exec failure, if any
    i32 v[4] = {};
    iz  got  = 0;
    for (;;) {
        iz n = read(p[0], (u8 *)v+got, sizeof(v)-(uz)got);
        if (n<0 && errno==EINTR) continue;
        if (n <= 0) break;
        got += n;
    }
    close(p[0]);
    if (got<8 || v[0]<=0) {
        errno = got>=8 && v[1] ? v[1] : ECHILD;
        return r;
    } else if (got >= 16) {
        errno = v[3] ? v[3] : ENOEXEC;
        return r;
    }
    r.pid = v[0];
    return r;
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
    EDGE_TRUST,
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
    case EDGE_TRUST:   err = HTTP_ETRUST;   break;
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
    new (h) Http();
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


#if PROSPERO_GUI
#  include "app.cpp"
#  include "gui_sdl.cpp"
#endif

#if PROSPERO_GUI && defined(__APPLE__)
// A window the desktop started takes its login shell's environment
// (loginwanted): $SHELL, else the account's, else zsh, run once with
// PROSPERO_RESOLVING_ENVIRONMENT=1 so a profile can tell. Here, before
// any thread starts, so setenv races nothing. Five seconds at most: a
// profile that waits on a terminal is killed, and the window keeps
// launchd's environment.
static void loginenviron(Os *os, Arena scratch)
{
    if (!loginwanted(os_getenv(os, &scratch, "PATH"))) {
        return;
    }
    Str shell = os_getenv(os, &scratch, "SHELL");
    if (!shell.len) {
        struct passwd *pw = getpwuid(getuid());
        shell = pw && pw->pw_shell ? clone(&scratch, inistr(pw->pw_shell)) : Str{};
    }
    Slice<Str> args = loginargv(&scratch, shell.len ? shell : Str("/bin/zsh"));
    if (!args.len) {
        return;
    }
    char **argv = alloc<char *>(&scratch, args.len+1);
    for (iz i = 0; i < args.len; i++) {
        argv[i] = cstr(&scratch, args[i]);
    }
    iz n = 0;
    for (; environ[n]; n++) {}
    char **envp = alloc<char *>(&scratch, n+2);
    for (iz i = 0; i < n; i++) {
        envp[i] = environ[i];
    }
    envp[n] = (char *)"PROSPERO_RESOLVING_ENVIRONMENT=1";

    i32 out[2];
    i32 null = open("/dev/null", O_RDWR|O_CLOEXEC);
    if (null < 0) {
        return;
    } else if (!cloexecpipe(out)) {
        close(null);
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, null, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, null, 2);
    posix_spawnattr_t attr;
    childsignals(&attr, POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_CLOEXEC_DEFAULT, 1);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    i32   err = posix_spawn(&pid, argv[0], &fa, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(null);
    close(out[1]);
    if (err) {
        close(out[0]);
        return;
    }

    // All of it, or nothing if it runs out of time or room
    iz  cap = (iz)1 << 22;
    u8 *buf = alloc<u8>(&scratch, cap);
    iz  len = 0;
    b32 ok  = 0;
    i64 by  = monotonic() + 5*1000000;
    for (;;) {
        i64 now = monotonic();
        struct pollfd p = {out[0], POLLIN, 0};
        if (now>=by || poll(&p, 1, (int)((by-now+999)/1000))<=0 || len==cap) {
            break;
        }
        ssize_t r = read(out[0], buf+len, (uz)(cap-len));
        if (r < 0 && errno==EINTR) {
            continue;
        } else if (r <= 0) {
            ok = !r;
            break;
        }
        len += r;
    }
    close(out[0]);
    i32 status = 0;
    i32 reaped = 0;
    while (ok && !reaped && monotonic()<by) {
        reaped = (i32)waitpid(pid, &status, WNOHANG);
        reaped = reaped<0 && errno==EINTR ? 0 : reaped;
        if (!reaped) {
            poll(0, 0, 10);
        }
    }
    if (!reaped) {
        sendsignal(pid, SIGKILL, 1);  // its own group, its children too
        sendsignal(pid, SIGKILL, 0);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        return;
    } else if (reaped<0 || !WIFEXITED(status)) {
        return;
    }

    Slice<Str> env = loginenv(&scratch, {buf, len});
    for (iz i = 0; i < env.len; i++) {
        Cut kv = cut(env[i], '=');
        setenv(cstr(&scratch, kv.head), cstr(&scratch, kv.tail), 1);
    }
}
#endif


#if PROSPERO_DESKTOP
// The desktop helper, "prospero desktop" (main_desktop_x11.cpp)
extern "C" int prospero_desktop(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    if (argc>1 && (!__builtin_strcmp(argv[1], "--reap") ||
                   !__builtin_strcmp(argv[1], "--reap-disclaimed"))) {
        reap(argc, argv);
    }
    #if PROSPERO_DESKTOP
    if (argc>1 && !__builtin_strcmp(argv[1], "desktop")) {
        return prospero_desktop(argc-1, argv+1);
    }
    #endif

    // Standard descriptors must exist, or new ones would take their place
    for (i32 fd = open("/dev/null", O_RDWR); fd>=0 && fd<=2;) {
        fd = dup(fd);
    }
    signal(SIGPIPE, SIG_IGN);

    Os os = {};
    i32 life[2];
    if (!cloexecpipe(life)) {
        writeall(2, "prospero: cannot create the lifeline pipe\n");
        return 2;
    }
    os.lifer = life[0];
    os.lifew = life[1];

    // macOS: disclaimed reapers only for a harness that answers for
    // itself, and only while they run its own code (reap.cpp)
    os.resp     = respspi();
    os.respself = os.resp.pidfor ? os.resp.pidfor(getpid()) : 0;

    iz      cap = sizeof(void *)==8 ? (iz)1<<34 : (iz)1<<28;
    Region *r   = os_reserve(&os, cap);
    if (!r) {
        writeall(2, "prospero: out of memory\n");
        return 2;
    }
    Arena a = newarena(r);

    #ifdef __linux__
    os.exe = (char *)"/proc/self/exe";  // this binary, even if replaced
    #else
    Str exe = os_exepath(&os, &a);
    os.exe = exe.len ? cstr(&a, exe) : argv[0];
    struct stat st;
    if (exe.len && !stat(os.exe, &st)) {
        os.exeid = fileid(&st);  // what reapers run, while it stays so
    }
    #endif

    Str *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i].data = (u8 *)argv[i];
        while (args[i].data[args[i].len]) args[i].len++;
    }
    #if PROSPERO_GUI
    if (guiwanted(args, argc)) {
        #ifdef __APPLE__
        loginenviron(&os, a);
        #endif
        // For sigthread (gui_sdl.cpp) alone: every thread inherits this
        sigset_t quit;
        sigemptyset(&quit);
        sigaddset(&quit, SIGINT);
        sigaddset(&quit, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &quit, 0);
        return gui_main(&os, a, args, argc);
    }
    #endif
    return app_main(&os, a, args, argc);
}
