// Delta tests: ops that remake the new body from the base, for random
// edits, a moved cache marker, a body unlike the base and empty ones,
// small where the bodies share most of their bytes, and ops that are
// unsound refused.

static u64 deltarand(u64 *s)
{
    *s = *s*0x5851f42d4c957f2d + 0x14057b7ef767814f;
    return *s >> 33;
}

// Random bytes, as JSON's: printable, with runs that repeat.
static Str deltabytes(Arena *a, u64 *s, iz len)
{
    Str r = Str(alloc<u8>(a, len), len);
    for (iz i = 0; i < len; i++) {
        r.data[i] = (u8)(' ' + deltarand(s)%64);
    }
    return r;
}

static b32 deltaround(Arena scratch, Str base, Str next, iz *opslen)
{
    Arena tmp = carve(&scratch, 4*(base.len + next.len) + (1<<16));
    Str   ops = deltamake(&scratch, tmp, base, next);
    Str got = deltaapply(&scratch, base, ops, next.len);
    *opslen = ops.len;
    return got.data && got==next;
}

static void test_delta(Test *t)
{
    Arena a = t->perm;
    u64   s = 1;
    iz    n = 0;

    // Random edits: inserts, deletes and replacements, at random places
    b32 ok = 1;
    for (i32 trial = 0; trial < 200; trial++) {
        Arena tmp  = a;
        iz    blen = (iz)(deltarand(&s) % 20000);
        Str   base = deltabytes(&tmp, &s, blen);
        Buf   b(&tmp, blen + 4096);
        iz    at = 0;
        for (i32 e = (i32)(deltarand(&s)%8); e >= 0 && at < base.len; e--) {
            iz to = at + (iz)(deltarand(&s) % (base.len-at+1));
            print(&b, Str(base.data+at, to-at));
            at = to;
            switch (deltarand(&s) % 3) {
            case 0:  // insert
                print(&b, deltabytes(&tmp, &s, (iz)(deltarand(&s)%300)));
                break;
            case 1:  // delete
                at += (iz)(deltarand(&s) % (base.len-at+1));
                break;
            case 2: {  // replace
                iz m = (iz)(deltarand(&s) % (base.len-at+1));
                print(&b, deltabytes(&tmp, &s, m));
                at += m;
            } break;
            }
        }
        print(&b, Str(base.data+at, base.len-at));
        ok &= deltaround(tmp, base, finish(&b), &n);
    }
    expect(t, "delta.random", ok ? Str("ok") : Str("bad"), "ok");

    // A message appended, and the cache marker moved from the last
    // message to the new one: copies but for the new message's bytes
    {
        Arena tmp   = a;
        Str   mark  = ",\"cache_control\":{\"type\":\"ephemeral\"}";
        Str   head  = deltabytes(&tmp, &s, 40000);
        Str   m1    = deltabytes(&tmp, &s, 3000);
        Str   m2    = deltabytes(&tmp, &s, 500);
        Buf   b(&tmp, 50000);
        print(&b, head);
        print(&b, m1);
        print(&b, mark);
        print(&b, "]}");
        Str   base = finish(&b);
        Buf   c(&tmp, 50000);
        print(&c, head);
        print(&c, m1);
        print(&c, m2);
        print(&c, mark);
        print(&c, "]}");
        Str next = finish(&c);
        ok = deltaround(tmp, base, next, &n);
        expect(t, "delta.marker", ok ? Str("ok") : Str("bad"), "ok");
        expect(t, "delta.marker.small", istr(&tmp, n < m2.len+64), "1");

        // Moved earlier, as Anthropic's cache markers do: the marker
        // leaves the last message for an earlier one
        Buf d(&tmp, 50000);
        print(&d, head);
        print(&d, mark);
        print(&d, m1);
        print(&d, "]}");
        next = finish(&d);
        ok = deltaround(tmp, base, next, &n);
        expect(t, "delta.moved", ok ? Str("ok") : Str("bad"), "ok");
        expect(t, "delta.moved.small", istr(&tmp, n < mark.len+64), "1");
    }

    // Unlike the base, as after a compaction, and the empty cases
    {
        Arena tmp  = a;
        Str   base = deltabytes(&tmp, &s, 5000);
        Str   next = deltabytes(&tmp, &s, 7000);
        ok = deltaround(tmp, base, next, &n);
        expect(t, "delta.unlike", ok ? Str("ok") : Str("bad"), "ok");
        expect(t, "delta.unlike.size", istr(&tmp, n <= next.len+8), "1");
        ok  = deltaround(tmp, {}, next, &n);
        ok &= deltaround(tmp, base, {}, &n);
        ok &= n == 0;
        ok &= deltaround(tmp, {}, {}, &n);
        ok &= deltaround(tmp, base, base, &n);
        ok &= n < 16;
        expect(t, "delta.empty", ok ? Str("ok") : Str("bad"), "ok");
    }

    // Unsound ops are refused
    {
        Arena tmp  = a;
        Str   base = "0123456789";
        struct {
            Str ops;
            iz  n;
        } bad[] = {
            {Str("\x15\x00"), 11},    // copy 10 from 0: fine, but n is 11
            {Str("\x17\x00"), 11},    // copy 11 from 0: past the base
            {Str("\x05\x09"), 2},     // copy 2 from 9: past the base
            {Str("\x08" "ab"), 4},    // add 4, but 2 follow
            {Str("\x00"), 0},         // an empty add
            {Str("\x01\x00"), 0},     // an empty copy
            {Str("\x05"), 2},         // a copy without its offset
            {Str("\x85"), 2},         // a varint cut short
            {Str("\x04" "ab" "\x04" "cd"), 2},  // more than n
        };
        for (i32 i = 0; i < countof(bad); i++) {
            Str got = deltaapply(&tmp, base, bad[i].ops, bad[i].n);
            expect(t, concat(&tmp, "delta.unsound ", istr(&tmp, i)),
                   istr(&tmp, got.data != 0), "0");
        }
        Str got = deltaapply(&tmp, base, Str("\x15\x00"), 10);
        expect(t, "delta.sound", got, base);
    }
}
