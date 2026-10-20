// Guard tests: lexical path normalization and containment, POSIX and
// Windows

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
        {"C:\\w", ".", "C:/w"},
    };
    for (iz i = 0; i < countof(win); i++) {
        Str got = normpath(a, win[i].cwd, win[i].path, 1);
        expect(t, concat(a, "windows ", win[i].path), got, win[i].want);
    }

    expect(t, "within itself", istr(a, within("/a/b", "/a/b", 0)), "1");
    expect(t, "within below", istr(a, within("/a/b", "/a/b/c", 0)), "1");
    expect(t, "within sibling", istr(a, within("/a/b", "/a/bc", 0)), "0");
    expect(t, "within case posix", istr(a, within("/a/B", "/a/b/c", 0)), "0");
    expect(t, "within case windows", istr(a, within("C:/Users/Me", "C:/users/me/x", 1)), "1");
    expect(t, "within root", istr(a, within("/", "/anything", 0)), "1");

    GuardPath g = guardpath(a, "/w", "/s/out", "src/x.c", 0, 0);
    expect(t, "guard inside", concat(a, g.path, istr(a, g.inside)), "/w/src/x.c1");
    g = guardpath(a, "/w", "/s/out", "../x", 0, 0);
    expect(t, "guard outside", concat(a, g.path, istr(a, g.inside)), "/x0");
    g = guardpath(a, "/w", "/s/out", "/s/out/c1.log", 1, 0);
    expect(t, "guard spill readable", istr(a, g.inside), "1");
    g = guardpath(a, "/w", "/s/out", "/s/out/c1.log", 0, 0);
    expect(t, "guard spill not writable", istr(a, g.inside), "0");
    g = guardpath(a, "/w", "", Str{(u8 *)"x\0/../../etc", 12}, 1, 0);
    expect(t, "guard null byte", istr(a, g.inside), "0");
    g = guardpath(a, "C:\\Work", "", "c:\\work\\Sub\\f.txt", 0, 1);
    expect(t, "guard windows case", istr(a, g.inside), "1");

    Str deny = guarddenial(a, "/w", "/x.txt", "write");
    expect(t, "denial hint", istr(a, find(deny, "outside the working directory /w")>0 &&
                                     find(deny, "(for example /w/x.txt)")>0), "1");
}
