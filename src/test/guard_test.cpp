// Guard tests: normalization (POSIX and Windows forms), containment,
// real paths through symlinks, hints, and denials

static void test_guard(Test *t)
{
    Arena *a = &t->perm;

    struct { Str cwd; Str path; Str want; } posix[] = {
        {"/a/b", "c", "/a/b/c"},
        {"/a/b", "../x", "/a/x"},
        {"/a/b", "/etc/passwd", "/etc/passwd"},
        {"/a", "../../..", "/"},
        {"/a/b", "./c/./d/../e", "/a/b/c/e"},
        {"/a/b", "c//d/", "/a/b/c/d"},
        {"/a/b", ".", "/a/b"},
        {"/", "x", "/x"},
        {"/a/b", "c\\d", "/a/b/c\\d"},
        {"/a/b", "x.", "/a/b/x."},          // Win32 rules do not apply
        {"/a/b", ".. /x", "/a/b/.. /x"},
        {"/a/b", "C:x", "/a/b/C:x"},
        {"/a/b", "\\\\?\\C:\\x", "/a/b/\\\\?\\C:\\x"},
    };
    for (iz i = 0; i < countof(posix); i++) {
        Str got = normpath(a, posix[i].cwd, posix[i].path, 0);
        expect(t, concat(a, "posix ", posix[i].path), got, posix[i].want);
    }

    struct { Str cwd; Str path; Str want; } win[] = {
        {"C:\\Users\\me", "x\\y", "C:/Users/me/x/y"},
        {"c:/w", "..\\..\\z", "C:/z"},
        {"C:/w", "D:/q", "D:/q"},
        {"C:/w", "d:\\Q", "D:/Q"},
        {"C:/w/v", "\\x", "C:/x"},
        {"//srv/share/dir", "../x", "//srv/share/x"},
        {"//srv/share/dir", "../../..", "//srv/share/"},
        {"//srv/share/dir", "\\x", "//srv/share/x"},
        {"C:\\w", ".", "C:/w"},
        {"C:/w", "//x", "//x/"},
        {"C:/w", "/\\a\\b", "//a/b/"},
        // Extended-length forms
        {"C:/w", "\\\\?\\C:\\x\\y", "C:/x/y"},
        {"C:/w", "//?/c:/a/../b", "C:/b"},
        {"C:/w", "\\\\?\\UNC\\srv\\share\\d", "//srv/share/d"},
        {"C:/w", "\\\\?\\unc\\srv\\share", "//srv/share/"},
        {"C:/w", "\\\\?\\UNC\\?\\C:\\x", "C:/x"},
        {"C:/w", "\\\\?\\GLOBALROOT\\Device\\x", "//?/GLOBALROOT/Device/x"},
        {"\\\\?\\C:\\w", "x", "C:/w/x"},
        // Devices keep a root of their own
        {"C:/w", "\\\\.\\pipe\\p", "//./pipe/p"},
        // Drive-relative
        {"C:/w", "C:x\\y", "C:/w/x/y"},
        {"C:/w", "c:", "C:/w"},
        {"C:/w", "D:x", "D:/x"},
        {"//srv/share/d", "C:x", "C:/x"},
        // Trailing dots and spaces, as Win32 drops them
        {"C:/w", "a./b ", "C:/w/a/b"},
        {"C:/w", ".. \\x", "C:/w/x"},  // a name, not the parent
        {"C:/w", "x\\...\\y", "C:/w/x/y"},
        {"C:/w", "x\\. .\\y", "C:/w/x/y"},
        {"C:/w", "..\\..\\..", "C:/"},
    };
    for (iz i = 0; i < countof(win); i++) {
        Str got = normpath(a, win[i].cwd, win[i].path, 1);
        expect(t, concat(a, "windows ", win[i].path), got, win[i].want);
        Str again = normpath(a, win[i].cwd, got, 1);
        expect(t, concat(a, "windows idempotent ", win[i].path), again, got);
    }

    // Random paths from the characters that matter: always rooted, no
    // empty, "." or ".." names, and normalized already once normalized
    {
        Str alpha = "/\\.. ?:CcUuNnx~";
        u64 rng   = 1;
        i32 bad   = 0;
        for (i32 n = 0; n < 20000; n++) {
            Arena tmp = *a;
            u8    buf[16];
            rng = rng*0x9e3779b97f4a7c15u + 1;
            iz len = (iz)(rng>>60);
            for (iz i = 0; i < len; i++) {
                rng = rng*0x9e3779b97f4a7c15u + 1;
                buf[i] = alpha[(iz)((rng>>33) % (u64)alpha.len)];
            }
            for (b32 w = 0; w < 2; w++) {
                Str cwd  = w ? Str("C:/w") : Str("/w");
                Str p    = normpath(&tmp, cwd, {buf, len}, w);
                iz  root = pathroot(p, w);
                b32 ok   = root > 0 && normpath(&tmp, cwd, p, w) == p;
                for (Str rest = cuthead(p, root); ok && rest.len;) {
                    Cut c = cut(rest, '/');
                    ok   = c.head.len && c.head!="." && c.head!="..";
                    rest = c.tail;
                }
                if (!ok && bad++ < 3) {
                    expect(t, concat(&tmp, "random path ", {buf, len}), p, "(normalized)");
                }
            }
        }
        expect(t, "random paths normalize", istr(a, bad), "0");
    }

    expect(t, "within itself", istr(a, within("/a/b", "/a/b", 0)), "1");
    expect(t, "within below", istr(a, within("/a/b", "/a/b/c", 0)), "1");
    expect(t, "within sibling", istr(a, within("/a/b", "/a/bc", 0)), "0");
    expect(t, "within case posix", istr(a, within("/a/B", "/a/b/c", 0)), "0");
    expect(t, "within case windows", istr(a, within("C:/Users/Me", "C:/users/me/x", 1)), "1");
    expect(t, "within root", istr(a, within("/", "/anything", 0)), "1");
    expect(t, "within drive root", istr(a, within("C:/", "c:/x", 1)), "1");
    expect(t, "within other drive", istr(a, within("C:/", "D:/x", 1)), "0");
    expect(t, "within unc", istr(a, within("//srv/share/", "//SRV/Share/x", 1)), "1");

    struct { Str path; Str home; Str want; } homes[] = {
        {"~", "/home/u", "/home/u"},
        {"~/x", "/home/u", "/home/u/x"},
        {"~user/x", "/home/u", "~user/x"},
        {"~/x", "", "~/x"},
        {"a/~", "/home/u", "a/~"},
    };
    for (iz i = 0; i < countof(homes); i++) {
        Str got = expandhome(a, homes[i].path, homes[i].home, 0);
        expect(t, concat(a, "home ", homes[i].path), got, homes[i].want);
    }
    struct { Str path; Str home; Str want; } tildes[] = {
        {"/home/u", "/home/u", "~"},
        {"/home/u/x/y", "/home/u", "~/x/y"},
        {"/home/u/x", "/home/u/", "~/x"},
        {"/home/user/x", "/home/u", "/home/user/x"},
        {"/home/U/x", "/home/u", "/home/U/x"},
        {"/home", "/home/u", "/home"},
        {"/x", "/", "/x"},
        {"/x", "", "/x"},
        {"C:/Users/Me/src", "C:\\Users\\me", "~/src"},
        {"C:\\Users\\Me\\src", "C:\\Users\\Me", "~\\src"},
        {"C:\\Users\\Mel", "C:\\Users\\Me", "C:\\Users\\Mel"},
        {"D:/x", "D:\\", "D:/x"},
    };
    for (iz i = 0; i < countof(tildes); i++) {
        Str got = tildeform(a, tildes[i].path, tildes[i].home);
        expect(t, concat(a, "tilde form ", tildes[i].path), got, tildes[i].want);
    }

    // A workspace on a virtual file system with symlinks
    Os *os = newos(a, 1<<20);
    Str dirs[] = {
        "/w", "/w/src", "/w/foo", "/x/extra", "/s/sess", "/s/sess/out",
        "/etc", "/home/u", "/private/tmp/w2",
    };
    for (iz i = 0; i < countof(dirs); i++) {
        os_mkdirs(os, *a, dirs[i]);
    }
    Str files[] = {"/w/src/x.c", "/w/foo/bar", "/etc/passwd", "/s/sess/out/c1.log"};
    for (iz i = 0; i < countof(files); i++) {
        os_write(os, *a, files[i], "data");
    }
    vlink(os, "/w/out", "/etc");          // leads out
    vlink(os, "/w/inner", "/w/src");      // stays in
    vlink(os, "/w/loop", "/w/loop");      // never resolves
    vlink(os, "/tmp", "/private/tmp");    // a root reached through a link

    expect(t, "realpath through a link",
           realpathof(os, a, "/w/out/passwd", 0), "/etc/passwd");
    expect(t, "realpath of a missing name through a link",
           realpathof(os, a, "/w/out/new/x.txt", 0), "/etc/new/x.txt");
    expect(t, "realpath of a missing name",
           realpathof(os, a, "/w/new.txt", 0), "/w/new.txt");
    expect(t, "realpath of nothing", realpathof(os, a, "/nope/x", 0), Str{});
    {
        // A made-up path hundreds of names deep costs few lookups
        Buf deep(a, 1024);
        print(&deep, "/w");
        for (i32 i = 0; i < 300; i++) {
            print(&deep, "/d");
        }
        Str path = finish(&deep);
        os->realpaths = 0;
        expect(t, "realpath of a deep missing path",
               realpathof(os, a, path, 0), path);
        expect(t, "realpath of a deep missing path (lookups)",
               istr(a, os->realpaths <= 20), "1");
    }

    Str   extra[] = {"/x/extra", "/tmp/w2", "/missing/root"};
    Guard g = newguard(os, a, "/w", "/home/u", {extra, countof(extra), countof(extra)},
                       "/s/sess", 0);
    expect(t, "guard roots", istr(a, g.roots.len), "4");

    struct { Str path; b32 read; Str want; } cases[] = {
        // want: normalized path, then I(nside), D(iverged), or O(utside)
        {"src/x.c",                 1, "/w/src/x.c I"},
        {"new.txt",                 0, "/w/new.txt I"},
        {"deep/new/dir/f",          0, "/w/deep/new/dir/f I"},
        {"../x",                    0, "/x O"},
        {"/etc/passwd",             1, "/etc/passwd O"},
        {"~/notes",                 1, "/home/u/notes O"},
        {"~",                       1, "/home/u O"},
        {"~user/x",                 0, "/w/~user/x I"},
        {"out/passwd",              1, "/w/out/passwd D"},
        {"out/new.txt",             0, "/w/out/new.txt D"},
        {"out",                     0, "/w/out D"},
        {"inner/x.c",               1, "/w/inner/x.c I"},
        {"loop/x",                  0, "/w/loop/x I"},  // as a missing name
        {"/x/extra/f",              0, "/x/extra/f I"},
        {"/x/extrafile",            0, "/x/extrafile O"},
        {"/tmp/w2/f",               0, "/tmp/w2/f I"},
        {"/private/tmp/w2/f",       0, "/private/tmp/w2/f I"},
        {"/missing/root/f",         0, "/missing/root/f O"},
        {"/s/sess/out/c1.log",      1, "/s/sess/out/c1.log I"},
        {"/s/sess/out/c1.log",      0, "/s/sess/out/c1.log O"},
        {"/s/sess/events.jsonl",    0, "/s/sess/events.jsonl O"},
        {"src/../../w/src/x.c",     1, "/w/src/x.c I"},
        {"/c/x",                    1, "/c/x O"},  // no drives off Windows
        {"/cygdrive/c/x",           1, "/cygdrive/c/x O"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        GuardPath gp = guardpath(os, a, &g, cases[i].path, cases[i].read);
        Str got = concat(a, gp.path, gp.inside ? Str(" I") : gp.diverged ? Str(" D") : Str(" O"));
        Str name = concat(a, cases[i].read ? Str("guard read ") : Str("guard write "),
                          cases[i].path);
        expect(t, name, got, cases[i].want);
    }
    GuardPath nul = guardpath(os, a, &g, Str{(u8 *)"x\0/../../etc", 12}, 1);
    expect(t, "guard null byte", istr(a, nul.inside), "0");
    GuardPath dv = guardpath(os, a, &g, "out/passwd", 1);
    expect(t, "guard diverged real", dv.real, "/etc/passwd");

    // Hints: the same trailing names inside the workspace
    struct { Str path; b32 read; b32 create; Str want; } hints[] = {
        {"/q/foo/bar",   1, 0, "/w/foo/bar +"},
        {"/a/b/src/x.c", 1, 0, "/w/src/x.c +"},
        {"/new/x.txt",   0, 1, "/w/x.txt -"},
        {"/q/foo/n.txt", 0, 1, "/w/foo/n.txt -"},
        {"/q/zzz",       1, 0, " "},
        {"/",            1, 0, " "},
        {"/q/c1.log",    1, 0, " "},
        {"/q/out/c1.log", 1, 0, "/s/sess/out/c1.log +"},
    };
    for (iz i = 0; i < countof(hints); i++) {
        GuardHint h = guardhint(os, a, &g, hints[i].path, hints[i].read, hints[i].create);
        Str got = concat(a, h.path, h.path.data ? (h.exists ? Str(" +") : Str(" -")) : Str(" "));
        expect(t, concat(a, "hint ", hints[i].path), got, hints[i].want);
    }

    // Hints keep at most HINT_PARTS trailing names, so a long path costs
    // a bounded number of lookups
    os_write(os, *a, "/w/2/3/4/5/6/7/8/f", "data");
    os_write(os, *a, "/w/1/2/3/4/5/6/7/8/g", "data");
    {
        GuardHint h = guardhint(os, a, &g, "/q/2/3/4/5/6/7/8/f", 1, 0);
        expect(t, "hint of eight names", h.path, "/w/2/3/4/5/6/7/8/f");
        h = guardhint(os, a, &g, "/q/1/2/3/4/5/6/7/8/g", 1, 0);
        expect(t, "hint of nine names", h.path, Str{});

        Buf deep(a, 1024);
        print(&deep, "/q");
        for (i32 i = 0; i < 300; i++) {
            print(&deep, "/a");
        }
        print(&deep, "/src/x.c");
        os->realpaths = 0;
        h = guardhint(os, a, &g, finish(&deep), 1, 1);
        expect(t, "hint of a deep path", h.path, "/w/src/x.c");
        expect(t, "hint of a deep path (lookups)", istr(a, os->realpaths <= 20), "1");
        Buf miss(a, 1024);
        print(&miss, "/q");
        for (i32 i = 0; i < 300; i++) {
            print(&miss, "/a");
        }
        print(&miss, "/none.txt");
        os->realpaths = 0;
        h = guardhint(os, a, &g, finish(&miss), 0, 1);
        expect(t, "hint of a deep new file", h.path, "/w/none.txt");
        expect(t, "hint of a deep new file (lookups)", istr(a, os->realpaths <= 20), "1");
    }

    // Windows: drives, case, prefixes, and the trailing dots
    Os *wos = newos(a, 1<<20);
    os_mkdirs(wos, *a, "C:/W");
    os_mkdirs(wos, *a, "C:/W/Sub");
    os_write(wos, *a, "C:/W/Sub/f.txt", "data");
    Guard wg = newguard(wos, a, "C:\\W", "C:\\Users\\me", {}, "", 1);
    struct { Str path; Str want; } wcases[] = {
        {"Sub\\f.txt",                  "C:/W/Sub/f.txt I"},
        {"c:\\W\\Sub\\f.txt",           "C:/W/Sub/f.txt I"},
        {"\\\\?\\C:\\W\\Sub\\f.txt",    "C:/W/Sub/f.txt I"},
        {"C:Sub\\f.txt",                "C:/W/Sub/f.txt I"},
        {"Sub. \\f.txt",                "C:/W/Sub/f.txt I"},
        {"Sub\\..\\..\\x",              "C:/x O"},
        {".. \\x",                      "C:/W/x I"},
        {"D:\\x",                       "D:/x O"},
        {"D:x",                         "D:/x O"},
        {"\\x",                         "C:/x O"},
        {"\\\\srv\\share\\x",           "//srv/share/x O"},
        {"\\\\?\\UNC\\srv\\share\\x",   "//srv/share/x O"},
        {"\\\\.\\pipe\\x",              "//./pipe/x O"},
        {"~\\x",                        "C:/Users/me/x O"},
        {"C:/W2/x",                     "C:/W2/x O"},
        // Drives as MSYS2, Git Bash and Cygwin print them
        {"/c/W/Sub/f.txt",              "C:/W/Sub/f.txt I"},
        {"/C/W/Sub",                    "C:/W/Sub I"},
        {"/cygdrive/c/W/Sub/f.txt",     "C:/W/Sub/f.txt I"},
        {"/d/x",                        "D:/x O"},
        {"/c",                          "C:/ O"},
        {"/cygdrive/d",                 "D:/ O"},
        {"/cygdrive/",                  "C:/cygdrive O"},
        {"/mnt/c/x",                    "C:/mnt/c/x O"},
        {"/cx/y",                       "C:/cx/y O"},
        {"/tmp/x",                      "C:/tmp/x O"},
        {"\\c\\x",                      "C:/c/x O"},
        {"//c/x",                       "//c/x/ O"},
    };
    for (iz i = 0; i < countof(wcases); i++) {
        GuardPath gp = guardpath(wos, a, &wg, wcases[i].path, 0);
        Str got = concat(a, gp.path, gp.inside ? Str(" I") : gp.diverged ? Str(" D") : Str(" O"));
        expect(t, concat(a, "guard windows ", wcases[i].path), got, wcases[i].want);
    }
    GuardPath wl = guardpath(wos, a, &wg, "c:/w/sub/x", 0);
    expect(t, "guard windows case, lexically", istr(a, within(wg.roots[0].lex, wl.path, 1)), "1");

    // Denials
    GuardDenial d = {};
    d.tool = "write";
    d.gp   = guardpath(os, a, &g, "/q/foo/bar", 0);
    d.cwd  = g.cwd;
    d.hint = guardhint(os, a, &g, d.gp.path, 0, 1);
    Str deny = guarddenial(a, &d);
    expect(t, "denial policy", istr(a, find(deny, "outside the working directory /w")>0 &&
                                      find(deny, "cannot ask the user")>0 &&
                                      find(deny, "Did you mean /w/foo/bar?")>0), "1");
    d.by   = DENY_USER;
    d.note = "use the copy inside";
    deny   = guarddenial(a, &d);
    expect(t, "denial user", istr(a, find(deny, "denied by the user")>=0 &&
                                    find(deny, "The user's note: use the copy inside")>0), "1");
    d.by   = DENY_TIMEOUT;
    d.wait = 30;
    deny   = guarddenial(a, &d);
    expect(t, "denial timeout", istr(a, find(deny, "nobody answered within 30 seconds")>0 &&
                                       find(deny, "as with the shell")>0 &&
                                       find(deny, "in your final response.")>0), "1");
    d.by   = DENY_STEER;
    deny   = guarddenial(a, &d);
    expect(t, "denial steer", istr(a, find(deny, "sent a new message")>0), "1");
    d.by   = DENY_CANCEL;
    expect(t, "denial cancel", guarddenial(a, &d), "Not run: the user cancelled the turn.");

    d.by   = DENY_POLICY;
    d.gp   = guardpath(os, a, &g, "out/passwd", 0);
    d.hint = {};
    deny   = guarddenial(a, &d);
    expect(t, "denial symlink", istr(a, find(deny, "symbolic link to /etc/passwd")>0), "1");
    d.gp   = guardpath(os, a, &g, "/new.txt", 0);
    d.hint = guardhint(os, a, &g, d.gp.path, 0, 1);
    deny   = guarddenial(a, &d);
    expect(t, "denial example", istr(a, find(deny, "(for example /w/new.txt)")>0), "1");
    deny   = guardstatedenial(a, "write", "/s/sessions/x/events.jsonl", "/s", "/w");
    expect(t, "denial state", deny,
           "Permission denied: write of /s/sessions/x/events.jsonl would write "
           "under the harness's state directory /s, where it keeps its "
           "sessions' logs, locks and caches. Bypass mode allows writes "
           "outside the working directory, but never there, so nothing was "
           "done. Do not retry this path. Write somewhere else instead, for "
           "example inside /w.");

    // What "always" grants: the directory, a search's directory itself,
    // the file alone in a root or the home directory, never those whole,
    // and where a link leads out, the real directory
    struct { Str path; b32 isdir; Str want; } grants[] = {
        {"/x/y/f.txt",   0, "/x/y"},
        {"/x/y",         1, "/x/y"},
        {"/f.txt",       0, "/f.txt"},
        {"/home/u/f",    0, "/home/u/f"},
        {"~/sub/f",      0, "/home/u/sub"},
        {"/",            1, "(none)"},
        {"/home/u",      1, "(none)"},
        {"/home/u/",     1, "(none)"},
        {"out/passwd",   0, "/etc"},
        {"out",          1, "/etc"},
    };
    for (iz i = 0; i < countof(grants); i++) {
        GuardPath gp = guardpath(os, a, &g, grants[i].path, 1);
        Str       gr = guardgrant(a, &g, &gp, grants[i].isdir);
        expect(t, concat(a, "grant ", grants[i].path), gr.data ? gr : Str("(none)"),
               grants[i].want);
    }

    // Grants and temporary directories as hidden roots: a directory not
    // made yet, a file, one for reads alone, and /tmp, which is a link
    // here as on macOS, where a link planted inside leads out still
    os_mkdirs(os, *a, "/private/tmp");
    os_mkdirs(os, *a, "/g");
    os_write(os, *a, "/g/file.txt", "data");
    os_write(os, *a, "/private/tmp/near/x.c", "data");
    vlink(os, "/private/tmp/evil", "/etc");
    Guard h = newguard(os, a, "/w", "/home/u", {}, "/s/sess", 0);
    guardadd(os, a, &h, "/g/later", ROOT_HIDDEN|ROOT_MISSING);
    guardadd(os, a, &h, "/g/file.txt", ROOT_HIDDEN|ROOT_MISSING);
    guardadd(os, a, &h, "/x/extra", ROOT_HIDDEN|ROOT_MISSING|ROOT_READ);
    guardadd(os, a, &h, "/tmp", ROOT_HIDDEN);
    guardadd(os, a, &h, "/nowhere", ROOT_HIDDEN);
    expect(t, "hidden roots", istr(a, h.roots.len), "6");
    expect(t, "hidden root real", h.roots[5].real, "/private/tmp");
    struct { Str path; b32 read; Str want; } hidden[] = {
        {"/g/later/new.txt",    0, "/g/later/new.txt I"},
        {"/g/later",            0, "/g/later I"},
        {"/g/other.txt",        0, "/g/other.txt O"},
        {"/g/file.txt",         0, "/g/file.txt I"},
        {"/x/extra/f",          1, "/x/extra/f I"},
        {"/x/extra/f",          0, "/x/extra/f O"},
        {"/tmp/t.txt",          0, "/tmp/t.txt I"},
        {"/private/tmp/t.txt",  0, "/private/tmp/t.txt I"},
        {"/tmp/evil/passwd",    1, "/tmp/evil/passwd D"},
        {"/nowhere/f",          0, "/nowhere/f O"},
    };
    for (iz i = 0; i < countof(hidden); i++) {
        GuardPath gp = guardpath(os, a, &h, hidden[i].path, hidden[i].read);
        Str got = concat(a, gp.path, gp.inside ? Str(" I") : gp.diverged ? Str(" D") : Str(" O"));
        expect(t, concat(a, "hidden root ", hidden[i].path), got, hidden[i].want);
    }
    // A hint is never in a hidden root
    GuardHint nh = guardhint(os, a, &h, "/q/near/x.c", 1, 0);
    expect(t, "no hint in a hidden root", nh.path.data ? nh.path : Str("(none)"), "(none)");
}
