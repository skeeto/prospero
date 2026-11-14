// The '@' picker's adapter between the compose box and its candidate
// sources: one typed query in, the candidates it may become out, and a
// line saying what they are. Two sources, chosen here: the workspace
// index of the session's working tree (filewalk.cpp), walked on this
// machine by a worker of the window's file pool (GFS_WALK), and one
// folder of that tree listed through dirget, which serves a remote host
// and stands in while the walk goes on, or when it could not be had.
//
// It knows the GUI only for dirget, the window's pool (guifsput), the
// scheduler (scheddirty) and the attachments (attachfile, attachwhy,
// attachremove): nothing here draws, reads a key or waits. Its arena is
// the frame's, so the candidates and their paths live for one frame, and
// the caller keeps none of them; the index is the exception, kept per
// session in g->perm and the 16 MiB reservation its walk and paths are
// in. The walk makes no frames: its answer is sorted on the window's
// thread, and draws again only if the picker is up.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    MENTION_NONE,       // no picker
    MENTION_INDEX,      // the workspace walk (filewalk.cpp), on this machine
    MENTION_LIST,       // one directory, listed through dirget
    MENTION_ROWS = 12,  // rows the popup draws
};

enum : i64 {
    MENTION_TTL  = 60000000,  // µs: an index older than this is rebuilt, and
                              // no walk is begun this long after one given
                              // up or refused (its tree not responding)
    MENTION_BUSY = 1000000,   // µs: or this long after the pool was busy
};

enum : iz {
    MENTION_IDX_MEM = 16<<20,  // bytes reserved for one session's index
};

// A candidate, as the token is written: a path relative to the session's
// working directory, '/' separated on every host, with what the popup
// shows of it beside the path. Or another session, by its title, or by
// its id where the title cannot be a token or another session has it.
struct MentionCand {
    Str path;   // "src/ui/draw.cpp", or the session's title or id
    Str name;   // "draw.cpp", or the session's title
    Str dir;    // "src/ui", without its trailing '/', or the session's id
    b32 isdir;
    b32 attached;  // already in the draft's attachments
    Str sid;       // a session's, else empty
    i32 score;     // the query's rank of it (mentionpicks), 0 with none
};

enum : i32 {
    MENTION_SESS = 8,  // sessions a query lists, at most
};

// What one frame's query came to.
struct MentionSrc {
    i32   kind;       // MENTION_*
    Str   root;       // the session's working directory, absolute
    Str   qdir;       // LIST: the folder being listed, relative to root
    Str   filter;     // LIST: what the typed word narrows to
    Slice<MentionCand> cands;
    b32   truncated;  // the listing hit its cap
    Str   status;     // one line for the popup's footer
};

// Whether a listing's name is one the picker keeps out of the way: "."
// and "..", which a host's dir may still send, and a dot file unless the
// filter asks for one, as the path field's completion does (pathrank).
static b32 mentionhidden(Str name, Str filter)
{
    if (name == "." || name == "..") {
        return 1;
    }
    return name.len && name[0]=='.' && !(filter.len && filter[0]=='.');
}

// Folders before files, then by name ignoring case, then byte-wise, so
// the order is the same on every filesystem: the order of an empty query,
// whose listing order means nothing. A comparator of its own rather than
// pickless: that one orders by a rank this source has none of, and its
// name is a PathPick's.
static b32 mentionless(MentionCand *a, MentionCand *b)
{
    if (a->isdir != b->isdir) {
        return a->isdir;
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
    return compare(a->name, b->name) < 0;
}

// The index's mtimes and paths, which its sort orders by: sort hands its
// comparator no context, as mentionpickpaths does for the ranker. Set by
// mentionidxsort for the length of its sort.
static Slice<i64> mentionidxsorts;
static Slice<Str> mentionidxpaths;

// An index entry's order: the newest first, then by the path's bytes so
// that entries of one mtime still come in an order every filesystem
// agrees on. Newest first is why: the empty query ranks nothing and keeps
// the list's order (mentionpicks), so sorting once here gives the picker
// its "recent files" for free, on every frame, instead of a sort a frame.
static b32 mentionidxless(i32 *a, i32 *b)
{
    i64 x = mentionidxsorts[*a];
    i64 y = mentionidxsorts[*b];
    if (x != y) {
        return x > y;
    }
    return compare(mentionidxpaths[*a], mentionidxpaths[*b]) < 0;
}

// Order a finished index's entries: newest first. The three arrays are
// permuted together, in place, through an order of their own, so paths,
// mtimes and isdir bits cannot come apart. Nothing is pushed into the
// index's arena after this, which is why its sort scratch can come from
// it (a copy of an arena grows it, and this one's end is not refreshed).
static void mentionidxsort(MentionIdx *idx)
{
    Slice<Str> paths  = idx->w->paths;
    Slice<i64> mtimes = idx->w->mtimes;
    Slice<u8>  isdirs = idx->w->isdirs;
    iz         n      = paths.len;
    if (n < 2) {
        return;
    }
    i32 *ord = alloc<i32>(&idx->w->mem, n);
    for (iz i = 0; i < n; i++) {
        ord[i] = (i32)i;
    }
    mentionidxsorts = mtimes;
    mentionidxpaths = paths;
    sort(ord, n, mentionidxless, idx->w->mem);
    mentionidxsorts = {};
    mentionidxpaths = {};
    for (iz i = 0; i < n; i++) {
        while ((iz)ord[i] != i) {
            iz j = (iz)ord[i];
            Str p = paths[i];  paths[i] = paths[j];   paths[j] = p;
            i64 m = mtimes[i]; mtimes[i] = mtimes[j]; mtimes[j] = m;
            u8  d = isdirs[i]; isdirs[i] = isdirs[j]; isdirs[j] = d;
            i32 k = ord[i];    ord[i] = ord[j];      ord[j] = k;
        }
    }
}

// A walk's Region let go, by the pool once its worker is out of it (an
// index that gave its walk up), or by the index itself.
static void mentionwalkdrop(Os *os, void *own)
{
    FileWalkJob *j = (FileWalkJob *)own;
    Region      *r = j->r;
    filewalkfree(&j->w);
    os_release(os, r);
}

// An index emptied: a walk still out is abandoned, its Region left to the
// pool when its worker still runs, else let go here.
static void mentionidxfree(Gui *g, MentionIdx *idx)
{
    b32 pools = idx->walkid && g->fs && fsabandon(g->fs, idx->walkid);
    if (idx->job && !pools) {
        mentionwalkdrop(g->os, idx->job);
    }
    idx->job    = 0;
    idx->w      = 0;
    idx->walkid = 0;
    idx->root   = {};
    idx->born   = 0;
    // The cached rank indexes paths this Region held: it goes with them
    idx->ranked = 0;
    idx->qlen   = 0;
    idx->npicks = 0;
}

// Set an index up for a walk over root, in a reservation of its own, its
// job not yet put (mentionidxput). Returns 0 when the reservation cannot
// be made. win is the host's own rule for paths and .gitignore chains, as
// hostwin reads it.
static b32 mentionidxbegin(Gui *g, MentionIdx *idx, Str root, b32 win)
{
    Region *r = os_reserve(g->os, MENTION_IDX_MEM);
    if (!r) {
        return 0;
    }
    Arena        a = newarena(r);
    FileWalkJob *j = alloc<FileWalkJob>(&a);
    j->r     = r;
    j->root  = clone(&a, root);
    j->win   = win;
    j->limit = FILEWALK_TIME;
    j->w.mem = a;  // the rest of the Region, the walk's
    idx->job  = j;
    idx->w    = &j->w;
    idx->root = j->root;
    idx->born = os_ticks(g->os);
    // A new walk: any rank cached against the last one is of no use, and
    // the generation is what says so
    idx->gen++;
    idx->ranked = 0;
    return 1;
}

static b32 guifsput(Gui *, FsPut *, Str *);

// The slot being walked, its job put on the window's pool unless it is
// already out. Refused (its tree's file system not responding, the pool
// busy), the slot is let go and the index served stays: no walk is begun
// again for a while.
static void mentionidxput(Gui *g, MentionIdxs *m, Arena scratch)
{
    MentionIdx *idx = m->next>=0 ? &m->slot[m->next] : 0;
    if (!idx || !idx->job || idx->walkid) {
        return;
    }
    Str   norm = normpath(&scratch, idx->root, ".", HOST_WINDOWS);
    FsPut put  = {};
    put.id    = ++g->nextid;
    put.kind  = GFS_WALK;
    put.run   = filewalkrun;
    put.ctx   = idx->job;
    put.paths = {&norm, 1, 1};
    put.exact = 1;  // a stuck walk marks where it was alone, not the tree
    put.own   = idx->job;
    put.drop  = mentionwalkdrop;
    idx->walkid = put.id;  // first: an inline job is answered in guifsput
    Str why     = {};
    if (!guifsput(g, &put, &why)) {
        idx->walkid = 0;
        mentionidxfree(g, idx);
        m->next  = -1;
        m->again = os_ticks(g->os) + (why==fsbusy ? MENTION_BUSY : MENTION_TTL);
    }
}

// The session's index of root to serve, its walk done, else null, and the
// listing stands in. It is built on first use, and built again in the
// other slot when it is older than MENTION_TTL or a turn has ended since,
// as a turn may make or delete the very files the picker should offer; the
// old one is served until the new one is done, so a rebuild costs the
// picker nothing. A turn's start makes nothing stale, and a turn going on
// leaves the index alone: the user may be writing while it works. The walk
// goes to the window's pool, unless a checklist holds it (mentionhold):
// then its slot waits, set up, and the listing answers.
static MentionIdx *mentionidx(Gui *g, GuiSession *s, Str root, Arena scratch)
{
    MentionIdxs *m = s->midx;
    if (!m) {
        // Kept even when no index can be had (nomem), so that a shortage
        // costs one reservation and not one a frame while the popup is open
        m = alloc<MentionIdxs>(&g->perm);
        m->cur  = -1;
        m->next = -1;
        s->midx = m;
    }
    if (m->nomem) {
        return 0;
    }
    b32 busy = s->view && s->view->busy;
    if (busy != m->wasbusy) {
        m->stale   = m->stale || !busy;  // the turn ended
        m->wasbusy = busy;
    }
    // Another tree's are of no use, served or walking
    for (i32 k = 0; k < 2; k++) {
        if ((k==m->cur || k==m->next) && m->slot[k].root!=root) {
            mentionidxfree(g, &m->slot[k]);
            m->cur  = m->cur ==k ? -1 : m->cur;
            m->next = m->next==k ? -1 : m->next;
        }
    }
    MentionIdx *cur = m->cur>=0 ? &m->slot[m->cur] : 0;
    i64 now = os_ticks(g->os);
    b32 old = !cur || m->stale || now-cur->born > MENTION_TTL;
    if (old && m->next<0 && now>=m->again) {
        i32 k = m->cur==0 ? 1 : 0;
        mentionidxfree(g, &m->slot[k]);
        m->stale = 0;
        if (mentionidxbegin(g, &m->slot[k], root, hostwin(s->host))) {
            m->next = k;
        } else if (!cur) {
            m->nomem = 1;  // no more trying: the listing answers instead
            return 0;
        } else {
            cur->born = now;  // the old one again, for a while
        }
    }
    if (!g->mentionhold) {
        mentionidxput(g, m, scratch);
    }
    return m->cur>=0 ? &m->slot[m->cur] : 0;  // an inline walk is served now
}

// A checklist's hold let go (OP_MENTIONHOLD 0): the walk held is put.
static void mentionidxgo(Gui *g, GuiSession *s, Arena scratch)
{
    if (s->midx) {
        mentionidxput(g, s->midx, scratch);
    }
}

// The window's pool's answer to a walk (GFS_WALK), on the window's
// thread: sorted here and served, the index it replaces let go, and the
// picker drawn again if it is up. One given up or refused (back unset,
// or err) keeps the index served, and none is begun again for
// MENTION_TTL, or MENTION_BUSY when it waited in the queue past FS_STUCK
// (fsbusy); its Region is the pool's when back is unset.
static void mentionwalkdone(Gui *g, FsJob *job, b32 back, Str err)
{
    MentionIdxs *m = 0;
    i32          k = -1;
    for (GuiSession *s = g->sessions; s && k<0; s = s->next) {
        for (i32 i = 0; s->midx && i<2 && k<0; i++) {
            if (s->midx->slot[i].walkid == job->put.id) {
                m = s->midx;
                k = i;
            }
        }
    }
    if (k < 0) {  // not kept: an index abandons its walk as it goes
        if (back) {
            mentionwalkdrop(g->os, job->put.own);
        }
        return;
    }
    MentionIdx *idx = &m->slot[k];
    if (!back) {
        idx->job = 0;  // the pool's, dropped once its worker is out of it
    }
    idx->walkid = 0;
    if (!back || err.len || k!=m->next) {
        mentionidxfree(g, idx);
        m->next  = m->next==k ? -1 : m->next;
        m->again = os_ticks(g->os) + (err==fsbusy ? MENTION_BUSY : MENTION_TTL);
        return;
    }
    mentionidxsort(idx);
    if (m->cur >= 0) {
        mentionidxfree(g, &m->slot[m->cur]);
    }
    m->cur  = k;
    m->next = -1;
    if (g->mentionshown) {
        scheddirty(&g->sched);  // the picker's list is now the index's
    }
}

// A session's indexes go with the session: their reservations must not
// outlive the tree they were made for, and a walk out is abandoned.
static void mentionidxdrop(Gui *g, GuiSession *s)
{
    MentionIdxs *m = s->midx;
    if (m) {
        // Emptied and kept, for the session's next index, as it holds
        // nothing but the two slots
        mentionidxfree(g, &m->slot[0]);
        mentionidxfree(g, &m->slot[1]);
        m->cur   = m->next = -1;
        m->stale = m->nomem = 0;
        m->again = 0;
    }
}

// The row's "attached" mark, and a pick of it attaching nothing: a file is
// in the draft already when an attachment's src, the absolute path
// attachfile took, is this candidate's own.
static void mentionmarkattached(Arena *a, GuiSession *s, Str root, Slice<MentionCand> cands)
{
    for (iz i = 0; i < cands.len; i++) {
        Str abs = join(a, root, cands[i].path);
        for (i32 k = 0; s->att && k < s->att->len; k++) {
            if (s->att->imgs[k].src.len && s->att->imgs[k].src == abs) {
                cands[i].attached = 1;
            }
        }
    }
}

// The picks a query ranks the whole index to, ranked once and then kept:
// the popup asks twice a frame with the same word, and the index holds up to
// 30,000 paths. The cache is the walk's own — a pick is an index into
// w.paths, which stops moving once the walk is done and mentionidxsort has
// run — so it is read only for the generation it was made in, and only for
// the word it was made for. A query longer than a mention can be is ranked
// and not kept, since the buffer holding the word is a mention's own size.
static Slice<MentionPick> mentionidxpicks(Arena *a, MentionIdx *idx, Str query)
{
    Str q = trim(query);
    if (idx->ranked && idx->rankgen==idx->gen &&
        Str{idx->qtext, idx->qlen} == q) {
        return Slice<MentionPick>{idx->picks, idx->npicks, idx->npicks};
    }
    Slice<MentionPick> picks = mentionpicks(a, idx->w->paths, q, MENTION_PICKS, 1);
    idx->ranked = 0;  // what it held is about to be another word's
    if (q.len > MENTION_QUERY) {
        return picks;
    }
    for (iz i = 0; i < picks.len; i++) {
        idx->picks[i] = picks[i];
    }
    idx->npicks  = picks.len;
    idx->qlen    = q.len;
    copybytes(idx->qtext, q.data, q.len);
    idx->rankgen = idx->gen;
    idx->ranked  = 1;
    return picks;
}

// The candidates the index offers: its whole tree, ranked against the typed
// word as a word — the paths in it are whole relative paths already, so
// there is nothing to split as a path here. A query holding '/' still gets
// the ranker's segment discipline (mentionpicks). The empty query ranks
// nothing and keeps the list's order, which mentionidxsort made the newest
// first. The rank itself is mentionidxpicks's: this runs every frame the
// popup is open, and ranking 30,000 paths a frame is not what a frame is
// for.
static void mentionfillindex(Arena *a, Gui *g, GuiSession *s, Str query,
                             MentionSrc *src, MentionIdx *idx)
{
    src->kind      = MENTION_INDEX;
    src->truncated = idx->w->truncated;  // capped, and still the better source
    Slice<MentionPick> picks = mentionidxpicks(a, idx, query);
    Slice<MentionCand> cands = {};
    for (iz i = 0; i < picks.len; i++) {
        iz  k    = (iz)picks[i].i;
        Str path = idx->w->paths[k];
        Str name = basename(path);
        Str dir  = cuttail(path, name.len);
        if (dir.len && dir[dir.len-1] == '/') {
            dir.len--;  // the separator before the name
        }
        cands = push(a, cands, MentionCand{path, name, dir, idx->w->isdirs[k]!=0, 0, {},
                                           picks[i].score});
    }
    mentionmarkattached(a, s, src->root, cands);
    // One line: that the list is the tree's newest files, else how many
    // the query chose out of how many, else which kind of nothing it came
    // to: a query that matched none of a tree that has files, or a tree
    // with no files to match; and, when the walk's caps cut the tree short,
    // that the list is the tree's first paths and not all of them. Not that
    // the walk is still going: an index reaches here only once it is done,
    // and mentionfill says that of the listing that stands in for a walk
    // still going.
    Buf b(a, 64);
    if (!trim(query).len) {
        print(&b, idx->w->paths.len ? Str("recent files") : Str("no files here"));
    } else if (!cands.len) {
        print(&b, idx->w->paths.len ? Str("no files match") : Str("no files here"));
    } else {
        print(&b, (i64)cands.len);
        print(&b, " of ");
        print(&b, (i64)idx->w->paths.len);
    }
    if (idx->w->truncated) {
        print(&b, " \xc2\xb7 index capped");
    }
    src->cands  = cands;
    src->status = finish(&b);
}

// The candidates a typed mention query offers, from one of two sources.
//
// The index (filewalk.cpp) when this machine holds the tree: it is walked by
// a worker of the window's file pool, so no frame waits for a big tree or a
// folder that stopped answering, and ranked against the whole typed word. A
// capped index is still the index: it holds the tree's first FILEWALK_FILES
// paths, which is more than the one folder a listing can reach, so a
// truncated walk does not give way to the listing. The listing is the
// fallback for four cases: a remote host, whose files are not here, an index
// that could not be given memory, the first walk still going (or held by a
// checklist), and a first walk given up or refused.
//
// The listing: one folder of the session's working tree, through dirget.
// The query is split as a path — the folder typed, then the word that
// narrows its entries — always with '/', since a mention is written that
// way on every host and the host's own separator never appears in one.
// That word is the ranker's query (mentionpicks, compose.cpp), so the best
// candidates come first and a folder and a file of one score are ordered by
// its tie-break.
static void mentionfillsrc(Arena *a, Gui *g, GuiSession *s, Str query, MentionSrc *src)
{
    *src = {};
    src->kind = MENTION_NONE;
    View *v    = s->view;
    Str   root = v && v->cwd.len ? v->cwd : s->cwd;
    if (!root.len) {
        src->status = "this session has no folder to list";
        return;
    } else if (sessnofolder(s)) {
        src->status = "this session has no folder of its own";
        return;
    } else if (!s->host || !hostlists(s->host)) {
        src->status = "this host cannot list folders";
        return;
    }
    src->root = root;
    // The index: this machine's tree, and only this machine's, since a
    // remote's files are on the other host
    MentionIdx *idx = s->host->kind!=GH_REMOTE ? mentionidx(g, s, root, *a) : 0;
    if (idx) {
        mentionfillindex(a, g, s, query, src, idx);
        return;
    }
    // The listing: a remote's tree, or one this index could not be given
    // memory for. Not a truncated index's: its 30,000 paths are the tree's
    // own and reach further than one folder's listing does
    PathQuery q = pathquery(query, 0);  // '/' on every host: a mention's rule
    src->qdir   = q.dir;
    src->filter = trim(q.filter);  // ranked and sorted by the same word
    // The folder is joined before the scratch copy is taken: given in the
    // same call, the copy could be made first, and the listing's
    // allocations would land on the joined path, which a holds
    Str      dir = join(a, root, q.dir);
    DirList *l   = dirget(g, *a, s->host, dir, 1);  // files too
    src->kind    = MENTION_LIST;
    if (!l->got) {  // the answer comes in later, through dirlocal or ondir
        src->status = "listing...";
        return;
    } else if (l->err.len) {
        src->status = l->err;
        return;
    }
    // The candidates first, as a token writes them, in the listing's
    // order; the query picks among them and says which came first
    Slice<MentionCand> all = {};
    for (iz i = 0; i < l->ents.len; i++) {
        PathEntry e = l->ents[i];
        if (mentionhidden(e.name, src->filter)) {
            continue;
        }
        Str name = clone(a, e.name);
        Str dir  = src->qdir;
        if (dir.len && dir[dir.len-1]=='/') {
            dir.len--;
        }
        all = push(a, all, MentionCand{concat(a, src->qdir, name), name, dir, e.dir, 0, {}, 0});
    }
    // With no query there is nothing to rank by and the listing's order
    // means nothing, so folders come first; with one, the score decides
    if (!src->filter.len) {
        sort(all.data, all.len, mentionless, *a);
    }
    Slice<Str> paths = {};
    for (iz i = 0; i < all.len; i++) {
        paths = push(a, paths, all[i].path);
    }
    Slice<MentionPick> picks = mentionpicks(a, paths, src->filter, MENTION_PICKS);
    Slice<MentionCand> cands = {};
    for (iz i = 0; i < picks.len; i++) {
        cands = push(a, cands, all[picks[i].i]);
        cands[cands.len-1].score = picks[i].score;
    }
    // One line: how many the query chose, out of how many it chose from;
    // else which kind of nothing it came to: an empty folder, or a query
    // that matched none of the folder's files
    Buf b(a, 64);
    if (!all.len) {
        print(&b, "no files here");
    } else if (!cands.len) {
        print(&b, "no files match");
    } else {
        print(&b, (i64)cands.len);
        if (src->filter.len) {
            print(&b, " of ");
            print(&b, (i64)all.len);
        } else {
            print(&b, cands.len==1 ? Str(" item") : Str(" items"));
        }
    }
    src->cands     = cands;
    src->truncated = l->more;
    if (src->truncated) {
        print(&b, " \xc2\xb7 capped");
    }
    src->status    = finish(&b);
    mentionmarkattached(a, s, root, cands);
}

// Where a drilled-in listing is, as the popup's one line says it: the folder's
// segments, so a pick made inside one is not mistaken for a pick at the root
static Str mentioncrumb(Arena *a, Str qdir)
{
    Buf b(a, qdir.len + 16);
    print(&b, "@ ");
    for (iz from = 0, i = 0; i <= qdir.len; i++) {
        if (i==qdir.len || qdir[i]=='/') {
            if (i > from) {
                print(&b, span(qdir.data+from, qdir.data+i));
                print(&b, " \xe2\x80\xba ");
            }
            from = i + 1;
        }
    }
    return finish(&b);
}

// The sessions an '@' may name from s's draft: its host's, listed there,
// neither archived nor in the Trash, as send_message reaches them, and
// neither a subagent, which its parent speaks for, nor s itself.
static b32 mentionable(GuiSession *s, GuiSession *o)
{
    return o!=s && o->host==s->host && o->listed && !o->gone && !o->archived &&
           !o->trashed && !o->parent.len && o->sid.len;
}

static Str mentiontitle(GuiSession *o)
{
    return o->view && o->view->title.len ? o->view->title : o->title;
}

// What a session's token names it by: its title, which reads as the user
// knows it, unless no token can hold the title or another session has
// it too, and then its id, which send_message takes as well.
static Str mentionsessname(Arena *a, Gui *g, GuiSession *s, GuiSession *o)
{
    Str t = mentiontitle(o);
    if (!t.len || !mentiontext(a, t, 0).len) {
        return o->sid;
    }
    for (GuiSession *p = g->sessions; p; p = p->next) {
        if (p!=o && mentionable(s, p) && mentiontitle(p)==t) {
            return o->sid;
        }
    }
    return t;
}

// A session's order among those a query matched: the higher score, then
// the newer.
static b32 mentionsessless(MentionCand *a, MentionCand *b)
{
    if (a->score != b->score) {
        return a->score > b->score;
    }
    return compare(a->dir, b->dir) > 0;  // the id: its time first
}

// The sessions a typed word matches, by title or id, ranked as files are
// so that the two lists merge by score, a file first on a tie: the files
// are what '@' is mostly for. Not for a bare '@', whose list is the
// recent files, nor a word holding '/', which is a path.
static Slice<MentionCand> mentionsessions(Arena *a, Gui *g, GuiSession *s, Str query)
{
    Slice<MentionCand> r = {};
    Str q = trim(query);
    if (!q.len || cut(q, '/').ok) {
        return r;
    }
    Buf lb(a, q.len);
    for (iz i = 0; i < q.len; i++) {
        putbyte(&lb, lowercase(q[i]));
    }
    Str low = finish(&lb);
    for (GuiSession *o = g->sessions; o; o = o->next) {
        if (!mentionable(s, o)) {
            continue;
        }
        // A title is ranked as one name: a '/' in it is no folder
        Str t  = clone(a, mentiontitle(o));
        for (iz i = 0; i < t.len; i++) {
            t.data[i] = t[i]=='/' ? ' ' : t[i];
        }
        i32 sc = t.len ? mentionscore(t, low) : -1;
        i32 si = mentionscore(o->sid, low);
        sc = si>sc ? si : sc;
        if (sc < 0) {
            continue;
        }
        MentionCand c = {};
        c.path  = mentionsessname(a, g, s, o);
        c.name  = mentiontitle(o).len ? mentiontitle(o) : o->sid;
        c.dir   = mentiontitle(o).len ? o->sid : Str{};
        c.sid   = o->sid;
        c.score = sc;
        r = push(a, r, c);
    }
    sort(r.data, r.len, mentionsessless, *a);
    r.len = r.len<MENTION_SESS ? r.len : (iz)MENTION_SESS;
    return r;
}

// The candidates one typed query offers, with the one line that says what
// they are: where a drilled-in listing is, how many, whether the workspace
// index is still being built — which is why the line speaks of a folder
// rather than of the tree — and, on a host whose files this machine cannot
// read, that the pick carries no contents. A capped index is not one of
// those cases: it answers for itself, and says `index capped` on its own
// line, since it is still the tree's paths and not one folder's.
static void mentionfill(Arena *a, Gui *g, GuiSession *s, Str query, MentionSrc *src)
{
    mentionfillsrc(a, g, s, query, src);
    MentionIdxs *m  = s->midx;
    Str          st = src->status;
    // The index's own state, said here because the listing's line speaks
    // only of the listing: a walk still going is why this is one folder's
    // files and not the tree's
    if (src->kind==MENTION_LIST && m && st.len && m->next>=0) {
        st = concat(a, Str("indexing\xe2\x80\xa6 \xc2\xb7 "), st);
    }
    src->status = st;
    if (src->kind==MENTION_LIST && src->qdir.len && st.len) {
        src->status = concat(a, concat(a, mentioncrumb(a, src->qdir), "\xc2\xb7 "), st);
    }
    if (s->host && s->host->kind==GH_REMOTE && !hostcap(s->host, "files.path") &&
        src->status.len) {  // an engine too old to attach it there
        src->status = concat(a, concat(a, src->status, " \xc2\xb7 "),
                             Str("path only"));
    }
    // The sessions the word matches, merged into the files by score
    Slice<MentionCand> ss = mentionsessions(a, g, s, query);
    if (ss.len) {
        Slice<MentionCand> files = src->cands;
        Slice<MentionCand> all   = {};
        for (iz i = 0, k = 0; i<files.len || k<ss.len;) {
            b32 file = k>=ss.len || (i<files.len && files[i].score>=ss[k].score);
            all = push(a, all, file ? files[i++] : ss[k++]);
        }
        src->cands  = all;
        Buf nb(a, 32);
        print(&nb, (i64)ss.len);
        print(&nb, ss.len==1 ? Str(" session") : Str(" sessions"));
        Str n = finish(&nb);
        src->status = src->status.len ? concat(a, concat(a, src->status, " \xc2\xb7 "), n) : n;
    }
}
// Whether text holds token as a mention of its own, where one may begin:
// the text's start, or after whitespace or '(' as mentionat reads it. A
// file's token ends with the space that follows it, which a message about
// to be sent may have trimmed from its end, so the text's end stands in
// for that space.
static b32 mentionholds(Str text, Str token)
{
    b32 space = token.len && token[token.len-1]==' ';
    iz  n     = space ? token.len-1 : token.len;  // the path, without that space
    for (iz i = 0; n && i+n <= text.len; i++) {
        if (i && !whitespace(text[i-1]) && text[i-1]!='(') {
            continue;  // not where a mention may begin
        }
        if (__builtin_memcmp(text.data+i, token.data, (uz)n)) {
            continue;
        }
        if (!space || i+n==text.len || whitespace(text[i+n])) {
            return 1;
        }
    }
    return 0;
}

// The sessions a message's text mentions, by the token a pick writes, or
// by id: their ids, for the prompt's sessions key. Read from the text
// as it goes, so a token typed by hand counts, and one deleted does not.
static Slice<Str> mentionrefs(Arena *a, Gui *g, GuiSession *s, Str text)
{
    Slice<Str> r = {};
    if (g->nomentions || !s->host || !hostcap(s->host, "sessions.mention") ||
        !cut(text, '@').ok) {
        return r;
    }
    for (GuiSession *o = g->sessions; o && r.len<16; o = o->next) {
        if (!mentionable(s, o)) {
            continue;
        }
        Str named = mentiontext(a, mentionsessname(a, g, s, o), 0);
        Str byid  = mentiontext(a, o->sid, 0);
        if ((named.len && mentionholds(text, named)) || mentionholds(text, byid)) {
            r = push(a, r, o->sid);
        }
    }
    return r;
}

// The prompt's sessions key, if the text mentions any (mentionrefs).
static void mentionputrefs(JsonOut *w, Slice<Str> refs)
{
    if (refs.len) {
        putkey(w, "sessions");
        putarray(w);
        for (iz i = 0; i < refs.len; i++) {
            putstr(w, refs[i]);
        }
        putend(w);
    }
}

// A chip of the draft's for a mention, holding no contents: label is what
// it shows, src the absolute path it names, and with rpath the path the
// host's engine attaches when the message goes (attachnext), else none,
// a path-only chip with why. Null when there is no room for it, which the
// caller says.
static AttImage *mentionchip(Gui *g, GuiSession *s, Str label, Str src, Str rpath, Str why)
{
    if (s->att && s->att->len==countof(s->att->imgs)) {
        return 0;
    }
    Region *r = os_reserve(g->os, 4096 + src.len + rpath.len + why.len);  // its strings'
    if (!r) {
        return 0;
    }
    if (!s->att) {
        s->att = alloc<AttBox>(&g->perm);
    }
    AttImage *m = s->att->imgs + s->att->len++;
    *m = {};
    m->r        = r;
    m->mem      = attarena(r);
    m->label    = clone(m->mem, label);
    m->file     = 1;
    m->mention  = 1;
    m->pathonly = !rpath.len;
    m->src      = clone(m->mem, src);
    m->rpath    = clone(m->mem, rpath);
    m->why      = clone(m->mem, why);
    scheddirty(&g->sched);
    return m;
}

// The note for a mention whose contents do not come: it went as a path.
static Str mentionpathnote(Arena *a, Str name, Str why)
{
    Buf b(a, 96 + name.len + why.len);
    putbyte(&b, '@');
    print(&b, name);
    print(&b, " added as a path \xe2\x80\x94 ");
    print(&b, why);
    print(&b, ", so the agent reads it itself");
    return finish(&b);
}

// Attach a picked file's contents to the draft, once, marked as the
// mention's, so that the send drops it with its token (mentionreconcile).
// Returns the note to show, or empty. A file that cannot be attached still
// gets a path-only chip, so the mention is visibly there, its tooltip says
// its contents did not come, and the note says why. On another machine,
// the host's engine reads the file as the message goes (files.path); an
// engine too old for that gets the path-only chip. The root is worked out
// as mentionfill works it out, so the path attached is the one listed.
static Str mentionattach(Arena *a, Gui *g, GuiSession *s, Str rel)
{
    View *v    = s->view;
    Str   root = v && v->cwd.len ? v->cwd : s->cwd;
    Str   abs  = join(a, root, rel);
    Str   name = basename(rel);
    AttBox *x = s->att;
    for (i32 k = 0; x && k < x->len; k++) {
        if (x->imgs[k].src.len && x->imgs[k].src == abs) {
            return {};  // attached once already: no note, no second chip
        }
    }
    // Why it could not come, in a phrase the note can use: the long prose
    // attachfile gives repeats the file's name and, for a large file, quotes
    // an image cap that has nothing to do with this one
    Str why = attachwhy(s);
    if (why.len && !startswith(why, "At most 10")) {
        return why;  // waiting, or an old engine: no chip
    }
    Str shortwhy = why.len ? Str("ten files already attached") : Str{};
    if (!shortwhy.len && s->host && s->host->kind==GH_REMOTE) {
        if (hostcap(s->host, "files.path") && !hostcap(s->host, "pdf") &&
                (endswith(name, ".pdf") || endswith(name, ".PDF"))) {
            // Not read here, so known only by its name
            shortwhy = "a PDF the engine is too old to read";
        } else if (hostcap(s->host, "files.path")) {
            // Read there when the message goes: a chip that names it
            if (!mentionchip(g, s, name, abs, rel, {})) {
                return "no room for another attachment";
            }
            return {};
        } else {
            shortwhy = "the host's engine is too old to attach it";
        }
    }
    if (!shortwhy.len) {
        why = attachfile(g, s, abs);
        if (!why.len) {
            s->att->imgs[s->att->len-1].mention = 1;
            return {};
        }
        shortwhy = why;
        Read    read = {};
        Region *rr   = os_reserve(g->os, (iz)ATTACH_FILE + 4096);  // for its kind only
        if (rr) {
            Arena ra = newarena(rr);
            read = os_read(g->os, &ra, abs, (iz)ATTACH_FILE + 1);
        }
        b32 pdf = read.err==READ_OK && pdfsniff(read.data) && pdflikely(read.data);
        if (read.err==READ_TOOBIG || (read.err==READ_OK && read.data.len>ATTACH_FILE)) {
            shortwhy = "over 16 MiB";
        } else if (pdf && !hostcap(s->host, "pdf")) {
            shortwhy = "a PDF the engine is too old to read";
        } else if (!pdf && read.err==READ_OK && readbinary(read.data)) {
            shortwhy = "binary";
        }
        if (rr) {
            os_release(g->os, rr);
        }
    }
    // The chip: the file's name, the path it came from, and no contents,
    // so attachnext and the send skip it and drawattached draws it dimmed
    if (!mentionchip(g, s, name, abs, {}, shortwhy)) {
        return concat(a, why.len ? why : shortwhy, " (and no room for its chip)");
    }
    return mentionpathnote(a, name, shortwhy);
}

// Attach the files picked last frame. Not done at the pick: drawprompt
// holds a copy of the frame arena with its allocations live, and attaching
// works in that arena itself, for its scratch and for a failure's words.
// At the frame's start nothing holds a copy, and a frame later is soon
// enough for the chip.
static void mentionlate(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        Str rel = keepstr(&s->mattach);
        if (!rel.len) {
            continue;
        }
        Str why = mentionattach(&g->frame, g, s, rel);
        s->mattach.len = 0;
        if (why.len) {
            setnote(g, s, why, 0, 1);  // not an error: the mention worked, and typing keeps it
        }
    }
}

// Drop the attachments a mention made whose token the message no longer
// holds, so the box and what it sends cannot disagree. The rule: an
// attachment marked as a mention's was made by an '@' token naming its
// src, so a message that no longer holds that token must not carry it;
// the Attach button's, a drop's and a paste's have a src too, but no
// token made them, and they stay. The
// token is compared as mentiontext writes it, not as a bare substring, so
// "@a.cpp" does not keep what "@aa.cpp" attached.
static void mentionreconcile(Gui *g, GuiSession *s, Str text)
{
    View   *v    = s->view;
    Str     root = v && v->cwd.len ? v->cwd : s->cwd;
    AttBox *x    = s->att;
    for (i32 k = (x ? x->len : 0) - 1; k >= 0; k--) {
        AttImage *m = x->imgs + k;
        if (!m->mention) {
            continue;  // a paste's or the Attach button's: no token made it
        }
        iz off = root.len;
        if (!off || !startswith(m->src, root) || off>=m->src.len) {
            continue;  // not under the working tree: no token to look for
        } else if (m->src[off] == '/') {
            off++;     // the separator join put between them
        }
        Str t = mentiontext(&g->frame, cuthead(m->src, off), 0);
        if (t.len && !mentionholds(text, t)) {
            attachremove(g, s, k);
        }
    }
}
