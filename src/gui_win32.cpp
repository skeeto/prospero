// GUI platform for Windows: Win32 and Direct3D 11
//
// This file has two halves. Compiled on its own with PROSPERO_GUIEDGE,
// it is the edge translation unit: windows.h, D3D11, ImGui's Win32 and
// DX11 backends, the window procedure, and the embedded manifest, all
// behind extern "C" functions. Included by main_win32.cpp, it is the
// other side: the GUI's os_* functions, calling those. So main_win32.cpp
// stays free of headers, and the headless build stays free of the CRT.
//
// The window procedure renders from inside Windows' modal loops: a
// resize renders at once from WM_SIZE, and a timer armed while a move,
// resize or menu loop runs keeps owed frames and deadlines going.
// Wakes arrive as a posted message, which those loops dispatch too, so
// streaming continues during a drag. They go to a hidden window of
// their own, which outlives the main window when it goes (w32gui_shed)
// as the program runs on in the background, and which takes the tray
// icon's notifications (Shell_NotifyIconW): a click opens the window
// again, and its menu stops all or quits.
//
// The system's theme is the registry's AppsUseLightTheme, read again
// when WM_SETTINGCHANGE names "ImmersiveColorSet". The title bar takes
// the theme in effect through DWM (DWMWA_USE_IMMERSIVE_DARK_MODE, 20,
// or 19 before Windows 10 20H1); Windows 10 repaints it only when the
// window's activation changes, so that is restated.
//
// The window opens where it was last, its frame fitted to the monitors'
// work areas as they are (place.cpp, in the core's half), maximized if
// it was, else at its first size (place.cpp, the core's half too).
// WM_MOVE and WM_SIZE tell the core its geometry: the frame, or
// when maximized, the frame it restores to (GetWindowPlacement). A
// precision touchpad's scrolling comes as WM_MOUSEWHEEL with deltas
// smaller than a notch, inertia included, which ImGui takes as they are.
//
// This is free and unencumbered software released into the public domain.

// Shared by both halves: the core's callbacks, called on the GUI thread.
struct W32Hooks {
    void  *gui;
    void (*input)(void *gui, int kind);   // an input message went to ImGui
    void (*wake)(void *gui);              // the wake flag was cleared
    void (*force)(void *gui);             // must present: resize, expose
    void (*tick)(void *gui);              // draw a frame if one is due
    void (*frame)(void *gui);             // draw a frame now
    void (*scale)(void *gui, float);      // the DPI scale changed
    void (*minimized)(void *gui, int);
    void (*modal)(void *gui, int);        // a modal loop began or ended
    void (*close)(void *gui);             // the user asks to close it
    void (*systheme)(void *gui, int dark);// the system's theme changed
    void (*tray)(void *gui, int item);    // a pick from the tray's menu
    void (*placed)(void *gui, int const *geom);  // moved, resized: GEOM_*
    void (*drop)(void *gui, unsigned short const *path);  // a file dropped
    int    drive;                         // scripted: never take focus
};

// Window geometry, in pixels of the virtual screen: the frame when not
// maximized, as an array of these
enum {
    GEOM_X, GEOM_Y, GEOM_W, GEOM_H,
    GEOM_MAX,    // maximized
    GEOM_OK,     // known: else centered on the primary display, W and H
                 // the client area's first size, maximized if MAX
    GEOM_COUNT,
};

// Input kinds, as app.cpp numbers them (INPUT_*)
enum {
    W32IN_MOVE, W32IN_BUTTON, W32IN_WHEEL, W32IN_KEY, W32IN_TEXT,
    W32IN_FOCUS, W32IN_WINDOW, W32IN_OTHER,
};

extern "C" void     *w32gui_open(W32Hooks *, unsigned short const *title,
                                  int const *geom);
extern "C" void      w32gui_show(void *);
extern "C" int       w32gui_wait(void *, long long us);
extern "C" void      w32gui_wake(void *);
extern "C" void      w32gui_newframe(void *);
extern "C" void      w32gui_present(void *, int present, unsigned clear);
extern "C" long long w32gui_refresh(void *);
extern "C" void      w32gui_raise(void *);
extern "C" void      w32gui_inject(void *, int kind, float x, float y,
                                   int button, int down, int key, int ctrl,
                                   int shift, unsigned short const *text);
extern "C" int       w32gui_systheme(void *);  // 1 if apps should be dark
extern "C" int       w32gui_theme(void *, int dark);  // the title bar's
extern "C" void      w32gui_close(void *);
extern "C" void      w32gui_shed(void *);      // the window goes, the loop stays
extern "C" int       w32gui_reopen(void *, unsigned short const *title,
                                    int const *geom);
extern "C" int       w32gui_displays(int *xywh, int max);  // work areas
extern "C" float     w32gui_scale0(void);  // the primary display's DPI scale
extern "C" int       w32gui_where(void *, int *geom);
extern "C" void      w32gui_place(void *, int const *geom);
extern "C" int       w32gui_trayopen(void *, unsigned short const *tip);
extern "C" void      w32gui_trayclose(void *);
// The save dialog, on a thread of its own, over the window, which calls
// done with the path chosen, empty if cancelled, and a copy of ctx (at
// most 32 bytes)
extern "C" int       w32gui_save(void *, unsigned short const *title,
                                 unsigned short const *name,
                                 void (*done)(void *ctx, unsigned short const *path),
                                 void const *ctx, int ctxlen);
// The open dialog likewise, for a session's raw log (*.jsonl), or for
// an image (images)
extern "C" int       w32gui_openfile(void *, unsigned short const *title, int images,
                                     void (*done)(void *ctx, unsigned short const *path),
                                     void const *ctx, int ctxlen);
// And for a directory, starting in at (empty for the dialog's own)
extern "C" int       w32gui_opendir(void *, unsigned short const *title,
                                    unsigned short const *at,
                                    void (*done)(void *ctx, unsigned short const *path),
                                    void const *ctx, int ctxlen);
// The clipboard's data in a format (W32CLIP_*), copied to buf if it has
// room: its size in bytes, or -1 if it holds none. A file is its path,
// the first of those copied, null-terminated UTF-16.
enum { W32CLIP_PNG, W32CLIP_DIBV5, W32CLIP_DIB, W32CLIP_FILE };
extern "C" long long w32gui_clip(void *, int format, void *buf, long long cap);
// Put a PNG on the clipboard, as the "PNG" format. Returns 0 on failure.
extern "C" int       w32gui_clipput(void *, void const *png, long long len);
extern "C" int       w32gui_reveal(unsigned short const *dir);
// Show a file selected in Explorer: a path with backslashes
extern "C" int       w32gui_select(unsigned short const *file);
extern "C" void      w32gui_letfront(unsigned long pid);  // AllowSetForegroundWindow


#if PROSPERO_GUIEDGE

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>  // DROPFILES
#include <shobjidl.h>  // IFileOpenDialog
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// Resources: the manifest (per-monitor DPI awareness v2, common
// controls v6, the Windows versions it targets), from assets/ by the
// assembler's include path (-Wa,-I, set by CMake). The pattern is
// platform/gui_windows.c in skeeto/tugz, https://github.com/skeeto/tugz.
__asm__(
    "    .section .rsrc,\"dr\"\n"
    "    .p2align 2\n"
    ".Lroot: .long 0, 0\n"
    "    .short 0, 0, 0, 1\n"
    "    .long 24, 0x80000000 + (.Lmans - .Lroot)\n"    // RT_MANIFEST
    ".Lmans: .long 0, 0\n"
    "    .short 0, 0, 0, 1\n"
    "    .long 1, 0x80000000 + (.Lmanid - .Lroot)\n"
    ".Lmanid: .long 0, 0\n"
    "    .short 0, 0, 0, 1\n"
    "    .long 0x409, .Lmandata - .Lroot\n"
    ".Lmandata:\n"
    "    .rva .Lman\n"
    "    .long .Lmanend - .Lman, 0, 0\n"
    "    .p2align 3\n"
    ".Lman: .incbin \"assets/prospero.manifest\"\n"
    ".Lmanend:\n"
    "    .text\n"
);

enum {
    WM_APP_WAKE = WM_APP + 1,
    WM_APP_TRAY,              // the tray icon's notifications
    WM_APP_FRONT,             // the save dialog (wp) to bring forward
    TIMER_MODAL = 1,
    TRAY_ID     = 1,
    TRAY_CMD    = 100,        // + the TRAY_* item, as a menu command
};

struct W32Gui {
    W32Hooks                hooks;
    HWND                    hwnd;        // null while shed
    HWND                    msgwin;      // hidden, for wakes and the tray
    NOTIFYICONDATAW         nid;
    int                     trayon;
    UINT                    taskbarmsg;  // Explorer started (again): add the icon
    HANDLE                  timer;       // high-resolution waitable timer
    ID3D11Device           *device;
    ID3D11DeviceContext    *context;
    IDXGISwapChain         *swapchain;
    ID3D11RenderTargetView *rtv;
    LONG                    wakeflag;    // a wake message is queued
    int                     occluded;
    int                     inmodal;     // inside a move, size or menu loop
    int                     width, height;
    int                     framedark;   // the title bar: -1 unset, 0, 1
    int                     showmax;     // show it maximized
    int                     drivetheme;  // the checklist's system theme:
                                         // 1 light, 2 dark, 0 the real one
};

// Whether apps should be dark, as Settings > Personalization > Colors
// says, or as the checklist pretends. Light where unset, as before
// Windows 10 1809.
static int systemdark(W32Gui *w)
{
    if (w->drivetheme) {
        return w->drivetheme == 2;
    }
    DWORD v = 1, n = sizeof(v);
    LSTATUS r = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, 0, &v, &n
    );
    return r==ERROR_SUCCESS && v==0;
}

static void makertv(W32Gui *w)
{
    ID3D11Texture2D *back = 0;
    w->swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        w->device->CreateRenderTargetView(back, 0, &w->rtv);
        back->Release();
    }
}

static void resize(W32Gui *w, int width, int height)
{
    if (!w->swapchain || width<=0 || height<=0) {
        return;
    }
    if (width==w->width && height==w->height) {
        return;
    }
    if (w->rtv) {
        w->rtv->Release();
        w->rtv = 0;
    }
    w->swapchain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    makertv(w);
    w->width  = width;
    w->height = height;
}

static int inputkind(UINT msg)
{
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
        return W32IN_MOVE;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        return W32IN_BUTTON;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return W32IN_WHEEL;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        return W32IN_KEY;
    case WM_CHAR:
        return W32IN_TEXT;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_MOUSELEAVE:
    case WM_NCMOUSELEAVE:
        return W32IN_FOCUS;
    case WM_INPUTLANGCHANGE: case WM_DEVICECHANGE: case WM_SETTINGCHANGE:
        return W32IN_OTHER;
    }
    return -1;
}

// The window's geometry (GEOM_*), unless minimized: its frame, or when
// maximized, the frame it is restored to, whose workspace coordinates
// are offset by the primary display's task bar, if at its top or left.
static int geomnow(W32Gui *w, int *geom)
{
    if (!w->hwnd || IsIconic(w->hwnd) || !IsWindowVisible(w->hwnd)) {
        return 0;
    }
    int  max = IsZoomed(w->hwnd) != 0;
    RECT r;
    if (max) {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(w->hwnd, &wp);
        r = wp.rcNormalPosition;
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    } else {
        GetWindowRect(w->hwnd, &r);
    }
    geom[GEOM_X]   = r.left;
    geom[GEOM_Y]   = r.top;
    geom[GEOM_W]   = r.right - r.left;
    geom[GEOM_H]   = r.bottom - r.top;
    geom[GEOM_MAX] = max;
    geom[GEOM_OK]  = 1;
    return 1;
}

static void placed(W32Gui *w)
{
    int geom[GEOM_COUNT];
    if (geomnow(w, geom)) {
        w->hooks.placed(w->hooks.gui, geom);
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    W32Gui *w = (W32Gui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    W32Hooks *h = &w->hooks;

    // Scripted input must not be undone by the real pointer, which is
    // somewhere else entirely.
    if (h->drive && (msg==WM_MOUSELEAVE || msg==WM_NCMOUSELEAVE)) {
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        return 1;
    }
    int kind = inputkind(msg);
    if (kind >= 0) {
        h->input(h->gui, kind);
    }

    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            h->minimized(h->gui, 1);
            return 0;
        }
        resize(w, LOWORD(lp), HIWORD(lp));
        h->minimized(h->gui, 0);
        h->force(h->gui);
        h->frame(h->gui);  // at once, even inside a modal resize
        placed(w);
        return 0;

    case WM_MOVE:
        placed(w);
        return 0;

    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        w->inmodal = 1;
        h->modal(h->gui, 1);
        SetTimer(hwnd, TIMER_MODAL, 10, 0);
        return 0;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        w->inmodal = 0;
        h->modal(h->gui, 0);
        KillTimer(hwnd, TIMER_MODAL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_MODAL) {
            h->tick(h->gui);
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        w->occluded = 0;
        h->minimized(h->gui, 0);
        h->force(h->gui);
        if (w->inmodal) {
            h->frame(h->gui);
        }
    } return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && w->occluded) {
            w->occluded = 0;
            h->minimized(h->gui, 0);
            h->force(h->gui);
        }
        break;

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right-r->left,
                     r->bottom-r->top, SWP_NOZORDER|SWP_NOACTIVATE);
        h->scale(h->gui, (float)HIWORD(wp) / 96.0f);
        h->force(h->gui);
    } return 0;

    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
            h->systheme(h->gui, systemdark(w));
        }
        break;

    case WM_SYSKEYDOWN:
        // No menu loop for F10 or Alt, but Alt+F4 still closes.
        if (wp != VK_F4) {
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;

    case WM_CLOSE:
        // A request the core may decline: it closes the window itself
        h->close(h->gui);
        return 0;

    case WM_DROPFILES: {
        HDROP    d = (HDROP)wp;
        UINT     n = DragQueryFileW(d, 0xffffffff, 0, 0);
        wchar_t *p = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, 32768*sizeof(wchar_t));
        for (UINT i = 0; p && i < n; i++) {
            if (DragQueryFileW(d, i, p, 32768)) {
                h->drop(h->gui, (unsigned short const *)p);
            }
        }
        if (p) HeapFree(GetProcessHeap(), 0, p);
        DragFinish(d);
    } return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The hidden window's: wakes, which come here whether or not the main
// window exists, and the tray icon's clicks and menu.
static LRESULT CALLBACK msgproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    W32Gui *w = (W32Gui *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    W32Hooks *h = &w->hooks;
    if (msg && msg==w->taskbarmsg) {
        // Explorer started over, and lost every icon: this one again
        if (w->trayon) {
            Shell_NotifyIconW(NIM_ADD, &w->nid);
        }
        return 0;
    }
    switch (msg) {
    case WM_APP_WAKE:
        // Cleared before draining: a post during the drain wakes again.
        InterlockedExchange(&w->wakeflag, 0);
        h->wake(h->gui);
        return 0;
    case WM_APP_FRONT: {
        // From this thread, which has the foreground while the user
        // works in the window, where the dialog's own thread may not
        HWND d = (HWND)wp;
        if (IsWindow(d) && IsWindowVisible(d) && GetForegroundWindow()!=d) {
            SetForegroundWindow(d);
        }
    } return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            h->tray(h->gui, 0);  // TRAY_OPEN
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, TRAY_CMD+0, L"Open Prospero");
            AppendMenuW(m, MF_STRING, TRAY_CMD+1, L"Stop all sessions");
            AppendMenuW(m, MF_SEPARATOR, 0, 0);
            AppendMenuW(m, MF_STRING, TRAY_CMD+2, L"Quit Prospero");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);  // so that the menu goes away
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, 0);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(m);
        } return 0;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp)>=TRAY_CMD && LOWORD(wp)<TRAY_CMD+3) {
            h->tray(h->gui, LOWORD(wp) - TRAY_CMD);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int makedevice(W32Gui *w, D3D_DRIVER_TYPE type)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = w->hwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT r = D3D11CreateDeviceAndSwapChain(
        0, type, 0, 0, levels, 2, D3D11_SDK_VERSION, &sd, &w->swapchain,
        &w->device, &got, &w->context
    );
    if (r == DXGI_ERROR_INVALID_CALL) {
        // Before Windows 10: no flip discard, so the classic model.
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        r = D3D11CreateDeviceAndSwapChain(
            0, type, 0, 0, levels, 2, D3D11_SDK_VERSION, &sd, &w->swapchain,
            &w->device, &got, &w->context
        );
    }
    return r == S_OK;
}

static int makewindow(W32Gui *w, unsigned short const *title, int const *geom);

extern "C" void *w32gui_open(W32Hooks *hooks, unsigned short const *title,
                             int const *geom)
{
    W32Gui *w = (W32Gui *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(W32Gui));
    if (!w) {
        return 0;
    }
    w->hooks      = *hooks;
    w->framedark  = -1;
    w->taskbarmsg = RegisterWindowMessageW(L"TaskbarCreated");
    // A high-resolution timer waits precisely without raising the
    // system timer rate (Windows 10 1803+); otherwise a plain one.
    w->timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!w->timer) {
        w->timer = CreateWaitableTimerW(0, 0, 0);
    }

    HINSTANCE   inst = GetModuleHandleW(0);
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"prospero";
    RegisterClassExW(&wc);

    // A top-level window never shown, rather than a message-only one,
    // since a tray icon's menu needs a window that can be in front
    WNDCLASSEXW mc   = {};
    mc.cbSize        = sizeof(mc);
    mc.lpfnWndProc   = msgproc;
    mc.hInstance     = inst;
    mc.lpszClassName = L"prospero-msg";
    RegisterClassExW(&mc);
    w->msgwin = CreateWindowExW(0, L"prospero-msg", L"Prospero", WS_OVERLAPPED,
                                0, 0, 0, 0, 0, 0, inst, 0);
    if (!w->msgwin) {
        return 0;
    }
    SetWindowLongPtrW(w->msgwin, GWLP_USERDATA, (LONG_PTR)w);
    return makewindow(w, title, geom) ? w : 0;
}

// The main window, its device, and ImGui's backends for them, which
// w32gui_shed takes down again. It goes where geom says (fitted to the
// displays already), else centered on the primary display, its client
// area geom's first size (the core's, at that display's scale).
static int makewindow(W32Gui *w, unsigned short const *title, int const *geom)
{
    W32Hooks *hooks = &w->hooks;
    HINSTANCE inst  = GetModuleHandleW(0);
    int       at    = geom[GEOM_OK];
    w->framedark = -1;
    w->width     = 0;
    w->height    = 0;
    w->showmax   = geom[GEOM_MAX];
    // Maximized from creation, for a show that does not activate (the
    // checklist's), as no show command maximizes without activating.
    // Its normal box is still the one given.
    DWORD style = WS_OVERLAPPEDWINDOW | (w->showmax ? WS_MAXIMIZE : 0);
    // A first window centered on the primary display's work area, as
    // the system's cascade would leave a big one hanging off it
    RECT  r     = {0, 0, geom[GEOM_W], geom[GEOM_H]};
    int   x     = geom[GEOM_X];
    int   y     = geom[GEOM_Y];
    if (!at) {
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, 0, 0);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        RECT wa = mi.rcWork;
        x = wa.left + ((wa.right-wa.left) - (r.right-r.left))/2;
        y = wa.top  + ((wa.bottom-wa.top) - (r.bottom-r.top))/2;
        x = x<wa.left ? wa.left : x;
        y = y<wa.top  ? wa.top  : y;
    }
    w->hwnd = CreateWindowExW(
        0, L"prospero", (LPCWSTR)title, style, x, y,
        r.right-r.left, r.bottom-r.top, 0, 0, inst, 0
    );
    if (!w->hwnd) {
        return 0;
    }
    DragAcceptFiles(w->hwnd, TRUE);
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(w->hwnd);

    // Hardware, else WARP: a VM or a remote session may have no GPU.
    if (!makedevice(w, D3D_DRIVER_TYPE_HARDWARE) &&
            !makedevice(w, D3D_DRIVER_TYPE_WARP)) {
        DestroyWindow(w->hwnd);
        w->hwnd = 0;
        return 0;
    }
    IDXGIFactory *factory = 0;
    if (SUCCEEDED(w->swapchain->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(w->hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    makertv(w);
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->width  = cr.right;
    w->height = cr.bottom;

    ImGui_ImplWin32_Init(w->hwnd);
    ImGui_ImplDX11_Init(w->device, w->context);
    hooks->scale(hooks->gui, scale);

    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    return 1;
}

// The main window goes, with its device and backends, and without the
// quit message its destruction would post: wakes and the tray go on
// through the hidden window.
extern "C" void w32gui_shed(void *p)
{
    W32Gui *w = (W32Gui *)p;
    if (!w->hwnd) {
        return;
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (w->rtv)       w->rtv->Release();
    if (w->swapchain) w->swapchain->Release();
    if (w->context)   w->context->Release();
    if (w->device)    w->device->Release();
    w->rtv       = 0;
    w->swapchain = 0;
    w->context   = 0;
    w->device    = 0;
    DestroyWindow(w->hwnd);
    w->hwnd     = 0;
    w->occluded = 0;
    w->inmodal  = 0;
}

extern "C" int w32gui_reopen(void *p, unsigned short const *title, int const *geom)
{
    W32Gui *w = (W32Gui *)p;
    return w->hwnd || makewindow(w, title, geom);
}

struct Displays {
    int *xywh;
    int  max;
    int  n;
};

static BOOL CALLBACK adddisplay(HMONITOR m, HDC, LPRECT, LPARAM arg)
{
    Displays   *d  = (Displays *)arg;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (d->n<d->max && GetMonitorInfoW(m, &mi)) {
        int i = d->n++;
        if (mi.dwFlags & MONITORINFOF_PRIMARY) {
            for (; i > 0; i--) {  // the primary first
                for (int k = 0; k < 4; k++) {
                    d->xywh[4*i+k] = d->xywh[4*(i-1)+k];
                }
            }
        }
        RECT r = mi.rcWork;
        d->xywh[4*i+0] = r.left;
        d->xywh[4*i+1] = r.top;
        d->xywh[4*i+2] = r.right - r.left;
        d->xywh[4*i+3] = r.bottom - r.top;
    }
    return TRUE;
}

extern "C" int w32gui_displays(int *xywh, int max)
{
    Displays d = {xywh, max, 0};
    EnumDisplayMonitors(0, 0, adddisplay, (LPARAM)&d);
    return d.n;
}

extern "C" float w32gui_scale0(void)
{
    return ImGui_ImplWin32_GetDpiScaleForMonitor(
        MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)
    );
}

extern "C" int w32gui_where(void *p, int *geom)
{
    return geomnow((W32Gui *)p, geom);
}

extern "C" void w32gui_place(void *p, int const *geom)
{
    W32Gui *w = (W32Gui *)p;
    if (!w->hwnd || !geom[GEOM_OK]) {
        return;
    }
    if (IsZoomed(w->hwnd)) {
        ShowWindow(w->hwnd, SW_RESTORE);
    }
    SetWindowPos(w->hwnd, 0, geom[GEOM_X], geom[GEOM_Y], geom[GEOM_W],
                 geom[GEOM_H], SWP_NOZORDER|SWP_NOACTIVATE);
    if (geom[GEOM_MAX]) {
        ShowWindow(w->hwnd, SW_MAXIMIZE);
    }
}

extern "C" int w32gui_trayopen(void *p, unsigned short const *tip)
{
    W32Gui *w = (W32Gui *)p;
    if (w->trayon) {
        return 1;
    }
    NOTIFYICONDATAW *n = &w->nid;
    *n = {};
    n->cbSize           = sizeof(*n);
    n->hWnd             = w->msgwin;
    n->uID              = TRAY_ID;
    n->uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n->uCallbackMessage = WM_APP_TRAY;
    n->hIcon            = LoadIconW(0, (LPCWSTR)IDI_APPLICATION);
    for (int i = 0; tip && tip[i] && i<(int)(sizeof(n->szTip)/2)-1; i++) {
        n->szTip[i] = tip[i];
    }
    w->trayon = Shell_NotifyIconW(NIM_ADD, n) != 0;
    return w->trayon;
}

extern "C" void w32gui_trayclose(void *p)
{
    W32Gui *w = (W32Gui *)p;
    if (w->trayon) {
        Shell_NotifyIconW(NIM_DELETE, &w->nid);
        w->trayon = 0;
    }
}

// Separate from opening, since showing sends WM_SIZE, which draws, which
// needs the core to know this window. The checklist's shows as created,
// maximized or not (SW_SHOWNOACTIVATE would restore it), without the
// focus.
extern "C" void w32gui_show(void *p)
{
    W32Gui *w = (W32Gui *)p;
    ShowWindow(w->hwnd, w->hooks.drive ? SW_SHOWNA :
                        w->showmax     ? SW_SHOWMAXIMIZED  : SW_SHOWDEFAULT);
}

extern "C" int w32gui_wait(void *p, long long us)
{
    W32Gui *w = (W32Gui *)p;
    if (us < 0) {
        MsgWaitForMultipleObjectsEx(0, 0, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } else if (us > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -us * 10;  // relative, in 100 ns units
        SetWaitableTimer(w->timer, &due, 0, 0, 0, 0);
        MsgWaitForMultipleObjectsEx(1, &w->timer, INFINITE, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
    MSG m;
    while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            return 0;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 1;
}

extern "C" void w32gui_wake(void *p)
{
    W32Gui *w = (W32Gui *)p;
    if (!InterlockedExchange(&w->wakeflag, 1)) {
        PostMessageW(w->msgwin, WM_APP_WAKE, 0, 0);
    }
}

extern "C" void w32gui_newframe(void *)
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
}

extern "C" void w32gui_present(void *p, int present, unsigned clear)
{
    W32Gui     *w  = (W32Gui *)p;
    ImDrawData *dd = ImGui::GetDrawData();
    if (!present) {
        // Atlas growth must reach the GPU even when the picture holds.
        for (int i = 0; dd->Textures && i < dd->Textures->Size; i++) {
            ImTextureData *tex = (*dd->Textures)[i];
            if (tex->Status != ImTextureStatus_OK) {
                ImGui_ImplDX11_UpdateTexture(tex);
            }
        }
        return;
    }
    float rgba[4] = {
        (float)(clear&255)/255, (float)(clear>>8&255)/255,
        (float)(clear>>16&255)/255, 1,
    };
    w->context->OMSetRenderTargets(1, &w->rtv, 0);
    w->context->ClearRenderTargetView(w->rtv, rgba);
    ImGui_ImplDX11_RenderDrawData(dd);
    HRESULT r = w->swapchain->Present(1, 0);
    if (r == DXGI_STATUS_OCCLUDED) {
        // Nothing shows: rest until painted or activated again.
        w->occluded = 1;
        w->hooks.minimized(w->hooks.gui, 1);
    }
}

extern "C" long long w32gui_refresh(void *p)
{
    W32Gui *w = (W32Gui *)p;
    if (!w->hwnd) {
        return 1000000 / 60;
    }
    HDC dc = GetDC(w->hwnd);
    int hz = GetDeviceCaps(dc, VREFRESH);
    ReleaseDC(w->hwnd, dc);
    return 1000000 / (hz>1 ? hz : 60);
}

// Arrow keys for a modal size loop, paced like a hand on the keyboard,
// so the loop runs long enough for its timer and wakes to matter.
struct ModalSteer {
    HWND hwnd;
    int  steps;
};

static DWORD WINAPI steermodal(void *arg)
{
    ModalSteer *s = (ModalSteer *)arg;
    Sleep(100);
    PostMessageW(s->hwnd, WM_KEYDOWN, VK_RIGHT, 1);  // picks the right edge
    for (int i = 0; i < s->steps; i++) {
        Sleep(60);
        PostMessageW(s->hwnd, WM_KEYDOWN, VK_RIGHT, 1);
    }
    Sleep(60);
    PostMessageW(s->hwnd, WM_KEYDOWN, VK_RETURN, 1);
    return 0;
}

static DWORD WINAPI announcetheme(void *hwnd)
{
    SendMessageW((HWND)hwnd, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
    return 0;
}

extern "C" void w32gui_inject(void *p, int kind, float x, float y,
                              int button, int down, int key, int ctrl,
                              int shift, unsigned short const *text)
{
    W32Gui *w  = (W32Gui *)p;
    LPARAM  xy = MAKELPARAM((int)x, (int)y);
    if (kind == 11) {  // a tray menu pick, tray or not, window or not
        PostMessageW(w->msgwin, WM_COMMAND, TRAY_CMD + (int)x, 0);
        return;
    } else if (!w->hwnd) {
        return;
    }
    switch (kind) {
    case 0:  // move
        PostMessageW(w->hwnd, WM_MOUSEMOVE, 0, xy);
        break;
    case 1: {  // button
        UINT m = button ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                        : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
        PostMessageW(w->hwnd, m, down ? (button ? MK_RBUTTON : MK_LBUTTON) : 0, xy);
    } break;
    case 2: {  // wheel
        POINT pt = {(LONG)x, 0};
        ClientToScreen(w->hwnd, &pt);
        PostMessageW(w->hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(y*WHEEL_DELTA)),
                     MAKELPARAM(pt.x, pt.y));
    } break;
    case 3: {  // key: the backend reads modifiers from the key state
        static WORD const vks[] = {
            VK_TAB, VK_CONTROL, VK_ESCAPE, VK_RETURN, 'C', VK_SHIFT, 'A',
            VK_END, VK_CONTROL, VK_OEM_PLUS, VK_OEM_MINUS, '0', 'F', 'G', 'T',
            'R', VK_UP, VK_DOWN, 'V',
        };
        BYTE state[256];
        GetKeyboardState(state);
        state[VK_CONTROL] = state[VK_LCONTROL] = ctrl ? 0x80 : 0;
        state[VK_SHIFT]   = state[VK_LSHIFT]   = shift ? 0x80 : 0;
        if (vks[key]!=VK_CONTROL && vks[key]!=VK_SHIFT) {
            state[vks[key]] = down ? 0x80 : 0;
        }
        SetKeyboardState(state);
        WORD   vk   = vks[key]==VK_CONTROL ? VK_LCONTROL :
                      vks[key]==VK_SHIFT   ? VK_LSHIFT   : vks[key];
        UINT   scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LPARAM l    = 1 | (LPARAM)scan<<16 | (down ? 0 : (LPARAM)3<<30);
        PostMessageW(w->hwnd, down ? WM_KEYDOWN : WM_KEYUP, vks[key], l);
    } break;
    case 4:  // text
        for (int i = 0; text && text[i]; i++) {
            PostMessageW(w->hwnd, WM_CHAR, text[i], 1);
        }
        break;
    case 5: {  // resize the client area
        RECT r = {0, 0, (LONG)x, (LONG)y};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, 0, 0);
        SetWindowPos(w->hwnd, 0, 0, 0, r.right-r.left, r.bottom-r.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    } break;
    case 6: {  // the system's modal size loop, steered by arrow keys
        PostMessageW(w->hwnd, WM_SYSCOMMAND, SC_SIZE, 0);
        static ModalSteer steer;
        steer.hwnd  = w->hwnd;
        steer.steps = (int)x;
        HANDLE t = CreateThread(0, 0, steermodal, &steer, 0, 0);
        if (t) CloseHandle(t);
    } break;
    case 7: {  // as if moved to a monitor of x DPI, keeping the window size
        RECT r;
        GetWindowRect(w->hwnd, &r);
        WPARAM dpi = (WPARAM)x;
        SendMessageW(w->hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&r);
    } break;
    case 8:  // close, as from the title bar
        PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
        break;
    case 9:  // minimize, as from the title bar or the taskbar
        ShowWindow(w->hwnd, SW_SHOWMINNOACTIVE);
        break;
    case 10: {  // the system's theme changes, as Settings announces it
        // Sent from another thread, as the broadcast is: a pointer in a
        // system message cannot be posted
        w->drivetheme = x<0 ? 0 : x>0 ? 2 : 1;
        HANDLE t = CreateThread(0, 0, announcetheme, w->hwnd, 0, 0);
        if (t) CloseHandle(t);
    } break;
    case 12: {  // a file dropped, as Explorer drops it
        int n = 0;
        for (; text && text[n]; n++) {}
        SIZE_T  size = sizeof(DROPFILES) + (SIZE_T)(n + 2)*sizeof(wchar_t);
        HGLOBAL mem  = GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT, size);
        DROPFILES *df = mem ? (DROPFILES *)GlobalLock(mem) : 0;
        if (!df) {
            if (mem) GlobalFree(mem);
            break;
        }
        df->pFiles = sizeof(DROPFILES);
        df->pt.x   = (LONG)x;
        df->pt.y   = (LONG)y;
        df->fWide  = TRUE;
        memcpy(df + 1, text, (size_t)n*sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!PostMessageW(w->hwnd, WM_DROPFILES, (WPARAM)mem, 0)) {
            GlobalFree(mem);
        }
    } break;
    }
}

extern "C" void w32gui_raise(void *p)
{
    W32Gui *w = (W32Gui *)p;
    if (!w->hwnd) {
        return;
    }
    if (IsIconic(w->hwnd) || !IsWindowVisible(w->hwnd)) {
        ShowWindow(w->hwnd, w->hooks.drive ? SW_SHOWNOACTIVATE : SW_RESTORE);
    }
    if (!w->hooks.drive) {
        SetForegroundWindow(w->hwnd);
    }
}

extern "C" void w32gui_letfront(unsigned long pid)
{
    AllowSetForegroundWindow(pid);
}

extern "C" int w32gui_systheme(void *p)
{
    return systemdark((W32Gui *)p);
}

extern "C" int w32gui_theme(void *p, int dark)
{
    W32Gui *w = (W32Gui *)p;
    if (!w->hwnd) {
        return 0;
    } else if (w->framedark == dark) {
        return 1;
    }
    BOOL    v = dark;
    HRESULT r = DwmSetWindowAttribute(w->hwnd, 20, &v, sizeof(v));
    if (FAILED(r)) {
        r = DwmSetWindowAttribute(w->hwnd, 19, &v, sizeof(v));
    }
    if (FAILED(r)) {
        return 0;
    }
    if (w->framedark>=0 && IsWindowVisible(w->hwnd)) {
        BOOL active = GetActiveWindow() == w->hwnd;
        SendMessageW(w->hwnd, WM_NCACTIVATE, !active, 0);
        SendMessageW(w->hwnd, WM_NCACTIVATE, active, 0);
    }
    w->framedark = dark;
    return 1;
}

extern "C" void w32gui_close(void *p)
{
    W32Gui *w = (W32Gui *)p;
    w32gui_shed(w);
    w32gui_trayclose(w);
    SetWindowLongPtrW(w->msgwin, GWLP_USERDATA, 0);
    DestroyWindow(w->msgwin);
    UnregisterClassW(L"prospero", GetModuleHandleW(0));
    UnregisterClassW(L"prospero-msg", GetModuleHandleW(0));
}

// The save dialog runs on a thread of its own, so that the window's
// loop never waits on it: GetSaveFileNameW (GetOpenFileNameW for the
// open dialog, and IFileOpenDialog for a directory, which work the same
// way) is modal to the thread that calls it, and an owner on another
// thread would tie the two threads' input together. Its owner is a
// hidden window of that thread over the
// main window, which places it there. To be in front, a CBT hook on
// that thread names the dialog as it activates to the GUI thread
// (WM_APP_FRONT), which has the foreground to give it while the user
// works in the window.
struct W32Save {
    wchar_t title[256];
    wchar_t path[32768];
    void  (*done)(void *, unsigned short const *);
    char    ctx[32];
    HWND    main;    // the window, for its place at the call
    HWND    msgwin;  // the GUI thread's hidden window
    int     open;    // an open dialog, not a save dialog
    int     images;  // and for an image
    int     dir;     // or for a directory, starting in path
};

static thread_local HWND savemsgwin;  // the dialog's thread's W32Save's
static thread_local int  savefront;   // the dialog was named

static LRESULT CALLBACK savecbt(int code, WPARAM wp, LPARAM lp)
{
    if (code==HCBT_ACTIVATE && !savefront) {
        wchar_t cls[16];
        if (GetClassNameW((HWND)wp, cls, 16) && !lstrcmpW(cls, L"#32770")) {
            savefront = 1;
            PostMessageW(savemsgwin, WM_APP_FRONT, wp, 0);
        }
    }
    return CallNextHookEx(0, code, wp, lp);
}

// The folder dialog, which GetOpenFileNameW cannot be: the shell's
// IFileOpenDialog picking folders (Vista and later), starting in the
// directory in s->path, where it puts the answer. Returns 0 if
// cancelled, or if no dialog could be shown.
static int pickdir(W32Save *s, HWND owner)
{
    IFileOpenDialog *d = 0;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), 0, CLSCTX_INPROC_SERVER,
                                __uuidof(IFileOpenDialog), (void **)&d))) {
        return 0;
    }
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                  FOS_NOCHANGEDIR);
    d->SetTitle(s->title);
    for (wchar_t *c = s->path; *c; c++) {
        *c = *c=='/' ? '\\' : *c;  // the shell parses only backslashes
    }
    IShellItem *at = 0;
    if (s->path[0] && SUCCEEDED(SHCreateItemFromParsingName(s->path, 0, __uuidof(IShellItem),
                                                            (void **)&at))) {
        d->SetFolder(at);
        at->Release();
    }
    int         ok = 0;
    IShellItem *r  = 0;
    if (SUCCEEDED(d->Show(owner)) && SUCCEEDED(d->GetResult(&r))) {
        wchar_t *path = 0;
        if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            lstrcpynW(s->path, path, sizeof(s->path)/sizeof(s->path[0]));
            CoTaskMemFree(path);
            ok = 1;
        }
        r->Release();
    }
    d->Release();
    return ok;
}

static DWORD WINAPI savethread(void *arg)
{
    W32Save *s  = (W32Save *)arg;
    HRESULT  co = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    RECT     r  = {};
    HWND     ow = 0;
    if (s->main && GetWindowRect(s->main, &r)) {
        ow = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                             r.left, r.top, r.right-r.left, r.bottom-r.top,
                             0, 0, GetModuleHandleW(0), 0);
    }
    savemsgwin = s->msgwin;
    savefront  = 0;
    HHOOK hook = s->msgwin ? SetWindowsHookExW(WH_CBT, savecbt, 0, GetCurrentThreadId()) : 0;
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = ow;
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.lpstrFile   = s->path;
    ofn.nMaxFile    = sizeof(s->path)/sizeof(s->path[0]);
    ofn.lpstrTitle  = s->title;
    ofn.Flags       = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                      OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (s->open) {
        ofn.lpstrFilter = s->images
                        ? L"Images (*.png, *.jpg, *.gif, *.webp, *.bmp)\0"
                          L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp\0All files\0*.*\0"
                        : L"Session logs (*.jsonl)\0*.jsonl\0All files\0*.*\0";
        ofn.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                          OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    }
    if (s->dir ? !pickdir(s, ow) :
                 !(s->open ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn))) {
        s->path[0] = 0;
    }
    if (hook) {
        UnhookWindowsHookEx(hook);
    }
    if (ow) {
        DestroyWindow(ow);
    }
    s->done(s->ctx, (unsigned short const *)s->path);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    HeapFree(GetProcessHeap(), 0, s);
    return 0;
}

static int w32dialog(void *p, unsigned short const *title,
                     unsigned short const *name,
                     void (*done)(void *, unsigned short const *),
                     void const *ctx, int ctxlen, int open, int images, int dir = 0)
{
    W32Gui  *w = (W32Gui *)p;
    W32Save *s = (W32Save *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(W32Save));
    if (!s || ctxlen>(int)sizeof(s->ctx)) {
        if (s) HeapFree(GetProcessHeap(), 0, s);
        return 0;
    }
    s->main   = w ? w->hwnd : 0;
    s->msgwin = w ? w->msgwin : 0;
    lstrcpynW(s->title, (wchar_t const *)title, sizeof(s->title)/sizeof(s->title[0]));
    lstrcpynW(s->path, (wchar_t const *)name, sizeof(s->path)/sizeof(s->path[0]));
    memcpy(s->ctx, ctx, (size_t)ctxlen);
    s->done   = done;
    s->open   = open;
    s->images = images;
    s->dir    = dir;
    HANDLE t = CreateThread(0, 0, savethread, s, 0, 0);
    if (!t) {
        HeapFree(GetProcessHeap(), 0, s);
        return 0;
    }
    CloseHandle(t);
    return 1;
}

extern "C" int w32gui_save(void *p, unsigned short const *title,
                           unsigned short const *name,
                           void (*done)(void *, unsigned short const *),
                           void const *ctx, int ctxlen)
{
    return w32dialog(p, title, name, done, ctx, ctxlen, 0, 0);
}

extern "C" int w32gui_openfile(void *p, unsigned short const *title, int images,
                               void (*done)(void *, unsigned short const *),
                               void const *ctx, int ctxlen)
{
    return w32dialog(p, title, (unsigned short const *)L"", done, ctx, ctxlen, 1, images);
}

extern "C" int w32gui_opendir(void *p, unsigned short const *title,
                              unsigned short const *at,
                              void (*done)(void *, unsigned short const *),
                              void const *ctx, int ctxlen)
{
    return w32dialog(p, title, at, done, ctx, ctxlen, 1, 0, 1);
}

extern "C" long long w32gui_clip(void *p, int format, void *buf, long long cap)
{
    W32Gui *w  = (W32Gui *)p;
    UINT    cf = format==W32CLIP_PNG   ? RegisterClipboardFormatW(L"PNG") :
                 format==W32CLIP_DIBV5 ? CF_DIBV5 :
                 format==W32CLIP_DIB   ? CF_DIB : CF_HDROP;
    if (!cf || !IsClipboardFormatAvailable(cf) || !OpenClipboard(w ? w->hwnd : 0)) {
        return -1;
    }
    long long n = -1;
    HANDLE    h = GetClipboardData(cf);
    if (h && cf==CF_HDROP) {
        UINT len = DragQueryFileW((HDROP)h, 0, 0, 0);
        if (len) {
            n = (long long)(len + 1) * (long long)sizeof(wchar_t);
            if (buf && cap>=n) {
                DragQueryFileW((HDROP)h, 0, (wchar_t *)buf, len + 1);
            }
        }
    } else if (h) {
        void  *d  = GlobalLock(h);
        SIZE_T sz = GlobalSize(h);
        if (d) {
            n = (long long)sz;
            if (buf && cap>=n) {
                memcpy(buf, d, sz);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return n;
}

extern "C" int w32gui_clipput(void *p, void const *png, long long len)
{
    W32Gui *w  = (W32Gui *)p;
    UINT    cf = RegisterClipboardFormatW(L"PNG");
    HGLOBAL h  = cf ? GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len) : 0;
    void   *d  = h ? GlobalLock(h) : 0;
    if (!d) {
        if (h) GlobalFree(h);
        return 0;
    }
    memcpy(d, png, (size_t)len);
    GlobalUnlock(h);
    if (!OpenClipboard(w ? w->hwnd : 0)) {
        GlobalFree(h);
        return 0;
    }
    EmptyClipboard();
    int ok = SetClipboardData(cf, h) != 0;
    CloseClipboard();
    if (!ok) {
        GlobalFree(h);
    }
    return ok;
}

extern "C" int w32gui_reveal(unsigned short const *dir)
{
    HINSTANCE r = ShellExecuteW(0, L"open", (wchar_t const *)dir, 0, 0, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// The shell opens its folder, or finds a window that has it open, with
// the file selected. It wants COM on the calling thread.
extern "C" int w32gui_select(unsigned short const *file)
{
    HRESULT          co = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE id = ILCreateFromPathW((wchar_t const *)file);
    HRESULT          r  = id ? SHOpenFolderAndSelectItems(id, 0, 0, 0) : E_FAIL;
    if (id) {
        ILFree(id);
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return SUCCEEDED(r);
}

// CRT startup lands here (-municode), and the core takes over.
extern "C" void prospero_main();

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    prospero_main();
    return 0;
}


#else  // the core's side, included by main_win32.cpp


static_assert((i32)W32IN_MOVE==(i32)INPUT_MOVE && (i32)W32IN_OTHER==(i32)INPUT_OTHER);
static_assert(GEV_MOVE==0 && GEV_RESIZE==5 && GEV_MODALSIZE==6 && GEV_DPI==7 &&
              GEV_CLOSE==8 && GEV_MINIMIZE==9 && GEV_SYSTHEME==10 && GEV_TRAY==11 &&
              GEV_DROP==12);
static_assert(TRAY_OPEN==0 && TRAY_STOPALL==1 && TRAY_QUIT==2);

W32(i32, GetCurrentThreadId());

static void hookinput(void *g, int kind) { gui_input((Gui *)g, kind); }
static void hookwake(void *g)            { gui_wake((Gui *)g); }
static void hookforce(void *g)           { gui_force((Gui *)g); }
static void hookframe(void *g)           { gui_frame((Gui *)g); }
static void hookscale(void *g, float s)  { gui_scale((Gui *)g, s); }
static void hookminimized(void *g, int m){ gui_minimized((Gui *)g, m); }
static void hookmodal(void *g, int m)    { gui_modal((Gui *)g, m); }
static void hookclose(void *g)           { gui_close((Gui *)g); }
static void hooksystheme(void *g, int d) { gui_systheme((Gui *)g, d); }
static void hooktray(void *g, int item)  { gui_tray((Gui *)g, item); }

static WinGeom fromgeom(int const *geom)
{
    WinGeom r = {};
    r.box = {geom[GEOM_X], geom[GEOM_Y], geom[GEOM_W], geom[GEOM_H]};
    r.max = geom[GEOM_MAX];
    r.ok  = geom[GEOM_OK];
    return r;
}

static void hookplaced(void *g, int const *geom)
{
    gui_placed((Gui *)g, fromgeom(geom));
}

static void hookdrop(void *p, unsigned short const *path)
{
    Gui *g = (Gui *)p;
    iz   n = 0;
    for (; path[n]; n++) {}
    gui_drop(g, fromwide(&g->frame, (c16 *)path, n), 0);  // until the next frame
}

static i32 os_guidisplays(Os *, WinBox *out, i32 max)
{
    int xywh[4*16];
    int n = w32gui_displays(xywh, max<16 ? max : 16);
    for (int i = 0; i < n; i++) {
        out[i] = {xywh[4*i], xywh[4*i+1], xywh[4*i+2], xywh[4*i+3]};
    }
    return n;
}

// A geometry fitted to the displays as they are now (place.cpp), as
// GEOM_* for the edge.
static void fitgeom(Os *os, WinGeom g, int *geom)
{
    WinBox disp[16];
    i32    n = os_guidisplays(os, disp, countof(disp));
    WinBox b = g.ok ? placewindow(g.box, disp, n) : g.box;
    geom[GEOM_X]   = b.x;
    geom[GEOM_Y]   = b.y;
    geom[GEOM_W]   = b.w;
    geom[GEOM_H]   = b.h;
    geom[GEOM_MAX] = g.max;
    geom[GEOM_OK]  = g.ok;
}

static WinGeom os_guiwhere(Os *os)
{
    int geom[GEOM_COUNT] = {};
    return os->gui && w32gui_where(os->gui, geom) ? fromgeom(geom) : WinGeom{};
}

static void os_guiplace(Os *os, WinGeom g)
{
    int geom[GEOM_COUNT];
    fitgeom(os, g, geom);
    if (os->gui) {
        w32gui_place(os->gui, geom);
    }
}

static void hooktick(void *p)
{
    Gui *g = (Gui *)p;
    g->sched.now = os_ticks(g->os);
    if (scheddue(&g->sched)) {
        gui_frame(g);
    }
}

static void os_wake(Os *os)
{
    if (os->gui) {
        w32gui_wake(os->gui);
    }
}

static b32 os_guiopen(Os *os, Gui *g, Str title)
{
    Arena t = g->frame;
    unsigned short *wtitle = (unsigned short *)towide(&t, title).data;
    // Where it was last, if anywhere, else its first size: the work
    // area's pixels hold it at the primary display's scale, or not
    WinGeom at = g->geom;
    if (!at.ok) {
        WinBox d = g->firstdisp;
        if (!d.w) {
            os_guidisplays(os, &d, 1);
        }
        at = firstgeom(d, w32gui_scale0());
    }
    int geom[GEOM_COUNT];
    fitgeom(os, at, geom);
    if (os->gui) {
        // Again, after os_guished
        if (!w32gui_reopen(os->gui, wtitle, geom)) {
            return 0;
        }
        gui_systheme(g, w32gui_systheme(os->gui));
        w32gui_show(os->gui);
        return 1;
    }
    W32Hooks h = {};
    h.gui       = g;
    h.input     = hookinput;
    h.wake      = hookwake;
    h.force     = hookforce;
    h.tick      = hooktick;
    h.frame     = hookframe;
    h.scale     = hookscale;
    h.minimized = hookminimized;
    h.modal     = hookmodal;
    h.close     = hookclose;
    h.systheme  = hooksystheme;
    h.tray      = hooktray;
    h.placed    = hookplaced;
    h.drop      = hookdrop;
    h.drive     = g->drive;
    os->gui = w32gui_open(&h, wtitle, geom);
    if (!os->gui) {
        return 0;
    }
    gui_systheme(g, w32gui_systheme(os->gui));  // the title bar before it shows
    w32gui_show(os->gui);
    if (g->drive) {
        Arena a = g->frame;
        Buf   b(&a, 64);
        print(&b, "drive: GUI thread ");
        print(&b, (i64)GetCurrentThreadId());
        guilog(g, finish(&b));
    }
    return 1;
}

static b32 os_guiwait(Os *os, i64 us)
{
    return w32gui_wait(os->gui, us);
}

static void os_guinewframe(Os *os)
{
    w32gui_newframe(os->gui);
}

static void os_guipresent(Os *os, b32 present, u32 clear)
{
    w32gui_present(os->gui, present, clear);
}

static i64 os_guirefresh(Os *os)
{
    return w32gui_refresh(os->gui);
}

static void os_guiinject(Os *os, GuiEvent *e)
{
    Arena a = {};
    u8    buf[1024];
    a.beg = (byte *)buf;
    a.end = (byte *)buf + countof(buf);
    unsigned short *text = 0;
    if (e->text.len) {
        text = (unsigned short *)towide(&a, e->text).data;
    }
    w32gui_inject(os->gui, e->kind, e->x, e->y, e->button, e->down, e->key,
                  e->ctrl, e->shift, text);
}

static void os_guiraise(Os *os)
{
    w32gui_raise(os->gui);
}

static void os_guitoken(Os *, Str) {}

static void os_guiletfront(Os *, i64 pid)
{
    w32gui_letfront((unsigned long)pid);
}

static b32 os_guitheme(Os *os, b32 dark)
{
    return os->gui ? w32gui_theme(os->gui, dark) : 0;
}

static void os_guiclose(Os *os)
{
    w32gui_close(os->gui);
    os->gui = 0;
}

static void os_guished(Os *os)
{
    w32gui_shed(os->gui);
}

static b32 os_trayopen(Os *os, Str tip)
{
    u8    buf[512];
    Arena a = {};
    a.beg = (byte *)buf;
    a.end = (byte *)buf + countof(buf);
    return w32gui_trayopen(os->gui, (unsigned short *)towide(&a, tip).data);
}

static void os_trayclose(Os *os)
{
    w32gui_trayclose(os->gui);
}

struct W32SaveCtx {
    Mailbox *mb;
    u32      type;
    u64      id;
};

// On the dialog's thread: the path, in UTF-8, to the mailbox.
static void savedone(void *ctx, unsigned short const *path)
{
    W32SaveCtx *c = (W32SaveCtx *)ctx;
    static_assert(sizeof(W32SaveCtx) <= 32);
    byte  mem[1<<17];
    Arena a = {mem, mem+countof(mem)};
    iz    n = 0;
    for (; path[n]; n++) {}
    Str   p = fromwide(&a, (c16 *)path, n);
    mailpost(c->mb, c->type, c->id, p, 1);
}

static b32 os_guisave(Os *os, Arena scratch, Str title, Str name, Mailbox *mb,
                      u32 type, u64 id)
{
    W32SaveCtx c = {mb, type, id};
    return w32gui_save(os->gui, (unsigned short *)towide(&scratch, title).data,
                       (unsigned short *)towide(&scratch, name).data,
                       savedone, &c, (int)sizeof(c));
}

static b32 os_guiopenfile(Os *os, Arena scratch, Str title, i32 kind, Mailbox *mb,
                          u32 type, u64 id)
{
    W32SaveCtx c = {mb, type, id};
    return w32gui_openfile(os->gui, (unsigned short *)towide(&scratch, title).data,
                           kind==OPEN_IMAGE, savedone, &c, (int)sizeof(c));
}

static b32 os_guiopendir(Os *os, Arena scratch, Str title, Str at, Mailbox *mb,
                         u32 type, u64 id)
{
    W32SaveCtx c = {mb, type, id};
    return w32gui_opendir(os->gui, (unsigned short *)towide(&scratch, title).data,
                          (unsigned short *)towide(&scratch, at).data,
                          savedone, &c, (int)sizeof(c));
}

static i32 dibint(u8 *p)
{
    return (i32)((u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24);
}

// A device-independent bitmap, 24-bit, or 32-bit as BGRA (its alpha
// kept unless all of it is zero, as most writers leave it), as a PNG in
// a. Null if it is another kind.
static Str dibpng(Os *os, Arena *a, Str dib)
{
    Str r = {};
    if (dib.len < 40) {
        return r;
    }
    u8 *p    = dib.data;
    i32 hdr  = dibint(p);
    i32 w    = dibint(p+4);
    i32 h    = dibint(p+8);
    i32 bpp  = p[14] | p[15]<<8;
    i32 comp = dibint(p+16);
    b32 down = h < 0;  // top-down
    h = down ? -h : h;
    if (hdr<40 || hdr>dib.len || w<1 || h<1 || (bpp!=24 && bpp!=32) ||
            (comp!=0 && comp!=3) || (iz)w*h>(iz)ATTACH_PIXELS) {
        return r;
    }
    // BI_BITFIELDS: its masks follow a short header, or are in a long
    // one, and only the usual BGRA is taken
    iz off = hdr + (comp==3 && hdr==40 ? 12 : 0);
    if (comp==3 && (off>dib.len || dibint(p+40)!=0xff0000 ||
                    dibint(p+44)!=0xff00 || dibint(p+48)!=0xff)) {
        return r;
    }
    iz stride = ((iz)w*bpp/8 + 3) & ~(iz)3;
    if (off>dib.len || stride*h>dib.len-off) {
        return r;
    }
    iz      pixels = (iz)w * h;
    Region *reg    = os_reserve(os, 14*pixels + ((iz)4<<20));
    if (!reg) {
        return r;
    }
    Arena t     = newarena(reg);
    u8   *rgba  = allocbytes(&t, 4*pixels);
    b32   alpha = 0;
    for (i32 y = 0; y < h; y++) {
        u8 *src = p + off + stride*(down ? y : h-1-y);
        u8 *dst = rgba + (iz)y*w*4;
        for (i32 x = 0; x < w; x++) {
            u8 *s = src + x*(bpp/8);
            dst[x*4+0] = s[2];
            dst[x*4+1] = s[1];
            dst[x*4+2] = s[0];
            dst[x*4+3] = bpp==32 ? s[3] : 255;
            alpha |= bpp==32 && s[3];
        }
    }
    for (iz i = 0; !alpha && i < pixels; i++) {
        rgba[i*4+3] = 255;
    }
    Str png = pngencodez(&t, rgba, w, h, 0);
    if (png.len <= (iz)ATTACH_UPLOAD) {
        r = clone(a, png);
    }
    os_release(os, reg);
    return r;
}

// The clipboard's image: a PNG as it is, else a bitmap made one, else
// the first file copied, by its path.
static ClipImage os_guiclipimage(Os *os, Arena *a)
{
    ClipImage r = {};
    for (i32 f = W32CLIP_PNG; f <= W32CLIP_FILE; f++) {
        long long n = w32gui_clip(os->gui, f, 0, 0);
        if (n<1 || n>ATTACH_UPLOAD) {
            continue;
        }
        Arena t   = *a;
        u8   *buf = allocbytes(&t, (iz)n);
        if (w32gui_clip(os->gui, f, buf, n) != n) {
            continue;
        }
        if (f == W32CLIP_PNG) {
            *a     = t;
            r.data = {buf, (iz)n};
            return r;
        } else if (f == W32CLIP_FILE) {
            iz len = 0;
            for (c16 *w = (c16 *)buf; len<n/2 && w[len]; len++) {}
            *a     = t;
            r.path = fromwide(a, (c16 *)buf, len);
            return r;
        }
        Str png = dibpng(os, &t, {buf, (iz)n});
        if (png.len) {
            *a     = t;
            r.data = png;
            return r;
        }
    }
    return r;
}

static b32 os_guiclipput(Os *os, Arena, Str png)
{
    return w32gui_clipput(os->gui, png.data, png.len);
}

static u32 os_guilimits(Os *)
{
    return 0;
}

static b32 os_guireveal(Os *, Arena scratch, Str dir)
{
    return w32gui_reveal((unsigned short *)towide(&scratch, dir).data);
}

static b32 os_guishow(Os *, Arena scratch, Str file)
{
    Str16 w = towide(&scratch, file);
    for (iz i = 0; i < w.len; i++) {
        w.data[i] = w.data[i]=='/' ? '\\' : w.data[i];  // as the shell parses
    }
    return w32gui_select((unsigned short *)w.data);
}

#endif
