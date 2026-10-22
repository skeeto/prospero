// Engine tests: the pure parts (the turn machine itself runs end to end
// in tests/scenarios.py)

// Roles: a section over a built-in role, a role of the configuration's
// own, and the task tool's list of them.
static void testroles(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    os_write(v.os, a, "/r/h.ini",
             "[role explore]\nmodel = small\n"
             "[role reader]\ndescription = Reads\nprompt = Only read.\n"
             "tools = read, glob\nendpoint = fast\n"
             "[role open]\n");
    Engine *e = alloc<Engine>(&a);
    e->os  = v.os;
    e->cfg = newconfig(v.os, &a, v.scratch, "/r/h.ini", 0);
    e->ini = e->cfg->ini;

    Role ex = findrole(e, "explore");
    expect(t, "explore over the built-in", concat(&a, concat(&a, ex.model, "|"),
           istr(&a, ex.deny==tooldeny("read grep glob"))), "small|1");
    expect(t, "explore keeps its prompt", istr(&a, startswith(ex.prompt, "Explore the code")), "1");
    Role rd = findrole(e, "reader");
    expect(t, "reader", concat(&a, concat(&a, rd.desc, "|"), concat(&a, rd.prompt,
           concat(&a, "|", rd.endpoint))), "Reads|Only read.|fast");
    expect(t, "reader tools", istr(&a, rd.deny==tooldeny("read glob")), "1");
    Role op = findrole(e, "open");
    expect(t, "a section's default tools", istr(&a, op.ok*10 + (op.deny==TOOL_OFFMASK)), "11");
    expect(t, "general", istr(&a, findrole(e, "general").deny), istr(&a, TOOL_OFFMASK));
    expect(t, "no such role", istr(&a, findrole(e, "nope").ok), "0");
    expect(t, "no empty role", istr(&a, findrole(e, "").ok), "0");

    ToolEnv te = {};
    taskroles(e, &a, v.scratch, &te);
    expect(t, "task roles", te.roles,
           "\n- explore: Finds and reads code to answer a question, and reports "
           "what it found; it cannot change files or run commands (tools: read, "
           "grep, glob)\n- general: Does any task, with every tool (every tool)\n"
           "- reader: Reads (tools: read, glob)\n- open: (no description) (every tool)");
    expect(t, "task role enum", te.roleenum,
           "\"enum\":[\"explore\",\"general\",\"reader\",\"open\"]");
}

// The sessions list: newest first, each subagent under its parent, and
// a parked session idle, its lock its own.
static void testlist(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->statedir = "/s";
    struct { Str sid; Str parent; } made[] = {
        {"20260101-000000-aaaa", {}},
        {"20260101-000001-bbbb", "20260101-000000-aaaa"},
        {"20260101-000002-cccc", {}},
        {"20260101-000003-dddd", "20260101-000001-bbbb"},
        {"20260101-000004-eeee", "20260101-000000-aaaa"},
        {"20260101-000005-ffff", "20260101-999999-gone"},
    };
    for (iz i = 0; i < countof(made); i++) {
        Buf b(&a, 256);
        print(&b, "{\"sid\":\"");
        print(&b, made[i].sid);
        print(&b, "\",\"seq\":1,\"ts\":0,\"ev\":\"session\",\"cwd\":\"/w\"");
        if (made[i].parent.len) {
            print(&b, ",\"parent\":\"");
            print(&b, made[i].parent);
            putbyte(&b, '"');
        }
        print(&b, "}\n");
        Str dir = concat(&a, "/s/sessions/", made[i].sid);
        os_mkdirs(v.os, a, dir);
        os_write(v.os, a, concat(&a, dir, "/events.jsonl"), finish(&b));
    }
    Parked k = {};
    k.sid  = "20260101-000002-cccc";
    k.lock = os_lockfile(v.os, a, "/s/sessions/20260101-000002-cccc/lock");
    e->parked = push(&a, e->parked, k);

    cmdlist(e, v.scratch, 1);
    Buf  got(&a, 1024);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        for (Json *x = jsonget(rec.root, "list")->child; x; x = x->next) {
            print(&got, cuthead(jsonstr(jsonget(x, "sid")), 16));
            putbyte(&got, ':');
            print(&got, jsonint(jsonget(x, "depth"), 0));
            putbyte(&got, ':');
            print(&got, jsonstr(jsonget(x, "state")));
            if (jsonint(jsonget(x, "parked"), 0)) {
                print(&got, ":parked");
            }
            if (jsonint(jsonget(x, "locked"), 0)) {
                print(&got, ":locked");
            }
            putbyte(&got, ' ');
        }
    }
    expect(t, "sessions list", finish(&got),
           "ffff:0:closed cccc:0:idle:parked aaaa:0:closed bbbb:1:closed "
           "dddd:2:closed eeee:1:closed ");
}

static void test_engine(Test *t)
{
    testroles(t);
    testlist(t);
    Arena *a = &t->perm;

    Str long60 = "Refactor the parser so that it handles nested quotes and "
                 "escapes correctly everywhere";
    struct { Str in; Str want; } titles[] = {
        {"go", "go"},
        {"  fix   the\tbug \n", "fix the bug"},
        {"line one\nline two\r\nline three", "line one line two line three"},
        {"", ""},
        {" \n\t ", ""},
        {long60, "Refactor the parser so that it handles nested quotes and\xe2\x80\xa6"},
        // A word that ends exactly at the limit, then more
        {"aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff ggg",
         "aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff\xe2\x80\xa6"},
        // Code points, not bytes
        {"\xc3\xa9\xc3\xa9\xc3\xa9", "\xc3\xa9\xc3\xa9\xc3\xa9"},
        {"tab\x7f" "del\xc2\xa0nbsp", "tab del nbsp"},
    };
    for (iz i = 0; i < countof(titles); i++) {
        expect(t, concat(a, "title ", titles[i].in), maketitle(a, titles[i].in),
               titles[i].want);
    }

    // Exactly 60 code points of two-byte characters: no ellipsis
    Buf b(a, 256);
    for (i32 i = 0; i < 60; i++) {
        print(&b, "\xc3\xa9");
    }
    Str sixty = finish(&b);
    expect(t, "title of 60 code points", maketitle(a, sixty), sixty);
    Str more = concat(a, sixty, "\xc3\xa9");
    expect(t, "title of 61 code points",
           maketitle(a, more), concat(a, sixty, "\xe2\x80\xa6"));

    // No space in its second half: cut inside the long word
    Buf in(a, 128);
    print(&in, "x ");
    for (i32 i = 0; i < 70; i++) {
        putbyte(&in, 'a');
    }
    Str word = finish(&in);
    expect(t, "title cut in a word", maketitle(a, word),
           concat(a, takehead(word, 60), "\xe2\x80\xa6"));

    // The id of a command too long to parse, from its start
    struct { Str line; i64 want; } ids[] = {
        {"{\"cmd\":\"prompt\",\"id\":42,\"text\":\"xx", 42},
        {" { \"id\" : -7 , \"cmd\":\"x\"}", -7},
        {"{\"cmd\":\"prompt\",\"sid\":\"a\\\"id\\\":9\",\"id\":3", 3},
        {"{\"text\":{\"id\":5,\"x\":[1,{\"id\":6}]},\"id\":8}", 8},
        {"{\"cmd\":\"prompt\",\"text\":\"xxxxxxxxxxxxxxxx", 0},
        {"{\"cmd\":\"prompt\",\"text\":\"\\\"id\\\":4,", 0},
        {"[\"id\",1]", 0},
        {"{\"id\":\"12\"}", 0},
        {"", 0},
    };
    for (iz i = 0; i < countof(ids); i++) {
        expect(t, concat(a, "cmdlineid ", ids[i].line),
               istr(a, cmdlineid(ids[i].line)), istr(a, ids[i].want));
    }

    // Every command gets its reply: once stopping, those that would start
    // something are refused, and one too long for the mailbox is refused
    // by its id
    {
        Engine *e = alloc<Engine>(a);
        e->os       = newos(a, 1<<16);
        e->out      = newmailbox(a, 1<<16, 0, 0);
        e->in       = newmailbox(a, 1<<12, 0, 0);
        e->quitting = 1;
        Str cmds[] = {
            "{\"cmd\":\"hello\",\"id\":1,\"proto\":0}",
            "{\"cmd\":\"new\",\"id\":2,\"cwd\":\"/w\"}",
            "{\"cmd\":\"open\",\"id\":3,\"sid\":\"x\"}",
            "{\"cmd\":\"prompt\",\"id\":4,\"sid\":\"x\",\"text\":\"t\"}",
            "{\"cmd\":\"steer\",\"id\":5,\"sid\":\"x\",\"text\":\"t\"}",
            "{\"cmd\":\"answer\",\"id\":6,\"sid\":\"x\",\"req\":\"r\"}",
            "{\"cmd\":\"cancel\",\"id\":7,\"sid\":\"x\"}",
        };
        for (iz i = 0; i < countof(cmds); i++) {
            command(e, *a, cmds[i]);
        }
        Buf  big(a, 8192);
        print(&big, "{\"cmd\":\"prompt\",\"id\":8,\"text\":\"");
        for (i32 i = 0; i < 6000; i++) {
            putbyte(&big, 'x');
        }
        print(&big, "\"}");
        expect(t, "enginecmd too long", istr(a, enginecmd(e, finish(&big))), "0");
        Mail in = mailtake(e->in);
        for (Record r; (r = mailnext(&in)).ok;) {
            onrecord(e, *a, r);
        }

        Buf  got(a, 1024);
        Mail m = mailtake(e->out);
        for (Record r; (r = mailnext(&m)).ok;) {
            EvRec rec = evparse(a, r.data);
            if (rec.ev != EV_REPLY) continue;
            print(&got, jsonint(jsonget(rec.root, "id"), 0));
            putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
            print(&got, jsonstr(jsonget(rec.root, "err")));
            putbyte(&got, '\n');
        }
        expect(t, "replies while stopping", finish(&got),
               "1+\n"
               "2-the engine is stopping\n"
               "3-the engine is stopping\n"
               "4-the engine is stopping\n"
               "5-the engine is stopping\n"
               "6-the engine is stopping\n"
               "7-no such open session\n"
               "8-command too long\n");
    }
}
