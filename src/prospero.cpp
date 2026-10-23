// prospero: a desktop coding harness for locally hosted and remote LLMs
//
// The program is the harness; the LLM driving it is the agent.
//
// Unity build: a platform layer (main_posix.cpp, main_win32.cpp,
// main_test.cpp) includes this file and implements the os_* API.
//   $ cmake -B build && cmake --build build && ctest --test-dir build
//
// This is free and unencumbered software released into the public domain.
#include "base.cpp"

#ifndef PROSPERO_VERSION
#  define PROSPERO_VERSION "0.0.0"
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
    PROC_DATA,      // (M3) os_spawnio: standard output bytes, in order
    HTTP_HEAD,      // i32 status, then raw "Name: value\r\n" header lines
    HTTP_BODY,      // response body bytes, in order
    HTTP_DONE,      // i32 error (HTTP_OK or HTTP_E*), then a description
    CONN_NEW,       // (M5) os_listen: a Conn pointer, a connection made
    CONN_DATA,      // (M5) bytes a connection received, in order
    CONN_EOF,       // (M5) the connection's input ended or broke
};

enum : i32 {
    HTTP_OK,
    HTTP_ECANCEL,   // os_httpcancel
    HTTP_ECONNECT,  // could not connect (includes TLS failures)
    HTTP_EIO,       // the connection failed after connecting
    HTTP_ETOOBIG,   // the response passed maxbytes
    HTTP_ETRUST,    // the server's certificate was refused: not trusted,
                    // expired, or for another name
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
    b32        breakaway; // Windows: its job lets processes leave it, as
                          // the programs the desktop helper starts do
};
// Null on failure, explained by os_lasterr. The process is not waited
// for, and its records may arrive before this returns.
static Proc   *os_spawn(Os *, Spawn *, Arena scratch);
// Soft: asks the tree to stop, SIGTERM to the process group (POSIX) or
// CTRL_BREAK to the console process group (Windows), which a process
// may ignore. Hard: kills the whole tree, including on POSIX whatever
// left the group (the reaper's sweep), or terminates the job. Prompt:
// neither waits for the processes.
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

// (M3) Absolute path with every symlink resolved (Windows: links and
// junctions, where the system can tell; else only made absolute). Null
// if missing, including a link to something missing.
static Str     os_realpath(Os *, Arena *, Str path);

// (M3) A child to talk to (the engine as a subprocess, later ssh), set
// up like os_spawn but with pipes for standard input and output. Its
// output arrives as PROC_DATA records, which wait for room in the
// mailbox and are never dropped, then PROC_EOF; standard error goes to
// outpath, or nowhere if empty; maxbytes is unused. PROC_EXIT arrives
// as with os_spawn.
static Proc   *os_spawnio(Os *, Spawn *, Arena scratch);
// (M3) Queue bytes for the child's standard input, written in order by
// a thread of the platform's, so this never waits on the child. A null
// string closes its input once everything before it is written, as does
// os_procfree. Returns 0 if the input is closed or broken (the child
// stopped reading), or too much is queued already.
static b32     os_procwrite(Os *, Proc *, Str);

// (M3) Session locks: an exclusive lock on a file, created if missing,
// held until os_unlockfile or until the process ends, by any means. The
// handle is -1 if another process holds it, or the file cannot be had.
// os_lockheld tests without taking it, and the lock taken in a race with
// such a test is waited for briefly, so that a test never makes a lock
// fail.
static iz      os_lockfile(Os *, Arena, Str path);
static void    os_unlockfile(Os *, iz lock);
static b32     os_lockheld(Os *, Arena, Str path);

static b32     os_openurl(Os *, Arena, Str url);
static void    os_busy(Os *, i32 state);

// (M5) Local endpoints: byte streams between processes of one user on
// one machine, an AF_UNIX socket (POSIX) or a named pipe (Windows) that
// nobody else may reach, nor anyone over the network. A connection works
// as os_spawnio's child does: what it receives arrives as CONN_DATA
// records, which wait for room in the mailbox and are never dropped,
// then one CONN_EOF, and os_connwrite queues bytes that a thread of the
// platform's writes in order, so that the caller never waits.
struct Conn;
struct Listener;
// Where the host of a state directory listens, under the given name,
// with the place made ready: a socket in its run directory, which only
// the owner may enter (through a short link in /tmp where that path is
// too long for a socket), or a pipe name for this user and directory.
// Null on failure, explained by os_lasterr.
static Str       os_endpoint(Os *, Arena *, Str statedir, Str name);
// Listen at an endpoint that no live process holds. Each connection made
// to it is posted as a CONN_NEW record whose payload is the Conn pointer,
// with a fresh id that its CONN_DATA and CONN_EOF records then carry.
// The caller owns a reference to each. Null on failure, explained by
// os_lasterr, as when another process holds the endpoint.
static Listener *os_listen(Os *, Arena scratch, Str endpoint, Mailbox *);
// Stop listening, and free the endpoint for another process.
static void      os_unlisten(Os *, Listener *);
// Connect to an endpoint, with records carrying id. Null if nobody
// listens there, or on failure (os_lasterr).
static Conn     *os_connect(Os *, Arena scratch, Str endpoint, Mailbox *, u64 id);
// Whether a process listens at the endpoint: connect, and hang up.
static b32       os_listening(Os *, Arena scratch, Str endpoint);
// Queue bytes for the connection, as os_procwrite: a null string ends
// the output once the queue is written. Returns 0 if the output is
// closed or broken, or too much is queued already.
static b32       os_connwrite(Os *, Conn *, Str);
// Break the connection at once, dropping whatever is queued, and drop
// the caller's reference. Its CONN_EOF follows, if not posted already.
static void      os_connclose(Os *, Conn *);

// (M5) Start a program detached from this process and from whatever
// would stop it with this one: in a session of its own between two
// forks (POSIX), or with no console, in a process group of its own and
// outside any job (Windows). Its input and output are null, and its
// errors are appended to errpath. Should this process's job forbid
// breaking away (an ssh session's), it starts in the job, attached. The
// pid is 0 on failure, explained by os_lasterr.
struct Detached {
    i32 pid;
    b32 attached;  // still in this process's job, and ending with it
};
static Detached  os_detach(Os *, Arena scratch, Slice<Str> argv, Str cwd, Str errpath);
// (M5) This process's id.
static i32       os_pid(Os *);
// (M7) Post a record of the given type to the mailbox whenever this
// process is asked to quit: SIGTERM, SIGINT or SIGHUP (POSIX), or its
// console's Ctrl+C, Ctrl+Break or closing (Windows). Call it before
// starting any thread, so that every thread leaves those to it. Returns
// 0 on failure.
static b32       os_onquit(Os *, Mailbox *, u32 type);
// macOS: the pid of the process responsible for pid, which TCC charges
// with its privacy access: its own for an app, else what started it, as
// Terminal. 0 elsewhere, or if unknown. (The selftest checks reapers by
// it: see reap.cpp.)
static i32       os_responsible(Os *, i32 pid);

// (M1) GUI builds: wake the GUI loop from any thread, coalesced.
static void    os_wake(Os *);
#if PROSPERO_GUI
// The platform owns the window, the renderer and the event loop. The
// core (app.cpp) decides when to draw (sched.cpp) and what (ui.cpp).
// The platform calls back into the core: gui_input after giving an
// input event to ImGui, gui_wake after clearing the wake flag,
// gui_force for resizes and exposes, gui_scale for DPI changes,
// gui_minimized, gui_modal around OS modal loops, gui_frame to draw
// from inside them, gui_systheme with the system's theme preference
// (dark or light) as the window opens and whenever it changes, and
// gui_close when the user asks to close the window or quit, which the
// core may decline: the window stays until os_guiclose. gui_tray tells
// of a pick from the tray icon's menu (TRAY_*), and gui_placed of the
// window moved, resized, maximized or restored (WinGeom, below).
struct Gui;
// Open the main window with ImGui's platform and renderer backends, or
// open it again after os_guished. Returns 0 on failure.
static b32     os_guiopen(Os *, Gui *, Str title);
// Wait up to us microseconds (forever if negative) for an event, then
// dispatch every pending one. Returns 0 if the window is gone.
static b32     os_guiwait(Os *, i64 us);
// Begin a frame in the backends, before ImGui::NewFrame.
static void    os_guinewframe(Os *);
// Render the frame's draw data over the clear color (packed as IM_COL32)
// and present it; or, when not presenting, only apply its texture
// updates.
static void    os_guipresent(Os *, b32 present, u32 clear);
// The display's refresh period in microseconds.
static i64     os_guirefresh(Os *);
// Bring the window back for the user: restored if minimized, and raised
// unless scripted (it would take the user's focus).
static void    os_guiraise(Os *);
// Make the window's frame (its title bar) dark or light to match the
// theme, where the platform can: Windows 10 1809 and later, and macOS.
// Returns 0 where it cannot, and before the window exists.
static b32     os_guitheme(Os *, b32 dark);
// Close the window, and everything with it.
static void    os_guiclose(Os *);
// (M5) Shed the window and its renderer, keeping the event loop, which
// os_guiwait goes on running, wakes included, for the tray icon and
// until os_guiopen opens a window again or os_guiclose ends it all.
static void    os_guished(Os *);
// (M5) A tray icon (Windows' notification area, macOS's menu bar, a
// Linux desktop's status notifier) with a menu of the TRAY_* items,
// and the tip shown over it. Returns 0 where there is none to be had.
enum {
    TRAY_OPEN,     // bring the window back
    TRAY_STOPALL,  // stop every session's work
    TRAY_QUIT,     // stop everything, and exit
};
static b32     os_trayopen(Os *, Str tip);
static void    os_trayclose(Os *);

// Synthetic input for the scripted checklist (gui --drive), posted to
// the window's own event queue so that it takes the path real input
// takes. Coordinates are in ImGui's units; GEV_RESIZE resizes the window
// to x by y.
enum {
    GEV_MOVE,
    GEV_BUTTON,  // button 0 left, 1 right
    GEV_WHEEL,   // y notches, positive up
    GEV_KEY,     // key GKEY_*, with ctrl held if set
    GEV_TEXT,
    GEV_RESIZE,
    GEV_MODALSIZE,  // a modal resize: Win32 by keyboard, x steps wider;
                    // SDL as x of the exposes a live resize sends
    GEV_DPI,        // Win32: the window moved to a monitor of x DPI
    GEV_CLOSE,      // the user asks to close the window
    GEV_MINIMIZE,   // the window is minimized
    GEV_SYSTHEME,   // the system's theme turns dark (x 1) or light (x 0),
                    // or back to what it really is (x -1)
    GEV_TRAY,       // the tray icon's menu item x (TRAY_*) is picked,
                    // tray or not, window or not
};
enum {
    GKEY_TAB,
    GKEY_CTRL,
    GKEY_ESCAPE,
    GKEY_ENTER,
    GKEY_C,
    GKEY_SHIFT,
    GKEY_A,
    GKEY_END,
    GKEY_CMD,    // macOS's Cmd (SDL only: elsewhere the core sends Ctrl)
    GKEY_EQUAL,
    GKEY_MINUS,
    GKEY_0,
};
struct GuiEvent {
    i32 kind;
    f32 x, y;
    i32 button;
    b32 down;
    i32 key;
    b32 ctrl;   // held with the key
    b32 shift;
    b32 cmd;    // macOS's Cmd held (SDL only)
    Str text;   // null-terminated, and must outlive the event's dispatch
};
static void    os_guiinject(Os *, GuiEvent *);

// (M8) Window geometry, in the platform's screen coordinates: SDL's
// (points on macOS) for the window's client area, Win32's pixels for
// its frame. The box is the window's when not maximized, which max
// says it is. The core keeps the latest in its settings, as gui_placed
// tells of changes, and os_guiopen opens the window there, fitted to
// the displays as they are then (place.cpp).
struct WinBox {
    i32 x, y, w, h;
};
struct WinGeom {
    WinBox box;
    b32    max;
    b32    ok;   // known: else the platform's own place and size
};
// The displays' usable rectangles (without menu bars, docks and task
// bars), the primary first. Returns how many, at most max.
static i32     os_guidisplays(Os *, WinBox *, i32 max);
// The open window's geometry now.
static WinGeom os_guiwhere(Os *);
// Move the open window to a geometry, fitted as os_guiopen fits it.
static void    os_guiplace(Os *, WinGeom);
#endif


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
#include "frame.cpp"
#include "provider.cpp"
#include "anthropic.cpp"
#include "edit.cpp"
#include "desktop.cpp"
#include "tools.cpp"
#include "guard.cpp"
#include "search.cpp"
#include "instructions.cpp"
#include "config.cpp"
#include "admit.cpp"
#include "compact.cpp"
#include "engine.cpp"
#include "cli.cpp"
#include "host.cpp"
#include "deploy.cpp"
#include "remote.cpp"
#include "selftest.cpp"
#include "selftest_io.cpp"
#include "eval.cpp"  // after selftest.cpp: tempdir, removeall


static Str usage =
"usage: prospero [options]\n"
"       prospero run [options] PROMPT\n"
"       prospero --headless [options]\n"
"       prospero serve [options]\n"
"       prospero attach [--reconnect] [options]\n"
"       prospero hosts [--state DIR]\n"
"       prospero connect NAME | --ssh DEST | --local [options]\n"
"       prospero deploy NAME|DEST [--force] [options]\n"
"       prospero eval [options] SUITE...\n"
"  --version         print the version and exit\n"
"  --selftest [URL]  run the platform integration tests, with HTTP\n"
"                    tests against a fake provider at URL\n"
"  -h                print this message\n"
"Run \"prospero run\" without arguments for its options.\n"
#if defined(_WIN32) && PROSPERO_GUI
// The GUI subsystem: an interactive console does not wait for it, nor
// show what it writes, unless that goes through a pipe or a file
"\n"
"prospero.exe is a GUI program, so at an interactive cmd or PowerShell\n"
"prompt it prints nothing and the prompt returns at once. Pipe or\n"
"redirect its output instead: prospero --version | more, or > out.txt.\n"
"Over ssh and in scripts it works as usual.\n"
#endif
;

static i32 app_main(Os *os, Arena mem, Str *args, i32 nargs)
{
    Arena perm    = mem;
    iz    cap     = sizeof(void *)==8 ? (iz)1<<30 : (iz)1<<27;  // 32-bit: XP
    Arena scratch = newarena(os_reserve(os, cap));
    if (!scratch.beg) {
        os_print(os, 2, "prospero: out of memory\n");
        return 2;
    }

    if (nargs>1 && args[1]=="run") {
        return climain(os, perm, scratch, args, nargs, 0);
    } else if (nargs>1 && args[1]=="--headless") {
        return climain(os, perm, scratch, args, nargs, 1);
    } else if (nargs>1 && args[1]=="--search") {
        return searchmain(os, perm, scratch, args, nargs);  // grep and glob
    } else if (nargs>1 && args[1]=="serve") {
        return servemain(os, perm, scratch, args, nargs);
    } else if (nargs>1 && args[1]=="attach") {
        return attachmain(os, perm, scratch, args, nargs);
    } else if (nargs>1 && args[1]=="hosts") {
        return hostsmain(os, perm, scratch, args, nargs);
    } else if (nargs>1 && args[1]=="connect") {
        return connectmain(os, perm, scratch, args, nargs);
    } else if (nargs>1 && args[1]=="deploy") {
        return deploymain(os, perm, scratch, args, nargs);
    } else if (nargs>1 && args[1]=="eval") {
        return evalmain(os, perm, scratch, args, nargs);
    }

    for (i32 i = 1; i < nargs; i++) {
        Str a = args[i];
        if (a == "--version") {
            os_print(os, 1, "prospero " PROSPERO_VERSION "\n");
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
