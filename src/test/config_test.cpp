// Configuration tests: schema, seed, validation, getters, edits, reload,
// protocol payloads, and the engine's use of the settings

// A test platform with its own storage and a separate scratch arena,
// since a Config's perm and scratch must not overlap.
struct CfgEnv {
    Os   *os;
    Arena scratch;
};

static CfgEnv newcfgenv(Arena *a)
{
    CfgEnv r = {};
    r.os = newos(a, 1<<20);
    r.scratch = carve(a, 1<<22);
    return r;
}

static void addenv(Os *os, Arena *a, Str entry)
{
    os->env = push(a, os->env, entry);
}

static Str cfgfile(Os *os, Str path)
{
    VFile *f = vfind(os, path, 0);
    return f && f->exists ? f->data : Str("(missing)");
}

static Str showlist(Arena *a, Slice<Str> list)
{
    Buf b(a, 64);
    for (iz i = 0; i < list.len; i++) {
        print(&b, i ? Str("|") : Str(""));
        print(&b, list[i]);
    }
    return finish(&b);
}

static void testcfgschema(Test *t)
{
    Arena a   = t->perm;
    i32   bad = 0;
    for (iz i = 0; i < countof(cfgschema); i++) {
        CfgKey *k  = &cfgschema[i];
        b32     ok = cfgkind(k->kind) && cfgkey(k->kind, k->key)==k &&
                     cfgjudge(k, k->dflt, 0)==CFG_OK &&
                     (!k->local.data || cfgjudge(k, k->local, 0)==CFG_OK) &&
                     k->desc.len && k->group.len && k->type>=0 &&
                     k->type<CFG_NTYPES &&
                     ((k->type!=CFG_ENUM && k->type!=CFG_LIST) ||
                      k->choices.len) &&
                     iniword(k->key, 1) &&
                     (!k->kind.len || iniword(k->kind, 0));
        if (!ok) {
            expect(t, concat(&a, "schema entry ", k->key), "bad", "ok");
            bad++;
        }
    }
    expect(t, "schema", istr(&a, bad), "0");

    // Judgments by type
    static struct { Str kind, key, value; i32 want; b32 shared; } judge[] = {
        {"endpoint", "url", "http://h/", CFG_OK, 0},
        {"endpoint", "url", "ftp://h/", CFG_BAD, 0},
        {"endpoint", "url", "", CFG_OK, 0},
        {"endpoint", "idle_timeout", "2.5", CFG_OK, 0},
        {"endpoint", "idle_timeout", "0", CFG_BAD, 0},
        {"endpoint", "idle_timeout", "-1", CFG_BAD, 0},
        {"endpoint", "idle_timeout", "1e7", CFG_BAD, 0},
        {"endpoint", "idle_timeout", " 30 ", CFG_OK, 0},
        {"endpoint", "idle_timeout", "30s", CFG_BAD, 0},
        {"endpoint", "slow_retry", "60 120, 0.5", CFG_OK, 0},
        {"endpoint", "slow_retry", "60 x", CFG_BAD, 0},
        {"endpoint", "slow_retry", "60 0", CFG_BAD, 0},
        {"endpoint", "key", "env:API_KEY", CFG_OK, 1},
        {"endpoint", "key", "env:", CFG_BAD, 0},
        {"endpoint", "key", "env:1X", CFG_BAD, 0},
        {"endpoint", "key", "env:A-B", CFG_BAD, 0},
        {"endpoint", "key", "sk-123", CFG_ODD, 0},
        {"endpoint", "key", "sk-123", CFG_BAD, 1},
        {"model", "context", "32768", CFG_OK, 0},
        {"model", "context", "32k", CFG_BAD, 0},
        {"model", "temperature", "0.7", CFG_OK, 0},
        {"model", "temperature", "warm", CFG_BAD, 0},
        {"model", "seed", "-1", CFG_OK, 0},
        {"model", "reasoning", "none", CFG_OK, 0},
        {"model", "reasoning", "thoughts", CFG_ODD, 0},
        {"role", "tools", "read, shell", CFG_OK, 0},
        {"role", "tools", "read fly", CFG_ODD, 0},
        {"role", "prompt", "anything # at all", CFG_OK, 0},
        {"guard", "roots", "/a; ~/b; ~; C:\\x; //srv/share", CFG_OK, 0},
        {"guard", "roots", "/a; b", CFG_ODD, 0},
        {"guard", "roots", ";;", CFG_OK, 0},
        {"tools", "path_prepend", "/x; ~/y", CFG_OK, 0},
        {"tools", "path_prepend", "/x", CFG_BAD, 1},
        {"tools", "path_append", "/x", CFG_BAD, 1},
        {"tools", "path_append", " ", CFG_OK, 1},
        {"tools", "shell", "/bin/zsh", CFG_OK, 0},
        {"tools", "shell", "/bin/zsh", CFG_BAD, 1},
        {"tools", "shell", "zsh", CFG_BAD, 1},
        {"tools", "shell", " powershell ", CFG_OK, 1},
        {"tools", "shell", "auto", CFG_OK, 1},
        {"tools", "git", "/opt/homebrew/bin/git", CFG_OK, 0},
        {"tools", "git", "git", CFG_BAD, 1},
        {"tools", "git", " ", CFG_OK, 1},
        {"ui", "editor", "code --wait", CFG_OK, 0},
        {"ui", "editor", "code --wait", CFG_BAD, 1},
        {"ui", "theme", "dark", CFG_OK, 1},
        {"ui", "notify", "allow, done", CFG_OK, 0},
        {"ui", "notify", "none", CFG_OK, 0},
        {"ui", "notify", "done bell", CFG_ODD, 0},
        {"ui", "notify", "allow done -done", CFG_OK, 1},
    };
    for (iz i = 0; i < countof(judge); i++) {
        CfgKey *k = cfgkey(judge[i].kind, judge[i].key);
        Str     name = concat(&a, concat(&a, judge[i].key, " = "),
                              judge[i].value);
        expect(t, name, istr(&a, cfgjudge(k, judge[i].value, judge[i].shared)),
               istr(&a, judge[i].want));
    }

    static struct { Str in; i32 want; } bools[] = {
        {"true", 1}, {"Yes", 1}, {"ON", 1}, {"1", 1}, {"false", 0},
        {"no", 0}, {"off", 0}, {"0", 0}, {"", -1}, {"maybe", -1},
    };
    for (iz i = 0; i < countof(bools); i++) {
        expect(t, concat(&a, "bool ", bools[i].in),
               istr(&a, cfgbool(bools[i].in)), istr(&a, bools[i].want));
    }

    static struct { Str a, b; iz want; } dist[] = {
        {"idle_timeout", "idle_timout", 1}, {"url", "url2", 1},
        {"", "abc", 3}, {"model", "color", 4}, {"kitten", "sitting", 3},
    };
    for (iz i = 0; i < countof(dist); i++) {
        expect(t, concat(&a, "distance ", dist[i].a),
               istr(&a, cfgdistance(dist[i].a, dist[i].b)),
               istr(&a, dist[i].want));
    }
}

static void testcfgdefault(Test *t)
{
    static struct { Str env[2]; Str want; } cases[] = {
        {{"XDG_CONFIG_HOME=/x", "HOME=/h"}, "/x/prospero/prospero.ini"},
        {{"HOME=/h", "APPDATA=C:\\A"}, "/h/.config/prospero/prospero.ini"},
        {{"APPDATA=C:\\A", {}}, "C:\\A/prospero/prospero.ini"},
        {{"XDG_CONFIG_HOME=", "HOME=/h"}, "/h/.config/prospero/prospero.ini"},
        {{{}, {}}, "(null)"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena a  = t->perm;
        Os   *os = newos(&a, 1<<12);
        for (i32 k = 0; k < 2; k++) {
            if (cases[i].env[k].data) {
                addenv(os, &a, cases[i].env[k]);
            }
        }
        Str got = cfgdefault(os, &a);
        expect(t, concat(&a, "default ", cases[i].want),
               got.data ? got : Str("(null)"), cases[i].want);
    }

    // Normalized for loading and editing
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "APPDATA=C:\\Users\\u\\AppData\\Roaming");
    Config *c = newconfig(v.os, &a, v.scratch, cfgdefault(v.os, &a), 1);
    expect(t, "default.normalized", c->path,
           "C:/Users/u/AppData/Roaming/prospero/prospero.ini");
    expect(t, "default.seeded", cfgfile(v.os, c->path), cfgseed());
    cfgfree(c);

    // With a home, a file only in the older place is still used
    CfgEnv k = newcfgenv(&a);
    addenv(k.os, &a, "APPDATA=/A");
    addenv(k.os, &a, "HOME=/h");
    os_mkdirs(k.os, a, "/A/prospero");
    os_write(k.os, a, "/A/prospero/prospero.ini", "");
    expect(t, "default.kept", cfgdefault(k.os, &a), "/A/prospero/prospero.ini");
    os_mkdirs(k.os, a, "/h/.config/prospero");
    os_write(k.os, a, "/h/.config/prospero/prospero.ini", "");
    expect(t, "default.home", cfgdefault(k.os, &a), "/h/.config/prospero/prospero.ini");
}

// First run writes the seed byte for byte, and it is a clean, complete
// configuration; a later run keeps the user's file as it is.
static void testcfgseed(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "HOME=/home/u");
    Str     path = cfgdefault(v.os, &a);
    Config *c    = newconfig(v.os, &a, v.scratch, path, 1);
    expect(t, "seed.path", c->path, "/home/u/.config/prospero/prospero.ini");
    expect(t, "seed.written", cfgfile(v.os, c->path), cfgseed());
    Stat dir = os_stat(v.os, a, "/home/u/.config/prospero");
    expect(t, "seed.dir", istr(&a, dir.type), istr(&a, STAT_DIR));
    expect(t, "seed.diags", inidiags(&a, c->ini), "");
    expect(t, "seed.exists", istr(&a, c->user != 0), "1");
    expect(t, "seed.rev", istr(&a, c->rev), "1");
    expect(t, "seed.endpoint", cfgdefaults(c).endpoint, "local");
    expect(t, "seed.url", cfgget(c, "endpoint", "local", "url", 1),
           "http://localhost/");
    expect(t, "seed.configured", istr(&a, cfgconfigured(c)), "0");
    expect(t, "seed.newline", istr(&a, cfgseed()[cfgseed().len-1]), "10");
    for (iz i = 0; i < c->user->lines.len; i++) {
        if (c->user->lines[i].raw.len > 79) {
            expect(t, "seed line too long", c->user->lines[i].raw, "");
        }
    }
    cfgfree(c);

    // Every key in the schema is documented in the seed ("git" is in
    // "digits" too, so it is looked for as the seed lists it)
    for (iz i = 0; i < countof(cfgschema); i++) {
        Str key = cfgschema[i].key;
        if (find(cfgseed(), key) < 0) {
            expect(t, "seed documents", key, "");
        }
    }
    expect(t, "seed documents git", istr(&a, find(cfgseed(), "#   git ") >= 0), "1");

    // An existing file is left alone, and no seed without permission
    os_write(v.os, a, path, "# mine\n");
    c = newconfig(v.os, &a, v.scratch, path, 1);
    expect(t, "seed.kept", cfgfile(v.os, path), "# mine\n");
    cfgfree(c);
    c = newconfig(v.os, &a, v.scratch, "/elsewhere/h.ini", 0);
    expect(t, "seed.optional", cfgfile(v.os, "/elsewhere/h.ini"), "(missing)");
    expect(t, "seed.missing", inidiags(&a, c->ini),
           "/elsewhere/h.ini: warning: cannot read file (not found)");
    expect(t, "seed.notuser", istr(&a, c->user != 0), "0");
    cfgfree(c);

    // A model, by default or by section, or a remote host: configured
    Str configured[] = {
        "model = m\n",
        "[model m]\ncontext = 1000\n",
        "[remote r]\nssh = r.invalid\n",
        "include = team.ini\n",
    };
    os_write(v.os, a, "/c/team.ini", "[model shared]\n");
    for (i32 i = 0; i < countof(configured); i++) {
        os_write(v.os, a, "/c/x.ini", configured[i]);
        c = newconfig(v.os, &a, v.scratch, "/c/x.ini", 0);
        expect(t, "seed.configured.yes", istr(&a, cfgconfigured(c)), "1");
        cfgfree(c);
    }
    Str unconfigured[] = {
        "",
        "model =\n[endpoint e]\nurl = http://localhost/\n",
        "[role r]\nmodel = m\n[model]\n",
    };
    for (i32 i = 0; i < countof(unconfigured); i++) {
        os_write(v.os, a, "/c/x.ini", unconfigured[i]);
        c = newconfig(v.os, &a, v.scratch, "/c/x.ini", 0);
        expect(t, "seed.configured.no", istr(&a, cfgconfigured(c)), "0");
        cfgfree(c);
    }

    // The defaults of a new session: Defaults', else the first endpoint
    // with a URL and the first [model], trimmed
    struct {
        Str ini;
        Str want;  // "ENDPOINT MODEL", then q if qualified, g if guessed
    } dflts[] = {
        {"", " "},
        {"endpoint = \" e \"\nmodel = \" m \"\n[endpoint x]\nurl = http://x/\n"
         "[model y]\n", "e m"},
        {"[endpoint nourl]\nkind = openai\n[endpoint bad]\nurl = nope\n"
         "[endpoint a]\nurl = http://a/\n[endpoint b]\nurl = http://b/\n", "a "},
        {"[model]\n[model first]\n[endpoint a]\nurl = http://a/\n[model second]\n",
         "a first g"},
        {"endpoint = http://u/\n[endpoint a]\nurl = http://a/\n", "http://u/ "},
        {"model =\n[role r]\nmodel = m\n", " "},
        // A model named with its endpoint, which wins over endpoint
        {"model = b/Org/M-1\n[endpoint a]\nurl = http://a/\n"
         "[endpoint b]\nurl = http://b/\n", "b Org/M-1 q"},
        {"endpoint = a\nmodel = b/m\n[endpoint a]\nurl = http://a/\n"
         "[endpoint b]\nurl = http://b/\n", "b m q"},
        // A slash that names no endpoint is the model's own
        {"model = Qwen/Qwen3-8B\n[endpoint a]\nurl = http://a/\n",
         "a Qwen/Qwen3-8B g"},
        {"endpoint = a\nmodel = nourl/m\n[endpoint a]\nurl = http://a/\n"
         "[endpoint nourl]\nkind = openai\n", "a nourl/m"},
    };
    for (i32 i = 0; i < countof(dflts); i++) {
        os_write(v.os, a, "/c/x.ini", dflts[i].ini);
        c = newconfig(v.os, &a, v.scratch, "/c/x.ini", 0);
        CfgDefaults d = cfgdefaults(c);
        Str got = concat(&a, concat(&a, d.endpoint, " "), d.model);
        got = d.qualified ? concat(&a, got, " q") : got;
        got = d.guessed && d.model.len ? concat(&a, got, " g") : got;
        expect(t, "cfgdefaults", got, dflts[i].want);
        if (i == countof(dflts)-3) {
            expect(t, "cfgdefaults.conflict", inidiags(&a, c->ini),
                   "/c/x.ini:1:12: warning: model names endpoint b, so this is "
                   "ignored");
        }
        cfgfree(c);
    }

    // No location at all
    c = newconfig(v.os, &a, v.scratch, {}, 1);
    expect(t, "seed.nowhere", inidiags(&a, c->ini),
           "warning: no configuration file: none of XDG_CONFIG_HOME, HOME "
           "or APPDATA is set");
    CfgSet r = cfgset(c, &a, "", "", "model", "m");
    expect(t, "seed.nowhere.set", r.err, "no configuration file location");
    cfgfree(c);
}

// A remote reaches its machine one way: wsl beside ssh is an error.
static void testcfgwsl(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "HOME=/home/u");
    os_write(v.os, a, "/c/prospero.ini",
        "[remote both]\n"
        "ssh = pi\n"
        "wsl = Debian\n"
        "[remote deb]\n"
        "wsl = Debian\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/prospero.ini", 0);
    expect(t, "wsl.diags", inidiags(&a, c->ini),
        "/c/prospero.ini:3:7: error: ssh is set too, which reaches the machine, "
        "so this is ignored");
    expect(t, "wsl.get", cfgget(c, "remote", "deb", "wsl", 0), "Debian");
    Slice<LinkTo> r = cfgremotes(&a, c);
    expect(t, "wsl.remotes", istr(&a, r.len), "2");
    expect(t, "wsl.both", concat(&a, r[0].ssh, concat(&a, "/", r[0].wsl)), "pi/");
    expect(t, "wsl.deb", concat(&a, r[1].ssh, concat(&a, "/", r[1].wsl)), "/Debian");
}

static void testcfgvalidate(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "HOME=/home/u");
    os_write(v.os, a, "/c/prospero.ini",
        "endpoint = local\n"                    // 1
        "include = team.ini\n"
        "model = m\n"
        "color = blue\n"
        "[endpoint local]\n"                    // 5
        "url = http://localhost:8080/\n"
        "idle_timeout = soon\n"
        "idle_timout = 5\n"
        "key = sk-plain\n"
        "[endpoint broken]\n"                   // 10
        "key = env:MISSING_VAR\n"
        "[endpoint]\n"
        "url = http://x/\n"
        "[guard]\n"
        "roots = rel/path; ~/ok; /abs\n"        // 15
        "[model m]\n"
        "reasoning = thoughts\n"
        "top_k = 2.5\n"
        "[role r]\n"
        "tools = read fly\n"                    // 20
        "[provider x]\n"
        "url = http://y/\n"
        "[guard g]\n"
        "roots = /x\n");
    os_write(v.os, a, "/c/team.ini",
        "endpoint = team\n"
        "[endpoint team]\n"
        "url = https://team.example/\n"
        "key = sk-team-plain\n"
        "connect_timeout = 3\n"                 // 5
        "url2 = x\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/prospero.ini", 0);
    expect(t, "validate.diags", inidiags(&a, c->ini),
        "/c/prospero.ini:4:1: warning: unknown key \"color\" at the top level\n"
        "/c/prospero.ini:7:16: error: expected seconds, a number above 0, so "
        "the default applies\n"
        "/c/prospero.ini:8:1: warning: unknown key \"idle_timout\" in "
        "[endpoint]; did you mean idle_timeout?\n"
        "/c/prospero.ini:9:7: warning: a plaintext secret: write env:NAME and "
        "set the variable, or file:PATH, so that this file can be shared\n"
        "/c/prospero.ini:10:2: warning: endpoint broken has no url\n"
        "/c/prospero.ini:12:2: warning: a section needs a name, as in "
        "[endpoint NAME], or its settings are unused\n"
        "/c/prospero.ini:15:9: warning: relative path \"rel/path\": use an "
        "absolute path or one starting with ~\n"
        "/c/prospero.ini:17:13: warning: unknown value \"thoughts\", expected "
        "one of: reasoning_content, reasoning, reasoning_text, none\n"
        "/c/prospero.ini:18:9: error: expected a whole number, so this is "
        "ignored\n"
        "/c/prospero.ini:20:9: warning: unknown value \"fly\", expected any "
        "of: read, write, edit, shell, grep, glob, todo_write, jobs, "
        "job_output, job_kill, task, skill, sessions, send_message, memory, "
        "web_search, web_fetch, desktop, *, default\n"
        "/c/prospero.ini:21:2: warning: unknown section kind \"provider\", "
        "expected endpoint, model, role, guard, tools, shell, instructions, "
        "skills, memory, ui, desktop, web, host, remote or mcp\n"
        "/c/prospero.ini:23:8: warning: [guard] takes no name, or its "
        "settings are unused\n"
        "/c/team.ini:4:7: error: a plaintext secret in an included file is "
        "ignored: write env:NAME and set the variable\n"
        "/c/team.ini:6:1: warning: unknown key \"url2\" in [endpoint]; did "
        "you mean url?\n"
        "/c/prospero.ini:1:1: warning: overridden by /c/team.ini:1, included "
        "after this line\n"
        "/c/prospero.ini:11:7: warning: environment variable MISSING_VAR is "
        "not set");

    // Values in effect
    static struct { Str kind, name, key; b32 local; Str want; } gets[] = {
        {"", "", "endpoint", 0, "team"},
        {"", "", "model", 0, "m"},
        {"", "", "color", 0, "blue"},
        {"", "", "nothing", 0, "(null)"},
        {"endpoint", "local", "idle_timeout", 1, "90"},
        {"endpoint", "local", "first_byte_timeout", 1, "1800"},
        {"endpoint", "local", "first_byte_timeout", 0, "300"},
        {"endpoint", "team", "connect_timeout", 0, "3"},
        {"endpoint", "nope", "connect_timeout", 0, "15"},
        {"endpoint", "", "url", 0, ""},
        {"model", "m", "top_k", 0, ""},
        {"model", "m", "reasoning", 0, "thoughts"},
        {"model", "x", "reasoning", 0, "reasoning_content"},
        {"guard", "g", "roots", 0, "/x"},
    };
    for (iz i = 0; i < countof(gets); i++) {
        Str got  = cfgget(c, gets[i].kind, gets[i].name, gets[i].key,
                          gets[i].local);
        Str name = concat(&a, concat(&a, gets[i].name, "."), gets[i].key);
        expect(t, concat(&a, "get ", name), got.data ? got : Str("(null)"),
               gets[i].want);
    }
    i64 ms1 = cfgms(c, "endpoint", "local", "idle_timeout", 1);
    i64 ms2 = cfgms(c, "endpoint", "team", "connect_timeout", 0);
    expect(t, "ms.bad", istr(&a, ms1), "90000");
    expect(t, "ms.team", istr(&a, ms2), "3000");
    Str s1 = cfgsecret(c, &a, "endpoint", "local", "key");
    Str s2 = cfgsecret(c, &a, "endpoint", "team", "key");
    Str s3 = cfgsecret(c, &a, "endpoint", "broken", "key");
    expect(t, "secret.user", s1, "sk-plain");
    expect(t, "secret.shared", s2.data ? s2 : Str("(null)"), "(null)");
    expect(t, "secret.unset", s3.data ? s3 : Str("(null)"), "(null)");
    expect(t, "paths", showlist(&a, cfgpaths(c, &a, "guard", "", "roots", "/home/u")),
           "rel/path|/home/u/ok|/abs");
    expect(t, "paths.nohome", showlist(&a, cfgpaths(c, &a, "guard", "", "roots", "")),
           "rel/path|~/ok|/abs");
    cfgfree(c);

    // The reader's limit on diagnostics per file holds for the schema's
    {
        Buf b(&a, 4096);
        for (i32 i = 0; i < 150; i++) {
            print(&b, "unknown");
            print(&b, (i64)i);
            print(&b, " = 1\n");
        }
        os_write(v.os, a, "/c/many.ini", finish(&b));
        c = newconfig(v.os, &a, v.scratch, "/c/many.ini", 0);
        Ini *ini = c->ini;
        expect(t, "cap.count", istr(&a, ini->diags.len),
               istr(&a, INI_MAXDIAGS+1));
        Buf last(&a, 128);
        print(&last, ini->diags[ini->diags.len-1]);
        expect(t, "cap.last", finish(&last),
               "/c/many.ini:101: warning: too many diagnostics, rest "
               "suppressed");
        cfgfree(c);
    }

    os_write(v.os, a, "/c/refs.ini",
             "endpoint = nowhere\n"
             "[role r]\n"
             "endpoint = http://x/\n"
             "[role s]\n"
             "endpoint = e\n"
             "[endpoint e]\n"
             "url = http://e/\n");
    c = newconfig(v.os, &a, v.scratch, "/c/refs.ini", 0);
    expect(t, "refs.diags", inidiags(&a, c->ini),
           "/c/refs.ini:1:12: warning: no [endpoint nowhere] with a url, "
           "and not a URL");
    cfgfree(c);

    addenv(v.os, &a, "MISSING_VAR=s3cret");
    addenv(v.os, &a, "EMPTY_VAR=");
    os_write(v.os, a, "/c/env.ini",
             "[endpoint a]\nurl = http://a/\nkey = env:MISSING_VAR\n"
             "[endpoint b]\nurl = http://b/\nkey = env:EMPTY_VAR\n");
    c = newconfig(v.os, &a, v.scratch, "/c/env.ini", 0);
    expect(t, "env.diags", inidiags(&a, c->ini), "");
    expect(t, "env.secret", cfgsecret(c, &a, "endpoint", "a", "key"), "s3cret");
    expect(t, "env.empty", cfgsecret(c, &a, "endpoint", "b", "key"), "");
    cfgfree(c);

    // Secrets read from files where the engine runs, from the user's own
    // file only, since an include could name any file to be sent
    addenv(v.os, &a, "HOME=/home/u");
    os_write(v.os, a, "/home/u/k.key", "sk-from-file\n");
    os_write(v.os, a, "/c/k.key", "  sk-abs  \n");
    os_write(v.os, a, "/c/files.ini",
             "include = files-team.ini\n"
             "[endpoint a]\nurl = http://a/\nkey = file:~/k.key\n"
             "[endpoint b]\nurl = http://b/\nkey = file: /c/k.key\n"   // 7
             "[endpoint c]\nurl = http://c/\nkey = file:/c/none.key\n"
             "[endpoint d]\nurl = http://d/\nkey = file:k.key\n");     // 13
    os_write(v.os, a, "/c/files-team.ini",
             "[endpoint e]\nurl = http://e/\nkey = file:/c/k.key\n");
    c = newconfig(v.os, &a, v.scratch, "/c/files.ini", 0);
    expect(t, "file.diags", inidiags(&a, c->ini),
        "/c/files.ini:13:7: error: expected file:PATH, with PATH absolute or "
        "under ~\n"
        "/c/files-team.ini:3:7: error: a secret from a file in an included "
        "file is ignored, since it could send any of your files: set it in "
        "your own file\n"
        "/c/files.ini:10:7: warning: /c/none.key cannot be read, or is empty");
    static struct { Str name, want; } files[] = {
        {"a", "sk-from-file"}, {"b", "sk-abs"}, {"c", "(null)"},
        {"d", "(null)"}, {"e", "(null)"},
    };
    for (iz i = 0; i < countof(files); i++) {
        Str got = cfgsecret(c, &a, "endpoint", files[i].name, "key");
        expect(t, concat(&a, "file.secret ", files[i].name),
               got.len ? got : Str("(null)"), files[i].want);
    }
    expect(t, "file.secrets", showlist(&a, cfgsecrets(c, &a)), "");
    cfgfree(c);

    // The variables of every secret setting, overridden ones too, which
    // the agent's commands do not get
    os_write(v.os, a, "/c/secrets.ini",
             "[endpoint a]\nurl = http://a/\nkey = env:FIRST_KEY\n"
             "key = env:SECOND_KEY\n"
             "[endpoint b]\nurl = http://b/\nkey = sk-plain\n"
             "[endpoint c]\nurl = http://c/\nkey = env: OTHER_KEY \n"
             "[endpoint d]\nurl = http://d/\nkey = env:\n"
             "[model m]\nkey = env:NOT_A_SECRET\n");
    c = newconfig(v.os, &a, v.scratch, "/c/secrets.ini", 0);
    expect(t, "secrets", showlist(&a, cfgsecrets(c, &a)), "SECOND_KEY|FIRST_KEY|OTHER_KEY");
    cfgfree(c);
}

// The exact file after each edit, and the reload that follows.
static void testcfgset(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    Str    team = "[endpoint team]\nurl = https://team.example/\n";
    os_write(v.os, a, "/c/team.ini", team);
    os_write(v.os, a, "/c/prospero.ini",
        "# My settings\n"
        "include = team.ini\n"
        "\n"
        "[endpoint local]\n"
        "url = http://localhost:8080/   # llama.cpp\n"
        "idle_timeout = 90\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/prospero.ini", 0);
    expect(t, "set.clean", inidiags(&a, c->ini), "");

    static struct { Str kind, name, key, value; b32 rm; iz line; } edits[] = {
        {"endpoint", "local", "idle_timeout", "120", 0, 6},
        {"endpoint", "local", "first_byte_timeout", "600", 0, 7},
        {"endpoint", "team", "connect_timeout", "4", 0, 10},
        {"", "", "model", "qwen", 0, 3},
        {"endpoint", "local", "idle_timeout", {}, 1, 0},
        {"endpoint", "local", "first_byte_timeout", "600", 0, 7},
        {"model", "qwen", "temperature", "0.5", 0, 13},
    };
    for (iz i = 0; i < countof(edits); i++) {
        Arena  tmp = v.scratch;
        Str    val = edits[i].rm ? Str{} : edits[i].value;
        CfgSet r   = cfgset(c, &tmp, edits[i].kind, edits[i].name,
                            edits[i].key, val);
        Str    name = concat(&a, "set ", edits[i].key);
        expect(t, name, r.err, "");
        expect(t, concat(&a, name, " (line)"), istr(&a, r.line),
               istr(&a, edits[i].line));
        Str got = cfgget(c, edits[i].kind, edits[i].name, edits[i].key, 0);
        expect(t, concat(&a, name, " (value)"), got,
               edits[i].rm ? cfgkey(edits[i].kind, edits[i].key)->dflt
                           : edits[i].value);
    }
    expect(t, "set.text", cfgfile(v.os, "/c/prospero.ini"),
        "# My settings\n"
        "include = team.ini\n"
        "model = qwen\n"
        "\n"
        "[endpoint local]\n"
        "url = http://localhost:8080/   # llama.cpp\n"
        "first_byte_timeout = 600\n"
        "\n"
        "[endpoint team]\n"
        "connect_timeout = 4\n"
        "\n"
        "[model qwen]\n"
        "temperature = 0.5\n");
    expect(t, "set.team", cfgfile(v.os, "/c/team.ini"), team);
    expect(t, "set.rev", istr(&a, c->rev), "8");
    expect(t, "set.diags", inidiags(&a, c->ini), "");

    // Refusals leave the file alone
    Str before = cfgfile(v.os, "/c/prospero.ini");
    static struct { Str kind, name, key, value, err; } bad[] = {
        {"endpoint", "local", "colour", "x", "unknown key"},
        {"endpoint", "local", "idle_timeout", "soon",
         "expected seconds, a number above 0, so the default applies"},
        {"endpoint", "local", "key", "sk-x",
         "a secret must be a reference such as env:NAME or file:PATH"},
        {"endpoint", "local", "key", "file:rel/k", "expected file:PATH, with "
         "PATH absolute or under ~"},
        {"endpoint", "local", "key", "env:", "expected env:NAME, with NAME "
         "an environment variable"},
        {"endpoint", "", "url", "http://x/",
         "this kind of section needs a name"},
        {"guard", "g", "roots", "/x", "this kind of section has no name"},
        {"provider", "x", "url", "http://x/", "unknown section kind"},
        {"role", "r", "prompt", "two\nlines",
         "a value cannot contain a newline"},
        {"model", "a b", "seed", "1", "invalid section or key name"},
    };
    for (iz i = 0; i < countof(bad); i++) {
        Arena  tmp = v.scratch;
        CfgSet r   = cfgset(c, &tmp, bad[i].kind, bad[i].name, bad[i].key,
                            bad[i].value);
        expect(t, concat(&a, "refuse ", bad[i].key), r.err, bad[i].err);
        expect(t, concat(&a, "refuse.ok ", bad[i].key), istr(&a, r.ok), "0");
    }
    expect(t, "refuse.file", cfgfile(v.os, "/c/prospero.ini"), before);

    // Setting what is already there writes nothing, but reloads
    i64 rev = c->rev;
    {
        Arena  tmp = v.scratch;
        VFile *f   = vfind(v.os, "/c/prospero.ini", 0);
        Str    old = f->data;
        CfgSet r   = cfgset(c, &tmp, "", "", "model", "qwen");
        expect(t, "same.ok", istr(&a, r.ok && r.line==3), "1");
        expect(t, "same.untouched", istr(&a, f->data.data == old.data), "1");
        expect(t, "same.rev", istr(&a, c->rev - rev), "1");
    }
    cfgfree(c);

    // A missing file starts from the seed, or from nothing without it
    for (i32 seed = 0; seed < 2; seed++) {
        Arena   tmp = v.scratch;
        Str     p   = seed ? Str("/s/prospero.ini") : Str("/n/prospero.ini");
        Config *m   = newconfig(v.os, &a, tmp, p, seed);
        os_remove(v.os, a, p);
        CfgSet  r   = cfgset(m, &tmp, "", "", "model", "m");
        Str     want = "model = m\n";
        if (seed) {
            Str s   = cfgseed();
            Str ex  = "# model = local/qwen3-coder\n";  // set under its example
            iz  at  = find(s, ex) + ex.len;
            want = concat(&a, takehead(s, at), want);
            want = concat(&a, want, cuthead(s, at));
        }
        expect(t, concat(&a, "missing ", p), cfgfile(v.os, p), want);
        expect(t, concat(&a, "missing.ok ", p), istr(&a, r.ok), "1");
        expect(t, concat(&a, "missing.diags ", p), inidiags(&a, m->ini), "");
        cfgfree(m);
    }

    // Created for the editor as the first edit would, the seed or nothing,
    // and one that exists is left alone
    for (i32 seed = 0; seed < 2; seed++) {
        Arena   tmp = v.scratch;
        Str     p   = seed ? Str("/cs/prospero.ini") : Str("/cn/prospero.ini");
        Config *m   = newconfig(v.os, &a, tmp, p, seed);
        os_remove(v.os, a, p);
        i64     rev = m->rev;
        CfgSet  r   = cfgcreate(m, &tmp);
        expect(t, concat(&a, "create ", p), cfgfile(v.os, p), seed ? cfgseed() : Str(""));
        expect(t, concat(&a, "create.ok ", p), istr(&a, r.ok), "1");
        expect(t, concat(&a, "create.user ", p), istr(&a, m->user != 0), "1");
        expect(t, concat(&a, "create.rev ", p), istr(&a, m->rev > rev), "1");
        os_write(v.os, a, p, "# mine\n");
        tmp = v.scratch;
        r   = cfgcreate(m, &tmp);
        expect(t, concat(&a, "create.kept ", p), cfgfile(v.os, p), "# mine\n");
        expect(t, concat(&a, "create.kept.ok ", p), istr(&a, r.ok), "1");
        cfgfree(m);
    }
    {
        Arena   tmp = v.scratch;
        Config *m   = newconfig(v.os, &a, tmp, {}, 0);
        CfgSet  r   = cfgcreate(m, &tmp);
        expect(t, "create.nowhere", r.err, "no configuration file location");
        cfgfree(m);
    }

    // Includes: a top-level setting an include overrides moves below it,
    // and removing what only an include sets says so
    {
        os_write(v.os, a, "/i/team.ini", "model = team-model\n"
                 "[endpoint team]\nurl = http://t/\nidle_timeout = 5\n");
        os_write(v.os, a, "/i/h.ini", "model = mine\ninclude = team.ini\n");
        Arena   tmp = v.scratch;
        Config *m   = newconfig(v.os, &a, tmp, "/i/h.ini", 0);
        expect(t, "include.before", cfgget(m, "", "", "model", 0), "team-model");
        CfgSet  r   = cfgset(m, &tmp, "", "", "model", "new-model");
        expect(t, "include.set", istr(&a, r.ok*10 + r.line), "12");
        expect(t, "include.set.text", cfgfile(v.os, "/i/h.ini"),
               "include = team.ini\nmodel = new-model\n");
        expect(t, "include.set.value", cfgget(m, "", "", "model", 0), "new-model");

        tmp = v.scratch;
        r = cfgset(m, &tmp, "", "", "model", {});
        expect(t, "include.remove", istr(&a, r.ok), "1");
        expect(t, "include.remove.value", cfgget(m, "", "", "model", 0), "team-model");
        tmp = v.scratch;
        r = cfgset(m, &tmp, "", "", "model", {});
        expect(t, "include.remove.again", r.err,
               "nothing to remove: /i/team.ini:1 sets it, not this file");
        tmp = v.scratch;
        r = cfgset(m, &tmp, "endpoint", "team", "idle_timeout", {});
        expect(t, "include.remove.section", r.err,
               "nothing to remove: /i/team.ini:4 sets it, not this file");
        expect(t, "include.untouched", cfgfile(v.os, "/i/team.ini"),
               "model = team-model\n[endpoint team]\nurl = http://t/\nidle_timeout = 5\n");
        cfgfree(m);
    }

    // A hand edit of the same size within the same mtime tick is not
    // lost to an edit made from the last load
    {
        os_write(v.os, a, "/t/h.ini", "[endpoint local]\nurl = http://l/\nidle_timeout = 90\n");
        Arena   tmp = v.scratch;
        Config *m   = newconfig(v.os, &a, tmp, "/t/h.ini", 0);
        os_write(v.os, a, "/t/h.ini", "[endpoint local]\nurl = http://l/\nidle_timeout = 60\n");
        CfgSet  r   = cfgset(m, &tmp, "endpoint", "local", "connect_timeout", "3");
        expect(t, "tick.ok", istr(&a, r.ok), "1");
        expect(t, "tick.text", cfgfile(v.os, "/t/h.ini"),
               "[endpoint local]\nurl = http://l/\nidle_timeout = 60\nconnect_timeout = 3\n");
        cfgfree(m);
    }
}

// Deleting sections: only the user's own, never an include's, saying
// where one is defined when it cannot go
static void testcfgdelete(Test *t)
{
    Arena  a    = t->perm;
    CfgEnv v    = newcfgenv(&a);
    Str    team = "# shared\n[endpoint team]\nurl = http://t/\n\n"
                  "[model both]\ntemperature = 0.1\n";
    os_write(v.os, a, "/d/team.ini", team);
    os_write(v.os, a, "/d/h.ini",
        "# My settings\n"
        "include = team.ini\n"
        "\n"
        "# The local server\n"
        "[endpoint local]\n"
        "url = http://localhost/\n"
        "\n"
        "# Mine too\n"
        "[model both]\n"
        "top_k = 20\n"
        "\n"
        "[guard]\n"
        "ask_timeout = 9\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/d/h.ini", 0);
    expect(t, "del.clean", inidiags(&a, c->ini), "");

    // Refusals leave the file alone
    Str before = cfgfile(v.os, "/d/h.ini");
    static struct { Str kind, name, err; } bad[] = {
        {"endpoint", "team", "[endpoint team] is defined in /d/team.ini:2, an "
         "included file, not in /d/h.ini"},
        {"endpoint", "none", "[endpoint none] is not in /d/h.ini"},
        {"", "", "the top level is not a section to delete"},
        {"provider", "x", "unknown section kind"},
        {"endpoint", "", "this kind of section needs a name"},
        {"guard", "g", "this kind of section has no name"},
        {"model", "a b", "[model a b] is not in /d/h.ini"},
    };
    for (iz i = 0; i < countof(bad); i++) {
        Arena  tmp = v.scratch;
        CfgSet r   = cfgdelete(c, &tmp, bad[i].kind, bad[i].name);
        Str    name = concat(&a, "del.refuse ", concat(&a, bad[i].kind, bad[i].name));
        expect(t, name, r.err, bad[i].err);
        expect(t, concat(&a, name, " (ok)"), istr(&a, r.ok), "0");
    }
    expect(t, "del.refuse.file", cfgfile(v.os, "/d/h.ini"), before);

    // The user's own: gone with the comment above it
    Arena  tmp = v.scratch;
    CfgSet r   = cfgdelete(c, &tmp, "endpoint", "local");
    expect(t, "del.local", istr(&a, r.ok*10 + r.line), "10");
    expect(t, "del.local.gone", istr(&a, inisection(c->ini, "endpoint", "local") != 0), "0");
    expect(t, "del.local.text", cfgfile(v.os, "/d/h.ini"),
        "# My settings\n"
        "include = team.ini\n"
        "\n"
        "# Mine too\n"
        "[model both]\n"
        "top_k = 20\n"
        "\n"
        "[guard]\n"
        "ask_timeout = 9\n");

    // One an include also defines stays, as the include has it
    tmp = v.scratch;
    r   = cfgdelete(c, &tmp, "model", "both");
    expect(t, "del.both", istr(&a, r.ok), "1");
    expect(t, "del.both.kept", cfgget(c, "model", "both", "temperature", 0), "0.1");
    expect(t, "del.both.mine", cfgget(c, "model", "both", "top_k", 0), "");
    tmp = v.scratch;
    r   = cfgdelete(c, &tmp, "model", "both");
    expect(t, "del.both.again", r.err, "[model both] is defined in "
           "/d/team.ini:5, an included file, not in /d/h.ini");

    // A kind without names, the file's last section
    tmp = v.scratch;
    r   = cfgdelete(c, &tmp, "guard", "");
    expect(t, "del.guard", istr(&a, r.ok), "1");
    expect(t, "del.guard.value", cfgget(c, "guard", "", "ask_timeout", 0), "30");
    expect(t, "del.text", cfgfile(v.os, "/d/h.ini"),
           "# My settings\ninclude = team.ini\n");
    expect(t, "del.team", cfgfile(v.os, "/d/team.ini"), team);
    expect(t, "del.diags", inidiags(&a, c->ini), "");
    cfgfree(c);

    // A hand edit not yet loaded is what the deletion edits
    os_write(v.os, a, "/d/k.ini", "[role a]\nprompt = x\n\n[role b]\nprompt = y\n");
    tmp = v.scratch;
    c = newconfig(v.os, &a, tmp, "/d/k.ini", 0);
    os_write(v.os, a, "/d/k.ini", "[role a]\nprompt = z\n\n[role b]\nprompt = y\n");
    r = cfgdelete(c, &tmp, "role", "b");
    expect(t, "del.tick", cfgfile(v.os, "/d/k.ini"), "[role a]\nprompt = z\n");
    cfgfree(c);
}

static void testcfgpoll(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    v.os->now = 100 * 1000000;
    Str head = "include = t.ini\ninclude = later.ini\n[endpoint e]\n";
    Str ini = concat(&a, head, "url = http://e/\n");
    os_write(v.os, a, "/c/h.ini", ini);
    os_write(v.os, a, "/c/t.ini", "[model m]\ntemperature = 1\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    expect(t, "poll.watch", showlist(&a, c->watch),
           "/c/h.ini|/c/t.ini|/c/later.ini");

    Str steps[] = {
        "rate limited", "unchanged", "user file", "include", "new include",
        "too soon", "after a second", "forced", "removed",
    };
    i32 got[countof(steps)] = {};
    got[0] = cfgpoll(c, v.scratch, 0);

    v.os->now += 2000000;
    got[1] = cfgpoll(c, v.scratch, 0);

    ini = concat(&a, head, "url = http://f/\n");
    os_write(v.os, a, "/c/h.ini", ini);
    v.os->now += 2000000;
    got[2] = cfgpoll(c, v.scratch, 0);
    expect(t, "poll.url", cfgget(c, "endpoint", "e", "url", 0), "http://f/");

    os_write(v.os, a, "/c/t.ini", "[model m]\ntemperature = 0.5\n");
    v.os->now += 2000000;
    got[3] = cfgpoll(c, v.scratch, 0);
    expect(t, "poll.include", cfgget(c, "model", "m", "temperature", 0), "0.5");

    os_write(v.os, a, "/c/later.ini", "model = late\n");
    v.os->now += 2000000;
    got[4] = cfgpoll(c, v.scratch, 0);
    expect(t, "poll.later", cfgget(c, "", "", "model", 0), "late");

    os_write(v.os, a, "/c/later.ini", "model = later\n");
    v.os->now += 500000;
    got[5] = cfgpoll(c, v.scratch, 0);
    v.os->now += 600000;
    got[6] = cfgpoll(c, v.scratch, 0);
    expect(t, "poll.later2", cfgget(c, "", "", "model", 0), "later");

    os_write(v.os, a, "/c/later.ini", "model = now\n");
    got[7] = cfgpoll(c, v.scratch, 1);
    expect(t, "poll.forced", cfgget(c, "", "", "model", 0), "now");

    os_remove(v.os, a, "/c/h.ini");
    v.os->now += 2000000;
    got[8] = cfgpoll(c, v.scratch, 0);
    expect(t, "poll.gone", istr(&a, c->user != 0), "0");
    expect(t, "poll.defaults", cfgget(c, "", "", "model", 0), "");

    i32 want[countof(steps)] = {0, 0, 1, 1, 1, 0, 1, 1, 1};
    for (iz i = 0; i < countof(steps); i++) {
        expect(t, concat(&a, "poll ", steps[i]), istr(&a, got[i]),
               istr(&a, want[i]));
    }
    expect(t, "poll.rev", istr(&a, c->rev), "7");
    cfgfree(c);
}

static Str cfgjson(Arena *a, Config *c, CfgSet *r)
{
    Buf     b(a, 1<<12);
    JsonOut w = {&b};
    putobject(&w);
    if (r) {
        putcfgset(&w, c, r);
    } else {
        putcfg(&w, c);
    }
    putend(&w);
    return finish(&b);
}

static void testcfgjson(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    os_write(v.os, a, "/c/team.ini", "[endpoint team]\nurl = https://t/\n"
             "key = plain\n");
    os_write(v.os, a, "/c/h.ini",
             "include = team.ini\n"
             "[endpoint local]\n"
             "url = http://127.0.0.1:8080/\n"
             "idle_timeout = 30\n"
             "key = env:KEY\n"
             "[model m]\n"
             "top_k = many\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);

    // The settings, compactly: kind/name/key=value@file:line, with *
    // marking defaults and ! values ignored
    JsonDoc d = jsonparse(&a, cfgjson(&a, c, 0));
    expect(t, "json.ok", istr(&a, d.ok), "1");
    Json *j = d.root;
    expect(t, "json.path", jsonstr(jsonget(j, "path")), "/c/h.ini");
    expect(t, "json.exists", istr(&a, jsonint(jsonget(j, "exists"), -1)), "1");
    expect(t, "json.rev", istr(&a, jsonint(jsonget(j, "rev"), -1)), "1");
    expect(t, "json.files", istr(&a, jsonlen(jsonget(j, "files"))), "2");
    Buf b(&a, 1024);
    for (Json *s = jsonget(j, "settings")->child; s; s = s->next) {
        print(&b, jsonstr(jsonget(s, "kind")));
        putbyte(&b, '/');
        print(&b, jsonstr(jsonget(s, "name")));
        putbyte(&b, '/');
        print(&b, jsonstr(jsonget(s, "key")));
        putbyte(&b, '=');
        print(&b, jsonstr(jsonget(s, "value")));
        if (jsonint(jsonget(s, "default"), -1)) {
            putbyte(&b, '*');
        } else {
            putbyte(&b, '@');
            print(&b, jsonstr(jsonget(s, "file")));
            putbyte(&b, ':');
            print(&b, jsonint(jsonget(s, "line"), -1));
        }
        if (!jsonint(jsonget(s, "valid"), -1)) {
            putbyte(&b, '!');
        }
        putbyte(&b, '\n');
    }
    expect(t, "json.settings", finish(&b),
        "//endpoint=*\n"
        "//model=*\n"
        "endpoint/team/url=https://t/@/c/team.ini:2\n"
        "endpoint/team/kind=openai*\n"
        "endpoint/team/key=(hidden)@/c/team.ini:3!\n"
        "endpoint/team/connect_timeout=15*\n"
        "endpoint/team/first_byte_timeout=300*\n"
        "endpoint/team/idle_timeout=90*\n"
        "endpoint/team/progress_timeout=600*\n"
        "endpoint/team/slow_retry=60 120 300 600 900*\n"
        "endpoint/team/slow_retry_budget=7200*\n"
        "endpoint/team/max_concurrent=*\n"
        "endpoint/team/max_models=*\n"
        "endpoint/local/url=http://127.0.0.1:8080/@/c/h.ini:3\n"
        "endpoint/local/kind=openai*\n"
        "endpoint/local/key=env:KEY@/c/h.ini:5\n"
        "endpoint/local/connect_timeout=5*\n"
        "endpoint/local/first_byte_timeout=1800*\n"
        "endpoint/local/idle_timeout=30@/c/h.ini:4\n"
        "endpoint/local/progress_timeout=600*\n"
        "endpoint/local/slow_retry=60 120 300 600 900*\n"
        "endpoint/local/slow_retry_budget=7200*\n"
        "endpoint/local/max_concurrent=*\n"
        "endpoint/local/max_models=*\n"
        "model/m/like=*\n"
        "model/m/id=m*\n"
        "model/m/reasoning=reasoning_content*\n"
        "model/m/context=*\n"
        "model/m/compactor=*\n"
        "model/m/fallback=*\n"
        "model/m/fallback_cooldown=600*\n"
        "model/m/subagent_model=*\n"
        "model/m/reasoning_budget=*\n"
        "model/m/thinking=adaptive*\n"
        "model/m/thinking_display=summarized*\n"
        "model/m/effort=*\n"
        "model/m/vision=true*\n"
        "model/m/coordinates=pixels*\n"
        "model/m/image_edge=*\n"
        "model/m/image_pixels=*\n"
        "model/m/edit_format=replace*\n"
        "model/m/empty_retries=2*\n"
        "model/m/max_concurrent=*\n"
        "model/m/temperature=*\n"
        "model/m/top_p=*\n"
        "model/m/top_k=many@/c/h.ini:7!\n"
        "model/m/min_p=*\n"
        "model/m/presence_penalty=*\n"
        "model/m/frequency_penalty=*\n"
        "model/m/repeat_penalty=*\n"
        "model/m/seed=*\n"
        "model/m/max_tokens=*\n"
        "guard//roots=*\n"
        "guard//ask_timeout=30*\n"
        "guard//temp=allow*\n"
        "tools//job_timeout=1800*\n"
        "tools//job_timeout_max=7200*\n"
        "tools//auto_resume=5*\n"
        "tools//stop_grace=*\n"
        "tools//shell=auto*\n"
        "tools//shells=*\n"
        "tools//path_prepend=*\n"
        "tools//path_append=*\n"
        "tools//use_rg=true*\n"
        "tools//git=*\n"
        "tools//turn_budget=0*\n"
        "tools//task_budget=200*\n"
        "instructions//files=AGENTS.md CLAUDE.md*\n"
        "skills//enabled=true*\n"
        "skills//paths=*\n"
        "memory//enabled=true*\n"
        "memory//path=*\n"
        "ui//theme=system*\n"
        "ui//thinking=fold*\n"
        "ui//tools=auto*\n"
        "ui//font_size=17*\n"
        "ui//editor=*\n"
        "ui//terminal=*\n"
        "ui//notify=allow done*\n"
        "desktop//offer=all*\n"
        "desktop//ask=true*\n"
        "desktop//ask_timeout=60*\n"
        "desktop//images=3*\n"
        "desktop//images_slack=4*\n"
        "web//search=none*\n"
        "web//url=*\n"
        "web//key=*\n"
        "web//model=*\n"
        "web//fetch=true*\n"
        "web//ask=true*\n"
        "web//results=8*\n"
        "web//searches=200*\n"
        "host//idle_exit=600*\n"
        "host//grace=10800*\n");

    Json *schema = jsonget(j, "schema");
    expect(t, "json.schema", istr(&a, jsonlen(schema)),
           istr(&a, countof(cfgschema)));
    Json *ft = jsonat(schema, 6);
    expect(t, "json.schema.key", jsonstr(jsonget(ft, "key")),
           "first_byte_timeout");
    expect(t, "json.schema.type", jsonstr(jsonget(ft, "type")), "seconds");
    expect(t, "json.schema.default", jsonstr(jsonget(ft, "default")), "300");
    expect(t, "json.schema.local", jsonstr(jsonget(ft, "local")), "1800");
    iz    ti    = cfgkey("role", "tools") - cfgschema;
    Json *tools = jsonget(jsonat(schema, ti), "choices");
    expect(t, "json.schema.choices", istr(&a, jsonlen(tools)), "20");
    expect(t, "json.kinds", istr(&a, jsonlen(jsonget(j, "kinds"))),
           istr(&a, countof(cfgkinds)));
    Buf hb(&a, 256);
    for (Json *h = jsonget(j, "headers")->child; h; h = h->next) {
        print(&hb, jsonstr(jsonget(h, "kind")));
        putbyte(&hb, '/');
        print(&hb, jsonstr(jsonget(h, "name")));
        putbyte(&hb, '@');
        print(&hb, jsonstr(jsonget(h, "file")));
        putbyte(&hb, ':');
        print(&hb, jsonint(jsonget(h, "line"), -1));
        print(&hb, jsonint(jsonget(h, "own"), -1) ? Str(" own\n") : Str("\n"));
    }
    expect(t, "json.headers", finish(&hb),
           "endpoint/local@/c/h.ini:2 own\n"
           "model/m@/c/h.ini:6 own\n"
           "endpoint/team@/c/team.ini:1\n");

    Json *diag = jsonget(j, "diags")->child;
    expect(t, "json.diags", istr(&a, jsonlen(jsonget(j, "diags"))), "3");
    expect(t, "json.diag.file", jsonstr(jsonget(diag, "file")), "/c/h.ini");
    expect(t, "json.diag.line", istr(&a, jsonint(jsonget(diag, "line"), -1)),
           "7");
    expect(t, "json.diag.col", istr(&a, jsonint(jsonget(diag, "col"), -1)),
           "9");
    expect(t, "json.diag.severity", jsonstr(jsonget(diag, "severity")),
           "error");
    expect(t, "json.diag.message", jsonstr(jsonget(diag, "message")),
           "expected a whole number, so this is ignored");

    // A config.set payload, exactly
    Arena  tmp = v.scratch;
    CfgSet r   = cfgset(c, &tmp, "model", "m", "top_k", {});
    expect(t, "json.set", cfgjson(&a, c, &r),
        "{\"path\":\"/c/h.ini\",\"line\":0,\"rev\":2,\"diags\":["
        "{\"file\":\"/c/team.ini\",\"line\":3,\"col\":7,\"severity\":\"error\","
        "\"message\":\"a plaintext secret in an included file is ignored: "
        "write env:NAME and set the variable\"},"
        "{\"file\":\"/c/h.ini\",\"line\":5,\"col\":7,\"severity\":\"warning\","
        "\"message\":\"environment variable KEY is not set\"}]}");
    cfgfree(c);

    // A key set empty means its default: shown as the default, from
    // where it is set
    os_write(v.os, a, "/e/h.ini", "[endpoint local]\nurl = http://localhost/\n"
             "idle_timeout =   # fill in later\n");
    c = newconfig(v.os, &a, v.scratch, "/e/h.ini", 0);
    d = jsonparse(&a, cfgjson(&a, c, 0));
    Json *idle = 0;
    for (Json *s = jsonget(d.root, "settings")->child; s; s = s->next) {
        if (jsonstr(jsonget(s, "key")) == "idle_timeout") {
            idle = s;
        }
    }
    Buf eb(&a, 128);
    print(&eb, jsonstr(jsonget(idle, "value")));
    putbyte(&eb, ' ');
    print(&eb, jsonint(jsonget(idle, "default"), -1));
    putbyte(&eb, ' ');
    print(&eb, jsonstr(jsonget(idle, "file")));
    putbyte(&eb, ':');
    print(&eb, jsonint(jsonget(idle, "line"), -1));
    expect(t, "json.empty", finish(&eb), "90 1 /e/h.ini:3");
    cfgfree(c);
}

// The engine resolves endpoints and models through the configuration,
// with the schema's defaults.
static void testcfgengine(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "LOCALKEY=abc");
    os_write(v.os, a, "/c/h.ini",
             "[endpoint local]\n"
             "url = http://localhost:8080/\n"
             "idle_timeout = 7\n"
             "key = env:LOCALKEY\n"
             "[endpoint slow]\n"
             "url = http://slow.example/\n"
             "connect_timeout = 3000000\n"
             "[model m]\n"
             "reasoning = none\n"
             "temperature = 0.5\n"
             "top_k = x\n"
             "context = 8192\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    Engine *e = alloc<Engine>(&a);
    e->os  = v.os;
    e->cfg = c;
    e->ini = c->ini;

    // name url first_byte idle progress connect_ms key
    static struct { Str which, want; } eps[] = {
        {"local", "local http://localhost:8080/ 1800 7 600 5000 abc"},
        {"http://localhost:8080/",
         "local http://localhost:8080/ 1800 7 600 5000 abc"},
        {"https://remote.example/",
         " https://remote.example/ 300 90 600 15000 "},
        {"http://192.168.1.5/", " http://192.168.1.5/ 1800 90 600 5000 "},
        // Valid seconds, but more milliseconds than an i32 holds
        {"slow", "slow http://slow.example/ 300 90 600 2147483647 "},
        {"nope", "(none)"},
    };
    for (iz i = 0; i < countof(eps); i++) {
        Endpoint ep = {};
        Str      got = "(none)";
        if (endpointconf(e, &a, eps[i].which, &ep)) {
            Buf b(&a, 128);
            print(&b, ep.name);
            putbyte(&b, ' ');
            print(&b, ep.url);
            putbyte(&b, ' ');
            print(&b, ep.firstbyte / SEC);
            putbyte(&b, ' ');
            print(&b, ep.idle / SEC);
            putbyte(&b, ' ');
            print(&b, ep.progress / SEC);
            putbyte(&b, ' ');
            print(&b, (i64)ep.connectms);
            putbyte(&b, ' ');
            print(&b, ep.key);
            got = finish(&b);
        }
        expect(t, concat(&a, "endpoint ", eps[i].which), got, eps[i].want);
    }

    static struct { Str model, want; } models[] = {
        {"m", "[] temperature=0.5 ctx=8192"},
        {"other", "[reasoning_content] ctx=0"},
    };
    for (iz i = 0; i < countof(models); i++) {
        ModelConf mc = {};
        modelconf(e, &a, models[i].model, &mc);
        Buf b(&a, 128);
        putbyte(&b, '[');
        print(&b, mc.reasoning);
        putbyte(&b, ']');
        for (iz k = 0; k < mc.extras.len; k++) {
            putbyte(&b, ' ');
            print(&b, mc.extras[k].key);
            putbyte(&b, '=');
            print(&b, mc.extras[k].json);
        }
        print(&b, " ctx=");
        print(&b, mc.ctx);
        expect(t, concat(&a, "model ", models[i].model), finish(&b),
               models[i].want);
    }

    // A session whose endpoint and model sections are deleted keeps the
    // settings it had, as another reload finds them still gone
    Session *s = alloc<Session>(&a);
    s->e        = e;
    s->perm     = carve(&a, 1<<20);
    s->model    = "m";
    s->cwd      = "/w";
    endpointconf(e, &s->perm, "local", &s->ep);
    modelconf(e, &s->perm, s->model, &s->mc);
    e->sessions = s;
    for (i32 i = 0; i < 2; i++) {
        Arena tmp = v.scratch;
        cfgdelete(c, &tmp, "model", "m");
        tmp = v.scratch;
        cfgdelete(c, &tmp, "endpoint", "local");
        reconfigure(e, v.scratch);
        Buf b(&a, 128);
        print(&b, s->ep.url);
        putbyte(&b, ' ');
        print(&b, s->ep.idle / SEC);
        putbyte(&b, ' ');
        print(&b, s->mc.name);
        print(&b, " ctx=");
        print(&b, s->mc.ctx);
        print(&b, " extras=");
        print(&b, s->mc.extras.len);
        expect(t, "deleted.kept", finish(&b),
               "http://localhost:8080/ 7 m ctx=8192 extras=1");
    }
    expect(t, "deleted.gone", cfgfile(v.os, "/c/h.ini"),
           "[endpoint slow]\nurl = http://slow.example/\n"
           "connect_timeout = 3000000\n");
    e->sessions = 0;
    cfgfree(c);
}

// Run a live Config's fetches to their ends, as the engine does.
static void cfgdrain(Config *c, Arena scratch)
{
    for (i32 i = 0; i<100 && cfgbusy(c); i++) {
        Mail m = mailtake(c->mbox);
        for (Record r; (r = mailnext(&m)).ok;) {
            cfgrecord(c, scratch, r);
        }
        cfgtimeout(c, scratch);
    }
}

static Str cfgcached(Arena *a, Os *os, Str url)
{
    return cfgfile(os, concat(a, "/s/includes/", cfgsha256(a, url)));
}

// The includes of a config.get, one per line: url fetched fetching
// error.
static Str cfgshowincludes(Arena *a, Config *c)
{
    JsonDoc d = jsonparse(a, cfgjson(a, c, 0));
    Buf     b(a, 256);
    for (Json *i = jsonget(d.root, "includes")->child; i; i = i->next) {
        print(&b, b.len ? Str("\n") : Str(""));
        print(&b, jsonstr(jsonget(i, "url")));
        putbyte(&b, ' ');
        print(&b, jsonint(jsonget(i, "fetched"), -1));
        putbyte(&b, ' ');
        print(&b, jsonint(jsonget(i, "fetching"), -1));
        putbyte(&b, ' ');
        print(&b, jsonstr(jsonget(i, "error")));
    }
    return finish(&b);
}

static void testcfgsha256(Test *t)
{
    Arena a = t->perm;
    Str   m = {allocbytes(&a, 1000000), 1000000};
    for (iz i = 0; i < m.len; i++) {
        m[i] = 'a';
    }
    static struct { Str in, want; } cases[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        expect(t, concat(&a, "sha256 ", cases[i].in),
               cfgsha256(&a, cases[i].in), cases[i].want);
    }
    static struct { iz len; Str want; } as[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (iz i = 0; i < countof(as); i++) {
        expect(t, concat(&a, "sha256 a*", istr(&a, as[i].len)),
               cfgsha256(&a, takehead(m, as[i].len)), as[i].want);
    }

    static struct { i64 ms; Str want; } ages[] = {
        {-5, "0 seconds"}, {999, "0 seconds"}, {1000, "1 second"},
        {59999, "59 seconds"}, {60000, "1 minute"}, {7199999, "1 hour"},
        {7200000, "2 hours"}, {86400000, "1 day"}, {10*86400000ll, "10 days"},
    };
    for (iz i = 0; i < countof(ages); i++) {
        Buf b(&a, 32);
        printcfgage(&b, ages[i].ms);
        expect(t, concat(&a, "age ", ages[i].want), finish(&b), ages[i].want);
    }
}

// URL includes: fetched at startup, cached, refreshed with conditional
// requests, used offline with their age, pinned, redirected and capped.
static void testcfgurls(Test *t)
{
    Arena  a  = t->perm;
    CfgEnv v  = newcfgenv(&a);
    Os    *os = v.os;
    i64    s  = 1000000;  // a second of os_ticks
    os->now = 1000*s;
    Str url  = "https://team.example/h.ini";
    Str more = "https://team.example/more.ini";
    Str team = "include = more.ini\n"
               "[endpoint team]\n"
               "url = https://team.example/v1\n"
               "key = sk-plaintext\n";
    Str lm   = "Last-Modified: Wed, 21 Oct 2026 07:28:00 GMT\r\n";
    VRoute *r1 = vroute(os, url, 200, lm, team);
    VRoute *r2 = vroute(os, more, 200, "", "model = team-model\n");
    r1->etag = "\"v1\"";
    r2->etag = "\"m1\"";
    VRoute *routes = os->routes;
    Str user = "include = HTTPS://Team.example/./h.ini\n"
               "[endpoint local]\n"
               "url = http://localhost/\n";
    os_write(os, a, "/c/h.ini", user);
    Str secret = "https://team.example/h.ini:4:7: error: a plaintext secret "
                 "in an included file is ignored: write env:NAME and set the "
                 "variable";

    // First run: fetched while starting, the nested include too
    Config *c = newconfig(os, &a, v.scratch, "/c/h.ini", 0, "/s", 1);
    expect(t, "url.requests", istr(&a, os->requests), "2");
    expect(t, "url.model", cfgget(c, "", "", "model", 0), "team-model");
    expect(t, "url.team", cfgget(c, "endpoint", "team", "url", 0),
           "https://team.example/v1");
    expect(t, "url.diags", inidiags(&a, c->ini), secret);
    {
        Slice<Str> files = {};
        for (IniFile *f = c->ini->files; f; f = f->next) {
            files = push(&a, files, f->path);
        }
        expect(t, "url.files", showlist(&a, files), concat(&a,
               "/c/h.ini|https://team.example/h.ini|", more));
    }
    expect(t, "url.cache", cfgcached(&a, os, url), concat(&a,
           "prospero include 1\nurl https://team.example/h.ini\n"
           "fetched 1000000\netag \"v1\"\n"
           "modified Wed, 21 Oct 2026 07:28:00 GMT\n\n", team));
    expect(t, "url.cachename", cfgsha256(&a, url),
           "e0f300c06ecf61a441d54e8044994313844a27de962dafcf38176916c5e44bd6");
    expect(t, "url.includes", cfgshowincludes(&a, c),
           "https://team.example/h.ini 1000000 0 \n"
           "https://team.example/more.ini 1000000 0 ");
    {
        // Provenance: the URL, and when its copy was fetched
        JsonDoc d = jsonparse(&a, cfgjson(&a, c, 0));
        Str     got = {};
        for (Json *e = jsonget(d.root, "settings")->child; e; e = e->next) {
            if (jsonstr(jsonget(e, "name"))=="team" &&
                    jsonstr(jsonget(e, "key"))=="url") {
                Buf b(&a, 128);
                print(&b, jsonstr(jsonget(e, "file")));
                putbyte(&b, ':');
                print(&b, jsonint(jsonget(e, "line"), -1));
                putbyte(&b, ' ');
                print(&b, jsonint(jsonget(e, "fetched"), -1));
                got = finish(&b);
            }
        }
        expect(t, "url.provenance", got, "https://team.example/h.ini:3 1000000");
    }
    cfgfree(c);

    // Later runs load from the cache at once, then refresh in the
    // background with conditional requests
    os->now += 60*s;
    c = newconfig(os, &a, v.scratch, "/c/h.ini", 0, "/s", 1);
    expect(t, "cached.requests", istr(&a, os->requests), "2");
    expect(t, "cached.model", cfgget(c, "", "", "model", 0), "team-model");
    expect(t, "cached.diags", inidiags(&a, c->ini), secret);
    Mailbox *mb = newmailbox(&a, 1<<20, 0, 0);
    cfgattach(c, mb, CFG_REFRESH);
    i64 rev = c->rev;
    cfgrefresh(c, v.scratch, 0);
    expect(t, "refresh.busy", istr(&a, cfgbusy(c)), "1");
    cfgdrain(c, v.scratch);
    expect(t, "refresh.requests", istr(&a, os->requests), "4");
    expect(t, "refresh.conditional",
           istr(&a, find(os->request, "If-None-Match: \"m1\"\r\n") >= 0), "1");
    expect(t, "refresh.unchanged", istr(&a, c->rev - rev), "0");
    expect(t, "refresh.fetched",
           istr(&a, find(cfgcached(&a, os, url), "\nfetched 1060000\n")>0), "1");
    expect(t, "refresh.includes", cfgshowincludes(&a, c),
           "https://team.example/h.ini 1060000 0 \n"
           "https://team.example/more.ini 1060000 0 ");

    // Each at most every interval, unless forced
    cfgrefresh(c, v.scratch, 0);
    cfgdrain(c, v.scratch);
    expect(t, "interval.none", istr(&a, os->requests), "4");
    os->now += CFG_REFRESH;
    cfgpoll(c, v.scratch, 0);
    cfgdrain(c, v.scratch);
    expect(t, "interval.due", istr(&a, os->requests), "6");
    cfgrefresh(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "interval.forced", istr(&a, os->requests), "8");

    // Offline: the cached copies, with a warning of their age
    os->routes = 0;
    os->now += 3*3600*s;
    cfgrefresh(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "offline.model", cfgget(c, "", "", "model", 0), "team-model");
    expect(t, "offline.diags", inidiags(&a, c->ini), concat(&a,
           "/c/h.ini:1:11: warning: cannot fetch https://team.example/h.ini "
           "(not supported in tests); using the copy fetched 3 hours ago\n"
           "https://team.example/h.ini:1:11: warning: cannot fetch "
           "https://team.example/more.ini (not supported in tests); using "
           "the copy fetched 3 hours ago\n", secret));
    expect(t, "offline.includes", cfgshowincludes(&a, c),
           "https://team.example/h.ini 1660000 0 not supported in tests\n"
           "https://team.example/more.ini 1660000 0 not supported in tests");
    os->routes = routes;
    cfgrefresh(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "online.diags", inidiags(&a, c->ini), secret);

    // A pin the cached copy does not match: fetched afresh, which does
    // not match either, so the include is skipped, and the cache kept
    Str zeros = "0000000000000000000000000000000000000000000000000000000000000000";
    Str sha   = cfgsha256(&a, team);
    Str copy  = cfgcached(&a, os, url);
    Str ini = concat(&a, concat(&a,
             "include = https://team.example/h.ini sha256=", zeros), "\n");
    os_write(os, a, "/c/h.ini", ini);
    i32 before = os->requests;
    cfgpoll(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "pin.requests", istr(&a, os->requests - before), "1");
    expect(t, "pin.unconditional",
           istr(&a, find(os->request, "If-None-Match") < 0), "1");
    expect(t, "pin.diags", inidiags(&a, c->ini), concat(&a, concat(&a,
           "/c/h.ini:1:11: error: https://team.example/h.ini does not match "
           "its pin: its sha256 is ", sha), ", include skipped"));
    expect(t, "pin.model", cfgget(c, "", "", "model", 0), "");
    expect(t, "pin.cache", cfgcached(&a, os, url), copy);

    // The right pin: the cached copy, with no fetch
    before = os->requests;
    os->now += s;  // a new mtime: the edit is the same size
    ini = concat(&a, concat(&a,
             "include = https://team.example/h.ini sha256=", sha), "\n");
    os_write(os, a, "/c/h.ini", ini);
    cfgpoll(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "pinned.requests", istr(&a, os->requests - before), "0");
    expect(t, "pinned.diags", inidiags(&a, c->ini), secret);
    expect(t, "pinned.model", cfgget(c, "", "", "model", 0), "team-model");

    // The server's text changes under the pin: an error, and the copy
    // that matches stays in use
    Str evil = "[endpoint team]\nurl = https://evil.example/\n";
    r1->body = evil;
    r1->etag = "\"v2\"";
    cfgrefresh(c, v.scratch, 1);
    cfgdrain(c, v.scratch);
    expect(t, "pinbad.diags", inidiags(&a, c->ini), concat(&a, concat(&a,
           "/c/h.ini:1:11: error: https://team.example/h.ini does not match "
           "its pin: its sha256 is ", cfgsha256(&a, evil)),
           concat(&a, "; using the copy fetched 1 second ago\n", secret)));
    expect(t, "pinbad.url", cfgget(c, "endpoint", "team", "url", 0),
           "https://team.example/v1");
    expect(t, "pinbad.cache", istr(&a, find(cfgcached(&a, os, url), team)>0), "1");
    expect(t, "pinbad.text", istr(&a, find(cfgcached(&a, os, url), evil)), "-1");
    cfgfree(c);
    r1->body = team;
    r1->etag = "\"v1\"";

    // Redirects: five are followed, relative or not; six are too many,
    // and https never goes to http
    Str hops[][2] = {
        {"https://r.example/0", "/1"},
        {"https://r.example/1", "https://r.example/2"},
        {"https://r.example/2", "3"},
        {"https://r.example/3", "//r.example/4"},
        {"https://r.example/4", "./5?x"},
        {"https://r.example/s", "/0"},
        {"https://r.example/down", "http://r.example/5?x"},
    };
    i32 codes[] = {302, 301, 307, 308, 303, 302, 302};
    for (iz i = 0; i < countof(hops); i++) {
        Str loc = concat(&a, concat(&a, "Location: ", hops[i][1]), "\r\n");
        vroute(os, hops[i][0], codes[i], loc, "moved\n");
    }
    vroute(os, "https://r.example/5?x", 200, "", "model = redirected\n");
    static struct { Str url, want; } redirects[] = {
        {"https://r.example/0", "redirected "},
        {"https://r.example/s", " /c/r.ini:1:11: warning: cannot fetch "
         "https://r.example/s (more than 5 redirects), include skipped"},
        {"https://r.example/down", " /c/r.ini:1:11: warning: cannot fetch "
         "https://r.example/down (redirected from https to http), include "
         "skipped"},
    };
    for (iz i = 0; i < countof(redirects); i++) {
        Str ini = concat(&a, concat(&a, "include = ", redirects[i].url), "\n");
        os_write(os, a, "/c/r.ini", ini);
        Config *r = newconfig(os, &a, v.scratch, "/c/r.ini", 0, "/s", 1);
        Str got = concat(&a, concat(&a, cfgget(r, "", "", "model", 0), " "),
                         inidiags(&a, r->ini));
        expect(t, concat(&a, "redirect ", redirects[i].url), got,
               redirects[i].want);
        cfgfree(r);
    }

    // Failures with no cached copy skip the include
    iz  big  = CFG_FETCHMAX + 1;
    Str huge = {allocbytes(&a, big), big};
    for (iz i = 0; i < big; i++) {
        huge[i] = '\n';
    }
    vroute(os, "https://f.example/big", 200, "", huge);
    vroute(os, "https://f.example/gone", 404, "", "not found\n");
    vroute(os, "https://f.example/reset", 200, "", {})->err = HTTP_EIO;
    static struct { Str url, why; } fails[] = {
        {"https://f.example/big", "larger than 1 MiB"},
        {"https://f.example/gone", "HTTP 404"},
        {"https://f.example/reset", "refused"},
        {"https://f.example/none", "not supported in tests"},
    };
    for (iz i = 0; i < countof(fails); i++) {
        Str ini = concat(&a, concat(&a, "include = ", fails[i].url), "\n");
        os_write(os, a, "/c/f.ini", ini);
        Config *f = newconfig(os, &a, v.scratch, "/c/f.ini", 0, "/s", 1);
        Str want = concat(&a, "/c/f.ini:1:11: warning: cannot fetch ",
                          fails[i].url);
        want = concat(&a, want, concat(&a, " (", fails[i].why));
        want = concat(&a, want, "), include skipped");
        expect(t, concat(&a, "fail ", fails[i].url), inidiags(&a, f->ini), want);
        expect(t, concat(&a, "fail.cache ", fails[i].url),
               cfgcached(&a, os, fails[i].url), "(missing)");
        cfgfree(f);
    }

    // Without fetching, cached copies only; without a state directory,
    // no URL includes
    os_write(os, a, "/c/n.ini", "include = https://team.example/more.ini\n"
             "include = https://team.example/new.ini\n");
    before = os->requests;
    c = newconfig(os, &a, v.scratch, "/c/n.ini", 0, "/s", 0);
    expect(t, "nonet.requests", istr(&a, os->requests - before), "0");
    expect(t, "nonet.model", cfgget(c, "", "", "model", 0), "team-model");
    expect(t, "nonet.diags", inidiags(&a, c->ini),
           "/c/n.ini:2:11: warning: https://team.example/new.ini is not "
           "fetched yet, include skipped");
    cfgattach(c, mb, CFG_REFRESH);
    cfgrefresh(c, v.scratch, 1);
    expect(t, "nonet.attach", istr(&a, os->requests - before), "0");
    cfgfree(c);
    c = newconfig(os, &a, v.scratch, "/c/n.ini", 0, {}, 1);
    expect(t, "nostate.diags", inidiags(&a, c->ini),
           "/c/n.ini:1:11: warning: cannot include "
           "https://team.example/more.ini (no state directory for a cache)\n"
           "/c/n.ini:2:11: warning: cannot include "
           "https://team.example/new.ini (no state directory for a cache)");
    cfgfree(c);
}

// Template arguments (template.NAME) and variants (like, id): typing,
// inheritance, cycles, edits, rows, and the requests that carry them.
static void testcfglike(Test *t)
{
    Arena a = t->perm;

    static struct { Str in; b32 want; } nums[] = {
        {"0", 1}, {"25", 1}, {"-3", 1}, {"0.5", 1}, {"1e9", 1}, {"2.5E-3", 1},
        {"-0", 1}, {"007", 0}, {".5", 0}, {"1.", 0}, {"1e", 0}, {"+1", 0},
        {"-", 0}, {"", 0}, {"0x10", 0}, {"1 2", 0}, {"NaN", 0},
    };
    for (iz i = 0; i < countof(nums); i++) {
        expect(t, concat(&a, "number ", nums[i].in),
               istr(&a, cfgjsonnumber(nums[i].in)), istr(&a, nums[i].want));
    }
    static struct { Str key, want; } names[] = {
        {"template.thinking", "thinking"}, {"template._x9", "_x9"},
        {"template.", ""}, {"template.9x", ""}, {"template.a-b", ""},
        {"template.a.b", ""}, {"templatex", ""},
    };
    for (iz i = 0; i < countof(names); i++) {
        expect(t, concat(&a, "name ", names[i].key),
               cfgtemplatename(names[i].key), names[i].want);
    }

    CfgEnv v = newcfgenv(&a);
    os_write(v.os, a, "/c/team.ini",
             "[model shared]\n"
             "template.from_team = yes\n");
    os_write(v.os, a, "/c/h.ini",
             "include = team.ini\n"
             "[model large]\n"
             "top_p = 0.95\n"
             "max_tokens = 32768\n"
             "fallback = small\n"                  // 5
             "template.thinking = true\n"
             "template.reasoning_effort = 50\n"
             "template.label = \"42\"\n"
             "template.mode = fast # note\n"
             "template.empty = \"\"\n"             // 10
             "[model large-quick]\n"
             "like = large\n"
             "template.reasoning_effort = 25\n"
             "template.mode =\n"
             "fallback =\n"                        // 15
             "top_p = warm\n"
             "[model large-deep]\n"
             "like = large-quick\n"
             "id = deepseek\n"
             "template.reasoning_effort = 100\n"   // 20
             "template.extra = 1.5\n"
             "[model loop-a]\n"
             "like = loop-b\n"
             "temperature = 0.1\n"
             "[model loop-b]\n"                    // 25
             "like = loop-a\n"
             "template.x = 1\n"
             "[model self]\n"
             "like = self\n"
             "[model into]\n"                      // 30
             "like = loop-a\n"
             "[model bad]\n"
             "template.9lives = 1\n"
             "template.a-b = 2\n"
             "[model nosection]\n"                 // 35
             "like = unlisted\n"
             "[model team]\n"
             "like = shared\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    expect(t, "like.diags", inidiags(&a, c->ini),
           "/c/h.ini:16:9: error: expected a number, so this is ignored\n"
           "/c/h.ini:33:1: error: expected template.NAME, NAME letters, "
           "digits and _ but not a digit first, so this is ignored\n"
           "/c/h.ini:34:1: error: expected template.NAME, NAME letters, "
           "digits and _ but not a digit first, so this is ignored\n"
           "/c/h.ini:23:8: warning: like makes a cycle, loop-a -> loop-b -> "
           "loop-a, so settings are taken once around it\n"
           "/c/h.ini:26:8: warning: like makes a cycle, loop-b -> loop-a -> "
           "loop-b, so settings are taken once around it\n"
           "/c/h.ini:29:8: warning: like makes a cycle, self -> self, so "
           "settings are taken once around it");

    // Settings along the chain: set empty is the default, bad is skipped
    static struct { Str model, key, want; } gets[] = {
        {"large", "top_p", "0.95"},
        {"large-quick", "top_p", "0.95"},
        {"large-quick", "max_tokens", "32768"},
        {"large-quick", "fallback", ""},
        {"large-deep", "max_tokens", "32768"},
        {"large-deep", "fallback", ""},
        {"large-deep", "like", "large-quick"},
        {"large-quick", "id", ""},
        {"large-deep", "reasoning", "reasoning_content"},
        {"loop-a", "temperature", "0.1"},
        {"loop-b", "temperature", "0.1"},
        {"into", "temperature", "0.1"},
        {"other", "top_p", ""},
    };
    for (iz i = 0; i < countof(gets); i++) {
        Str got = cfgget(c, "model", gets[i].model, gets[i].key, 0);
        expect(t, concat(&a, concat(&a, concat(&a, "like.get ", gets[i].model), "."),
                         gets[i].key), got, gets[i].want);
    }

    // Ids: an own id, else the end of the chain's name; a cycle's itself
    static struct { Str model, want; } ids[] = {
        {"large", "large"}, {"large-quick", "large"}, {"large-deep", "deepseek"},
        {"loop-a", "loop-a"}, {"self", "self"}, {"into", "into"},
        {"nosection", "unlisted"}, {"other", "other"}, {"team", "shared"},
    };
    for (iz i = 0; i < countof(ids); i++) {
        expect(t, concat(&a, "like.id ", ids[i].model), cfgmodelid(c, ids[i].model),
               ids[i].want);
    }

    // Arguments merged from the chain's end, each in its first place
    static struct { Str model, want; } args[] = {
        {"large", "{\"thinking\":true,\"reasoning_effort\":50,\"label\":\"42\","
                  "\"mode\":\"fast\",\"empty\":\"\"}"},
        {"large-quick", "{\"thinking\":true,\"reasoning_effort\":25,"
                        "\"label\":\"42\",\"empty\":\"\"}"},
        {"large-deep", "{\"thinking\":true,\"reasoning_effort\":100,"
                       "\"label\":\"42\",\"empty\":\"\",\"extra\":1.5}"},
        {"loop-a", "{\"x\":1}"},
        {"bad", "(null)"},
        {"other", "(null)"},
        {"team", "{\"from_team\":\"yes\"}"},
    };
    for (iz i = 0; i < countof(args); i++) {
        Str got = cfgtemplate(c, &a, args[i].model);
        expect(t, concat(&a, "like.args ", args[i].model),
               got.data ? got : Str("(null)"), args[i].want);
    }

    // The settings window's rows of a variant
    JsonDoc d = jsonparse(&a, cfgjson(&a, c, 0));
    Buf     b(&a, 1024);
    for (Json *s = jsonget(d.root, "settings")->child; s; s = s->next) {
        Str key  = jsonstr(jsonget(s, "key"));
        Str like = jsonstr(jsonget(s, "like"));
        if (!(jsonstr(jsonget(s, "name")) == "large-deep") ||
                !(key=="id" || key=="top_p" || key=="fallback" ||
                  startswith(key, "template."))) {
            continue;
        }
        print(&b, key);
        putbyte(&b, '=');
        print(&b, jsonstr(jsonget(s, "value")));
        print(&b, jsonint(jsonget(s, "default"), -1) ? Str("*") : Str(""));
        if (like.len) {
            print(&b, " like ");
            print(&b, like);
        }
        putbyte(&b, ' ');
        print(&b, jsonint(jsonget(s, "line"), -1));
        putbyte(&b, '\n');
    }
    expect(t, "like.rows", finish(&b),
           "id=deepseek 19\n"
           "fallback=* like large-quick 15\n"
           "top_p=0.95 like large 3\n"
           "template.reasoning_effort=100 20\n"
           "template.extra=1.5 21\n"
           "template.mode=* like large-quick 14\n"
           "template.thinking=true like large 6\n"
           "template.label=\"42\" like large 8\n"
           "template.empty=\"\" like large 10\n");

    // The engine: sent as the id, with the arguments, never to anthropic
    Engine *e = alloc<Engine>(&a);
    e->os  = v.os;
    e->cfg = c;
    e->ini = c->ini;
    ModelConf mc = {};
    modelconf(e, &a, "large-quick", &mc);
    expect(t, "like.mc.name", mc.name, "large");
    expect(t, "like.mc.kwargs", mc.kwargs,
           "{\"thinking\":true,\"reasoning_effort\":25,\"label\":\"42\","
           "\"empty\":\"\"}");
    Msg msg = {};
    msg.role = ROLE_USER;
    msg.text = "hi";
    ChatReq r = {};
    r.mc      = &mc;
    r.history = {&msg, 1, 1};
    Arena tmp = v.scratch;
    expect(t, "like.request", encodechat(&a, tmp, &r),
           "{\"model\":\"large\",\"messages\":[{\"role\":\"user\",\"content\":"
           "\"hi\"}],\"stream\":true,\"stream_options\":{\"include_usage\":"
           "true},\"top_p\":0.95,\"max_tokens\":32768,\"chat_template_kwargs\":"
           "{\"thinking\":true,\"reasoning_effort\":25,\"label\":\"42\","
           "\"empty\":\"\"}}");
    Str an = anencode(&a, tmp, &r);
    expect(t, "like.anthropic", istr(&a, find(an, "chat_template_kwargs")), "-1");
    expect(t, "like.anthropic.model", istr(&a, find(an, "\"model\":\"large\"")>0), "1");
    ModelConf none = {};
    modelconf(e, &a, "other", &none);
    expect(t, "like.none", none.kwargs.data ? none.kwargs : Str("(null)"), "(null)");
    cfgfree(c);

    // Edits: quoted values stay strings, bare ones are typed, bad names
    // are refused
    os_write(v.os, a, "/e/h.ini", "[model m]\ntemplate.n = \"7\"\n");
    c = newconfig(v.os, &a, v.scratch, "/e/h.ini", 0);
    static struct { Str key, value, want; } edits[] = {
        {"template.n", "7", "[model m]\ntemplate.n = 7\n"},
        {"template.n", "\"7\"", "[model m]\ntemplate.n = \"7\"\n"},
        {"template.n", "\"say \\\"hi\\\"\"",
         "[model m]\ntemplate.n = \"say \\\"hi\\\"\"\n"},
        {"template.on", "true", "[model m]\ntemplate.n = \"say \\\"hi\\\"\"\n"
                                "template.on = true\n"},
        {"template.on", "", "[model m]\ntemplate.n = \"say \\\"hi\\\"\"\n"
                            "template.on = \n"},
        {"like", "base", "[model m]\ntemplate.n = \"say \\\"hi\\\"\"\n"
                         "template.on = \nlike = base\n"},
    };
    for (iz i = 0; i < countof(edits); i++) {
        Arena  t2 = v.scratch;
        CfgSet s  = cfgset(c, &t2, "model", "m", edits[i].key, edits[i].value);
        Str    n  = concat(&a, "like.set ", edits[i].value);
        expect(t, concat(&a, n, " ok"), s.ok ? Str("ok") : s.err, "ok");
        expect(t, n, cfgfile(v.os, "/e/h.ini"), edits[i].want);
    }
    expect(t, "like.set.kwargs", cfgtemplate(c, &a, "m"), "{\"n\":\"say \\\"hi\\\"\"}");
    static struct { Str key, value; } bad[] = {
        {"template.", "1"}, {"template.9", "1"}, {"template.a b", "1"},
        {"template.q", "\"a\" b\""},
    };
    for (iz i = 0; i < countof(bad); i++) {
        Arena  t2 = v.scratch;
        CfgSet s  = cfgset(c, &t2, "model", "m", bad[i].key, bad[i].value);
        expect(t, concat(&a, "like.bad ", bad[i].key), istr(&a, s.ok), "0");
    }
    cfgfree(c);
}

// [tools] path_prepend and path_append: the PATH programs are found by,
// and in an included file, ignored with the other settings that choose
// what runs; the environment's PATH set to it, and the engine's lookups
// on it.
static void testcfgpath(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);

    // An included file chooses nothing that runs, but may name a shell
    // by its keyword
    os_write(v.os, a, "/c/keyword.ini", "[tools]\nshell = powershell\n");
    os_write(v.os, a, "/c/shared.ini",
        "[tools]\n"
        "path_prepend = /evil/bin\n"
        "path_append = /evil/late\n"
        "shell = /evil/sh\n"
        "[ui]\n"                                // 5
        "editor = evil --wait\n"
        "theme = dark\n");
    os_write(v.os, a, "/c/h.ini", "include = keyword.ini\ninclude = shared.ini\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    expect(t, "path.shared.diags", inidiags(&a, c->ini),
        "/c/shared.ini:2:16: error: [tools] path_prepend in an included file "
        "is ignored, since it chooses what runs: set it in your own file\n"
        "/c/shared.ini:3:15: error: [tools] path_append in an included file "
        "is ignored, since it chooses what runs: set it in your own file\n"
        "/c/shared.ini:4:9: error: a shell by its path or name in an included "
        "file is ignored, since it chooses what runs: set it in your own "
        "file, or here one of auto, sh, bash, powershell, cmd\n"
        "/c/shared.ini:6:10: error: [ui] editor in an included file is "
        "ignored, since it chooses what runs: set it in your own file");
    expect(t, "path.shared.prepend", cfgget(c, "tools", "", "path_prepend", 0), "");
    expect(t, "path.shared.shell", cfgget(c, "tools", "", "shell", 0), "auto");
    expect(t, "path.shared.editor", cfgget(c, "ui", "", "editor", 0), "");
    expect(t, "path.shared.theme", cfgget(c, "ui", "", "theme", 0), "dark");
    expect(t, "path.shared.path", cfgsearchpath(c, &a, "/usr/bin:/bin", "/home/u", 0),
           "/usr/bin:/bin");
    cfgfree(c);
    os_write(v.os, a, "/c/h.ini", "include = shared.ini\ninclude = keyword.ini\n");
    c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    expect(t, "path.keyword", cfgget(c, "tools", "", "shell", 0), "powershell");
    cfgfree(c);

    // The user's own: in order, ~ the home directory, empty, relative
    // and (on POSIX) colon entries left out
    os_write(v.os, a, "/c/h.ini",
             "include = shared.ini\n"
             "[tools]\n"
             "path_prepend = ~/bin; ; /opt/x/bin ;rel/bin; C:/x\n"
             "path_append = /late\n"
             "shell = /bin/zsh\n"
             "[ui]\n"
             "editor = code --wait\n");
    c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    expect(t, "path.own.shell", cfgget(c, "tools", "", "shell", 0), "/bin/zsh");
    expect(t, "path.own.editor", cfgget(c, "ui", "", "editor", 0), "code --wait");
    expect(t, "path.own", cfgsearchpath(c, &a, "/usr/bin:/bin", "/home/u", 0),
           "/home/u/bin:/opt/x/bin:/usr/bin:/bin:/late");
    expect(t, "path.own.nohome", cfgsearchpath(c, &a, "/usr/bin", "", 0),
           "/opt/x/bin:/usr/bin:/late");
    expect(t, "path.own.empty", cfgsearchpath(c, &a, "", "/home/u", 0),
           "/home/u/bin:/opt/x/bin:/late");
    cfgfree(c);

    // A family of keys, [mcp] env.NAME: an included file's are left
    // out, empty or not, as validation says
    os_write(v.os, a, "/c/mcp.ini",
             "[mcp srv]\n"
             "env.A = 1\n"
             "env.B =\n"
             "header.X = y\n");
    os_write(v.os, a, "/c/h.ini",
             "include = mcp.ini\n"
             "[mcp srv]\n"
             "command = srv\n"
             "env.C = 3\n"
             "env.A = 4\n"
             "env. = 5\n");
    c = newconfig(v.os, &a, v.scratch, "/c/h.ini", 0);
    {
        Buf            b(&a, 64);
        Slice<CfgPair> p = cfgpairs(c, &a, "mcp", "srv", "env.");
        for (iz i = 0; i < p.len; i++) {
            print(&b, p[i].name);
            putbyte(&b, '=');
            print(&b, p[i].value);
            putbyte(&b, ' ');
        }
        expect(t, "pairs.own", finish(&b), "A=4 C=3 ");
        expect(t, "pairs.shared", istr(&a, cfgpairs(c, &a, "mcp", "srv", "header.").len), "0");
    }
    cfgfree(c);

    // Windows: ; between, backslashes in the directories added
    os_write(v.os, a, "/c/w.ini",
             "[tools]\n"
             "path_prepend = C:\\tools; ~/bin\n"
             "path_append = D:/late;\n");
    c = newconfig(v.os, &a, v.scratch, "/c/w.ini", 0);
    expect(t, "path.windows",
           cfgsearchpath(c, &a, "C:\\Windows;C:/Windows/System32", "C:\\Users\\u", 1),
           "C:\\tools;C:\\Users\\u\\bin;C:\\Windows;C:/Windows/System32;D:\\late");
    cfgfree(c);
    os_write(v.os, a, "/c/none.ini", "");
    c = newconfig(v.os, &a, v.scratch, "/c/none.ini", 0);
    Str same = "/usr/bin:/bin";
    Str got  = cfgsearchpath(c, &a, same, "/home/u", 0);
    expect(t, "path.unset", istr(&a, got.data==same.data && got.len==same.len), "1");
    cfgfree(c);

    // The environment's PATH, set to it: in place, under its own name
    {
        Str posix[] = {"HOME=/h", "PATH=/bin", "X=1"};
        Slice<Str> env = {posix, countof(posix), countof(posix)};
        expect(t, "env.path", envpath(env, 0), "/bin");
        expect(t, "env.setpath", showlist(&a, envsetpath(&a, env, "/a:/bin", 0)),
               "HOME=/h|PATH=/a:/bin|X=1");
        Slice<Str> same = envsetpath(&a, env, "/bin", 0);
        expect(t, "env.setpath.same", istr(&a, same.data==env.data), "1");

        Str win[] = {"=C:=C:\\x", "Path=C:\\w", "PATH2=y"};
        Slice<Str> wenv = {win, countof(win), countof(win)};
        expect(t, "env.path.win", envpath(wenv, 1), "C:\\w");
        expect(t, "env.setpath.win",
               showlist(&a, envsetpath(&a, wenv, "C:\\t;C:\\w", 1)),
               "=C:=C:\\x|Path=C:\\t;C:\\w|PATH2=y");
        expect(t, "env.path.case", envpath(wenv, 0).data ? Str("set") : Str("null"),
               "null");
        expect(t, "env.setpath.case", showlist(&a, envsetpath(&a, wenv, "/a", 0)),
               "=C:=C:\\x|Path=C:\\w|PATH2=y|PATH=/a");

        Slice<Str> none = {};
        expect(t, "env.setpath.none", istr(&a, envsetpath(&a, none, "", 0).len), "0");
    }

    // The engine's lookups go by the PATH given: busybox for Windows'
    // sh, a shell by its name, ripgrep
    {
        os_write(v.os, a, "/bb/busybox.exe", "MZ");
        os_write(v.os, a, "/opt/bin/zsh", "#!");
        os_write(v.os, a, "/w/bin/nu.exe", "MZ");
        Shell sh = findshell(v.os, &a, v.scratch, "/exe", "C:/nowhere;/bb", 1, "auto");
        expect(t, "findshell.busybox", sh.path, "/bb/busybox.exe");
        expect(t, "findshell.busybox.kind", istr(&a, sh.kind), istr(&a, SHELL_BUSYBOX));
        expect(t, "findshell.busybox.flavor", istr(&a, sh.flavor), istr(&a, FLAVOR_BUSYBOX));
        sh = findshell(v.os, &a, v.scratch, "/exe", "/usr/bin:/opt/bin", 0, "zsh");
        expect(t, "findshell.name", sh.path, "/opt/bin/zsh");
        sh = findshell(v.os, &a, v.scratch, "/exe", "/usr/bin", 0, "zsh");
        expect(t, "findshell.name.missing", sh.path, "/bin/sh");
        sh = findshell(v.os, &a, v.scratch, "/exe", "/w/bin", 1, "nu");
        expect(t, "findshell.name.exe", sh.path, "/w/bin/nu.exe");

        // What a sh on Windows is, by the files beside it
        os_write(v.os, a, "/w64/bin/sh.exe", "MZ");
        os_write(v.os, a, "/w64/bin/busybox.exe", "MZ");
        os_write(v.os, a, "/msys/usr/bin/bash.exe", "MZ");
        os_write(v.os, a, "/msys/usr/bin/msys-2.0.dll", "MZ");
        os_write(v.os, a, "/git/bin/bash.exe", "MZ");
        os_write(v.os, a, "/git/usr/bin/msys-2.0.dll", "MZ");
        os_write(v.os, a, "/cyg/bin/bash.exe", "MZ");
        os_write(v.os, a, "/cyg/bin/cygwin1.dll", "MZ");
        os_write(v.os, a, "/other/bin/sh.exe", "MZ");
        struct { Str path; b32 win; i32 flavor; } fl[] = {
            {"/w64/bin/sh.exe",        1, FLAVOR_BUSYBOX},
            {"/msys/usr/bin/bash.exe", 1, FLAVOR_MSYS},
            {"/git/bin/bash.exe",      1, FLAVOR_MSYS},
            {"/cyg/bin/bash.exe",      1, FLAVOR_CYGWIN},
            {"/other/bin/sh.exe",      1, FLAVOR_NONE},
            {"/w64/bin/sh.exe",        0, FLAVOR_NONE},
        };
        for (iz i = 0; i < countof(fl); i++) {
            sh = findshell(v.os, &a, v.scratch, "/exe", "", fl[i].win, fl[i].path);
            expect(t, concat(&a, "findshell.flavor ", fl[i].path),
                   istr(&a, sh.flavor), istr(&a, fl[i].flavor));
        }

        // cmd as chosen, or for want of any other
        sh = findshell(v.os, &a, v.scratch, "/exe", "C:/nowhere", 1, "cmd");
        expect(t, "findshell.cmd", istr(&a, sh.kind*10 + sh.fallback), istr(&a, SHELL_CMD*10));
        sh = findshell(v.os, &a, v.scratch, "/exe", "C:/nowhere", 1, "auto");
        expect(t, "findshell.cmd.fallback", istr(&a, sh.kind*10 + sh.fallback), istr(&a, SHELL_CMD*10 + 1));
    }

    // Shells by name: [shell] sections of the user's own file only, a
    // program found along a section's own path, keywords that must find
    // their kind, and a command's environment from its section
    {
        os_write(v.os, a, "/c/shared.ini",
                 "[shell evil]\n"
                 "program = /opt/bin/zsh\n"
                 "[shell zsh]\n"
                 "env.EVIL = 1\n");
        os_write(v.os, a, "/c/s.ini",
                 "include = shared.ini\n"
                 "[tools]\n"
                 "shell = zsh\n"
                 "[shell zsh]\n"
                 "path_prepend = /opt/bin\n"
                 "env.FOO = bar\n"
                 "env.MINE = env:SH_TEST\n"
                 "login = true\n"
                 "description = Zsh here\n"
                 "[shell ps]\n"
                 "program = powershell\n"
                 "[shell gone]\n"
                 "program = /nowhere/x\n"
                 "[shell deb]\n"
                 "program = /c/wsl.exe\n"
                 "distro = Debian\n"
                 "path_prepend = ~/bin; /opt/it's\n"
                 "path_append = /post\n"
                 "env.FOO = bar\n");
        os_write(v.os, a, "/c/wsl.exe", "");
        addenv(v.os, &a, "SH_TEST=42");
        Config *c = newconfig(v.os, &a, v.scratch, "/c/s.ini", 0);
        Engine *e = alloc<Engine>(&a);
        e->os     = v.os;
        e->cfg    = c;
        e->ini    = c->ini;
        e->home   = "/home/u";
        e->exedir = "/exe";
        e->env    = push(&a, e->env, Str("PATH=/usr/bin"));
        e->env    = push(&a, e->env, Str("ORIGINAL_PATH=/c/stale"));

        struct { Str name; b32 prog; Str want; } cases[] = {
            {"zsh",          0, "zsh /opt/bin/zsh Zsh here"},
            {"evil",         0, "(there is no [shell evil] in your configuration)"},
            {"sh",           0, "sh /bin/sh "},
            {"auto",         0, "sh /bin/sh "},
            {"",             0, "sh /bin/sh "},
            {"ps",           0, "(powershell is not found here)"},
            {"cmd",          0, "(cmd is not found here)"},
            {"gone",         0, "(/nowhere/x is not found)"},
            {"/opt/bin/zsh", 1, "zsh /opt/bin/zsh "},
            {"/opt/bin/zsh", 0, "(there is no [shell /opt/bin/zsh] in your configuration)"},
            {"bash",         1, "(bash is not found here)"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Str   err = {};
            Shell sh  = shellnamed(e, &a, v.scratch, cases[i].name, cases[i].prog, 0, &err);
            Str   got = sh.path.len
                ? concat(&a, concat(&a, concat(&a, sh.name, " "), concat(&a, sh.path, " ")), sh.desc)
                : concat(&a, concat(&a, "(", err), ")");
            expect(t, concat(&a, "shellnamed ", cases[i].name), got, cases[i].want);
        }
        // bash on PATH: auto's first, by name; sh stays /bin/sh. On
        // Windows bash is a program, not a keyword
        {
            os_write(v.os, a, "/usr/bin/bash", "");
            struct { Str name; b32 win; Str want; } bash[] = {
                {"auto", 0, "bash /usr/bin/bash "},
                {"bash", 0, "bash /usr/bin/bash "},
                {"sh",   0, "sh /bin/sh "},
                {"bash", 1, "(there is no [shell bash] in your configuration)"},
            };
            for (iz i = 0; i < countof(bash); i++) {
                Str   err = {};
                Shell sh  = shellnamed(e, &a, v.scratch, bash[i].name, 0, bash[i].win, &err);
                Str   got = sh.path.len
                    ? concat(&a, concat(&a, concat(&a, sh.name, " "), concat(&a, sh.path, " ")), sh.desc)
                    : concat(&a, concat(&a, "(", err), ")");
                expect(t, concat(&a, "shellnamed.bash ", bash[i].name), got, bash[i].want);
            }
            os_remove(v.os, a, "/usr/bin/bash");
        }

        Str   warn = {};
        Shell sh   = defaultshell(e, &a, v.scratch, 0, &warn);
        expect(t, "defaultshell", concat(&a, sh.name, warn), "zsh");

        Session *s = alloc<Session>(&a);
        s->e = e;
        Slice<Str> env = shellenvfor(s, &a, &sh);
        expect(t, "shellenv.login", istr(&a, sh.login), "1");
        Str all = showlist(&a, env);
        expect(t, "shellenv", istr(&a, startswith(all, "PATH=/opt/bin:/usr/bin|") &&
               endswith(all, "|FOO=bar|MINE=42") && find(all, "EVIL")<0), "1");
        s->te.windows = 1;
        env = shellenvfor(s, &a, &sh);
        all = showlist(&a, env);
        expect(t, "shellenv.windows", istr(&a, find(all, "|CHERE_INVOKING=1|MSYS2_PATH_TYPE=inherit|FOO=bar")>0 &&
               find(all, "ORIGINAL_PATH")<0), "1");
        Shell plain = {};
        plain.name  = "sh";
        plain.login = 1;
        s->te.windows = 0;
        env = shellenvfor(s, &a, &plain);
        all = showlist(&a, env);
        expect(t, "shellenv.plain", istr(&a, !plain.login && startswith(all, "PATH=/usr/bin|") &&
               find(all, "FOO")<0), "1");

        // WSL: its distro, its directories Linux's, before the command,
        // and its env.NAME passed into Linux
        Str   werr = {};
        Shell deb  = shellnamed(e, &a, v.scratch, "deb", 0, 1, &werr);
        expect(t, "wsl shellnamed", concat(&a, shellkinds[deb.kind], concat(&a, " ", deb.distro)),
               "wsl Debian");
        s->te.windows = 1;
        env = shellenvfor(s, &a, &deb);
        all = showlist(&a, env);
        expect(t, "wsl shellenv", istr(&a, startswith(all, "PATH=/usr/bin|") &&
               endswith(all, "|FOO=bar|WSLENV=FOO|WSL_UTF8=1")), "1");
        expect(t, "wsl prelude", deb.prelude,
               "PATH=\"$HOME\"'/bin':'/opt/it'\\''s':\"$PATH\":'/post'; export PATH; ");
        e->env = push(&a, e->env, Str("WSLENV=USERPROFILE/p"));
        env = shellenvfor(s, &a, &deb);
        expect(t, "wsl shellenv, WSLENV kept", istr(&a, find(showlist(&a, env),
               "|WSLENV=USERPROFILE/p:FOO|")>0), "1");
        cfgfree(c);
    }
}

static void test_config(Test *t)
{
    testcfgwsl(t);
    testcfgsha256(t);
    testcfgurls(t);
    testcfgschema(t);
    testcfgdefault(t);
    testcfgseed(t);
    testcfgvalidate(t);
    testcfgset(t);
    testcfgdelete(t);
    testcfgpoll(t);
    testcfgjson(t);
    testcfgengine(t);
    testcfglike(t);
    testcfgpath(t);
}
