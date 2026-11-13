// Where a drop goes, apart from ImGui: one rule (dropwhere) for files
// dropped on the window and, as the drag goes, for the place it would
// go, with the words that say so.
//
// What the drop holds (DropLoad) is known as far as the platform tells:
// a drop always holds files, as gui_drop is handed them one at a time,
// while a drag may say no more than that something is there. The facts
// of the window (DropFacts) come from the last frame (dropfacts, in
// files.cpp). The rules, the first that matches deciding:
//   1. text, or anything else not files, is refused;
//   2. folders only start a New session, the last of them its folder,
//      wherever they are dropped;
//   3. with Open file shown, the first file fills its field;
//   4. with another modal up, files attach behind it;
//   5. at a point over the Files pane, files open there;
//   6. otherwise files attach to the next message.
// Files dropped together attach together or not at all: when they don't
// all fit, none do, and the words say why (amber). Folders dropped with
// files still start their New session, so that drop is never refused
// whole.
//
// As files are dragged over the window, the place a drop would go is
// outlined with a card that says so, which cardplace puts in the place,
// beside it, or by the pointer.
//
// This is free and unencumbered software released into the public domain.

// What a drag or a drop holds, as the platform knows it
enum : i32 {
    DRAGLOAD_UNKNOWN,  // something, no more known (Linux's drags)
    DRAGLOAD_FILES,    // files or folders
    DRAGLOAD_TEXT,
    DRAGLOAD_OTHER,
};

enum : i32 {
    DROPLOAD_SCAN = 16,  // items checked for a folder, at most
};

struct DropLoad {
    i32 kind;  // DRAGLOAD_*
    i32 n;     // items
    i32 dirs;  // folders among the first DROPLOAD_SCAN
};

// What a drag holds, from what a platform read of it: of items in all,
// the first scanned (at most DROPLOAD_SCAN) held files, dirs of them
// folders, and, of those not files, some held text. The items past the
// scan are taken as files when all the scanned were; items neither
// files nor text are left out where there are files, as the drop leaves
// them.
static DropLoad dropload(i64 items, i32 scanned, i32 files, i32 dirs, b32 text)
{
    DropLoad l = {DRAGLOAD_OTHER, 0, 0};
    if (files > 0) {
        i64 n = files==scanned ? items : files;
        l = {DRAGLOAD_FILES, n>0x7fffffff ? 0x7fffffff : (i32)n, dirs};
    } else if (text) {
        l.kind = DRAGLOAD_TEXT;
    }
    return l;
}

// Where a drop goes (DEST_* is help's)
enum : i32 {
    DROPTO_NONE,        // nowhere: refused
    DROPTO_FIELD,       // Open file's path field
    DROPTO_NEWSESSION,  // the New session dialog, in the folder
    DROPTO_FILES,       // the Files pane, a tab each
    DROPTO_ATTACH,      // the next message's attachments
    DROPTO_COUNT,
};

struct DropFacts {
    b32 at;         // at a point in the window, x and y known
    b32 askshown;   // Open file asks for a path
    b32 modal;      // another modal is up
    b32 overfiles;  // the point is over the Files pane
    Str attachwhy;  // why the session takes no attachment, if it doesn't
    i32 room;       // attachments the message has room for
    i32 attachmax;  // a message's attachments, at most
    i32 tabs;       // Files tabs a root keeps
};

struct DropWhere {
    i32 to;      // DROPTO_*
    Str label;   // what happens, said in a word or three
    Str detail;  // more of it, or why not
    b32 amber;   // the files would not go: shown as a warning
    b32 refuse;  // the platform should refuse the drop here
};

static Str droptoname(i32 to)
{
    static Str const names[] = {"none", "field", "newsession", "files", "attach"};
    static_assert(countof(names) == DROPTO_COUNT);
    return to>=0 && to<DROPTO_COUNT ? names[to] : Str{};
}

// Room for attachments in a message holding counted of at most max, its
// slots (counted or not, as path-only chips aren't) used of slots.
static i32 attachroom(i32 max, i32 counted, i32 used, i32 slots)
{
    i32 r = max - counted;
    r = slots-used<r ? slots-used : r;
    return r<0 ? 0 : r;
}

// "head N tail", the count left out (one, or not known) unless always
static Str dropcount(Arena *a, Str head, i32 n, Str tail, b32 always = 0)
{
    Buf b(a, head.len + tail.len + 16);
    print(&b, head);
    if (always || n>1) {
        putbyte(&b, ' ');
        print(&b, (i64)n);
    }
    print(&b, tail);
    return finish(&b);
}

static DropWhere dropwhere(Arena *a, DropFacts f, DropLoad l)
{
    DropWhere w = {};
    if (l.kind==DRAGLOAD_TEXT || l.kind==DRAGLOAD_OTHER) {
        w.to     = DROPTO_NONE;
        w.label  = "Only files can be dropped here";
        w.amber  = 1;
        w.refuse = 1;
        return w;
    }

    b32 known = l.kind == DRAGLOAD_FILES;
    i32 dirs  = known ? l.dirs : 0;
    i32 seen  = l.n<DROPLOAD_SCAN ? l.n : DROPLOAD_SCAN;
    if (known && l.n>0 && dirs>=seen) {
        w.to     = DROPTO_NEWSESSION;
        w.label  = "New session";
        w.detail = l.n==1 ? Str("in the folder dropped")
                          : dropcount(a, "in the last of", l.n, " folders");
        return w;
    }

    i32 n = known ? l.n - dirs : 0;  // files, 0 if not known
    if (f.askshown) {
        w.to     = DROPTO_FIELD;
        w.label  = "Fill in the path";
        w.detail = n>1 ? dropcount(a, "the first of", n, " files only") : Str{};
    } else if (!f.modal && f.at && f.overfiles) {
        w.to     = DROPTO_FILES;
        w.label  = dropcount(a, "Open", n, " in Files");
        w.detail = n>f.tabs ? dropcount(a, "only the last", f.tabs, " stay open")
                            : Str("to read, not attached");
    } else {
        w.to     = DROPTO_ATTACH;
        w.label  = dropcount(a, "Attach", n, "");
        w.detail = f.modal ? Str("to the next message, behind this dialog")
                           : Str("to the next message");
        Str why = f.attachwhy;
        if (!why.len && n>f.attachmax) {
            why = dropcount(a, "At most", f.attachmax, " files go in a message", 1);
        } else if (!why.len && n>f.room) {
            why = dropcount(a, "Room for", f.room, " more in this message", 1);
        }
        if (why.len) {
            w.label  = "Not attached";
            w.detail = why;
            w.amber  = 1;
            w.refuse = known && !dirs;
        }
    }

    if (dirs) {
        // The folders go too, whatever the files do
        Str tail = dirs==1 ? Str("and New session for the folder")
                           : Str("and New session for the last folder");
        w.detail = w.detail.len ? concat(a, concat(a, w.detail, ", "), tail) : tail;
    }
    return w;
}

// A box, x0 and y0 its top left, empty unless x1>x0 and y1>y0
struct DropBox {
    f32 x0, y0, x1, y1;
};

// Where a drag's card w by h goes, kept within work: centered in the
// place a drop would go if that is at least three cards tall (a pane,
// the window), else centered above it, margin clear, or below it where
// there is no room above (a field, the prompt); with no place, margin
// below and right of the pointer at px, py.
static DropBox cardplace(DropBox place, f32 w, f32 h, DropBox work, f32 px, f32 py,
                         f32 margin)
{
    b32 some = place.x1>place.x0 && place.y1>place.y0;
    f32 x    = px + margin;
    f32 y    = py + margin;
    if (some && place.y1-place.y0 >= 3*h) {
        x = (place.x0 + place.x1 - w)/2;
        y = (place.y0 + place.y1 - h)/2;
    } else if (some) {
        x = (place.x0 + place.x1 - w)/2;
        y = place.y0 - margin - h;
        y = y<work.y0 ? place.y1 + margin : y;
    }
    // The far edges first, so that a card wider than work starts at its
    // near edge
    x = x+w>work.x1 ? work.x1 - w : x;
    y = y+h>work.y1 ? work.y1 - h : y;
    x = x<work.x0 ? work.x0 : x;
    y = y<work.y0 ? work.y0 : y;
    return {x, y, x + w, y + h};
}
