// Server-sent events: an incremental text/event-stream tokenizer
//
// Follows "event stream interpretation" from the WHATWG HTML spec, over
// raw bytes. Lines end in CR, LF, or CRLF; one leading BOM is skipped;
// "field: value" lines fill the pending event, and a blank line dispatches
// it. Bytes pass through undecoded, so a chunk may end anywhere, even
// inside a CRLF pair or a UTF-8 sequence, and the events never depend on
// where. Invalid UTF-8 is left for the consumer (the JSON reader).
//
// One tokenizer per response body, fed chunk by chunk:
//
//   Sse sse = newsse(perm, max);
//   ... for each chunk:
//       Arena tmp = scratch;
//       Slice<SseEvent> evs = ssefeed(&sse, chunk, &tmp);
//       ... handle evs in order, then give up if sse.err ...
//   ... at the end of the body:
//   b32 cutoff = sseend(&sse);
//
// Comments (":" lines) are returned too, since servers send them as pings
// (llama.cpp every 30 s). Anthropic's pings are ordinary "ping" events.
//
// This is free and unencumbered software released into the public domain.

enum {
    SSE_EVENT = 1,  // a dispatched event
    SSE_COMMENT,    // a comment line: a sign of life, otherwise ignorable
};

// Longest event ID kept. Every feed that dispatches copies the last ID,
// so a longer one is ignored, as the spec ignores one holding a NUL.
enum { SSE_IDMAX = 1<<8 };

struct SseEvent {
    Str type;  // the event field, or "message" if there was none
    Str data;  // data lines joined with \n, or the comment text
    Str id;    // last event ID, empty if none
    i32 kind;
};

struct Sse {
    Buf  line;     // partial line, awaiting its line ending
    Buf  data;     // pending event data, each line followed by \n
    Buf  type;     // pending event type
    Buf  id;       // last event ID, kept across events
    Str  idout;    // id as copied into the current ssefeed arena
    i64  retry;    // last valid retry field (milliseconds), or -1
    iz   max;      // longest line, and most data in one event
    i32  bom;      // BOM bytes matched at the stream start, 3 once past
    b32  cr;       // the last line ended in CR, so skip a following LF
    b32  inevent;  // fields since the last blank line
    b32  err;      // sticky: a line or an event was over max
};

// A tokenizer whose buffers live in perm. A line longer than max bytes,
// or an event with more than max bytes of data, is an error. Buffers
// start small and grow at most once, so the tokenizer never takes more
// than 3*max + 5 KiB of perm. The arena must outlive the tokenizer, and
// must not share memory with ssefeed's arena, as a by-value copy would.
static Sse newsse(Arena *perm, iz max)
{
    assert(max >= 0);

    Sse s   = {};
    s.line  = Buf(perm, 1<<10);
    s.data  = Buf(perm, 1<<10);
    s.type  = Buf(perm, 1<<5);
    s.id    = Buf(perm, SSE_IDMAX);  // never grows
    s.retry = -1;
    s.max   = max;
    return s;
}

// Append to one of the tokenizer's buffers. None ever holds over max+2
// bytes (data's final newline, or a non-BOM's first two bytes under a
// tiny cap), so growth goes straight there, and only once: no chain of
// abandoned copies is left behind in perm.
static void sseprint(Sse *s, Buf *b, Str v)
{
    iz limit = s->max + 2;
    assert(v.len <= limit - b->len);
    if (b->cap-b->len < v.len) {
        reserve(b, limit - b->len);
    }
    print(b, v);
}

// Whether two distinct arenas could hand out the same memory: their free
// space overlaps, or both may grow into one region.
static b32 sseshared(Arena *a, Arena *b)
{
    b32 region = a->region && a->region==b->region;
    b32 space  = (uz)a->beg<(uz)b->end && (uz)b->beg<(uz)a->end;
    return region || space;
}

// Process one complete line (without its ending). Returns a dispatched
// event or a comment, if any, with its strings copied into perm.
static SseEvent sseline(Sse *s, Str line, Arena *perm)
{
    SseEvent e = {};

    if (!line.len) {
        // Dispatch, unless there was no data field: then only reset
        s->inevent = 0;
        if (s->data.len) {
            // Events in one feed share a copy of the ID
            if (s->id.len && !s->idout.data) {
                s->idout = clone(perm, finish(&s->id));
            }
            e.kind = SSE_EVENT;
            e.type = "message";
            e.data = clone(perm, cuttail(finish(&s->data), 1));  // final \n
            e.id   = s->idout;
            if (s->type.len) {
                e.type = clone(perm, finish(&s->type));
            }
        }
        s->data.len = 0;
        s->type.len = 0;
        return e;
    }

    Cut c     = cut(line, ':');
    Str field = c.head;  // whole line if there is no colon
    Str value = c.tail;
    if (value.len && value[0]==' ') {
        value = cuthead(value, 1);
    }

    if (!field.len) {
        // Comment: the spec ignores it, but it shows the server is alive
        e.kind = SSE_COMMENT;
        e.data = clone(perm, value);
        return e;
    }

    s->inevent = 1;
    if (field == "event") {
        s->type.len = 0;
        sseprint(s, &s->type, value);
    } else if (field == "data") {
        if (value.len > s->max - s->data.len) {
            s->err = 1;
            return e;
        }
        sseprint(s, &s->data, value);
        sseprint(s, &s->data, "\n");
    } else if (field == "id") {
        b32 keep = value.len <= SSE_IDMAX;
        for (iz i = 0; keep && i<value.len; i++) {
            keep = value[i] != 0;
        }
        if (keep) {
            s->id.len = 0;
            print(&s->id, value);
            s->idout = {};
        }
    } else if (field == "retry") {
        b32 digits = value.len > 0;
        for (iz i = 0; i < value.len; i++) {
            digits &= digit(value[i]);
        }
        Int ms = parseint(value);
        if (digits && ms.ok) {  // too large to fit is ignored, too
            s->retry = ms.val;
        }
    }
    // Other fields are ignored, but still part of the pending event
    return e;
}

// Tokenize the next chunk of the stream. Returns the events and comments
// it completes, in stream order, allocated with their strings in perm:
// nothing points into the chunk or the tokenizer. Lines that span chunks
// are buffered, so their events come from the chunk that finishes them.
// After an error, returns the events that preceded it, then nothing.
static Slice<SseEvent> ssefeed(Sse *s, Str chunk, Arena *perm)
{
    // The tokenizer may yet allocate from its own arena (newsse), and
    // growing buffers must not land on these events
    Arena *own = s->line.a;
    assert(perm==own || !sseshared(perm, own));

    Slice<SseEvent> r = {};
    if (s->err) {
        return r;
    }
    s->idout = {};  // any copy is in a previous feed's arena

    // Skip a leading BOM, itself possibly split across chunks. Bytes that
    // turn out not to be one are ordinary line content.
    Str bom = "\xef\xbb\xbf";
    for (; s->bom<3 && chunk.len; chunk = cuthead(chunk, 1)) {
        if (chunk[0] != bom[s->bom]) {
            sseprint(s, &s->line, takehead(bom, s->bom));
            s->bom = 3;
            break;
        }
        s->bom++;
    }

    while (chunk.len && !s->err) {
        if (s->cr) {
            s->cr = 0;
            if (chunk[0] == '\n') {
                chunk = cuthead(chunk, 1);  // rest of a CRLF
                continue;
            }
        }

        iz i = 0;
        for (; i<chunk.len && chunk[i]!='\n' && chunk[i]!='\r'; i++) {}
        Str seg = takehead(chunk, i);
        if (seg.len > s->max - s->line.len) {
            s->err = 1;
            break;
        }
        if (i == chunk.len) {
            sseprint(s, &s->line, seg);  // no line ending yet
            break;
        }
        s->cr = chunk[i] == '\r';
        chunk = cuthead(chunk, i+1);

        Str line = seg;  // usually straight from the chunk, without a copy
        if (s->line.len) {
            sseprint(s, &s->line, seg);
            line = finish(&s->line);
        }
        SseEvent e = sseline(s, line, perm);
        s->line.len = 0;
        if (e.kind) {
            r = push(perm, r, e);
        }
    }
    return r;
}

// Call at the end of the body: 1 if the stream stopped inside a line or
// an event (fields with no blank line after them), or after an error. Per
// the spec, the unfinished event is discarded, never dispatched, but a
// stream cut off mid-event is a sign the connection dropped.
static b32 sseend(Sse *s)
{
    b32 partial = s->line.len || (s->bom>0 && s->bom<3);
    return s->err || partial || s->inevent;
}
