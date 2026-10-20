// harness: a desktop coding harness for locally hosted and remote LLMs
//
// The program is the harness; the LLM driving it is the agent.
//
// Unity build: a platform layer (main_posix.cpp, main_win32.cpp,
// main_test.cpp) includes this file and implements the os_* API.
//   $ cmake -B build && cmake --build build && ctest --test-dir build
//
// This is free and unencumbered software released into the public domain.
#include "base.cpp"

#ifndef HARNESS_VERSION
#  define HARNESS_VERSION "0.0.0"
#endif


// Platform API
//
// The platform does raw OS work only. Arena* parameters receive results;
// Arena by value is scratch the platform may use freely. Functions marked
// (M2) or (M3) are implemented by those milestones and may be undefined
// until then: declaring a static function without defining it is fine as
// long as nothing calls it.
//
// A path or name holding a null byte names nothing: os_stat gives
// STAT_MISSING, os_read READ_NOTFOUND, os_getenv null, os_logopen -1, and
// the rest fail.

struct Os;
struct Mailbox;
struct Proc;
struct Http;

// Mutex storage, initialized in place by os_lockinit and never moved.
struct Lock {
    alignas(8) u8 opaque[64];
};

// Auto-reset event: os_signal from any thread (and from a POSIX signal
// handler), coalescing until os_wait consumes it. One signal wakes at
// most one waiter.
struct Event {
    iz h[2];
};

enum {
    STAT_MISSING,
    STAT_FILE,
    STAT_DIR,
    STAT_OTHER,
};

struct Stat {
    i64 size;
    i64 mtime;  // Unix epoch milliseconds
    i32 type;
};

struct Dirent {
    Dirent *next;
    Str     name;
    i64     size;
    i64     mtime;  // Unix epoch milliseconds
    b32     isdir;
};

enum {
    READ_OK,
    READ_NOTFOUND,
    READ_TOOBIG,
    READ_NOTFILE,
    READ_IO,
};

struct Read {
    Str data;  // null unless err is READ_OK
    i64 size;  // reported even when too big
    i32 err;
};

// Reserve address space for a growable arena (newarena), with the Region
// header at its start. Returns null on failure.
static Region *os_reserve(Os *, iz cap);
// Release a reservation and everything allocated in it.
static void    os_release(Os *, Region *);
// Also: os_oom() and os_commit(), declared in base.cpp.

// Monotonic clock in microseconds, only for measuring intervals.
static i64     os_ticks(Os *);
// Current time in Unix epoch milliseconds.
static i64     os_time(Os *);
// 64 bits from the operating system's entropy source.
static u64     os_random(Os *);

// Read a regular file of at most max bytes into the arena.
static Read    os_read(Os *, Arena *, Str path, iz max);
// Replace a file atomically: write a temporary sibling, then rename it
// over the target.
static b32     os_write(Os *, Arena, Str path, Str data);
// Create a directory and all its parents.
static b32     os_mkdirs(Os *, Arena, Str path);
// List a directory, excluding "." and "..", symlinks, and special files.
static Dirent *os_list(Os *, Arena *, Str path);
// Type, size, and modification time of a path, following symlinks.
static Stat    os_stat(Os *, Arena, Str path);
// Rename a file, replacing any existing target.
static b32     os_rename(Os *, Arena, Str from, Str to);
// Delete a file.
static b32     os_remove(Os *, Arena, Str path);
// Write to standard output (1) or standard error (2).
static b32     os_print(Os *, i32 fd, Str);
// Environment variable, or a null string if unset.
static Str     os_getenv(Os *, Arena *, Str name);
// Absolute path of the running executable. On Linux, after the binary
// is replaced, this names the replacement: exec /proc/self/exe to run
// this same binary again.
static Str     os_exepath(Os *, Arena *);

// Start a detached thread running fn(arg). Returns 0 on failure.
static b32     os_thread(Os *, void (*fn)(void *), void *arg);
static void    os_lockinit(Lock *);
static void    os_lock(Lock *);
static void    os_unlock(Lock *);
// Returns 0 on failure.
static b32     os_eventinit(Event *);
static void    os_signal(Event *);
// Wait up to ms milliseconds (forever if negative). 1 if signaled.
static b32     os_wait(Event *, i32 ms);

// Append-only log files. Handles are -1 on failure.
static iz      os_logopen(Os *, Arena, Str path);  // creates if missing
static b32     os_logwrite(Os *, iz log, Str record);  // one write per record
static b32     os_logsync(Os *, iz log);  // flush to stable storage
static b32     os_logtrunc(Os *, iz log, i64 len);  // drop a torn tail
static void    os_logclose(Os *, iz log);

// (M2) Read up to max bytes of a regular file starting at off. The size
// is that of the whole file, and data is short only at its end.
static Read    os_readat(Os *, Arena *, Str path, i64 off, iz max);
// (M2) The environment as "NAME=value" entries.
static Slice<Str> os_environ(Os *, Arena *);
// (M2) Read standard input: bytes read, 0 at its end, -1 on error.
static iz      os_stdin(Os *, u8 *buf, iz len);

// (M2) Records that processes and HTTP requests post to their mailbox,
// with the Spawn or HttpReq id. A payload starts with its integers,
// native-endian and unaligned, then any text.
enum : u32 {
    PROC_TAIL = 1,  // i64 total, then the latest output (4 KiB, <=20 Hz)
    PROC_LIMIT,     // i64 total: maxbytes reached, output no longer kept
    PROC_EXIT,      // i32 code, i32 signal (POSIX, else 0): it exited
    PROC_EOF,       // i64 total: every writer closed the output
    HTTP_HEAD,      // i32 status, then raw "Name: value\r\n" header lines
    HTTP_BODY,      // response body bytes, in order
    HTTP_DONE,      // i32 error (HTTP_OK or HTTP_E*), then a description
};

enum : i32 {
    HTTP_OK,
    HTTP_ECANCEL,   // os_httpcancel
    HTTP_ECONNECT,  // could not connect (includes TLS failures)
    HTTP_EIO,       // the connection failed after connecting
    HTTP_ETOOBIG,   // the response passed maxbytes
    HTTP_EURL,      // the URL could not be used
};

// (M2) Child processes: null stdin, merged stdout and stderr pumped into
// outpath, in a kill-on-close job (Windows) or under a reaper (POSIX).
// Records: PROC_TAIL may be dropped when the mailbox is full; PROC_EXIT
// and PROC_EOF arrive exactly once each, in either order, and PROC_LIMIT
// at most once. A process that escapes and holds the output open delays
// PROC_EOF indefinitely, so callers bound their wait for it.
struct Spawn {
    Slice<Str> argv;
    Str        cmdline;   // Windows: used verbatim instead of quoting argv
    Str        cwd;
    Slice<Str> env;       // complete environment, "NAME=value"
    Str        outpath;   // receives merged output
    i64        maxbytes;  // output limit
    Mailbox   *mbox;      // receives process records
    u64        id;
};
// Null on failure, explained by os_lasterr. The process is not waited
// for, and its records may arrive before this returns.
static Proc   *os_spawn(Os *, Spawn *, Arena scratch);
// Soft: SIGTERM to the process group (Windows: same as hard, for now).
// Hard: SIGKILL to the group, or terminate the job: the whole tree.
static void    os_kill(Os *, Proc *, b32 hard);
// Drop the caller's reference. Kills nothing by itself on POSIX, but on
// Windows closing the last reference closes the job, killing the tree.
static void    os_procfree(Os *, Proc *);

// (M2) HTTP requests, each on its own thread, posting records to mbox:
// HTTP_HEAD once if a response arrives, then HTTP_BODY, then always one
// HTTP_DONE. Body records wait for room in the mailbox. Redirects are
// not followed. No retries at this layer.
struct HttpReq {
    Str      method;
    Str      url;
    Str      headers;    // "Name: value\r\n" lines
    Str      body;
    i32      connectms;
    i32      readms;     // longest wait for data, 0 for no limit
    i64      maxbytes;   // response limit
    Mailbox *mbox;
    u64      id;
};
// Null on failure, explained by os_lasterr.
static Http   *os_http(Os *, HttpReq *, Arena scratch);
static void    os_httpcancel(Os *, Http *);  // any thread, prompt
static void    os_httpfree(Os *, Http *);
// Description of the last platform error on this thread.
static Str     os_lasterr(Os *, Arena *);

// (M3) Guard, bidirectional children, session locks, desktop.
static Str     os_realpath(Os *, Arena *, Str path);  // null if missing
static Proc   *os_spawnio(Os *, Spawn *, Arena scratch);
static b32     os_procwrite(Os *, Proc *, Str);
static b32     os_lockfile(Os *, Arena, Str path);
static b32     os_openurl(Os *, Arena, Str url);
static void    os_busy(Os *, i32 state);

// (M1) GUI builds: wake the GUI loop from any thread, coalesced.
static void    os_wake(Os *);


// A growable arena over a reservation, or a null arena given null.
static Arena newarena(Region *r)
{
    Arena a = {};
    if (r) {
        a.beg    = r->base;
        a.end    = r->base + r->committed;
        a.region = r;
    }
    return a;
}


#include "json.cpp"
#include "sse.cpp"
#include "ini.cpp"
#include "mailbox.cpp"
#include "events.cpp"
#include "provider.cpp"
#include "tools.cpp"
#include "guard.cpp"
#include "engine.cpp"
#include "cli.cpp"
#include "selftest.cpp"
#include "selftest_io.cpp"


static Str usage =
"usage: harness [options]\n"
"       harness run [options] PROMPT\n"
"       harness --headless [options]\n"
"  --version         print the version and exit\n"
"  --selftest [URL]  run the platform integration tests, with HTTP\n"
"                    tests against a fake provider at URL\n"
"  -h                print this message\n"
"Run \"harness run\" without arguments for its options.\n";

static i32 app_main(Os *os, Arena mem, Str *args, i32 nargs)
{
    Arena perm    = mem;
    Arena scratch = newarena(os_reserve(os, (iz)1<<30));
    if (!scratch.beg) {
        os_print(os, 2, "harness: out of memory\n");
        return 2;
    }

    if (nargs>1 && args[1]=="run") {
        return climain(os, perm, scratch, args, nargs, 0);
    } else if (nargs>1 && args[1]=="--headless") {
        return climain(os, perm, scratch, args, nargs, 1);
    }

    for (i32 i = 1; i < nargs; i++) {
        Str a = args[i];
        if (a == "--version") {
            os_print(os, 1, "harness " HARNESS_VERSION "\n");
            return 0;
        } else if (a == "--selftest") {
            Str url = i+1<nargs ? args[i+1] : Str{};
            i32 r   = url.len ? 0 : selftest(os, &perm, scratch);
            return r | selftestio(os, &perm, scratch, url);
        } else if (a == "-h" || a == "--help") {
            os_print(os, 1, usage);
            return 0;
        } else {
            os_print(os, 2, usage);
            return 1;
        }
    }
    os_print(os, 2, usage);
    return 1;
}
