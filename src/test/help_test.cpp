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
    }
    return 0;
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
}
