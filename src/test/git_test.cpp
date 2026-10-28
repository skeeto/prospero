// Git: the branch from a repository's files, git's output parsed, the
// changes as JSON within their budget, finding git, its errors, and the
// git command (git.cpp)

static Str githeadstr(Os *os, Arena *a, Str cwd, Str ceiling, b32 win)
{
    GitHead h = githead(os, a, cwd, ceiling, win);
    return githeadjson(a, &h);
}

// Files as parsed, a line each: kind, status, path, and where from.
static Str gitfilesstr(Arena *a, GitStatus *st)
{
    Buf b(a, 1024);
    for (iz i = 0; i < st->files.len; i++) {
        GitFile *f = &st->files[i];
        putbyte(&b, f->kind);
        putbyte(&b, ' ');
        putbyte(&b, f->x);
        putbyte(&b, f->y);
        putbyte(&b, ' ');
        print(&b, f->path);
        if (f->orig.len) {
            print(&b, " <- ");
            print(&b, f->orig);
        }
        if (f->bin) {
            print(&b, " bin");
        } else if (f->counted) {
            print(&b, " +");
            print(&b, f->add);
            print(&b, " -");
            print(&b, f->del);
        }
        putbyte(&b, '|');
    }
    return finish(&b);
}

static void testgithead(Test *t)
{
    Arena *a = &t->perm;

    // Found from below, through a worktree's file (by its path, and
    // relative), past a .git with no HEAD and one leading nowhere, and
    // not past a ceiling, though the ceiling itself counts
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/r/.git");
        os_write(os, *a, "/r/.git/HEAD", "ref: refs/heads/main\n");
        os_mkdirs(os, *a, "/r/.git/worktrees/w");
        os_write(os, *a, "/r/.git/worktrees/w/HEAD", "ref: refs/heads/feature/x\n");
        os_write(os, *a, "/w/.git", "gitdir: /r/.git/worktrees/w\n");
        os_write(os, *a, "/w2/.git", "gitdir: ../r/.git/worktrees/w\n");
        os_write(os, *a, "/r/sub/.git", "gitdir: /nowhere\n");
        os_write(os, *a, "/r/junk/.git", "not a gitdir line\n");
        os_mkdirs(os, *a, "/r/empty/.git");

        Str main = "{\"repo\":1,\"root\":\"/r\",\"branch\":\"main\"}";
        expect(t, "githead itself", githeadstr(os, a, "/r", {}, 0), main);
        expect(t, "githead below", githeadstr(os, a, "/r/src/deep", {}, 0), main);
        expect(t, "githead dots", githeadstr(os, a, "/r/src/../src/./x", {}, 0), main);
        expect(t, "githead worktree", githeadstr(os, a, "/w/pkg", {}, 0),
               "{\"repo\":1,\"root\":\"/w\",\"branch\":\"feature/x\"}");
        expect(t, "githead worktree relative", githeadstr(os, a, "/w2", {}, 0),
               "{\"repo\":1,\"root\":\"/w2\",\"branch\":\"feature/x\"}");
        expect(t, "githead past nowhere", githeadstr(os, a, "/r/sub/x", {}, 0), main);
        expect(t, "githead past junk", githeadstr(os, a, "/r/junk", {}, 0), main);
        expect(t, "githead past no HEAD", githeadstr(os, a, "/r/empty/x", {}, 0), main);
        expect(t, "githead ceiling", githeadstr(os, a, "/r/src/deep", "/r/src", 0),
               "{\"repo\":0}");
        expect(t, "githead ceiling itself", githeadstr(os, a, "/r/src", "/r", 0), main);
        expect(t, "githead ceiling below", githeadstr(os, a, "/r", "/r/src", 0), main);
        expect(t, "githead none", githeadstr(os, a, "/plain/dir", {}, 0), "{\"repo\":0}");
        expect(t, "githead no cwd", githeadstr(os, a, {}, {}, 0), "{\"repo\":0}");

        // Walked up GIT_WALKMAX levels at most
        Buf deep(a, 4096);
        print(&deep, "/r");
        for (i32 i = 0; i < GIT_WALKMAX-1; i++) {
            print(&deep, "/d");
        }
        Str near = finish(&deep);
        expect(t, "githead deep", githeadstr(os, a, near, {}, 0), main);
        expect(t, "githead too deep", githeadstr(os, a, concat(a, near, "/d"), {}, 0),
               "{\"repo\":0}");
    }

    // At the root, on either system, named without a doubled slash
    {
        Os *os = newos(a, 1<<16);
        os_mkdirs(os, *a, "/.git");
        os_write(os, *a, "/.git/HEAD", "ref: refs/heads/dotfiles\n");
        os_mkdirs(os, *a, "C:/.git");
        os_write(os, *a, "C:/.git/HEAD", "ref: refs/heads/c\n");
        expect(t, "githead root", githeadstr(os, a, "/home/u", {}, 0),
               "{\"repo\":1,\"root\":\"/\",\"branch\":\"dotfiles\"}");
        expect(t, "githead drive root", githeadstr(os, a, "C:\\x\\y", {}, 1),
               "{\"repo\":1,\"root\":\"C:\\\\\",\"branch\":\"c\"}");
    }

    // Windows: backslashes, CRLF, and a ceiling in another case
    {
        Os *os = newos(a, 1<<16);
        os_mkdirs(os, *a, "C:/Users/u/proj/.git");
        os_write(os, *a, "C:/Users/u/proj/.git/HEAD", "ref: refs/heads/main\r\n");
        expect(t, "githead windows", githeadstr(os, a, "C:\\Users\\u\\proj\\src", {}, 1),
               "{\"repo\":1,\"root\":\"C:\\\\Users\\\\u\\\\proj\",\"branch\":\"main\"}");
        expect(t, "githead windows ceiling",
               githeadstr(os, a, "C:\\Users\\u\\proj\\src", "c:/users/U/proj/SRC", 1),
               "{\"repo\":0}");
    }

    // What HEAD may hold
    {
        Os *os = newos(a, 1<<16);
        os_mkdirs(os, *a, "/h/.git");
        struct { Str head; Str want; } heads[] = {
            {"ref: refs/heads/main\n", "\"branch\":\"main\""},
            {"ref:refs/heads/tight", "\"branch\":\"tight\""},
            {"0123456789abcdef0123456789abcdef01234567\n",
             "\"detached\":1,\"oid\":\"0123456789abcdef0123456789abcdef01234567\""},
            {"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
             "\"detached\":1,\"oid\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\""},
            {"0123456789ABCDEF0123456789ABCDEF01234567\n", ""},
            {"0123456789abcdef\n", ""},
            {"ref: refs/heads/.invalid\n", "\"reftable\":1"},
            {"ref: refs/remotes/origin/main\n", "\"branch\":\"refs/remotes/origin/main\""},
            {"ref: refs/heads/ma\x1b[31min\n", "\"branch\":\"ma\""},
            {"ref: refs/heads/caf\xc3\xa9\n", "\"branch\":\"caf\xc3\xa9\""},
            {"ref: refs/heads/\xff\n", "\"branch\":\"\xef\xbf\xbd\""},
            {"garbage\n", ""},
            {"", ""},
        };
        for (iz i = 0; i < countof(heads); i++) {
            os_write(os, *a, "/h/.git/HEAD", heads[i].head);
            Str want = heads[i].want.len ? concat(a, ",", heads[i].want) : Str{};
            want = concat(a, concat(a, "{\"repo\":1,\"root\":\"/h\"", want), "}");
            expect(t, concat(a, "githead HEAD ", istr(a, i)),
                   githeadstr(os, a, "/h", {}, 0), want);
        }
    }

    // Operations under way: a rebase names its branch and steps, git am
    // and the rest only themselves, and steps out of order are left out
    {
        Os *os = newos(a, 1<<16);
        Str oid = "0123456789abcdef0123456789abcdef01234567";
        os_mkdirs(os, *a, "/o/.git");
        os_write(os, *a, "/o/.git/HEAD", oid);
        os_mkdirs(os, *a, "/o/.git/rebase-merge");
        os_write(os, *a, "/o/.git/rebase-merge/msgnum", "3\n");
        os_write(os, *a, "/o/.git/rebase-merge/end", "7\n");
        os_write(os, *a, "/o/.git/rebase-merge/head-name", "refs/heads/topic\n");
        os_mkdirs(os, *a, "/o/.git/rebase-apply");
        os_write(os, *a, "/o/.git/rebase-apply/next", "2\n");
        os_write(os, *a, "/o/.git/rebase-apply/last", "5\n");
        os_write(os, *a, "/o/.git/rebase-apply/applying", "");
        os_write(os, *a, "/o/.git/MERGE_HEAD", oid);
        expect(t, "githead rebase", githeadstr(os, a, "/o", {}, 0),
               "{\"repo\":1,\"root\":\"/o\",\"branch\":\"topic\",\"op\":\"rebase\","
               "\"step\":3,\"steps\":7}");

        os_remove(os, *a, "/o/.git/rebase-merge");
        expect(t, "githead am", githeadstr(os, a, "/o", {}, 0),
               concat(a, concat(a, "{\"repo\":1,\"root\":\"/o\",\"detached\":1,\"oid\":\"",
                      oid), "\",\"op\":\"am\",\"step\":2,\"steps\":5}"));

        os_remove(os, *a, "/o/.git/rebase-apply/applying");
        os_write(os, *a, "/o/.git/rebase-apply/head-name", "refs/heads/old\n");
        os_write(os, *a, "/o/.git/rebase-apply/next", "6\n");
        expect(t, "githead rebase apply", githeadstr(os, a, "/o", {}, 0),
               "{\"repo\":1,\"root\":\"/o\",\"branch\":\"old\",\"op\":\"rebase\"}");

        os_remove(os, *a, "/o/.git/rebase-apply");
        os_write(os, *a, "/o/.git/HEAD", "ref: refs/heads/main\n");
        struct { Str file; Str op; } marks[] = {
            {"MERGE_HEAD", "merge"}, {"CHERRY_PICK_HEAD", "cherry-pick"},
            {"REVERT_HEAD", "revert"}, {"BISECT_LOG", "bisect"},
        };
        for (iz i = 0; i < countof(marks); i++) {
            Str path = concat(a, "/o/.git/", marks[i].file);
            os_write(os, *a, path, oid);
            expect(t, concat(a, "githead ", marks[i].op), githeadstr(os, a, "/o", {}, 0),
                   concat(a, concat(a, "{\"repo\":1,\"root\":\"/o\",\"branch\":\"main\",\"op\":\"",
                          marks[i].op), "\"}"));
            os_remove(os, *a, path);
        }
        expect(t, "githead no op", githeadstr(os, a, "/o", {}, 0),
               "{\"repo\":1,\"root\":\"/o\",\"branch\":\"main\"}");
    }
}

static void testgitparse(Test *t)
{
    Arena *a = &t->perm;

    // Headers and every kind of entry: paths with spaces, a tab, a
    // newline and a byte that is no UTF-8, a rename with where it came
    // from, the ignored left out, and a last record cut off dropped
    Str out =
        "# branch.oid 0123456789abcdef0123456789abcdef01234567\0"
        "# branch.head main\0"
        "# branch.upstream origin/main\0"
        "# branch.ab +3 -1\0"
        "# stash 2\0"
        "1 .M N... 100644 100644 100644 0123 0123 src/a.c\0"
        "1 A. N... 000000 100644 100644 0000 0123 dir/with space.txt\0"
        "2 R. N... 100644 100644 100644 0123 0123 R100 new name.c\0" "old name.c\0"
        "u UU N... 100644 100644 100644 100644 0123 0123 0123 conflict.c\0"
        "? untracked\twith tab\0"
        "? line\nbreak\0"
        "? bad\xff" "name\0"
        "! ignored.o\0"
        "1 D. N... 100644 000000 000000 0123 0000 gone.c\0"
        "1 .M N... 100644 100644 100644 0123 0123 cut off";
    GitStatus st = gitparsestatus(a, out);
    expect(t, "gitstatus head", concat(a, concat(a, st.branch, "|"),
           concat(a, st.upstream, concat(a, "|", concat(a, istr(a, st.ahead*10 + st.behind),
           istr(a, st.ab*100 + st.unborn*10 + st.detached))))),
           "main|origin/main|31100");
    expect(t, "gitstatus files", gitfilesstr(a, &st),
           "1 .M src/a.c|1 A. dir/with space.txt|2 R. new name.c <- old name.c|"
           "u UU conflict.c|? ?? untracked\twith tab|? ?? line\nbreak|"
           "? ?? bad\xff" "name|1 D. gone.c|");

    // Lines from numstat, by the new path of a rename, a binary file's
    // none, and paths not in the status, or cut off, ignored
    Arena scratch = carve(a, 4<<20);
    Str ns =
        "3\t1\tsrc/a.c\0"
        "10\t0\tdir/with space.txt\0"
        "5\t2\t\0old name.c\0new name.c\0"
        "-\t-\tconflict.c\0"
        "0\t4\tgone.c\0"
        "2\t2\tnot listed\0"
        "7\t7\tcut";
    gitparsenumstat(scratch, ns, &st);
    expect(t, "gitnumstat", gitfilesstr(a, &st),
           "1 .M src/a.c +3 -1|1 A. dir/with space.txt +10 -0|"
           "2 R. new name.c <- old name.c +5 -2|u UU conflict.c bin|"
           "? ?? untracked\twith tab|? ?? line\nbreak|? ?? bad\xff" "name|"
           "1 D. gone.c +0 -4|");

    // As JSON: conflicts first, then tracked files, then new ones
    expect(t, "gitstatusjson", gitstatusjson(a, scratch, &st, 12, GIT_REPLYMAX),
           "{\"ok\":1,\"branch\":\"main\",\"upstream\":\"origin/main\",\"ahead\":3,"
           "\"behind\":1,\"count\":8,\"add\":18,\"del\":7,\"ms\":12,\"files\":["
           "{\"xy\":\"UU\",\"path\":\"conflict.c\",\"bin\":1},"
           "{\"xy\":\".M\",\"path\":\"src/a.c\",\"add\":3,\"del\":1},"
           "{\"xy\":\"A.\",\"path\":\"dir/with space.txt\",\"add\":10,\"del\":0},"
           "{\"xy\":\"R.\",\"path\":\"new name.c\",\"orig\":\"old name.c\",\"add\":5,\"del\":2},"
           "{\"xy\":\"D.\",\"path\":\"gone.c\",\"add\":0,\"del\":4},"
           "{\"xy\":\"??\",\"path\":\"untracked\\twith tab\"},"
           "{\"xy\":\"??\",\"path\":\"line\\nbreak\"},"
           "{\"xy\":\"??\",\"path\":\"bad\xef\xbf\xbdname\"}],\"more\":0}");

    // Before the first commit, detached, and a rename cut before its
    // origin
    {
        GitStatus u = gitparsestatus(a, "# branch.oid (initial)\0# branch.head (detached)\0"
                                        "2 R. N... 100644 100644 100644 0123 0123 R100 new\0");
        expect(t, "gitstatus unborn", concat(a, istr(a, u.unborn*10 + u.detached),
               concat(a, "|", concat(a, u.branch, concat(a, "|", istr(a, u.files.len))))),
               "11||0");
        expect(t, "gitstatus unborn json", gitstatusjson(a, scratch, &u, 0, GIT_REPLYMAX),
               "{\"ok\":1,\"detached\":1,\"unborn\":1,\"count\":0,\"add\":0,\"del\":0,"
               "\"ms\":0,\"files\":[],\"more\":0}");
        GitStatus bad = gitparsestatus(a, "# branch.ab 3 1\0" "1 .M\0" "1 .M N... x\0" "?\0" "x y\0");
        expect(t, "gitstatus malformed", concat(a, istr(a, bad.ab), istr(a, bad.files.len)), "00");
    }

    // Lines of new files: text, binary, empty, without a final newline
    {
        b32 bin = 0;
        expect(t, "gitlines", istr(a, gitlines("a\nb\n", &bin)*10 + bin), "20");
        expect(t, "gitlines last", istr(a, gitlines("a\nb", &bin)*10 + bin), "20");
        expect(t, "gitlines empty", istr(a, gitlines("", &bin)*10 + bin), "0");
        expect(t, "gitlines blank", istr(a, gitlines("\n\n\n", &bin)*10 + bin), "30");
        expect(t, "gitlines bin", istr(a, gitlines("x\ny\0z\n", &bin)*10 + bin), "1");
        Buf late(a, GIT_BINPEEK + 8);
        for (iz i = 0; i < GIT_BINPEEK; i++) {
            putbyte(&late, i%64==63 ? '\n' : 'x');
        }
        putbyte(&late, 0);
        expect(t, "gitlines late NUL", istr(a, gitlines(finish(&late), &bin)*10 + bin),
               istr(a, (GIT_BINPEEK/64 + 1)*10));
    }

    // Counting new files from the working tree: a regular file, a binary
    // one, and partial for a directory, one too big, one gone. Before the
    // first commit, added files count too, and one added then removed
    // has nothing to count.
    {
        Os *os = newos(a, 4<<20);
        os_write(os, *a, "/g/a.txt", "one\ntwo\nthree");
        os_write(os, *a, "/g/b.bin", Str((u8 *)"x\0y", 3));
        os_mkdirs(os, *a, "/g/dir");
        Buf big(a, GIT_COUNTMAX + 1);
        for (iz i = 0; i <= GIT_COUNTMAX; i++) {
            putbyte(&big, '\n');
        }
        os_write(os, *a, "/g/big.txt", finish(&big));
        GitStatus c = gitparsestatus(a, "? a.txt\0? b.bin\0? dir/\0? big.txt\0? missing\0");
        gitcount(os, scratch, "/g", &c);
        expect(t, "gitcount", concat(a, gitfilesstr(a, &c), istr(a, c.partial)),
               "? ?? a.txt +3 -0|? ?? b.bin bin|? ?? dir/|? ?? big.txt|? ?? missing|1");

        os_write(os, *a, "/g/first.c", "x\n");
        GitStatus n = gitparsestatus(a, "# branch.oid (initial)\0"
            "1 A. N... 000000 100644 100644 0000 0123 first.c\0"
            "1 AD N... 000000 100644 000000 0000 0123 dropped.c\0");
        gitcount(os, scratch, "/g", &n);
        expect(t, "gitcount unborn", concat(a, gitfilesstr(a, &n), istr(a, n.partial)),
               "1 A. first.c +1 -0|1 AD dropped.c +0 -0|0");
    }

    // A budget: as many files as fit, in order, the rest counted in more
    {
        GitStatus big = {};
        for (i32 i = 0; i < 10000; i++) {
            GitFile f = {};
            f.kind = i==9999 ? 'u' : i%2 ? '?' : '1';
            f.x    = f.kind=='u' ? 'U' : f.kind=='?' ? '?' : '.';
            f.y    = f.kind=='u' ? 'U' : f.kind=='?' ? '?' : 'M';
            Buf p(a, 32);
            print(&p, "some/directory/file");
            print(&p, (i64)i);
            print(&p, ".txt");
            f.path    = finish(&p);
            f.add     = i;
            f.counted = f.kind == '1';
            big.files = push(a, big.files, f);
        }
        iz      budget = GIT_REPLYMAX - 300;
        Str     json   = gitstatusjson(a, scratch, &big, 5, budget);
        JsonDoc d      = jsonparse(a, json);
        Json   *files  = jsonget(d.root, "files");
        i64     more   = jsonint(jsonget(d.root, "more"), -1);
        expect(t, "gitbudget fits", istr(a, d.ok && json.len<=budget && json.len>budget-200), "1");
        expect(t, "gitbudget count", istr(a, jsonint(jsonget(d.root, "count"), 0)), "10000");
        expect(t, "gitbudget more", istr(a, files->len + more), "10000");
        expect(t, "gitbudget conflict first",
               jsonstr(jsonget(jsonat(files, 0), "path")), "some/directory/file9999.txt");
        expect(t, "gitbudget tracked next",
               jsonstr(jsonget(jsonat(files, 1), "path")), "some/directory/file0.txt");
        i64 want = 0;
        for (i64 i = 0; i < 9999; i += 2) {
            want += i;
        }
        expect(t, "gitbudget totals", istr(a, jsonint(jsonget(d.root, "add"), 0)), istr(a, want));

        GitStatus none = {};
        Str tiny = gitstatusjson(a, scratch, &big, 0, 100);
        expect(t, "gitbudget none fit", istr(a, jsonparse(a, tiny).ok &&
               jsonint(jsonget(jsonparse(a, tiny).root, "more"), 0)==10000), "1");
        expect(t, "gitbudget empty", gitstatusjson(a, scratch, &none, 0, 100),
               "{\"ok\":1,\"count\":0,\"add\":0,\"del\":0,\"ms\":0,\"files\":[],\"more\":0}");
    }

    // Past GIT_FILESMAX the rest is cut, so memory stays bounded
    {
        Buf many(a, (GIT_FILESMAX + 2) * 8);
        for (i64 i = 0; i < GIT_FILESMAX+2; i++) {
            print(&many, "? ");
            print(&many, i);
            putbyte(&many, 0);
        }
        GitStatus m = gitparsestatus(a, finish(&many));
        expect(t, "gitstatus most", istr(a, m.files.len*10 + m.cut), istr(a, GIT_FILESMAX*10 + 1));
    }
}

static void testgitrun(Test *t)
{
    Arena *a       = &t->perm;
    Arena  scratch = carve(a, 1<<16);

    // Finding git: on PATH, by name or path, ~ expanded, .exe on Windows,
    // and on macOS /usr/bin/git only with the developer tools
    {
        Os *os = newos(a, 1<<16);
        os_write(os, *a, "/usr/bin/git", "");
        os_write(os, *a, "/opt/homebrew/bin/git", "");
        os_write(os, *a, "/home/u/bin/git", "");
        os_write(os, *a, "/opt/x/mygit", "");
        os_write(os, *a, "C:/Git/cmd/git.exe", "");
        struct {
            Str setting; Str path; b32 win; b32 mac; Str want;
        } cases[] = {
            {"",                      "/usr/bin:/bin",          0, 0, "/usr/bin/git|0"},
            {"",                      "/usr/bin:/bin",          0, 1, "/usr/bin/git|1"},
            {"",                      "/opt/homebrew/bin:/usr/bin", 0, 1, "/opt/homebrew/bin/git|0"},
            {"  ",                    "/bin",                   0, 0, "|0"},
            {"/opt/homebrew/bin/git", "",                       0, 1, "/opt/homebrew/bin/git|0"},
            {"/opt/nothere/git",      "/usr/bin",               0, 0, "|0"},
            {"~/bin/git",             "",                       0, 0, "/home/u/bin/git|0"},
            {"mygit",                 "/usr/bin:/opt/x",        0, 0, "/opt/x/mygit|0"},
            {"git",                   "C:/Windows;C:/Git/cmd",  1, 0, "C:/Git/cmd/git.exe|0"},
            {"git.exe",               "C:/Git/cmd",             1, 0, "C:/Git/cmd/git.exe|0"},
            {"",                      "C:/Git/cmd",             1, 0, "C:/Git/cmd/git.exe|0"},
            {"C:/Git/cmd/git.exe",    "",                       1, 0, "C:/Git/cmd/git.exe|0"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            GitProg p = gitprog(os, a, scratch, cases[i].setting, cases[i].path, "/home/u",
                                cases[i].win, cases[i].mac);
            expect(t, concat(a, "gitprog ", istr(a, i)),
                   concat(a, p.path, concat(a, "|", istr(a, p.shim))), cases[i].want);
        }
        os_write(os, *a, "/Library/Developer/CommandLineTools/usr/bin/git", "");
        GitProg clt = gitprog(os, a, scratch, "", "/usr/bin", "", 0, 1);
        expect(t, "gitprog tools", istr(a, clt.path.len && !clt.shim), "1");
    }

    // What the pane says when git is missing or fails
    {
        expect(t, "gitmissing", gitmissing(a, "", 0),
               "git was not found on PATH: set [tools] git, or add its folder to "
               "[tools] path_prepend");
        expect(t, "gitmissing set", gitmissing(a, " /opt/git ", 0),
               "[tools] git, /opt/git, was not found");
        expect(t, "gitmissing shim", gitmissing(a, "", 1),
               "git needs the Command Line Tools (xcode-select --install), or set [tools] git");

        // How each run went: spawned, code, signal, timed out or cut
        struct { b32 spawned; i32 code; i32 sig; i32 how; Str err; Str want; } runs[] = {
            {0, -1,  0, 0, "No such file", "cannot run /x/git: No such file"},
            {0, -1,  0, 0, {},             "cannot run /x/git"},
            {1, -1,  0, 1, {},             "git status took over 10 s: Refresh to try again"},
            {1,  0,  0, 0, {},             ""},
            {1, -1,  9, 2, {},             ""},
            {1, 127, 0, 0, {},             "cannot run /x/git"},
            {1, 126, 0, 0, {},             "cannot run /x/git"},
            {1, 129, 0, 0, "usage: git",   "git 2.15 or newer is needed"},
            {1, 128, 0, 0, "warning: unable to read\nfatal: detected dubious "
                           "ownership in repository at '/r'\nTo add an exception\n",
             "detected dubious ownership in repository at '/r'"},
            {1, 1,   0, 0, "\nerror: bad index\r\nmore\n", "bad index"},
            {1, 2,   0, 0, "something odd\n", "something odd"},
            {1, 1,   0, 0, {},             "git exited with code 1"},
            {1, -1, 11, 0, {},             "git was killed by signal 11"},
        };
        for (iz i = 0; i < countof(runs); i++) {
            GitRun r = {};
            r.spawned = runs[i].spawned;
            r.code    = runs[i].code;
            r.sig     = runs[i].sig;
            r.timeout = runs[i].how == 1;
            r.cut     = runs[i].how == 2;
            r.err     = runs[i].err;
            expect(t, concat(a, "giterror ", istr(a, i)),
                   giterror(a, &r, "/x/git", "status", GIT_STATUSWAIT), runs[i].want);
        }
    }

    // The arguments, and the requests the worker takes
    {
        Slice<Str> st = gitargv(a, "/usr/bin/git", 0);
        Slice<Str> df = gitargv(a, "/usr/bin/git", 1);
        Buf b(a, 512);
        for (iz i = 0; i < st.len; i++) {
            print(&b, st[i]);
            putbyte(&b, i+1<st.len ? ' ' : '|');
        }
        for (iz i = 0; i < df.len; i++) {
            print(&b, df[i]);
            putbyte(&b, i+1<df.len ? ' ' : '|');
        }
        expect(t, "gitargv", finish(&b),
               "/usr/bin/git --no-optional-locks -c core.fsmonitor=false status "
               "--porcelain=v2 --branch -z|"
               "/usr/bin/git --no-optional-locks -c core.fsmonitor=false diff-index "
               "--numstat -z -M HEAD --|");

        GitAsk q = gitparseask(a, "{\"id\":7,\"sid\":\"S\",\"cwd\":\"/w/x\",\"ceiling\":\"\","
                                  "\"root\":\"/w\",\"git\":\"\",\"home\":\"/h\","
                                  "\"errpath\":\"/s/tmp/git-1.err\",\"env\":[\"A=1\",\"PATH=/bin\"]}");
        expect(t, "gitparseask", concat(a, istr(a, q.id), concat(a, q.sid,
               concat(a, q.root, concat(a, envpath(q.env, 0), q.errpath)))),
               "7S/w/bin/s/tmp/git-1.err");
    }
}

// The git command: a session's head, whether open, parked, read-only or
// only on disk, with a folder of its own looking no further, its
// status refused here, where no thread starts, and refusals
static void testgitcmd(Test *t)
{
    Arena   a = t->perm;
    CfgEnv  v = newcfgenv(&a);
    Engine *e = alloc<Engine>(&a);
    e->os       = v.os;
    e->out      = newmailbox(&a, 1<<16, 0, 0);
    e->perm     = carve(&a, 4<<20);
    e->statedir = "/s";
    MadeSession made[] = {
        {"20260101-000000-aaaa", {}},
        {"20260101-000001-bbbb", {}},
    };
    makesessions(v.os, &a, {made, countof(made)});  // in /w
    os_mkdirs(v.os, a, "/w/.git");
    os_write(v.os, a, "/w/.git/HEAD", "ref: refs/heads/main\n");
    os_mkdirs(v.os, a, "/s/.git");
    os_write(v.os, a, "/s/.git/HEAD", "ref: refs/heads/state\n");
    os_mkdirs(v.os, a, "/s/sessions/20260101-000002-cccc/work");
    os_write(v.os, a, "/s/sessions/20260101-000002-cccc/events.jsonl",
             "{\"sid\":\"20260101-000002-cccc\",\"seq\":1,\"ts\":0,\"ev\":\"session\","
             "\"cwd\":\"/s/sessions/20260101-000002-cccc/work\",\"model\":\"m\","
             "\"nofolder\":1}\n");

    Parked k = {};
    k.sid  = "20260101-000001-bbbb";
    k.lock = -1;
    e->parked = push(&a, e->parked, k);
    Session *ro = alloc<Session>(&a);
    ro->sid      = "20260101-000009-ropn";
    ro->cwd      = "/plain";
    ro->readonly = 1;
    e->sessions  = ro;

    Str cmds[] = {
        "{\"cmd\":\"git\",\"id\":1,\"sid\":\"20260101-000000-aaaa\"}",
        "{\"cmd\":\"git\",\"id\":2,\"sid\":\"20260101-000001-bbbb\"}",
        "{\"cmd\":\"git\",\"id\":3,\"sid\":\"20260101-000009-ropn\",\"status\":1}",
        "{\"cmd\":\"git\",\"id\":4,\"sid\":\"20260101-000002-cccc\"}",
        "{\"cmd\":\"git\",\"id\":5,\"sid\":\"20260101-000000-aaaa\",\"status\":true}",
        "{\"cmd\":\"git\",\"id\":6,\"sid\":\"20260101-000009-gone\"}",
        "{\"cmd\":\"git\",\"id\":7,\"sid\":\"../x\"}",
        "{\"cmd\":\"git\",\"id\":8}",
    };
    for (iz i = 0; i < countof(cmds); i++) {
        command(e, v.scratch, cmds[i]);
    }
    e->quitting = 1;
    command(e, v.scratch, "{\"cmd\":\"git\",\"id\":9,\"sid\":\"20260101-000000-aaaa\"}");

    Buf  got(&a, 2048);
    Mail m = mailtake(e->out);
    for (Record r; (r = mailnext(&m)).ok;) {
        EvRec rec = evparse(&a, r.data);
        print(&got, jsonint(jsonget(rec.root, "id"), 0));
        putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
        print(&got, jsonstr(jsonget(rec.root, "err")));
        Json *head   = jsonget(rec.root, "head");
        Json *status = jsonget(rec.root, "status");
        if (head->type) {
            putbyte(&got, ' ');
            print(&got, head->raw);
        }
        if (status->type) {
            putbyte(&got, ' ');
            print(&got, status->raw);
        }
        putbyte(&got, '\n');
    }
    expect(t, "git commands", finish(&got),
           "1+ {\"repo\":1,\"root\":\"/w\",\"branch\":\"main\"}\n"
           "2+ {\"repo\":1,\"root\":\"/w\",\"branch\":\"main\"}\n"
           "3+ {\"repo\":0}\n"
           "4+ {\"repo\":0}\n"
           "5+ {\"repo\":1,\"root\":\"/w\",\"branch\":\"main\"} "
           "{\"ok\":0,\"err\":\"cannot start a thread for git\"}\n"
           "6-no such session\n"
           "7-invalid session id\n"
           "8-invalid session id\n"
           "9-the engine is stopping\n");
    expect(t, "git leaves parked", istr(&a, e->parked.len*10 + (e->sessions==ro && !ro->next)), "11");
    expect(t, "git pending", istr(&a, e->git ? e->git->pending : -1), "0");
}

// Refs from loose and packed files, the common git directory, and git
// worktree list's porcelain.
static void testgitrefs(Test *t)
{
    Arena  mem = t->perm;  // let go as it returns
    Arena *a   = &mem;
    Str    x = "1111111111111111111111111111111111111111";
    Str    y = "2222222222222222222222222222222222222222";
    Str    z = "3333333333333333333333333333333333333333";
    {
        Os *os = newos(a, 1<<20);
        os_mkdirs(os, *a, "/r/.git/refs/heads/prospero");
        os_write(os, *a, "/r/.git/refs/heads/prospero/a", concat(a, x, "\n"));
        os_write(os, *a, "/r/.git/packed-refs",
                 concat(a, concat(a, concat(a, concat(a,
                 "# pack-refs with: peeled fully-peeled sorted\r\n", y),
                 " refs/heads/prospero/a\n"), z), " refs/heads/b\n^4444\n"));
        os_mkdirs(os, *a, "/r/.git/worktrees/w");
        os_write(os, *a, "/r/.git/worktrees/w/commondir", "../..\n");
        expect(t, "refoid loose first", gitrefoid(os, a, "/r/.git", "refs/heads/prospero/a"), x);
        expect(t, "refoid packed", gitrefoid(os, a, "/r/.git", "refs/heads/b"), z);
        expect(t, "refoid none", gitrefoid(os, a, "/r/.git", "refs/heads/c"), "");
        expect(t, "refoid prefix", gitrefoid(os, a, "/r/.git", "refs/heads/prospero"), "");
        expect(t, "common of worktree", gitcommon(os, a, "/r/.git/worktrees/w", 0), "/r/.git");
        expect(t, "common of main", gitcommon(os, a, "/r/.git", 0), "/r/.git");
    }

    {
        Str out =
            "worktree /r\nHEAD 1111\nbranch refs/heads/master\n\n"
            "worktree /r/.prospero/worktrees/brisk-otter\nHEAD 2222\n"
            "branch refs/heads/prospero/brisk-otter\nlocked\n\n"
            "worktree /gone\r\nHEAD 3333\r\ndetached\r\nprunable gitdir file points to non-existent location\r\n\r\n"
            "worktree /bare\nbare\n"
            "worktree /last\nHEAD 4444\nbranch refs/heads/x";
        Slice<GitTree> ts = gitparsetrees(a, out);
        Buf b(a, 1024);
        for (iz i = 0; i < ts.len; i++) {
            print(&b, ts[i].path);
            putbyte(&b, ' ');
            print(&b, ts[i].oid);
            putbyte(&b, ' ');
            print(&b, ts[i].branch);
            print(&b, ts[i].bare ? Str(" bare") : Str{});
            print(&b, ts[i].locked ? Str(" locked") : Str{});
            print(&b, ts[i].prunable ? Str(" prunable") : Str{});
            putbyte(&b, '|');
        }
        expect(t, "parsetrees", finish(&b),
               "/r 1111 refs/heads/master|"
               "/r/.prospero/worktrees/brisk-otter 2222 refs/heads/prospero/brisk-otter locked|"
               "/gone 3333  prunable|/bare   bare|/last 4444 refs/heads/x|");
        expect(t, "parsetrees empty", istr(a, gitparsetrees(a, "").len), "0");
    }
}

static void test_git(Test *t)
{
    testgitrefs(t);
    testgithead(t);
    testgitparse(t);
    testgitrun(t);
    testgitcmd(t);
}
