// File references: links parsed as models write them and Claude Desktop
// and opencode take them, resolved on POSIX and Windows hosts, and the
// inline-code spans that may name files.

// A reference as "path", then " L:a-b" for lines, " C:c" for a column,
// " #s" for a section; or its kind alone.
static Str frefstr(Arena *a, FileRef r)
{
    switch (r.kind) {
    case FREF_NET:    return "NET";
    case FREF_BAD:    return "BAD";
    case FREF_SCHEME: return "SCHEME";
    }
    Buf b(a, 64);
    print(&b, r.path);
    if (r.line0) {
        print(&b, " L:");
        print(&b, r.line0);
        print(&b, "-");
        print(&b, r.line1);
    }
    if (r.col) {
        print(&b, " C:");
        print(&b, r.col);
    }
    if (r.section.data) {
        print(&b, " #");
        print(&b, r.section);
    }
    return finish(&b);
}

static void test_fileref(Test *t)
{
    Arena *a = &t->perm;

    struct { Str href; Str want; } refs[] = {
        // opencode's resolve-link corpus
        {"packages/app/src/app.tsx", "packages/app/src/app.tsx"},
        {"session/timeline/interaction.ts:74", "session/timeline/interaction.ts L:74-74"},
        {"packages/app/src/workspaces/files/model.tsx:164:11",
         "packages/app/src/workspaces/files/model.tsx L:164-164 C:11"},
        {"src/components/markdown.tsx:398-410", "src/components/markdown.tsx L:398-410"},
        {"src/components/markdown.tsx#L398", "src/components/markdown.tsx L:398-398"},
        {"src/components/markdown.tsx#L398-L410", "src/components/markdown.tsx L:398-410"},
        {"file:///C:/repo/a/b.ts?x=1#L3", "C:/repo/a/b.ts L:3-3"},
        {"what?.md", "what?.md"},
        {"a#b.md#L10", "a#b.md L:10-10"},
        {"foo.ts:0", "foo.ts"},
        {"./a/../b.ts", "./a/../b.ts"},  // folded by filepath
        {"..", ".."},
        {"C:12", "BAD"},                 // opencode keeps it; a drive and no path
        {"../timeline/interaction.ts:74", "../timeline/interaction.ts L:74-74"},
        {"src/C#/a.cs", "src/C#/a.cs"},
        {"foo#bar.ts", "foo#bar.ts"},
        {"foo.ts:12:", "foo.ts L:12-12"},
        {"foo.ts:99999999999999999999", "foo.ts"},
        {"\\\\server\\share\\x.ts", "\\\\server\\share\\x.ts"},  // refused by filepath
        {"file://server/share/x%20y.ts", "NET"},
        {"a%2520b.ts", "a%20b.ts"},
        {"C:/foo/../../x.ts", "C:/foo/../../x.ts"},
        {"file:///C:/tmp/demo%20file.ts:12", "C:/tmp/demo file.ts L:12-12"},

        // Claude Desktop's forms
        {"a.ts:12:", "a.ts L:12-12"},
        {"my%20f.cpp:3", "my f.cpp L:3-3"},
        {"x%0Ay", "BAD"},
        {"x%7fy", "BAD"},
        {"x\ty", "BAD"},
        {"javascript:", "SCHEME"},
        {"javascript:alert(1)", "SCHEME"},
        {"#L10", "BAD"},
        {"#intro", "BAD"},
        {"src/", "BAD"},
        {"src\\", "BAD"},
        {"src%2F", "BAD"},
        {"~bob/x", "~bob/x"},  // refused by filepath
        {"", "BAD"},

        // file: URLs
        {"file:///home/u/a.c", "/home/u/a.c"},
        {"file:/home/u/a.c", "/home/u/a.c"},
        {"FILE:///x", "/x"},
        {"file://localhost/etc/hosts:3", "/etc/hosts L:3-3"},
        {"file://LocalHost/x", "/x"},
        {"file://host/x", "NET"},
        {"file://localhost", "BAD"},
        {"file:x.c", "BAD"},
        {"file:", "BAD"},
        {"file:///C:x", "BAD"},
        {"file:///a%23b.c#L2", "/a#b.c L:2-2"},
        {"file:///c%3A/w/a.c", "c:/w/a.c"},     // as VS Code writes them
        {"file:///C%3a/x#L2", "C:/x L:2-2"},
        {"file:///c%3A", "BAD"},

        // Drives and schemes
        {"C:/x/y.c", "C:/x/y.c"},
        {"c:\\x\\y.c:7", "c:\\x\\y.c L:7-7"},
        {"C:x", "BAD"},
        {"C:", "BAD"},
        {"README:10", "README L:10-10"},
        {"Makefile:3:5", "Makefile L:3-3 C:5"},
        {"a.c:3-9", "a.c L:3-9"},
        {"mailto:x", "SCHEME"},
        {"https://example.com/a.c", "SCHEME"},
        {"vscode://file/a.c:3", "SCHEME"},
        {"x-y.z+w:4", "x-y.z+w L:4-4"},

        // Fragments
        {"a.c#L10-L20", "a.c L:10-20"},
        {"a.c#L10-20", "a.c L:10-20"},
        {"a.c#L10C3", "a.c L:10-10 C:3"},
        {"a.c#L10C3-L12C1", "a.c L:10-12 C:3"},
        {"a.c#L20-L10", "a.c L:10-20"},
        {"a.c#L0", "a.c"},
        {"a.c#L10000001", "a.c"},
        {"a.c#L10000000", "a.c L:10000000-10000000"},
        {"a.c#L5-L0", "a.c L:5-5"},
        {"a.c#Lx", "a.c #Lx"},
        {"a.c#L5-", "a.c #L5-"},
        {"README.md#getting-started", "README.md #getting-started"},
        {"doc.md#caf%C3%A9", "doc.md #caf\xc3\xa9"},
        {"doc.md#a%0Ab", "BAD"},
        {"a.c#", "a.c#"},
        {"a.c#L3:9", "a.c#L3 L:9-9"},
        {"README:10#L5", "README L:5-5"},
        {"notes.md:3#intro", "notes.md L:3-3 #intro"},
        {"mailto:x#y", "SCHEME"},

        // Line suffixes
        {"a.c:20-10", "a.c L:10-20"},
        {"a.c:0", "a.c"},
        {"a.c:0:5", "a.c"},
        {"a.c:10000001", "a.c"},
        {"a.c:3-0", "a.c L:3-3"},
        {"a.c:3:", "a.c L:3-3"},
        {"a.c:", "SCHEME"},           // as "javascript:" is
        {"/w/a.c:", "/w/a.c"},
        {"a.c:x", "SCHEME"},
        {"x-1:3", "x-1 L:3-3"},
        {"a:3", "BAD"},
        {"/a:3", "/a L:3-3"},
        {":10", "BAD"},
        {"a.c:3:4:5", "SCHEME"},
    };
    for (iz i = 0; i < countof(refs); i++) {
        Str got = frefstr(a, fileref(a, refs[i].href));
        expect(t, concat(a, "fileref ", refs[i].href), got, refs[i].want);
    }

    // Resolved on a host: base is the working directory, "" for none
    struct {
        Str href; Str base; Str home; b32 win; b32 unasked; Str want;
    } paths[] = {
        {"src/a.c:3",       "/w",    "/home/u", 0, 0, "/w/src/a.c"},
        {"./a/../b.ts",     "/w",    "/home/u", 0, 0, "/w/b.ts"},
        {"/etc/hosts",      "/w",    "/home/u", 0, 0, "/etc/hosts"},
        {"/etc/hosts",      "",      "/home/u", 0, 0, "/etc/hosts"},
        {"a.c",             "",      "/home/u", 0, 0, "err: it is relative, and there is no working directory"},
        {"~/notes.md",      "/w",    "/home/u", 0, 0, "/home/u/notes.md"},
        {"~",               "/w",    "/home/u", 0, 0, "/home/u"},
        {"~bob/x",          "/w",    "/home/u", 0, 0, "err: it is in another user's home"},
        {"~/x",             "/w",    "",        0, 0, "err: the home directory is not known"},
        {"~/x",             "/w",    "/home/u", 0, 1, "err: it is outside the working directory"},
        {"file://h/x",      "/w",    "/home/u", 0, 0, "err: it is on another computer"},
        {"mailto:x",        "/w",    "/home/u", 0, 0, "err: it is not a file"},
        {"src/",            "/w",    "/home/u", 0, 0, "err: it does not name a file"},
        {"//srv/share/x",   "/w",    "/home/u", 0, 0, "/srv/share/x"},
        {"\\\\srv\\s\\x",   "/w",    "/home/u", 0, 0, "/w/\\\\srv\\s\\x"},
        {"/c/x",            "/w",    "/home/u", 0, 0, "/c/x"},
        {"src/a.c",         "/w",    "/home/u", 0, 1, "/w/src/a.c"},
        {".",               "/w",    "/home/u", 0, 1, "/w"},
        {"../x/a.c",        "/w",    "/home/u", 0, 1, "err: it is outside the working directory"},
        {"src/../../w2/a",  "/w",    "/home/u", 0, 1, "err: it is outside the working directory"},
        {"/w/a.c",          "/w/",   "/home/u", 0, 1, "/w/a.c"},
        {"/w2/a.c",         "/w",    "/home/u", 0, 1, "err: it is outside the working directory"},
        {"/etc/hosts",      "",      "/home/u", 0, 1, "err: it is outside the working directory"},

        // Windows hosts
        {"src\\a.c:3",      "C:/w",  "C:/Users/u", 1, 0, "C:/w/src/a.c"},
        {"c:\\x\\y.c",      "C:/w",  "C:/Users/u", 1, 0, "C:/x/y.c"},
        {"/c/x/y.c",        "C:/w",  "C:/Users/u", 1, 0, "C:/x/y.c"},
        {"/cygdrive/d/q",   "C:/w",  "C:/Users/u", 1, 0, "D:/q"},
        {"\\x",             "C:/w",  "C:/Users/u", 1, 0, "C:/x"},
        {"\\x",             "",      "C:/Users/u", 1, 0, "err: it is relative, and there is no working directory"},
        {"~\\notes.md",     "C:/w",  "C:/Users/u", 1, 0, "C:/Users/u/notes.md"},
        {"\\\\srv\\s\\x",   "C:/w",  "C:/Users/u", 1, 0, "err: it is a network path"},
        {"//srv/s/x",       "C:/w",  "C:/Users/u", 1, 0, "err: it is a network path"},
        {"\\\\?\\UNC\\srv\\s\\x", "C:/w", "C:/Users/u", 1, 0, "err: it is a network path"},
        {"\\\\?\\GLOBALROOT\\Device\\Mup\\h\\s\\x", "C:/w", "C:/Users/u", 1, 0, "err: it is a network path"},
        {"\\\\wsl$\\Ubuntu\\x", "C:/w", "C:/Users/u", 1, 0, "//wsl$/Ubuntu/x"},
        {"\\\\wsl.localhost\\U\\x", "C:/w", "C:/Users/u", 1, 1, "err: it is outside the working directory"},
        {"file:///C:/w/a.c#L4", "C:/w", "C:/Users/u", 1, 1, "C:/w/a.c"},
        {"file:///c%3A/w/a.c", "C:/w", "C:/Users/u", 1, 1, "C:/w/a.c"},
        {"c:/W/a.c",        "C:/w",  "C:/Users/u", 1, 1, "C:/W/a.c"},
        {"..\\a.c",         "C:/w",  "C:/Users/u", 1, 1, "err: it is outside the working directory"},
        {"D:/w/a.c",        "C:/w",  "C:/Users/u", 1, 1, "err: it is outside the working directory"},
    };
    for (iz i = 0; i < countof(paths); i++) {
        FileRef  ref = fileref(a, paths[i].href);
        FilePath fp  = filepath(a, ref, paths[i].base, paths[i].home, paths[i].win,
                                paths[i].unasked);
        Str got  = fp.err.data ? concat(a, "err: ", fp.err) : fp.path;
        Str name = concat(a, paths[i].win ? Str("filepath win ") : Str("filepath posix "),
                          concat(a, paths[i].href, paths[i].unasked ? Str(" unasked") : Str("")));
        expect(t, name, got, paths[i].want);
    }

    // Inline-code spans that may name a file
    struct { Str span; b32 want; } spans[] = {
        {"src/main.c", 1},
        {"main.c", 1},
        {"main.c:42", 1},
        {"main.c:42:7", 1},
        {"tools.cpp:10-20", 1},
        {"Makefile", 0},
        {"src/", 1},
        {"a/b", 1},
        {"~/notes.md", 1},
        {"C:\\w\\x.py", 1},
        {"README.markdown", 1},
        {"x.abcdefghij", 1},
        {"x.abcdefghijk", 0},
        {"v1.2", 0},
        {"x.7z", 1},
        {"x.", 0},
        {"foo()", 0},
        {"a.c b.c", 0},
        {"*.c", 0},
        {"a.c;", 0},
        {"$HOME/x", 0},
        {"{a,b}.c", 0},
        {"<a.h>", 0},
        {"a|b.c", 0},
        {"\"a.c\"", 0},
        {"what?.md", 0},
        {"https://x.com/a.c", 0},
        {"-o.c", 0},
        {"--out=x.c", 0},
        {"::std.x", 0},
        {"std::vector", 0},
        {"", 0},
        {"a\tb.c", 0},
        {"cite/hello.c:7", 1},
        {"hello.txt", 1},
        {"#L3.c", 1},
        {"e.g.", 0},
        {"1.5", 0},
        {"i.e", 1},
    };
    for (iz i = 0; i < countof(spans); i++) {
        Str got  = codepath(spans[i].span) ? Str("path") : Str("not");
        Str want = spans[i].want ? Str("path") : Str("not");
        expect(t, concat(a, "codepath ", spans[i].span), got, want);
    }
    {
        Buf b(a, 300);
        for (i32 i = 0; i < 250; i++) {
            putbyte(&b, 'a');
        }
        print(&b, ".c");
        expect(t, "codepath 252 bytes", codepath(finish(&b)) ? Str("path") : Str("not"), "path");
        print(&b, "cccc");
        expect(t, "codepath 256 bytes", codepath(finish(&b)) ? Str("path") : Str("not"), "not");
    }
}
