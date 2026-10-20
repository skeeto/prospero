// Provider tests: URLs, classification, Retry-After, backoff, discovery,
// argument repair, streamed chunks, request encoding, and text cycles

static Str urlshow(Arena *a, Str s)
{
    Url u = parseurl(s);
    if (!u.ok) {
        return "invalid";
    }
    Buf b(a, 128);
    print(&b, urlorigin(a, u));
    putbyte(&b, ' ');
    print(&b, u.path);
    return finish(&b);
}

// Feed chunks (JSON texts) into a partial response.
static Partial *feed(Arena *a, Str *chunks, iz n, Slice<Delta> *deltas)
{
    Partial *p = newpartial(a);
    for (iz i = 0; i < n; i++) {
        JsonDoc d = jsonparse(a, chunks[i]);
        Chunk   c = applychunk(p, d.root, a);
        for (iz k = 0; deltas && k<c.deltas.len; k++) {
            *deltas = push(a, *deltas, c.deltas[k]);
        }
    }
    return p;
}

static Str blocksshow(Arena *a, Slice<Block> blocks)
{
    Buf b(a, 256);
    for (iz i = 0; i < blocks.len; i++) {
        Block *k = &blocks[i];
        if (i) putbyte(&b, '|');
        print(&b, blockname(k->type));
        putbyte(&b, ':');
        if (k->type == BLOCK_CALL) {
            print(&b, k->id);
            putbyte(&b, ',');
            print(&b, k->name);
            putbyte(&b, ',');
            print(&b, k->args);
        } else {
            print(&b, k->text);
        }
    }
    return finish(&b);
}

static void test_provider(Test *t)
{
    Arena *a = &t->perm;

    // URLs
    {
        expect(t, "url plain", urlshow(a, "http://localhost/"), "http://localhost:80 /");
        expect(t, "url bare", urlshow(a, "http://h"), "http://h:80 /");
        expect(t, "url port and query", urlshow(a, "HTTPS://h:8443/v1?x=1"),
               "https://h:8443 /v1?x=1");
        expect(t, "url ipv6", urlshow(a, "http://[::1]:8080/x"), "http://[::1]:8080 /x");
        expect(t, "url ftp", urlshow(a, "ftp://h/"), "invalid");
        expect(t, "url empty host", urlshow(a, "http:///x"), "invalid");
        expect(t, "url credentials", urlshow(a, "http://u:p@h/"), "invalid");
        expect(t, "url port 0", urlshow(a, "http://h:0/"), "invalid");
        expect(t, "url port big", urlshow(a, "http://h:65536/"), "invalid");
        expect(t, "url no scheme", urlshow(a, "localhost:80"), "invalid");
        expect(t, "url query only", urlshow(a, "http://h?x"), "invalid");

        Str local[] = {"localhost", "LOCALHOST", "127.0.0.1", "127.9.9.9", "10.0.0.1",
                       "172.16.0.1", "172.31.255.255", "192.168.1.106", "169.254.1.1",
                       "biron.local", "[::1]", "x.localhost"};
        Str remote[] = {"example.com", "8.8.8.8", "172.32.0.1", "172.15.0.1",
                        "192.169.0.1", "local", "1.2.3", "1.2.3.4.5", "300.1.1.1"};
        for (iz i = 0; i < countof(local); i++) {
            expect(t, local[i], istr(a, localhost(local[i])), "1");
        }
        for (iz i = 0; i < countof(remote); i++) {
            expect(t, remote[i], istr(a, localhost(remote[i])), "0");
        }

        expect(t, "apiurl /", apiurl(a, "http://h/", "models"), "http://h/v1/models");
        expect(t, "apiurl /v1", apiurl(a, "http://h/v1", "models"), "http://h/v1/models");
        expect(t, "apiurl /v1/", apiurl(a, "http://h/v1/", "chat/completions"),
               "http://h/v1/chat/completions");
        expect(t, "apiurl prefix", apiurl(a, "https://h/api", "models"),
               "https://h/api/v1/models");
    }

    // Classification
    {
        struct { i32 status; Str text; i32 want; } cases[] = {
            {500, "{\"error\":{\"message\":\"Failed to parse tool call arguments as JSON\"}}", CLASS_DETERMINISTIC},
            {500, "Context size has been exceeded.", CLASS_TRANSIENT},
            {400, "{\"error\":{\"type\":\"exceed_context_size_error\"}}", CLASS_OVERFLOW},
            {400, "This model's maximum context length is 8192 tokens", CLASS_OVERFLOW},
            {400, "Error rendering prompt with jinja template", CLASS_DETERMINISTIC},
            {400, "bad request", CLASS_DETERMINISTIC},
            {401, "", CLASS_FATAL},
            {403, "", CLASS_FATAL},
            {404, "", CLASS_DETERMINISTIC},
            {408, "", CLASS_TRANSIENT},
            {429, "slow down", CLASS_TRANSIENT},
            {429, "{\"error\":{\"details\":{\"error_code\":\"enforced_spend_limit_reached\"}}}", CLASS_FATAL},
            {502, "", CLASS_TRANSIENT},
            {503, "Loading model", CLASS_TRANSIENT},
            {529, "overloaded", CLASS_TRANSIENT},
            {599, "", CLASS_TRANSIENT},
            {0, "boom", CLASS_TRANSIENT},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Buf name(a, 64);
            print(&name, "classify ");
            print(&name, (i64)cases[i].status);
            putbyte(&name, ' ');
            print(&name, cases[i].text);
            expect(t, finish(&name), classname(classify(cases[i].status, cases[i].text)),
                   classname(cases[i].want));
        }
        expect(t, "errormessage object",
               errormessage(a, "{\"error\":{\"message\":\"m1\",\"code\":5}}"), "m1");
        expect(t, "errormessage string", errormessage(a, "{\"error\":\"m2\"}"), "m2");
        expect(t, "errormessage top", errormessage(a, "{\"message\":\"m3\"}"), "m3");
        expect(t, "errormessage text", errormessage(a, "  plain text \n"), "plain text");
    }

    // Retry-After and backoff
    {
        expect(t, "retry-after seconds", istr(a, retryafter("Retry-After: 2\r\n", 0)), "2000");
        expect(t, "retry-after case", istr(a, retryafter("x: y\r\nretry-after:  3600 \r\n", 0)),
               "3600000");
        expect(t, "retry-after-ms wins",
               istr(a, retryafter("Retry-After: 9\r\nRetry-After-Ms: 150\r\n", 0)), "150");
        expect(t, "retry-after absent", istr(a, retryafter("Content-Type: x\r\n", 0)), "-1");
        expect(t, "retry-after negative", istr(a, retryafter("Retry-After: -5\r\n", 0)), "-1");
        i64 at = httpdate("Sun, 06 Nov 1994 08:49:37 GMT");
        expect(t, "httpdate", istr(a, at), "784111777000");
        expect(t, "retry-after date",
               istr(a, retryafter("Retry-After: Sun, 06 Nov 1994 08:49:47 GMT\r\n", at)),
               "10000");
        expect(t, "retry-after past date",
               istr(a, retryafter("Retry-After: Sun, 06 Nov 1994 08:49:27 GMT\r\n", at)), "0");
        expect(t, "httpdate bad", istr(a, httpdate("yesterday")), "-1");

        i64 bases[] = {500, 1000, 2000, 4000, 8000, 8000, 8000, 8000, 8000, 8000};
        for (i32 n = 1; n <= 10; n++) {
            i64 lo = backoffms(n, 0);
            i64 hi = backoffms(n, ~(u64)0);
            Buf name(a, 32);
            print(&name, "backoff ");
            print(&name, (i64)n);
            expect(t, finish(&name), istr(a, lo==bases[n-1]*3/4 && hi<=bases[n-1] &&
                                            hi>=bases[n-1]-1), "1");
        }
    }

    // Discovery
    {
        Str loaded =
            "{\"data\":[{\"id\":\"gemma\",\"owned_by\":\"llamacpp\","
            "\"meta\":{\"n_ctx_train\":262144,\"n_ctx\":131072},"
            "\"status\":{\"value\":\"loaded\",\"args\":[\"llama-server\","
            "\"--ctx-size\",\"131072\",\"--kv-unified\",\"--parallel\",\"8\"]}},"
            "{\"id\":\"qwen\",\"owned_by\":\"llamacpp\","
            "\"status\":{\"value\":\"unloaded\",\"args\":[\"llama-server\","
            "\"--ctx-size\",\"524288\",\"--kv-unified\",\"--parallel\",\"8\"]}},"
            "{\"id\":\"split\",\"owned_by\":\"llamacpp\","
            "\"status\":{\"value\":\"unloaded\",\"args\":[\"llama-server\","
            "\"-c\",\"65536\",\"-np\",\"4\"]}},"
            "{\"id\":\"yarn\",\"owned_by\":\"llamacpp\","
            "\"meta\":{\"n_ctx_train\":262144,\"n_ctx\":1048576},"
            "\"status\":{\"value\":\"loaded\",\"args\":[\"--rope-scale\",\"4\","
            "\"--rope-scaling\",\"yarn\",\"--ctx-size\",\"1048576\"]}},"
            "{\"id\":\"large\",\"owned_by\":\"remote\",\"status\":{\"value\":\"loaded\"}}]}";
        ModelInfo m = parsemodels(*a, loaded, "gemma", 0);
        expect(t, "models loaded", istr(a, m.found && m.llamacpp && m.loaded && m.learned), "1");
        expect(t, "models loaded ctx", istr(a, m.ctx), "131072");
        expect(t, "models trained", istr(a, m.trained), "262144");
        m = parsemodels(*a, loaded, "qwen", 0);
        expect(t, "models unloaded pool", istr(a, m.ctx), "524288");
        expect(t, "models unloaded not learned", istr(a, m.learned), "0");
        m = parsemodels(*a, loaded, "qwen", 262144);
        expect(t, "models cached trained", istr(a, m.ctx), "262144");
        m = parsemodels(*a, loaded, "split", 0);
        expect(t, "models per slot", istr(a, m.ctx), "16384");
        m = parsemodels(*a, loaded, "yarn", 0);
        expect(t, "models yarn", istr(a, m.ctx), "1048576");
        m = parsemodels(*a, loaded, "large", 0);
        expect(t, "models remote", istr(a, m.found && !m.llamacpp && !m.ctx), "1");
        m = parsemodels(*a, loaded, "nope", 0);
        expect(t, "models missing", istr(a, m.found), "0");
        m = parsemodels(*a, "not json", "gemma", 0);
        expect(t, "models garbage", istr(a, m.found), "0");
    }

    // Arguments
    {
        struct { Str raw; Str want; } cases[] = {
            {"{\"a\":1}", "{\"a\":1}"},
            {"", "{}"},
            {"  ", "{}"},
            {"\"{\\\"a\\\":2}\"", "{\"a\":2}"},
            {"{\"a\":\"x\ny\"}", "{\"a\":\"x\\ny\"}"},
            {"{\"a\":{\"b\":1}", "{\"a\":{\"b\":1}}"},
            {"{\"a\":1}}", "{\"a\":1}"},
            {"{\"a\":[1,2}", "{\"a\":[1,2]}"},
            {"{\"a\":\"trunc", ""},
            {"[1,2]", ""},
            {"nonsense", ""},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Repair r = repairargs(a, cases[i].raw);
            Str    got = {};
            if (r.root->type == JSON_OBJECT) {
                Buf     b(a, 64);
                JsonOut w = {&b};
                putjson(&w, r.root);
                got = finish(&b);
            }
            expect(t, concat(a, "repair ", cases[i].raw), got, cases[i].want);
        }
        expect(t, "wireargs valid", wireargs(a, "{\"b\": 2, \"a\": 1}"), "{\"b\": 2, \"a\": 1}");
        expect(t, "wireargs repaired", wireargs(a, "{\"a\":1"), "{\"a\":1}");
        expect(t, "wireargs hopeless", wireargs(a, "{\"a\":\"x"), "{}");
    }

    // Streamed chunks
    {
        Str chunks[] = {
            "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}",
            "{\"choices\":[{\"delta\":{\"reasoning_content\":\"hmm \"}}]}",
            "{\"choices\":[{\"delta\":{\"reasoning_content\":\"ok\"}}]}",
            "{\"choices\":[{\"delta\":{\"content\":\"Let me \"}}]}",
            "{\"choices\":[{\"delta\":{\"content\":\"look.\"}}]}",
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c 1\","
                "\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}",
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                "\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}",
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                "\"function\":{\"arguments\":\"\\\"a\\\"}\"}}]}}]}",
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"c 1\","
                "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]}}]}",
            "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}",
            "{\"choices\":[],\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":7,"
                "\"prompt_tokens_details\":{\"cached_tokens\":40}},\"timings\":{\"cache_n\":45}}",
        };
        Slice<Delta> ds = {};
        Partial *p = feed(a, chunks, countof(chunks), &ds);
        expect(t, "chunks finish", p->finish, "tool_calls");
        expect(t, "chunks usage", istr(a, p->usage.in*10000 + p->usage.out*100 + p->usage.cached),
               "500745");
        expect(t, "chunks deltas", istr(a, ds.len), "8");
        expect(t, "chunks first call delta names it",
               concat(a, ds[4].id, concat(a, "/", ds[4].name)), "c 1/read");
        expect(t, "chunks committed", istr(a, committed(p)), "1");
        Finished f = finishpartial(a, p, 1, 3, 2);
        expect(t, "chunks blocks", blocksshow(a, f.blocks),
               "think:hmm ok|text:Let me look.|call:c_1,read,{\"path\":\"a\"}|"
               "call:c_1_dup2,shell,{\"command\":\"ls\"}");
        Finished g = finishpartial(a, p, 0, 3, 2);
        expect(t, "chunks interrupted drops the last call", blocksshow(a, g.blocks),
               "think:hmm ok|text:Let me look.|call:c_1,read,{\"path\":\"a\"}");
        expect(t, "chunks dropped", istr(a, g.dropped), "1");

        Str only[] = {"{\"choices\":[{\"delta\":{\"reasoning\":\"just thinking\"}}]}"};
        p = feed(a, only, 1, 0);
        expect(t, "reasoning alone is not committed", istr(a, committed(p)), "0");
        Str blank[] = {"{\"choices\":[{\"delta\":{\"content\":\" \\n \"}}]}"};
        p = feed(a, blank, 1, 0);
        expect(t, "whitespace is not committed", istr(a, committed(p)), "0");

        Str noid[] = {
            "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                "\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}}]}}]}",
        };
        p = feed(a, noid, 1, 0);
        f = finishpartial(a, p, 1, 3, 2);
        expect(t, "missing call ids are made up", f.blocks[0].id, "call_3_2_0");

        Partial *q   = newpartial(a);
        JsonDoc  err = jsonparse(a, "{\"error\":{\"code\":500,\"message\":\"boom\"}}");
        Chunk    c   = applychunk(q, err.root, a);
        expect(t, "error chunk", concat(a, c.error, istr(a, c.errcode)), "boom500");
        JsonDoc  pp  = jsonparse(a, "{\"choices\":[],\"prompt_progress\":"
                                    "{\"total\":100,\"cache\":10,\"processed\":40}}");
        c = applychunk(q, pp.root, a);
        expect(t, "progress chunk",
               istr(a, c.prefill*1000000 + c.ptotal*10000 + c.pdone*100 + c.pcached),
               "2004010");
        JsonDoc bad = jsonparse(a, "[1]");
        expect(t, "bad chunk", istr(a, applychunk(q, bad.root, a).bad), "1");
    }

    // Request encoding
    {
        ModelConf mc = {};
        mc.name      = "m";
        mc.reasoning = "reasoning_content";
        Msg   msgs[3] = {};
        msgs[0].role = ROLE_USER;
        msgs[0].text = "hi";
        Block blocks[3] = {};
        blocks[0].type = BLOCK_THINK;
        blocks[0].text = "think";
        blocks[1].type = BLOCK_TEXT;
        blocks[1].text = "ok";
        blocks[2].type = BLOCK_CALL;
        blocks[2].id   = "c1";
        blocks[2].name = "read";
        blocks[2].args = "{\"path\":\"x\"";  // missing its brace
        msgs[1].role   = ROLE_ASSISTANT;
        msgs[1].blocks = {blocks, 3, 3};
        msgs[2].role   = ROLE_TOOL;
        msgs[2].callid = "c1";
        msgs[2].text   = "data";
        ChatReq r = {};
        r.mc      = &mc;
        r.system  = "sys";
        r.tools   = "[]";
        r.history = {msgs, 3, 3};
        Arena tmp  = *a;
        tmp.beg   += (tmp.end - tmp.beg)/2;
        Str   one  = encodechat(a, tmp, &r);
        Str   two  = encodechat(a, tmp, &r);
        expect(t, "encoding is deterministic", istr(a, one==two), "1");
        expect(t, "encoding", one,
               "{\"model\":\"m\",\"messages\":[{\"role\":\"system\",\"content\":\"sys\"},"
               "{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\","
               "\"content\":\"ok\",\"reasoning_content\":\"think\",\"tool_calls\":[{"
               "\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read\","
               "\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}}]},{\"role\":\"tool\","
               "\"tool_call_id\":\"c1\",\"content\":\"data\"}],\"tools\":[],"
               "\"stream\":true,\"stream_options\":{\"include_usage\":true}}");

        mc.reasoning = {};
        mc.llamacpp  = 1;
        Extra x = {"temperature", "0.7"};
        mc.extras = {&x, 1, 1};
        Str three = encodechat(a, tmp, &r);
        expect(t, "no reasoning sent back", istr(a, find(three, "reasoning")), "-1");
        expect(t, "llama.cpp and sampling extras",
               istr(a, endswith(three, ",\"return_progress\":true,\"temperature\":0.7}")), "1");
    }

    // Cycles
    {
        Cycle *c = alloc<Cycle>(a);
        b32    hit = 0;
        for (i32 i = 0; i < 200 && !hit; i++) {
            hit = cyclefeed(c, "ab") >= 0;
        }
        expect(t, "short cycle found", istr(a, hit), "1");
        expect(t, "short cycle covers 180 bytes", istr(a, c->unit*c->reps >= 180), "1");

        c = alloc<Cycle>(a);
        Buf text(a, 8192);
        for (i32 i = 0; i < 300; i++) {
            print(&text, "word");
            print(&text, (i64)i);
            putbyte(&text, ' ');
        }
        expect(t, "no cycle in counting", istr(a, cyclefeed(c, finish(&text))), "-1");

        c = alloc<Cycle>(a);
        Buf unit(a, 128);
        for (i32 i = 0; i < 100; i++) putbyte(&unit, (u8)('a' + i%26 + (i/26)));
        Str u = finish(&unit);
        expect(t, "long unit thrice is short of 1 KiB",
               istr(a, cyclefeed(c, concat(a, concat(a, u, u), u))), "-1");
        b32 found = 0;
        for (i32 i = 0; i < 20 && !found; i++) {
            found = cyclefeed(c, u) >= 0;
        }
        expect(t, "long unit repeated past 1 KiB", istr(a, found && c->unit==100), "1");
    }
}
