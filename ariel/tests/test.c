// ariel tests: unit tests and *.test scripts
//
// $ cc -std=c11 -DARIEL_TEST -g3 -fsanitize=address,undefined -o test test.c
// $ ./test grapheme.txt emoji.txt *.test
//
// Arguments name the data files and scripts. grapheme.txt and emoji.txt
// are Unicode's test data, which gentables.py derives. Every other file
// is a script: lines of commands, run twice, feeding each PUSH whole and
// then a byte at a time, since a split must never change the result.
//
// Scripts follow libvterm's test language closely enough that its tests
// port with few edits. Leading whitespace doesn't matter; # starts a
// comment; !text names the section reported on failure. Commands:
//
//   INIT [ROWS,COLS]          fresh terminal, 25x80 by default
//   FLAGS NAME...             wideambig legacywidth boldbright noreply
//   RESET                     full reset (RIS)
//   RESIZE ROWS,COLS
//   PUSH "bytes"              C escapes: \e \n \r \t \a \b \0 \xHH \\ \"
//   $REP N: PUSH "bytes"      N times
//   $SEQ A B: PUSH "..\#.."   once per number from A to B, put at \#
//   KEY NAME [MOD...]         encode a key (NAME: UP, F1... or 'c')
//   MOUSE ACTION BTN R,C [MOD...]   encode a mouse event
//   PASTE "text"              encode a paste in one piece
//
// Checks, each against the latest PUSH, KEY, MOUSE or PASTE:
//
//   ?cursor = R,C
//   ?screen_row R = "text"           trailing blanks trimmed
//   ?screen_chars R0,C0,R1,C1 = "text"   half-open; rows joined by \n
//   ?screen_text R0,C0,R1,C1 = 0x41,0x42
//   ?screen_cell R,C = {0x41} width=1 attrs={B,I,U1} fg=rgb(R,G,B) bg=rgb(R,G,B)
//   ?screen_eol R,C = 0|1            nothing but blanks from C on
//   ?title = "text"                  ?icon too
//   ?mode NAME = V                   a field of ariel_Modes
//   ?lineinfo R = wrapped|prompt|-
//   output "bytes"                   the next reply bytes, in order
//   event "text"                     the next non-reply event, as text
//   noevent                          no events left unchecked
//   encoded "bytes"                  the encoder's result
//
// Other lowercase lines are libvterm callback traces, ignored.
//
// This is free and unencumbered software released into the public domain.
#define ARIEL_TEST
#include "../ariel.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <malloc.h>
#  define alignedalloc(n)  _aligned_malloc(n, 16)
#  define alignedfree(p)   _aligned_free(p)
#else
#  define alignedalloc(n)  aligned_alloc(16, n)
#  define alignedfree(p)   free(p)
#endif

static int failures;
static int checks;


// Terminal under test

typedef struct {
    ariel_Term *t;
    void       *mem;
    i32         rows, cols;
    u32         flags;
    u8          replies[1<<16];
    i32         nreplies, rpos;
    char        events[64][256];
    i32         nevents, epos;
    u8          enc[4096];
    i32         nenc;
} Tester;

static void tfree(Tester *s)
{
    if (s->mem) alignedfree(s->mem);
    s->mem = 0;
    s->t   = 0;
}

static void tinit(Tester *s, i32 rows, i32 cols)
{
    tfree(s);
    ariel_Config cfg = {0};
    cfg.cols      = cols;
    cfg.rows      = rows;
    cfg.capcols   = cols < 132 ? 132 : cols;
    cfg.caprows   = rows < 50 ? 50 : rows;
    cfg.histlines = 1000;
    cfg.histbytes = 1<<16;
    cfg.flags     = s->flags;
    iz size = ariel_size(&cfg);
    if (!size) {
        fprintf(stderr, "ariel_size failed\n");
        exit(1);
    }
    s->mem = alignedalloc((size_t)size);
    // Garbage, as an uninitialized block may hold
    memset(s->mem, 0xa5, (size_t)size);
    s->t = ariel_init(s->mem, size, &cfg);
    if (!s->t) {
        fprintf(stderr, "ariel_init failed\n");
        exit(1);
    }
    s->rows = rows;
    s->cols = cols;
}

static void logevent(Tester *s, ariel_Event *e)
{
    if (e->kind == ARIEL_EV_REPLY) {
        iz room = (iz)sizeof(s->replies) - s->nreplies;
        iz n    = e->data.len < room ? e->data.len : room;
        memcpy(s->replies+s->nreplies, e->data.data, (size_t)n);
        s->nreplies += (i32)n;
        return;
    }
    if (s->nevents == countof(s->events)) return;
    char *b = s->events[s->nevents++];
    int   n = (int)(e->data.len < 200 ? e->data.len : 200);
    int   m = (int)(e->more.len < 40 ? e->more.len : 40);
    char *d = (char *)e->data.data;
    char *o = (char *)e->more.data;
    switch (e->kind) {
    case ARIEL_EV_BELL:     snprintf(b, 256, "bell"); break;
    case ARIEL_EV_TITLE:    snprintf(b, 256, "title %d %.*s", e->arg, n, d); break;
    case ARIEL_EV_CWD:      snprintf(b, 256, "cwd %.*s", n, d); break;
    case ARIEL_EV_CLIPSET:  snprintf(b, 256, "clipset %d %d %.*s", e->arg, e->part, n, d); break;
    case ARIEL_EV_CLIPGET:  snprintf(b, 256, "clipget %d", e->arg); break;
    case ARIEL_EV_NOTIFY:   snprintf(b, 256, "notify %.*s|%.*s", n, d, m, o); break;
    case ARIEL_EV_PROGRESS: snprintf(b, 256, "progress %d %d", e->arg, e->value); break;
    case ARIEL_EV_MARK:     snprintf(b, 256, "mark %c %d", e->arg, e->value); break;
    case ARIEL_EV_SYNC:     snprintf(b, 256, "sync %d", e->arg); break;
    default:                snprintf(b, 256, "kind %d", e->kind); break;
    }
}

static void tfeed(Tester *s, u8 *data, iz len, int bytewise)
{
    iz step = bytewise ? 1 : len;
    for (iz i = 0; i < len || (!len && !i); i += step ? step : 1) {
        ariel_Str in = {data+i, len-i < step ? len-i : step};
        if (!len) in.data = 0;
        for (;;) {
            ariel_Fed f = ariel_feed(s->t, in);
            if (f.ev.kind == ARIEL_EV_NONE) {
                if (f.rest.len) {
                    fprintf(stderr, "feed stopped with input left\n");
                    exit(1);
                }
                break;
            }
            logevent(s, &f.ev);
            in = f.rest;
        }
        if (!len) break;
    }
}

static void clearlog(Tester *s)
{
    s->nreplies = s->rpos = 0;
    s->nevents  = s->epos = 0;
}


// Script parsing

typedef struct {
    char *p;
    char *file;
    int   line;
    char  section[128];
    int   bytewise;
} Script;

static void fail(Script *sc, char *fmt, ...)
{
    failures++;
    fprintf(stderr, "%s:%d: [%s%s] ", sc->file, sc->line, sc->section,
            sc->bytewise ? ", bytewise" : "");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void skipws(char **p)
{
    while (**p==' ' || **p=='\t') (*p)++;
}

static int word(char **p, char *w)
{
    skipws(p);
    size_t n = strlen(w);
    if (strncmp(*p, w, n)) return 0;
    char c = (*p)[n];
    if (c && c!=' ' && c!='\t' && c!='\n' && c!='=' && c!=',') return 0;
    *p += n;
    return 1;
}

static long number(char **p)
{
    skipws(p);
    char *e;
    long v = strtol(*p, &e, 0);
    *p = e;
    return v;
}

static int comma(char **p)
{
    skipws(p);
    if (**p != ',') return 0;
    (*p)++;
    return 1;
}

static int expect(char **p, char c)
{
    skipws(p);
    if (**p != c) return 0;
    (*p)++;
    return 1;
}

// A quoted string with C escapes into buf, returning its length or -1.
// \# becomes the byte 0xff for $SEQ.
static int quoted(char **p, u8 *buf, int cap)
{
    skipws(p);
    if (**p != '"') return -1;
    char *s = *p + 1;
    int   n = 0;
    for (; *s && *s!='"'; s++) {
        int c = (u8)*s;
        if (c == '\\') {
            s++;
            switch (*s) {
            case 'e': c = 0x1b; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'a': c = 0x07; break;
            case 'b': c = 0x08; break;
            case '0': c = 0x00; break;
            case '#': c = 0xff; break;
            case 'x': {
                char hex[3] = {s[1], s[1] ? s[2] : 0, 0};
                c = (int)strtol(hex, 0, 16);
                s += 2;
            } break;
            default: c = (u8)*s;
            }
        }
        if (n < cap) buf[n++] = (u8)c;
    }
    if (*s != '"') return -1;
    *p = s + 1;
    return n;
}

static void escape(char *dst, int cap, u8 *s, int n)
{
    int j = 0;
    for (int i = 0; i < n && j < cap-5; i++) {
        u8 c = s[i];
        if (c == 0x1b) {
            j += snprintf(dst+j, (size_t)(cap-j), "\\e");
        } else if (c < 0x20 || c == 0x7f) {
            j += snprintf(dst+j, (size_t)(cap-j), "\\x%02x", c);
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = 0;
}


// Screen inspection

// Text of cells [c0, c1) of row r: blanks as spaces, trailing trimmed
static int rowtext(Tester *s, int r, int c0, int c1, u8 *out, int cap)
{
    ariel_Cell cells[4096];
    int cols = ariel_row(s->t, r, cells, countof(cells));
    if (c1 > cols) c1 = cols;
    Out o = newout(out, cap);
    int keep = 0;
    for (int x = c0; x < c1; x++) {
        if (cells[x].width == 0) continue;
        if (!cells[x].cp) {
            outc(&o, ' ');
            continue;
        }
        u32 cps[16];
        iz  n = ariel_cluster(s->t, x, r, cps, countof(cps));
        for (iz i = 0; i < n && i < countof(cps); i++) {
            oututf8(&o, cps[i]);
        }
        keep = o.len;
    }
    return keep;
}

static int checkcell(Script *sc, Tester *s, char *p)
{
    int r = (int)number(&p);
    comma(&p);
    int c = (int)number(&p);
    if (!expect(&p, '=')) return 0;
    ariel_Cell cells[4096];
    int cols = ariel_row(s->t, r, cells, countof(cells));
    if (c >= cols) {
        fail(sc, "cell %d,%d out of range", r, c);
        return 1;
    }
    ariel_Cell *cell = cells + c;
    for (;;) {
        skipws(&p);
        if (!*p || *p=='\n') break;
        if (*p == '{') {
            p++;
            u32 want[16], got[16];
            int nw = 0;
            while (!expect(&p, '}')) {
                if (nw < 16) want[nw++] = (u32)number(&p);
                comma(&p);
                if (!*p) return 0;
            }
            iz ng = ariel_cluster(s->t, c, r, got, countof(got));
            if (!cell->width) ng = 0;
            int same = ng == nw;
            for (int i = 0; same && i < nw; i++) same = want[i] == got[i];
            if (!same) {
                fail(sc, "cell %d,%d: chars differ (%d vs %d, first U+%04X)",
                     r, c, (int)ng, nw, ng ? got[0] : 0);
            }
        } else if (word(&p, "width")) {
            expect(&p, '=');
            int w = (int)number(&p);
            if (w != cell->width) {
                fail(sc, "cell %d,%d: width %d, want %d", r, c, cell->width, w);
            }
        } else if (word(&p, "attrs")) {
            expect(&p, '=');
            expect(&p, '{');
            int want = 0;
            while (!expect(&p, '}')) {
                skipws(&p);
                if (!strncmp(p, "BL", 2)) { want |= ARIEL_A_BLINK; p += 2; }
                else if (*p == 'B') { want |= ARIEL_A_BOLD; p++; }
                else if (*p == 'I') { want |= ARIEL_A_ITALIC; p++; }
                else if (*p == 'D') { want |= ARIEL_A_FAINT; p++; }
                else if (*p == 'S') { want |= ARIEL_A_STRIKE; p++; }
                else if (*p == 'O') { want |= ARIEL_A_OVERLINE; p++; }
                else if (*p == 'H') { want |= ARIEL_A_HIDDEN; p++; }
                else if (*p == 'U') { p++; want |= (int)number(&p) << 3; }
                else if (*p == 'R' || *p == 'F') { p++; number(&p); }  // libvterm's
                else return 0;
                comma(&p);
            }
            int mask = ~(ARIEL_A_DEFBG | ARIEL_A_SELECTED);
            if ((cell->attr & mask) != want) {
                fail(sc, "cell %d,%d: attrs %#x, want %#x", r, c, cell->attr & mask, want);
            }
        } else if (word(&p, "fg") || word(&p, "bg")) {
            int isfg = p[-1] == 'g' && p[-2] == 'f';
            expect(&p, '=');
            if (strncmp(p, "rgb(", 4)) return 0;
            p += 4;
            long rr = number(&p); comma(&p);
            long gg = number(&p); comma(&p);
            long bb = number(&p);
            expect(&p, ')');
            u32 want = (u32)(rr<<16 | gg<<8 | bb);
            u32 got  = isfg ? cell->fg : cell->bg;
            if (got != want) {
                fail(sc, "cell %d,%d: %s %06x, want %06x", r, c, isfg ? "fg" : "bg", got, want);
            }
        } else {
            return 0;
        }
    }
    return 1;
}

static int modefield(ariel_Modes *m, char *name, int *v)
{
    static struct { char *name; int off, size; } const fields[] = {
        #define F(n) {#n, (int)offsetof(ariel_Modes, n), (int)sizeof(((ariel_Modes *)0)->n)}
        F(cols), F(rows), F(mouse), F(mouseenc), F(kitty), F(altscreen),
        F(sync), F(appcursor), F(appkeypad), F(paste), F(focus),
        F(altscroll), F(reverse),
        #undef F
    };
    for (int i = 0; i < countof(fields); i++) {
        if (!strcmp(fields[i].name, name)) {
            u8 *base = (u8 *)m + fields[i].off;
            *v = fields[i].size==4 ? *(i32 *)base : *base;
            return 1;
        }
    }
    return 0;
}

static int keyname(char *name, ariel_Key *k)
{
    static struct { char *name; int key; } const keys[] = {
        {"ENTER", ARIEL_K_ENTER}, {"TAB", ARIEL_K_TAB},
        {"BACKSPACE", ARIEL_K_BACKSPACE}, {"ESCAPE", ARIEL_K_ESCAPE},
        {"INSERT", ARIEL_K_INSERT}, {"DELETE", ARIEL_K_DELETE},
        {"HOME", ARIEL_K_HOME}, {"END", ARIEL_K_END},
        {"PAGEUP", ARIEL_K_PAGEUP}, {"PAGEDOWN", ARIEL_K_PAGEDOWN},
        {"UP", ARIEL_K_UP}, {"DOWN", ARIEL_K_DOWN},
        {"LEFT", ARIEL_K_LEFT}, {"RIGHT", ARIEL_K_RIGHT},
        {"KPENTER", ARIEL_K_KPENTER}, {"KPADD", ARIEL_K_KPADD},
    };
    if (name[0]=='\'' && name[1] && name[2]=='\'') {
        k->cp = (u8)name[1];
        return 1;
    }
    if (name[0]=='F' && name[1]>='1' && name[1]<='9') {
        k->key = ARIEL_K_F1 + atoi(name+1) - 1;
        return 1;
    }
    if (!strncmp(name, "KP", 2) && name[2]>='0' && name[2]<='9' && !name[3]) {
        k->key = ARIEL_K_KP0 + name[2] - '0';
        return 1;
    }
    for (int i = 0; i < countof(keys); i++) {
        if (!strcmp(keys[i].name, name)) {
            k->key = keys[i].key;
            return 1;
        }
    }
    return 0;
}

static int mods(char **p)
{
    int m = 0;
    for (;;) {
        if (word(p, "shift")) m |= ARIEL_SHIFT;
        else if (word(p, "alt")) m |= ARIEL_ALT;
        else if (word(p, "ctrl")) m |= ARIEL_CTRL;
        else return m;
    }
}

static void setenc(Tester *s, ariel_Str r)
{
    s->nenc = (i32)(r.len < (iz)sizeof(s->enc) ? r.len : (iz)sizeof(s->enc));
    if (s->nenc) memcpy(s->enc, r.data, (size_t)s->nenc);
}

static void runline(Script *sc, Tester *s, char *p)
{
    static u8 buf[1<<16];
    static u8 got[1<<16];
    char tmp[512], tmp2[512];
    skipws(&p);
    if (!*p || *p=='\n' || *p=='#') return;
    if (*p == '!') {
        char *e = strchr(p, '\n');
        int   n = (int)(e ? e-p-1 : (long)strlen(p+1));
        snprintf(sc->section, sizeof(sc->section), "%.*s", n, p+1);
        return;
    }

    if (word(&p, "INIT")) {
        int rows = 25, cols = 80;
        skipws(&p);
        if (*p>='0' && *p<='9') {
            rows = (int)number(&p);
            comma(&p);
            cols = (int)number(&p);
        }
        tinit(s, rows, cols);
        clearlog(s);
        return;
    }
    if (word(&p, "FLAGS")) {
        s->flags = 0;
        for (;;) {
            if (word(&p, "wideambig")) s->flags |= ARIEL_F_WIDEAMBIG;
            else if (word(&p, "legacywidth")) s->flags |= ARIEL_F_LEGACYWIDTH;
            else if (word(&p, "boldbright")) s->flags |= ARIEL_F_BOLDBRIGHT;
            else if (word(&p, "noreply")) s->flags |= ARIEL_F_NOREPLY;
            else break;
        }
        tinit(s, s->rows, s->cols);
        return;
    }
    if (word(&p, "RESET")) {
        ariel_reset(s->t, ARIEL_RESET_FULL);
        clearlog(s);
        return;
    }
    if (word(&p, "RESIZE")) {
        int rows = (int)number(&p);
        comma(&p);
        int cols = (int)number(&p);
        if (!ariel_resize(s->t, cols, rows, 8, 16)) fail(sc, "resize refused");
        return;
    }
    if (word(&p, "PUSH")) {
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        clearlog(s);
        tfeed(s, buf, n, sc->bytewise);
        return;
    }
    if (word(&p, "$REP")) {
        long count = number(&p);
        if (!expect(&p, ':') || !word(&p, "PUSH")) goto syntax;
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        clearlog(s);
        for (long i = 0; i < count; i++) tfeed(s, buf, n, sc->bytewise);
        return;
    }
    if (word(&p, "$SEQ")) {
        long a = number(&p);
        long b = number(&p);
        if (!expect(&p, ':') || !word(&p, "PUSH")) goto syntax;
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        clearlog(s);
        for (long v = a; v <= b; v++) {
            int m = 0;
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0xff) {
                    m += snprintf((char *)got+m, sizeof(got)-(size_t)m, "%ld", v);
                } else {
                    got[m++] = buf[i];
                }
            }
            tfeed(s, got, m, sc->bytewise);
        }
        return;
    }
    if (word(&p, "KEY")) {
        skipws(&p);
        char name[32] = {0};
        int  i = 0;
        for (; *p && *p!=' ' && *p!='\n' && i < 31; p++) name[i++] = *p;
        ariel_Key k = {0};
        if (!keyname(name, &k)) goto syntax;
        k.mods = mods(&p);
        if (k.cp && !(k.mods & (ARIEL_CTRL))) {
            k.text.data = (u8 *)name + 1;
            k.text.len  = 1;
        }
        setenc(s, ariel_key(s->t, &k));
        return;
    }
    if (word(&p, "MOUSE")) {
        ariel_Mouse m = {0};
        if (word(&p, "press")) m.action = ARIEL_PRESS;
        else if (word(&p, "release")) m.action = ARIEL_RELEASE;
        else if (word(&p, "motion")) m.action = ARIEL_MOTION;
        else goto syntax;
        m.button = (int)number(&p);
        m.y = (int)number(&p);
        comma(&p);
        m.x = (int)number(&p);
        m.mods = mods(&p);
        setenc(s, ariel_mouse(s->t, &m));
        return;
    }
    if (word(&p, "PASTE")) {
        int n = quoted(&p, buf, countof(buf)/2);
        if (n < 0) goto syntax;
        ariel_Str in = {buf, n};
        iz len = ariel_paste(s->t, s->enc, countof(s->enc), in,
                             ARIEL_PASTE_BEGIN|ARIEL_PASTE_END);
        s->nenc = (i32)len;
        return;
    }
    if (word(&p, "WANTSTATE") || word(&p, "WANTSCREEN") || word(&p, "UTF8") ||
        word(&p, "WANTENCODING") || word(&p, "WANTPARSER")) {
        return;
    }

    checks++;
    if (word(&p, "?cursor")) {
        if (!expect(&p, '=')) goto syntax;
        int r = (int)number(&p);
        comma(&p);
        int c = (int)number(&p);
        ariel_Cursor cur = ariel_cursor(s->t);
        if (cur.y != r || cur.x != c) {
            fail(sc, "cursor %d,%d, want %d,%d", cur.y, cur.x, r, c);
        }
        return;
    }
    if (word(&p, "?screen_row")) {
        int r = (int)number(&p);
        if (!expect(&p, '=')) goto syntax;
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        int m = rowtext(s, r, 0, 4096, got, countof(got));
        if (m != n || memcmp(got, buf, (size_t)n)) {
            escape(tmp, sizeof(tmp), got, m);
            escape(tmp2, sizeof(tmp2), buf, n);
            fail(sc, "row %d \"%s\", want \"%s\"", r, tmp, tmp2);
        }
        return;
    }
    if (word(&p, "?screen_chars") || word(&p, "?screen_text")) {
        int text = p[-1] == 't';
        int r0 = (int)number(&p); comma(&p);
        int c0 = (int)number(&p); comma(&p);
        int r1 = (int)number(&p); comma(&p);
        int c1 = (int)number(&p);
        if (!expect(&p, '=')) goto syntax;
        int m = 0;
        for (int r = r0; r < r1; r++) {
            if (r > r0) got[m++] = '\n';
            m += rowtext(s, r, c0, c1, got+m, countof(got)-m);
        }
        // Trailing blank rows don't count
        while (m && got[m-1]=='\n') m--;
        int n;
        if (text) {
            Out o = newout(buf, countof(buf));
            while (*p && *p!='\n') {
                u32 cp = (u32)number(&p);
                oututf8(&o, cp);
                comma(&p);
                skipws(&p);
            }
            n = o.len;
        } else {
            n = quoted(&p, buf, countof(buf));
            if (n < 0) goto syntax;
        }
        if (text) {
            // screen_text has no newlines
            int k = 0;
            for (int i = 0; i < m; i++) if (got[i] != '\n') got[k++] = got[i];
            m = k;
        }
        if (m != n || memcmp(got, buf, (size_t)n)) {
            escape(tmp, sizeof(tmp), got, m);
            escape(tmp2, sizeof(tmp2), buf, n);
            fail(sc, "chars \"%s\", want \"%s\"", tmp, tmp2);
        }
        return;
    }
    if (word(&p, "?screen_cell")) {
        if (!checkcell(sc, s, p)) goto syntax;
        return;
    }
    if (word(&p, "?screen_eol")) {
        int r = (int)number(&p);
        comma(&p);
        int c = (int)number(&p);
        if (!expect(&p, '=')) goto syntax;
        int want = (int)number(&p);
        ariel_Cell cells[4096];
        int cols = ariel_row(s->t, r, cells, countof(cells));
        int eol  = 1;
        for (int x = c; x < cols; x++) eol &= !cells[x].cp;
        if (eol != want) fail(sc, "eol %d,%d is %d", r, c, eol);
        return;
    }
    if (word(&p, "?title") || word(&p, "?icon")) {
        int which = p[-1] == 'n';
        if (!expect(&p, '=')) goto syntax;
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        ariel_Str t = ariel_title(s->t, which);
        if (t.len != n || (n && memcmp(t.data, buf, (size_t)n))) {
            fail(sc, "title \"%.*s\", want \"%.*s\"", (int)t.len, (char *)t.data, n, (char *)buf);
        }
        return;
    }
    if (word(&p, "?mode")) {
        skipws(&p);
        char name[32] = {0};
        int  i = 0;
        for (; *p && *p!=' ' && *p!='=' && i < 31; p++) name[i++] = *p;
        if (!expect(&p, '=')) goto syntax;
        int want = (int)number(&p);
        ariel_Modes m = ariel_modes(s->t);
        int got1;
        if (!modefield(&m, name, &got1)) goto syntax;
        if (got1 != want) fail(sc, "mode %s is %d, want %d", name, got1, want);
        return;
    }
    if (word(&p, "?lineinfo")) {
        int r = (int)number(&p);
        if (!expect(&p, '=')) goto syntax;
        u32 want = 0;
        if (word(&p, "wrapped")) want = ARIEL_R_WRAPPED;
        else if (word(&p, "prompt")) want = ARIEL_R_PROMPT;
        else if (!word(&p, "-")) goto syntax;
        u32 flags = ariel_rowinfo(s->t, r).flags;
        if (flags != want) fail(sc, "lineinfo %d is %#x, want %#x", r, flags, want);
        return;
    }
    if (word(&p, "output")) {
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        int left = s->nreplies - s->rpos;
        if (left < n || memcmp(s->replies+s->rpos, buf, (size_t)n)) {
            escape(tmp, sizeof(tmp), s->replies+s->rpos, left);
            escape(tmp2, sizeof(tmp2), buf, n);
            fail(sc, "output \"%s\", want \"%s\"", tmp, tmp2);
            s->rpos = s->nreplies;
        } else {
            s->rpos += n;
        }
        return;
    }
    if (word(&p, "encoded")) {
        int n = quoted(&p, buf, countof(buf));
        if (n < 0) goto syntax;
        if (s->nenc != n || memcmp(s->enc, buf, (size_t)n)) {
            escape(tmp, sizeof(tmp), s->enc, s->nenc);
            escape(tmp2, sizeof(tmp2), buf, n);
            fail(sc, "encoded \"%s\", want \"%s\"", tmp, tmp2);
        }
        return;
    }
    if (word(&p, "event")) {
        int n = quoted(&p, buf, countof(buf)-1);
        if (n < 0) goto syntax;
        buf[n] = 0;
        if (s->epos == s->nevents) {
            fail(sc, "no event, want \"%s\"", buf);
        } else if (strcmp(s->events[s->epos], (char *)buf)) {
            fail(sc, "event \"%s\", want \"%s\"", s->events[s->epos], buf);
        }
        if (s->epos < s->nevents) s->epos++;
        return;
    }
    if (word(&p, "noevent")) {
        if (s->epos < s->nevents) fail(sc, "unexpected event \"%s\"", s->events[s->epos]);
        return;
    }
    checks--;
    if (*p>='a' && *p<='z') return;     // a libvterm trace
syntax:
    fail(sc, "bad line");
}

static char *slurp(char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) n = 0;
    buf[n] = 0;
    fclose(f);
    if (len) *len = n;
    return buf;
}

static void runscript(char *path)
{
    char *text = slurp(path, 0);
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        failures++;
        return;
    }
    for (int bytewise = 0; bytewise < 2; bytewise++) {
        static Tester s;
        s.flags = 0;
        tinit(&s, 25, 80);
        Script sc = {0};
        sc.file     = path;
        sc.bytewise = bytewise;
        for (char *p = text; *p;) {
            sc.line++;
            char *e = strchr(p, '\n');
            runline(&sc, &s, p);
            p = e ? e+1 : p+strlen(p);
        }
        tfree(&s);
    }
    free(text);
}


// Unicode data

static u32 readhex(char **p)
{
    skipws(p);
    char *e;
    u32 v = (u32)strtoul(*p, &e, 16);
    *p = e;
    return v;
}

static void graphemetest(char *path)
{
    char *text = slurp(path, 0);
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        failures++;
        return;
    }
    int line = 0;
    for (char *p = text; *p;) {
        line++;
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *start = p;
        skipws(&p);
        if (*p == '/') {
            checks++;
            p++;
            Seg seg   = {0};
            int first = 1;
            int want  = 1;
            for (;;) {
                skipws(&p);
                if (!*p) break;
                u32 cp  = readhex(&p);
                b32 got = segbreak(&seg, uniprop(cp));
                if (!first && got != want) {
                    fprintf(stderr, "%s:%d: break before U+%04X is %d: %s\n",
                            path, line, cp, got, start);
                    failures++;
                    break;
                }
                first = 0;
                skipws(&p);
                want = *p == '/';
                if (*p) p++;
            }
        }
        p = e ? e+1 : p+strlen(p);
    }
    free(text);
}

static void emojitest(char *path)
{
    char *text = slurp(path, 0);
    if (!text) {
        fprintf(stderr, "%s: cannot read\n", path);
        failures++;
        return;
    }
    static Tester s;
    s.flags = 0;
    int line = 0;
    for (char *p = text; *p;) {
        line++;
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *start = p;
        skipws(&p);
        if (*p && *p != '#') {
            checks++;
            u8  buf[128];
            Out o = newout(buf, countof(buf));
            int n = 0;
            while (*p) {
                oututf8(&o, readhex(&p));
                n++;
                skipws(&p);
            }
            tinit(&s, 2, 10);
            tfeed(&s, buf, o.len, 0);
            ariel_Cursor c = ariel_cursor(s.t);
            ariel_Cell cells[10];
            ariel_row(s.t, 0, cells, 10);
            u32 cps[16];
            iz  got  = ariel_cluster(s.t, 0, 0, cps, countof(cps));
            int want = n < 1+POOLN ? n : 1+POOLN;
            if (c.x!=2 || c.y!=0 || cells[0].width!=2 || got!=want) {
                fprintf(stderr, "%s:%d: cursor %d,%d width %d count %d: %s\n",
                        path, line, c.y, c.x, cells[0].width, (int)got, start);
                failures++;
            }
        }
        p = e ? e+1 : p+strlen(p);
    }
    tfree(&s);
    free(text);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *name = argv[i];
        size_t n = strlen(name);
        if (n>=12 && !strcmp(name+n-12, "grapheme.txt")) {
            graphemetest(name);
        } else if (n>=9 && !strcmp(name+n-9, "emoji.txt")) {
            emojitest(name);
        } else {
            runscript(name);
        }
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
