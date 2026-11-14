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
#include <pwd.h>  // getpwuid
#include <signal.h>
#include <spawn.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <string.h>  // strerror_r
#include <sys/file.h>  // flock
#include <sys/ioctl.h>  // TIOCSCTTY, TIOCSWINSZ
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <dlfcn.h>       // dlsym, for the responsibility functions
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
    Slice<Str> launchenv;  // macOS: launchd's, if loginenviron replaced it
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

static i64 os_utcoffset(Os *, i64 ms)
{
    time_t    t  = (time_t)(ms>=0 ? ms/1000 : (ms - 999)/1000);
    struct tm tm = {};
    return localtime_r(&t, &tm) ? (i64)tm.tm_gmtoff : 0;
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

static Slice<Str> os_launchenv(Os *os, Arena *a)
{
    if (!os->launchenv.len) {
        return os_environ(os, a);
    }
    Slice<Str> r = {};
    for (iz i = 0; i < os->launchenv.len; i++) {
        r = push(a, r, clone(a, os->launchenv[i]));
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

static Str os_hostids(Os *, Arena *a)
{
#ifdef __linux__
    Buf b(a, 32);
    print(&b, (i64)getuid());
    putbyte(&b, ':');
    print(&b, (i64)getgid());
    return finish(&b);
#else
    (void)a;
    return {};
#endif
}

static b32 os_linkfar(Os *, Arena, Str) { return 0; }

static b32 os_haslink(Os *, Arena scratch, Str path, iz from)
{
    if (hasnul(path) || from<0) {
        return 0;
    }
    char *c = cstr(&scratch, path);
    for (iz end = from; end <= path.len; end++) {
        if ((end<path.len && path[end]!='/') || end==from || path[end-1]=='/') {
            continue;
        }
        char        save = c[end];
        struct stat st;
        c[end] = 0;
        int r  = lstat(c, &st);
        c[end] = save;
        if (r) {
            return 0;  // missing: nothing past it is reached
        } else if (S_ISLNK(st.st_mode)) {
            return 1;
        }
    }
    return 0;
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
// os_letgo asks the reaper, with SIGUSR2, to end without killing
// anything, once its tool's status is out: what the tool left running,
// a daemon it started such as podman start's conmon, goes on as an
// orphan of the system's, no longer one that the reaper would end with
// the harness. A sweep asked for as well wins.
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
// A terminal's tool (os_ptyspawn) runs under "prospero --reap-tty",
// with the terminal's slave on fds 0-2. That reaper forks the tool into
// a session of its own, with the slave as its controlling terminal, so
// that closing the master, the hang-up, sends it SIGHUP; its sweep
// takes the tool's session too, while the tool lives. At lifeline EOF
// it only ends: the master closed with the harness, which is the
// hang-up. The harness side has no output pipe: a reader thread fills a
// ring from the non-blocking master, and the writer waits on it for
// room. The master closes once the caller, the reader and the writer
// have all let go of it.
//
// Every signal goes out through sendsignal, which refuses a pid or a
// group id that would reach more than meant (reap.cpp). A reaper signals
// its own group only if setsid made it the group's leader.

// The reaper's first argument: for a tool or a terminal's tool, each
// spawned disclaimed or not (macOS).
static char const *reapmodes[] = {
    "--reap", "--reap-disclaimed", "--reap-tty", "--reap-tty-disclaimed",
};

static i32 reapmode(char const *arg)  // 1 + its index, or 0 if none
{
    for (i32 i = 0; i < (i32)countof(reapmodes); i++) {
        if (!__builtin_strcmp(arg, reapmodes[i])) {
            return 1 + i;
        }
    }
    return 0;
}

static b32 reapdisclaimed(char const *arg) { return reapmode(arg) % 2 == 0; }
static b32 reaptty(char const *arg)        { return reapmode(arg) > 2; }

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
    // os_ptyspawn: the master, the reader's ring
    b32        pty;
    i32        master;    // closed when the last of mrefs is dropped
    i32        mrefs;     // atomic: the caller, the reader, the writer
    Event      stop;      // for the reader: a hang-up, room, an exit
    b32        hangup;    // under lock: os_procfree closed the terminal
    b32        exited;    // under lock: the child's status is in
    b32        full;      // under lock: the reader waits for room
    b32        told;      // under lock: PROC_READY sent, not re-armed
    u8        *ring;
    iz         ringcap;
    i64        rpos;      // under lock: bytes taken
    i64        wpos;      // under lock: bytes read into the ring
    u8         tail[PROC_TAILMAX];
    u8         msg[8+PROC_TAILMAX];
    u8         buf[PROC_BUFLEN];
};

static void procrelease(Proc *p)
{
    if (!__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) {
        if (p->pty) {
            close((i32)p->stop.h[0]);
            close((i32)p->stop.h[1]);
            free(p->ring);
        }
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

// A holder of a terminal's master is done with it, and the last closes
// it: a hang-up, should anything still hold the terminal.
static void ptydrop(Proc *p)
{
    if (!__atomic_sub_fetch(&p->mrefs, 1, __ATOMIC_ACQ_REL)) {
        close(p->master);
    }
}

// The reader of a terminal: output into the ring while it has room,
// PROC_READY when none was waiting to be told of. It ends at EOF (EIO
// on Linux), when nothing holds the terminal; at a hang-up; or 200 ms
// after the child exited, from the last output, should what it left
// behind still hold the terminal. A full ring never times out.
static void ptyreader(void *arg)
{
    Proc *p     = (Proc *)arg;
    i64   quiet = 0;  // the child exited: the end of the wait for EOF
    for (;;) {
        os_lock(&p->lock);
        b32 hangup = p->hangup;
        b32 exited = p->exited;
        i64 wpos   = p->wpos;
        iz  room   = p->ringcap - (iz)(p->wpos - p->rpos);
        p->full    = !room;
        os_unlock(&p->lock);
        i64 now = monotonic();
        quiet = exited && !quiet ? now+200000 : quiet;
        if (hangup || (room && quiet && now>=quiet)) {
            break;
        }

        struct pollfd fds[2] = {};
        fds[0].fd     = room ? p->master : -1;
        fds[0].events = POLLIN;
        fds[1].fd     = (i32)p->stop.h[0];
        fds[1].events = POLLIN;
        i32 ms = room && quiet ? (i32)((quiet - now + 999)/1000) : -1;
        if (poll(fds, 2, ms) < 0) {
            continue;  // EINTR
        }
        if (fds[1].revents) {
            u8 drain[64];
            while (read((i32)p->stop.h[0], drain, sizeof(drain)) > 0) {}
        }
        if (!fds[0].revents) {
            continue;
        }

        iz at  = (iz)(wpos % p->ringcap);
        iz len = room<p->ringcap-at ? room : p->ringcap-at;
        iz got = read(p->master, p->ring+at, (uz)len);
        if (got<0 && (errno==EINTR || errno==EAGAIN)) {
            continue;
        } else if (got <= 0) {
            break;  // nothing holds the terminal
        }
        p->total += got;
        quiet = quiet ? monotonic()+200000 : 0;
        os_lock(&p->lock);
        p->wpos += got;
        b32 tell = !p->told;
        p->told  = 1;
        os_unlock(&p->lock);
        if (tell) {
            u8 msg[8];
            put64(msg, p->total);
            mailpost(p->mbox, PROC_READY, p->id, {msg, 8}, 1);
        }
    }
    u8 msg[8];
    put64(msg, p->total);
    mailpost(p->mbox, PROC_EOF, p->id, {msg, 8}, 1);
    ptydrop(p);
    procrelease(p);
}

// Write to a terminal's master, which is non-blocking, waiting for room
// on the writer's event too: 0 once it broke, or was hung up.
static b32 ptywrite(Proc *p, Str data)
{
    while (data.len) {
        iz n = write(p->master, data.data, (uz)data.len);
        if (n > 0) {
            data = cuthead(data, n);
            continue;
        } else if (n<0 && errno==EINTR) {
            continue;
        } else if (n==0 || errno!=EAGAIN) {
            return 0;
        }
        struct pollfd fds[2] = {};
        fds[0].fd     = p->master;
        fds[0].events = POLLOUT;
        fds[1].fd     = (i32)p->wake.h[0];
        fds[1].events = POLLIN;
        poll(fds, 2, -1);
        if (fds[1].revents) {
            u8 drain[64];
            while (read((i32)p->wake.h[0], drain, sizeof(drain)) > 0) {}
        }
        os_lock(&p->lock);
        b32 hangup = p->hangup;
        os_unlock(&p->lock);
        if (hangup) {
            return 0;
        }
    }
    return 1;
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

        Str data = {(u8 *)(c + 1), c->len};
        b32 ok   = p->pty ? ptywrite(p, data) : writeall(p->infd, data);
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
    if (p->pty) {
        ptydrop(p);
    } else {
        close(p->infd);
    }
    close((i32)p->wake.h[0]);
    close((i32)p->wake.h[1]);
    procrelease(p);
}

// The child's status is in: a terminal's reader stops waiting for EOF
static void ptyexited(Proc *p)
{
    if (p->pty) {
        os_lock(&p->lock);
        p->exited = 1;
        os_unlock(&p->lock);
        os_signal(&p->stop);
    }
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
        ptyexited(p);
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
        ptyexited(p);
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

// Close a terminal from the caller's side, dropping input not yet
// written: its reader and writer stop, and the last hold on the master
// closes it. Also drops extra holds, those of threads that never ran.
static void ptyhangup(Proc *p, i32 extra)
{
    os_lock(&p->lock);
    if (!p->closing && !p->broken) {
        os_signal(&p->wake);
    }
    p->closing = 1;
    p->hangup  = 1;
    freequeue(p);
    os_unlock(&p->lock);
    os_signal(&p->stop);
    for (i32 i = 0; i < extra; i++) {
        ptydrop(p);
    }
    ptydrop(p);  // the caller's
}

// A terminal: its master, non-blocking, and its slave, to become the
// child's, both close-on-exec, the slave not this process's controlling
// terminal. UTF-8 input, DEL erasing, and the size given. Returns 0 on
// failure, with errno, closing what it opened.
struct Pty {
    i32 master;
    i32 slave;
};

static unsigned short ptydim(i32 v, i32 lo)
{
    return (unsigned short)(v<lo ? lo : v>0xffff ? 0xffff : v);
}

static b32 ptysize(i32 master, PtySize z)
{
    struct winsize ws = {};
    ws.ws_row    = ptydim(z.rows, 1);
    ws.ws_col    = ptydim(z.cols, 1);
    ws.ws_xpixel = ptydim(z.xpx, 0);
    ws.ws_ypixel = ptydim(z.ypx, 0);
    return !ioctl(master, TIOCSWINSZ, &ws);
}

static Pty openpty(PtySize z)
{
    Pty  r    = {-1, -1};
    char name[128];
    #ifdef __linux__
    r.master = posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    #else
    r.master = posix_openpt(O_RDWR|O_NOCTTY);
    if (r.master >= 0) {
        fcntl(r.master, F_SETFD, FD_CLOEXEC);
    }
    #endif
    b32 ok = r.master>=0 && !grantpt(r.master) && !unlockpt(r.master) &&
             !ptsname_r(r.master, name, sizeof(name));
    if (ok) {
        r.slave = open(name, O_RDWR|O_NOCTTY|O_CLOEXEC);
        ok = r.slave >= 0;
    }
    struct termios tio;
    if (ok && !tcgetattr(r.slave, &tio)) {
        tio.c_iflag |= IUTF8;
        tio.c_cc[VERASE] = 0x7f;
        tcsetattr(r.slave, TCSANOW, &tio);
    }
    ok = ok && ptysize(r.master, z) &&
         !fcntl(r.master, F_SETFL, fcntl(r.master, F_GETFL) | O_NONBLOCK);
    if (!ok) {
        i32 save = errno;
        i32 fds[] = {r.master, r.slave};
        closeall(fds, countof(fds));
        errno = save;
        r.master = r.slave = -1;
    }
    return r;
}

enum : i32 {
    SPAWN_FILE,  // os_spawn: null input, output into a file
    SPAWN_IO,    // os_spawnio: an input pipe, output as records
    SPAWN_PTY,   // os_ptyspawn: a terminal, output into a ring
};

// Spawn under a reaper. With SPAWN_IO, input is a pipe from the writer
// thread, errors go to outpath (or /dev/null) directly, and the reader
// passes output on as records instead of into the file. With SPAWN_PTY
// the terminal's slave is all three, the writer types on its master,
// and the reaper runs the tool in a session of its own. On macOS the
// reaper is spawned disclaimed, its tool's responsible process, only
// while that changes nothing privacy grants answer to (reapdisclaim).
static Proc *spawnproc(Os *os, Spawn *sp, Arena scratch, i32 mode, PtySize z, iz ring)
{
    b32 io  = mode == SPAWN_IO;
    b32 pty = mode == SPAWN_PTY;
    if (!sp->argv.len || hasnul(sp->cwd) || hasnul(sp->outpath) ||
            (pty && ring<=0)) {
        errno = EINVAL;
        return 0;
    }

    // Child-side descriptors: input (null, a pipe's read end, or the
    // slave), output write end (or the slave), errors (the same, the
    // file, or the slave), status write end
    i32   in[2]  = {-1, -1};
    i32   out[2] = {-1, -1};
    i32   st[2]  = {-1, -1};
    Event wake   = {{-1, -1}};
    Event stop   = {{-1, -1}};
    Pty   tty    = {-1, -1};
    i32   null   = open("/dev/null", O_RDWR|O_CLOEXEC);
    i32   file   = -1;
    b32   ok     = null >= 0;
    if (ok && mode==SPAWN_FILE) {
        file = open(cstr(&scratch, sp->outpath),
                    O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
        ok   = file >= 0;
    } else if (ok && io && sp->outpath.len) {
        file = open(cstr(&scratch, sp->outpath),
                    O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
        ok   = file >= 0;
    }
    if (ok && pty) {
        tty = openpty(z);
        ok  = tty.master>=0 && cloexecpipe(st) && os_eventinit(&wake) &&
              os_eventinit(&stop);
    } else if (ok) {
        ok = cloexecpipe(out) && cloexecpipe(st) &&
             (!io || (cloexecpipe(in) && os_eventinit(&wake)));
    }
    u8 *buf = ok && pty ? (u8 *)malloc((uz)ring) : 0;
    ok = ok && (!pty || buf);
    if (!ok) {
        i32 save = errno ? errno : ENOMEM;
        i32 fds[] = {null, file, in[0], in[1], out[0], out[1], st[0], st[1],
                     (i32)wake.h[0], (i32)wake.h[1], (i32)stop.h[0],
                     (i32)stop.h[1], tty.master, tty.slave};
        closeall(fds, countof(fds));
        free(buf);
        errno = save;
        return 0;
    }
    null   = highfd(null);
    st[1]  = highfd(st[1]);
    if (pty) {
        tty.slave = highfd(tty.slave);
        out[1]    = tty.slave;
    } else {
        out[1] = highfd(out[1]);
    }
    if (io) {
        in[0] = highfd(in[0]);
        file  = highfd(file);
    }
    i32 fd0 = pty ? tty.slave : io ? in[0] : null;
    i32 fd2 = pty ? tty.slave : !io ? out[1] : sp->outpath.len ? file : null;
    b32 disclaim = os->resp.disclaim &&
                   reapdisclaim(getpid(), os->respself, sameexe(os));

    iz     argc = 3 + sp->argv.len;
    char **argv = alloc<char *>(&scratch, argc+1);
    argv[0] = os->exe;
    argv[1] = (char *)reapmodes[2*pty + disclaim];
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
    i32 mine[] = {file, out[0], st[0], in[1], (i32)wake.h[0], (i32)wake.h[1],
                  (i32)stop.h[0], (i32)stop.h[1], tty.master};
    if (err) {
        closeall(mine, countof(mine));
        free(buf);
        errno = err;
        return 0;
    }

    Proc *p = (Proc *)malloc(sizeof(Proc));
    if (!p) {
        spawnkill(pid, 0);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        closeall(mine, countof(mine));
        free(buf);
        errno = ENOMEM;
        return 0;
    }
    __builtin_memset(p, 0, __builtin_offsetof(Proc, tail));
    os_lockinit(&p->lock);
    p->refs     = io||pty ? 4 : 3;
    p->pid      = pid;
    p->mbox     = sp->mbox;
    p->id       = sp->id;
    p->outfd    = out[0];
    p->statfd   = st[0];
    p->filefd   = file;
    p->maxbytes = sp->maxbytes;
    p->io       = io || pty;
    p->infd     = in[1];
    p->wake     = wake;
    p->pty      = pty;
    p->master   = tty.master;
    p->mrefs    = 3;
    p->stop     = stop;
    p->ring     = buf;
    p->ringcap  = ring;

    void (*reader)(void *) = pty ? ptyreader : io ? procpipe : procreader;
    if (!os_thread(os, reader, p)) {
        spawnkill(pid, 0);
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        closeall(mine, countof(mine));
        free(buf);
        pthread_mutex_destroy((pthread_mutex_t *)p->lock.opaque);
        free(p);
        errno = EAGAIN;
        return 0;
    }
    i32 input[] = {in[1], (i32)wake.h[0], (i32)wake.h[1]};
    if (!os_thread(os, procwaiter, p)) {
        // The reader finishes once the group is dead, or at the hang-up
        spawnkill(pid, 0);  // no waiter
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        close(st[0]);
        if (pty) {
            ptyhangup(p, 1);  // the caller's hold, and the writer's
        }
        closeall(input, countof(input));
        if (p->io) procrelease(p);  // the writer's
        procrelease(p);  // the waiter's
        procrelease(p);  // the caller's
        errno = EAGAIN;
        return 0;
    }
    if (p->io && !os_thread(os, procwriter, p)) {
        spawnkill(pid, p);  // the reader and waiter then finish
        if (pty) {
            ptyhangup(p, 1);  // the caller's hold, and the writer's
        }
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
    return spawnproc(os, sp, scratch, SPAWN_FILE, {}, 0);
}

static Proc *os_spawnio(Os *os, Spawn *sp, Arena scratch)
{
    return spawnproc(os, sp, scratch, SPAWN_IO, {}, 0);
}

static Proc *os_ptyspawn(Os *os, Spawn *sp, PtySize z, iz ring, Arena scratch)
{
    return spawnproc(os, sp, scratch, SPAWN_PTY, z, ring);
}

// A grandchild, so nothing waits for it and no reaper sweeps it: the
// child starts a session, forks it, and exits at once. Between fork and
// exec only async-signal-safe calls, everything prepared ahead. cmdline
// and window are Windows' only.
static b32 os_launch(Os *, Arena scratch, Slice<Str> argv, Str cwd, Slice<Str> env, Str, b32)
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
    } else if (p->pty && !data.data) {
        return 1;  // a terminal's input closes only with a hang-up
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

// Under the lock, as os_kill, so that the pid is still the reaper's
static void os_letgo(Os *, Proc *p)
{
    os_lock(&p->lock);
    if (reapkill(p->pid, p->reaped, 1) != REAPKILL_NONE) {
        sendsignal(p->pid, SIGUSR2, 0);
        sendsignal(p->pid, SIGCONT, 0);  // should something have stopped it
    }
    os_unlock(&p->lock);
}

static void os_procfree(Os *os, Proc *p)
{
    if (p->pty) {
        ptyhangup(p, 0);
    } else if (p->io) {
        os_procwrite(os, p, {});  // close the input after the queue
    }
    procrelease(p);
}

static Str os_ptypeek(Os *, Proc *p, iz off)
{
    os_lock(&p->lock);
    iz avail = (iz)(p->wpos - p->rpos);
    assert(off>=0 && off<=avail);
    avail -= off;
    iz at    = (iz)((p->rpos + off) % p->ringcap);
    iz len   = avail<p->ringcap-at ? avail : p->ringcap-at;
    p->told  = p->told && avail;
    os_unlock(&p->lock);
    return {p->ring+at, len};
}

static void os_ptytake(Os *, Proc *p, iz len)
{
    os_lock(&p->lock);
    assert(len>=0 && len<=p->wpos-p->rpos);
    p->rpos += len;
    b32 full = p->full;
    os_unlock(&p->lock);
    if (full && len) {
        os_signal(&p->stop);
    }
}

static b32 os_ptyresize(Os *, Proc *p, PtySize z)
{
    return ptysize(p->master, z);
}

// The foreground group is the shell's own while it waits at a prompt,
// and the shell leads the session, so its pid is the session's id
static b32 os_ptyable(Os *)
{
    return 1;
}

// The master reports the terminal's modes, which the slave's side set
static i32 os_ptymode(Os *, Proc *p)
{
    struct termios tio = {};
    if (tcgetattr(p->master, &tio)) {
        return PTY_UNKNOWN;
    } else if (!(tio.c_lflag & ICANON)) {
        return PTY_RAW;
    }
    return tio.c_lflag&ECHO ? PTY_ECHO : PTY_SECRET;
}

static b32 os_ptybusy(Os *, Proc *p)
{
    pid_t fg  = tcgetpgrp(p->master);
    pid_t sid = tcgetsid(p->master);
    return fg>0 && sid>0 && fg!=sid;
}

// Reaper only: the self-pipe's write end, for SIGCHLD, SIGUSR1 and
// SIGUSR2, whether a sweep was asked for, and whether to end without
// one (os_letgo). The reaper is single-threaded.
static i32          reapchld = -1;
static volatile i32 reapasked;
static volatile i32 reapletgo;

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

static void onletgo(int sig)
{
    reapletgo = 1;
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
    i32      toolsid;   // a terminal's tool, its session while unreaped, or 0
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
        }
        if (pid == r->tool) {
            r->toolsid = 0;  // its pid, and so its session's id, is free
        }
        if (pid==r->tool && !r->reported) {
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
        if (reapmine(&procs[i], r->self, r->group, r->toolsid, mine[i])) {
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
                reapmine(p, r->self, r->group, r->toolsid, 1)) {
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
        if (mine[i] && reapmine(p, r->self, r->group, r->toolsid, 1) &&
                reapstart(p->pid)==p->start) {
            sendsignal(p->pid, sig, 0);
            count++;
            if (r->nseen < REAP_SEEN) {
                r->seen[r->nseen++] = *p;
                reapwatch(r, p);
            }
        }
    }

    if (!r->group && !r->toolsid) {
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
            if (reapmine(&p, r->self, r->group, r->toolsid, 0) && reapstart(p.pid)==p.start) {
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

// A terminal's tool, forked rather than spawned, since posix_spawn can
// neither start a session nor take a controlling terminal: it leads a
// session of its own, the terminal on its standard descriptors its
// controlling one, with every signal at its default and none blocked.
// Returns its pid, or -1 with errno, the child's own if it could not
// start, which it tells through a pipe that its exec closes.
static i32 reapfork(char **argv)
{
    i32 errp[2];
    if (!cloexecpipe(errp)) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        i32 save = errno;
        close(errp[0]);
        close(errp[1]);
        errno = save;
        return -1;
    } else if (!pid) {
        i32 sigs[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD, SIGUSR1, SIGUSR2};
        for (iz i = 0; i < countof(sigs); i++) {
            signal(sigs[i], SIG_DFL);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, 0);
        i32 err = 0;
        if (setsid()<0 || ioctl(0, TIOCSCTTY, 0)<0) {
            err = errno;
        } else {
            execvp(argv[0], argv);
            err = errno;
        }
        while (write(errp[1], &err, sizeof(err))<0 && errno==EINTR) {}
        _exit(127);
    }
    close(errp[1]);
    i32 err = 0;
    iz  n   = 0;
    while ((n = read(errp[0], &err, sizeof(err)))<0 && errno==EINTR) {}
    close(errp[0]);
    if (n == (iz)sizeof(err)) {
        while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
        errno = err;
        return -1;
    }
    return pid;
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
    sa.sa_handler = onletgo;
    sigaction(SIGUSR2, &sa, 0);

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
    if (reapdisclaimed(argv[1])) {
        r.spi = respspi();
    }
    char *hook   = getenv("PROSPERO_TEST_REAP");
    b32   origin = !hook || __builtin_strcmp(hook, "noorigin");
    i32   resp   = r.spi.pidfor ? r.spi.pidfor(r.self) : 0;
    r.uniq  = reapinfo(&r, r.self, 0).uniq;
    r.resp  = reaprespid(r.spi.pidfor!=0, r.self, resp, r.uniq);
    r.idcap = origin ? (iz)REAP_IDS : 0;
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
    b32   tty = reaptty(argv[1]);
    pid_t pid = 0;
    i32   err = 0;
    if (tty) {
        pid = reapfork(argv+3);
        err = pid<0 ? errno : 0;
    } else {
        err = posix_spawnp(&pid, argv[3], &fa, &attr, argv+3, environ);
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
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

    r.tool    = pid;
    r.toolsid = tty ? pid : 0;
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
        if (reapletgo && r.reported) {
            _exit(0);  // its orphans go to the system's reaper
        }
        if (n>0 && fds[0].revents) {
            u8 b;
            iz got = read(3, &b, 1);
            if (got==0 || (got<0 && errno!=EINTR && errno!=EAGAIN)) {
                if (tty) {
                    // Its terminal closed with the harness: a hang-up
                    _exit(0);
                }
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
    #ifdef __APPLE__
    // macOS can lose the wakeup of a read blocked on a local stream
    // socket when the peer shuts down its side: the read sleeps on with
    // the end already there, and any later read returns it at once. Seen
    // a few times in 10,000 exchanges of a megabyte and its end under
    // load, in C apart from Prospero. With a timeout the reader looks
    // again, so a lost end costs half a second, not the connection.
    struct timeval tv = {0, 500000};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    #endif
    for (;;) {
        iz n = read(c->fd, c->buf, CONN_BUFLEN);
        if (n<0 && (errno==EINTR || errno==EAGAIN)) {
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
    iz       pathlen;
    u8       path[256];
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
        Str t   = loopbacktarget(Str{(u8 *)buf, len}, Str{l->path, l->pathlen});
        Str ans = t.data ? loopbackpage : loopbackmissing;
        (void)!write(c, ans.data, (uz)ans.len);
        close(c);
        target = t;
    }
    close(l->fd);
    mailpost(l->mbox, l->type, l->id, target.data ? target : Str{}, 1);
    free(l);
}

static b32 os_loopback(Os *os, Mailbox *mbox, u32 type, u64 id, i32 ms, Str path, i32 *port)
{
    if (path.len > (iz)sizeof(Loopback::path)) {
        return 0;
    }
    i32 fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (*port) {
        int one = 1;  // the last sign-in's connections may linger there
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    struct sockaddr_in sa = {};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons((uint16_t)*port);
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
    l->pathlen = path.len;
    copybytes(l->path, path.data, path.len);
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

static b32 os_pidalive(Os *, i32 pid)
{
    // Signal 0 sends nothing; never 0 or less, which name groups
    return pid>0 && (!kill(pid, 0) || errno==EPERM);
}

static b32 os_fromterminal(Os *)
{
    return isatty(2);
}

static Str os_hostname(Os *, Arena *a)
{
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)-1)) {
        return {};
    }
    return clone(a, Str((u8 *)buf, (iz)strlen(buf)));
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
// launchd's environment. That stays, in perm, for os_launchenv.
static void loginenviron(Os *os, Arena *perm)
{
    Slice<Str> launch  = os_environ(os, perm);
    Arena      scratch = *perm;
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
    os->launchenv = env.len ? launch : Slice<Str>{};
    for (iz i = 0; i < env.len; i++) {
        Cut kv = cut(env[i], '=');
        setenv(cstr(&scratch, kv.head), cstr(&scratch, kv.tail), 1);
    }
}
#endif


// prospero --tty-edit: an editor on a file in a terminal, as term.open's
// edit starts it (ptyhost.cpp), with the request in PROSPERO_EDIT_*.
// Phase one becomes the user's login shell, interactive as a terminal's
// is, which runs phase two, "--tty-edit-in", by this program's full
// path; phase two picks the editor by the PATH, $VISUAL and $EDITOR the
// profile left, and becomes it, through sh -c. A chain of execs: the
// editor ends as the terminal's own process, its exit the terminal's,
// and with no shell above it, a Ctrl+Z never strands it stopped.

static Str edstr(char const *s)
{
    Str r = {(u8 *)s, 0};
    for (; s && s[r.len]; r.len++) {}
    return r;
}

static Str edenv(char const *name)
{
    return edstr(getenv(name));
}

[[noreturn]] static void edfail(Str why)
{
    writeall(2, "prospero: ");
    writeall(2, why);
    writeall(2, "\n");
    _exit(127);
}

static Arena edarena(Os *os)
{
    Region *r = os_reserve(os, (iz)1<<28);
    if (!r) {
        edfail("out of memory");
    }
    return newarena(r);
}

struct EdFind {
    Os *os;
    Str path;
    Str home;
};

// Whether the shell's PATH has an editor's program, executable, and
// whether it is busybox's or toybox's, whose vi takes no +N.
static i32 edfound(void *ctx, Arena *a, Str prog)
{
    EdFind *f    = (EdFind *)ctx;
    Arena   perm = *a;
    if (cfgtilde(prog)) {
        prog = concat(&perm, f->home, cuthead(prog, 1));
    }
    Arena tmp = carve(&perm, 1<<20);
    Str   exe = findprog(f->os, &perm, tmp, f->path, prog, {}, 0);
    if (!exe.len || hasnul(exe) || access(cstr(&perm, exe), X_OK)) {
        return EDITOR_MISSING;
    }
    Str real = os_realpath(f->os, &perm, exe);
    Str name = basename(real.len ? real : exe);
    b32 box  = startswith(name, "busybox") || startswith(name, "toybox");
    return box ? EDITOR_BOX : EDITOR_FOUND;
}

// Phase two: the editor, in place of this process.
[[noreturn]] static void ttyeditin()
{
    Os    os = {};
    Arena a  = edarena(&os);
    Str   file = clone(&a, edenv("PROSPERO_EDIT_FILE"));
    if (!file.len) {
        edfail("no file to edit");
    }
    Int    line = parseint(edenv("PROSPERO_EDIT_LINE"));
    Int    col  = parseint(edenv("PROSPERO_EDIT_COL"));
    i32    ln   = line.ok && line.val>0 && line.val<=1000000000 ? (i32)line.val : 0;
    i32    cn   = col.ok && col.val>0 && col.val<=1000000000 ? (i32)col.val : 0;
    // sh takes an empty or relative PATH entry as one in the current
    // folder, the session's, where the agent writes files: the editor
    // is looked for, and run, by the full-path entries alone, so that
    // what runs is what was found
    Str path = edenv("PATH");
    Buf pb(&a, path.len+1);
    for (Str rest = path; rest.len;) {
        Cut c = cut(rest, ':');
        rest = c.tail;
        if (c.head.len && c.head[0]=='/') {
            if (pb.len) {
                putbyte(&pb, ':');
            }
            print(&pb, c.head);
        }
    }
    Str clean = finish(&pb);
    if (clean.len) {
        setenv("PATH", cstr(&a, clean), 1);
    } else {
        unsetenv("PATH");  // sh's own default
    }
    EdFind f    = {&os, clean, edenv("HOME")};
    EditPick pick = editpick(&a, edenv("PROSPERO_EDIT_EDITOR"), edenv("PROSPERO_EDIT_TEMPLATE"),
                             edenv("VISUAL"), edenv("EDITOR"), edfound, &f);
    if (!pick.cmd.len && !pick.tmpl.len) {
        edfail("no editor here: set $VISUAL or $EDITOR, or install vi");
    }
    EditRun run = editorrun(&a, pick.cmd, pick.tmpl, file, ln, cn, pick.nojump);
    if (run.why.len) {
        edfail(run.why);
    }
    char **av = alloc<char *>(&a, run.args.len+5);
    av[0] = (char *)"sh";
    av[1] = (char *)"-c";
    av[2] = cstr(&a, run.script);
    av[3] = cstr(&a, run.name);
    for (iz i = 0; i < run.args.len; i++) {
        av[4+i] = cstr(&a, run.args[i]);
    }

    // The tab's name, OSC 1, then none of the request for the editor
    Buf b(&a, 16 + run.name.len);
    print(&b, "\x1b]1;");
    for (iz i = 0; i < run.name.len; i++) {
        u8 c = run.name[i];
        if (c>=0x20 && c!=0x7f) {
            putbyte(&b, c);
        }
    }
    putbyte(&b, 7);
    writeall(1, finish(&b));
    char const *names[] = {
        "PROSPERO_EDIT_FILE", "PROSPERO_EDIT_LINE", "PROSPERO_EDIT_COL",
        "PROSPERO_EDIT_EDITOR", "PROSPERO_EDIT_TEMPLATE", "PROSPERO_EDIT_LOGIN",
    };
    for (iz i = 0; i < countof(names); i++) {
        unsetenv(names[i]);
    }
    execve("/bin/sh", av, environ);
    edfail(concat(&a, "cannot run /bin/sh: ", os_lasterr(&os, &a)));
}

// The login shell: $SHELL if a full path, else the account's, else
// /bin/sh.
static Str edshell(Arena *a)
{
    Str shell = edenv("SHELL");
    if (!shell.len || shell[0]!='/') {
        struct passwd *pw = getpwuid(getuid());
        shell = pw ? clone(a, edstr(pw->pw_shell)) : Str{};
        shell = shell.len && shell[0]=='/' ? shell : Str("/bin/sh");
    }
    return shell;
}

// This program's path, for phase two: not os_exepath on Linux, which
// names a rebuild's replacement, another program, which phase two must
// not be; what, refused, if it was replaced on disk.
static Str edself(Os *os, Arena *a, Str what)
{
    #ifdef __linux__
    Str self = {};
    iz    cap = 4096;
    char *buf = (char *)allocbytes(a, cap);
    iz    len = readlink("/proc/self/exe", buf, (uz)cap-1);
    if (len > 0) {
        buf[len] = 0;
        self = {(u8 *)buf, len};
        struct stat st;
        if (endswith(self, " (deleted)") && stat(buf, &st)) {
            edfail(concat(a, what, ": this program was replaced on disk since its "
                          "engine started, which must start again first"));
        }
    }
    (void)os;
    return self;
    #else
    (void)what;
    return os_exepath(os, a);
    #endif
}

// Become shell with editshell's flags, running "exec 'SELF' FLAG", else
// /bin/sh so; returns only where neither starts, or where this
// program's path is not one every shell kind reads alike
// (editshellsafe), for phase two to run in place, without the profile.
static void edphase(Arena *a, Str shell, Str self, Str flag, b32 login)
{
    if (!editshellsafe(self)) {
        return;
    }
    Str cmd = concat(a, concat(a, concat(a, "'", self), "' "), flag);
    Str shells[] = {shell, "/bin/sh"};
    for (iz k = 0; k < countof(shells); k++) {
        Slice<Str> argv = editshell(a, shells[k], cmd, login);
        char     **av   = alloc<char *>(a, argv.len+1);
        for (iz i = 0; i < argv.len; i++) {
            av[i] = cstr(a, argv[i]);
        }
        execve(av[0], av, environ);
    }
}

// Phase one: the login shell, which runs phase two.
[[noreturn]] static void ttyedit()
{
    Os    os    = {};
    Arena a     = edarena(&os);
    Str   shell = edshell(&a);
    b32   login = !(edenv("PROSPERO_EDIT_LOGIN") == "0");
    Str   self  = edself(&os, &a, "cannot start the editor");
    edphase(&a, shell, self, "--tty-edit-in", login);
    ttyeditin();
}


// prospero --tty-run: a code block of the agent's, run in a terminal
// as term.open's run starts it (ptyhost.cpp), with the request in
// PROSPERO_RUN_TEXT, _LANG, _SHELL (the terminal's shell) and _LOGIN.
// It shows the command, dimmed, then becomes its shell: a POSIX sh's
// interactive, so that its rc files define what a pasted command would
// find (nvm, conda), running the text by -c; for another (fish, csh)
// the login shell runs phase two, "--tty-run-in", which runs the text
// in the block's own shell if this machine has it (bash, zsh, ksh),
// else /bin/sh. PowerShell's runs in pwsh, by psrunboot. The command's
// exit is the terminal's.

static char const *const runnames[] = {
    "PROSPERO_RUN_TEXT", "PROSPERO_RUN_LANG", "PROSPERO_RUN_SHELL", "PROSPERO_RUN_LOGIN",
};

// A program on PATH, by its full-path entries alone, as phase two of an
// edit finds its editor: else null.
static Str runfind(Os *os, Arena *a, Str prog)
{
    Str path = edenv("PATH");
    Buf pb(a, path.len+1);
    for (Str rest = path; rest.len;) {
        Cut c = cut(rest, ':');
        rest = c.tail;
        if (c.head.len && c.head[0]=='/') {
            if (pb.len) {
                putbyte(&pb, ':');
            }
            print(&pb, c.head);
        }
    }
    Str clean = finish(&pb);
    Arena tmp = carve(a, 1<<20);
    Str   exe = clean.len ? findprog(os, a, tmp, clean, prog, {}, 0) : Str{};
    return exe.len && !hasnul(exe) && !access(cstr(a, exe), X_OK) ? exe : Str{};
}

// sh -c's text: options may follow -c, so text that would read as one
// goes after a newline.
static char *runarg(Arena *a, Str text)
{
    b32 opt = text.len && (text[0]=='-' || text[0]=='+');
    return cstr(a, opt ? concat(a, "\n", text) : text);
}

// Phase two: the block's shell, else /bin/sh, in place of this process.
[[noreturn]] static void ttyrunin()
{
    Os    os   = {};
    Arena a    = edarena(&os);
    Str   text = clone(&a, edenv("PROSPERO_RUN_TEXT"));
    Str   lang = clone(&a, edenv("PROSPERO_RUN_LANG"));
    if (!text.len) {
        edfail("nothing to run");
    }
    for (iz i = 0; i < countof(runnames); i++) {
        unsetenv(runnames[i]);
    }
    Str exe = {};
    Str want[] = {"bash", "zsh", "ksh"};
    for (iz i = 0; !exe.len && i < countof(want); i++) {
        exe = equalfold(trim(lang), want[i]) ? runfind(&os, &a, want[i]) : Str{};
    }
    exe = exe.len ? exe : Str("/bin/sh");
    char *av[] = {cstr(&a, exe), (char *)"-c", runarg(&a, text), 0};
    execve(av[0], av, environ);
    edfail(concat(&a, concat(&a, concat(&a, "cannot run ", exe), ": "), os_lasterr(&os, &a)));
}

// Phase one: the header, then the shell.
[[noreturn]] static void ttyrun()
{
    Os    os    = {};
    Arena a     = edarena(&os);
    Str   text  = clone(&a, edenv("PROSPERO_RUN_TEXT"));
    Str   lang  = clone(&a, edenv("PROSPERO_RUN_LANG"));
    Str   shell = clone(&a, edenv("PROSPERO_RUN_SHELL"));
    b32   login = !(edenv("PROSPERO_RUN_LOGIN") == "0");
    if (!text.len) {
        edfail("nothing to run");
    }

    // What runs, dimmed, so that the scrollback and the agent see it
    Buf b(&a, text.len + 16);
    print(&b, "\x1b[2m");
    print(&b, text);
    print(&b, "\x1b[0m\n");
    writeall(1, finish(&b));

    if (termrunlang(lang) == RUN_PS) {
        Str pwsh = runfind(&os, &a, "pwsh");
        if (!pwsh.len) {
            edfail("PowerShell blocks run in pwsh, which is not on PATH");
        }
        for (iz i = 1; i < countof(runnames); i++) {
            unsetenv(runnames[i]);  // the text stays, for psrunboot
        }
        char *av[6] = {};
        i32   n     = 0;
        av[n++] = cstr(&a, pwsh);
        if (login) {
            av[n++] = (char *)"-Login";  // it must come first
        }
        av[n++] = (char *)"-NoLogo";
        av[n++] = (char *)"-Command";
        av[n++] = (char *)psrunboot;
        execve(av[0], av, environ);
        edfail(concat(&a, "cannot run pwsh: ", os_lasterr(&os, &a)));
    }

    shell = shell.len && shell[0]=='/' ? shell : edshell(&a);
    Str  name    = basename(shell);
    Str  posix[] = {"sh", "bash", "zsh", "dash", "ksh", "mksh", "ash", "yash"};
    b32  sh      = 0;
    for (iz i = 0; !sh && i < countof(posix); i++) {
        sh = name == posix[i];
    }
    if (sh) {
        for (iz i = 0; i < countof(runnames); i++) {
            unsetenv(runnames[i]);
        }
        char *av[6] = {};
        i32   n     = 0;
        av[n++] = cstr(&a, shell);
        if (login) {
            av[n++] = (char *)"-l";
        }
        av[n++] = (char *)"-i";
        av[n++] = (char *)"-c";
        av[n++] = runarg(&a, text);
        execve(av[0], av, environ);
        setenv(runnames[0], cstr(&a, text), 1);  // for phase two after all
        setenv(runnames[1], cstr(&a, lang), 1);
    }
    unsetenv("PROSPERO_RUN_SHELL");
    unsetenv("PROSPERO_RUN_LOGIN");
    edphase(&a, shell, edself(&os, &a, "cannot run the command"), "--tty-run-in", login);
    ttyrunin();
}


// prospero --tty-probe: a stand-in shell for testing terminals, which
// reads a command a line from its terminal and answers on it, raw (no
// line discipline, echo or output processing), so that what it writes
// arrives as written, and never as a password prompt would read:
//
//   size           "size COLSxROWS XPXxYPX"
//   echo TEXT      "TEXT"
//   flood N        N bytes of floodbyte, then "flood end"
//   env NAME       "env NAME=VALUE", or "env NAME unset"
//   cwd            "cwd DIR"
//   args           "args [WORD] [WORD]...", each of its own arguments
//                  after its name, with controls, brackets and backslash
//                  as \xHH
//   hup FILE       on SIGHUP, write "hup" to FILE and exit 129
//   sleep MS       "slept" once MS passed
//   busy MS        a job in the foreground for MS, then "busy done"
//   nohup SECONDS  a child that ignores SIGHUP and lives that long,
//                  "nohup PID"
//   exit N         exit with status N
//   pass [MS]      after MS, echo off in line mode, "Password: ", a line
//                  read, then "\r\npass got LINE\r\n", as a program that
//                  prints what it read
//   cooked, raw    a shell's line mode with echo, or back: "cooked", "raw"
//   say TEXT       "TEXT\r\n"
//   lines N        "line 1\r\n" to "line N\r\n"
//   title TEXT     OSC 2, the window's title
//
// It starts with "probe ready", and a resize prints "winch COLSxROWS".

static volatile sig_atomic_t probewinch;
static char probehup[1024];

static void probeonwinch(int) { probewinch = 1; }

static void probeonhup(int)
{
    i32 fd = open(probehup, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, "hup\n", 4)) {}
        close(fd);
    }
    _exit(129);
}

struct ProbeOut {
    u8 buf[4096];
    iz len;
};

static void probeput(ProbeOut *o, Str s)
{
    for (iz i = 0; i<s.len && o->len<countof(o->buf); i++) {
        o->buf[o->len++] = s[i];
    }
}

static void probeint(ProbeOut *o, i64 v)
{
    u8  tmp[24];
    iz  n   = 0;
    u64 u   = v<0 ? -(u64)v : (u64)v;
    do tmp[n++] = (u8)('0' + u%10); while (u /= 10);
    if (v < 0) tmp[n++] = '-';
    for (; n > 0; n--) probeput(o, {tmp+n-1, 1});
}

static void probeflush(ProbeOut *o)
{
    probeput(o, "\n");
    writeall(1, {o->buf, o->len});
    o->len = 0;
}

static void probesize(ProbeOut *o, Str what, b32 px)
{
    struct winsize ws = {};
    ioctl(0, TIOCGWINSZ, &ws);
    probeput(o, what);
    probeint(o, ws.ws_col);
    probeput(o, "x");
    probeint(o, ws.ws_row);
    if (px) {
        probeput(o, " ");
        probeint(o, ws.ws_xpixel);
        probeput(o, "x");
        probeint(o, ws.ws_ypixel);
    }
    probeflush(o);
}

static void probenap(i64 ms)
{
    struct timespec ts = {(time_t)(ms/1000), (long)(ms%1000*1000000)};
    while (nanosleep(&ts, &ts)<0 && errno==EINTR) {}
}

// Byte i of a flood: lines of 64 bytes, letters shifting by line.
static u8 floodbyte(i64 i)
{
    return (u8)(i%64==63 ? '\n' : 'a' + (i/64 + i%64)%26);
}

// A word of "args", in brackets: controls, brackets and backslash as \xHH.
static void probeword(ProbeOut *o, Str w)
{
    probeput(o, " [");
    for (iz i = 0; i < w.len; i++) {
        u8 c = w[i];
        if (c<0x20 || c==0x7f || c=='[' || c==']' || c=='\\') {
            u8 hex[] = {'\\', 'x', (u8)"0123456789abcdef"[c>>4], (u8)"0123456789abcdef"[c&15]};
            probeput(o, {hex, 4});
        } else {
            probeput(o, {w.data+i, 1});
        }
    }
    probeput(o, "]");
}

// The probe's terminal modes: raw (no line discipline, no echo, so that
// it never looks like a password prompt), cooked (a shell's: lines and
// echo), or a password's (lines, no echo). Signals stay on throughout.
enum { PROBE_RAW, PROBE_COOKED, PROBE_PASS };

static void probemode(i32 mode, i32 when)
{
    struct termios tio = {};
    if (tcgetattr(0, &tio)) {
        return;
    }
    tio.c_oflag &= ~(tcflag_t)OPOST;
    tio.c_lflag &= ~(tcflag_t)(ECHO|ECHONL|ICANON);
    tio.c_lflag |= mode==PROBE_COOKED ? (tcflag_t)(ECHO|ICANON) :
                   mode==PROBE_PASS   ? (tcflag_t)ICANON : 0;
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(0, when, &tio);
}

static void ttyprobe(char **argv)
{
    i32 mode = PROBE_RAW;
    probemode(mode, TCSANOW);
    struct sigaction sa = {};
    sa.sa_handler = probeonwinch;  // no SA_RESTART: read returns
    sigaction(SIGWINCH, &sa, 0);

    ProbeOut o = {};
    probeput(&o, "probe ready");
    probeflush(&o);
    u8 line[4096];
    iz len = 0;
    for (;;) {
        if (probewinch) {
            probewinch = 0;
            probesize(&o, "winch ", 0);
        }
        iz nl = 0;
        for (; nl<len && line[nl]!='\n' && line[nl]!='\r'; nl++) {}
        if (nl == len) {
            len = len==countof(line) ? 0 : len;
            iz r = read(0, line+len, (size_t)(countof(line)-len));
            if (r<0 && errno==EINTR) {
                continue;
            } else if (r <= 0) {
                _exit(0);
            }
            len += r;
            continue;
        }
        Str cmd = {line, nl};
        Cut c   = cut(cmd, ' ');
        Str arg = c.tail;
        Int n   = parseint(arg);
        if (c.head == "size") {
            probesize(&o, "size ", 1);
        } else if (c.head == "echo") {
            probeput(&o, arg);
            probeflush(&o);
        } else if (c.head=="flood" && n.ok && n.val>=0) {
            u8 buf[4096];
            for (i64 i = 0; i < n.val;) {
                iz k = 0;
                for (; k<countof(buf) && i<n.val; k++, i++) {
                    buf[k] = floodbyte(i);
                }
                writeall(1, {buf, k});
            }
            probeput(&o, "flood end");
            probeflush(&o);
        } else if (c.head == "env") {
            char name[256] = {};
            for (iz i = 0; i<arg.len && i<countof(name)-1; i++) {
                name[i] = (char)arg[i];
            }
            char *v = getenv(name);
            probeput(&o, "env ");
            probeput(&o, arg);
            if (v) {
                probeput(&o, "=");
                probeput(&o, {(u8 *)v, (iz)strlen(v)});
            } else {
                probeput(&o, " unset");
            }
            probeflush(&o);
        } else if (cmd == "args") {
            probeput(&o, "args");
            for (char **w = argv+1; *w; w++) {
                probeword(&o, edstr(*w));
            }
            probeflush(&o);
        } else if (c.head == "cwd") {
            char dir[1024];
            probeput(&o, "cwd ");
            if (getcwd(dir, sizeof(dir))) {
                probeput(&o, {(u8 *)dir, (iz)strlen(dir)});
            }
            probeflush(&o);
        } else if (c.head=="hup" && arg.len && arg.len<countof(probehup)) {
            __builtin_memcpy(probehup, arg.data, (size_t)arg.len);
            probehup[arg.len] = 0;
            signal(SIGHUP, probeonhup);
            probeput(&o, "hup set");
            probeflush(&o);
        } else if (c.head=="sleep" && n.ok) {
            probenap(n.val);
            probeput(&o, "slept");
            probeflush(&o);
        } else if (c.head=="busy" && n.ok) {
            signal(SIGTTOU, SIG_IGN);
            pid_t pid = fork();
            if (!pid) {
                setpgid(0, 0);
                tcsetpgrp(0, getpid());
                probenap(n.val);
                _exit(0);
            } else if (pid > 0) {
                setpgid(pid, pid);
                tcsetpgrp(0, pid);
                while (waitpid(pid, 0, 0)<0 && errno==EINTR) {}
                tcsetpgrp(0, getpgrp());
            }
            probeput(&o, "busy done");
            probeflush(&o);
        } else if (c.head=="nohup" && n.ok) {
            pid_t pid = fork();
            if (!pid) {
                signal(SIGHUP, SIG_IGN);
                i32 null = open("/dev/null", O_RDWR);
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
                probenap(n.val*1000);
                _exit(0);
            }
            probeput(&o, "nohup ");
            probeint(&o, pid);
            probeflush(&o);
        } else if (c.head=="exit" && n.ok) {
            _exit((int)n.val);
        } else if (c.head=="pass" && (!arg.len || n.ok)) {
            // As sudo asks: after a pause (typing may go ahead of it),
            // echo off with what was typed kept, a prompt, then a line;
            // what was read is printed, a program's slip
            probenap(arg.len ? n.val : 0);
            probemode(PROBE_PASS, TCSADRAIN);
            writeall(1, "Password: ");
            u8 pw[256];
            iz pwlen = 0;
            for (;;) {
                iz r = read(0, pw+pwlen, (size_t)(countof(pw)-pwlen));
                if (r<0 && errno==EINTR) {
                    continue;
                } else if (r <= 0) {
                    _exit(0);
                }
                pwlen += r;
                if (pw[pwlen-1]=='\n' || pwlen==countof(pw)) {
                    break;
                }
            }
            pwlen -= pwlen && pw[pwlen-1]=='\n';
            probemode(mode, TCSANOW);
            writeall(1, "\r\npass got ");
            writeall(1, {pw, pwlen});
            writeall(1, "\r\n");
        } else if (cmd=="cooked" || cmd=="raw") {
            mode = cmd=="cooked" ? PROBE_COOKED : PROBE_RAW;
            probemode(mode, TCSANOW);
            probeput(&o, cmd);
            probeflush(&o);
        } else if (c.head == "say") {
            writeall(1, arg);
            writeall(1, "\r\n");
        } else if (c.head=="lines" && n.ok) {
            for (i64 i = 1; i <= n.val; i++) {
                probeput(&o, "line ");
                probeint(&o, i);
                probeput(&o, "\r");
                probeflush(&o);
            }
        } else if (c.head == "title") {
            writeall(1, "\x1b]2;");
            writeall(1, arg);
            writeall(1, "\x07");
        } else if (cmd.len) {
            probeput(&o, "? ");
            probeput(&o, cmd);
            probeflush(&o);
        }
        iz rest = len - nl - 1;
        __builtin_memmove(line, line+nl+1, (size_t)rest);
        len = rest;
    }
}


#if PROSPERO_DESKTOP
// The desktop helper, "prospero desktop" (main_desktop_x11.cpp)
extern "C" int prospero_desktop(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    if (argc>1 && reapmode(argv[1])) {
        reap(argc, argv);
    } else if (argc>1 && !__builtin_strcmp(argv[1], "--tty-probe")) {
        ttyprobe(argv);
    } else if (argc>1 && !__builtin_strcmp(argv[1], "--tty-edit")) {
        ttyedit();
    } else if (argc>1 && !__builtin_strcmp(argv[1], "--tty-edit-in")) {
        ttyeditin();
    } else if (argc>1 && !__builtin_strcmp(argv[1], "--tty-run")) {
        ttyrun();
    } else if (argc>1 && !__builtin_strcmp(argv[1], "--tty-run-in")) {
        ttyrunin();
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
        loginenviron(&os, &a);
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
