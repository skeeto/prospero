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
           istr(&a, ex.deny==tooldeny("read grep glob skill"))), "small|1");
    expect(t, "explore keeps its prompt", istr(&a, startswith(ex.prompt, "Explore the code")), "1");
    Role rd = findrole(e, "reader");
    expect(t, "reader", concat(&a, concat(&a, rd.desc, "|"), concat(&a, rd.prompt,
           concat(&a, "|", rd.endpoint))), "Reads|Only read.|fast");
    expect(t, "reader tools", istr(&a, rd.deny==tooldeny("read glob")), "1");
    Role op = findrole(e, "open");
    expect(t, "a section's default tools", istr(&a, op.ok*10 + (op.deny==(TOOL_OFFMASK|TOOL_PLANMASK))), "11");
    expect(t, "general", istr(&a, findrole(e, "general").deny), istr(&a, TOOL_OFFMASK|TOOL_PLANMASK));
    expect(t, "no such role", istr(&a, findrole(e, "nope").ok), "0");
    expect(t, "no empty role", istr(&a, findrole(e, "").ok), "0");

    ToolEnv te = {};
    taskroles(e, &a, v.scratch, &te);
    expect(t, "task roles", te.roles,
           "\n- explore: Finds and reads code to answer a question, and reports "
           "what it found; it cannot change files or run commands (tools: read, "
           "grep, glob, skill)\n- general: Does any task, with every tool (every tool)\n"
           "- reader: Reads (tools: read, glob)\n- open: (no description) (every tool)");
    expect(t, "task role enum", te.roleenum,
           "\"enum\":[\"explore\",\"general\",\"reader\",\"open\"]");
}

// Sessions in a state directory of the test platform, each a session
// record with the parent given, if any.
struct MadeSession {
    Str sid;
    Str parent;
};

static void makesessions(Os *os, Arena *a, Slice<MadeSession> made)
{
    for (iz i = 0; i < made.len; i++) {
        Buf b(a, 256);
        print(&b, "{\"sid\":\"");
        print(&b, made[i].sid);
        print(&b, "\",\"seq\":1,\"ts\":0,\"ev\":\"session\",\"cwd\":\"/w\","
                  "\"model\":\"m\"");
        if (made[i].parent.len) {
            print(&b, ",\"parent\":\"");
            print(&b, made[i].parent);
            putbyte(&b, '"');
        }
        print(&b, "}\n");
        Str dir = concat(a, "/s/sessions/", made[i].sid);
        os_mkdirs(os, *a, dir);
        os_write(os, *a, concat(a, dir, "/events.jsonl"), finish(&b));
    }
}

// The sessions list: newest first, each subagent under its parent, a
// parked session idle, its lock its own, one another process has open
// locked, the titles from their sidecars, and a cycle of parents still
// listed. It rests on scansessions, which delete walks too.
static void testlist(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->statedir = "/s";
    MadeSession made[] = {
        {"20260101-000000-aaaa", {}},
        {"20260101-000001-bbbb", "20260101-000000-aaaa"},
        {"20260101-000002-cccc", {}},
        {"20260101-000003-dddd", "20260101-000001-bbbb"},
        {"20260101-000004-eeee", "20260101-000000-aaaa"},
        {"20260101-000005-ffff", "20260101-999999-gone"},
        {"20260101-000006-gggg", "20260101-000007-hhhh"},
        {"20260101-000007-hhhh", "20260101-000006-gggg"},
        {"20260101-000008-iiii", "20260101-000008-iiii"},
    };
    makesessions(v.os, &a, {made, countof(made)});
    os_write(v.os, a, "/s/sessions/20260101-000000-aaaa/title", "Root \"one\"");
    os_write(v.os, a, "/s/sessions/20260101-000003-dddd/title", "A grandchild");
    os_mkdirs(v.os, a, "/s/sessions/not-a-session");
    os_write(v.os, a, "/s/sessions/stray", "a file");
    Parked k = {};
    k.sid  = "20260101-000002-cccc";
    k.lock = os_lockfile(v.os, a, "/s/sessions/20260101-000002-cccc/lock");
    e->parked = push(&a, e->parked, k);
    os_lockfile(v.os, a, "/s/sessions/20260101-000004-eeee/lock");  // elsewhere

    {
        Arena    tmp = v.scratch;
        SessScan sc  = scansessions(e, &tmp);
        Buf      got(&tmp, 1024);
        for (iz i = 0; i < sc.list.len; i++) {
            ListEntry *en = &sc.list[i];
            print(&got, cuthead(en->sid, 16));
            if (en->parent.len) {
                putbyte(&got, '^');
                print(&got, cuthead(en->parent, 16));
            }
            for (iz n = 0; n < en->kids.len; n++) {
                putbyte(&got, n ? ',' : '>');
                print(&got, cuthead(sc.list[en->kids[n]].sid, 16));
            }
            iz *at = upsert(&sc.bysid, en->sid, (Arena *)0);
            putbyte(&got, at && *at==i ? ' ' : '!');
        }
        expect(t, "scansessions", finish(&got),
               "aaaa>bbbb,eeee bbbb^aaaa>dddd cccc dddd^bbbb eeee^aaaa ffff "
               "gggg^hhhh>hhhh hhhh^gggg>gggg iiii ");

        // What delete takes: a tree, each once, a cycle of parents too
        Str roots[] = {"20260101-000000-aaaa", "20260101-000001-bbbb",
                       "20260101-000006-gggg", "20260101-000008-iiii"};
        Buf trees(&tmp, 256);
        for (iz r = 0; r < countof(roots); r++) {
            Slice<iz> tree = subtree(&tmp, &sc, *upsert(&sc.bysid, roots[r], (Arena *)0));
            for (iz i = 0; i < tree.len; i++) {
                print(&trees, cuthead(sc.list[tree[i]].sid, 16));
                putbyte(&trees, i+1<tree.len ? ',' : ' ');
            }
        }
        expect(t, "subtree", finish(&trees),
               "aaaa,bbbb,eeee,dddd bbbb,dddd gggg,hhhh iiii ");
    }

    cmdlist(e, v.scratch, 1);
    Buf  got(&a, 1024);
    Buf  raw(&a, 1024);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        Json *x   = jsonget(rec.root, "list")->child;
        if (x) {
            print(&raw, r.data);
        }
        for (; x; x = x->next) {
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
           "iiii:0:closed ffff:0:closed cccc:0:idle:parked aaaa:0:closed "
           "bbbb:1:closed dddd:2:closed eeee:1:closed:locked "
           "hhhh:0:closed gggg:1:closed ");
    // The record itself, as it was before scansessions: keys in order,
    // parent and depth only for a subagent under its parent
    Str     line = finish(&raw);
    JsonDoc d    = jsonparse(&a, line);
    Json   *list = jsonget(d.root, "list");
    Buf     want(&a, 1024);
    print(&want, "{\"sid\":\"20260101-000000-aaaa\",\"title\":\"Root \\\"one\\\"\","
                 "\"cwd\":\"/w\",\"model\":\"m\",\"state\":\"closed\",\"mtime\":0,"
                 "\"locked\":0,\"jobs\":0}");
    Buf entry(&a, 1024);
    JsonOut w = {&entry};
    putjson(&w, jsonat(list, 3));
    expect(t, "list entry", finish(&entry), finish(&want));
    entry.len = 0;
    w = {&entry};
    putjson(&w, jsonat(list, 5));
    expect(t, "list subagent entry", finish(&entry),
           "{\"sid\":\"20260101-000003-dddd\",\"title\":\"A grandchild\",\"cwd\":\"/w\","
           "\"model\":\"m\",\"state\":\"closed\",\"mtime\":0,\"locked\":0,\"jobs\":0,"
           "\"parent\":\"20260101-000001-bbbb\",\"depth\":2}");
    entry.len = 0;
    w = {&entry};
    putjson(&w, jsonat(list, 2));
    expect(t, "list parked entry", finish(&entry),
           "{\"sid\":\"20260101-000002-cccc\",\"title\":\"\",\"cwd\":\"/w\","
           "\"model\":\"m\",\"state\":\"idle\",\"mtime\":0,\"locked\":0,\"jobs\":0,"
           "\"parked\":1}");
    expect(t, "list record", istr(&a, startswith(line, "{\"ts\":0,\"ev\":\"sessions\","
           "\"list\":[{\"sid\":\"20260101-000008-iiii\"")), "1");
}

// delete: a tree, checked whole first, refused while one of it is open in
// another process, nothing moved then; else children first, each into
// the trash, which a thread would empty (none starts in a test).
static void testdelete(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->statedir = "/s";
    MadeSession made[] = {
        {"20260101-000000-aaaa", {}},
        {"20260101-000001-bbbb", "20260101-000000-aaaa"},
        {"20260101-000002-cccc", "20260101-000001-bbbb"},
        {"20260101-000003-dddd", {}},
    };
    makesessions(v.os, &a, {made, countof(made)});
    iz held = os_lockfile(v.os, a, "/s/sessions/20260101-000002-cccc/lock");
    command(e, v.scratch, "{\"cmd\":\"delete\",\"id\":1,\"sid\":\"20260101-000000-aaaa\"}");
    os_unlockfile(v.os, held);
    command(e, v.scratch, "{\"cmd\":\"delete\",\"id\":2,\"sid\":\"20260101-000000-aaaa\"}");
    command(e, v.scratch, "{\"cmd\":\"delete\",\"id\":3,\"sid\":\"20260101-000000-aaaa\"}");

    Buf  got(&a, 1024);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        print(&got, jsonint(jsonget(rec.root, "id"), 0));
        putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
        print(&got, jsonstr(jsonget(rec.root, "err")));
        for (Json *k = jsonget(rec.root, "deleted")->child; k; k = k->next) {
            putbyte(&got, ' ');
            print(&got, cuthead(jsonstr(k), 16));
        }
        putbyte(&got, '\n');
    }
    expect(t, "delete", finish(&got),
           "1-open in another process: its subagent 20260101-000002-cccc\n"
           "2+ cccc bbbb aaaa\n"
           "3-no such session\n");
    Buf left(&a, 256);
    for (iz i = 0; i < countof(made); i++) {
        Str dir = concat(&a, "/s/sessions/", made[i].sid);
        print(&left, os_stat(v.os, a, dir).type==STAT_DIR ? cuthead(made[i].sid, 16) : Str("-"));
    }
    i32 trashed = 0;
    for (Dirent *d = os_list(v.os, &a, "/s/trash"); d; d = d->next) {
        trashed += startswith(d->name, "20260101-00000");
    }
    print(&left, " ");
    print(&left, (i64)trashed);
    expect(t, "delete left", finish(&left), "---dddd 3");
}

// Sidecars are written by the holder of the session's lock alone: this
// process with the session in memory or parked, else whoever can take
// the lock for a moment, but never while another process has it; and
// borrow, as far as it goes without a session to load.
static void testsidecars(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->statedir = "/s";
    MadeSession made[] = {
        {"20260101-000000-aaaa", {}},  // closed
        {"20260101-000001-bbbb", {}},  // open in another process
        {"20260101-000002-cccc", {}},  // parked
        {"20260101-000003-dddd", {}},  // open here
        {"20260101-000004-eeee", {}},  // open here, read-only
    };
    makesessions(v.os, &a, {made, countof(made)});
    Str lockb = "/s/sessions/20260101-000001-bbbb/lock";
    Str lockc = "/s/sessions/20260101-000002-cccc/lock";
    Str lockd = "/s/sessions/20260101-000003-dddd/lock";
    os_lockfile(v.os, a, lockb);
    Parked k = {};
    k.sid  = "20260101-000002-cccc";
    k.lock = os_lockfile(v.os, a, lockc);
    e->parked = push(&a, e->parked, k);
    Session *sd = alloc<Session>(&a);
    sd->sid  = "20260101-000003-dddd";
    sd->lock = os_lockfile(v.os, a, lockd);
    Session *se = alloc<Session>(&a);
    se->sid      = "20260101-000004-eeee";
    se->lock     = -1;
    se->readonly = 1;
    sd->next     = se;
    e->sessions  = sd;
    os_lockfile(v.os, a, "/s/sessions/20260101-000004-eeee/lock");  // the other's

    struct { Str sid; Str data; Str want; } puts[] = {
        {"20260101-000000-aaaa", "closed", ""},
        {"20260101-000001-bbbb", "elsewhere", "open in another process"},
        {"20260101-000002-cccc", "parked", ""},
        {"20260101-000003-dddd", "open", ""},
        {"20260101-000004-eeee", "read-only", "open in another process"},
        {"20260101-000009-gone", "gone", "no such session"},
        {"../20260101-000000-aaaa", "escape", "no such session"},
    };
    for (iz i = 0; i < countof(puts); i++) {
        Str err  = sidecarput(e, v.scratch, puts[i].sid, "group", puts[i].data);
        Str side = sidecarget(e, &a, puts[i].sid, "group", 64);
        expect(t, concat(&a, "sidecarput ", puts[i].sid),
               concat(&a, err, concat(&a, "|", side.data ? side : Str("(none)"))),
               concat(&a, puts[i].want, concat(&a, "|",
                      puts[i].want.len ? Str("(none)") : puts[i].data)));
    }
    expect(t, "sidecar lock let go", istr(&a,
           os_lockheld(v.os, a, "/s/sessions/20260101-000000-aaaa/lock")), "0");
    expect(t, "sidecar locks kept", istr(&a, os_lockheld(v.os, a, lockb) +
           os_lockheld(v.os, a, lockc) + os_lockheld(v.os, a, lockd)), "3");

    // Empty, removed, and removed again
    Str aaaa = "20260101-000000-aaaa";
    sidecarput(e, v.scratch, aaaa, "draft", "");
    Str empty = sidecarget(e, &a, aaaa, "draft", 64);
    expect(t, "sidecar empty", istr(&a, empty.data && !empty.len), "1");
    expect(t, "sidecar too long", istr(&a, !sidecarget(e, &a, aaaa, "group", 3).data), "1");
    Str gone = concat(&a, sidecarput(e, v.scratch, aaaa, "group", {}), "|");
    gone = concat(&a, gone, sidecarput(e, v.scratch, aaaa, "group", {}));
    expect(t, "sidecar removed", concat(&a, gone,
           sidecarget(e, &a, aaaa, "group", 64).data ? Str("|there") : Str("|none")),
           "||none");

    // borrow: what is open, nothing for what is not there
    b32 transient = 1;
    expect(t, "borrow open", istr(&a, borrow(e, v.scratch, sd->sid, &transient)==sd &&
           !transient), "1");
    transient = 1;
    expect(t, "borrow invalid", istr(&a, !borrow(e, v.scratch, "../x", &transient) &&
           !transient), "1");
    transient = 1;
    expect(t, "borrow missing", istr(&a,
           !borrow(e, v.scratch, "20260101-000009-gone", &transient) && !transient), "1");
    Parked q = {};
    q.sid  = "20260101-000009-gone";
    q.lock = os_lockfile(v.os, a, "/s/sessions/20260101-000009-gone/lock");
    e->parked = push(&a, e->parked, q);
    expect(t, "borrow parked, its log gone", istr(&a,
           !borrow(e, v.scratch, q.sid, &transient) && e->parked.len==1 &&
           !os_lockheld(v.os, a, "/s/sessions/20260101-000009-gone/lock")), "1");

    // The commands: a draft past DRAFT_MAX refused, one written over
    // another process's lock, as the last writer wins, where archive and
    // group are refused, and a group's name checked
    e->out = newmailbox(&a, 1<<16, 0, 0);
    Buf big(&a, DRAFT_MAX + 128);
    print(&big, "{\"cmd\":\"draft\",\"id\":1,\"sid\":\"20260101-000001-bbbb\",\"text\":\"");
    for (iz i = 0; i < DRAFT_MAX+1; i++) {
        putbyte(&big, 'x');
    }
    print(&big, "\"}");
    Str cmds[] = {
        finish(&big),
        "{\"cmd\":\"draft\",\"id\":2,\"sid\":\"20260101-000001-bbbb\",\"text\":\"mine\"}",
        "{\"cmd\":\"archive\",\"id\":3,\"sid\":\"20260101-000001-bbbb\"}",
        "{\"cmd\":\"group\",\"id\":4,\"sid\":\"20260101-000001-bbbb\",\"name\":\"g\"}",
        "{\"cmd\":\"group\",\"id\":5,\"sid\":\"20260101-000000-aaaa\",\"name\":\" a\\tb \"}",
        "{\"cmd\":\"group\",\"id\":6,\"sid\":\"20260101-000000-aaaa\"}",
        "{\"cmd\":\"group\",\"id\":7,\"sid\":\"20260101-000000-aaaa\",\"name\":\" Mine \"}",
        "{\"cmd\":\"archive\",\"id\":8,\"sid\":\"20260101-000000-aaaa\",\"on\":true}",
        "{\"cmd\":\"draft\",\"id\":9,\"sid\":\"20260101-000000-aaaa\",\"text\":7}",
    };
    for (iz i = 0; i < countof(cmds); i++) {
        command(e, v.scratch, cmds[i]);
    }
    Buf  got(&a, 1024);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        print(&got, jsonint(jsonget(rec.root, "id"), 0));
        putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
        print(&got, jsonstr(jsonget(rec.root, "err")));
        putbyte(&got, '\n');
    }
    expect(t, "sidecar commands", finish(&got),
           "1-the draft is too long: at most 1 MiB\n"
           "2+\n"
           "3-open in another process\n"
           "4-open in another process\n"
           "5-name must be one line of text, or null to leave the group\n"
           "6-name must be one line of text, or null to leave the group\n"
           "7+\n"
           "8+\n"
           "9-text must be a string\n");
    expect(t, "sidecar commands wrote", concat(&a,
           sidecarget(e, &a, "20260101-000001-bbbb", "draft", 64), concat(&a, "|",
           concat(&a, sidecarget(e, &a, aaaa, "group", 64), concat(&a, "|",
           sidecarget(e, &a, aaaa, "archived", 64))))), "mine|Mine|1");
}

// Models as settings name them: ENDPOINT/MODEL only where ENDPOINT is a
// configured endpoint, so that a model named with a slash stays whole;
// and a subagent's model, known by its section or its endpoint's
// listing, or anything at an endpoint never listed.
static void testtargets(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    os_write(v.os, a, "/r/h.ini",
             "[endpoint fast]\nurl = http://fast/v1\n"
             "[endpoint Qwen]\nkind = openai\n"  // no url: not an endpoint
             "[model small]\n[model big]\n");
    Engine *e   = alloc<Engine>(&a);
    byte   *mem = alloc<byte>(&a, 1<<16);
    e->os   = v.os;
    e->perm = {mem, mem + (1<<16)};
    e->cfg  = newconfig(v.os, &a, v.scratch, "/r/h.ini", 0);
    e->ini  = e->cfg->ini;

    struct { Str spec; Str want; } specs[] = {
        {"small",             "home|small"},
        {"fast/small",        "fast|small"},
        {"Qwen/Qwen3-8B",     "home|Qwen/Qwen3-8B"},
        {"org/model",         "home|org/model"},
        {"fast/org/model",    "fast|org/model"},
        {"fast/",             "home|fast/"},
        {"/small",            "home|/small"},
        {"",                  "home|"},
    };
    for (iz i = 0; i < countof(specs); i++) {
        Target got = targetof(e, specs[i].spec, "home");
        expect(t, concat(&a, "targetof ", specs[i].spec),
               concat(&a, concat(&a, got.which, "|"), got.model), specs[i].want);
    }

    Str url = "http://fast/v1";
    expect(t, "submodel never listed", submodel(e, &a, v.scratch, "any", url), "");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"listed\"},{\"id\":\"small\"},"
                                 "{\"id\":\"other\"}]}");
    expect(t, "submodel listed", submodel(e, &a, v.scratch, "listed", url), "");
    expect(t, "submodel configured", submodel(e, &a, v.scratch, "big", url), "");
    expect(t, "submodel unknown", submodel(e, &a, v.scratch, "nope", url),
           "No subagent was started: no model \"nope\" is configured or listed "
           "at http://fast/v1. Give one of these, or none for the default: "
           "small, big, listed, other.");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"nope\"}]}");
    expect(t, "submodel listed anew", submodel(e, &a, v.scratch, "nope", url), "");
    expect(t, "submodel listed no more", istr(&a,
           submodel(e, &a, v.scratch, "listed", url).len > 0), "1");
}

static void test_engine(Test *t)
{
    testroles(t);
    testlist(t);
    testsidecars(t);
    testdelete(t);
    testtargets(t);
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
        expect(t, concat(a, "title ", titles[i].in), cleantitle(a, titles[i].in, TITLE_MAX),
               titles[i].want);
    }

    // Exactly 60 code points of two-byte characters: no ellipsis
    Buf b(a, 256);
    for (i32 i = 0; i < 60; i++) {
        print(&b, "\xc3\xa9");
    }
    Str sixty = finish(&b);
    expect(t, "title of 60 code points", cleantitle(a, sixty, TITLE_MAX), sixty);
    Str more = concat(a, sixty, "\xc3\xa9");
    expect(t, "title of 61 code points",
           cleantitle(a, more, TITLE_MAX), concat(a, sixty, "\xe2\x80\xa6"));

    // No space in its second half: cut inside the long word
    Buf in(a, 128);
    print(&in, "x ");
    for (i32 i = 0; i < 70; i++) {
        putbyte(&in, 'a');
    }
    Str word = finish(&in);
    expect(t, "title cut in a word", cleantitle(a, word, TITLE_MAX),
           concat(a, takehead(word, 60), "\xe2\x80\xa6"));

    // A user's title (rename) goes to 120 code points, on one line
    Buf rn(a, 512);
    Buf rw(a, 512);
    for (i32 i = 0; i < 26; i++) {
        print(&rn, i&1 ? Str("word\n") : Str("word\t"));
        if (i < 24) {
            print(&rw, i ? Str(" word") : Str("word"));  // 119 code points
        }
    }
    Str words = finish(&rn);
    expect(t, "user title cut", cleantitle(a, words, RENAME_MAX),
           concat(a, finish(&rw), "\xe2\x80\xa6"));
    expect(t, "user title fits", cleantitle(a, takehead(words, 30), RENAME_MAX),
           "word word word word word word");
    expect(t, "user title blank", istr(a, cleantitle(a, " \t\r\n\x7f ", RENAME_MAX).len), "0");

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
            "{\"cmd\":\"rename\",\"id\":9,\"sid\":\"x\",\"title\":\"t\"}",
            "{\"cmd\":\"delete\",\"id\":10,\"sid\":\"x\"}",
            "{\"cmd\":\"fork\",\"id\":11,\"sid\":\"x\"}",
            "{\"cmd\":\"compact\",\"id\":12,\"sid\":\"x\"}",
            "{\"cmd\":\"model\",\"id\":13,\"sid\":\"x\",\"model\":\"m\"}",
            "{\"cmd\":\"archive\",\"id\":14,\"sid\":\"x\",\"on\":1}",
            "{\"cmd\":\"group\",\"id\":15,\"sid\":\"x\",\"name\":\"g\"}",
            "{\"cmd\":\"draft\",\"id\":16,\"sid\":\"x\",\"text\":\"t\"}",
            "{\"cmd\":\"draft\",\"id\":17,\"sid\":\"../x\",\"text\":\"t\"}",
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
        Str  hello = {};
        Mail m = mailtake(e->out);
        for (Record r; (r = mailnext(&m)).ok;) {
            EvRec rec = evparse(a, r.data);
            if (rec.ev == EV_HELLO) {
                hello = clone(a, r.data);
            }
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
               "9-the engine is stopping\n"
               "10-the engine is stopping\n"
               "11-the engine is stopping\n"
               "12-the engine is stopping\n"
               "13-the engine is stopping\n"
               "14-no such session\n"
               "15-no such session\n"
               "16-no such session\n"
               "17-invalid session id\n"
               "8-command too long\n");
        Buf want(a, 1024);
        print(&want, "{\"ts\":0,\"ev\":\"hello\",\"proto\":0,\"version\":\""
                     PROSPERO_VERSION "\",");
        JsonOut w = {&want};
        w.depth   = 1;  // inside the record, as evbegin leaves it
        w.objects = 1;
        w.comma   = 0;
        putcaps(&w, 0);
        print(&want, "}\n");
        expect(t, "hello", hello, finish(&want));
    }

    // Not stopping: the session commands, on sessions that are not there
    {
        Engine *e = alloc<Engine>(a);
        e->os  = newos(a, 1<<16);
        e->out = newmailbox(a, 1<<16, 0, 0);
        Str cmds[] = {
            "{\"cmd\":\"delete\",\"id\":1,\"sid\":\"20260101-000000-aaaa\"}",
            "{\"cmd\":\"archive\",\"id\":2,\"sid\":\"20260101-000000-aaaa\",\"on\":1}",
            "{\"cmd\":\"group\",\"id\":3,\"sid\":\"20260101-000000-aaaa\",\"name\":\"g\"}",
            "{\"cmd\":\"fork\",\"id\":4,\"sid\":\"20260101-000000-aaaa\"}",
            "{\"cmd\":\"draft\",\"id\":5,\"sid\":\"20260101-000000-aaaa\",\"text\":\"t\"}",
            "{\"cmd\":\"delete\",\"id\":6,\"sid\":\"a/b\"}",
            "{\"cmd\":\"fork\",\"id\":7}",
            "{\"cmd\":\"rename\",\"id\":8,\"sid\":\"20260101-000000-aaaa\",\"title\":\"t\"}",
            "{\"cmd\":\"compact\",\"id\":9,\"sid\":\"20260101-000000-aaaa\"}",
            "{\"cmd\":\"model\",\"id\":10,\"sid\":\"20260101-000000-aaaa\",\"model\":\"m\"}",
            "{\"cmd\":\"open\",\"id\":11,\"sid\":\"C:x\"}",
        };
        for (iz i = 0; i < countof(cmds); i++) {
            command(e, *a, cmds[i]);
        }
        Buf  got(a, 1024);
        Mail m = mailtake(e->out);
        for (Record r; (r = mailnext(&m)).ok;) {
            EvRec rec = evparse(a, r.data);
            print(&got, jsonint(jsonget(rec.root, "id"), 0));
            putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
            print(&got, jsonstr(jsonget(rec.root, "err")));
            print(&got, rec.sid.len ? Str(" @") : Str(""));
            print(&got, rec.sid.len ? takehead(rec.sid, 4) : Str(""));
            putbyte(&got, '\n');
        }
        expect(t, "the commands to come", finish(&got),
               "1-no such session @2026\n"
               "2-no such session @2026\n"
               "3-no such session @2026\n"
               "4-no such session @2026\n"
               "5-no such session @2026\n"
               "6-invalid session id\n"
               "7-invalid session id\n"
               "8-no such session @2026\n"
               "9-no such open session @2026\n"
               "10-no such session @2026\n"
               "11-invalid session id\n");
    }
}
