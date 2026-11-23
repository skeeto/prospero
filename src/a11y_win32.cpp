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

static W32AxProv *axprov(W32Gui *w, unsigned key);

struct W32AxProv : IRawElementProviderSimple, IRawElementProviderFragment,
                   IRawElementProviderFragmentRoot, IInvokeProvider, IToggleProvider,
                   IExpandCollapseProvider, ISelectionItemProvider, IValueProvider {
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

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown **out) override
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
        }
        if (p) {
            p->AddRef();
            *out = p;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT *out) override
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
            out->boolVal = n.actions & (W32A_FOCUS|W32A_EDIT) ? VARIANT_TRUE : VARIANT_FALSE;
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

// The window goes: every provider with it
static void w32axgone(W32Gui *w)
{
    if (!w32ax.dll || axw!=w) {
        return;
    }
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
    *out = w32ax.ret(w->hwnd, wp, lp, static_cast<IRawElementProviderSimple *>(r));
    return 1;
}

static void axraiseprop(W32AxProv *p, PROPERTYID id, VARIANT now)
{
    VARIANT old = {};
    old.vt = VT_EMPTY;
    w32ax.raiseprop(static_cast<IRawElementProviderSimple *>(p), id, old, now);
}

static void axraisebool(W32AxProv *p, PROPERTYID id, int on)
{
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
};

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
    int want = axeq(verb, vlen, "start") ? 0
             : axeq(verb, vlen, "can") || axeq(verb, vlen, "parent") ||
               axeq(verb, vlen, "value") || axeq(verb, vlen, "setvalue") ? 2 : 1;
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
    h->setvalue = hookaxsetvalue;
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
