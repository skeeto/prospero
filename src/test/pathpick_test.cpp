// Path completion tests: the folder and filter a typed path makes, the
// filter's parts in any case, the order of the picks, dot entries, and
// Windows' separators.

// The picks for text, as "name/ name ..." (a folder with its slash),
// each matched part in brackets.
static Str pickstr(Arena *a, Slice<PathEntry> ents, Str text, b32 win, b32 dirs)
{
    PathQuery       q = pathquery(text, win);
    Slice<PathPick> p = pathpicks(a, ents, q.filter, dirs);
    Buf             b(a, 256);
    for (iz i = 0; i < p.len; i++) {
        u8 *m = pathmarks(a, p[i].name, q.filter);
        for (iz k = 0; k < p[i].name.len; k++) {
            if (m[k] && (!k || !m[k-1])) {
                putbyte(&b, '[');
            }
            putbyte(&b, p[i].name[k]);
            if (m[k] && !m[k+1]) {
                putbyte(&b, ']');
            }
        }
        print(&b, p[i].dir ? Str("/ ") : Str(" "));
    }
    return finish(&b);
}

static Str querystr(Arena *a, Str text, b32 win)
{
    PathQuery q = pathquery(text, win);
    return concat(a, concat(a, q.dir.data ? q.dir : Str("(none)"), "|"), q.filter);
}

static void test_pathpick(Test *t)
{
    Arena a = t->perm;

    // The folder, up to the last separator, and the filter after it
    expect(t, "path.split", querystr(&a, "/home/u/src/har", 0), "/home/u/src/|har");
    expect(t, "path.slash", querystr(&a, "/home/u/", 0), "/home/u/|");
    expect(t, "path.root", querystr(&a, "/", 0), "/|");
    expect(t, "path.none", querystr(&a, "src", 0), "(none)|src");
    expect(t, "path.tilde", querystr(&a, "~/pro jects", 0), "~/|pro jects");
    expect(t, "path.backposix", querystr(&a, "/a/b\\c", 0), "/a/|b\\c");
    expect(t, "path.backwin", querystr(&a, "C:\\Users\\u\\Doc", 1), "C:\\Users\\u\\|Doc");
    expect(t, "path.mixed", querystr(&a, "C:/Users\\we", 1), "C:/Users\\|we");
    expect(t, "path.drive", querystr(&a, "C:", 1), "(none)|C:");

    // What can be listed: absolute, or under ~
    expect(t, "list.abs", istr(&a, pathlistable("/home/", 0)), "1");
    expect(t, "list.tilde", istr(&a, pathlistable("~/", 0)), "1");
    expect(t, "list.tildeback", istr(&a, pathlistable("~\\", 1)), "1");
    expect(t, "list.tildeposix", istr(&a, pathlistable("~\\", 0)), "0");
    expect(t, "list.user", istr(&a, pathlistable("~bob/", 0)), "0");
    expect(t, "list.rel", istr(&a, pathlistable("src/", 0)), "0");
    expect(t, "list.drive", istr(&a, pathlistable("C:\\", 1)), "1");
    expect(t, "list.unc", istr(&a, pathlistable("\\\\host\\share\\", 1)), "1");
    expect(t, "list.drivelinux", istr(&a, pathlistable("C:\\", 0)), "0");
    expect(t, "list.slashwin", istr(&a, pathlistable("/home/", 1)), "0");

    PathEntry items[] = {
        {"harness", 1}, {"Hare", 1}, {"share", 1}, {"chart", 1}, {"har", 1},
        {"harbor.txt", 0}, {".hard", 1}, {"notes", 1}, {"My Harness", 1},
        {"HAR", 1}, {".config", 1}, {"zeta", 1},
    };
    Slice<PathEntry> ents = {items, countof(items)};

    // Exact first (folded, then by byte), then the first part's prefix,
    // then the rest, each by name ignoring case
    expect(t, "rank.order", pickstr(&a, ents, "/s/har", 0, 1),
           "[HAR]/ [har]/ [Har]e/ [har]ness/ c[har]t/ My [Har]ness/ s[har]e/ ");
    expect(t, "rank.case", pickstr(&a, ents, "/s/HArN", 0, 1),
           "[harn]ess/ My [Harn]ess/ ");
    // Files too, after the folders of their rank, unless only folders
    expect(t, "rank.files", pickstr(&a, ents, "/s/harb", 0, 0), "[harb]or.txt ");
    expect(t, "rank.dirsonly", pickstr(&a, ents, "/s/harb", 0, 1), "");
    expect(t, "rank.filesafter", pickstr(&a, ents, "/s/ha", 0, 0),
           "[HA]R/ [ha]r/ [Ha]re/ [ha]rness/ [ha]rbor.txt c[ha]rt/ My [Ha]rness/ s[ha]re/ ");

    // Parts split on spaces, each anywhere, all needed
    expect(t, "parts.both", pickstr(&a, ents, "/s/ness my", 0, 1), "[My] Har[ness]/ ");
    expect(t, "parts.order", pickstr(&a, ents, "/s/ar h", 0, 1),
           "c[har]t/ [HAR]/ [har]/ [Har]e/ [har]ness/ My [Har]ness/ s[har]e/ ");
    expect(t, "parts.spaces", pickstr(&a, ents, "/s/  zet  ", 0, 1), "[zet]a/ ");
    expect(t, "parts.none", pickstr(&a, ents, "/s/har zz", 0, 1), "");
    expect(t, "parts.exactspace", pickstr(&a, ents, "/s/my harness", 0, 1),
           "[My] [Harness]/ ");

    // Everything with no filter, but dot entries, which a dot shows
    expect(t, "dot.hidden", pickstr(&a, ents, "/s/", 0, 1),
           "chart/ HAR/ har/ Hare/ harness/ My Harness/ notes/ share/ zeta/ ");
    expect(t, "dot.hiddenmatch", pickstr(&a, ents, "/s/hard", 0, 1), "");
    expect(t, "dot.shown", pickstr(&a, ents, "/s/.", 0, 1), "[.]config/ [.]hard/ ");
    expect(t, "dot.filter", pickstr(&a, ents, "/s/.h", 0, 1), "[.h]ard/ ");

    // Windows: either separator, the folder's kept on accepting
    expect(t, "win.back", pickstr(&a, ents, "C:\\s\\zE", 1, 1), "[ze]ta/ ");
    expect(t, "win.slash", pickstr(&a, ents, "C:/s/ze", 1, 1), "[ze]ta/ ");
    PathPick zeta = {"zeta", 1, PRANK_PREFIX};
    PathPick file = {"harbor.txt", 0, PRANK_PREFIX};
    expect(t, "accept.back", pathaccept(&a, pathquery("C:\\Users\\z", 1), zeta),
           "C:\\Users\\zeta\\");
    expect(t, "accept.mixed", pathaccept(&a, pathquery("C:\\Users/z", 1), zeta),
           "C:\\Users/zeta/");
    expect(t, "accept.posix", pathaccept(&a, pathquery("~/src/z", 0), zeta), "~/src/zeta/");
    expect(t, "accept.file", pathaccept(&a, pathquery("/s/harb", 0), file), "/s/harbor.txt");
    // On POSIX a backslash is a name's: "a\b" is listed as /x/'s
    expect(t, "posix.back", pickstr(&a, ents, "/s\\ze", 0, 1), "");
}
