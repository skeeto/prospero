// Price tests: model ids normalized to the table's, each row, the cost
// of a request in microusd, [model] price parsed, and which price a
// request takes (pricefor)

// The input price of the row an id finds, as text, or "none".
static Str ptrow(Arena *a, Str id)
{
    PriceRow const *r = pricerow(*a, id);
    if (!r) {
        return "none";
    }
    Buf b(a, 32);
    printfloat(&b, r->in, 2);
    putbyte(&b, ' ');
    printfloat(&b, r->out, 2);
    return finish(&b);
}

static Usage ptuse(i64 in, i64 cached, i64 written, i64 out)
{
    Usage u   = {};
    u.in      = in;
    u.cached  = cached;
    u.written = written;
    u.out     = out;
    return u;
}

// A request's cost at the price an id's row has, -1 for none.
static i64 ptcost(Arena *a, Str id, Usage u, i64 searches = 0)
{
    Price p = pricefor(*a, 0, EP_ANTHROPIC, {}, id);
    return costof(&p, &u, searches);
}

static Str ptparse(Arena *a, Str v)
{
    Price p  = {};
    b32   ok = priceparse(v, &p);
    Buf   b(a, 128);
    print(&b, ok ? Str("ok ") : Str("bad "));
    print(&b, p.how==PRICE_UNSET ? Str("unset") :
              p.how==PRICE_NONE  ? Str("none")  : Str("set"));
    if (p.how == PRICE_SET) {
        f64 xs[] = {p.in, p.out, p.cached, p.written};
        for (iz i = 0; i < countof(xs); i++) {
            putbyte(&b, ' ');
            printfloat(&b, xs[i], 4);
        }
    }
    return finish(&b);
}

static void test_price(Test *t)
{
    Arena *a = &t->perm;

    // Ids as providers and proxies spell them, each to one key
    {
        struct { Str id; Str want; } cases[] = {
            {"claude-sonnet-4-5", "3 15"},
            {"claude-sonnet-4-5-20250929", "3 15"},
            {"us.anthropic.claude-sonnet-4-5-20250929-v1:0", "3 15"},
            {"anthropic.claude-haiku-4-5-20251001-v1:0", "1 5"},
            {"Anthropic.Claude-Haiku-4-5", "1 5"},
            {"claude-opus-4-1@20250805", "15 75"},
            {"anthropic/claude-opus-4.1", "15 75"},
            {"openrouter/anthropic/claude-opus-4.5", "5 25"},
            {"CLAUDE-OPUS-5-5", "4 20"},
            {"claude-opus-4", "15 75"},
            {"claude-opus-4-20250514", "15 75"},
            {"claude-sonnet-5", "2 10"},
            {"claude-sonnet-5-5", "2 10"},
            {"claude-fable-5-2", "8 40"},
            {"claude-mythos-5-1", "10 50"},
            {"claude-fable-5", "10 50"},
            {"claude-haiku-5-5", "0.1 0.5"},
            // Never a substring: a different model, or no Anthropic one
            {"claude-sonnet-4-5-mini", "none"},
            {"my-claude-sonnet-4-5", "none"},
            {"gpt-5", "none"},
            {"claude-3-opus-20240229", "none"},
            {"", "none"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena s = *a;
            expect(t, concat(&s, "pricerow ", cases[i].id), ptrow(&s, cases[i].id),
                   cases[i].want);
        }
    }

    // Each row's prices, input, cache write, cache read, output, from a
    // million tokens of each
    {
        struct { Str id; i64 in, written, cached, out; } rows[] = {
            {"claude-fable-5-2",  8000000, 10000000,  200000, 40000000},
            {"claude-mythos-5-2", 8000000, 10000000,  200000, 40000000},
            {"claude-fable-5-1", 10000000, 12500000,  250000, 50000000},
            {"claude-fable-5",   10000000, 12500000, 1000000, 50000000},
            {"claude-opus-5-5",   4000000,  5000000,  200000, 20000000},
            {"claude-opus-5",     5000000,  6250000,  500000, 25000000},
            {"claude-opus-4-8",   5000000,  6250000,  500000, 25000000},
            {"claude-opus-4-7",   5000000,  6250000,  500000, 25000000},
            {"claude-opus-4-6",   5000000,  6250000,  500000, 25000000},
            {"claude-opus-4-5",   5000000,  6250000,  500000, 25000000},
            {"claude-opus-4-1",  15000000, 18750000, 1500000, 75000000},
            {"claude-opus-4",    15000000, 18750000, 1500000, 75000000},
            {"claude-sonnet-5-5", 2000000,  2500000,  100000, 10000000},
            {"claude-sonnet-5",   2000000,  2500000,  200000, 10000000},
            {"claude-sonnet-4-6", 3000000,  3750000,  300000, 15000000},
            {"claude-sonnet-4-5", 3000000,  3750000,  300000, 15000000},
            {"claude-sonnet-4",   3000000,  3750000,  300000, 15000000},
            {"claude-haiku-4-5",  1000000,  1250000,  100000,  5000000},
        };
        i64 M = 1000000;
        for (iz i = 0; i < countof(rows); i++) {
            Arena s = *a;
            Str   n = rows[i].id;
            expect(t, concat(&s, "in ", n), istr(&s, ptcost(&s, n, ptuse(M, 0, 0, 0))),
                   istr(&s, rows[i].in));
            expect(t, concat(&s, "written ", n), istr(&s, ptcost(&s, n, ptuse(M, 0, M, 0))),
                   istr(&s, rows[i].written));
            expect(t, concat(&s, "cached ", n), istr(&s, ptcost(&s, n, ptuse(M, M, 0, 0))),
                   istr(&s, rows[i].cached));
            expect(t, concat(&s, "out ", n), istr(&s, ptcost(&s, n, ptuse(0, 0, 0, M))),
                   istr(&s, rows[i].out));
        }
    }

    // A request's parts together: in counts the cache's reads and writes
    {
        Usage u = ptuse(1000000, 200000, 100000, 10000);
        // 700000*3 + 100000*3.75 + 200000*0.30 + 10000*15
        expect(t, "cost parts", istr(a, ptcost(a, "claude-sonnet-4-5", u)), "2685000");
        // Rounded to the microusd, each request on its own
        expect(t, "cost one out", istr(a, ptcost(a, "claude-opus-4-5", ptuse(0, 0, 0, 1))), "25");
        expect(t, "cost one cached", istr(a, ptcost(a, "claude-sonnet-4-5", ptuse(1, 1, 0, 0))), "0");
        expect(t, "cost two cached", istr(a, ptcost(a, "claude-sonnet-4-5", ptuse(2, 2, 0, 0))), "1");
        // A count past in (a server's odd report) costs nothing below 0
        expect(t, "cost cache past in", istr(a, ptcost(a, "claude-sonnet-4-5", ptuse(0, 10, 0, 0))), "3");
        // Haiku 5.5: five times the price past 100,000 tokens of prompt,
        // the cache's included
        expect(t, "haiku at 100000", istr(a, ptcost(a, "claude-haiku-5-5", ptuse(100000, 0, 0, 0))),
               "10000");
        expect(t, "haiku at 100001", istr(a, ptcost(a, "claude-haiku-5-5", ptuse(100001, 0, 0, 0))),
               "50001");
        expect(t, "haiku cached past", istr(a, ptcost(a, "claude-haiku-5-5",
                                                       ptuse(200000, 200000, 0, 1000))),
               "12500");  // (200000*0.01 + 1000*0.5) * 5
        // Searches, $0.01 each, at no multiple
        expect(t, "searches", istr(a, ptcost(a, "claude-haiku-5-5", ptuse(1000, 0, 0, 100), 2)),
               "20150");
    }

    // [model] price
    {
        expect(t, "parse empty", ptparse(a, ""), "ok unset");
        expect(t, "parse none", ptparse(a, "none"), "ok none");
        expect(t, "parse free", ptparse(a, "0"), "ok set 0 0 0 0");
        expect(t, "parse two", ptparse(a, "3 15"), "ok set 3 15 0.3 3.75");
        expect(t, "parse three", ptparse(a, " 3 15 0.5 "), "ok set 3 15 0.5 3.75");
        expect(t, "parse four", ptparse(a, "3\t15 0.3 4"), "ok set 3 15 0.3 4");
        expect(t, "parse zeros", ptparse(a, "0 0"), "ok set 0 0 0 0");
        expect(t, "parse one", ptparse(a, "3"), "bad unset");
        expect(t, "parse five", ptparse(a, "1 2 3 4 5"), "bad unset");
        expect(t, "parse negative", ptparse(a, "-1 2"), "bad unset");
        expect(t, "parse word", ptparse(a, "cheap"), "bad unset");
        expect(t, "parse dollar", ptparse(a, "$3 $15"), "bad unset");
        expect(t, "parse huge", ptparse(a, "1e9 1"), "bad unset");
    }

    // Which price a request takes: the setting, then the endpoint's
    // kind, then the table by the model that answered, then by the id
    {
        Usage u = ptuse(1000000, 0, 0, 0);
        Price cfg = {};
        priceparse("2 4", &cfg);
        Price none = {};
        priceparse("none", &none);
        Price unset = {};
        struct { Str name; Price *cfg; i32 kind; Str answered; Str asked; i64 want; } cases[] = {
            {"setting", &cfg, EP_OPENAI, {}, "local", 2000000},
            {"setting over table", &cfg, EP_ANTHROPIC, {}, "claude-opus-4-5", 2000000},
            {"none over table", &none, EP_ANTHROPIC, {}, "claude-opus-4-5", -1},
            {"setting over plan", &cfg, EP_CHATGPT, {}, "gpt-5", 2000000},
            {"builtin free", &unset, EP_BUILTIN, {}, "gemma", 0},
            {"chatgpt plan", &unset, EP_CHATGPT, {}, "claude-opus-4-5", -1},
            {"copilot plan", &unset, EP_COPILOT, "claude-opus-4-5", "claude-opus-4-5", -1},
            {"table by id", &unset, EP_OPENAI, {}, "claude-opus-4-5", 5000000},
            {"table by answer", &unset, EP_OPENAI, "claude-opus-4-1-20250805", "large", 15000000},
            {"answer over id", &unset, EP_ANTHROPIC, "claude-haiku-4-5-20251001",
             "claude-opus-4-5", 1000000},
            {"unknown answer, id", &unset, EP_OPENAI, "x-1", "claude-opus-4-5", 5000000},
            {"unknown", &unset, EP_OPENAI, {}, "qwen3", -1},
            {"localhost not free", &unset, EP_OPENAI, {}, "large", -1},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena s = *a;
            Price p = pricefor(s, cases[i].cfg, cases[i].kind, cases[i].answered,
                               cases[i].asked);
            expect(t, concat(&s, "pricefor ", cases[i].name), istr(&s, costof(&p, &u, 0)),
                   istr(&s, cases[i].want));
        }
    }

    // The model a response names, kept from its first event
    {
        Arena    s = *a;
        Partial *p = newpartial(&s);
        JsonDoc  d = jsonparse(&s, "{\"type\":\"message_start\",\"message\":{\"id\":\"m\","
                                   "\"model\":\"claude-opus-4-5-20251101\",\"usage\":"
                                   "{\"input_tokens\":3,\"output_tokens\":1}}}");
        anchunk(p, d.root, &s);
        expect(t, "anthropic model", p->model, "claude-opus-4-5-20251101");

        p = newpartial(&s);
        d = jsonparse(&s, "{\"id\":\"c\",\"model\":\"claude-sonnet-4-5\",\"choices\":"
                          "[{\"index\":0,\"delta\":{\"content\":\"hi\"}}]}");
        applychunk(p, d.root, &s);
        d = jsonparse(&s, "{\"id\":\"c\",\"model\":\"other\",\"choices\":"
                          "[{\"index\":0,\"delta\":{\"content\":\"!\"}}]}");
        applychunk(p, d.root, &s);
        expect(t, "chat model", p->model, "claude-sonnet-4-5");

        p = newpartial(&s);
        d = jsonparse(&s, "{\"type\":\"response.created\",\"response\":{\"id\":\"r\","
                          "\"model\":\"gpt-5\",\"status\":\"in_progress\"}}");
        rschunk(p, d.root, &s);
        expect(t, "responses model", p->model, "gpt-5");
    }
}
