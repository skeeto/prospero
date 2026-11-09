// Terminal tab sets: adding, dropping and reordering, which tab shows
// after each, and the titles tabs show.

static Str termsetstr(Arena *a, TermSet *s, TermTab **names)
{
    // Each tab by its letter, the front one starred
    Buf b(a, 64);
    for (i32 i = 0; i < s->n; i++) {
        if (i == s->front) {
            print(&b, "*");
        }
        for (i32 k = 0; k < TERMTABS; k++) {
            if (names[k] == s->tabs[i]) {
                u8 c = (u8)('a' + k);
                print(&b, Str{&c, 1});
            }
        }
    }
    return finish(&b);
}

static Str termtitlestr(Arena *a, Str raw)
{
    u8 *dst = alloc<u8>(a, TERMTITLECAP);
    return {dst, termtitle(dst, raw)};
}

static void test_termset(Test *t)
{
    Arena a = t->perm;

    // Tabs are only compared, never read: any distinct pointers do
    static u8 tabs[TERMTABS+1];
    TermTab *names[TERMTABS+1];
    for (i32 i = 0; i <= TERMTABS; i++) {
        names[i] = (TermTab *)(tabs + i);
    }

    TermSet s = {};
    expect(t, "termset.empty", termsetstr(&a, &s, names), "");
    for (i32 i = 0; i < 3; i++) {
        termsetadd(&s, names[i]);
    }
    expect(t, "termset.add", termsetstr(&a, &s, names), "ab*c");
    expect(t, "termset.addshows", s.frontset ? Str("set") : Str("unset"), "set");

    // The front tab dropped: its right neighbour, else its left
    termsetshow(&s, 1);
    termsetdrop(&s, 1);
    expect(t, "termset.dropright", termsetstr(&a, &s, names), "a*c");
    termsetdrop(&s, 1);
    expect(t, "termset.dropleft", termsetstr(&a, &s, names), "*a");
    termsetdrop(&s, 0);
    expect(t, "termset.droplast", termsetstr(&a, &s, names), "");
    expect(t, "termset.dropclears", s.frontset ? Str("set") : Str("unset"), "unset");

    // A tab dropped before the front keeps the front where it is
    for (i32 i = 0; i < 4; i++) {
        termsetadd(&s, names[i]);
    }
    termsetshow(&s, 2);
    s.frontset = 0;
    termsetdrop(&s, 0);
    expect(t, "termset.dropbefore", termsetstr(&a, &s, names), "b*cd");
    expect(t, "termset.dropbeforeset", s.frontset ? Str("set") : Str("unset"), "unset");
    termsetdrop(&s, 2);
    expect(t, "termset.dropafter", termsetstr(&a, &s, names), "b*c");

    // Full at TERMTABS
    TermSet full = {};
    for (i32 i = 0; i < TERMTABS; i++) {
        termsetadd(&full, names[i]);
    }
    expect(t, "termset.full", termsetadd(&full, names[TERMTABS]) ? Str("added") : Str("refused"),
           "refused");

    // The bar's order: tabs it doesn't have keep their place after, and
    // tabs not in the set are ignored; the front follows its tab
    TermSet o = {};
    for (i32 i = 0; i < 4; i++) {
        termsetadd(&o, names[i]);
    }
    termsetshow(&o, 1);
    TermTab *order1[] = {names[3], names[1], names[0], names[2]};
    termsetorder(&o, order1, 4);
    expect(t, "termset.order", termsetstr(&a, &o, names), "d*bac");
    TermTab *order2[] = {names[7], names[2], names[1]};
    termsetorder(&o, order2, 3);
    expect(t, "termset.orderpart", termsetstr(&a, &o, names), "c*bda");

    // Scrollback
    expect(t, "termscroll.plain", termscrollparse("500")==500 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.zero", termscrollparse("0")==0 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.neg", termscrollparse("-5")==0 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.big", termscrollparse("999999999999")==100000 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.junk", termscrollparse("lots")==10000 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.bytes0", termscrollbytes(0)==0 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.bytesmin", termscrollbytes(10)==65536 ? Str("ok") : "bad", "ok");
    expect(t, "termscroll.bytes", termscrollbytes(10000)==4200000 ? Str("ok") : "bad", "ok");

    // Titles
    expect(t, "termtitle.plain", termtitlestr(&a, Str("vim main.c")), "vim main.c");
    expect(t, "termtitle.blank", termtitlestr(&a, Str("  a \t\n b  ")), "a b");
    expect(t, "termtitle.bidi", termtitlestr(&a, Str("one\xe2\x80\xaetwo")), "onetwo");
    expect(t, "termtitle.c0c1", termtitlestr(&a, Str("a\x1b[31mb\xc2\x9b" "c")), "a[31mbc");
    expect(t, "termtitle.badutf8", termtitlestr(&a, Str("a\xff" "b")), "a\xc3\xbf" "b");
    expect(t, "termtitle.hashes", termtitlestr(&a, Str("a##b###c")), "a# #b# # #c");
    expect(t, "termtitle.cap",
           termtitlestr(&a, Str("0123456789012345678901234567890123456789X")),
           "0123456789012345678901234567890123456789\xe2\x80\xa6");
    expect(t, "termtitle.fits",
           termtitlestr(&a, Str("0123456789012345678901234567890123456789")),
           "0123456789012345678901234567890123456789");
    expect(t, "termtitle.wide",
           termtitlestr(&a, Str("\xe4\xb8\xad\xe6\x96\x87")), "\xe4\xb8\xad\xe6\x96\x87");
    expect(t, "termtitle.empty", termtitlestr(&a, Str(" \xe2\x80\x8b ")), "");
}
