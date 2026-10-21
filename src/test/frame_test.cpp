// FrameIn tests: the header codec at every split point, rejected headers,
// and two links over in-memory streams that break, lose bytes, deliver
// twice, restart, and run into their limits.

// Encode one frame.
static Str frameof(Arena *a, u8 type, u32 id, u32 ack, Str payload)
{
    u8 *p = allocbytes(a, FRAME_HEAD+payload.len);
    framehead(p, type, id, ack, payload.len);
    copybytes(p+FRAME_HEAD, payload.data, payload.len);
    return {p, FRAME_HEAD+payload.len};
}

// Decode a stream fed in the given pieces, rendering each frame as
// "type/id/ack:payload;", then "!" if it broke.
static Str frameshow(Arena *a, FrameDec *d, Slice<Str> pieces)
{
    Buf b(a, 256);
    for (iz i = 0; i < pieces.len; i++) {
        Str in = pieces[i];
        for (FrameIn f; (f = framenext(d, &in)).ok;) {
            print(&b, (i64)f.type);
            putbyte(&b, '/');
            print(&b, (i64)f.id);
            putbyte(&b, '/');
            print(&b, (i64)f.ack);
            putbyte(&b, ':');
            print(&b, f.payload);
            putbyte(&b, ';');
        }
        if (d->bad) {
            putbyte(&b, '!');
            break;
        }
        if (in.len) {
            print(&b, "(left)");  // consumed only as frames ended
        }
    }
    return finish(&b);
}

// Two links joined by streams of bytes in flight. Each side delivers
// its payloads as lines, and counts the other events.
struct FrameSide {
    Link     link;
    FrameDec dec;
    Buf      inflight;  // written by this side, not yet received
    Buf      got;       // payloads delivered, each with a newline
    i32      events[LINK_BROKEN+1];
};

static void sidesinit(Arena *a, FrameSide *s, u64 token, iz keepmax, i32 keepcount)
{
    *s = {};
    s->link     = newlink(a, token, keepmax, keepcount);
    s->dec      = newframedec(a, 1<<10);
    s->inflight = Buf(a, 1<<12);
    s->got      = Buf(a, 1<<12);
}

// A fresh stream between the two.
static void sidesconnect(FrameSide *x, FrameSide *y, i64 now)
{
    FrameSide *s[] = {x, y};
    for (i32 i = 0; i < 2; i++) {
        s[i]->dec.len  = 0;
        s[i]->dec.bad  = 0;
        s[i]->inflight.len = 0;
        linkconnect(&s[i]->link, now);
    }
}

// Write what each side has due into its stream.
static void sidesflush(Arena *a, FrameSide *x, FrameSide *y, i64 now)
{
    FrameSide *s[] = {x, y};
    for (i32 i = 0; i < 2; i++) {
        Arena t = *a;
        print(&s[i]->inflight, linkout(&s[i]->link, &t, now));
    }
}

// Deliver up to max bytes of what from wrote into to.
static void sidesdeliver(FrameSide *from, FrameSide *to, iz max, i64 now)
{
    Str in = {from->inflight.data, from->inflight.len<max ? from->inflight.len : max};
    iz  n  = in.len;
    for (FrameIn f; (f = framenext(&to->dec, &in)).ok;) {
        i32 ev = linkframe(&to->link, &f, now);
        to->events[ev]++;
        if (ev == LINK_DATA) {
            print(&to->got, f.payload);
            putbyte(&to->got, '\n');
        }
    }
    if (to->dec.bad) {
        to->events[LINK_BROKEN]++;
    }
    for (iz i = n; i < from->inflight.len; i++) {
        from->inflight.data[i-n] = from->inflight.data[i];
    }
    from->inflight.len -= n;
}

// Exchange until both streams are empty, in pieces of at most step bytes.
static void sidesrun(Arena *a, FrameSide *x, FrameSide *y, iz step, i64 now)
{
    for (i32 round = 0; round < 1000; round++) {
        sidesflush(a, x, y, now);
        if (!x->inflight.len && !y->inflight.len) {
            return;
        }
        sidesdeliver(x, y, step, now);
        sidesdeliver(y, x, step, now);
    }
}

static Str numlines(Arena *a, i32 from, i32 to)
{
    Buf b(a, 64);
    for (i32 i = from; i <= to; i++) {
        print(&b, "line ");
        print(&b, (i64)i);
        putbyte(&b, '\n');
    }
    return finish(&b);
}

static b32 sendlines(Arena *a, Link *l, i32 from, i32 to)
{
    for (i32 i = from; i <= to; i++) {
        Arena t = *a;
        Buf   b(&t, 32);
        print(&b, "line ");
        print(&b, (i64)i);
        if (!linksend(l, finish(&b))) {
            return 0;
        }
    }
    return 1;
}

static void test_frame(Test *t)
{
    Arena a = t->perm;

    // Headers: big-endian fields, every type round trips
    {
        Str f = frameof(&a, FRAME_REGULAR, 0x01020304, 0x0a0b0c0d, "hi");
        u8 want[] = {1, 1,2,3,4, 10,11,12,13, 0,0,0,2, 'h','i'};
        expect(t, "frame.bytes", f, {want, countof(want)});

        Buf b(&a, 256);
        print(&b, frameof(&a, FRAME_REGULAR, 1, 0, "{\"cmd\":\"hello\"}\n"));
        print(&b, frameof(&a, FRAME_ACK, 0, 7, ""));
        print(&b, frameof(&a, FRAME_KEEPALIVE, 0, 7, ""));
        print(&b, frameof(&a, FRAME_REPLAY, 3, 7, "12345678"));
        print(&b, frameof(&a, FRAME_REGULAR, 2, 7, ""));
        print(&b, frameof(&a, FRAME_DISCONNECT, 0, 8, "bye"));
        Str all  = finish(&b);
        Str want2 = "1/1/0:{\"cmd\":\"hello\"}\n;3/0/7:;9/0/7:;6/3/7:12345678;"
                    "1/2/7:;5/0/8:bye;";

        Slice<Str> one = {};
        one = push(&a, one, all);
        FrameDec d = newframedec(&a, 64);
        expect(t, "frame.whole", frameshow(&a, &d, one), want2);

        // Split once at every point, then a byte at a time
        b32 same = 1;
        for (iz i = 0; i <= all.len; i++) {
            Arena tmp = a;
            Slice<Str> two = {};
            two = push(&tmp, two, takehead(all, i));
            two = push(&tmp, two, cuthead(all, i));
            FrameDec dd = newframedec(&tmp, 64);
            Str got = frameshow(&tmp, &dd, two);
            if (!(got == want2)) {
                expect(t, "frame.split", got, want2);
                same = 0;
                break;
            }
        }
        expect(t, "frame.splits", istr(&a, same), "1");
        Slice<Str> bytes = {};
        for (iz i = 0; i < all.len; i++) {
            bytes = push(&a, bytes, Str{all.data+i, 1});
        }
        d = newframedec(&a, 64);
        expect(t, "frame.bytewise", frameshow(&a, &d, bytes), want2);
    }

    // Malformed headers break the stream, and it stays broken
    {
        struct { Str name; u8 type; iz len; } bad[] = {
            {"frame.type0",     0,                 0},
            {"frame.type2",     2,                 0},
            {"frame.type255",   255,               0},
            {"frame.acklen",    FRAME_ACK,         1},
            {"frame.keeplen",   FRAME_KEEPALIVE,   4},
            {"frame.replay7",   FRAME_REPLAY,      7},
            {"frame.replay9",   FRAME_REPLAY,      9},
            {"frame.byelong",   FRAME_DISCONNECT,  FRAME_CTLMAX+1},
            {"frame.toolong",   FRAME_REGULAR,     65},
        };
        for (iz i = 0; i < countof(bad); i++) {
            Arena tmp = a;
            u8 h[FRAME_HEAD];
            framehead(h, bad[i].type, 1, 0, bad[i].len);
            Slice<Str> in = {};
            in = push(&tmp, in, frameof(&tmp, FRAME_ACK, 0, 1, ""));
            in = push(&tmp, in, Str{h, FRAME_HEAD});
            in = push(&tmp, in, frameof(&tmp, FRAME_ACK, 0, 2, ""));
            FrameDec d = newframedec(&tmp, 64);
            expect(t, bad[i].name, frameshow(&tmp, &d, in), "3/0/1:;!");
        }
        // A huge length is refused at its header, before any payload
        Arena tmp = a;
        u8 h[FRAME_HEAD] = {1, 0,0,0,1, 0,0,0,0, 0xff,0xff,0xff,0xff};
        Slice<Str> in = {};
        in = push(&tmp, in, Str{h, 12});
        in = push(&tmp, in, Str{h+12, 1});
        FrameDec d = newframedec(&tmp, FRAME_MAX);
        expect(t, "frame.huge", frameshow(&tmp, &d, in), "!");
        // The largest frame passes
        Str big  = {allocbytes(&tmp, 64), 64};
        for (iz i = 0; i < 64; i++) big[i] = 'x';
        in = {};
        in = push(&tmp, in, frameof(&tmp, FRAME_REGULAR, 9, 0, big));
        d = newframedec(&tmp, 64);
        Str want = concat(&tmp, concat(&tmp, "1/9/0:", big), ";");
        expect(t, "frame.max", frameshow(&tmp, &d, in), want);
    }

    // Two links: lines both ways, in pieces of every size
    for (iz step = 1; step <= 40; step += 13) {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        sidesconnect(x, y, 0);
        sendlines(&tmp, &x->link, 1, 20);
        sendlines(&tmp, &y->link, 1, 5);
        sidesrun(&tmp, x, y, step, 0);
        expect(t, "link.xtoy", finish(&y->got), numlines(&tmp, 1, 20));
        expect(t, "link.ytox", finish(&x->got), numlines(&tmp, 1, 5));
        expect(t, "link.ready", istr(&tmp, x->events[LINK_READY]*10 + y->events[LINK_READY]), "11");
        expect(t, "link.broken", istr(&tmp, x->events[LINK_BROKEN] + y->events[LINK_BROKEN]), "0");
        // Everything was acknowledged once the replies went back
        sidesrun(&tmp, x, y, step, LINK_ACKTIME);
        linktick(&x->link, LINK_ACKTIME);
        linktick(&y->link, LINK_ACKTIME);
        sidesrun(&tmp, x, y, step, LINK_ACKTIME);
        expect(t, "link.kept", istr(&tmp, x->link.qcount*100 + y->link.qcount), "0");
    }

    // A stream breaks with bytes in flight, at every point: the next
    // stream delivers the rest, once each, in order
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        sidesconnect(x, y, 0);
        sidesrun(&tmp, x, y, 1000, 0);
        sendlines(&tmp, &x->link, 1, 10);
        sidesflush(&tmp, x, y, 0);
        iz total = x->inflight.len;
        b32 ok = 1;
        for (iz cutat = 0; cutat <= total && ok; cutat++) {
            Arena u = tmp;
            FrameSide *p = alloc<FrameSide>(&u);
            FrameSide *q = alloc<FrameSide>(&u);
            *p = *x;
            *q = *y;
            // Each side's buffers of its own, as this run changes them
            Buf in = Buf(&u, 1<<12);
            print(&in, finish(&x->inflight));
            p->inflight = in;
            p->got = Buf(&u, 1<<12);
            q->got = Buf(&u, 1<<12);
            q->inflight = Buf(&u, 1<<12);
            u8 *pq = allocbytes(&u, p->link.keepmax);
            copybytes(pq, x->link.q, p->link.keepmax);
            p->link.q = pq;
            u8 *qq = allocbytes(&u, q->link.keepmax);
            copybytes(qq, y->link.q, q->link.keepmax);
            q->link.q = qq;
            q->dec = newframedec(&u, 1<<10);

            sidesdeliver(p, q, cutat, 1);
            sidesconnect(p, q, 2);   // the rest in flight is lost
            sendlines(&u, &p->link, 11, 12);
            sidesrun(&u, p, q, 7, 2);
            Str got  = finish(&q->got);
            Str want = numlines(&u, 1, 12);
            if (!(got == want) || q->events[LINK_LOST] || q->events[LINK_FRESH]) {
                Buf why(&u, 64);
                print(&why, "link.break at ");
                print(&why, cutat);
                expect(t, finish(&why), got, want);
                ok = 0;
            }
        }
        expect(t, "link.breaks", istr(&tmp, ok), "1");
    }

    // A frame received twice on a stream counts once
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        sidesconnect(x, y, 0);
        sidesrun(&tmp, x, y, 1000, 0);
        sendlines(&tmp, &x->link, 1, 3);
        sidesflush(&tmp, x, y, 0);
        Str again = clone(&tmp, finish(&x->inflight));
        sidesdeliver(x, y, 1000, 0);
        print(&x->inflight, again);
        sidesdeliver(x, y, 1000, 0);
        expect(t, "link.twice", finish(&y->got), numlines(&tmp, 1, 3));
        expect(t, "link.twicenone", istr(&tmp, y->events[LINK_NONE]), "3");

        // A new stream, with nothing acknowledged yet: the replay
        // requests settle it, and nothing arrives twice
        sidesconnect(x, y, 1);
        sidesrun(&tmp, x, y, 5, 1);
        expect(t, "link.resent", finish(&y->got), numlines(&tmp, 1, 3));
        expect(t, "link.resentkept", istr(&tmp, x->link.qcount), "0");

        // Out of order on one stream breaks it
        sidesconnect(x, y, 2);
        sidesrun(&tmp, x, y, 1000, 2);
        FrameIn f = {};
        f.type = FRAME_REGULAR;
        f.id   = y->link.recvd + 2;
        f.ack  = 0;
        expect(t, "link.gap", istr(&tmp, linkframe(&y->link, &f, 3)), istr(&tmp, LINK_BROKEN));
        // As does an acknowledgement of what was never sent
        f.type = FRAME_ACK;
        f.ack  = y->link.nextid + 5;
        expect(t, "link.badack", istr(&tmp, linkframe(&y->link, &f, 3)), istr(&tmp, LINK_BROKEN));
        // Or anything before the replay request, or a second request
        sidesconnect(x, y, 4);
        f.type = FRAME_KEEPALIVE;
        f.ack  = 0;
        expect(t, "link.early", istr(&tmp, linkframe(&y->link, &f, 4)), istr(&tmp, LINK_BROKEN));
        sidesflush(&tmp, x, y, 4);
        sidesdeliver(x, y, 1000, 4);
        Str again2 = frameof(&tmp, FRAME_REPLAY, 1, 0, "12345678");
        f = framenext(&y->dec, &again2);
        expect(t, "link.replay2", istr(&tmp, linkframe(&y->link, &f, 4)), istr(&tmp, LINK_BROKEN));
    }

    // Limits: a count and a size, freed by acknowledgements
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 256, 4);
        sidesinit(&tmp, y, 0x2222, 256, 4);
        sidesconnect(x, y, 0);
        expect(t, "link.count4", istr(&tmp, sendlines(&tmp, &x->link, 1, 4)), "1");
        expect(t, "link.count5", istr(&tmp, sendlines(&tmp, &x->link, 5, 5)), "0");
        sidesrun(&tmp, x, y, 1000, 0);
        linktick(&y->link, LINK_ACKTIME);
        sidesrun(&tmp, x, y, 1000, LINK_ACKTIME);
        expect(t, "link.freed", istr(&tmp, sendlines(&tmp, &x->link, 5, 8)), "1");
        Str big = {allocbytes(&tmp, 250), 250};
        for (iz i = 0; i < big.len; i++) big[i] = 'z';
        expect(t, "link.size", istr(&tmp, linksend(&x->link, big)), "0");
        sidesrun(&tmp, x, y, 1000, LINK_ACKTIME);
        linktick(&y->link, 2*LINK_ACKTIME);
        sidesrun(&tmp, x, y, 1000, 2*LINK_ACKTIME);
        expect(t, "link.sizefits", istr(&tmp, linksend(&x->link, takehead(big, 256-FRAME_HEAD))), "1");
        expect(t, "link.sizefull", istr(&tmp, linksend(&x->link, "")), "0");
        sidesrun(&tmp, x, y, 1000, 2*LINK_ACKTIME);
        expect(t, "link.limits", finish(&y->got),
               concat(&tmp, numlines(&tmp, 1, 8), concat(&tmp, takehead(big, 256-FRAME_HEAD), "\n")));
    }

    // Acknowledgements: every 50 frames, or 2 s after the first owed;
    // keepalives after 5 s of silence; dead after 20 s of nothing
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<14, 1000);
        sidesinit(&tmp, y, 0x2222, 1<<14, 1000);
        sidesconnect(x, y, 0);
        sidesrun(&tmp, x, y, 1<<14, 0);
        sendlines(&tmp, &x->link, 1, 49);
        sidesflush(&tmp, x, y, 10);
        sidesdeliver(x, y, 1<<14, 10);
        expect(t, "ack.49", istr(&tmp, linkpending(&y->link)), "0");
        sendlines(&tmp, &x->link, 50, 50);
        sidesflush(&tmp, x, y, 10);
        sidesdeliver(x, y, 1<<14, 10);
        expect(t, "ack.50", istr(&tmp, linkpending(&y->link)), "1");
        sidesrun(&tmp, x, y, 1<<14, 10);
        expect(t, "ack.50kept", istr(&tmp, x->link.qcount), "0");

        sendlines(&tmp, &x->link, 51, 51);
        sidesflush(&tmp, x, y, 1000);
        sidesdeliver(x, y, 1<<14, 1000);
        expect(t, "ack.deadline", istr(&tmp, linkdeadline(&y->link)), istr(&tmp, 1000+LINK_ACKTIME));
        linktick(&y->link, 1000+LINK_ACKTIME-1);
        expect(t, "ack.early", istr(&tmp, linkpending(&y->link)), "0");
        linktick(&y->link, 1000+LINK_ACKTIME);
        Str out = linkout(&y->link, &tmp, 1000+LINK_ACKTIME);
        expect(t, "ack.frame", out, frameof(&tmp, FRAME_ACK, 0, 51, ""));

        i64 quiet = 1000 + LINK_ACKTIME;
        linktick(&y->link, quiet+LINK_KEEPALIVE-1);
        expect(t, "keep.early", istr(&tmp, linkpending(&y->link)), "0");
        linktick(&y->link, quiet+LINK_KEEPALIVE);
        out = linkout(&y->link, &tmp, quiet+LINK_KEEPALIVE);
        expect(t, "keep.frame", out, frameof(&tmp, FRAME_KEEPALIVE, 0, 51, ""));
        expect(t, "dead.not", istr(&tmp, linktick(&y->link, 1000+LINK_DEAD-1)), "1");
        expect(t, "dead.yes", istr(&tmp, linktick(&y->link, 1000+LINK_DEAD)), "0");

        // A disconnect goes last, after anything queued
        sendlines(&tmp, &y->link, 1, 1);
        linkbye(&y->link);
        sidesflush(&tmp, x, y, 0);
        sidesdeliver(y, x, 1<<14, 0);
        expect(t, "bye.after", finish(&x->got), "line 1\n");
        expect(t, "bye.event", istr(&tmp, x->events[LINK_BYE]), "1");
    }

    // A peer that kept fewer frames than were missed: LINK_LOST, and the
    // stream goes on from what it has
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        sidesconnect(x, y, 0);
        sendlines(&tmp, &x->link, 1, 10);
        sidesrun(&tmp, x, y, 1000, 0);  // y acknowledges none of it
        expect(t, "lost.before", finish(&y->got), numlines(&tmp, 1, 10));
        expect(t, "lost.kept", istr(&tmp, x->link.qcount), "10");
        sendlines(&tmp, &x->link, 11, 20);
        linktrim(&x->link, 15);         // as if 11 to 15 were gone
        y->got.len = 0;
        sidesconnect(x, y, 1);
        sidesrun(&tmp, x, y, 1000, 1);
        expect(t, "lost.event", istr(&tmp, y->events[LINK_LOST]), "1");
        expect(t, "lost.rest", finish(&y->got), numlines(&tmp, 16, 20));
    }

    // A peer that started over (a new token): LINK_FRESH, its numbers
    // taken as they are, and what this side kept for the old one, sent
    // or not, dropped, numbering going on as the replay request said
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        sidesconnect(x, y, 0);
        sendlines(&tmp, &y->link, 1, 30);
        sendlines(&tmp, &x->link, 1, 3);
        sidesrun(&tmp, x, y, 1000, 0);    // sent, never acknowledged
        sendlines(&tmp, &x->link, 4, 5);  // not yet sent
        expect(t, "fresh.before", istr(&tmp, x->link.qcount), "5");
        sidesinit(&tmp, y, 0x3333, 1<<12, 64);
        sendlines(&tmp, &y->link, 1, 2);
        x->got.len = 0;
        sidesconnect(x, y, 1);
        sidesrun(&tmp, x, y, 1000, 1);
        expect(t, "fresh.event", istr(&tmp, x->events[LINK_FRESH]), "1");
        expect(t, "fresh.peer", finish(&x->got), numlines(&tmp, 1, 2));
        expect(t, "fresh.dropped", finish(&y->got), "");
        expect(t, "fresh.kept", istr(&tmp, x->link.qcount), "0");
        sendlines(&tmp, &x->link, 6, 7);
        sidesrun(&tmp, x, y, 1000, 1);
        expect(t, "fresh.after", finish(&y->got), numlines(&tmp, 6, 7));
        expect(t, "fresh.broken", istr(&tmp, x->events[LINK_BROKEN] + y->events[LINK_BROKEN]), "0");
    }

    // The ring of kept frames wraps around its end, over and over, a
    // frame's header and payload split anywhere across it
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 101, 1000);
        sidesinit(&tmp, y, 0x2222, 101, 1000);
        sidesconnect(x, y, 0);
        b32 ok = 1;
        i64 now = 0;
        for (i32 i = 1; i <= 200 && ok; i++) {
            ok &= sendlines(&tmp, &x->link, i, i);
            sidesrun(&tmp, x, y, 7, now);
            now += LINK_ACKTIME;
            linktick(&y->link, now);       // acknowledge: frees the ring
            sidesrun(&tmp, x, y, 7, now);
        }
        expect(t, "ring.sent", istr(&tmp, ok), "1");
        expect(t, "ring.got", finish(&y->got), numlines(&tmp, 1, 200));
        expect(t, "ring.wrapped", istr(&tmp, x->link.qtail > 10*x->link.keepmax), "1");

        // A break with frames kept across the end: resent whole
        sendlines(&tmp, &x->link, 201, 203);
        expect(t, "ring.room", istr(&tmp, linkroom(&x->link) < x->link.keepmax-30), "1");
        sidesconnect(x, y, now);
        sidesrun(&tmp, x, y, 3, now);
        expect(t, "ring.resent", finish(&y->got), numlines(&tmp, 1, 203));
    }

    // Numbers wrap around: comparisons are modular
    {
        Arena tmp = a;
        FrameSide *x = alloc<FrameSide>(&tmp);
        FrameSide *y = alloc<FrameSide>(&tmp);
        sidesinit(&tmp, x, 0x1111, 1<<12, 64);
        sidesinit(&tmp, y, 0x2222, 1<<12, 64);
        x->link.nextid = x->link.firstid = 0xfffffffe;
        sidesconnect(x, y, 0);
        sendlines(&tmp, &x->link, 1, 5);
        sidesrun(&tmp, x, y, 3, 0);
        linktick(&y->link, LINK_ACKTIME);
        sidesrun(&tmp, x, y, 3, LINK_ACKTIME);
        expect(t, "wrap.got", finish(&y->got), numlines(&tmp, 1, 5));
        expect(t, "wrap.kept", istr(&tmp, x->link.qcount), "0");
        expect(t, "wrap.recvd", istr(&tmp, y->link.recvd), "2");
    }
}
