// Palette tests: which sessions a query offers, and in what order.

static PalItem palsess(Str sid, Str title, i64 at)
{
    PalItem it = {};
    it.kind  = PAL_SESSION;
    it.id    = sid;
    it.sid   = sid;
    it.title = title;
    it.at    = at;
    return it;
}

// The titles offered for a query, one a line.
static Str paltitles(Arena *a, Slice<PalItem> items, Str query)
{
    Slice<i32> order = palorder(a, items, query);
    Buf b(a, 256);
    for (iz i = 0; i < order.len; i++) {
        print(&b, items[order[i]].title);
        putbyte(&b, '\n');
    }
    return finish(&b);
}

static void test_palette(Test *t)
{
    Arena a = t->perm;

    Slice<PalItem> items = {};
    PalItem it = palsess("20260101-000000-aaaa", "Fix the parser", 100);
    it.folder  = "/home/u/src/json";
    items = push(&a, items, it);
    it = palsess("20260102-000000-bbbb", "The best test", 300);
    it.folder  = "/home/u/src/parser";
    items = push(&a, items, it);
    it = palsess("20260103-000000-cccc", "Write tests", 200);
    it.group   = "Work";
    items = push(&a, items, it);
    it = palsess("20260104-000000-dddd", "Parse a subtask", 400);
    it.minor   = 1;  // a subagent
    items = push(&a, items, it);
    it = palsess("20260105-000000-eeee", "Old parser notes", 50);
    it.minor   = 1;  // archived
    it.folder  = "/home/u/src/parser";
    items = push(&a, items, it);

    // Every word must match somewhere
    expect(t, "palette.allwords", paltitles(&a, items, "fix pars"), "Fix the parser\n");
    expect(t, "palette.nomatch", paltitles(&a, items, "fix zzz"), "");

    // A title's match outranks a folder's, and a minor item ranks after
    // the rest only on a tie
    expect(t, "palette.title", paltitles(&a, items, "parser"),
           "Fix the parser\nOld parser notes\nThe best test\n");

    // The best start counts: "test" matched where it is whole, not in the
    // scattered letters of "The best", so "The best test" ties with
    // "Write tests" on the word, then goes by activity
    PalItem *best = &items.data[1];
    PalItem *wr   = &items.data[2];
    expect(t, "palette.beststart", istr(&a, palscore(best, "test") == palscore(wr, "test")), "1");
    expect(t, "palette.ties", paltitles(&a, items, "test"), "The best test\nWrite tests\n");

    // A group matches as a title does, but without the title's weight
    expect(t, "palette.group", paltitles(&a, items, "work"), "Write tests\n");

    // Folders, hosts and sids only as substrings: "jsn" is no match,
    // though a subsequence of the folder
    expect(t, "palette.folder", paltitles(&a, items, "json"), "Fix the parser\n");
    expect(t, "palette.nosubseq", paltitles(&a, items, "jsn"), "");
    expect(t, "palette.sid", paltitles(&a, items, "BBBB"), "The best test\n");
    expect(t, "palette.case", paltitles(&a, items, "FIX  Parser "), "Fix the parser\n");

    // The empty query: the sessions viewed in this run, latest first,
    // the one shown left out, then the rest by activity; no minor ones
    items.data[0].seen  = 2;
    items.data[2].seen  = 3;
    items.data[2].shown = 1;
    items.data[1].seen  = 1;
    expect(t, "palette.empty", paltitles(&a, items, ""), "Fix the parser\nThe best test\n");
    items.data[2].shown = 0;
    items.data[1].seen  = 0;
    expect(t, "palette.emptyrest", paltitles(&a, items, "  "),
           "Write tests\nFix the parser\nThe best test\n");

    // On equal times, the newest sid first
    Slice<PalItem> same = {};
    same = push(&a, same, palsess("20260101-000000-aaaa", "one", 5));
    same = push(&a, same, palsess("20260102-000000-bbbb", "two", 5));
    expect(t, "palette.sidtie", paltitles(&a, same, ""), "two\none\n");

    // The pick is kept by its identity as the order changes, else the top
    Slice<i32> order = palorder(&a, items, "");
    expect(t, "palette.keep", istr(&a, palkeep(items, order, "20260102-000000-bbbb")), "2");
    expect(t, "palette.keepgone", istr(&a, palkeep(items, order, "20260104-000000-dddd")), "0");
    order = palorder(&a, items, "test");
    expect(t, "palette.keepquery", istr(&a, palkeep(items, order, "20260103-000000-cccc")), "1");
}
