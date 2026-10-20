// Server-sent events tests: spec examples, line endings, provider streams,
// caps, and feeding each stream split into chunks every possible way

// Show controls as \n or \xHH; other bytes, backslash included, as is.
static void sseesc(Buf *b, Str s)
{
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c == '\n') {
            print(b, "\\n");
        } else if (c<0x20 || c==0x7f) {
            print(b, "\\x");
            printhex(b, c, 2);
        } else {
            putbyte(b, c);
        }
    }
}

static void sseclobber(byte *beg, byte *end)
{
    for (; beg < end; beg++) {
        *beg = 0x55;
    }
}

// Render an event as [type#id:data] and a comment as (text).
static void sserender(Buf *b, SseEvent e)
{
    if (e.kind == SSE_COMMENT) {
        putbyte(b, '(');
        sseesc(b, e.data);
        print(b, ") ");
    } else {
        putbyte(b, '[');
        sseesc(b, e.type);
        if (e.id.len) {
            putbyte(b, '#');
            sseesc(b, e.id);
        }
        putbyte(b, ':');
        sseesc(b, e.data);
        print(b, "] ");
    }
}

// Render the end state: ok, truncated, or error, then the retry field.
static void ssestatus(Buf *b, b32 err, b32 cutoff, i64 retry)
{
    print(b, err ? Str("error") : cutoff ? Str("truncated") : Str("ok"));
    if (retry >= 0) {
        print(b, " retry=");
        print(b, retry);
    }
}

// Feed src split at the given offsets (clamped to its length), rendering
// each event and comment, then the end state. Chunks and events live in
// a per-feed arena of their own, clobbered as soon as the chunk is fed
// and again once its events are rendered, so neither the events nor the
// tokenizer may hold on to a chunk or a previous feed.
static Str ssetrace(Arena *a, Str src, iz *cuts, iz ncuts, iz max)
{
    iz    cap     = (iz)1<<20;
    Arena scratch = {};
    scratch.beg = (byte *)allocbytes(a, cap);
    scratch.end = scratch.beg + cap;

    Sse s = newsse(a, max);
    Buf b(a, 256);
    iz  beg = 0;
    for (iz i = 0; i <= ncuts; i++) {
        Arena           feed  = scratch;
        iz              end   = i<ncuts && cuts[i]<src.len ? cuts[i] : src.len;
        Str             chunk = clone(&feed, span(src.data+beg, src.data+end));
        Slice<SseEvent> evs   = ssefeed(&s, chunk, &feed);
        sseclobber((byte *)chunk.data, (byte *)(chunk.data+chunk.len));
        for (iz j = 0; j < evs.len; j++) {
            sserender(&b, evs[j]);
        }
        sseclobber(scratch.beg, feed.beg);
        beg = end;
    }

    ssestatus(&b, s.err, sseend(&s), s.retry);
    return finish(&b);
}

// An independent model of the tokenizer over a whole stream, rendered
// like ssetrace: strip the BOM, split all the lines up front, then apply
// the spec's field rules and the caps line by line.
static Str ssemodel(Arena *a, Str src, iz max)
{
    Buf b(a, 256);
    Str bom = "\xef\xbb\xbf";
    if (src.len>0 && src.len<3 && src==takehead(bom, src.len)) {
        print(&b, "truncated");  // might have been a BOM
        return finish(&b);
    }
    if (startswith(src, bom)) {
        src = cuthead(src, 3);
    }

    Buf data(a, src.len+1);  // values, each followed by \n
    Str type    = {};
    Str id      = {};
    i64 retry   = -1;
    b32 err     = 0;
    b32 partial = 0;
    b32 inevent = 0;
    while (src.len) {
        iz n = 0;
        for (; n<src.len && src[n]!='\r' && src[n]!='\n'; n++) {}
        Str line = takehead(src, n);
        if (line.len > max) {
            err = 1;
            break;
        } else if (n == src.len) {
            partial = 1;  // no line ending
            break;
        }
        b32 crlf = src[n]=='\r' && n+1<src.len && src[n+1]=='\n';
        src = cuthead(src, n + 1 + crlf);

        if (!line.len) {
            if (data.len) {
                SseEvent e = {};
                e.kind = SSE_EVENT;
                e.type = type.len ? type : Str("message");
                e.data = takehead(finish(&data), data.len-1);
                e.id   = id;
                sserender(&b, e);
            }
            data.len = 0;
            type     = {};
            inevent  = 0;
            continue;
        }

        iz colon = 0;
        for (; colon<line.len && line[colon]!=':'; colon++) {}
        Str name  = takehead(line, colon);
        Str value = cuthead(line, colon<line.len ? colon+1 : colon);
        if (value.len && value[0]==' ') {
            value = cuthead(value, 1);
        }

        if (!name.len) {
            SseEvent e = {};
            e.kind = SSE_COMMENT;
            e.data = value;
            sserender(&b, e);
            continue;
        }

        inevent = 1;
        if (name == "event") {
            type = value;
        } else if (name == "data") {
            if (data.len+value.len > max) {  // joined, this value included
                err = 1;
                break;
            }
            print(&data, value);
            putbyte(&data, '\n');
        } else if (name == "id") {
            b32 nul = 0;
            for (iz i = 0; i < value.len; i++) {
                nul |= value[i] == 0;
            }
            if (!nul && value.len<=SSE_IDMAX) {
                id = value;
            }
        } else if (name == "retry") {
            i64 v  = 0;
            b32 ok = value.len > 0;
            for (iz i = 0; ok && i<value.len; i++) {
                i32 d = value[i] - '0';
                ok = d>=0 && d<=9 && v<=(I64MAX - d)/10;
                v  = ok ? v*10 + d : v;
            }
            retry = ok ? v : retry;
        }
    }

    ssestatus(&b, err, partial||inevent, retry);
    return finish(&b);
}

static Str sselabel(Arena *a, Str name, Str mode, iz i, iz j)
{
    Buf b(a, 64);
    print(&b, name);
    print(&b, mode);
    if (i >= 0) {
        putbyte(&b, '@');
        print(&b, i);
    }
    if (j >= 0) {
        putbyte(&b, ',');
        print(&b, j);
    }
    return finish(&b);
}

// Chunking must not matter: split src once at every offset (empty chunks
// at either end included), twice at every pair of offsets if it's short,
// and into single bytes, and compare each trace with the whole one. Each
// mode reports its first difference, labeled with the offsets.
static void ssesplits(Test *t, Arena a, Str name, Str src, iz max, Str whole)
{
    Arena tmp = a;
    Str   got = whole;
    iz    at  = -1;
    for (iz i = 0; i<=src.len && at<0; i++) {
        tmp = a;
        got = ssetrace(&tmp, src, &i, 1, max);
        at  = got==whole ? -1 : i;
    }
    expect(t, sselabel(&tmp, name, ".split", at, -1), got, whole);

    if (src.len <= 256) {
        tmp = a;
        got = whole;
        at  = -1;
        iz at2 = -1;
        for (iz i = 1; i<src.len && at<0; i++) {
            for (iz j = i+1; j<src.len && at<0; j++) {
                tmp = a;
                iz cuts[] = {i, j};
                got = ssetrace(&tmp, src, cuts, 2, max);
                if (got != whole) {
                    at  = i;
                    at2 = j;
                }
            }
        }
        expect(t, sselabel(&tmp, name, ".split2", at, at2), got, whole);
    }

    tmp = a;
    iz  ncuts = src.len>1 ? src.len-1 : 0;
    iz *cuts  = alloc<iz>(&tmp, ncuts);
    for (iz i = 0; i < ncuts; i++) {
        cuts[i] = i + 1;
    }
    got = ssetrace(&tmp, src, cuts, ncuts, max);
    expect(t, sselabel(&tmp, name, ".bytes", -1, -1), got, whole);
}

// Check the whole stream and the model against want, then every split.
static void ssecase(Test *t, Arena a, Str name, Str in, Str want, iz max)
{
    Str whole = ssetrace(&a, in, 0, 0, max);
    expect(t, name, whole, want);
    expect(t, sselabel(&a, name, ".model", -1, -1), ssemodel(&a, in, max), want);
    ssesplits(t, a, name, in, max, whole);
}

// Append pre, then n filler bytes, then post.
static void ssefill(Buf *b, Str pre, iz n, u8 c, Str post)
{
    print(b, pre);
    for (iz i = 0; i < n; i++) {
        putbyte(b, c);
    }
    print(b, post);
}

static void test_sse(Test *t)
{
    Arena a = t->perm;

    struct { Str name, in, want; } cases[] = {
        // Examples from the WHATWG HTML spec, 9.2.6
        {"spec.stock", "data: YHOO\ndata: +2\ndata: 10\n\n",
                       "[message:YHOO\\n+2\\n10] ok"},
        {"spec.blocks", ": test stream\n\ndata: first event\nid: 1\n\n"
                        "data:second event\nid\n\ndata:  third event\n\n",
                        "(test stream) [message#1:first event] "
                        "[message:second event] [message: third event] ok"},
        {"spec.empty", "data\n\ndata\ndata\n\ndata:",
                       "[message:] [message:\\n] truncated"},
        {"spec.space", "data:test\n\ndata: test\n\n",
                       "[message:test] [message:test] ok"},
        {"spec.types", "event: add\ndata: 73857293\n\nevent: remove\n"
                       "data: 2153\n\nevent: add\ndata: 113411\n\n",
                       "[add:73857293] [remove:2153] [add:113411] ok"},

        // Line endings
        {"eol.crlf",   "data: a\r\ndata: b\r\n\r\n", "[message:a\\nb] ok"},
        {"eol.cr",     "data: a\rdata: b\r\r", "[message:a\\nb] ok"},
        {"eol.mixed",  "data: a\r\ndata: b\rdata: c\n\r\n",
                       "[message:a\\nb\\nc] ok"},
        {"eol.wpt",    "data:test\r\ndata\ndata:test\r\n\r",
                       "[message:test\\n\\ntest] ok"},
        {"eol.lfcr",   "data: a\n\rdata: b\n\n", "[message:a] [message:b] ok"},
        {"eol.crcrlf", "data: a\r\r\ndata: b\n\n", "[message:a] [message:b] ok"},
        {"eol.blanks", "\r\n\n\r\r\n\n", "ok"},
        {"eol.lastcr", "data: a\n\r", "[message:a] ok"},
        {"eol.none",   "data: a", "truncated"},
        {"eol.nodisp", "data: a\n", "truncated"},
        {"empty",      "", "ok"},

        // Byte order mark
        {"bom.skip",    "\xef\xbb\xbf" "data: a\n\n", "[message:a] ok"},
        {"bom.twice",   "\xef\xbb\xbf\xef\xbb\xbf" "data: a\n\ndata: b\n\n",
                        "[message:b] ok"},
        {"bom.partial", "\xef\xbb" "data: a\n\ndata: b\n\n", "[message:b] ok"},
        {"bom.lone",    "\xef\n\ndata: b\n\n", "[message:b] ok"},
        {"bom.only",    "\xef\xbb\xbf", "ok"},
        {"bom.cut",     "\xef\xbb", "truncated"},
        {"bom.late",    "data: a\n\n\xef\xbb\xbf" "data: b\n\n", "[message:a] ok"},
        {"bom.comment", "\xef\xbb\xbf: hi\n", "(hi) ok"},
        {"bom.eol",     "\xef\xbb\xbf\r\ndata: a\r\n\r\n", "[message:a] ok"},

        // Comments, surfaced as liveness
        {"comment.ping",    ": ping\n", "(ping) ok"},
        {"comment.bare",    ":\n", "() ok"},
        {"comment.tight",   ":ping\n", "(ping) ok"},
        {"comment.spaces",  ":  two\n", "( two) ok"},
        {"comment.inside",  "data: a\n: c\ndata: b\n\n", "(c) [message:a\\nb] ok"},
        {"comment.llama",   ":\n\n:\n\ndata: {}\n\n", "() () [message:{}] ok"},
        {"comment.cut",     ": pi", "truncated"},
        {"comment.wpt",     "data:1\r:\0\n:\r\ndata:2\n:xxxx\rdata:3\n"
                            ":data:fail\r:yyyy\ndata:4\n\n",
                            "(\\x00) () (xxxx) (data:fail) (yyyy) "
                            "[message:1\\n2\\n3\\n4] ok"},

        // Fields
        {"field.nocolon", "data\n\n", "[message:] ok"},
        {"field.colons",  "data: a:b\n\ndata::\n\n", "[message:a:b] [message::] ok"},
        {"field.tab",     "data:\ttab\n\n", "[message:\\x09tab] ok"},
        {"field.case",    "Data: a\nDATA: b\n\n", "ok"},
        {"field.unknown", "data:test\n data\ndata\nfoobar:xxx\njustsometext\n"
                          ":thisisacommentyay\ndata:test\n\n",
                          "(thisisacommentyay) [message:test\\n\\ntest] ok"},
        {"field.parsing", "data:\0\ndata:  2\rData:1\ndata\0:2\ndata:1\r\0data:4\n"
                          "da-ta:3\rdata_5\ndata:3\rdata:\r\n data:32\ndata:4\n\n",
                          "[message:\\x00\\n 2\\n1\\n3\\n\\n4] ok"},
        {"field.cut",     "foo: bar\n", "truncated"},
        {"data.blank",    "data:\ndata:\n\n", "[message:\\n] ok"},
        {"data.nul",      "data:\0\n\n\n\n", "[message:\\x00] ok"},
        {"data.utf8",     "data: h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80\n\n",
                          "[message:h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80] ok"},
        {"data.invalid",  "data: \xff\xc3\n\n", "[message:\xff\xc3] ok"},
        {"event.nodata",  "event: x\n\n", "ok"},
        {"event.reset",   "event: x\n\ndata: a\n\n", "[message:a] ok"},
        {"event.empty",   "event: \ndata: a\n\n", "[message:a] ok"},
        {"event.message", "event: message\ndata: a\n\n", "[message:a] ok"},
        {"event.last",    "event: a\nevent: b\ndata: x\n\n", "[b:x] ok"},
        {"event.after",   "data: x\nevent: late\n\n", "[late:x] ok"},
        {"event.spaced",  "event:  sp ace\ndata: x\n\n", "[ sp ace:x] ok"},

        // The last event ID persists until changed
        {"id.keep",   "id: 1\ndata: a\n\ndata: b\n\n",
                      "[message#1:a] [message#1:b] ok"},
        {"id.change", "id: 1\ndata: a\n\nid: 2\ndata: b\n\n",
                      "[message#1:a] [message#2:b] ok"},
        {"id.reset",  "id: 1\ndata: a\n\nid\ndata: b\n\n",
                      "[message#1:a] [message:b] ok"},
        {"id.nul",    "id: 1\ndata: a\n\nid: 2\0x\ndata: b\n\n",
                      "[message#1:a] [message#1:b] ok"},
        {"id.nodata", "id: 5\n\ndata: x\n\n", "[message#5:x] ok"},
        {"id.after",  "data: x\nid: 9\n\n", "[message#9:x] ok"},
        {"id.space",  "id:  7\ndata: a\n\n", "[message# 7:a] ok"},

        // Reconnection time
        {"retry.set",   "retry: 1000\n\n", "ok retry=1000"},
        {"retry.zero",  "retry: 0\n\n", "ok retry=0"},
        {"retry.bogus", "retry:3000\nretry:1000x\ndata:x\n\n",
                        "[message:x] ok retry=3000"},
        {"retry.empty", "retry\nretry:\ndata:x\n\n", "[message:x] ok"},
        {"retry.sign",  "retry: -1\nretry: +1\nretry:  5\nretry: 1.5\n\n", "ok"},
        {"retry.huge",  "retry: 7\nretry: 99999999999999999999\n\n", "ok retry=7"},
        {"retry.wpt",   "retry:1000\ndata:test1\n\nid:test\ndata:test2",
                        "[message:test1] truncated retry=1000"},

        // OpenAI-compatible chat completions, as from llama.cpp
        {"openai",
         "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
         "\"content\":null}}],\"object\":\"chat.completion.chunk\"}\n\n"
         ": ping\n\n"
         "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi\"}}]}\n\n"
         "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\"\"}}]}}]}\n\n"
         "data: {\"choices\":[{\"index\":0,\"delta\":{},"
         "\"finish_reason\":\"tool_calls\"}]}\n\n"
         "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10}}\n\n"
         "data: [DONE]\n\n",
         "[message:{\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
         "\"content\":null}}],\"object\":\"chat.completion.chunk\"}] "
         "(ping) "
         "[message:{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi\"}}]}] "
         "[message:{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\"\"}}]}}]}] "
         "[message:{\"choices\":[{\"index\":0,\"delta\":{},"
         "\"finish_reason\":\"tool_calls\"}]}] "
         "[message:{\"choices\":[],\"usage\":{\"prompt_tokens\":10}}] "
         "[message:[DONE]] ok"},
        {"openai.crlf",
         "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r\n\r\n"
         "data: [DONE]\r\n\r\n",
         "[message:{\"choices\":[{\"delta\":{\"content\":\"a\"}}]}] "
         "[message:[DONE]] ok"},
        {"openai.error",
         "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
         "data: {\"error\":{\"code\":500,\"message\":\"boom\","
         "\"type\":\"server_error\"}}\n\n",
         "[message:{\"choices\":[{\"delta\":{\"content\":\"a\"}}]}] "
         "[message:{\"error\":{\"code\":500,\"message\":\"boom\","
         "\"type\":\"server_error\"}}] ok"},
        {"openai.cut",
         "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\ndata: {\"cho",
         "[message:{\"choices\":[{\"delta\":{\"content\":\"a\"}}]}] truncated"},
        {"openai.nodone",
         "data: {\"choices\":[]}\n\ndata: [DONE]\n",
         "[message:{\"choices\":[]}] truncated"},

        // Anthropic Messages
        {"anthropic",
         "event: message_start\n"
         "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
         "\"type\":\"message\",\"role\":\"assistant\",\"content\":[],"
         "\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n"
         "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,"
         "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
         "event: ping\n"
         "data: {\"type\": \"ping\"}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,"
         "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":1,"
         "\"delta\":{\"type\":\"input_json_delta\","
         "\"partial_json\":\"{\\\"path\\\": \\\"a.c\"}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
         "\"end_turn\"},\"usage\":{\"output_tokens\":15}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n",
         "[message_start:{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
         "\"type\":\"message\",\"role\":\"assistant\",\"content\":[],"
         "\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}] "
         "[content_block_start:{\"type\":\"content_block_start\",\"index\":0,"
         "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}] "
         "[ping:{\"type\": \"ping\"}] "
         "[content_block_delta:{\"type\":\"content_block_delta\",\"index\":0,"
         "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}] "
         "[content_block_delta:{\"type\":\"content_block_delta\",\"index\":1,"
         "\"delta\":{\"type\":\"input_json_delta\","
         "\"partial_json\":\"{\\\"path\\\": \\\"a.c\"}}] "
         "[content_block_stop:{\"type\":\"content_block_stop\",\"index\":0}] "
         "[message_delta:{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
         "\"end_turn\"},\"usage\":{\"output_tokens\":15}}] "
         "[message_stop:{\"type\":\"message_stop\"}] ok"},
        {"anthropic.error",
         "event: error\n"
         "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
         "\"message\":\"Overloaded\"}}\n\n",
         "[error:{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
         "\"message\":\"Overloaded\"}}] ok"},
        {"anthropic.cut",
         "event: message_stop\n",
         "truncated"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        ssecase(t, a, cases[i].name, cases[i].in, cases[i].want, 1<<20);
    }

    // Lines and event data are capped
    struct { Str name, in, want; iz max; } capped[] = {
        {"cap.fits",     "data: 0123456789\n\n", "[message:0123456789] ok", 16},
        {"cap.line",     "data: 0123456789A\n\n", "error", 16},
        {"cap.eol",      "data: 0123456789\r\n\r\n", "[message:0123456789] ok", 16},
        {"cap.unended",  "data: 0123456789ABCDEF", "error", 16},
        {"cap.comment",  ": 0123456789ABCDEF\n", "error", 16},
        {"cap.data",     "data: 01234567\ndata: 01234567\n\n", "error", 16},
        {"cap.datafits", "data: 0123456\ndata: 01234567\n\n",
                         "[message:0123456\\n01234567] ok", 16},
        {"cap.before",   "data: a\n\n: c\ndata: 0123456789AB\n\ndata: b\n\n",
                         "[message:a] (c) error", 16},
        {"cap.bom",      "\xef\xbb\xbf" "data: 0123456789\n\n",
                         "[message:0123456789] ok", 16},
        {"cap.tiny",     "\xef\xbb\n", "error", 1},
        {"cap.tiny2",    "\xef\xbb\n", "truncated", 2},
        {"cap.zero",     "\r\n\n", "ok", 0},
    };
    for (iz i = 0; i < countof(capped); i++) {
        ssecase(t, a, capped[i].name, capped[i].in, capped[i].want,
                capped[i].max);
    }

    // Random streams from field-ish tokens, under small and large caps:
    // the whole stream must match the model, and the first hundred must
    // match it however they're split. Past those, report the first miss.
    {
        Str parts[] = {
            "data", "event", "id", "retry", ":", ":", " ", "x", "7",
            "\n", "\n", "\r", "\r\n", "\xef", "\xbb", "\xbf",
            Str((u8 *)"\0", 1),
        };
        Arena tmp  = a;
        u64   rng  = 1;
        i32   miss = -1;
        Str   got  = {};
        Str   want = {};
        for (i32 n = 0; n<5000 && miss<0; n++) {
            tmp = a;
            Buf b(&tmp, 256);
            if (n & 1) {
                print(&b, "\xef\xbb\xbf");
            }
            for (i32 k = 0; k < 40; k++) {
                rng = rng*0x3243f6a8885a308d + 1;
                print(&b, parts[(rng>>33) % countof(parts)]);
            }
            Str src   = finish(&b);
            iz  max   = n&2 ? 1<<20 : (n>>2)%12;
            Str whole = ssetrace(&tmp, src, 0, 0, max);
            Str model = ssemodel(&tmp, src, max);
            if (n < 100) {
                Str name = sselabel(&tmp, "random", "", n, -1);
                expect(t, sselabel(&tmp, name, ".model", -1, -1), whole, model);
                ssesplits(t, tmp, name, src, max, whole);
            } else if (whole != model) {
                miss = n;  // keep tmp, which holds both
                got  = whole;
                want = model;
            }
        }
        expect(t, sselabel(&tmp, "random", ".model", miss, -1), got, want);
    }

    // A large event grows the buffers, which still stop at the cap
    {
        Arena tmp = a;
        iz    len = 100000;
        Buf   in(&tmp, len+16);
        Buf   want(&tmp, len+16);
        print(&in, "data: ");
        print(&want, "[message:");
        for (iz i = 0; i < len; i++) {
            u8 c = (u8)('a' + i%26);
            putbyte(&in, c);
            putbyte(&want, c);
        }
        print(&in, "\n\n");
        print(&want, "] ok");
        iz  ncuts = 99;
        iz *cuts  = alloc<iz>(&tmp, ncuts);
        for (iz i = 0; i < ncuts; i++) {
            cuts[i] = (i+1) * 1013;  // past the end is clamped
        }
        Str src = finish(&in);
        Str got = ssetrace(&tmp, src, cuts, ncuts, len+6);
        expect(t, "big.fits", got, finish(&want));
        got = ssetrace(&tmp, src, cuts, ncuts, len+5);
        expect(t, "big.line", got, "error");

        // Many short lines into one event: the data cap
        Buf lines(&tmp, 1<<15);
        Buf joined(&tmp, 1<<15);
        print(&joined, "[message:");
        for (i32 i = 0; i < 2000; i++) {
            print(&lines, "data: 0123456789\n");
            print(&joined, i ? Str("\\n0123456789") : Str("0123456789"));
        }
        print(&lines, "\n");
        print(&joined, "] ok");
        src = finish(&lines);
        iz data = 2000*11 - 1;  // with the joining newlines
        got = ssetrace(&tmp, src, cuts, ncuts, data);
        expect(t, "big.data", got, finish(&joined));
        got = ssetrace(&tmp, src, cuts, ncuts, data-1);
        expect(t, "big.over", got, "error");
    }

    // An error is sticky, and the events before it are still returned
    {
        Arena tmp = a;
        Sse   s   = newsse(&tmp, 8);
        Str             in  = "data: a\n\ndata: 0123456789\n\n";
        Slice<SseEvent> evs = ssefeed(&s, in, &tmp);
        expect(t, "sticky.before", istr(&tmp, evs.len), "1");
        expect(t, "sticky.err", istr(&tmp, s.err), "1");
        evs = ssefeed(&s, "data: b\n\n", &tmp);
        expect(t, "sticky.after", istr(&tmp, evs.len), "0");
        expect(t, "sticky.end", istr(&tmp, sseend(&s)), "1");
    }

    // A long stream in uneven chunks: the tokenizer's own memory, here a
    // fixed arena that traps if exhausted, doesn't grow past newsse
    {
        Arena tmp   = a;
        iz    size  = 1<<13;
        Arena state = {};
        state.beg = (byte *)allocbytes(&tmp, size);
        state.end = state.beg + size;
        Sse   s    = newsse(&state, 1<<20);
        byte *mark = state.beg;

        Str ev = "event: content_block_delta\n"
                 "data: {\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"text_delta\",\"text\":\"tok\"}}\n\n";
        i32 count = 10000;
        Buf in(&tmp, ev.len*count);
        for (i32 i = 0; i < count; i++) {
            print(&in, ev);
        }
        Str src = finish(&in);

        u64 rng  = 2;
        iz  seen = 0;
        for (iz off = 0; off < src.len;) {
            Arena feed = tmp;
            rng = rng*0x3243f6a8885a308d + 1;
            iz len = (iz)(rng>>33)%4096 + 1;
            len = len<src.len-off ? len : src.len-off;
            Str             chunk = {src.data+off, len};
            Slice<SseEvent> evs   = ssefeed(&s, chunk, &feed);
            for (iz i = 0; i < evs.len; i++) {
                SseEvent e = evs[i];
                seen += e.type=="content_block_delta" && endswith(e.data, "}}");
            }
            off += len;
        }
        expect(t, "long.events", istr(&tmp, seen), istr(&tmp, count));
        expect(t, "long.end", istr(&tmp, sseend(&s)), "0");
        expect(t, "long.memory", istr(&tmp, state.beg-mark), "0");
    }

    // An ID up to SSE_IDMAX bytes is kept, a longer one ignored
    {
        Arena tmp  = a;
        Buf   in(&tmp, 1<<10);
        Buf   want(&tmp, 1<<10);
        ssefill(&in, "id: ", SSE_IDMAX, 'i', "\ndata: a\n\n");
        ssefill(&in, "id: ", SSE_IDMAX+1, 'j', "\ndata: b\n\n");
        ssefill(&want, "[message#", SSE_IDMAX, 'i', ":a] ");
        ssefill(&want, "[message#", SSE_IDMAX, 'i', ":b] ok");
        ssecase(t, tmp, "id.max", finish(&in), finish(&want), 1<<20);
    }

    // Every feed that dispatches copies the ID, so a long one sent once
    // must not make each later small feed costly
    {
        Arena tmp     = a;
        iz    max     = 1<<20;
        iz    cap     = (iz)1<<21;  // roomy: a regression fails, not traps
        Arena scratch = {};
        scratch.beg = (byte *)allocbytes(&tmp, cap);
        scratch.end = scratch.beg + cap;
        Sse s = newsse(&tmp, max);

        Buf id(&tmp, max+(1<<10));
        ssefill(&id, "id: ", SSE_IDMAX, 'i', "\n");
        Str kept = cuttail(cuthead(finish(&id), 4), 1);
        ssefill(&id, "id: ", max-4, 'j', "\n");  // a whole line of ID
        Arena feed = scratch;
        ssefeed(&s, finish(&id), &feed);

        iz  most = 0;
        i32 same = 0;
        for (i32 i = 0; i < 1000; i++) {
            feed = scratch;
            Slice<SseEvent> evs = ssefeed(&s, "data:x\n\n", &feed);
            same += evs.len==1 && evs[0].id==kept;
            iz used = feed.beg - scratch.beg;
            most = used>most ? used : most;
        }
        expect(t, "id.events", istr(&tmp, same), "1000");
        // The ID's copy, then the event with its data and slice
        expect(t, "id.feed", istr(&tmp, most <= SSE_IDMAX+(1<<10)), "1");
    }

    // Lines, types and data that grow step by step to the cap: each
    // buffer grows at most once, so the tokenizer stays within its
    // documented 3*max + 5 KiB of perm
    {
        Arena tmp   = a;
        iz    max   = ((iz)1<<16) + 8;
        iz    size  = 16*max;  // roomy: a regression fails, not traps
        Arena state = {};
        state.beg = (byte *)allocbytes(&tmp, size);
        state.end = state.beg + size;
        byte *mark = state.beg;
        Sse   s    = newsse(&state, max);

        Buf in(&tmp, 1<<20);
        for (iz n = 32; n < max-7; n *= 2) {
            ssefill(&in, "event: ", n, 'e', "\n");
            ssefill(&in, "id: ", n, 'i', "\n");
        }
        ssefill(&in, "event: ", max-7, 'e', "\n");  // a line of max bytes
        iz lines = (max+1) / 11;
        for (iz i = 0; i < lines; i++) {
            print(&in, "data: 0123456789\n");
        }
        iz rest = max - (lines*11 - 1);  // fill the data to exactly max
        if (rest) {
            ssefill(&in, "data: ", rest-1, 'd', "\n");
        }
        print(&in, "\n");
        Str src = finish(&in);

        // One byte at a time, so every line passes through the line buffer
        iz events = 0;
        iz type   = 0;
        iz data   = 0;
        for (iz i = 0; i < src.len; i++) {
            Arena           feed = tmp;
            Slice<SseEvent> evs  = ssefeed(&s, {src.data+i, 1}, &feed);
            for (iz j = 0; j < evs.len; j++) {
                events++;
                type = evs[j].type.len;
                data = evs[j].data.len;
            }
        }
        expect(t, "grow.events", istr(&tmp, events), "1");
        expect(t, "grow.type", istr(&tmp, type), istr(&tmp, max-7));
        expect(t, "grow.data", istr(&tmp, data), istr(&tmp, max));
        expect(t, "grow.end", istr(&tmp, sseend(&s)), "0");

        // All three buffers reached the cap, and stopped there
        iz used = state.beg - mark;
        expect(t, "grow.used", istr(&tmp, used/max), "3");
        expect(t, "grow.bound", istr(&tmp, used <= 3*max+(5<<10)), "1");
    }
}
