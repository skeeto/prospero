// The Files pane: files shown in tabs, a root session's (fileset.cpp),
// each read into a body of its own and drawn as text, highlighted, its
// lines numbered (drawdoc's DocLines), or as Markdown; an image file
// (PNG, JPEG, GIF, WebP, BMP) is read whole, up to IMG_FILEMAX, decoded
// and shown as a texture (drawimage), its bytes counted with the
// bodies'.
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
    FBODY_IMAGE,  // a texture, no text or views
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
    GuiTex      *tex;      // an image's, maybe shrunk to fit a texture
    i32          w;        // and the image's own size in pixels
    i32          h;
};

// Whether a file's name says an image, shown as one.
static b32 fileimgname(Str path)
{
    Str exts[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};
    for (i32 i = 0; i < countof(exts); i++) {
        if (path.len>exts[i].len && equalfold(cuthead(path, path.len-exts[i].len), exts[i])) {
            return 1;
        }
    }
    return 0;
}

// Why an image file is not read: past IMG_FILEMAX, as it says.
static Str filebigwhy()
{
    return IMG_FILEMAX==(iz)32<<20 ? Str("Over 32 MiB, it is too big to show") :
           IMG_FILEMAX==(iz)8<<20  ? Str("Over 8 MiB, it is too big to show")  :
                                     Str("It is too big to show");
}

// A tab's limits as read: an image's whole file, else the text's start;
// and the scratch it is read into and made a body in.
static iz filecap(FileTab *ft)
{
    return fileimgname(ft->path) ? (iz)IMG_FILEMAX : (iz)FILES_MAX;
}

static iz filescratch(FileTab *ft)
{
    return fileimgname(ft->path) ? (iz)IMG_REGION : (iz)FILES_SCRATCH;
}

// A body's bytes, as FILES_BUDGET counts them: its Region's, and an
// image's texture's.
static iz filebodybytes(FileBody *b)
{
    return b->r->committed + (b->tex ? b->tex->r->committed : 0);
}

// A body's memory let go: its texture, once the renderer lets it go,
// and its Region.
static void filebodyfree(Gui *g, FileBody *b)
{
    killtex(g, b->tex);
    os_release(g->os, b->r);
}

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
static void filefindforget(Gui *, FileBody *);

static void filefree(Gui *g, FileTab *ft)
{
    FileBody *b = ft->body;
    if (!b) {
        return;
    }
    View *views[] = {b->text ? b->text->v : 0, b->md ? b->md->v : 0};
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
    filefindforget(g, b);
    filebodyfree(g, b);
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

// Close the shown root's front tab, if any (Ctrl+W).
static void fileclosefront(Gui *g)
{
    FileSet *set = &filesroot(g)->files;
    if (set->n) {
        fileclose(g, set, set->front);
    }
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
// of its own, highlighted, in chunks, else null, saying why: the scratch
// sr is released, and the tab left as it is.
static FileBody *fileimgmake(Gui *, FileTab *, Region *, Arena, Str, i64, i64, b32, Str *);

static FileBody *filemake(Gui *g, FileTab *ft, Region *sr, Arena s, Str raw, i64 size,
                          i64 mtime, b32 changed, Str *why)
{
    if (fileimgname(ft->path)) {
        return fileimgmake(g, ft, sr, s, raw, size, mtime, changed, why);
    }

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
        *why = "There is no memory to show it";
        return 0;
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
    return b;
}

// A tab's image file, raw and whole, decoded into a texture, its body
// else null, saying why: the scratch sr is released, and the tab left as
// it is but for errbuf, where a decoder's why is said.
static FileBody *fileimgmake(Gui *g, FileTab *ft, Region *sr, Arena s, Str raw, i64 size,
                             i64 mtime, b32 changed, Str *why)
{
    if (size > raw.len) {
        // Past the cap, it grew past it after its stat; else it shrank
        // while it was read
        os_release(g->os, sr);
        *why = size>IMG_FILEMAX ? filebigwhy() : Str("It changed while it was read");
        return 0;
    }
    u64   sum = hash(raw);  // before the decode's scratch, past raw, is used
    Image p   = imgdecode(&s, raw, IMG_MAXPIXELS);
    if (p.err.data) {
        Str head = "Not an image it can show: ";
        iz  room = countof(ft->errbuf) - head.len;
        iz  n    = p.err.len<room ? p.err.len : room;
        copybytes(ft->errbuf, head.data, head.len);
        copybytes(ft->errbuf+head.len, p.err.data, n);
        *why = {ft->errbuf, head.len+n};
        os_release(g->os, sr);
        return 0;
    }
    Region *r   = os_reserve(g->os, (iz)64<<10);
    Arena   mem = newarena(r);
    GuiTex *tex = r && fits(&mem, sizeof(FileBody)) ? bigtex(g, s, &p) : 0;
    os_release(g->os, sr);
    if (!tex) {
        if (r) {
            os_release(g->os, r);
        }
        *why = "There is no memory to show it";
        return 0;
    }
    FileBody *b = alloc<FileBody>(&mem);
    b->r       = r;
    b->kind    = FBODY_IMAGE;
    b->tex     = tex;
    b->w       = p.width;
    b->h       = p.height;
    b->size    = size;
    b->mtime   = mtime;
    b->sum     = sum;
    b->changed = changed;
    b->drawn   = g->total.frames;
    return b;
}

// A tab's new body shown, else (null) why not, its old one let go. A
// body of the same text as the old, read again, keeps the old, its place
// laid out, with the new one's stat. Otherwise a body read again shows
// from the same top line and with the same band, as far as it has them.
// Returns whether what shows changed.
static b32 fileswap(Gui *g, FileTab *ft, FileBody *nb, Str why)
{
    FileBody *old = ft->body;
    if (!nb) {
        filefree(g, ft);
        filefail(g, ft, why);
        return 1;
    } else if (old && old->kind==nb->kind && old->sum==nb->sum && old->body==nb->body &&
               old->flags==nb->flags && old->w==nb->w && old->h==nb->h) {
        b32 note = old->changed != nb->changed;
        old->size    = nb->size;
        old->mtime   = nb->mtime;
        old->changed = nb->changed;
        old->fetched = nb->fetched;
        filebodyfree(g, nb);
        return note;
    }
    filefree(g, ft);
    ft->body  = nb;
    ft->state = FTAB_READ;
    ft->err   = {};
    if (ft->mode==FMODE_MARKDOWN && (!nb->md || nb->body.len>DOC_MAXSIZE)) {
        ft->mode = FMODE_TEXT;  // too big to render, or binary shown as text
    }
    if (ft->top >= nb->lines) {
        ft->top = nb->lines>0 ? nb->lines-1 : 0;  // shorter now
        ft->dy  = 0;
    }
    return 1;
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
                    total += filebodybytes(b);
                    (*n)++;
                }
            }
        }
    }
    return total;
}

// Bodies let go, least recently drawn first, until all, with extra
// bytes more, fit in FILES_BUDGET, but never keep's nor the one shown.
static void filesfit(Gui *g, FileTab *keep, iz extra)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    GuiSession *shown   = filesroot(g);
    FileTab    *front   = filesetfront(&shown->files);
    i32         n       = 0;
    iz          total   = filebytes(g, &n) + extra;
    while (total > FILES_BUDGET) {
        FileTab *old = 0;
        for (i32 k = 0; k < countof(roots); k++) {
            for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
                for (i32 i = 0; i < s->files.n; i++) {
                    FileTab *ft = s->files.tabs[i];
                    if (ft->body && ft!=front && ft!=keep &&
                            (!old || ft->body->drawn<old->body->drawn)) {
                        old = ft;
                    }
                }
            }
        }
        if (!old) {
            break;
        }
        total -= filebodybytes(old->body);
        filefree(g, old);
    }
}

// A tab's file, on this computer, read into a new body, else null,
// saying why, the tab left as it is.
static FileBody *filebody(Gui *g, FileTab *ft, Str *why)
{
    Region *sr = os_reserve(g->os, filescratch(ft));
    if (!sr) {
        *why = "There is no memory to show it";
        return 0;
    }
    Arena s  = newarena(sr);
    Stat  st = filestat(g, s, ft->path);
    Read  rd = {};
    *why = filestatwhy(st.type);
    if (!why->len && fileimgname(ft->path) && st.size>IMG_FILEMAX) {
        *why = filebigwhy();
    } else if (!why->len) {
        rd   = fileread(g, &s, ft->path, filecap(ft));
        *why = rd.err==READ_OK       ? Str{} :
               rd.err==READ_NOTFOUND ? Str("No such file") :
               rd.err==READ_NOTFILE  ? Str("Not a regular file") :
               Str("It could not be read");
    }
    if (why->len) {
        os_release(g->os, sr);
        return 0;
    }
    return filemake(g, ft, sr, s, rd.data, rd.size, st.mtime, 0, why);
}

// Read a tab's file, on this computer, into a body of its own, else say
// why not in the tab. One read already stays shown until the new one is
// made, room for it made first.
static void fileload(Gui *g, FileTab *ft)
{
    g->fileloads++;
    if (ft->body) {
        filesfit(g, ft, filebodybytes(ft->body));
    } else {
        ft->err = {};
    }
    ft->watch.stale  = 0;
    ft->watch.readat = os_time(g->os);
    Str       why = {};
    FileBody *nb  = filebody(g, ft, &why);
    if (fileswap(g, ft, nb, why)) {
        scheddirty(&g->sched);
    }
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
    i64   want = f->max - f->off;
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

// The fetch let go, its tab as it is: one read again checked again.
static void fetchend(Gui *g)
{
    FileFetch *f = &g->ffetch;
    if (f->reload && f->ft && f->ft->serial==f->serial) {
        f->ft->watch.id = 0;
        if (f->ft->watch.stale || f->ft->watch.force) {
            scheddirty(&g->sched);  // marked meanwhile: checked now
        }
    }
    if (f->r) {
        os_release(g->os, f->r);
    }
    *f = {};
}

// Whether the fetch's tab still waits for it: unread, or read and
// shown while it is read again.
static b32 fetchwanted(FileFetch *f)
{
    FileTab *ft = f->ft;
    return ft && ft->serial==f->serial &&
           (f->reload ? ft->state==FTAB_READ && ft->body : ft->state==FTAB_READING);
}

// The fetch abandoned: its tab, still waiting for it, read again later,
// or, read again, checked again once it shows.
static void fetchdrop(Gui *g)
{
    FileFetch *f = &g->ffetch;
    if (fetchwanted(f) && f->reload) {
        f->ft->watch.stale = 1;
    } else if (fetchwanted(f)) {
        f->ft->state = FTAB_UNREAD;
    }
    fetchend(g);
}

// The fetch failed, saying why in its tab, which is tried again once
// its host has connected again.
static void fetchfail(Gui *g, Str why)
{
    filefree(g, g->ffetch.ft);  // read again: what it showed is gone
    filefail(g, g->ffetch.ft, why);
    fetchend(g);
    scheddirty(&g->sched);
}

// Start reading a tab's file on another host, as its tab shows: one
// fetch at a time, the last given up. With its host down, the tab waits
// (filesnote says so); with an engine there too old, it fails. A tab
// read already is read again (reload) into a new body, the old shown
// until it is made.
static void filefetch(Gui *g, FileTab *ft, b32 reload)
{
    GuiHost *h   = filehost(g, ft->host);
    Str      why = filehostwhy(h);
    if (why.len && h && h->r && !h->closing && h->up && !reload) {
        filefail(g, ft, why);  // too old, until updated
        return;
    } else if (why.len) {
        return;  // until it connects
    }
    fetchdrop(g);
    FileFetch *f = &g->ffetch;
    f->r = os_reserve(g->os, filescratch(ft));
    if (!f->r) {
        filefree(g, ft);
        filefail(g, ft, "There is no memory to show it");
        return;
    }
    // The buffer is the region's, not f->mem's: f->mem is let go (fetchend)
    // before the body is made from what came, which must stay readable
    Arena m = newarena(f->r);
    f->max    = filecap(ft);
    f->buf    = allocbytes(&m, f->max);
    f->mem    = m;
    f->ft     = ft;
    f->serial = ft->serial;
    f->host   = ft->host;
    f->size   = -1;
    f->reload = reload;
    if (reload) {
        ft->watch.id = -1;
    } else {
        ft->err         = {};
        ft->state       = FTAB_READING;
        ft->watch.stale = 0;
    }
    fsrequest(g);
    scheddirty(&g->sched);
}

// Each frame the pane is drawn, for the front tab ft: a fetch for
// another given up, and one whose request slot went (REQ_MAX) asked
// for again.
static void fetchtick(Gui *g, FileTab *ft)
{
    FileFetch *f = &g->ffetch;
    if (f->ft && (f->ft!=ft || !fetchwanted(f))) {
        fetchdrop(g);
    } else if (f->ft && f->id && !reqheld(g, f->id)) {
        fsrequest(g);
    }
}

static void fileaskstat(Gui *);

// The uid a host's files are known by in tabs: 0, this computer, for
// every host but a remote, whose own engine reads them.
static u64 fileuid(GuiHost *h)
{
    return h && h->kind==GH_REMOTE ? h->uid : 0;
}

// Files on the host of uid host may have changed, as of ts, epoch ms on
// its clock (0: now): each of its tabs read before then is marked, to
// be checked once shown (filewatch). A replay's older records mark
// nothing. A check in flight there is let go, with lost, as its reply
// will never come.
static void filesstale(Gui *g, u64 host, i64 ts, b32 lost)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    FileTab    *front   = filesetfront(&filesroot(g)->files);
    for (i32 k = 0; k < countof(roots); k++) {
        for (GuiSession *s = roots[k]; s; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                FileTab   *ft = s->files.tabs[i];
                FileWatch *w  = &ft->watch;
                if (ft->host != host) {
                    continue;
                } else if (lost && w->id>0) {
                    w->id = 0;
                }
                if (!ts || ts>=w->readat) {
                    w->stale = 1;
                    if (ft==front && g->filesdrawn) {
                        scheddirty(&g->sched);
                    }
                }
            }
        }
    }
}

// One tab may have changed: checked once shown.
static void filetabstale(Gui *g, FileTab *ft)
{
    ft->watch.stale = 1;
    scheddirty(&g->sched);
}

// Its host connected again: the part in flight asked for again, as its
// reply may have gone with the link, and Open file's check; its files
// checked again, changed perhaps meanwhile, checks in flight lost.
static void filefetchagain(Gui *g, GuiHost *h)
{
    FileFetch *f = &g->ffetch;
    if (f->ft && f->id && f->host==h->uid) {
        fsrequest(g);
    }
    if (g->fask.statid && g->fask.host==h->uid) {
        fileaskstat(g);
    }
    if (h->kind == GH_REMOTE) {
        filesstale(g, h->uid, 0, 1);
    }
}

// Its host's link ended (Disconnect, quitting): a reply in flight from
// it is gone, unasked for, so the fetch is dropped, to read again once
// it is back, and Open file's check, at it, fails; checks of its files
// are lost, to be made again once it is back.
static void filehostend(Gui *g, GuiHost *h)
{
    if (g->ffetch.ft && g->ffetch.host==h->uid) {
        fetchdrop(g);
    }
    if (g->fask.statid && g->fask.host==h->uid) {
        g->fask.statid = 0;
        g->fask.p.err  = filehostwhy(0);
    }
    codelinkhostend(g, h->uid);
    if (h->kind == GH_REMOTE) {
        filesstale(g, h->uid, 0, 1);
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
// here (or its cap, FILES_MAX for text), made into a body; an image too
// big to read whole fails at once. A size or mtime changed reads
// it again from the start once; changed again, what came is kept.
static void onfsread(Gui *g, Arena scratch, Json *j, b32 ok, Str err, i64 ts)
{
    FileFetch *f  = &g->ffetch;
    FileTab   *ft = f->ft;
    f->id = 0;
    if (!fetchwanted(f)) {
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
    if (f->size<0 && fileimgname(ft->path) && size>IMG_FILEMAX) {
        fetchfail(g, filebigwhy());
        return;
    } else if (f->size < 0) {
        f->size  = size;
        f->mtime = mtime;
        f->ts    = ts;  // when it was read, on its host's clock
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
        iz n = u.data.len<f->max-f->off ? u.data.len : (iz)(f->max-f->off);
        copybytes(f->buf+f->off, u.data.data, n);
        f->off += n;
    }
    if (more && u.data.len && f->off<f->max) {
        fsrequest(g);
        return;
    }

    // All here: its body, from the scratch past what came, room made
    // first for it beside one read already
    Region *r = f->r;
    Arena   s = f->mem;
    Str     raw = {f->buf, (iz)f->off};
    i64     total = f->changed ? f->off : f->size;
    b32     twice = f->changed;
    i64     when  = f->mtime;
    ft->watch.readat = f->ts;
    f->r = 0;
    fetchend(g);
    g->fileloads++;
    if (ft->body) {
        filesfit(g, ft, filebodybytes(ft->body));
    }
    Str       why = {};
    FileBody *nb  = filemake(g, ft, r, s, raw, total>raw.len ? total : raw.len, when, twice,
                             &why);
    if (nb) {
        nb->fetched = 1;
    }
    if (fileswap(g, ft, nb, why)) {
        scheddirty(&g->sched);
    }
}

// Each frame the pane is drawn, for its front tab ft, read: its file
// checked, as filenext says, once marked as it may have changed (stale),
// here by its stat, on another host by fs.stat there, and read again
// into a new body, the old shown meanwhile, when its stat says so
// (filechanged), or at once for Reload. Nothing while its host is down.
static void filewatch(Gui *g, FileTab *ft)
{
    FileWatch *w = &ft->watch;
    GuiHost   *h = filehost(g, ft->host);
    if (ft->state!=FTAB_READ || !ft->body || (ft->host && filehostwhy(h).len)) {
        return;
    }
    if (w->id>0 && !reqheld(g, w->id)) {
        w->id    = 0;  // its request slot went (REQ_MAX)
        w->stale = 1;
    } else if (w->id<0 && !(g->ffetch.ft==ft && fetchwanted(&g->ffetch))) {
        w->id = 0;  // its read again is gone (fetchend clears it)
    }
    i64      now = os_time(g->os);
    FileNext n   = filenext(w, now);
    if (n.wake) {
        schedat(&g->sched, g->sched.now + (n.wake - now)*1000, DUE_APP);
    }
    if (!n.check) {
        return;
    }
    b32 force = w->force;
    w->stale = 0;
    w->force = 0;
    w->sent  = now;
    if (ft->host && force) {
        filefetch(g, ft, 1);
    } else if (ft->host) {
        Arena a = newarena(g->cmdr);
        Buf   b(&a, 256 + 6*ft->path.len);
        EvOut o = cmdopen(g, &b, CMD_FSSTAT, 0, h);
        putkey(&o.json, "paths");
        putarray(&o.json);
        putstr(&o.json, ft->path);
        putend(&o.json);
        w->id = g->nextid;
        cmdsend(g, &o, a);
    } else {
        FileBody *b  = ft->body;
        Stat      st = filestat(g, g->frame, ft->path);
        b32       go = force || st.type!=STAT_FILE ||
                       filechanged(b->size, b->mtime, w->readat, st.size, st.mtime);
        if (go) {
            fileload(g, ft);
        }
    }
}

// A host's answer to a tab's check (filewatch), the reply to id: read
// again if its stat says so, else as it is. A file gone fails the tab.
static void onfswatch(Gui *g, i64 id, Json *j, b32 ok, Str err)
{
    GuiSession *roots[] = {&g->nosession, g->sessions};
    FileTab    *ft      = 0;
    for (i32 k = 0; k<countof(roots) && !ft; k++) {
        for (GuiSession *s = roots[k]; s && !ft; s = k ? s->next : 0) {
            for (i32 i = 0; i < s->files.n; i++) {
                ft = s->files.tabs[i]->watch.id==id ? s->files.tabs[i] : ft;
            }
        }
    }
    if (!ft) {
        return;
    }
    FileWatch *w = &ft->watch;
    w->id = 0;
    if (ft->state!=FTAB_READ || !ft->body) {
        return;
    } else if (!ok) {
        // Not sent, lost, refused, or its host went: the mark kept, so
        // checked again a few times (else at the next mark), or once
        // its host is back
        w->fails++;
        if (w->fails<=FILES_TRIES || filehostwhy(filehost(g, ft->host)).len) {
            w->stale = 1;
            scheddirty(&g->sched);
        }
        return;
    }
    w->fails = 0;
    FileBody *b     = ft->body;
    Json     *st    = jsonat(jsonget(j, "stats"), 0);
    Str       type  = jsonstr(jsonget(st, "type"));
    i64       size  = jsonint(jsonget(st, "size"), -1);
    i64       mtime = jsonint(jsonget(st, "mtime"), 0);
    if (type != "file") {
        Str why = type=="dir"     ? Str("That is a folder") :
                  type=="missing" ? Str("No such file") :
                  type=="bad"     ? Str("It cannot be shown") :
                                    Str("Not a regular file");
        fileswap(g, ft, 0, why);
        scheddirty(&g->sched);
    } else if (filechanged(b->size, b->mtime, w->readat, size, mtime)) {
        if (ft==filesetfront(&filesroot(g)->files) && g->filesdrawn) {
            filefetch(g, ft, 1);
        } else {
            w->stale = 1;  // no longer shown: checked again once it is
        }
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
                if (b && b->c) {
                    cachereset(b->c, 0);
                }
                total += b ? filebodybytes(b) : 0;
            }
        }
    }
    if (total > FILES_BUDGET) {
        filesfit(g, 0, 0);
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
                if (b && b->c) {
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
        ft->fit    = 1;
        ft->mode   = filemdname(path) ? FMODE_MARKDOWN : FMODE_TEXT;
        FileTab *gone = filesetadd(set, ft);
        if (gone) {
            filedrop(g, gone);
        }
    }
    if (line0 && !fileimgname(path)) {
        ft->mode   = FMODE_TEXT;  // a line is the text's; an image has none
        ft->band0  = line0;
        ft->band1  = line1>=line0 ? line1 : line0;
        ft->jump   = line0;
        ft->jumped = 0;
        ft->goline = line0;
        ft->gocol  = 0;
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
    if (!d->here && r->line0 && !fileimgname(f.path)) {
        // An image opens with no line (filesopen), so none is named
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
        filetabstale(g, root->files.tabs[i]);
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
    m->col   = d.here ? 0 : d.file.col;
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

// A file of a host (its uid, 0 for this computer) as a path here, for
// the editor and the file manager, else null and why: a container's in
// the folder mounted at /work, a WSL distro's through Windows' share.
static Str filelocal(Gui *g, Arena *a, u64 host, Str path, Str *why)
{
    *why = {};
    if (!host) {
        return path;
    }
    GuiHost *h   = filehost(g, host);
    b32      run = 0;
    Ctr     *c   = h ? hostctr(g, h, &run) : 0;
    if (c) {
        Str p = ctrhostpath(a, c, path, HOST_WINDOWS);
        *why = p.len ? Str{} : Str("Its file is only in its container");
        return p;
    } else if (HOST_WINDOWS && h && hostwsl(g, h)) {
        return wslwinpath(a, remoteset(g, h->name, "wsl"), path);
    }
    *why = "Its file is on another computer";
    return {};
}

// Open in editor for a host's file at a line and column (0 for none):
// the editor's command, or why not.
static EditJump filejump(Gui *g, Arena *a, u64 host, Str path, i32 line, i32 col)
{
    Str      why  = {};
    Str      here = filelocal(g, a, host, path, &why);
    EditJump j    = {};
    if (why.len) {
        j.why = why;
        return j;
    }
    return editorjumpfor(g, a, here, line, col);
}

// Open in editor, launched: the editor on a host's file.
static void filejumprun(Gui *g, Arena a, u64 host, Str path, i32 line, i32 col)
{
    Str why  = {};
    Str here = filelocal(g, &a, host, path, &why);
    if (why.len) {
        g->lasterr = clone(&g->perm, why);
        return;
    }
    editorjumprun(g, a, here, line, col);
}

// Open in editor's tooltip while it can: what it runs, and for an
// editor whose arguments for a line aren't known, how to give them.
static Str filejumptip(Arena *a, EditJump *j)
{
    Str tip = concat(a, "Open the file in your editor: ", j->line);
    if (!j->known) {
        tip = concat(a, tip, ". Its editor is not one known to take a line: "
                             "[ui] editor_line can give its arguments");
    }
    return tip;
}

// Reveal's tooltip, by the platform's file manager.
static Str filerevealtip()
{
    #if defined(__APPLE__)
    return "Reveal: show the file in Finder";
    #elif defined(_WIN32)
    return "Reveal: show the file in File Explorer";
    #else
    return "Reveal: show the file in the file manager";
    #endif
}

// The text's context menu's first items, for a file link right-clicked:
// Open in Files, Open in editor and Copy path, or why not.
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
    EditJump j = path.len ? filejump(g, &a, m->host, path, m->line0, m->col) : EditJump{};
    Str jwhy = !path.len ? why : j.why;
    if (ImGui::MenuItem("Open in editor", 0, false, !jwhy.len)) {
        filejumprun(g, a, m->host, path, m->line0, m->col);
    }
    itemnamed(g, "linkmenu editor");
    if (jwhy.len) {
        whytip(jwhy);
    } else {
        itemtip("%s", cstr0(&a, filejumptip(&a, &j)));
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


// Inline code naming a file (doc.cpp's DocCand): a link once its file
// is found (CodeLinks, app.cpp). Only what is drawn is asked about, and
// only a committed message's. A span resolves as nobody asked for it
// (filepath, unasked 1): inside the working directory, with no "~" and
// no network path, so nothing a model writes makes the window touch
// another computer. Here no link is followed either: a path through a
// symlink or junction below the folder (os_haslink, told without
// following it) is a miss, never stat'ed, wherever the link leads, as
// a link inside a cloned repository may name a share or an automount.
// Else filestat answers, at most CODELINK_BATCH paths
// or CODELINK_US a frame; on another host fs.stat does, in batches of
// CODELINK_BATCH, one in flight per host. A miss is asked again only
// when drawn CODELINK_AGAIN later: no timers, no idle frames. A batch
// whose reply is lost (its host gone, its request slot taken, or
// FILES_LOST passed) lets its paths be asked again.

// The cache's entry for a span's file from where it is, made if new;
// null if it names none it may check, or there is no room. It is
// resolved in scratch, which must hold nothing the caller still uses.
static CodeLink *codelinkget(Gui *g, Arena scratch, LinkFrom *from, Str span)
{
    if (!from || !from->opens || !span.len) {
        return 0;
    }
    Arena a = scratch;
    Dest  d = helpdest(&a, span);
    if (d.kind!=DEST_FILE || d.here || d.file.kind!=FREF_PATH) {
        return 0;
    }
    FilePath p = filepath(&a, d.file, from->base, {}, from->win, 1);
    if (p.err.len || !p.path.len) {
        return 0;
    }

    CodeLinks *c = &g->codelinks;
    if (!c->r) {
        c->r = os_reserve(g->os, CODELINK_REGION);
        if (!c->r) {
            return 0;
        }
        c->mem  = newarena(c->r);
        c->base = c->mem.beg;
    }
    Hash64 h = {};
    hashbytes(&h, &from->host, sizeof(from->host));
    hashbytes(&h, p.path.data, p.path.len);
    u64 key = hashfinal(&h);

    CodeLink **m = &c->root;
    for (u64 k = key; *m; k <<= 2) {
        CodeLink *e = *m;
        if (e->key==key && e->host==from->host && e->path==p.path) {
            return e;
        }
        m = &e->child[k>>62];
    }
    iz need = (iz)sizeof(CodeLink) + p.path.len + 64;
    if (c->count>=CODELINK_MAX || !fits(&c->mem, need)) {
        // Full: all forgotten, batches in flight too (their replies
        // then match nothing), and asked again as drawn
        c->mem.beg = c->base;
        c->root    = 0;
        c->count   = 0;
        for (i32 i = 0; i < CODELINK_HOSTS; i++) {
            c->asks[i] = {};
        }
        m = &c->root;
    }
    CodeLink *e = alloc<CodeLink>(&c->mem);
    e->key  = key;
    e->host = from->host;
    e->path = clone(&c->mem, p.path);
    *m = e;
    c->count++;
    return e;
}

// Whether a span drawn is a link: its file found. Asked about if not
// known, or a miss long enough ago.
static b32 codelinkdrawn(Gui *g, Arena scratch, LinkFrom *from, Str span)
{
    CodeLink *e = codelinkget(g, scratch, from, span);
    if (!e) {
        return 0;
    }
    CodeLinks *c   = &g->codelinks;
    i64        now = g->sched.now;
    switch (e->state) {
    case CODE_FILE:
        return 1;
    case CODE_ASKING:
        return 0;
    case CODE_MISS:
        if (now-e->asked < CODELINK_AGAIN) {
            return 0;
        }
        break;
    }

    if (!e->host) {
        if (c->stats>=CODELINK_BATCH || c->spent>=CODELINK_US) {
            c->more = 1;  // the next frame's
            return 0;
        }
        // Below the folder, by its parts' bytes: its own path, and
        // what leads to it, are the user's choice
        Str root = normpath(&scratch, from->base, ".", from->win);
        for (; root.len && issep(root[root.len-1], from->win); root.len--) {}
        i64  t0 = os_ticks(g->os);
        b32  ln = os_haslink(g->os, scratch, e->path, root.len+1);
        Stat st = ln ? Stat{0, 0, STAT_OTHER} : filestat(g, scratch, e->path);
        i64  t1 = os_ticks(g->os);
        e->linked = (u8)ln;
        c->stats++;
        c->spent += t1 - t0;
        e->asked  = t1;
        e->state  = st.type==STAT_FILE ? CODE_FILE : CODE_MISS;
        if (e->state == CODE_FILE) {
            scheddirty(&g->sched);
        }
        return e->state == CODE_FILE;
    }

    GuiHost *h = filehost(g, e->host);
    if (filehostwhy(h).len) {
        return 0;  // asked once it can answer, and drawn
    }
    CodeAsk *q = 0;
    for (i32 i = 0; !q && i < CODELINK_HOSTS; i++) {
        q = c->asks[i].host==e->host ? &c->asks[i] : 0;
    }
    for (i32 i = 0; !q && i < CODELINK_HOSTS; i++) {
        q = !c->asks[i].host ? &c->asks[i] : 0;
    }
    if (!q || q->id || q->n==CODELINK_BATCH) {
        return 0;  // after the batch in flight, whose answer draws again
    }
    q->host = e->host;
    q->paths[q->n++] = e;
    e->state = CODE_ASKING;
    return 0;
}

// The candidate under the mouse in a view's text, if a link: its text.
static Str codelinkat(Gui *g, Arena scratch, View *v, TextHit hit, LinkFrom *from)
{
    if (!hit.hit || !hit.body || v->msgs[hit.at.msg].prov) {
        return {};
    }
    Doc *d = msgdoc(v, hit.at.msg);
    if (!d->cands.len) {
        return {};
    }
    i32 off = hit.at.off;
    for (iz i = 0; i < d->cands.len; i++) {
        DocCand *k = &d->cands[i];
        if (off>=k->off && off<k->off+k->len) {
            CodeLink *e = codelinkget(g, scratch, from, k->text);
            return e && e->state==CODE_FILE ? k->text : Str{};
        }
    }
    return {};
}

// A batch's paths, its answer lost, to be asked again as drawn.
static void codeaskdrop(CodeAsk *q)
{
    for (i32 i = 0; i < q->n; i++) {
        if (q->paths[i]->state == CODE_ASKING) {
            q->paths[i]->state = CODE_NEW;
        }
    }
    *q = {};
}

// At the end of a frame's drawing: the batches built sent, those lost
// let go, and another frame if paths drawn here were left unasked.
static void codelinksflush(Gui *g)
{
    CodeLinks *c   = &g->codelinks;
    i64        now = g->sched.now;
    for (i32 i = 0; i < CODELINK_HOSTS; i++) {
        CodeAsk *q = &c->asks[i];
        if (!q->host) {
            continue;
        }
        GuiHost *h = filehost(g, q->host);
        if (q->id) {
            if (filehostwhy(h).len || !reqheld(g, q->id) ||
                    now-q->sent > (i64)FILES_LOST*1000) {
                codeaskdrop(q);
            }
            continue;
        } else if (filehostwhy(h).len) {
            codeaskdrop(q);
            continue;
        }
        iz    len = 0;
        for (i32 k = 0; k < q->n; k++) {
            len += q->paths[k]->path.len;
        }
        Arena a = newarena(g->cmdr);
        Buf   b(&a, 256 + 6*len);
        EvOut o = cmdopen(g, &b, CMD_FSSTAT, 0, h);
        putkey(&o.json, "paths");
        putarray(&o.json);
        for (i32 k = 0; k < q->n; k++) {
            putstr(&o.json, q->paths[k]->path);
        }
        putend(&o.json);
        q->id   = g->nextid;
        q->sent = now;
        cmdsend(g, &o, a);
    }
    if (c->more) {
        scheddirty(&g->sched);
    }
    c->stats = 0;
    c->spent = 0;
    c->more  = 0;
}

// A host's answer to a batch, if id is one's: each path a file or a
// miss. One not answered (refused, not sent) is a miss, asked again
// later.
static b32 codelinkreply(Gui *g, i64 id, Json *j, b32 ok)
{
    CodeLinks *c = &g->codelinks;
    CodeAsk   *q = 0;
    for (i32 i = 0; !q && i < CODELINK_HOSTS; i++) {
        q = c->asks[i].id==id ? &c->asks[i] : 0;
    }
    if (!id || !q) {
        return 0;
    }
    Json *stats = ok ? jsonget(j, "stats") : 0;
    b32   found = 0;
    for (i32 i = 0; i < q->n; i++) {
        CodeLink *e    = q->paths[i];
        Str       type = jsonstr(jsonget(jsonat(stats, i), "type"));
        e->state = type=="file" ? CODE_FILE : CODE_MISS;
        e->asked = g->sched.now;
        found   |= e->state == CODE_FILE;
    }
    *q = {};
    if (found) {
        scheddirty(&g->sched);
    }
    return 1;
}

// Its host's link ended: its batch in flight is lost.
static void codelinkhostend(Gui *g, u64 host)
{
    for (i32 i = 0; i < CODELINK_HOSTS; i++) {
        if (g->codelinks.asks[i].host == host) {
            codeaskdrop(&g->codelinks.asks[i]);
        }
    }
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


// Files dropped at a point (gui_drop's queue), routed each frame after
// ImGui's NewFrame, which says what windows were where last frame, and
// before anything is drawn. With Open file up, the first fills its
// field, a check in flight for the path it replaces let go, or, too
// long for the field, says so there; the rest go nowhere. With another
// modal up, each is attached, as a drop always was; else a file dropped
// on the Files pane, drawn last frame, opens there, a file on this
// computer, in the shown root's tabs, and one dropped elsewhere is
// attached.
static void droproute(Gui *g)
{
    DropAt *d = g->drophead;
    if (!d) {
        return;
    }
    g->drophead = g->droptail = 0;
    ImGuiWindow *files = ImGui::FindWindowByName(panenames[PANE_FILES]);
    b32          ask   = g->fask.p.shown;
    b32          modal = !ask && ImGui::GetTopMostPopupModal();
    for (; d; d = d->next) {
        if (ask) {
            ask = 0;  // the first only, and the rest nowhere
            if (d->path.len >= FIELD_MAX) {
                // Never cut, which would name another file
                g->fask.p.err = "Its path is too long";
                guilog(g, concat(&g->frame, "dropped on Open file, too long: ", d->path));
                continue;
            }
            fieldput(g->fask.p.buf, d->path);
            g->fask.p.err   = {};
            g->fask.statid  = 0;  // a check in flight is of the path replaced,
            g->fask.close   = 0;  // its answer to open nothing, as on Cancel
            g->fask.dropped = 1;
            continue;
        } else if (g->fask.p.shown) {
            guilog(g, concat(&g->frame, "dropped on Open file, which takes one: ", d->path));
            continue;
        }
        ImGuiWindow *w = 0, *under = 0;
        if (!modal && files && g->filesdrawn) {
            ImGui::FindHoveredWindowEx(v2(d->x, d->y), true, &w, &under);
        }
        if (w && w->RootWindow==files) {
            // From g->frame itself, past what filesopen's copies of it take
            Str path = d->path;
            path = pathroot(path, HOST_WINDOWS) ? normpath(&g->frame, {}, path, HOST_WINDOWS)
                                                : path;
            filesopen(g, g->sess, 0, path, 0, 0, {});
        } else {
            dropattach(g, d->path);
        }
    }
    os_release(g->os, g->dropr);
    g->dropr = 0;
    g->dropa = {};
}


// The pane

// Reload: a tab read again, its body shown until the new one is made
// (filewatch), else read when shown.
static void fileforce(Gui *g, FileTab *ft)
{
    if (ft->state==FTAB_READ && ft->body) {
        ft->watch.force = 1;
    } else {
        filefree(g, ft);
        ft->state = FTAB_UNREAD;
    }
    scheddirty(&g->sched);
}

struct FileBar {
    i32 closed;  // the tab to close, else -1
    b32 others;  //   and the others instead
    b32 all;     //   or all of them
    i32 reload;  // the tab to read again, else -1
};

// The line Open in editor opens a tab's file at, and its column (0 for
// none): the place last gone to (a link's first line, or the current
// find match), else its band's first line (a tab restored), else the
// top one in view.
static i32 filetabline(FileTab *ft, i32 *col)
{
    i32 line = ft->goline>0 ? ft->goline : ft->band0>0 ? ft->band0 : ft->top+1;
    *col = ft->goline>0 ? ft->gocol : 0;
    if (ft->body && ft->mode==FMODE_TEXT) {
        i32 in = fileclamp(line, ft->body->lines);
        *col = in==line ? *col : 0;
        line = in;
    }
    return line>0 ? line : 1;
}

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
            i32      col  = 0;
            i32      line = filetabline(ft, &col);
            EditJump j    = filejump(g, &a, ft->host, ft->path, line, col);
            Str      tip  = j.why.len ? Str{} : filejumptip(&a, &j);
            if (menutip(g, &a, "Open in editor", j.why, tip)) {
                filejumprun(g, a, ft->host, ft->path, line, col);
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
                filesetshow(set, i);  // picked: as opened, and checked
                set->frontset = 0;
                filetabstale(g, set->tabs[i]);
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
                        "lines numbered, as Markdown, or as an image, in a tab of the "
                        "session shown.");
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

// Find in a file (Ctrl/Cmd+F while Files has the focus, or Edit, Find
// in file): one bar for the pane, g->ffind, over the file shown, its
// text searched as it is, or a Markdown file as rendered. It stays open
// across tabs, reloads and modes, shown only over a text, and searches
// each body or mode anew from the tab's top line, scrolling only when a
// hit is typed for or stepped to.

// Why the file shown can't be searched, else empty. One still being
// read can be: the bar shows once its text comes.
static Str filefindwhy(Gui *g)
{
    FileTab *ft = filesetfront(&filesroot(g)->files);
    if (!ft) {
        return "No file is open";
    }
    FileBody *b      = ft->body;
    b32       notext = ft->state==FTAB_FAILED || (!b && fileimgname(ft->path)) ||
                       (b && (b->kind==FBODY_IMAGE || !b->body.len ||
                              (b->flags&FTEXT_BINARY && !ft->astext)));
    return notext ? Str("The file shown has no text") : Str{};
}

static void filefindopen(Gui *g)
{
    FileFindBar *fb = &g->ffind;
    FileTab     *ft = filesetfront(&filesroot(g)->files);
    if (filefindwhy(g).len) {
        return;
    }
    setpane(g, PANE_FILES, 1);
    fb->open   = 1;
    fb->mode   = FBAR_FIND;
    fb->serial = ft->serial;
    fb->focus  = 1;
    scheddirty(&g->sched);
}

// Closed; the focus, if the bar had it, to the text it was over, so
// that the Files keys go on working.
static void filefindclose(Gui *g)
{
    FileFindBar *fb   = &g->ffind;
    b32          had  = fb->held || fb->active;
    FileBody    *b    = fb->body;
    DocView     *d    = !b ? 0 : fb->bmode==FMODE_MARKDOWN ? b->md : b->text;
    fb->open   = 0;
    fb->active = 0;
    fb->held   = 0;
    fb->focus  = 0;
    fb->reveal = 0;
    if (had && d && d->win) {
        ImGui::FocusWindow(d->win);
    }
    scheddirty(&g->sched);
}

// Go to line (Ctrl+G while Files has the focus, physical Ctrl on macOS,
// or Edit, Go to line): the bar in line mode, over the file shown. One
// already in line mode only takes the focus again, keeping what was
// typed and where it leaves to: the find bar, if it was open.
static void filelineopen(Gui *g)
{
    FileFindBar *fb = &g->ffind;
    FileTab     *ft = filesetfront(&filesroot(g)->files);
    if (filefindwhy(g).len) {
        return;
    }
    setpane(g, PANE_FILES, 1);
    if (!(fb->open && fb->mode==FBAR_LINE)) {
        fb->back    = fb->open && fb->mode==FBAR_FIND;
        fb->open    = 1;
        fb->mode    = FBAR_LINE;
        fb->line[0] = 0;
        fb->bad     = 0;
        fb->serial  = ft->serial;
    }
    fb->focus = 1;
    scheddirty(&g->sched);
}

// Line mode left: to the find bar if it was open, else closed. With
// focus, the focus to the text, so that the Files keys go on working.
static void filelineleave(Gui *g, b32 focus)
{
    FileFindBar *fb = &g->ffind;
    if (!fb->open || fb->mode!=FBAR_LINE) {
        return;
    }
    fb->open   = fb->back;
    fb->mode   = fb->back ? FBAR_FIND : FBAR_LINE;
    fb->back   = 0;
    fb->bad    = 0;
    fb->active = 0;
    fb->held   = 0;
    fb->focus  = 0;
    fb->reveal = fb->open && fb->reveal;
    FileTab  *ft = filesetfront(&filesroot(g)->files);
    FileBody *b  = ft ? ft->body : 0;
    DocView  *d  = !b ? 0 : ft->mode==FMODE_MARKDOWN ? b->md : b->text;
    if (focus && d && d->win) {
        ImGui::FocusWindow(d->win);
    } else if (focus && g->fileswindow) {
        ImGui::FocusWindow(g->fileswindow);  // the text, not drawn yet
    }
    if (focus) {
        // FocusWindow restores the text's last nav item, and the keys
        // typed in the field left the nav cursor showing: hidden, else
        // the window's Esc takes it for keyboard navigation and the next
        // Esc would not close the find bar
        ImGui::SetNavCursorVisible(false);
    }
    scheddirty(&g->sched);
}

// To lines l of a tab's file, as a link to them goes: as text, banded,
// scrolled to once, and the place Open in editor opens, its column too.
static void filegoto(Gui *g, FileTab *ft, FrefLines l)
{
    ft->mode   = FMODE_TEXT;
    ft->band0  = l.line0;
    ft->band1  = l.line1;
    ft->goline = l.line0;
    ft->gocol  = l.col;
    ft->jump   = l.line0;
    ft->jumped = 0;
    scheddirty(&g->sched);
}

// Open in editor goes where the current hit begins, its line and column.
static void filefindgo(Gui *g, FileTab *ft)
{
    FileFindBar *fb = &g->ffind;
    FileBody    *b  = fb->body;
    iz           at = -1;
    if (b && fb->bmode==FMODE_MARKDOWN && fb->md->cur>=0) {
        at = fb->md->hits[fb->md->cur].off;
    } else if (b && fb->bmode==FMODE_TEXT && fb->text->cur>=0) {
        FindHit *h = &fb->text->hits[fb->text->cur];
        at = fb->text->chunkat[h->msg] + h->off;
    }
    if (at>=0 && ft->body==b) {
        fileoffline(b->body, at, &ft->goline, &ft->gocol);
    }
}

// The next hit (dir 1) or the previous (-1), wrapping around among
// those found, brought into view.
static void filefindnext(Gui *g, FileTab *ft, i32 dir)
{
    FileFindBar *fb  = &g->ffind;
    b32          md  = fb->bmode == FMODE_MARKDOWN;
    iz           n   = md ? fb->md->hits.len : fb->text->hits.len;
    i32         *cur = md ? &fb->md->cur : &fb->text->cur;
    if (!fb->open || !fb->body || !n) {
        return;
    }
    *cur = (i32)(*cur<0 ? (dir>0 ? 0 : n-1) : (*cur + dir + n) % n);
    fb->pick   = 0;
    fb->reveal = 1;
    filefindgo(g, ft);
    scheddirty(&g->sched);
}

// A body let go: the searches over it emptied, so that one made again,
// maybe at its address, is searched again.
static void filefindforget(Gui *g, FileBody *b)
{
    FileFindBar *fb = &g->ffind;
    if (fb->body == b) {
        filefindreset(fb->text, {}, {});
        searchreset(fb->md, 0, {});
        fb->body   = 0;
        fb->reveal = 0;
    }
}

// Scroll a Markdown view to bring a hit in it into view, below the bar,
// centered, if it is out of it. Returns whether it could, laid out.
static b32 filemdreveal(Gui *g, FileBody *b, FindHit h)
{
    DocView *d = b->md;
    View    *v = d->v;
    if (!d->win || v->lastw<=0 || !v->msgs.len) {
        return 0;
    }
    f32        pad  = 4*g->scale;
    MsgLayout *ml   = msglayout(v, 0, &g->measure, v->lastw, g->scale);
    MsgRow     r    = msgrowat(&g->measure, msgdoc(v, 0), ml, h.off);
    f64        y0   = v->msgs[0].top + pad + r.y;
    f64        y1   = y0 + r.h;
    f64        cur  = d->win->Scroll.y;
    f32        vish = d->win->Size.y;
    f32        up   = g->ffind.barh;
    if (y0<cur+up || y1>cur+vish) {
        f64 want = (y0 + y1)/2 - (up + vish)/2;
        d->scrollto = want<0 ? 0 : want;
    }
    return 1;
}

// The bar in line mode (filefindbar): the line typed, gone to on Enter,
// or 'Not a line'; Enter on nothing, Esc or its close leaves.
static void filelinebar(Gui *g, FileTab *ft, FileBody *b, ImVec2 r0, ImVec2 r1,
                        b32 held, b32 esc)
{
    FileFindBar *fb = &g->ffind;
    Arena        a  = g->frame;
    FindBar      bar = {};
    bar.child    = "##filefindbar";
    bar.name     = "file line";
    bar.widest   = "888888 of 888888+";
    bar.arrows   = 0;
    bar.nextrect = -1;
    bar.closetip = "Leave Go to line (Esc)";
    findbarbegin(g, &bar, r0, r1);
    if (fb->focus) {
        ImGui::FocusWindow(ImGui::GetCurrentWindow());
        ImGui::SetKeyboardFocusHere();
        fb->focus = 0;
    }
    // a11y: "Go to line"
    b32 enter = ImGui::InputTextWithHint("##line", "Line, line:column or first-last", fb->line,
                                         countof(fb->line),
                                         ImGuiInputTextFlags_EnterReturnsTrue|
                                         ImGuiInputTextFlags_AutoSelectAll);
    itemnamed(g, "file line field");
    if (ImGui::IsItemEdited()) {
        fb->bad = 0;
    }
    esc |= ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (ImGui::IsItemDeactivated() || held);
    fb->active = ImGui::IsItemActive();

    Buf cb(&a, 64);
    if (fb->bad) {
        print(&cb, "Not a line");
    } else {
        print(&cb, "of ");
        print(&cb, (i64)b->lines);
    }
    Str count = finish(&cb);
    iz  n     = count.len<countof(fb->shown) ? count.len : countof(fb->shown);
    copybytes(fb->shown, count.data, n);
    fb->shownlen = (i32)n;
    findbarend(g, &bar, count);
    fb->held  = bar.focused;
    fb->barh  = bar.bottom - r0.y;
    fb->drawn = 1;

    if (esc || bar.close) {
        filelineleave(g, 1);
    } else if (enter) {
        Str       t = trim(Str{(u8 *)fb->line, (iz)__builtin_strlen(fb->line)});
        FrefLines l = {};
        if (!t.len) {
            filelineleave(g, 1);
        } else if (filegotoparse(t, &l)) {
            filegoto(g, ft, l);
            filelineleave(g, 1);
        } else {
            fb->bad   = 1;
            fb->focus = 1;  // Enter let go of the field: take it back
            scheddirty(&g->sched);
        }
    }
}

// The bar, over the text drawn from r0 to r1 (filefindbar's chrome),
// drawn only over a text or Markdown body, after it. Its keys are the
// Files window's, so a tab with no text never steps: Ctrl+G to go to a
// line (physical Ctrl on macOS); F3 and Shift+F3, on macOS Cmd+G and
// Cmd+Shift+G too, and Esc from the text; in the bar, Enter,
// Shift+Enter, the arrows, and Ctrl/Cmd(+Shift)+G.
static void filefindbar(Gui *g, FileTab *ft, FileBody *b, ImVec2 r0, ImVec2 r1)
{
    FileFindBar *fb = &g->ffind;
    Arena        a  = g->frame;
    // Go to line. In the find field Ctrl+G is the next match, the bar's
    // own key going ahead of this one; on macOS ImGui holds back Super
    // (physical Ctrl) chords from an active field, so there it is read
    // as it is, as the prompt box does, once a frame
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::Shortcut((g->mac ? ImGuiMod_Super : ImGuiMod_Ctrl)|ImGuiKey_G)) {
        filelineopen(g);
    } else if (g->mac && fb->open && fb->mode==FBAR_FIND && fb->active &&
               io.KeyMods==ImGuiMod_Super && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        filelineopen(g);
    }
    if (!fb->open) {
        return;
    }
    // Esc while the bar has the focus but its field does not, as after
    // a click on an arrow, goes by last frame's, as drawfind's
    b32 held = fb->held;
    fb->held = 0;
    i32 step = 0;
    if (fb->mode == FBAR_FIND) {
        if (ImGui::Shortcut(ImGuiKey_F3)) {
            step = 1;
        }
        if (ImGui::Shortcut(ImGuiMod_Shift|ImGuiKey_F3)) {
            step = -1;
        }
        if (g->mac && ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_G)) {
            step = 1;  // Cmd+G
        }
        if (g->mac && ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_G)) {
            step = -1;
        }
    }
    // Esc from the text: not while ImGui's keyboard navigation shows its
    // cursor or is in a menu bar, nor over a popup, as escapecancel; nor
    // while the field has the keyboard, which takes its own, so no
    // RouteOverActive
    ImGuiContext &c   = *GImGui;
    b32           nav = (c.NavCursorVisible && c.NavId && c.NavId!=c.ActiveId) ||
                        c.NavLayer!=ImGuiNavLayer_Main;
    b32 esc = !fb->active && !held && !nav &&
              !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
              ImGui::Shortcut(ImGuiKey_Escape);
    if (fb->mode == FBAR_LINE) {
        filelinebar(g, ft, b, r0, r1, held, esc);
        return;
    }

    FindBar bar = {};
    bar.child    = "##filefindbar";
    bar.name     = "file find";
    bar.widest   = "888888 of 888888+";
    bar.arrows   = 1;
    bar.nextrect = -1;
    findbarbegin(g, &bar, r0, r1);
    if (fb->focus) {
        ImGui::FocusWindow(ImGui::GetCurrentWindow());  // over the text's menu, say
        ImGui::SetKeyboardFocusHere();
        fb->focus = 0;
    }
    // a11y: "Find in the file"
    b32 enter = ImGui::InputTextWithHint("##find", "Find in the file", fb->q, FIELD_MAX,
                                         ImGuiInputTextFlags_EnterReturnsTrue|
                                         ImGuiInputTextFlags_AutoSelectAll);
    itemnamed(g, "file find field");
    b32 edited = ImGui::IsItemEdited();
    esc |= ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (ImGui::IsItemDeactivated() || held);
    fb->active = ImGui::IsItemActive();
    if (enter) {
        fb->focus = 1;  // Enter lets go of the field: take it back
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiKey_G)) {
        step = 1;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl|ImGuiMod_Shift|ImGuiKey_G)) {
        step = -1;
    }

    // Searched again for a new text, body or mode, unless closing: Esc
    // in the field reverts its text, which counts as an edit, so that
    // text is searched when the bar is next drawn (below)
    b32         md  = ft->mode == FMODE_MARKDOWN;
    ViewSearch *ms  = fb->md;
    FileFind   *ts  = fb->text;
    if (!esc && (edited || fb->body!=b || fb->bmode!=ft->mode)) {
        Str q = {(u8 *)fb->q, (iz)__builtin_strlen(fb->q)};
        fb->from = {};
        if (md) {
            searchreset(ms, b->md->v, q);
            DocView *d = b->md;
            if (d->win && d->v->lastw>0 && d->v->msgs.len) {
                Anchor an = viewanchor(d->v, &g->measure, d->v->lastw, g->scale, 4*g->scale,
                                       d->win->Scroll.y);
                fb->from.off = an.msg>=0 && an.off>=0 ? an.off : 0;
            }
        } else {
            filefindreset(ts, b->body, q);
            View *v     = b->text->v;
            i32   chunk = ft->top / FILES_CHUNK;
            if (chunk < v->msgs.len) {
                fb->from.msg = chunk;
                fb->from.off = filelineoff(v->msgs[chunk].text, ft->top % FILES_CHUNK);
            }
        }
        fb->body   = b;
        fb->bmode  = ft->mode;
        fb->pick   = 1;
        fb->typed  = edited;
        fb->reveal = 0;
    }

    Slice<FindHit> hits = md ? ms->hits : ts->hits;
    i32           *cur  = md ? &ms->cur : &ts->cur;
    iz             n0   = hits.len;
    i32            c0   = *cur;
    b32            done = 1;
    if (!esc) {
        done = md ? searchstep(ms, a, FIND_BUDGET) : filefindstep(ts, FIND_BUDGET);
        hits = md ? ms->hits : ts->hits;
    }
    if (!esc && *cur<0 && fb->pick) {
        // The first hit from the tab's top line down, else from the
        // start once all are found; none while closing, as nothing is
        // shown to go to
        iz i = hitfrom(hits, fb->from.msg, fb->from.off);
        if (i < hits.len) {
            *cur = (i32)i;
        } else if (done && hits.len) {
            *cur = 0;
        }
        if (*cur >= 0) {
            fb->pick   = 0;
            fb->reveal = fb->typed;
            if (fb->typed) {
                filefindgo(g, ft);
            }
        }
    }

    // The count: '+' while it searches, or past FILES_HITS
    iz  qlen = md ? ms->q.len : ts->q.len;
    b32 full = md ? ms->full : ts->full;
    Buf cb(&a, 64);
    if (qlen && !hits.len && done) {
        print(&cb, "No matches");
    } else if (qlen) {
        print(&cb, (i64)(*cur>=0 ? *cur+1 : 0));
        print(&cb, " of ");
        print(&cb, (i64)hits.len);
        print(&cb, !done || full ? Str("+") : Str(""));
    }
    Str count = finish(&cb);
    iz  n     = count.len<countof(fb->shown) ? count.len : countof(fb->shown);
    copybytes(fb->shown, count.data, n);
    fb->shownlen = (i32)n;
    findbarend(g, &bar, count);
    fb->held  = bar.focused;
    fb->barh  = bar.bottom - r0.y;
    fb->done  = done;
    fb->drawn = 1;

    if (esc || bar.close) {
        filefindclose(g);
        if (esc && edited) {
            fb->body = 0;  // the reverted text searched on reopening, not gone to
        }
    } else if (enter || bar.next || bar.prev || step) {
        i32 dir = enter ? (ImGui::GetIO().KeyShift ? -1 : 1) : bar.prev ? -1 :
                  bar.next ? 1 : step;
        filefindnext(g, ft, dir);
    }
    if (!done || hits.len!=n0 || *cur!=c0 || fb->reveal) {
        scheddirty(&g->sched);  // more next frame, or what it changed drawn
    }
}

// The Files pane: the shown root's tabs, and the front one's file, read
// as it is first shown, with its notes, its toolbar and its text.
static void drawfiles(Gui *g)
{
    Arena       a    = g->frame;
    GuiSession *root = filesroot(g);
    FileSet    *set  = &root->files;
    ImVec2      wp   = ImGui::GetWindowPos();
    ImVec2      ws   = ImGui::GetWindowSize();
    g->rects[RECT_FILESPANE] = {wp.x, wp.y, wp.x + ws.x, wp.y + ws.y};
    g->fshown.v   = 0;
    g->fnoteslen  = 0;
    g->fimgshown  = 0;
    if (!set->n) {
        g->fshowing = 0;
        filesempty(g);
        return;
    }
    // The shown tab checked as the window or the pane takes the focus,
    // or the pane shows
    b32 win   = guifocused(g);
    b32 focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if ((win && !g->fileswin) || (focus && !g->filesfocus) || ImGui::IsWindowAppearing()) {
        FileTab *front = filesetfront(set);
        front->watch.stale = 1;
    }
    g->fileswin   = win;
    g->filesfocus = focus;

    FileBar bar = filetabs(g, root);
    FileTab *rt = bar.reload>=0 ? set->tabs[bar.reload] : 0;
    if (rt && !(rt->host && filehostwhy(filehost(g, rt->host)).len)) {
        fileforce(g, rt);  // not one whose host can't serve it
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
    if (g->ffind.serial != ft->serial) {
        filelineleave(g, 0);  // Go to line is the tab's it was opened over
        g->ffind.serial = ft->serial;  // opened for another tab: not this one's focus
        g->ffind.focus  = 0;
    }
    GuiHost *fh = filehost(g, ft->host);
    if (ft->state==FTAB_FAILED && ft->host && fh && fh->opens!=ft->opens) {
        ft->state = FTAB_UNREAD;  // its host connected again: once more
    } else if (ft->state==FTAB_FAILED && ft->watch.stale && !(ft->host && filehostwhy(fh).len)) {
        // Marked since it failed (a tool may have put it back): read once
        // more, as often as filenext allows
        i64      now = os_time(g->os);
        FileNext n   = filenext(&ft->watch, now);
        if (n.wake) {
            schedat(&g->sched, g->sched.now + (n.wake - now)*1000, DUE_APP);
        }
        if (n.check) {
            ft->watch.stale = 0;
            ft->watch.sent  = now;
            ft->state       = FTAB_UNREAD;
        }
    }
    if (ft->state==FTAB_UNREAD && ft->host) {
        filefetch(g, ft, 0);
    } else if (ft->state == FTAB_UNREAD) {
        fileload(g, ft);
    } else {
        filewatch(g, ft);  // changed, maybe, since it was read
    }
    FileBody *b = ft->body;
    Str       farwhy = ft->host ? filehostwhy(fh) : Str{};

    // The toolbar: Markdown or text, Wrap, Reload, Reveal, Open in
    // editor, then the path; an image's Fit is drawimage's, below it
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
    if (b && b->kind==FBODY_TEXT && ft->mode==FMODE_TEXT) {
        if (checkbox("Wrap", &ft->wrap)) {
            g->filewrap = ft->wrap;  // new tabs take it
        }
        itemnamed(g, "file wrap");
        itemtip("Wrap long lines at the pane's width");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(farwhy.len > 0);
    if (iconbutton(ICON_REFRESH "##filereload")) {
        fileforce(g, ft);
        b = ft->body;
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
        Str rwhy = {};
        Str here = filelocal(g, &a, ft->host, ft->path, &rwhy);
        ImGui::BeginDisabled(rwhy.len > 0);
        if (iconbutton(ICON_FOLDER "##filereveal")) {
            guishow(g, here);
        }
        ImGui::EndDisabled();
        itemnamed(g, "file reveal");
        whytip(rwhy.len ? rwhy : filerevealtip());
        ImGui::SameLine();

        i32      col  = 0;
        i32      line = filetabline(ft, &col);
        EditJump j    = filejump(g, &a, ft->host, ft->path, line, col);
        ImGui::BeginDisabled(j.why.len > 0);
        if (ImGui::Button("Open in editor")) {
            filejumprun(g, a, ft->host, ft->path, line, col);
        }
        ImGui::EndDisabled();
        itemnamed(g, "file editor");
        whytip(j.why.len ? j.why : filejumptip(&a, &j));
        iz n = j.why.len<(iz)sizeof(g->fjumpwhy) ? j.why.len : (iz)sizeof(g->fjumpwhy);
        copybytes(g->fjumpwhy, j.why.data, n);
        g->fjumpwhylen = (i32)n;
        ImGui::SameLine();
    }
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
    if (b->kind == FBODY_IMAGE) {
        // Its size, from g->frame itself: drawimage takes a copy of it
        Buf nb(&g->frame, 64);
        print(&nb, (i64)b->w);
        putbyte(&nb, 'x');
        print(&nb, (i64)b->h);
        print(&nb, " pixels");
        ImgDrawn d = drawimage(g, b->tex, b->w, b->h, &ft->fit, finish(&nb),
                               "Scale it to fit the pane");
        g->fimgshown = d.shown;
        g->fimgsize  = d.size;
        g->fimgroom  = d.room;
        g->fshowing  = 0;
        return;
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
    if (ft->band0>b->lines && !(b->flags&FTEXT_CUT)) {
        // A cut file's own note says why
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
        // Its find's hits marked, and the current one brought into view
        FileFindBar *fb  = &g->ffind;
        b32          fon = fb->open && (fb->mode==FBAR_FIND || fb->back) && fb->body==b &&
                           fb->bmode==FMODE_MARKDOWN;
        d->find = fon ? fb->md : 0;
        if (fon && fb->reveal && fb->md->cur>=0 &&
                filemdreveal(g, b, fb->md->hits[fb->md->cur]) && !pending) {
            fb->reveal = 0;
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
        filefindbar(g, ft, b, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
    // Go to line's column, on its line
    dl->jumpcol = dl->jump0 && ft->goline==ft->band0 && ft->band0<=lines ? ft->gocol : 0;
    // Its find's hits marked, under the bar, and the current one brought
    // into view
    FileFindBar *fb  = &g->ffind;
    b32          fon = fb->open && (fb->mode==FBAR_FIND || fb->back) && fb->body==b &&
                       fb->bmode==FMODE_TEXT;
    FileFind    *ff  = fb->text;
    dl->find    = fon ? ff : 0;
    dl->reveal  = fon && fb->reveal && ff->cur>=0 ? ff->hits[ff->cur] : FindHit{};
    dl->covered = fb->barh;
    drawdoc(g, b->text, "##filetext", 0, dl);
    itemnamed(g, "file text");
    if (dl->jumped && dl->reveal.len) {
        fb->reveal = 0;
    }
    if (dl->jumped) {
        ft->jumped  = 1;
        dl->restore = 0;
    } else if (dl->restored) {
        dl->restore = 0;
    }
    filefindbar(g, ft, b, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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


// Where line n (1-based), column col of the shown text view is on screen,
// the top left of its place on its row, the row's height in *h; else
// (-1, -1).
static ImVec2 filespotat(Gui *g, i32 n, i32 col, f32 *h)
{
    FileShown *fs = &g->fshown;
    View      *v  = fs->v;
    i32        ci = (n - 1)/FILES_CHUNK;
    *h = 0;
    if (!v || n<1 || ci>=v->msgs.len) {
        return v2(-1, -1);
    }
    Str        chunk = v->msgs[ci].text;
    i32        lo    = filelineoff(chunk, (n - 1)%FILES_CHUNK);
    i32        off   = lo + filecolbyte(cuthead(chunk, lo), col);
    MsgLayout *ml    = msglayout(v, ci, &g->measure, fs->width, g->scale);
    MsgRow     r     = msgrowat(&g->measure, msgdoc(v, ci), ml, off);
    *h = r.h;
    f32 y = fs->wpos.y + fs->pad + (f32)v->msgs[ci].top + r.y - fs->scrolly;
    f32 x = fs->wpos.x + fs->pad + fs->gutter - fs->scrollx + r.x;
    return v2(x, y);
}

// The line and column where the current match of the find over the
// front tab's text begins, if there is one.
static b32 filefindcur(Gui *g, FileTab *ft, i32 *line, i32 *col)
{
    FileFindBar *fb = &g->ffind;
    FileFind    *f  = fb->text;
    if (!ft || !fb->body || fb->body!=ft->body || fb->bmode!=FMODE_TEXT || f->cur<0) {
        return 0;
    }
    FindHit *h = &f->hits[f->cur];
    fileoffline(fb->body->body, f->chunkat[h->msg] + h->off, line, col);
    return 1;
}
