// Sessions sidebar: the tree it shows, as rows to draw
//
// The hierarchy is Group › Host › Session › Subagent. Groups come
// first, in the GUI's order (which keeps empty ones too), each merged by
// name across hosts; then the sessions in no group, host by host; then
// an Archived heading over the archived sessions; then a Trash heading
// over those in the Trash, shown only with sessions in it. A subagent
// belongs where its root is: the root's group, archiving and trashing
// are its own, and a root in the Trash is in no group and not archived.
// Host headings show only when more than one host could be shown
// (heads), under each group and the Archived and Trash headings too,
// where that host has sessions; at the top level every host listed always shows, empty or
// not. Within a host, roots come as given, newest made first, or with
// byat by the latest time (at) in each one's subtree, most recent first,
// ties as given; subagents come oldest first under their parents. A
// root's time counts only its own host's sessions, whose clock it is:
// times are never compared across hosts. A subagent on another
// host than its parent's (phost) goes under that parent, in its
// section and host heading, marked away.
//
// With byfolder (the Folders view), the groups are not shown: under
// each host, or at the top with no host headings, the live roots go
// under a heading for each project folder (SideSess.folder), those of no
// folder under one of their own, the headings in the order of their
// first roots, so that a folder ranks by its newest session as the
// roots are sorted. Archived and the Trash are as without it. A
// heading's label is the folder's last segment, with as many before it
// as tell it apart from the host's other folders (sidelabel).
//
// Every heading folds, and so does a session with subagents. The user's
// choices are kept by key (folds: 1 folded, 0 open); without one, the
// Archived and Trash headings are folded and everything else open. A session stays
// open while anything below it runs a turn or asks, and a heading while
// anything below it asks, so that nothing waiting for the user is ever
// hidden. A folded row's sessions are left out of the rows.
//
// A subagent cannot be resumed, so one whose task is over is done for
// good. A session with SIDE_DONEMIN or more such subagents lists the
// rest, those at work, asking, or in view (and those above them), then
// a "finished" row over the done ones, folded unless opened, so that
// dozens of done subagents do not bury the few still at work.
//
// Fold keys:
//   g:NAME            a group
//   h:HOST            a host's heading at the top level
//   g:NAME|h:HOST     a host's heading under a group
//   a                 the Archived heading
//   a|h:HOST          a host's heading under it
//   t                 the Trash heading
//   t|h:HOST          a host's heading under it
//   s:HOST:SID        a session with subagents
//   d:HOST:SID        its finished subagents
//   f:HOST:PATH       a folder (byfolder), PATH empty for no folder
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    SROW_GROUP,
    SROW_HOST,
    SROW_SESSION,
    SROW_ARCHIVED,  // the Archived heading
    SROW_TRASH,     // the Trash heading
    SROW_DONE,      // a session's finished subagents
    SROW_FOLDER,    // a project folder (byfolder)
};

// The sections a root can be in (sidein).
enum : i32 {
    SIDE_LIVE,      // a group's, or in none
    SIDE_ARCHIVED,
    SIDE_TRASH,
};

enum : i32 {
    SIDE_DEPTH   = 16,  // levels of subagents placed under their parents
    SIDE_DONEMIN = 3,   // finished subagents that fold into a row of their own
};

// A session as the sidebar sees it.
struct SideSess {
    i32  host;      // index into the hosts
    Str  sid;
    Str  parent;    // a subagent's, else empty
    i32  phost;     // that parent's host index + 1, if another host's
    Str  group;     // its group, if any: only a root's counts
    Str  folder;    // its project's folder, empty for none: only a root's
                    // counts
    b32  archived;  // only a root's counts
    b32  trashed;   // in the Trash: only a root's counts
    b32  active;    // runs a turn
    b32  asks;      // waits for the user
    b32  shown;     // the session in view
    i64  at;        // its time to sort by (byat), on its host's clock
};

struct SideHost {
    Str key;        // stable across runs, for its fold state
    b32 always;     // listed with no sessions to show
    i32 under;      // its heading's host index + 1, if under another's
                    // (the containers under their runtime), else 0
};

struct SideIn {
    Slice<SideSess> sess;    // each host's newest first
    b32             byat;    // roots by their subtrees' latest at
    b32             byfolder;// live roots under folder headings, no groups
    Slice<SideHost> hosts;   // in order
    Slice<Str>      groups;  // in order, empty ones included
    Map<b32>       *folds;   // the user's choices: 1 folded, 0 open
    b32             heads;   // host headings shown
    b32             archive; // the Archived heading shown even if empty,
                             // as a place to drop a session
};

struct SideRow {
    i32 kind;     // SROW_*
    i32 depth;    // levels of indentation
    i32 host;     // SROW_HOST and SROW_SESSION: index into the hosts
    i32 sess;     // SROW_SESSION: index into the sessions, else -1
    Str group;    // SROW_GROUP: its name; else the group it is in, if any
    Str folder;   // SROW_FOLDER: its path, empty for no folder
    Str label;    // SROW_FOLDER: its name to show (sidelabel)
    b32 archived; // under the Archived heading, or it
    b32 trashed;  // under the Trash heading, or it
    Str key;      // its fold key, empty if it does not fold
    b32 folded;   // its rows below are left out
    b32 forced;   // open whatever the user chose
    b32 kids;     // a session with subagents
    b32 away;     // SROW_SESSION: on another host than its root
    i32 count;    // sessions under it, subagents included (headings)
    b32 asks;     // something under it asks (headings)
};

struct SideTree {
    SideIn                   *in;
    Arena                    *a;
    Slice<SideRow>            rows;
    Slice<Slice<i32>>         kids;     // each session's, oldest first
    i32                      *root;     // each session's root, -1 if unplaced
    i32                      *under;    // the session it was placed under, else -1
    i32                      *size;     // sessions in its subtree
    b32                      *active;   // something in its subtree runs or asks
    b32                      *asks;     // something in its subtree asks
    b32                      *keep;     // something in its subtree runs, asks
                                        // or is in view: not finished
    i64                      *at;       // the latest at in its subtree, on
                                        // its root's host
};

static Str sidekey(Arena *a, Str prefix, Str tail)
{
    return concat(a, prefix, tail);
}

static b32 sidefolded(SideIn *in, Str key, b32 dflt)
{
    b32 *f = upsert(&in->folds, key, (Arena *)0);
    return f ? *f : dflt;
}

// Place a session's subtree under its root, depth first, adding up what
// the subtree holds.
static void sideplace(SideTree *t, i32 i, i32 root, i32 depth)
{
    t->root[i] = root;
    SideSess *s = &t->in->sess[i];
    t->size[i]   = 1;
    t->active[i] = s->active || s->asks;
    t->asks[i]   = s->asks;
    t->keep[i]   = s->active || s->asks || s->shown;
    t->at[i]     = s->at;
    i32 host     = t->in->sess[root].host;
    Slice<i32> k = t->kids[i];
    for (iz n = 0; n < k.len; n++) {
        i32 c = k[n];
        if (t->root[c]>=0 || depth>=SIDE_DEPTH) {
            continue;  // a cycle of parents, or too deep
        }
        t->under[c] = i;
        sideplace(t, c, root, depth+1);
        t->size[i]   += t->size[c];
        t->active[i] |= t->active[c];
        t->asks[i]   |= t->asks[c];
        t->keep[i]   |= t->keep[c];
        if (t->in->sess[c].host==host && t->at[c]>t->at[i]) {
            t->at[i] = t->at[c];
        }
    }
}

// A root and its subtree's time, to sort roots by (sort hands its
// comparator no context).
struct SideAt {
    i64 at;
    i32 root;
};

static b32 sideatafter(SideAt *a, SideAt *b)
{
    return a->at > b->at;
}

// A session's row, then its subagents', unless folded.
static void sideemit(SideTree *t, i32 i, i32 depth, Str group, i32 aside, i32 level)
{
    SideSess *s   = &t->in->sess[i];
    SideHost *h   = &t->in->hosts[s->host];
    SideRow   r   = {};
    b32       any = 0;
    for (iz n = 0; n < t->kids[i].len; n++) {
        any |= t->under[t->kids[i][n]] == i;
    }
    r.kind   = SROW_SESSION;
    r.depth  = depth;
    r.host   = s->host;
    r.sess   = i;
    r.group    = group;
    r.archived = aside == SIDE_ARCHIVED;
    r.trashed  = aside == SIDE_TRASH;
    r.kids     = any && level<SIDE_DEPTH;
    r.away   = s->host != t->in->sess[t->root[i]].host;
    r.count  = t->size[i];
    r.asks   = t->asks[i];
    if (r.kids) {
        r.key    = sidekey(t->a, sidekey(t->a, sidekey(t->a, "s:", h->key), ":"), s->sid);
        for (iz n = 0; n < t->kids[i].len; n++) {
            i32 c = t->kids[i][n];
            r.forced |= t->under[c]==i && t->active[c];
        }
        r.folded = sidefolded(t->in, r.key, 0) && !r.forced;
    }
    t->rows = push(t->a, t->rows, r);
    if (!r.kids || r.folded) {
        return;
    }
    Slice<i32> k    = t->kids[i];
    i32        done = 0;  // finished subagents
    i32        size = 0;  // and the sessions under them
    for (iz n = 0; n < k.len; n++) {
        if (t->under[k[n]]==i && !t->keep[k[n]]) {
            done++;
            size += t->size[k[n]];
        }
    }
    b32 fold = done >= SIDE_DONEMIN;
    for (iz n = 0; n < k.len; n++) {
        if (t->under[k[n]]==i && (!fold || t->keep[k[n]])) {
            sideemit(t, k[n], depth+1, group, aside, level+1);
        }
    }
    if (!fold) {
        return;
    }
    SideRow f = {};
    f.kind     = SROW_DONE;
    f.depth    = depth + 1;
    f.host     = s->host;
    f.sess     = -1;
    f.group    = group;
    f.archived = r.archived;
    f.trashed  = r.trashed;
    f.key      = concat(t->a, "d", Str(r.key.data+1, r.key.len-1));
    f.count    = size;
    f.folded   = sidefolded(t->in, f.key, 1);
    t->rows = push(t->a, t->rows, f);
    for (iz n = 0; !f.folded && n < k.len; n++) {
        if (t->under[k[n]]==i && !t->keep[k[n]]) {
            sideemit(t, k[n], depth+2, group, aside, level+1);
        }
    }
}

// Whether a root belongs to a section: a group's (SIDE_LIVE, group),
// the ungrouped (SIDE_LIVE, group empty), the archived or the Trash.
static b32 sidein(SideIn *in, SideSess *s, i32 aside, Str group)
{
    if (s->trashed) {
        return aside == SIDE_TRASH;
    } else if (s->archived) {
        return aside == SIDE_ARCHIVED;
    }
    return aside==SIDE_LIVE && (in->byfolder || s->group==group);
}

// The segments of a path, separated by / or \, empty ones left out.
static Slice<Str> sidesegs(Arena *a, Str path)
{
    Slice<Str> r = {};
    iz         b = 0;
    for (iz i = 0; i <= path.len; i++) {
        if (i==path.len || path[i]=='/' || path[i]=='\\') {
            if (i > b) {
                r = push(a, r, Str(path.data+b, i-b));
            }
            b = i + 1;
        }
    }
    return r;
}

// The last n segments of a path, joined by its own separators.
static Str sidetail(Str path, Slice<Str> segs, iz n)
{
    if (n > segs.len) {
        return path;
    }
    Str first = segs[segs.len-n];
    Str last  = segs[segs.len-1];
    return Str(first.data, last.data+last.len-first.data);
}

// How folders[i] is shown: its last segment, with as many before it as
// tell it apart from every other folder's as many, else the whole path.
static Str sidelabel(Arena *a, Slice<Str> folders, iz i)
{
    Slice<Str> mine = sidesegs(a, folders[i]);
    for (iz n = 1; n <= mine.len; n++) {
        Str tail = sidetail(folders[i], mine, n);
        b32 same = 0;
        for (iz k = 0; !same && k < folders.len; k++) {
            if (k != i) {
                Slice<Str> segs = sidesegs(a, folders[k]);
                same = segs.len>=n && sidetail(folders[k], segs, n)==tail;
            }
        }
        if (!same) {
            return tail;
        }
    }
    return folders[i];
}

// A section's roots of one host (host >= 0) or of all, and what they
// hold.
struct SideSum {
    i32 count;
    b32 asks;
    b32 any;
};

static SideSum sidesum(SideTree *t, Slice<i32> roots, i32 host, i32 aside, Str group)
{
    SideSum r = {};
    for (iz n = 0; n < roots.len; n++) {
        SideSess *s = &t->in->sess[roots[n]];
        if ((host<0 || s->host==host) && sidein(t->in, s, aside, group)) {
            r.any    = 1;
            r.count += t->size[roots[n]];
            r.asks  |= t->asks[roots[n]];
        }
    }
    return r;
}

// A host's sessions in a section and those of the hosts under it, as
// sidesum adds them.
static SideSum sidesums(SideTree *t, Slice<i32> roots, i32 h, i32 aside, Str group)
{
    SideSum r = sidesum(t, roots, h, aside, group);
    for (i32 k = 0; k < (i32)t->in->hosts.len; k++) {
        if (t->in->hosts[k].under == h+1) {
            SideSum c = sidesums(t, roots, k, aside, group);
            r.any   |= c.any;
            r.count += c.count;
            r.asks  |= c.asks;
        }
    }
    return r;
}

static void sidehost(SideTree *, Slice<i32>, i32, i32, i32, Str, Str, b32);

// A host's live roots under a heading for each folder, in the order of
// their first roots.
static void sidefolders(SideTree *t, Slice<i32> roots, i32 h, i32 depth)
{
    SideIn    *in      = t->in;
    Slice<Str> folders = {};
    for (iz n = 0; n < roots.len; n++) {
        SideSess *s   = &in->sess[roots[n]];
        b32       dup = s->host!=h || !sidein(in, s, SIDE_LIVE, {});
        for (iz k = 0; !dup && k < folders.len; k++) {
            dup = folders[k] == s->folder;
        }
        if (!dup) {
            folders = push(t->a, folders, s->folder);
        }
    }
    for (iz f = 0; f < folders.len; f++) {
        SideRow r = {};
        r.kind   = SROW_FOLDER;
        r.depth  = depth;
        r.host   = h;
        r.sess   = -1;
        r.folder = folders[f];
        r.label  = folders[f].len ? sidelabel(t->a, folders, f) : Str{};
        r.key    = concat(t->a, concat(t->a, concat(t->a, "f:", in->hosts[h].key), ":"),
                          folders[f]);
        for (iz n = 0; n < roots.len; n++) {
            SideSess *s = &in->sess[roots[n]];
            if (s->host==h && sidein(in, s, SIDE_LIVE, {}) && s->folder==folders[f]) {
                r.count += t->size[roots[n]];
                r.asks  |= t->asks[roots[n]];
            }
        }
        r.forced = r.asks;
        r.folded = sidefolded(in, r.key, 0) && !r.forced;
        t->rows  = push(t->a, t->rows, r);
        for (iz n = 0; !r.folded && n < roots.len; n++) {
            SideSess *s = &in->sess[roots[n]];
            if (s->host==h && sidein(in, s, SIDE_LIVE, {}) && s->folder==folders[f]) {
                sideemit(t, roots[n], depth+1, {}, SIDE_LIVE, 0);
            }
        }
    }
}

// A section's sessions, under host headings when there are any, a host
// under another (SideHost.under) after that one's sessions, a level in.
static void sidesection(SideTree *t, Slice<i32> roots, i32 depth, i32 aside,
                        Str group, Str prefix, b32 always)
{
    for (i32 h = 0; h < (i32)t->in->hosts.len; h++) {
        if (!t->in->hosts[h].under) {
            sidehost(t, roots, h, depth, aside, group, prefix, always);
        }
    }
}

static void sidehost(SideTree *t, Slice<i32> roots, i32 h, i32 depth, i32 aside,
                     Str group, Str prefix, b32 always)
{
    SideIn *in = t->in;
    {
        SideSum sum = sidesums(t, roots, h, aside, group);
        if (!in->heads && !sum.any) {
            return;
        } else if (in->heads) {
            if (!sum.any && !(always && in->hosts[h].always)) {
                return;
            }
            SideRow r = {};
            r.kind   = SROW_HOST;
            r.depth  = depth;
            r.host   = h;
            r.sess   = -1;
            r.group    = group;
            r.archived = aside == SIDE_ARCHIVED;
            r.trashed  = aside == SIDE_TRASH;
            r.key      = sidekey(t->a, prefix, sidekey(t->a, "h:", in->hosts[h].key));
            r.count  = sum.count;
            r.asks   = sum.asks;
            r.forced = sum.asks;
            r.folded = sidefolded(in, r.key, 0) && !r.forced;
            t->rows  = push(t->a, t->rows, r);
            if (r.folded) {
                return;
            }
        }
        i32 inner = depth + (in->heads ? 1 : 0);
        if (in->byfolder && aside==SIDE_LIVE) {
            sidefolders(t, roots, h, inner);
        } else {
            for (iz n = 0; n < roots.len; n++) {
                SideSess *s = &in->sess[roots[n]];
                if (s->host==h && sidein(in, s, aside, group)) {
                    sideemit(t, roots[n], inner, group, aside, 0);
                }
            }
        }
        for (i32 k = 0; k < (i32)in->hosts.len; k++) {
            if (in->hosts[k].under == h+1) {
                sidehost(t, roots, k, depth + (in->heads ? 1 : 0), aside, group, prefix,
                         always);
            }
        }
    }
}

// The sidebar's rows, as the file's comment says.
static Slice<SideRow> sidetree(Arena *a, SideIn *in)
{
    SideTree t = {};
    t.in = in;
    t.a  = a;
    iz n = in->sess.len;
    t.kids   = {};
    t.root   = alloc<i32>(a, n ? n : 1);
    t.under  = alloc<i32>(a, n ? n : 1);
    t.size   = alloc<i32>(a, n ? n : 1);
    t.active = alloc<b32>(a, n ? n : 1);
    t.asks   = alloc<b32>(a, n ? n : 1);
    t.keep   = alloc<b32>(a, n ? n : 1);
    t.at     = alloc<i64>(a, n ? n : 1);
    for (iz i = 0; i < n; i++) {
        t.kids    = push(a, t.kids, Slice<i32>{});
        t.root[i]  = -1;
        t.under[i] = -1;
    }

    // Sessions by host and sid, then each subagent under its parent,
    // oldest first
    Map<i32> *bysid = 0;
    for (iz i = 0; i < n; i++) {
        SideSess *s   = &in->sess[i];
        Buf       key(a, 8 + s->sid.len);
        print(&key, (i64)s->host);
        putbyte(&key, ':');
        print(&key, s->sid);
        *upsert(&bysid, finish(&key), a) = (i32)i + 1;
    }
    i32 *parent = alloc<i32>(a, n ? n : 1);
    for (iz i = 0; i < n; i++) {
        SideSess *s = &in->sess[i];
        parent[i] = -1;
        if (s->parent.len) {
            Buf key(a, 8 + s->parent.len);
            print(&key, (i64)(s->phost ? s->phost-1 : s->host));
            putbyte(&key, ':');
            print(&key, s->parent);
            i32 *p = upsert(&bysid, finish(&key), (Arena *)0);
            parent[i] = p && *p-1!=(i32)i ? *p-1 : -1;
        }
    }
    for (iz i = n-1; i >= 0; i--) {
        if (parent[i] >= 0) {
            t.kids[parent[i]] = push(a, t.kids[parent[i]], (i32)i);
        }
    }

    // Roots: those without a parent here, then any left in a cycle
    Slice<i32> roots = {};
    for (iz i = 0; i < n; i++) {
        if (parent[i] < 0) {
            sideplace(&t, (i32)i, (i32)i, 0);
            roots = push(a, roots, (i32)i);
        }
    }
    for (iz i = 0; i < n; i++) {
        if (t.root[i] < 0) {
            sideplace(&t, (i32)i, (i32)i, 0);
            roots = push(a, roots, (i32)i);
        }
    }

    // The groups: in order, then any others the roots name, newest first
    Slice<Str> groups = {};
    for (iz i = 0; i < in->groups.len; i++) {
        b32 dup = !in->groups[i].len;
        for (iz k = 0; !dup && k < groups.len; k++) {
            dup = groups[k] == in->groups[i];
        }
        if (!dup) {
            groups = push(a, groups, in->groups[i]);
        }
    }
    for (iz r = 0; r < roots.len; r++) {
        SideSess *s   = &in->sess[roots[r]];
        b32       dup = s->archived || s->trashed || !s->group.len;
        for (iz k = 0; !dup && k < groups.len; k++) {
            dup = groups[k] == s->group;
        }
        if (!dup) {
            groups = push(a, groups, s->group);
        }
    }

    // Then, with byat, the roots by their subtrees' latest time: after
    // the groups, which keep the order they were named in
    if (in->byat && roots.len > 1) {
        SideAt *by = alloc<SideAt>(a, roots.len);
        for (iz r = 0; r < roots.len; r++) {
            by[r] = {t.at[roots[r]], roots[r]};
        }
        sort(by, roots.len, sideatafter, *a);
        for (iz r = 0; r < roots.len; r++) {
            roots[r] = by[r].root;
        }
    }
    for (iz i = 0; !in->byfolder && i < groups.len; i++) {
        SideSum sum = sidesum(&t, roots, -1, SIDE_LIVE, groups[i]);
        SideRow r   = {};
        r.kind   = SROW_GROUP;
        r.sess   = -1;
        r.group  = groups[i];
        r.key    = sidekey(a, "g:", groups[i]);
        r.count  = sum.count;
        r.asks   = sum.asks;
        r.forced = sum.asks;
        r.folded = sidefolded(in, r.key, 0) && !r.forced;
        t.rows = push(a, t.rows, r);
        if (!r.folded) {
            sidesection(&t, roots, 1, SIDE_LIVE, groups[i], concat(a, r.key, "|"), 0);
        }
    }

    // The rest, by host, every host listed always
    sidesection(&t, roots, 0, SIDE_LIVE, {}, {}, 1);

    // Archived, folded unless opened
    SideSum sum = sidesum(&t, roots, -1, SIDE_ARCHIVED, {});
    if (sum.any || in->archive) {
        SideRow r = {};
        r.kind     = SROW_ARCHIVED;
        r.archived = 1;
        r.sess   = -1;
        r.key    = "a";
        r.count  = sum.count;
        r.asks   = sum.asks;
        r.forced = sum.asks;
        r.folded = sidefolded(in, r.key, 1) && !r.forced;
        t.rows = push(a, t.rows, r);
        if (!r.folded) {
            sidesection(&t, roots, 1, SIDE_ARCHIVED, {}, "a|", 0);
        }
    }

    // The Trash, folded unless opened, and only with sessions in it
    sum = sidesum(&t, roots, -1, SIDE_TRASH, {});
    if (sum.any) {
        SideRow r = {};
        r.kind    = SROW_TRASH;
        r.trashed = 1;
        r.sess    = -1;
        r.key     = "t";
        r.count   = sum.count;
        r.asks    = sum.asks;
        r.forced  = sum.asks;
        r.folded  = sidefolded(in, r.key, 1) && !r.forced;
        t.rows = push(a, t.rows, r);
        if (!r.folded) {
            sidesection(&t, roots, 1, SIDE_TRASH, {}, "t|", 0);
        }
    }
    return t.rows;
}
