// Accessibility: macOS's side
//
// For now a trace, which changes nothing: SDL's text input view
// (SDL3TranslatorResponder, its NSTextInputClient) is what Dictation and
// the input methods talk to, and NSAccessibility is what Voice Control
// asks, of the window, its content view (SDL3View) and that text input.
// Each call they make is logged (axnote, a11y_imgui.cpp) with its
// arguments and what SDL or AppKit answered, then answered as before,
// so the adapter to come is built on what they really do. That is only
// while the tree is built (a tool asked, `[ui] accessibility = on`, or
// PROSPERO_AXTRACE) and something can read the log (a Debug build's
// Metrics pane, or the trace). Text is logged only from a field the
// model knows is not a password, and a key's text, which reaches SDL
// through these same calls, only as its length: the trace is of speech
// tools, not typing.
//
// SDL's NSTextInputClient methods are its own, so method_setImplementation
// wraps them, and the original is called. NSAccessibility's are AppKit's,
// inherited: changing those would change every view, so the three classes
// get methods of their own (class_addMethod) that log and call the
// superclass's. Both are per class: SDL makes a new text input view at
// each Stop/StartTextInput, which the wrappers serve all the same.
//
// It all runs on the main thread, between frames or inside them (an
// IME's queries as text input starts), and never touches ImGui. An
// Objective-C exception can't unwind through C++, so nothing here sends
// a message that could throw: strings are read, not ranged into.
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


// SDL's NSTextInputClient, wrapped

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

// A key's text, by its length alone. Nor is the text model logged
// ahead of it: the next call that isn't a key's logs it.
static void mactyped(Buf *b, void *o)
{
    b32   attr = maciskind(o, "NSAttributedString");
    void *s    = attr ? objcsend<void *>(o, "string") : o;
    print(b, "(typed)#");
    print(b, maciskind(s, "NSString") ? (i64)objcsend<uz>(s, "length") : (i64)-1);
}

static void macinsert(void *self, void *sel, void *s, NsRange r)
{
    if (AxGui *x = maclog()) {
        Arena a     = x->logmem;
        b32   keyed = mackeyed();
        Buf   b(&a, 256);
        print(&b, "insertText:replacementRange: ");
        if (keyed) {
            mactyped(&b, s);
        } else {
            macstrarg(&b, s);
        }
        putbyte(&b, ' ');
        macrange(&b, r);
        if (keyed) {
            axnote(x, a, finish(&b));
        } else {
            macnote(x, a, &b);
        }
    }
    macinsertimp(self, sel, s, r);
}

static void macsetmarked(void *self, void *sel, void *s, NsRange sr, NsRange rr)
{
    if (AxGui *x = maclog()) {
        Arena a     = x->logmem;
        b32   keyed = mackeyed();
        Buf   b(&a, 256);
        print(&b, "setMarkedText:selectedRange:replacementRange: ");
        if (keyed) {
            mactyped(&b, s);
        } else {
            macstrarg(&b, s);
        }
        putbyte(&b, ' ');
        macrange(&b, sr);
        putbyte(&b, ' ');
        macrange(&b, rr);
        if (keyed) {
            axnote(x, a, finish(&b));
        } else {
            macnote(x, a, &b);
        }
    }
    macsetmarkedimp(self, sel, s, sr, rr);
}

static void macunmark(void *self, void *sel)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 64);
        print(&b, "unmarkText");
        macnote(x, a, &b);
    }
    macunmarkimp(self, sel);
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

static NsRange macselected(void *self, void *sel)
{
    NsRange r = macselectedimp(self, sel);
    macnoterange("selectedRange", r);
    return r;
}

static NsRange macmarked(void *self, void *sel)
{
    NsRange r = macmarkedimp(self, sel);
    macnoterange("markedRange", r);
    return r;
}

static ObjcBool machasmarked(void *self, void *sel)
{
    ObjcBool r = machasmarkedimp(self, sel);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 64);
        print(&b, r ? Str("hasMarkedText -> YES") : Str("hasMarkedText -> NO"));
        macnote(x, a, &b);
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

static void *macattrsub(void *self, void *sel, NsRange r, NsRange *actual)
{
    NsRange was = actual ? *actual : NsRange{};
    void   *s   = macattrsubimp(self, sel, r, actual);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 256);
        print(&b, "attributedSubstringForProposedRange:actualRange: ");
        macrange(&b, r);
        print(&b, " -> ");
        macstrarg(&b, s);
        macactual(&b, actual, was);
        macnote(x, a, &b);
    }
    return s;
}

static NsRect macfirstrect(void *self, void *sel, NsRange r, NsRange *actual)
{
    NsRange was = actual ? *actual : NsRange{};
    NsRect  rc  = macfirstrectimp(self, sel, r, actual);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 128);
        print(&b, "firstRectForCharacterRange:actualRange: ");
        macrange(&b, r);
        print(&b, " -> ");
        macrect(&b, rc);
        macactual(&b, actual, was);
        macnote(x, a, &b);
    }
    return rc;
}

static uz macindexat(void *self, void *sel, NsPoint p)
{
    uz i = macindexatimp(self, sel, p);
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
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
        macnote(x, a, &b);
    }
    return i;
}

static void maccommand(void *self, void *sel, void *cmd)
{
    if (AxGui *x = maclog()) {
        Arena a = x->logmem;
        Buf   b(&a, 64);
        print(&b, "doCommandBySelector: ");
        char const *name = cmd ? sel_getName(cmd) : "nil";
        print(&b, Str{(u8 *)name, (iz)__builtin_strlen(name)});
        macnote(x, a, &b);
    }
    maccommandimp(self, sel, cmd);
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


// NSAccessibility on the window, its content view and the text input,
// through the protocol's older, attribute-keyed methods, which AppKit
// answers a remote query with. The classes' own methods call their
// superclass's, looked up at each call, not at install: AppKit's may be
// replaced or added to later, as when an assistive tool's bundle loads.

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

// Install the wrappers, once: with accessibility off, none.
static void axmacinit(Gui *g)
{
    if (!g->ax || axmac) {
        return;
    }
    axmac = g->ax;

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
