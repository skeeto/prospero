// View model tests: events to messages, virtualization, selection, copy.

static View *testview(Arena *a)
{
    Os        *os = newos(a, 1<<12);
    iz         mb = (iz)1<<20;
    ViewCache *c  = newviewcache(a, os_reserve(os, 64*mb), 32*mb);
    return newview(a, c, os_reserve(os, 16*mb), os_reserve(os, 16*mb),
                   os_reserve(os, 16*mb));
}

static Str msgsshow(Arena *a, View *v)
{
    Buf b(a, 256);
    static char const roles[] = "UATXNK";
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        putbyte(&b, (u8)roles[m->role]);
        if (!m->seq) putbyte(&b, '~');
        if (m->role == VIEW_TOOL) {
            putbyte(&b, '[');
            print(&b, m->tool->name);
            putbyte(&b, ' ');
            print(&b, m->tool->summary);
            putbyte(&b, ' ');
            print(&b, vtoolnames[m->tool->status]);
            putbyte(&b, ']');
        }
        putbyte(&b, ':');
        print(&b, m->text);
        putbyte(&b, ';');
    }
    return finish(&b);
}

static b32 apply(View *v, Arena scratch, Str line)
{
    return viewapply(v, scratch, line);
}

// Select [s, e) of one message, or across messages.
static void selset(View *v, i32 m0, i32 s, i32 m1, i32 e)
{
    v->sel = {};
    v->sel.active = 1;
    v->sel.anchor = {m0, s};
    v->sel.focus  = {m1, e};
}

static void test_view(Test *t)
{
    Arena perm = t->perm;

    // A turn: deltas grow provisional messages, which the committed
    // record replaces, reclaiming their text
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"hi","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})");
        expect(t, "view.busy", istr(&a, v->busy), "1");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"think","text":"hm"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"text","text":"Hel"})");
        byte *grow = v->text.beg;
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"text","text":"lo"})");
        expect(t, "view.inplace", istr(&a, v->text.beg - grow), "2");
        expect(t, "view.streaming", msgsshow(&a, v), "U:hi;T~:hm;A~:Hello;");
        byte *prov = v->provtext;
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"think","text":"hm"},{"t":"text","text":"Hello"},)"
                    R"({"t":"call","id":"c1","name":"read","args":"{\"path\":\"x\"}"}]})");
        expect(t, "view.committed", msgsshow(&a, v),
               "U:hi;T:hm;A:Hello;X[read x called]:;");
        // The committed texts take the provisional ones' place
        expect(t, "view.reclaimed", istr(&a, v->text.beg - prov), "7");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"turn.end","turn":1,"why":"done"})");
        expect(t, "view.idle", istr(&a, v->busy), "0");
        b32 changed = apply(v, a, R"({"ts":0,"ev":"hello","proto":0})");
        expect(t, "view.unchanged", istr(&a, changed), "0");
        changed = apply(v, a, "not json");
        expect(t, "view.garbage", istr(&a, changed), "0");
    }

    // A note arriving mid-stream survives the commit that replaces the
    // provisional messages around it
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"text","text":"ab"})");
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"note","level":"info","text":"retrying"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"text","text":"cd"})");
        expect(t, "view.midnote", msgsshow(&a, v), "A~:ab;N:retrying;A~:cd;");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"assistant","turn":1,)"
                    R"("blocks":[{"t":"text","text":"abcd"}]})");
        expect(t, "view.midnotekept", msgsshow(&a, v), "N:retrying;A:abcd;");
    }

    // An interrupted turn drops its provisional text, and a selection
    // in it is clamped to what remains
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"q"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"kind":"text","text":"partial"})");
        selset(v, 0, 0, 1, 7);
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn.end","turn":1,"why":"cancel"})");
        expect(t, "view.dropped", msgsshow(&a, v), "U:q;N:Cancelled.;");
        // The cancel's note took the dropped message's place
        Range r = selrange(&v->sel);
        expect(t, "view.clamped", istr(&a, r.end.msg*100 + r.end.off), "107");
    }

    // Tops: prefix sums of heights (estimated until measured) plus the
    // per-message extra and gap, searched by binary search
    {
        Arena a = perm;
        View *v = testview(&a);
        v->extra = 10;
        v->gap   = 5;
        addmsg(v, VIEW_USER, "short");                 // 1 line
        addmsg(v, VIEW_ASSISTANT, "x\ny\nz");          // 3 lines
        addmsg(v, VIEW_ASSISTANT, "0123456789abcde");  // 2 lines at 10/line
        f64 total = viewtops(v, 100, 10, 20);
        Buf b(&a, 64);
        for (iz i = 0; i < v->msgs.len; i++) {
            print(&b, (i64)v->msgs[i].top);
            putbyte(&b, ' ');
        }
        print(&b, (i64)total);
        expect(t, "view.tops", finish(&b), "0 35 110 160");
        expect(t, "view.find0", istr(&a, viewfind(v, 0)), "0");
        expect(t, "view.find34", istr(&a, viewfind(v, 34)), "0");
        expect(t, "view.find35", istr(&a, viewfind(v, 35)), "1");
        expect(t, "view.findend", istr(&a, viewfind(v, 1e9)), "2");
        expect(t, "view.findneg", istr(&a, viewfind(v, -5)), "0");
    }

    // Measured heights replace estimates once laid out
    {
        Arena   a = perm;
        View   *v = testview(&a);
        Measure m = fixedmeasure();
        addmsg(v, VIEW_ASSISTANT, "# Big\n\ntext");
        viewtops(v, 200, 10, 20);
        f32 est = v->msgs[0].h;
        msglayout(v, 0, &m, 200, 1);
        f64 total = viewtops(v, 200, 10, 20);
        expect(t, "view.estimate", istr(&a, (i64)est), "60");
        expect(t, "view.measured", istr(&a, (i64)total), "72");
    }

    // Words, blocks, and extending by units
    {
        Arena a = perm;
        View *v = testview(&a);
        addmsg(v, VIEW_ASSISTANT, "alpha beta_2 γδ.\n\nsecond para");
        Range w = wordat(v, {0, 8});
        expect(t, "view.word", istr(&a, w.beg.off*100 + w.end.off), "612");
        w = wordat(v, {0, 13});  // inside "γδ"
        expect(t, "view.wordutf8", istr(&a, w.beg.off*100 + w.end.off), "1317");
        w = wordat(v, {0, 5});   // the space
        expect(t, "view.wordspace", istr(&a, w.beg.off*100 + w.end.off), "506");
        selbegin(v, {0, 8}, SEL_WORD);
        selextend(v, {0, 1});
        Range r = selrange(&v->sel);
        expect(t, "view.wordback", istr(&a, r.beg.off*100 + r.end.off), "12");
        selextend(v, {0, 20});
        r = selrange(&v->sel);
        expect(t, "view.wordfwd", istr(&a, r.beg.off*100 + r.end.off), "626");
        selbegin(v, {0, 22}, SEL_BLOCK);
        r = selrange(&v->sel);
        expect(t, "view.block", istr(&a, r.beg.off*100 + r.end.off), "2031");
        selall(v);
        r = selrange(&v->sel);
        expect(t, "view.all", istr(&a, r.end.msg*100 + r.end.off), "31");
    }

    // Copy as text: piece texts, code byte-exact, list markers, table
    // cells by tabs, and messages and blocks by blank lines
    {
        Arena a = perm;
        View *v = testview(&a);
        Str   md =
            "Some **bold** &amp; text\nwrapped.\n\n"
            "- one\n- two\n\n"
            "```c\nint\tx;  \n```\n\n"
            "| a | b |\n|---|---|\n| 1 | 2 |\n";
        addmsg(v, VIEW_ASSISTANT, md);
        addmsg(v, VIEW_USER, "user line 1\nline 2");
        selset(v, 0, 0, 1, 6);
        expect(t, "view.copytext", copysel(v, &a, COPY_TEXT),
               "Some bold & text\nwrapped.\n\n- one\n- two\n\nint\tx;  \n\n"
               "a\tb\n1\t2\n\nuser l");

        // Partial: from inside "bold" to inside "wrapped"
        selset(v, 0, 8, 0, 29);
        expect(t, "view.copypart", copysel(v, &a, COPY_TEXT), "old & text\nwrap");

        // Only the code: byte-exact, tab and trailing spaces
        i32 c0 = (i32)find(md, "int");
        selset(v, 0, c0, 0, c0+10);
        expect(t, "view.copycode", copysel(v, &a, COPY_TEXT), "int\tx;  \n");

        // Reversed selections copy the same
        selset(v, 0, c0+10, 0, c0);
        expect(t, "view.copyrev", copysel(v, &a, COPY_TEXT), "int\tx;  \n");

        // Empty
        selset(v, 0, 3, 0, 3);
        expect(t, "view.copyempty", copysel(v, &a, COPY_TEXT), "");
    }

    // Copy as markdown: the source within a block, whole blocks' lines
    // (fences included) across blocks
    {
        Arena a  = perm;
        View *v  = testview(&a);
        Str   md = "# Head\n\nSome **bold** text.\n\n```sh\necho hi\n```\n\n> quoted\n";
        addmsg(v, VIEW_ASSISTANT, md);
        i32 b0 = (i32)find(md, "**bold");
        selset(v, 0, b0, 0, b0+8);
        expect(t, "view.mdinner", copysel(v, &a, COPY_MARKDOWN), "**bold**");
        i32 s = (i32)find(md, "bold");
        i32 e = (i32)find(md, "hi");
        selset(v, 0, s, 0, e);
        expect(t, "view.mdspan", copysel(v, &a, COPY_MARKDOWN),
               "Some **bold** text.\n\n```sh\necho hi\n```");
        selset(v, 0, 3, 0, (i32)find(md, "quoted")+3);
        expect(t, "view.mdall", copysel(v, &a, COPY_MARKDOWN),
               "# Head\n\nSome **bold** text.\n\n```sh\necho hi\n```\n\n> quoted");
    }
    // Tool rows: arguments streaming, the committed calls, a running
    // command's output, its end with the output file, and an edit's diff
    {
        Arena a = perm;
        View *v = testview(&a);
        v->dir  = "/st/sessions/s";
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m","endpoint":"e"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"go","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":1000,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":1000,"ev":"attempt","turn":1,"n":1})");
        apply(v, a, R"({"sid":"s","ts":1100,"ev":"delta","turn":1,"attempt":1,"block":0,"kind":"args","id":"c1","name":"shell","text":"{\"com"})");
        expect(t, "view.argsprov", msgsshow(&a, v), "U:go;X~[shell  streaming]:;");
        expect(t, "view.lastdata", istr(&a, v->lastdata), "1100");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c1","name":"shell","args":"{\"command\":\"make test\\nmore\"}"},)"
                    R"({"t":"call","id":"c2","name":"edit","args":"{\"path\":\"a.c\",\"old_string\":\"x\\ny\",\"new_string\":\"z\"}"}]})");
        expect(t, "view.calls", msgsshow(&a, v),
               "U:go;X[shell make test\xe2\x80\xa6 called]:;X[edit a.c called]:;");
        b32 stale = apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":0,"kind":"text","text":"late"})");
        expect(t, "view.staledelta", istr(&a, stale), "0");
        apply(v, a, R"({"sid":"s","seq":6,"ts":2000,"ev":"tool.start","call":"c1","name":"shell","args":"{}","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","ts":2500,"ev":"tool.out","call":"c1","tail":"line 1\n","bytes":7})");
        expect(t, "view.running", msgsshow(&a, v),
               "U:go;X[shell make test\xe2\x80\xa6 running]:line 1\n;X[edit a.c called]:;");
        apply(v, a, R"({"sid":"s","seq":7,"ts":3200,"ev":"tool.end","call":"c1","status":"ok","exit":0,"text":"ok\n","spill":"out/c1.log","bytes":3,"ms":1200})");
        ViewTool *sh = v->msgs[1].tool;
        expect(t, "view.shellend", v->msgs[1].text,
               "ok\n\nFull output: /st/sessions/s/out/c1.log (3 bytes)");
        expect(t, "view.shellexit", istr(&a, sh->hasexit*10000 + sh->exit*1000 + sh->ms), "11200");
        b32 late = apply(v, a, R"({"sid":"s","ts":0,"ev":"tool.out","call":"c1","tail":"late","bytes":9})");
        expect(t, "view.lateout", istr(&a, late), "0");
        apply(v, a, R"({"sid":"s","seq":8,"ts":0,"ev":"tool.start","call":"c2","name":"edit","args":"{}","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":9,"ts":0,"ev":"tool.end","call":"c2","status":"ok","text":"Edited a.c: replaced 1 match at line 12.","ms":5})");
        expect(t, "view.diff", v->msgs[2].text,
               "```diff\n--- a.c\n+++ a.c\n@@ -12,2 +12,1 @@\n-x\n-y\n+z\n```\n");
        expect(t, "view.diffmd", istr(&a, v->msgs[2].md*10 + v->msgs[2].collapsed), "10");
        apply(v, a, R"({"sid":"s","seq":10,"ts":0,"ev":"turn.end","turn":1,"why":"done"})");
        expect(t, "view.done", istr(&a, v->busy*10 + v->state), "0");
    }

    // Asks: a card while pending, with room for its buttons, resolved
    // by the answer; the call's row waits meanwhile
    {
        Arena a = perm;
        View *v = testview(&a);
        v->foot = 30;
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c3","name":"write","args":"{\"path\":\"../y\",\"content\":\"a\\nb\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"ask","req":"c3/ask","call":"c3","kind":"path",)"
                    R"("tool":"write","path":"/x/y","detail":"outside","hint":"/w/y","deadline":30000,"dflt":"deny"})");
        expect(t, "view.ask", msgsshow(&a, v),
               "X[write ../y asking]:;K:outside\nNearest path inside the workspace: /w/y;");
        expect(t, "view.asks", istr(&a, v->asks), "1");
        expect(t, "view.askfoot", istr(&a, (i64)msgfoot(v, &v->msgs[1])), "30");
        v->readonly = 1;
        expect(t, "view.askro", istr(&a, (i64)msgfoot(v, &v->msgs[1])), "0");
        v->readonly = 0;
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"answer","req":"c3/ask","decision":"deny","by":"timeout","note":"n"})");
        ViewTool *k = v->msgs[1].tool;
        expect(t, "view.answer", istr(&a, v->asks*100 + k->answer*10 + (k->by=="timeout")), "21");
        expect(t, "view.answered", istr(&a, (i64)msgfoot(v, &v->msgs[1])), "0");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.start","call":"c3","name":"write","args":"{}","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"tool.end","call":"c3","status":"denied","text":"Denied.","ms":30001})");
        expect(t, "view.denied", msgsshow(&a, v),
               "X[write ../y denied]:Denied.;K:outside\nNearest path inside the workspace: /w/y;");
        expect(t, "view.deniedopen", istr(&a, v->msgs[0].collapsed), "0");
    }

    // A crash while asking leaves no answer, and recovery ends the call:
    // its end closes the card too
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c4","name":"write","args":"{\"path\":\"../z\",\"content\":\"z\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"ask","req":"c4/ask","call":"c4","kind":"path",)"
                    R"("tool":"write","path":"/x/z","detail":"outside","deadline":30000,"dflt":"deny"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"tool.end","call":"c4","status":"cancelled",)"
                    R"("text":"Not run: the harness stopped before this call started."})");
        ViewTool *k = v->msgs[1].tool;
        expect(t, "view.askcrash", istr(&a, v->asks*100 + k->answer*10 + (k->by=="interrupted")), "21");
    }

    // A successful write shows its content as added lines
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"w","name":"write","args":"{\"path\":\"n.txt\",\"content\":\"a ```` b\\nc\\n\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"tool.end","call":"w","status":"ok","text":"Created n.txt.","ms":1})");
        expect(t, "view.writediff", v->msgs[0].text,
               "`````diff\n--- /dev/null\n+++ n.txt\n@@ -0,0 +1,2 @@\n+a ```` b\n+c\n`````\n");
    }

    // Order: a record applied once, a gap marks the view lost and is not
    // applied, and a replay may skip numbers
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"note","text":"two"})");
        expect(t, "view.gap", istr(&a, v->lost*100 + v->lastseq*10 + v->msgs.len), "100");
        v->lost = 0;
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"note","text":"one"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"note","text":"two"})");
        b32 dup = apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"note","text":"two"})");
        expect(t, "view.dup", istr(&a, dup*100 + v->lastseq*10 + v->msgs.len), "22");
        v->loose = 1;
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"note","text":"five"})");
        expect(t, "view.loose", istr(&a, v->lost*100 + v->lastseq*10 + v->msgs.len), "53");
    }

    // A failed attempt's text fades, then the next attempt drops it;
    // liveness comes from live and progress records
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":5000,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"attempt","turn":1,"n":1})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":0,"kind":"think","text":"hm"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"retry","turn":1,"attempt":1,"class":"transient","status":503,"delay_ms":1500,"msg":"busy"})");
        expect(t, "view.faded", istr(&a, v->msgs[0].faded*10 + v->msgs[0].prov), "11");
        expect(t, "view.retrynote", v->msgs[1].text,
               "Attempt 1 failed (transient, HTTP 503): busy. Retrying in 1.5 s.");
        apply(v, a, R"({"sid":"s","ts":9000,"ev":"live","state":"backoff","retry_at":10500})");
        expect(t, "view.backoff", istr(&a, v->state*100000 + v->retryat), "510500");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"attempt","turn":1,"n":2})");
        expect(t, "view.dropfaded", msgsshow(&a, v),
               "N:Attempt 1 failed (transient, HTTP 503): busy. Retrying in 1.5 s.;");
        apply(v, a, R"({"sid":"s","ts":12000,"ev":"live","state":"waiting","idle_ms":700,"tps":0,"watchdog_ms":29300,"watchdog_of":30000})");
        expect(t, "view.live", istr(&a, v->lastdata*1000000 + v->watchat), "11300041300");
        apply(v, a, R"({"sid":"s","ts":12500,"ev":"progress","turn":1,"done":40,"total":100,"cached":0})");
        expect(t, "view.progress", istr(&a, v->pdone*1000 + v->ptotal), "40100");
        expect(t, "view.turnts", istr(&a, v->turnts), "5000");
    }

    // A selection survives the next attempt dropping a failed one's text:
    // positions in the messages after it move with them, and one in the
    // dropped text goes, so copying never reaches past the messages
    for (i32 where = 0; where < 2; where++) {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"hi"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"attempt","turn":1,"n":1})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":0,"kind":"text","text":"partial answer"})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"retry","turn":1,"attempt":1,"class":"transient","status":503,"delay_ms":1500,"msg":"busy"})");
        expect(t, "view.retrysel.before", msgsshow(&a, v),
               "U:hi;A~:partial answer;N:Attempt 1 failed (transient, HTTP 503): busy. Retrying in 1.5 s.;");
        if (where) {
            selset(v, 1, 0, 1, 7);  // in the failed text
        } else {
            selset(v, 2, 0, 2, 9);  // in the retry note
        }
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"attempt","turn":1,"n":2})");
        expect(t, "view.retrysel.after", istr(&a, v->msgs.len), "2");
        Str got = copysel(v, &a, COPY_TEXT);
        expect(t, where ? Str("view.retrysel.dropped") : Str("view.retrysel.moved"),
               got.data ? got : Str("(none)"), where ? Str("(none)") : Str("Attempt 1"));
    }

    // Opening mid-stream: a snapshot replaces what was provisional, the
    // deltas after it extend it, and the commit replaces it all
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"hi"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"attempt","turn":1,"n":1})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":1,"kind":"text","text":"Hel"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"snapshot","turn":1,"attempt":1,"blocks":[)"
                    R"({"block":0,"kind":"think","text":"hmm"},{"block":1,"kind":"text","text":"Hello"},)"
                    R"({"block":2,"kind":"args","id":"c1","name":"read","text":"{\"pa"}]})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":2,"kind":"args","text":"th\":\"x\"}"})");
        expect(t, "view.snapshot", msgsshow(&a, v), "U:hi;T~:hmm;A~:Hello;X~[read  streaming]:;");
        i32 last = (i32)v->msgs.len - 1;
        expect(t, "view.snapshot.args", v->msgs[last].tool->args, "{\"path\":\"x\"}");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"snapshot","turn":1,"attempt":1,"blocks":[)"
                    R"({"block":1,"kind":"text","text":"llo, world","cut":3}]})");
        expect(t, "view.snapshot.cut", msgsshow(&a, v), "U:hi;A~:\xe2\x80\xa6llo, world;");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"text","text":"Hello, world"}]})");
        b32 stale = apply(v, a, R"({"sid":"s","ts":0,"ev":"snapshot","turn":1,"attempt":1,"blocks":[]})");
        expect(t, "view.snapshot.stale", istr(&a, stale), "0");
        expect(t, "view.snapshot.done", msgsshow(&a, v), "U:hi;A:Hello, world;");
    }

    // Records lost to a full mailbox come back from the log: a view fed
    // through a tiny mailbox, resynced from the durable records written
    // so far whenever it overflowed, ends as one fed everything.
    {
        Arena a = perm;
        Buf   lb(&a, 1<<14);
        print(&lb, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m","endpoint":"e"})" "\n");
        print(&lb, R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"go","via":"prompt"})" "\n");
        print(&lb, R"({"sid":"s","seq":3,"ts":0,"ev":"title","text":"go"})" "\n");
        print(&lb, R"({"sid":"s","seq":4,"ts":0,"ev":"turn","n":1})" "\n");
        print(&lb, R"({"sid":"s","seq":5,"ts":0,"ev":"attempt","turn":1,"n":1})" "\n");
        for (i32 i = 0; i < 12; i++) {
            print(&lb, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":1,"block":0,"kind":"text","text":"word "})" "\n");
        }
        print(&lb, R"({"sid":"s","seq":6,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                   R"({"t":"text","text":"word word"},{"t":"call","id":"c","name":"shell","args":"{\"command\":\"ls\"}"}]})" "\n");
        print(&lb, R"({"sid":"s","seq":7,"ts":0,"ev":"tool.start","call":"c","name":"shell","args":"{}","cwd":"/w"})" "\n");
        for (i32 i = 0; i < 6; i++) {
            print(&lb, R"({"sid":"s","ts":0,"ev":"tool.out","call":"c","tail":"a\nb\n","bytes":4})" "\n");
        }
        print(&lb, R"({"sid":"s","seq":8,"ts":0,"ev":"tool.end","call":"c","status":"ok","exit":0,"text":"a\nb\n","spill":"out/c.log","bytes":4,"ms":3})" "\n");
        print(&lb, R"({"sid":"s","seq":9,"ts":0,"ev":"attempt","turn":1,"n":2})" "\n");
        print(&lb, R"({"sid":"s","seq":10,"ts":0,"ev":"assistant","turn":1,"attempt":2,"blocks":[{"t":"text","text":"done"}]})" "\n");
        print(&lb, R"({"sid":"s","seq":11,"ts":0,"ev":"turn.end","turn":1,"why":"done"})" "\n");
        Slice<Str> lines = {};
        for (Str rest = finish(&lb); rest.len;) {
            Cut c = cut(rest, '\n');
            lines = push(&a, lines, c.head);
            rest  = c.tail;
        }

        View *want = testview(&a);
        for (iz i = 0; i < lines.len; i++) {
            apply(want, a, lines[i]);
        }

        View    *got  = testview(&a);
        Mailbox *mb   = newmailbox(&a, 400, 0, 0);
        i32      lost = 0;
        for (iz i = 0; i < lines.len; i++) {
            mailpost(mb, MAIL_OUT, 0, lines[i], 0);
            if (i%5!=4 && i!=lines.len-1) {
                continue;
            }
            Mail m = mailtake(mb);
            for (Record r = mailnext(&m); r.ok; r = mailnext(&m)) {
                apply(got, a, r.data);
            }
            if (m.overflow || got->lost) {
                // The log: the durable records written by now
                lost++;
                got->loose = 1;
                for (iz k = 0; k <= i; k++) {
                    Arena tmp = a;
                    EvRec rec = evparse(&tmp, lines[k]);
                    if (rec.seq > got->lastseq) {
                        viewrecord(got, tmp, &rec);
                    }
                }
                got->loose = 0;
                got->lost  = 0;
            }
        }
        Buf x(&a, 1<<12);
        Buf y(&a, 1<<12);
        viewdump(&x, got);
        viewdump(&y, want);
        expect(t, "view.resync", finish(&x), finish(&y));
        expect(t, "view.overflowed", istr(&a, lost>0), "1");
    }
}
