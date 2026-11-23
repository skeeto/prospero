// Accessibility: Windows' side
//
// As gui_win32.cpp, two halves. The edge's, included by gui_win32.cpp's
// edge, has windows.h and answers the window's messages; the core's,
// included by its other half, answers the edge's questions from the
// tree and the text model (a11y.cpp) through W32AxHooks.
//
// For now a trace: what speech tools send the window, logged (axnote,
// a11y_imgui.cpp) as the macOS adapter logs Dictation's calls, to learn
// which way Win+H's voice typing and Voice Access come: UI Automation's
// WM_GETOBJECT, the input method's messages (IMM32, through TSF's
// compatibility layer for an application that has no text store), and
// keys and characters a program injected, as Voice Access types.
//
// Logged only while the core says so (W32AXLOG_ON: the tree is built and
// something reads the log, as on macOS), and text only from a field the
// model knows, not a password's. Keys and characters from the keyboard
// itself are typing, never logged; those another program sent are, with
// where they came from.
//
// This is free and unencumbered software released into the public domain.

#if PROSPERO_GUIEDGE

#include <imm.h>
#include <new>

// A line being built, cut at its end
struct W32AxLine {
    char buf[1024];
    int  len;
};

static void axlput(W32AxLine *l, char const *s)
{
    for (; *s && l->len<(int)sizeof(l->buf); s++) {
        l->buf[l->len++] = *s;
    }
}

static void axlputn(W32AxLine *l, long long v)
{
    char t[24];
    int  n = 0;
    unsigned long long u = v<0 ? 0-(unsigned long long)v : (unsigned long long)v;
    do {
        t[n++] = (char)('0' + u%10);
        u /= 10;
    } while (u);
    if (v < 0) {
        t[n++] = '-';
    }
    while (n && l->len<(int)sizeof(l->buf)) {
        l->buf[l->len++] = t[--n];
    }
}

static void axlputx(W32AxLine *l, unsigned long long v)
{
    char t[16];
    int  n = 0;
    do {
        t[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    axlput(l, "0x");
    while (n && l->len<(int)sizeof(l->buf)) {
        l->buf[l->len++] = t[--n];
    }
}

// UTF-16 text in quote marks, as UTF-8, at most 600 units of it
static void axlputw(W32AxLine *l, wchar_t const *s, int n)
{
    int cut = n>600 ? 600 : n;
    axlput(l, "\"");
    int room = (int)sizeof(l->buf) - l->len - 1;
    int got  = room>0 && cut>0 ? WideCharToMultiByte(CP_UTF8, 0, s, cut, l->buf+l->len,
                                                     room, 0, 0) : 0;
    l->len += got>0 ? got : 0;
    axlput(l, "\"");
    if (cut < n) {
        axlput(l, "\xe2\x80\xa6+");
        axlputn(l, n-cut);
    }
}

static void axsend(W32Gui *w, W32AxLine *l)
{
    w->hooks.ax.note(w->hooks.gui, l->buf, l->len);
}

// Where the message being handled came from (GetCurrentInputMessageSource,
// Windows 8): a program's injected input says so; posted messages, as
// the checklist's, and older systems say nothing.
enum { W32SRC_UNKNOWN, W32SRC_HARDWARE, W32SRC_INJECTED, W32SRC_SYSTEM, W32SRC_UNAVAILABLE };

static int axsource(char const **name)
{
    struct Src { int device, origin; };  // INPUT_MESSAGE_SOURCE
    typedef BOOL (WINAPI *Fn)(Src *);
    static Fn   fn;
    static int  looked;
    if (!looked) {
        looked = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        fn = u ? (Fn)(void *)GetProcAddress(u, "GetCurrentInputMessageSource") : 0;
    }
    Src s = {};
    if (!fn || !fn(&s)) {
        *name = "unknown";
        return W32SRC_UNKNOWN;
    }
    // IMO_HARDWARE 1, IMO_INJECTED 2, IMO_SYSTEM 4; IMDT_* devices
    static char const *const devices[] = {"", " keyboard", " mouse", "", " touch", "", "", "",
                                          " pen"};
    static char buf[32];
    char const *dev = s.device>=0 && s.device<9 ? devices[s.device] : "";
    char const *org = s.origin==1 ? "hardware" : s.origin==2 ? "injected"
                    : s.origin==4 ? "system"   : "unavailable";
    W32AxLine l = {};
    axlput(&l, org);
    axlput(&l, dev);
    int n = l.len < (int)sizeof(buf)-1 ? l.len : (int)sizeof(buf)-1;
    memcpy(buf, l.buf, (size_t)n);
    buf[n] = 0;
    *name = buf;
    return s.origin==1 ? W32SRC_HARDWARE : s.origin==2 ? W32SRC_INJECTED
         : s.origin==4 ? W32SRC_SYSTEM   : W32SRC_UNAVAILABLE;
}

static char const *axobjid(LONG id)
{
    switch (id) {
    case 0:      return "OBJID_WINDOW";
    case -4:     return "OBJID_CLIENT";
    case -25:    return "UiaRootObjectId";
    case -1:     return "OBJID_SYSMENU";
    case -2:     return "OBJID_TITLEBAR";
    case -3:     return "OBJID_MENU";
    case -8:     return "OBJID_CARET";
    case -9:     return "OBJID_CURSOR";
    case -12:    return "OBJID_QUERYCLASSNAMEIDX";
    case -14:    return "OBJID_NATIVEOM";
    }
    return 0;
}

static char const *aximenotify(WPARAM c)
{
    switch (c) {
    case IMN_CLOSESTATUSWINDOW:    return "CLOSESTATUSWINDOW";
    case IMN_OPENSTATUSWINDOW:     return "OPENSTATUSWINDOW";
    case IMN_CHANGECANDIDATE:      return "CHANGECANDIDATE";
    case IMN_CLOSECANDIDATE:       return "CLOSECANDIDATE";
    case IMN_OPENCANDIDATE:        return "OPENCANDIDATE";
    case IMN_SETCONVERSIONMODE:    return "SETCONVERSIONMODE";
    case IMN_SETSENTENCEMODE:      return "SETSENTENCEMODE";
    case IMN_SETOPENSTATUS:        return "SETOPENSTATUS";
    case IMN_SETCANDIDATEPOS:      return "SETCANDIDATEPOS";
    case IMN_SETCOMPOSITIONFONT:   return "SETCOMPOSITIONFONT";
    case IMN_SETCOMPOSITIONWINDOW: return "SETCOMPOSITIONWINDOW";
    case IMN_SETSTATUSWINDOWPOS:   return "SETSTATUSWINDOWPOS";
    case IMN_GUIDELINE:            return "GUIDELINE";
    case IMN_PRIVATE:              return "PRIVATE";
    }
    return 0;
}

static char const *aximerequest(WPARAM c)
{
    switch (c) {
    case IMR_COMPOSITIONWINDOW:      return "COMPOSITIONWINDOW";
    case IMR_CANDIDATEWINDOW:        return "CANDIDATEWINDOW";
    case IMR_COMPOSITIONFONT:        return "COMPOSITIONFONT";
    case IMR_RECONVERTSTRING:        return "RECONVERTSTRING";
    case IMR_CONFIRMRECONVERTSTRING: return "CONFIRMRECONVERTSTRING";
    case IMR_QUERYCHARPOSITION:      return "QUERYCHARPOSITION";
    case IMR_DOCUMENTFEED:           return "DOCUMENTFEED";
    }
    return 0;
}

// The composition's string of kind (GCS_COMPSTR, GCS_RESULTSTR), as
// read without changing it, or its length alone when text is withheld
static void axlputcomp(W32AxLine *l, HWND hwnd, DWORD kind, int text)
{
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) {
        axlput(l, "(no context)");
        return;
    }
    wchar_t buf[512];
    LONG    n = ImmGetCompositionStringW(imc, kind, buf, sizeof(buf));
    if (n < 0) {
        axlput(l, "(error ");
        axlputn(l, n);
        axlput(l, ")");
    } else if (!text) {
        axlput(l, "(withheld)#");
        axlputn(l, n/2);
    } else {
        axlputw(l, buf, n/2 < 512 ? (int)n/2 : 512);
    }
    if (kind == GCS_COMPSTR) {
        LONG cur = ImmGetCompositionStringW(imc, GCS_CURSORPOS, 0, 0);
        axlput(l, " cursor ");
        axlputn(l, cur);
    }
    ImmReleaseContext(hwnd, imc);
}

static char const *axvkname(WPARAM vk)
{
    switch (vk) {
    case VK_PACKET:     return "VK_PACKET";
    case VK_PROCESSKEY: return "VK_PROCESSKEY";
    case VK_BACK:       return "VK_BACK";
    case VK_DELETE:     return "VK_DELETE";
    case VK_RETURN:     return "VK_RETURN";
    case VK_LEFT:       return "VK_LEFT";
    case VK_RIGHT:      return "VK_RIGHT";
    case VK_UP:         return "VK_UP";
    case VK_DOWN:       return "VK_DOWN";
    case VK_HOME:       return "VK_HOME";
    case VK_END:        return "VK_END";
    case VK_SHIFT:      return "VK_SHIFT";
    case VK_CONTROL:    return "VK_CONTROL";
    case VK_MENU:       return "VK_MENU";
    case VK_LWIN:       return "VK_LWIN";
    case VK_ESCAPE:     return "VK_ESCAPE";
    case VK_TAB:        return "VK_TAB";
    }
    return 0;
}

// Dictation's "new line" (Voice Access's, Win+H's): its words committed
// through the input method, then, a few milliseconds later, an Enter
// that says it came from the keyboard, which would send the prompt. No
// one presses Enter that soon after a commit (an input method's own
// Enter comes before its commit), so an Enter that soon is a new line:
// a newline character for ImGui, the key itself kept from it. Returns
// whether msg was taken.
enum { W32AX_SPEECHENTER_MS = 150 };
static DWORD axcommitat;
static int   axcommitted;
static int   axreconverted;  // a reconversion confirmed, its commit to come

static void axemptycommit(W32Gui *w, HWND hwnd);

static int w32axspeechkey(W32Gui *w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg==WM_IME_COMPOSITION && (lp & GCS_RESULTSTR)) {
        axcommitat  = GetTickCount();
        axcommitted = 1;
        axemptycommit(w, hwnd);
        return 0;
    } else if (msg!=WM_KEYDOWN || wp==VK_PROCESSKEY || wp==VK_PACKET) {
        return 0;
    }
    int soon = axcommitted && GetTickCount()-axcommitat < W32AX_SPEECHENTER_MS;
    axcommitted = 0;
    if (!soon || wp!=VK_RETURN || (lp & (1<<30)) || GetKeyState(VK_SHIFT)<0 ||
            GetKeyState(VK_CONTROL)<0 || GetKeyState(VK_MENU)<0) {
        return 0;
    }
    ImGui_ImplWin32_WndProcHandler(hwnd, WM_CHAR, '\n', 1);
    w->hooks.input(w->hooks.gui, W32IN_TEXT);
    W32AxHooks *h = &w->hooks.ax;
    if (h->logging && (h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        W32AxLine l = {};
        axlput(&l, "Enter just after a commit: a new line, not sent");
        axsend(w, &l);
    }
    return 1;
}

// Reconversion, as an input method edits text already in the field:
// what TSF's compatibility layer (CUAS) asks for to give a text service
// the text around the caret, as Voice Access's "delete that" needs. A
// request names the live field's text, its selection the part to
// convert (IMR_RECONVERTSTRING) or none at the caret (IMR_DOCUMENTFEED);
// a confirmation (IMR_CONFIRMRECONVERTSTRING) selects the part the input
// method settled on, which its commit then replaces. An empty commit
// replaces it with nothing, which ImGui, given no characters, wouldn't:
// that is a Backspace. Returns whether msg was answered, into *out.

static void axlogf(W32Gui *w, char const *a, long long n0, long long n1, char const *b)
{
    W32AxHooks *h = &w->hooks.ax;
    if (!h->logging || !(h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        return;
    }
    W32AxLine l = {};
    axlput(&l, a);
    if (n0 != LLONG_MIN) {
        axlput(&l, " ");
        axlputn(&l, n0);
    }
    if (n1 != LLONG_MIN) {
        axlput(&l, ",");
        axlputn(&l, n1);
    }
    if (b) {
        axlput(&l, " ");
        axlput(&l, b);
    }
    axsend(w, &l);
}

// The live field's key and text, its length: -1 if there is none
static int axlivetext(W32Gui *w, unsigned *key, W32AxText *info, unsigned short **buf)
{
    W32AxHooks *h = &w->hooks.ax;
    *buf = 0;
    if (!h->focus || !h->textget) {
        return -1;
    }
    *key = h->focus(w->hooks.gui);
    if (!*key || *key==W32AX_ROOT) {
        return -1;
    }
    int len = h->textget(w->hooks.gui, *key, info, 0, 0);
    if (!info->live || len<0) {
        return -1;
    }
    *buf = (unsigned short *)HeapAlloc(GetProcessHeap(), 0, ((size_t)len+1)*2);
    if (!*buf) {
        return -1;
    }
    h->textget(w->hooks.gui, *key, info, *buf, len);
    return len;
}

static int w32aximerequest(W32Gui *w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out)
{
    if (msg!=WM_IME_REQUEST ||
            (wp!=IMR_RECONVERTSTRING && wp!=IMR_DOCUMENTFEED && wp!=IMR_CONFIRMRECONVERTSTRING)) {
        return 0;
    }
    unsigned        key;
    W32AxText       info = {};
    unsigned short *text = 0;
    int             len  = axlivetext(w, &key, &info, &text);
    if (len < 0) {
        return 0;  // no field: as before, the default's nothing
    }
    int lo = info.anchor<info.caret ? info.anchor : info.caret;
    int hi = info.anchor<info.caret ? info.caret : info.anchor;
    RECONVERTSTRING *r = (RECONVERTSTRING *)lp;
    DWORD need = (DWORD)(sizeof(RECONVERTSTRING) + ((size_t)len+1)*2);

    if (wp == IMR_CONFIRMRECONVERTSTRING) {
        *out = FALSE;
        if (r && r->dwSize>=sizeof(*r) && r->dwStrLen==(DWORD)len) {
            int a = (int)(r->dwCompStrOffset/2);
            int b = a + (int)r->dwCompStrLen;
            if (a>=0 && a<=b && b<=len && !info.readonly &&
                    w->hooks.ax.textselect(w->hooks.gui, key, a, b)) {
                axreconverted = 1;
                *out = TRUE;
            }
            axlogf(w, "IME confirm reconversion", a, b, *out ? "-> selected" : "-> refused");
        } else {
            axlogf(w, "IME confirm reconversion", LLONG_MIN, LLONG_MIN, "-> refused: not ours");
        }
    } else if (!r) {
        *out = (LRESULT)need;  // the size, asked first
    } else if (r->dwSize < need) {
        *out = 0;
    } else {
        int doc  = wp == IMR_DOCUMENTFEED;
        int a    = doc ? info.caret : lo;
        int b    = doc ? info.caret : hi;
        r->dwVersion         = 0;
        r->dwStrLen          = (DWORD)len;
        r->dwStrOffset       = sizeof(RECONVERTSTRING);
        r->dwCompStrLen      = (DWORD)(b - a);
        r->dwCompStrOffset   = (DWORD)a * 2;
        r->dwTargetStrLen    = (DWORD)(b - a);
        r->dwTargetStrOffset = (DWORD)a * 2;
        memcpy((char *)r + sizeof(RECONVERTSTRING), text, (size_t)len*2);
        ((unsigned short *)((char *)r + sizeof(RECONVERTSTRING)))[len] = 0;
        *out = (LRESULT)need;
    }
    if (wp != IMR_CONFIRMRECONVERTSTRING) {
        axlogf(w, wp==IMR_DOCUMENTFEED ? "IME document feed" : "IME reconvert string",
               lo, hi, r ? (*out ? "-> given" : "-> too small") : "-> size");
    }
    HeapFree(GetProcessHeap(), 0, text);
    return 1;
}

static void axemptycommit(W32Gui *w, HWND hwnd)
{
    int reconverted = axreconverted;
    axreconverted   = 0;
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) {
        return;
    }
    LONG n = ImmGetCompositionStringW(imc, GCS_RESULTSTR, 0, 0);
    ImmReleaseContext(hwnd, imc);
    if (n != 0) {
        return;
    }
    unsigned        key;
    W32AxText       info = {};
    unsigned short *text = 0;
    int             len  = axlivetext(w, &key, &info, &text);
    HeapFree(GetProcessHeap(), 0, text);
    if (len<0 || info.anchor==info.caret || info.readonly) {
        axlogf(w, "IME empty commit", LLONG_MIN, LLONG_MIN, "-> nothing selected");
        return;
    }
    UINT scan = MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC);
    ImGui_ImplWin32_WndProcHandler(hwnd, WM_KEYDOWN, VK_BACK, 1 | (LPARAM)scan<<16);
    ImGui_ImplWin32_WndProcHandler(hwnd, WM_KEYUP, VK_BACK, 1 | (LPARAM)scan<<16 | (LPARAM)3<<30);
    w->hooks.input(w->hooks.gui, W32IN_TEXT);
    axlogf(w, "IME empty commit", info.anchor, info.caret,
           reconverted ? "-> deleted, reconverted" : "-> deleted");
}

// Log msg, if it is one a speech tool sends and the core logs: before
// it is handled, so the text model is still as the message found it.
static void w32axtrace(W32Gui *w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_GETOBJECT:
    case WM_IME_SETCONTEXT: case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION: case WM_IME_NOTIFY: case WM_IME_REQUEST:
    case WM_IME_CHAR: case WM_IME_KEYDOWN: case WM_IME_KEYUP:
    case WM_INPUTLANGCHANGE:
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_UNICHAR:
        break;
    default:
        return;
    }
    W32AxHooks *h  = &w->hooks.ax;
    int         on = h->logging ? h->logging(w->hooks.gui) : 0;
    if (!(on & W32AXLOG_ON)) {
        return;
    }
    int       text = (on & W32AXLOG_TEXT) != 0;
    W32AxLine l    = {};
    switch (msg) {
    case WM_GETOBJECT: {
        LONG        id   = (LONG)lp;
        char const *name = axobjid(id);
        axlput(&l, "WM_GETOBJECT ");
        if (name) {
            axlput(&l, name);
        } else {
            axlputn(&l, id);
        }
        axlput(&l, " flags ");
        axlputx(&l, (DWORD)wp);
    } break;

    case WM_IME_SETCONTEXT:
        axlput(&l, "WM_IME_SETCONTEXT ");
        axlput(&l, wp ? "active" : "inactive");
        axlput(&l, " show ");
        axlputx(&l, (unsigned long long)(DWORD)lp);
        break;
    case WM_IME_STARTCOMPOSITION:
        axlput(&l, "WM_IME_STARTCOMPOSITION");
        break;
    case WM_IME_ENDCOMPOSITION:
        axlput(&l, "WM_IME_ENDCOMPOSITION");
        break;
    case WM_IME_COMPOSITION:
        axlput(&l, "WM_IME_COMPOSITION gcs ");
        axlputx(&l, (unsigned long long)(DWORD)lp);
        if (lp & GCS_RESULTSTR) {
            axlput(&l, " result ");
            axlputcomp(&l, w->hwnd, GCS_RESULTSTR, text);
        }
        if (lp & GCS_COMPSTR) {
            axlput(&l, " comp ");
            axlputcomp(&l, w->hwnd, GCS_COMPSTR, text);
        }
        break;
    case WM_IME_NOTIFY: {
        char const *name = aximenotify(wp);
        axlput(&l, "WM_IME_NOTIFY ");
        if (name) {
            axlput(&l, name);
        } else {
            axlputx(&l, wp);
        }
        if (wp==IMN_OPENCANDIDATE || wp==IMN_CHANGECANDIDATE || wp==IMN_CLOSECANDIDATE) {
            axlput(&l, " lists ");
            axlputx(&l, (unsigned long long)(DWORD)lp);
        }
    } break;
    case WM_IME_REQUEST: {
        char const *name = aximerequest(wp);
        axlput(&l, "WM_IME_REQUEST ");
        if (name) {
            axlput(&l, name);
        } else {
            axlputx(&l, wp);
        }
    } break;
    case WM_IME_CHAR:
        axlput(&l, "WM_IME_CHAR ");
        if (text) {
            wchar_t c = (wchar_t)wp;
            axlputw(&l, &c, 1);
        } else {
            axlput(&l, "(withheld)");
        }
        break;
    case WM_IME_KEYDOWN:
    case WM_IME_KEYUP:
        axlput(&l, msg==WM_IME_KEYDOWN ? "WM_IME_KEYDOWN " : "WM_IME_KEYUP ");
        axlputx(&l, wp);
        break;
    case WM_INPUTLANGCHANGE:
        axlput(&l, "WM_INPUTLANGCHANGE hkl ");
        axlputx(&l, (unsigned long long)(ULONG_PTR)lp);
        break;

    default: {
        // Keys and characters: only those not from the keyboard itself
        char const *from;
        int         src = axsource(&from);
        if (src==W32SRC_HARDWARE && !(msg==WM_KEYDOWN && axvkname(wp))) {
            return;  // typing; but editing keys, as a speech tool's may come so
        }
        if (src==W32SRC_UNKNOWN && !(msg==WM_CHAR && wp>=0xd800 && wp<0xe000) &&
                !(msg==WM_KEYDOWN && (wp==VK_PACKET || wp==VK_PROCESSKEY))) {
            // An old system: as typing, unless it is what only a
            // program or an input method sends
            return;
        }
        if (src==W32SRC_UNAVAILABLE && (msg==WM_CHAR ? wp>=0x20 && wp!=0x7f
                                        : msg==WM_UNICHAR || !axvkname(wp))) {
            // Posted, as the checklist's and an input method's are, and
            // as a speech tool's Enter came (Voice Access's "new line",
            // which sent): the editing keys and control characters, but
            // not text, which may be typing through an input method
            return;
        }
        if (msg==WM_CHAR || msg==WM_UNICHAR) {
            axlput(&l, msg==WM_CHAR ? "WM_CHAR " : "WM_UNICHAR ");
            if (msg==WM_CHAR && (wp<0x20 || wp==0x7f)) {
                axlputx(&l, wp);  // a control character, never raw
            } else if (text && msg==WM_CHAR) {
                wchar_t c = (wchar_t)wp;
                axlputw(&l, &c, 1);
            } else if (text) {
                axlputx(&l, wp);
            } else {
                axlput(&l, "(withheld)");
            }
        } else {
            char const *name = axvkname(wp);
            axlput(&l, msg==WM_KEYDOWN    ? "WM_KEYDOWN "
                    : msg==WM_KEYUP      ? "WM_KEYUP "
                    : msg==WM_SYSKEYDOWN ? "WM_SYSKEYDOWN " : "WM_SYSKEYUP ");
            if (name) {
                axlput(&l, name);
            } else if (text) {
                axlputx(&l, wp);
            } else {
                axlput(&l, "(withheld)");
            }
            axlput(&l, " scan ");
            axlputx(&l, (lp >> 16) & 0x1ff);
        }
        axlput(&l, " from ");
        axlput(&l, from);
    }
    }
    axsend(w, &l);
}


// UI Automation: the tree as providers
//
// UI Automation asks the window for its root with WM_GETOBJECT and
// UiaRootObjectId, then asks the root for the rest: each node shown to
// tools is a fragment, a W32AxProv for its key, made when a client is
// given it and kept in a table by key, so the element a client holds
// stays the same object while its node lasts. Each method asks the
// core about its key as the published tree has it now; a node gone
// answers UIA_E_ELEMENTNOTAVAILABLE, and its provider is disconnected
// and let go when a publish says it went (w32gui_axevents).
//
// The root is the window's client area: the window's own provider
// (UiaHostProviderFromHwnd) gives its name, place and frame, and the
// root its children. The calls come on this thread, as COM's for an
// apartment (ProviderOptions_UseComThreading), dispatched by the loop's
// wait or a modal loop's, between frames; the events are raised at a
// frame's end. uiautomationcore.dll is loaded only when a client first
// asks, so a window no tool looks at never loads it.

#include <uiautomation.h>
#include <msctf.h>
#include <olectl.h>

struct W32AxProv;

struct W32Ax {
    HMODULE     dll;
    LRESULT (WINAPI *ret)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *);
    HRESULT (WINAPI *host)(HWND, IRawElementProviderSimple **);
    HRESULT (WINAPI *raise)(IRawElementProviderSimple *, EVENTID);
    HRESULT (WINAPI *raiseprop)(IRawElementProviderSimple *, PROPERTYID, VARIANT, VARIANT);
    HRESULT (WINAPI *raisetree)(IRawElementProviderSimple *, StructureChangeType, int *, int);
    BOOL    (WINAPI *listening)(void);
    HRESULT (WINAPI *disconnect)(IRawElementProviderSimple *);  // Windows 8
    HRESULT (WINAPI *notsupported)(IUnknown **);
    W32AxProv  *root;
    W32AxProv **tab;     // by key, open addressing, null for empty
    int         exp;     // 1<<exp slots
    int         len;
    int         asked;   // a client asked since the tree started
    int         told;    // the first publish since was told
    int         focused; // the last focus told, a key, or 0
};

static W32Ax w32ax;

static int axloaduia()
{
    if (w32ax.dll) {
        return 1;
    }
    HMODULE d = LoadLibraryExW(L"uiautomationcore.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d) {
        return 0;
    }
    #define AXFN(f, n) *(FARPROC *)&w32ax.f = GetProcAddress(d, n)
    AXFN(ret,        "UiaReturnRawElementProvider");
    AXFN(host,       "UiaHostProviderFromHwnd");
    AXFN(raise,      "UiaRaiseAutomationEvent");
    AXFN(raiseprop,  "UiaRaiseAutomationPropertyChangedEvent");
    AXFN(raisetree,  "UiaRaiseStructureChangedEvent");
    AXFN(listening,  "UiaClientsAreListening");
    AXFN(disconnect, "UiaDisconnectProvider");
    AXFN(notsupported, "UiaGetReservedNotSupportedValue");
    #undef AXFN
    if (!w32ax.ret || !w32ax.host || !w32ax.raise || !w32ax.raiseprop ||
            !w32ax.raisetree || !w32ax.listening) {
        FreeLibrary(d);
        w32ax = {};
        return 0;
    }
    w32ax.dll = d;
    return 1;
}

// UI Automation's control type for a role (AX_*, as a11y.cpp numbers them)
enum {
    W32R_NONE, W32R_APP, W32R_WINDOW, W32R_DIALOG, W32R_POPUP, W32R_MENUBAR,
    W32R_MENU, W32R_MENUITEM, W32R_GROUP, W32R_BUTTON, W32R_CHECKBOX, W32R_RADIO,
    W32R_LINK, W32R_TABLIST, W32R_TAB, W32R_TREEITEM, W32R_LISTITEM, W32R_COMBO,
    W32R_SLIDER, W32R_TEXTFIELD, W32R_TEXTAREA, W32R_TEXT, W32R_IMAGE, W32R_N
};
enum {  // AXS_*, AXA_*
    W32S_FOCUSED = 1<<0, W32S_DISABLED = 1<<1, W32S_CHECKED = 1<<2,
    W32S_EXPANDED = 1<<3, W32S_SELECTED = 1<<4, W32S_OFFSCREEN = 1<<5,
    W32S_READONLY = 1<<6, W32S_PASSWORD = 1<<7, W32S_MODAL = 1<<8,
    W32A_PRESS = 1<<0, W32A_TOGGLE = 1<<1, W32A_EXPAND = 1<<2, W32A_FOCUS = 1<<3,
    W32A_EDIT = 1<<4, W32A_SELECT = 1<<5, W32A_MENU = 1<<6,
};

static long const axcontroltypes[W32R_N] = {
    UIA_CustomControlTypeId,    UIA_PaneControlTypeId,     UIA_PaneControlTypeId,
    UIA_WindowControlTypeId,    UIA_PaneControlTypeId,     UIA_MenuBarControlTypeId,
    UIA_MenuControlTypeId,      UIA_MenuItemControlTypeId, UIA_GroupControlTypeId,
    UIA_ButtonControlTypeId,    UIA_CheckBoxControlTypeId, UIA_RadioButtonControlTypeId,
    UIA_HyperlinkControlTypeId, UIA_TabControlTypeId,      UIA_TabItemControlTypeId,
    UIA_TreeItemControlTypeId,  UIA_ListItemControlTypeId, UIA_ComboBoxControlTypeId,
    UIA_SliderControlTypeId,    UIA_EditControlTypeId,     UIA_EditControlTypeId,
    UIA_TextControlTypeId,      UIA_ImageControlTypeId,
};

static int axnode(unsigned key, W32AxNode *n, W32Gui **wp);

// Which of the patterns a node has
static int axhasinvoke(W32AxNode *n)
{
    return (n->actions & W32A_PRESS) && n->role!=W32R_CHECKBOX;
}
static int axhastoggle(W32AxNode *n)
{
    return n->role==W32R_CHECKBOX || (n->actions & W32A_TOGGLE);
}
static int axhasexpand(W32AxNode *n)
{
    return (n->actions & W32A_EXPAND) != 0;
}
static int axhasselect(W32AxNode *n)
{
    return n->role==W32R_TAB || n->role==W32R_LISTITEM || n->role==W32R_RADIO;
}
// A text field's: without it, Voice Access passes an Edit by, neither
// labeling it nor dictating into it by name
static int axhasvalue(W32AxNode *n)
{
    return n->role==W32R_TEXTFIELD || n->role==W32R_TEXTAREA;
}

// Whether it takes the keyboard, as tools see it: what ImGui can focus,
// and the kinds a native app's always do, as Voice Access gives a
// number to a tab, list item, link or drop-down only if it says so
static int axfocusable(W32AxNode *n)
{
    return (n->actions & (W32A_FOCUS|W32A_EDIT)) || n->role==W32R_TAB ||
           n->role==W32R_LISTITEM || n->role==W32R_LINK || n->role==W32R_COMBO ||
           n->role==W32R_CHECKBOX || n->role==W32R_RADIO;
}

static W32AxProv *axprov(W32Gui *w, unsigned key);
static void axasked(W32AxProv *p, int pattern, int id, VARIANT const *v, int has);
static ITextRangeProvider *axrange(W32AxProv *p, int lo, int hi);
static void axhitlog(W32AxProv *p, double x, double y);
static void axtextlog(W32AxProv *p, char const *what, int lo, int hi, char const *arg, long long a,
                      long long b, long long got);
static SAFEARRAY *axranges(ITextRangeProvider *r);

struct W32AxProv : IRawElementProviderSimple, IRawElementProviderFragment,
                   IRawElementProviderFragmentRoot, IInvokeProvider, IToggleProvider,
                   IExpandCollapseProvider, ISelectionItemProvider, IValueProvider,
                   ITextProvider2 {
    LONG     refs;
    unsigned key;
    int      dead;
    W32Gui  *w;

    // IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) ||
                IsEqualIID(id, __uuidof(IRawElementProviderSimple))) {
            *out = static_cast<IRawElementProviderSimple *>(this);
        } else if (IsEqualIID(id, __uuidof(IRawElementProviderFragment))) {
            *out = static_cast<IRawElementProviderFragment *>(this);
        } else if (IsEqualIID(id, __uuidof(IRawElementProviderFragmentRoot)) &&
                   key==W32AX_ROOT) {
            *out = static_cast<IRawElementProviderFragmentRoot *>(this);
        // The patterns, each one always: GetPatternProvider decides
        // which a node has, and hands this object out as IUnknown,
        // which another process's client reaches by asking for the
        // pattern's interface across COM
        } else if (IsEqualIID(id, __uuidof(IInvokeProvider))) {
            *out = static_cast<IInvokeProvider *>(this);
        } else if (IsEqualIID(id, __uuidof(IToggleProvider))) {
            *out = static_cast<IToggleProvider *>(this);
        } else if (IsEqualIID(id, __uuidof(IExpandCollapseProvider))) {
            *out = static_cast<IExpandCollapseProvider *>(this);
        } else if (IsEqualIID(id, __uuidof(ISelectionItemProvider))) {
            *out = static_cast<ISelectionItemProvider *>(this);
        } else if (IsEqualIID(id, __uuidof(IValueProvider))) {
            *out = static_cast<IValueProvider *>(this);
        } else if (IsEqualIID(id, __uuidof(ITextProvider)) ||
                   IsEqualIID(id, __uuidof(ITextProvider2))) {
            *out = static_cast<ITextProvider2 *>(this);
        } else {
            *out = 0;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            HeapFree(GetProcessHeap(), 0, this);
        }
        return (ULONG)r;
    }

    // The node as it is now, 0 if gone
    int now(W32AxNode *n)
    {
        return !dead && w && w->hwnd && axnode(key, n, 0);
    }

    // IRawElementProviderSimple

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions *out) override
    {
        *out = (ProviderOptions)(ProviderOptions_ServerSideProvider |
                                 ProviderOptions_UseComThreading);
        return S_OK;
    }

    // What a client asks, and the answer, logged the first time: how a
    // tool decides what it can click, label or dictate into
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown **out) override
    {
        HRESULT r = pattern(id, out);
        axasked(this, 1, id, 0, r==S_OK && *out);
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT *out) override
    {
        HRESULT r = property(id, out);
        axasked(this, 0, id, r==S_OK ? out : 0, 0);
        return r;
    }

    HRESULT pattern(PATTERNID id, IUnknown **out)
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        IUnknown *p = 0;
        switch (id) {
        case UIA_InvokePatternId:
            p = axhasinvoke(&n) ? static_cast<IInvokeProvider *>(this) : 0;
            break;
        case UIA_TogglePatternId:
            p = axhastoggle(&n) ? static_cast<IToggleProvider *>(this) : 0;
            break;
        case UIA_ExpandCollapsePatternId:
            p = axhasexpand(&n) ? static_cast<IExpandCollapseProvider *>(this) : 0;
            break;
        case UIA_SelectionItemPatternId:
            p = axhasselect(&n) ? static_cast<ISelectionItemProvider *>(this) : 0;
            break;
        case UIA_ValuePatternId:
            p = axhasvalue(&n) ? static_cast<IValueProvider *>(this) : 0;
            break;
        case UIA_TextPatternId:
        case UIA_TextPattern2Id:
            p = axhasvalue(&n) ? static_cast<ITextProvider2 *>(this) : 0;
            break;
        }
        if (p) {
            p->AddRef();
            *out = p;
        }
        return S_OK;
    }

    HRESULT property(PROPERTYID id, VARIANT *out)
    {
        out->vt = VT_EMPTY;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int role = n.role>=0 && n.role<W32R_N ? n.role : 0;
        switch (id) {
        case UIA_ControlTypePropertyId:
            if (key != W32AX_ROOT) {
                out->vt   = VT_I4;
                out->lVal = axcontroltypes[role];
            }
            break;
        case UIA_NamePropertyId:
            if (key != W32AX_ROOT) {
                bstr(out, W32AXSTR_NAME);
            }
            break;
        case UIA_HelpTextPropertyId:
            bstr(out, W32AXSTR_HELP);
            break;
        case UIA_AutomationIdPropertyId:
            if (key != W32AX_ROOT) {
                wchar_t id[16];
                int     len = 0;
                for (int s = 28; s >= 0; s -= 4) {
                    id[len++] = L"0123456789abcdef"[(key >> s) & 15];
                }
                out->vt      = VT_BSTR;
                out->bstrVal = SysAllocStringLen(id, (UINT)len);
            }
            break;
        case UIA_IsKeyboardFocusablePropertyId:
            out->vt      = VT_BOOL;
            out->boolVal = axfocusable(&n) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_HasKeyboardFocusPropertyId:
            out->vt      = VT_BOOL;
            out->boolVal = n.state & W32S_FOCUSED ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsEnabledPropertyId:
            out->vt      = VT_BOOL;
            out->boolVal = n.state & W32S_DISABLED ? VARIANT_FALSE : VARIANT_TRUE;
            break;
        case UIA_IsOffscreenPropertyId:
            out->vt      = VT_BOOL;
            out->boolVal = n.state & W32S_OFFSCREEN ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsPasswordPropertyId:
            out->vt      = VT_BOOL;
            out->boolVal = n.state & W32S_PASSWORD ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsDialogPropertyId:
            if (role == W32R_DIALOG) {
                out->vt      = VT_BOOL;
                out->boolVal = VARIANT_TRUE;
            }
            break;
        }
        return S_OK;
    }

    // A string of the node's (W32AXSTR_*), or nothing if empty
    void bstr(VARIANT *out, int which)
    {
        unsigned short buf[256];
        int len = w->hooks.ax.text(w->hooks.gui, key, which, buf, 256);
        if (len <= 0) {
            return;
        }
        BSTR s = SysAllocStringLen(0, (UINT)len);
        if (!s) {
            return;
        }
        if (len > 256) {
            w->hooks.ax.text(w->hooks.gui, key, which, (unsigned short *)s, len);
        } else {
            memcpy(s, buf, (size_t)len*2);
        }
        out->vt      = VT_BSTR;
        out->bstrVal = s;
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple **out) override
    {
        *out = 0;
        if (key==W32AX_ROOT && !dead && w->hwnd) {
            return w32ax.host(w->hwnd, out);
        }
        return S_OK;
    }

    // IRawElementProviderFragment

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        W32AxHooks *h   = &w->hooks.ax;
        unsigned    got = 0;
        if (d == NavigateDirection_Parent) {
            got = key==W32AX_ROOT ? 0 : h->parent(w->hooks.gui, key);
        } else if (d==NavigateDirection_FirstChild || d==NavigateDirection_LastChild) {
            got = kid(key, d==NavigateDirection_FirstChild ? 0 : -1);
        } else if (key != W32AX_ROOT) {
            unsigned up = h->parent(w->hooks.gui, key);
            got = up ? sibling(up, d==NavigateDirection_NextSibling ? 1 : -1) : 0;
        }
        W32AxProv *p = got ? axprov(w, got) : 0;
        if (p) {
            p->AddRef();
            *out = static_cast<IRawElementProviderFragment *>(p);
        }
        return S_OK;
    }

    // Of parent's children as tools see them, the i-th, from the end
    // if negative
    unsigned kid(unsigned parent, int i)
    {
        enum { CAP = 4096 };
        static unsigned kids[CAP];
        int n = w->hooks.ax.kids(w->hooks.gui, parent, kids, CAP);
        n = n<CAP ? n : CAP;
        i = i<0 ? n+i : i;
        return i>=0 && i<n ? kids[i] : 0;
    }

    // This one's sibling under parent, by
    unsigned sibling(unsigned parent, int by)
    {
        enum { CAP = 4096 };
        static unsigned kids[CAP];
        int n = w->hooks.ax.kids(w->hooks.gui, parent, kids, CAP);
        n = n<CAP ? n : CAP;
        for (int i = 0; i < n; i++) {
            if (kids[i] == key) {
                return i+by>=0 && i+by<n ? kids[i+by] : 0;
            }
        }
        return 0;
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY **out) override
    {
        *out = 0;
        if (key == W32AX_ROOT) {
            return S_OK;  // the host's
        }
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        SAFEARRAY *a = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!a) {
            return E_OUTOFMEMORY;
        }
        LONG i = 0, v = UiaAppendRuntimeId;
        SafeArrayPutElement(a, &i, &v);
        i = 1;
        v = (LONG)key;
        SafeArrayPutElement(a, &i, &v);
        *out = a;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect *out) override
    {
        *out = {};
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        if (key==W32AX_ROOT || (n.state & W32S_OFFSCREEN)) {
            return S_OK;  // the host's, or none
        }
        POINT o = {0, 0};
        ClientToScreen(w->hwnd, &o);
        out->left   = o.x + n.x0;
        out->top    = o.y + n.y0;
        out->width  = n.x1>n.x0 ? n.x1-n.x0 : 0;
        out->height = n.y1>n.y0 ? n.y1-n.y0 : 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY **out) override
    {
        *out = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        if (n.actions & W32A_FOCUS) {
            w->hooks.ax.act(w->hooks.gui, key, W32AXACT_FOCUS);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot **out) override
    {
        *out = 0;
        W32AxProv *r = dead ? 0 : axprov(w, W32AX_ROOT);
        if (!r) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        r->AddRef();
        *out = static_cast<IRawElementProviderFragmentRoot *>(r);
        return S_OK;
    }

    // IRawElementProviderFragmentRoot

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                       IRawElementProviderFragment **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        POINT o = {0, 0};
        ClientToScreen(w->hwnd, &o);
        unsigned   k = w->hooks.ax.hit(w->hooks.gui, (float)(x - o.x), (float)(y - o.y));
        W32AxProv *p = axprov(w, k ? k : W32AX_ROOT);
        if (p) {
            p->AddRef();
            *out = static_cast<IRawElementProviderFragment *>(p);
            axhitlog(p, x, y);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        unsigned   k = w->hooks.ax.focus(w->hooks.gui);
        W32AxProv *p = k && k!=W32AX_ROOT ? axprov(w, k) : 0;
        if (p) {
            p->AddRef();
            *out = static_cast<IRawElementProviderFragment *>(p);
        }
        return S_OK;
    }

    // The patterns

    HRESULT act(int kind)
    {
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        return w->hooks.ax.act(w->hooks.gui, key, kind) ? S_OK : UIA_E_ELEMENTNOTENABLED;
    }

    HRESULT STDMETHODCALLTYPE Invoke() override { return act(W32AXACT_PRESS); }

    // ITextProvider2: a text field's text, as ranges of it (W32AxRange)
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        W32AxText info;
        w->hooks.ax.textget(w->hooks.gui, key, &info, 0, 0);
        if (!info.live) {
            *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        int lo = info.anchor<info.caret ? info.anchor : info.caret;
        int hi = info.anchor<info.caret ? info.caret : info.anchor;
        axtextlog(this, "GetSelection", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        *out = axranges(axrange(this, lo, hi));
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY **out) override
    {
        ITextRangeProvider *r = 0;
        axtextlog(this, "GetVisibleRanges", 0, 0, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        HRESULT e = get_DocumentRange(&r);
        *out = e==S_OK ? axranges(r) : 0;
        return e!=S_OK ? e : *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple *, ITextRangeProvider **out) override
    {
        *out = 0;
        return E_INVALIDARG;  // a field has no children
    }
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint pt, ITextRangeProvider **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        POINT o = {0, 0};
        ClientToScreen(w->hwnd, &o);
        int at = w->hooks.ax.textat(w->hooks.gui, key, (float)(pt.x - o.x), (float)(pt.y - o.y));
        at = at>0 ? at : 0;
        axtextlog(this, "RangeFromPoint", at, at, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        *out = axrange(this, at, at);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider **out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        W32AxText info;
        int len = w->hooks.ax.textget(w->hooks.gui, key, &info, 0, 0);
        axtextlog(this, "DocumentRange", 0, len, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        *out = axrange(this, 0, len);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection *out) override
    {
        *out = SupportedTextSelection_Single;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RangeFromAnnotation(IRawElementProviderSimple *, ITextRangeProvider **out) override
    {
        *out = 0;
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE GetCaretRange(BOOL *active, ITextRangeProvider **out) override
    {
        *out    = 0;
        *active = FALSE;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        W32AxText info;
        w->hooks.ax.textget(w->hooks.gui, key, &info, 0, 0);
        int at  = info.live ? info.caret : 0;
        *active = info.live && GetForegroundWindow()==w->hwnd;
        axtextlog(this, "GetCaretRange", at, at, *active ? "active" : "inactive", LLONG_MIN, LLONG_MIN, LLONG_MIN);
        *out    = axrange(this, at, at);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    // IValueProvider: a text field's text, set only while it has the
    // keyboard, as the core's text model is of that field alone
    HRESULT STDMETHODCALLTYPE get_Value(BSTR *out) override
    {
        *out = 0;
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        VARIANT v = {};
        bstr(&v, W32AXSTR_VALUE);
        *out = v.vt==VT_BSTR ? v.bstrVal : SysAllocStringLen(L"", 0);
        axtextlog(this, "Value.get_Value", 0, 0, 0, LLONG_MIN, LLONG_MIN, *out ? (long long)SysStringLen(*out) : -1);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL *out) override
    {
        W32AxNode n;
        *out = TRUE;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        *out = !(n.actions & W32A_EDIT) || (n.state & (W32S_READONLY|W32S_DISABLED));
        axtextlog(this, "Value.IsReadOnly", 0, 0, 0, LLONG_MIN, LLONG_MIN, *out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR text) override
    {
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int len = text ? lstrlenW(text) : 0;
        return w->hooks.ax.setvalue(w->hooks.gui, key, (unsigned short const *)text, len)
               ? S_OK : UIA_E_ELEMENTNOTENABLED;
    }
    HRESULT STDMETHODCALLTYPE Toggle() override { return act(W32AXACT_PRESS); }

    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState *out) override
    {
        W32AxNode n;
        *out = ToggleState_Off;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        *out = n.state & W32S_CHECKED ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // Expand or collapse: a press, unless it is that already
    HRESULT expand(int open)
    {
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        if (!(n.state & W32S_EXPANDED) == !open) {
            return S_OK;
        }
        return act(W32AXACT_PRESS);
    }
    HRESULT STDMETHODCALLTYPE Expand() override   { return expand(1); }
    HRESULT STDMETHODCALLTYPE Collapse() override { return expand(0); }

    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState *out) override
    {
        W32AxNode n;
        *out = ExpandCollapseState_LeafNode;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        *out = n.state & W32S_EXPANDED ? ExpandCollapseState_Expanded
                                       : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    // Selecting is pressing, unless it is selected already
    HRESULT STDMETHODCALLTYPE Select() override
    {
        W32AxNode n;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        return n.state & W32S_SELECTED ? S_OK : act(W32AXACT_PRESS);
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {
        return UIA_E_INVALIDOPERATION;
    }

    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL *out) override
    {
        W32AxNode n;
        *out = FALSE;
        if (!now(&n)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        *out = (n.state & W32S_SELECTED) != 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple **out) override
    {
        *out = 0;
        return S_OK;
    }
};

static W32Gui *axw;  // the window whose tree it is, while it has one

static int axnode(unsigned key, W32AxNode *n, W32Gui **wp)
{
    W32Gui *w = axw;
    if (wp) {
        *wp = w;
    }
    return w && w->hooks.ax.node(w->hooks.gui, key, n);
}

static unsigned axslot(unsigned key, int exp)
{
    unsigned long long h = (unsigned long long)key * 1111111111111111111ull;
    return (unsigned)(h >> (64 - exp));
}

static int axfind(unsigned key)
{
    if (!w32ax.tab) {
        return -1;
    }
    unsigned mask = (1u << w32ax.exp) - 1;
    for (unsigned i = axslot(key, w32ax.exp);; i = (i + 1) & mask) {
        W32AxProv *p = w32ax.tab[i];
        if (!p) {
            return -1;
        } else if (p->key == key) {
            return (int)i;
        }
    }
}

static int axinsert(W32AxProv *p)
{
    if (!w32ax.tab || 2*(w32ax.len+1) > (1<<w32ax.exp)) {
        int         exp = w32ax.tab ? w32ax.exp+1 : 10;
        W32AxProv **t   = (W32AxProv **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                  sizeof(*t) << exp);
        if (!t) {
            return 0;
        }
        unsigned mask = (1u << exp) - 1;
        for (int i = 0; w32ax.tab && i < (1<<w32ax.exp); i++) {
            W32AxProv *q = w32ax.tab[i];
            if (q) {
                unsigned j = axslot(q->key, exp);
                while (t[j]) {
                    j = (j + 1) & mask;
                }
                t[j] = q;
            }
        }
        if (w32ax.tab) {
            HeapFree(GetProcessHeap(), 0, w32ax.tab);
        }
        w32ax.tab = t;
        w32ax.exp = exp;
    }
    unsigned mask = (1u << w32ax.exp) - 1;
    unsigned i    = axslot(p->key, w32ax.exp);
    while (w32ax.tab[i]) {
        i = (i + 1) & mask;
    }
    w32ax.tab[i] = p;
    w32ax.len++;
    return 1;
}

// Take slot i out, moving those after it back (backward shift)
static void axremove(int i)
{
    unsigned mask = (1u << w32ax.exp) - 1;
    unsigned at   = (unsigned)i;
    w32ax.tab[at] = 0;
    w32ax.len--;
    for (unsigned j = (at + 1) & mask; w32ax.tab[j]; j = (j + 1) & mask) {
        unsigned home = axslot(w32ax.tab[j]->key, w32ax.exp);
        // j's entry may move to the hole if its home is not in (at, j]
        if (((j - home) & mask) >= ((j - at) & mask)) {
            w32ax.tab[at] = w32ax.tab[j];
            w32ax.tab[j]  = 0;
            at = j;
        }
    }
}

// The provider for key, made if its node exists: the table's reference,
// which the caller adds to before giving it away
static W32AxProv *axprov(W32Gui *w, unsigned key)
{
    int i = axfind(key);
    if (i >= 0) {
        return w32ax.tab[i];
    }
    W32AxNode n;
    if (!w->hwnd || !w->hooks.ax.node(w->hooks.gui, key, &n)) {
        return 0;
    }
    void *mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(W32AxProv));
    if (!mem) {
        return 0;
    }
    W32AxProv *p = new (mem) W32AxProv;
    p->refs = 1;
    p->key  = key;
    p->w    = w;
    if (!axinsert(p)) {
        p->Release();
        return 0;
    }
    if (key == W32AX_ROOT) {
        w32ax.root = p;
    }
    return p;
}

// Let one go: dead to clients that hold it, and disconnected
static void axdrop(int i)
{
    W32AxProv *p = w32ax.tab[i];
    axremove(i);
    p->dead = 1;
    if (p == w32ax.root) {
        w32ax.root = 0;
    }
    if (w32ax.disconnect) {
        w32ax.disconnect(static_cast<IRawElementProviderSimple *>(p));
    }
    p->Release();
}

// The TSF store, below
struct W32Tsf;
static void tsfstart(W32Gui *w);
static void tsfgone(W32Gui *w);
static void tsfsync(W32Tsf *t, int published);
static struct {
    ITfThreadMgr   *tm;
    TfClientId      cid;
    ITfDocumentMgr *dm, *prev;  // ours, and the window's before
    ITfContext     *ctx;
    TfEditCookie    ec;
    W32Tsf         *store;
    DWORD           editcookie;  // the checklist's edit sink, advised
    int             edits;       // ends of edits it was told of
} w32tsf;

// The window goes: every provider with it, and the TSF store
static void w32axgone(W32Gui *w)
{
    if (!w32ax.dll || axw!=w) {
        return;
    }
    tsfgone(w);
    if (w->hwnd) {
        w32ax.ret(w->hwnd, 0, 0, 0);
    }
    for (int i = 0; w32ax.tab && i < (1<<w32ax.exp);) {
        if (w32ax.tab[i]) {
            axdrop(i);  // shifts another into i, perhaps
        } else {
            i++;
        }
    }
    axw = 0;
    w32ax.asked = w32ax.told = w32ax.focused = 0;
}

// WM_GETOBJECT for UI Automation: the root, the tree started. 0 if
// accessibility is off, or UI Automation can't load, for DefWindowProc.
static int w32axroot(W32Gui *w, WPARAM wp, LPARAM lp, LRESULT *out)
{
    if ((LONG)lp!=UiaRootObjectId || !w->hooks.ax.start) {
        return 0;
    }
    if (!w->hooks.ax.start(w->hooks.gui) || !axloaduia()) {
        return 0;
    }
    axw = w;
    w32ax.asked = 1;
    W32AxProv *r = axprov(w, W32AX_ROOT);
    if (!r) {
        return 0;
    }
    tsfstart(w);
    *out = w32ax.ret(w->hwnd, wp, lp, static_cast<IRawElementProviderSimple *>(r));
    return 1;
}

// A text field's text, loaded for a range's call: its units, and its
// line starts (the live field's wrapped lines, else after newlines)
struct W32AxDoc {
    unsigned short *s;
    int             len;
    W32AxText       info;
    int            *lines;
    int             nlines;
};

static void axdocfree(W32AxDoc *d)
{
    HeapFree(GetProcessHeap(), 0, d->s);
    HeapFree(GetProcessHeap(), 0, d->lines);
    *d = {};
}

static int axdocload(W32Gui *w, unsigned key, W32AxDoc *d)
{
    *d = {};
    W32AxHooks *h = &w->hooks.ax;
    int len = h->textget(w->hooks.gui, key, &d->info, 0, 0);
    d->s = (unsigned short *)HeapAlloc(GetProcessHeap(), 0, (size_t)len*2 + 2);
    if (!d->s) {
        return 0;
    }
    d->len = h->textget(w->hooks.gui, key, &d->info, d->s, len);
    d->len = d->len<len ? d->len : len;
    int n  = h->textlines(w->hooks.gui, key, 0, 0);
    if (n <= 0) {
        n = 1;
        for (int i = 0; i < d->len; i++) {
            n += d->s[i] == '\n';
        }
    }
    d->lines = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)n*sizeof(int));
    if (!d->lines) {
        axdocfree(d);
        return 0;
    }
    if (d->info.live && h->textlines(w->hooks.gui, key, d->lines, n) == n) {
        d->nlines = n;
    } else {
        d->nlines = 0;
        d->lines[d->nlines++] = 0;
        for (int i = 0; i<d->len && d->nlines<n; i++) {
            if (d->s[i] == '\n') {
                d->lines[d->nlines++] = i + 1;
            }
        }
    }
    return 1;
}

static int axwclass(unsigned c)
{
    if (c==' ' || c=='\t' || c=='\n' || c=='\r' || c==0xa0 || c==0x3000) {
        return 1;
    }
    int alnum = (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z');
    return alnum || c=='_' || c>=0x80 ? 2 : 3;
}

// Whether a unit (TextUnit_*) starts at i: words take their spaces after
// them, as Windows' own fields count them; a line is drawn as one
static int axisbound(W32AxDoc *d, int i, int unit)
{
    if (i<=0 || i>=d->len) {
        return 1;
    }
    unsigned c = d->s[i], p = d->s[i-1];
    if (c>=0xdc00 && c<0xe000 && p>=0xd800 && p<0xdc00) {
        return 0;  // within a surrogate pair
    }
    switch (unit) {
    case TextUnit_Character:
        return 1;
    case TextUnit_Word: {
        int k = axwclass(c), q = axwclass(p);
        return p=='\n' || (k!=1 && k!=q);
    }
    case TextUnit_Line: {
        int lo = 0, hi = d->nlines;
        while (lo < hi) {
            int mid = lo + (hi - lo)/2;
            if (d->lines[mid] < i) lo = mid + 1; else hi = mid;
        }
        return lo<d->nlines && d->lines[lo]==i;
    }
    case TextUnit_Paragraph:
        return p == '\n';
    }
    return 0;  // Format, Page, Document: the whole
}

static int axnextbound(W32AxDoc *d, int i, int unit)
{
    for (i++; i < d->len && !axisbound(d, i, unit); i++) {}
    return i<d->len ? i : d->len;
}

static int axprevbound(W32AxDoc *d, int i, int unit)
{
    for (i--; i > 0 && !axisbound(d, i, unit); i--) {}
    return i>0 ? i : 0;
}

// A Text pattern call, logged with the range after it and what it
// gave: how a tool finds "that" in "delete that". Text itself is not.
static void axtextlog(W32AxProv *p, char const *what, int lo, int hi, char const *arg, long long a,
                      long long b, long long got)
{
    W32Gui     *w = p->w;
    W32AxHooks *h = &w->hooks.ax;
    if (!h->logging || !(h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        return;
    }
    W32AxLine l = {};
    axlput(&l, "text ");
    axlput(&l, what);
    if (arg) {
        axlput(&l, " ");
        axlput(&l, arg);
    }
    if (a != LLONG_MIN) {
        axlput(&l, " ");
        axlputn(&l, a);
    }
    if (b != LLONG_MIN) {
        axlput(&l, " ");
        axlputn(&l, b);
    }
    axlput(&l, " -> [");
    axlputn(&l, lo);
    axlput(&l, ",");
    axlputn(&l, hi);
    axlput(&l, ")");
    if (got != LLONG_MIN) {
        axlput(&l, " ");
        axlputn(&l, got);
    }
    axsend(w, &l);
}

static char const *axunitname(int u)
{
    static char const *const names[] = {"char", "format", "word", "line", "para", "page", "doc"};
    return u>=0 && u<7 ? names[u] : "?";
}

static char const *axepname(int ep)
{
    return ep==TextPatternRangeEndpoint_Start ? "start" : "end";
}

// A range of a text field's units, as the Text pattern hands them out:
// clamped to its text as it is at each call, which edits change
struct W32AxRange : ITextRangeProvider {
    LONG       refs;
    W32AxProv *p;
    int        lo, hi;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(ITextRangeProvider))) {
            *out = static_cast<ITextRangeProvider *>(this);
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            p->Release();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return (ULONG)r;
    }

    // Its field's text now, the range clamped to it: 0 if the field is gone
    int load(W32AxDoc *d)
    {
        W32AxNode n;
        if (!p->now(&n) || !axdocload(p->w, p->key, d)) {
            return 0;
        }
        lo = lo<0 ? 0 : lo>d->len ? d->len : lo;
        hi = hi<lo ? lo : hi>d->len ? d->len : hi;
        return 1;
    }

    static W32AxRange *of(ITextRangeProvider *r)
    {
        return static_cast<W32AxRange *>(r);
    }

    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider **out) override
    {
        *out = axrange(p, lo, hi);
        axtextlog(p, "Clone", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider *r, BOOL *out) override
    {
        W32AxRange *o = r ? of(r) : 0;
        *out = o && o->p->key==p->key && o->lo==lo && o->hi==hi;
        axtextlog(p, "Compare", lo, hi, 0, o ? o->lo : -1, o ? o->hi : -1, *out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint ep, ITextRangeProvider *r,
                                               TextPatternRangeEndpoint rep, int *out) override
    {
        *out = 0;
        if (!r) {
            return E_INVALIDARG;
        }
        W32AxRange *o = of(r);
        int a = ep==TextPatternRangeEndpoint_Start ? lo : hi;
        int b = rep==TextPatternRangeEndpoint_Start ? o->lo : o->hi;
        *out = a<b ? -1 : a>b;
        axtextlog(p, "CompareEndpoints", lo, hi, axepname(ep), b, LLONG_MIN, *out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override
    {
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int start = lo==d.len && d.len ? axprevbound(&d, d.len, unit)  // the last unit
                  : axisbound(&d, lo, unit) ? lo : axprevbound(&d, lo, unit);
        int end   = axnextbound(&d, start, unit);
        if (hi > end) {
            end = axisbound(&d, hi, unit) ? hi : axnextbound(&d, hi, unit);
        }
        lo = start;
        hi = end;
        axdocfree(&d);
        axtextlog(p, "Expand", lo, hi, axunitname(unit), LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, ITextRangeProvider **out) override
    {
        *out = 0;  // one style throughout
        axtextlog(p, "FindAttribute", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindText(BSTR text, BOOL back, BOOL nocase, ITextRangeProvider **out) override
    {
        *out = 0;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int n = text ? (int)SysStringLen(text) : 0;
        int found = -1;
        for (int k = 0; n && k <= hi-lo-n; k++) {
            int i = back ? hi-n-k : lo+k;
            if (CompareStringOrdinal((wchar_t const *)d.s+i, n, text, n, nocase) == CSTR_EQUAL) {
                found = i;
                break;
            }
        }
        axdocfree(&d);
        axtextlog(p, "FindText", lo, hi, back ? "back" : "forward", n, found, LLONG_MIN);
        if (found >= 0) {
            *out = axrange(p, found, found+n);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID id, VARIANT *out) override
    {
        out->vt = VT_EMPTY;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        W32AxNode n = {};
        p->now(&n);
        int ro = d.info.live ? d.info.readonly : (n.state & W32S_READONLY) != 0;
        axdocfree(&d);
        if (id == UIA_IsReadOnlyAttributeId) {
            out->vt      = VT_BOOL;
            out->boolVal = ro ? VARIANT_TRUE : VARIANT_FALSE;
        } else if (id == UIA_IsHiddenAttributeId) {
            out->vt      = VT_BOOL;
            out->boolVal = VARIANT_FALSE;
        } else if (w32ax.notsupported && w32ax.notsupported(&out->punkVal) == S_OK) {
            out->vt = VT_UNKNOWN;
        }
        axtextlog(p, "GetAttributeValue", lo, hi, id==UIA_IsReadOnlyAttributeId ? "IsReadOnly" : 0, id, LLONG_MIN,
                  out->vt==VT_BOOL ? (long long)(out->boolVal != 0) : LLONG_MIN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY **out) override
    {
        *out = 0;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        enum { MAXRECTS = 64 };
        double r[4*MAXRECTS];
        int    nr = 0;
        POINT  o  = {0, 0};
        ClientToScreen(p->w->hwnd, &o);
        W32AxHooks *h = &p->w->hooks.ax;
        for (int k = 0; d.info.live && k<d.nlines && nr<MAXRECTS; k++) {
            int ls = d.lines[k], le = k+1<d.nlines ? d.lines[k+1] : d.len;
            int a  = lo>ls ? lo : ls, b = hi<le ? hi : le;
            if (lo==hi ? !(lo>=ls && (lo<le || k==d.nlines-1)) : a>=b) {
                continue;
            }
            if (b>a && d.s[b-1]=='\n') {
                b--;  // its newline drawn as nothing
            }
            float f[4];
            if (h->textrect(p->w->hooks.gui, p->key, a, b, f)) {
                r[4*nr+0] = o.x + f[0];
                r[4*nr+1] = o.y + f[1];
                r[4*nr+2] = f[2]>f[0] ? f[2]-f[0] : 0;
                r[4*nr+3] = f[3]>f[1] ? f[3]-f[1] : 0;
                nr++;
            }
            if (lo == hi) {
                break;
            }
        }
        W32AxNode n;
        if (!nr && !d.info.live && hi>lo && p->now(&n) && !(n.state & W32S_OFFSCREEN)) {
            r[0] = o.x + n.x0;  // where the field is, for a field not focused
            r[1] = o.y + n.y0;
            r[2] = n.x1>n.x0 ? n.x1-n.x0 : 0;
            r[3] = n.y1>n.y0 ? n.y1-n.y0 : 0;
            nr   = 1;
        }
        axdocfree(&d);
        SAFEARRAY *a = SafeArrayCreateVector(VT_R8, 0, (ULONG)(4*nr));
        if (!a) {
            return E_OUTOFMEMORY;
        }
        for (LONG i = 0; i < 4*nr; i++) {
            SafeArrayPutElement(a, &i, &r[i]);
        }
        *out = a;
        axtextlog(p, "GetBoundingRectangles", lo, hi, 0, LLONG_MIN, LLONG_MIN, nr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple **out) override
    {
        *out = static_cast<IRawElementProviderSimple *>(p);
        p->AddRef();
        axtextlog(p, "GetEnclosingElement", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(int max, BSTR *out) override
    {
        *out = 0;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int n = hi - lo;
        n = max>=0 && max<n ? max : n;
        if (n>0 && lo+n<d.len && d.s[lo+n]>=0xdc00 && d.s[lo+n]<0xe000) {
            n--;  // not half a pair
        }
        *out = SysAllocStringLen((wchar_t const *)d.s+lo, (UINT)(n>0 ? n : 0));
        axdocfree(&d);
        axtextlog(p, "GetText", lo, hi, 0, max, LLONG_MIN, n);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    // An endpoint moved count units: the units moved, as far as the text
    // goes. An endpoint within a unit takes one to reach its edge.
    int step(W32AxDoc *d, int *at, TextUnit unit, int count)
    {
        int moved = 0;
        if (unit==TextUnit_Format || unit==TextUnit_Page || unit==TextUnit_Document) {
            unit = TextUnit_Document;
        }
        for (; count > 0 && *at < d->len; count--, moved++) {
            *at = unit==TextUnit_Document ? d->len : axnextbound(d, *at, unit);
        }
        for (; count < 0 && *at > 0; count++, moved--) {
            *at = unit==TextUnit_Document ? 0 : axprevbound(d, *at, unit);
        }
        return moved;
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count, int *out) override
    {
        *out = 0;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int degenerate = lo == hi;
        if (!degenerate) {
            ExpandToEnclosingUnit(unit);  // whole units, then moved by them
        }
        int at = lo;
        *out = count ? step(&d, &at, unit, count) : 0;
        lo = at;
        hi = at;
        if (!degenerate) {
            hi = unit==TextUnit_Document ? d.len : axnextbound(&d, lo, unit);
            hi = lo==d.len ? d.len : hi;
        }
        axdocfree(&d);
        axtextlog(p, "Move", lo, hi, axunitname(unit), count, LLONG_MIN, *out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint ep, TextUnit unit, int count,
                                                 int *out) override
    {
        *out = 0;
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int at = ep==TextPatternRangeEndpoint_Start ? lo : hi;
        *out = step(&d, &at, unit, count);
        setend(ep, at);
        axdocfree(&d);
        axtextlog(p, ep==TextPatternRangeEndpoint_Start ? "MoveStartBy" : "MoveEndBy", lo, hi,
                  axunitname(unit), count, LLONG_MIN, *out);
        return S_OK;
    }
    // One endpoint set: past the other, the other comes along
    void setend(TextPatternRangeEndpoint ep, int at)
    {
        if (ep == TextPatternRangeEndpoint_Start) {
            lo = at;
            hi = hi<lo ? lo : hi;
        } else {
            hi = at;
            lo = lo>hi ? hi : lo;
        }
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint ep, ITextRangeProvider *r,
                                                  TextPatternRangeEndpoint rep) override
    {
        if (!r) {
            return E_INVALIDARG;
        }
        W32AxRange *o = of(r);
        setend(ep, rep==TextPatternRangeEndpoint_Start ? o->lo : o->hi);
        axtextlog(p, ep==TextPatternRangeEndpoint_Start ? "SetStartTo" : "SetEndTo", lo, hi,
                  axepname(rep), LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Select() override
    {
        W32AxDoc d;
        if (!load(&d)) {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int live = d.info.live;
        axdocfree(&d);
        if (!live) {
            axtextlog(p, "Select", lo, hi, "refused: no keyboard", LLONG_MIN, LLONG_MIN, LLONG_MIN);
            return UIA_E_INVALIDOPERATION;  // a field without the keyboard
        }
        W32AxHooks *h = &p->w->hooks.ax;
        return h->textselect(p->w->hooks.gui, p->key, lo, hi) ? S_OK : UIA_E_INVALIDOPERATION;  // logged
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override
    {
        axtextlog(p, "AddToSelection", lo, hi, "refused", LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return UIA_E_INVALIDOPERATION;  // one selection
    }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {
        axtextlog(p, "RemoveFromSelection", lo, hi, "refused", LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return UIA_E_INVALIDOPERATION;
    }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) override
    {
        axtextlog(p, "ScrollIntoView", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        return S_OK;  // ImGui keeps the caret in view
    }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY **out) override
    {
        axtextlog(p, "GetChildren", lo, hi, 0, LLONG_MIN, LLONG_MIN, LLONG_MIN);
        *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
};

static ITextRangeProvider *axrange(W32AxProv *p, int lo, int hi)
{
    void *m = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(W32AxRange));
    if (!m) {
        return 0;
    }
    W32AxRange *r = new (m) W32AxRange;
    r->refs = 1;
    r->p    = p;
    r->lo   = lo;
    r->hi   = hi>lo ? hi : lo;
    p->AddRef();
    return r;
}

// One range as an array of them, taking its reference
static SAFEARRAY *axranges(ITextRangeProvider *r)
{
    if (!r) {
        return 0;
    }
    SAFEARRAY *a = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
    if (a) {
        LONG i = 0;
        SafeArrayPutElement(a, &i, r);  // its own reference
    }
    r->Release();
    return a;
}

static void axraiseprop(W32AxProv *p, PROPERTYID id, VARIANT now)
{
    VARIANT old = {};
    old.vt = VT_EMPTY;
    w32ax.raiseprop(static_cast<IRawElementProviderSimple *>(p), id, old, now);
}

static void axdescribe(W32AxLine *l, W32AxProv *p);

static void axraisebool(W32AxProv *p, PROPERTYID id, int on)
{
    W32Gui     *w = p->w;
    W32AxHooks *h = &w->hooks.ax;
    if (id==UIA_IsEnabledPropertyId && h->logging && (h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        W32AxLine l = {};  // what a tool that asked once is told since
        axlput(&l, "event ");
        axdescribe(&l, p);
        axlput(&l, on ? " IsEnabled true" : " IsEnabled false");
        axsend(w, &l);
    }
    VARIANT v = {};
    v.vt      = VT_BOOL;
    v.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
    axraiseprop(p, id, v);
}

static void axraiseint(W32AxProv *p, PROPERTYID id, int i)
{
    VARIANT v = {};
    v.vt   = VT_I4;
    v.lVal = i;
    axraiseprop(p, id, v);
}

static void axchildren(W32AxProv *p)
{
    if (p == w32ax.root) {
        int id[2] = {UiaAppendRuntimeId, 0};
        w32ax.raisetree(static_cast<IRawElementProviderSimple *>(p),
                        StructureChangeType_ChildrenInvalidated, id, 1);
    } else {
        int id[2] = {UiaAppendRuntimeId, (int)p->key};
        w32ax.raisetree(static_cast<IRawElementProviderSimple *>(p),
                        StructureChangeType_ChildrenInvalidated, id, 2);
    }
}

extern "C" void w32gui_axevents(void *gp, W32AxEvent const *ev, int n)
{
    W32Gui *w = (W32Gui *)gp;
    if (!w || w!=axw || !w32ax.dll || !w->hwnd) {
        return;
    }
    W32AxHooks *h    = &w->hooks.ax;
    int         tell = w32ax.listening() != 0;
    // Only the window the user is in has the focus to tell of
    int         front = GetForegroundWindow() == w->hwnd;
    for (int e = 0; e < n; e++) {
        int i = axfind(ev[e].key);
        if (ev[e].kind == W32AXE_DESTROYED) {
            if (i >= 0) {
                axdrop(i);
            }
            continue;
        } else if (!tell) {
            continue;
        }
        W32AxProv *p = i>=0 ? w32ax.tab[i] : 0;
        W32AxNode  nd;
        switch (ev[e].kind) {
        case W32AXE_FOCUS: {
            unsigned k = h->focus(w->hooks.gui);
            if (front && k && k!=W32AX_ROOT && k!=(unsigned)w32ax.focused) {
                W32AxProv *f = axprov(w, k);
                if (f) {
                    w32ax.raise(static_cast<IRawElementProviderSimple *>(f),
                                UIA_AutomationFocusChangedEventId);
                }
            }
            w32ax.focused = (int)k;
        } break;
        case W32AXE_TEXTEDIT:
        case W32AXE_TEXTSEL:
            if (p) {  // only a field a client has an element of
                w32ax.raise(static_cast<IRawElementProviderSimple *>(p),
                            ev[e].kind==W32AXE_TEXTEDIT ? UIA_Text_TextChangedEventId
                                                        : UIA_Text_TextSelectionChangedEventId);
            }
            break;
        case W32AXE_NAME:
            if (p) {
                VARIANT v = {};
                p->bstr(&v, W32AXSTR_NAME);
                axraiseprop(p, UIA_NamePropertyId, v);
                if (v.vt == VT_BSTR) {
                    SysFreeString(v.bstrVal);
                }
            }
            break;
        case W32AXE_VALUE:
        case W32AXE_STATE:
            if (p && h->node(w->hooks.gui, p->key, &nd)) {
                axraisebool(p, UIA_IsEnabledPropertyId, !(nd.state & W32S_DISABLED));
                if (axhastoggle(&nd)) {
                    axraiseint(p, UIA_ToggleToggleStatePropertyId,
                               nd.state & W32S_CHECKED ? ToggleState_On : ToggleState_Off);
                }
                if (axhasexpand(&nd)) {
                    axraiseint(p, UIA_ExpandCollapseExpandCollapseStatePropertyId,
                               nd.state & W32S_EXPANDED ? ExpandCollapseState_Expanded
                                                        : ExpandCollapseState_Collapsed);
                }
                if (axhasselect(&nd)) {
                    axraisebool(p, UIA_SelectionItemIsSelectedPropertyId,
                                (nd.state & W32S_SELECTED) != 0);
                }
                if (axhasvalue(&nd) && ev[e].kind==W32AXE_VALUE) {
                    VARIANT v = {};
                    p->bstr(&v, W32AXSTR_VALUE);
                    if (v.vt != VT_BSTR) {
                        v.vt      = VT_BSTR;
                        v.bstrVal = SysAllocStringLen(L"", 0);
                    }
                    axraiseprop(p, UIA_ValueValuePropertyId, v);
                    VariantClear(&v);
                }
            }
            break;
        case W32AXE_CHILDREN:
            if (p) {
                axchildren(p);
            }
            break;
        }
    }
    // The first tree since a client asked: the root's children came
    if (w32ax.asked && !w32ax.told && w32ax.root) {
        w32ax.told = 1;
        if (tell) {
            axchildren(w32ax.root);
        }
    }
    tsfsync(w32tsf.store, 1);
}


// TSF: a text store for the live field
//
// Voice Access edits text already in a field ("delete that", "correct
// that") as a text service of TSF's, and only through a document of the
// application's own, a text store; TSF's compatibility layer (CUAS),
// which serves typing and commits for a window without one, gives it
// nothing to edit (decisions.md, 2026-11-23). So while accessibility is
// on, the window has one: a document manager with one context over this
// store, associated with the window (AssociateFocus). Its document is
// the live field's text in UTF-16 units, as the Text pattern has it; with
// no live field, or one not editable, it is empty and read-only, and
// text services leave keys alone.
//
// A text service's write becomes the core's edit (hooks.textedit), which
// the shadow shows at once and ImGui takes at the next frame. A change
// from anywhere else, typing or ImGui's own, is told to TSF after the
// publish that shows it (tsfsync), as the change from the document TSF
// was last told of. Locks are granted at once when free, a queued one
// when the holder lets go; nothing is told to TSF while one is held.

struct W32TsfDoc {
    unsigned        key;        // the live field, 0 for none
    int             readonly;
    int             anchor, caret;
    unsigned short *text;       // HeapAlloc'd, or null when empty
    int             len;
    RECT            at;         // the field, in screen pixels
};

static void tsffree(W32TsfDoc *d)
{
    if (d->text) {
        HeapFree(GetProcessHeap(), 0, d->text);
    }
    *d = {};
}

// The document as it is now
static void tsfload(W32Gui *w, W32TsfDoc *d)
{
    *d = {};
    W32AxText info = {};
    int len = axlivetext(w, &d->key, &info, &d->text);
    if (len<0 || info.readonly) {
        // Nothing a text service may edit, a password's text among it
        if (d->text) {
            HeapFree(GetProcessHeap(), 0, d->text);
        }
        *d = {};
        d->readonly = 1;
        GetClientRect(w->hwnd, &d->at);
        MapWindowPoints(w->hwnd, 0, (POINT *)&d->at, 2);
        return;
    }
    d->len    = len;
    d->anchor = info.anchor;
    d->caret  = info.caret;
    W32AxNode n;
    if (w->hooks.ax.node(w->hooks.gui, d->key, &n)) {
        d->at = {(LONG)n.x0, (LONG)n.y0, (LONG)n.x1, (LONG)n.y1};
        MapWindowPoints(w->hwnd, 0, (POINT *)&d->at, 2);
    }
}

static void tsflog(W32Gui *w, char const *what, long long a, long long b, char const *tail)
{
    axlogf(w, what, a, b, tail);
}

static GUID const tsfthreadmgr =  // CLSID_TF_ThreadMgr
    {0x529a9e6b, 0x6587, 0x4f23, {0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50}};


struct W32Tsf : ITextStoreACP, ITfContextOwnerCompositionSink {
    LONG               refs = 1;
    W32Gui            *w;
    ITextStoreACPSink *sink   = 0;
    IUnknown          *sinkid = 0;  // its identity, to know it again
    DWORD              mask   = 0;
    DWORD              lock   = 0;  // TS_LF_READ or TS_LF_READWRITE, held
    DWORD              queued = 0;  // asked for while held, granted after
    int                composing = 0;
    int                nolayout  = 0;  // answered TS_E_NOLAYOUT: tell later
    int                syncing   = 0;  // telling TSF now: not again within
    W32TsfDoc          told = {};      // the document as TSF knows it

    explicit W32Tsf(W32Gui *gw) : w(gw) {}

    int reading() { return (lock & TS_LF_READ) == TS_LF_READ; }
    int writing() { return (lock & TS_LF_READWRITE) == TS_LF_READWRITE; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(ITextStoreACP))) {
            *out = static_cast<ITextStoreACP *>(this);
        } else if (IsEqualIID(id, __uuidof(ITfContextOwnerCompositionSink))) {
            *out = static_cast<ITfContextOwnerCompositionSink *>(this);
        } else {
            *out = 0;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) {
            tsffree(&told);
            delete this;
        }
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown *punk, DWORD m) override
    {
        if (!punk || !IsEqualIID(id, __uuidof(ITextStoreACPSink))) {
            return E_INVALIDARG;
        }
        IUnknown *who = 0;
        punk->QueryInterface(__uuidof(IUnknown), (void **)&who);
        if (who && who==sinkid) {
            mask = m;
            who->Release();
            return S_OK;
        }
        if (sink) {
            if (who) who->Release();
            return CONNECT_E_ADVISELIMIT;
        }
        if (FAILED(punk->QueryInterface(__uuidof(ITextStoreACPSink), (void **)&sink))) {
            sink = 0;
            if (who) who->Release();
            return E_NOINTERFACE;
        }
        sinkid = who;
        mask   = m;
        tsffree(&told);
        tsfload(w, &told);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown *punk) override
    {
        IUnknown *who = 0;
        if (punk) {
            punk->QueryInterface(__uuidof(IUnknown), (void **)&who);
        }
        int ours = who && who==sinkid;
        if (who) who->Release();
        if (!ours) {
            return CONNECT_E_NOCONNECTION;
        }
        sink->Release();
        sinkid->Release();
        sink   = 0;
        sinkid = 0;
        mask   = 0;
        return S_OK;
    }

    HRESULT grant(DWORD f)
    {
        lock = f;
        HRESULT r = sink->OnLockGranted(f);
        lock = 0;
        return r;
    }
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT *hr) override;

    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS *s) override
    {
        if (!s) {
            return E_INVALIDARG;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        s->dwDynamicFlags = d.readonly ? TS_SD_READONLY : 0;
        s->dwStaticFlags  = TS_SS_NOHIDDENTEXT;
        tsffree(&d);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG lo, LONG hi, ULONG, LONG *a, LONG *b) override
    {
        if (!a || !b) {
            return E_INVALIDARG;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        int len = d.len;
        tsffree(&d);
        if (lo<0 || lo>hi || hi>len) {
            return E_INVALIDARG;
        }
        *a = lo;
        *b = hi;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSelection(ULONG i, ULONG n, TS_SELECTION_ACP *sel,
                                           ULONG *got) override
    {
        if (!sel || !got) {
            return E_INVALIDARG;
        }
        *got = 0;
        if (!reading()) {
            return TS_E_NOLOCK;
        }
        if (i!=0 && i!=(ULONG)TS_DEFAULT_SELECTION) {
            return E_INVALIDARG;
        }
        if (!n) {
            return S_OK;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        int lo = d.anchor<d.caret ? d.anchor : d.caret;
        int hi = d.anchor<d.caret ? d.caret : d.anchor;
        sel[0].acpStart           = lo;
        sel[0].acpEnd             = hi;
        sel[0].style.ase          = d.caret<d.anchor ? TS_AE_START : TS_AE_END;
        sel[0].style.fInterimChar = FALSE;
        *got = 1;
        tsffree(&d);
        tsflog(w, "TSF GetSelection", lo, hi, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG n, TS_SELECTION_ACP const *sel) override
    {
        if (!writing()) {
            return TS_E_NOLOCK;
        }
        if (n<1 || !sel) {
            return E_INVALIDARG;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        LONG lo = sel[0].acpStart, hi = sel[0].acpEnd;
        HRESULT r = S_OK;
        if (lo<0 || lo>hi || hi>d.len) {
            r = TF_E_INVALIDPOS;
        } else if (d.readonly) {
            r = TS_E_READONLY;
        } else {
            int start = sel[0].style.ase == TS_AE_START;
            int ok    = w->hooks.ax.textselect(w->hooks.gui, d.key, start ? hi : lo, start ? lo : hi);
            r = ok ? S_OK : E_FAIL;
        }
        tsffree(&d);
        tsflog(w, "TSF SetSelection", lo, hi, r==S_OK ? "-> ok" : "-> refused");
        if (r == S_OK) {
            tsffree(&told);
            tsfload(w, &told);  // its own change: not to be told back
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetText(LONG lo, LONG hi, WCHAR *buf, ULONG cap, ULONG *got,
                                      TS_RUNINFO *runs, ULONG nruns, ULONG *gotruns,
                                      LONG *next) override
    {
        if (!got || !gotruns || !next) {
            return E_INVALIDARG;
        }
        *got = *gotruns = 0;
        if (!reading()) {
            return TS_E_NOLOCK;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        hi = hi==-1 ? d.len : hi;
        if (lo<0 || lo>hi || hi>d.len) {
            tsffree(&d);
            return TF_E_INVALIDPOS;
        }
        ULONG n = (ULONG)(hi - lo);
        if (buf && cap) {
            n = n<cap ? n : cap;
            memcpy(buf, d.text+lo, (size_t)n*2);
            *got = n;
        }
        if (runs && nruns) {
            runs[0].uCount = n;
            runs[0].type   = TS_RT_PLAIN;
            *gotruns = 1;
        }
        *next = lo + (LONG)n;
        tsffree(&d);
        tsflog(w, "TSF GetText", lo, hi, 0);
        return S_OK;
    }

    // Replace [lo, hi) with s: the change as it came out, which the
    // field's filtering (newlines, tabs) may make other than asked
    HRESULT edit(char const *what, LONG lo, LONG hi, WCHAR const *s, ULONG n,
                 TS_TEXTCHANGE *change, LONG *end)
    {
        W32TsfDoc d;
        tsfload(w, &d);
        HRESULT r = S_OK;
        if (lo<0 || lo>hi || hi>d.len) {
            r = TF_E_INVALIDPOS;
        } else if (d.readonly) {
            r = TS_E_READONLY;
        } else if (!w->hooks.ax.textedit(w->hooks.gui, d.key, lo, hi, (unsigned short const *)s,
                                         (int)n)) {
            r = E_FAIL;
        }
        int before = d.len;
        tsffree(&d);
        if (r != S_OK) {
            tsflog(w, what, lo, hi, "-> refused");
            return r;
        }
        tsffree(&told);
        tsfload(w, &told);  // its own change: not to be told back
        LONG e = hi + (told.len - before);
        if (change) {
            change->acpStart  = lo;
            change->acpOldEnd = hi;
            change->acpNewEnd = e;
        }
        if (end) {
            *end = e;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG lo, LONG hi, WCHAR const *s, ULONG n,
                                      TS_TEXTCHANGE *change) override
    {
        if (!writing()) {
            return TS_E_NOLOCK;
        }
        if (!s && n) {
            return E_INVALIDARG;
        }
        return edit("TSF SetText", lo, hi, s, n, change, 0);
    }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, WCHAR const *s, ULONG n,
                                                    LONG *start, LONG *end,
                                                    TS_TEXTCHANGE *change) override
    {
        W32TsfDoc d;
        tsfload(w, &d);
        LONG lo = d.anchor<d.caret ? d.anchor : d.caret;
        LONG hi = d.anchor<d.caret ? d.caret : d.anchor;
        tsffree(&d);
        if (flags & TF_IAS_QUERYONLY) {
            if (!reading()) {
                return TS_E_NOLOCK;
            }
            if (start) *start = lo;
            if (end) *end = hi;
            return S_OK;
        }
        if (!writing()) {
            return TS_E_NOLOCK;
        }
        if (!s && n) {
            return E_INVALIDARG;
        }
        LONG    e = 0;
        HRESULT r = edit("TSF InsertTextAtSelection", lo, hi, s, n, change, &e);
        if (r==S_OK && start) *start = lo;
        if (r==S_OK && end) *end = e;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject **) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown **) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(GUID const *, FORMATETC const *,
                                                  BOOL *ok) override
    {
        if (!ok) {
            return E_INVALIDARG;
        }
        *ok = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject *,
                                             TS_TEXTCHANGE *) override
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject *, LONG *, LONG *,
                                                        TS_TEXTCHANGE *) override
    {
        return E_NOTIMPL;
    }

    // No attributes: none supported, none at any place
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, TS_ATTRID const *) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, TS_ATTRID const *,
                                                     DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, TS_ATTRID const *,
                                                                  DWORD) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG halt, ULONG, TS_ATTRID const *,
                                                     DWORD, LONG *next, BOOL *found,
                                                     LONG *offset) override
    {
        if (!next || !found || !offset) {
            return E_INVALIDARG;
        }
        *next   = halt;
        *found  = FALSE;
        *offset = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL *, ULONG *got) override
    {
        if (!got) {
            return E_INVALIDARG;
        }
        *got = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEndACP(LONG *end) override
    {
        if (!end) {
            return E_INVALIDARG;
        }
        if (!reading()) {
            return TS_E_NOLOCK;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        *end = d.len;
        tsffree(&d);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie *v) override
    {
        if (!v) {
            return E_INVALIDARG;
        }
        *v = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, POINT const *pt, DWORD,
                                              LONG *acp) override
    {
        if (!pt || !acp) {
            return E_INVALIDARG;
        }
        if (!reading()) {
            return TS_E_NOLOCK;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        POINT c = *pt;
        ScreenToClient(w->hwnd, &c);
        int at = d.key ? w->hooks.ax.textat(w->hooks.gui, d.key, (float)c.x, (float)c.y) : -1;
        tsffree(&d);
        if (at < 0) {
            return TS_E_INVALIDPOINT;
        }
        *acp = at;
        return S_OK;
    }
    // Where [lo, hi) is drawn, in screen pixels: the union of its pieces
    // on each line it is on
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG lo, LONG hi, RECT *rc,
                                         BOOL *clipped) override
    {
        if (!rc || !clipped) {
            return E_INVALIDARG;
        }
        if (!reading()) {
            return TS_E_NOLOCK;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        int len = d.len, key = (int)d.key;
        tsffree(&d);
        if (lo<0 || lo>hi || hi>len) {
            return TF_E_INVALIDPOS;
        }
        if (!key) {
            // No field, nothing in it: the window, as a text service
            // places its windows by it
            RECT c;
            GetClientRect(w->hwnd, &c);
            MapWindowPoints(w->hwnd, 0, (POINT *)&c, 2);
            *rc      = {c.left, c.top, c.left, c.top};
            *clipped = FALSE;
            return S_OK;
        }
        W32AxHooks *h = &w->hooks.ax;
        int   starts[512];
        int   nlines = key ? h->textlines(w->hooks.gui, (unsigned)key, starts, 512) : 0;
        nlines = nlines<512 ? nlines : 512;
        float u[4] = {}, r[4];
        int   have = 0, ok = key != 0;
        for (int a = lo, k = 0; ok;) {
            while (k<nlines && starts[k]<=a) k++;
            int b = k<nlines && starts[k]<hi ? starts[k] : hi;
            ok = h->textrect(w->hooks.gui, (unsigned)key, a, b, r);
            if (ok) {
                if (!have) {
                    memcpy(u, r, sizeof(u));
                } else {
                    u[0] = r[0]<u[0] ? r[0] : u[0];
                    u[1] = r[1]<u[1] ? r[1] : u[1];
                    u[2] = r[2]>u[2] ? r[2] : u[2];
                    u[3] = r[3]>u[3] ? r[3] : u[3];
                }
                have = 1;
            }
            if (b >= hi) {
                break;
            }
            a = b;
        }
        if (!ok || !have) {
            nolayout = 1;
            tsflog(w, "TSF GetTextExt", lo, hi, "-> no layout");
            return TS_E_NOLAYOUT;
        }
        *rc = {(LONG)u[0], (LONG)u[1], (LONG)(u[2] + .999f), (LONG)(u[3] + .999f)};
        MapWindowPoints(w->hwnd, 0, (POINT *)rc, 2);
        *clipped = FALSE;
        tsflog(w, "TSF GetTextExt", lo, hi, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT *rc) override
    {
        if (!rc) {
            return E_INVALIDARG;
        }
        W32TsfDoc d;
        tsfload(w, &d);
        *rc = d.at;
        tsffree(&d);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND *out) override
    {
        if (!out) {
            return E_INVALIDARG;
        }
        *out = w->hwnd;
        return S_OK;
    }

    // Compositions, each allowed: their text is the document's
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView *, BOOL *ok) override
    {
        if (ok) {
            *ok = TRUE;
        }
        composing++;
        tsflog(w, "TSF composition starts", LLONG_MIN, LLONG_MIN, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView *, ITfRange *) override
    {
        tsflog(w, "TSF composition updates", LLONG_MIN, LLONG_MIN, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView *) override
    {
        composing -= composing > 0;
        tsflog(w, "TSF composition ends", LLONG_MIN, LLONG_MIN, 0);
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE W32Tsf::RequestLock(DWORD flags, HRESULT *hr)
{
    if (!sink) {
        return E_UNEXPECTED;
    }
    if (!hr) {
        return E_INVALIDARG;
    }
    DWORD want = flags & TS_LF_READWRITE;
    if (lock) {
        if (flags & TS_LF_SYNC) {
            *hr = TS_E_SYNCHRONOUS;
        } else {
            queued |= want;
            *hr     = TS_S_ASYNC;
        }
        tsflog(w, *hr==TS_S_ASYNC ? "TSF lock queued" : "TSF lock refused: held",
               LLONG_MIN, LLONG_MIN, 0);
        return S_OK;
    }
    AddRef();  // a sink's call may let the store go
    *hr = grant(want);
    while (queued && sink) {
        DWORD q = queued;
        queued  = 0;
        grant(q);
    }
    tsfsync(this, 0);  // what changed meanwhile, held back while locked
    Release();
    return S_OK;
}

// Tell TSF what changed in the document since it was last told: typing,
// ImGui's edits, another field taking the focus
//
// A layout once refused (TS_E_NOLAYOUT) is told only after a publish
// (published), when there may be one: told at once, TSF would ask
// again, be refused again, and be told again, without end.
static void tsfsync(W32Tsf *t, int published)
{
    if (!t || !t->sink || t->lock || t->syncing) {
        return;
    }
    W32TsfDoc now;
    tsfload(t->w, &now);
    W32TsfDoc was  = t->told;
    t->told        = now;  // before telling: a sink's lock reads it
    int field      = now.key != was.key;
    if (field && t->composing && w32tsf.ctx) {
        // A composition over another field's text ends with it
        ITfContextOwnerCompositionServices *cs = 0;
        if (SUCCEEDED(w32tsf.ctx->QueryInterface(__uuidof(ITfContextOwnerCompositionServices),
                                                (void **)&cs))) {
            cs->TerminateComposition(0);
            cs->Release();
        }
        t->composing = 0;
    }
    int p = 0, oldend = was.len, newend = now.len;
    if (!field) {
        int n = was.len<now.len ? was.len : now.len;
        while (p<n && was.text[p]==now.text[p]) p++;
        while (oldend>p && newend>p && was.text[oldend-1]==now.text[newend-1]) {
            oldend--;
            newend--;
        }
    }
    int edited = field || p<oldend || p<newend;
    int moved  = memcmp(&was.at, &now.at, sizeof(RECT)) != 0;
    int sel    = edited || was.anchor!=now.anchor || was.caret!=now.caret;
    int status = was.readonly != now.readonly;
    tsffree(&was);

    t->AddRef();
    t->syncing = 1;
    if (edited && (t->mask & TS_AS_TEXT_CHANGE)) {
        TS_TEXTCHANGE c = {p, oldend, newend};
        t->sink->OnTextChange(0, &c);
    }
    if (t->sink && sel && (t->mask & TS_AS_SEL_CHANGE)) {
        t->sink->OnSelectionChange();
    }
    if (t->sink && status && (t->mask & TS_AS_STATUS_CHANGE)) {
        t->sink->OnStatusChange(now.readonly ? TS_SD_READONLY : 0);
    }
    int relayout = published && t->nolayout;
    if (t->sink && (edited || moved || relayout) && (t->mask & TS_AS_LAYOUT_CHANGE)) {
        t->nolayout = relayout ? 0 : t->nolayout;
        t->sink->OnLayoutChange(TS_LC_CHANGE, 1);
    }
    if (edited || sel || status) {
        axlogf(t->w, "TSF told", field ? -1 : p, edited ? newend : LLONG_MIN,
               field ? "another field" : edited ? "text" : sel ? "selection" : "status");
    }
    t->syncing = 0;
    t->Release();
}

// The checklist's sink: how many edits TSF said ended, as a text
// service hears of a change it didn't make
struct W32TsfEdits : ITfTextEditSink, ITfCompositionSink {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(ITfTextEditSink))) {
            *out = static_cast<ITfTextEditSink *>(this);
            AddRef();
            return S_OK;
        } else if (IsEqualIID(id, __uuidof(ITfCompositionSink))) {
            *out = static_cast<ITfCompositionSink *>(this);
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext *, TfEditCookie, ITfEditRecord *) override
    {
        w32tsf.edits++;
        return S_OK;
    }
    // Its compositions' owner, as a text service is
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition *) override
    {
        return S_OK;
    }
};

// At the first root asked for: the store, the window's document
static void tsfstart(W32Gui *w)
{
    if (w32tsf.store) {
        return;
    }
    char const *fail = 0;
    if (FAILED(CoCreateInstance(tsfthreadmgr, 0, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgr),
                                (void **)&w32tsf.tm))) {
        w32tsf.tm = 0;
        fail      = "no thread manager";
    } else if (FAILED(w32tsf.tm->Activate(&w32tsf.cid))) {
        w32tsf.tm->Release();
        w32tsf.tm = 0;
        fail      = "not activated";
    } else if (FAILED(w32tsf.tm->CreateDocumentMgr(&w32tsf.dm))) {
        w32tsf.dm = 0;
        fail      = "no document manager";
    } else {
        W32Tsf *s = new (std::nothrow) W32Tsf(w);
        if (!s) {
            fail = "no memory";
        } else if (FAILED(w32tsf.dm->CreateContext(w32tsf.cid, 0, static_cast<ITextStoreACP *>(s),
                                                   &w32tsf.ctx, &w32tsf.ec))) {
            w32tsf.ctx = 0;
            s->Release();
            fail = "no context";
        } else if (FAILED(w32tsf.dm->Push(w32tsf.ctx))) {
            w32tsf.ctx->Release();
            w32tsf.ctx = 0;
            s->Release();
            fail = "not pushed";
        } else {
            w32tsf.store = s;
            w32tsf.tm->AssociateFocus(w->hwnd, w32tsf.dm, &w32tsf.prev);
            ITfSource *src = 0;
            if (SUCCEEDED(w32tsf.ctx->QueryInterface(__uuidof(ITfSource), (void **)&src))) {
                W32TsfEdits *e = new (std::nothrow) W32TsfEdits;
                if (e) {
                    src->AdviseSink(__uuidof(ITfTextEditSink), static_cast<ITfTextEditSink *>(e),
                                    &w32tsf.editcookie);
                    e->Release();
                }
                src->Release();
            }
        }
    }
    if (fail) {
        if (w32tsf.dm) {
            w32tsf.dm->Release();
            w32tsf.dm = 0;
        }
        if (w32tsf.tm) {
            w32tsf.tm->Deactivate();
            w32tsf.tm->Release();
            w32tsf.tm = 0;
        }
    }
    axlogf(w, "TSF store", LLONG_MIN, LLONG_MIN, fail ? fail : "-> associated");
}

// The window goes: its document with it, the one before given back
static void tsfgone(W32Gui *w)
{
    if (!w32tsf.store) {
        return;
    }
    ITfDocumentMgr *mine = 0;
    if (w->hwnd) {
        w32tsf.tm->AssociateFocus(w->hwnd, w32tsf.prev, &mine);
    }
    if (mine) mine->Release();
    if (w32tsf.prev) w32tsf.prev->Release();
    if (w32tsf.editcookie) {
        ITfSource *src = 0;
        if (SUCCEEDED(w32tsf.ctx->QueryInterface(__uuidof(ITfSource), (void **)&src))) {
            src->UnadviseSink(w32tsf.editcookie);
            src->Release();
        }
    }
    w32tsf.dm->Pop(TF_POPF_ALL);
    w32tsf.ctx->Release();
    w32tsf.dm->Release();
    w32tsf.store->Release();
    w32tsf.tm->Deactivate();
    w32tsf.tm->Release();
    w32tsf = {};
}

// The checklist's text service: an edit session through the manager, as
// Voice Access's come, on units [lo, hi)
enum { TSFOP_SET, TSFOP_COMPOSE, TSFOP_SELECT, TSFOP_READ, TSFOP_SELIS, TSFOP_EXT };

struct W32TsfSession : ITfEditSession {
    LONG          refs = 1;
    int           op, lo, hi;
    WCHAR const  *s;
    int           n;
    WCHAR         got[256];
    ULONG         gotn = 0;
    LONG          a = -1, b = -1;
    RECT          rc = {};
    HRESULT       hr = E_FAIL;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(ITfEditSession))) {
            *out = static_cast<ITfEditSession *>(this);
            AddRef();
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return (ULONG)r;
    }

    ITfRange *range(TfEditCookie ec, int x, int y)
    {
        ITfRange    *r  = 0;
        ITfRangeACP *ra = 0;
        if (FAILED(w32tsf.ctx->GetStart(ec, &r))) {
            return 0;
        }
        if (FAILED(r->QueryInterface(__uuidof(ITfRangeACP), (void **)&ra)) ||
                FAILED(ra->SetExtent(x, y - x))) {
            if (ra) ra->Release();
            r->Release();
            return 0;
        }
        ra->Release();
        return r;
    }

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override
    {
        ITfContext *ctx = w32tsf.ctx;
        if (op == TSFOP_READ) {
            ITfRange *r = 0, *e = 0;
            if (SUCCEEDED(ctx->GetStart(ec, &r)) && SUCCEEDED(ctx->GetEnd(ec, &e))) {
                r->ShiftEndToRange(ec, e, TF_ANCHOR_END);
                hr = r->GetText(ec, 0, got, 255, &gotn);
            }
            if (r) r->Release();
            if (e) e->Release();
            return S_OK;
        }
        if (op == TSFOP_SELIS) {
            TF_SELECTION sel = {};
            ULONG        k   = 0;
            hr = ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &k);
            if (SUCCEEDED(hr) && k==1) {
                ITfRangeACP *ra = 0;
                if (SUCCEEDED(sel.range->QueryInterface(__uuidof(ITfRangeACP), (void **)&ra))) {
                    LONG cch = 0;
                    ra->GetExtent(&a, &cch);
                    b = a + cch;
                    ra->Release();
                }
                sel.range->Release();
            }
            return S_OK;
        }
        ITfRange *r = range(ec, lo, hi);
        if (!r) {
            return S_OK;
        }
        if (op == TSFOP_SET) {
            hr = r->SetText(ec, 0, s, n);
        } else if (op == TSFOP_SELECT) {
            TF_SELECTION sel = {};
            sel.range          = r;
            sel.style.ase      = TF_AE_END;
            sel.style.fInterimChar = FALSE;
            hr = ctx->SetSelection(ec, 1, &sel);
        } else if (op == TSFOP_COMPOSE) {
            ITfContextComposition *cc   = 0;
            ITfComposition        *comp = 0;
            hr = ctx->QueryInterface(__uuidof(ITfContextComposition), (void **)&cc);
            W32TsfEdits *owner = new (std::nothrow) W32TsfEdits;
            hr = SUCCEEDED(hr) && !owner ? E_OUTOFMEMORY : hr;
            hr = SUCCEEDED(hr) ? cc->StartComposition(ec, r, owner, &comp) : hr;
            if (owner) owner->Release();
            hr = SUCCEEDED(hr) && !comp ? E_ABORT : hr;
            if (SUCCEEDED(hr)) {
                hr = r->SetText(ec, 0, s, n);
                axlogf(axw, "checklist composition SetText", LLONG_MIN, LLONG_MIN,
                       SUCCEEDED(hr) ? "-> ok" : "-> failed");
                comp->EndComposition(ec);
                comp->Release();
            }
            if (cc) cc->Release();
        } else if (op == TSFOP_EXT) {
            ITfContextView *v = 0;
            BOOL clipped = FALSE;
            if (SUCCEEDED(ctx->GetActiveView(&v))) {
                hr = v->GetTextExt(ec, r, &rc, &clipped);
                v->Release();
            }
        }
        r->Release();
        return S_OK;
    }
};

// One session, synchronous: 0 if it couldn't run
static W32TsfSession *tsfsession(int op, int lo, int hi, WCHAR const *s, int n)
{
    if (!w32tsf.ctx) {
        return 0;
    }
    W32TsfSession *e = new (std::nothrow) W32TsfSession;
    if (!e) {
        return 0;
    }
    e->op = op;
    e->lo = lo;
    e->hi = hi;
    e->s  = s;
    e->n  = n;
    HRESULT got = E_FAIL;
    int     rw  = op==TSFOP_READ || op==TSFOP_SELIS || op==TSFOP_EXT ? TF_ES_READ : TF_ES_READWRITE;
    if (FAILED(w32tsf.ctx->RequestEditSession(w32tsf.cid, e, TF_ES_SYNC | rw, &got)) ||
            FAILED(got)) {
        e->Release();
        return 0;
    }
    return e;
}

// The checklist's "uia" checks: through the providers' methods, as a
// client's calls come to them, from the root the window gives
// UI Automation.

static struct { char const *name; long id; } const axtypenames[] = {
    {"Button", UIA_ButtonControlTypeId},       {"CheckBox", UIA_CheckBoxControlTypeId},
    {"ComboBox", UIA_ComboBoxControlTypeId},   {"Custom", UIA_CustomControlTypeId},
    {"Edit", UIA_EditControlTypeId},           {"Group", UIA_GroupControlTypeId},
    {"Hyperlink", UIA_HyperlinkControlTypeId}, {"Image", UIA_ImageControlTypeId},
    {"ListItem", UIA_ListItemControlTypeId},   {"Menu", UIA_MenuControlTypeId},
    {"MenuBar", UIA_MenuBarControlTypeId},     {"MenuItem", UIA_MenuItemControlTypeId},
    {"Pane", UIA_PaneControlTypeId},           {"RadioButton", UIA_RadioButtonControlTypeId},
    {"Slider", UIA_SliderControlTypeId},       {"Tab", UIA_TabControlTypeId},
    {"TabItem", UIA_TabItemControlTypeId},     {"Text", UIA_TextControlTypeId},
    {"TreeItem", UIA_TreeItemControlTypeId},   {"Window", UIA_WindowControlTypeId},
};

static struct { char const *name; PATTERNID id; } const axpatnames[] = {
    {"Invoke", UIA_InvokePatternId}, {"Toggle", UIA_TogglePatternId},
    {"ExpandCollapse", UIA_ExpandCollapsePatternId},
    {"SelectionItem", UIA_SelectionItemPatternId}, {"Value", UIA_ValuePatternId},
    {"Text", UIA_TextPatternId}, {"Text2", UIA_TextPattern2Id},
};

static char const *axpropname(int id)
{
    switch (id) {
    case UIA_ControlTypePropertyId:          return "ControlType";
    case UIA_NamePropertyId:                 return "Name";
    case UIA_HelpTextPropertyId:             return "HelpText";
    case UIA_AutomationIdPropertyId:         return "AutomationId";
    case UIA_IsKeyboardFocusablePropertyId:  return "IsKeyboardFocusable";
    case UIA_HasKeyboardFocusPropertyId:     return "HasKeyboardFocus";
    case UIA_IsEnabledPropertyId:            return "IsEnabled";
    case UIA_IsOffscreenPropertyId:          return "IsOffscreen";
    case UIA_IsPasswordPropertyId:           return "IsPassword";
    case UIA_IsDialogPropertyId:             return "IsDialog";
    case UIA_IsControlElementPropertyId:     return "IsControlElement";
    case UIA_IsContentElementPropertyId:     return "IsContentElement";
    case UIA_ClickablePointPropertyId:       return "ClickablePoint";
    case UIA_LocalizedControlTypePropertyId: return "LocalizedControlType";
    case UIA_AcceleratorKeyPropertyId:       return "AcceleratorKey";
    case UIA_AccessKeyPropertyId:            return "AccessKey";
    case UIA_ClassNamePropertyId:            return "ClassName";
    case UIA_FrameworkIdPropertyId:          return "FrameworkId";
    case UIA_ProcessIdPropertyId:            return "ProcessId";
    case UIA_LabeledByPropertyId:            return "LabeledBy";
    case UIA_ItemStatusPropertyId:           return "ItemStatus";
    case UIA_ItemTypePropertyId:             return "ItemType";
    case UIA_BoundingRectanglePropertyId:    return "BoundingRectangle";
    case UIA_NativeWindowHandlePropertyId:   return "NativeWindowHandle";
    case UIA_RuntimeIdPropertyId:            return "RuntimeId";
    case UIA_ProviderDescriptionPropertyId:  return "ProviderDescription";
    case UIA_IsPeripheralPropertyId:         return "IsPeripheral";
    case UIA_LiveSettingPropertyId:          return "LiveSetting";
    case UIA_LegacyIAccessibleRolePropertyId:          return "LegacyRole";
    case UIA_LegacyIAccessibleDefaultActionPropertyId: return "LegacyDefaultAction";
    case UIA_LegacyIAccessibleStatePropertyId:         return "LegacyState";
    }
    return 0;
}

static char const *axpatname(int id)
{
    switch (id) {
    case UIA_InvokePatternId:           return "Invoke";
    case UIA_TogglePatternId:           return "Toggle";
    case UIA_ExpandCollapsePatternId:   return "ExpandCollapse";
    case UIA_SelectionItemPatternId:    return "SelectionItem";
    case UIA_SelectionPatternId:        return "Selection";
    case UIA_ValuePatternId:            return "Value";
    case UIA_TextPatternId:             return "Text";
    case UIA_TextPattern2Id:            return "Text2";
    case UIA_TextEditPatternId:         return "TextEdit";
    case UIA_LegacyIAccessiblePatternId: return "LegacyIAccessible";
    case UIA_ScrollItemPatternId:       return "ScrollItem";
    case UIA_ScrollPatternId:           return "Scroll";
    case UIA_RangeValuePatternId:       return "RangeValue";
    case UIA_GridItemPatternId:         return "GridItem";
    case UIA_TableItemPatternId:        return "TableItem";
    case UIA_VirtualizedItemPatternId:  return "VirtualizedItem";
    case UIA_ItemContainerPatternId:    return "ItemContainer";
    case UIA_WindowPatternId:           return "Window";
    }
    return 0;
}

// An element as the trace names it: its control type and name
static void axdescribe(W32AxLine *l, W32AxProv *p)
{
    W32Gui     *w = p->w;
    W32AxHooks *h = &w->hooks.ax;
    W32AxNode   n = {};
    if (p->key == W32AX_ROOT) {
        axlput(l, "root");
    } else if (h->node(w->hooks.gui, p->key, &n)) {
        long ct = axcontroltypes[n.role>=0 && n.role<W32R_N ? n.role : 0];
        char const *tn = "?";
        for (int j = 0; j < (int)(sizeof(axtypenames)/sizeof(*axtypenames)); j++) {
            tn = axtypenames[j].id==ct ? axtypenames[j].name : tn;
        }
        axlput(l, tn);
        axlput(l, " ");
        unsigned short buf[48];
        int len = h->text(w->hooks.gui, p->key, W32AXSTR_NAME, buf, 48);
        axlputw(l, (wchar_t const *)buf, len<48 ? len : 48);
    } else {
        axlput(l, "(gone)");
    }
}

// A client's hit test, logged: how it finds where to click an element
static void axhitlog(W32AxProv *p, double x, double y)
{
    W32Gui     *w = p->w;
    W32AxHooks *h = &w->hooks.ax;
    if (!h->logging || !(h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        return;
    }
    POINT o = {0, 0};
    ClientToScreen(w->hwnd, &o);
    W32AxLine l = {};
    axlput(&l, "hit ");
    axlputn(&l, (long long)x - o.x);
    axlput(&l, ",");
    axlputn(&l, (long long)y - o.y);
    axlput(&l, " -> ");
    axdescribe(&l, p);
    axsend(w, &l);
}

// The first ask of each element and property or pattern, with the
// answer, while the log is read: a set of (key, id), let go when full
static void axasked(W32AxProv *p, int pattern, int id, VARIANT const *v, int has)
{
    // Logged the first time, and again whenever the answer changes: a
    // tool may decide from a state it saw once (disabled, read-only)
    enum { SEEN = 1<<14 };
    static unsigned long long seen[SEEN];
    static unsigned long long said[SEEN];
    static int                nseen;
    W32Gui     *w = p->w;
    W32AxHooks *h = w ? &w->hooks.ax : 0;
    if (!h || !h->logging || !(h->logging(w->hooks.gui) & W32AXLOG_ON)) {
        return;
    }
    unsigned long long answer = 0x100;  // a digest of the answer
    if (pattern) {
        answer = has ? 1 : 0;
    } else if (v && v->vt==VT_BOOL) {
        answer = 0x200 | (v->boolVal ? 1u : 0u);
    } else if (v && v->vt==VT_I4) {
        answer = 0x300ull<<32 | (unsigned)v->lVal;
    } else if (v && v->vt==VT_BSTR) {
        answer = 0x400;
        UINT n = SysStringLen(v->bstrVal);
        for (UINT j = 0; j < n; j++) {
            answer = (answer ^ v->bstrVal[j]) * 1111111111111111111u;
        }
    } else if (v) {
        answer = 0x500u | v->vt;
    }
    unsigned long long k = (unsigned long long)p->key<<32 | (unsigned)id<<1 | (pattern!=0);
    unsigned long long hash = (k + 1) * 0x9e3779b97f4a7c15u;
    int i = (int)(hash >> 50);
    for (; seen[i]; i = (i+1) & (SEEN-1)) {
        if (seen[i] == k+1) {
            break;
        }
    }
    if (seen[i] == k+1) {
        if (said[i] == answer) {
            return;
        }
    } else {
        if (nseen >= SEEN/2) {
            memset(seen, 0, sizeof(seen));
            nseen = 0;
            i = (int)(hash >> 50);
        }
        seen[i] = k + 1;
        nseen++;
    }
    said[i] = answer;

    W32AxLine l = {};
    axlput(&l, "ask ");
    axdescribe(&l, p);
    char const *name = pattern ? axpatname(id) : axpropname(id);

    axlput(&l, pattern ? " pattern " : " ");
    if (name) {
        axlput(&l, name);
    } else {
        axlputn(&l, id);
    }
    axlput(&l, " -> ");
    if (pattern) {
        axlput(&l, has ? "yes" : "no");
    } else if (!v || v->vt==VT_EMPTY) {
        axlput(&l, "(default)");
    } else if (v->vt == VT_BOOL) {
        axlput(&l, v->boolVal ? "true" : "false");
    } else if (v->vt == VT_I4) {
        axlputn(&l, v->lVal);
    } else if (v->vt==VT_BSTR && id!=UIA_ValueValuePropertyId) {
        axlputw(&l, v->bstrVal, (int)SysStringLen(v->bstrVal));
    } else {
        axlput(&l, "vt ");
        axlputn(&l, v->vt);
    }
    axsend(w, &l);
}

static int axeq(char const *a, int alen, char const *b)
{
    int i = 0;
    for (; i<alen && b[i]; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return i==alen && !b[i];
}

static long axtypeid(char const *s, int len)
{
    for (int i = 0; i < (int)(sizeof(axtypenames)/sizeof(*axtypenames)); i++) {
        if (axeq(s, len, axtypenames[i].name)) {
            return axtypenames[i].id;
        }
    }
    return -1;
}

// p's control type and name match, name as UTF-8
static int axmatches(IRawElementProviderSimple *p, long type, char const *name, int len)
{
    VARIANT v = {};
    if (FAILED(p->GetPropertyValue(UIA_ControlTypePropertyId, &v)) || v.vt!=VT_I4 ||
            v.lVal!=type) {
        return 0;
    }
    v = {};
    if (FAILED(p->GetPropertyValue(UIA_NamePropertyId, &v))) {
        return 0;
    }
    char buf[1024];
    int  n = 0;
    if (v.vt == VT_BSTR) {
        n = WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, (int)SysStringLen(v.bstrVal),
                                buf, (int)sizeof(buf), 0, 0);
        SysFreeString(v.bstrVal);
    }
    return n==len && !memcmp(buf, name, (size_t)len);
}

// The first fragment under f, depth first through Navigate, that
// matches, with a reference; or null
static IRawElementProviderFragment *axsearch(IRawElementProviderFragment *f, long type,
                                             char const *name, int len, int depth, int *budget)
{
    IRawElementProviderFragment *c = 0;
    f->Navigate(NavigateDirection_FirstChild, &c);
    while (c && --*budget>0) {
        IRawElementProviderSimple *s = 0;
        c->QueryInterface(__uuidof(IRawElementProviderSimple), (void **)&s);
        int hit = s && axmatches(s, type, name, len);
        if (s) {
            s->Release();
        }
        if (hit) {
            return c;
        }
        IRawElementProviderFragment *in = depth<64 ? axsearch(c, type, name, len, depth+1, budget) : 0;
        if (in) {
            c->Release();
            return in;
        }
        IRawElementProviderFragment *next = 0;
        c->Navigate(NavigateDirection_NextSibling, &next);
        c->Release();
        c = next;
    }
    if (c) {
        c->Release();
    }
    return 0;
}

static void axout(char *out, int cap, W32AxLine *l)
{
    int n = l->len < cap-1 ? l->len : cap-1;
    memcpy(out, l->buf, (size_t)n);
    out[n] = 0;
}

extern "C" void w32gui_axdrive(void *gp, char const *cmd, int len, char *out, int cap)
{
    W32Gui   *w = (W32Gui *)gp;
    W32AxLine l = {};
    // verb, then words as the verb takes them, then the name
    char const *s = cmd, *e = cmd + len;
    char const *word[3] = {};
    int         wlen[3] = {};
    int         nwords  = 0;
    char const *verb = s;
    while (s<e && *s!=' ') s++;
    int vlen = (int)(s - verb);
    int want = axeq(verb, vlen, "start") || axeq(verb, vlen, "newline") ||
               axeq(verb, vlen, "emptycommit") || axeq(verb, vlen, "tsf") ||
               axeq(verb, vlen, "tsfread") || axeq(verb, vlen, "tsfedits") ? 0
             : axeq(verb, vlen, "reconvert") || axeq(verb, vlen, "confirm") ? 1
             : axeq(verb, vlen, "can") || axeq(verb, vlen, "parent") ||
               axeq(verb, vlen, "value") || axeq(verb, vlen, "setvalue") ||
               axeq(verb, vlen, "textis") || axeq(verb, vlen, "textsel") ||
               axeq(verb, vlen, "selis") || axeq(verb, vlen, "textunit") ? 2 : 1;
    for (; nwords < want; nwords++) {
        while (s<e && *s==' ') s++;
        word[nwords] = s;
        while (s<e && *s!=' ') s++;
        wlen[nwords] = (int)(s - word[nwords]);
    }
    if (s<e && *s==' ') s++;
    char const *name = s;
    int         nlen = (int)(e - s);

    if (!w || !w->hwnd) {
        axlput(&l, "no window");
        axout(out, cap, &l);
        return;
    }
    if (axeq(verb, vlen, "start")) {
        // What the message returns is UI Automation's, for a client of
        // another process: here, only that the root was made counts
        SendMessageW(w->hwnd, WM_GETOBJECT, 0, (LPARAM)UiaRootObjectId);
        axlput(&l, w32ax.root && w32ax.asked ? "ok" : "no root made");
        axout(out, cap, &l);
        return;
    }
    if (axeq(verb, vlen, "newline")) {
        // As dictation's "new line" comes: a commit, then Enter (posted,
        // as the checklist's keys are, though dictation's say keyboard)
        PostMessageW(w->hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        UINT scan = MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC);
        PostMessageW(w->hwnd, WM_KEYDOWN, VK_RETURN, 1 | (LPARAM)scan<<16);
        PostMessageW(w->hwnd, WM_KEYUP, VK_RETURN, 1 | (LPARAM)scan<<16 | (LPARAM)3<<30);
        axlput(&l, "ok");
        axout(out, cap, &l);
        return;
    }
    if (axeq(verb, vlen, "emptycommit")) {
        // As "delete that" may end: a commit of nothing (the context's
        // result string, the checklist's, is empty)
        PostMessageW(w->hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        axlput(&l, "ok");
        axout(out, cap, &l);
        return;
    }
    if (vlen>=3 && axeq(verb, 3, "tsf")) {
        // As a text service edits through TSF, an edit session each:
        //   tsf                 the store is the window's, its sink advised
        //   tsfset LO,HI TEXT   units [LO, HI) replaced with TEXT
        //   tsfcompose LO,HI T  the same, in a composition
        //   tsfsel LO,HI        selected
        //   tsfselis LO,HI      the selection TSF reads is [LO, HI)
        //   tsfread TEXT        the text TSF reads is TEXT
        //   tsfext LO,HI        TSF finds [LO, HI) drawn in the window
        //   tsfedits            TSF told a text service of an edit since
        //                       the last ask
        int lo = 0, hi = 0, *at = &lo;
        for (int i = 0; i < wlen[0]; i++) {
            char c = word[0][i];
            if (c == ',') {
                at = &hi;
            } else {
                *at = *at*10 + (c - '0');
            }
        }
        WCHAR text[256];
        int   tn = nlen>0 ? MultiByteToWideChar(CP_UTF8, 0, name, nlen, text, 255) : 0;
        tn = tn>0 ? tn : 0;
        text[tn] = 0;
        W32TsfSession *e = 0;
        if (!w32tsf.store) {
            axlput(&l, "no store");
        } else if (axeq(verb, vlen, "tsf")) {
            axlput(&l, w32tsf.store->sink ? "ok" : "no sink advised");
        } else if (axeq(verb, vlen, "tsfedits")) {
            axlput(&l, w32tsf.edits ? "ok" : "no edit told");
            w32tsf.edits = 0;
        } else {
            int op = axeq(verb, vlen, "tsfset")     ? TSFOP_SET
                   : axeq(verb, vlen, "tsfcompose") ? TSFOP_COMPOSE
                   : axeq(verb, vlen, "tsfsel")     ? TSFOP_SELECT
                   : axeq(verb, vlen, "tsfselis")   ? TSFOP_SELIS
                   : axeq(verb, vlen, "tsfread")    ? TSFOP_READ
                   : axeq(verb, vlen, "tsfext")     ? TSFOP_EXT : -1;
            e = op<0 ? 0 : tsfsession(op, lo, hi, text, tn);
            RECT wr;
            GetClientRect(w->hwnd, &wr);
            MapWindowPoints(w->hwnd, 0, (POINT *)&wr, 2);
            if (op < 0) {
                axlput(&l, "unknown verb");
            } else if (!e) {
                axlput(&l, "no edit session");
            } else if (FAILED(e->hr)) {
                axlput(&l, "failed ");
                axlputx(&l, (unsigned long)e->hr);
            } else if (op == TSFOP_READ) {
                if (e->gotn==(ULONG)tn && !memcmp(e->got, text, (size_t)tn*2)) {
                    axlput(&l, "ok");
                } else {
                    axlput(&l, "read ");
                    axlputw(&l, e->got, (int)e->gotn);
                }
            } else if (op == TSFOP_SELIS) {
                if (e->a==lo && e->b==hi) {
                    axlput(&l, "ok");
                } else {
                    axlput(&l, "selection ");
                    axlputn(&l, e->a);
                    axlput(&l, ",");
                    axlputn(&l, e->b);
                }
            } else if (op == TSFOP_EXT) {
                RECT r = e->rc;
                if (r.bottom>r.top && r.right>=r.left && r.left>=wr.left && r.right<=wr.right &&
                        r.top>=wr.top && r.bottom<=wr.bottom) {
                    axlput(&l, "ok");
                } else {
                    axlput(&l, "rect ");
                    axlputn(&l, r.left);
                    axlput(&l, ",");
                    axlputn(&l, r.top);
                    axlput(&l, ",");
                    axlputn(&l, r.right);
                    axlput(&l, ",");
                    axlputn(&l, r.bottom);
                }
            } else {
                axlput(&l, "ok");
            }
        }
        if (e) {
            e->Release();
        }
        axout(out, cap, &l);
        return;
    }
    if (axeq(verb, vlen, "reconvert") || axeq(verb, vlen, "confirm")) {
        // reconvert LO,HI: the field's text with LO,HI to convert, as an
        // input method asks for it; confirm LO,HI: that part settled on
        int lo = 0, hi = 0, *at = &lo;
        for (int i = 0; i < wlen[0]; i++) {
            char c = word[0][i];
            if (c == ',') {
                at = &hi;
            } else {
                *at = *at*10 + (c - '0');
            }
        }
        LRESULT need = SendMessageW(w->hwnd, WM_IME_REQUEST, IMR_RECONVERTSTRING, 0);
        RECONVERTSTRING *r = need>(LRESULT)sizeof(RECONVERTSTRING)
            ? (RECONVERTSTRING *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)need) : 0;
        if (!r) {
            axlput(&l, "no reconversion offered");
            axout(out, cap, &l);
            return;
        }
        r->dwSize = (DWORD)need;
        LRESULT got = SendMessageW(w->hwnd, WM_IME_REQUEST, IMR_RECONVERTSTRING, (LPARAM)r);
        int a = (int)(r->dwCompStrOffset/2), b = a + (int)r->dwCompStrLen;
        if (got != need) {
            axlput(&l, "not given");
        } else if (axeq(verb, vlen, "reconvert")) {
            if (a==lo && b==hi) {
                axlput(&l, "ok");
            } else {
                axlput(&l, "offered ");
                axlputn(&l, a);
                axlput(&l, ",");
                axlputn(&l, b);
            }
        } else {
            r->dwCompStrOffset   = (DWORD)lo * 2;
            r->dwCompStrLen      = (DWORD)(hi - lo);
            r->dwTargetStrOffset = r->dwCompStrOffset;
            r->dwTargetStrLen    = r->dwCompStrLen;
            LRESULT ok = SendMessageW(w->hwnd, WM_IME_REQUEST, IMR_CONFIRMRECONVERTSTRING, (LPARAM)r);
            axlput(&l, ok ? "ok" : "refused");
        }
        HeapFree(GetProcessHeap(), 0, r);
        axout(out, cap, &l);
        return;
    }
    W32AxProv *root = w32ax.root;
    if (!root) {
        axlput(&l, "no root: not started");
        axout(out, cap, &l);
        return;
    }
    long type = axtypeid(word[want-1], wlen[want-1]);
    if (type < 0) {
        axlput(&l, "unknown control type");
        axout(out, cap, &l);
        return;
    }
    if (axeq(verb, vlen, "focus")) {
        IRawElementProviderFragment *f = 0;
        root->GetFocus(&f);
        IRawElementProviderSimple *fs = 0;
        if (f) {
            f->QueryInterface(__uuidof(IRawElementProviderSimple), (void **)&fs);
            f->Release();
        }
        axlput(&l, !fs ? "nothing focused" : axmatches(fs, type, name, nlen) ? "ok"
                                                                         : "another focused");
        if (fs) {
            fs->Release();
        }
        axout(out, cap, &l);
        return;
    }
    int budget = 20000;
    IRawElementProviderFragment *f = axsearch(root, type, name, nlen, 0, &budget);
    if (!f) {
        axlput(&l, "not found");
        axout(out, cap, &l);
        return;
    }
    IRawElementProviderSimple *fs = 0;
    f->QueryInterface(__uuidof(IRawElementProviderSimple), (void **)&fs);

    if (axeq(verb, vlen, "has")) {
        axlput(&l, "ok");
    } else if (axeq(verb, vlen, "can")) {
        // PAT,!PAT,...: each had, or not
        int         ok = 1;
        char const *p  = word[0], *pe = word[0] + wlen[0];
        while (p < pe) {
            int not_ = *p == '!';
            p += not_;
            char const *q = p;
            while (q<pe && *q!=',') q++;
            PATTERNID id = 0;
            for (int i = 0; i < (int)(sizeof(axpatnames)/sizeof(*axpatnames)); i++) {
                id = axeq(p, (int)(q-p), axpatnames[i].name) ? axpatnames[i].id : id;
            }
            IUnknown *u = 0;
            if (id) {
                fs->GetPatternProvider(id, &u);
            }
            if (!id || !u==!not_) {
                ok = 0;
                axlput(&l, l.len ? ", " : "");
                axlput(&l, !id ? "unknown pattern " : not_ ? "has " : "lacks ");
                for (char const *c = p; c<q && l.len<(int)sizeof(l.buf); c++) {
                    l.buf[l.len++] = *c;
                }
            }
            if (u) {
                u->Release();
            }
            p = q<pe ? q+1 : q;
        }
        if (ok) {
            axlput(&l, "ok");
        }
    } else if (axeq(verb, vlen, "press")) {
        IUnknown *u = 0;
        HRESULT   r = E_NOINTERFACE;
        if (fs->GetPatternProvider(UIA_InvokePatternId, &u)==S_OK && u) {
            r = ((IInvokeProvider *)u)->Invoke();
        } else if (fs->GetPatternProvider(UIA_TogglePatternId, &u)==S_OK && u) {
            r = ((IToggleProvider *)u)->Toggle();
        } else if (fs->GetPatternProvider(UIA_SelectionItemPatternId, &u)==S_OK && u) {
            r = ((ISelectionItemProvider *)u)->Select();
        } else if (fs->GetPatternProvider(UIA_ExpandCollapsePatternId, &u)==S_OK && u) {
            r = ((IExpandCollapseProvider *)u)->Expand();
        }
        if (u) {
            u->Release();
        }
        if (r == S_OK) {
            axlput(&l, "ok");
        } else {
            axlput(&l, "refused ");
            axlputx(&l, (unsigned long)r);
        }
    } else if (axeq(verb, vlen, "value") || axeq(verb, vlen, "setvalue")) {
        // Through the Value pattern, TEXT a word, "-" for empty
        IUnknown *u = 0;
        int       set = axeq(verb, vlen, "setvalue");
        wchar_t   want[128];
        int       n = wlen[0]==1 && word[0][0]=='-' ? 0
                    : MultiByteToWideChar(CP_UTF8, 0, word[0], wlen[0], want, 127);
        want[n > 0 ? n : 0] = 0;
        if (fs->GetPatternProvider(UIA_ValuePatternId, &u)!=S_OK || !u) {
            axlput(&l, "no Value pattern");
        } else if (set) {
            HRESULT r = ((IValueProvider *)u)->SetValue(want);
            if (r == S_OK) {
                axlput(&l, "ok");
            } else {
                axlput(&l, "refused ");
                axlputx(&l, (unsigned long)r);
            }
        } else {
            BSTR got = 0;
            ((IValueProvider *)u)->get_Value(&got);
            int glen = got ? (int)SysStringLen(got) : 0;
            if (glen==n && !memcmp(got, want, (size_t)n*2)) {
                axlput(&l, "ok");
            } else {
                axlput(&l, "is ");
                axlputw(&l, got ? got : L"", glen);
            }
            SysFreeString(got);
        }
        if (u) {
            u->Release();
        }
    } else if (axeq(verb, vlen, "textis") || axeq(verb, vlen, "textsel") ||
               axeq(verb, vlen, "selis") || axeq(verb, vlen, "textunit")) {
        // Through the Text pattern: ARG a word, '_' for a space
        //   textis TEXT          the document's text is TEXT
        //   textsel LO,HI        select units [LO, HI)
        //   selis LO,HI          the selection is [LO, HI)
        //   textunit U,N,TEXT    at the start, moved N units U (c w l p d),
        //                        then expanded to one, is TEXT
        wchar_t arg[128];
        int     n = MultiByteToWideChar(CP_UTF8, 0, word[0], wlen[0], arg, 127);
        n = n>0 ? n : 0;
        for (int i = 0; i < n; i++) {
            arg[i] = arg[i]=='_' ? ' ' : arg[i];
        }
        arg[n] = 0;
        int nums[2] = {};
        int nn = 0;
        wchar_t const *q = arg;
        for (; nn < 2 && *q>='0' && *q<='9'; nn++) {
            while (*q>='0' && *q<='9') nums[nn] = nums[nn]*10 + (*q++ - '0');
            q += *q == ',';
        }
        IUnknown *u = 0;
        if (fs->GetPatternProvider(UIA_TextPatternId, &u)!=S_OK || !u) {
            axlput(&l, "no Text pattern");
        } else {
            ITextProvider      *tp = (ITextProvider *)u;
            ITextRangeProvider *r  = 0;
            BSTR                got = 0;
            if (axeq(verb, vlen, "textis")) {
                tp->get_DocumentRange(&r);
                if (r) r->GetText(-1, &got);
            } else if (axeq(verb, vlen, "textsel")) {
                tp->get_DocumentRange(&r);
                HRESULT e = E_FAIL;
                if (r) {
                    int m;
                    r->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Document, -1, &m);
                    r->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, nums[1], &m);
                    r->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, nums[0], &m);
                    e = r->Select();
                }
                got = SysAllocString(e==S_OK ? L"ok" : L"refused");
                n   = 2;
                memcpy(arg, L"ok", 6);
            } else if (axeq(verb, vlen, "selis")) {
                SAFEARRAY *a = 0;
                tp->GetSelection(&a);
                LONG i = 0;
                if (a && a->rgsabound[0].cElements==1) {
                    SafeArrayGetElement(a, &i, &r);
                }
                if (a) SafeArrayDestroy(a);
                wchar_t b[32];
                int lo = r ? W32AxRange::of(r)->lo : -1, hi = r ? W32AxRange::of(r)->hi : -1;
                wsprintfW(b, L"%d,%d", lo, hi);
                got = SysAllocString(b);
            } else {
                // textunit: the unit's letter, the count, then the text
                TextUnit unit = arg[0]=='c' ? TextUnit_Character : arg[0]=='w' ? TextUnit_Word
                              : arg[0]=='l' ? TextUnit_Line : arg[0]=='p' ? TextUnit_Paragraph
                              : TextUnit_Document;
                wchar_t const *t = arg + 2;
                int count = 0;
                while (*t>='0' && *t<='9') count = count*10 + (*t++ - '0');
                t += *t == ',';
                tp->get_DocumentRange(&r);
                if (r) {
                    int m;
                    r->MoveEndpointByRange(TextPatternRangeEndpoint_End, r, TextPatternRangeEndpoint_Start);
                    r->Move(unit, count, &m);
                    r->ExpandToEnclosingUnit(unit);
                    r->GetText(-1, &got);
                }
                n = lstrlenW(t);
                memmove(arg, t, (size_t)(n+1)*2);
            }
            int glen = got ? (int)SysStringLen(got) : 0;
            if (glen==n && !memcmp(got, arg, (size_t)n*2)) {
                axlput(&l, "ok");
            } else {
                axlput(&l, "is ");
                axlputw(&l, got ? got : L"", glen);
            }
            SysFreeString(got);
            if (r) {
                r->Release();
            }
        }
        if (u) {
            u->Release();
        }
    } else if (axeq(verb, vlen, "hit")) {
        UiaRect r = {};
        f->get_BoundingRectangle(&r);
        IRawElementProviderFragment *at = 0;
        root->ElementProviderFromPoint(r.left + r.width/2, r.top + r.height/2, &at);
        W32AxProv *ap = at ? static_cast<W32AxProv *>(at) : 0;
        if (r.width<=0 || r.height<=0) {
            axlput(&l, "no rectangle");
        } else if (ap && ap->key==static_cast<W32AxProv *>(fs)->key) {
            axlput(&l, "ok");
        } else {
            axlput(&l, "another at its center");
        }
        if (at) {
            at->Release();
        }
    } else if (axeq(verb, vlen, "parent")) {
        long ptype = axtypeid(word[0], wlen[0]);
        IRawElementProviderFragment *up = 0;
        f->Navigate(NavigateDirection_Parent, &up);
        VARIANT v = {};
        if (up) {
            static_cast<W32AxProv *>(up)->GetPropertyValue(UIA_ControlTypePropertyId, &v);
            up->Release();
        }
        axlput(&l, v.vt==VT_I4 && v.lVal==ptype ? "ok" : "another parent");
    } else {
        axlput(&l, "unknown verb");
    }
    if (fs) {
        fs->Release();
    }
    f->Release();
    axout(out, cap, &l);
}


#else  // the core's side


// What the edge may log now: as the macOS adapter's maclog and
// macwithheld have it
static int hookaxlogging(void *p)
{
    Gui   *g  = (Gui *)p;
    AxGui *x  = g->ax;
    b32    read = PROSPERO_DEVTOOLS || (x && x->tracefd>=0);
    if (!x || !x->on || !read) {
        return 0;
    }
    AxText *t = &x->ax->text;
    return W32AXLOG_ON | (t->live && !(t->state & AXS_PASSWORD) ? W32AXLOG_TEXT : 0);
}

static void hookaxnote(void *p, char const *line, int len)
{
    Gui   *g = (Gui *)p;
    AxGui *x = g->ax;
    if (x && x->on) {
        Arena a = x->logmem;
        axnotemodel(x, a);
        axnote(x, a, {(u8 *)line, (iz)len});
    }
}

// The edge's copies of a11y.cpp's numbers
static_assert((i32)W32AX_ROOT==(i32)AXKEY_ROOT && (i32)AX_NONE==0 && (i32)AX_IMAGE==22 &&
              (i32)AX_NROLES==23);
static_assert((i32)AXS_FOCUSED==1 && (i32)AXS_DISABLED==2 && (i32)AXS_CHECKED==4 &&
              (i32)AXS_EXPANDED==8 && (i32)AXS_SELECTED==16 && (i32)AXS_OFFSCREEN==32 &&
              (i32)AXS_READONLY==64 && (i32)AXS_PASSWORD==128 && (i32)AXS_MODAL==256);
static_assert((i32)AXA_PRESS==1 && (i32)AXA_TOGGLE==2 && (i32)AXA_EXPAND==4 &&
              (i32)AXA_FOCUS==8 && (i32)AXA_EDIT==16 && (i32)AXA_SELECT==32 &&
              (i32)AXA_MENU==64);
static_assert((i32)W32AXACT_PRESS==(i32)AXACT_PRESS && (i32)W32AXACT_FOCUS==(i32)AXACT_FOCUS &&
              (i32)W32AXACT_MENU==(i32)AXACT_MENU);
static_assert((i32)W32AXE_DESTROYED==(i32)AXE_DESTROYED && (i32)W32AXE_CREATED==(i32)AXE_CREATED &&
              (i32)W32AXE_CHILDREN==(i32)AXE_CHILDREN && (i32)W32AXE_NAME==(i32)AXE_NAME &&
              (i32)W32AXE_VALUE==(i32)AXE_VALUE && (i32)W32AXE_STATE==(i32)AXE_STATE &&
              (i32)W32AXE_FOCUS==(i32)AXE_FOCUS && (i32)W32AXE_LAYOUT==(i32)AXE_LAYOUT);

// The published tree, while accessibility is on
static AxTree *w32axtree(void *p)
{
    Gui *g = (Gui *)p;
    return g->ax && g->ax->on ? g->ax->ax->pub : 0;
}

// The node with key, as tools see it: not hidden, and the first with
// its key (a later duplicate is the first's element)
static i32 w32axfind(AxTree *t, u32 key)
{
    i32 i = t ? axfind(t, key) : -1;
    return i>0 && t->nodes[i].role==AX_NONE ? -1 : i;
}

static int hookaxstart(void *p)
{
    Gui *g = (Gui *)p;
    if (!g->ax) {
        return 0;  // off
    }
    axstart(g);
    os_wake(g->os);
    return 1;
}

static int hookaxnode(void *p, unsigned key, W32AxNode *out)
{
    AxTree *t = w32axtree(p);
    i32     i = w32axfind(t, key);
    if (i<0 && t && key==AXKEY_ROOT) {
        *out = {};  // before the first tree: a bare root
        out->role = AX_APP;
        return 1;
    } else if (i < 0) {
        return 0;
    }
    AxNode *n = t->nodes + i;
    out->role    = n->role;
    out->state   = n->state;
    out->actions = n->actions;
    out->x0      = n->rect.x0;
    out->y0      = n->rect.y0;
    out->x1      = n->rect.x1;
    out->y1      = n->rect.y1;
    return 1;
}

static unsigned hookaxparent(void *p, unsigned key)
{
    AxTree *t = w32axtree(p);
    i32     i = w32axfind(t, key);
    if (i <= 0) {
        return 0;
    }
    i32 up = t->nodes[i].parent;
    while (up>0 && t->nodes[up].role==AX_NONE) {
        up = t->nodes[up].parent;
    }
    return up>0 ? t->nodes[up].key : AXKEY_ROOT;
}

static void w32axkids(AxTree *t, i32 i, unsigned *out, int cap, int *n, i32 depth)
{
    for (i32 c = t->nodes[i].child; c >= 0; c = t->nodes[c].next) {
        if (axfind(t, t->nodes[c].key) != c) {
            continue;  // a duplicate key
        } else if (t->nodes[c].role != AX_NONE) {
            if (*n < cap) {
                out[*n] = t->nodes[c].key;
            }
            ++*n;
        } else if (depth < 64) {
            w32axkids(t, c, out, cap, n, depth+1);
        }
    }
}

static int hookaxkids(void *p, unsigned key, unsigned *out, int cap)
{
    AxTree *t = w32axtree(p);
    i32     i = w32axfind(t, key);
    int     n = 0;
    if (i >= 0) {
        w32axkids(t, i, out, cap, &n, 0);
    }
    return n;
}

static int hookaxtext(void *p, unsigned key, int which, unsigned short *buf, int cap)
{
    AxTree *t = w32axtree(p);
    i32     i = w32axfind(t, key);
    if (i < 0) {
        return 0;
    }
    AxNode *n = t->nodes + i;
    Str     s = which==W32AXSTR_NAME ? n->name : which==W32AXSTR_HELP ? n->help : n->value;
    int     len = 0;
    for (iz o = 0; o < s.len;) {
        c16 u[2];
        iz  k = utf16encode(u, utf8decode(s, &o));
        for (iz j = 0; j < k; j++, len++) {
            if (len < cap) {
                buf[len] = u[j];
            }
        }
    }
    return len;
}

static unsigned hookaxfocus(void *p)
{
    AxTree *t = w32axtree(p);
    i32     i = t ? t->focus : -1;
    while (i>0 && t->nodes[i].role==AX_NONE) {
        i = t->nodes[i].parent;
    }
    return i>0 ? t->nodes[i].key : 0;
}

// Whether node a is in front of node b: of their ancestors where they
// part, the later sibling, which ImGui drew later, or the deeper
static b32 w32axabove(AxTree *t, i32 a, i32 b)
{
    enum { DEPTH = 64 };
    i32 pa[DEPTH], pb[DEPTH];
    i32 na = 0, nb = 0;
    for (i32 i = a; i>0 && na<DEPTH; i = t->nodes[i].parent) {
        pa[na++] = i;
    }
    for (i32 i = b; i>0 && nb<DEPTH; i = t->nodes[i].parent) {
        pb[nb++] = i;
    }
    while (na && nb && pa[na-1]==pb[nb-1]) {
        na--;
        nb--;
    }
    return na && nb ? pa[na-1]>pb[nb-1] : na>0;
}

static unsigned hookaxhit(void *p, float x, float y)
{
    AxTree *t    = w32axtree(p);
    i32     best = 0;
    for (i32 i = 1; t && i < t->len; i++) {
        AxNode *n = t->nodes + i;
        if (n->role!=AX_NONE && x>=n->rect.x0 && x<n->rect.x1 && y>=n->rect.y0 &&
                y<n->rect.y1 && axfind(t, n->key)==i && w32axabove(t, i, best)) {
            best = i;
        }
    }
    return t ? t->nodes[best].key : AXKEY_ROOT;
}

// A tool's action: logged, as macOS logs its presses
static int hookaxact(void *p, unsigned key, int kind)
{
    Gui *g  = (Gui *)p;
    b32  ok = axaction(g, key, kind);
    if (ok) {
        os_wake(g->os);
    }
    AxGui *x = g->ax;
    if (x && x->on && (PROSPERO_DEVTOOLS || x->tracefd>=0)) {
        AxTree *t = x->ax->pub;
        i32     i = w32axfind(t, key);
        Arena   a = x->logmem;
        Buf     b(&a, 160);
        print(&b, kind==AXACT_FOCUS ? Str("SetFocus ") : kind==AXACT_MENU ? Str("menu ")
                                                                          : Str("press "));
        if (i >= 0) {
            print(&b, axrolenames[t->nodes[i].role]);
            putbyte(&b, ' ');
            axlogstr(&b, t->nodes[i].name);
        } else {
            print(&b, "(gone)");
        }
        print(&b, ok ? Str(" -> ok") : Str(" -> refused"));
        Str line = finish(&b);
        axnote(x, a, line);
    }
    return ok;
}

// UI Automation's SetValue: the field's whole text, as AXValue's set is
// on macOS, for the focused editable field only
static int hookaxsetvalue(void *p, unsigned key, unsigned short const *s, int n)
{
    Gui   *g  = (Gui *)p;
    AxGui *x  = g->ax && g->ax->on ? g->ax : 0;
    b32    ok = 0;
    Arena  a  = x ? x->logmem : Arena{};
    Str    text = {};
    if (x && x->ax->text.live && x->ax->text.key==key) {
        text = fromwide(&a, (c16 *)s, n);
        ok   = axtextedit(g, 0, axtextlen(x->ax), text, AX_AFTER, AX_AFTER);
    }
    if (ok) {
        os_wake(g->os);
    }
    if (x && x->on && (PROSPERO_DEVTOOLS || x->tracefd>=0)) {
        Buf b(&a, 64 + text.len);
        print(&b, "SetValue ");
        if (ok) {  // the field took it: no password
            axlogstr(&b, text);
        } else {
            print(&b, "(withheld)");
        }
        print(&b, ok ? Str(" -> ok") : Str(" -> refused"));
        Str line = finish(&b);
        axnote(x, a, line);
    }
    return ok;
}

// The Text pattern's: the focused field's document in UTF-16 units (the
// text with marked text, if any, in place of the selection), or another
// field's value.

static A11y *w32axlive(void *p, unsigned key)
{
    Gui   *g = (Gui *)p;
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    return x && x->ax->text.live && x->ax->text.key==key ? x->ax : 0;
}

static i32 w32axto16(A11y *x, i32 d)  // document byte to unit
{
    return axdocunit(x, AXU_UTF16, d);
}

static int hookaxtextget(void *p, unsigned key, W32AxText *info, unsigned short *buf, int cap)
{
    *info = {};
    A11y *x = w32axlive(p, key);
    if (!x) {
        return hookaxtext(p, key, W32AXSTR_VALUE, buf, cap);
    }
    AxText *t = &x->text;
    info->live     = 1;
    info->readonly = !axeditable(t);
    info->anchor   = w32axto16(x, axtodoc(x, t->anchor));
    info->caret    = w32axto16(x, axtodoc(x, t->caret));
    Arena a   = ((Gui *)p)->ax->logmem;
    Str   s   = axdoc(x, &a);
    int   len = 0;
    for (iz o = 0; o < s.len;) {
        c16 u[2];
        iz  k = utf16encode(u, utf8decode(s, &o));
        for (iz j = 0; j < k; j++, len++) {
            if (len < cap) {
                buf[len] = u[j];
            }
        }
    }
    return len;
}

static int hookaxtextlines(void *p, unsigned key, int *starts, int cap)
{
    A11y *x = w32axlive(p, key);
    if (!x) {
        return 0;
    }
    i32  n;
    i32 *lines = axlines(x, &n);
    for (i32 k = 0; k<n && k<cap; k++) {
        starts[k] = w32axto16(x, axtodoc(x, lines[k]));
    }
    return n;
}

static int hookaxtextrect(void *p, unsigned key, int lo, int hi, float *r)
{
    A11y *x = w32axlive(p, key);
    if (!x || x->text.geom.rev!=x->text.rev) {
        return 0;  // none, or of the text before an edit (stale)
    }
    AxSpan d = {axdocbyte(x, AXU_UTF16, lo), axdocbyte(x, AXU_UTF16, hi)};
    AxRect b = axdocbounds(x, d);
    r[0] = b.x0;
    r[1] = b.y0;
    r[2] = b.x1;
    r[3] = b.y1;
    return 1;
}

static int hookaxtextat(void *p, unsigned key, float px, float py)
{
    A11y *x = w32axlive(p, key);
    if (!x || x->text.geom.rev!=x->text.rev) {
        return -1;
    }
    return w32axto16(x, axdocat(x, px, py));
}

static int hookaxtextselect(void *p, unsigned key, int anchor, int caret)
{
    Gui  *g = (Gui *)p;
    A11y *x = w32axlive(p, key);
    if (!x) {
        return 0;
    }
    i32 a  = axdoctext(x, axdocbyte(x, AXU_UTF16, anchor));
    i32 c  = axdoctext(x, axdocbyte(x, AXU_UTF16, caret));
    b32 ok = axtextselect(g, a, c);
    if (ok) {
        os_wake(g->os);
    }
    AxGui *ag = g->ax;
    if (PROSPERO_DEVTOOLS || ag->tracefd>=0) {
        Arena l = ag->logmem;
        Buf   b(&l, 64);
        print(&b, "Select ");
        print(&b, (i64)anchor);
        putbyte(&b, ',');
        print(&b, (i64)caret);
        print(&b, ok ? Str(" -> ok") : Str(" -> refused"));
        Str line = finish(&b);
        axnote(ag, l, line);
    }
    return ok;
}

// A text service's write through the TSF store: units [lo, hi) of the
// document replaced, logged with its text, as SetValue is
static int hookaxtextedit(void *p, unsigned key, int lo, int hi, unsigned short const *s, int n)
{
    Gui   *g  = (Gui *)p;
    A11y  *x  = w32axlive(p, key);
    AxGui *ag = g->ax;
    b32    ok = 0;
    Arena  a  = ag ? ag->logmem : Arena{};
    Str    text = {};
    if (x && lo>=0 && lo<=hi && n>=0) {
        i32 blo = axdoctext(x, axdocbyte(x, AXU_UTF16, lo));
        i32 bhi = axdoctext(x, axdocbyte(x, AXU_UTF16, hi));
        text = fromwide(&a, (c16 *)s, n);
        ok   = axtextedit(g, blo, bhi, text, AX_AFTER, AX_AFTER);
    }
    if (ok) {
        os_wake(g->os);
    }
    if (ag && ag->on && (PROSPERO_DEVTOOLS || ag->tracefd>=0)) {
        Buf b(&a, 80 + text.len);
        print(&b, "TSF edit ");
        print(&b, (i64)lo);
        putbyte(&b, ',');
        print(&b, (i64)hi);
        putbyte(&b, ' ');
        if (ok) {
            axlogstr(&b, text);
        } else {
            print(&b, "(withheld)");
        }
        print(&b, ok ? Str(" -> ok") : Str(" -> refused"));
        Str line = finish(&b);
        axnote(ag, a, line);
    }
    return ok;
}

// After each publish: its events to the edge, in batches
static void w32axpublished(AxGui *x)
{
    AxTree *t = x->ax->pub;
    void   *w = x->g->os->gui;
    if (!w) {
        return;
    }
    W32AxEvent ev[256];
    iz         e = 0;
    do {
        int n = 0;
        for (; e<t->events.len && n<countof(ev); e++, n++) {
            ev[n].key  = t->events[e].key;
            ev[n].kind = t->events[e].kind;
        }
        w32gui_axevents(w, ev, n);
    } while (e < t->events.len);

    // The focused field's text or selection changed, as a tool reading
    // it through the Text pattern is told
    static u32 tkey;
    static i64 trev;
    static i32 tanchor, tcaret;
    AxText *tx = &x->ax->text;
    if (tx->live) {
        b32 edited = tx->rev != trev;
        if (tx->key==tkey && (edited || tx->anchor!=tanchor || tx->caret!=tcaret)) {
            W32AxEvent te[2];
            int        n = 0;
            if (edited) {
                te[n++] = {tx->key, W32AXE_TEXTEDIT};
            }
            te[n++] = {tx->key, W32AXE_TEXTSEL};
            w32gui_axevents(w, te, n);
        }
        tkey    = tx->key;
        trev    = tx->rev;
        tanchor = tx->anchor;
        tcaret  = tx->caret;
    } else {
        tkey = 0;
    }
}

// The checklist's "uia ...": the edge's, through its providers
static void w32axdrive(Gui *g, Arena scratch, Str cmd)
{
    Arena a = scratch;
    char  out[256];
    if (g->os->gui) {
        w32gui_axdrive(g->os->gui, (char const *)cmd.data, (int)cmd.len, out, countof(out));
    } else {
        out[0] = 0;
    }
    Str got = {(u8 *)out, (iz)__builtin_strlen(out)};
    Buf b(&a, 128);
    print(&b, "uia ");
    print(&b, cmd);
    print(&b, ": ");
    print(&b, got.len ? got : Str("no window"));
    axcheck(g, &a, got==Str("ok"), finish(&b));
}

static void w32axhooks(W32AxHooks *h)
{
    h->logging = hookaxlogging;
    h->note    = hookaxnote;
    h->start   = hookaxstart;
    h->node    = hookaxnode;
    h->parent  = hookaxparent;
    h->kids    = hookaxkids;
    h->text    = hookaxtext;
    h->focus   = hookaxfocus;
    h->hit     = hookaxhit;
    h->act     = hookaxact;
    h->setvalue   = hookaxsetvalue;
    h->textget    = hookaxtextget;
    h->textlines  = hookaxtextlines;
    h->textrect   = hookaxtextrect;
    h->textat     = hookaxtextat;
    h->textselect = hookaxtextselect;
    h->textedit   = hookaxtextedit;
}

// At the window's opening, with accessibility not off
static void w32axinit(Gui *g)
{
    if (g->ax) {
        g->ax->published = w32axpublished;
        g->ax->osdrive   = w32axdrive;
        g->ax->osverb    = "uia";
    }
}

#endif
