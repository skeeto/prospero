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
    expect(t, "a section's default tools", istr(&a, op.ok*10 + (op.deny==(TOOL_OFFMASK|TOOL_PLANMASK|TOOL_ASKMASK))), "11");
    expect(t, "general", istr(&a, findrole(e, "general").deny),
           istr(&a, TOOL_OFFMASK|TOOL_PLANMASK|TOOL_ASKMASK));
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
        Str log = concat(a, dir, "/events.jsonl");
        os_write(os, *a, log, finish(&b));
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
    os_write(v.os, a, "/s/sessions/20260101-000005-ffff/lastact", "1767229200000");
    os_write(v.os, a, "/s/sessions/20260101-000007-hhhh/lastact", "junk");
    os_write(v.os, a, "/s/sessions/20260101-000008-iiii/lastact", "5");
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

        // The latest activity, past the sids' first second: the lastact
        // sidecar's, but never before the making in the sid, which is
        // also what a session without one, or with junk, gets
        Buf acts(&tmp, 256);
        for (iz i = 0; i < sc.list.len; i++) {
            print(&acts, cuthead(sc.list[i].sid, 16));
            putbyte(&acts, '=');
            print(&acts, sc.list[i].lastact - 1767225600000);
            putbyte(&acts, ' ');
        }
        expect(t, "lastact", finish(&acts),
               "aaaa=0 bbbb=1000 cccc=2000 dddd=3000 eeee=4000 ffff=3600000 "
               "gggg=6000 hhhh=7000 iiii=8000 ");
    }

    cmdlist(e, v.scratch, 1, 0);
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
                 "\"lastact\":1767225600000,\"locked\":0,\"jobs\":0}");
    Buf entry(&a, 1024);
    JsonOut w = {&entry};
    putjson(&w, jsonat(list, 3));
    expect(t, "list entry", finish(&entry), finish(&want));
    entry.len = 0;
    w = {&entry};
    putjson(&w, jsonat(list, 5));
    expect(t, "list subagent entry", finish(&entry),
           "{\"sid\":\"20260101-000003-dddd\",\"title\":\"A grandchild\",\"cwd\":\"/w\","
           "\"model\":\"m\",\"state\":\"closed\",\"mtime\":0,\"lastact\":1767225603000,"
           "\"locked\":0,\"jobs\":0,\"parent\":\"20260101-000001-bbbb\",\"depth\":2}");
    entry.len = 0;
    w = {&entry};
    putjson(&w, jsonat(list, 2));
    expect(t, "list parked entry", finish(&entry),
           "{\"sid\":\"20260101-000002-cccc\",\"title\":\"\",\"cwd\":\"/w\","
           "\"model\":\"m\",\"state\":\"idle\",\"mtime\":0,\"lastact\":1767225602000,"
           "\"locked\":0,\"jobs\":0,\"parked\":1}");
    expect(t, "list record", istr(&a, startswith(line, "{\"ts\":0,\"ev\":\"sessions\","
           "\"list\":[{\"sid\":\"20260101-000008-iiii\"")), "1");
}

// delete: a tree, checked whole first, refused while one of it is open in
// another process, nothing moved then; else children first, each into
// STATE/trash, which a thread would empty (none starts in a test).
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

// The Trash: only a root goes, its tree checked whole as for delete, and
// it stays where it is, marked on its root, out of a plain list, refused
// to open, subagents too; restored, it is back. The purge takes those
// in it long enough, a tree at a time, and trash.empty all the rest.
static void testtrash(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->statedir = "/s";
    e->perm     = carve(&a, 1<<16);  // sessions trashed due a sweep
    v.os->now   = (i64)1800000000000*1000;  // in 2027, as trashed times go
    MadeSession made[] = {
        {"20260101-000000-aaaa", {}},
        {"20260101-000001-bbbb", "20260101-000000-aaaa"},
        {"20260101-000003-dddd", {}},
    };
    makesessions(v.os, &a, {made, countof(made)});
    Str root = "/s/sessions/20260101-000000-aaaa";
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":1,\"sid\":\"20260101-000001-bbbb\"}");
    iz held = os_lockfile(v.os, a, "/s/sessions/20260101-000001-bbbb/lock");
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":2,\"sid\":\"20260101-000000-aaaa\"}");
    os_unlockfile(v.os, held);
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":3,\"sid\":\"20260101-000000-aaaa\"}");
    Str trashed = concat(&a, root, "/trashed");
    b32 marked = os_stat(v.os, a, trashed).type == STAT_FILE;
    b32 kidmark = os_stat(v.os, a, "/s/sessions/20260101-000001-bbbb/trashed").type == STAT_FILE;
    command(e, v.scratch, "{\"cmd\":\"list\",\"id\":4}");
    command(e, v.scratch, "{\"cmd\":\"list\",\"id\":5,\"trash\":1}");
    command(e, v.scratch, "{\"cmd\":\"open\",\"id\":6,\"sid\":\"20260101-000000-aaaa\"}");
    command(e, v.scratch, "{\"cmd\":\"open\",\"id\":7,\"sid\":\"20260101-000001-bbbb\"}");
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":8,\"sid\":\"20260101-000000-aaaa\",\"on\":false}");
    b32 restored = os_stat(v.os, a, trashed).type == STAT_MISSING;
    command(e, v.scratch, "{\"cmd\":\"list\",\"id\":9}");

    Buf  got(&a, 2048);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        if (rec.ev == EV_SESSIONS) {
            print(&got, "list");
            for (Json *k = jsonget(rec.root, "list")->child; k; k = k->next) {
                putbyte(&got, ' ');
                print(&got, cuthead(jsonstr(jsonget(k, "sid")), 16));
                print(&got, jsonint(jsonget(k, "trashed"), 0)>0 ? Str("*") : Str(""));
            }
            print(&got, " days ");
            print(&got, jsonint(jsonget(rec.root, "trash_days"), -1));
            putbyte(&got, '\n');
            continue;
        }
        print(&got, jsonint(jsonget(rec.root, "id"), 0));
        putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
        print(&got, jsonstr(jsonget(rec.root, "err")));
        for (Json *k = jsonget(rec.root, "trashed")->child; k; k = k->next) {
            putbyte(&got, ' ');
            print(&got, cuthead(jsonstr(k), 16));
        }
        putbyte(&got, '\n');
    }
    expect(t, "trash", finish(&got),
           "1-a subagent goes with the session that started it\n"
           "2-open in another process: its subagent 20260101-000001-bbbb\n"
           "3+ bbbb aaaa\n"
           "list dddd days 30\n"
           "4+\n"
           "list dddd aaaa* bbbb days 30\n"
           "5+\n"
           "6-in the Trash: restore it first\n"
           "7-in the Trash: restore it first\n"
           "8+\n"
           "list dddd aaaa bbbb days 30\n"
           "9+\n");
    expect(t, "trash marks", istr(&a, marked && !kidmark && restored), "1");

    // Long enough in the Trash, by a time written back: aaaa's tree goes,
    // dddd, only just in, stays for trash.empty
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":10,\"sid\":\"20260101-000000-aaaa\"}");
    command(e, v.scratch, "{\"cmd\":\"trash\",\"id\":11,\"sid\":\"20260101-000003-dddd\"}");
    os_write(v.os, a, trashed, "1000");
    Slice<Str> gone = {};
    Str        err  = trashpurge(e, &a, 30*DAY_MS, &gone);
    Buf        left(&a, 256);
    for (iz i = 0; i < gone.len; i++) {
        print(&left, cuthead(gone[i], 16));
        putbyte(&left, ' ');
    }
    print(&left, err.len ? err : Str("-"));
    expect(t, "trash purge", finish(&left), "bbbb aaaa -");
    mailtake(e->out);
    command(e, v.scratch, "{\"cmd\":\"trash.empty\",\"id\":12}");
    Buf  emptied(&a, 256);
    Mail m2 = mailtake(e->out);
    for (Record r; (r = mailnext(&m2)).ok;) {
        EvRec rec = evparse(&a, r.data);
        print(&emptied, jsonint(jsonget(rec.root, "ok"), 0) ? Str("+") : Str("-"));
        for (Json *k = jsonget(rec.root, "deleted")->child; k; k = k->next) {
            putbyte(&emptied, ' ');
            print(&emptied, cuthead(jsonstr(k), 16));
        }
    }
    for (iz i = 0; i < countof(made); i++) {
        Str dir = concat(&a, "/s/sessions/", made[i].sid);
        putbyte(&emptied, os_stat(v.os, a, dir).type==STAT_DIR ? '1' : '0');
    }
    expect(t, "trash empty", finish(&emptied), "+ dddd000");
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
    e->perm     = carve(&a, 1<<16);  // an archive's sessions due a sweep
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
// and a subagent's model, known by its section, its own or its
// endpoint's, or its endpoint's listing, or anything at an endpoint
// never listed.
static void testtargets(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    os_write(v.os, a, "/r/h.ini",
             "[endpoint fast]\nurl = http://fast/v1\n"
             "[endpoint Qwen]\nkind = openai\n"  // no url: not an endpoint
             "[model small]\n[model big]\n[model fast/solo]\n[model fast/big]\n"
             "[model slow/far]\n[endpoint slow]\nurl = http://slow/\n");
    Engine *e = alloc<Engine>(&a);
    e->os   = v.os;
    e->perm = carve(&a, 1<<16);
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
    expect(t, "submodel never listed", submodel(e, &a, v.scratch, "any", "fast", url), "");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"listed\"},{\"id\":\"small\"},"
                                 "{\"id\":\"other\"}]}");
    expect(t, "submodel listed", submodel(e, &a, v.scratch, "listed", "fast", url), "");
    expect(t, "submodel configured", submodel(e, &a, v.scratch, "big", "fast", url), "");
    expect(t, "submodel qualified", submodel(e, &a, v.scratch, "solo", "fast", url), "");
    expect(t, "submodel elsewhere", istr(&a,
           submodel(e, &a, v.scratch, "far", "fast", url).len > 0), "1");
    expect(t, "submodel unknown", submodel(e, &a, v.scratch, "nope", "fast", url),
           "No subagent was started: no model \"nope\" is configured or listed "
           "at http://fast/v1. Give one of these, or none for the default: "
           "small, big, solo, listed, other.");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"nope\"}]}");
    expect(t, "submodel listed anew", submodel(e, &a, v.scratch, "nope", "fast", url), "");
    expect(t, "submodel listed no more", istr(&a,
           submodel(e, &a, v.scratch, "listed", "fast", url).len > 0), "1");

    // A new session's defaults: the configuration's, else the first
    // model its endpoint listed, the one named or the first with a URL
    os_write(v.os, a, "/r/d.ini",
             "[endpoint Qwen]\nkind = openai\n"
             "[endpoint fast]\nurl = http://fast/v1\n"
             "[endpoint slow]\nurl = http://slow/\n");
    e->cfg       = newconfig(v.os, &a, v.scratch, "/r/d.ini", 0);
    e->ini       = e->cfg->ini;
    e->listings  = 0;
    CfgDefaults d = defaults(e, v.scratch, {});
    expect(t, "defaults unlisted", concat(&a, concat(&a, d.endpoint, "|"), d.model),
           "fast|");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"one\"},{\"id\":\"two\"}]}");
    d = defaults(e, v.scratch, {});
    expect(t, "defaults listed", concat(&a, concat(&a, d.endpoint, "|"), d.model),
           "fast|one");
    d = defaults(e, v.scratch, "slow");
    expect(t, "defaults elsewhere", concat(&a, concat(&a, d.endpoint, "|"), d.model),
           "slow|");
    os_write(v.os, a, "/r/d.ini", "model = m\n[endpoint fast]\nurl = http://fast/v1\n");
    e->cfg = newconfig(v.os, &a, v.scratch, "/r/d.ini", 0);
    e->ini = e->cfg->ini;
    d = defaults(e, v.scratch, {});
    expect(t, "defaults configured", concat(&a, concat(&a, d.endpoint, "|"), d.model),
           "fast|m");

    // A guess among endpoints that all list the model: the first, and
    // the others, for the session's note
    os_write(v.os, a, "/r/d.ini",
             "model = m\n"
             "[endpoint fast]\nurl = http://fast/v1\n"
             "[endpoint slow]\nurl = http://slow/\n"
             "[endpoint more]\nurl = http://more/\n");
    e->cfg = newconfig(v.os, &a, v.scratch, "/r/d.ini", 0);
    e->ini = e->cfg->ini;
    notekinds(e, v.scratch, "http://slow/", "{\"data\":[{\"id\":\"m\"}]}");
    d = defaults(e, v.scratch, {});
    expect(t, "defaults one lists", concat(&a, concat(&a, d.endpoint, "|"), d.also),
           "slow|");
    notekinds(e, v.scratch, url, "{\"data\":[{\"id\":\"m\"}]}");
    notekinds(e, v.scratch, "http://more/", "{\"data\":[{\"id\":\"m\"}]}");
    d = defaults(e, v.scratch, {});
    expect(t, "defaults all list", concat(&a, concat(&a, concat(&a, d.endpoint, "|"),
                                                      concat(&a, d.also, "|")),
                                           istr(&a, d.nalso)), "fast|slow|2");
    d = defaults(e, v.scratch, "more");
    expect(t, "defaults named", concat(&a, concat(&a, d.endpoint, "|"), d.also), "more|");
}

// A typed directory's leading ~ (new, import): this host's home, else
// refused, as is ~user; a ~ elsewhere is a plain name.
static void testtilde(Test *t)
{
    Arena *a = &t->perm;
    struct { Str path; Str home; b32 win; Str want; } cases[] = {
        {"~", "/home/u", 0, "/home/u"},
        {"~/src/x", "/home/u", 0, "/home/u/src/x"},
        {"~/", "/home/u", 0, "/home/u/"},
        {"~\\x", "/home/u", 0, "!~user is not supported: give the directory's full path"},
        {"~\\x", "C:\\Users\\u", 1, "C:\\Users\\u\\x"},
        {"~/x", "C:\\Users\\u", 1, "C:\\Users\\u/x"},
        {"~bob/x", "/home/u", 0, "!~user is not supported: give the directory's full path"},
        {"~/x", "", 0, "!no home directory for ~: HOME is not set"},
        {"~", "", 1, "!no home directory for ~: neither HOME nor USERPROFILE is set"},
        {"/tmp/~", "", 0, "/tmp/~"},
        {"a~", "/home/u", 0, "a~"},
        {"", "/home/u", 0, ""},
    };
    for (iz i = 0; i < countof(cases); i++) {
        HomePath r = tildepath(a, cases[i].path, cases[i].home, cases[i].win);
        Str got = r.err.len ? concat(a, "!", r.err) : r.path;
        expect(t, concat(a, "tilde ", cases[i].path), got, cases[i].want);
    }
}

// A subagent of another host's session (new {task}): the task as new
// gives it, its model, and its list entry, a root here even where its
// parent's sid names a session here too.
static void testremotetask(Test *t)
{
    Arena a = t->perm;
    struct { Str json; Str want; } tasks[] = {
        {"{}", "none"},
        {"{\"task\":null}", "none"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"ssh\"}}", "ok|20260101-000000-aaaa|c1|ssh||1|0|0"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"wsl\",\"distro\":\"Ubuntu-24.04\",\"depth\":9,"
         "\"hop\":-3,\"plan\":true,\"title\":\"T\",\"prefer\":\" e/m \"}}",
         "ok|20260101-000000-aaaa|c1|wsl|Ubuntu-24.04|2|0|1"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"wsl\",\"depth\":0,\"hop\":99}}",
         "ok|20260101-000000-aaaa|c1|wsl||1|8|0"},
        {"{\"task\":\"go\"}", "!task must be an object"},
        {"{\"task\":{\"parent\":\"../x\",\"call\":\"c1\",\"prompt\":\"go\","
         "\"via\":\"ssh\"}}", "!task.parent must be a session id"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"prompt\":\"go\","
         "\"via\":\"ssh\"}}", "!task.call must name the task call"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\" \",\"via\":\"ssh\"}}", "!task.prompt is empty"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"door\"}}", "!task.via must be wsl, ssh or exec"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"ssh\",\"distro\":\"U\"}}",
         "!task.distro must be a WSL distro's name"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"wsl\",\"distro\":\"a/b\"}}",
         "!task.distro must be a WSL distro's name"},
        {"{\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c1\","
         "\"prompt\":\"go\",\"via\":\"wsl\",\"distro\":\"-x\"}}",
         "!task.distro must be a WSL distro's name"},
    };
    for (iz i = 0; i < countof(tasks); i++) {
        JsonDoc d = jsonparse(&a, tasks[i].json);
        NewTask r = newtaskof(jsonget(d.root, "task"));
        Buf     b(&a, 256);
        if (r.err.len) {
            print(&b, "!");
            print(&b, r.err);
        } else if (!r.ok) {
            print(&b, "none");
        } else {
            Str parts[] = {r.parent, r.call, r.via, r.distro};
            print(&b, "ok");
            for (iz k = 0; k < countof(parts); k++) {
                putbyte(&b, '|');
                print(&b, parts[k]);
            }
            i64 nums[] = {r.depth, r.hop, r.plan};
            for (iz k = 0; k < countof(nums); k++) {
                putbyte(&b, '|');
                print(&b, nums[k]);
            }
        }
        expect(t, concat(&a, "newtaskof ", tasks[i].json), finish(&b), tasks[i].want);
    }

    // The call's model, else the role's (new's own), else the parent's
    // where this host has its endpoint, else the default
    CfgEnv v = newcfgenv(&a);
    os_write(v.os, a, "/r/h.ini", "[endpoint fast]\nurl = http://fast/v1\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/r/h.ini", 0);
    struct { Str model, role, prefer, want; } models[] = {
        {"x",   "r", "fast/m", "x"},
        {"",    "r", "fast/m", ""},
        {"",    "",  "fast/m", "fast/m"},
        {"",    "",  "slow/m", ""},
        {"",    "",  "m",      ""},
        {"",    "",  "",       ""},
    };
    for (iz i = 0; i < countof(models); i++) {
        Str got = taskmodel(c, models[i].model, models[i].role, models[i].prefer);
        Str tag = concat(&a, concat(&a, models[i].model, "|"), models[i].prefer);
        expect(t, concat(&a, "taskmodel ", tag), got, models[i].want);
    }

    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->statedir = "/s";
    MadeSession made[] = {{"20260101-000000-aaaa", {}}};
    makesessions(v.os, &a, {made, countof(made)});
    os_mkdirs(v.os, a, "/s/sessions/20260101-000001-bbbb");
    os_write(v.os, a, "/s/sessions/20260101-000001-bbbb/events.jsonl",
             "{\"sid\":\"20260101-000001-bbbb\",\"seq\":1,\"ts\":0,\"ev\":\"session\","
             "\"cwd\":\"/w\",\"model\":\"m\",\"parent\":\"20260101-000000-aaaa\","
             "\"call\":\"c1\",\"depth\":1,\"remote\":1,\"via\":\"ssh\"}\n");
    {
        Arena    tmp = v.scratch;
        SessScan sc  = scansessions(e, &tmp);
        expect(t, "remote child no kid here", istr(&tmp, sc.list[0].kids.len), "0");
        expect(t, "remote child a root here", sc.list[1].parent, "");
    }
    cmdlist(e, v.scratch, 1, 0);
    Mail m    = mailtake(e->out);
    Str  line = mailnext(&m).data;
    JsonDoc d = jsonparse(&a, line);
    Json   *x = jsonat(jsonget(d.root, "list"), 0);
    Buf     entry(&a, 1024);
    JsonOut w = {&entry};
    putjson(&w, x);
    expect(t, "list remote child entry", finish(&entry),
           "{\"sid\":\"20260101-000001-bbbb\",\"title\":\"\",\"cwd\":\"/w\","
           "\"model\":\"m\",\"state\":\"closed\",\"mtime\":0,\"lastact\":1767225601000,"
           "\"locked\":0,\"jobs\":0,\"parent\":\"20260101-000000-aaaa\",\"remote\":1}");
}

// The prompt's ETA (prefilleta), against a server whose cost per token
// is a line over position, as attention makes it: ms at n tokens is
// the line's integral, rounded as time_ms is.
static void testprefill(Test *t)
{
    Arena a = t->perm;
    struct { f64 a, b; } c = {2.0, 0.0001};  // ms per token at x: a + b*x
    auto at = [&](i64 n) {
        f64 x = (f64)n;
        return (i64)(c.a*x + c.b*x*x/2 + 0.5);
    };
    auto near = [&](i64 got, i64 want) {  // within 0.5%, time_ms rounded
        return got*200>=want*199 && got*200<=want*201 ? Str("ok") : istr(&a, got);
    };
    i64 total = 20480, batch = 2048;

    // Nothing to go on at 0%, the one batch's rate gone on flat after
    // it, then the line
    Prefill     p    = {};
    PrefillLine last = {};
    expect(t, "prefill: at 0%, no ETA",
           istr(&a, prefilleta(&p, &last, 7, 0, total, at(0))), "-1");
    expect(t, "prefill: one batch, flat",
           near(prefilleta(&p, &last, 7, batch, total, at(batch)),
                at(batch)*(total - batch)/batch), "ok");
    for (i64 n = 2*batch; n < total; n += batch) {
        i64 eta = prefilleta(&p, &last, 7, n, total, at(n));
        expect(t, concat(&a, "prefill: the line at ", istr(&a, n)),
               near(eta, at(total) - at(n)), "ok");
    }
    expect(t, "prefill: done", istr(&a, prefilleta(&p, &last, 7, total, total, at(total))), "0");
    expect(t, "prefill: the line kept", istr(&a, last.key==7 && last.b>0), "1");

    // The next request, for the same model, from its 0% on, its cache
    // counted in its position; another model's has nothing to go on
    Prefill q = {};
    i64 cached = 6000;
    expect(t, "prefill: the last line at 0%",
           near(prefilleta(&q, &last, 7, cached, total, 0), at(total) - at(cached)), "ok");
    // Slower now (another request shares the server): the one batch
    // moves the line, its slope kept
    i64 eta = prefilleta(&q, &last, 7, cached+batch, total, 2*(at(cached+batch) - at(cached)));
    i64 want = 2*(at(total) - at(cached)) - 2*(at(cached+batch) - at(cached));
    expect(t, "prefill: the last line, moved", istr(&a, eta>want/2 && eta<want), "1");
    Prefill o = {};
    expect(t, "prefill: another model's line unused",
           istr(&a, prefilleta(&o, &last, 9, 0, total, 0)), "-1");

    // Lines by endpoint and model, the least recently used replaced
    {
        Engine eng = {};
        for (u64 k = 1; k <= 16; k++) {
            prefillline(&eng, k)->key = k;
        }
        prefillline(&eng, 1);
        PrefillLine *l = prefillline(&eng, 17);
        expect(t, "prefill: the oldest line replaced", istr(&a, (i64)l->key), "2");
        expect(t, "prefill: a line found", istr(&a, prefillline(&eng, 1)->key==1), "1");
    }

    // Cheaper further in, as only noise makes it: flat at the mean
    Prefill     d     = {};
    PrefillLine dl    = {};
    i64         ms[]  = {0, 3000, 5000, 6000};
    for (i32 i = 0; i < 3; i++) {
        prefilleta(&d, &dl, 3, i*1000, 10000, ms[i]);
    }
    i64 flat = prefilleta(&d, &dl, 3, 3000, 10000, ms[3]);
    expect(t, "prefill: no slope below zero", istr(&a, dl.b==0 && flat>0 && flat<7*3000), "1");

    // A repeated or out-of-order event changes nothing
    Prefill r = {};
    PrefillLine rl = {};
    prefilleta(&r, &rl, 5, 0, 1000, 0);
    prefilleta(&r, &rl, 5, 100, 1000, 100);
    prefilleta(&r, &rl, 5, 200, 1000, 200);
    expect(t, "prefill: a repeat", istr(&a, prefilleta(&r, &rl, 5, 200, 1000, 200)), "800");
    expect(t, "prefill: time going back", istr(&a, prefilleta(&r, &rl, 5, 300, 1000, 150)), "700");
    // then from where it was: 200 tokens in 300 ms, 1.5 ms each at 300,
    // against 1 ms at 50 and at 150 (weighing 56.25 and 75 of 200 by
    // now), a line integrated from 400 to 1000, worked out by hand
    expect(t, "prefill: then from where it was",
           istr(&a, prefilleta(&r, &rl, 5, 400, 1000, 500)), "1449");
}

// A tool result's several images: its record keeps them, the first
// under the keys of one image as well, and replays them, labels and
// all; a PDF's record has no images; an older record replays as before.
static void testcallimages(Test *t)
{
    Arena    a = t->perm;
    MsgImage imgs[2] = {};
    imgs[0].name  = "out/c1.png";
    imgs[0].mime  = "image/png";
    imgs[0].w     = 64;
    imgs[0].h     = 32;
    imgs[0].label = "image 1 (page 1)";
    imgs[1].name  = "out/c1-1.jpg";
    imgs[1].mime  = "image/jpeg";
    imgs[1].w     = 10;
    imgs[1].h     = 20;
    Msg m = {};
    m.role   = ROLE_TOOL;
    m.images = {imgs, 2, 2};
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        putobject(&w);
        putcallimages(&w, &m, {});
        putend(&w);
        Str rec = finish(&b);
        expect(t, "callimages record", rec,
               "{\"images\":[{\"name\":\"out/c1.png\",\"mime\":\"image/png\",\"w\":64,"
               "\"h\":32,\"label\":\"image 1 (page 1)\"},{\"name\":\"out/c1-1.jpg\","
               "\"mime\":\"image/jpeg\",\"w\":10,\"h\":20}],\"image\":\"out/c1.png\","
               "\"width\":64,\"height\":32}");

        Msg     got = {};
        JsonDoc d   = jsonparse(&a, rec);
        getcallimages(&a, d.root, &got);
        Buf g(&a, 256);
        print(&g, got.image.len ? Str("image ") : Str(""));
        for (iz i = 0; i < got.images.len; i++) {
            MsgImage *x = &got.images[i];
            print(&g, x->name);
            putbyte(&g, ' ');
            print(&g, x->mime);
            putbyte(&g, ' ');
            print(&g, (i64)x->w);
            putbyte(&g, 'x');
            print(&g, (i64)x->h);
            print(&g, " [");
            print(&g, x->label);
            print(&g, "]|");
        }
        expect(t, "callimages replay", finish(&g),
               "out/c1.png image/png 64x32 [image 1 (page 1)]|"
               "out/c1-1.jpg image/jpeg 10x20 []|");
    }

    // A PDF sent as a document: never images as well
    m.image = "out/c2.pdf";
    m.pages = 3;
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        putobject(&w);
        putcallimages(&w, &m, {});
        putend(&w);
        expect(t, "callimages pdf", finish(&b), "{\"pdf\":\"out/c2.pdf\",\"pages\":3}");
    }

    // One screenshot, and an older record of one, replayed as one
    Msg one = {};
    one.role  = ROLE_TOOL;
    one.image = "out/c3.png";
    one.imgw  = 8;
    one.imgh  = 6;
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        putobject(&w);
        putcallimages(&w, &one, "{\"x\":1}");
        putend(&w);
        Str rec = finish(&b);
        expect(t, "callimages one", rec,
               "{\"image\":\"out/c3.png\",\"width\":8,\"height\":6,\"shot\":\"{\\\"x\\\":1}\"}");
        Msg     got = {};
        JsonDoc d   = jsonparse(&a, rec);
        getcallimages(&a, d.root, &got);
        expect(t, "callimages one replay", concat(&a, got.image,
               istr(&a, got.imgw*1000 + got.imgh*10 + got.imgread + 100000*got.images.len)),
               "out/c3.png8060");
        d = jsonparse(&a, "{\"pdf\":\"out/c4.pdf\",\"pages\":0,\"images\":[]}");
        got = {};
        getcallimages(&a, d.root, &got);
        expect(t, "callimages pdf replay", concat(&a, got.image,
               istr(&a, got.pages*10 + got.imgread)), "out/c4.pdf11");
    }

    // The keys of one image, sent as a PNG, name the first PNG, never a
    // JPEG, or are left out
    MsgImage first = imgs[0];
    imgs[0] = imgs[1];
    imgs[1] = first;
    m.image = {};
    m.pages = 0;
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        putobject(&w);
        putcallimages(&w, &m, {});
        putend(&w);
        Str rec = finish(&b);
        expect(t, "callimages jpeg first", cut(rec, ']').tail,
               ",\"image\":\"out/c1.png\",\"width\":64,\"height\":32}");
    }
    m.images.len = 1;
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        putobject(&w);
        putcallimages(&w, &m, {});
        putend(&w);
        expect(t, "callimages jpeg only", finish(&b),
               "{\"images\":[{\"name\":\"out/c1-1.jpg\",\"mime\":\"image/jpeg\","
               "\"w\":10,\"h\":20}]}");
    }

    // Images none well formed: the keys of one image instead
    {
        JsonDoc d = jsonparse(&a, "{\"images\":[{\"name\":\"out/x.png\"},7],"
                                  "\"image\":\"out/y.png\",\"width\":3,\"height\":4}");
        Msg got = {};
        getcallimages(&a, d.root, &got);
        expect(t, "callimages fallback", concat(&a, got.image,
               istr(&a, got.imgw*1000 + got.imgh*10 + got.imgread + 100000*got.images.len)),
               "out/y.png3041");
    }
}

// toolend: a call's several images go into its record and its message,
// the first PNG also under the keys of one, then are forgotten, so that
// the next call's result has none of them.
static void testtoolendimages(Test *t)
{
    Arena   a = t->perm;
    Engine *e = alloc<Engine>(&a);
    e->os  = newos(&a, 1<<16);
    e->out = newmailbox(&a, 1<<16, 0, 0);
    Session *s = alloc<Session>(&a);
    s->e      = e;
    s->sid    = "20260101-000000-aaaa";
    s->log.os = e->os;
    s->log.fd = -1;  // no log: records go to the frontend alone
    s->perm   = carve(&a, 1<<16);
    Block calls[2] = {};
    calls[0].type = BLOCK_CALL;
    calls[0].id   = "c1";
    calls[0].name = "read";
    calls[0].args = "{}";
    calls[1]      = calls[0];
    calls[1].id   = "c2";
    s->calls = {calls, 2, 2};
    MsgImage imgs[2] = {};
    imgs[0].name = "out/c1-1.jpg";
    imgs[0].mime = "image/jpeg";
    imgs[0].w    = 5;
    imgs[0].h    = 6;
    imgs[1].name = "out/c1-2.png";
    imgs[1].mime = "image/png";
    imgs[1].w    = 7;
    imgs[1].h    = 8;
    s->callimages = {imgs, 2, 2};
    toolend(s, a, "error", "t", 0, 0, {}, 0);
    toolend(s, a, "error", "u", 0, 0, {}, 0);

    Buf  got(&a, 512);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        if (rec.ev != EV_TOOLEND) continue;
        print(&got, jsonstr(jsonget(rec.root, "call")));
        print(&got, " images=");
        print(&got, jsonget(rec.root, "images")->raw);
        print(&got, " image=");
        print(&got, jsonstr(jsonget(rec.root, "image")));
        putbyte(&got, '\n');
    }
    expect(t, "toolend images record", finish(&got),
           "c1 images=[{\"name\":\"out/c1-1.jpg\",\"mime\":\"image/jpeg\",\"w\":5,\"h\":6},"
           "{\"name\":\"out/c1-2.png\",\"mime\":\"image/png\",\"w\":7,\"h\":8}] "
           "image=out/c1-2.png\n"
           "c2 images= image=\n");
    expect(t, "toolend images history", istr(&a, s->history.len*1000 +
           s->history[0].images.len*100 + s->history[0].image.len*10 +
           s->history[1].images.len), "2200");
    expect(t, "toolend images reset", istr(&a, s->callimages.len), "0");
}

// The images of tool results in a request: a set counts as one under
// [desktop] images, left out whole with a text naming its call; of
// those shown, at most PDF_REQIMAGES images and PDF_REQIMGBYTES bytes
// of files, the newest first; each file read in if it is one of out/
// and what its mime says, else a text in its place.
static void testdeskkeepset(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = {};
    v.os      = newos(&a, 24<<20);  // room for files of 5 MiB
    v.scratch = carve(&a, 1<<22);
    os_write(v.os, a, "/r/h.ini", "[desktop]\nimages = 3\nimages_slack = 0\n");
    Engine *e = alloc<Engine>(&a);
    e->os  = v.os;
    e->cfg = newconfig(v.os, &a, v.scratch, "/r/h.ini", 0);
    e->ini = e->cfg->ini;
    Session *s = alloc<Session>(&a);
    s->e   = e;
    s->dir = "/s";
    os_mkdirs(v.os, a, "/s/out");
    Str png = "\x89PNG\r\n\x1a\nxx";
    Str jpg = "\xff\xd8\xff\xe0yy";
    os_write(v.os, a, "/s/out/p.png", png);
    os_write(v.os, a, "/s/out/j.jpg", jpg);

    // Sets of 30, 20 and 15 images, the newest last
    i32 counts[] = {30, 20, 15};
    for (i32 i = 0; i < 3; i++) {
        Slice<MsgImage> set = {};
        for (i32 k = 0; k < counts[i]; k++) {
            MsgImage g = {};
            g.name = k&1 ? Str("out/j.jpg") : Str("out/p.png");
            g.mime = k&1 ? Str("image/jpeg") : Str("image/png");
            g.w    = 750;
            g.h    = 1;
            set = push(&a, set, g);
        }
        Msg m = {};
        m.role   = ROLE_TOOL;
        m.callid = concat(&a, "c", istr(&a, i));
        m.images = set;
        s->history = push(&a, s->history, m);
    }
    {
        Arena tmp  = v.scratch;
        b32  *keep = deskkeepset(s, &tmp, 0, s->history.len);
        expect(t, "keepset count", concat(&a, concat(&a, istr(&a, keep[0]),
               istr(&a, keep[1])), istr(&a, keep[2])), "011");
        expect(t, "keepset tokens", istr(&a, deskimagetokens(s)), "70");

        tmp = v.scratch;
        Slice<Msg> h = deskimages(s, &tmp);
        expect(t, "keepset alt", h[0].imgalt,
               "[PDF images omitted: c0; read the file again to see them]");
        b32 ok = !h[0].images.len && h[1].images.len==20 && h[2].images.len==15;
        for (iz k = 0; ok && k < 15; k++) {
            ok = h[2].images[k].b64 == (k&1 ? Str("/9j/4Hl5") : Str("iVBORw0KGgp4eA=="));
        }
        expect(t, "keepset read in", istr(&a, ok && !h[1].imgalt.len), "1");
    }

    // By bytes: two files of 5 MiB are more than 8 MiB, so the newest
    // set goes; the next, of 20, stays, and the oldest still has no room
    {
        Buf big(&a, 5<<20);
        print(&big, png);
        while (big.len < 5<<20) putbyte(&big, 'z');
        os_write(v.os, a, "/s/out/big.png", finish(&big));
        Msg *m = &s->history[2];
        m->images.data[0].name = "out/big.png";
        m->images.data[2].name = "out/big.png";
        Arena tmp  = v.scratch;
        b32  *keep = deskkeepset(s, &tmp, 0, s->history.len);
        expect(t, "keepset bytes", concat(&a, concat(&a, istr(&a, keep[0]),
               istr(&a, keep[1])), istr(&a, keep[2])), "010");
        m->images.data[0].name = "out/p.png";
        m->images.data[2].name = "out/p.png";
    }

    // [desktop] images: of four items, the oldest leaves, a screenshot
    // counted alike
    {
        Msg shot = {};
        shot.role   = ROLE_TOOL;
        shot.callid = "c3";
        shot.image  = "out/p.png";
        shot.imgw   = 750;
        shot.imgh   = 2;
        s->history  = push(&a, s->history, shot);
        expect(t, "keepset drop", istr(&a, deskdrop(s).drop), "1");
        expect(t, "keepset drop tokens", istr(&a, deskimagetokens(s)), "73");
        Arena      tmp = v.scratch;
        Slice<Msg> h   = deskimages(s, &tmp);
        expect(t, "keepset dropped", concat(&a, h[0].imgalt, istr(&a, h[0].images.len)),
               "[PDF images omitted: c0; read the file again to see them]0");
        expect(t, "keepset shot", istr(&a, h[3].imgb64.len>0 && h[1].images.len==20), "1");
        s->history.len--;
    }

    // A name not of out/, or a file not what its mime says: lost
    {
        Msg *m = &s->history[2];
        m->images.data[0].name = "img/1.png";
        m->images.data[1].mime = "image/png";
        Arena      tmp = v.scratch;
        Slice<Msg> h   = deskimages(s, &tmp);
        expect(t, "keepset lost name", h[2].images[0].alt,
               "[image lost: img/1.png, its file could not be read]");
        expect(t, "keepset lost mime", h[2].images[1].alt,
               "[image lost: out/j.jpg, its file could not be read]");
        expect(t, "keepset others", istr(&a, h[2].images[2].b64.len>0), "1");
    }

    // Attached images are the user's alone (attachtokens)
    expect(t, "attachtokens tool", istr(&a, attachtokens(s)), "0");
}

// A session for tests of the images in requests, its files under /s,
// with [desktop] images = keep and no slack.
static Session *desksession(Arena *a, i32 keep)
{
    CfgEnv v = {};
    v.os      = newos(a, 24<<20);
    v.scratch = carve(a, 1<<22);
    Str ini = concat(a, concat(a, "[desktop]\nimages = ", istr(a, keep)),
                     "\nimages_slack = 0\n");
    os_write(v.os, *a, "/r/h.ini", ini);
    Engine *e = alloc<Engine>(a);
    e->os  = v.os;
    e->cfg = newconfig(v.os, a, v.scratch, "/r/h.ini", 0);
    e->ini = e->cfg->ini;
    Session *s = alloc<Session>(a);
    s->e   = e;
    s->dir = "/s";
    os_mkdirs(v.os, *a, "/s/out");
    os_mkdirs(v.os, *a, "/s/img");
    os_write(v.os, *a, "/s/out/p.png", "\x89PNG\r\n\x1a\nxx");
    os_write(v.os, *a, "/s/img/1.png", "\x89PNG\r\n\x1a\nxx");
    return s;
}

// A tool result of n images, each of out/p.png at 750x1, appended.
static Msg *deskset(Arena *a, Session *s, Str call, i32 n)
{
    Msg m = {};
    m.role   = ROLE_TOOL;
    m.callid = call;
    for (i32 k = 0; k < n; k++) {
        MsgImage g = {};
        g.name = "out/p.png";
        g.mime = "image/png";
        g.w    = 750;
        g.h    = 1;
        m.images = push(a, m.images, g);
    }
    s->history = push(a, s->history, m);
    return &s->history[s->history.len-1];
}

static Str keepflags(Arena *a, Session *s)
{
    Arena t    = *a;
    b32  *keep = deskkeepset(s, &t, 0, s->history.len);
    Buf   b(a, 64);
    for (iz i = 0; i < s->history.len; i++) {
        putbyte(&b, s->history[i].role==ROLE_TOOL ? (u8)('0'+keep[i]) : '-');
    }
    return finish(&b);
}

// deskkeepset's limits, inclusive; that a set only ever leaves as newer
// ones arrive, where FLOOR_READ marks the prefix's change; names not of
// out/ weigh nothing; a model with no vision gets notes, not images; a
// user's attachments are never an item of [desktop] images.
static void testdeskkeep2(Test *t)
{
    Arena a = t->perm;

    // Exactly 40 images, and exactly 8 MiB
    {
        Session *s = desksession(&a, 3);
        deskset(&a, s, "c0", 25);
        deskset(&a, s, "c1", 15);
        expect(t, "keep2 forty", keepflags(&a, s), "11");
        expect(t, "keep2 forty tokens", istr(&a, deskimagetokens(s)), "80");
        Buf big(&a, 4<<20);
        print(&big, "\x89PNG\r\n\x1a\n");
        while (big.len < 4<<20) putbyte(&big, 'z');
        Str bigpng = finish(&big);
        os_write(s->e->os, a, "/s/out/big.png", bigpng);
        os_write(s->e->os, a, "/s/img/big.png", bigpng);
        s->history[0].images.len = 1;
        s->history[1].images.len = 1;
        s->history[0].images.data[0].name = "out/big.png";
        s->history[1].images.data[0].name = "out/big.png";
        expect(t, "keep2 8 MiB", keepflags(&a, s), "11");

        // A name not of out/ is never read, so it weighs nothing: here
        // 4 MiB more would leave no room for the older set
        deskset(&a, s, "c2", 1)->images.data[0].name = "img/big.png";
        expect(t, "keep2 not out", keepflags(&a, s), "111");
        Arena      tmp = a;
        Slice<Msg> h   = deskimages(s, &tmp);
        expect(t, "keep2 not out lost", h[2].images[0].alt,
               "[image lost: img/big.png, its file could not be read]");
        s->history[2].images.data[0].name = "img/1.png";  // a real PNG
        tmp = a;
        h   = deskimages(s, &tmp);
        expect(t, "keep2 img lost", h[2].images[0].alt,
               "[image lost: img/1.png, its file could not be read]");
    }

    // Sets of 10 and 35, then 10 more: the oldest, out for room, stays
    // out, and where the 35 leaves the prefix changes (FLOOR_READ)
    {
        Session *s = desksession(&a, 3);
        deskset(&a, s, "a", 10);
        deskset(&a, s, "b", 35);
        Msg reply = {};
        reply.role = ROLE_ASSISTANT;
        reply.text = "ok";
        s->history = push(&a, s->history, reply);
        expect(t, "keep2 before", keepflags(&a, s), "01-");
        deskset(&a, s, "c", 10);
        expect(t, "keep2 after", keepflags(&a, s), "00-1");
        expect(t, "keep2 after tokens", istr(&a, deskimagetokens(s)), "20");
        Arena      tmp = a;
        Slice<Msg> h   = deskimages(s, &tmp);
        Buf        b(&a, 64);
        for (iz i = 0; i < h.len; i++) {
            print(&b, (i64)h[i].floor);
        }
        expect(t, "keep2 floor", finish(&b), "0200");
        expect(t, "keep2 omitted", concat(&a, h[0].imgalt, h[1].imgalt),
               "[PDF images omitted: a; read the file again to see them]"
               "[PDF images omitted: b; read the file again to see them]");
    }

    // Sets of 4 MiB each: the two newest fill 8 MiB, which ends sets,
    // and the files are weighed by paths that survive os_stat's scratch
    // (deskfilesize: the path allocated before its arena is copied)
    {
        Session *s = desksession(&a, 3);
        Buf big(&a, 4<<20);
        print(&big, "\x89PNG\r\n\x1a\n");
        while (big.len < 4<<20) putbyte(&big, 'z');
        os_write(s->e->os, a, "/s/out/big.png", finish(&big));
        for (i32 i = 0; i < 3; i++) {
            deskset(&a, s, "b", 1)->images.data[0].name = "out/big.png";
        }
        expect(t, "keep2 bytes over", keepflags(&a, s), "011");
        expect(t, "keep2 bytes over tokens", istr(&a, deskimagetokens(s)), "4");

        // A set too big even alone is passed over; older ones still go
        deskset(&a, s, "huge", 3);
        for (i32 k = 0; k < 3; k++) {
            s->history[3].images.data[k].name = "out/big.png";
        }
        expect(t, "keep2 bytes alone", keepflags(&a, s), "0110");
    }

    // A single image read sent as what its bytes are, not always a PNG
    {
        Session *s = desksession(&a, 3);
        os_write(s->e->os, a, "/s/out/j.jpg", "\xff\xd8\xff\xe0xx");
        Msg m = {};
        m.role    = ROLE_TOOL;
        m.callid  = "j";
        m.image   = "out/j.jpg";
        m.imgw    = 750;
        m.imgh    = 1;
        m.imgread = 1;
        s->history = push(&a, s->history, m);
        m.callid   = "p";
        m.image    = "out/p.png";
        s->history = push(&a, s->history, m);
        Arena      tmp = a;
        Slice<Msg> h   = deskimages(s, &tmp);
        expect(t, "keep2 single mime", concat(&a, concat(&a, h[0].imgmime, " "),
               h[1].imgmime), "image/jpeg image/png");
    }

    // A PDF kept by its size in a request
    {
        Session *s = desksession(&a, 3);
        os_write(s->e->os, a, "/s/out/d.pdf", "%PDF-1.4\n");
        Msg m = {};
        m.role   = ROLE_TOOL;
        m.callid = "d";
        m.image  = "out/d.pdf";
        m.pages  = 2;
        s->history = push(&a, s->history, m);
        deskset(&a, s, "e", 2);
        expect(t, "keep2 pdf", keepflags(&a, s), "11");
    }

    // No vision: a note in place of each image, and no tokens for them
    {
        Session *s = desksession(&a, 3);
        Msg user = {};
        user.role = ROLE_USER;
        user.text = "look";
        MsgImage att = {};
        att.name = "img/1.png";
        att.mime = "image/png";
        att.w    = 750;
        att.h    = 1;
        user.images = push(&a, user.images, att);
        s->history  = push(&a, s->history, user);
        deskset(&a, s, "c0", 2);
        Msg shot = {};
        shot.role   = ROLE_TOOL;
        shot.callid = "c1";
        shot.image  = "out/p.png";
        shot.imgw   = 750;
        shot.imgh   = 1;
        s->history  = push(&a, s->history, shot);
        s->mc.novision = 1;
        expect(t, "keep2 blind tokens", istr(&a, deskimagetokens(s)+attachtokens(s)), "0");
        Arena      tmp = a;
        Slice<Msg> h   = deskimages(s, &tmp);
        expect(t, "keep2 blind attach", concat(&a, h[0].images[0].alt,
               istr(&a, h[0].images[0].b64.len)),
               "[image omitted: img/1.png, since this model takes no images]0");
        expect(t, "keep2 blind set", concat(&a, h[1].imgalt, istr(&a, h[1].images.len)),
               "[PDF images omitted: c0, since this model takes no images]0");
        expect(t, "keep2 blind shot", concat(&a, h[2].imgalt, istr(&a, h[2].imgb64.len)),
               "[screenshot omitted: c1, since this model takes no images]0");

        // With vision, and [desktop] images = 1 leaving out the set: the
        // attachments still go, counted once
        s->mc.novision = 0;
        Session *one = desksession(&a, 1);
        one->history = s->history;
        expect(t, "keep2 attach drop", istr(&a, deskdrop(one).drop), "1");
        expect(t, "keep2 attach tokens", istr(&a, deskimagetokens(one)), "2");
        tmp = a;
        h   = deskimages(one, &tmp);
        expect(t, "keep2 attach kept", concat(&a, h[0].images[0].b64,
               istr(&a, h[2].imgb64.len>0)), "iVBORw0KGgp4eA==1");
    }
}

// What a subagent spent, as its parent reads it after the report
// (issue #40): its context's peak against the window, its compactions,
// its requests; and the line it goes in.
static void testtaskspent(Test *t)
{
    Arena    a = t->perm;
    Session *s = alloc<Session>(&a);
    s->mc.ctx = 131072;
    s->tpeak  = 118000;
    s->treqs  = 41;
    expect(t, "spent", taskspent(&a, s),
           "Its context peaked at 118k of 131k tokens (90%), over 41 requests.");
    s->tsumms = 1;
    expect(t, "spent once", taskspent(&a, s),
           "Its context peaked at 118k of 131k tokens (90%), compacted once, "
           "over 41 requests.");
    s->tsumms = 2;
    expect(t, "spent twice", taskspent(&a, s),
           "Its context peaked at 118k of 131k tokens (90%), compacted twice, "
           "over 41 requests.");
    s->tsumms = 3;
    s->mc.ctx = 0;
    s->tpeak  = 8540;
    s->treqs  = 1;
    expect(t, "spent unknown", taskspent(&a, s),
           "Its context peaked at 8.5k tokens, its window unknown, compacted "
           "3 times, over 1 request.");
    s->mc.ctx  = 0;
    s->ctxhint = 32768;
    s->tsumms  = 0;
    expect(t, "spent hint", taskspent(&a, s),
           "Its context peaked at 8.5k of 33k tokens (26%), over 1 request.");
    expect(t, "line", spentline(&a, 372000, "Its context peaked at 8.5k."),
           "[The subagent worked for 6 min 12 s. Its context peaked at 8.5k.]");
    expect(t, "line old", spentline(&a, 1500, {}), "[The subagent worked for 1.5 s.]");
    expect(t, "line untimed", spentline(&a, 0, "Its context peaked at 8.5k."),
           "[Its context peaked at 8.5k.]");
}

static void test_engine(Test *t)
{
    testtaskspent(t);
    testcallimages(t);
    testdeskkeepset(t);
    testdeskkeep2(t);
    testtoolendimages(t);
    testprefill(t);
    testremotetask(t);
    testtilde(t);
    testroles(t);
    testlist(t);
    testsidecars(t);
    testdelete(t);
    testtrash(t);
    testtargets(t);
    Arena *a = &t->perm;

    // A command that only sleeps, a wait for a job (#32)
    {
        struct { Str cmd; b32 want; } cases[] = {
            {"sleep 30", 1},
            {"  sleep 1.5;", 1},
            {"sleep 2m", 1},
            {"sleep 1 1", 1},
            {"Start-Sleep -Seconds 20", 1},
            {"start-sleep 5", 1},
            {"sleep", 0},
            {"sleep 5; tail log", 0},
            {"sleep 5 && curl localhost:8080", 0},
            {"sleep $N", 0},
            {"sleeper 5", 0},
            {"Start-Sleep -Verbose 5", 0},
            {"echo sleep 5", 0},
        };
        for (iz i = 0; i < countof(cases); i++) {
            expect(t, concat(a, "onlysleeps ", cases[i].cmd),
                   istr(a, onlysleeps(cases[i].cmd)), istr(a, cases[i].want));
        }
    }

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
            "{\"cmd\":\"term.open\",\"id\":18,\"sid\":\"x\"}",
            "{\"cmd\":\"term.ack\",\"tid\":1,\"off\":0}",
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
               "18-the engine is stopping\n"
               "8-command too long\n");
        Buf want(a, 1024);
        print(&want, "{\"ts\":0,\"ev\":\"hello\",\"proto\":0,\"version\":\""
                     PROSPERO_VERSION "\",");
        JsonOut w = {&want};
        w.depth   = 1;  // inside the record, as evbegin leaves it
        w.objects = 1;
        w.comma   = 0;
        putcaps(&w, 0);
        print(&want, ",\"pty\":");
        print(&want, (i64)0);  // the test platform has none
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
