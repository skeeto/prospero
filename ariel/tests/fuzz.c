// ariel fuzz target: AFL++ persistent mode, or one input from stdin
//
// $ afl-clang-fast -std=c11 -g3 -fsanitize=address,undefined -o fuzz fuzz.c
// $ afl-fuzz -i corpus -o out ./fuzz
// $ cc -std=c11 -g3 -fsanitize=address,undefined -o fuzz fuzz.c && ./fuzz <input
//
// The first bytes of an input choose the size, flags and feed split. The
// block is malloc'ed at exactly ariel_size so ASan sees overruns. After
// each piece a checker verifies the invariants: bounds, wide pairs, the
// row map, pool references. Feeds interleave with resize, move, the
// encoders and the other host calls, under these cross-checks:
//
// - any split gives the same terminal, byte by byte included, which
//   also holds the ASCII fast path to the slow one;
// - a memcpy'd fork, fed and driven the same, stays identical;
// - ariel_move keeps every visible thing;
// - replies hold only ESC, BEL and printable ASCII;
// - the generation never goes down.
//
// With history:
//
// - the history checker: records in the ring, in cyclic order, apart,
//   well formed, and the row count right;
// - a resize keeps the logical text (history and the main screen, rows
//   joined into lines), unless history evicted or rows below the cursor
//   were dropped (prompt marks go with them, on blank rows too), or a
//   prompt mark inside a line, after a wrapped row or on the first row
//   continuing history, merged with one on its line's first row, and keeps
//   the main screen's saved cursor on its text while the alternate
//   screen is up (a cursor in erased cells whose line's later text was
//   on rows dropped stays as many columns in, past the line's shorter
//   text);
// - resizing the width there and back with nothing between changes
//   nothing (a taller screen pulls history, so not the height; a line
//   from history that joins the screen stays there; a cursor clamped
//   to the narrower screen stays clamped; a prompt mark goes to its
//   line's first row; a cursor between two rows of a line, at the
//   start of one or waiting to wrap at the end of the other, may come
//   back as the other; a line's styled fill is lost when its text
//   fills the last row, and erased cells at the margin become the
//   fill, as do erased cells wrapping onto an empty last row; a cursor
//   on a margin spacer comes back on the wide cell after it; a margin
//   spacer comes back in the style of that wide cell, which may have
//   been overwritten in another since; rows lent to history by an
//   earlier resize come back; a line too long for one history record,
//   split across new ones, breaks at their seams).
//
// This is free and unencumbered software released into the public domain.
#define ARIEL_TEST
#include "../ariel.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#  include <malloc.h>
#  define alignedalloc(n)  _aligned_malloc(n, 16)
#  define alignedfree(p)   _aligned_free(p)
#  define binarystdin()    _setmode(0, _O_BINARY)
#else
#  define alignedalloc(n)  aligned_alloc(16, n)
#  define alignedfree(p)   free(p)
#  define binarystdin()    ((void)0)
#endif

#ifndef __AFL_FUZZ_TESTCASE_LEN
static unsigned char afl_buf[1<<20];
static ssize_t       afl_len;
#  define __AFL_FUZZ_TESTCASE_LEN  afl_len
#  define __AFL_FUZZ_TESTCASE_BUF  afl_buf
#  define __AFL_FUZZ_INIT()        void ariel_fuzz_init(void)
#  define __AFL_INIT()             afl_len = read(0, afl_buf, sizeof(afl_buf))
#  define __AFL_LOOP(n)            afl_loop()
static int afl_loop(void) { static int n; return !n++; }
#endif

__AFL_FUZZ_INIT();

// -DFUZZ_TRACE prints the steps and the failing states
#ifdef FUZZ_TRACE
#  define trace(...)  fprintf(stderr, __VA_ARGS__)
#else
#  define trace(...)  ((void)0)
#endif

static void checkhistory(ariel_Term *t);
static void check(ariel_Term *t)
{
    checkhistory(t);
    affirm(t->c.x>=0 && t->c.x<t->cols);
    affirm(t->c.y>=0 && t->c.y<t->rows);
    for (i32 i = 0; i < 2; i++) {
        Cursor *c = t->saved + i;
        affirm(c->x>=0 && c->x<t->cols);
        affirm(c->y>=0 && c->y<t->rows);
        affirm(!c->wrapnext || c->x==t->cols-1);
    }
    affirm(t->top>=0 && t->top<=t->bot && t->bot<t->rows);
    static u8 owned[MAXDIM*MAXDIM/4];   // no two cells share a pool slot
    memset(owned, 0, (size_t)t->poolused);
    for (i32 g = 0; g < 2; g++) {
        // The map is a permutation of the slots
        static u8 seen[MAXDIM];
        memset(seen, 0, sizeof(seen));
        u16 *map = gridmap(t, g);
        for (i32 i = 0; i < t->caprows; i++) {
            affirm(map[i] < t->caprows && !seen[map[i]]);
            seen[map[i]] = 1;
        }
        for (i32 y = 0; y < t->rows; y++) {
            Cell *r = gridcells(t, g) + (iz)map[y]*t->capcols;
            for (i32 x = 0; x < t->cols; x++) {
                i32 k = cellkind(r+x);
                if (k == KIND_WIDE) {
                    affirm(x+1 < t->cols && cellkind(r+x+1)==KIND_TAIL);
                } else if (k == KIND_TAIL) {
                    affirm(x>0 && cellkind(r+x-1)==KIND_WIDE);
                }
                if (r[x].ext >= EXT_POOL) {
                    u32 slot = r[x].ext - EXT_POOL;
                    affirm(slot < (u32)t->poolused && !owned[slot]);
                    owned[slot] = 1;
                }
                affirm(cellcp(r+x) < 0x110000);
            }
        }
    }
}

static void checkhistory(ariel_Term *t)
{
    affirm(t->hcount>=0 && t->hcount<=t->histlines);
    affirm(!t->hcount || histon(t));
    affirm(!t->hopen || t->hcount);
    affirm(!t->scr || !t->viewoff);
    hsync(t);
    i64 rows = 0;
    u32 prev = 0;
    i32 wraps = 0;
    for (i32 i = 0; i < t->hcount; i++) {
        Hent *e = hent(t, i);
        affirm(e->len >= 1);
        affirm(e->off+e->len <= (u32)t->histbytes);
        if (i) {
            // Cyclic order: offsets rise, wrapping around once at most
            if (e->off < prev) wraps++;
            affirm(wraps <= 1);
        }
        prev = e->off + e->len;
        // Runs are well formed and add up to the width
        u8 *rec   = hbytes(t, e->off);
        u32 width = 0;
        b32 wide  = 0;
        for (i32 pos = 0; pos < (i32)e->len;) {
            affirm(pos+5 <= (i32)e->len);
            Hpos h = hrunat(rec, pos);
            affirm(h.runend <= (i32)e->len);
            i32   n = 0;
            Hcell c;
            Hpos  q = h;
            while (q.pos < h.runend && hnext(rec, h.runend, &q, &c)) {
                width += (u32)c.width;
                wide  |= c.width == 2;
                // RUN_WIDE may outlive a wide cell cut off, not the reverse
                affirm(c.width<2 || h.mask&RUN_WIDE);
                n++;
            }
            affirm(q.pos == h.runend && n == h.ncells);
            pos = h.runend;
        }
        affirm(width == e->width);
        affirm(!wide || e->flags&HR_WIDE);
        Wrap w = hwrap(t, e, t->hcols);
        affirm(hrowsof(t, i) == w.rows);
        rows += w.rows;
    }
    if (t->hcount) {
        Hent *e = hent(t, t->hcount-1);
        affirm(t->hw == e->off+e->len);
        affirm(!(e->flags & HR_JOIN));  // the newest continues in none
        // The records are apart: the oldest lies past the newest's end,
        // or wholly before it when the ring hasn't wrapped
        Hent *o = hent(t, 0);
        affirm(t->hcount==1 || wraps==0 || o->off>=t->hw);
    }
    affirm(rows == t->hrows);
    affirm(t->viewoff>=0 && t->viewoff<=t->hrows);
}

// History and the main screen as text, rows joined into lines, erased
// cells ending a line and blank lines at the end dropped. With cur, cur[0] gets the main screen
// cell (y, x)'s offset in the text, clamped to its line, and cur[1] how
// far past the line's end it was. *top, if given, gets the offset of the
// main screen's text.
static iz logical(ariel_Term *t, u8 *out, iz cap, i32 y, i32 x, iz *cur,
                  iz *top)
{
    hsync(t);
    iz  n = 0, textend = 0, spacerat = -1, wideat = 0;
    i64 total = t->hrows + t->rows;
    for (i64 v = 0; v < total; v++) {
        Cell *r;
        u32   flags;
        i32   end = t->cols;
        if (v==t->hrows && top) *top = n;
        if (v < t->hrows) {
            Vrow vr = vcells(t, v, 0);
            r     = vr.cells;
            flags = vr.flags;
            end   = vr.end;
        } else {
            i32 sy = (i32)(v - t->hrows);
            r     = gridcells(t, 0) + (iz)gridmap(t, 0)[sy]*t->capcols;
            flags = gridheads(t, 0)[gridmap(t, 0)[sy]].flags;
        }
        // A margin spacer before a wide cell is no text, and a cursor on
        // it or waiting to wrap past it is at the wide cell
        if (spacerat>=0 && cellkind(r)==KIND_WIDE) {
            n = spacerat;
            if (cur && cur[0]>n) cur[0] = n;
        }
        spacerat = -1;
        for (i32 i = 0; i < end; i++) {
            Cell *c = r + i;
            if (v-t->hrows==y && i==x && cur) {
                cur[0] = cellkind(c)==KIND_TAIL ? wideat : n;
            }
            if (cellkind(c) == KIND_TAIL) continue;
            wideat = n;
            if (i==t->cols-1 && cellkind(c)==KIND_SPACER && flags&ARIEL_R_WRAPPED) {
                spacerat = n;
            }
            u32 cp = erased(c) ? ' ' : cellcp(c);
            if (n+4 < cap) n += pututf8(out+n, cp);
            if (!erased(c)) textend = n;
        }
        if (v-t->hrows==y && x>=t->cols && cur) cur[0] = n;
        if (!(flags & ARIEL_R_WRAPPED) || v==total-1) {
            n = textend;    // erased cells at the end are no text
            if (cur && cur[0]>n) {
                cur[1] = cur[0] - n;
                cur[0] = n;
            }
            if (n < cap) out[n++] = '\n';
            textend  = n;
            spacerat = -1;
        }
    }
    spareend(t);
    for (; n && out[n-1]=='\n'; n--) {}
    return n;
}

// Visible cells that hold a pool slot, each its own (see check)
static i32 poolrefs(ariel_Term *t)
{
    i32 n = 0;
    for (i32 g = 0; g < 2; g++) {
        u16 *map = gridmap(t, g);
        for (i32 y = 0; y < t->rows; y++) {
            Cell *r = gridcells(t, g) + (iz)map[y]*t->capcols;
            for (i32 x = 0; x < t->cols; x++) {
                n += r[x].ext >= EXT_POOL;
            }
        }
    }
    return n;
}

static ariel_Term *newterm(ariel_Config *cfg, iz size)
{
    void *mem = alignedalloc((size_t)size);
    memset(mem, 0xa5, (size_t)size);
    ariel_Term *t = ariel_init(mem, size, cfg);
    affirm(t);
    return t;
}

static void feedall(ariel_Term *t, u8 *p, iz n)
{
    ariel_Str in = {p, n};
    for (;;) {
        ariel_Fed f = ariel_feed(t, in);
        if (!f.ev.kind) break;
        in = f.rest;
    }
}

// Feed it all, holding replies to canonical bytes
static void feedcheck(ariel_Term *t, u8 *p, iz n)
{
    ariel_Str in = {p, n};
    for (;;) {
        ariel_Fed f = ariel_feed(t, in);
        if (!f.ev.kind) break;
        if (f.ev.kind == ARIEL_EV_REPLY) {
            ariel_Str r = f.ev.data;
            affirm(r.len>0 && r.data[0]==0x1b);
            for (iz i = 0; i < r.len; i++) {
                u8 b = r.data[i];
                affirm(b==0x1b || b==0x07 || (b>=0x20 && b<0x7f));
            }
        }
        in = f.rest;
    }
}

// The cursors reflow carries: the main screen's cursor, and its saved
// cursor when valid
typedef struct {
    Cursor *c[2];
} Anchors;

static Anchors anchorsof(ariel_Term *t)
{
    Anchors r = {0};
    i32     n = 0;
    if (!t->scr) r.c[n++] = &t->c;
    if (t->saved[0].valid) r.c[n++] = t->saved + 0;
    return r;
}

typedef struct {
    u8 *text;
    iz  len;
    iz  cur[2][2];
    iz  top;        // offset of the screen's text
    u64 serial;
    i32 prompts;    // marks on main screen rows and history lines
    i32 droppable;  // of them, on rows below the first anchor
    b32 mid;        // a mark inside a line, which may merge
    i32 joins;      // records a line continues past (HR_JOIN)
} Snap;

static b32 midprompt(ariel_Term *);

static Snap snapof(ariel_Term *t)
{
    Snap s = {0};
    hsync(t);
    iz cap = (iz)(t->hrows + t->rows)*(t->cols*4 + 1) + 16;
    s.text   = malloc((size_t)cap);
    s.serial = t->hserial;
    s.len    = logical(t, s.text, cap, -1, 0, 0, &s.top);
    Anchors a = anchorsof(t);
    for (i32 i = 0; i < t->hcount; i++) {
        s.prompts += !!(hent(t, i)->flags & HR_PROMPT);
        s.joins   += !!(hent(t, i)->flags & HR_JOIN);
    }
    for (i32 y = 0; y < t->rows; y++) {
        i32 r = gridmap(t, 0)[y];
        if (!(gridheads(t, 0)[r].flags & ARIEL_R_PROMPT)) continue;
        s.prompts++;
        s.droppable += !a.c[0] || y>a.c[0]->y;
    }
    s.mid = midprompt(t);
    for (i32 i = 0; i < 2; i++) {
        s.cur[i][0] = -1;
        if (a.c[i]) {
            Cursor *c = a.c[i];
            logical(t, s.text, cap, c->y, c->x+c->wrapnext, s.cur[i], 0);
            affirm(s.cur[i][0] >= 0);
        }
    }
    return s;
}

// After a resize from before: no text lost unless history evicted or
// rows dropped from the bottom, and the cursors stay on their text.
// True if nothing was lost, not even a cursor's column past its text.
static b32 reflowcheck(ariel_Term *t, Snap *before)
{
    Snap after = snapof(t);
    b32  same  = 0;
    trace("before %td [%.*s] cur %td %td\n", before->len, (int)before->len,
          before->text, before->cur[0][0], before->cur[1][0]);
    trace("after  %td [%.*s] cur %td %td\n", after.len, (int)after.len,
          after.text, after.cur[0][0], after.cur[1][0]);
    if (after.serial == before->serial) {
        affirm(after.len <= before->len);
        affirm(!memcmp(after.text, before->text, (size_t)after.len));
        // Prompt marks go only with rows dropped below the cursor
        affirm(after.prompts <= before->prompts);
        affirm(before->mid || after.prompts>=before->prompts-before->droppable);
        same = after.len==before->len && after.prompts==before->prompts;
        // A cursor past its line's text may land on the text, one whose
        // text went to history lands on the screen, and the second, if
        // below the first, may have been on rows dropped
        for (i32 i = 0; i < 2; i++) {
            iz *b = before->cur[i], *a = after.cur[i];
            if (b[0] < 0) continue;
            b32 sure = !i || b[0]<=before->cur[0][0];
            // Its line the last text left, cut short past erased cells
            b32 cut  = a[1] && a[0]>=after.len && a[0]+a[1]==b[0] &&
                       after.len<before->len;
            for (iz j = a[0]; cut && j<=b[0] && j<before->len; j++) {
                cut = before->text[j] == ' ';
            }
            affirm(!sure || b[1] || a[0]==b[0] || cut || b[0]<after.top);
            same &= a[0]==b[0] && a[1]==b[1];
        }
    }
    free(after.text);
    return same;
}

// A prompt mark off its line's first row, which reflow moves there,
// merging with any mark already there: on a row after a wrapped one, or
// on the first row continuing a marked line in history (the wrapped row
// above it scrolled there)
static b32 midprompt(ariel_Term *t)
{
    Head *h = gridheads(t, 0);
    u16  *m = gridmap(t, 0);
    if (hcontinues(t) && h[m[0]].flags&ARIEL_R_PROMPT &&
        hent(t, hlinefirst(t))->flags&HR_PROMPT) {
        return 1;
    }
    for (i32 y = 1; y < t->rows; y++) {
        if (h[m[y-1]].flags&ARIEL_R_WRAPPED && h[m[y]].flags&ARIEL_R_PROMPT) {
            return 1;
        }
    }
    return 0;
}

// A cursor between two rows of a line: at the start of the second, or
// waiting to wrap at the end of the first, or on a margin spacer. Wider,
// they're one place.
static b32 rowstart(ariel_Term *t)
{
    Anchors a = anchorsof(t);
    Head   *h = gridheads(t, 0);
    u16    *m = gridmap(t, 0);
    for (i32 i = 0; i < 2; i++) {
        Cursor *c = a.c[i];
        if (c && c->y && !c->x && !c->wrapnext &&
            h[m[c->y-1]].flags&ARIEL_R_WRAPPED) {
            return 1;
        }
        if (c && c->wrapnext && h[m[c->y]].flags&ARIEL_R_WRAPPED) {
            return 1;
        }
        Cell *r = c ? gridcells(t, 0) + (iz)m[c->y]*t->capcols : 0;
        if (c && cellkind(r+c->x)==KIND_SPACER) {
            return 1;   // the wide cell it stands for, on the next row
        }
    }
    return 0;
}

// A line with a styled fill, which a narrower screen loses when the text
// fills the last row, or erased cells of two styles at its end, the
// inner ones reading as the fill once at the margin
static b32 fills(ariel_Term *t)
{
    for (i32 y = 0; y < t->rows; y++) {
        Head *h = gridheads(t, 0) + gridmap(t, 0)[y];
        Cell *r = gridcells(t, 0) + (iz)gridmap(t, 0)[y]*t->capcols;
        if (h->flags & ARIEL_R_WRAPPED) continue;
        Trim tr = trimrow(r, t->cols);
        Pen  z  = {0};
        if (tr.n && erased(r+tr.n-1)) return 1;
        if (tr.n<t->cols && memcmp(&tr.fill, &z, sizeof(z))) return 1;
        // Erased cells count on a wrapped row, not on the line's last
        if (!tr.n && y && gridheads(t, 0)[gridmap(t, 0)[y-1]].flags&ARIEL_R_WRAPPED) {
            return 1;
        }
    }
    return 0;
}

// A margin spacer styled unlike the wide cell after it, which was
// overwritten since: reflow drops the spacer and redraws it in the wide
// cell's style
static b32 spacers(ariel_Term *t)
{
    Head *h = gridheads(t, 0);
    u16  *m = gridmap(t, 0);
    for (i32 y = 0; y+1 < t->rows; y++) {
        Cell *r = gridcells(t, 0) + (iz)m[y]*t->capcols + t->cols-1;
        Cell *n = gridcells(t, 0) + (iz)m[y+1]*t->capcols;
        if (h[m[y]].flags&ARIEL_R_WRAPPED && cellkind(r)==KIND_SPACER &&
            cellkind(n)==KIND_WIDE &&
            (r->fg!=n->fg || r->bg!=n->bg || r->ul!=n->ul ||
             r->attr!=n->attr || r->link!=n->link)) {
            return 1;
        }
    }
    return 0;
}

static u8 *dumpflags(ariel_Term *t, iz *len, int flags)
{
    *len = ariel_dump(t, flags, 0, 0);
    u8 *d = malloc((size_t)*len + 1);
    ariel_dump(t, flags, d, *len);
    return d;
}

static u8 *dumpof(ariel_Term *t, iz *len)
{
    int flags = ARIEL_DUMP_ATTRS | ARIEL_DUMP_STATE | ARIEL_DUMP_HISTORY;
    return dumpflags(t, len, flags);
}

// The screens and state without history, which a dump decodes through
// the cluster pool: with a smaller pool, the decoding itself degrades
static u8 *screenof(ariel_Term *t, iz *len)
{
    return dumpflags(t, len, ARIEL_DUMP_ATTRS | ARIEL_DUMP_STATE);
}

// Everything a host would read
static void render(ariel_Term *t)
{
    static ariel_Cell cells[MAXDIM];
    ariel_gen(t);
    ariel_Modes m = ariel_modes(t);
    for (i32 y = 0; y < m.rows; y++) {
        i32 n = ariel_row(t, y, cells, countof(cells));
        ariel_rowinfo(t, y);
        for (i32 x = 0; x < n; x++) {
            if (cells[x].extra) {
                u32 cps[16];
                affirm(ariel_cluster(t, x, y, cps, countof(cps)) == 1+cells[x].extra);
            }
        }
    }
    ariel_cursor(t);
    ariel_title(t, 0);
}

static void encode(ariel_Term *t, u8 b)
{
    ariel_Key k = {0};
    k.key  = b % 80;
    k.mods = b >> 4;
    k.cp   = k.key ? 0 : 'a' + b%26;
    ariel_key(t, &k);
    ariel_Mouse m = {0};
    m.action = 1 + b%4;
    m.button = b % 12;
    m.x = b % 300;
    m.y = b / 3;
    m.mods = b >> 5;
    ariel_mouse(t, &m);
    ariel_focus(t, b & 1);
}

// The host's other calls: colours, flags, pastes, clipboard, theme
static void host(ariel_Term *t, u8 op, u8 arg)
{
    static u8 paste[64];
    switch (op % 8) {
    case 0:
        ariel_setcolor(t, (op*7 + arg) % (ARIEL_C_COUNT+2) - 1, arg*0x10101u);
        break;
    case 1:
        ariel_setflags(t, arg);
        break;
    case 2: {
        for (int i = 0; i < 16; i++) paste[i] = (u8)(arg*31 + i*op);
        ariel_Str in = {paste, 16};
        u8 out[64];
        ariel_paste(t, out, sizeof(out), in, 1 + arg%4);
        ariel_pastecheck(t, in);
        ariel_text(t, in);
    } break;
    case 3: {
        ariel_Str in = {paste, arg%48};
        ariel_clipreply(t, 'a' + arg%30, in);
    } break;
    case 4:
        ariel_scheme(t, arg & 1);
        break;
    case 5:
        ariel_reset(t, arg & 6);
        break;
    case 6:
        ariel_title(t, arg & 1);
        ariel_color(t, (arg*3) % (ARIEL_C_COUNT+2) - 1);
        break;
    case 7: {
        // The viewport, selection and copy
        static u8 out[1<<16];
        if (arg & 1) ariel_scroll(t, (int8_t)arg/4);
        if (arg & 2) ariel_scrollto(t, (op>>3) % 5);
        ariel_Modes m = ariel_modes(t);
        i32 sop = (1 + (op>>3)%6) | (op&0x80 ? ARIEL_SEL_BLOCK : 0);
        ariel_select(t, sop, arg%(m.cols+2) - 1, (arg>>2)%(m.rows+2) - 1);
        iz need = ariel_copy(t, 0, 0);
        affirm(need >= 0);
        if (need <= (iz)sizeof(out)) {
            affirm(ariel_copy(t, out, sizeof(out)) == need);
        }
        ariel_View v = ariel_view(t);
        affirm(v.offset>=0 && v.offset<=v.total-m.rows);
    } break;
    }
}

// Export round trip: a fresh terminal fed the export equals the original,
// erased cells compared by background, history by logical line as drawn
// at the screen's width

#define EQ(c)  do { if (!(c)) { trace("termeq: %s\n", #c); return 0; } } while (0)

static b32 peneq(Pen *a, Pen *b)
{
    return a->fg==b->fg && a->bg==b->bg && a->ul==b->ul &&
           a->attr==b->attr && a->link==b->link;
}

static b32 celleq(ariel_Term *a, Cell *x, ariel_Term *b, Cell *y)
{
    b32 ex = eerased(x), ey = eerased(y);
    if (ex || ey) return ex==ey && x->bg==y->bg;
    if (x->cp!=y->cp || x->fg!=y->fg || x->bg!=y->bg || x->ul!=y->ul ||
        x->attr!=y->attr || x->link!=y->link) {
        return 0;
    }
    u32 xa[POOLN], xb[POOLN];
    i32 na = cellextra(a, x, xa, POOLN);
    i32 nb = cellextra(b, y, xb, POOLN);
    return na==nb && !memcmp(xa, xb, sizeof(u32)*(size_t)na);
}

static b32 cursoreq(Cursor *a, Cursor *b)
{
    return a->x==b->x && a->y==b->y && a->wrapnext==b->wrapnext &&
           peneq(&a->pen, &b->pen) && !memcmp(a->g, b->g, 4) &&
           a->gl==b->gl && a->ss==b->ss;
}

// History lines, oldest first, each a u32 string: per cell its code
// point and kind, colors, attributes and extras, erased cells their
// background alone, and the last row filled out with the line's fill
typedef struct { u32 *v; iz len; b32 prompt; } Hline;

static iz hlinesof(ariel_Term *t, Hline **out)
{
    iz     n     = 0;
    Hline *lines = calloc((size_t)t->hcount + 1, sizeof(*lines));
    for (i32 i = 0; i < t->hcount; n++) {
        Hline *l   = lines + n;
        iz     cap = 64;
        l->v = malloc(sizeof(u32)*(size_t)cap);
        Wrap w = {1, 0};
        Pen  fill = {0};
        for (;; i++) {
            Hent *e   = hent(t, i);
            u8   *rec = hbytes(t, e->off);
            Hpos  h   = {0};
            Hcell c;
            l->prompt |= !!(e->flags & HR_PROMPT);
            while (hnext(rec, (i32)e->len, &h, &c)) {
                if (l->len+6+POOLN > cap) {
                    cap *= 2;
                    l->v = realloc(l->v, sizeof(u32)*(size_t)cap);
                }
                u32 *v = l->v + l->len;
                if (eerased(&c.c)) {
                    v[0] = 0; v[1] = 0; v[2] = c.c.bg; v[3] = 0; v[4] = 0; v[5] = 0;
                    l->len += 6;
                } else {
                    v[0] = c.c.cp; v[1] = c.c.fg; v[2] = c.c.bg; v[3] = c.c.ul;
                    v[4] = c.c.attr;
                    v[5] = (u32)imin(hextras(rec, &c, v+6, POOLN), POOLN);
                    l->len += 6 + v[5];
                }
                wrapcell(&w, c.width, t->cols);
            }
            fill = h.ncells ? (Pen){0} : h.pen;
            if (!(e->flags&HR_JOIN) || i+1==t->hcount) {
                i++;
                break;
            }
        }
        if (i<t->hcount || !hcontinues(t)) {
            for (i32 x = w.col; x < t->cols; x++) {
                if (l->len+6 > cap) {
                    cap *= 2;
                    l->v = realloc(l->v, sizeof(u32)*(size_t)cap);
                }
                u32 *v = l->v + l->len;
                v[0] = 0; v[1] = 0; v[2] = fill.bg; v[3] = 0; v[4] = 0; v[5] = 0;
                l->len += 6;
            }
        }
    }
    *out = lines;
    return n;
}

static void freelines(Hline *l, iz n)
{
    for (iz i = 0; i < n; i++) free(l[i].v);
    free(l);
}

static b32 historyeq(ariel_Term *a, ariel_Term *b)
{
    Hline *la, *lb;
    iz na = hlinesof(a, &la);
    iz nb = hlinesof(b, &lb);
    b32 evicted = b->hserial > 0;
    iz  m = na<nb ? na : nb;
    if (evicted && m) m--;      // its oldest line may be cut short
    b32 ok = evicted ? nb<=na : na==nb;
    if (!ok) trace("termeq: %td history lines, %td back\n", na, nb);
    for (iz i = 1; ok && i <= m; i++) {
        Hline *x = la + na - i, *y = lb + nb - i;
        ok = x->len==y->len && x->prompt==y->prompt &&
             !memcmp(x->v, y->v, sizeof(u32)*(size_t)x->len);
        if (!ok) {
            trace("termeq: history line %td from the newest\n", i);
            trace("  prompt %d %d, len %td %td\n", x->prompt, y->prompt, x->len, y->len);
            for (iz k = 0; k < x->len || k < y->len; k++) {
                trace("  %td: %x %x\n", k, k<x->len ? x->v[k] : 0, k<y->len ? y->v[k] : 0);
            }
        }
    }
    freelines(la, na);
    freelines(lb, nb);
    return ok;
}

static b32 termeq(ariel_Term *a, ariel_Term *b)
{
    EQ(a->cols==b->cols && a->rows==b->rows && a->scr==b->scr);
    for (i32 g = 0; g < 2; g++) {
        for (i32 y = 0; y < a->rows; y++) {
            u32 mask = ARIEL_R_WRAPPED | ARIEL_R_PROMPT;
            if ((ghead(a, g, y)->flags&mask) != (ghead(b, g, y)->flags&mask)) {
                trace("termeq: grid %d row %d flags %x, back %x\n", g, y,
                      ghead(a, g, y)->flags&mask, ghead(b, g, y)->flags&mask);
                return 0;
            }
            Cell *ra = grow(a, g, y), *rb = grow(b, g, y);
            for (i32 x = 0; x < a->cols; x++) {
                if (!celleq(a, ra+x, b, rb+x)) {
                    trace("termeq: grid %d cell %d,%d\n", g, y, x);
                    return 0;
                }
            }
        }
    }
    EQ(cursoreq(&a->c, &b->c));
    EQ(a->origin == b->origin);
    for (i32 i = 0; i < 2; i++) {
        Cursor *x = a->saved + i, *y = b->saved + i;
        EQ(x->valid == y->valid);
        EQ(!x->valid || (cursoreq(x, y) && x->origin==y->origin));
    }
    EQ(a->top==b->top && a->bot==b->bot);
    for (i32 x = 0; x < a->cols; x++) EQ(istab(a, x) == istab(b, x));
    EQ(a->appcursor==b->appcursor && a->reverse==b->reverse);
    EQ(a->autowrap==b->autowrap && a->cursorblink==b->cursorblink);
    EQ(a->cursorvisible==b->cursorvisible && a->insert==b->insert);
    EQ(a->newline==b->newline && a->appkeypad==b->appkeypad);
    EQ(a->backarrow==b->backarrow && a->focus==b->focus);
    EQ(a->altscroll==b->altscroll && a->paste==b->paste && a->sync==b->sync);
    EQ(a->grapheme==b->grapheme && a->themereport==b->themereport);
    EQ(a->inband==b->inband);
    EQ(a->mouse==b->mouse && a->mouseenc==b->mouseenc && a->modkeys==b->modkeys);
    for (i32 s = 0; s < 2; s++) {
        EQ(a->nkitty[s] == b->nkitty[s]);
        EQ(!memcmp(a->kitty[s], b->kitty[s], (size_t)a->nkitty[s]));
    }
    EQ(a->nxtsaved == b->nxtsaved);
    EQ(!memcmp(a->xtsaved, b->xtsaved, sizeof(a->xtsaved[0])*(size_t)a->nxtsaved));
    for (i32 k = 0; k < 2; k++) {
        EQ(a->titlelen[k] == b->titlelen[k]);
        EQ(!memcmp(a->title[k], b->title[k], (size_t)a->titlelen[k]));
    }
    EQ(a->ntstack == b->ntstack);
    for (i32 i = 0; i < a->ntstack; i++) {
        for (i32 k = 0; k < 2; k++) {
            i32 n = a->tstacklen[i][k];
            EQ(n == b->tstacklen[i][k]);
            EQ(n<=0 || !memcmp(a->tstack[i][k], b->tstack[i][k], (size_t)n));
        }
    }
    EQ(!memcmp(a->pal, b->pal, sizeof(a->pal)));
    EQ(a->cursorshape == b->cursorshape);
    EQ((a->segy<0) == (b->segy<0) && a->lastcp==b->lastcp);
    EQ(a->segy<0 || (a->segx==b->segx && a->segy==b->segy && segeq(&a->seg, &b->seg)));
    EQ(hcontinues(a) == hcontinues(b));
    EQ(historyeq(a, b));
    return 1;
}

// Unless the export says it's lossy, it rebuilds the terminal
static void exportcheck(ariel_Term *a, ariel_Config *cfg)
{
    if (a->state!=ST_GROUND || a->uneed) return;    // mid-sequence
    Exported x = exportstate(a, a->hcount, 0, 0);
    if (x.lossy) return;
    u8 *buf = malloc((size_t)x.len + 1);
    affirm(exportstate(a, a->hcount, buf, x.len).len == x.len);
    ariel_Config c = *cfg;
    c.cols    = a->cols;
    c.rows    = a->rows;
    c.flags   = a->flags;
    // The replay holds history rows on screen at once, whose clusters
    // the original kept in the ring: give it a slot for every cell of
    // both screens, or a full pool would drop extras
    c.capcols = a->cols;
    c.caprows = imin(8*a->rows + 4, MAXDIM);
    ariel_Term *b = newterm(&c, ariel_size(&c));
    memcpy(b->paldef, a->paldef, sizeof(b->paldef));
    memcpy(b->pal,    a->paldef, sizeof(b->pal));
    feedall(b, buf, x.len);
#ifdef FUZZ_TRACE
    if (!termeq(a, b)) {
        iz  al, bl;
        u8 *ad = dumpof(a, &al);
        u8 *bd = dumpof(b, &bl);
        trace("export:\n%.*s\noriginal:\n%.*s\nrebuilt:\n%.*s\n",
              (int)x.len, buf, (int)al, ad, (int)bl, bd);
        free(ad);
        free(bd);
    }
#endif
    affirm(termeq(a, b));
    free(buf);
    alignedfree(b);
}

int main(void)
{
    binarystdin();
    __AFL_INIT();
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        iz len = __AFL_FUZZ_TESTCASE_LEN;
        if (len < 4) continue;
        ariel_Config cfg = {0};
        cfg.cols    = 2 + buf[0]%100;
        cfg.rows    = 1 + buf[1]%40;
        cfg.capcols = cfg.cols + buf[2]%8;
        cfg.caprows = cfg.rows + buf[2]/32;
        cfg.flags   = buf[3] & 0x1f;
        cfg.histlines = 100;
        cfg.histbytes = 1<<14;
        i32 split = 1 + (buf[3]>>5)*7;
        iz  size  = ariel_size(&cfg);
        u8 *data  = buf + 4;
        iz  n     = len - 4;
        iz  every = 1 + n/split/32;     // about 32 checks per pass

        // Split invariance: pieces and the whole give the same terminal
        ariel_Term *a = newterm(&cfg, size);
        ariel_Term *b = newterm(&cfg, size);
        for (iz i = 0, k = 0; i < n; i += split, k++) {
            feedall(a, data+i, n-i < split ? n-i : split);
            if (!(k % every)) check(a);
        }
        check(a);
        feedall(b, data, n);
        iz alen, blen;
        u8 *ad = dumpof(a, &alen);
        u8 *bd = dumpof(b, &blen);
        affirm(alen==blen && !memcmp(ad, bd, (size_t)alen));
        free(ad);
        free(bd);
        alignedfree(b);

        // Interleaved operations, with a memcpy'd fork driven the same
        ariel_reset(a, ARIEL_RESET_FULL);
        ariel_Term *f   = 0;
        u32         gen = ariel_gen(a);
        for (iz i = 0, k = 0; i < n; i += split, k++) {
            iz m = n-i < split ? n-i : split;
            feedcheck(a, data+i, m);
            if (f) feedall(f, data+i, m);
            u8 op  = data[i];
            u8 arg = data[n-1-i];
            switch (k % 7) {
            case 1: {
                i32 cols = 2 + op%cfg.capcols;
                i32 rows = 1 + arg%cfg.caprows;
                i32 oc   = a->cols;
                i32 or   = a->rows;
                if (op & 1) rows = or;  // width alone round-trips
                check(a);
                Snap s    = snapof(a);
                b32  open = hcontinues(a) || midprompt(a) || rowstart(a) ||
                            fills(a) || hlentrows(a) || spacers(a);
                iz   blen;
                u8  *bd = dumpof(a, &blen);
                trace("resize %dx%d -> %dx%d\n", oc, or, cols, rows);
                if (ariel_resize(a, cols, rows, 8, 16)) {
#ifdef FUZZ_TRACE
                    iz  tl;
                    u8 *td = dumpof(a, &tl);
                    trace("before:\n%.*s\nafter:\n%.*s\n", (int)blen, bd, (int)tl, td);
                    free(td);
#endif
                    b32 same = reflowcheck(a, &s);
                    if (same && !a->scr && rows==or && !open) {
                        // There and back again changes nothing
                        ariel_Term *r = newterm(&cfg, size);
                        memcpy(r, a, (size_t)size);
                        Snap rs = snapof(r);
                        ariel_resize(r, oc, or, 8, 16);
                        Snap back = snapof(r);
                        free(back.text);
                        // A line split across new records breaks at the
                        // seams, each record wrapped on its own
                        if (reflowcheck(r, &rs) && back.joins==s.joins) {
                            iz  rlen;
                            u8 *rd = dumpof(r, &rlen);
                            trace("there:\n%.*s\nback:\n%.*s\n",
                                  (int)blen, bd, (int)rlen, rd);
                            affirm(rlen==blen && !memcmp(rd, bd, (size_t)rlen));
                            free(rd);
                        }
                        check(r);
                        free(rs.text);
                        alignedfree(r);
                    }
                }
                free(bd);
                free(s.text);
                if (f) ariel_resize(f, cols, rows, 8, 16);
            } break;
            case 2:
                encode(a, op);
                if (f) encode(f, op);
                break;
            case 3:
                if (!(k % every)) {
                    render(a);
                    if (f) render(f);
                }
                break;
            case 4:
                if (!f && (op & 1)) {
                    f = newterm(&cfg, size);
                    memcpy(f, a, (size_t)size);
                } else if (!(op & 6)) {
                    exportcheck(a, &cfg);
                }
                break;
            case 5:
                if (!(op & 3)) {
                    ariel_Config c = cfg;
                    c.capcols = imin(cfg.cols + arg%16, MAXDIM);
                    c.caprows = imin(cfg.rows + arg/16, MAXDIM);
                    ariel_Modes md = ariel_modes(a);
                    c.capcols = imax(c.capcols, md.cols);
                    c.caprows = imax(c.caprows, md.rows);
                    if (arg & 1) {
                        c.histlines = arg%120;
                        c.histbytes = (iz)1 << (10 + arg%7);
                    }
                    i32 hcount = a->hcount;
                    iz  msize = ariel_size(&c);
                    u8 *mem   = alignedalloc((size_t)msize);
                    memset(mem, 0x5a, (size_t)msize);
                    iz  blen, alen;
                    u8 *bd = screenof(a, &blen);
                    i32 live = poolrefs(a);
                    ariel_Term *moved = ariel_move(a, mem, msize, &c);
                    affirm(moved);
                    i32 used = moved->poolused;
                    u8 *ad = screenof(moved, &alen);
                    b32 fits = live <= moved->npool;  // else clusters degrade
                    b32 kept = moved->hcount == hcount;
                    affirm(!fits || (alen==blen && !memcmp(ad, bd, (size_t)alen)));
                    affirm(!kept || historyeq(a, moved));
                    affirm(used == imin(live, moved->npool));
                    free(ad);
                    free(bd);
                    alignedfree(a);
                    a    = moved;
                    cfg  = c;
                    size = msize;
                    check(a);
                    if (f) {
                        // The fork matched until now; it can't follow a move
                        bd = screenof(f, &blen);
                        ad = screenof(a, &alen);
                        affirm(!fits || (alen==blen && !memcmp(ad, bd, (size_t)alen)));
                        affirm(!kept || historyeq(f, a));
                        free(ad);
                        free(bd);
                        alignedfree(f);
                        f = 0;
                    }
                }
                break;
            case 6:
                host(a, op, arg);
                if (f) host(f, op, arg);
                break;
            }
            if (!(k % every)) {
                check(a);
                u32 g = ariel_gen(a);
                affirm(g >= gen);
                gen = g;
                if (f) ariel_gen(f);
            }
        }
        check(a);
        exportcheck(a, &cfg);
        render(a);
        if (f) {
            render(f);
            iz  alen, flen;
            u8 *ad = dumpof(a, &alen);
            u8 *fd = dumpof(f, &flen);
            affirm(alen==flen && !memcmp(ad, fd, (size_t)alen));
            free(ad);
            free(fd);
            alignedfree(f);
        }
        alignedfree(a);
    }
    return 0;
}
