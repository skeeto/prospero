// Worktree sessions: names, branch prefixes, and where a worktree
// session can start from a folder (worktree.cpp)

static Str wtplacestr(Os *os, Arena *a, Str cwd)
{
    Arena   keep = carve(a, 1<<16);  // as small as the engine's
    WtPlace p    = wtplace(os, &keep, *a, cwd, 0);
    if (p.err.len) {
        return concat(a, "err: ", p.err);
    }
    Buf b(a, 512);
    print(&b, p.main);
    print(&b, " sub=");
    print(&b, p.sub);
    print(&b, " common=");
    print(&b, p.common);
    print(&b, " base=");
    print(&b, p.base);
    print(&b, " oid=");
    print(&b, takehead(p.oid, 4));
    return finish(&b);
}

static void testlonghead(Test *);

static void test_worktree(Test *t)
{
    testlonghead(t);

    Arena  mem = t->perm;  // let go as it returns
    Arena *a   = &mem;

    {
        static Str const good[] = {
            "brisk-otter", "a", "x1", "feature-2", "com", "com10", "lpt",
            "abcdefghijklmnopqrstuvwx",
        };
        static Str const bad[] = {
            "", "-x", "x-", "a--b", "A", "a_b", "a.b", "../x", "a/b", "con",
            "nul", "com1", "lpt9", "abcdefghijklmnopqrstuvwxy", "é",
        };
        for (iz i = 0; i < countof(good); i++) {
            expect(t, concat(a, "wtname good ", good[i]), istr(a, wtname(good[i])), "1");
        }
        for (iz i = 0; i < countof(bad); i++) {
            expect(t, concat(a, "wtname bad ", bad[i]), istr(a, wtname(bad[i])), "0");
        }
        Str r = wtrandom(a, 0x0000000500000003);
        expect(t, "wtrandom", r, "calm-ferret");
        expect(t, "wtrandom valid", istr(a, wtname(wtrandom(a, ~(u64)0))), "1");
    }

    {
        // A subagent's worktree, named after its task's description
        struct { Str desc; iz max; Str want; } slugs[] = {
            {"Fix the parser",                          24, "fix-the-parser"},
            {"  Fix: the PARSER!! ",                    24, "fix-the-parser"},
            {"\xc3\x9c" "berarbeite die Doku",          24, "berarbeite-die-doku"},
            {"v2 of x_y",                               24, "v2-of-x-y"},
            {"Refactor the session list scanning code", 24, "refactor-the-session"},
            {"Refactor the session list",               22, "refactor-the-session"},
            {"Refactor the sessions",                   24, "refactor-the-sessions"},
            {"abcdefghijklmnopqrstuvwx yz",             24, "abcdefghijklmnopqrstuvwx"},
            {"abcdefghijklmnopqrstuvwxyz",              24, "abcdefghijklmnopqrstuvwx"},
            {"!!!",                                     24, ""},
            {"",                                        24, ""},
            {"con",                                     24, "con"},
        };
        for (iz i = 0; i < countof(slugs); i++) {
            expect(t, concat(a, "wtslug ", slugs[i].desc),
                   wtslug(a, slugs[i].desc, slugs[i].max), slugs[i].want);
        }

        // A parent's grant, as its subagent's worktree k has it: from
        // the parent's checkout or the main one, else as it is
        struct { Str path; Str from; Str want; b32 win; } maps[] = {
            {"/r/src/x.c",                       "/r", "/r/.prospero/worktrees/k/src/x.c", 0},
            {"/r",                               "/r", "/r/.prospero/worktrees/k", 0},
            {"/r/src/x.c",                       "",   "/r/.prospero/worktrees/k/src/x.c", 0},
            {"/r/.prospero/worktrees/p/src/x.c", "/r/.prospero/worktrees/p",
             "/r/.prospero/worktrees/k/src/x.c", 0},
            {"/r/src/x.c",                       "/r/.prospero/worktrees/p",
             "/r/.prospero/worktrees/k/src/x.c", 0},
            {"/r/.prospero/worktrees/o/x.c",     "/r", "/r/.prospero/worktrees/o/x.c", 0},
            {"/r/.prospero/worktrees/o/x.c",     "/r/.prospero/worktrees/p",
             "/r/.prospero/worktrees/o/x.c", 0},
            {"/rx/y",                            "/r", "/rx/y", 0},
            {"/home/u/notes",                    "/r", "/home/u/notes", 0},
            {"c:\\R\\src\\x.c",                  "C:\\r",
             "C:\\r\\.prospero\\worktrees\\k\\src\\x.c", 1},
            {"C:/r/src/x.c",                     "C:\\r", "C:/r/.prospero/worktrees/k/src/x.c", 1},
            {"C:\\r\\.prospero\\worktrees\\o\\x", "C:\\r", "C:\\r\\.prospero\\worktrees\\o\\x", 1},
        };
        for (iz i = 0; i < countof(maps); i++) {
            Str main = maps[i].win ? Str("C:\\r") : Str("/r");
            Str to   = maps[i].win ? Str("C:\\r\\.prospero\\worktrees\\k")
                                   : Str("/r/.prospero/worktrees/k");
            expect(t, concat(a, "wtinto ", maps[i].path),
                   wtinto(a, maps[i].path, maps[i].from, main, to, maps[i].win), maps[i].want);
        }
    }

    {
        static Str const good[] = {"prospero/", "wt-", "a/b/", "me.x/", "x"};
        static Str const bad[] = {
            "", "/x", "-x", "x.", "a//b", "a..b", "a/.b", ".x", "x.lock/",
            "a b", "a~", "a^", "a:", "a?", "a*", "a[", "a\\", "a@{",
        };
        for (iz i = 0; i < countof(good); i++) {
            expect(t, concat(a, "prefix good ", good[i]), istr(a, cfgrefprefix(good[i])), "1");
        }
        for (iz i = 0; i < countof(bad); i++) {
            expect(t, concat(a, "prefix bad ", bad[i]), istr(a, cfgrefprefix(bad[i])), "0");
        }
    }

    {
        Str x = "1111111111111111111111111111111111111111";
        Str y = "2222222222222222222222222222222222222222";
        Os *os = newos(a, 1<<21);
        Str xl = concat(a, x, "\n");
        Str yl = concat(a, y, "\n");
        // A main checkout on main, with a subdirectory
        os_mkdirs(os, *a, "/r");
        os_mkdirs(os, *a, "/r/.git");
        os_mkdirs(os, *a, "/r/.git/refs");
        os_mkdirs(os, *a, "/r/.git/refs/heads");
        os_mkdirs(os, *a, "/r/src");
        os_mkdirs(os, *a, "/r/src/deep");
        os_write(os, *a, "/r/.git/HEAD", "ref: refs/heads/main\n");
        os_write(os, *a, "/r/.git/refs/heads/main", xl);
        // Its worktree, detached
        os_mkdirs(os, *a, "/r/.git/worktrees/w");
        os_mkdirs(os, *a, "/w/sub");
        os_write(os, *a, "/r/.git/worktrees/w/HEAD", yl);
        os_write(os, *a, "/r/.git/worktrees/w/commondir", "../..\n");
        os_write(os, *a, "/w/.git", "gitdir: /r/.git/worktrees/w\n");
        // A submodule, with no commondir
        os_mkdirs(os, *a, "/r/.git/modules/m");
        os_mkdirs(os, *a, "/r/m");
        os_write(os, *a, "/r/.git/modules/m/HEAD", xl);
        os_write(os, *a, "/r/m/.git", "gitdir: ../.git/modules/m\n");
        // An unborn branch, and a rebase under way
        os_mkdirs(os, *a, "/u/.git");
        os_write(os, *a, "/u/.git/HEAD", "ref: refs/heads/main\n");
        os_mkdirs(os, *a, "/b/.git");
        os_mkdirs(os, *a, "/b/.git/rebase-merge");
        os_write(os, *a, "/b/.git/HEAD", xl);
        os_mkdirs(os, *a, "/n");

        expect(t, "place root", wtplacestr(os, a, "/r"),
               "/r sub= common=/r/.git base=main oid=1111");
        expect(t, "place sub", wtplacestr(os, a, "/r/src/deep"),
               "/r sub=src/deep common=/r/.git base=main oid=1111");
        expect(t, "place worktree", wtplacestr(os, a, "/w/sub"),
               "/r sub=sub common=/r/.git base= oid=2222");
        expect(t, "place submodule", wtplacestr(os, a, "/r/m"),
               "err: the folder is in a submodule, or a worktree of a bare "
               "repository: open its own repository instead");
        expect(t, "place unborn", wtplacestr(os, a, "/u"),
               "err: the branch main has no commits yet");
        expect(t, "place rebase", wtplacestr(os, a, "/b"),
               "err: a rebase is under way in the folder");
        expect(t, "place none", wtplacestr(os, a, "/n"),
               "err: the folder is not in a git repository");

        // A branch only packed, in a packed-refs larger than the arena
        // the place is made in, as a large repository's is
        Buf pk(a, 1<<18);
        print(&pk, "# pack-refs with: peeled fully-peeled sorted\n");
        for (i32 i = 0; pk.len < 1<<17; i++) {
            print(&pk, y);
            print(&pk, " refs/tags/t");
            print(&pk, (i64)i);
            print(&pk, "\n");
        }
        print(&pk, x);
        print(&pk, " refs/heads/packed\n");
        os_mkdirs(os, *a, "/p/.git");
        os_mkdirs(os, *a, "/p/.git/refs/heads");
        os_write(os, *a, "/p/.git/HEAD", "ref: refs/heads/packed\n");
        os_write(os, *a, "/p/.git/packed-refs", finish(&pk));
        expect(t, "place packed large", wtplacestr(os, a, "/p"),
               "/p sub= common=/p/.git base=packed oid=1111");
        expect(t, "wtpath", wtpath(a, "/r", "brisk-otter", 0),
               "/r/.prospero/worktrees/brisk-otter");
        expect(t, "wtpath win", wtpath(a, "C:\\r", "x", 1), "C:\\r\\.prospero\\worktrees\\x");

        // A managed worktree of /r, and a directory there git knows not
        os_mkdirs(os, *a, "/r/.prospero");
        os_mkdirs(os, *a, "/r/.prospero/worktrees");
        os_mkdirs(os, *a, "/r/.prospero/worktrees/ok");
        os_mkdirs(os, *a, "/r/.prospero/worktrees/ok/sub");
        os_mkdirs(os, *a, "/r/.prospero/worktrees/stray");
        os_mkdirs(os, *a, "/r/.git/worktrees/ok");
        os_write(os, *a, "/r/.git/worktrees/ok/commondir", "../..\n");
        os_write(os, *a, "/r/.prospero/worktrees/ok/.git", "gitdir: /r/.git/worktrees/ok\n");
        struct { Str path; Str want; } mains[] = {
            {"/r/.prospero/worktrees/ok/sub/.mcp.json", "/r/sub/.mcp.json"},
            {"/r/.prospero/worktrees/ok/.mcp.json",     "/r/.mcp.json"},
            {"/r/.prospero/worktrees/ok",               "/r"},
            {"/r/.prospero/worktrees/stray/.mcp.json",  "/r/.prospero/worktrees/stray/.mcp.json"},
            {"/r/.prospero/worktrees/../x/.mcp.json",   "/r/.prospero/worktrees/../x/.mcp.json"},
            {"/r/sub/.mcp.json",                        "/r/sub/.mcp.json"},
            {"/w/sub/.mcp.json",                        "/w/sub/.mcp.json"},
        };
        for (iz i = 0; i < countof(mains); i++) {
            expect(t, concat(a, "wtmainof ", mains[i].path),
                   wtmainof(os, a, mains[i].path, 0), mains[i].want);
        }

        // A main checkout's session asks before going into its
        // worktrees, but a grant there holds, as does the worktree's own
        // session in its tree
        Guard g = newguard(os, a, "/r", "/home/u", {}, "", 0);
        guardhole(os, a, &g, "/r/.prospero/worktrees");
        guardadd(os, a, &g, "/r/.prospero/worktrees/ok/sub", ROOT_HIDDEN|ROOT_MISSING);
        Guard o = newguard(os, a, "/r/.prospero/worktrees/ok", "/home/u", {}, "", 0);
        guardhole(os, a, &o, "/r/.prospero/worktrees/ok/.prospero/worktrees");
        struct { Guard *g; Str path; Str want; } holes[] = {
            {&g, "src/x.c",                             "/r/src/x.c I"},
            {&g, ".prospero/worktrees/ok/x.c",          "/r/.prospero/worktrees/ok/x.c O"},
            {&g, ".prospero/worktrees",                 "/r/.prospero/worktrees O"},
            {&g, ".prospero/x",                         "/r/.prospero/x I"},
            {&g, ".prospero/worktrees/ok/sub/x.c",      "/r/.prospero/worktrees/ok/sub/x.c I"},
            {&o, "x.c",                                 "/r/.prospero/worktrees/ok/x.c I"},
            {&o, "sub/x.c",                             "/r/.prospero/worktrees/ok/sub/x.c I"},
            {&o, "/r/src/x.c",                          "/r/src/x.c O"},
        };
        for (iz i = 0; i < countof(holes); i++) {
            GuardPath gp = guardpath(os, a, holes[i].g, holes[i].path, 0);
            Str got = concat(a, gp.path, gp.inside ? Str(" I") : gp.diverged ? Str(" D") : Str(" O"));
            expect(t, concat(a, "hole ", holes[i].path), got, holes[i].want);
        }
    }
}

// A session record longer than a first read: a subagent whose role has
// a long prompt, kept in its record, in a worktree of its own. Whatever
// reads the record back (the sessions list, the worktree names taken,
// the sessions recording a worktree, the parent walks of the Trash and
// of terminals) reads it whole, at each size it grows through; one past
// LOG_HEADMAX reads as none, and a lack of room fails, not overflows.
static void testlonghead(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    v.os->mem = carve(&a, 16<<20);  // room for the longest record
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->statedir = "/s";
    MadeSession made[] = {{"20260101-000000-aaaa", {}}};
    makesessions(v.os, &a, {made, countof(made)});

    Str    oid     = "0123456789abcdef0123456789abcdef01234567";
    iz     sizes[] = {LOG_HEADFIRST, 5000, 40000, 300000, LOG_HEADMAX + 1000};
    Str    names[] = {"calm-ferret", "brisk-otter", "slow-heron", "pale-moth", "too-long"};
    for (iz i = 0; i < countof(sizes); i++) {
        Buf b(&a, 1024 + sizes[i]);
        Str sid = concat(&a, "20260101-00000", concat(&a, istr(&a, i+1), "-bbbb"));
        print(&b, "{\"sid\":\"");
        print(&b, sid);
        print(&b, "\",\"seq\":1,\"ts\":0,\"ev\":\"session\",\"cwd\":\"/w\",\"role\":\"r\","
                  "\"model\":\"m\",\"parent\":\"20260101-000000-aaaa\",\"call\":\"c\","
                  "\"depth\":1,\"prompt\":\"");
        while (b.len < sizes[i]-200) {
            print(&b, "Read the code\\n and \\\"report\\\". ");
        }
        print(&b, "\",\"worktree\":{\"name\":\"");
        print(&b, names[i]);
        print(&b, "\",\"main\":\"/w\",\"branch\":\"p/x\",\"oid\":\"");
        print(&b, oid);
        print(&b, "\"}}\n{\"sid\":\"x\"}\n");
        Str dir = concat(&a, "/s/sessions/", sid);
        os_mkdirs(v.os, a, dir);
        Str log = concat(&a, dir, "/events.jsonl");
        os_write(v.os, a, log, finish(&b));
    }
    os_write(v.os, a, "/s/sessions/20260101-000000-aaaa/trashed", "1");

    Arena    tmp = carve(&a, 64<<20);
    SessScan sc  = scansessions(e, &tmp);
    Buf      got(&tmp, 1024);
    for (iz i = 0; i < sc.list.len; i++) {
        print(&got, cuthead(sc.list[i].sid, 13));
        if (sc.list[i].parent.len) {
            putbyte(&got, '^');
            print(&got, cuthead(sc.list[i].parent, 16));
        }
        if (sc.list[i].wt.name.len) {
            putbyte(&got, '@');
            print(&got, sc.list[i].wt.name);
        }
        putbyte(&got, ' ');
    }
    expect(t, "long records listed", finish(&got),
           "00-aaaa 01-bbbb^aaaa@calm-ferret 02-bbbb^aaaa@brisk-otter "
           "03-bbbb^aaaa@slow-heron 04-bbbb^aaaa@pale-moth ");

    Slice<WtName> recs = wtrecords(e, &tmp);
    Buf           wts(&tmp, 256);
    for (iz i = 0; i < recs.len; i++) {
        print(&wts, recs[i].name);
        putbyte(&wts, ' ');
    }
    expect(t, "long records' worktrees", finish(&wts),
           "calm-ferret brisk-otter slow-heron pale-moth ");
    for (iz i = 0; i < countof(names); i++) {
        b32 want = i < countof(names)-1;
        expect(t, concat(&tmp, "wtrecorded ", names[i]),
               istr(&tmp, wtrecorded(e, tmp, "/w", names[i], 0)), istr(&tmp, want));
        Str path = wtpath(&tmp, "/w", names[i], 0);
        expect(t, concat(&tmp, "wtrecorders ", names[i]),
               istr(&tmp, wtrecorders(e, &tmp, path).len), istr(&tmp, want));
        Str sid = concat(&tmp, "20260101-00000", concat(&tmp, istr(&tmp, i+1), "-bbbb"));
        expect(t, concat(&tmp, "intrash ", names[i]), istr(&tmp, intrash(e, tmp, sid)),
               istr(&tmp, want));
        TermNode n = termlookup(e, &tmp, sid);
        expect(t, concat(&tmp, "termlookup ", names[i]),
               concat(&tmp, n.parent, istr(&tmp, n.worktree)),
               want ? Str("20260101-000000-aaaa1") : Str("0"));
    }

    // Too little room: no record, rather than an overflow (the test
    // platform reads a whole file whatever the bound, hence 48 KiB)
    Arena small = carve(&tmp, 48<<10);
    Cut   c     = loghead(v.os, &small, "/s/sessions/20260101-000003-bbbb/events.jsonl");
    expect(t, "loghead without room", istr(&tmp, c.ok*10 + (c.head.len>0)), "0");
}
