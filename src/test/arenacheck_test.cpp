// Arena aliasing checks (arenacheck.cpp): each form of the bug it is
// for, as past fixes found them, faults; the safe uses of copies around
// them do not.

// Faults counted since the last call. Each case works in a carve of its
// own, apart from the arena that its results are printed in.
static i32 arenafaults()
{
    i32 n = arenacheck.faults;
    arenacheck.faults = 0;
    return n;
}

// The callee of f(&a, a): its results first, then its scratch over them.
static Str resultsfirst(Arena *perm, Arena scratch)
{
    Str r = clone(perm, "result");
    allocbytes(&scratch, 64);
    return concat(perm, r, ", more");
}

// The same with its scratch first, which its results go over.
static Str scratchfirst(Arena *perm, Arena scratch)
{
    allocbytes(&scratch, 64);
    Str r = clone(perm, "result");
    allocbytes(&scratch, 64);
    return r;
}

// A callee using its scratch, which it was given by value.
static void scribble(Arena scratch)
{
    allocbytes(&scratch, 64);
}

static void test_arenacheck(Test *t)
{
    Arena a    = t->perm;
    b32   prev = arenacheck.record;
    arenacheck.record = 1;
    arenafaults();

    // One arena given as both results and scratch, either way round
    {
        Arena  p  = carve(&a, 1<<16);
        Arena *pp = &p;  // given twice on purpose, unseen by arenalint
        resultsfirst(pp, p);
        expect(t, "arenacheck.twice", istr(&a, arenafaults()), "1");
        Str msg = {arenacheck.last, arenacheck.lastlen};
        expect(t, "arenacheck.twice where", istr(&a, find(msg, "arenacheck_test.cpp:")>=0), "1");
        scratchfirst(pp, p);
        expect(t, "arenacheck.twice scratch first", istr(&a, arenafaults()), "1");
    }

    // Separate arenas for each, as carve gives, are fine
    {
        Arena p = carve(&a, 1<<16);
        resultsfirst(&p, carve(&p, 1<<10));
        Arena c = carve(&p, 1<<10);
        scratchfirst(&p, c);
        expect(t, "arenacheck.carved", istr(&a, arenafaults()), "0");
    }

    // Two copies of a frame, each from its base: the second allocates
    // over what the first parsed, which the first then adds to
    {
        Arena frame = carve(&a, 1<<16);
        Arena x     = frame;
        Str   parse = clone(&x, "parsed");
        Arena y     = frame;
        allocbytes(&y, 64);
        concat(&x, parse, "!");
        expect(t, "arenacheck.siblings", istr(&a, arenafaults()), "1");
    }

    // Arguments by value live to the end of the full expression, the
    // first done with when the second allocates over it
    {
        Arena scratch = carve(&a, 1<<16);
        scribble(scratch), scribble(scratch);
        expect(t, "arenacheck.arguments", istr(&a, arenafaults()), "0");
    }

    // The original allocating over a copy, then the copy again
    {
        Arena o = carve(&a, 1<<16);
        Arena c = o;
        allocbytes(&c, 64);
        allocbytes(&o, 64);
        allocbytes(&c, 8);
        expect(t, "arenacheck.under copy", istr(&a, arenafaults()), "1");
    }

    // A copy done with before the original allocates over it, in scope
    // or not, is fine
    {
        Arena o = carve(&a, 1<<16);
        {
            Arena c = o;
            allocbytes(&c, 64);
        }
        Arena c = o;
        allocbytes(&c, 64);
        allocbytes(&o, 128);
        allocbytes(&o, 8);
        expect(t, "arenacheck.done copy", istr(&a, arenafaults()), "0");
    }

    // A carve's own allocations, and its parent's past it, are fine
    {
        Arena p = carve(&a, 1<<16);
        Arena c = carve(&p, 256);
        allocbytes(&c, 100);
        allocbytes(&p, 10);
        allocbytes(&c, 10);
        Arena cc = c;
        allocbytes(&cc, 10);
        expect(t, "arenacheck.carve", istr(&a, arenafaults()), "0");
    }

    // A carve given back, then allocated over by its parent, faults
    // once the carve allocates again
    {
        Arena p    = carve(&a, 1<<16);
        byte *mark = p.beg;
        Arena c    = carve(&p, 256);
        allocbytes(&c, 100);
        p.beg = mark;
        allocbytes(&p, 64);
        allocbytes(&c, 10);
        expect(t, "arenacheck.carve given back", istr(&a, arenafaults()), "1");
    }

    // Halves split by hand
    {
        Arena h = carve(&a, 1<<13);
        Arena p = h;
        p.end   = p.beg + (1<<12);
        Arena s = h;
        s.beg   = p.end;
        for (i32 i = 0; i < 4; i++) {
            allocbytes(&p, 100);
            allocbytes(&s, 100);
        }
        expect(t, "arenacheck.halves", istr(&a, arenafaults()), "0");
    }

    // Commit and restore
    {
        Arena  o = carve(&a, 1<<16);
        Arena *p = &o;
        Arena  c = *p;
        allocbytes(&c, 100);
        *p = c;  // commit
        allocbytes(p, 10);
        Arena save = *p;
        allocbytes(p, 100);
        *p = save;  // restore
        allocbytes(p, 10);
        expect(t, "arenacheck.commit", istr(&a, arenafaults()), "0");
    }

    // A commit keeps watching what it took over
    {
        Arena o     = carve(&a, 1<<16);
        Arena stale = o;
        Arena c     = o;
        allocbytes(&c, 100);
        o = c;
        allocbytes(&stale, 50);
        allocbytes(&o, 10);
        expect(t, "arenacheck.committed", istr(&a, arenafaults()), "1");
    }

    // A hand-off into a struct from a copy that then ends, whose bytes
    // are the struct's now
    {
        struct { Arena perm; } h;
        Arena o = carve(&a, 1<<16);
        u8   *p = 0;
        {
            Arena l = o;
            p = allocbytes(&l, 100);
            h.perm = l;
        }
        allocbytes(&h.perm, 10);
        expect(t, "arenacheck.handoff", istr(&a, arenafaults()), "0");
        #if ARENA_ASAN
        expect(t, "arenacheck.handoff kept", istr(&a, __asan_address_is_poisoned(p+8)), "0");
        #else
        (void)p;
        #endif
    }

    // A copy made early and set again from the original, which has put
    // things in itself meanwhile, keeps them
    {
        Arena o = carve(&a, 1<<16);
        Arena x = o;
        Str   f = clone(&o, "kept by the original");
        x = o;
        allocbytes(&x, 8);
        x = o;
        allocbytes(&x, 8);
        expect(t, "arenacheck.recopy keeps", f, "kept by the original");
    }

    // A copy re-pointed again and again, and an arena reset
    {
        Arena scratch = carve(&a, 1<<16);
        Arena r       = scratch;
        for (i32 i = 0; i < 3; i++) {
            r = scratch;
            allocbytes(&r, 100);
        }
        r = newarena(0);
        r = scratch;
        allocbytes(&r, 10);
        expect(t, "arenacheck.recopy", istr(&a, arenafaults()), "0");
    }

    // A trim gives bytes back, which may then go to another
    {
        Arena o = carve(&a, 1<<16);
        u8   *p = allocbytes(&o, 100);
        o.beg   = (byte *)p + 10;
        Arena c = o;
        allocbytes(&c, 50);
        allocbytes(&o, 10);
        expect(t, "arenacheck.trim", istr(&a, arenafaults()), "0");
    }

    // A frame reset, with copies in each frame
    {
        Arena frame = carve(&a, 1<<16);
        byte *base  = frame.beg;
        for (i32 i = 0; i < 3; i++) {
            frame.beg = base;
            allocbytes(&frame, 32);
            Arena c = frame;
            allocbytes(&c, 64);
            allocbytes(&frame, 32);
        }
        expect(t, "arenacheck.frames", istr(&a, arenafaults()), "0");
    }

    // A Buf bound to an arena since destroyed
    {
        alignas(Arena) byte mem[sizeof(Arena)];
        Arena *dead = new (mem) Arena(carve(&a, 1<<10));
        Buf    b(dead, 16);
        dead->~Arena();
        print(&b, "more than sixteen bytes, so it grows");
        expect(t, "arenacheck.destroyed", istr(&a, arenafaults()), "1");
        Str msg = {arenacheck.last, arenacheck.lastlen};
        expect(t, "arenacheck.destroyed says", istr(&a, find(msg, "destroyed")>=0), "1");
    }

    // A reservation released forgets the arenas inside it
    {
        Os     *os = newos(&a, 1<<12);
        Region *r  = os_reserve(os, 1<<20);
        Arena   g  = newarena(r);
        Arena  *in = alloc<Arena>(&g);
        *in = carve(&g, 1<<10);
        allocbytes(in, 8);
        u32 aid = in->aid;
        expect(t, "arenacheck.registered", istr(&a, !!arenarec(aid)), "1");
        os_release(os, r);
        expect(t, "arenacheck.forgotten", istr(&a, !!arenarec(aid)), "0");
    }

    #if ARENA_ASAN
    // Memory no arena holds is poisoned: a copy's once it ends or is
    // re-pointed, what a restore gives back, and a region's fresh space
    {
        Arena base = carve(&a, 1<<16);
        u8   *p    = 0;
        {
            Arena c = base;
            p = allocbytes(&c, 64);
            expect(t, "arenacheck.poison allocated", istr(&a, __asan_address_is_poisoned(p+8)), "0");
        }
        expect(t, "arenacheck.poison ended", istr(&a, __asan_address_is_poisoned(p+8)), "1");
        u8 *q = allocbytes(&base, 64);
        expect(t, "arenacheck.poison taken again", istr(&a, q==p && !__asan_address_is_poisoned(p+8)), "1");

        Arena save = base;
        u8   *r    = allocbytes(&base, 64);
        base = save;
        expect(t, "arenacheck.poison restored", istr(&a, __asan_address_is_poisoned(r+8)), "1");

        Arena c = base;
        u8   *x = allocbytes(&c, 64);
        c = base;
        expect(t, "arenacheck.poison re-pointed", istr(&a, __asan_address_is_poisoned(x+8)), "1");

        Arena keep = base;
        u8   *k    = allocbytes(&keep, 64);
        base = keep;  // a commit: nothing given back
        expect(t, "arenacheck.poison committed", istr(&a, __asan_address_is_poisoned(k+8)), "0");

        Os     *os = newos(&a, 1<<12);
        Region *rg = os_reserve(os, 1<<22);
        Arena   g  = newarena(rg);
        u8     *f  = allocbytes(&g, 16);
        expect(t, "arenacheck.poison fresh", istr(&a, __asan_address_is_poisoned(f+16)), "1");
        expect(t, "arenacheck.poison fresh taken", istr(&a, __asan_address_is_poisoned(f)), "0");
        os_release(os, rg);
    }
    #endif

    expect(t, "arenacheck.stray", istr(&a, arenafaults()), "0");
    arenacheck.record = prev;
}
