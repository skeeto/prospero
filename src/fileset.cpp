// A root session's Files tabs as a set, the labels shown on them, and a
// file's bytes as the text they show, apart from ImGui and the engine
// (files.cpp draws and reads them).
//
// The set is in the tab bar's order, as a TermSet is, front the tab
// shown. A tab added goes last and shows; a tab dropped shows its right
// neighbour, else its left. Unlike a terminal, a file costs nothing to
// open again, so a full set takes a new tab by closing the one least
// recently opened, picking a tab counting as opening it.
//
// This is free and unencumbered software released into the public domain.

struct FileBody;

enum : i32 {
    FILES_TABS    = 8,       // tabs a root may have
    FILES_CHUNK   = 256,     // lines per chunk message
    FILES_LINES   = 100000,  // lines kept
    FILES_COLS    = 1000,    // columns a line runs to, unwrapped
    FILES_SECTION = 64,      // bytes of a heading's slug kept
    FILES_HITS    = 1<<18,   // find hits kept
    FILES_CHUNKS  = (FILES_LINES + FILES_CHUNK - 1)/FILES_CHUNK,  // at most
};

enum : iz {
    FILES_MAX    = (iz)4 << 20,   // bytes of text kept, normalized
    FILES_CACHE  = (iz)8 << 20,   // a body's own layout cache
    FILES_BUDGET = (iz)64 << 20,  // all bodies, image textures included
    FILES_HITMEM = (iz)4 << 20,   // find hits' Region, FILES_HITS of them
};

enum : i64 {  // epoch ms
    FILES_GAP    = 1000,       // the least time between a tab's checks
    FILES_LOST   = 30*1000,    // a check unanswered this long was lost
    FILES_COARSE = 2000,       // an mtime this near its read may hide a change
};

enum : i32 {
    FILES_TRIES = 3,  // checks that failed in a row, kept stale, then left
};

// Whether a tab's file may have changed since it was read, and the
// checking of it (filenext). Nothing polls: marks set stale, as tools,
// jobs and turns end on its host, as a turn is rewound or a cut undone,
// as the window or the pane takes the focus, and as the tab is picked
// or a link to it clicked.
struct FileWatch {
    b32 stale;   // it may have changed since last checked
    b32 force;   // Reload: read it again, whatever its stat says
    i64 id;      // a check in flight: its fs.stat's id, -1 its read; else 0
    i32 fails;   // checks in a row that failed (FILES_TRIES)
    i64 sent;    // when the last check began, epoch ms here
    i64 readat;  // when its body was read, epoch ms on its host's clock
};

struct FileNext {
    b32 check;  // check it now
    i64 wake;   // else when to look again, epoch ms, or 0 for no sooner
                // than the next mark
};

// Whether to check a tab's file as of now (epoch ms): only once it may
// have changed (stale), with its last check answered or lost, once per
// FILES_GAP at most, but at once for Reload (force). A mark while a
// check is in flight sets stale again, for another once it is done.
// Nothing while it is read again (id -1): that read, however slow, ends
// on its own, and checking meanwhile would restart it.
static FileNext filenext(FileWatch *w, i64 now)
{
    FileNext r = {};
    if ((!w->stale && !w->force) || w->id<0) {
        return r;
    }
    i64 at = w->force ? 0 : w->sent + FILES_GAP;
    if (w->id) {
        at = at>w->sent+FILES_LOST ? at : w->sent+FILES_LOST;
    }
    if (now < at) {
        r.wake = at;
    } else {
        r.check = 1;
    }
    return r;
}

// Whether a file read at readat, size and mtime then (its host's
// clock), is to be read again, its stat now size1 and mtime1: either
// changed, or its mtime was within FILES_COARSE of the read, when a
// write since may have left a coarse mtime as it was.
static b32 filechanged(i64 size, i64 mtime, i64 readat, i64 size1, i64 mtime1)
{
    i64 near = readat - mtime;
    return size!=size1 || mtime!=mtime1 || (near<FILES_COARSE && near>-FILES_COARSE);
}

// A path's type on this computer, a device or a network path refused
// (STAT_OTHER) before anything touches it: on Windows os_stat opens
// the path, and opening a device can do things. On the window's file
// workers (files.cpp), never on its own thread.
static Stat filestat(Os *os, Arena scratch, Str path)
{
    if (devicepath(path, HOST_WINDOWS) || netpath(path, HOST_WINDOWS)) {
        return {0, 0, STAT_OTHER};
    }
    return os_stat(os, scratch, path);
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

// A file on this computer as filereadall read it into its buffer.
struct FileRead {
    i64 size;     // the file's, as its stat said; what came, if it changed twice
    i64 mtime;
    i64 readat;   // epoch ms, taken before the stat
    iz  len;      // bytes in the buffer
    b32 changed;  // it changed while it was read, twice: what came is kept
    b32 toobig;   // an image past max, not read
    Str err;      // why it was not read, as a tab says it, else null
};

// A file on this computer, at most max bytes from its start, into buf,
// for a Files tab, on a worker (c, else null as in tests) with a
// heartbeat before the stat and between pieces of FILES_PART: stat'ed
// first, so a folder, a device or a network path is refused as filestat
// refuses it, and an image (image) past max is not read at all. A size
// changed by the end reads it again from the start, once; changed again,
// what came is kept (changed), as fs.read's parts are (onfsread).
static FileRead filereadall(FsCall *c, Os *os, Arena scratch, Str path, u8 *buf, iz max,
                            b32 image)
{
    FileRead r = {};
    for (i32 again = 0;; again++) {
        if (c && !fsat(c, path, again==0)) {
            r.err = "The file system is not responding";
            return r;
        }
        r.readat = os_time(os);
        Stat st = filestat(os, scratch, path);
        r.err = filestatwhy(st.type);
        if (r.err.len) {
            return r;
        } else if (image && st.size>max) {
            r.toobig = 1;
            return r;
        }
        r.size  = st.size;
        r.mtime = st.mtime;
        r.len   = 0;
        b32 changed = 0;
        while (r.len < max) {
            if (c && (fsgone(c) || (r.len && !fsat(c, path)))) {
                r.err = "The file system is not responding";
                return r;
            }
            Arena t    = scratch;  // for the piece, copied into buf
            iz    want = max - r.len;
            want = want<(iz)FILES_PART ? want : (iz)FILES_PART;
            Read  rd   = os_readat(os, &t, path, r.len, want);
            if (rd.err != READ_OK) {
                r.err = rd.err==READ_NOTFOUND ? Str("No such file") :
                        rd.err==READ_NOTFILE  ? Str("Not a regular file") :
                                                Str("It could not be read");
                return r;
            }
            copybytes(buf+r.len, rd.data.data, rd.data.len);
            r.len   += rd.data.len;
            changed |= rd.size != st.size;
            if (rd.data.len < want) {
                break;
            }
        }
        if (!changed) {
            return r;
        } else if (again) {
            r.changed = 1;
            r.size    = r.len;
            return r;
        }
    }
}

enum : i32 {  // FileTab.state
    FTAB_UNREAD,   // not read, or its body freed: read when shown
    FTAB_READING,  // a read in flight
    FTAB_READ,     // its body holds the file
    FTAB_FAILED,   // err says why
};

enum : i32 {  // FileTab.mode
    FMODE_TEXT,
    FMODE_MARKDOWN,
    FMODE_IMAGE,
};

struct FileTab {
    FileTab  *next;       // on the spare list, once closed
    u32       serial;     // its tab's ID
    i64       opened;     // the set's count when last opened or picked
    u64       host;       // its host's uid, 0 for this computer
    b32       win;        // a Windows host's: compared without case
    Str       path;       // absolute and normalized
    iz        pathcap;    // its room, used again by a spare
    i32       state;      // FTAB_*
    i32       mode;       // FMODE_*
    b32       wrap;
    i32       band0;      // the cited lines, banded, 1-based; else 0
    i32       band1;
    i32       jump;       // a line to scroll to, 1-based, else 0
    b32       jumped;     // and the view went there
    i32       top;        // the line at the view's top, 0-based
    f32       dy;         // and how far past its top the view is
    f32       scrollx;    // unwrapped
    i32       goline;     // where Open in editor opens, 1-based: the line
    i32       gocol;      // last gone to (a link's first, or the current
                          // find match's) and its column; else 0
    FileWatch watch;     // whether it changed since it was read
    b32       astext;    // binary, but shown as text anyway
    Str       err;        // why it failed, a literal, or in errbuf
    u8        errbuf[128];
    b32       fit;        // an image, scaled to fit the pane
    i64       opens;      // and its host's opens then: tried again once it changes
    u8        section[FILES_SECTION];  // a heading to scroll to
    i32       sectionlen;
    FileBody *body;       // null until read, and once freed
};

struct FileSet {
    FileTab *tabs[FILES_TABS];
    i32      n;
    i32      front;      // the tab shown, while n > 0
    b32      frontset;   // front changed here: the bar must select it
    i64      opens;      // tabs opened or picked, ordering them
};

// Whether two tabs' paths are the same file's, without case on Windows.
static b32 filesame(Str a, Str b, b32 win)
{
    return win ? equalfold(a, b) : a==b;
}

// The tab for a file, by host and path, or -1.
static i32 filesetfind(FileSet *s, u64 host, Str path, b32 win)
{
    for (i32 i = 0; i < s->n; i++) {
        FileTab *ft = s->tabs[i];
        if (ft->host==host && filesame(ft->path, path, win)) {
            return i;
        }
    }
    return -1;
}

static FileTab *filesetfront(FileSet *s)
{
    return s->n ? s->tabs[s->front] : 0;
}

// Show tab i, which counts as opening it.
static void filesetshow(FileSet *s, i32 i)
{
    if (i<0 || i>=s->n) {
        return;
    }
    s->tabs[i]->opened = ++s->opens;
    if (i!=s->front || !s->frontset) {
        s->front    = i;
        s->frontset = 1;
    }
}

// Remove tab i, returning it.
static FileTab *filesetdrop(FileSet *s, i32 i)
{
    assert(i>=0 && i<s->n);
    FileTab *ft = s->tabs[i];
    for (i32 k = i; k < s->n-1; k++) {
        s->tabs[k] = s->tabs[k+1];
    }
    s->tabs[--s->n] = 0;
    if (!s->n) {
        s->front    = 0;
        s->frontset = 0;
    } else if (i < s->front) {
        s->front--;  // the same tab, moved down
    } else if (i == s->front) {
        // Its right neighbour, now at i, else its left
        s->front    = i<s->n ? i : s->n-1;
        s->frontset = 1;
    }
    return ft;
}

// Add a tab last, shown. A full set first drops the tab least recently
// opened, which is returned for the caller to free, else null.
static FileTab *filesetadd(FileSet *s, FileTab *ft)
{
    FileTab *gone = 0;
    if (s->n == FILES_TABS) {
        i32 old = 0;
        for (i32 i = 1; i < s->n; i++) {
            old = s->tabs[i]->opened<s->tabs[old]->opened ? i : old;
        }
        gone = filesetdrop(s, old);
    }
    s->tabs[s->n++] = ft;
    ft->opened  = ++s->opens;
    s->front    = s->n - 1;
    s->frontset = 1;
    return gone;
}

// Take the bar's order, n tabs as it has them: the set's tabs in that
// order, then any it doesn't have yet (not yet drawn) as they were.
// Tabs not in the set are ignored. The front tab stays front.
static void filesetorder(FileSet *s, FileTab **order, i32 n)
{
    FileTab *front = filesetfront(s);
    FileTab *was[FILES_TABS];
    i32      nwas = s->n;
    for (i32 i = 0; i < nwas; i++) {
        was[i] = s->tabs[i];
    }
    i32 k = 0;
    for (i32 i = 0; i < n; i++) {
        for (i32 j = 0; j < nwas; j++) {
            if (was[j] && was[j]==order[i]) {
                s->tabs[k++] = was[j];
                was[j] = 0;
                break;
            }
        }
    }
    for (i32 j = 0; j < nwas; j++) {
        if (was[j]) {
            s->tabs[k++] = was[j];
        }
    }
    assert(k == s->n);
    for (i32 i = 0; front && i < s->n; i++) {
        s->front = s->tabs[i]==front ? i : s->front;
    }
}

// A normalized path's last k names, or all of it if it has no more,
// setting *whole.
static Str filetail(Str path, i32 k, b32 *whole)
{
    i32 n = 0;
    for (iz i = path.len; i > 0; i--) {
        if (path[i-1]=='/' && ++n==k) {
            return cuthead(path, i);
        }
    }
    *whole = 1;
    return path;
}

// The tabs' labels, s->n of them in a, for the frame that draws them:
// each file's name, with as few of its folders as tell it from the
// other tabs', then "host:" before it when the same path is open from
// another host (hosts[i] names tab i's host). Cut in its middle to
// width, unless width is 0, and "##" broken up, which ImGui would take
// as an ID.
static Str *filelabels(Arena *a, FileSet *s, Str *hosts, f32 width, Measure *m)
{
    Str *labels = alloc<Str>(a, s->n);
    for (i32 i = 0; i < s->n; i++) {
        FileTab *ft    = s->tabs[i];
        Str      label = {};
        b32      twin  = 0;  // another host has this very path
        for (i32 k = 1;; k++) {
            b32 whole = 0;
            b32 clash = 0;
            label = filetail(ft->path, k, &whole);
            for (i32 j = 0; j < s->n; j++) {
                FileTab *o = s->tabs[j];
                b32      w = ft->win && o->win;
                if (j == i) {
                    continue;
                } else if (filesame(o->path, ft->path, w)) {
                    twin = 1;
                    continue;
                }
                b32 owhole = 0;
                clash |= filesame(filetail(o->path, k, &owhole), label, w);
            }
            if (!clash || whole) {
                break;
            }
        }

        Str text = twin ? concat(a, concat(a, hosts[i], ":"), label) : label;
        Buf b(a, text.len + 8);
        u8  prev = 0;
        for (iz k = 0; k < text.len; k++) {
            if (text[k]=='#' && prev=='#') {
                print(&b, " ");
            }
            putbyte(&b, text[k]);
            prev = text[k];
        }
        text      = finish(&b);
        labels[i] = width>0 ? midcut(a, text, width, m, 0) : text;
    }
    return labels;
}

enum : i32 {  // FileText.flags
    FTEXT_BINARY = 1<<0,  // a NUL near the start: no text, unless astext
    FTEXT_BOM    = 1<<1,  // a byte order mark, dropped
    FTEXT_CUT    = 1<<2,  // more than maxbytes or maxlines: cut at a line
    FTEXT_UTF16  = 1<<3,  // decoded from UTF-16, by its mark
    FTEXT_1252   = 1<<4,  // decoded as windows-1252
};

struct FileText {
    Str text;   // UTF-8, lines ending in LF
    i32 lines;  // a last line without an LF counted too
    i32 cols;   // the widest line's, tabs to 4-column stops
    i32 flags;  // FTEXT_*
};

struct FileNorm {
    Buf      b;
    FileText r;
    iz       line;      // where the current line starts in b
    i32      col;       // its columns so far
    iz       maxbytes;
    i32      maxlines;
    b32      done;      // cut: take no more
};

// End the line being put, which has something in it.
static void fileline(FileNorm *z)
{
    z->r.lines++;
    z->r.cols = z->col>z->r.cols ? z->col : z->r.cols;
    z->col    = 0;
    z->line   = z->b.len;
}

// Take no more, keeping the whole lines put. A first line too long for
// maxbytes keeps its head instead, rather than nothing showing.
static void filecut(FileNorm *z)
{
    if (z->r.lines) {
        z->b.len = z->line;
    } else if (z->b.len > z->line) {
        fileline(z);
    }
    z->r.flags |= FTEXT_CUT;
    z->done     = 1;
}

// Append one code point as shown: a C0 control but tab and LF, and DEL,
// as its Control Picture.
static void fileput(FileNorm *z, i32 c)
{
    if (z->b.len==z->line && z->r.lines==z->maxlines) {
        filecut(z);
        return;
    }
    if (c!='\n' && c!='\t' && (c<0x20 || c==0x7f)) {
        c = c==0x7f ? 0x2421 : 0x2400 + c;
    }
    u8 tmp[4];
    iz n = utf8encode(tmp, c);
    if (z->b.len+n > z->maxbytes) {
        filecut(z);
        return;
    }
    print(&z->b, Str{tmp, n});
    if (c == '\n') {
        fileline(z);
    } else {
        z->col = c=='\t' ? (z->col/4 + 1)*4 : z->col+1;
    }
}

// A file's bytes as the text a view shows, at most maxbytes of it and
// maxlines lines, cut at a line: a UTF-8 mark dropped, UTF-16 decoded
// by its mark, invalid UTF-8 as U+FFFD, unless no multibyte sequence is
// valid at all, when it is windows-1252. CRLF is LF, and a lone CR and
// the other controls (but tab) show as Control Pictures. A NUL in the
// first 8 KiB, before decoding, makes it binary, unless astext: then
// only flags are returned.
static FileText filenorm(Arena *a, Str raw, b32 astext, iz maxbytes, i32 maxlines)
{
    FileNorm z = {};
    z.maxbytes = maxbytes;
    z.maxlines = maxlines;

    i32 utf16 = 0;  // 1 little-endian, 2 big
    if (startswith(raw, "\xef\xbb\xbf")) {
        raw = cuthead(raw, 3);
        z.r.flags |= FTEXT_BOM;
    } else if (startswith(raw, "\xff\xfe") || startswith(raw, "\xfe\xff")) {
        utf16 = raw[0]==0xff ? 1 : 2;
        raw   = cuthead(raw, 2);
        z.r.flags |= FTEXT_BOM | FTEXT_UTF16;
    }

    if (!utf16) {
        for (iz i = 0; i<raw.len && i<((iz)8<<10); i++) {
            if (!raw[i]) {
                z.r.flags |= FTEXT_BINARY;
                break;
            }
        }
        if (z.r.flags&FTEXT_BINARY && !astext) {
            return z.r;
        }
    }

    b32 cp1252 = 0;
    if (!utf16) {
        b32 valid = 0;
        b32 bad   = 0;
        for (iz i = 0; i<raw.len && !valid;) {
            i32 c   = utf8strict(raw, &i);
            b32 sur = c>=0xd800 && c<=0xdfff;
            bad    |= c<0 || sur;
            valid  |= c>=0x80 && !sur;
        }
        cp1252 = bad && !valid;
        z.r.flags |= cp1252 ? FTEXT_1252 : 0;
    }

    // At most 3 bytes a byte, or 3 a UTF-16 unit, and an odd last byte's
    // 3 in the slack: allocated once, never grown, and what is unused
    // given back, so the arena never takes more than maxbytes
    iz est = utf16 ? raw.len/2*3 : raw.len*3;
    z.b    = Buf(a, (est<maxbytes ? est : maxbytes) + 4);
    for (iz i = 0; i<raw.len && !z.done;) {
        i32 c;
        if (utf16) {
            c = pdfutf16(raw, &i, utf16==1);
        } else if (cp1252) {
            c = raw[i++];
            if (c>=0x80 && c<=0x9f) {
                c = web1252[c - 0x80];
                c = c<0xa0 ? 0xfffd : c;  // its holes
            }
        } else {
            c = webdecode(raw, &i);
        }

        if (c == '\r') {
            // CRLF as LF: look ahead for the LF in this encoding
            b32 lf = utf16==1 ? i+1<raw.len && raw[i]=='\n' && !raw[i+1] :
                     utf16==2 ? i+1<raw.len && !raw[i] && raw[i+1]=='\n' :
                     i<raw.len && raw[i]=='\n';
            if (lf) {
                continue;
            }
        }
        fileput(&z, c);
    }
    if (!z.done && z.b.len>z.line) {
        fileline(&z);
    }
    z.r.text = webfinish(&z.b);
    return z.r;
}

// How much of a file's head, read short of its end, to show: up to its
// last whole line, an LF a whole code unit in UTF-16 by its mark. With
// no LF, all of it (in whole units), for filenorm to cut its line.
static iz filewhole(Str raw)
{
    if (startswith(raw, "\xff\xfe") || startswith(raw, "\xfe\xff")) {
        b32 le  = raw[0] == 0xff;
        iz  end = 2 + (raw.len - 2)/2*2;
        for (iz n = end; n > 2; n -= 2) {
            u8 lo = le ? raw[n-2] : raw[n-1];
            u8 hi = le ? raw[n-1] : raw[n-2];
            if (lo=='\n' && !hi) {
                return n;
            }
        }
        return end;
    }
    for (iz n = raw.len; n > 0; n--) {
        if (raw[n-1] == '\n') {
            return n;
        }
    }
    return raw.len;
}

// A cited line as it can be shown: 1 to lines, else 0 for none.
static i32 fileclamp(i32 line, i32 lines)
{
    return line<1 || lines<1 ? 0 : line>lines ? lines : line;
}

// Finding in a file's text: its bytes scanned as they are, matched as
// the transcript's find matches (findnorm: ASCII case folded, and any
// run of whitespace, line breaks and blank lines too, matching one
// space), so a match may cross lines and chunks. No document is parsed,
// so the layout cache keeps what is shown. Hits are in the terms of the
// lines view's chunk messages: a chunk, the offset from its first byte,
// and a length that may run into later chunks (filemarks).
struct FileFind {
    Str            text;     // the body searched, empty if none
    Str            q;        // findnorm'd
    iz             qmax;
    Arena          hitmem;   // alone in its Region, to grow in place
    byte          *hitbase;
    iz             hitmax;
    Slice<FindHit> hits;     // in order and apart
    i32            cur;      // the current hit, -1 if none
    iz             at;       // searched up to here
    i32            line;     // lines before at, 0-based
    b32            done;
    b32            full;     // hitmax hits kept, and more found
    i32            chunkat[FILES_CHUNKS+1];  // each chunk's first byte, as reached
};

static FileFind *newfilefind(Arena *perm, Region *hits, iz qmax, iz hitmax)
{
    FileFind *f = alloc<FileFind>(perm);
    f->hitmem  = newarena(hits);
    f->hitbase = f->hitmem.beg;
    f->q.data  = alloc<u8>(perm, qmax);
    f->qmax    = qmax;
    f->hitmax  = hitmax;
    f->cur     = -1;
    f->done    = 1;
    return f;
}

// Search text for q from its start, as filefindstep goes on to.
static void filefindreset(FileFind *f, Str text, Str q)
{
    f->q.len      = findq(f->q.data, q, f->qmax);
    f->text       = text;
    f->hitmem.beg = f->hitbase;
    f->hits       = {};
    f->cur        = -1;
    f->at         = 0;
    f->line       = 0;
    f->chunkat[0] = 0;
    f->full       = 0;
    f->done       = !f->q.len || !text.len;
}

// The length of a match of a findnorm'd q at t[at], else -1: a space in
// q takes a whitespace byte and, unless it ends q, the rest of its run.
// The bytes compared are added to *cost.
static iz filefindmatch(Str t, iz at, Str q, iz *cost)
{
    iz i = at;
    for (iz k = 0; k < q.len; k++) {
        if (i == t.len) {
            return -1;
        }
        u8 c = t[i++];
        (*cost)++;
        if (q[k] != ' ') {
            if (lowercase(c) != q[k]) {
                return -1;
            }
        } else if (!whitespace(c)) {
            return -1;
        } else if (k < q.len-1) {
            for (; i<t.len && whitespace(t[i]); i++) {
                (*cost)++;
            }
        }
    }
    return i - at;
}

// Go on searching, about budget bytes passed and compared at most, the
// text past FILES_CHUNKS chunks ignored. Returns whether it is done:
// all searched, or hitmax hits kept (full) once another is found.
static b32 filefindstep(FileFind *f, iz budget)
{
    Str t  = f->text;
    u8  q0 = f->q.len ? f->q[0] : 0;
    while (!f->done && budget>0) {
        if (f->at == t.len) {
            f->done = 1;
            break;
        }
        iz at   = f->at;
        u8 c    = t[at];
        b32 can = q0==' ' ? whitespace(c) && (!at || !whitespace(t[at-1]))
                          : lowercase(c)==q0;
        iz cost = 0;
        iz n    = can ? filefindmatch(t, at, f->q, &cost) : -1;
        budget -= cost;
        if (n > 0) {
            if (f->hits.len == f->hitmax) {
                f->full = 1;
                f->done = 1;
                break;
            }
            i32     chunk = f->line / FILES_CHUNK;
            FindHit h     = {chunk, (i32)(at - f->chunkat[chunk]), (i32)n};
            f->hits = push(&f->hitmem, f->hits, h);
        } else {
            n = 1;
        }
        budget -= n;
        for (iz end = at+n; f->at<end && !f->done; f->at++) {
            if (t[f->at]=='\n' && !(++f->line % FILES_CHUNK)) {
                i32 chunk = f->line / FILES_CHUNK;
                f->chunkat[chunk] = (i32)(f->at + 1);
                f->done = chunk == FILES_CHUNKS;
            }
        }
    }
    return f->done;
}

// The first hit in a chunk at or after an offset, else the next chunk's,
// else hits.len.
static iz filefindfrom(FileFind *f, i32 msg, i32 off)
{
    return hitfrom(f->hits, msg, off);
}

// A chunk's hits, start its first byte in the text, as drawmarks takes
// them: into hits, unless a hit begun in an earlier chunk runs into it.
// Then its tail there comes first, the chunk's own hits copied after it
// into a, the current hit remapped, and *carried set.
static Marks filemarks(Arena *a, FileFind *f, i32 msg, iz start, b32 *carried)
{
    *carried = 0;
    Marks r  = hitmarks(f->hits, f->cur, msg);
    iz    lo = hitfrom(f->hits, msg, 0);
    if (!lo) {
        return r;
    }
    FindHit *p   = &f->hits[lo-1];
    iz       end = f->chunkat[p->msg] + p->off + p->len;
    if (end <= start) {
        return r;
    }
    Marks c = {};
    c.h     = alloc<FindHit>(a, r.n+1);
    c.h[0]  = {msg, 0, (i32)(end - start)};
    for (iz i = 0; i < r.n; i++) {
        c.h[i+1] = r.h[i];
    }
    c.n   = r.n + 1;
    c.cur = r.cur>=0 ? r.cur+1 : f->cur==lo-1 ? 0 : -1;
    *carried = 1;
    return c;
}

// Where a chunk's line k (0-based) begins, else the chunk's end.
static i32 filelineoff(Str chunk, i32 k)
{
    i32 n = 0;
    for (iz i = 0; i<chunk.len && n<k; i++) {
        if (chunk[i]=='\n' && ++n==k) {
            return (i32)(i + 1);
        }
    }
    return k>0 ? (i32)chunk.len : 0;
}

// The line and column of a byte of text, both 1-based. A column counts
// code points from the line's start, a tab as one, as an editor's go to
// line and column does.
static void fileoffline(Str text, iz off, i32 *line, i32 *col)
{
    off = off<0 ? 0 : off>text.len ? text.len : off;
    i32 l     = 1;
    iz  begin = 0;
    for (iz i = 0; i < off; i++) {
        if (text[i] == '\n') {
            l++;
            begin = i + 1;
        }
    }
    i32 c = 1;
    for (iz i = begin; i < off; i++) {
        c += (text[i]&0xc0) != 0x80;
    }
    *line = l;
    *col  = c;
}

// The byte of a line's 1-based column, as fileoffline counts it, clamped
// to the line's end, before its LF.
static i32 filecolbyte(Str line, i32 col)
{
    iz i = 0;
    for (i32 c = 1; c<col && i<line.len && line[i]!='\n'; c++) {
        utf8decode(line, &i);
    }
    return (i32)i;
}

// Go to line's text as lines: N, N:C or A-B, as frefsuffix reads them
// after a name, spaces dropped and one leading ':' allowed. A number
// past FREF_MAXLINE is FREF_MAXLINE, past any file's end; a reversed
// range is swapped and drops its column, as freflines has it. Returns
// whether it is lines, else *out is empty.
static b32 filegotoparse(Str s, FrefLines *out)
{
    *out = {};
    u8  buf[32];
    iz  n     = 0;
    b32 colon = 0;
    buf[n++] = ':';
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (whitespace(c)) {
            continue;
        } else if (c==':' && n==1 && !colon) {
            colon = 1;
            continue;
        } else if (n == countof(buf)) {
            return 0;
        }
        buf[n++] = c;
    }
    Str       t = {buf, n};
    FrefLines l = {};
    if (frefsuffix(t, &l)!=t.len || !l.ok || l.line0<1) {
        return 0;
    }
    i32 a = l.line0<FREF_MAXLINE ? l.line0 : FREF_MAXLINE;
    i32 b = l.line1<1 ? a : l.line1<FREF_MAXLINE ? l.line1 : FREF_MAXLINE;
    out->line0 = a<b ? a : b;
    out->line1 = a<b ? b : a;
    out->col   = b<a || l.col<1 ? 0 : l.col<FREF_MAXLINE ? l.col : FREF_MAXLINE;
    out->ok    = 1;
    return 1;
}
