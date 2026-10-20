// Mailbox tests: framing, padding, overflow, buffer swaps, wakeups. The
// test platform is single-threaded, so nothing here may block.

// Render every record of a take as "type/id:payload;", then a trailing
// "!" if records were dropped after them.
static Str mailshow(Arena *a, Mail m)
{
    Buf b(a, 64);
    for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
        print(&b, (i64)r.type);
        putbyte(&b, '/');
        if (r.id > (u64)I64MAX) {
            print(&b, "0x");
            printhex(&b, r.id, 16);
        } else {
            print(&b, (i64)r.id);
        }
        putbyte(&b, ':');
        print(&b, r.data);
        putbyte(&b, ';');
    }
    if (m.overflow) {
        putbyte(&b, '!');
    }
    return finish(&b);
}

static void wakecount(void *ctx)
{
    (*(i32 *)ctx)++;
}

static void test_mailbox(Test *t)
{
    Arena a = t->perm;

    // Framing: a 16-byte header, then the payload padded to 8 bytes
    {
        Mailbox *m = newmailbox(&a, 256, 0, 0);
        expect(t, "mail.max", istr(&a, mailmax(m)), "240");
        mailpost(m, 1, 10, "", 0);
        mailpost(m, 2, 20, "a", 0);
        mailpost(m, 3, 30, "hello, world", 0);
        mailpost(m, 4, (u64)-1, "12345678", 1);
        Mail mail = mailtake(m);
        expect(t, "mail.frame", mailshow(&a, mail),
            "1/10:;2/20:a;3/30:hello, world;4/0xffffffffffffffff:12345678;");
        expect(t, "mail.framelen", istr(&a, mail.len), "96");

        // Iteration yields views into the buffer, never null data
        Record r = mailnext(&mail);
        expect(t, "mail.emptydata", istr(&a, r.ok && r.data.data), "1");
        expect(t, "mail.emptylen", istr(&a, r.data.len), "0");
    }

    // Padding by payload length: 0 needs none, 1..8 pad to 8, 9 to 16
    {
        Mailbox *m = newmailbox(&a, 256, 0, 0);
        Str digits = "0123456789";
        for (iz n = 0; n <= 9; n++) {
            mailpost(m, (u32)n, 0, takehead(digits, n), 0);
        }
        Mail mail = mailtake(m);
        expect(t, "mail.padlen", istr(&a, mail.len), istr(&a, 16 + 8*24 + 32));
        expect(t, "mail.padrecs", mailshow(&a, mail),
            "0/0:;1/0:0;2/0:01;3/0:012;4/0:0123;5/0:01234;6/0:012345;"
            "7/0:0123456;8/0:01234567;9/0:012345678;");
        // The pad after "0" (record 1) is zeroed
        expect(t, "mail.padzero", istr(&a, mail.data[16+16+1]), "0");
    }

    // Capacity rounds up to whole 8-byte units
    {
        Mailbox *m = newmailbox(&a, 61, 0, 0);
        expect(t, "mail.capround", istr(&a, mailmax(m)), "48");
    }

    // An empty take
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Mail mail = mailtake(m);
        expect(t, "mail.emptytake", istr(&a, mail.len), "0");
        expect(t, "mail.emptyshow", mailshow(&a, mail), "");
    }

    // Overflow: a full buffer drops non-blocking records, and keeps
    // dropping them, even ones that would fit, until the next take.
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Str s20 = "abcdefghijklmnopqrst";
        expect(t, "mail.ovpost1", istr(&a, mailpost(m, 1, 1, s20, 0)), "1");
        expect(t, "mail.ovpost2", istr(&a, mailpost(m, 2, 2, s20, 0)), "0");
        expect(t, "mail.ovsticky", istr(&a, mailpost(m, 3, 3, "x", 0)), "0");
        Mail mail = mailtake(m);
        expect(t, "mail.ovtake", mailshow(&a, mail),
            "1/1:abcdefghijklmnopqrst;!");
        expect(t, "mail.ovclear", mailshow(&a, mailtake(m)), "");
        expect(t, "mail.ovresume", istr(&a, mailpost(m, 4, 4, "y", 0)), "1");
        expect(t, "mail.ovafter", mailshow(&a, mailtake(m)), "4/4:y;");
    }

    // A blocking record that fits is posted even while overflow is set,
    // after the gap.
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        mailpost(m, 1, 0, "abcdefghijklmnopqrstuvwx", 0);   // 40 bytes
        mailpost(m, 2, 0, "abcdefghijklmnopqrstuvwx", 0);   // dropped
        expect(t, "mail.ovblock", istr(&a, mailpost(m, 3, 0, "z", 1)), "1");
        expect(t, "mail.ovblockshow", mailshow(&a, mailtake(m)),
            "1/0:abcdefghijklmnopqrstuvwx;3/0:z;!");
    }

    // Exact fit, then full
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Str s48 = "0123456789abcdef0123456789abcdef0123456789abcdef";
        expect(t, "mail.exact", istr(&a, mailpost(m, 1, 0, s48, 0)), "1");
        expect(t, "mail.full", istr(&a, mailpost(m, 2, 0, "", 0)), "0");
        Mail mail = mailtake(m);
        expect(t, "mail.exactlen", istr(&a, mail.len), "64");
        expect(t, "mail.exactov", istr(&a, mail.overflow), "1");
    }

    // Too big for any buffer: rejected in either mode, flagged as lost
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Str s49 = "0123456789abcdef0123456789abcdef0123456789abcdefX";
        expect(t, "mail.bigdrop", istr(&a, mailpost(m, 1, 0, s49, 0)), "0");
        expect(t, "mail.bigblock", istr(&a, mailpost(m, 1, 0, s49, 1)), "0");
        Mail mail = mailtake(m);
        expect(t, "mail.bigtake", mailshow(&a, mail), "!");
    }

    // Swaps alternate between the two buffers, and a taken buffer is
    // untouched by posts until the following take.
    {
        Mailbox *m    = newmailbox(&a, 64, 0, 0);
        u8      *prev = 0;
        u8      *bufs[2] = {};
        b32      ok   = 1;
        Mail     last = {};
        for (i32 round = 0; round < 7; round++) {
            Buf b(&a, 16);
            print(&b, "round ");
            print(&b, (i64)round);
            mailpost(m, (u32)round, (u64)round, finish(&b), 0);
            if (round) {
                // Posted after the previous take: that take is intact
                Buf w(&a, 16);
                print(&w, (i64)round-1);
                print(&w, "/");
                print(&w, (i64)round-1);
                print(&w, ":round ");
                print(&w, (i64)round-1);
                print(&w, ";");
                ok &= mailshow(&a, last) == finish(&w);
            }
            last = mailtake(m);
            ok &= last.data != prev;
            if (round < 2) {
                bufs[round] = last.data;
            } else {
                ok &= last.data == bufs[round%2];
            }
            prev = last.data;
        }
        expect(t, "mail.swaps", istr(&a, ok), "1");
        expect(t, "mail.lastround", mailshow(&a, last), "6/6:round 6;");
    }

    // Only the first record after a take signals the ready event
    {
        Mailbox *m = newmailbox(&a, 256, 0, 0);
        Buf b(&a, 16);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailpost(m, 1, 0, "a", 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailpost(m, 1, 0, "b", 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailtake(m);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailpost(m, 1, 0, "c", 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        expect(t, "mail.ready", finish(&b), "010001");
    }

    // A drop into an empty mailbox wakes the consumer, as no record will,
    // so that it learns of the drop. Later drops add no wakes, and
    // records resume after the take.
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Str s49 = "0123456789abcdef0123456789abcdef0123456789abcdefX";
        Buf b(&a, 16);
        putbyte(&b, '0' + (u8)mailpost(m, 1, 0, s49, 0));
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        putbyte(&b, '0' + (u8)mailpost(m, 2, 0, "a", 0));  // sticky drop
        putbyte(&b, '0' + (u8)mailpost(m, 3, 0, s49, 1));
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        expect(t, "mail.bigwake", finish(&b), "01000");
        expect(t, "mail.bigwaketake", mailshow(&a, mailtake(m)), "!");
        expect(t, "mail.bigresume", istr(&a, mailpost(m, 4, 0, "b", 0)), "1");
        expect(t, "mail.bigresumewake", istr(&a, mailwait(m, 0)), "1");
        expect(t, "mail.bigresumetake", mailshow(&a, mailtake(m)), "4/0:b;");
    }

    // A drop behind pending records adds no wake, nor does a blocking
    // record behind a drop.
    {
        Mailbox *m = newmailbox(&a, 64, 0, 0);
        Str s49 = "0123456789abcdef0123456789abcdef0123456789abcdefX";
        Buf b(&a, 16);
        mailpost(m, 1, 0, "a", 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailpost(m, 2, 0, s49, 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailtake(m);
        mailpost(m, 3, 0, s49, 0);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        mailpost(m, 4, 0, "c", 1);
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        expect(t, "mail.dropwakes", finish(&b), "1010");
        expect(t, "mail.droptake", mailshow(&a, mailtake(m)), "4/0:c;!");
    }

    // Through a notify hook, as the GUI mailbox: a drop into an empty
    // mailbox notifies once, however many drops follow.
    {
        i32      count = 0;
        Mailbox *m     = newmailbox(&a, 64, wakecount, &count);
        Str s49 = "0123456789abcdef0123456789abcdef0123456789abcdefX";
        mailpost(m, 1, 0, s49, 0);
        for (i32 i = 0; i < 100; i++) {
            mailpost(m, 2, (u64)i, "small", 0);
        }
        expect(t, "mail.bignotify", istr(&a, count), "1");
        expect(t, "mail.bignotifytake", mailshow(&a, mailtake(m)), "!");
        mailpost(m, 3, 7, "small", 0);
        expect(t, "mail.bignotifyresume", istr(&a, count), "2");
        expect(t, "mail.bignotifyshow", mailshow(&a, mailtake(m)),
            "3/7:small;");
    }

    // A notify hook replaces the ready event
    {
        i32      count = 0;
        Mailbox *m     = newmailbox(&a, 256, wakecount, &count);
        Buf b(&a, 16);
        mailpost(m, 1, 0, "a", 0);
        print(&b, (i64)count);
        mailpost(m, 1, 0, "b", 0);
        print(&b, (i64)count);
        mailtake(m);
        mailpost(m, 1, 0, "c", 1);
        print(&b, (i64)count);
        putbyte(&b, ' ');
        putbyte(&b, '0' + (u8)mailwait(m, 0));
        expect(t, "mail.notify", finish(&b), "112 0");
    }
}
