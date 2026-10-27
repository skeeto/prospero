// Desktop helper, macOS platform layer
// $ c++ -std=c++20 -o prospero-desktop src/main_desktop_macos.cpp
//       -framework ApplicationServices -framework Carbon -lobjc
//
// Plain C++ over the system's C interfaces: CoreGraphics lists windows
// (CGWindowList) and posts input (CGEvent), Accessibility (AXUIElement)
// raises, closes and moves windows, and Text Input Sources map keys. Two
// jobs have no C interface, the screenshot (ScreenCaptureKit, the only
// way left since macOS 15) and starting an application (NSWorkspace), so
// those go through the Objective-C runtime's C functions, their
// frameworks loaded at first use. Blocks (clang's -fblocks, on by
// default for Apple targets) serve as completion handlers.
//
// Keys go to the key window, so before any input aimed at a window, it
// is made key and every account of the focus that can be read must
// confirm it (dk_activate), else the command fails and sends nothing.
// The public ways can leave a newly launched application active with no
// key window, so the last way is the window server's private one, as
// window managers use.
//
// Coordinates are points, the unit every one of those interfaces uses:
// the global display space, from the top-left of the main display, y
// down. A Retina display has two pixels to the point (or another
// factor), and a shot is captured at full resolution, which the core
// scales down to the points at most, so a point of the screenshot maps
// back to the screen exactly. hello says "points".
//
// macOS asks the user's permission, by program, for two things: Screen
// Recording, to capture the screen (and to read other programs' window
// titles), and Accessibility, to post input and to control windows. It
// charges both to the helper's responsible process (notes/decisions.md):
// Prospero.app, when it was started as an app, else whatever started it,
// such as Terminal. Every call that needs one checks for it first, with
// the preflight functions, which never prompt, and fails at once naming
// what to grant to which app. Only "access -ask" prompts. The capture,
// the launch and every Accessibility message wait a bounded time, so an
// unresponsive system or program fails a call rather than hanging it.
//
// The screen is every online display, which includes a sleeping one, so
// geometry holds while displays sleep. A capture while the screen is
// locked or a display it covers sleeps fails saying so, without waking
// anything (readdesk, unseen).
//
// For tests, PROSPERO_TEST_DESKTOP_DENY lists permissions to treat as
// missing, "screen" and "accessibility", so that the error paths run
// without touching the screen. The DK_SELFTEST build runs this layer's
// tests (test/desktop_macos_test.cpp) instead of the helper.
//
// This is free and unencumbered software released into the public domain.
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>  // Text Input Sources, UCKeyTranslate
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#undef assert  // the system's, from <assert.h>: base.cpp has its own
#include "base.cpp"
#include "wide.cpp"
#include "png.cpp"
#include "deskhelp.cpp"

extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void *);
extern char **environ;

enum : i32 {
    PERM_SCREEN = 1 << 0,  // Screen Recording
    PERM_AX     = 1 << 1,  // Accessibility
};

enum : i64 {
    MAC_CAPTUREMS = 10000,  // longest screenshot
    MAC_LAUNCHMS  = 30000,  // longest application start
    MAC_DOUBLEMS  = 500,    // clicks this close make a double click
};

// Holds still while a command reads its fields: the core's arena is
// reset per command, but a completion handler that comes after its wait
// gave up still writes to these, so they live in perm, never reset.
struct MacWait {
    dispatch_semaphore_t done;
    CGImageRef           image;
    b32                  ok;
    i32                  pid;
    char                 err[256];
};

struct Click {
    i32 button;
    i32 x, y;
    i64 ms;
    i32 count;
};

// The displays and the session as a capture sees them (readdesk). Online
// displays, not just active ones: a sleeping display is online but not
// active, and its bounds still hold, so the screen keeps its geometry
// while the display sleeps.
struct Display {
    DkRect rect;
    b32    asleep;
};

struct Desk {
    Display disp[32];
    i32     len;
    b32     locked;  // the session's screen is locked
};

struct Dk {
    Arena  perm;      // what outlives a command: messages, MacWait
    u32    deny;      // PERM_* treated as missing (a test hook)
    u32    allow;     // PERM_* treated as granted (a test hook)
    Desk  *fixed;     // displays in place of the live ones (a test hook)
    Str    why;       // the latest failure's reason, for dk_why
    Str    granted;   // who permissions are granted to, for messages
    Str    missing[2];  // each permission's message, made once
    i32    held;      // mouse button down, -1 if none
    b32    at;        // the pointer was put somewhere
    CGPoint pos;      // where
    Click  click;     // the latest button press, for click counts
    i32    keymap[128];  // ASCII to key code | shift<<8 | 1<<9, if any
    b32    keyready;
    b32    sck;       // ScreenCaptureKit is loaded
    b32    appkit;    // AppKit is loaded
    i32  (*axwinid)(AXUIElementRef, CGWindowID *);  // private, if any
    i32  (*respfor)(i32);                           // likewise
    i32  (*psnfor)(i32, u32 *);         // GetProcessForPID, deprecated
    i32  (*pidfor)(u32 *, i32 *);       // GetProcessPID, likewise
    i32  (*wsfront)(u32 *);             // SkyLight's front process, private
    i32  (*setfront)(u32 *, u32, u32);  // SkyLight's, private
    i32  (*postrec)(u32 *, u8 *);       // likewise
};

static b32 writeall(i32 fd, Str s)
{
    for (iz off = 0; off < s.len;) {
        iz len = s.len-off<(1<<30) ? s.len-off : (1<<30);
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
    writeall(2, "prospero-desktop: out of memory\n");
    _exit(DK_FAIL);
}

#if PROSPERO_ARENACHECK
static void os_arenafault(Str msg)
{
    writeall(2, "prospero-desktop: ");
    writeall(2, msg);
}
#endif

static iz pagesize()
{
    return (iz)sysconf(_SC_PAGESIZE);
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

static Region *reserve(iz cap)
{
    iz page = pagesize();
    cap += -cap & (page - 1);
    byte *p = (byte *)mmap(0, (uz)cap, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (p==MAP_FAILED || mprotect(p, (uz)page, PROT_READ|PROT_WRITE)) {
        return 0;
    }
    iz      hdr = (iz)sizeof(Region) + (-(iz)sizeof(Region) & 63);
    Region *r   = (Region *)p;
    r->base      = p + hdr;
    r->cap       = cap - hdr;
    r->committed = page - hdr;
    return r;
}

static char *cstr(Arena *a, Str s)
{
    char *r = alloc<char>(a, s.len+1);
    copybytes(r, s.data, s.len);
    return r;
}

// True if a string cannot cross the system call boundary intact.
static b32 hasnul(Str s)
{
    return cut(s, 0).ok;
}

static Str fromcstr(char const *s)
{
    Str r = {};
    r.data = (u8 *)s;
    for (; s && s[r.len]; r.len++) {}
    return r;
}


// Pure parts, which the self-test covers

// A rectangle of points, rounded to whole points, as CoreGraphics gives
// them: window frames are whole already, displays too.
static i32 ptround(f64 v)
{
    v = v<-(1<<30) ? -(1<<30) : v>(1<<30) ? (1<<30) : v;
    return (i32)(v<0 ? -(i64)(0.5 - v) : (i64)(v + 0.5));
}

static DkRect rectof(CGRect r)
{
    DkRect d = {};
    d.left   = ptround(r.origin.x);
    d.top    = ptround(r.origin.y);
    d.right  = ptround(r.origin.x + r.size.width);
    d.bottom = ptround(r.origin.y + r.size.height);
    return d;
}

// Every display together, empty if none.
static DkRect deskrect(Desk *d)
{
    DkRect r = {};
    for (i32 i = 0; i < d->len; i++) {
        DkRect e = d->disp[i].rect;
        if (!i) {
            r = e;
            continue;
        }
        r.left   = e.left   < r.left   ? e.left   : r.left;
        r.top    = e.top    < r.top    ? e.top    : r.top;
        r.right  = e.right  > r.right  ? e.right  : r.right;
        r.bottom = e.bottom > r.bottom ? e.bottom : r.bottom;
    }
    return r;
}

// Why the rectangle cannot be captured, without waking anything to
// find out, else null: the screen is locked, or a display it touches
// sleeps, and either would capture black or nothing.
static Str unseen(Desk *d, DkRect r)
{
    if (d->locked) {
        return "cannot capture: the screen is locked";
    }
    for (i32 i = 0; i < d->len; i++) {
        DkRect e = d->disp[i].rect;
        b32 meets = e.left<r.right && r.left<e.right &&
                    e.top<r.bottom && r.top<e.bottom;
        if (meets && d->disp[i].asleep) {
            return "cannot capture: the display is asleep";
        }
    }
    return {};
}

static CGRect cgrect(DkRect r)
{
    CGRect c = {};
    c.origin.x    = r.left;
    c.origin.y    = r.top;
    c.size.width  = r.right - r.left;
    c.size.height = r.bottom - r.top;
    return c;
}

// Window levels (CGWindowLevel.h): the menu bar, its status items and
// the Dock are the system's, not windows to act on, nor are those under
// the normal level (the desktop) or at the screen saver's and above.
enum : i32 {
    LEVEL_DOCK     = 20,
    LEVEL_MAINMENU = 24,
    LEVEL_STATUS   = 25,
    LEVEL_POPUP    = 101,  // menus; tooltips and overlays sit higher
    LEVEL_TOP      = 1000,
};

static b32 listed(i32 level, f64 alpha, DkRect r, Str owner)
{
    return level>=0 && level<LEVEL_TOP && level!=LEVEL_DOCK &&
           level!=LEVEL_MAINMENU && level!=LEVEL_STATUS && alpha>0 &&
           r.right>r.left && r.bottom>r.top && !(owner == "Window Server");
}

static b32 popuplevel(i32 level)
{
    return level >= 100;
}

// Whether a button press continues a multiple click: the same button,
// near the same place, soon enough. Returns the click count.
static i32 clickcount(Click *c, i32 button, i32 x, i32 y, i64 now)
{
    i32 dx   = x - c->x;
    i32 dy   = y - c->y;
    b32 more = c->count && c->button==button && now-c->ms<=MAC_DOUBLEMS &&
               dx>=-4 && dx<=4 && dy>=-4 && dy<=4;
    c->count  = more ? c->count+1 : 1;
    c->button = button;
    c->x      = x;
    c->y      = y;
    c->ms     = now;
    return c->count;
}

// Who has the keyboard: the frontmost application and its focused
// window, which is the key window. Keys go to the key window, and an
// application active with none, as a launch can leave one, discards
// them all.
//
// Not every account of it can be believed. As observed live on macOS 27
// (Darwin 27), with TextEdit made active and a typed "x" landing in its
// document, while its window plainly showed as key:
// - the window's own AXFocused read 0, so it cannot be required;
// - the system-wide AXFocusedApplication and AXFocusedUIElement failed
//   with kAXErrorCannotComplete (-25204), so they are unknown, never a
//   failure, and only a contradiction when they do answer;
// - what did read true: NSWorkspace's frontmost application and the
//   window server's front process were TextEdit, TextEdit's own
//   AXFocusedWindow was the window, and TextEdit's own AXFocusedUIElement
//   (asked of the application, not system-wide), a text area, lay in it.
// So those decide, each that can be read, and any that disagrees means
// no input is sent.
struct Focus {
    u32 pid;       // frontmost by NSWorkspace (else Accessibility), 0 if unknown
    u32 wspid;     // the window server's front process, 0 if unknown
    u32 axpid;     // Accessibility's focused application, 0 if unknown
    u64 win;       // the frontmost application's focused window, 0 if none
    b32 attached;  // that window is a sheet of the target
    b32 astray;    // its focused element is known to lie outside the target
    Str app;       // names, for messages
    Str title;
};

// Whether the window id of process pid has the keyboard, or one of its
// sheets has, which takes keys in the window's place. With no id, the
// window has no Accessibility element, as a menu has not, and its
// application frontmost is as near as it gets.
static b32 haskeyboard(Focus *f, u32 pid, u64 id)
{
    if (!f->pid || f->pid!=pid) {
        return 0;
    } else if ((f->wspid && f->wspid!=pid) || (f->axpid && f->axpid!=pid)) {
        return 0;  // the readable accounts disagree
    } else if (!id) {
        return 1;
    } else if (f->win!=id && !f->attached) {
        return 0;
    }
    return !f->astray;
}

// Why a window did not get the keyboard, from what has it instead.
static Str unfocused(Arena *a, Focus *f, DkWin *w)
{
    Buf b(a, 256 + w->title.len + w->cls.len + f->app.len + f->title.len);
    print(&b, "cannot give the keyboard to the window '");
    print(&b, w->title);
    print(&b, "' of ");
    print(&b, w->cls.len ? w->cls : Str("pid"));
    print(&b, " (");
    print(&b, (i64)w->pid);
    print(&b, "): ");
    if (!f->pid) {
        print(&b, "no application is frontmost");
    } else if (f->pid != w->pid) {
        print(&b, f->app.len ? f->app : Str("another application"));
        print(&b, " (");
        print(&b, (i64)f->pid);
        print(&b, ") is frontmost");
        if (f->win) {
            print(&b, ", its window '");
            print(&b, f->title);
            print(&b, "' focused");
        }
    } else if (f->wspid && f->wspid!=w->pid) {
        print(&b, "it is frontmost, but the window server has pid ");
        print(&b, (i64)f->wspid);
        print(&b, " in front");
    } else if (f->axpid && f->axpid!=w->pid) {
        print(&b, "it is frontmost, but Accessibility has pid ");
        print(&b, (i64)f->axpid);
        print(&b, " focused");
    } else if (!f->win) {
        print(&b, "it is frontmost, but with no window focused");
    } else if (f->win!=w->id && !f->attached) {
        print(&b, "it is frontmost, but its window '");
        print(&b, f->title);
        print(&b, "' (0x");
        printhex(&b, f->win, 8);
        print(&b, ") is focused");
    } else {
        print(&b, "the window is focused, but its focused element is outside it");
    }
    print(&b, "; no input was sent");
    return finish(&b);
}

// Virtual key codes (HIToolbox's kVK_*), which name physical keys. Mac
// keyboards label backspace "delete", so "delete" is that key, as a Mac
// user means it, and "del" the forward delete, as on Windows. Command is
// "cmd", and also "win", "super" and "meta", the same key's place.
static i32 namedkey(Str k)
{
    static struct { Str name; i32 code; } const names[] = {
        {"cmd",   0x37}, {"command", 0x37}, {"super",  0x37},
        {"win",   0x37}, {"meta",    0x37}, {"shift",  0x38},
        {"alt",   0x3A}, {"option",  0x3A}, {"opt",    0x3A},
        {"ctrl",  0x3B}, {"control", 0x3B}, {"fn",     0x3F},
        {"enter", 0x24}, {"return",  0x24}, {"tab",    0x30},
        {"space", 0x31}, {"backspace", 0x33}, {"bs",   0x33},
        {"delete", 0x33}, {"esc",    0x35}, {"escape", 0x35},
        {"del",   0x75}, {"forwarddelete", 0x75},
        {"ins",   0x72}, {"insert",  0x72}, {"help",   0x72},
        {"home",  0x73}, {"end",     0x77}, {"pgup",   0x74},
        {"pageup", 0x74}, {"pgdn",   0x79}, {"pagedown", 0x79},
        {"left",  0x7B}, {"right",   0x7C}, {"down",   0x7D},
        {"up",    0x7E},
    };
    for (iz i = 0; i < countof(names); i++) {
        if (equalfold(k, names[i].name)) {
            return names[i].code;
        }
    }
    static i32 const fkeys[] = {
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D,
        0x67, 0x6F, 0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A,
    };
    if (k.len>=2 && k.len<=3 && lowercase(k[0])=='f') {
        Int n = parseint(cuthead(k, 1));
        if (n.ok && n.val>=1 && n.val<=countof(fkeys)) {
            return fkeys[n.val-1];
        }
    }
    return -1;
}

// The modifier flag a key holds down, 0 if it is no modifier.
static u64 modflag(i32 code)
{
    switch (code) {
    case 0x37: case 0x36: return kCGEventFlagMaskCommand;
    case 0x38: case 0x3C: return kCGEventFlagMaskShift;
    case 0x3A: case 0x3D: return kCGEventFlagMaskAlternate;
    case 0x3B: case 0x3E: return kCGEventFlagMaskControl;
    case 0x3F:            return kCGEventFlagMaskSecondaryFn;
    }
    return 0;
}

// The US layout, for when the current one cannot be read: the keys of
// printable ASCII, unshifted then shifted, by code.
static void uskeymap(i32 *map)
{
    static char const plain[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0\0`";
    static char const shift[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>\0\0~";
    for (i32 i = 0; i < 128; i++) {
        map[i] = 0;
    }
    for (i32 code = 0; code < countof(plain)-1; code++) {
        u8 c = (u8)plain[code];
        u8 s = (u8)shift[code];
        if (c) map[c] = code | 1<<9;
        if (s) map[s] = code | 1<<8 | 1<<9;
    }
    map[' '] = 0x31 | 1<<9;
}

// The key code of a named key or one ASCII character, and whether it
// needs shift, from map; -1 if none. Letters never imply shift.
static i32 keyfor(i32 *map, Str k, b32 *shift)
{
    *shift = 0;
    i32 code = namedkey(k);
    if (code >= 0) {
        return code;
    }
    u8 c = 0;
    if (equalfold(k, "plus")) {
        c = '=';  // the + key, unshifted, as Windows' VK_OEM_PLUS
    } else if (equalfold(k, "minus")) {
        c = '-';
    } else if (equalfold(k, "comma")) {
        c = ',';
    } else if (k.len==1 && k[0]>' ' && k[0]<0x7f) {
        c = letter(k[0]) ? lowercase(k[0]) : k[0];
    } else {
        return -1;
    }
    i32 v = map[c];
    if (!(v & 1<<9)) {
        return -1;
    }
    *shift = (v>>8) & 1;
    return v & 0xff;
}

// The app a permission is granted to, from its responsible process's
// executable: the innermost bundle holding it, else the executable.
static Str appof(Str exe)
{
    for (iz i = exe.len; i >= 5; i--) {
        Str head = takehead(exe, i);
        if (endswith(head, ".app") && (i==exe.len || exe[i]=='/')) {
            return head;
        }
    }
    return exe;
}

// What to grant to, for messages: "Prospero.app (/path/Prospero.app)".
// A helper that answers for itself, run disclaimed, is charged as a
// program of its own, though it sits in Prospero.app.
static Str grantee(Arena *a, Str exe, b32 self = 0)
{
    if (!exe.len) {
        return "Prospero.app";
    } else if (self) {
        return exe;
    }
    Str app  = appof(exe);
    Str name = app;
    if (!endswith(app, ".app")) {
        return app;  // a program of no bundle, such as sshd
    }
    for (iz i = app.len-1; i >= 0; i--) {
        if (app[i] == '/') {
            name = cuthead(app, i+1);
            break;
        }
    }
    Buf b(a, app.len + name.len + 4);
    print(&b, name);
    print(&b, " (");
    print(&b, app);
    print(&b, ")");
    return finish(&b);
}

static Str permname(i32 perm)
{
    return perm==PERM_SCREEN ? Str("Screen Recording") : Str("Accessibility");
}

// The message for a permission missing, which names what to grant it
// to and where.
static Str permmsg(Arena *a, i32 perm, Str to)
{
    Buf b(a, 256);
    print(&b, permname(perm));
    print(&b, " permission is missing: grant it to ");
    print(&b, to);
    print(&b, " in System Settings > Privacy & Security > ");
    print(&b, permname(perm));
    if (perm == PERM_SCREEN) {
        print(&b, ", then quit and reopen it");
    }
    return finish(&b);
}

// The test hook's permissions: a list of "screen" and "accessibility".
static u32 parsedeny(Str s)
{
    u32 r = 0;
    for (Str rest = s; rest.len;) {
        Cut c = cut(rest, ',');
        Str p = trim(c.head);
        rest  = c.tail;
        r |= p=="screen" ? PERM_SCREEN : p=="accessibility" ? PERM_AX : 0;
    }
    return r;
}

// Where an application named by run may be, by name: a bundle path, a
// name in the usual folders, with or without .app. Empty for a path to
// something else, or a name that is no app's.
static Slice<Str> appplaces(Arena *a, Str name, Str home)
{
    Slice<Str> r = {};
    while (name.len>1 && name[name.len-1]=='/') {
        name.len--;
    }
    if (cut(name, '/').ok) {
        if (endswith(name, ".app")) {
            r = push(a, r, name);
        }
        return r;
    }
    if (!name.len) {
        return r;
    }
    Str file = endswith(name, ".app") ? name : concat(a, name, ".app");
    static Str const dirs[] = {
        "/Applications", "/System/Applications",
        "/System/Applications/Utilities", "/Applications/Utilities",
        "/System/Library/CoreServices",
    };
    for (iz i = 0; i < countof(dirs); i++) {
        r = push(a, r, join(a, dirs[i], file));
    }
    if (home.len) {
        r = push(a, r, join(a, join(a, home, "Applications"), file));
    }
    return r;
}


// Core Foundation

static i64 cfint(CFDictionaryRef d, CFStringRef key)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    i64       r = 0;
    if (v && CFGetTypeID(v)==CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &r);
    }
    return r;
}

static f64 cfreal(CFDictionaryRef d, CFStringRef key, f64 otherwise)
{
    CFTypeRef v = CFDictionaryGetValue(d, key);
    f64       r = otherwise;
    if (v && CFGetTypeID(v)==CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)v, kCFNumberFloat64Type, &r);
    }
    return r;
}

// A CFString as UTF-8, empty if it is none.
static Str cfstr(Arena *a, CFTypeRef v)
{
    Str r = {};
    if (!v || CFGetTypeID(v)!=CFStringGetTypeID()) {
        return r;
    }
    CFStringRef s   = (CFStringRef)v;
    CFIndex     n   = CFStringGetLength(s);
    CFIndex     max = CFStringGetMaximumSizeForEncoding(n, kCFStringEncodingUTF8);
    CFIndex     len = 0;
    r.data = allocbytes(a, max);
    CFStringGetBytes(s, CFRangeMake(0, n), kCFStringEncodingUTF8, '?', 0,
                     r.data, max, &len);
    r.len = len;
    return r;
}

// A new CFString, null if s holds no valid UTF-8.
static CFStringRef newcfstr(Str s)
{
    return CFStringCreateWithBytes(0, s.data, (CFIndex)s.len,
                                   kCFStringEncodingUTF8, 0);
}


// Permissions

static b32 granted(Dk *dk, i32 perm)
{
    if (dk->deny & perm) {
        return 0;
    } else if (dk->allow & perm) {
        return 1;
    }
    return perm==PERM_SCREEN ? CGPreflightScreenCaptureAccess() : AXIsProcessTrusted();
}

// Whether a permission is there, else the reason why not.
static b32 needperm(Dk *dk, i32 perm)
{
    if (granted(dk, perm)) {
        return 1;
    }
    Str *m = &dk->missing[perm==PERM_AX];
    if (!m->data) {
        *m = permmsg(&dk->perm, perm, dk->granted);
    }
    dk->why = *m;
    return 0;
}

// The executable of the process responsible for this one, which the
// permissions are charged to; empty if unknown.
static Str responsible(Dk *dk, Arena *a, b32 *self)
{
    Str r = {};
    if (!dk->respfor) {
        return r;
    }
    i32 pid = dk->respfor(getpid());
    if (pid <= 0) {
        return r;
    }
    *self = pid == getpid();
    r.data = allocbytes(a, PROC_PIDPATHINFO_MAXSIZE);
    i32 n  = proc_pidpath(pid, r.data, PROC_PIDPATHINFO_MAXSIZE);
    r.len  = n>0 ? n : 0;
    return r;
}

static b32 dk_access(Dk *dk, Buf *b, b32 ask)
{
    static i32 const perms[] = {PERM_SCREEN, PERM_AX};
    static Str const names[] = {"screen-recording", "accessibility"};
    b32 all = 1;
    for (i32 i = 0; i < countof(perms); i++) {
        b32 ok = granted(dk, perms[i]);
        if (!ok && ask && !(dk->deny & perms[i])) {
            // Each shows the system's prompt, or opens the settings
            if (perms[i] == PERM_SCREEN) {
                CGRequestScreenCaptureAccess();
            } else {
                void const *k[] = {kAXTrustedCheckOptionPrompt};
                void const *v[] = {kCFBooleanTrue};
                CFDictionaryRef o = CFDictionaryCreate(0, k, v, 1,
                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                AXIsProcessTrustedWithOptions(o);
                CFRelease(o);
            }
            ok = granted(dk, perms[i]);
        }
        all &= ok;
        print(b, names[i]);
        print(b, ok ? Str("\tgranted\n") : Str("\tmissing\n"));
    }
    print(b, "granted-to\t");
    print(b, dk->granted);
    putbyte(b, '\n');
    return all;
}

static Str dk_why(Dk *dk)
{
    Str r = dk->why;
    dk->why = {};
    return r;
}

// Always a desktop: each call asks for the permission it needs (dk_why).
static Str dk_ready(Dk *)
{
    return {};
}


// Objective-C, through the runtime's C functions

static b32 loadfw(b32 *done, char const *path)
{
    if (!*done) {
        *done = dlopen(path, RTLD_LAZY|RTLD_LOCAL) != 0;
    }
    return *done;
}

static id cls(char const *name)
{
    return (id)objc_getClass(name);
}

template<typename R, typename... A>
static R msg(id obj, char const *sel, A... args)
{
    return ((R (*)(id, SEL, A...))objc_msgSend)(obj, sel_registerName(sel), args...);
}

static b32 responds(id obj, char const *sel)
{
    return obj && class_respondsToSelector(object_getClass(obj), sel_registerName(sel));
}

// An NSError's description into a buffer, in a completion handler.
static void errcopy(char *dst, iz cap, id err)
{
    id          s = err ? msg<id>(err, "localizedDescription") : (id)0;
    char const *c = s ? msg<char const *>(s, "UTF8String") : "unknown error";
    iz          n = 0;
    for (; c && c[n] && n<cap-1; n++) {
        dst[n] = c[n];
    }
    dst[n] = 0;
}

static MacWait *newwait(Dk *dk)
{
    MacWait *w = alloc<MacWait>(&dk->perm);
    w->done = dispatch_semaphore_create(0);
    return w;
}

static b32 waitfor(MacWait *w, i64 ms)
{
    return !dispatch_semaphore_wait(w->done, dispatch_time(DISPATCH_TIME_NOW, ms*1000000));
}

static Str failed(Dk *dk, Str what, char const *err)
{
    return concat(&dk->perm, what, fromcstr(err));
}


// Windows

static Str dk_os(Dk *)
{
    return "macos";
}

static Str dk_units(Dk *)
{
    return "points";
}

static DkWin describe(Arena *a, CFDictionaryRef d)
{
    DkWin  r = {};
    CGRect b = {};
    CFTypeRef bounds = CFDictionaryGetValue(d, kCGWindowBounds);
    if (bounds && CFGetTypeID(bounds)==CFDictionaryGetTypeID()) {
        CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)bounds, &b);
    }
    r.id    = (u32)cfint(d, kCGWindowNumber);
    r.pid   = (u32)cfint(d, kCGWindowOwnerPID);
    r.rect  = rectof(b);
    r.title = cfstr(a, CFDictionaryGetValue(d, kCGWindowName));
    r.cls   = cfstr(a, CFDictionaryGetValue(d, kCGWindowOwnerName));
    r.level = (i32)cfint(d, kCGWindowLayer);
    return r;
}

// An application's Accessibility window of a window id, retained, or
// null. Through the private _AXUIElementGetWindow, as every window
// manager does, since nothing public links the two.
static AXUIElementRef axwindow(Dk *dk, u32 pid, u64 id)
{
    if (!dk->axwinid || !granted(dk, PERM_AX)) {
        return 0;
    }
    AXUIElementRef app  = AXUIElementCreateApplication((pid_t)pid);
    CFTypeRef      wins = 0;
    AXUIElementRef r    = 0;
    if (!AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) &&
            CFGetTypeID(wins)==CFArrayGetTypeID()) {
        CFArrayRef list = (CFArrayRef)wins;
        for (CFIndex i = 0; !r && i < CFArrayGetCount(list); i++) {
            AXUIElementRef w   = (AXUIElementRef)CFArrayGetValueAtIndex(list, i);
            CGWindowID     wid = 0;
            if (!dk->axwinid(w, &wid) && wid==id) {
                r = (AXUIElementRef)CFRetain(w);
            }
        }
    }
    if (wins) CFRelease(wins);
    CFRelease(app);
    return r;
}

// Window titles need Screen Recording; without it, they come from
// Accessibility, one application at a time, if that is granted.
static void axtitles(Dk *dk, Arena *a, Slice<DkWin> ws)
{
    if (!dk->axwinid || granted(dk, PERM_SCREEN) || !granted(dk, PERM_AX)) {
        return;
    }
    for (iz i = 0; i < ws.len; i++) {
        if (ws[i].title.len || ws[i].level) {
            continue;
        }
        b32 seen = 0;
        for (iz j = 0; j < i && !seen; j++) {
            seen = ws[j].pid==ws[i].pid && !ws[j].level;
        }
        if (seen) {
            continue;  // that app's windows were looked up already
        }
        AXUIElementRef app  = AXUIElementCreateApplication((pid_t)ws[i].pid);
        CFTypeRef      wins = 0;
        if (!AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) &&
                CFGetTypeID(wins)==CFArrayGetTypeID()) {
            CFArrayRef list = (CFArrayRef)wins;
            for (CFIndex k = 0; k < CFArrayGetCount(list); k++) {
                AXUIElementRef w   = (AXUIElementRef)CFArrayGetValueAtIndex(list, k);
                CGWindowID     wid = 0;
                CFTypeRef      t   = 0;
                if (dk->axwinid(w, &wid) ||
                        AXUIElementCopyAttributeValue(w, kAXTitleAttribute, &t)) {
                    continue;
                }
                for (iz j = i; j < ws.len; j++) {
                    if (ws[j].id==wid && !ws[j].title.len) {
                        ws[j].title = cfstr(a, t);
                    }
                }
                CFRelease(t);
            }
        }
        if (wins) CFRelease(wins);
        CFRelease(app);
    }
}

static Slice<DkWin> dk_windows(Dk *dk, Arena *a, b32 all)
{
    CGWindowListOption opt = all ? kCGWindowListOptionAll
                                 : kCGWindowListOptionOnScreenOnly |
                                   kCGWindowListExcludeDesktopElements;
    CFArrayRef   list = CGWindowListCopyWindowInfo(opt, kCGNullWindowID);
    Slice<DkWin> r    = {};
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        DkWin           w = describe(a, d);
        f64             alpha = cfreal(d, kCGWindowAlpha, 1);
        if (all || listed(w.level, alpha, w.rect, w.cls)) {
            r = push(a, r, w);
        }
    }
    if (list) CFRelease(list);
    axtitles(dk, a, r);
    return r;
}

static DkWin dk_window(Dk *dk, Arena *a, u64 id)
{
    DkWin r = {};
    if (!id || id>0xffffffff) {
        return r;
    }
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                 (CGWindowID)id);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        DkWin w = describe(a, (CFDictionaryRef)CFArrayGetValueAtIndex(list, i));
        if (w.id == id) {
            r = w;
            break;
        }
    }
    if (list) CFRelease(list);
    if (r.id) {
        axtitles(dk, a, {&r, 1, 1});
    }
    return r;
}

// The displays and the session's lock, read without waking a display.
static Desk readdesk(Dk *dk)
{
    if (dk->fixed) {
        return *dk->fixed;
    }
    Desk              r = {};
    CGDirectDisplayID ids[countof(r.disp)];
    u32               n = 0;
    if (CGGetOnlineDisplayList((u32)countof(ids), ids, &n)) {
        n = 0;
    }
    for (u32 i = 0; i < n; i++) {
        if (CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) {
            continue;  // the same points as the display it mirrors
        }
        Display *d = r.disp + r.len++;
        d->rect   = rectof(CGDisplayBounds(ids[i]));
        d->asleep = CGDisplayIsAsleep(ids[i]) != 0;
    }
    CFStringRef     key = CFSTR("CGSSessionScreenIsLocked");
    CFDictionaryRef s   = CGSessionCopyCurrentDictionary();
    if (s) {
        CFTypeRef v = CFDictionaryGetValue(s, key);
        if (v && CFGetTypeID(v)==CFBooleanGetTypeID()) {
            r.locked = CFBooleanGetValue((CFBooleanRef)v);
        } else {
            r.locked = cfint(s, key) != 0;
        }
        CFRelease(s);
    }
    return r;
}

// Every display together, asleep or awake.
static DkWin dk_screen(Dk *dk)
{
    Desk  d = readdesk(dk);
    DkWin r = {};
    r.rect  = deskrect(&d);
    r.title = "screen";
    return r;
}

static Str axtitle(Arena *a, AXUIElementRef e)
{
    CFTypeRef t = 0;
    Str       r = {};
    if (!AXUIElementCopyAttributeValue(e, kAXTitleAttribute, &t)) {
        r = cfstr(a, t);
        CFRelease(t);
    }
    return r;
}

// A boolean attribute: 1, 0, or -1 if it cannot be read.
static i32 axflag(AXUIElementRef e, CFStringRef attr)
{
    CFTypeRef v = 0;
    i32       r = -1;
    if (!AXUIElementCopyAttributeValue(e, attr, &v)) {
        if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            r = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        }
        CFRelease(v);
    }
    return r;
}

// Whether element e lies outside the window target and its sheets: up
// from e's window (AXWindow), else from e itself, through its parents.
// Reaching the target means inside, another window means outside, and
// the top reached with neither, as from the application, means unknown,
// which is not outside.
static b32 outside(AXUIElementRef e, AXUIElementRef target)
{
    CFTypeRef cur = 0;
    if (AXUIElementCopyAttributeValue(e, kAXWindowAttribute, &cur)) {
        cur = CFRetain(e);
    }
    i32 r = -1;
    for (i32 depth = 0; r<0 && cur && depth<64; depth++) {
        AXUIElementRef el   = (AXUIElementRef)cur;
        CFTypeRef      role = 0;
        CFTypeRef      next = 0;
        if (CFEqual(cur, target)) {
            r = 0;
        } else if (!AXUIElementCopyAttributeValue(el, kAXRoleAttribute, &role) &&
                   CFEqual(role, kAXWindowRole)) {
            r = 1;  // a sheet's role is AXSheet, so it goes on to its window
        } else if (AXUIElementCopyAttributeValue(el, kAXParentAttribute, &next)) {
            next = 0;
        }
        if (role) CFRelease(role);
        CFRelease(cur);
        cur = next;
    }
    if (cur) CFRelease(cur);
    return r == 1;
}

// The frontmost application by NSWorkspace, and its name, 0 if unknown.
// Its answer changes by notifications, which arrive through the run
// loop, so first those pending are taken.
static u32 nsfront(Dk *dk, Arena *a, Str *name)
{
    char const *fw = "/System/Library/Frameworks/AppKit.framework/AppKit";
    if (!loadfw(&dk->appkit, fw)) {
        return 0;
    }
    for (i32 i = 0; i < 64; i++) {
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) !=
                kCFRunLoopRunHandledSource) {
            break;
        }
    }
    void *pool = objc_autoreleasePoolPush();
    id    ws   = msg<id>(cls("NSWorkspace"), "sharedWorkspace");
    id    app  = ws ? msg<id>(ws, "frontmostApplication") : (id)0;
    u32   r    = 0;
    if (app) {
        i32 pid = msg<i32>(app, "processIdentifier");
        r     = pid>0 ? (u32)pid : 0;
        *name = cfstr(a, (CFTypeRef)msg<id>(app, "localizedName"));
    }
    objc_autoreleasePoolPop(pool);
    return r;
}

// The window server's front process, 0 if unknown.
static u32 wsfront(Dk *dk)
{
    u32 psn[2] = {};
    i32 pid    = 0;
    if (!dk->wsfront || !dk->pidfor || dk->wsfront(psn) || dk->pidfor(psn, &pid)) {
        return 0;
    }
    return pid>0 ? (u32)pid : 0;
}

// Who has the keyboard, by each account that can be read (Focus), and
// with a target window (its element and id), how that window stands.
static Focus observe(Dk *dk, Arena *a, AXUIElementRef target, u64 id)
{
    Focus r = {};
    if (!granted(dk, PERM_AX)) {
        return r;
    }
    r.pid   = nsfront(dk, a, &r.app);
    r.wspid = wsfront(dk);

    // System-wide, which may fail (-25204), and is then unknown
    AXUIElementRef sys     = AXUIElementCreateSystemWide();
    CFTypeRef      focused = 0;
    if (!AXUIElementCopyAttributeValue(sys, kAXFocusedApplicationAttribute, &focused)) {
        pid_t pid = 0;
        AXUIElementGetPid((AXUIElementRef)focused, &pid);
        r.axpid = pid>0 ? (u32)pid : 0;
        CFRelease(focused);
    }
    CFRelease(sys);
    r.pid = r.pid ? r.pid : r.axpid;
    if (!r.pid) {
        return r;
    }

    // The rest asked of the frontmost application itself
    AXUIElementRef app = AXUIElementCreateApplication((pid_t)r.pid);
    if (!r.app.len) {
        r.app = axtitle(a, app);
    }
    CFTypeRef win = 0;
    if (!AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &win)) {
        CGWindowID wid = 0;
        if (dk->axwinid && !dk->axwinid((AXUIElementRef)win, &wid)) {
            r.win = wid;
        }
        r.title = axtitle(a, (AXUIElementRef)win);
        if (target && CFEqual(win, target)) {
            r.win = id;
        } else if (target) {
            CFTypeRef parent = 0;
            if (!AXUIElementCopyAttributeValue((AXUIElementRef)win,
                                               kAXParentAttribute, &parent)) {
                r.attached = CFEqual(parent, target);
                CFRelease(parent);
            }
        }
        CFRelease(win);
    }
    CFTypeRef elem = 0;
    if (target && id && (r.win==id || r.attached) &&
            !AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &elem)) {
        r.astray = outside((AXUIElementRef)elem, target);
        CFRelease(elem);
    }
    CFRelease(app);
    return r;
}

// The focused window, else the frontmost normal window on the screen,
// which belongs to the active application.
static u64 dk_foreground(Dk *dk)
{
    Arena scratch = dk->perm;  // discarded: names are not kept
    Focus f       = observe(dk, &scratch, 0, 0);
    if (f.win) {
        return f.win;
    }
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    u64 r = 0;
    for (CFIndex i = 0; !r && list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        b32 ok = !cfint(d, kCGWindowLayer) && cfreal(d, kCGWindowAlpha, 1)>0 &&
                 (!f.pid || cfint(d, kCGWindowOwnerPID)==f.pid);
        r = ok ? (u64)cfint(d, kCGWindowNumber) : 0;
    }
    if (list) CFRelease(list);
    return r;
}

static b32 dk_popup(Dk *, DkWin *w)
{
    return popuplevel(w->level);
}

static void dk_sleep(Dk *, i32 ms)
{
    struct timespec ts = {};
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    while (nanosleep(&ts, &ts) && errno==EINTR) {}
}

static i64 dk_ms(Dk *)
{
    return (i64)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000);
}

// A wait with the run loop running, by which NSWorkspace learns which
// application is now frontmost (nsfront).
static void dk_pump(Dk *dk, i32 ms)
{
    i64 end = dk_ms(dk) + ms;
    for (i64 now = dk_ms(dk); now < end; now = dk_ms(dk)) {
        f64 secs = (f64)(end - now) / 1000;
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, true) ==
                kCFRunLoopRunFinished) {
            dk_sleep(dk, (i32)(end - now));  // nothing to run
        }
    }
}

// Activation through AppKit, as the Dock asks for it. Since macOS 14,
// activation is cooperative: the active application yields it to
// another, which then takes it, and a request from a process not active
// itself, as this helper with no windows never is, may be declined. Yet
// live, on macOS 27, it alone made the application frontmost and its
// window key, and typed keys landed, so it is the first way tried, and
// alone.
static void nsactivate(Dk *dk, u32 pid)
{
    char const *fw = "/System/Library/Frameworks/AppKit.framework/AppKit";
    if (!loadfw(&dk->appkit, fw)) {
        return;
    }
    void *pool = objc_autoreleasePoolPush();
    id    app  = msg<id>(cls("NSRunningApplication"),
                         "runningApplicationWithProcessIdentifier:", (i32)pid);
    if (app) {
        // NSApplicationActivateAllWindows
        msg<BOOL>(app, "activateWithOptions:", (uz)1);
    }
    objc_autoreleasePoolPop(pool);
}

// The public ways: the application frontmost by Accessibility, which
// the application does itself when asked, then the window raised, made
// main, and focused, and the application's focused window.
static void axactivate(AXUIElementRef app, AXUIElementRef win)
{
    AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
    if (win) {
        AXUIElementPerformAction(win, kAXRaiseAction);
        AXUIElementSetAttributeValue(win, kAXMainAttribute, kCFBooleanTrue);
        AXUIElementSetAttributeValue(win, kAXFocusedAttribute, kCFBooleanTrue);
        AXUIElementSetAttributeValue(app, kAXFocusedWindowAttribute, win);
    }
}

// The window server's own way, private, as window managers (yabai,
// AltTab) focus a window: the process frontmost with that window, as if
// by the user (kCPSUserGenerated), then the two event records by which
// the window server tells a window it is key. A launch can leave an
// application active with no key window, which the public ways do not
// mend; this does.
static void skyactivate(Dk *dk, u32 pid, u64 id)
{
    u32 psn[2] = {};
    if (!dk->psnfor || !dk->setfront || !dk->postrec || !id ||
            dk->psnfor((i32)pid, psn)) {
        return;
    }
    dk->setfront(psn, (u32)id, 0x200);
    for (i32 i = 1; i <= 2; i++) {
        u8 rec[0xf8] = {};
        rec[0x04] = 0xf8;
        rec[0x08] = (u8)i;
        rec[0x3a] = 0x10;
        for (i32 j = 0x20; j < 0x30; j++) {
            rec[j] = 0xff;
        }
        for (i32 j = 0; j < 4; j++) {
            rec[0x3c+j] = (u8)(id >> (8*j));
        }
        dk->postrec(psn, rec);
    }
}

// The window unminimized, its app frontmost and the window key, by each
// way in turn until every readable account agrees that the window (or a
// sheet of it) has the keyboard (Focus, haskeyboard). Activation is
// asked of the application, which takes it in its own time, so each way
// waits a little for it. Without that agreement it fails, naming what
// has the keyboard instead, and the caller sends no input.
static b32 dk_activate(Dk *dk, DkWin *w)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    AXUIElementRef app = AXUIElementCreateApplication((pid_t)w->pid);
    AXUIElementRef win = axwindow(dk, w->pid, w->id);
    if (win && axflag(win, kAXMinimizedAttribute)==1) {
        AXUIElementSetAttributeValue(win, kAXMinimizedAttribute, kCFBooleanFalse);
    }
    u64 id = win ? w->id : 0;  // none to confirm without its element
    b32 ok = 0;
    for (i32 way = 0; !ok && way < 3; way++) {
        switch (way) {
        case 0: nsactivate(dk, w->pid);
                break;
        case 1: axactivate(app, win);  // the window, now that it may be active
                break;
        case 2: skyactivate(dk, w->pid, id);
                axactivate(app, win);
                break;
        }
        for (i32 i = 0; !ok && i < 25; i++) {
            Arena scratch = dk->perm;  // discarded: names are not kept
            Focus f       = observe(dk, &scratch, win, id);
            ok = haskeyboard(&f, w->pid, id);
            if (!ok) {
                dk_pump(dk, 20);
            }
        }
    }
    if (ok) {
        dk_sleep(dk, 50);  // let it draw
    } else {
        Focus f = observe(dk, &dk->perm, win, id);
        ok = haskeyboard(&f, w->pid, id);
        if (!ok) {
            dk->why = unfocused(&dk->perm, &f, w);
        }
    }
    if (win) CFRelease(win);
    CFRelease(app);

    Arena  scratch = dk->perm;  // discarded: only the rectangle is kept
    DkWin  now     = dk_window(dk, &scratch, w->id);
    if (now.id) {
        w->rect = now.rect;  // it may have been minimized
    }
    return ok;
}


// Capture

// The rectangle's pixels at the display's resolution, by ScreenCaptureKit
// (macOS 15.2), converted to sRGB as 32-bit BGRX.
static u8 *dk_capture(Dk *dk, Arena *a, DkRect r, DkSize *size)
{
    if (!needperm(dk, PERM_SCREEN)) {
        return 0;
    }
    Desk d   = readdesk(dk);
    Str  why = unseen(&d, r);
    if (why.data) {
        dk->why = why;
        return 0;
    }
    char const *path = "/System/Library/Frameworks/ScreenCaptureKit.framework/ScreenCaptureKit";
    id mgr = loadfw(&dk->sck, path) ? cls("SCScreenshotManager") : (id)0;
    if (!responds(mgr, "captureImageInRect:completionHandler:")) {
        dk->why = "screen capture needs macOS 15.2 or later (ScreenCaptureKit)";
        return 0;
    }

    void    *pool = objc_autoreleasePoolPush();
    MacWait *w    = newwait(dk);
    void (^done)(CGImageRef, id) = ^(CGImageRef image, id err) {
        if (image) {
            w->image = CGImageRetain(image);
        } else {
            errcopy(w->err, countof(w->err), err);
        }
        dispatch_semaphore_signal(w->done);
    };
    msg<void>(mgr, "captureImageInRect:completionHandler:", cgrect(r), (id)(void *)done);
    b32 ok = waitfor(w, MAC_CAPTUREMS);
    objc_autoreleasePoolPop(pool);
    if (!ok) {
        dk->why = "screen capture timed out";
        return 0;  // w and its image, if any, are left to leak
    } else if (!w->image) {
        dk->why = failed(dk, "screen capture failed: ", w->err);
        return 0;
    }

    CGImageRef img = w->image;
    iz         iw  = (iz)CGImageGetWidth(img);
    iz         ih  = (iz)CGImageGetHeight(img);
    u8        *px  = 0;
    if (iw>0 && ih>0 && iw<=(1<<15) && ih<=(1<<15)) {
        px = allocbytes(a, iw*ih*4);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef    ctx  = CGBitmapContextCreate(px, (uz)iw, (uz)ih, 8, (uz)iw*4, srgb,
            (u32)kCGImageAlphaNoneSkipFirst | (u32)kCGBitmapByteOrder32Little);
        if (ctx) {
            CGContextSetBlendMode(ctx, kCGBlendModeCopy);
            CGContextDrawImage(ctx, CGRectMake(0, 0, (f64)iw, (f64)ih), img);
            CGContextRelease(ctx);
        } else {
            px = 0;
        }
        CGColorSpaceRelease(srgb);
    }
    CGImageRelease(img);
    w->image = 0;
    if (!px) {
        dk->why = "screen capture failed: an image of no usable size";
        return 0;
    }
    *size = {(i32)iw, (i32)ih};
    return px;
}


// Input, posted at the HID level, as the devices' own. Not to the target
// process (CGEventPostToPid): AppKit routes keys to its key window all
// the same, and discards them with none, so it would not mend a window
// that is not key; system shortcuts and input methods see only HID
// events; and "key active cmd+tab" must reach the system, not one app.

static b32 post(CGEventRef e)
{
    if (!e) {
        return 0;
    }
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    return 1;
}

static CGPoint pointer(Dk *dk)
{
    if (dk->at) {
        return dk->pos;
    }
    CGEventRef e = CGEventCreate(0);
    CGPoint    p = e ? CGEventGetLocation(e) : CGPoint{};
    if (e) CFRelease(e);
    return p;
}

static b32 dk_pointer(Dk *dk, i32 x, i32 y)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    CGEventType type = dk->held==0 ? kCGEventLeftMouseDragged :
                       dk->held==1 ? kCGEventRightMouseDragged : kCGEventMouseMoved;
    CGPoint p = {(f64)x, (f64)y};
    dk->pos = p;
    dk->at  = 1;
    return post(CGEventCreateMouseEvent(0, type, p, dk->held==1 ? kCGMouseButtonRight
                                                                 : kCGMouseButtonLeft));
}

static b32 dk_button(Dk *dk, i32 button, b32 down)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    CGPoint p = pointer(dk);
    i32     n = down ? clickcount(&dk->click, button, (i32)p.x, (i32)p.y, dk_ms(dk))
                     : dk->click.count;
    CGEventType type = button ? (down ? kCGEventRightMouseDown : kCGEventRightMouseUp)
                              : (down ? kCGEventLeftMouseDown  : kCGEventLeftMouseUp);
    CGEventRef e = CGEventCreateMouseEvent(0, type, p, button ? kCGMouseButtonRight
                                                              : kCGMouseButtonLeft);
    if (e) {
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, n>0 ? n : 1);
    }
    dk->held = down ? button : -1;
    return post(e);
}

// Three lines a notch, as Windows scrolls by default.
static b32 dk_wheel(Dk *dk, i32 notches)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    return post(CGEventCreateScrollWheelEvent2(0, kCGScrollEventUnitLine, 1,
                                               notches*3, 0, 0));
}

static i32 dk_dragstep(Dk *)
{
    return 3;
}

// The current layout's keys of printable ASCII, unshifted preferred,
// through UCKeyTranslate; the US layout's if it cannot be read.
static void keymap(Dk *dk)
{
    if (dk->keyready) {
        return;
    }
    dk->keyready = 1;
    uskeymap(dk->keymap);
    TISInputSourceRef src  = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef         data = src ? (CFDataRef)TISGetInputSourceProperty(
                                       src, kTISPropertyUnicodeKeyLayoutData) : 0;
    if (!data) {
        if (src) CFRelease(src);
        return;
    }
    UCKeyboardLayout const *layout = (UCKeyboardLayout const *)CFDataGetBytePtr(data);
    i32 map[128] = {};
    for (i32 shift = 1; shift >= 0; shift--) {  // unshifted last, so it wins
        for (i32 code = 0; code < 128; code++) {
            UInt32     dead = 0;
            UniChar    ch[4];
            UniCharCount n  = 0;
            u32 mods = shift ? (shiftKey >> 8) & 0xff : 0;
            if (!UCKeyTranslate(layout, (UInt16)code, kUCKeyActionDown, mods,
                                LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit,
                                &dead, countof(ch), &n, ch) &&
                    n==1 && ch[0]>=' ' && ch[0]<0x7f) {
                map[ch[0]] = code | shift<<8 | 1<<9;
            }
        }
    }
    if (map['a'] && map['1']) {  // a layout with Latin letters and digits
        for (i32 c = 0; c < 128; c++) {
            dk->keymap[c] = map[c] ? map[c] : dk->keymap[c];
        }
    }
    CFRelease(src);
}

static i32 dk_keycode(Dk *dk, Str k, b32 *shift)
{
    keymap(dk);
    return keyfor(dk->keymap, k, shift);
}

static b32 postkey(i32 code, b32 down, u64 flags)
{
    CGEventRef e = CGEventCreateKeyboardEvent(0, (CGKeyCode)code, down);
    if (e) {
        CGEventSetFlags(e, (CGEventFlags)flags);
    }
    return post(e);
}

// Each event carries the modifiers held so far, which applications read
// from the event itself rather than from the earlier key events.
static b32 dk_keys(Dk *dk, i32 *codes, i32 n)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    u64 flags = 0;
    b32 ok    = 1;
    for (i32 i = 0; i < n; i++) {
        flags |= modflag(codes[i]);
        ok &= postkey(codes[i], 1, flags);
        dk_sleep(dk, 5);
    }
    for (i32 i = n-1; i >= 0; i--) {
        flags &= ~modflag(codes[i]);  // a modifier's release lifts it
        ok &= postkey(codes[i], 0, flags);
        dk_sleep(dk, 5);
    }
    return ok;
}

// Text as Unicode key events, a character (one or two UTF-16 units) at a
// time, with no modifiers; newlines and tabs as their keys. These are
// typed as by the user, so the application's text substitutions apply
// to them: live, a lone "x" typed at the start of a TextEdit document
// read back as "x", and soon after as "X", automatically capitalized.
static b32 dk_text(Dk *dk, Arena scratch, Str s)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    b32 ok = 1;
    for (iz i = 0; ok && i < s.len;) {
        i32 c = utf8decode(s, &i);
        if (c == '\r') {
            if (i<s.len && s[i]=='\n') {
                continue;
            }
            c = '\n';
        }
        if (c=='\n' || c=='\t') {
            i32 code = c=='\n' ? 0x24 : 0x30;
            ok &= postkey(code, 1, 0);
            ok &= postkey(code, 0, 0);
        } else {
            c16 u[2];
            iz  n = utf16encode(u, c);
            for (i32 down = 1; down >= 0; down--) {
                CGEventRef e = CGEventCreateKeyboardEvent(0, 0, down);
                if (e) {
                    CGEventSetFlags(e, 0);
                    CGEventKeyboardSetUnicodeString(e, (UniCharCount)n, (UniChar *)u);
                }
                ok &= post(e);
            }
        }
        dk_sleep(dk, 3);
    }
    return ok;
}


// Windows' lives, through Accessibility

static AXUIElementRef needwindow(Dk *dk, DkWin *w)
{
    if (!needperm(dk, PERM_AX)) {
        return 0;
    }
    AXUIElementRef r = axwindow(dk, w->pid, w->id);
    if (!r) {
        dk->why = "the window has no Accessibility element to act on (a menu "
                  "or a sheet?): act on its program's window instead";
    }
    return r;
}

// Press its close button, as a click on it would.
static b32 dk_close(Dk *dk, DkWin *w)
{
    AXUIElementRef win = needwindow(dk, w);
    if (!win) {
        return 0;
    }
    CFTypeRef btn = 0;
    b32       ok  = 0;
    if (!AXUIElementCopyAttributeValue(win, kAXCloseButtonAttribute, &btn)) {
        ok = !AXUIElementPerformAction((AXUIElementRef)btn, kAXPressAction);
        CFRelease(btn);
    } else {
        dk->why = "the window has no close button";
    }
    CFRelease(win);
    return ok;
}

static b32 dk_move(Dk *dk, DkWin *w, i32 x, i32 y, i32 cw, i32 ch, b32 size)
{
    AXUIElementRef win = needwindow(dk, w);
    if (!win) {
        return 0;
    }
    CGPoint  p  = {(f64)x, (f64)y};
    AXValueRef v = AXValueCreate((AXValueType)kAXValueCGPointType, &p);
    b32      ok = !AXUIElementSetAttributeValue(win, kAXPositionAttribute, v);
    CFRelease(v);
    if (ok && size) {
        CGSize s = {(f64)cw, (f64)ch};
        v  = AXValueCreate((AXValueType)kAXValueCGSizeType, &s);
        ok = !AXUIElementSetAttributeValue(win, kAXSizeAttribute, v);
        CFRelease(v);
    }
    CFRelease(win);
    return ok;
}


// Programs

static b32 isdir(Arena scratch, Str path)
{
    struct stat st;
    return !hasnul(path) && !stat(cstr(&scratch, path), &st) && S_ISDIR(st.st_mode);
}

// An application bundle, by LaunchServices, which starts it as the
// Finder would, apart from this process and the reaper that runs it, so
// it outlives both, as on Windows a program that leaves the job does.
// A running application is activated instead, and keeps its pid.
static u32 launchapp(Dk *dk, Arena scratch, Str path, Slice<Str> args)
{
    char const *fw = "/System/Library/Frameworks/AppKit.framework/AppKit";
    if (!loadfw(&dk->appkit, fw) || !cls("NSWorkspaceOpenConfiguration")) {
        dk->why = "cannot start applications: AppKit's NSWorkspace is missing";
        return 0;
    }
    void       *pool = objc_autoreleasePoolPush();
    CFStringRef cp   = newcfstr(path);
    if (!cp) {
        objc_autoreleasePoolPop(pool);
        return 0;
    }
    id url = msg<id>(cls("NSURL"), "fileURLWithPath:", (id)cp);
    CFRelease(cp);
    id argv = msg<id>(cls("NSMutableArray"), "array");
    for (iz i = 0; i < args.len; i++) {
        CFStringRef s = newcfstr(args[i]);
        if (s) {
            msg<void>(argv, "addObject:", (id)s);
            CFRelease(s);
        }
    }
    id conf = msg<id>(cls("NSWorkspaceOpenConfiguration"), "configuration");
    msg<void>(conf, "setArguments:", argv);
    msg<void>(conf, "setActivates:", (BOOL)YES);

    MacWait *w = newwait(dk);
    void (^done)(id, id) = ^(id app, id err) {
        if (app) {
            w->ok  = 1;
            w->pid = msg<i32>(app, "processIdentifier");
        } else {
            errcopy(w->err, countof(w->err), err);
        }
        dispatch_semaphore_signal(w->done);
    };
    id ws = msg<id>(cls("NSWorkspace"), "sharedWorkspace");
    msg<void>(ws, "openApplicationAtURL:configuration:completionHandler:",
              url, conf, (id)(void *)done);
    b32 ok = waitfor(w, MAC_LAUNCHMS);
    objc_autoreleasePoolPop(pool);
    if (!ok) {
        dk->why = "the application did not start in time";
        return 0;
    } else if (!w->ok) {
        dk->why = failed(dk, "cannot start the application: ", w->err);
        return 0;
    } else if (w->pid <= 0) {
        // LaunchServices answers once the program checks in, as every
        // application does, or once it has ended
        dk->why = "the application ended as soon as it started";
        return 0;
    }
    return (u32)w->pid;
}

// Anything else, from PATH, in a session of its own with nothing
// inherited. It is still the reaper's descendant, which ends it with
// the helper, so it counts as attached.
static u32 spawnprog(Dk *dk, Arena scratch, Slice<Str> argv)
{
    char **v = alloc<char *>(&scratch, argv.len+1);
    for (iz i = 0; i < argv.len; i++) {
        if (hasnul(argv[i])) {
            return 0;
        }
        v[i] = cstr(&scratch, argv[i]);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (i32 fd = 0; fd < 3; fd++) {
        posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t all, none;
    sigfillset(&all);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF |
                             POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = 0;
    i32   err = posix_spawnp(&pid, v[0], &fa, &attr, v, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (err) {
        dk->why = failed(dk, "cannot start it: ", strerror(err));
        return 0;
    }
    return (u32)pid;
}

static u32 dk_run(Dk *dk, Arena scratch, Slice<Str> argv, b32 *attached)
{
    *attached = 0;
    char const *home   = getenv("HOME");
    Slice<Str>  places = appplaces(&scratch, argv[0], fromcstr(home));
    for (iz i = 0; i < places.len; i++) {
        if (isdir(scratch, places[i])) {
            Slice<Str> args = {argv.data+1, argv.len-1, argv.len-1};
            return launchapp(dk, scratch, places[i], args);
        }
    }
    *attached = 1;
    return spawnprog(dk, scratch, argv);
}

// No program says when it waits for input, but an application is ready
// for it once it shows a window: 0 then, 1 at the timeout, 2 if the
// process is gone.
static i32 dk_idle(Dk *dk, u32 pid, i32 ms)
{
    i64 end = dk_ms(dk) + ms;
    for (;;) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (!pid || pid>0x7fffffff || proc_pidpath((i32)pid, path, sizeof(path)) <= 0) {
            return 2;
        }
        Arena        scratch = dk->perm;
        Slice<DkWin> ws      = dk_windows(dk, &scratch, 0);
        for (iz i = 0; i < ws.len; i++) {
            if (ws[i].pid==pid && !ws[i].level) {
                return 0;
            }
        }
        if (dk_ms(dk) >= end) {
            return 1;
        }
        dk_sleep(dk, 50);
    }
}


// Standard I/O

static iz dk_read(Dk *, u8 *buf, iz len)
{
    for (;;) {
        iz n = read(0, buf, (uz)(len<(1<<30) ? len : (1<<30)));
        if (n<0 && errno==EINTR) {
            continue;
        }
        return n;
    }
}

static b32 dk_write(Dk *, i32 fd, Str s)
{
    return writeall(fd, s);
}

static b32 dk_console(Dk *, i32 fd)
{
    return isatty(fd);
}


static Dk *newdk(Region *r)
{
    Arena perm = newarena(r);
    Dk   *dk   = alloc<Dk>(&perm);
    dk->perm  = perm;
    dk->held  = -1;
    dk->deny  = parsedeny(fromcstr(getenv("PROSPERO_TEST_DESKTOP_DENY")));
    // Private, but long stable: what every window manager and LLDB use
    dk->axwinid = (i32 (*)(AXUIElementRef, CGWindowID *))
                  dlsym(RTLD_DEFAULT, "_AXUIElementGetWindow");
    dk->respfor = (i32 (*)(i32))
                  dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
    // Focusing a window as window managers do (skyactivate), when the
    // public ways leave it without the keyboard
    dk->psnfor  = (i32 (*)(i32, u32 *))dlsym(RTLD_DEFAULT, "GetProcessForPID");
    dk->pidfor  = (i32 (*)(u32 *, i32 *))dlsym(RTLD_DEFAULT, "GetProcessPID");
    void *sky   = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                         RTLD_LAZY|RTLD_LOCAL);
    if (sky) {
        dk->setfront = (i32 (*)(u32 *, u32, u32))
                       dlsym(sky, "_SLPSSetFrontProcessWithOptions");
        dk->postrec  = (i32 (*)(u32 *, u8 *))dlsym(sky, "SLPSPostEventRecordTo");
        dk->wsfront  = (i32 (*)(u32 *))dlsym(sky, "_SLPSGetFrontProcess");
    }
    b32 self    = 0;
    Str resp    = responsible(dk, &dk->perm, &self);
    dk->granted = grantee(&dk->perm, resp, self);
    // Every Accessibility message gives up after this long, rather than
    // wait on a program that does not answer
    AXUIElementRef sys = AXUIElementCreateSystemWide();
    AXUIElementSetMessagingTimeout(sys, 2.0f);
    CFRelease(sys);
    return dk;
}

#if DK_SELFTEST
#  include "test/desktop_macos_test.cpp"
#else
int main(int argc, char **argv)
{
    // Standard descriptors must exist, or new ones would take their place
    for (i32 fd = open("/dev/null", O_RDWR); fd>=0 && fd<=2;) {
        fd = dup(fd);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);  // programs run are reaped by the system

    Region *pr = reserve((iz)1 << 24);
    Region *r  = reserve((iz)1 << 32);
    if (!pr || !r) {
        os_oom();
    }
    Dk   *dk = newdk(pr);
    Arena a  = newarena(r);
    Str  *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i] = fromcstr(argv[i]);
    }
    return dkmain(dk, a, args, argc);
}
#endif
