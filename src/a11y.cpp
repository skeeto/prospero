// Accessibility: the tree the OS's assistive tools see
//
// Each frame, while an assistive tool is listening, a11y_imgui.cpp
// builds a tree of what the window shows: its windows and panes, and the
// items in them, each with a role, a name, state and a rectangle. Here
// is the tree itself, which knows nothing of ImGui, so the adapters for
// each OS and the unit tests share it.
//
// Two trees are kept, in two arenas: the one published, which answers
// the OS's queries between frames, and the one being built. Publishing
// links the new tree, compares it with the old, swaps the two, and
// returns what changed as events, for the adapters to post. A node is
// known across frames by its key, an item's ImGuiID, so an element the
// OS holds finds its node again, or learns it is gone.
//
// Names come from labels with ImGui's "##" suffix and icon glyphs taken
// out, or, for a button that shows only icons, from the icons' names.
// Lint finds what a tool could act on, on screen, with no name.
//
// This is free and unencumbered software released into the public domain.

enum AxRole : u8 {
    AX_NONE,       // hidden from assistive tools
    AX_APP,        // the root
    AX_WINDOW,     // a pane or a floating window
    AX_DIALOG,     // a modal
    AX_POPUP,
    AX_MENUBAR,
    AX_MENU,
    AX_MENUITEM,
    AX_GROUP,
    AX_BUTTON,
    AX_CHECKBOX,
    AX_RADIO,
    AX_LINK,
    AX_TABLIST,
    AX_TAB,
    AX_TREEITEM,   // a collapsing header or tree node
    AX_LISTITEM,   // a selectable
    AX_COMBO,
    AX_SLIDER,
    AX_TEXTFIELD,  // one line
    AX_TEXTAREA,   // many lines
    AX_TEXT,       // static
    AX_IMAGE,
    AX_NROLES
};

static Str const axrolenames[AX_NROLES] = {
    "none", "app", "window", "dialog", "popup", "menubar", "menu",
    "menuitem", "group", "button", "checkbox", "radio", "link", "tablist",
    "tab", "treeitem", "listitem", "combo", "slider", "textfield",
    "textarea", "text", "image",
};

enum : u32 {
    AXS_FOCUSED   = 1u << 0,
    AXS_DISABLED  = 1u << 1,
    AXS_CHECKED   = 1u << 2,
    AXS_EXPANDED  = 1u << 3,
    AXS_SELECTED  = 1u << 4,
    AXS_OFFSCREEN = 1u << 5,   // clipped away: no label known
    AXS_READONLY  = 1u << 6,
    AXS_PASSWORD  = 1u << 7,
    AXS_MODAL     = 1u << 8,
    AXS_NAMES     = 9,

    // Changes to these are state events
    AXS_EVENTFUL = AXS_DISABLED|AXS_CHECKED|AXS_EXPANDED|AXS_SELECTED|AXS_READONLY,
};

static Str const axstatenames[AXS_NAMES] = {
    "focused", "disabled", "checked", "expanded", "selected", "offscreen",
    "readonly", "password", "modal",
};

enum : u32 {
    AXA_PRESS  = 1u << 0,
    AXA_TOGGLE = 1u << 1,
    AXA_EXPAND = 1u << 2,   // and collapse
    AXA_FOCUS  = 1u << 3,
    AXA_EDIT   = 1u << 4,
    AXA_SELECT = 1u << 5,

    // What lint expects a name on
    AXA_NAMED = AXA_PRESS|AXA_TOGGLE|AXA_EXPAND|AXA_EDIT|AXA_SELECT,
};

enum : u32 {
    AXKEY_ROOT = 1,
};

struct AxRect {
    f32 x0, y0, x1, y1;  // the window's client coordinates, unscaled
};

static b32 operator==(AxRect a, AxRect b)
{
    return a.x0==b.x0 && a.y0==b.y0 && a.x1==b.x1 && a.y1==b.y1;
}

struct AxNode {
    u32    key;
    u32    parentkey;     // as built; resolved into parent on publish
    i32    parent;        // index, -1 for the root
    i32    child;         // first child, or -1
    i32    next;          // next sibling, or -1
    i32    nchild;
    AxRole role;
    u32    state;
    u32    actions;
    Str    name;
    Str    help;          // a tooltip's text
    Str    value;
    AxRect rect;
    u64    kids;          // hash of the children's keys, in order
};

enum AxEventKind : u8 {
    AXE_DESTROYED,  // the OS's element for this key must go
    AXE_CREATED,
    AXE_CHILDREN,   // this node's children changed
    AXE_NAME,
    AXE_VALUE,
    AXE_STATE,
    AXE_FOCUS,      // key is the focus now, or 0 for none
    AXE_LAYOUT,     // something moved, came or went (key 0)
};

struct AxEvent {
    u32         key;
    AxEventKind kind;
};

struct AxTree {
    AxNode         *nodes;
    i32             len;
    i32            *index;   // open addressing by key: node index or -1
    i32             exp;     // the index holds 1<<exp slots
    i32             focus;   // node index, or -1
    i32             dups;    // nodes whose key an earlier node had
    i64             rev;
    Slice<AxEvent>  events;  // what changed since the tree before
};

struct AxBuild {
    Arena          *a;
    Slice<AxNode>   nodes;
};

struct A11y {
    Arena    mem[2];
    byte    *base[2];
    i32      cur;     // which arena the published tree lives in
    AxTree  *pub;
    AxBuild  build;
    b32      building;
};

// The trees' two arenas are given whole: nothing else may use them.
static A11y *newa11y(Arena *perm, Arena m0, Arena m1)
{
    A11y *x = alloc<A11y>(perm);
    x->mem[0]  = m0;
    x->mem[1]  = m1;
    x->base[0] = x->mem[0].beg;
    x->base[1] = x->mem[1].beg;
    x->pub     = alloc<AxTree>(&x->mem[0]);
    x->pub->focus = -1;
    return x;
}

static u64 axhash(u32 key, i32 exp)
{
    return ((u64)key * 0x9e3779b97f4a7c15u) >> (64 - exp);
}

// The node with this key, or -1.
static i32 axfind(AxTree *t, u32 key)
{
    if (!key || !t->index) {
        return -1;
    }
    u64 mask = ((u64)1 << t->exp) - 1;
    for (u64 h = axhash(key, t->exp);; h = (h+1) & mask) {
        i32 i = t->index[h];
        if (i<0 || t->nodes[i].key==key) {
            return i;
        }
    }
}

// Start a new tree, its root the app. Nodes and their strings go in the
// arena the published tree isn't in, emptied first.
static AxBuild *axbegin(A11y *x)
{
    i32 n = !x->cur;
    x->mem[n].beg = x->base[n];
    AxBuild *b = &x->build;
    b->a = &x->mem[n];
    b->nodes = {};
    i32 cap  = x->pub->len>128 ? x->pub->len*2 : 256;
    b->nodes.data = alloc<AxNode>(b->a, cap);
    b->nodes.cap  = cap;
    x->building = 1;

    AxNode root = {};
    root.key  = AXKEY_ROOT;
    root.role = AX_APP;
    b->nodes = push(b->a, b->nodes, root);
    return b;
}

// Add a node under parent (a key; the root with 0, or one not added by
// publish time). The strings are copied. The node returned may be
// filled in until the next is added.
static AxNode *axadd(AxBuild *b, u32 parent, u32 key, AxRole role, Str name)
{
    AxNode n = {};
    n.key       = key;
    n.parentkey = parent ? parent : AXKEY_ROOT;
    n.role      = role;
    n.name      = clone(b->a, name);
    b->nodes = push(b->a, b->nodes, n);
    return &b->nodes[b->nodes.len-1];
}

static void axsethelp(AxBuild *b, AxNode *n, Str help)
{
    n->help = clone(b->a, help);
}

static void axsetvalue(AxBuild *b, AxNode *n, Str value)
{
    n->value = clone(b->a, value);
}

// Link a built tree: index its keys, resolve parents, and thread the
// children of each node in the order added. A parent not found, or one
// that would make a cycle, leaves its node under the root.
static void axlink(AxTree *t, Arena *a)
{
    i32 exp = 4;
    while (((i64)1 << exp) < (i64)t->len*2) {
        exp++;
    }
    t->exp   = exp;
    t->index = alloc<i32>(a, (iz)1 << exp);
    u64 mask = ((u64)1 << exp) - 1;
    for (u64 h = 0; h <= mask; h++) {
        t->index[h] = -1;
    }
    for (i32 i = 0; i < t->len; i++) {
        u32 key = t->nodes[i].key;
        if (!key) {
            continue;
        }
        for (u64 h = axhash(key, exp);; h = (h+1) & mask) {
            i32 j = t->index[h];
            if (j < 0) {
                t->index[h] = i;
                break;
            } else if (t->nodes[j].key == key) {
                t->dups++;
                break;
            }
        }
    }

    for (i32 i = 0; i < t->len; i++) {
        AxNode *n = t->nodes + i;
        n->child = n->next = -1;
        n->parent = i ? axfind(t, n->parentkey) : -1;
        n->parent = n->parent==i ? 0 : n->parent;
        if (i && n->parent<0) {
            n->parent = 0;
        }
    }
    for (i32 i = 1; i < t->len; i++) {
        // A walk up longer than the tree is a cycle
        i32 p = t->nodes[i].parent;
        for (i32 steps = 0; p > 0; steps++) {
            if (steps == t->len) {
                t->nodes[i].parent = 0;
                break;
            }
            p = t->nodes[p].parent;
        }
    }

    i32 *last = alloc<i32>(a, t->len);
    for (i32 i = 0; i < t->len; i++) {
        last[i] = -1;
    }
    for (i32 i = 1; i < t->len; i++) {
        AxNode *n = t->nodes + i;
        AxNode *p = t->nodes + n->parent;
        if (last[n->parent] < 0) {
            p->child = i;
        } else {
            t->nodes[last[n->parent]].next = i;
        }
        last[n->parent] = i;
        p->nchild++;
        p->kids ^= n->key;
        p->kids *= 1111111111111111111u;
    }
}

static void axevent(Arena *a, AxTree *t, AxEventKind kind, u32 key)
{
    AxEvent e = {};
    e.key  = key;
    e.kind = kind;
    t->events = push(a, t->events, e);
}

// What changed from old to t, in t's events: those destroyed first,
// so the adapters let go of them, then those created, then changes.
static void axdiff(AxTree *old, AxTree *t, Arena *a)
{
    b32 layout = 0;
    for (i32 i = 1; i < old->len; i++) {
        u32 key = old->nodes[i].key;
        if (key && axfind(old, key)==i && axfind(t, key)<0) {
            axevent(a, t, AXE_DESTROYED, key);
            layout = 1;
        }
    }
    for (i32 i = 1; i < t->len; i++) {
        u32 key = t->nodes[i].key;
        if (key && axfind(t, key)==i && axfind(old, key)<0) {
            axevent(a, t, AXE_CREATED, key);
            layout = 1;
        }
    }
    for (i32 i = 0; i < t->len; i++) {
        AxNode *n = t->nodes + i;
        i32     j = axfind(old, n->key);
        if (j<0 || axfind(t, n->key)!=i) {
            continue;
        }
        AxNode *o = old->nodes + j;
        if (o->kids != n->kids) {
            axevent(a, t, AXE_CHILDREN, n->key);
        }
        if (!(o->name == n->name)) {
            axevent(a, t, AXE_NAME, n->key);
        }
        if (!(o->value == n->value)) {
            axevent(a, t, AXE_VALUE, n->key);
        }
        if ((o->state ^ n->state) & AXS_EVENTFUL) {
            axevent(a, t, AXE_STATE, n->key);
        }
        if (!(o->rect == n->rect) || ((o->state ^ n->state) & AXS_OFFSCREEN)) {
            layout = 1;
        }
    }
    u32 was = old->focus<0 ? 0 : old->nodes[old->focus].key;
    u32 now = t->focus<0   ? 0 : t->nodes[t->focus].key;
    if (was != now) {
        axevent(a, t, AXE_FOCUS, now);
    }
    if (layout) {
        axevent(a, t, AXE_LAYOUT, 0);
    }
}

// Publish the tree built since axbegin, focus on the node with focuskey
// (0 for none), and return what changed. The tree before it is gone:
// nothing from it may be held past this call.
static Slice<AxEvent> axpublish(A11y *x, u32 focuskey)
{
    assert(x->building);
    x->building = 0;
    AxBuild *b = &x->build;
    AxTree  *t = alloc<AxTree>(b->a);
    t->nodes = b->nodes.data;
    t->len   = (i32)b->nodes.len;
    t->rev   = x->pub->rev + 1;
    axlink(t, b->a);
    t->focus = axfind(t, focuskey);
    if (t->focus >= 0) {
        t->nodes[t->focus].state |= AXS_FOCUSED;
    }
    axdiff(x->pub, t, b->a);
    x->pub = t;
    x->cur = !x->cur;
    return t->events;
}

// The nodes a tool could act on, on screen, with no name.
static Slice<i32> axlint(Arena *a, AxTree *t)
{
    Slice<i32> r = {};
    for (i32 i = 1; i < t->len; i++) {
        AxNode *n = t->nodes + i;
        if (n->role!=AX_NONE && (n->actions & AXA_NAMED) &&
                !(n->state & AXS_OFFSCREEN) && !n->name.len) {
            r = push(a, r, i);
        }
    }
    return r;
}


// Names

// The icons buttons show (fonts.cpp), named for a tool to say
static struct { Str glyph, name; } const axicons[] = {
    {ICON_ADD,        "Add"},
    {ICON_TRASH,      "Delete"},
    {ICON_HISTORY,    "History"},
    {ICON_CHECK,      "Check"},
    {ICON_DISCARD,    "Discard"},
    {ICON_EXTERNAL,   "Open externally"},
    {ICON_LIGHTBULB,  "Tip"},
    {ICON_INFO,       "Info"},
    {ICON_CLOSE,      "Close"},
    {ICON_FILE,       "File"},
    {ICON_REPLY,      "Reply"},
    {ICON_TERMINAL,   "Terminal"},
    {ICON_DISCONNECT, "Disconnect"},
    {ICON_STOP,       "Stop"},
    {ICON_FOLDER,     "Folder"},
    {ICON_PLUG,       "Plug"},
    {ICON_REFRESH,    "Refresh"},
    {ICON_TOOLS,      "Tools"},
    {ICON_SHELL,      "Shell"},
    {ICON_COPY,       "Copy"},
    {ICON_SEND,       "Send"},
    {ICON_ATTACH,     "Attach"},
    {ICON_UP,         "Up"},
    {ICON_SESSION,    "Sessions"},
    {ICON_SORT,       "Sort"},
    {ICON_ARCHIVE,    "Archive"},
    {ICON_UNARCHIVE,  "Unarchive"},
    {ICON_GRIPPER,    "Drag handle"},
    {ICON_NEWFOLDER,  "New folder"},
    {ICON_TREE,       "Tree"},
};

// The name of an icon's code point, or empty for one unknown.
static Str axiconname(i32 c)
{
    for (iz i = 0; i < countof(axicons); i++) {
        iz at = 0;
        if (utf8decode(axicons[i].glyph, &at) == c) {
            return axicons[i].name;
        }
    }
    return {};
}

static b32 axprivate(i32 c)
{
    return c>=0xe000 && c<=0xf8ff;
}

// A label as a name: what shows of it, before ImGui's "##", without
// icons (private-use code points), trimmed. A label that is only icons
// is named by its icons, joined by spaces.
static Str axlabel(Arena *a, Str label)
{
    iz hide = find(label, "##");
    label = hide<0 ? label : takehead(label, hide);

    b32 icons = 0;
    b32 text  = 0;
    for (iz i = 0; i < label.len;) {
        i32 c = utf8decode(label, &i);
        icons |= axprivate(c);
        text  |= !axprivate(c) && !whitespace((u8)(c<0x80 ? c : 'x'));
    }
    if (!icons) {
        return trim(label);
    }

    Buf b(a, label.len+16);
    for (iz i = 0; i < label.len;) {
        iz  beg = i;
        i32 c   = utf8decode(label, &i);
        if (!axprivate(c)) {
            if (text) {
                print(&b, span(label.data+beg, label.data+i));
            }
        } else if (!text) {
            Str name = axiconname(c);
            if (name.len) {
                if (b.len) {
                    putbyte(&b, ' ');
                }
                print(&b, name);
            }
        }
    }
    Str r = trim(finish(&b));
    // Spaces where an icon was within the text come down to one
    Buf s(a, r.len);
    for (iz i = 0; i < r.len; i++) {
        if (r[i]!=' ' || !s.len || s.data[s.len-1]!=' ') {
            putbyte(&s, r[i]);
        }
    }
    return finish(&s);
}


// A dump of the tree as text, one node a line, for tests and the GUI
// checklist: role, "name", = "value", ?"help", [state], @x,y wxh.

static void axquote(Buf *b, Str s)
{
    putbyte(b, '"');
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c == '\n') {
            print(b, "\\n");
        } else if (c=='"' || c=='\\') {
            putbyte(b, '\\');
            putbyte(b, c);
        } else if (c < 0x20) {
            print(b, "\\x");
            printhex(b, c, 2);
        } else {
            putbyte(b, c);
        }
    }
    putbyte(b, '"');
}

static i64 axround(f32 v)
{
    return (i64)__builtin_floorf(v + 0.5f);
}

static void axdumpnode(Buf *b, AxTree *t, i32 i, i32 depth, b32 rects)
{
    AxNode *n = t->nodes + i;
    for (i32 d = 0; d < depth; d++) {
        print(b, "  ");
    }
    print(b, axrolenames[n->role<AX_NROLES ? n->role : 0]);
    if (n->name.len) {
        putbyte(b, ' ');
        axquote(b, n->name);
    }
    if (n->value.len) {
        print(b, " = ");
        axquote(b, n->value);
    }
    if (n->help.len) {
        print(b, " ?");
        axquote(b, n->help);
    }
    if (n->state) {
        print(b, " [");
        b32 first = 1;
        for (u32 s = 0; s < AXS_NAMES; s++) {
            if (n->state & (1u << s)) {
                if (!first) {
                    putbyte(b, ' ');
                }
                print(b, axstatenames[s]);
                first = 0;
            }
        }
        putbyte(b, ']');
    }
    if (rects && i) {
        print(b, " @");
        print(b, axround(n->rect.x0));
        putbyte(b, ',');
        print(b, axround(n->rect.y0));
        putbyte(b, ' ');
        print(b, axround(n->rect.x1 - n->rect.x0));
        putbyte(b, 'x');
        print(b, axround(n->rect.y1 - n->rect.y0));
    }
    putbyte(b, '\n');
}

static void axdump(Buf *b, AxTree *t, b32 rects)
{
    if (!t->len) {
        return;
    }
    // Depth first, without recursion: up through parents for the next
    i32 depth = 0;
    for (i32 i = 0; i >= 0;) {
        axdumpnode(b, t, i, depth, rects);
        AxNode *n = t->nodes + i;
        if (n->child >= 0) {
            i = n->child;
            depth++;
            continue;
        }
        for (;;) {
            if (t->nodes[i].next >= 0) {
                i = t->nodes[i].next;
                break;
            }
            i = t->nodes[i].parent;
            depth--;
            if (i < 0) {
                break;
            }
        }
    }
}
