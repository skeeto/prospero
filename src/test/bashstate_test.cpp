// bash (builtin)'s portable part (bashstate.cpp): its command line, the
// state file one run leaves for the next, and the carried exports, plus
// the argv shellcmd builds for it. No library: the hosts are tested by
// the scenarios.

static Str bashjoin(Arena *a, Slice<Str> s)
{
    Buf b(a, 256);
    for (iz i = 0; i < s.len; i++) {
        print(&b, i ? Str("|") : Str{});
        print(&b, s[i]);
    }
    return finish(&b);
}

static void test_bashstate(Test *t)
{
    Arena *a = &t->perm;

    // Arguments
    {
        Str args[] = {"prospero", "--bash", "--root", "C:/git/usr/bin",
                      "--carry", "in.state", "--state", "out.state", "-c", "ls"};
        BashArgs r = bashargs(args, countof(args), 1);
        expect(t, "args ok", istr(a, r.ok), "1");
        expect(t, "args root", r.root, "C:/git/usr/bin");
        expect(t, "args carry", r.carry, "in.state");
        expect(t, "args state", r.state, "out.state");
        expect(t, "args rest", istr(a, r.rest), "8");

        Str bare[] = {"prospero", "--bash", "-c", "ls"};
        r = bashargs(bare, countof(bare), 1);
        expect(t, "args bare", istr(a, r.ok && r.rest==2 && !r.root.len), "1");

        Str cut[] = {"prospero", "--bash", "--state"};
        r = bashargs(cut, countof(cut), 1);
        expect(t, "args cut short", istr(a, r.ok), "0");

        Str term[] = {"prospero", "--bash", "--term", "-l"};
        r = bashargs(term, countof(term), 1);
        expect(t, "args term", istr(a, r.ok && r.term && r.rest==3), "1");

        Str termlate[] = {"prospero", "--bash", "-l", "--term"};
        r = bashargs(termlate, countof(termlate), 1);
        expect(t, "args term is bash's after", istr(a, r.ok && !r.term && r.rest==2), "1");

        Str termcarry[] = {"prospero", "--bash", "--term", "--carry", "in.state"};
        r = bashargs(termcarry, countof(termcarry), 1);
        expect(t, "args term carry", istr(a, r.ok), "0");
        expect(t, "args term carry err", r.err, "--term takes no --root, --carry or --state");

        Str statelate[] = {"prospero", "--bash", "--state", "o", "--term"};
        r = bashargs(statelate, countof(statelate), 1);
        expect(t, "args state term", istr(a, r.ok), "0");
    }

    // Round trips
    {
        BashState s = {};
        s.pwd     = "/home/u/proj/sub";
        s.cwd     = "/real/proj/sub";
        s.exports = push(a, s.exports, Str("X=1"));
        s.exports = push(a, s.exports, Str("GONE"));
        s.exports = push(a, s.exports, Str("EMPTY="));
        s.known   = 1;
        s.changed = 1;
        Str       data = bashstateencode(a, &s);
        BashState r    = bashstatedecode(a, data);
        expect(t, "state ok", istr(a, r.ok && r.known && r.changed), "1");
        expect(t, "state pwd", r.pwd, "/home/u/proj/sub");
        expect(t, "state cwd", r.cwd, "/real/proj/sub");
        expect(t, "state exports", bashjoin(a, r.exports), "X=1|GONE|EMPTY=");

        // Every shorter prefix reads as none
        b32 any = 0;
        for (iz n = 0; n < data.len; n++) {
            any |= bashstatedecode(a, takehead(data, n)).ok;
        }
        expect(t, "state truncated", istr(a, any), "0");
        expect(t, "state trailing", istr(a, bashstatedecode(a, concat(a, data, "x")).ok), "0");

        // Unknown changes: no exports, unchanged
        BashState u = {};
        u.cwd = "/x";
        r = bashstatedecode(a, bashstateencode(a, &u));
        expect(t, "state unknown", istr(a, r.ok && !r.known && !r.changed && !r.pwd.len), "1");

        expect(t, "state garbage", istr(a, bashstatedecode(a, "garbage").ok), "0");
        expect(t, "state empty", istr(a, bashstatedecode(a, {}).ok), "0");
        Str other = concat(a, "prospero-bash-state 2", cuthead(data, bashstatemagic.len));
        expect(t, "state other version", istr(a, bashstatedecode(a, other).ok), "0");

        // An entry that never carries makes the file bad
        BashState p = s;
        p.exports = {};
        p.exports = push(a, p.exports, Str("PWD=/x"));
        r = bashstatedecode(a, bashstateencode(a, &p));
        expect(t, "state filtered entry", istr(a, r.ok), "0");

        // A count past the data
        Buf b(a, 128);
        print(&b, bashstatemagic);
        u8 z[] = "\0pwd\0\0cwd\0\0exports\0999\0X=1\0changed\0" "1\0end\0";
        print(&b, Str{z, countof(z)-1});
        expect(t, "state count", istr(a, bashstatedecode(a, finish(&b)).ok), "0");
    }

    // Names that never carry
    {
        expect(t, "name", bashexportname("X=1"), "X");
        expect(t, "name unset", bashexportname("X"), "X");
        expect(t, "name PWD", bashexportname("PWD=/x"), "");
        expect(t, "name OLDPWD", bashexportname("OLDPWD=/x"), "");
        expect(t, "name SHLVL", bashexportname("SHLVL=2"), "");
        expect(t, "name _", bashexportname("_=/bin/ls"), "");
        expect(t, "name drive", bashexportname("=C:=C:\\x"), "");
        expect(t, "name empty", bashexportname(""), "");
    }

    // Merge and apply
    {
        Slice<Str> carried = {};
        carried = push(a, carried, Str("A=1"));
        carried = push(a, carried, Str("Path=C:\\x"));
        carried = push(a, carried, Str("B=2"));
        Slice<Str> delta = {};
        delta = push(a, delta, Str("PATH=C:\\y"));
        delta = push(a, delta, Str("B"));
        delta = push(a, delta, Str("C=3"));
        delta = push(a, delta, Str("SHLVL=9"));
        expect(t, "merge windows", bashjoin(a, bashmerge(a, carried, delta, 1)),
               "A=1|PATH=C:\\y|B|C=3");
        expect(t, "merge posix", bashjoin(a, bashmerge(a, carried, delta, 0)),
               "A=1|Path=C:\\x|B|PATH=C:\\y|C=3");

        Slice<Str> env = {};
        env = push(a, env, Str("HOME=/h"));
        env = push(a, env, Str("B=old"));
        env = push(a, env, Str("A=old"));
        Slice<Str> ex = {};
        ex = push(a, ex, Str("A=new"));
        ex = push(a, ex, Str("B"));
        ex = push(a, ex, Str("D=4"));
        ex = push(a, ex, Str("PWD=/nope"));
        Slice<Str> r = bashapply(a, env, ex, 0);
        b32 home = 0, b = 0, an = 0, d = 0, pwd = 0;
        for (iz i = 0; i < r.len; i++) {
            home |= r[i] == "HOME=/h";
            b    |= startswith(r[i], "B=");
            an   |= r[i] == "A=new";
            d    |= r[i] == "D=4";
            pwd  |= startswith(r[i], "PWD=");
        }
        expect(t, "apply", istr(a, home && !b && an && d && !pwd), "1");

        Slice<Str> wenv = {};
        wenv = push(a, wenv, Str("Path=C:\\w"));
        Slice<Str> wex = {};
        wex = push(a, wex, Str("PATH=C:\\v"));
        Slice<Str> w = bashapply(a, wenv, wex, 1);
        expect(t, "apply windows", istr(a, w.len==1 && cut(w[0], '=').tail=="C:\\v"), "1");
    }

    // The argv shellcmd builds
    {
        Shell sh = {};
        sh.name = BUILTIN_NAME;
        sh.kind = SHELL_BUILTIN;
        sh.path = "/opt/prospero";
        ShellCmd c = shellcmd(a, &sh, 0, "ls");
        expect(t, "builtin argv", bashjoin(a, c.argv), "/opt/prospero|--bash|-c|ls");
        expect(t, "builtin no event", istr(a, c.stopevent), "0");

        BuiltinRun run = {};
        run.exe   = "/x/prospero";
        run.carry = "/s/out/a.state";
        run.state = "/s/out/b.state";
        sh.login  = 1;
        c = shellcmd(a, &sh, 0, "ls", run);
        expect(t, "builtin argv full", bashjoin(a, c.argv),
               "/x/prospero|--bash|--carry|/s/out/a.state|--state|/s/out/b.state|-l|-c|ls");

        sh.login = 0;
        sh.root  = "C:/git/usr/bin";
        sh.win   = 1;
        c = shellcmd(a, &sh, 1, "ls", {});
        expect(t, "builtin argv root", bashjoin(a, c.argv),
               "/opt/prospero|--bash|--root|C:/git/usr/bin|-c|ls");
        expect(t, "builtin event", istr(a, c.stopevent), "1");
        expect(t, "builtin no cmdline", istr(a, c.cmdline.len), "0");
        expect(t, "builtin no terminal", istr(a, termshell(a, "builtin", sh.path, 0).len), "0");
    }
}
