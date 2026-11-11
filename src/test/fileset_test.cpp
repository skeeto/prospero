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
}
