// Accessibility: Dear ImGui's side
//
// ImGui keeps no tree of what it shows, so this builds one each frame,
// while an assistive tool is listening, from the test engine's item
// hooks (IMGUI_ENABLE_TEST_ENGINE, imconfig.h), and the text fields'
// own reports from the patched InputTextEx (imgui_a11y.h), then
// publishes it (a11y.cpp) at the frame's end for the OS's adapter.
//
// An item's hooks come as ImGui adds it: its ID, rectangle and window,
// then, unless clipped away, its label and status flags. Its role comes
// from those flags and where it is (a menu, a tab bar), and its name
// from its label (axlabel). After a widget, axname and axrole set what
// its label and flags cannot say. Items that never gave a label and
// show, ImGui's own (a window's title bar, its scrollbars), are left
// out, as are those clipped away for now.
//
// Off ([ui] accessibility = off), or until an assistive tool first asks
// (auto), ImGui's hooks stay off, and each costs a branch.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    AXSET_AUTO,
    AXSET_ON,
    AXSET_OFF,
};

static i32 axsetparse(Str s)
{
    return s=="auto" ? AXSET_AUTO : s=="on" ? AXSET_ON : s=="off" ? AXSET_OFF : -1;
}

// An item as its hooks told of it this frame
struct AxItem {
    u32    id;
    u32    window;     // the ImGuiWindow's ID it went in
    ImRect rect;
    b32    offscreen;
    b32    info;       // ImGui gave its label
    i32    status;     // ImGuiItemStatusFlags, with the widget's own
    i32    itemflags;
    AxRole role;
    u32    state;
    u32    actions;
    Str    label;
    Str    name;       // from axname
    b32    named;
    Str    help;       // its tooltip's text
    u32    key;        // from axkey, else its ID
    Str    value;      // a combo's, as it shows it
};

// A text field as drawn this frame, copied from its report
struct AxFieldDraw {
    u32  id;
    i32  flags;
    b32  active;
    b32  password;
    Str  text;
    Str  hint;
    i32  anchor, caret;
    AxGeom geom;
};

// An action an assistive tool asked for, until ImGui activates its item
enum : i32 {
    AXACT_PRESS,  // as a click, or the keyboard's Space
    AXACT_FOCUS,  // a text field, to edit: its caret kept
    AXACT_MENU,   // its context menu, as a right click (uirclicked)
};

struct AxAct {
    u32 key;
    u32 id;
    i32 kind;
    i64 when;
    i32 tries;  // deliveries so far
    b32 seen;   // its item was submitted since the last delivery
    b32 done;
};

struct AxGui {
    Gui                *g;
    A11y               *ax;
    b32                 on;
    Arena               mem;      // this frame's items and fields
    byte               *base;
    Slice<AxItem>       items;
    Slice<AxFieldDraw>  fields;
    iz                  last;     // the item added last, or -1
    Slice<AxEvent>      events;   // the last publish's
    i64                 publishes;
    AxAct               acts[16];
    i32                 nacts;
    u64                *linted;   // --a11y-lint: what was logged, hashed
    i32                 nlinted;
    Str                 nextname; // axnextname's, for the next item in nextwin
    u32                 nextwin;
    u32                 menuid;   // an item whose context menu was asked for
    i64                 menuwhen;
    u32                 tabid;    // a tab whose info came before its add
    char const         *tablabel;
    u32                 navid;    // the checklist's "nav": the keyboard's item
    u32                 navwin;
    AxLog              *log;      // the adapter's calls (axnote)
    Arena               logmem;   // scratch for them, between frames too
    i64                 logt0;    // when the log began
    u64                 logmodel; // the text model as last logged, hashed
    iz                  tracefd;  // PROSPERO_AXTRACE's file, or -1
    i64                 logmark;  // the checklist's: where "logged" looks from
    Str                 tracepath;
    void              (*published)(AxGui *);  // the OS adapter's, after each publish
    void              (*osdrive)(Gui *, Str); // its checklist verbs ("ns ...")
};

enum { AXLINT_CAP = 1<<12 };

static Str axcstr(char const *);

// --a11y-lint: log a visible control with no name, once, as where it is
// and its ImGui label (tests/gui.py checks these against
// tests/a11y_allow.txt).
static void axlintnote(AxGui *x, AxRole role, ImGuiWindow *w, Str label)
{
    Gui  *g = x->g;
    Arena a = g->frame;
    Buf   b(&a, 256);
    print(&b, "a11y-lint: unnamed ");
    print(&b, axrolenames[role]);
    print(&b, " in ");
    // The window's name without the ID suffixes ImGui gives child
    // windows (Parent/##child_1A2B3C4D), which differ run to run
    Str name = w ? axcstr(w->Name) : Str("?");
    for (iz i = 0; i < name.len; i++) {
        b32 hex = i+9<=name.len && name[i]=='_' && (i+9==name.len || name[i+9]=='/');
        for (iz k = 1; hex && k < 9; k++) {
            u8 c = name[i+k];
            hex = (c>='0' && c<='9') || (c>='A' && c<='F') || (c>='a' && c<='f');
        }
        if (hex) {
            i += 8;
        } else {
            putbyte(&b, name[i]);
        }
    }
    print(&b, ": ");
    print(&b, label.len ? label : Str("(no label)"));
    Str note = finish(&b);
    u64 h    = hash(note) | 1;
    if (!x->linted) {
        x->linted = alloc<u64>(&g->perm, AXLINT_CAP);
    }
    for (u32 i = (u32)h;; i++) {
        u64 *slot = x->linted + (i & (AXLINT_CAP-1));
        if (*slot == h) {
            return;
        } else if (!*slot) {
            if (x->nlinted == AXLINT_CAP/2) {
                return;  // enough to go on
            }
            *slot = h;
            x->nlinted++;
            guilog(g, note);
            return;
        }
    }
}

static AxGui *axgui(ImGuiContext *ctx)
{
    Gui *g = ctx ? (Gui *)ctx->IO.UserData : 0;
    return g && g->ax && g->ax->on ? g->ax : 0;
}

static Str axcstr(char const *s)
{
    Str r = {};
    r.cdata = s;
    r.len   = s ? (iz)__builtin_strlen(s) : 0;
    return r;
}


// The test engine's hooks, which ImGui calls only while its context has
// TestEngineHookItems set.

void ImGuiTestEngineHook_ItemAdd(ImGuiContext *ctx, ImGuiID id, ImRect const &bb,
                                 ImGuiLastItemData const *item)
{
    AxGui *x = axgui(ctx);
    if (!x || !item || !ctx->CurrentWindow) {
        return;  // a window's own report (Begin), or off
    }
    ImGuiWindow *w = ctx->CurrentWindow;
    AxItem it = {};
    it.id        = id;
    it.window    = w->ID;
    it.rect      = item->Rect;
    it.offscreen = !bb.Overlaps(w->ClipRect);
    it.itemflags = item->ItemFlags;
    if (x->nextname.len) {
        it.named     = w->ID == x->nextwin;  // else its window skipped it
        it.name      = it.named ? x->nextname : Str{};
        x->nextname  = {};
    }
    x->items = push(&x->mem, x->items, it);
    x->last  = x->items.len - 1;
    if (x->tabid == id) {
        x->tabid = 0;
        ImGuiTestEngineHook_ItemInfo(ctx, id, x->tablabel, 0);
    }
    if (x->nacts && x->acts[0].id==id) {
        x->acts[0].seen = 1;
        if (ctx->NavActivateId == id) {
            x->acts[0].done = 1;  // it landed
        }
    }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID id, char const *label,
                                  ImGuiItemStatusFlags flags)
{
    AxGui *x = axgui(ctx);
    if (!x) {
        return;
    }
    // Its info follows its add, but for a few widgets made of others
    iz i = x->items.len - 1;
    for (iz stop = i-8; i>=0 && i>stop && x->items[i].id!=id; i--) {}
    if (i<0 || x->items[i].id!=id) {
        ImGuiTabBar *bar = ctx->CurrentTabBar;
        if (bar && ImGui::TabBarFindTabByID(bar, id)) {
            // A tab gives its info first (TabItemEx): kept for its add
            x->tabid    = id;
            x->tablabel = cstr0(&x->mem, axcstr(label));
        }
        return;
    }
    AxItem      *it = &x->items[i];
    ImGuiWindow *w  = ctx->CurrentWindow;
    it->info   = 1;
    it->status = flags;
    it->label  = clone(&x->mem, axcstr(label));

    b32 inmenu = (w->Flags & ImGuiWindowFlags_ChildMenu) ||
                 w->DC.NavLayerCurrent==ImGuiNavLayer_Menu ||
                 ((w->Flags & ImGuiWindowFlags_Popup) && (flags & ImGuiItemStatusFlags_Checkable));
    ImGuiTabBar *bar = ctx->CurrentTabBar;
    if (bar && ImGui::TabBarFindTabByID(bar, id)) {
        it->role    = AX_TAB;
        it->actions = AXA_PRESS;
        it->state  |= bar->SelectedTabId==id ? AXS_SELECTED : 0;
    } else if (inmenu) {
        it->role    = AX_MENUITEM;
        it->actions = AXA_PRESS;
        if (flags & ImGuiItemStatusFlags_Openable) {
            it->actions |= AXA_EXPAND;
            it->state   |= flags & ImGuiItemStatusFlags_Opened ? AXS_EXPANDED : 0;
        }
        it->state |= flags & ImGuiItemStatusFlags_Checked ? AXS_CHECKED : 0;
    } else if (flags & ImGuiItemStatusFlags_Inputable) {
        it->role    = AX_TEXTFIELD;  // or a slider, once its field is known
        it->actions = AXA_EDIT | AXA_FOCUS;
    } else if (flags & ImGuiItemStatusFlags_Checkable) {
        it->role    = AX_CHECKBOX;
        it->actions = AXA_TOGGLE;
        it->state  |= flags & ImGuiItemStatusFlags_Checked ? AXS_CHECKED : 0;
    } else if (flags & ImGuiItemStatusFlags_Openable) {
        it->role    = AX_TREEITEM;
        it->actions = AXA_EXPAND;
        it->state  |= flags & ImGuiItemStatusFlags_Opened ? AXS_EXPANDED : 0;
    } else {
        it->role    = AX_BUTTON;
        it->actions = AXA_PRESS;
    }
    if (bar && (it->label=="##<" || it->label=="##>")) {
        // ImGui's own arrows on a tab bar too full to show (a dock's)
        it->role    = AX_BUTTON;
        it->actions = AXA_PRESS;
        it->name    = it->label=="##<" ? Str("Scroll tabs left") : Str("Scroll tabs right");
        it->named   = 1;
    }
    it->state |= it->itemflags & ImGuiItemFlags_Disabled ? AXS_DISABLED : 0;
}

// A combo box, as the patched BeginCombo added it (FetchGui.cmake),
// which gives ImGui no info: its value is what it shows, and a press
// opens it.
extern "C" void prospero_axcombo(unsigned id, char const *label, char const *preview, int open)
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    AxGui        *x   = axgui(ctx);
    if (!x) {
        return;
    }
    ImGuiTestEngineHook_ItemInfo(ctx, id, label, ctx->LastItemData.StatusFlags);
    iz i = x->items.len - 1;
    if (i<0 || x->items[i].id!=id) {
        return;
    }
    AxItem *it = &x->items[i];
    it->role    = AX_COMBO;
    it->actions = AXA_PRESS|AXA_EXPAND;
    it->state   = open ? it->state|AXS_EXPANDED : it->state&~AXS_EXPANDED;
    it->value   = clone(&x->mem, axcstr(preview));
}

void ImGuiTestEngineHook_Log(ImGuiContext *, char const *, ...)
{
}

char const *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID)
{
    return 0;
}


// The patched InputTextEx's halves (imgui_a11y.h).

extern "C" int prospero_axedit(unsigned id, char const *text, int len, ProsperoAxEdit *out)
{
    AxGui *x = axgui(ImGui::GetCurrentContext());
    if (!x) {
        return 0;
    }
    Str    now = {(u8 *)text, len};
    AxEdit e;
    if (!axnextedit(x->ax, id, now, x->g->sched.now, &e)) {
        return 0;
    }
    out->lo     = e.lo;
    out->hi     = e.hi;
    out->text   = e.text.cdata;
    out->len    = (int)e.text.len;
    out->anchor = e.anchor;
    out->caret  = e.caret;
    return 1;
}

enum { AXVALUE_MAX = 1<<16 };

extern "C" void prospero_axfield(ProsperoAxField const *f)
{
    AxGui *x = axgui(ImGui::GetCurrentContext());
    if (!x) {
        return;
    }
    AxFieldDraw d = {};
    // Only the active field's text is needed whole, for editing; the
    // others are their values, of which a start says enough.
    Str text = {(u8 *)f->text, f->text && !f->toolong ? f->len : 0};
    if (!f->active && text.len>AXVALUE_MAX) {
        iz n = AXVALUE_MAX;
        while (n>0 && (text[n]&0xc0)==0x80) {
            n--;
        }
        text = takehead(text, n);
    }
    d.id       = f->id;
    d.flags    = f->flags | (f->toolong ? ImGuiInputTextFlags_ReadOnly : 0);
    d.active   = f->active;
    d.password = !f->text;
    d.text     = clone(&x->mem, text);
    d.hint     = clone(&x->mem, axcstr(f->hint));
    d.anchor   = f->anchor;
    d.caret    = f->caret;
    AxGeom *g  = &d.geom;
    g->x       = f->x;
    g->y       = f->y;
    g->lineh   = f->lineh;
    g->caretline = -1;
    g->clip    = {f->clip[0], f->clip[1], f->clip[2], f->clip[3]};
    if (f->active) {
        g->nlines = f->nlines;
        g->lines  = alloc<i32>(&x->mem, f->nlines);
        for (i32 i = 0; i < f->nlines; i++) {
            g->lines[i] = f->lines[i];
        }
        g->vis0 = f->vis0;
        g->vis1 = f->vis1;
        g->caretline = f->caretline;
        g->nxs  = f->nxs;
        g->xs   = alloc<f32>(&x->mem, f->nxs);
        for (i32 i = 0; i < f->nxs; i++) {
            g->xs[i] = f->xs[i];
        }
    }
    x->fields = push(&x->mem, x->fields, d);
}


// Names and roles that labels and flags cannot give, right after the
// item. A no-op while off.

// ImGui's last item, if it had an ID, so hooks; among the last few
// added, since a widget may add parts after itself (a multiline field's
// child window).
static AxItem *axlastitem(Gui *g)
{
    AxGui *x  = g && g->ax && g->ax->on ? g->ax : 0;
    u32    id = x ? ImGui::GetItemID() : 0;
    if (!id) {
        return 0;
    }
    for (iz i = x->items.len-1, stop = i-8; i>=0 && i>stop; i--) {
        if (x->items[i].id == id) {
            return &x->items[i];
        }
    }
    return 0;
}

static Gui *axguiof()
{
    return (Gui *)ImGui::GetIO().UserData;
}

// The last item's help: its tooltip's text, formatted only while on,
// for an item with a node.
static void axtip(Str help)
{
    Gui    *g  = axguiof();
    AxItem *it = axlastitem(g);
    if (it) {
        it->help = clone(&g->ax->mem, help);
    }
}

static void axtipv(char const *fmt, __builtin_va_list ap)
{
    Gui    *g  = axguiof();
    AxItem *it = axlastitem(g);
    if (it) {
        char const *beg, *end;
        ImFormatStringToTempBufferV(&beg, &end, fmt, ap);
        it->help = clone(&g->ax->mem, span((u8 *)beg, (u8 *)end));
    }
}

// The last item is checked now or not, as a checkbox or a radio is (its
// role, axrole): a checkbox's press toggles it.
static void axchecked(Gui *g, b32 on)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->state = on ? it->state|AXS_CHECKED : it->state&~AXS_CHECKED;
        if (it->role==AX_CHECKBOX && (it->actions & AXA_PRESS)) {
            it->actions = (it->actions & ~AXA_PRESS) | AXA_TOGGLE;
        }
    }
}

// The last item is one of a list's, selected now or not: a Selectable,
// a button to ImGui's flags, made a list's item.
static void axselected(Gui *g, b32 on)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->role  = it->role==AX_BUTTON ? AX_LISTITEM : it->role;
        it->state = on ? it->state|AXS_SELECTED : it->state&~AXS_SELECTED;
    }
}

// The next item's name, ahead of it as ImGui's SetNextItem* go: for a
// combo, whose popup, while open, comes after it as the last item.
static void axnextname(Gui *g, Str name)
{
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    if (x) {
        x->nextname = clone(&x->mem, name);
        x->nextwin  = ImGui::GetCurrentWindow()->ID;
    }
}

static void axname(Gui *g, Str name)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->name  = clone(&g->ax->mem, name);
        it->named = 1;
    }
}

// The last item's key, for one whose ImGuiID changes when what it is
// doesn't (AXKEY_*): the prompt, in each session.
static void axkey(Gui *g, u32 key)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->key = key;
    }
}

static void axrole(Gui *g, AxRole role)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->role = role;
        it->info = 1;
        if (role==AX_NONE || role==AX_TEXT || role==AX_IMAGE || role==AX_GROUP) {
            // Nothing to press, though ImGui's flags said a button: a
            // menu (axmenu) and the keyboard (axfocusable) stay
            it->actions &= ~(AXA_PRESS|AXA_TOGGLE|AXA_EXPAND);
            it->state   &= ~(AXS_CHECKED|AXS_EXPANDED);
        }
    }
}

// The last item folds and opens what is under it, open now or not, as a
// tree node does: a heading in the sidebar.
static void axexpanded(Gui *g, b32 open)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->role     = AX_TREEITEM;
        it->info     = 1;
        it->actions |= AXA_EXPAND;
        it->state    = open ? it->state|AXS_EXPANDED : it->state&~AXS_EXPANDED;
    }
}

// The last item has a context menu, which an assistive tool may ask for
// (uirclicked).
static void axmenu(Gui *g)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->actions |= AXA_MENU;
    }
}

// The last item takes the keyboard, as a tool may ask (AXACT_FOCUS),
// which comes as ImGui's activation: one that is no field, the terminal.
static void axfocusable(Gui *g)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->actions |= AXA_FOCUS;
    }
}

// Whether a tool asked for item id's context menu: once.
// Or the keyboard, its Menu key or Shift+F10 on the item as ImGui's
// (BeginPopupContextItem's), accessibility on or not.
static b32 axmenuasked(Gui *g, u32 id)
{
    ImGuiContext *ctx = GImGui;
    if (id && ctx->NavOpenContextMenuItemId==id && ctx->NavId==id) {
        return 1;
    }
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    if (!x || !id || x->menuid!=id) {
        return 0;
    }
    x->menuid = 0;
    return 1;
}


// The frame's tree

static AxFieldDraw *axfieldof(AxGui *x, u32 id)
{
    for (iz i = 0; i < x->fields.len; i++) {
        if (x->fields[i].id == id) {
            return &x->fields[i];
        }
    }
    return 0;
}

// An item's key, by its ImGuiID
static u32 axkeyof(AxGui *x, u32 id)
{
    for (iz i = x->items.len-1; i >= 0; i--) {
        if (x->items[i].id == id) {
            return x->items[i].key ? x->items[i].key : id;
        }
    }
    return id;
}

static b32 axshown(ImGuiWindow *w)
{
    return w->Active && !w->Hidden && !(w->Flags & ImGuiWindowFlags_Tooltip);
}

// The node a window's items go under: a multiline field's own child
// window is its field, so its items go under the field's window.
static u32 axwindowkey(AxGui *x, ImGuiWindow *w)
{
    while (w && (w->Flags & ImGuiWindowFlags_ChildWindow) && w->ParentWindow &&
            axfieldof(x, w->ChildId)) {
        w = w->ParentWindow;
    }
    return w ? w->ID : 0;
}

static void axbuild(AxGui *x, ImGuiContext *ctx)
{
    Gui     *g = x->g;
    AxBuild *b = axbegin(x->ax);

    for (ImGuiWindow *w : ctx->Windows) {
        if (!axshown(w) || axwindowkey(x, w)!=w->ID) {
            continue;
        }
        u32    flags  = (u32)w->Flags;
        u32    parent = 0;
        AxRole role   = AX_WINDOW;
        u32    state  = 0;
        Str    name   = axlabel(&x->mem, axcstr(w->Name));
        if (w->DockIsActive) {
            role   = AX_GROUP;  // a pane, docked in its host
            parent = w->ParentWindow ? w->ParentWindow->ID : 0;
        } else if (flags & ImGuiWindowFlags_ChildMenu) {
            role   = AX_MENU;  // a submenu, under the menu it opened from
            parent = w->ParentWindow ? w->ParentWindow->ID : 0;
        } else if (flags & ImGuiWindowFlags_ChildWindow) {
            role   = AX_GROUP;
            parent = w->ParentWindow ? w->ParentWindow->ID : 0;
            name   = {};
        } else if (flags & ImGuiWindowFlags_Modal) {
            role   = AX_DIALOG;
            state |= AXS_MODAL;
        } else if (flags & ImGuiWindowFlags_Popup) {
            role = AX_POPUP;
        } else if (startswith(axcstr(w->Name), "##MainMenuBar")) {
            role = AX_MENUBAR;
        } else if (flags & ImGuiWindowFlags_NoTitleBar) {
            role = AX_GROUP;  // a dock space's host: the OS's window is the app
        }
        AxNode *n = axadd(b, parent, w->ID, role, name);
        n->rect  = {w->Pos.x, w->Pos.y, w->Pos.x+w->Size.x, w->Pos.y+w->Size.y};
        n->state = state;
    }

    for (iz i = 0; i < x->items.len; i++) {
        AxItem *it = &x->items[i];
        if (!it->info || it->offscreen) {
            continue;  // ImGui's own, or clipped away: no label known
        }
        ImGuiWindow *w      = ImGui::FindWindowByID(it->window);
        u32          parent = axwindowkey(x, w);
        AxFieldDraw *f      = it->role==AX_TEXTFIELD ? axfieldof(x, it->id) : 0;
        AxRole       role   = it->role;
        u32          state  = it->state;
        if (it->role==AX_TEXTFIELD && !f) {
            role = AX_SLIDER;
        } else if (f) {
            role   = f->flags & ImGuiInputTextFlags_Multiline ? AX_TEXTAREA : AX_TEXTFIELD;
            state |= f->password ? AXS_PASSWORD : 0;
            state |= f->flags & ImGuiInputTextFlags_ReadOnly ? AXS_READONLY : 0;
        }
        Str     name = it->named ? it->name : axlabel(&x->mem, it->label);
        if (!name.len && f && f->hint.len) {
            name = f->hint;  // a field with no label says what it is for
        }
        AxNode *n    = axadd(b, parent, it->key ? it->key : it->id, role, name);
        if (g->axlint && role!=AX_NONE && (it->actions & AXA_NAMED) && !name.len) {
            axlintnote(x, role, w, it->label);
        }
        n->id      = it->id;
        n->rect    = {it->rect.Min.x, it->rect.Min.y, it->rect.Max.x, it->rect.Max.y};
        n->state   = state;
        n->actions = it->actions;
        if (f && !f->password) {
            axsetvalue(b, n, f->text);
        } else if (it->value.len) {
            axsetvalue(b, n, it->value);
        }
        if (it->help.len) {
            axsethelp(b, n, it->help);  // read when asked: no event
        }
    }

    // The field being edited is the focus, else the keyboard's item, else
    // its window; none while the window isn't the user's
    AxFieldDraw *active = 0;
    for (iz i = 0; i < x->fields.len; i++) {
        active = x->fields[i].active ? &x->fields[i] : active;
    }
    u32 focus = 0;
    if (!guifocused(g)) {
        focus = 0;
    } else if (active) {
        focus = active->id;
    } else if (ctx->NavId) {
        focus = ctx->NavId;
    } else if (ctx->NavWindow) {
        focus = ctx->NavWindow->ID;
    }

    focus = axkeyof(x, focus);

    if (active) {
        u32 state = active->password ? AXS_PASSWORD : 0;
        state |= active->flags & ImGuiInputTextFlags_ReadOnly ? AXS_READONLY : 0;
        // An input method's composition, drawn over it this frame
        Preedit *p  = g->ime;
        b32      pe = p && p->len && p->drawn && p->owner==active->id;
        if (pe) {
            active->geom.mxs = p->xs;
            active->geom.mys = p->ys;
            active->geom.nm  = p->len + 1;
        }
        axtextsync(x->ax, axkeyof(x, active->id), active->id, state,
                   (active->flags & ImGuiInputTextFlags_Multiline) != 0,
                   active->text, active->anchor, active->caret, &active->geom);
        axmark(x->ax, pe ? imetext(p) : Str{}, pe ? p->sel0 : 0, pe ? p->sel1 : 0);
    } else {
        axtextnone(x->ax);
    }

    x->events = axpublish(x->ax, focus);
    x->publishes++;
}

// Whether a modal dialog is open and the node is under it: neither in it
// nor in a popup or menu over it. ImGui's activation ignores modals, as a
// click on what is under one never does.
static b32 axblocked(AxGui *x, u32 key)
{
    ImGuiWindow *m = ImGui::GetTopMostPopupModal();
    if (!m) {
        return 0;
    }
    AxTree *t = x->ax->pub;
    for (i32 i = axfind(t, key); i >= 0; i = t->nodes[i].parent) {
        ImGuiWindow *w = ImGui::FindWindowByID(t->nodes[i].key);
        if (w && (w->RootWindow==m || ImGui::IsWindowAbove(w->RootWindow, m))) {
            return 0;
        }
    }
    return 1;
}

// The first pending action, as ImGui's activation of its item, unless the
// app asked for one already, for this frame. Until ItemAdd sees it land,
// it is asked again, a few times: not when its item went unsubmitted.
static void axdeliver(AxGui *x, ImGuiContext *ctx)
{
    Gui *g = x->g;
    i32  n = 0;
    for (i32 i = 0; i < x->nacts; i++) {
        AxAct *a = x->acts + i;
        b32 gone = a->tries>0 && !a->seen;
        if (!a->done && !gone && a->tries<8 && g->sched.now-a->when<=AXEDIT_TTL) {
            x->acts[n++] = *a;
        }
    }
    x->nacts = n;
    if (!n) {
        return;
    }
    AxAct *a = x->acts;
    if (axblocked(x, a->key)) {
        a->done = 1;  // refused, as a click would be
    } else if (!ctx->NavNextActivateId && !ctx->NavMoveSubmitted) {
        ctx->NavNextActivateId    = a->id;
        ctx->NavNextActivateFlags = a->kind==AXACT_FOCUS
                                  ? ImGuiActivateFlags_PreferInput|ImGuiActivateFlags_TryToPreserveState
                                  : ImGuiActivateFlags_None;
        a->tries++;
        a->seen = 0;
    }
    scheddirty(&g->sched);  // until it lands
}

// Ask for an action on the published node with key: 0 if it has no such
// action, or is disabled.
static b32 axaction(Gui *g, u32 key, i32 kind)
{
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    if (!x) {
        return 0;
    }
    AxTree *t = x->ax->pub;
    i32     i = axfind(t, key);
    u32     want = kind==AXACT_FOCUS ? AXA_FOCUS
                 : kind==AXACT_MENU  ? AXA_MENU
                 :                     AXA_PRESS|AXA_TOGGLE|AXA_EXPAND|AXA_SELECT;
    if (i<0 || !(t->nodes[i].actions & want) || (t->nodes[i].state & AXS_DISABLED) ||
            axblocked(x, key)) {
        return 0;
    } else if (kind == AXACT_MENU) {
        x->menuid   = t->nodes[i].id;  // for the item to see, as it asks
        x->menuwhen = g->sched.now;
        scheddirty(&g->sched);
        return 1;
    } else if (x->nacts == countof(x->acts)) {
        return 0;
    }
    AxAct a = {};
    a.key  = key;
    a.id   = t->nodes[i].id;
    a.kind = kind;
    a.when = g->sched.now;
    x->acts[x->nacts++] = a;
    scheddirty(&g->sched);
    return 1;
}

// Edit the focused field (axedit), for the next frame to apply.
static b32 axtextedit(Gui *g, i32 lo, i32 hi, Str text, i32 anchor, i32 caret)
{
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    if (!x || !axedit(x->ax, lo, hi, text, anchor, caret, g->sched.now)) {
        return 0;
    }
    scheddirty(&g->sched);
    return 1;
}

static b32 axtextselect(Gui *g, i32 anchor, i32 caret)
{
    AxGui *x = g->ax && g->ax->on ? g->ax : 0;
    if (!x || !axselect(x->ax, anchor, caret, g->sched.now)) {
        return 0;
    }
    scheddirty(&g->sched);
    return 1;
}

// The adapter's log (AxLog): each call an OS adapter answers, a line led
// by the seconds since the log began, kept for the Metrics pane and, with
// PROSPERO_AXTRACE=path, appended to that file as it comes, so that a
// trace can come from a release build, which has no Metrics pane. The
// adapter builds its lines in logmem, by value, never across a call
// into AppKit: the calls nest, and come inside frames as well as between.

static void axstamp(AxGui *x, Buf *b)
{
    i64 ms = (os_ticks(x->g->os) - x->logt0) / 1000;
    print(b, ms/1000);
    putbyte(b, '.');
    putbyte(b, (u8)('0' + ms/100%10));
    putbyte(b, (u8)('0' + ms/10%10));
    putbyte(b, (u8)('0' + ms%10));
    putbyte(b, ' ');
}

// Log line, built in scratch's arena; the rest of scratch is free. A
// line the same as the last is counted on it, with the time of the
// latest, and the trace has the count once the run ends.
static void axnote(AxGui *x, Arena scratch, Str line)
{
    AxLog *l    = x->log;
    i32    was  = l->run;
    i32    run  = axlogrun(l, line);
    Buf    b(&scratch, line.len+64);
    if (run==1 && was>1 && x->tracefd>=0) {
        axstamp(x, &b);
        print(&b, "(the line before, ");
        print(&b, (i64)was);
        print(&b, " times in a row)\n");
        os_logwrite(x->g->os, x->tracefd, finish(&b));
        b.len = 0;
    }
    axstamp(x, &b);
    print(&b, line);
    if (run > 1) {
        print(&b, " (\xc3\x97");
        print(&b, (i64)run);
        putbyte(&b, ')');
        axlogredo(l, finish(&b));
        return;
    }
    putbyte(&b, '\n');
    Str s = finish(&b);
    axlogput(l, takehead(s, s.len-1));
    if (x->tracefd >= 0) {
        os_logwrite(x->g->os, x->tracefd, s);
    }
}

// The focused field's text model, as a tool is told of it: its length,
// selection and marked text in UTF-16 units, and its text.
static void axmodelline(AxGui *x, Buf *b)
{
    A11y   *ax = x->ax;
    AxText *t  = &ax->text;
    if (!x->on || !t->live) {
        print(b, x->on ? Str("model: no field") : Str("model: not started"));
        return;
    }
    print(b, "model: field ");
    print(b, (i64)t->key);
    print(b, " len ");
    print(b, (i64)axunitlen(ax, AXU_UTF16));
    print(b, " sel ");
    print(b, (i64)axtounit(ax, AXU_UTF16, t->anchor));
    putbyte(b, ',');
    print(b, (i64)axtounit(ax, AXU_UTF16, t->caret));
    if (t->state & AXS_READONLY) {
        print(b, " readonly");
    }
    if (t->state & AXS_PASSWORD) {
        print(b, " password");  // its text never logged
        return;
    }
    if (t->marked.len) {
        print(b, " marked ");
        axlogstr(b, t->marked);
        putbyte(b, ' ');
        print(b, (i64)axcount(t->marked, AXU_UTF16, t->mlo));
        putbyte(b, ',');
        print(b, (i64)axcount(t->marked, AXU_UTF16, t->mhi));
    }
    putbyte(b, ' ');
    axlogstr(b, t->text);
}

// Log the text model if it changed since it was last logged: ahead of a
// call, as its ranges are against it.
static void axnotemodel(AxGui *x, Arena scratch)
{
    Buf b(&scratch, 256);
    axmodelline(x, &b);
    Str s = finish(&b);
    u64 h = hash(s);
    if (h != x->logmodel) {
        x->logmodel = h;
        axnote(x, scratch, s);
    }
}

// Lines logged since total since that hold want, for the checklist.
static i32 axlogged(Gui *g, i64 since, Str want)
{
    AxLog *l = g->ax ? g->ax->log : 0;
    i32    n = 0;
    for (iz i = 0, kept = l ? axlogkept(l) : 0; i < kept; i++) {
        n += l->total-kept+i>=since && find(axlogline(l, i), want)>=0;
    }
    return n;
}

static Str const axsetnames[] = {"auto", "on", "off"};

static void axtraceopen(AxGui *x)
{
    Gui  *g    = x->g;
    Arena a    = x->logmem;
    Str   path = os_getenv(g->os, &a, "PROSPERO_AXTRACE");
    if (!path.len) {
        return;
    }
    x->tracefd = os_logopen(g->os, a, path);
    if (x->tracefd>=0 && !os_logprivate(g->os, x->tracefd)) {
        os_logclose(g->os, x->tracefd);  // typed text mustn't be readable to all
        x->tracefd = -1;
    }
    if (x->tracefd < 0) {
        guilog(g, concat(&a, "could not open the accessibility trace, ", path));
        return;
    }
    x->tracepath = clone(&g->perm, path);
    Buf b(&a, 128);
    print(&b, "trace: prospero " PROSPERO_VERSION ", pid ");
    print(&b, (i64)os_pid(g->os));
    print(&b, ", accessibility ");
    print(&b, axsetnames[g->axset]);
    axnote(x, a, finish(&b));
}

// The Metrics pane's Accessibility section (Debug builds): the setting,
// the focus, the text model, and the adapter's log, to copy.
static void axmetrics(Gui *g)
{
    AxGui *x = g->ax;
    Arena  a = g->frame;
    Buf    b(&a, 256);
    ImGui::SeparatorText("Accessibility");
    print(&b, "setting ");
    print(&b, axsetnames[g->axlint ? AXSET_ON : g->axset]);
    print(&b, !x ? Str("") : x->on ? Str(", started") : Str(", not started"));
    if (x && x->tracefd>=0) {
        print(&b, ", tracing to ");
        print(&b, x->tracepath);
    }
    text(finish(&b));
    if (!x) {
        return;
    }

    AxTree *t = x->ax->pub;
    b.len = 0;
    print(&b, "focus ");
    if (t->focus < 0) {
        print(&b, "none");
    } else {
        AxNode *n = t->nodes + t->focus;
        print(&b, axrolenames[n->role<AX_NROLES ? n->role : 0]);
        putbyte(&b, ' ');
        axquote(&b, n->name);
        print(&b, ", key ");
        print(&b, (i64)n->key);
    }
    print(&b, "  nodes ");
    print(&b, (i64)t->len);
    print(&b, "  queued edits ");
    i64 queued = 0;
    for (iz i = x->ax->qhead; i < x->ax->queue.len; i++) {
        queued += x->ax->queue[i].id != 0;
    }
    print(&b, queued);
    print(&b, "  actions ");
    print(&b, (i64)x->nacts);
    ImGui::PushTextWrapPos(0);
    text(finish(&b));

    b.len = 0;
    Buf mb(&a, 256);
    axmodelline(x, &mb);
    Str model = finish(&mb);
    text(takehead(model, axcutat(model, 400)));
    ImGui::PopTextWrapPos();

    AxLog *l    = x->log;
    iz     kept = axlogkept(l);
    b.len = 0;
    print(&b, "log ");
    print(&b, (i64)kept);
    print(&b, " lines, of ");
    print(&b, l->total);
    ImGui::AlignTextToFramePadding();
    text(finish(&b));
    ImGui::SameLine();
    ImGui::BeginDisabled(!kept);
    if (ImGui::SmallButton("Copy log##axlog")) {
        Buf all(&a, kept*64);
        for (iz i = 0; i < kept; i++) {
            print(&all, axlogline(l, i));
            putbyte(&all, '\n');
        }
        ImGui::SetClipboardText(cstr0(&a, finish(&all)));
    }
    ImGui::EndDisabled();
    itemtip("Copy the accessibility adapter's calls, as text");

    f32 h = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##axlog", v2(0, 12*h), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clip;
        clip.Begin((i32)kept);
        while (clip.Step()) {
            for (i32 i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                Str s = axlogline(l, i);
                ImGui::TextUnformatted(s.cdata, s.cdata+s.len);
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1);  // following the latest
        }
    }
    ImGui::EndChild();
}

static void axnewframe(ImGuiContext *ctx, ImGuiContextHook *hook)
{
    AxGui *x = (AxGui *)hook->UserData;
    if (!x->on) {
        return;
    }
    axdeliver(x, ctx);
    if (x->navid) {
        // As though the keyboard moved there, letting go of a field
        ImGuiWindow *w = ImGui::FindWindowByID(x->navwin);
        if (w) {
            ImGui::ClearActiveID();
            ImGui::FocusWindow(w);
            ImGui::SetNavID(x->navid, ImGuiNavLayer_Main, w->NavRootFocusScopeId, ImRect());
            ctx->NavInputSource           = ImGuiInputSource_Keyboard;
            ctx->NavCursorVisible         = true;
            ctx->NavHighlightItemUnderNav = true;
        }
        x->navid = 0;
    }
    if (x->menuid && x->g->sched.now-x->menuwhen>AXEDIT_TTL) {
        x->menuid = 0;  // its item never asked
    }
    ctx->TestEngineHookItems = true;
    x->mem.beg = x->base;
    x->items   = {};
    x->fields  = {};
    x->nextname = {};
    x->last    = -1;
    x->tabid   = 0;
}

static void axendframe(ImGuiContext *ctx, ImGuiContextHook *hook)
{
    AxGui *x = (AxGui *)hook->UserData;
    if (x->on) {
        axbuild(x, ctx);
        if (x->published) {
            x->published(x);
        }
    }
}

// Set up, at the context's creation, after the configuration is read:
// on at once with [ui] accessibility = on.
static void axinit(Gui *g, i32 set)
{
    if (set == AXSET_OFF) {
        return;
    }
    Os     *os = g->os;
    iz      mb = (iz)1 << 20;
    AxGui  *x  = alloc<AxGui>(&g->perm);
    x->g    = g;
    x->mem  = newarena(os_reserve(os, 256*mb));
    x->base = x->mem.beg;
    x->last = -1;
    Arena mem[AXM_N];
    for (i32 i = 0; i < AXM_N; i++) {
        mem[i] = newarena(os_reserve(os, 256*mb));
    }
    x->ax = newa11y(&g->perm, mem);
    g->ax = x;
    x->log     = newaxlog(&g->perm);
    x->logmem  = carve(&g->perm, (iz)1 << 18);
    x->logt0   = os_ticks(os);
    x->tracefd = -1;
    axtraceopen(x);

    ImGuiContext    *ctx  = ImGui::GetCurrentContext();
    ImGuiContextHook hook = {};
    hook.Type     = ImGuiContextHookType_NewFramePre;
    hook.Callback = axnewframe;
    hook.UserData = x;
    ImGui::AddContextHook(ctx, &hook);
    hook.Type     = ImGuiContextHookType_EndFramePost;
    hook.Callback = axendframe;
    ImGui::AddContextHook(ctx, &hook);

    if (set==AXSET_ON || x->tracefd>=0) {
        x->on = 1;  // a trace wants the text model the calls are against
    }
}

// An assistive tool is listening: build trees from the next frame.
static void axstart(Gui *g)
{
    if (g->ax && !g->ax->on) {
        g->ax->on = 1;
        scheddirty(&g->sched);
    }
}


// The GUI checklist's OP_A11Y: text is a command
//   on               build trees from now, as when a tool first asks
//   dump             log the published tree
//   has ROLE NAME    check that a node has the role and name
//   focus ROLE NAME  check that the focus has them
//   value TEXT       check the focus's value
//   help ROLE NAME   check that a node has them, and help
//   can ACTS ROLE NAME  check the item's actions: press,menu, or nothing
//                    (of the item in front, if more than one has the name)
//   is STATES ROLE NAME  check the item's states: checked,!expanded
//   edit LO HI TEXT  edit the focused field: check that it's taken
//   noedit LO HI TEXT  ... or refused
//   select A C       select in the focused field
//   text TEXT        check the focused field's text here, ahead of ImGui's
//   queued N         check the edits waiting for ImGui
//   press ROLE NAME  press the node: check that it's taken
//   activate ROLE NAME  focus the field, to edit
//   pending N        check the actions waiting for ImGui
//   nav ROLE NAME    make the item ImGui's keyboard item, as arrows would
//   lint             check that nothing actionable is unnamed, logging
//                    what is
//   logged TEXT      check that the adapter logged a line holding TEXT
//                    since the last OP_NSTEXT
//   ns ...           the OS adapter's own checks, through the OS's API
//                    (macOS: a11y_macos.cpp); skipped where there is none

// what may be in a, the caller's scratch
static void axcheck(Gui *g, Arena *a, b32 ok, Str what)
{
    g->drivechecks++;
    g->drivefails += !ok;
    Buf b(a, 128);
    print(&b, "drive: ");
    print(&b, ok ? Str("ok   ") : Str("FAIL "));
    print(&b, what);
    guilog(g, finish(&b));
}

static b32 axmatch(AxNode *n, Str role, Str name)
{
    return axrolenames[n->role] == role && n->name == name;
}

// The item so named, not a pane of that name, in the window in front
// if more than one has it, as a click finds it (the tree goes back to
// front): t->len if none
static i32 axitemat(AxTree *t, Str role, Str name)
{
    for (i32 i = t->len-1; i >= 0; i--) {
        if (axmatch(t->nodes+i, role, name) && !ImGui::FindWindowByID(t->nodes[i].key)) {
            return i;
        }
    }
    return t->len;
}

static void axcount(Gui *g, Arena *a, Str what, i64 got, Str want)
{
    Str msg = concat(a, what, want);
    if (parseint(want).val != got) {
        Buf b(a, 32);
        print(&b, ", not ");
        print(&b, got);
        msg = concat(a, msg, finish(&b));
    }
    axcheck(g, a, parseint(want).val==got, msg);
}

static void driveax(Gui *g, Str cmd)
{
    Arena   a   = g->frame;
    Cut     c   = cut(cmd, ' ');
    Str     verb = c.head;
    Cut     r   = cut(c.tail, ' ');
    AxTree *t   = g->ax ? g->ax->ax->pub : 0;

    if (verb == "on") {
        axstart(g);
        guilog(g, g->ax ? Str("drive: accessibility on") : Str("drive: accessibility is off"));
    } else if (!t) {
        axcheck(g, &a, 0, concat(&a, "accessibility (off): ", cmd));
    } else if (verb == "dump") {
        Buf b(&a, 1<<12);
        print(&b, "drive: the accessibility tree:\n");
        axdump(&b, t, 0);
        guilog(g, trimright(finish(&b)));
    } else if (verb == "has") {
        b32 found = 0;
        for (i32 i = 0; i < t->len; i++) {
            found |= axmatch(t->nodes+i, r.head, r.tail);
        }
        axcheck(g, &a, found, concat(&a, "accessible: ", c.tail));
    } else if (verb == "help") {
        b32 found = 0;
        for (i32 i = 0; i < t->len; i++) {
            found |= axmatch(t->nodes+i, r.head, r.tail) && t->nodes[i].help.len;
        }
        axcheck(g, &a, found, concat(&a, "accessible help: ", c.tail));
    } else if (verb == "can") {
        // can ACTIONS ROLE NAME: the first item so named offers just those
        // actions, a comma between, or "nothing"
        static Str const acts[] = {"press", "toggle", "expand", "focus", "edit", "select", "menu"};
        Cut  rn   = cut(r.tail, ' ');
        u32  want = 0;
        for (Cut w = cut(r.head, ','); w.head.len; w = cut(w.tail, ',')) {
            for (i32 k = 0; k < countof(acts); k++) {
                want |= w.head==acts[k] ? 1u<<k : 0;
            }
        }
        i32 i    = axitemat(t, rn.head, rn.tail);
        u32 got  = i<t->len ? t->nodes[i].actions : ~0u;
        Str what = concat(&a, "accessible actions: ", c.tail);
        if (i<t->len && got!=want) {
            Buf b(&a, 64);
            print(&b, ", not");
            for (i32 k = 0; k < countof(acts); k++) {
                if (got & (1u<<k)) {
                    putbyte(&b, ' ');
                    print(&b, acts[k]);
                }
            }
            what = concat(&a, what, finish(&b));
        }
        axcheck(g, &a, got==want, what);
    } else if (verb == "is") {
        // is STATES ROLE NAME: the first item so named has each state, a
        // comma between, with a '!' before one it hasn't
        Cut rn = cut(r.tail, ' ');
        i32 i  = axitemat(t, rn.head, rn.tail);
        b32 ok = i < t->len;
        for (Cut w = cut(r.head, ','); ok && w.head.len; w = cut(w.tail, ',')) {
            b32 no  = w.head[0] == '!';
            Str s   = no ? cuthead(w.head, 1) : w.head;
            u32 bit = 0;
            for (i32 k = 0; k < (i32)AXS_NAMES; k++) {
                bit |= s==axstatenames[k] ? 1u<<k : 0;
            }
            ok = bit && ((t->nodes[i].state & bit) != 0) != no;
        }
        axcheck(g, &a, ok, concat(&a, "accessible state: ", c.tail));
    } else if (verb == "focus") {
        b32 ok = t->focus>=0 && axmatch(t->nodes+t->focus, r.head, r.tail);
        Str what = concat(&a, "accessible focus: ", c.tail);
        if (!ok && t->focus>=0) {
            AxNode *n = t->nodes + t->focus;
            what = concat(&a, what, concat(&a, ", not ", concat(&a, axrolenames[n->role],
                          concat(&a, " ", n->name))));
        }
        axcheck(g, &a, ok, what);
    } else if (verb == "value") {
        Str got = t->focus>=0 ? t->nodes[t->focus].value : Str("(no focus)");
        Str what = concat(&a, "accessible value: ", c.tail);
        if (!(got == c.tail)) {
            what = concat(&a, what, concat(&a, ", not ", got));
        }
        axcheck(g, &a, got==c.tail, what);
    } else if (verb=="edit" || verb=="noedit") {
        Cut hi  = cut(r.tail, ' ');
        b32 got = axtextedit(g, (i32)parseint(r.head).val, (i32)parseint(hi.head).val,
                             hi.tail, AX_AFTER, AX_AFTER);
        axcheck(g, &a, got == (verb=="edit"), concat(&a, "accessible ", cmd));
    } else if (verb == "select") {
        b32 got = axtextselect(g, (i32)parseint(r.head).val, (i32)parseint(r.tail).val);
        axcheck(g, &a, got, concat(&a, "accessible ", cmd));
    } else if (verb == "text") {
        Str got  = g->ax->ax->text.text;
        Str what = concat(&a, "accessible text: ", c.tail);
        if (!(got == c.tail)) {
            what = concat(&a, what, concat(&a, ", not ", got));
        }
        axcheck(g, &a, got==c.tail, what);
    } else if (verb == "queued") {
        A11y *x = g->ax->ax;
        i64   n = 0;
        for (iz i = x->qhead; i < x->queue.len; i++) {
            n += x->queue[i].id != 0;
        }
        axcount(g, &a, "accessible edits queued: ", n, c.tail);
    } else if (verb=="press" || verb=="activate" || verb=="menu") {
        // The one in front that has it (axitemat): a pane and its
        // contents may share a name, and a picker's row a session's
        i32 kind = verb=="press" ? AXACT_PRESS : verb=="menu" ? AXACT_MENU : AXACT_FOCUS;
        b32 got  = 0;
        for (i32 i = t->len-1; !got && i>=0; i--) {
            got = axmatch(t->nodes+i, r.head, r.tail) && axaction(g, t->nodes[i].key, kind);
        }
        axcheck(g, &a, got, concat(&a, "accessible ", cmd));
    } else if (verb == "pending") {
        axcount(g, &a, "accessible actions pending: ", g->ax->nacts, c.tail);
    } else if (verb == "nav") {
        i32 i = axitemat(t, r.head, r.tail);
        i32 p = i<t->len ? t->nodes[i].parent : -1;
        if (p >= 0) {
            g->ax->navid  = t->nodes[i].id;
            g->ax->navwin = t->nodes[p].key;
            scheddirty(&g->sched);
        }
        axcheck(g, &a, p>=0, concat(&a, "accessible ", cmd));
    } else if (verb == "marked") {
        Str got  = g->ax->ax->text.marked;
        Str what = concat(&a, "accessible marked text: ", c.tail);
        if (!(got == c.tail)) {
            what = concat(&a, what, concat(&a, ", not ", got));
        }
        axcheck(g, &a, got==c.tail, what);
    } else if (verb == "doc") {
        Str got  = axdoc(g->ax->ax, &a);
        Str what = concat(&a, "accessible document: ", c.tail);
        if (!(got == c.tail)) {
            what = concat(&a, what, concat(&a, ", not ", got));
        }
        axcheck(g, &a, got==c.tail, what);
    } else if (verb=="caret" || verb=="ime") {
        // Where ImGui told the platform its IME goes (one left of the
        // caret's top, or of the composition's clause, ime.cpp): where
        // the text model has them, give or take truncation.
        A11y   *x = g->ax->ax;
        AxText *tx = &x->text;
        AxRect  rc = {};
        b32     ok = tx->live && tx->geom.rev==tx->rev;
        if (verb == "caret") {
            ok = ok && !tx->marked.len;
            rc = axcaretrect(x);
        } else {
            // And it went where the text model has the caret, or the
            // selection's start, which it replaces
            AxSpan sp = axselection(tx);
            AxRect at = sp.lo==sp.hi ? axcaretrect(x) : axcharrect(x, sp.lo);
            f32    ax = g->ime->at.x - (f32)(i32)at.x0;
            f32    ay = g->ime->at.y - (f32)(i32)at.y0;
            ok = ok && tx->marked.len && ax>=-1 && ax<=1 && ay>=-1 && ay<=1;
            rc = axdocrect(x, sp.lo + tx->mlo);
        }
        ImGuiPlatformImeData *d = &ImGui::GetCurrentContext()->PlatformImeData;
        f32 dx = d->InputPos.x + 1 - (f32)(i32)rc.x0;
        f32 dy = d->InputPos.y - (f32)(i32)rc.y0;
        ok = ok && dx>=-1 && dx<=1 && dy>=-1 && dy<=1;
        Buf b(&a, 128);
        print(&b, "accessible ");
        print(&b, verb);
        print(&b, " at ");
        print(&b, axround(rc.x0));
        putbyte(&b, ',');
        print(&b, axround(rc.y0));
        print(&b, ", IME at ");
        print(&b, axround(d->InputPos.x + 1));
        putbyte(&b, ',');
        print(&b, axround(d->InputPos.y));
        axcheck(g, &a, ok, finish(&b));
    } else if (verb == "logged") {
        axcheck(g, &a, axlogged(g, g->ax->logmark, c.tail) > 0,
                concat(&a, "accessibility log: ", c.tail));
    } else if (verb == "ns") {
        if (g->ax->osdrive) {
            g->ax->osdrive(g, c.tail);
        } else {
            guilog(g, concat(&a, "drive: skipped (no OS adapter here): ", cmd));
        }
    } else if (verb == "lint") {
        Slice<i32> bad = axlint(&a, t);
        Buf b(&a, 256);
        print(&b, "accessible names: ");
        print(&b, (i64)bad.len);
        print(&b, " unnamed");
        for (iz i = 0; i < bad.len; i++) {
            AxNode *n = t->nodes + bad[i];
            print(&b, i ? Str(", ") : Str(": "));
            print(&b, axrolenames[n->role]);
            print(&b, " @");
            print(&b, axround(n->rect.x0));
            putbyte(&b, ',');
            print(&b, axround(n->rect.y0));
        }
        axcheck(g, &a, !bad.len, finish(&b));
    } else {
        axcheck(g, &a, 0, concat(&a, "accessibility: no such command: ", cmd));
    }
}
