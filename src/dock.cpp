// Dock layouts: how the panes are arranged, as the View menu's presets
// and the user's saved layouts have it
//
// A layout is a tree over the dock space, kept in pre-order, root first.
// A split divides its node along X (left | right) or Y (top over
// bottom), its first child taking share of it; a leaf holds panes as
// tabs, in order, one of them in front. Exactly one leaf is the dock
// space's central node, which stays when its panes close, where the
// other leaves close with their last pane. A pane's leaf is its home,
// where it shows whenever it is shown. A pane with no home floats, at
// the layout's place for it if it has one, as shares of the dock space.
// A layout also says which panes show, but a preset leaves some shown
// or hidden as they are (keep).
//
// The presets:
//   Standard  Sessions over Todo, Transcript over Prompt, and a right
//             column of Jobs and Log over Plan
//   Compact   Standard without its right column: Jobs behind Todo, Plan
//             behind Transcript, and Log closed
//   Focus     a session alone: Transcript, Todo and Plan as tabs, over
//             Prompt
//
// A saved layout is kept in the layout file (imgui.ini), a section
// each, with a line a part in pre-order (dockwrite, dockline):
//
//   [Prospero][Layout Review]
//   Shown=Sessions,Transcript,Prompt,Todo,Plan,Jobs,Log
//   Split=X,0.75
//   Split=X,0.22
//   Split=Y,0.75
//   Tabs=Sessions
//   Tabs=Todo
//   Split=Y,0.72
//   Center=Transcript
//   Tabs=Prompt
//   Split=Y,0.5
//   Tabs=*Jobs,Log,Help,Settings,Hosts
//   Tabs=Plan,Changes
//   Float=Metrics,0.3,0.2,0.4,0.5
//
// A * marks the tab in front, else the first is. Float gives a pane's
// place: x, y, width and height. A pane not known (a layout newer than
// this build) is skipped, and so is a pane placed again, and a leaf
// left empty is dropped. Parts that make no whole tree, or no one
// Center, refuse the layout (dockdone).
//
// This is free and unencumbered software released into the public domain.

// The panes, which the View menu shows and hides
enum : i32 {
    PANE_SESSIONS,
    PANE_TRANSCRIPT,
    PANE_PROMPT,
    PANE_TODO,
    PANE_PLAN,
    PANE_CHANGES,
    PANE_JOBS,
    PANE_LOG,
    PANE_HOSTS,
    PANE_SETTINGS,
    PANE_HELP,
    PANE_METRICS,
    PANE_COUNT,
};
static_assert(PANE_COUNT <= 32);  // as bits of a u32

static char const *panenames[] = {
    "Sessions", "Transcript", "Prompt", "Todo", "Plan", "Changes", "Jobs", "Log",
    "Hosts", "Settings", "Help", "Metrics",
};
static_assert(countof(panenames) == PANE_COUNT);

// The panes hidden until asked for: the metrics, and the changes, which
// a click on the session bar's branch shows (ui.cpp drawbranch).
static b32 panehidden(i32 pane)
{
    return pane==PANE_METRICS || pane==PANE_CHANGES;
}

#ifndef PROSPERO_DEVTOOLS
#  define PROSPERO_DEVTOOLS 0  // the developer's panes (CMakeLists.txt)
#endif

// Whether a pane is there to show at all: the metrics are a developer's
// tool, left out of the builds released.
static b32 paneoffered(i32 pane)
{
    return PROSPERO_DEVTOOLS || pane!=PANE_METRICS;
}

static Str panestr(i32 pane)
{
    return {(u8 *)panenames[pane], (iz)__builtin_strlen(panenames[pane])};
}

// The pane of a name, or -1.
static i32 panenamed(Str name)
{
    for (i32 i = 0; i < PANE_COUNT; i++) {
        if (name == panestr(i)) {
            return i;
        }
    }
    return -1;
}

// Standard's splits, each a share of the node split: the right column's
// of the width, the left column's of the rest, the prompt's of the
// center's height, and the lower node's of each column. At the first
// window's 1700 pixels wide, the side columns are 425 and about 280
// wide; at 1280, 320 and about 210.
static f32 const LAYOUT_RIGHT  = 0.25f;
static f32 const LAYOUT_LEFT   = 0.22f;
static f32 const LAYOUT_PROMPT = 0.28f;
static f32 const LAYOUT_PLAN   = 0.5f;
static f32 const LAYOUT_TODO   = 0.25f;

enum : i32 {
    PRESET_STANDARD,
    PRESET_COMPACT,
    PRESET_FOCUS,
    PRESET_COUNT,
};

static Str presetnames[] = {"Standard", "Compact", "Focus"};
static_assert(countof(presetnames) == PRESET_COUNT);

// What each preset is, for the View menu
static Str presettips[] = {
    "Sessions and Todo on the left, Jobs, Log and Plan on the right",
    "Standard without its right column: Jobs beside Todo, Plan beside Transcript",
    "One session alone: Transcript, Todo and Plan over Prompt",
};
static_assert(countof(presettips) == PRESET_COUNT);

enum : i32 {
    DOCK_LEAF,
    DOCK_X,  // a split: left | right
    DOCK_Y,  // a split: top over bottom
};

enum : i32 {
    DOCK_PARTS   = 64,  // splits and leaves in a layout, at most
    DOCK_NAMEMAX = 64,  // bytes of a saved layout's name
};
static_assert(DOCK_PARTS >= 2*PANE_COUNT + 1);  // a leaf a pane, an empty center

static f32 const DOCK_MINSHARE = 0.05f;  // of a split, or of a place's size

struct DockPart {
    i32 kind;              // DOCK_LEAF, DOCK_X or DOCK_Y
    f32 share;             // a split's: its first child's share of it
    i32 tabs[PANE_COUNT];  // a leaf's panes, in tab order
    i32 ntabs;
    i32 front;             // a leaf's pane in front, -1 for its first
    b32 central;           // a leaf's: the dock space's central node
};

struct DockRect {
    f32 x, y, w, h;  // shares of the dock space
};

struct DockLayout {
    DockPart parts[DOCK_PARTS];  // pre-order, root first
    i32      nparts;
    DockRect place[PANE_COUNT];  // where a floating pane goes
    u32      floats;             // the panes with a place
    u32      shown;              // the panes it shows
    u32      keep;               // the panes it leaves shown or hidden
    b32      bad;                // a line could not be read
    Str      name;
};

// Clamp, NaN to lo.
static f32 dockclamp(f64 v, f32 lo, f32 hi)
{
    return !(v >= lo) ? lo : v > hi ? hi : (f32)v;
}

// Append a part, or spoil the layout if it has too many.
static DockPart *dockpart(DockLayout *l, i32 kind)
{
    if (l->nparts == DOCK_PARTS) {
        l->bad = 1;
        return 0;
    }
    DockPart *p = l->parts + l->nparts++;
    *p = {};
    p->kind  = kind;
    p->front = -1;
    return p;
}

static void docksplit(DockLayout *l, i32 kind, f64 share)
{
    DockPart *p = dockpart(l, kind);
    if (p) {
        p->share = dockclamp(share, DOCK_MINSHARE, 1 - DOCK_MINSHARE);
    }
}

// Add a pane to a leaf's tabs, unless it is there.
static void docktab(DockPart *p, i32 pane)
{
    for (i32 t = 0; t < p->ntabs; t++) {
        if (p->tabs[t] == pane) {
            return;
        }
    }
    p->tabs[p->ntabs++] = pane;
}

// Give a pane a place to float, kept within the dock space.
static void dockfloat(DockLayout *l, i32 pane, f64 x, f64 y, f64 w, f64 h)
{
    DockRect *r = l->place + pane;
    r->w = dockclamp(w, DOCK_MINSHARE, 1);
    r->h = dockclamp(h, DOCK_MINSHARE, 1);
    r->x = dockclamp(x, 0, 1 - r->w);
    r->y = dockclamp(y, 0, 1 - r->h);
    l->floats |= 1u << pane;
}

// The panes a layout places, in its leaves or floating at a place.
static u32 dockplaced(DockLayout *l)
{
    u32 r = l->floats;
    for (i32 i = 0; i < l->nparts; i++) {
        DockPart *p = l->parts + i;
        for (i32 t = 0; t < p->ntabs; t++) {
            r |= 1u << p->tabs[t];
        }
    }
    return r;
}

// Read one line of a layout, as the layout file has it. Unknown keys
// and panes are skipped, but a line malformed spoils the layout.
static void dockline(DockLayout *l, Str key, Str val)
{
    if (key == "Shown") {
        for (Str rest = val; rest.len;) {
            Cut c    = cut(rest, ',');
            i32 pane = panenamed(trim(c.head));
            rest = c.tail;
            l->shown |= pane<0 ? 0 : 1u<<pane;
        }

    } else if (key == "Split") {
        Cut   axis  = cut(val, ',');
        Float share = parsefloat(trim(cut(axis.tail, ',').head));
        Str   name  = trim(axis.head);
        if (!share.ok || !(name=="X" || name=="Y")) {
            l->bad = 1;
            return;
        }
        docksplit(l, name=="X" ? DOCK_X : DOCK_Y, share.val);

    } else if (key=="Tabs" || key=="Center") {
        u32       placed = dockplaced(l);
        DockPart *p      = dockpart(l, DOCK_LEAF);
        if (!p) {
            return;
        }
        p->central = key == "Center";
        for (Str rest = val; rest.len;) {
            Cut c     = cut(rest, ',');
            Str name  = trim(c.head);
            b32 front = name.len && name[0]=='*';
            i32 pane  = panenamed(trim(cuthead(name, front)));
            rest = c.tail;
            if (pane>=0 && !(placed>>pane & 1)) {
                placed |= 1u << pane;
                docktab(p, pane);
                p->front = front ? pane : p->front;
            }
        }

    } else if (key == "Float") {
        Cut c    = cut(val, ',');
        i32 pane = panenamed(trim(c.head));
        f64 v[4] = {};
        for (i32 i = 0; i < 4; i++) {
            Cut   n = cut(c.tail, ',');
            Float f = parsefloat(trim(n.head));
            if (!f.ok) {
                l->bad = 1;
                return;
            }
            v[i]   = f.val;
            c.tail = n.tail;
        }
        if (pane>=0 && !(dockplaced(l)>>pane & 1)) {
            dockfloat(l, pane, v[0], v[1], v[2], v[3]);
        }
    }
}

// Copy the subtree at in's part *i to the end of out, leaving out the
// leaves with no panes but the center: a split left with one child gives
// way to it. Returns whether anything of it is left.
static b32 dockprune(DockLayout *out, DockLayout *in, i32 *i)
{
    DockPart p = in->parts[(*i)++];
    if (p.kind == DOCK_LEAF) {
        if (p.ntabs || p.central) {
            out->parts[out->nparts++] = p;
        }
        return p.ntabs || p.central;
    }
    i32 at = out->nparts++;
    out->parts[at] = p;
    b32 first  = dockprune(out, in, i);
    b32 second = dockprune(out, in, i);
    if (!first || !second) {
        for (i32 k = at; k < out->nparts-1; k++) {
            out->parts[k] = out->parts[k+1];
        }
        out->nparts--;
    }
    return first || second;
}

// Check a layout once read, and drop its empty leaves. Its parts must
// make one whole tree, with one Center.
static b32 dockdone(DockLayout *l)
{
    i32 need    = 1;  // parts still wanted to finish the tree
    i32 centers = 0;
    for (i32 i = 0; i < l->nparts; i++) {
        if (!need) {
            return 0;  // a second tree
        }
        need    += l->parts[i].kind==DOCK_LEAF ? -1 : 1;
        centers += l->parts[i].central;
    }
    if (l->bad || need || centers!=1) {
        return 0;
    }
    DockLayout in = *l;
    i32        i  = 0;
    l->nparts = 0;
    dockprune(l, &in, &i);
    return 1;
}

static void dockpanes(Buf *b, u32 panes)
{
    for (i32 i = 0, n = 0; i < PANE_COUNT; i++) {
        if (panes>>i & 1) {
            print(b, n++ ? Str(",") : Str(""));
            print(b, panestr(i));
        }
    }
}

// A layout's lines for the layout file, as dockline reads them.
static void dockwrite(Buf *b, DockLayout *l)
{
    print(b, "Shown=");
    dockpanes(b, l->shown);
    putbyte(b, '\n');
    for (i32 i = 0; i < l->nparts; i++) {
        DockPart *p = l->parts + i;
        if (p->kind != DOCK_LEAF) {
            print(b, p->kind==DOCK_X ? Str("Split=X,") : Str("Split=Y,"));
            printfloat(b, p->share, 4);
        } else {
            print(b, p->central ? Str("Center=") : Str("Tabs="));
            for (i32 t = 0; t < p->ntabs; t++) {
                b32 front = p->ntabs>1 && p->tabs[t]==p->front;
                print(b, t ? Str(",") : Str(""));
                print(b, front ? Str("*") : Str(""));
                print(b, panestr(p->tabs[t]));
            }
        }
        putbyte(b, '\n');
    }
    for (i32 i = 0; i < PANE_COUNT; i++) {
        if (l->floats>>i & 1) {
            DockRect r = l->place[i];
            print(b, "Float=");
            print(b, panestr(i));
            f32 v[] = {r.x, r.y, r.w, r.h};
            for (i32 k = 0; k < 4; k++) {
                putbyte(b, ',');
                printfloat(b, v[k], 4);
            }
            putbyte(b, '\n');
        }
    }
}

// The index just past the subtree at part i.
static i32 dockskip(DockLayout *l, i32 i)
{
    for (i32 need = 1; need; i++) {
        need += l->parts[i].kind==DOCK_LEAF ? -1 : 1;
    }
    return i;
}

// Whether the subtree at part i holds the center.
static b32 dockcentered(DockLayout *l, i32 i)
{
    for (i32 end = dockskip(l, i); i < end; i++) {
        if (l->parts[i].central) {
            return 1;
        }
    }
    return 0;
}

// The pane a leaf shows in front, of the panes shown: its front, else
// its first shown, or -1 if it shows none.
static i32 dockfront(DockPart *p, u32 shown)
{
    i32 first = -1;
    for (i32 t = 0; t < p->ntabs; t++) {
        i32 pane = p->tabs[t];
        if (shown>>pane & 1) {
            if (pane == p->front) {
                return pane;
            }
            first = first<0 ? pane : first;
        }
    }
    return first;
}

// Why a saved layout's name, trimmed, cannot be had, else empty. A ##
// would cut its label short in the menu (ImGui's id separator).
static Str docknamewhy(Str name)
{
    if (!name.len) {
        return "A layout needs a name";
    } else if (name.len > DOCK_NAMEMAX) {
        return "A name is at most 64 bytes";
    } else if (find(name, "##") >= 0) {
        return "A name cannot have ##";
    }
    for (iz i = 0; i < name.len; i++) {
        if (name[i]<0x20 || name[i]==0x7f) {
            return "A name cannot have control characters";
        }
    }
    for (i32 i = 0; i < PRESET_COUNT; i++) {
        if (equalfold(name, presetnames[i])) {
            return "A built-in layout has that name";
        }
    }
    return {};
}

// A preset, as if read from the layout file. Help, Settings and Hosts
// stay shown or hidden in Standard and Compact, whose side columns have
// little room for them.
static DockLayout dockpreset(i32 preset)
{
    u32        extras = 1u<<PANE_HELP | 1u<<PANE_SETTINGS | 1u<<PANE_HOSTS;
    DockLayout l      = {};
    l.name = presetnames[preset];
    switch (preset) {
    case PRESET_STANDARD:
        dockline(&l, "Shown", "Sessions,Transcript,Prompt,Todo,Plan,Jobs,Log");
        docksplit(&l, DOCK_X, 1 - LAYOUT_RIGHT);
        docksplit(&l, DOCK_X, LAYOUT_LEFT);
        docksplit(&l, DOCK_Y, 1 - LAYOUT_TODO);
        dockline(&l, "Tabs", "Sessions");
        dockline(&l, "Tabs", "Todo");
        docksplit(&l, DOCK_Y, 1 - LAYOUT_PROMPT);
        dockline(&l, "Center", "Transcript");
        dockline(&l, "Tabs", "Prompt");
        docksplit(&l, DOCK_Y, 1 - LAYOUT_PLAN);
        dockline(&l, "Tabs", "*Jobs,Log,Help,Settings,Hosts");
        dockline(&l, "Tabs", "Plan,Changes");
        l.keep = extras;
        break;
    case PRESET_COMPACT:
        dockline(&l, "Shown", "Sessions,Transcript,Prompt,Todo,Plan,Jobs");
        docksplit(&l, DOCK_X, LAYOUT_LEFT*(1 - LAYOUT_RIGHT));  // as Standard's
        docksplit(&l, DOCK_Y, 1 - LAYOUT_TODO);
        dockline(&l, "Tabs", "Sessions");
        dockline(&l, "Tabs", "*Todo,Jobs,Log");
        docksplit(&l, DOCK_Y, 1 - LAYOUT_PROMPT);
        dockline(&l, "Center", "*Transcript,Plan,Changes,Help,Settings,Hosts");
        dockline(&l, "Tabs", "Prompt");
        l.keep = extras;
        break;
    case PRESET_FOCUS:
        dockline(&l, "Shown", "Transcript,Prompt,Todo,Plan");
        docksplit(&l, DOCK_Y, 1 - LAYOUT_PROMPT);
        dockline(&l, "Center", "*Transcript,Todo,Plan,Changes,Sessions,Jobs,Log,"
                               "Help,Settings,Hosts");
        dockline(&l, "Tabs", "Prompt");
        break;
    }
    return l;
}
