// Project memory: keys, the index's edits and cut, change notices, files

static void test_memory(Test *t)
{
    Arena *a = &t->perm;

    // Keys: the repository's root, a worktree's main checkout
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/src/app");  // the fake's makes only the last
        os_mkdirs(os, *a, "/src/app/.git");
        os_mkdirs(os, *a, "/src/app/.git/worktrees/fix");
        os_mkdirs(os, *a, "/src/app/sub/deep");
        os_mkdirs(os, *a, "/src/app/.git/modules/lib");
        os_mkdirs(os, *a, "/src/app/lib");
        os_mkdirs(os, *a, "/wt/fix/pkg");
        os_mkdirs(os, *a, "/bare.git/worktrees/b");
        os_mkdirs(os, *a, "/wt/b");
        os_mkdirs(os, *a, "/loose/dir");
        os_write(os, *a, "/src/app/.git/worktrees/fix/commondir", "../..\n");
        os_write(os, *a, "/wt/fix/.git", "gitdir: /src/app/.git/worktrees/fix\n");
        os_write(os, *a, "/src/app/lib/.git", "gitdir: ../.git/modules/lib\n");
        os_write(os, *a, "/bare.git/worktrees/b/commondir", "../..\n");
        os_write(os, *a, "/wt/b/.git", "gitdir: /bare.git/worktrees/b\n");
        expect(t, "memkey repo", memkey(os, a, "/src/app", 0), "-src-app");
        expect(t, "memkey below", memkey(os, a, "/src/app/sub/deep", 0), "-src-app");
        expect(t, "memkey worktree", memkey(os, a, "/wt/fix/pkg", 0), "-src-app");
        expect(t, "memkey submodule", memkey(os, a, "/src/app/lib", 0), "-src-app-lib");
        expect(t, "memkey bare", memkey(os, a, "/wt/b", 0), "-wt-b");
        expect(t, "memkey none", memkey(os, a, "/loose/dir", 0), "-loose-dir");

        os_mkdirs(os, *a, "/Src/My App+x/.git");
        expect(t, "memkey bytes", memkey(os, a, "/Src/My App+x", 0), "-Src-My-App-x");
        expect(t, "memkey windows", memkey(os, a, "/Src/My App+x", 1), "-src-my-app-x");

        Buf b(a, 512);
        print(&b, "/long");
        for (i32 i = 0; i < 30; i++) {
            print(&b, "/directory");
        }
        Str deep = finish(&b);
        os_mkdirs(os, *a, deep);
        Str key = memkey(os, a, deep, 0);
        expect(t, "memkey long", istr(a, key.len), istr(a, MEM_KEYMAX));
        expect(t, "memkey long tail", istr(a, endswith(key, "-directory-directory")), "1");
        Str other = memkey(os, a, parentdir(deep), 0);
        expect(t, "memkey long apart", istr(a, key==other), "0");
    }

    // Names
    {
        expect(t, "name ok", istr(a, !memnamebad("build-steps_2").data), "1");
        expect(t, "name upper", istr(a, !!memnamebad("Build").data), "1");
        expect(t, "name dot", istr(a, !!memnamebad("../x").data), "1");
        expect(t, "name index", istr(a, !!memnamebad("memory").data), "1");
        expect(t, "name empty", istr(a, !!memnamebad("").data), "1");
    }

    // The index: a line replaced in place, appended, removed; the
    // user's own lines and CRLF kept
    {
        Str line = memline(a, "hosts", "Test hosts");
        expect(t, "memline", line, "- [hosts](hosts.md) \xe2\x80\x94 Test hosts");
        MemEdit e = memedit(a, "", "hosts", line);
        expect(t, "index new", e.text, "- [hosts](hosts.md) \xe2\x80\x94 Test hosts\n");
        expect(t, "index new found", istr(a, e.found), "0");

        Str index = "# Notes\n- [a](a.md) \xe2\x80\x94 A\nmy own line\n"
                    "- [hosts](hosts.md) \xe2\x80\x94 old\n- [b](b.md) \xe2\x80\x94 B";
        e = memedit(a, index, "hosts", memline(a, "hosts", "new"));
        expect(t, "index replaced", e.text,
               "# Notes\n- [a](a.md) \xe2\x80\x94 A\nmy own line\n"
               "- [hosts](hosts.md) \xe2\x80\x94 new\n- [b](b.md) \xe2\x80\x94 B\n");
        expect(t, "index replaced found", istr(a, e.found), "1");
        e = memedit(a, index, "c", memline(a, "c", "C"));
        expect(t, "index appended", istr(a, endswith(e.text, "B\n- [c](c.md) \xe2\x80\x94 C\n")), "1");
        e = memedit(a, index, "a", {});
        expect(t, "index removed", e.text,
               "# Notes\nmy own line\n- [hosts](hosts.md) \xe2\x80\x94 old\n"
               "- [b](b.md) \xe2\x80\x94 B\n");
        e = memedit(a, index, "host", {});  // not hosts.md
        expect(t, "index prefix", istr(a, e.found), "0");

        Str crlf = "- [a](a.md) \xe2\x80\x94 A\r\n- [b](b.md) \xe2\x80\x94 B\r\n";
        e = memedit(a, crlf, "a", memline(a, "a", "AA"));
        expect(t, "index crlf", e.text,
               "- [a](a.md) \xe2\x80\x94 AA\r\n- [b](b.md) \xe2\x80\x94 B\r\n");
        e = memedit(a, crlf, "c", memline(a, "c", "C"));
        expect(t, "index crlf appended", istr(a, endswith(e.text, "B\r\n- [c](c.md) \xe2\x80\x94 C\r\n")), "1");
        e = memedit(a, "- [a](a.md) x\n- [a](a.md) y\n", "a", memline(a, "a", "z"));
        expect(t, "index duplicates", e.text, "- [a](a.md) \xe2\x80\x94 z\n");
    }

    // The cut: 200 lines, 25 KB, at a line
    {
        Buf b(a, 64<<10);
        for (i32 i = 0; i < 250; i++) {
            print(&b, "- [m");
            print(&b, (i64)i);
            print(&b, "](m");
            print(&b, (i64)i);
            print(&b, ".md) \xe2\x80\x94 a memory\n");
        }
        Str    index = finish(&b);
        MemCut c     = memcut(index);
        expect(t, "cut lines", istr(a, c.lines), "250");
        expect(t, "cut", istr(a, c.cut), "1");
        expect(t, "cut at 200", istr(a, endswith(c.text, "[m199](m199.md) \xe2\x80\x94 a memory")), "1");
        expect(t, "full", istr(a, memfull(index)), "2");

        b.len = 0;
        for (i32 i = 0; i < 100; i++) {
            print(&b, "- ");
            for (i32 k = 0; k < 40; k++) {
                print(&b, "padding ");
            }
            print(&b, "\n");
        }
        index = finish(&b);
        c     = memcut(index);
        expect(t, "cut bytes", istr(a, c.cut && c.text.len<=MEM_BYTES &&
               endswith(c.text, "padding")), "1");

        c = memcut("- one\n- two\n");
        expect(t, "cut none", c.text, "- one\n- two");
        expect(t, "cut none flag", istr(a, c.cut || c.lines!=2), "0");
        expect(t, "full none", istr(a, memfull("- one\n")), "0");
        b.len = 0;
        for (i32 i = 0; i < 160; i++) {
            print(&b, "- x\n");
        }
        expect(t, "full near", istr(a, memfull(finish(&b))), "1");
    }

    // What changed, as the agent is told
    {
        Str before = "- [a](a.md) \xe2\x80\x94 A\n- [b](b.md) \xe2\x80\x94 B\n";
        Str after  = "- [b](b.md) \xe2\x80\x94 B\r\n\n- [c](c.md) \xe2\x80\x94 C\n";
        Str text   = memchanges(a, before, after);
        expect(t, "changes", istr(a, endswith(text,
               "\n\nAdded:\n- [c](c.md) \xe2\x80\x94 C\n\nRemoved:\n- [a](a.md) \xe2\x80\x94 A")), "1");
        expect(t, "changes none", istr(a, !memchanges(a, before, concat(a, before, "\n")).data), "1");
    }

    // A memory's file, its description quoted
    {
        Str f = memfile(a, "hosts", "Say \"hi\" \\ there", "reference",
                        1792972800000, "20261026-000000-abcd", "Biron.\n\n");
        expect(t, "file", f,
               "---\nname: hosts\ndescription: \"Say \\\"hi\\\" \\\\ there\"\n"
               "type: reference\nmodified: 2026-10-26T00:00:00Z\n"
               "session: 20261026-000000-abcd\n---\n\nBiron.\n");
    }

    // Times
    {
        Buf b(a, 64);
        printiso(&b, 0);
        expect(t, "iso epoch", finish(&b), "1970-01-01T00:00:00Z");
        b.len = 0;
        printiso(&b, 951782400000 + 86399999);  // 2000-02-29, its last ms
        expect(t, "iso leap", finish(&b), "2000-02-29T23:59:59Z");
        b.len = 0;
        printiso(&b, -1);
        expect(t, "iso before", finish(&b), "1969-12-31T23:59:59Z");
    }
}
