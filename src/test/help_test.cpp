// Help tests: each built-in page has a heading for its title, the
// titles are distinct, none the start of another (the checklist finds a
// topic by its title's start), and every help: link names a page.

static void test_help(Test *t)
{
    Arena   a    = t->perm;
    HlMemo *memo = 0;

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
