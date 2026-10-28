// Worktree sessions: names, branch prefixes, and where a worktree
// session can start from a folder (worktree.cpp)

static Str wtplacestr(Os *os, Arena *a, Str cwd)
{
    WtPlace p = wtplace(os, a, cwd, 0);
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

static void test_worktree(Test *t)
{
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
        Os *os = newos(a, 1<<20);
        // A main checkout on main, with a subdirectory
        os_mkdirs(os, *a, "/r");
        os_mkdirs(os, *a, "/r/.git");
        os_mkdirs(os, *a, "/r/.git/refs");
        os_mkdirs(os, *a, "/r/.git/refs/heads");
        os_mkdirs(os, *a, "/r/src");
        os_mkdirs(os, *a, "/r/src/deep");
        os_write(os, *a, "/r/.git/HEAD", "ref: refs/heads/main\n");
        os_write(os, *a, "/r/.git/refs/heads/main", concat(a, x, "\n"));
        // Its worktree, detached
        os_mkdirs(os, *a, "/r/.git/worktrees/w");
        os_mkdirs(os, *a, "/w/sub");
        os_write(os, *a, "/r/.git/worktrees/w/HEAD", concat(a, y, "\n"));
        os_write(os, *a, "/r/.git/worktrees/w/commondir", "../..\n");
        os_write(os, *a, "/w/.git", "gitdir: /r/.git/worktrees/w\n");
        // A submodule, with no commondir
        os_mkdirs(os, *a, "/r/.git/modules/m");
        os_mkdirs(os, *a, "/r/m");
        os_write(os, *a, "/r/.git/modules/m/HEAD", concat(a, x, "\n"));
        os_write(os, *a, "/r/m/.git", "gitdir: ../.git/modules/m\n");
        // An unborn branch, and a rebase under way
        os_mkdirs(os, *a, "/u/.git");
        os_write(os, *a, "/u/.git/HEAD", "ref: refs/heads/main\n");
        os_mkdirs(os, *a, "/b/.git");
        os_mkdirs(os, *a, "/b/.git/rebase-merge");
        os_write(os, *a, "/b/.git/HEAD", concat(a, x, "\n"));
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
