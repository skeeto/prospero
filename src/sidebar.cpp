// Sessions sidebar: the tree it shows, as rows to draw
//
// The hierarchy is Group › Host › Session › Subagent. Groups come
// first, in the GUI's order (which keeps empty ones too), each merged by
// name across hosts; then the sessions in no group, host by host; then
// an Archived heading over the archived sessions. A subagent belongs
// where its root is: the root's group and archiving are its own. Host
// headings show only when more than one host could be shown (heads),
// under each group and the Archived heading too, where that host has
// sessions; at the top level every host listed always shows, empty or
// not. Within a host, roots are newest first and subagents oldest first
// under their parents, as the engine lists them.
//
// Every heading folds, and so does a session with subagents. The user's
// choices are kept by key (folds: 1 folded, 0 open); without one, the
// Archived heading is folded and everything else open. A session stays
// open while anything below it runs a turn or asks, and a heading while
// anything below it asks, so that nothing waiting for the user is ever
// hidden. A folded row's sessions are left out of the rows.
//
// Fold keys:
//   g:NAME            a group
//   h:HOST            a host's heading at the top level
//   g:NAME|h:HOST     a host's heading under a group
//   a                 the Archived heading
//   a|h:HOST          a host's heading under it
//   s:HOST:SID        a session with subagents
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    SROW_GROUP,
    SROW_HOST,
    SROW_SESSION,
    SROW_ARCHIVED,  // the Archived heading
};

enum : i32 {
    SIDE_DEPTH = 16,  // levels of subagents placed under their parents
};

// A session as the sidebar sees it.
struct SideSess {
    i32  host;      // index into the hosts
    Str  sid;
    Str  parent;    // a subagent's, else empty
    Str  group;     // its group, if any: only a root's counts
    b32  archived;  // only a root's counts
    b32  active;    // runs a turn
    b32  asks;      // waits for the user
};

struct SideHost {
    Str key;        // stable across runs, for its fold state
    b32 always;     // listed with no sessions to show
};

struct SideIn {
    Slice<SideSess> sess;    // each host's newest first
    Slice<SideHost> hosts;   // in order
    Slice<Str>      groups;  // in order, empty ones included
    Map<b32>       *folds;   // the user's choices: 1 folded, 0 open
    b32             heads;   // host headings shown
};

struct SideRow {
    i32 kind;     // SROW_*
    i32 depth;    // levels of indentation
    i32 host;     // SROW_HOST and SROW_SESSION: index into the hosts
    i32 sess;     // SROW_SESSION: index into the sessions, else -1
    Str group;    // SROW_GROUP: its name; else the group it is in, if any
    Str key;      // its fold key, empty if it does not fold
    b32 folded;   // its rows below are left out
    b32 forced;   // open whatever the user chose
    b32 kids;     // a session with subagents
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
    }
}

// A session's row, then its subagents', unless folded.
static void sideemit(SideTree *t, i32 i, i32 depth, Str group, i32 level)
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
    r.group  = group;
    r.kids   = any && level<SIDE_DEPTH;
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
    Slice<i32> k = t->kids[i];
    for (iz n = 0; n < k.len; n++) {
        if (t->under[k[n]] == i) {
            sideemit(t, k[n], depth+1, group, level+1);
        }
    }
}

// Whether a root belongs to a section: a group's (group), the ungrouped
// (archived 0, group empty), or the archived (archived 1).
static b32 sidein(SideSess *s, b32 archived, Str group)
{
    if (s->archived) {
        return archived;
    }
    return !archived && s->group==group;
}

// A section's roots of one host (host >= 0) or of all, and what they
// hold.
struct SideSum {
    i32 count;
    b32 asks;
    b32 any;
};

static SideSum sidesum(SideTree *t, Slice<i32> roots, i32 host, b32 archived, Str group)
{
    SideSum r = {};
    for (iz n = 0; n < roots.len; n++) {
        SideSess *s = &t->in->sess[roots[n]];
        if ((host<0 || s->host==host) && sidein(s, archived, group)) {
            r.any    = 1;
            r.count += t->size[roots[n]];
            r.asks  |= t->asks[roots[n]];
        }
    }
    return r;
}

// A section's sessions, under host headings when there are any.
static void sidesection(SideTree *t, Slice<i32> roots, i32 depth, b32 archived,
                        Str group, Str prefix, b32 always)
{
    SideIn *in = t->in;
    for (i32 h = 0; h < (i32)in->hosts.len; h++) {
        SideSum sum = sidesum(t, roots, h, archived, group);
        if (!in->heads && !sum.any) {
            continue;
        } else if (in->heads) {
            if (!sum.any && !(always && in->hosts[h].always)) {
                continue;
            }
            SideRow r = {};
            r.kind   = SROW_HOST;
            r.depth  = depth;
            r.host   = h;
            r.sess   = -1;
            r.group  = group;
            r.key    = sidekey(t->a, prefix, sidekey(t->a, "h:", in->hosts[h].key));
            r.count  = sum.count;
            r.asks   = sum.asks;
            r.forced = sum.asks;
            r.folded = sidefolded(in, r.key, 0) && !r.forced;
            t->rows  = push(t->a, t->rows, r);
            if (r.folded) {
                continue;
            }
        }
        for (iz n = 0; n < roots.len; n++) {
            SideSess *s = &in->sess[roots[n]];
            if (s->host==h && sidein(s, archived, group)) {
                sideemit(t, roots[n], depth + (in->heads ? 1 : 0), group, 0);
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
            print(&key, (i64)s->host);
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
        b32       dup = s->archived || !s->group.len;
        for (iz k = 0; !dup && k < groups.len; k++) {
            dup = groups[k] == s->group;
        }
        if (!dup) {
            groups = push(a, groups, s->group);
        }
    }
    for (iz i = 0; i < groups.len; i++) {
        SideSum sum = sidesum(&t, roots, -1, 0, groups[i]);
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
            sidesection(&t, roots, 1, 0, groups[i], concat(a, r.key, "|"), 0);
        }
    }

    // The rest, by host, every host listed always
    sidesection(&t, roots, 0, 0, {}, {}, 1);

    // Archived, folded unless opened
    SideSum sum = sidesum(&t, roots, -1, 1, {});
    if (sum.any) {
        SideRow r = {};
        r.kind   = SROW_ARCHIVED;
        r.sess   = -1;
        r.key    = "a";
        r.count  = sum.count;
        r.asks   = sum.asks;
        r.forced = sum.asks;
        r.folded = sidefolded(in, r.key, 1) && !r.forced;
        t.rows = push(a, t.rows, r);
        if (!r.folded) {
            sidesection(&t, roots, 1, 1, {}, "a|", 0);
        }
    }
    return t.rows;
}
