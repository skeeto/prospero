// Engine tests: the pure parts (the turn machine itself runs end to end
// in tests/scenarios.py)

static void test_engine(Test *t)
{
    Arena *a = &t->perm;

    Str long60 = "Refactor the parser so that it handles nested quotes and "
                 "escapes correctly everywhere";
    struct { Str in; Str want; } titles[] = {
        {"go", "go"},
        {"  fix   the\tbug \n", "fix the bug"},
        {"line one\nline two\r\nline three", "line one line two line three"},
        {"", ""},
        {" \n\t ", ""},
        {long60, "Refactor the parser so that it handles nested quotes and\xe2\x80\xa6"},
        // A word that ends exactly at the limit, then more
        {"aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff ggg",
         "aaaaaaaaa bbbbbbbbb ccccccccc ddddddddd eeeeeeeee ffffffffff\xe2\x80\xa6"},
        // Code points, not bytes
        {"\xc3\xa9\xc3\xa9\xc3\xa9", "\xc3\xa9\xc3\xa9\xc3\xa9"},
        {"tab\x7f" "del\xc2\xa0nbsp", "tab del nbsp"},
    };
    for (iz i = 0; i < countof(titles); i++) {
        expect(t, concat(a, "title ", titles[i].in), maketitle(a, titles[i].in),
               titles[i].want);
    }

    // Exactly 60 code points of two-byte characters: no ellipsis
    Buf b(a, 256);
    for (i32 i = 0; i < 60; i++) {
        print(&b, "\xc3\xa9");
    }
    Str sixty = finish(&b);
    expect(t, "title of 60 code points", maketitle(a, sixty), sixty);
    Str more = concat(a, sixty, "\xc3\xa9");
    expect(t, "title of 61 code points",
           maketitle(a, more), concat(a, sixty, "\xe2\x80\xa6"));

    // No space in its second half: cut inside the long word
    Buf in(a, 128);
    print(&in, "x ");
    for (i32 i = 0; i < 70; i++) {
        putbyte(&in, 'a');
    }
    Str word = finish(&in);
    expect(t, "title cut in a word", maketitle(a, word),
           concat(a, takehead(word, 60), "\xe2\x80\xa6"));

    // The id of a command too long to parse, from its start
    struct { Str line; i64 want; } ids[] = {
        {"{\"cmd\":\"prompt\",\"id\":42,\"text\":\"xx", 42},
        {" { \"id\" : -7 , \"cmd\":\"x\"}", -7},
        {"{\"cmd\":\"prompt\",\"sid\":\"a\\\"id\\\":9\",\"id\":3", 3},
        {"{\"text\":{\"id\":5,\"x\":[1,{\"id\":6}]},\"id\":8}", 8},
        {"{\"cmd\":\"prompt\",\"text\":\"xxxxxxxxxxxxxxxx", 0},
        {"{\"cmd\":\"prompt\",\"text\":\"\\\"id\\\":4,", 0},
        {"[\"id\",1]", 0},
        {"{\"id\":\"12\"}", 0},
        {"", 0},
    };
    for (iz i = 0; i < countof(ids); i++) {
        expect(t, concat(a, "cmdlineid ", ids[i].line),
               istr(a, cmdlineid(ids[i].line)), istr(a, ids[i].want));
    }

    // Every command gets its reply: once stopping, those that would start
    // something are refused, and one too long for the mailbox is refused
    // by its id
    {
        Engine *e = alloc<Engine>(a);
        e->os       = newos(a, 1<<16);
        e->out      = newmailbox(a, 1<<16, 0, 0);
        e->in       = newmailbox(a, 1<<12, 0, 0);
        e->quitting = 1;
        Str cmds[] = {
            "{\"cmd\":\"hello\",\"id\":1,\"proto\":0}",
            "{\"cmd\":\"new\",\"id\":2,\"cwd\":\"/w\"}",
            "{\"cmd\":\"open\",\"id\":3,\"sid\":\"x\"}",
            "{\"cmd\":\"prompt\",\"id\":4,\"sid\":\"x\",\"text\":\"t\"}",
            "{\"cmd\":\"steer\",\"id\":5,\"sid\":\"x\",\"text\":\"t\"}",
            "{\"cmd\":\"answer\",\"id\":6,\"sid\":\"x\",\"req\":\"r\"}",
            "{\"cmd\":\"cancel\",\"id\":7,\"sid\":\"x\"}",
        };
        for (iz i = 0; i < countof(cmds); i++) {
            command(e, *a, cmds[i]);
        }
        Buf  big(a, 8192);
        print(&big, "{\"cmd\":\"prompt\",\"id\":8,\"text\":\"");
        for (i32 i = 0; i < 6000; i++) {
            putbyte(&big, 'x');
        }
        print(&big, "\"}");
        expect(t, "enginecmd too long", istr(a, enginecmd(e, finish(&big))), "0");
        Mail in = mailtake(e->in);
        for (Record r; (r = mailnext(&in)).ok;) {
            onrecord(e, *a, r);
        }

        Buf  got(a, 1024);
        Mail m = mailtake(e->out);
        for (Record r; (r = mailnext(&m)).ok;) {
            EvRec rec = evparse(a, r.data);
            if (rec.ev != EV_REPLY) continue;
            print(&got, jsonint(jsonget(rec.root, "id"), 0));
            putbyte(&got, jsonint(jsonget(rec.root, "ok"), 0) ? '+' : '-');
            print(&got, jsonstr(jsonget(rec.root, "err")));
            putbyte(&got, '\n');
        }
        expect(t, "replies while stopping", finish(&got),
               "1+\n"
               "2-the engine is stopping\n"
               "3-the engine is stopping\n"
               "4-the engine is stopping\n"
               "5-the engine is stopping\n"
               "6-the engine is stopping\n"
               "7-no such open session\n"
               "8-command too long\n");
    }
}
