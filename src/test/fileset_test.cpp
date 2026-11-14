// Files tab sets: adding, evicting, dropping and reordering, which tab
// shows after each, the labels tabs show, a file's bytes as text, and
// tabs kept in the layout file.

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

    // Whether the order changed, for a save: not for the same order, nor
    // for a bar that lacks tabs at its end
    FileTab *order3[] = {tabs+2, tabs+1, tabs+3, tabs+0};
    expect(t, "fileset.ordersame", istr(&a, filesetorder(&o, order3, 4)), "0");
    expect(t, "fileset.ordershort", istr(&a, filesetorder(&o, order3, 2)), "0");
    FileTab *order4[] = {tabs+1, tabs+2};
    expect(t, "fileset.orderswap", istr(&a, filesetorder(&o, order4, 2)), "1");
    expect(t, "fileset.orderswapped", filesetstr(&a, &o, tabs), "*bcda");

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

// The Files section's lines as ImGui's loader hands them to the ini
// handler: split at CR and at LF, with blank lines, headers and comments
// skipped, each cut at its first '='. Then filekeepdone.
static void keepread(FileKeeps *ks, Arena *a, Str text)
{
    for (iz i = 0; i < text.len;) {
        iz e = i;
        for (; e<text.len && text[e]!='\n' && text[e]!='\r'; e++) {}
        Str line = {text.data+i, e-i};
        i = e + 1;
        if (!line.len || line[0]=='[' || line[0]==';') {
            continue;
        }
        Cut kv = cut(line, '=');
        if (kv.ok && !filekeepline(ks, a, kv.head, kv.tail)) {
            ks->bad++;
        }
    }
    filekeepdone(ks);
}

// Every record written again, as pending.
static Str keepwrite(Arena *a, FileKeeps *ks, i64 now)
{
    FileKeep **list = alloc<FileKeep *>(a, ks->n);
    for (i32 i = 0; i < ks->n; i++) {
        list[i] = &ks->keeps[i];
    }
    return filekeeptext(a, list, ks->n, ks->nowrap, now);
}

// Read, then written again.
static Str keepagain(Arena *a, Str text, i64 now)
{
    FileKeeps *ks = alloc<FileKeeps>(a);
    keepread(ks, a, text);
    return keepwrite(a, ks, now);
}

// How many times a needle is in s.
static i32 keepcount(Str s, Str needle)
{
    i32 n = 0;
    for (iz i = find(s, needle); i >= 0; n++) {
        s = cuthead(s, i+needle.len);
        i = find(s, needle);
    }
    return n;
}

// "sidNN", for many roots apart.
static Str keepsid(Arena *a, i32 i)
{
    Buf b(a, 8);
    print(&b, "sid");
    printpad(&b, i, 2);
    return finish(&b);
}

static void test_filekeep(Test *t)
{
    Arena a   = t->perm;
    i64   now = 1794480000000;
    Str   sid = "20261112-101500-abcd";

    // Escaped: control bytes, DEL, '%', ',' and a space ending it; all
    // else raw, UTF-8 or not
    struct { Str raw; Str enc; } escs[] = {
        {"a,b",           "a%2Cb"},
        {"50%",           "50%25"},
        {"x\r\ny",        "x%0D%0Ay"},
        {Str("\0"),       "%00"},
        {"\x7f",          "%7F"},
        {"trail ",        "trail%20"},
        {"\xff\xfe",      "\xff\xfe"},
        {"\xc3\xa9",      "\xc3\xa9"},
        {"[x]",           "[x]"},
        {"a=b;c|d",       "a=b;c|d"},
        {"C:\\x",         "C:\\x"},
        {" lead",         " lead"},
    };
    for (iz i = 0; i < countof(escs); i++) {
        Buf b(&a, 32);
        filekeepesc(&b, escs[i].raw);
        expect(t, concat(&a, "filekeep esc ", escs[i].enc), finish(&b), escs[i].enc);
        b32 ok  = 0;
        Str got = filekeepunesc(&a, escs[i].enc, &ok);
        expect(t, concat(&a, "filekeep unesc ", escs[i].enc),
               ok ? got : Str("!"), escs[i].raw);
    }
    Str badescs[] = {"%", "%4", "%zz", "a%g0"};
    for (iz i = 0; i < countof(badescs); i++) {
        b32 ok = 1;
        filekeepunesc(&a, badescs[i], &ok);
        expect(t, concat(&a, "filekeep unesc bad ", badescs[i]), istr(&a, ok), "0");
    }
    {
        b32 ok  = 0;
        Str got = filekeepunesc(&a, "%2c", &ok);
        expect(t, "filekeep unesc lower", ok ? got : Str("!"), ",");
    }

    // A live set as its lines, in ems at 15 pixels an em
    static FileTab tabs[16];
    for (i32 i = 0; i < countof(tabs); i++) {
        tabs[i] = {};
    }
    Str text = "[Prospero][Files]\n"
               "Wrap=0\n"
               "Root=here,u1794480000000,s20261112-101500-abcd\n"
               "Tab=m,o0,t202,p/w/files/guide.md\n"
               "Tab=o1,p/w/files/manual.md\n"
               "Tab=u,o2,b150-150,p/w/files/long.txt\n"
               "Tab=*,u,o3,t1487,y0.51,b1500-1504,p/w/files/lines.txt\n"
               "\n";
    {
        FileSet s = {};
        Str paths[] = {"/w/files/guide.md", "/w/files/manual.md", "/w/files/long.txt",
                       "/w/files/lines.txt"};
        for (i32 i = 0; i < 4; i++) {
            tabs[i].path = paths[i];
            tabs[i].wrap = 1;
            tabs[i].fit  = 1;
            filesetadd(&s, tabs+i);
        }
        tabs[0].mode  = FMODE_MARKDOWN;
        tabs[0].top   = 202;
        tabs[2].wrap  = 0;
        tabs[2].band0 = tabs[2].band1 = 150;
        tabs[3].wrap  = 0;
        tabs[3].band0 = 1500;
        tabs[3].band1 = 1504;
        tabs[3].top   = 1487;
        tabs[3].dy    = 7.65f;
        FileKeep  k  = {};
        FileKeep *kp = &k;
        filesetkeep(&s, 0, 15, &k);
        k.kind = FKEEP_HERE;
        k.sid  = sid;
        k.seen = now;
        k.live = 1;
        expect(t, "filekeep text", filekeeptext(&a, &kp, 1, 1, now), text);

        // A remote root: tabs on this computer and on its host, those on
        // any other host passed over
        FileSet r = {};
        tabs[4].path = "/w/a.txt";
        tabs[5].path = "/w/b.txt";
        tabs[5].host = 5;
        tabs[6].path = "C:/w/c.txt";
        tabs[6].host = 9;
        tabs[6].win  = 1;
        for (i32 i = 4; i < 7; i++) {
            tabs[i].wrap = tabs[i].fit = 1;
            filesetadd(&r, tabs+i);
        }
        FileKeep rk = {};
        filesetkeep(&r, 9, 15, &rk);
        rk.kind = FKEEP_REMOTE;
        rk.sid  = sid;
        rk.host = "ctr:box";
        rk.seen = now;
        rk.live = 1;
        FileKeep *both[] = {&k, &rk};
        Str       two    = filekeeptext(&a, both, 2, 0, now);
        expect(t, "filekeep text remote", cuthead(two, find(two, "Root=remote")),
               "Root=remote,u1794480000000,s20261112-101500-abcd,hctr:box\n"
               "Tab=o0,p/w/a.txt\n"
               "Tab=*,r,w,o1,pC:/w/c.txt\n"
               "\n");
        expect(t, "filekeep again remote", keepagain(&a, two, now), two);
    }

    // Read and written again, the same, with CRLF too; a path of bytes
    // that need escaping, and some that don't, byte for byte. A NUL is
    // escaped, but no path has one: decoded, it is refused
    expect(t, "filekeep again", keepagain(&a, text, now), text);
    {
        Buf b(&a, 512);
        for (iz i = 0; i < text.len; i++) {
            print(&b, text[i]=='\n' ? Str("\r\n") : Str{text.data+i, 1});
        }
        expect(t, "filekeep again crlf", keepagain(&a, finish(&b), now), text);
    }
    {
        Str       path = "/x\r\ny\t,%=;[x]\x7f\xff\xc3\xa9 ";
        FileKeep  k    = {};
        FileKeep *kp   = &k;
        k.kind = FKEEP_NONE;
        k.seen = now;
        k.n    = 1;
        k.tabs[0].path = path;
        Str        out = filekeeptext(&a, &kp, 1, 0, now);
        FileKeeps *ks  = alloc<FileKeeps>(&a);
        keepread(ks, &a, out);
        expect(t, "filekeep again bytes", ks->n==1 && ks->keeps[0].n==1 ?
               ks->keeps[0].tabs[0].path : Str("!"), path);
        expect(t, "filekeep again lines", istr(&a, keepcount(out, "\n")), "4");
        FileKeeps *kn = alloc<FileKeeps>(&a);
        keepread(kn, &a, "Root=none,u1\nTab=o0,p/x%00y\n");
        expect(t, "filekeep again nul", istr(&a, kn->bad), "1");
    }

    // Restored into an empty set: in order, the front shown, each as it
    // was, evicted by rank
    {
        Str rtext = "[Prospero][Files]\n"
                    "Root=here,u1794480000000,s20261112-101500-abcd\n"
                    "Tab=m,o2,t202,y0.51,p/w/a.md\n"
                    "Tab=o0,p/w/b.txt\n"
                    "Tab=u,n,a,o3,b150-150,p/w/c.txt\n"
                    "Tab=*,o1,x1.33,p/w/d.txt\n";
        FileKeeps *ks = alloc<FileKeeps>(&a);
        keepread(ks, &a, rtext);
        FileKeep *k     = &ks->keeps[0];
        FileTab  *made[FILES_TABS];
        i32       front = -1;
        for (i32 i = 0; i < k->n; i++) {
            tabs[i] = {};
            tabs[i].path = k->tabs[i].path;
            filekeeptab(tabs+i, &k->tabs[i]);
            made[i] = tabs+i;
            front   = k->tabs[i].flags&FKT_FRONT ? i : front;
        }
        FileSet s = {};
        expect(t, "filekeep restore took", istr(&a, filesetrestore(&s, made, k->n, front)), "4");
        expect(t, "filekeep restore", filesetstr(&a, &s, tabs), "abc*d");
        expect(t, "filekeep restore frontset", istr(&a, s.frontset), "1");
        expect(t, "filekeep restore modes",
               concat(&a, concat(&a, istr(&a, tabs[0].mode), istr(&a, tabs[1].mode)),
                      concat(&a, istr(&a, tabs[2].mode), istr(&a, tabs[3].mode))),
               "1000");
        expect(t, "filekeep restore c",
               concat(&a, concat(&a, istr(&a, tabs[2].wrap), istr(&a, tabs[2].fit)),
                      istr(&a, tabs[2].astext)),
               "001");
        expect(t, "filekeep restore band",
               concat(&a, concat(&a, istr(&a, tabs[2].band0), "-"), istr(&a, tabs[2].band1)),
               "150-150");
        expect(t, "filekeep restore top", istr(&a, tabs[0].top), "202");
        f32 dy = tabs[0].dy - 0.51f;
        expect(t, "filekeep restore dy", dy>-0.01f && dy<0.01f ? Str("near") : Str("far"),
               "near");
        f32 sx = tabs[3].scrollx - 1.33f;
        expect(t, "filekeep restore scrollx", sx>-0.01f && sx<0.01f ? Str("near") : Str("far"),
               "near");
        expect(t, "filekeep restore ems", istr(&a, tabs[0].ems && tabs[3].ems), "1");

        // Eight more: four fill the set, then each evicts by rank
        Buf gone(&a, 8);
        for (i32 i = 4; i < 12; i++) {
            tabs[i] = {};
            FileTab *ft = filesetadd(&s, tabs+i);
            if (ft) {
                putbyte(&gone, (u8)('a' + (ft - tabs)));
            }
        }
        expect(t, "filekeep restore evicts", finish(&gone), "bdac");
    }

    // Restored into a set with tabs: those it has passed over, a Windows
    // host's without case, only as many as fit, ranked below its own
    {
        FileSet s = {};
        Str     paths[] = {"/w/0", "/w/1", "/w/2", "/w/3", "/w/4", "C:/W/A.c"};
        for (i32 i = 0; i < 6; i++) {
            tabs[i] = {};
            tabs[i].path = paths[i];
            filesetadd(&s, tabs+i);
        }
        tabs[5].host = 3;
        tabs[5].win  = 1;
        filesetshow(&s, 2);
        Str cand[]  = {"/w/1", "c:/w/a.C", "/w/x", "/w/y"};
        i32 ranks[] = {2, 3, 1, 0};
        FileTab *made[4];
        for (i32 i = 0; i < 4; i++) {
            FileKeepTab kt = {};
            kt.path = cand[i];
            kt.rank = ranks[i];
            tabs[6+i] = {};
            tabs[6+i].path = cand[i];
            filekeeptab(tabs+6+i, &kt);
            made[i] = tabs+6+i;
        }
        tabs[7].host = 3;
        tabs[7].win  = 1;
        expect(t, "filekeep merge took", istr(&a, filesetrestore(&s, made, 4, 2)), "2");
        expect(t, "filekeep merge", filesetstr(&a, &s, tabs), "ab*cdefij");
        expect(t, "filekeep merge rest",
               made[0]==tabs+8 && made[1]==tabs+9 && made[2]==tabs+6 && made[3]==tabs+7 ?
               Str("ijgh") : Str("other"), "ijgh");
        FileTab *gone = filesetadd(&s, tabs+10);
        expect(t, "filekeep merge evicts", gone==tabs+9 ? Str("j") : Str("other"), "j");
    }

    // Caps: the newest 32 roots, the text's bytes, a path's bytes, the
    // tabs and roots read, and a host's bytes
    {
        FileKeep  *ks   = alloc<FileKeep>(&a, 40);
        FileKeep **list = alloc<FileKeep *>(&a, 40);
        for (i32 i = 0; i < 40; i++) {
            ks[i].kind = FKEEP_HERE;
            ks[i].sid  = keepsid(&a, i);
            ks[i].seen = now - 40 + i;
            ks[i].live = 1;
            ks[i].n    = 1;
            ks[i].tabs[0].path = "/w/a";
            list[i] = ks + i;
        }
        Str out = filekeeptext(&a, list, 40, 0, now);
        expect(t, "filekeep caps roots", istr(&a, keepcount(out, "Root=")), "32");
        expect(t, "filekeep caps newest", istr(&a, find(out, ",ssid39\n")>=0), "1");
        expect(t, "filekeep caps oldest kept", istr(&a, find(out, ",ssid08\n")>=0), "1");
        expect(t, "filekeep caps oldest", istr(&a, find(out, ",ssid07\n")>=0), "0");

        Buf pb(&a, FILES_KEEPPATH);
        putbyte(&pb, '/');
        for (i32 i = 1; i < FILES_KEEPPATH; i++) {
            putbyte(&pb, '%');
        }
        Str big = finish(&pb);
        for (i32 i = 0; i < 4; i++) {
            ks[i].n = FILES_TABS;
            for (i32 j = 0; j < FILES_TABS; j++) {
                ks[i].tabs[j].path = big;
            }
        }
        out = filekeeptext(&a, list, 4, 0, now);
        expect(t, "filekeep caps bytes", istr(&a, out.len<=FILES_KEEPBYTES), "1");
        expect(t, "filekeep caps bytes roots", istr(&a, keepcount(out, "Root=")), "2");
        expect(t, "filekeep caps bytes newest",
               istr(&a, find(out, ",ssid03\n")>=0 && find(out, ",ssid02\n")>=0), "1");
        expect(t, "filekeep caps bytes end", istr(&a, endswith(out, "\n\n")), "1");

        FileSet s = {};
        tabs[0] = {};
        tabs[0].path = concat(&a, big, "x");
        tabs[1] = {};
        tabs[1].path = big;
        filesetadd(&s, tabs+0);
        filesetadd(&s, tabs+1);
        FileKeep k = {};
        filesetkeep(&s, 0, 15, &k);
        expect(t, "filekeep caps path", istr(&a, k.n==1 && k.tabs[0].path.len==FILES_KEEPPATH),
               "1");

        Buf tb(&a, 512);
        print(&tb, "Root=none,u1\n");
        for (i32 i = 0; i < 9; i++) {
            print(&tb, "Tab=o0,p/w/");
            print(&tb, (i64)i);
            putbyte(&tb, '\n');
        }
        FileKeeps *kt = alloc<FileKeeps>(&a);
        keepread(kt, &a, finish(&tb));
        expect(t, "filekeep caps tabs", istr(&a, kt->keeps[0].n), "8");
        expect(t, "filekeep caps tabs bad", istr(&a, kt->bad), "1");

        Buf rb(&a, 2048);
        for (i32 i = 0; i < 33; i++) {
            print(&rb, "Root=here,u1,s");
            print(&rb, keepsid(&a, i));
            print(&rb, "\nTab=o0,p/w/a\n");
        }
        FileKeeps *kr = alloc<FileKeeps>(&a);
        keepread(kr, &a, finish(&rb));
        expect(t, "filekeep caps read roots", istr(&a, kr->n), "32");
        expect(t, "filekeep caps read bad", istr(&a, kr->bad), "2");

        for (i32 len = 256; len <= 257; len++) {
            Buf hb(&a, 512);
            print(&hb, "Root=remote,u1,s20261112-101500-abcd,h");
            for (i32 i = 0; i < len; i++) {
                putbyte(&hb, 'h');
            }
            print(&hb, "\nTab=r,o0,p/w/a\n");
            FileKeeps *kh = alloc<FileKeeps>(&a);
            keepread(kh, &a, finish(&hb));
            expect(t, concat(&a, "filekeep caps host ", istr(&a, len)),
                   istr(&a, kh->n), len==256 ? Str("1") : Str("0"));
        }

        // Only what reading takes is written: a live remote record with
        // a 257-byte host, an empty host or a bad sid writes no root
        Str longhost = {};
        {
            Buf hb(&a, 512);
            for (i32 i = 0; i < 257; i++) {
                putbyte(&hb, 'h');
            }
            longhost = finish(&hb);
        }
        Str hosts[] = {longhost, "", "pi"};
        Str sids[]  = {"20261112-101500-abcd", "20261112-101500-abcd", "a/b"};
        for (iz i = 0; i < countof(hosts); i++) {
            FileKeep kw = {};
            kw.kind = FKEEP_REMOTE;
            kw.sid  = sids[i];
            kw.host = hosts[i];
            kw.seen = 5;
            kw.live = 1;
            kw.n    = 1;
            kw.tabs[0].flags = FKT_FAR;
            kw.tabs[0].path  = "/w/a";
            FileKeep *kp = &kw;
            expect(t, concat(&a, "filekeep caps write ", istr(&a, i)),
                   filekeeptext(&a, &kp, 1, 0, 5), "");
        }
    }

    // A root without a valid u is pending of unknown age: stamped at its
    // first save, kept FILES_KEEPAGE from then, not dropped at once
    {
        i64 now = 1794480000000;
        Str in  = "Root=remote,s20261112-101500-abcd,hpi\n"
                  "Tab=r,o0,p/w/a\n"
                  "Root=here,ux,s20261112-101500-beef\n"
                  "Tab=o0,p/w/b\n";
        Str want = "[Prospero][Files]\n"
                   "Root=remote,u1794480000000,s20261112-101500-abcd,hpi\n"
                   "Tab=r,o0,p/w/a\n"
                   "Root=here,u1794480000000,s20261112-101500-beef\n"
                   "Tab=o0,p/w/b\n"
                   "\n";
        Str out = keepagain(&a, in, now);
        expect(t, "filekeep unseen", out, want);
        expect(t, "filekeep unseen kept", keepagain(&a, out, now+FILES_KEEPAGE), want);
        expect(t, "filekeep unseen aged", keepagain(&a, out, now+FILES_KEEPAGE+1), "");
    }

    // Tolerance: what is ignored, the tab kept, and what is refused and
    // counted, the next good root read all the same
    {
        Str in = "Tab=o0,p/w/z\n"
                 "Root=here,u1,s20261112-101500-abcd\n"
                 "Tab=q7,z,o9,t-1,p/w/a\n"
                 "Foo=1\n"
                 "Tab=tq,y1e9,b5-3,p/w/b\n"
                 "Tab=o1\n"
                 "Tab=o1,p/w/%zz\n"
                 "Tab=o1,pw/rel\n"
                 "Root=elsewhere,u1,s20261112-101500-abcd\n"
                 "Tab=o0,p/w/c\n"
                 "Root=here,u1\n"
                 "Tab=o0,p/w/d\n"
                 "Root=here,u1,sa/b\n"
                 "Root=remote,u1,s20261112-101500-abcd\n"
                 "Tab=r,o0,p/w/e\n"
                 "Root=here,u1,s20261112-101500-beef\n"
                 "Tab=*,o0,p/w/f\n";
        FileKeeps *ks = alloc<FileKeeps>(&a);
        keepread(ks, &a, in);
        expect(t, "filekeep tolerance bad", istr(&a, ks->bad), "11");
        expect(t, "filekeep tolerance", keepwrite(&a, ks, 1),
               "[Prospero][Files]\n"
               "Root=here,u1,s20261112-101500-abcd\n"
               "Tab=o0,p/w/a\n"
               "Tab=o1,p/w/b\n"
               "Root=here,u1,s20261112-101500-beef\n"
               "Tab=*,o0,p/w/f\n"
               "\n");

        // Absolute either way, a drive's or a share's
        Str abs[] = {"/a", "C:/a", "c:\\a", "\\\\srv\\s", "//srv/s", "\\/x"};
        Str rel[] = {"a", "C:a", "1:/a", "\\a", ""};
        for (iz i = 0; i < countof(abs); i++) {
            expect(t, concat(&a, "filekeep abs ", abs[i]), istr(&a, filekeepabs(abs[i])), "1");
        }
        for (iz i = 0; i < countof(rel); i++) {
            expect(t, concat(&a, "filekeep rel ", rel[i]), istr(&a, filekeepabs(rel[i])), "0");
        }
    }

    // Duplicates: a root's key as one before it, two none roots, two
    // fronts; the same path twice stays, as a local and a remote tab may
    // have it, and a true duplicate is not restored
    {
        Str in = "Root=here,u5,s20261112-101500-abcd\n"
                 "Tab=*,o0,p/w/a\n"
                 "Tab=*,o1,p/w/b\n"
                 "Tab=o2,p/w/a\n"
                 "Tab=r,o3,p/w/a\n"
                 "Root=here,u9,s20261112-101500-abcd\n"
                 "Tab=o0,p/w/z\n"
                 "Root=none,u5\n"
                 "Tab=o0,p/w/n1\n"
                 "Root=none,u5\n"
                 "Tab=o0,p/w/n2\n";
        FileKeeps *ks = alloc<FileKeeps>(&a);
        keepread(ks, &a, in);
        expect(t, "filekeep dups", keepwrite(&a, ks, 1),
               "[Prospero][Files]\n"
               "Root=here,u5,s20261112-101500-abcd\n"
               "Tab=*,o0,p/w/a\n"
               "Tab=o1,p/w/b\n"
               "Tab=o2,p/w/a\n"
               "Tab=r,o3,p/w/a\n"
               "Root=none,u5\n"
               "Tab=o0,p/w/n1\n"
               "\n");
        FileKeep *k = &ks->keeps[0];
        FileTab  *made[FILES_TABS];
        for (i32 i = 0; i < k->n; i++) {
            tabs[i] = {};
            tabs[i].path = k->tabs[i].path;
            tabs[i].host = k->tabs[i].flags&FKT_FAR ? 7 : 0;
            filekeeptab(tabs+i, &k->tabs[i]);
            made[i] = tabs+i;
        }
        FileSet s = {};
        expect(t, "filekeep dups took", istr(&a, filesetrestore(&s, made, k->n, 0)), "3");
        expect(t, "filekeep dups restore", filesetstr(&a, &s, tabs), "*abd");
        expect(t, "filekeep dups rest", made[3]==tabs+2 ? Str("c") : Str("other"), "c");
    }

    // Age: a pending root unseen 90 days is dropped, a live one never,
    // and a stamp in the future kept
    {
        i64 day = 24*60*60*1000LL;
        struct { i64 seen; b32 live; Str want; } ages[] = {
            {now - 91*day,  0, "0"},
            {now - 89*day,  0, "1"},
            {now - 200*day, 1, "1"},
            {now + day,     0, "1"},
        };
        for (iz i = 0; i < countof(ages); i++) {
            FileKeep  k  = {};
            FileKeep *kp = &k;
            k.kind = FKEEP_NONE;
            k.seen = ages[i].seen;
            k.live = ages[i].live;
            k.n    = 1;
            k.tabs[0].path = "/w/a";
            Str out = filekeeptext(&a, &kp, 1, 0, now);
            expect(t, concat(&a, "filekeep age ", istr(&a, (i64)i)),
                   istr(&a, keepcount(out, "Root=")), ages[i].want);
        }
    }

    // Ems: as restored, written as they are; else by the em
    {
        FileSet s = {};
        tabs[0] = {};
        tabs[0].path = "/w/a";
        tabs[0].dy   = 0.51f;
        tabs[0].ems  = 1;
        tabs[1] = {};
        tabs[1].path    = "/w/b";
        tabs[1].dy      = 7.5f;
        tabs[1].scrollx = 20;
        filesetadd(&s, tabs+0);
        filesetadd(&s, tabs+1);
        FileKeep  k  = {};
        FileKeep *kp = &k;
        filesetkeep(&s, 0, 15, &k);
        k.kind = FKEEP_NONE;
        k.seen = now;
        k.live = 1;
        Str out = filekeeptext(&a, &kp, 1, 0, now);
        expect(t, "filekeep ems", out,
               "[Prospero][Files]\n"
               "Root=none,u1794480000000\n"
               "Tab=u,n,o0,y0.51,p/w/a\n"
               "Tab=*,u,n,o1,y0.5,x1.33,p/w/b\n"
               "\n");
    }

    // Wrap: only 0 turns it off, and is written alone with no roots
    {
        struct { Str in; Str want; } wraps[] = {
            {"Wrap=0\n", "[Prospero][Files]\nWrap=0\n\n"},
            {"Wrap=1\n", ""},
            {"Wrap=x\n", ""},
            {"",         ""},
        };
        for (iz i = 0; i < countof(wraps); i++) {
            expect(t, concat(&a, "filekeep wrap ", wraps[i].in),
                   keepagain(&a, wraps[i].in, now), wraps[i].want);
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
