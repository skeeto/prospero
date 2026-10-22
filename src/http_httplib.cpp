// POSIX HTTP edge: cpp-httplib over mbedTLS, behind a C interface
//
// The one translation unit in the POSIX build that uses the C++ standard
// library. main_posix.cpp runs each request on its own thread through
// httpedge_run and may cancel it from any other thread. Built without
// exceptions (CPPHTTPLIB_NO_EXCEPTIONS), and never retries.
//
// Cancellation must return promptly, so it never calls into the client,
// whose stop() waits on a mutex held for the whole connect and TLS
// handshake. Instead it latches a flag and shuts down a duplicate of the
// socket, kept from the socket-options callback. The duplicate shares
// the connection but not the descriptor number, so a shutdown can never
// land on an unrelated descriptor that reused the number after the
// client closed its own. A cancel that lands before the socket exists
// is caught by the callback, and one that lands during the connect, when
// a shutdown may not take, is caught when the client asks for the
// request body. Requests without a body rely on their read timeout.
//
// On macOS the system decides whether to trust a server's chain, as it
// does for Safari, so that roots a user or an administrator added (a
// corporate proxy's) are trusted, and roots they distrusted are not:
// mbedTLS's own verdict, against /etc/ssl/cert.pem, holds Apple's roots
// alone. mbedTLS still checks the host name, and so does the system.
// main_edgetest.cpp tests this through calls of its own (EDGE_TEST).
//
// This is free and unencumbered software released into the public domain.
#define CPPHTTPLIB_EXPECT_100_THRESHOLD 0  // never wait for "100 Continue"
#include <httplib.h>
#include <psa/crypto.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <stddef.h>
#ifdef __APPLE__
#  include <dlfcn.h>
#endif

// Shared with main_posix.cpp, which declares the same layout.
extern "C" {
struct HttpEdgeReq {
    char const *origin;     // "scheme://host:port", null-terminated
    char const *path;       // with any query, null-terminated
    char const *method;     // null-terminated
    char const *headers;    // "Name: value\r\n" lines
    ptrdiff_t   headerslen;
    char const *body;
    ptrdiff_t   bodylen;
    int         connectms;
    int         readms;     // 0: no limit
    void       *ctx;
    int       (*head)(void *ctx, int status, char const *hdrs, ptrdiff_t len);
    int       (*data)(void *ctx, char const *buf, ptrdiff_t len);
    char       *err;        // receives a description, null-terminated
    ptrdiff_t   errcap;
};
}

enum {
    EDGE_OK,
    EDGE_CANCEL,   // httpedge_cancel was called
    EDGE_CONNECT,  // could not connect
    EDGE_IO,       // the connection failed after connecting
    EDGE_ABORT,    // a callback returned 0
};

struct Edge {
    std::mutex  mu;
    bool        cancelled = false;
    int         dupfd     = -1;
    #ifdef __APPLE__
    std::string untrusted;  // the request's thread only: why the server's
    #endif                  // certificate was refused, if it was
};

// Store msg as the error, cut short at a character boundary if need be.
static void edgeerr(HttpEdgeReq *r, std::string const &msg)
{
    if (r->errcap > 0) {
        size_t n = msg.size() < (size_t)r->errcap-1 ? msg.size() : r->errcap-1;
        for (; n<msg.size() && n && (msg[n]&0xc0)==0x80; n--) {}
        memcpy(r->err, msg.data(), n);
        r->err[n] = 0;
    }
}


#ifdef __APPLE__
// The system's trust evaluation (Security.framework), declared by hand,
// and loaded at the first https request rather than linked: every tool
// process starts under a reaper that is this executable, and the two
// frameworks would cost each one about a millisecond.
using CFRef = void const *;  // any CoreFoundation or Security object

enum : unsigned {
    CF_UTF8 = 0x08000100,  // kCFStringEncodingUTF8
};

struct EdgeSec {
    CFRef         (*datanew)(CFRef, unsigned char const *, long);
    CFRef         (*arraynew)(CFRef, CFRef *, long, void const *);
    CFRef         (*strnew)(CFRef, unsigned char const *, long, unsigned, unsigned char);
    unsigned char (*strget)(CFRef, char *, long, unsigned);
    CFRef         (*errdesc)(CFRef);
    void          (*release)(CFRef);
    CFRef         (*certnew)(CFRef, CFRef);
    CFRef         (*sslpolicy)(unsigned char, CFRef);
    int           (*trustnew)(CFRef, CFRef, CFRef *);
    int           (*setanchors)(CFRef, CFRef);
    bool          (*evaluate)(CFRef, CFRef *);
    void const     *arraycb;  // &kCFTypeArrayCallBacks
};

static CFRef edgeanchors;  // tests only: in place of the system's

// The frameworks, loaded once, or null if they cannot be, which leaves
// the chain to mbedTLS.
static EdgeSec *edgesec()
{
    static EdgeSec        s;
    static bool           ok;
    static std::once_flag once;
    std::call_once(once, [] {
        int   flags = RTLD_NOW | RTLD_LOCAL;
        void *cf    = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", flags);
        void *sec   = dlopen("/System/Library/Frameworks/Security.framework/Security", flags);
        if (!cf || !sec) {
            return;
        }
        s.datanew    = (CFRef (*)(CFRef, unsigned char const *, long))dlsym(cf, "CFDataCreate");
        s.arraynew   = (CFRef (*)(CFRef, CFRef *, long, void const *))dlsym(cf, "CFArrayCreate");
        s.strnew     = (CFRef (*)(CFRef, unsigned char const *, long, unsigned,
                                  unsigned char))dlsym(cf, "CFStringCreateWithBytes");
        s.strget     = (unsigned char (*)(CFRef, char *, long, unsigned))dlsym(cf, "CFStringGetCString");
        s.errdesc    = (CFRef (*)(CFRef))dlsym(cf, "CFErrorCopyDescription");
        s.release    = (void (*)(CFRef))dlsym(cf, "CFRelease");
        s.arraycb    = dlsym(cf, "kCFTypeArrayCallBacks");
        s.certnew    = (CFRef (*)(CFRef, CFRef))dlsym(sec, "SecCertificateCreateWithData");
        s.sslpolicy  = (CFRef (*)(unsigned char, CFRef))dlsym(sec, "SecPolicyCreateSSL");
        s.trustnew   = (int (*)(CFRef, CFRef, CFRef *))dlsym(sec, "SecTrustCreateWithCertificates");
        s.setanchors = (int (*)(CFRef, CFRef))dlsym(sec, "SecTrustSetAnchorCertificates");
        s.evaluate   = (bool (*)(CFRef, CFRef *))dlsym(sec, "SecTrustEvaluateWithError");
        ok = s.datanew && s.arraynew && s.strnew && s.strget && s.errdesc &&
             s.release && s.arraycb && s.certnew && s.sslpolicy &&
             s.trustnew && s.setanchors && s.evaluate;
    });
    return ok ? &s : nullptr;
}

// Certificates as an array, or null. The list stops at the first that
// the system cannot read: one that mbedTLS took (a leaf with a length
// in BER, say) must never be left out, or the next would be judged in
// its place, so that a leaf the handshake proved, but the system never
// saw, would be trusted on another's chain. No leaf, no array.
static CFRef edgecerts(EdgeSec *s, mbedtls_x509_crt const *chain)
{
    CFRef certs[16];  // the leaf first; the system may find the rest
    long  n = 0;
    for (auto c = chain; c && n<16 && c->raw.len; c = c->next) {
        CFRef data = s->datanew(0, c->raw.p, (long)c->raw.len);
        CFRef cert = data ? s->certnew(0, data) : nullptr;
        if (data) {
            s->release(data);
        }
        if (!cert) {
            break;
        }
        certs[n++] = cert;
    }
    CFRef r = n ? s->arraynew(0, certs, n, s->arraycb) : nullptr;
    for (long i = 0; i < n; i++) {
        s->release(certs[i]);  // the array holds its own references
    }
    return r;
}

// The system's verdict on a chain, the leaf first, for a server named
// host: empty if trusted, else why not.
static std::string edgetrust(EdgeSec *s, mbedtls_x509_crt const *chain,
                             std::string const &host)
{
    CFRef name   = s->strnew(0, (unsigned char const *)host.data(),
                             (long)host.size(), CF_UTF8, 0);
    CFRef policy = name ? s->sslpolicy(1, name) : nullptr;
    CFRef certs  = edgecerts(s, chain);
    CFRef trust  = nullptr;
    CFRef err    = nullptr;
    std::string why;
    if (!policy || !certs || s->trustnew(certs, policy, &trust) || !trust) {
        why = "the certificates could not be read";
    } else if (edgeanchors && s->setanchors(trust, edgeanchors)) {
        why = "the test anchors could not be set";
    } else if (!s->evaluate(trust, &err)) {
        why = "the system does not trust it";
        CFRef desc = err ? s->errdesc(err) : nullptr;
        char  buf[512];
        if (desc && s->strget(desc, buf, sizeof(buf), CF_UTF8) && *buf) {
            why = buf;
        }
        if (desc) s->release(desc);
    }
    for (CFRef r : {err, trust, certs, policy, name}) {
        if (r) s->release(r);
    }
    return why;
}

// Have the system decide on the server's chain, once mbedTLS reaches the
// leaf (depth 0), after the rest, whose verdicts give way to this one.
// The leaf comes first in the list of what the server sent. A name that
// mbedTLS found wrong stays wrong.
static void edgeverify(httplib::Client *cli, Edge *e)
{
    EdgeSec *s = edgesec();
    if (!s) {
        return;
    }
    std::string host = cli->host();
    cli->set_server_certificate_verifier([s, e, host](httplib::tls::VerifyContext const &v) {
        if (v.depth > 0) {
            return true;
        } else if (v.error_code & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
            e->untrusted = "it is not for " + host;
            return false;
        }
        e->untrusted = edgetrust(s, (mbedtls_x509_crt const *)v.cert, host);
        return e->untrusted.empty();
    });
}
#endif

static bool edgecancelled(Edge *e)
{
    std::lock_guard<std::mutex> g(e->mu);
    return e->cancelled;
}

extern "C" void *httpedge_new()
{
    static std::once_flag once;
    std::call_once(once, [] { psa_crypto_init(); });
    return new Edge;
}

extern "C" void httpedge_free(void *p)
{
    delete (Edge *)p;
}

extern "C" void httpedge_cancel(void *p)
{
    Edge *e = (Edge *)p;
    std::lock_guard<std::mutex> g(e->mu);
    e->cancelled = true;
    if (e->dupfd >= 0) {
        shutdown(e->dupfd, SHUT_RDWR);
    }
}

extern "C" int httpedge_run(void *p, HttpEdgeReq *r)
{
    Edge *e = (Edge *)p;
    if (edgecancelled(e)) {
        edgeerr(r, "cancelled");
        return EDGE_CANCEL;
    }

    httplib::Client cli(r->origin);
    if (!cli.is_valid()) {
        edgeerr(r, std::string("invalid URL: ") + r->origin);
        return EDGE_CONNECT;
    }
    time_t cs = r->connectms / 1000;
    time_t cu = (r->connectms % 1000) * 1000;
    cli.set_connection_timeout(cs, cu);
    if (r->readms > 0) {
        cli.set_read_timeout(r->readms/1000, (r->readms%1000)*1000);
    } else {
        cli.set_read_timeout(7*24*3600, 0);  // the engine owns idleness
    }
    cli.set_write_timeout(60, 0);
    cli.set_keep_alive(false);
    cli.set_follow_location(false);
    cli.set_socket_options([e](int sock) {
        std::lock_guard<std::mutex> g(e->mu);
        if (e->dupfd >= 0) {
            close(e->dupfd);  // an earlier address that failed
        }
        e->dupfd = fcntl(sock, F_DUPFD_CLOEXEC, 0);
        if (e->cancelled) {
            shutdown(sock, SHUT_RDWR);
        }
    });
    #ifdef __APPLE__
    edgeverify(&cli, e);
    #endif

    httplib::Request req;
    req.method = r->method;
    req.path   = r->path;
    std::string_view hs(r->headers, (size_t)r->headerslen);
    while (!hs.empty()) {
        size_t eol  = hs.find("\r\n");
        auto   line = hs.substr(0, eol);
        hs = eol==std::string_view::npos ? std::string_view{} : hs.substr(eol+2);
        size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto value = line.substr(colon + 1);
        while (!value.empty() && value[0]==' ') {
            value.remove_prefix(1);
        }
        req.headers.emplace(std::string(line.substr(0, colon)), std::string(value));
    }

    if (r->bodylen) {
        // The client asks for the body only once connected, which makes
        // this the place to catch a cancel that the shutdown missed.
        char const *body = r->body;
        req.content_length_ = (size_t)r->bodylen;
        req.content_provider_ = [e, body](size_t off, size_t len,
                                          httplib::DataSink &sink) {
            if (edgecancelled(e)) {
                return false;
            }
            size_t n = len < (1<<16) ? len : (1<<16);
            return sink.write(body + off, n);
        };
    }

    bool aborted = false;
    req.response_handler = [e, r, &aborted](httplib::Response const &res) {
        if (edgecancelled(e)) {
            return false;
        }
        std::string raw;
        for (auto &h : res.headers) {
            raw += h.first;
            raw += ": ";
            raw += h.second;
            raw += "\r\n";
        }
        if (!r->head(r->ctx, res.status, raw.data(), (ptrdiff_t)raw.size())) {
            aborted = true;
            return false;
        }
        return true;
    };
    req.content_receiver = [e, r, &aborted](char const *buf, size_t n,
                                            size_t, size_t) {
        if (edgecancelled(e)) {
            return false;
        }
        if (!r->data(r->ctx, buf, (ptrdiff_t)n)) {
            aborted = true;
            return false;
        }
        return true;
    };

    httplib::Response res;
    httplib::Error    err = httplib::Error::Success;
    bool ok = cli.send(req, res, err);

    bool cancelled;
    {
        std::lock_guard<std::mutex> g(e->mu);
        if (e->dupfd >= 0) {
            close(e->dupfd);
            e->dupfd = -1;
        }
        cancelled = e->cancelled;
    }

    if (ok) {
        if (res.status == 204 || req.method == "HEAD") {
            // No body, so no response handler: report the head here
            if (!r->head(r->ctx, res.status, "", 0)) {
                edgeerr(r, "aborted");
                return EDGE_ABORT;
            }
        }
        return EDGE_OK;
    } else if (cancelled) {
        edgeerr(r, "cancelled");
        return EDGE_CANCEL;
    } else if (aborted) {
        edgeerr(r, "aborted");
        return EDGE_ABORT;
    }
    #ifdef __APPLE__
    if (!e->untrusted.empty()) {
        edgeerr(r, "the server's certificate is not trusted: " + e->untrusted);
        return EDGE_CONNECT;
    }
    #endif
    std::string msg = httplib::to_string(err);
    if (err == httplib::Error::Connection ||
            err == httplib::Error::ConnectionTimeout ||
            err == httplib::Error::SSLConnection ||
            err == httplib::Error::SSLServerVerification ||
            err == httplib::Error::SSLServerHostnameVerification) {
        return edgeerr(r, msg), EDGE_CONNECT;
    }
    edgeerr(r, msg);
    return EDGE_IO;
}


#if defined(__APPLE__) && EDGE_TEST
// Tests only (main_edgetest.cpp), never in the harness itself.

// Certificates in PEM, or null if there are none.
static mbedtls_x509_crt *edgepem(char const *pem, ptrdiff_t len)
{
    std::string text(pem, (size_t)len);  // null-terminated, as mbedTLS wants
    auto *c = new mbedtls_x509_crt;
    mbedtls_x509_crt_init(c);
    if (mbedtls_x509_crt_parse(c, (unsigned char const *)text.c_str(), text.size()+1) ||
            !c->raw.len) {
        mbedtls_x509_crt_free(c);
        delete c;
        return nullptr;
    }
    return c;
}

// Trust the certificates in PEM as the only anchors, in place of the
// system's, from the next request on, made after this returns. Returns
// how many, or -1.
extern "C" int httpedge_testanchors(char const *pem, ptrdiff_t len)
{
    EdgeSec          *s = edgesec();
    mbedtls_x509_crt *c = s ? edgepem(pem, len) : nullptr;
    if (!c) {
        return -1;
    }
    CFRef anchors = edgecerts(s, c);
    int   n       = 0;
    for (auto p = c; p; p = p->next) n++;
    mbedtls_x509_crt_free(c);
    delete c;
    if (!anchors) {
        return -1;
    }
    if (edgeanchors) {
        s->release(edgeanchors);
    }
    edgeanchors = anchors;
    return n;
}

// The system's verdict, as a request would have it, on a chain in PEM,
// the leaf first, for a server named host: 1 if trusted, else 0 and why
// not, or -1 if it is not PEM.
extern "C" int httpedge_testtrust(char const *pem, ptrdiff_t len, char const *host,
                                  char *why, ptrdiff_t cap)
{
    EdgeSec          *s = edgesec();
    mbedtls_x509_crt *c = s ? edgepem(pem, len) : nullptr;
    if (!c) {
        return -1;
    }
    std::string msg = edgetrust(s, c, host);
    mbedtls_x509_crt_free(c);
    delete c;
    HttpEdgeReq r = {};
    r.err    = why;
    r.errcap = cap;
    edgeerr(&r, msg);
    return msg.empty();
}
#endif
