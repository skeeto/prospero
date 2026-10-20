// Transcript view model: messages built from protocol v0 events
//
// The GUI never reads engine memory. It applies event records, the same
// lines the session log holds, to its own list of messages, and draws
// from that. Streamed deltas grow a provisional message, which the
// committed assistant record later replaces.
//
// Memory, by lifetime:
//   text   message sources, appended forever; the streaming message is
//          the last allocation, so its deltas extend it in place
//   msgs   the message array, alone in its arena so it grows in place
//   cache  parsed documents and layouts, reset wholesale when full: a
//          generation number marks everything made before as stale
//
// Messages have heights, measured once laid out or else estimated from
// their size, and each knows its top: the prefix sum of the heights
// above it, recomputed lazily at most once per frame, which a binary
// search turns into the range of messages on screen.
//
// A selection is a pair of positions (message, source offset), ordered
// lexicographically, so it spans messages and survives relayout and
// streaming. Copying turns it back into text or markdown.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_THINK,
    ROLE_TOOL,
    ROLE_NOTE,
};

struct Msg {
    Str       text;       // source, in the text arena
    i32       role;
    i32       turn;
    i64       seq;        // durable record, or 0 while provisional
    i32       ver;        // bumped when text changes
    i64       nl;         // newlines in text, for estimates
    b32       prov;       // provisional: streamed, not yet committed
    f64       top;        // prefix sum of heights above, with gaps
    f32       h;          // content height: measured, or estimated
    b32       measured;   // h is from a layout at the current width
    // Cached in the cache arena, valid while gen matches
    Doc      *doc;
    i32       docver;
    i32       docgen;
    MsgLayout layout;
    i32       layoutver;
    i32       layoutgen;
    f32       layoutw;
};

struct Pos {
    i32 msg;
    i32 off;
};

enum : i32 {
    SEL_CHAR,
    SEL_WORD,
    SEL_BLOCK,
};

struct Sel {
    Pos anchor;   // where the selection began
    Pos anchor1;  // with word or block units: the end of the first unit
    Pos focus;    // where it extends to
    i32 unit;     // SEL_*
    b32 active;
};

struct View {
    Arena        text;
    Arena        meta;
    Arena        cache;
    byte        *cachebase;
    iz           cachemax;   // reset the cache past this many bytes
    i32          gen;
    Slice<Msg>   msgs;
    i32          stream;     // the provisional message, -1 if none
    i32          streamkind; // its delta kind: ROLE_ASSISTANT or ROLE_THINK
    i32          prov;       // first provisional message, -1 if none
    byte        *provtext;   // text arena position when it began
    b32          provmixed;  // others were added after it began
    b32          topsdirty;
    f32          gap;        // between messages
    f32          extra;      // header and padding around each message
    HlMemo      *memo;
    LayoutCache *lcache;
    Sel          sel;
    i32          turn;
    b32          busy;       // a turn is running
    i64          deltas;     // delta records applied, for rates
    i64          deltabytes;
    i64          laidout;    // messages laid out, for benchmarks
    b32          overflow;   // events were dropped: resync needed
};

static View *newview(Arena *perm, Region *text, Region *meta, Region *cache,
                     iz cachemax)
{
    View *v = alloc<View>(perm);
    v->text      = newarena(text);
    v->meta      = newarena(meta);
    v->cache     = newarena(cache);
    v->cachebase = v->cache.beg;
    v->cachemax  = cachemax;
    v->stream    = -1;
    v->prov      = -1;
    v->topsdirty = 1;
    return v;
}

// At frame start: drop every cached document and layout if the cache
// has grown past its budget. Nothing from the cache may be held across
// this call.
static void viewreset(View *v, b32 force)
{
    if (force || v->cache.beg-v->cachebase > v->cachemax) {
        v->cache.beg = v->cachebase;
        v->gen++;
        v->memo   = 0;
        v->lcache = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            v->msgs[i].measured = 0;
        }
        v->topsdirty = 1;
    }
}

static i64 countnl(Str s)
{
    i64 n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s.data[i] == '\n';
    }
    return n;
}

static i32 addmsg(View *v, i32 role, Str text)
{
    Msg m = {};
    m.role = role;
    m.turn = v->turn;
    m.text = clone(&v->text, text);
    m.nl   = countnl(text);
    if (!m.text.data) m.text.data = (u8 *)"";
    v->msgs = push(&v->meta, v->msgs, m);
    v->topsdirty  = 1;
    v->provmixed |= v->prov >= 0;
    v->stream     = -1;  // later deltas start a new message after it
    return (i32)v->msgs.len - 1;
}

// Append to the provisional message for this kind of delta, starting
// one if needed. Its text is the text arena's last allocation, so this
// extends it in place.
static void viewdelta(View *v, i32 role, Str text)
{
    if (v->stream<0 || v->streamkind!=role) {
        if (v->prov < 0) {
            v->prov      = (i32)v->msgs.len;
            v->provtext  = v->text.beg;
            v->provmixed = 0;
        }
        Msg m = {};
        m.role = role;
        m.turn = v->turn;
        m.prov = 1;
        v->msgs = push(&v->meta, v->msgs, m);
        v->stream     = (i32)v->msgs.len - 1;
        v->streamkind = role;
    }
    Msg *m = &v->msgs[v->stream];
    m->text = concat(&v->text, m->text, text);
    m->nl  += countnl(text);
    m->ver++;
    v->deltas++;
    v->deltabytes += text.len;
    v->topsdirty   = 1;
}

// Drop the provisional messages, which a committed record replaces.
// Their text is reclaimed unless other messages came in between.
static void dropprov(View *v)
{
    if (v->prov >= 0) {
        iz n = v->prov;
        for (iz i = v->prov; i < v->msgs.len; i++) {
            if (!v->msgs[i].prov) {
                v->msgs[n++] = v->msgs[i];
            }
        }
        v->msgs.len = n;
        if (!v->provmixed) {
            v->text.beg = v->provtext;
        }
        v->prov      = -1;
        v->topsdirty = 1;
    }
    v->stream = -1;
}

// Keep a selection inside the messages that exist.
static void clampsel(View *v)
{
    Sel *s = &v->sel;
    if (!s->active) {
        return;
    }
    i32 n = (i32)v->msgs.len;
    Pos *ps[] = {&s->anchor, &s->anchor1, &s->focus};
    for (iz i = 0; i < countof(ps); i++) {
        Pos *p = ps[i];
        if (p->msg >= n) {
            if (!n) {
                s->active = 0;
                return;
            }
            p->msg = n - 1;
            p->off = (i32)v->msgs[n-1].text.len;
        }
        if (p->off > v->msgs[p->msg].text.len) {
            p->off = (i32)v->msgs[p->msg].text.len;
        }
    }
}

// Apply one event record. Returns 1 if what the view shows changed.
static b32 viewapply(View *v, Arena scratch, Str line)
{
    EvRec r = evparse(&scratch, line);
    if (!r.ok) {
        return 0;
    }
    Json *j = r.root;
    switch (r.ev) {
    case EV_USER:
        dropprov(v);
        addmsg(v, ROLE_USER, jsonstr(jsonget(j, "text")));
        v->msgs[v->msgs.len-1].seq = r.seq;
        return 1;

    case EV_TURN:
        v->turn = (i32)jsonint(jsonget(j, "n"), v->turn+1);
        v->busy = 1;
        return 1;

    case EV_DELTA: {
        Str kind = jsonstr(jsonget(j, "kind"));
        i32 role = kind=="think" ? ROLE_THINK : ROLE_ASSISTANT;
        if (kind == "args") {
            return 0;  // tool arguments stream invisibly until M3
        }
        viewdelta(v, role, jsonstr(jsonget(j, "text")));
    } return 1;

    case EV_ASSISTANT: {
        dropprov(v);
        Json *blocks = jsonget(j, "blocks");
        for (Json *b = blocks->child; b; b = b->next) {
            Str t = jsonstr(jsonget(b, "t"));
            if (t == "think") {
                addmsg(v, ROLE_THINK, jsonstr(jsonget(b, "text")));
            } else if (t == "text") {
                addmsg(v, ROLE_ASSISTANT, jsonstr(jsonget(b, "text")));
            } else if (t == "call") {
                Arena tmp = scratch;
                Buf   buf(&tmp, 256);
                print(&buf, jsonstr(jsonget(b, "name")));
                putbyte(&buf, ' ');
                print(&buf, jsonstr(jsonget(b, "args")));
                addmsg(v, ROLE_TOOL, finish(&buf));
            } else {
                continue;
            }
            v->msgs[v->msgs.len-1].seq = r.seq;
        }
        clampsel(v);
    } return 1;

    case EV_TOOLEND: {
        Str text = jsonstr(jsonget(j, "text"));
        if (!text.len) {
            return 0;
        }
        addmsg(v, ROLE_TOOL, text);
        v->msgs[v->msgs.len-1].seq = r.seq;
    } return 1;

    case EV_NOTE:
        addmsg(v, ROLE_NOTE, jsonstr(jsonget(j, "text")));
        v->msgs[v->msgs.len-1].seq = r.seq;
        return 1;

    case EV_TURNEND:
        dropprov(v);
        clampsel(v);
        v->busy = 0;
        return 1;

    case EV_OVERFLOW:
        v->overflow = 1;
        return 1;
    }
    return 0;
}


// Documents and layouts, made on demand in the cache

static Doc *msgdoc(View *v, i32 i)
{
    Msg *m = &v->msgs[i];
    if (m->doc && m->docver==m->ver && m->docgen==v->gen) {
        return m->doc;
    }
    switch (m->role) {
    case ROLE_USER:
    case ROLE_TOOL:
        m->doc = parseplain(&v->cache, m->text, m->role==ROLE_TOOL ? STYLE_CODE : 0);
        break;
    case ROLE_THINK:
    case ROLE_NOTE:
        m->doc = parseplain(&v->cache, m->text, STYLE_DIM);
        break;
    default:
        m->doc = parsedoc(&v->cache, m->text, &v->memo);
    }
    m->docver = m->ver;
    m->docgen = v->gen;
    return m->doc;
}

// Lay out message i at the width, updating its height.
static MsgLayout *msglayout(View *v, i32 i, Measure *ms, f32 width, f32 scale)
{
    Msg *m = &v->msgs[i];
    if (m->layoutver!=m->ver || m->layoutgen!=v->gen || m->layoutw!=width ||
            !m->measured) {
        Doc *d = msgdoc(v, i);
        m->layout    = layoutmsg(&v->lcache, &v->cache, ms, d, width, scale);
        v->laidout++;
        m->layoutver = m->ver;
        m->layoutgen = v->gen;
        m->layoutw   = width;
        if (!m->measured || m->h!=m->layout.h) {
            v->topsdirty = 1;
        }
        m->h        = m->layout.h;
        m->measured = 1;
    }
    return &m->layout;
}

// Height estimate for a message not yet laid out: its lines, plus its
// bytes wrapped at a typical character width.
static f32 estimate(Msg *m, f32 width, f32 charw, f32 lineh)
{
    i64 perline = width>charw ? (i64)(width/charw) : 1;
    i64 lines   = m->nl + 1 + m->text.len/perline;
    return (f32)lines * lineh;
}

// Recompute message tops if anything moved. Returns the total height.
static f64 viewtops(View *v, f32 width, f32 charw, f32 lineh)
{
    if (v->topsdirty) {
        f64 y = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            Msg *m = &v->msgs[i];
            if (!m->measured || m->layoutw!=width) {
                m->measured = 0;
                m->h = estimate(m, width, charw, lineh);
            }
            m->top = y;
            y += v->extra + m->h + v->gap;
        }
        v->topsdirty = 0;
    }
    if (!v->msgs.len) {
        return 0;
    }
    Msg *last = &v->msgs[v->msgs.len-1];
    return last->top + v->extra + last->h;
}

// The message at y: the last whose top is at or above it.
static i32 viewfind(View *v, f64 y)
{
    iz lo = 0, hi = v->msgs.len;
    if (!hi) {
        return -1;
    }
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (v->msgs[mid].top <= y) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (i32)lo;
}


// Selection

static b32 posless(Pos a, Pos b)
{
    return a.msg<b.msg || (a.msg==b.msg && a.off<b.off);
}

static b32 poseq(Pos a, Pos b)
{
    return a.msg==b.msg && a.off==b.off;
}

struct Range {
    Pos beg;
    Pos end;
};

// The selected range, ordered. Empty if inactive.
static Range selrange(Sel *s)
{
    Range r = {};
    if (!s->active) {
        return r;
    }
    Pos a = s->anchor, b = s->focus;
    if (s->unit != SEL_CHAR) {
        // Extend by whole units: keep the first unit selected whichever
        // way the focus goes.
        Pos a1 = s->anchor1;
        if (posless(b, a)) {
            r.beg = b;
            r.end = a1;
        } else {
            r.beg = a;
            r.end = posless(b, a1) ? a1 : b;
        }
        return r;
    }
    r.beg = posless(b, a) ? b : a;
    r.end = posless(b, a) ? a : b;
    return r;
}

static b32 selempty(Sel *s)
{
    Range r = selrange(s);
    return poseq(r.beg, r.end);
}

static b32 wordbyte(u8 c)
{
    return alnum(c) || c=='_' || c>=0x80;
}

// The word around an offset: letters, digits, underscores and non-ASCII;
// otherwise the single character there.
static Range wordat(View *v, Pos p)
{
    Str s = v->msgs[p.msg].text;
    Range r = {{p.msg, p.off}, {p.msg, p.off}};
    if (p.off>=s.len || !wordbyte(s[p.off])) {
        if (p.off < s.len) {
            iz i = p.off;
            utf8decode(s, &i);
            r.end.off = (i32)i;
        }
        return r;
    }
    i32 b = p.off, e = p.off;
    for (; b>0 && wordbyte(s[b-1]); b--) {}
    for (; e<s.len && wordbyte(s[e]); e++) {}
    // Keep whole UTF-8 sequences: never start on a continuation byte.
    for (; b<p.off && (s[b]&0xc0)==0x80; b++) {}
    r.beg.off = b;
    r.end.off = e;
    return r;
}

// Index of the block holding a source offset, or the one before it.
static i32 blockat(Doc *d, i32 off)
{
    iz lo = 0, hi = d->blocks.len;
    if (!hi) {
        return -1;
    }
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (d->blocks[mid].off0 <= off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (i32)lo;
}

static Range blockrange(View *v, Pos p)
{
    Range r = {{p.msg, 0}, {p.msg, (i32)v->msgs[p.msg].text.len}};
    Doc  *d = msgdoc(v, p.msg);
    i32   b = blockat(d, p.off);
    if (b >= 0) {
        r.beg.off = d->blocks[b].off0;
        r.end.off = d->blocks[b].off1;
    }
    return r;
}

// Begin a selection at p with the given unit (click, double, triple).
static void selbegin(View *v, Pos p, i32 unit)
{
    Sel *s = &v->sel;
    s->active = 1;
    s->unit   = unit;
    Range r = unit==SEL_WORD  ? wordat(v, p) :
              unit==SEL_BLOCK ? blockrange(v, p) :
              Range{p, p};
    s->anchor  = r.beg;
    s->anchor1 = r.end;
    s->focus   = r.end;
}

// Extend the selection to p, by the selection's unit.
static void selextend(View *v, Pos p)
{
    Sel *s = &v->sel;
    if (!s->active) {
        selbegin(v, p, SEL_CHAR);
        return;
    }
    if (s->unit == SEL_CHAR) {
        s->focus = p;
        return;
    }
    Range r = s->unit==SEL_WORD ? wordat(v, p) : blockrange(v, p);
    s->focus = posless(p, s->anchor) ? r.beg : r.end;
}

static void selall(View *v)
{
    if (!v->msgs.len) {
        return;
    }
    i32 last = (i32)v->msgs.len - 1;
    v->sel.active  = 1;
    v->sel.unit    = SEL_CHAR;
    v->sel.anchor  = {0, 0};
    v->sel.anchor1 = {0, 0};
    v->sel.focus   = {last, (i32)v->msgs[last].text.len};
}


// Copying

// Text a piece contributes to the source range [s, e).
static Str piececlip(Piece *p, i32 s, i32 e)
{
    if (p->flags & PIECE_SYNTH) {
        return p->off>=s && p->off<e ? p->text : Str{};
    } else if (p->len != p->text.len) {
        return p->off<e && p->off+p->len>s ? p->text : Str{};
    }
    i32 b = p->off>s ? p->off : s;
    i32 x = p->off+p->len<e ? p->off+p->len : e;
    if (b >= x) {
        return {};
    }
    return {p->text.data + (b - p->off), x - b};
}

// What goes between two blocks in copied text.
static Str blocksep(Block *prev, Block *b)
{
    if (prev->type==BLOCK_CELL && b->type==BLOCK_CELL && prev->table==b->table) {
        return prev->row==b->row ? Str("\t") : Str("\n");
    } else if (prev->type==BLOCK_PLAIN && b->type==BLOCK_PLAIN) {
        return "\n";
    } else if (b->type==BLOCK_ITEM && (prev->type==BLOCK_ITEM || prev->depth)) {
        return "\n";
    }
    return "\n\n";
}

// The selected part [s, e) of a message as plain text: piece texts, code
// byte-exact, with list markers and block separators restored.
static void copytext(Buf *b, Doc *d, i32 s, i32 e)
{
    Block *prev = 0;
    for (iz i = 0; i < d->blocks.len; i++) {
        Block *k = &d->blocks[i];
        if (k->off1<s || k->off0>e) {
            continue;
        }
        iz  before = b->len;
        b32 starts = s<=k->off0 && k->off0<e;  // markers belong to starts
        if (prev) {
            Str sep = blocksep(prev, k);
            if (b->len && b->data[b->len-1]=='\n' && sep.len && sep[0]=='\n') {
                sep = cuthead(sep, 1);  // code already ended its line
            }
            print(b, sep);
        }
        b32 any = 0;
        if (k->item && starts) {
            for (i32 n = 1; n < k->depth; n++) print(b, "  ");
            if (k->task) {
                print(b, k->task==2 ? Str("- [x] ") : Str("- [ ] "));
            } else if (k->number < 0) {
                print(b, "- ");
            } else {
                print(b, (i64)k->number);
                print(b, ". ");
            }
            any = 1;
        }
        if (k->type==BLOCK_HR && starts) {
            print(b, "---");
            any = 1;
        }
        if (!k->npiece && starts) {
            any = 1;  // an empty line or item
        }
        for (i32 j = 0; j < k->npiece; j++) {
            Str t = piececlip(&d->pieces[k->piece0+j], s, e);
            print(b, t);
            any |= t.len > 0;
        }
        if (!any) {
            b->len = before;  // nothing of this block was selected
            continue;
        }
        prev = k;
    }
}

struct Range32 {
    i32 beg;
    i32 end;
};

static i32 linestart(Str s, i32 off)
{
    for (; off>0 && s[off-1]!='\n'; off--) {}
    return off;
}

// Whether a line is a code fence, past any container prefix.
static b32 fenceline(Str line)
{
    iz i = 0;
    for (; i<line.len && (line[i]==' ' || line[i]=='>' || line[i]=='\t'); i++) {}
    Str t = cuthead(line, i);
    return startswith(t, "```") || startswith(t, "~~~");
}

// The source lines of block i, with a code block's fences.
static Range32 blocklines(Doc *d, iz i)
{
    Str    src = d->src;
    Block *k   = &d->blocks[i];
    Range32 r  = {linestart(src, k->off0), lineend(src, k->off1)};
    if (k->type==BLOCK_CODE && k->fence) {
        if (k->npiece && r.beg>0) {
            r.beg = linestart(src, r.beg-1);  // the opening fence
        }
        if (r.end < src.len) {
            i32 ne = lineend(src, r.end+1);
            if (fenceline(span(src.data+r.end+1, src.data+ne))) {
                r.end = ne;  // the closing fence
            }
        }
    }
    return r;
}

// The selected part [s, e) of a message as markdown: the source itself
// within one block, else whole blocks' source lines.
static void copymarkdown(Buf *b, Doc *d, i32 s, i32 e)
{
    Str src = d->src;
    i32 bs  = blockat(d, s);
    i32 be  = blockat(d, e>s ? e-1 : e);
    if (bs<0 || bs==be) {
        print(b, span(src.data+s, src.data+e));
        return;
    }
    Range32 first = blocklines(d, bs);
    Range32 last  = blocklines(d, be);
    i32 beg = first.beg<s ? first.beg : s;
    i32 end = last.end>e ? last.end : e;
    print(b, span(src.data+beg, src.data+end));
}

enum : i32 {
    COPY_TEXT,
    COPY_MARKDOWN,
};

// The selection as text or markdown; messages are separated by a blank
// line.
static Str copysel(View *v, Arena *perm, i32 how)
{
    Range r = selrange(&v->sel);
    if (!v->sel.active || poseq(r.beg, r.end)) {
        return {};
    }
    Buf b(perm, 1<<12);
    for (i32 i = r.beg.msg; i <= r.end.msg; i++) {
        Msg *m = &v->msgs[i];
        i32  s = i==r.beg.msg ? r.beg.off : 0;
        i32  e = i==r.end.msg ? r.end.off : (i32)m->text.len;
        if (i > r.beg.msg) {
            print(&b, "\n\n");
        }
        Doc *d = msgdoc(v, i);
        if (how == COPY_MARKDOWN) {
            copymarkdown(&b, d, s, e);
        } else {
            copytext(&b, d, s, e);
        }
    }
    return finish(&b);
}
