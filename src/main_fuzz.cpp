// AFL++ fuzz layer: the JSON reader and writer, or with -DFUZZ_EVENTS the
// session log's open, recovery, and replay over a one-file stub platform,
// or with -DFUZZ_PROVIDER the engine's parsers of untrusted text
//   $ afl-clang-fast++ -std=c++20 -fno-exceptions -fno-rtti -g3 \
//         -fsanitize=address,undefined -fsanitize-trap=undefined \
//         -o fuzzjson src/main_fuzz.cpp
//   $ mkdir i && printf '{"a":[1,"\\u00e9",null]}' >i/doc
//   $ afl-fuzz -i i -o o ./fuzzjson
//
// Likewise with -DFUZZ_EVENTS or -DFUZZ_PROVIDER for the other targets.
//
// JSON: an input either fails at an offset inside it, or parses to a root
// spanning the input less surrounding whitespace, and writes canonical
// UTF-8 that parses again and writes back to identical bytes. Written
// inside a record it parses again too, and comes back the same unless
// it nests too deeply to fit, when only its innermost containers change.
//
// Events: opening leaves only complete lines (the original ones followed
// by recovery records), replay agrees with open, and a second open finds
// nothing to repair or recover. All of this holds as well when the log
// refuses truncation, and when writes fail partway through recovery, a
// later open leaves the same file as an undisturbed one would have.
//
// Provider: a stream, chunked at points the input picks, accumulates
// into blocks whose request encoding always parses; repaired arguments
// sent back are always an object; cleaned output holds no escape or
// carriage return; normalized paths are rooted, free of "." and ".."
// and stable under renormalizing; edit matches are ordered spans inside
// the text; and URL, discovery, error and cycle parsing never fault.
//
// Without AFL++ it runs one input from standard input.
//
// This is free and unencumbered software released into the public domain.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "harness.cpp"

#ifndef __AFL_FUZZ_TESTCASE_LEN
static ssize_t fuzzlen;
static i32     fuzzruns;
static u8      fuzzbuf[1<<16];
#  define __AFL_FUZZ_INIT()
#  define __AFL_INIT()
#  define __AFL_FUZZ_TESTCASE_BUF fuzzbuf
#  define __AFL_FUZZ_TESTCASE_LEN fuzzlen
#  define __AFL_LOOP(n) \
       (!fuzzruns++ && (fuzzlen = read(0, fuzzbuf, sizeof(fuzzbuf))) >= 0)
#endif

// One file, standing in for any path, with failures on request.
struct Os {
    u8 *data;
    iz  len;
    iz  cap;
    b32 exists;
    b32 failtrunc;   // os_logtrunc fails
    b32 failwrites;  // os_logwrite fails once writes runs out
    i32 writes;
};

[[noreturn]] static void os_oom() { __builtin_trap(); }  // a bug at these sizes
static b32 os_commit(Region *, iz) { return 0; }
static Region *os_reserve(Os *, iz) { return 0; }
static void os_release(Os *, Region *) {}
static i64  os_ticks(Os *) { return 0; }
static i64  os_time(Os *) { return 0; }
static u64  os_random(Os *) { return 0; }

static Read os_read(Os *os, Arena *a, Str, iz max)
{
    Read r = {};
    if (!os->exists) {
        r.err = READ_NOTFOUND;
        return r;
    }
    r.size = os->len;
    if (os->len > max) {
        r.err = READ_TOOBIG;
        return r;
    }
    r.data = clone(a, {os->data, os->len});
    return r;
}

static b32     os_write(Os *, Arena, Str, Str) { return 0; }
static b32     os_mkdirs(Os *, Arena, Str) { return 0; }
static Dirent *os_list(Os *, Arena *, Str) { return 0; }
static Stat    os_stat(Os *, Arena, Str) { return {}; }
static b32     os_rename(Os *, Arena, Str, Str) { return 0; }
static b32     os_remove(Os *, Arena, Str) { return 0; }
static b32     os_print(Os *, i32, Str) { return 1; }
static Str     os_getenv(Os *, Arena *, Str) { return {}; }
static Str     os_exepath(Os *, Arena *) { return "harness"; }
static b32     os_thread(Os *, void (*)(void *), void *) { return 0; }
static void    os_lockinit(Lock *) {}
static void    os_lock(Lock *) {}
static void    os_unlock(Lock *) {}
static b32     os_eventinit(Event *) { return 1; }
static void    os_signal(Event *) {}
static b32     os_wait(Event *, i32) { return 0; }

static iz os_logopen(Os *os, Arena, Str)
{
    os->exists = 1;
    return 0;
}

static b32 os_logwrite(Os *os, iz, Str s)
{
    if (os->failwrites) {
        if (!os->writes) {
            return 0;
        }
        os->writes--;
    }
    if (s.len > os->cap-os->len) {
        return 0;
    }
    copybytes(os->data+os->len, s.data, s.len);
    os->len += s.len;
    return 1;
}

static b32 os_logsync(Os *, iz) { return 1; }

static b32 os_logtrunc(Os *os, iz, i64 len)
{
    if (os->failtrunc || len<0 || len>os->len) {
        return 0;
    }
    os->len = (iz)len;
    return 1;
}

static void os_logclose(Os *, iz) {}

static Read os_readat(Os *, Arena *, Str, i64, iz) { return {{}, 0, READ_NOTFOUND}; }
static Slice<Str> os_environ(Os *, Arena *) { return {}; }
static iz    os_stdin(Os *, u8 *, iz) { return 0; }
static Str   os_realpath(Os *, Arena *, Str) { return {}; }
static Proc *os_spawn(Os *, Spawn *, Arena) { return 0; }
static void  os_kill(Os *, Proc *, b32) {}
static void  os_procfree(Os *, Proc *) {}
static Http *os_http(Os *, HttpReq *, Arena) { return 0; }
static void  os_httpcancel(Os *, Http *) {}
static void  os_httpfree(Os *, Http *) {}
static Str   os_lasterr(Os *, Arena *) { return {}; }


// Valid UTF-8 without surrogates or raw control characters.
static b32 cleanjson(Str s)
{
    for (iz i = 0; i < s.len;) {
        if (s[i] < 0x20) {
            return 0;
        }
        i32 c = utf8strict(s, &i);
        if (c<0 || (c>=0xd800 && c<=0xdfff)) {
            return 0;
        }
    }
    return 1;
}

// Exercise the accessors and the number printer over a parsed tree,
// returning its depth in arrays and objects.
static i32 walk(Json *j, Arena scratch)
{
    if (j->type == JSON_NUMBER) {
        jsonint(j, 0);
        f64 v = jsonfloat(j, 0);
        Buf     b(&scratch, 64);
        JsonOut w = {&b};
        putfloat(&w, v);
        Str   s = finish(&b);
        Float f = parsefloat(s);
        if (v-v != 0) {
            assert(s == "null");
        } else {
            f64 m = v<0 ? -v : v;
            f64 d = f.val>v ? f.val-v : v-f.val;
            assert(f.ok);
            assert(m<1e-300 || m>1e300 || d<=m*1e-13);
        }
    }

    iz  i     = 0;
    i32 depth = 0;
    for (Json *e = j->child; e; e = e->next, i++) {
        if (i < 16) {
            assert(jsonat(j, i) == e);
            if (j->type == JSON_OBJECT) {
                assert(jsonget(j, e->key)->key == e->key);
            }
        }
        i32 d = walk(e, scratch);
        depth = d>depth ? d : depth;
    }
    assert(i == jsonlen(j));
    assert(jsonat(j, i)->type == JSON_MISSING);
    return depth + (j->type==JSON_ARRAY || j->type==JSON_OBJECT);
}

static Str canon(Arena *a, Json *j)
{
    Buf     b(a, 1<<12);
    JsonOut w = {&b};
    putjson(&w, j);
    return finish(&b);
}

static void fuzzjson(Arena a, Str src)
{
    JsonDoc d = jsonparse(&a, src);
    if (!d.ok) {
        assert(d.root->type == JSON_MISSING);
        assert(d.off>=0 && d.off<=src.len && d.err.len);
        return;
    }
    assert(d.root->raw == trim(src));
    i32 depth = walk(d.root, a);

    Str one = canon(&a, d.root);
    assert(cleanjson(one));
    JsonDoc d2 = jsonparse(&a, one);
    assert(d2.ok);
    assert(canon(&a, d2.root) == one);

    // One level down, as in a request or a record
    Buf   b(&a, 1<<12);
    EvOut w = evbegin(&b, "s", 1, 0, EV_NOTE);
    putkey(&w.json, "v");
    putjson(&w.json, d.root);
    JsonDoc d3 = jsonparse(&a, evend(&w));
    assert(d3.ok);
    Json *v = jsonget(d3.root, "v");
    if (depth < JSON_MAXDEPTH) {
        assert(canon(&a, v) == one);
    } else {
        assert(walk(v, a) == JSON_MAXDEPTH-1);
    }
}

static Os *newos(Arena *a, Str src)
{
    Os *os = alloc<Os>(a);
    os->cap    = 4*src.len + 4096;
    os->data   = allocbytes(a, os->cap);
    os->len    = src.len;
    os->exists = 1;
    copybytes(os->data, src.data, src.len);
    return os;
}

// Check a log just opened: complete lines, a replay that agrees, and
// nothing left for another open to do. Returns the file.
static Str checklog(Os *os, EventLog *log, Arena perm, Arena scratch)
{
    assert(log->ok);
    Str file = {os->data, os->len};
    assert(!file.len || file[file.len-1]=='\n');

    Replay r = logreplay(os, &perm, scratch, "events.jsonl", "s", 0, os->cap);
    assert(r.ok && r.last==log->seq);
    i64 last = 0;
    for (iz i = 0; i < r.lines.len; i++) {
        Arena tmp = scratch;
        EvRec rec = evparse(&tmp, r.lines[i]);
        assert(evvalid(&rec, "s", last));
        last = rec.seq;
    }
    logclose(log);

    EventLog again = logopen(os, &perm, scratch, "events.jsonl", "s", os->cap);
    assert(again.ok && !again.torn && !again.synth);
    assert(again.seq==log->seq && again.bad==log->bad);
    logclose(&again);
    return file;
}

static void fuzzevents(Arena a, Str src)
{
    Arena perm    = a;
    perm.end      = perm.beg + (perm.end - perm.beg)/2;
    Arena scratch = a;
    scratch.beg   = perm.end;
    Str   path    = "events.jsonl";

    Os      *os   = newos(&perm, src);
    EventLog log  = logopen(os, &perm, scratch, path, "s", os->cap);
    Str      want = checklog(os, &log, perm, scratch);
    assert(startswith(want, takehead(src, src.len-log.torn)));

    // Truncation refused: the torn tail is ended instead, and kept
    Os *nt = newos(&perm, src);
    nt->failtrunc = 1;
    EventLog keep = logopen(nt, &perm, scratch, path, "s", nt->cap);
    Str      kept = checklog(nt, &keep, perm, scratch);
    assert(!keep.torn && startswith(kept, src));

    // A write fails partway through recovery: the open fails, and the
    // next one finishes the job as if nothing had happened
    if (log.synth) {
        Os *pw = newos(&perm, src);
        pw->failwrites = 1;
        pw->writes     = (i32)(src.len % log.synth);
        EventLog fail = logopen(pw, &perm, scratch, path, "s", pw->cap);
        assert(!fail.ok && fail.fd<0);
        pw->failwrites = 0;
        EventLog redo = logopen(pw, &perm, scratch, path, "s", pw->cap);
        assert(redo.seq == log.seq);
        assert(checklog(pw, &redo, perm, scratch) == want);
    }
}

// Provider

static void fuzzpaths(Arena a, Str src, b32 win)
{
    Str cwd = win ? Str("C:/w") : Str("/w");
    Str p   = normpath(&a, cwd, src, win);
    iz  r   = pathroot(p, win);
    assert(r > 0);
    for (Str rest = cuthead(p, r); rest.len;) {
        Cut c = cut(rest, '/');
        assert(c.head.len && c.head!="." && c.head!="..");
        rest = c.tail;
    }
    assert(normpath(&a, cwd, p, win) == p);
}

static void fuzzspans(Str hay, Slice<Span> m)
{
    iz last = 0;
    for (iz i = 0; i < m.len; i++) {
        assert(m[i].beg>=last && m[i].beg<=m[i].end && m[i].end<=hay.len);
        last = m[i].end;
    }
}

static void fuzzprovider(Arena a, Str src)
{
    // The stream, cut where the input's own bytes say. The tokenizer and
    // the partial response grow in their own half of the arena, while
    // each chunk's events and parses use the other, reset per chunk.
    Arena own = a;
    own.end   = own.beg + (a.end - a.beg)/2;
    Arena scr = a;
    scr.beg   = own.end;
    Partial *p   = newpartial(&own);
    Sse      sse = newsse(&own, 1<<12);
    for (Str rest = src; rest.len;) {
        iz  n   = 1 + rest[0]%23;
        Str got = takehead(rest, n<rest.len ? n : rest.len);
        rest = cuthead(rest, got.len);
        Arena t = scr;
        Slice<SseEvent> evs = ssefeed(&sse, got, &t);
        for (iz i = 0; i < evs.len; i++) {
            JsonDoc d = jsonparse(&t, evs[i].data);
            if (d.ok) {
                Chunk c = applychunk(p, d.root, &t);
                for (iz k = 0; k < c.deltas.len; k++) {
                    assert(c.deltas[k].block>=0 && c.deltas[k].block<p->nblocks);
                }
            }
        }
    }
    a = scr;
    for (b32 complete = 0; complete < 2; complete++) {
        Finished f = finishpartial(&a, p, complete, 1, 1);
        assert(f.blocks.len <= p->nblocks);
        for (iz i = 0; i < f.blocks.len; i++) {
            Block *b = &f.blocks[i];
            if (b->type != BLOCK_CALL) continue;
            assert(b->id.len);
            for (iz k = 0; k < b->id.len; k++) {
                assert(alnum(b->id[k]) || b->id[k]=='_' || b->id[k]=='-');
            }
            for (iz k = 0; k < i; k++) {
                assert(f.blocks[k].type!=BLOCK_CALL || f.blocks[k].id!=b->id);
            }
        }
        ModelConf mc = {};
        mc.name      = "m";
        mc.reasoning = "reasoning_content";
        Msg m = {};
        m.role   = ROLE_ASSISTANT;
        m.blocks = f.blocks;
        ChatReq req = {};
        req.mc      = &mc;
        req.history = {&m, 1, 1};
        Arena tmp = a;
        tmp.beg  += (tmp.end - tmp.beg)/2;
        Str body  = encodechat(&a, tmp, &req);
        assert(jsonparse(&a, body).ok);
    }

    // Arguments sent back
    Str     wire = wireargs(&a, src);
    JsonDoc d    = jsonparse(&a, wire);
    assert(d.ok && d.root->type==JSON_OBJECT);

    // Output
    Str clean = cleanoutput(&a, src);
    assert(clean.len <= src.len);
    for (iz i = 0; i < clean.len; i++) {
        assert(clean[i]!=0x1b && clean[i]!='\r');
    }

    // Paths, without NULs (which name nothing)
    if (!cut(src, 0).ok) {
        fuzzpaths(a, src, 0);
        fuzzpaths(a, src, 1);
    }

    // Edits: the haystack before the first newline-pair, the rest needle
    iz  sep    = find(src, "\n\n");
    Str hay    = sep<0 ? src : takehead(src, sep);
    Str needle = sep<0 ? Str("a") : cuthead(src, sep+2);
    if (needle.len) {
        fuzzspans(hay, matchexact(&a, hay, needle));
        fuzzspans(hay, matchlines(&a, hay, needle));
        fuzzspans(hay, matchnorm(&a, hay, needle));
    }

    Url u = parseurl(src);
    assert(!u.ok || (u.host.len && u.port>0 && u.port<65536 && u.path.len && u.path[0]=='/'));
    parsemodels(a, src, "m", 0);
    classify(500, src);
    retryafter(src, 0);
    errormessage(&a, src);
    Cycle *c = alloc<Cycle>(&a);
    cyclefeed(c, src);
}

__AFL_FUZZ_INIT();

int main()
{
    __AFL_INIT();
    iz    cap = (iz)1<<26;
    byte *mem = (byte *)malloc((uz)cap);
    u8   *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wconversion"  // inside AFL's macro
        iz len = __AFL_FUZZ_TESTCASE_LEN;
        #pragma GCC diagnostic pop
        if (len > 1<<16) {
            continue;  // bounds memory use
        }
        // An exact-size copy, so ASan sees overreads
        u8 *src = (u8 *)malloc((uz)len + !len);
        memcpy(src, buf, (uz)len);
        Arena a = {mem, mem+cap};
        #if FUZZ_EVENTS
        fuzzevents(a, {src, len});
        #elif FUZZ_PROVIDER
        fuzzprovider(a, {src, len});
        #else
        fuzzjson(a, {src, len});
        #endif
        free(src);
    }
    return 0;
}
