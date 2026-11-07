// ariel-replay: feed a recording to ariel and print the result
//
// $ cc -std=c11 -O2 -o ariel-replay replay.c
// $ ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE
// $ ariel-replay --pty [-s COLSxROWS] [-a] [-k KEYS] [-o OUT] -- CMD [ARG...]
//
//   -s  terminal size (80x24)
//   -a  dump attributes and state too
//   -b  benchmark: feed the file repeatedly for this long, report MB/s
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

static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ariel-replay [-s COLSxROWS] [-a] [-b SECONDS] FILE\n"
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

int main(int argc, char **argv)
{
    int    cols = 80, rows = 24, attrs = 0, pty = 0;
    double bench = 0;
    char  *path = 0, *keys = 0, *out = 0, **cmd = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pty")) {
            pty = 1;
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
        } else if (!strcmp(argv[i], "-b") && i+1 < argc) {
            bench = atof(argv[++i]);
        } else if (argv[i][0]=='-' && argv[i][1]) {
            usage();
        } else {
            path = argv[i];
        }
    }
    if (pty ? !cmd || !*cmd : !path) usage();

    if (pty) {
        ariel_Config cfg = {0};
        cfg.cols = cols;
        cfg.rows = rows;
        ptrdiff_t size = ariel_size(&cfg);
        if (!size) usage();
        ariel_Term *t = ariel_init(aligned_alloc(16, (size_t)size), size, &cfg);
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
    dump(t, attrs);
    return 0;
}
