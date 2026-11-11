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
};

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
    x->items = push(&x->mem, x->items, it);
    x->last  = x->items.len - 1;
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
    it->state |= it->itemflags & ImGuiItemFlags_Disabled ? AXS_DISABLED : 0;
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

extern "C" void prospero_axfield(ProsperoAxField const *f)
{
    AxGui *x = axgui(ImGui::GetCurrentContext());
    if (!x) {
        return;
    }
    AxFieldDraw d = {};
    d.id       = f->id;
    d.flags    = f->flags;
    d.active   = f->active;
    d.password = !f->text;
    d.text     = clone(&x->mem, Str{(u8 *)f->text, f->text ? f->len : 0});
    d.hint     = clone(&x->mem, axcstr(f->hint));
    d.anchor   = f->anchor;
    d.caret    = f->caret;
    AxGeom *g  = &d.geom;
    g->x       = f->x;
    g->y       = f->y;
    g->lineh   = f->lineh;
    g->clip    = {f->clip[0], f->clip[1], f->clip[2], f->clip[3]};
    if (f->active) {
        g->nlines = f->nlines;
        g->lines  = alloc<i32>(&x->mem, f->nlines);
        for (i32 i = 0; i < f->nlines; i++) {
            g->lines[i] = f->lines[i];
        }
        g->vis0 = f->vis0;
        g->vis1 = f->vis1;
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

static void axname(Gui *g, Str name)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->name  = clone(&g->ax->mem, name);
        it->named = 1;
    }
}

static void axrole(Gui *g, AxRole role)
{
    AxItem *it = axlastitem(g);
    if (it) {
        it->role = role;
        it->info = 1;
    }
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
        } else if (flags & ImGuiWindowFlags_ChildWindow) {
            role   = AX_GROUP;
            parent = w->ParentWindow ? w->ParentWindow->ID : 0;
            name   = {};
        } else if (flags & ImGuiWindowFlags_ChildMenu) {
            role = AX_MENU;
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
        AxNode *n    = axadd(b, parent, it->id, role, name);
        n->rect    = {it->rect.Min.x, it->rect.Min.y, it->rect.Max.x, it->rect.Max.y};
        n->state   = state;
        n->actions = it->actions;
        if (f && !f->password) {
            axsetvalue(b, n, f->text);
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

    if (active) {
        u32 state = active->password ? AXS_PASSWORD : 0;
        state |= active->flags & ImGuiInputTextFlags_ReadOnly ? AXS_READONLY : 0;
        axtextsync(x->ax, active->id, active->id, state,
                   (active->flags & ImGuiInputTextFlags_Multiline) != 0,
                   active->text, active->anchor, active->caret, &active->geom);
    } else {
        axtextnone(x->ax);
    }

    x->events = axpublish(x->ax, focus);
    x->publishes++;
}

static void axnewframe(ImGuiContext *ctx, ImGuiContextHook *hook)
{
    AxGui *x = (AxGui *)hook->UserData;
    if (!x->on) {
        return;
    }
    ctx->TestEngineHookItems = true;
    x->mem.beg = x->base;
    x->items   = {};
    x->fields  = {};
    x->last    = -1;
}

static void axendframe(ImGuiContext *ctx, ImGuiContextHook *hook)
{
    AxGui *x = (AxGui *)hook->UserData;
    if (x->on) {
        axbuild(x, ctx);
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

    ImGuiContext    *ctx  = ImGui::GetCurrentContext();
    ImGuiContextHook hook = {};
    hook.Type     = ImGuiContextHookType_NewFramePre;
    hook.Callback = axnewframe;
    hook.UserData = x;
    ImGui::AddContextHook(ctx, &hook);
    hook.Type     = ImGuiContextHookType_EndFramePost;
    hook.Callback = axendframe;
    ImGui::AddContextHook(ctx, &hook);

    if (set == AXSET_ON) {
        x->on = 1;
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
//   lint             check that nothing actionable is unnamed, logging
//                    what is

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
