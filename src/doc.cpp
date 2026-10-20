// Markdown documents: md4c's events flattened into blocks of pieces
//
// A message's markdown becomes a list of leaf blocks (paragraphs,
// headings, code, list item text, rules, table cells), each a run of
// styled pieces. Containers (lists and quotes) do not become blocks of
// their own, but annotate the leaves inside them with their depth.
//
// Every piece remembers where it came from: off and len give its range
// in the source, so a selection can be kept as source offsets, survive
// a re-parse, and be copied as text or as markdown. Text md4c makes up
// (break newlines, indentation, U+FFFD) is synthetic: len 0, at the
// nearest source offset. md4c's dialect is GitHub's, with raw HTML off
// so that text like Vec<T> or <thinking> stays visible.
//
// Code blocks go through the highlighter, which splits their pieces at
// token boundaries. Concatenated, a code block's piece texts are its
// code exactly, tabs and trailing spaces included, which is what makes
// copied code byte-exact.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    BLOCK_PARA,
    BLOCK_HEADING,
    BLOCK_CODE,
    BLOCK_ITEM,   // text of a tight list item, outside any paragraph
    BLOCK_HR,
    BLOCK_CELL,   // a table cell
    BLOCK_PLAIN,  // a line of plain text, not markdown
};

enum : i32 {
    STYLE_BOLD   = 1<<0,
    STYLE_ITALIC = 1<<1,
    STYLE_CODE   = 1<<2,   // monospace: code spans and blocks
    STYLE_STRIKE = 1<<3,
    STYLE_LINK   = 1<<4,
    STYLE_HEADER = 1<<5,   // table header cell
    STYLE_DIM    = 1<<6,   // de-emphasized: reasoning, notes
    STYLE_HSHIFT = 8,      // heading level 1-6 in bits 8-10
    STYLE_HMASK  = 7<<STYLE_HSHIFT,
};

enum : u8 {
    PIECE_SYNTH = 1<<0,  // display text with no source bytes
    PIECE_HARD  = 1<<1,  // a hard line break
    PIECE_SOFT  = 1<<2,  // a soft line break: a space when wrapped
};

enum : i32 {
    DOC_MAXDEPTH = 32,        // list and quote nesting tracked
    DOC_MAXSIZE  = 1 << 20,   // larger messages are shown as plain text
};

struct Piece {
    i32 off;    // source offset; synthetic text: the nearest one
    i32 len;    // source bytes covered: 0 if synthetic, 5 for "&amp;"
    Str text;   // display text, usually a slice of the source
    i32 style;  // STYLE_*
    u8  hl;     // HL_* class in code blocks
    u8  flags;  // PIECE_*
    i32 link;   // index into Doc.links, -1 if none
};

struct Block {
    i32 off0, off1;   // source range covered by its pieces
    i32 piece0;
    i32 npiece;
    i32 type;         // BLOCK_*
    i32 level;        // heading level
    i32 depth;        // list nesting
    i32 quote;        // blockquote nesting
    i32 number;       // ordered item number, or -1 for a bullet
    u8  item;         // first block of a list item: draw its marker
    u8  task;         // 0 none, 1 open, 2 done
    u8  align;        // cells: 0 left, 1 center, 2 right
    u8  fence;        // code: fenced (vs indented)
    Str lang;         // code: the fence's info word
    i32 table;        // cells: table index in the document
    i32 row, col;     // cells: position, header row 0
    u64 hash;         // everything layout depends on: the cache key
};

struct Doc {
    Str          src;
    Slice<Block> blocks;
    Slice<Piece> pieces;
    Slice<Str>   links;
    i32          ntables;
    b32          plain;  // parsed as plain text
};

// Highlighted code blocks, memoized by content (a 4-ary hash trie).
struct HlMemo {
    HlMemo      *child[4];
    u64          key;
    Slice<HlTok> toks;
};

struct DocParse {
    Arena   *perm;
    Doc     *doc;
    HlMemo **memo;
    u8      *base;     // source bounds, for telling synthetic text apart
    iz       size;
    i32      lastoff;  // end of the last source text seen
    i32      cur;      // open leaf block, -1 if none
    i32      indent0;  // first piece of a code line's indentation, or -1
    i32      indentcols;
    i32      ordered[DOC_MAXDEPTH];  // per list level: next number, -1 bullet
    i32      depth;    // list nesting, uncapped
    i32      quote;    // quote nesting, uncapped
    i32      items;    // open list items, uncapped
    b32      pending;  // an item began and its marker is not yet shown
    u8       task;     // the pending item's task state
    i32      bold, italic, code, strike;  // span nesting counts
    i32      link;     // innermost open link, -1 if none
    i32      table, row, col;
    u8       align;
    b32      header;
};

static i32 clampdepth(i32 d)
{
    return (i32)(d<DOC_MAXDEPTH ? d : DOC_MAXDEPTH);
}

static i32 curstyle(DocParse *p)
{
    i32 s = 0;
    s |= p->bold   ? STYLE_BOLD   : 0;
    s |= p->italic ? STYLE_ITALIC : 0;
    s |= p->code   ? STYLE_CODE   : 0;
    s |= p->strike ? STYLE_STRIKE : 0;
    s |= p->link>=0 ? STYLE_LINK  : 0;
    s |= p->header ? STYLE_HEADER : 0;
    return s;
}

// The first non-blank line from off, which ends a block's text, on:
// where a block with no text of its own (a rule, an empty code block)
// must be.
static i32 nextline(Str src, i32 off)
{
    iz i = off;
    if (i>0 && src[i-1]!='\n') {
        for (; i<src.len && src[i]!='\n'; i++) {}
        if (i < src.len) i++;
    }
    for (;;) {
        iz j = i;
        for (; j<src.len && (src[j]==' ' || src[j]=='\t' || src[j]=='\r'); j++) {}
        if (j<src.len && src[j]=='\n') {
            i = j + 1;
            continue;
        }
        return (i32)(i<src.len ? i : src.len);
    }
}

static i32 lineend(Str src, i32 off)
{
    iz i = off;
    for (; i<src.len && src[i]!='\n'; i++) {}
    return (i32)i;
}

static i32 openblock(DocParse *p, i32 type)
{
    Doc  *d = p->doc;
    Block b = {};
    b.type   = type;
    b.piece0 = (i32)d->pieces.len;
    b.off0   = b.off1 = p->lastoff;
    b.depth  = clampdepth(p->depth);
    b.quote  = clampdepth(p->quote);
    b.number = -1;
    b.table  = -1;
    if (p->pending && p->depth>0) {
        // The first leaf of an item shows the item's marker.
        i32 lvl = p->depth<=DOC_MAXDEPTH ? p->depth-1 : DOC_MAXDEPTH-1;
        b.item   = 1;
        b.task   = p->task;
        b.number = p->ordered[lvl];
        if (p->ordered[lvl] >= 0) {
            p->ordered[lvl]++;
        }
        p->pending = 0;
    }
    d->blocks = push(p->perm, d->blocks, b);
    p->cur = (i32)d->blocks.len - 1;
    return p->cur;
}

static void addpiece(DocParse *p, Piece pc)
{
    Doc *d = p->doc;
    if (p->cur < 0) {
        // Text outside any leaf: a tight list item's, or stray.
        openblock(p, p->items ? BLOCK_ITEM : BLOCK_PARA);
    }
    Block *b = &d->blocks[p->cur];
    if (!b->npiece) {
        b->off0 = pc.off;
    }
    b->off1 = pc.off + pc.len;
    b->npiece++;
    d->pieces = push(p->perm, d->pieces, pc);
}

static b32 insource(DocParse *p, u8 *s)
{
    return s>=p->base && s<p->base+p->size;
}

// Decode the entities the transcript understands: numeric ones and the
// five named in XML. Others stay as written.
static Str decodeentity(Arena *perm, Str e)
{
    static struct { Str name; Str text; } named[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"},
    };
    for (iz i = 0; i < countof(named); i++) {
        if (named[i].name == e) {
            return named[i].text;
        }
    }
    if (e.len>3 && e[1]=='#' && e[e.len-1]==';') {
        b32 hex = e[2]=='x' || e[2]=='X';
        i64 v   = 0;
        iz  i   = 2 + hex;
        if (i == e.len-1) {
            return e;
        }
        for (; i < e.len-1; i++) {
            u8  c = lowercase(e[i]);
            i32 n = digit(c) ? c-'0' : hex && c>='a' && c<='f' ? c-'a'+10 : -1;
            if (n<0 || n>=(hex?16:10)) {
                return e;
            }
            v = v*(hex?16:10) + n;
            if (v > 0x10ffff) {
                v = 0x110000;  // saturate: invalid
            }
        }
        if (!v || v>0x10ffff || (v>=0xd800 && v<=0xdfff)) {
            v = 0xfffd;
        }
        Str r = {allocbytes(perm, 4), 0};
        r.len = utf8encode(r.data, (i32)v);
        return r;
    }
    return e;
}

static i32 nextcol(i32 col, u8 c)
{
    return c=='\t' ? (col+4) & ~3 : col+1;
}

// md4c makes up its own spaces for a code line's indentation. Recover
// the source's bytes when they span exactly those columns, so that tabs
// come through, given where the line's text begins.
static b32 rawindent(DocParse *p, i32 off, i32 cols, Piece *pc)
{
    Str src = {p->base, p->size};
    i32 ls  = off;
    for (; ls>0 && src[ls-1]!='\n'; ls--) {}
    i32 bs = off;
    for (; bs>ls && (src[bs-1]==' ' || src[bs-1]=='\t'); bs--) {}

    i32 col = 0;
    for (i32 i = ls; i < bs; i++) {
        col = nextcol(col, src[i]);
    }
    i32 end = col;
    for (i32 i = bs; i < off; i++) {
        end = nextcol(end, src[i]);
    }
    i32 target = end - cols;
    for (i32 i = bs; i < off; i++) {
        if (col == target) {
            pc->off   = i;
            pc->len   = off - i;
            pc->text  = {p->base+i, (iz)(off-i)};
            pc->flags = 0;
            return 1;
        }
        col = nextcol(col, src[i]);
    }
    return 0;
}

static int mdtext(MD_TEXTTYPE type, MD_CHAR const *text, MD_SIZE size, void *ud)
{
    DocParse *p = (DocParse *)ud;
    u8       *s = (u8 *)text;
    Piece    pc = {};
    pc.style = curstyle(p);
    if (p->cur>=0 && p->doc->blocks[p->cur].type==BLOCK_HEADING) {
        pc.style |= p->doc->blocks[p->cur].level << STYLE_HSHIFT;
    }
    pc.link  = p->link;
    pc.text  = {s, (iz)size};
    if (insource(p, s)) {
        pc.off = (i32)(s - p->base);
        pc.len = (i32)size;
    } else {
        pc.off   = p->lastoff;
        pc.flags = PIECE_SYNTH;
    }

    switch (type) {
    case MD_TEXT_NULLCHAR:
        pc.text = "\xef\xbf\xbd";
        pc.len  = pc.flags&PIECE_SYNTH ? 0 : 1;
        break;
    case MD_TEXT_BR:
        pc.text   = "\n";
        pc.flags |= PIECE_HARD;
        break;
    case MD_TEXT_SOFTBR:
        pc.text   = "\n";
        pc.flags |= PIECE_SOFT;
        break;
    case MD_TEXT_ENTITY:
        pc.text = decodeentity(p->perm, pc.text);
        break;
    case MD_TEXT_CODE:
        if (p->cur<0 || p->doc->blocks[p->cur].type!=BLOCK_CODE) {
            break;
        }
        if (pc.flags&PIECE_SYNTH && size && text[0]==' ') {
            // A code line's indentation, in chunks: kept as spaces unless
            // the source's own bytes are found when the text arrives.
            if (p->indent0 < 0) {
                p->indent0    = (i32)p->doc->pieces.len;
                p->indentcols = 0;
            }
            p->indentcols += (i32)size;
        } else if (!(pc.flags&PIECE_SYNTH) && p->indent0>=0) {
            Piece raw = pc;
            if (rawindent(p, pc.off, p->indentcols, &raw)) {
                Doc   *d = p->doc;
                Block *b = &d->blocks[p->cur];
                b->npiece    -= (i32)(d->pieces.len - p->indent0);
                d->pieces.len = p->indent0;
                addpiece(p, raw);
            }
            p->indent0 = -1;
        } else {
            p->indent0 = -1;
        }
        break;
    default:
        break;
    }

    if (!(pc.flags & PIECE_SYNTH)) {
        p->lastoff = pc.off + pc.len;
    }
    addpiece(p, pc);
    return 0;
}

// Split the code block's pieces at the highlighter's token boundaries.
static void highlightblock(DocParse *p, Block *b)
{
    Doc *d     = p->doc;
    iz   total = 0;
    for (i32 i = 0; i < b->npiece; i++) {
        total += d->pieces[b->piece0+i].text.len;
    }
    if (!total) {
        return;
    }

    Str code = {allocbytes(p->perm, total), 0};
    for (i32 i = 0; i < b->npiece; i++) {
        Str t = d->pieces[b->piece0+i].text;
        copybytes(code.data+code.len, t.data, t.len);
        code.len += t.len;
    }

    u64 key = hash(code) ^ hash(b->lang)*0x9e3779b97f4a7c15u;
    Slice<HlTok> toks = {};
    HlMemo **m = p->memo;
    if (m) {
        for (u64 h = key; *m; h <<= 2) {
            if ((*m)->key == key) break;
            m = &(*m)->child[h>>62];
        }
    }
    if (m && *m) {
        toks = (*m)->toks;
    } else {
        toks = highlight(p->perm, b->lang, code).toks;
        if (m) {
            *m = alloc<HlMemo>(p->perm);
            (*m)->key  = key;
            (*m)->toks = toks;
        }
    }

    // The block's pieces are the last ones: copy them out, then rebuild.
    i32    n   = b->npiece;
    Piece *old = alloc<Piece>(p->perm, n);
    for (i32 i = 0; i < n; i++) {
        old[i] = d->pieces[b->piece0+i];
    }
    d->pieces.len = b->piece0;
    b->npiece     = 0;

    iz t   = 0;  // token index
    iz pos = 0;  // code offset of old[i]
    for (i32 i = 0; i < n; i++) {
        Piece o  = old[i];
        iz    pb = pos;
        iz    pe = pos + o.text.len;
        for (iz at = pb; at < pe;) {
            for (; t<toks.len && toks[t].end<=at; t++) {}
            iz    end = t<toks.len && toks[t].end<pe ? toks[t].end : pe;
            Piece q   = o;
            q.text = takehead(cuthead(o.text, at-pb), end-at);
            q.hl   = t<toks.len ? (u8)toks[t].cls : (u8)HL_PLAIN;
            if (!(o.flags & PIECE_SYNTH) && o.len==o.text.len) {
                q.off = o.off + (i32)(at - pb);
                q.len = (i32)(end - at);
            } else if (at > pb) {
                q.len = 0;  // the rest of a synthetic or entity piece
                q.off = o.off + o.len;
            }
            d->pieces = push(p->perm, d->pieces, q);
            b->npiece++;
            at = end;
        }
        pos = pe;
    }
}

static void closeblock(DocParse *p)
{
    if (p->cur < 0) {
        return;
    }
    Doc   *d = p->doc;
    Block *b = &d->blocks[p->cur];
    if (b->type==BLOCK_CODE && b->npiece) {
        highlightblock(p, b);
    }
    p->indent0 = -1;
    if (!b->npiece && (b->type==BLOCK_HR || b->type==BLOCK_CODE)) {
        b->off0    = nextline({p->base, p->size}, b->off0);
        b->off1    = lineend({p->base, p->size}, b->off0);
        p->lastoff = b->off1;
    }

    // The layout cache key: what the block shows and how.
    Hash64 h = {};
    i64 attrs[] = {
        b->type, b->level, b->depth, b->quote, b->number, b->item, b->task,
        b->align, b->row, b->col, b->fence,
    };
    hashbytes(&h, attrs, sizeof(attrs));
    hashbytes(&h, b->lang.data, b->lang.len);
    for (i32 i = 0; i < b->npiece; i++) {
        Piece *pc = &d->pieces[b->piece0+i];
        u64 w = (u64)pc->style | (u64)pc->hl<<16 | (u64)pc->flags<<24;
        hashword(&h, w);
        hashbytes(&h, pc->text.data, pc->text.len);
    }
    b->hash = hashfinal(&h);
    p->cur  = -1;
}

static int mdenter(MD_BLOCKTYPE type, void *detail, void *ud)
{
    DocParse *p = (DocParse *)ud;
    if (type != MD_BLOCK_DOC) {
        closeblock(p);  // an item's text ends where a nested block begins
    }
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_ADMONITION:
        p->quote++;
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (p->depth < DOC_MAXDEPTH) {
            p->ordered[p->depth] = -1;
            if (type == MD_BLOCK_OL) {
                MD_BLOCK_OL_DETAIL *ol = (MD_BLOCK_OL_DETAIL *)detail;
                p->ordered[p->depth] = (i32)(ol->start<1000000000 ? ol->start : 0);
            }
        }
        p->depth++;
        break;
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *li = (MD_BLOCK_LI_DETAIL *)detail;
        p->items++;
        p->pending = 1;
        p->task    = 0;
        if (li->is_task) {
            p->task = li->task_mark==' ' ? 1 : 2;
        }
    } break;
    case MD_BLOCK_HR:
        openblock(p, BLOCK_HR);
        break;
    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
        i32 b = openblock(p, BLOCK_HEADING);
        p->doc->blocks[b].level = (i32)h->level;
    } break;
    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *c = (MD_BLOCK_CODE_DETAIL *)detail;
        i32    i = openblock(p, BLOCK_CODE);
        Block *b = &p->doc->blocks[i];
        b->fence = c->fence_char != 0;
        b->lang  = clone(p->perm, {(u8 *)c->lang.text, (iz)c->lang.size});
        p->code++;
    } break;
    case MD_BLOCK_P:
        openblock(p, BLOCK_PARA);
        break;
    case MD_BLOCK_TABLE:
        p->table = p->doc->ntables++;
        p->row   = -1;
        break;
    case MD_BLOCK_TR:
        p->row++;
        p->col = -1;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
        p->col++;
        p->header = type == MD_BLOCK_TH;
        p->align  = td->align==MD_ALIGN_CENTER ? 1 : td->align==MD_ALIGN_RIGHT ? 2 : 0;
        i32    i = openblock(p, BLOCK_CELL);
        Block *b = &p->doc->blocks[i];
        b->table = p->table;
        b->row   = p->row;
        b->col   = p->col;
        b->align = p->align;
    } break;
    default:
        break;
    }
    return 0;
}

static int mdleave(MD_BLOCKTYPE type, void *, void *ud)
{
    DocParse *p = (DocParse *)ud;
    switch (type) {
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_ADMONITION:
        closeblock(p);
        p->quote--;
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        closeblock(p);
        p->depth--;
        break;
    case MD_BLOCK_LI:
        if (p->pending) {
            // An empty item still shows its marker.
            openblock(p, BLOCK_ITEM);
        }
        closeblock(p);
        p->items--;
        break;
    case MD_BLOCK_CODE:
        closeblock(p);
        p->code--;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        closeblock(p);
        p->header = 0;
        break;
    default:
        closeblock(p);
        break;
    }
    return 0;
}

static int mdenterspan(MD_SPANTYPE type, void *detail, void *ud)
{
    DocParse *p = (DocParse *)ud;
    switch (type) {
    case MD_SPAN_EM:     p->italic++; break;
    case MD_SPAN_STRONG: p->bold++;   break;
    case MD_SPAN_CODE:   p->code++;   break;
    case MD_SPAN_DEL:    p->strike++; break;
    case MD_SPAN_A: {
        MD_SPAN_A_DETAIL *a = (MD_SPAN_A_DETAIL *)detail;
        Str href = clone(p->perm, {(u8 *)a->href.text, (iz)a->href.size});
        p->doc->links = push(p->perm, p->doc->links, href);
        p->link = (i32)p->doc->links.len - 1;
    } break;
    case MD_SPAN_IMG: {
        MD_SPAN_IMG_DETAIL *img = (MD_SPAN_IMG_DETAIL *)detail;
        Str src = clone(p->perm, {(u8 *)img->src.text, (iz)img->src.size});
        p->doc->links = push(p->perm, p->doc->links, src);
        p->link = (i32)p->doc->links.len - 1;
        p->italic++;
    } break;
    case MD_SPAN_FOOTNOTE_REF: {
        MD_SPAN_FOOTNOTE_REF_DETAIL *f = (MD_SPAN_FOOTNOTE_REF_DETAIL *)detail;
        Str label = {(u8 *)f->label.text, (iz)f->label.size};
        Piece pc = {};
        pc.off   = p->lastoff;
        pc.flags = PIECE_SYNTH;
        pc.style = curstyle(p) | STYLE_LINK;
        pc.link  = -1;
        pc.text  = concat(p->perm, concat(p->perm, "[", label), "]");
        addpiece(p, pc);
    } break;
    default:
        break;
    }
    return 0;
}

static int mdleavespan(MD_SPANTYPE type, void *, void *ud)
{
    DocParse *p = (DocParse *)ud;
    switch (type) {
    case MD_SPAN_EM:     p->italic--; break;
    case MD_SPAN_STRONG: p->bold--;   break;
    case MD_SPAN_CODE:   p->code--;   break;
    case MD_SPAN_DEL:    p->strike--; break;
    case MD_SPAN_A:      p->link = -1; break;
    case MD_SPAN_IMG:    p->link = -1; p->italic--; break;
    default: break;
    }
    return 0;
}

// Plain text: one block per line, nothing interpreted.
static Doc *parseplain(Arena *perm, Str src, i32 style)
{
    Doc     *d = alloc<Doc>(perm);
    DocParse p = {};
    p.perm  = perm;
    p.doc   = d;
    p.cur   = -1;
    p.base  = src.data;
    p.size  = src.len;
    d->src   = src;
    d->plain = 1;
    for (Str rest = src;;) {
        Cut c = cut(rest, '\n');
        i32 off = (i32)(rest.data - src.data);
        p.lastoff = off;
        openblock(&p, BLOCK_PLAIN);
        if (c.head.len) {
            Piece pc = {};
            pc.off   = off;
            pc.len   = (i32)c.head.len;
            pc.text  = c.head;
            pc.style = style;
            pc.link  = -1;
            addpiece(&p, pc);
        }
        closeblock(&p);
        if (!c.ok) {
            break;
        }
        rest = c.tail;
    }
    return d;
}

// Parse markdown into a document in perm, referring into src. Messages
// past DOC_MAXSIZE are shown as plain text. The memo, if given, keeps
// highlighted code across parses of a growing message.
static Doc *parsedoc(Arena *perm, Str src, HlMemo **memo)
{
    if (src.len > DOC_MAXSIZE) {
        return parseplain(perm, src, 0);
    }

    Doc     *d = alloc<Doc>(perm);
    DocParse p = {};
    p.perm = perm;
    p.doc  = d;
    p.memo = memo;
    p.base    = src.data;
    p.size    = src.len;
    p.cur     = -1;
    p.link    = -1;
    p.indent0 = -1;
    d->src    = src;

    MD_PARSER parser = {};
    parser.flags       = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = mdenter;
    parser.leave_block = mdleave;
    parser.enter_span  = mdenterspan;
    parser.leave_span  = mdleavespan;
    parser.text        = mdtext;
    if (src.len) {
        md_parse((MD_CHAR *)src.data, (MD_SIZE)src.len, &parser, &p);
    }
    closeblock(&p);
    return d;
}
