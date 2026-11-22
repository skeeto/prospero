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
enum { W32SRC_UNKNOWN, W32SRC_HARDWARE, W32SRC_INJECTED, W32SRC_SYSTEM };

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
         : s.origin==4 ? W32SRC_SYSTEM   : W32SRC_UNKNOWN;
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
        if (src == W32SRC_HARDWARE) {
            return;
        }
        if (src==W32SRC_UNKNOWN && !(msg==WM_CHAR && wp>=0xd800 && wp<0xe000) &&
                !(msg==WM_KEYDOWN && (wp==VK_PACKET || wp==VK_PROCESSKEY))) {
            // Posted (the checklist's), or an old system: as typing,
            // unless it is what only a program or an input method sends
            return;
        }
        if (msg==WM_CHAR || msg==WM_UNICHAR) {
            axlput(&l, msg==WM_CHAR ? "WM_CHAR " : "WM_UNICHAR ");
            if (text && msg==WM_CHAR) {
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

static void w32axhooks(W32AxHooks *h)
{
    h->logging = hookaxlogging;
    h->note    = hookaxnote;
}

#endif
