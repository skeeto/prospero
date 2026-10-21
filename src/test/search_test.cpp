// Search tests: the regex NFA, globs, .gitignore rules, the walker over
// the in-memory file system, and reading ripgrep's output

// "1" if the pattern matches the line, "0" if not, or the error.
static Str rxshow(Arena *a, Str pattern, b32 fold, Str line)
{
    Regex rx = rxcompile(a, pattern, fold);
    if (rx.err.data) {
        return concat(a, "error: ", rx.err);
    }
    RxRun r = rxrun(a, &rx);
    return rxmatch(&r, line) ? Str("1") : Str("0");
}

static Str globshow(Arena *a, Str pattern)
{
    Slice<Str> g = globexpand(a, pattern);
    Buf b(a, 128);
    for (iz i = 0; i < g.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, g[i]);
    }
    return finish(&b);
}

static Str searchshow(Os *os, Arena *a, i32 mode, Str root, Str pattern,
                      Str glob, b32 fold, b32 literal)
{
    SearchOpts o = {};
    o.mode    = mode;
    o.root    = root;
    o.pattern = pattern;
    o.glob    = glob;
    o.fold    = fold;
    o.literal = literal;
    Arena scratch = {};
    scratch.beg = (byte *)allocbytes(a, 1<<20);
    scratch.end = scratch.beg + (1<<20);
    Buf out(a, 1<<12);
    iz  found = 0;
    Str err   = searchrun(os, a, scratch, &o, &out, 1, &found);
    if (err.data) {
        return concat(a, "error: ", err);
    }
    Str s = finish(&out);
    Buf b(a, s.len + 16);
    for (iz i = 0; i < s.len; i++) {
        putbyte(&b, s[i] ? s[i] : '|');  // NUL after each path
    }
    return finish(&b);
}

static void test_search(Test *t)
{
    Arena *a = &t->perm;

    // Regular expressions
    {
        struct {
            Str pattern;
            b32 fold;
            Str line;
            Str want;
        } cases[] = {
            {"abc", 0, "xxabcxx", "1"},
            {"abc", 0, "xxabxcx", "0"},
            {"a.c", 0, "abc", "1"},
            {"a.c", 0, "ac", "0"},
            {"^ab", 0, "abc", "1"},
            {"^ab", 0, "cab", "0"},
            {"bc$", 0, "abc", "1"},
            {"bc$", 0, "bcd", "0"},
            {"a*b", 0, "b", "1"},
            {"a+b", 0, "b", "0"},
            {"a+b", 0, "xaab", "1"},
            {"colou?r", 0, "color", "1"},
            {"colou?r", 0, "colour", "1"},
            {"colou?r", 0, "colouur", "0"},
            {"(ab|cd)+e", 0, "xcdabe", "1"},
            {"(ab|cd)+e", 0, "ace", "0"},
            {"[a-c]x", 0, "bx", "1"},
            {"[^a-c]x", 0, "bx", "0"},
            {"[^a-c]x", 0, "dx", "1"},
            {"[]a]", 0, "]", "1"},
            {"[a-]", 0, "-", "1"},
            {"\\d{3}-\\d{4}", 0, "call 555-1234", "1"},
            {"\\d{3}-\\d{4}", 0, "555-123", "0"},
            {"^x{2,3}y", 0, "xy", "0"},
            {"^x{2,3}y", 0, "xxy", "1"},
            {"^x{2,3}y", 0, "xxxxy", "0"},
            {"x{2,}y", 0, "xxxxxy", "1"},
            {"\\bfoo\\b", 0, "a foo b", "1"},
            {"\\bfoo\\b", 0, "afoob", "0"},
            {"\\Bfoo", 0, "afoo", "1"},
            {"\\Bfoo", 0, "foo", "0"},
            {"\\w+\\s\\S", 0, "ab c", "1"},
            {"\\W", 0, "abc", "0"},
            {"HELLO", 1, "say hello", "1"},
            {"[A-C]x", 1, "bX", "1"},
            {"hello", 0, "HELLO", "0"},
            {"^.$", 0, "\xc3\xa9", "1"},
            {"\xc3\xa9.", 0, "\xc3\xa9x", "1"},
            {"[\xc3\xa0-\xc3\xbf]", 0, "caf\xc3\xa9", "1"},
            {"a|", 0, "x", "1"},
            {"(?:ab)c", 0, "abc", "1"},
            {"a\\.b", 0, "a.b", "1"},
            {"a\\.b", 0, "axb", "0"},
            {"\\x41", 0, "A", "1"},
            {"a{", 0, "a{", "1"},
            {"a{x}", 0, "a{x}", "1"},
            {"a*?b", 0, "aab", "1"},
            {"^$", 0, "", "1"},
            {"(", 0, "", "error: unclosed ( group"},
            {")", 0, "", "error: unmatched )"},
            {"*a", 0, "", "error: nothing before * + or ? to repeat"},
            {"[a", 0, "", "error: unclosed [ class"},
            {"a{3,2}", 0, "", "error: in {n,m}, m is less than n"},
            {"a{1001}", 0, "", "error: a repetition count is over 1000"},
            {"(?=x)", 0, "", "error: only (?: ) groups are supported, not (?"},
            {"^*", 0, "", "error: an anchor cannot be repeated"},
            {"[z-a]", 0, "", "error: a range in [ ] runs backward"},
            {"(a{1000}){1000}", 0, "", "error: the pattern is too large: use "
                                       "fewer or smaller {n,m} counts"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena tmp = *a;
            Str   got = rxshow(&tmp, cases[i].pattern, cases[i].fold, cases[i].line);
            expect(t, concat(&tmp, "rx ", cases[i].pattern), got, cases[i].want);
        }
        Arena tmp = *a;
        Str   err = rxshow(&tmp, "\\q", 0, "");
        expect(t, "rx unknown escape", istr(&tmp, startswith(err, "error: unknown escape")), "1");

        // Linear: nested stars cannot blow up
        Buf b(&tmp, 1<<16);
        for (i32 i = 0; i < 20000; i++) putbyte(&b, 'a');
        expect(t, "rx nested stars", rxshow(&tmp, "(a*)*b", 0, finish(&b)), "0");
        expect(t, "rx many options", rxshow(&tmp, "(a|aa)*c", 0, finish(&b)), "0");
    }

    // The literal every match contains
    {
        struct {
            Str pattern;
            b32 fold;
            Str want;
        } cases[] = {
            {"foo.*bar", 0, "foo"},
            {"x(ab)+y", 0, "xab"},
            {"a|b", 0, ""},
            {"(abc)?d", 0, "d"},
            {"^main\\(", 0, "main("},
            {"\\bTODO\\b", 0, "TODO"},
            {"HeLLo", 1, "hello"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena tmp = *a;
            Regex rx  = rxcompile(&tmp, cases[i].pattern, cases[i].fold);
            expect(t, concat(&tmp, "rx literal ", cases[i].pattern), rx.lit, cases[i].want);
        }
    }

    // Globs
    {
        expect(t, "glob braces", globshow(a, "*.{c,h}"), "*.c *.h");
        expect(t, "glob braces twice", globshow(a, "{a,b}{c,d}"), "ac ad bc bd");
        expect(t, "glob braces nested", globshow(a, "x{a,{b,c}}"), "xa xb xc");
        expect(t, "glob brace open", globshow(a, "x{"), "x{");
        expect(t, "glob brace quoted", globshow(a, "\\{a,b}"), "\\{a,b}");

        struct {
            Str glob;
            Str path;
            b32 want;
        } cases[] = {
            {"*.c", "src/a.c", 1},
            {"*.c", "a.h", 0},
            {"src/*.c", "src/a.c", 1},
            {"src/*.c", "src/x/a.c", 0},
            {"src/*.c", "x/src/a.c", 0},
            {"**/*.c", "a.c", 1},
            {"**/*.c", "x/y/a.c", 1},
            {"src/**", "src/a/b", 1},
            {"src/**", "src", 0},
            {"a/**/b", "a/b", 1},
            {"a/**/b", "a/x/y/b", 1},
            {"a/**/b", "a/x/y/c", 0},
            {"/a.c", "a.c", 1},
            {"/a.c", "x/a.c", 0},
            {"?.c", "a.c", 1},
            {"?.c", "ab.c", 0},
            {"[ab].c", "b.c", 1},
            {"[!ab].c", "b.c", 0},
            {"[!ab].c", "c.c", 1},
            {"\\*.c", "*.c", 1},
            {"\\*.c", "a.c", 0},
            {"build/", "x/build", 1},
            {"a*b*c", "abxbxc", 1},
            {"*", "x/y", 1},
            {"x**y", "x/y", 0},
            {"[a", "[a", 1},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena tmp = *a;
            Buf   n(&tmp, 64);
            print(&n, "glob ");
            print(&n, cases[i].glob);
            print(&n, " ~ ");
            print(&n, cases[i].path);
            expect(t, finish(&n), istr(&tmp, globone(cases[i].glob, cases[i].path)),
                   istr(&tmp, cases[i].want));
        }
    }

    // .gitignore rules: the last match wins, and deeper files win
    {
        Arena   tmp  = *a;
        Ignore *root = alloc<Ignore>(&tmp);
        root->dir   = "/r";
        root->rules = ignoreparse(&tmp, "# comment\n*.log\n!keep.log\nbuild/\n"
                                        "/top.txt\ndocs/*.tmp\n\\#hash\n  \n");
        Ignore *sub = alloc<Ignore>(&tmp);
        sub->dir   = "/r/sub";
        sub->next  = root;
        sub->rules = ignoreparse(&tmp, "!*.log\n");
        struct {
            Str path;
            b32 dir;
            b32 want;
        } cases[] = {
            {"/r/a.log", 0, 1},
            {"/r/x/keep.log", 0, 0},
            {"/r/build", 1, 1},
            {"/r/build", 0, 0},
            {"/r/x/build", 1, 1},
            {"/r/top.txt", 0, 1},
            {"/r/x/top.txt", 0, 0},
            {"/r/docs/a.tmp", 0, 1},
            {"/r/x/docs/a.tmp", 0, 0},
            {"/r/#hash", 0, 1},
            {"/r/a.c", 0, 0},
            {"/r/sub/a.log", 0, 0},
        };
        for (iz i = 0; i < countof(cases); i++) {
            Arena t2 = tmp;
            Str   n  = concat(&t2, "ignored ", cases[i].path);
            expect(t, n, istr(&t2, ignored(sub, cases[i].path, cases[i].dir)),
                   istr(&t2, cases[i].want));
        }
    }

    // The walker over a file system in memory
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/w");
        os_mkdirs(os, *a, "/w/.git");
        os_mkdirs(os, *a, "/w/src");
        os_mkdirs(os, *a, "/w/ignored");
        os_mkdirs(os, *a, "/w/deep");
        os_write(os, *a, "/w/.gitignore", "ignored/\n*.log\n");
        os->now = 1000000;
        os_write(os, *a, "/w/src/b.h", "void hello(void);\n");
        os->now = 2000000;
        os_write(os, *a, "/w/src/c.c", "static int helper(void);\n");
        os->now = 3000000;
        os_write(os, *a, "/w/src/a.c", "int main(void)\r\n{\n    // TODO: fix\n}\n");
        os_write(os, *a, "/w/ignored/x.c", "int main(void) {}\n");
        os_write(os, *a, "/w/trace.log", "main(\n");
        os_write(os, *a, "/w/deep/n.txt", "needle\n");
        os_write(os, *a, "/w/bin.dat", Str{(u8 *)"main(\0\n", 7});
        os_write(os, *a, "/w/.git/config", "main(\n");

        expect(t, "walk grep", searchshow(os, a, SEARCH_GREP, "/w", "ma.n\\(", {}, 0, 0),
               "src/a.c|1:int main(void)\n");
        expect(t, "walk grep glob", searchshow(os, a, SEARCH_GREP, "/w", "e", "*.h", 0, 0),
               "src/b.h|1:void hello(void);\n");
        expect(t, "walk grep fold", searchshow(os, a, SEARCH_GREP, "/w", "todo", {}, 1, 0),
               "src/a.c|3:    // TODO: fix\n");
        expect(t, "walk grep literal", searchshow(os, a, SEARCH_GREP, "/w", "(void)", {}, 0, 1),
               "src/a.c|1:int main(void)\nsrc/b.h|1:void hello(void);\n"
               "src/c.c|1:static int helper(void);\n");
        expect(t, "walk grep file", searchshow(os, a, SEARCH_GREP, "/w/src/b.h", "hello", {}, 0, 0),
               "b.h|1:void hello(void);\n");
        expect(t, "walk grep error", searchshow(os, a, SEARCH_GREP, "/w", "(", {}, 0, 0),
               "error: unclosed ( group");
        expect(t, "walk glob", searchshow(os, a, SEARCH_GLOB, "/w", "**/*.{c,h}", {}, 0, 0),
               "src/a.c|src/c.c|src/b.h|");
        expect(t, "walk glob path", searchshow(os, a, SEARCH_GLOB, "/w", "deep/*", {}, 0, 0),
               "deep/n.txt|");
        expect(t, "walk glob none", searchshow(os, a, SEARCH_GLOB, "/w", "*.zip", {}, 0, 0), "");
    }

    // ripgrep's output as the agent sees it
    {
        Os *os = newos(a, 1<<20);
        os_write(os, *a, "/s/out.log",
                 "./src/a.c\0" "1:int main(void)\r\n" "src/b.h\0" "7:x\n"
                 "rg: some warning\n");
        SearchEnd e = {};
        e.tool    = TOOL_GREP;
        e.root    = "/w";
        e.cwd     = "/w";
        e.pattern = "x";
        ToolOut o = searchresult(os, a, "/s/out.log", &e);
        expect(t, "result grep", o.text, "src/a.c:1: int main(void)\nsrc/b.h:7: x\n");

        // For hashline edits, the files' tags; one gone is left out
        os_write(os, *a, "/w/src/a.c", "int main(void)\n");
        e.tagged = 1;
        o = searchresult(os, a, "/s/out.log", &e);
        expect(t, "result grep tagged", o.text, concat(a, concat(a,
               "src/a.c:1: int main(void)\nsrc/b.h:7: x\nTags for edit: [src/a.c#",
               filetag(a, "int main(void)\n")), "]\n"));
        e.tagged = 0;

        e.root = "/w/sub";
        o = searchresult(os, a, "/s/out.log", &e);
        expect(t, "result grep sub", o.text,
               "sub/src/a.c:1: int main(void)\nsub/src/b.h:7: x\n");

        os_write(os, *a, "/s/none.log", "");
        e.root = "/w";
        e.code = 1;
        o = searchresult(os, a, "/s/none.log", &e);
        expect(t, "result none", o.text, "No lines match x in the working directory. "
               "The search is case-sensitive: set ignore_case to true to match "
               "letters in either case.");

        os_write(os, *a, "/s/err.log", "regex parse error: unclosed ( group\n");
        e.code = 2;
        o = searchresult(os, a, "/s/err.log", &e);
        expect(t, "result error", istr(a, o.ok*10 + startswith(o.text,
               "The search failed, so there are no results: regex parse error")), "1");

        os_write(os, *a, "/s/glob.log", Str{(u8 *)"./x/a.c\0./b.h\0./c.c\0", 21});
        e.tool = TOOL_GLOB;
        e.code = 0;
        e.glob = "*.c";
        o = searchresult(os, a, "/s/glob.log", &e);
        expect(t, "result glob", o.text, "x/a.c\nc.c\n");

        e.windows = 1;
        e.root    = "C:/w";
        e.cwd     = "C:/w";
        os_write(os, *a, "/s/win.log", Str{(u8 *)".\\x\\a.c\0", 8});
        o = searchresult(os, a, "/s/win.log", &e);
        expect(t, "result windows", o.text, "x/a.c\n");
    }
}
