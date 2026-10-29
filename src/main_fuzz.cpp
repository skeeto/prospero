// AFL++ fuzz layer: the JSON reader and writer, or with -DFUZZ_EVENTS the
// session log's open, recovery, and replay over a one-file stub platform,
// or with -DFUZZ_PROVIDER the engine's parsers of untrusted text, or with
// -DFUZZ_FRAMES the framing of remote links (frame.cpp), or with
// -DFUZZ_IMAGE an image's intake: its kind, its decode through Wuffs
// (image.cpp, wuffs.c), shrinking and PNG encoding, or with -DFUZZ_WEB
// the web tools' text work (web.cpp). Every target links Wuffs, from the
// release file CMake fetched:
//   $ afl-clang-fast -O2 -g3 -fsanitize=address,undefined \
//         -fsanitize-trap=undefined -I build/_deps/wuffs-src \
//         -c -o wuffs.o src/wuffs.c
//   $ afl-clang-fast++ -std=c++20 -fno-exceptions -fno-rtti -g3 \
//         -fsanitize=address,undefined -fsanitize-trap=undefined \
//         -o fuzzjson src/main_fuzz.cpp wuffs.o
//   $ mkdir i && printf '{"a":[1,"\\u00e9",null]}' >i/doc
//   $ afl-fuzz -i i -o o ./fuzzjson
//
// Likewise with -DFUZZ_EVENTS, -DFUZZ_PROVIDER, -DFUZZ_FRAMES,
// -DFUZZ_IMAGE or -DFUZZ_WEB for the other targets.
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
// Provider: a stream, chunked at points the input picks, accumulates as
// chat chunks and as Anthropic events into blocks whose request
// encodings, of both kinds, always parse; repaired arguments
// sent back are always an object; cleaned output holds no escape or
// carriage return; normalized paths are rooted, free of "." and ".."
// and stable under renormalizing; edit matches are ordered spans inside
// the text; and URL, discovery, error and cycle parsing never fault.
//
// Image: a decode fails with a reason and no pixels, leaving the arena
// as it was, or gives pixels within the limit, of the size the header
// alone gives, which shrink to a quarter, and which encode as a PNG
// that decodes back to the same pixels.
//
// Web: a page, read as HTML in its charset, becomes clean text and a
// clean title, and when the input holds no ']' or '&' to forge one, each
// "](" in the text opens a link target that is a canonical URL. URLs
// taken from text, bare or not, are canonical; refusals and redirects
// never fault, and a redirect that passes leads to a canonical URL that
// is not refused. The search services' replies parse without fault into
// canonical hit URLs and clean, cut texts, merged as well, and a window
// of a page's text is valid UTF-8.
//
// Without AFL++ it runs one input from standard input.
//
// This is free and unencumbered software released into the public domain.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "prospero.cpp"

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

#if PROSPERO_ARENACHECK
static void os_arenafault(Str msg)
{
    write(2, msg.data, (uz)msg.len);
}
#endif
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
static b32     os_writemode(Os *, Arena, Str, Str, i32) { return 0; }
static b32     os_mkdirs(Os *, Arena, Str) { return 0; }
static Dirent *os_list(Os *, Arena *, Str) { return 0; }
static Listing os_listall(Os *, Arena *, Str, b32, iz) { return {}; }
static Stat    os_stat(Os *, Arena, Str) { return {}; }
static b32     os_rename(Os *, Arena, Str, Str) { return 0; }
static b32     os_remove(Os *, Arena, Str) { return 0; }
static b32     os_print(Os *, i32, Str) { return 1; }
static Str     os_getenv(Os *, Arena *, Str) { return {}; }
static Str     os_exepath(Os *, Arena *) { return "prospero"; }
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
static Proc *os_spawnio(Os *, Spawn *, Arena) { return 0; }
static b32 os_launch(Os *, Arena, Slice<Str>, Str, Slice<Str>) { return 0; }
static b32   os_procwrite(Os *, Proc *, Str) { return 0; }
static void  os_kill(Os *, Proc *, b32) {}
static void  os_procfree(Os *, Proc *) {}
static iz    os_lockfile(Os *, Arena, Str) { return -1; }
static void  os_unlockfile(Os *, iz) {}
static b32   os_lockheld(Os *, Arena, Str) { return 0; }
static Http *os_http(Os *, HttpReq *, Arena) { return 0; }
static void  os_httpcancel(Os *, Http *) {}
static void  os_httpfree(Os *, Http *) {}
static Str   os_lasterr(Os *, Arena *) { return {}; }
static Str   os_endpoint(Os *, Arena *, Str, Str) { return {}; }
static Listener *os_listen(Os *, Arena, Str, Mailbox *) { return 0; }
static b32 os_loopback(Os *, Mailbox *, u32, u64, i32, Str, i32 *) { return 0; }
static void  os_unlisten(Os *, Listener *) {}
static Conn *os_connect(Os *, Arena, Str, Mailbox *, u64) { return 0; }
static b32   os_listening(Os *, Arena, Str) { return 0; }
static b32   os_connwrite(Os *, Conn *, Str) { return 0; }
static void  os_connclose(Os *, Conn *) {}
static Detached os_detach(Os *, Arena, Slice<Str>, Str, Str) { return {}; }
static i32   os_pid(Os *) { return 1; }
static i32   os_responsible(Os *, i32) { return 0; }
static b32   os_removetree(Os *, Arena, Str) { return 0; }
static b32   os_copyfile(Os *, Arena, Str, Str) { return 0; }
static Str   os_scratchdir(Os *, Arena *) { return {}; }
static Slice<Str> os_tempdirs(Os *, Arena *) { return {}; }


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

static void fuzzspans(Str hay, Slice<EditSpan> m)
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
    Partial *q   = newpartial(&own);  // the same events as Anthropic's
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
                c = anchunk(q, d.root, &t);
                for (iz k = 0; k < c.deltas.len; k++) {
                    assert(c.deltas[k].block>=0 && c.deltas[k].block<q->nblocks);
                }
            }
        }
    }
    a = scr;
    for (i32 run = 0; run < 4; run++) {
        b32      complete = run & 1;
        Partial *r        = run<2 ? p : q;
        if (r == q) {
            q->text.len = q->text.len/(1 + run%2);  // a cycle cut away
        }
        Finished f = finishpartial(&a, r, complete, 1, 1);
        assert(f.blocks.len <= r->nblocks);
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
        Msg m[3] = {};
        m[0].role   = ROLE_USER;
        m[0].text   = src;
        m[1].role   = ROLE_ASSISTANT;
        m[1].blocks = f.blocks;
        m[2].role   = ROLE_TOOL;
        m[2].callid = "c";
        m[2].text   = src;
        ChatReq req = {};
        req.mc      = &mc;
        req.history = {m+1, 1, 1};
        Arena tmp = a;
        tmp.beg  += (tmp.end - tmp.beg)/2;
        Str body  = encodechat(&a, tmp, &req);
        assert(jsonparse(&a, body).ok);
        req.history   = {m, 3, 3};
        req.thinkfrom = run;
        body = anencode(&a, tmp, &req);
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
        fuzzspans(hay, matchnorm(&a, hay, needle));
        // Every rule of the ladder ends in a change or a reason
        EditOut o = editreplace(&a, hay, "f", needle, "x\n", 0);
        assert(o.data.data || o.text.len);
        o = editreplace(&a, hay, "f", needle, needle, 1);
        assert(o.data.data || o.text.len);
    }

    // Patches: parsed or refused, and applied or refused, to the text
    Patch pt = parsepatch(&a, src);
    assert(pt.err.data || pt.files.len);
    for (iz i = 0; i<pt.files.len && !pt.err.data; i++) {
        if (pt.files[i].op == PATCH_UPDATE) {
            EditOut o = patchupdate(&a, hay, "f", &pt.files[i]);
            assert(o.data.data || o.text.len);
        }
    }
    Str tag = filetag(&a, src);
    assert(tag.len == 4);

    Url u = parseurl(src);
    assert(!u.ok || (u.host.len && u.port>0 && u.port<65536 && u.path.len && u.path[0]=='/'));
    parsemodels(a, src, "m", 0);
    classify(500, src);
    retryafter(src, 0);
    errormessage(&a, src);
    Cycle *c = alloc<Cycle>(&a);
    cyclefeed(c, src);
}

// Frames, two ways. First the input as a stream from a peer, cut where
// its bytes say, into a decoder and a link up and running: whatever it
// holds, the decoder takes only whole, valid frames, nothing after it
// breaks, and what the link writes back decodes cleanly. Then the
// input's lines sent over a link whose streams break where its bytes
// say, losing what was in flight: the other side gets every line once,
// in order.
static void fuzzframes(Arena a, Str src)
{
    // A hostile peer
    {
        Arena    t = a;
        FrameDec d = newframedec(&t, 1<<10);
        Link     l = newlink(&t, 1, 1<<12, 16);
        linkconnect(&l, 0);
        FrameIn  hello = {};
        u8       tok[FRAME_TOKEN] = {0, 0, 0, 0, 0, 0, 0, 2};
        hello.type    = FRAME_REPLAY;
        hello.id      = 1;
        hello.payload = {tok, FRAME_TOKEN};
        assert(linkframe(&l, &hello, 0) == LINK_READY);
        linksend(&l, "x");
        i64 now = 0;
        b32 bad = 0;
        for (Str rest = src; rest.len;) {
            iz  n  = 1 + rest[0]%31;
            Str in = takehead(rest, n<rest.len ? n : rest.len);
            rest = cuthead(rest, in.len);
            for (FrameIn f; (f = framenext(&d, &in)).ok;) {
                assert(!bad);
                assert(f.type==FRAME_REGULAR || f.type==FRAME_ACK ||
                       f.type==FRAME_KEEPALIVE || f.type==FRAME_REPLAY ||
                       f.type==FRAME_DISCONNECT);
                assert(f.payload.len <= 1<<10);
                i32 ev = linkframe(&l, &f, now += 1000000);
                assert(ev>=LINK_NONE && ev<=LINK_BROKEN);
                linksend(&l, f.payload);
                linktick(&l, now);
            }
            bad |= d.bad;
            assert(!d.bad || !in.len || framenext(&d, &in).ok==0);

            // What it writes back is well formed
            Arena    u   = t;
            Str      out = linkout(&l, &u, now);
            FrameDec e   = newframedec(&u, 1<<12);
            while (framenext(&e, &out).ok) {}
            assert(!e.bad && !out.len && !e.len);
        }
    }

    // Lines over breaking streams
    {
        Arena t = a;
        Link  x = newlink(&t, 0x11, 1<<14, 1<<10);
        Link  y = newlink(&t, 0x22, 1<<14, 1<<10);
        Slice<Str> lines = {};
        for (Str rest = src; rest.len;) {
            Cut c = cut(rest, '\n');
            lines = push(&t, lines, c.head);
            rest  = c.tail;
        }
        Buf got(&t, src.len + 64);
        iz  sent = 0, cutat = 0;
        i64 now  = 0;
        b32 done = 0;  // nothing left to say on either side
        for (i32 stream = 0; stream < 64; stream++) {
            FrameDec dx = newframedec(&t, 1<<14);  // what the links take
            FrameDec dy = newframedec(&t, 1<<14);
            linkconnect(&x, now);
            linkconnect(&y, now);
            for (; sent<lines.len && linksend(&x, lines[sent]); sent++) {}
            // Run until the stream breaks after so many bytes
            iz budget = src.len ? 1 + src[cutat++ % src.len]*7 : 1<<20;
            b32 quiet = 0;
            for (i32 round = 0; round < 100 && budget>0 && !quiet; round++) {
                now += 3000000;
                linktick(&x, now);
                linktick(&y, now);
                Arena u   = t;
                Str   ox  = linkout(&x, &u, now);
                Str   oy  = linkout(&y, &u, now);
                quiet = !ox.len && !oy.len && sent==lines.len;
                ox = takehead(ox, ox.len<budget ? ox.len : budget);
                budget -= ox.len;
                for (FrameIn f; (f = framenext(&dy, &ox)).ok;) {
                    if (linkframe(&y, &f, now) == LINK_DATA) {
                        print(&got, f.payload);
                        putbyte(&got, '\n');
                    }
                }
                for (FrameIn f; (f = framenext(&dx, &oy)).ok;) {
                    linkframe(&x, &f, now);
                }
                assert(!dx.bad && !dy.bad);
                for (; sent<lines.len && linksend(&x, lines[sent]); sent++) {}
            }
            if (quiet) {
                done = 1;
                break;
            }
        }
        // Everything sent arrived once, in order
        Buf want(&t, src.len + 64);
        for (iz i = 0; i < sent; i++) {
            print(&want, lines[i]);
            putbyte(&want, '\n');
        }
        Str g = finish(&got);
        Str w = finish(&want);
        assert(g.len <= w.len && takehead(w, g.len)==g);
        assert(!done || g==w);
    }
}

static b32 fuzzutf8(Str s)  // whether s is valid UTF-8
{
    for (iz i = 0; i < s.len;) {
        i32 c = utf8strict(s, &i);
        if (c<0 || (c>=0xd800 && c<=0xdfff)) {
            return 0;
        }
    }
    return 1;
}

static void fuzzhits(Arena *a, WebHits *h, i32 max)
{
    assert(h->hits.len <= max);
    for (iz i = 0; i < h->hits.len; i++) {
        WebHit *x = &h->hits[i];
        assert(weburl(a, x->url) == x->url);
        assert(webclean(a, x->title) == x->title);
        assert(webclean(a, x->excerpt) == x->excerpt);
        assert(x->title.len<=WEB_TITLEMAX && x->excerpt.len<=WEB_EXCERPTMAX);
    }
    assert(webclean(a, h->summary) == h->summary);
}

static void fuzzweb(Arena a, Str src)
{
    // A page: clean text, its links' targets canonical
    {
        Arena   t    = a;
        Str     html = webutf8(&t, "text/html", src, 1);
        WebPage p    = webhtml(&t, html, "https://example.com/a/b");
        assert(fuzzutf8(html));
        assert(webclean(&t, p.text) == p.text);
        assert(webclean(&t, p.title) == p.title);
        assert(p.title.len <= WEB_TITLEMAX);
        // Without these in the input, every "](" opens a link's target,
        // which runs to some ')' before any whitespace
        b32 own = find(src, "]")<0 && find(src, "&")<0;
        for (Str rest = p.text; own;) {
            iz k = find(rest, "](");
            if (k < 0) {
                break;
            }
            rest = cuthead(rest, k+2);
            b32 ok = 0;
            for (iz e = 0; e<rest.len && !whitespace(rest[e]) && !ok; e++) {
                Str url = takehead(rest, e);
                ok = rest[e]==')' && weburl(&t, url)==url;
            }
            assert(ok);
        }
        Str latin = webutf8(&t, "text/plain; charset=latin1", src, 0);
        assert(fuzzutf8(latin));
    }

    // URLs from text, refusals and redirects
    for (i32 bare = 0; bare < 2; bare++) {
        Arena      t    = a;
        Slice<Str> urls = weburls(&t, src, bare);
        for (iz i = 0; i < urls.len; i++) {
            assert(weburl(&t, urls[i]) == urls[i]);
            assert(iniurlclean(&t, urls[i]) == urls[i]);
            webrefuse(urls[i]);
        }
    }
    {
        Arena t   = a;
        Str   url = weburl(&t, src);
        if (url.data) {
            assert(weburl(&t, url) == url);
            assert(iniurlclean(&t, url) == url);
            webrefuse(url);
        }
        WebRedirect r = webredirect(&t, "https://example.com/a", src);
        if (r.kind == WEBR_BAD) {
            assert(r.why.len);
        } else {
            assert(weburl(&t, r.url) == r.url);
            assert(!webrefuse(r.url).data);
        }
        webkind(src, src);
    }

    // Search replies
    {
        Arena   t      = a;
        WebHits hits[] = {
            websearxng(&t, src, 5),
            webexa(&t, src, 5),
            webanthropic(&t, src, 5),
        };
        WebHits acc = {};
        for (iz i = 0; i < countof(hits); i++) {
            fuzzhits(&t, hits+i, 5);
            webmerge(&t, &acc, hits[i], 8);
        }
        fuzzhits(&t, &acc, 8);
        websearchtext(&t, src, "S", "D", &acc, webnonce(&t, 1));
    }

    // A window of a page's text
    {
        Arena t    = a;
        Str   text = webclean(&t, src);
        iz    off  = src.len<2 ? 0 : (src[0] | src[1]<<8) % (text.len ? text.len : 1);
        iz    max  = src.len<3 ? 64 : 1 + src[2]*5;
        Str   out  = webfetchtext(&t, "https://example.com/", "T", "text/plain",
                                  text, off, max, "nonce123");
        assert(fuzzutf8(out));
    }
}

#if FUZZ_IMAGE
static void fuzzimage(Arena a, Str src)
{
    ImageKind k   = imagekind(a, src);
    byte     *beg = a.beg;
    Image     p   = imgdecode(&a, src, 1<<20);
    assert(!p.err.data == !!p.rgba);
    if (!p.rgba) {
        assert(a.beg == beg);
        return;
    }
    i32 w = p.width;
    i32 h = p.height;
    assert(w>0 && h>0 && (i64)w*h<=1<<20);
    assert(k.mime.data && k.w==w && k.h==h);
    shrink(&a, p.rgba, w, h, (w+1)/2, (h+1)/2);

    Str   png = pngencodez(&a, p.rgba, w, h, 0);
    Image q   = imgdecode(&a, png, 1<<20);
    assert(q.rgba && q.width==w && q.height==h);
    assert(!__builtin_memcmp(p.rgba, q.rgba, (uz)w*h*4));
}
#endif

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
        #elif FUZZ_FRAMES
        fuzzframes(a, {src, len});
        #elif FUZZ_IMAGE
        fuzzimage(a, {src, len});
        #elif FUZZ_WEB
        fuzzweb(a, {src, len});
        #else
        fuzzjson(a, {src, len});
        #endif
        free(src);
    }
    return 0;
}
