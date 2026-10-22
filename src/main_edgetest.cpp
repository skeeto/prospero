// Edge tests: http_httplib.cpp driven through its C interface, as
// main_posix.cpp drives it, with the test-only calls of its macOS trust
// evaluation, which the harness itself never has (EDGE_TEST)
//
//   prospero_edgetest get URL [ANCHORS]          one GET: "status N", or
//                                                "error: WHY" (exit 1)
//   prospero_edgetest trust HOST CHAIN [ANCHORS] "trusted", or
//                                                "untrusted: WHY" (exit 1)
//
// CHAIN and ANCHORS are PEM files, the leaf first. With ANCHORS, they
// are trusted in place of the system's anchors; without, the system
// decides as it would for the harness. tests/sectrust.py runs these.
//
// This is free and unencumbered software released into the public domain.
#define EDGE_TEST 1
#include "http_httplib.cpp"
#include <stdio.h>

// The whole file, or empty on failure.
static std::string readfile(char const *path)
{
    std::string r;
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[1<<12];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f));) {
            r.append(buf, n);
        }
        if (ferror(f)) {
            r.clear();
        }
        fclose(f);
    }
    return r;
}

static int status;
static int gothead(void *, int s, char const *, ptrdiff_t) { status = s; return 1; }
static int gotdata(void *, char const *, ptrdiff_t)        { return 1; }

static int usage()
{
    fputs("usage: prospero_edgetest get URL [ANCHORS]\n"
          "       prospero_edgetest trust HOST CHAIN [ANCHORS]\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    std::string cmd   = argc>1 ? argv[1] : "";
    int         nargs = cmd=="get" ? 3 : cmd=="trust" ? 4 : 0;
    if (!nargs || argc<nargs || argc>nargs+1) {
        return usage();
    }

    if (argc > nargs) {
        std::string pem = readfile(argv[nargs]);
        if (httpedge_testanchors(pem.data(), (ptrdiff_t)pem.size()) < 1) {
            fprintf(stderr, "prospero_edgetest: no anchors in %s\n", argv[nargs]);
            return 2;
        }
    }

    char why[512] = {};
    if (cmd == "trust") {
        std::string pem = readfile(argv[3]);
        int r = httpedge_testtrust(pem.data(), (ptrdiff_t)pem.size(),
                                   argv[2], why, sizeof(why));
        if (r < 0) {
            fprintf(stderr, "prospero_edgetest: no certificates in %s\n", argv[3]);
            return 2;
        }
        if (!r) {
            printf("untrusted: %s\n", why);
            return 1;
        }
        printf("trusted\n");
        return 0;
    }

    // "scheme://host:port/path": the origin, then the path
    std::string url    = argv[2];
    size_t      slash  = url.find('/', url.find("://")+3);
    std::string origin = url.substr(0, slash);
    std::string path   = slash==std::string::npos ? "/" : url.substr(slash);
    HttpEdgeReq r = {};
    r.origin    = origin.c_str();
    r.path      = path.c_str();
    r.method    = "GET";
    r.headers   = "";
    r.connectms = 10000;
    r.readms    = 30000;
    r.head      = gothead;
    r.data      = gotdata;
    r.err       = why;
    r.errcap    = sizeof(why);
    void *edge = httpedge_new();
    int   err  = httpedge_run(edge, &r);
    httpedge_free(edge);
    if (err != EDGE_OK) {
        printf("error: %s\n", why);
        return 1;
    }
    printf("status %d\n", status);
    return 0;
}
