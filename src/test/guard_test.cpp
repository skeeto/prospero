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
    expect(t, "denial timeout", istr(a, find(deny, "nobody answered within 30 seconds")>0), "1");
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
}
