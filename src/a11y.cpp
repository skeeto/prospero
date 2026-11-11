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

// Offsets into text count bytes of UTF-8 here. The OS counts UTF-16
// units (macOS, UIA) or code points (AT-SPI): see axtounit.
enum AxUnit : i32 {
    AXU_BYTE,
    AXU_UTF16,
    AXU_CP,
    AXU_N
};

// Where the focused field drew its text: lines are where each starts,
// for all of the text, and xs the x of each byte of the lines that show
// ([vis0, vis1)), from its start through its end, so one more than its
// length. Coordinates are the window's, the first line's top left at x,
// y, scrolled.
struct AxGeom {
    f32     x, y, lineh;
    AxRect  clip;
    i32    *lines;
    i32     nlines;
    i32     vis0, vis1;
    f32    *xs;
    i32     nxs;
    i64     rev;      // the text's revision it was drawn from
};

// The focused text field: its text, selection, and the text an input
// method is composing (marked), which stays out of the text: to the OS
// the document is the text with the marked text in place of the
// selection (axdoc).
struct AxText {
    b32     live;      // a field is focused
    u32     key;       // its node
    u32     id;        // its ImGuiID, which edits are for
    u32     state;     // AXS_READONLY, AXS_PASSWORD
    b32     multiline;
    i64     rev;       // bumped by every change to the text
    Str     text;
    i32     anchor, caret;
    Str     marked;
    i32     mlo, mhi;  // the selection within marked
    AxGeom  geom;
    i32    *units[AXU_N];   // per unit, the byte where each starts
    i32     nunits[AXU_N];  // and how many, built on demand
    i32    *nl;             // lines by newlines alone, when geom is stale
    i32     nnl;
};

// An edit queued for ImGui: bytes [lo, hi), which held expect, become
// text, then the selection is anchor to caret. With lo < 0 only the
// selection is set.
struct AxEdit {
    u32  id;
    i32  lo, hi;
    Str  expect;
    Str  text;
    i32  anchor, caret;
    i64  when;
};

enum : i64 {
    AXEDIT_TTL = 5000000,  // µs an edit waits for its field
};

// The arenas, each given whole: nothing else may use them.
enum : i32 {
    AXM_TREE0,  // the two trees, in turn
    AXM_TREE1,
    AXM_TEXT0,  // the focused field's text and offsets, in turn
    AXM_TEXT1,
    AXM_GEOM,   // its geometry, emptied each frame
    AXM_QUEUE,  // queued edits, emptied once all are taken
    AXM_N
};

struct A11y {
    Arena          mem[AXM_N];
    byte          *base[AXM_N];
    i32            cur;     // which tree arena the published tree is in
    AxTree        *pub;
    AxBuild        build;
    b32            building;
    i32            tcur;    // which text arena the text is in
    AxText         text;
    Slice<AxEdit>  queue;
    iz             qhead;
};

static A11y *newa11y(Arena *perm, Arena (&mem)[AXM_N])
{
    A11y *x = alloc<A11y>(perm);
    for (i32 i = 0; i < AXM_N; i++) {
        x->mem[i]  = mem[i];
        x->base[i] = x->mem[i].beg;
    }
    x->pub        = alloc<AxTree>(&x->mem[AXM_TREE0]);
    x->pub->focus = -1;
    x->tcur       = AXM_TEXT0;
    return x;
}

static Arena *axempty(A11y *x, i32 m)
{
    x->mem[m].beg = x->base[m];
    return &x->mem[m];
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
    AxBuild *b = &x->build;
    b->a = axempty(x, x->cur==AXM_TREE0 ? AXM_TREE1 : AXM_TREE0);
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
    x->cur = x->cur==AXM_TREE0 ? AXM_TREE1 : AXM_TREE0;
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


// Text: the focused field, as the OS reads and edits it
//
// The field's text is copied when it changes, into the text arena not
// in use, so the copy in use can build the new. An assistive tool's edit
// applies here at once, a shadow of what ImGui will do with it in the
// next frame, so the tool reads back what it wrote, and is queued with
// the bytes it replaces: ImGui refuses it if those changed, keys typed
// in between, say, and the frame after takes ImGui's text again. While
// an edit for the field waits, its text here is ahead of ImGui's, and
// its geometry is of the text before (stale): lines then come from
// newlines alone, and x positions are unknown.

struct AxSpan {
    i32 lo, hi;
};

static i32 axclamp(i32 v, i32 lo, i32 hi)
{
    return v<lo ? lo : v>hi ? hi : v;
}

static i32 axtextlen(A11y *x)
{
    return (i32)x->text.text.len;
}

static AxSpan axselection(AxText *t)
{
    AxSpan s = {};
    s.lo = t->anchor<t->caret ? t->anchor : t->caret;
    s.hi = t->anchor<t->caret ? t->caret  : t->anchor;
    return s;
}

static void axunits(A11y *x, AxUnit u)
{
    AxText *t = &x->text;
    if (u==AXU_BYTE || t->units[u]) {
        return;
    }
    Str  s = t->text;
    i32 *r = alloc<i32>(&x->mem[x->tcur], s.len+1);
    i32  n = 0;
    for (iz i = 0; i < s.len;) {
        i32 beg = (i32)i;
        i32 c   = utf8decode(s, &i);
        r[n++] = beg;
        if (u==AXU_UTF16 && c>0xffff) {
            r[n++] = beg;  // the low surrogate: the same code point
        }
    }
    r[n] = (i32)s.len;
    t->units[u]  = r;
    t->nunits[u] = n;
}

// A byte offset in units: within a code point, the code point's.
static i32 axtounit(A11y *x, AxUnit u, i32 b)
{
    b = axclamp(b, 0, axtextlen(x));
    if (u == AXU_BYTE) {
        return b;
    }
    axunits(x, u);
    i32 *r  = x->text.units[u];
    i32  lo = 0;
    i32  hi = x->text.nunits[u] + 1;
    while (lo < hi) {  // the first unit starting past b
        i32 mid = lo + (hi - lo)/2;
        if (r[mid] > b) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    i32 k = lo - 1;
    while (k>0 && r[k-1]==r[k]) {
        k--;
    }
    return k;
}

// Units as a byte offset: within a code point, its start.
static i32 axtobyte(A11y *x, AxUnit u, i32 off)
{
    if (u == AXU_BYTE) {
        u   = AXU_CP;
        off = axtounit(x, AXU_CP, off);
    }
    axunits(x, u);
    return x->text.units[u][axclamp(off, 0, x->text.nunits[u])];
}

static i32 axunitlen(A11y *x, AxUnit u)
{
    return axtounit(x, u, axtextlen(x));
}

// Units in s up to byte b, and the byte after n units, for marked text.
static i32 axcount(Str s, AxUnit u, i32 b)
{
    i32 n = 0;
    for (iz i = 0; i < b && i < s.len;) {
        i32 c = utf8decode(s, &i);
        n += u==AXU_BYTE ? 0 : u==AXU_UTF16 && c>0xffff ? 2 : 1;
    }
    return u==AXU_BYTE ? axclamp(b, 0, (i32)s.len) : n;
}

static i32 axadvance(Str s, AxUnit u, i32 n)
{
    iz i = 0;
    for (i32 k = 0; i < s.len;) {
        iz  beg = i;
        i32 c   = utf8decode(s, &i);
        k += u==AXU_BYTE ? (i32)(i - beg) : u==AXU_UTF16 && c>0xffff ? 2 : 1;
        if (k > n) {
            return (i32)beg;
        }
    }
    return (i32)s.len;
}

// The text arena not in use, emptied, for the next text.
static Arena *axspare(A11y *x)
{
    return axempty(x, x->tcur==AXM_TEXT0 ? AXM_TEXT1 : AXM_TEXT0);
}

// Make s, made in the spare arena, the text.
static void axtake(A11y *x, Str s)
{
    AxText *t = &x->text;
    x->tcur   = x->tcur==AXM_TEXT0 ? AXM_TEXT1 : AXM_TEXT0;
    t->text   = s;
    t->marked = clone(&x->mem[x->tcur], t->marked);
    t->rev++;
    for (i32 u = 0; u < AXU_N; u++) {
        t->units[u]  = 0;
        t->nunits[u] = 0;
    }
    t->nl  = 0;
    t->nnl = 0;
}

static void axsettext(A11y *x, Str s)
{
    axtake(x, clone(axspare(x), s));
}


// The document the OS sees: the text with the marked text, if any, in
// place of the selection. Offsets in it are in units.

static i32 axdoclen(A11y *x, AxUnit u)
{
    AxText *t = &x->text;
    if (!t->marked.len) {
        return axunitlen(x, u);
    }
    AxSpan s = axselection(t);
    return axunitlen(x, u) - (axtounit(x, u, s.hi) - axtounit(x, u, s.lo))
         + axcount(t->marked, u, (i32)t->marked.len);
}

// A byte offset in the document as units of it.
static i32 axdocunit(A11y *x, AxUnit u, i32 d)
{
    AxText *t = &x->text;
    if (!t->marked.len) {
        return axtounit(x, u, d);
    }
    AxSpan s  = axselection(t);
    i32    ml = (i32)t->marked.len;
    i32    u0 = axtounit(x, u, s.lo);
    if (d <= s.lo) {
        return axtounit(x, u, d);
    } else if (d < s.lo+ml) {
        return u0 + axcount(t->marked, u, d - s.lo);
    }
    return u0 + axcount(t->marked, u, ml)
         + axtounit(x, u, d - s.lo - ml + s.hi) - axtounit(x, u, s.hi);
}

// Units of the document as a byte offset in it.
static i32 axdocbyte(A11y *x, AxUnit u, i32 v)
{
    AxText *t = &x->text;
    if (!t->marked.len) {
        return axtobyte(x, u, v);
    }
    AxSpan s  = axselection(t);
    i32    ml = (i32)t->marked.len;
    i32    u0 = axtounit(x, u, s.lo);
    i32    m  = axcount(t->marked, u, ml);
    if (v <= u0) {
        return axtobyte(x, u, v);
    } else if (v < u0+m) {
        return s.lo + axadvance(t->marked, u, v - u0);
    }
    return s.lo + ml + axtobyte(x, u, v - u0 - m + axtounit(x, u, s.hi)) - s.hi;
}

// A byte offset in the document as one in the text: within the marked
// text, where it goes.
static i32 axdoctext(A11y *x, i32 d)
{
    AxText *t = &x->text;
    if (!t->marked.len) {
        return axclamp(d, 0, axtextlen(x));
    }
    AxSpan s  = axselection(t);
    i32    ml = (i32)t->marked.len;
    if (d <= s.lo) {
        return axclamp(d, 0, s.lo);
    } else if (d < s.lo+ml) {
        return s.lo;
    }
    return axclamp(d - s.lo - ml + s.hi, s.hi, axtextlen(x));
}

// The document itself, made on demand.
static Str axdoc(A11y *x, Arena *a)
{
    AxText *t = &x->text;
    if (!t->marked.len) {
        return t->text;
    }
    AxSpan s = axselection(t);
    Buf    b(a, t->text.len + t->marked.len);
    print(&b, takehead(t->text, s.lo));
    print(&b, t->marked);
    print(&b, cuthead(t->text, s.hi));
    return finish(&b);
}

// Set the text an input method composes, and its selection (bytes of
// it), in place of the selection; empty when it is done.
static void axmark(A11y *x, Str marked, i32 lo, i32 hi)
{
    AxText *t = &x->text;
    t->marked = clone(&x->mem[x->tcur], marked);
    t->mlo    = axclamp(lo, 0, (i32)marked.len);
    t->mhi    = axclamp(hi, t->mlo, (i32)marked.len);
}


// Lines, words, paragraphs: spans of bytes in the text

static i32 *axlines(A11y *x, i32 *n)
{
    AxText *t = &x->text;
    if (t->geom.lines && t->geom.rev==t->rev) {
        *n = t->geom.nlines;
        return t->geom.lines;
    }
    if (!t->nl) {
        i32 count = 1;
        for (iz i = 0; i < t->text.len; i++) {
            count += t->text[i] == '\n';
        }
        t->nl  = alloc<i32>(&x->mem[x->tcur], count);
        t->nnl = 0;
        t->nl[t->nnl++] = 0;
        for (iz i = 0; i < t->text.len; i++) {
            if (t->text[i] == '\n') {
                t->nl[t->nnl++] = (i32)i + 1;
            }
        }
    }
    *n = t->nnl;
    return t->nl;
}

static i32 axlinecount(A11y *x)
{
    i32 n;
    axlines(x, &n);
    return n;
}

// The line byte b is on.
static i32 axlineat(A11y *x, i32 b)
{
    i32  n;
    i32 *lines = axlines(x, &n);
    i32  lo = 0;
    i32  hi = n;
    while (lo < hi) {  // the first line starting past b
        i32 mid = lo + (hi - lo)/2;
        if (lines[mid] > b) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo>0 ? lo-1 : 0;
}

// Line k's bytes, its newline included.
static AxSpan axlinespan(A11y *x, i32 k)
{
    i32  n;
    i32 *lines = axlines(x, &n);
    k = axclamp(k, 0, n-1);
    AxSpan s = {};
    s.lo = lines[k];
    s.hi = k+1<n ? lines[k+1] : axtextlen(x);
    return s;
}

static i32 axclass(i32 c)
{
    if (c==' ' || c=='\t' || c=='\n' || c=='\r' || c==0xa0 || c==0x3000) {
        return 1;
    } else if (c>=0x80 || alnum((u8)c) || c=='_') {
        return 2;
    }
    return 3;
}

// The run of letters and digits, of spaces, or of punctuation that byte
// b starts or is within; at the end, the run before it.
static AxSpan axwordat(Str s, i32 b)
{
    b = axclamp(b, 0, (i32)s.len);
    if (b == s.len) {
        if (!b) {
            return {};
        }
        iz start;
        utf8prev(s, b, &start);
        b = (i32)start;
    }
    while (b>0 && (s[b]&0xc0)==0x80) {
        b--;
    }
    iz  i   = b;
    i32 cls = axclass(utf8decode(s, &i));
    AxSpan r = {b, (i32)i};
    while (r.hi < s.len) {
        iz j = r.hi;
        if (axclass(utf8decode(s, &j)) != cls) {
            break;
        }
        r.hi = (i32)j;
    }
    while (r.lo > 0) {
        iz  start;
        i32 c = utf8prev(s, r.lo, &start);
        if (axclass(c) != cls) {
            break;
        }
        r.lo = (i32)start;
    }
    return r;
}

// The paragraph byte b is in, through its newline.
static AxSpan axparaat(Str s, i32 b)
{
    b = axclamp(b, 0, (i32)s.len);
    AxSpan r = {b, b};
    while (r.lo>0 && s[r.lo-1]!='\n') {
        r.lo--;
    }
    while (r.hi<s.len && s[r.hi]!='\n') {
        r.hi++;
    }
    r.hi += r.hi < s.len;
    return r;
}


// Geometry: where the text's bytes are, in the window

// x of byte b, on line k, from the line's start, or -1 when unknown
static f32 axxof(A11y *x, i32 k, i32 b)
{
    AxText *t = &x->text;
    AxGeom *g = &t->geom;
    if (g->rev!=t->rev || !g->xs || k<g->vis0 || k>=g->vis1) {
        return -1;
    }
    i32 at = 0;
    for (i32 j = g->vis0; j < k; j++) {
        AxSpan s = axlinespan(x, j);
        at += s.hi - s.lo + 1;
    }
    AxSpan s = axlinespan(x, k);
    i32    i = at + axclamp(b, s.lo, s.hi) - s.lo;
    return i<g->nxs ? g->xs[i] : -1;
}

// Where byte b's character is: as wide as it, or none at a line's end.
static AxRect axcharrect(A11y *x, i32 b)
{
    AxGeom *g = &x->text.geom;
    b = axtobyte(x, AXU_BYTE, b);
    i32    k = axlineat(x, b);
    AxSpan s = axlinespan(x, k);
    iz     e = b;
    if (b < s.hi) {
        utf8decode(x->text.text, &e);
    }
    f32 x0 = axxof(x, k, b);
    f32 x1 = axxof(x, k, (i32)e);
    x0 = x0<0 ? 0  : x0;
    x1 = x1<0 ? x0 : x1;
    AxRect r = {};
    r.x0 = g->x + x0;
    r.x1 = g->x + x1;
    r.y0 = g->y + (f32)k*g->lineh;
    r.y1 = r.y0 + g->lineh;
    return r;
}

// The byte offset nearest a point: before the character it is left of
// the middle of, and never past a line's newline.
static i32 axoffsetat(A11y *x, f32 px, f32 py)
{
    AxGeom *g = &x->text.geom;
    Str     s = x->text.text;
    i32     k = 0;
    if (g->lineh > 0) {
        k = (i32)__builtin_floorf((py - g->y) / g->lineh);
    }
    k = axclamp(k, 0, axlinecount(x)-1);
    AxSpan line = axlinespan(x, k);
    i32    end  = line.hi>line.lo && s[line.hi-1]=='\n' ? line.hi-1 : line.hi;
    if (axxof(x, k, line.lo) < 0) {
        return line.lo;
    }
    for (iz i = line.lo; i < end;) {
        iz beg = i;
        utf8decode(s, &i);
        f32 mid = (axxof(x, k, (i32)beg) + axxof(x, k, (i32)i)) / 2;
        if (px - g->x < mid) {
            return (i32)beg;
        }
    }
    return end;
}


// Keeping up with ImGui

static b32 axqueued(A11y *x, u32 id)
{
    for (iz i = x->qhead; i < x->queue.len; i++) {
        if (x->queue[i].id == id) {
            return 1;
        }
    }
    return 0;
}

// The focused field as drawn this frame: its text, selection and where
// it drew them. With an edit for it queued, its text and selection here
// are ahead of ImGui's, and stay.
static void axtextsync(A11y *x, u32 key, u32 id, u32 state, b32 multiline,
                       Str text, i32 anchor, i32 caret, AxGeom *g)
{
    AxText *t     = &x->text;
    b32     other = !t->live || t->key!=key || t->id!=id;
    if (other) {
        t->marked = {};
        t->mlo = t->mhi = 0;
    }
    t->live      = 1;
    t->key       = key;
    t->id        = id;
    t->state     = state;
    t->multiline = multiline;
    if (other || !axqueued(x, id)) {
        if (other || !(t->text == text)) {
            axsettext(x, text);
        }
        t->anchor = axclamp(anchor, 0, axtextlen(x));
        t->caret  = axclamp(caret,  0, axtextlen(x));
    }

    Arena *a = axempty(x, AXM_GEOM);
    t->geom  = *g;
    t->geom.lines = alloc<i32>(a, g->nlines);
    for (i32 i = 0; i < g->nlines; i++) {
        t->geom.lines[i] = g->lines[i];
    }
    t->geom.xs = alloc<f32>(a, g->nxs);
    for (i32 i = 0; i < g->nxs; i++) {
        t->geom.xs[i] = g->xs[i];
    }
    t->geom.rev = t->text==text ? t->rev : -1;
}

// No field is focused.
static void axtextnone(A11y *x)
{
    AxText *t = &x->text;
    t->live   = 0;
    t->marked = {};
    t->mlo = t->mhi = 0;
    t->geom   = {};
    t->geom.rev = -1;
}

// Text as a field takes it: CRLF and CR as LF, tabs as spaces, other
// control characters dropped, and newlines too in a field of one line.
static Str axclean(Arena *a, Str s, b32 multiline)
{
    Buf b(a, s.len);
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c == '\r') {
            c = '\n';
            i += i+1<s.len && s[i+1]=='\n';
        }
        c = c=='\t' ? ' ' : c;
        if ((c=='\n' && !multiline) || (c<0x20 && c!='\n') || c==0x7f) {
            continue;
        }
        putbyte(&b, c);
    }
    return finish(&b);
}

enum : i32 {
    AX_AFTER = -1,  // a selection at the end of the text put in
};

static b32 axeditable(AxText *t)
{
    return t->live && !(t->state & (AXS_READONLY|AXS_PASSWORD));
}

// Replace bytes [lo, hi) of the focused field's text, then select from
// anchor to caret, both in the text after, or AX_AFTER. Refused (0) for
// a field not editable, or a range not in its text.
static b32 axedit(A11y *x, i32 lo, i32 hi, Str text, i32 anchor, i32 caret, i64 now)
{
    AxText *t   = &x->text;
    i32     len = axtextlen(x);
    if (!axeditable(t) || lo<0 || hi<lo || hi>len) {
        return 0;
    }
    lo = axtobyte(x, AXU_BYTE, lo);
    hi = axtobyte(x, AXU_BYTE, hi);

    Arena *q = &x->mem[AXM_QUEUE];
    AxEdit e = {};
    e.id     = t->id;
    e.lo     = lo;
    e.hi     = hi;
    e.when   = now;
    e.expect = clone(q, span(t->text.data+lo, t->text.data+hi));
    e.text   = axclean(q, text, t->multiline);

    i32  raw   = (i32)text.len;
    i32  add   = (i32)e.text.len;
    i32  after = len - (hi - lo) + add;
    auto fix   = [&](i32 p) {
        p = p==AX_AFTER ? lo+add : p>lo+raw ? p-raw+add : p>lo+add ? lo+add : p;
        return axclamp(p, 0, after);
    };
    e.anchor = fix(anchor);
    e.caret  = fix(caret);
    x->queue = push(q, x->queue, e);

    Buf b(axspare(x), after);
    print(&b, takehead(t->text, lo));
    print(&b, e.text);
    print(&b, cuthead(t->text, hi));
    axtake(x, finish(&b));
    t->anchor = e.anchor;
    t->caret  = e.caret;
    return 1;
}

// Select from anchor to caret in the focused field.
static b32 axselect(A11y *x, i32 anchor, i32 caret, i64 now)
{
    AxText *t = &x->text;
    if (!axeditable(t)) {
        return 0;
    }
    AxEdit e = {};
    e.id     = t->id;
    e.lo     = -1;
    e.when   = now;
    e.anchor = axtobyte(x, AXU_BYTE, anchor);
    e.caret  = axtobyte(x, AXU_BYTE, caret);
    x->queue = push(&x->mem[AXM_QUEUE], x->queue, e);
    t->anchor = e.anchor;
    t->caret  = e.caret;
    return 1;
}

// The next edit for field id, whose text is now text, for ImGui to
// apply. An edit whose bytes changed under it is dropped, with those
// after it for the field, which counted on it; one waiting longer than
// AXEDIT_TTL for its field is dropped.
static b32 axnextedit(A11y *x, u32 id, Str text, i64 now, AxEdit *out)
{
    b32 r = 0;
    for (iz i = x->qhead; i < x->queue.len; i++) {
        AxEdit *e = &x->queue[i];
        if (e->id && now-e->when>AXEDIT_TTL) {
            e->id = 0;
        }
        if (!e->id || e->id!=id || r) {
            continue;
        }
        e->id = 0;
        if (e->lo>=0 && (e->hi>text.len ||
                !(span(text.data+e->lo, text.data+e->hi) == e->expect))) {
            for (iz j = i+1; j < x->queue.len; j++) {
                x->queue[j].id = x->queue[j].id==id ? 0 : x->queue[j].id;
            }
            break;
        }
        *out = *e;
        r    = 1;
    }
    while (x->qhead<x->queue.len && !x->queue[x->qhead].id) {
        x->qhead++;
    }
    if (x->qhead == x->queue.len) {
        axempty(x, AXM_QUEUE);
        x->queue = {};
        x->qhead = 0;
    }
    return r;
}
