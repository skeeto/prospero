// Foundation tests: growable arenas, numbers, strings

static Str fstr(Arena *a, f64 v, i32 decimals)
{
    Buf b(a, 32);
    printfloat(&b, v, decimals);
    return finish(&b);
}

static Str parseshow(Arena *a, Str s)
{
    Float f = parsefloat(s);
    return f.ok ? fstr(a, f.val, 9) : Str("fail");
}

static Str intshow(Arena *a, Str s)
{
    Int r = parseint(s);
    return r.ok ? istr(a, r.val) : Str("fail");
}

static void test_base(Test *t)
{
    Arena a = t->perm;

    // A region-backed arena commits in grains as it grows, and a scratch
    // copy's growth is visible to the arena it was copied from.
    {
        Os     *os = newos(&a, 1<<12);
        iz      mb = (iz)1<<20;
        Region *r  = os_reserve(os, 4*mb);
        Arena   g  = newarena(r);
        expect(t, "arena.empty", istr(&a, g.end - g.beg), "0");
        alloc<u8>(&g, 100);
        expect(t, "arena.grain", istr(&a, r->committed), istr(&a, mb));
        {
            Arena tmp = g;
            allocbytes(&tmp, 2*mb);
            expect(t, "arena.scratch", istr(&a, r->committed), istr(&a, 3*mb));
        }
        allocbytes(&g, mb);
        expect(t, "arena.parent", istr(&a, g.end - r->base), istr(&a, 3*mb));
        expect(t, "arena.fits", istr(&a, fits(&g, 2*mb)), "1");
        expect(t, "arena.nofit", istr(&a, fits(&g, 4*mb)), "0");
        Arena fixed = {g.beg, g.beg+10};
        expect(t, "arena.fixed", istr(&a, fits(&fixed, 11)), "0");
        os_release(os, r);
    }

    struct { Str in, want; } ints[] = {
        {"0", "0"},
        {"-0", "0"},
        {"123", "123"},
        {"-123", "-123"},
        {"9223372036854775807", "9223372036854775807"},
        {"9223372036854775808", "fail"},
        {"-9223372036854775808", "-9223372036854775808"},
        {"-9223372036854775809", "fail"},
        {"99999999999999999999", "fail"},
        {"", "fail"},
        {"-", "fail"},
        {"+1", "fail"},
        {"1a", "fail"},
        {" 1", "fail"},
    };
    for (iz i = 0; i < countof(ints); i++) {
        expect(t, ints[i].in, intshow(&a, ints[i].in), ints[i].want);
    }

    struct { Str in, want; } floats[] = {
        {"0", "0"},
        {"-0", "0"},
        {"1.5", "1.5"},
        {"0.95", "0.95"},
        {"-12.25", "-12.25"},
        {"1e3", "1000"},
        {"1E-2", "0.01"},
        {"123.456e2", "12345.6"},
        {"2.5e+1", "25"},
        {"0.30000000000000004", "0.3"},
        {"0.000000001", "0.000000001"},
        {"1e400", "inf"},
        {"-1e400", "-inf"},
        {"1e-400", "0"},
        {"12345678901234567890", "1.23456789e19"},
        {"0.00000000000000000000000000001", "0"},
        {"", "fail"},
        {"-", "fail"},
        {"01", "fail"},
        {"1.", "fail"},
        {".5", "fail"},
        {"1e", "fail"},
        {"1e+", "fail"},
        {"+1", "fail"},
        {"1x", "fail"},
        {"0x10", "fail"},
        {" 1", "fail"},
        {"Infinity", "fail"},
        {"NaN", "fail"},
    };
    for (iz i = 0; i < countof(floats); i++) {
        expect(t, floats[i].in, parseshow(&a, floats[i].in), floats[i].want);
    }

    struct { f64 v; i32 decimals; Str want; } prints[] = {
        {0.1, 3, "0.1"},
        {2.0/3, 4, "0.6667"},
        {1234.5, 0, "1235"},
        {-1.25, 2, "-1.25"},
        {1e-7, 6, "0"},
        {0.000001, 6, "0.000001"},
        {100, 2, "100"},
        {1e18, 0, "1e18"},
        {9.99999e20, 2, "1e21"},
        {-0.0, 2, "0"},
        {__builtin_inf(), 2, "inf"},
        {-__builtin_inf(), 2, "-inf"},
        {__builtin_nan(""), 2, "nan"},
    };
    for (iz i = 0; i < countof(prints); i++) {
        Str got = fstr(&a, prints[i].v, prints[i].decimals);
        expect(t, prints[i].want, got, prints[i].want);
    }

    expect(t, "equalfold", istr(&a, equalfold("Content-Type", "content-TYPE")), "1");
    expect(t, "equalfold.diff", istr(&a, equalfold("ab", "ac")), "0");
    expect(t, "equalfold.len", istr(&a, equalfold("ab", "a")), "0");
    expect(t, "join", join(&a, "a/b", "c"), "a/b/c");
    expect(t, "join.empty", join(&a, "", "c"), "c");
    expect(t, "join.dot", join(&a, ".", "c"), "c");

    i32 points[] = {0, 0x41, 0x7f, 0x80, 0x7ff, 0x800, 0xfffd, 0xffff,
                    0x10000, 0x1f600, 0x10ffff};
    for (iz i = 0; i < countof(points); i++) {
        u8  buf[4];
        Str s   = {buf, utf8encode(buf, points[i])};
        iz  off = 0;
        i32 c   = utf8decode(s, &off);
        expect(t, "utf8.roundtrip", istr(&a, c), istr(&a, points[i]));
        expect(t, "utf8.length", istr(&a, off), istr(&a, s.len));
    }
}
