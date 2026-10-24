// Path completion's logic, apart from ImGui, after raddbg's file pickers:
// a path typed in part names, up to its last separator ('/', on Windows
// '\' too), the folder to list, and the rest is a filter. The filter is
// split on spaces into parts, each of which must be in an entry's name,
// ignoring case. An entry named exactly the filter comes first, then
// those that begin with its first part, then the rest, folders before
// files, each by name. Dot entries show only to a filter that begins
// with a dot. Accepting a folder makes the text its path and a
// separator, ready to list it in turn.
//
// This is free and unencumbered software released into the public domain.

struct PathQuery {
    Str dir;     // the folder, with its last separator; null if none typed
    Str filter;  // the rest
};

static PathQuery pathquery(Str text, b32 win)
{
    PathQuery r = {};
    r.filter = text;
    for (iz i = text.len-1; i >= 0; i--) {
        if (issep(text[i], win)) {
            r.dir    = takehead(text, i+1);
            r.filter = cuthead(text, i+1);
            break;
        }
    }
    return r;
}

// Whether a folder typed can be listed: absolute, or in the home
// directory (~), never relative to a working directory nobody sees.
static b32 pathlistable(Str dir, b32 win)
{
    return dir.len && (pathroot(dir, win) || (dir[0]=='~' && dir.len>1 && issep(dir[1], win)));
}

// The next part of a filter, from *rest, spaces skipped: empty at its end.
static Str filterpart(Str *rest)
{
    Str s = *rest;
    iz  i = 0;
    for (; i<s.len && s[i]==' '; i++) {}
    iz j = i;
    for (; j<s.len && s[j]!=' '; j++) {}
    *rest = cuthead(s, j);
    return {s.data+i, j-i};
}

enum : i32 {
    PRANK_EXACT,   // the name is the filter
    PRANK_PREFIX,  // it begins with the filter's first part
    PRANK_OTHER,
};

// How a name ranks for a filter (PRANK_*), or -1 if it is not picked.
static i32 pathrank(Str name, Str filter)
{
    Str rest  = filter;
    Str first = filterpart(&rest);
    if (name.len && name[0]=='.' && !startswith(first, ".")) {
        return -1;
    }
    rest = filter;
    for (Str p = filterpart(&rest); p.len; p = filterpart(&rest)) {
        if (findfold(name, p) < 0) {
            return -1;
        }
    }
    Str whole = filter;
    for (; whole.len && whole[0]==' '; whole = cuthead(whole, 1)) {}
    for (; whole.len && whole[whole.len-1]==' '; whole.len--) {}
    return whole.len && equalfold(name, whole) ? PRANK_EXACT :
           first.len && equalfold(takehead(name, first.len), first) ? PRANK_PREFIX :
           PRANK_OTHER;
}

struct PathEntry {
    Str name;
    b32 dir;
};

struct PathPick {
    Str name;
    b32 dir;
    i32 rank;  // PRANK_*
};

// Names in order ignoring case, those equal so in byte order.
static b32 pickless(PathPick *a, PathPick *b)
{
    if (a->rank != b->rank) {
        return a->rank < b->rank;
    } else if (a->dir != b->dir) {
        return a->dir;
    }
    iz n = a->name.len<b->name.len ? a->name.len : b->name.len;
    for (iz i = 0; i < n; i++) {
        u8 x = lowercase(a->name[i]);
        u8 y = lowercase(b->name[i]);
        if (x != y) {
            return x < y;
        }
    }
    if (a->name.len != b->name.len) {
        return a->name.len < b->name.len;
    }
    for (iz i = 0; i < n; i++) {
        if (a->name[i] != b->name[i]) {
            return a->name[i] < b->name[i];
        }
    }
    return 0;
}

// The entries a filter picks, best first, only folders with dirs.
static Slice<PathPick> pathpicks(Arena *a, Slice<PathEntry> ents, Str filter, b32 dirs)
{
    Slice<PathPick> r = {};
    for (iz i = 0; i < ents.len; i++) {
        i32 rank = dirs && !ents[i].dir ? -1 : pathrank(ents[i].name, filter);
        if (rank >= 0) {
            r = push(a, r, PathPick{ents[i].name, ents[i].dir, rank});
        }
    }
    sort(r.data, r.len, pickless, *a);
    return r;
}

// Which of a picked name's bytes the filter's parts matched, each where
// it is first found: 1 for those to highlight.
static u8 *pathmarks(Arena *a, Str name, Str filter)
{
    u8 *m = alloc<u8>(a, name.len+1);
    for (Str p = filterpart(&filter); p.len; p = filterpart(&filter)) {
        iz at = findfold(name, p);
        for (iz i = 0; at>=0 && i<p.len; i++) {
            m[at+i] = 1;
        }
    }
    return m;
}

// The text accepting a pick makes: a folder's path ends with the
// separator its own folder was typed with, to list it next.
static Str pathaccept(Arena *a, PathQuery q, PathPick p)
{
    Str r = concat(a, q.dir, p.name);
    if (p.dir && q.dir.len) {
        r = concat(a, r, {q.dir.data+q.dir.len-1, 1});
    }
    return r;
}
