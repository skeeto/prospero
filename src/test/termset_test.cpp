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

// Run in Terminal's rules (termset.cpp): when a tab may take a block,
// which one does, when a Run still has its tab, and when a new tab is
// ready for its block. Times in ms from 0, the tab live at 1000.
static void test_termrunto(Test *t)
{
    Arena a  = t->perm;
    i64   ms = 1000;

    TermState sh = {};
    sh.live   = 1;
    sh.kind   = TERMK_SH;
    sh.liveat = 1000*ms;

    // Unhooked: nothing typed, or a line entered, then a hold for busy
    TermState u = sh;
    expect(t, "avail.unhooked.new", istr(&a, termavail(&u, TERMK_SH, 2000*ms)), "0");
    expect(t, "avail.unhooked.held", istr(&a, termavail(&u, TERMK_SH, 2600*ms)), "1");
    expect(t, "avail.unhooked.ps", istr(&a, termavail(&u, TERMK_PS, 2600*ms)), "0");
    expect(t, "avail.unhooked.none", istr(&a, termavail(&u, TERMK_OTHER, 2600*ms)), "0");
    u.typed   = 1;
    u.typedat = 3000*ms;
    expect(t, "avail.unhooked.ctrlc", istr(&a, termavail(&u, TERMK_SH, 9000*ms)), "0");
    u.typedcr = 1;
    expect(t, "avail.unhooked.cr.soon", istr(&a, termavail(&u, TERMK_SH, 4000*ms)), "0");
    expect(t, "avail.unhooked.cr", istr(&a, termavail(&u, TERMK_SH, 4500*ms)), "1");
    u.busy = 1;
    expect(t, "avail.unhooked.busy", istr(&a, termavail(&u, TERMK_SH, 9000*ms)), "0");
    u.busy   = 0;
    u.queued = 1;
    expect(t, "avail.queued", istr(&a, termavail(&u, TERMK_SH, 9000*ms)), "0");

    // Hooked: by its prompt's mark, whatever the last key; without a
    // mark for HOOKWAIT, by the unhooked rule
    TermState h = sh;
    h.hooked = 1;
    expect(t, "avail.hooked.waits", istr(&a, termavail(&h, TERMK_SH, 5000*ms)), "0");
    expect(t, "avail.hooked.nomark", istr(&a, termavail(&h, TERMK_SH, 11000*ms)), "1");
    h.marked   = 1;
    h.prompted = 1;
    h.typed    = 1;
    h.typedat  = 3000*ms;  // ^C, its prompt drawn again
    expect(t, "avail.hooked.prompt", istr(&a, termavail(&h, TERMK_SH, 3001*ms)), "1");
    h.prompted = 0;        // typed since, or a C, or PS2
    expect(t, "avail.hooked.typed", istr(&a, termavail(&h, TERMK_SH, 30000*ms)), "0");
    h.live     = 0;
    h.prompted = 1;
    expect(t, "avail.dead", istr(&a, termavail(&h, TERMK_SH, 30000*ms)), "0");

    // Picking: the front one, else the right-most
    {
        TermTab  *tabs[4] = {};
        TermSet   s       = {};
        TermState sts[4];
        for (i32 i = 0; i < 4; i++) {
            tabs[i] = (TermTab *)(uz)(i + 1);
            termsetadd(&s, tabs[i]);
            sts[i] = sh;
            sts[i].hooked = sts[i].marked = 1;
        }
        termsetshow(&s, 1);
        sts[0].prompted = sts[2].prompted = 1;
        expect(t, "pick.rightmost", istr(&a, termsetpick(&s, sts, TERMK_SH, 5000*ms)), "2");
        sts[1].prompted = 1;
        expect(t, "pick.front", istr(&a, termsetpick(&s, sts, TERMK_SH, 5000*ms)), "1");
        sts[1].prompted = sts[2].prompted = 0;
        expect(t, "pick.left", istr(&a, termsetpick(&s, sts, TERMK_SH, 5000*ms)), "0");
        sts[0].prompted = 0;
        expect(t, "pick.none", istr(&a, termsetpick(&s, sts, TERMK_SH, 5000*ms)), "-1");
        sts[3].prompted = 1;
        sts[3].kind     = TERMK_PS;
        expect(t, "pick.kind", istr(&a, termsetpick(&s, sts, TERMK_SH, 5000*ms)), "-1");
        expect(t, "pick.ps", istr(&a, termsetpick(&s, sts, TERMK_PS, 5000*ms)), "3");
        // Reordered by the bar: the states follow the set's order
        TermTab *order[4] = {tabs[3], tabs[0], tabs[1], tabs[2]};
        termsetorder(&s, order, 4);
        TermState was[4] = {sts[0], sts[1], sts[2], sts[3]};
        sts[0] = was[3];
        sts[1] = was[0];
        sts[2] = was[1];
        sts[3] = was[2];
        expect(t, "pick.ordered", istr(&a, termsetpick(&s, sts, TERMK_PS, 5000*ms)), "0");
        TermSet e = {};
        expect(t, "pick.empty", istr(&a, termsetpick(&e, sts, TERMK_SH, 5000*ms)), "-1");
    }

    // A Run's hold on its tab
    {
        TermState r = sh;
        r.hooked = r.marked = 1;
        r.runkey = 7;
        r.sentat = 2000*ms;
        expect(t, "held.runs", istr(&a, termrunheld(&r, 7, 9000*ms)), "1");
        expect(t, "held.other", istr(&a, termrunheld(&r, 8, 9000*ms)), "0");
        expect(t, "held.nokey", istr(&a, termrunheld(&r, 0, 9000*ms)), "0");
        r.doneat = 9000*ms;
        expect(t, "held.ran", istr(&a, termrunheld(&r, 7, 9500*ms)), "2");
        expect(t, "held.over", istr(&a, termrunheld(&r, 7, 10000*ms)), "0");
        r.doneat  = 0;
        r.typed   = 1;
        r.typedat = 3000*ms;
        expect(t, "held.typed", istr(&a, termrunheld(&r, 7, 9000*ms)), "0");
        TermState q = sh;
        q.runkey = 7;
        q.sentat = 2000*ms;
        expect(t, "held.unhooked.soon", istr(&a, termrunheld(&q, 7, 3000*ms)), "1");
        expect(t, "held.unhooked.idle", istr(&a, termrunheld(&q, 7, 3600*ms)), "0");
        q.busy = 1;
        expect(t, "held.unhooked.busy", istr(&a, termrunheld(&q, 7, 9000*ms)), "1");
    }

    // A new tab's Run: at its first prompt, after its output pauses
    {
        TermState p = {};
        p.kind   = TERMK_SH;
        p.hooked = 1;
        expect(t, "pend.opening", istr(&a, termpendgo(&p, 0, 0, 500*ms)), "0");
        p.live   = 1;
        p.liveat = 1000*ms;
        p.outat  = 1200*ms;
        expect(t, "pend.noprompt", istr(&a, termpendgo(&p, 0, 0, 2000*ms)), "0");
        p.marked = p.prompted = 1;
        p.outat  = 2000*ms;
        expect(t, "pend.quiet", istr(&a, termpendgo(&p, 0, 0, 2050*ms)), "0");
        expect(t, "pend.go", istr(&a, termpendgo(&p, 0, 0, 2100*ms)), "1");
        expect(t, "pend.ended", istr(&a, termpendgo(&p, 1, 0, 2100*ms)), "2");
        expect(t, "pend.late", istr(&a, termpendgo(&p, 0, 0, 15000*ms)), "2");
        p.typed = 1;
        expect(t, "pend.typed", istr(&a, termpendgo(&p, 0, 0, 2100*ms)), "2");
        TermState q = {};
        q.kind   = TERMK_SH;
        q.live   = 1;
        q.liveat = 1000*ms;
        q.outat  = 1100*ms;
        expect(t, "pend.unhooked.quiet", istr(&a, termpendgo(&q, 0, 0, 1500*ms)), "0");
        expect(t, "pend.unhooked.go", istr(&a, termpendgo(&q, 0, 0, 1600*ms)), "1");
        q.busy = 1;
        expect(t, "pend.unhooked.busy", istr(&a, termpendgo(&q, 0, 0, 1600*ms)), "0");
    }

    // When to look again
    {
        TermState l = sh;
        l.hooked  = 1;
        l.typed   = 1;
        l.typedat = 3000*ms;
        l.outat   = 3200*ms;
        expect(t, "due.output", istr(&a, termlookdue(&l, 3250*ms) / ms), "3300");
        expect(t, "due.hold", istr(&a, termlookdue(&l, 3800*ms) / ms), "4500");
        expect(t, "due.hookwait", istr(&a, termlookdue(&l, 4600*ms) / ms), "11000");
        expect(t, "due.none", istr(&a, termlookdue(&l, 12000*ms)), "0");
    }
}
