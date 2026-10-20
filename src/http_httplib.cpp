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
// This is free and unencumbered software released into the public domain.
#define CPPHTTPLIB_EXPECT_100_THRESHOLD 0  // never wait for "100 Continue"
#include <httplib.h>
#include <psa/crypto.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <stddef.h>

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
    std::mutex mu;
    bool       cancelled = false;
    int        dupfd     = -1;
};

static void edgeerr(HttpEdgeReq *r, std::string const &msg)
{
    if (r->errcap > 0) {
        size_t n = msg.size() < (size_t)r->errcap-1 ? msg.size() : r->errcap-1;
        memcpy(r->err, msg.data(), n);
        r->err[n] = 0;
    }
}

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
