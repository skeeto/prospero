// The Ctrl/Cmd+K palette's logic, apart from ImGui: the items it offers
// and their order for a query.
//
// Items come in sections, in this order: sessions; layouts and panes;
// settings; Help's sections. Each section keeps its own ranking, and
// ranks are never compared across them. Settings and Help are searched
// by their own panes' searches (cfgfindlist, helpsearch), which give
// their items already ranked (rank); the rest are matched here.
//
// A query is split on spaces into words, ignoring case, and every word
// must match one of an item's fields. A title or group matches as a
// subsequence, scored as the '@' picker scores a name (mentionrange),
// from each place the word's first character occurs, the best kept; a
// title's score counts double. A folder, host or sid matches only as a
// substring, since a subsequence of a path or of hex matches almost
// anything. An item's score is the sum of its words' best. Higher scores
// come first; on a tie the minor items (subagents, archived sessions)
// come after the rest, then the most recently active first, then the
// newest sid.
//
// The empty query offers only sessions, the major ones, and not the one
// shown:
// those viewed in this run first, most recently viewed first, so that
// the palette then Enter goes back to the session before, then the rest
// by recent activity.
//
// Items are built and ordered as the palette opens and as its query
// changes, never in between, so nothing moves under the pointer; the
// pick is kept by its identity (id) when they are built again.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    PAL_SESSION,
    PAL_LAYOUT,
    PAL_PANE,
    PAL_SETTING,
    PAL_HELP,
};

// The sections, as palsect numbers them, with their headings.
static Str palsectnames[] = {"Sessions", "Layouts and panes", "Settings", "Help"};

static i32 palsect(i32 kind)
{
    switch (kind) {
    case PAL_SESSION: return 0;
    case PAL_LAYOUT:
    case PAL_PANE:    return 1;
    case PAL_SETTING: return 2;
    }
    return 3;
}

enum : i32 {
    PAL_TITLE_WEIGHT = 2,              // a title's match counts this many times
    PAL_SUBSTR       = MENTION_MATCH,  // a substring match, per character
};

struct PalItem {
    i32  kind;      // PAL_*
    Str  id;        // its identity, which the pick is kept by
    Str  title;     // matched as a subsequence, counted double
    Str  group;     // matched as a subsequence
    Str  folder;    // matched as a substring
    Str  host;      // matched as a substring
    Str  sid;       // matched as a substring, and the last tie-break
    i64  at;        // its latest activity, epoch ms
    i64  seen;      // when viewed in this run, larger is later; 0 if not
    b32  minor;     // a subagent or archived: offered only to a query
    b32  shown;     // the one in view: left out of the empty query
    i32  rank;      // its place in another search's results, 1 first,
                    // offered as given; 0 to be matched here
    Str  note;      // the dim line under its title, if not a session's
    i32  ref;       // what it goes to: a layout's number, a pane, a
                    // setting's row, a Help page
    Str  arg;       // and a Help section's heading, empty for the top
};

// The best score of a lower-cased word as a subsequence of s, from any
// place its first character occurs, or -1.
static i32 palsubseq(Str s, Str word)
{
    i32 best = -1;
    for (iz i = 0; i < s.len; i++) {
        if (lowercase(s[i]) == word[0]) {
            i32 r = mentionrange(s, i, s.len, word, 1);
            best = r>best ? r : best;
        }
    }
    return best;
}

// Whether a lower-cased word is in s, ignoring case.
static b32 palsubstr(Str s, Str word)
{
    for (iz i = 0; i+word.len <= s.len; i++) {
        iz j = 0;
        for (; j<word.len && lowercase(s[i+j])==word[j]; j++) {}
        if (j == word.len) {
            return 1;
        }
    }
    return 0;
}

// An item's score for a lower-cased query, -1 if a word matches nowhere.
static i32 palscore(PalItem *it, Str lowq)
{
    i32 total = 0;
    Str rest  = lowq;
    for (;;) {
        Str w = filterpart(&rest);
        if (!w.len) {
            return total;
        }
        i32 best = palsubseq(it->title, w);
        best = best>=0 ? best*PAL_TITLE_WEIGHT : -1;
        i32 g = palsubseq(it->group, w);
        best = g>best ? g : best;
        if (best<0 && (palsubstr(it->folder, w) || palsubstr(it->host, w) ||
                       palsubstr(it->sid, w))) {
            best = (i32)w.len * PAL_SUBSTR;
        }
        if (best < 0) {
            return -1;
        }
        total += best;
    }
}

struct PalKey {
    i32  sect;
    i32  score;
    i32  minor;
    i64  seen;
    i64  at;
    Str  sid;
    i32  i;
};

static b32 palbefore(PalKey *a, PalKey *b)
{
    if (a->sect  != b->sect)  return a->sect  < b->sect;
    if (a->score != b->score) return a->score > b->score;
    if (a->minor != b->minor) return a->minor < b->minor;
    if (a->seen  != b->seen)  return a->seen  > b->seen;
    if (a->at    != b->at)    return a->at    > b->at;
    return compare(a->sid, b->sid) > 0;
}

// The items offered for a query, as indexes into items, in order.
static Slice<i32> palorder(Arena *a, Slice<PalItem> items, Str query)
{
    Str lowq = clone(a, trim(query));
    for (iz i = 0; i < lowq.len; i++) {
        lowq.data[i] = lowercase(lowq[i]);
    }
    PalKey *keys = alloc<PalKey>(a, items.len);
    i32     n    = 0;
    for (i32 i = 0; i < (i32)items.len; i++) {
        PalItem *it = &items.data[i];
        PalKey   k  = {};
        k.i    = i;
        k.sect = palsect(it->kind);
        k.at   = it->at;
        k.sid  = it->sid;
        if (!lowq.len) {
            if (it->kind!=PAL_SESSION || it->minor || it->shown) {
                continue;
            }
            k.seen = it->seen;
        } else if (it->rank) {
            k.score = -it->rank;  // as that search ranked it
        } else {
            k.score = palscore(it, lowq);
            k.minor = it->minor;
            if (k.score < 0) {
                continue;
            }
        }
        keys[n++] = k;
    }
    sort(keys, n, palbefore, *a);
    Slice<i32> r = {};
    r.data = alloc<i32>(a, n);
    r.len  = r.cap = n;
    for (i32 i = 0; i < n; i++) {
        r.data[i] = keys[i].i;
    }
    return r;
}

// The place in order of the item whose identity is id, else 0.
static i32 palkeep(Slice<PalItem> items, Slice<i32> order, Str id)
{
    for (i32 i = 0; id.len && i < (i32)order.len; i++) {
        if (items.data[order.data[i]].id == id) {
            return i;
        }
    }
    return 0;
}
