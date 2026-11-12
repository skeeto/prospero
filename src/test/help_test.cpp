// Help tests: each built-in page has a heading for its title, the
// titles are distinct, none the start of another (the checklist finds a
// topic by its title's start), its sections are told apart by their
// links, and every link in the pages and the built-in skill goes
// somewhere: a page and its section, a setting, a pane, or the web.

// Whether a link goes somewhere that exists.
static b32 helpgoes(Arena a, Str link)
{
    Dest d = helpdest(&a, link);
    switch (d.kind) {
    case DEST_HELP:
        return d.page>=0 && (!d.section.data || helpsection(&a, d.page, d.section).len);
    case DEST_SETTINGS:
        return d.whole || d.diag || (d.known && (!d.cfgkey.len || cfgkey(d.cfgkind, d.cfgkey)));
    case DEST_VIEW:
        return d.pane >= 0;
    case DEST_WEB:
        return 1;
    case DEST_FILE:
        return 0;  // a Help page has no files of its own
    }
    return 0;
}

// A link's destination as a line: its kind, then for a file the kind of
// reference, its path, lines and section, or its heading in the
// document ("here").
static Str deststr(Arena *a, Str link)
{
    Dest d = helpdest(a, link);
    Buf  b(a, 128);
    print(&b, (i64)d.kind);
    if (d.kind == DEST_FILE && d.here) {
        print(&b, " here ");
        print(&b, d.section);
    } else if (d.kind == DEST_FILE) {
        FileRef *r = &d.file;
        putbyte(&b, ' ');
        print(&b, (i64)r->kind);
        putbyte(&b, ' ');
        print(&b, r->path);
        putbyte(&b, ' ');
        print(&b, (i64)r->line0);
        putbyte(&b, '-');
        print(&b, (i64)r->line1);
        if (r->section.len) {
            putbyte(&b, '#');
            print(&b, r->section);
        }
    }
    return finish(&b);
}

static void test_help(Test *t)
{
    Arena   a    = t->perm;
    HlMemo *memo = 0;

    // Every link of the pages and the skill
    {
        Buf bad(&a, 256);
        for (i32 i = 0; i <= HELP_COUNT; i++) {
            Str  md = i<HELP_COUNT ? helptext(i) : helpskillmd();
            Doc *d  = parsedoc(&a, md, &memo);
            for (iz k = 0; k < d->links.len; k++) {
                if (!helpgoes(a, d->links[k])) {
                    print(&bad, d->links[k]);
                    putbyte(&bad, ' ');
                }
            }
            Slice<HelpHead> heads = helpheads(&a, md);
            for (iz k = 0; i<HELP_COUNT && k < heads.len; k++) {
                for (iz j = 0; j < k; j++) {
                    if (helpsame(heads[j].text, heads[k].text)) {
                        print(&bad, helpname(i));
                        print(&bad, ":same:");
                        print(&bad, heads[k].text);
                        putbyte(&bad, ' ');
                    }
                }
            }
        }
        expect(t, "help.links", finish(&bad), "");
        expect(t, "help.goes.section", istr(&a, helpgoes(a, "help:config#settings-worth-knowing")), "1");
        expect(t, "help.goes.nosection", istr(&a, helpgoes(a, "help:config#nope")), "0");
        expect(t, "help.goes.key", istr(&a, helpgoes(a, "settings:ui#nope")), "0");
        expect(t, "help.goes.mailto", istr(&a, helpgoes(a, "mailto:x@example.com")), "0");
        expect(t, "help.goes.file", istr(&a, helpgoes(a, "src/engine.cpp:42")), "0");
    }

    // Links to files: the known schemes first, then fileref; another
    // scheme is still nothing (DEST_NONE), and a reference that names no
    // file (BAD, NET) is a file's link that opens nothing
    {
        struct { Str link, want; } cases[] = {
            {"src/engine.cpp:42", "5 0 src/engine.cpp 42-42"},
            {"cite/hello.c:40-45", "5 0 cite/hello.c 40-45"},
            {"cite/hello.c#L20-L22", "5 0 cite/hello.c 20-22"},
            {"notes.md#install", "5 0 notes.md 0-0#install"},
            {"file:///tmp/a.c", "5 0 /tmp/a.c 0-0"},
            {"File:///tmp/a.c:7", "5 0 /tmp/a.c 7-7"},
            {"file://host/x.c", "5 1  0-0"},
            {"README:10", "5 0 README 10-10"},
            {"C:12", "5 2  0-0"},
            {"#install", "5 here install"},
            {"#Getting%20started", "5 here Getting started"},
            {"#", "0"},
            {"mailto:x@example.com", "0"},
            {"javascript:alert(1)", "0"},
            {"help:config", "1"},
            {"view:files", "3"},
            {"https://example.com/a.c", "4"},
            {"", "0"},
        };
        for (iz i = 0; i < countof(cases); i++) {
            expect(t, concat(&a, "help.dest.", cases[i].link),
                   deststr(&a, cases[i].link), cases[i].want);
        }
    }

    Buf bad(&a, 256);
    for (i32 i = 0; i < HELP_COUNT; i++) {
        Str md    = helptext(i);
        Str title = helptitle(i);
        if (md.len<100 || !title.len) {
            print(&bad, helpname(i));
            print(&bad, ":untitled ");
        }
        for (i32 j = 0; j < HELP_COUNT; j++) {
            if (j!=i && startswith(helptitle(j), title)) {
                print(&bad, helpname(i));
                print(&bad, ":prefix ");
            }
        }
        Doc *d = parsedoc(&a, md, &memo);
        for (iz k = 0; k < d->links.len; k++) {
            Str link = d->links[k];
            if (startswith(link, "help:") && helplink(link)<0) {
                print(&bad, link);
                putbyte(&bad, ' ');
            }
        }
    }
    expect(t, "help.pages", finish(&bad), "");

    expect(t, "help.title", helptitle(HELP_START), "Getting started");
    expect(t, "help.link", istr(&a, helplink("help:plan")), istr(&a, HELP_PLAN));
    expect(t, "help.link.none", istr(&a, helplink("help:nope")), "-1");
    expect(t, "help.link.other", istr(&a, helplink("https://example.com/")), "-1");
    expect(t, "help.named", istr(&a, helpnamed("trouble")), istr(&a, HELP_TROUBLE));

    // The link under an offset, and none beside it
    Doc *d = parsedoc(&a, "See [plans](help:plan) here.", &memo);
    expect(t, "help.at.in", doclinkat(d, 6), "help:plan");
    expect(t, "help.at.out", doclinkat(d, 0), "");
    expect(t, "help.at.after", doclinkat(d, 24), "");

    // File links as the system prompt asks for them (systemprompt's
    // filelinks): any label, <...> around a path with spaces, which
    // otherwise is no link, and a backslash before punctuation dropped
    struct { Str name, src, want; } links[] = {
        {"help.at.label", "[:88](src/engine.cpp:88)", "src/engine.cpp:88"},
        {"help.at.angle", "[x](<My Files/a.c:3-5>)", "My Files/a.c:3-5"},
        {"help.at.space", "[x](My Files/a.c:3)", ""},
        {"help.at.slash", "[x](src\\_a.c)", "src_a.c"},
    };
    for (iz i = 0; i < countof(links); i++) {
        expect(t, links[i].name, doclinkat(parsedoc(&a, links[i].src, &memo), 1), links[i].want);
    }

    // The search: a heading first, its section's line; every word, any
    // case; a page's opening by its title; nothing for nonsense
    {
        Slice<HelpHit> h = helpsearch(&a, "  approving  ", 50);
        expect(t, "help.find.head", h.len ? h[0].head : Str("none"), "Approving");
        expect(t, "help.find.page", istr(&a, h.len ? h[0].page : -1), istr(&a, HELP_PLAN));
        expect(t, "help.find.rank", istr(&a, h.len ? h[0].rank : 0), "1");

        h = helpsearch(&a, "APPROVE PLAN keep", 50);
        b32 all = h.len > 0;
        for (iz i = 0; i < h.len; i++) {
            Str s = helptext(h[i].page);
            all &= findfold(s, "approve")>=0 && findfold(s, "keep")>=0;
        }
        expect(t, "help.find.words", istr(&a, all), "1");
        b32 card = 0;
        for (iz i = 0; i < h.len; i++) {
            card |= h[i].page==HELP_PLAN && h[i].head=="Approving" &&
                    findfold(h[i].snippet, "approve")>=0;
        }
        expect(t, "help.find.snippet", istr(&a, card), "1");

        h = helpsearch(&a, "keyboard", 50);
        b32 top = 0;
        for (iz i = 0; i < h.len; i++) {
            top |= h[i].page==HELP_KEYS && !h[i].head.len && h[i].rank==3;
        }
        expect(t, "help.find.title", istr(&a, top), "1");

        h = helpsearch(&a, "zqxjv", 50);
        expect(t, "help.find.none", istr(&a, h.len), "0");
        h = helpsearch(&a, "   ", 50);
        expect(t, "help.find.empty", istr(&a, h.len), "0");
        h = helpsearch(&a, "the", 3);
        expect(t, "help.find.max", istr(&a, h.len), "3");

        // Ranked, best first, the pages in order within a rank
        h = helpsearch(&a, "session", 50);
        b32 order = 1;
        for (iz i = 1; i < h.len; i++) {
            order &= h[i-1].rank<h[i].rank ||
                     (h[i-1].rank==h[i].rank && h[i-1].page<=h[i].page);
        }
        expect(t, "help.find.order", istr(&a, order), "1");
    }
}
