// Subagents on other hosts: the pure parts (both engines run end to
// end in tests/remote.py, far_*)

static Str farshow(Arena *a, Slice<FarHost> hosts)
{
    Buf b(a, 256);
    for (iz i = 0; i < hosts.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, hosts[i].name);
        print(&b, hosts[i].ssh.len ? Str("=ssh:") : Str("=wsl:"));
        print(&b, hosts[i].ssh.len ? hosts[i].ssh : hosts[i].wsl);
    }
    return finish(&b);
}

static void test_hostlink(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);

    // The configuration's own remotes, then the distros not claimed,
    // but none an included file defines, and none with subagents = no
    os_write(v.os, a, "/h/team.ini", "[remote team]\nssh = team.invalid\n"
                                     "[remote box]\nsubagents = no\n");
    os_write(v.os, a, "/h/x.ini",
             "include = team.ini\n"
             "[remote box]\nssh = me@box\n"
             "[remote deb]\nwsl = Debian\n"
             "[remote off]\nssh = off.invalid\nsubagents = no\n"
             "[remote Ubuntu]\nsubagents = no\n"
             "[remote empty]\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/h/x.ini", 0);
    Str     d[] = {"Debian", "Ubuntu", "Alpine"};
    Slice<Str> distros = {d, countof(d), countof(d)};
    expect(t, "farhosts", farshow(&a, farhosts(&a, c, distros, 1)),
           "box=ssh:me@box deb=wsl:Debian Alpine=wsl:Alpine");
    expect(t, "farhosts without wsl", farshow(&a, farhosts(&a, c, distros, 0)),
           "box=ssh:me@box");
    expect(t, "farhosts none", farshow(&a, farhosts(&a, c, {}, 0)), "box=ssh:me@box");

    Slice<FarHost> hosts = farhosts(&a, c, distros, 1);
    expect(t, "farhostlines", farhostlines(&a, hosts),
           "\n- box: another machine over ssh: it shares no files with this one"
           "\n- deb: Linux (WSL) on this machine: it sees this machine's drives "
           "under /mnt (C:\\Users is /mnt/c/Users); its own files are "
           "\\\\wsl.localhost\\Debian\\... here"
           "\n- Alpine: Linux (WSL) on this machine: it sees this machine's drives "
           "under /mnt (C:\\Users is /mnt/c/Users); its own files are "
           "\\\\wsl.localhost\\Alpine\\... here");
    expect(t, "farhostlines none", istr(&a, !farhostlines(&a, {}).data), "1");

    // Where a subagent works there
    FarHost deb = {"deb", {}, "Debian"};
    FarHost box = {"box", "me@box", {}};
    struct { FarHost *h; Str pcwd; Str cwd; Str want; } cwds[] = {
        {&deb, "C:\\Users\\me\\proj", {}, "/mnt/c/Users/me/proj"},
        {&deb, "d:/src", {}, "/mnt/d/src"},
        {&deb, "\\\\wsl.localhost\\Debian\\home\\me", {}, "/home/me"},
        {&deb, "\\\\WSL$\\debian\\home", {}, "/home"},
        {&deb, "\\\\wsl.localhost\\Ubuntu\\home\\me", {}, "~"},
        {&deb, "\\\\server\\share\\x", {}, "~"},
        {&deb, "/home/me", {}, "~"},
        {&deb, "C:\\x", "/srv/y", "/srv/y"},
        {&box, "C:\\x", {}, "~"},
        {&box, "/home/me", "~/src", "~/src"},
    };
    for (iz i = 0; i < countof(cwds); i++) {
        Str got = farcwd(&a, cwds[i].h, cwds[i].pcwd, cwds[i].cwd);
        expect(t, concat(&a, "farcwd ", cwds[i].pcwd), got, cwds[i].want);
    }

    // The parent's model, where its endpoint has a name
    expect(t, "farprefer", farprefer(&a, "fast", "qwen3"), "fast/qwen3");
    expect(t, "farprefer url", farprefer(&a, "http://x:8080/v1", "qwen3"), "");
    expect(t, "farprefer none", farprefer(&a, {}, "qwen3"), "");

    // The new command
    FarAsk k = {};
    k.id        = 7;
    k.cwd       = "/mnt/c/proj";
    k.attended  = 1;
    k.role      = "explore";
    k.projinstr = 1;
    k.tempask   = 1;
    k.parent    = "20260101-000000-aaaa";
    k.call      = "call_3_0";
    k.depth     = 1;
    k.hop       = 2;
    k.plan      = 1;
    k.title     = "Build on Linux";
    k.prefer    = "local/qwen3";
    k.via       = "wsl";
    k.distro    = "Ubuntu";
    k.prompt    = "Run \"make\".";
    expect(t, "farline", farline(&a, &k),
           "{\"cmd\":\"new\",\"id\":7,\"cwd\":\"/mnt/c/proj\",\"attended\":1,"
           "\"role\":\"explore\",\"instructions\":\"project\",\"temp\":\"ask\","
           "\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"call_3_0\","
           "\"depth\":1,\"hop\":2,\"plan\":1,\"title\":\"Build on Linux\","
           "\"prefer\":\"local/qwen3\",\"via\":\"wsl\",\"distro\":\"Ubuntu\","
           "\"prompt\":\"Run \\\"make\\\".\"}}");
    k = {};
    k.id     = 1;
    k.cwd    = "~";
    k.model  = "big";
    k.nofallback = 1;
    k.parent = "20260101-000000-aaaa";
    k.call   = "c";
    k.depth  = 2;
    k.via    = "ssh";
    k.prompt = "p";
    expect(t, "farline plain", farline(&a, &k),
           "{\"cmd\":\"new\",\"id\":1,\"cwd\":\"~\",\"attended\":0,\"model\":\"big\","
           "\"fallback\":0,\"task\":{\"parent\":\"20260101-000000-aaaa\",\"call\":\"c\","
           "\"depth\":2,\"hop\":0,\"plan\":0,\"title\":\"\",\"via\":\"ssh\",\"prompt\":\"p\"}}");
    cfgfree(c);
}
