// Skills: the frontmatter parser, discovery and precedence, and the
// prompt that invokes one

static Str skillnames(Arena *a, Slice<Skill> list)
{
    Buf b(a, 256);
    for (iz i = 0; i < list.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, list[i].name);
        if (list[i].hidden) {
            putbyte(&b, '*');
        }
    }
    return finish(&b);
}

static void test_skills(Test *t)
{
    Arena *a = &t->perm;

    // Frontmatter
    {
        SkillMeta m = skillparse(a,
            "---\r\nname: pdf-tools\r\ndescription: Fill PDF forms # a comment\r\n"
            "license: MIT\r\n---\r\n# PDF\r\nBody.\r\n");
        expect(t, "crlf", concat(a, concat(a, m.name, "|"), m.desc), "pdf-tools|Fill PDF forms");
        expect(t, "crlf body", m.body, "# PDF\r\nBody.\r\n");
        expect(t, "crlf ok", istr(a, !m.err.data), "1");

        m = skillparse(a, "\xef\xbb\xbf---\ndescription: \"Say \\\"hi\\\" # not a comment\"\n"
                          "disable-model-invocation: true\n---\nx\n");
        expect(t, "double quoted", m.desc, "Say \"hi\" # not a comment");
        expect(t, "hidden", istr(a, m.hidden), "1");
        expect(t, "no name", istr(a, !m.name.data), "1");

        m = skillparse(a, "---\nname: 'it''s'\n---\n");
        expect(t, "single quoted, bad name", istr(a, !!m.err.data), "1");

        m = skillparse(a, "---\ndescription: >-\n  Folded over\n  two lines.\n\n"
                          "  Second.\nallowed-tools:\n- Read\n- Grep\nmetadata:\n"
                          "  version: 1\nname: fold\n---\n");
        expect(t, "folded", m.desc, "Folded over two lines.\nSecond.");
        expect(t, "folded name", m.name, "fold");
        expect(t, "folded ok", istr(a, !m.err.data), "1");

        m = skillparse(a, "---\ndescription: |\n  line one\n  line two\n---\n");
        expect(t, "literal", trim(m.desc), "line one\nline two");

        m = skillparse(a, "---\ndescription: plain that\n  goes on\n---\n");
        expect(t, "plain continued", m.desc, "plain that goes on");

        m = skillparse(a, "# Just markdown\n\nNo frontmatter.\n");
        expect(t, "no frontmatter", concat(a, istr(a, !m.err.data && !m.desc.data), m.body),
               "1# Just markdown\n\nNo frontmatter.\n");

        static Str const bad[] = {
            "---\nname: x\n",                   // never closed
            "---\nname x\n---\n",               // no colon
            "---\n  indented: x\n---\n",        // belongs to no key
            "---\ndescription: \"open\n---\n",  // a quote that does not close
            "---\nname: has space\n---\n",      // not a name
            "---\nname:x\n---\n",               // no space after the colon
        };
        for (iz i = 0; i < countof(bad); i++) {
            m = skillparse(a, bad[i]);
            expect(t, concat(a, "malformed ", istr(a, i)), istr(a, !!m.err.data), "1");
        }
        expect(t, "para", skillpara(a, "\n\n## Title\nmore\n\nlater"), "Title more");
    }

    // Discovery: precedence, hidden, malformed, and the caps
    Os *os = newos(a, 4<<20);
    static Str const made[] = {  // the test platform makes no parents
        "/r", "/r/.git", "/r/sub", "/r/sub/.claude", "/r/sub/.claude/skills",
        "/r/.claude", "/r/.claude/skills", "/r/.agents", "/r/.agents/skills",
        "/u", "/u/skills",
    };
    for (iz i = 0; i < countof(made); i++) {
        os_mkdirs(os, *a, made[i]);
    }
    os_mkdirs(os, *a, "/r/sub/.claude/skills/near");
    os_mkdirs(os, *a, "/r/.claude/skills/near");
    os_mkdirs(os, *a, "/r/.claude/skills/far");
    os_mkdirs(os, *a, "/r/.agents/skills/bad");
    os_mkdirs(os, *a, "/r/.agents/skills/notaskill");
    os_mkdirs(os, *a, "/u/skills/far");
    os_mkdirs(os, *a, "/u/skills/mine");
    os_mkdirs(os, *a, "/u/skills/big");
    os_write(os, *a, "/r/sub/.claude/skills/near/SKILL.md",
             "---\ndescription: the nearer one\n---\nNear body.\n");
    os_write(os, *a, "/r/.claude/skills/near/SKILL.md",
             "---\ndescription: the root's\n---\n");
    os_write(os, *a, "/r/.claude/skills/far/SKILL.md",
             "---\nname: far\ndescription: project far\n---\n");
    os_write(os, *a, "/r/.agents/skills/bad/SKILL.md", "---\nname: x\n");
    os_write(os, *a, "/r/.agents/skills/notaskill/README.md", "nothing\n");
    os_write(os, *a, "/u/skills/far/SKILL.md", "---\ndescription: user far\n---\n");
    os_write(os, *a, "/u/skills/mine/SKILL.md",
             "---\ndisable-model-invocation: yes\n---\nFirst paragraph\nof mine.\n\nMore.\n");
    {
        Buf big(a, SKILL_BYTES+64);
        print(&big, "---\ndescription: big\n---\n");
        for (; big.len <= SKILL_BYTES; putbyte(&big, 'x')) {}
        os_write(os, *a, "/u/skills/big/SKILL.md", finish(&big));
    }

    Slice<SkillDir> dirs = {};
    static Str const paths[] = {
        "/r/sub/.claude/skills", "/r/sub/.agents/skills",
        "/r/.claude/skills", "/r/.agents/skills",
    };
    for (iz i = 0; i < countof(paths); i++) {
        dirs = push(a, dirs, SkillDir{paths[i], "/r"});
    }
    dirs = push(a, dirs, SkillDir{"/u/skills", {}});
    Skills sk = skillfind(os, a, dirs, 0);
    expect(t, "found", skillnames(a, sk.list), "far mine* near");
    Skill *near = skillbyname(sk.list, "near");
    Skill *far  = skillbyname(sk.list, "far");
    Skill *mine = skillbyname(sk.list, "mine");
    expect(t, "nearer wins", near->desc, "the nearer one");
    expect(t, "nearer dir", near->dir, "/r/sub/.claude/skills/near");
    expect(t, "project over user", far->desc, "project far");
    expect(t, "first paragraph", mine->desc, "First paragraph of mine.");
    expect(t, "skipped", concat(a, istr(a, sk.skipped.len), concat(a, " ",
           concat(a, sk.skipped[0], concat(a, " ", sk.skipped[1])))),
           "2 /r/.agents/skills/bad/SKILL.md: its frontmatter has no closing --- "
           "/u/skills/big/SKILL.md: it is larger than 256 KiB");

    SkillMeta m = skillload(os, a, near);
    Str text = skilltext(a, near, m.body, " now ");
    expect(t, "tool text", text, "Base directory for this skill: "
           "/r/sub/.claude/skills/near\n\nNear body.\n\nARGUMENTS: now");

    // Below folders that group skills, as Cursor allows: a skill's own
    // directory is not searched further, nor past SKILL_DEPTH levels, and
    // the first by path wins
    {
        static Str const nest[] = {
            "/n", "/n/a", "/n/a/lint", "/n/a/lint/inner", "/n/b", "/n/b/lint",
            "/n/top", "/n/d1", "/n/d1/d2", "/n/d1/d2/d3", "/n/d1/d2/d3/ok",
            "/n/d1/d2/d3/d4", "/n/d1/d2/d3/d4/deep",
        };
        for (iz i = 0; i < countof(nest); i++) {
            os_mkdirs(os, *a, nest[i]);
        }
        os_write(os, *a, "/n/a/lint/SKILL.md", "---\ndescription: a's\n---\n");
        os_write(os, *a, "/n/a/lint/inner/SKILL.md", "x\n");
        os_write(os, *a, "/n/b/lint/SKILL.md", "---\ndescription: b's\n---\n");
        os_write(os, *a, "/n/top/SKILL.md", "x\n");
        os_write(os, *a, "/n/d1/d2/d3/ok/SKILL.md", "x\n");
        os_write(os, *a, "/n/d1/d2/d3/d4/deep/SKILL.md", "x\n");
        Slice<SkillDir> nd = {};
        nd = push(a, nd, SkillDir{"/n", {}});
        Skills ns = skillfind(os, a, nd, 0);
        expect(t, "nested", skillnames(a, ns.list), "lint ok top");
        expect(t, "nested first", skillbyname(ns.list, "lint")->dir, "/n/a/lint");
        expect(t, "nested skipped", istr(a, ns.skipped.len), "0");
    }

    // A long description is cut, and too many skills are left out
    {
        os_mkdirs(os, *a, "/many");
        for (i32 i = 0; i < SKILL_MAX+3; i++) {
            Buf n(a, 64);
            print(&n, "/many/s");
            print(&n, (i64)(1000+i));
            os_mkdirs(os, *a, finish(&n));
            print(&n, "/SKILL.md");
            os_write(os, *a, finish(&n), i ? Str("x\n") : Str(
                "---\ndescription: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n---\n"));
        }
        Slice<SkillDir> md = {};
        md = push(a, md, SkillDir{"/many", {}});
        Skills many = skillfind(os, a, md, 0);
        expect(t, "capped", istr(a, many.list.len), istr(a, SKILL_MAX));
        expect(t, "capped note", istr(a, many.skipped.len), "1");
        expect(t, "description cut", istr(a, many.list[0].desc.len), istr(a, SKILL_DESC));
    }

    // A prompt invoking a skill, and back
    {
        Skill k = {};
        k.name = "pdf";
        k.dir  = "/s/\"q\"/pdf";
        Str p = skillprompt(a, &k, "\nDo it.\n", "  file.pdf ");
        expect(t, "prompt", p, "<skill name=\"pdf\" dir=\"/s/&quot;q&quot;/pdf\">\n"
               "Do it.\n</skill>\n\nfile.pdf");
        SkillCall c = skillcalled(p);
        expect(t, "called", concat(a, concat(a, istr(a, c.ok), c.name), c.args), "1pdffile.pdf");
        c = skillcalled(skillprompt(a, &k, "", ""));
        expect(t, "called bare", concat(a, istr(a, c.ok), c.name), "1pdf");
        expect(t, "not called", istr(a, skillcalled("<skill name=\"x\">no end").ok), "0");

        Skill b = helpskill(a);
        p = skillprompt(a, &b, "Body.", "how?");
        expect(t, "prompt builtin", p, "<skill name=\"prospero\">\nBody.\n</skill>\n\nhow?");
        c = skillcalled(p);
        expect(t, "called builtin", concat(a, concat(a, istr(a, c.ok), c.name), c.args), "1prosperohow?");
    }

    // A project's skill may not take the built-in one's name, by its
    // directory or its frontmatter, but the user's may
    {
        static Str const own[] = {
            "/o", "/o/.git", "/o/.claude", "/o/.claude/skills",
            "/o/.claude/skills/prospero", "/o/.claude/skills/other",
            "/ou", "/ou/prospero",
        };
        for (iz i = 0; i < countof(own); i++) {
            os_mkdirs(os, *a, own[i]);
        }
        os_write(os, *a, "/o/.claude/skills/prospero/SKILL.md", "x\n");
        os_write(os, *a, "/o/.claude/skills/other/SKILL.md", "---\nname: prospero\n---\n");
        os_write(os, *a, "/ou/prospero/SKILL.md", "---\ndescription: mine\n---\n");
        Slice<SkillDir> od = {};
        od = push(a, od, SkillDir{"/o/.claude/skills", "/o"});
        Skills os1 = skillfind(os, a, od, 0);
        expect(t, "project builtin", skillnames(a, os1.list), "");
        expect(t, "project builtin skipped", istr(a, os1.skipped.len), "2");
        od = push(a, od, SkillDir{"/ou", {}});
        Skills os2 = skillfind(os, a, od, 0);
        expect(t, "user builtin", skillbyname(os2.list, "prospero")->desc, "mine");
    }
}

// The built-in skill: its description, its body filled, and the topics
// its tool's args name
static void test_helpskill(Test *t)
{
    Arena *a = &t->perm;

    Skill k = helpskill(a);
    expect(t, "name", k.name, skillbuiltin);
    expect(t, "builtin", istr(a, k.builtin), "1");
    expect(t, "desc", k.desc, "Prospero, the coding harness this session runs "
           "in: its help pages, every setting in prospero.ini, its panes and "
           "keys, and how to change them. Use when the user asks about "
           "Prospero, how to do something in it, or to change its settings.");

    HelpSelf self = {"9.9", "/c/prospero.ini", "/s", "/c/AGENTS.md", "/c/skills"};
    Str body = helpskillbody(a, &self);
    expect(t, "filled", istr(a, find(body, "{{") < 0 && find(body, "(unknown)") < 0), "1");
    expect(t, "no end tag", istr(a, find(body, "\n</skill>")), "-1");
    expect(t, "version", istr(a, find(body, "Prospero 9.9,") > 0), "1");
    expect(t, "topics", istr(a, find(body, "`config`") > 0), "1");
    HelpSelf none = {};
    expect(t, "unknown", istr(a, find(helpskillbody(a, &none), "from (unknown)") > 0), "1");

    HelpPick p = helpargs(a, 0, "config");
    expect(t, "page", istr(a, startswith(p.text, "Help topic `config` (help:config):\n\n# ")), "1");
    expect(t, "page missed", p.missed, "");
    p = helpargs(a, 0, "\"Configuration\", help:config#includes");
    expect(t, "page once", istr(a, find(p.text, "---")), "-1");
    p = helpargs(a, 0, "settings ui");
    expect(t, "kind", istr(a, find(p.text, "(settings:ui#editor)") > 0), "1");
    expect(t, "kind only", istr(a, find(p.text, "settings:model")), "-1");
    p = helpargs(a, 0, "settings Interface");
    expect(t, "kind label", istr(a, find(p.text, "(settings:ui#editor)") > 0), "1");
    p = helpargs(a, 0, "settings");
    expect(t, "index", istr(a, find(p.text, "settings:diagnostics") > 0), "1");
    p = helpargs(a, 0, "settings config");
    expect(t, "index, page", istr(a, find(p.text, "help:config") > 0 &&
                                     find(p.text, "settings:diagnostics") > 0), "1");
    p = helpargs(a, 0, "nope, config");
    expect(t, "missed", p.missed, "nope");
    p = helpargs(a, 0, "nope");
    expect(t, "none", istr(a, !p.text.data), "1");

    // Where links go
    Dest d = helpdest(a, "help:config#Settings-worth-knowing");
    expect(t, "help", istr(a, d.kind==DEST_HELP && d.page==helpnamed("config")), "1");
    expect(t, "help section", d.section, "Settings-worth-knowing");
    expect(t, "help none", istr(a, helpdest(a, "help:nope").page), "-1");
    d = helpdest(a, "settings:ui#editor");
    expect(t, "settings", concat(a, concat(a, d.cfgkind, "|"), d.cfgkey), "ui|editor");
    expect(t, "settings known", istr(a, d.known && !d.cfgname.data), "1");
    d = helpdest(a, "settings:model/a%2Fb.c#effort");
    expect(t, "settings named", concat(a, concat(a, d.cfgkind, "|"), d.cfgname), "model|a/b.c");
    d = helpdest(a, "settings:Interface");
    expect(t, "settings label", d.cfgkind, "ui");
    d = helpdest(a, "settings:defaults#theme");
    expect(t, "settings top", istr(a, d.known && !d.cfgkind.len), "1");
    expect(t, "settings diag", istr(a, helpdest(a, "settings:diagnostics").diag), "1");
    expect(t, "settings whole", istr(a, helpdest(a, "settings:").whole), "1");
    d = helpdest(a, "settings:nope/x#y");
    expect(t, "settings unknown", concat(a, istr(a, d.known), d.find), "0y");
    d = helpdest(a, "view:Hosts");
    expect(t, "view", istr(a, d.kind==DEST_VIEW && d.pane==8), "1");
    expect(t, "view none", istr(a, helpdest(a, "view:nope").pane), "-1");
    expect(t, "web", istr(a, helpdest(a, "HTTPS://example.com/").kind), istr(a, DEST_WEB));
    expect(t, "other", istr(a, helpdest(a, "file:///etc/passwd").kind), istr(a, DEST_NONE));
}
