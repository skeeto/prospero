// Responses API tests: request encoding (items, reasoning, parameters,
// a stable prefix), streamed events from OpenAI and llama.cpp, whole
// responses, failures, and the kinds' URLs

// A request's input in brief: each item "type" or "role:type", with
// "+enc" for reasoning with encrypted content, separated by " | ".
static Str rsshape(Arena *a, Str body)
{
    JsonDoc d = jsonparse(a, body);
    Buf     b(a, 256);
    for (Json *m = jsonget(d.root, "input")->child; m; m = m->next) {
        if (b.len) print(&b, " | ");
        Str role = jsonstr(jsonget(m, "role"));
        if (role.len) {
            print(&b, role);
            putbyte(&b, ':');
        }
        print(&b, jsonstr(jsonget(m, "type")));
        if (jsonstr(jsonget(m, "encrypted_content")).len) {
            print(&b, "+enc");
        }
    }
    return finish(&b);
}

// Feed events (JSON texts) into a partial response, the deltas shown as
// "block:kind:text" separated by spaces, and any error as "!ERROR".
static Partial *rsfeed(Arena *a, Str *events, iz n, Str *deltas)
{
    Partial *p = newpartial(a);
    Buf      b(a, 256);
    for (iz i = 0; i < n; i++) {
        JsonDoc d = jsonparse(a, events[i]);
        Chunk   c = rschunk(p, d.root, a);
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
        if (c.error.data) {
            if (b.len) putbyte(&b, ' ');
            putbyte(&b, '!');
            print(&b, c.error);
        }
    }
    if (deltas) {
        *deltas = finish(&b);
    }
    return p;
}

static void test_responses(Test *t)
{
    Arena *a = &t->perm;

    // Kinds, their paths and URLs
    {
        expect(t, "rs.kinds", concat(a, concat(a, epkindname(epkindof("responses")), ","),
                                      epkindname(epkindof("chatgpt"))), "responses,chatgpt");
        expect(t, "rs.kind unknown", epkindname(epkindof("nosuch")), "openai");
        expect(t, "rs.path", eppath(EP_CHATGPT), "responses");
        expect(t, "rs.url openai", istr(a, epkindurl(EP_OPENAI).len), "0");
        Str gpt = epkindurl(EP_CHATGPT);
        expect(t, "rs.chatgpt call", apiurl(a, gpt, eppath(EP_CHATGPT)),
               "https://chatgpt.com/backend-api/codex/responses");
        expect(t, "rs.chatgpt models", apiurl(a, gpt, "models"),
               "https://chatgpt.com/backend-api/codex/models");
        expect(t, "rs.openai call", apiurl(a, epkindurl(EP_RESPONSES), "responses"),
               "https://api.openai.com/v1/responses");
        expect(t, "rs.local call", apiurl(a, "http://127.0.0.1:8080", "responses"),
               "http://127.0.0.1:8080/v1/responses");
    }

    // Requests: items in order, reasoning with its encrypted content,
    // screenshots after the results, and a stable prefix
    {
        ModelConf mc = {};
        mc.name      = "gpt-test";
        mc.reasoning = "reasoning_content";
        Block asst1[4] = {};
        asst1[0].type = BLOCK_THINK;
        asst1[0].text = "plan";
        asst1[0].sig  = "ENC1";
        asst1[1].type = BLOCK_TEXT;
        asst1[1].text = " \n";  // blank: left out
        asst1[2].type = BLOCK_CALL;
        asst1[2].id   = "call_1";
        asst1[2].name = "read";
        asst1[2].args = "{\"path\":\"a.c\"}";
        asst1[3].type = BLOCK_CALL;
        asst1[3].id   = "call_2";
        asst1[3].name = "desktop";
        asst1[3].args = "{\"action\":\"screenshot\"";  // repaired on the way
        Block asst2[2] = {};
        asst2[0].type = BLOCK_THINK;  // another model's: no encrypted content
        asst2[0].text = "hmm";
        asst2[1].type = BLOCK_TEXT;
        asst2[1].text = "Done.";
        MsgImage img = {};
        img.mime = "image/png";
        img.b64  = "QUJD";
        Msg h[6] = {};
        h[0].role   = ROLE_USER;
        h[0].text   = "fix it";
        h[0].images = {&img, 1, 1};
        h[1].role   = ROLE_ASSISTANT;
        h[1].blocks = {asst1, 4, 4};
        h[2].role   = ROLE_TOOL;
        h[2].callid = "call_1";
        h[2].text   = "int x;";
        h[3].role   = ROLE_TOOL;
        h[3].callid = "call_2";
        h[3].text   = "a screenshot";
        h[3].imgb64 = "UE5H";
        h[4].role   = ROLE_ASSISTANT;
        h[4].blocks = {asst2, 2, 2};
        h[5].role   = ROLE_USER;
        h[5].text   = "thanks";

        ChatReq r = {};
        r.mc       = &mc;
        r.system   = "You are an agent.";
        r.tools    = "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
                     "\"description\":\"Read a file\",\"parameters\":{\"type\":"
                     "\"object\"}}}]";
        r.history  = {h, 4, 4};
        r.cachekey = "sess1";
        Arena tmp = *a;
        tmp.beg  += (tmp.end - tmp.beg)/2;

        Str one = rsencode(a, tmp, &r, 0);
        expect(t, "rs.deterministic", istr(a, one==rsencode(a, tmp, &r, 0)), "1");
        expect(t, "rs.shape", rsshape(a, one),
               "user:message | reasoning+enc | function_call | function_call | "
               "function_call_output | function_call_output | user:message");
        expect(t, "rs.head", takehead(one, find(one, ",\"input\"")),
               "{\"model\":\"gpt-test\",\"instructions\":\"You are an agent.\"");
        expect(t, "rs.tail", cuthead(one, find(one, ",\"tools\"")),
               ",\"tools\":[{\"type\":\"function\",\"name\":\"read\",\"description\":"
               "\"Read a file\",\"parameters\":{\"type\":\"object\"},\"strict\":false}],"
               "\"tool_choice\":\"auto\",\"parallel_tool_calls\":true,"
               "\"store\":false,\"stream\":true,"
               "\"include\":[\"reasoning.encrypted_content\"],"
               "\"prompt_cache_key\":\"sess1\"}");

        JsonDoc d  = jsonparse(a, one);
        Json   *in = jsonget(d.root, "input");
        expect(t, "rs.user", jsonat(in, 0)->raw,
               "{\"type\":\"message\",\"role\":\"user\",\"content\":["
               "{\"type\":\"input_text\",\"text\":\"fix it\"},"
               "{\"type\":\"input_image\",\"image_url\":\"data:image/png;base64,QUJD\"}]}");
        expect(t, "rs.reasoning", jsonat(in, 1)->raw,
               "{\"type\":\"reasoning\",\"summary\":[{\"type\":\"summary_text\","
               "\"text\":\"plan\"}],\"encrypted_content\":\"ENC1\"}");
        expect(t, "rs.call", jsonat(in, 3)->raw,
               "{\"type\":\"function_call\",\"call_id\":\"call_2\",\"name\":"
               "\"desktop\",\"arguments\":\"{\\\"action\\\":\\\"screenshot\\\"}\"}");
        expect(t, "rs.output", jsonat(in, 4)->raw,
               "{\"type\":\"function_call_output\",\"call_id\":\"call_1\","
               "\"output\":\"int x;\"}");
        expect(t, "rs.screenshot", jsonat(in, 6)->raw,
               "{\"type\":\"message\",\"role\":\"user\",\"content\":["
               "{\"type\":\"input_text\",\"text\":\"The screenshot of tool call call_2:\"},"
               "{\"type\":\"input_image\",\"image_url\":\"data:image/png;base64,UE5H\"}]}");

        // The response came: reasoning without encrypted content stays
        // out, and every item before is the same
        r.history = {h, 6, 6};
        Str  two  = rsencode(a, tmp, &r, 0);
        expect(t, "rs.shape after", rsshape(a, two),
               "user:message | reasoning+enc | function_call | function_call | "
               "function_call_output | function_call_output | user:message | "
               "assistant:message | user:message");
        Json *in2  = jsonget(jsonparse(a, two).root, "input");
        b32   same = takehead(one, find(one, ",\"input\"")) ==
                     takehead(two, find(two, ",\"input\""));
        for (iz i = 0; i < jsonlen(in); i++) {
            same &= jsonat(in, i)->raw == jsonat(in2, i)->raw;
        }
        expect(t, "rs.prefix", istr(a, same), "1");

        // ... but to llama.cpp it goes as text, as the model's settings
        // send reasoning back
        mc.llamacpp = 1;
        Str  three = rsencode(a, tmp, &r, 0);
        Json *in3  = jsonget(jsonparse(a, three).root, "input");
        expect(t, "rs.llama reasoning", jsonat(in3, 7)->raw,
               "{\"type\":\"reasoning\",\"summary\":[],\"content\":["
               "{\"type\":\"reasoning_text\",\"text\":\"hmm\"}]}");
        expect(t, "rs.llama progress",
               jsonget(jsonparse(a, three).root, "return_progress")->raw, "true");
        mc.reasoning = {};
        expect(t, "rs.llama none", rsshape(a, rsencode(a, tmp, &r, 0)),
               rsshape(a, two));
        mc.llamacpp = 0;

        // Parameters: effort, sampling, max_tokens renamed; none of the
        // sampling for ChatGPT's backend, which still gets summaries
        mc.effort = "high";
        Extra x[2] = {{"temperature", "0.5"}, {"max_tokens", "9"}};
        mc.extras = {x, 2, 2};
        JsonDoc d4 = jsonparse(a, rsencode(a, tmp, &r, 0));
        expect(t, "rs.effort", jsonget(d4.root, "reasoning")->raw,
               "{\"effort\":\"high\",\"summary\":\"auto\"}");
        expect(t, "rs.temperature", jsonget(d4.root, "temperature")->raw, "0.5");
        expect(t, "rs.max_output_tokens", jsonget(d4.root, "max_output_tokens")->raw, "9");
        mc.effort = {};
        JsonDoc d5 = jsonparse(a, rsencode(a, tmp, &r, 1));
        expect(t, "rs.codex reasoning", jsonget(d5.root, "reasoning")->raw,
               "{\"summary\":\"auto\"}");
        expect(t, "rs.codex sampling", istr(a, jsonget(d5.root, "temperature")->type),
               istr(a, JSON_MISSING));
        expect(t, "rs.codex max", istr(a, jsonget(d5.root, "max_output_tokens")->type),
               istr(a, JSON_MISSING));
    }

    // OpenAI's stream: items by output_index and item_id, summaries in
    // parts, encrypted content at the item's end, calls interleaved
    {
        Str ev[] = {
            "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}",
            "{\"type\":\"response.output_item.added\",\"output_index\":0,"
            "\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[]}}",
            "{\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"rs_1\","
            "\"output_index\":0,\"summary_index\":0}",
            "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_1\","
            "\"output_index\":0,\"delta\":\"First.\"}",
            "{\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"rs_1\","
            "\"output_index\":0,\"summary_index\":1}",
            "{\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_1\","
            "\"output_index\":0,\"delta\":\"Second.\"}",
            "{\"type\":\"response.output_item.done\",\"output_index\":0,"
            "\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[{\"type\":"
            "\"summary_text\",\"text\":\"First.\"}],\"encrypted_content\":\"ENC\"}}",
            "{\"type\":\"response.output_item.added\",\"output_index\":1,"
            "\"item\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
            "\"content\":[]}}",
            "{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\","
            "\"output_index\":1,\"delta\":\"Reading.\"}",
            "{\"type\":\"response.output_item.added\",\"output_index\":2,"
            "\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"call_id\":\"call_a\","
            "\"name\":\"read\",\"arguments\":\"\"}}",
            "{\"type\":\"response.output_item.added\",\"output_index\":3,"
            "\"item\":{\"id\":\"fc_2\",\"type\":\"function_call\",\"call_id\":\"call_b\","
            "\"name\":\"shell\",\"arguments\":\"\"}}",
            "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\","
            "\"output_index\":2,\"delta\":\"{\\\"path\\\":\"}",
            "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_2\","
            "\"output_index\":3,\"delta\":\"{}\"}",
            "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\","
            "\"output_index\":2,\"delta\":\"\\\"a\\\"}\"}",
            "{\"type\":\"response.output_item.done\",\"output_index\":2,"
            "\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\",\"call_id\":\"call_a\","
            "\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}",
            "{\"type\":\"response.output_item.done\",\"output_index\":3,"
            "\"item\":{\"id\":\"fc_2\",\"type\":\"function_call\",\"call_id\":\"call_b\","
            "\"name\":\"shell\",\"arguments\":\"{}\"}}",
            "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\","
            "\"status\":\"completed\",\"usage\":{\"input_tokens\":100,"
            "\"input_tokens_details\":{\"cached_tokens\":60},\"output_tokens\":20}}}",
        };
        Str      deltas = {};
        Partial *p      = rsfeed(a, ev, countof(ev), &deltas);
        expect(t, "rs.stream deltas", deltas,
               "0:think:First. 0:think:\n\n 0:think:Second. 1:text:Reading. "
               "2:args:call_a/read/ 3:args:call_b/shell/ 2:args:{\"path\": "
               "3:args:{} 2:args:\"a\"}");
        expect(t, "rs.stream id", p->id, "resp_1");
        expect(t, "rs.stream finish", p->finish, "tool_calls");
        expect(t, "rs.stream done", istr(a, p->done), "1");
        expect(t, "rs.stream usage", concat(a, concat(a, istr(a, p->usage.in), "/"),
                                            concat(a, concat(a, istr(a, p->usage.cached), "/"),
                                                   istr(a, p->usage.out))), "100/60/20");
        Finished f = finishpartial(a, p, 0, 1, 1);
        expect(t, "rs.stream blocks", anblocks(a, f.blocks),
               "think:First.\n\nSecond.#ENC|text:Reading.|call:call_a,read,{\"path\":\"a\"}|"
               "call:call_b,shell,{}");
    }

    // llama.cpp's stream: item ids alone, reasoning_text, progress on
    // any event, and the calls whole only at the end
    {
        Str ev[] = {
            "{\"type\":\"response.created\",\"response\":{\"id\":\"resp_x\"},"
            "\"prompt_progress\":{\"total\":50,\"processed\":10,\"cache\":5,\"time_ms\":7}}",
            "{\"type\":\"response.output_item.added\",\"item\":{\"id\":\"rs_x\","
            "\"type\":\"reasoning\",\"summary\":[],\"content\":[],\"encrypted_content\":\"\"}}",
            "{\"type\":\"response.reasoning_text.delta\",\"delta\":\"Think.\",\"item_id\":\"rs_x\"}",
            "{\"type\":\"response.output_item.added\",\"item\":{\"id\":\"fc_9\","
            "\"arguments\":\"\",\"call_id\":\"call_9\",\"name\":\"read\","
            "\"type\":\"function_call\"}}",
            "{\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{}\","
            "\"item_id\":\"fc_9\"}",
            "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"rs_x\","
            "\"type\":\"reasoning\",\"summary\":[],\"content\":[{\"text\":\"Think.\","
            "\"type\":\"reasoning_text\"}],\"encrypted_content\":\"\"}}",
            "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"fc_9\","
            "\"type\":\"function_call\",\"arguments\":\"{}\",\"call_id\":\"call_9\","
            "\"name\":\"read\"}}",
            "{\"type\":\"response.completed\",\"response\":{\"id\":\"resp_x\","
            "\"status\":\"completed\",\"output\":[{\"id\":\"rs_x\",\"type\":\"reasoning\","
            "\"content\":[{\"text\":\"Think.\",\"type\":\"reasoning_text\"}]},"
            "{\"id\":\"fc_9\",\"type\":\"function_call\",\"arguments\":\"{}\","
            "\"call_id\":\"call_9\",\"name\":\"read\"}],"
            "\"usage\":{\"input_tokens\":50,\"output_tokens\":4,"
            "\"input_tokens_details\":{\"cached_tokens\":5}}}}",
        };
        JsonDoc d = jsonparse(a, ev[0]);
        Partial *q = newpartial(a);
        Chunk    c = rschunk(q, d.root, a);
        expect(t, "rs.llama prefill", concat(a, istr(a, c.prefill),
                                             concat(a, "/", istr(a, c.pdone))), "1/10");
        Str      deltas = {};
        Partial *p      = rsfeed(a, ev, countof(ev), &deltas);
        expect(t, "rs.llama deltas", deltas,
               "0:think:Think. 1:args:call_9/read/ 1:args:{}");
        Finished f = finishpartial(a, p, 0, 1, 1);
        expect(t, "rs.llama blocks", anblocks(a, f.blocks), "think:Think.|call:call_9,read,{}");
        expect(t, "rs.llama finish", p->finish, "tool_calls");
    }

    // A response not streamed, and one whose items came only whole
    {
        Str ev[] = {
            "{\"object\":\"response\",\"id\":\"resp_w\",\"status\":\"completed\","
            "\"output\":[{\"id\":\"msg_w\",\"type\":\"message\",\"role\":\"assistant\","
            "\"content\":[{\"type\":\"output_text\",\"text\":\"Hello\"},"
            "{\"type\":\"output_text\",\"text\":\" there.\"}]}],"
            "\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        };
        Str      deltas = {};
        Partial *p      = rsfeed(a, ev, 1, &deltas);
        expect(t, "rs.whole deltas", deltas, "0:text:Hello there.");
        expect(t, "rs.whole finish", p->finish, "stop");
        expect(t, "rs.whole done", istr(a, p->done), "1");
    }

    // Stopping short, declining, and failing
    {
        Str ev1[] = {
            "{\"type\":\"response.output_text.delta\",\"item_id\":\"m\",\"delta\":\"Hal\"}",
            "{\"type\":\"response.incomplete\",\"response\":{\"status\":\"incomplete\","
            "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}",
        };
        expect(t, "rs.incomplete", rsfeed(a, ev1, 2, 0)->finish, "length");

        Str ev2[] = {
            "{\"type\":\"response.refusal.delta\",\"item_id\":\"m\",\"delta\":\"I can't.\"}",
            "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}",
        };
        Partial *p2 = rsfeed(a, ev2, 2, 0);
        expect(t, "rs.refusal", concat(a, concat(a, p2->finish, ": "), p2->refusal),
               "refusal: I can't.");

        Str ev3[] = {
            "{\"type\":\"response.failed\",\"response\":{\"status\":\"failed\","
            "\"error\":{\"code\":\"server_error\",\"message\":\"Try again.\"}}}",
        };
        Str got3 = {};
        rsfeed(a, ev3, 1, &got3);
        expect(t, "rs.failed", got3, "!server_error: Try again.");

        Str ev4[] = {
            "{\"type\":\"error\",\"code\":\"rate_limit_exceeded\",\"message\":\"Slow down.\"}",
        };
        Str got4 = {};
        rsfeed(a, ev4, 1, &got4);
        expect(t, "rs.error", got4, "!rate_limit_exceeded: Slow down.");

        Str ev5[] = {"[1]"};
        JsonDoc d5 = jsonparse(a, ev5[0]);
        expect(t, "rs.bad", istr(a, rschunk(newpartial(a), d5.root, a).bad), "1");
    }
    // ChatGPT: the Codex backend's listing, its usage limit, the claims
    // of its tokens, and the sign-in's URL
    {
        Arena tmp = *a;
        tmp.beg += (tmp.end - tmp.beg) / 2;  // another arena's room
        Str listing = rslisting(a, tmp, "{\"models\":[{\"slug\":\"gpt-5-codex\","
                                "\"context_window\":272000},{\"display_name\":\"x\"},"
                                "{\"slug\":\"gpt-5\"}]}");
        expect(t, "cg.listing", listing,
               "{\"object\":\"list\",\"data\":[{\"id\":\"gpt-5-codex\",\"object\":\"model\","
               "\"max_input_tokens\":272000},{\"id\":\"gpt-5\",\"object\":\"model\"}]}");
        ModelInfo mi = parsemodels(*a, listing, "gpt-5-codex", 0);
        expect(t, "cg.listing.ctx", istr(a, mi.found ? mi.ctx : -1), "272000");
        expect(t, "cg.listing.other", rslisting(a, tmp, "{\"data\":[]}"), "{\"data\":[]}");

        Str lim = "{\"error\":{\"type\":\"usage_limit_reached\",\"message\":\"x\","
                  "\"resets_in_seconds\":7500}}";
        expect(t, "cg.limit.in", istr(a, rsusagelimit(*a, lim, 1000)), "7500");
        Str at = "{\"error\":{\"type\":\"usage_limit_reached\",\"resets_at\":1600}}";
        expect(t, "cg.limit.at", istr(a, rsusagelimit(*a, at, 1000)), "600");
        Str no = "{\"error\":{\"type\":\"usage_limit_reached\"}}";
        expect(t, "cg.limit.unsaid", istr(a, rsusagelimit(*a, no, 1000)), "0");
        Str rate = "{\"error\":{\"type\":\"rate_limit_exceeded\",\"resets_in_seconds\":5}}";
        expect(t, "cg.limit.not", istr(a, rsusagelimit(*a, rate, 1000)), "-1");
        expect(t, "cg.limit.junk", istr(a, rsusagelimit(*a, "nope", 1000)), "-1");

        // {"email":"me@example.com","https://api.openai.com/auth":
        //  {"chatgpt_account_id":"acct-1"}}, unpadded, URL-safe
        Str  jwt  = "eyJhbGciOiJub25lIn0.eyJlbWFpbCI6Im1lQGV4YW1wbGUuY29tIiwiaHR0cHM6Ly9hcGku"
                    "b3BlbmFpLmNvbS9hdXRoIjp7ImNoYXRncHRfYWNjb3VudF9pZCI6ImFjY3QtMSJ9fQ.sig";
        Json *c   = jwtclaims(a, jwt);
        expect(t, "cg.jwt.email", jsonstr(jsonget(c, "email")), "me@example.com");
        expect(t, "cg.jwt.account", jsonstr(jsonget(jsonget(c, "https://api.openai.com/auth"),
                                                    "chatgpt_account_id")), "acct-1");
        expect(t, "cg.jwt.bad", istr(a, jwtclaims(a, "notajwt")!=0), "0");
        expect(t, "cg.jwt.junk", istr(a, jwtclaims(a, "a.!!!.b")!=0), "0");

        McpAuth x = {};
        x.ep       = 1;
        x.authz    = "https://auth.openai.com/oauth/authorize";
        x.client   = chatgptclient;
        x.redirect = "http://localhost:1455/auth/callback";
        x.state    = "st";
        x.scope    = chatgptscope;
        x.resource = "https://chatgpt.com/backend-api/codex/responses";
        expect(t, "cg.authurl", mcpauthurl(a, &x, "CH"),
               "https://auth.openai.com/oauth/authorize?response_type=code&client_id="
               "app_EMoamEEZ73f0CkXaXp7hrann&redirect_uri=http%3A%2F%2Flocalhost%3A1455"
               "%2Fauth%2Fcallback&code_challenge=CH&code_challenge_method=S256&state=st"
               "&id_token_add_organizations=true&codex_cli_simplified_flow=true"
               "&originator=prospero&scope=openid%20profile%20email%20offline_access");
        expect(t, "cg.listpath", eplistpath(EP_CHATGPT), "models?client_version=1.0.0");
        expect(t, "cg.form", mcpauthform(a, &x, "grant_type=refresh_token"),
               "grant_type=refresh_token&client_id=app_EMoamEEZ73f0CkXaXp7hrann");
    }
    // Copilot: its listing, each model's format, and its URLs
    {
        Arena tmp = *a;
        tmp.beg += (tmp.end - tmp.beg) / 2;
        Str listing = cplisting(a, tmp,
            "{\"data\":[{\"id\":\"claude-x\",\"supported_endpoints\":[\"/chat/completions\","
            "\"/v1/messages\"],\"capabilities\":{\"limits\":{\"max_prompt_tokens\":100,"
            "\"max_output_tokens\":20,\"max_context_window_tokens\":200}}},"
            "{\"id\":\"gpt-r\",\"supported_endpoints\":[\"/responses\",\"/chat/completions\"]},"
            "{\"id\":\"gpt-4o\",\"capabilities\":{\"limits\":{\"max_context_window_tokens\":300}}},"
            "{\"id\":\"off\",\"policy\":{\"state\":\"disabled\"}},{\"name\":\"no id\"}]}");
        expect(t, "cp.listing", listing,
               "{\"object\":\"list\",\"data\":[{\"id\":\"claude-x\",\"object\":\"model\","
               "\"max_input_tokens\":100,\"max_tokens\":20,\"wire\":\"anthropic\"},"
               "{\"id\":\"gpt-r\",\"object\":\"model\",\"wire\":\"responses\"},"
               "{\"id\":\"gpt-4o\",\"object\":\"model\",\"max_input_tokens\":300,"
               "\"wire\":\"openai\"}]}");
        ModelInfo mi = parsemodels(*a, listing, "claude-x", 0);
        expect(t, "cp.listing.info", istr(a, mi.found*1000000 + mi.ctx*1000 + mi.maxout),
               "1100020");
        expect(t, "cp.listing.wire", epkindname(mi.wire), "anthropic");
        expect(t, "cp.listing.other", cplisting(a, tmp, "{\"models\":[]}"), "{\"models\":[]}");

        Str guesses = {};
        Str ids[] = {"gpt-5.6-luna", "gpt-5-mini", "gpt-6", "gpt-4.1", "gpt-41-copilot",
                     "grok-4.7", "mai-code-1", "claude-sonnet-5", "gpt-"};
        for (iz i = 0; i < countof(ids); i++) {
            guesses = concat(a, guesses, concat(a, epkindname(copilotguess(ids[i])), " "));
        }
        expect(t, "cp.guess", guesses, "responses openai responses openai openai "
                                       "responses responses openai openai ");

        expect(t, "cp.url", copiloturl(a, "https://api.githubcopilot.com/", "models"),
               "https://api.githubcopilot.com/models");
        expect(t, "cp.paths", concat(a, concat(a, copilotpath(EP_ANTHROPIC), " "),
                                     concat(a, concat(a, copilotpath(EP_RESPONSES), " "),
                                            copilotpath(EP_OPENAI))),
               "v1/messages responses chat/completions");
        expect(t, "cp.sameurl", istr(a, sameurl("https://h/", "https://h")*10 +
                                        sameurl("https://h/x", "https://h")), "10");

        Endpoint ep = {};
        ep.kind = EP_COPILOT;
        ep.wire = EP_ANTHROPIC;
        ep.url  = epkindurl(EP_COPILOT);
        expect(t, "cp.call", callurl(a, &ep), "https://api.githubcopilot.com/v1/messages");
        ep.api  = "https://api.individual.githubcopilot.com";
        ep.wire = EP_OPENAI;
        expect(t, "cp.call account", callurl(a, &ep),
               "https://api.individual.githubcopilot.com/chat/completions");
        expect(t, "cp.models", epurl(a, &ep, eplistpath(EP_COPILOT)),
               "https://api.individual.githubcopilot.com/models");
        ep.relay = "x";
        ep.url   = "relay+copilot://x";
        ep.wire  = EP_ANTHROPIC;
        expect(t, "cp.relayed", callurl(a, &ep), "relay+copilot://x/v1/messages");
        RelayUrl ru = relayurl("relay+copilot://x");
        expect(t, "cp.relayurl", istr(a, ru.ok*10 + (ru.kind==EP_COPILOT)), "11");
    }
}
