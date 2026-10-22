// Desktop helper, Win32 platform layer: no CRT and no headers
// $ c++ -std=c++20 -nostartfiles -o prospero-desktop.exe
//       src/main_desktop_win32.cpp -luser32 -lgdi32
//
// Imports only kernel32, user32 and gdi32, with nothing newer than
// Windows XP SP3, so one i686 build runs everywhere (tests/imports.py
// checks it). Newer functions, DPI awareness and the DWM's frame bounds,
// are looked up at run time. The window, input and capture code is
// winctl's (~/src/sshd/src/winctl.cpp).
//
// This is free and unencumbered software released into the public domain.
#include "base.cpp"
#include "wide.cpp"
#include "deskhelp.cpp"

using u16 = decltype((unsigned short)0);
using i16 = decltype((short)0);

enum : i32 {
    CP_UTF8                    = 65001,
    CREATE_BREAKAWAY_FROM_JOB  = 0x01000000,
    CREATE_NEW_CONSOLE         = 0x00000010,
    CREATE_UNICODE_ENVIRONMENT = 0x00000400,
    ERROR_ACCESS_DENIED        = 5,
    MEM_COMMIT                 = 0x1000,
    MEM_RELEASE                = 0x8000,
    MEM_RESERVE                = 0x2000,
    PAGE_NOACCESS              = 1,
    PAGE_READWRITE             = 4,
    PROCESS_QUERY_INFORMATION  = 0x0400,
    STD_ERROR_HANDLE           = -12,
    STD_INPUT_HANDLE           = -10,
    STD_OUTPUT_HANDLE          = -11,
    SYNCHRONIZE                = 0x00100000,
    WAIT_TIMEOUT               = 258,
    SEM_FAILCRITICALERRORS     = 0x0001,
    SEM_NOGPFAULTERRORBOX      = 0x0002,

    CAPTUREBLT                 = 0x40000000,
    SRCCOPY                    = 0x00CC0020,

    GW_OWNER                   = 4,
    INPUT_MOUSE                = 0,
    INPUT_KEYBOARD             = 1,
    KEYEVENTF_EXTENDEDKEY      = 0x0001,
    KEYEVENTF_KEYUP            = 0x0002,
    KEYEVENTF_UNICODE          = 0x0004,
    MOUSEEVENTF_MOVE           = 0x0001,
    MOUSEEVENTF_LEFTDOWN       = 0x0002,
    MOUSEEVENTF_LEFTUP         = 0x0004,
    MOUSEEVENTF_RIGHTDOWN      = 0x0008,
    MOUSEEVENTF_RIGHTUP        = 0x0010,
    MOUSEEVENTF_WHEEL          = 0x0800,
    MOUSEEVENTF_VIRTUALDESK    = 0x4000,
    MOUSEEVENTF_ABSOLUTE       = 0x8000,
    SM_CXDRAG                  = 68,
    SM_CYDRAG                  = 69,
    SM_XVIRTUALSCREEN          = 76,
    SM_YVIRTUALSCREEN          = 77,
    SM_CXVIRTUALSCREEN         = 78,
    SM_CYVIRTUALSCREEN         = 79,
    SW_RESTORE                 = 9,
    SWP_NOSIZE                 = 0x0001,
    SWP_NOZORDER               = 0x0004,
    SWP_NOACTIVATE             = 0x0010,
    WM_CLOSE                   = 0x0010,
    VK_TAB                     = 0x09,
    VK_RETURN                  = 0x0D,
    VK_SHIFT                   = 0x10,

    DWMWA_EXTENDED_FRAME_BOUNDS = 9,
    DWMWA_CLOAKED               = 14,
};

struct Rect {
    i32 left, top, right, bottom;
};

struct Point {
    i32 x, y;
};

struct Msg {  // MSG, only for PeekMessageW
    uz    hwnd;
    u32   message;
    uz    wparam;
    iz    lparam;
    u32   time;
    Point pt;
    u32   priv;
};

struct Input {  // INPUT
    u32 type;
    union {
        struct {
            i32 dx, dy;
            u32 data, flags, time;
            uz  extra;
        } mi;
        struct {
            u16 vk, scan;
            u32 flags, time;
            uz  extra;
        } ki;
    };
};
static_assert(sizeof(Input) == (sizeof(uz)==4 ? 28 : 40));

struct BitmapInfo {  // BITMAPINFO with BITMAPINFOHEADER
    u32 size;
    i32 width, height;
    u16 planes, bitcount;
    u32 compression, sizeimage;
    i32 xppm, yppm;
    u32 clrused, clrimportant;
    u32 colors[1];
};

struct StartupInfo {  // STARTUPINFOW
    u32  cb;
    c16 *reserved;
    c16 *desktop;
    c16 *title;
    u32  x, y, xsize, ysize, xchars, ychars, fill, flags;
    u16  show, reserved2;
    void *reserved3;
    uz   in, out, err;
};

struct ProcessInfo {  // PROCESS_INFORMATION
    uz  process;
    uz  thread;
    u32 pid;
    u32 tid;
};

#define W32(r, p) extern "C" __declspec(dllimport) r __stdcall p noexcept
W32(b32,    CloseHandle(uz));
W32(b32,    CreateProcessW(c16 *, c16 *, void *, void *, b32, u32,
                           void *, c16 *, StartupInfo *, ProcessInfo *));
W32(void,   ExitProcess[[noreturn]](u32));
W32(c16 *,  GetCommandLineW());
W32(b32,    GetConsoleMode(uz, u32 *));
W32(u32,    GetCurrentThreadId());
W32(u32,    GetLastError());
W32(uz,     GetModuleHandleW(c16 *));
W32(void *, GetProcAddress(uz, char *));
W32(uz,     GetStdHandle(i32));
W32(u32,    GetTickCount());
W32(uz,     LoadLibraryW(c16 *));
W32(uz,     OpenProcess(u32, b32, u32));
W32(b32,    ReadFile(uz, void *, u32, u32 *, void *));
W32(u32,    SetErrorMode(u32));
W32(void,   Sleep(u32));
W32(byte *, VirtualAlloc(byte *, uz, u32, u32));
W32(b32,    VirtualFree(byte *, uz, u32));
W32(b32,    WriteFile(uz, void *, u32, u32 *, void *));
W32(b32,    AttachThreadInput(u32, u32, b32));
W32(b32,    BringWindowToTop(uz));
W32(b32,    EnumWindows(i32 (__stdcall *)(uz, iz), iz));
W32(i32,    GetClassNameW(uz, c16 *, i32));
W32(b32,    GetCursorPos(Point *));
W32(uz,     GetDC(uz));
W32(uz,     GetForegroundWindow());
W32(i32,    GetSystemMetrics(i32));
W32(uz,     GetWindow(uz, u32));
W32(b32,    GetWindowRect(uz, Rect *));
W32(i32,    GetWindowTextW(uz, c16 *, i32));
W32(u32,    GetWindowThreadProcessId(uz, u32 *));
W32(b32,    IsIconic(uz));
W32(b32,    IsWindow(uz));
W32(b32,    IsWindowVisible(uz));
W32(u32,    MapVirtualKeyW(u32, u32));
W32(b32,    PeekMessageW(Msg *, uz, u32, u32, u32));
W32(b32,    PostMessageW(uz, u32, uz, iz));
W32(i32,    ReleaseDC(uz, uz));
W32(u32,    SendInput(u32, Input *, i32));
W32(b32,    SetCursorPos(i32, i32));
W32(b32,    SetForegroundWindow(uz));
W32(b32,    SetWindowPos(uz, uz, i32, i32, i32, i32, u32));
W32(b32,    ShowWindow(uz, i32));
W32(i16,    VkKeyScanW(c16));
W32(u32,    WaitForInputIdle(uz, u32));
W32(b32,    BitBlt(uz, i32, i32, i32, i32, uz, i32, i32, u32));
W32(uz,     CreateCompatibleDC(uz));
W32(uz,     CreateDIBSection(uz, BitmapInfo *, u32, void **, uz, u32));
W32(b32,    DeleteDC(uz));
W32(b32,    DeleteObject(uz));
W32(b32,    GdiFlush());
W32(uz,     SelectObject(uz, uz));

// GCC emits calls to these even without a CRT. Defining them keeps
// msvcrt.dll out of the imports.
#if defined(__x86_64__) || defined(__i386__)
extern "C" void *memcpy(void *dst, void const *src, uz len)
{
    void *r = dst;
    __asm volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(len) : : "memory");
    return r;
}

extern "C" void *memset(void *dst, i32 c, uz len)
{
    void *r = dst;
    __asm volatile ("rep stosb" : "+D"(dst), "+c"(len) : "a"(c) : "memory");
    return r;
}

extern "C" i32 memcmp(void const *a, void const *b, uz len)
{
    u8 *x = (u8 *)a;
    u8 *y = (u8 *)b;
    for (uz i = 0; i < len; i++) {
        if (x[i] != y[i]) {
            return x[i] - y[i];
        }
    }
    return 0;
}
#endif

struct Dk {
    uz  std[3];
    // From Vista on, else null
    i32 (__stdcall *dwmattr)(uz, u32, void *, u32);
};

static b32 writeall(uz h, Str s)
{
    for (iz off = 0; off < s.len;) {
        u32 n   = s.len-off>(1<<20) ? (1<<20) : (u32)(s.len-off);
        u32 got = 0;
        if (!WriteFile(h, s.data+off, n, &got, 0) || !got) {
            return 0;
        }
        off += got;
    }
    return 1;
}

[[noreturn]] static void os_oom()
{
    writeall(GetStdHandle(STD_ERROR_HANDLE), "prospero-desktop: out of memory\n");
    ExitProcess(DK_FAIL);
}

static b32 os_commit(Region *r, iz upto)
{
    if (upto <= r->committed) {
        return 1;
    }
    byte *top = r->base + r->committed;
    iz    len = upto - r->committed;
    if (!VirtualAlloc(top, (uz)len, MEM_COMMIT, PAGE_READWRITE)) {
        return 0;
    }
    r->committed = upto;
    return 1;
}

static Region *reserve(iz cap)
{
    iz    page = 1<<12;
    byte *p    = VirtualAlloc(0, (uz)cap, MEM_RESERVE, PAGE_NOACCESS);
    if (!p || !VirtualAlloc(p, (uz)page, MEM_COMMIT, PAGE_READWRITE)) {
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


// Windows

static Str dk_os(Dk *)
{
    return "windows";
}

static Str dk_units(Dk *)
{
    return "pixels";  // physical, since the process is DPI aware
}

static Str dk_why(Dk *)
{
    return {};
}

// Windows asks no permission of a desktop program of the user's own.
static b32 dk_access(Dk *, Buf *, b32)
{
    return 1;
}

static Str dk_ready(Dk *)
{
    return {};  // a desktop always, if perhaps a locked one
}

// The visible frame. On Vista and later GetWindowRect includes invisible
// resize borders, so prefer the DWM's frame bounds when available.
static DkRect winrect(Dk *dk, uz hwnd)
{
    Rect r = {};
    if (!dk->dwmattr || IsIconic(hwnd) ||
            dk->dwmattr(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) {
        GetWindowRect(hwnd, &r);
    }
    return {r.left, r.top, r.right, r.bottom};
}

// Visible and not cloaked: Windows 8 and later keep many invisible
// "visible" windows around, such as suspended store apps.
static b32 onscreen(Dk *dk, uz hwnd)
{
    if (!IsWindowVisible(hwnd)) {
        return 0;
    }
    u32 cloaked = 0;
    if (dk->dwmattr) {
        dk->dwmattr(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    }
    return !cloaked;
}

static DkWin describe(Dk *dk, Arena *a, uz hwnd)
{
    DkWin r = {};
    c16   buf[256];
    GetWindowThreadProcessId(hwnd, &r.pid);
    r.id    = hwnd;
    r.rect  = winrect(dk, hwnd);
    r.title = fromwide(a, buf, GetWindowTextW(hwnd, buf, countof(buf)));
    r.cls   = fromwide(a, buf, GetClassNameW(hwnd, buf, countof(buf)));
    return r;
}

struct Enum {
    Arena    *a;
    Slice<uz> hwnds;
};

static i32 __stdcall enumproc(uz hwnd, iz arg)
{
    Enum *e = (Enum *)arg;
    e->hwnds = push(e->a, e->hwnds, hwnd);
    return 1;
}

static Slice<DkWin> dk_windows(Dk *dk, Arena *a, b32 all)
{
    Enum e = {};
    e.a = a;
    EnumWindows(enumproc, (iz)&e);
    Slice<DkWin> r = {};
    for (iz i = 0; i < e.hwnds.len; i++) {
        uz h = e.hwnds[i];
        if (all || onscreen(dk, h)) {
            r = push(a, r, describe(dk, a, h));
        }
    }
    return r;
}

static DkWin dk_window(Dk *dk, Arena *a, u64 id)
{
    DkWin r = {};
    if (id==(uz)id && IsWindow((uz)id)) {
        r = describe(dk, a, (uz)id);
    }
    return r;
}

static DkWin dk_screen(Dk *)
{
    DkWin r = {};
    r.rect.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.rect.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.rect.right  = r.rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.rect.bottom = r.rect.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    r.title = "screen";
    return r;
}

static u64 dk_foreground(Dk *)
{
    return GetForegroundWindow();
}

static b32 dk_popup(Dk *, DkWin *w)
{
    return w->cls=="#32768" || w->cls=="SysShadow" ||
           w->cls=="tooltips_class32";
}


// Input

// Input goes to the foreground window, or to one of its owned popups
// (dialogs) when the target itself is disabled under them.
static b32 isforeground(uz hwnd)
{
    for (uz h = GetForegroundWindow(); h; h = GetWindow(h, GW_OWNER)) {
        if (h == hwnd) {
            return 1;
        }
    }
    return 0;
}

static b32 sendinput(Input *in, iz n)
{
    return (iz)SendInput((u32)n, in, sizeof(Input)) == n;
}

// SetForegroundWindow is refused to background processes, which is what
// a program run over SSH, or by the harness, is. Attaching to the input
// queue of the current foreground thread lifts the restriction.
static b32 dk_activate(Dk *dk, DkWin *w)
{
    uz hwnd = (uz)w->id;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    b32 ok = isforeground(hwnd);

    Msg msg;
    PeekMessageW(&msg, 0, 0, 0, 0);  // AttachThreadInput needs a queue
    for (i32 attempt = 0; !ok && attempt < 2; attempt++) {
        if (attempt) {
            // A synthesized input event makes this process the source
            // of the last input, which also permits a foreground change.
            Input in = {};
            in.type     = INPUT_MOUSE;
            in.mi.flags = MOUSEEVENTF_MOVE;
            sendinput(&in, 1);
        }
        u32 self = GetCurrentThreadId();
        u32 tid  = GetWindowThreadProcessId(hwnd, 0);
        uz  fg   = GetForegroundWindow();
        u32 fgt  = fg ? GetWindowThreadProcessId(fg, 0) : 0;
        b32 a1   = fgt && fgt!=self && AttachThreadInput(self, fgt, 1);
        b32 a2   = tid!=self && tid!=fgt && AttachThreadInput(self, tid, 1);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (a2) AttachThreadInput(self, tid, 0);
        if (a1) AttachThreadInput(self, fgt, 0);

        for (i32 i = 0; !ok && i < 25; i++) {
            ok = isforeground(hwnd);
            Sleep(ok ? 50 : 20);  // once there, let it repaint
        }
    }
    w->rect = winrect(dk, hwnd);  // it may have been minimized
    return ok;
}

static u8 *dk_capture(Dk *, Arena *a, DkRect r, DkSize *size)
{
    i32 w = r.right - r.left;
    i32 h = r.bottom - r.top;
    *size = {w, h};
    // Copy the screen, not the window, so popups over it are included
    uz sdc = GetDC(0);
    uz mdc = CreateCompatibleDC(sdc);
    BitmapInfo bi = {};
    bi.size     = 40;
    bi.width    = w;
    bi.height   = -h;  // top-down
    bi.planes   = 1;
    bi.bitcount = 32;
    void *bits  = 0;
    uz    bmp   = CreateDIBSection(sdc, &bi, 0, &bits, 0, 0);
    u8   *r32   = 0;
    if (bmp) {
        uz old = SelectObject(mdc, bmp);
        if (BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY|CAPTUREBLT)) {
            GdiFlush();
            iz len = (iz)w * h * 4;
            r32 = allocbytes(a, len);
            copybytes(r32, bits, len);
        }
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(0, sdc);
    return r32;
}

// Move the cursor to a screen position in physical pixels.
static b32 dk_pointer(Dk *, i32 x, i32 y)
{
    i64 vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    i64 vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    i64 vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    i64 vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    vw = vw>1 ? vw : 2;
    vh = vh>1 ? vh : 2;
    Input in = {};
    in.type     = INPUT_MOUSE;
    in.mi.dx    = (i32)(((x - vx)*65535 + (vw-1)/2) / (vw - 1));
    in.mi.dy    = (i32)(((y - vy)*65535 + (vh-1)/2) / (vh - 1));
    in.mi.flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                  MOUSEEVENTF_VIRTUALDESK;
    if (!sendinput(&in, 1)) {
        return 0;
    }
    Point p = {};
    if (GetCursorPos(&p) && (p.x!=x || p.y!=y)) {
        SetCursorPos(x, y);  // correct rounding in the normalized coordinates
    }
    return 1;
}

static b32 dk_button(Dk *, i32 button, b32 down)
{
    Input in = {};
    in.type = INPUT_MOUSE;
    if (button) {
        in.mi.flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else {
        in.mi.flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    return sendinput(&in, 1);
}

static b32 dk_wheel(Dk *, i32 notches)
{
    Input in = {};
    in.type     = INPUT_MOUSE;
    in.mi.flags = MOUSEEVENTF_WHEEL;
    in.mi.data  = (u32)(notches * 120);
    return sendinput(&in, 1);
}

static i32 dk_dragstep(Dk *)
{
    i32 x = GetSystemMetrics(SM_CXDRAG);
    i32 y = GetSystemMetrics(SM_CYDRAG);
    return x>y ? x : y;
}

static b32 isextended(u32 vk)
{
    return (vk>=0x21 && vk<=0x28) ||  // page up/down, end, home, arrows
           vk==0x2D || vk==0x2E   ||  // insert, delete
           vk==0x5B || vk==0x5C   ||  // windows keys
           vk==0x5D || vk==0x6F   ||  // apps, numpad divide
           vk==0x90 || vk==0x2C;      // num lock, print screen
}

static Input keyinput(u32 vk, b32 up)
{
    Input in = {};
    in.type     = INPUT_KEYBOARD;
    in.ki.vk    = (u16)vk;
    in.ki.scan  = (u16)MapVirtualKeyW(vk, 0);
    in.ki.flags = (up ? KEYEVENTF_KEYUP : 0) |
                  (isextended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    return in;
}

static i32 dk_keycode(Dk *, Str k, b32 *shift)
{
    static struct { Str name; i32 vk; } const names[] = {
        {"ctrl",  0x11}, {"control", 0x11}, {"alt",    0x12},
        {"shift", 0x10}, {"win",     0x5B}, {"super",  0x5B},
        {"cmd",   0x11}, {"command", 0x11}, {"meta",   0x5B},
        {"enter", 0x0D}, {"return",  0x0D}, {"tab",    0x09},
        {"esc",   0x1B}, {"escape",  0x1B}, {"space",  0x20},
        {"backspace", 0x08}, {"bs",  0x08},
        {"del",   0x2E}, {"delete",  0x2E}, {"ins",    0x2D},
        {"insert",0x2D}, {"home",    0x24}, {"end",    0x23},
        {"pgup",  0x21}, {"pageup",  0x21}, {"pgdn",   0x22},
        {"pagedown", 0x22},
        {"left",  0x25}, {"up",      0x26}, {"right",  0x27},
        {"down",  0x28}, {"apps",    0x5D}, {"menu",   0x5D},
        {"pause", 0x13}, {"prtsc",   0x2C}, {"plus",   0xBB},
        {"minus", 0xBD}, {"comma",   0xBC},
    };
    *shift = 0;
    for (iz i = 0; i < countof(names); i++) {
        if (equalfold(k, names[i].name)) {
            return names[i].vk;
        }
    }
    if (k.len>=2 && k.len<=3 && lowercase(k[0])=='f') {
        Int n = parseint(cuthead(k, 1));
        if (n.ok && n.val>=1 && n.val<=24) {
            return 0x70 + (i32)n.val - 1;
        }
    }
    if (k.len == 1) {
        u8 ch = k[0];
        if (ch>='a' && ch<='z') {
            return ch - 'a' + 'A';
        } else if ((ch>='A' && ch<='Z') || digit(ch)) {
            return ch;
        } else if (ch>' ' && ch<0x7f) {
            i32 v = VkKeyScanW(ch);
            if (v == -1) {
                return -1;
            }
            *shift = (v & 0x100) != 0;
            return v & 0xff;
        }
    }
    return -1;
}

static b32 dk_keys(Dk *, i32 *codes, i32 n)
{
    Input in[16];
    iz    len = 0;
    for (i32 i = 0; i<n && i<8; i++) {
        in[len++] = keyinput((u32)codes[i], 0);
    }
    for (i32 i = (n<8 ? n : 8)-1; i >= 0; i--) {
        in[len++] = keyinput((u32)codes[i], 1);
    }
    return sendinput(in, len);
}

// Type UTF-16 text as Unicode key events. Newlines and tabs are sent as
// the Enter and Tab keys, which edit controls handle more reliably.
static b32 dk_text(Dk *, Arena scratch, Str s)
{
    Str16 w = towide(&scratch, s);
    Input in[64];
    iz    n = 0;
    for (iz i = 0; i < w.len; i++) {
        c16 u = w.data[i];
        if (u == '\r') {
            if (i+1<w.len && w.data[i+1]=='\n') {
                continue;
            }
            u = '\n';
        }
        if (u=='\n' || u=='\t') {
            u32 vk = u=='\n' ? VK_RETURN : VK_TAB;
            in[n++] = keyinput(vk, 0);
            in[n++] = keyinput(vk, 1);
        } else {
            Input k = {};
            k.type     = INPUT_KEYBOARD;
            k.ki.scan  = u;
            k.ki.flags = KEYEVENTF_UNICODE;
            in[n++] = k;
            k.ki.flags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            in[n++] = k;
        }
        if (n > countof(in)-2) {
            if (!sendinput(in, n)) {
                return 0;
            }
            n = 0;
            Sleep(1);
        }
    }
    return !n || sendinput(in, n);
}


// Windows' lives

static b32 dk_close(Dk *, DkWin *w)
{
    return PostMessageW((uz)w->id, WM_CLOSE, 0, 0);
}

// Arguments are in the visible-frame terms of list and shot, so add
// back any invisible borders.
static b32 dk_move(Dk *, DkWin *w, i32 x, i32 y, i32 cw, i32 ch, b32 size)
{
    Rect o = {};
    GetWindowRect((uz)w->id, &o);
    DkRect f = w->rect;
    x  -= f.left - o.left;
    y  -= f.top  - o.top;
    cw += (o.right - o.left) - (f.right - f.left);
    ch += (o.bottom - o.top) - (f.bottom - f.top);
    u32 flags = SWP_NOZORDER | SWP_NOACTIVATE | (size ? 0 : SWP_NOSIZE);
    return SetWindowPos((uz)w->id, 0, x, y, cw, ch, flags);
}

// No inherited handles, so that whoever reads this one's output is not
// held up by the program, and outside this one's job when it may, so
// that the program outlives it.
static u32 dk_run(Dk *, Arena scratch, Slice<Str> argv, b32 *attached)
{
    Str16 line = towide(&scratch, quoteargv(&scratch, argv));
    StartupInfo si = {};
    si.cb = sizeof(si);
    ProcessInfo pi = {};
    u32 flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;
    b32 ok = CreateProcessW(0, line.data, 0, 0, 0, flags | CREATE_BREAKAWAY_FROM_JOB,
                            0, 0, &si, &pi);
    *attached = 0;
    if (!ok && GetLastError()==ERROR_ACCESS_DENIED) {
        ok = CreateProcessW(0, line.data, 0, 0, 0, flags, 0, 0, &si, &pi);
        *attached = ok;
    }
    if (!ok) {
        return 0;
    }
    CloseHandle(pi.thread);
    CloseHandle(pi.process);
    return pi.pid;
}

static i32 dk_idle(Dk *, u32 pid, i32 ms)
{
    uz h = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION, 0, pid);
    if (!h) {
        return 2;
    }
    u32 r = WaitForInputIdle(h, (u32)(ms>0 ? ms : 0));
    CloseHandle(h);
    return r==WAIT_TIMEOUT ? 1 : r ? 2 : 0;
}

static void dk_sleep(Dk *, i32 ms)
{
    Sleep((u32)ms);
}

static i64 dk_ms(Dk *)
{
    // Wraps after 49 days, which no single command lives to see
    static u32 base;
    static i64 high;
    u32 now = GetTickCount();
    if (now < base) {
        high += (i64)1 << 32;
    }
    base = now;
    return high + now;
}


// Standard I/O

static iz dk_read(Dk *dk, u8 *buf, iz len)
{
    u32 n   = len>(1<<20) ? (1<<20) : (u32)len;
    u32 got = 0;
    if (!ReadFile(dk->std[0], buf, n, &got, 0)) {
        return 0;  // a broken pipe is the end
    }
    return got;
}

static b32 dk_write(Dk *dk, i32 fd, Str s)
{
    return writeall(dk->std[fd], s);
}

static b32 dk_console(Dk *dk, i32 fd)
{
    u32 mode = 0;
    return GetConsoleMode(dk->std[fd], &mode);
}


static void initdpi()
{
    uz    user32 = GetModuleHandleW((c16 *)u"user32.dll");
    void *ctx    = GetProcAddress(user32, (char *)"SetProcessDpiAwarenessContext");
    if (ctx) {
        b32 (__stdcall *set)(iz) = (b32 (__stdcall *)(iz))ctx;
        if (set(-4) || set(-3)) {  // per-monitor v2, else v1
            return;
        }
    }
    void *old = GetProcAddress(user32, (char *)"SetProcessDPIAware");  // Vista
    if (old) {
        ((b32 (__stdcall *)())old)();
    }
}

extern "C" __attribute((force_align_arg_pointer))
void mainCRTStartup()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Dk dk = {};
    for (i32 fd = 0; fd < 3; fd++) {
        dk.std[fd] = GetStdHandle(-10 - fd);
    }
    initdpi();
    uz dwm = LoadLibraryW((c16 *)u"dwmapi.dll");  // Vista
    if (dwm) {
        void *f = GetProcAddress(dwm, (char *)"DwmGetWindowAttribute");
        dk.dwmattr = (i32 (__stdcall *)(uz, u32, void *, u32))f;
    }

    // A 32-bit process has 2 GiB of address space, which XP fragments
    // into pieces of at most 1 GiB
    iz      cap = sizeof(void *)==8 ? (iz)1<<32 : (iz)1<<28;
    Region *r   = reserve(cap);
    if (!r) {
        os_oom();
    }
    Arena a = newarena(r);

    c16 *cmd = GetCommandLineW();
    iz   len = 0;
    for (; cmd[len]; len++) {}
    Slice<Str> args = splitcmdline(&a, fromwide(&a, cmd, len));
    ExitProcess((u32)dkmain(&dk, a, args.data, (i32)args.len));
}
