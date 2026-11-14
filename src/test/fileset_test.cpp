// Files tab sets: adding, evicting, dropping and reordering, which tab
// shows after each, the labels tabs show, and a file's bytes as text.

static Str filesetstr(Arena *a, FileSet *s, FileTab *tabs)
{
    // Each tab by its letter, the front one starred
    Buf b(a, 64);
    for (i32 i = 0; i < s->n; i++) {
        if (i == s->front) {
            print(&b, "*");
        }
        u8 c = (u8)('a' + (s->tabs[i] - tabs));
        print(&b, Str{&c, 1});
    }
    return finish(&b);
}

// The labels, in order, joined by '|'.
static Str filelabelstr(Arena *a, Str *labels, i32 n)
{
    Buf b(a, 64);
    for (i32 i = 0; i < n; i++) {
        print(&b, i ? Str("|") : Str(""));
        print(&b, labels[i]);
    }
    return finish(&b);
}

// Normalized text, then "[lines cols flags]", flags as letters: Binary,
// Mark, Cut, Utf-16, Windows-1252.
static Str filetextstr(Arena *a, FileText ft)
{
    Buf b(a, 64);
    print(&b, ft.text);
    print(&b, "[");
    print(&b, ft.lines);
    print(&b, " ");
    print(&b, ft.cols);
    print(&b, " ");
    print(&b, ft.flags&FTEXT_BINARY ? Str("B") : Str(""));
    print(&b, ft.flags&FTEXT_BOM    ? Str("M") : Str(""));
    print(&b, ft.flags&FTEXT_CUT    ? Str("C") : Str(""));
    print(&b, ft.flags&FTEXT_UTF16  ? Str("U") : Str(""));
    print(&b, ft.flags&FTEXT_1252   ? Str("W") : Str(""));
    print(&b, "]");
    return finish(&b);
}

// filenext's decision: "check" or "none", and "@" its wake.
static Str filenextstr(Arena *a, FileWatch w, i64 now)
{
    FileNext n = filenext(&w, now);
    Buf      b(a, 32);
    print(&b, n.check ? Str("check") : Str("none"));
    if (n.wake) {
        putbyte(&b, '@');
        print(&b, n.wake);
    }
    return finish(&b);
}

static void test_fileset(Test *t)
{
    Arena a = t->perm;

    static FileTab tabs[FILES_TABS+2];
    for (i32 i = 0; i < FILES_TABS+2; i++) {
        tabs[i] = {};
    }

    FileSet s = {};
    expect(t, "fileset.empty", filesetstr(&a, &s, tabs), "");
    for (i32 i = 0; i < 3; i++) {
        filesetadd(&s, tabs+i);
    }
    expect(t, "fileset.add", filesetstr(&a, &s, tabs), "ab*c");

    // The front tab dropped: its right neighbour, else its left
    filesetshow(&s, 1);
    expect(t, "fileset.drop", filesetdrop(&s, 1)==tabs+1 ? Str("b") : Str("other"), "b");
    expect(t, "fileset.dropright", filesetstr(&a, &s, tabs), "a*c");
    filesetdrop(&s, 1);
    expect(t, "fileset.dropleft", filesetstr(&a, &s, tabs), "*a");
    filesetdrop(&s, 0);
    expect(t, "fileset.droplast", filesetstr(&a, &s, tabs), "");
    expect(t, "fileset.dropclears", s.frontset ? Str("set") : Str("unset"), "unset");

    // Full: a new tab evicts the one least recently opened, picking
    // counting as opening, and never the new tab itself
    FileSet full = {};
    for (i32 i = 0; i < FILES_TABS; i++) {
        expect(t, "fileset.fill", filesetadd(&full, tabs+i) ? Str("evicted") : Str("none"),
               "none");
    }
    filesetshow(&full, 0);
    FileTab *gone = filesetadd(&full, tabs+FILES_TABS);
    expect(t, "fileset.evict", gone==tabs+1 ? Str("b") : Str("other"), "b");
    expect(t, "fileset.evicted", filesetstr(&a, &full, tabs), "acdefgh*i");
    gone = filesetadd(&full, tabs+1);
    expect(t, "fileset.evictnext", gone==tabs+2 ? Str("c") : Str("other"), "c");
    expect(t, "fileset.evictednext", filesetstr(&a, &full, tabs), "adefghi*b");

    // Found by host and path, without case on Windows
    FileSet f = {};
    tabs[0].host = 0;
    tabs[0].path = "/w/a.c";
    tabs[1].host = 7;
    tabs[1].path = "/w/a.c";
    tabs[2].host = 3;
    tabs[2].win  = 1;
    tabs[2].path = "C:/W/A.c";
    for (i32 i = 0; i < 3; i++) {
        filesetadd(&f, tabs+i);
    }
    expect(t, "fileset.find", istr(&a, filesetfind(&f, 7, "/w/a.c", 0)), "1");
    expect(t, "fileset.findlocal", istr(&a, filesetfind(&f, 0, "/w/a.c", 0)), "0");
    expect(t, "fileset.findcase", istr(&a, filesetfind(&f, 0, "/w/A.c", 0)), "-1");
    expect(t, "fileset.findwin", istr(&a, filesetfind(&f, 3, "c:/w/a.C", 1)), "2");
    expect(t, "fileset.findhost", istr(&a, filesetfind(&f, 3, "/w/a.c", 0)), "-1");

    // The bar's order: tabs it doesn't have keep their place after, and
    // tabs not in the set are ignored; the front follows its tab
    FileSet o = {};
    for (i32 i = 0; i < 4; i++) {
        filesetadd(&o, tabs+i);
    }
    filesetshow(&o, 1);
    FileTab *order1[] = {tabs+3, tabs+1, tabs+0, tabs+2};
    filesetorder(&o, order1, 4);
    expect(t, "fileset.order", filesetstr(&a, &o, tabs), "d*bac");
    FileTab *order2[] = {tabs+7, tabs+2, tabs+1};
    filesetorder(&o, order2, 3);
    expect(t, "fileset.orderpart", filesetstr(&a, &o, tabs), "c*bda");

    // Labels: the name, with as few folders as tell it apart
    {
        Measure m       = fixedmeasure();
        Str     hosts[] = {"here", "pi", "here", "here", "here"};
        struct { Str path[5]; i32 n; b32 win; f32 width; Str want; } labels[] = {
            {{"/w/src/main.c", "/w/test/main.c", "/w/src/ui.cpp"}, 3, 0, 0,
             "src/main.c|test/main.c|ui.cpp"},
            {{"/a/x/y/z.c", "/b/x/y/z.c"}, 2, 0, 0, "a/x/y/z.c|b/x/y/z.c"},
            {{"/x/a.c", "/y/x/a.c"}, 2, 0, 0, "/x/a.c|y/x/a.c"},
            {{"/w/a##b.c", "/w/c###d"}, 2, 0, 0, "a# #b.c|c# # #d"},
            {{"/w/abcdefghij.c"}, 1, 0, 60, "ab\xe2\x80\xa6j.c"},
            {{"/w/abcde.c"}, 1, 0, 70, "abcde.c"},
            {{"C:/x/Main.c", "C:/y/main.c"}, 2, 1, 0, "x/Main.c|y/main.c"},
            {{"/x/Main.c", "/y/main.c"}, 2, 0, 0, "Main.c|main.c"},
        };
        for (iz i = 0; i < countof(labels); i++) {
            FileSet ls = {};
            for (i32 k = 0; k < labels[i].n; k++) {
                tabs[k] = {};
                tabs[k].path = labels[i].path[k];
                tabs[k].win  = labels[i].win;
                filesetadd(&ls, tabs+k);
            }
            Str *got = filelabels(&a, &ls, hosts, labels[i].width, &m);
            expect(t, concat(&a, "filelabels ", labels[i].want),
                   filelabelstr(&a, got, ls.n), labels[i].want);
        }

        // The same path from two hosts, marked, beside another file; on
        // Windows hosts, the same path in another case; and "##" in a
        // host's name broken up as in a path
        struct { Str path[3]; i32 n; b32 win; Str hosts[3]; Str want; } twins[] = {
            {{"/w/a.c", "/w/a.c", "/v/a.c"}, 3, 0, {"here", "pi", "here"},
             "here:w/a.c|pi:w/a.c|v/a.c"},
            {{"C:/W/a.c", "c:/w/A.c"}, 2, 1, {"win", "box"}, "win:a.c|box:A.c"},
            {{"/w/a.c", "/w/a.c"}, 2, 0, {"pi##2", "here"}, "pi# #2:a.c|here:a.c"},
        };
        for (iz i = 0; i < countof(twins); i++) {
            FileSet ls = {};
            for (i32 k = 0; k < twins[i].n; k++) {
                tabs[k] = {};
                tabs[k].path = twins[i].path[k];
                tabs[k].host = (u64)k;
                tabs[k].win  = twins[i].win;
                filesetadd(&ls, tabs+k);
            }
            Str *got = filelabels(&a, &ls, twins[i].hosts, 0, &m);
            expect(t, concat(&a, "filelabels.hosts ", twins[i].want),
                   filelabelstr(&a, got, ls.n), twins[i].want);
        }
    }

    // Text as shown
    struct { Str raw; b32 astext; Str want; } texts[] = {
        {"a\nb\n",              0, "a\nb\n[2 1 ]"},
        {"a\nbc",               0, "a\nbc[2 2 ]"},
        {"",                    0, "[0 0 ]"},
        {"\n\n",                0, "\n\n[2 0 ]"},
        {"a\r\nb\r\n",          0, "a\nb\n[2 1 ]"},
        {"a\rb\r",              0, "a\xe2\x90\x8d" "b\xe2\x90\x8d[1 4 ]"},
        {"\r\r\n",              0, "\xe2\x90\x8d\n[1 1 ]"},
        {"\x01\x1b\x7f\tz",     0, "\xe2\x90\x81\xe2\x90\x9b\xe2\x90\xa1\tz[1 5 ]"},
        {"\tx\nab\tc\nabcd\t",  0, "\tx\nab\tc\nabcd\t[3 8 ]"},
        {"\xef\xbb\xbfhi\n",    0, "hi\n[1 2 M]"},
        {"\xff\xfeh\0i\0\r\0\n\0", 0, "hi\n[1 2 MU]"},
        {"\xfe\xff\0h\0i",      0, "hi[1 2 MU]"},
        {"\xfe\xff\0a\0\r\0\n\0b", 0, "a\nb[2 1 MU]"},   // CRLF, big-endian
        {"\xfe\xff\0a\0\r",     0, "a\xe2\x90\x8d[1 2 MU]"},  // a lone CR last
        {"\xff\xfe" "a\0\r\0", 0, "a\xe2\x90\x8d[1 2 MU]"},
        {"\xff\xfe=\xd8\0\xde", 0, "\xf0\x9f\x98\x80[1 1 MU]"},  // a pair
        {"\xff\xfe\0\xdcx\0",   0, "\xef\xbf\xbdx[1 2 MU]"},      // a lone low half
        {"\xff\xfe=\xd8x\0",    0, "\xef\xbf\xbdx[1 2 MU]"},      // a lone high half
        {"\xff\xfex\0y",        0, "x\xef\xbf\xbd[1 2 MU]"},      // an odd byte
        {"\xff\xfe\0\0",        0, "\xe2\x90\x80[1 1 MU]"},       // NUL is no binary
        {"a\0b",                0, "[0 0 B]"},
        {"a\0b",                1, "a\xe2\x90\x80" "b[1 3 B]"},
        {"caf\xc3\xa9 \xff\n",  0, "caf\xc3\xa9 \xef\xbf\xbd\n[1 6 ]"},
        {"\xed\xa0\x80" "\xc3\xa9", 0, "\xef\xbf\xbd\xc3\xa9[1 2 ]"},  // a surrogate
        {"caf\xe9 \x80\x81\x9f\n", 0, "caf\xc3\xa9 \xe2\x82\xac\xef\xbf\xbd\xc5\xb8\n[1 8 W]"},
        {"\xc0\xaf",            0, "\xc3\x80\xc2\xaf[1 2 W]"},
    };
    for (iz i = 0; i < countof(texts); i++) {
        Str got = filetextstr(&a, filenorm(&a, texts[i].raw, texts[i].astext, FILES_MAX,
                                           FILES_LINES));
        expect(t, concat(&a, "filenorm ", texts[i].want), got, texts[i].want);
    }

    // A NUL past the first 8 KiB is not binary
    {
        Buf b(&a, 9000);
        for (i32 i = 0; i < 8192; i++) {
            putbyte(&b, 'x');
        }
        putbyte(&b, 0);
        FileText ft = filenorm(&a, finish(&b), 0, FILES_MAX, FILES_LINES);
        expect(t, "filenorm.latenul", istr(&a, ft.text.len), "8195");
    }

    // Cut at lines and at bytes, at a line
    struct { Str raw; iz maxbytes; i32 maxlines; Str want; } cuts[] = {
        {"a\nb\nc\n", 100, 2, "a\nb\n[2 1 C]"},
        {"a\nb\nc",   100, 2, "a\nb\n[2 1 C]"},
        {"a\nb",      100, 2, "a\nb[2 1 ]"},
        {"a\nb\n",    100, 2, "a\nb\n[2 1 ]"},
        {"ab\ncd\n",  5,   9, "ab\n[1 2 C]"},
        {"ab\ncd\n",  6,   9, "ab\ncd\n[2 2 ]"},
        {"ab\ncdef",  6,   9, "ab\n[1 2 C]"},
        {"abcdef",    4,   9, "abcd[1 4 C]"},
        {"ab\xc3\xa9", 3,  9, "ab[1 2 C]"},
        {"ab\n",      0,   9, "[0 0 C]"},
        {"ab\n",      9,   0, "[0 0 C]"},
        {"\xff\xfe\xac\x20\xac\x20\n\0", 4, 9, "\xe2\x82\xac[1 1 MCU]"},
        {"\x80\x80x\n", 5,  9, "\xe2\x82\xac[1 1 CW]"},
    };
    for (iz i = 0; i < countof(cuts); i++) {
        Str got = filetextstr(&a, filenorm(&a, cuts[i].raw, 0, cuts[i].maxbytes,
                                           cuts[i].maxlines));
        expect(t, concat(&a, "filenorm cut ", cuts[i].want), got, cuts[i].want);
    }

    // A head read short of the file's end, up to its last whole line
    struct { Str raw; iz want; } wholes[] = {
        {"ab\ncd",                  3},
        {"ab\r\ncd\r",             4},  // CRLF
        {"abcd",                    4},  // no LF: all, for filenorm to cut
        {"",                        0},
        {"\n",                      1},
        {"\xff\xfe" "a\0\n\0b\0",  6},  // UTF-16LE
        {"\xff\xfe" "a\0\n\0\n",   6},  // an odd last byte
        {"\xff\xfe" "\n\0\x0aN" "a",  4},  // U+4E0A's 0A no LF
        {"\xff\xfe\x0a\x0a\x0aN",    6},  // nor U+0A0A's, nor U+4E0A's
        {"\xff\xfe" "abc",           4},  // no LF, whole units
        {"\xfe\xff\0a\0\n\0b",      6},  // UTF-16BE
        {"\xfe\xff\0\n\x0a\0",      4},  // U+0A00's 0A no LF
        {"\xfe\xff\0\r\0\n\0b",     6},  // CRLF, big-endian
        {"\xfe\xff",                2},
    };
    for (iz i = 0; i < countof(wholes); i++) {
        expect(t, concat(&a, "filewhole ", istr(&a, (i64)i)),
               istr(&a, filewhole(wholes[i].raw)), istr(&a, wholes[i].want));
    }

    // Decoding that grows past maxbytes, though the file is smaller:
    // windows-1252 at 3 bytes a byte, U+FFFD runs, and UTF-16
    {
        Buf b(&a, 512);
        for (i32 i = 0; i < 101; i++) {
            print(&b, "\x80\x80\x80\n");  // 4 bytes, 10 shown
        }
        Str      raw = finish(&b);
        FileText ft  = filenorm(&a, raw, 0, 1000, FILES_LINES);
        expect(t, "filenorm.1252grow", raw.len<1000 ? Str("smaller") : Str("larger"), "smaller");
        expect(t, "filenorm.1252", filetextstr(&a, {{}, ft.lines, ft.cols, ft.flags}),
               "[100 3 CW]");
        expect(t, "filenorm.1252len", istr(&a, ft.text.len), "1000");
        ft = filenorm(&a, raw, 0, 1009, FILES_LINES);
        expect(t, "filenorm.1252under", istr(&a, ft.text.len), "1000");
    }
    {
        Buf b(&a, 512);
        print(&b, "\xc3\xa9\n");
        for (i32 i = 0; i < 10; i++) {
            print(&b, "\xff\xff\xff\n");  // 4 bytes, 10 shown
        }
        Str      raw = finish(&b);
        FileText ft  = filenorm(&a, raw, 0, 50, FILES_LINES);
        expect(t, "filenorm.fffd", filetextstr(&a, {{}, ft.lines, ft.cols, ft.flags}),
               "[5 3 C]");
        expect(t, "filenorm.fffdlen", istr(&a, ft.text.len), "43");
        expect(t, "filenorm.fffdsmall", raw.len<50 ? Str("smaller") : Str("larger"), "smaller");
    }
    {
        Buf b(&a, 512);
        print(&b, "\xff\xfe");
        for (i32 i = 0; i < 20; i++) {
            print(&b, Str("\xac\x20\xac\x20\xac\x20\xac\x20\n\0"));  // 10 bytes, 13 shown
        }
        Str      raw = finish(&b);
        FileText ft  = filenorm(&a, raw, 0, 250, FILES_LINES);
        expect(t, "filenorm.utf16", filetextstr(&a, {{}, ft.lines, ft.cols, ft.flags}),
               "[19 4 MCU]");
        expect(t, "filenorm.utf16len", istr(&a, ft.text.len), "247");
        expect(t, "filenorm.utf16small", raw.len<250 ? Str("smaller") : Str("larger"), "smaller");
    }

    // Expansion never grows the buffer: the arena takes at most maxbytes
    {
        Buf b(&a, 1<<15);
        for (i32 i = 0; i < 1<<15; i++) {
            putbyte(&b, 1);  // 3 bytes shown each
        }
        Str      raw  = finish(&b);
        byte    *save = a.beg;
        FileText ft   = filenorm(&a, raw, 0, 1<<16, FILES_LINES);
        expect(t, "filenorm.alloc", istr(&a, a.beg - save), "65535");
        expect(t, "filenorm.alloclen", istr(&a, ft.text.len), "65535");
    }

    // Cited lines clamped
    expect(t, "fileclamp.in", istr(&a, fileclamp(5, 10)), "5");
    expect(t, "fileclamp.past", istr(&a, fileclamp(11, 10)), "10");
    expect(t, "fileclamp.zero", istr(&a, fileclamp(0, 10)), "0");
    expect(t, "fileclamp.neg", istr(&a, fileclamp(-3, 10)), "0");
    expect(t, "fileclamp.empty", istr(&a, fileclamp(3, 0)), "0");

    // Checks: only once stale, a second apart, at once for Reload, and
    // after one in flight answers or is lost
    {
        i64 t0 = 1793546320000;
        struct { b32 stale, force; i64 id, sent, now; Str want; } cases[] = {
            {0, 0, 0, 0, t0, "none"},                        // never marked
            {1, 0, 0, 0, t0, "check"},                       // never checked
            {1, 0, 0, t0-400, t0, "none@1793546320600"},     // within the gap
            {1, 0, 0, t0-1000, t0, "check"},                 // past it
            {0, 1, 0, t0-100, t0, "check"},                  // Reload: no gap
            {1, 1, 0, t0-100, t0, "check"},
            {0, 0, 7, t0-100, t0, "none"},                   // in flight, unmarked
            {1, 0, 7, t0-5000, t0, "none@1793546345000"},    // marked meanwhile
            {1, 0, -1, t0-5000, t0, "none"},                 // its read in flight
            {1, 1, -1, t0-60000, t0, "none"},                // however long
            {0, 1, 7, t0-100, t0, "none@1793546349900"},     // Reload waits for it
            {1, 0, 7, t0-30000, t0, "check"},                // lost
            {1, 0, 0, t0-5000, t0, "check"},                 // answered: once more
        };
        for (auto &c : cases) {
            FileWatch w = {};
            w.stale = c.stale;
            w.force = c.force;
            w.id    = c.id;
            w.sent  = c.sent;
            expect(t, "filenext", filenextstr(&a, w, c.now), c.want);
        }
    }

    // Read again when its size or mtime changed, or its mtime was within
    // 2 s of the read either way, which a coarse clock may hide
    {
        i64 t0 = 1793546320000;
        struct { i64 size, mtime, readat, size1, mtime1; b32 want; } cases[] = {
            {10, t0-60000, t0, 10, t0-60000, 0},  // as it was
            {10, t0-60000, t0, 11, t0-60000, 1},  // grew
            {10, t0-60000, t0, 10, t0-59000, 1},  // touched
            {10, t0-60000, t0, 10, t0-61000, 1},  // older
            {10, t0-1999, t0, 10, t0-1999, 1},    // near its read
            {10, t0-2000, t0, 10, t0-2000, 0},
            {10, t0+1500, t0, 10, t0+1500, 1},    // ahead of this clock
            {10, t0+2000, t0, 10, t0+2000, 0},
        };
        for (auto &c : cases) {
            b32 got = filechanged(c.size, c.mtime, c.readat, c.size1, c.mtime1);
            expect(t, "filechanged", istr(&a, got), istr(&a, c.want));
        }
    }
}

// A file find's hits as "chunk:off+len;", then "+" if full.
static Str filehitstr(Arena *a, FileFind *f)
{
    Buf b(a, 64);
    for (iz i = 0; i < f->hits.len; i++) {
        FindHit *h = &f->hits[i];
        print(&b, (i64)h->msg);
        putbyte(&b, ':');
        print(&b, (i64)h->off);
        putbyte(&b, '+');
        print(&b, (i64)h->len);
        putbyte(&b, ';');
    }
    print(&b, f->full ? Str("+") : Str(""));
    return finish(&b);
}

// Marks as filehitstr has hits, then "@cur", and "*" if carried.
static Str filemarkstr(Arena *a, Marks mk, b32 carried)
{
    Buf b(a, 64);
    for (iz i = 0; i < mk.n; i++) {
        print(&b, (i64)mk.h[i].msg);
        putbyte(&b, ':');
        print(&b, (i64)mk.h[i].off);
        putbyte(&b, '+');
        print(&b, (i64)mk.h[i].len);
        putbyte(&b, ';');
    }
    putbyte(&b, '@');
    print(&b, (i64)mk.cur);
    print(&b, carried ? Str("*") : Str(""));
    return finish(&b);
}

// Go to line's parse as "line0-line1:col", else "no".
static Str filegotostr(Arena *a, Str s)
{
    FrefLines l = {};
    if (!filegotoparse(s, &l)) {
        return "no";
    }
    Buf b(a, 64);
    print(&b, (i64)l.line0);
    putbyte(&b, '-');
    print(&b, (i64)l.line1);
    putbyte(&b, ':');
    print(&b, (i64)l.col);
    return finish(&b);
}

// Lines "x" to fill a chunk up to line k (0-based), then rest.
static Str filelinesto(Arena *a, i32 k, Str rest)
{
    Buf b(a, 64);
    for (i32 i = 0; i < k; i++) {
        print(&b, "x\n");
    }
    print(&b, rest);
    return finish(&b);
}

static void test_filefind(Test *t)
{
    Arena     a  = t->perm;
    Os       *os = newos(&a, 1<<12);
    FileFind *f  = newfilefind(&a, os_reserve(os, FILES_HITMEM), 256, FILES_HITS);

    auto find = [&](Str text, Str q) {
        filefindreset(f, text, q);
        filefindstep(f, (iz)1<<30);
        return filehitstr(&a, f);
    };

    // Matching: ASCII case folded, a run of whitespace one space, across
    // lines; hits apart, a leading space a run's start, a trailing space
    // one byte of its run, and other bytes compared as they are
    expect(t, "filefind.case", find("a line here\n", "LINE"), "0:2+4;");
    expect(t, "filefind.space", find("xa \t\n\n by", "a b"), "0:1+7;");
    expect(t, "filefind.lines", find("one\n\ntwo\n", "one  TWO"), "0:0+8;");
    expect(t, "filefind.apart", find("aaaa", "aa"), "0:0+2;0:2+2;");
    expect(t, "filefind.lead", find("x  b\tb", " b"), "0:1+3;0:4+2;");
    expect(t, "filefind.lead", find("x a  a b", " a "), "0:1+3;");
    expect(t, "filefind.trail", find("a  b", "a "), "0:0+2;");
    expect(t, "filefind.utf8", find("\xc3\x9cn\xc3\x8f \xc3\xbcn\xc3\xaf \xc3\x9cN\xc3\x8f",
                                    "\xc3\x9cN\xc3\x8f"), "0:0+5;0:12+5;");
    expect(t, "filefind.none", find("abc", "abcd"), "");

    // Hits by chunk: line 255 to 257 across a blank line 256, from line
    // 255's start in chunk 0, and chunk 1 from the 256th line break
    {
        Buf b(&a, 64);
        iz  at255 = 0, at256 = 0, at299 = 0;
        for (i32 i = 0; i < 300; i++) {
            at255 = i==255 ? b.len : at255;
            at256 = i==256 ? b.len : at256;
            at299 = i==299 ? b.len : at299;
            if (i != 256) {
                print(&b, "line ");
                print(&b, (i64)i);
            }
            putbyte(&b, '\n');
        }
        Str text = finish(&b);
        Str want = concat(&a, concat(&a, "0:", istr(&a, at255)), "+18;");
        expect(t, "filefind.chunk", find(text, "line 255 line 257"), want);
        expect(t, "filefind.chunkat", istr(&a, f->chunkat[1]), istr(&a, at256));
        want = concat(&a, concat(&a, "1:", istr(&a, at299 - at256)), "+8;");
        expect(t, "filefind.chunk1", find(text, "line 299"), want);
    }

    // Stepping: a small budget goes on where it left off, to the same
    // hits; a cap keeps hitmax hits, full once another is found; nothing
    // to find is done at once
    {
        Str text = filelinesto(&a, 600, "the end x\n");
        Str all  = find(text, "x ");
        filefindreset(f, text, "x ");
        expect(t, "filefind.budget", istr(&a, filefindstep(f, 10)), "0");
        i32 steps = 1;
        for (; !filefindstep(f, 10); steps++) {}
        expect(t, "filefind.budget", filehitstr(&a, f), all);
        expect(t, "filefind.steps", istr(&a, steps>100), "1");

        FileFind *g = newfilefind(&a, os_reserve(os, FILES_HITMEM), 256, 4);
        filefindreset(g, "aaaaaaaaaa", "a");
        expect(t, "filefind.cap", istr(&a, filefindstep(g, 1<<20)), "1");
        expect(t, "filefind.cap", filehitstr(&a, g), "0:0+1;0:1+1;0:2+1;0:3+1;+");
        filefindreset(g, "aaaa", "a");
        filefindstep(g, 1<<20);
        expect(t, "filefind.capexact", filehitstr(&a, g), "0:0+1;0:1+1;0:2+1;0:3+1;");

        // The real cap in its real Region: FILES_HITS hits kept, full
        Buf hb(&a, FILES_HITS+1);
        for (i32 i = 0; i <= FILES_HITS; i++) {
            putbyte(&hb, 'a');
        }
        filefindreset(f, finish(&hb), "a");
        expect(t, "filefind.hitsmax", istr(&a, filefindstep(f, (iz)1<<30)), "1");
        expect(t, "filefind.hitsmax", istr(&a, f->hits.len), istr(&a, FILES_HITS));
        expect(t, "filefind.hitsmax", istr(&a, f->full), "1");

        filefindreset(f, "abc", "");
        expect(t, "filefind.empty", istr(&a, f->done && filefindstep(f, 1)), "1");
        expect(t, "filefind.empty", istr(&a, f->hits.len), "0");
        filefindreset(f, "", "abc");
        expect(t, "filefind.empty", istr(&a, f->done && filefindstep(f, 1)), "1");
        filefindreset(f, "abc", "   ");
        filefindstep(f, 1<<20);
        expect(t, "filefind.blank", filehitstr(&a, f), "");

        find("ab ab ab", "ab");
        expect(t, "filefind.from", istr(&a, filefindfrom(f, 0, 4)), "1");  // inside
        expect(t, "filefind.from", istr(&a, filefindfrom(f, 0, 5)), "2");
        expect(t, "filefind.from", istr(&a, filefindfrom(f, 0, 8)), "3");  // past
        expect(t, "filefind.from", istr(&a, filefindfrom(f, 1, 0)), "3");
    }

    // Marks by chunk: into hits as they are, unless a hit from an
    // earlier chunk runs into it, whose tail then comes first, and the
    // current hit is remapped past it
    {
        b32 carried = 0;
        find("ab ab\n", "ab");
        Marks mk = filemarks(&a, f, 0, 0, &carried);
        expect(t, "filemarks.plain", filemarkstr(&a, mk, carried), "0:0+2;0:3+2;@-1");
        expect(t, "filemarks.plain", istr(&a, mk.h == f->hits.data), "1");

        Str text = filelinesto(&a, 255, "foo\nbar\nfoo bar\n");
        find(text, "FOO BAR");
        iz  c1   = f->chunkat[1];
        expect(t, "filemarks.chunkat", istr(&a, c1), istr(&a, 255*2 + 4));
        mk = filemarks(&a, f, 0, 0, &carried);
        expect(t, "filemarks.carry0", filemarkstr(&a, mk, carried), "0:510+7;@-1");
        mk = filemarks(&a, f, 1, c1, &carried);
        expect(t, "filemarks.carry", filemarkstr(&a, mk, carried), "1:0+3;1:4+7;@-1*");
        f->cur = 1;
        mk = filemarks(&a, f, 1, c1, &carried);
        expect(t, "filemarks.cur", filemarkstr(&a, mk, carried), "1:0+3;1:4+7;@1*");
        f->cur = 0;
        mk = filemarks(&a, f, 1, c1, &carried);
        expect(t, "filemarks.curtail", filemarkstr(&a, mk, carried), "1:0+3;1:4+7;@0*");
        mk = filemarks(&a, f, 2, f->text.len, &carried);
        expect(t, "filemarks.after", filemarkstr(&a, mk, carried), "@-1");

        // A hit through 300 blank lines, from chunk 0 into chunks 1 and 2
        Buf b(&a, 64);
        print(&b, filelinesto(&a, 255, "foo\n"));
        for (i32 i = 0; i < 300; i++) {
            putbyte(&b, '\n');
        }
        print(&b, "bar\n");
        text = finish(&b);
        find(text, "foo bar");
        iz end = text.len - 1;
        expect(t, "filemarks.span", filehitstr(&a, f), "0:510+307;");
        for (i32 c = 1; c <= 2; c++) {
            mk = filemarks(&a, f, c, f->chunkat[c], &carried);
            Str want = concat(&a, concat(&a, concat(&a, istr(&a, c), ":0+"),
                                         istr(&a, end - f->chunkat[c])), ";@-1*");
            expect(t, "filemarks.span", filemarkstr(&a, mk, carried), want);
        }
    }

    // Lines and columns: a chunk's line offsets, and a byte's line and
    // column, a column counting code points and a tab one, both ways
    {
        Str chunk = "ab\ncd\n\nef";
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 0)), "0");
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 1)), "3");
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 2)), "6");
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 3)), "7");
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 4)), "9");
        expect(t, "fileline.off", istr(&a, filelineoff(chunk, 99)), "9");

        auto lc = [&](Str text, iz off) {
            i32 line = 0, col = 0;
            fileoffline(text, off, &line, &col);
            Str l = cuthead(text, filelineoff(text, line-1));
            Buf b(&a, 32);
            print(&b, (i64)line);
            putbyte(&b, ':');
            print(&b, (i64)col);
            putbyte(&b, '=');
            print(&b, (i64)(filelineoff(text, line-1) + filecolbyte(l, col)));
            return finish(&b);
        };
        expect(t, "filecol.ascii", lc("abc\ndef\n", 5), "2:2=5");
        expect(t, "filecol.ascii", lc("abc\ndef\n", 0), "1:1=0");
        expect(t, "filecol.ascii", lc("abc\ndef\n", 4), "2:1=4");
        expect(t, "filecol.tab", lc("\t\tx\n", 2), "1:3=2");
        expect(t, "filecol.utf8", lc("a\xc3\xa9\xe2\x82\xac" "b\n", 6), "1:4=6");
        expect(t, "filecol.utf8", lc("z\na\xc3\xa9\xe2\x82\xac" "b\n", 8), "2:4=8");
        expect(t, "filecol.clamp", istr(&a, filecolbyte("ab\ncd", 9)), "2");
        expect(t, "filecol.clamp", istr(&a, filecolbyte("ab\ncd", 0)), "0");
        expect(t, "filecol.clamp", istr(&a, filecolbyte("a\xc3\xa9", 3)), "3");
    }

    // Go to line: N, N:C or A-B, spaces and a leading ':' allowed, past
    // FREF_MAXLINE clamped, a reversed range swapped without its column
    expect(t, "filegoto.n", filegotostr(&a, "12"), "12-12:0");
    expect(t, "filegoto.nc", filegotostr(&a, "12:5"), "12-12:5");
    expect(t, "filegoto.range", filegotostr(&a, "10-20"), "10-20:0");
    expect(t, "filegoto.swap", filegotostr(&a, "20-10"), "10-20:0");
    expect(t, "filegoto.colon", filegotostr(&a, ":12"), "12-12:0");
    expect(t, "filegoto.space", filegotostr(&a, " 1500 - 1504 "), "1500-1504:0");
    expect(t, "filegoto.space", filegotostr(&a, " : 1500 : 7 "), "1500-1500:7");
    expect(t, "filegoto.big", filegotostr(&a, "99999999"), "10000000-10000000:0");
    expect(t, "filegoto.big", filegotostr(&a, "5:99999999"), "5-5:10000000");
    expect(t, "filegoto.zero", filegotostr(&a, "0"), "no");
    expect(t, "filegoto.zero", filegotostr(&a, "12:0"), "12-12:0");
    expect(t, "filegoto.bad", filegotostr(&a, "abc"), "no");
    expect(t, "filegoto.bad", filegotostr(&a, "1:2:3"), "no");
    expect(t, "filegoto.bad", filegotostr(&a, "-5"), "no");
    expect(t, "filegoto.bad", filegotostr(&a, "12-"), "no");
    expect(t, "filegoto.bad", filegotostr(&a, ""), "no");
    expect(t, "filegoto.bad", filegotostr(&a, "::12"), "no");
    expect(t, "filegoto.bad", filegotostr(&a, "123456789012345678901234567890123"), "no");
}

// A FileRead as "len size changed toobig err", and whether buf holds the
// file's first len bytes ("same", else "differs").
static Str filereadstr(Arena *a, FileRead r, u8 *buf, Str file)
{
    Buf b(a, 128);
    print(&b, (i64)r.len);
    putbyte(&b, ' ');
    print(&b, r.size);
    print(&b, r.changed ? Str(" changed") : Str(""));
    print(&b, r.toobig ? Str(" toobig") : Str(""));
    if (r.err.len) {
        putbyte(&b, ' ');
        print(&b, r.err);
    }
    b32 same = r.len<=file.len && Str{buf, r.len}==takehead(file, r.len);
    print(&b, same ? Str(" same") : Str(" differs"));
    return finish(&b);
}

static void test_filereadall(Test *t)
{
    Arena a  = t->perm;
    Os   *os = newos(&a, (iz)32<<20);
    Arena s  = carve(&a, (iz)16<<20);
    iz    max  = 4*(iz)FILES_PART;
    u8   *buf  = allocbytes(&a, max);
    Str   path = "/f/data.txt";

    // Across FILES_PART pieces, whole, and cut at max
    iz  n    = 2*(iz)FILES_PART + 10;
    Str file = {allocbytes(&a, n), n};
    for (iz i = 0; i < n; i++) {
        file.data[i] = (u8)('a' + i%26);
    }
    os_write(os, s, path, file);
    Str      want = concat(&a, concat(&a, istr(&a, (i64)n), " "), istr(&a, (i64)n));
    FileRead r    = filereadall(0, os, s, path, buf, max, 0);
    expect(t, "filereadall.whole", filereadstr(&a, r, buf, file), concat(&a, want, " same"));
    expect(t, "filereadall.mtime", istr(&a, r.mtime), istr(&a, os_time(os)));
    expect(t, "filereadall.readat", r.readat<=os_time(os) ? Str("before") : Str("after"),
           "before");
    r = filereadall(0, os, s, path, buf, FILES_PART, 0);
    expect(t, "filereadall.cut", filereadstr(&a, r, buf, file),
           concat(&a, concat(&a, istr(&a, (i64)FILES_PART), " "),
                  concat(&a, istr(&a, (i64)n), " same")));

    // An image past max is not read; under it, read whole
    r = filereadall(0, os, s, path, buf, FILES_PART, 1);
    expect(t, "filereadall.imagebig", filereadstr(&a, r, buf, file), "0 0 toobig same");
    r = filereadall(0, os, s, path, buf, max, 1);
    expect(t, "filereadall.image", filereadstr(&a, r, buf, file), concat(&a, want, " same"));

    // Missing, a folder
    r = filereadall(0, os, s, "/f/none.txt", buf, max, 0);
    expect(t, "filereadall.missing", r.err, "No such file");
    os_mkdirs(os, s, "/f/dir");
    r = filereadall(0, os, s, "/f/dir", buf, max, 0);
    expect(t, "filereadall.folder", r.err, "That is a folder");

    // Grown once while read: read again from the start, whole
    os->growpath  = path;
    os->growby    = "xyz";
    os->growevery = 1;
    os->growtimes = 1;
    os->growreads = 0;
    r = filereadall(0, os, s, path, buf, max, 0);
    Str grown = concat(&a, file, "xyz");
    Str gwant = concat(&a, concat(&a, istr(&a, (i64)grown.len), " "),
                       istr(&a, (i64)grown.len));
    expect(t, "filereadall.again", filereadstr(&a, r, buf, grown), concat(&a, gwant, " same"));

    // Growing as it is read again too: what came is kept, its size
    os->growtimes = 1000;
    os->growreads = 0;
    r = filereadall(0, os, s, path, buf, max, 0);
    expect(t, "filereadall.changed", r.changed ? Str("changed") : Str("unchanged"), "changed");
    expect(t, "filereadall.changedsize", istr(&a, r.size), istr(&a, (i64)r.len));
    os->growtimes = 0;
}
