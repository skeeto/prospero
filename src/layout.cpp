// Block layout: wrapping a document's pieces into lines of runs
//
// Layout never calls ImGui. It measures text through a Measure, which
// the GUI backs with its fonts and tests back with fixed advances, and
// produces positions only: lines of runs, each run a byte range of one
// piece, plus decorations (list markers, quote bars, code backgrounds,
// rules, table grids) for the renderer to draw underneath.
//
// Each block is laid out on its own, keyed by (block hash, width,
// scale), so a re-parse that leaves a block unchanged leaves its layout
// cached. A table is laid out as a whole, under its first cell. Runs
// refer to pieces relative to the first block's piece0, so a layout
// stays valid wherever its block lands in a later parse.
//
// Wrapping is greedy at spaces, breaking within a word only when the
// word alone overflows a line. Code wraps at any character. Tabs in
// code advance to the next multiple of four spaces.
//
// This is free and unencumbered software released into the public domain.

struct FontMetrics {
    f32 lineh;   // line height
    f32 ascent;  // baseline below the line's top
};

struct Measure {
    f32         (*advance)(void *ctx, i32 style, i32 cp);
    FontMetrics (*font)(void *ctx, i32 style);
    void         *ctx;
};

enum : u32 {
    RUN_NODRAW = 1u<<0,  // a tab or break: occupies space, draws nothing
};

struct Run {
    f32 x, w;
    i32 piece;   // relative to the block layout's first piece
    i32 b0, b1;  // byte range within the piece's text
    u32 flags;
};

struct Line {
    f32 y, h, base;  // top, height, baseline below top
    i32 run0, nrun;
};

enum : i32 {
    DECO_MARKER,    // list bullet or number: text drawn at x, baseline y
    DECO_QUOTE,     // quote bar
    DECO_CODEBG,    // code block background
    DECO_RULE,      // horizontal rule
    DECO_HEADERBG,  // table header row background
    DECO_GRID,      // table cell border line
};

struct Deco {
    i32 kind;
    f32 x, y, w, h;
    Str text;   // DECO_MARKER
};

struct BlockLayout {
    u64          hash;
    f32          width;
    f32          scale;
    f32          h;
    i32          nblocks;  // blocks covered: more than one for a table
    b32          grid;     // a table: lines are not in y order
    Slice<Line>  lines;
    Slice<Run>   runs;
    Slice<Deco>  decos;
};

// Spacing in em of the base font, so it scales with it.
struct Spacing {
    f32 em;
    f32 indent;   // per list level
    f32 quote;    // per quote level
    f32 pad;      // inside code blocks and cells
    f32 gap;      // between blocks
    f32 tight;    // between items of a tight list
};

static Spacing spacing(Measure *m)
{
    FontMetrics f = m->font(m->ctx, 0);
    f32 em = f.lineh;
    Spacing s = {};
    s.em     = em;
    s.indent = 1.6f * em;
    s.quote  = 1.0f * em;
    s.pad    = 0.4f * em;
    s.gap    = 0.6f * em;
    s.tight  = 0.15f * em;
    return s;
}

static b32 piecespace(u8 c)
{
    return c==' ' || c=='\t';
}

// Measure a byte range of text in one style.
static f32 measure(Measure *m, i32 style, Str s)
{
    f32 w = 0;
    for (iz i = 0; i < s.len;) {
        w += m->advance(m->ctx, style, utf8decode(s, &i));
    }
    return w;
}


// Wrapping

struct Wrap {
    Arena       *perm;
    Measure     *m;
    BlockLayout *bl;
    Piece       *pieces;  // the first piece of the layout
    f32          x0;      // line start
    f32          width;   // available width from x0
    f32          y;       // top of the current line
    f32          x;       // pen position
    f32          lineh;   // tallest font on the current line
    f32          ascent;
    i32          run0;    // first run of the current line
    b32          code;    // wrap anywhere, tabs to stops
    // Last break opportunity on this line: just after a space
    b32          canbreak;
    i32          bpiece;
    iz           bbyte;
    i32          bruns;   // run count up to and including the break
};

static void wrapfont(Wrap *w, i32 style)
{
    FontMetrics f = w->m->font(w->m->ctx, style);
    if (f.lineh > w->lineh) w->lineh = f.lineh;
    if (f.ascent > w->ascent) w->ascent = f.ascent;
}

static void endline(Wrap *w)
{
    BlockLayout *bl = w->bl;
    Line l = {};
    l.y    = w->y;
    l.h    = w->lineh;
    l.base = w->ascent;
    l.run0 = w->run0;
    l.nrun = (i32)bl->runs.len - w->run0;
    bl->lines = push(w->perm, bl->lines, l);
    w->y       += w->lineh;
    w->x        = w->x0;
    w->run0     = (i32)bl->runs.len;
    w->canbreak = 0;
    FontMetrics f = w->m->font(w->m->ctx, 0);
    w->lineh  = f.lineh;
    w->ascent = f.ascent;
}

// Append to the current run if it continues the same piece, else start
// a new one.
static void addrun(Wrap *w, i32 piece, iz b0, iz b1, f32 adv, u32 flags)
{
    BlockLayout *bl = w->bl;
    if (bl->runs.len > w->run0 && !flags) {
        Run *r = &bl->runs[bl->runs.len-1];
        if (r->piece==piece && r->b1==b0 && !r->flags) {
            r->b1  = (i32)b1;
            r->w  += adv;
            w->x  += adv;
            return;
        }
    }
    Run r = {};
    r.x     = w->x;
    r.w     = adv;
    r.piece = piece;
    r.b0    = (i32)b0;
    r.b1    = (i32)b1;
    r.flags = flags;
    bl->runs = push(w->perm, bl->runs, r);
    w->x += adv;
}

// Lay out pieces [p0, p1) of the layout's pieces from the current pen.
static void wrappieces(Wrap *w, i32 p0, i32 p1)
{
    Measure *m      = w->m;
    f32      spacew = m->advance(m->ctx, 0, ' ');
    for (i32 p = p0; p < p1; p++) {
        Piece *pc = &w->pieces[p];
        wrapfont(w, pc->style);

        if (pc->flags & PIECE_HARD) {
            addrun(w, p, 0, pc->text.len, 0, RUN_NODRAW);
            endline(w);
            continue;
        }
        if (pc->flags & PIECE_SOFT) {
            f32 adv = m->advance(m->ctx, pc->style, ' ');
            addrun(w, p, 0, pc->text.len, adv, RUN_NODRAW);
            w->canbreak = 1;
            w->bpiece   = p;
            w->bbyte    = pc->text.len;
            w->bruns    = (i32)w->bl->runs.len;
            continue;
        }

        Str s = pc->text;
        for (iz i = 0; i < s.len;) {
            iz  start = i;
            i32 cp    = utf8decode(s, &i);
            if (cp == '\n') {  // a code line ends
                addrun(w, p, start, i, 0, RUN_NODRAW);
                endline(w);
                continue;
            }

            f32 adv   = 0;
            u32 flags = 0;
            if (cp=='\t' && w->code) {
                f32 tab = 4*spacew;
                f32 col = w->x - w->x0;
                adv   = tab*(f32)((i32)(col/tab) + 1) - col;
                flags = RUN_NODRAW;
            } else {
                adv = m->advance(m->ctx, pc->style, cp);
            }

            b32 empty = (i32)w->bl->runs.len == w->run0;
            if (!empty && w->x+adv > w->x0+w->width && !piecespace((u8)cp)) {
                if (w->canbreak && !w->code) {
                    // Move everything after the break to a new line.
                    w->bl->runs.len = w->bruns;
                    Run *r = &w->bl->runs[w->bruns-1];
                    if (r->piece==w->bpiece && r->b1>w->bbyte) {
                        r->b1 = (i32)w->bbyte;  // cut within the run
                        r->w  = measure(m, w->pieces[r->piece].style,
                                        {w->pieces[r->piece].text.data+r->b0,
                                         r->b1-r->b0});
                    }
                    i32 bp = w->bpiece;
                    iz  bb = w->bbyte;
                    endline(w);
                    // Resume from the break.
                    if (bb == w->pieces[bp].text.len) {
                        p = bp;  // the loop increments past it
                        goto nextpiece;
                    }
                    p  = bp;
                    pc = &w->pieces[p];
                    s  = pc->text;
                    i  = bb;
                    wrapfont(w, pc->style);
                    continue;
                }
                endline(w);
            }

            addrun(w, p, start, i, adv, flags);
            if (piecespace((u8)cp) && !w->code) {
                w->canbreak = 1;
                w->bpiece   = p;
                w->bbyte    = i;
                w->bruns    = (i32)w->bl->runs.len;
            }
        }
        nextpiece:;
    }
}

static void adddeco(Wrap *w, i32 kind, f32 x, f32 y, f32 dw, f32 dh, Str text)
{
    Deco d = {};
    d.kind = kind;
    d.x    = x;
    d.y    = y;
    d.w    = dw;
    d.h    = dh;
    d.text = text;
    w->bl->decos = push(w->perm, w->bl->decos, d);
}

static Str markertext(Arena *perm, Block *b)
{
    if (b->task) {
        return b->task==2 ? Str("\xe2\x98\x91") : Str("\xe2\x98\x90");  // ☑ ☐
    } else if (b->number < 0) {
        static Str bullets[] = {"\xe2\x80\xa2", "\xe2\x97\xa6", "\xe2\x96\xaa"};
        return bullets[(b->depth-1) % countof(bullets)];
    }
    Buf buf(perm, 16);
    print(&buf, (i64)b->number);
    putbyte(&buf, '.');
    return finish(&buf);
}

// Left edge of a block's text: list and quote indentation.
static f32 blockindent(Spacing *sp, Block *b)
{
    return (f32)b->depth*sp->indent + (f32)b->quote*sp->quote;
}

static Wrap newwrap(Arena *perm, Measure *m, BlockLayout *bl, Piece *pieces)
{
    Wrap w = {};
    w.perm   = perm;
    w.m      = m;
    w.bl     = bl;
    w.pieces = pieces;
    FontMetrics f = m->font(m->ctx, 0);
    w.lineh  = f.lineh;
    w.ascent = f.ascent;
    return w;
}

// Lay out one block (not a table cell) at the given width.
static BlockLayout *layoutblock(Arena *perm, Measure *m, Doc *d, i32 bi,
                                f32 width, f32 scale)
{
    Block       *b  = &d->blocks[bi];
    BlockLayout *bl = alloc<BlockLayout>(perm);
    bl->hash    = b->hash;
    bl->width   = width;
    bl->scale   = scale;
    bl->nblocks = 1;

    Spacing sp = spacing(m);
    f32     x0 = blockindent(&sp, b);
    f32     w0 = width - x0;
    f32     mn = width<4*sp.em ? width : 4*sp.em;
    w0 = w0<mn ? mn : w0;  // deep nesting overflows rather than vanishes

    Wrap w = newwrap(perm, m, bl, d->pieces.data+b->piece0);
    w.x0    = x0;
    w.x     = x0;
    w.width = w0;

    if (b->type == BLOCK_CODE) {
        w.code   = 1;
        w.x0    += sp.pad;
        w.x      = w.x0;
        w.width -= 2*sp.pad;
        w.y      = sp.pad;
        FontMetrics f = m->font(m->ctx, STYLE_CODE);
        w.lineh  = f.lineh;
        w.ascent = f.ascent;
    }

    if (b->type == BLOCK_HR) {
        f32 h = sp.em;
        adddeco(&w, DECO_RULE, x0, h/2 - 1, w0, 2, "");
        bl->h = h;
        return bl;
    }

    wrappieces(&w, 0, b->npiece);
    if ((i32)bl->runs.len>w.run0 || !bl->lines.len) {
        endline(&w);  // the last line, or an empty block's one line
    }

    f32 h = w.y;
    if (b->type == BLOCK_CODE) {
        h += sp.pad;
        adddeco(&w, DECO_CODEBG, x0, 0, w0, h, "");
    }
    if (b->item && bl->lines.len) {
        Line *l      = &bl->lines[0];
        Str   marker = markertext(perm, b);
        f32   mw     = measure(m, 0, marker);
        f32   mx     = x0 - mw - 0.4f*sp.em;
        adddeco(&w, DECO_MARKER, mx, l->y + l->base, mw, 0, marker);
    }
    for (i32 q = 0; q < b->quote; q++) {
        f32 qx = (f32)b->depth*sp.indent + (f32)q*sp.quote + 0.2f*sp.em;
        adddeco(&w, DECO_QUOTE, qx, 0, 0.18f*sp.em, h, "");
    }
    bl->h = h;
    return bl;
}

struct TableShape {
    u64 hash;   // of every cell
    i32 n;      // cells
    i32 nrows;
    i32 ncols;
};

static TableShape tableshape(Doc *d, i32 bi)
{
    TableShape r     = {};
    Block     *first = &d->blocks[bi];
    Hash64     h     = {};
    for (; bi+r.n < d->blocks.len; r.n++) {
        Block *c = &d->blocks[bi+r.n];
        if (c->type!=BLOCK_CELL || c->table!=first->table) break;
        r.ncols = c->col+1>r.ncols ? c->col+1 : r.ncols;
        r.nrows = c->row+1>r.nrows ? c->row+1 : r.nrows;
        hashword(&h, c->hash);
    }
    r.hash = hashfinal(&h);
    return r;
}

// Lay out the table whose first cell is block bi.
static BlockLayout *layouttable(Arena *perm, Measure *m, Doc *d, i32 bi,
                                f32 width, f32 scale)
{
    Block     *first = &d->blocks[bi];
    TableShape ts    = tableshape(d, bi);
    i32        n     = ts.n;
    i32        ncols = ts.ncols;
    i32        nrows = ts.nrows;

    BlockLayout *bl = alloc<BlockLayout>(perm);
    bl->hash    = ts.hash;
    bl->width   = width;
    bl->scale   = scale;
    bl->nblocks = n;
    bl->grid    = 1;

    Spacing sp    = spacing(m);
    f32     x0    = blockindent(&sp, first);
    f32     avail = width - x0;
    Piece  *base  = d->pieces.data + first->piece0;

    // Natural column widths: each cell on one line.
    f32 *natural = alloc<f32>(perm, ncols);
    for (i32 i = 0; i < n; i++) {
        Block *c  = &d->blocks[bi+i];
        f32    cw = 2*sp.pad;
        for (i32 k = 0; k < c->npiece; k++) {
            Piece *pc = &d->pieces[c->piece0+k];
            cw += pc->flags&(PIECE_HARD|PIECE_SOFT)
                ? m->advance(m->ctx, pc->style, ' ')
                : measure(m, pc->style, pc->text);
        }
        natural[c->col] = cw>natural[c->col] ? cw : natural[c->col];
    }
    f32 total = 0;
    for (i32 c = 0; c < ncols; c++) {
        natural[c] = natural[c]<3*sp.em ? 3*sp.em : natural[c];
        total += natural[c];
    }
    f32 shrink = total>avail && total>0 ? avail/total : 1;
    f32 *colx  = alloc<f32>(perm, ncols+1);
    colx[0] = x0;
    for (i32 c = 0; c < ncols; c++) {
        colx[c+1] = colx[c] + natural[c]*shrink;
    }

    // Rows: lay out each cell in its column, then size the row.
    Wrap w = newwrap(perm, m, bl, base);
    f32  y = 0;
    i32  i = 0;
    for (i32 r = 0; r < nrows; r++) {
        f32 rowh = 0;
        for (; i<n && d->blocks[bi+i].row==r; i++) {
            Block *c = &d->blocks[bi+i];
            w.x0    = colx[c->col] + sp.pad;
            w.x     = w.x0;
            w.width = colx[c->col+1] - colx[c->col] - 2*sp.pad;
            w.y     = y + sp.pad;
            w.run0  = (i32)bl->runs.len;
            iz  l0  = bl->lines.len;
            i32 rel = c->piece0 - first->piece0;
            wrappieces(&w, rel, rel+c->npiece);
            if ((i32)bl->runs.len > w.run0) {
                endline(&w);
            }
            // Alignment: shift the cell's lines within the column.
            for (iz l = l0; c->align && l < bl->lines.len; l++) {
                Line *ln = &bl->lines[l];
                if (!ln->nrun) continue;
                Run *last  = &bl->runs[ln->run0+ln->nrun-1];
                f32  slack = w.x0 + w.width - (last->x + last->w);
                f32  dx    = c->align==1 ? slack/2 : slack;
                for (i32 k = 0; dx>0 && k < ln->nrun; k++) {
                    bl->runs[ln->run0+k].x += dx;
                }
            }
            f32 ch = w.y + sp.pad - y;
            rowh = ch>rowh ? ch : rowh;
        }
        if (r == 0) {
            adddeco(&w, DECO_HEADERBG, x0, y, colx[ncols]-x0, rowh, "");
        }
        adddeco(&w, DECO_GRID, x0, y, colx[ncols]-x0, 1, "");
        y += rowh;
    }
    adddeco(&w, DECO_GRID, x0, y, colx[ncols]-x0, 1, "");
    for (i32 c = 0; c <= ncols; c++) {
        adddeco(&w, DECO_GRID, colx[c], 0, 1, y, "");
    }
    bl->h = y;
    return bl;
}


// The layout cache: block layouts by (hash, width, scale), in a 4-ary
// hash trie in the cache arena.

struct LayoutCache {
    LayoutCache *child[4];
    u64          key;
    BlockLayout *bl;
};

static u64 layoutkey(u64 hash, f32 width, f32 scale)
{
    Hash64 h = {hash};
    hashword(&h, (u64)(i64)(width + 0.5f));
    hashword(&h, (u64)(i64)(scale*1000 + 0.5f));
    return hashfinal(&h);
}

// The layout of block bi, from the cache or made and cached. Tables come
// whole from their first cell.
static BlockLayout *getlayout(LayoutCache **cache, Arena *perm, Measure *m,
                              Doc *d, i32 bi, f32 width, f32 scale)
{
    Block *b    = &d->blocks[bi];
    u64    hash = b->type==BLOCK_CELL ? tableshape(d, bi).hash : b->hash;
    u64    key  = layoutkey(hash, width, scale);
    LayoutCache **slot = cache;
    for (u64 h = key; *slot; h <<= 2) {
        if ((*slot)->key == key) {
            return (*slot)->bl;
        }
        slot = &(*slot)->child[h>>62];
    }
    BlockLayout *bl = b->type==BLOCK_CELL
        ? layouttable(perm, m, d, bi, width, scale)
        : layoutblock(perm, m, d, bi, width, scale);
    *slot = alloc<LayoutCache>(perm);
    (*slot)->key = key;
    (*slot)->bl  = bl;
    return bl;
}


// Messages: block layouts stacked with gaps

struct Placed {
    i32          block;  // first block covered
    f32          y;
    BlockLayout *bl;
};

struct MsgLayout {
    Slice<Placed> blocks;
    f32           width;
    f32           h;
};

static f32 blockgap(Spacing *sp, Block *prev, Block *b)
{
    if (prev->type==BLOCK_PLAIN && b->type==BLOCK_PLAIN) {
        return 0;
    } else if (b->type==BLOCK_ITEM && (prev->type==BLOCK_ITEM || prev->depth<b->depth)) {
        return sp->tight;
    } else if (b->type == BLOCK_HEADING) {
        return 1.2f * sp->gap;
    }
    return sp->gap;
}

static MsgLayout layoutmsg(LayoutCache **cache, Arena *perm, Measure *m,
                           Doc *d, f32 width, f32 scale)
{
    MsgLayout r = {};
    r.width = width;
    Spacing sp = spacing(m);
    f32     y  = 0;
    for (i32 bi = 0; bi < d->blocks.len;) {
        BlockLayout *bl = getlayout(cache, perm, m, d, bi, width, scale);
        if (bi) {
            y += blockgap(&sp, &d->blocks[bi-1], &d->blocks[bi]);
        }
        Placed p = {bi, y, bl};
        r.blocks = push(perm, r.blocks, p);
        y  += bl->h;
        bi += bl->nblocks;
    }
    r.h = y;
    return r;
}


// Positions: between source offsets and points

// Source offset of byte i of a piece's display text.
static i32 pieceoff(Piece *p, iz i)
{
    if (p->flags & PIECE_SYNTH) {
        return p->off;
    } else if (p->len == p->text.len) {
        return p->off + (i32)i;
    }
    return i ? p->off+p->len : p->off;  // an entity is indivisible
}

// Source range of a run.
static i32 runbeg(Piece *pieces, Run *r)
{
    return pieceoff(&pieces[r->piece], r->b0);
}

static i32 runend(Piece *pieces, Run *r)
{
    return pieceoff(&pieces[r->piece], r->b1);
}

// The source offset at x within a run: the nearer edge of the character
// under it.
static i32 runhit(Measure *m, Piece *pieces, Run *r, f32 x)
{
    Piece *p = &pieces[r->piece];
    if (r->flags & RUN_NODRAW) {
        return x < r->x+r->w/2 ? runbeg(pieces, r) : runend(pieces, r);
    }
    f32 pen = r->x;
    Str s   = p->text;
    for (iz i = r->b0; i < r->b1;) {
        iz  start = i;
        f32 adv   = m->advance(m->ctx, p->style, utf8decode(s, &i));
        if (x < pen + adv/2) {
            return pieceoff(p, start);
        }
        pen += adv;
    }
    return runend(pieces, r);
}

// The x of a source offset within a run, clamped to its ends.
static f32 runx(Measure *m, Piece *pieces, Run *r, i32 off)
{
    Piece *p = &pieces[r->piece];
    if (off <= runbeg(pieces, r)) {
        return r->x;
    } else if (off >= runend(pieces, r)) {
        return r->x + r->w;
    } else if (p->flags&PIECE_SYNTH || p->len!=p->text.len || r->flags) {
        return r->x + r->w;
    }
    f32 pen = r->x;
    Str s   = p->text;
    iz  end = off - p->off;
    for (iz i = r->b0; i < end;) {
        pen += m->advance(m->ctx, p->style, utf8decode(s, &i));
    }
    return pen;
}

// The line of a block layout at y, or nearest to it; -1 if none.
static i32 linehit(BlockLayout *bl, f32 x, f32 y)
{
    if (!bl->lines.len) {
        return -1;
    }
    if (bl->grid) {
        // Table cells: the line whose box holds the point, else the
        // nearest by vertical then horizontal distance.
        i32 best  = 0;
        f32 bestd = 1e30f;
        for (iz i = 0; i < bl->lines.len; i++) {
            Line *l = &bl->lines[i];
            f32 dy = y<l->y ? l->y-y : y>l->y+l->h ? y-l->y-l->h : 0;
            f32 lx0 = l->nrun ? bl->runs[l->run0].x : 0;
            Run *last = l->nrun ? &bl->runs[l->run0+l->nrun-1] : 0;
            f32 lx1 = last ? last->x+last->w : lx0;
            f32 dx = x<lx0 ? lx0-x : x>lx1 ? x-lx1 : 0;
            f32 d  = dy*1000 + dx;
            if (d < bestd) {
                bestd = d;
                best  = (i32)i;
            }
        }
        return best;
    }
    iz lo = 0, hi = bl->lines.len;
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (bl->lines[mid].y <= y) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (i32)lo;
}

// The source offset nearest to (x, y) in a placed block. Points left of
// a line select its start, right of it its end.
static i32 blockhit(Measure *m, Doc *d, Placed *p, f32 x, f32 y)
{
    BlockLayout *bl     = p->bl;
    Block       *b      = &d->blocks[p->block];
    Piece       *pieces = d->pieces.data + b->piece0;
    i32          li     = linehit(bl, x, y - p->y);
    if (li < 0) {
        return b->off0;
    }
    Line *l = &bl->lines[li];
    if (!l->nrun) {
        // An empty line: the end of the previous line's text, if any.
        for (i32 k = li-1; k >= 0; k--) {
            Line *pl = &bl->lines[k];
            if (pl->nrun) return runend(pieces, &bl->runs[pl->run0+pl->nrun-1]);
        }
        return b->off0;
    }
    Run *first = &bl->runs[l->run0];
    if (x < first->x) {
        return runbeg(pieces, first);
    }
    for (i32 k = 0; k < l->nrun; k++) {
        Run *r = &bl->runs[l->run0+k];
        if (x < r->x+r->w) {
            return runhit(m, pieces, r, x);
        }
    }
    Run *last = &bl->runs[l->run0+l->nrun-1];
    if (last->flags & RUN_NODRAW) {
        return runbeg(pieces, last);  // before the break, not after it
    }
    return runend(pieces, last);
}

// The source offset nearest to (x, y) in a message laid out by
// layoutmsg, at y relative to the message's top.
static i32 msghit(Measure *m, Doc *d, MsgLayout *ml, f32 x, f32 y)
{
    if (!ml->blocks.len) {
        return 0;
    }
    iz lo = 0, hi = ml->blocks.len;
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (ml->blocks[mid].y <= y) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    Placed *p = &ml->blocks[lo];
    if (y > p->y+p->bl->h && lo+1<ml->blocks.len) {
        // In the gap below: whichever block is nearer.
        Placed *n = &ml->blocks[lo+1];
        if (n->y-y < y-(p->y+p->bl->h)) p = n;
    }
    return blockhit(m, d, p, x, y);
}
