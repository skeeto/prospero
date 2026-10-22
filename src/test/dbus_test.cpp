// D-Bus messages (dbus.cpp): bodies built byte for byte as the
// specification lays them out, whole messages read back, and malformed
// ones refused


static Str hexstr(Arena *a, Str s)
{
    Buf b(a, s.len*2);
    for (iz i = 0; i < s.len; i++) {
        printhex(&b, s[i], 2);
    }
    return finish(&b);
}

static void test_dbus(Test *t)
{
    Arena *a = &t->perm;

    // An a{sv} with one entry: the length counts from the padded start
    {
        Buf b(a, 64);
        DbArray o = dbarray(&b, 8);
        dbentryu32(&b, "x", 5);
        dbarrayend(&b, o);
        expect(t, "a{sv} bytes", hexstr(a, finish(&b)),
               "100000000000000001000000780001750000000005000000");
    }

    // Alignment counts from the body's start: a byte, then a u64
    {
        Buf b(a, 64);
        dbu8(&b, 7);
        dbu64(&b, 0x0102030405060708ull);
        dbsig(&b, "ai");
        expect(t, "pad bytes", hexstr(a, finish(&b)),
               "0700000000000000080706050403020102616900");
    }

    // A call, read back with its fields and parsed body
    {
        Buf b(a, 128);
        dbstr(&b, "/org/freedesktop/portal/desktop/session/1_2/t");
        DbArray o = dbarray(&b, 8);
        dbentrystr(&b, "handle_token", "tok");
        dbentrybool(&b, "multiple", 1);
        dbentryu32(&b, "types", 3);
        dbarrayend(&b, o);
        dbi32(&b, -2);
        dbf64(&b, 2.5);
        DbHead h = {};
        h.type   = DB_CALL;
        h.serial = 42;
        h.path   = "/org/freedesktop/portal/desktop";
        h.iface  = "org.freedesktop.portal.ScreenCast";
        h.member = "SelectSources";
        h.dest   = "org.freedesktop.portal.Desktop";
        h.sig    = "oa{sv}id";
        Str m = dbmessage(a, &h, finish(&b));
        expect(t, "msg size", istr(a, dbmsgsize(m)), istr(a, m.len));
        expect(t, "msg partial", istr(a, dbmsgsize(takehead(m, 15))), "0");
        DbMsg r = dbreadmsg(a, m);
        expect(t, "msg ok", istr(a, r.ok), "1");
        expect(t, "msg member", r.h.member, "SelectSources");
        expect(t, "msg iface", r.h.iface, "org.freedesktop.portal.ScreenCast");
        expect(t, "msg path", r.h.path, "/org/freedesktop/portal/desktop");
        expect(t, "msg sig", r.h.sig, "oa{sv}id");
        expect(t, "msg serial", istr(a, r.h.serial), "42");
        DbVal *v = dbparse(a, r.body, r.h.sig);
        expect(t, "body ok", istr(a, v != 0), "1");
        if (v) {
            expect(t, "body path", dbat(v, 0)->s, "/org/freedesktop/portal/desktop/session/1_2/t");
            expect(t, "body token", dbget(dbat(v, 1), "handle_token")->s, "tok");
            expect(t, "body bool", istr(a, (i64)dbget(dbat(v, 1), "multiple")->u), "1");
            expect(t, "body u32", istr(a, (i64)dbget(dbat(v, 1), "types")->u), "3");
            expect(t, "body missing", istr(a, dbget(dbat(v, 1), "nope")->type), "0");
            expect(t, "body i32", istr(a, dbat(v, 2)->i), "-2");
            expect(t, "body f64", istr(a, (i64)(dbat(v, 3)->f * 10)), "25");
            expect(t, "body past end", istr(a, dbat(v, 9)->type), "0");
        }
        expect(t, "body wrong sig", istr(a, dbparse(a, r.body, "oa{sv}i") != 0), "0");
        expect(t, "body short", istr(a, dbparse(a, cuttail(r.body, 1), r.h.sig) != 0), "0");
    }

    // A portal's Start response: a(ua{sv}) streams with (ii) tuples
    {
        Buf b(a, 256);
        dbu32(&b, 0);
        DbArray res = dbarray(&b, 8);
        dbkey(&b, "streams", "a(ua{sv})");
        DbArray ss = dbarray(&b, 8);
        dbstruct(&b);
        dbu32(&b, 63);
        DbArray props = dbarray(&b, 8);
        dbkey(&b, "position", "(ii)");
        dbstruct(&b);
        dbi32(&b, -1920);
        dbi32(&b, 0);
        dbkey(&b, "size", "(ii)");
        dbstruct(&b);
        dbi32(&b, 1920);
        dbi32(&b, 1080);
        dbarrayend(&b, props);
        dbarrayend(&b, ss);
        dbentrystr(&b, "restore_token", "abc");
        dbarrayend(&b, res);
        DbVal *v = dbparse(a, finish(&b), "ua{sv}");
        expect(t, "start ok", istr(a, v != 0), "1");
        if (v) {
            DbVal *s = dbat(dbget(dbat(v, 1), "streams"), 0);
            expect(t, "start node", istr(a, (i64)dbat(s, 0)->u), "63");
            DbVal *pos = dbget(dbat(s, 1), "position");
            expect(t, "start x", istr(a, dbat(pos, 0)->i), "-1920");
            expect(t, "start w", istr(a, dbat(dbget(dbat(s, 1), "size"), 0)->i), "1920");
            expect(t, "start token", dbget(dbat(v, 1), "restore_token")->s, "abc");
        }
    }

    // Malformed: nonzero padding, a string without its null, a bad
    // signature, an array longer than its message
    {
        u8 pad[] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        expect(t, "bad pad", istr(a, dbparse(a, Str{pad, 12}, "yt") != 0), "0");
        u8 nonull[] = {1, 0, 0, 0, 'a', 'b'};
        expect(t, "no null", istr(a, dbparse(a, Str{nonull, 6}, "s") != 0), "0");
        u8 any[] = {0, 0, 0, 0};
        expect(t, "bad sig", istr(a, dbparse(a, Str{any, 4}, "(") != 0), "0");
        expect(t, "empty struct", istr(a, dbparse(a, Str{any, 4}, "()") != 0), "0");
        u8 longarr[] = {0xff, 0, 0, 0, 1, 0, 0, 0};
        expect(t, "long array", istr(a, dbparse(a, Str{longarr, 8}, "ai") != 0), "0");
        u8 big[] = {'B', 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
        expect(t, "big endian", istr(a, dbmsgsize(Str{big, 16})), "-1");
    }

    expect(t, "request path", dbrequestpath(a, ":1.130", "t1"),
           "/org/freedesktop/portal/desktop/request/1_130/t1");
}
