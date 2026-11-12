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
};

enum : iz {
    FILES_MAX    = (iz)4 << 20,   // bytes of text kept, normalized
    FILES_CACHE  = (iz)8 << 20,   // a body's own layout cache
    FILES_BUDGET = (iz)64 << 20,  // all bodies, image textures included
};

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
    b32       stale;      // it may have changed since it was read
    b32       astext;     // binary, but shown as text anyway
    Str       err;        // why it failed, a literal
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
