// Events tests: name tables, record encoding, and session logs over the
// in-memory file system (torn tails, crash recovery, seq, replay)

// Append a durable record with one payload field given as raw JSON.
static Str addrec(EventLog *log, Arena *a, i32 ev, Str key, Str raw)
{
    Buf   b(a, 256);
    EvOut w = logbegin(log, &b, ev);
    if (key.data) {
        putkey(&w.json, key);
        putraw(&w.json, raw);
    }
    return logappend(log, &w);
}

// Whole file contents, or "(missing)".
static Str vcontents(Os *os, Arena *a, Str path)
{
    Read r = os_read(os, a, path, 1<<20);
    return r.err==READ_OK ? r.data : Str("(missing)");
}

static Str joinlines(Arena *a, Slice<Str> lines)
{
    Buf b(a, 256);
    for (iz i = 0; i < lines.len; i++) {
        print(&b, lines[i]);
    }
    return finish(&b);
}

// Summarize an opened log for comparison.
static Str logshow(Arena *a, EventLog *log)
{
    Buf b(a, 64);
    print(&b, "ok=");
    print(&b, (i64)log->ok);
    print(&b, " seq=");
    print(&b, log->seq);
    print(&b, " turn=");
    print(&b, log->turn);
    print(&b, " torn=");
    print(&b, (i64)log->torn);
    print(&b, " bad=");
    print(&b, (i64)log->bad);
    print(&b, " synth=");
    print(&b, (i64)log->synth);
    return finish(&b);
}

// The seqs of a log's lines, then its sids where they are not "B".
static Str logseqs(Arena *a, Str log)
{
    Buf b(a, 256);
    for (Str rest = log; rest.len;) {
        Cut   c   = cut(rest, '\n');
        EvRec rec = evparse(a, c.head);
        rest = c.tail;
        print(&b, b.len ? Str(" ") : Str(""));
        print(&b, rec.seq);
        if (!(rec.sid == "B")) {
            print(&b, concat(a, "!", rec.sid));
        }
    }
    return finish(&b);
}

// A fork's copy of a log (logcopy): where the cut falls, the session
// record and the sids, the session directory in each of its spellings,
// and the files the copy names.
static void testlogcopy(Test *t)
{
    Arena a       = t->perm;
    Arena scratch = t->perm;
    scratch.beg  += (scratch.end - scratch.beg)/2;
    Str src[] = {
        "{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"proto\":0,\"cwd\":\"/w\","
        "\"scratch\":\"/t/prospero-1/A\"}\n",
        "{\"sid\":\"A\",\"seq\":2,\"ts\":6,\"ev\":\"prefix\",\"system\":\"s\"}\n",
        "{\"sid\":\"A\",\"seq\":3,\"ts\":7,\"ev\":\"user\",\"text\":\"one\"}\n",
        "{\"sid\":\"A\",\"seq\":4,\"ts\":7,\"ev\":\"turn\",\"n\":1}\n",
        "{\"sid\":\"A\",\"seq\":5,\"ts\":8,\"ev\":\"tool.end\",\"call\":\"c1\",\"text\":"
        "\"see /s/sessions/A/out/c1.log, C:\\\\S\\\\sessions\\\\A\\\\out\\\\c2.log, "
        "C:/S/sessions/A/snap/3. Not /s/sessions/AB/x\",\"spill\":\"out/c1.log\"}\n",
        "{\"ts\":9,\"sid\":\"A\",\"seq\":6,\"ev\":\"title\",\"text\":\"First\",\"by\":\"auto\"}\n",
        "{\"sid\":\"A\",\"seq\":7,\"ts\":9,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n",
        "{\"sid\":\"A\",\"seq\":8,\"ts\":9,\"ev\":\"job.end\",\"job\":1}\n",
        "{\"sid\":\"A\",\"seq\":9,\"ts\":9,\"ev\":\"turn\",\"n\":2,\"via\":\"job\"}\n",
        "{\"sid\":\"A\",\"seq\":10,\"ts\":9,\"ev\":\"turn.end\",\"turn\":2,\"why\":\"done\"}\n",
        "{\"sid\":\"A\",\"seq\":11,\"ts\":9,\"ev\":\"model\",\"model\":\"m2\"}\n",
        "{\"sid\":\"A\",\"seq\":12,\"ts\":9,\"ev\":\"user\",\"text\":\"two\"}\n",
        "{\"sid\":\"A\",\"seq\":13,\"ts\":9,\"ev\":\"turn\",\"n\":3}\n",
    };
    Slice<Str> lines = {src, countof(src), countof(src)};
    Str        po[]  = {"/s/sessions/A"};
    Str        pn[]  = {"/s/sessions/B"};
    Str        wo[]  = {"C:\\S\\sessions\\A"};
    Str        wn[]  = {"C:\\S\\sessions\\B"};
    Slice<Str> posix = {po, 1, 1};
    Slice<Str> posixnew = {pn, 1, 1};
    Slice<Str> win   = {wo, 1, 1};
    Slice<Str> winnew = {wn, 1, 1};

    struct {
        i64 turn;
        Str want;  // seqs, the next prompt, the model, open, the turn
    } cuts[] = {
        { 1, "1 2 3 4 5 6 7 8 9 next= model= open=0 turn=1"},
        { 2, "1 2 3 4 5 6 7 8 9 10 11 12 next=two model=m2 open=0 turn=2"},
        { 0, "1 2 3 next=one model= open=0 turn=0"},
        {-1, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 next= model=m2 open=1 turn=3"},
    };
    for (iz i = 0; i < countof(cuts); i++) {
        LogCopy c = logcopy(&a, scratch, lines, "A", "B", posix, posixnew,
                            cuts[i].turn, 100, 0);
        Buf     b(&a, 256);
        print(&b, logseqs(&a, c.log));
        print(&b, " next=");
        print(&b, c.next);
        print(&b, " model=");
        print(&b, c.model);
        print(&b, " open=");
        print(&b, (i64)c.open);
        print(&b, " turn=");
        print(&b, c.turn);
        expect(t, concat(&a, "logcopy cut ", istr(&a, cuts[i].turn)), finish(&b),
               cuts[i].want);
    }

    LogCopy c = logcopy(&a, scratch, lines, "A", "B", posix, posixnew, 1, 100, 0);
    Str     want =
        "{\"sid\":\"B\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"forked_from\":\"A\","
        "\"fork_turn\":1,\"proto\":0,\"cwd\":\"/w\",\"scratch\":\"/t/prospero-1/A\"}\n"
        "{\"sid\":\"B\",\"seq\":2,\"ts\":6,\"ev\":\"prefix\",\"system\":\"s\"}\n"
        "{\"sid\":\"B\",\"seq\":3,\"ts\":7,\"ev\":\"user\",\"text\":\"one\"}\n"
        "{\"sid\":\"B\",\"seq\":4,\"ts\":7,\"ev\":\"turn\",\"n\":1}\n"
        "{\"sid\":\"B\",\"seq\":5,\"ts\":8,\"ev\":\"tool.end\",\"call\":\"c1\",\"text\":"
        "\"see /s/sessions/B/out/c1.log, C:\\\\S\\\\sessions\\\\A\\\\out\\\\c2.log, "
        "C:/S/sessions/A/snap/3. Not /s/sessions/AB/x\",\"spill\":\"out/c1.log\"}\n"
        "{\"sid\":\"B\",\"ts\":9,\"seq\":6,\"ev\":\"title\",\"text\":\"First\",\"by\":\"auto\"}\n"
        "{\"sid\":\"B\",\"seq\":7,\"ts\":9,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n"
        "{\"sid\":\"B\",\"seq\":8,\"ts\":9,\"ev\":\"job.end\",\"job\":1}\n"
        "{\"sid\":\"B\",\"seq\":9,\"ts\":100,\"ev\":\"title\",\"text\":\"First (fork)\","
        "\"by\":\"auto\"}\n";
    expect(t, "logcopy", c.log, want);
    expect(t, "logcopy title", c.title, "First (fork)");
    expect(t, "logcopy files", joinlines(&a, c.files), "out/c1.logout/c2.logsnap/3.snap/3");

    // Windows: backslashes doubled in JSON, or either separator
    c = logcopy(&a, scratch, lines, "A", "B", win, winnew, 1, 100, 1);
    Str rest = cuthead(c.log, find(c.log, "{\"sid\":\"B\",\"seq\":5,"));
    Str text = jsonstr(jsonget(evparse(&a, cut(rest, '\n').head).root, "text"));
    expect(t, "logcopy windows", text,
           "see /s/sessions/A/out/c1.log, C:\\S\\sessions\\B\\out\\c2.log, "
           "C:/S/sessions/B/snap/3. Not /s/sessions/AB/x");

    // Bypass mode stays behind, as plan mode does not: a mode record
    // back to Build, in the new directory, before the title
    {
        Str modes[] = {
            src[0],
            "{\"sid\":\"A\",\"seq\":2,\"ts\":6,\"ev\":\"mode\",\"mode\":\"plan\","
            "\"by\":\"new\",\"path\":\"/s/sessions/A/plan.md\"}\n",
            "{\"sid\":\"A\",\"seq\":3,\"ts\":7,\"ev\":\"mode\",\"mode\":\"bypass\","
            "\"by\":\"user\",\"path\":\"/s/sessions/A/plan.md\"}\n",
        };
        Slice<Str> ml = {modes, countof(modes), countof(modes)};
        c = logcopy(&a, scratch, ml, "A", "B", posix, posixnew, -1, 100, 0);
        rest = cuthead(c.log, find(c.log, "{\"sid\":\"B\",\"seq\":4,"));
        expect(t, "logcopy bypass", rest,
               "{\"sid\":\"B\",\"seq\":4,\"ts\":100,\"ev\":\"mode\",\"mode\":\"default\","
               "\"by\":\"fork\",\"path\":\"/s/sessions/B/plan.md\"}\n"
               "{\"sid\":\"B\",\"seq\":5,\"ts\":100,\"ev\":\"title\",\"text\":\"Untitled (fork)\","
               "\"by\":\"auto\"}\n");
        ml.len = 2;
        c = logcopy(&a, scratch, ml, "A", "B", posix, posixnew, -1, 100, 0);
        expect(t, "logcopy plan", logseqs(&a, c.log), "1 2 3");
        ml.len = 3;
        c = logcopy(&a, scratch, ml, "A", "B", posix, posixnew, -1, 100, 0, 1);
        expect(t, "logcopy bypass imported", logseqs(&a, c.log), "1 2 3 4");
    }

    // Refused: a subagent's log, a turn not ended, a turn not there
    Str kid[] = {"{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"parent\":\"P\"}\n"};
    c = logcopy(&a, scratch, {kid, 1, 1}, "A", "B", posix, posixnew, -1, 100, 0);
    expect(t, "logcopy subagent", c.err, "a subagent cannot be forked, only the "
           "session it worked for");
    c = logcopy(&a, scratch, lines, "A", "B", posix, posixnew, 3, 100, 0);
    expect(t, "logcopy open turn", c.err, "turn 3 has not ended");
    c = logcopy(&a, scratch, lines, "A", "B", posix, posixnew, 4, 100, 0);
    expect(t, "logcopy no turn", c.err, "no turn 4 in this session");
    c = logcopy(&a, scratch, {src+1, 1, 1}, "A", "B", posix, posixnew, -1, 100, 0);
    expect(t, "logcopy no session", istr(&a, c.err.len>0 && !c.log.data), "1");
}

// An import of a log from elsewhere (logimport): the conversation's
// records kept, with their seqs, and every other dropped; tool results
// without the fields naming the old session's files; the session record
// made anew; a torn tail and a few bad lines skipped; and refusals.
static void testlogimport(Test *t)
{
    Arena a       = t->perm;
    Arena scratch = t->perm;
    scratch.beg  += (scratch.end - scratch.beg)/2;
    Str log =
        "{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"forked_from\":\"Z\","
        "\"proto\":0,\"version\":\"0.9\",\"cwd\":\"/w\",\"role\":\"r\",\"model\":\"m\","
        "\"endpoint\":\"http://evil/\",\"scratch\":\"/etc\",\"roots\":[\"/\"],"
        "\"temp\":\"allow\",\"tools\":[\"read\"],\"prompt\":\"p\",\"steps\":9}\n"
        "{\"sid\":\"A\",\"seq\":2,\"ts\":6,\"ev\":\"instructions\",\"why\":\"start\"}\n"
        "{\"sid\":\"A\",\"seq\":3,\"ts\":6,\"ev\":\"prefix\",\"system\":\"s\"}\n"
        "{\"sid\":\"A\",\"seq\":4,\"ts\":7,\"ev\":\"user\",\"text\":\"one\"}\n"
        "{\"sid\":\"A\",\"seq\":5,\"ts\":7,\"ev\":\"turn\",\"n\":1}\n"
        "{\"sid\":\"A\",\"seq\":6,\"ts\":7,\"ev\":\"grant\",\"path\":\"/\",\"access\":\"write\"}\n"
        "garbage\n"
        "{\"sid\":\"A\",\"seq\":7,\"ts\":8,\"ev\":\"tool.start\",\"call\":\"c1\",\"name\":\"x\"}\n"
        "{\"sid\":\"A\",\"seq\":8,\"ts\":8,\"ev\":\"snap\",\"call\":\"c1\",\"path\":\"/etc/passwd\","
        "\"snap\":\"snap/1\"}\n"
        "{\"sid\":\"A\",\"seq\":9,\"ts\":8,\"ev\":\"tool.end\",\"call\":\"c1\",\"text\":\"t\","
        "\"spill\":\"../../x\",\"bytes\":3,\"image\":\"../../x\",\"width\":1,\"height\":1,"
        "\"shot\":\"s\",\"child\":\"K\",\"job\":2,\"to\":\"K2\",\"to_title\":\"T\","
        "\"ms\":4}\n"
        "{\"sid\":\"B\",\"seq\":10,\"ts\":8,\"ev\":\"user\",\"text\":\"other\"}\n"
        "{\"sid\":\"A\",\"seq\":11,\"ts\":8,\"ev\":\"rewind\",\"undone\":[\"snap/1\"]}\n"
        "{\"sid\":\"A\",\"seq\":12,\"ts\":8,\"ev\":\"job.start\",\"job\":2,\"spill\":\"/x\"}\n"
        "{\"sid\":\"A\",\"seq\":13,\"ts\":8,\"ev\":\"ask\",\"req\":\"q\"}\n"
        "{\"sid\":\"A\",\"seq\":14,\"ts\":8,\"ev\":\"answer\",\"req\":\"q\"}\n"
        "{\"sid\":\"A\",\"seq\":15,\"ts\":8,\"ev\":\"model\",\"model\":\"m2\"}\n"
        "{\"sid\":\"A\",\"seq\":16,\"ts\":9,\"ev\":\"title\",\"text\":\"First\",\"by\":\"auto\"}\n"
        "{\"sid\":\"A\",\"seq\":17,\"ts\":9,\"ev\":\"newer\",\"x\":1}\n"
        "{\"sid\":\"A\",\"seq\":18,\"ts\":9,\"ev\":\"notice\",\"kind\":\"message\","
        "\"from\":\"K3\",\"from_title\":\"U\",\"hop\":1,\"text\":\"hi\"}\n"
        "{\"sid\":\"A\",\"seq\":19,\"ts\":9,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n"
        "{\"sid\":\"A\",\"seq\":20,\"ts\":9,\"ev\":\"user\",\"te";
    LogImport r = logimport(&a, scratch, log, "N", "/here", "mm", "http://e/",
                            "/t/prospero-1/N", 100);
    Str want =
        "{\"sid\":\"N\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"imported_from\":\"A\","
        "\"proto\":0,\"version\":\"0.9\",\"cwd\":\"/here\",\"model\":\"mm\","
        "\"endpoint\":\"http://e/\",\"scratch\":\"/t/prospero-1/N\",\"role\":\"r\","
        "\"prompt\":\"p\",\"tools\":[\"read\"],\"steps\":9}\n"
        "{\"sid\":\"N\",\"seq\":4,\"ts\":7,\"ev\":\"user\",\"text\":\"one\"}\n"
        "{\"sid\":\"N\",\"seq\":5,\"ts\":7,\"ev\":\"turn\",\"n\":1}\n"
        "{\"sid\":\"N\",\"seq\":7,\"ts\":8,\"ev\":\"tool.start\",\"call\":\"c1\",\"name\":\"x\"}\n"
        "{\"sid\":\"N\",\"seq\":9,\"ts\":8,\"ev\":\"tool.end\",\"call\":\"c1\",\"text\":\"t\","
        "\"to_title\":\"T\",\"ms\":4}\n"
        "{\"sid\":\"N\",\"seq\":16,\"ts\":9,\"ev\":\"title\",\"text\":\"First\",\"by\":\"auto\"}\n"
        "{\"sid\":\"N\",\"seq\":18,\"ts\":9,\"ev\":\"notice\",\"kind\":\"message\","
        "\"from_title\":\"U\",\"hop\":1,\"text\":\"hi\"}\n"
        "{\"sid\":\"N\",\"seq\":19,\"ts\":9,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n"
        "{\"sid\":\"N\",\"seq\":20,\"ts\":100,\"ev\":\"title\",\"text\":\"First (imported)\","
        "\"by\":\"auto\"}\n";
    expect(t, "logimport", r.log, want);
    expect(t, "logimport err", r.err, "");
    expect(t, "logimport from", r.from, "A");
    expect(t, "logimport title", r.title, "First (imported)");
    Buf b(&a, 64);
    print(&b, (i64)r.kept);
    putbyte(&b, ' ');
    print(&b, (i64)r.dropped);
    putbyte(&b, ' ');
    print(&b, (i64)r.bad);
    expect(t, "logimport counts", finish(&b), "8 10 2");

    // Refused: a subagent's log, one that begins otherwise, mostly bad
    struct {
        Str log;
        Str err;
    } refused[] = {
        {"{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"parent\":\"P\"}\n",
         "a subagent's log cannot be imported, only its parent's"},
        {"{\"sid\":\"A\",\"seq\":2,\"ts\":5,\"ev\":\"session\"}\n",
         "not a session's raw log: it must begin with its session record"},
        {"{\"sid\":\"../x\",\"seq\":1,\"ts\":5,\"ev\":\"session\"}\n",
         "not a session's raw log: it must begin with its session record"},
        {"{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"user\"}\n",
         "not a session's raw log: it must begin with its session record"},
        {"{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\"}",
         "not a session's raw log: it must begin with its session record"},
        {"", "not a session's raw log: it must begin with its session record"},
        {"{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\"}\nx\ny\n",
         "most of its lines are not records of its session"},
    };
    for (iz i = 0; i < countof(refused); i++) {
        r = logimport(&a, scratch, refused[i].log, "N", "/here", "mm", "http://e/",
                      {}, 100);
        expect(t, concat(&a, "logimport refused ", istr(&a, i)), r.err, refused[i].err);
        expect(t, concat(&a, "logimport no log ", istr(&a, i)), istr(&a, !r.log.data), "1");
    }

    // A session record too long for the sessions list
    Buf big(&a, 8192);
    print(&big, "{\"sid\":\"A\",\"seq\":1,\"ts\":5,\"ev\":\"session\",\"prompt\":\"");
    for (i32 i = 0; i < 4096; i++) {
        putbyte(&big, 'p');
    }
    print(&big, "\"}\n");
    r = logimport(&a, scratch, finish(&big), "N", "/here", "mm", "http://e/", {}, 100);
    expect(t, "logimport long", r.err, "its session record is too long to be listed: over 4 KiB");
}

static void test_events(Test *t)
{
    Arena perm    = t->perm;
    perm.end      = perm.beg + (perm.end - perm.beg)/2;
    Arena scratch = t->perm;
    scratch.beg   = perm.end;
    Arena *a      = &perm;

    // Name tables
    {
        Buf durable(a, 256);
        Buf sync(a, 256);
        for (i32 ev = 1; ev < EV_COUNT; ev++) {
            expect(t, evname(ev), istr(a, evfind(evname(ev))), istr(a, ev));
            if (evdurable(ev)) {
                print(&durable, evname(ev));
                putbyte(&durable, ' ');
            }
            if (evinfo[ev].sync) {
                print(&sync, evname(ev));
                putbyte(&sync, ' ');
            }
        }
        expect(t, "durable events", finish(&durable),
               "session title user turn attempt assistant tool.start "
               "tool.end ask answer retry nudge turn.end note compact job.start "
               "job.end todo snap rewind instructions notice prefix model grant mode ");
        expect(t, "synced events", finish(&sync), "tool.start ask answer turn.end rewind ");
        expect(t, "unknown event", istr(a, evfind("tool")), istr(a, EV_NONE));
        expect(t, "empty event", istr(a, evfind("")), istr(a, EV_NONE));
        for (i32 cmd = 1; cmd < CMD_COUNT; cmd++) {
            Str name = cmdname(cmd);
            expect(t, name, istr(a, cmdfind(name)), istr(a, cmd));
        }
        expect(t, "cmd config.set", istr(a, cmdfind("config.set")), istr(a, CMD_CONFIGSET));
        expect(t, "unknown cmd", istr(a, cmdfind("Quit")), istr(a, CMD_NONE));
        expect(t, "cmd draft", istr(a, cmdfind("draft")), istr(a, CMD_DRAFT));
    }

    // What hello says the engine speaks, a host adding "host"
    {
        Buf     b(a, 64);
        JsonOut w = {&b};
        putobject(&w);
        putcaps(&w, 0);
        putend(&w);
        expect(t, "putcaps", finish(&b),
               "{\"cmds\":[\"hello\",\"list\",\"new\",\"open\",\"close\",\"prompt\","
               "\"steer\",\"cancel\",\"answer\",\"config.get\",\"config.set\",\"quit\","
               "\"rewind\",\"job.kill\",\"status\",\"stop\",\"file\",\"rename\",\"delete\","
               "\"archive\",\"group\",\"fork\",\"compact\",\"model\",\"draft\",\"upload\","
               "\"import\",\"skills\",\"mode\",\"unqueue\"],"
               "\"caps\":[\"close.forget\",\"config.refresh\",\"file.base64\",\"desktop\","
               "\"subagents\",\"rename\",\"title.by\",\"archive\",\"group\",\"draft\","
               "\"compact\",\"model\",\"gauge\",\"answer.always\",\"scratch\","
               "\"delete\",\"fork\",\"config.delete\",\"import\","
               "\"answer.always.desktop\",\"skills\",\"plan\",\"queue\",\"message\",\"images\",\"home\",\"bypass\"]}");
        b.len = 0;
        w = {&b};
        putobject(&w);
        putcaps(&w, 1);
        putend(&w);
        JsonDoc d    = jsonparse(a, finish(&b));
        Json   *caps = jsonget(d.root, "caps");
        expect(t, "putcaps host", concat(a, istr(a, jsonlen(jsonget(d.root, "cmds"))),
               concat(a, " ", jsonstr(jsonat(caps, jsonlen(caps)-1)))),
               concat(a, istr(a, CMD_COUNT-1), " host"));
    }

    // Session ids: one plain name of a directory, never a path
    {
        struct { Str sid; b32 ok; } sids[] = {
            {"20261020-153012-7f3a", 1},
            {"a", 1},
            {"x.y", 1},
            {"", 0},
            {".", 0},
            {"..", 0},
            {".hidden", 0},
            {"a/b", 0},
            {"a\\b", 0},
            {"C:x", 0},
            {"tab\tin", 0},
            {"nul\0in", 0},
            {"del\x7f", 0},
            {"0123456789012345678901234567890123456789012345678901234567890123", 1},
            {"01234567890123456789012345678901234567890123456789012345678901234", 0},
        };
        for (iz i = 0; i < countof(sids); i++) {
            expect(t, concat(a, "validsid ", sids[i].sid),
                   istr(a, validsid(sids[i].sid)), istr(a, sids[i].ok));
        }
    }

    // Encoding: fixed header order, seq only when durable, one line each
    {
        Buf   b(a, 64);
        EvOut w = evbegin(&b, "20261020-153012-7f3a", 17, 1760974212345, EV_TOOLSTART);
        putkey(&w.json, "call");
        putstr(&w.json, "c1");
        putkey(&w.json, "name");
        putstr(&w.json, "shell");
        putkey(&w.json, "args");
        putstr(&w.json, "{\"cmd\":\"ls\\n\"}");
        putkey(&w.json, "cwd");
        putstr(&w.json, "/tmp");
        expect(t, "durable record", evend(&w),
               "{\"sid\":\"20261020-153012-7f3a\",\"seq\":17,\"ts\":1760974212345,"
               "\"ev\":\"tool.start\",\"call\":\"c1\",\"name\":\"shell\","
               "\"args\":\"{\\\"cmd\\\":\\\"ls\\\\n\\\"}\",\"cwd\":\"/tmp\"}\n");

        w = evbegin(&b, "s", 99, 5, EV_DELTA);
        putkey(&w.json, "turn");
        putint(&w.json, 1);
        putkey(&w.json, "kind");
        putstr(&w.json, "text");
        putkey(&w.json, "text");
        putstr(&w.json, "Hel\nlo\r\n");
        expect(t, "transient record", evend(&w),
               "{\"sid\":\"s\",\"ts\":5,\"ev\":\"delta\",\"turn\":1,"
               "\"kind\":\"text\",\"text\":\"Hel\\nlo\\r\\n\"}\n");

        w = evbegin(&b, {}, 0, 6, EV_REPLY);
        putkey(&w.json, "id");
        putint(&w.json, 3);
        putkey(&w.json, "ok");
        putint(&w.json, 0);
        putkey(&w.json, "err");
        putstr(&w.json, "no such session");
        expect(t, "reply without sid", evend(&w),
               "{\"ts\":6,\"ev\":\"reply\",\"id\":3,\"ok\":0,\"err\":\"no such session\"}\n");

        w = evbegin(&b, "s", 4, 7, EV_ASSISTANT);
        putkey(&w.json, "blocks");
        putarray(&w.json);
        putobject(&w.json);
        putkey(&w.json, "t");
        putstr(&w.json, "call");
        putkey(&w.json, "args");
        putstr(&w.json, "{\"a\": 1}");
        putend(&w.json);
        putend(&w.json);
        putkey(&w.json, "usage");
        putobject(&w.json);
        putkey(&w.json, "in");
        putint(&w.json, 10);
        putend(&w.json);
        Str line = evend(&w);
        expect(t, "nested payload", line,
               "{\"sid\":\"s\",\"seq\":4,\"ts\":7,\"ev\":\"assistant\",\"blocks\":"
               "[{\"t\":\"call\",\"args\":\"{\\\"a\\\": 1}\"}],\"usage\":{\"in\":10}}\n");

        w = cmdbegin(&b, CMD_PROMPT, 3);
        putkey(&w.json, "sid");
        putstr(&w.json, "s");
        putkey(&w.json, "text");
        putstr(&w.json, "hi");
        expect(t, "command", evend(&w), "{\"cmd\":\"prompt\",\"id\":3,\"sid\":\"s\",\"text\":\"hi\"}\n");

        // Records read back
        EvRec r = evparse(a, line);
        Buf   s(a, 64);
        print(&s, r.sid);
        putbyte(&s, ' ');
        print(&s, r.seq);
        putbyte(&s, ' ');
        print(&s, r.ts);
        putbyte(&s, ' ');
        print(&s, evname(r.ev));
        putbyte(&s, ' ');
        print(&s, (i64)r.ok);
        expect(t, "evparse", finish(&s), "s 4 7 assistant 1");
        r = evparse(a, "[1]");
        expect(t, "evparse array", istr(a, r.ok), "0");
        r = evparse(a, "{\"ev\":\"nope\"}");
        expect(t, "evparse unknown", istr(a, r.ok*10 + r.ev), "10");
    }

    // A new log, written through a crash, then recovered
    {
        Os *os = newos(a, 1<<20);
        Str path = "state/sessions/s1/events.jsonl";
        os->now = 1000000;
        EventLog log = logopen(os, a, scratch, path, "s1", 1<<20);
        expect(t, "new log", logshow(a, &log), "ok=1 seq=0 turn=0 torn=0 bad=0 synth=0");
        expect(t, "new log file", vcontents(os, a, path), "");

        Str line = addrec(&log, a, EV_SESSION, "cwd", "\"/w\"");
        expect(t, "first record", line, "{\"sid\":\"s1\",\"seq\":1,\"ts\":1000,\"ev\":\"session\",\"cwd\":\"/w\"}\n");
        expect(t, "first record file", vcontents(os, a, path), line);
        addrec(&log, a, EV_TURN, "n", "1");
        addrec(&log, a, EV_TOOLSTART, "call", "\"c1\"");
        addrec(&log, a, EV_TOOLSTART, "call", "\"c\\\"2\"");
        addrec(&log, a, EV_TOOLEND, "call", "\"c1\"");
        expect(t, "seq after writes", istr(a, log.seq), "5");
        logclose(&log);
        expect(t, "closed append", istr(a, !addrec(&log, a, EV_NOTE, {}, {}).data), "1");

        Str before =
            "{\"sid\":\"s1\",\"seq\":1,\"ts\":1000,\"ev\":\"session\",\"cwd\":\"/w\"}\n"
            "{\"sid\":\"s1\",\"seq\":2,\"ts\":1000,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s1\",\"seq\":3,\"ts\":1000,\"ev\":\"tool.start\",\"call\":\"c1\"}\n"
            "{\"sid\":\"s1\",\"seq\":4,\"ts\":1000,\"ev\":\"tool.start\",\"call\":\"c\\\"2\"}\n"
            "{\"sid\":\"s1\",\"seq\":5,\"ts\":1000,\"ev\":\"tool.end\",\"call\":\"c1\"}\n";
        expect(t, "log before crash", vcontents(os, a, path), before);

        // Restart: the open tool ends as unknown, the turn as interrupted
        os->now = 2000000;
        log = logopen(os, a, scratch, path, "s1", 1<<20);
        expect(t, "recovered", logshow(a, &log), "ok=1 seq=7 turn=1 torn=0 bad=0 synth=2");
        Str after = concat(a, before,
            "{\"sid\":\"s1\",\"seq\":6,\"ts\":2000,\"ev\":\"tool.end\",\"call\":\"c\\\"2\","
            "\"status\":\"unknown\",\"text\":\"outcome unknown: the harness stopped while this ran\"}\n"
            "{\"sid\":\"s1\",\"seq\":7,\"ts\":2000,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"interrupted\"}\n");
        expect(t, "log after recovery", vcontents(os, a, path), after);

        // Sequence numbers continue, and recovery is idempotent
        line = addrec(&log, a, EV_TURN, "n", "2");
        expect(t, "seq continues", line, "{\"sid\":\"s1\",\"seq\":8,\"ts\":2000,\"ev\":\"turn\",\"n\":2}\n");
        addrec(&log, a, EV_TURNEND, "turn", "2");
        logclose(&log);
        log = logopen(os, a, scratch, path, "s1", 1<<20);
        expect(t, "reopen clean", logshow(a, &log), "ok=1 seq=9 turn=2 torn=0 bad=0 synth=0");
        logclose(&log);
        after = vcontents(os, a, path);

        // Replay from a sequence number
        Replay r = logreplay(os, a, scratch, path, "s1", 0, 1<<20);
        expect(t, "replay all", joinlines(a, r.lines), after);
        expect(t, "replay all last", istr(a, r.last), "9");
        r = logreplay(os, a, scratch, path, "s1", 7, 1<<20);
        expect(t, "replay from 7", joinlines(a, r.lines),
               "{\"sid\":\"s1\",\"seq\":8,\"ts\":2000,\"ev\":\"turn\",\"n\":2}\n"
               "{\"sid\":\"s1\",\"seq\":9,\"ts\":2000,\"ev\":\"turn.end\",\"turn\":2}\n");
        r = logreplay(os, a, scratch, path, "s1", 9, 1<<20);
        expect(t, "replay none", istr(a, r.ok*10 + r.lines.len), "10");
        r = logreplay(os, a, scratch, path, "s2", 0, 1<<20);
        expect(t, "replay other sid", istr(a, r.lines.len), "0");
        r = logreplay(os, a, scratch, "nope/events.jsonl", "s1", 0, 1<<20);
        expect(t, "replay missing", istr(a, r.ok*10 + r.lines.len), "10");
        r = logreplay(os, a, scratch, path, "s1", 0, 100);
        expect(t, "replay too big", istr(a, r.ok), "0");

        // From where a replay ended: only what came since, never a torn
        // line, which a later replay reads once it is whole
        r = logreplay(os, a, scratch, path, "s1", 0, 1<<20);
        expect(t, "replay end", istr(a, r.end), istr(a, after.len));
        Str more = "{\"sid\":\"s1\",\"seq\":10,\"ts\":3000,\"ev\":\"note\",\"text\":\"n\"}\n";
        Str torn = "{\"sid\":\"s1\",\"seq\":11,\"ts\":3000,\"ev\":\"no";
        os_write(os, scratch, path, concat(a, concat(a, after, more), torn));
        Replay r2 = logreplayat(os, a, scratch, path, "s1", 9, r.end, 1<<20);
        expect(t, "replay at", joinlines(a, r2.lines), more);
        expect(t, "replay at end", istr(a, r2.end), istr(a, after.len + more.len));
        Str rest = "te\",\"text\":\"m\"}\n";
        os_write(os, scratch, path, concat(a, concat(a, concat(a, after, more), torn), rest));
        Replay r3 = logreplayat(os, a, scratch, path, "s1", 10, r2.end, 1<<20);
        expect(t, "replay at whole", joinlines(a, r3.lines), concat(a, torn, rest));
        os_write(os, scratch, path, after);
        log = logopen(os, a, scratch, path, "s1", 100);
        expect(t, "open too big", istr(a, log.ok*10 + (log.fd>=0)), "0");
    }

    // A torn tail is cut back to the last complete line
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s2.jsonl";
        Str  good =
            "{\"sid\":\"s2\",\"seq\":1,\"ts\":0,\"ev\":\"session\"}\n"
            "{\"sid\":\"s2\",\"seq\":2,\"ts\":0,\"ev\":\"title\",\"text\":\"x\"}\n";
        Str  torn = "{\"sid\":\"s2\",\"seq\":3,\"ts\":0,\"ev\":\"ti";
        os_write(os, scratch, path, concat(a, good, torn));

        Replay r = logreplay(os, a, scratch, path, "s2", 0, 1<<20);
        expect(t, "replay skips torn", joinlines(a, r.lines), good);
        expect(t, "replay leaves torn", vcontents(os, a, path), concat(a, good, torn));

        EventLog log = logopen(os, a, scratch, path, "s2", 1<<20);
        expect(t, "torn open", logshow(a, &log), "ok=1 seq=2 turn=0 torn=35 bad=0 synth=0");
        expect(t, "torn file", vcontents(os, a, path), good);
        Str line = addrec(&log, a, EV_TITLE, "text", "\"y\"");
        expect(t, "torn next", line, "{\"sid\":\"s2\",\"seq\":3,\"ts\":0,\"ev\":\"title\",\"text\":\"y\"}\n");
        expect(t, "torn appended", vcontents(os, a, path), concat(a, good, line));
        logclose(&log);

        // Torn power-loss garbage with no newline at all
        os_write(os, scratch, "s3.jsonl", Str("{\"sid\":\"s3\"\0\0\0\0"));
        log = logopen(os, a, scratch, "s3.jsonl", "s3", 1<<20);
        expect(t, "torn only", logshow(a, &log), "ok=1 seq=0 turn=0 torn=15 bad=0 synth=0");
        expect(t, "torn only file", vcontents(os, a, "s3.jsonl"), "");
        logclose(&log);
    }

    // Invalid lines are skipped, never trusted, and never deleted, while
    // an unknown event (from a newer version) keeps its seq
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s4.jsonl";
        Str  first  = "{\"sid\":\"s4\",\"seq\":1,\"ts\":0,\"ev\":\"session\"}\n";
        Str  third  = "{\"sid\":\"s4\",\"seq\":3,\"ts\":0,\"ev\":\"title\",\"text\":\"t\"}\n";
        Str  fourth = "{\"sid\":\"s4\",\"seq\":4,\"ts\":0,\"ev\":\"bogus\"}\n";
        Str  data  = concat(a, first,
            "garbage\n"
            "\n"
            "{\"sid\":\"other\",\"seq\":2,\"ts\":0,\"ev\":\"title\",\"text\":\"x\"}\n"
            "{\"sid\":\"s4\",\"ts\":0,\"ev\":\"delta\",\"text\":\"x\"}\n"
            "{\"sid\":\"s4\",\"seq\":1,\"ts\":0,\"ev\":\"title\",\"text\":\"dup\"}\n"
            "{\"sid\":\"s4\",\"seq\":2,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"x\"\n");
        data = concat(a, data, third);
        data = concat(a, data, fourth);
        os_write(os, scratch, path, data);

        EventLog log = logopen(os, a, scratch, path, "s4", 1<<20);
        expect(t, "bad lines", logshow(a, &log), "ok=1 seq=4 turn=0 torn=0 bad=6 synth=0");
        expect(t, "bad lines kept", vcontents(os, a, path), data);
        Replay r = logreplay(os, a, scratch, path, "s4", 0, 1<<20);
        expect(t, "bad lines replay", joinlines(a, r.lines),
               concat(a, concat(a, first, third), fourth));
        expect(t, "bad lines next", addrec(&log, a, EV_NOTE, "level", "\"info\""),
               "{\"sid\":\"s4\",\"seq\":5,\"ts\":0,\"ev\":\"note\",\"level\":\"info\"}\n");
        logclose(&log);
    }

    // Records from a newer version end the log, so their numbers must
    // not be reused, and replay passes them on for the GUI to ignore.
    // An open turn still gets its end after them.
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s8.jsonl";
        Str  done =
            "{\"sid\":\"s8\",\"seq\":1,\"ts\":0,\"ev\":\"session\"}\n"
            "{\"sid\":\"s8\",\"seq\":2,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s8\",\"seq\":3,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n";
        Str  newer = "{\"sid\":\"s8\",\"seq\":4,\"ts\":0,\"ev\":\"later\",\"n\":1}\n";
        os_write(os, scratch, path, concat(a, done, newer));
        EventLog log = logopen(os, a, scratch, path, "s8", 1<<20);
        expect(t, "newer event", logshow(a, &log), "ok=1 seq=4 turn=1 torn=0 bad=0 synth=0");
        Str line = addrec(&log, a, EV_NOTE, {}, {});
        expect(t, "newer event next", line, "{\"sid\":\"s8\",\"seq\":5,\"ts\":0,\"ev\":\"note\"}\n");
        Replay r = logreplay(os, a, scratch, path, "s8", 3, 1<<20);
        expect(t, "newer event replay", joinlines(a, r.lines), concat(a, newer, line));
        logclose(&log);

        os_write(os, scratch, path,
            "{\"sid\":\"s8\",\"seq\":1,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s8\",\"seq\":2,\"ts\":0,\"ev\":\"later\"}\n"
            "{\"sid\":\"s8\",\"seq\":3,\"ts\":0,\"ev\":\"delta\",\"text\":\"x\"}\n");
        log = logopen(os, a, scratch, path, "s8", 1<<20);
        expect(t, "newer event in turn", logshow(a, &log), "ok=1 seq=4 turn=1 torn=0 bad=0 synth=1");
        r = logreplay(os, a, scratch, path, "s8", 3, 1<<20);
        expect(t, "newer event in turn end", joinlines(a, r.lines),
            "{\"sid\":\"s8\",\"seq\":4,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"interrupted\"}\n");
        logclose(&log);
    }

    // A document as deep as the reader takes, logged inside a record,
    // still makes a line that reads back: only its innermost array,
    // which would nest too deeply, becomes null.
    {
        Os      *os   = newos(a, 1<<20);
        Str      path = "s9.jsonl";
        EventLog log  = logopen(os, a, scratch, path, "s9", 1<<20);
        Str      deep = nested(a, 64, "[", "]");
        JsonDoc  d    = jsonparse(a, deep);
        Buf      b(a, 512);
        EvOut    w = logbegin(&log, &b, EV_TOOLSTART);
        putkey(&w.json, "call");
        putstr(&w.json, "c1");
        putkey(&w.json, "args");
        putjson(&w.json, d.root);
        Str line = logappend(&log, &w);
        Str want = "{\"sid\":\"s9\",\"seq\":1,\"ts\":0,\"ev\":\"tool.start\","
                   "\"call\":\"c1\",\"args\":";
        want = concat(a, want, takehead(deep, 63));
        want = concat(a, want, "null");
        want = concat(a, want, cuthead(deep, 65));
        want = concat(a, want, "}\n");
        expect(t, "deep record", line, want);
        logclose(&log);
        log = logopen(os, a, scratch, path, "s9", 1<<20);
        expect(t, "deep record reopen", logshow(a, &log), "ok=1 seq=2 turn=0 torn=0 bad=0 synth=1");
        logclose(&log);
    }

    // With no log handle to be had, opening fails and leaves the file,
    // torn tail and all, for a later attempt.
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s10.jsonl";
        Str  data = "{\"sid\":\"s10\",\"seq\":1,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n{\"sid";
        os_write(os, scratch, path, data);
        os->nlogs = countof(os->logs);
        EventLog log = logopen(os, a, scratch, path, "s10", 1<<20);
        expect(t, "no handle", logshow(a, &log), "ok=0 seq=0 turn=0 torn=0 bad=0 synth=0");
        expect(t, "no handle fd", istr(a, log.fd), "-1");
        expect(t, "no handle file", vcontents(os, a, path), data);
        expect(t, "no handle append", istr(a, !addrec(&log, a, EV_NOTE, {}, {}).data), "1");
        expect(t, "no handle file after", vcontents(os, a, path), data);
    }

    // Recovery cases: a tool outside any turn, finished turns, and a
    // second turn left open
    {
        Os  *os   = newos(a, 1<<20);
        os_write(os, scratch, "s5.jsonl",
            "{\"sid\":\"s5\",\"seq\":1,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"a\"}\n"
            "{\"sid\":\"s5\",\"seq\":2,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"b\"}\n");
        EventLog log = logopen(os, a, scratch, "s5.jsonl", "s5", 1<<20);
        expect(t, "tools only", logshow(a, &log), "ok=1 seq=4 turn=0 torn=0 bad=0 synth=2");
        Replay r = logreplay(os, a, scratch, "s5.jsonl", "s5", 2, 1<<20);
        expect(t, "tools only order", joinlines(a, r.lines),
            "{\"sid\":\"s5\",\"seq\":3,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"a\","
            "\"status\":\"unknown\",\"text\":\"outcome unknown: the harness stopped while this ran\"}\n"
            "{\"sid\":\"s5\",\"seq\":4,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"b\","
            "\"status\":\"unknown\",\"text\":\"outcome unknown: the harness stopped while this ran\"}\n");
        logclose(&log);

        os_write(os, scratch, "s6.jsonl",
            "{\"sid\":\"s6\",\"seq\":1,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s6\",\"seq\":2,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n"
            "{\"sid\":\"s6\",\"seq\":3,\"ts\":0,\"ev\":\"turn\",\"n\":2}\n"
            "{\"sid\":\"s6\",\"seq\":4,\"ts\":0,\"ev\":\"ask\",\"req\":1}\n");
        log = logopen(os, a, scratch, "s6.jsonl", "s6", 1<<20);
        expect(t, "second turn", logshow(a, &log), "ok=1 seq=5 turn=2 torn=0 bad=0 synth=1");
        r = logreplay(os, a, scratch, "s6.jsonl", "s6", 4, 1<<20);
        expect(t, "second turn end", joinlines(a, r.lines),
            "{\"sid\":\"s6\",\"seq\":5,\"ts\":0,\"ev\":\"turn.end\",\"turn\":2,\"why\":\"interrupted\"}\n");
        logclose(&log);

        os_write(os, scratch, "s7.jsonl",
            "{\"sid\":\"s7\",\"seq\":1,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s7\",\"seq\":2,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"done\"}\n");
        log = logopen(os, a, scratch, "s7.jsonl", "s7", 1<<20);
        expect(t, "finished turn", logshow(a, &log), "ok=1 seq=2 turn=1 torn=0 bad=0 synth=0");
        logclose(&log);
    }

    // Faults (main_test.cpp failure hooks): truncation refused, as for
    // an append-only file, ends the torn tail to be judged as a line
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s11.jsonl";
        Str  good =
            "{\"sid\":\"s11\",\"seq\":1,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s11\",\"seq\":2,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"a\"}\n"
            "{\"sid\":\"s11\",\"seq\":3,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"b\"}\n";
        Str  torn = "{\"sid\":\"s11\",\"seq\":4,\"ts\":0,\"ev\":\"tool.e";
        Str  enda =
            "{\"sid\":\"s11\",\"seq\":4,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"a\","
            "\"status\":\"unknown\",\"text\":\"outcome unknown: the harness stopped while this ran\"}\n";
        Str  rest =
            "{\"sid\":\"s11\",\"seq\":5,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"b\","
            "\"status\":\"unknown\",\"text\":\"outcome unknown: the harness stopped while this ran\"}\n"
            "{\"sid\":\"s11\",\"seq\":6,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"interrupted\"}\n";
        Str  done = concat(a, concat(a, good, enda), rest);

        os_write(os, scratch, path, concat(a, good, torn));
        os->failtrunc = 1;
        EventLog log = logopen(os, a, scratch, path, "s11", 1<<20);
        expect(t, "no truncate", logshow(a, &log), "ok=1 seq=6 turn=1 torn=0 bad=1 synth=3");
        Str want = concat(a, concat(a, concat(a, good, torn), "\n"), cuthead(done, good.len));
        expect(t, "no truncate file", vcontents(os, a, path), want);
        logclose(&log);
        log = logopen(os, a, scratch, path, "s11", 1<<20);
        expect(t, "no truncate again", logshow(a, &log), "ok=1 seq=6 turn=1 torn=0 bad=1 synth=0");
        logclose(&log);
        os->failtrunc = 0;

        // A write fails partway through recovery: the open fails, and the
        // next finishes the job as if nothing had happened
        os_write(os, scratch, path, concat(a, good, torn));
        os->failwrites = 1;
        os->writes     = 1;
        log = logopen(os, a, scratch, path, "s11", 1<<20);
        expect(t, "write fails", istr(a, log.ok*10 + (log.fd>=0)), "0");
        expect(t, "write fails file", vcontents(os, a, path), concat(a, good, enda));
        os->failwrites = 0;
        log = logopen(os, a, scratch, path, "s11", 1<<20);
        expect(t, "write fails redo", logshow(a, &log), "ok=1 seq=6 turn=1 torn=0 bad=0 synth=2");
        expect(t, "write fails redo file", vcontents(os, a, path), done);
        logclose(&log);
    }

    // Calls that never started, one of them asking: each is closed in the
    // order a live session logs it, before the turn ends
    {
        Os  *os   = newos(a, 1<<20);
        Str  path = "s14.jsonl";
        Str  head = "{\"sid\":\"s14\",\"seq\":1,\"ts\":0,\"ev\":\"session\",\"cwd\":\"/w\"}\n"
                    "{\"sid\":\"s14\",\"seq\":2,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n";
        Str  asst = "{\"sid\":\"s14\",\"seq\":3,\"ts\":0,\"ev\":\"assistant\",\"blocks\":["
                    "{\"t\":\"text\",\"text\":\"hi\"},"
                    "{\"t\":\"call\",\"id\":\"c1\",\"name\":\"write\",\"args\":\"{\\\"path\\\":\\\"../x\\\"}\"},"
                    "{\"t\":\"call\",\"id\":\"c2\",\"name\":\"read\",\"args\":\"{}\"}]}\n";
        Str  ask  = "{\"sid\":\"s14\",\"seq\":4,\"ts\":0,\"ev\":\"ask\",\"req\":\"c1/ask\",\"call\":\"c1\"}\n";
        Str  log0 = concat(a, concat(a, head, asst), ask);
        os_write(os, scratch, path, log0);
        EventLog log = logopen(os, a, scratch, path, "s14", 1<<20);
        expect(t, "asking repaired", logshow(a, &log), "ok=1 seq=10 turn=1 torn=0 bad=0 synth=6");
        expect(t, "asking repaired file", vcontents(os, a, path), concat(a, log0,
            "{\"sid\":\"s14\",\"seq\":5,\"ts\":0,\"ev\":\"answer\",\"req\":\"c1/ask\",\"decision\":\"deny\","
            "\"by\":\"cancel\",\"note\":\"the harness stopped while asking\"}\n"
            "{\"sid\":\"s14\",\"seq\":6,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"c1\",\"name\":\"write\","
            "\"args\":\"{\\\"path\\\":\\\"../x\\\"}\",\"cwd\":\"/w\"}\n"
            "{\"sid\":\"s14\",\"seq\":7,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"c1\",\"status\":\"cancelled\","
            "\"text\":\"Not run: the harness stopped before this call started.\"}\n"
            "{\"sid\":\"s14\",\"seq\":8,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"c2\",\"name\":\"read\","
            "\"args\":\"{}\",\"cwd\":\"/w\"}\n"
            "{\"sid\":\"s14\",\"seq\":9,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"c2\",\"status\":\"cancelled\","
            "\"text\":\"Not run: the harness stopped before this call started.\"}\n"
            "{\"sid\":\"s14\",\"seq\":10,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"interrupted\"}\n"));
        logclose(&log);
        log = logopen(os, a, scratch, path, "s14", 1<<20);
        expect(t, "asking repaired once", logshow(a, &log), "ok=1 seq=10 turn=1 torn=0 bad=0 synth=0");
        logclose(&log);

        // An answered ask, a finished call, one cut off, one not begun
        Str more =
            "{\"sid\":\"s14\",\"seq\":11,\"ts\":0,\"ev\":\"turn\",\"n\":2}\n"
            "{\"sid\":\"s14\",\"seq\":12,\"ts\":0,\"ev\":\"assistant\",\"blocks\":["
            "{\"t\":\"call\",\"id\":\"d1\",\"name\":\"write\",\"args\":\"{}\"},"
            "{\"t\":\"call\",\"id\":\"d2\",\"name\":\"shell\",\"args\":\"{}\"},"
            "{\"t\":\"call\",\"id\":\"d3\",\"name\":\"read\",\"args\":\"{}\"}]}\n"
            "{\"sid\":\"s14\",\"seq\":13,\"ts\":0,\"ev\":\"ask\",\"req\":\"d1/ask\",\"call\":\"d1\"}\n"
            "{\"sid\":\"s14\",\"seq\":14,\"ts\":0,\"ev\":\"answer\",\"req\":\"d1/ask\",\"decision\":\"allow\"}\n"
            "{\"sid\":\"s14\",\"seq\":15,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"d1\"}\n"
            "{\"sid\":\"s14\",\"seq\":16,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"d1\",\"status\":\"ok\"}\n"
            "{\"sid\":\"s14\",\"seq\":17,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"d2\"}\n";
        Str log1 = concat(a, vcontents(os, a, path), more);
        os_write(os, scratch, path, log1);
        log = logopen(os, a, scratch, path, "s14", 1<<20);
        expect(t, "cut off repaired", logshow(a, &log), "ok=1 seq=21 turn=2 torn=0 bad=0 synth=4");
        expect(t, "cut off repaired file", vcontents(os, a, path), concat(a, log1,
            "{\"sid\":\"s14\",\"seq\":18,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"d2\",\"status\":\"unknown\","
            "\"text\":\"outcome unknown: the harness stopped while this ran\"}\n"
            "{\"sid\":\"s14\",\"seq\":19,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"d3\",\"name\":\"read\","
            "\"args\":\"{}\",\"cwd\":\"/w\"}\n"
            "{\"sid\":\"s14\",\"seq\":20,\"ts\":0,\"ev\":\"tool.end\",\"call\":\"d3\",\"status\":\"cancelled\","
            "\"text\":\"Not run: the harness stopped before this call started.\"}\n"
            "{\"sid\":\"s14\",\"seq\":21,\"ts\":0,\"ev\":\"turn.end\",\"turn\":2,\"why\":\"interrupted\"}\n"));
        logclose(&log);
    }

    // Syncs follow the table, recovery syncs once at the end, and a
    // failed sync fails the append or the open (the record stays written)
    {
        Os      *os  = newos(a, 1<<20);
        EventLog log = logopen(os, a, scratch, "s12.jsonl", "s12", 1<<20);
        Buf      b(a, 256);
        for (i32 ev = 1; ev < EV_COUNT; ev++) {
            if (evdurable(ev)) {
                i32 before = os->syncs;
                addrec(&log, a, ev, {}, {});
                print(&b, evname(ev));
                putbyte(&b, '=');
                print(&b, (i64)(os->syncs - before));
                putbyte(&b, ' ');
            }
        }
        expect(t, "syncs", finish(&b),
               "session=0 title=0 user=0 turn=0 attempt=0 assistant=0 "
               "tool.start=1 tool.end=0 ask=1 answer=1 retry=0 nudge=0 "
               "turn.end=1 note=0 compact=0 job.start=0 job.end=0 todo=0 snap=0 "
               "rewind=1 instructions=0 notice=0 prefix=0 model=0 grant=0 mode=0 ");
        logclose(&log);

        Str start = "{\"sid\":\"s13\",\"seq\":1,\"ts\":0,\"ev\":\"tool.start\",\"call\":\"a\"}\n";
        os_write(os, scratch, "s13.jsonl", start);
        os->syncs = 0;
        log = logopen(os, a, scratch, "s13.jsonl", "s13", 1<<20);
        expect(t, "recovery syncs", istr(a, os->syncs), "1");
        os->failsync = 1;
        expect(t, "sync fails", istr(a, !addrec(&log, a, EV_ASK, {}, {}).data), "1");
        expect(t, "sync fails seq", istr(a, log.seq), "3");
        logclose(&log);

        os_write(os, scratch, "s13.jsonl", start);
        log = logopen(os, a, scratch, "s13.jsonl", "s13", 1<<20);
        expect(t, "recovery sync fails", istr(a, log.ok*10 + (log.fd>=0)), "0");
        os->failsync = 0;
        log = logopen(os, a, scratch, "s13.jsonl", "s13", 1<<20);
        expect(t, "recovery sync fails redo", logshow(a, &log), "ok=1 seq=2 turn=0 torn=0 bad=0 synth=0");
        logclose(&log);
    }

    // Background jobs open at a crash end as unknown, before the turn
    {
        Os *os = newos(a, 1<<20);
        Str before =
            "{\"sid\":\"s15\",\"seq\":1,\"ts\":0,\"ev\":\"session\",\"cwd\":\"/w\"}\n"
            "{\"sid\":\"s15\",\"seq\":2,\"ts\":0,\"ev\":\"job.start\",\"job\":1,\"call\":\"a\"}\n"
            "{\"sid\":\"s15\",\"seq\":3,\"ts\":0,\"ev\":\"job.start\",\"job\":2,\"call\":\"b\"}\n"
            "{\"sid\":\"s15\",\"seq\":4,\"ts\":0,\"ev\":\"turn\",\"n\":1}\n"
            "{\"sid\":\"s15\",\"seq\":5,\"ts\":0,\"ev\":\"job.end\",\"job\":1,\"call\":\"a\","
            "\"status\":\"exited\"}\n";
        os_write(os, scratch, "s15.jsonl", before);
        EventLog log = logopen(os, a, scratch, "s15.jsonl", "s15", 1<<20);
        expect(t, "jobs recovered", logshow(a, &log), "ok=1 seq=7 turn=1 torn=0 bad=0 synth=2");
        expect(t, "jobs recovered log", vcontents(os, a, "s15.jsonl"), concat(a, before,
            "{\"sid\":\"s15\",\"seq\":6,\"ts\":0,\"ev\":\"job.end\",\"job\":2,\"call\":\"b\","
            "\"status\":\"unknown\",\"text\":\"the harness stopped while this job ran, "
            "which stopped the job too\"}\n"
            "{\"sid\":\"s15\",\"seq\":7,\"ts\":0,\"ev\":\"turn.end\",\"turn\":1,\"why\":\"interrupted\"}\n"));
        logclose(&log);
    }

    testlogcopy(t);
    testlogimport(t);
}
