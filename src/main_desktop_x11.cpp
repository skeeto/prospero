// Desktop helper, Linux platform layer: X11 here, Wayland in
// deskwayland.cpp, chosen at run time
// $ c++ -std=c++20 -o prospero-desktop src/main_desktop_x11.cpp
//
// A program of its own beside a static engine, which cannot dlopen.
// Built with PROSPERO_DESKTOP, it is instead a translation unit of
// prospero's, its "desktop" command (main_posix.cpp), with one entry
// point, prospero_desktop, so that a build from source is one program.
//
// A session is Wayland's when WAYLAND_DISPLAY is set, or when
// XDG_SESSION_TYPE says wayland and there is no DISPLAY; else X11's.
// PROSPERO_DESKTOP_SESSION=x11 or =wayland overrides that. Under
// Wayland, X11 programs shown by Xwayland are still X windows, and when
// the compositor offers no way to name windows of its own, theirs are
// named through the X11 code below, while the screen and input go
// through the portal (deskwayland.cpp), since Xwayland sees neither.
//
// libX11, libXext (MIT-SHM) and libXtst (XTEST) are loaded at run time
// with dlopen, as SDL3 loads them for the GUI, so the helper builds
// without their headers and runs without them, if only to say what is
// missing. Xlib's types are declared here by hand, as much of them as
// is used.
//
// Screenshots read the root window through a shared memory segment, or
// with a plain GetImage on a display that cannot share memory (a remote
// one). Input is XTEST's fake events, into whatever the server would
// send real ones to. Text is typed as keysyms: a character no key of
// the keyboard makes is bound for a while to a spare keycode, one with
// no symbols, and the keycode is unbound again as the helper exits, at
// the end of its input or when asked to quit (SIGTERM). A helper killed
// outright leaves its bindings, recorded on the root window, which the
// next helper to run a command on the display undoes.
//
// Windows are the window manager's, through EWMH (_NET_CLIENT_LIST and
// the client messages for activating, closing and moving), and without
// one that speaks EWMH, the top-level windows themselves: the root's
// children, or the clients inside the frames of an older manager, found
// by their WM_STATE. A window's visible frame is the client and the
// decorations of _NET_FRAME_EXTENTS, or else its manager's frame.
// Input aimed at a window waits until the keyboard is confirmed to go
// to it: the X focus inside it, and a manager's _NET_ACTIVE_WINDOW too;
// else the command fails, naming what has the keyboard, and sends none.
//
// This is free and unencumbered software released into the public domain.
#if PROSPERO_DESKTOP
#  define DK_NAME "prospero desktop"
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>   // rename
#include <stdlib.h>  // getenv
#include <string.h>  // strerror
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "base.cpp"
#include "png.cpp"
#include "deskhelp.cpp"

extern char **environ;

using Xid   = decltype(0ul);  // XID, Window, Atom, KeySym, Time
using Xlong = decltype(0l);
struct Display;

// The start of Xlib's public Display, as _XPrivDisplay lays it out, for
// the mask of a client's resource ids (private4, resource_mask): all of
// one client's windows share the bits outside it.
struct XDisplayHead {
    void *ext_data;
    void *private1;
    i32   fd;
    i32   private2;
    i32   proto_major_version;
    i32   proto_minor_version;
    char *vendor;
    Xid   private3;  // resource_base
    Xid   private4;  // resource_mask
};

enum : i32 {
    X_ISVIEWABLE      = 2,   // map_state
    X_INPUTONLY       = 2,   // class
    X_ZPIXMAP         = 2,
    X_LSBFIRST        = 0,
    X_CLIENTMESSAGE   = 33,
    X_REVERTTOPARENT  = 2,
    X_STATICGRAVITY   = 10,
    X_PROPMODEREPLACE = 0,

    XA_ATOM           = 4,
    XA_CARDINAL       = 6,
    XA_STRING         = 31,
    XA_WINDOW         = 33,
    XA_WM_NAME        = 39,
    XA_WM_CLASS       = 67,
};

enum : Xlong {
    X_SUBSTRUCTURENOTIFY   = 1L << 19,
    X_SUBSTRUCTUREREDIRECT = 1L << 20,
};

struct XImage {
    i32    width, height;
    i32    xoffset;
    i32    format;
    u8    *data;
    i32    byte_order;
    i32    bitmap_unit;
    i32    bitmap_bit_order;
    i32    bitmap_pad;
    i32    depth;
    i32    bytes_per_line;
    i32    bits_per_pixel;
    Xid    red_mask;
    Xid    green_mask;
    Xid    blue_mask;
    void  *obdata;
    struct {
        void *create_image;
        i32 (*destroy_image)(XImage *);
        void *get_pixel;
        void *put_pixel;
        void *sub_image;
        void *add_pixel;
    } f;
};

struct XShmSegmentInfo {
    Xid  shmseg;
    i32  shmid;
    u8  *shmaddr;
    b32  readonly;
};

struct XWindowAttributes {
    i32   x, y;
    i32   width, height;
    i32   border_width;
    i32   depth;
    void *visual;
    Xid   root;
    i32   cls;
    i32   bit_gravity;
    i32   win_gravity;
    i32   backing_store;
    Xid   backing_planes;
    Xid   backing_pixel;
    b32   save_under;
    Xid   colormap;
    b32   map_installed;
    i32   map_state;
    Xlong all_event_masks;
    Xlong your_event_mask;
    Xlong do_not_propagate_mask;
    b32   override_redirect;
    void *screen;
};

struct XErrorEvent {
    i32      type;
    Display *display;
    Xid      resourceid;
    Xid      serial;
    u8       error_code;
    u8       request_code;
    u8       minor_code;
};

struct XClientMessageEvent {
    i32      type;
    Xid      serial;
    b32      send_event;
    Display *display;
    Xid      window;
    Xid      message_type;
    i32      format;
    Xlong    l[5];
};

union XEvent {
    i32                 type;
    XClientMessageEvent xclient;
    Xlong               pad[24];
};

// The functions used, looked up by name (load)
struct Xlib {
    // libX11
    Display *(*XOpenDisplay)(char *);
    i32      (*XDefaultScreen)(Display *);
    Xid      (*XRootWindow)(Display *, i32);
    void    *(*XDefaultVisual)(Display *, i32);
    i32      (*XDefaultDepth)(Display *, i32);
    i32      (*XInternAtoms)(Display *, char **, i32, b32, Xid *);
    i32      (*XGetWindowProperty)(Display *, Xid, Xid, Xlong, Xlong, b32, Xid,
                                   Xid *, i32 *, Xid *, Xid *, u8 **);
    i32      (*XFree)(void *);
    i32      (*XQueryTree)(Display *, Xid, Xid *, Xid *, Xid **, u32 *);
    i32      (*XGetWindowAttributes)(Display *, Xid, XWindowAttributes *);
    b32      (*XTranslateCoordinates)(Display *, Xid, Xid, i32, i32,
                                      i32 *, i32 *, Xid *);
    XImage  *(*XGetImage)(Display *, Xid, i32, i32, u32, u32, Xid, i32);
    i32      (*XSync)(Display *, b32);
    void    *(*XSetErrorHandler)(i32 (*)(Display *, XErrorEvent *));
    void    *(*XSetIOErrorHandler)(i32 (*)(Display *));
    i32      (*XSendEvent)(Display *, Xid, b32, Xlong, XEvent *);
    i32      (*XRaiseWindow)(Display *, Xid);
    i32      (*XSetInputFocus)(Display *, Xid, i32, Xid);
    i32      (*XGetInputFocus)(Display *, Xid *, i32 *);
    i32      (*XMoveWindow)(Display *, Xid, i32, i32);
    i32      (*XMoveResizeWindow)(Display *, Xid, i32, i32, u32, u32);
    i32      (*XDisplayKeycodes)(Display *, i32 *, i32 *);
    Xid     *(*XGetKeyboardMapping)(Display *, u8, i32, i32 *);
    i32      (*XChangeKeyboardMapping)(Display *, i32, i32, Xid *, i32);
    i32      (*XChangeProperty)(Display *, Xid, Xid, Xid, i32, i32, u8 *, i32);
    i32      (*XDeleteProperty)(Display *, Xid, Xid);
    Xid      (*XCreateWindow)(Display *, Xid, i32, i32, u32, u32, u32, i32,
                              u32, void *, Xid, void *);
    i32      (*XGrabServer)(Display *);
    i32      (*XUngrabServer)(Display *);
    // libXext, optional
    b32      (*XShmQueryExtension)(Display *);
    XImage  *(*XShmCreateImage)(Display *, void *, u32, i32, u8 *,
                                XShmSegmentInfo *, u32, u32);
    b32      (*XShmAttach)(Display *, XShmSegmentInfo *);
    b32      (*XShmDetach)(Display *, XShmSegmentInfo *);
    b32      (*XShmGetImage)(Display *, Xid, XImage *, i32, i32, Xid);
    // libXtst
    b32      (*XTestQueryExtension)(Display *, i32 *, i32 *, i32 *, i32 *);
    i32      (*XTestFakeMotionEvent)(Display *, i32, i32, i32, Xid);
    i32      (*XTestFakeButtonEvent)(Display *, u32, b32, Xid);
    i32      (*XTestFakeKeyEvent)(Display *, u32, b32, Xid);
};

enum {
    A_CLIENTS, A_STACKING, A_ACTIVE, A_CLOSE, A_MOVERESIZE, A_SUPPORTED,
    A_CHECK, A_NAME, A_UTF8, A_PID, A_EXTENTS, A_GTKEXTENTS, A_TYPE,
    A_MENU, A_DROPDOWN, A_POPUP, A_TOOLTIP, A_NOTIFICATION, A_COMBO, A_DND,
    A_WMSTATE, A_PROTOCOLS, A_DELETE, A_KEYS,
    A_COUNT
};

static char const *atomnames[A_COUNT] = {
    "_NET_CLIENT_LIST", "_NET_CLIENT_LIST_STACKING", "_NET_ACTIVE_WINDOW",
    "_NET_CLOSE_WINDOW", "_NET_MOVERESIZE_WINDOW", "_NET_SUPPORTED",
    "_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME", "UTF8_STRING",
    "_NET_WM_PID", "_NET_FRAME_EXTENTS", "_GTK_FRAME_EXTENTS",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_COMBO", "_NET_WM_WINDOW_TYPE_DND",
    "WM_STATE", "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_PROSPERO_DESKTOP_KEYS",
};

enum : iz {
    KEYCOLS = 8,     // keysyms kept per keycode
    SPARES  = 32,    // spare keycodes bound at once
    GRACE   = 1000,  // milliseconds a key stays bound after it is typed
};

// A spare keycode this helper bound to a keysym.
struct Bound {
    i32 code;
    Xid sym;
    Xid prev;     // its last keysym, until it is bound to sym, else 0
    b32 pending;  // bound to sym only as this helper knows the keyboard
    i64 used;     // when it was last typed, dk_ms
};

struct Wl;

struct Dk {
    b32       wayland;      // a Wayland session: through wl
    Wl       *wl;
    b32       xwl;          // and its windows are Xwayland's, through X11
    f64       xk;           // Xwayland's pixels to a logical pixel
    Xlib      x;
    Display  *dpy;
    Xid       root;
    i32       screen;
    Str       why;          // set once loading failed for good
    Str       failed;       // why the latest call failed, for dk_why
    b32       shm;          // MIT-SHM may be tried
    Xid       atom[A_COUNT];
    Xid       idmask;       // a client's resource ids differ only in these
    Xid       wmcheck;      // the manager's check window, 0 without EWMH
    b32       ewmh;         // a manager speaks EWMH, with
    b32       has[A_COUNT]; // these in its _NET_SUPPORTED

    // The keyboard: keysyms by keycode, and the spare keycodes
    i32       minkey, maxkey, cols;
    Xid       keys[256][KEYCOLS];
    u8        spare[SPARES];
    i32       nspare;
    Bound     bound[SPARES];  // by this helper, until it exits
    i32       nbound;
    Xid       owner;        // names this helper in the record of bindings

    Arena     tmp;          // for calls without an arena, reset each
};

// Xlib reports errors to one handler for the whole process, with no
// context of the caller's: the latest one is kept here, and a window
// that went away meanwhile is an error to expect, not to die of.
static i32 xerror;

static i32 onxerror(Display *, XErrorEvent *e)
{
    xerror = e->error_code ? e->error_code : 1;
    return 0;
}

// Asked to quit (SIGTERM, SIGINT, SIGHUP): stop waiting, and finish as
// at the end of input, unbinding the keys bound for typing.
static volatile sig_atomic_t quitting;

static void onquit(int)
{
    quitting = 1;
}

// Sleep, but not past a request to quit.
static void nap(i64 ms)
{
    struct timespec ts = {};
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    while (!quitting && nanosleep(&ts, &ts) && errno==EINTR) {}
}

static b32 writeall(i32 fd, Str s)
{
    for (iz off = 0; off < s.len;) {
        iz n = write(fd, s.data+off, (uz)(s.len-off));
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        off += n;
    }
    return 1;
}

static i32 onxioerror(Display *)
{
    writeall(2, "prospero-desktop: lost the connection to the X display\n");
    _exit(DK_FAIL);
}

[[noreturn]] static void os_oom()
{
    writeall(2, "prospero-desktop: out of memory\n");
    _exit(DK_FAIL);
}

static iz pagesize()
{
    iz page = sysconf(_SC_PAGESIZE);
    return page>0 ? page : 1<<12;
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
    iz    page = pagesize();
    byte *p    = (byte *)mmap(0, (uz)cap, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
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

static Arena newarena(Region *r)
{
    Arena a = {};
    a.beg    = r->base;
    a.end    = r->base + r->committed;
    a.region = r;
    return a;
}

static char *cstr(Arena *a, Str s)
{
    char *r = (char *)allocbytes(a, s.len+1);
    copybytes(r, s.data, s.len);
    r[s.len] = 0;
    return r;
}

static Str fromcstr(char *s)
{
    Str r = {};
    r.data = (u8 *)s;
    for (; s && s[r.len]; r.len++) {}
    return r;
}

#include "deskwayland.cpp"


// Loading Xlib

template<typename T>
static b32 load(T *f, void *lib, char const *name)
{
    *f = lib ? (T)dlsym(lib, name) : 0;
    return *f != 0;
}

static void *openlib(char const *name, char const *alt)
{
    void *lib = dlopen(name, RTLD_NOW|RTLD_LOCAL);
    return lib ? lib : dlopen(alt, RTLD_NOW|RTLD_LOCAL);
}

static Str loadfail(Arena *a, Str what)
{
    Buf b(a, 256);
    print(&b, what);
    char *err = dlerror();
    if (err) {
        print(&b, " (");
        print(&b, fromcstr(err));
        putbyte(&b, ')');
    }
    return finish(&b);
}

// Load the libraries, else why not.
static Str loadxlib(Dk *dk, Arena *a)
{
    Xlib *x   = &dk->x;
    void *x11 = openlib("libX11.so.6", "libX11.so");
    if (!x11) {
        return loadfail(a, "cannot load libX11, the X client library");
    }
    b32 ok = 1;
    ok &= load(&x->XOpenDisplay,           x11, "XOpenDisplay");
    ok &= load(&x->XDefaultScreen,         x11, "XDefaultScreen");
    ok &= load(&x->XRootWindow,            x11, "XRootWindow");
    ok &= load(&x->XDefaultVisual,         x11, "XDefaultVisual");
    ok &= load(&x->XDefaultDepth,          x11, "XDefaultDepth");
    ok &= load(&x->XInternAtoms,           x11, "XInternAtoms");
    ok &= load(&x->XGetWindowProperty,     x11, "XGetWindowProperty");
    ok &= load(&x->XFree,                  x11, "XFree");
    ok &= load(&x->XQueryTree,             x11, "XQueryTree");
    ok &= load(&x->XGetWindowAttributes,   x11, "XGetWindowAttributes");
    ok &= load(&x->XTranslateCoordinates,  x11, "XTranslateCoordinates");
    ok &= load(&x->XGetImage,              x11, "XGetImage");
    ok &= load(&x->XSync,                  x11, "XSync");
    ok &= load(&x->XSetErrorHandler,       x11, "XSetErrorHandler");
    ok &= load(&x->XSetIOErrorHandler,     x11, "XSetIOErrorHandler");
    ok &= load(&x->XSendEvent,             x11, "XSendEvent");
    ok &= load(&x->XRaiseWindow,           x11, "XRaiseWindow");
    ok &= load(&x->XSetInputFocus,         x11, "XSetInputFocus");
    ok &= load(&x->XGetInputFocus,         x11, "XGetInputFocus");
    ok &= load(&x->XMoveWindow,            x11, "XMoveWindow");
    ok &= load(&x->XMoveResizeWindow,      x11, "XMoveResizeWindow");
    ok &= load(&x->XDisplayKeycodes,       x11, "XDisplayKeycodes");
    ok &= load(&x->XGetKeyboardMapping,    x11, "XGetKeyboardMapping");
    ok &= load(&x->XChangeKeyboardMapping, x11, "XChangeKeyboardMapping");
    ok &= load(&x->XChangeProperty,        x11, "XChangeProperty");
    ok &= load(&x->XDeleteProperty,        x11, "XDeleteProperty");
    ok &= load(&x->XCreateWindow,          x11, "XCreateWindow");
    ok &= load(&x->XGrabServer,            x11, "XGrabServer");
    ok &= load(&x->XUngrabServer,          x11, "XUngrabServer");
    if (!ok) {
        return loadfail(a, "libX11 lacks a function it should have");
    }

    void *tst = openlib("libXtst.so.6", "libXtst.so");
    ok  = load(&x->XTestQueryExtension,  tst, "XTestQueryExtension");
    ok &= load(&x->XTestFakeMotionEvent, tst, "XTestFakeMotionEvent");
    ok &= load(&x->XTestFakeButtonEvent, tst, "XTestFakeButtonEvent");
    ok &= load(&x->XTestFakeKeyEvent,    tst, "XTestFakeKeyEvent");
    if (!ok) {
        return loadfail(a, "cannot load libXtst, the XTEST library input needs");
    }

    // Without MIT-SHM, screenshots take the slower way
    void *ext = openlib("libXext.so.6", "libXext.so");
    ok  = load(&x->XShmQueryExtension, ext, "XShmQueryExtension");
    ok &= load(&x->XShmCreateImage,    ext, "XShmCreateImage");
    ok &= load(&x->XShmAttach,         ext, "XShmAttach");
    ok &= load(&x->XShmDetach,         ext, "XShmDetach");
    ok &= load(&x->XShmGetImage,       ext, "XShmGetImage");
    dk->shm = ok;
    return {};
}


// Properties

struct Prop {
    u8 *data;    // format 8: bytes; 32: Xlong items, whatever the size
    iz  len;     // items
    Xid type;
    b32 ok;
};

// Up to max items of a window's property of the given type (0: any),
// copied into the arena.
static Prop getprop(Dk *dk, Arena *a, Xid w, Xid prop, Xid type, iz max)
{
    Prop r      = {};
    Xid  actual = 0;
    i32  format = 0;
    Xid  n      = 0;
    Xid  after  = 0;
    u8  *data   = 0;
    i32  err    = dk->x.XGetWindowProperty(dk->dpy, w, prop, 0, (Xlong)max,
                                           0, type, &actual, &format, &n,
                                           &after, &data);
    if (!err && data && actual && (!type || actual==type)) {
        // Items are read in place, so they keep their alignment
        r.len = (iz)n<max ? (iz)n : max;
        switch (format) {
        case 32: r.data = (u8 *)alloc<Xlong>(a, r.len); break;
        case 16: r.data = (u8 *)alloc<short>(a, r.len); break;
        default: r.data = allocbytes(a, r.len);
        }
        iz size = format==32 ? (iz)sizeof(Xlong) : format==16 ? 2 : 1;
        copybytes(r.data, data, r.len*size);
        r.type = actual;
        r.ok   = 1;
    }
    if (data) {
        dk->x.XFree(data);
    }
    return r;
}

static Xlong propitem(Prop p, iz i)
{
    return ((Xlong *)p.data)[i];
}

struct Card {
    Xlong val;
    b32   ok;
};

// The first 32-bit item of a property, as a window or a cardinal, of
// the given type (0: any).
static Card getcard(Dk *dk, Xid w, Xid prop, Xid type)
{
    Card r      = {};
    Xid  actual = 0;
    i32  format = 0;
    Xid  n      = 0;
    Xid  after  = 0;
    u8  *data   = 0;
    i32  err    = dk->x.XGetWindowProperty(dk->dpy, w, prop, 0, 1, 0, type,
                                           &actual, &format, &n, &after, &data);
    if (!err && data && actual && (!type || actual==type) && format==32 && n) {
        r.val = *(Xlong *)data;
        r.ok  = 1;
    }
    if (data) {
        dk->x.XFree(data);
    }
    return r;
}

// Latin-1, as WM_NAME's STRING is, to UTF-8.
static Str fromlatin1(Arena *a, Str s)
{
    Buf b(a, s.len*2);
    for (iz i = 0; i < s.len; i++) {
        u8 tmp[4];
        print(&b, Str{tmp, utf8encode(tmp, s[i])});
    }
    return finish(&b);
}

static Str gettitle(Dk *dk, Arena *a, Xid w)
{
    Prop p = getprop(dk, a, w, dk->atom[A_NAME], dk->atom[A_UTF8], 1024);
    if (p.ok) {
        return Str{p.data, p.len};
    }
    p = getprop(dk, a, w, XA_WM_NAME, 0, 1024);
    Str s = {p.data, p.len};
    if (!p.ok) {
        return "";
    }
    return p.type==dk->atom[A_UTF8] ? s : fromlatin1(a, s);
}

// WM_CLASS is "instance\0class\0": the class, as in "XTerm".
static Str getclass(Dk *dk, Arena *a, Xid w)
{
    Prop p = getprop(dk, a, w, XA_WM_CLASS, XA_STRING, 256);
    Cut  c = cut(Str{p.data, p.len}, 0);
    return c.ok ? cut(c.tail, 0).head : Str("");
}


// The window manager

static void sendroot(Dk *dk, Xid w, Xid type, Xlong *l)
{
    XEvent ev = {};
    ev.xclient.type         = X_CLIENTMESSAGE;
    ev.xclient.window       = w;
    ev.xclient.message_type = type;
    ev.xclient.format       = 32;
    for (i32 i = 0; i < 5; i++) {
        ev.xclient.l[i] = l[i];
    }
    dk->x.XSendEvent(dk->dpy, dk->root, 0,
                     X_SUBSTRUCTURENOTIFY|X_SUBSTRUCTUREREDIRECT, &ev);
}

// Whether a manager that speaks EWMH runs, and what it supports: asked
// before each command, since one may start or stop between them.
static void findwm(Dk *dk, Arena scratch)
{
    dk->ewmh    = 0;
    dk->wmcheck = 0;
    for (i32 i = 0; i < A_COUNT; i++) {
        dk->has[i] = 0;
    }
    Card c = getcard(dk, dk->root, dk->atom[A_CHECK], XA_WINDOW);
    if (!c.ok || !c.val) {
        return;
    }
    // The check window names itself, unless it is a stale one left
    // by a manager that died
    Card self = getcard(dk, (Xid)c.val, dk->atom[A_CHECK], XA_WINDOW);
    if (!self.ok || self.val!=c.val) {
        return;
    }
    dk->ewmh    = 1;
    dk->wmcheck = (Xid)c.val;
    Prop p = getprop(dk, &scratch, dk->root, dk->atom[A_SUPPORTED], XA_ATOM, 4096);
    for (iz i = 0; i < p.len; i++) {
        for (i32 k = 0; k < A_COUNT; k++) {
            dk->has[k] |= (Xid)propitem(p, i) == dk->atom[k];
        }
    }
}

struct Kids {
    Xid *ids;  // bottom of the stack first
    iz   len;
    Xid  parent;
    b32  ok;
};

static Kids querytree(Dk *dk, Arena *a, Xid w)
{
    Kids r    = {};
    Xid  root = 0;
    Xid *ids  = 0;
    u32  n    = 0;
    if (dk->x.XQueryTree(dk->dpy, w, &root, &r.parent, &ids, &n)) {
        r.ok  = 1;
        r.len = n;
        r.ids = alloc<Xid>(a, r.len);
        copybytes(r.ids, ids, r.len*(iz)sizeof(Xid));
    }
    if (ids) {
        dk->x.XFree(ids);
    }
    return r;
}

// The client window in w, the first with WM_STATE, searching depth
// levels down; 0 if none.
static Xid clientin(Dk *dk, Arena scratch, Xid w, i32 depth)
{
    if (getcard(dk, w, dk->atom[A_WMSTATE], 0).ok) {
        return w;
    } else if (!depth) {
        return 0;
    }
    Kids k = querytree(dk, &scratch, w);
    for (iz i = k.len-1; i >= 0; i--) {
        Xid c = clientin(dk, scratch, k.ids[i], depth-1);
        if (c) {
            return c;
        }
    }
    return 0;
}

// The root's child that holds w: its frame, or w itself.
static Xid toplevel(Dk *dk, Arena scratch, Xid w)
{
    for (i32 i = 0; w && w!=dk->root && i < 64; i++) {
        Kids k = querytree(dk, &scratch, w);
        if (!k.ok) {
            return 0;
        } else if (k.parent == dk->root) {
            return w;
        }
        w = k.parent;
    }
    return 0;
}

// Whether the manager made a window, as it makes its check window: the
// bits of their ids that name the client are the same.
static b32 wmowned(Dk *dk, Xid w)
{
    Xid client = ~dk->idmask;
    return dk->wmcheck && (w & client)==(dk->wmcheck & client);
}

// The windows that are windows to the user, top first: the manager's
// clients, under the popups (override-redirect, as menus are) over
// them, or without EWMH, the root's children, or their clients. The
// manager's own windows are none of them: its check window, and its
// panels, which some list as clients (IceWM's taskbar), nor are frames,
// which some make override-redirect (IceWM).
static Slice<Xid> toplevels(Dk *dk, Arena *a, Arena tmp)
{
    Slice<Xid> r    = {};
    Kids       kids = querytree(dk, &tmp, dk->root);
    if (dk->ewmh) {
        for (iz i = kids.len-1; i >= 0; i--) {
            Xid               w  = kids.ids[i];
            XWindowAttributes wa = {};
            if (dk->x.XGetWindowAttributes(dk->dpy, w, &wa) &&
                    wa.override_redirect && wa.map_state==X_ISVIEWABLE &&
                    wa.cls!=X_INPUTONLY && !wmowned(dk, w) &&
                    !clientin(dk, tmp, w, 2)) {
                r = push(a, r, w);
            }
        }
        Prop p = getprop(dk, &tmp, dk->root, dk->atom[A_STACKING], XA_WINDOW, 1<<16);
        if (!p.ok) {
            // Mapping order, which is no z-order, but the best there is
            p = getprop(dk, &tmp, dk->root, dk->atom[A_CLIENTS], XA_WINDOW, 1<<16);
        }
        if (p.ok) {
            for (iz i = p.len-1; i >= 0; i--) {
                Xid w = (Xid)propitem(p, i);
                if (!wmowned(dk, w)) {
                    r = push(a, r, w);
                }
            }
            return r;
        }
        r = {};  // a manager without a list, as Xfwm4 before its first client
    }
    for (iz i = kids.len-1; i >= 0; i--) {
        Xid c = clientin(dk, tmp, kids.ids[i], 2);
        Xid w = c ? c : kids.ids[i];
        if (!wmowned(dk, w)) {
            r = push(a, r, w);
        }
    }
    return r;
}

struct Geom {
    DkRect client;  // on the screen, inside the border
    DkRect frame;   // the visible frame
    b32    viewable;
    b32    popup;   // override-redirect
    b32    ok;
};

static DkRect makerect(i32 x, i32 y, i32 w, i32 h)
{
    return {x, y, x+w, y+h};
}

static DkRect unscale(DkRect r, f64 k)
{
    r.left   = (i32)(r.left / k);
    r.top    = (i32)(r.top / k);
    r.right  = (i32)(r.right / k);
    r.bottom = (i32)(r.bottom / k);
    return r;
}

static Geom geometry(Dk *dk, Arena a, Xid w)
{
    Geom              r  = {};
    XWindowAttributes wa = {};
    Xid               child = 0;
    i32               x = 0, y = 0;
    if (!dk->x.XGetWindowAttributes(dk->dpy, w, &wa) || wa.cls==X_INPUTONLY ||
            !dk->x.XTranslateCoordinates(dk->dpy, w, dk->root, 0, 0, &x, &y, &child)) {
        return r;
    }
    r.client   = makerect(x, y, wa.width, wa.height);
    r.frame    = r.client;
    r.viewable = wa.map_state == X_ISVIEWABLE;
    r.popup    = wa.override_redirect;
    r.ok       = 1;

    Prop e = getprop(dk, &a, w, dk->atom[A_EXTENTS], XA_CARDINAL, 4);
    if (e.ok && e.len==4) {
        // left, right, top, bottom
        r.frame.left   -= (i32)propitem(e, 0);
        r.frame.right  += (i32)propitem(e, 1);
        r.frame.top    -= (i32)propitem(e, 2);
        r.frame.bottom += (i32)propitem(e, 3);
    } else {
        Xid top = toplevel(dk, a, w);
        XWindowAttributes fa = {};
        if (top && top!=w && dk->x.XGetWindowAttributes(dk->dpy, top, &fa)) {
            i32 b = fa.border_width;
            r.frame = makerect(fa.x, fa.y, fa.width + 2*b, fa.height + 2*b);
        }
    }

    // Client-side decorations draw a shadow inside the window
    Prop g = getprop(dk, &a, w, dk->atom[A_GTKEXTENTS], XA_CARDINAL, 4);
    if (g.ok && g.len==4) {
        r.frame.left   += (i32)propitem(g, 0);
        r.frame.right  -= (i32)propitem(g, 1);
        r.frame.top    += (i32)propitem(g, 2);
        r.frame.bottom -= (i32)propitem(g, 3);
    }

    // Xwayland may count in pixels of a scaled display (Mutter's native
    // scaling), more of them than the compositor's logical pixels
    if (dk->xk > 1) {
        r.client = unscale(r.client, dk->xk);
        r.frame  = unscale(r.frame, dk->xk);
    }
    return r;
}

struct XWin {
    DkWin win;
    Geom  geom;
};

static XWin describe(Dk *dk, Arena *a, Xid w)
{
    XWin r = {};
    xerror = 0;
    r.geom = geometry(dk, *a, w);
    if (!r.geom.ok) {
        return r;
    }
    Card pid = getcard(dk, w, dk->atom[A_PID], XA_CARDINAL);
    r.win.id    = w;
    r.win.pid   = pid.ok ? (u32)pid.val : 0;
    r.win.rect  = r.geom.frame;
    r.win.title = gettitle(dk, a, w);
    r.win.cls   = getclass(dk, a, w);
    return r;
}


// Windows

static Str dk_os(Dk *)
{
    return "linux";
}

static Str dk_units(Dk *)
{
    return "pixels";
}

static Str dk_why(Dk *dk)
{
    Str r = dk->failed;
    dk->failed = {};
    if (dk->wayland && dk->wl->failed.data) {
        r = dk->wl->failed;
        dk->wl->failed = {};
    }
    return r;
}

// X asks no permission of a client of the display; Wayland's portal
// asks the user.
static b32 dk_access(Dk *dk, Buf *b, b32 ask)
{
    if (dk->wayland) {
        return wlaccess(dk->wl, dk->tmp, b, ask);
    }
    return 1;
}

static i32  recoverkeys(Dk *);
static void reserved(Dk *, Prop);

// The keyboard's keysyms, and the keycodes without any.
static void loadkeys(Dk *dk)
{
    i32 lo = 0, hi = 0, cols = 0;
    dk->x.XDisplayKeycodes(dk->dpy, &lo, &hi);
    lo = lo<8 ? 8 : lo;
    hi = hi>255 ? 255 : hi;
    dk->minkey = lo;
    dk->maxkey = hi;
    dk->nspare = 0;
    for (i32 k = 0; k < 256; k++) {
        for (i32 c = 0; c < KEYCOLS; c++) {
            dk->keys[k][c] = 0;
        }
    }
    if (hi < lo) {
        return;
    }
    Xid *syms = dk->x.XGetKeyboardMapping(dk->dpy, (u8)lo, hi-lo+1, &cols);
    if (!syms) {
        return;
    }
    dk->cols = cols<KEYCOLS ? cols : (i32)KEYCOLS;
    for (i32 k = lo; k <= hi; k++) {
        b32 none = 1;
        for (i32 c = 0; c < cols; c++) {
            Xid s = syms[(k-lo)*cols + c];
            none &= !s;
            if (c < KEYCOLS) {
                dk->keys[k][c] = s;
            }
        }
        if (none && dk->nspare<SPARES) {
            dk->spare[dk->nspare++] = (u8)k;
        }
    }
    dk->x.XFree(syms);
}

// Open the X display once, or why it cannot be.
static Str x11open(Dk *dk)
{
    Arena *perm    = &dk->tmp;  // what stays allocated here stays for good
    Str    display = fromcstr(getenv("DISPLAY"));
    if (!display.len) {
        return "no X display: DISPLAY is not set";
    }
    if (!dk->dpy) {
        if (!dk->x.XOpenDisplay) {
            Str why = loadxlib(dk, perm);
            if (why.data) {
                dk->why = why;  // for good
                return why;
            }
        }
        dk->dpy = dk->x.XOpenDisplay(0);
        if (!dk->dpy) {
            Buf b(perm, 64);
            print(&b, "cannot open the X display '");
            print(&b, display);
            print(&b, "'");
            return finish(&b);
        }
        dk->x.XSetErrorHandler(onxerror);
        dk->x.XSetIOErrorHandler(onxioerror);
        dk->screen = dk->x.XDefaultScreen(dk->dpy);
        Xid mask   = ((XDisplayHead *)dk->dpy)->private4;
        dk->idmask = mask && !(mask & (mask+1)) ? mask : 0x1fffff;
        dk->root   = dk->x.XRootWindow(dk->dpy, dk->screen);
        i32 n[4] = {};
        if (!dk->x.XTestQueryExtension(dk->dpy, n, n+1, n+2, n+3)) {
            dk->why = "the X server lacks the XTEST extension, which input needs";
            return dk->why;
        }
        dk->shm = dk->shm && dk->x.XShmQueryExtension(dk->dpy);
        dk->x.XInternAtoms(dk->dpy, (char **)atomnames, A_COUNT, 0, dk->atom);
    }
    findwm(dk, dk->tmp);
    return {};
}

static b32 waylandsession()
{
    Str force   = fromcstr(getenv("PROSPERO_DESKTOP_SESSION"));
    Str session = fromcstr(getenv("XDG_SESSION_TYPE"));
    Str wayland = fromcstr(getenv("WAYLAND_DISPLAY"));
    Str display = fromcstr(getenv("DISPLAY"));
    if (force=="x11" || force=="wayland") {
        return force == "wayland";
    }
    return wayland.len || (session=="wayland" && !display.len);
}

static Str dk_ready(Dk *dk)
{
    while (waitpid(-1, 0, WNOHANG) > 0) {}  // programs run that ended
    if (dk->why.data) {
        return dk->why;
    }
    dk->wayland = waylandsession();
    if (dk->wayland) {
        Str why = wlready(dk->wl, dk->tmp);
        if (why.data) {
            return why;
        }
        // Xwayland's windows, when the compositor names none
        dk->xwl = 0;
        if (dk->wl->winapi == WIN_NONE) {
            dk->xwl = !x11open(dk).data && dk->ewmh;
            dk->why = {};  // X11 is optional here
        }
        if (dk->xwl && dk->xk==0) {
            XWindowAttributes wa = {};
            dk->x.XGetWindowAttributes(dk->dpy, dk->root, &wa);
            i32 lw = wlwidth(dk->wl, dk->tmp);
            dk->xk = lw>0 && wa.width>lw ? (f64)wa.width/lw : 1;
        }
        return {};
    }
    Str why = x11open(dk);
    if (!why.data) {
        loadkeys(dk);
        if (recoverkeys(dk)) {
            loadkeys(dk);  // with the spare keycodes recovered
            recoverkeys(dk);
        }
    }
    return why;
}

// Under Wayland without a compositor's window list, whatever names
// windows says so for a selection that matches none (dk_why).
static void nowindows(Dk *dk)
{
    Str why = dk->wl->winwhy;
    if (dk->xwl) {
        why = concat(&dk->wl->perm, why, ", so only X11 windows (Xwayland's) "
                     "can be named");
    }
    dk->wl->failed = why;
}

// The windows mapped, and those a manager minimized or keeps on another
// desktop (IconicState), as Windows lists a minimized window: they may
// be named, and activating one brings it back.
static Slice<DkWin> dk_windows(Dk *dk, Arena *a, b32 all)
{
    if (dk->wayland) {
        if (dk->wl->winapi != WIN_NONE) {
            return wlwindows(dk->wl, a, all);
        }
        nowindows(dk);
        if (!dk->xwl) {
            return {};
        }
    }
    Slice<Xid>   ids = toplevels(dk, a, dk->tmp);
    Slice<DkWin> r   = {};
    for (iz i = 0; i < ids.len; i++) {
        XWin w = describe(dk, a, ids[i]);
        if (!w.win.id) {
            continue;
        } else if (all || w.geom.viewable) {
            r = push(a, r, w.win);
        } else if (dk->ewmh) {
            Card st = getcard(dk, ids[i], dk->atom[A_WMSTATE], dk->atom[A_WMSTATE]);
            if (st.ok && st.val==3) {
                r = push(a, r, w.win);
            }
        }
    }
    return r;
}

static DkWin dk_screen(Dk *dk)
{
    DkWin r = {};
    r.title = "screen";
    if (dk->wayland) {
        if (!portalopen(dk->wl, dk->tmp).data) {
            r.rect = wlscreen(dk->wl);
        }
        return r;
    }
    XWindowAttributes wa = {};
    dk->x.XGetWindowAttributes(dk->dpy, dk->root, &wa);
    r.rect = makerect(0, 0, wa.width, wa.height);
    return r;
}

// The window with the keyboard, on a desktop that cannot say which: the
// screen's rectangle, since where it lies is not known either.
static DkWin focusedwin(Dk *dk)
{
    DkWin r = {};
    r.id    = WL_FOCUSED;
    r.rect  = dk_screen(dk).rect;
    r.title = "(the window with the keyboard)";
    return r;
}

static DkWin dk_window(Dk *dk, Arena *a, u64 id)
{
    if (dk->wayland && id==WL_FOCUSED) {
        return focusedwin(dk);
    } else if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return wlwindow(dk->wl, a, id);
    } else if (dk->wayland && !dk->xwl) {
        return {};
    }
    return id ? describe(dk, a, (Xid)id).win : DkWin{};
}

static u64 x11foreground(Dk *dk);

static u64 dk_foreground(Dk *dk)
{
    if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return wlforeground(dk->wl, dk->tmp);
    } else if (dk->wayland) {
        // An X window, or else one of Wayland's that cannot be named.
        // While a Wayland window has the keyboard, Mutter calls a window
        // of its own active, which is no window to the user.
        u64 x = dk->xwl ? x11foreground(dk) : 0;
        if (x) {
            Arena t = dk->tmp;
            XWin  w = describe(dk, &t, (Xid)x);
            DkRect r = w.geom.frame;
            x = w.win.id && w.geom.viewable && !w.geom.popup &&
                r.right-r.left>1 && r.bottom-r.top>1 ? x : 0;
        }
        return x ? x : WL_FOCUSED;
    }
    return x11foreground(dk);
}

static u64 x11foreground(Dk *dk)
{
    if (dk->has[A_ACTIVE]) {
        Card c = getcard(dk, dk->root, dk->atom[A_ACTIVE], XA_WINDOW);
        return c.ok ? (u64)(Xid)c.val : 0;
    }
    Xid w      = 0;
    i32 revert = 0;
    dk->x.XGetInputFocus(dk->dpy, &w, &revert);
    if (w <= 1) {
        return 0;  // None, or PointerRoot
    }
    Xid top = toplevel(dk, dk->tmp, w);
    Xid c   = top ? clientin(dk, dk->tmp, top, 2) : 0;
    return c ? c : top;
}

static b32 dk_popup(Dk *dk, DkWin *w)
{
    if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return metapopup(w->level & 0xffff);
    }
    XWindowAttributes wa = {};
    if (dk->x.XGetWindowAttributes(dk->dpy, (Xid)w->id, &wa) && wa.override_redirect) {
        return 1;
    }
    Arena a = dk->tmp;
    Prop  p = getprop(dk, &a, (Xid)w->id, dk->atom[A_TYPE], XA_ATOM, 16);
    for (iz i = 0; i < p.len; i++) {
        for (i32 k = A_MENU; k <= A_DND; k++) {
            if ((Xid)propitem(p, i) == dk->atom[k]) {
                return 1;
            }
        }
    }
    return 0;
}

// The X focus: None (0), PointerRoot (1), or a window.
static Xid focus(Dk *dk)
{
    Xid f      = 0;
    i32 revert = 0;
    dk->x.XGetInputFocus(dk->dpy, &f, &revert);
    return f;
}

// Whether keys go to w: the X focus is inside its top-level window (its
// frame, it, or a child of either), and a manager that speaks EWMH also
// calls it active. Focus that follows the pointer confirms nothing.
static b32 haskeyboard(Dk *dk, Xid w)
{
    if (dk->has[A_ACTIVE] && x11foreground(dk)!=w) {
        return 0;
    }
    Xid f   = focus(dk);
    Xid top = f>1 ? toplevel(dk, dk->tmp, w) : 0;
    return top && toplevel(dk, dk->tmp, f)==top;
}

// Why a window did not get the keyboard: what has it instead.
static Str refused(Dk *dk, DkWin *w)
{
    Arena *perm = &dk->tmp;  // stays, for dk_why
    Xid    f    = focus(dk);
    Xid    top  = f>1 ? toplevel(dk, dk->tmp, f) : 0;
    Xid    c    = top ? clientin(dk, dk->tmp, top, 2) : 0;
    Str    has  = top ? gettitle(dk, perm, c ? c : top) : Str{};
    Buf    b(perm, 128);
    print(&b, "cannot give the keyboard to the window '");
    print(&b, w->title);
    print(&b, "'");
    if (f == 1) {
        print(&b, ", whose focus follows the pointer");
    } else if (top && top==toplevel(dk, dk->tmp, (Xid)w->id)) {
        print(&b, ", which the window manager would not make active");
    } else if (top) {
        print(&b, ", which '");
        print(&b, has);
        print(&b, "' has");
    }
    print(&b, "; no input was sent");
    return finish(&b);
}

// Through the manager when it speaks EWMH, which also raises and maps a
// minimized window, else directly: focus to the window itself. Done
// only when the keyboard is confirmed to go to it (haskeyboard).
static b32 dk_activate(Dk *dk, DkWin *w)
{
    if (dk->wayland && w->id==WL_FOCUSED) {
        return 1;  // has the keyboard by definition
    } else if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return wlactivate(dk->wl, dk->tmp, w);
    }
    Xid id = (Xid)w->id;
    b32 ok = haskeyboard(dk, id);
    for (i32 attempt = 0; !ok && attempt < 2; attempt++) {
        xerror = 0;
        if (!attempt && dk->has[A_ACTIVE]) {
            Xlong l[5] = {2, 0, 0, 0, 0};  // from a pager, which managers obey
            sendroot(dk, id, dk->atom[A_ACTIVE], l);
        } else {
            dk->x.XRaiseWindow(dk->dpy, id);
            dk->x.XSetInputFocus(dk->dpy, id, X_REVERTTOPARENT, 0);
        }
        dk->x.XSync(dk->dpy, 0);
        for (i32 i = 0; !ok && i < 25; i++) {
            ok = haskeyboard(dk, id);
            dk_sleep(dk, ok ? 50 : 20);  // once there, let it repaint
        }
    }
    if (!ok) {
        dk->failed = refused(dk, w);
        return 0;
    }
    Geom g = geometry(dk, dk->tmp, id);
    if (g.ok) {
        w->rect = g.frame;
    }
    return 1;
}


// Screenshots

struct Shift {
    i32 shift;  // of the lowest bit
    u32 max;    // largest value, 0 if no mask
};

static Shift maskshift(Xid mask)
{
    Shift r = {};
    if (!mask) {
        return r;
    }
    for (; !(mask>>r.shift & 1); r.shift++) {}
    r.max = (u32)(mask >> r.shift);
    for (; r.max & (r.max+1); r.max >>= 1) {}  // only the low run of ones
    return r;
}

static u8 channel(u32 p, Shift s)
{
    if (!s.max) {
        return 0;
    }
    return (u8)(((p>>s.shift & s.max)*255 + s.max/2) / s.max);
}

// An image's pixels as 32-bit BGRX, top-down; null for a format with
// pixels smaller than a byte.
static u8 *tobgrx(Arena *a, XImage *im, i32 w, i32 h)
{
    i32 bpp = im->bits_per_pixel;
    if (bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32) {
        return 0;
    }
    u8 *dst  = allocbytes(a, (iz)w*h*4);
    b32 fast = bpp==32 && im->byte_order==X_LSBFIRST && im->red_mask==0xff0000 &&
               im->green_mask==0xff00 && im->blue_mask==0xff;
    if (fast) {
        for (i32 y = 0; y < h; y++) {
            copybytes(dst + (iz)y*w*4, im->data + (iz)y*im->bytes_per_line, (iz)w*4);
        }
        return dst;
    }
    Shift r     = maskshift(im->red_mask);
    Shift g     = maskshift(im->green_mask);
    Shift b     = maskshift(im->blue_mask);
    i32   bytes = bpp / 8;
    for (i32 y = 0; y < h; y++) {
        u8 *row = im->data + (iz)y*im->bytes_per_line;
        for (i32 x = 0; x < w; x++) {
            u8 *src = row + (iz)x*bytes;
            u32 p   = 0;
            for (i32 k = 0; k < bytes; k++) {
                i32 at = im->byte_order==X_LSBFIRST ? bytes-1-k : k;
                p = p<<8 | src[at];
            }
            u8 *q = dst + ((iz)y*w + x)*4;
            q[0] = channel(p, b);
            q[1] = channel(p, g);
            q[2] = channel(p, r);
            q[3] = 0;
        }
    }
    return dst;
}

// Through a shared memory segment, or null if that fails, after which
// it is not tried again: a display elsewhere cannot attach one.
static u8 *shmcapture(Dk *dk, Arena *a, DkRect r)
{
    i32 w = r.right - r.left;
    i32 h = r.bottom - r.top;
    Xlib *x = &dk->x;
    XShmSegmentInfo info = {};
    void *visual = x->XDefaultVisual(dk->dpy, dk->screen);
    u32   depth  = (u32)x->XDefaultDepth(dk->dpy, dk->screen);
    XImage *im = x->XShmCreateImage(dk->dpy, visual, depth, X_ZPIXMAP, 0,
                                    &info, (u32)w, (u32)h);
    if (!im) {
        dk->shm = 0;
        return 0;
    }
    u8 *r32 = 0;
    info.shmid = shmget(IPC_PRIVATE, (uz)im->bytes_per_line*(uz)h, IPC_CREAT|0600);
    if (info.shmid >= 0) {
        void *p = shmat(info.shmid, 0, 0);
        if (p != (void *)-1) {
            info.shmaddr = (u8 *)p;
            im->data     = info.shmaddr;
            xerror = 0;
            b32 attached = x->XShmAttach(dk->dpy, &info);
            x->XSync(dk->dpy, 0);
            shmctl(info.shmid, IPC_RMID, 0);  // gone once both detach
            attached &= !xerror;
            if (attached && x->XShmGetImage(dk->dpy, dk->root, im, r.left, r.top, ~0ul)) {
                r32 = tobgrx(a, im, w, h);
            }
            if (attached) {
                x->XShmDetach(dk->dpy, &info);
                x->XSync(dk->dpy, 0);
            } else {
                dk->shm = 0;
            }
            shmdt(p);
        } else {
            shmctl(info.shmid, IPC_RMID, 0);
            dk->shm = 0;
        }
    } else {
        dk->shm = 0;
    }
    im->data = 0;  // not the image's to free
    im->f.destroy_image(im);
    return r32;
}

// The screen is in pixels, its units, so a capture is the size asked.
static u8 *dk_capture(Dk *dk, Arena *a, DkRect r, DkSize *size)
{
    if (dk->wayland) {
        return wlcapture(dk->wl, a, r, size);
    }
    size->w = r.right - r.left;
    size->h = r.bottom - r.top;
    u8 *r32 = dk->shm ? shmcapture(dk, a, r) : 0;
    if (r32) {
        return r32;
    }
    xerror = 0;
    XImage *im = dk->x.XGetImage(dk->dpy, dk->root, r.left, r.top,
                                 (u32)(r.right - r.left), (u32)(r.bottom - r.top),
                                 ~0ul, X_ZPIXMAP);
    if (!im) {
        return 0;
    }
    r32 = tobgrx(a, im, r.right - r.left, r.bottom - r.top);
    im->f.destroy_image(im);
    return r32;
}


// Input

static b32 flushed(Dk *dk)
{
    xerror = 0;
    dk->x.XSync(dk->dpy, 0);
    return !xerror;
}

static b32 dk_pointer(Dk *dk, i32 x, i32 y)
{
    if (dk->wayland) {
        return wlpointer(dk->wl, dk->tmp, x, y);
    }
    dk->x.XTestFakeMotionEvent(dk->dpy, -1, x, y, 0);
    return flushed(dk);
}

static b32 dk_button(Dk *dk, i32 button, b32 down)
{
    if (dk->wayland) {
        return wlbutton(dk->wl, dk->tmp, button, down);
    }
    dk->x.XTestFakeButtonEvent(dk->dpy, button ? 3 : 1, down, 0);
    return flushed(dk);
}

// Buttons 4 and 5 are the wheel up and down, a click a notch.
static b32 dk_wheel(Dk *dk, i32 notches)
{
    if (dk->wayland) {
        return wlwheel(dk->wl, dk->tmp, notches);
    }
    u32 button = notches>0 ? 4 : 5;
    i32 n      = notches>0 ? notches : -notches;
    for (i32 i = 0; i < n; i++) {
        dk->x.XTestFakeButtonEvent(dk->dpy, button, 1, 0);
        dk->x.XTestFakeButtonEvent(dk->dpy, button, 0, 0);
    }
    return flushed(dk);
}

// GTK's default threshold, Qt's is 10
static i32 dk_dragstep(Dk *)
{
    return 8;
}

struct Key {
    i32 code;   // 0 if no key makes it
    b32 shift;
};

// The key that makes a keysym, without shift if one does.
static Key findkey(Dk *dk, Xid sym)
{
    Key r = {};
    for (i32 col = 0; col<2 && col<dk->cols; col++) {
        for (i32 k = dk->minkey; k <= dk->maxkey; k++) {
            if (dk->keys[k][col] == sym) {
                r.code  = k;
                r.shift = col == 1;
                return r;
            }
        }
    }
    return r;
}

// Latin-1 keysyms are the code points, the rest of Unicode is offset.
static Xid keysymof(i32 c)
{
    return (c>=0x20 && c<0x7f) || (c>=0xa0 && c<=0xff) ? (Xid)c : 0x01000000ul|(Xid)c;
}

static i32 dk_keycode(Dk *dk, Str k, b32 *shift)
{
    static struct { Str name; Xid sym; } const names[] = {
        {"ctrl",  0xffe3}, {"control", 0xffe3}, {"alt",    0xffe9},
        {"shift", 0xffe1}, {"win",     0xffeb}, {"super",  0xffeb},
        {"cmd",   0xffe3}, {"command", 0xffe3}, {"meta",   0xffeb},
        {"enter", 0xff0d}, {"return",  0xff0d}, {"tab",    0xff09},
        {"esc",   0xff1b}, {"escape",  0xff1b}, {"space",  0x0020},
        {"backspace", 0xff08}, {"bs",  0xff08},
        {"del",   0xffff}, {"delete",  0xffff}, {"ins",    0xff63},
        {"insert",0xff63}, {"home",    0xff50}, {"end",    0xff57},
        {"pgup",  0xff55}, {"pageup",  0xff55}, {"pgdn",   0xff56},
        {"pagedown", 0xff56},
        {"left",  0xff51}, {"up",      0xff52}, {"right",  0xff53},
        {"down",  0xff54}, {"apps",    0xff67}, {"menu",   0xff67},
        {"pause", 0xff13}, {"prtsc",   0xff61}, {"plus",   0x003d},
        {"minus", 0x002d}, {"comma",   0x002c},
    };
    *shift = 0;
    Xid sym = 0;
    for (iz i = 0; !sym && i < countof(names); i++) {
        if (equalfold(k, names[i].name)) {
            sym = names[i].sym;
        }
    }
    if (!sym && k.len>=2 && k.len<=3 && lowercase(k[0])=='f') {
        Int n = parseint(cuthead(k, 1));
        if (n.ok && n.val>=1 && n.val<=24) {
            sym = 0xffbe + (Xid)n.val - 1;  // F1
        }
    }
    if (!sym && k.len) {
        // One character: letters as themselves, never shifted
        iz  i = 0;
        i32 c = utf8decode(k, &i);
        if (i == k.len && c > ' ') {
            c   = c>='A' && c<='Z' ? c+'a'-'A' : c;
            sym = dk->wayland ? wlsymof(dk->wl, c) : keysymof(c);
        }
    }
    if (dk->wayland) {
        // Keysyms, found in the desktop's keymap as they are pressed
        return sym ? (i32)sym : -1;
    }
    Key key = sym ? findkey(dk, sym) : Key{};
    if (!key.code) {
        return -1;
    }
    *shift = key.shift;
    return key.code;
}

static b32 dk_keys(Dk *dk, i32 *codes, i32 n)
{
    if (dk->wayland) {
        return wlkeys(dk->wl, dk->tmp, codes, n);
    }
    for (i32 i = 0; i < n; i++) {
        dk->x.XTestFakeKeyEvent(dk->dpy, (u32)codes[i], 1, 0);
    }
    for (i32 i = n-1; i >= 0; i--) {
        dk->x.XTestFakeKeyEvent(dk->dpy, (u32)codes[i], 0, 0);
    }
    return flushed(dk);
}

// Bind a keycode to a keysym, the same shifted or not, or with none,
// unbind it, in the keyboard as this helper knows it.
static void cachekey(Dk *dk, i32 code, Xid sym)
{
    for (i32 c = 0; c < KEYCOLS; c++) {
        dk->keys[code][c] = c<2 ? sym : 0;
    }
}

// The same in the server's keyboard, for every client.
static void bindkey(Dk *dk, i32 code, Xid sym)
{
    Xid row[KEYCOLS] = {};
    i32 cols = dk->cols>1 ? dk->cols : 2;
    row[0] = row[1] = sym;
    dk->x.XChangeKeyboardMapping(dk->dpy, code, cols, row, 1);
    cachekey(dk, code, sym);
}

// Whether the server's keyboard binds a keycode to a keysym as bindkey
// does, and to nothing else: XKB repeats it in each group it reports.
static b32 boundas(Dk *dk, i32 code, Xid sym)
{
    i32  cols = 0;
    Xid *row  = dk->x.XGetKeyboardMapping(dk->dpy, (u8)code, 1, &cols);
    if (!row) {
        return 0;
    }
    b32 r = cols>=1 && row[0]==sym;
    for (i32 c = 1; c < cols; c++) {
        r &= row[c]==sym || !row[c];
    }
    dk->x.XFree(row);
    return r;
}


// The record of bindings
//
// A helper killed outright cannot unbind its keys, so each records what
// it binds in a property of the root window, _PROSPERO_DESKTOP_KEYS, as
// CARDINAL triples: the owner, an input-only window of the helper's, a
// keycode, and the keysym bound to it. A binding is recorded before it
// is made, and while a key is rebound, both its keysyms are. Before each
// command, every helper unbinds the keys of owners that are gone, those
// still bound exactly as recorded, which nothing else has changed since,
// and drops their records. An owner window is gone with its helper's
// connection, and carries the same property, unlike a later client's
// window that the server gave the same id. Records change under a server
// grab, since helpers may share a display.

enum : iz {
    RECORDMAX = 3 << 14,  // items of the record read, of every helper
};

static void putrecords(Dk *dk, Xlong *items, iz len)
{
    if (len) {
        dk->x.XChangeProperty(dk->dpy, dk->root, dk->atom[A_KEYS], XA_CARDINAL,
                              32, X_PROPMODEREPLACE, (u8 *)items, (i32)len);
    } else {
        dk->x.XDeleteProperty(dk->dpy, dk->root, dk->atom[A_KEYS]);
    }
}

// Whether the helper that recorded a binding is still there.
static b32 ownerlives(Dk *dk, Xid owner)
{
    xerror = 0;  // BadWindow, once it has gone
    return owner==dk->owner || getcard(dk, owner, dk->atom[A_KEYS], XA_CARDINAL).ok;
}

// Replace this helper's records with its bindings as they are now.
static void recordkeys(Dk *dk)
{
    Xlib *x = &dk->x;
    if (!dk->owner) {
        dk->owner = x->XCreateWindow(dk->dpy, dk->root, -1, -1, 1, 1, 0, 0,
                                     X_INPUTONLY, 0, 0, 0);
        Xlong pid = getpid();
        x->XChangeProperty(dk->dpy, dk->owner, dk->atom[A_KEYS], XA_CARDINAL,
                           32, X_PROPMODEREPLACE, (u8 *)&pid, 1);
    }
    Arena a = dk->tmp;
    x->XGrabServer(dk->dpy);
    Prop   p     = getprop(dk, &a, dk->root, dk->atom[A_KEYS], XA_CARDINAL, RECORDMAX);
    Xlong *items = alloc<Xlong>(&a, p.len + 6*SPARES);
    iz     n     = 0;
    for (iz i = 0; i+2 < p.len; i += 3) {
        if ((Xid)propitem(p, i) != dk->owner) {
            items[n++] = propitem(p, i);
            items[n++] = propitem(p, i+1);
            items[n++] = propitem(p, i+2);
        }
    }
    for (i32 i = 0; i < dk->nbound; i++) {
        Bound *b = &dk->bound[i];
        for (i32 k = 0; k < 2; k++) {
            Xid sym = k ? b->prev : b->sym;
            if (sym) {
                items[n++] = (Xlong)dk->owner;
                items[n++] = b->code;
                items[n++] = (Xlong)sym;
            }
        }
    }
    putrecords(dk, items, n);
    x->XUngrabServer(dk->dpy);
    x->XSync(dk->dpy, 0);
}

// Unbind the keys recorded by helpers that are gone, if still bound as
// recorded, and drop their records. Returns how many it unbound. Then
// the keycodes that other helpers recorded are not spare, even before
// they are bound.
static i32 recoverkeys(Dk *dk)
{
    Arena a     = dk->tmp;
    Prop  p     = getprop(dk, &a, dk->root, dk->atom[A_KEYS], XA_CARDINAL, RECORDMAX);
    b32   stale = 0;
    for (iz i = 0; !stale && i+2 < p.len; i += 3) {
        stale = !ownerlives(dk, (Xid)propitem(p, i));
    }
    if (!stale) {
        reserved(dk, p);
        return 0;
    }

    i32 n = 0;
    dk->x.XGrabServer(dk->dpy);
    p = getprop(dk, &a, dk->root, dk->atom[A_KEYS], XA_CARDINAL, RECORDMAX);
    Xlong *keep = alloc<Xlong>(&a, p.len);
    iz     len  = 0;
    for (iz i = 0; i+2 < p.len; i += 3) {
        Xid   owner = (Xid)propitem(p, i);
        Xlong code  = propitem(p, i+1);
        Xid   sym   = (Xid)propitem(p, i+2);
        if (ownerlives(dk, owner)) {
            keep[len++] = (Xlong)owner;
            keep[len++] = code;
            keep[len++] = (Xlong)sym;
        } else if (code>=dk->minkey && code<=dk->maxkey && sym &&
                   boundas(dk, (i32)code, sym)) {
            bindkey(dk, (i32)code, 0);
            n++;
        }
    }
    putrecords(dk, keep, len);
    dk->x.XUngrabServer(dk->dpy);
    dk->x.XSync(dk->dpy, 0);
    Prop kept = {};
    kept.data = (u8 *)keep;
    kept.len  = len;
    reserved(dk, kept);
    return n;
}

// Drop from the spare keycodes those that another helper recorded.
static void reserved(Dk *dk, Prop p)
{
    for (iz i = 0; i+2 < p.len; i += 3) {
        if ((Xid)propitem(p, i) == dk->owner) {
            continue;
        }
        for (i32 k = 0; k < dk->nspare; k++) {
            if (dk->spare[k] == propitem(p, i+1)) {
                dk->spare[k--] = dk->spare[--dk->nspare];
            }
        }
    }
}


// A keycode for a keysym that no key makes: a spare one, or else the
// binding of this helper's typed longest ago, but not in this run (used
// before now), so far only as this helper knows the keyboard: pending
// until dk_text records and makes it. 0 if there is none.
static i32 bindfor(Dk *dk, Xid sym, i64 now)
{
    Bound *b = 0;
    if (dk->nspare && dk->nbound<SPARES) {
        b = &dk->bound[dk->nbound++];
        b->code = dk->spare[--dk->nspare];
        b->prev = 0;
    } else {
        for (i32 i = 0; i < dk->nbound; i++) {
            Bound *c = &dk->bound[i];
            b = c->used<now && (!b || c->used<b->used) ? c : b;
        }
        if (!b) {
            return 0;
        }
        // Its last key may not be looked up yet
        i64 wait = b->used + GRACE - dk_ms(dk);
        if (wait > 0) {
            dk->x.XSync(dk->dpy, 0);
            dk_sleep(dk, (i32)wait);
        }
        b->prev = b->pending ? b->prev : b->sym;
    }
    b->sym     = sym;
    b->used    = now;
    b->pending = 1;
    cachekey(dk, b->code, sym);
    return b->code;
}

// Record the pending bindings, then make them.
static void bindpending(Dk *dk)
{
    recordkeys(dk);
    for (i32 i = 0; i < dk->nbound; i++) {
        Bound *b = &dk->bound[i];
        if (b->pending) {
            bindkey(dk, b->code, b->sym);
            b->pending = 0;
            b->prev    = 0;
        }
    }
}

// Mark a key as typed in this run, if it is one this helper bound.
static void markused(Dk *dk, i32 code, i64 now)
{
    for (i32 i = 0; i < dk->nbound; i++) {
        if (dk->bound[i].code == code) {
            dk->bound[i].used = now;
        }
    }
}

// Unbind every keycode this helper bound, once its last key has had
// time to be looked up, or at once when asked to quit, then its record.
static void unbindall(Dk *dk)
{
    if (!dk->dpy || !dk->nbound) {
        return;
    }
    i64 last = 0;
    for (i32 i = 0; i < dk->nbound; i++) {
        last = dk->bound[i].used>last ? dk->bound[i].used : last;
    }
    i64 wait = last + GRACE - dk_ms(dk);
    if (wait > 0) {
        dk->x.XSync(dk->dpy, 0);
        nap(wait);
    }
    for (i32 i = 0; i < dk->nbound; i++) {
        bindkey(dk, dk->bound[i].code, 0);
    }
    dk->nbound = 0;
    recordkeys(dk);
}

// Type keysyms in runs: each as long as the spare keycodes can hold the
// characters in it that no key makes. Those stay bound until this helper
// exits: a client looks a key up when it gets to its event, and a slow
// one gets there after an unbinding that followed the key at once.
static b32 dk_text(Dk *dk, Arena scratch, Str s)
{
    if (dk->wayland) {
        return wltext(dk->wl, scratch, s);
    }
    Slice<Xid> syms = {};
    for (iz i = 0; i < s.len;) {
        i32 c = utf8decode(s, &i);
        if (c == '\r') {
            if (i<s.len && s[i]=='\n') {
                continue;
            }
            c = '\n';
        }
        if (c == '\n') {
            syms = push(&scratch, syms, (Xid)0xff0d);  // Return
        } else if (c == '\t') {
            syms = push(&scratch, syms, (Xid)0xff09);  // Tab
        } else if (c>=' ' && c!=0x7f && !(c>=0x80 && c<0xa0)) {
            syms = push(&scratch, syms, keysymof(c));
        }
    }

    Key *keys  = alloc<Key>(&scratch, syms.len);
    Key  shift = findkey(dk, 0xffe1);  // Shift_L
    b32  ok    = 1;
    for (iz i = 0; ok && i < syms.len;) {
        i64 now   = dk_ms(dk) + 1;  // after every earlier use
        b32 bound = 0;
        iz  end   = i;
        for (; end < syms.len; end++) {
            Key k = findkey(dk, syms[end]);
            k.shift &= shift.code != 0;
            if (!k.code) {
                k.code = bindfor(dk, syms[end], now);
                if (!k.code) {
                    break;  // to be typed in the next run
                }
                bound = 1;
            }
            markused(dk, k.code, now);
            keys[end] = k;
        }
        if (end == i) {
            return 0;  // no spare keycodes at all
        }

        if (bound) {
            // Clients learn of a new mapping from an event, which they
            // should have before the keys of it
            bindpending(dk);
            dk->x.XSync(dk->dpy, 0);
            dk_sleep(dk, 30);
        }
        for (; ok && i < end; i++) {
            if (keys[i].shift) {
                dk->x.XTestFakeKeyEvent(dk->dpy, (u32)shift.code, 1, 0);
            }
            dk->x.XTestFakeKeyEvent(dk->dpy, (u32)keys[i].code, 1, 0);
            dk->x.XTestFakeKeyEvent(dk->dpy, (u32)keys[i].code, 0, 0);
            if (keys[i].shift) {
                dk->x.XTestFakeKeyEvent(dk->dpy, (u32)shift.code, 0, 0);
            }
            if (i%32 == 31) {
                ok = flushed(dk);
                dk_sleep(dk, 1);
            }
        }
        ok &= flushed(dk);
        i64 typed = dk_ms(dk);
        for (i32 b = 0; b < dk->nbound; b++) {
            Bound *k = &dk->bound[b];
            k->used = k->used>=now ? typed : k->used;
        }
    }
    return ok;
}


// Windows' lives

// Through the manager, else as a manager would: WM_DELETE_WINDOW to a
// client that takes it. One that does not is not closed by force.
// The window with the keyboard, when it cannot be named, cannot be
// acted on either.
static b32 unnamed(Dk *dk, DkWin *w)
{
    if (dk->wayland && w->id==WL_FOCUSED) {
        dk->wl->failed = concat(&dk->wl->perm, "cannot act on a window that "
                                "cannot be named: ", dk->wl->winwhy);
        return 1;
    }
    return 0;
}

static b32 dk_close(Dk *dk, DkWin *w)
{
    if (unnamed(dk, w)) {
        return 0;
    } else if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return wlclose(dk->wl, dk->tmp, w);
    }
    Xid id = (Xid)w->id;
    xerror = 0;
    if (dk->has[A_CLOSE]) {
        Xlong l[5] = {0, 2, 0, 0, 0};  // no timestamp, from a pager
        sendroot(dk, id, dk->atom[A_CLOSE], l);
        return flushed(dk);
    }
    Arena a = dk->tmp;
    Prop  p = getprop(dk, &a, id, dk->atom[A_PROTOCOLS], XA_ATOM, 32);
    b32   takes = 0;
    for (iz i = 0; i < p.len; i++) {
        takes |= (Xid)propitem(p, i) == dk->atom[A_DELETE];
    }
    if (!takes) {
        dk->failed = "the window takes no WM_DELETE_WINDOW, and with no "
                     "window manager that speaks EWMH, nothing else asks "
                     "it to close";
        return 0;
    }
    XEvent ev = {};
    ev.xclient.type         = X_CLIENTMESSAGE;
    ev.xclient.window       = id;
    ev.xclient.message_type = dk->atom[A_PROTOCOLS];
    ev.xclient.format       = 32;
    ev.xclient.l[0]         = (Xlong)dk->atom[A_DELETE];
    dk->x.XSendEvent(dk->dpy, id, 0, 0, &ev);
    return flushed(dk);
}

// Arguments are in the visible-frame terms of list and shot: the client
// goes where that puts it, through the manager with static gravity, in
// the client's own terms, else directly.
static b32 dk_move(Dk *dk, DkWin *w, i32 x, i32 y, i32 cw, i32 ch, b32 size)
{
    if (unnamed(dk, w)) {
        return 0;
    } else if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        return wlmove(dk->wl, dk->tmp, w, x, y, cw, ch, size);
    }
    Xid  id = (Xid)w->id;
    Geom g  = geometry(dk, dk->tmp, id);
    if (!g.ok) {
        return 0;
    }
    x  += g.client.left - g.frame.left;
    y  += g.client.top  - g.frame.top;
    cw -= (g.frame.right - g.frame.left) - (g.client.right - g.client.left);
    ch -= (g.frame.bottom - g.frame.top) - (g.client.bottom - g.client.top);
    if (size && (cw<1 || ch<1)) {
        return 0;
    } else if (dk->xk > 1) {
        x  = (i32)(x * dk->xk);
        y  = (i32)(y * dk->xk);
        cw = (i32)(cw * dk->xk);
        ch = (i32)(ch * dk->xk);
    }
    xerror = 0;
    if (dk->has[A_MOVERESIZE]) {
        Xlong flags = X_STATICGRAVITY | 1<<8 | 1<<9 | (size ? 1<<10 | 1<<11 : 0) | 2<<12;
        Xlong l[5]  = {flags, x, y, cw, ch};
        sendroot(dk, id, dk->atom[A_MOVERESIZE], l);
    } else if (size) {
        dk->x.XMoveResizeWindow(dk->dpy, id, x, y, (u32)cw, (u32)ch);
    } else {
        dk->x.XMoveWindow(dk->dpy, id, x, y);
    }
    return flushed(dk);
}

// Running programs
//
// A program run outlives the turn and the harness, as on Windows, where
// it leaves the job, and on macOS, where LaunchServices starts it. Here
// the helper runs under the harness's reaper, a child subreaper, which
// ends whatever came from the tool however it detaches: setsid and
// double forks only make it the reaper's child. So the user's service
// manager starts it instead, as a transient service of its own
// (systemd-run --user), as desktops start applications: in the user's
// app.slice, its parent the manager, outside the reaper's tree, with
// the helper's environment and directory. ExitType=cgroup keeps the
// service up while anything in it runs, as after a launcher that hands
// off to a child and exits. Without a user manager, a desktop entry
// that D-Bus activates goes through gio launch, which asks the session
// bus to start it, and anything else runs in the helper's tree, in a
// session of its own, and ends with the harness: attached.
//
// The program is a path, a name found in PATH, or a desktop entry: a
// .desktop file, or its id (org.gnome.TextEditor) where no program has
// that name, whose Exec line runs, with the arguments in place of its
// file or URL field code.

enum : i64 {
    RUNWAIT  = 8000,  // milliseconds a program the helper waits for may take
    BUSWAIT  = 5000,  // and an activated one, to take its D-Bus name
};

static char **cargv(Arena *a, Slice<Str> argv)
{
    char **r = alloc<char *>(a, argv.len+1);
    for (iz i = 0; i < argv.len; i++) {
        r[i] = cstr(a, argv[i]);
    }
    return r;
}

// Spawn with default signals, standard input null, output and errors to
// out, or null if it is -1, and with session, in a session of its own.
// An errno value on failure.
static i32 spawn(pid_t *pid, char **argv, i32 out, b32 session)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    for (i32 fd = 1; fd < 3; fd++) {
        if (out >= 0) {
            posix_spawn_file_actions_adddup2(&fa, out, fd);
        } else {
            posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", O_WRONLY, 0);
        }
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (session) {
        #ifdef POSIX_SPAWN_SETSID
        flags |= POSIX_SPAWN_SETSID;
        #else
        flags |= POSIX_SPAWN_SETPGROUP;
        #endif
    }
    posix_spawnattr_setflags(&attr, flags);
    i32 err = posix_spawnp(pid, argv[0], &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return err;
}

struct Ran {
    Str out;     // the start of its output and errors
    i32 status;  // its exit status, -1 if it did not run, or end by itself
};

// Run a program to its end, for RUNWAIT at most.
static Ran runwait(Dk *dk, Arena *a, Slice<Str> argv)
{
    Ran r = {};
    r.status = -1;
    i32 fds[2];
    if (pipe2(fds, O_CLOEXEC)) {
        return r;
    }
    pid_t pid = 0;
    i32   err = spawn(&pid, cargv(a, argv), fds[1], 0);
    close(fds[1]);
    if (err) {
        close(fds[0]);
        return r;
    }

    iz  cap  = 1<<16;
    u8 *buf  = allocbytes(a, cap);
    iz  len  = 0;
    i64 end  = dk_ms(dk) + RUNWAIT;
    b32 done = 0;
    for (;;) {
        i64 left = end - dk_ms(dk);
        struct pollfd pf = {};
        pf.fd     = fds[0];
        pf.events = POLLIN;
        i32 n = left>0 ? poll(&pf, 1, (i32)left) : 0;
        if (n<0 && errno==EINTR) {
            continue;
        } else if (n <= 0) {
            break;  // out of time
        }
        u8  discard[4096];
        b32 room = len < cap;
        iz  got  = read(fds[0], room ? buf+len : discard,
                        room ? (uz)(cap-len) : sizeof(discard));
        if (got<0 && errno==EINTR) {
            continue;
        } else if (got <= 0) {
            done = 1;
            break;
        }
        len += room ? got : 0;
    }
    close(fds[0]);
    if (!done) {
        kill(pid, SIGKILL);  // its own child, not yet waited for
    }
    i32 st = 0;
    while (waitpid(pid, &st, 0)<0 && errno==EINTR) {}
    r.out    = {buf, len};
    r.status = done && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

static Str firstline(Str s)
{
    return trim(cut(trim(s), '\n').head);
}

static Str readfile(Arena *a, Str path)
{
    Str r  = {};
    i32 fd = open(cstr(a, path), O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return r;
    }
    iz cap = 1<<20;
    r.data = allocbytes(a, cap);
    for (;;) {
        iz got = read(fd, r.data+r.len, (uz)(cap-r.len));
        if (got<0 && errno==EINTR) {
            continue;
        } else if (got <= 0) {
            break;
        }
        r.len += got;
    }
    close(fd);
    a->beg = (byte *)(r.data + r.len);  // give back the rest
    return r;
}

// A program as execvp finds it: its path, or null with errno set.
static Str findprog(Arena *a, Str name)
{
    if (!name.len) {
        errno = ENOENT;
        return {};
    } else if (cut(name, '/').ok) {
        return access(cstr(a, name), X_OK) ? Str{} : name;
    }
    Str path = fromcstr(getenv("PATH"));
    path = path.data ? path : Str("/usr/local/bin:/usr/bin:/bin");
    i32 why  = ENOENT;
    for (Str rest = path;;) {
        Cut         c    = cut(rest, ':');
        Str         dir  = c.head.len ? c.head : Str(".");
        Str         full = concat(a, concat(a, dir, "/"), name);
        char       *z    = cstr(a, full);
        struct stat st   = {};
        if (!stat(z, &st) && S_ISREG(st.st_mode)) {
            if (!access(z, X_OK)) {
                return full;
            }
            why = EACCES;
        }
        if (!c.ok) {
            break;
        }
        rest = c.tail;
    }
    errno = why;
    return {};
}

// Fail with "what 'name': why", in the dk's arena, which outlives the
// command's scratch.
static void failwith(Dk *dk, Str what, Str name, Str why)
{
    Buf b(&dk->tmp, 64 + name.len + why.len);
    print(&b, what);
    print(&b, " '");
    print(&b, name);
    print(&b, "': ");
    print(&b, why);
    dk->failed = finish(&b);
}


// Desktop entries

struct Entry {
    Str        path;  // the file
    Str        id;    // its name less .desktop, which D-Bus activates
    Slice<Str> argv;  // its Exec line, with the arguments in place
    b32        dbus;  // DBusActivatable
};

// The directories of desktop entries, the first to look in first.
static Slice<Str> appdirs(Arena *a)
{
    Slice<Str> r    = {};
    Str        home = fromcstr(getenv("XDG_DATA_HOME"));
    Str        user = fromcstr(getenv("HOME"));
    if (!home.len && user.len) {
        home = join(a, user, ".local/share");
    }
    if (home.len) {
        r = push(a, r, join(a, home, "applications"));
    }
    Str dirs = fromcstr(getenv("XDG_DATA_DIRS"));
    dirs = dirs.len ? dirs : Str("/usr/local/share:/usr/share");
    for (Str rest = dirs; rest.len;) {
        Cut c = cut(rest, ':');
        rest = c.tail;
        if (c.head.len) {
            r = push(a, r, join(a, c.head, "applications"));
        }
    }
    return r;
}

// A desktop entry's file by a path to it, or by its id; null if none.
static Str findentry(Arena *a, Str name)
{
    b32 named = endswith(name, ".desktop");
    if (named && cut(name, '/').ok) {
        return access(cstr(a, name), R_OK) ? Str{} : name;
    } else if (cut(name, '/').ok || !name.len) {
        return {};
    }
    Str        file = named ? name : concat(a, name, ".desktop");
    Slice<Str> dirs = appdirs(a);
    for (iz i = 0; i < dirs.len; i++) {
        Str path = join(a, dirs[i], file);
        if (!access(cstr(a, path), R_OK)) {
            return path;
        }
    }
    return {};
}

// An Exec value split into words: after the string escapes (\s \n \t
// \r \\), double quotes group a word, inside which a backslash escapes
// the next character. %f %F %u %U, each a word of its own, give way to
// the arguments, at the first of them; %% is %, and the other field
// codes (%i %c %k, and deprecated ones) are dropped.
static Slice<Str> execwords(Arena *a, Str value, Slice<Str> args)
{
    Buf u(a, value.len + 1);
    for (iz i = 0; i < value.len; i++) {
        u8 c = value[i];
        if (c=='\\' && i+1<value.len) {
            u8 e = value[++i];
            switch (e) {
            case 's':  c = ' ';  break;
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '\\': c = '\\'; break;
            default:   putbyte(&u, '\\');  // for the quoting below
                       c = e;
            }
        }
        putbyte(&u, c);
    }
    Str v = finish(&u);

    Slice<Str> r    = {};
    b32        used = 0;
    for (iz i = 0; i < v.len;) {
        for (; i<v.len && (v[i]==' ' || v[i]=='\t' || v[i]=='\n'); i++) {}
        if (i == v.len) {
            break;
        }
        Buf w(a, 64);
        b32 quoted = 0;
        b32 field  = 0;  // a file or URL field code
        b32 any    = 0;
        for (; i<v.len && (quoted || (v[i]!=' ' && v[i]!='\t' && v[i]!='\n')); i++) {
            u8 c = v[i];
            if (c == '"') {
                quoted = !quoted;
                any    = 1;
            } else if (c=='\\' && i+1<v.len) {
                putbyte(&w, v[++i]);
                any = 1;
            } else if (c=='%' && i+1<v.len) {
                u8 f = v[++i];
                if (f == '%') {
                    putbyte(&w, '%');
                    any = 1;
                }
                field |= f=='f' || f=='F' || f=='u' || f=='U';
            } else {
                putbyte(&w, c);
                any = 1;
            }
        }
        if (field && !w.len) {
            for (iz k = 0; !used && k < args.len; k++) {
                r = push(a, r, args[k]);
            }
            used = 1;
        } else if (any) {
            r = push(a, r, finish(&w));
        }
    }
    return r;
}

// The [Desktop Entry] group's Exec and DBusActivatable.
static Entry loadentry(Arena *a, Str path, Slice<Str> args)
{
    Entry r = {};
    r.path = path;
    Str base = path;
    for (Cut c = cut(base, '/'); c.ok; c = cut(base, '/')) {
        base = c.tail;
    }
    r.id = endswith(base, ".desktop") ? cuttail(base, 8) : base;

    Str text  = readfile(a, path);
    b32 group = 0;
    for (Str rest = text; rest.len;) {
        Cut line = cut(rest, '\n');
        rest = line.tail;
        Str l = trim(line.head);
        if (l.len && l[0]=='[') {
            group = l == "[Desktop Entry]";
            continue;
        }
        Cut kv = cut(l, '=');
        if (!group || !kv.ok) {
            continue;
        }
        Str key   = trim(kv.head);
        Str value = trim(kv.tail);
        if (key == "Exec") {
            r.argv = execwords(a, value, args);
        } else if (key == "DBusActivatable") {
            r.dbus = value == "true";
        }
    }
    return r;
}


// Starting it

// Whether an environment variable may go to the service: a valid name
// and value, as systemd checks them, and not one of those the manager
// gave this process about itself.
static b32 passenv(Str var)
{
    static Str const own[] = {
        "NOTIFY_SOCKET", "LISTEN_PID", "LISTEN_PIDFDID", "LISTEN_FDS",
        "LISTEN_FDNAMES", "INVOCATION_ID", "JOURNAL_STREAM", "MANAGERPID",
        "SYSTEMD_EXEC_PID", "WATCHDOG_PID", "WATCHDOG_USEC", "MAINPID",
        "MEMORY_PRESSURE_WATCH", "MEMORY_PRESSURE_WRITE", "TRIGGER_UNIT",
        "TRIGGER_PATH", "TRIGGER_TIMER_REALTIME_USEC",
        "TRIGGER_TIMER_MONOTONIC_USEC", "CREDENTIALS_DIRECTORY",
    };
    Cut c = cut(var, '=');
    if (!c.ok || !c.head.len || digit(c.head[0])) {
        return 0;
    }
    for (iz i = 0; i < c.head.len; i++) {
        if (!alnum(c.head[i]) && c.head[i]!='_') {
            return 0;
        }
    }
    for (iz i = 0; i < countof(own); i++) {
        if (c.head == own[i]) {
            return 0;
        }
    }
    Str v = c.tail;
    for (iz i = 0; i < v.len;) {
        u8  b  = v[i];
        iz  at = i;
        i32 cp = utf8decode(v, &i);
        b32 cc = (b<0x20 && b!='\t' && b!='\n') || b==0x7f;
        if (cc || (b>=0x80 && (cp==b || i-at<2))) {
            return 0;  // a control character, or not UTF-8
        }
    }
    return 1;
}

// A unit name of systemd's convention for applications,
// app-LAUNCHER-APPID-RANDOM.service, the app id from the program's name.
static Str unitname(Dk *dk, Arena *a, Str prog)
{
    static u32 serial;
    Str base = prog;
    for (Cut c = cut(base, '/'); c.ok; c = cut(base, '/')) {
        base = c.tail;
    }
    Buf b(a, 128);
    print(&b, "app-prospero-");
    for (iz i = 0; i < base.len && i < 64; i++) {
        u8 c = base[i];
        putbyte(&b, alnum(c) || c=='_' || c=='.' ? c : '_');
    }
    putbyte(&b, '-');
    printhex(&b, (u64)getpid(), 8);
    printhex(&b, (u64)dk_ms(dk)<<8 | (serial++ & 0xff), 12);
    print(&b, ".service");
    return finish(&b);
}

static u32 topid(Str s)
{
    Int v = parseint(s);
    return v.ok && v.val>1 && v.val<=0x7fffffff ? (u32)v.val : 0;
}

// The service's main process, or else the first one left in its control
// group, as after a launcher that exited; 0 if none.
static u32 unitpid(Dk *dk, Arena scratch, Str systemctl, Str unit)
{
    Slice<Str> cmd = {};
    cmd = push(&scratch, cmd, systemctl);
    cmd = push(&scratch, cmd, Str("--user"));
    cmd = push(&scratch, cmd, Str("show"));
    cmd = push(&scratch, cmd, Str("--property=MainPID"));
    cmd = push(&scratch, cmd, Str("--property=ControlGroup"));
    cmd = push(&scratch, cmd, unit);
    Ran x  = runwait(dk, &scratch, cmd);
    Str cg = {};
    for (Str rest = x.status ? Str{} : x.out; rest.len;) {
        Cut line = cut(rest, '\n');
        rest = line.tail;
        Cut kv = cut(trim(line.head), '=');
        if (kv.head=="MainPID" && topid(kv.tail)) {
            return topid(kv.tail);
        } else if (kv.head == "ControlGroup") {
            cg = kv.tail;
        }
    }
    if (!cg.len) {
        return 0;
    }
    // cgroup v2, else v1's hybrid or legacy layout
    Str roots[] = {"/sys/fs/cgroup", "/sys/fs/cgroup/unified", "/sys/fs/cgroup/systemd"};
    for (iz i = 0; i < countof(roots); i++) {
        Str path = concat(&scratch, concat(&scratch, roots[i], cg), "/cgroup.procs");
        u32 pid  = topid(firstline(readfile(&scratch, path)));
        if (pid) {
            return pid;
        }
    }
    return 0;
}

struct Started {
    u32 pid;  // the program's, if any is left of it
    b32 ran;  // the manager started it
    Str why;  // why it did not, if not: in the dk's arena
};

// As a transient service of the user's service manager.
static Started startunit(Dk *dk, Arena scratch, Slice<Str> argv)
{
    Started r      = {};
    Str     sysrun = findprog(&scratch, "systemd-run");
    Str     sysctl = findprog(&scratch, "systemctl");
    if (!sysrun.data || !sysctl.data) {
        r.why = "systemd-run is not installed";
        return r;
    }
    Str unit = unitname(dk, &scratch, argv[0]);
    for (i32 attempt = 0; !r.ran && attempt < 2; attempt++) {
        Arena      a   = scratch;
        Slice<Str> cmd = {};
        cmd = push(&a, cmd, sysrun);
        cmd = push(&a, cmd, Str("--user"));
        cmd = push(&a, cmd, Str("--quiet"));
        cmd = push(&a, cmd, Str("--collect"));
        cmd = push(&a, cmd, Str("--service-type=exec"));
        cmd = push(&a, cmd, Str("--same-dir"));
        cmd = push(&a, cmd, Str("--slice=app.slice"));
        cmd = push(&a, cmd, concat(&a, "--unit=", unit));
        cmd = push(&a, cmd, concat(&a, "--description=Prospero: ", argv[0]));
        if (!attempt) {
            cmd = push(&a, cmd, Str("--property=ExitType=cgroup"));
        }
        for (char **e = environ; *e; e++) {
            Str var = fromcstr(*e);
            if (passenv(var)) {
                cmd = push(&a, cmd, concat(&a, "--setenv=", var));
            }
        }
        cmd = push(&a, cmd, Str("--"));
        for (iz i = 0; i < argv.len; i++) {
            cmd = push(&a, cmd, argv[i]);
        }
        Ran x = runwait(dk, &a, cmd);
        r.ran = !x.status;
        if (!r.ran) {
            Str line = firstline(x.out);
            r.why = clone(&dk->tmp, line.len ? line : Str("systemd-run failed"));
            if (x.status<0 || find(x.out, "ExitType")<0) {
                return r;  // only an older manager is tried again, without it
            }
        }
    }
    if (r.ran) {
        r.pid = unitpid(dk, scratch, sysctl, unit);
        r.why = {};
    }
    return r;
}

// D-Bus activation of a desktop entry, through gio launch: the session
// bus starts it, apart from this tree. Its pid is the owner's of its
// well-known name, its id, once it takes it.
static u32 activate(Dk *dk, Arena scratch, Entry *e, Slice<Str> args, Str *why)
{
    Str gio   = findprog(&scratch, "gio");
    Str gdbus = findprog(&scratch, "gdbus");
    if (!gio.data || !gdbus.data) {
        return 0;
    }
    Slice<Str> cmd = {};
    cmd = push(&scratch, cmd, gio);
    cmd = push(&scratch, cmd, Str("launch"));
    cmd = push(&scratch, cmd, e->path);
    for (iz i = 0; i < args.len; i++) {
        cmd = push(&scratch, cmd, args[i]);
    }
    Ran x = runwait(dk, &scratch, cmd);
    if (x.status) {
        Str line = firstline(x.out);
        *why = clone(&dk->tmp, line.len ? line : Str("gio launch failed"));
        return 0;
    }
    for (i64 end = dk_ms(dk) + BUSWAIT; dk_ms(dk) < end;) {
        Arena      a = scratch;
        Slice<Str> q = {};
        q = push(&a, q, gdbus);
        q = push(&a, q, Str("call"));
        q = push(&a, q, Str("--session"));
        q = push(&a, q, Str("--dest=org.freedesktop.DBus"));
        q = push(&a, q, Str("--object-path=/org/freedesktop/DBus"));
        q = push(&a, q, Str("--method=org.freedesktop.DBus.GetConnectionUnixProcessID"));
        q = push(&a, q, e->id);
        Ran o   = runwait(dk, &a, q);
        Str num = cut(firstline(o.out), ' ').tail;  // "(uint32 1234,)"
        u32 pid = topid(cut(num, ',').head);
        if (!o.status && pid) {
            return pid;
        }
        dk_sleep(dk, 100);
    }
    *why = "it was activated, but did not take its D-Bus name in time";
    return 0;
}

static u32 dk_run(Dk *dk, Arena scratch, Slice<Str> argv, b32 *attached)
{
    *attached = 0;
    Slice<Str> args = {argv.data+1, argv.len-1, argv.len-1};
    Str        name = argv[0];
    Str        path = endswith(name, ".desktop") ? findentry(&scratch, name) : Str{};
    Str        prog = path.data ? Str{} : findprog(&scratch, name);
    i32        err  = errno;
    Entry      e    = {};
    if (!prog.data) {
        path = path.data ? path : findentry(&scratch, name);
        if (path.data) {
            e = loadentry(&scratch, path, args);
            if (!e.argv.len && !e.dbus) {
                failwith(dk, "cannot start", name, "the desktop entry has no Exec line");
                return 0;
            }
            prog = e.argv.len ? findprog(&scratch, e.argv[0]) : Str{};
            err  = errno;
            argv = e.argv;
        }
    }
    if (!prog.data && !e.dbus) {
        failwith(dk, "cannot start", argv.len ? argv[0] : name, fromcstr(strerror(err)));
        return 0;
    }

    Str why = {};
    if (prog.data) {
        Slice<Str> v = {};
        v = push(&scratch, v, prog);
        for (iz i = 1; i < argv.len; i++) {
            v = push(&scratch, v, argv[i]);
        }
        argv = v;
        Started s = startunit(dk, scratch, argv);
        if (s.pid) {
            return s.pid;
        } else if (s.ran) {
            failwith(dk, "cannot start", name, "it ended as soon as it started, "
                     "perhaps handing its work to one already running");
            return 0;
        }
        why = s.why;
    }
    if (e.dbus) {
        u32 pid = activate(dk, scratch, &e, args, &why);
        if (pid || !prog.data) {
            if (!pid) {
                why = why.len ? why : Str("gio is not installed");
                failwith(dk, "cannot start", name, why);
            }
            return pid;
        }
    }

    // In this tree after all, which the reaper ends with the harness
    pid_t pid = 0;
    err = spawn(&pid, cargv(&scratch, argv), -1, 1);
    if (err) {
        failwith(dk, "cannot start", name, fromcstr(strerror(err)));
        return 0;
    }
    *attached = 1;
    Buf b(&dk->tmp, 64 + why.len);
    print(&b, "no user service manager would start it (");
    print(&b, why.len ? why : Str("unknown"));
    print(&b, ")");
    dk->failed = finish(&b);
    return (u32)pid;
}

// A process's state letter from /proc, 0 if it is gone.
static u8 procstate(u32 pid)
{
    char path[32] = "/proc/";
    char digits[16];
    iz   len = 6;
    iz   n   = 0;
    for (u32 v = pid; v; v /= 10) {
        digits[n++] = (char)('0' + v%10);
    }
    while (n) {
        path[len++] = digits[--n];
    }
    copybytes(path+len, (void *)"/stat", 6);
    i32 fd = open(path, O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    u8 buf[512];
    iz got = read(fd, buf, sizeof(buf));
    close(fd);
    // "pid (comm) S ...", where comm may hold anything up to the last )
    iz paren = -1;
    for (iz i = 0; i < got; i++) {
        paren = buf[i]==')' ? i : paren;
    }
    return paren>=0 && paren+2<got ? buf[paren+2] : 0;
}

// Under Wayland with no way to see its windows, any process counts as
// having one.
static b32 haswindow(Dk *dk, u32 pid)
{
    if (dk->wayland && dk->wl->winapi!=WIN_NONE) {
        Arena        a  = dk->tmp;
        Slice<DkWin> ws = wlwindows(dk->wl, &a, 0);
        for (iz i = 0; i < ws.len; i++) {
            if (ws[i].pid == pid) {
                return 1;
            }
        }
        return 0;
    } else if (dk->wayland) {
        return 1;  // its window may be Wayland's, which cannot be seen
    }
    Arena a = dk->tmp;
    Arena t = {};  // apart from the list, which grows in a
    iz    n = (iz)1 << 20;
    t.beg = (byte *)allocbytes(&a, n);
    t.end = t.beg + n;
    Slice<Xid> ids = toplevels(dk, &a, t);
    for (iz i = 0; i < ids.len; i++) {
        Card c = getcard(dk, ids[i], dk->atom[A_PID], XA_CARDINAL);
        if (c.ok && (u32)c.val==pid && geometry(dk, a, ids[i]).viewable) {
            return 1;
        }
    }
    return 0;
}

// X has no WaitForInputIdle: a process waits for input once it shows a
// window and sleeps, twice in a row 50 ms apart.
static i32 dk_idle(Dk *dk, u32 pid, i32 ms)
{
    i64 end    = dk_ms(dk) + ms;
    i32 asleep = 0;
    for (;;) {
        u8 st = procstate(pid);
        if (!st || st=='Z' || st=='X') {
            return 2;
        }
        asleep = (st=='S' || st=='I') && haswindow(dk, pid) ? asleep+1 : 0;
        if (asleep == 2) {
            return 0;
        } else if (dk_ms(dk) >= end) {
            return 1;
        }
        dk_sleep(dk, 50);
    }
}

// Every command sleeps between steps, which is where it stops when the
// helper is asked to quit.
static void dk_sleep(Dk *dk, i32 ms)
{
    nap(ms);
    if (quitting) {
        unbindall(dk);
        _exit(DK_FAIL);
    }
}

static i64 dk_ms(Dk *)
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}


// Standard I/O

static iz dk_read(Dk *, u8 *buf, iz len)
{
    for (;;) {
        if (quitting) {
            return 0;  // as at the end
        }
        iz n = read(0, buf, (uz)(len<(1<<20) ? len : 1<<20));
        if (n<0 && errno==EINTR) {
            continue;
        }
        return n<0 ? -1 : n;
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


#if PROSPERO_DESKTOP
// "prospero desktop ARGS...", "desktop" first in place of a program name
extern "C" int prospero_desktop(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    // Standard descriptors must exist, or the display's connection may
    // take one, and output would go into it
    for (i32 fd = open("/dev/null", O_RDWR); fd>=0 && fd<=2;) {
        fd = dup(fd);
    }
    // A reply to a harness that has gone fails rather than kills. The
    // programs it waits for are reaped by their pids (runwait), and
    // those run in its tree before each command (dk_ready).
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {};
    sa.sa_handler = onquit;  // no SA_RESTART: a blocked read returns
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT,  &sa, 0);
    sigaction(SIGHUP,  &sa, 0);
    // Nothing inherited reaches the programs run
    for (i32 fd = 3; fd < 1024; fd++) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    iz      cap = sizeof(void *)==8 ? (iz)1<<32 : (iz)1<<29;
    Region *r   = reserve(cap);
    Region *t   = reserve((iz)1<<26);
    Region *w   = reserve((iz)1<<28);
    if (!r || !t || !w) {
        os_oom();
    }
    Arena a  = newarena(r);
    Dk   *dk = alloc<Dk>(&a);
    dk->tmp = newarena(t);
    dk->wl  = alloc<Wl>(&a);
    dk->wl->perm = newarena(w);

    Str *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i] = fromcstr(argv[i]);
    }
    i32 status = dkmain(dk, a, args, argc);
    unbindall(dk);
    return status;
}
