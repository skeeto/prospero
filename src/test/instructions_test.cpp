// Instruction files: discovery, imports, the cap, and nested loading

static Str instrfiles(Arena *a, Slice<InstrFile> files)
{
    Buf b(a, 256);
    for (iz i = 0; i < files.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, files[i].path);
        if (files[i].importer.data) {
            print(&b, "<");
            print(&b, files[i].importer);
        }
    }
    return finish(&b);
}

static Str strlist(Arena *a, Slice<Str> list)
{
    Buf b(a, 256);
    for (iz i = 0; i < list.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, list[i]);
    }
    return finish(&b);
}

static void test_instructions(Test *t)
{
    Arena *a = &t->perm;

    {
        Slice<Str> imports = instrimports(a,
            "See @a.md, then `@b.md` and @c/d.txt) or foo@bar.com @mention\n"
            "```\n@fenced.md\n```\n"
            "~~~~\n@tilde.md\n```\nstill @in.md\n~~~~\n"
            "(@paren/e.md) @~/home.md @x@y.md ``code @f.md`` @g.md.\n");
        expect(t, "imports", strlist(a, imports), "a.md c/d.txt paren/e.md ~/home.md g.md");
        expect(t, "names", strlist(a, instrnames(a, " AGENTS.md, CLAUDE.md ../x a/b .")),
               "AGENTS.md CLAUDE.md");
    }

    Os *os = newos(a, 1<<20);
    os_mkdirs(os, *a, "/r");
    os_mkdirs(os, *a, "/r/.git");
    os_mkdirs(os, *a, "/r/docs");
    os_mkdirs(os, *a, "/r/sub");
    os_mkdirs(os, *a, "/r/sub/deeper");
    os_mkdirs(os, *a, "/g");
    os_write(os, *a, "/g/AGENTS.md", "global rules @/r/docs/no.md\n");
    os_write(os, *a, "/r/AGENTS.md",
             "root rules @docs/a.md `@docs/no.md` @../out.md @missing.md\n"
             "```\n@docs/fence.md\n```\n");
    os_write(os, *a, "/r/CLAUDE.md", "not loaded\n");
    os_write(os, *a, "/r/docs/a.md", "doc A @b.md @a.md\n");
    os_write(os, *a, "/r/docs/b.md", "doc B\n");
    os_write(os, *a, "/r/docs/no.md", "no\n");
    os_write(os, *a, "/r/docs/fence.md", "fence\n");
    os_write(os, *a, "/out.md", "outside\n");
    os_write(os, *a, "/r/sub/CLAUDE.md", "sub rules\n");
    os_write(os, *a, "/r/sub/deeper/AGENTS.md", "deeper rules\n");
    Slice<Str> names = instrnames(a, "AGENTS.md CLAUDE.md");

    {
        Instructions in = instructions(os, a, "/r/sub", "/g/AGENTS.md", names, "", 0);
        expect(t, "instr files", instrfiles(a, in.files),
               "/g/AGENTS.md /r/docs/no.md</g/AGENTS.md /r/AGENTS.md "
               "/r/docs/a.md</r/AGENTS.md /r/docs/b.md</r/docs/a.md /r/sub/CLAUDE.md");
        expect(t, "instr skipped", strlist(a, in.skipped), "/out.md");
        b32 ok = find(in.text, "global rules")>=0 && find(in.text, "root rules")>=0 &&
                 find(in.text, "doc A")>=0 && find(in.text, "doc B")>=0 &&
                 find(in.text, "sub rules")>=0 && find(in.text, "fence\n")<0 &&
                 find(in.text, "not loaded")<0 && find(in.text, "outside")<0 &&
                 find(in.text, "deeper")<0;
        expect(t, "instr text", istr(a, ok), "1");
        expect(t, "instr heading", istr(a, find(in.text,
               "## /g/AGENTS.md (the user's own instructions)\n\nglobal rules")>=0), "1");
        expect(t, "instr imported heading", istr(a, find(in.text,
               "## /r/docs/b.md (imported by /r/docs/a.md)\n\ndoc B")>=0), "1");

        // Without a repository, only the working directory itself
        Instructions bare = instructions(os, a, "/g", {}, names, "", 0);
        expect(t, "instr no repository", instrfiles(a, bare.files), "/g/AGENTS.md");
        Instructions none = instructions(os, a, "/r/docs", {}, instrnames(a, "README"), "", 0);
        expect(t, "instr none", istr(a, none.files.len + none.text.len), "0");
    }

    {
        Slice<Str> loaded = {};
        loaded = push(a, loaded, Str("/r/AGENTS.md"));
        loaded = push(a, loaded, Str("/r/sub/CLAUDE.md"));
        Instructions n = instrnested(os, a, "/r", "/r/sub/deeper/x.c", names, loaded, "", 0);
        expect(t, "nested", instrfiles(a, n.files), "/r/sub/deeper/AGENTS.md");
        expect(t, "nested text", istr(a, find(n.text, "deeper rules")>=0), "1");
        n = instrnested(os, a, "/r", "/r/docs/a.md", names, loaded, "", 0);
        expect(t, "nested none", istr(a, n.files.len), "0");
        n = instrnested(os, a, "/r/sub", "/r/x.c", names, loaded, "", 0);
        expect(t, "nested outside", istr(a, n.files.len), "0");
    }

    // Five hops of imports at most
    {
        os_mkdirs(os, *a, "/c");
        os_mkdirs(os, *a, "/c/.git");
        for (i32 i = 0; i < 8; i++) {
            Buf p(a, 32), d(a, 32);
            print(&p, "/c/");
            print(&p, (i64)i);
            print(&p, ".md");
            print(&d, "file ");
            print(&d, (i64)i);
            print(&d, " @");
            print(&d, (i64)(i+1));
            print(&d, ".md\n");
            os_write(os, *a, finish(&p), finish(&d));
        }
        os_write(os, *a, "/c/AGENTS.md", "@0.md\n");
        Instructions in = instructions(os, a, "/c", {}, names, "", 0);
        expect(t, "hops", istr(a, in.files.len), "6");
    }

    // The cap, cut at a line
    {
        os_mkdirs(os, *a, "/big");
        Buf b(a, 80<<10);
        for (i32 i = 0; i < 2000; i++) {
            print(&b, "a line of instructions that goes on for a while\n");
        }
        os_write(os, *a, "/big/AGENTS.md", finish(&b));
        Instructions in = instructions(os, a, "/big", {}, names, "", 0);
        expect(t, "cap", istr(a, in.cut && in.text.len<INSTR_MAX+200 &&
               endswith(in.text, " more bytes were not loaded.]")), "1");
    }
}
