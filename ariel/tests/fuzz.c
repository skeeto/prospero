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
        void *mem = aligned_alloc(16, (size_t)size);
        memset(mem, 0xa5, (size_t)size);
        ariel_Term *t = ariel_init(mem, size, &cfg);
        affirm(t);
        for (iz i = 4; i < len; i += split) {
            ariel_Str in = {buf+i, len-i < split ? len-i : split};
            for (;;) {
                ariel_Fed f = ariel_feed(t, in);
                if (!f.ev.kind) break;
                in = f.rest;
            }
            check(t);
            ariel_gen(t);
        }
        free(mem);
    }
    return 0;
}
