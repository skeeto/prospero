// ariel fuzz target: AFL++ persistent mode, or one input from stdin
//
// $ afl-clang-fast -std=c11 -g3 -fsanitize=address,undefined -o fuzz fuzz.c
// $ afl-fuzz -i corpus -o out ./fuzz
// $ cc -std=c11 -g3 -fsanitize=address,undefined -o fuzz fuzz.c && ./fuzz <input
//
// The first bytes of an input choose the size, flags and feed split. The
// block is malloc'ed at exactly ariel_size so ASan sees overruns. After
// each piece a checker verifies the invariants: bounds, wide pairs, the
// row map, pool references. Interleaved resize, move, scroll, selection
// and encoder operations, and the cross-checks (split invariance, memcpy
// forks, export round trips), join as their features land.
//
// This is free and unencumbered software released into the public domain.
#define ARIEL_TEST
#include "../ariel.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void check(ariel_Term *t)
{
    affirm(t->c.x>=0 && t->c.x<t->cols);
    affirm(t->c.y>=0 && t->c.y<t->rows);
    affirm(t->top>=0 && t->top<=t->bot && t->bot<t->rows);
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
                    affirm(r[x].ext-EXT_POOL < (u32)t->poolused);
                }
                affirm(cellcp(r+x) < 0x110000);
            }
        }
    }
}

static ariel_Term *newterm(ariel_Config *cfg, iz size)
{
    void *mem = aligned_alloc(16, (size_t)size);
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

static u8 *dumpof(ariel_Term *t, iz *len)
{
    int flags = ARIEL_DUMP_ATTRS | ARIEL_DUMP_STATE;
    *len = ariel_dump(t, flags, 0, 0);
    u8 *d = malloc((size_t)*len + 1);
    ariel_dump(t, flags, d, *len);
    return d;
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

int main(void)
{
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
        free(b);

        // Interleaved operations: resize, encoders, rendering
        ariel_reset(a, ARIEL_RESET_FULL);
        for (iz i = 0, k = 0; i < n; i += split, k++) {
            feedall(a, data+i, n-i < split ? n-i : split);
            u8 op = data[i];
            switch (k % 5) {
            case 1:
                ariel_resize(a, 2 + op%cfg.capcols, 1 + data[n-1-i]%cfg.caprows, 8, 16);
                break;
            case 2:
                encode(a, op);
                break;
            case 3:
                if (!(k % every)) render(a);
                break;
            }
            if (!(k % every)) check(a);
        }
        check(a);
        render(a);
        free(a);
    }
    return 0;
}
