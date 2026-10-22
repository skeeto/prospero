// Desktop helper, Linux X11 platform layer
// $ c++ -std=c++20 -o prospero-desktop src/main_desktop_x11.cpp
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
// outright leaves its bindings, which later ones use as keys.
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
// Wayland is not supported yet: a Wayland session is refused, rather
// than half-driven through Xwayland.
//
// This is free and unencumbered software released into the public domain.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>  // getenv
#include <string.h>  // strerror
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include "base.cpp"
#include "deskhelp.cpp"

extern char **environ;

using Xid   = decltype(0ul);  // XID, Window, Atom, KeySym, Time
using Xlong = decltype(0l);
struct Display;

enum : i32 {
    X_ISVIEWABLE      = 2,   // map_state
    X_INPUTONLY       = 2,   // class
    X_ZPIXMAP         = 2,
    X_LSBFIRST        = 0,
    X_CLIENTMESSAGE   = 33,
    X_REVERTTOPARENT  = 2,
    X_STATICGRAVITY   = 10,

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
    A_WMSTATE, A_PROTOCOLS, A_DELETE,
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
    "WM_STATE", "WM_PROTOCOLS", "WM_DELETE_WINDOW",
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
    i64 used;  // when it was last typed, dk_ms
};

struct Dk {
    Xlib      x;
    Display  *dpy;
    Xid       root;
    i32       screen;
    Str       why;          // set once loading failed for good
    Str       failed;       // why the latest call failed, for dk_why
    b32       shm;          // MIT-SHM may be tried
    Xid       atom[A_COUNT];
    b32       ewmh;         // a manager speaks EWMH, with
    b32       has[A_COUNT]; // these in its _NET_SUPPORTED

    // The keyboard: keysyms by keycode, and the spare keycodes
    i32       minkey, maxkey, cols;
    Xid       keys[256][KEYCOLS];
    u8        spare[SPARES];
    i32       nspare;
    Bound     bound[SPARES];  // by this helper, until it exits
    i32       nbound;

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
        iz size = format==32 ? (iz)sizeof(Xlong) : format==16 ? 2 : 1;
        r.len  = (iz)n<max ? (iz)n : max;
        r.data = allocbytes(a, r.len*size);
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
    dk->ewmh = 0;
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
    dk->ewmh = 1;
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

// The windows that are windows to the user, top first: the manager's
// clients, under the popups (override-redirect, as menus are) over
// them, or without EWMH, the root's children, or their clients.
static Slice<Xid> toplevels(Dk *dk, Arena *a, Arena tmp)
{
    Slice<Xid> r    = {};
    Kids       kids = querytree(dk, &tmp, dk->root);
    if (dk->ewmh) {
        for (iz i = kids.len-1; i >= 0; i--) {
            XWindowAttributes wa = {};
            if (dk->x.XGetWindowAttributes(dk->dpy, kids.ids[i], &wa) &&
                    wa.override_redirect && wa.map_state==X_ISVIEWABLE &&
                    wa.cls!=X_INPUTONLY) {
                r = push(a, r, kids.ids[i]);
            }
        }
        Prop p = getprop(dk, &tmp, dk->root, dk->atom[A_STACKING], XA_WINDOW, 1<<16);
        if (!p.ok) {
            // Mapping order, which is no z-order, but the best there is
            p = getprop(dk, &tmp, dk->root, dk->atom[A_CLIENTS], XA_WINDOW, 1<<16);
        }
        if (p.ok) {
            for (iz i = p.len-1; i >= 0; i--) {
                r = push(a, r, (Xid)propitem(p, i));
            }
            return r;
        }
        r = {};  // a manager without a list
    }
    for (iz i = kids.len-1; i >= 0; i--) {
        Xid c = clientin(dk, tmp, kids.ids[i], 2);
        r = push(a, r, c ? c : kids.ids[i]);
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
    return r;
}

// X asks no permission of a client of the display.
static b32 dk_access(Dk *, Buf *, b32)
{
    return 1;
}

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

static Str dk_ready(Dk *dk)
{
    if (dk->why.data) {
        return dk->why;
    }
    Arena *perm = &dk->tmp;  // what stays allocated here stays for good
    Str    session = fromcstr(getenv("XDG_SESSION_TYPE"));
    Str    wayland = fromcstr(getenv("WAYLAND_DISPLAY"));
    Str    display = fromcstr(getenv("DISPLAY"));
    if (session=="wayland" || (wayland.len && !display.len)) {
        return "this is a Wayland session, and Wayland is not supported "
               "yet: the desktop helper works with X11 only";
    } else if (!display.len) {
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
    loadkeys(dk);
    return {};
}

static Slice<DkWin> dk_windows(Dk *dk, Arena *a, b32 all)
{
    Slice<Xid>   ids = toplevels(dk, a, dk->tmp);
    Slice<DkWin> r   = {};
    for (iz i = 0; i < ids.len; i++) {
        XWin w = describe(dk, a, ids[i]);
        if (w.win.id && (all || w.geom.viewable)) {
            r = push(a, r, w.win);
        }
    }
    return r;
}

static DkWin dk_window(Dk *dk, Arena *a, u64 id)
{
    return id ? describe(dk, a, (Xid)id).win : DkWin{};
}

static DkWin dk_screen(Dk *dk)
{
    DkWin             r  = {};
    XWindowAttributes wa = {};
    dk->x.XGetWindowAttributes(dk->dpy, dk->root, &wa);
    r.rect  = makerect(0, 0, wa.width, wa.height);
    r.title = "screen";
    return r;
}

static u64 dk_foreground(Dk *dk)
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
    if (dk->has[A_ACTIVE] && dk_foreground(dk)!=w) {
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
    dk->x.XTestFakeMotionEvent(dk->dpy, -1, x, y, 0);
    return flushed(dk);
}

static b32 dk_button(Dk *dk, i32 button, b32 down)
{
    dk->x.XTestFakeButtonEvent(dk->dpy, button ? 3 : 1, down, 0);
    return flushed(dk);
}

// Buttons 4 and 5 are the wheel up and down, a click a notch.
static b32 dk_wheel(Dk *dk, i32 notches)
{
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
            sym = keysymof(c);
        }
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
    for (i32 i = 0; i < n; i++) {
        dk->x.XTestFakeKeyEvent(dk->dpy, (u32)codes[i], 1, 0);
    }
    for (i32 i = n-1; i >= 0; i--) {
        dk->x.XTestFakeKeyEvent(dk->dpy, (u32)codes[i], 0, 0);
    }
    return flushed(dk);
}

// Bind a keycode to a keysym, the same shifted or not, or with none,
// unbind it.
static void bindkey(Dk *dk, i32 code, Xid sym)
{
    Xid row[KEYCOLS] = {};
    i32 cols = dk->cols>1 ? dk->cols : 2;
    row[0] = row[1] = sym;
    dk->x.XChangeKeyboardMapping(dk->dpy, code, cols, row, 1);
    for (i32 c = 0; c < KEYCOLS; c++) {
        dk->keys[code][c] = row[c];
    }
}

// A keycode for a keysym that no key makes: a spare one bound to it, or
// else the binding of this helper's typed longest ago, but not in this
// run (used before now). 0 if there is none.
static i32 bindfor(Dk *dk, Xid sym, i64 now)
{
    Bound *b = 0;
    if (dk->nspare && dk->nbound<SPARES) {
        b = &dk->bound[dk->nbound++];
        b->code = dk->spare[--dk->nspare];
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
    }
    b->sym  = sym;
    b->used = now;
    bindkey(dk, b->code, sym);
    return b->code;
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
// time to be looked up, or at once when asked to quit.
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
    dk->x.XSync(dk->dpy, 0);
}

// Type keysyms in runs: each as long as the spare keycodes can hold the
// characters in it that no key makes. Those stay bound until this helper
// exits: a client looks a key up when it gets to its event, and a slow
// one gets there after an unbinding that followed the key at once.
static b32 dk_text(Dk *dk, Arena scratch, Str s)
{
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
static b32 dk_close(Dk *dk, DkWin *w)
{
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

// In a session of its own, with null standard streams and default
// signals. It cannot leave the harness's reaper (a subreaper), which
// ends what is left of a tool when the harness exits.
static u32 dk_run(Dk *dk, Arena scratch, Slice<Str> argv, b32 *attached)
{
    *attached = 0;
    char **args = alloc<char *>(&scratch, argv.len+1);
    for (iz i = 0; i < argv.len; i++) {
        args[i] = cstr(&scratch, argv[i]);
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (i32 fd = 0; fd < 3; fd++) {
        posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    #ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;
    #else
    flags |= POSIX_SPAWN_SETPGROUP;
    #endif
    posix_spawnattr_setflags(&attr, flags);

    pid_t pid = 0;
    i32   err = posix_spawnp(&pid, args[0], &fa, &attr, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (err) {
        // In the dk's arena, which outlives this scratch
        Buf b(&dk->tmp, 64);
        print(&b, "cannot start '");
        print(&b, argv[0]);
        print(&b, "': ");
        print(&b, fromcstr(strerror(err)));
        dk->failed = finish(&b);
        return 0;
    }
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

static b32 haswindow(Dk *dk, u32 pid)
{
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


int main(int argc, char **argv)
{
    // Standard descriptors must exist, or the display's connection may
    // take one, and output would go into it
    for (i32 fd = open("/dev/null", O_RDWR); fd>=0 && fd<=2;) {
        fd = dup(fd);
    }
    // A reply to a harness that has gone fails rather than kills, and
    // the programs run are reaped by the system
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
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
    if (!r || !t) {
        os_oom();
    }
    Arena a  = newarena(r);
    Dk   *dk = alloc<Dk>(&a);
    dk->tmp = newarena(t);

    Str *args = alloc<Str>(&a, argc);
    for (i32 i = 0; i < argc; i++) {
        args[i] = fromcstr(argv[i]);
    }
    i32 status = dkmain(dk, a, args, argc);
    unbindall(dk);
    return status;
}
