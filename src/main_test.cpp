// Unit tests: a platform layer with an in-memory file system, a fake
// clock, and single-threaded stand-ins for threads, locks, and events.
// $ cmake --build build && ./build/harness_test
// This is free and unencumbered software released into the public domain.
#include <stdio.h>
#include <stdlib.h>
#if HARNESS_GUI
#  include "md4c.h"
#endif
#include "harness.cpp"
#include "wide.cpp"

struct VFile {
    VFile *next;
    Str    path;
    Str    data;
    i64    mtime;     // os_time when last written
    b32    exists;
    b32    isdir;
};

struct VLink {
    VLink *next;
    Str    from;  // absolute
    Str    to;    // absolute
};

struct Os {
    Arena      mem;       // file contents and names live here
    VFile     *files;
    VLink     *links;     // symlinks, followed by os_realpath only
    Str        locks[16]; // held lock files, by os_lockfile handle
    VFile     *logs[16];  // log handles index this table
    iz         nlogs;
    Slice<Str> env;       // "NAME=value" entries for os_getenv
    i64        now;       // microseconds, advanced by tests
    u64        rng;
    i32        syncs;     // os_logsync calls
    i32        realpaths; // os_realpath calls
    i32        writes;    // os_logwrite calls left while failwrites is set
    b32        failwrites;
    b32        failtrunc;
    b32        failsync;
};

static VFile *vfind(Os *os, Str path, b32 create)
{
    for (VFile *f = os->files; f; f = f->next) {
        if (f->path == path) {
            return f;
        }
    }
    if (!create) {
        return 0;
    }
    VFile *f = alloc<VFile>(&os->mem);
    f->path   = clone(&os->mem, path);
    f->next   = os->files;
    os->files = f;
    return f;
}

[[noreturn]] static void os_oom() { __builtin_trap(); }

// Reservations come from the C heap, with commits only bookkeeping.
static Region *os_reserve(Os *, iz cap)
{
    byte *p = (byte *)malloc((uz)cap);
    if (!p) {
        return 0;
    }
    Region *r = (Region *)p;
    r->base      = p + 64;
    r->cap       = cap - 64;
    r->committed = 0;
    return r;
}

static b32 os_commit(Region *r, iz upto)
{
    r->committed = upto>r->committed ? upto : r->committed;
    return 1;
}

static void os_release(Os *, Region *r) { free(r); }

static i64 os_ticks(Os *os) { return os->now; }
static i64 os_time(Os *os) { return os->now / 1000; }

static u64 os_random(Os *os)
{
    os->rng += 0x9e3779b97f4a7c15;
    u64 x = os->rng;
    x ^= x >> 32;
    x *= 0xd6e8feb86659fd93;
    x ^= x >> 32;
    return x;
}

static Read os_read(Os *os, Arena *a, Str path, iz max)
{
    Read   r = {};
    VFile *f = vfind(os, path, 0);
    if (!f || !f->exists) {
        r.err = READ_NOTFOUND;
    } else if (f->isdir) {
        r.err = READ_NOTFILE;
    } else if (f->data.len > max) {
        r.size = f->data.len;
        r.err  = READ_TOOBIG;
    } else {
        r.size = f->data.len;
        r.data = clone(a, f->data);
        if (!r.data.data) r.data.data = (u8 *)"";
    }
    return r;
}

static b32 os_write(Os *os, Arena, Str path, Str data)
{
    VFile *f = vfind(os, path, 1);
    f->data   = clone(&os->mem, data);
    f->mtime  = os_time(os);
    f->exists = 1;
    f->isdir  = 0;
    return 1;
}

static b32 os_mkdirs(Os *os, Arena, Str path)
{
    VFile *f = vfind(os, path, 1);
    f->exists = 1;
    f->isdir  = 1;
    return 1;
}

static Dirent *os_list(Os *os, Arena *a, Str path)
{
    Dirent *head = 0;
    for (VFile *f = os->files; f; f = f->next) {
        Str p = f->path;
        if (!f->exists || p.len<=path.len+1 || !startswith(p, path) ||
                p[path.len]!='/') {
            continue;
        }
        Str name = cuthead(p, path.len+1);
        if (cut(name, '/').ok) {
            continue;
        }
        Dirent *d = alloc<Dirent>(a);
        d->name  = clone(a, name);
        d->size  = f->data.len;
        d->isdir = f->isdir;
        d->mtime = f->mtime;
        d->next  = head;
        head = d;
    }
    return head;
}

static Stat os_stat(Os *os, Arena, Str path)
{
    Stat   s = {};
    VFile *f = vfind(os, path, 0);
    if (f && f->exists) {
        s.type  = f->isdir ? STAT_DIR : STAT_FILE;
        s.size  = f->data.len;
        s.mtime = f->mtime;
    }
    return s;
}

static b32 os_rename(Os *os, Arena, Str from, Str to)
{
    VFile *src = vfind(os, from, 0);
    if (!src || !src->exists) {
        return 0;
    }
    VFile *dst = vfind(os, to, 1);
    dst->data   = src->data;
    dst->isdir  = src->isdir;
    dst->exists = 1;
    src->exists = 0;
    return 1;
}

static b32 os_remove(Os *os, Arena, Str path)
{
    VFile *f = vfind(os, path, 0);
    if (!f || !f->exists) {
        return 0;
    }
    f->exists = 0;
    return 1;
}

static b32 os_print(Os *, i32 fd, Str s)
{
    return fwrite(s.data, 1, (uz)s.len, fd==1 ? stdout : stderr) == (uz)s.len;
}

static Str os_getenv(Os *os, Arena *a, Str name)
{
    for (iz i = 0; i < os->env.len; i++) {
        Cut c = cut(os->env[i], '=');
        if (c.head == name) {
            return clone(a, c.tail);
        }
    }
    return {};
}
static Str os_exepath(Os *, Arena *) { return "harness"; }

static b32  os_thread(Os *, void (*)(void *), void *) { return 0; }
static void os_lockinit(Lock *) {}
static void os_lock(Lock *) {}
static void os_unlock(Lock *) {}
static b32  os_eventinit(Event *e) { e->h[0] = 0; return 1; }
static void os_signal(Event *e) { e->h[0] = 1; }

static b32 os_wait(Event *e, i32)
{
    b32 r = (b32)e->h[0];
    e->h[0] = 0;
    return r;
}

static iz os_logopen(Os *os, Arena, Str path)
{
    if (os->nlogs == countof(os->logs)) {
        return -1;
    }
    VFile *f = vfind(os, path, 1);
    if (!f->exists) {
        f->data   = {};
        f->exists = 1;
    }
    os->logs[os->nlogs] = f;
    return os->nlogs++;
}

static b32 os_logwrite(Os *os, iz log, Str record)
{
    if (os->failwrites) {
        if (!os->writes) {
            return 0;
        }
        os->writes--;
    }
    VFile *f = os->logs[log];
    f->data = concat(&os->mem, f->data, record);
    return 1;
}

static b32 os_logsync(Os *os, iz)
{
    os->syncs++;
    return !os->failsync;
}

static b32 os_logtrunc(Os *os, iz log, i64 len)
{
    VFile *f = os->logs[log];
    if (os->failtrunc || len<0 || len>f->data.len) {
        return 0;
    }
    f->data.len = (iz)len;
    return 1;
}

static void os_logclose(Os *os, iz log)
{
    os->logs[log] = 0;
}

static Read os_readat(Os *os, Arena *a, Str path, i64 off, iz max)
{
    Read r = os_read(os, a, path, I64MAX>>1);
    if (!r.err) {
        Str d = r.data;
        d = off<d.len ? cuthead(d, (iz)off) : cuthead(d, d.len);
        r.data = d.len>max ? takehead(d, max) : d;
    }
    return r;
}

// A symlink at from, to an absolute target.
static void vlink(Os *os, Str from, Str to)
{
    VLink *l = alloc<VLink>(&os->mem);
    l->from  = clone(&os->mem, from);
    l->to    = clone(&os->mem, to);
    l->next  = os->links;
    os->links = l;
}

// Paths are taken as given, except that links are followed wherever one
// covers a leading part of the path. Windows paths in tests use '/'.
static Str os_realpath(Os *os, Arena *a, Str path)
{
    os->realpaths++;
    for (i32 hops = 0; hops < 40; hops++) {
        VLink *l = os->links;
        for (; l; l = l->next) {
            Str f = l->from;
            if (startswith(path, f) && (path.len==f.len || path[f.len]=='/')) {
                break;
            }
        }
        if (!l) {
            VFile *f = vfind(os, path, 0);
            return f && f->exists ? clone(a, path) : Str{};
        }
        path = concat(a, l->to, cuthead(path, l->from.len));
    }
    return {};
}

static iz os_lockfile(Os *os, Arena, Str path)
{
    iz slot = -1;
    for (iz i = countof(os->locks)-1; i >= 0; i--) {
        if (os->locks[i].data && os->locks[i]==path) {
            return -1;
        }
        slot = os->locks[i].data ? slot : i;
    }
    if (slot >= 0) {
        os->locks[slot] = clone(&os->mem, path);
    }
    return slot;
}

static void os_unlockfile(Os *os, iz lock)
{
    os->locks[lock] = {};
}

static b32 os_lockheld(Os *os, Arena, Str path)
{
    for (iz i = 0; i < countof(os->locks); i++) {
        if (os->locks[i].data && os->locks[i]==path) {
            return 1;
        }
    }
    return 0;
}

// No processes, HTTP, or standard input: the engine is tested end to
// end by tests/scenarios.py against the real platforms.
static Slice<Str> os_environ(Os *, Arena *) { return {}; }
static iz    os_stdin(Os *, u8 *, iz) { return 0; }
static Proc *os_spawn(Os *, Spawn *, Arena) { return 0; }
static Proc *os_spawnio(Os *, Spawn *, Arena) { return 0; }
static b32   os_procwrite(Os *, Proc *, Str) { return 0; }
static void  os_kill(Os *, Proc *, b32) {}
static void  os_procfree(Os *, Proc *) {}
static Http *os_http(Os *, HttpReq *, Arena) { return 0; }
static void  os_httpcancel(Os *, Http *) {}
static void  os_httpfree(Os *, Http *) {}
static Str   os_lasterr(Os *, Arena *) { return "not supported in tests"; }

// A fresh test platform with its own storage carved from the arena.
static Os *newos(Arena *a, iz cap)
{
    Os *os = alloc<Os>(a);
    os->mem.beg = (byte *)allocbytes(a, cap);
    os->mem.end = os->mem.beg + cap;
    return os;
}


struct Test {
    Arena perm;
    i32   run;
    i32   failed;
};

static void printstr(FILE *f, Str s)
{
    fwrite(s.data, 1, (uz)s.len, f);
}

// Compare strings, printing both on mismatch.
static b32 expect(Test *t, Str name, Str got, Str want)
{
    t->run++;
    if (got == want) {
        return 1;
    }
    t->failed++;
    fprintf(stderr, "FAIL: ");
    printstr(stderr, name);
    fprintf(stderr, "\n  got:  [");
    printstr(stderr, got);
    fprintf(stderr, "]\n  want: [");
    printstr(stderr, want);
    fprintf(stderr, "]\n");
    return 0;
}

// Render an integer for comparison with expect.
static Str istr(Arena *a, i64 v)
{
    Buf b(a, 24);
    print(&b, v);
    return finish(&b);
}

#include "test/base_test.cpp"
#include "test/wide_test.cpp"
#include "test/json_test.cpp"
#include "test/sse_test.cpp"
#include "test/ini_test.cpp"
#include "test/mailbox_test.cpp"
#include "test/events_test.cpp"
#include "test/provider_test.cpp"
#include "test/tools_test.cpp"
#include "test/guard_test.cpp"
#include "test/admit_test.cpp"
#include "test/compact_test.cpp"
#include "test/search_test.cpp"
#include "test/instructions_test.cpp"
#include "test/engine_test.cpp"
#include "test/config_test.cpp"
#if HARNESS_GUI
#  include "sched.cpp"
#  include "highlight.cpp"
#  include "doc.cpp"
#  include "layout.cpp"
#  include "view.cpp"
#  include "theme.cpp"
#  include "test/sched_test.cpp"
#  include "test/highlight_test.cpp"
#  include "test/doc_test.cpp"
#  include "test/layout_test.cpp"
#  include "test/view_test.cpp"
#  include "test/theme_test.cpp"
#endif

int main()
{
    iz    cap = (iz)1<<28;
    byte *mem = (byte *)malloc((uz)cap);
    Test  t   = {};
    t.perm = {mem, mem+cap};

    test_base(&t);
    test_wide(&t);
    test_json(&t);
    test_sse(&t);
    test_ini(&t);
    test_mailbox(&t);
    test_events(&t);
    test_provider(&t);
    test_tools(&t);
    test_guard(&t);
    test_admit(&t);
    test_compact(&t);
    test_search(&t);
    test_instructions(&t);
    test_engine(&t);
    test_config(&t);
    #if HARNESS_GUI
    test_sched(&t);
    test_highlight(&t);
    test_doc(&t);
    test_layout(&t);
    test_view(&t);
    test_theme(&t);
    #endif

    fprintf(stderr, "%d tests, %d failed\n", t.run, t.failed);
    return t.failed ? 1 : 0;
}
