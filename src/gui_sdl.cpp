// GUI platform for macOS and Linux: SDL3 and OpenGL 3
// Included by main_posix.cpp after the core when HARNESS_GUI is set, so
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
// SDL reports the system's theme (macOS: the app's effective appearance;
// Linux: the desktop portal's color-scheme, unknown without one, taken
// as dark) and its changes as events. On macOS the window's frame takes
// the theme in effect through its NSWindow's appearance, set through
// the Objective-C runtime; elsewhere the window manager draws it.
//
// This is free and unencumbered software released into the public domain.

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
#endif

using GlViewport   = void (*)(i32, i32, i32, i32);
using GlClearColor = void (*)(f32, f32, f32, f32);
using GlClear      = void (*)(u32);
using GlReadPixels = void (*)(i32, i32, i32, i32, u32, u32, void *);

struct GuiSdl {
    Gui          *gui;
    SDL_Window   *window;
    SDL_GLContext gl;
    u32           wakeevent;
    i32           wakeflag;    // atomic: a wake event is queued
    b32           inwatch;     // drawing from the event watch
    i32           framedark;   // the frame's theme: -1 unset, 0 light, 1 dark
    i32           drivetheme;  // the checklist's system theme: 1 light,
                               // 2 dark, 0 the real one
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
    if (p->drivetheme) {
        return p->drivetheme == 2;
    }
    return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT;
}

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

static b32 os_guiopen(Os *os, Gui *g, Str title)
{
    GuiSdl *p = alloc<GuiSdl>(&g->perm);
    p->gui = g;
    SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // sigthread's job
    // A click that activates the window also reaches what it lands on,
    // as on Windows, rather than being swallowed by the activation.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (g->drive) {
        // Synthetic input must not compete with the real mouse, which
        // the backend reads directly while the window has focus.
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    Arena t = g->frame;
    p->window = SDL_CreateWindow(cstr0(&t, title), 1280, 800, flags);
    if (!p->window) {
        return 0;
    }
    p->gl = SDL_GL_CreateContext(p->window);
    if (!p->gl) {
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
        return 0;
    }
    p->wakeevent = SDL_RegisterEvents(1);
    p->framedark = -1;
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
    os_thread(os, sigthread, 0);
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
static void dispatch(GuiSdl *p, SDL_Event *e)
{
    Gui *g = p->gui;
    if (e->type == p->wakeevent) {
        takewake(p);
        return;
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        gui_close(g);
        return;
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        gui_systheme(g, sysdark(p));
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
        dispatch(p, &e);
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
    GuiSdl         *p = (GuiSdl *)os->gui;
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
    u32       id = SDL_GetWindowID(p->window);
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
        };
        e.type         = ge->down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = id;
        e.key.key      = keys[ge->key].key;
        e.key.scancode = keys[ge->key].scan;
        e.key.mod      = (SDL_Keymod)((ge->ctrl ? SDL_KMOD_LCTRL : 0) |
                                      (ge->shift ? SDL_KMOD_LSHIFT : 0));
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
    }
    e.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&e);
}

static b32 os_guitheme(Os *os, b32 dark)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    if (!p) {
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

static void os_guiclose(Os *os)
{
    GuiSdl *p = (GuiSdl *)os->gui;
    SDL_RemoveEventWatch(sdlwatch, p);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    SDL_GL_DestroyContext(p->gl);
    SDL_DestroyWindow(p->window);
    os->gui = 0;
    SDL_Quit();
}
