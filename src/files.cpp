// The Files pane: files shown in tabs, a root session's (fileset.cpp),
// each read into a body of its own and drawn as text, highlighted, its
// lines numbered (drawdoc's DocLines), or as Markdown.
//
// A body holds a file as read, in a Region of its own: the text,
// normalized (filenorm), its tokens, highlighted once over the whole of
// it, a lines view whose messages are chunks of FILES_CHUNK lines
// slicing the text, a Markdown view over the same text for a Markdown
// file, and a layout cache of its own, reset past FILES_CACHE as the
// transcripts' is, so that a big file never empties theirs. Bodies
// together are kept within FILES_BUDGET: the least recently drawn goes
// first, at frame start, never the one shown, its tab kept to be read
// again when shown.
//
// Files on this computer are read here, by the GUI, through filestat
// and fileread, which refuse device and network paths before anything
// touches them. Files on other hosts (remotes, containers, WSL) come
// through their engines, fs.read a part at a time (FileFetch), the
// front tab's only, and Open file checks a path there with fs.stat.
//
// A file opens from Open file, or from a link to it (filelinkopen) in
// the transcript, the plan or a Markdown file shown here.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    FBODY_TEXT,
};

enum : iz {
    FILES_REGION  = (iz)512 << 20,  // a body's reservation
    FILES_SCRATCH = (iz)256 << 20,  // a read's, while it is made into a body
};

struct FileBody {
    Region      *r;
    i32          kind;     // FBODY_*
    ViewCache   *c;        // its views' documents and layouts
    DocView     *text;     // a chunk message per FILES_CHUNK lines
    DocView     *md;       // the whole text as Markdown, if a Markdown file
    Str          body;     // normalized, as shown
    Slice<HlTok> toks;     // over body, as far as it is highlighted
    i32          lines;
    i32          cols;     // the widest line's
    i64          size;     // the file's bytes
    i64          mtime;
    u64          sum;      // body's hash
    i32          flags;    // FTEXT_*
    b32          changed;  // it changed while it was read, twice
    b32          fetched;  // read on its host (fs.read), for the checklist
    Str          lang;
    i64          drawn;    // total.frames when last drawn
};

// Whether a file's name says Markdown.
static b32 filemdname(Str path)
{
    Str exts[] = {".md", ".markdown", ".mdx"};
    for (i32 i = 0; i < countof(exts); i++) {
        if (path.len>exts[i].len && equalfold(cuthead(path, path.len-exts[i].len), exts[i])) {
            return 1;
        }
    }
    return 0;
}

// The host of a tab's uid, null if none is (0 is this computer's).
static GuiHost *filehost(Gui *g, u64 uid)
{
    for (GuiHost *h = g->hosts; uid && h; h = h->next) {
        if (h->uid == uid) {
            return h;
        }
    }
    return 0;
}

// What a host is called, for a tab's label and the path's tooltip.
static Str filehostname(Gui *g, Arena *a, u64 uid)
{
    GuiHost *h = filehost(g, uid);
    return h ? hostlabel(g, a, h) : Str("this computer");
}

// A tab failed, saying why. One on another host is tried again only
// once that host has connected again (drawfiles), or on Retry.
static void filefail(Gui *g, FileTab *ft, Str why)
{
    GuiHost *h = filehost(g, ft->host);
    ft->state = FTAB_FAILED;
    ft->err   = why;
    ft->opens = h ? h->opens : 0;
}

// The root whose tabs the pane shows: the shown session's, else the
// window's own, for files opened with no session shown.
static GuiSession *filesroot(Gui *g)
{
    return g->sess ? termroot(g, g->sess) : &g->nosession;
}

// A path's type on this computer, a device or a network path refused
// (STAT_OTHER) before anything touches it: on Windows os_stat opens
// the path, and opening a device can do things.
static Stat filestat(Gui *g, Arena scratch, Str path)
{
    if (devicepath(path, HOST_WINDOWS) || netpath(path, HOST_WINDOWS)) {
        return {0, 0, STAT_OTHER};
    }
    return os_stat(g->os, scratch, path);
}

// At most max bytes of a file on this computer, refused likewise.
static Read fileread(Gui *g, Arena *a, Str path, iz max)
{
    if (devicepath(path, HOST_WINDOWS) || netpath(path, HOST_WINDOWS)) {
        return {{}, 0, READ_NOTFILE};
    }
    return os_readat(g->os, a, path, 0, max);
}

// Why a stat's type is not a file to read, else empty.
static Str filestatwhy(i32 type)
{
    switch (type) {
    case STAT_MISSING: return "No such file";
    case STAT_DIR:     return "That is a folder";
    case STAT_OTHER:   return "Not a regular file";
    }
    return {};
}

// Let a body go: its Region, and whatever names its views.
static void filefree(Gui *g, FileTab *ft)
{
    FileBody *b = ft->body;
    if (!b) {
        return;
    }
    View *views[] = {b->text->v, b->md ? b->md->v : 0};
    for (i32 i = 0; i < countof(views); i++) {
        if (views[i] && g->copiedview==views[i]) {
            g->copiedview = 0;
        }
        if (views[i] && g->fshown.v==views[i]) {
            g->fshown.v = 0;
        }
    }
    if (g->fshowingbody == b) {
        g->fshowing     = 0;  // a body read again, maybe here, is new
        g->fshowingbody = 0;
    }
    os_release(g->os, b->r);
    ft->body  = 0;
    ft->state = ft->state==FTAB_READ ? FTAB_UNREAD : ft->state;
}

// A tab closed: its body freed, to the spares.
static void fetchend(Gui *);

static void filedrop(Gui *g, FileTab *ft)
{
    if (g->ffetch.ft == ft) {
        fetchend(g);  // the tab may be another's next
    }
    filefree(g, ft);
    ft->next     = g->filespare;
    g->filespare = ft;
}

// Close tab i of a root's set.
static void fileclose(Gui *g, FileSet *set, i32 i)
{
    filedrop(g, filesetdrop(set, i));
    scheddirty(&g->sched);
}

// A view of its own over a body's text, made in its arena: no copies,
// its messages' room taken once, so nothing grows past the cache.
static DocView *filedocview(Arena *mem, ViewCache *c, iz nmsgs)
{
    DocView *d = alloc<DocView>(mem);
    View    *v = alloc<View>(mem);
    v->c         = c;
    v->stream    = -1;
    v->prov      = -1;
    v->topsdirty = 1;
    v->state     = LIVE_CLOSED;
    v->msgs      = {alloc<ViewMsg>(mem, nmsgs), 0, nmsgs};
    d->v         = v;
    d->scrollto  = -1;
    return d;
}

// A tab's file, raw, its first FILES_MAX bytes of size, made into a body
// of its own, highlighted, in chunks: the scratch sr is released.
static void filemake(Gui *g, FileTab *ft, Region *sr, Arena s, Str raw, i64 size,
                     i64 mtime, b32 changed)
{
    // Past FILES_MAX: up to its last whole line, and so cut
    b32 cut = size > raw.len;
    if (cut) {
        raw = takehead(raw, filewhole(raw));
    }
    FileText ftx = filenorm(&s, raw, ft->astext, FILES_MAX, FILES_LINES);
    ftx.flags |= cut ? FTEXT_CUT : 0;
    b32 binary = ftx.flags&FTEXT_BINARY && !ft->astext;
    u8  langbuf[32];
    Str lang = binary ? Str{} : hlfile(ft->path, langbuf, countof(langbuf));
    Slice<HlTok> toks = lang.len ? hlsource(&s, lang, ftx.text) : Slice<HlTok>{};

    // Its own Region, sized from what it holds, then the cache after
    iz chunks = ((iz)ftx.lines + FILES_CHUNK - 1) / FILES_CHUNK;
    b32 md    = !binary && filemdname(ft->path);
    iz  need  = ftx.text.len + toks.len*(iz)sizeof(HlTok) + (chunks+2)*(iz)sizeof(ViewMsg) +
                4*(iz)(sizeof(View) + sizeof(DocView)) + (iz)sizeof(FileBody) +
                (iz)sizeof(ViewCache) + ((iz)64<<10);
    Region *r   = os_reserve(g->os, FILES_REGION);
    Arena   mem = newarena(r);
    if (!r || !fits(&mem, need)) {
        if (r) {
            os_release(g->os, r);
        }
        os_release(g->os, sr);
        filefail(g, ft, "There is no memory to show it");
        return;
    }
    FileBody *b = alloc<FileBody>(&mem);
    b->r     = r;
    b->kind  = FBODY_TEXT;
    b->body  = clone(&mem, ftx.text);
    b->body.data = b->body.data ? b->body.data : (u8 *)"";
    b->toks  = {alloc<HlTok>(&mem, toks.len), toks.len, toks.len};
    for (iz i = 0; i < toks.len; i++) {
        b->toks[i] = toks[i];
    }
    b->lines = ftx.lines;
    b->cols  = ftx.cols;
    b->size  = size;
    b->mtime = mtime;
    b->sum   = hash(b->body);
    b->flags = ftx.flags;
    b->changed = changed;
    b->lang  = clone(&mem, lang);
    b->drawn = g->total.frames;
    b->c     = alloc<ViewCache>(&mem);

    // The text, a chunk of lines a message, each its slice of the body
    b->text = filedocview(&mem, b->c, chunks);
    View *v = b->text->v;
    v->body = b->body;
    v->toks = b->toks;
    Str rest = b->body;
    for (i32 line = 1; rest.len;) {
        iz  at = 0;
        i32 n  = 0;
        for (; at<rest.len && n<FILES_CHUNK; at++) {
            n += rest[at] == '\n';
        }
        addlines(v, takehead(rest, at), line);
        line += FILES_CHUNK;
        rest  = cuthead(rest, at);
    }
    if (md) {
        // Made now, small, as nothing may follow the cache in the Region
        b->md = filedocview(&mem, b->c, 1);
        ViewMsg m = {};
        m.role = VIEW_ASSISTANT;
        m.text = b->body;
        m.nl   = countnl(b->body);
        m.md   = 1;
        b->md->v->msgs = push(&b->md->v->meta, b->md->v->msgs, m);
    }

    // The rest of the Region is the cache's, growing at its top
    b->c->arena = mem;
    b->c->base  = mem.beg;
    b->c->max   = FILES_CACHE;
    os_release(g->os, sr);

    ft->body  = b;
    ft->state = FTAB_READ;
    if (ft->mode==FMODE_MARKDOWN && (!b->md || b->body.len>DOC_MAXSIZE)) {
        ft->mode = FMODE_TEXT;  // too big to render, or binary shown as text
    }
}

// Read a tab's file, on this computer, into a body of its own, else say
// why not in the tab.
static void fileload(Gui *g, FileTab *ft)
{
    g->fileloads++;
    scheddirty(&g->sched);
    ft->err = {};
    Region *sr = os_reserve(g->os, FILES_SCRATCH);
    if (!sr) {
        filefail(g, ft, "There is no memory to show it");
        return;
    }
    Arena s  = newarena(sr);
    Stat  st = filestat(g, s, ft->path);
    Str   why = filestatwhy(st.type);
    Read  rd  = {};
    if (!why.len) {
        rd  = fileread(g, &s, ft->path, FILES_MAX);
        why = rd.err==READ_OK       ? Str{} :
              rd.err==READ_NOTFOUND ? Str("No such file") :
              rd.err==READ_NOTFILE  ? Str("Not a regular file") :
              Str("It could not be read");
    }
    if (why.len) {
        os_release(g->os, sr);
        filefail(g, ft, why);
        return;
    }
    filemake(g, ft, sr, s, rd.data, rd.size, st.mtime, 0);
}


// Files on another host, through its engine

// Whether a host is up to show its files, else why not.
static Str filehostwhy(GuiHost *h)
{
    if (!h || !h->r || h->closing || !h->up) {
        return "Its host is not connected";
    } else if (!hostcap(h, "fs")) {
        return "The engine there is too old to show its files: update it";
    }
    return {};
}

// Whether a request still waits for reply id: else REQ_MAX evicted it.
static b32 reqheld(Gui *g, i64 id)
{
    for (i32 i = 0; i < REQ_MAX; i++) {
        if (g->reqs[i].id == id) {
            return 1;
        }
    }
    return 0;
}

// Ask for the fetch's next part.
static void fsrequest(Gui *g)
{
    FileFetch *f = &g->ffetch;
    GuiHost   *h = filehost(g, f->host);
    if (!h) {
        return;
    }
    i64   want = FILES_MAX - f->off;
    Arena a    = newarena(g->cmdr);
    Buf   b(&a, 256 + 6*f->ft->path.len);
    EvOut w = cmdopen(g, &b, CMD_FSREAD, 0, h);
    putkey(&w.json, "path");
    putstr(&w.json, f->ft->path);
    putkey(&w.json, "off");
    putint(&w.json, f->off);
    putkey(&w.json, "max");
    putint(&w.json, want<FILES_PART ? want : (i64)FILES_PART);
    f->id = g->nextid;
    cmdsend(g, &w, a);
}

// The fetch let go, its tab as it is.
static void fetchend(Gui *g)
{
    FileFetch *f = &g->ffetch;
    if (f->r) {
        os_release(g->os, f->r);
    }
    *f = {};
}

// The fetch abandoned: its tab, still waiting for it, read again later.
static void fetchdrop(Gui *g)
{
    FileFetch *f = &g->ffetch;
    if (f->ft && f->ft->serial==f->serial && f->ft->state==FTAB_READING) {
        f->ft->state = FTAB_UNREAD;
    }
    fetchend(g);
}

// The fetch failed, saying why in its tab, which is tried again once
// its host has connected again.
static void fetchfail(Gui *g, Str why)
{
    filefail(g, g->ffetch.ft, why);
    fetchend(g);
    scheddirty(&g->sched);
}

// Start reading a tab's file on another host, as its tab shows: one
// fetch at a time, the last given up. With its host down, the tab waits
// (filesnote says so); with an engine there too old, it fails.
static void filefetch(Gui *g, FileTab *ft)
{
    GuiHost *h   = filehost(g, ft->host);
    Str      why = filehostwhy(h);
    if (why.len && h && h->r && !h->closing && h->up) {
        filefail(g, ft, why);  // too old, until updated
        return;
    } else if (why.len) {
        return;  // until it connects
    }
    fetchdrop(g);
    FileFetch *f = &g->ffetch;
    f->r = os_reserve(g->os, FILES_SCRATCH);
    if (!f->r) {
        filefail(g, ft, "There is no memory to show it");
        return;
    }
    f->mem    = newarena(f->r);
    f->buf    = allocbytes(&f->mem, FILES_MAX);
    f->ft     = ft;
    f->serial = ft->serial;
    f->host   = ft->host;
    f->size   = -1;
    ft->err   = {};
    ft->state = FTAB_READING;
    fsrequest(g);
    scheddirty(&g->sched);
}

// Each frame the pane is drawn, for the front tab ft: a fetch for
// another given up, and one whose request slot went (REQ_MAX) asked
// for again.
static void fetchtick(Gui *g, FileTab *ft)
{
    FileFetch *f = &g->ffetch;
    if (f->ft && (f->ft!=ft || f->ft->serial!=f->serial || f->ft->state!=FTAB_READING)) {
        fetchdrop(g);
    } else if (f->ft && f->id && !reqheld(g, f->id)) {
        fsrequest(g);
    }
}

static void fileaskstat(Gui *);

// Its host connected again: the part in flight asked for again, as its
// reply may have gone with the link, and Open file's check.
static void filefetchagain(Gui *g, GuiHost *h)
{
    FileFetch *f = &g->ffetch;
    if (f->ft && f->id && f->host==h->uid) {
        fsrequest(g);
    }
    if (g->fask.statid && g->fask.host==h->uid) {
        fileaskstat(g);
    }
}

// Its host's link ended (Disconnect, quitting): a reply in flight from
// it is gone, unasked for, so the fetch is dropped, to read again once
// it is back, and Open file's check, at it, fails.
static void filehostend(Gui *g, GuiHost *h)
{
    if (g->ffetch.ft && g->ffetch.host==h->uid) {
        fetchdrop(g);
    }
    if (g->fask.statid && g->fask.host==h->uid) {
        g->fask.statid = 0;
        g->fask.p.err  = filehostwhy(0);
    }
}

// An engine's reason, as a tab says it.
static Str fswhy(Str err)
{
    return err=="no such file"       ? Str("No such file") :
           err=="that is a folder"   ? Str("That is a folder") :
           err=="not a regular file" ? Str("Not a regular file") :
                                       Str("It could not be read");
}

// A part of the fetch's file came: the next asked for, or once all is
// here (or FILES_MAX), made into a body. A size or mtime changed reads
// it again from the start once; changed again, what came is kept.
static void onfsread(Gui *g, Arena scratch, Json *j, b32 ok, Str err)
{
    FileFetch *f  = &g->ffetch;
    FileTab   *ft = f->ft;
    f->id = 0;
    if (!ft || ft->serial!=f->serial || ft->state!=FTAB_READING) {
        fetchend(g);
        return;
    }
    if (!ok && err=="the reply was lost: try again" && f->lost<3) {
        f->lost++;
        fsrequest(g);
        return;
    } else if (!ok && filehostwhy(filehost(g, f->host)).len) {
        fetchdrop(g);  // its host went: read when it is back
        scheddirty(&g->sched);
        return;
    } else if (!ok) {
        fetchfail(g, fswhy(err));
        return;
    }
    Unbase64 u     = unbase64(&scratch, jsonstr(jsonget(j, "data")));
    i64      size  = jsonint(jsonget(j, "size"), -1);
    i64      mtime = jsonint(jsonget(j, "mtime"), 0);
    i64      next  = jsonint(jsonget(j, "off"), -1);
    b32      more  = jsonint(jsonget(j, "more"), 0) != 0;
    if (!u.ok || size<0 || u.data.len>FILES_PART) {
        fetchfail(g, "It could not be read");
        return;
    }
    if (f->size < 0) {
        f->size  = size;
        f->mtime = mtime;
    }
    b32 changed = size!=f->size || mtime!=f->mtime || next!=f->off+u.data.len;
    if (changed && !f->again) {
        f->again = 1;  // from the start, once
        f->off   = 0;
        f->size  = -1;
        fsrequest(g);
        return;
    } else if (changed) {
        f->changed = 1;
        more       = 0;  // what came is kept
    } else {
        iz n = u.data.len<FILES_MAX-f->off ? u.data.len : (iz)(FILES_MAX-f->off);
        copybytes(f->buf+f->off, u.data.data, n);
        f->off += n;
    }
    if (more && u.data.len && f->off<FILES_MAX) {
        fsrequest(g);
        return;
    }

    // All here: its body, from the scratch past what came
    Region *r = f->r;
    Arena   s = f->mem;
    Str     raw = {f->buf, (iz)f->off};
    i64     total = f->changed ? f->off : f->size;
    b32     twice = f->changed;
    i64     when  = f->mtime;
    f->r = 0;
    fetchend(g);
    g->fileloads++;
    scheddirty(&g->sched);
    filemake(g, ft, r, s, raw, total>raw.len ? total : raw.len, when, twice);
    if (ft->body) {
        ft->body->fetched = 1;
    }
}

// Each frame, at its start: the bodies' caches reset when past their
// size, then bodies let go, least recently drawn first, until all fit
// in FILES_BUDGET, but never the one shown.
static void filesframe(Gui *g)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    iz          total   = 0;
    for (i32 k = 0; k < countof(roots); k++) {
        for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                FileBody *b = s->files.tabs[i]->body;
                if (b) {
                    cachereset(b->c, 0);
                    total += b->r->committed;
                }
            }
        }
    }
    GuiSession *shown = filesroot(g);
    FileTab    *front = filesetfront(&shown->files);
    while (total > FILES_BUDGET) {
        FileTab *old = 0;
        for (i32 k = 0; k < countof(roots); k++) {
            for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
                for (i32 i = 0; i < s->files.n; i++) {
                    FileTab *ft = s->files.tabs[i];
                    if (ft->body && ft!=front && (!old || ft->body->drawn<old->body->drawn)) {
                        old = ft;
                    }
                }
            }
        }
        if (!old) {
            break;
        }
        total -= old->body->r->committed;
        filefree(g, old);
    }
}

// Each body's layouts made again, the font's size having changed.
static void filesfont(Gui *g)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    for (i32 k = 0; k < countof(roots); k++) {
        for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                FileBody *b = s->files.tabs[i]->body;
                if (b) {
                    cachereset(b->c, 1);
                    b->text->v->topsdirty = 1;
                    if (b->md) {
                        b->md->v->topsdirty = 1;
                    }
                }
            }
        }
    }
}

// Each frame: the tabs of sessions deleted or put in the Trash go.
static void filesweep(Gui *g)
{
    for (GuiSession *s = g->sessions; s; s = s->next) {
        if (s->gone || s->trashed) {
            while (s->files.n) {
                fileclose(g, &s->files, s->files.n-1);
            }
        }
    }
}

// As the window goes to the background: every body let go, the tabs
// kept, to be read again when shown.
static void filesrelease(Gui *g)
{
    fetchdrop(g);  // a read in flight too: read again when shown
    GuiSession *roots[] = {&g->nosession, g->sessions};
    for (i32 k = 0; k < countof(roots); k++) {
        for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                filefree(g, s->files.tabs[i]);
            }
        }
    }
    g->fshowing = 0;
}

// Show a file in the Files pane, in the tab set of session s's root, or
// the window's own with s null: its tab, made if it has none, in front;
// at lines line0 to line1 (1-based; 0 for none), banded and scrolled
// to, in the text view; else at the heading section, a Markdown file in
// Markdown. The path is absolute and normalized, on the host of uid
// host (0 for this computer). Opening a file again goes there again.
static FileTab *filesopen(Gui *g, GuiSession *s, u64 host, Str path, i32 line0, i32 line1,
                          Str section)
{
    GuiSession *root = s ? termroot(g, s) : &g->nosession;
    FileSet    *set  = &root->files;
    GuiHost    *h    = filehost(g, host);
    b32         win  = h ? hostwin(h) : (b32)HOST_WINDOWS;
    i32         i    = filesetfind(set, host, path, win);
    FileTab    *ft   = 0;
    if (i >= 0) {
        ft = set->tabs[i];
        filesetshow(set, i);
    } else {
        ft = g->filespare;
        if (ft) {
            g->filespare = ft->next;
        } else {
            ft = alloc<FileTab>(&g->perm);
        }
        // A spare's path buffer is used again where it has room
        Str was = ft->path;
        iz  cap = ft->pathcap;
        *ft = {};
        if (cap >= path.len) {
            copybytes(was.data, path.data, path.len);
            ft->path    = {was.data, path.len};
            ft->pathcap = cap;
        } else {
            ft->path    = clone(&g->perm, path);
            ft->pathcap = path.len;
        }
        ft->serial = ++g->fileserial;
        ft->host   = host;
        ft->win    = win;
        ft->wrap   = g->filewrap;
        ft->mode   = filemdname(path) ? FMODE_MARKDOWN : FMODE_TEXT;
        FileTab *gone = filesetadd(set, ft);
        if (gone) {
            filedrop(g, gone);
        }
    }
    if (line0) {
        ft->mode   = FMODE_TEXT;  // a line is the text's
        ft->band0  = line0;
        ft->band1  = line1>=line0 ? line1 : line0;
        ft->jump   = line0;
        ft->jumped = 0;
    } else if (section.len && filemdname(path)) {
        // A heading is the Markdown's, where it can show
        FileBody *b = ft->body;
        b32 mdok = !b || (b->md && b->body.len<=DOC_MAXSIZE);
        ft->mode = mdok ? FMODE_MARKDOWN : ft->mode;
    }
    iz n = section.len<FILES_SECTION ? section.len : (iz)FILES_SECTION;
    copybytes(ft->section, section.data, n);
    ft->sectionlen = (i32)n;
    if (ft->state == FTAB_FAILED) {
        ft->state = FTAB_UNREAD;  // try again
    }
    setpane(g, PANE_FILES, 1);
    scheddirty(&g->sched);
    return ft;
}


// Links to files (help.cpp's DEST_FILE), in the transcript, Plan and
// Markdown files: resolved as asked (filepath, unasked 0) from where
// the link is (LinkFrom), then shown in the tabs of its session's root,
// on its host: one on another computer is read there (fs.read).

// Where links in session s's text are, relative paths from base: its
// root's tabs, on its host. With no session, nowhere.
static LinkFrom filelinkfrom(GuiSession *s, Str base)
{
    LinkFrom r = {};
    if (s) {
        r.s     = s;
        r.host  = s->host->kind==GH_REMOTE ? s->host->uid : 0;
        r.base  = base;
        r.win   = hostwin(s->host);
        r.opens = 1;
    }
    return r;
}

// A path as a link's tooltip names it: below base relative to it, else
// in its home as "~".
static Str filelinkshow(Arena *a, Str path, Str base, Str home, b32 win)
{
    Str root = base.len ? normpath(a, base, ".", win) : Str{};
    for (; root.len>1 && issep(root[root.len-1], win); root.len--) {}
    if (root.len && path.len>root.len+1 && issep(path[root.len], win)) {
        Str head = takehead(path, root.len);
        if (win ? equalfold(head, root) : head==root) {
            return cuthead(path, root.len+1);
        }
    }
    return tildeform(a, path, home);
}

struct FileLink {
    Str path;  // absolute and normalized, else null
    Str show;  // as a tooltip names it
    Str err;   // why it names no file, else null
    b32 far;   // on another computer, read through its engine
};

// The file a link names, from where it is.
static FileLink filelinkget(Gui *g, Arena *a, Dest *d, LinkFrom *from)
{
    FileLink r    = {};
    GuiHost *h    = filehost(g, from->host);
    Str      home = h ? h->home : g->main ? g->main->home : Str{};
    if (d->here) {
        r.path = from->file;
        r.err  = r.path.len ? Str{} : Str("its heading is of no file shown here");
    } else {
        FilePath p = filepath(a, d->file, from->base, home, from->win, 0);
        r.path = p.path;
        r.err  = p.err;
    }
    if (r.path.len) {
        r.show = filelinkshow(a, r.path, from->base, home, from->win);
        r.far  = from->host != 0;
    }
    return r;
}

// Where a link to a file goes, in a tooltip: "Files → PATH:N", else
// why it opens nothing.
static Str filelinktip(Gui *g, Arena *a, Str link, Dest *d, LinkFrom *from)
{
    if (!from || !from->opens) {
        return concat(a, link, " (a link to a file opens nothing here)");
    }
    FileLink f = filelinkget(g, a, d, from);
    Buf      b(a, link.len + f.show.len + f.err.len + d->section.len + 64);
    print(&b, "Files \xe2\x86\x92 ");
    if (f.err.len) {
        print(&b, link);
        print(&b, ": ");
        print(&b, f.err);
        return finish(&b);
    }
    print(&b, f.show);
    FileRef *r = &d->file;
    if (!d->here && r->line0) {
        putbyte(&b, ':');
        print(&b, (i64)r->line0);
        if (r->line1 > r->line0) {
            putbyte(&b, '-');
            print(&b, (i64)r->line1);
        }
    }
    Str section = d->here ? d->section : r->section;
    if (section.len) {
        print(&b, " \xe2\x80\xba ");
        print(&b, section);
    }
    if (f.far) {
        print(&b, ", on ");
        print(&b, filehostname(g, a, from->host));
    }
    return finish(&b);
}

// Show a link's file, as filesopen does; its tab, if open already, may
// have changed since it was read.
static void filelinkgo(Gui *g, GuiSession *s, u64 host, Str path, b32 win,
                       i32 line0, i32 line1, Str section)
{
    GuiSession *root = s ? termroot(g, s) : &g->nosession;
    i32         i    = filesetfind(&root->files, host, path, win);
    if (i >= 0) {
        root->files.tabs[i]->stale = 1;
    }
    filesopen(g, s, host, path, line0, line1, section);
}

// A link to a file clicked: shown in Files, at its lines or heading.
// The arena is the one d was made in, past it.
static void filelinkopen(Gui *g, Arena *a, Dest *d, LinkFrom *from)
{
    if (!from || !from->opens) {
        return;
    }
    FileLink f = filelinkget(g, a, d, from);
    if (!f.path.len) {
        return;
    }
    if (d->here) {
        filelinkgo(g, from->s, from->host, f.path, from->win, 0, 0, d->section);
    } else {
        FileRef *r = &d->file;
        filelinkgo(g, from->s, from->host, f.path, from->win, r->line0, r->line1, r->section);
    }
}

// A right-click in text, on link (empty for none): the menu's file
// link, as it resolves now, kept for as long as the menu is open.
static void filelinkmark(Gui *g, Str link, LinkFrom *from)
{
    LinkMenu *m = &g->linkmenu;
    *m = {};
    Arena a = g->frame;
    Dest  d = helpdest(&a, link);
    if (!link.len || d.kind!=DEST_FILE || !from || !from->opens) {
        return;
    }
    FileLink f = filelinkget(g, &a, &d, from);
    if (f.path.len > countof(m->path)) {
        f = {{}, {}, "its path is too long", 0};
    }
    m->on    = 1;
    m->s     = from->s;
    m->host  = from->host;
    m->win   = from->win;
    m->line0 = d.here ? 0 : d.file.line0;
    m->line1 = d.here ? 0 : d.file.line1;
    copybytes(m->path, f.path.data, f.path.len);
    m->pathlen = (i32)f.path.len;
    Str section = d.here ? d.section : d.file.section;
    iz  n = section.len<FILES_SECTION ? section.len : (iz)FILES_SECTION;
    copybytes(m->section, section.data, n);
    m->sectionlen = (i32)n;
    Str why = f.err;
    n = why.len<countof(m->why) ? why.len : countof(m->why);
    copybytes(m->why, why.data, n);
    if (n) {
        m->why[0] = (u8)(why[0]>='a' && why[0]<='z' ? why[0]-0x20 : why[0]);
    }
    m->whylen = (i32)n;
}

// The text's context menu's first items, for a file link right-clicked:
// Open in Files and Copy path, or why not.
static void filelinkitems(Gui *g)
{
    LinkMenu *m = &g->linkmenu;
    if (!m->on) {
        return;
    }
    Arena a    = g->frame;
    Str   path = {m->path, m->pathlen};
    Str   why  = {m->why, m->whylen};
    if (ImGui::MenuItem("Open in Files", 0, false, !why.len)) {
        filelinkgo(g, m->s, m->host, path, m->win, m->line0, m->line1,
                   {m->section, m->sectionlen});
    }
    itemnamed(g, "link open");
    if (why.len) {
        whytip(why);
    } else {
        itemtip("Show the file in the Files pane");
    }
    if (ImGui::MenuItem("Copy path", 0, false, path.len>0)) {
        ImGui::SetClipboardText(cstr0(&a, path));
    }
    itemnamed(g, "link copy path");
    if (!path.len) {
        whytip(why);
    } else {
        itemtip("Put the file's path on the clipboard");
    }
    ImGui::Separator();
}


// Open file...: a path typed in a modal of its own (drawpathask), on
// the shown session's host, its answer TASK_OPENFILE. On another host
// the path is checked there (fs.stat) first.

// The host Open file opens files on: the shown session's, else this
// computer (null).
static GuiHost *fileaskhost(Gui *g)
{
    GuiSession *s = g->sess;
    return s && s->host->kind==GH_REMOTE ? s->host : 0;
}

// Why Open file can't be used now, else empty.
static Str fileaskwhy(Gui *g)
{
    GuiHost *h = fileaskhost(g);
    if (!g->main) {
        return "No engine";
    } else if (h) {
        return filehostwhy(h);
    }
    return {};
}

static void fileaskopen(Gui *g)
{
    if (fileaskwhy(g).len || g->fask.p.shown) {
        return;
    }
    Arena       a = g->frame;
    GuiSession *s = g->sess;
    Str         at = "~/";
    if (s && termcwd(s).len && !sessnofolder(s)) {
        Str cwd = homeshow(&a, s->host, termcwd(s));
        b32 sep = cwd.len && issep(cwd[cwd.len-1], hostwin(s->host));
        at = sep ? cwd : concat(&a, cwd, hostwin(s->host) ? Str("\\") : Str("/"));
    }
    PathAsk *p = &g->fask.p;
    p->kind  = ASK_OPEN;
    p->type  = TASK_OPENFILE;
    p->id++;
    p->title = "Open file";
    p->open  = 1;
    fieldput(p->buf, at);
    GuiHost *h = fileaskhost(g);
    g->fask.host   = h ? h->uid : 0;
    g->fask.statid = 0;
    g->fask.stated = 0;
    g->fask.close  = 0;
    scheddirty(&g->sched);
}

// A path typed in Open file, maybe with a line or range after it
// (path:N, path:A-B), as fileref takes it: the file's path resolved
// against the shown session's folder, else why not, its first letter
// a capital, for the modal.
static FilePath fileasked(Gui *g, Arena *a, Str text, FileRef *ref)
{
    GuiSession *s    = g->sess;
    GuiHost    *far  = filehost(g, g->fask.host);
    GuiHost    *h    = far ? far : g->main;
    b32         win  = far ? hostwin(far) : (b32)HOST_WINDOWS;
    Str         base = s && !sessnofolder(s) && (!far || s->host==far) ? termcwd(s) : Str{};
    *ref = fileref(a, text);
    FilePath p = text.len ? filepath(a, *ref, base, h ? h->home : Str{}, win, 0)
                          : FilePath{{}, "type a path"};
    if (p.err.len) {
        iz  n   = p.err.len<countof(g->fask.err) ? p.err.len : countof(g->fask.err);
        copybytes(g->fask.err, p.err.data, n);
        g->fask.err[0] = (u8)(p.err[0]>='a' && p.err[0]<='z' ? p.err[0]-0x20 : p.err[0]);
        p.err = {g->fask.err, n};  // kept past the frame, for the modal
        return p;
    } else if (g->fask.host) {
        return p;  // checked there (fileaskok)
    }
    Str why = filestatwhy(filestat(g, *a, p.path).type);
    if (why.len) {
        p.err  = why;
        p.path = {};
    }
    return p;
}

// Open file's answer, as drawpathask posted it: "LINE0 LINE1 PATH",
// then a line with its section, if any.
static Str fileanswerput(Arena *a, FilePath p, FileRef ref)
{
    Buf b(a, p.path.len + ref.section.len + 32);
    print(&b, (i64)ref.line0);
    putbyte(&b, ' ');
    print(&b, (i64)ref.line1);
    putbyte(&b, ' ');
    print(&b, p.path);
    putbyte(&b, '\n');
    print(&b, ref.section);
    return finish(&b);
}

// Ask the host whether Open file's path, kept as its answer, is a file.
static void fileaskstat(Gui *g)
{
    FileAsk *f = &g->fask;
    GuiHost *h = filehost(g, f->host);
    Str      why = filehostwhy(h);
    if (why.len) {
        f->statid = 0;
        f->p.err  = why;
        return;
    }
    Str   answer = {f->answer, f->answerlen};
    Cut   pl     = cut(cut(cut(answer, ' ').tail, ' ').tail, '\n');
    Arena a      = newarena(g->cmdr);
    Buf   b(&a, 256 + 6*pl.head.len);
    EvOut w = cmdopen(g, &b, CMD_FSSTAT, 0, h);
    putkey(&w.json, "paths");
    putarray(&w.json);
    putstr(&w.json, pl.head);
    putend(&w.json);
    f->statid = g->nextid;
    cmdsend(g, &w, a);
}

// Open file's OK: the path typed, its answer posted, else why not. On
// another host, its answer waits for the check there, the modal open,
// saying "Checking...".
static Str fileaskok(Gui *g, Str text)
{
    Arena    a   = g->frame;
    FileRef  ref = {};
    FilePath p   = fileasked(g, &a, text, &ref);
    if (p.err.len) {
        return p.err;
    }
    Str answer = fileanswerput(&a, p, ref);
    if (!g->fask.host) {
        mailpost(g->tasks, TASK_OPENFILE, g->fask.p.id, answer, 1);
        return {};
    } else if (answer.len > countof(g->fask.answer)) {
        return "Its path is too long";
    }
    copybytes(g->fask.answer, answer.data, answer.len);
    g->fask.answerlen = answer.len;
    fileaskstat(g);
    return g->fask.statid ? Str{} : g->fask.p.err;
}

// Each frame Open file is up: its check asked again where its request
// slot went (REQ_MAX).
static void fileaskcheck(Gui *g)
{
    if (g->fask.statid && !reqheld(g, g->fask.statid)) {
        fileaskstat(g);
    }
}

// The host's answer to Open file's check: a file opens, else the modal
// says why not.
static void onfsstat(Gui *g, Arena, Json *j, b32 ok, Str err)
{
    FileAsk *f = &g->fask;
    f->statid = 0;
    f->stated = 1;
    Str type = jsonstr(jsonget(jsonat(jsonget(j, "stats"), 0), "type"));
    Str why  = !ok              ? err :
               type == "file"   ? Str{} :
               type == "dir"    ? Str("That is a folder") :
               type == "missing" ? Str("No such file") :
               type == "bad"    ? Str("It cannot be shown") :
                                  Str("Not a regular file");
    if (!why.len) {
        mailpost(g->tasks, TASK_OPENFILE, f->p.id, {f->answer, f->answerlen}, 1);
        f->close = 1;
    } else {
        iz n = why.len<countof(f->err) ? why.len : countof(f->err);
        copybytes(f->err, why.data, n);
        f->err[0] = (u8)(why[0]>='a' && why[0]<='z' ? why[0]-0x20 : why[0]);
        f->p.err  = {f->err, n};
    }
    scheddirty(&g->sched);
}

static void fileanswer(Gui *g, Str data)
{
    Cut l0 = cut(data, ' ');
    Cut l1 = cut(l0.tail, ' ');
    Cut pl = cut(l1.tail, '\n');
    Int a  = parseint(l0.head);
    Int b  = parseint(l1.head);
    if (!l0.ok || !l1.ok || !a.ok || !b.ok || !pl.head.len) {
        return;
    }
    filesopen(g, g->sess, g->fask.host, pl.head, (i32)a.val, (i32)b.val, pl.tail);
}


// The pane

struct FileBar {
    i32 closed;  // the tab to close, else -1
    b32 others;  //   and the others instead
    b32 all;     //   or all of them
    i32 reload;  // the tab to read again, else -1
};

// The tab bar over the file, modeled on the Terminal's (termtabs): it
// selects its tab when front changed here (frontset), else front
// follows the tab picked, which counts as opening it, and the set
// follows the bar's order. Each tab's context menu closes, copies its
// path or reads it again; "+" is Open file.
static FileBar filetabs(Gui *g, GuiSession *root)
{
    FileBar  r   = {-1, 0, 0, -1};
    FileSet *set = &root->files;
    if (g->filebarroot != root) {
        g->filebarroot = root;  // another root: its own front, selected
        set->frontset  = set->n > 0;
    }
    ImGuiTabBarFlags bf = ImGuiTabBarFlags_Reorderable |
                          ImGuiTabBarFlags_FittingPolicyShrink;
    if (!ImGui::BeginTabBar("##filetabs", bf)) {
        return r;
    }
    Arena   a = g->frame;
    Str    *hosts = alloc<Str>(&a, set->n);
    for (i32 i = 0; i < set->n; i++) {
        hosts[i] = filehostname(g, &a, set->tabs[i]->host);
    }
    Measure m = {};
    m.advance = widgetadvance;
    Str    *labels = filelabels(&a, set, hosts, 240*g->scale, &m);
    ImGuiID ids[FILES_TABS] = {};
    for (i32 i = 0; i < set->n; i++) {
        FileTab          *ft    = set->tabs[i];
        bool              open  = true;
        ImGuiTabItemFlags flags = 0;
        if (set->frontset && i==set->front) {
            flags |= ImGuiTabItemFlags_SetSelected;
        }
        Buf b(&a, labels[i].len + 32);
        print(&b, labels[i]);
        print(&b, "###ft");
        print(&b, (i64)ft->serial);
        b32 shown = ImGui::BeginTabItem(cstr0(&a, finish(&b)), &open, flags);
        ids[i] = ImGui::GetItemID();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            tipnow("%s", cstr0(&a, ft->path));
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
            r.closed = i;
        }
        Buf nb(&a, 32);
        print(&nb, "file tab ");
        print(&nb, (i64)i + 1);
        itemnamed(g, finish(&nb));
        if (ImGui::BeginPopupContextItem()) {
            if (menutip(g, &a, "Close", {}, "Close this tab")) {
                r.closed = i;
            }
            Str one = set->n>1 ? Str{} : Str("No other tab is open");
            if (menutip(g, &a, "Close others", one, "Close every tab but this one")) {
                r.closed = i;
                r.others = 1;
            }
            if (menutip(g, &a, "Close all", {}, "Close every tab")) {
                r.closed = i;
                r.all    = 1;
            }
            ImGui::Separator();
            if (menutip(g, &a, "Copy path", {}, "Put the file's path on the clipboard")) {
                ImGui::SetClipboardText(cstr0(&a, ft->path));
            }
            Str farwhy = ft->host ? filehostwhy(filehost(g, ft->host)) : Str{};
            if (menutip(g, &a, "Reload", farwhy, "Read the file again")) {
                r.reload = i;
            }
            ImGui::EndPopup();
        }
        if (shown) {
            ImGui::EndTabItem();
        }
        if (!open && r.closed<0) {
            r.closed = i;
        }
    }
    Str why = fileaskwhy(g);
    ImGui::BeginDisabled(why.len > 0);
    if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                  ImGuiTabItemFlags_NoTooltip)) {
        fileaskopen(g);
    }
    ImGui::EndDisabled();
    if (why.len) {
        whytip(why);
    } else {
        itemtip("Open file... (%s)", cmdkey(g, &a, "O"));
    }
    itemnamed(g, "file tab +");

    // The bar's order and selection back into the set
    ImGuiTabBar *bar = ImGui::GetCurrentTabBar();
    FileTab     *order[FILES_TABS];
    i32          norder = 0;
    for (i32 k = 0; k < bar->Tabs.Size; k++) {
        for (i32 i = 0; i < set->n; i++) {
            if (ids[i] && bar->Tabs[k].ID==ids[i] && norder<FILES_TABS) {
                order[norder++] = set->tabs[i];
            }
        }
    }
    FileTab *selected = 0;
    for (i32 i = 0; i < set->n; i++) {
        if (ids[i] && ids[i]==bar->SelectedTabId) {
            selected = set->tabs[i];
        }
    }
    if (set->frontset) {
        if (set->n && selected==set->tabs[set->front]) {
            set->frontset = 0;
        }
    } else if (selected && selected!=set->tabs[set->front]) {
        for (i32 i = 0; i < set->n; i++) {
            if (set->tabs[i] == selected) {
                filesetshow(set, i);  // picked: as opened
                set->frontset = 0;
            }
        }
        scheddirty(&g->sched);
    }
    filesetorder(set, order, norder);
    ImGui::EndTabBar();
    return r;
}

// What the pane says with no file open.
static void filesempty(Gui *g)
{
    Arena a = g->frame;
    ImGui::TextDisabled("No file is open");
    Str why = fileaskwhy(g);
    ImGui::BeginDisabled(why.len > 0);
    if (ImGui::Button("Open file...")) {
        fileaskopen(g);
    }
    ImGui::EndDisabled();
    itemnamed(g, "files open");
    if (why.len) {
        whytip(why);
    } else {
        itemtip("Type a file's path, with :N after it for a line (%s)", cmdkey(g, &a, "O"));
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("A file opened here, or by a click on a link to it in the "
                        "transcript, the plan or a Markdown file, shows as text, its "
                        "lines numbered, or as Markdown, in a tab of the session shown.");
    ImGui::PopTextWrapPos();
}

// A note over the file, dim, or in red for an error.
static void filenote(Gui *g, Str s, b32 bad)
{
    Arena a = g->frame;
    for (iz i = 0; i<s.len && g->fnoteslen<countof(g->fnotes); i++) {
        g->fnotes[g->fnoteslen++] = s[i];
    }
    if (g->fnoteslen < countof(g->fnotes)) {
        g->fnotes[g->fnoteslen++] = '\n';
    }
    ImGui::PushStyleColor(ImGuiCol_Text, bad ? g->pal->bad
                                             : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(cstr0(&a, s));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// The line a Markdown view's top shows, 0-based, and how far past it:
// its anchor's offset into the text, counted in lines.
static void filemdtop(Gui *g, FileBody *b, i32 *top, f32 *dy)
{
    DocView *d = b->md;
    View    *v = d->v;
    *top = 0;
    *dy  = 0;
    if (!d->win || v->lastw<=0 || !v->msgs.len) {
        return;
    }
    f32    pad = 4*g->scale;
    Anchor an  = viewanchor(v, &g->measure, v->lastw, g->scale, pad, d->win->Scroll.y);
    if (an.msg>=0 && an.off>=0) {
        *top = (i32)countnl(takehead(b->body, an.off));
        *dy  = (f32)an.dy;
    }
}

// Scroll a Markdown view to a line, 0-based, at width: the place its
// first character is laid out.
static void filemdgo(Gui *g, FileBody *b, i32 line, f32 dy)
{
    DocView *d     = b->md;
    View    *v     = d->v;
    f32      sc    = g->scale;
    f32      pad   = 4*sc;
    f32      width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 2*pad;
    width = (f32)(i32)(width<50*sc ? 50*sc : width);  // as drawdoc's
    if (!v->msgs.len) {
        return;
    }
    i32 off = 0;
    for (i32 n = 0; n<line && off<b->body.len; off++) {
        n += b->body[off] == '\n';
    }
    FontMetrics fm    = fontmetrics(&g->fonts, 0);
    f32         charw = fontadvance(&g->fonts, 0, 'n');
    if (width != v->lastw) {
        v->topsdirty = 1;
        v->lastw     = width;
    }
    msglayout(v, 0, &g->measure, width, sc);
    viewtops(v, width, charw, fm.lineh);
    Anchor an = {0, off, dy};
    d->scrollto = anchory(v, an, &g->measure, width, sc, pad);
}

// The Files pane: the shown root's tabs, and the front one's file, read
// as it is first shown, with its notes, its toolbar and its text.
static void drawfiles(Gui *g)
{
    Arena       a    = g->frame;
    GuiSession *root = filesroot(g);
    FileSet    *set  = &root->files;
    g->fshown.v   = 0;
    g->fnoteslen  = 0;
    if (!set->n) {
        g->fshowing = 0;
        filesempty(g);
        return;
    }
    FileBar bar = filetabs(g, root);
    FileTab *rt = bar.reload>=0 ? set->tabs[bar.reload] : 0;
    if (rt && !(rt->host && filehostwhy(filehost(g, rt->host)).len)) {
        filefree(g, rt);  // not one whose host can't serve it
        rt->state = FTAB_UNREAD;
    }
    if (bar.closed>=0 && (bar.others || bar.all)) {
        FileTab *keep = bar.all ? 0 : set->tabs[bar.closed];
        for (i32 i = set->n-1; i >= 0; i--) {
            if (set->tabs[i] != keep) {
                fileclose(g, set, i);
            }
        }
    } else if (bar.closed >= 0) {
        fileclose(g, set, bar.closed);
    }
    FileTab *ft = filesetfront(set);
    if (!ft) {
        g->fshowing = 0;
        filesempty(g);
        fetchtick(g, 0);
        return;
    }
    fetchtick(g, ft);
    GuiHost *fh = filehost(g, ft->host);
    if (ft->state==FTAB_FAILED && ft->host && fh && fh->opens!=ft->opens) {
        ft->state = FTAB_UNREAD;  // its host connected again: once more
    }
    if (ft->state==FTAB_UNREAD && ft->host) {
        filefetch(g, ft);
    } else if (ft->state == FTAB_UNREAD) {
        fileload(g, ft);
    }
    FileBody *b = ft->body;
    Str       farwhy = ft->host ? filehostwhy(fh) : Str{};

    // The toolbar: Markdown or text, Wrap, Reload, then the path
    b32 mdok = b && b->md;
    if (mdok) {
        b32 on  = ft->mode == FMODE_MARKDOWN;
        Str why = b->body.len>DOC_MAXSIZE ? Str("Over 1 MiB, it is too big to show as Markdown")
                                          : Str{};
        ImGui::BeginDisabled(why.len > 0);
        if (checkbox("Markdown", &on)) {
            // The line at the top stays there, at its top
            ft->mode = on ? FMODE_MARKDOWN : FMODE_TEXT;
            ft->dy   = 0;
        }
        ImGui::EndDisabled();
        itemnamed(g, "file markdown");
        whytip(why.len ? why : Str("Show it rendered as Markdown, else as text"));
        ImGui::SameLine();
    }
    if (b && ft->mode==FMODE_TEXT) {
        if (checkbox("Wrap", &ft->wrap)) {
            g->filewrap = ft->wrap;  // new tabs take it
        }
        itemnamed(g, "file wrap");
        itemtip("Wrap long lines at the pane's width");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(farwhy.len > 0);
    if (iconbutton(ICON_REFRESH "##filereload")) {
        filefree(g, ft);
        ft->state = FTAB_UNREAD;
        b = 0;
    }
    ImGui::EndDisabled();
    itemnamed(g, "file reload");
    if (farwhy.len) {
        whytip(farwhy);
    } else {
        itemtip("Reload: read the file again");
    }
    ImGui::SameLine();
    {
        Measure m = {};
        m.advance = widgetadvance;
        Str shown = homeshow(&a, filehost(g, ft->host) ? filehost(g, ft->host) : g->main,
                             ft->path);
        Str cutp  = midcut(&a, shown, ImGui::GetContentRegionAvail().x, &m, 0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", cstr0(&a, cutp.len ? cutp : shown));
        Str tip = concat(&a, concat(&a, ft->path, ", on "), filehostname(g, &a, ft->host));
        itemtip("%s", cstr0(&a, tip));
        itemnamed(g, "file path");
    }

    // Notes
    if (ft->state == FTAB_FAILED) {
        filenote(g, ft->err, 1);
        if (ImGui::Button("Retry")) {
            ft->state = FTAB_UNREAD;
            scheddirty(&g->sched);
        }
        itemnamed(g, "file retry");
        g->fshowing = 0;
        return;
    } else if (!b) {
        // Coming from its host, or waiting for it to connect
        if (ft->state == FTAB_READING) {
            filenote(g, "Reading...", 0);
        } else if (farwhy.len) {
            filenote(g, farwhy, 1);
            if (fh && fh->r && !fh->closing && !fh->up) {
                Buf nb(&a, 64 + fh->name.len);
                print(&nb, "Reconnecting to ");
                print(&nb, fh->name);
                print(&nb, "...");
                filenote(g, finish(&nb), 0);
            }
        }
        g->fshowing = 0;
        return;
    }
    b->drawn = g->total.frames;
    if (b->changed) {
        filenote(g, "It changed while it was read.", 0);
    }
    if (b->flags&FTEXT_BINARY && !ft->astext) {
        Buf nb(&a, 64);
        print(&nb, "This is a binary file (");
        print(&nb, b->size);
        print(&nb, " bytes).");
        filenote(g, finish(&nb), 0);
        if (ImGui::Button("Show as text anyway")) {
            ft->astext = 1;
            filefree(g, ft);
            ft->state = FTAB_UNREAD;
            scheddirty(&g->sched);
        }
        itemnamed(g, "file astext");
        g->fshowing = 0;
        return;
    } else if (!b->body.len) {
        filenote(g, "This file is empty.", 0);
        g->fshowing = 0;
        return;
    }
    if (b->flags & FTEXT_CUT) {
        Buf nb(&a, 64);
        print(&nb, "Only its first ");
        print(&nb, (i64)b->lines);
        print(&nb, " lines are shown.");
        filenote(g, finish(&nb), 0);
    }
    if (ft->band0 > b->lines) {
        Buf nb(&a, 64);
        print(&nb, "The file has only ");
        print(&nb, (i64)b->lines);
        print(&nb, b->lines==1 ? Str(" line.") : Str(" lines."));
        filenote(g, finish(&nb), 0);
    }

    // Another tab, mode or body than last frame's: its own place again,
    // which the last frame drew kept in its tab
    DocLines *dl    = &g->flines;
    b32       fresh = g->fshowing!=ft || g->fshowingserial!=ft->serial ||
                      g->fshowingmode!=ft->mode || g->fshowingbody!=b;
    if (fresh) {
        g->fshowing       = ft;
        g->fshowingserial = ft->serial;
        g->fshowingmode   = ft->mode;
        g->fshowingbody   = b;
        dl->restore = 1;
        dl->top     = ft->top;
        dl->dy      = ft->dy;
        dl->scrollx = ft->scrollx;
        dl->seenw   = 0;
    }

    if (ft->mode == FMODE_MARKDOWN) {
        DocView *d = b->md;
        b32 pending = ImGui::IsWindowAppearing() || g->layoutpending || g->layoutnow;
        if (ft->sectionlen) {
            d->section = {ft->section, ft->sectionlen};  // the tab's, outliving the frame
            if (!pending) {
                ft->sectionlen = 0;
                dl->restore    = 0;
            }
        } else {
            d->section = {};
            if (dl->restore) {
                filemdgo(g, b, dl->top, dl->dy);
                dl->restore = pending;
            }
        }
        if (pending) {
            scheddirty(&g->sched);
        }
        // Its child's scroll is the last tab's until it is set. Its links
        // are from its folder, and "#section" stays in it
        b32 set = d->scrollto>=0 || d->section.len;
        d->from       = filelinkfrom(g->sess, {});
        d->from.base  = parentdir(ft->path);
        d->from.host  = ft->host;
        d->from.win   = ft->win;
        d->from.opens = 1;
        d->from.file  = ft->path;
        Str link = drawdoc(g, d, "##filetext");
        itemnamed(g, "file text");
        if (link.len) {
            followlink(g, link, &d->from);
        }
        if (!pending && !set && !dl->restore) {
            filemdtop(g, b, &ft->top, &ft->dy);
        }
        g->fshown.v = 0;
        return;
    }

    ft->sectionlen = 0;  // a heading means nothing in the text

    // The text: jumped to its cited lines once, else where it was
    i32 lines = b->lines;
    dl->wrap  = ft->wrap;
    dl->cols  = b->cols;
    dl->lines = lines;
    dl->band0 = fileclamp(ft->band0, lines);
    dl->band1 = fileclamp(ft->band1, lines);
    dl->jump0 = ft->jump && !ft->jumped ? fileclamp(ft->band0, lines) : 0;
    dl->jump1 = dl->jump0 ? fileclamp(ft->band1, lines) : 0;
    drawdoc(g, b->text, "##filetext", 0, dl);
    itemnamed(g, "file text");
    if (dl->jumped) {
        ft->jumped  = 1;
        dl->restore = 0;
    } else if (dl->restored) {
        dl->restore = 0;
    }
    if (dl->recorded) {
        ft->top     = dl->top;
        ft->dy      = dl->dy;
        ft->scrollx = dl->scrollx;
    }
}


// For the checklist (--drive)

// The front tab of the shown root, else null.
static FileTab *filefront(Gui *g)
{
    return filesetfront(&filesroot(g)->files);
}

// Where line n (1-based) of the shown text view is on screen, its first
// row's middle at x from the text's left edge, else (-1, -1).
static ImVec2 filelineat(Gui *g, i32 n, f32 x)
{
    FileShown *fs = &g->fshown;
    View      *v  = fs->v;
    if (!v || n<1 || !v->msgs.len) {
        return v2(-1, -1);
    }
    i32 ci = (n - 1)/FILES_CHUNK;
    if (ci >= v->msgs.len) {
        return v2(-1, -1);
    }
    MsgLayout *ml = msglayout(v, ci, &g->measure, fs->width, g->scale);
    i32        k  = (n - 1)%FILES_CHUNK;
    if (k >= ml->blocks.len) {
        return v2(-1, -1);
    }
    Placed *pl = &ml->blocks[k];
    f32     h  = pl->bl->lines.len ? pl->bl->lines[0].h : pl->bl->h;
    f32     y  = fs->wpos.y + fs->pad + (f32)v->msgs[ci].top + pl->y - fs->scrolly + h/2;
    f32     tx = fs->wpos.x + fs->pad + fs->gutter - fs->scrollx + x;
    return v2(tx, y);
}

// The rows line n of the shown text view takes, as laid out, else -1.
static i32 filelinerows(Gui *g, i32 n)
{
    FileShown *fs = &g->fshown;
    View      *v  = fs->v;
    i32        ci = (n - 1)/FILES_CHUNK;
    if (!v || n<1 || ci>=v->msgs.len) {
        return -1;
    }
    MsgLayout *ml = msglayout(v, ci, &g->measure, fs->width, g->scale);
    i32        k  = (n - 1)%FILES_CHUNK;
    return k<ml->blocks.len ? (i32)ml->blocks[k].bl->lines.len : -1;
}

// The bodies' committed bytes, and how many there are.
static iz filebytes(Gui *g, i32 *n)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    iz          total   = 0;
    *n = 0;
    for (i32 k = 0; k < countof(roots); k++) {
        for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                FileBody *b = s->files.tabs[i]->body;
                if (b) {
                    total += b->r->committed;
                    (*n)++;
                }
            }
        }
    }
    return total;
}
