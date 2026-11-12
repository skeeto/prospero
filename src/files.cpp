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
// touches them. Files on other hosts come later, through their engines.
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
static void filedrop(Gui *g, FileTab *ft)
{
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

// Read a tab's file, on this computer, into a body of its own, else say
// why not in the tab.
static void fileload(Gui *g, FileTab *ft)
{
    g->fileloads++;
    scheddirty(&g->sched);
    ft->err = {};
    Region *sr = os_reserve(g->os, FILES_SCRATCH);
    if (!sr) {
        ft->state = FTAB_FAILED;
        ft->err   = "There is no memory to show it";
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
        ft->state = FTAB_FAILED;
        ft->err   = why;
        return;
    }

    // Past FILES_MAX: up to its last whole line, and so cut
    Str raw = rd.data;
    b32 cut = rd.size > raw.len;
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
        ft->state = FTAB_FAILED;
        ft->err   = "There is no memory to show it";
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
    b->size  = rd.size;
    b->mtime = st.mtime;
    b->sum   = hash(b->body);
    b->flags = ftx.flags;
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
// the link is (LinkFrom), then shown in the tabs of its session's root.
// Until engines read files (fs.read), one on another computer only
// says so.

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
    b32 far;   // on another computer
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
        print(&b, ": its file is on another computer");
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
    if (!f.path.len || f.far) {
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
    Str why = f.err.len ? f.err : f.far ? Str("its file is on another computer") : Str{};
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
// this computer, its answer TASK_OPENFILE

// Why Open file can't be used now, else empty.
static Str fileaskwhy(Gui *g)
{
    GuiSession *s = g->sess;
    if (!g->main) {
        return "No engine";
    } else if (s && s->host->kind==GH_REMOTE) {
        return "Open file opens files on this computer, and this session is on another";
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
    g->fask.host = 0;
    scheddirty(&g->sched);
}

// A path typed in Open file, maybe with a line or range after it
// (path:N, path:A-B), as fileref takes it: the file's path resolved
// against the shown session's folder, else why not, its first letter
// a capital, for the modal.
static FilePath fileasked(Gui *g, Arena *a, Str text, FileRef *ref)
{
    GuiSession *s    = g->sess;
    GuiHost    *h    = g->main;
    Str         base = s && !sessnofolder(s) ? termcwd(s) : Str{};
    *ref = fileref(a, text);
    FilePath p = text.len ? filepath(a, *ref, base, h ? h->home : Str{}, HOST_WINDOWS, 0)
                          : FilePath{{}, "type a path"};
    if (p.err.len) {
        iz  n   = p.err.len<countof(g->fask.err) ? p.err.len : countof(g->fask.err);
        copybytes(g->fask.err, p.err.data, n);
        g->fask.err[0] = (u8)(p.err[0]>='a' && p.err[0]<='z' ? p.err[0]-0x20 : p.err[0]);
        p.err = {g->fask.err, n};  // kept past the frame, for the modal
        return p;
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

// Open file's OK: the path typed, its answer posted, else why not.
static Str fileaskok(Gui *g, Str text)
{
    Arena    a   = g->frame;
    FileRef  ref = {};
    FilePath p   = fileasked(g, &a, text, &ref);
    if (p.err.len) {
        return p.err;
    }
    mailpost(g->tasks, TASK_OPENFILE, g->fask.p.id, fileanswerput(&a, p, ref), 1);
    return {};
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
            if (menutip(g, &a, "Reload", {}, "Read the file again")) {
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
    if (bar.reload >= 0) {
        filefree(g, set->tabs[bar.reload]);
        set->tabs[bar.reload]->state = FTAB_UNREAD;
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
        return;
    }
    if (ft->state == FTAB_UNREAD) {
        fileload(g, ft);
    }
    FileBody *b = ft->body;

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
    if (iconbutton(ICON_REFRESH "##filereload")) {
        filefree(g, ft);
        ft->state = FTAB_UNREAD;
        b = 0;
    }
    itemnamed(g, "file reload");
    itemtip("Reload: read the file again");
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
        g->fshowing = 0;
        return;
    }
    b->drawn = g->total.frames;
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
