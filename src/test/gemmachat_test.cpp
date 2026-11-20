// The built-in model's chat (gemmachat.cpp): the template, the output's
// calls and stream, sampling, and the cache of sequences, without the
// model's network.

// The template, as gemma4.c's server renders the same request: thoughts
// only since the last user message, calls and results in the model's
// turn, the turn left open for the reply to the results, and a user's
// "<|turn>" text, not markup.
static void test_gemmarender(Test *t)
{
    Arena a = t->perm;
    Str   req =
        "{\"reasoning_effort\":\"low\",\"messages\":["
        "{\"role\":\"system\",\"content\":\" Be brief. \"},"
        "{\"role\":\"user\",\"content\":\"Read <|turn>a\"},"
        "{\"role\":\"assistant\",\"reasoning_content\":\"old\",\"content\":\"Ok.\","
        "\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":\"read\","
        "\"arguments\":\"{\\\"path\\\":\\\"a\\\",\\\"n\\\":2}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"x\\\"y\"},"
        "{\"role\":\"assistant\",\"content\":\"Done.\"},"
        "{\"role\":\"user\",\"content\":\"Again\"},"
        "{\"role\":\"assistant\",\"reasoning_content\":\"new\",\"content\":\"\","
        "\"tool_calls\":[{\"id\":\"c2\",\"function\":{\"name\":\"read\","
        "\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"c2\",\"content\":\"z\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"read\","
        "\"description\":\"Read\",\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"path\":{\"type\":\"string\"},\"n\":{\"type\":[\"integer\",\"null\"]},"
        "\"m\":{\"type\":\"string\",\"enum\":[\"a\",\"b\"]},"
        "\"l\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"o\":{\"type\":\"object\",\"properties\":{\"k\":{\"type\":\"boolean\","
        "\"nullable\":true}},\"required\":[\"k\"]}},\"required\":[\"path\"]}}}]}";
    Str want =
        "<bos><|turn>system\n<|think|>\nBe brief.<|tool>declaration:read{"
        "description:<|\"|>Read<|\"|>,parameters:{properties:{"
        "l:{items:{type:<|\"|>STRING<|\"|>},type:<|\"|>ARRAY<|\"|>},"
        "m:{enum:[<|\"|>a<|\"|>,<|\"|>b<|\"|>],type:<|\"|>STRING<|\"|>},"
        "n:{type:<|\"|>INTEGER<|\"|>},"
        "o:{properties:{k:{nullable:true,type:<|\"|>BOOLEAN<|\"|>}},"
        "required:[<|\"|>k<|\"|>],type:<|\"|>OBJECT<|\"|>},"
        "path:{type:<|\"|>STRING<|\"|>}},required:[<|\"|>path<|\"|>],"
        "type:<|\"|>OBJECT<|\"|>}}<tool|><turn|>\n"
        "<|turn>user\nRead <|turn>a<turn|>\n"
        "<|turn>model\nOk.<|tool_call>call:read{path:<|\"|>a<|\"|>,n:2}<tool_call|>"
        "<|tool_response>response:read{value:<|\"|>x\"y<|\"|>}<tool_response|>"
        "Done.<turn|>\n<|turn>user\nAgain<turn|>\n"
        "<|turn>model\n<|channel>thought\nnew\n<channel|>"
        "<|tool_call>call:read{path:<|\"|>a<|\"|>}<tool_call|>"
        "<|tool_response>response:read{value:<|\"|>z<|\"|>}<tool_response|>"
        "<|channel>thought\n";
    JsonDoc  d = jsonparse(&a, req);
    GmRender r = gmrender(&a, d.root, gmthinking(d.root), 4096);
    Str      got = Str(r.p.text, r.p.len);
    expect(t, "gemmachat.render", got, want);
    expect(t, "gemmachat.render.flags",
           istr(&a, r.system*100 + r.userlast*10 + r.inthought), "101");

    // Markup where the template wrote it, never in a message's text
    iz user = find(got, "Read <|turn>a");
    iz mine = find(got, "<|turn>user");
    expect(t, "gemmachat.render.mask",
           istr(&a, r.p.mask[mine]*10 + r.p.mask[user+5]), "10");

    // A user's message last, thinking off: the reply opens a model turn
    d = jsonparse(&a, "{\"messages\":[{\"role\":\"user\",\"content\":"
                      "[{\"type\":\"text\",\"text\":\"Hi\"},{\"type\":\"image_url\"},"
                      "{\"type\":\"text\",\"text\":\" there\"}]},"
                      "{\"role\":\"assistant\",\"content\":\"Yo\",\"reasoning_content\":\"x\"},"
                      "{\"role\":\"user\",\"content\":\"Bye\"}]}");
    r = gmrender(&a, d.root, 0, 4096);
    expect(t, "gemmachat.render.plain", Str(r.p.text, r.p.len),
           "<bos><|turn>user\nHi there<turn|>\n<|turn>model\nYo<turn|>\n"
           "<|turn>user\nBye<turn|>\n<|turn>model\n");
    expect(t, "gemmachat.render.plainflags",
           istr(&a, r.system*100 + r.userlast*10 + r.inthought), "10");

    // Thinking as the request says, on if it says nothing
    struct { Str req; Str want; } th[] = {
        {"{}", "1"},
        {"{\"reasoning_effort\":\"none\"}", "0"},
        {"{\"reasoning_effort\":\"high\"}", "1"},
        {"{\"reasoning_effort\":\"high\",\"chat_template_kwargs\":{\"enable_thinking\":false}}", "0"},
        {"{\"chat_template_kwargs\":{\"enable_thinking\":true}}", "1"},
    };
    for (iz i = 0; i < countof(th); i++) {
        expect(t, concat(&a, "gemmachat.thinking ", th[i].req),
               istr(&a, gmthinking(jsonparse(&a, th[i].req).root)), th[i].want);
    }

    // No messages, or no room
    d = jsonparse(&a, "{\"messages\":[]}");
    expect(t, "gemmachat.render.empty", gmrender(&a, d.root, 0, 4096).err,
           "request has no messages");
    d = jsonparse(&a, "{\"messages\":[{\"role\":\"user\",\"content\":\"a long message\"}]}");
    expect(t, "gemmachat.render.full", gmrender(&a, d.root, 0, 16).err, "request too large");
}

static void test_gemmacalls(Test *t)
{
    Arena a = t->perm;
    struct {
        Str text;
        Str want;  // name, a space, its arguments, or "-" if not a call
    } cases[] = {
        {"call:read{path:<|\"|>a b<|\"|>,n:10,f:true,x:null,o:{k:[1,<|\"|>s<|\"|>]}}",
         "read {\"path\":\"a b\",\"n\":10,\"f\":true,\"x\":null,\"o\":{\"k\":[1,\"s\"]}}"},
        {" call:x{ a : hello world , b:2.5e3 }", "x {\"a\":\"hello world\",\"b\":2.5e3}"},
        {"call:x{a:+1,b:.5,c:0x10,d:}", "x {\"a\":\"+1\",\"b\":\".5\",\"c\":\"0x10\",\"d\":\"\"}"},
        {"call:x{<|\"|>k k<|\"|>:1}", "x {\"k k\":1}"},
        {"call:x{s:<|\"|>a\"b\nc\\<|\"|>}", "x {\"s\":\"a\\\"b\\nc\\\\\"}"},
        {"call:x{}", "x {}"},
        {"call:x{a:[]}", "x {\"a\":[]}"},
        {"call:x{a:<|\"|>open}", "-"},
        {"call:{}", "-"},
        {"call:x{}y", "-"},
        {"call:x{a:1", "-"},
        {"call:x{a 1}", "-"},
        {"call:x", "-"},
        {"read{}", "-"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        GmCall c   = gmparsecall(&a, cases[i].text);
        Str    got = c.ok ? concat(&a, concat(&a, c.name, " "), c.args) : Str("-");
        expect(t, concat(&a, "gemmachat.call ", cases[i].text), got, cases[i].want);
    }

    // Too deep for the reader: not a call, and no crash
    Buf deep(&a, 512);
    print(&deep, "call:x{a:");
    for (i32 i = 0; i < 100; i++) putbyte(&deep, '[');
    for (i32 i = 0; i < 100; i++) putbyte(&deep, ']');
    putbyte(&deep, '}');
    expect(t, "gemmachat.call.deep", istr(&a, gmparsecall(&a, finish(&deep)).ok), "0");
}

// A tokenizer of control tokens at their ids in the real file, and a
// few pieces, for the stream.
enum { GMT_HELLO = 600, GMT_WORLD, GMT_THOUGHT, GMT_NL, GMT_THINK, GMT_HEAD,
       GMT_A, GMT_CLOSE, GMT_READ };

static GmTok gmtchattok(Arena *a)
{
    static struct { Str text; i32 id; } const sp[] = {
        {"<|turn>", 105}, {"<turn|>", 106}, {"<|tool_call>", 48},
        {"<tool_call|>", 49}, {"<|tool_response>", 50}, {"<|\"|>", 52},
        {"<|channel>", 100}, {"<channel|>", 101},
    };
    GmTok tok = {};
    tok.text      = alloc<Str>(a, GM_VOCAB);
    tok.specials  = alloc<GmSpecial>(a, countof(sp));
    tok.nspecials = countof(sp);
    for (i32 i = 0; i < countof(sp); i++) {
        tok.specials[i].text = sp[i].text;
        tok.specials[i].id   = sp[i].id;
    }
    for (i32 b = 0; b < 256; b++) {
        u8 *c = alloc<u8>(a, 1);
        *c = (u8)b;
        tok.text[238+b] = {c, 1};
    }
    tok.text[GMT_HELLO]   = "Hello";
    tok.text[GMT_WORLD]   = " w\xc3\xb6rld";
    tok.text[GMT_THOUGHT] = "thought";
    tok.text[GMT_NL]      = "\n";
    tok.text[GMT_THINK]   = "Hmm";
    tok.text[GMT_HEAD]    = "call:re";
    tok.text[GMT_READ]    = "ad{path:";
    tok.text[GMT_A]       = "a";
    tok.text[GMT_CLOSE]   = "}";
    return tok;
}

static void gmtcollect(void *ctx, Str sse)
{
    print((Buf *)ctx, sse);
}

// Each chunk's delta and finish as delta keys and values, one a line.
static Str gmtdeltas(Arena *a, Str sse)
{
    Buf b(a, 1024);
    for (Str rest = sse; rest.len;) {
        Cut c = cut(rest, '\n');
        rest  = c.tail;
        if (!startswith(c.head, "data: {")) {
            continue;
        }
        Json *j  = jsonparse(a, cuthead(c.head, 6)).root;
        Json *ch = jsonat(jsonget(j, "choices"), 0);
        for (Json *m = jsonget(ch, "delta")->child; m; m = m->next) {
            print(&b, m->key);
            print(&b, "=");
            if (m->key == "tool_calls") {
                Json *fn = jsonget(jsonat(m, 0), "function");
                print(&b, jsonstr(jsonget(fn, "name")));
                print(&b, " ");
                print(&b, jsonstr(jsonget(fn, "arguments")));
            } else {
                print(&b, jsonstr(m));
            }
            print(&b, "\n");
        }
        Str fr = jsonstr(jsonget(ch, "finish_reason"));
        if (fr.len) {
            print(&b, "finish=");
            print(&b, fr);
            print(&b, "\n");
        }
    }
    return finish(&b);
}

// What streams of a call's arguments as each byte comes: always a prefix
// of the whole's JSON, which it reaches at the end.
static void test_gemmacallprefix(Test *t)
{
    Arena a = t->perm;
    struct { Str call; Str json; } cases[] = {
        {"{path:<|\"|>a b<|\"|>,n:12,ok:true,v:[1,x,{k:2}],s:x y}",
         "{\"path\":\"a b\",\"n\":12,\"ok\":true,\"v\":[1,\"x\",{\"k\":2}],\"s\":\"x y\"}"},
        {"{text:<|\"|>\xc3\xa9\"\\\n<|\"|>}", "{\"text\":\"\xc3\xa9\\\"\\\\\\n\"}"},
        {"{}", "{}"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Str  call   = cases[i].call;
        b32  prefix = 1;
        Str  last   = {};
        for (iz n = 0; n <= call.len; n++) {
            Arena t2 = a;
            Str   c  = takehead(call, n);
            Buf   b(&t2, 256);
            gmcallprefix(&c, &b, 0);
            last    = clone(&a, finish(&b));
            prefix &= startswith(cases[i].json, last);
        }
        expect(t, "gemmachat.callprefix.prefix", istr(&a, prefix), "1");
        expect(t, "gemmachat.callprefix.whole", last, cases[i].json);
        GmCall g = gmparsecall(&a, concat(&a, "call:f", call));
        expect(t, "gemmachat.callprefix.parse", g.args, cases[i].json);
    }
}

static void test_gemmastream(Test *t)
{
    Arena a   = t->perm;
    GmTok tok = gmtchattok(&a);
    GmIds ids = gmids(&tok);
    expect(t, "gemmachat.ids", istr(&a, ids.ok), "1");
    Json *req = jsonparse(&a, "{\"tools\":[{\"type\":\"function\",\"function\":"
                              "{\"name\":\"read\"}}]}").root;

    // A thought, text with a character split over byte tokens, a call,
    // and the end
    Buf      out(&a, 1<<14);
    GmStream s = gmstream(&a, &tok, ids, req, 0, gmtcollect, &out, "call_x");
    i32 seq[] = {
        100, GMT_THOUGHT, GMT_NL, GMT_THINK, 101,
        GMT_HELLO, 238+0xc3, 238+0xa9, GMT_WORLD,
        48, GMT_HEAD, GMT_READ, 52, GMT_A, 52, GMT_CLOSE, 49,
    };
    for (iz i = 0; i < countof(seq); i++) {
        expect(t, "gemmachat.stream.goes", istr(&a, gmstreamtoken(&s, a, seq[i])), "0");
    }
    expect(t, "gemmachat.stream.stops", istr(&a, gmstreamtoken(&s, a, 50)), "1");
    gmflushtext(&s, a);
    gmfinish(&s, a, "tool_calls", 10, 4, 17);
    expect(t, "gemmachat.stream", gmtdeltas(&a, finish(&out)),
           "reasoning_content=Hmm\n"
           "content=Hello\n"
           "content=\xc3\xa9\n"
           "content= w\xc3\xb6rld\n"
           "tool_calls=read \n"
           "tool_calls= {\"path\":\n"
           "tool_calls= \"\n"
           "tool_calls= a\n"
           "tool_calls= \"\n"
           "tool_calls= }\n"
           "finish=tool_calls\n");
    Str sse = finish(&out);
    expect(t, "gemmachat.stream.callid",
           istr(&a, find(sse, "\"id\":\"call_x_0\"") >= 0), "1");
    expect(t, "gemmachat.stream.usage",
           istr(&a, find(sse, "\"cached_tokens\":4") >= 0 &&
                    find(sse, "\"cache_n\":4") >= 0 &&
                    endswith(sse, "data: [DONE]\n\n")), "1");

    // A thought the prompt opened, and a call that is no call
    out = Buf(&a, 1<<12);
    s   = gmstream(&a, &tok, ids, req, 0, gmtcollect, &out, "c");
    s.thought = 1;
    i32 seq2[] = {GMT_THINK, 101, 48, GMT_A, 49, GMT_HELLO};
    for (iz i = 0; i < countof(seq2); i++) {
        gmstreamtoken(&s, a, seq2[i]);
    }
    expect(t, "gemmachat.stream.notcall", gmtdeltas(&a, finish(&out)),
           "reasoning_content=Hmm\n"
           "content=<|tool_call>\n"
           "content=a\n"
           "content=<tool_call|>\n"
           "content=Hello\n");

    // Constrained: no call with no tools, and in a call's header, only
    // tokens toward a tool's name, and no end
    f32 *logits = alloc<f32>(&a, GM_VOCAB);
    s = gmstream(&a, &tok, ids, jsonparse(&a, "{}").root, 0, gmtcollect, &out, "c");
    gmconstrain(&s, a, logits);
    expect(t, "gemmachat.constrain.nocall", istr(&a, logits[48] < -1e30f), "1");
    s = gmstream(&a, &tok, ids, req, 0, gmtcollect, &out, "c");
    for (i32 i = 0; i < GM_VOCAB; i++) logits[i] = 0;
    gmstreamtoken(&s, a, 48);
    gmstreamtoken(&s, a, GMT_HEAD);
    gmconstrain(&s, a, logits);
    expect(t, "gemmachat.constrain.header",
           istr(&a, logits[GMT_READ]==0 && logits[GMT_A]==0 && logits[GMT_WORLD]<-1e30f &&
                    logits[GM_EOS]<-1e30f && logits[106]<-1e30f), "1");

    // With a schema: past the name, a required key's string still open
    // or its arguments unclosed, no end; once closed, only the end
    Json *schema = jsonparse(&a, "{\"tools\":[{\"type\":\"function\",\"function\":"
                                 "{\"name\":\"read\",\"parameters\":{\"properties\":"
                                 "{\"path\":{}},\"required\":[\"path\"]}}}]}").root;
    s = gmstream(&a, &tok, ids, schema, 0, gmtcollect, &out, "c");
    i32 open[] = {48, GMT_HEAD, GMT_READ, 52, GMT_A, 52};
    for (iz i = 0; i < countof(open); i++) {
        gmstreamtoken(&s, a, open[i]);
    }
    for (i32 i = 0; i < GM_VOCAB; i++) logits[i] = 0;
    gmconstrain(&s, a, logits);
    expect(t, "gemmachat.constrain.open",
           istr(&a, logits[49]<-1e30f && logits[GMT_CLOSE]==0 && logits[GMT_HELLO]<-1e30f &&
                    logits[48]<-1e30f && logits[100]<-1e30f), "1");
    gmstreamtoken(&s, a, GMT_CLOSE);
    for (i32 i = 0; i < GM_VOCAB; i++) logits[i] = 0;
    gmconstrain(&s, a, logits);
    expect(t, "gemmachat.constrain.closed",
           istr(&a, logits[49]==0 && logits[GMT_CLOSE]<-1e30f && logits[GMT_A]<-1e30f), "1");
}

// A call's arguments held to its tool's schema, as llama.cpp's grammar
// would: edit wants path, old_string, new_string; ask_user's questions
// each a question.
static void test_gemmaargs(Test *t)
{
    Arena a      = t->perm;
    Json *params = jsonparse(&a,
        "{\"properties\":{\"path\":{},\"old_string\":{},\"new_string\":{},"
        "\"q\":{\"type\":\"array\",\"items\":{\"properties\":"
        "{\"question\":{},\"header\":{}},\"required\":[\"question\"]}}},"
        "\"required\":[\"path\"]}").root;
    GmArgs s = gmargs(&a, params, "path:<|\"|>a,b<|\"|>,old");
    expect(t, "gemmachat.args.key",
           istr(&a, s.top && s.top->seen.len==1 && s.top->seen[0]=="path" &&
                    s.top->atkey && s.key=="old" && !s.instr), "1");
    s = gmargs(&a, params, "content:<|\"|>x{y,z:");
    expect(t, "gemmachat.args.instr", istr(&a, s.instr && !s.top->atkey), "1");
    s = gmargs(&a, params, "q:[{a:1,b:2}],");
    expect(t, "gemmachat.args.after",
           istr(&a, s.top->schema==params && s.top->seen.len==1 && s.top->atkey &&
                    !s.key.len), "1");
    s = gmargs(&a, params, "q:[{header:<|\"|>x<|\"|>,qu");
    expect(t, "gemmachat.args.nested",
           istr(&a, s.top->schema==jsonget(jsonget(jsonget(params, "properties"), "q"), "items") &&
                    s.top->seen.len==1 && s.top->seen[0]=="header" && s.key=="qu"), "1");
    s = gmargs(&a, params, "n:1}");
    expect(t, "gemmachat.args.closed", istr(&a, !s.top && s.closed), "1");

    // Where a value begins, and of what type
    Str fresh[][2] = {
        {"path:", "1"}, {"path: ", "1"}, {"path:<|\"|>", "0"}, {"q:[", "1"},
        {"q:[{question:<|\"|>a<|\"|>},", "1"}, {"q:[{", "0"}, {"path:1", "0"},
    };
    for (iz i = 0; i < countof(fresh); i++) {
        s = gmargs(&a, params, fresh[i][0]);
        expect(t, concat(&a, "gemmachat.args.fresh ", fresh[i][0]), istr(&a, s.fresh), fresh[i][1]);
    }
    struct { Str type; b32 quote; Str text; Str want; } begins[] = {
        {"string", 1, "", "1"}, {"string", 0, "a", "0"}, {"object", 0, "{", "1"},
        {"object", 0, "<", "0"}, {"array", 0, "[{", "1"}, {"array", 1, "", "0"},
        {"boolean", 0, "tr", "1"}, {"boolean", 0, "false", "1"}, {"boolean", 0, "true}", "1"},
        {"boolean", 0, "yes", "0"}, {"integer", 0, "-3", "1"}, {"number", 0, "x", "0"},
        {"", 0, "x", "1"},
    };
    for (iz i = 0; i < countof(begins); i++) {
        Buf b(&a, 64);
        print(&b, "gemmachat.args.begins.");
        print(&b, (i64)i);
        expect(t, finish(&b), istr(&a, gmvaluebegins(begins[i].type, begins[i].quote, begins[i].text)),
               begins[i].want);
    }

    Str  left[] = {"old_string", "new_string", "replace_all"};
    Slice<Str> unused = {left, countof(left)};
    struct { Str args; Str text; b32 done; Str want; } cases[] = {
        {"path:<|\"|>a<|\"|>,", "new",      0, "1"},
        {"path:<|\"|>a<|\"|>,", "content",  0, "0"},
        {"path:<|\"|>a<|\"|>,", "}",        0, "0"},
        {"path:<|\"|>a<|\"|>,", "}",        1, "1"},
        {"path:<|\"|>a<|\"|>,", "",         0, "0"},  // a special
        {"path:<|\"|>a<|\"|>,old_", "string:", 0, "1"},
        {"path:<|\"|>a<|\"|>,old_", "str:",    0, "0"},
        {"path:<|\"|>a<|\"|>", "}",         0, "0"},
        {"path:<|\"|>a<|\"|>", "}",         1, "1"},
        {"path:<|\"|>a<|\"|>", ",new",      0, "1"},
        {"path:<|\"|>a<|\"|>", ",content",  0, "0"},
        {"path:",               "",           0, "1"},  // the delimiter
        {"path:<|\"|>a<|\"|>", "",          0, "0"},
        {"path:<|\"|>a<|\"|>", "Hello",     1, "0"},
        {"n:",                  "12",         0, "1"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        GmArgs st = gmargs(&a, params, cases[i].args);
        Buf b(&a, 64);
        print(&b, "gemmachat.args.allow.");
        print(&b, (i64)i);
        expect(t, finish(&b), istr(&a, gmargsallow(&st, unused, cases[i].done, cases[i].text)),
               cases[i].want);
    }
}

static void test_gemmasample(Test *t)
{
    Arena a = t->perm;
    f32   v[8] = {0, 3, 1, 3.5f, 2, -1, 0, 0};
    GmSampler sp = {};
    expect(t, "gemmachat.sample.greedy", istr(&a, gmsample(&sp, v, 8)), "3");
    sp.temp = 1;
    sp.topk = 1;
    sp.topp = 1;
    sp.rng  = 7;
    expect(t, "gemmachat.sample.topk1", istr(&a, gmsample(&sp, v, 8)), "3");

    // Two equally likely, the rest out: each about half the time, and the
    // same again from the same seed
    f32 no   = -__builtin_inff();
    f32 w[4] = {no, 1, no, 1};
    sp.topk = 0;
    sp.rng  = 1;
    i32 ones = 0;
    Buf first(&a, 128), again(&a, 128);
    for (i32 i = 0; i < 1000; i++) {
        i32 k = gmsample(&sp, w, 4);
        ones += k == 1;
        if (i < 32) print(&first, (i64)k);
    }
    sp.rng = 1;
    for (i32 i = 0; i < 32; i++) {
        print(&again, (i64)gmsample(&sp, w, 4));
    }
    expect(t, "gemmachat.sample.even", istr(&a, ones>400 && ones<600), "1");
    expect(t, "gemmachat.sample.seed", finish(&again), finish(&first));

    // top_p keeps only the likeliest when it alone is enough
    f32 x[3] = {10, 0, 0};
    sp.topp = 0.5f;
    b32 only = 1;
    for (i32 i = 0; i < 100; i++) {
        only &= gmsample(&sp, x, 3) == 0;
    }
    expect(t, "gemmachat.sample.topp", istr(&a, only), "1");

    // Unequal logits sample as their softmax: 5, 3, 0 gives the first
    // 0.876 of the time, the second 0.118
    f32 y[3] = {5, 3, 0};
    sp.topp = 1;
    sp.rng  = 3;
    i32 hits[3] = {};
    for (i32 i = 0; i < 4000; i++) {
        hits[gmsample(&sp, y, 3)]++;
    }
    expect(t, "gemmachat.sample.softmax",
           istr(&a, hits[0]>3380 && hits[0]<3620 && hits[1]>360 && hits[1]<580), "1");

    // min_p drops what is under that share of the likeliest: e^-3 < 0.1
    f32 z[3] = {5, 2, 2};
    sp.minp = 0.1f;
    only = 1;
    for (i32 i = 0; i < 200; i++) {
        only &= gmsample(&sp, z, 3) == 0;
    }
    expect(t, "gemmachat.sample.minp", istr(&a, only), "1");
}

// A sequence as though the model had run tokens 0 to n-1, each token its
// position plus base.
static void gmtfill(GmSeq *s, i32 n, i32 base)
{
    for (i32 i = 0; i < n; i++) {
        s->kv.tokens[i] = i + base;
    }
    gmgrowseq(s, n);  // committed, as a forward's are: a fork reads them
    s->n      = n;
    s->ringlo = n>GM_RING ? n-GM_RING : 0;
}

static void test_gemmacache(Test *t)
{
    Arena   a = t->perm;
    Os     *os = newos(&a, 1<<12);
    GmCache c  = {};
    c.os     = os;
    c.perm   = &a;
    c.cap    = 4096;
    c.budget = (i64)1<<40;
    i32 *p = alloc<i32>(&a, 4096);
    for (i32 i = 0; i < 4096; i++) p[i] = i;

    // A new session starts empty
    GmPlan pl = gmplan(&c, "a", p, 100);
    expect(t, "gemmachat.cache.new", istr(&a, !pl.from && !pl.start), "1");
    GmSeq *sa = gmtake(&c, "a", pl);
    expect(t, "gemmachat.cache.take", istr(&a, sa && sa->n==0), "1");
    if (!sa) {
        return;
    }

    // A checkpoint at 500, then 3000 positions: the rings hold the last
    // 1024
    gmtfill(sa, 500, 0);
    sa->kv.ring[0] = 42;
    gmcheckpoint(&c, sa, 0);
    sa->kv.ring[0] = 0;
    gmtfill(sa, 3000, 0);

    // The same prompt and more: all of it, in its own rings
    pl = gmplan(&c, "a", p, 3010);
    expect(t, "gemmachat.cache.extend",
           istr(&a, pl.from==sa && pl.start==3000 && pl.check<0), "1");

    // Changed at 2600: still within the rings' window
    p[2600] = -1;
    pl = gmplan(&c, "a", p, 3010);
    expect(t, "gemmachat.cache.window", istr(&a, pl.start==2600 && pl.check<0), "1");
    p[2600] = 2600;

    // Changed at 1000: back to the checkpoint
    p[1000] = -1;
    pl = gmplan(&c, "a", p, 3010);
    expect(t, "gemmachat.cache.checkpoint", istr(&a, pl.start==500 && pl.check>=0), "1");

    // Another session sharing 1000 tokens takes a's prefix to the
    // checkpoint, rings and all
    pl = gmplan(&c, "b", p, 2000);
    expect(t, "gemmachat.cache.fork", istr(&a, pl.from==sa && pl.start==500), "1");
    GmSeq *sb = gmtake(&c, "b", pl);
    expect(t, "gemmachat.cache.forked",
           istr(&a, sb && sb!=sa && sb->n==500 && sb->kv.tokens[499]==499 &&
                    sb->kv.ring[0]==42 && c.seqs==sb), "1");
    p[1000] = 1000;

    // Going back before a checkpoint drops it
    pl = gmplan(&c, "a", p, 3010);
    pl.start = 400;
    pl.check = -1;
    gmtake(&c, "a", pl);
    b32 any = 0;
    for (i32 i = 0; i < GM_CHECKS; i++) {
        any |= sa->checks[i].region != 0;
    }
    expect(t, "gemmachat.cache.dropped", istr(&a, sa->n==400 && !any), "1");

    // At most GM_SEQS, the least recently used going
    for (i32 i = 0; i < GM_SEQS; i++) {
        Str sid = concat(&a, "s", istr(&a, i));
        gmtake(&c, sid, gmplan(&c, sid, p, 10));
    }
    i32 n = 0;
    for (GmSeq *s = c.seqs; s; s = s->next) n++;
    expect(t, "gemmachat.cache.count",
           istr(&a, n==GM_SEQS && !gmfindseq(&c, "b") && !gmfindseq(&c, "a")), "1");

    // A budget for one: each new one evicts the last
    c.budget = gmseqbytes(c.seqs);
    gmtake(&c, "z", gmplan(&c, "z", p, 10));
    n = 0;
    for (GmSeq *s = c.seqs; s; s = s->next) n++;
    expect(t, "gemmachat.cache.budget", istr(&a, n==1 && gmfindseq(&c, "z")), "1");
    while (c.seqs) {
        gmevict(&c, c.seqs);
    }
}
