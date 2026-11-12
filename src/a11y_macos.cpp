// Accessibility: macOS's side
//
// The tree (a11y.cpp) to NSAccessibility, which Voice Control and
// VoiceOver ask: an element for each node, under the window, as the
// section "The tree" below has it, and a text field's as an NSTextView's,
// which a tool reads and edits by ranges ("A text field's own").
//
// SDL's text input view (SDL3TranslatorResponder, its NSTextInputClient)
// is what Dictation and the input methods talk to: while the focused
// field is in the text model, its methods answer from the model and edit
// through it, and typing keeps SDL's way ("SDL's NSTextInputClient").
//
// And a trace: each call these tools make, to the text input and through
// NSAccessibility to the window, its content view (SDL3View) and the
// text input, is logged (axnote, a11y_imgui.cpp) with its arguments and
// its answer. That is only while the tree is built (a tool asked,
// `[ui] accessibility = on`, or PROSPERO_AXTRACE) and something can read
// the log (a Debug build's Metrics pane, or the trace). Text is logged
// only from a field the model knows is not a password, and a key's
// text, which reaches SDL through these same calls, only as its length:
// the trace is of speech tools, not typing.
//
// SDL's NSTextInputClient methods are its own, so method_setImplementation
// replaces them, and the original is called. NSAccessibility's are
// AppKit's, inherited: changing those would change every view, so the
// three classes get methods of their own (class_addMethod) that log and
// call the superclass's. Both are per class: SDL makes a new text input
// view at each Stop/StartTextInput, which the methods serve all the same.
//
// It all runs on the main thread, between frames or inside them (an
// IME's queries as text input starts), and never touches ImGui. An
// Objective-C exception can't unwind through C++, so nothing here sends
// a message that could throw: strings are read, not ranged into, and a
// range given to SDL is checked first.
//
// This is free and unencumbered software released into the public domain.

extern "C" void *class_getSuperclass(void *);
extern "C" void *class_getMethodImplementation(void *, void *);
extern "C" char const *method_getTypeEncoding(void *);
extern "C" void *method_getName(void *);
extern "C" void **class_copyMethodList(void *, unsigned *);
extern "C" char const *sel_getName(void *);
extern "C" char const *class_getName(void *);

static AxGui *axmac;  // the window's, for methods that have only self

// Whether to log: only while the tree is built, and only where the log
// is read, so that a release build without a trace keeps nothing.
static AxGui *maclog()
{
    b32 read = PROSPERO_DEVTOOLS || (axmac && axmac->tracefd>=0);
    return axmac && axmac->on && read ? axmac : 0;
}

// Why a text argument mustn't be logged, or null if it may: only text
// for a field the model knows, and not a password. The model changes at
// frame end, so a call may be for a field it doesn't know yet (focus
// just moved): unknown is withheld.
static char const *macwithheld()
{
    AxText *t = &axmac->ax->text;
    return !t->live                ? "(no field)"
         : t->state & AXS_PASSWORD ? "(password)"
         :                           0;
}

// Whether a key event is being dispatched: its text comes to SDL's
// NSTextInputClient through interpretKeyEvents:. Counted by macsend
// (gui_sdl.cpp), or, if that isn't installed, AppKit's current event,
// which may be a key already handled: then speech is withheld too.
static b32 mackeyed()
{
    if (macsendevent) {
        return mackeydepth > 0;
    }
    void *app = objcsend<void *>(objc_getClass("NSApplication"), "sharedApplication");
    void *ev  = app ? objcsend<void *>(app, "currentEvent") : 0;
    return ev && objcsend<uz>(ev, "type")==NSKEYDOWN;
}

// The call b holds: the text model first, if it changed since. a is the
// arena b grew in, copied once b is done, so past it.
static void macnote(AxGui *x, Arena a, Buf *b)
{
    Str line = finish(b);
    axnotemodel(x, a);
    axnote(x, a, line);
}

static void macrange(Buf *b, NsRange r)
{
    putbyte(b, '{');
    if (r.loc == NSNOTFOUND) {
        print(b, "NotFound");
    } else {
        print(b, (i64)r.loc);
    }
    putbyte(b, ',');
    print(b, (i64)r.len);
    putbyte(b, '}');
}

static void macrect(Buf *b, NsRect r)
{
    putbyte(b, '{');
    print(b, axround((f32)r.x));
    putbyte(b, ',');
    print(b, axround((f32)r.y));
    putbyte(b, ' ');
    print(b, axround((f32)r.w));
    putbyte(b, 'x');
    print(b, axround((f32)r.h));
    putbyte(b, '}');
}

static Str macutf8(void *ns)
{
    char const *s = cstring(ns);
    return {(u8 *)s, s ? (iz)__builtin_strlen(s) : 0};
}

// An object as it describes itself, its runs of white space as one
// space, cut after max bytes.
static void macdesc(Buf *b, void *o, iz max)
{
    if (!o) {
        print(b, "nil");
        return;
    }
    Str s    = macutf8(objcsend<void *>(o, "description"));
    iz  n    = axcutat(s, max);
    b32 gap  = 0;
    for (iz i = 0; i < n; i++) {
        u8 c = s[i];
        if (c==' ' || c=='\n' || c=='\t' || c=='\r') {
            gap = 1;
            continue;
        }
        if (gap) {
            putbyte(b, ' ');
            gap = 0;
        }
        putbyte(b, c);
    }
    if (n < s.len) {
        print(b, "\xe2\x80\xa6");
    }
}

static b32 maciskind(void *o, char const *cls)
{
    void *c = objc_getClass(cls);
    return o && c && objcsend<ObjcBool>(o, "isKindOfClass:", c);
}

// A text argument, NSString or NSAttributedString: quoted, its length in
// UTF-16 units after '#', which ranges count, and an attributed string's
// runs of attributes (Dictation's alternatives, an input method's
// clauses) after.
static void macstrarg(Buf *b, void *o)
{
    if (!o) {
        print(b, "nil");
        return;
    }
    b32   attr = maciskind(o, "NSAttributedString");
    void *s    = attr ? objcsend<void *>(o, "string") : o;
    if (!maciskind(s, "NSString")) {
        macdesc(b, o, 200);
        return;
    }
    uz          units = objcsend<uz>(s, "length");
    char const *why   = macwithheld();
    Str         text  = why ? Str{} : macutf8(s);
    if (why) {
        print(b, Str{(u8 *)why, (iz)__builtin_strlen(why)});
    } else if (!text.data && units) {
        print(b, "(not UTF-8)");
    } else {
        axlogstr(b, text);
    }
    putbyte(b, '#');
    print(b, (i64)units);
    if (!attr || why) {
        return;  // Dictation's alternatives describe themselves in full
    }
    print(b, " attrs[");
    i32 runs = 0;
    for (uz at = 0; at<units && runs<8; runs++) {
        NsRange r = {};
        void   *d = objcsend<void *>(o, "attributesAtIndex:effectiveRange:", at, &r);
        if (r.len==0 || r.loc+r.len<=at || r.loc>at) {
            break;  // as documented never, but a loop must end
        }
        if (runs) {
            putbyte(b, ' ');
        }
        macrange(b, r);
        putbyte(b, ' ');
        macdesc(b, d, 300);
        at = r.loc + r.len;
    }
    putbyte(b, ']');
}


// NSAccessibility on the window, its content view and the text input,
// through the protocol's older, attribute-keyed methods, which AppKit
// answers a remote query with. The classes' own methods call their
// superclass's, looked up at each call, not at install: AppKit's may be
// replaced or added to later, as when an assistive tool's bundle loads.
// What the tree answers itself (the content view's hit test and focus,
// the text input's focus) it added first, and these leave be.

enum MacAxSig : i32 {
    MAXS_ID,       // id f()
    MAXS_IDID,     // id f(id)
    MAXS_BOOLID,   // BOOL f(id)
    MAXS_VOIDID,   // void f(id)
    MAXS_VOIDIDID, // void f(id, id)
    MAXS_IDIDID,   // id f(id, id)
    MAXS_IDPOINT,  // id f(NSPoint)
};

static struct { char const *sel; MacAxSig sig; } const macaxsels[] = {
    {"accessibilityAttributeNames",                          MAXS_ID},
    {"accessibilityAttributeValue:",                         MAXS_IDID},
    {"accessibilityIsAttributeSettable:",                    MAXS_BOOLID},
    {"accessibilitySetValue:forAttribute:",                  MAXS_VOIDIDID},
    {"accessibilityParameterizedAttributeNames",             MAXS_ID},
    {"accessibilityParameterizedAttributeValue:forParameter:", MAXS_IDIDID},
    {"accessibilityActionNames",                             MAXS_ID},
    {"accessibilityPerformAction:",                          MAXS_VOIDID},
    {"accessibilityHitTest:",                                MAXS_IDPOINT},
    {"accessibilityFocusedUIElement",                        MAXS_ID},
};

static char const *const macaxclasses[] = {
    "SDL3Window", "SDL3View", "SDL3TranslatorResponder",
};

struct MacAxImp {
    void *cls;
    void *sel;
};

static MacAxImp macaximps[countof(macaxclasses) * countof(macaxsels)];
static i32      nmacaximps;

static void *macaxsuper(void *self, void *sel, char const **cls)
{
    for (void *c = object_getClass(self); c; c = class_getSuperclass(c)) {
        for (i32 i = 0; i < nmacaximps; i++) {
            if (macaximps[i].cls==c && macaximps[i].sel==sel) {
                *cls = class_getName(c);
                return class_getMethodImplementation(class_getSuperclass(c), sel);
            }
        }
    }
    return 0;  // not reached: only these classes have these methods
}

// Begin a line: the class (less its "SDL3"), the selector, its argument.
static void macaxhead(Buf *b, char const *cls, void *sel)
{
    Str c = {(u8 *)cls, (iz)__builtin_strlen(cls)};
    print(b, startswith(c, "SDL3") ? cuthead(c, 4) : c);
    putbyte(b, ' ');
    char const *s = sel_getName(sel);
    print(b, Str{(u8 *)s, (iz)__builtin_strlen(s)});
}

static void macaxarg(Buf *b, void *o)
{
    putbyte(b, ' ');
    if (maciskind(o, "NSString")) {
        Str s = macutf8(o);  // an attribute's or action's name, any client's
        iz  n = axcutat(s, 200);
        print(b, takehead(s, n));
        if (n < s.len) {
            print(b, "\xe2\x80\xa6");
        }
    } else {
        macdesc(b, o, 200);
    }
}

static void macaxresult(AxGui *x, Arena *a, Buf *b, void *r)
{
    print(b, " -> ");
    macdesc(b, r, 300);
    macnote(x, *a, b);
}

static void *macaxid(void *self, void *sel)
{
    char const *cls = "";
    void *r = ((void *(*)(void *, void *))macaxsuper(self, sel, &cls))(self, sel);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        macaxhead(&b, cls, sel);
        macaxresult(x, &a, &b, r);
    }
    return r;
}

static void *macaxidid(void *self, void *sel, void *arg)
{
    char const *cls = "";
    void *r = ((void *(*)(void *, void *, void *))macaxsuper(self, sel, &cls))(self, sel, arg);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        macaxhead(&b, cls, sel);
        macaxarg(&b, arg);
        macaxresult(x, &a, &b, r);
    }
    return r;
}

static ObjcBool macaxboolid(void *self, void *sel, void *arg)
{
    char const *cls = "";
    ObjcBool r = ((ObjcBool (*)(void *, void *, void *))macaxsuper(self, sel, &cls))(self, sel, arg);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 128);
        macaxhead(&b, cls, sel);
        macaxarg(&b, arg);
        print(&b, r ? Str(" -> YES") : Str(" -> NO"));
        macnote(x, a, &b);
    }
    return r;
}

static void macaxvoidid(void *self, void *sel, void *arg)
{
    char const *cls = "";
    void (*f)(void *, void *, void *) = (void (*)(void *, void *, void *))macaxsuper(self, sel, &cls);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 128);
        macaxhead(&b, cls, sel);
        macaxarg(&b, arg);
        macnote(x, a, &b);
    }
    f(self, sel, arg);
}

static void macaxvoididid(void *self, void *sel, void *v, void *attr)
{
    char const *cls = "";
    void (*f)(void *, void *, void *, void *) =
        (void (*)(void *, void *, void *, void *))macaxsuper(self, sel, &cls);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        macaxhead(&b, cls, sel);
        putbyte(&b, ' ');
        if (maciskind(v, "NSString") || maciskind(v, "NSAttributedString")) {
            macstrarg(&b, v);
        } else {
            macdesc(&b, v, 300);
        }
        macaxarg(&b, attr);
        macnote(x, a, &b);
    }
    f(self, sel, v, attr);
}

static void *macaxidid2(void *self, void *sel, void *attr, void *param)
{
    char const *cls = "";
    void *r = ((void *(*)(void *, void *, void *, void *))
               macaxsuper(self, sel, &cls))(self, sel, attr, param);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        macaxhead(&b, cls, sel);
        macaxarg(&b, attr);
        putbyte(&b, ' ');
        macdesc(&b, param, 200);
        macaxresult(x, &a, &b, r);
    }
    return r;
}

static void *macaxidpoint(void *self, void *sel, NsPoint p)
{
    char const *cls = "";
    void *r = ((void *(*)(void *, void *, NsPoint))macaxsuper(self, sel, &cls))(self, sel, p);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        macaxhead(&b, cls, sel);
        print(&b, " (");
        print(&b, axround((f32)p.x));
        putbyte(&b, ',');
        print(&b, axround((f32)p.y));
        putbyte(&b, ')');
        macaxresult(x, &a, &b, r);
    }
    return r;
}

static void *const macaxwrappers[] = {
    (void *)macaxid, (void *)macaxidid, (void *)macaxboolid, (void *)macaxvoidid,
    (void *)macaxvoididid, (void *)macaxidid2, (void *)macaxidpoint,
};

// The method cls itself has for sel, not one it inherits, or null.
static void *macownmethod(void *cls, char const *name)
{
    void    *sel = sel_registerName(name);
    unsigned n   = 0;
    void   **ms  = class_copyMethodList(cls, &n);
    void    *m   = 0;
    for (unsigned i = 0; !m && i < n; i++) {
        m = method_getName(ms[i])==sel ? ms[i] : 0;
    }
    free(ms);
    return m;
}

template<typename F>
static void macwrap(void *cls, char const *name, F wrapper, F *orig)
{
    void *m = macownmethod(cls, name);
    if (m) {
        *orig = (F)method_setImplementation(m, (void *)wrapper);
    }
}


// The tree, to NSAccessibility: an element for each node of the
// published tree that a tool has asked about, of a class made at run
// time from NSAccessibilityElement, which holds only its node's key and
// looks the node up in the published tree at each call, so it answers
// for the latest frame and never holds a pointer into a tree. The
// elements are kept by key until their node leaves the tree, when
// AppKit is told (UIElementDestroyed) and they are let go, marked dead
// for any tool still holding one.
//
// SDL3View, the window's content view, stays ignored, as AppKit has
// it, so the window's children are the root's: it gains its children,
// hit test and focus (class_addMethod, as NSView's are inherited), and
// the text input view its focus, which AppKit asks as first responder.
// The first of those calls starts the tree (axstart) with
// `[ui] accessibility = auto`: it answers a bare root then, and AppKit
// is told the layout changed once the first tree is published.
//
// ImGui coordinates are the content view's points, from its top left;
// the view isn't flipped, so y is turned over, then converted through
// the window to the screen, at each call, since the window moves with
// no frame drawn.
//
// Actions are queued through the glue (axaction), which ImGui takes at
// the next frame; the wake is ours, as the call came between frames,
// and the GUI may be waiting. Getters are only counted, as a tool reads
// whole subtrees: their counts are logged once per frame.

extern "C" bool  class_addIvar(void *, char const *, uz, u8, char const *);
extern "C" void *class_getInstanceVariable(void *, char const *);
extern "C" iz    ivar_getOffset(void *);
extern "C" char const *ivar_getTypeEncoding(void *);
extern "C" void  NSAccessibilityPostNotification(void *, void *);
extern "C" void  NSAccessibilityPostNotificationWithUserInfo(void *, void *, void *);
extern "C" void *NSAccessibilityUnignoredAncestor(void *);
extern "C" void *method_getImplementation(void *);
#if !__arm64__
extern "C" void  objc_msgSend_stret();
#endif

// A message whose answer is an NSRect, which x86-64 returns in memory
template<typename... A>
static NsRect objcrect(void *o, char const *sel, A... a)
{
    using F = NsRect (*)(void *, void *, A...);
    #if __arm64__
    return ((F)(void *)objc_msgSend)(o, sel_registerName(sel), a...);
    #else
    return ((F)(void *)objc_msgSend_stret)(o, sel_registerName(sel), a...);
    #endif
}

// What the elements are asked, counted for the log
enum : i32 {
    MG_ROLE,   // role, subrole
    MG_NAME,   // label, title, help
    MG_VALUE,
    MG_STATE,  // enabled, focused, selected, expanded, allowed selectors
    MG_TREE,   // parent, children, window
    MG_FRAME,
    MG_HIT,
    MG_FOCUS,  // the focused element
    MG_TEXT,   // a text field's selection, length, lines
    MG_N
};

static char const *const macgetnames[MG_N] = {
    "role", "name", "value", "state", "tree", "frame", "hit", "focus", "text",
};

// What AppKit is told, counted for the checklist
enum : i32 {
    MN_FOCUS,
    MN_VALUE,
    MN_SELTEXT,
    MN_TITLE,
    MN_CREATED,
    MN_DESTROYED,
    MN_LAYOUT,
    MN_N
};

static char const *const macnotes[MN_N] = {
    "AXFocusedUIElementChanged", "AXValueChanged", "AXSelectedTextChanged",
    "AXTitleChanged", "AXCreated", "AXUIElementDestroyed", "AXLayoutChanged",
};

enum : i64 {
    MACLAYOUT_GAP = 250000,  // µs between layout changes told in a nested loop
};

struct MacEl {
    u32   key;   // 0: empty
    u32   born;  // the publish that made it for a tool, or 0
    void *el;
};

static struct {
    void   *cls;         // ProsperoAxElement
    iz      keyoff;      // its ivars: the node's key,
    iz      deadoff;     // and whether that node is gone (MACDEAD_*)
    Arena   mem;         // the tables, the latest last
    MacEl  *tab;         // key -> element, open addressing
    i32     exp;
    i32     len;
    b32     rootasked;   // a tool has the root's children
    b32     started;     // a query started the tree: say so once published
    b32     layoutowed;  // a layout change waits for its gap
    i64     layoutat;
    u32     tkey;        // the text field as last told: which,
    i64     trev;        // its text,
    i32     tanchor;     // its selection,
    i32     tcaret;
    u64     tmarked;     // and its marked text, hashed,
    i32     tmlo, tmhi;  // with the input method's selection in it
    u32     pass;        // publishes told, the one telling while inpass
    b32     inpass;
    b32     startwason;  // for "ns start": the tree was on already,
    i32     startposts;  // and layout told for a start since "ns mark"
    i32     gets[MG_N];
    i32     posted[MN_N];
    i32     seltype;     // the last AXSelectedTextChanged's change type, or -1
    i32     alive;       // elements not yet deallocated
    void   *kept;        // the checklist's element, kept past its node
    b32     resizing;    // the checklist's: every publish changes layout,
    i32     modalposts;  // as a live resize's does; told in a nested loop,
    i32     modalheld;   // or held for the gap,
    i64     modal0;      // the first and last publish in one
    i64     modal1;
} macax;

enum : u32 {
    MACDEAD_LIVE,
    MACDEAD_GONE,
    MACDEAD_DYING,  // gone, while AppKit is told so: still an element
};

static u32 *mackeyp(void *el)
{
    return (u32 *)((byte *)el + macax.keyoff);
}

static u32 *macdeadp(void *el)
{
    return (u32 *)((byte *)el + macax.deadoff);
}

static void macgrow()
{
    MacEl *old  = macax.tab;
    i32    oldn = old ? 1<<macax.exp : 0;
    macax.exp   = old ? macax.exp+1 : 10;
    macax.tab   = alloc<MacEl>(&macax.mem, (iz)1<<macax.exp);
    u64 mask = ((u64)1 << macax.exp) - 1;
    for (i32 i = 0; i < oldn; i++) {
        if (old[i].key) {
            u64 h = axhash(old[i].key, macax.exp);
            while (macax.tab[h].key) {
                h = (h+1) & mask;
            }
            macax.tab[h] = old[i];
        }
    }
}

// The slot holding key, or -1
static i32 macslot(u32 key)
{
    if (!macax.tab || !key) {
        return -1;
    }
    u64 mask = ((u64)1 << macax.exp) - 1;
    for (u64 h = axhash(key, macax.exp);; h = (h+1) & mask) {
        if (!macax.tab[h].key) {
            return -1;
        } else if (macax.tab[h].key == key) {
            return (i32)h;
        }
    }
}

// Empty slot h, moving back what probed past it
static void macunslot(i32 h)
{
    u64 mask = ((u64)1 << macax.exp) - 1;
    u64 hole = (u64)h;
    macax.tab[hole] = {};
    for (u64 i = (hole+1) & mask; macax.tab[i].key; i = (i+1) & mask) {
        u64 home = axhash(macax.tab[i].key, macax.exp);
        if (((i - home) & mask) >= ((i - hole) & mask)) {
            macax.tab[hole] = macax.tab[i];
            macax.tab[i]    = {};
            hole = i;
        }
    }
    macax.len--;
}

// The live node an element stands for, or null: its index in at
static AxNode *macnodeof(void *el, i32 *at = 0)
{
    if (!el || !axmac || *macdeadp(el)) {
        return 0;
    }
    AxTree *t = axmac->ax->pub;
    i32     i = axfind(t, *mackeyp(el));
    if (i<=0 || t->nodes[i].role==AX_NONE) {
        return 0;
    }
    if (at) {
        *at = i;
    }
    return t->nodes + i;
}

// The element for node i, made if it has none
static void *macelof(i32 i)
{
    u32 key = axmac->ax->pub->nodes[i].key;
    i32 s   = macslot(key);
    if (s >= 0) {
        return macax.tab[s].el;
    }
    if (!macax.tab || (macax.len+1)*2 > 1<<macax.exp) {
        macgrow();
    }
    void *el = objcsend<void *>(objcsend<void *>(macax.cls, "alloc"), "init");
    if (!el) {
        return 0;
    }
    *mackeyp(el)  = key;
    *macdeadp(el) = MACDEAD_LIVE;
    macax.alive++;
    u64 mask = ((u64)1 << macax.exp) - 1;
    u64 h    = axhash(key, macax.exp);
    while (macax.tab[h].key) {
        h = (h+1) & mask;
    }
    macax.tab[h] = {key, macax.inpass ? macax.pass : 0, el};
    macax.len++;
    return el;
}

// The element for node i if a tool was given it, or null
static void *macelif(i32 i)
{
    i32 s = macslot(axmac->ax->pub->nodes[i].key);
    return s<0 ? 0 : macax.tab[s].el;
}

static void *macwindow()
{
    GuiSdl *p = axmac ? (GuiSdl *)axmac->g->os->gui : 0;
    if (!p || !p->window) {
        return 0;
    }
    return SDL_GetPointerProperty(SDL_GetWindowProperties(p->window),
                                  SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, 0);
}

static void *macview()
{
    void *win = macwindow();
    return win ? objcsend<void *>(win, "contentView") : 0;
}

// Where the root's children say their parent is: the view's first
// ancestor that isn't ignored, the window
static void *macroot()
{
    void *view = macview();
    return view ? NSAccessibilityUnignoredAncestor(view) : 0;
}

static void macwake()
{
    os_wake(axmac->g->os);
}

// Whether the tree is there to answer from; if not, start it, for the
// next frame, as a tool has asked (`auto`). On isn't enough: until the
// first publish the published tree is empty, without even a root.
static b32 macready(char const *why)
{
    AxGui *x = axmac;
    if (x->on && x->ax->pub->len>0) {
        return 1;
    } else if (x->on) {
        macax.started = 1;  // tell AppKit once there is a tree
        macwake();
        return 0;
    }
    axstart(x->g);
    macax.started = 1;
    macwake();
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 128);
        print(&b, "started by ");
        print(&b, Str{(u8 *)why, (iz)__builtin_strlen(why)});
        axnote(l, a, finish(&b));
    }
    return 0;
}

// An NSString of s, its scratch arena past s if s is in it
static void *macnsstr(Str s, Arena a)
{
    void *r = nsstringn(s);
    if (!r && s.len) {
        // Not UTF-8: each bad byte as U+FFFD, as ImGui shows it
        if (fits(&a, s.len*3 + 1)) {
            r = nsstring(toastclean(&a, s, s.len*3));
        }
    }
    return r ? r : nsstring("");
}

static void *macnsstr(Str s)
{
    return macnsstr(s, axmac->logmem);
}

// The role as NSAccessibility names it, and its subrole, if any
static char const *macrole(AxTree *t, i32 i, char const **sub)
{
    AxNode *n = t->nodes + i;
    *sub = 0;
    switch (n->role) {
    case AX_NONE:     return "AXUnknown";
    case AX_APP:      return "AXGroup";
    case AX_WINDOW:   return "AXGroup";
    case AX_DIALOG:   return "AXGroup";
    case AX_POPUP: {
        // A context menu's popup holds menu items
        i32 c = n->child;
        while (c>=0 && t->nodes[c].role==AX_NONE) {
            c = t->nodes[c].next;
        }
        return c>=0 && t->nodes[c].role==AX_MENUITEM ? "AXMenu" : "AXGroup";
    }
    case AX_MENUBAR:  return "AXMenuBar";
    case AX_MENU:     return "AXMenu";
    case AX_MENUITEM: return "AXMenuItem";
    case AX_GROUP:    return "AXGroup";
    case AX_BUTTON:   return "AXButton";
    case AX_CHECKBOX: return "AXCheckBox";
    case AX_RADIO:    return "AXRadioButton";
    case AX_LINK:     return "AXLink";
    case AX_TABLIST:  return "AXTabGroup";
    case AX_TAB:      *sub = "AXTabButton";
                      return "AXRadioButton";
    // A collapsing header: AXRow wants an AXOutline above it, which no
    // ImGui window is, so it is the disclosure control it looks like
    case AX_TREEITEM: return "AXDisclosureTriangle";
    // A Selectable, a row of a list that is no AXList: pressed to pick
    case AX_LISTITEM: return "AXButton";
    case AX_COMBO:    return "AXPopUpButton";
    case AX_SLIDER:   return "AXSlider";
    case AX_TEXTFIELD:
        *sub = n->state & AXS_PASSWORD ? "AXSecureTextField" : 0;
        return "AXTextField";
    case AX_TEXTAREA: return "AXTextArea";
    case AX_TEXT:     return "AXStaticText";
    case AX_IMAGE:    return "AXImage";
    case AX_NROLES:   break;
    }
    return "AXUnknown";
}

static b32 macpressable(AxNode *n)
{
    return (n->actions & (AXA_PRESS|AXA_TOGGLE|AXA_EXPAND|AXA_SELECT)) != 0;
}

// Whether its value means something: a state, or text
static b32 macvalued(AxNode *n)
{
    switch (n->role) {
    case AX_CHECKBOX: case AX_RADIO: case AX_TAB: case AX_TREEITEM:
    case AX_TEXTFIELD: case AX_TEXTAREA: case AX_TEXT:
        return 1;
    default:
        return n->value.len > 0;
    }
}

// Its NSAccessibility frame: the screen's points, from its bottom left
static NsRect macscreen(AxRect r)
{
    void *win  = macwindow();
    void *view = win ? objcsend<void *>(win, "contentView") : 0;
    if (!view) {
        return {};
    }
    NsRect b = objcrect(view, "bounds");
    NsRect v = {r.x0, 0, (f64)r.x1 - r.x0, (f64)r.y1 - r.y0};
    v.y = objcsend<ObjcBool>(view, "isFlipped") ? b.y + r.y0 : b.y + b.h - r.y1;
    NsRect w = objcrect(view, "convertRect:toView:", v, (void *)0);
    return objcrect(win, "convertRectToScreen:", w);
}

// A screen point in ImGui's coordinates, or 0 with no window
static b32 macfromscreen(NsPoint p, f32 *x, f32 *y)
{
    void *win  = macwindow();
    void *view = win ? objcsend<void *>(win, "contentView") : 0;
    if (!view) {
        return 0;
    }
    NsRect  w  = objcrect(win, "convertRectFromScreen:", NsRect{p.x, p.y, 0, 0});
    NsPoint v  = objcsend<NsPoint>(view, "convertPoint:fromView:", NsPoint{w.x, w.y}, (void *)0);
    NsRect  b  = objcrect(view, "bounds");
    *x = (f32)v.x;
    *y = (f32)(objcsend<ObjcBool>(view, "isFlipped") ? v.y - b.y : b.y + b.h - v.y);
    return 1;
}

// Whether node a is in front of node b: of their ancestors where they
// part, the later sibling, which ImGui drew later, or the deeper
static b32 macfront(AxTree *t, i32 a, i32 b)
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

// The node at a point, front most and deepest, or the root
static i32 machit(f32 x, f32 y)
{
    AxTree *t    = axmac->ax->pub;
    i32     best = 0;
    for (i32 i = 1; i < t->len; i++) {
        AxNode *n = t->nodes + i;
        if (n->role!=AX_NONE && x>=n->rect.x0 && x<n->rect.x1 && y>=n->rect.y0 &&
                y<n->rect.y1 && axfind(t, n->key)==i && macfront(t, i, best)) {
            best = i;
        }
    }
    return best;
}

// The visible parent: past those hidden, to the root (0)
static i32 macparent(AxTree *t, i32 i)
{
    i32 p = t->nodes[i].parent;
    while (p>0 && t->nodes[p].role==AX_NONE) {
        p = t->nodes[p].parent;
    }
    return p<0 ? 0 : p;
}

// The children of node i as tools see them: those hidden give theirs
static void mackids(AxTree *t, i32 i, void *arr, i32 depth)
{
    for (i32 c = t->nodes[i].child; c >= 0; c = t->nodes[c].next) {
        if (axfind(t, t->nodes[c].key) != c) {
            continue;  // a duplicate key: its element is the first's
        } else if (t->nodes[c].role != AX_NONE) {
            void *el = macelof(c);
            if (el) {
                objcsend<void>(arr, "addObject:", el);
            }
        } else if (depth < 64) {
            mackids(t, c, arr, depth+1);
        }
    }
}

static void *mackidsof(i32 i)
{
    void *arr = objcsend<void *>(objc_getClass("NSMutableArray"), "array");
    if (i < axmac->ax->pub->len) {
        mackids(axmac->ax->pub, i, arr, 0);
    }
    return arr;
}

static void *macfocused()
{
    AxTree *t = axmac->ax->pub;
    i32     i = t->focus;
    while (i>0 && t->nodes[i].role==AX_NONE) {
        i = t->nodes[i].parent;
    }
    return i>0 ? macelof(i) : 0;
}

// The focused field's text model, if it is node n's
static AxText *mactextof(AxNode *n)
{
    AxText *tx = &axmac->ax->text;
    return tx->live && tx->key==n->key ? tx : 0;
}

static void macnoteact(char const *what, AxNode *n, b32 ok)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        print(&b, Str{(u8 *)what, (iz)__builtin_strlen(what)});
        putbyte(&b, ' ');
        if (n) {
            print(&b, axrolenames[n->role]);
            putbyte(&b, ' ');
            axquote(&b, takehead(n->name, axcutat(n->name, 200)));
        } else {
            print(&b, "(gone)");
        }
        print(&b, ok ? Str(" -> YES") : Str(" -> NO"));
        macnote(x, a, &b);
    }
}


// The element's methods. Each looks its node up again; a node gone
// answers as nothing at all, never nil where AppKit wants an object.

static void *macelrole(void *self, void *)
{
    macax.gets[MG_ROLE]++;
    i32  i = 0;
    char const *sub = 0;
    return macnodeof(self, &i) ? nsstring(macrole(axmac->ax->pub, i, &sub))
                               : nsstring("AXUnknown");
}

static void *macelsubrole(void *self, void *)
{
    macax.gets[MG_ROLE]++;
    i32  i = 0;
    char const *sub = 0;
    if (macnodeof(self, &i)) {
        macrole(axmac->ax->pub, i, &sub);
    }
    return sub ? nsstring(sub) : 0;
}

// A menu item's name is its title, as AppKit's are; anything else's,
// its label (AXDescription), which a static text gives as its value
static void *macellabel(void *self, void *)
{
    macax.gets[MG_NAME]++;
    AxNode *n = macnodeof(self);
    if (!n || n->role==AX_MENUITEM || n->role==AX_TEXT || !n->name.len) {
        return 0;
    }
    return macnsstr(n->name);
}

static void *maceltitle(void *self, void *)
{
    macax.gets[MG_NAME]++;
    AxNode *n = macnodeof(self);
    return n && n->role==AX_MENUITEM ? macnsstr(n->name) : 0;
}

static void *macelhelp(void *self, void *)
{
    macax.gets[MG_NAME]++;
    AxNode *n = macnodeof(self);
    return n && n->help.len ? macnsstr(n->help) : 0;
}

static void *macnumber(b32 v)
{
    return objcsend<void *>(objc_getClass("NSNumber"), "numberWithInt:", (int)(v != 0));
}

static void *macelvalue(void *self, void *)
{
    macax.gets[MG_VALUE]++;
    AxNode *n = macnodeof(self);
    if (!n) {
        return 0;
    }
    switch (n->role) {
    case AX_CHECKBOX:
    case AX_RADIO:    return macnumber(n->state & AXS_CHECKED);
    case AX_TAB:      return macnumber(n->state & (AXS_SELECTED|AXS_CHECKED));
    case AX_TREEITEM: return macnumber(n->state & AXS_EXPANDED);
    case AX_TEXT:     return macnsstr(n->name);
    case AX_TEXTFIELD:
    case AX_TEXTAREA: {
        if (n->state & AXS_PASSWORD) {
            return nsstring("");
        }
        // The field being edited: its text as the model has it, with a
        // tool's edits in already, and an input method's marked text
        AxText *tx = mactextof(n);
        if (!tx) {
            return macnsstr(n->value);
        } else if (!tx->marked.len) {
            return macnsstr(tx->text);
        }
        Arena a = axmac->logmem;
        if (!fits(&a, tx->text.len + tx->marked.len + 64)) {
            return macnsstr(tx->text);
        }
        Str doc = axdoc(axmac->ax, &a);
        return macnsstr(doc, a);
    }
    default:
        return n->value.len ? macnsstr(n->value) : 0;
    }
}

static ObjcBool macelenabled(void *self, void *)
{
    macax.gets[MG_STATE]++;
    AxNode *n = macnodeof(self);
    return n && !(n->state & AXS_DISABLED);
}

static ObjcBool macelfocusedq(void *self, void *)
{
    macax.gets[MG_STATE]++;
    AxNode *n = macnodeof(self);
    return n && (n->state & AXS_FOCUSED);
}

static ObjcBool macelselected(void *self, void *)
{
    macax.gets[MG_STATE]++;
    AxNode *n = macnodeof(self);
    return n && (n->state & AXS_SELECTED);
}

static ObjcBool macelexpanded(void *self, void *)
{
    macax.gets[MG_STATE]++;
    AxNode *n = macnodeof(self);
    return n && (n->state & AXS_EXPANDED);
}

static ObjcBool macelisel(void *self, void *)
{
    // A dying element is one still, so AppKit passes on its destruction
    // rather than ignore it
    return *macdeadp(self)==MACDEAD_DYING || macnodeof(self);
}

static void *macelparent(void *self, void *)
{
    macax.gets[MG_TREE]++;
    i32 i = 0;
    if (!macnodeof(self, &i)) {
        return 0;
    }
    i32 p = macparent(axmac->ax->pub, i);
    return p ? macelof(p) : macroot();
}

static void *macelkids(void *self, void *)
{
    macax.gets[MG_TREE]++;
    i32 i = 0;
    if (!macnodeof(self, &i)) {
        return objcsend<void *>(objc_getClass("NSArray"), "array");
    }
    return mackidsof(i);
}

static void *macelwindow(void *self, void *)
{
    macax.gets[MG_TREE]++;
    return macnodeof(self) ? macwindow() : 0;
}

static NsRect macelframe(void *self, void *)
{
    macax.gets[MG_FRAME]++;
    AxNode *n = macnodeof(self);
    return n ? macscreen(n->rect) : NsRect{};
}

static void *macelhit(void *self, void *, NsPoint p)
{
    macax.gets[MG_HIT]++;
    i32 i = 0;
    f32 x = 0, y = 0;
    if (!macnodeof(self, &i) || !macfromscreen(p, &x, &y)) {
        return self;
    }
    // The deepest at the point, if it is this or under it
    AxTree *t = axmac->ax->pub;
    i32     h = machit(x, y);
    for (i32 k = h; k > 0; k = t->nodes[k].parent) {
        if (k == i) {
            return macelof(h);
        }
    }
    return self;
}

static ObjcBool macelpress(void *self, void *)
{
    AxNode *n  = macnodeof(self);
    b32     ok = n && macpressable(n) && axaction(axmac->g, n->key, AXACT_PRESS);
    if (ok) {
        macwake();
    }
    macnoteact("AXPress", n, ok);
    return ok;
}

static ObjcBool macelmenu(void *self, void *)
{
    AxNode *n  = macnodeof(self);
    b32     ok = n && axaction(axmac->g, n->key, AXACT_MENU);
    if (ok) {
        macwake();
    }
    macnoteact("AXShowMenu", n, ok);
    return ok;
}

// AXFocused = YES: a field takes the keyboard, its caret kept. Focus
// can't be given away, only taken, as with the mouse.
static void macelsetfocused(void *self, void *, ObjcBool on)
{
    AxNode *n  = macnodeof(self);
    b32     ok = n && on && axaction(axmac->g, n->key, AXACT_FOCUS);
    if (ok) {
        macwake();
    }
    macnoteact(on ? "AXFocused=YES" : "AXFocused=NO", n, ok);
}


// A text field's own, as an NSTextView's. Its document is the text with
// an input method's marked text in it (axdoc), counted in UTF-16 units:
// the focused field's from the text model, which a tool's edits are in at
// once (the shadow), so a read right after a set agrees; another field's,
// its value, with no selection, lines or geometry of its own. A range a
// tool gives is checked (axdocrange), as one out of the text would raise
// in AppKit's own, and an exception can't unwind through C++: refused, it
// reads as nothing. Sets are queued edits for the focused field alone
// (axtextedit, axtextselect), each one undo, taken at the next frame.
// Parameterized attributes and sets are logged with their arguments.

struct MacDoc {
    A11y *x;  // the text model, if the field is the focused one
    Str   s;  // else its value
};

// Whether node n is a text field a tool may read: not a password's
static b32 mactextrole(AxNode *n)
{
    return n && (n->role==AX_TEXTFIELD || n->role==AX_TEXTAREA) &&
           !(n->state & AXS_PASSWORD);
}

// ... and may set, which takes the focus to do
static b32 maceditable(AxNode *n)
{
    return mactextrole(n) && !(n->state & (AXS_READONLY|AXS_DISABLED));
}

static char const *const mactextsels[] = {
    "accessibilityNumberOfCharacters", "accessibilitySelectedText",
    "accessibilitySelectedTextRange", "accessibilitySelectedTextRanges",
    "accessibilityVisibleCharacterRange", "accessibilityInsertionPointLineNumber",
    "accessibilityLineForIndex:", "accessibilityRangeForLine:",
    "accessibilityStringForRange:", "accessibilityAttributedStringForRange:",
    "accessibilityRangeForPosition:", "accessibilityRangeForIndex:",
    "accessibilityFrameForRange:", "accessibilityStyleRangeForIndex:",
};

static char const *const macsetsels[] = {
    "setAccessibilityValue:", "setAccessibilitySelectedText:",
    "setAccessibilitySelectedTextRange:", "setAccessibilitySelectedTextRanges:",
};

static b32 macin(char const *s, char const *const *set, i32 n)
{
    for (i32 i = 0; i < n; i++) {
        if (!__builtin_strcmp(s, set[i])) {
            return 1;
        }
    }
    return 0;
}

static b32 mactextsel(char const *s)
{
    return macin(s, mactextsels, countof(mactextsels));
}

static b32 macsetsel(char const *s)
{
    return macin(s, macsetsels, countof(macsetsels));
}

// self's node and document, or null if it is no text field to read
static AxNode *macdocof(void *self, MacDoc *d)
{
    AxNode *n = macnodeof(self);
    *d = {};
    if (!mactextrole(n)) {
        return 0;
    } else if (mactextof(n)) {
        d->x = axmac->ax;
    } else {
        d->s = n->value;
    }
    return n;
}

static i32 macdoclen(MacDoc *d)
{
    return d->x ? axdoclen(d->x, AXU_UTF16) : axcount(d->s, AXU_UTF16, (i32)d->s.len);
}

// The document's bytes as a tool's units
static NsRange macunits(MacDoc *d, AxSpan s)
{
    i32 lo = d->x ? axdocunit(d->x, AXU_UTF16, s.lo) : axcount(d->s, AXU_UTF16, s.lo);
    i32 hi = d->x ? axdocunit(d->x, AXU_UTF16, s.hi) : axcount(d->s, AXU_UTF16, s.hi);
    return {(uz)lo, (uz)(hi>lo ? hi-lo : 0)};
}

// A tool's units as the document's bytes: 0 if not all in it
static b32 macbytes(MacDoc *d, NsRange r, AxSpan *out)
{
    i64 loc = (i64)r.loc;  // past I64MAX, negative: refused
    i64 len = (i64)r.len;
    if (d->x) {
        return axdocrange(d->x, AXU_UTF16, loc, len, out);
    }
    i64 n = macdoclen(d);
    if (loc<0 || len<0 || loc>n || len>n-loc) {
        return 0;
    }
    out->lo = axadvance(d->s, AXU_UTF16, (i32)loc);
    out->hi = axadvance(d->s, AXU_UTF16, (i32)(loc + len));
    return 1;
}

// The document's byte at unit i, which is put in it first
static i32 macbyteat(MacDoc *d, iz i)
{
    i32    n = macdoclen(d);
    AxSpan b = {};
    macbytes(d, {(uz)(i<0 ? 0 : i>n ? n : i), 0}, &b);
    return b.lo;
}

// The character at unit i, as units: none past the end
static NsRange macchar(MacDoc *d, iz i)
{
    i32    b = macbyteat(d, i);
    AxSpan c = {b, b};
    if (d->x) {
        c = axdocchar(d->x, b);
    } else if (b < d->s.len) {
        iz e = b;
        utf8decode(d->s, &e);
        c.hi = (i32)e;
    }
    return macunits(d, c);
}

// The selection, as the document's bytes: within marked text, the input
// method's own
static AxSpan macsel(MacDoc *d)
{
    if (!d->x) {
        return {};
    }
    AxText *t = &d->x->text;
    AxSpan  s = axselection(t);
    return t->marked.len ? AxSpan{s.lo + t->mlo, s.lo + t->mhi} : s;
}

// The document's bytes as an NSString, made in the adapter's scratch
static void *macdocstr(MacDoc *d, AxSpan s)
{
    Arena a = axmac->logmem;
    Str   r = {};
    if (!d->x) {
        r = span(d->s.data+s.lo, d->s.data+s.hi);
    } else if (!d->x->text.marked.len || fits(&a, s.hi - s.lo + 64)) {
        r = axdocslice(d->x, &a, s);
    }
    return macnsstr(r, a);
}

// The log's arguments and answers, of the kinds these take
enum : i32 {
    MACA_RANGE,
    MACA_NUM,
    MACA_STR,
    MACA_RECT,
    MACA_POINT,
    MACA_OK,
};

struct MacArg {
    i32     kind;
    NsRange r;
    i64     i;
    void   *o;
    NsRect  rc;
};

static MacArg macarange(NsRange r) { MacArg a = {}; a.kind = MACA_RANGE; a.r = r; return a; }
static MacArg macanum(i64 i)       { MacArg a = {}; a.kind = MACA_NUM; a.i = i; return a; }
static MacArg macastr(void *o)     { MacArg a = {}; a.kind = MACA_STR; a.o = o; return a; }
static MacArg macarect(NsRect r)   { MacArg a = {}; a.kind = MACA_RECT; a.rc = r; return a; }
static MacArg macaok(b32 ok)       { MacArg a = {}; a.kind = MACA_OK; a.i = ok; return a; }

static MacArg macapoint(NsPoint p)
{
    MacArg a = {};
    a.kind = MACA_POINT;
    a.rc   = {p.x, p.y, 0, 0};
    return a;
}

static void macputarg(Buf *b, MacArg v)
{
    switch (v.kind) {
    case MACA_RANGE: macrange(b, v.r); break;
    case MACA_NUM:   print(b, v.i); break;
    case MACA_STR:   macstrarg(b, v.o); break;
    case MACA_RECT:  macrect(b, v.rc); break;
    case MACA_OK:    print(b, v.i ? Str("YES") : Str("NO")); break;
    case MACA_POINT:
        putbyte(b, '(');
        print(b, axround((f32)v.rc.x));
        putbyte(b, ',');
        print(b, axround((f32)v.rc.y));
        putbyte(b, ')');
        break;
    }
}

// Log "WHAT ARG -> GOT", after the text model the call is against
static void maclogtext(char const *what, MacArg arg, MacArg got)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        print(&b, Str{(u8 *)what, (iz)__builtin_strlen(what)});
        putbyte(&b, ' ');
        macputarg(&b, arg);
        print(&b, " -> ");
        macputarg(&b, got);
        macnote(x, a, &b);
    }
}

static iz macelnchars(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    return macdocof(self, &d) ? macdoclen(&d) : 0;
}

static NsRange macelselrange(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    return macdocof(self, &d) ? macunits(&d, macsel(&d)) : NsRange{};
}

static void *macelseltext(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    return macdocof(self, &d) ? macdocstr(&d, macsel(&d)) : 0;
}

static void *macelselranges(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    if (!macdocof(self, &d)) {
        return 0;
    }
    void *v = objcsend<void *>(objc_getClass("NSValue"), "valueWithRange:",
                               macunits(&d, macsel(&d)));
    return objcsend<void *>(objc_getClass("NSArray"), "arrayWithObject:", v);
}

// The lines that show; all of another field
static NsRange macelvisible(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    if (!macdocof(self, &d)) {
        return {};
    } else if (!d.x) {
        return {0, (uz)macdoclen(&d)};
    }
    AxSpan v = axvisible(d.x);
    return macunits(&d, {axtodoc(d.x, v.lo), axtodoc(d.x, v.hi)});
}

static iz macelcaretline(void *self, void *)
{
    macax.gets[MG_TEXT]++;
    MacDoc d;
    return macdocof(self, &d) && d.x ? axcaretline(d.x) : 0;
}

// Lines are as drawn, wrapped, as an NSTextView's; another field's text
// is all on one
static iz macellineof(void *self, void *, iz i)
{
    MacDoc d;
    iz     k = 0;
    if (macdocof(self, &d) && d.x) {
        k = axlineat(d.x, axdoctext(d.x, macbyteat(&d, i)));
    }
    maclogtext("AXLineForIndex", macanum(i), macanum(k));
    return k;
}

static NsRange macelrangeofline(void *self, void *, iz k)
{
    MacDoc  d;
    NsRange r = {};
    if (!macdocof(self, &d)) {
        // no text
    } else if (!d.x) {
        r = k ? r : NsRange{0, (uz)macdoclen(&d)};
    } else if (k>=0 && k<axlinecount(d.x)) {
        AxSpan s = axlinespan(d.x, (i32)k);
        r = macunits(&d, {axtodoc(d.x, s.lo), axtodoc(d.x, s.hi)});
    }
    maclogtext("AXRangeForLine", macanum(k), macarange(r));
    return r;
}

// The range's text, or none for a range not in it
static void *macrangestr(void *self, NsRange r)
{
    MacDoc d;
    AxSpan s = {};
    if (!macdocof(self, &d)) {
        return 0;
    }
    return macbytes(&d, r, &s) ? macdocstr(&d, s) : nsstring("");
}

static void *macelstring(void *self, void *, NsRange r)
{
    void *s = macrangestr(self, r);
    maclogtext("AXStringForRange", macarange(r), macastr(s));
    return s;
}

// Plain: ImGui draws it all in the one font and color
static void *macelattrstring(void *self, void *, NsRange r)
{
    void *s = macrangestr(self, r);
    void *o = 0;
    if (s) {
        o = objcsend<void *>(objc_getClass("NSAttributedString"), "alloc");
        o = o ? objcsend<void *>(o, "initWithString:", s) : 0;
        o = o ? objcsend<void *>(o, "autorelease") : 0;
    }
    maclogtext("AXAttributedStringForRange", macarange(r), macastr(o));
    return o;
}

// The character under a point of the screen
static NsRange macelrangeat(void *self, void *, NsPoint p)
{
    MacDoc  d;
    NsRange r = {};
    f32     x = 0, y = 0;
    if (macdocof(self, &d) && d.x && macfromscreen(p, &x, &y)) {
        r = macunits(&d, axdocchar(d.x, axdocat(d.x, x, y)));
    }
    maclogtext("AXRangeForPosition", macapoint(p), macarange(r));
    return r;
}

// A code point's: two units for one past U+FFFF
static NsRange macelrangeofindex(void *self, void *, iz i)
{
    MacDoc  d;
    NsRange r = macdocof(self, &d) ? macchar(&d, i) : NsRange{};
    maclogtext("AXRangeForIndex", macanum(i), macarange(r));
    return r;
}

// The marked text, underlined, is a run of its own
static NsRange macelstyle(void *self, void *, iz i)
{
    MacDoc  d;
    NsRange r = {};
    if (macdocof(self, &d)) {
        r = d.x ? macunits(&d, axdocstyle(d.x, macbyteat(&d, i)))
                : NsRange{0, (uz)macdoclen(&d)};
    }
    maclogtext("AXStyleRangeForIndex", macanum(i), macarange(r));
    return r;
}

// AXBoundsForRange: where the text was drawn, on the screen; another
// field's, the field
static NsRect macelframeof(void *self, void *, NsRange r)
{
    MacDoc  d;
    AxSpan  s = {};
    NsRect  f = {};
    AxNode *n = macdocof(self, &d);
    if (n && macbytes(&d, r, &s)) {
        f = macscreen(d.x ? axdocbounds(d.x, s) : n->rect);
    }
    maclogtext("AXBoundsForRange", macarange(r), macarect(f));
    return f;
}

// A tool's text, an NSString or an attributed one, as UTF-8; ok 0 if
// it is neither, or has no UTF-8 (a lone surrogate)
static Str mactextarg(void *o, b32 *ok)
{
    void *s = maciskind(o, "NSAttributedString") ? objcsend<void *>(o, "string") : o;
    *ok = maciskind(s, "NSString");
    Str r = *ok ? macutf8(s) : Str{};
    *ok = *ok && (r.data || !objcsend<uz>(s, "length"));
    return r;
}

// The model, if self is the focused field and a tool may set its text
static A11y *macsettable(void *self)
{
    AxNode *n = macnodeof(self);
    return maceditable(n) && mactextof(n) ? axmac->ax : 0;
}

static void macelsetvalue(void *self, void *, void *v)
{
    A11y *x  = macsettable(self);
    b32   ok = 0;
    Str   s  = x ? mactextarg(v, &ok) : Str{};
    ok = ok && axtextedit(axmac->g, 0, axtextlen(x), s, AX_AFTER, AX_AFTER);
    if (ok) {
        macwake();
    }
    maclogtext("AXValue=", macastr(v), macaok(ok));
}

static void macelsetseltext(void *self, void *, void *v)
{
    A11y *x  = macsettable(self);
    b32   ok = 0;
    Str   s  = x ? mactextarg(v, &ok) : Str{};
    if (ok) {
        AxSpan sel = axselection(&x->text);
        ok = axtextedit(axmac->g, sel.lo, sel.hi, s, AX_AFTER, AX_AFTER);
    }
    if (ok) {
        macwake();
    }
    maclogtext("AXSelectedText=", macastr(v), macaok(ok));
}

static b32 macselect(void *self, NsRange r)
{
    MacDoc d;
    AxSpan s = {};
    return macsettable(self) && macdocof(self, &d) && macbytes(&d, r, &s) &&
           axtextselect(axmac->g, axdoctext(d.x, s.lo), axdoctext(d.x, s.hi));
}

static void macelsetselrange(void *self, void *, NsRange r)
{
    b32 ok = macselect(self, r);
    if (ok) {
        macwake();
    }
    maclogtext("AXSelectedTextRange=", macarange(r), macaok(ok));
}

// The first range: ImGui selects only one
static void macelsetselranges(void *self, void *, void *arr)
{
    void       *v    = maciskind(arr, "NSArray") && objcsend<uz>(arr, "count")
                       ? objcsend<void *>(arr, "objectAtIndex:", (uz)0) : 0;
    char const *type = maciskind(v, "NSValue") ? objcsend<char const *>(v, "objCType") : 0;
    b32         is   = type && !__builtin_strncmp(type, "{_NSRange=", 10);
    NsRange     r    = is ? objcsend<NsRange>(v, "rangeValue") : NsRange{NSNOTFOUND, 0};
    b32         ok   = is && macselect(self, r);
    if (ok) {
        macwake();
    }
    maclogtext("AXSelectedTextRanges=", macarange(r), macaok(ok));
}

static void *macallowedup;  // NSAccessibilityElement's, if it has one

// What each node offers: AppKit lists attributes and actions by this
static ObjcBool macelallowed(void *self, void *sel, void *what)
{
    macax.gets[MG_STATE]++;
    AxNode *n = macnodeof(self);
    char const *s = what ? sel_getName(what) : "";
    if (!n) {
        // A node gone: nothing to act on or set
        return __builtin_strncmp(s, "accessibilityPerform", 20) &&
               __builtin_strncmp(s, "setAccessibility", 16);
    } else if (macsetsel(s)) {
        return maceditable(n);
    } else if (mactextsel(s)) {
        return mactextrole(n);
    } else if (!__builtin_strcmp(s, "accessibilityPerformPress")) {
        return macpressable(n);
    } else if (!__builtin_strcmp(s, "accessibilityPerformShowMenu")) {
        return (n->actions & AXA_MENU) != 0;
    } else if (!__builtin_strcmp(s, "setAccessibilityFocused:")) {
        return (n->actions & AXA_FOCUS) != 0;
    } else if (!__builtin_strcmp(s, "accessibilityValue")) {
        return macvalued(n);
    } else if (!__builtin_strcmp(s, "isAccessibilityExpanded") ||
               !__builtin_strcmp(s, "isAccessibilityDisclosed")) {
        return n->role==AX_TREEITEM || n->role==AX_COMBO;
    } else if (!__builtin_strcmp(s, "isAccessibilitySelected")) {
        return n->role==AX_TAB || n->role==AX_LISTITEM || n->role==AX_MENUITEM;
    }
    using F = ObjcBool (*)(void *, void *, void *);
    return macallowedup ? ((F)macallowedup)(self, sel, what) : 1;
}


// SDL's NSTextInputClient, backed by the text model
//
// Dictation and the input methods talk to SDL's text input view, which
// knows nothing of the text: its ranges are stale, it reads nothing
// back, a replacement range becomes one Backspace, and a newline is
// dropped. While the focused field is in the model (the tree is on),
// the methods SDL defines answer from the model instead, in UTF-16
// units of the document (axdoc), and change the text through the edit
// queue (axtextedit), whose shadow agrees at once for a read-back.
//
// Typing keeps SDL's way: a key's text (inside a key down's dispatch,
// mackeyed) with no replacement range goes to SDL, as do its commands,
// so Return still sends, and an input method's commit lands as before,
// in order with the keys around it; unless an edit is still queued for
// the field, which the key's text then queues after, so as not to land
// ahead of it. Everything else is queued: Dictation's text, its
// newlines and corrections, and replacement ranges. SDL's own methods
// are still called, for their bookkeeping (its marked text, a key's
// pending press), with what can do no harm: an empty string, a range
// checked first, as an exception can't unwind through C++. Their calls
// back into these (SDL's insertText asks hasMarkedText, then calls
// unmarkText) get SDL's own answers (macinsdl).
//
// A composition (setMarkedText) is drawn by the field (gui_preedit) and
// is the model's marked text at once (axmark); with a replacement range,
// that range is selected, for the composition to stand in for until it
// is committed, as one edit. One from no key is Dictation's
// (macspoken), which never says it is done: a key's text or command
// commits it ahead of the key (macspokenend), and the input method is
// told; ime.cpp drops it on a send, a draft put, a recall, or another
// field.
//
// When the tree is off, a call that changes text from no key and looks
// like Dictation's or Voice Control's starts it (`auto`): setMarkedText,
// insertText with Dictation's alternatives, a known command. Queries
// never do, as AppKit, input methods and their windows make them on
// their own, nor does anything a key does: typing, the emoji picker or
// a candidate clicked cost nothing. Until the model answers, SDL does,
// and a replacement range a client worked out from SDL's answers is
// taken as none (macsdlranges): SDL's ranges count from the marked
// text, not the document.
//
// Each call is logged, after the text model it is against, as M1.0's
// trace has them ("ns replay" plays one back); one SDL handles is
// marked "(SDL's)".

static void (*macinsertimp)(void *, void *, void *, NsRange);
static void (*macsetmarkedimp)(void *, void *, void *, NsRange, NsRange);
static void (*macunmarkimp)(void *, void *);
static NsRange (*macselectedimp)(void *, void *);
static NsRange (*macmarkedimp)(void *, void *);
static ObjcBool (*machasmarkedimp)(void *, void *);
static void *(*macattrsubimp)(void *, void *, NsRange, NsRange *);
static NsRect (*macfirstrectimp)(void *, void *, NsRange, NsRange *);
static uz (*macindexatimp)(void *, void *, NsPoint);
static void (*maccommandimp)(void *, void *, void *);
static void *(*macvalidattrsimp)(void *, void *);

static i32 macinsdl;      // SDL's methods running: their calls back are SDL's
static b32 macspoken;     // the composition came from no key: Dictation's
static b32 macsdlranges;  // SDL gave the client its ranges, since the last text

// The model, if the methods answer from it: the focused field's, which
// SDL's text input serves, when it may be edited
static A11y *mactext()
{
    AxGui *x = axmac;
    return x && x->on && !macinsdl && axeditable(&x->ax->text) ? x->ax : 0;
}

// A call from no key that changes text: start the tree, so that the
// next is the model's
static void macwant(char const *why)
{
    if (axmac && !axmac->on && !macinsdl && !mackeyed()) {
        macready(why);
    }
}

// Whether s carries Dictation's alternatives (NSTextAlternatives), as
// its final text does: other text from no key (the emoji picker, a
// candidate clicked) is no reason to start the tree.
static b32 macdictated(void *s)
{
    if (!maciskind(s, "NSAttributedString")) {
        return 0;
    }
    uz   n   = objcsend<uz>(s, "length");
    void *key = nsstring("NSTextAlternatives");
    for (uz i = 0, k = 0; i<n && k<64; k++) {
        NsRange r = {};
        if (objcsend<void *>(s, "attribute:atIndex:effectiveRange:", key, i, &r)) {
            return 1;
        } else if (r.loc+r.len <= i) {
            break;
        }
        i = r.loc + r.len;
    }
    return 0;
}

// A replacement range, unless the client worked it out from SDL's
// answers, in other units: then none
static NsRange macreplacing(NsRange r)
{
    return macsdlranges ? NsRange{NSNOTFOUND, 0} : r;
}

// The composition is now s, the input method's selection bytes [lo, hi)
// of it: drawn by the field, and the model's at once
static void maccompose(A11y *x, Str s, i32 lo, i32 hi)
{
    i32 c0 = axcount(s, AXU_CP, lo);
    i32 c1 = axcount(s, AXU_CP, hi);
    gui_preedit(axmac->g, s, c0, c1-c0);
    axmark(x, s, lo, hi);
    macwake();
}

// The composition is over, its text kept or not
static void macendmark(A11y *x)
{
    gui_preedit(axmac->g, {}, 0, 0);
    axmark(x, {}, 0, 0);
    macspoken    = 0;
    macsdlranges = 0;
    macwake();
}

// r within n units: past the end, cut to it
static NsRange macclamp(NsRange r, uz n)
{
    uz lo = r.loc>n ? n : r.loc;
    return {lo, r.len>n-lo ? n-lo : r.len};
}

// Units r of s (a composition) as its bytes, cut to fit
static AxSpan macmarkbytes(Str s, NsRange r)
{
    NsRange c = macclamp(r, (uz)axcount(s, AXU_UTF16, (i32)s.len));
    return {axadvance(s, AXU_UTF16, (i32)c.loc), axadvance(s, AXU_UTF16, (i32)(c.loc + c.len))};
}

// Units r of the document as bytes of the text: within the marked text,
// the selection it stands in for. 0 if not all in it. One that starts
// in the marked text ends with it: the field keeps less of a long
// composition than the client sent (PREEDIT_MAX), and the client's
// range of all of it would reach into the text after.
static b32 mactextspan(A11y *x, NsRange r, AxSpan *out)
{
    MacDoc d = {x, {}};
    AxSpan s = {};
    if (r.loc==NSNOTFOUND || !macbytes(&d, r, &s)) {
        return 0;
    }
    i32 ml = (i32)x->text.marked.len;
    i32 mlo = axselection(&x->text).lo;
    if (ml && s.lo>=mlo && s.lo<mlo+ml && s.hi>mlo+ml) {
        s.hi = mlo + ml;
    }
    out->lo = axdoctext(x, s.lo);
    out->hi = axdoctext(x, s.hi);
    return 1;
}

// A text argument's NSString, or an empty one for anything else, as
// SDL's methods take it unchecked
static void *macstrobj(void *o)
{
    void *s = maciskind(o, "NSAttributedString") ? objcsend<void *>(o, "string") : o;
    return maciskind(s, "NSString") ? s : nsstring("");
}

static void macsdlinsert(void *self, void *s)
{
    macinsdl++;
    macinsertimp(self, sel_registerName("insertText:replacementRange:"), s,
                 NsRange{NSNOTFOUND, 0});
    macinsdl--;
}

static void macsdlunmark(void *self)
{
    macinsdl++;
    macunmarkimp(self, sel_registerName("unmarkText"));
    macinsdl--;
}

// A key's text, by its length alone. Nor is the text model logged
// ahead of it: the next call that isn't a key's logs it.
static void mactyped(Buf *b, void *o)
{
    b32   attr = maciskind(o, "NSAttributedString");
    void *s    = attr ? objcsend<void *>(o, "string") : o;
    print(b, "(typed)#");
    print(b, maciskind(s, "NSString") ? (i64)objcsend<uz>(s, "length") : (i64)-1);
}

// Log a call with a text argument, then its ranges, before it is made
static void maclogcall(char const *call, b32 keyed, void *s, NsRange *rs, i32 n, b32 sdl)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        print(&b, Str{(u8 *)call, (iz)__builtin_strlen(call)});
        putbyte(&b, ' ');
        if (keyed) {
            mactyped(&b, s);
        } else {
            macstrarg(&b, s);
        }
        for (i32 i = 0; i < n; i++) {
            putbyte(&b, ' ');
            macrange(&b, rs[i]);
        }
        if (sdl) {
            print(&b, " (SDL's)");
        }
        if (keyed) {
            axnote(x, a, finish(&b));
        } else {
            macnote(x, a, &b);
        }
    }
}

// The marked text, not from a key, becomes the field's text
static void maccommit(void *self, A11y *x)
{
    AxSpan at = axselection(&x->text);
    axtextedit(axmac->g, at.lo, at.hi, x->text.marked, AX_AFTER, AX_AFTER);
    macsdlunmark(self);
    macendmark(x);
}

// SDL's key pending in self (SDL3TranslatorResponder's ivar), which its
// unmarkText clears as consumed by the input method: kept across a
// commit that isn't the key's, so that the key still goes out after.
static i32 *macpendingkey(void *self)
{
    void *iv = class_getInstanceVariable(object_getClass(self), "_pendingRawCode");
    char const *t = iv ? ivar_getTypeEncoding(iv) : 0;
    return t && !__builtin_strcmp(t, "i") ? (i32 *)((u8 *)self + ivar_getOffset(iv)) : 0;
}

// A key comes while Dictation's composition is up: since macOS 13 one
// may type while it listens. Its words are kept, ahead of the key, as
// the key's text or command would be in order: through SDL, as a key's
// commit, unless an edit is queued for the field, which it then follows.
// The input method is told after the key (mackeydone), so that it
// stops counting on marked text that is gone.
static void macspokenend(void *self, A11y *x)
{
    if (!macspoken || !x->text.marked.len) {
        return;
    }
    if (AxGui *l = maclog()) {
        axnote(l, l->logmem, "a key commits Dictation's marked text");
    }
    if (axqueued(x, x->text.id)) {
        AxSpan at = axselection(&x->text);
        axtextedit(axmac->g, at.lo, at.hi, x->text.marked, AX_AFTER, AX_AFTER);
        macsdlunmark(self);
    } else {
        // Held back meanwhile: SDL's insertText sends a pending key
        // ahead of the text when it has no marked text of its own
        i32 *pk  = macpendingkey(self);
        i32  key = pk ? *pk : -1;
        if (pk) {
            *pk = -1;
        }
        macsdlinsert(self, macnsstr(x->text.marked));
        if (pk) {
            *pk = key;
        }
    }
    macendmark(x);
    macdropsoon = self;
}

static void macinsert(void *self, void *sel, void *s, NsRange r)
{
    b32 keyed = mackeyed();
    if (macdiscarding && !keyed) {
        // An input method's commit as it was told to drop its text, or
        // its view was taken away (os_guiimereset): kept from the field
        A11y *x = mactext();
        maclogcall("insertText:replacementRange: (discarded)", 0, s, &r, 1, 0);
        macsdlinsert(self, nsstring(""));
        if (x) {
            macendmark(x);
        }
        return;
    }
    if (!keyed && macdictated(s)) {
        macwant("insertText");
    }
    A11y *x = mactext();
    if (x && keyed) {
        macspokenend(self, x);
    }
    NsRange rr   = keyed ? r : macreplacing(r);
    b32     none = rr.loc == NSNOTFOUND;
    b32     sdl  = !x || (keyed && none && !axqueued(x, x->text.id));
    maclogcall("insertText:replacementRange:", keyed, s, &r, 1, sdl);
    if (sdl) {
        if (x) {
            axmark(x, {}, 0, 0);  // SDL's text ends it (gui_sdl.cpp's dispatch)
            macspoken = 0;
        }
        macsdlranges = 0;
        macinsdl++;
        macinsertimp(self, sel, s, r);
        macinsdl--;
        return;
    }

    b32    ok   = 0;
    Str    text = mactextarg(s, &ok);
    AxSpan at   = axselection(&x->text);  // which the marked text stands in for
    if (!none && !mactextspan(x, rr, &at)) {
        at = axselection(&x->text);  // not in the text: as though none
    }
    macsdlinsert(self, nsstring(""));  // SDL's marked text let go, a key's press sent
    ok = ok && axtextedit(axmac->g, at.lo, at.hi, text, AX_AFTER, AX_AFTER);
    macendmark(x);
    if (!ok) {
        if (AxGui *l = maclog()) {
            axnote(l, l->logmem, "insertText refused");
        }
    }
}

static void macsetmarked(void *self, void *sel, void *s, NsRange sr, NsRange rr)
{
    b32 keyed = mackeyed();
    if (!keyed) {
        macwant("setMarkedText");
    }
    A11y   *x     = mactext();
    if (x && keyed) {
        macspokenend(self, x);
    }
    NsRange rs[2] = {sr, rr};
    maclogcall("setMarkedText:selectedRange:replacementRange:", keyed, s, rs, 2, !x);

    // SDL's takes the selection unchecked, and a string past its end
    // raises an exception
    void   *str = macstrobj(s);
    NsRange in  = macclamp(sr, objcsend<uz>(str, "length"));
    if (!x) {
        macsetmarkedimp(self, sel, s==str || maciskind(s, "NSAttributedString") ? s : str,
                        in, rr);
        return;
    }

    b32 ok   = 0;
    Str text = mactextarg(str, &ok);
    text = ok ? text : Str{};
    AxSpan  at   = {};
    NsRange rep  = keyed ? rr : macreplacing(rr);
    if (rep.loc!=NSNOTFOUND && mactextspan(x, rep, &at)) {
        // What it replaces is selected, for the composition to stand in
        // for: the commit replaces it, one edit to undo, and a
        // composition dropped leaves it be
        axtextselect(axmac->g, at.lo, at.hi);
    }
    b32 none = rr.loc == NSNOTFOUND;
    macinsdl++;
    macsetmarkedimp(self, sel, str, in, NsRange{NSNOTFOUND, 0});
    macinsdl--;
    AxSpan m = macmarkbytes(text, in);
    if (!text.len) {
        macendmark(x);
    } else if (keyed && none) {
        axmark(x, text, m.lo, m.hi);  // SDL's event draws it, in order with the keys
        macspoken = 0;
    } else {
        maccompose(x, text, m.lo, m.hi);
        macspoken = !keyed;
    }
}

// Marked text becomes text: a key's, through SDL, as a key's commit; else
// queued
static void macunmark(void *self, void *sel)
{
    b32   keyed = mackeyed();
    A11y *x     = mactext();
    b32   has   = x && x->text.marked.len;
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 64);
        print(&b, has ? Str("unmarkText") : Str("unmarkText (SDL's)"));
        macnote(l, a, &b);
    }
    if (!has) {
        macunmarkimp(self, sel);
    } else if (macdiscarding) {
        macsdlunmark(self);  // told to drop it (macdiscard): nothing is kept
        macendmark(x);
    } else if (keyed) {
        void *s = macnsstr(x->text.marked);
        axmark(x, {}, 0, 0);
        macspoken = 0;
        macsdlinsert(self, s);
    } else {
        maccommit(self, x);
    }
}

// The queries are logged after they're answered, with the answer.

static void macnoterange(char const *call, NsRange r)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 64);
        print(&b, Str{(u8 *)call, (iz)__builtin_strlen(call)});
        print(&b, " -> ");
        macrange(&b, r);
        macnote(x, a, &b);
    }
}

// Within marked text, the input method's selection in it
static NsRange macselected(void *self, void *sel)
{
    A11y   *x = mactext();
    MacDoc  d = {x, {}};
    NsRange r = x ? macunits(&d, macsel(&d)) : macselectedimp(self, sel);
    macsdlranges = macinsdl ? macsdlranges : !x;
    macnoterange("selectedRange", r);
    return r;
}

static NsRange macmarked(void *self, void *sel)
{
    A11y   *x = mactext();
    NsRange r = {NSNOTFOUND, 0};
    if (!x) {
        r = macmarkedimp(self, sel);
    } else if (x->text.marked.len) {
        MacDoc d  = {x, {}};
        i32    lo = axselection(&x->text).lo;
        r = macunits(&d, {lo, lo + (i32)x->text.marked.len});
    }
    macsdlranges = macinsdl ? macsdlranges : !x;
    macnoterange("markedRange", r);
    return r;
}

static ObjcBool machasmarked(void *self, void *sel)
{
    A11y    *x = mactext();
    ObjcBool r = x ? x->text.marked.len>0 : machasmarkedimp(self, sel);
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 64);
        print(&b, r ? Str("hasMarkedText -> YES") : Str("hasMarkedText -> NO"));
        macnote(l, a, &b);
    }
    return r;
}

// What the callee made of an actualRange: SDL's leave it alone, so
// what's there is the caller's, not an answer.
static void macactual(Buf *b, NsRange *actual, NsRange was)
{
    if (!actual) {
        return;
    } else if (actual->loc==was.loc && actual->len==was.len) {
        print(b, " actual unchanged");
    } else {
        print(b, " actual ");
        macrange(b, *actual);
    }
}

// The document's text in range, cut to its end; nil for one starting
// past it, as the protocol allows
static void *macattrsub(void *self, void *sel, NsRange r, NsRange *actual)
{
    NsRange was = actual ? *actual : NsRange{};
    A11y   *x   = mactext();
    void *s = 0;
    if (!x) {
        s = macattrsubimp(self, sel, r, actual);
    } else {
        MacDoc d = {x, {}};
        uz     n = (uz)macdoclen(&d);
        AxSpan b = {};
        if (r.loc <= n && macbytes(&d, macclamp(r, n), &b)) {
            void *str = macdocstr(&d, b);
            s = objcsend<void *>(objc_getClass("NSAttributedString"), "alloc");
            s = s ? objcsend<void *>(s, "initWithString:", str) : 0;
            s = s ? objcsend<void *>(s, "autorelease") : 0;
            if (actual) {
                *actual = macunits(&d, b);
            }
        }
    }
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 256);
        print(&b, "attributedSubstringForProposedRange:actualRange: ");
        macrange(&b, r);
        print(&b, " -> ");
        macstrarg(&b, s);
        macactual(&b, actual, was);
        macnote(l, a, &b);
    }
    return s;
}

// Where the part of document units r on its first line was drawn, on
// the screen: 0 if that isn't known, the text or its marked text changed
// since it was drawn
static b32 macfirstof(A11y *x, NsRange r, NsRect *out, NsRange *actual)
{
    AxText *t  = &x->text;
    MacDoc  d  = {x, {}};
    i32     ml = (i32)t->marked.len;
    AxSpan  s  = {};
    if (t->geom.rev!=t->rev || (ml && t->geom.nm!=ml+1) ||
            !macbytes(&d, macclamp(r, (uz)macdoclen(&d)), &s)) {
        return 0;
    }
    if (r.len && macunits(&d, s).len==0) {
        s.hi = axdocchar(x, s.lo).hi;  // half a surrogate pair: the character
    }
    AxRect rc  = axdocrect(x, s.lo);
    i32    end = s.lo;
    for (i32 b = s.lo; b < s.hi;) {
        AxSpan c = axdocchar(x, b);
        if (c.hi <= c.lo) {
            break;
        }
        AxRect cr = axdocrect(x, c.lo);
        if (cr.y0 != rc.y0) {
            break;
        }
        rc.x1 = cr.x1>rc.x1 ? cr.x1 : rc.x1;
        end = b = c.hi;
    }
    rc.x1 = end>s.lo ? rc.x1 : rc.x0;  // none: the caret's place
    *out  = macscreen(rc);
    if (actual) {
        *actual = macunits(&d, {s.lo, end});
    }
    return 1;
}

// Where Dictation's and the input method's windows go: until the field
// draws the text as it is, SDL's answer, the input method's place as the
// field last gave it
static NsRect macfirstrect(void *self, void *sel, NsRange r, NsRange *actual)
{
    NsRange was = actual ? *actual : NsRange{};
    A11y   *x   = mactext();
    NsRect rc = {};
    if (!x || !macfirstof(x, r, &rc, actual)) {
        rc = macfirstrectimp(self, sel, r, actual);
    }
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 128);
        print(&b, "firstRectForCharacterRange:actualRange: ");
        macrange(&b, r);
        print(&b, " -> ");
        macrect(&b, rc);
        macactual(&b, actual, was);
        macnote(l, a, &b);
    }
    return rc;
}

// The insertion point nearest a point of the screen
static uz macindexat(void *self, void *sel, NsPoint p)
{
    A11y *x = mactext();
    uz    i = NSNOTFOUND;
    f32   px = 0, py = 0;
    if (!x) {
        i = macindexatimp(self, sel, p);
    } else if (x->text.geom.rev==x->text.rev && macfromscreen(p, &px, &py)) {
        MacDoc d = {x, {}};
        i32    b = axtodoc(x, axoffsetat(x, px, py));
        i = macunits(&d, {b, b}).loc;
    }
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 128);
        print(&b, "characterIndexForPoint: (");
        print(&b, axround((f32)p.x));
        putbyte(&b, ',');
        print(&b, axround((f32)p.y));
        print(&b, ") -> ");
        if (i == NSNOTFOUND) {
            print(&b, "NotFound");
        } else {
            print(&b, (i64)i);
        }
        macnote(l, a, &b);
    }
    return i;
}

// The end of byte b's line, before its newline
static i32 maclineend(A11y *x, i32 b)
{
    Str    s = x->text.text;
    AxSpan l = axlinespan(x, axlineat(x, b));
    return l.hi>l.lo && s[l.hi-1]=='\n' ? l.hi-1 : l.hi;
}

// A command from no key, Dictation's or Voice Control's, done to the
// model as Cocoa's text views do it: a delete takes the selection if
// there is one; a move collapses it. 0 for one not known here.
static Str const maccommands[] = {
    "insertNewline:", "insertLineBreak:", "insertParagraphSeparator:",
    "insertNewlineIgnoringFieldEditor:", "deleteBackward:",
    "deleteBackwardByDecomposingPreviousCharacter:", "deleteForward:",
    "deleteWordBackward:", "deleteWordForward:", "deleteToBeginningOfLine:",
    "deleteToEndOfLine:", "deleteToBeginningOfParagraph:", "deleteToEndOfParagraph:",
    "moveLeft:", "moveBackward:", "moveRight:", "moveForward:", "moveWordLeft:",
    "moveWordBackward:", "moveWordRight:", "moveWordForward:", "moveToBeginningOfLine:",
    "moveToLeftEndOfLine:", "moveToEndOfLine:", "moveToRightEndOfLine:",
    "moveToBeginningOfParagraph:", "moveToEndOfParagraph:",
    "moveToBeginningOfDocument:", "moveToEndOfDocument:", "selectAll:",
};

static b32 macknown(Str name)
{
    b32 known = 0;
    for (i32 i = 0; i < countof(maccommands); i++) {
        known |= maccommands[i] == name;
    }
    return known;
}

static b32 macdocommand(void *self, A11y *x, Str name)
{
    if (!macknown(name)) {
        return 0;
    } else if (x->text.marked.len) {
        maccommit(self, x);  // a command ends a composition
    }
    Gui    *g   = axmac->g;
    AxText *t   = &x->text;
    Str     s   = t->text;
    i32     n   = (i32)s.len;
    i32     c   = t->caret;
    AxSpan  sel = axselection(t);
    i32     to  = -1;  // a delete's other end, from the caret
    i32     at  = -1;  // a move's
    iz      e   = c;
    iz      p   = c;
    if (c > 0) {
        utf8prev(s, c, &p);
    }
    if (c < n) {
        utf8decode(s, &e);
    }

    if (name=="insertNewline:" || name=="insertLineBreak:" ||
            name=="insertParagraphSeparator:" || name=="insertNewlineIgnoringFieldEditor:") {
        axtextedit(g, sel.lo, sel.hi, "\n", AX_AFTER, AX_AFTER);
        return 1;
    } else if (name=="deleteBackward:" || name=="deleteBackwardByDecomposingPreviousCharacter:") {
        to = (i32)p;
    } else if (name == "deleteForward:") {
        to = (i32)e;
    } else if (name == "deleteWordBackward:") {
        to = axwordback(s, c);
    } else if (name == "deleteWordForward:") {
        to = axwordnext(s, c);
    } else if (name=="deleteToBeginningOfLine:") {
        to = axlinespan(x, axlineat(x, c)).lo;
    } else if (name == "deleteToEndOfLine:") {
        to = maclineend(x, c);
    } else if (name == "deleteToBeginningOfParagraph:") {
        to = axparaat(s, c).lo;
    } else if (name == "deleteToEndOfParagraph:") {
        AxSpan q = axparaat(s, c);
        to = q.hi>q.lo && s[q.hi-1]=='\n' ? q.hi-1 : q.hi;
    } else if (name=="moveLeft:" || name=="moveBackward:") {
        at = sel.hi>sel.lo ? sel.lo : (i32)p;
    } else if (name=="moveRight:" || name=="moveForward:") {
        at = sel.hi>sel.lo ? sel.hi : (i32)e;
    } else if (name=="moveWordLeft:" || name=="moveWordBackward:") {
        at = axwordback(s, sel.lo);
    } else if (name=="moveWordRight:" || name=="moveWordForward:") {
        at = axwordnext(s, sel.hi);
    } else if (name=="moveToBeginningOfLine:" || name=="moveToLeftEndOfLine:") {
        at = axlinespan(x, axlineat(x, c)).lo;
    } else if (name=="moveToEndOfLine:" || name=="moveToRightEndOfLine:") {
        at = maclineend(x, c);
    } else if (name == "moveToBeginningOfParagraph:") {
        at = axparaat(s, c).lo;
    } else if (name == "moveToEndOfParagraph:") {
        AxSpan q = axparaat(s, c);
        at = q.hi>q.lo && s[q.hi-1]=='\n' ? q.hi-1 : q.hi;
    } else if (name == "moveToBeginningOfDocument:") {
        at = 0;
    } else if (name == "moveToEndOfDocument:") {
        at = n;
    } else if (name == "selectAll:") {
        axtextselect(g, 0, n);
        return 1;
    } else {
        return 0;
    }

    if (to >= 0) {
        AxSpan d = sel.hi>sel.lo ? sel : AxSpan{to<c ? to : c, to<c ? c : to};
        if (d.hi > d.lo) {
            axtextedit(g, d.lo, d.hi, "", d.lo, d.lo);
        }
    } else if (at >= 0) {
        axtextselect(g, at, at);
    }
    return 1;
}

static void maccommand(void *self, void *sel, void *cmd)
{
    b32         keyed = mackeyed();
    char const *cn    = cmd ? sel_getName(cmd) : "nil";
    Str         name  = {(u8 *)cn, (iz)__builtin_strlen(cn)};
    if (!keyed && macknown(name)) {
        macwant("doCommandBySelector");
    }
    A11y *x = mactext();
    if (AxGui *l = maclog()) {
        Arena a = l->logmem;
        Buf   b(&a, 64);
        print(&b, "doCommandBySelector: ");
        print(&b, name);
        macnote(l, a, &b);
    }
    if (x && keyed) {
        macspokenend(self, x);  // the key is the field's, not Dictation's
    }
    if (!x || keyed || !macdocommand(self, x, name)) {
        maccommandimp(self, sel, cmd);  // a key's: SDL sends it as a key
    } else {
        macwake();
    }
}

static void *macvalidattrs(void *self, void *sel)
{
    void *r = macvalidattrsimp(self, sel);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 128);
        print(&b, "validAttributesForMarkedText -> ");
        macdesc(&b, r, 300);
        macnote(x, a, &b);
    }
    return r;
}


// SDL3View's and the text input's methods, answering for the root

static void *macviewcls, *macrespcls;

// The superclass's method, past a subclass AppKit may have made of ours
// at run time (key-value observing), which would call ours again
static void *macviewsuper(void *self, void *sel)
{
    void *c = object_getClass(self);
    while (c && c!=macviewcls && c!=macrespcls) {
        c = class_getSuperclass(c);
    }
    return class_getMethodImplementation(class_getSuperclass(c), sel);
}

// Whether self is the window's own view, not the hidden one SDL waits
// on while the window is shed
static b32 macours(void *self)
{
    void *win = macwindow();
    return win && objcsend<void *>(self, "window")==win;
}

static void *macviewkids(void *self, void *sel)
{
    if (!macours(self)) {
        return ((void *(*)(void *, void *))macviewsuper(self, sel))(self, sel);
    }
    macax.gets[MG_TREE]++;
    if (!macready("AXChildren")) {
        return objcsend<void *>(objc_getClass("NSArray"), "array");  // a bare root, for now
    }
    macax.rootasked = 1;
    return mackidsof(0);
}

static void *macviewhit(void *self, void *sel, NsPoint p)
{
    using F = void *(*)(void *, void *, NsPoint);
    f32 x = 0, y = 0;
    if (macours(self) && macready("hit test") && macfromscreen(p, &x, &y)) {
        macax.gets[MG_HIT]++;
        i32 i = machit(x, y);
        if (i > 0) {
            return macelof(i);
        }
    }
    return ((F)macviewsuper(self, sel))(self, sel, p);
}

static void *macviewfocus(void *self, void *sel)
{
    if (macours(self) && macready("focus")) {
        macax.gets[MG_FOCUS]++;
        if (void *el = macfocused()) {
            return el;
        }
    }
    return ((void *(*)(void *, void *))macviewsuper(self, sel))(self, sel);
}


// Counted, so the checklist sees that each element made is let go
static void macedealloc(void *self, void *sel)
{
    macax.alive--;
    void *up = class_getSuperclass(macax.cls);
    ((void (*)(void *, void *))class_getMethodImplementation(up, sel))(self, sel);
}

// The input method was told to drop its composition (os_guiimereset,
// gui_sdl.cpp): a send, a draft put, a recall, another field, or a reset
// of the terminal's dead keys. If it was Dictation's, that is over, and
// ranges SDL gave are no one's now. The drawn composition goes too: only
// imedrop's caller cleared it, and Dictation, which keeps its marked text
// until told, may never take it back. The model drops it at once, not at
// the next frame's sync, as a tool may ask before then.
static void macimedropped()
{
    if (!axmac) {
        return;
    }
    A11y *x = axmac->ax;
    if (AxGui *l = maclog(); l && (macspoken || x->text.marked.len)) {
        axnote(l, l->logmem, "the composition is dropped");
    }
    gui_preedit(axmac->g, {}, 0, 0);
    macspoken    = 0;
    macsdlranges = 0;
    if (x->text.marked.len) {
        axmark(x, {}, 0, 0);
    }
}


// After each publish: tell AppKit what changed, of the elements a tool
// has, and let go of those whose nodes are gone.

static void macpost(void *el, i32 note)
{
    if (el) {
        NSAccessibilityPostNotification(el, nsstring(macnotes[note]));
        macax.posted[note]++;
    }
}

// The text's selection changed, with how when it only moved: a tool may
// read the text again after an edit, and only the selection after a move
static void macposttext(void *el, b32 edited)
{
    if (!el) {
        return;
    } else if (edited) {
        macpost(el, MN_SELTEXT);
        macax.seltype = 0;
        return;
    }
    enum { SELECTIONMOVE = 2 };  // AXTextStateChangeType's
    void *v    = objcsend<void *>(objc_getClass("NSNumber"), "numberWithInt:", (int)SELECTIONMOVE);
    void *info = objcsend<void *>(objc_getClass("NSDictionary"), "dictionaryWithObject:forKey:",
                                  v, nsstring("AXTextStateChangeType"));
    NSAccessibilityPostNotificationWithUserInfo(el, nsstring(macnotes[MN_SELTEXT]), info);
    macax.posted[MN_SELTEXT]++;
    macax.seltype = SELECTIONMOVE;
}

static void macgets(AxGui *l)
{
    i32 total = 0;
    for (i32 k = 0; k < MG_N; k++) {
        total += macax.gets[k];
    }
    if (!total || !l) {
        return;
    }
    Arena a = l->logmem;
    Buf   b(&a, 256);
    print(&b, "asked");
    for (i32 k = 0; k < MG_N; k++) {
        if (macax.gets[k]) {
            putbyte(&b, ' ');
            print(&b, Str{(u8 *)macgetnames[k], (iz)__builtin_strlen(macgetnames[k])});
            putbyte(&b, ' ');
            print(&b, (i64)macax.gets[k]);
        }
    }
    print(&b, ", elements ");
    print(&b, (i64)macax.len);
    axnote(l, a, finish(&b));
}

static void macpublished(AxGui *x)
{
    AxTree *t    = x->ax->pub;
    Gui    *g    = x->g;
    void   *pool = objc_autoreleasePoolPush();
    void   *win  = macwindow();
    b32     key  = win && objcsend<ObjcBool>(win, "isKeyWindow");
    b32     layout = macax.started || macax.layoutowed || macax.resizing;
    macax.pass   = macax.pass+1 ? macax.pass+1 : 1;  // never 0: no publish
    macax.inpass = 1;

    for (iz e = 0; e < t->events.len; e++) {
        AxEvent ev = t->events[e];
        i32     i  = axfind(t, ev.key);
        switch (ev.kind) {
        case AXE_DESTROYED: {
            i32 s = macslot(ev.key);
            if (s >= 0) {
                void *el = macax.tab[s].el;
                macunslot(s);
                *macdeadp(el) = MACDEAD_DYING;
                macpost(el, MN_DESTROYED);
                *macdeadp(el) = MACDEAD_GONE;
                objcsend<void>(el, "release");
            }
        } break;
        case AXE_CREATED:
            // Under a parent a tool knew of before this publish, so its
            // children's change. Not under one made just now: its own
            // creation tells of what is under it.
            if (i>0 && t->nodes[i].role!=AX_NONE) {
                i32 p = macparent(t, i);
                i32 s = p ? macslot(t->nodes[p].key) : -1;
                if (p ? s>=0 && macax.tab[s].born!=macax.pass : macax.rootasked) {
                    macpost(macelof(i), MN_CREATED);
                }
            }
            break;
        case AXE_NAME:
            // Where the name is: a static text's value, a menu item's
            // title, otherwise its label, which has no notice of its own
            if (void *el = i>0 ? macelif(i) : 0) {
                switch (t->nodes[i].role) {
                case AX_TEXT:     macpost(el, MN_VALUE); break;
                case AX_MENUITEM: macpost(el, MN_TITLE); break;
                default:          layout = 1;
                }
            }
            break;
        case AXE_VALUE:
        case AXE_STATE:
            macpost(i>0 ? macelif(i) : 0, MN_VALUE);
            break;
        case AXE_FOCUS:
            // Only the window the user is in has the focus to tell of
            if (key && i>0) {
                macpost(macfocused(), MN_FOCUS);
            }
            break;
        case AXE_CHILDREN:
            break;  // as layout
        case AXE_LAYOUT:
            layout = 1;
            break;
        }
    }

    // The field's selection, or its marked text, moved
    AxText *tx = &x->ax->text;
    if (tx->live) {
        // The document holds the marked text, so its change is an edit,
        // though the text's revision stays
        u64 mh     = hash(tx->marked);
        b32 edited = tx->rev!=macax.trev || mh!=macax.tmarked;
        b32 moved  = tx->key==macax.tkey && (edited ||
                     tx->anchor!=macax.tanchor || tx->caret!=macax.tcaret ||
                     tx->mlo!=macax.tmlo || tx->mhi!=macax.tmhi);
        if (moved) {
            i32 i = axfind(t, tx->key);
            macposttext(i>0 ? macelif(i) : 0, edited);
        }
        macax.tkey    = tx->key;
        macax.trev    = tx->rev;
        macax.tanchor = tx->anchor;
        macax.tcaret  = tx->caret;
        macax.tmarked = mh;
        macax.tmlo    = tx->mlo;
        macax.tmhi    = tx->mhi;
    } else {
        macax.tkey = 0;
    }

    // In a nested loop (a live resize) frames come fast: tell less often.
    // What is held is owed. SDL's timer draws on while the user holds
    // still, and leaves an expose queued for the main loop after, so a
    // frame tells it; the frame asked for once the gap is up is only a
    // safeguard, for a loop that ends with nothing queued.
    i64 now  = g->sched.now;
    b32 tell = layout && win && (!g->inmodal || now-macax.layoutat>=MACLAYOUT_GAP);
    if (tell) {
        macpost(NSAccessibilityUnignoredAncestor(objcsend<void *>(win, "contentView")),
                MN_LAYOUT);
        macax.startposts += macax.started;
        macax.layoutat   = now;
        macax.layoutowed = 0;
        macax.started    = 0;
    } else if (layout) {
        macax.layoutowed = 1;
        if (win) {
            schedat(&g->sched, macax.layoutat+MACLAYOUT_GAP, DUE_APP);
        }
    }
    if (layout && g->inmodal) {
        macax.modal0      = macax.modalposts+macax.modalheld ? macax.modal0 : now;
        macax.modal1      = now;
        macax.modalposts += tell;
        macax.modalheld  += !tell;
    }

    macax.inpass = 0;
    macgets(maclog());
    for (i32 k = 0; k < MG_N; k++) {
        macax.gets[k] = 0;
    }
    objc_autoreleasePoolPop(pool);
}


// The checklist's "ns" verbs (driveax): the tree through AppKit, as a
// tool in this process would ask it, from the window down
//   start              ask the view's children: a bare root, if the
//                      tree wasn't on, and on from then
//   started            AppKit told of the tree once a start published it
//   has AXROLE NAME    an element so named, its parents up to the window
//   focus AXROLE NAME  the window's focused element, or its first
//                      responder's while it isn't key
//   press AXROLE NAME  its AXPress: check that it's taken
//   can ACTS AXROLE NAME  its actions (AXPress, AXShowMenu) include each,
//                      a comma between, and not those with a '!'
//   attr ATTR VALUE AXROLE NAME  an attribute, by its description
//   hit AXROLE NAME    the window's hit test at its frame's middle
//   mark               forget what was posted, and told for a start
//   posted NOTE [N]    AppKit was told NOTE since the mark, N times if given
//   nofocus            the focused element, asked as AppKit would, is none
//                      of ours (the window isn't the user's)
//   alive              every element made and let go was deallocated: those
//                      left are the table's (and the one kept)
//   keep AXROLE NAME   keep an element, past its node
//   kept               the one kept, its node gone, answers as nothing, and
//                      is let go
//   resizing 1|0       each publish changes layout, as a live resize's do
//   throttled          in the nested loop since "resizing 1", layout was
//                      told no more than the gap allows, and some held
//   owed N             a layout change is held (1) or not (0)
//   seltype N          the last AXSelectedTextChanged since the mark had
//                      AXTextStateChangeType N, 0 for none
// and of the focused element's text, as Voice Control asks it (a range
// is LOC,LEN in UTF-16 units):
//   tget ATTR WANT     an attribute: AXValue, AXNumberOfCharacters,
//                      AXSelectedText, AXSelectedTextRange(s),
//                      AXVisibleCharacterRange, AXInsertionPointLineNumber
//   tset ATTR VALUE    set AXValue, AXSelectedText, AXSelectedTextRange(s)
//   tparam ATTR ARG WANT  a parameterized attribute: AXLineForIndex,
//                      AXRangeForLine, AXStringForRange,
//                      AXAttributedStringForRange, AXRangeForIndex,
//                      AXStyleRangeForIndex
//   tround N           the bounds of character N lie in the element's
//                      frame, and its middle's character is N
//   tsettable ATTRS    each attribute is settable, a comma between, and
//                      not those with a '!'
// and of the window's text input, as Dictation calls it: "ti ..." and
// "replay FILE" (mactidrive, macreplay).

static Str macstrof(void *o, char const *sel)
{
    if (!o || !objcsend<ObjcBool>(o, "respondsToSelector:", sel_registerName(sel))) {
        return {};
    }
    void *s = objcsend<void *>(o, sel);
    return maciskind(s, "NSString") ? macutf8(s) : Str{};
}

static void *macfindel(Arena a, void *win, Str role, Str name)
{
    enum { MAXSEEN = 1<<15 };
    void **stack = alloc<void *>(&a, MAXSEEN);
    i32    top   = 0;
    void  *found = 0;
    stack[top++] = win;
    void  *sel   = sel_registerName("accessibilityChildren");
    for (i32 seen = 0; top && seen<MAXSEEN; seen++) {
        void *o = stack[--top];
        if (o != win && macstrof(o, "accessibilityRole")==role &&
                (macstrof(o, "accessibilityLabel")==name ||
                 macstrof(o, "accessibilityTitle")==name)) {
            found = found ? found : o;  // the first: the walk goes front first
        }
        void *kids = objcsend<ObjcBool>(o, "respondsToSelector:", sel)
                     ? objcsend<void *>(o, "accessibilityChildren") : 0;
        uz    n    = kids ? objcsend<uz>(kids, "count") : 0;
        for (uz k = 0; k<n && top<MAXSEEN; k++) {
            stack[top++] = objcsend<void *>(kids, "objectAtIndex:", k);  // front on top
        }
    }
    return found;
}

// The focused element, as AppKit answers a tool: the window's while it is
// key, which the checklist's mostly isn't; then, as AppKit would ask once
// it is, its first responder's, a view of ours (the text input's, or the
// window's own, with none)
static void *macfocusof(void *win, void *view)
{
    b32   key = objcsend<ObjcBool>(win, "isKeyWindow");
    void *fr  = objcsend<void *>(win, "firstResponder");
    void *at  = key ? win : maciskind(fr, "NSView") ? fr : view;
    return objcsend<void *>(at, "accessibilityFocusedUIElement");
}

static NsRange macparserange(Str s)
{
    Cut c = cut(s, ',');
    return {(uz)parseint(c.head).val, (uz)parseint(c.tail).val};
}

static void macputrange(Buf *b, NsRange r)
{
    print(b, (i64)r.loc);
    putbyte(b, ',');
    print(b, (i64)r.len);
}

static void macputstr(Buf *b, void *o)
{
    if (maciskind(o, "NSAttributedString")) {
        o = objcsend<void *>(o, "string");
    }
    print(b, maciskind(o, "NSString") ? macutf8(o) : Str("(nil)"));
}

// The setter for attribute name, or null
static char const *macsetter(Str attr)
{
    static struct { Str attr; char const *sel; } const ss[] = {
        {"AXValue",              "setAccessibilityValue:"},
        {"AXSelectedText",       "setAccessibilitySelectedText:"},
        {"AXSelectedTextRange",  "setAccessibilitySelectedTextRange:"},
        {"AXSelectedTextRanges", "setAccessibilitySelectedTextRanges:"},
        {"AXFocused",            "setAccessibilityFocused:"},
    };
    for (i32 i = 0; i < countof(ss); i++) {
        if (ss[i].attr == attr) {
            return ss[i].sel;
        }
    }
    return 0;
}

// The text verbs (tget, tset, tparam, tround, tsettable) on el
static void mactextdrive(Gui *g, Arena *a, void *el, Str verb, Str args, Str what)
{
    Cut av   = cut(args, ' ');
    Str attr = av.head;
    Str want = av.tail;
    Buf b(a, 256);
    b32 known = 1;
    if (!el) {
        axcheck(g, a, 0, concat(a, what, ": no focused element"));
        return;
    }

    if (verb == "tget") {
        if (attr=="AXValue" || attr=="AXSelectedText") {
            macputstr(&b, objcsend<void *>(el, attr=="AXValue" ? "accessibilityValue"
                                                               : "accessibilitySelectedText"));
        } else if (attr == "AXNumberOfCharacters") {
            print(&b, (i64)objcsend<iz>(el, "accessibilityNumberOfCharacters"));
        } else if (attr == "AXInsertionPointLineNumber") {
            print(&b, (i64)objcsend<iz>(el, "accessibilityInsertionPointLineNumber"));
        } else if (attr == "AXSelectedTextRange") {
            macputrange(&b, objcsend<NsRange>(el, "accessibilitySelectedTextRange"));
        } else if (attr == "AXVisibleCharacterRange") {
            macputrange(&b, objcsend<NsRange>(el, "accessibilityVisibleCharacterRange"));
        } else if (attr == "AXSelectedTextRanges") {
            void *arr = objcsend<void *>(el, "accessibilitySelectedTextRanges");
            uz    n   = maciskind(arr, "NSArray") ? objcsend<uz>(arr, "count") : 0;
            for (uz i = 0; i < n; i++) {
                if (i) {
                    putbyte(&b, ' ');
                }
                macputrange(&b, objcsend<NsRange>(objcsend<void *>(arr, "objectAtIndex:", i),
                                                  "rangeValue"));
            }
        } else {
            known = 0;
        }

    } else if (verb == "tset") {
        if (attr=="AXValue" || attr=="AXSelectedText") {
            objcsend<void>(el, attr=="AXValue" ? "setAccessibilityValue:"
                                               : "setAccessibilitySelectedText:", macnsstr(want));
        } else if (attr == "AXSelectedTextRange") {
            objcsend<void>(el, "setAccessibilitySelectedTextRange:", macparserange(want));
        } else if (attr == "AXSelectedTextRanges") {
            void *v = objcsend<void *>(objc_getClass("NSValue"), "valueWithRange:",
                                       macparserange(want));
            objcsend<void>(el, "setAccessibilitySelectedTextRanges:",
                           objcsend<void *>(objc_getClass("NSArray"), "arrayWithObject:", v));
        } else {
            axcheck(g, a, 0, concat(a, what, ": no such attribute"));
            return;
        }
        guilog(g, concat(a, "drive: ", what));
        return;

    } else if (verb == "tparam") {
        Cut pv  = cut(want, ' ');
        Str arg = pv.head;
        iz  i   = (iz)parseint(arg).val;
        want = pv.tail;
        if (attr == "AXLineForIndex") {
            print(&b, (i64)objcsend<iz>(el, "accessibilityLineForIndex:", i));
        } else if (attr == "AXRangeForLine") {
            macputrange(&b, objcsend<NsRange>(el, "accessibilityRangeForLine:", i));
        } else if (attr == "AXRangeForIndex") {
            macputrange(&b, objcsend<NsRange>(el, "accessibilityRangeForIndex:", i));
        } else if (attr == "AXStyleRangeForIndex") {
            macputrange(&b, objcsend<NsRange>(el, "accessibilityStyleRangeForIndex:", i));
        } else if (attr == "AXStringForRange") {
            macputstr(&b, objcsend<void *>(el, "accessibilityStringForRange:", macparserange(arg)));
        } else if (attr == "AXAttributedStringForRange") {
            macputstr(&b, objcsend<void *>(el, "accessibilityAttributedStringForRange:",
                                           macparserange(arg)));
        } else {
            known = 0;
        }

    } else if (verb == "tround") {
        // Through the screen and back: character N's bounds, then the
        // character at their middle
        iz      i  = (iz)parseint(attr).val;
        NsRect  f  = objcrect(el, "accessibilityFrame");
        NsRange c  = objcsend<NsRange>(el, "accessibilityRangeForIndex:", i);
        NsRect  r  = objcrect(el, "accessibilityFrameForRange:", c);
        NsRange at = objcsend<NsRange>(el, "accessibilityRangeForPosition:",
                                       NsPoint{r.x + r.w/2, r.y + r.h/2});
        b32 in = r.w>0 && r.h>0 && r.x>=f.x-1 && r.y>=f.y-1 &&
                 r.x+r.w<=f.x+f.w+1 && r.y+r.h<=f.y+f.h+1;
        b32 ok = in && at.loc==c.loc && at.len==c.len;
        print(&b, what);
        if (!ok) {
            print(&b, ": character ");
            macputrange(&b, c);
            print(&b, " at ");
            macrect(&b, r);
            print(&b, in ? Str(", in ") : Str(", not in "));
            macrect(&b, f);
            print(&b, ", found ");
            macputrange(&b, at);
        }
        axcheck(g, a, ok, finish(&b));
        return;

    } else if (verb == "tsettable") {
        // As AppKit lists them for a tool: the setter, if allowed
        b32 ok = 1;
        for (Cut w = cut(attr, ','); w.head.len; w = cut(w.tail, ',')) {
            b32         no  = w.head[0] == '!';
            char const *sel = macsetter(no ? cuthead(w.head, 1) : w.head);
            b32         has = sel && objcsend<ObjcBool>(el, "respondsToSelector:",
                                                        sel_registerName(sel)) &&
                              objcsend<ObjcBool>(el, "isAccessibilitySelectorAllowed:",
                                                 sel_registerName(sel));
            ok &= sel && has!=no;
        }
        axcheck(g, a, ok, what);
        return;
    }

    if (!known) {
        axcheck(g, a, 0, concat(a, what, ": no such attribute"));
        return;
    }
    Str got = finish(&b);
    axcheck(g, a, got==want, got==want ? what : concat(a, what, concat(a, ", not ", got)));
}

// The text input's calls (ti), on the window's first responder, as
// Dictation makes them, from no key (a range is LOC,LEN in UTF-16 units,
// or "none" for NSNotFound; text is the rest of the line; " ; " between
// calls makes them in one step, with no frame between):
//   ti key CALL        CALL as though inside a key down's dispatch
//                      (mackeyed), as macsend counts one
//   ti insert R TEXT   insertText:replacementRange:, an NSString
//   ti ainsert R TEXT  the same, an NSAttributedString
//   ti mark S R TEXT   setMarkedText:selectedRange:replacementRange:,
//                      attributed
//   ti unmark          unmarkText
//   ti cmd SELECTOR    doCommandBySelector:
//   ti sel R           selectedRange is R
//   ti marked R        markedRange is R
//   ti has 0|1         hasMarkedText
//   ti sub R ACTUAL TEXT  attributedSubstringForProposedRange:actualRange:
//                      gives TEXT, and ACTUAL ("nil" for none)
//   ti round N         firstRectForCharacterRange: of character N lies in
//                      the focused element's frame, and
//                      characterIndexForPoint: at its left edge is N
static NsRange macparsens(Str s)
{
    return s=="none" ? NsRange{NSNOTFOUND, 0} : macparserange(s);
}

static void *macattrstr(void *s)
{
    void *o = objcsend<void *>(objc_getClass("NSAttributedString"), "alloc");
    o = o ? objcsend<void *>(o, "initWithString:", s) : 0;
    return o ? objcsend<void *>(o, "autorelease") : 0;
}

// The window's text input, SDL's, or null
static void *macresponder(void *win)
{
    void *r = win ? objcsend<void *>(win, "firstResponder") : 0;
    return maciskind(r, "SDL3TranslatorResponder") ? r : 0;
}

static void mactidrive(Gui *g, Arena *a, void *win, void *view, Str args, Str what)
{
    void *resp = macresponder(win);
    Cut   c    = cut(args, ' ');
    Str   verb = c.head;
    Buf   b(a, 256);
    if (!resp) {
        axcheck(g, a, 0, concat(a, what, ": no text input"));
        return;
    }

    if (verb == "key") {
        mackeydepth++;
        mactidrive(g, a, win, view, c.tail, what);
        mackeydepth--;
        mackeydone();
        return;
    } else if (verb=="insert" || verb=="ainsert") {
        Cut   r = cut(c.tail, ' ');
        void *s = macnsstr(r.tail, *a);
        objcsend<void>(resp, "insertText:replacementRange:",
                       verb=="ainsert" ? macattrstr(s) : s, macparsens(r.head));
    } else if (verb == "mark") {
        Cut sr = cut(c.tail, ' ');
        Cut rr = cut(sr.tail, ' ');
        objcsend<void>(resp, "setMarkedText:selectedRange:replacementRange:",
                       macattrstr(macnsstr(rr.tail, *a)), macparsens(sr.head),
                       macparsens(rr.head));
    } else if (verb == "unmark") {
        objcsend<void>(resp, "unmarkText");
    } else if (verb == "cmd") {
        objcsend<void>(resp, "doCommandBySelector:", sel_registerName(cstr0(a, c.tail)));
    } else if (verb=="sel" || verb=="marked") {
        NsRange r = objcsend<NsRange>(resp, verb=="sel" ? "selectedRange" : "markedRange");
        if (r.loc == NSNOTFOUND) {
            print(&b, "none");
        } else {
            macputrange(&b, r);
        }
        Str got = finish(&b);
        axcheck(g, a, got==c.tail, got==c.tail ? what : concat(a, what, concat(a, ", not ", got)));
        return;
    } else if (verb == "has") {
        Str got = objcsend<ObjcBool>(resp, "hasMarkedText") ? Str("1") : Str("0");
        axcheck(g, a, got==c.tail, got==c.tail ? what : concat(a, what, concat(a, ", not ", got)));
        return;
    } else if (verb == "sub") {
        Cut     rv  = cut(c.tail, ' ');
        Cut     av  = cut(rv.tail, ' ');
        NsRange act = {NSNOTFOUND, 0};
        void   *s   = objcsend<void *>(resp, "attributedSubstringForProposedRange:actualRange:",
                                       macparsens(rv.head), &act);
        if (!s) {
            print(&b, "nil");
        } else {
            macputrange(&b, act);
            putbyte(&b, ' ');
            macputstr(&b, s);
        }
        Str got  = finish(&b);
        Str want = av.head=="nil" ? av.head : rv.tail;
        axcheck(g, a, got==want, got==want ? what : concat(a, what, concat(a, ", not ", got)));
        return;
    } else if (verb == "round") {
        iz      i  = (iz)parseint(c.tail).val;
        void   *el = macfocusof(win, view);
        NsRect  f  = el ? objcrect(el, "accessibilityFrame") : NsRect{};
        NsRange ac = {};
        NsRect  r  = objcrect(resp, "firstRectForCharacterRange:actualRange:",
                              NsRange{(uz)i, 1}, &ac);
        uz      at = objcsend<uz>(resp, "characterIndexForPoint:",
                                  NsPoint{r.x + 1, r.y + r.h/2});
        b32 in = r.w>0 && r.h>0 && r.x>=f.x-1 && r.y>=f.y-1 &&
                 r.x+r.w<=f.x+f.w+1 && r.y+r.h<=f.y+f.h+1;
        b32 ok = in && at==(uz)i && ac.loc==(uz)i && ac.len>=1;
        print(&b, what);
        if (!ok) {
            print(&b, ": ");
            macrect(&b, r);
            print(&b, in ? Str(" in ") : Str(" not in "));
            macrect(&b, f);
            print(&b, ", actual ");
            macputrange(&b, ac);
            print(&b, ", index ");
            print(&b, at==NSNOTFOUND ? (i64)-1 : (i64)at);
        }
        axcheck(g, a, ok, finish(&b));
        return;
    } else {
        axcheck(g, a, 0, concat(a, what, ": no such call"));
        return;
    }
    guilog(g, concat(a, "drive: ", what));
}

// "ns replay FILE": the calls of a trace (PROSPERO_AXTRACE's file, or the
// Metrics pane's copied log), FILE in the checklist's --cwd, made again
// in order, from no key: NSTextInputClient's insertText:, setMarkedText:,
// unmarkText and doCommandBySelector: on the window's text input, and the
// AXValue, AXSelectedText and AXSelectedTextRange(s) sets on its focused
// element. Queries are left out: their answers are this run's. A call
// whose text the trace withheld (typed, no field, a password) or cut is
// skipped, and counted. A repeated line (×N, or the trace's "the line
// before, N times") is made N times.

// A logged string (axquote), from its opening quote; s is left past its
// closing one. 0 if there is none, or the log cut it (…+N follows).
static b32 macunquote(Arena *a, Str *s, Str *out)
{
    if (!s->len || (*s)[0]!='"') {
        return 0;
    }
    Buf b(a, s->len);
    iz  i = 1;
    for (; i<s->len && (*s)[i]!='"'; i++) {
        u8 c = (*s)[i];
        if (c=='\\' && i+1<s->len) {
            u8 e = (*s)[++i];
            c = e;
            if (e == 'n') {
                c = '\n';
            } else if (e=='x' && i+2<s->len && hexval((*s)[i+1])>=0 && hexval((*s)[i+2])>=0) {
                c = (u8)(hexval((*s)[i+1])<<4 | hexval((*s)[i+2]));
                i += 2;
            }
        }
        putbyte(&b, c);
    }
    if (i == s->len) {
        return 0;
    }
    *s   = cuthead(*s, i+1);
    *out = finish(&b);
    return !startswith(*s, "\xe2\x80\xa6+");
}

// The last n ranges in s ("{LOC,LEN}", LOC possibly "NotFound"), in order
static b32 maclastranges(Str s, NsRange *rs, i32 n)
{
    i32 got = 0;
    for (iz i = s.len; i>0 && got<n; i--) {
        if (s[i-1] != '}') {
            continue;
        }
        iz j = i-1;
        while (j>0 && s[j-1]!='{') {
            j--;
        }
        if (!j) {
            break;
        }
        Cut c   = cut(span(s.data+j, s.data+i-1), ',');
        Int lo  = parseint(c.head);
        Int len = parseint(c.tail);
        if (!len.ok || (!lo.ok && !(c.head=="NotFound"))) {
            continue;  // braces of something else: an attribute's
        }
        rs[n-1-got++] = {c.head=="NotFound" ? NSNOTFOUND : (uz)lo.val, (uz)len.val};
        i = j;
    }
    return got == n;
}

// Make the call on line, a line of the trace less its time: 1 made, 0
// not a call, -1 a call that can't be made again
static i32 macreplayline(Arena a, void *resp, void *el, Str line)
{
    static Str const calls[] = {
        "insertText:replacementRange: ",
        "setMarkedText:selectedRange:replacementRange: ",
        "unmarkText",
        "doCommandBySelector: ",
        "AXValue= ",
        "AXSelectedText= ",
        "AXSelectedTextRange= ",
        "AXSelectedTextRanges= ",
    };
    i32 k = -1;
    for (i32 i = 0; k<0 && i<countof(calls); i++) {
        k = startswith(line, calls[i]) ? i : k;
    }
    if (k < 0) {
        return 0;
    }
    Str rest = cuthead(line, calls[k].len);
    Str text = {};
    NsRange rs[2] = {};
    b32 hastext = k==0 || k==1 || k==4 || k==5;
    if (hastext && !macunquote(&a, &rest, &text)) {
        return -1;
    }
    void *s = hastext ? macnsstr(text, a) : 0;
    if (hastext && contains(rest, "attrs[")) {
        s = macattrstr(s);  // as given: Dictation's are attributed
    }
    switch (k) {
    case 0:
        if (!maclastranges(rest, rs, 1)) {
            return -1;
        }
        objcsend<void>(resp, "insertText:replacementRange:", s, rs[0]);
        break;
    case 1:
        if (!maclastranges(rest, rs, 2)) {
            return -1;
        }
        objcsend<void>(resp, "setMarkedText:selectedRange:replacementRange:", s, rs[0], rs[1]);
        break;
    case 2:
        if (rest.len && !startswith(rest, " (")) {
            return 0;  // another call that starts the same
        }
        objcsend<void>(resp, "unmarkText");
        break;
    case 3: {
        Cut c = cut(rest, ' ');
        if (!c.head.len || c.head=="nil") {
            return -1;
        }
        objcsend<void>(resp, "doCommandBySelector:", sel_registerName(cstr0(&a, c.head)));
    } break;
    case 4:
    case 5:
        if (!el) {
            return -1;
        }
        objcsend<void>(el, k==4 ? "setAccessibilityValue:" : "setAccessibilitySelectedText:", s);
        break;
    case 6:
    case 7:
        if (!el || !maclastranges(cut(rest, ' ').head, rs, 1)) {
            return -1;
        } else if (k == 6) {
            objcsend<void>(el, "setAccessibilitySelectedTextRange:", rs[0]);
        } else {
            void *v = objcsend<void *>(objc_getClass("NSValue"), "valueWithRange:", rs[0]);
            objcsend<void>(el, "setAccessibilitySelectedTextRanges:",
                           objcsend<void *>(objc_getClass("NSArray"), "arrayWithObject:", v));
        }
        break;
    }
    return 1;
}

static void macreplay(Gui *g, Arena *a, void *win, void *view, Str file, Str what)
{
    void *resp = macresponder(win);
    Read  r    = os_read(g->os, a, join(a, g->drivecwd, file), (iz)1<<24);
    if (r.err != READ_OK) {
        axcheck(g, a, 0, concat(a, what, ": cannot read it"));
        return;
    } else if (!resp) {
        axcheck(g, a, 0, concat(a, what, ": no text input"));
        return;
    }
    i32 made = 0, skipped = 0;
    Str last = {};  // the call line before, for a repeat
    for (Cut c = cut(r.data, '\n'); c.head.len || c.tail.len; c = cut(c.tail, '\n')) {
        Str line = c.head;
        if (line.len && line[line.len-1]=='\r') {
            line = takehead(line, line.len-1);
        }
        Cut t = cut(line, ' ');  // its time, seconds.ms
        if (contains(t.head, ".") && parseint(cut(t.head, '.').head).ok) {
            line = t.tail;
        }
        i32 times = 1;
        if (startswith(line, "(the line before, ")) {
            times = (i32)parseint(cut(cuthead(line, 18), ' ').head).val - 1;
            line  = last;
        } else if (endswith(line, ")") && contains(line, " (\xc3\x97")) {
            // The ring's count of a line repeated: " (×N)" at its end
            iz i = line.len;
            while (i>0 && line[i-1]!='(') {
                i--;
            }
            times = (i32)parseint(span(line.data+i+2, line.data+line.len-1)).val;
            line  = takehead(line, i>1 ? i-2 : 0);
        }
        for (i32 n = 0; n < times; n++) {
            void *pool = objc_autoreleasePoolPush();
            void *el   = macfocusof(win, view);
            i32   got  = macreplayline(*a, macresponder(win), el, line);
            objc_autoreleasePoolPop(pool);
            made    += got > 0;
            skipped += got < 0;
            last     = got ? line : last;
        }
    }
    Buf b(a, 128);
    print(&b, what);
    print(&b, ": ");
    print(&b, (i64)made);
    print(&b, " calls made, ");
    print(&b, (i64)skipped);
    print(&b, " skipped");
    axcheck(g, a, made > 0, finish(&b));
}

// Whether el's parents lead to the window
static b32 macreaches(void *el, void *win)
{
    for (i32 i = 0; el && i < 64; i++) {
        if (el == win) {
            return 1;
        }
        el = objcsend<void *>(el, "accessibilityParent");
    }
    return 0;
}

static void macdrive(Gui *g, Str cmd)
{
    Arena a    = g->frame;
    void *pool = objc_autoreleasePoolPush();
    Cut   c    = cut(cmd, ' ');
    Str   verb = c.head;
    Cut   r    = cut(c.tail, ' ');
    void *win  = macwindow();
    void *view = win ? objcsend<void *>(win, "contentView") : 0;
    Str   what = concat(&a, "ns ", cmd);

    if (!view || !axmac) {
        axcheck(g, &a, 0, concat(&a, what, ": no window"));
    } else if (verb == "start") {
        b32   was  = axmac->on;
        void *kids = objcsend<void *>(view, "accessibilityChildren");
        uz    n    = kids ? objcsend<uz>(kids, "count") : 99;
        Buf b(&a, 128);
        print(&b, what);
        print(&b, was ? Str(": on already, ") : Str(": started, "));
        print(&b, (i64)n);
        print(&b, " children");
        axcheck(g, &a, axmac->on && (was || n==0), finish(&b));
        macax.startwason = was;
    } else if (verb == "started") {
        // AppKit told of the tree a start made, unless it was on already
        Buf b(&a, 128);
        print(&b, what);
        print(&b, macax.startwason ? Str(": on already, nothing to tell") : Str(": layout told "));
        if (!macax.startwason) {
            print(&b, (i64)macax.startposts);
        }
        axcheck(g, &a, macax.startwason || macax.startposts>0, finish(&b));
    } else if (verb == "mark") {
        for (i32 k = 0; k < MN_N; k++) {
            macax.posted[k] = 0;
        }
        macax.startposts = 0;
        macax.seltype    = -1;
        guilog(g, concat(&a, "drive: ", what));
    } else if (verb == "posted") {
        i32 got = -1;
        for (i32 k = 0; k < MN_N; k++) {
            got = r.head==Str{(u8 *)macnotes[k], (iz)__builtin_strlen(macnotes[k])}
                  ? macax.posted[k] : got;
        }
        Int n = parseint(r.tail);
        Buf b(&a, 128);
        print(&b, what);
        print(&b, ": ");
        print(&b, (i64)got);
        axcheck(g, &a, got>=0 && (r.tail.len ? n.ok && got==n.val : got>0), finish(&b));
    } else if (verb == "nofocus") {
        void *f  = macfocusof(win, view);
        b32   ok = !f || !maciskind(f, "ProsperoAxElement");
        Buf b(&a, 128);
        print(&b, what);
        if (!ok) {
            print(&b, ": ");
            print(&b, macstrof(f, "accessibilityRole"));
            putbyte(&b, ' ');
            print(&b, macstrof(f, "accessibilityLabel"));
        }
        axcheck(g, &a, ok, finish(&b));
    } else if (verb == "alive") {
        i32 want = macax.len + (macax.kept ? 1 : 0);
        Buf b(&a, 128);
        print(&b, what);
        print(&b, ": ");
        print(&b, (i64)macax.alive);
        print(&b, " elements, the table's ");
        print(&b, (i64)macax.len);
        axcheck(g, &a, macax.alive==want, finish(&b));
    } else if (verb == "keep") {
        void *el = macfindel(a, win, r.head, r.tail);
        if (el && !macax.kept) {
            macax.kept = objcsend<void *>(el, "retain");
        }
        axcheck(g, &a, el!=0, concat(&a, what, el ? Str("") : Str(": not found")));
    } else if (verb == "kept") {
        // Everything a tool may ask, of an element whose node is gone,
        // ranges in and out of reach: no answer that AppKit would raise
        // on, no nil where it wants an object
        void   *el = macax.kept;
        NsRange r1 = {0, 5};
        NsRange nf = {NSNOTFOUND, 0};
        b32 ok = el && *macdeadp(el)==MACDEAD_GONE;
        ok = ok && !objcsend<ObjcBool>(el, "isAccessibilityElement");
        ok = ok && macstrof(el, "accessibilityRole")=="AXUnknown";
        void *kids = el ? objcsend<void *>(el, "accessibilityChildren") : 0;
        ok = ok && kids && objcsend<uz>(kids, "count")==0;
        ok = ok && !objcsend<void *>(el, "accessibilityParent");
        ok = ok && !objcsend<ObjcBool>(el, "accessibilityPerformPress");
        ok = ok && !objcsend<ObjcBool>(el, "accessibilityPerformShowMenu");
        ok = ok && !objcsend<ObjcBool>(el, "isAccessibilitySelectorAllowed:",
                                       sel_registerName("accessibilityPerformPress"));
        NsRect f = el ? objcrect(el, "accessibilityFrame") : NsRect{};
        ok = ok && f.w==0 && f.h==0;
        ok = ok && objcsend<iz>(el, "accessibilityNumberOfCharacters")==0;
        ok = ok && !objcsend<void *>(el, "accessibilityStringForRange:", r1);
        ok = ok && !objcsend<void *>(el, "accessibilityAttributedStringForRange:", nf);
        NsRange s = el ? objcsend<NsRange>(el, "accessibilityRangeForLine:", (iz)-1) : NsRange{};
        ok = ok && s.loc==0 && s.len==0;
        NsRect fr = el ? objcrect(el, "accessibilityFrameForRange:", nf) : NsRect{};
        ok = ok && fr.w==0 && fr.h==0;
        if (el) {
            objcsend<void>(el, "accessibilityHitTest:", NsPoint{});
            objcsend<void>(el, "setAccessibilityFocused:", (ObjcBool)1);
            objcsend<void>(el, "setAccessibilityValue:", nsstring("x"));
            objcsend<void>(el, "setAccessibilitySelectedTextRange:", nf);
            objcsend<void>(el, "release");
        }
        macax.kept = 0;
        axcheck(g, &a, ok, what);
    } else if (verb == "resizing") {
        macax.resizing = c.tail == "1";
        if (macax.resizing) {
            macax.modalposts = macax.modalheld = 0;
            macax.modal0 = macax.modal1 = 0;
        }
        guilog(g, concat(&a, "drive: ", what));
    } else if (verb == "throttled") {
        i64 most = 1 + (macax.modal1 - macax.modal0)/MACLAYOUT_GAP;
        Buf b(&a, 128);
        print(&b, what);
        print(&b, ": told ");
        print(&b, (i64)macax.modalposts);
        print(&b, ", held ");
        print(&b, (i64)macax.modalheld);
        print(&b, " over ");
        print(&b, (macax.modal1 - macax.modal0)/1000);
        print(&b, " ms");
        axcheck(g, &a, macax.modalheld>0 && macax.modalposts<=most, finish(&b));
    } else if (verb == "owed") {
        Buf b(&a, 64);
        print(&b, what);
        print(&b, ": ");
        print(&b, (i64)macax.layoutowed);
        axcheck(g, &a, macax.layoutowed==(b32)parseint(c.tail).val, finish(&b));
    } else if (verb == "seltype") {
        i32 want = (i32)parseint(c.tail).val;
        Buf b(&a, 128);
        print(&b, what);
        print(&b, ": ");
        print(&b, (i64)macax.seltype);
        axcheck(g, &a, macax.seltype==want, finish(&b));
    } else if (verb=="tget" || verb=="tset" || verb=="tparam" || verb=="tround" ||
               verb=="tsettable") {
        mactextdrive(g, &a, macfocusof(win, view), verb, c.tail, what);
    } else if (verb == "ti") {
        for (Str rest = c.tail; rest.len;) {
            iz i = 0;
            while (i+3<=rest.len && !(rest[i]==' ' && rest[i+1]==';' && rest[i+2]==' ')) {
                i++;
            }
            b32 last = i+3 > rest.len;
            mactidrive(g, &a, win, view, last ? rest : takehead(rest, i), what);
            rest = last ? Str{} : cuthead(rest, i+3);
        }
    } else if (verb == "replay") {
        macreplay(g, &a, win, view, c.tail, what);
    } else {
        // VERB [ARGS] AXROLE NAME
        Cut   av    = cut(c.tail, ' ');  // attr's ATTR, then VALUE
        Cut   vv    = cut(av.tail, ' ');
        Str   rest  = verb=="can"  ? r.tail
                    : verb=="attr" ? vv.tail
                    :                c.tail;
        Cut   rn    = cut(rest, ' ');
        void *el    = macfindel(a, win, rn.head, rn.tail);
        if (!el) {
            axcheck(g, &a, 0, concat(&a, what, ": not found"));
        } else if (verb == "has") {
            axcheck(g, &a, macreaches(el, win),
                    concat(&a, what, macreaches(el, win) ? Str("") : Str(": not under the window")));
        } else if (verb == "focus") {
            b32   key = objcsend<ObjcBool>(win, "isKeyWindow");
            void *f   = macfocusof(win, view);
            Str   by  = key ? Str(": the window's is ") : Str(": its first responder's is ");
            axcheck(g, &a, f==el, f==el ? what : concat(&a, what, concat(&a, by,
                    concat(&a, macstrof(f, "accessibilityRole"),
                           concat(&a, " ", macstrof(f, "accessibilityLabel"))))));
        } else if (verb == "press") {
            b32 ok = objcsend<ObjcBool>(el, "accessibilityPerformPress");
            axcheck(g, &a, ok, concat(&a, what, ok ? Str("") : Str(": refused")));
        } else if (verb == "can") {
            // NSAccessibilityElement has no list of actions in process:
            // AppKit makes one for a tool from the selectors allowed
            b32 ok = 1;
            for (Cut w = cut(r.head, ','); w.head.len; w = cut(w.tail, ',')) {
                b32 no  = w.head[0] == '!';
                Str act = no ? cuthead(w.head, 1) : w.head;
                char const *sel = act=="AXPress"    ? "accessibilityPerformPress"
                                : act=="AXShowMenu" ? "accessibilityPerformShowMenu"
                                :                     "";
                b32 has = *sel && objcsend<ObjcBool>(el, "respondsToSelector:",
                                                     sel_registerName(sel)) &&
                          objcsend<ObjcBool>(el, "isAccessibilitySelectorAllowed:",
                                             sel_registerName(sel));
                ok &= has != no;
            }
            axcheck(g, &a, ok, what);
        } else if (verb == "attr") {
            // By the getter AppKit answers a tool's attribute with
            static struct { Str attr; char const *sel; b32 flag; } const getters[] = {
                {"AXRole",        "accessibilityRole",      0},
                {"AXSubrole",     "accessibilitySubrole",   0},
                {"AXDescription", "accessibilityLabel",     0},
                {"AXTitle",       "accessibilityTitle",     0},
                {"AXHelp",        "accessibilityHelp",      0},
                {"AXValue",       "accessibilityValue",     0},
                {"AXEnabled",     "isAccessibilityEnabled", 1},
                {"AXFocused",     "isAccessibilityFocused", 1},
                {"AXSelected",    "isAccessibilitySelected", 1},
                {"AXExpanded",    "isAccessibilityExpanded", 1},
            };
            Buf b(&a, 256);
            for (i32 k = 0; k < countof(getters); k++) {
                if (getters[k].attr != av.head) {
                    continue;
                } else if (getters[k].flag) {
                    print(&b, objcsend<ObjcBool>(el, getters[k].sel) ? Str("1") : Str("0"));
                } else {
                    macdesc(&b, objcsend<void *>(el, getters[k].sel), 200);
                }
            }
            Str got = finish(&b);
            axcheck(g, &a, got==vv.head,
                    got==vv.head ? what : concat(&a, what, concat(&a, ", not ", got)));
        } else if (verb == "hit") {
            NsRect  f = objcrect(el, "accessibilityFrame");
            NsPoint p = {f.x + f.w/2, f.y + f.h/2};
            void   *h = objcsend<void *>(win, "accessibilityHitTest:", p);
            Buf b(&a, 256);
            print(&b, what);
            if (h != el) {
                print(&b, ": found ");
                print(&b, macstrof(h, "accessibilityRole"));
                putbyte(&b, ' ');
                print(&b, macstrof(h, "accessibilityLabel"));
            }
            axcheck(g, &a, h==el, finish(&b));
        } else {
            axcheck(g, &a, 0, concat(&a, what, ": no such command"));
        }
    }
    objc_autoreleasePoolPop(pool);
}

// The element class and the views' methods, once
static void macaxtree(Gui *g)
{
    void *up  = objc_getClass("NSAccessibilityElement");
    void *cls = up ? objc_allocateClassPair(up, "ProsperoAxElement", 0) : 0;
    if (!cls) {
        return;
    }
    void *am = class_getInstanceMethod(up, sel_registerName("isAccessibilitySelectorAllowed:"));
    macallowedup = am ? method_getImplementation(am) : 0;
    class_addIvar(cls, "axkey", sizeof(u32), 2, "I");
    class_addIvar(cls, "axdead", sizeof(u32), 2, "I");
    static struct { char const *sel; void *imp; char const *types; } const ms[] = {
        {"accessibilityRole",            (void *)macelrole,       "@@:"},
        {"accessibilitySubrole",         (void *)macelsubrole,    "@@:"},
        {"accessibilityLabel",           (void *)macellabel,      "@@:"},
        {"accessibilityTitle",           (void *)maceltitle,      "@@:"},
        {"accessibilityHelp",            (void *)macelhelp,       "@@:"},
        {"accessibilityValue",           (void *)macelvalue,      "@@:"},
        {"isAccessibilityEnabled",       (void *)macelenabled,    "B@:"},
        {"isAccessibilityFocused",       (void *)macelfocusedq,   "B@:"},
        {"isAccessibilitySelected",      (void *)macelselected,   "B@:"},
        {"isAccessibilityExpanded",      (void *)macelexpanded,   "B@:"},
        {"isAccessibilityDisclosed",     (void *)macelexpanded,   "B@:"},
        {"isAccessibilityElement",       (void *)macelisel,       "B@:"},
        {"accessibilityParent",          (void *)macelparent,     "@@:"},
        {"accessibilityChildren",        (void *)macelkids,       "@@:"},
        {"accessibilityWindow",          (void *)macelwindow,     "@@:"},
        {"accessibilityTopLevelUIElement", (void *)macelwindow,   "@@:"},
        {"accessibilityFrame",           (void *)macelframe,      "{CGRect={CGPoint=dd}{CGSize=dd}}@:"},
        {"accessibilityHitTest:",        (void *)macelhit,        "@@:{CGPoint=dd}"},
        {"accessibilityPerformPress",    (void *)macelpress,      "B@:"},
        {"accessibilityPerformShowMenu", (void *)macelmenu,       "B@:"},
        {"setAccessibilityFocused:",     (void *)macelsetfocused, "v@:B"},
        {"isAccessibilitySelectorAllowed:", (void *)macelallowed, "B@::"},
        {"accessibilityNumberOfCharacters", (void *)macelnchars, "q@:"},
        {"accessibilitySelectedText",    (void *)macelseltext,    "@@:"},
        {"accessibilitySelectedTextRange", (void *)macelselrange, "{_NSRange=QQ}@:"},
        {"accessibilitySelectedTextRanges", (void *)macelselranges, "@@:"},
        {"accessibilityVisibleCharacterRange", (void *)macelvisible, "{_NSRange=QQ}@:"},
        {"accessibilityInsertionPointLineNumber", (void *)macelcaretline, "q@:"},
        {"accessibilityLineForIndex:",   (void *)macellineof,     "q@:q"},
        {"accessibilityRangeForLine:",   (void *)macelrangeofline, "{_NSRange=QQ}@:q"},
        {"accessibilityStringForRange:", (void *)macelstring,     "@@:{_NSRange=QQ}"},
        {"accessibilityAttributedStringForRange:", (void *)macelattrstring, "@@:{_NSRange=QQ}"},
        {"accessibilityRangeForPosition:", (void *)macelrangeat,  "{_NSRange=QQ}@:{CGPoint=dd}"},
        {"accessibilityRangeForIndex:",  (void *)macelrangeofindex, "{_NSRange=QQ}@:q"},
        {"accessibilityFrameForRange:",  (void *)macelframeof,
            "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}"},
        {"accessibilityStyleRangeForIndex:", (void *)macelstyle,  "{_NSRange=QQ}@:q"},
        {"setAccessibilityValue:",       (void *)macelsetvalue,   "v@:@"},
        {"setAccessibilitySelectedText:", (void *)macelsetseltext, "v@:@"},
        {"setAccessibilitySelectedTextRange:", (void *)macelsetselrange, "v@:{_NSRange=QQ}"},
        {"setAccessibilitySelectedTextRanges:", (void *)macelsetselranges, "v@:@"},
        {"dealloc",                      (void *)macedealloc,     "v@:"},
    };
    for (i32 i = 0; i < countof(ms); i++) {
        void       *sel   = sel_registerName(ms[i].sel);
        void       *m     = class_getInstanceMethod(up, sel);
        char const *types = m ? method_getTypeEncoding(m) : ms[i].types;
        class_addMethod(cls, sel, ms[i].imp, types);
    }
    objc_registerClassPair(cls);
    macax.cls     = cls;
    macax.keyoff  = ivar_getOffset(class_getInstanceVariable(cls, "axkey"));
    macax.deadoff = ivar_getOffset(class_getInstanceVariable(cls, "axdead"));
    macax.mem     = newarena(os_reserve(g->os, (iz)1 << 28));

    // The root's, on the views; before the log's wrappers, which then
    // leave these selectors to them
    static struct { char const *cls, *sel; void *imp; } const vs[] = {
        {"SDL3View",                "accessibilityChildren",         (void *)macviewkids},
        {"SDL3View",                "accessibilityHitTest:",         (void *)macviewhit},
        {"SDL3View",                "accessibilityFocusedUIElement", (void *)macviewfocus},
        {"SDL3TranslatorResponder", "accessibilityFocusedUIElement", (void *)macviewfocus},
    };
    macviewcls = objc_getClass("SDL3View");
    macrespcls = objc_getClass("SDL3TranslatorResponder");
    for (i32 i = 0; i < countof(vs); i++) {
        void *c   = objc_getClass(vs[i].cls);
        void *sel = sel_registerName(vs[i].sel);
        void *m   = c ? class_getInstanceMethod(c, sel) : 0;
        if (m && !macownmethod(c, vs[i].sel)) {
            class_addMethod(c, sel, vs[i].imp, method_getTypeEncoding(m));
        }
    }
    g->ax->published = macpublished;
    g->ax->osdrive   = macdrive;
}

// The checklist's "off": nothing of the above was installed
static void macoffcheck(Gui *g, Arena *a)
{
    // Asked as a tool would, AppKit answers, and nothing starts
    void *pool = objc_autoreleasePoolPush();
    void *cv   = 0;
    if (GuiSdl *p = (GuiSdl *)g->os->gui; p && p->window) {
        void *win = SDL_GetPointerProperty(SDL_GetWindowProperties(p->window),
                                           SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, 0);
        cv = win ? objcsend<void *>(win, "contentView") : 0;
    }
    if (cv) {
        objcsend<void *>(cv, "accessibilityChildren");
    }
    objc_autoreleasePoolPop(pool);
    void *view = objc_getClass("SDL3View");
    void *resp = objc_getClass("SDL3TranslatorResponder");
    void *kids = view ? (void *)class_getMethodImplementation(view,
                        sel_registerName("accessibilityChildren")) : 0;
    void *ins  = resp ? (void *)class_getMethodImplementation(resp,
                        sel_registerName("insertText:replacementRange:")) : 0;
    b32 ok = !objc_getClass("ProsperoAxElement") && !nmacaximps && !macinsertimp &&
             kids!=(void *)macviewkids && ins!=(void *)macinsert && !axmac;
    axcheck(g, a, ok, "ns off: nothing installed");
}

// Install the tree and the log's wrappers, once: with accessibility
// off, none.
static void axmacinit(Gui *g)
{
    axoffcheck = macoffcheck;
    if (!g->ax || axmac) {
        return;
    }
    axmac = g->ax;
    macaxtree(g);

    void *t = objc_getClass("SDL3TranslatorResponder");
    if (t) {
        macwrap(t, "insertText:replacementRange:", macinsert, &macinsertimp);
        macwrap(t, "setMarkedText:selectedRange:replacementRange:", macsetmarked,
                &macsetmarkedimp);
        macwrap(t, "unmarkText", macunmark, &macunmarkimp);
        macwrap(t, "selectedRange", macselected, &macselectedimp);
        macwrap(t, "markedRange", macmarked, &macmarkedimp);
        macwrap(t, "hasMarkedText", machasmarked, &machasmarkedimp);
        macwrap(t, "attributedSubstringForProposedRange:actualRange:", macattrsub,
                &macattrsubimp);
        macwrap(t, "firstRectForCharacterRange:actualRange:", macfirstrect,
                &macfirstrectimp);
        macwrap(t, "characterIndexForPoint:", macindexat, &macindexatimp);
        macwrap(t, "doCommandBySelector:", maccommand, &maccommandimp);
        macwrap(t, "validAttributesForMarkedText", macvalidattrs, &macvalidattrsimp);
    }

    for (i32 c = 0; c < countof(macaxclasses); c++) {
        void *cls = objc_getClass(macaxclasses[c]);
        void *up  = cls ? class_getSuperclass(cls) : 0;
        for (i32 i = 0; up && i < countof(macaxsels); i++) {
            void *sel = sel_registerName(macaxsels[i].sel);
            void *m   = class_getInstanceMethod(up, sel);
            if (!m || macownmethod(cls, macaxsels[i].sel)) {
                continue;  // AppKit lacks it, or SDL took it: leave it be
            }
            MacAxImp *e = macaximps + nmacaximps;
            e->cls = cls;
            e->sel = sel;
            if (class_addMethod(cls, sel, macaxwrappers[macaxsels[i].sig],
                                method_getTypeEncoding(m))) {
                nmacaximps++;
            }
        }
    }
}
