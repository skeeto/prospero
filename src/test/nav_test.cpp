// Back and forward tests: the order of steps, what a jump keeps or lets
// go, the cap, skipping dead and unchanged spots, and what is the same
// place.

static NavSpot navspot(i32 pane, i32 page)
{
    NavSpot s = {};
    s.pane = pane;
    s.page = page;  // tells Help spots apart
    return s;
}

static NavSpot navfile(Str path, i32 top)
{
    NavSpot s = {};
    s.pane = PANE_FILES;
    s.top  = top;
    copybytes(s.text, path.data, path.len);
    s.textlen = (i32)path.len;
    return s;
}

// The pages of each side, oldest first, as "back|fwd".
static Str navsides(Arena *a, Nav *n)
{
    Buf b(a, 256);
    for (i32 i = 0; i < n->nback; i++) {
        if (i) {
            putbyte(&b, ',');
        }
        print(&b, (i64)n->back[i].page);
    }
    putbyte(&b, '|');
    for (i32 i = 0; i < n->nfwd; i++) {
        if (i) {
            putbyte(&b, ',');
        }
        print(&b, (i64)n->fwd[i].page);
    }
    return finish(&b);
}

// A jump from one Help page to another.
static void navgojump(Nav *n, NavSpot *now, i32 to)
{
    navhold(n, now);
    *now = navspot(PANE_HELP, to);
}

// A step, as the GUI takes it: the page reached, or -1.
static i32 navgostep(Nav *n, NavSpot *now, i32 dir)
{
    navsettle(n, now);
    if (!navpeek(n, dir, now, 0, 0)) {
        return -1;
    }
    *now = navstep(n, dir, now);
    return now->page;
}

static b32 navalive(void *ctx, void *sess)
{
    return sess == ctx;
}

static void test_nav(Test *t)
{
    Arena a = t->perm;

    {
        Nav    *n   = alloc<Nav>(&a);
        NavSpot now = navspot(PANE_HELP, 1);
        navgojump(n, &now, 2);
        navgojump(n, &now, 3);
        // The last jump is held until settled
        expect(t, "nav.held", navsides(&a, n), "1|");
        expect(t, "nav.back", istr(&a, navgostep(n, &now, NAV_BACK)), "2");
        expect(t, "nav.sides", navsides(&a, n), "1|3");
        expect(t, "nav.back2", istr(&a, navgostep(n, &now, NAV_BACK)), "1");
        expect(t, "nav.empty", istr(&a, navgostep(n, &now, NAV_BACK)), "-1");
        expect(t, "nav.fwd", istr(&a, navgostep(n, &now, NAV_FWD)), "2");
        expect(t, "nav.fwd2", istr(&a, navgostep(n, &now, NAV_FWD)), "3");
        expect(t, "nav.end", istr(&a, navgostep(n, &now, NAV_FWD)), "-1");
        expect(t, "nav.after", navsides(&a, n), "1,2|");

        // A jump that went somewhere clears Forward
        navgostep(n, &now, NAV_BACK);
        navgojump(n, &now, 4);
        navsettle(n, &now);
        expect(t, "nav.cleared", navsides(&a, n), "1,2|");
        expect(t, "nav.new", istr(&a, navgostep(n, &now, NAV_BACK)), "2");

        // One that went nowhere keeps it
        expect(t, "nav.before", navsides(&a, n), "1|4");
        navhold(n, &now);
        navsettle(n, &now);
        expect(t, "nav.nowhere", navsides(&a, n), "1|4");
    }

    {
        // The same place twice in a row is kept once, the newer
        Nav    *n   = alloc<Nav>(&a);
        NavSpot now = navspot(PANE_HELP, 1);
        navgojump(n, &now, 2);
        navgojump(n, &now, 1);
        navgojump(n, &now, 1);  // nowhere
        navgojump(n, &now, 5);
        navsettle(n, &now);
        expect(t, "nav.dedup", navsides(&a, n), "1,2,1|");

        // Spots equal to now are skipped
        n->back[n->nback++] = now;
        expect(t, "nav.skipnow", istr(&a, navgostep(n, &now, NAV_BACK)), "1");
    }

    {
        // The cap drops the oldest
        Nav    *n   = alloc<Nav>(&a);
        NavSpot now = navspot(PANE_HELP, 0);
        for (i32 i = 1; i <= NAV_MAX+5; i++) {
            navgojump(n, &now, i);
        }
        navsettle(n, &now);
        expect(t, "nav.cap", istr(&a, n->nback), istr(&a, NAV_MAX));
        expect(t, "nav.oldest", istr(&a, n->back[0].page), "5");
        expect(t, "nav.newest", istr(&a, n->back[NAV_MAX-1].page), istr(&a, NAV_MAX+4));
    }

    {
        // A gone session's spots are dropped on the way
        Nav    *n    = alloc<Nav>(&a);
        i32     live = 0;
        i32     dead = 0;
        NavSpot now  = navspot(PANE_HELP, 1);
        now.sess = &live;
        navhold(n, &now);
        now = navspot(PANE_HELP, 2);
        now.sess = &dead;
        navhold(n, &now);
        now = navspot(PANE_HELP, 3);
        now.sess = &live;
        navsettle(n, &now);
        NavSpot *to = navpeek(n, NAV_BACK, &now, navalive, &live);
        expect(t, "nav.dead", istr(&a, to ? to->page : -1), "1");
        expect(t, "nav.dropped", istr(&a, n->nback), "1");
    }

    {
        // navwhere counts a held spot and changes nothing
        Nav    *n   = alloc<Nav>(&a);
        NavSpot now = navspot(PANE_HELP, 1);
        navgojump(n, &now, 2);
        navgojump(n, &now, 3);
        navgostep(n, &now, NAV_BACK);
        navgojump(n, &now, 4);
        NavSpot *b = navwhere(n, NAV_BACK, &now, 0, 0);
        NavSpot *f = navwhere(n, NAV_FWD, &now, 0, 0);
        expect(t, "nav.whereback", istr(&a, b ? b->page : -1), "2");
        expect(t, "nav.wherefwd", istr(&a, f ? f->page : -1), "-1");
        expect(t, "nav.unchanged", navsides(&a, n), "1|3");
        expect(t, "nav.stillheld", istr(&a, n->hold), "1");

        // A held spot that went nowhere doesn't count
        n->hold = 0;
        navhold(n, &now);
        f = navwhere(n, NAV_FWD, &now, 0, 0);
        expect(t, "nav.wherenowhere", istr(&a, f ? f->page : -1), "3");
    }

    {
        // Files compare by path, without case on Windows, and by place
        NavSpot x = navfile("C:/src/Hello.c", 10);
        NavSpot y = navfile("c:/src/hello.c", 10);
        expect(t, "nav.filecase", istr(&a, navsame(&x, &y)), "0");
        x.win = y.win = 1;
        expect(t, "nav.filewin", istr(&a, navsame(&x, &y)), "1");
        y.top = 11;
        expect(t, "nav.filetop", istr(&a, navsame(&x, &y)), "0");
        y.top = 10;
        y.band0 = y.band1 = 12;
        expect(t, "nav.fileband", istr(&a, navsame(&x, &y)), "0");

        // A transcript following its end is the same wherever it was
        NavSpot p = navspot(PANE_TRANSCRIPT, 0);
        NavSpot q = p;
        p.stick = q.stick = 1;
        p.msg = 3;
        expect(t, "nav.stuck", istr(&a, navsame(&p, &q)), "1");
        p.stick = q.stick = 0;
        expect(t, "nav.anchored", istr(&a, navsame(&p, &q)), "0");
        q.msg = 3;
        expect(t, "nav.anchor", istr(&a, navsame(&p, &q)), "1");
        q.pane = PANE_PROMPT;
        expect(t, "nav.pane", istr(&a, navsame(&p, &q)), "0");
    }
}
