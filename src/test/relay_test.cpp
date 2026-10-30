// Relay tests: relayed endpoints' URLs, an offer as a configuration
// layer, with nothing unsound or secret in it, the layer under the
// user's file only while that is thin, and mailroom's holding back.

static void testrelayurl(Test *t)
{
    Arena a = t->perm;
    struct {
        Str url;
        Str want;  // name, ":a" if anthropic, "-" if not a relay URL
    } cases[] = {
        {"relay://fake",            "fake"},
        {"relay+anthropic://an.1",  "an.1:a"},
        {"relay://",                "-"},
        {"relay://a/b",             "-"},
        {"relay://a b",             "-"},
        {"http://fake",             "-"},
        {"relay+openai://x",        "-"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        RelayUrl u   = relayurl(cases[i].url);
        Str      got = !u.ok ? Str("-") : u.anthropic ? concat(&a, u.name, ":a") : u.name;
        expect(t, concat(&a, "relayurl ", cases[i].url), got, cases[i].want);
    }
    expect(t, "endpointurl", istr(&a, endpointurl("relay://x") && endpointurl("http://h/")
                                      && !endpointurl("x")), "1");
    expect(t, "relaytoken", istr(&a, relaytoken("0123456789abcdef") && !relaytoken("") &&
                                     !relaytoken("ABC") && !relaytoken("a b") &&
                                     !relaytoken("0123456789abcdef0123456789abcdef0")), "1");
}

static void testrelayparse(Test *t)
{
    Arena a = t->perm;
    Str   offer =
        "{\"machine\":\"my box.local\\u0007with a very long name that goes on and on "
        "and on past sixty-three bytes\",\"search\":1,"
        "\"url\":\"http://secret/\",\"key\":\"sk-1\","
        "\"endpoint\":\"fake\",\"model\":\"m1\","
        "\"endpoints\":["
        "{\"name\":\"fake\",\"kind\":\"openai\",\"url\":\"http://secret/\","
        "\"key\":\"sk-2\",\"first_byte_timeout\":\"5\",\"connect_timeout\":\"1\","
        "\"slow_retry_budget\":\"60\",\"models\":[\"m1\",\"m2\\nx\"]},"
        "{\"name\":\"an\",\"kind\":\"anthropic\",\"idle_timeout\":\"9\\n[x]\"},"
        "{\"name\":\"bad name\"},{\"name\":\"fake\"}],"
        "\"models\":["
        "{\"name\":\"m1\",\"settings\":{\"context\":\"1000\",\"like\":\"x\","
        "\"max_concurrent\":\"9\",\"template.enable_thinking\":\"false\","
        "\"bogus\":\"1\",\"reasoning\":\"a\\nb\"}},"
        "{\"name\":\"we[ird]\",\"settings\":{}}]}";
    JsonDoc   d = jsonparse(&a, offer);
    RelayPeer p = {};
    relayparse(&p, &a, d.root);
    expect(t, "relayparse.machine", p.machine,
           "mybox.localwithaverylongnamethatgoesonandonandonpastsixty-three");
    expect(t, "relayparse.machine.len", istr(&a, p.machine.len <= 63), "1");
    expect(t, "relayparse.search", istr(&a, p.search), "1");
    expect(t, "relayparse.own", istr(&a, p.own.len), "0");
    RelayPeer q = {};
    relayparse(&q, &a, jsonparse(&a, "{\"own\":\"9f\"}").root);
    expect(t, "relayparse.own.ok", q.own, "9f");
    relayparse(&q, &a, jsonparse(&a, "{\"own\":\"x\\n[y]\"}").root);
    expect(t, "relayparse.own.bad", istr(&a, q.own.len), "0");
    expect(t, "relayparse.eps", istr(&a, p.eps.len), "2");
    expect(t, "relayparse.models", istr(&a, p.eps[0].models.len), "1");
    expect(t, "relayparse.kind", istr(&a, p.eps[1].anthropic), "1");
    expect(t, "relayparse.layer", p.layer,
           "# relayed from mybox.localwithaverylongnamethatgoesonandonandonpastsixty-three\n"
           "endpoint = fake\n"
           "model = m1\n"
           "[endpoint fake]\n"
           "url = relay://fake\n"
           "kind = openai\n"
           "first_byte_timeout = 5\n"
           "slow_retry_budget = 60\n"
           "[endpoint an]\n"
           "url = relay+anthropic://an\n"
           "kind = anthropic\n"
           "[model m1]\n"
           "context = 1000\n"
           "template.enable_thinking = false\n");

    // Nothing in the offer becomes a diagnostic
    Ini ini = {};
    iniparse(&ini, &a, "relayed", p.layer);
    expect(t, "relayparse.parses", istr(&a, ini.diags.len), "0");
}

static void testrelaylayer(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    Str    layer =
        "endpoint = fake\n"
        "[endpoint fake]\nurl = relay://fake\nkind = openai\nfirst_byte_timeout = 5\n"
        "[model m1]\ncontext = 1000\nreasoning_budget = 7\n";
    CfgLayer layers[] = {{"relayed from box", layer}};

    // A thin file: the layer under it, the file winning
    os_write(v.os, a, "/c/p.ini", "[model m1]\ncontext = 5\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/p.ini", 0);
    expect(t, "layer.thin", istr(&a, c->thin), "1");
    c->layers = {layers, 1};
    cfgload(c, v.scratch);
    expect(t, "layer.url", cfgget(c, "endpoint", "fake", "url", 0), "relay://fake");
    expect(t, "layer.default", cfgdefaults(c).endpoint, "fake");
    expect(t, "layer.filewins", cfgget(c, "model", "m1", "context", 0), "5");
    expect(t, "layer.under", cfgget(c, "model", "m1", "reasoning_budget", 0), "7");
    expect(t, "layer.quiet", inidiags(&a, c->ini), "");

    // An endpoint of the file's own: not thin, and the layer is gone, in
    // the same load
    os_write(v.os, a, "/c/p.ini", "[endpoint mine]\nurl = http://h/\n");
    cfgload(c, v.scratch);
    expect(t, "layer.notthin", istr(&a, c->thin), "0");
    expect(t, "layer.dropped", cfgget(c, "endpoint", "fake", "url", 0), "");
    expect(t, "layer.own", cfgdefaults(c).endpoint, "mine");

    // Thin again: the layer comes back in the same load
    os_write(v.os, a, "/c/p.ini", "# none\n");
    cfgload(c, v.scratch);
    expect(t, "layer.thinagain", istr(&a, c->thin), "1");
    expect(t, "layer.back", cfgget(c, "endpoint", "fake", "url", 0), "relay://fake");

    // A relayed URL in the file is the file's, but no endpoint of its
    // own: still thin
    os_write(v.os, a, "/c/p.ini", "[endpoint x]\nurl = relay://x\n");
    cfgload(c, v.scratch);
    expect(t, "layer.relayedown", istr(&a, c->thin), "1");
    expect(t, "layer.relayedurl", cfgget(c, "endpoint", "x", "url", 0), "relay://x");
    cfgfree(c);
}

static void testmailroom(Test *t)
{
    Arena    a = t->perm;
    Mailbox *m = newmailbox(&a, 1<<12, 0, 0);
    Str      line = Str(alloc<u8>(&a, 1500), 1500);
    expect(t, "mailroom.empty", istr(&a, mailroom(m, 1500)), "1");
    mailpost(m, 1, 0, line, 0);
    expect(t, "mailroom.half", istr(&a, mailroom(m, 1500)), "0");
    expect(t, "mailroom.small", istr(&a, mailroom(m, 100)), "1");
    expect(t, "mailroom.toobig", istr(&a, mailroom(m, 5000)), "0");
    mailtake(m);
    expect(t, "mailroom.big", istr(&a, mailroom(m, 3000)), "1");
}

static void test_relay(Test *t)
{
    testrelayurl(t);
    testrelayparse(t);
    testrelaylayer(t);
    testmailroom(t);
}
