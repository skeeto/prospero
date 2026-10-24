// GUI platform for macOS and Linux: SDL3 and OpenGL 3
// Included by main_posix.cpp after the core when PROSPERO_GUI is set, so
// it implements the GUI's os_* functions directly.
//
// Wakes from other threads are coalesced with an atomic flag: a producer
// pushes a wake event only when the flag was clear, and the loop clears
// it before draining, so every record posted after a drain began gets a
// wake of its own.
//
// macOS resizes a window in a modal loop that SDL_WaitEvent does not
// return from. SDL reports it through an event watch with expose events
// (data1 1) at 60 Hz, and the watch draws frames from there, and drains
// the mailbox, so streaming continues during the drag. GL functions are
// looked up through SDL rather than linked, so that one Linux binary
// still starts without display libraries.
//
// SIGINT and SIGTERM (Ctrl+C in a terminal, a logout) ask to close the
// window, as quitting from the Dock does. main blocks them before any
// thread starts, and one thread takes them with sigwait and pushes a
// quit event, which wakes the loop: SDL's own handlers only set a flag
// for the next event pump, which an idle loop never runs.
//
// The window may go while the program runs on (os_guished), keeping
// SDL and its event loop for wakes and a tray icon (macOS: the menu
// bar; Linux: a status notifier, through appindicator loaded at run
// time, or none), whose menu picks arrive as events of our own; a window
// comes back through os_guiopen. While only the icon is left, the app
// has no Dock icon on macOS (an accessory app). AppKit's reopen (the
// Dock's icon clicked, or the app launched again through Launch
// Services) comes as the tray's Open, by a method added at run time to
// SDL's app delegate, so that it brings a shed window back too. SDL
// waits for events on a window, and with none it polls instead, every
// millisecond (3-4% of a core): a hidden window stands in while shed.
//
// SDL reports the system's theme (macOS: the app's effective appearance;
// Linux: the desktop portal's color-scheme, unknown without one, taken
// as dark) and its changes as events. On macOS the window's frame takes
// the theme in effect through its NSWindow's appearance, set through
// the Objective-C runtime; elsewhere the window manager draws it. On
// Linux, SDL reads the portal's signal only when its event loop runs,
// and an idle loop waits on the display connection alone, so a thread
// of ours watches for the signal on a D-Bus connection of its own
// (libdbus, loaded as SDL loads it) and brings the change in at once.
//
// The window opens where it was last, its client area's place and size
// fitted to the displays as they are (place.cpp), the title bar counted
// above it where SDL knows its height; under Wayland, where a window
// never knows its place, only its size. Moves, resizes, maximizing,
// restoring and full screen go to the core (gui_placed), which saves
// them. On macOS a window can fill its display without being zoomed
// (Fill, tiling), which counts as maximized; and zooming toggles, so a
// window opened maximized from a box that fills its display already
// starts from a smaller one, else AppKit would take it as zoomed and
// shrink it.
//
// Scrolling: macOS trackpads' momentum (the scroll that goes on after a
// flick), which SDL turns off unless asked, comes as more wheel events,
// each input like any other, so frames end as they stop. Neither X11
// nor Wayland has momentum: libinput leaves kinetic scrolling to the
// toolkit, and SDL ignores Wayland's axis source and stop events, which
// would tell a finger's lift, so none is made here.
//
// This is free and unencumbered software released into the public domain.

#ifdef __linux__
#  include <dlfcn.h>
#endif

#ifdef __APPLE__
// The Objective-C runtime and AppKit's appearance names, declared by
// hand: enough to set a window's appearance (macOS 10.14+) from C++.
extern "C" void *objc_getClass(char const *);
extern "C" void *sel_registerName(char const *);
extern "C" void  objc_msgSend();
extern "C" void *NSAppearanceNameAqua;      // NSString *
extern "C" void *NSAppearanceNameDarkAqua;

// Whether the window's view takes the click that activates it, as the
// click-through hint makes SDL answer AppKit.
static b32 firstmouse(SDL_Window *w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    void *win = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, 0);
    if (!win) {
        return 0;
    }
    using Get  = void *(*)(void *, void *);
    using Test = bool (*)(void *, void *, void *);
    void *view = ((Get)(void *)objc_msgSend)(win, sel_registerName("contentView"));
    return view && ((Test)(void *)objc_msgSend)(view, sel_registerName("acceptsFirstMouse:"), 0);
}

extern "C" void *objc_autoreleasePoolPush();
extern "C" void  objc_autoreleasePoolPop(void *);

// No window restoration, which the window's own saved place makes moot:
// after a crash AppKit would otherwise hold the launch at a modal "reopen
// its windows?" alert, before any window shows. Registered defaults, so
// the user's own settings are untouched. Before SDL_Init, which finishes
// the launch.
static void nopersist()
{
    using Get  = void *(*)(void *, void *);
    using Cstr = void *(*)(void *, void *, char const *);
    using Bool = void *(*)(void *, void *, bool);
    using Two  = void *(*)(void *, void *, void *, void *);
    using Set  = void  (*)(void *, void *, void *);
    void *pool = objc_autoreleasePoolPush();
    Cstr  str  = (Cstr)(void *)objc_msgSend;
    void *nstr = objc_getClass("NSString");
    void *no   = ((Bool)(void *)objc_msgSend)(objc_getClass("NSNumber"),
                                              sel_registerName("numberWithBool:"), false);
    void *yes  = ((Bool)(void *)objc_msgSend)(objc_getClass("NSNumber"),
                                              sel_registerName("numberWithBool:"), true);
    void *keys[] = {
        str(nstr, sel_registerName("stringWithUTF8String:"), "ApplePersistenceIgnoreState"),
        str(nstr, sel_registerName("stringWithUTF8String:"), "NSQuitAlwaysKeepsWindows"),
    };
    void *ka   = ((Two)(void *)objc_msgSend)(objc_getClass("NSArray"),
                                             sel_registerName("arrayWithObjects:count:"),
                                             keys, (void *)(uz)2);
    void *vals[] = {yes, no};
    void *va   = ((Two)(void *)objc_msgSend)(objc_getClass("NSArray"),
                                             sel_registerName("arrayWithObjects:count:"),
                                             vals, (void *)(uz)2);
    void *dict = ((Two)(void *)objc_msgSend)(objc_getClass("NSDictionary"),
                                             sel_registerName("dictionaryWithObjects:forKeys:"),
                                             va, ka);
    void *defs = ((Get)(void *)objc_msgSend)(objc_getClass("NSUserDefaults"),
                                             sel_registerName("standardUserDefaults"));
    if (yes && no && keys[0] && keys[1] && dict && defs) {
        ((Set)(void *)objc_msgSend)(defs, sel_registerName("registerDefaults:"), dict);
    }
    objc_autoreleasePoolPop(pool);
}
#endif

using GlViewport   = void (*)(i32, i32, i32, i32);
using GlClearColor = void (*)(f32, f32, f32, f32);
using GlClear      = void (*)(u32);
using GlReadPixels = void (*)(i32, i32, i32, i32, u32, u32, void *);

struct GuiSdl;

enum : i32 {
    FILL_SLACK = 16,  // points between a filling window's edge and its display's
};

struct TrayItem {
    GuiSdl *p;
    i32     item;  // TRAY_*
};

struct GuiSdl {
    Gui          *gui;
    SDL_Window   *window;      // null while shed (os_guished)
    SDL_Window   *waiter;      // hidden, while shed: SDL waits on it
    SDL_GLContext gl;
    SDL_Tray     *tray;
    TrayItem      items[3];
    u32           wakeevent;
    u32           trayevent;   // a tray menu pick: code is the TRAY_* item
    i32           wakeflag;    // atomic: a wake event is queued
    b32           inwatch;     // drawing from the event watch
    i32           framedark;   // the frame's theme: -1 unset, 0 light, 1 dark
    i32           drivetheme;  // the checklist's system theme: 1 light,
                               // 2 dark, 0 the real one
    i32           portal;      // atomic: the portal's latest, as drivetheme,
                               // 0 if it said nothing yet (Linux)
    WinBox        normal;      // the window when last neither maximized,
                               // minimized nor full screen
    b32           max;         // maximized when last not full screen
    GlViewport    glviewport;
    GlClearColor  glclearcolor;
    GlClear       glclear;
    GlReadPixels  glreadpixels;
};

static void os_wake(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (p && !__atomic_exchange_n(&p->wakeflag, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event e = {};
        e.type = p->wakeevent;
        SDL_PushEvent(&e);
    }
}

static void sigthread(void *)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    for (;;) {
        int sig = 0;
        if (!sigwait(&set, &sig)) {
            SDL_Event e = {};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        }
    }
}

// Whether the system prefers dark, or the checklist pretends it does.
static b32 sysdark(GuiSdl *p)
{
    i32 portal = __atomic_load_n(&p->portal, __ATOMIC_ACQUIRE);
    if (p->drivetheme) {
        return p->drivetheme == 2;
    } else if (portal) {
        return portal == 2;
    }
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
}

#ifdef __linux__
// libdbus, as much as the portal watch needs, declared by hand
using DbusInit     = u32   (*)();
using DbusGet      = void *(*)(i32, void *);
using DbusExit     = void  (*)(void *, u32);
using DbusMatch    = void  (*)(void *, char const *, void *);
using DbusRead     = u32   (*)(void *, i32);
using DbusPop      = void *(*)(void *);
using DbusIsSignal = u32   (*)(void *, char const *, char const *);
using DbusIterInit = u32   (*)(void *, void *);
using DbusIterNext = u32   (*)(void *);
using DbusIterType = i32   (*)(void *);
using DbusIterSub  = void  (*)(void *, void *);
using DbusIterGet  = void  (*)(void *, void *);
using DbusUnref    = void  (*)(void *);
using DbusFlush    = void  (*)(void *);

struct DbusIter {
    void *opaque[16];  // a DBusMessageIter, 72 bytes on 64-bit hosts
};

struct Portal {
    GuiSdl      *p;
    DbusGet      get;
    DbusExit     exitondisc;
    DbusMatch    match;
    DbusRead     read;
    DbusPop      pop;
    DbusIsSignal issignal;
    DbusIterInit iterinit;
    DbusIterNext iternext;
    DbusIterType itertype;
    DbusIterSub  itersub;
    DbusIterGet  iterget;
    DbusUnref    unref;
    DbusFlush    flush;
};

// The color-scheme of a SettingChanged signal: 1 dark, 2 light, 0 no
// preference, or -1 if it is not that setting.
static i32 portalscheme(Portal *d, void *msg)
{
    static char const *const want[] = {"org.freedesktop.appearance", "color-scheme"};
    DbusIter it = {};
    if (!d->iterinit(msg, &it)) {
        return -1;
    }
    for (i32 i = 0; i < 2; i++) {
        char const *s = 0;
        if (d->itertype(&it) != 's') {
            return -1;
        }
        d->iterget(&it, &s);
        if (!s || __builtin_strcmp(s, want[i]) || !d->iternext(&it)) {
            return -1;
        }
    }
    DbusIter v = {};
    if (d->itertype(&it) != 'v') {
        return -1;
    }
    d->itersub(&it, &v);
    u32 scheme = 0;
    if (d->itertype(&v) != 'u') {
        return -1;
    }
    d->iterget(&v, &scheme);
    return (i32)scheme;
}

static void portalthread(void *arg)
{
    Portal *d    = (Portal *)arg;
    void   *conn = d->get(0, 0);  // the session bus, privately
    if (!conn) {
        return;  // no session bus: no portal to follow
    }
    d->exitondisc(conn, 0);  // the bus going away is no reason to exit
    d->match(conn, "type='signal',interface='org.freedesktop.portal.Settings',"
                   "member='SettingChanged',arg0='org.freedesktop.appearance',"
                   "arg1='color-scheme'", 0);
    d->flush(conn);
    while (d->read(conn, -1)) {
        for (void *m; (m = d->pop(conn));) {
            i32 scheme = -1;
            if (d->issignal(m, "org.freedesktop.portal.Settings", "SettingChanged")) {
                scheme = portalscheme(d, m);
            }
            d->unref(m);
            if (scheme >= 0) {
                __atomic_store_n(&d->p->portal, scheme==2 ? 1 : 2, __ATOMIC_RELEASE);
                SDL_Event e = {};
                e.type = SDL_EVENT_SYSTEM_THEME_CHANGED;
                e.common.timestamp = SDL_GetTicksNS();
                SDL_PushEvent(&e);
            }
        }
    }
}

// Start the portal watch, if libdbus is there to watch with.
static void portalwatch(Os *os, Arena *perm, GuiSdl *p)
{
    void *h = dlopen("libdbus-1.so.3", RTLD_NOW|RTLD_LOCAL);
    if (!h) {
        return;
    }
    Portal  *d    = alloc<Portal>(perm);
    DbusInit init = (DbusInit)dlsym(h, "dbus_threads_init_default");
    d->p          = p;
    d->get        = (DbusGet)dlsym(h, "dbus_bus_get_private");
    d->exitondisc = (DbusExit)dlsym(h, "dbus_connection_set_exit_on_disconnect");
    d->match      = (DbusMatch)dlsym(h, "dbus_bus_add_match");
    d->read       = (DbusRead)dlsym(h, "dbus_connection_read_write");
    d->pop        = (DbusPop)dlsym(h, "dbus_connection_pop_message");
    d->issignal   = (DbusIsSignal)dlsym(h, "dbus_message_is_signal");
    d->iterinit   = (DbusIterInit)dlsym(h, "dbus_message_iter_init");
    d->iternext   = (DbusIterNext)dlsym(h, "dbus_message_iter_next");
    d->itertype   = (DbusIterType)dlsym(h, "dbus_message_iter_get_arg_type");
    d->itersub    = (DbusIterSub)dlsym(h, "dbus_message_iter_recurse");
    d->iterget    = (DbusIterGet)dlsym(h, "dbus_message_iter_get_basic");
    d->unref      = (DbusUnref)dlsym(h, "dbus_message_unref");
    d->flush      = (DbusFlush)dlsym(h, "dbus_connection_flush");
    b32 ok = init && d->get && d->exitondisc && d->match && d->read && d->pop &&
             d->issignal && d->iterinit && d->iternext && d->itertype &&
             d->itersub && d->iterget && d->unref && d->flush;
    if (ok && init()) {
        os_thread(os, portalthread, d);
    }
}
#endif

static f32 contentscale(SDL_Window *w)
{
    f32 s = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(w));
    return s>0 ? s : 1;
}

static void takewake(GuiSdl *p)
{
    if (__atomic_exchange_n(&p->wakeflag, 0, __ATOMIC_ACQ_REL)) {
        gui_wake(p->gui);
    }
}

// Runs on whatever thread pushed an event: only exposes, which SDL
// sends from the main thread, are handled here.
static bool SDLCALL sdlwatch(void *ud, SDL_Event *e)
{
    GuiSdl *p = (GuiSdl *)ud;
    if (e->type==SDL_EVENT_WINDOW_EXPOSED && e->window.data1==1 && !p->inwatch) {
        p->inwatch = 1;
        gui_modal(p->gui, 1);
        takewake(p);
        gui_force(p->gui);
        gui_frame(p->gui);
        gui_modal(p->gui, 0);
        p->inwatch = 0;
    }
    return 1;
}

static b32 sdlwindow(Os *, Gui *, GuiSdl *, Str title);

// Whether windows are Wayland's, whose compositor places them, so that
// a window knows neither where it is nor how to move.
static b32 wayland()
{
    char const *d = SDL_GetCurrentVideoDriver();
    return d && !__builtin_strcmp(d, "wayland");
}

static u32 os_guilimits(Os *)
{
    return wayland() ? GUI_NOPLACE|GUI_NOSERIAL : 0;
}

static i32 os_guidisplays(Os *, WinBox *out, i32 max)
{
    i32            n       = 0;
    i32            count   = 0;
    SDL_DisplayID  primary = SDL_GetPrimaryDisplay();
    SDL_DisplayID *ids     = SDL_GetDisplays(&count);
    for (i32 pass = 0; pass < 2; pass++) {
        for (i32 i = 0; ids && i<count && n<max; i++) {
            SDL_Rect r = {};
            if ((ids[i]==primary) == (pass==0) && SDL_GetDisplayUsableBounds(ids[i], &r)) {
                out[n++] = {r.x, r.y, r.w, r.h};
            }
        }
    }
    SDL_free(ids);
    return n;
}

// A geometry fitted to the displays as they are now (place.cpp), the
// title bar above the client area title high. Under Wayland only the
// size counts, fitted to the primary display.
static WinGeom fitgeom(Os *os, WinGeom geom, i32 title)
{
    WinBox disp[16];
    i32    n     = os_guidisplays(os, disp, countof(disp));
    WinBox frame = geom.box;
    frame.y -= title;
    frame.h += title;
    if (wayland() && n) {
        frame.x = disp[0].x;
        frame.y = disp[0].y;
        n = 1;
    }
    frame    = placewindow(frame, disp, n);
    geom.box = {frame.x, frame.y + title, frame.w, frame.h - title};
    return geom;
}

// The title bar's height, from the window's borders where SDL knows
// them (macOS; an X11 window manager's only once the window shows),
// else none, the client area's top standing for it.
static i32 titleheight(SDL_Window *w)
{
    i32 top = 0;
    return SDL_GetWindowBordersSize(w, &top, 0, 0, 0) && top>0 ? top : 0;
}

// Whether a client box, the title bar title high above it (roughly, if
// not yet known), fills one of the displays, near enough each edge: as
// macOS's Fill (tiling, or a title bar's double-click) leaves a window,
// never flagged maximized, and as zooming does. Never under Wayland,
// where boxes have no place.
static b32 filling(Os *os, WinBox box, i32 title)
{
    WinBox disp[16];
    i32    n = wayland() ? 0 : os_guidisplays(os, disp, countof(disp));
    title = title>0 ? title : PLACE_TITLE;
    box.y -= title;
    box.h += title;
    for (i32 i = 0; i < n; i++) {
        WinBox d       = disp[i];
        i32    edges[] = {box.x - d.x, box.y - d.y, d.x+d.w - (box.x+box.w),
                          d.y+d.h - (box.y+box.h)};
        b32    near    = 1;
        for (i32 e = 0; e < countof(edges); e++) {
            near &= edges[e]>=-FILL_SLACK && edges[e]<=FILL_SLACK;
        }
        if (near) {
            return 1;
        }
    }
    return 0;
}

// Put the window at a geometry, fitted to the displays. Maximizing on
// macOS toggles (NSWindow zoom:), and a box that fills its display
// already counts as zoomed, so it would go smaller instead: such a box,
// to be maximized, first shrinks by a quarter about its center.
static void sdlplace(Os *os, GuiSdl *p, WinGeom geom)
{
    i32 title = titleheight(p->window);
    geom = fitgeom(os, geom, title);
    if (geom.max && filling(os, geom.box, title)) {
        geom.box.x += geom.box.w/8;
        geom.box.y += geom.box.h/8;
        geom.box.w -= geom.box.w/4;
        geom.box.h -= geom.box.h/4;
    }
    p->normal = geom.box;
    SDL_SetWindowSize(p->window, geom.box.w, geom.box.h);
    if (!wayland()) {
        SDL_SetWindowPosition(p->window, geom.box.x, geom.box.y);
    }
}

// The window moved, resized, was maximized or restored, or went full
// screen or back: the core keeps the latest, which the window opens at
// next time. Filling its display unflagged counts as maximized, the
// normal box the one before.
static void sdlplaced(Os *os, GuiSdl *p)
{
    SDL_WindowFlags f = SDL_GetWindowFlags(p->window);
    if (f & (SDL_WINDOW_MINIMIZED|SDL_WINDOW_HIDDEN)) {
        return;
    }
    if (!(f & SDL_WINDOW_FULLSCREEN)) {
        WinBox b = {};
        SDL_GetWindowPosition(p->window, &b.x, &b.y);
        SDL_GetWindowSize(p->window, &b.w, &b.h);
        p->max = (f & SDL_WINDOW_MAXIMIZED) || filling(os, b, titleheight(p->window));
        if (!p->max) {
            p->normal = b;
        }
    }
    WinGeom geom = {p->normal, p->max, 1, (f & SDL_WINDOW_FULLSCREEN)!=0};
    gui_placed(p->gui, geom);
}

static WinGeom os_guiwhere(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    WinGeom r = {};
    if (p && p->window) {
        SDL_WindowFlags f = SDL_GetWindowFlags(p->window);
        SDL_GetWindowPosition(p->window, &r.box.x, &r.box.y);
        SDL_GetWindowSize(p->window, &r.box.w, &r.box.h);
        r.max  = (f & SDL_WINDOW_MAXIMIZED) != 0;
        r.ok   = 1;
        r.full = (f & SDL_WINDOW_FULLSCREEN) != 0;
    }
    return r;
}

static void os_guiplace(Os *os, WinGeom geom)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p || !p->window || !geom.ok) {
        return;
    }
    if (SDL_GetWindowFlags(p->window) & SDL_WINDOW_MAXIMIZED) {
        SDL_RestoreWindow(p->window);
    }
    sdlplace(os, p, geom);
    if (geom.max) {
        SDL_MaximizeWindow(p->window);
    }
}

#ifdef __APPLE__
// Whether the app shows in the Dock and the app switcher: not while
// only its menu bar icon is left (an accessory app).
static void dockicon(b32 show)
{
    using Get = void *(*)(void *, void *);
    using Set = void  (*)(void *, void *, i64);
    void *cls = objc_getClass("NSApplication");
    void *app = cls ? ((Get)(void *)objc_msgSend)(cls, sel_registerName("sharedApplication")) : 0;
    if (app) {
        ((Set)(void *)objc_msgSend)(app, sel_registerName("setActivationPolicy:"), show ? 0 : 1);
    }
}

// AppKit asks the app delegate to reopen when the Dock's icon is clicked
// or the app is launched again (Launch Services) while it runs, and SDL
// leaves it unanswered, which would leave a shed window shed. It goes
// as the tray's Open, through the queue: the type of SDL's event for
// that, for the method, which has nowhere else to find it.
static u32 reopenevent;

#if __arm64__
using ObjcBool = bool;
static char const reopentypes[] = "B@:@B";
#else
using ObjcBool = signed char;
static char const reopentypes[] = "c@:@c";
#endif

static ObjcBool shouldreopen(void *, void *, void *, ObjcBool)
{
    SDL_Event e = {};
    e.type      = reopenevent;
    e.user.code = TRAY_OPEN;
    SDL_PushEvent(&e);
    return 0;  // handled: AppKit does nothing more
}

extern "C" void *object_getClass(void *);
extern "C" bool  class_addMethod(void *, void *, void *, char const *);

static void handlereopen(u32 trayevent)
{
    using Get = void *(*)(void *, void *);
    Get   get = (Get)(void *)objc_msgSend;
    void *cls = objc_getClass("NSApplication");
    void *app = cls ? get(cls, sel_registerName("sharedApplication")) : 0;
    void *dlg = app ? get(app, sel_registerName("delegate")) : 0;
    if (dlg) {
        reopenevent = trayevent;
        class_addMethod(object_getClass(dlg),
                        sel_registerName("applicationShouldHandleReopen:hasVisibleWindows:"),
                        (void *)shouldreopen, reopentypes);
    }
}
#endif

static b32 os_guiopen(Os *os, Gui *g, Str title)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (p) {
        #ifdef __APPLE__
        dockicon(1);
        #endif
        p->framedark = -1;
        if (!sdlwindow(os, g, p, title)) {
            return 0;
        }
        SDL_DestroyWindow(p->waiter);
        p->waiter = 0;
        return 1;
    }
    p = alloc<GuiSdl>(&g->perm);
    p->gui = g;
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // sigthread's job
    // A click that activates the window also reaches what it lands on,
    // as on Windows, rather than being swallowed by the activation.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    // The window may go while a tray icon keeps the program running
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    // A trackpad's flick goes on scrolling after the fingers lift, as
    // elsewhere on macOS: SDL turns that off unless asked, as it reads
    // the hint once, in SDL_Init
    SDL_SetHint(SDL_HINT_MAC_SCROLL_MOMENTUM, "1");
    if (g->drive) {
        // Synthetic input must not compete with the real mouse, which
        // the backend reads directly while the window has focus.
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    #ifdef __APPLE__
    nopersist();
    #endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 0;
    }
    p->wakeevent = SDL_RegisterEvents(2);
    p->trayevent = p->wakeevent + 1;
    p->framedark = -1;
    #ifdef __APPLE__
    handlereopen(p->trayevent);
    #endif
    if (!sdlwindow(os, g, p, title)) {
        return 0;
    }
    os_thread(os, sigthread, 0);
    #ifdef __linux__
    portalwatch(os, &g->perm, p);
    #endif
    return 1;
}

// The window, its GL context, and ImGui's backends for them, which
// os_guished takes down again.
static b32 sdlwindow(Os *os, Gui *g, GuiSdl *p, Str title)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    // Where it was last, if anywhere, fitted to the displays once it
    // exists and its borders are known, before it shows
    WinGeom geom = g->geom;
    i32     w    = geom.ok ? geom.box.w : 1280;
    i32     h    = geom.ok ? geom.box.h : 800;
    flags |= geom.ok && geom.max ? SDL_WINDOW_MAXIMIZED : 0;
    Arena t = g->frame;
    p->window = SDL_CreateWindow(cstr0(&t, title), w, h, flags);
    if (!p->window) {
        return 0;
    }
    if (geom.ok) {
        sdlplace(os, p, geom);
    } else {
        // The system's place, the normal box should it be maximized
        // before it ever moves
        WinBox *b = &p->normal;
        SDL_GetWindowPosition(p->window, &b->x, &b->y);
        SDL_GetWindowSize(p->window, &b->w, &b->h);
    }
    p->max = geom.ok && geom.max;
    if (geom.ok && geom.full) {
        SDL_SetWindowFullscreen(p->window, 1);  // as it shows
    }
    p->gl = SDL_GL_CreateContext(p->window);
    if (!p->gl) {
        SDL_DestroyWindow(p->window);
        p->window = 0;
        return 0;
    }
    SDL_GL_MakeCurrent(p->window, p->gl);
    // The scheduler paces frames to the display. SDL's own vsync on macOS
    // waits in each swap for a display link tick, which never comes while
    // the display is off (a locked screen): the GUI would hang there, and
    // not even quit on SIGTERM.
    SDL_GL_SetSwapInterval(0);
    p->glviewport   = (GlViewport)SDL_GL_GetProcAddress("glViewport");
    p->glclearcolor = (GlClearColor)SDL_GL_GetProcAddress("glClearColor");
    p->glclear      = (GlClear)SDL_GL_GetProcAddress("glClear");
    p->glreadpixels = (GlReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    if (!p->glviewport || !p->glclearcolor || !p->glclear) {
        SDL_GL_DestroyContext(p->gl);
        SDL_DestroyWindow(p->window);
        p->window = 0;
        return 0;
    }
    #ifdef __APPLE__
    g->clickthrough = firstmouse(p->window);
    #else
    g->clickthrough = SDL_GetHintBoolean(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, 0);
    #endif

    ImGui_ImplSDL3_InitForOpenGL(p->window, p->gl);
    ImGui_ImplOpenGL3_Init("#version 150");
    gui_scale(g, contentscale(p->window));

    os->gui = p;  // before any thread can wake it
    gui_systheme(g, sysdark(p));  // the frame takes it before it shows
    SDL_AddEventWatch(sdlwatch, p);
    SDL_ShowWindow(p->window);
    return 1;
}

static i32 inputkind(SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:      return INPUT_MOVE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:   return INPUT_BUTTON;
    case SDL_EVENT_MOUSE_WHEEL:       return INPUT_WHEEL;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:            return INPUT_KEY;
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:      return INPUT_TEXT;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return INPUT_FOCUS;
    }
    return e->type>=SDL_EVENT_WINDOW_FIRST && e->type<=SDL_EVENT_WINDOW_LAST
         ? INPUT_WINDOW : INPUT_OTHER;
}

// Closing and quitting are requests the core may decline.
static void dispatch(Os *os, GuiSdl *p, SDL_Event *e)
{
    Gui *g = p->gui;
    if (e->type == p->wakeevent) {
        takewake(p);
        return;
    } else if (e->type == p->trayevent) {
        gui_tray(g, e->user.code);
        return;
    } else if (!p->window) {
        // Shed: what is left is quitting (a signal, a logout)
        if (e->type == SDL_EVENT_QUIT) {
            gui_close(g);
        }
        return;
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (g->drive) {
            guilog(g, e->type==SDL_EVENT_QUIT ? Str("drive: SDL quit event")
                                              : Str("drive: SDL close event"));
        }
        gui_close(g);
        return;
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        gui_systheme(g, sysdark(p));
        return;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) {
            // No window: opened in the application (macOS's open events)
            gui_drop(g, {(u8 *)e->drop.data, (iz)SDL_strlen(e->drop.data)},
                     !e->drop.windowID);
        }
        return;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        gui_minimized(g, 0);
        gui_force(g);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        gui_scale(g, contentscale(p->window));
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN:
        gui_minimized(g, 1);
        break;
    case SDL_EVENT_WINDOW_OCCLUDED:
        // Nothing visible to draw, though the checklist draws anyway: it
        // runs behind other windows so as not to take the user's focus.
        if (!g->drive) {
            gui_minimized(g, 1);
        }
        break;
    }
    if (e->type==SDL_EVENT_WINDOW_MOVED || e->type==SDL_EVENT_WINDOW_RESIZED ||
            e->type==SDL_EVENT_WINDOW_MAXIMIZED || e->type==SDL_EVENT_WINDOW_RESTORED ||
            e->type==SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
            e->type==SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
        sdlplaced(os, p);
    }
    if (ImGui_ImplSDL3_ProcessEvent(e)) {
        gui_input(g, inputkind(e));
    } else {
        g->woke |= WOKE_OTHER;
    }
}

static b32 os_guiwait(Os *os, i64 us)
{
    GuiSdl   *p = (GuiSdl *)os->gui;
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, timeoutms(us))) {
        return 1;  // timed out
    }
    do {
        dispatch(os, p, &e);
    } while (SDL_PollEvent(&e));
    return 1;
}

static void os_guinewframe(Os *)
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

static void os_guipresent(Os *os, b32 present, u32 clear)
{
    GuiSdl     *p  = (GuiSdl *)os->gui;
    ImDrawData *dd = ImGui::GetDrawData();
    if (present) {
        i32 w = 0, h = 0;
        SDL_GetWindowSizeInPixels(p->window, &w, &h);
        p->glviewport(0, 0, w, h);
        p->glclearcolor((f32)(clear&255)/255, (f32)(clear>>8&255)/255,
                        (f32)(clear>>16&255)/255, 1);
        p->glclear(0x4000);  // GL_COLOR_BUFFER_BIT
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        Gui *g = p->gui;
        if (g->shotpath.len && p->glreadpixels) {
            // The checklist's screenshot, bottom row first from GL
            Arena a   = g->frame;
            u8   *px  = allocbytes(&a, (iz)w*h*4);
            u8   *top = allocbytes(&a, (iz)w*h*4);
            p->glreadpixels(0, 0, w, h, 0x1908, 0x1401, px);  // RGBA, UNSIGNED_BYTE
            for (i32 y = 0; y < h; y++) {
                copybytes(top + (iz)y*w*4, px + (iz)(h-1-y)*w*4, (iz)w*4);
            }
            Str png = pngencode(&a, top, w, h);
            if (!os_write(g->os, a, g->shotpath, png)) {
                guilog(g, "could not write a screenshot");
            }
            g->shotpath = {};
        }
        SDL_GL_SwapWindow(p->window);
    } else if (dd->Textures) {
        // Glyphs may have been added to the atlas even though the
        // picture is unchanged, and must be uploaded all the same.
        for (i32 i = 0; i < dd->Textures->Size; i++) {
            ImTextureData *tex = (*dd->Textures)[i];
            if (tex->Status != ImTextureStatus_OK) {
                ImGui_ImplOpenGL3_UpdateTexture(tex);
            }
        }
    }
}

static void os_guiraise(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p || !p->window) {
        return;
    }
    SDL_WindowFlags f = SDL_GetWindowFlags(p->window);
    if (f & SDL_WINDOW_HIDDEN) {
        SDL_ShowWindow(p->window);
    }
    if (f & SDL_WINDOW_MINIMIZED) {
        SDL_RestoreWindow(p->window);
    }
    if (!p->gui->drive) {
        SDL_RaiseWindow(p->window);
    }
}

// SDL's Wayland backend activates a window it shows with the token in
// the environment, then removes it from there. Elsewhere it would only
// linger, for the engine's commands to inherit.
static void os_guitoken(Os *os, Str token)
{
    if (os->gui && wayland() && token.len) {
        Arena a = ((GuiSdl *)os->gui)->gui->frame;
        SDL_setenv_unsafe("XDG_ACTIVATION_TOKEN", cstr0(&a, token), 1);
    }
}

static void os_guiletfront(Os *, i64) {}

static i64 os_guirefresh(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    SDL_DisplayMode const *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(p->window));
    f32 hz = m && m->refresh_rate>1 ? m->refresh_rate : 60;
    return (i64)(1e6f / hz);
}

static void os_guiinject(Os *os, GuiEvent *ge)
{
    GuiSdl   *p  = (GuiSdl *)os->gui;
    SDL_Event e  = {};
    if (ge->kind == GEV_TRAY) {
        // As a pick from its menu sends it, tray or not
        e.type      = p->trayevent;
        e.user.code = (i32)ge->x;
        SDL_PushEvent(&e);
        return;
    } else if (!p->window) {
        return;
    }
    u32 id = SDL_GetWindowID(p->window);
    switch (ge->kind) {
    case GEV_MOVE:
        e.type            = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = id;
        e.motion.which    = 1;
        e.motion.x        = ge->x;
        e.motion.y        = ge->y;
        break;
    case GEV_BUTTON:
        e.type            = ge->down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = id;
        e.button.which    = 1;
        e.button.button   = ge->button ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        e.button.down     = ge->down;
        e.button.clicks   = 1;
        e.button.x        = ge->x;
        e.button.y        = ge->y;
        break;
    case GEV_WHEEL:
        e.type           = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = id;
        e.wheel.which    = 1;
        e.wheel.y        = ge->y;
        e.wheel.mouse_x  = ge->x;
        break;
    case GEV_KEY: {
        static struct { SDL_Keycode key; SDL_Scancode scan; } keys[] = {
            {SDLK_TAB,    SDL_SCANCODE_TAB},
            {SDLK_LCTRL,  SDL_SCANCODE_LCTRL},
            {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
            {SDLK_RETURN, SDL_SCANCODE_RETURN},
            {SDLK_C,      SDL_SCANCODE_C},
            {SDLK_LSHIFT, SDL_SCANCODE_LSHIFT},
            {SDLK_A,      SDL_SCANCODE_A},
            {SDLK_END,    SDL_SCANCODE_END},
            {SDLK_LGUI,   SDL_SCANCODE_LGUI},
            {SDLK_EQUALS, SDL_SCANCODE_EQUALS},
            {SDLK_MINUS,  SDL_SCANCODE_MINUS},
            {SDLK_0,      SDL_SCANCODE_0},
            {SDLK_F,      SDL_SCANCODE_F},
            {SDLK_G,      SDL_SCANCODE_G},
            {SDLK_T,      SDL_SCANCODE_T},
            {SDLK_R,      SDL_SCANCODE_R},
            {SDLK_UP,     SDL_SCANCODE_UP},
            {SDLK_DOWN,   SDL_SCANCODE_DOWN},
            {SDLK_V,      SDL_SCANCODE_V},
        };
        e.type         = ge->down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = id;
        e.key.key      = keys[ge->key].key;
        e.key.scancode = keys[ge->key].scan;
        e.key.mod      = (SDL_Keymod)((ge->ctrl ? SDL_KMOD_LCTRL : 0) |
                                      (ge->shift ? SDL_KMOD_LSHIFT : 0) |
                                      (ge->cmd ? SDL_KMOD_LGUI : 0));
        e.key.down     = ge->down;
    } break;
    case GEV_TEXT:
        e.type          = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = id;
        e.text.text     = ge->text.cdata;  // a literal: outlives dispatch
        break;
    case GEV_RESIZE:
        SDL_SetWindowSize(p->window, (i32)ge->x, (i32)ge->y);
        return;
    case GEV_MODALSIZE:
        // What SDL sends from macOS's live-resize loop, 60 times a second
        // while the user drags: the watch draws from these.
        for (i32 i = 0; i < (i32)ge->x; i++) {
            SDL_Event x = {};
            x.type            = SDL_EVENT_WINDOW_EXPOSED;
            x.window.windowID = id;
            x.window.data1    = 1;
            SDL_PushEvent(&x);
        }
        return;
    case GEV_DPI:
        return;  // a new scale needs another monitor
    case GEV_CLOSE:
        e.type            = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = id;
        break;
    case GEV_MINIMIZE:
        SDL_MinimizeWindow(p->window);
        return;
    case GEV_SYSTHEME:
        // As SDL reports a change, which the pretense then answers
        p->drivetheme = ge->x<0 ? 0 : ge->x>0 ? 2 : 1;
        e.type = SDL_EVENT_SYSTEM_THEME_CHANGED;
        break;
    case GEV_DROP:
        e.type          = SDL_EVENT_DROP_FILE;
        e.drop.windowID = id;
        e.drop.x        = ge->x;
        e.drop.y        = ge->y;
        e.drop.data     = ge->text.cdata;  // outlives dispatch
        break;
    }
    e.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&e);
}

static b32 os_guitheme(Os *os, b32 dark)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p || !p->window) {
        return 0;
    }
    #ifdef __APPLE__
    if (p->framedark == dark) {
        return 1;
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(p->window);
    void *win = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, 0);
    void *cls = objc_getClass("NSAppearance");
    if (!win || !cls) {
        return 0;
    }
    using Send = void *(*)(void *, void *, void *);
    Send  send = (Send)(void *)objc_msgSend;
    void *name = dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua;
    void *look = send(cls, sel_registerName("appearanceNamed:"), name);
    if (!look) {
        return 0;
    }
    send(win, sel_registerName("setAppearance:"), look);
    p->framedark = dark;
    return 1;
    #else
    return 0;  // the window manager's
    #endif
}

static void os_guished(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p || !p->window) {
        return;
    }
    sdlplaced(os, p);  // where it opens again
    SDL_RemoveEventWatch(sdlwatch, p);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    SDL_GL_DestroyContext(p->gl);
    SDL_DestroyWindow(p->window);
    p->window = 0;
    p->gl     = 0;
    p->waiter = SDL_CreateWindow("", 1, 1, SDL_WINDOW_HIDDEN);
    #ifdef __APPLE__
    if (p->tray) {
        dockicon(0);  // the menu bar icon is the way back
    }
    #endif
}

// A pick from the tray icon's menu, through the queue: it may come from
// inside the system's menu loop.
static void SDLCALL traypick(void *ud, SDL_TrayEntry *)
{
    TrayItem *t = (TrayItem *)ud;
    SDL_Event e = {};
    e.type      = t->p->trayevent;
    e.user.code = t->item;
    SDL_PushEvent(&e);
}

// A blue round icon of n by n pixels, for the tray: an SDL surface of
// its own, which the tray copies.
static SDL_Surface *trayicon(i32 n)
{
    SDL_Surface *s = SDL_CreateSurface(n, n, SDL_PIXELFORMAT_RGBA32);
    if (!s) {
        return 0;
    }
    f32 c = (f32)n/2;
    for (i32 y = 0; y < n; y++) {
        u8 *row = (u8 *)s->pixels + (iz)y*s->pitch;
        for (i32 x = 0; x < n; x++) {
            f32 dx = (f32)x + 0.5f - c, dy = (f32)y + 0.5f - c;
            f32 d  = c - 1 - __builtin_sqrtf(dx*dx + dy*dy);  // inside if > 0
            f32 a  = d>1 ? 1 : d<0 ? 0 : d;
            row[4*x+0] = 0x2f;
            row[4*x+1] = 0x6f;
            row[4*x+2] = 0xd0;
            row[4*x+3] = (u8)(a*255);
        }
    }
    return s;
}

static b32 os_trayopen(Os *os, Str tip)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p || p->tray) {
        return p && p->tray;
    }
    Arena        a    = p->gui->frame;
    SDL_Surface *icon = trayicon(32);
    p->tray = SDL_CreateTray(icon, cstr0(&a, tip));
    SDL_DestroySurface(icon);
    SDL_TrayMenu *menu = p->tray ? SDL_CreateTrayMenu(p->tray) : 0;
    if (!menu) {
        char const *why = SDL_GetError();
        guilog(p->gui, concat(&a, "no tray icon: ", {(u8 *)why, (iz)__builtin_strlen(why)}));
        if (p->tray) {
            SDL_DestroyTray(p->tray);
            p->tray = 0;
        }
        return 0;
    }
    static char const *labels[] = {"Open Prospero", "Stop all sessions", "Quit Prospero"};
    for (i32 i = 0; i < countof(labels); i++) {
        p->items[i].p    = p;
        p->items[i].item = i;
        SDL_TrayEntry *e = SDL_InsertTrayEntryAt(menu, -1, labels[i], SDL_TRAYENTRY_BUTTON);
        if (e) {
            SDL_SetTrayEntryCallback(e, traypick, &p->items[i]);
        }
    }
    return 1;
}

static void os_trayclose(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (p && p->tray) {
        SDL_DestroyTray(p->tray);
        p->tray = 0;
    }
}

// The save dialog's answer, from whichever thread SDL gives it on.
struct SdlSave {
    Mailbox *mb;
    u32      type;
    u64      id;
};

static void SDLCALL savedone(void *ud, char const * const *files, int)
{
    SdlSave *s    = (SdlSave *)ud;
    Str      path = {};
    if (files && files[0]) {
        path = {(u8 *)files[0], (iz)SDL_strlen(files[0])};
    }
    mailpost(s->mb, s->type, s->id, path, 1);  // empty: cancelled, or failed
    SDL_free(s);
}

// A sheet over the window on macOS, the desktop portal's dialog (or
// zenity's) on Linux: each answers later, the loop running meanwhile.
// It starts in the Documents folder, the file named.
static b32 os_guisave(Os *os, Arena scratch, Str title, Str name, Mailbox *mb,
                      u32 type, u64 id)
{
    GuiSdl  *p = (GuiSdl *)os->gui;
    SdlSave *s = (SdlSave *)SDL_malloc(sizeof(SdlSave));
    if (!p || !p->window || !s) {
        SDL_free(s);
        return 0;
    }
    s->mb   = mb;
    s->type = type;
    s->id   = id;
    char const *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    Str         dir  = docs ? Str{(u8 *)docs, (iz)SDL_strlen(docs)} : Str{};
    Str         at   = dir.len ? join(&scratch, dir, name) : name;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, p->window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, cstr(&scratch, title));
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, cstr(&scratch, at));
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_SAVEFILE, savedone, s, props);
    SDL_DestroyProperties(props);
    return 1;
}

// The same for a file to open, a raw log or an image, answered as
// savedone does.
static b32 os_guiopenfile(Os *os, Arena scratch, Str title, i32 kind, Mailbox *mb,
                          u32 type, u64 id)
{
    static SDL_DialogFileFilter const logs[] = {
        {"Session logs (*.jsonl)", "jsonl"},
        {"All files", "*"},
    };
    static SDL_DialogFileFilter const images[] = {
        {"Images (*.png, *.jpg, *.gif, *.webp, *.bmp)", "png;jpg;jpeg;gif;webp;bmp"},
        {"All files", "*"},
    };
    SDL_DialogFileFilter const *filters = kind==OPEN_IMAGE ? images : logs;
    GuiSdl  *p = (GuiSdl *)os->gui;
    SdlSave *s = (SdlSave *)SDL_malloc(sizeof(SdlSave));
    if (!p || !p->window || !s) {
        SDL_free(s);
        return 0;
    }
    s->mb   = mb;
    s->type = type;
    s->id   = id;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void *)filters);
    #ifdef __APPLE__
    // Cocoa's panel has no list of filters, and a "*" among them turns
    // filtering off: logs or images only
    i64 nfilters = 1;
    #else
    i64 nfilters = 2;
    #endif
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, p->window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, cstr(&scratch, title));
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, savedone, s, props);
    SDL_DestroyProperties(props);
    return 1;
}

// And for a directory, starting in at.
static b32 os_guiopendir(Os *os, Arena scratch, Str title, Str at, Mailbox *mb,
                         u32 type, u64 id)
{
    GuiSdl  *p = (GuiSdl *)os->gui;
    SdlSave *s = (SdlSave *)SDL_malloc(sizeof(SdlSave));
    if (!p || !p->window || !s) {
        SDL_free(s);
        return 0;
    }
    s->mb   = mb;
    s->type = type;
    s->id   = id;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, p->window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, cstr(&scratch, title));
    if (at.len) {
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, cstr(&scratch, at));
    }
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, savedone, s, props);
    SDL_DestroyProperties(props);
    return 1;
}

static i32 urlhex(u8 c)
{
    return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 :
           c>='A' && c<='F' ? c-'A'+10 : -1;
}

// A file URL's path, its %XX escapes decoded, else null.
static Str fileurlpath(Arena *a, Str url)
{
    Str r = {};
    if (!startswith(url, "file://")) {
        return r;
    }
    url = cuthead(url, 7);
    iz i = 0;
    for (; i<url.len && url[i]!='/'; i++) {}  // a host, usually none
    url = cuthead(url, i);
    r.data = alloc<u8>(a, url.len);
    for (iz k = 0; k < url.len; k++) {
        u8  c = url[k];
        i32 hi = k+2<url.len ? urlhex(url[k+1]) : -1;
        i32 lo = k+2<url.len ? urlhex(url[k+2]) : -1;
        if (c=='%' && hi>=0 && lo>=0) {
            c  = (u8)(hi<<4 | lo);
            k += 2;
        }
        r.data[r.len++] = c;
    }
    return r;
}

// The clipboard's image, as PNG (or JPEG, GIF, WebP or BMP) from SDL's
// MIME types, else
// the first file it holds, as Finder (public.file-url) or a Linux file
// manager (text/uri-list) puts it there.
static ClipImage os_guiclipimage(Os *, Arena *a)
{
    ClipImage r = {};
    static char const *mimes[] = {
        "image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp",
    };
    for (i32 i = 0; i < countof(mimes); i++) {
        if (!SDL_HasClipboardData(mimes[i])) {
            continue;
        }
        size_t len  = 0;
        void  *data = SDL_GetClipboardData(mimes[i], &len);
        if (data && len>0 && len<=(size_t)ATTACH_UPLOAD) {
            r.data = clone(a, Str{(u8 *)data, (iz)len});
        }
        SDL_free(data);
        if (r.data.len) {
            return r;
        }
    }
    #ifdef __APPLE__
    char const *urls = "public.file-url";
    #else
    char const *urls = "text/uri-list";
    #endif
    if (SDL_HasClipboardData(urls)) {
        size_t len  = 0;
        void  *data = SDL_GetClipboardData(urls, &len);
        if (data && len>0 && len<4096) {
            Str list = {(u8 *)data, (iz)len};
            for (Str rest = list; rest.len && !r.path.data;) {
                Cut c = cut(rest, '\n');
                rest  = c.tail;
                Str l = trim(c.head);
                if (l.len && l[0]!='#') {
                    r.path = fileurlpath(a, l);
                }
            }
        }
        SDL_free(data);
    }
    return r;
}

// SDL asks for the PNG when something pastes it.
static void const *SDLCALL clipsend(void *ud, char const *mime, size_t *size)
{
    Str *png = (Str *)ud;
    if (mime && !SDL_strcmp(mime, "image/png")) {
        *size = (size_t)png->len;
        return png->data;
    }
    *size = 0;
    return 0;
}

static void SDLCALL clipfree(void *ud)
{
    Str *png = (Str *)ud;
    SDL_free(png);
}

static b32 os_guiclipput(Os *, Arena, Str png)
{
    // A copy that lives until the clipboard changes again
    Str *keep = (Str *)SDL_malloc(sizeof(Str) + (size_t)png.len);
    if (!keep) {
        return 0;
    }
    keep->data = (u8 *)(keep + 1);
    keep->len  = png.len;
    SDL_memcpy(keep->data, png.data, (size_t)png.len);
    static char const *mimes[] = {"image/png"};
    return SDL_SetClipboardData(clipsend, clipfree, keep, mimes, 1);
}

// An absolute path as a file URL, null-terminated.
static char *fileurl(Arena *a, Str path)
{
    Buf b(a, 16 + 3*path.len);
    print(&b, "file://");
    for (iz i = 0; i < path.len; i++) {
        u8 c = path[i];
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
                c=='/' || c=='-' || c=='_' || c=='.' || c=='~') {
            putbyte(&b, c);
        } else {
            putbyte(&b, '%');
            putbyte(&b, "0123456789ABCDEF"[c>>4]);
            putbyte(&b, "0123456789ABCDEF"[c&15]);
        }
    }
    return cstr(a, finish(&b));
}

// As a file URL, which the system opens with its file manager.
static b32 os_guireveal(Os *, Arena scratch, Str dir)
{
    return SDL_OpenURL(fileurl(&scratch, dir));
}

#if defined(__APPLE__)
extern "C" void *objc_autoreleasePoolPush();
extern "C" void  objc_autoreleasePoolPop(void *);

// NSWorkspace selects it in a Finder window, which it brings forward.
static b32 os_guishow(Os *, Arena scratch, Str file)
{
    using Get  = void *(*)(void *, void *);
    using Cstr = void *(*)(void *, void *, char const *);
    using Obj  = void *(*)(void *, void *, void *);
    using Show = void  (*)(void *, void *, void *);
    void *pool = objc_autoreleasePoolPush();
    void *path = ((Cstr)(void *)objc_msgSend)(objc_getClass("NSString"),
                                              sel_registerName("stringWithUTF8String:"),
                                              cstr(&scratch, file));
    void *url  = !path ? 0 : ((Obj)(void *)objc_msgSend)(objc_getClass("NSURL"),
                                                         sel_registerName("fileURLWithPath:"),
                                                         path);
    void *urls = !url ? 0 : ((Obj)(void *)objc_msgSend)(objc_getClass("NSArray"),
                                                        sel_registerName("arrayWithObject:"),
                                                        url);
    void *ws   = ((Get)(void *)objc_msgSend)(objc_getClass("NSWorkspace"),
                                             sel_registerName("sharedWorkspace"));
    if (urls && ws) {
        ((Show)(void *)objc_msgSend)(ws, sel_registerName("activateFileViewerSelectingURLs:"),
                                     urls);
    }
    objc_autoreleasePoolPop(pool);
    return urls && ws;
}

#elif defined(__linux__)
// libdbus, as much as ShowItems needs, beside the portal watch's
using DbusClose    = void  (*)(void *);
using DbusNewCall  = void *(*)(char const *, char const *, char const *, char const *);
using DbusIterApp  = void  (*)(void *, void *);
using DbusIterOpen = u32   (*)(void *, i32, char const *, void *);
using DbusIterEnd  = u32   (*)(void *, void *);
using DbusIterPut  = u32   (*)(void *, i32, void const *);
using DbusCallWait = void *(*)(void *, void *, i32, void *);

// A file to show, its URL and its directory's, which a thread shows,
// then frees: in one allocation of SDL's.
struct ShowFile {
    char *url;
    char *dir;
};

// The file manager's ShowItems ({file URL}, no startup ID) on the
// session bus, waiting for its answer, since one may be started for
// it; with no answer, its directory opened instead.
static void showthread(void *arg)
{
    ShowFile *sf = (ShowFile *)arg;
    b32       ok = 0;
    void     *h  = dlopen("libdbus-1.so.3", RTLD_NOW|RTLD_LOCAL);
    if (h) {
        DbusInit     init    = (DbusInit)dlsym(h, "dbus_threads_init_default");
        DbusGet      get     = (DbusGet)dlsym(h, "dbus_bus_get_private");
        DbusExit     noexit  = (DbusExit)dlsym(h, "dbus_connection_set_exit_on_disconnect");
        DbusClose    shut    = (DbusClose)dlsym(h, "dbus_connection_close");
        DbusUnref    connrel = (DbusUnref)dlsym(h, "dbus_connection_unref");
        DbusNewCall  newcall = (DbusNewCall)dlsym(h, "dbus_message_new_method_call");
        DbusIterApp  args    = (DbusIterApp)dlsym(h, "dbus_message_iter_init_append");
        DbusIterOpen nest    = (DbusIterOpen)dlsym(h, "dbus_message_iter_open_container");
        DbusIterEnd  unnest  = (DbusIterEnd)dlsym(h, "dbus_message_iter_close_container");
        DbusIterPut  put     = (DbusIterPut)dlsym(h, "dbus_message_iter_append_basic");
        DbusCallWait callw   = (DbusCallWait)dlsym(h, "dbus_connection_send_with_reply_and_block");
        DbusUnref    msgrel  = (DbusUnref)dlsym(h, "dbus_message_unref");
        b32 have = init && get && noexit && shut && connrel && newcall && args && nest &&
                   unnest && put && callw && msgrel;
        void *conn = have && init() ? get(0, 0) : 0;  // the session bus
        if (conn) {
            noexit(conn, 0);
            void *m = newcall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                              "org.freedesktop.FileManager1", "ShowItems");
            if (m) {
                DbusIter    it  = {};
                DbusIter    arr = {};
                char const *url = sf->url;
                char const *id  = "";
                args(m, &it);
                ok = nest(&it, 'a', "s", &arr) && put(&arr, 's', &url) &&
                     unnest(&it, &arr) && put(&it, 's', &id);
                void *r = ok ? callw(conn, m, 10000, 0) : 0;
                ok = r != 0;
                if (r) {
                    msgrel(r);
                }
                msgrel(m);
            }
            shut(conn);
            connrel(conn);
        }
    }
    if (!ok) {
        SDL_OpenURL(sf->dir);
    }
    SDL_free(sf);
}

static b32 os_guishow(Os *os, Arena scratch, Str file)
{
    iz i = file.len;
    for (; i>1 && file[i-1]!='/'; i--) {}
    Str   dir = takehead(file, i>1 ? i-1 : i);
    char *url = fileurl(&scratch, file);
    char *du  = fileurl(&scratch, dir);
    iz    ul  = (iz)__builtin_strlen(url) + 1;
    iz    dl  = (iz)__builtin_strlen(du) + 1;
    ShowFile *sf = (ShowFile *)SDL_malloc(sizeof(ShowFile) + (size_t)(ul + dl));
    if (!sf) {
        return 0;
    }
    sf->url = (char *)(sf + 1);
    sf->dir = sf->url + ul;
    SDL_memcpy(sf->url, url, (size_t)ul);
    SDL_memcpy(sf->dir, du, (size_t)dl);
    if (!os_thread(os, showthread, sf)) {
        SDL_free(sf);
        return os_guireveal(os, scratch, dir);
    }
    return 1;
}

#else
static b32 os_guishow(Os *os, Arena scratch, Str file)
{
    iz i = file.len;
    for (; i>1 && file[i-1]!='/'; i--) {}
    return os_guireveal(os, scratch, takehead(file, i>1 ? i-1 : i));
}
#endif

static void os_guiclose(Os *os)
{
    os_guished(os);
    os_trayclose(os);
    os->gui = 0;
    SDL_Quit();
}
