// AFL++ fuzz layer: the JSON reader and writer, or with -DFUZZ_EVENTS the
// session log's open, recovery, and replay over a one-file stub platform,
// or with -DFUZZ_PROVIDER the engine's parsers of untrusted text, or with
// -DFUZZ_FRAMES the framing of remote links (frame.cpp), or with
// -DFUZZ_IMAGE an image's intake: its kind, its decode through Wuffs
// (image.cpp, wuffs.c), shrinking and PNG encoding, or with -DFUZZ_WEB
// the web tools' text work (web.cpp), or with -DFUZZ_PDF the PDF reader
// (pdf.cpp), or with -DFUZZ_PDFENC its text encodings (pdfenc.cpp), or
// with -DFUZZ_PDFIMG its image decoding (pdfimage.cpp): CCITT fax,
// samples to RGBA, and showing one as a PNG or a JPEG. Every target
// links Wuffs, from the release file CMake fetched:
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
// -DFUZZ_IMAGE, -DFUZZ_WEB, -DFUZZ_PDF, -DFUZZ_PDFENC or -DFUZZ_PDFIMG
// for the other targets. FUZZ_PDFIMG takes its parameters from the first
// 16 bytes; seeds are test/pdfimage_test.cpp's fax fixtures after 16
// bytes that pick their kind (byte 0: K+1, then the align and BlackIs1
// bits; bytes 1 and 2: the columns, little endian; byte 3: twice the
// rows, odd to count). The PDF target has a dictionary, used with
// AFL++'s own:
//   $ afl-fuzz -i i -o o -x tests/pdffuzz.dict \
//         -x /opt/homebrew/share/afl/dictionaries/pdf.dict ./fuzzpdf
// With -DPROSPERO_ARENACHECK=1 the arena is checked for aliasing, and
// under ASan memory an arena gave back is poisoned.
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
// PDF: the old scan (pdfinfo) never faults. Every token consumes input,
// and tokens tile it, with only whitespace and comments between them.
// The objects parsed from the input print canonically, the same whether
// the input is taken as stable or copied from, and the print parses
// back and prints to identical bytes. In a scratch too small, a parse
// stops for want of memory, never trapping, or prints the same. Every
// decoder (Flate, LZW in both EarlyChange modes, ASCIIHex, ASCII85,
// RunLength, TIFF and PNG predictors), with parameters and a cap the
// input picks, stays within its cap and its budget, and all but Flate
// decode a prefix of their input to a prefix of their whole output.
// A document loads, as not a PDF with nothing found, or with pages
// that are dicts, no more than PDF_PAGEMAX, and every object in its
// xref and its scan resolves and every stream decodes, within budgets
// that never go negative. A second read, at another offset, finds the
// same; in a scratch too small, it finds the same or runs out of memory,
// never trapping. Its text (pdfread) is valid UTF-8 with no controls
// but newline and tab, at most PDF_TEXTMAX, its page markers 1..n at
// the lines pageline names, every placement on a page read and a line
// of it (pdfoutbad); a refusal leaves perm unchanged; two reads at
// different offsets are byte-identical, and in a scratch too small the
// read is the same or cut short for memory. Up to four of its images,
// shown (pdfimage) within limits the input picks, fail with a reason
// leaving their arena as it was, or give a PNG or a JPEG within those
// limits that decodes to the size it states, the same at another
// offset. A window of lines the input picks gets a pick (pdfpick) of at
// most PDF_IMAGES distinct images inside it, in line order, none small
// or undecodable, their shares within PDF_SHOWPIXELS and PDF_SHOWBYTES;
// the window is cut only past PDF_IMAGES, and every image left in it is
// one picked. Inputs over 64 KiB are
// skipped, so the budgets that grow with the input are only fuzzed at
// their minimums.
//
// PDF encodings: a text string decodes to clean text (below) costing
// exactly its untrimmed length, all of it spaces outside the result, or
// to nothing in an arena too small for it; the emitter writes only clean
// text from any value; glyph names give at most PDF_UNIMAX scalars; the
// Cork fallback gives 0 or the code's table entry; UTF-16 decoding
// always advances; and standard widths and tables come from the tables.
// Clean text is valid UTF-8 without NUL, controls, DEL, C1, NBSP, soft
// hyphen, ligature forms or noncharacters.
//
// PDF images: CCITT fax data, with the parameters the input picks,
// decodes to exactly its rows of bits, or fails with a reason leaving
// the arena as it was, and any prefix of the data decodes the same rows
// as far as both decoded cleanly. Samples of any space, depth, Decode,
// colour key and soft mask the input picks, the fax rows among them,
// convert at a size the input picks to opaque pixels of that size and
// nothing more, or fail leaving the arena as it was; shown as a PNG they
// decode back to the size reported, and a JPEG shown passes through or
// converts the same way, in scratch of a size the input picks (byte 15's
// top bit: tight), failing for want of it without touching the result
// arena.
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
static i64  os_utcoffset(Os *, i64) { return 0; }
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
static Str   os_hostids(Os *, Arena *) { return {}; }
static Proc *os_spawn(Os *, Spawn *, Arena) { return 0; }
static Proc *os_spawnio(Os *, Spawn *, Arena) { return 0; }
static b32 os_launch(Os *, Arena, Slice<Str>, Str, Slice<Str>) { return 0; }
static b32   os_procwrite(Os *, Proc *, Str) { return 0; }
static void  os_kill(Os *, Proc *, b32) {}
static void  os_letgo(Os *, Proc *) {}
static void  os_procfree(Os *, Proc *) {}
static Proc *os_ptyspawn(Os *, Spawn *, PtySize, iz, Arena) { return 0; }
static Str   os_ptypeek(Os *, Proc *, iz) { return {}; }
static void  os_ptytake(Os *, Proc *, iz) {}
static b32   os_ptyresize(Os *, Proc *, PtySize) { return 0; }
static b32   os_ptybusy(Os *, Proc *) { return 0; }
static b32   os_ptyable(Os *) { return 0; }
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
static b32   os_pidalive(Os *, i32) { return 0; }
static b32   os_fromterminal(Os *) { return 0; }
static Str   os_hostname(Os *, Arena *) { return {}; }
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
            webkeenable(&t, src, 5),
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

#if FUZZ_PDFENC
static b32 fuzzpdfclean(Str s)
{
    for (iz i = 0; i < s.len;) {
        i32 c = utf8strict(s, &i);
        if (c<0x20 || (c>=0x7f && c<=0xa0) || c==0xad ||
            (c>=0xd800 && c<=0xdfff) || (c>=0xfb00 && c<=0xfb06) ||
            (c&0xfffe)==0xfffe) {
            return 0;
        }
    }
    return 1;
}

static void fuzzpdfenc(Arena a, Str src)
{
    {
        Arena t    = a;
        u8   *beg  = (u8 *)t.beg;
        Str   r    = pdftextstr(&t, src);
        u8   *end  = (u8 *)t.beg;
        iz    cost = end - beg;
        assert(fuzzpdfclean(r));
        assert(cost <= 3*src.len);
        if (r.len) {
            assert(r.data>=beg && r.data+r.len<=end);
            assert(r[0]!=' ' && r[r.len-1]!=' ');
        }
        for (u8 *p = beg; p < end; p++) {
            assert((p>=r.data && p<r.data+r.len) || *p==' ');
        }

        // Too small an arena: nothing, and never a trap
        Arena small = carve(&t, cost/2);
        Str   z     = pdftextstr(&small, src);
        assert(!cost || !z.len);
    }

    {
        u32 cp[PDF_UNIMAX];
        i32 n = pdfglyphname(src, cp);
        assert(n>=0 && n<=PDF_UNIMAX);
        for (i32 i = 0; i < n; i++) {
            assert(cp[i]<=0x10ffff && (cp[i]<0xd800 || cp[i]>0xdfff));
        }
        i32 code = src.len ? src[0] : 0;
        u32 k    = pdfcork(src, code);
        assert(!k || k==PDF_CORK[code]);
    }

    for (b32 le = 0; le < 2; le++) {
        for (iz i = 0; i < src.len;) {
            iz  was = i;
            i32 c   = pdfutf16(src, &i, le);
            assert(i>was && i<=src.len);
            assert(c>=0 && c<=0x10ffff && (c<0xd800 || c>0xdfff));
        }
    }

    {
        Arena t = a;
        Buf   b(&t, 64);
        for (iz i = 0; i+8 <= src.len; i += 8) {
            i64 v;
            memcpy(&v, src.data+i, 8);
            pdfputcp(&b, v);
            pdfputcp(&b, (i32)v);
            pdfputcp(&b, (u16)v);
            pdfputcp(&b, (u8)v);
        }
        assert(fuzzpdfclean(finish(&b)));
    }

    u16 const *ws[] = {PDF_HELVW, PDF_HELVBW, PDF_TIMESW,
                       PDF_TIMESBW, PDF_TIMESIW, PDF_TIMESBIW};
    for (i32 c = -1; c <= 256; c++) {
        i32 w  = pdfstdwidth(src, c);
        b32 ok = !w || w==600;
        for (iz i = 0; !ok && c>=32 && c<=126 && i<countof(ws); i++) {
            ok = w == ws[i][c-32];
        }
        assert(ok);
    }
    u16 const *e = pdfbuiltin(src);
    assert(!e || e==PDF_SYMBOL || e==PDF_ZAPF);
    e = pdfencoding(src);
    assert(!e || e==PDF_STDENC || e==PDF_WINANSI ||
           e==PDF_MACROMAN || e==PDF_PDFDOC);
}
#endif

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

#if FUZZ_PDF
// The objects parsed from src, in a reader over scratch, printed
// canonically, space-separated, with null data if the parse stopped.
static Str fuzzpdfprint(Arena *perm, Arena scratch, Str src, b32 stable)
{
    Pdf    p = pdfstart(scratch, src, {});
    PdfLex x = {&p, &p.keep, src, 0, stable, 0};
    Buf    b(perm, 256);
    for (i32 n = 0; !p.why.data && pdfpeek(&x)>=0; n++) {
        if (n) putbyte(&b, ' ');
        pdfprintobj(&b, pdfparse(&x));
    }
    return p.why.data ? Str{} : finish(&b);
}

// One filter over in, in a reader over a copy of scratch whose budgets
// are in's, so that every run gets the same caps: its output in perm.
static PdfData fuzzpdffilter(Arena *perm, Arena scratch, i32 id, Str parms,
                             Str in, iz cap)
{
    Pdf     p = pdfstart(scratch, in, {});
    PdfLex  x = {&p, &p.keep, parms, 0, 1, 0};
    PdfObj  d = pdfparse(&x);
    i64     b = p.decoded;
    PdfData r = pdffilter(&p, &p.page, id, d, in, cap);
    assert(r.data.len<=cap && p.decoded>=0);
    assert(id==PDFF_CRYPT || r.data.len<=b-p.decoded);  // charged
    if (id!=PDFF_FLATE && id!=PDFF_LZW) {               // no predictor
        assert(!r.cut || r.data.len==cap || p.why.data);
    }
    r.data = p.why.data ? Str{} : clone(perm, r.data);
    return r;
}

// The decoders over the input past its first six bytes, which pick a
// cap, a point to cut the input, and the predictor's parameters.
static void fuzzpdfdecode(Arena *perm, Arena scratch, Str src)
{
    if (src.len < 6) {
        return;
    }
    u8 *h      = src.data;
    Str in     = cuthead(src, 6);
    iz  cap    = 1 + (h[1]<<8 | h[2]) * (h[0]&1 ? 64 : 1);
    iz  cut    = in.len * h[3] / 255;
    i32 preds[] = {1, 2, 10, 11, 12, 13, 14, 15};
    i32 bpcs[]  = {1, 2, 4, 8, 16, 8, 3, 8};
    Buf parms(perm, 128);
    print(&parms, "<</Predictor ");
    print(&parms, (i64)preds[h[0]>>1 & 7]);
    print(&parms, " /Colors ");
    print(&parms, (i64)(h[4]&3) + 1);
    print(&parms, " /BitsPerComponent ");
    print(&parms, (i64)bpcs[h[4]>>2 & 7]);
    print(&parms, " /Columns ");
    print(&parms, (i64)h[5] + (h[4]&0x80 ? 0 : 1));
    print(&parms, " /EarlyChange ");
    print(&parms, (i64)(h[0]>>4 & 1));
    print(&parms, ">>");
    Str pd = finish(&parms);

    // Each within its cap, and a prefix decoding to a prefix. Outputs,
    // up to two caps' worth, are given back each round.
    i32 ids[] = {PDFF_FLATE, PDFF_LZW, PDFF_AHX, PDFF_A85, PDFF_RL, PDFF_CRYPT};
    for (i32 i = 0; i < countof(ids); i++) {
        Arena   t    = *perm;
        PdfData full = fuzzpdffilter(&t, scratch, ids[i], pd, in, cap);
        if (ids[i]==PDFF_FLATE || !full.data.data) {
            continue;
        }
        PdfData part = fuzzpdffilter(&t, scratch, ids[i], pd, takehead(in, cut), cap);
        assert(startswith(full.data, part.data));
    }

    // A chain the bytes pick, with the parameters for each
    {
        static Str const names[] = {
            "/FlateDecode", "/LZW", "/AHx", "/A85", "/RL", "/DCT", "/Crypt", "/Foo",
        };
        Buf d(perm, 256);
        print(&d, "<</Filter[");
        for (i32 i = 0; i < (h[3]&3)+1; i++) {
            print(&d, names[h[5-i]&7]);
        }
        print(&d, "]/DecodeParms ");
        print(&d, pd);
        print(&d, ">>");
        Str     text = finish(&d);
        Pdf     p    = pdfstart(scratch, in, {});
        PdfLex  x    = {&p, &p.keep, text, 0, 1, 0};
        PdfData r    = pdfdecode(&p, &p.page, pdfparse(&x), in, cap);
        assert(r.data.len<=PDF_STREAMMAX && p.decoded>=0);
        assert(r.image || r.data.len<=cap);
    }
}

static u64 fuzzhash(u64 h, Str s)
{
    for (iz i = 0; i < s.len; i++) {
        h = (h ^ s.data[i]) * 1111111111111111111u;
    }
    return h;
}

// An object, digested without allocating: its kind and contents, a
// stream's dict and raw data.
static u64 fuzzpdfhash(u64 h, PdfObj o)
{
    h = (h ^ (u64)o.kind) * 1111111111111111111u;
    switch (o.kind) {
    case PDFO_BOOL:
    case PDFO_INT:
    case PDFO_REAL:
    case PDFO_REF:
        h = (h ^ (u64)o.i) * 1111111111111111111u;
        break;
    case PDFO_STR:
    case PDFO_NAME:
        h = fuzzhash(h, o.s);
        break;
    case PDFO_ARRAY:
        for (i32 i = 0; i < o.len; i++) h = fuzzpdfhash(h, o.items[i]);
        break;
    case PDFO_DICT:
        for (i32 i = 0; i < 2*o.len; i++) h = fuzzpdfhash(h, o.items[i]);
        break;
    case PDFO_STREAM:
        h = fuzzhash(fuzzpdfhash(h, o.st->dict), o.st->raw);
        break;
    }
    return h;
}

// Every object in a trie of slots resolved, each stream decoded too.
static u64 fuzzpdfslots(Pdf *p, PdfSlot *s, u64 h)
{
    if (!s) {
        return h;
    }
    for (i32 i = 0; i < 4; i++) {
        h = fuzzpdfslots(p, s->child[i], h);
    }
    PdfObj o = pdfresolve(p, s);
    h = fuzzpdfhash(h ^ (u64)s->num, o);
    if (o.kind == PDFO_STREAM) {
        Arena   save = p->page;  // given back after
        PdfData d    = pdfstream(p, &p->page, o, 0);
        assert(p->decoded>=0 && (d.image || d.data.len<=PDF_STREAMMAX));
        h = fuzzhash(h ^ (u64)(d.image<<1 | d.cut), d.data);
        p->page = save;
    }
    return h;
}

// The document read whole, from a reader over scratch: loaded, its
// pages walked, every object the xref and the scan know resolved and
// every stream decoded, within budgets. What it gave, digested.
struct FuzzPdfDoc {
    u64 digest;
    i32 status;
    iz  pages;
    i32 damage;
    b32 recovered;
    Str why;
};

static FuzzPdfDoc fuzzpdfdoc(Arena scratch, Str src)
{
    FuzzPdfDoc r = {};
    Pdf        p = pdfstart(scratch, src, {});
    r.status = pdfload(&p);
    if (r.status == PDFR_NOTPDF) {
        assert(!p.xref && !p.index && !p.why.data);
        return r;
    }
    PdfPages g = pdfpages(&p);
    assert(g.pages.len <= PDF_PAGEMAX);
    u64 h = 0;
    for (iz i = 0; i < g.pages.len; i++) {
        PdfPage pg = g.pages.data[i];
        assert(pg.page.kind == PDFO_DICT);
        assert(pg.rotate>=0 && pg.rotate<360 && pg.rotate%90==0);
        h = fuzzpdfhash(h, pg.page);
    }
    h = fuzzpdfslots(&p, p.xref, h);
    h = p.index ? fuzzpdfslots(&p, p.index->objs, h) : h;
    assert(p.work>=0 && p.decoded>=0 && p.scanned>=0 && p.hops==0);
    r.digest    = h;
    r.pages     = g.pages.len;
    r.damage    = p.damage;
    r.recovered = p.recovered;
    r.why       = p.why;
    return r;
}

static b32 fuzzpdfsame(FuzzPdfDoc a, FuzzPdfDoc b)
{
    return a.digest==b.digest && a.status==b.status && a.pages==b.pages &&
           a.damage==b.damage && a.recovered==b.recovered && a.why==b.why;
}

// Whether two reads gave the same results, byte for byte.
static b32 fuzzpdfsameout(PdfOut *a, PdfOut *b)
{
    if (a->status!=b->status || !(a->why==b->why) || a->pages!=b->pages ||
        !(a->text==b->text) || a->recovered!=b->recovered ||
        a->pageline.len!=b->pageline.len || a->places.len!=b->places.len) {
        return 0;
    }
    for (iz i = 0; i < a->pageline.len; i++) {
        if (a->pageline.data[i] != b->pageline.data[i]) return 0;
    }
    for (iz i = 0; i < a->places.len; i++) {
        PdfPlace x = a->places.data[i];
        PdfPlace y = b->places.data[i];
        if (!(x.label==y.label)) return 0;
        x.label = y.label = {};
        if (__builtin_memcmp(&x, &y, sizeof(x))) return 0;
    }
    return 1;
}

static void fuzzpdfimages(Arena, Str, PdfOut *);

static void fuzzpdf(Arena a, Str src)
{
    pdfinfo(a, src);

    // Tokens tile the input
    {
        Pdf    p    = pdfstart(a, src, {});
        PdfLex x    = {&p, &p.keep, src, 0, 1, 0};
        iz     prev = 0;
        for (;;) {
            PdfTok k = pdflex(&x);
            assert(k.beg>=prev && k.end>=k.beg && k.end<=src.len);
            PdfLex gap = {&p, &p.op, {src.data+prev, k.beg-prev}, 0, 1, 0};
            assert(pdfpeek(&gap) < 0);
            if (!k.kind) {
                assert(k.end==src.len && !p.why.data);
                break;
            }
            assert(k.end > k.beg);
            prev = k.end;
        }
    }

    // Parse, print, parse again and print the same
    Arena perm  = carve(&a, 16<<20);
    Str   once  = fuzzpdfprint(&perm, a, src, 1);
    assert(once.data);
    Str   copy  = fuzzpdfprint(&perm, a, src, 0);
    assert(copy == once);
    Str   twice = fuzzpdfprint(&perm, a, once, 1);
    assert(twice == once);

    // In a small scratch, the same or out of memory
    Arena tiny  = carve(&a, 256 + (src.len ? src[0]*64 : 0));
    Str   small = fuzzpdfprint(&perm, tiny, src, 1);
    assert(!small.data || small==once);

    fuzzpdfdecode(&perm, a, src);

    // The document, read the same at another offset, and in a scratch
    // too small the same or out of memory
    FuzzPdfDoc d1 = fuzzpdfdoc(a, src);
    Arena      at = a;
    allocbytes(&at, 24 + (src.len ? src[0] : 0));
    FuzzPdfDoc d2 = fuzzpdfdoc(at, src);
    assert(fuzzpdfsame(d1, d2));
    Arena      lean = a;
    lean.end = lean.beg + (1<<14) + (src.len>1 ? src[1]<<12 : 0);
    FuzzPdfDoc d3 = fuzzpdfdoc(lean, src);
    assert(fuzzpdfsame(d1, d3) || d3.why=="out of memory");

    // The text, read twice at different offsets in scratches of one size,
    // the same and valid, perm as it was after a refusal; and in a
    // scratch too small, the same or cut short for memory
    Arena   rest = a;
    Arena   p1 = carve(&rest, 8<<20);
    byte   *b1 = p1.beg;
    Arena   s1 = rest;
    s1.end = s1.beg + (24<<20);
    PdfOut  r1 = pdfread(&p1, s1, src, 0);
    assert(!pdfoutbad(&r1, &s1).data);
    assert(r1.status<=PDFR_PARTIAL || p1.beg==b1);
    Arena   s2 = rest;  // past r1's output
    allocbytes(&s2, 40 + (src.len ? src[0] : 0));
    Arena   p2 = carve(&s2, 8<<20);
    s2.end = s2.beg + (24<<20);
    PdfOut  r2 = pdfread(&p2, s2, src, 0);
    assert(fuzzpdfsameout(&r1, &r2));
    Arena   s3 = rest;
    Arena   p3 = carve(&s3, 8<<20);
    s3.end = s3.beg + (1<<15) + (src.len>2 ? src[2]<<14 : 0);
    PdfOut  r3 = pdfread(&p3, s3, src, 0);
    Arena   c3 = s3;  // past r3's output, for the check's scratch
    c3.end = rest.end;
    assert(!pdfoutbad(&r3, &c3).data);
    assert(fuzzpdfsameout(&r1, &r3) || r3.why=="out of memory" ||
           find(r3.text, "out of memory]")>=0 ||
           (r3.status==PDFR_NOPAGES && r1.status!=PDFR_NOPAGES));

    fuzzpdfimages(rest, src, &r1);
}

// Up to 4 of a read's placements shown (pdfimage), within limits the
// input picks, the first twice at different offsets; then a window of
// lines the input picks, and its pick (pdfpick).
static void fuzzpdfimages(Arena rest, Str src, PdfOut *r)
{
    u64 h      = fuzzhash(0x9e3779b97f4a7c15u, src);
    i64 edge   = 8 + (i64)(h % 700);
    i64 pixels = 64 + (i64)(h>>10 & 0x3ffff);
    iz  maxb64 = 256 + (iz)(h>>30 & 0xfffff);
    iz  n      = r->places.len;
    Str first  = {};
    for (iz i = 0; i<4 && i<n; i++) {
        PdfPlace *pl   = r->places.data + (i*n/4 + (iz)(h>>50)%(n/4 + 1)) % n;
        Arena     t    = rest;  // 19 MiB of 40, twice with the offset below
        Arena     perm = carve(&t, 2<<20);
        byte     *beg  = perm.beg;
        t.end = t.beg + (17<<20);
        PdfShown  s    = pdfimage(&perm, t, src, pl, edge, pixels, maxb64);
        assert(!s.data.data == !!s.err.len);
        if (!s.data.data) {
            assert(perm.beg == beg);
            continue;
        }
        assert(s.mime=="image/png" || s.mime=="image/jpeg");
        assert(s.w>=1 && s.h>=1 && s.w<=edge && s.h<=edge && (i64)s.w*s.h<=pixels);
        assert(base64len(s.data.len) <= maxb64);
        Image p = imgdecode(&t, s.data, pixels);
        assert(p.rgba && p.width==s.w && p.height==s.h);
        if (i == 0) {
            first = s.data;
            Arena u = rest;  // past the first: another offset, room for
            allocbytes(&u, (19<<20) + 8 + (src.len ? src[src.len-1] : 0));
            Arena    perm2 = carve(&u, (2<<20) + 64);  // ...its alignments
            u.end = u.beg + (17<<20) + 64;
            PdfShown s2    = pdfimage(&perm2, u, src, pl, edge, pixels, maxb64);
            assert(s2.data == first && s2.mime == s.mime);
        }
    }

    // A window and its pick
    iz lines = 0;
    for (iz i = 0; i < r->text.len; i++) {
        lines += r->text[i] == '\n';
    }
    iz wfirst = 1 + (iz)(h>>8 & 0xffff) % (lines + 1);
    iz wlast  = wfirst + (iz)(h>>24 & 0xffff) % (lines + 1);
    // Limits wider than PDF_SHOWPIXELS and PDF_SHOWBYTES, to be shared
    pixels  = 64 + (i64)(h>>10 & 0x7fffff);
    maxb64  = 256 + (iz)(h>>33 & 0xffffff);
    PdfPick k = pdfpick(rest, r->places, wfirst, wlast, edge, pixels, maxb64);
    assert(k.n>=0 && k.n<=PDF_IMAGES);
    assert(k.last<=wlast && (!k.n || k.last>=wfirst) && (k.cut ? k.n==PDF_IMAGES : k.last==wlast));
    assert(k.edge==edge && k.pixels<=pixels && k.maxb64<=maxb64);
    assert(k.pixels*(k.n ? k.n : 1) <= PDF_SHOWPIXELS);
    assert(k.maxb64*(k.n ? k.n : 1) <= PDF_SHOWBYTES);
    for (i32 i = 0; i < k.n; i++) {
        assert(k.at[i]>=0 && k.at[i]<n);
        PdfPlace *pl = r->places.data + k.at[i];
        assert(pl->line>=wfirst && pl->line<=k.last);
        assert(pl->w>=PDF_PICKMIN && pl->h>=PDF_PICKMIN);
        assert(pl->image!=PDFI_JPX && pl->image!=PDFI_JBIG2);
        assert(!i || r->places.data[k.at[i-1]].line<=pl->line);
        for (i32 j = 0; j < i; j++) {
            PdfPlace *q = r->places.data + k.at[j];
            assert(q->id!=pl->id || (pl->mask && __builtin_memcmp(q->fill, pl->fill, 3)));
        }
    }
    // Every image the window shows is a pick, or one of a pick's, but
    // those past the most on the last pick's line
    iz full = k.n==PDF_IMAGES ? r->places.data[k.at[k.n-1]].line : -1;
    for (iz i = 0; i<n; i++) {
        PdfPlace *pl = r->places.data + i;
        if (pl->line==full || pl->line<wfirst || pl->line>k.last ||
            pl->w<PDF_PICKMIN || pl->h<PDF_PICKMIN ||
            pl->image==PDFI_JPX || pl->image==PDFI_JBIG2) {
            continue;
        }
        b32 found = 0;
        for (i32 j = 0; j<k.n && !found; j++) {
            PdfPlace *q = r->places.data + k.at[j];
            found = q->id==pl->id && (!pl->mask || !__builtin_memcmp(q->fill, pl->fill, 3));
        }
        assert(found);
    }
}
#endif

#if FUZZ_PDFIMG
// Pixels converted, checked opaque and of the size asked for, else the
// arena as it was.
static void fuzzpixels(Arena *a, PdfSamples *s, i32 tw, i32 th, i64 *work)
{
    byte *beg = a->beg;
    Image p   = pdfpixels(a, s, tw, th, work);
    assert(!p.err.data == !!p.rgba);
    if (!p.rgba) {
        assert(a->beg == beg);
        return;
    }
    assert(p.width==tw && p.height==th && a->beg-beg==(iz)tw*th*4);
    for (iz i = 0; i < (iz)tw*th; i++) {
        assert(p.rgba[4*i+3] == 255);
    }
}

static void fuzzshown(Arena a, byte *beg, PdfShown r)
{
    assert(!r.data.data == !!r.err.data);
    assert(r.data.data || a.beg==beg);
    if (r.data.data) {
        ImageKind k = imagekind(a, r.data);
        assert(k.mime == r.mime && k.w==r.w && k.h==r.h);
    }
}

static void fuzzpdfimg(Arena a, Str src)
{
    if (src.len < 16) {
        return;
    }
    u8 *p    = src.data;
    Str body = cuthead(src, 16);

    // CCITT
    CcittParams cp = {};
    cp.k        = (p[0]&3) - 1;
    cp.align    = p[0]>>2 & 1;
    cp.blackis1 = p[0]>>3 & 1;
    cp.columns  = (p[1] | p[2]<<8) & 0x3fff;
    cp.rows     = p[3]&1 ? 0 : p[3]>>1;
    {
        Arena t    = a;
        byte *beg  = t.beg;
        i64   work = (i64)1<<24;
        Ccitt c    = pdfccitt(&t, body, cp, 1<<22, &work);
        if (!c.bits.data) {
            assert(c.err.data && t.beg==beg);
        } else {
            iz stride = ((iz)c.cols + 7)/8;
            assert(c.rows>=1 && c.good>=0 && c.good<=c.rows);
            assert(c.bits.len==c.rows*stride && t.beg-beg==c.bits.len);
            assert(!cp.rows || c.rows==cp.rows);

            iz   n    = body.len * p[4] / 256;
            u8  *cpy  = allocbytes(&t, n);  // so that nothing reads past it
            copybytes(cpy, body.data, n);
            i64  w2   = (i64)1<<24;
            Arena u   = t;
            Ccitt d   = pdfccitt(&u, {cpy, n}, cp, 1<<22, &w2);
            if (d.bits.data) {
                iz good = d.good<c.good ? d.good : c.good;
                assert(d.cols == c.cols);
                assert(!__builtin_memcmp(d.bits.data, c.bits.data, (uz)(good*stride)));
            }

            PdfSamples s = {};
            s.data = c.bits;
            s.w    = c.cols;
            s.h    = c.rows;
            s.bpc  = 1;
            if (p[5] & 1) {
                s.ndecode   = 2;
                s.decode[0] = 1;
            }
            i32 tw = 1 + (i32)((p[6] | p[7]<<8) % s.w);
            i32 th = 1 + (i32)((p[8] | p[9]<<8) % s.h);
            fuzzpixels(&t, &s, tw, th, &work);
        }
    }

    // Samples of every kind
    static i32 const depths[] = {1, 2, 4, 8, 16};
    static f32 const odd[]    = {0, 1, -1, 0.5f, 1e30f, -1e30f, 1.0f/0.0f, 0.0f/0.0f};
    PdfSamples s = {};
    PdfSamples m = {};
    s.data    = body;
    s.w       = 1 + (p[9] | (p[10]&7)<<8);
    s.h       = 1 + p[11];
    s.bpc     = depths[p[12]%5];
    s.cs      = p[12]/5 % 7;  // one past the last
    s.ncomp   = 1 + p[13]%5;
    s.base    = (p[13]>>3) % 5;
    s.hival   = p[14] - 8;
    s.palette = body;
    s.mask    = p[15]>>2 & 1;
    s.fill[0] = p[14];
    switch (p[15] & 3) {
    case 1:
        s.ndecode = 2*PDFIMG_COMPS;
        for (i32 i = 0; i < s.ndecode; i++) {
            s.decode[i] = (f32)(~i & 1);
        }
        break;
    case 2:
    case 3:
        s.ndecode = 2*PDFIMG_COMPS;
        for (i32 i = 0; i < s.ndecode; i++) {
            s.decode[i] = odd[(p[i%16] >> (i%3)) & 7];
        }
        break;
    }
    if (p[15]>>3 & 1) {
        s.nkey = 2*PDFIMG_COMPS;
        for (i32 i = 0; i < s.nkey; i++) {
            s.key[i] = p[i%16] * (i&1 ? 300 : 1);
        }
    }
    if (p[15]>>4 & 1) {
        m.data  = cuthead(body, body.len/2);
        m.w     = 1 + p[1]%40;
        m.h     = 1 + p[2]%40;
        m.bpc   = depths[p[3]%5];
        m.cs    = p[15]>>5 & 1 ? PDFCS_TINT : PDFCS_GRAY;
        m.ncomp = 1;
        s.smask = &m;
    }
    i32 tw = 1 + p[3] % s.w;
    i32 th = 1 + p[4] % s.h;
    i64 work = (i64)1<<24;
    fuzzpixels(&a, &s, tw, th, &work);

    // Tight scratch reaches every out-of-memory branch, pngfit's too
    iz    cap = p[15]>>7 ? (32<<10) + ((p[7] | p[8]<<8) & 0x3ff)*1024 + p[1] : 32<<20;
    Arena t   = a;
    Arena mem = carve(&t, cap);
    byte *beg = t.beg;
    work = (i64)1<<24;
    PdfShown r = pdfshowsamples(&t, mem, &s, 1 + p[5]%600, 1 + p[6]*256, 1<<20, &work);
    fuzzshown(t, beg, r);

    pdfjpeg(body);
    if (imagesniff(body) == IMAGE_JPEG) {
        t   = a;
        mem = carve(&t, cap);
        beg = t.beg;
        r   = pdfshowjpeg(&t, mem, &s, 1 + p[5]%600, 1 + p[6]*256, 1<<20);
        fuzzshown(t, beg, r);
    }
}
#endif

__AFL_FUZZ_INIT();

int main()
{
    __AFL_INIT();
    iz    cap = (iz)1<<26;
    byte *mem = (byte *)malloc((uz)cap);
    u8   *buf = __AFL_FUZZ_TESTCASE_BUF;
    #if PROSPERO_ARENACHECK
    arenablank(mem, mem+cap);  // poisoned until allocated, and checked
    #endif
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
        #elif FUZZ_PDF
        fuzzpdf(a, {src, len});
        #elif FUZZ_PDFENC
        fuzzpdfenc(a, {src, len});
        #elif FUZZ_PDFIMG
        fuzzpdfimg(a, {src, len});
        #else
        fuzzjson(a, {src, len});
        #endif
        free(src);
    }
    return 0;
}
