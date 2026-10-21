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
    r.scratch.beg = (byte *)allocbytes(a, 1<<22);
    r.scratch.end = r.scratch.beg + (1<<22);
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
        {{"XDG_CONFIG_HOME=/x", "HOME=/h"}, "/x/harness/harness.ini"},
        {{"HOME=/h", "APPDATA=C:\\A"}, "/h/.config/harness/harness.ini"},
        {{"APPDATA=C:\\A", {}}, "C:\\A/harness/harness.ini"},
        {{"XDG_CONFIG_HOME=", "HOME=/h"}, "/h/.config/harness/harness.ini"},
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
           "C:/Users/u/AppData/Roaming/harness/harness.ini");
    expect(t, "default.seeded", cfgfile(v.os, c->path), cfgseed());
    cfgfree(c);
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
    expect(t, "seed.path", c->path, "/home/u/.config/harness/harness.ini");
    expect(t, "seed.written", cfgfile(v.os, c->path), cfgseed());
    Stat dir = os_stat(v.os, a, "/home/u/.config/harness");
    expect(t, "seed.dir", istr(&a, dir.type), istr(&a, STAT_DIR));
    expect(t, "seed.diags", inidiags(&a, c->ini), "");
    expect(t, "seed.exists", istr(&a, c->user != 0), "1");
    expect(t, "seed.rev", istr(&a, c->rev), "1");
    expect(t, "seed.endpoint", cfgget(c, "", "", "endpoint", 0), "local");
    expect(t, "seed.url", cfgget(c, "endpoint", "local", "url", 1),
           "http://localhost/");
    expect(t, "seed.newline", istr(&a, cfgseed()[cfgseed().len-1]), "10");
    for (iz i = 0; i < c->user->lines.len; i++) {
        if (c->user->lines[i].raw.len > 79) {
            expect(t, "seed line too long", c->user->lines[i].raw, "");
        }
    }
    cfgfree(c);

    // Every key in the schema is documented in the seed
    for (iz i = 0; i < countof(cfgschema); i++) {
        Str key = cfgschema[i].key;
        if (find(cfgseed(), key) < 0) {
            expect(t, "seed documents", key, "");
        }
    }

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

    // No location at all
    c = newconfig(v.os, &a, v.scratch, {}, 1);
    expect(t, "seed.nowhere", inidiags(&a, c->ini),
           "warning: no configuration file: none of XDG_CONFIG_HOME, HOME "
           "or APPDATA is set");
    CfgSet r = cfgset(c, &a, "", "", "model", "m");
    expect(t, "seed.nowhere.set", r.err, "no configuration file location");
    cfgfree(c);
}

static void testcfgvalidate(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    addenv(v.os, &a, "HOME=/home/u");
    os_write(v.os, a, "/c/harness.ini",
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
    Config *c = newconfig(v.os, &a, v.scratch, "/c/harness.ini", 0);
    expect(t, "validate.diags", inidiags(&a, c->ini),
        "/c/harness.ini:4:1: warning: unknown key \"color\" at the top level\n"
        "/c/harness.ini:7:16: error: expected seconds, a number above 0, so "
        "the default applies\n"
        "/c/harness.ini:8:1: warning: unknown key \"idle_timout\" in "
        "[endpoint]; did you mean idle_timeout?\n"
        "/c/harness.ini:9:7: warning: a plaintext secret: write env:NAME and "
        "set the variable, so that this file can be shared\n"
        "/c/harness.ini:10:2: warning: endpoint broken has no url\n"
        "/c/harness.ini:12:2: warning: a section needs a name, as in "
        "[endpoint NAME], or its settings are unused\n"
        "/c/harness.ini:15:9: warning: relative path \"rel/path\": use an "
        "absolute path or one starting with ~\n"
        "/c/harness.ini:17:13: warning: unknown value \"thoughts\", expected "
        "one of: reasoning_content, reasoning, reasoning_text, none\n"
        "/c/harness.ini:18:9: error: expected a whole number, so this is "
        "ignored\n"
        "/c/harness.ini:20:9: warning: unknown value \"fly\", expected any "
        "of: read, write, edit, shell, grep, glob, todo_write, jobs, "
        "job_output, job_kill, task\n"
        "/c/harness.ini:21:2: warning: unknown section kind \"provider\", "
        "expected endpoint, model, role, guard, tools, instructions or ui\n"
        "/c/harness.ini:23:8: warning: [guard] takes no name, or its "
        "settings are unused\n"
        "/c/team.ini:4:7: error: a plaintext secret in an included file is "
        "ignored: write env:NAME and set the variable\n"
        "/c/team.ini:6:1: warning: unknown key \"url2\" in [endpoint]; did "
        "you mean url?\n"
        "/c/harness.ini:1:1: warning: overridden by /c/team.ini:1, included "
        "after this line\n"
        "/c/harness.ini:11:7: warning: environment variable MISSING_VAR is "
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
}

// The exact file after each edit, and the reload that follows.
static void testcfgset(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    Str    team = "[endpoint team]\nurl = https://team.example/\n";
    os_write(v.os, a, "/c/team.ini", team);
    os_write(v.os, a, "/c/harness.ini",
        "# My settings\n"
        "include = team.ini\n"
        "\n"
        "[endpoint local]\n"
        "url = http://localhost:8080/   # llama.cpp\n"
        "idle_timeout = 90\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/harness.ini", 0);
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
    expect(t, "set.text", cfgfile(v.os, "/c/harness.ini"),
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
    Str before = cfgfile(v.os, "/c/harness.ini");
    static struct { Str kind, name, key, value, err; } bad[] = {
        {"endpoint", "local", "colour", "x", "unknown key"},
        {"endpoint", "local", "idle_timeout", "soon",
         "expected seconds, a number above 0, so the default applies"},
        {"endpoint", "local", "key", "sk-x",
         "a secret must be a reference such as env:NAME"},
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
    expect(t, "refuse.file", cfgfile(v.os, "/c/harness.ini"), before);

    // Setting what is already there writes nothing, but reloads
    i64 rev = c->rev;
    {
        Arena  tmp = v.scratch;
        VFile *f   = vfind(v.os, "/c/harness.ini", 0);
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
        Str     p   = seed ? Str("/s/harness.ini") : Str("/n/harness.ini");
        Config *m   = newconfig(v.os, &a, tmp, p, seed);
        os_remove(v.os, a, p);
        CfgSet  r   = cfgset(m, &tmp, "", "", "model", "m");
        Str     want = "model = m\n";
        if (seed) {
            Str s   = cfgseed();
            iz  at  = find(s, "endpoint = local\n") + 17;
            want = concat(&a, takehead(s, at), want);
            want = concat(&a, want, cuthead(s, at));
        }
        expect(t, concat(&a, "missing ", p), cfgfile(v.os, p), want);
        expect(t, concat(&a, "missing.ok ", p), istr(&a, r.ok), "1");
        expect(t, concat(&a, "missing.diags ", p), inidiags(&a, m->ini), "");
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

static void testcfgpoll(Test *t)
{
    Arena  a = t->perm;
    CfgEnv v = newcfgenv(&a);
    v.os->now = 100 * 1000000;
    Str head = "include = t.ini\ninclude = later.ini\n[endpoint e]\n";
    os_write(v.os, a, "/c/h.ini", concat(&a, head, "url = http://e/\n"));
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

    os_write(v.os, a, "/c/h.ini", concat(&a, head, "url = http://f/\n"));
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
        "endpoint/local/key=env:KEY@/c/h.ini:5\n"
        "endpoint/local/connect_timeout=5*\n"
        "endpoint/local/first_byte_timeout=1800*\n"
        "endpoint/local/idle_timeout=30@/c/h.ini:4\n"
        "endpoint/local/progress_timeout=600*\n"
        "endpoint/local/slow_retry=60 120 300 600 900*\n"
        "endpoint/local/slow_retry_budget=7200*\n"
        "endpoint/local/max_concurrent=*\n"
        "endpoint/local/max_models=*\n"
        "model/m/reasoning=reasoning_content*\n"
        "model/m/context=*\n"
        "model/m/compactor=*\n"
        "model/m/fallback=*\n"
        "model/m/fallback_cooldown=600*\n"
        "model/m/reasoning_budget=*\n"
        "model/m/edit_format=replace*\n"
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
        "tools//job_timeout=1800*\n"
        "tools//job_timeout_max=7200*\n"
        "tools//auto_resume=5*\n"
        "tools//stop_grace=*\n"
        "tools//use_rg=true*\n"
        "tools//task_budget=200*\n"
        "instructions//files=AGENTS.md CLAUDE.md*\n"
        "ui//theme=system*\n"
        "host//idle_exit=600*\n"
        "host//grace=10800*\n");

    Json *schema = jsonget(j, "schema");
    expect(t, "json.schema", istr(&a, jsonlen(schema)),
           istr(&a, countof(cfgschema)));
    Json *ft = jsonat(schema, 5);
    expect(t, "json.schema.key", jsonstr(jsonget(ft, "key")),
           "first_byte_timeout");
    expect(t, "json.schema.type", jsonstr(jsonget(ft, "type")), "seconds");
    expect(t, "json.schema.default", jsonstr(jsonget(ft, "default")), "300");
    expect(t, "json.schema.local", jsonstr(jsonget(ft, "local")), "1800");
    iz    ti    = cfgkey("role", "tools") - cfgschema;
    Json *tools = jsonget(jsonat(schema, ti), "choices");
    expect(t, "json.schema.choices", istr(&a, jsonlen(tools)), "11");
    expect(t, "json.kinds", istr(&a, jsonlen(jsonget(j, "kinds"))),
           istr(&a, countof(cfgkinds)));

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
    cfgfree(c);
}

static void test_config(Test *t)
{
    testcfgschema(t);
    testcfgdefault(t);
    testcfgseed(t);
    testcfgvalidate(t);
    testcfgset(t);
    testcfgpoll(t);
    testcfgjson(t);
    testcfgengine(t);
}
