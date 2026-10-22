// Anthropic tests: request encoding (merging, reasoning, breakpoints,
// parameters), streamed events, error classification and the listing

// A request's messages in brief: each "role:block,block" with a * on a
// block with a breakpoint, separated by " | ".
static Str anshape(Arena *a, Str body)
{
    JsonDoc d = jsonparse(a, body);
    Buf     b(a, 256);
    for (Json *m = jsonget(d.root, "messages")->child; m; m = m->next) {
        if (b.len) print(&b, " | ");
        print(&b, jsonstr(jsonget(m, "role")));
        putbyte(&b, ':');
        for (Json *k = jsonget(m, "content")->child; k; k = k->next) {
            if (k != jsonget(m, "content")->child) putbyte(&b, ',');
            print(&b, jsonstr(jsonget(k, "type")));
            if (jsonget(k, "cache_control")->type == JSON_OBJECT) {
                putbyte(&b, '*');
            }
        }
    }
    return finish(&b);
}

// A request with its breakpoints taken out, which the cache ignores.
static Str anplain(Arena *a, Str body)
{
    Buf b(a, body.len);
    Str mark = ",\"cache_control\":{\"type\":\"ephemeral\"}";
    for (iz i = 0; i < body.len;) {
        Str rest = cuthead(body, i);
        if (startswith(rest, mark)) {
            i += mark.len;
        } else {
            putbyte(&b, body[i++]);
        }
    }
    return finish(&b);
}

// Feed events (JSON texts) into a partial response, the deltas shown as
// "block:kind:text" separated by spaces.
static Partial *anfeed(Arena *a, Str *events, iz n, Str *deltas)
{
    Partial *p = newpartial(a);
    Buf      b(a, 256);
    for (iz i = 0; i < n; i++) {
        JsonDoc d = jsonparse(a, events[i]);
        Chunk   c = anchunk(p, d.root, a);
        for (iz k = 0; k < c.deltas.len; k++) {
            Delta *x = &c.deltas[k];
            if (b.len) putbyte(&b, ' ');
            print(&b, (i64)x->block);
            putbyte(&b, ':');
            print(&b, x->kind==DELTA_TEXT ? Str("text") :
                      x->kind==DELTA_THINK ? Str("think") : Str("args"));
            putbyte(&b, ':');
            print(&b, x->id.len ? concat(a, x->id, "/") : Str(""));
            print(&b, x->name.len ? concat(a, x->name, "/") : Str(""));
            print(&b, x->text);
        }
    }
    if (deltas) {
        *deltas = finish(&b);
    }
    return p;
}

static Str anblocks(Arena *a, Slice<Block> blocks)
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
        if (k->sig.len) {
            putbyte(&b, '#');
            print(&b, k->sig);
        }
    }
    return finish(&b);
}

static void test_anthropic(Test *t)
{
    Arena *a = &t->perm;

    // Requests: the history merged into alternating messages, reasoning
    // verbatim where it may go, and four breakpoints
    {
        ModelConf mc = {};
        mc.name = "claude-test";
        Block asst1[4] = {};
        asst1[0].type = BLOCK_THINK;
        asst1[0].text = "plan";
        asst1[0].sig  = "SIG1";
        asst1[1].type = BLOCK_TEXT;
        asst1[1].text = "  \n";  // blank: left out
        asst1[2].type = BLOCK_CALL;
        asst1[2].id   = "toolu_1";
        asst1[2].name = "read";
        asst1[2].args = "{\"path\":\"a.c\"}";
        asst1[3].type = BLOCK_CALL;
        asst1[3].id   = "toolu_2";
        asst1[3].name = "shell";
        asst1[3].args = "{\"command\":\"ls\"";  // repaired on the way
        Block asst2[2] = {};
        asst2[0].type = BLOCK_THINK;  // another model's: no signature
        asst2[0].text = "hmm";
        asst2[1].type = BLOCK_REDACTED;
        asst2[1].text = "OPAQUE";
        Block asst3[3] = {};
        asst3[0].type = BLOCK_REDACTED;
        asst3[0].text = "OPAQUE2";
        asst3[1].type = BLOCK_TEXT;
        asst3[1].text = "Done.";
        asst3[2].type = BLOCK_THINK;
        asst3[2].text = "";
        asst3[2].sig  = "SIG3";
        Msg h[8] = {};
        h[0].role = ROLE_USER;
        h[0].text = "fix it";
        h[0].seq  = 1;
        h[1].role   = ROLE_ASSISTANT;
        h[1].blocks = {asst1, 4, 4};
        h[1].seq    = 2;
        h[2].role   = ROLE_TOOL;
        h[2].callid = "toolu_1";
        h[2].text   = "int x;";
        h[2].seq    = 3;
        h[3].role   = ROLE_USER;  // a notice, between the results
        h[3].text   = "job 1 ended";
        h[3].seq    = 4;
        h[4].role   = ROLE_TOOL;
        h[4].callid = "toolu_2";
        h[4].text   = "denied";
        h[4].error  = 1;
        h[4].seq    = 5;
        h[5].role   = ROLE_ASSISTANT;  // reasoning only: left out
        h[5].blocks = {asst2, 2, 2};
        h[5].seq    = 6;
        h[6].role   = ROLE_USER;
        h[6].text   = "go on";
        h[6].seq    = 7;
        h[7].role   = ROLE_ASSISTANT;
        h[7].blocks = {asst3, 3, 3};
        h[7].seq    = 8;

        ChatReq r = {};
        r.mc      = &mc;
        r.system  = "You are an agent.";
        r.tools   = "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
                    "\"description\":\"Read a file\",\"parameters\":{\"type\":"
                    "\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}},"
                    "{\"type\":\"function\",\"function\":{\"name\":\"shell\","
                    "\"description\":\"Run\",\"parameters\":{\"type\":\"object\"}}}]";
        r.history = {h, 7, 7};
        Arena tmp = *a;
        tmp.beg  += (tmp.end - tmp.beg)/2;

        Str one = anencode(a, tmp, &r);
        Str two = anencode(a, tmp, &r);
        expect(t, "an.deterministic", istr(a, one==two), "1");
        expect(t, "an.shape", anshape(a, one),
               "user:text* | assistant:thinking,tool_use,tool_use | "
               "user:tool_result,tool_result,text,text*");
        expect(t, "an.head", takehead(one, find(one, ",\"messages\"")),
               "{\"model\":\"claude-test\",\"max_tokens\":32000,\"system\":["
               "{\"type\":\"text\",\"text\":\"You are an agent.\","
               "\"cache_control\":{\"type\":\"ephemeral\"}}]");
        expect(t, "an.tail", cuthead(one, find(one, ",\"tools\"")),
               ",\"tools\":[{\"name\":\"read\",\"description\":\"Read a file\","
               "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":"
               "{\"type\":\"string\"}}},\"eager_input_streaming\":true},"
               "{\"name\":\"shell\",\"description\":\"Run\",\"input_schema\":"
               "{\"type\":\"object\"},\"eager_input_streaming\":true,"
               "\"cache_control\":{\"type\":\"ephemeral\"}}],"
               "\"tool_choice\":{\"type\":\"auto\"},\"thinking\":{\"type\":"
               "\"adaptive\",\"display\":\"summarized\"},\"stream\":true}");

        JsonDoc d  = jsonparse(a, one);
        Json   *ms = jsonget(d.root, "messages");
        Json   *as = jsonget(jsonat(ms, 1), "content");
        expect(t, "an.thinking", jsonat(as, 0)->raw,
               "{\"type\":\"thinking\",\"thinking\":\"plan\",\"signature\":\"SIG1\"}");
        expect(t, "an.tool_use", jsonat(as, 2)->raw,
               "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"shell\","
               "\"input\":{\"command\":\"ls\"}}");
        Json *us = jsonget(jsonat(ms, 2), "content");
        expect(t, "an.tool_result", jsonat(us, 1)->raw,
               "{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_2\","
               "\"content\":\"denied\",\"is_error\":true}");
        expect(t, "an.tool_result ok", istr(a, jsonget(jsonat(us, 0), "is_error")->type),
               istr(a, JSON_MISSING));

        // The response came: the last message moves, and the one before
        // it keeps the breakpoint the previous request wrote. Unsigned
        // reasoning stays out, redacted goes as it came, and empty but
        // signed reasoning goes too.
        r.history = {h, 8, 8};
        Str three = anencode(a, tmp, &r);
        expect(t, "an.shape after", anshape(a, three),
               "user:text | assistant:thinking,tool_use,tool_use | "
               "user:tool_result,tool_result,text,text* | "
               "assistant:redacted_thinking,text*,thinking");
        // ... except that a breakpoint cannot sit on reasoning
        JsonDoc d3   = jsonparse(a, three);
        Json   *last = jsonat(jsonget(d3.root, "messages"), 3);
        expect(t, "an.no breakpoint on reasoning",
               jsonat(jsonget(last, "content"), 1)->raw,
               "{\"type\":\"text\",\"text\":\"Done.\",\"cache_control\":"
               "{\"type\":\"ephemeral\"}}");

        // The prefix is stable, apart from breakpoints: the system prompt
        // and tools, and every message of the first request, are the same
        // in the second
        Str   pl1  = anplain(a, one);
        Str   pl3  = anplain(a, three);
        Json *m1   = jsonget(jsonparse(a, pl1).root, "messages");
        Json *m3   = jsonget(jsonparse(a, pl3).root, "messages");
        b32   same = jsonlen(m1)==3 && jsonlen(m3)==4;
        for (iz i = 0; i < jsonlen(m1); i++) {
            same &= jsonat(m1, i)->raw == jsonat(m3, i)->raw;
        }
        same &= takehead(pl1, find(pl1, ",\"messages\"")) ==
                takehead(pl3, find(pl3, ",\"messages\""));
        same &= cuthead(pl1, find(pl1, ",\"tools\"")) ==
                cuthead(pl3, find(pl3, ",\"tools\""));
        expect(t, "an.prefix", istr(a, same), "1");

        // Reasoning from before a compaction stays out
        r.thinkfrom = 5;
        expect(t, "an.thinkfrom", anshape(a, anencode(a, tmp, &r)),
               "user:text | assistant:tool_use,tool_use | "
               "user:tool_result,tool_result,text,text* | "
               "assistant:redacted_thinking,text*,thinking");
        r.thinkfrom = 0;

        // Parameters
        mc.maxtokens = 100000;
        mc.maxout    = 64000;
        mc.think     = THINK_BUDGET;
        mc.thinkmax  = 2000;
        mc.effort    = "high";
        Extra x[3] = {{"temperature", "0.5"}, {"min_p", "0.1"}, {"max_tokens", "9"}};
        mc.extras = {x, 3, 3};
        Str four = anencode(a, tmp, &r);
        JsonDoc d4 = jsonparse(a, four);
        expect(t, "an.max_tokens", jsonget(d4.root, "max_tokens")->raw, "64000");
        expect(t, "an.budget", jsonget(d4.root, "thinking")->raw,
               "{\"type\":\"enabled\",\"budget_tokens\":2000}");
        expect(t, "an.effort", jsonget(d4.root, "output_config")->raw,
               "{\"effort\":\"high\"}");
        expect(t, "an.temperature", jsonget(d4.root, "temperature")->raw, "0.5");
        expect(t, "an.no min_p", istr(a, find(four, "min_p")), "-1");
        mc.thinkmax = 0;
        mc.maxout   = 512;
        d4 = jsonparse(a, anencode(a, tmp, &r));
        expect(t, "an.budget too small", istr(a, jsonget(d4.root, "thinking")->type),
               istr(a, JSON_MISSING));
        mc.think = THINK_OFF;
        d4 = jsonparse(a, anencode(a, tmp, &r));
        expect(t, "an.off", jsonget(d4.root, "thinking")->raw, "{\"type\":\"disabled\"}");
        mc.think = THINK_DEFAULT;
        d4 = jsonparse(a, anencode(a, tmp, &r));
        expect(t, "an.default", istr(a, jsonget(d4.root, "thinking")->type),
               istr(a, JSON_MISSING));
        mc.think     = THINK_ADAPTIVE;
        mc.hidethink = 1;
        d4 = jsonparse(a, anencode(a, tmp, &r));
        expect(t, "an.omitted", jsonget(d4.root, "thinking")->raw,
               "{\"type\":\"adaptive\",\"display\":\"omitted\"}");

        // No tools, no system: no tool_choice, two breakpoints at most
        ChatReq bare = {};
        bare.mc      = &mc;
        bare.history = {h, 1, 1};
        Str five = anencode(a, tmp, &bare);
        expect(t, "an.bare", anshape(a, five), "user:text*");
        expect(t, "an.bare tools", istr(a, find(five, "tool")), "-1");
        expect(t, "an.bare system", istr(a, find(five, "system")), "-1");

        // A paused response goes back last, as it is
        Msg paused[2] = {h[0], h[1]};
        bare.history = {paused, 2, 2};
        expect(t, "an.paused", anshape(a, anencode(a, tmp, &bare)),
               "user:text* | assistant:thinking,tool_use,tool_use*");
    }

    // A streamed response: reasoning with its signature, text, a call in
    // pieces, the stop reason and the usage, cache counts included
    {
        Str events[] = {
            "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":"
                "\"message\",\"role\":\"assistant\",\"content\":[],\"usage\":{"
                "\"input_tokens\":12,\"cache_creation_input_tokens\":100,"
                "\"cache_read_input_tokens\":2000,\"output_tokens\":1}}}",
            "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
                "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}",
            "{\"type\":\"ping\"}",
            "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                "\"thinking_delta\",\"thinking\":\"Look first.\"}}",
            "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                "\"signature_delta\",\"signature\":\"EqQB\"}}",
            "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                "\"signature_delta\",\"signature\":\"x==\"}}",
            "{\"type\":\"content_block_stop\",\"index\":0}",
            "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
                "{\"type\":\"text\",\"text\":\"\"}}",
            "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
                "\"text_delta\",\"text\":\"Reading.\"}}",
            "{\"type\":\"content_block_stop\",\"index\":1}",
            "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":"
                "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"read\","
                "\"input\":{}}}",
            "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
                "\"input_json_delta\",\"partial_json\":\"\"}}",
            "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
                "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\": \"}}",
            "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
                "\"input_json_delta\",\"partial_json\":\"\\\"a.c\\\"}\"}}",
            "{\"type\":\"content_block_stop\",\"index\":2}",
            "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\","
                "\"stop_sequence\":null},\"usage\":{\"output_tokens\":42}}",
            "{\"type\":\"message_stop\"}",
        };
        Str      deltas = {};
        Partial *p      = anfeed(a, events, countof(events), &deltas);
        expect(t, "an.deltas", deltas,
               "0:think:Look first. 1:text:Reading. 2:args:toolu_9/read/ "
               "2:args:{\"path\":  2:args:\"a.c\"}");
        Finished f = finishpartial(a, p, 1, 1, 1);
        expect(t, "an.blocks", anblocks(a, f.blocks),
               "think:Look first.#EqQBx==|text:Reading.|call:toolu_9,read,"
               "{\"path\": \"a.c\"}");
        expect(t, "an.finish", p->finish, "tool_calls");
        expect(t, "an.done", istr(a, p->done), "1");
        expect(t, "an.usage",
               concat(a, concat(a, istr(a, p->usage.in), " "),
                      concat(a, concat(a, istr(a, p->usage.cached), " "),
                             concat(a, concat(a, istr(a, p->usage.written), " "),
                                    istr(a, p->usage.out)))),
               "2112 2000 100 42");
        expect(t, "an.committed", istr(a, committed(p)), "1");

        // Cut short: the call that did not end is left out, the rest kept
        f = finishpartial(a, anfeed(a, events, 13, 0), 0, 1, 1);
        expect(t, "an.incomplete call", anblocks(a, f.blocks),
               "think:Look first.#EqQBx==|text:Reading.");

        // A cycle cut away from the text: what follows the cut goes too
        Str two[] = {
            "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
                "{\"type\":\"text\",\"text\":\"abc\"}}",
            "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
                "{\"type\":\"redacted_thinking\",\"data\":\"QUJD\"}}",
            "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":"
                "{\"type\":\"text\",\"text\":\"defgh\"}}",
            "{\"type\":\"content_block_start\",\"index\":3,\"content_block\":"
                "{\"type\":\"text\",\"text\":\"ijk\"}}",
        };
        p = anfeed(a, two, countof(two), 0);
        p->text.len = 5;
        f = finishpartial(a, p, 1, 1, 1);
        expect(t, "an.cut", anblocks(a, f.blocks), "text:abc|redacted:QUJD|text:de");
    }

    // Stop reasons, refusals, errors, and a response not streamed
    {
        struct { Str reason, want; } stops[] = {
            {"end_turn",                      "stop"},
            {"stop_sequence",                 "stop"},
            {"tool_use",                      "tool_calls"},
            {"max_tokens",                    "length"},
            {"model_context_window_exceeded", "length"},
            {"pause_turn",                    "pause_turn"},
            {"refusal",                       "refusal"},
        };
        for (iz i = 0; i < countof(stops); i++) {
            Buf b(a, 128);
            print(&b, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"");
            print(&b, stops[i].reason);
            print(&b, "\"},\"usage\":{\"output_tokens\":1}}");
            Str      ev = finish(&b);
            Partial *p  = anfeed(a, &ev, 1, 0);
            expect(t, stops[i].reason, p->finish, stops[i].want);
        }

        Str refusal = "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
                      "\"refusal\",\"stop_details\":{\"type\":\"refusal\","
                      "\"category\":\"cyber\",\"explanation\":\"Not this.\"}},"
                      "\"usage\":{\"output_tokens\":3}}";
        expect(t, "an.refusal", anfeed(a, &refusal, 1, 0)->refusal,
               "category cyber: Not this.");

        Partial *q = newpartial(a);
        JsonDoc  e = jsonparse(a, "{\"type\":\"error\",\"error\":{\"type\":"
                                  "\"overloaded_error\",\"message\":\"Overloaded\"}}");
        Chunk    c = anchunk(q, e.root, a);
        expect(t, "an.error", c.error, "overloaded_error: Overloaded");
        expect(t, "an.error class", classname(classify(0, c.error)), "transient");
        JsonDoc bad = jsonparse(a, "[]");
        expect(t, "an.bad", istr(a, anchunk(q, bad.root, a).bad), "1");

        Str whole = "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\","
                    "\"content\":[{\"type\":\"text\",\"text\":\"Hi.\"},{\"type\":"
                    "\"tool_use\",\"id\":\"toolu_3\",\"name\":\"read\",\"input\":"
                    "{\"path\":\"b\"}}],\"stop_reason\":\"tool_use\",\"usage\":{"
                    "\"input_tokens\":5,\"output_tokens\":7}}";
        Partial *w = anfeed(a, &whole, 1, 0);
        Finished f = finishpartial(a, w, 0, 1, 1);
        expect(t, "an.whole", anblocks(a, f.blocks),
               "text:Hi.|call:toolu_3,read,{\"path\":\"b\"}");
        expect(t, "an.whole done", concat(a, w->finish, istr(a, w->done)), "tool_calls1");
    }

    // Errors as the API reports them
    {
        struct { i32 status; Str body, want; } cases[] = {
            {529, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
                  "\"message\":\"Overloaded\"}}", "transient"},
            {500, "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                  "\"message\":\"Internal server error\"}}", "transient"},
            {429, "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\","
                  "\"message\":\"Number of request tokens has exceeded your "
                  "per-minute rate limit\"}}", "transient"},
            {400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                  "\"message\":\"prompt is too long: 208310 tokens > 200000 "
                  "maximum\"}}", "overflow"},
            {413, "{\"type\":\"error\",\"error\":{\"type\":\"request_too_large\","
                  "\"message\":\"Request exceeds the maximum allowed number of "
                  "bytes.\"}}", "overflow"},
            {400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                  "\"message\":\"messages.1.content.0.type: Expected `thinking` "
                  "or `redacted_thinking`\"}}", "deterministic"},
            {400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                  "\"message\":\"Your credit balance is too low to access the "
                  "Anthropic API.\"}}", "limit"},
            {402, "{\"type\":\"error\",\"error\":{\"type\":\"billing_error\","
                  "\"message\":\"x\"}}", "limit"},
            {401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
                  "\"message\":\"invalid x-api-key\"}}", "fatal"},
            {403, "{\"type\":\"error\",\"error\":{\"type\":\"permission_error\","
                  "\"message\":\"no\"}}", "fatal"},
            {404, "{\"type\":\"error\",\"error\":{\"type\":\"not_found_error\","
                  "\"message\":\"model: claude-nope\"}}", "deterministic"},
            {0, "invalid_request_error: bad", "deterministic"},
            {0, "authentication_error: bad", "fatal"},
            {0, "api_error: Internal server error", "transient"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Str name = concat(a, "an.class ", istr(a, cases[i].status));
            name = concat(a, concat(a, name, " "), istr(a, i));
            expect(t, name, classname(classify(cases[i].status, cases[i].body)),
                   cases[i].want);
        }
        Overflow o = overflowsizes(*a, cases[3].body);
        expect(t, "an.overflow sizes",
               concat(a, concat(a, istr(a, o.prompt), "/"), istr(a, o.ctx)),
               "208310/200000");
        expect(t, "an.message", errormessage(a, cases[8].body),
               "authentication_error: invalid x-api-key");
    }

    // The listing gives each model's limits outright
    {
        Str body = "{\"data\":[{\"type\":\"model\",\"id\":\"claude-haiku-5-5\","
                   "\"display_name\":\"Claude Haiku 5.5\",\"created_at\":"
                   "\"2026-09-01T00:00:00Z\",\"max_input_tokens\":1000000,"
                   "\"max_tokens\":128000},{\"type\":\"model\",\"id\":\"old\"}],"
                   "\"has_more\":false,\"first_id\":\"claude-haiku-5-5\","
                   "\"last_id\":\"old\"}";
        ModelInfo mi = parsemodels(*a, body, "claude-haiku-5-5", 0);
        expect(t, "an.models",
               concat(a, concat(a, istr(a, mi.found*100 + mi.learned*10 +
                                       mi.llamacpp), " "),
                      concat(a, concat(a, istr(a, mi.ctx), " "), istr(a, mi.maxout))),
               "110 1000000 128000");
        mi = parsemodels(*a, body, "old", 0);
        expect(t, "an.models unknown",
               concat(a, istr(a, mi.found*100 + mi.learned*10), istr(a, mi.ctx)),
               "1000");
    }

    // Tool results that fail
    {
        Str failed[] = {"error", "denied", "timeout", "cancelled", "unknown"};
        Str fine[]   = {"ok", "background"};
        for (iz i = 0; i < countof(failed); i++) {
            expect(t, failed[i], istr(a, toolfailed(failed[i])), "1");
        }
        for (iz i = 0; i < countof(fine); i++) {
            expect(t, fine[i], istr(a, toolfailed(fine[i])), "0");
        }
        expect(t, "an.blocktype", blockname(blocktype("redacted")), "redacted");
    }
}
