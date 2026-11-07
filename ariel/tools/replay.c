// ariel-replay: feed a recording to ariel and print the result
//
// $ cc -std=c11 -O2 -o ariel-replay replay.c
// $ ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE
//
//   -s  terminal size (80x24)
//   -a  dump attributes and state too
//   -b  benchmark: feed the file repeatedly for this long, report MB/s
//
// Planned: --export, and --pty to record a program under a real PTY.
//
// This is free and unencumbered software released into the public domain.
#include "../ariel.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

static void usage(void)
{
    fprintf(stderr, "usage: ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int    cols = 80, rows = 24, attrs = 0;
    double bench = 0;
    char  *path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i+1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &cols, &rows) != 2) usage();
        } else if (!strcmp(argv[i], "-a")) {
            attrs = 1;
        } else if (!strcmp(argv[i], "-b") && i+1 < argc) {
            bench = atof(argv[++i]);
        } else if (argv[i][0]=='-' && argv[i][1]) {
            usage();
        } else {
            path = argv[i];
        }
    }
    if (!path) usage();

    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!f) {
        fprintf(stderr, "ariel-replay: cannot open %s\n", path);
        return 1;
    }
    size_t cap = 1<<20, len = 0;
    unsigned char *data = malloc(cap);
    for (size_t n; (n = fread(data+len, 1, cap-len, f));) {
        len += n;
        if (len == cap) data = realloc(data, cap *= 2);
    }
    if (f != stdin) fclose(f);

    ariel_Config cfg = {0};
    cfg.cols = cols;
    cfg.rows = rows;
    ptrdiff_t size = ariel_size(&cfg);
    if (!size) {
        fprintf(stderr, "ariel-replay: invalid size %dx%d\n", cols, rows);
        return 1;
    }
    void *mem = aligned_alloc(16, (size_t)size);
    ariel_Term *t = ariel_init(mem, size, &cfg);

    double start = now(), elapsed = 0;
    long long total = 0;
    do {
        ariel_Str in = {data, (ptrdiff_t)len};
        for (;;) {
            ariel_Fed r = ariel_feed(t, in);
            if (!r.ev.kind) break;
            in = r.rest;
        }
        total += (long long)len;
        elapsed = now() - start;
    } while (bench>0 && elapsed<bench && len);

    if (bench > 0) {
        printf("%.1f MB/s (%lld bytes in %.2f s)\n", (double)total/elapsed/1e6, total, elapsed);
        return 0;
    }
    int flags = attrs ? ARIEL_DUMP_ATTRS|ARIEL_DUMP_STATE : 0;
    ptrdiff_t n = ariel_dump(t, flags, 0, 0);
    unsigned char *out = malloc((size_t)n + 1);
    ariel_dump(t, flags, out, n);
    fwrite(out, 1, (size_t)n, stdout);
    return 0;
}
