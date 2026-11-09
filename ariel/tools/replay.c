// ariel-replay: feed a recording to ariel and print the result
//
// $ cc -std=c11 -O2 -o ariel-replay replay.c
// $ ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE
// $ ariel-replay --bench [-b SECONDS] [-x SLACK]
// $ ariel-replay --pty [-s COLSxROWS] [-a] [-k KEYS] [-o OUT] -- CMD [ARG...]
//
//   -s  terminal size (80x24)
//   -a  dump attributes and state too
//   -b  benchmark: feed the file repeatedly for this long, report MB/s
//
// With --bench, run the synthetic suite against the performance targets:
// plain text with scrolling, SGR-dense text, fetching every row of
// 200x60, and resize steps. Each part runs about -b seconds (1). Debug
// builds meet the targets loosened by -x 3.
//
// With --pty (POSIX), run CMD under a pseudo-terminal with ariel as its
// terminal: replies go back to CMD, its output is saved to OUT (a .vt
// recording) and the final screen is dumped. KEYS holds one line per
// keystroke group, sent once output has gone quiet: C escapes (\e \r
// \n \t \xHH), # comments, "@MS" to sleep, an empty line to wait.
//
// Planned: --export.
//
// This is free and unencumbered software released into the public domain.
#include "../ariel.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);

static double now(void)
{
    long long c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c / (double)f;
}
#else
static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}
#endif

// A terminal's block, never freed (msvcrt has no aligned_alloc)
static void *alloc16(ptrdiff_t size)
{
    char *p = malloc((size_t)size + 15);
    if (!p) {
        fprintf(stderr, "ariel-replay: out of memory\n");
        exit(1);
    }
    return p + (-(uintptr_t)p & 15);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE\n"
        "       ariel-replay --bench [-b SECONDS] [-x SLACK]\n"
        "       ariel-replay --pty [-s COLSxROWS] [-a] [-k KEYS] [-o OUT] -- CMD...\n");
    exit(2);
}

static void dump(ariel_Term *t, int attrs)
{
    int flags = attrs ? ARIEL_DUMP_ATTRS|ARIEL_DUMP_STATE : 0;
    ptrdiff_t n = ariel_dump(t, flags, 0, 0);
    unsigned char *out = malloc((size_t)n + 1);
    ariel_dump(t, flags, out, n);
    fwrite(out, 1, (size_t)n, stdout);
    free(out);
}

#ifdef _WIN32
static int record(ariel_Term *t, int cols, int rows, char *keys, char *out, char **argv)
{
    (void)t; (void)cols; (void)rows; (void)keys; (void)out; (void)argv;
    fprintf(stderr, "ariel-replay: --pty needs POSIX\n");
    return 1;
}
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#  include <util.h>
#else
#  include <pty.h>
#endif

// Decode a KEYS line's C escapes in place, returning its length
static size_t unescape(char *s)
{
    size_t n = 0;
    for (char *p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '\\' && p[1]) {
            switch (*++p) {
            case 'e': c = 0x1b; break;
            case 'r': c = '\r'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'a': c = 0x07; break;
            case 'b': c = 0x08; break;
            case 'x': {
                char hex[3] = {p[1], p[1] ? p[2] : 0, 0};
                c = (int)strtol(hex, 0, 16);
                p += p[1] && p[2] ? 2 : 0;
            } break;
            default: c = (unsigned char)*p;
            }
        }
        s[n++] = (char)c;
    }
    return n;
}

static void writeall(int fd, unsigned char *p, ptrdiff_t n)
{
    while (n > 0) {
        ssize_t r = write(fd, p, (size_t)n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
        p += r;
        n -= r;
    }
}

static int record(ariel_Term *t, int cols, int rows, char *keys, char *out, char **argv)
{
    FILE *kf = 0;
    if (keys && !(kf = fopen(keys, "r"))) {
        fprintf(stderr, "ariel-replay: cannot open %s\n", keys);
        return 1;
    }
    struct winsize ws = {0};
    ws.ws_col    = (unsigned short)cols;
    ws.ws_row    = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * 8);
    ws.ws_ypixel = (unsigned short)(rows * 16);
    int   fd;
    pid_t pid = forkpty(&fd, 0, 0, &ws);
    if (pid < 0) {
        perror("ariel-replay: forkpty");
        return 1;
    }
    if (!pid) {
        // Dispositions a background parent ignored survive exec
        static int const sigs[] = {SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU};
        for (size_t i = 0; i < sizeof(sigs)/sizeof(*sigs); i++) {
            signal(sigs[i], SIG_DFL);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, 0);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    size_t cap = 1<<20, len = 0;
    unsigned char *rec = malloc(cap);
    int quiet = 300;    // ms of silence before the next keys
    int done = !kf, idle = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, 50);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0) {
            unsigned char buf[65536];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;  // EIO: the child is gone
            idle = 0;
            if (len+(size_t)n > cap) rec = realloc(rec, cap = 2*(len+(size_t)n));
            memcpy(rec+len, buf, (size_t)n);
            len += (size_t)n;
            ariel_Str in = {buf, n};
            for (;;) {
                ariel_Fed f = ariel_feed(t, in);
                if (!f.ev.kind) break;
                if (f.ev.kind == ARIEL_EV_REPLY) {
                    writeall(fd, f.ev.data.data, f.ev.data.len);
                }
                in = f.rest;
            }
            continue;
        }
        idle += 50;
        if (idle < quiet) continue;
        if (done) {
            if (idle > 1000) break;     // left running: hang up
            continue;
        }
        char line[4096];
        if (!fgets(line, sizeof(line), kf)) {
            done = 1;
            continue;
        }
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#') continue;
        if (line[0] == '@') {
            usleep((useconds_t)atoi(line+1) * 1000);
            continue;
        }
        size_t n = unescape(line);
        writeall(fd, (unsigned char *)line, (ptrdiff_t)n);
        idle = 0;
    }
    // Hang up as a terminal does, then insist
    close(fd);
    int status;
    for (int i = 0; i < 40 && !waitpid(pid, &status, WNOHANG); i++) {
        usleep(50000);
    }
    if (!waitpid(pid, &status, WNOHANG)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(rec, 1, len, f) != len || fclose(f)) {
            fprintf(stderr, "ariel-replay: cannot write %s\n", out);
            return 1;
        }
    }
    return 0;
}
#endif

// Benchmarks

static unsigned long long rng = 0x9e3779b97f4a7c15;

static unsigned rand32(void)
{
    rng = rng*0x3243f6a8885a308d + 1;
    return (unsigned)(rng >> 32);
}

typedef struct {
    unsigned char *data;
    size_t         len, cap;
} Gen;

static void genput(Gen *g, char *s, size_t n)
{
    if (g->cap-g->len < n) {
        g->cap  = g->cap ? g->cap*2 : 1<<20;
        g->data = realloc(g->data, g->cap);
    }
    memcpy(g->data+g->len, s, n);
    g->len += n;
}

static void gens(Gen *g, char *s)
{
    genput(g, s, strlen(s));
}

// Log-like lines of words, 10 to 130 columns, so the screen scrolls
static Gen genplain(size_t len)
{
    Gen g = {0};
    while (g.len < len) {
        int  n = 10 + (int)(rand32()%120);
        char line[160];
        for (int i = 0; i < n; i++) {
            unsigned r = rand32();
            line[i] = r%7 ? (char)('a' + r%26) : ' ';
        }
        line[n] = '\r';
        line[n+1] = '\n';
        genput(&g, line, (size_t)n+2);
    }
    return g;
}

// Lines of CJK text, 5 to max columns, a space now and then: every
// history record holds wide cells
static Gen gencjk(size_t len, int max)
{
    Gen g = {0};
    while (g.len < len) {
        int n = 5 + (int)(rand32()%(unsigned)(max-4));
        for (int col = 0; col < n;) {
            unsigned r = rand32();
            if (r%9 == 0) {
                gens(&g, " ");
                col++;
            } else {
                unsigned cp = 0x4e00 + (r>>8)%0x5000;
                char b[3] = {(char)(0xe0|cp>>12), (char)(0x80|(cp>>6&63)),
                             (char)(0x80|(cp&63))};
                genput(&g, b, 3);
                col += 2;
            }
        }
        gens(&g, "\r\n");
    }
    return g;
}

// Short words, each with its own colors and attributes, as from a
// syntax highlighter or a coloured ls
static Gen gensgr(size_t len)
{
    Gen g = {0};
    int col = 0;
    while (g.len < len) {
        char buf[64];
        unsigned r = rand32();
        switch (r % 6) {
        case 0: snprintf(buf, sizeof(buf), "\x1b[%um", 30+(r>>8)%8); break;
        case 1: snprintf(buf, sizeof(buf), "\x1b[1;%um", 90+(r>>8)%8); break;
        case 2: snprintf(buf, sizeof(buf), "\x1b[38;5;%um", (r>>8)%256); break;
        case 3: snprintf(buf, sizeof(buf), "\x1b[38;2;%u;%u;%um",
                         (r>>8)&255, (r>>16)&255, (r>>24)&255); break;
        case 4: snprintf(buf, sizeof(buf), "\x1b[0;4;48;5;%um", (r>>8)%256); break;
        case 5: snprintf(buf, sizeof(buf), "\x1b[m"); break;
        }
        gens(&g, buf);
        int n = 2 + (int)(rand32()%7);
        for (int i = 0; i < n; i++) buf[i] = (char)('a' + rand32()%26);
        buf[n] = ' ';
        genput(&g, buf, (size_t)n+1);
        col += n + 1;
        if (col > 100) {
            gens(&g, "\x1b[m\r\n");
            col = 0;
        }
    }
    return g;
}

static ariel_Term *benchterm(int cols, int rows, int capcols, int caprows)
{
    ariel_Config cfg = {0};
    cfg.cols    = cols;
    cfg.rows    = rows;
    cfg.capcols = capcols;
    cfg.caprows = caprows;
    ptrdiff_t size = ariel_size(&cfg);
    ariel_Term *t = ariel_init(alloc16(size), size, &cfg);
    ariel_resize(t, cols, rows, 8, 16);
    return t;
}

static void feedgen(ariel_Term *t, Gen *g)
{
    ariel_Str in = {g->data, (ptrdiff_t)g->len};
    for (;;) {
        ariel_Fed r = ariel_feed(t, in);
        if (!r.ev.kind) break;
        in = r.rest;
    }
}

// MB/s feeding g into a 120x40 terminal for about secs seconds
static double benchfeed(Gen *g, double secs)
{
    ariel_Term *t = benchterm(120, 40, 0, 0);
    double start = now(), elapsed = 0;
    long long total = 0;
    do {
        feedgen(t, g);
        total += (long long)g->len;
        elapsed = now() - start;
    } while (elapsed < secs);
    return (double)total/elapsed/1e6;
}

static double slack = 1;   // -x: targets loosened by this factor

static int verdict(char *name, double v, char *unit, double target, int below)
{
    target = below ? target*slack : target/slack;
    int ok = below ? v<target : v>=target;
    printf("%-14s %10.2f %-5s  target %s %g  %s\n",
           name, v, unit, below ? "<" : ">=", target, ok ? "ok" : "MISS");
    return ok;
}

static int benchsuite(double secs)
{
    int ok = 1;

    Gen plain = genplain(8<<20);
    ok &= verdict("plain", benchfeed(&plain, secs), "MB/s", 100, 0);

    Gen sgr = gensgr(8<<20);
    ok &= verdict("sgr", benchfeed(&sgr, secs), "MB/s", 40, 0);

    // Every row of a full, coloured 200x60 screen
    {
        ariel_Term *t = benchterm(200, 60, 0, 0);
        feedgen(t, &sgr);
        static ariel_Cell cells[200];
        long long n = 0;
        unsigned sink = 0;
        double start = now(), elapsed = 0;
        do {
            for (int i = 0; i < 100; i++, n++) {
                for (int y = 0; y < 60; y++) {
                    ariel_row(t, y, cells, 200);
                    sink += cells[y].fg;
                }
            }
            elapsed = now() - start;
        } while (elapsed < secs);
        ok &= verdict("rows 200x60", elapsed/(double)n*1e6, "us", 50, 1);
        if (sink == 1) puts("");   // keep the loop
    }

    // Resize steps on a full 200x60 screen with full history, back and
    // forth across 160x40 to 200x60 inside a 256x96 capacity
    // Lines that fit take a row; longer ones with wide cells are walked
    // whenever the width changes, so that case has no target
    Gen cjk  = gencjk(8<<20, 150);
    Gen cjkl = gencjk(8<<20, 400);
    Gen *fill[] = {&sgr, &cjk, &cjkl};
    char *name[][2] = {{"resize step", "resize worst"},
                       {"resize cjk", "cjk worst"},
                       {"resize cjk400", "cjk400 worst"}};
    for (int f = 0; f < 3; f++) {
        ariel_Term *t = benchterm(200, 60, 256, 96);
        feedgen(t, fill[f]);
        for (int i = 0; i < 80; i++) {
            int d = i<40 ? i : 79-i;   // warm up: touch the whole capacity
            ariel_resize(t, 200-d, 60-d/2, 8, 16);
        }
        long long n = 0;
        double start = now(), elapsed = 0, worst = 0;
        do {
            for (int i = 0; i < 40; i++, n++) {
                int    k  = (int)(n % 80);
                int    d  = k<40 ? k : 79-k;
                double t0 = now();
                ariel_resize(t, 200-d, 60-d/2, 8, 16);
                double dt = now() - t0;
                worst = dt>worst ? dt : worst;
            }
            elapsed = now() - start;
        } while (elapsed < secs);
        double step = elapsed/(double)n*1e3;
        if (f < 2) {
            ok &= verdict(name[f][0], step, "ms", 1, 1);
        } else {
            printf("%-14s %10.2f ms     (no target)\n", name[f][0], step);
        }
        printf("%-14s %10.2f ms     (preemption shows here)\n", name[f][1], worst*1e3);
    }
    free(cjk.data);
    free(cjkl.data);

    free(plain.data);
    free(sgr.data);
    return !ok;
}

int main(int argc, char **argv)
{
    int    cols = 80, rows = 24, attrs = 0, pty = 0, suite = 0;
    double bench = 0;
    char  *path = 0, *keys = 0, *out = 0, **cmd = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pty")) {
            pty = 1;
        } else if (!strcmp(argv[i], "--bench")) {
            suite = 1;
        } else if (!strcmp(argv[i], "--")) {
            cmd = argv + i + 1;
            break;
        } else if (!strcmp(argv[i], "-k") && i+1 < argc) {
            keys = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i+1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "-s") && i+1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &cols, &rows) != 2) usage();
        } else if (!strcmp(argv[i], "-a")) {
            attrs = 1;
        } else if (!strcmp(argv[i], "-x") && i+1 < argc) {
            slack = atof(argv[++i]);
            if (!(slack >= 1)) usage();
        } else if (!strcmp(argv[i], "-b") && i+1 < argc) {
            bench = atof(argv[++i]);
        } else if (argv[i][0]=='-' && argv[i][1]) {
            usage();
        } else {
            path = argv[i];
        }
    }
    if (suite) return benchsuite(bench>0 ? bench : 1);
    if (pty ? !cmd || !*cmd : !path) usage();

    if (pty) {
        ariel_Config cfg = {0};
        cfg.cols = cols;
        cfg.rows = rows;
        ptrdiff_t size = ariel_size(&cfg);
        if (!size) usage();
        ariel_Term *t = ariel_init(alloc16(size), size, &cfg);
        ariel_resize(t, cols, rows, 8, 16);
        int r = record(t, cols, rows, keys, out, cmd);
        dump(t, attrs);
        return r;
    }

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
    void *mem = alloc16(size);
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
    dump(t, attrs);
    return 0;
}
