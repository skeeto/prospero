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

// The edition (CMakeLists.txt): the itch.io builds are plain Prospero,
// and source builds, as the GitHub releases, the Community Edition
#if PROSPERO_ITCH
#  define PROSPERO_TITLE "Prospero"
#else
#  define PROSPERO_TITLE "Prospero Community Edition"
#endif
#define PROSPERO_ITCH_URL "https://skeeto.itch.io/prospero"


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

struct Listing {
    Dirent *head;
    iz      len;
    b32     ok;    // the directory could be read
    b32     more;  // entries past max were left out
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
// The same, the file's permissions mode (POSIX), as 0600 for a private
// file or 0700 for a script; Windows leaves them to its directory.
static b32     os_writemode(Os *, Arena, Str path, Str data, i32 mode);
// Create a directory and all its parents.
static b32     os_mkdirs(Os *, Arena, Str path);
// List a directory, excluding "." and "..", symlinks, and special files.
static Dirent *os_list(Os *, Arena *, Str path);
// List a directory for a person to pick from (path completion): as
// os_list, but symlinks are what they name, a link to a directory one
// too, only directories with dirs, and at most max entries, in no
// particular order. Broken links are left out.
static Listing os_listall(Os *, Arena *, Str path, b32 dirs, iz max);
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

// Listen at 127.0.0.1, on a port the system picks (*port), on a thread
// of its own, for an HTTP request for /callback, as a browser makes
// coming back from a sign-in: its target (path and query) is posted to
// mbox as a record of that type and id, and the browser told it may
// close the tab. After ms with none, the record is empty. 0 on failure.
static b32     os_loopback(Os *, Mailbox *, u32 type, u64 id, i32 ms, i32 *port);
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
    b32        holdin;    // Windows: input a pipe that sends nothing, held
                          // open until the Proc is freed, not NUL
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
// A program started apart from this one, to outlive it, as a terminal
// window is: neither waited for nor killed with it, its output nowhere,
// in its own session (POSIX) or console (Windows). argv[0] is found on
// env's PATH. Returns 0 on failure, explained by os_lasterr.
static b32     os_launch(Os *, Arena scratch, Slice<Str> argv, Str cwd, Slice<Str> env);

// (M3) Session locks: an exclusive lock on a file, created if missing,
// held until os_unlockfile or until the process ends, by any means. The
// handle is -1 if another process holds it, or the file cannot be had.
// os_lockheld tests without taking it, and the lock taken in a race with
// such a test is waited for briefly, so that a test never makes a lock
// fail.
static iz      os_lockfile(Os *, Arena, Str path);
static void    os_unlockfile(Os *, iz lock);
static b32     os_lockheld(Os *, Arena, Str path);

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

// (M8) Remove a file or a directory tree, never through a link: a
// symlink, or on Windows a junction or any other reparse point, is
// removed itself and never entered, so that nothing outside the tree is
// touched. Read-only files and directories go too. What is gone already,
// or goes meanwhile (another remover's), counts as removed. Returns 0 if
// anything is left.
static b32       os_removetree(Os *, Arena scratch, Str path);
// (M8) Copy a regular file, not through a link, to a new file. Returns
// 0 on failure, the target then gone, or if it existed already.
static b32       os_copyfile(Os *, Arena scratch, Str from, Str to);
// (M8) The directory for sessions' scratch directories, made if missing:
// prospero-EUID in $TMPDIR (if absolute) or /tmp, this user's alone
// (mode 0700) and no link; on Windows, prospero in GetTempPathW's. Null
// on failure, explained by os_lasterr.
static Str       os_scratchdir(Os *, Arena *);
// (M8) The system's temporary directories: $TMPDIR (if absolute) and
// /tmp, or GetTempPathW's on Windows, without a trailing separator.
static Slice<Str> os_tempdirs(Os *, Arena *);

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
// of a pick from the tray icon's menu (TRAY_*), gui_placed of the
// window moved, resized, maximized or restored (WinGeom, below), and
// gui_drop of a file dropped on the window, by its path, or opened in
// the application as a whole (macOS: Finder, the Dock, open -a), and
// gui_toasted and gui_toastfail of a desktop notification clicked or
// refused (os_guitoast).
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
// (M9) A launcher's activation token (Wayland's XDG_ACTIVATION_TOKEN),
// for the window that os_guiopen opens next to take the focus with.
// Ignored elsewhere.
static void    os_guitoken(Os *, Str token);
// (M9) Let another process take the foreground: this one, launched in
// front, hands off to that one's window (Windows). A no-op elsewhere.
static void    os_guiletfront(Os *, i64 pid);
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
// Desktop notifications (toasts): macOS's Notification Center (only as
// Prospero.app, asking the user's leave once, in os_guitoastprep),
// Windows' notification area (a balloon, which Windows 10 and later show
// as a toast, from an icon there while one is out), and a Linux
// desktop's org.freedesktop.Notifications. A toast of id, the core's,
// replaces any earlier one of that id, and alerts again; os_guiuntoast
// withdraws it. Returns 0 if none can be shown now. A click on one comes
// back through the event loop as gui_toasted, and a refusal found later,
// as the user's turning them off, as gui_toastfail (id -1 for all).
// The scripted checklist shows none, but with PROSPERO_TEST_TOAST.
// os_guitoastprep, as the window opens, says why none can be, if so.
static Str     os_guitoastprep(Os *, Arena *);
static b32     os_guitoast(Os *, Arena scratch, i32 id, Str title, Str body);
static void    os_guiuntoast(Os *, i32 id);
// Ask for the user's attention without a toast, as a toast refused
// does: the taskbar button flashes, the Dock's icon bounces, or the
// window is marked urgent. FLASH_CANCEL stops it.
enum {
    FLASH_CANCEL,
    FLASH_BRIEFLY,
    FLASH_UNTILFOCUSED,
};
static void    os_guiflash(Os *, i32 how);
// Whether the window has the keyboard focus, and is not minimized.
static b32     os_guifocused(Os *);

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
    GEV_DROP,       // a file, its path the text, is dropped on the window
    GEV_TOAST,      // toast x is clicked (y 0), or was refused (y 1),
                    // shown or not, by the platform's own path
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
    GKEY_F,
    GKEY_G,
    GKEY_T,
    GKEY_R,
    GKEY_UP,
    GKEY_DOWN,
    GKEY_V,
    GKEY_B,
    GKEY_2,
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
    b32    full; // full screen (SDL only), over max and box
};
// The displays' usable rectangles (without menu bars, docks and task
// bars), the primary first. Returns how many, at most max.
static i32     os_guidisplays(Os *, WinBox *, i32 max);
// The open window's geometry now.
static WinGeom os_guiwhere(Os *);
// Move the open window to a geometry, fitted as os_guiopen fits it.
static void    os_guiplace(Os *, WinGeom);
// (M8) Ask where to save a file, in the platform's own dialog, which
// never holds up the loop: the window goes on drawing while it is up.
// The path chosen, or an empty one if cancelled, comes to the mailbox
// as a record of the type with id, posted from whatever thread has the
// answer. The dialog has the title, and suggests the file name. Returns
// 0 if no dialog can be shown, when nothing is posted.
static b32     os_guisave(Os *, Arena scratch, Str title, Str name, Mailbox *,
                          u32 type, u64 id);
// (M9) Ask which file to open, of a kind (OPEN_*), likewise: the path
// chosen, or an empty one if cancelled, posted to the mailbox as a
// record of the type with id. Returns 0 if no dialog can be shown.
enum : i32 {
    OPEN_LOG,    // a session's raw log (*.jsonl)
    OPEN_ATTACH, // a file to attach: any, or an image (*.png, *.jpg, ...)
};
static b32     os_guiopenfile(Os *, Arena scratch, Str title, i32 kind,
                              Mailbox *, u32 type, u64 id);
// Ask which directory, likewise, the dialog starting in the directory
// at, if not null.
static b32     os_guiopendir(Os *, Arena scratch, Str title, Str at,
                             Mailbox *, u32 type, u64 id);
// An image the clipboard holds, to attach, as its bytes in a (PNG, JPEG,
// GIF, WebP or BMP), else a file it holds (a file manager's copy) as its
// path; both null if neither.
struct ClipImage {
    Str data;
    Str path;
};
static ClipImage os_guiclipimage(Os *, Arena *);
// Put a PNG on the clipboard, as an image (the checklist's paste).
// Returns 0 on failure.
static b32     os_guiclipput(Os *, Arena scratch, Str png);
// (M8) Show a directory in the platform's file manager: Finder,
// Explorer, or whatever opens directories. Returns 0 on failure.
static b32     os_guireveal(Os *, Arena scratch, Str dir);
// Show a file in the platform's file manager, selected in its directory:
// Finder, Explorer, or what answers org.freedesktop.FileManager1, else
// its directory as os_guireveal shows it. Returns 0 on failure, where
// the platform knows of it before the file manager takes over.
static b32     os_guishow(Os *, Arena scratch, Str file);
// Open a web page in the user's browser. Returns 0 on failure.
static b32     os_guiopenurl(Os *, Arena scratch, Str url);
// (M8) What the window system keeps from the window, as GUI_* flags.
// Wayland's compositor places windows, so a window neither knows nor
// sets its place, and cannot leave being minimized by itself
// (GUI_NOPLACE); it takes a new clipboard selection only with the serial
// of a real input event newer than the last one's, which no synthetic
// event has (GUI_NOSERIAL). The checklist allows for both.
enum : u32 {
    GUI_NOPLACE  = 1<<0,
    GUI_NOSERIAL = 1<<1,
};
static u32     os_guilimits(Os *);
#endif


#include "json.cpp"
#include "sse.cpp"
#include "ini.cpp"
#include "web.cpp"
#include "mailbox.cpp"
#include "events.cpp"
#include "frame.cpp"
#include "provider.cpp"
#include "anthropic.cpp"
#include "edit.cpp"
#include "png.cpp"
#include "image.cpp"
#include "zip.cpp"    // after png.cpp and image.cpp: zcrc32, wuffsinflate
#include "desktop.cpp"
#include "tools.cpp"
#include "guard.cpp"
#include "search.cpp"
#include "instructions.cpp"
#include "memory.cpp"
#include "skills.cpp"
#include "config.cpp"
#include "mcp.cpp"
#include "admit.cpp"
#include "compact.cpp"
#include "help.cpp"   // after config.cpp and mcp.cpp: cfgkinds, hexval
#include "engine.cpp"
#include "git.cpp"
#include "worktree.cpp"
#include "cli.cpp"
#include "host.cpp"
#include "door.cpp"
#include "deploy.cpp"
#include "remote.cpp"
#include "hostlink.cpp"
#include "selftest.cpp"
#include "selftest_io.cpp"
#include "eval.cpp"  // after selftest.cpp: tempdir, removeall


static Str usage =
"usage: prospero [options]\n"
#if PROSPERO_GUI
"       prospero [gui] [DIR] [options]\n"
#endif
"       prospero run [options] PROMPT\n"
"       prospero --headless [options]\n"
"       prospero serve [options]\n"
"       prospero attach [--reconnect] [options]\n"
"       prospero hosts [--state DIR]\n"
"       prospero connect NAME | --ssh DEST | --local [options]\n"
"       prospero deploy NAME|DEST [--force] [options]\n"
"       prospero eval [options] SUITE...\n"
"       prospero mcp list | auth NAME | logout NAME [options]\n"
#if PROSPERO_DESKTOP
"       prospero desktop COMMAND [ARGS]  (prospero desktop -h)\n"
#endif
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
    } else if (nargs>1 && args[1]=="mcp") {
        return mcpmain(os, perm, scratch, args, nargs);
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
